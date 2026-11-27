// See content.h.
#include "content.h"
#include "nsw.h"
#include "nsw_core.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <system_error>

// Defined in nsw.cpp beside the rest of the dump reader (not in nsw_core.h,
// which its other users share): the NCAs a .nsp/.xci holds.
namespace nsw {
namespace core {
std::vector<Entry> openContainer(const View& file);
}
} // namespace nsw

namespace content {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- small things

// The rest of the program writes Windows paths on every platform.
fs::path P(const std::wstring& s) {
#ifdef _WIN32
    return fs::path(s);
#else
    std::wstring t = s;
    std::replace(t.begin(), t.end(), L'\\', L'/');
    return fs::path(t);
#endif
}

// A path as the emulator itself spells it in its files (its own system's separators).
std::wstring Native(const std::wstring& s) {
#ifdef _WIN32
    return s;
#else
    std::wstring t = s;
    std::replace(t.begin(), t.end(), L'\\', L'/');
    return t;
#endif
}

// And back: a path from the emulator's file in the program's own spelling.
std::wstring FromNative(const std::wstring& s) {
#ifdef _WIN32
    return s;
#else
    std::wstring t = s;
    std::replace(t.begin(), t.end(), L'/', L'\\');
    return t;
#endif
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    return a.back() == L'\\' || a.back() == L'/' ? a + b : a + L"\\" + b;
}

std::wstring LowerW(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

std::string LowerA(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string UpperA(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

bool EqI(const std::wstring& a, const std::wstring& b) {
    return LowerW(a) == LowerW(b);
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && LowerW(s.substr(s.size() - n)) == LowerW(suffix);
}

bool IsDir(const std::wstring& p) {
    std::error_code ec;
    return fs::is_directory(P(p), ec);
}

bool IsFile(const std::wstring& p) {
    std::error_code ec;
    return fs::is_regular_file(P(p), ec);
}

struct DirEntry {
    std::wstring name;
    bool dir = false;
};

// A folder's entries, in the order the system gives them (by name on NTFS,
// the order the emulators' own walks see too).
std::vector<DirEntry> List(const std::wstring& dir) {
    std::vector<DirEntry> out;
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code te;
        const bool d = it->is_directory(te);
        out.push_back({it->path().filename().wstring(), d && !te});
    }
    return out;
}

// The entry of `dir` with this name in any case ("" when there is none).
std::wstring FindI(const std::wstring& dir, const std::wstring& name, bool wantDir) {
    for (const DirEntry& e : List(dir))
        if (e.dir == wantDir && EqI(e.name, name)) return Join(dir, e.name);
    return {};
}

bool ReadFile(const std::wstring& path, std::string& out) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

// Through a temporary file that then takes the original's place in one step;
// the first write of a file that was there keeps "<file>.dockside-backup".
bool WriteFile(const std::wstring& path, const std::string& data) {
    std::error_code ec;
    const fs::path p = P(path), backup = P(path + L".dockside-backup"), tmp = P(path + L".dockside-tmp");
    if (fs::exists(p, ec) && !fs::exists(backup, ec)) {
        fs::copy_file(p, backup, ec);
        if (ec) return false; // no backup, no write
    }
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code ignore;
        fs::remove(tmp, ignore);
        return false;
    }
    return true;
}

bool RemoveFile(const std::wstring& path) {
    std::error_code ec;
    fs::remove(P(path), ec);
    return !ec;
}

// Makes the folder and the missing ones above it; the ones made are added to
// `made` (top first), so that they can be taken away again.
bool MakeDirs(const std::wstring& dir, std::vector<std::wstring>& made) {
    if (dir.empty() || IsDir(dir)) return !dir.empty();
    const size_t cut = dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos || cut == 0) return false;
    if (!MakeDirs(dir.substr(0, cut), made)) return false;
    std::error_code ec;
    fs::create_directory(P(dir), ec);
    if (ec || !IsDir(dir)) return false;
    made.push_back(dir);
    return true;
}

// ---------------------------------------------------------------- what NSW-Home added

// "<file>.dockside-made": one line per thing NSW-Home added to that file
// (UTF-8). "*" = the file itself; "dir:<folder>" = a folder made for it.
struct Made {
    std::wstring file;
    std::vector<std::string> items;
    explicit Made(const std::wstring& f) : file(f + L".dockside-made") {
        std::string text;
        if (!ReadFile(file, text)) return;
        size_t at = 0;
        while (at < text.size()) {
            size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(at, nl - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) items.push_back(line);
            at = nl + 1;
        }
    }
    bool has(const std::string& s) const { return std::find(items.begin(), items.end(), s) != items.end(); }
    void add(const std::string& s) {
        if (!has(s)) items.push_back(s);
    }
    void drop(const std::string& s) { items.erase(std::remove(items.begin(), items.end(), s), items.end()); }
    bool save() const {
        if (items.empty()) return !IsFile(file) || RemoveFile(file);
        std::string text;
        for (const std::string& s : items) text += s + "\n";
        std::string old;
        if (ReadFile(file, old) && old == text) return true;
        std::ofstream f(P(file), std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(f);
    }
    // Takes away the folders noted as made, deepest first, while they are empty.
    void dropDirs() {
        // This note lives in the folder too: with nothing else left to say, it goes first.
        if (std::all_of(items.begin(), items.end(), [](const std::string& s) { return s.compare(0, 4, "dir:") == 0; }))
            RemoveFile(file);
        std::vector<std::wstring> dirs;
        for (const std::string& s : items)
            if (s.compare(0, 4, "dir:") == 0) dirs.push_back(FromUtf8(s.substr(4)));
        std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
        for (const std::wstring& d : dirs) {
            std::error_code ec;
            if (IsDir(d) && fs::is_empty(P(d), ec) && !ec) fs::remove(P(d), ec);
            drop("dir:" + ToUtf8(d));
        }
    }
};

// ---------------------------------------------------------------- JSON

// Enough JSON for the emulators' small files: the tree keeps the order of the
// keys and the text of each number, so what is not changed is written back
// as it was.
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    std::string s; // Num: its text; Str: the value (UTF-8)
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    J* get(const char* k) {
        for (auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    const J* get(const char* k) const {
        for (const auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    static J str(const std::string& v) {
        J j;
        j.t = Str;
        j.s = v;
        return j;
    }
    static J boolean(bool v) {
        J j;
        j.t = Bool;
        j.b = v;
        return j;
    }
    static J num(const std::string& v) {
        J j;
        j.t = Num;
        j.s = v;
        return j;
    }
    static J arr() {
        J j;
        j.t = Arr;
        return j;
    }
    static J obj() {
        J j;
        j.t = Obj;
        return j;
    }
};

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit Parser(const std::string& text) : s(text) {
        if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
            static_cast<unsigned char>(s[2]) == 0xBF)
            i = 3;
    }
    // Whitespace, and the comments the Ryujinx family's reader accepts.
    void ws() {
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') i++;
            } else if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
                size_t e = s.find("*/", i + 2);
                i = e == std::string::npos ? s.size() : e + 2;
            } else {
                return;
            }
        }
    }
    static void put(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
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
    uint32_t hex4() {
        if (i + 4 > s.size()) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else ok = false;
        }
        return v;
    }
    std::string string() {
        std::string out;
        if (i >= s.size() || s[i] != '"') {
            ok = false;
            return out;
        }
        i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            c = s[i++];
            switch (c) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    const uint32_t lo = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put(out, cp);
                break;
            }
            default: out += c; break;
            }
        }
        if (i >= s.size()) ok = false;
        else i++;
        return out;
    }
    J value(int depth) {
        J j;
        ws();
        if (i >= s.size() || depth > 64) {
            ok = false;
            return j;
        }
        const char c = s[i];
        if (c == '{') {
            j.t = J::Obj;
            i++;
            for (;;) {
                ws();
                if (i < s.size() && s[i] == '}') {
                    i++;
                    break;
                }
                std::string k = string();
                ws();
                if (!ok || i >= s.size() || s[i] != ':') {
                    ok = false;
                    return j;
                }
                i++;
                J v = value(depth + 1);
                if (!ok) return j;
                j.o.emplace_back(std::move(k), std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    i++;
                    break;
                }
                ok = false;
                return j;
            }
        } else if (c == '[') {
            j.t = J::Arr;
            i++;
            for (;;) {
                ws();
                if (i < s.size() && s[i] == ']') {
                    i++;
                    break;
                }
                J v = value(depth + 1);
                if (!ok) return j;
                j.a.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    i++;
                    break;
                }
                ok = false;
                return j;
            }
        } else if (c == '"') {
            j.t = J::Str;
            j.s = string();
        } else if (s.compare(i, 4, "true") == 0) {
            j.t = J::Bool;
            j.b = true;
            i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            j.t = J::Bool;
            i += 5;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            j.t = J::Num;
            const size_t from = i;
            while (i < s.size() && (s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    (s[i] >= '0' && s[i] <= '9')))
                i++;
            j.s = s.substr(from, i - from);
        } else {
            ok = false;
        }
        return j;
    }
};

