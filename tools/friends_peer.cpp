// One headless NSW-Home "user" for testing friends over the real network:
// the same module the menu uses (src\friends.cpp), with no window and a data
// folder of its own. tools\friends_net.py starts several of these and reads
// what they print.
//
//   friends_peer <data folder> <nickname> [options]
//     --request <code>    look this code up and send it a friend request (looked up again until found)
//     --accept            accept every request that arrives
//     --decline           decline every request that arrives
//     --withdraw-at <s>   withdraw every request still waiting at this time
//     --remove-at <s>     remove every friend at this time
//     --rounds <n>        change state n times (menu <-> playing "Ronda k"), one per period
//     --period <s>        seconds between changes (default 45)
//     --first <s>         seconds before the first change (default 20)
//     --duration <s>      total run time (default: until the rounds are done, plus 30 s)
//     --poll <s>          how often friends' statuses are read (default 60, the menu's own)
//     --inbox <s>         how often each inbox place is read (default 120)
//     --answer <s>        how often a request's answer is looked for at first (default 60)
//     --card <s>          how often the own card is looked at (default 900)
//     --republish <s>     how often the own status is written again (default 600)
//     --rate <n>          queries a second at most (default 8)
//     --noshare           never turn sharing on (to check that nothing is sent)
//     --invite-at <s>     playing together (netplay.h): at this time, invite every friend
//                         there is to a made-up game (one invitation at a time; again
//                         after each outcome, --invites times in all)
//     --invites <n>       how many invitations to send in all (default 1)
//     --join              answer "join" to every invitation that arrives
//     --refuse            answer "no" to every invitation that arrives
//     --message <s>       how often an awaited answer is read (default 4, the menu's own)
//
//   friends_peer --stranger <code> [--overwrite]
//     Someone who has the code and nothing else, with the DHT directly: reads
//     the card, tries to read every inbox place and to find a status, and with
//     --overwrite writes a card of its own over the code's and something that
//     is not a request into today's first inbox place. Prints what it got.
//
// Every line starts with milliseconds of Unix time. NET lines come from the
// module (one per network operation); START gives the own code; SELF is this
// peer changing state; SEEN is a friend's status as the pages would show it;
// RECEIVED / SENT / ACCEPTED / DECLINED / WITHDRAWN / REMOVED / GONE follow
// the requests and friendships. INVITE / INVITE_STORED / INVITED (received) /
// ANSWERED / READY / DECLINED / NOANSWER / UNSENT follow the invitations.
#include "../src/fr_bencode.h"
#include "../src/fr_crypto.h"
#include "../src/fr_dht.h"
#include "../src/friends.h"
#include "../src/netplay.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::mutex outMutex;
void Say(const std::string& line) {
    std::lock_guard<std::mutex> lk(outMutex);
    printf("%lld %s\n", NowMs(), line.c_str());
    fflush(stdout);
}

void Sleep(double s) {
    std::this_thread::sleep_for(std::chrono::duration<double>(s));
}

int Peer(int argc, char** argv);

} // namespace

#ifdef _WIN32
// The module takes its folder as UTF-8, as the menu gives it; a Windows
// program's plain arguments come in the system's own code page instead.
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
        std::string s;
        for (const wchar_t* p = wargv[i]; *p; p++) {
            unsigned c = static_cast<unsigned>(*p);
            if (c >= 0xD800 && c < 0xDC00 && p[1]) c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<unsigned>(*++p) - 0xDC00);
            if (c < 0x80) s += static_cast<char>(c);
            else if (c < 0x800) s += static_cast<char>(0xC0 | (c >> 6)), s += static_cast<char>(0x80 | (c & 63));
            else if (c < 0x10000)
                s += static_cast<char>(0xE0 | (c >> 12)), s += static_cast<char>(0x80 | ((c >> 6) & 63)), s += static_cast<char>(0x80 | (c & 63));
            else
                s += static_cast<char>(0xF0 | (c >> 18)), s += static_cast<char>(0x80 | ((c >> 12) & 63)),
                    s += static_cast<char>(0x80 | ((c >> 6) & 63)), s += static_cast<char>(0x80 | (c & 63));
        }
        args.push_back(s);
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    return Peer(argc, argv.data());
}
#else
int main(int argc, char** argv) {
    return Peer(argc, argv);
}
#endif

