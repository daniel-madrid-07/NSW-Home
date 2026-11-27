// The console's message archives from the installed firmware. See fwtext.h.
//
// Path through the data (verified on firmware 19.0.0):
//   registered\<id>.nca\00        program NCA of a system applet (0100000000001000 HOME Menu, ...)
//   -> RomFS section              message/<folder>/<name>.msbt.szs, found by full path (the same
//                                 file names repeat in every language folder, so the RomFS
//                                 directory tree is walked here instead of nsw's flat listing)
//   -> Yaz0                       "Yaz0", u32 BE decoded size, 8 reserved bytes, then groups of
//                                 eight items led by a flag byte (1 = literal, 0 = back-reference)
//   -> MsgStdBn (MSBT)            header of 0x20 bytes, then sections padded to 16 bytes:
//                                 LBL1 (label hash table: label -> string index), TXT2 (strings),
//                                 ATR1 and others ignored
//
// MSBT text is UTF-16 in the header's byte order. A tag is 0x0E, u16 group, u16 type, u16 size of
// its parameters, the parameters; 0x0F, u16 group, u16 type closes a tag that encloses text.
// Group 0 is the format's own (0 ruby, 1 font, 2 size, 3 colour, 4 page break): ruby keeps its
// base text, which follows the tag, and drops the reading; the rest only change the look.
// Every other group is the applet's own, a value put in at run time, kept as a token.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "fwtext.h"
#include "nsw_core.h"

#include <cstring>