bool ParseJson(const std::string& text, J& out) {
    Parser p(text);
    out = p.value(0);
    p.ws();
    return p.ok && p.i == text.size();
}

// The text System.Text.Json writes with its default encoder, as the Ryujinx
// family's files show it ("DynamicFPS++"): everything outside
// printable ASCII and the characters HTML gives a meaning to as \uXXXX in
// capitals; the usual short forms for the rest.
void Escape(std::string& out, const std::string& v) {
    auto u = [&](uint32_t cp) {
        char b[8];
        snprintf(b, sizeof b, "\\u%04X", cp);
        out += b;
    };
    for (size_t i = 0; i < v.size();) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < v.size()) {
            cp = ((c & 0x07u) << 18) | ((v[i + 1] & 0x3Fu) << 12) | ((v[i + 2] & 0x3Fu) << 6) | (v[i + 3] & 0x3Fu);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < v.size()) {
            cp = ((c & 0x0Fu) << 12) | ((v[i + 1] & 0x3Fu) << 6) | (v[i + 2] & 0x3Fu);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < v.size()) {
            cp = ((c & 0x1Fu) << 6) | (v[i + 1] & 0x3Fu);
            n = 2;
        }
        i += n;
        switch (cp) {
        case '\\': out += "\\\\"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\t': out += "\\t"; continue;
        case '\b': out += "\\b"; continue;
        case '\f': out += "\\f"; continue;
        case '"': case '&': case '\'': case '<': case '>': case '+': case '`': u(cp); continue;
        default: break;
        }
        if (cp < 0x20 || cp >= 0x7F) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                u(0xD800 + (cp >> 10));
                u(0xDC00 + (cp & 0x3FF));
            } else {
                u(cp);
            }
        } else {
            out += static_cast<char>(cp);
        }
    }
}

// Indented with two spaces, ": " after a key, empty lists as "[]", no line
// break at the end: what the emulator writes. `nl` is its line ending.
void Emit(std::string& out, const J& j, int depth, const std::string& nl) {
    const std::string pad(static_cast<size_t>(depth + 1) * 2, ' '), close(static_cast<size_t>(depth) * 2, ' ');
    switch (j.t) {
    case J::Null: out += "null"; break;
    case J::Bool: out += j.b ? "true" : "false"; break;
    case J::Num: out += j.s; break;
    case J::Str:
        out += '"';
        Escape(out, j.s);
        out += '"';
        break;
    case J::Arr:
        if (j.a.empty()) {
            out += "[]";
            break;
        }
        out += "[" + nl;
        for (size_t k = 0; k < j.a.size(); k++) {
            out += pad;
            Emit(out, j.a[k], depth + 1, nl);
            out += (k + 1 < j.a.size() ? "," : "") + nl;
        }
        out += close + "]";
        break;
    case J::Obj:
        if (j.o.empty()) {
            out += "{}";
            break;
        }
        out += "{" + nl;
        for (size_t k = 0; k < j.o.size(); k++) {
            out += pad + '"';
            Escape(out, j.o[k].first);
            out += "\": ";
            Emit(out, j.o[k].second, depth + 1, nl);
            out += (k + 1 < j.o.size() ? "," : "") + nl;
        }
        out += close + "}";
        break;
    }
}

#ifdef _WIN32
const char* const kNl = "\r\n";
#else
const char* const kNl = "\n";
#endif

// The line ending the file uses; the system's for a new one (.NET writes
// Environment.NewLine).
std::string NlOf(const std::string& text) {
    if (text.find("\r\n") != std::string::npos) return "\r\n";
    if (text.find('\n') != std::string::npos) return "\n";
    return kNl;
}

// A JSON file of the emulator's: `exists` false when it is not there; false
// returned when it is there but cannot be understood (then nothing may be
// written over it).
bool LoadJson(const std::wstring& path, J& doc, std::string& text, bool& exists) {
    exists = ReadFile(path, text);
    if (!exists) return true;
    return ParseJson(text, doc);
}

bool StoreJson(const std::wstring& path, const J& doc, const std::string& oldText, bool existed) {
    std::string out;
    Emit(out, doc, 0, existed ? NlOf(oldText) : std::string(kNl));
    if (existed && out == oldText) return true;
    return WriteFile(path, out);
}

std::wstring StrW(const J* j) {
    return j && j->t == J::Str ? FromUtf8(j->s) : std::wstring();
}

uint64_t NumU64(const J* j) {
    if (!j) return 0;
    if (j->t == J::Num || j->t == J::Str) return strtoull(j->s.c_str(), nullptr, 10);
    return 0;
}

// ---------------------------------------------------------------- reading dumps

std::mutex cacheLock;
std::wstring cacheFile;
bool cacheLoaded = false, cacheDirty = false;
std::map<std::string, Dump> cache; // key: the path in lower case (UTF-8)

void LoadCacheLocked() {
    if (cacheLoaded) return;
    cacheLoaded = true;
    std::string text;
    J doc;
    if (cacheFile.empty() || !ReadFile(cacheFile, text) || !ParseJson(text, doc) || doc.t != J::Obj) return;
    for (const auto& kv : doc.o) {
        const J& e = kv.second;
        Dump d;
        d.file = StrW(e.get("file"));
        d.size = NumU64(e.get("size"));
        d.mtime = NumU64(e.get("mtime"));
        if (const J* er = e.get("error")) d.error = er->s;
        if (const J* ms = e.get("metas"))
            for (const J& m : ms->a) {
                Meta x;
                x.type = static_cast<uint8_t>(NumU64(m.get("type")));
                if (const J* v = m.get("id")) ParseHex16(v->s, x.id);
                if (const J* v = m.get("app")) ParseHex16(v->s, x.app);
                x.version = static_cast<uint32_t>(NumU64(m.get("version")));
                x.display = StrW(m.get("display"));
                if (const J* v = m.get("nca")) x.nca = v->s;
                if (const J* v = m.get("ncaTitle")) ParseHex16(v->s, x.ncaTitle);
                d.metas.push_back(x);
            }
        cache[kv.first] = d;
    }
}

std::string HexOf(const uint8_t* p, size_t n) {
    static const char* const h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += h[p[i] >> 4];
        s += h[p[i] & 15];
    }
    return s;
}

using namespace nsw::core;

bool EndsWithA(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && LowerA(s.substr(s.size() - n)) == suffix;
}

const Entry* EntryOf(const Context& ctx, const std::string& ncaId) {
    for (const Entry& e : ctx.entries)
        if (LowerA(e.name).compare(0, ncaId.size(), ncaId) == 0 && EndsWithA(e.name, ".nca")) return &e;
    return nullptr;
}

// The display version an update's control data carries ("1.6.0").
std::wstring PatchDisplay(Context& ctx, const std::string& controlId) {
    const Entry* e = EntryOf(ctx, controlId);
    if (!e) return {};
    Nca nca = openNca(ctx, *e);
    for (const Section& s : nca.sections) {
        if (s.fsType != 0) continue;
        View v = romfsView(ctx, nca, s);
        auto files = listRomfs(v);
        auto it = files.find("control.nacp");
        if (it == files.end() || it->second.size < 0x3070) return {};
        Bytes b = v.read(it->second.offset + 0x3060, 0x10);
        std::string t(reinterpret_cast<const char*>(b.data()), strnlen(reinterpret_cast<const char*>(b.data()), 0x10));
        while (!t.empty() && (t.back() == ' ')) t.pop_back();
        return FromUtf8(t);
    }
    return {};
}

