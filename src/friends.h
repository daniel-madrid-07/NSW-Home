// Friends: seeing when someone you are friends with is in NSW-Home too, what
// they are playing and their play time. There is no server: everything lives
// in the public BitTorrent DHT (fr_dht) as small signed values (BEP 44
// mutable items). Friendship is by request, as on the console: a code is
// looked up, a request is sent, the other person accepts or declines, and
// only an accepted request makes two people friends, both ways.
//
// What lives where (version 2; the formats are in friends.cpp):
//   - Each user has an identity key (Ed25519) that never leaves the data
//     folder, and the friend code is MADE from it (a slow hash of its public
//     half, cut to twelve digits): a card that names another identity cannot
//     pass as the code's owner without some 10^12 tries of a fifth of a
//     second each.
//   - The card: nickname, disc colour, identity, and the X25519 key requests
//     are sealed to. Stored at a place made from the code, so anyone who has
//     the code can read it, and also write over it; a card is only believed
//     when its identity makes the code.
//   - The inbox: four places a day, also made from the code, where requests
//     are left, each sealed to the card's X25519 key and signed by the
//     asker's identity. Nobody but the recipient can read one, nobody but the
//     asker can make one.
//   - Between two friends, a note in each direction at a place only the two
//     can work out (from their X25519 secret): "we are friends, here is my
//     status key and where my status is" or "this friendship is over".
//     Beside it, at the same keys, a message place in each direction
//     (invitations to play together; see SendMessage).
//   - The status: at a place made from a random key of the user's own,
//     sealed with a random status key; both are given only to accepted
//     friends and replaced (new key, new place) whenever a friend is removed.
//
// The module starts no network activity by itself: nothing is sent until
// sharing is on for a user (SetSharing) or a code is looked up (StartLookup),
// and the pages only do either after the user has agreed.
//
// Everything here may be called from the menu's thread at any time; the
// network runs on a thread of the module's own.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace friends {

struct GameTime {
    std::string id; // title ID (16 hex digits) when the game has one
    std::string name;
    uint64_t seconds = 0;
};

enum State { InMenu = 0, Playing = 1, SignedOff = 2 };

struct Status {
    std::string nick; // UTF-8
    int state = InMenu;
    std::string game, gameId; // while Playing
    int64_t time = 0;         // when it was written (Unix seconds, the writer's clock)
    uint64_t total = 0;       // play time over all games, seconds
    int colour = -1;          // index of the disc colour; -1: the user has a picture
    std::vector<GameTime> games; // most played first, as many as fit
    // Filled in by the module: one mark (6 bytes) per friend a message waits
    // for (see SendMessage); only that friend can tell which one is theirs.
    std::vector<std::string> wake;
};

// What anyone who has a code can learn about its owner.
struct Card {
    std::string code;     // twelve digits
    std::string nick;     // UTF-8
    int colour = -1;
    std::string identity; // Ed25519 public key (32 bytes); the code is made from it
    std::string dh;       // X25519 public key (32 bytes) requests are sealed to
};

struct Friend {
    std::string code;
    Status st;            // nick and colour from the request until a status is read
    bool known = false;   // a status has been read
    int64_t seen = 0;     // when the friend was last known to be there (Unix seconds, our clock)
    int64_t since = 0;    // when the friendship began (our clock)
};

struct Request {
    Card who;
    int64_t time = 0; // sent at (the sender's clock)
    int64_t at = 0;   // received: when it arrived here; sent: when it was sent (our clock)
};

// A friend counts as present for this long after the last sign of life.
constexpr int64_t kOnlineSeconds = 15 * 60;
bool Online(const Friend& f, int64_t now);
int64_t UnixNow();
bool ValidCode(const std::string& code);

// ---- the item formats (also used by the tests) ------------------------------

// The code an identity makes: PBKDF2-SHA-256 of its public key, cut to twelve digits.
std::string CodeOf(const std::string& identity);
struct Keys {
    std::string seed, pub; // Ed25519: the card's and the inbox's places
    std::string seal;      // the card's encryption
};
Keys DeriveKeys(const std::string& code);
extern const char* const kCardSalt;
std::string InboxSalt(int64_t day, int slot);
constexpr int kInboxSlots = 4;
int64_t DayOf(int64_t unix);

