// amiibo tag dumps: layout, keys, decryption and signing, the console's
// operations, and the collection in <data>\amiibo. See amiibo.h.
//
// What is taken from where (all of it public description, none of it code):
//   - the tag layout, the "internal" order the signatures and the cipher work
//     in, the key derivation (an HMAC-SHA-256 counter generator over a seed
//     built from the tag) and which bytes each signature covers: amiitool
//     (socram8888/amiitool, MIT), its README and nfc3d/amiibo.c, keygen.c,
//     drbg.c as documentation;
//   - the meaning of the decrypted fields (settings flags, nickname, owner Mii,
//     game data area, the register-info CRC) and what the console's operations
//     change: the NFP service of the yuzu family as kept by Eden
//     (src/core/hle/service/nfc/common/device.cpp, nfp_types.h) and 3dbrew's
//     "Amiibo" page;
//   - AES-128 (FIPS-197) and the counter mode (NIST SP 800-38A).
// Nothing was checked against a real figure's dump or the real keys: the
// round trip, the signatures and the layout were proven with made-up tags
// and a made-up key file (tools/amiibo_test.cpp).
#include "amiibo.h"
#include "app.h"
#include "fr_crypto.h"
#include <algorithm>
#include <cstring>
#include <set>

#ifdef _WIN32
#include <shobjidl.h>
#endif

namespace amiibo {
namespace {

// ---- AES-128, encryption only (counter mode needs nothing else) ----

const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59,
    0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1,
    0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83,
    0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c,
    0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee,
    0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6,
    0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1,
    0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

uint8_t Xtime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

struct Aes128 {
    uint8_t rk[176];
    explicit Aes128(const uint8_t key[16]) {
        memcpy(rk, key, 16);
        uint8_t rcon = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
            if (i % 16 == 0) {
                const uint8_t first = t[0];
                t[0] = static_cast<uint8_t>(kSbox[t[1]] ^ rcon);
                t[1] = kSbox[t[2]];
                t[2] = kSbox[t[3]];
                t[3] = kSbox[first];
                rcon = Xtime(rcon);
            }
            for (int j = 0; j < 4; j++) rk[i + j] = static_cast<uint8_t>(rk[i - 16 + j] ^ t[j]);
        }
    }
    void Block(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = static_cast<uint8_t>(in[i] ^ rk[i]);
        for (int round = 1; round <= 10; round++) {
            uint8_t t[16];
            // SubBytes and ShiftRows together (the state is column after column).
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) t[c * 4 + r] = kSbox[s[((c + r) % 4) * 4 + r]];
            if (round < 10) {
                for (int c = 0; c < 4; c++) {
                    uint8_t* col = t + c * 4;
                    const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
                    col[0] = static_cast<uint8_t>(a0 ^ all ^ Xtime(static_cast<uint8_t>(a0 ^ a1)));
                    col[1] = static_cast<uint8_t>(a1 ^ all ^ Xtime(static_cast<uint8_t>(a1 ^ a2)));
                    col[2] = static_cast<uint8_t>(a2 ^ all ^ Xtime(static_cast<uint8_t>(a2 ^ a3)));
                    col[3] = static_cast<uint8_t>(a3 ^ all ^ Xtime(static_cast<uint8_t>(a3 ^ a0)));
                }
            }
            for (int i = 0; i < 16; i++) s[i] = static_cast<uint8_t>(t[i] ^ rk[round * 16 + i]);
        }
        memcpy(out, s, 16);
    }
};

// Counter mode with the whole 16-byte block as a big-endian counter (as the
// tags were written: amiitool hands the cipher the derived IV as the first block).
void AesCtr(const uint8_t key[16], const uint8_t iv[16], uint8_t* data, size_t n) {
    Aes128 aes(key);
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    for (size_t at = 0; at < n; at += 16) {
        aes.Block(ctr, ks);
        for (size_t i = 0; i < 16 && at + i < n; i++) data[at + i] ^= ks[i];
        for (int i = 15; i >= 0 && ++ctr[i] == 0; i--) {
        }
    }
}

