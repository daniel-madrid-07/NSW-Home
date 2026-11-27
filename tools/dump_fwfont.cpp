// Test CLI for src/fwfont.cpp.
// Usage: dump_fwfont <outDir> [--registered <dir>] [--keys <dir>]
// Decodes the shared system fonts from the installed firmware and writes <outDir>\<name>.ttf.
// Prints, per font: source file, family name (from the name table and as DirectWrite reports it),
// size, glyph count, coverage of a few code points, and for the extended fonts the private-use
// ranges they cover (with glyph names where the font carries them).
// Defaults: %APPDATA%\Ryujinx\bis\system\Contents\registered and %APPDATA%\Ryujinx\system.
// <outDir> must be outside the project: the fonts are firmware assets.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dwrite_3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/fwfont.h"

typedef std::vector<uint8_t> Bytes;

static std::wstring envVar(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

static bool writeFile(const std::wstring& path, const Bytes& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

static double now() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// ---------------------------------------------------------------- sfnt reading (bounds-checked)

struct Table {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool has(size_t off, size_t len) const { return off <= n && len <= n - off; }
    unsigned u16(size_t off) const { return has(off, 2) ? static_cast<unsigned>((p[off] << 8) | p[off + 1]) : 0u; }
    uint32_t u32(size_t off) const { return has(off, 4) ? (static_cast<uint32_t>(u16(off)) << 16) | u16(off + 2) : 0u; }
    Table from(size_t off) const { return off <= n ? Table{p + off, n - off} : Table{}; }
};

static Table findTable(const Bytes& f, const char* tag) {
    Table all{f.data(), f.size()};
    unsigned count = all.u16(4);
    for (unsigned i = 0; i < count; ++i) {
        size_t r = 12 + static_cast<size_t>(i) * 16;
        if (!all.has(r, 16) || std::memcmp(f.data() + r, tag, 4) != 0) continue;
        size_t off = all.u32(r + 8), len = all.u32(r + 12);
        if (!all.has(off, len)) return Table{};
        return Table{f.data() + off, len};
    }
    return Table{};
}

// code point -> glyph index, from the widest Unicode cmap subtable (format 12, else format 4).
static std::map<uint32_t, unsigned> readCmap(const Bytes& f) {
    std::map<uint32_t, unsigned> map;
    Table cmap = findTable(f, "cmap");
    unsigned count = cmap.u16(2);
    Table best;
    int bestRank = 0;
    for (unsigned i = 0; i < count; ++i) {
        size_t r = 4 + static_cast<size_t>(i) * 8;
        unsigned platform = cmap.u16(r), encoding = cmap.u16(r + 2);
        Table sub = cmap.from(cmap.u32(r + 4));
        unsigned format = sub.u16(0);
        int rank = 0;
        if (format == 12 && ((platform == 3 && encoding == 10) || platform == 0)) rank = 4;
        else if (format == 4 && platform == 3 && encoding == 1) rank = 3;
        else if (format == 4 && platform == 0) rank = 2;
        if (rank > bestRank) {
            bestRank = rank;
            best = sub;
        }
    }
    if (bestRank == 0) return map;

    if (best.u16(0) == 12) {
        uint32_t groups = best.u32(12);
        for (uint32_t g = 0; g < groups && best.has(16 + static_cast<size_t>(g) * 12, 12); ++g) {
            size_t e = 16 + static_cast<size_t>(g) * 12;
            uint32_t first = best.u32(e), last = best.u32(e + 4), glyph = best.u32(e + 8);
            if (last < first || last > 0x10ffff) continue;
            for (uint32_t c = first; c <= last; ++c)
                if (glyph + (c - first) != 0) map[c] = glyph + (c - first);
        }
    } else {
        unsigned segs = best.u16(6) / 2;
        size_t endAt = 14, startAt = 16 + static_cast<size_t>(segs) * 2, deltaAt = startAt + static_cast<size_t>(segs) * 2,
               rangeAt = deltaAt + static_cast<size_t>(segs) * 2;
        for (unsigned s = 0; s < segs; ++s) {
            unsigned end = best.u16(endAt + s * 2), start = best.u16(startAt + s * 2);
            unsigned delta = best.u16(deltaAt + s * 2), range = best.u16(rangeAt + s * 2);
            if (end < start) continue;
            for (unsigned c = start; c <= end && c != 0xffff; ++c) {
                unsigned glyph;
                if (range == 0) {
                    glyph = (c + delta) & 0xffff;
                } else {
                    size_t at = rangeAt + s * 2 + range + static_cast<size_t>(c - start) * 2;
                    glyph = best.u16(at);
                    if (glyph != 0) glyph = (glyph + delta) & 0xffff;
                }
                if (glyph != 0) map[c] = glyph;
            }
        }
    }
    return map;
}

// Glyph names from a format 2.0 `post` table; empty if the font has none. Names that only
// restate the code point ("uniE0A0") are dropped.
static std::vector<std::string> readGlyphNames(const Bytes& f) {
    std::vector<std::string> names;
    Table post = findTable(f, "post");
    if (post.u32(0) != 0x00020000) return names;
    unsigned count = post.u16(32);
    size_t strings = 34 + static_cast<size_t>(count) * 2;
    std::vector<std::string> extra;
    for (size_t at = strings; post.has(at, 1);) {
        size_t len = post.p[at];
        if (!post.has(at + 1, len)) break;
        extra.emplace_back(reinterpret_cast<const char*>(post.p + at + 1), len);
        at += 1 + len;
    }
    names.resize(count);
    for (unsigned g = 0; g < count; ++g) {
        unsigned index = post.u16(34 + static_cast<size_t>(g) * 2);
        if (index >= 258 && index - 258 < extra.size() && extra[index - 258].compare(0, 3, "uni") != 0)
            names[g] = extra[index - 258];
    }
    return names;
}

// ---------------------------------------------------------------- DirectWrite's view of the font

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
};

static std::wstring localized(IDWriteLocalizedStrings* names) {
    if (!names || names->GetCount() == 0) return std::wstring();
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
    UINT32 len = 0;
    if (FAILED(names->GetStringLength(index, &len))) return std::wstring();
    std::wstring s(len + 1, L'\0');
    if (FAILED(names->GetString(index, &s[0], len + 1))) return std::wstring();
    s.resize(len);
    return s;
}

// Loads the font bytes through an in-memory loader into a custom font set, the way the launcher
// will, and returns the family name of the resulting collection. model: 0 = the default
// collection (weight-stretch-style families), 1 = typographic families (needs IDWriteFactory6).
// `found` tells whether that collection resolves `lookFor` (the name-table family) by name.
static std::wstring dwriteFamily(const Bytes& font, int model, const std::wstring& lookFor, bool& found, std::string& why) {
    found = false;
    Com<IDWriteFactory5> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5),
                                   reinterpret_cast<IUnknown**>(&factory.p)))) {
        why = "no IDWriteFactory5";
        return std::wstring();
    }
    Com<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(factory->CreateInMemoryFontFileLoader(&loader.p)) || FAILED(factory->RegisterFontFileLoader(loader.p))) {
        why = "no in-memory loader";
        return std::wstring();
    }
    std::wstring result;
    {
        Com<IDWriteFontFile> file;
        Com<IDWriteFontSetBuilder1> builder;
        Com<IDWriteFontSet> set;
        Com<IDWriteFontCollection1> collection;
        Com<IDWriteFontFamily1> family;
        Com<IDWriteLocalizedStrings> names;
        HRESULT hr = loader->CreateInMemoryFontFileReference(factory.p, font.data(), static_cast<UINT32>(font.size()),
                                                             nullptr, &file.p);
        if (SUCCEEDED(hr)) hr = factory->CreateFontSetBuilder(&builder.p);
        if (SUCCEEDED(hr)) hr = builder->AddFontFile(file.p);
        if (SUCCEEDED(hr)) hr = builder->CreateFontSet(&set.p);
        if (SUCCEEDED(hr) && model == 0) hr = factory->CreateFontCollectionFromFontSet(set.p, &collection.p);
        if (SUCCEEDED(hr) && model == 1) {
            Com<IDWriteFactory6> factory6;
            Com<IDWriteFontCollection2> collection2;
            hr = factory.p->QueryInterface(__uuidof(IDWriteFactory6), reinterpret_cast<void**>(&factory6.p));
            if (SUCCEEDED(hr))
                hr = factory6->CreateFontCollectionFromFontSet(set.p, DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &collection2.p);
            if (SUCCEEDED(hr)) {
                collection.p = collection2.p;
                collection2.p = nullptr;
            }
        }
        if (SUCCEEDED(hr) && collection->GetFontFamilyCount() != 1) {
            why = "families: " + std::to_string(collection->GetFontFamilyCount());
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) hr = collection->GetFontFamily(0, &family.p);
        if (SUCCEEDED(hr)) hr = family->GetFamilyNames(&names.p);
        if (SUCCEEDED(hr)) {
            result = localized(names.p);
            UINT32 index = 0;
            BOOL exists = FALSE;
            found = SUCCEEDED(collection->FindFamilyName(lookFor.c_str(), &index, &exists)) && exists;
        }
        else if (why.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "hr=0x%08lx", static_cast<unsigned long>(hr));
            why = buf;
        }
    }
    factory->UnregisterFontFileLoader(loader.p);
    return result;
}