namespace fwtext {
namespace {

using namespace nsw::core;

const uint64_t kMaxPacked = 0x400000;    // the largest archive (setting) is under 64 KB packed
const uint64_t kMaxUnpacked = 0x1000000; // and under 1 MB unpacked

struct Title {
    uint64_t id;
    bool required;
};
// The HOME Menu first: its files win over the copies other applets carry.
const Title kTitles[] = {
    {0x0100000000001000ull, true},  // qlaunch: HOME Menu, System Settings, options
    {0x0100000000001013ull, false}, // myPage: the user page, friends
    {0x010000000000100Cull, false}, // overlayDisp: the quick settings, notices over a game
    {0x0100000000001007ull, false}, // playerSelect: who is playing
    {0x010000000000100Dull, false}, // photoViewer: the album
    {0x0100000000001003ull, false}, // controller: the controllers applet
    {0x0100000000001005ull, false}, // error
    {0x0100000000001012ull, false}, // starter: the console's first start
};

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// Every file of a RomFS with its full path ("message/EUes/setting.msbt.szs").
std::map<std::string, RomFile> listPaths(const View& view) {
    Bytes h = view.read(0, 0x50);
    if (rd64(&h[0]) != 0x50) fail("BAD_NCA");
    uint64_t dirOff = rd64(&h[0x18]), dirSize = rd64(&h[0x20]);
    uint64_t fileOff = rd64(&h[0x38]), fileSize = rd64(&h[0x40]);
    uint64_t dataOff = rd64(&h[0x48]);
    if (dirSize > 0x400000 || fileSize > 0x400000) fail("BAD_NCA");
    Bytes dirs = view.read(dirOff, static_cast<size_t>(dirSize));
    Bytes files = view.read(fileOff, static_cast<size_t>(fileSize));

    std::map<uint32_t, std::string> dirPath;  // directory entry offset -> "a/b/" (root: "")
    std::function<std::string(uint32_t, int)> pathOf = [&](uint32_t off, int depth) -> std::string {
        if (off == 0) return std::string();
        auto it = dirPath.find(off);
        if (it != dirPath.end()) return it->second;
        if (depth > 64 || off > dirs.size() || dirs.size() - off < 0x18) fail("BAD_NCA");
        uint32_t parent = rd32(&dirs[off]);
        uint32_t nameSize = rd32(&dirs[off + 0x14]);
        if (nameSize > dirs.size() - off - 0x18) fail("BAD_NCA");
        std::string p = (parent == off ? std::string() : pathOf(parent, depth + 1)) +
                        std::string(reinterpret_cast<const char*>(&dirs[off + 0x18]), nameSize) + "/";
        return dirPath[off] = p;
    };

    std::map<std::string, RomFile> out;
    size_t p = 0;
    while (p + 0x20 <= files.size()) {
        uint32_t parent = rd32(&files[p]);
        size_t nameSize = rd32(&files[p + 0x1c]);
        if (nameSize > files.size() - p - 0x20) fail("BAD_NCA");
        RomFile f;
        f.offset = dataOff + rd64(&files[p + 0x08]);
        f.size = rd64(&files[p + 0x10]);
        if (f.offset < dataOff || f.offset > view.size || f.size > view.size - f.offset) fail("BAD_NCA");
        out[pathOf(parent, 0) + std::string(reinterpret_cast<const char*>(&files[p + 0x20]), nameSize)] = f;
        p += 0x20 + ((nameSize + 3) & ~size_t{3});
    }
    return out;
}

Bytes yaz0(const Bytes& in) {
    if (in.size() < 16 || std::memcmp(in.data(), "Yaz0", 4) != 0) fail("BAD_FORMAT");
    uint32_t size = be32(&in[4]);
    if (size > kMaxUnpacked) fail("BAD_FORMAT");
    Bytes out;
    out.reserve(size);
    size_t p = 16;
    while (out.size() < size) {
        if (p >= in.size()) fail("BAD_FORMAT");
        uint8_t flags = in[p++];
        for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
            if (flags & (1u << bit)) {
                if (p >= in.size()) fail("BAD_FORMAT");
                out.push_back(in[p++]);
                continue;
            }
            if (p + 2 > in.size()) fail("BAD_FORMAT");
            uint8_t b1 = in[p++], b2 = in[p++];
            size_t dist = (static_cast<size_t>(b1 & 0x0f) << 8 | b2) + 1;
            size_t n = b1 >> 4;
            if (n == 0) {
                if (p >= in.size()) fail("BAD_FORMAT");
                n = static_cast<size_t>(in[p++]) + 0x12;
            } else {
                n += 2;
            }
            if (dist > out.size()) fail("BAD_FORMAT");
            size_t from = out.size() - dist;
            for (size_t k = 0; k < n && out.size() < size; ++k) out.push_back(out[from + k]);
        }
    }
    return out;
}

const wchar_t kHex[] = L"0123456789abcdef";

// One MSBT file into `out` as "<prefix><label>" -> text (existing keys are kept).
void parseMsbt(const Bytes& f, const std::string& prefix, std::map<std::string, std::wstring>& out) {
    if (f.size() < 0x20 || std::memcmp(f.data(), "MsgStdBn", 8) != 0) fail("BAD_FORMAT");
    const bool big = f[8] == 0xfe && f[9] == 0xff;
    auto u16 = [&](size_t o) -> uint32_t {
        if (o + 2 > f.size()) fail("BAD_FORMAT");
        return big ? (f[o] << 8 | f[o + 1]) : (f[o] | f[o + 1] << 8);
    };
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > f.size()) fail("BAD_FORMAT");
        return big ? be32(&f[o]) : rd32(&f[o]);
    };
    if (f[0x0c] != 1) fail("UNSUPPORTED");  // UTF-16; the console's archives are all of this kind
    const uint32_t sections = u16(0x0e);

    size_t lbl = 0, lblSize = 0, txt = 0, txtSize = 0;
    size_t p = 0x20;
    for (uint32_t s = 0; s < sections && p + 0x10 <= f.size(); ++s) {
        uint32_t size = u32(p + 4);
        if (size > f.size() - p - 0x10) fail("BAD_FORMAT");
        if (!std::memcmp(&f[p], "LBL1", 4)) lbl = p + 0x10, lblSize = size;
        if (!std::memcmp(&f[p], "TXT2", 4)) txt = p + 0x10, txtSize = size;
        p += 0x10 + ((static_cast<size_t>(size) + 15) & ~size_t{15});
    }
    if (!lbl || !txt || lblSize < 4 || txtSize < 4) fail("BAD_FORMAT");

    const uint32_t count = u32(txt);
    if (count > (txtSize - 4) / 4) fail("BAD_FORMAT");
    auto text = [&](uint32_t index) -> std::wstring {
        if (index >= count) fail("BAD_FORMAT");
        size_t begin = txt + u32(txt + 4 + index * 4);
        size_t end = index + 1 < count ? txt + u32(txt + 4 + (index + 1) * 4) : txt + txtSize;
        if (begin > end || end > txt + txtSize) fail("BAD_FORMAT");
        std::wstring s;
        for (size_t q = begin; q + 2 <= end;) {
            uint32_t c = u16(q);
            q += 2;
            if (c == 0) break;
            if (c == 0x0e) {
                if (q + 6 > end) fail("BAD_FORMAT");
                uint32_t group = u16(q), type = u16(q + 2), size = u16(q + 4);
                q += 6;
                if (size > end - q) fail("BAD_FORMAT");
                if (group != 0) {
                    s += wchar_t(0xE000);
                    s += kHex[group & 15];
                    s += kHex[type & 15];
                    for (size_t k = 0; k < size; ++k) {
                        s += kHex[f[q + k] >> 4];
                        s += kHex[f[q + k] & 15];
                    }
                    s += wchar_t(0xE001);
                } else if (type == 4) {
                    s += L'\n';
                }
                q += size;
                continue;
            }
            if (c == 0x0f) {  // end of an enclosing tag: group, type
                q += 4;
                continue;
            }
            s += static_cast<wchar_t>(c);
        }
        return s;
    };

    const uint32_t groups = u32(lbl);
    if (groups > (lblSize - 4) / 8) fail("BAD_FORMAT");
    for (uint32_t g = 0; g < groups; ++g) {
        uint32_t n = u32(lbl + 4 + g * 8);
        size_t q = lbl + u32(lbl + 8 + g * 8);
        for (uint32_t k = 0; k < n; ++k) {
            if (q >= lbl + lblSize) fail("BAD_FORMAT");
            size_t len = f[q];
            if (q + 1 + len + 4 > lbl + lblSize) fail("BAD_FORMAT");
            std::string label(reinterpret_cast<const char*>(&f[q + 1]), len);
            uint32_t index = u32(q + 1 + len);
            q += 1 + len + 4;
            std::string key = prefix + label;
            if (!out.count(key)) out[key] = text(index);
        }
    }
}

