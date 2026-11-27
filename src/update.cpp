// See update.h.
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#endif
#include "update.h"
#include "fr_crypto.h"
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

namespace update {

// The repository's name is TO BE CONFIRMED when the program is published: the
// account is the owner's, the name a placeholder. Until it exists GitHub
// answers 404, which the menu reports as "no version published yet".
const char* const kRepository = "daniel-madrid-07/NSW-Home";

// The name build.bat gives the program. A release is installable from the
// menu when it carries a file of exactly this name.
const char* const kAssetName = "NSW-Home.exe";

namespace {

// ---- the answer's text ----------------------------------------------------------

// Just enough JSON to read GitHub's answer: the whole value is parsed, nothing
// is assumed about the order of its fields, and anything malformed is refused.
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool flag = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* Get(const char* key) const {
        for (const auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
    std::string Str(const char* key) const {
        const Json* v = Get(key);
        return v && v->kind == String ? v->text : std::string();
    }
    bool Is(const char* key) const {
        const Json* v = Get(key);
        return v && v->kind == Bool && v->flag;
    }
};

void PutUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s) {}
    bool Read(Json& out) {
        if (!Value(out, 0)) return false;
        Space();
        return at_ == s_.size();
    }

private:
    const std::string& s_;
    size_t at_ = 0;

