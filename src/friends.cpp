// See friends.h.
#include "friends.h"
#include "fr_bencode.h"
#include "fr_crypto.h"
#include "fr_dht.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace friends {

const char* const kCardSalt = "card";
const char* const kNoteSalt = "note";
const char* const kStatusSalt = "st";
const char* const kMsgSalt = "msg";

namespace {

// Format version of every stored value (its first byte). Version 1 (round 1:
// one status per code, readable by anyone with the code) is not read any more.
constexpr char kVersion = 2;
// PBKDF2 rounds, twice: between an identity and its code, and between a code
// and the keys of its places. A code is only twelve digits. Making an identity
// that gives a chosen code takes some 10^12 tries of this; trying every code
// against a card's place (to read a nickname off a node) takes 10^12 too.
constexpr uint32_t kCodeRounds = 200000;
// A node accepts 1000 bytes; NSW-Home stops at 800. A node's answer carries
// the value plus some 420 bytes of its own (keys, signature, eight nodes),
// and an answer that does not fit one datagram on the way (1400 bytes on
// some connections) is cut in pieces that many routers throw away.
constexpr size_t kMaxSealed = 800;
constexpr size_t kTag = 16, kNonce = 12;
constexpr size_t kMaxStatusBody = kMaxSealed - 1 - kNonce - kTag;
constexpr size_t kMaxName = 60; // bytes of a nickname or game name
constexpr int64_t kDay = 86400;
// A status' marks for waiting messages: six bytes each (one chance in 2^48 of
// mistaking another's for one's own), and at most a dozen.
constexpr size_t kWakeTag = 6, kMaxWake = 12;

// Cuts UTF-8 text to at most n bytes without splitting a character.
std::string Clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

// The byte strings in this file that start with "dockside" (historical name)
// are the program's former name, kept on purpose: as key-derivation labels,
// salts and signing contexts they are part of the wire format and decide
// every user's friend code. Changing one would cut everyone off from their
// friends; they stay as they are whatever the program is called.

// Encrypt-then-MAC: ChaCha20 (RFC 8439), then HMAC-SHA-256 over nonce and
// ciphertext cut to 16 bytes, both keys made by HKDF (RFC 5869) from
// `material` and a label of their own, so that no key serves two purposes.
std::string Seal(const std::string& material, const char* label, const std::string& nonce, const std::string& plain) {
    const std::string k = frc::Hkdf("", material, std::string("dockside-v2 ") + label, 64);
    const std::string ct = frc::ChaCha20(k.substr(0, 32), nonce, 1, plain);
    return nonce + ct + frc::HmacSha256(k.substr(32), nonce + ct).substr(0, kTag);
}
bool Unseal(const std::string& material, const char* label, const std::string& sealed, std::string& plain) {
    if (sealed.size() < kNonce + kTag) return false;
    const std::string k = frc::Hkdf("", material, std::string("dockside-v2 ") + label, 64);
    const std::string nonce = sealed.substr(0, kNonce), ct = sealed.substr(kNonce, sealed.size() - kNonce - kTag);
    if (!frc::SameBytes(frc::HmacSha256(k.substr(32), nonce + ct).substr(0, kTag), sealed.substr(sealed.size() - kTag))) return false;
    plain = frc::ChaCha20(k.substr(0, 32), nonce, 1, ct);
    return true;
}

ben::Val BodyOf(const Status& st, size_t games, bool moved) {
    ben::Val d = ben::Val::MakeDict();
    d.Set("v", 2);
    d.Set("n", Clip(st.nick, kMaxName));
    d.Set("s", st.state);
    d.Set("t", st.time);
    d.Set("p", static_cast<int64_t>(st.total));
    d.Set("c", st.colour);
    if (moved) d.Set("m", 1);
    if (st.state == Playing) {
        d.Set("g", Clip(st.game, kMaxName));
        d.Set("i", frc::Unhex(st.gameId));
    }
    ben::Val list = ben::Val::MakeList();
    for (size_t i = 0; i < games && i < st.games.size(); i++) {
        ben::Val g = ben::Val::MakeList();
        g.list.push_back(frc::Unhex(st.games[i].id));
        g.list.push_back(Clip(st.games[i].name, kMaxName));
        g.list.push_back(static_cast<int64_t>(st.games[i].seconds));
        list.list.push_back(std::move(g));
    }
    d.Set("l", std::move(list));
    if (!st.wake.empty()) {
        ben::Val w = ben::Val::MakeList();
        for (size_t i = 0; i < st.wake.size() && i < kMaxWake; i++) w.list.push_back(st.wake[i].substr(0, kWakeTag));
        d.Set("w", std::move(w));
    }
    return d;
}

// The body with as many games as fit in `limit` bytes.
std::string EncodeBody(const Status& st, size_t limit, bool moved = false) {
    size_t n = st.games.size();
    for (;;) {
        std::string out = ben::Encode(BodyOf(st, n, moved));
        if (out.size() <= limit || n == 0) return out;
        n--;
    }
}

bool DecodeBody(const std::string& body, Status& st, bool* moved = nullptr) {
    ben::Val d;
    if (!ben::Decode(body, d) || d.type != ben::Val::Dict || d.Int_("v") != 2) return false;
    if (!d.Has("n", ben::Val::Str) || !d.Has("t", ben::Val::Int)) return false;
    st = Status();
    st.nick = Clip(d.Str_("n"), kMaxName);
    int64_t state = d.Int_("s");
    st.state = state == Playing ? Playing : state == SignedOff ? SignedOff : InMenu;
    st.time = d.Int_("t");
    st.total = static_cast<uint64_t>((std::max)(int64_t{0}, d.Int_("p")));
    int64_t colour = d.Int_("c", -1);
    st.colour = colour >= 0 && colour < 64 ? static_cast<int>(colour) : -1;
    st.game = Clip(d.Str_("g"), kMaxName);
    st.gameId = frc::Hex(d.Str_("i").substr(0, 8));
    if (moved) *moved = d.Int_("m") != 0;
    if (const ben::Val* l = d.Get("l"))
        if (l->type == ben::Val::List)
            for (const auto& g : l->list) {
                if (g.type != ben::Val::List || g.list.size() != 3 || g.list[0].type != ben::Val::Str ||
                    g.list[1].type != ben::Val::Str || g.list[2].type != ben::Val::Int)
                    continue;
                GameTime t;
                t.id = frc::Hex(g.list[0].s.substr(0, 8));
                t.name = Clip(g.list[1].s, kMaxName);
                t.seconds = static_cast<uint64_t>((std::max)(int64_t{0}, g.list[2].i));
                st.games.push_back(std::move(t));
            }
    if (const ben::Val* w = d.Get("w"))
        if (w->type == ben::Val::List)
            for (const auto& t : w->list)
                if (t.type == ben::Val::Str && t.s.size() == kWakeTag && st.wake.size() < kMaxWake) st.wake.push_back(t.s);
    return true;
}

int ColourOf(int64_t c) {
    return c >= 0 && c < 64 ? static_cast<int>(c) : -1;
}

// X25519 with the all-zero answer of a low-order point refused (RFC 7748, 6.1).
std::string SharedSecret(const std::string& secret, const std::string& theirs) {
    std::string s = frc::X25519(secret, theirs);
    if (s.size() != 32 || frc::SameBytes(s, std::string(32, '\0'))) return {};
    return s;
}

} // namespace

bool ValidCode(const std::string& code) {
    if (code.size() != 12) return false;
    for (char c : code)
        if (c < '0' || c > '9') return false;
    return true;
}

std::string CodeOf(const std::string& identity) {
    const std::string h = frc::Pbkdf2Sha256(identity, "dockside.friendcode.v2", kCodeRounds);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | static_cast<unsigned char>(h[i]);
    // (64 bits folded to twelve digits: no code is more than 1.00002 times as likely as another.)
    char out[16];
    snprintf(out, sizeof out, "%012llu", static_cast<unsigned long long>(v % 1000000000000ULL));
    return out;
}

Keys DeriveKeys(const std::string& code) {
    Keys k;
    std::string master = frc::Pbkdf2Sha256("dockside-friend-code:" + code, "dockside.friends.v2", kCodeRounds);
    k.seed = frc::Sha256("slot" + master);
    k.seal = frc::Sha256("seal" + master);
    k.pub = frc::Ed25519Public(k.seed);
    return k;
}

int64_t DayOf(int64_t unix) {
    return unix >= 0 ? unix / kDay : (unix - kDay + 1) / kDay;
}

std::string InboxSalt(int64_t day, int slot) {
    return "rq" + std::to_string(day) + "." + std::to_string(slot);
}

std::string SealCard(const Keys& k, const std::string& idSeed, const Card& c, bool open) {
    ben::Val d = ben::Val::MakeDict();
    d.Set("v", 2).Set("o", open ? 1 : 0).Set("x", c.dh);
    if (open) d.Set("n", Clip(c.nick, kMaxName)).Set("c", c.colour);
    const std::string body = ben::Encode(d);
    const std::string idPub = frc::Ed25519Public(idSeed);
    // Signed together with the place it is meant for: a card cannot be moved to another code.
    const std::string plain = idPub + frc::Ed25519Sign(idSeed, idPub, "dockside-card-v2" + k.pub + body) + body;
    // The nonce follows from the content, so that the same card seals to the
    // same bytes and can be written again without anything changing.
    return std::string(1, kVersion) + Seal(k.seal, "card", frc::HmacSha256(k.seal, plain).substr(0, kNonce), plain);
}

bool OpenCard(const Keys& k, const std::string& code, const std::string& value, Card& c, bool& open, bool checkCode) {
    std::string plain;
    if (value.size() < 1 + kNonce + kTag + 96 || value.size() > kMaxSealed || value[0] != kVersion) return false;
    if (!Unseal(k.seal, "card", value.substr(1), plain) || plain.size() < 97) return false;
    const std::string pub = plain.substr(0, 32), sig = plain.substr(32, 64), body = plain.substr(96);
    if (!frc::Ed25519Verify(pub, "dockside-card-v2" + k.pub + body, sig)) return false;
    ben::Val d;
    if (!ben::Decode(body, d) || d.type != ben::Val::Dict || d.Int_("v") != 2 || d.Str_("x").size() != 32) return false;
    if (checkCode && CodeOf(pub) != code) return false;
    c = Card();
    c.code = code;
    c.identity = pub;
    c.dh = d.Str_("x");
    open = d.Int_("o") != 0;
    c.nick = Clip(d.Str_("n"), kMaxName);
    c.colour = ColourOf(d.Int_("c", -1));
    return true;
}

