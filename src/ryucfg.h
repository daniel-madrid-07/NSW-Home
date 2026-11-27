// Ryujinx settings: reading and changing individual values of its Config.json,
// and the list of the ones worth showing in the console-style Settings pages.
//
// Stands alone (no app.h), so tools can link it without the rest of the app.
//
// How writing works: only the text of the value being changed is replaced.
// Every other byte of the file (key order, indentation, line endings, keys this
// module has never heard of, arrays) stays as it was. Each write goes to a
// temporary file that then replaces the original in one step, and the first
// write keeps a copy of the file as the emulator left it in
// "<file>.dockside-backup" (never overwritten once it exists).
//
// When changes take effect: the emulator reads this file once, when it starts.
// Every setting here therefore applies from the NEXT GAME START; none of them
// reaches a game that is already running. Worse, Ryujinx saves its in-memory
// settings back over the whole file at times of its own (on closing, to store
// the window position; this is from its source, not observed here), which
// would discard whatever was written meanwhile. So: do not call Set while a
// game is open; keep the change and apply it once the game has closed.
//
// Not thread-safe: one opened file at a time, used from the UI thread.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace ryucfg {

enum class Kind { Toggle, Choice, Number };
// How the value is written in the file.
enum class Type { Bool, Int, Float, String };

struct Choice {
    std::string value;  // exact text for the file ("Vulkan", "2", "-1")
    std::wstring label; // Spanish
};

struct Setting {
    std::string key;
    std::wstring section; // one of the k* section names below
    std::wstring label;
    std::wstring help; // one line
    Kind kind;
    Type type;
    std::vector<Choice> choices; // Choice
    double min, max, step;       // Number, in file units
    // Number, for showing only: shown = file value * shown, followed by unit.
    // audio_volume is 0..1 in the file and reads "80 %" with shown = 100.
    double shown;
    std::wstring unit;
    // True: the key and every value string were checked against Ryujinx
    // 1.1.1403 (its Config.json and the names inside its executable).
    // False: written from memory of a fork's source; see the note in the .cpp.
    bool verified;
    // Highest config "version" the entry is valid for; 0 = any.
    int maxVersion;
};

// Section names, as the Settings pages group them.
extern const wchar_t* const kConsola;   // language, region, clock
extern const wchar_t* const kPantallas; // TV mode, resolution, picture
extern const wchar_t* const kSonido;
extern const wchar_t* const kInternet;
extern const wchar_t* const kMandos;    // "Mandos y accesorios"
extern const wchar_t* const kDatos;     // "Gestión de datos"
extern const wchar_t* const kEmulador;  // no console equivalent

// The curated list, in display order within each section.
const std::vector<Setting>& Settings();
const Setting* Find(const std::string& key);

// %APPDATA%\Ryujinx\Config.json, or <exe folder>\portable\Config.json when the
// emulator at `emulatorExe` is a portable install. `emulatorExe` may be empty.
std::wstring ConfigPath(const std::wstring& emulatorExe = {});

// Reads the file. False if it is missing, unreadable or not a JSON object; in
// that case nothing is open and Has/Get/Set all fail. Call again to re-read.
bool Open(const std::wstring& configPath);
// The "version" number of the opened file (51 for Ryujinx 1.1.1403), 0 if none.
int Version();

// Keys are top-level names ("docked_mode"); a dot goes into an object
// ("window_startup.window_position_x"). Arrays cannot be addressed.
bool Has(const std::string& key);
// Current value as text: "true", "2", "0.8", "Vulkan" (strings unquoted).
// Empty if the key is missing or is not a single value.
std::string Get(const std::string& key);

// Whether a curated setting can be offered for the opened file: the key is
// there, holds the kind of value this module expects, and the file's version
// is one the entry is valid for. Show only settings for which this is true.
bool Available(const Setting& s);

// Changes one value and writes the file. The file is re-read from disk first,
// so changes made by the emulator since Open are kept. Returns false, leaving
// the file untouched, when: nothing is open, the key is missing, the value is
// not of the type the file holds there (a number for a number, true/false for
// a boolean), or, for a curated key, the value is not allowed: not one of its
// choices / outside min..max. A value that this key was seen to hold in the
// file is always accepted, so an unknown value from a newer fork can be put
// back. Writing the value already there succeeds without touching the file.
bool Set(const std::string& key, const std::string& value);
// Several changes in one write: all of them or none.
bool SetMany(const std::vector<std::pair<std::string, std::string>>& changes);

// Helpers for the pages.
// The value one step away (dir = +1 / -1): toggles flip, choices move through
// the list and wrap around, numbers move by `step` and stop at min/max.
std::string Next(const Setting& s, const std::string& current, int dir);
// Text to show for a value: "Sí"/"No", the choice's label, or the number with
// its unit. A value not in the list is shown as it is.
std::wstring Display(const Setting& s, const std::string& value);

} // namespace ryucfg
