// Nintendo Switch dump metadata reader. See nsw.h.
// C++17, Windows only. AES-ECB comes from CNG (bcrypt); XTS and CTR are built on it here.
// Reference implementation (same logic, Node.js): tools/nsw-reference/.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "nsw.h"
#include "nsw_core.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>

namespace nsw {
// Shared with fwsound.cpp; the types and the functions meant for use outside this file are declared in nsw_core.h.
namespace core {

// Internal failure; converted to the `err` code at the API boundary.
[[noreturn]] void fail(const char* code) { throw Fail{code}; }

// ---------------------------------------------------------------- byte helpers

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }

void wr64be(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v & 0xff);
        v >>= 8;
    }
}

uint64_t addChecked(uint64_t a, uint64_t b, const char* code) {
    if (a > UINT64_MAX - b) fail(code);
    return a + b;
}

// Fails unless [off, off + len) lies inside a buffer of `size` bytes.
void need(size_t size, uint64_t off, uint64_t len, const char* code) {
    if (off > size || len > size - off) fail(code);
}

std::string cString(const Bytes& b, size_t start, size_t maxLen) {
    size_t end = std::min(b.size(), start + maxLen);
    size_t i = start;
    while (i < end && b[i] != 0) ++i;
    return i > start ? std::string(reinterpret_cast<const char*>(b.data()) + start, i - start) : std::string();
}

std::string toHex(const uint8_t* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = digits[p[i] >> 4];
        s[i * 2 + 1] = digits[p[i] & 15];
    }
    return s;
}

std::string hex64(uint64_t v) {
    uint8_t be[8];
    wr64be(be, v);
    return toHex(be, 8);
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool isSpace(wchar_t c) {
    return c == L' ' || (c >= 0x09 && c <= 0x0d) || c == 0x00a0 || c == 0x3000 || c == 0xfeff ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x1680;
}

// UTF-8 -> UTF-16, trimmed. Invalid sequences become U+FFFD.
std::wstring toWideTrimmed(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    size_t a = 0, b = w.size();
    while (a < b && isSpace(w[a])) ++a;
    while (b > a && isSpace(w[b - 1])) --b;
    return w.substr(a, b - a);
}

// ---------------------------------------------------------------- file access

class File {
public:
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }

    bool open(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h_, &sz)) return false;
        size_ = static_cast<uint64_t>(sz.QuadPart);
        return true;
    }

    uint64_t size() const { return size_; }

    // Positional read; does not use or move the file pointer.
    void read(uint64_t offset, uint8_t* dst, size_t len) const {
        while (len > 0) {
            OVERLAPPED ov = {};
            ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD want = static_cast<DWORD>(std::min<size_t>(len, 0x40000000u));
            DWORD got = 0;
            if (!ReadFile(h_, dst, want, &got, &ov) || got == 0) fail("FILE_ERROR");
            offset += got;
            dst += got;
            len -= got;
        }
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

bool openFileView(const std::wstring& path, View& out) {
    auto file = std::make_shared<File>();
    if (!file->open(path)) return false;
    out.size = file->size();
    out.rd = [file](uint64_t off, uint8_t* dst, size_t len) { file->read(off, dst, len); };
    return true;
}

// ---------------------------------------------------------------- AES

BCRYPT_ALG_HANDLE aesProvider() {
    // Opened once per process (thread-safe static init) and kept for its lifetime.
    static const BCRYPT_ALG_HANDLE handle = []() -> BCRYPT_ALG_HANDLE {
        BCRYPT_ALG_HANDLE h = nullptr;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, BCRYPT_AES_ALGORITHM, nullptr, 0))) return nullptr;
        wchar_t mode[] = BCRYPT_CHAIN_MODE_ECB;
        if (!BCRYPT_SUCCESS(BCryptSetProperty(h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode), sizeof(mode), 0))) {
            BCryptCloseAlgorithmProvider(h, 0);
            return nullptr;
        }
        return h;
    }();
    return handle;
}

