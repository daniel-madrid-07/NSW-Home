// The Windows calls the shared sources make, implemented for Linux and macOS.
// Declarations and the per-call notes are in win/*.h.
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <wincrypt.h>

#include "port.h"

#include <cerrno>
#include <clocale>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#endif

// ---- locale -------------------------------------------------------------------

namespace {

// Character classification (towlower/towupper) and wide stdio need a UTF-8
// character set; number formatting must stay in the "C" locale because the
// config files are written with printf.
bool InitLocale() {
    for (const char* name : {"", "C.UTF-8", "en_US.UTF-8", "UTF-8"}) {
        const char* got = setlocale(LC_CTYPE, name);
        if (got && (strstr(got, "UTF-8") || strstr(got, "utf8"))) return true;
    }
    return false;
}
const bool localeReady = InitLocale();

} // namespace

// ---- UTF-8 <-> wide -----------------------------------------------------------

namespace {

// Decodes one code point; returns the bytes consumed (>= 1). Malformed input
// yields U+FFFD and consumes one byte, like the Windows converter does.
int DecodeUtf8(const unsigned char* s, int left, uint32_t& cp) {
    unsigned char c = s[0];
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    int need = 0;
    uint32_t min = 0;
    if ((c & 0xE0) == 0xC0) need = 1, cp = c & 0x1F, min = 0x80;
    else if ((c & 0xF0) == 0xE0) need = 2, cp = c & 0x0F, min = 0x800;
    else if ((c & 0xF8) == 0xF0) need = 3, cp = c & 0x07, min = 0x10000;
    else {
        cp = 0xFFFD;
        return 1;
    }
    if (need >= left) {
        cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            cp = 0xFFFD;
            return 1;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
        return 1;
    }
    return need + 1;
}

int EncodeUtf8(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

} // namespace

int MultiByteToWideChar(UINT, DWORD, const char* src, int srcLen, wchar_t* dst, int dstLen) {
    if (!src || srcLen == 0) return 0;
    if (srcLen < 0) srcLen = static_cast<int>(strlen(src)) + 1;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(src);
    int out = 0;
    for (int i = 0; i < srcLen;) {
        uint32_t cp;
        i += DecodeUtf8(s + i, srcLen - i, cp);
        if (dstLen > 0) {
            if (out >= dstLen) return 0;
            dst[out] = static_cast<wchar_t>(cp);
        }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t* src, int srcLen, char* dst, int dstLen, const char*, BOOL*) {
    if (!src || srcLen == 0) return 0;
    if (srcLen < 0) srcLen = static_cast<int>(wcslen(src)) + 1;
    int out = 0;
    for (int i = 0; i < srcLen; i++) {
        uint32_t cp = static_cast<uint32_t>(src[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < srcLen) {
            uint32_t lo = static_cast<uint32_t>(src[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        char buf[4];
        int n = EncodeUtf8(cp, buf);
        if (dstLen > 0) {
            if (out + n > dstLen) return 0;
            memcpy(dst + out, buf, static_cast<size_t>(n));
        }
        out += n;
    }
    return out;
}

namespace port {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

const wchar_t* MsWideFormat(const wchar_t* fmt, wchar_t* scratch, size_t scratchLen) {
    size_t o = 0;
    auto put = [&](wchar_t c) {
        if (o + 1 < scratchLen) scratch[o++] = c;
    };
    for (const wchar_t* p = fmt; *p;) {
        if (*p != L'%') {
            put(*p++);
            continue;
        }
        put(*p++);
        if (*p == L'%') {
            put(*p++);
            continue;
        }
        while (*p && wcschr(L"-+ #0", *p)) put(*p++);         // flags
        while (*p && (iswdigit(static_cast<wint_t>(*p)) || *p == L'*')) put(*p++); // width
        if (*p == L'.') {
            put(*p++);
            while (*p && (iswdigit(static_cast<wint_t>(*p)) || *p == L'*')) put(*p++); // precision
        }
        bool hasLength = false;
        while (*p && wcschr(L"hlLqjzt", *p)) {
            hasLength = true;
            put(*p++);
        }
        if (!*p) break;
        wchar_t conv = *p++;
        if (!hasLength && (conv == L's' || conv == L'c')) put(L'l'); // wide argument
        else if (conv == L'S') conv = L's';                          // narrow argument
        else if (conv == L'C') conv = L'c';
        put(conv);
    }
    scratch[o] = 0;
    return scratch;
}

} // namespace port

DWORD CharLowerBuffW(wchar_t* s, DWORD len) {
    for (DWORD i = 0; i < len; i++) s[i] = static_cast<wchar_t>(towlower(static_cast<wint_t>(s[i])));
    return len;
}

int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        wint_t ca = towlower(static_cast<wint_t>(a[i])), cb = towlower(static_cast<wint_t>(b[i]));
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) break;
    }
    return 0;
}

int _wcsicmp(const wchar_t* a, const wchar_t* b) {
    return _wcsnicmp(a, b, static_cast<size_t>(-1));
}

int _wtoi(const wchar_t* s) {
    return static_cast<int>(wcstol(s, nullptr, 10));
}

void* SecureZeroMemory(void* p, size_t n) {
    volatile unsigned char* b = static_cast<volatile unsigned char*>(p);
    while (n--) *b++ = 0;
    return p;
}

// ---- folders and environment --------------------------------------------------

namespace port {

std::string HomeDir() {
    const char* h = getenv("HOME");
    if (h && *h) return h;
    if (passwd* pw = getpwuid(getuid()))
        if (pw->pw_dir) return pw->pw_dir;
    return "/";
}

#ifdef __APPLE__
std::string ConfigHome() { return HomeDir() + "/Library/Application Support"; }
std::string DataHome() { return HomeDir() + "/Library/Application Support"; }
std::string AppsDir() { return "/Applications"; }
#else
std::string ConfigHome() {
    const char* x = getenv("XDG_CONFIG_HOME");
    return x && x[0] == '/' ? std::string(x) : HomeDir() + "/.config";
}
std::string DataHome() {
    const char* x = getenv("XDG_DATA_HOME");
    return x && x[0] == '/' ? std::string(x) : HomeDir() + "/.local/share";
}
std::string AppsDir() { return "/opt"; }
#endif

std::string NativePath(const std::wstring& path) {
    std::string s = Utf8(path);
    for (char& c : s)
        if (c == '\\') c = '/';
    bool drive = s.size() >= 2 && isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' && (s.size() == 2 || s[2] == '/');
    if (drive) {
        // Whether <root>/<letter> exists is looked up once per letter.
        static std::mutex lock;
        static int known[26]; // 0 = not checked, 1 = mapped, 2 = no such folder
        static std::string root;
        std::lock_guard<std::mutex> guard(lock);
        if (root.empty()) {
            const char* r = getenv("NSWHOME_DRIVE_ROOT");
            if (!r || !*r) r = getenv("DOCKSIDE_DRIVE_ROOT"); // historical name, still honoured
            root = r && *r ? r : "/mnt";
        }
        int letter = tolower(static_cast<unsigned char>(s[0])) - 'a';
        std::string mount = root + "/" + static_cast<char>('a' + letter);
        if (!known[letter]) {
            struct stat st;
            known[letter] = stat(mount.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 2;
        }
        if (known[letter] == 1) s = mount + s.substr(2);
    }
    return s;
}

std::vector<std::string> SplitCommandLine(const std::wstring& line) {
    std::vector<std::string> out;
    std::wstring word;
    bool quoted = false, have = false;
    for (wchar_t c : line) {
        if (c == L'"') {
            quoted = !quoted;
            have = true; // "" is an argument, an empty one
        } else if (c == L' ' && !quoted) {
            if (have) out.push_back(Utf8(word));
            word.clear();
            have = false;
        } else {
            word += c;
            have = true;
        }
    }
    if (have) out.push_back(Utf8(word));
    return out;
}

} // namespace port

DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buf, DWORD size) {
    std::string value;
    if (!wcscmp(name, L"APPDATA")) value = port::ConfigHome();
    else if (!wcscmp(name, L"LOCALAPPDATA")) value = port::DataHome();
    else if (!wcscmp(name, L"ProgramFiles")) value = port::AppsDir();
    else if (!wcscmp(name, L"USERPROFILE")) value = port::HomeDir();
    else if (const char* v = getenv(port::Utf8(name).c_str())) value = v;
    std::wstring w = port::Wide(value);
    if (w.empty()) return 0;
    if (w.size() + 1 > size) return static_cast<DWORD>(w.size() + 1); // too small: the size needed
    memcpy(buf, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    return static_cast<DWORD>(w.size());
}

HRESULT SHGetKnownFolderPath(const KNOWNFOLDERID& id, DWORD, HANDLE, wchar_t** path) {
    *path = nullptr;
    if (id.which != FOLDERID_Pictures.which) return E_FAIL;
    std::string dir = port::HomeDir() + "/Pictures";
#ifndef __APPLE__
    // XDG_PICTURES_DIR="$HOME/Imágenes"
    if (FILE* f = fopen((port::ConfigHome() + "/user-dirs.dirs").c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            const char* key = "XDG_PICTURES_DIR=\"";
            if (strncmp(line, key, strlen(key)) != 0) continue;
            std::string v = line + strlen(key);
            size_t quote = v.find('"');
            if (quote == std::string::npos) continue;
            v.resize(quote);
            if (v.compare(0, 5, "$HOME") == 0) v = port::HomeDir() + v.substr(5);
            if (!v.empty() && v[0] == '/') dir = v;
        }
        fclose(f);
    }
#endif
    std::wstring w = port::Wide(dir);
    wchar_t* out = static_cast<wchar_t*>(malloc((w.size() + 1) * sizeof(wchar_t)));
    if (!out) return E_FAIL;
    memcpy(out, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    *path = out;
    return S_OK;
}

void CoTaskMemFree(void* p) {
    free(p);
}

// ---- files --------------------------------------------------------------------

namespace {

thread_local DWORD lastError = 0;

DWORD ErrorFromErrno(int e) {
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT:
    case ENOTDIR: return ERROR_FILE_NOT_FOUND;
    case EEXIST: return ERROR_FILE_EXISTS;
    default: return ERROR_ACCESS_DENIED;
    }
}

} // namespace

DWORD GetLastError() {
    return lastError;
}
void SetLastError(DWORD code) {
    lastError = code;
}

namespace {

// 100 ns ticks since 1601, which is what a FILETIME counts.
uint64_t ToFileTime(time_t sec, long nsec) {
    return (static_cast<uint64_t>(sec) + 11644473600ULL) * 10000000ULL + static_cast<uint64_t>(nsec) / 100;
}

FILETIME MakeFileTime(uint64_t t) {
    return {static_cast<DWORD>(t & 0xFFFFFFFFu), static_cast<DWORD>(t >> 32)};
}

#ifdef __APPLE__
#define NSWHOME_MTIME(st) (st).st_mtimespec
#else
#define NSWHOME_MTIME(st) (st).st_mtim
#endif

struct FindState {
    DIR* dir = nullptr;
    std::string path;
#if defined(__ANDROID__)
    // A folder granted through the system picker: listed in one go by the
    // content resolver, then handed out entry by entry.
    std::vector<port::android::Entry> granted;
    size_t next = 0;
#endif
};

struct FileState {
    int fd = -1;
};

bool MakeDirs(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) { // exists already (Windows fails too)
        errno = EEXIST;
        return false;
    }
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::string parent = path.substr(0, slash);
        if (stat(parent.c_str(), &st) != 0) MakeDirs(parent);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

} // namespace

int _wfopen_s(FILE** out, const wchar_t* path, const wchar_t* mode) {
    std::string m = port::Utf8(mode);
    size_t comma = m.find(',');
    if (comma != std::string::npos) m.resize(comma);
    while (!m.empty() && m.back() == ' ') m.pop_back();
    *out = fopen(port::NativePath(path).c_str(), m.c_str());
    return *out ? 0 : (errno ? errno : EINVAL);
}

DWORD GetFileAttributesW(const wchar_t* path) {
#if defined(__ANDROID__)
    if (port::android::Granted(port::NativePath(path))) {
        port::android::Entry e;
        if (!port::android::Stat(port::NativePath(path), e)) return INVALID_FILE_ATTRIBUTES;
        return e.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    }
#endif
    struct stat st;
    if (stat(port::NativePath(path).c_str(), &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL CreateDirectoryW(const wchar_t* path, SECURITY_ATTRIBUTES*) {
    std::string p = port::NativePath(path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (MakeDirs(p)) return TRUE;
    SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS : ErrorFromErrno(errno));
    return FALSE;
}

int SHCreateDirectoryExW(HWND, const wchar_t* path, const SECURITY_ATTRIBUTES*) {
    std::string p = port::NativePath(path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (MakeDirs(p)) return ERROR_SUCCESS;
    DWORD code = errno == EEXIST ? ERROR_ALREADY_EXISTS : ErrorFromErrno(errno);
    SetLastError(code);
    return static_cast<int>(code);
}

BOOL MoveFileExW(const wchar_t* from, const wchar_t* to, DWORD flags) {
    std::string a = port::NativePath(from), b = port::NativePath(to);
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(b.c_str(), &st) == 0) return FALSE;
    return rename(a.c_str(), b.c_str()) == 0 ? TRUE : FALSE;
}

BOOL CopyFileW(const wchar_t* from, const wchar_t* to, BOOL failIfExists) {
    int in = open(port::NativePath(from).c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        SetLastError(ErrorFromErrno(errno));
        return FALSE;
    }
    int out = open(port::NativePath(to).c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (failIfExists ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) {
        SetLastError(ErrorFromErrno(errno)); // ERROR_FILE_EXISTS when the target was there and must not be replaced
        close(in);
        return FALSE;
    }
    bool ok = true;
    char buf[65536];
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ok = false;
        if (n <= 0) break;
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, static_cast<size_t>(n - done));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                ok = false;
                break;
            }
            done += w;
        }
        if (!ok) break;
    }
    close(in);
    if (close(out) != 0) ok = false;
    if (!ok) SetLastError(ERROR_ACCESS_DENIED);
    return ok ? TRUE : FALSE;
}

BOOL DeleteFileW(const wchar_t* path) {
    return unlink(port::NativePath(path).c_str()) == 0 ? TRUE : FALSE;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* out) {
    FindState* fs = static_cast<FindState*>(h);
    if (!fs || h == INVALID_HANDLE_VALUE) return FALSE;
#if defined(__ANDROID__)
    if (!fs->dir) {
        while (fs->next < fs->granted.size()) {
            const port::android::Entry& e = fs->granted[fs->next++];
            std::wstring name = port::Wide(e.name);
            if (name.empty() || name.size() >= MAX_PATH) continue;
            memset(out, 0, sizeof *out);
            out->dwFileAttributes = e.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            uint64_t size = e.dir ? 0 : e.size;
            out->nFileSizeLow = static_cast<DWORD>(size & 0xFFFFFFFFu);
            out->nFileSizeHigh = static_cast<DWORD>(size >> 32);
            out->ftLastWriteTime = MakeFileTime(
                ToFileTime(static_cast<time_t>(e.mtimeMs / 1000), static_cast<long>(e.mtimeMs % 1000) * 1000000L));
            out->ftCreationTime = out->ftLastAccessTime = out->ftLastWriteTime;
            memcpy(out->cFileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
            return TRUE;
        }
        return FALSE;
    }
#endif
    for (;;) {
        dirent* e = readdir(fs->dir);
        if (!e) return FALSE;
        std::wstring name = port::Wide(e->d_name);
        if (name.empty() || name.size() >= MAX_PATH) continue;
        struct stat st;
        std::string full = fs->path + "/" + e->d_name;
        // Links are followed; a dangling one is listed as an empty file.
        if (stat(full.c_str(), &st) != 0 && lstat(full.c_str(), &st) != 0) continue;
        memset(out, 0, sizeof *out);
        bool dir = S_ISDIR(st.st_mode);
        out->dwFileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        uint64_t size = dir ? 0 : static_cast<uint64_t>(st.st_size);
        out->nFileSizeLow = static_cast<DWORD>(size & 0xFFFFFFFFu);
        out->nFileSizeHigh = static_cast<DWORD>(size >> 32);
        out->ftLastWriteTime = MakeFileTime(ToFileTime(NSWHOME_MTIME(st).tv_sec, NSWHOME_MTIME(st).tv_nsec));
        out->ftCreationTime = out->ftLastAccessTime = out->ftLastWriteTime;
        memcpy(out->cFileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        return TRUE;
    }
}

HANDLE FindFirstFileExW(const wchar_t* pattern, FINDEX_INFO_LEVELS, WIN32_FIND_DATAW* out, FINDEX_SEARCH_OPS, void*,
                        DWORD) {
    std::string p = port::NativePath(pattern);
    if (p.size() < 2 || p.compare(p.size() - 2, 2, "/*") != 0) return INVALID_HANDLE_VALUE;
    p.resize(p.size() - 2);
    if (p.empty()) p = "/";
#if defined(__ANDROID__)
    if (port::android::Granted(p)) {
        FindState* granted = new FindState{nullptr, p, {}, 0};
        if (!port::android::List(p, granted->granted) || !FindNextFileW(granted, out)) {
            delete granted;
            return INVALID_HANDLE_VALUE;
        }
        return granted;
    }
#endif
    DIR* d = opendir(p.c_str());
    if (!d) return INVALID_HANDLE_VALUE;
    FindState* fs = new FindState;
    fs->dir = d;
    fs->path = p;
    if (!FindNextFileW(fs, out)) {
        closedir(d);
        delete fs;
        return INVALID_HANDLE_VALUE;
    }
    return fs;
}

BOOL FindClose(HANDLE h) {
    FindState* fs = static_cast<FindState*>(h);
    if (!fs || h == INVALID_HANDLE_VALUE) return FALSE;
    if (fs->dir) closedir(fs->dir);
    delete fs;
    return TRUE;
}

HANDLE CreateFileW(const wchar_t* path, DWORD access, DWORD, SECURITY_ATTRIBUTES*, DWORD disposition, DWORD, HANDLE) {
    int flags;
    if (access == GENERIC_READ && disposition == OPEN_EXISTING) flags = O_RDONLY;
    else if (access == GENERIC_WRITE && disposition == CREATE_ALWAYS) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    int fd = -1;
#if defined(__ANDROID__)
    if (flags == O_RDONLY && port::android::Granted(port::NativePath(path))) {
        fd = port::android::OpenRead(port::NativePath(path));
        if (fd < 0) errno = ENOENT;
    } else
#endif
        fd = open(port::NativePath(path).c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0) {
        SetLastError(ErrorFromErrno(errno));
        return INVALID_HANDLE_VALUE;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        close(fd);
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return new FileState{fd};
}

BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    FileState* f = static_cast<FileState*>(h);
    struct stat st;
    if (!f || h == INVALID_HANDLE_VALUE || fstat(f->fd, &st) != 0) return FALSE;
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL ReadFile(HANDLE h, void* buf, DWORD len, DWORD* got, OVERLAPPED* ov) {
    FileState* f = static_cast<FileState*>(h);
    if (got) *got = 0;
    if (!f || h == INVALID_HANDLE_VALUE) return FALSE;
    off_t at = ov ? static_cast<off_t>((static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset) : 0;
    DWORD done = 0;
    while (done < len) {
        ssize_t n = ov ? pread(f->fd, static_cast<char*>(buf) + done, len - done, at + static_cast<off_t>(done))
                       : read(f->fd, static_cast<char*>(buf) + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return FALSE;
        if (n == 0) break; // end of the file
        done += static_cast<DWORD>(n);
    }
    if (got) *got = done;
    return TRUE;
}

BOOL WriteFile(HANDLE h, const void* buf, DWORD len, DWORD* put, OVERLAPPED* ov) {
    FileState* f = static_cast<FileState*>(h);
    if (put) *put = 0;
    if (!f || h == INVALID_HANDLE_VALUE || ov) return FALSE;
    DWORD done = 0;
    while (done < len) {
        ssize_t n = write(f->fd, static_cast<const char*>(buf) + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return FALSE;
        done += static_cast<DWORD>(n);
    }
    if (put) *put = done;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h) {
    FileState* f = static_cast<FileState*>(h);
    if (!f || h == INVALID_HANDLE_VALUE) return FALSE;
    return fsync(f->fd) == 0 ? TRUE : FALSE;
}

BOOL GetDiskFreeSpaceExW(const wchar_t* path, ULARGE_INTEGER* freeForCaller, ULARGE_INTEGER* total, ULARGE_INTEGER* totalFree) {
    struct statvfs vfs;
#if defined(__ANDROID__)
    // A granted folder itself may be closed to path calls; the volume it is on
    // is what is being asked about, and a folder further up answers for it.
    if (!path) return FALSE;
    std::string at = port::NativePath(path);
    while (statvfs(at.c_str(), &vfs) != 0) {
        size_t slash = at.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return FALSE;
        at.resize(slash);
    }
#else
    if (!path || statvfs(port::NativePath(path).c_str(), &vfs) != 0) return FALSE;
#endif
    const uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    if (freeForCaller) freeForCaller->QuadPart = static_cast<uint64_t>(vfs.f_bavail) * unit;
    if (total) total->QuadPart = static_cast<uint64_t>(vfs.f_blocks) * unit;
    if (totalFree) totalFree->QuadPart = static_cast<uint64_t>(vfs.f_bfree) * unit;
    return TRUE;
}

BOOL CloseHandle(HANDLE h) {
    FileState* f = static_cast<FileState*>(h);
    if (!f || h == INVALID_HANDLE_VALUE) return FALSE;
    close(f->fd);
    delete f;
    return TRUE;
}

// ---- time and power -----------------------------------------------------------

namespace {

void FillTime(SYSTEMTIME* st, bool local) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    time_t sec = tv.tv_sec;
    tm t{};
    if (local) localtime_r(&sec, &t);
    else gmtime_r(&sec, &t);
    st->wYear = static_cast<WORD>(t.tm_year + 1900);
    st->wMonth = static_cast<WORD>(t.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(t.tm_wday);
    st->wDay = static_cast<WORD>(t.tm_mday);
    st->wHour = static_cast<WORD>(t.tm_hour);
    st->wMinute = static_cast<WORD>(t.tm_min);
    st->wSecond = static_cast<WORD>(t.tm_sec);
    st->wMilliseconds = static_cast<WORD>(tv.tv_usec / 1000);
}

} // namespace

void GetLocalTime(SYSTEMTIME* st) {
    FillTime(st, true);
}
void GetSystemTime(SYSTEMTIME* st) {
    FillTime(st, false);
}

ULONGLONG GetTickCount64() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<ULONGLONG>(ts.tv_sec) * 1000ULL + static_cast<ULONGLONG>(ts.tv_nsec) / 1000000ULL;
}

namespace {

SYSTEM_POWER_STATUS ReadPower() {
    SYSTEM_POWER_STATUS ps{};
    ps.ACLineStatus = 255;
    ps.BatteryFlag = 128; // no system battery
    ps.BatteryLifePercent = 255;
    ps.BatteryLifeTime = ps.BatteryFullLifeTime = static_cast<DWORD>(-1);
#if defined(__APPLE__)
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    CFArrayRef list = info ? IOPSCopyPowerSourcesList(info) : nullptr;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        if (!d) continue;
        CFStringRef type = static_cast<CFStringRef>(CFDictionaryGetValue(d, CFSTR(kIOPSTypeKey)));
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo) continue;
        int cur = 0, max = 0;
        CFNumberRef c = static_cast<CFNumberRef>(CFDictionaryGetValue(d, CFSTR(kIOPSCurrentCapacityKey)));
        CFNumberRef m = static_cast<CFNumberRef>(CFDictionaryGetValue(d, CFSTR(kIOPSMaxCapacityKey)));
        if (!c || !m || !CFNumberGetValue(c, kCFNumberIntType, &cur) || !CFNumberGetValue(m, kCFNumberIntType, &max) ||
            max <= 0)
            continue;
        int pct = cur * 100 / max;
        ps.BatteryLifePercent = static_cast<BYTE>(pct < 0 ? 0 : pct > 100 ? 100 : pct);
        ps.BatteryFlag = 0;
        CFStringRef state = static_cast<CFStringRef>(CFDictionaryGetValue(d, CFSTR(kIOPSPowerSourceStateKey)));
        if (state) ps.ACLineStatus = CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo ? 1 : 0;
        break;
    }
    if (list) CFRelease(list);
    if (info) CFRelease(info);
#elif defined(__ANDROID__)
    // /sys/class/power_supply is closed to apps; the system's battery service answers.
    int pct = 0;
    bool charging = false;
    if (port::android::Battery(pct, charging)) {
        ps.BatteryLifePercent = static_cast<BYTE>(pct < 0 ? 0 : pct > 100 ? 100 : pct);
        ps.BatteryFlag = 0;
        ps.ACLineStatus = charging ? 1 : 0;
    }
#else
    auto readLine = [](const std::string& file) {
        std::string out;
        if (FILE* f = fopen(file.c_str(), "r")) {
            char b[64];
            if (fgets(b, sizeof b, f)) out = b;
            fclose(f);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
        return out;
    };
    const std::string base = "/sys/class/power_supply";
    if (DIR* d = opendir(base.c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string dir = base + "/" + e->d_name;
            std::string type = readLine(dir + "/type");
            if (type == "Mains") {
                std::string online = readLine(dir + "/online");
                if (!online.empty()) ps.ACLineStatus = online == "1" ? 1 : 0;
            } else if (type == "Battery" && readLine(dir + "/scope") != "Device") { // not a mouse or a pad
                std::string cap = readLine(dir + "/capacity");
                if (cap.empty() || ps.BatteryFlag == 0) continue;
                int pct = atoi(cap.c_str());
                ps.BatteryLifePercent = static_cast<BYTE>(pct < 0 ? 0 : pct > 100 ? 100 : pct);
                ps.BatteryFlag = 0;
            }
        }
        closedir(d);
    }
#endif
    return ps;
}

} // namespace

BOOL GetSystemPowerStatus(SYSTEM_POWER_STATUS* out) {
    // Asked for on every frame by the status bar, so the answer is kept a while.
    static std::mutex lock;
    static SYSTEM_POWER_STATUS cached;
    static ULONGLONG readAt = 0;
    static bool have = false;
    std::lock_guard<std::mutex> guard(lock);
    ULONGLONG now = GetTickCount64();
    if (!have || now - readAt > 10000) {
        cached = ReadPower();
        readAt = now;
        have = true;
    }
    *out = cached;
    return TRUE;
}

// ---- base64 -------------------------------------------------------------------

BOOL CryptStringToBinaryA(const char* text, DWORD textLen, DWORD flags, BYTE* out, DWORD* outLen, DWORD* skip,
                          DWORD* usedFlags) {
    if (!text || !outLen || !(flags & CRYPT_STRING_BASE64)) return FALSE;
    if (textLen == 0) textLen = static_cast<DWORD>(strlen(text));
    auto value = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    DWORD produced = 0, capacity = out ? *outLen : 0;
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    bool overflow = false;
    for (DWORD i = 0; i < textLen; i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') {
            pad++;
            continue;
        }
        int v = value(c);
        if (v < 0 || pad) return FALSE; // not base64, or data after the padding
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out) {
                if (produced < capacity) out[produced] = static_cast<BYTE>((acc >> bits) & 0xFF);
                else overflow = true;
            }
            produced++;
        }
    }
    if (bits >= 6 || pad > 2) return FALSE; // a lone leftover character, or too much padding
    *outLen = produced;
    if (skip) *skip = 0;
    if (usedFlags) *usedFlags = CRYPT_STRING_BASE64;
    return overflow ? FALSE : TRUE;
}

BOOL CryptBinaryToStringA(const BYTE* data, DWORD dataLen, DWORD flags, char* out, DWORD* outLen) {
    if (!outLen || (flags & 0xFF) != CRYPT_STRING_BASE64) return FALSE;
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const bool crlf = !(flags & CRYPT_STRING_NOCRLF);
    std::string s;
    s.reserve((static_cast<size_t>(dataLen) + 2) / 3 * 4 + 4);
    size_t column = 0;
    auto put = [&](char c) {
        s += c;
        if (crlf && ++column == 64) {
            s += "\r\n";
            column = 0;
        }
    };
    for (DWORD i = 0; i < dataLen; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < dataLen) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < dataLen) v |= data[i + 2];
        put(digits[(v >> 18) & 63]);
        put(digits[(v >> 12) & 63]);
        put(i + 1 < dataLen ? digits[(v >> 6) & 63] : '=');
        put(i + 2 < dataLen ? digits[v & 63] : '=');
    }
    if (crlf && column != 0) s += "\r\n";
    if (!out) {
        *outLen = static_cast<DWORD>(s.size() + 1);
        return TRUE;
    }
    if (*outLen < s.size() + 1) return FALSE;
    memcpy(out, s.c_str(), s.size() + 1);
    *outLen = static_cast<DWORD>(s.size());
    return TRUE;
}

// ---- AES ----------------------------------------------------------------------
// Table-driven AES (FIPS-197). The tables are derived at start-up rather than
// typed in: the S-box from the field inverse and affine map, the round tables
// from the S-box.

namespace {

struct AesTables {
    uint8_t sbox[256], inv[256];
    uint32_t te[4][256], td[4][256];

    static uint8_t mul(uint8_t a, uint8_t b) { // product in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1
        uint8_t p = 0;
        while (b) {
            if (b & 1) p ^= a;
            a = static_cast<uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
            b >>= 1;
        }
        return p;
    }
    static uint32_t ror8(uint32_t v) { return (v >> 8) | (v << 24); }

    AesTables() {
        // 3 generates the multiplicative group: p walks it forwards, q
        // backwards (dividing by 3), so q is always 1/p.
        uint8_t p = 1, q = 1;
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= static_cast<uint8_t>(q << 1);
            q ^= static_cast<uint8_t>(q << 2);
            q ^= static_cast<uint8_t>(q << 4);
            if (q & 0x80) q ^= 0x09;
            auto rotl = [](uint8_t v, int n) { return static_cast<uint8_t>((v << n) | (v >> (8 - n))); };
            sbox[p] = static_cast<uint8_t>(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) inv[sbox[i]] = static_cast<uint8_t>(i);
        for (int i = 0; i < 256; i++) {
            uint8_t s = sbox[i];
            uint32_t e = (static_cast<uint32_t>(mul(s, 2)) << 24) | (static_cast<uint32_t>(s) << 16) |
                         (static_cast<uint32_t>(s) << 8) | mul(s, 3);
            uint8_t v = inv[i];
            uint32_t d = (static_cast<uint32_t>(mul(v, 14)) << 24) | (static_cast<uint32_t>(mul(v, 9)) << 16) |
                         (static_cast<uint32_t>(mul(v, 13)) << 8) | mul(v, 11);
            for (int t = 0; t < 4; t++) {
                te[t][i] = e;
                td[t][i] = d;
                e = ror8(e);
                d = ror8(d);
            }
        }
    }
};

const AesTables& Tables() {
    static const AesTables t;
    return t;
}

uint32_t Load32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
void Store32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

} // namespace

namespace port {

bool Aes::SetKey(const uint8_t* key, size_t len) {
    if (len != 16 && len != 24 && len != 32) return false;
    const AesTables& T = Tables();
    const int nk = static_cast<int>(len / 4);
    rounds = nk + 6;
    const int total = 4 * (rounds + 1);
    auto subWord = [&](uint32_t w) {
        return (static_cast<uint32_t>(T.sbox[w >> 24]) << 24) | (static_cast<uint32_t>(T.sbox[(w >> 16) & 255]) << 16) |
               (static_cast<uint32_t>(T.sbox[(w >> 8) & 255]) << 8) | T.sbox[w & 255];
    };
    for (int i = 0; i < nk; i++) enc[i] = Load32(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = enc[i - 1];
        if (i % nk == 0) {
            t = subWord((t << 8) | (t >> 24)) ^ (static_cast<uint32_t>(rcon) << 24);
            rcon = AesTables::mul(rcon, 2);
        } else if (nk > 6 && i % nk == 4) {
            t = subWord(t);
        }
        enc[i] = enc[i - nk] ^ t;
    }
    // Decryption runs the same round shape with the keys in reverse order and
    // the middle ones passed through InvMixColumns.
    for (int r = 0; r <= rounds; r++) {
        for (int j = 0; j < 4; j++) {
            uint32_t w = enc[4 * (rounds - r) + j];
            if (r > 0 && r < rounds)
                w = T.td[0][T.sbox[w >> 24]] ^ T.td[1][T.sbox[(w >> 16) & 255]] ^ T.td[2][T.sbox[(w >> 8) & 255]] ^
                    T.td[3][T.sbox[w & 255]];
            dec[4 * r + j] = w;
        }
    }
    return true;
}

void Aes::EncryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const AesTables& T = Tables();
    const uint32_t* rk = enc;
    uint32_t s0 = Load32(in) ^ rk[0], s1 = Load32(in + 4) ^ rk[1], s2 = Load32(in + 8) ^ rk[2], s3 = Load32(in + 12) ^ rk[3];
    for (int r = 1; r < rounds; r++) {
        rk += 4;
        uint32_t t0 = T.te[0][s0 >> 24] ^ T.te[1][(s1 >> 16) & 255] ^ T.te[2][(s2 >> 8) & 255] ^ T.te[3][s3 & 255] ^ rk[0];
        uint32_t t1 = T.te[0][s1 >> 24] ^ T.te[1][(s2 >> 16) & 255] ^ T.te[2][(s3 >> 8) & 255] ^ T.te[3][s0 & 255] ^ rk[1];
        uint32_t t2 = T.te[0][s2 >> 24] ^ T.te[1][(s3 >> 16) & 255] ^ T.te[2][(s0 >> 8) & 255] ^ T.te[3][s1 & 255] ^ rk[2];
        uint32_t t3 = T.te[0][s3 >> 24] ^ T.te[1][(s0 >> 16) & 255] ^ T.te[2][(s1 >> 8) & 255] ^ T.te[3][s2 & 255] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    rk += 4;
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
        return ((static_cast<uint32_t>(T.sbox[a >> 24]) << 24) | (static_cast<uint32_t>(T.sbox[(b >> 16) & 255]) << 16) |
                (static_cast<uint32_t>(T.sbox[(c >> 8) & 255]) << 8) | T.sbox[d & 255]) ^ k;
    };
    Store32(out, last(s0, s1, s2, s3, rk[0]));
    Store32(out + 4, last(s1, s2, s3, s0, rk[1]));
    Store32(out + 8, last(s2, s3, s0, s1, rk[2]));
    Store32(out + 12, last(s3, s0, s1, s2, rk[3]));
}

void Aes::DecryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const AesTables& T = Tables();
    const uint32_t* rk = dec;
    uint32_t s0 = Load32(in) ^ rk[0], s1 = Load32(in + 4) ^ rk[1], s2 = Load32(in + 8) ^ rk[2], s3 = Load32(in + 12) ^ rk[3];
    for (int r = 1; r < rounds; r++) {
        rk += 4;
        uint32_t t0 = T.td[0][s0 >> 24] ^ T.td[1][(s3 >> 16) & 255] ^ T.td[2][(s2 >> 8) & 255] ^ T.td[3][s1 & 255] ^ rk[0];
        uint32_t t1 = T.td[0][s1 >> 24] ^ T.td[1][(s0 >> 16) & 255] ^ T.td[2][(s3 >> 8) & 255] ^ T.td[3][s2 & 255] ^ rk[1];
        uint32_t t2 = T.td[0][s2 >> 24] ^ T.td[1][(s1 >> 16) & 255] ^ T.td[2][(s0 >> 8) & 255] ^ T.td[3][s3 & 255] ^ rk[2];
        uint32_t t3 = T.td[0][s3 >> 24] ^ T.td[1][(s2 >> 16) & 255] ^ T.td[2][(s1 >> 8) & 255] ^ T.td[3][s0 & 255] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    rk += 4;
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
        return ((static_cast<uint32_t>(T.inv[a >> 24]) << 24) | (static_cast<uint32_t>(T.inv[(b >> 16) & 255]) << 16) |
                (static_cast<uint32_t>(T.inv[(c >> 8) & 255]) << 8) | T.inv[d & 255]) ^ k;
    };
    Store32(out, last(s0, s3, s2, s1, rk[0]));
    Store32(out + 4, last(s1, s0, s3, s2, rk[1]));
    Store32(out + 8, last(s2, s1, s0, s3, rk[2]));
    Store32(out + 12, last(s3, s2, s1, s0, rk[3]));
}

} // namespace port

namespace {
int aesProviderTag; // its address is the one algorithm handle there is
}

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* out, const wchar_t* algorithm, const wchar_t*, ULONG) {
    if (!out || !algorithm || wcscmp(algorithm, BCRYPT_AES_ALGORITHM) != 0) return static_cast<NTSTATUS>(0xC0000225); // not found
    *out = &aesProviderTag;
    return 0;
}