// ---- the tag's two byte orders ----
// The signatures and the cipher work on the tag rearranged ("internal" order,
// 520 bytes). Each line: offset in the internal image, offset on the tag, length.
const int kMap[7][3] = {{0x000, 0x008, 0x008}, {0x008, 0x080, 0x020}, {0x028, 0x010, 0x024}, {0x04C, 0x0A0, 0x168},
                        {0x1B4, 0x034, 0x020}, {0x1D4, 0x000, 0x008}, {0x1DC, 0x054, 0x02C}};
constexpr size_t kInternal = 0x208;

void ToInternal(const uint8_t* tag, uint8_t* in) {
    for (const auto& m : kMap) memcpy(in + m[0], tag + m[1], m[2]);
}
void ToTag(const uint8_t* in, uint8_t* tag) {
    for (const auto& m : kMap) memcpy(tag + m[1], in + m[0], m[2]);
}

// One master key (80 bytes): HMAC key 16, type name 14, a spare byte, the
// number of magic bytes, 16 magic bytes, a 32-byte pad.
struct Master {
    const uint8_t* hmac;
    const uint8_t* type;
    uint8_t magicSize;
    const uint8_t* magic;
    const uint8_t* pad;
};
Master Split(const uint8_t* k) {
    return {k, k + 16, k[31], k + 32, k + 48};
}

// AES key, IV and HMAC key for one dump, from one master key.
struct Derived {
    uint8_t aesKey[16], iv[16], hmacKey[16];
};

Derived Derive(const uint8_t* masterRaw, const uint8_t* in) {
    const Master m = Split(masterRaw);
    // The seed: the write counter, 14 zeros, the serial number twice, the salt.
    uint8_t seed[64] = {};
    memcpy(seed, in + 0x029, 2);
    memcpy(seed + 0x10, in + 0x1D4, 8);
    memcpy(seed + 0x18, in + 0x1D4, 8);
    memcpy(seed + 0x20, in + 0x1E8, 32);
    // What the generator hashes: the type name up to and with its end mark, the
    // first (16 - magic) bytes of the seed, the magic bytes, seed 0x10-0x1F,
    // and seed 0x20-0x3F masked with the pad.
    std::string prepared;
    for (int i = 0; i < 14; i++) {
        prepared += static_cast<char>(m.type[i]);
        if (!m.type[i]) break;
    }
    prepared.append(reinterpret_cast<const char*>(seed), 16 - m.magicSize);
    prepared.append(reinterpret_cast<const char*>(m.magic), m.magicSize);
    prepared.append(reinterpret_cast<const char*>(seed + 0x10), 16);
    for (int i = 0; i < 32; i++) prepared += static_cast<char>(seed[0x20 + i] ^ m.pad[i]);
    const std::string key(reinterpret_cast<const char*>(m.hmac), 16);
    std::string out;
    for (uint16_t it = 0; out.size() < 48; it++) {
        std::string block;
        block += static_cast<char>(it >> 8);
        block += static_cast<char>(it & 0xFF);
        out += frc::HmacSha256(key, block + prepared);
    }
    Derived d;
    memcpy(d.aesKey, out.data(), 16);
    memcpy(d.iv, out.data() + 16, 16);
    memcpy(d.hmacKey, out.data() + 32, 16);
    return d;
}

std::string Hmac(const uint8_t key[16], const uint8_t* data, size_t n) {
    return frc::HmacSha256(std::string(reinterpret_cast<const char*>(key), 16), std::string(reinterpret_cast<const char*>(data), n));
}

// Up to 540 bytes, zero-padded (a 532-byte dump lacks only the password pages).
std::string Padded(const std::string& tag) {
    std::string t = tag.substr(0, kTag);
    t.resize(kTag, '\0');
    return t;
}

uint8_t* U8(std::string& s) {
    return reinterpret_cast<uint8_t*>(&s[0]);
}
const uint8_t* U8(const std::string& s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}
unsigned Be16(const std::string& p, size_t at) {
    return static_cast<unsigned>(static_cast<uint8_t>(p[at]) << 8 | static_cast<uint8_t>(p[at + 1]));
}
void PutBe16(std::string& p, size_t at, unsigned v) {
    p[at] = static_cast<char>((v >> 8) & 0xFF);
    p[at + 1] = static_cast<char>(v & 0xFF);
}

