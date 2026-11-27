// Updates, downloadable content, mods and cheats of one game, as the
// emulators themselves keep them: finding them, saying which are in use, and
// changing that in the emulator's own files.
//
// Stands alone (no app.h; the C++ file library, plus nsw_core.h for reading
// dumps), so it is the same code on every platform and tools can link it.
//
// What each emulator family reads when a game starts (checked in their
// sources; see the report of this work for file and line):
//
//   Ryujinx, Ryubing, Kenji-NX: per game, in <data>\games\<title id>:
//     updates.json  {"selected": "<file>", "paths": [...]}; only "selected"
//                   counts at start: an empty or missing file is the game
//                   without update.
//     dlc.json      [{"path": "<file>", "dlc_nca_list": [{"path": "/<x>.nca",
//                   "title_id": <number>, "is_enabled": true}]}]; only what is
//                   listed and enabled is loaded.
//     mods.json     {"mods": [{"name", "path", "enabled"}]}; a mod folder that
//                   is not listed is ON.
//     Mods are folders holding romfs\ or exefs\ anywhere under
//     <data>\mods\contents\<title id>\ and <data>\sdcard\atmosphere\contents\<title id>\;
//     cheats are <16 hex digits>.txt files in any cheats\ folder there, and
//     the ones that are on are the lines of
//     <data>\mods\contents\<title id>\cheats\enabled.txt ("<BUILD ID>-<<name> Cheat>");
//     a cheat not listed there is OFF.
//   Eden, Suyu, Citron (yuzu family): qt-config.ini, section [DisabledAddOns]:
//     per game the names of the add-ons that are OFF: "Update@<version>"
//     (Eden, Suyu: one per version of an update), "Update" (all updates),
//     "DLC" (all downloadable content: there is no switch per item), and the
//     name of each mod folder of <data>\load\<TITLE ID>\ (which also carries
//     that folder's cheats\). Eden and Suyu load update and DLC files from the
//     folders listed in [UI] Paths\external_content_dirs before the game
//     starts; a folder of games is only looked through by their game list,
//     which does not wait for (Eden, Citron) or does not even run with (Suyu)
//     a game given on the command line.
//
// How files are written: as each emulator writes them (the Ryujinx family's
// JSON with two spaces, CRLF and its escaping; the ini's key order and line
// endings), one change at a time, through a temporary file that replaces the
// original in one step, keeping "<file>.dockside-backup" from the first
// write. Whatever NSW-Home adds (an entry, a line, a whole file, a folder) is
// noted in "<file>.dockside-made" beside it, so that putting a choice back
// takes exactly that away again and gives back the file as it was. Nothing is
// ever written while the emulator runs: the caller makes sure of that.
//
// The ".dockside-*" suffixes (historical name: -backup, -made, -tmp, -part)
// are the program's former name, kept on purpose for every file written
// beside an emulator's (here, ryucfg, yuzucfg, padcfg, data.cpp, emudata): a
// backup made by an earlier version must be found as one, never taken for
// missing and overwritten with a file this program has already changed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace content {

// ---- the user's dumps -------------------------------------------------------

enum : uint8_t { kGame = 0x80, kUpdate = 0x81, kDlc = 0x82, kDataPatch = 0x83 };

// One title inside a dump (its content meta).
struct Meta {
    uint8_t type = 0;       // kGame, kUpdate, kDlc...
    uint64_t id = 0;        // the title's own id
    uint64_t app = 0;       // the game it belongs to (its own id for a game)
    uint32_t version = 0;   // the meta's version number (v65536 = 1.0.1-style steps)
    std::wstring display;   // an update's version as the game shows it ("1.6.0"); may be empty
    std::string nca;        // DLC: its data as Ryujinx names it ("/<name>.nca", as in the container)
    uint64_t ncaTitle = 0;  // DLC: the title id in that data's header (what dlc.json stores)
};

struct Dump {
    std::wstring file;
    uint64_t size = 0, mtime = 0;
    std::string error;      // "" or the reader's code (NO_KEYS, BAD_FORMAT...)
    std::vector<Meta> metas;
};

// Where what was read from each dump is kept between runs (a JSON file).
// Without one nothing is remembered.
void SetCache(const std::wstring& file);
// Reads the dump, or takes what was read before while its size and time are
// the same. Safe to call from several threads. False when it cannot be read.
bool Probe(const std::wstring& file, const std::wstring& keysDir, Dump& out);
// Writes the cache file if anything new was read.
void SaveCache();
// The dump holds updates or downloadable content and no game: it is not a
// game of its own.
bool AddOnOnly(const Dump& d);
// Every .nsp and .xci in these folders (and `depth` levels of folders below),
// and these files, each once, read through Probe.
std::vector<Dump> Collect(const std::vector<std::wstring>& dirs, int depth, const std::wstring& keysDir,
                          const std::vector<std::wstring>& files = {});