NTSTATUS BCryptSetProperty(void* object, const wchar_t* property, PUCHAR value, ULONG valueLen, ULONG) {
    if (object != &aesProviderTag || !property || !value) return static_cast<NTSTATUS>(0xC000000D); // invalid parameter
    const wchar_t ecb[] = BCRYPT_CHAIN_MODE_ECB;
    // The value is a wide string, passed as bytes with or without its NUL.
    bool isEcb = (valueLen == sizeof(ecb) || valueLen == sizeof(ecb) - sizeof(wchar_t)) && memcmp(value, ecb, valueLen) == 0;
    if (wcscmp(property, BCRYPT_CHAINING_MODE) != 0 || !isEcb) return static_cast<NTSTATUS>(0xC00000BB); // not supported
    return 0;
}

NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE, ULONG) {
    return 0;
}

NTSTATUS BCryptGenerateSymmetricKey(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE* out, PUCHAR, ULONG, PUCHAR secret,
                                    ULONG secretLen, ULONG) {
    if (alg != &aesProviderTag || !out || !secret) return static_cast<NTSTATUS>(0xC000000D);
    port::Aes* key = new port::Aes;
    if (!key->SetKey(secret, secretLen)) {
        delete key;
        return static_cast<NTSTATUS>(0xC000000D);
    }
    *out = key;
    return 0;
}