// ---- fields of the decrypted tag (tag order) ----
// 0x11 write counter, 0x14 flags (bit 4: registered, bit 5: game data; low
// nibble: font region), 0x15 country, 0x16 settings CRC counter, 0x18 date set
// up, 0x1A date written, 0x1C settings CRC, 0x20 nickname (10 UTF-16 BE units),
// 0xA0 owner Mii (0x60), 0x100 game id, 0x108 game write counter, 0x10A data
// area id, 0x10E game id byte, 0x10F unknown, 0x110 Mii extension (8),
// 0x118 unknown (20), 0x12C register-info CRC, 0x130 game data area (0xD8).
constexpr size_t kFlags = 0x14, kSetUp = 0x18, kWritten = 0x1A, kName = 0x20, kMii = 0xA0, kAppId = 0x100, kAppWrites = 0x108,
                 kAreaId = 0x10A, kAppIdByte = 0x10E, kUnknown = 0x10F, kMiiExt = 0x110, kUnknown2 = 0x118, kRegCrc = 0x12C,
                 kArea = 0x130;
constexpr uint8_t kRegistered = 0x10, kHasData = 0x20;

uint32_t Crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// The register-info CRC covers the owner Mii, the game id byte, the unknown
// byte, the Mii extension and the 20 unknown bytes, in that order (Eden's
// UpdateRegisterInfoCrc), stored big-endian.
uint32_t RegisterCrc(const std::string& p) {
    std::string d = p.substr(kMii, 0x60);
    d += p[kAppIdByte];
    d += p[kUnknown];
    d += p.substr(kMiiExt, 8);
    d += p.substr(kUnknown2, 20);
    return Crc32(d);
}
uint32_t StoredRegisterCrc(const std::string& p) {
    return static_cast<uint32_t>(Be16(p, kRegCrc)) << 16 | Be16(p, kRegCrc + 2);
}
void PutRegisterCrc(std::string& p, uint32_t v) {
    PutBe16(p, kRegCrc, v >> 16);
    PutBe16(p, kRegCrc + 2, v & 0xFFFF);
}

void Randomize(std::string& p, size_t at, size_t n) {
    const std::string r = frc::RandomBytes(n);
    p.replace(at, n, r);
}

unsigned DateOf(int y, int m, int d) {
    return static_cast<unsigned>(((y - 2000) & 0x7F) << 9 | (m & 0xF) << 5 | (d & 0x1F));
}
std::wstring DateText(unsigned v) {
    const int y = static_cast<int>(v >> 9) + 2000, m = static_cast<int>(v >> 5 & 0xF), d = static_cast<int>(v & 0x1F);
    if (m < 1 || m > 12 || d < 1 || d > 31) return {};
    wchar_t b[24];
    swprintf_s(b, L"%02d/%02d/%04d", d, m, y);
    return b;
}

// Every write the console makes counts on the tag and dates it.
void Written(std::string& p, int y, int m, int d) {
    const unsigned n = Be16(p, 0x11);
    if (n < 0xFFFF) PutBe16(p, 0x11, n + 1);
    PutBe16(p, kWritten, DateOf(y, m, d));
}

// UTF-16 units into a wide string (wchar_t is 32 bits outside Windows).
std::wstring FromUnits(const std::vector<uint16_t>& u) {
    std::wstring out;
    for (size_t i = 0; i < u.size() && u[i]; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00u);
            i++;
        }
        if (sizeof(wchar_t) == 2 && c > 0xFFFF) {
            out += static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10));
            out += static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            out += static_cast<wchar_t>(c);
        }
    }
    return out;
}
std::vector<uint16_t> ToUnits(const std::wstring& s) {
    std::vector<uint16_t> u;
    for (wchar_t w : s) {
        const uint32_t c = static_cast<uint32_t>(w);
        if (c > 0xFFFF) {
            u.push_back(static_cast<uint16_t>(0xD800 + ((c - 0x10000) >> 10)));
            u.push_back(static_cast<uint16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            u.push_back(static_cast<uint16_t>(c));
        }
    }
    return u;
}

// ---- files ----

bool ReadFile(const std::wstring& path, std::string& out, size_t most) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    out.assign(most + 1, '\0');
    const size_t got = fread(&out[0], 1, most + 1, f);
    fclose(f);
    out.resize(got);
    return got <= most;
}