void ProbeImpl(const std::wstring& file, const std::wstring& keysDir, Dump& d) {
    Context ctx;
    ctx.keys.dir = keysDir;
    if (keysDir.empty() || !loadKeyFile(joinPath(keysDir, L"prod.keys"), ctx.keys.prod) || ctx.keys.prod.empty())
        fail("NO_KEYS");
    if (!openFileView(file, ctx.file)) fail("FILE_ERROR");
    ctx.entries = openContainer(ctx.file);
    for (const Entry& e : ctx.entries)
        if (e.offset > ctx.file.size || e.size > ctx.file.size - e.offset) fail("BAD_FORMAT");
    for (const Entry& e : ctx.entries) {
        if (!EndsWithA(e.name, ".cnmt.nca")) continue;
        Nca nca = openNca(ctx, e);
        const Section* sec = nullptr;
        for (const Section& s : nca.sections)
            if (s.fsType == 1) {
                sec = &s;
                break;
            }
        if (!sec) continue;
        View view = pfs0View(ctx, nca, *sec);
        for (const Entry& f : parsePartition(view, "PFS0")) {
            if (!EndsWithA(f.name, ".cnmt") || f.size < 0x20 || f.size > 0x100000) continue;
            Bytes c = view.read(f.offset, static_cast<size_t>(f.size));
            Meta m;
            m.id = rd64(&c[0]);
            m.version = rd32(&c[8]);
            m.type = c[0x0c];
            const size_t ext = rd16(&c[0x0e]), count = rd16(&c[0x10]);
            // The extended header of an update and of a DLC begins with the
            // id of the game it belongs to.
            m.app = m.id;
            if ((m.type == kUpdate || m.type == kDlc) && ext >= 8 && c.size() >= 0x28) m.app = rd64(&c[0x20]);
            std::string control, data;
            for (size_t i = 0; i < count; i++) {
                const size_t r = 0x20 + ext + i * 0x38;
                if (r + 0x38 > c.size()) break;
                const uint8_t type = c[r + 0x36];
                if (type == 3 && control.empty()) control = HexOf(&c[r + 0x20], 16);
                if (type == 2 && data.empty()) data = HexOf(&c[r + 0x20], 16);
            }
            if (m.type == kUpdate && !control.empty()) {
                try {
                    m.display = PatchDisplay(ctx, control);
                } catch (const Fail&) {
                    // without its version text the update is still told apart by its number
                }
            }
            if (m.type == kDlc && !data.empty()) {
                if (const Entry* de = EntryOf(ctx, data)) {
                    // What Ryujinx stores: the entry's name in the container, and
                    // the title id its header carries.
                    m.nca = "/" + de->name;
                    try {
                        Nca dn = openNca(ctx, *de);
                        m.ncaTitle = dn.programId;
                    } catch (const Fail&) {
                        m.ncaTitle = m.id;
                    }
                }
            }
            d.metas.push_back(m);
        }
    }
    if (d.metas.empty()) fail("NO_META");
}

} // namespace

// ---------------------------------------------------------------- helpers

std::string Hex16(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

bool ParseHex16(const std::string& s, uint64_t& v) {
    if (s.empty() || s.size() > 16) return false;
    uint64_t r = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        r = (r << 4) | static_cast<uint64_t>(d);
    }
    v = r;
    return true;
}

std::string ToUtf8(const std::wstring& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = static_cast<uint32_t>(s[i]);
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size()) {
            const uint32_t lo = static_cast<uint32_t>(s[i + 1]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        Parser::put(out, cp);
    }
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
            n = 2;
        }
        i += n;
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<wchar_t>(cp);
        }
    }
    return out;
}

// ---------------------------------------------------------------- dumps

void SetCache(const std::wstring& file) {
    std::lock_guard<std::mutex> g(cacheLock);
    if (file == cacheFile) return;
    cacheFile = file;
    cacheLoaded = false;
    cache.clear();
}

bool Probe(const std::wstring& file, const std::wstring& keysDir, Dump& out) {
    std::error_code ec;
    const fs::path p = P(file);
    const uint64_t size = fs::file_size(p, ec);
    if (ec) return false;
    const uint64_t mtime = static_cast<uint64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
    const std::string key = ToUtf8(LowerW(file));
    {
        std::lock_guard<std::mutex> g(cacheLock);
        LoadCacheLocked();
        auto it = cache.find(key);
        if (it != cache.end() && it->second.size == size && it->second.mtime == mtime) {
            out = it->second;
            out.file = file;
            return out.error.empty();
        }
    }
    Dump d;
    d.file = file;
    d.size = size;
    d.mtime = mtime;
    try {
        ProbeImpl(file, keysDir, d);
    } catch (const nsw::core::Fail& f) {
        d.metas.clear();
        d.error = f.code;
    } catch (...) {
        d.metas.clear();
        d.error = "INTERNAL";
    }
    // Missing keys may be put right later: such a failure is not remembered.
    if (d.error != "NO_KEYS") {
        std::lock_guard<std::mutex> g(cacheLock);
        cache[key] = d;
        cacheDirty = true;
    }
    out = d;
    return d.error.empty();
}

void SaveCache() {
    std::lock_guard<std::mutex> g(cacheLock);
    if (!cacheDirty || cacheFile.empty()) return;
    J doc = J::obj();
    for (const auto& kv : cache) {
        const Dump& d = kv.second;
        J e = J::obj();
        e.o.emplace_back("file", J::str(ToUtf8(d.file)));
        e.o.emplace_back("size", J::num(std::to_string(d.size)));
        e.o.emplace_back("mtime", J::num(std::to_string(d.mtime)));
        e.o.emplace_back("error", J::str(d.error));
        J ms = J::arr();
        for (const Meta& m : d.metas) {
            J x = J::obj();
            x.o.emplace_back("type", J::num(std::to_string(m.type)));
            x.o.emplace_back("id", J::str(Hex16(m.id)));
            x.o.emplace_back("app", J::str(Hex16(m.app)));
            x.o.emplace_back("version", J::num(std::to_string(m.version)));
            x.o.emplace_back("display", J::str(ToUtf8(m.display)));
            x.o.emplace_back("nca", J::str(m.nca));
            x.o.emplace_back("ncaTitle", J::str(Hex16(m.ncaTitle)));
            ms.a.push_back(x);
        }
        e.o.emplace_back("metas", ms);
        doc.o.emplace_back(kv.first, e);
    }
    std::string out;
    Emit(out, doc, 0, "\n");
    std::ofstream f(P(cacheFile), std::ios::binary | std::ios::trunc);
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (f) cacheDirty = false;
}

bool AddOnOnly(const Dump& d) {
    if (d.metas.empty()) return false;
    for (const Meta& m : d.metas)
        if (m.type == kGame) return false;
    return true;
}

namespace {
// Windows takes both slashes and any case: one spelling for telling files apart.
std::wstring SameKey(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return LowerW(p);
}
// The program's own spelling of a path the emulator wrote with forward slashes.
std::wstring Backslashed(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return p;
}
} // namespace

std::vector<Dump> Collect(const std::vector<std::wstring>& dirs, int depth, const std::wstring& keysDir,
                          const std::vector<std::wstring>& files) {
    std::vector<Dump> out;
    std::set<std::wstring> seen;
    std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& dir, int left) {
        for (const DirEntry& e : List(dir)) {
            const std::wstring full = Join(dir, e.name);
            if (e.dir) {
                if (left > 0) walk(full, left - 1);
            } else if ((EndsWithI(e.name, L".nsp") || EndsWithI(e.name, L".xci")) && seen.insert(SameKey(full)).second) {
                Dump d;
                Probe(full, keysDir, d);
                out.push_back(d);
            }
        }
    };
    for (const std::wstring& d : dirs) walk(d, depth);
    for (const std::wstring& f : files) {
        if (f.empty() || !seen.insert(SameKey(f)).second || !IsFile(f)) continue;
        Dump d;
        Probe(f, keysDir, d);
        out.push_back(d);
    }
    return out;
}