NTSTATUS BCryptDestroyKey(BCRYPT_KEY_HANDLE key) {
    port::Aes* k = static_cast<port::Aes*>(key);
    if (!k) return static_cast<NTSTATUS>(0xC0000008); // invalid handle
    SecureZeroMemory(k, sizeof *k);
    delete k;
    return 0;
}

static NTSTATUS AesEcb(BCRYPT_KEY_HANDLE key, PUCHAR in, ULONG inLen, PUCHAR iv, PUCHAR out, ULONG outLen, ULONG* done,
                       bool decrypt) {
    const port::Aes* k = static_cast<const port::Aes*>(key);
    if (!k || !in || !done || iv) return static_cast<NTSTATUS>(0xC000000D);
    if (inLen % 16 != 0) return static_cast<NTSTATUS>(0xC0000206); // invalid buffer size
    *done = inLen;
    if (!out) return 0; // size query
    if (outLen < inLen) return static_cast<NTSTATUS>(0xC0000023); // buffer too small
    for (ULONG i = 0; i < inLen; i += 16) {
        uint8_t block[16];
        if (decrypt) k->DecryptBlock(in + i, block);
        else k->EncryptBlock(in + i, block);
        memcpy(out + i, block, 16);
    }
    return 0;
}

NTSTATUS BCryptEncrypt(BCRYPT_KEY_HANDLE key, PUCHAR in, ULONG inLen, void*, PUCHAR iv, ULONG, PUCHAR out, ULONG outLen,
                       ULONG* done, ULONG) {
    return AesEcb(key, in, inLen, iv, out, outLen, done, false);
}

NTSTATUS BCryptDecrypt(BCRYPT_KEY_HANDLE key, PUCHAR in, ULONG inLen, void*, PUCHAR iv, ULONG, PUCHAR out, ULONG outLen,
                       ULONG* done, ULONG) {
    return AesEcb(key, in, inLen, iv, out, outLen, done, true);
}

NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE, PUCHAR buf, ULONG len, ULONG) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return static_cast<NTSTATUS>(0xC0000001);
    ULONG got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<ULONG>(n);
    }
    close(fd);
    return got == len ? 0 : static_cast<NTSTATUS>(0xC0000001);
}