struct Opened {
    Content content;
    View romfs;
    std::map<std::string, RomFile> files;
};

// Reads every message/<folder>/*.msbt.szs of one applet.
void readFolder(Opened& app, const std::string& folder, std::map<std::string, std::wstring>& out) {
    const std::string dir = "message/" + folder + "/";
    const std::string ext = ".msbt.szs";
    for (const auto& f : app.files) {
        const std::string& path = f.first;
        if (path.compare(0, dir.size(), dir) != 0 || path.size() <= dir.size() + ext.size() ||
            path.compare(path.size() - ext.size(), ext.size(), ext) != 0)
            continue;
        if (f.second.size > kMaxPacked) fail("BAD_FORMAT");
        std::string name = path.substr(dir.size(), path.size() - dir.size() - ext.size());
        if (name.find('/') != std::string::npos) continue;
        Bytes packed = app.romfs.read(f.second.offset, static_cast<size_t>(f.second.size));
        parseMsbt(yaz0(packed), name + "/", out);
    }
}

bool hasFolder(const Opened& app, const std::string& folder) {
    const std::string dir = "message/" + folder + "/";
    auto it = app.files.lower_bound(dir);
    return it != app.files.end() && it->first.compare(0, dir.size(), dir) == 0;
}

void loadImpl(const std::wstring& registeredDir, const std::wstring& keysDir, const std::vector<std::string>& folders,
              std::vector<std::map<std::string, std::wstring>>& out) {
    out.assign(folders.size(), {});
    std::vector<Opened> apps;
    for (const Title& t : kTitles) {
        Opened app;
        try {
            findContent(registeredDir, keysDir, t.id, kContentProgram, "NO_MENU_NCA", app.content);
            app.romfs = contentRomfs(app.content);
            app.files = listPaths(app.romfs);
        } catch (const Fail& f) {
            if (t.required || std::strcmp(f.code, "NO_MENU_NCA") != 0) throw;
            continue;
        }
        apps.push_back(std::move(app));
    }
    bool any = false;
    for (size_t i = 0; i < folders.size(); ++i) {
        if (!hasFolder(apps[0], folders[i])) continue;
        any = true;
        for (Opened& app : apps) {
            try {
                readFolder(app, folders[i], out[i]);
            } catch (const Fail& f) {
                // A broken archive of a secondary applet does not take the HOME Menu's down.
                if (&app == &apps[0] || !std::strcmp(f.code, "CRYPTO")) throw;
            }
        }
    }
    if (!any) fail("NO_LANGUAGE");
}

bool run(const std::wstring& registeredDir, const std::wstring& keysDir, const std::vector<std::string>& folders,
         std::vector<std::map<std::string, std::wstring>>& out, std::string& err) {
    try {
        err.clear();
        std::vector<std::map<std::string, std::wstring>> got;
        loadImpl(registeredDir, keysDir, folders, got);
        out = std::move(got);
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

}  // namespace

bool Load(const std::wstring& registeredDir, const std::wstring& keysDir, const std::string& folder,
          std::map<std::string, std::wstring>& out, std::string& err) {
    std::vector<std::map<std::string, std::wstring>> got;
    if (!run(registeredDir, keysDir, {folder}, got, err)) return false;
    out = std::move(got[0]);
    return true;
}

bool LoadMany(const std::wstring& registeredDir, const std::wstring& keysDir, const std::vector<std::string>& folders,
              std::vector<std::map<std::string, std::wstring>>& out, std::string& err) {
    return run(registeredDir, keysDir, folders, out, err);
}

std::wstring Plain(const std::wstring& text, const std::wstring& with) {
    std::wstring s;
    bool in = false;
    for (wchar_t c : text) {
        if (c == 0xE000) {
            in = true;
            s += with;
        } else if (c == 0xE001) {
            in = false;
        } else if (!in) {
            s += c;
        }
    }
    return s;
}

}  // namespace fwtext