std::string SealRequest(const Card& to, const std::string& fromSeed, const Envelope& e, const std::string& ephemeral) {
    const std::string ePub = frc::X25519Base(ephemeral), ss = SharedSecret(ephemeral, to.dh);
    if (ss.empty() || to.identity.size() != 32) return {};
    ben::Val d = ben::Val::MakeDict();
    d.Set("v", 2).Set("k", e.kind).Set("c", e.from.code).Set("x", e.from.dh).Set("t", e.time);
    if (e.kind == 1) d.Set("n", Clip(e.from.nick, kMaxName)).Set("o", e.from.colour);
    const std::string body = ben::Encode(d);
    const std::string idPub = frc::Ed25519Public(fromSeed);
    // Signed for this recipient: a request cannot be passed on to someone else.
    const std::string plain = idPub + frc::Ed25519Sign(fromSeed, idPub, "dockside-request-v2" + to.identity + body) + body;
    // A new X25519 key for every request: the sealed bytes say nothing about
    // who sent them, even to someone who knows the sender's card.
    return std::string(1, kVersion) + ePub + Seal(frc::Sha256(ss + ePub + to.dh), "request", std::string(kNonce, '\0'), plain);
}

bool OpenRequest(const std::string& myIdentity, const std::string& myDhSecret, const std::string& myDh, const std::string& value, Envelope& e) {
    if (value.size() < 1 + 32 + kNonce + kTag + 97 || value.size() > kMaxSealed || value[0] != kVersion) return false;
    const std::string ePub = value.substr(1, 32), ss = SharedSecret(myDhSecret, ePub);
    std::string plain;
    if (ss.empty() || !Unseal(frc::Sha256(ss + ePub + myDh), "request", value.substr(33), plain) || plain.size() < 97) return false;
    const std::string pub = plain.substr(0, 32), sig = plain.substr(32, 64), body = plain.substr(96);
    if (!frc::Ed25519Verify(pub, "dockside-request-v2" + myIdentity + body, sig)) return false;
    ben::Val d;
    if (!ben::Decode(body, d) || d.type != ben::Val::Dict || d.Int_("v") != 2) return false;
    e = Envelope();
    e.kind = static_cast<int>(d.Int_("k"));
    e.time = d.Int_("t");
    e.from.identity = pub;
    e.from.code = d.Str_("c");
    e.from.dh = d.Str_("x");
    e.from.nick = Clip(d.Str_("n"), kMaxName);
    e.from.colour = ColourOf(d.Int_("o", -1));
    return (e.kind == 1 || e.kind == 2) && ValidCode(e.from.code) && e.from.dh.size() == 32 && pub != myIdentity;
}

PairKeys PairKeysOf(const std::string& shared, const std::string& writer) {
    PairKeys k;
    k.seed = frc::Hkdf("dockside-note-v2", shared, "place" + writer, 32);
    k.seal = frc::Hkdf("dockside-note-v2", shared, "seal" + writer, 32);
    k.pub = frc::Ed25519Public(k.seed);
    return k;
}

std::string SealNote(const PairKeys& k, const Note& n, const std::string& nonce) {
    ben::Val d = ben::Val::MakeDict();
    d.Set("v", 2).Set("s", n.state).Set("t", n.time);
    if (n.state == 1) d.Set("g", n.gen).Set("k", n.key).Set("p", n.place);
    return std::string(1, kVersion) + Seal(k.seal, "note", nonce, ben::Encode(d));
}

bool OpenNote(const PairKeys& k, const std::string& value, Note& n) {
    std::string plain;
    if (value.size() < 1 + kNonce + kTag || value.size() > kMaxSealed || value[0] != kVersion) return false;
    if (!Unseal(k.seal, "note", value.substr(1), plain)) return false;
    ben::Val d;
    if (!ben::Decode(plain, d) || d.type != ben::Val::Dict || d.Int_("v") != 2) return false;
    n = Note();
    n.state = d.Int_("s") == 1 ? 1 : 0;
    n.time = d.Int_("t");
    n.gen = d.Int_("g");
    n.key = d.Str_("k");
    n.place = d.Str_("p");
    return n.state == 0 || (n.key.size() == 32 && n.place.size() == 32);
}

std::string SealStatus(const std::string& key, const Status& st, bool moved, const std::string& nonce) {
    return std::string(1, kVersion) + Seal(key, "status", nonce, EncodeBody(st, kMaxStatusBody, moved));
}

bool OpenStatus(const std::string& key, const std::string& value, Status& st, bool& moved) {
    std::string plain;
    if (value.size() < 1 + kNonce + kTag || value.size() > kMaxSealed || value[0] != kVersion) return false;
    return Unseal(key, "status", value.substr(1), plain) && DecodeBody(plain, st, &moved);
}

std::string SealMessage(const PairKeys& k, const std::string& body, const std::string& nonce) {
    // (Its own label: the key that seals messages is not the one that seals notes.)
    return std::string(1, kVersion) + Seal(k.seal, "message", nonce, body);
}

bool OpenMessage(const PairKeys& k, const std::string& value, std::string& body) {
    if (value.size() < 1 + kNonce + kTag || value.size() > kMaxSealed || value[0] != kVersion) return false;
    return Unseal(k.seal, "message", value.substr(1), body);
}

std::string WakeTag(const PairKeys& k, int64_t statusTime) {
    return frc::HmacSha256(k.seal, "dockside-wake-v2 " + std::to_string(statusTime)).substr(0, kWakeTag);
}

int64_t UnixNow() {
    return static_cast<int64_t>(std::time(nullptr));
}

bool Online(const Friend& f, int64_t now) {
    return f.known && f.st.state != SignedOff && now - f.seen < kOnlineSeconds;
}

namespace {

// ---- the module's state ---------------------------------------------------

struct Mate {
    Friend f;
    std::string identity, dh;     // theirs
    std::string key, place;       // their status key and place, from their note
    int64_t gen = -1;
    // Not kept on disk:
    std::string shared;           // X25519 secret of the two
    double readAt = 0, noteReadAt = 0, noteWriteAt = 0, lastOkAt = -1e9;
    int64_t knownSeq = -1, noteSeq = -1;
    bool noteDirty = true;        // our note to them has to be written
    // Messages (not kept on disk: they only matter for minutes). Ours to them:
    std::string msgOut;
    bool msgDirty = false;
    MsgState msgState = MsgState::None;
    int msgFails = 0, msgSerial = 0;
    double msgWriteAt = 0;
    int64_t msgAt = 0;            // when it was stored (Unix): the status carries its mark for a while
    // Theirs to us:
    std::string msgIn;
    int64_t msgInSeq = -1, msgInAt = 0;
    double msgReadAt = -1;        // -1: no read due; else when
    int msgRetries = 0;
    bool expect = false;
    double expectAt = 0;
};

struct Asking {
    Request r;
    double writeAt = 0, pollAt = 0;
    int64_t day = -1;
    int slot = -1;
};

// Writes that have to happen whatever else goes on: kind 1, a withdrawn
// request's notice into its recipient's inbox; 2, "the friendship is over"
// in our note to someone; 3, "moved" at a status place no longer used.
struct Errand {
    int kind = 0;
    Card to;
    std::string seed, key;
    int left = 1;
    int64_t time = 0;
    double at = 0;
};

struct Owner {
    bool sharing = false;
    int colour = -2; // -2: never chosen
    std::string idSeed, idPub, dhSecret, dhPub, code;
    // The card as last written, and where things stand with it.
    std::string cardValue;
    int64_t cardSeq = 0;
    bool cardOpen = false, cardDirty = true, cardTrouble = false;
    double cardAt = 0;
    // The status key and place of now.
    int64_t gen = 0;
    std::string key, seed;
    std::vector<Mate> mates;
    std::vector<Request> received;
    std::vector<Asking> sent;
    std::vector<std::pair<std::string, int64_t>> declined; // identity, time of the request declined
    std::vector<Errand> errands;
    // Inbox places: when each was read, the version known, whether it holds something that is not ours.
    std::map<std::string, double> inboxAt;
    std::map<std::string, int64_t> inboxSeq;
    std::map<std::string, bool> junk;
    std::set<std::string> inboxOnce;
    // The status.
    Status self;
    bool selfSet = false, dirty = false;
    double dirtyAt = 0, nextPublish = 0;
    bool live = false; // a status that is not "signed off" is out there
    std::string liveSeed, liveKey;
    int64_t statusTime = 0; // of the last status written: the next one is always later
    Status liveSt;
};

// What the network is: the DHT, or (unit tests) a store inside this process.
class Net {
public:
    virtual ~Net() = default;
    virtual dht::GetResult Get(const std::string& pub, const std::string& salt, int64_t knownSeq, double maxSeconds) = 0;
    virtual dht::PutResult Put(const std::string& pub, const std::string& salt, const std::string& value, int64_t seq,
                               const std::string& sig, double maxSeconds, bool refresh) = 0;
    virtual void SetRate(double) {}
    virtual void Count(Traffic&) {}
    virtual void Close() {}
};

struct MemItem {
    std::string value;
    int64_t seq = 0;
};
std::mutex memM;
std::map<std::string, MemItem> memStore;

class MemNet : public Net {
public:
    dht::GetResult Get(const std::string& pub, const std::string& salt, int64_t knownSeq, double) override {
        dht::GetResult g;
        g.reached = true;
        std::string v;
        int64_t seq = 0;
        if (MemGet(pub, salt, v, seq)) {
            g.maxSeq = seq;
            g.holders = 1;
            if (seq > knownSeq) g.items.push_back({v, seq});
        }
        return g;
    }
    dht::PutResult Put(const std::string& pub, const std::string& salt, const std::string& value, int64_t seq, const std::string& sig,
                       double, bool refresh) override {
        dht::PutResult r;
        r.reached = true;
        r.sent = 1;
        std::string old;
        int64_t oldSeq = 0;
        if (MemGet(pub, salt, old, oldSeq) && (oldSeq > seq || (oldSeq == seq && !refresh && old != value))) {
            r.conflicts = 1;
            return r;
        }
        r.stored = MemPut(pub, salt, value, seq, sig) ? 1 : 0;
        return r;
    }
};

class DhtNet : public Net {
public:
    explicit DhtNet(std::atomic<bool>* stop) : c_(stop) {}
    dht::GetResult Get(const std::string& pub, const std::string& salt, int64_t knownSeq, double maxSeconds) override {
        return c_.Get(pub, salt, knownSeq, maxSeconds);
    }
    dht::PutResult Put(const std::string& pub, const std::string& salt, const std::string& value, int64_t seq, const std::string& sig,
                       double maxSeconds, bool refresh) override {
        return c_.Put(pub, salt, value, seq, sig, maxSeconds, refresh);
    }
    void SetRate(double r) override { c_.SetRate(r); }
    void Count(Traffic& t) override {
        t.packetsOut = c_.packetsOut;
        t.packetsIn = c_.packetsIn;
        t.bytesOut = c_.bytesOut;
        t.bytesIn = c_.bytesIn;
    }
    void Close() override { c_.Close(); }

private:
    dht::Client c_;
};

// Everything the module holds. Kept alive by the thread as well, so that a
// thread that could not be stopped in time (stuck in a name lookup) can be
// left behind at exit without touching freed memory.
struct Shared {
    std::mutex m;
    std::condition_variable cv;
    bool inited = false, offline = false, memory = false, threadStarted = false;
    std::string dir;
    std::map<std::string, Owner> owners;

