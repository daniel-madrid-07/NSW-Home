// What the "Gestión de datos" rows do with an emulator's own files: listing
// and removing its rebuildable caches, copying its save data somewhere else,
// and moving a game's file between two folders of the library.
//
// The save and cache folders are the most valuable thing on the machine, so:
//   - nothing here ever writes into, renames or deletes a save folder, a key
//     or firmware; saves are only ever read (copied out);
//   - the only thing deleted inside an emulator's data folder is a cache
//     folder, and only one that sits exactly where that emulator family puts
//     its caches and is called what the emulator calls it (IsCacheDir). A
//     path that is anything else is refused, whoever asks;
//   - links and junctions are never followed: a link found inside a cache is
//     removed as a link, and one found among the saves is not copied.
//
// Stands alone (no app.h, no system calls of its own: the C++ file library),
// so it is the same code on every platform.
//
// Layouts (docs/emulators/catalogue.md, checked against the owner's folders):
//   Ryujinx family  <data>\games\<title id>\cache\cpu      translated code (PPTC)
//                   <data>\games\<title id>\cache\shader   shader cache
//                   <data>\bis\user\save\<n>               save data, numbered
//                   <data>\bis\user\saveMeta\<n>           its metadata
//                   <data>\bis\system\save\8000000000000000  the index that says whose each <n> is
//   yuzu family     <data>\shader\<title id>               shader/pipeline cache
//                   <data>\nand\user\save\0000000000000000\<user>\<title id>
// A title id is sixteen hex digits. Beside a Ryujinx cache folder live
// updates.json, dlc.json, mods.json and a per-game Config.json: not caches.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace emudata {

// One emulator's data folder. `family`: 'r' Ryujinx and forks, 'y' yuzu family.
struct Root {
    std::string emu; // NSW-Home's id, for telling the user which
    char family = 'r';
    std::wstring dir;
};

struct Cache {
    std::wstring dir;
    std::string titleId; // lower case
    uint64_t bytes = 0;
};

struct Outcome {
    bool ok = false;
    uint64_t bytes = 0; // removed, copied or moved
    int items = 0;      // folders removed, files copied
    // "" or one of: REFUSED (not a place this may touch), MISSING, EXISTS,
    // NO_SPACE, IO, VERIFY (the copy does not match), KEPT (copied, but the
    // original could not be removed: both files are there), NO_INDEX.
    std::string error;
    std::wstring where; // the folder or file made
};

// Bytes in all regular files under `dir` (links not followed); 0 if missing.
uint64_t TreeSize(const std::wstring& dir);

// True only for a cache folder of that emulator family inside `r.dir`.
bool IsCacheDir(const Root& r, const std::wstring& dir);
// Every cache folder there is, with its size.
std::vector<Cache> ListCaches(const Root& r);
// Removes exactly these folders. All of them are checked first: one that is
// not a cache folder of `r` refuses the whole call and nothing is removed.
Outcome RemoveCaches(const Root& r, const std::vector<std::wstring>& dirs);

// The folders that make up the save data, relative to `r.dir`, existing ones only.
std::vector<std::wstring> SaveParts(const Root& r);
uint64_t SavesSize(const Root& r);
// Copies the save data to the new folder `dest` (which must not exist yet),
// keeping the layout it has inside the data folder so that it can be put back
// by copying it over. Nothing else is copied.
Outcome BackupSaves(const Root& r, const std::wstring& dest);

// One user's save folders: (title id, folder). The yuzu family keeps a folder
// per user; Ryujinx numbers its saves and says whose they are in an index,
// which is read here. `error` is NO_INDEX when that index cannot be read.
// `userId`: NSW-Home's 32 hex digits.
std::vector<std::pair<std::string, std::wstring>> UserSaves(const Root& r, const std::string& userId, std::string& error);
// The yuzu family's folder for that user ("" for Ryujinx, which has none).
std::wstring UserSaveFolder(const Root& r, const std::string& userId);
// Copies them to the new folder `dest` as <dest>\<title id>\...
Outcome ExportUserSaves(const Root& r, const std::string& userId, const std::wstring& dest);

// Moves `file` into the folder `toDir`: copied under a temporary name, its
// size compared with the original's, given its name, and only then is the
// original removed. A failure at any point leaves the original
// where it was. `where` is the new file.
Outcome MoveGameFile(const std::wstring& file, const std::wstring& toDir);

// Small helpers for the pages: whether a folder is there, making one (with
// its parents), and the plain files directly inside one (name, size).
bool IsDir(const std::wstring& dir);
bool MakeDir(const std::wstring& dir);
std::vector<std::pair<std::wstring, uint64_t>> ListFiles(const std::wstring& dir);

// Free bytes on the disk holding `dir`; false if unknown.
bool FreeSpace(const std::wstring& dir, uint64_t& bytes);

// "2026-11-26 14.32.05" (local time), for naming a dated copy.
std::wstring Stamp();

} // namespace emudata