std::wstring DlcName(const std::wstring& file) {
    size_t slash = file.find_last_of(L"\\/");
    std::wstring stem = slash == std::wstring::npos ? file : file.substr(slash + 1);
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);
    // A tag that only says what the file is: a title id, a version, a region,
    // a count of items, a site.
    auto bare = [](const std::wstring& t) {
        if (t.empty()) return true;
        bool hex = t.size() == 16;
        for (wchar_t c : t) hex = hex && ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'));
        if (hex) return true;
        if ((t[0] == L'v' || t[0] == L'V') && t.size() > 1 && t.find_first_not_of(L"0123456789", 1) == std::wstring::npos)
            return true;
        if (t.size() <= 3 && t.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::wstring::npos) return true;
        if (t.find(L'.') != std::wstring::npos && t.find(L' ') == std::wstring::npos) return true;
        return false;
    };
    std::wstring plain, tags;
    for (size_t i = 0; i < stem.size();) {
        const wchar_t c = stem[i];
        if (c == L'[' || c == L'(') {
            const size_t end = stem.find(c == L'[' ? L']' : L')', i + 1);
            if (end == std::wstring::npos) {
                plain += stem.substr(i);
                break;
            }
            const std::wstring t = stem.substr(i + 1, end - i - 1);
            if (!bare(t)) tags += (tags.empty() ? L"" : L" · ") + t;
            i = end + 1;
            continue;
        }
        plain += c;
        i++;
    }
    std::wstring out = tags.empty() ? plain : tags;
    for (wchar_t& c : out)
        if (c == L'_') c = L' ';
    std::wstring tidy;
    for (wchar_t c : out)
        if (!(c == L' ' && (tidy.empty() || tidy.back() == L' '))) tidy += c;
    while (!tidy.empty() && (tidy.back() == L' ' || tidy.back() == L'-')) tidy.pop_back();
    return tidy.empty() ? stem : tidy;
}

std::wstring VersionText(const Meta& m) {
    if (!m.display.empty()) return m.display;
    return L"v" + std::to_wstring(m.version);
}

// ---------------------------------------------------------------- Ryujinx family

namespace {

// The cheats of one file, as the Ryujinx family reads them: "[name]" begins
// a cheat; lines before the first one form "<default>"; a malformed "[" line
// makes the whole file count for nothing.
std::vector<std::wstring> RyuCheatNames(const std::wstring& file) {
    std::string text;
    std::vector<std::wstring> out;
    if (!ReadFile(file, text)) return out;
    std::string name = "<default>";
    bool any = false;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) lead++;
        line = line.substr(lead);
        if (!line.empty() && line[0] == '[') {
            if (line.back() != ']' || line.size() < 3) return {};
            if (any) out.push_back(FromUtf8(name));
            name = line.substr(1, line.size() - 2);
            any = false;
        } else if (!line.empty()) {
            any = true;
        }
        if (nl == text.size()) break;
    }
    if (any) out.push_back(FromUtf8(name));
    return out;
}

bool Hex16Name(const std::wstring& stem) {
    if (stem.size() != 16) return false;
    for (wchar_t c : stem)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
    return true;
}

// Walks one title's mod folder as the Ryujinx family's mod loader does: a
// folder holding romfs\ or exefs\ is a mod (named after it), cheats\ is a
// folder of cheats, anything else is looked into.
void RyuWalk(const std::wstring& dir, std::vector<Mod>& mods, std::vector<std::wstring>& cheatDirs, int depth) {
    if (depth > 16) return;
    for (const DirEntry& e : List(dir)) {
        if (!e.dir) continue;
        const std::wstring full = Join(dir, e.name);
        const std::wstring n = LowerW(e.name);
        if (n == L"romfs" || n == L"exefs") {
            auto it = std::find_if(mods.begin(), mods.end(), [&](const Mod& m) { return m.path == dir; });
            if (it == mods.end()) {
                Mod m;
                const size_t s = dir.find_last_of(L"\\/");
                m.name = s == std::wstring::npos ? dir : dir.substr(s + 1);
                m.path = dir;
                mods.push_back(m);
                it = mods.end() - 1;
            }
            it->kinds += (it->kinds.empty() ? L"" : L", ") + n;
        } else if (n == L"cheats") {
            cheatDirs.push_back(full);
        } else {
            RyuWalk(full, mods, cheatDirs, depth + 1);
        }
    }
}