    Look look = Look::Idle;
    bool lookTaken = false;
    std::string lookCode;
    Card lookResult;
    double lookReadyAt = 0;
    Look testLook = Look::NotFound;
    Card testCard;
    double testDelay = 1;

    bool watching = false, refresh = false;
    int refreshing = 0;
    std::atomic<unsigned> version{0};
    std::atomic<bool> abortOp{false}, quit{false}, done{false};
    Tuning tune;
    Traffic traffic;
    std::function<void(const std::string&)> log;
};

std::shared_ptr<Shared>& G() {
    static std::shared_ptr<Shared> g = std::make_shared<Shared>();
    return g;
}

void Log(Shared& s, const std::string& line) {
    if (s.log) s.log(line);
}

// ---- identities -------------------------------------------------------------

void NewGen(Owner& o) {
    o.gen++;
    o.key = frc::RandomBytes(32);
    o.seed = frc::RandomBytes(32);
}

void Complete(Owner& o) {
    if (o.idSeed.size() != 32) {
        o.idSeed = frc::RandomBytes(32);
        o.code.clear();
    }
    o.idPub = frc::Ed25519Public(o.idSeed);
    o.dhSecret = frc::Sha256("dockside-dh-v2" + o.idSeed);
    o.dhPub = frc::X25519Base(o.dhSecret);
    if (o.key.size() != 32 || o.seed.size() != 32) NewGen(o);
}

Owner& OwnerOf(Shared& s, const std::string& id) {
    Owner& o = s.owners[id];
    if (o.idPub.empty()) Complete(o);
    return o;
}

// (A fifth of a second, once per identity.)
const std::string& CodeFor(Owner& o) {
    if (o.code.empty()) o.code = CodeOf(o.idPub);
    return o.code;
}

Card CardOf(Owner& o) {
    Card c;
    c.code = CodeFor(o);
    c.nick = o.self.nick;
    c.colour = o.colour != -2 ? o.colour : o.self.colour;
    c.identity = o.idPub;
    c.dh = o.dhPub;
    return c;
}

Mate* MateOf(Owner& o, const std::string& identity) {
    for (Mate& m : o.mates)
        if (m.identity == identity) return &m;
    return nullptr;
}
Mate* MateByCode(Owner& o, const std::string& code) {
    for (Mate& m : o.mates)
        if (m.f.code == code) return &m;
    return nullptr;
}

// A new status key and place: everyone still a friend is given them in our
// note; the old place is told "moved" once and then left alone. Whoever is
// not a friend any more has neither.
void Rotate(Owner& o) {
    // (The old place gets "moved", not a goodbye with the last status in it.)
    o.live = false;
    Errand e;
    e.kind = 3;
    e.seed = o.seed;
    e.key = o.key;
    o.errands.push_back(e);
    NewGen(o);
    for (Mate& m : o.mates) m.noteDirty = true;
    o.dirty = true;
    o.dirtyAt = 0;
}

void MakeFriend(Shared& s, Owner& o, const Card& who) {
    Mate* m = MateOf(o, who.identity);
    if (!m) {
        o.mates.emplace_back();
        m = &o.mates.back();
    }
    *m = Mate();
    m->identity = who.identity;
    m->dh = who.dh;
    m->f.code = who.code;
    m->f.st.nick = who.nick;
    m->f.st.colour = who.colour;
    m->f.st.state = SignedOff;
    m->f.since = UnixNow();
    m->shared = SharedSecret(o.dhSecret, who.dh);
    // (By code: a code is made from its identity, so one names the other.)
    o.received.erase(std::remove_if(o.received.begin(), o.received.end(), [&](const Request& r) { return r.who.code == who.code; }),
                     o.received.end());
    o.sent.erase(std::remove_if(o.sent.begin(), o.sent.end(), [&](const Asking& a) { return a.r.who.code == who.code; }), o.sent.end());
    // Our status is written at once: they may be looking.
    o.dirty = true;
    o.dirtyAt = 0;
    s.version++;
}

// ---- friends.dat ------------------------------------------------------------

// Plain C files (the C++ streams would add a fifth to the size of the
// program); the path is UTF-8, which Windows' C library only takes as UTF-16.
#ifdef _WIN32
std::wstring Wide(const std::string& u8) {
    std::wstring out;
    for (size_t i = 0; i < u8.size();) {
        unsigned char c = static_cast<unsigned char>(u8[i]);
        uint32_t cp = c;
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n > 1) cp = c & (0xFF >> (n + 1));
        for (size_t k = 1; k < n && i + k < u8.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(u8[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<wchar_t>(cp);
        }
    }
    return out;
}
FILE* OpenFile(const std::string& path, bool write) {
    FILE* f = nullptr;
    return _wfopen_s(&f, Wide(path).c_str(), write ? L"wb" : L"rb") == 0 ? f : nullptr;
}
bool MoveOver(const std::string& from, const std::string& to) {
    if (_wrename(Wide(from).c_str(), Wide(to).c_str()) == 0) return true;
    _wremove(Wide(to).c_str()); // rename does not replace a file there
    return _wrename(Wide(from).c_str(), Wide(to).c_str()) == 0;
}
const char kSlash = '\\';
#else
FILE* OpenFile(const std::string& path, bool write) {
    return fopen(path.c_str(), write ? "wb" : "rb");
}
bool MoveOver(const std::string& from, const std::string& to) {
    return rename(from.c_str(), to.c_str()) == 0;
}
const char kSlash = '/';
#endif

std::string StorePath(const Shared& s) {
    return s.dir + kSlash + "friends.dat";
}

ben::Val CardVal(const Card& c) {
    ben::Val e = ben::Val::MakeDict();
    e.Set("c", c.code).Set("n", c.nick).Set("o", c.colour).Set("i", c.identity).Set("x", c.dh);
    return e;
}
Card CardFrom(const ben::Val& e) {
    Card c;
    c.code = e.Str_("c");
    c.nick = Clip(e.Str_("n"), kMaxName);
    c.colour = ColourOf(e.Int_("o", -1));
    c.identity = e.Str_("i");
    c.dh = e.Str_("x");
    return c;
}
bool CardOk(const Card& c) {
    return ValidCode(c.code) && c.identity.size() == 32 && c.dh.size() == 32;
}

void Save(Shared& s) {
    if (s.offline || s.dir.empty()) return;
    ben::Val users = ben::Val::MakeDict();
    for (const auto& kv : s.owners) {
        const Owner& o = kv.second;
        ben::Val u = ben::Val::MakeDict();
        u.Set("on", o.sharing ? 1 : 0).Set("id", o.idSeed).Set("col", o.colour).Set("code", o.code);
        u.Set("cv", o.cardValue).Set("cs", o.cardSeq).Set("co", o.cardOpen ? 1 : 0);
        u.Set("g", o.gen).Set("gk", o.key).Set("gs", o.seed);
        ben::Val f = ben::Val::MakeList();
        for (const Mate& m : o.mates) {
            Card c;
            c.code = m.f.code;
            c.nick = m.f.st.nick;
            c.colour = m.f.st.colour;
            c.identity = m.identity;
            c.dh = m.dh;
            ben::Val e = CardVal(c);
            e.Set("k", m.key).Set("p", m.place).Set("g", m.gen).Set("s", m.f.seen).Set("a", m.f.since);
            if (m.f.known) e.Set("b", EncodeBody(m.f.st, 4096));
            if (m.noteDirty) e.Set("w", 1);
            f.list.push_back(std::move(e));
        }
        u.Set("f", std::move(f));
        ben::Val r = ben::Val::MakeList();
        for (const Request& q : o.received) r.list.push_back(CardVal(q.who).Set("t", q.time).Set("a", q.at));
        u.Set("r", std::move(r));
        ben::Val q = ben::Val::MakeList();
        for (const Asking& a : o.sent) q.list.push_back(CardVal(a.r.who).Set("t", a.r.time).Set("a", a.r.at));
        u.Set("q", std::move(q));
        ben::Val d = ben::Val::MakeList();
        for (const auto& x : o.declined) d.list.push_back(ben::Val::MakeDict().Set("i", x.first).Set("t", x.second));
        u.Set("d", std::move(d));
        ben::Val e = ben::Val::MakeList();
        for (const Errand& x : o.errands)
            e.list.push_back(CardVal(x.to).Set("k", x.kind).Set("sd", x.seed).Set("ky", x.key).Set("l", x.left).Set("t", x.time));
        u.Set("e", std::move(e));
        users.Set(kv.first, std::move(u));
    }
    ben::Val root = ben::Val::MakeDict();
    root.Set("v", 2).Set("users", std::move(users));
    // (The identity keys are in this file: written whole, beside it, then moved over it.)
    const std::string path = StorePath(s), tmp = path + ".tmp", text = ben::Encode(root);
    FILE* f = OpenFile(tmp, true);
    if (!f) return;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (ok) MoveOver(tmp, path);
}

void Load(Shared& s) {
    FILE* f = OpenFile(StorePath(s), false);
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    ben::Val root;
    if (!ben::Decode(text, root)) return;
    const ben::Val* users = root.Get("users");
    if (!users || users->type != ben::Val::Dict) return;
    // Version 1 (round 1's trial) is read for the identity and the sharing
    // choice only; its friends were one-way and are not carried over.
    const bool v1 = root.Int_("v") != 2;
    for (const auto& kv : users->dict) {
        const ben::Val& u = kv.second;
        if (u.type != ben::Val::Dict) continue;
        Owner o;
        o.sharing = u.Int_("on") != 0;
        o.colour = static_cast<int>(u.Int_("col", -2));
        o.idSeed = u.Str_("id");
        if (o.idSeed.size() != 32) o.idSeed.clear();
        if (!v1) {
            o.code = u.Str_("code");
            if (!ValidCode(o.code)) o.code.clear();
            o.cardValue = u.Str_("cv");
            o.cardSeq = u.Int_("cs");
            o.cardOpen = u.Int_("co") != 0;
            o.gen = u.Int_("g");
            o.key = u.Str_("gk");
            o.seed = u.Str_("gs");
        }
        Complete(o);
        if (o.idSeed != u.Str_("id")) o.code.clear();
        auto list = [&](const char* key) -> const std::vector<ben::Val>* {
            const ben::Val* l = v1 ? nullptr : u.Get(key);
            return l && l->type == ben::Val::List ? &l->list : nullptr;
        };
        if (auto l = list("f"))
            for (const auto& e : *l) {
                Card c = CardFrom(e);
                if (!CardOk(c)) continue;
                Mate m;
                m.identity = c.identity;
                m.dh = c.dh;
                m.f.code = c.code;
                m.key = e.Str_("k");
                m.place = e.Str_("p");
                m.gen = e.Int_("g", -1);
                if (m.key.size() != 32 || m.place.size() != 32) m.key.clear(), m.place.clear(), m.gen = -1;
                m.f.seen = e.Int_("s");
                m.f.since = e.Int_("a");
                m.f.known = e.Has("b", ben::Val::Str) && DecodeBody(e.Str_("b"), m.f.st);
                if (!m.f.known) {
                    m.f.st.nick = c.nick;
                    m.f.st.colour = c.colour;
                    m.f.st.state = SignedOff;
                }
                m.noteDirty = e.Int_("w") != 0;
                m.shared = SharedSecret(o.dhSecret, m.dh);
                if (!m.shared.empty()) o.mates.push_back(std::move(m));
            }
        if (auto l = list("r"))
            for (const auto& e : *l) {
                Request q;
                q.who = CardFrom(e);
                q.time = e.Int_("t");
                q.at = e.Int_("a");
                if (CardOk(q.who)) o.received.push_back(q);
            }
        if (auto l = list("q"))
            for (const auto& e : *l) {
                Asking a;
                a.r.who = CardFrom(e);
                a.r.time = e.Int_("t");
                a.r.at = e.Int_("a");
                if (CardOk(a.r.who)) o.sent.push_back(a);
            }
        if (auto l = list("d"))
            for (const auto& e : *l) o.declined.emplace_back(e.Str_("i"), e.Int_("t"));
        if (auto l = list("e"))
            for (const auto& e : *l) {
                Errand x;
                x.to = CardFrom(e);
                x.kind = static_cast<int>(e.Int_("k"));
                x.seed = e.Str_("sd");
                x.key = e.Str_("ky");
                x.left = static_cast<int>(e.Int_("l", 1));
                x.time = e.Int_("t");
                if (x.kind >= 1 && x.kind <= 3 && x.left > 0) o.errands.push_back(x);
            }
        s.owners[kv.first] = std::move(o);
    }
}

// ---- the network thread ---------------------------------------------------------

// How often the answer to a request still waiting is looked for: often at
// first (the other person may be at the PC right now), then less.
double AnswerGap(const Tuning& t, double age) {
    return age < 15 * t.answerSeconds ? t.answerSeconds : age < 60 * t.answerSeconds ? 5 * t.answerSeconds : 15 * t.answerSeconds;
}

void Run(std::shared_ptr<Shared> sp) {
    Shared& s = *sp;
    std::unique_ptr<Net> net;
    if (s.memory) net.reset(new MemNet());
    else net.reset(new DhtNet(&s.abortOp));
    std::map<std::string, Keys> keyCache;          // deriving a code's keys takes a fifth of a second
    std::map<std::string, std::string> codeCache;  // and so does an identity's code
    std::map<std::string, int64_t> lastSeq;        // per place: the highest number we wrote
    auto keysOf = [&](const std::string& code) -> Keys {
        auto it = keyCache.find(code);
        if (it == keyCache.end()) it = keyCache.emplace(code, DeriveKeys(code)).first;
        return it->second;
    };
    auto codeOf = [&](const std::string& identity) -> std::string {
        auto it = codeCache.find(identity);
        if (it == codeCache.end()) it = codeCache.emplace(identity, CodeOf(identity)).first;
        return it->second;
    };
    auto get = [&](const std::string& pub, const std::string& salt, int64_t known, double secs) {
        s.traffic.gets++;
        return net->Get(pub, salt, known, secs);
    };
    // Writes one item, going above whatever number a node holds (written by
    // someone else who has the place's key, or by a clock that ran ahead).
    // Called without the lock. `seq` < 0: the clock.
    auto put = [&](const std::string& what, const std::string& seed, const std::string& salt, const std::string& value, int64_t seq,
                   double maxSeconds, bool refresh = false) {
        const std::string pub = frc::Ed25519Public(seed);
        const std::string where = pub + salt;
        if (seq < 0) seq = (std::max)(lastSeq[where] + 1, UnixNow());
        s.traffic.puts++;
        dht::PutResult r = net->Put(pub, salt, value, seq, frc::Ed25519Sign(seed, pub, dht::SignBuffer(salt, seq, value)), maxSeconds, refresh);
        int queried = r.queried;
        double seconds = r.seconds;
        if (r.conflicts > 0 && r.stored == 0 && !s.abortOp.load()) {
            dht::GetResult g = get(pub, salt, -1, maxSeconds);
            seq = (std::max)(g.maxSeq, seq) + 1;
            s.traffic.puts++;
            r = net->Put(pub, salt, value, seq, frc::Ed25519Sign(seed, pub, dht::SignBuffer(salt, seq, value)), maxSeconds, false);
            queried += g.queried + r.queried;
            seconds += g.seconds + r.seconds;
        }
        lastSeq[where] = (std::max)(lastSeq[where], seq);
        Log(s, "put " + what + " seq=" + std::to_string(seq) + " stored=" + std::to_string(r.stored) + "/" + std::to_string(r.sent) +
                   " conflicts=" + std::to_string(r.conflicts) + " lookup_queries=" + std::to_string(queried) +
                   " seconds=" + std::to_string(seconds) + (refresh ? " refresh" : "") + (r.errors.empty() ? "" : " errors=" + r.errors) +
                   (r.reached ? "" : " unreachable"));
        return r.stored > 0;
    };
    // Leaves a request (or its withdrawal) in someone's inbox: in a place of
    // today's four that is free or already ours, else in the one that comes
    // from our identity (over whatever lies there).
    auto deliver = [&](const Card& to, const std::string& idSeed, const Envelope& e, int64_t& dayOut, int& slotOut, int prefer) {
        const Keys k = keysOf(to.code);
        const int64_t day = DayOf(UnixNow());
        int first = prefer >= 0 ? prefer : static_cast<int>(static_cast<unsigned char>(frc::Sha256(e.from.identity + to.code + std::to_string(day))[0]) % kInboxSlots);
        int slot = first;
        int64_t above = -1;
        for (int probe = 0; probe < (prefer >= 0 ? 1 : kInboxSlots); probe++) {
            const int at = (first + probe) % kInboxSlots;
            dht::GetResult g = get(k.pub, InboxSalt(day, at), -1, 15);
            // Something written in the last day by someone else: taken.
            bool taken = false;
            for (const auto& it : g.items) taken = taken || it.seq > UnixNow() - kDay;
            if (!taken || probe == kInboxSlots - 1) {
                slot = taken ? first : at;
                if (!taken) above = g.maxSeq;
                break;
            }
            if (s.abortOp.load()) break;
        }
        const std::string value = SealRequest(to, idSeed, e, frc::RandomBytes(32));
        dayOut = day;
        slotOut = slot;
        if (value.empty()) return false;
        int64_t seq = (std::max)(UnixNow(), above + 1);
        return put(std::string(e.kind == 1 ? "request" : "withdrawal") + " to=" + to.code + " place=" + std::to_string(slot), k.seed,
                   InboxSalt(day, slot), value, seq, 20);
    };

    std::unique_lock<std::mutex> lk(s.m);
    for (;;) {
        net->SetRate(s.tune.queriesPerSecond);
        net->Count(s.traffic);
        const double now = frn::Now();
        const int64_t unow = UnixNow();
        const bool quitting = s.quit.load();
        const Tuning tune = s.tune;

        // ---- 1. Goodbyes: whoever was published and should no longer be is
        // shown signed off; a card whose owner stopped sharing is closed.
        bool did = false;
        for (auto& kv : s.owners) {
            Owner& o = kv.second;
            if (o.live && (quitting || !o.sharing)) {
                Status st = o.liveSt;
                st.state = SignedOff;
                st.time = unow;
                st.game.clear();
                const std::string seed = o.liveSeed, key = o.liveKey;
                o.live = false;
                lk.unlock();
                s.abortOp = false;
                put("status state=2", seed, kStatusSalt, SealStatus(key, st, false, frc::RandomBytes(kNonce)), -1, quitting ? 2.5 : 8);
                lk.lock();
                did = true;
                break;
            }
            if (!quitting && !o.sharing && o.cardOpen && o.selfSet) {
                const Card me = CardOf(o);
                const std::string idSeed = o.idSeed;
                const int64_t seq = o.cardSeq + 1;
                lk.unlock();
                const Keys k = keysOf(me.code);
                const std::string value = SealCard(k, idSeed, me, false);
                bool ok = put("card closed", k.seed, kCardSalt, value, seq, 15);
                lk.lock();
                if (ok) {
                    o.cardOpen = false;
                    o.cardSeq = (std::max)(o.cardSeq, lastSeq[k.pub + kCardSalt]);
                    o.cardValue = value;
                    o.cardDirty = true; // written again in full when sharing is back
                    Save(s);
                } else {
                    o.cardOpen = false; // tried once: the card expires on its own anyway
                }
                did = true;
                break;
            }
        }
        if (did) continue;
        if (quitting) break;

        // ---- 2. A code being looked up: someone is waiting at the screen.
        if (s.look == Look::Busy && !s.lookTaken) {
            s.lookTaken = true;
            const std::string code = s.lookCode;
            lk.unlock();
            const Keys k = keysOf(code);
            dht::GetResult g = get(k.pub, kCardSalt, -1, 20);
            Look result = g.reached ? Look::NotFound : Look::NoNetwork;
            Card best;
            int64_t bestSeq = -1;
            int forged = 0;
            bool closed = false;
            for (const auto& it : g.items) {
                Card c;
                bool open = false;
                if (!OpenCard(k, code, it.value, c, open, false) || codeOf(c.identity) != code) {
                    forged++;
                    continue;
                }
                if (it.seq > bestSeq) {
                    bestSeq = it.seq;
                    best = c;
                    closed = !open;
                }
            }
            if (bestSeq >= 0) result = closed ? Look::NotFound : Look::Found;
            else if (forged > 0) result = Look::Unverified;
            Log(s, "lookup code=" + code + " reached=" + std::to_string(g.reached) + " values=" + std::to_string(g.items.size()) +
                       " forged=" + std::to_string(forged) + " result=" + std::to_string(static_cast<int>(result)) + " queries=" +
                       std::to_string(g.queried) + " seconds=" + std::to_string(g.seconds));
            lk.lock();
            if (s.look == Look::Busy && s.lookCode == code) {
                s.look = result;
                s.lookResult = best;
                s.version++;
            }
            continue;
        }

        // ---- 3. Writes the user is waiting for, or that must happen.
        // Messages first (someone is waiting at the screen, maybe on both
        // sides), then errands, then our notes to friends, then new requests.
        for (auto& kv : s.owners) {
            Owner& o = kv.second;
            if (!o.sharing) continue;
            for (Mate& m : o.mates) {
                if (!m.msgDirty || m.msgWriteAt > now || m.shared.empty()) continue;
                const PairKeys pk = PairKeysOf(m.shared, o.idPub);
                const std::string owner = kv.first, identity = m.identity, code = m.f.code, body = m.msgOut;
                const int serial = m.msgSerial;
                lk.unlock();
                bool ok = put("message to=" + code + " bytes=" + std::to_string(body.size()), pk.seed, kMsgSalt,
                              SealMessage(pk, body, frc::RandomBytes(kNonce)), -1, 20);
                lk.lock();
                auto oit = s.owners.find(owner);
                if (oit != s.owners.end())
                    if (Mate* mm = MateOf(oit->second, identity))
                        if (mm->msgSerial == serial) { // (not replaced meanwhile: that one goes next)
                            if (ok) {
                                mm->msgDirty = false;
                                mm->msgState = MsgState::Stored;
                                mm->msgAt = UnixNow();
                                // The mark goes out with the status now, not before: a
                                // reader woken earlier would find the old message.
                                oit->second.dirty = true;
                                oit->second.dirtyAt = frn::Now();
                            } else if (++mm->msgFails >= 4) {
                                mm->msgDirty = false;
                                mm->msgState = MsgState::Failed;
                            } else {
                                mm->msgWriteAt = frn::Now() + 3.0 * mm->msgFails;
                            }
                            s.version++;
                        }
                did = true;
                break;
            }
            if (did) break;
            for (size_t i = 0; i < o.errands.size() && !did; i++) {
                if (o.errands[i].at > now) continue;
                Errand e = o.errands[i];
                const std::string owner = kv.first, idSeed = o.idSeed, idPub = o.idPub, dhSecret = o.dhSecret;
                Card me = CardOf(o);
                lk.unlock();
                bool ok = false;
                if (e.kind == 1) {
                    Envelope env;
                    env.kind = 2;
                    env.from = me;
                    env.time = e.time;
                    int64_t day = 0;
                    int slot = 0;
                    ok = deliver(e.to, idSeed, env, day, slot, -1);
                } else if (e.kind == 2) {
                    const std::string sh = SharedSecret(dhSecret, e.to.dh);
                    if (!sh.empty()) {
                        Note n;
                        n.state = 0;
                        n.time = UnixNow();
                        ok = put("note over to=" + e.to.code, PairKeysOf(sh, idPub).seed, kNoteSalt, SealNote(PairKeysOf(sh, idPub), n, frc::RandomBytes(kNonce)),
                                 -1, 20);
                    } else {
                        ok = true;
                    }
                } else if (e.kind == 3) {
                    Status st;
                    st.state = SignedOff;
                    st.time = UnixNow();
                    ok = put("status moved", e.seed, kStatusSalt, SealStatus(e.key, st, true, frc::RandomBytes(kNonce)), -1, 20);
                }
                lk.lock();
                auto oit = s.owners.find(owner);
                if (oit != s.owners.end()) {
                    for (auto it = oit->second.errands.begin(); it != oit->second.errands.end(); ++it) {
                        if (it->kind != e.kind || it->to.identity != e.to.identity || it->seed != e.seed) continue;
                        // Repeated a few times, apart: one write can be missed
                        // by nodes that drop the item soon after.
                        if (ok) it->left--;
                        it->at = frn::Now() + (ok ? 10 * tune.answerSeconds * (it->left == 1 ? 6 : 1) : 30);
                        if (it->left <= 0) oit->second.errands.erase(it);
                        break;
                    }
                    Save(s);
                }
                did = true;
            }
            if (did) break;
            for (Mate& m : o.mates) {
                if (!(m.noteDirty || m.noteWriteAt <= now) || m.shared.empty()) continue;
                if (!m.noteDirty && m.noteWriteAt == 0) {
                    m.noteWriteAt = now + tune.noteWriteSeconds * 0.5; // (staggered after a start: written again in time)
                    continue;
                }
                Note n;
                n.state = 1;
                n.gen = o.gen;
                n.key = o.key;
                n.place = frc::Ed25519Public(o.seed);
                n.time = unow;
                const PairKeys pk = PairKeysOf(m.shared, o.idPub);
                const std::string owner = kv.first, identity = m.identity, code = m.f.code;
                const int64_t gen = o.gen;
                lk.unlock();
                bool ok = put("note to=" + code + " gen=" + std::to_string(gen), pk.seed, kNoteSalt, SealNote(pk, n, frc::RandomBytes(kNonce)), -1, 20);
                lk.lock();
                auto oit = s.owners.find(owner);
                if (oit != s.owners.end())
                    if (Mate* mm = MateOf(oit->second, identity)) {
                        if (ok && oit->second.gen == gen) mm->noteDirty = false;
                        mm->noteWriteAt = frn::Now() + (ok ? tune.noteWriteSeconds : 30);
                        if (mm->noteReadAt == 0) mm->noteReadAt = frn::Now();
                        Save(s);
                    }
                did = true;
                break;
            }
            if (did) break;
            for (Asking& a : o.sent) {
                if (a.writeAt > now) continue;
                if (unow - a.r.at > 30 * kDay) {
                    a.writeAt = 1e18; // a month without an answer: no longer written, still shown
                    if (a.pollAt == 0) a.pollAt = now;
                    continue;
                }
                Envelope env;
                env.kind = 1;
                env.from = CardOf(o);
                env.time = a.r.time;
                const Card to = a.r.who;
                const std::string owner = kv.first, idSeed = o.idSeed;
                // Written again where it was written today: over our own copy, not into another place.
                const int prefer = a.day == DayOf(unow) ? a.slot : -1;
                lk.unlock();
                int64_t day = 0;
                int slot = 0;
                bool ok = deliver(to, idSeed, env, day, slot, prefer);
                lk.lock();
                auto oit = s.owners.find(owner);
                if (oit != s.owners.end())
                    for (Asking& b : oit->second.sent)
                        if (b.r.who.identity == to.identity) {
                            b.writeAt = frn::Now() + (ok ? tune.resendSeconds : 30);
                            b.day = day;
                            b.slot = slot;
                            if (b.pollAt == 0) b.pollAt = frn::Now() + tune.answerSeconds;
                        }
                did = true;
                break;
            }
            if (did) break;
        }
        if (did) continue;

        // ---- 4. The status: whenever it changes and every ten minutes, while
        // there is anyone to read it.
        for (auto& kv : s.owners) {
            Owner& o = kv.second;
            if (!o.sharing || !o.selfSet || o.mates.empty()) continue;
            const bool signedOff = o.self.state == SignedOff;
            const bool due = (o.dirty && now >= o.dirtyAt) || (!signedOff && now >= o.nextPublish);
            if (!due || (signedOff && !o.live && !o.dirty)) continue;
            o.dirty = false;
            Status st = o.self;
            // A reader takes a status only when it is newer than the last it
            // read, by its time: two in the same second must still differ, or
            // a message's mark in the second would go unseen.
            st.time = (std::max)(unow, o.statusTime + 1);
            o.statusTime = st.time;
            if (o.colour != -2) st.colour = o.colour;
            st.wake.clear();
            for (const Mate& m : o.mates)
                if (m.msgState == MsgState::Stored && m.msgAt > 0 && unow - m.msgAt < tune.wakeSeconds && !m.shared.empty())
                    st.wake.push_back(WakeTag(PairKeysOf(m.shared, o.idPub), st.time));
            const std::string seed = o.seed, key = o.key, owner = kv.first;
            lk.unlock();
            bool ok = put("status state=" + std::to_string(st.state), seed, kStatusSalt, SealStatus(key, st, false, frc::RandomBytes(kNonce)), -1, 20);
            lk.lock();
            auto oit = s.owners.find(owner);
            if (oit != s.owners.end()) {
                Owner& p = oit->second;
                if (ok) {
                    p.live = st.state != SignedOff;
                    p.liveSeed = seed;
                    p.liveKey = key;
                    p.liveSt = st;
                }
                // Nodes drop an item a couple of hours after its last write; a
                // write that reached nobody is tried again soon.
                p.nextPublish = frn::Now() + (ok ? tune.republishSeconds : 30);
            }
            did = true;
            break;
        }
        if (did) continue;

        // ---- 5. Reads, the most overdue first.
        struct Pick {
            int kind = 0; // 1 status, 2 note, 3 answer, 4 inbox, 5 card, 6 message
            double due = 1e18;
            std::string owner, who, salt;
        } pick;
        const double interval = s.watching ? tune.watchSeconds : tune.pollSeconds;
        if (s.refresh) {
            s.refresh = false;
            s.refreshing = 0;
            for (auto& kv : s.owners) {
                if (!kv.second.sharing) continue;
                for (Mate& m : kv.second.mates) {
                    m.readAt = 0;
                    if (m.key.empty()) m.noteReadAt = 0;
                    else s.refreshing++;
                }
                for (Asking& a : kv.second.sent) a.pollAt = now;
                for (auto& x : kv.second.inboxAt) x.second = -1e9;
            }
            s.version++;
        }
        double soonest = now + 1.0;
        auto consider = [&](int kind, double due, const std::string& owner, const std::string& who, const std::string& salt = {}) {
            soonest = (std::min)(soonest, due);
            if (due <= now && due < pick.due) pick = {kind, due, owner, who, salt};
        };
        for (auto& kv : s.owners) {
            Owner& o = kv.second;
            if (!o.sharing) continue;
            for (Mate& m : o.mates) {
                // Messages before everything else that is due: someone waits for them.
                if (m.msgReadAt >= 0) consider(6, m.msgReadAt == 0 ? now - 3e6 : m.msgReadAt, kv.first, m.identity);
                if (m.expect) consider(6, m.expectAt == 0 ? now - 3e6 : m.expectAt, kv.first, m.identity);
                if (!m.key.empty()) consider(1, m.readAt == 0 ? now - 1e6 : m.readAt + interval, kv.first, m.identity);
                double gap = m.key.empty() ? AnswerGap(tune, static_cast<double>(unow - m.f.since)) : tune.noteSeconds;
                consider(2, m.noteReadAt == 0 ? now - 1e6 + 1 : m.noteReadAt + gap, kv.first, m.identity);
            }
            for (Asking& a : o.sent)
                if (a.pollAt > 0) consider(3, a.pollAt, kv.first, a.r.who.identity);
            if (!o.selfSet) continue;
            consider(5, o.cardDirty ? now - 2e6 : o.cardAt, kv.first, {});
            // Today's four places, and the day before (and after) around midnight
            // or once after a start, for clocks that disagree a little.
            const int64_t day = DayOf(unow), into = unow - day * kDay;
            const double inbox = s.watching ? tune.inboxSeconds / 2 : tune.inboxSeconds;
            for (int64_t d = day - 1; d <= day + 1; d++) {
                for (int i = 0; i < kInboxSlots; i++) {
                    const std::string salt = InboxSalt(d, i);
                    if (d != day && !((d < day && into < 900) || (d > day && into > kDay - 900) || (d < day && !o.inboxOnce.count(salt))))
                        continue;
                    auto it = o.inboxAt.find(salt);
                    if (it == o.inboxAt.end()) it = o.inboxAt.emplace(salt, now - inbox + i * inbox / kInboxSlots).first;
                    consider(4, it->second + inbox, kv.first, {}, salt);
                }
            }
        }
        if (pick.kind == 0) {
            double wait = (std::max)(0.02, (std::min)(1.0, soonest - now));
            for (auto& kv : s.owners) {
                const Owner& o = kv.second;
                if (!o.sharing) continue;
                if (o.dirty) wait = (std::min)(wait, (std::max)(0.02, o.dirtyAt - now));
                else if (o.selfSet && !o.mates.empty()) wait = (std::min)(wait, (std::max)(0.02, o.nextPublish - now));
                for (const Errand& e : o.errands) wait = (std::min)(wait, (std::max)(0.02, e.at - now));
                for (const Asking& a : o.sent) wait = (std::min)(wait, (std::max)(0.02, a.writeAt - now));
            }
            s.cv.wait_for(lk, std::chrono::duration<double>(wait));
            continue;
        }

        Owner& o = s.owners[pick.owner];
        const std::string owner = pick.owner;
        if (pick.kind == 6) {
            Mate& m = *MateOf(o, pick.who);
            const bool woken = m.msgReadAt >= 0 && m.msgReadAt <= now;
            if (woken) m.msgReadAt = -1;
            if (m.expect) m.expectAt = now + tune.messageSeconds;
            const PairKeys pk = PairKeysOf(m.shared, m.identity);
            const std::string identity = m.identity, code = m.f.code;
            const int64_t known = m.msgInSeq;
            if (m.shared.empty()) continue;
            lk.unlock();
            dht::GetResult g = get(pk.pub, kMsgSalt, known, 20);
            std::string body;
            int64_t bestSeq = -1;
            for (const auto& it : g.items) {
                std::string b;
                if (it.seq > bestSeq && OpenMessage(pk, it.value, b)) body = b, bestSeq = it.seq;
            }
            Log(s, "message from=" + code + " reached=" + std::to_string(g.reached) + " found=" + std::to_string(bestSeq >= 0 ? 1 : 0) +
                       " seq=" + std::to_string(bestSeq) + " queries=" + std::to_string(g.queried) + " seconds=" + std::to_string(g.seconds));
            lk.lock();
            auto oit = s.owners.find(owner);
            if (oit == s.owners.end()) continue;
            Mate* mm = MateOf(oit->second, identity);
            if (!mm) continue;
            // A wake-up whose read reached nobody, or found nothing at all yet
            // (the nodes asked may not have the value): tried a few times.
            // (Nothing newer than what we hold is the usual answer to a
            // status written again while its mark lasts.)
            if (woken && (!g.reached || (bestSeq < 0 && known < 0)) && mm->msgRetries < 4) {
                mm->msgRetries++;
                mm->msgReadAt = frn::Now() + 3.0 * mm->msgRetries;
            }
            if (bestSeq > mm->msgInSeq) {
                mm->msgInSeq = bestSeq;
                mm->msgIn = body;
                mm->msgInAt = UnixNow();
                s.version++;
            }
            continue;
        }
        if (pick.kind == 1) {
            Mate& m = *MateOf(o, pick.who);
            const std::string place = m.place, key = m.key, code = m.f.code, identity = m.identity;
            const int64_t knownSeq = m.knownSeq;
            m.readAt = now; // (also when it fails: tried again after an interval)
            lk.unlock();
            dht::GetResult g = get(place, kStatusSalt, knownSeq, 20);
            Status best;
            bool found = false, moved = false;
            int64_t bestSeq = -1;
            for (const auto& it : g.items) {
                Status st;
                bool mv = false;
                if (!OpenStatus(key, it.value, st, mv)) continue;
                if (mv) {
                    moved = true;
                    continue;
                }
                if (!found || st.time > best.time) best = st, bestSeq = it.seq;
                found = true;
            }
            Log(s, "get code=" + code + " reached=" + std::to_string(g.reached) + " statuses=" + std::to_string(found ? 1 : 0) + " holders=" +
                       std::to_string(g.holders) + " max_seq=" + std::to_string(g.maxSeq) + " queries=" + std::to_string(g.queried) +
                       " replies=" + std::to_string(g.replied) + " seconds=" + std::to_string(g.seconds) + (moved ? " moved" : "") +
                       (found ? " state=" + std::to_string(best.state) + " game=" + best.game + " t=" + std::to_string(best.time) : ""));
            lk.lock();
            bool changed = false;
            if (s.refreshing > 0 && --s.refreshing == 0) changed = true;
            auto oit = s.owners.find(owner);
            if (oit != s.owners.end())
                if (Mate* mm = MateOf(oit->second, identity))
                    if (mm->key == key && g.reached) {
                        const double after = frn::Now();
                        // Read less than two and a half intervals ago and newer
                        // now: written in between, so the friend is there now
                        // whatever their clock says.
                        const bool live = after - mm->lastOkAt < 2.5 * tune.pollSeconds;
                        if (moved) mm->noteReadAt = 0; // the note says where the status went
                        if (found) {
                            mm->knownSeq = (std::max)(mm->knownSeq, bestSeq);
                            // Only ever forwards: an old status sent again changes nothing.
                            if (!mm->f.known || best.time > mm->f.st.time) {
                                // A message waits for us there: read at once.
                                if (!best.wake.empty() && !mm->shared.empty()) {
                                    const std::string tag = WakeTag(PairKeysOf(mm->shared, mm->identity), best.time);
                                    for (const auto& w : best.wake)
                                        if (frc::SameBytes(w, tag)) {
                                            mm->msgReadAt = 0;
                                            mm->msgRetries = 0;
                                            Log(s, "wake from=" + code);
                                        }
                                }
                                mm->f.seen = live ? UnixNow() : (std::min)(best.time, UnixNow());
                                mm->f.st = best;
                                mm->f.known = true;
                                changed = true;
                            }
                        }
                        mm->lastOkAt = after;
                    }
            if (changed) {
                Save(s);
                s.version++;
            }
            continue;
        }
        if (pick.kind == 2 || pick.kind == 3) {
            // A note to us: from a friend, or the answer to a request we sent.
            Card who;
            std::string shared;
            if (pick.kind == 2) {
                Mate& m = *MateOf(o, pick.who);
                m.noteReadAt = now;
                who.identity = m.identity;
                who.dh = m.dh;
                who.code = m.f.code;
                shared = m.shared;
            } else {
                for (Asking& a : o.sent)
                    if (a.r.who.identity == pick.who) {
                        who = a.r.who;
                        a.pollAt = now + AnswerGap(tune, static_cast<double>(unow - a.r.at));
                    }
                shared = SharedSecret(o.dhSecret, who.dh);
            }
            if (shared.empty()) continue;
            const PairKeys pk = PairKeysOf(shared, who.identity);
            lk.unlock();
            dht::GetResult g = get(pk.pub, kNoteSalt, -1, 20);
            Note best;
            bool found = false;
            int64_t bestSeq = -1;
            for (const auto& it : g.items) {
                Note n;
                if (OpenNote(pk, it.value, n) && it.seq > bestSeq) best = n, bestSeq = it.seq, found = true;
            }
            Log(s, std::string(pick.kind == 2 ? "note" : "answer") + " from=" + who.code + " reached=" + std::to_string(g.reached) +
                       " found=" + std::to_string(found) + (found ? " state=" + std::to_string(best.state) + " gen=" + std::to_string(best.gen) : "") +
                       " queries=" + std::to_string(g.queried) + " seconds=" + std::to_string(g.seconds));
            lk.lock();
            auto oit = s.owners.find(owner);
            if (oit == s.owners.end() || !found) continue;
            Owner& p = oit->second;
            Mate* mm = MateOf(p, who.identity);
            if (pick.kind == 3 && !mm) {
                bool waiting = false;
                for (const Asking& a : p.sent) waiting = waiting || a.r.who.identity == who.identity;
                if (!waiting || best.state != 1) continue;
                // Accepted. (Our note to them follows; theirs gives us their status.)
                MakeFriend(s, p, who);
                mm = MateOf(p, who.identity);
                Log(s, "accepted by=" + who.code);
            }
            if (!mm) continue;
            if (best.state == 0) {
                // Over, from their side: gone from ours too, and our status
                // moves where they cannot follow.
                p.mates.erase(std::remove_if(p.mates.begin(), p.mates.end(), [&](const Mate& x) { return x.identity == who.identity; }),
                              p.mates.end());
                Rotate(p);
                Log(s, "removed by=" + who.code);
            } else if (best.gen != mm->gen || best.key != mm->key || best.place != mm->place) {
                mm->gen = best.gen;
                mm->key = best.key;
                mm->place = best.place;
                mm->knownSeq = -1;
                mm->readAt = 0;
            }
            Save(s);
            s.version++;
            continue;
        }
        if (pick.kind == 4) {
            o.inboxAt[pick.salt] = now;
            o.inboxOnce.insert(pick.salt);
            const std::string code = CodeFor(o), idPub = o.idPub, dhSecret = o.dhSecret, dhPub = o.dhPub;
            const int64_t known = o.inboxSeq.count(pick.salt) ? o.inboxSeq[pick.salt] : -1;
            lk.unlock();
            const Keys k = keysOf(code);
            dht::GetResult g = get(k.pub, pick.salt, known, 20);
            std::vector<Envelope> got;
            int junk = 0;
            for (const auto& it : g.items) {
                Envelope e;
                // A request is believed only from an identity that makes the code it gives.
                if (OpenRequest(idPub, dhSecret, dhPub, it.value, e) && codeOf(e.from.identity) == e.from.code) got.push_back(e);
                else junk++;
            }
            Log(s, "inbox place=" + pick.salt + " reached=" + std::to_string(g.reached) + " values=" + std::to_string(g.items.size()) +
                       " requests=" + std::to_string(got.size()) + " other=" + std::to_string(junk) + " max_seq=" + std::to_string(g.maxSeq) +
                       " queries=" + std::to_string(g.queried) + " seconds=" + std::to_string(g.seconds));
            lk.lock();
            auto oit = s.owners.find(owner);
            if (oit == s.owners.end() || !g.reached) continue;
            Owner& p = oit->second;
            if (g.maxSeq > known && junk == 0) p.inboxSeq[pick.salt] = g.maxSeq;
            if (!g.items.empty() || g.maxSeq < 0) p.junk[pick.salt] = junk > 0 && got.empty();
            bool changed = false;
            for (const Envelope& e : got) {
                if (e.kind == 2) {
                    size_t before = p.received.size();
                    p.received.erase(std::remove_if(p.received.begin(), p.received.end(),
                                                    [&](const Request& r) { return r.who.identity == e.from.identity && e.time >= r.time; }),
                                     p.received.end());
                    changed = changed || before != p.received.size();
                    continue;
                }
                if (MateOf(p, e.from.identity)) continue;
                bool declined = false;
                for (const auto& d : p.declined) declined = declined || (d.first == e.from.identity && d.second >= e.time);
                if (declined) continue;
                bool asked = false;
                for (const Asking& a : p.sent) asked = asked || a.r.who.identity == e.from.identity;
                if (asked) {
                    // Both asked: as good as accepted on both sides.
                    MakeFriend(s, p, e.from);
                    Log(s, "both asked code=" + e.from.code);
                    changed = true;
                    continue;
                }
                bool known2 = false;
                for (Request& r : p.received)
                    if (r.who.identity == e.from.identity) {
                        known2 = true;
                        if (e.time > r.time) r.time = e.time, r.who = e.from, changed = true;
                    }
                if (!known2) {
                    Request r;
                    r.who = e.from;
                    r.time = e.time;
                    r.at = UnixNow();
                    p.received.push_back(r);
                    changed = true;
                    Log(s, "received from=" + e.from.code + " nick=" + e.from.nick);
                }
            }
            if (changed) Save(s);
            s.version++;
            continue;
        }
        if (pick.kind == 5) {
            // The own card: kept alive under the same number while it is the
            // newest there (nothing an outsider could see change), written
            // above anything else someone has put in its place.
            o.cardAt = now + tune.cardSeconds;
            const bool wasDirty = o.cardDirty;
            o.cardDirty = false;
            const std::string code = CodeFor(o), idSeed = o.idSeed, oldValue = o.cardValue;
            const Card me = CardOf(o);
            const int64_t oldSeq = o.cardSeq;
            lk.unlock();
            const Keys k = keysOf(code);
            const std::string value = SealCard(k, idSeed, me, true);
            dht::GetResult g = get(k.pub, kCardSalt, -1, 20);
            bool ours = false, foreign = false;
            int64_t oursSeq = -1;
            for (const auto& it : g.items) {
                if (it.value == value) ours = true, oursSeq = (std::max)(oursSeq, it.seq);
                else if (it.value != oldValue) foreign = foreign || it.seq >= oldSeq;
            }
            const bool newestOurs = ours && oursSeq >= g.maxSeq;
            bool ok = false;
            int64_t seq = 0;
            if (newestOurs) {
                seq = oursSeq;
                ok = put("card refresh", k.seed, kCardSalt, value, seq, 20, true);
            } else if (g.reached) {
                seq = (std::max)(oldSeq + (value == oldValue ? 0 : 1), g.maxSeq + 1);
                if (g.items.empty() && value == oldValue && g.maxSeq < 0) seq = (std::max<int64_t>)(oldSeq, 1);
                ok = put("card", k.seed, kCardSalt, value, seq, 20, value == oldValue && seq == oldSeq);
            }
            Log(s, "card own=" + std::to_string(ours) + " newest_ours=" + std::to_string(newestOurs) + " foreign=" + std::to_string(foreign) +
                       " max_seq=" + std::to_string(g.maxSeq) + " reached=" + std::to_string(g.reached));
            lk.lock();
            auto oit = s.owners.find(owner);
            if (oit == s.owners.end()) continue;
            Owner& p = oit->second;
            if (!g.reached || !ok) {
                p.cardAt = frn::Now() + (std::min)(60.0, tune.cardSeconds);
                p.cardDirty = p.cardDirty || wasDirty;
                continue;
            }
            if (p.cardTrouble != foreign) s.version++;
            p.cardTrouble = foreign;
            p.cardValue = value;
            p.cardSeq = (std::max)(seq, lastSeq[k.pub + kCardSalt]);
            p.cardOpen = true;
            Save(s);
            continue;
        }
    }
    net->Count(s.traffic);
    Log(s, "traffic gets=" + std::to_string(s.traffic.gets) + " puts=" + std::to_string(s.traffic.puts) + " packets_out=" +
               std::to_string(s.traffic.packetsOut) + " packets_in=" + std::to_string(s.traffic.packetsIn) + " bytes_out=" +
               std::to_string(s.traffic.bytesOut) + " bytes_in=" + std::to_string(s.traffic.bytesIn));
    net->Close();
    s.done = true;
    s.cv.notify_all();
}

// Starts the thread once there is something for it to do. Called with the lock.
void Kick(std::shared_ptr<Shared>& sp) {
    Shared& s = *sp;
    if (s.offline || !s.inited) return;
    if (!s.threadStarted) {
        bool need = s.look == Look::Busy;
        for (const auto& kv : s.owners) need = need || kv.second.sharing;
        if (!need) return;
        s.threadStarted = true;
        std::thread(Run, sp).detach();
    }
    s.cv.notify_all();
}

void Changed(std::shared_ptr<Shared>& sp) {
    Save(*sp);
    sp->version++;
    Kick(sp);
}

Owner* Find(Shared& s, const std::string& owner) {
    auto it = s.owners.find(owner);
    return it == s.owners.end() ? nullptr : &it->second;
}

} // namespace

void Init(const std::string& dataDir, bool offline) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    if (sp->inited) return;
    sp->inited = true;
    sp->offline = offline;
    sp->dir = dataDir;
    if (!offline) Load(*sp);
    Kick(sp);
}

