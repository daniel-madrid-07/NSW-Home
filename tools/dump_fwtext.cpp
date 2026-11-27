// Test CLI for src/fwtext.cpp.
// Usage: dump_fwtext <outFile> <folder> [<folder> ...] [--registered <dir>] [--keys <dir>]
// Writes every message of the given language folders ("EUes", "USen", ...) as tab-separated
// lines: key, then the text of each folder in turn (line breaks as \n, value tokens as {gt:params}).
// Prints only counts. <outFile> must be outside the project: the texts are the firmware's.
// Defaults: %APPDATA%\Ryujinx\bis\system\Contents\registered and %APPDATA%\Ryujinx\system.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "../src/fwtext.h"

static std::wstring envVar(const wchar_t* n) {
    wchar_t* v = nullptr;
    size_t len = 0;
    std::wstring out;
    if (!_wdupenv_s(&v, &len, n) && v) out = v;
    free(v);
    return out;
}

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring shown(const std::wstring& t) {
    std::wstring s;
    for (wchar_t c : t) {
        if (c == L'\n') s += L"\\n";
        else if (c == L'\t') s += L"\\t";
        else if (c == 0xE000) s += L'{';
        else if (c == 0xE001) s += L'}';
        else s += c;
    }
    return s;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring reg = envVar(L"APPDATA") + L"\\Ryujinx\\bis\\system\\Contents\\registered";
    std::wstring keys = envVar(L"APPDATA") + L"\\Ryujinx\\system";
    std::wstring outFile;
    std::vector<std::string> folders;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--registered" && i + 1 < argc) reg = argv[++i];
        else if (a == L"--keys" && i + 1 < argc) keys = argv[++i];
        else if (outFile.empty()) outFile = a;
        else folders.push_back(utf8(a));
    }
    if (outFile.empty() || folders.empty()) {
        fprintf(stderr, "usage: dump_fwtext <outFile> <folder>...\n");
        return 2;
    }
    std::vector<std::map<std::string, std::wstring>> msgs;
    std::string err;
    DWORD t0 = GetTickCount();
    if (!fwtext::LoadMany(reg, keys, folders, msgs, err)) {
        printf("fail %s\n", err.c_str());
        return 1;
    }
    printf("loaded in %lu ms\n", GetTickCount() - t0);
    std::set<std::string> all;
    for (size_t i = 0; i < folders.size(); i++) {
        printf("%s: %zu messages\n", folders[i].c_str(), msgs[i].size());
        for (const auto& m : msgs[i]) all.insert(m.first);
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, outFile.c_str(), L"wb") || !f) return 1;
    for (const std::string& k : all) {
        std::string line = k;
        for (auto& m : msgs) {
            auto it = m.find(k);
            line += "\t" + (it == m.end() ? std::string() : utf8(shown(it->second)));
        }
        line += "\n";
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
    return 0;
}