// AES-128-ECB, no padding.
class Aes {
public:
    explicit Aes(const uint8_t* key16) {
        BCRYPT_ALG_HANDLE alg = aesProvider();
        if (!alg) fail("CRYPTO");
        uint8_t k[16];
        std::memcpy(k, key16, 16);
        NTSTATUS st = BCryptGenerateSymmetricKey(alg, &key_, nullptr, 0, k, 16, 0);
        SecureZeroMemory(k, sizeof(k));
        if (!BCRYPT_SUCCESS(st)) fail("CRYPTO");
    }
    Aes(const Aes&) = delete;
    Aes& operator=(const Aes&) = delete;
    ~Aes() {
        if (key_) BCryptDestroyKey(key_);
    }

    // In place; len must be a multiple of 16.
    void crypt(uint8_t* data, size_t len, bool decrypt) const {
        if (len == 0) return;
        if (len % 16 != 0 || len > 0x7fffffffu) fail("CRYPTO");
        ULONG n = static_cast<ULONG>(len);
        ULONG done = 0;
        NTSTATUS st = decrypt ? BCryptDecrypt(key_, data, n, nullptr, nullptr, 0, data, n, &done, 0)
                              : BCryptEncrypt(key_, data, n, nullptr, nullptr, 0, data, n, &done, 0);
        if (!BCRYPT_SUCCESS(st) || done != n) fail("CRYPTO");
    }

private:
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

const size_t kSector = 0x200;

// AES-128-XTS decrypt with Nintendo's tweak: the sector number as a big-endian
// 128-bit value. len must be a multiple of 0x200.
void xtsDecrypt(uint8_t* data, size_t len, const uint8_t* key32, uint64_t firstSector) {
    Aes dataKey(key32);
    Aes tweakKey(key32 + 16);
    Bytes tweaks(len);
    for (size_t s = 0; s * kSector < len; ++s) {
        uint8_t t[16] = {};
        wr64be(t + 8, firstSector + s);
        tweakKey.crypt(t, 16, false);
        for (size_t b = 0; b < kSector; b += 16) {
            std::memcpy(&tweaks[s * kSector + b], t, 16);
            // multiply the tweak by x in GF(2^128), little-endian
            uint8_t carry = 0;
            for (int i = 0; i < 16; ++i) {
                uint8_t next = static_cast<uint8_t>(t[i] >> 7);
                t[i] = static_cast<uint8_t>((t[i] << 1) | carry);
                carry = next;
            }
            if (carry) t[0] ^= 0x87;
        }
    }
    for (size_t i = 0; i < len; ++i) data[i] ^= tweaks[i];
    dataKey.crypt(data, len, true);
    for (size_t i = 0; i < len; ++i) data[i] ^= tweaks[i];
}

// ---------------------------------------------------------------- keys

std::wstring joinPath(const std::wstring& dir, const wchar_t* name) {
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d + L"\\" + name;
}

// Parses "name = hex" lines. Returns false if the file cannot be read.
bool loadKeyFile(const std::wstring& path, KeyMap& out) {
    File f;
    if (!f.open(path) || f.size() > 0x1000000) return false;
    Bytes text(static_cast<size_t>(f.size()));
    if (!text.empty()) f.read(0, text.data(), text.size());

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = pos;
        while (eol < text.size() && text[eol] != '\n') ++eol;
        std::string line(reinterpret_cast<const char*>(text.data()) + pos, eol - pos);
        pos = eol + 1;

        size_t sep = line.find_first_of("=,");
        if (sep == std::string::npos) continue;
        auto strip = [](const std::string& s) {
            size_t a = s.find_first_not_of(" \t\r");
            size_t b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        std::string name = lowerAscii(strip(line.substr(0, sep)));
        std::string hex = strip(line.substr(sep + 1));
        if (name.empty() || hex.empty() || hex.size() % 2 != 0) continue;
        Bytes value(hex.size() / 2);
        bool ok = true;
        for (size_t i = 0; i < value.size() && ok; ++i) {
            int hi = hexVal(hex[i * 2]), lo = hexVal(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) ok = false;
            else value[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        if (ok) out[name] = std::move(value);
    }
    return true;
}

// ---------------------------------------------------------------- PFS0 / HFS0 / XCI

bool magicIs(const uint8_t* p, const char* magic) { return std::memcmp(p, magic, 4) == 0; }

std::vector<Entry> parsePartition(const View& view, const char* expectMagic) {
    Bytes head = view.read(0, 0x10);
    bool pfs = magicIs(head.data(), "PFS0");
    if (!pfs && !magicIs(head.data(), "HFS0")) fail("BAD_FORMAT");
    if (!magicIs(head.data(), expectMagic)) fail("BAD_FORMAT");
    uint32_t count = rd32(&head[4]);
    uint32_t stringSize = rd32(&head[8]);
    if (count > 0x4000 || stringSize > 0x400000) fail("BAD_FORMAT");

    size_t entrySize = pfs ? 0x18 : 0x40;
    size_t tableSize = count * entrySize;
    Bytes rest = view.read(0x10, tableSize + stringSize);
    uint64_t dataStart = 0x10 + tableSize + stringSize;

    std::vector<Entry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = &rest[i * entrySize];
        uint32_t nameOffset = rd32(e + 0x10);
        if (nameOffset >= stringSize) fail("BAD_FORMAT");
        Entry en;
        en.name = cString(rest, tableSize + nameOffset, stringSize - nameOffset);
        en.offset = addChecked(dataStart, rd64(e), "BAD_FORMAT");
        en.size = rd64(e + 8);
        entries.push_back(std::move(en));
    }
    return entries;
}

// Returns the files that hold the NCAs, with offsets absolute in the dump.
// Detection is by magic, not by extension.
std::vector<Entry> openContainer(const View& file) {
    if (file.size < 0x10) fail("BAD_FORMAT");
    Bytes magic = file.read(0, 4);
    if (magicIs(magic.data(), "PFS0")) return parsePartition(file, "PFS0");

    // XCI: "HEAD" at 0x100. Full dumps carry a 0x1000 key area in front of the card image.
    uint64_t base = UINT64_MAX;
    for (uint64_t b : {uint64_t{0}, uint64_t{0x1000}}) {
        if (file.size >= b + 0x200 && magicIs(file.read(b + 0x100, 4).data(), "HEAD")) {
            base = b;
            break;
        }
    }
    if (base == UINT64_MAX) fail("BAD_FORMAT");

    View card = file.sub(base, file.size - base);
    Bytes header = card.read(0, 0x200);
    uint64_t rootOffset = rd64(&header[0x130]);
    if (rootOffset > card.size) fail("BAD_FORMAT");
    std::vector<Entry> root = parsePartition(card.sub(rootOffset, card.size - rootOffset), "HFS0");
    for (const Entry& e : root) {
        if (e.name != "secure") continue;
        uint64_t secureBase = addChecked(rootOffset, e.offset, "BAD_FORMAT");
        if (secureBase > card.size) fail("BAD_FORMAT");
        std::vector<Entry> files = parsePartition(card.sub(secureBase, card.size - secureBase), "HFS0");
        for (Entry& f : files) f.offset = addChecked(addChecked(base, secureBase, "BAD_FORMAT"), f.offset, "BAD_FORMAT");
        return files;
    }
    fail("BAD_FORMAT");
}

// ---------------------------------------------------------------- NCA

Nca openNca(const Context& ctx, const Entry& e) {
    if (e.size < 0xc00) fail("BAD_NCA");
    const Bytes& headerKey = ctx.keys.get("header_key", 32);
    Nca nca;
    nca.view = ctx.file.sub(e.offset, e.size);
    Bytes h = nca.view.read(0, 0xc00);
    xtsDecrypt(h.data(), 0x400, headerKey.data(), 0);
    if (magicIs(&h[0x200], "NCA3")) {
        xtsDecrypt(&h[0x400], 0x800, headerKey.data(), 2);
    } else if (magicIs(&h[0x200], "NCA2")) {
        // NCA2: every fs header is its own sector 0
        for (size_t i = 0; i < 4; ++i) xtsDecrypt(&h[0x400 + i * kSector], kSector, headerKey.data(), 0);
    } else {
        fail("BAD_NCA");  // wrong header_key, or not an NCA2/NCA3
    }

    nca.contentType = h[0x205];
    nca.kakIndex = h[0x207];
    nca.programId = rd64(&h[0x210]);
    unsigned gen = std::max<unsigned>(h[0x206], h[0x220]);
    nca.keyGeneration = gen > 0 ? gen - 1 : 0;
    std::memcpy(nca.rightsId, &h[0x230], 16);
    nca.hasRightsId = std::any_of(nca.rightsId, nca.rightsId + 16, [](uint8_t b) { return b != 0; });
    std::memcpy(nca.keyArea, &h[0x300], 0x40);

    for (size_t i = 0; i < 4; ++i) {
        uint64_t start = static_cast<uint64_t>(rd32(&h[0x240 + i * 0x10])) * kSector;
        uint64_t end = static_cast<uint64_t>(rd32(&h[0x244 + i * 0x10])) * kSector;
        if (end <= start) continue;
        if (end > e.size) fail("BAD_NCA");
        const uint8_t* fh = &h[0x400 + i * kSector];
        Section s;
        s.offset = start;
        s.size = end - start;
        s.fsType = fh[2];
        s.hashType = fh[3];
        s.encType = fh[4];
        for (int j = 0; j < 8; ++j) s.ctrUpper[j] = fh[0x140 + 7 - j];
        std::memcpy(s.fsHeader, fh, kSector);
        nca.sections.push_back(s);
    }
    return nca;
}

// Encrypted title key from a common ticket (.tik) inside the container; empty if none.
Bytes findTicketKey(const Context& ctx, const std::string& rightsHex) {
    std::vector<const Entry*> tiks;
    for (const Entry& e : ctx.entries)
        if (endsWith(lowerAscii(e.name), ".tik")) tiks.push_back(&e);
    // the ticket is normally named <rightsId>.tik; try that one first
    std::stable_sort(tiks.begin(), tiks.end(), [&](const Entry* a, const Entry* b) {
        return startsWith(lowerAscii(a->name), rightsHex) && !startsWith(lowerAscii(b->name), rightsHex);
    });
    for (const Entry* e : tiks) {
        if (e->size < 0x2c0 || e->size > 0x10000) continue;
        Bytes tik = ctx.file.read(e->offset, static_cast<size_t>(std::min<uint64_t>(e->size, 0x400)));
        size_t sigSize;
        switch (rd32(tik.data())) {
            case 0x10000: case 0x10003: sigSize = 0x23c; break;  // RSA-4096
            case 0x10001: case 0x10004: sigSize = 0x13c; break;  // RSA-2048
            case 0x10002: case 0x10005: sigSize = 0x7c; break;   // ECDSA
            default: continue;
        }
        size_t body = 4 + sigSize;  // 0x140 for the usual RSA-2048 ticket -> title key at 0x180
        if (body + 0x170 > tik.size()) continue;
        if (toHex(&tik[body + 0x160], 16) != rightsHex) continue;
        if (tik[body + 0x141] != 0) continue;  // personalized ticket: key is RSA-wrapped, not usable here
        return Bytes(tik.begin() + static_cast<ptrdiff_t>(body + 0x40), tik.begin() + static_cast<ptrdiff_t>(body + 0x50));
    }
    return Bytes();
}

std::string keyName(const char* prefix, unsigned generation) {
    uint8_t g = static_cast<uint8_t>(generation);
    return std::string(prefix) + toHex(&g, 1);
}

void ncaCtrKey(Context& ctx, const Nca& nca, uint8_t out[16]) {
    if (nca.hasRightsId) {
        std::string rightsHex = toHex(nca.rightsId, 16);
        Bytes enc = findTicketKey(ctx, rightsHex);
        if (enc.empty()) enc = ctx.keys.titleKey(rightsHex);
        if (enc.size() != 16) fail("NO_TITLE_KEY");
        Aes kek(ctx.keys.get(keyName("titlekek_", nca.keyGeneration), 16).data());
        kek.crypt(enc.data(), 16, true);
        std::memcpy(out, enc.data(), 16);
    } else {
        static const char* const kinds[] = {"key_area_key_application_", "key_area_key_ocean_", "key_area_key_system_"};
        if (nca.kakIndex > 2) fail("BAD_NCA");
        Aes kak(ctx.keys.get(keyName(kinds[nca.kakIndex], nca.keyGeneration), 16).data());
        uint8_t area[0x40];
        std::memcpy(area, nca.keyArea, sizeof(area));
        kak.crypt(area, sizeof(area), true);
        std::memcpy(out, area + 0x20, 16);
    }
}

// Decrypted view over a whole section (offsets relative to the section start).
View sectionView(Context& ctx, const Nca& nca, const Section& sec) {
    View raw = nca.view.sub(sec.offset, sec.size);
    if (sec.encType == 1) return raw;
    if (sec.encType != 3) fail("UNSUPPORTED");

    uint8_t key[16];
    ncaCtrKey(ctx, nca, key);
    auto aes = std::make_shared<Aes>(key);
    SecureZeroMemory(key, sizeof(key));

    uint64_t sectionOffset = sec.offset;
    uint64_t sectionSize = sec.size;
    uint64_t upper = rd64(sec.ctrUpper);  // kept as raw bytes, copied back verbatim below

    View v;
    v.size = sec.size;
    v.rd = [raw, aes, sectionOffset, sectionSize, upper](uint64_t off, uint8_t* dst, size_t len) {
        // AES-CTR: counter = section ctr (upper 8 bytes) | big-endian (absolute NCA offset >> 4)
        uint64_t start = off & ~uint64_t{15};
        uint64_t end = std::min(sectionSize, (off + len + 15) & ~uint64_t{15});
        Bytes data = raw.read(start, static_cast<size_t>(end - start));
        Bytes stream((data.size() + 15) & ~size_t{15});
        uint8_t ctr[16];
        std::memcpy(ctr, &upper, 8);
        wr64be(ctr + 8, (sectionOffset + start) >> 4);
        for (size_t i = 0; i < stream.size(); i += 16) {
            std::memcpy(&stream[i], ctr, 16);
            for (int j = 15; j >= 0 && ++ctr[j] == 0; --j) {
            }
        }
        aes->crypt(stream.data(), stream.size(), false);
        size_t skip = static_cast<size_t>(off - start);
        if (skip + len > data.size()) fail("BAD_NCA");
        for (size_t i = 0; i < len; ++i) dst[i] = static_cast<uint8_t>(data[skip + i] ^ stream[skip + i]);
    };
    return v;
}

// PFS0 image inside a PartitionFS section (past the hash table).
View pfs0View(Context& ctx, const Nca& nca, const Section& sec) {
    if (sec.hashType != 2) fail("BAD_NCA");
    uint64_t offset = rd64(sec.fsHeader + 0x40);
    uint64_t size = rd64(sec.fsHeader + 0x48);
    if (offset > sec.size || size > sec.size - offset) fail("BAD_NCA");
    return sectionView(ctx, nca, sec).sub(offset, size);
}

// RomFS image inside a RomFS section (last IVFC level).
View romfsView(Context& ctx, const Nca& nca, const Section& sec) {
    const uint8_t* h = sec.fsHeader;
    if (sec.hashType != 3 || !magicIs(h + 0x08, "IVFC")) fail("BAD_NCA");
    uint32_t levels = rd32(h + 0x14);  // includes the master hash level
    size_t last = levels < 2 ? 0 : std::min<size_t>(levels - 2, 5);
    uint64_t offset = rd64(h + 0x18 + last * 0x18);
    uint64_t size = rd64(h + 0x20 + last * 0x18);
    if (offset > sec.size || size > sec.size - offset) fail("BAD_NCA");
    return sectionView(ctx, nca, sec).sub(offset, size);
}

// ---------------------------------------------------------------- RomFS

// Lists every file by walking the file meta table linearly. Directory structure
// is ignored (the control RomFS is flat).
std::map<std::string, RomFile> listRomfs(const View& view) {
    Bytes h = view.read(0, 0x50);
    if (rd64(&h[0]) != 0x50) fail("BAD_NCA");  // wrong key, or not a RomFS
    uint64_t metaOffset = rd64(&h[0x38]);
    uint64_t metaSize = rd64(&h[0x40]);
    uint64_t dataOffset = rd64(&h[0x48]);
    if (metaSize > 0x400000 || metaOffset > view.size || metaSize > view.size - metaOffset) fail("BAD_NCA");

    Bytes meta = view.read(metaOffset, static_cast<size_t>(metaSize));
    std::map<std::string, RomFile> files;
    size_t p = 0;
    while (p + 0x20 <= meta.size()) {
        size_t nameSize = rd32(&meta[p + 0x1c]);
        if (nameSize > meta.size() - p - 0x20) fail("BAD_NCA");
        RomFile f;
        f.offset = addChecked(dataOffset, rd64(&meta[p + 0x08]), "BAD_NCA");
        f.size = rd64(&meta[p + 0x10]);
        if (f.offset > view.size || f.size > view.size - f.offset) fail("BAD_NCA");
        files[cString(meta, p + 0x20, nameSize)] = f;
        p += 0x20 + ((nameSize + 3) & ~size_t{3});
    }
    return files;
}

// ---------------------------------------------------------------- installed firmware

void findContent(const std::wstring& registeredDir, const std::wstring& keysDir, uint64_t titleId, uint8_t contentType,
                 const char* notFound, Content& out) {
    out.ctx.keys.dir = keysDir;
    if (keysDir.empty() || !loadKeyFile(joinPath(keysDir, L"prod.keys"), out.ctx.keys.prod) || out.ctx.keys.prod.empty())
        fail("NO_KEYS");
    out.ctx.keys.get("header_key", 32);

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(joinPath(registeredDir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) fail("NO_FIRMWARE");
    std::vector<std::wstring> paths;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring path = joinPath(registeredDir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) path = joinPath(path, L"00");
        paths.push_back(std::move(path));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (paths.empty()) fail("NO_FIRMWARE");

    size_t headersRead = 0;
    for (const std::wstring& path : paths) {
        View file;
        if (!openFileView(path, file)) continue;
        out.ctx.file = file;
        Entry whole;
        whole.size = file.size;
        try {
            Nca nca = openNca(out.ctx, whole);
            ++headersRead;
            if (nca.programId == titleId && nca.contentType == contentType) {
                out.nca = std::move(nca);
                return;
            }
        } catch (const Fail& f) {
            // not an NCA, truncated, unreadable: skip it. Anything else is a real failure.
            if (std::strcmp(f.code, "BAD_NCA") != 0 && std::strcmp(f.code, "FILE_ERROR") != 0 &&
                std::strcmp(f.code, "BAD_FORMAT") != 0)
                throw;
        }
    }
    fail(headersRead == 0 ? "BAD_KEYS" : notFound);
}

View contentRomfs(Content& c) {
    for (const Section& s : c.nca.sections)
        if (s.fsType == 0) return romfsView(c.ctx, c.nca, s);
    fail("BAD_NCA");
}

}  // namespace core

using namespace core;

namespace {

// ---------------------------------------------------------------- CNMT

enum : uint8_t { kMetaApplication = 0x80, kMetaPatch = 0x81 };

struct Cnmt {
    uint64_t titleId = 0;
    uint8_t type = 0;
    uint32_t version = 0;
    std::string controlId;  // NCA ID (32 hex chars) of the control NCA, may be empty
    int rank() const { return type == kMetaApplication ? 0 : type == kMetaPatch ? 1 : 2; }
};

bool readCnmt(Context& ctx, const Nca& nca, Cnmt& out) {
    if (nca.sections.empty()) return false;
    const Section* sec = &nca.sections[0];
    for (const Section& s : nca.sections)
        if (s.fsType == 1) {
            sec = &s;
            break;
        }
    View view = pfs0View(ctx, nca, *sec);
    for (const Entry& f : parsePartition(view, "PFS0")) {
        if (!endsWith(lowerAscii(f.name), ".cnmt")) continue;
        if (f.size < 0x20 || f.size > 0x100000) return false;
        Bytes c = view.read(f.offset, static_cast<size_t>(f.size));

        out.type = c[0x0c];
        size_t extSize = rd16(&c[0x0e]);
        size_t count = rd16(&c[0x10]);
        out.version = rd32(&c[8]);
        out.titleId = rd64(&c[0]);
        // a patch's own ID is the update ID; its extended header names the application
        if (out.type == kMetaPatch && extSize >= 8) {
            need(c.size(), 0x20, 8, "BAD_NCA");
            out.titleId = rd64(&c[0x20]);
        }
        for (size_t i = 0; i < count; ++i) {
            size_t r = 0x20 + extSize + i * 0x38;
            if (r + 0x38 > c.size()) break;
            if (c[r + 0x36] == 3) {  // content type Control
                out.controlId = toHex(&c[r + 0x20], 16);
                break;
            }
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- NACP

const char* const kLanguages[16] = {
    "AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish",
    "Spanish", "CanadianFrench", "Italian", "Dutch", "Portuguese", "Russian", "Korean",
    "TraditionalChinese", "SimplifiedChinese", "BrazilianPortuguese",
};

int pickLanguage(const std::vector<int>& preferred, const std::function<bool(int)>& available) {
    for (int i : preferred)
        if (i >= 0 && i < 16 && available(i)) return i;
    for (int i = 0; i < 16; ++i)
        if (available(i)) return i;
    return -1;
}

void readControl(Context& ctx, const Nca& nca, const std::vector<int>& preferred, Title& out) {
    if (nca.sections.empty()) fail("BAD_NCA");
    const Section* sec = &nca.sections[0];
    for (const Section& s : nca.sections)
        if (s.fsType == 0) {
            sec = &s;
            break;
        }
    View view = romfsView(ctx, nca, *sec);
    std::map<std::string, RomFile> files = listRomfs(view);

    auto nacpFile = files.find("control.nacp");
    if (nacpFile == files.end() || nacpFile->second.size < 0x3070) fail("NO_NACP");
    Bytes nacp = view.read(nacpFile->second.offset, static_cast<size_t>(std::min<uint64_t>(nacpFile->second.size, 0x4000)));

    auto name = [&](int i) { return toWideTrimmed(cString(nacp, static_cast<size_t>(i) * 0x300, 0x200)); };
    auto publisher = [&](int i) { return toWideTrimmed(cString(nacp, static_cast<size_t>(i) * 0x300 + 0x200, 0x100)); };
    int titleLang = pickLanguage(preferred, [&](int i) { return !name(i).empty(); });
    if (titleLang >= 0) {
        out.name = name(titleLang);
        out.publisher = publisher(titleLang);
    }
    out.version = toWideTrimmed(cString(nacp, 0x3060, 0x10));

    auto icon = [&](int i) -> const RomFile* {
        auto it = files.find(std::string("icon_") + kLanguages[i] + ".dat");
        return it != files.end() && it->second.size > 0 && it->second.size <= 0x800000 ? &it->second : nullptr;
    };
    int iconLang = pickLanguage(preferred, [&](int i) { return icon(i) != nullptr; });
    if (iconLang >= 0) {
        const RomFile* f = icon(iconLang);
        out.icon = view.read(f->offset, static_cast<size_t>(f->size));
    }
}

// ---------------------------------------------------------------- top level

bool isKeyError(const Fail& f) { return !std::strcmp(f.code, "MISSING_KEY") || !std::strcmp(f.code, "NO_KEYS"); }

void readTitleImpl(const std::wstring& path, const std::wstring& keysDir, const std::vector<int>& languages, Title& out) {
    Context ctx;
    ctx.keys.dir = keysDir;
    if (keysDir.empty() || !loadKeyFile(joinPath(keysDir, L"prod.keys"), ctx.keys.prod) || ctx.keys.prod.empty())
        fail("NO_KEYS");

    if (!openFileView(path, ctx.file)) fail("FILE_ERROR");
    ctx.entries = openContainer(ctx.file);
    for (const Entry& e : ctx.entries)
        if (e.offset > ctx.file.size || e.size > ctx.file.size - e.offset) fail("BAD_FORMAT");

    std::vector<const Entry*> ncas, metaNcas;
    for (const Entry& e : ctx.entries) {
        std::string n = lowerAscii(e.name);
        if (!endsWith(n, ".nca")) continue;
        ncas.push_back(&e);
        if (endsWith(n, ".cnmt.nca")) metaNcas.push_back(&e);
    }

    // 1) Meta NCAs -> content records -> control NCA. A dump can hold several titles
    //    (base + update + DLC); the application wins, then the update.
    std::vector<Cnmt> metas;
    for (const Entry* e : metaNcas) {
        try {
            Cnmt c;
            if (readCnmt(ctx, openNca(ctx, *e), c)) metas.push_back(c);
        } catch (const Fail& f) {
            if (isKeyError(f)) throw;
        }
    }
    std::stable_sort(metas.begin(), metas.end(), [](const Cnmt& a, const Cnmt& b) {
        return a.rank() != b.rank() ? a.rank() < b.rank() : a.version > b.version;
    });

    bool found = false;
    Nca control;
    uint64_t titleId = 0;
    for (const Cnmt& m : metas) {
        if (m.controlId.empty() || found) continue;
        for (const Entry* e : ncas) {
            if (!startsWith(lowerAscii(e->name), m.controlId)) continue;
            Nca nca = openNca(ctx, *e);
            if (nca.contentType == kContentControl) {
                control = std::move(nca);
                titleId = m.titleId;
                found = true;
            }
            break;
        }
    }

    // 2) Fallback: look at the header of every small NCA.
    if (!found) {
        std::vector<const Entry*> small;
        for (const Entry* e : ncas)
            if (e->size <= 0x4000000 && !endsWith(lowerAscii(e->name), ".cnmt.nca")) small.push_back(e);
        std::stable_sort(small.begin(), small.end(), [](const Entry* a, const Entry* b) { return a->size < b->size; });
        for (const Entry* e : small) {
            try {
                Nca nca = openNca(ctx, *e);
                if (nca.contentType != kContentControl) continue;
                // an update's NCAs already carry the application ID in the header
                titleId = nca.programId;
                for (const Cnmt& m : metas)
                    if (m.rank() < 2) {
                        titleId = m.titleId;
                        break;
                    }
                control = std::move(nca);
                found = true;
                break;
            } catch (const Fail& f) {
                if (isKeyError(f)) throw;
            }
        }
    }
    if (!found) fail("NO_CONTROL_NCA");

    readControl(ctx, control, languages, out);
    out.titleId = hex64(titleId);
}

}  // namespace

bool ReadTitle(const std::wstring& file, const std::wstring& keysDir, const std::vector<int>& languages, Title& out,
               std::string& err) {
    try {
        err.clear();
        Title t;
        readTitleImpl(file, keysDir, languages, t);
        out = std::move(t);
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

}  // namespace nsw
