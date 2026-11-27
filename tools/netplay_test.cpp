// Checks playing together online (src\netplay.cpp) without any real network
// and without any real emulator:
//   - the invitation format: what is written, what is refused, its size;
//     the passphrase's form (RyuLDN's own pattern); expiry by both clocks;
//   - the friends message it travels in: sealed and signed with the pair's
//     keys, so a third person (even a friend of both) cannot read one or
//     write one that passes as the other's; the status' wake-up mark;
//   - the whole exchange between users of one friends module over the
//     network inside the process: invite -> answer (join / no), withdrawal,
//     an answer to a wrong invitation, replay of a handled invitation,
//     a timeout; and the time each step took;
//   - the emulator's settings: the four values written and put back on
//     copies of Ryubing's and Kenji-NX's Config.json, byte for byte, also
//     after a restart in between (a second process) and after the emulator
//     rewrote its file meanwhile; Ryujinx 1.1 refused.
//
//   netplay_test <work folder> <data folder>       (the data folder holds ryubing\, kenjinx\, ryujinx\ Config.json copies)
//   netplay_test --apply <dir> <Config.json> <emu>  (Apply and exit, as an NSW-Home that is closed during the game)
//   netplay_test --restore <dir>                    (Init and Restore, as the next start)
//
// Build: tools\build_netplay.bat
#define _CRT_SECURE_NO_WARNINGS // (fopen, getenv: a test, not the program)
#include "../src/fr_bencode.h"
#include "../src/fr_crypto.h"
#include "../src/fr_dht.h"
#include "../src/friends.h"
#include "../src/netplay.h"
#include "../src/ryucfg.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