void Shutdown() {
    auto sp = G();
    std::unique_lock<std::mutex> lk(sp->m);
    if (!sp->threadStarted || sp->done) return;
    // (Both under the lock, in this order: the thread clears the abort flag
    // again once it has seen `quit`.)
    sp->abortOp = true; // whatever is being read gives way to the goodbye
    sp->quit = true;
    sp->cv.notify_all();
    // The goodbye is allowed two and a half seconds; a thread that is still
    // busy after that (a name lookup without a network) is left behind.
    if (!sp->cv.wait_for(lk, std::chrono::milliseconds(3200), [&] { return sp->done.load(); })) {
        sp->abortOp = true;
        sp->cv.wait_for(lk, std::chrono::milliseconds(300), [&] { return sp->done.load(); });
    }
}

std::string OwnCode(const std::string& owner) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    const bool fresh = o.code.empty();
    std::string code = CodeFor(o);
    if (fresh) Save(*sp);
    return code;
}

bool Sharing(const std::string& owner) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    return o && o->sharing;
}

void SetSharing(const std::string& owner, bool on) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    if (o.sharing == on) return;
    o.sharing = on;
    o.dirty = true;
    o.dirtyAt = frn::Now();
    o.cardDirty = true;
    Changed(sp);
}

std::vector<Friend> List(const std::string& owner) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    std::vector<Friend> out;
    if (Owner* o = Find(*sp, owner))
        for (const Mate& m : o->mates) out.push_back(m.f);
    return out;
}

