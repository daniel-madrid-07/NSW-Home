// Test CLI for src/nsw.cpp.
// Usage: dump_title <file.nsp|file.xci> [outDir] [--keys <dir>] [--lang 6,0]
// Prints the title metadata as JSON (UTF-8) and writes the icon to <outDir>\<titleId>.jpg.
// Default keys dir: %APPDATA%\Ryujinx\system. Default languages: Spanish, AmericanEnglish.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "../src/nsw.h"

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            o += '\\';
            o += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            o += buf;
        } else {
            o += ch;
        }
    }
    return o + "\"";
}

static std::wstring envVar(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

// Width/height from the JPEG SOF marker; false if this is not a JPEG.
static bool jpegSize(const std::vector<uint8_t>& b, unsigned& width, unsigned& height) {
    if (b.size() < 4 || b[0] != 0xff || b[1] != 0xd8) return false;
    size_t p = 2;
    while (p + 9 < b.size()) {
        if (b[p] != 0xff) return false;
        uint8_t marker = b[p + 1];
        if (marker == 0xff) {
            ++p;
            continue;
        }
        size_t len = (static_cast<size_t>(b[p + 2]) << 8) | b[p + 3];
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            height = (static_cast<unsigned>(b[p + 5]) << 8) | b[p + 6];
            width = (static_cast<unsigned>(b[p + 7]) << 8) | b[p + 8];
            return true;
        }
        p += 2 + len;
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> positional;
    std::wstring keysDir = envVar(L"APPDATA") + L"\\Ryujinx\\system";
    std::vector<int> languages = {6, 0};
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--keys" && i + 1 < argc) {
            keysDir = argv[++i];
        } else if (a == L"--lang" && i + 1 < argc) {
            languages.clear();
            const wchar_t* p = argv[++i];
            while (*p) {
                wchar_t* end = nullptr;
                long v = std::wcstol(p, &end, 10);
                if (end == p) break;
                languages.push_back(static_cast<int>(v));
                p = *end == L',' ? end + 1 : end;
            }
        } else {
            positional.push_back(a);
        }
    }
    if (positional.empty()) {
        std::fputs("usage: dump_title <file.nsp|file.xci> [outDir] [--keys <dir>] [--lang 6,0]\n", stderr);
        return 2;
    }
    const std::wstring& file = positional[0];
    std::wstring outDir = positional.size() > 1 ? positional[1] : L"build\\icons";

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    nsw::Title t;
    std::string err;
    bool ok = nsw::ReadTitle(file, keysDir, languages, t, err);
    QueryPerformanceCounter(&t1);
    double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);

    if (!ok) {
        std::fprintf(stderr, "{\"error\": %s}\n", jsonString(err).c_str());
        return 1;
    }

    std::wstring iconPath;
    bool iconWritten = false;
    if (!t.icon.empty()) {
        CreateDirectoryW(outDir.c_str(), nullptr);
        iconPath = outDir + L"\\" + std::wstring(t.titleId.begin(), t.titleId.end()) + L".jpg";
        HANDLE h = CreateFileW(iconPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            iconWritten = WriteFile(h, t.icon.data(), static_cast<DWORD>(t.icon.size()), &written, nullptr) &&
                          written == t.icon.size();
            CloseHandle(h);
        }
    }

    unsigned width = 0, height = 0;
    bool jpeg = jpegSize(t.icon, width, height);
    std::printf("{\"titleId\": %s, \"name\": %s, \"publisher\": %s, \"displayVersion\": %s, ",
                jsonString(t.titleId).c_str(), jsonString(utf8(t.name)).c_str(), jsonString(utf8(t.publisher)).c_str(),
                jsonString(utf8(t.version)).c_str());
    if (t.icon.empty()) {
        std::printf("\"icon\": null, ");
    } else {
        std::printf("\"icon\": {\"bytes\": %zu, \"path\": %s, \"valid\": %s, \"width\": %u, \"height\": %u}, ",
                    t.icon.size(), iconWritten ? jsonString(utf8(iconPath)).c_str() : "null", jpeg ? "true" : "false",
                    width, height);
    }
    std::printf("\"ms\": %.1f}\n", ms);
    return 0;
}
