// Settings of the yuzu family (Eden, Suyu, Citron, Sudachi, yuzu): reading and
// changing individual values of their qt-config.ini, and the list of the ones
// worth showing in the console-style Settings pages.
//
// Stands alone (no app.h), like ryucfg, whose Setting type it shares so the
// pages can treat both kinds of emulator alike.
//
// The file: INI sections ("[System]"), and inside them every setting is a pair
// of lines,
//     name\default=false
//     name=5
// The emulator only takes the value when the first line says false; with
// "true" (or without that line) it uses its built-in default whatever the
// second line says (Eden: src/frontend_common/config.cpp, ReadSettingGeneric).
// So a change writes the value and makes sure the first line reads false.
// Booleans are true/false; numbers and every enumeration are decimal numbers.
//
// The guarantees are ryucfg's: only the lines of the settings being changed
// are touched and every other byte stays as it was; each write goes to a
// temporary file that replaces the original in one step; the first write keeps
// "<file>.dockside-backup". The emulator reads the file when it starts and
// writes all of it back when it starts and when it closes, so nothing may be
// written while it runs: keep the change and apply it after the game closed.
#pragma once
#include "ryucfg.h"

namespace yuzucfg {

// Keys are "Section.name" ("System.language_index").

// The curated list for one emulator (its id in NSW-Home: "eden", "suyu",
// "citron"...). The meaning of some numbers differs between the members of
// the family, so entries are only listed for the emulators whose source or
// own files they were checked against.
const std::vector<ryucfg::Setting>& Settings(const std::string& emulatorId);
const ryucfg::Setting* Find(const std::string& emulatorId, const std::string& key);

// Reads the file. False if it is missing or unreadable; then nothing is open.
bool Open(const std::wstring& iniPath);
bool Has(const std::string& key);
// The text after "name=" (quotes kept as written). Empty if the key is missing.
std::string Get(const std::string& key);
// What the emulator will really use: Get, unless the "\default" line says
// true, in which case it is the setting's built-in default (`fallback`).
std::string Effective(const std::string& key, const std::string& fallback);
bool Available(const ryucfg::Setting& s);

// Changes values and writes the file (re-read from disk first). A key that is
// not in the file is added to its section, and so is a missing section, when
// `add` is true; otherwise a missing key makes the call fail with nothing
// written. Writing what is already there does not touch the file.
bool SetMany(const std::vector<std::pair<std::string, std::string>>& changes, bool add = false);
bool Set(const std::string& key, const std::string& value, bool add = false);

// The same on a file that need not exist yet: it is created (with its folder's
// parent already there) holding just these settings, which is a valid file,
// since every setting that is not in it takes its default.
bool Seed(const std::wstring& iniPath, const std::vector<std::pair<std::string, std::string>>& values);

} // namespace yuzucfg