void Remove(const std::string& owner, const std::string& code) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    Mate* m = o ? MateByCode(*o, code) : nullptr;
    if (!m) return;
    Errand e;
    e.kind = 2;
    e.to.code = m->f.code;
    e.to.identity = m->identity;
    e.to.dh = m->dh;
    e.left = 3;
    o->errands.push_back(e);
    const std::string identity = m->identity;
    o->mates.erase(std::remove_if(o->mates.begin(), o->mates.end(), [&](const Mate& x) { return x.identity == identity; }), o->mates.end());
    Rotate(*o);
    Changed(sp);
}

void SetColour(const std::string& owner, int colour) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    if (o.colour == colour) return;
    o.colour = colour;
    o.cardDirty = true;
    o.dirty = true;
    o.dirtyAt = frn::Now() + 1.5;
    Save(*sp);
    sp->cv.notify_all();
}

int Colour(const std::string& owner, int fallback) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    return !o || o->colour == -2 ? fallback : o->colour;
}

void SetSelf(const std::string& owner, const Status& st) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    auto same = [](const Status& a, const Status& b) {
        if (a.nick != b.nick || a.state != b.state || a.game != b.game || a.gameId != b.gameId || a.total != b.total ||
            a.colour != b.colour || a.games.size() != b.games.size())
            return false;
        for (size_t i = 0; i < a.games.size(); i++)
            if (a.games[i].id != b.games[i].id || a.games[i].name != b.games[i].name || a.games[i].seconds != b.games[i].seconds)
                return false;
        return true;
    };
    if (o.selfSet && same(o.self, st)) return;
    if (!o.selfSet || o.self.nick != st.nick || o.self.colour != st.colour) o.cardDirty = true;
    o.selfSet = true;
    o.self = st;
    o.dirty = true;
    // A moment's patience: a change is often followed by another at once
    // (a game ends: the state and the play time).
    o.dirtyAt = frn::Now() + 1.5;
    Kick(sp);
}