namespace {

// ---- the stranger ---------------------------------------------------------------

int Stranger(const std::string& code, bool overwrite) {
    std::atomic<bool> stop{false};
    dht::Client c(&stop);
    const friends::Keys k = friends::DeriveKeys(code);
    Say("STRANGER code=" + code);
    dht::GetResult g = c.Get(k.pub, friends::kCardSalt, -1, 20);
    int64_t cardSeq = g.maxSeq;
    for (const auto& it : g.items) {
        friends::Card card;
        bool open = false;
        bool ok = friends::OpenCard(k, code, it.value, card, open);
        Say("CARD seq=" + std::to_string(it.seq) + " valid=" + std::to_string(ok) + " open=" + std::to_string(open) + " nick=" + card.nick +
            " colour=" + std::to_string(card.colour) + " (a card carries no game, play time or presence)");
    }
    if (g.items.empty()) Say("CARD none reached=" + std::to_string(g.reached));
    // Everything the code leads to, tried with every key the code gives.
    const std::string mySeed = frc::Sha256("stranger " + code), myDhSecret = frc::Sha256("stranger dh " + code);
    const std::string myPub = frc::Ed25519Public(mySeed), myDh = frc::X25519Base(myDhSecret);
    const int64_t day = friends::DayOf(friends::UnixNow());
    int values = 0, opened = 0;
    for (int64_t d = day - 1; d <= day; d++)
        for (int i = 0; i < friends::kInboxSlots; i++) {
            dht::GetResult r = c.Get(k.pub, friends::InboxSalt(d, i), -1, 15);
            for (const auto& it : r.items) {
                values++;
                friends::Status st;
                bool moved = false;
                friends::Envelope e;
                if (friends::OpenStatus(k.seal, it.value, st, moved) || friends::OpenStatus(k.seed, it.value, st, moved) ||
                    friends::OpenRequest(myPub, myDhSecret, myDh, it.value, e))
                    opened++;
            }
        }
    Say("INBOX values=" + std::to_string(values) + " opened=" + std::to_string(opened));
    // A status under the code itself, where version 1 kept it.
    dht::GetResult st = c.Get(k.pub, "status", -1, 15);
    Say("STATUS_AT_CODE values=" + std::to_string(st.items.size()) + " (version 1's place; version 2 keeps none there)");
    if (overwrite) {
        friends::Card fake;
        fake.code = code;
        fake.nick = "Impostor";
        fake.colour = 0;
        fake.identity = myPub;
        fake.dh = myDh;
        const std::string value = friends::SealCard(k, mySeed, fake, true);
        const int64_t seq = (std::max<int64_t>)(cardSeq, 0) + 1000;
        dht::PutResult p = c.Put(k.pub, friends::kCardSalt, value, seq, frc::Ed25519Sign(k.seed, k.pub, dht::SignBuffer(friends::kCardSalt, seq, value)), 20);
        Say("OVERWROTE card seq=" + std::to_string(seq) + " stored=" + std::to_string(p.stored) + "/" + std::to_string(p.sent));
        const std::string salt = friends::InboxSalt(day, 0), junk = std::string(1, 2) + frc::RandomBytes(150);
        const int64_t jseq = friends::UnixNow() + 1000;
        p = c.Put(k.pub, salt, junk, jseq, frc::Ed25519Sign(k.seed, k.pub, dht::SignBuffer(salt, jseq, junk)), 20);
        Say("FILLED inbox place=" + salt + " stored=" + std::to_string(p.stored) + "/" + std::to_string(p.sent));
    }
    Say("traffic packets_out=" + std::to_string(c.packetsOut) + " packets_in=" + std::to_string(c.packetsIn));
    c.Close();
    return 0;
}

std::string Line(const friends::Friend& f) {
    return f.code + " nick=" + f.st.nick + " known=" + (f.known ? "1" : "0") + " state=" + std::to_string(f.st.state) + " game=" + f.st.game +
           " round=" + std::to_string(f.st.total >= 3600 ? f.st.total - 3600 : 0) +
           " online=" + (friends::Online(f, friends::UnixNow()) ? "1" : "0");
}

int Peer(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--stranger")
        return Stranger(argv[2], argc >= 4 && std::string(argv[3]) == "--overwrite");
    if (argc < 3) {
        fprintf(stderr, "usage: friends_peer <data folder> <nickname> [options] | friends_peer --stranger <code> [--overwrite]\n");
        return 2;
    }
    const std::string dir = argv[1], nick = argv[2], owner = "00000000000000000000000000000001";
    std::vector<std::string> wanted;
    int rounds = 0;
    double period = 45, first = 20, duration = -1, withdrawAt = -1, removeAt = -1;
    bool share = true, accept = false, decline = false, join = false, refuse = false;
    double inviteAt = -1;
    int invites = 1;
    friends::Tuning tune;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--request") wanted.push_back(next());
        else if (a == "--accept") accept = true;
        else if (a == "--decline") decline = true;
        else if (a == "--withdraw-at") withdrawAt = atof(next().c_str());
        else if (a == "--remove-at") removeAt = atof(next().c_str());
        else if (a == "--rounds") rounds = atoi(next().c_str());
        else if (a == "--period") period = atof(next().c_str());
        else if (a == "--first") first = atof(next().c_str());
        else if (a == "--duration") duration = atof(next().c_str());
        else if (a == "--poll") tune.pollSeconds = tune.watchSeconds = atof(next().c_str());
        else if (a == "--inbox") tune.inboxSeconds = atof(next().c_str());
        else if (a == "--answer") tune.answerSeconds = atof(next().c_str());
        else if (a == "--card") tune.cardSeconds = atof(next().c_str());
        else if (a == "--republish") tune.republishSeconds = atof(next().c_str());
        else if (a == "--rate") tune.queriesPerSecond = atof(next().c_str());
        else if (a == "--noshare") share = false;
        else if (a == "--invite-at") inviteAt = atof(next().c_str());
        else if (a == "--invites") invites = atoi(next().c_str());
        else if (a == "--join") join = true;
        else if (a == "--refuse") refuse = true;
        else if (a == "--message") tune.messageSeconds = atof(next().c_str());
    }
    if (duration < 0) duration = first + rounds * period + 30;