std::vector<std::wstring> RyuTitleDirs(const std::wstring& dir, uint64_t titleId) {
    std::vector<std::wstring> out;
    for (const wchar_t* root : {L"mods\\contents", L"sdcard\\atmosphere\\contents"}) {
        const std::wstring t = FindI(Join(dir, root), FromUtf8(Hex16(titleId)), true);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

void RyuCollect(const std::wstring& dir, uint64_t titleId, std::vector<Mod>& mods, std::vector<std::wstring>& cheatDirs) {
    for (const std::wstring& t : RyuTitleDirs(dir, titleId)) {
        // romfs.bin / exefs.nsp right in the title's folder are mods of their own.
        for (const DirEntry& e : List(t))
            if (!e.dir && (EqI(e.name, L"romfs.bin") || EqI(e.name, L"exefs.nsp"))) {
                Mod m;
                m.name = e.name;
                m.path = Join(t, e.name);
                m.kinds = LowerW(e.name);
                mods.push_back(m);
            }
        RyuWalk(t, mods, cheatDirs, 0);
    }
}

std::wstring EnabledTxt(const std::wstring& dir, uint64_t titleId) {
    return Join(ryu::ModsDir(dir, titleId), L"cheats\\enabled.txt");
}

} // namespace

namespace ryu {

std::wstring GameDir(const std::wstring& dir, uint64_t titleId) {
    return Join(dir, L"games\\" + FromUtf8(Hex16(titleId)));
}

std::wstring ModsDir(const std::wstring& dir, uint64_t titleId) {
    const std::wstring contents = Join(dir, L"mods\\contents");
    const std::wstring t = FindI(contents, FromUtf8(Hex16(titleId)), true);
    return t.empty() ? Join(contents, FromUtf8(Hex16(titleId))) : t;
}

State Read(const std::wstring& dir, uint64_t titleId) {
    State st;
    J doc;
    std::string text;
    bool exists = false;
    if (LoadJson(Join(GameDir(dir, titleId), L"updates.json"), doc, text, exists) && exists) {
        st.selected = FromNative(StrW(doc.get("selected")));
        if (const J* ps = doc.get("paths"))
            for (const J& p : ps->a) st.paths.push_back(FromNative(StrW(&p)));
    }
    J dl;
    if (LoadJson(Join(GameDir(dir, titleId), L"dlc.json"), dl, text, exists) && exists && dl.t == J::Arr)
        for (const J& c : dl.a) {
            const std::wstring file = FromNative(StrW(c.get("path")));
            if (const J* list = c.get("dlc_nca_list"))
                for (const J& n : list->a) {
                    State::Dlc d;
                    d.file = file;
                    if (const J* p = n.get("path")) d.nca = p->s;
                    d.id = NumU64(n.get("title_id"));
                    const J* on = n.get("is_enabled");
                    d.on = on && on->t == J::Bool && on->b;
                    st.dlc.push_back(d);
                }
        }
    return st;
}

bool SelectUpdate(const std::wstring& dir, uint64_t titleId, const std::wstring& fileIn) {
    const std::wstring gdir = GameDir(dir, titleId), path = Join(gdir, L"updates.json");
    const std::string file = ToUtf8(Native(fileIn));
    J doc;
    std::string text;
    bool exists = false;
    if (!LoadJson(path, doc, text, exists) || (exists && doc.t != J::Obj)) return false;
    Made made(path);
    if (!exists) {
        if (file.empty()) return true; // no file: already the game without update
        std::vector<std::wstring> dirs;
        if (!MakeDirs(gdir, dirs)) return false;
        for (const std::wstring& d : dirs) made.add("dir:" + ToUtf8(d));
        doc = J::obj();
        made.add("*");
    }
    if (!doc.get("selected")) doc.o.emplace_back("selected", J::str(""));
    if (!doc.get("paths")) doc.o.emplace_back("paths", J::arr());
    J& paths = *doc.get("paths");
    if (paths.t != J::Arr) return false;
    *doc.get("selected") = J::str(file);
    auto listed = [&](const std::string& p) {
        return std::any_of(paths.a.begin(), paths.a.end(), [&](const J& j) { return j.t == J::Str && j.s == p; });
    };
    if (!file.empty() && !listed(file)) {
        paths.a.push_back(J::str(file));
        made.add("path:" + file);
    }
    // What NSW-Home listed for an earlier choice goes again.
    for (const std::string& m : std::vector<std::string>(made.items)) {
        if (m.compare(0, 5, "path:") != 0 || m.substr(5) == file) continue;
        const std::string p = m.substr(5);
        paths.a.erase(std::remove_if(paths.a.begin(), paths.a.end(), [&](const J& j) { return j.t == J::Str && j.s == p; }),
                      paths.a.end());
        made.drop(m);
    }
    if (made.has("*") && file.empty() && paths.a.empty()) {
        if (!RemoveFile(path)) return false;
        made.drop("*");
        made.dropDirs();
        return made.save();
    }
    return StoreJson(path, doc, text, exists) && made.save();
}

bool SetDlc(const std::wstring& dir, uint64_t titleId, const std::wstring& fileIn, const std::string& nca, uint64_t dlcId, bool on) {
    const std::wstring gdir = GameDir(dir, titleId), path = Join(gdir, L"dlc.json");
    const std::string file = ToUtf8(Native(fileIn));
    J doc;
    std::string text;
    bool exists = false;
    if (!LoadJson(path, doc, text, exists) || (exists && doc.t != J::Arr)) return false;
    Made made(path);
    const std::string tag = "dlc:" + file + "|" + std::to_string(dlcId), ctag = "container:" + file;
    // The container and the item, matched as the emulator does: the
    // container by its path, the item by its title id.
    J* cont = nullptr;
    for (J& c : doc.a) {
        const J* p = c.get("path");
#ifdef _WIN32
        if (p && EqI(FromUtf8(p->s), FromUtf8(file))) {
#else
        if (p && p->s == file) {
#endif
            cont = &c;
            break;
        }
    }
    J* item = nullptr;
    if (cont)
        if (J* list = cont->get("dlc_nca_list"))
            for (J& n : list->a)
                if (NumU64(n.get("title_id")) == dlcId) item = &n;
    if (item) {
        if (!on && made.has(tag)) {
            J& list = *cont->get("dlc_nca_list");
            list.a.erase(std::remove_if(list.a.begin(), list.a.end(), [&](const J& n) { return NumU64(n.get("title_id")) == dlcId; }),
                         list.a.end());
            made.drop(tag);
            if (list.a.empty() && made.has(ctag)) {
                doc.a.erase(std::remove_if(doc.a.begin(), doc.a.end(), [&](const J& c) { return &c == cont; }), doc.a.end());
                made.drop(ctag);
            }
        } else {
            if (J* e = item->get("is_enabled")) *e = J::boolean(on);
            else item->o.emplace_back("is_enabled", J::boolean(on));
        }
    } else {
        if (!on) return true; // not listed: the emulator does not load it
        if (!exists) {
            std::vector<std::wstring> dirs;
            if (!MakeDirs(gdir, dirs)) return false;
            for (const std::wstring& d : dirs) made.add("dir:" + ToUtf8(d));
            doc = J::arr();
            made.add("*");
        }
        if (!cont) {
            J c = J::obj();
            c.o.emplace_back("path", J::str(file));
            c.o.emplace_back("dlc_nca_list", J::arr());
            doc.a.push_back(c);
            cont = &doc.a.back();
            made.add(ctag);
        }
        if (!cont->get("dlc_nca_list")) cont->o.emplace_back("dlc_nca_list", J::arr());
        J n = J::obj();
        n.o.emplace_back("path", J::str(nca));
        n.o.emplace_back("title_id", J::num(std::to_string(dlcId)));
        n.o.emplace_back("is_enabled", J::boolean(true));
        cont->get("dlc_nca_list")->a.push_back(n);
        made.add(tag);
    }
    if (made.has("*") && doc.a.empty()) {
        if (!RemoveFile(path)) return false;
        made.drop("*");
        made.dropDirs();
        return made.save();
    }
    return StoreJson(path, doc, text, exists) && made.save();
}

std::vector<Mod> Mods(const std::wstring& dir, uint64_t titleId) {
    std::vector<Mod> mods;
    std::vector<std::wstring> cheatDirs;
    RyuCollect(dir, titleId, mods, cheatDirs);
    J doc;
    std::string text;
    bool exists = false;
    if (LoadJson(Join(GameDir(dir, titleId), L"mods.json"), doc, text, exists) && exists)
        if (const J* list = doc.get("mods"))
            for (Mod& m : mods) {
                const std::string want = ToUtf8(Native(m.path));
                const J* hit = nullptr;
                // Ryubing and Kenji-NX: the same path; Ryujinx 1.1.1403: the
                // first entry whose path is inside the mod's own.
                for (const J& e : list->a)
                    if (const J* p = e.get("path"))
                        if (p->s == want) hit = hit ? hit : &e;
                if (!hit)
                    for (const J& e : list->a)
                        if (const J* p = e.get("path"))
                            if (!p->s.empty() && want.find(p->s) != std::string::npos) {
                                hit = &e;
                                break;
                            }
                if (hit)
                    if (const J* en = hit->get("enabled")) m.on = en->t == J::Bool && en->b;
            }
    return mods;
}

bool SetMod(const std::wstring& dir, uint64_t titleId, const Mod& mod, bool on) {
    const std::wstring gdir = GameDir(dir, titleId), path = Join(gdir, L"mods.json");
    const std::string want = ToUtf8(Native(mod.path));
    J doc;
    std::string text;
    bool exists = false;
    if (!LoadJson(path, doc, text, exists) || (exists && doc.t != J::Obj)) return false;
    Made made(path);
    const std::string tag = "mod:" + want;
    if (!exists) {
        if (on) return true; // not listed: on
        std::vector<std::wstring> dirs;
        if (!MakeDirs(gdir, dirs)) return false;
        for (const std::wstring& d : dirs) made.add("dir:" + ToUtf8(d));
        doc = J::obj();
        made.add("*");
    }
    if (!doc.get("mods")) doc.o.emplace_back("mods", J::arr());
    J& list = *doc.get("mods");
    if (list.t != J::Arr) return false;
    auto it = std::find_if(list.a.begin(), list.a.end(), [&](const J& e) {
        const J* p = e.get("path");
        return p && p->s == want;
    });
    if (it != list.a.end()) {
        if (on && made.has(tag)) {
            list.a.erase(it);
            made.drop(tag);
        } else if (J* en = it->get("enabled")) {
            *en = J::boolean(on);
        } else {
            it->o.emplace_back("enabled", J::boolean(on));
        }
    } else if (!on) {
        J e = J::obj();
        e.o.emplace_back("name", J::str(ToUtf8(mod.name)));
        e.o.emplace_back("path", J::str(want));
        e.o.emplace_back("enabled", J::boolean(false));
        // Ryujinx 1.1.1403 takes the first entry whose path the mod's folder
        // contains: one for a longer path goes before a shorter one inside it.
        auto at = std::find_if(list.a.begin(), list.a.end(), [&](const J& o) {
            const J* p = o.get("path");
            return p && p->s.size() < want.size() && want.compare(0, p->s.size(), p->s) == 0;
        });
        list.a.insert(at, e);
        made.add(tag);
    }
    if (made.has("*") && list.a.empty()) {
        if (!RemoveFile(path)) return false;
        made.drop("*");
        made.dropDirs();
        return made.save();
    }
    return StoreJson(path, doc, text, exists) && made.save();
}

std::vector<Cheat> Cheats(const std::wstring& dir, uint64_t titleId) {
    std::vector<Mod> mods;
    std::vector<std::wstring> cheatDirs;
    RyuCollect(dir, titleId, mods, cheatDirs);
    std::vector<std::string> enabled;
    std::string text;
    if (ReadFile(EnabledTxt(dir, titleId), text)) {
        size_t at = 0;
        while (at < text.size()) {
            size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(at, nl - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            enabled.push_back(line);
            at = nl + 1;
        }
    }
    std::vector<Cheat> out;
    for (const std::wstring& cd : cheatDirs)
        for (const DirEntry& e : List(cd)) {
            if (e.dir || !EndsWithI(e.name, L".txt")) continue;
            const std::wstring stem = e.name.substr(0, e.name.size() - 4);
            if (!Hex16Name(stem)) continue;
            const std::wstring file = Join(cd, e.name);
            for (const std::wstring& n : RyuCheatNames(file)) {
                Cheat c;
                c.name = n;
                c.file = file;
                // The emulator's key: the build id in capitals, then its own
                // name for the cheat.
                c.key = UpperA(ToUtf8(stem)) + "-<" + ToUtf8(n) + " Cheat>";
                c.on = std::find(enabled.begin(), enabled.end(), c.key) != enabled.end();
                out.push_back(c);
            }
        }
    return out;
}

bool SetCheat(const std::wstring& dir, uint64_t titleId, const std::string& key, bool on) {
    const std::wstring path = EnabledTxt(dir, titleId);
    std::string text;
    const bool exists = ReadFile(path, text);
    Made made(path);
    // An emptied file keeps no line ending to go by: the note remembers it.
    const std::string nl = exists && !text.empty() ? NlOf(text) : made.has("nl:crlf") ? "\r\n" : made.has("nl:lf") ? "\n" : std::string(kNl);
    made.drop("nl:crlf");
    made.drop("nl:lf");
    // Lines with their endings, as they are.
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        size_t e = text.find('\n', at);
        e = e == std::string::npos ? text.size() : e + 1;
        lines.push_back(text.substr(at, e - at));
        at = e;
    }
    auto bare = [](std::string l) {
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        return l;
    };
    const bool listed = std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return bare(l) == key; });
    if (on == listed) return true;
    if (on) {
        if (!exists) {
            std::vector<std::wstring> dirs;
            if (!MakeDirs(path.substr(0, path.find_last_of(L"\\/")), dirs)) return false;
            for (const std::wstring& d : dirs) made.add("dir:" + ToUtf8(d));
            made.add("*");
        }
        if (!lines.empty() && lines.back().back() != '\n') lines.back() += nl;
        lines.push_back(key + nl);
    } else {
        lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const std::string& l) { return bare(l) == key; }), lines.end());
    }
    std::string out;
    for (const std::string& l : lines) out += l;
    if (made.has("*") && out.empty()) {
        if (!RemoveFile(path)) return false;
        made.drop("*");
        made.dropDirs();
        return made.save();
    }
    if (out.empty() && nl != kNl) made.add(nl == "\r\n" ? "nl:crlf" : "nl:lf");
    return WriteFile(path, out) && made.save();
}