void StartLookup(const std::string& code) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    sp->look = Look::Busy;
    sp->lookTaken = false;
    sp->lookCode = code;
    sp->lookReadyAt = frn::Now() + sp->testDelay;
    Kick(sp);
}

Look LookupState(Card* found) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    if (sp->offline && sp->look == Look::Busy && frn::Now() >= sp->lookReadyAt) {
        sp->look = sp->testLook;
        sp->lookResult = sp->testCard;
        sp->lookResult.code = sp->lookCode;
    }
    if (found && sp->look == Look::Found) *found = sp->lookResult;
    return sp->look;
}

void CancelLookup() {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    sp->look = Look::Idle;
}

Sent SendRequest(const std::string& owner, const Card& to) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    if (!ValidCode(to.code) || (!sp->offline && !CardOk(to)) || to.code == o.code) return Sent::Failed;
    for (const Mate& m : o.mates)
        if (m.f.code == to.code) return Sent::Friends;
    for (const Asking& a : o.sent)
        if (a.r.who.code == to.code) return Sent::Already;
    for (const Request& r : o.received)
        if (r.who.code == to.code && r.who.identity == to.identity) {
            MakeFriend(*sp, o, r.who);
            Changed(sp);
            return Sent::Accepted;
        }
    Asking a;
    a.r.who = to;
    a.r.time = a.r.at = UnixNow();
    o.sent.push_back(a);
    // A request sent again after being withdrawn is a new one.
    o.errands.erase(std::remove_if(o.errands.begin(), o.errands.end(), [&](const Errand& e) { return e.kind != 3 && e.to.identity == to.identity; }),
                    o.errands.end());
    Changed(sp);
    return Sent::Sent;
}