bool WriteFile(const std::wstring& path, const std::string& data) {
    // A temp file first, so that a failure never leaves half a dump.
    const std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}

bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ParentOf(const std::wstring& p) {
    const size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}
std::wstring LeafOf(const std::wstring& p) {
    const size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
bool EndsWithI(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

struct Found {
    std::wstring name;
    bool dir;
    uint64_t size;
};
std::vector<Found> Listing(const std::wstring& dir) {
    std::vector<Found> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back({fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       static_cast<uint64_t>(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// A name that is safe as a file name everywhere, at most 60 characters.
std::wstring CleanName(std::wstring s) {
    std::wstring out;
    for (wchar_t c : s)
        if (c >= 32 && !wcschr(L"\\/:*?\"<>|", c)) out += c;
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    if (out.size() > 60) out.resize(60);
    return out;
}

std::wstring StemOf(const std::wstring& path) {
    std::wstring leaf = LeafOf(path);
    const size_t dot = leaf.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) leaf.resize(dot);
    return leaf;
}

bool Ours(const std::wstring& file) {
    return ParentOf(file) == Dir() && EndsWithI(file, L".bin") && file.find(L"..") == std::wstring::npos;
}

void MakeDirs() {
    CreateDirectoryW(DataDir().c_str(), nullptr);
    CreateDirectoryW(Dir().c_str(), nullptr);
}

// Where a new entry called `name` goes: the name, or the name and a number.
std::wstring FreeFile(const std::wstring& name) {
    std::wstring base = CleanName(name);
    if (base.empty()) base = L"amiibo";
    std::wstring file = Dir() + L"\\" + base + L".bin";
    for (int n = 2; Exists(file) && n < 10000; n++) file = Dir() + L"\\" + base + L" (" + std::to_wstring(n) + L").bin";
    return file;
}

void Today(int& y, int& m, int& d) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    y = st.wYear, m = st.wMonth, d = st.wDay;
}

} // namespace

// ---- a dump, read without keys ----

bool TagSize(uint64_t n) {
    return n == 540 || n == 532 || n == 572;
}

bool LooksLikeTag(const std::string& s) {
    if (!TagSize(s.size())) return false;
    const uint8_t* b = U8(s);
    // The serial number's two check bytes (ISO/IEC 14443-3: the first covers
    // the cascade tag 0x88 and three bytes, the second the next four).
    if ((0x88 ^ b[0] ^ b[1] ^ b[2]) != b[3] || (b[4] ^ b[5] ^ b[6] ^ b[7]) != b[8]) return false;
    // Lock bytes and capability container of an amiibo, its 0xA5 marker, tag
    // type 2 in the identification block, dynamic lock, CFG0 and CFG1: the
    // constants every amiibo carries (the same ones the yuzu family checks).
    static const uint8_t cc[4] = {0xF1, 0x10, 0xFF, 0xEE}, cfg0[4] = {0x00, 0x00, 0x00, 0x04}, cfg1[4] = {0x5F, 0x00, 0x00, 0x00};
    return b[0x0A] == 0x0F && b[0x0B] == 0xE0 && !memcmp(b + 0x0C, cc, 4) && b[0x10] == 0xA5 && b[0x5B] == 0x02 && b[0x208] == 0x01 &&
           b[0x209] == 0x00 && b[0x20A] == 0x0F && !memcmp(b + 0x20C, cfg0, 4) && !memcmp(b + 0x210, cfg1, 4);
}

std::string IdHex(const std::string& tag) {
    if (tag.size() < 0x5C) return {};
    static const char* const hex = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0x54; i < 0x5C; i++) {
        const uint8_t v = static_cast<uint8_t>(tag[i]);
        out += hex[v >> 4];
        out += hex[v & 15];
    }
    return out;
}

int Series(const std::string& tag) {
    return tag.size() > 0x5A ? static_cast<uint8_t>(tag[0x5A]) : 0;
}

// ---- keys ----

std::string ParseKeys(const std::string& raw, Keys& out) {
    if (raw.size() != 160) return "KEYS_INVALID";
    static const char data[14] = "unfixed infos", tag[14] = "locked secret"; // the public type names, with their end mark
    const uint8_t* a = U8(raw);
    const uint8_t* b = a + 80;
    if (!memcmp(a + 16, tag, 14) && !memcmp(b + 16, data, 14)) std::swap(a, b);
    if (memcmp(a + 16, data, 14) || memcmp(b + 16, tag, 14) || a[31] > 16 || b[31] > 16) return "KEYS_INVALID";
    memcpy(out.data, a, 80);
    memcpy(out.tag, b, 80);
    return {};
}

// ---- the decrypted tag ----

bool Decode(const Keys& k, const std::string& tagIn, std::string& plain) {
    if (tagIn.size() < kInternal) return false;
    const std::string tag = Padded(tagIn);
    uint8_t in[kInternal], p[kInternal];
    ToInternal(U8(tag), in);
    memcpy(p, in, kInternal);
    const Derived dk = Derive(k.data, in), tk = Derive(k.tag, in);
    AesCtr(dk.aesKey, dk.iv, p + 0x02C, 0x188);
    // The tag signature first: the data signature covers it.
    const std::string th = Hmac(tk.hmacKey, p + 0x1D4, 0x34);
    memcpy(p + 0x1B4, th.data(), 32);
    const std::string dh = Hmac(dk.hmacKey, p + 0x029, 0x1DF);
    memcpy(p + 0x008, dh.data(), 32);
    if (memcmp(p + 0x1B4, in + 0x1B4, 32) || memcmp(p + 0x008, in + 0x008, 32)) return false;
    plain = tag;
    ToTag(p, U8(plain));
    return true;
}

std::string Encode(const Keys& k, const std::string& plain) {
    std::string tag = plain;
    if (tag.size() < kInternal) tag.resize(kTag, '\0');
    uint8_t in[kInternal];
    ToInternal(U8(tag), in);
    const Derived dk = Derive(k.data, in), tk = Derive(k.tag, in);
    const std::string th = Hmac(tk.hmacKey, in + 0x1D4, 0x34);
    memcpy(in + 0x1B4, th.data(), 32);
    const std::string dh = Hmac(dk.hmacKey, in + 0x029, 0x1DF);
    memcpy(in + 0x008, dh.data(), 32);
    AesCtr(dk.aesKey, dk.iv, in + 0x02C, 0x188);
    ToTag(in, U8(tag));
    return tag;
}

Facts Read(const std::string& p) {
    Facts f;
    if (p.size() < kInternal) return f;
    const uint8_t flags = static_cast<uint8_t>(p[kFlags]);
    f.registered = (flags & kRegistered) != 0;
    f.gameData = (flags & kHasData) != 0;
    f.writes = Be16(p, 0x11);
    if (f.registered) {
        std::vector<uint16_t> name(10), mii(10);
        for (size_t i = 0; i < 10; i++) {
            name[i] = static_cast<uint16_t>(Be16(p, kName + i * 2));
            // The owner's name in the Mii record: 10 UTF-16 units, little-endian, at 0x1A.
            mii[i] = static_cast<uint16_t>(static_cast<uint8_t>(p[kMii + 0x1A + i * 2]) | static_cast<uint8_t>(p[kMii + 0x1B + i * 2]) << 8);
        }
        f.nickname = FromUnits(name);
        f.owner = FromUnits(mii);
        f.setUp = DateText(Be16(p, kSetUp));
    }
    if (f.gameData)
        for (size_t i = 0; i < 8; i++) f.appId = f.appId << 8 | static_cast<uint8_t>(p[kAppId + i]);
    f.written = DateText(Be16(p, kWritten));
    return f;
}

std::string SetNickname(std::string& p, const std::wstring& name, int y, int m, int d) {
    if (!(static_cast<uint8_t>(p[kFlags]) & kRegistered)) return "NOT_REGISTERED";
    const std::vector<uint16_t> u = ToUnits(name);
    if (u.empty() || u.size() > 10) return "BAD_NAME";
    for (size_t i = 0; i < 10; i++) PutBe16(p, kName + i * 2, i < u.size() ? u[i] : 0);
    Written(p, y, m, d);
    return {};
}

namespace {
// The data area goes the way the console's own deletion leaves it: overwritten
// with random bytes along with the game's ids, the flag cleared, the game write
// counter moved on. The game id byte is part of the register-info CRC: that is
// kept right when it was right before (NSW-Home then computes it the same way
// as whatever wrote it) and otherwise left alone.
void ClearData(std::string& p) {
    const bool crcRight = RegisterCrc(p) == StoredRegisterCrc(p);
    Randomize(p, kArea, 0xD8);
    Randomize(p, kAppId, 8);
    Randomize(p, kAreaId, 4);
    Randomize(p, kAppIdByte, 1);
    p[kFlags] = static_cast<char>(static_cast<uint8_t>(p[kFlags]) & ~kHasData);
    const unsigned n = Be16(p, kAppWrites);
    if (n < 0xFFFF) PutBe16(p, kAppWrites, n + 1);
    if (crcRight) PutRegisterCrc(p, RegisterCrc(p));
}
} // namespace

std::string DeleteGameData(std::string& p, int y, int m, int d) {
    if (!(static_cast<uint8_t>(p[kFlags]) & kHasData)) return "NO_DATA";
    ClearData(p);
    Written(p, y, m, d);
    return {};
}

std::string Format(std::string& p, int y, int m, int d) {
    const uint8_t flags = static_cast<uint8_t>(p[kFlags]);
    if (!(flags & (kRegistered | kHasData))) return "BLANK";
    if (flags & kHasData) ClearData(p);
    if (flags & kRegistered) {
        // Owner, nickname, the bytes that go with them and the dates: random,
        // as the console leaves them; the flag and the font region cleared.
        Randomize(p, kMii, 0x60);
        Randomize(p, kName, 20);
        Randomize(p, kUnknown, 1);
        Randomize(p, kUnknown2, 8);
        Randomize(p, kRegCrc, 4);
        Randomize(p, kSetUp, 4);
        p[kFlags] = static_cast<char>(static_cast<uint8_t>(p[kFlags]) & ~(kRegistered | 0x0F));
    }
    Written(p, y, m, d);
    return {};
}

// ---- the collection ----

std::wstring Dir() {
    return DataDir() + L"\\amiibo";
}

std::wstring KeyFile() {
    return Dir() + L"\\claves\\key_retail.bin";
}

void Backup(const std::wstring& file) {
    const std::wstring bak = file + L".bak";
    std::string bytes;
    if (Ours(file) && !Exists(bak) && ReadFile(file, bytes, 4096)) WriteFile(bak, bytes);
}

std::vector<Entry> List() {
    std::vector<Entry> out;
    // Ryujinx and its forks never change the dump they are given: what the game
    // wrote goes to <name>_modified.bin beside it (beside the entry, or in the
    // hand-over folder, see ForEmulator). That becomes the entry, in the
    // entry's own length.
    for (const std::wstring& dir : {Dir(), Dir() + L"\\envio"}) {
        for (const Found& f : Listing(dir)) {
            if (f.dir || !EndsWithI(f.name, L"_modified.bin")) continue;
            const std::wstring mod = dir + L"\\" + f.name;
            const std::wstring base = Dir() + L"\\" + f.name.substr(0, f.name.size() - 13) + L".bin";
            std::string bytes, old;
            if (!ReadFile(base, old, 4096) || !LooksLikeTag(old) || !ReadFile(mod, bytes, 4096) || !LooksLikeTag(bytes)) continue;
            std::string merged = bytes.substr(0, (std::min)(bytes.size(), old.size()));
            if (old.size() > merged.size()) merged += old.substr(merged.size());
            Backup(base);
            if (WriteFile(base, merged)) DeleteFileW(mod.c_str());
        }
    }
    for (const Found& f : Listing(Dir())) {
        if (f.dir || !EndsWithI(f.name, L".bin") || !TagSize(f.size)) continue;
        Entry e;
        e.file = Dir() + L"\\" + f.name;
        if (!ReadFile(e.file, e.bytes, 4096) || !LooksLikeTag(e.bytes)) continue;
        e.name = f.name.substr(0, f.name.size() - 4);
        e.id = IdHex(e.bytes);
        e.series = Series(e.bytes);
        e.backup = Exists(e.file + L".bak");
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

namespace {
struct Importer {
    std::set<std::string> have;
    Imported r;
    int visited = 0;
    Importer() {
        for (const Entry& e : List()) have.insert(e.bytes);
    }
    void File(const std::wstring& path, uint64_t size) {
        std::string bytes;
        if (!TagSize(size) || !ReadFile(path, bytes, 4096) || !LooksLikeTag(bytes)) {
            r.skipped++;
            return;
        }
        if (have.count(bytes)) {
            r.had++;
            return;
        }
        MakeDirs();
        const std::wstring to = FreeFile(StemOf(path));
        if (!WriteFile(to, bytes)) {
            r.failed++;
            return;
        }
        have.insert(bytes);
        r.added++;
        r.name = StemOf(to);
    }
    // Ten levels and fifty thousand entries at most: a folder of dumps, not a whole disk.
    void Folder(const std::wstring& dir, int depth) {
        if (depth > 10) return;
        for (const Found& f : Listing(dir)) {
            if (++visited > 50000) return;
            const std::wstring path = dir + L"\\" + f.name;
            if (f.dir) {
                if (path != Dir()) Folder(path, depth + 1); // (not the collection itself)
            } else {
                File(path, f.size);
            }
        }
    }
};
} // namespace

Imported ImportFile(const std::wstring& path) {
    Importer im;
    std::string probe;
    if (!ReadFile(path, probe, 4096)) {
        im.r.skipped++;
        return im.r;
    }
    im.File(path, probe.size());
    return im.r;
}

Imported ImportFolder(const std::wstring& dir) {
    Importer im;
    std::wstring d = dir;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    im.Folder(d, 0);
    return im.r;
}

std::string Rename(const std::wstring& file, const std::wstring& name) {
    if (!Ours(file) || !Exists(file)) return "NOT_OURS";
    const std::wstring clean = CleanName(name);
    if (clean.empty()) return "BAD_NAME";
    const std::wstring to = Dir() + L"\\" + clean + L".bin";
    if (to == file) return {};
    // (Only the case changing: the same file to a system that ignores case.)
    if (Exists(to) && _wcsicmp(to.c_str(), file.c_str())) return "NAME_TAKEN";
    if (!MoveFileExW(file.c_str(), to.c_str(), 0)) return "IO";
    const std::wstring bak = file + L".bak";
    if (Exists(bak)) MoveFileExW(bak.c_str(), (to + L".bak").c_str(), MOVEFILE_REPLACE_EXISTING);
    return {};
}

std::string Remove(const std::wstring& file) {
    if (!Ours(file)) return "NOT_OURS";
    if (!DeleteFileW(file.c_str())) return "IO";
    DeleteFileW((file + L".bak").c_str());
    return {};
}

bool HaveKeys() {
    Keys k;
    return LoadKeys(k);
}

bool LoadKeys(Keys& out) {
    std::string raw;
    return ReadFile(KeyFile(), raw, 160) && ParseKeys(raw, out).empty();
}

std::string SetKeys(const std::wstring& file) {
    std::string raw;
    Keys k;
    if (!ReadFile(file, raw, 4096) || !ParseKeys(raw, k).empty()) return "KEYS_INVALID";
    MakeDirs();
    CreateDirectoryW(ParentOf(KeyFile()).c_str(), nullptr);
    return WriteFile(KeyFile(), raw) ? std::string() : std::string("IO");
}

void ForgetKeys() {
    DeleteFileW(KeyFile().c_str());
}

bool Open(const std::string& bytes, Facts& out) {
    Keys k;
    std::string plain;
    if (!LoadKeys(k) || !Decode(k, bytes, plain)) return false;
    out = Read(plain);
    return true;
}

std::string Apply(const std::wstring& file, Op op, const std::wstring& nickname) {
    if (!Ours(file)) return "NOT_OURS";
    Keys k;
    if (!LoadKeys(k)) return "NO_KEYS";
    std::string bytes, plain;
    if (!ReadFile(file, bytes, 4096) || !LooksLikeTag(bytes)) return "IO";
    if (!Decode(k, bytes, plain)) return "BAD_KEYS";
    int y, m, d;
    Today(y, m, d);
    std::string meant = plain;
    const std::string err = op == Op::Nickname ? SetNickname(meant, nickname, y, m, d)
                            : op == Op::DeleteData ? DeleteGameData(meant, y, m, d)
                                                   : Format(meant, y, m, d);
    if (!err.empty()) return err;
    const std::string coded = Encode(k, meant);
    // Opened again with the keys, the new dump must give back exactly what was
    // meant, signatures included: a wrong write would leave a useless dump.
    std::string check;
    if (!Decode(k, coded, check) || !LooksLikeTag(coded)) return "VERIFY";
    std::string expect = meant;
    memcpy(U8(expect) + 0x34, U8(check) + 0x34, 32); // the two signatures, as only the keys can make them
    memcpy(U8(expect) + 0x80, U8(check) + 0x80, 32);
    if (expect.compare(0, kInternal, check, 0, kInternal)) return "VERIFY";
    // Back in the file's own length: a 532-byte dump stays 532, the 32 bytes
    // after a 572-byte one (the chip's signature) stay as they were.
    std::string out = bytes;
    out.replace(0, (std::min)(out.size(), kTag), coded.substr(0, (std::min)(out.size(), kTag)));
    Backup(file);
    return WriteFile(file, out) ? std::string() : std::string("IO");
}

std::wstring ForEmulator(const std::wstring& file, char family) {
    std::string bytes;
    if (family != 'r' || !Ours(file) || !ReadFile(file, bytes, 4096) || bytes.size() == kTag) return file;
    // Ryujinx's forks read exactly 540 bytes (a 572-byte file fails, and what
    // the game writes to a 532-byte one is not saved): they get a 540-byte copy.
    const std::wstring dir = Dir() + L"\\envio", copy = dir + L"\\" + LeafOf(file);
    CreateDirectoryW(dir.c_str(), nullptr);
    return WriteFile(copy, Padded(bytes)) ? copy : file;
}

bool EmulatorCanRead(const std::wstring& emuDataDir, char family) {
    return family != 'r' || emuDataDir.empty() || Exists(emuDataDir + L"\\system\\key_retail.bin");
}

std::string CtrForTest(const std::string& key16, const std::string& iv16, const std::string& data) {
    std::string out = data;
    if (key16.size() == 16 && iv16.size() == 16 && !out.empty()) AesCtr(U8(key16), U8(iv16), U8(out), out.size());
    return out;
}

// ---- pickers ----

#ifdef _WIN32
namespace {
std::wstring PickBin(const wchar_t* kind) {
    // An offscreen test run never opens anything on the desktop (the UI gives
    // its test states a path of their own; this is the second lock).
    if (wcsstr(GetCommandLineW(), L"--shot")) return {};
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    const COMDLG_FILTERSPEC spec[2] = {{kind, L"*.bin"}, {L"Todos los archivos", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}
} // namespace
std::wstring PickDump() {
    return PickBin(L"Archivos de amiibo (*.bin)");
}
std::wstring PickKeyFile() {
    return PickBin(L"Claves de amiibo (key_retail.bin)");
}
#else
// The port's own pickers: on Android the one that takes any file, elsewhere
// the program picker, which has no filter.
std::wstring PickDump() {
#ifdef __ANDROID__
    return PickKeys();
#else
    return PickExe();
#endif
}
std::wstring PickKeyFile() {
    return PickDump();
}
#endif

} // namespace amiibo
