// amiibo on a PC: a collection of tag dumps kept by NSW-Home, what can be read
// from a dump with and without the tag keys, the console's three amiibo
// operations done on the dump itself, and handing a dump to the game that is
// running in an emulator.
//
// A figure is an NTAG215 tag; a dump of it is a file of 540 bytes (sometimes
// 532, without the password pages, or 572, with the tag's 32-byte originality
// signature after it). Most of the tag is encrypted and signed with keys
// derived from a 160-byte master key file (commonly key_retail.bin) that only
// the user can supply. The layout and the derivation are public: they are
// documented by the amiitool project (socram8888/amiitool, MIT licence); the
// code in amiibo.cpp is written from that description, not copied.
//
// amiibo.cpp is portable (it uses only the Windows-style file calls that the
// port's compatibility layer also provides). The hand-over to an emulator is
// in amiibo_emu.cpp and works on Windows only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace amiibo {

// ---- a dump, read without keys ----------------------------------------------

constexpr size_t kTag = 540;
bool TagSize(uint64_t n);
// The size and the fixed parts of an amiibo tag's layout (serial number check
// bytes, lock bytes, capability container, the 0xA5 marker, tag type, dynamic
// lock and configuration pages). Says nothing about the signatures.
bool LooksLikeTag(const std::string& bytes);
// The figure's identification block, the 8 bytes at 0x54, as 16 upper-case hex
// digits (head then tail, the way the emulators print it).
std::string IdHex(const std::string& tag);
int Series(const std::string& tag); // the series byte of that block (0x5A)

// ---- keys ---------------------------------------------------------------------

// Two 80-byte master keys: "unfixed infos" (the data key) then "locked secret"
// (the tag key). Never printed, logged or copied anywhere but KeyFile().
struct Keys {
    uint8_t data[80], tag[80];
};
// "" or KEYS_INVALID (not 160 bytes, or not the two master keys in either order).
std::string ParseKeys(const std::string& raw, Keys& out);

// ---- the decrypted tag ------------------------------------------------------------

// A decrypted dump in the tag's own layout (540 bytes; the signatures in it are
// the ones it should carry). False: the keys do not fit this dump, or it was
// changed without them (a signature does not match).
bool Decode(const Keys& k, const std::string& tag, std::string& plain);
// Signs and encrypts. The result has the length of `plain` (bytes after 0x208
// are copied as they are).
std::string Encode(const Keys& k, const std::string& plain);

struct Facts {
    bool registered = false; // has an owner and a nickname
    bool gameData = false;   // a game has written its data area
    std::wstring nickname, owner; // the owner Mii's name
    uint64_t appId = 0;       // the game that owns the data area
    unsigned writes = 0;      // the tag's write counter
    std::wstring setUp, written; // dates of registration and of the last write, dd/mm/yyyy
};
Facts Read(const std::string& plain);

// The console's operations, on a decoded dump. Each returns "" or an error
// code: NOT_REGISTERED, NO_DATA, BLANK, BAD_NAME. `y m d` is today's date.
std::string SetNickname(std::string& plain, const std::wstring& name, int y, int m, int d);
std::string DeleteGameData(std::string& plain, int y, int m, int d);
std::string Format(std::string& plain, int y, int m, int d);

// ---- the collection (<data>\amiibo) -------------------------------------------------

struct Entry {
    std::wstring file; // <data>\amiibo\<name>.bin
    std::wstring name;
    std::string id;    // IdHex
    int series = 0;
    std::string bytes;
    bool backup = false; // <name>.bin.bak, the entry as it was before NSW-Home first changed it
};
std::wstring Dir();
std::wstring KeyFile(); // NSW-Home's own copy of the user's key file
// Every dump in the collection, by name. A dump that Ryujinx and its forks
// wrote back next to the one they were given (<name>_modified.bin) replaces it
// first (the old one is kept as the .bak when there is none).
std::vector<Entry> List();

struct Imported {
    int added = 0, skipped = 0, had = 0, failed = 0;
    std::wstring name; // of the last one added
};
// Copies (never moves or changes the original). A folder is walked with its
// subfolders; anything that is not a tag dump is skipped.
Imported ImportFile(const std::wstring& path);
Imported ImportFolder(const std::wstring& dir);
// "" or BAD_NAME, NAME_TAKEN, NOT_OURS, IO.
std::string Rename(const std::wstring& file, const std::wstring& name);
std::string Remove(const std::wstring& file); // the dump and its .bak; only inside Dir()

bool HaveKeys();
std::string SetKeys(const std::wstring& file); // "" or KEYS_INVALID, IO
void ForgetKeys();
bool LoadKeys(Keys& out);

// Decodes an entry with the stored keys, applies one of the operations above,
// encodes it, decodes the result again and compares it with what was meant,
// and only then writes it (keeping the .bak first). "" or the operation's code,
// NO_KEYS, BAD_KEYS (the keys do not open this dump), VERIFY, IO.
enum class Op { Nickname, DeleteData, Format };
std::string Apply(const std::wstring& file, Op op, const std::wstring& nickname = {});
// The facts of an entry, with the stored keys; false without keys or when they do not open it.
bool Open(const std::string& bytes, Facts& out);
// The .bak of an entry made now if it has none (before the dump goes to an emulator that writes to it).
void Backup(const std::wstring& file);

// The system's file pickers for a dump and for the key file ("" when nothing
// was chosen, and always in offscreen test runs).
std::wstring PickDump();
std::wstring PickKeyFile();

// tools/amiibo_test.cpp: the cipher alone (AES-128 in counter mode), for the
// published test vectors and the comparison with the system's own AES.
std::string CtrForTest(const std::string& key16, const std::string& iv16, const std::string& data);

// ---- the running game (amiibo_emu.cpp) -------------------------------------------------

// Windows only; elsewhere false and nothing happens.
bool HandOverAvailable();
// Starts handing the dump to the emulator the running game is shown from
// (`family` as EmuDef::family). "" or NO_GAME, BUSY. The rest happens on a
// thread of its own: the emulator is given the keyboard, sent its own amiibo
// key, and the file dialog it opens (never on screen: its windows are cloaked
// while it runs) gets the file's path and is confirmed.
std::string HandOverStart(const std::wstring& file);
// True once it has finished: `code` "" (the game has the amiibo) or NOT_LOOKING,
// NO_DIALOG, FILL, FAILED; `said` is what the emulator's own message said, if any.
bool HandOverDone(std::string& code, std::wstring& said);
void HandOverCancel();
// Whether that emulator can read a dump at all: Ryujinx's forks decrypt it and
// need key_retail.bin in their "system" folder (only looked for, never read);
// the yuzu family loads one without keys (read-only then).
bool EmulatorCanRead(const std::wstring& emuDataDir, char family);
// The file to hand to an emulator of that family for an entry: the entry
// itself, or for Ryujinx's forks, which read exactly 540 bytes, a 540-byte copy
// in <data>\amiibo\envio (what the game writes comes back beside it as
// <name>_modified.bin, which List() folds into the entry).
std::wstring ForEmulator(const std::wstring& file, char family);
bool HandOverBusy(); // main.cpp: while true, the emulator's dialogs are ours to answer
// Supplied by main.cpp: the emulator shown through the game viewer, if any.
bool EmuLink(uint32_t& pid, void*& emuWindow, void*& menuWindow, char& family);
// Test of the hand-over against stand-in windows of this process (cloaked,
// off screen, never activated). Writes a report; true when every case passed.
bool HandOverSelfTest(const std::wstring& dump, std::string& report);

} // namespace amiibo