std::vector<Request> Received(const std::string& owner) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    return o ? o->received : std::vector<Request>();
}

std::vector<Request> Outgoing(const std::string& owner) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    std::vector<Request> out;
    if (Owner* o = Find(*sp, owner))
        for (const Asking& a : o->sent) out.push_back(a.r);
    return out;
}

bool Accept(const std::string& owner, const std::string& code) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    if (!o) return false;
    for (const Request& r : o->received)
        if (r.who.code == code) {
            const Card who = r.who;
            MakeFriend(*sp, *o, who);
            Changed(sp);
            return true;
        }
    return false;
}

void Decline(const std::string& owner, const std::string& code) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    if (!o) return;
    for (auto it = o->received.begin(); it != o->received.end(); ++it)
        if (it->who.code == code) {
            // Nothing is written: for the asker it stays waiting. Their copies
            // of this request (written again every half hour) are not shown again.
            o->declined.emplace_back(it->who.identity, it->time);
            if (o->declined.size() > 200) o->declined.erase(o->declined.begin());
            o->received.erase(it);
            Changed(sp);
            return;
        }
}

void Withdraw(const std::string& owner, const std::string& code) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    if (!o) return;
    for (auto it = o->sent.begin(); it != o->sent.end(); ++it)
        if (it->r.who.code == code) {
            const Card to = it->r.who;
            o->sent.erase(it);
            if (!sp->offline) {
                // A notice into their inbox takes the request off their list;
                // a note "over" covers an acceptance that crossed it on the way.
                Errand e;
                e.kind = 1;
                e.to = to;
                e.time = UnixNow();
                e.left = 2;
                o->errands.push_back(e);
                e.kind = 2;
                e.left = 1;
                o->errands.push_back(e);
            }
            Changed(sp);
            return;
        }
}