// A DLC dump's name for the list: the file's name without the bracketed
// tags people's dumps carry (title id, version, region, the site).
std::wstring DlcName(const std::wstring& file);
// "1.6.0" or, without a version text, "v196608" read as steps ("3.ª revisión").
std::wstring VersionText(const Meta& m);

// ---- mods and cheats ----------------------------------------------------------

struct Mod {
    std::wstring name;   // the folder's name
    std::wstring path;   // the folder (Ryujinx: the folder holding romfs\ / exefs\)
    std::wstring kinds;  // what it changes: "romfs", "exefs", "cheats"... joined by ", "
    bool on = true;
    bool cheatsOnly = false; // yuzu family: a folder with cheats and nothing else
};

struct Cheat {
    std::wstring name;   // the section's name in the file
    std::wstring file;   // the .txt it is in
    std::string key;     // Ryujinx: the enabled.txt line; yuzu family: the add-on name of its folder
    bool on = false;
};

// ---- Ryujinx family (dir = the emulator's data folder) ----------------------

namespace ryu {
// What the game will start with now.
struct State {
    std::wstring selected;            // update file ("" = none)
    std::vector<std::wstring> paths;  // the updates the emulator lists
    struct Dlc {
        std::wstring file;
        std::string nca;
        uint64_t id = 0;
        bool on = false;
    };
    std::vector<Dlc> dlc;
};
std::wstring GameDir(const std::wstring& dir, uint64_t titleId);
State Read(const std::wstring& dir, uint64_t titleId);
// `file` "" = the game without update.
bool SelectUpdate(const std::wstring& dir, uint64_t titleId, const std::wstring& file);
bool SetDlc(const std::wstring& dir, uint64_t titleId, const std::wstring& file, const std::string& nca, uint64_t dlcId, bool on);
std::vector<Mod> Mods(const std::wstring& dir, uint64_t titleId);
bool SetMod(const std::wstring& dir, uint64_t titleId, const Mod& mod, bool on);
std::vector<Cheat> Cheats(const std::wstring& dir, uint64_t titleId);
bool SetCheat(const std::wstring& dir, uint64_t titleId, const std::string& key, bool on);
// <dir>\mods\contents\<title id> (the one there is, whatever its case).
std::wstring ModsDir(const std::wstring& dir, uint64_t titleId);
// The update and DLC files this data folder lists for any game.
std::vector<std::wstring> ListedFiles(const std::wstring& dir);
// The folders Ryubing / Kenji-NX look through for updates and DLC
// (Config.json "autoload_dirs"); empty for the others.
std::vector<std::wstring> AutoloadDirs(const std::wstring& dir);
} // namespace ryu

// ---- yuzu family (ini = config\qt-config.ini, dir = the data folder) --------

namespace yuzu {
struct State {
    bool read = false;                 // the ini could be read
    std::vector<std::string> disabled; // this game's [DisabledAddOns] names
    std::vector<std::wstring> extDirs; // Paths\external_content_dirs
};
State Read(const std::wstring& ini, uint64_t titleId);
bool SetDisabled(const std::wstring& ini, uint64_t titleId, const std::string& name, bool disabled);
// Adds a folder to external_content_dirs, or takes away one NSW-Home added.
bool SetExtDir(const std::wstring& ini, const std::wstring& folder, bool present);
// The folders external_content_dirs holds that NSW-Home put there.
std::vector<std::wstring> ExtDirsMade(const std::wstring& ini);
std::vector<Mod> Mods(const std::wstring& dir, uint64_t titleId, const std::vector<std::string>& disabled);
std::vector<Cheat> Cheats(const std::wstring& dir, uint64_t titleId, const std::vector<std::string>& disabled, bool looseFiles);
std::wstring ModsDir(const std::wstring& dir, uint64_t titleId);
// The add-on name a folder is matched by (its name).
std::string AddOnName(const std::wstring& folderName);
} // namespace yuzu

// Small helpers.
std::string Hex16(uint64_t v);          // 16 lower-case hex digits
bool ParseHex16(const std::string& s, uint64_t& v);
std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);

} // namespace content

// Defined in data.cpp: the data folder NSW-Home reads and writes for the
// emulator with this id (a test run's copy of it when there is one).
std::wstring EmuFilesFor(const std::string& emuId);