namespace {

int passed = 0, failed = 0;

void Check(bool ok, const std::string& what) {
    if (ok) passed++;
    else {
        failed++;
        printf("  FAIL  %s\n", what.c_str());
    }
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool WaitFor(const std::function<bool()>& cond, double seconds) {
    const double end = Now() + seconds;
    while (Now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

bool ReadFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    fclose(f);
    return true;
}
bool WriteFile(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}
std::wstring W(const std::string& s) {
    return std::wstring(s.begin(), s.end()); // (ASCII paths only in this test)
}

netplay::Msg SampleInvite() {
    netplay::Msg m;
    m.kind = netplay::kInvite;
    m.nonce = netplay::NewNonce();
    m.time = friends::UnixNow();
    m.expires = m.time + netplay::kLifetime;
    m.titleId = "0100152000022000";
    m.game = "Mario Kart 8 Deluxe";
    m.gameVersion = "1.0.0";
    m.family = netplay::kFamilyRyuLdn;
    m.emulator = "ryubing";
    m.emuVersion = "1.3.3";
    m.passphrase = netplay::NewPassphrase();
    return m;
}

// ---- the format -------------------------------------------------------------------

void Format() {
    using namespace netplay;
    Msg m = SampleInvite(), back;
    const std::string body = Encode(m);
    Check(Decode(body, back) && back.kind == kInvite && back.nonce == m.nonce && back.time == m.time && back.expires == m.expires &&
              back.titleId == m.titleId && back.game == m.game && back.gameVersion == m.gameVersion && back.family == m.family &&
              back.emulator == m.emulator && back.emuVersion == m.emuVersion && back.passphrase == m.passphrase,
          "format: an invitation reads back as written");
    printf("  an invitation is %zu bytes (a message may be %zu; sealed %zu, a DHT value at most 1000)\n", body.size(), friends::kMaxMessage,
           body.size() + 29);
    Msg big = m;
    big.game = std::string(500, 'x');
    big.gameVersion = std::string(500, 'y');
    big.emulator = std::string(500, 'z');
    big.emuVersion = std::string(500, 'w');
    Check(Encode(big).size() <= friends::kMaxMessage, "format: long names are cut so that it always fits");

    Msg a;
    a.kind = kAnswer;
    a.nonce = m.nonce;
    a.time = m.time + 5;
    a.yes = true;
    a.emulator = "kenjinx";
    Check(Decode(Encode(a), back) && back.kind == kAnswer && back.yes && back.nonce == m.nonce && back.emulator == "kenjinx",
          "format: an answer reads back");
    a.yes = false;
    Check(Decode(Encode(a), back) && !back.yes, "format: a 'no' reads back");
    Msg c;
    c.kind = kCancel;
    c.nonce = m.nonce;
    c.time = m.time;
    Check(Decode(Encode(c), back) && back.kind == kCancel, "format: a withdrawal reads back");

    auto refused = [&](std::function<void(ben::Val&)> spoil, const char* what) {
        ben::Val d;
        ben::Decode(Encode(m), d);
        spoil(d);
        Msg x;
        Check(!Decode(ben::Encode(d), x), std::string("format: refused: ") + what);
    };
    refused([](ben::Val& d) { d.Set("v", 2); }, "another format version");
    refused([](ben::Val& d) { d.Set("k", 7); }, "an unknown kind");
    refused([](ben::Val& d) { d.Set("n", "short"); }, "a nonce that is not 16 bytes");
    refused([](ben::Val& d) { d.Set("p", "Ryujinx-ABCDEF12"); }, "a passphrase in capitals (RyuLDN would treat it as none)");
    refused([](ben::Val& d) { d.Set("p", ""); }, "no passphrase (that would be the public lobby)");
    refused([](ben::Val& d) { d.Set("p", "Ryujinx-1234567"); }, "a passphrase one digit short");
    refused([&](ben::Val& d) { d.Set("x", m.time + 7200); }, "a lifetime of two hours");
    refused([&](ben::Val& d) { d.Set("x", m.time); }, "a lifetime of nothing");
    refused([](ben::Val& d) { d.Set("g", "123"); }, "a title ID that is not 8 bytes");
    refused([](ben::Val& d) { d.Set("f", ""); }, "no family");
    Check(!Decode("garbage", back) && !Decode("", back), "format: not bencoding is refused");
    Msg noAns;
    {
        ben::Val d;
        ben::Decode(Encode(a), d);
        ben::Val e = ben::Val::MakeDict();
        for (auto& kv : d.dict)
            if (kv.first != "a") e.Set(kv.first, kv.second);
        Check(!Decode(ben::Encode(e), noAns), "format: refused: an answer without yes or no");
    }

    bool allOk = true;
    for (int i = 0; i < 2000; i++) {
        std::string p = NewPassphrase();
        allOk = allOk && ValidPassphrase(p) && p.size() == 16;
    }
    Check(allOk, "passphrase: 2000 made, all 'Ryujinx-' + 8 lowercase hex");
    Check(NewPassphrase() != NewPassphrase(), "passphrase: a new one each time");
    Check(ValidPassphrase("Ryujinx-0a1b2c3d") && !ValidPassphrase("ryujinx-0a1b2c3d") && !ValidPassphrase("Ryujinx-0a1b2c3g") &&
              !ValidPassphrase("Ryujinx-0a1b2c3d0"),
          "passphrase: the form is checked");

    // Expiry, by the writer's clock and by ours.
    const int64_t t = 1700000000;
    Msg inv = m;
    inv.time = t;
    inv.expires = t + kLifetime;
    Check(Fresh(inv, t + 5, t + 5) && SecondsLeft(inv, t + 5, t + 5) == kLifetime, "expiry: fresh when it arrives, ten minutes left");
    Check(Fresh(inv, t + 5, t + 300) && !Fresh(inv, t + 5, t + kLifetime + 6), "expiry: gone ten minutes after it arrived");
    Check(!Fresh(inv, t + 2000, t + 2000), "expiry: arriving after its end (by the writer's clock, beyond the skew) it is gone");
    Check(Fresh(inv, t - 100, t - 100), "expiry: a writer's clock 100 s ahead is tolerated");
    Check(!Fresh(inv, t - 500, t - 500), "expiry: a writer's clock 500 s ahead is not");
    Check(Fresh(inv, t + kLifetime + 60, t + kLifetime + 60) && SecondsLeft(inv, t + kLifetime + 60, t + kLifetime + 60) == 60,
          "expiry: a writer's clock 60 s behind leaves a minute");
}

// ---- the message's seal, at the format level ------------------------------------------

struct Person {
    std::string seed, pub, dhSecret, dh;
};
Person MakePerson(const std::string& name) {
    Person p;
    p.seed = frc::Sha256("identity " + name);
    p.pub = frc::Ed25519Public(p.seed);
    p.dhSecret = frc::Sha256("dh " + name);
    p.dh = frc::X25519Base(p.dhSecret);
    return p;
}

void Seal() {
    using namespace friends;
    Person A = MakePerson("A"), B = MakePerson("B"), C = MakePerson("C");
    const std::string ab = frc::X25519(A.dhSecret, B.dh), ba = frc::X25519(B.dhSecret, A.dh);
    Check(ab == ba, "seal: A and B work out the same secret");
    const PairKeys aToB = PairKeysOf(ab, A.pub);
    const std::string body = netplay::Encode(SampleInvite());
    const std::string value = SealMessage(aToB, body, frc::RandomBytes(12));
    std::string out;
    Check(OpenMessage(PairKeysOf(ba, A.pub), value, out) && out == body, "seal: B opens A's message");
    // C is a friend of both: it has a secret with A and one with B, never A and B's.
    const PairKeys cGuess1 = PairKeysOf(frc::X25519(C.dhSecret, A.dh), A.pub), cGuess2 = PairKeysOf(frc::X25519(C.dhSecret, B.dh), A.pub);
    Check(!OpenMessage(cGuess1, value, out) && !OpenMessage(cGuess2, value, out), "seal: a third person (a friend of both) cannot read it");
    Check(cGuess1.pub != aToB.pub && cGuess2.pub != aToB.pub, "seal: ... nor even work out where it is");
    std::string spoilt = value;
    spoilt[spoilt.size() / 2] ^= 1;
    Check(!OpenMessage(PairKeysOf(ba, A.pub), spoilt, out), "seal: one bit changed: refused");
    Note asNote;
    Check(!OpenNote(aToB, value, asNote), "seal: a message does not open as the note (keys of their own)");
    const PairKeys bToA = PairKeysOf(ab, B.pub);
    Check(!OpenMessage(bToA, value, out), "seal: B's own direction is another key");

    // Writing: the place's key comes from the pair's secret.
    MemPut(aToB.pub, kMsgSalt, value, 5, frc::Ed25519Sign(aToB.seed, aToB.pub, dht::SignBuffer(kMsgSalt, 5, value)));
    const std::string forged = SealMessage(aToB, "forged", frc::RandomBytes(12)); // (as if C had the seal key, which it does not)
    Check(!MemPut(aToB.pub, kMsgSalt, forged, 6, frc::Ed25519Sign(C.seed, C.pub, dht::SignBuffer(kMsgSalt, 6, forged))),
          "seal: a value at A's place for B signed by anyone else is not stored");
    std::string held;
    int64_t seq = 0;
    Check(MemGet(aToB.pub, kMsgSalt, held, seq) && held == value && seq == 5, "seal: the place still holds A's message");
    Check(!MemPut(aToB.pub, kMsgSalt, value, 4, frc::Ed25519Sign(aToB.seed, aToB.pub, dht::SignBuffer(kMsgSalt, 4, value))),
          "seal: an older number is not taken over a newer one");

    // The wake-up mark.
    const std::string w1 = WakeTag(aToB, 1000), w2 = WakeTag(aToB, 1001);
    Check(w1.size() == 6 && w1 != w2, "wake: six bytes, different at every status");
    Check(WakeTag(PairKeysOf(ba, A.pub), 1000) == w1, "wake: B recognises it");
    Check(WakeTag(cGuess1, 1000) != w1 && WakeTag(cGuess2, 1000) != w1, "wake: C cannot tell it is for B");
    Status s;
    s.nick = "A";
    s.time = 1000;
    s.wake = {w1, WakeTag(PairKeysOf(frc::X25519(A.dhSecret, C.dh), A.pub), 1000)};
    Status back;
    bool moved = false;
    const std::string key = frc::RandomBytes(32);
    Check(OpenStatus(key, SealStatus(key, s, false, frc::RandomBytes(12)), back, moved) && back.wake == s.wake, "wake: the marks travel in the status");
    s.wake.clear();
    Check(OpenStatus(key, SealStatus(key, s, false, frc::RandomBytes(12)), back, moved) && back.wake.empty(), "wake: a status without marks has none");
}

// ---- the exchange, over the network inside this process ----------------------------------

void Exchange() {
    using namespace friends;
    UseMemoryNetwork();
    Tuning t;
    t.pollSeconds = t.watchSeconds = 0.3;
    t.republishSeconds = 2;
    t.inboxSeconds = 0.4;
    t.answerSeconds = 0.2;
    t.noteSeconds = 1;
    t.noteWriteSeconds = 5;
    t.cardSeconds = 1;
    t.resendSeconds = 1;
    t.queriesPerSecond = 1000;
    t.messageSeconds = 0.2;
    t.wakeSeconds = 30;
    SetTuning(t);
    if (getenv("NETPLAY_TEST_LOG")) SetLog([](const std::string& l) { printf("    | %s\n", l.c_str()); });
    Init(".", false);
    netplay::Init(".", false);

    auto user = [](const std::string& id, const std::string& nick) {
        std::string code = OwnCode(id);
        Status st;
        st.nick = nick;
        SetSelf(id, st);
        SetSharing(id, true);
        return code;
    };
    auto look = [](const std::string& code) {
        Card c;
        for (int tries = 0; tries < 40; tries++) {
            StartLookup(code);
            Look l = Look::Busy;
            WaitFor([&] { return (l = LookupState(&c)) != Look::Busy; }, 20);
            CancelLookup();
            if (l == Look::Found) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return c;
    };
    auto befriend = [&](const std::string& x, const std::string& codeY, const std::string& y, const std::string& codeX) {
        SendRequest(x, look(codeY));
        WaitFor([&] {
            for (const auto& r : Received(y))
                if (r.who.code == codeX) return true;
            return false;
        }, 10);
        Accept(y, codeX);
        WaitFor([&] {
            for (const auto& f : List(x))
                if (f.code == codeY && f.known) return true;
            return false;
        }, 10);
    };
    const std::string a = user("A", "Prueba A"), b = user("B", "Prueba B"), c = user("C", "Prueba C");
    befriend("A", b, "B", a);
    befriend("A", c, "C", a);
    befriend("B", c, "C", b);
    Check(List("A").size() == 2 && List("B").size() == 2 && List("C").size() == 2, "exchange: A, B and C are friends of each other");
    Check(!SendMessage("A", "000000000000", "x") && !SendMessage("A", b, std::string(kMaxMessage + 1, 'x')),
          "exchange: no message to someone who is not a friend, none too long");

    // ---- A invites B (A uses the session; B answers by hand, as its NSW-Home would).
    netplay::Msg inv = SampleInvite();
    double t0 = Now();
    Check(netplay::Invite("A", b, inv) && netplay::Current().phase == netplay::Phase::Inviting && netplay::Current().host,
          "invite: A's session waits for an answer");
    netplay::Msg got;
    bool arrived = WaitFor([&] {
        netplay::Poll();
        for (const auto& m : Messages("B"))
            if (m.code == a && netplay::Decode(m.body, got) && got.kind == netplay::kInvite) return true;
        return false;
    }, 15);
    const double tInvite = Now() - t0;
    Check(arrived && got.nonce == netplay::Current().inv.nonce && got.passphrase == netplay::Current().inv.passphrase && got.titleId == inv.titleId,
          "invite: B reads it (game, passphrase)");
    Check(netplay::Current().sendState == "stored", "invite: A sees it stored");
    bool cSees = false;
    for (const auto& m : Messages("C")) cSees = cSees || m.code == a;
    Check(!cSees, "invite: C, a friend of both, gets nothing");
    // C tries to answer for B with a guess of its own: A reads only B's place.
    netplay::Msg fake;
    fake.kind = netplay::kAnswer;
    fake.nonce = got.nonce; // (even knowing the nonce, which C cannot)
    fake.time = UnixNow();
    fake.yes = true;
    SendMessage("C", a, netplay::Encode(fake));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    netplay::Poll();
    Check(netplay::Current().phase == netplay::Phase::Inviting, "invite: an answer from C (not the invited) changes nothing");
    // B answers for a wrong invitation first, then for the right one.
    netplay::Msg wrong = fake;
    wrong.nonce = netplay::NewNonce();
    SendMessage("B", a, netplay::Encode(wrong));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    netplay::Poll();
    Check(netplay::Current().phase == netplay::Phase::Inviting, "invite: an answer naming another invitation changes nothing");
    netplay::Msg yes = fake;
    yes.time = UnixNow();
    t0 = Now();
    SendMessage("B", a, netplay::Encode(yes));
    bool ready = WaitFor([&] {
        netplay::Poll();
        return netplay::Current().phase == netplay::Phase::Ready;
    }, 15);
    const double tAnswer = Now() - t0;
    Check(ready, "invite: B's 'join' reaches A: both agreed, A starts");
    netplay::Launched();
    Check(netplay::Current().phase == netplay::Phase::Playing, "invite: playing");
    netplay::Ended();
    Check(netplay::Current().phase == netplay::Phase::Idle, "invite: the game ended, the session is over");

    // ---- A invites B again; B says no.
    netplay::Invite("A", b, SampleInvite());
    WaitFor([&] {
        netplay::Poll();
        for (const auto& m : Messages("B"))
            if (m.code == a && netplay::Decode(m.body, got) && got.nonce == netplay::Current().inv.nonce) return true;
        return false;
    }, 15);
    netplay::Msg no = fake;
    no.nonce = got.nonce;
    no.yes = false;
    no.time = UnixNow();
    SendMessage("B", a, netplay::Encode(no));
    Check(WaitFor([&] {
        netplay::Poll();
        return netplay::Current().phase == netplay::Phase::Declined;
    }, 15), "decline: A sees the 'no'");
    netplay::Dismiss();

    // ---- A invites, then withdraws: B's list drops it.
    netplay::Invite("A", b, SampleInvite());
    const std::string withdrawnNonce = netplay::Current().inv.nonce;
    WaitFor([&] {
        netplay::Poll();
        for (const auto& m : Messages("B"))
            if (m.code == a && netplay::Decode(m.body, got) && got.nonce == withdrawnNonce && got.kind == netplay::kInvite) return true;
        return false;
    }, 15);
    netplay::Cancel();
    Check(WaitFor([&] {
        for (const auto& m : Messages("B"))
            if (m.code == a && netplay::Decode(m.body, got) && got.nonce == withdrawnNonce && got.kind == netplay::kCancel) return true;
        return false;
    }, 15), "withdraw: B reads the withdrawal");

    // ---- the other side: B invites A (by hand), A's session answers.
    netplay::Msg bInv = SampleInvite();
    t0 = Now();
    SendMessage("B", a, netplay::Encode(bInv));
    bool listed = WaitFor([&] {
        auto l = netplay::Invitations("A");
        return l.size() == 1 && l[0].code == b && l[0].inv.nonce == bInv.nonce;
    }, 15);
    const double tIncoming = Now() - t0;
    Check(listed, "guest: B's invitation is listed for A");
    netplay::Msg mine;
    mine.emulator = "kenjinx";
    mine.emuVersion = "2.0.5";
    mine.gameVersion = "1.0.0";
    Check(netplay::Answer("A", b, true, mine) && netplay::Current().phase == netplay::Phase::Joining && !netplay::Current().host,
          "guest: 'Unirse': joining");
    Check(netplay::Invitations("A").empty(), "guest: answered, it leaves the list");
    t0 = Now();
    Check(WaitFor([&] {
        netplay::Poll();
        return netplay::Current().phase == netplay::Phase::Ready;
    }, 15), "guest: the answer is stored: start");
    const double tJoin = Now() - t0;
    bool bGotYes = WaitFor([&] {
        for (const auto& m : Messages("B"))
            if (m.code == a && netplay::Decode(m.body, got) && got.kind == netplay::kAnswer && got.nonce == bInv.nonce) return got.yes;
        return false;
    }, 15);
    Check(bGotYes && got.emulator == "kenjinx" && got.emuVersion == "2.0.5", "guest: B reads the 'join' (and the emulator that answers)");
    netplay::Launched();
    netplay::Ended();

    // ---- replay: B's place still holds that invitation; written again (as a node could serve it), it stays handled.
    Check(netplay::Invitations("A").empty(), "replay: the answered invitation is not listed again");
    SendMessage("B", a, netplay::Encode(bInv)); // the same bytes again, under a newer number
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    Check(netplay::Invitations("A").empty(), "replay: the same invitation written again is not listed again");
    std::string dat;
    Check(ReadFile("netplay.dat", dat) && dat.find(bInv.nonce) != std::string::npos, "replay: handled invitations are kept on disk (a restart does not bring them back)");
    // An old one: written thirteen minutes ago (gone by the writer's clock, beyond any skew).
    netplay::Msg old = SampleInvite();
    old.time = UnixNow() - 800;
    old.expires = old.time + netplay::kLifetime;
    SendMessage("B", a, netplay::Encode(old));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    bool oldArrived = false;
    for (const auto& m : Messages("A")) oldArrived = oldArrived || (m.code == b && netplay::Decode(m.body, got) && got.nonce == old.nonce);
    Check(oldArrived && netplay::Invitations("A").empty(), "expiry: an invitation that ran out on the way is read but not listed");

    // ---- guest: B withdraws after A said "join" but before A's answer was stored is covered by
    // Withdrawn; here the plain case: B invites, withdraws, A's list drops it.
    netplay::Msg bInv2 = SampleInvite();
    SendMessage("B", a, netplay::Encode(bInv2));
    WaitFor([&] { return netplay::Invitations("A").size() == 1; }, 15);
    netplay::Msg bCancel;
    bCancel.kind = netplay::kCancel;
    bCancel.nonce = bInv2.nonce;
    bCancel.time = UnixNow();
    SendMessage("B", a, netplay::Encode(bCancel));
    Check(WaitFor([&] { return netplay::Invitations("A").empty(); }, 15), "withdraw: A's list drops B's withdrawn invitation");

    // ---- timeout: an invitation of A's that nobody answers.
    netplay::Invite("A", c, SampleInvite());
    netplay::Session s = netplay::Current();
    s.inv.expires = UnixNow() - 1; // (as if ten minutes had gone by)
    netplay::TestSetSession(s);
    netplay::Poll();
    Check(netplay::Current().phase == netplay::Phase::NoAnswer, "timeout: no answer in time: the session says so");
    Check(WaitFor([&] {
        for (const auto& m : Messages("C"))
            if (m.code == a && netplay::Decode(m.body, got) && got.nonce == s.inv.nonce && got.kind == netplay::kCancel) return true;
        return false;
    }, 15), "timeout: C is told it is withdrawn");
    netplay::Dismiss();
    Check(netplay::Current().phase == netplay::Phase::Idle, "timeout: dismissed");

    printf("  in-process network (status read every %.1f s, an awaited answer every %.1f s):\n", t.pollSeconds, t.messageSeconds);
    printf("    invitation A -> B readable at B after %.2f s (woken by A's status)\n", tInvite);
    printf("    answer B -> A seen by A after %.2f s\n", tAnswer);
    printf("    invitation B -> A listed at A after %.2f s; A's 'join' stored after %.2f s\n", tIncoming, tJoin);
    Traffic tr = Counters();
    printf("    %llu reads, %llu writes in all\n", static_cast<unsigned long long>(tr.gets), static_cast<unsigned long long>(tr.puts));
    Shutdown();
}

// ---- the emulator's settings -----------------------------------------------------------------

void Config(const std::string& work, const std::string& data, const std::string& self) {
    using namespace netplay;
    for (const char* emu : {"ryubing", "kenjinx"}) {
        const std::string src = data + "\\" + emu + "\\Config.json", dir = work + "\\cfg_" + emu, path = dir + "\\Config.json";
        system(("mkdir \"" + dir + "\" 2>nul").c_str());
        remove((path + ".dockside-backup").c_str());
        std::string orig;
        Check(ReadFile(src, orig) && WriteFile(path, orig), std::string(emu) + ": copy made");
        Support s = CanPlay(emu, W(emu), W(path));
        Check(s.ok && s.family == kFamilyRyuLdn, std::string(emu) + ": can play online");
        ryucfg::Open(W(path));
        printf("  %s (config version %d) before: multiplayer_mode=%s multiplayer_ldn_passphrase=\"%s\" ldn_server=\"%s\" multiplayer_disable_p2p=%s\n",
               emu, ryucfg::Version(), ryucfg::Get("multiplayer_mode").c_str(), ryucfg::Get("multiplayer_ldn_passphrase").c_str(),
               ryucfg::Get("ldn_server").c_str(), ryucfg::Get("multiplayer_disable_p2p").c_str());
        const std::string pass = NewPassphrase();
        std::string err;
        Check(Apply(W(path), emu, pass, err) && RestorePending(), std::string(emu) + ": session values written");
        ryucfg::Open(W(path));
        Check(ryucfg::Get("multiplayer_mode") == "1" && ryucfg::Get("multiplayer_ldn_passphrase") == pass && ryucfg::Get("ldn_server").empty() &&
                  ryucfg::Get("multiplayer_disable_p2p") == "true",
              std::string(emu) + ": reads back mode 1 (LdnRyu), the passphrase, the default server, no hosting");
        std::string during;
        ReadFile(path, during);
        // Everything but the four values is the same bytes: the text with the
        // values put back by hand is the original.
        std::string undo = during;
        auto put = [&](const std::string& key, const std::string& from, const std::string& to) {
            size_t at = undo.find("\"" + key + "\": " + from);
            if (at != std::string::npos) undo.replace(at + key.size() + 4, from.size(), to);
        };
        put("multiplayer_mode", "1", "0");
        put("multiplayer_ldn_passphrase", "\"" + pass + "\"", "\"\"");
        put("multiplayer_disable_p2p", "true", "false");
        Check(undo == orig, std::string(emu) + ": every other byte untouched while the session is on");
        std::string backup;
        Check(ReadFile(path + ".dockside-backup", backup) && backup == orig, std::string(emu) + ": one-time backup is the file as it was");
        Check(Restore(&err) && !RestorePending(), std::string(emu) + ": put back");
        std::string after;
        Check(ReadFile(path, after) && after == orig, std::string(emu) + ": byte-identical to before (" + std::to_string(orig.size()) + " bytes)");

        // An NSW-Home closed during the game: the next start puts it back.
        Check(system(("\"\"" + self + "\" --apply \"" + work + "\" \"" + path + "\" " + emu + "\"").c_str()) == 0, std::string(emu) + ": a second process applies and quits");
        ReadFile(path, during);
        Check(during != orig, std::string(emu) + ": ... the file holds the session");
        Check(system(("\"\"" + self + "\" --restore \"" + work + "\"\"").c_str()) == 0, std::string(emu) + ": a third process (the next start) restores");
        Check(ReadFile(path, after) && after == orig, std::string(emu) + ": byte-identical again after a restart in between");

        // The emulator rewrote its file during the game (it does when it closes):
        // the four values come back, its other changes stay.
        Check(Apply(W(path), emu, NewPassphrase(), err), std::string(emu) + ": applied again");
        ReadFile(path, during);
        // (Line ends as the file's own: the copies are written with CRLF or LF.)
        const std::string eol = during.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        const std::string marker = "  \"nswhome_test_marker\": 1," + eol;
        std::string rewritten = during;
        rewritten.insert(rewritten.find('\n') + 1, marker);
        WriteFile(path, rewritten);
        Check(Restore(&err), std::string(emu) + ": put back over the emulator's own rewrite");
        ReadFile(path, after);
        std::string expect = orig;
        expect.insert(expect.find('\n') + 1, marker);
        Check(after == expect, std::string(emu) + ": the four values as before, the emulator's own change kept");
        WriteFile(path, orig);
    }
    // The original Ryujinx: its multiplayer_mode 1 is ldn_mitm (same network only).
    {
        const std::string src = data + "\\ryujinx\\Config.json", dir = work + "\\cfg_ryujinx", path = dir + "\\Config.json";
        system(("mkdir \"" + dir + "\" 2>nul").c_str());
        std::string orig, after;
        ReadFile(src, orig);
        WriteFile(path, orig);
        Support s = CanPlay("ryujinx", L"Ryujinx", W(path));
        Check(!s.ok && !s.why.empty(), "ryujinx: refused, with a reason");
        std::string err;
        Check(!Apply(W(path), "ryujinx", NewPassphrase(), err) && ReadFile(path, after) && after == orig, "ryujinx: nothing written");
        Check(!CanPlay("eden", L"Eden", L"").ok && !CanPlay("", L"", L"").ok, "the yuzu family and no emulator: refused");
        // A fork build too old to have the keys (Ryujinx's file under a fork's name).
        Check(!CanPlay("ryubing", L"Ryubing", W(path)).ok, "a fork whose file lacks the online keys: refused");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "--apply") {
        netplay::Init(argv[2], false);
        std::string err;
        return netplay::Apply(W(argv[3]), argv[4], netplay::NewPassphrase(), err) ? 0 : 1;
    }
    if (argc >= 3 && std::string(argv[1]) == "--restore") {
        netplay::Init(argv[2], false);
        if (!netplay::RestorePending()) return 2;
        return netplay::Restore() ? 0 : 1;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: netplay_test <work folder> <data folder>\n");
        return 2;
    }
    const std::string work = argv[1], data = argv[2];
    // (Its own path, whole: the folder changes below.)
    char full[1024];
    const std::string self = _fullpath(full, argv[0], sizeof full) ? full : argv[0];
    if (chdir(work.c_str()) != 0) {
        fprintf(stderr, "no folder %s\n", work.c_str());
        return 2;
    }
    remove("friends.dat");
    remove("netplay.dat");
    netplay::Init(".", false);
    Format();
    Seal();
    Config(work, data, self);
    Exchange();
    remove("friends.dat");
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