Trouble Troubles(const std::string& owner) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Trouble t;
    Owner* o = Find(*sp, owner);
    if (!o || !o->sharing) return t;
    t.card = o->cardTrouble;
    const int64_t day = DayOf(UnixNow());
    for (const auto& kv : o->junk)
        if (kv.second && kv.first.rfind("rq" + std::to_string(day) + ".", 0) == 0) t.inbox = true;
    return t;
}

bool SendMessage(const std::string& owner, const std::string& code, const std::string& body) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    Mate* m = o ? MateByCode(*o, code) : nullptr;
    if (!m || body.size() > kMaxMessage) return false;
    m->msgOut = body;
    m->msgDirty = true;
    m->msgState = MsgState::Writing;
    m->msgFails = 0;
    m->msgSerial++;
    m->msgWriteAt = 0;
    sp->version++;
    Kick(sp);
    return true;
}

MsgState MessageState(const std::string& owner, const std::string& code) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    Mate* m = o ? MateByCode(*o, code) : nullptr;
    return m ? m->msgState : MsgState::None;
}

std::vector<Message> Messages(const std::string& owner) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    std::vector<Message> out;
    if (Owner* o = Find(*sp, owner))
        for (const Mate& m : o->mates)
            if (m.msgInSeq >= 0) out.push_back({m.f.code, m.msgIn, m.msgInSeq, m.msgInAt});
    return out;
}

void Expect(const std::string& owner, const std::string& code, bool on) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    Mate* m = o ? MateByCode(*o, code) : nullptr;
    if (!m || m->expect == on) return;
    m->expect = on;
    m->expectAt = 0;
    Kick(sp);
}

void RefreshNow() {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    if (sp->offline) return;
    sp->refresh = true;
    Kick(sp);
}

void Watching(bool on) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    if (sp->watching == on) return;
    sp->watching = on;
    sp->cv.notify_all();
}

unsigned Version() {
    return G()->version.load();
}

bool Refreshing() {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    return sp->refreshing > 0 || sp->refresh;
}

void TestLookup(Look result, const Card& c, double seconds) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    sp->testLook = result;
    sp->testCard = c;
    sp->testDelay = seconds;
}

void TestAddFriend(const std::string& owner, const Friend& f) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    if (MateByCode(o, f.code)) return;
    Mate m;
    m.f = f;
    m.identity = frc::Sha256("test" + f.code);
    o.mates.push_back(m);
    sp->version++;
}

void TestAddReceived(const std::string& owner, const Request& r) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    for (const Request& q : o.received)
        if (q.who.code == r.who.code) return;
    o.received.push_back(r);
    sp->version++;
}

void TestAddSent(const std::string& owner, const Request& r) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    for (const Asking& q : o.sent)
        if (q.r.who.code == r.who.code) return;
    Asking a;
    a.r = r;
    o.sent.push_back(a);
    sp->version++;
}

void TestSetTrouble(const std::string& owner, const Trouble& t) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    o.cardTrouble = t.card;
    o.junk[InboxSalt(DayOf(UnixNow()), 0)] = t.inbox;
    sp->version++;
}

void SetTuning(const Tuning& t) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    sp->tune = t;
    sp->cv.notify_all();
}

void SetLog(std::function<void(const std::string&)> fn) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    sp->log = std::move(fn);
}

void UseMemoryNetwork() {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    sp->memory = true;
}

bool MemPut(const std::string& pub, const std::string& salt, const std::string& value, int64_t seq, const std::string& sig) {
    // As a BEP 44 node: a correctly signed value, and never under a lower number.
    if (value.size() > dht::kMaxValue || !frc::Ed25519Verify(pub, dht::SignBuffer(salt, seq, value), sig)) return false;
    std::lock_guard<std::mutex> lk(memM);
    MemItem& it = memStore[dht::Target(pub, salt)];
    if (!it.value.empty() && it.seq > seq) return false;
    if (!it.value.empty() && it.seq == seq) return true; // kept alive, not replaced
    it.value = value;
    it.seq = seq;
    return true;
}

bool MemGet(const std::string& pub, const std::string& salt, std::string& value, int64_t& seq) {
    std::lock_guard<std::mutex> lk(memM);
    auto it = memStore.find(dht::Target(pub, salt));
    if (it == memStore.end()) return false;
    value = it->second.value;
    seq = it->second.seq;
    return true;
}

bool TestPeek(const std::string& owner, const std::string& friendCode, std::string& key, std::string& place) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner* o = Find(*sp, owner);
    Mate* m = o ? MateByCode(*o, friendCode) : nullptr;
    if (!m || m->key.empty()) return false;
    key = m->key;
    place = m->place;
    return true;
}

void TestOwnPlace(const std::string& owner, std::string& key, std::string& place) {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    Owner& o = OwnerOf(*sp, owner);
    key = o.key;
    place = frc::Ed25519Public(o.seed);
}

Traffic Counters() {
    auto& sp = G();
    std::lock_guard<std::mutex> lk(sp->m);
    return sp->traffic;
}

} // namespace friends