    void Space() {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) at_++;
    }
    bool Word(const char* w) {
        const size_t n = strlen(w);
        if (s_.compare(at_, n, w) != 0) return false;
        at_ += n;
        return true;
    }
    bool Hex4(uint32_t& v) {
        if (at_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = s_[at_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool Text(std::string& out) {
        if (at_ >= s_.size() || s_[at_] != '"') return false;
        at_++;
        while (at_ < s_.size()) {
            const char c = s_[at_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= s_.size()) return false;
            const char e = s_[at_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t c1 = 0;
                if (!Hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00) {
                    // The first half of a pair: the second must follow.
                    uint32_t c2 = 0;
                    if (s_.compare(at_, 2, "\\u") != 0) return false;
                    at_ += 2;
                    if (!Hex4(c2) || c2 < 0xDC00 || c2 > 0xDFFF) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                } else if (c1 >= 0xDC00 && c1 <= 0xDFFF) {
                    return false;
                }
                PutUtf8(out, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Value(Json& out, int depth) {
        if (depth > 32) return false; // GitHub's answers are four deep
        Space();
        if (at_ >= s_.size()) return false;
        const char c = s_[at_];
        if (c == '{') {
            at_++;
            out.kind = Json::Object;
            Space();
            if (at_ < s_.size() && s_[at_] == '}') {
                at_++;
                return true;
            }
            for (;;) {
                Space();
                std::string key;
                if (!Text(key)) return false;
                Space();
                if (at_ >= s_.size() || s_[at_++] != ':') return false;
                Json v;
                if (!Value(v, depth + 1)) return false;
                out.fields.emplace_back(std::move(key), std::move(v));
                Space();
                if (at_ >= s_.size()) return false;
                const char d = s_[at_++];
                if (d == '}') return true;
                if (d != ',') return false;
            }
        }
        if (c == '[') {
            at_++;
            out.kind = Json::Array;
            Space();
            if (at_ < s_.size() && s_[at_] == ']') {
                at_++;
                return true;
            }
            for (;;) {
                Json v;
                if (!Value(v, depth + 1)) return false;
                out.items.push_back(std::move(v));
                Space();
                if (at_ >= s_.size()) return false;
                const char d = s_[at_++];
                if (d == ']') return true;
                if (d != ',') return false;
            }
        }
        if (c == '"') {
            out.kind = Json::String;
            return Text(out.text);
        }
        if (Word("true")) {
            out.kind = Json::Bool;
            out.flag = true;
            return true;
        }
        if (Word("false")) {
            out.kind = Json::Bool;
            return true;
        }
        if (Word("null")) return true;
        const size_t from = at_;
        while (at_ < s_.size() && (strchr("+-.eE", s_[at_]) || (s_[at_] >= '0' && s_[at_] <= '9'))) at_++;
        if (at_ == from) return false;
        out.kind = Json::Number;
        out.number = strtod(s_.substr(from, at_ - from).c_str(), nullptr);
        return true;
    }
};

// One character of UTF-8 at `at`, which moves past it. Anything malformed
// comes back as U+FFFD, one byte at a time.
uint32_t NextChar(const std::string& s, size_t& at) {
    const unsigned char c = static_cast<unsigned char>(s[at++]);
    if (c < 0x80) return c;
    const int more = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (more < 0 || c >= 0xF8 || at + more > s.size()) return 0xFFFD;
    uint32_t v = c & (0x3F >> more);
    for (int i = 0; i < more; i++) {
        const unsigned char n = static_cast<unsigned char>(s[at + i]);
        if ((n & 0xC0) != 0x80) return 0xFFFD;
        v = (v << 6) | (n & 0x3F);
    }
    at += more;
    return v;
}

// A character that may be shown in the menu's dialog. Out: control and
// formatting characters (a description could use them to reorder or hide
// text), the private-use block (where the menu's own button symbols live),
// and everything beyond the first plane (the menu's font has none of it).
bool Showable(uint32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return false;
    if (c >= 0x200B && c <= 0x200F) return false;
    if (c >= 0x202A && c <= 0x202E) return false;
    if (c >= 0x2060 && c <= 0x206F) return false;
    if (c >= 0xD800 && c <= 0xF8FF) return false;
    if (c >= 0xFFF0) return false;
    return true;
}

// The first lines of a release's description (GitHub's Markdown) as plain
// text: marks and addresses dropped, three lines of at most 64 characters.
std::string PlainNotes(const std::string& body) {
    const int kLines = 3, kChars = 64;
    std::string out;
    int lines = 0;
    size_t at = 0;
    while (at < body.size() && lines < kLines) {
        size_t end = body.find('\n', at);
        if (end == std::string::npos) end = body.size();
        std::string line = body.substr(at, end - at);
        at = end + 1;

        // Tags and what a link points to go; a link's own words stay.
        std::string kept;
        for (size_t i = 0; i < line.size(); i++) {
            const char c = line[i];
            if (c == '<') {
                const size_t close = line.find('>', i);
                if (close != std::string::npos) {
                    i = close;
                    continue;
                }
            }
            if (c == ']' && i + 1 < line.size() && line[i + 1] == '(') {
                const size_t close = line.find(')', i);
                if (close != std::string::npos) {
                    i = close;
                    continue;
                }
            }
            if (c == '[' || c == ']' || c == '`' || c == '*' || c == '\r' || (c == '!' && i + 1 < line.size() && line[i + 1] == '['))
                continue;
            kept += c;
        }
        // What starts the line: a heading's or a quote's mark goes, a list's becomes a dot.
        size_t from = 0;
        bool item = false;
        while (from < kept.size() && strchr(" \t#>-+=_", kept[from])) {
            item = item || kept[from] == '-' || kept[from] == '+';
            from++;
        }
        std::string text;
        int count = 0;
        bool space = false, cut = false;
        for (size_t i = from; i < kept.size();) {
            uint32_t c = NextChar(kept, i);
            if (c == '\t') c = ' ';
            if (!Showable(c)) continue;
            if (c == ' ') {
                space = !text.empty();
                continue;
            }
            if (count >= kChars) {
                cut = true;
                break;
            }
            if (space) {
                text += ' ';
                count++;
            }
            space = false;
            PutUtf8(text, c);
            count++;
        }
        if (text.empty()) continue;
        if (cut) PutUtf8(text, 0x2026);
        if (!out.empty()) out += '\n';
        if (item) {
            PutUtf8(out, 0xB7);
            out += ' ';
        }
        out += text;
        lines++;
    }
    return out;
}

bool SameNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
        if (x != y) return false;
    }
    return true;
}

std::string Hex(const std::string& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

// The path of a file of this repository's releases on github.com, from the
// address GitHub's API gave for it. Anything else is refused: the address
// comes over the network and decides where a program is fetched from.
bool AssetPath(const std::string& url, const std::string& repository, std::string& path) {
    const std::string site = "https://github.com/";
    if (url.compare(0, site.size(), site) != 0) return false;
    const std::string rest = url.substr(site.size());
    const std::string under = repository + "/releases/download/";
    if (rest.size() <= under.size() || !SameNoCase(rest.substr(0, under.size()), under)) return false;
    if (rest.find("..") != std::string::npos) return false;
    for (char c : rest) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("/._-~%+", c);
        if (!ok || c == '\0') return false;
    }
    path = "/" + rest;
    return true;
}

} // namespace

// ---- versions ---------------------------------------------------------------------

bool ParseVersion(const std::string& tag, std::vector<int>& out) {
    out.clear();
    size_t at = !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? 1 : 0;
    if (at >= tag.size()) return false;
    for (;;) {
        int n = 0, digits = 0;
        while (at < tag.size() && tag[at] >= '0' && tag[at] <= '9') {
            if (++digits > 6) return false; // no version has a number this long, and it must fit
            n = n * 10 + (tag[at++] - '0');
        }
        if (!digits) return false;
        out.push_back(n);
        if (at == tag.size()) return out.size() <= 4;
        if (tag[at++] != '.' || out.size() >= 4) return false;
    }
}

int CompareVersions(const std::vector<int>& a, const std::vector<int>& b) {
    for (size_t i = 0; i < a.size() || i < b.size(); i++) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// ---- GitHub's answer ----------------------------------------------------------------

Check Interpret(int status, const std::string& body, const std::string& current, const std::string& repository,
                const char* assetName) {
    Check c;
    c.status = status;
    if (status == 0) {
        c.answer = Answer::Offline;
        return c;
    }
    if (status == 404) {
        c.answer = Answer::None;
        return c;
    }
    if (status == 403 || status == 429) {
        c.answer = Answer::Refused;
        return c;
    }
    c.answer = Answer::Odd;
    if (status != 200) return c;
    Json j;
    if (!JsonReader(body).Read(j) || j.kind != Json::Object) return c;
    const std::string tag = j.Str("tag_name");
    if (tag.empty()) return c;

    // From here on it is a release. One that is not a plain numbered version,
    // or that its author marked as a draft or a pre-release, is never offered.
    c.answer = Answer::Latest;
    std::vector<int> theirs, ours;
    if (j.Is("draft") || j.Is("prerelease") || !ParseVersion(tag, theirs)) return c;
    if (!ParseVersion(current, ours) || CompareVersions(theirs, ours) <= 0) return c;

    c.answer = Answer::Newer;
    c.release.tag = tag;
    c.release.version = tag.substr(tag[0] == 'v' || tag[0] == 'V' ? 1 : 0);
    c.release.notes = PlainNotes(j.Str("body"));
    const Json* assets = j.Get("assets");
    if (assets)
        for (const Json& a : assets->items)
            if (a.kind == Json::Object && !a.Str("name").empty()) c.release.files.push_back(a.Str("name"));
    if (assets && assetName && *assetName)
        for (const Json& a : assets->items) {
            if (a.kind != Json::Object || !SameNoCase(a.Str("name"), assetName) || a.Str("state") != "uploaded") continue;
            const Json* size = a.Get("size");
            std::string path;
            Asset& out = c.release.asset;
            out.name = a.Str("name");
            out.url = a.Str("browser_download_url");
            if (!size || size->kind != Json::Number || size->number < 1 || size->number > static_cast<double>(kMaxAsset)) continue;
            if (!AssetPath(out.url, repository, path)) continue;
            out.size = static_cast<uint64_t>(size->number);
            // "sha256:" and 64 digits. Releases older than GitHub's digests have null here.
            const Json* digest = a.Get("digest");
            if (digest && digest->kind == Json::String) {
                const std::string d = digest->text;
                bool hex = d.size() == 71 && d.compare(0, 7, "sha256:") == 0;
                for (size_t i = 7; hex && i < d.size(); i++)
                    hex = (d[i] >= '0' && d[i] <= '9') || (d[i] >= 'a' && d[i] <= 'f') || (d[i] >= 'A' && d[i] <= 'F');
                if (hex) {
                    for (size_t i = 7; i < d.size(); i++)
                        out.sha256 += d[i] >= 'A' && d[i] <= 'F' ? static_cast<char>(d[i] + 32) : d[i];
                } else {
                    out.digestOdd = true;
                }
            } else if (digest && digest->kind != Json::Null) {
                out.digestOdd = true;
            }
            c.release.hasAsset = true;
            break;
        }
    if (!c.release.hasAsset) c.release.asset = Asset();
    return c;
}

// ---- the downloaded file ---------------------------------------------------------------

bool IsProgram(const std::string& bytes) {
    auto u16 = [&](size_t at) {
        return static_cast<uint32_t>(static_cast<unsigned char>(bytes[at])) |
               static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8;
    };
    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') return false;
    const size_t pe = u16(0x3C) | static_cast<size_t>(u16(0x3E)) << 16;
    // Signature (4), file header (20) and the first field of the optional header (2).
    if (pe > bytes.size() || bytes.size() - pe < 26) return false;
    if (bytes.compare(pe, 4, std::string("PE\0\0", 4)) != 0) return false;
#if defined(_M_ARM64) || defined(__aarch64__)
    const uint32_t machine = 0xAA64;
#else
    const uint32_t machine = 0x8664;
#endif
    const uint32_t flags = u16(pe + 22);
    // An executable image (0x0002) that is not a library (0x2000), in the 64-bit layout (0x20B).
    return u16(pe + 4) == machine && (flags & 0x0002) && !(flags & 0x2000) && u16(pe + 24) == 0x20B;
}

std::string VerifyBytes(const std::string& bytes, const Asset& asset) {
    if (asset.size == 0 || bytes.size() != asset.size) return "SIZE";
    if (asset.digestOdd) return "DIGEST";
    if (!asset.sha256.empty() && Hex(frc::Sha256(bytes)) != asset.sha256) return "DIGEST";
    if (!IsProgram(bytes)) return "NOT_PROGRAM";
    return "";
}

std::wstring DownloadPath(const std::wstring& dataDir) {
    const std::string name = kAssetName;
    return dataDir + L"\\update\\" + std::wstring(name.begin(), name.end()) + L".download";
}

std::string PageUrl(const std::string& tag) {
    std::string url = std::string("https://github.com/") + kRepository + "/releases";
    bool plain = !tag.empty() && tag.size() <= 40;
    for (char c : tag) plain = plain && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_');
    if (plain && tag.find("..") == std::string::npos) url += "/tag/" + tag;
    return url;
}

namespace {
std::atomic<bool> offscreen{false};
}

void SetOffscreen(bool on) {
    offscreen = on;
}

#ifdef _WIN32 // ---- Windows -------------------------------------------------------------

namespace {

std::wstring Wide(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

// One HTTPS GET. Returns the HTTP status, 0 when there was no answer (or it
// was cut short), -1 when `cancel` or the sink stopped it. The certificate is
// checked by the system (nothing here relaxes that), no cookies and no
// credentials are sent, and a redirect may never leave HTTPS.
int HttpsGet(const std::wstring& host, const std::wstring& path, const wchar_t* accept, const std::string& version,
             const std::function<bool(const char*, size_t)>& sink, const std::atomic<bool>* cancel) {
    auto stop = [&] { return cancel && cancel->load(); };
    int result = 0;
    const std::wstring agent = L"NSW-Home/" + Wide(version);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 15000);
    HINTERNET conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    const wchar_t* types[] = {accept, nullptr};
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, types, WINHTTP_FLAG_SECURE)
                         : nullptr;
    if (req && !stop()) {
        DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, size = sizeof status;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &size, WINHTTP_NO_HEADER_INDEX)) {
                result = static_cast<int>(status);
                std::vector<char> buf(64 * 1024);
                for (;;) {
                    if (stop()) {
                        result = -1;
                        break;
                    }
                    DWORD got = 0;
                    if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
                        result = 0;
                        break;
                    }
                    if (got == 0) break;
                    if (!sink(buf.data(), got)) {
                        result = -1;
                        break;
                    }
                }
            }
        }
    } else if (req) {
        result = -1;
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

bool Exists(const std::wstring& file) {
    return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ReadWhole(const std::wstring& file, std::string& out) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && static_cast<uint64_t>(size.QuadPart) <= kMaxAsset;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t at = 0;
        while (ok && at < out.size()) {
            DWORD got = 0;
            ok = ReadFile(h, &out[at], static_cast<DWORD>((std::min)(out.size() - at, static_cast<size_t>(1) << 20)), &got, nullptr) &&
                 got > 0;
            at += got;
        }
    }
    CloseHandle(h);
    return ok;
}

bool WriteWhole(const std::wstring& file, const std::string& bytes) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t at = 0;
    while (ok && at < bytes.size()) {
        DWORD put = 0;
        ok = WriteFile(h, bytes.data() + at, static_cast<DWORD>((std::min)(bytes.size() - at, static_cast<size_t>(1) << 20)), &put,
                       nullptr) &&
             put > 0;
        at += put;
    }
    // On the disk before the program it replaces is moved away.
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

std::wstring NewPath(const std::wstring& program) {
    return program + L".new";
}

} // namespace

bool Available() {
    return true;
}
bool CanInstall() {
    return true;
}

Check Ask(const std::string& repository, const std::string& current, const char* assetName, const std::atomic<bool>* cancel) {
    if (offscreen) return Check();
    std::string body;
    // (A release's answer is a few kilobytes; one that goes on past 2 MB is not one.)
    const int status = HttpsGet(L"api.github.com", L"/repos/" + Wide(repository) + L"/releases/latest", L"application/vnd.github+json",
                                current,
                                [&](const char* data, size_t n) {
                                    body.append(data, n);
                                    return body.size() <= (2u << 20);
                                },
                                cancel);
    if (cancel && cancel->load()) return Check();
    if (status < 0) { // more than any release's answer: not one
        Check odd;
        odd.answer = Answer::Odd;
        return odd;
    }
    return Interpret(status, body, current, repository, assetName);
}

std::string Download(const Asset& asset, const std::string& repository, const std::wstring& file, const std::string& current,
                     std::atomic<uint64_t>* got, const std::atomic<bool>* cancel) {
    if (offscreen) return "UNAVAILABLE";
    std::string path;
    if (asset.size == 0 || asset.size > kMaxAsset || !AssetPath(asset.url, repository, path)) return "REFUSED";
    const size_t slash = file.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(file.substr(0, slash).c_str(), nullptr);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "IO";
    uint64_t total = 0;
    bool wrote = true;
    if (got) *got = 0;
    const int status = HttpsGet(L"github.com", Wide(path), L"application/octet-stream", current,
                                [&](const char* data, size_t n) {
                                    // Not a byte more than GitHub said the file has.
                                    if (total + n > asset.size) return false;
                                    DWORD put = 0;
                                    wrote = WriteFile(h, data, static_cast<DWORD>(n), &put, nullptr) && put == n;
                                    total += n;
                                    if (got) *got = total;
                                    return wrote;
                                },
                                cancel);
    CloseHandle(h);
    std::string error;
    if (cancel && cancel->load()) error = "CANCELLED";
    else if (!wrote) error = "IO";
    else if (status != 200 || total != asset.size) error = "NETWORK";
    if (!error.empty()) DeleteFileW(file.c_str());
    return error;
}

std::wstring ProgramPath() {
    std::wstring path(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring AsidePath(const std::wstring& program) {
    const size_t slash = program.find_last_of(L"\\/");
    const size_t dot = program.rfind(L'.');
    const bool ext = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
    return (ext ? program.substr(0, dot) : program) + L".old.exe";
}

bool FolderWritable(const std::wstring& program) {
    if (offscreen || program.empty()) return false;
    // The very name Install writes first, gone again when the handle closes.
    HANDLE h = CreateFileW(NewPath(program).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::string Install(const std::wstring& program, const std::wstring& download, const Asset& asset) {
    if (offscreen || program.empty()) return "UNAVAILABLE";
    std::string bytes;
    if (!ReadWhole(download, bytes)) return "IO";
    // From here the bytes in memory are what gets installed: whatever happens
    // to the download's file afterwards, what was checked is what is written.
    DeleteFileW(download.c_str());
    const std::string bad = VerifyBytes(bytes, asset);
    if (!bad.empty()) return bad;

    const std::wstring fresh = NewPath(program), aside = AsidePath(program);
    std::string back;
    if (!WriteWhole(fresh, bytes) || !ReadWhole(fresh, back) || back != bytes) {
        DeleteFileW(fresh.c_str());
        return "NOT_WRITABLE";
    }
    // A program set aside by an earlier update that is still there (it could
    // not be removed at start: it may be running) is not written over.
    if (Exists(aside) && !DeleteFileW(aside.c_str())) {
        DeleteFileW(fresh.c_str());
        return "LOCKED";
    }
    // A running program can be renamed, though not written or deleted.
    if (!MoveFileExW(program.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(fresh.c_str());
        return "LOCKED";
    }
    if (!MoveFileExW(fresh.c_str(), program.c_str(), MOVEFILE_WRITE_THROUGH)) {
        // Back as it was. Should the way back be refused too, the program set
        // aside can still be read (a running one too): a copy of it then.
        bool restored = MoveFileExW(aside.c_str(), program.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
        if (!restored) restored = CopyFileW(aside.c_str(), program.c_str(), TRUE) != 0;
        DeleteFileW(fresh.c_str());
        return restored ? "LOCKED" : "ASIDE";
    }
    return "";
}

bool Restart() {
    if (offscreen) return false;
    const std::wstring program = ProgramPath();
    if (program.empty()) return false;
    std::wstring line = L"\"" + program + L"\"";
    // The ways it was started that still matter; nothing else is passed on.
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; i++)
            for (const wchar_t* keep : {L"--windowed", L"--background", L"--log", L"--no-setup"})
                if (!wcscmp(argv[i], keep)) line += std::wstring(L" ") + keep;
        LocalFree(argv);
    }
    line += L" --after-update " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(program.c_str(), &line[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void AtStart(const std::wstring& dataDir) {
    if (offscreen) return;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; i++) {
            if (wcscmp(argv[i], L"--after-update") != 0) continue;
            // The NSW-Home that was updated is still closing: it holds the
            // one-instance mark and its file cannot be deleted until it ends.
            if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(_wtoi(argv[i + 1])))) {
                WaitForSingleObject(old, 30000);
                CloseHandle(old);
            }
        }
        LocalFree(argv);
    }
    const std::wstring program = ProgramPath();
    if (!program.empty()) {
        DeleteFileW(AsidePath(program).c_str());
        DeleteFileW(NewPath(program).c_str());
    }
    if (!dataDir.empty()) DeleteFileW(DownloadPath(dataDir).c_str());
}

bool OpenPage(const std::string& tag) {
    if (offscreen) return false;
    const std::wstring url = Wide(PageUrl(tag));
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

#else // ---- every other system: not available ----------------------------------------------

bool Available() {
    return false;
}
bool CanInstall() {
    return false;
}
Check Ask(const std::string&, const std::string&, const char*, const std::atomic<bool>*) {
    return Check();
}
std::string Download(const Asset&, const std::string&, const std::wstring&, const std::string&, std::atomic<uint64_t>*,
                     const std::atomic<bool>*) {
    return "UNAVAILABLE";
}
std::wstring ProgramPath() {
    return {};
}
std::wstring AsidePath(const std::wstring& program) {
    return program + L".old";
}
bool FolderWritable(const std::wstring&) {
    return false;
}
std::string Install(const std::wstring&, const std::wstring&, const Asset&) {
    return "UNAVAILABLE";
}
bool Restart() {
    return false;
}
void AtStart(const std::wstring&) {}
bool OpenPage(const std::string&) {
    return false;
}

#endif

} // namespace update
