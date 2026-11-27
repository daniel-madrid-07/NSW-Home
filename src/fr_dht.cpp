// See fr_dht.h.
#include "fr_dht.h"
#include "fr_bencode.h"
#include "fr_crypto.h"
#include <algorithm>
#include <numeric>
#include <set>

namespace dht {
namespace {

// BEP 5's bucket size: a value is stored on the eight nodes closest to it.
constexpr int kK = 8;
// Queries in flight at once during a lookup (the usual choice is 3 or 4).
constexpr int kAlpha = 4;
// Most nodes answer within a few hundred milliseconds. One that has not
// after kSlowAfter no longer holds up the next query (a third of the nodes
// the network hands out are gone, and waiting for each would make a lookup
// take a quarter of a minute); its answer is still taken until kQueryTimeout.
constexpr double kQueryTimeout = 1.6, kSlowAfter = 0.5, kRepeatAfter = 0.7;
// A write token is good for at least five minutes on the common
// implementations (they change their secret every five and accept the one
// before); the nodes of a lookup are reused for a write only while younger.
constexpr double kNearFresh = 270;
constexpr size_t kMaxCandidates = 600, kMaxTable = 400, kMaxNearSets = 64, kMaxDead = 4000;
// Most of the addresses the network hands out near a given place are the
// same every time, and most of those that do not answer never will (they sit
// behind routers that only let in whom they have talked to). One that timed
// out is not asked again for this long, which is what makes reading the same
// item again a matter of a dozen queries instead of thirty.
constexpr double kDeadFor = 900;
// Queries that may go out at once after a quiet spell (then `rate_` a second).
constexpr double kBurst = 12;

// The long-standing public entry points of the network.
const struct {
    const char* host;
    uint16_t port;
} kRouters[] = {
    {"router.bittorrent.com", 6881},
    {"dht.transmissionbt.com", 6881},
    {"router.utorrent.com", 6881},
    {"dht.libtorrent.org", 25401},
};

std::string Xor(const std::string& a, const std::string& b) {
    std::string out(20, 0);
    for (size_t i = 0; i < 20; i++) out[i] = static_cast<char>(a[i] ^ b[i]);
    return out;
}

std::string TidText(uint16_t n) {
    std::string t(2, 0);
    t[0] = static_cast<char>(n >> 8);
    t[1] = static_cast<char>(n & 255);
    return t;
}

// Addresses no node of the public network can have. An answer may list any
// address it likes as a "node"; nothing is ever sent to the local network,
// to this PC or to a multicast group on a stranger's word.
bool Routable(const frn::Addr& a) {
    const uint32_t ip = a.ip, top = ip >> 24;
    if (a.port == 0 || top == 0 || top == 10 || top == 127 || top >= 224) return false;
    if ((ip >> 20) == ((172u << 4) | 1)) return false;       // 172.16.0.0/12
    if ((ip >> 16) == ((192u << 8) | 168)) return false;     // 192.168.0.0/16
    if ((ip >> 16) == ((169u << 8) | 254)) return false;     // 169.254.0.0/16
    if ((ip >> 22) == ((100u << 2) | 1)) return false;       // 100.64.0.0/10
    return true;
}

// Every query carries "ro": 1 (BEP 43): do not add me to your routing table.
std::string Query(uint16_t tid, const char* name, ben::Val args) {
    ben::Val d = ben::Val::MakeDict();
    d.Set("t", TidText(tid)).Set("y", "q").Set("q", name).Set("a", std::move(args)).Set("ro", 1);
    return ben::Encode(d);
}

} // namespace

std::string Target(const std::string& pub, const std::string& salt) {
    return frc::Sha1(pub + salt);
}

std::string SignBuffer(const std::string& salt, int64_t seq, const std::string& value) {
    std::string out;
    if (!salt.empty()) out += "4:salt" + std::to_string(salt.size()) + ":" + salt;
    out += "3:seqi" + std::to_string(seq) + "e1:v" + std::to_string(value.size()) + ":" + value;
    return out;
}

Client::Client(std::atomic<bool>* stop) : stop_(stop) {}

void Client::Close() {
    udp_.Close();
}

bool Client::Ready() {
    if (id_.empty()) id_ = frc::RandomBytes(20);
    if (!udp_.IsOpen()) {
        if (!udp_.Open()) return false;
        // A new socket is a new port: tokens were issued to the old one.
        near_.clear();
    }
    return true;
}

bool Client::Bootstrap() {
    double now = frn::Now();
    // Addresses are kept for an hour; a failed lookup of the names is not
    // repeated for half a minute (it can take seconds without a network).
    if (now - routersAt_ < (routers_.empty() ? 30 : 3600)) return !routers_.empty();
    routersAt_ = now;
    routers_.clear();
    for (const auto& r : kRouters) {
        if (stop_->load()) break;
        for (const auto& a : frn::Resolve(r.host, r.port)) routers_.push_back(a);
    }
    return !routers_.empty();
}

bool Client::TakeSlot() {
    double now = frn::Now();
    bucket_ = (std::min)(kBurst, bucket_ + (now - bucketAt_) * rate_);
    bucketAt_ = now;
    if (bucket_ < 1) return false;
    bucket_ -= 1;
    return true;
}

bool Client::SendQuery(const frn::Addr& to, const std::string& packet) {
    packetsOut++;
    bytesOut += packet.size();
    return udp_.Send(to, packet);
}

void Client::Remember(const frn::Addr& a, const std::string& id) {
    table_[a] = {id, frn::Now()};
    if (table_.size() <= kMaxTable) return;
    // Full: the quarter that answered longest ago goes.
    std::vector<std::pair<double, frn::Addr>> byAge;
    for (const auto& kv : table_) byAge.emplace_back(kv.second.at, kv.first);
    std::sort(byAge.begin(), byAge.end());
    for (size_t i = 0; i < kMaxTable / 4; i++) table_.erase(byAge[i].second);
}

void Client::Lookup(const std::string& target, const std::string& pub, const std::string& salt, int64_t knownSeq,
                    double maxSeconds, GetResult& res) {
    const double start = frn::Now();
    if (!Ready()) return;
    std::vector<Cand> c;
    auto add = [&](const std::string& id, const frn::Addr& a, bool router, bool trusted = false) {
        if (!Routable(a) || id == id_ || c.size() >= kMaxCandidates) return;
        for (const auto& e : c)
            if (e.addr == a) return;
        if (!router && !trusted) {
            auto d = dead_.find(a);
            if (d != dead_.end()) {
                if (start - d->second < kDeadFor) return;
                dead_.erase(d);
            }
        }
        Cand n;
        n.id = router ? std::string() : id;
        n.addr = a;
        n.findOnly = router;
        n.trusted = trusted || table_.count(a) > 0;
        // A router sorts after every real node: it is asked only for want of others.
        n.dist = router ? std::string(20, '\xff') : Xor(id, target);
        c.push_back(std::move(n));
    };
    // Start from the nodes that held this item last time, then from the
    // closest ones that answered anything lately, and from the routers only
    // when those are too few.
    auto nit = near_.find(target);
    if (nit != near_.end())
        for (const auto& n : nit->second.nodes) add(n.id, n.addr, false, true);
    {
        std::vector<std::pair<std::string, frn::Addr>> known;
        for (const auto& kv : table_) known.emplace_back(Xor(kv.second.id, target), kv.first);
        std::sort(known.begin(), known.end());
        for (size_t i = 0; i < known.size() && i < 16; i++) add(table_[known[i].second].id, known[i].second, false, true);
    }
    bool routersAdded = false;
    auto addRouters = [&] {
        if (routersAdded) return;
        routersAdded = true;
        if (Bootstrap())
            for (const auto& r : routers_) add({}, r, true);
    };
    if (c.size() < static_cast<size_t>(kK)) addRouters();

    std::map<uint16_t, size_t> pending; // transaction id -> candidate
    std::set<std::string> seenValues;
    std::vector<size_t> order;
    for (;;) {
        double now = frn::Now();
        if (stop_->load() || now - start > maxSeconds) break;
        for (auto& e : c)
            if (e.st == Cand::Sent && now - e.sentAt > kQueryTimeout) {
                // Datagrams get lost and busy nodes skip a query now and then.
                // A router, or a node that has answered before, is asked a
                // second time; a known node that stays silent through one
                // whole lookup is still asked in the next, and only after
                // two is it taken for gone. (Giving up on the first silence
                // lost the nodes that hold a friend's status one by one,
                // until the status could not be found any more.) A node
                // never heard from is not asked twice: most of those are
                // behind a router and will never answer.
                auto known = table_.find(e.addr);
                if ((e.id.empty() || e.trusted) && !e.retried) {
                    e.retried = true;
                    e.st = Cand::Fresh;
                } else if (known != table_.end() && known->second.strikes < 1) {
                    e.st = Cand::Failed;
                    known->second.strikes++;
                } else {
                    e.st = Cand::Failed;
                    if (known != table_.end()) table_.erase(known);
                    if (!e.id.empty()) dead_[e.addr] = now;
                }
            }
        order.resize(c.size());
        std::iota(order.begin(), order.end(), size_t{0});
        std::stable_sort(order.begin(), order.end(), [&c](size_t a, size_t b) { return c[a].dist < c[b].dist; });
        int alive = 0, outstanding = 0;
        bool waiting = false;
        for (const auto& e : c) outstanding += e.st == Cand::Sent && now - e.sentAt < kSlowAfter ? 1 : 0;
        // The lookup is over when the eight closest nodes that can hold the
        // item have answered and nothing closer is left to ask.
        for (size_t i : order) {
            Cand& e = c[i];
            if (e.st == Cand::Replied && !e.findOnly) {
                if (++alive >= kK) break;
            } else if (e.st == Cand::Sent) {
                waiting = true;
            } else if (e.st == Cand::Fresh) {
                waiting = true;
                if (outstanding >= kAlpha || !TakeSlot()) continue;
                uint16_t tid = nextTid_++;
                ben::Val a = ben::Val::MakeDict();
                a.Set("id", id_).Set("target", target);
                if (!e.findOnly && knownSeq >= 0) a.Set("seq", knownSeq);
                pending[tid] = i;
                SendQuery(e.addr, Query(tid, e.findOnly ? "find_node" : "get", std::move(a)));
                e.st = Cand::Sent;
                e.sentAt = now;
                outstanding++;
                res.queried++;
            }
        }
        if (!waiting) {
            if (res.replied == 0 && !routersAdded) {
                addRouters();
                continue;
            }
            break;
        }

        frn::Addr from;
        std::string data;
        int got = udp_.Receive(from, data, 40);
        if (got < 0) break;
        if (got == 0) continue;
        packetsIn++;
        bytesIn += data.size();
        ben::Val m;
        if (!ben::Decode(data, m) || m.type != ben::Val::Dict) continue;
        std::string t = m.Str_("t");
        if (t.size() != 2) continue;
        auto p = pending.find(static_cast<uint16_t>((static_cast<unsigned char>(t[0]) << 8) | static_cast<unsigned char>(t[1])));
        if (p == pending.end()) continue;
        const size_t idx = p->second;
        if (c[idx].addr.ip != from.ip) continue; // an answer comes from where the question went
        pending.erase(p);
        if (c[idx].st != Cand::Sent) continue; // too late: it was given up on
        res.reached = true;
        res.replied++;
        const std::string y = m.Str_("y");
        const ben::Val* r = m.Get("r");
        if (y != "r" || !r || r->type != ben::Val::Dict) {
            // An error to `get` is a node that does not store values (BEP 44
            // is an extension): it can still say who is closer.
            if (y == "e" && !c[idx].findOnly) {
                c[idx].findOnly = true;
                c[idx].st = Cand::Fresh;
            } else {
                c[idx].st = Cand::Failed;
            }
            continue;
        }
        c[idx].st = Cand::Replied;
        if (!c[idx].id.empty()) Remember(c[idx].addr, c[idx].id);
        if (!c[idx].findOnly) {
            c[idx].token = r->Str_("token");
            if (c[idx].token.empty()) c[idx].findOnly = true; // nothing can be written there
            if (r->Has("seq", ben::Val::Int)) {
                c[idx].hasSeq = true;
                c[idx].seq = r->Int_("seq");
                res.maxSeq = (std::max)(res.maxSeq, c[idx].seq);
            }
            const ben::Val* v = r->Get("v");
            std::string sig = r->Str_("sig");
            if (v && v->type == ben::Val::Str && c[idx].hasSeq && sig.size() == 64 && r->Str_("k") == pub &&
                v->rawEnd - v->rawBegin <= kMaxValue) {
                // The same value comes back from every node that holds it: check it once.
                if (seenValues.insert(sig).second) {
                    if (frc::Ed25519Verify(pub, SignBuffer(salt, c[idx].seq, v->s), sig)) {
                        Item it;
                        it.value = v->s;
                        it.seq = c[idx].seq;
                        res.items.push_back(std::move(it));
                    } else {
                        seenValues.insert("bad" + sig);
                    }
                }
                if (!seenValues.count("bad" + sig)) res.holders++;
            }
        }
        const std::string nodes = r->Str_("nodes");
        for (size_t i = 0; i + 26 <= nodes.size(); i += 26) {
            const unsigned char* b = reinterpret_cast<const unsigned char*>(nodes.data()) + i + 20;
            frn::Addr a;
            a.ip = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                   (static_cast<uint32_t>(b[2]) << 8) | b[3];
            a.port = static_cast<uint16_t>((b[4] << 8) | b[5]);
            add(nodes.substr(i, 20), a, false);
        }
    }

    // The nodes a write would go to, with the tokens they just gave.
    order.resize(c.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&c](size_t a, size_t b) { return c[a].dist < c[b].dist; });
    NearSet set;
    set.at = frn::Now();
    for (size_t i : order) {
        const Cand& e = c[i];
        if (e.st != Cand::Replied || e.findOnly || e.token.empty()) continue;
        Near n;
        n.id = e.id;
        n.addr = e.addr;
        n.token = e.token;
        n.hasSeq = e.hasSeq;
        n.seq = e.seq;
        set.nodes.push_back(std::move(n));
        if (set.nodes.size() >= static_cast<size_t>(kK)) break;
    }
    if (!set.nodes.empty()) {
        if (near_.size() >= kMaxNearSets && !near_.count(target)) {
            auto oldest = near_.begin();
            for (auto it = near_.begin(); it != near_.end(); ++it)
                if (it->second.at < oldest->second.at) oldest = it;
            near_.erase(oldest);
        }
        near_[target] = std::move(set);
    }
    // Nobody at all answered: that is this PC's connection, not the nodes.
    if (res.replied == 0 || dead_.size() > kMaxDead) dead_.clear();
    std::stable_sort(res.items.begin(), res.items.end(), [](const Item& a, const Item& b) { return a.seq > b.seq; });
    res.seconds += frn::Now() - start;
}