    friends::SetLog([](const std::string& line) { Say("NET " + line); });
    friends::SetTuning(tune);
    friends::Init(dir, false);
    netplay::Init(dir, false);
    const std::string code = friends::OwnCode(owner);
    friends::Status st;
    st.nick = nick;
    st.colour = 2;
    st.total = 3600;
    st.games = {{"0100000000001000", "Juego de prueba", 3000}, {"", "Otro juego de prueba", 600}};
    friends::SetSelf(owner, st);
    if (share) friends::SetSharing(owner, true);
    Say("START nick=" + nick + " code=" + code + " sharing=" + (share ? "1" : "0"));

    const double start = static_cast<double>(NowMs()) / 1000;
    int done = 0;
    bool looking = false, withdrawn = false, removed = false;
    double lookAgainAt = 0;
    std::vector<std::string> shown;
    std::set<std::string> received, sent, friendsNow;
    for (;;) {
        double t = static_cast<double>(NowMs()) / 1000 - start;
        if (t > duration) break;
        // Requests, one lookup at a time, as "Buscar por clave de amigo" does.
        if (share && !wanted.empty() && !looking && t >= lookAgainAt) {
            friends::StartLookup(wanted.front());
            looking = true;
        }
        if (looking) {
            friends::Card c;
            friends::Look l = friends::LookupState(&c);
            if (l == friends::Look::Found) {
                friends::Sent r = friends::SendRequest(owner, c);
                Say("SENT " + c.code + " nick=" + c.nick + " result=" + std::to_string(static_cast<int>(r)));
                wanted.erase(wanted.begin());
                looking = false;
                friends::CancelLookup();
            } else if (l == friends::Look::NotFound || l == friends::Look::NoNetwork || l == friends::Look::Unverified) {
                Say("LOOKUP " + std::to_string(static_cast<int>(l)) + " " + wanted.front());
                looking = false;
                lookAgainAt = t + 8;
                friends::CancelLookup();
            }
        }
        for (const auto& r : friends::Received(owner)) {
            if (received.insert(r.who.code).second) Say("RECEIVED " + r.who.code + " nick=" + r.who.nick);
            if (accept) {
                friends::Accept(owner, r.who.code);
                Say("ACCEPTED " + r.who.code);
            } else if (decline) {
                friends::Decline(owner, r.who.code);
                Say("DECLINED " + r.who.code);
            }
        }
        std::set<std::string> recvNow;
        for (const auto& r : friends::Received(owner)) recvNow.insert(r.who.code);
        for (auto it = received.begin(); it != received.end();) {
            if (!recvNow.count(*it) && !accept && !decline) {
                Say("RECEIVED_GONE " + *it);
                it = received.erase(it);
            } else {
                ++it;
            }
        }
        std::set<std::string> out;
        for (const auto& r : friends::Outgoing(owner)) out.insert(r.who.code);
        for (const auto& c : sent)
            if (!out.count(c)) Say("SENT_GONE " + c);
        sent = out;
        if (withdrawAt >= 0 && t >= withdrawAt && !withdrawn) {
            withdrawn = true;
            for (const auto& c : out) {
                friends::Withdraw(owner, c);
                Say("WITHDRAWN " + c);
            }
        }
        if (done < rounds && t >= first + done * period) {
            done++;
            st.state = done % 2 ? friends::Playing : friends::InMenu;
            st.game = done % 2 ? "Ronda " + std::to_string(done) : "";
            st.gameId = done % 2 ? "0100000000001000" : "";
            st.total = 3600 + static_cast<uint64_t>(done);
            friends::SetSelf(owner, st);
            Say("SELF round=" + std::to_string(done) + " state=" + std::to_string(st.state) + " game=" + st.game);
        }
        std::vector<friends::Friend> list = friends::List(owner);
        if (removeAt >= 0 && t >= removeAt && !removed) {
            removed = true;
            for (const auto& f : list) {
                friends::Remove(owner, f.code);
                Say("REMOVED " + f.code);
            }
            list = friends::List(owner);
        }
        std::set<std::string> nowCodes;
        for (const auto& f : list) {
            nowCodes.insert(f.code);
            if (friendsNow.insert(f.code).second) Say("FRIEND " + f.code + " nick=" + f.st.nick);
        }
        for (auto it = friendsNow.begin(); it != friendsNow.end();) {
            if (!nowCodes.count(*it)) {
                Say("GONE " + *it);
                it = friendsNow.erase(it);
            } else {
                ++it;
            }
        }
        shown.resize(list.size());
        for (size_t i = 0; i < list.size(); i++) {
            std::string line = Line(list[i]);
            if (line != shown[i]) {
                shown[i] = line;
                Say("SEEN " + line);
            }
        }
        static bool cardTrouble = false, inboxTrouble = false;
        friends::Trouble tr = friends::Troubles(owner);
        if (tr.card != cardTrouble || tr.inbox != inboxTrouble) {
            cardTrouble = tr.card;
            inboxTrouble = tr.inbox;
            Say("TROUBLE card=" + std::to_string(tr.card) + " inbox=" + std::to_string(tr.inbox));
        }
        // Playing together: invitations out, answers in.
        static int invited = 0;
        static bool stored = false;
        static double nextInviteAt = -1;
        if (nextInviteAt < 0) nextInviteAt = inviteAt;
        static std::set<std::string> answered;
        const netplay::Session& ses = netplay::Current();
        // (Only once the friend's status has been read: the invitation's delivery is what is measured, not the friendship's.)
        if (inviteAt >= 0 && t >= nextInviteAt && invited < invites && ses.phase == netplay::Phase::Idle && !list.empty() && list[0].known) {
            netplay::Msg m;
            m.titleId = "0100000000001000";
            m.game = "Juego de prueba";
            m.gameVersion = "1.0.0";
            m.emulator = "ryubing";
            m.emuVersion = "1.3.3";
            invited++;
            stored = false;
            const bool ok = netplay::Invite(owner, list[0].code, m);
            Say("INVITE " + list[0].code + " n=" + std::to_string(invited) + " ok=" + std::to_string(ok) + " nonce=" +
                frc::Hex(netplay::Current().inv.nonce).substr(0, 8));
        }
        if (netplay::Poll() || (ses.phase == netplay::Phase::Inviting && !stored && ses.sendState == "stored")) {
            if (ses.phase == netplay::Phase::Inviting && ses.sendState == "stored" && !stored) {
                stored = true;
                Say("INVITE_STORED " + ses.code);
            }
            const char* what = ses.phase == netplay::Phase::Ready     ? "READY"
                               : ses.phase == netplay::Phase::Declined ? "DECLINED"
                               : ses.phase == netplay::Phase::NoAnswer ? "NOANSWER"
                               : ses.phase == netplay::Phase::Unsent   ? "UNSENT"
                               : ses.phase == netplay::Phase::Withdrawn ? "WITHDRAWN"
                                                                        : nullptr;
            if (what) {
                Say(std::string(what) + " " + ses.code + " host=" + std::to_string(ses.host) + " passphrase=" + ses.inv.passphrase +
                    " nonce=" + frc::Hex(ses.inv.nonce).substr(0, 8));
                nextInviteAt = t + 60; // (the next one a minute after this outcome)
                if (ses.phase == netplay::Phase::Ready) {
                    netplay::Launched();
                    netplay::Ended(); // (no game here: the session ends at once)
                } else {
                    netplay::Dismiss();
                }
            }
        }
        for (const auto& in : netplay::Invitations(owner)) {
            if (!answered.insert(in.inv.nonce).second) continue;
            Say("INVITED " + in.code + " game=" + in.inv.game + " passphrase=" + in.inv.passphrase + " nonce=" + frc::Hex(in.inv.nonce).substr(0, 8) +
                " left=" + std::to_string(netplay::SecondsLeft(in.inv, in.at, friends::UnixNow())));
            if (join || refuse) {
                netplay::Msg mine;
                mine.emulator = "kenjinx";
                mine.emuVersion = "2.0.5";
                const bool ok = netplay::Answer(owner, in.code, join, mine);
                Say(std::string("ANSWERED ") + in.code + " join=" + (join ? "1" : "0") + " ok=" + std::to_string(ok));
            }
        }
        Sleep(0.05);
    }
    Say("STOP");
    friends::Shutdown();
    friends::Traffic tr = friends::Counters();
    Say("END gets=" + std::to_string(tr.gets) + " puts=" + std::to_string(tr.puts) + " packets_out=" + std::to_string(tr.packetsOut) +
        " bytes_out=" + std::to_string(tr.bytesOut) + " packets_in=" + std::to_string(tr.packetsIn) + " bytes_in=" + std::to_string(tr.bytesIn));
    return 0;
}

} // namespace