std::vector<std::wstring> ListedFiles(const std::wstring& dir) {
    std::vector<std::wstring> out;
    for (const DirEntry& e : List(Join(dir, L"games"))) {
        uint64_t tid = 0;
        if (!e.dir || !ParseHex16(ToUtf8(e.name), tid)) continue;
        const State st = Read(dir, tid);
        if (!st.selected.empty()) out.push_back(st.selected);
        for (const std::wstring& p : st.paths) out.push_back(p);
        for (const State::Dlc& d : st.dlc) out.push_back(d.file);
    }
    return out;
}

std::vector<std::wstring> AutoloadDirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    J doc;
    std::string text;
    bool exists = false;
    if (LoadJson(Join(dir, L"Config.json"), doc, text, exists) && exists)
        if (const J* a = doc.get("autoload_dirs"))
            for (const J& d : a->a)
                if (d.t == J::Str && !d.s.empty()) out.push_back(FromNative(FromUtf8(d.s)));
    return out;
}

} // namespace ryu

// ---------------------------------------------------------------- yuzu family

namespace {

// qt-config.ini as lines; written back with its own line endings.
struct Ini {
    std::vector<std::string> lines;
    std::string nl = "\n";
    bool lastNl = true;

    bool load(const std::wstring& path) {
        std::string text;
        if (!ReadFile(path, text)) return false;
        nl = NlOf(text);
        lines.clear();
        size_t at = 0;
        while (at < text.size()) {
            size_t e = text.find('\n', at);
            if (e == std::string::npos) {
                lines.push_back(text.substr(at));
                lastNl = false;
                break;
            }
            std::string l = text.substr(at, e - at);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            at = e + 1;
        }
        return true;
    }
    std::string text() const {
        std::string out;
        for (size_t i = 0; i < lines.size(); i++) {
            out += lines[i];
            if (i + 1 < lines.size() || lastNl) out += nl;
        }
        return out;
    }
    // [begin, end): the lines after the section's header up to the next one.
    bool section(const std::string& name, size_t& begin, size_t& end) const {
        const std::string head = "[" + name + "]";
        for (size_t i = 0; i < lines.size(); i++)
            if (lines[i] == head) {
                begin = i + 1;
                end = begin;
                while (end < lines.size() && !(lines[end].size() > 1 && lines[end][0] == '[')) end++;
                return true;
            }
        return false;
    }
    static std::string keyOf(const std::string& l) {
        const size_t eq = l.find('=');
        return eq == std::string::npos ? std::string() : l.substr(0, eq);
    }
    static std::string valueOf(const std::string& l) {
        const size_t eq = l.find('=');
        return eq == std::string::npos ? std::string() : l.substr(eq + 1);
    }
    long find(size_t b, size_t e, const std::string& key) const {
        for (size_t i = b; i < e; i++)
            if (keyOf(lines[i]) == key) return static_cast<long>(i);
        return -1;
    }
    std::string get(size_t b, size_t e, const std::string& key, bool& has) const {
        const long i = find(b, e, key);
        has = i >= 0;
        return has ? valueOf(lines[static_cast<size_t>(i)]) : std::string();
    }
    // The last line of the section that is not blank, plus one.
    size_t tail(size_t b, size_t e) const {
        size_t t = e;
        while (t > b && lines[t - 1].empty()) t--;
        return t;
    }
};

// How the family writes a text value (Config::AdjustOutputString): forward
// slashes, a doubled one made single, and the whole value in quotes when it
// holds one of the characters QSettings gave a meaning to.
std::string Adjust(std::string v) {
    std::replace(v.begin(), v.end(), '\\', '/');
    const bool unc = v.compare(0, 2, "//") == 0;
    for (size_t p; (p = v.find("//")) != std::string::npos;) v.erase(p, 1);
    if (unc) v.insert(0, "/");
    static const char special[] = "!#$%^&*|;'\",<>?`~=";
    if (v.find_first_of(special) != std::string::npos) v = "\"" + v + "\"";
    return v;
}

// And how it reads one back (ReadStringSetting): every quote taken out, a
// doubled slash made single.
std::string Unadjust(std::string v) {
    v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
    for (size_t p; (p = v.find("//")) != std::string::npos;) v.erase(p, 1);
    return v;
}

// One game's entry in [DisabledAddOns].
struct AddOnEntry {
    int index = 0;  // 1-based
    int count = 0;  // its disabled\size
    std::vector<std::string> names; // by slot, 1-based in the file
};

const char* const kAddOns = "DisabledAddOns";

int ToInt(const std::string& s) {
    return atoi(s.c_str());
}

// The entries as the emulator reads them: the "\default" line must say false
// for a value to count (Config::ReadUnsignedIntegerSetting / ReadStringSetting).
bool FindEntry(const Ini& ini, size_t b, size_t e, uint64_t titleId, AddOnEntry& out) {
    bool has = false;
    const int size = ToInt(ini.get(b, e, "size", has));
    bool found = false;
    for (int i = 1; i <= size; i++) {
        const std::string p = std::to_string(i) + "\\";
        if (ini.get(b, e, p + "title_id\\default", has) != "false" || !has) continue;
        if (strtoull(ini.get(b, e, p + "title_id", has).c_str(), nullptr, 10) != titleId) continue;
        AddOnEntry a;
        a.index = i;
        a.count = ToInt(ini.get(b, e, p + "disabled\\size", has));
        for (int k = 1; k <= a.count; k++) {
            const std::string q = p + "disabled\\" + std::to_string(k) + "\\d";
            const bool given = ini.get(b, e, q + "\\default", has) == "false" && has;
            a.names.push_back(given ? Unadjust(ini.get(b, e, q, has)) : std::string());
        }
        out = a;
        found = true; // a later entry for the same game replaces an earlier one
    }
    return found;
}

// Renames the keys starting with `from` to start with `to` inside [b, e).
void Renumber(Ini& ini, size_t b, size_t e, const std::string& from, const std::string& to) {
    for (size_t i = b; i < e; i++)
        if (ini.lines[i].compare(0, from.size(), from) == 0) ini.lines[i] = to + ini.lines[i].substr(from.size());
}

void SetLine(Ini& ini, size_t b, size_t& e, const std::string& key, const std::string& value, size_t insertAt) {
    const long at = ini.find(b, e, key);
    if (at >= 0) {
        ini.lines[static_cast<size_t>(at)] = key + "=" + value;
        return;
    }
    ini.lines.insert(ini.lines.begin() + static_cast<long>(insertAt), key + "=" + value);
    e++;
}

void EraseKeyPrefix(Ini& ini, size_t b, size_t& e, const std::string& prefix) {
    for (size_t i = b; i < e;) {
        if (ini.lines[i].compare(0, prefix.size(), prefix) == 0) {
            ini.lines.erase(ini.lines.begin() + static_cast<long>(i));
            e--;
        } else {
            i++;
        }
    }
}

bool StoreIni(const std::wstring& path, const Ini& ini, const std::string& oldText) {
    const std::string out = ini.text();
    if (out == oldText) return true;
    return WriteFile(path, out);
}

// The lines of external_content_dirs live in [UI] under "Paths\".
const char* const kExt = "Paths\\external_content_dirs\\";

std::string NormDir(std::string v) {
    v = Unadjust(v);
    std::replace(v.begin(), v.end(), '\\', '/');
    while (v.size() > 1 && v.back() == '/') v.pop_back();
#ifdef _WIN32
    v = LowerA(v);
#endif
    return v;
}

} // namespace