// The card. `open` false: the owner has stopped sharing (nickname left out).
std::string SealCard(const Keys& k, const std::string& idSeed, const Card& c, bool open);
// False unless the value decrypts, is signed by the identity it names and, when
// `checkCode`, that identity makes `code` (a fifth of a second).
bool OpenCard(const Keys& k, const std::string& code, const std::string& value, Card& c, bool& open, bool checkCode = true);

// A request (kind 1) or the withdrawal of one (kind 2), sealed to the recipient.
struct Envelope {
    int kind = 0;
    Card from;
    int64_t time = 0;
};
std::string SealRequest(const Card& to, const std::string& fromSeed, const Envelope& e, const std::string& ephemeral);
// Decrypts and checks the signature; whether the identity makes the code it
// claims is left to the caller (it is slow).
bool OpenRequest(const std::string& myIdentity, const std::string& myDhSecret, const std::string& myDh, const std::string& value, Envelope& e);

// The note from one friend to the other.
struct Note {
    int state = 0;           // 1: friends; 0: the friendship is over
    int64_t gen = 0;         // which status key this is
    std::string key, place;  // status key; public key of the status's place
    int64_t time = 0;
};
struct PairKeys {
    std::string seed, pub, seal;
};
// Keys of the note `writer` leaves for the other one, from the two users' shared secret.
PairKeys PairKeysOf(const std::string& shared, const std::string& writer);
std::string SealNote(const PairKeys& k, const Note& n, const std::string& nonce);
bool OpenNote(const PairKeys& k, const std::string& value, Note& n);
extern const char* const kNoteSalt;

// The status, sealed with the status key. `moved`: this place is no longer
// written; the reader asks its note for the new one.
std::string SealStatus(const std::string& key, const Status& st, bool moved, const std::string& nonce);
bool OpenStatus(const std::string& key, const std::string& value, Status& st, bool& moved);
extern const char* const kStatusSalt;

// A message from one friend to the other (play-together invitations and their
// answers, netplay.h): at the same pair keys as the note, under a salt of its
// own, sealed with a key of its own made from the same pair secret. Only the
// two can read it (the seal) and only the two can write there (the place's
// signing key). What the body says is the caller's.
extern const char* const kMsgSalt;
std::string SealMessage(const PairKeys& k, const std::string& body, const std::string& nonce);
bool OpenMessage(const PairKeys& k, const std::string& value, std::string& body);
// The mark the writer's status carries while a message waits for its reader:
// different at every status (it depends on the status' time), and only
// someone who holds the pair's keys can tell it is theirs.
std::string WakeTag(const PairKeys& k, int64_t statusTime);

// ---- the service ----------------------------------------------------------

// `dataDir` (UTF-8) is where friends.dat lives. `offline` is for test runs:
// no thread, no network, nothing read from or written to disk.
void Init(const std::string& dataDir, bool offline);
// Writes "signing off" where a status was published, then stops. Waits a few
// seconds at most.
void Shutdown();

// Per NSW-Home user (`owner` is the user's id).
// The user's friend code (made, with the identity, on the first call: a fifth of a second).
std::string OwnCode(const std::string& owner);
bool Sharing(const std::string& owner);
void SetSharing(const std::string& owner, bool on);
std::vector<Friend> List(const std::string& owner);
// Ends the friendship for both: the other is told, and the status moves to a
// new key and place they do not have.
void Remove(const std::string& owner, const std::string& code);
// The disc colour the user chose (-1: a picture); remembered for the card and the status.
void SetColour(const std::string& owner, int colour);
int Colour(const std::string& owner, int fallback);

// The status of a user; published (while the user shares and has friends)
// whenever it changes and every ten minutes. `time` is filled in here.
void SetSelf(const std::string& owner, const Status& st);

// Looking a code up (its card). One at a time.
enum class Look { Idle, Busy, Found, NotFound, NoNetwork, Unverified };
void StartLookup(const std::string& code);
Look LookupState(Card* found = nullptr);
void CancelLookup();