// ---------------------------------------------------------------- main

int wmain(int argc, wchar_t** argv) {
    std::wstring outDir;
    std::wstring appData = envVar(L"APPDATA");
    std::wstring registered = appData + L"\\Ryujinx\\bis\\system\\Contents\\registered";
    std::wstring keys = appData + L"\\Ryujinx\\system";
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--registered" && i + 1 < argc) registered = argv[++i];
        else if (a == L"--keys" && i + 1 < argc) keys = argv[++i];
        else if (outDir.empty()) outDir = a;
    }
    if (outDir.empty()) {
        std::fprintf(stderr, "usage: dump_fwfont <outDir> [--registered <dir>] [--keys <dir>]\n");
        return 2;
    }
    CreateDirectoryW(outDir.c_str(), nullptr);

    static const char* const kLabels[] = {"standard", "extended", "extended 2"};
    struct Probe {
        uint32_t cp;
        const char* label;
    };
    static const Probe kProbes[] = {
        {0x0041, "A"}, {0x00f1, "n-tilde"}, {0x00bf, "inverted ?"}, {0x00e9, "e-acute"}, {0x3042, "hiragana A"}, {0x6f22, "kanji 6F22"},
    };

    int bad = 0;
    for (int which = 0; which < 3; ++which) {
        fwfont::Font font;
        std::string err;
        double t0 = now();
        bool ok = fwfont::Load(registered, keys, which, font, err);
        double ms = (now() - t0) * 1000.0;
        std::printf("== %d (%s)\n", which, kLabels[which]);
        if (!ok) {
            std::printf("   FAILED: %s (%.1f ms)\n", err.c_str(), ms);
            ++bad;
            continue;
        }

        std::map<uint32_t, unsigned> cmap = readCmap(font.data);
        unsigned glyphs = findTable(font.data, "maxp").u16(4);
        bool cff = font.data.size() >= 4 && std::memcmp(font.data.data(), "OTTO", 4) == 0;
        std::printf("   source      %s\n", font.source.c_str());
        std::printf("   family      \"%s\" (name table)\n", narrow(font.family).c_str());
        for (int model = 0; model < 2; ++model) {
            std::string why;
            bool found = false;
            std::wstring dw = dwriteFamily(font.data, model, font.family, found, why);
            std::printf("   DirectWrite \"%s\" (%s)%s%s; FindFamilyName(family): %s\n", narrow(dw).c_str(),
                        model == 0 ? "weight-stretch-style model" : "typographic model",
                        dw.empty() ? " UNAVAILABLE " : dw == font.family ? " = family" : " differs from family", why.c_str(),
                        found ? "found" : "NOT found");
            if (model == 1 && (dw != font.family || !found)) ++bad;
        }
        std::printf("   size        %zu bytes, %s outlines, units/em %u\n", font.data.size(), cff ? "CFF" : "TrueType",
                    findTable(font.data, "head").u16(18));
        std::printf("   glyphs      %u, mapped code points %zu, load time %.1f ms\n", glyphs, cmap.size(), ms);
        std::printf("   coverage   ");
        for (const Probe& p : kProbes)
            std::printf(" U+%04X %s: %s%s", static_cast<unsigned>(p.cp), p.label, cmap.count(p.cp) ? "yes" : "NO",
                        &p == &kProbes[5] ? "\n" : ",");

        // Private Use Area (U+E000..U+F8FF) as ranges.
        std::printf("   private use");
        size_t pua = 0;
        uint32_t first = 0, prev = 0;
        bool open = false;
        auto flush = [&] {
            if (!open) return;
            if (first == prev) std::printf(" U+%04X", static_cast<unsigned>(first));
            else std::printf(" U+%04X-U+%04X", static_cast<unsigned>(first), static_cast<unsigned>(prev));
            open = false;
        };
        for (const auto& e : cmap) {
            if (e.first < 0xe000 || e.first > 0xf8ff) continue;
            ++pua;
            if (open && e.first == prev + 1) {
                prev = e.first;
                continue;
            }
            flush();
            first = prev = e.first;
            open = true;
        }
        flush();
        std::printf("%s (%zu code points)\n", pua ? "" : " none", pua);

        if (which > 0) {
            std::vector<std::string> names = readGlyphNames(font.data);
            size_t named = 0;
            for (const auto& e : cmap)
                if (e.first >= 0xe000 && e.first <= 0xf8ff && e.second < names.size() && !names[e.second].empty()) ++named;
            // a handful of stray names among uniXXXX ones says nothing about the symbols
            if (named * 2 < pua) named = 0;
            std::printf("   glyph names %s\n", named ? "from the post table:" : "none usable in the font (uniXXXX only)");
            if (named) {
                int col = 0;
                for (const auto& e : cmap) {
                    if (e.first < 0xe000 || e.first > 0xf8ff || e.second >= names.size() || names[e.second].empty()) continue;
                    std::printf("%s U+%04X=%s", col == 0 ? "     " : "", static_cast<unsigned>(e.first), names[e.second].c_str());
                    if (++col == 6) {
                        std::printf("\n");
                        col = 0;
                    }
                }
                if (col) std::printf("\n");
            }
        }

        std::string stem = font.source.substr(0, font.source.find_last_of('.'));
        std::wstring fileName;
        for (char c : stem) {
            bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            fileName += safe ? static_cast<wchar_t>(c) : L'_';
        }
        if (!writeFile(outDir + L"\\" + fileName + (cff ? L".otf" : L".ttf"), font.data)) {
            std::fprintf(stderr, "cannot write %s\n", stem.c_str());
            ++bad;
        }
    }
    std::printf("%d problem(s)\n", bad);
    return bad ? 1 : 0;
}