GetResult Client::Get(const std::string& pub, const std::string& salt, int64_t knownSeq, double maxSeconds) {
    GetResult res;
    Lookup(Target(pub, salt), pub, salt, knownSeq, maxSeconds, res);
    return res;
}

PutResult Client::Put(const std::string& pub, const std::string& salt, const std::string& value, int64_t seq,
                      const std::string& sig, double maxSeconds, bool refresh) {
    PutResult out;
    const double start = frn::Now();
    const std::string target = Target(pub, salt);
    if (!Ready()) return out;
    auto it = near_.find(target);
    if (it == near_.end() || it->second.nodes.empty() || start - it->second.at > kNearFresh) {
        GetResult g;
        Lookup(target, pub, salt, -1, maxSeconds * 0.7, g);
        out.queried = g.queried;
        out.reached = g.reached;
        it = near_.find(target);
        if (it == near_.end()) {
            out.seconds = frn::Now() - start;
            return out;
        }
    }
    std::vector<Near>& nodes = it->second.nodes;
    struct Try {
        size_t node;
        bool cas;
        bool repeated = false;
        std::string packet{};
    };
    std::vector<Try> queue;
    for (size_t i = 0; i < nodes.size(); i++) queue.push_back({i, true});
    std::map<uint16_t, size_t> pending; // transaction id -> attempt
    size_t next = 0;
    double lastSend = start;
    for (;;) {
        double now = frn::Now();
        if (stop_->load() || now - start > maxSeconds) break;
        while (next < queue.size()) {
            Near& n = nodes[queue[next].node];
            // A node that already holds something newer would refuse: the
            // caller has to read again and write over that. (The same number
            // again only when asked: it keeps the value alive on the node
            // without anything an outsider could see change.)
            if (n.hasSeq && (n.seq > seq || (n.seq == seq && !refresh))) {
                out.conflicts++;
                next++;
                continue;
            }
            if (!TakeSlot()) break;
            ben::Val a = ben::Val::MakeDict();
            a.Set("id", id_).Set("k", pub).Set("seq", seq).Set("sig", sig).Set("token", n.token).Set("v", value);
            if (!salt.empty()) a.Set("salt", salt);
            // Compare-and-swap: only over the version this node showed us.
            if (queue[next].cas && n.hasSeq) a.Set("cas", n.seq);
            uint16_t tid = nextTid_++;
            pending[tid] = next;
            queue[next].packet = Query(tid, "put", std::move(a));
            SendQuery(n.addr, queue[next].packet);
            if (queue[next].cas) out.sent++;
            lastSend = now;
            next++;
        }
        // A write that has not been acknowledged after a while is sent once
        // more: storing the same value twice does no harm, and a lost
        // datagram would otherwise leave that node with the old status.
        bool allRepeated = true;
        if (next >= queue.size() && now - lastSend > kRepeatAfter)
            for (auto& kv : pending) {
                Try& t = queue[kv.second];
                if (t.repeated || !TakeSlot()) continue;
                t.repeated = true;
                SendQuery(nodes[t.node].addr, t.packet);
                lastSend = now;
            }
        for (const auto& kv : pending) allRepeated = allRepeated && queue[kv.second].repeated;
        // (A node silent to both sendings gets one more second, not the full wait again.)
        if (next >= queue.size() && (pending.empty() || now - lastSend > (allRepeated ? 1.0 : kQueryTimeout))) break;
        frn::Addr from;
        std::string data;
        int got = udp_.Receive(from, data, 40);
        if (got < 0) break;
        if (got == 0) continue;
        packetsIn++;
        bytesIn += data.size();
        ben::Val m;
        if (!ben::Decode(data, m) || m.type != ben::Val::Dict) continue;
        std::string t = m.Str_("t");
        if (t.size() != 2) continue;
        auto p = pending.find(static_cast<uint16_t>((static_cast<unsigned char>(t[0]) << 8) | static_cast<unsigned char>(t[1])));
        if (p == pending.end() || nodes[queue[p->second].node].addr.ip != from.ip) continue;
        const Try attempt = queue[p->second];
        Near& n = nodes[attempt.node];
        pending.erase(p);
        out.reached = true;
        const std::string y = m.Str_("y");
        if (y == "r") {
            out.stored++;
            n.hasSeq = true;
            n.seq = seq;
        } else if (y == "e") {
            const ben::Val* e = m.Get("e");
            int64_t code = e && e->type == ben::Val::List && !e->list.empty() && e->list[0].type == ben::Val::Int ? e->list[0].i : 0;
            out.errors += (out.errors.empty() ? "" : ",") + std::to_string(code);
            if (code == 301 && attempt.cas) {
                // "CAS mismatch" although the number sent is the one this
                // node gave a moment ago: a node that follows the first
                // draft of BEP 44, where the field was a hash. It is asked
                // again without the field; the plain rule (only a higher
                // sequence number replaces a value) still holds there.
                queue.push_back({attempt.node, false});
            } else if (code == 302) {
                out.conflicts++; // the node holds a newer version
            }
        }
    }
    out.seconds = frn::Now() - start;
    return out;
}

} // namespace dht