namespace yuzu {

std::string AddOnName(const std::wstring& folderName) {
    return ToUtf8(folderName);
}

State Read(const std::wstring& iniPath, uint64_t titleId) {
    State st;
    Ini ini;
    if (!ini.load(iniPath)) return st;
    st.read = true;
    size_t b, e;
    if (ini.section(kAddOns, b, e)) {
        AddOnEntry a;
        if (FindEntry(ini, b, e, titleId, a))
            for (const std::string& n : a.names)
                if (!n.empty()) st.disabled.push_back(n);
    }
    if (ini.section("UI", b, e)) {
        bool has = false;
        const int n = ToInt(ini.get(b, e, std::string(kExt) + "size", has));
        for (int k = 1; k <= n; k++) {
            const std::string v = ini.get(b, e, std::string(kExt) + std::to_string(k) + "\\path", has);
            if (has && !v.empty()) st.extDirs.push_back(Backslashed(FromUtf8(Unadjust(v))));
        }
    }
    return st;
}

bool SetDisabled(const std::wstring& iniPath, uint64_t titleId, const std::string& name, bool disabled) {
    Ini ini;
    if (!ini.load(iniPath)) return false;
    const std::string oldText = ini.text();
    Made made(iniPath);
    const std::string tid = std::to_string(titleId);
    const std::string addTag = "addon:" + tid + ":" + name, entryTag = "entry:" + tid;
    size_t b, e;
    if (!ini.section(kAddOns, b, e)) {
        if (!disabled) return true;
        // A file NSW-Home seeded has no such section yet.
        if (!ini.lines.empty() && !ini.lines.back().empty()) {
            ini.lines.push_back("");
            made.add("section-gap");
        }
        ini.lines.push_back(std::string("[") + kAddOns + "]");
        ini.lines.push_back("size=0");
        made.add("section");
        ini.section(kAddOns, b, e);
    }
    AddOnEntry a;
    const bool have = FindEntry(ini, b, e, titleId, a);
    const auto slot = std::find(a.names.begin(), a.names.end(), name);
    const bool listed = have && slot != a.names.end();
    if (listed == disabled) return made.save();
    bool has = false;
    int size = ToInt(ini.get(b, e, "size", has));
    if (disabled) {
        if (!have) {
            // A new entry at the end, written as the emulator's EndArray leaves it.
            a.index = size + 1;
            const std::string p = std::to_string(a.index) + "\\";
            const size_t at = ini.tail(b, e);
            const std::vector<std::string> add = {p + "title_id\\default=false", p + "title_id=" + tid,
                                                  p + "disabled\\1\\d\\default=false", p + "disabled\\1\\d=" + Adjust(name),
                                                  p + "disabled\\size=1"};
            ini.lines.insert(ini.lines.begin() + static_cast<long>(at), add.begin(), add.end());
            e += add.size();
            SetLine(ini, b, e, "size", std::to_string(a.index), b);
            made.add(entryTag);
            made.add(addTag);
        } else {
            // Back into the slot NSW-Home took it out of, or after the others.
            const std::string p = std::to_string(a.index) + "\\disabled\\";
            int k = a.count + 1;
            std::string removedTag;
            for (const std::string& m : made.items)
                if (m.compare(0, ("removed:" + tid + ":").size(), "removed:" + tid + ":") == 0) {
                    const std::string rest = m.substr(("removed:" + tid + ":").size());
                    const size_t colon = rest.find(':');
                    if (colon != std::string::npos && rest.substr(colon + 1) == name) {
                        k = (std::min)(ToInt(rest.substr(0, colon)), a.count + 1);
                        removedTag = m;
                    }
                }
            for (int K = a.count; K >= k; K--)
                Renumber(ini, b, e, p + std::to_string(K) + "\\", p + std::to_string(K + 1) + "\\");
            size_t at;
            const long next = k <= a.count ? [&] {
                for (size_t i = b; i < e; i++)
                    if (ini.lines[i].compare(0, (p + std::to_string(k + 1) + "\\").size(), p + std::to_string(k + 1) + "\\") == 0)
                        return static_cast<long>(i);
                return -1L;
            }()
                                           : -1L;
            const long sizeLine = ini.find(b, e, p + "size");
            if (next >= 0) {
                at = static_cast<size_t>(next);
            } else if (sizeLine >= 0) {
                at = static_cast<size_t>(sizeLine);
            } else {
                at = b;
                const std::string ep = std::to_string(a.index) + "\\";
                for (size_t i = b; i < e; i++)
                    if (ini.lines[i].compare(0, ep.size(), ep) == 0) at = i + 1;
            }
            const std::string q = p + std::to_string(k) + "\\d";
            const std::vector<std::string> add = {q + "\\default=false", q + "=" + Adjust(name)};
            ini.lines.insert(ini.lines.begin() + static_cast<long>(at), add.begin(), add.end());
            e += add.size();
            SetLine(ini, b, e, p + "size", std::to_string(a.count + 1), at + add.size());
            if (removedTag.empty()) made.add(addTag);
            else made.drop(removedTag);
        }
    } else {
        const int k = static_cast<int>(slot - a.names.begin()) + 1;
        const std::string p = std::to_string(a.index) + "\\disabled\\";
        EraseKeyPrefix(ini, b, e, p + std::to_string(k) + "\\");
        for (int K = k + 1; K <= a.count; K++)
            Renumber(ini, b, e, p + std::to_string(K) + "\\", p + std::to_string(K - 1) + "\\");
        SetLine(ini, b, e, p + "size", std::to_string(a.count - 1), ini.tail(b, e));
        if (made.has(addTag)) made.drop(addTag);
        else made.add("removed:" + tid + ":" + std::to_string(k) + ":" + name);
        if (a.count - 1 == 0 && made.has(entryTag)) {
            // The entry NSW-Home made goes, and the ones after it move up.
            EraseKeyPrefix(ini, b, e, std::to_string(a.index) + "\\");
            for (int j = a.index + 1; j <= size; j++)
                Renumber(ini, b, e, std::to_string(j) + "\\", std::to_string(j - 1) + "\\");
            SetLine(ini, b, e, "size", std::to_string(size - 1), b);
            made.drop(entryTag);
            size--;
        }
        if (size == 0 && made.has("section")) {
            ini.lines.erase(ini.lines.begin() + static_cast<long>(b - 1), ini.lines.begin() + static_cast<long>(e));
            made.drop("section");
            if (made.has("section-gap") && !ini.lines.empty() && ini.lines.back().empty()) ini.lines.pop_back();
            made.drop("section-gap");
        }
    }
    return StoreIni(iniPath, ini, oldText) && made.save();
}

bool SetExtDir(const std::wstring& iniPath, const std::wstring& folder, bool present) {
    Ini ini;
    if (!ini.load(iniPath)) return false;
    const std::string oldText = ini.text();
    Made made(iniPath);
    const std::string value = Adjust(ToUtf8(Native(folder))), want = NormDir(value), tag = "ext:" + value;
    size_t b, e;
    if (!ini.section("UI", b, e)) {
        if (!present) return true;
        if (!ini.lines.empty() && !ini.lines.back().empty()) {
            ini.lines.push_back("");
            made.add("ui-gap");
        }
        ini.lines.push_back("[UI]");
        made.add("ui");
        ini.section("UI", b, e);
    }
    bool has = false;
    const std::string sizeKey = std::string(kExt) + "size";
    const int n = ToInt(ini.get(b, e, sizeKey, has));
    const bool hadSize = has;
    int at = 0;
    for (int k = 1; k <= n && !at; k++)
        if (NormDir(ini.get(b, e, std::string(kExt) + std::to_string(k) + "\\path", has)) == want && has) at = k;
    if (present) {
        if (at) return made.save();
        // After the list's own lines; a list never written goes after the
        // folders of games, where the emulator keeps its other paths.
        size_t pos = b;
        for (size_t i = b; i < e; i++)
            if (ini.lines[i].compare(0, strlen(kExt), kExt) == 0) pos = i + 1;
        if (!hadSize) {
            const long g = ini.find(b, e, "Paths\\gamedirs\\size");
            pos = g >= 0 ? static_cast<size_t>(g) + 1 : ini.tail(b, e);
            ini.lines.insert(ini.lines.begin() + static_cast<long>(pos), sizeKey + "=0");
            e++;
            pos++;
            made.add("ext-size");
        }
        ini.lines.insert(ini.lines.begin() + static_cast<long>(pos), std::string(kExt) + std::to_string(n + 1) + "\\path=" + value);
        e++;
        SetLine(ini, b, e, sizeKey, std::to_string(n + 1), pos);
        made.add(tag);
    } else {
        if (!at || !made.has(tag)) return made.save(); // only what NSW-Home added is taken away
        EraseKeyPrefix(ini, b, e, std::string(kExt) + std::to_string(at) + "\\");
        for (int k = at + 1; k <= n; k++)
            Renumber(ini, b, e, std::string(kExt) + std::to_string(k) + "\\", std::string(kExt) + std::to_string(k - 1) + "\\");
        SetLine(ini, b, e, sizeKey, std::to_string(n - 1), b);
        made.drop(tag);
        if (n - 1 == 0 && made.has("ext-size")) {
            EraseKeyPrefix(ini, b, e, sizeKey);
            made.drop("ext-size");
        }
        if (made.has("ui") && ini.tail(b, e) == b) {
            ini.lines.erase(ini.lines.begin() + static_cast<long>(b - 1), ini.lines.begin() + static_cast<long>(e));
            made.drop("ui");
            if (made.has("ui-gap") && !ini.lines.empty() && ini.lines.back().empty()) ini.lines.pop_back();
            made.drop("ui-gap");
        }
    }
    return StoreIni(iniPath, ini, oldText) && made.save();
}

std::vector<std::wstring> ExtDirsMade(const std::wstring& iniPath) {
    std::vector<std::wstring> out;
    Made made(iniPath);
    for (const std::string& m : made.items)
        if (m.compare(0, 4, "ext:") == 0) out.push_back(FromUtf8(Unadjust(m.substr(4))));
    return out;
}

std::wstring ModsDir(const std::wstring& dir, uint64_t titleId) {
    const std::wstring load = Join(dir, L"load");
    const std::wstring id = FromUtf8(UpperA(Hex16(titleId)));
    const std::wstring t = FindI(load, id, true);
    return t.empty() ? Join(load, id) : t;
}

std::vector<Mod> Mods(const std::wstring& dir, uint64_t titleId, const std::vector<std::string>& disabled) {
    std::vector<Mod> out;
    const std::wstring root = ModsDir(dir, titleId);
    std::vector<DirEntry> subs = List(root);
    // The emulator goes through them by name.
    std::sort(subs.begin(), subs.end(), [](const DirEntry& a, const DirEntry& b) { return ToUtf8(a.name) < ToUtf8(b.name); });
    for (const DirEntry& s : subs) {
        if (!s.dir) continue;
        Mod m;
        m.name = s.name;
        m.path = Join(root, s.name);
        bool other = false, cheats = false;
        for (const DirEntry& k : List(m.path)) {
            if (!k.dir) continue;
            const std::wstring n = LowerW(k.name);
            const bool known = n == L"exefs" || n == L"romfs" || n == L"romfslite" || n == L"romfs_ext" || n == L"cheats";
            // An empty folder changes nothing, and the emulator does not list it.
            if (!known || List(Join(m.path, k.name)).empty()) continue;
            m.kinds += (m.kinds.empty() ? L"" : L", ") + n;
            if (n == L"cheats") cheats = true;
            else other = true;
        }
        if (m.kinds.empty()) continue;
        m.cheatsOnly = cheats && !other;
        m.on = std::find(disabled.begin(), disabled.end(), AddOnName(s.name)) == disabled.end();
        out.push_back(m);
    }
    return out;
}

std::vector<Cheat> Cheats(const std::wstring& dir, uint64_t titleId, const std::vector<std::string>& disabled, bool looseFiles) {
    std::vector<Cheat> out;
    // The family's text parser: "{name}" is the master code, "[name]" a cheat.
    auto names = [](const std::wstring& file) {
        std::vector<std::wstring> v;
        std::string text;
        if (!ReadFile(file, text)) return v;
        for (size_t i = 0; i < text.size(); i++) {
            const char open = text[i];
            if (open != '[' && open != '{') continue;
            const size_t end = text.find(open == '[' ? ']' : '}', i + 1);
            if (end == std::string::npos) break;
            v.push_back(FromUtf8(text.substr(i + 1, end - i - 1)));
            i = end;
        }
        return v;
    };
    const std::wstring root = ModsDir(dir, titleId);
    for (const Mod& m : Mods(dir, titleId, disabled)) {
        const std::wstring cd = FindI(m.path, L"cheats", true);
        if (cd.empty()) continue;
        for (const DirEntry& e : List(cd)) {
            if (e.dir || !EndsWithI(e.name, L".txt") || !Hex16Name(e.name.substr(0, e.name.size() - 4))) continue;
            for (const std::wstring& n : names(Join(cd, e.name))) {
                Cheat c;
                c.name = n;
                c.file = Join(cd, e.name);
                c.key = AddOnName(m.name);
                c.on = m.on;
                out.push_back(c);
            }
        }
    }
    if (looseFiles)
        for (const DirEntry& e : List(root)) {
            if (e.dir || e.name.compare(0, 6, L"cheat_") != 0) continue;
            for (const std::wstring& n : names(Join(root, e.name))) {
                Cheat c;
                c.name = n;
                c.file = Join(root, e.name);
                c.key = ToUtf8(e.name);
                c.on = std::find(disabled.begin(), disabled.end(), c.key) == disabled.end();
                out.push_back(c);
            }
        }
    return out;
}

} // namespace yuzu

} // namespace content
