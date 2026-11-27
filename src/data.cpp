// Settings, emulator detection and the game library.
#include "app.h"
#include "content.h"
#include "nsw.h"
#include "padcfg.h"
#include "padwish.h"
#include "ryucfg.h"
#include "yuzucfg.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <set>

Config cfg;

// The program names differ by system: Windows builds end in .exe, the Linux
// and macOS ones have bare names.
#ifdef _WIN32
#define EXE(name) L##name L".exe"
#else
#define EXE(name) L##name
#endif
#if defined(__ANDROID__)
// [android] On Android the emulators are apps. `exes` lists the package names
// an emulator is published under (its build flavours); it counts as found when
// one of them is installed, and cfg.emulators holds that package name where
// the desktop builds hold a program path. The package manager is asked, and
// the activity each one is started through is kept, in
// port/android/main_android.cpp. NSW-Home keeps one private copy of the
// console keys whatever the emulator, hence the same dataDir and keysSub for
// all. Their family is 'a': an app, whose settings, users, firmware and window
// are its own and out of reach (see NoFiles below).
const std::vector<EmuDef> kEmus = {
    {"eden", L"Eden",
     {L"dev.eden.eden_emulator", L"dev.legacy.eden_emulator", L"dev.eden.eden_emulator.nightly",
      L"dev.legacy.eden_emulator.nightly", L"dev.eden.eden_nightly", L"com.miHoYo.Yuanshen", L"com.miHoYo.Yuanshen.nightly"},
     L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
    {"suyu", L"Suyu", {L"org.suyu.suyu_emu", L"org.suyu.suyu_legacy", L"com.miHoYo.Yuanshen"}, L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
    {"lemon", L"Lemon",
     {L"dev.lemon.lemon_emulator", L"dev.lemon.lemon_emulator.lite", L"dev.legacy.lemon_emulator", L"com.antutu.ABenchMark",
      L"com.miHoYo.Yuanshen"},
     L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
    {"kenjinx", L"Kenji-NX", {L"org.kenjinx.android", L"com.garena.game.codm"}, L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
    {"citron", L"Citron", {L"org.citron.citron_emu", L"org.citron.citron_emu.ea"}, L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
    {"sudachi", L"Sudachi", {L"org.sudachi.sudachi_emu", L"org.sudachi.sudachi_emu.ea"}, L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
    {"yuzu", L"yuzu", {L"org.yuzu.yuzu_emu", L"org.yuzu.yuzu_emu.ea"}, L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
    {"skyline", L"Skyline", {L"skyline.emu"}, L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
    {"strato", L"Strato", {L"org.stratoemu.strato", L"org.stratoemu.strato.dev"}, L"", L"nswhome", L"keys", nullptr, nullptr, 'a'},
};
#else
// Three programs are called Ryujinx: the forks have no file name of their own
// and are told apart by what is inside the file (IdentifyExe), so they list
// none here. Citron Neo runs under Citron's name and folder, Torzu under yuzu's.
#define RYU_FORK L"--hide-updates --fullscreen %s", L"Ryujinx", L"system", L"--profile %s ", L"NativeWindow", 'r'
#define YUZU(dir) L"-f -g %s", L##dir, L"keys", nullptr, L">Qt", 'y'
const std::vector<EmuDef> kEmus = {
    {"ryujinx", L"Ryujinx", {EXE("Ryujinx"), EXE("Ryujinx.Ava")}, L"--fullscreen %s", L"Ryujinx", L"system", L"--profile %s ", L"NativeWindow", 'r'},
    {"ryubing", L"Ryubing", {}, RYU_FORK},
    {"kenjinx", L"Kenji-NX", {}, RYU_FORK},
    {"eden", L"Eden", {EXE("eden")}, YUZU("eden")},
    // -gamer: Suyu 0.1.4 otherwise opens a window asking which of its three
    // interface modes to use, at every start until it has been answered once
    // (src/suyu/main.cpp; the answer is kept in the registry, not in its folder).
    {"suyu", L"Suyu", {EXE("suyu")}, L"-gamer -f -g %s", L"suyu", L"keys", nullptr, L">Qt", 'y'},
    {"citron", L"Citron", {EXE("citron")}, YUZU("citron")},
    {"sudachi", L"Sudachi", {EXE("sudachi")}, YUZU("sudachi")},
    {"yuzu", L"yuzu", {EXE("yuzu")}, YUZU("yuzu")},
};
#undef RYU_FORK
#undef YUZU
#endif
#undef EXE

const EmuDef* FindEmu(const std::string& id) {
    for (const auto& e : kEmus)
        if (id == e.id) return &e;
    return nullptr;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// ---- minimal JSON ---------------------------------------------------------

namespace {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const char* key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string s(const char* key, const std::string& def = {}) const {
        const Json* j = get(key);
        return j && j->type == Str ? j->str : def;
    }
    bool boolean(const char* key, bool def) const {
        const Json* j = get(key);
        return j && j->type == Bool ? j->b : def;
    }
    double number(const char* key, double def) const {
        const Json* j = get(key);
        return j && j->type == Num ? j->num : def;
    }
};

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4 && p < end; i++, p++) {
            char c = *p;
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 0);
        }
        return v;
    }
    std::string string() {
        std::string out;
        p++; // opening quote
        while (p < end && *p != '"') {
            if (*p != '\\') {
                out += *p++;
                continue;
            }
            if (++p >= end) break;
            char c = *p++;
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: out += c;
            }
        }
        if (p < end) p++;
        else ok = false;
        return out;
    }
    Json value(int depth = 0) {
        Json j;
        ws();
        if (p >= end || depth > 64) {
            ok = false;
            return j;
        }
        if (*p == '{') {
            j.type = Json::Obj;
            p++;
            ws();
            while (ok && p < end && *p != '}') {
                ws();
                if (*p != '"') { ok = false; break; }
                std::string k = string();
                ws();
                if (p >= end || *p != ':') { ok = false; break; }
                p++;
                j.obj.emplace_back(std::move(k), value(depth + 1));
                ws();
                if (p < end && *p == ',') p++;
                ws();
            }
            if (p < end) p++;
        } else if (*p == '[') {
            j.type = Json::Arr;
            p++;
            ws();
            while (ok && p < end && *p != ']') {
                j.arr.push_back(value(depth + 1));
                ws();
                if (p < end && *p == ',') p++;
                ws();
            }
            if (p < end) p++;
        } else if (*p == '"') {
            j.type = Json::Str;
            j.str = string();
        } else if (!strncmp(p, "true", (std::min)(static_cast<size_t>(4), static_cast<size_t>(end - p)))) {
            j.type = Json::Bool; j.b = true; p += 4;
        } else if (!strncmp(p, "false", (std::min)(static_cast<size_t>(5), static_cast<size_t>(end - p)))) {
            j.type = Json::Bool; p += 5;
        } else if (!strncmp(p, "null", (std::min)(static_cast<size_t>(4), static_cast<size_t>(end - p)))) {
            p += 4;
        } else {
            char* e = nullptr;
            j.type = Json::Num;
            j.num = strtod(p, &e);
            if (e == p) ok = false;
            p = e;
        }
        return j;
    }
};

bool ReadAll(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    out.resize(got);
    return true;
}

bool WriteAll(const std::wstring& path, const void* data, size_t n) {
    // Write to a temp file first so a crash never leaves a half-written file.
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool LoadJson(const std::wstring& path, Json& out) {
    std::string text;
    if (!ReadAll(path, text)) return false;
    const char* p = text.data();
    const char* end = p + text.size();
    if (text.size() >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
    Parser ps{p, end};
    out = ps.value();
    return ps.ok;
}

std::string Quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}
std::string Quote(const std::wstring& s) { return Quote(Narrow(s)); }

bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    if (n > 0 && n < ARRAYSIZE(buf)) return std::wstring(buf, n);
    // The program's former name, DOCKSIDE_* (historical name): scripts and
    // shells that still set the old variables must keep pointing it where they
    // mean, or a test run would read and write the real data folder.
    static const wchar_t kNew[] = L"NSWHOME_";
    if (!wcsncmp(name, kNew, ARRAYSIZE(kNew) - 1)) {
        const std::wstring old = std::wstring(L"DOCKSIDE_") + (name + ARRAYSIZE(kNew) - 1); // historical name
        n = GetEnvironmentVariableW(old.c_str(), buf, ARRAYSIZE(buf));
        if (n > 0 && n < ARRAYSIZE(buf)) return std::wstring(buf, n);
    }
    return std::wstring();
}

std::wstring Parent(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

struct DirEntry {
    std::wstring name;
    bool dir;
    uint64_t size;
    uint64_t mtime;
};

std::vector<DirEntry> ListDir(const std::wstring& dir) {
    std::vector<DirEntry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back({fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow,
                       (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                           fd.ftLastWriteTime.dwLowDateTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// Where an emulator keeps its keys, firmware, profiles and saves. A portable
// install keeps them beside its executable ("portable" for Ryujinx, "user"
// for the yuzu family) and never looks in %APPDATA%, so neither may we.
std::wstring EmuDataDir(const EmuDef& e) {
    auto it = cfg.emulators.find(e.id);
    if (it != cfg.emulators.end()) {
        std::wstring beside = Parent(it->second) + (e.family == 'r' ? L"\\portable" : L"\\user");
        if (Exists(beside)) return beside;
    }
    // NSWHOME_EMUDATA stands in for %APPDATA% so that test runs can write
    // profiles and keys without touching the real emulators' folders.
    std::wstring base = Env(L"NSWHOME_EMUDATA");
#if defined(__linux__)
    // On Linux only Ryujinx lives in ~/.config; the yuzu family keeps its keys
    // and screenshots under ~/.local/share/<name>.
    if (base.empty() && e.family != 'r') return Env(L"LOCALAPPDATA") + L"\\" + e.dataDir;
#endif
    return (base.empty() ? Env(L"APPDATA") : base) + L"\\" + e.dataDir;
}

// The emulator keeps its data beside its program, shared with nothing else.
bool OwnFolder(const EmuDef& e) {
    auto it = cfg.emulators.find(e.id);
    return it != cfg.emulators.end() && Exists(Parent(it->second) + (e.family == 'r' ? L"\\portable" : L"\\user"));
}

// Where NSW-Home writes into an emulator's data (profiles, keys, firmware) and
// reads its profiles and settings back. A test run (NSWHOME_DATA set) must
// never touch a real emulator's files, so there this is <data>\<emulator id>,
// which only exists when a copy has been put there (NSWHOME_EMUDATA, a
// stand-in for the emulators' shared folders, is already safe and wins).
// NSWHOME_LIVE lifts that for emulators with a folder of their own: the
// launch test needs NSW-Home to prepare those for real.
std::wstring EmuFiles(const EmuDef& e) {
    std::wstring test = Env(L"NSWHOME_DATA");
    if (!test.empty() && Env(L"NSWHOME_EMUDATA").empty() && !(OwnFolder(e) && !Env(L"NSWHOME_LIVE").empty()))
        return test + L"\\" + Widen(e.id);
    return EmuDataDir(e);
}

// The emulator games start with; failing that any configured one.
const EmuDef& MainEmu() {
    const EmuDef* e = FindEmu(cfg.defaultEmu);
    if (e && cfg.emulators.count(e->id)) return *e;
    for (const auto& d : kEmus)
        if (cfg.emulators.count(d.id)) return d;
    return kEmus[0];
}

// The same, for what only the Ryujinx family has (its game list's folders and
// play times): the main emulator when it is one of them.
const EmuDef& RyuEmu() {
    const EmuDef& m = MainEmu();
    if (m.family == 'r') return m;
    for (const auto& d : kEmus)
        if (d.family == 'r' && cfg.emulators.count(d.id)) return d;
    return kEmus[0];
}

// [android] An emulator that is an installed app has no files NSW-Home can
// read or write: nothing of its to import users from, mirror users into,
// prepare before a start or place a window for. (Its dataDir is NSW-Home's own
// private folder, which holds the copy of the console keys and nothing else.)
bool NoFiles(const EmuDef& e) {
    return e.family == 'a';
}

std::wstring FirmwareIn(const std::wstring& dataDir, const EmuDef& e) {
    return dataDir + (e.family == 'r' ? L"\\bis\\system\\Contents\\registered" : L"\\nand\\system\\Contents\\registered");
}

// ---- which emulator a program is ---------------------------------------------

std::string Utf16(const char* s) {
    std::string o;
    for (; *s; s++) {
        o += *s;
        o += '\0';
    }
    return o;
}

struct Ident {
    std::string id; // empty: not a known emulator
    std::wstring version;
};

// Ryujinx, Ryubing and Kenji-NX ship the same file name with the same product
// name and description in its version block; what differs is the version
// number (1.1.x, 1.2/1.3.x, 2.x) and the address of the project each build
// carries for its "about" and update pages. The address is the surer of the
// two, so it decides, and the number only when no address is found.
Ident IdentifyExe(const std::wstring& exe) {
    static std::map<std::wstring, Ident> cache;
    const std::wstring name = exe.substr(exe.find_last_of(L"\\/") + 1);
    const EmuDef* def = nullptr;
    for (const auto& e : kEmus)
        for (auto n : e.exes)
            if (!def && !_wcsicmp(name.c_str(), n)) def = &e;
    if (!def) return {};
    Ident out{def->id, {}};
    if (def->family != 'r') return out;

    // The file is 60-70 MB: read it once per build of it.
    std::wstring key = Lower(exe);
    for (const auto& e : ListDir(Parent(exe)))
        if (!e.dir && !_wcsicmp(e.name.c_str(), name.c_str()))
            key += L"|" + std::to_wstring(e.size) + L"|" + std::to_wstring(e.mtime);
    auto hit = cache.find(key);
    if (hit != cache.end()) return hit->second;

    std::string bin;
    if (!ReadAll(exe, bin)) return out;
    int major = 0, minor = 0;
    // The program's own version block is in its resource section; the same
    // words occur again in every library packed into the file, so the section
    // is located from the file's headers first.
    auto u16 = [&](size_t o) {
        return o + 2 <= bin.size() ? static_cast<unsigned char>(bin[o]) | static_cast<unsigned char>(bin[o + 1]) << 8 : 0;
    };
    auto u32 = [&](size_t o) { return static_cast<size_t>(u16(o)) | static_cast<size_t>(u16(o + 2)) << 16; };
    size_t from = bin.size(), to = bin.size();
    const size_t pe = u32(0x3C), opt = pe + 24;
    if (u32(pe) == 0x4550) { // "PE\0\0"
        const size_t dirs = opt + (u16(opt) == 0x20B ? 112 : 96); // 64-bit or 32-bit header
        const size_t rva = u32(dirs + 16), size = u32(dirs + 20);  // third entry: resources
        for (int i = 0; i < u16(pe + 6); i++) {
            const size_t s = opt + u16(pe + 20) + static_cast<size_t>(i) * 40;
            const size_t va = u32(s + 12), raw = u32(s + 16), ptr = u32(s + 20);
            if (size == 0 || rva < va || rva >= va + raw) continue;
            // Resource tree: type 16 (version) -> its first name -> its first
            // language -> where the block is. (The section also holds another
            // program's version text as plain data, so a search of the whole
            // section finds the wrong one.)
            const size_t base = ptr + (rva - va);
            auto child = [&](size_t dir, int wantId) -> size_t {
                const int n = u16(dir + 12) + u16(dir + 14);
                for (int k = 0; k < n && k < 4096; k++)
                    if (wantId < 0 || u32(dir + 16 + 8 * static_cast<size_t>(k)) == static_cast<size_t>(wantId))
                        return base + (u32(dir + 20 + 8 * static_cast<size_t>(k)) & 0x7FFFFFFF);
                return 0;
            };
            size_t entry = child(base, 16);
            if (entry) entry = child(entry, -1);
            if (entry) entry = child(entry, -1);
            if (entry && u32(entry) >= va) {
                from = ptr + (u32(entry) - va);
                to = (std::min)(from + u32(entry + 4), bin.size());
            }
        }
    }
    size_t at = from < to ? bin.find(Utf16("ProductVersion"), from) : std::string::npos;
    if (at != std::string::npos && at < to) {
        at += 28;
        while (at + 1 < bin.size() && !bin[at] && !bin[at + 1]) at += 2; // terminator and padding
        for (; at + 1 < bin.size() && !bin[at + 1] && (isdigit(static_cast<unsigned char>(bin[at])) || bin[at] == '.'); at += 2)
            out.version += static_cast<wchar_t>(bin[at]);
        swscanf_s(out.version.c_str(), L"%d.%d", &major, &minor);
    }
    if (bin.find(Utf16("kenji-nx/ryujinx")) != std::string::npos) out.id = "kenjinx";
    else if (bin.find(Utf16("ryubing/ryujinx")) != std::string::npos) out.id = "ryubing";
    else if (major >= 2) out.id = "kenjinx";
    else if (major == 1 && minor >= 2) out.id = "ryubing";
    cache[key] = out;
    return out;
}

// Every program under `root` that carries an emulator's name. Releases unpack
// into a folder of their own, often with a version folder inside and another
// inside that, so a folder named after an emulator is looked into further
// than the search would otherwise go.
void FindExes(const std::wstring& root, int depth, std::vector<std::wstring>& out) {
    for (const auto& e : ListDir(root)) {
        if (!e.dir) {
            bool known = false;
            for (const auto& d : kEmus)
                for (auto n : d.exes) known = known || !_wcsicmp(e.name.c_str(), n);
            if (known) out.push_back(root + L"\\" + e.name);
            continue;
        }
        if (e.name[0] == L'.' || e.name == L"node_modules") continue;
        int d = depth - 1;
        const std::wstring low = Lower(e.name);
        for (const wchar_t* hint : {L"ryu", L"kenji", L"eden", L"suyu", L"citron", L"sudachi", L"yuzu", L"torzu", L"emul"})
            if (low.find(hint) != std::wstring::npos) d = (std::max)(d, 2);
        if (d >= 0) FindExes(root + L"\\" + e.name, d, out);
    }
}

// Emulators tend to live near the ROM folder or in the usual app folders, so
// those are searched instead of the whole disk.
void DetectEmulators() {
#if defined(__ANDROID__)
    // [android] Apps are not found by walking folders: the package manager is
    // asked instead (RefreshEmulators in port/android/main_android.cpp).
    return;
#endif
    std::vector<std::pair<std::wstring, int>> roots;
    for (const auto& dir : cfg.gameDirs) {
        std::wstring cur = dir;
        for (int i = 0; i < 3; i++) {
            cur = Parent(cur);
            if (cur.size() <= 3) break; // drive root: too broad to walk
            roots.push_back({cur, 3});
        }
    }
    roots.push_back({Env(L"LOCALAPPDATA"), 3});
    roots.push_back({Env(L"ProgramFiles"), 2});
    roots.push_back({Env(L"USERPROFILE"), 2});
    for (const auto& r : roots) {
        if (r.first.empty()) continue;
        std::vector<std::wstring> found;
        FindExes(r.first, r.second, found);
        for (const auto& exe : found) {
            const std::string id = IdentifyExe(exe).id;
            if (!id.empty() && !cfg.emulators.count(id)) cfg.emulators[id] = exe;
        }
    }
    // Prefer the first known emulator in table order, not alphabetical order.
    if (cfg.defaultEmu.empty())
        for (const auto& emu : kEmus)
            if (cfg.emulators.count(emu.id)) {
                cfg.defaultEmu = emu.id;
                break;
            }
}

// Seeds a fresh install from what Ryujinx already knows: game folders, the
// profile name and its picture.
void SeedGameDirs() {
    Json j;
    if (!LoadJson(EmuDataDir(RyuEmu()) + L"\\Config.json", j)) return;
    if (const Json* dirs = j.get("game_dirs"))
        for (const auto& d : dirs->arr) {
            if (d.type != Json::Str || !Exists(Widen(d.str))) continue;
            bool have = false;
            for (const auto& g : cfg.gameDirs) have = have || !_wcsicmp(g.c_str(), Widen(d.str).c_str());
            if (!have) cfg.gameDirs.push_back(Widen(d.str));
        }
}

void FirstRun() {
    SeedGameDirs();
    DetectEmulators();
    // A portable Ryujinx is only known once it has been found.
    if (cfg.gameDirs.empty()) SeedGameDirs();
}

// ---- users ----------------------------------------------------------------

// The emulator's list of users. The Ryujinx family keeps a JSON file. The yuzu
// family keeps the console's own binary account file (Eden,
// src/core/hle/service/acc/profile_manager.cpp; "avators" is its spelling).
// Under `base`, which is EmuFiles(e) for NSW-Home's own reading and writing
// (a test run never touches the emulator's files) and EmuDataDir(e) for what
// the emulator itself will read.
std::wstring ProfilesIn(const std::wstring& base, const EmuDef& e) {
    return base + (e.family == 'r' ? L"\\system\\Profiles.json"
                                   : L"\\nand\\system\\save\\8000000000000010\\su\\avators\\profiles.dat");
}
std::wstring ProfilesPath(const EmuDef& e) {
    return ProfilesIn(EmuFiles(e), e);
}

// profiles.dat: 0x10 unused bytes, then eight records of 0xC8 bytes:
//   0x00 the user's 16-byte id   0x10 the same again   0x20 a time stamp
//   0x28 the name, 32 bytes of UTF-8 padded with zeros   0x48 0x80 bytes of
//   extra data (zeros are what the emulator itself writes for a new user).
// A record whose id is all zeros is empty; the emulator numbers its users by
// the used records in order, which is the index its -u option takes.
constexpr size_t kYuzuHead = 0x10, kYuzuUser = 0xC8, kYuzuUsers = 8, kYuzuName = 0x28, kYuzuNameLen = 32;
constexpr size_t kYuzuSize = kYuzuHead + kYuzuUsers * kYuzuUser;

// NSW-Home's user id is 32 hex digits; in the yuzu family those are the id's
// 16 bytes in order (what its source calls the raw string).
bool IdBytes(const std::string& id, unsigned char out[16]) {
    if (id.size() != 32) return false;
    for (int i = 0; i < 16; i++) {
        unsigned v = 0;
        if (sscanf_s(id.c_str() + i * 2, "%2x", &v) != 1) return false;
        out[i] = static_cast<unsigned char>(v);
    }
    return true;
}

bool YuzuUsed(const std::string& data, size_t slot) {
    for (size_t i = 0; i < 16; i++)
        if (data[kYuzuHead + slot * kYuzuUser + i]) return true;
    return false;
}

std::string YuzuId(const std::string& data, size_t slot) {
    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", static_cast<unsigned char>(data[kYuzuHead + slot * kYuzuUser + i]));
    return hex;
}

// A user's picture sits beside the list, named by the id written 8-4-4-4-12.
std::wstring YuzuPicture(const std::wstring& profiles, const std::string& id) {
    return Parent(profiles) + L"\\" + Widen(id.substr(0, 8) + "-" + id.substr(8, 4) + "-" + id.substr(12, 4) + "-" +
                                            id.substr(16, 4) + "-" + id.substr(20)) + L".jpg";
}

// Which of the emulator's users (in its own numbering) has this id; -1 if none.
int YuzuUserIndex(const EmuDef& e, const std::string& id) {
    std::string data;
    if (!ReadAll(ProfilesIn(EmuDataDir(e), e), data) || data.size() < kYuzuSize) return -1;
    int index = 0;
    for (size_t slot = 0; slot < kYuzuUsers; slot++) {
        if (!YuzuUsed(data, slot)) continue;
        if (YuzuId(data, slot) == id) return index;
        index++;
    }
    return -1;
}

std::string NewUserId() {
    BYTE raw[16] = {};
    BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", raw[i]);
    return hex;
}

// A user's friend code: twelve decimal digits from the system's random source,
// made once when the user is made and never changed. No emulator's profile
// carries it; src\friends.cpp derives from it where that user's status is published.
std::string NewFriendCode() {
    BYTE raw[12] = {};
    BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string code;
    // (250 is the largest multiple of ten a byte reaches: bytes past it would favour the low digits.)
    for (BYTE b : raw) code += static_cast<char>('0' + (b < 250 ? b : b - 250) % 10);
    return code;
}

// The ready-made pictures: a disc of one of these colours with the name's
// initial on it. NSW-Home offers only pictures of its own making.
const uint32_t kAvatarPalette[] = {0xE60012, 0x0AB9E6, 0x2DB84B, 0xF5A200, 0x8C46D2, 0xE6468C, 0x1E6EE6, 0x5A5A5A};

bool DefaultAvatar(const User& u, size_t index) {
    // (A user without a nickname gets the disc alone.)
    std::wstring initial = u.name.empty() ? L"" : std::wstring(1, static_cast<wchar_t>(towupper(u.name[0])));
    return gfx::MakeAvatar(L"", gfx::rgb(kAvatarPalette[index % ARRAYSIZE(kAvatarPalette)]), initial,
                           AvatarPath(u.id));
}

// ---- which of an emulator's profiles a user is --------------------------------
//
// The rule. An NSW-Home user has an id of its own; play time and the picture
// are kept under it. In each emulator the user is exactly one profile:
//   - the one named in user.emu[<emulator id>], when there is such an entry: a
//     profile the emulator already had, which the user stands for. NSW-Home
//     never writes to such a profile: its name, its picture and its saves stay
//     the emulator's, whatever the user is called here (and a user may have
//     no name at all);
//   - otherwise the profile whose id is the user's own. That one is NSW-Home's
//     to keep: it is made when missing (named after the user, or plainly
//     "Jugador" for a user without a name, since an emulator's profile needs
//     one) and follows the user's nickname and picture; while the user has no
//     name, the name it already has is kept.
// Entries are made in two ways only: for the first user, when the
// configuration is made from nothing or an emulator is added (LinkFirstUser:
// the profile that emulator was last used with, so the saves made before
// NSW-Home stay where the player finds them), and by AdoptProfile, which is
// how an emulator's other profiles become users: when asked for, never by
// themselves. Users of an install older than this rule have no entries and go
// on being the profile with their own id, kept up to date as before.
// A profile no user is, is left exactly as it is; so is an entry's profile
// when its user is removed here.

const std::string& ProfileOf(const User& u, const EmuDef& e) {
    auto it = u.emu.find(e.id);
    return it == u.emu.end() ? u.id : it->second;
}

bool Adopted(const User& u, const EmuDef& e) {
    return u.emu.count(e.id) != 0;
}

std::string FromBase64(const std::string& text) {
    DWORD len = 0;
    if (text.empty() || !CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr,
                                              &len, nullptr, nullptr))
        return {};
    std::string bin(len, 0);
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(bin.data()), &len, nullptr, nullptr))
        return {};
    bin.resize(len);
    return bin;
}

struct EmuProfile {
    std::string id;
    std::wstring name;
    std::string picture; // the image file's bytes, when asked for and there is one
};

// The emulator's own list, in its order, from the files under `base` (see
// ProfilesIn). `current`: which of them it was last used with (the Ryujinx
// family records the profile, the yuzu family a position in its settings).
std::vector<EmuProfile> ReadProfiles(const EmuDef& e, const std::wstring& base, int* current = nullptr,
                                     bool pictures = false) {
    std::vector<EmuProfile> out;
    if (current) *current = 0;
    if (NoFiles(e)) return out; // [android]
    const std::wstring path = ProfilesIn(base, e);
    if (e.family != 'r') {
        std::string data, ini;
        if (!ReadAll(path, data) || data.size() < kYuzuSize) return out;
        for (size_t slot = 0; slot < kYuzuUsers; slot++) {
            if (!YuzuUsed(data, slot)) continue;
            const char* name = data.data() + kYuzuHead + slot * kYuzuUser + kYuzuName;
            EmuProfile p{YuzuId(data, slot), Widen(std::string(name, strnlen(name, kYuzuNameLen))), {}};
            if (pictures) ReadAll(YuzuPicture(path, p.id), p.picture);
            out.push_back(std::move(p));
        }
        size_t at = ReadAll(base + L"\\config\\qt-config.ini", ini) ? ini.find("\ncurrent_user=") : std::string::npos;
        const int index = at == std::string::npos ? 0 : atoi(ini.c_str() + at + 14);
        if (current && index > 0 && index < static_cast<int>(out.size())) *current = index;
        return out;
    }
    Json prof;
    if (!LoadJson(path, prof)) return out;
    if (const Json* list = prof.get("profiles"))
        for (const Json& p : list->arr) {
            if (p.s("user_id").size() != 32) continue;
            if (current && p.s("user_id") == prof.s("last_opened")) *current = static_cast<int>(out.size());
            out.push_back({p.s("user_id"), Widen(p.s("name")), pictures ? FromBase64(p.s("image")) : std::string()});
        }
    return out;
}

// The first user becomes the profile this emulator was last used with, unless
// the user already is one of its profiles or somebody else is that one.
void LinkFirstUser(const EmuDef& e) {
    if (cfg.users.empty() || cfg.users[0].emu.count(e.id)) return;
    // Two emulators on one data folder share the list: who is who is the same.
    for (const auto& o : kEmus) {
        if (!strcmp(o.id, e.id) || !cfg.emulators.count(o.id) || _wcsicmp(ProfilesPath(o).c_str(), ProfilesPath(e).c_str()))
            continue;
        bool any = false;
        for (User& u : cfg.users)
            if (u.emu.count(o.id)) {
                const std::string same = u.emu[o.id];
                u.emu[e.id] = same;
                any = true;
            }
        if (any) return;
    }
    int current = 0;
    const std::vector<EmuProfile> list = ReadProfiles(e, EmuFiles(e), &current);
    if (list.empty()) return;
    for (size_t i = 1; i < cfg.users.size(); i++)
        if (ProfileOf(cfg.users[i], e) == list[current].id) return;
    cfg.users[0].emu[e.id] = list[current].id;
}

// What the profile NSW-Home keeps for `u` is called in an emulator's list:
// `had` is its name there so far, `others` the names of the list's other
// profiles (the Ryujinx family is told who plays by name, so no two may share one).
std::wstring ProfileName(const User& u, const std::wstring& had, const std::vector<std::wstring>& others) {
    if (u.name.empty() && !had.empty()) return had;
    const std::wstring base = u.name.empty() ? L"Jugador" : u.name;
    std::wstring name = base;
    for (int n = 2;; n++) {
        bool taken = false;
        for (const auto& o : others) taken = taken || !_wcsicmp(o.c_str(), name.c_str());
        if (!taken) return name;
        name = base + L" " + std::to_wstring(n);
    }
}

// Makes a Ryujinx-family profile list hold our users (see the rule above).
// Ryujinx keys save data by user_id, so a profile per user is what gives each
// one separate saves. Profiles the emulator has and no user is are kept as
// they are; `removed` is the profile NSW-Home kept for a user just deleted
// here, which goes. `onlyIfNeeded`: leave the file alone when it already has
// every user under the right name (the check before a launch). It is left
// alone in any case when every user stands for a profile that is there.
bool SyncRyu(const EmuDef& e, const std::string& removed, bool onlyIfNeeded) {
    const std::wstring path = ProfilesPath(e);
    if (!Exists(Parent(path))) return true; // the emulator is not set up: nothing to mirror
    Json old;
    bool hadFile = LoadJson(path, old);
    const Json* list = old.get("profiles");
    auto previous = [&](const std::string& id) -> const Json* {
        const Json* hit = nullptr;
        if (list && !id.empty())
            for (const Json& p : list->arr)
                if (p.s("user_id") == id) hit = &p;
        return hit;
    };
    std::vector<std::wstring> made; // names given in this pass to profiles that were not there
    auto wanted = [&](const User& u, const Json* prev) {
        std::vector<std::wstring> others = made;
        if (list)
            for (const Json& p : list->arr)
                if (p.s("user_id") != ProfileOf(u, e)) others.push_back(Widen(p.s("name")));
        return ProfileName(u, prev ? Widen(prev->s("name")) : std::wstring(), others);
    };
    bool same = hadFile, untouched = hadFile && !previous(removed);
    for (const User& u : cfg.users) {
        const Json* prev = previous(ProfileOf(u, e));
        if (prev && Adopted(u, e)) continue;
        untouched = false;
        same = same && prev && Widen(prev->s("name")) == wanted(u, prev);
    }
    if (untouched || (onlyIfNeeded && same)) return true;
    // The first change keeps a copy of the file as the emulator left it.
    if (hadFile && !Exists(path + L".dockside-backup"))
        CopyFileW(path.c_str(), (path + L".dockside-backup").c_str(), TRUE);

    std::string o = "{\n  \"profiles\": [\n";
    auto emit = [&](const std::string& id, const std::string& name, bool current, const std::string& stamp,
                    const std::string& image) {
        if (o.back() == '}') o += ",\n";
        o += "    {\n      \"user_id\": " + Quote(id) + ",\n      \"name\": " + Quote(name) +
             ",\n      \"account_state\": " + (current ? "\"Open\"" : "\"Closed\"") +
             ",\n      \"online_play_state\": \"Closed\",\n      \"last_modified_timestamp\": " + stamp +
             ",\n      \"image\": " + Quote(image) + "\n    }";
    };
    auto stampOf = [](const Json* prev) {
        char stamp[32];
        snprintf(stamp, sizeof stamp, "%.0f",
                 prev ? prev->number("last_modified_timestamp", 0) : static_cast<double>(time(nullptr)));
        return std::string(stamp);
    };
    for (size_t i = 0; i < cfg.users.size(); i++) {
        const User& u = cfg.users[i];
        const Json* prev = previous(ProfileOf(u, e));
        const bool theirs = prev && Adopted(u, e); // the emulator's own: written back as it was
        std::string image = prev ? prev->s("image") : std::string();
        std::string jpeg;
        if (!theirs && ReadAll(AvatarPath(u.id), jpeg) && !jpeg.empty()) {
            DWORD len = 0;
            CryptBinaryToStringA(reinterpret_cast<const BYTE*>(jpeg.data()), static_cast<DWORD>(jpeg.size()),
                                 CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
            std::string b64(len, 0);
            if (CryptBinaryToStringA(reinterpret_cast<const BYTE*>(jpeg.data()), static_cast<DWORD>(jpeg.size()),
                                     CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len)) {
                b64.resize(len);
                image = b64;
            }
        }
        if (image.empty()) return false; // Ryujinx needs a picture for every profile
        const std::wstring name = theirs ? Widen(prev->s("name")) : wanted(u, prev);
        if (!prev) made.push_back(name);
        emit(ProfileOf(u, e), Narrow(name), static_cast<int>(i) == cfg.currentUser, stampOf(prev), image);
    }
    if (list)
        for (const Json& p : list->arr) {
            const std::string id = p.s("user_id");
            bool ours = id == removed || id.size() != 32 || p.s("image").empty();
            for (const User& u : cfg.users) ours = ours || ProfileOf(u, e) == id;
            if (!ours) emit(id, p.s("name"), false, stampOf(&p), p.s("image"));
        }
    o += "\n  ],\n  \"last_opened\": " + Quote(ProfileOf(CurUser(), e)) + "\n}";
    return WriteAll(path, o.data(), o.size());
}

// Makes a yuzu-family account file hold our users: each gets the record that
// already carries their profile's id, or the first empty one. Nothing else is
// touched: a record a user stands for (see the rule above) and the emulator's
// other users keep their records and their order (its own "current user"
// setting is a position in that order), and a user deleted here is left in
// place too, because Eden treats save data whose user is missing from this
// file as orphaned and stops at start-up to say so.
bool SyncYuzu(const EmuDef& e) {
    const std::wstring path = ProfilesPath(e);
    std::string data;
    const bool hadFile = ReadAll(path, data);
    if (hadFile && data.size() < kYuzuSize) return false; // not the file described above: leave it alone
    if (!hadFile) {
        if (!Exists(EmuFiles(e))) return true; // the emulator is not set up: nothing to mirror
        data.assign(kYuzuSize, '\0');
    }
    const std::string before = data;
    bool all = true;
    std::vector<const User*> kept; // the users whose record is NSW-Home's to write
    for (const User& u : cfg.users) {
        const std::string pid = ProfileOf(u, e);
        unsigned char id[16];
        if (!IdBytes(pid, id)) continue;
        int slot = -1, empty = -1;
        std::vector<std::wstring> others;
        for (size_t i = 0; i < kYuzuUsers; i++) {
            const char* name = data.data() + kYuzuHead + i * kYuzuUser + kYuzuName;
            if (!YuzuUsed(data, i)) empty = empty < 0 ? static_cast<int>(i) : empty;
            else if (YuzuId(data, i) == pid) slot = static_cast<int>(i);
            else others.push_back(Widen(std::string(name, strnlen(name, kYuzuNameLen))));
        }
        if (slot >= 0 && Adopted(u, e)) continue; // the emulator's own: not a byte of it is ours
        if (slot < 0 && empty < 0) { // the console has room for eight users
            all = false;
            continue;
        }
        kept.push_back(&u);
        char* rec = &data[kYuzuHead + (slot < 0 ? empty : slot) * kYuzuUser];
        if (slot < 0) {
            memset(rec, 0, kYuzuUser);
            memcpy(rec, id, 16);
            memcpy(rec + 16, id, 16);
        }
        const std::wstring had = Widen(std::string(rec + kYuzuName, strnlen(rec + kYuzuName, kYuzuNameLen)));
        std::string name = Narrow(ProfileName(u, had, others));
        if (name.size() > kYuzuNameLen) { // cut between characters, not inside one
            size_t k = kYuzuNameLen;
            while (k > 0 && (static_cast<unsigned char>(name[k]) & 0xC0) == 0x80) k--;
            name.resize(k);
        }
        char field[kYuzuNameLen] = {};
        memcpy(field, name.data(), name.size());
        memcpy(rec + kYuzuName, field, kYuzuNameLen);
    }
    if (data != before) {
        if (hadFile && !Exists(path + L".dockside-backup"))
            CopyFileW(path.c_str(), (path + L".dockside-backup").c_str(), TRUE);
        SHCreateDirectoryExW(nullptr, Parent(path).c_str(), nullptr);
        if (!WriteAll(path, data.data(), data.size())) return false;
    }
    if (Exists(Parent(path)))
        for (const User* u : kept) {
            std::string ours, theirs;
            const std::wstring picture = YuzuPicture(path, ProfileOf(*u, e));
            if (ReadAll(AvatarPath(u->id), ours) && !ours.empty() && (!ReadAll(picture, theirs) || theirs != ours))
                WriteAll(picture, ours.data(), ours.size());
        }
    return all;
}

// `removed`: a user just deleted here. The profile NSW-Home kept for them
// goes from the list; one of the emulator's own that they stood for stays.
bool SyncEmu(const EmuDef& e, const User* removed = nullptr, bool onlyIfNeeded = false) {
    if (NoFiles(e)) return true; // [android]
    const std::string gone = removed && !Adopted(*removed, e) ? removed->id : std::string();
    return e.family == 'r' ? SyncRyu(e, gone, onlyIfNeeded) : SyncYuzu(e);
}

// Every configured emulator gets the users; two that share a data folder
// (Ryujinx and a fork of it, both in the user's profile) share the file.
bool SyncProfiles(const User* removed = nullptr) {
    bool ok = true;
    std::set<std::wstring> done;
    for (const auto& e : kEmus)
        if (cfg.emulators.count(e.id) && done.insert(Lower(ProfilesPath(e))).second) ok = SyncEmu(e, removed) && ok;
    // With no emulator yet, the usual place still gets them (as it always did).
    if (cfg.emulators.empty()) ok = SyncEmu(kEmus[0], removed);
    return ok;
}

// Moves each configured program to the entry it really is (a Ryubing that was
// set as "Ryujinx", a Suyu set as "yuzu"). True if anything moved.
bool ReIdentify() {
    bool moved = false;
    const std::map<std::string, std::wstring> was = cfg.emulators;
    for (const auto& kv : was) {
        const std::string id = IdentifyExe(kv.second).id;
        if (id.empty() || id == kv.first) continue;
        cfg.emulators.erase(kv.first);
        if (!cfg.emulators.count(id)) cfg.emulators[id] = kv.second;
        if (cfg.defaultEmu == kv.first) cfg.defaultEmu = id;
        for (User& u : cfg.users) // who each user is there goes with it
            if (u.emu.count(kv.first)) {
                const std::string profile = u.emu[kv.first];
                u.emu.erase(kv.first);
                u.emu[id] = profile;
            }
        for (auto& pg : cfg.perGame)
            if (pg.second == kv.first) pg.second = id;
        moved = true;
    }
    return moved;
}

bool NameTaken(const std::wstring& name, int except) {
    if (name.empty()) return false; // having no nickname is not a nickname
    for (int i = 0; i < static_cast<int>(cfg.users.size()); i++)
        if (i != except && !_wcsicmp(cfg.users[i].name.c_str(), name.c_str())) return true;
    return false;
}

// The user's own firmware: `dir`, or the usual place under it, when it holds
// entries named <id>.nca. Both shapes found in the wild count and are read the
// same way (nsw_core's findContent): a file per entry (the yuzu family, a dump
// straight from the console) or a folder per entry holding a file "00" (Ryujinx).
std::wstring FirmwareAt(const std::wstring& dir) {
    if (dir.empty()) return {};
    for (const wchar_t* sub : {L"", L"\\registered", L"\\Contents\\registered", L"\\system\\Contents\\registered",
                               L"\\nand\\system\\Contents\\registered", L"\\bis\\system\\Contents\\registered"})
        for (const auto& f : ListDir(dir + sub))
            if (EndsWithI(f.name, L".nca") && (!f.dir || Exists(dir + sub + L"\\" + f.name + L"\\00"))) return dir + sub;
    return {};
}

uint64_t ParseSpan(const std::string& s) {
    // Ryujinx stores play time as [d.]hh:mm:ss[.fraction]
    unsigned d = 0, h = 0, m = 0, sec = 0;
    size_t dot = s.find('.'), colon = s.find(':');
    if (dot != std::string::npos && colon != std::string::npos && dot < colon) {
        if (sscanf_s(s.c_str(), "%u.%u:%u:%u", &d, &h, &m, &sec) != 4) return 0;
    } else if (sscanf_s(s.c_str(), "%u:%u:%u", &h, &m, &sec) != 3) {
        return 0;
    }
    return static_cast<uint64_t>(d) * 86400 + h * 3600 + m * 60 + sec;
}

// ---- the data folder of the program's former name
// The program was published as Dockside first (historical name), and its data
// folder was <APPDATA>\Dockside (historical name). Users, play history and
// settings carry over: the first start after the rename moves that folder to
// the new name, or copies it when moving is refused (a file in it held open),
// the old one then kept untouched. Only that one folder is ever looked at:
// none of the same name with anything after it (a backup beside it), and
// nothing when the new folder is already there.
const wchar_t* const kFormerDataName = L"Dockside"; // historical name
const wchar_t* const kDataName = L"NSW-Home";

bool CopyFolder(const std::wstring& from, const std::wstring& to) {
    if (!CreateDirectoryW(to.c_str(), nullptr) && !Exists(to)) return false;
    bool ok = true;
    for (const DirEntry& e : ListDir(from)) {
        const std::wstring a = from + L"\\" + e.name, b = to + L"\\" + e.name;
        ok = (e.dir ? CopyFolder(a, b) : CopyFileW(a.c_str(), b.c_str(), FALSE) != 0) && ok;
    }
    return ok;
}

void DataLog(const std::wstring& dir, const std::wstring& line) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\debug.log").c_str(), L"ab") || !f) return;
    const std::string u = Narrow(line) + "\n";
    fwrite(u.data(), 1, u.size(), f);
    fclose(f);
}

// `dir`: the data folder about to be used. Returns the folder to use instead
// for this run: the former one, when it could be neither moved nor copied
// whole (then the next start tries again).
std::wstring TakeOverFormerDataDir(const std::wstring& dir) {
    const size_t cut = dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos || dir.compare(cut + 1, std::wstring::npos, kDataName) != 0 || Exists(dir)) return dir;
    const std::wstring former = dir.substr(0, cut + 1) + kFormerDataName;
    const DWORD attr = GetFileAttributesW(former.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return dir;
    if (MoveFileExW(former.c_str(), dir.c_str(), 0)) {
        DataLog(dir, L"data folder: " + former + L" moved to " + dir);
        return dir;
    }
    // A copy is made beside and renamed when whole, so that a copy cut short
    // never passes for the data folder.
    const std::wstring part = dir + L".copying";
    if (CopyFolder(former, part) && MoveFileExW(part.c_str(), dir.c_str(), 0)) {
        DataLog(dir, L"data folder: " + former + L" could not be moved; copied to " + dir + L" (the former folder is kept)");
        return dir;
    }
    DataLog(former, L"data folder: " + former + L" could be neither moved nor copied to " + dir + L"; used where it is");
    return former;
}

} // namespace

// ---- public ---------------------------------------------------------------

std::wstring DataDir() {
    static std::wstring dir;
    if (dir.empty()) {
        // NSWHOME_DATA points test runs at their own copy of the data folder.
        dir = Env(L"NSWHOME_DATA");
        if (dir.empty()) dir = Env(L"APPDATA") + L"\\" + kDataName;
        dir = TakeOverFormerDataDir(dir);
        CreateDirectoryW(dir.c_str(), nullptr);
        CreateDirectoryW((dir + L"\\icons").c_str(), nullptr);
        CreateDirectoryW((dir + L"\\avatars").c_str(), nullptr);
    }
    return dir;
}

std::wstring AvatarPath(const std::string& userId) {
    return DataDir() + L"\\avatars\\" + Widen(userId) + L".jpg";
}

User& CurUser() {
    if (cfg.currentUser < 0 || cfg.currentUser >= static_cast<int>(cfg.users.size())) cfg.currentUser = 0;
    return cfg.users[cfg.currentUser];
}

Played UserPlayed(int user, const Game& g) {
    Played p;
    auto it = cfg.played.find(cfg.users[user].id + "/" + g.id);
    if (it != cfg.played.end()) p = it->second;
    // Time Ryujinx logged before NSW-Home existed belongs to the first user.
    if (user == 0) {
        p.last = (std::max)(p.last, g.ryuLast);
        p.seconds = (std::max)(p.seconds, g.ryuSeconds);
    }
    return p;
}

std::string AddUser(const std::wstring& name) {
    if (NameTaken(name, -1)) return "NAME_TAKEN";
    cfg.users.push_back({NewUserId(), name, {}, NewFriendCode()});
    DefaultAvatar(cfg.users.back(), cfg.users.size() - 1);
    SaveConfig();
    return SyncProfiles() ? "" : "IO";
}

std::string RenameUser(int idx, const std::wstring& name) {
    if (NameTaken(name, idx)) return "NAME_TAKEN";
    cfg.users[idx].name = name;
    SaveConfig();
    return SyncProfiles() ? "" : "IO";
}

std::string SetUserImage(int idx, const std::wstring& imageFile) {
    return SetUserImageFramed(idx, imageFile, gfx::PicFrame());
}

// ---- profile pictures framed by the user (turned, zoomed, moved) -----------
// Beside <id>.jpg, the square every program shows, the avatars folder keeps
// the picture it was cut from as <id>.src.<ext> and how it was cut as
// <id>.frame, so "Ajustar imagen" can frame it again from the whole picture.

namespace {

std::wstring AvatarFramePath(const std::string& userId) {
    return DataDir() + L"\\avatars\\" + Widen(userId) + L".frame";
}

// Every kept original of the user but `keep` goes (there is one at a time;
// one with another extension replaces the old).
void DropAvatarSources(const std::string& userId, const std::wstring& keep) {
    const std::wstring prefix = Widen(userId) + L".src.";
    for (const auto& e : ListDir(DataDir() + L"\\avatars")) {
        if (e.dir || e.name.size() <= prefix.size() || _wcsnicmp(e.name.c_str(), prefix.c_str(), prefix.size())) continue;
        const std::wstring path = DataDir() + L"\\avatars\\" + e.name;
        if (keep.empty() || _wcsicmp(path.c_str(), keep.c_str())) DeleteFileW(path.c_str());
    }
}

void DropAvatarSource(const std::string& userId) {
    DropAvatarSources(userId, L"");
    DeleteFileW(AvatarFramePath(userId).c_str());
}

} // namespace

std::wstring AvatarSource(const std::string& userId) {
    const std::wstring prefix = Widen(userId) + L".src.";
    for (const auto& e : ListDir(DataDir() + L"\\avatars"))
        if (!e.dir && e.name.size() > prefix.size() && !_wcsnicmp(e.name.c_str(), prefix.c_str(), prefix.size()) &&
            !EndsWithI(e.name, L".tmp"))
            return DataDir() + L"\\avatars\\" + e.name;
    return {};
}

bool AvatarFrameOf(const std::string& userId, gfx::PicFrame& f) {
    std::string text;
    int x = 0, y = 0, z = 0, q = 0;
    // Whole numbers (hundred-thousandths): a decimal point would depend on the locale.
    if (!ReadAll(AvatarFramePath(userId), text) || sscanf_s(text.c_str(), "v1 %d %d %d %d", &x, &y, &z, &q) != 4)
        return false;
    f.panX = x / 100000.0f;
    f.panY = y / 100000.0f;
    f.zoom = z / 100000.0f;
    f.quarter = q;
    return true;
}

std::string SetUserImageFramed(int idx, const std::wstring& imageFile, const gfx::PicFrame& frame) {
    const std::string id = cfg.users[idx].id;
    // The original is copied in first: it may be the square itself (a picture
    // made before originals were kept, framed again), which is written over below.
    std::wstring ext;
    const size_t dot = imageFile.find_last_of(L'.'), slash = imageFile.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) ext = Lower(imageFile.substr(dot + 1));
    bool plain = !ext.empty() && ext.size() <= 5;
    for (wchar_t c : ext) plain = plain && ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'));
    if (!plain) ext = L"img";
    const std::wstring kept = DataDir() + L"\\avatars\\" + Widen(id) + L".src." + ext;
    if (_wcsicmp(kept.c_str(), imageFile.c_str())) {
        std::string bytes;
        if (!ReadAll(imageFile, bytes) || bytes.empty() || !WriteAll(kept, bytes.data(), bytes.size())) return "IO";
    }
    DropAvatarSources(id, kept);
    if (!gfx::MakeAvatarFramed(kept, frame, AvatarPath(id))) return "IO";
    char line[96];
    snprintf(line, sizeof line, "v1 %ld %ld %ld %d\n", lroundf(frame.panX * 100000), lroundf(frame.panY * 100000),
             lroundf(frame.zoom * 100000), frame.quarter);
    WriteAll(AvatarFramePath(id), line, strlen(line));
    return SyncProfiles() ? "" : "IO";
}

int AvatarColours() {
    return static_cast<int>(ARRAYSIZE(kAvatarPalette));
}

uint32_t AvatarColour(int i) {
    return kAvatarPalette[static_cast<size_t>(i) % ARRAYSIZE(kAvatarPalette)];
}

std::string SetUserDisc(int idx, int colour) {
    if (!DefaultAvatar(cfg.users[idx], static_cast<size_t>(colour))) return "IO";
    DropAvatarSource(cfg.users[idx].id); // a disc has no picture behind it to frame again
    return SyncProfiles() ? "" : "IO";
}

std::string RemoveUser(int idx) {
    if (cfg.users.size() <= 1) return "LAST_USER";
    const User removed = cfg.users[idx];
    DeleteFileW(AvatarPath(removed.id).c_str());
    DropAvatarSource(removed.id);
    cfg.users.erase(cfg.users.begin() + idx);
    if (cfg.currentUser >= idx && cfg.currentUser > 0) cfg.currentUser--;
    SaveConfig();
    return SyncProfiles(&removed) ? "" : "IO";
}

std::vector<FreeProfile> FreeProfiles() {
    std::vector<FreeProfile> out;
    std::set<std::wstring> done;
    for (const auto& e : kEmus) {
        if (!cfg.emulators.count(e.id) || !done.insert(Lower(ProfilesPath(e))).second) continue;
        for (const EmuProfile& p : ReadProfiles(e, EmuFiles(e))) {
            // An id some user has, or stands for in any emulator, is that
            // user's wherever it turns up (earlier versions copied every
            // user into every emulator under the one id).
            bool taken = false;
            for (const User& u : cfg.users) {
                taken = taken || u.id == p.id;
                for (const auto& link : u.emu) taken = taken || link.second == p.id;
            }
            if (taken) continue;
            bool listed = false;
            for (FreeProfile& f : out)
                if (f.id == p.id) {
                    f.emus += std::wstring(L", ") + e.label;
                    listed = true;
                }
            if (!listed) out.push_back({p.id, p.name, e.label});
        }
    }
    return out;
}

std::string AdoptProfile(const std::string& profileId) {
    // The profile's id becomes the user's: play time recorded under it by an
    // earlier version is theirs again.
    User u{profileId, {}, {}, NewFriendCode()};
    std::string picture;
    for (const User& o : cfg.users)
        if (o.id == profileId) return "IO";
    for (const auto& e : kEmus) {
        if (!cfg.emulators.count(e.id)) continue;
        for (const EmuProfile& p : ReadProfiles(e, EmuFiles(e), nullptr, true)) {
            if (p.id != profileId) continue;
            u.emu[e.id] = p.id;
            if (u.name.empty()) u.name = p.name;
            if (picture.empty()) picture = p.picture;
        }
    }
    if (u.emu.empty()) return "IO";
    if (NameTaken(u.name, -1)) u.name.clear(); // nicknames are unique here; the profile keeps its own
    cfg.users.push_back(u);
    if (picture.empty() || !WriteAll(AvatarPath(u.id), picture.data(), picture.size()))
        DefaultAvatar(cfg.users.back(), cfg.users.size() - 1);
    SaveConfig();
    // (An emulator that has no such profile gets one NSW-Home keeps for them.)
    return SyncProfiles() ? "" : "IO";
}

std::wstring AdoptedProfileName(int idx, const std::string& emuId) {
    const EmuDef* e = FindEmu(emuId);
    if (!e || idx < 0 || idx >= static_cast<int>(cfg.users.size()) || !Adopted(cfg.users[idx], *e)) return {};
    for (const EmuProfile& p : ReadProfiles(*e, EmuFiles(*e)))
        if (p.id == ProfileOf(cfg.users[idx], *e)) return p.name;
    return {};
}

// The main emulator's when it has them, so that what NSW-Home reads is what
// the games will run with; otherwise any emulator's; with none, the user's
// own (the place given for an install that has no emulator).
std::wstring KeysDir() {
    std::vector<const EmuDef*> order = {&MainEmu()};
    for (const auto& e : kEmus) order.push_back(&e);
    for (const EmuDef* e : order) {
        std::wstring d = EmuDataDir(*e) + L"\\" + e->keysSub;
        if (Exists(d + L"\\prod.keys")) return d;
    }
    return !cfg.ownKeys.empty() && Exists(cfg.ownKeys + L"\\prod.keys") ? cfg.ownKeys : std::wstring();
}

std::wstring FirmwareDir() {
    std::vector<const EmuDef*> order = {&MainEmu()};
    for (const auto& e : kEmus)
        if (cfg.emulators.count(e.id)) order.push_back(&e);
    order.push_back(&kEmus[0]);
    for (const EmuDef* e : order) {
        // The emulator creates the folder at its first start; it is firmware once it has contents.
        std::wstring d = FirmwareIn(EmuDataDir(*e), *e);
        if (!ListDir(d).empty()) return d;
    }
    return FirmwareAt(cfg.ownFirmware); // the user's own, when no emulator has any
}

bool UsingOwnKeys() {
    return !cfg.ownKeys.empty() && KeysDir() == cfg.ownKeys;
}

bool UsingOwnFirmware() {
    return !cfg.ownFirmware.empty() && FirmwareDir() == cfg.ownFirmware;
}

std::string SetOwnFirmware(const std::wstring& folder) {
    const std::wstring found = FirmwareAt(folder);
    if (!folder.empty() && found.empty()) return "NO_FIRMWARE";
    cfg.ownFirmware = found;
    SaveConfig();
    return {};
}

std::string SetOwnKeys(const std::wstring& fileOrFolder) {
    std::wstring dir = fileOrFolder;
    if (!dir.empty()) {
        // The readers look for a file of this name in a folder; the contents
        // are only checked for being a key file, never shown, logged or kept.
        if (EndsWithI(dir, L"\\prod.keys") || EndsWithI(dir, L"/prod.keys")) dir = Parent(dir);
        std::string text;
        if (!ReadAll(dir + L"\\prod.keys", text) || text.size() > (1u << 20) || text.find("header_key") == std::string::npos)
            return "KEYS_INVALID";
    }
    cfg.ownKeys = dir;
    SaveConfig();
    return {};
}

// ---- PC games ---------------------------------------------------------------

std::string PcGameId(const std::wstring& exe) {
    // A number made from the path (FNV-1a), so the same program is the same
    // game: added again after being removed, it finds its play time.
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : Lower(exe)) h = (h ^ static_cast<uint64_t>(c == L'/' ? L'\\' : c)) * 1099511628211ull;
    char id[24];
    snprintf(id, sizeof id, "pc:%016llx", static_cast<unsigned long long>(h));
    return id;
}

std::wstring PcIconFile(const std::string& id) {
    return DataDir() + L"\\icons\\pc_" + Widen(id.substr(id.find(':') + 1)) + L".png";
}

std::string AddPcGame(const std::wstring& exe, const std::wstring& name) {
    const DWORD attrs = GetFileAttributesW(exe.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return "PC_MISSING";
    PcGame g{PcGameId(exe), exe, name, {}};
    for (const PcGame& o : cfg.pcGames)
        if (o.id == g.id) return "PC_EXISTS";
    if (g.name.empty()) {
        g.name = exe.substr(exe.find_last_of(L"\\/") + 1);
        const size_t dot = g.name.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0) g.name.resize(dot);
    }
    cfg.pcGames.push_back(g);
    SaveConfig();
    return {};
}

void RenamePcGame(const std::string& id, const std::wstring& name) {
    for (PcGame& g : cfg.pcGames)
        if (g.id == id && !name.empty()) g.name = name;
    SaveConfig();
}

std::string SetPcGameImage(const std::string& id, const std::wstring& imageFile) {
    // Kept as it is under the picture's usual name: images are told apart by
    // their contents when decoded, not by the ending.
    return CopyFileW(imageFile.c_str(), PcIconFile(id).c_str(), FALSE) ? "" : "IO";
}

void RemovePcGame(const std::string& id) {
    for (size_t i = 0; i < cfg.pcGames.size(); i++)
        if (cfg.pcGames[i].id == id) cfg.pcGames.erase(cfg.pcGames.begin() + i--);
    DeleteFileW(PcIconFile(id).c_str());
    SaveConfig();
}

// ---- several emulators ----------------------------------------------------

std::string IdentifyEmulator(const std::wstring& exe) {
    return IdentifyExe(exe).id;
}

EmuStatus EmuInfo(const std::string& id) {
    EmuStatus st;
    const EmuDef* e = FindEmu(id);
    auto it = cfg.emulators.find(id);
    if (!e || it == cfg.emulators.end()) return st;
    st.found = IdentifyExe(it->second).id == id && Exists(it->second);
    st.version = IdentifyExe(it->second).version;
    st.ownFolder = OwnFolder(*e);
    st.dataDir = EmuDataDir(*e);
    st.keys = Exists(st.dataDir + L"\\" + e->keysSub + L"\\prod.keys");
    st.firmware = !ListDir(FirmwareIn(st.dataDir, *e)).empty();
    for (const auto& o : kEmus)
        if (st.sharedWith.empty() && strcmp(o.id, e->id) && cfg.emulators.count(o.id) &&
            !_wcsicmp(EmuDataDir(o).c_str(), st.dataDir.c_str()))
            st.sharedWith = o.id;
    // Somebody else has what this one lacks.
    for (const auto& o : kEmus) {
        if (!strcmp(o.id, e->id) || !cfg.emulators.count(o.id)) continue;
        const std::wstring dir = EmuDataDir(o);
        if (!st.keys && st.keysFrom.empty() && Exists(dir + L"\\" + o.keysSub + L"\\prod.keys")) st.keysFrom = o.id;
        if (!st.firmware && st.firmwareFrom.empty() && o.family == e->family && !ListDir(FirmwareIn(dir, o)).empty())
            st.firmwareFrom = o.id;
    }
    return st;
}

std::string AddEmulator(const std::wstring& exe) {
    const std::string id = IdentifyExe(exe).id;
    if (id.empty()) return {};
    cfg.emulators[id] = exe;
    if (cfg.defaultEmu.empty() || !cfg.emulators.count(cfg.defaultEmu)) cfg.defaultEmu = id;
    LinkFirstUser(*FindEmu(id)); // the profile it was used with stays the player's
    SaveConfig();
    SyncEmu(*FindEmu(id), nullptr, true); // and it gets the other users at once
    return id;
}

std::string GiveKeys(const std::string& toId) {
    const EmuStatus st = EmuInfo(toId);
    const EmuDef* from = FindEmu(st.keysFrom);
    if (!from) return "NO_KEYS";
    return InstallKeys(EmuDataDir(*from) + L"\\" + from->keysSub + L"\\prod.keys", toId);
}

std::string GiveFirmware(const std::string& toId) {
    const EmuStatus st = EmuInfo(toId);
    const EmuDef* from = FindEmu(st.firmwareFrom);
    const EmuDef* to = FindEmu(toId);
    if (!from || !to) return "NO_FIRMWARE";
    const std::wstring src = FirmwareIn(EmuDataDir(*from), *from), dst = FirmwareIn(EmuFiles(*to), *to);
    SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr);
    // One system file each: a file "<id>.nca" in the yuzu family, a folder of
    // that name holding one file called "00" in the Ryujinx family. Only
    // offered within a family, so the shape is copied as it is.
    for (const auto& f : ListDir(src)) {
        const std::wstring in = src + L"\\" + f.name, out = dst + L"\\" + f.name;
        if (!f.dir) {
            if (!CopyFileW(in.c_str(), out.c_str(), FALSE)) return "IO";
            continue;
        }
        CreateDirectoryW(out.c_str(), nullptr);
        for (const auto& part : ListDir(in))
            if (!part.dir && !CopyFileW((in + L"\\" + part.name).c_str(), (out + L"\\" + part.name).c_str(), FALSE))
                return "IO";
    }
    return {};
}

std::wstring EmuLaunchArgs(const std::string& emuId, const std::wstring& gameFile) {
    const EmuDef* def = FindEmu(emuId);
    if (!def) return {};
    wchar_t args[2048], user[300] = L"";
    swprintf_s(args, def->argsFmt, (L"\"" + gameFile + L"\"").c_str());
    // Start the emulator as the profile the selected user is there (see the
    // rule with ProfileOf), so it loads that user's saves: by the name that
    // profile has in the emulator's own list in the Ryujinx family, by its
    // position in the list in the yuzu family (which keeps its own current
    // user when the profile is not in its list).
    const std::string profile = ProfileOf(CurUser(), *def);
    if (def->profileFmt) {
        std::wstring name = CurUser().name;
        for (const EmuProfile& p : ReadProfiles(*def, EmuDataDir(*def)))
            if (p.id == profile) name = p.name;
        if (!name.empty() && name.size() < 200) swprintf_s(user, def->profileFmt, (L"\"" + name + L"\"").c_str());
    } else if (def->family == 'y') {
        int index = YuzuUserIndex(*def, profile);
        if (index >= 0) swprintf_s(user, L"-u %d ", index);
    }
    return std::wstring(user) + args;
}

// ---- first-start assistant ------------------------------------------------

void FindEmulators() {
    const std::map<std::string, std::wstring> had = cfg.emulators;
    DetectEmulators();
    // The first user is who each newly found emulator was being used as.
    for (const auto& e : kEmus)
        if (cfg.emulators.count(e.id) && !had.count(e.id)) LinkFirstUser(e);
}

std::string FindEmulatorIn(const std::wstring& where, std::wstring& exe) {
#if defined(__ANDROID__)
    // [android] `where` is a package name. cfg.emulators only ever holds
    // packages that are installed (it is refreshed whenever the app comes to
    // the front), so knowing the name is knowing the emulator.
    for (const auto& e : kEmus) {
        auto it = cfg.emulators.find(e.id);
        if (it != cfg.emulators.end() && it->second == where) {
            exe = where;
            return e.id;
        }
    }
    return {};
#endif
    std::wstring path = where;
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (path.empty() || !Exists(path)) return {};
    // (Where program names have no ending, a folder can carry the same name:
    // only a file is the program itself.)
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        exe = path;
        return IdentifyExe(path).id;
    }
    // A folder: the program itself is often one or two levels down (a
    // versioned folder inside the one the archive was unpacked to).
    std::vector<std::wstring> found;
    FindExes(path, 2, found);
    for (const auto& e : kEmus)
        for (const auto& f : found)
            if (IdentifyExe(f).id == e.id) {
                exe = f;
                return e.id;
            }
    return {};
}

void AdoptEmulator(const std::string& id, const std::wstring& exe) {
    cfg.emulators[id] = exe;
    cfg.defaultEmu = id;
    if (cfg.gameDirs.empty()) SeedGameDirs();
    // The first user is the profile it was being used with, which has the
    // saves; its other profiles come in only when asked for (AdoptProfile).
    LinkFirstUser(*FindEmu(id));
    CurUser();
    SaveConfig();
    SyncEmu(*FindEmu(id), nullptr, true); // and it gets the users it did not have
}

std::string InstallKeys(const std::wstring& file, const std::string& emuId) {
    // Only ever copied: the contents are not shown, logged or kept.
    std::string text;
    if (!ReadAll(file, text)) return "IO";
    if (text.size() > (1u << 20) || text.find("header_key") == std::string::npos) return "KEYS_INVALID";
    const EmuDef* emu = FindEmu(emuId);
    if (!emu) emu = &MainEmu();
    const std::wstring dir = EmuFiles(*emu) + L"\\" + emu->keysSub;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (!WriteAll(dir + L"\\prod.keys", text.data(), text.size())) return "IO";
    // The per-game keys usually sit next to it; the emulator wants both.
    std::string titles;
    if (!Exists(dir + L"\\title.keys") && ReadAll(Parent(file) + L"\\title.keys", titles) && !titles.empty())
        WriteAll(dir + L"\\title.keys", titles.data(), titles.size());
    return {};
}

// ---- controllers -------------------------------------------------------------

void PadOrderLoad() {
    static bool done = false;
    if (done) return;
    done = true;
    auto it = cfg.console.find("pads.order");
    if (it != cfg.console.end()) pads::SetOrder(pads::SplitOrder(it->second));
}

void PadOrderSave(const std::vector<std::string>& ids) {
    pads::SetOrder(ids);
    cfg.console["pads.order"] = pads::JoinOrder(ids);
    SaveConfig();
}

// Makes the emulator's own controller entries follow NSW-Home's players
// (padcfg.h: entries are only moved, never made). Returns the players whose
// pad the emulator has no entry for.
static padcfg::Result PadOrderApply(const EmuDef& e, bool checkOnly) {
    PadOrderLoad();
    if (NoFiles(e)) return {}; // [android]
    const std::vector<padcfg::Want> wants = padcfg::WantsFrom(pads::List());
    const std::wstring dir = EmuFiles(e);
    return e.family == 'r' ? padcfg::ApplyRyujinx(dir + L"\\Config.json", wants, checkOnly)
                           : padcfg::ApplyYuzu(dir + L"\\config\\qt-config.ini", wants, checkOnly);
}

bool PadsUnknown(std::vector<int>& players, std::wstring& emulator) {
    const EmuDef& e = MainEmu();
    emulator = e.label;
    const padcfg::Result r = PadOrderApply(e, true);
    players = r.missing;
    return r.ok;
}

// The controller settings waiting to be written (padwish.h) go into this
// emulator's file. True when the file now holds every one of them.
static bool PadWishesApply(const EmuDef& e) {
    if (NoFiles(e)) return false; // [android]
    const std::wstring file = EmuFiles(e) + (e.family == 'r' ? L"\\Config.json" : L"\\config\\qt-config.ini");
    const padcfg::Family family = e.family == 'r' ? padcfg::Family::Ryujinx : padcfg::Family::Yuzu;
    const auto wish = [](const char* key) {
        auto it = cfg.console.find(std::string("pads.wish.") + key);
        return it == cfg.console.end() ? std::string() : it->second;
    };
    bool all = true;
    std::string w = wish("rumble");
    if (!w.empty()) all = padcfg::RumbleSet(file, family, w == "on") && all;
    w = wish("keyboard");
    if (!w.empty()) all = padcfg::SwitchSet(file, family, padcfg::Switch::Keyboard, w == "on") && all;
    w = wish("mouse");
    if (!w.empty()) all = padcfg::SwitchSet(file, family, padcfg::Switch::Mouse, w == "on") && all;
    w = wish("mouse_speed");
    // (An emulator without that number simply has nothing to take.)
    if (!w.empty() && padcfg::MouseSpeedGet(file, family) >= 0) all = padcfg::MouseSpeedSet(file, family, atoi(w.c_str())) && all;
    return all;
}

std::wstring PadEmuName() {
    return MainEmu().label;
}

void PadWishesApplyMain() {
    bool any = false;
    for (const auto& kv : cfg.console) any = any || kv.first.compare(0, 10, "pads.wish.") == 0;
    if (!any || !PadWishesApply(MainEmu())) return;
    for (auto it = cfg.console.begin(); it != cfg.console.end();)
        it = it->first.compare(0, 10, "pads.wish.") == 0 ? cfg.console.erase(it) : std::next(it);
    SaveConfig();
}

std::wstring EmuFilesFor(const std::string& emuId) {
    const EmuDef* e = FindEmu(emuId);
    return e ? EmuFiles(*e) : std::wstring();
}

void PrepareEmulator(const std::string& emuId, int screenX, int screenY) {
    const EmuDef* def = FindEmu(emuId);
    if (!def || NoFiles(*def)) return; // [android]
    // The user the game is about to be started as has to be in its list.
    SyncEmu(*def, nullptr, true);
    // Player 1 to 4 are the pads NSW-Home says they are.
    PadOrderApply(*def, false);
    // Vibration, keyboard and mouse as chosen in Settings while a game was open.
    if (def == &MainEmu()) PadWishesApplyMain();
    else PadWishesApply(*def);
    if (def->family != 'r') {
        // Its interface asks questions that would stay hidden under the launch
        // screen: which user plays (we pass that), and whether to update. A
        // game paused "in the background" would never draw its first picture
        // either, since the launch screen is what has the foreground.
        // The file is written whole by the emulator and is complete after its
        // first start; before that, these lines alone are a valid file, and
        // its folder existing keeps Eden from offering to import another
        // emulator's data at its first start.
        const std::wstring dir = EmuDataDir(*def) + L"\\config";
        if (!Exists(EmuDataDir(*def))) return;
        std::vector<std::pair<std::string, std::string>> wanted = {
            {"UI.select_user_on_boot", "false"}, {"UI.check_for_updates", "false"}, {"UI.pauseWhenInBackground", "false"}};
        if (!Exists(dir + L"\\qt-config.ini")) {
            CreateDirectoryW(dir.c_str(), nullptr);
            yuzucfg::Seed(dir + L"\\qt-config.ini", wanted);
            return;
        }
        if (!yuzucfg::Open(dir + L"\\qt-config.ini")) return;
        // Its window is never on screen (the game's picture is shown inside
        // ours), so nothing may open that only that window could show. A
        // screenshot must not ask where to save; and the pad combinations it
        // reads even without the focus, which would leave full screen, pause,
        // stop, restart or exit emulation or open a file dialog, are emptied
        // (the keyboard's are kept from it while a game runs, see main.cpp).
        wanted.push_back({"UI.Screenshots\\enable_screenshot_save_as", "false"});
        for (const char* name : {"Continue\\Pause%20Emulation", "Fullscreen", "Load\\Remove%20Amiibo", "Restart%20Emulation", "Stop%20Emulation",
                                 "Exit%20Eden", "Exit%20eden", "Exit%20suyu", "Exit%20citron", "Exit%20sudachi", "Exit%20yuzu"})
            wanted.push_back({std::string("UI.Shortcuts\\Main%20Window\\") + name + "\\Controller_KeySeq", ""});
        std::vector<std::pair<std::string, std::string>> changes;
        for (const auto& w : wanted) {
            // (What each is when the file leaves it at its default: the update
            // check and asking where to save are on, the pad combinations set.)
            const bool onByDefault = w.first == "UI.check_for_updates" || w.second.empty() || w.first.find("save_as") != std::string::npos;
            if (yuzucfg::Has(w.first) && yuzucfg::Effective(w.first, onByDefault ? "true" : "false") != w.second) changes.push_back(w);
        }
        if (!changes.empty()) yuzucfg::SetMany(changes);
        return;
    }
    const std::wstring path = EmuDataDir(*def) + L"\\Config.json";
    std::string text;
    if (!ReadAll(path, text)) return;
    bool changed = false;
    for (const char* key : {"\"show_console\": true", "\"check_updates_on_start\": true"}) {
        size_t at = text.find(key);
        if (at == std::string::npos) continue;
        text.replace(at + strlen(key) - 4, 4, "false");
        changed = true;
    }
    // Where its window opens: on the menu's screen, under the launch screen.
    const std::pair<const char*, int> places[] = {{"\"window_position_x\":", screenX + 8},
                                                  {"\"window_position_y\":", screenY + 8}};
    for (const auto& pl : places) {
        size_t at = text.find(pl.first);
        if (at == std::string::npos) continue;
        size_t from = at + strlen(pl.first), to = text.find_first_of(",\r\n}", from);
        if (to == std::string::npos) continue;
        std::string want = " " + std::to_string(pl.second);
        if (text.compare(from, to - from, want) != 0) {
            text.replace(from, to - from, want);
            changed = true;
        }
    }
    if (!changed) return;
    if (!Exists(path + L".dockside-backup")) CopyFileW(path.c_str(), (path + L".dockside-backup").c_str(), TRUE);
    WriteAll(path, text.data(), text.size());
}

void PlaceEmulatorWindow(const std::string& emuId, int x, int y, int screenWidth, int screenNumber) {
    const EmuDef* def = FindEmu(emuId);
    if (!def || def->family != 'y') return;
    const std::wstring dir = EmuDataDir(*def) + L"\\config", path = dir + L"\\window_state.ini";
    if (!Exists(dir)) return;
    // What Qt's QWidget::saveGeometry writes (version 3; the layout was
    // checked against the entry in a file Citron wrote): a magic number, the
    // version, the window's rectangle with and without its frame (left, top,
    // right, bottom), the number of its screen, two flags (maximised, full
    // screen), that screen's width, and the rectangle without frame again;
    // all big-endian. Restoring refuses the entry when the screen's width
    // differs much from the stored one and pulls the window onto the numbered
    // screen when the rectangle is elsewhere, so all three must agree.
    std::string blob;
    auto put = [&](long long v, int bytes) {
        for (int i = bytes - 1; i >= 0; i--) blob += static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    const int w = (std::min)(1280, screenWidth - 120), h = w * 9 / 16, fx = x + 40, fy = y + 40;
    auto rect = [&](int l, int t, int rw, int rh) {
        put(l, 4), put(t, 4), put(l + rw - 1, 4), put(t + rh - 1, 4);
    };
    put(0x1D9D0CB, 4), put(3, 2), put(0, 2);
    rect(fx, fy, w + 16, h + 39);
    rect(fx + 8, fy + 31, w, h);
    put(screenNumber, 4), put(0, 1), put(0, 1), put(screenWidth, 4);
    rect(fx + 8, fy + 31, w, h);
    // In the file every byte is written as \x and its hexadecimal value, which
    // the reader accepts for any byte and which needs none of the format's
    // special cases.
    std::string line = "geometry=@ByteArray(";
    for (unsigned char c : blob) {
        char hex[8];
        snprintf(hex, sizeof hex, "\\x%x", c);
        line += hex;
    }
    line += ")";

    std::string text;
    const bool had = ReadAll(path, text);
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string out = text;
    size_t sec = out.find("[General]"), at = std::string::npos;
    if (sec != std::string::npos) {
        // The key at the start of a line, inside this section.
        size_t next = out.find("\n[", sec);
        for (size_t p = out.find("\ngeometry=", sec); p != std::string::npos && (next == std::string::npos || p < next);
             p = std::string::npos)
            at = p + 1;
    }
    if (at != std::string::npos) {
        size_t end = out.find_first_of("\r\n", at);
        out.replace(at, (end == std::string::npos ? out.size() : end) - at, line);
    } else if (sec != std::string::npos) {
        size_t nl = out.find('\n', sec);
        if (nl == std::string::npos) out += eol + line + eol;
        else out.insert(nl + 1, line + eol);
    } else {
        out = "[General]" + eol + line + eol + out;
    }
    if (out == text) return;
    if (had && !Exists(path + L".dockside-backup")) CopyFileW(path.c_str(), (path + L".dockside-backup").c_str(), TRUE);
    WriteAll(path, out.data(), out.size());
}

std::wstring EmuConfigPath() {
    // A run pointed at its own data folder (development, tests) only ever
    // works on the copy there, so the real file cannot be written by accident;
    // without a copy the emulator's settings simply show as unavailable.
    const EmuDef& e = MainEmu();
    return EmuFiles(e) + (e.family == 'r' ? L"\\Config.json" : L"\\config\\qt-config.ini");
}

std::string EmuConfigKind() {
    const EmuDef& e = MainEmu();
    return e.family == 'r' ? std::string() : std::string(e.id);
}

std::wstring SaveDataDir() {
    const EmuDef& e = MainEmu();
    std::wstring d = EmuDataDir(e) + (e.family == 'r' ? L"\\bis\\user\\save" : L"\\nand\\user\\save");
    return Exists(d) ? d : std::wstring();
}

std::wstring ScreenshotDir() {
    std::vector<std::wstring> shots = ListScreenshots();
    return shots.empty() ? std::wstring() : Parent(shots[0]);
}

bool DiskSpace(const std::wstring& path, uint64_t& free, uint64_t& total) {
    ULARGE_INTEGER avail, all;
    if (path.empty() || !GetDiskFreeSpaceExW(path.c_str(), &avail, &all, nullptr)) return false;
    free = avail.QuadPart;
    total = all.QuadPart;
    return true;
}

std::string NowIso() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    return b;
}

void LoadConfig() {
    Json j;
    bool fresh = !LoadJson(DataDir() + L"\\config.json", j) || j.type != Json::Obj;
    if (fresh) FirstRun();
    cfg.dark = j.boolean("dark", false);
    cfg.clock24 = j.boolean("clock24", false);
    cfg.sounds = j.boolean("sounds", true);
    cfg.swapAB = j.boolean("swapAB", false);
    cfg.fullscreen = j.boolean("fullscreen", true);
    cfg.monitor = static_cast<int>(j.number("monitor", -1));
    cfg.menuStyle = j.number("menuStyle", 1) == 2 ? 2 : 1;
    cfg.identical = j.boolean("identical", true);
    cfg.setupDone = j.boolean("setupDone", false);
    if (const Json* k = j.get("captureKey")) cfg.captureKey = k->str;
    cfg.defaultEmu = j.s("defaultEmu");
    cfg.currentUser = static_cast<int>(j.number("currentUser", 0));
    if (const Json* a = j.get("users"))
        for (const auto& u : a->arr)
            if (u.s("id").size() == 32) {
                cfg.users.push_back({u.s("id"), Widen(u.s("name")), {}, u.s("friendCode")});
                if (const Json* links = u.get("emu"))
                    for (const auto& kv : links->obj)
                        if (kv.second.str.size() == 32) cfg.users.back().emu[kv.first] = kv.second.str;
            }
    if (const Json* a = j.get("pcGames"))
        for (const auto& g : a->arr)
            if (!g.s("id").empty() && !g.s("exe").empty())
                cfg.pcGames.push_back({g.s("id"), Widen(g.s("exe")), Widen(g.s("name")), Widen(g.s("args"))});
    cfg.ownFirmware = Widen(j.s("ownFirmware"));
    cfg.ownKeys = Widen(j.s("ownKeys"));
    cfg.title = Widen(j.s("title"));
    if (const Json* a = j.get("gameDirs"))
        for (const auto& d : a->arr) cfg.gameDirs.push_back(Widen(d.str));
    if (const Json* o = j.get("emulators"))
        for (const auto& kv : o->obj) cfg.emulators[kv.first] = Widen(kv.second.str);
    if (const Json* o = j.get("perGame"))
        for (const auto& kv : o->obj) cfg.perGame[kv.first] = kv.second.str;
    if (const Json* o = j.get("console"))
        for (const auto& kv : o->obj) cfg.console[kv.first] = kv.second.str;
    if (const Json* o = j.get("played"))
        for (const auto& kv : o->obj)
            cfg.played[kv.first] = {kv.second.s("last"), static_cast<uint64_t>(kv.second.number("seconds", 0))};

    // Earlier versions knew one "ryujinx" and one "yuzu"; each program gets
    // its own identity, once.
    const bool renamed = !fresh && j.number("emuIdent", 0) < 1;
    if (renamed) ReIdentify();
    size_t before = cfg.users.size();
    if (cfg.users.empty()) {
        // One user, with no name and a plain picture (the assistant offers
        // both, and neither is needed), who in each emulator is the profile it
        // was being used with: the player's saves are that profile's. The id
        // is the one every Ryujinx starts with.
        cfg.users.push_back({"00000000000000010000000000000000", L"", {}, {}});
        DeleteFileW(AvatarPath(cfg.users[0].id).c_str()); // (drawn again below)
        for (const auto& e : kEmus)
            if (cfg.emulators.count(e.id)) LinkFirstUser(e);
    }
    // Every user has a friend code; those made before there were any get theirs now.
    bool coded = false;
    for (User& u : cfg.users)
        if (u.friendCode.size() != 12) {
            u.friendCode = NewFriendCode();
            coded = true;
        }
    // Every user has a picture file too (a profile made in an emulator needs
    // one): a missing one is drawn again, plain for a user without a nickname.
    for (size_t i = 0; i < cfg.users.size(); i++)
        if (!Exists(AvatarPath(cfg.users[i].id))) {
            if (cfg.users[i].name.empty()) gfx::MakeAvatar(L"", gfx::rgb(0x8C8C8C), L"", AvatarPath(cfg.users[i].id));
            else DefaultAvatar(cfg.users[i], i);
        }
    CurUser();
    // A removed user's picture cannot always be deleted on the spot (it may be
    // on screen at that moment); what was left behind goes at the next start.
    for (const auto& e : ListDir(DataDir() + L"\\avatars")) {
        // <id>.jpg, and the original and framing kept with it (<id>.src.<ext>, <id>.frame).
        if (e.dir || e.name.size() < 36 || e.name[32] != L'.' ||
            (!(e.name.size() == 36 && EndsWithI(e.name, L".jpg")) && e.name.compare(32, 5, L".src.") &&
             e.name.compare(32, std::wstring::npos, L".frame")))
            continue;
        std::string id = Narrow(e.name.substr(0, 32));
        bool owned = false;
        for (const auto& u : cfg.users) owned = owned || u.id == id;
        if (!owned) DeleteFileW((DataDir() + L"\\avatars\\" + e.name).c_str());
    }
    // The same for the picture of a PC game that was removed.
    const std::wstring icons = DataDir() + L"\\icons\\";
    for (const auto& e : ListDir(DataDir() + L"\\icons")) {
        if (e.dir || e.name.rfind(L"pc_", 0) != 0 || !EndsWithI(e.name, L".png")) continue;
        bool owned = false;
        for (const auto& g : cfg.pcGames) owned = owned || !_wcsicmp(PcIconFile(g.id).c_str(), (icons + e.name).c_str());
        if (!owned) DeleteFileW((icons + e.name).c_str());
    }
    if (fresh || renamed || coded || cfg.users.size() != before) SaveConfig();
}

void SaveConfig() {
    std::string o = "{\n";
    auto flag = [&](const char* k, bool v) { o += std::string("  \"") + k + "\": " + (v ? "true" : "false") + ",\n"; };
    flag("dark", cfg.dark);
    flag("clock24", cfg.clock24);
    flag("sounds", cfg.sounds);
    flag("swapAB", cfg.swapAB);
    flag("fullscreen", cfg.fullscreen);
    flag("identical", cfg.identical);
    flag("setupDone", cfg.setupDone);
    o += "  \"monitor\": " + std::to_string(cfg.monitor) + ",\n";
    o += "  \"captureKey\": " + Quote(cfg.captureKey) + ",\n";
    o += "  \"emuIdent\": 1,\n";
    o += "  \"menuStyle\": " + std::to_string(cfg.menuStyle) + ",\n";
    o += "  \"defaultEmu\": " + Quote(cfg.defaultEmu) + ",\n";
    o += "  \"currentUser\": " + std::to_string(cfg.currentUser) + ",\n  \"users\": [";
    for (size_t i = 0; i < cfg.users.size(); i++) {
        const User& u = cfg.users[i];
        o += std::string(i ? ", " : "") + "{\"id\": " + Quote(u.id) + ", \"name\": " + Quote(u.name) +
             ", \"friendCode\": " + Quote(u.friendCode);
        // Which profile of each emulator's own the user stands for (see ProfileOf).
        for (const auto& kv : u.emu)
            o += (kv.first == u.emu.begin()->first ? ", \"emu\": {" : ", ") + Quote(kv.first) + ": " + Quote(kv.second);
        o += u.emu.empty() ? "}" : "}}";
    }
    o += "],\n";
    o += "  \"title\": " + Quote(cfg.title) + ",\n";
    o += "  \"ownFirmware\": " + Quote(cfg.ownFirmware) + ",\n  \"ownKeys\": " + Quote(cfg.ownKeys) + ",\n";
    o += "  \"pcGames\": [";
    for (size_t i = 0; i < cfg.pcGames.size(); i++)
        o += std::string(i ? ", " : "") + "{\"id\": " + Quote(cfg.pcGames[i].id) + ", \"exe\": " + Quote(cfg.pcGames[i].exe) +
             ", \"name\": " + Quote(cfg.pcGames[i].name) + ", \"args\": " + Quote(cfg.pcGames[i].args) + "}";
    o += "],\n";
    o += "  \"gameDirs\": [";
    for (size_t i = 0; i < cfg.gameDirs.size(); i++) o += (i ? ", " : "") + Quote(cfg.gameDirs[i]);
    o += "],\n  \"emulators\": {";
    bool first = true;
    for (const auto& kv : cfg.emulators) {
        o += (first ? "" : ", ") + Quote(kv.first) + ": " + Quote(kv.second);
        first = false;
    }
    o += "},\n  \"perGame\": {";
    first = true;
    for (const auto& kv : cfg.perGame) {
        o += (first ? "" : ", ") + Quote(kv.first) + ": " + Quote(kv.second);
        first = false;
    }
    o += "},\n  \"console\": {";
    first = true;
    for (const auto& kv : cfg.console) {
        o += (first ? "" : ", ") + Quote(kv.first) + ": " + Quote(kv.second);
        first = false;
    }
    o += "},\n  \"played\": {";
    first = true;
    for (const auto& kv : cfg.played) {
        o += (first ? "" : ", ") + Quote(kv.first) + ": {\"last\": " + Quote(kv.second.last) +
             ", \"seconds\": " + std::to_string(kv.second.seconds) + "}";
        first = false;
    }
    o += "}\n}\n";
    WriteAll(DataDir() + L"\\config.json", o.data(), o.size());
}

std::vector<Game> ScanLibrary() {
    // Snapshot what the scan needs so the UI thread can keep using cfg.
    const std::vector<std::wstring> dirs = cfg.gameDirs;
    const std::map<std::string, Played> played = cfg.played;
    const std::vector<PcGame> programs = cfg.pcGames;
    const std::wstring keys = KeysDir();
    const std::wstring ryuGames = EmuDataDir(RyuEmu()) + L"\\games\\";

    struct Found {
        std::wstring file;
        uint64_t size, mtime;
    };
    std::vector<Found> files;
    std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& dir, int depth) {
        for (const auto& e : ListDir(dir)) {
            std::wstring full = dir + L"\\" + e.name;
            if (e.dir) {
                if (depth > 0) walk(full, depth - 1);
            } else if (EndsWithI(e.name, L".nsp") || EndsWithI(e.name, L".xci")) {
                files.push_back({full, e.size, e.mtime});
            }
        }
    };
    for (const auto& d : dirs) walk(d, 2);

    // Cache of what was read from each dump, keyed by path; an entry is reused
    // while the file's size and modification time are unchanged.
    const std::wstring cachePath = DataDir() + L"\\library.json";
    Json cache;
    LoadJson(cachePath, cache);

    std::vector<Game> out;
    std::set<std::string> seen;
    std::string newCache = "{\n";
    content::SetCache(DataDir() + L"\\content.json");
    for (const auto& f : files) {
        std::string key = Narrow(Lower(f.file));
        Game g;
        g.file = f.file;
        g.size = f.size;
        size_t slash = f.file.find_last_of(L'\\');
        g.name = f.file.substr(slash + 1, f.file.size() - slash - 5);

        const Json* hit = cache.get(key.c_str());
        if (hit && static_cast<uint64_t>(hit->number("size", 0)) == f.size && hit->s("mtime") == std::to_string(f.mtime)) {
            g.titleId = hit->s("titleId");
            if (!hit->s("name").empty()) g.name = Widen(hit->s("name"));
            g.publisher = Widen(hit->s("publisher"));
            g.version = Widen(hit->s("version"));
            g.error = hit->s("error");
        } else if (keys.empty()) {
            g.error = "NO_KEYS";
        } else {
            nsw::Title t;
            std::string err;
            // Spanish, Latin American Spanish, then English.
            if (nsw::ReadTitle(f.file, keys, {6, 5, 0, 1}, t, err)) {
                g.titleId = t.titleId;
                if (!t.name.empty()) g.name = t.name;
                g.publisher = t.publisher;
                g.version = t.version;
                if (!t.icon.empty())
                    WriteAll(DataDir() + L"\\icons\\" + Widen(t.titleId) + L".jpg", t.icon.data(), t.icon.size());
            } else {
                g.error = err.empty() ? "READ_FAILED" : err;
            }
        }
        // A missing-keys failure is retried next time; everything else is cached.
        if (g.error != "NO_KEYS") {
            newCache += "  " + Quote(key) + ": {\"size\": " + std::to_string(f.size) + ", \"mtime\": " +
                        Quote(std::to_string(f.mtime)) + ", \"titleId\": " + Quote(g.titleId) + ", \"name\": " +
                        Quote(g.name) + ", \"publisher\": " + Quote(g.publisher) + ", \"version\": " +
                        Quote(g.version) + ", \"error\": " + Quote(g.error) + "},\n";
        }

        // A dump of updates or DLC alone is not a game, whatever its name; an
        // update even carries its game's id, and would stand in for the game
        // when its file comes first ("X UPDATE.nsp" sorts before "X.nsp").
        if (!keys.empty()) {
            content::Dump d;
            if (content::Probe(f.file, keys, d) && content::AddOnOnly(d)) continue;
        }
        // Updates and DLC dumped as separate files carry other title IDs and
        // would show up as bogus tiles; base games end in 000.
        if (!g.titleId.empty() && g.titleId.compare(g.titleId.size() - 3, 3, "000") != 0) continue;
        g.id = g.titleId.empty() ? "file:" + key : g.titleId;
        if (!seen.insert(g.id).second) continue;

        if (!g.titleId.empty()) {
            std::wstring icon = DataDir() + L"\\icons\\" + Widen(g.titleId) + L".jpg";
            if (Exists(icon)) g.icon.path = icon;
            Json meta;
            if (LoadJson(ryuGames + Widen(g.titleId) + L"\\gui\\metadata.json", meta)) {
                g.ryuLast = g.last = meta.s("last_played_utc");
                g.ryuSeconds = g.seconds = ParseSpan(meta.s("timespan_played"));
            }
        }
        // Row order follows the latest session by any user.
        const std::string suffix = "/" + g.id;
        for (const auto& p : played) {
            const std::string& k = p.first;
            if (k.size() > suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0)
                g.last = (std::max)(g.last, p.second.last);
        }
        out.push_back(std::move(g));
    }
    // The PC games: nothing to read from them here (name and picture were
    // taken when each was added), and their play time is all NSW-Home's own.
    for (const PcGame& p : programs) {
        Game g;
        g.id = p.id;
        g.pc = true;
        g.name = p.name;
        g.file = p.exe;
        g.args = p.args;
        if (Exists(PcIconFile(p.id))) g.icon.path = PcIconFile(p.id);
        const std::string suffix = "/" + g.id;
        for (const auto& pl : played) {
            const std::string& k = pl.first;
            if (k.size() <= suffix.size() || k.compare(k.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            g.last = (std::max)(g.last, pl.second.last);
            g.seconds += pl.second.seconds;
        }
        out.push_back(std::move(g));
    }
    if (newCache.size() > 2) newCache.erase(newCache.size() - 2, 1); // trailing comma
    newCache += "}\n";
    WriteAll(cachePath, newCache.data(), newCache.size());
    content::SaveCache();

    std::stable_sort(out.begin(), out.end(), [](const Game& a, const Game& b) {
        if (a.last != b.last) return a.last > b.last;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

std::wstring CaptureDir() {
    std::wstring dir = DataDir() + L"\\captures";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::string CaptureTag(const std::string& gameId) {
    std::string tag;
    for (unsigned char c : gameId) {
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
        if (plain) {
            tag += static_cast<char>(c);
        } else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            tag += hex;
        }
    }
    // A game that could not be read is known by its file's path; with the
    // folder in front of it that would not fit in a file name everywhere.
    if (tag.size() > 80) {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : gameId) h = (h ^ c) * 1099511628211ULL;
        char hex[20];
        snprintf(hex, sizeof hex, "~%016llx", static_cast<unsigned long long>(h));
        tag = hex;
    }
    return tag;
}

std::wstring NewCaptureFile(const std::string& gameId) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[40];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d-%02d_", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds / 10);
    return CaptureDir() + L"\\" + Widen(stamp + CaptureTag(gameId)) + L".jpg";
}

std::string CaptureTagOf(const std::wstring& file) {
    const std::wstring dir = Lower(CaptureDir()) + L"\\";
    if (file.size() < dir.size() + 23 || Lower(file.substr(0, dir.size())) != dir || !EndsWithI(file, L".jpg")) return {};
    // "YYYYMMDD-HHMMSS-cc_": 19 characters, then the tag.
    const std::wstring name = file.substr(dir.size());
    if (name[8] != L'-' || name[15] != L'-' || name[18] != L'_') return {};
    return Narrow(name.substr(19, name.size() - 23));
}

bool DeleteCapture(const std::wstring& file) {
    const std::wstring dir = Lower(CaptureDir()) + L"\\";
    if (file.size() <= dir.size() || Lower(file.substr(0, dir.size())) != dir) return false;
    if (file.find_first_of(L"\\/", dir.size()) != std::wstring::npos) return false; // the file itself, nothing below
    return DeleteFileW(file.c_str()) || !Exists(file);
}

std::vector<std::wstring> ListScreenshots() {
    std::vector<std::wstring> dirs;
    dirs.push_back(CaptureDir());
    wchar_t* pics = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) {
        dirs.push_back(std::wstring(pics) + L"\\Ryujinx");
        CoTaskMemFree(pics);
    }
    for (const auto& e : kEmus) dirs.push_back(EmuDataDir(e) + L"\\screenshots");

    std::vector<std::pair<uint64_t, std::wstring>> found;
    for (const auto& d : dirs)
        for (const auto& e : ListDir(d))
            if (!e.dir && (EndsWithI(e.name, L".png") || EndsWithI(e.name, L".jpg")))
                found.push_back({e.mtime, d + L"\\" + e.name});
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (found.size() > 200) found.resize(200);
    std::vector<std::wstring> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

// ---- playing together online (netplay; declared in ui.cpp's netplay block) ----

// Which emulator a game starts with: the same choice as GameCommand in
// main.cpp (the game's own, else the default, else any configured one).
std::string GameEmulator(const std::string& gameId) {
    std::string emuId = cfg.defaultEmu;
    auto per = cfg.perGame.find(gameId);
    if (per != cfg.perGame.end() && cfg.emulators.count(per->second)) emuId = per->second;
    if (!cfg.emulators.count(emuId)) emuId = cfg.emulators.empty() ? std::string() : cfg.emulators.begin()->first;
    return emuId;
}

// That emulator's Config.json, where EmuConfigPath would find it if it were
// the main one: a test run only ever sees the copy in <data>\<emulator id>.
std::wstring EmuConfigPathOf(const std::string& emuId) {
    const EmuDef* e = FindEmu(emuId);
    if (!e || e->family != 'r') return {};
    return EmuFiles(*e) + L"\\Config.json";
}
