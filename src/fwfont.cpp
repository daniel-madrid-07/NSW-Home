// Shared system fonts from the installed firmware. See fwfont.h.
//
// Path through the data (verified on firmware 19.0.0):
//   registered\<id>.nca\00      NCA; program ID 0100000000000811 (standard) or ...0810 (extended), content type Data
//   -> RomFS section            one or two *.bfttf, found by name
//   -> BFTTF                    8-byte header + a plain TrueType file, all of it XORed with one 32-bit key
//
// BFTTF, after undoing the XOR (key bytes 49 62 18 06 repeating from offset 0, i.e. 0x06186249
// when the file is taken as little-endian words):
//   0x00  magic 7f 9a 02 18
//   0x04  u32 big-endian: size of the font that follows (= file size - 8)
//   0x08  the font: sfnt version 00 01 00 00 (or "OTTO"), table directory, tables
// The key is not trusted blindly: magic, length and sfnt signature are all checked, and the
// table directory must lie inside the file.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "fwfont.h"
#include "nsw_core.h"

#include <cstring>
#include <map>

namespace fwfont {
namespace {

using namespace nsw::core;

const uint8_t kXorKey[4] = {0x49, 0x62, 0x18, 0x06};
const uint8_t kMagic[4] = {0x7f, 0x9a, 0x02, 0x18};
const uint64_t kMaxFontFile = 0x4000000;  // 64 MB; the largest shared font is under 8 MB

struct Which {
    uint64_t title;
    const char* prefix;  // file name starts with this (the part after it is a version, "003")
};
const Which kWhich[] = {
    {0x0100000000000811ull, "nintendo_udsg-r_std_"},
    {0x0100000000000810ull, "nintendo_ext_"},
    {0x0100000000000810ull, "nintendo_ext2_"},
    {0x0100000000000812ull, "nintendo_udsg-r_ko_"},
    {0x0100000000000813ull, "nintendo_udjxh-db_zh-tw_"},
    {0x0100000000000814ull, "nintendo_udsg-r_org_zh-cn_"},
};

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Finds a table in the sfnt directory; false if absent or out of range.
bool findTable(const Bytes& f, const char* tag, size_t& offset, size_t& length) {
    if (f.size() < 12) return false;
    size_t count = be16(&f[4]);
    if (12 + count * 16 > f.size()) return false;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* r = &f[12 + i * 16];
        if (std::memcmp(r, tag, 4) != 0) continue;
        uint64_t off = be32(r + 8), len = be32(r + 12);
        if (off > f.size() || len > f.size() - off) return false;
        offset = static_cast<size_t>(off);
        length = static_cast<size_t>(len);
        return true;
    }
    return false;
}

// A valid font for our purposes: sfnt signature, a table directory that fits, and every table
// inside the file. (Checksums are left to DirectWrite.)
bool validSfnt(const Bytes& f) {
    if (f.size() < 12) return false;
    static const uint8_t trueType[4] = {0x00, 0x01, 0x00, 0x00};
    if (std::memcmp(f.data(), trueType, 4) != 0 && std::memcmp(f.data(), "OTTO", 4) != 0) return false;
    size_t count = be16(&f[4]);
    if (count == 0 || 12 + count * 16 > f.size()) return false;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* r = &f[12 + i * 16];
        uint64_t off = be32(r + 8), len = be32(r + 12);
        if (off > f.size() || len > f.size() - off) return false;
    }
    size_t o, n;
    return findTable(f, "cmap", o, n) && findTable(f, "head", o, n) && findTable(f, "name", o, n);
}

// Family name as DirectWrite's typographic model reports it: name ID 16 if the font has one,
// otherwise ID 1; Windows platform, US English preferred.
std::wstring familyName(const Bytes& f) {
    size_t base, size;
    if (!findTable(f, "name", base, size) || size < 6) return std::wstring();
    const uint8_t* t = &f[base];
    size_t count = be16(t + 2);
    size_t strings = be16(t + 4);
    if (6 + count * 12 > size || strings > size) return std::wstring();

    std::wstring best;
    int bestScore = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* r = t + 6 + i * 12;
        unsigned platform = be16(r), encoding = be16(r + 2), language = be16(r + 4), id = be16(r + 6);
        size_t len = be16(r + 8), off = be16(r + 10);
        if ((id != 1 && id != 16) || len == 0 || off > size - strings || len > size - strings - off) continue;

        int score;
        bool utf16;
        if (platform == 3 && (encoding == 1 || encoding == 10 || encoding == 0)) {
            utf16 = true;
            score = language == 0x0409 ? 40 : (language & 0x3ff) == 0x09 ? 30 : 20;
        } else if (platform == 0) {
            utf16 = true;
            score = 15;
        } else if (platform == 1 && encoding == 0) {
            utf16 = false;  // Mac Roman; ASCII range only is taken
            score = language == 0 ? 10 : 5;
        } else {
            continue;
        }
        score = score * 2 + (id == 16 ? 1 : 0);
        if (score <= bestScore) continue;

        const uint8_t* s = t + strings + off;
        std::wstring name;
        if (utf16) {
            for (size_t k = 0; k + 1 < len; k += 2) name += static_cast<wchar_t>(be16(s + k));
        } else {
            for (size_t k = 0; k < len; ++k) name += s[k] < 0x80 ? static_cast<wchar_t>(s[k]) : L'?';
        }
        if (name.empty()) continue;
        best = std::move(name);
        bestScore = score;
    }
    return best;
}

void loadImpl(const std::wstring& registeredDir, const std::wstring& keysDir, int which, Font& out) {
    if (which < 0 || which >= static_cast<int>(sizeof(kWhich) / sizeof(kWhich[0]))) fail("BAD_ARG");
    const Which& w = kWhich[which];

    Content content;
    findContent(registeredDir, keysDir, w.title, kContentData, "NO_FONT_NCA", content);
    View romfs = contentRomfs(content);

    std::map<std::string, RomFile> files = listRomfs(romfs);
    const std::pair<const std::string, RomFile>* pick = nullptr;
    for (const auto& f : files)
        if (endsWith(f.first, ".bfttf") && f.first.compare(0, std::strlen(w.prefix), w.prefix) == 0) pick = &f;
    if (!pick) fail("NO_FONT");
    if (pick->second.size < 8 + 12 || pick->second.size > kMaxFontFile) fail("BAD_FONT");

    Bytes raw = romfs.read(pick->second.offset, static_cast<size_t>(pick->second.size));
    for (size_t i = 0; i < raw.size(); ++i) raw[i] ^= kXorKey[i & 3];
    if (std::memcmp(raw.data(), kMagic, 4) != 0) fail("BAD_FONT");
    if (be32(&raw[4]) != raw.size() - 8) fail("BAD_FONT");
    raw.erase(raw.begin(), raw.begin() + 8);
    if (!validSfnt(raw)) fail("BAD_FONT");

    out.family = familyName(raw);
    if (out.family.empty()) fail("BAD_FONT");
    out.source = pick->first;
    out.data = std::move(raw);
}

}  // namespace

bool Load(const std::wstring& registeredDir, const std::wstring& keysDir, int which, Font& out, std::string& err) {
    try {
        err.clear();
        Font f;
        loadImpl(registeredDir, keysDir, which, f);
        out = std::move(f);
        return true;
    } catch (const Fail& f) {
        try {
            err = f.code;
        } catch (...) {
        }
    } catch (...) {
        try {
            err = "INTERNAL";
        } catch (...) {
        }
    }
    return false;
}

}  // namespace fwfont