// Requests.
enum class Sent { Sent, Already, Friends, Accepted, Failed };
Sent SendRequest(const std::string& owner, const Card& to);
std::vector<Request> Received(const std::string& owner);
std::vector<Request> Outgoing(const std::string& owner);
bool Accept(const std::string& owner, const std::string& code);
void Decline(const std::string& owner, const std::string& code);
void Withdraw(const std::string& owner, const std::string& code);

// Signs that someone who has the user's code is writing where they should not.
struct Trouble {
    bool card = false;  // another value lies over the user's card
    bool inbox = false; // the inbox holds values that are not requests to the user
};
Trouble Troubles(const std::string& owner);

// Messages to and from friends (the transport of netplay.h). Each friend has
// one place for each of the two: a new message replaces the one before.
// The status then carries a mark for that friend for a while (wakeSeconds),
// so their NSW-Home reads the message at its next reading of the status.
constexpr size_t kMaxMessage = 600;
// False when `code` is not a friend or the body is too long. Written at once
// (before notes, statuses and reads).
bool SendMessage(const std::string& owner, const std::string& code, const std::string& body);
enum class MsgState { None, Writing, Stored, Failed };
MsgState MessageState(const std::string& owner, const std::string& code);
struct Message {
    std::string code;  // from whom
    std::string body;
    int64_t seq = 0;   // the place's number (only ever goes up)
    int64_t at = 0;    // when it was read here (our clock)
};
// The latest message from each friend who has left one (since this start).
std::vector<Message> Messages(const std::string& owner);
// While on, that friend's place is read every messageSeconds (an answer is
// awaited); off, only when their status carries the mark.
void Expect(const std::string& owner, const std::string& code, bool on);

// Read everything again now ("Actualizar"); a friends page is on screen (read more often).
void RefreshNow();
void Watching(bool on);
// Goes up whenever something the pages show has changed.
unsigned Version();
// True while friends' statuses are being read after RefreshNow.
bool Refreshing();

// ---- for tests ------------------------------------------------------------

// Offline mode: what the next lookup answers, and after how long.
void TestLookup(Look result, const Card& c, double seconds);
// Offline mode: made-up entries for the pages.
void TestAddFriend(const std::string& owner, const Friend& f);
void TestAddReceived(const std::string& owner, const Request& r);
void TestAddSent(const std::string& owner, const Request& r);
void TestSetTrouble(const std::string& owner, const Trouble& t);

struct Tuning {
    double pollSeconds = 60, watchSeconds = 30, republishSeconds = 600;
    double queriesPerSecond = 8;
    double inboxSeconds = 120;     // each inbox place read this often (one read every inboxSeconds / 4)
    double answerSeconds = 60;     // a request just sent: how often the answer is looked for (it slows down later)
    double noteSeconds = 1800;     // a friend's note read again (sooner when their status says it moved)
    double noteWriteSeconds = 7200;// our note to each friend written again
    double cardSeconds = 900;      // the own card looked at (someone else's over it is noticed) and kept alive
    double resendSeconds = 1800;   // a request still waiting written again
    double messageSeconds = 4;     // a friend's message place read this often while an answer is awaited
    double wakeSeconds = 900;      // how long the status carries the mark of a message written
};
void SetTuning(const Tuning& t);
// One line per network operation, from the module's thread.
void SetLog(std::function<void(const std::string&)> fn);
// Before Init: a network inside this process instead of the DHT (the unit
// tests run several users of one module against it), and what is stored there.
void UseMemoryNetwork();
bool MemPut(const std::string& pub, const std::string& salt, const std::string& value, int64_t seq, const std::string& sig);
bool MemGet(const std::string& pub, const std::string& salt, std::string& value, int64_t& seq);
// What a user holds: the status key and place it got from a friend's note
// (false: none yet), and its own of now.
bool TestPeek(const std::string& owner, const std::string& friendCode, std::string& key, std::string& place);
void TestOwnPlace(const std::string& owner, std::string& key, std::string& place);
// Traffic so far (operations, packets) for the tests' report.
struct Traffic {
    uint64_t gets = 0, puts = 0, packetsOut = 0, packetsIn = 0, bytesOut = 0, bytesIn = 0;
};
Traffic Counters();

} // namespace friends
