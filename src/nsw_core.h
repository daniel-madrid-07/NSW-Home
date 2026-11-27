// Internal building blocks shared by nsw.cpp (game dumps) and fwsound.cpp (installed firmware):
// positional file views, key files, PFS0/HFS0 tables, NCA headers, section decryption, RomFS listing.
// Everything is defined in nsw.cpp. Not a public interface; UI code includes nsw.h / fwsound.h.
//
// Functions here report failure by throwing core::Fail (a short error code); the public entry
// points catch it and never let an exception out.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nsw {
namespace core {

using Bytes = std::vector<uint8_t>;

struct Fail {
    const char* code;
};
[[noreturn]] void fail(const char* code);

uint16_t rd16(const uint8_t* p);
uint32_t rd32(const uint8_t* p);
uint64_t rd64(const uint8_t* p);

// A View is a readable byte range; container entries, NCA sections and RomFS
// images are all Views layered on the file.
struct View {
    std::function<void(uint64_t, uint8_t*, size_t)> rd;
    uint64_t size = 0;

    void read(uint64_t off, uint8_t* dst, size_t len) const {
        if (off > size || len > size - off) fail("BAD_FORMAT");
        if (len) rd(off, dst, len);
    }
    Bytes read(uint64_t off, size_t len) const {
        if (off > size || len > size - off) fail("BAD_FORMAT");
        Bytes b(len);
        if (len) rd(off, b.data(), len);
        return b;
    }
    View sub(uint64_t base, uint64_t subSize) const {
        if (base > size || subSize > size - base) fail("BAD_FORMAT");
        View v;
        auto parent = rd;
        v.rd = [parent, base](uint64_t off, uint8_t* dst, size_t len) { parent(base + off, dst, len); };
        v.size = subSize;
        return v;
    }
};

// Opens a file read-only for positional reads; the handle lives as long as the View (and its copies).
// Returns false if the file cannot be opened.
bool openFileView(const std::wstring& path, View& out);

using KeyMap = std::map<std::string, Bytes>;

std::wstring joinPath(const std::wstring& dir, const wchar_t* name);

// Parses "name = hex" lines. Returns false if the file cannot be read.
bool loadKeyFile(const std::wstring& path, KeyMap& out);

struct Keys {
    std::wstring dir;
    KeyMap prod;
    KeyMap title;
    bool titleLoaded = false;

    const Bytes& get(const std::string& name, size_t length) const {
        auto it = prod.find(name);
        if (it == prod.end() || it->second.size() != length) fail("MISSING_KEY");
        return it->second;
    }

    // Encrypted title key from title.keys (optional file); empty if unknown.
    Bytes titleKey(const std::string& rightsHex) {
        if (!titleLoaded) {
            loadKeyFile(joinPath(dir, L"title.keys"), title);
            titleLoaded = true;
        }
        auto it = title.find(rightsHex);
        return it != title.end() && it->second.size() == 16 ? it->second : Bytes();
    }
};

struct Entry {
    std::string name;
    uint64_t offset = 0;  // relative to the view the table was parsed from
    uint64_t size = 0;
};

std::vector<Entry> parsePartition(const View& view, const char* expectMagic);

enum : uint8_t { kContentProgram = 0, kContentMeta = 1, kContentControl = 2, kContentData = 4 };

struct Section {
    uint64_t offset = 0;  // within the NCA
    uint64_t size = 0;
    uint8_t fsType = 0;    // 0 RomFS, 1 PartitionFS
    uint8_t hashType = 0;  // 2 HierarchicalSha256, 3 HierarchicalIntegrity (IVFC)
    uint8_t encType = 0;   // 1 none, 2 XTS, 3 CTR, 4 BKTR
    uint8_t ctrUpper[8] = {};
    uint8_t fsHeader[0x200] = {};
};

struct Nca {
    View view;
    uint8_t contentType = 0;
    uint8_t kakIndex = 0;  // 0 application, 1 ocean, 2 system
    uint64_t programId = 0;
    unsigned keyGeneration = 0;
    uint8_t rightsId[16] = {};
    bool hasRightsId = false;
    uint8_t keyArea[0x40] = {};
    std::vector<Section> sections;
};

// `file` is the container (or, for a loose NCA, the NCA file itself with one Entry covering it);
// `entries` is where tickets are looked up.
struct Context {
    Keys keys;
    View file;
    std::vector<Entry> entries;
};

// Reads and decrypts the 0xC00-byte header only.
Nca openNca(const Context& ctx, const Entry& e);

// Decrypted view over a whole section (offsets relative to the section start).
View sectionView(Context& ctx, const Nca& nca, const Section& sec);
// PFS0 image inside a PartitionFS section (past the hash table).
View pfs0View(Context& ctx, const Nca& nca, const Section& sec);
// RomFS image inside a RomFS section (last IVFC level).
View romfsView(Context& ctx, const Nca& nca, const Section& sec);

struct RomFile {
    uint64_t offset = 0;
    uint64_t size = 0;
};

// Lists every file by walking the file meta table linearly. Directory structure
// is ignored: the map is keyed by bare file name (a later duplicate name replaces an earlier one).
std::map<std::string, RomFile> listRomfs(const View& view);

// ---- installed firmware (a folder of loose NCAs), used by fwsound.cpp and fwfont.cpp

// One loose NCA of the installed firmware, opened and with its header parsed.
struct Content {
    Context ctx;
    Nca nca;
};

// Loads prod.keys from keysDir and finds the NCA with the given program ID and content type in
// registeredDir. Only the 0xC00-byte header of each file is read. Ryujinx stores every NCA as a
// folder "<id>.nca" holding a file "00"; a plain file "<id>.nca" is accepted as well.
// Fails with NO_KEYS, MISSING_KEY, NO_FIRMWARE, BAD_KEYS (no header decrypted at all) or `notFound`.
void findContent(const std::wstring& registeredDir, const std::wstring& keysDir, uint64_t titleId, uint8_t contentType,
                 const char* notFound, Content& out);

// RomFS image of the content's first RomFS section.
View contentRomfs(Content& c);

}  // namespace core
}  // namespace nsw
