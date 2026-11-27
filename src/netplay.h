// Playing together online: a friend is invited to a game, answers, and both
// NSW-Home copies start that game with their emulator set up for one private
// session of the Ryujinx family's own online play (RyuLDN: the project's
// public server, ldn.ryujinx.app, TCP 30456, which relays the game's local
// wireless traffic, so it works behind any router).
//
// What travels: invitations and answers are messages between two friends
// (friends.h: SendMessage / Messages), sealed with the keys only those two
// hold. Nothing here talks to the network itself.
//
// What it changes: right before the game starts, four values of the
// emulator's Config.json (ryucfg: value by value, every other byte left as it
// is); when the game has ended, the values that were there before are put
// back. What was there is kept in <data>\netplay.dat until it has been put
// back, so an NSW-Home that was closed in between restores it at its next start.
//
// Stands alone (no app.h), so the tests can link it with friends and ryucfg.
// Used from the menu's thread only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace netplay {

// ---- the message format (the body of a friends message) --------------------
//
// A bencoded dictionary (BEP 3), at most friends::kMaxMessage bytes:
//   v  1                       format version
//   k  1 invite, 2 answer, 3 cancel
//   n  16 random bytes          names the invitation; the answer and a cancel repeat it
//   t  Unix seconds             when it was written (the writer's clock)
// an invitation also has
//   x  Unix seconds             when it expires (t + 600)
//   g  8 bytes                  the game's title ID
//   m  text                     the game's name, to show
//   gv text                     the game's version as the writer's library reads it
//   f  "ryuldn"                 the online-play family (both must have the same)
//   e  text                     the writer's emulator ("ryubing", "kenjinx")
//   ev text                     ... and its version, to show
//   p  "Ryujinx-" + 8 hex       the session's passphrase
// an answer also has
//   a  1 join, 0 no             (and e / ev / gv of the one answering)
// Unknown keys are ignored, so later versions can add to it.
enum Kind { kInvite = 1, kAnswer = 2, kCancel = 3 };
constexpr int64_t kLifetime = 600; // an invitation is good for ten minutes
constexpr int64_t kSkew = 120;     // two PCs' clocks may disagree this much
extern const char* const kFamilyRyuLdn;

struct Msg {
    int kind = 0;
    std::string nonce; // 16 bytes
    int64_t time = 0, expires = 0;
    std::string titleId; // 16 hex digits
    std::string game, gameVersion;
    std::string family, emulator, emuVersion;
    std::string passphrase;
    bool yes = false;
};
std::string Encode(const Msg& m);
// False unless every field the kind needs is there and well formed.
bool Decode(const std::string& body, Msg& m);
std::string NewNonce();
// "Ryujinx-" and eight lowercase hex digits: the only form RyuLDN takes
// (Ryubing src/Ryujinx.Common/Helpers/Patterns.cs, the server's
// LdnServer/Session/Handlers/Passphrase.cs: anything else counts as none,
// i.e. the public lobby).
std::string NewPassphrase();
bool ValidPassphrase(const std::string& p);
// Whether an invitation read at `receivedAt` can still be answered at `now`
// (our clock): within its own lifetime by both clocks, give or take kSkew.
bool Fresh(const Msg& inv, int64_t receivedAt, int64_t now);
// Seconds left to answer, by our clock (0 when gone).
int64_t SecondsLeft(const Msg& inv, int64_t receivedAt, int64_t now);

// ---- emulators ---------------------------------------------------------------

struct Support {
    bool ok = false;
    std::string family;  // kFamilyRyuLdn when ok
    std::wstring why;    // when not: one or two lines for the user (Spanish)
};
// `emuId` as data.cpp's IdentifyExe tells them apart; `configPath` that
// emulator's Config.json. Opens the file with ryucfg (ryucfg's one open file
// changes: callers that keep it open must open theirs again).
Support CanPlay(const std::string& emuId, const std::wstring& emuLabel, const std::wstring& configPath);
// The number `multiplayer_mode` takes for LdnRyu in that fork; nullptr when it
// has none.
const char* LdnModeValue(const std::string& emuId);

// Writes the session's values; what was there is saved first (netplay.dat).
// A restore still waiting from an earlier session is done before. False, with
// nothing written, when the file cannot be read or lacks the keys.
bool Apply(const std::wstring& configPath, const std::string& emuId, const std::string& passphrase, std::string& error);
// Puts back what Apply replaced. True when nothing is left to put back.
bool Restore(std::string* error = nullptr);
bool RestorePending();

// ---- the session ---------------------------------------------------------------

enum class Phase {
    Idle,
    Inviting,  // host: the invitation is being written / waits for an answer
    Joining,   // guest: "Unirse" chosen, the answer is being written
    Ready,     // both agreed: start the game now
    Playing,   // the game was started
    Declined,  // host: the friend said no (or left)
    NoAnswer,  // host: the invitation expired
    Unsent,    // a message could not be written
    Withdrawn, // guest: the host took the invitation back
};
struct Session {
    Phase phase = Phase::Idle;
    bool host = false;
    std::string owner, code; // the user, the friend
    Msg inv;
    int64_t since = 0;       // when the phase began (Unix, our clock)
    std::string sendState;   // host while Inviting: "writing" / "stored"
};
struct Incoming {
    std::string code; // from
    Msg inv;
    int64_t at = 0;   // read here (our clock)
};

// `dataDir` (UTF-8) holds netplay.dat. `offline`: a test run of the menu
// (--shot): nothing is read or written on disk, and friends are simulated:
// an invited friend accepts after a few seconds (TestAnswer changes that).
void Init(const std::string& dataDir, bool offline);
// `inv` carries the game and the emulator; the nonce, passphrase and times
// are made here. Any other session is left first.
bool Invite(const std::string& owner, const std::string& code, Msg inv);
// Leaves the session: an invitation is withdrawn (the friend's list drops it).
void Cancel();
// The answer to `code`'s invitation. Joining: the session becomes theirs,
// `mine` saying which emulator and game version answer (shown to them).
bool Answer(const std::string& owner, const std::string& code, bool join, const Msg& mine = Msg());
// Invitations that can still be answered, newest first.
std::vector<Incoming> Invitations(const std::string& owner);
const Session& Current();
// Once a frame: reads the answers, moves the session on, times out. True
// when something the page shows has changed.
bool Poll();
// The game was started / has ended; Dismiss: an outcome was read.
void Launched();
void Ended();
void Dismiss();

// ---- tests ---------------------------------------------------------------------

// Offline mode: what an invited friend answers (1 join, 0 no, -1 never) and
// after how long, and how long writing a message takes.
void TestAnswer(int answer, double seconds, double writeSeconds = 1.5);
// Offline mode: the clock the simulation runs on (the menu's frames).
void TestTime(double seconds);
void TestAddIncoming(const std::string& owner, const Incoming& in);
void TestSetSession(const Session& s);
// Handled nonces (answered, refused, expired) and when they can be forgotten.
size_t TestHandledCount();

} // namespace netplay

// From the program (data.cpp), for the menu's netplay page: the emulator a
// game starts with, and that emulator's Config.json (the copy in
// <data>\<emulator id> in a test run).
std::string GameEmulator(const std::string& gameId);
std::wstring EmuConfigPathOf(const std::string& emuId);
