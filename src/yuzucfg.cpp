// See yuzucfg.h.
#include "yuzucfg.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <map>

namespace yuzucfg {

using ryucfg::Choice;
using ryucfg::Kind;
using ryucfg::Setting;
using ryucfg::Type;

namespace {

struct Span {
    size_t from = 0, to = 0; // [from, to) in File::text
};

struct Entry {
    Span value;       // the text after "name="
    Span flag;        // the text after "name\default="; empty span at 0 when the line is missing
    bool hasFlag = false;
};

struct File {
    bool open = false;
    std::wstring path;
    std::string text, eol = "\n";
    std::map<std::string, Entry> keys;       // "Section.name"
    std::map<std::string, size_t> sections;  // name -> offset just after the header line
};
File g;

bool ReadFile(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    out.clear();
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".dockside-tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Section names are stored the way Qt escapes them ("Data%20Storage"); the
// keys used here are the plain names, which is all the curated list needs.
void Parse(File& f) {
    f.keys.clear();
    f.sections.clear();
    f.eol = f.text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string section;
    size_t at = 0;
    while (at < f.text.size()) {
        size_t nl = f.text.find('\n', at);
        size_t next = nl == std::string::npos ? f.text.size() : nl + 1;
        size_t end = nl == std::string::npos ? f.text.size() : nl;
        if (end > at && f.text[end - 1] == '\r') end--;
        if (end > at && f.text[at] == '[' && f.text[end - 1] == ']') {
            section = f.text.substr(at + 1, end - at - 2);
            f.sections[section] = next;
        } else if (end > at && f.text[at] != ';' && f.text[at] != '#') {
            size_t eq = f.text.find('=', at);
            if (eq != std::string::npos && eq < end) {
                std::string name = f.text.substr(at, eq - at);
                static const std::string kFlag = "\\default";
                bool flag = name.size() > kFlag.size() &&
                            name.compare(name.size() - kFlag.size(), kFlag.size(), kFlag) == 0;
                if (flag) name.resize(name.size() - kFlag.size());
                Entry& e = f.keys[section + "." + name];
                if (flag) {
                    e.flag = {eq + 1, end};
                    e.hasFlag = true;
                } else {
                    e.value = {eq + 1, end};
                }
            }
        }
        at = next;
    }
    // A "\default" line whose value line is missing is not a setting we can read.
    for (auto it = f.keys.begin(); it != f.keys.end();)
        it = it->second.value.from == 0 ? f.keys.erase(it) : std::next(it);
}

bool Load(std::wstring path, File& f) { // by value: callers pass the path of the very file object being reset
    f = File();
    if (!ReadFile(path, f.text)) return false;
    f.path = path;
    f.open = true;
    Parse(f);
    return true;
}

bool IsNumber(const std::string& v) {
    if (v.empty()) return false;
    size_t i = v[0] == '-' ? 1 : 0;
    if (i == v.size()) return false;
    for (; i < v.size(); i++)
        if (v[i] < '0' || v[i] > '9') return false;
    return true;
}

// Applies the changes to `text` (already parsed into `f`). False when a key is
// missing and may not be added, or a value could break the file's structure.
bool Apply(File& f, const std::vector<std::pair<std::string, std::string>>& changes, bool add) {
    for (const auto& ch : changes) {
        const std::string& value = ch.second;
        if (value.find_first_of("\r\n") != std::string::npos) return false;
        size_t dot = ch.first.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= ch.first.size()) return false;
        const std::string section = ch.first.substr(0, dot), name = ch.first.substr(dot + 1);
        auto it = f.keys.find(ch.first);
        if (it == f.keys.end()) {
            if (!add) return false;
            auto sec = f.sections.find(section);
            size_t at;
            std::string lines = name + "\\default=false" + f.eol + name + "=" + value + f.eol;
            if (sec == f.sections.end()) {
                at = f.text.size();
                std::string head;
                if (!f.text.empty() && f.text.back() != '\n') head = f.eol;
                if (!f.text.empty()) head += f.eol;
                lines = head + "[" + section + "]" + f.eol + lines;
            } else {
                at = sec->second;
            }
            f.text.insert(at, lines);
        } else {
            // Value first: it lies after the flag line in every file the
            // emulators write, but nothing here depends on that.
            Entry e = it->second;
            struct Edit {
                Span at;
                std::string text;
            };
            std::vector<Edit> edits;
            if (f.text.compare(e.value.from, e.value.to - e.value.from, value) != 0) edits.push_back({e.value, value});
            if (!e.hasFlag) {
                // No flag line: put one in front of the value line.
                size_t lineStart = f.text.rfind('\n', e.value.from);
                lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
                edits.push_back({{lineStart, lineStart}, name + "\\default=false" + f.eol});
            } else if (f.text.compare(e.flag.from, e.flag.to - e.flag.from, "false") != 0) {
                edits.push_back({e.flag, "false"});
            }
            std::sort(edits.begin(), edits.end(), [](const Edit& x, const Edit& y) { return x.at.from > y.at.from; });
            for (const auto& ed : edits) f.text.replace(ed.at.from, ed.at.to - ed.at.from, ed.text);
        }
        Parse(f); // positions moved
    }
    return true;
}

bool Allowed(const Setting* s, const std::string& value, const std::string& current) {
    if (!s || value == current) return true; // not curated, or putting back what the file holds
    switch (s->kind) {
    case Kind::Toggle: return value == "true" || value == "false";
    case Kind::Choice:
        for (const auto& c : s->choices)
            if (c.value == value) return true;
        return false;
    case Kind::Number: {
        if (!IsNumber(value)) return false;
        double v = atof(value.c_str());
        return v >= s->min && v <= s->max;
    }
    }
    return false;
}

// The curated entries by key, over every emulator's list: a value is checked
// against the list of whichever emulator names the key (the lists only differ
// in what the numbers mean, not in which numbers exist).
const Setting* AnyCurated(const std::string& key);

} // namespace

// ---- the settings table ---------------------------------------------------
//
// Eden: names, sections, types and the meaning of every number are from its
// source as of 2026-11 (src/common/settings.h, settings_enums.h,
// src/frontend_common/config.cpp), and the names and defaults were then
// compared with the qt-config.ini that Eden v0.2.1 writes (language 1,
// region 1, docked 1, resolution 3, volume 100).
// The other members of the family share the names but not always the numbers
// (the resolution list is longer in Eden than in yuzu, the audio engine is a
// word in older builds), so an entry whose numbers could not be checked for
// an emulator is listed with verified = false and is not offered there.
// Language and region are the console's own numbers; the owner's Citron
// 0.6.1 file, set to Spanish and Europe in Citron itself, holds 5 and 2.

namespace {

std::vector<Choice> Languages() {
    // The console's language numbers (0 = Japanese ... 17), identical in every
    // member of the family since they are the console's own.
    return {{"5", L"Español"},          {"14", L"Español (Latinoamérica)"}, {"1", L"English"},
            {"12", L"English (UK)"},    {"2", L"Français"},                 {"13", L"Français (Canada)"},
            {"3", L"Deutsch"},          {"4", L"Italiano"},                 {"9", L"Português"},
            {"17", L"Português (Brasil)"}, {"8", L"Nederlands"},            {"10", L"Русский"},
            {"0", L"日本語"},            {"7", L"한국어"},                    {"15", L"简体中文"},
            {"16", L"繁體中文"}};
}

std::vector<Choice> Regions() {
    return {{"2", L"Europa"}, {"1", L"América"},   {"0", L"Japón"}, {"3", L"Australia y Nueva Zelanda"},
            {"4", L"China"},  {"5", L"Corea"},     {"6", L"Taiwán"}};
}

void Common(std::vector<Setting>& v) {
    v.push_back({"System.language_index", ryucfg::kConsola, L"Idioma", L"Idioma en el que se muestran los juegos.",
                 Kind::Choice, Type::Int, Languages(), 0, 0, 0, 1, L"", true, 0});
    v.push_back({"System.region_index", ryucfg::kConsola, L"Región",
                 L"Región de la consola. Algunos juegos cambian de contenido según la región.", Kind::Choice, Type::Int,
                 Regions(), 0, 0, 0, 1, L"", true, 0});
    v.push_back({"System.use_docked_mode", ryucfg::kPantallas, L"Modo televisor",
                 L"Televisor: la consola está en la base (más resolución). Portátil: como en la mano.", Kind::Choice,
                 Type::Int, {{"1", L"Televisor"}, {"0", L"Portátil"}}, 0, 0, 0, 1, L"", true, 0});
    v.push_back({"Audio.volume", ryucfg::kSonido, L"Volumen", L"Volumen del sonido de los juegos.", Kind::Number,
                 Type::Int, {}, 0, 200, 5, 1, L" %", true, 0});
}

} // namespace

const std::vector<Setting>& Settings(const std::string& emulatorId) {
    static std::map<std::string, std::vector<Setting>> lists;
    auto it = lists.find(emulatorId);
    if (it != lists.end()) return it->second;
    std::vector<Setting>& v = lists[emulatorId];
    Common(v);
    const bool edenBased = emulatorId == "eden" || emulatorId == "suyu";
    if (edenBased) {
        // Eden's ResolutionSetup (settings_enums.h): thirteen steps from a
        // quarter to eight times, 3 = original. Suyu v0.1.4 is built from
        // Eden's tree and its own file has the same default (3).
        v.push_back({"Renderer.resolution_setup", ryucfg::kPantallas, L"Resolución",
                     L"Multiplica la resolución a la que se dibuja el juego.", Kind::Choice, Type::Int,
                     {{"1", L"×0,5 (360p/540p)"},
                      {"2", L"×0,75 (540p/810p)"},
                      {"3", L"Original (720p/1080p)"},
                      {"4", L"×1,25 (900p/1350p)"},
                      {"5", L"×1,5 (1080p/1620p)"},
                      {"6", L"×2 (1440p/2160p)"},
                      {"7", L"×3 (2160p/3240p)"},
                      {"8", L"×4 (2880p/4320p)"}},
                     0, 0, 0, 1, L"", true, 0});
        // Eden's AudioEngine (settings_enums.h); both Eden's and Suyu's own
        // files hold 0. Older members of the family store a word here, which
        // Available turns away.
        v.push_back({"Audio.output_engine", ryucfg::kSonido, L"Salida de sonido", L"Sistema que reproduce el sonido.",
                     Kind::Choice, Type::Int,
                     {{"0", L"Automática"}, {"1", L"Cubeb"}, {"2", L"SDL"}, {"3", L"Sin sonido"}}, 0, 0, 0, 1, L"", true, 0});
        v.push_back({"Audio.audio_muted", ryucfg::kSonido, L"Silenciar", L"Los juegos no suenan.", Kind::Toggle,
                     Type::Bool, {}, 0, 1, 1, 1, L"", true, 0});
        // What the console calls the television's sound. Eden's AudioMode
        // (settings_enums.h: Mono, Stereo, Surround, numbered from 0) under
        // the name "sound_index" (settings.h, default Stereo). It is one of
        // the console's own settings, so it sits in [System], not in [Audio]:
        // the file Eden v0.2.1 writes holds "sound_index=1" there.
        v.push_back({"System.sound_index", ryucfg::kSonido, L"Modo de sonido",
                     L"Canales con los que el juego produce su sonido.", Kind::Choice, Type::Int,
                     {{"1", L"Estéreo"}, {"0", L"Mono"}, {"2", L"Envolvente"}}, 0, 0, 0, 1, L"", true, 0});
        // (Off in the file Eden v0.2.1 writes; Suyu's file has no such key,
        // where Available hides it.)
        v.push_back({"Services.airplane_mode", ryucfg::kInternet, L"Modo avión",
                     L"Activado: los juegos no pueden conectarse a la red.", Kind::Toggle, Type::Bool, {}, 0, 1, 1, 1,
                     L"", true, 0});
    } else {
        // yuzu's own list as remembered (0.5, 0.75, 1, 1.5, 2, 3, 4...). Not
        // checked against Citron, Sudachi or yuzu (the owner's Citron file
        // holds a 4 he chose himself, which does not say what 4 means), so
        // it is not offered for them yet.
        v.push_back({"Renderer.resolution_setup", ryucfg::kPantallas, L"Resolución",
                     L"Multiplica la resolución a la que se dibuja el juego.", Kind::Choice, Type::Int,
                     {{"0", L"×0,5 (360p/540p)"},
                      {"1", L"×0,75 (540p/810p)"},
                      {"2", L"Original (720p/1080p)"},
                      {"3", L"×1,5 (1080p/1620p)"},
                      {"4", L"×2 (1440p/2160p)"},
                      {"5", L"×3 (2160p/3240p)"},
                      {"6", L"×4 (2880p/4320p)"}},
                     0, 0, 0, 1, L"", false, 0});
        v.push_back({"Audio.audio_muted", ryucfg::kSonido, L"Silenciar", L"Los juegos no suenan.", Kind::Toggle,
                     Type::Bool, {}, 0, 1, 1, 1, L"", true, 0});
        // The owner's Citron file has the key too, holding 1 (its default),
        // which fits yuzu's Mono/Stereo/Surround but does not prove the
        // other two numbers: not offered there until its source is read.
        v.push_back({"System.sound_index", ryucfg::kSonido, L"Modo de sonido",
                     L"Canales con los que el juego produce su sonido.", Kind::Choice, Type::Int,
                     {{"1", L"Estéreo"}, {"0", L"Mono"}, {"2", L"Envolvente"}}, 0, 0, 0, 1, L"", false, 0});
    }
    return v;
}

const Setting* Find(const std::string& emulatorId, const std::string& key) {
    for (const auto& s : Settings(emulatorId))
        if (s.key == key) return &s;
    return nullptr;
}

namespace {
const Setting* AnyCurated(const std::string& key) {
    return Find("eden", key);
}
} // namespace

bool Open(const std::wstring& iniPath) {
    return Load(iniPath, g);
}

bool Has(const std::string& key) {
    return g.open && g.keys.count(key) != 0;
}

std::string Get(const std::string& key) {
    if (!g.open) return {};
    auto it = g.keys.find(key);
    if (it == g.keys.end()) return {};
    return g.text.substr(it->second.value.from, it->second.value.to - it->second.value.from);
}

std::string Effective(const std::string& key, const std::string& fallback) {
    if (!g.open) return {};
    auto it = g.keys.find(key);
    if (it == g.keys.end()) return {};
    const Entry& e = it->second;
    if (!e.hasFlag || g.text.compare(e.flag.from, e.flag.to - e.flag.from, "false") != 0) return fallback;
    return Get(key);
}

bool Available(const Setting& s) {
    if (!s.verified || !Has(s.key)) return false;
    const std::string v = Get(s.key);
    return s.type == Type::Bool ? v == "true" || v == "false" : IsNumber(v);
}

bool SetMany(const std::vector<std::pair<std::string, std::string>>& changes, bool add) {
    if (!g.open) return false;
    File f;
    if (!Load(g.path, f)) return false;
    for (const auto& ch : changes) {
        auto it = f.keys.find(ch.first);
        std::string current =
            it == f.keys.end() ? std::string() : f.text.substr(it->second.value.from, it->second.value.to - it->second.value.from);
        if (!Allowed(AnyCurated(ch.first), ch.second, current)) {
            g = std::move(f);
            return false;
        }
    }
    const std::string before = f.text;
    if (!Apply(f, changes, add)) {
        Load(g.path, g);
        return false;
    }
    if (f.text == before) {
        g = std::move(f);
        return true;
    }
    const std::wstring backup = f.path + L".dockside-backup";
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(f.path.c_str(), backup.c_str(), TRUE) && GetLastError() != ERROR_FILE_EXISTS) {
        Load(g.path, g);
        return false; // no backup, no write
    }
    bool ok = WriteFileAtomic(f.path, f.text);
    const std::wstring path = f.path;
    Load(path, g);
    return ok && g.open;
}

bool Set(const std::string& key, const std::string& value, bool add) {
    return SetMany({{key, value}}, add);
}

bool Seed(const std::wstring& iniPath, const std::vector<std::pair<std::string, std::string>>& values) {
    if (GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return Open(iniPath) && SetMany(values, true);
    File f;
    f.path = iniPath;
    f.open = true;
    if (!Apply(f, values, true)) return false;
    return WriteFileAtomic(iniPath, f.text) && Open(iniPath);
}

} // namespace yuzucfg
