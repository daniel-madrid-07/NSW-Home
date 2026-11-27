// Checks src/amiibo.cpp without any real figure or real key: published vectors
// for the cipher and the HMAC, the system's own AES against ours (Windows),
// and, with a made-up 160-byte key file and made-up tags written through
// NSW-Home's own encode path, the round trip, the signatures, the layout
// checks, each console operation (exactly which bytes it changes), the
// collection (import of a file and of a folder, duplicates, rename, removal,
// the .bak, dump lengths 532/540/572 kept) and the Ryujinx "_modified" fold.
//
// It also leaves the made-up files in <out>\ for NSW-Home's own test states
// (the amiibo-* states in ui.cpp; run it with local\amiibo_test). Nothing in
// them comes from a real amiibo.
//
// Build and run: tools\build_amiibo_test.bat <out folder>   (or, anywhere:
//   g++ -std=c++17 -Wall -Wextra -I src/port/win tools/amiibo_test.cpp src/amiibo.cpp src/fr_crypto.cpp src/port/compat.cpp)
#include "../src/amiibo.h"
#include "../src/app.h"
#include "../src/fr_crypto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

static std::wstring gData;
std::wstring DataDir() {
    return gData;
}
#ifndef _WIN32
std::wstring PickExe() { return {}; }
std::wstring PickKeys() { return {}; }
#endif

namespace {

int passed = 0, failed = 0;
void Check(bool ok, const std::string& what) {
    if (ok) {
        passed++;
        printf("  ok    %s\n", what.c_str());
    } else {
        failed++;
        printf("  FAIL  %s\n", what.c_str());
    }
}

std::string Bytes(const char* hex) {
    return frc::Unhex(hex);
}

bool ReadBin(const std::wstring& p, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") || !f) return false;
    char buf[8192];
    out.clear();
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
void WriteBin(const std::wstring& p, const std::string& d) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") || !f) return;
    fwrite(d.data(), 1, d.size(), f);
    fclose(f);
}
void Mk(const std::wstring& d) {
    CreateDirectoryW(d.c_str(), nullptr);
}
bool Has(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// A made-up key file: random keys with the public type names and magic sizes.
std::string FakeKeys() {
    std::string k;
    for (int half = 0; half < 2; half++) {
        std::string h = frc::RandomBytes(16);
        const char* type = half == 0 ? "unfixed infos" : "locked secret";
        h.append(type, 14); // with its end mark
        h += '\0';
        h += static_cast<char>(half == 0 ? 14 : 16);
        h += frc::RandomBytes(16);
        h += frc::RandomBytes(32);
        k += h;
    }
    return k;
}

uint32_t Crc32(const std::string& s) { // independent of amiibo.cpp's
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

void Put16(std::string& p, size_t at, unsigned v) {
    p[at] = static_cast<char>(v >> 8);
    p[at + 1] = static_cast<char>(v);
}

// A made-up decrypted tag in the tag's own layout.
std::string MakePlain(const char* idHex, bool registered, const wchar_t* nick, const wchar_t* owner, bool data) {
    std::string p(540, '\0');
    const std::string r = frc::RandomBytes(8);
    p[0] = 0x04;
    p[1] = r[0], p[2] = r[1];
    p[3] = static_cast<char>(0x88 ^ 0x04 ^ static_cast<uint8_t>(r[0]) ^ static_cast<uint8_t>(r[1]));
    for (int i = 4; i < 8; i++) p[i] = r[i - 2];
    p[8] = static_cast<char>(p[4] ^ p[5] ^ p[6] ^ p[7]);
    p[9] = 0x48;
    p[0x0A] = 0x0F, p[0x0B] = static_cast<char>(0xE0);
    p.replace(0x0C, 4, Bytes("f110ffee"));
    p[0x10] = static_cast<char>(0xA5);
    Put16(p, 0x11, 7);
    p[0x14] = static_cast<char>((registered ? 0x10 : 0) | (data ? 0x20 : 0));
    Put16(p, 0x16, 1);
    Put16(p, 0x18, (24 << 9) | (5 << 5) | 17); // 17/05/2024
    Put16(p, 0x1A, (25 << 9) | (2 << 5) | 3);  // 03/02/2025
    p.replace(0x1C, 4, frc::RandomBytes(4));
    if (registered)
        for (size_t i = 0; nick[i] && i < 10; i++) Put16(p, 0x20 + i * 2, static_cast<unsigned>(nick[i]));
    else
        p.replace(0x20, 20, frc::RandomBytes(20));
    p.replace(0x54, 8, Bytes(idHex));
    p.replace(0x60, 32, frc::RandomBytes(32));
    p.replace(0xA0, 0x60, frc::RandomBytes(0x60));
    if (registered) {
        for (size_t i = 0; i < 10; i++) {
            const unsigned c = i < wcslen(owner) ? static_cast<unsigned>(owner[i]) : 0;
            p[0xA0 + 0x1A + i * 2] = static_cast<char>(c & 0xFF);
            p[0xA0 + 0x1B + i * 2] = static_cast<char>(c >> 8);
        }
        p[0x10F] = 0;
        p.replace(0x110, 8, frc::RandomBytes(8));
        p.replace(0x118, 20, std::string(20, '\0'));
    }
    if (data) {
        p.replace(0x100, 8, Bytes("0100f2c0000bb000")); // a made-up game id
        Put16(p, 0x108, 3);
        p.replace(0x10A, 4, Bytes("1234abcd"));
        p[0x10E] = 0x01;
        p.replace(0x130, 0xD8, frc::RandomBytes(0xD8));
    } else {
        p.replace(0x100, 8, frc::RandomBytes(8));
        p.replace(0x130, 0xD8, frc::RandomBytes(0xD8));
    }
    std::string crc = p.substr(0xA0, 0x60);
    crc += p[0x10E];
    crc += p[0x10F];
    crc += p.substr(0x110, 8);
    crc += p.substr(0x118, 20);
    const uint32_t c = Crc32(crc);
    Put16(p, 0x12C, c >> 16);
    Put16(p, 0x12E, c & 0xFFFF);
    p.replace(0x208, 4, Bytes("01000fbd"));
    p.replace(0x20C, 4, Bytes("00000004"));
    p.replace(0x210, 4, Bytes("5f000000"));
    p.replace(0x214, 4, frc::RandomBytes(4)); // PWD (made up)
    p.replace(0x218, 4, Bytes("80800000"));
    return p;
}

// Which bytes of the decrypted tag an operation changed, as ranges.
std::vector<std::pair<size_t, size_t>> Changed(const std::string& a, const std::string& b) {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0; i < 0x208; i++) {
        if (a[i] == b[i]) continue;
        if (!out.empty() && out.back().second == i) out.back().second = i + 1;
        else out.push_back({i, i + 1});
    }
    return out;
}
// True when every changed byte lies in one of the allowed ranges.
bool Within(const std::vector<std::pair<size_t, size_t>>& ch, const std::vector<std::pair<size_t, size_t>>& allowed, std::string& why) {
    for (const auto& c : ch)
        for (size_t i = c.first; i < c.second; i++) {
            bool ok = false;
            for (const auto& a : allowed) ok = ok || (i >= a.first && i < a.second);
            if (!ok) {
                char b[64];
                snprintf(b, sizeof b, "byte 0x%03zx changed", i);
                why = b;
                return false;
            }
        }
    return true;
}
std::string Ranges(const std::vector<std::pair<size_t, size_t>>& ch) {
    std::string s;
    for (const auto& c : ch) {
        char b[32];
        snprintf(b, sizeof b, "%s0x%03zx-0x%03zx", s.empty() ? "" : " ", c.first, c.second - 1);
        s += b;
    }
    return s;
}

#ifdef _WIN32
std::string CngCtr(const std::string& key, const std::string& iv, const std::string& data);
#endif
// The cipher for the check below: the system's AES where there is one.
std::string IndependentCtr(const std::string& key, const std::string& iv, const std::string& data) {
#ifdef _WIN32
    return CngCtr(key, iv, data);
#else
    return amiibo::CtrForTest(key, iv, data);
#endif
}

// A second, independent reading of the format, written straight in tag order
// the way Ryubing's AmiiboDump.cs describes it (no "internal" order): derive
// the keys, check both signatures, decrypt. True when `tag` verifies and
// decrypts to `plain` (signature fields aside).
bool TagOrderCheck(const std::string& keyRaw, const std::string& tag, const std::string& plain) {
    auto derive = [&](const std::string& master, std::string& aesKey, std::string& iv) {
        const unsigned magic = static_cast<uint8_t>(master[31]);
        std::string seed = master.substr(16, 14);
        std::string lead = tag.substr(0x11, 2) + std::string(14, '\0');
        seed += lead.substr(0, 16 - magic);
        seed += master.substr(32, magic);
        seed += tag.substr(0, 8) + tag.substr(0, 8);
        for (int i = 0; i < 32; i++) seed += static_cast<char>(tag[0x60 + i] ^ master[48 + i]);
        const std::string key = master.substr(0, 16);
        const std::string b0 = frc::HmacSha256(key, std::string("\x00\x00", 2) + seed);
        const std::string b1 = frc::HmacSha256(key, std::string("\x00\x01", 2) + seed);
        aesKey = b0.substr(0, 16);
        iv = b0.substr(16, 16);
        return b1.substr(0, 16);
    };
    std::string aesKey, iv, unused1, unused2;
    const std::string dataKey = derive(keyRaw.substr(0, 80), aesKey, iv);
    const std::string tagKey = derive(keyRaw.substr(80, 80), unused1, unused2);
    const std::string tagHmac = frc::HmacSha256(tagKey, tag.substr(0, 8) + tag.substr(0x54, 44));
    if (tagHmac != tag.substr(0x34, 32)) return false;
    const std::string dec = IndependentCtr(aesKey, iv, tag.substr(0x14, 0x20) + tag.substr(0xA0, 0x168));
    std::string p = tag;
    p.replace(0x14, 0x20, dec.substr(0, 0x20));
    p.replace(0xA0, 0x168, dec.substr(0x20));
    const std::string dataHmac = frc::HmacSha256(dataKey, p.substr(0x11, 0x23) + p.substr(0xA0, 0x168) + tagHmac + p.substr(0, 8) + p.substr(0x54, 0x2C));
    if (dataHmac != tag.substr(0x80, 32)) return false;
    return p.compare(0x14, 0x20, plain, 0x14, 0x20) == 0 && p.compare(0xA0, 0x168, plain, 0xA0, 0x168) == 0;
}

#ifdef _WIN32
// The same counter mode built from the system's AES (CNG, one block at a time).
std::string CngCtr(const std::string& key, const std::string& iv, const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE kh = nullptr;
    std::string out = data;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) return {};
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    BCryptGenerateSymmetricKey(alg, &kh, nullptr, 0, (PUCHAR)key.data(), 16, 0);
    unsigned char ctr[16], ks[16];
    memcpy(ctr, iv.data(), 16);
    for (size_t at = 0; at < out.size(); at += 16) {
        ULONG got = 0;
        BCryptEncrypt(kh, ctr, 16, nullptr, nullptr, 0, ks, 16, &got, 0);
        for (size_t i = 0; i < 16 && at + i < out.size(); i++) out[at + i] ^= static_cast<char>(ks[i]);
        for (int i = 15; i >= 0 && ++ctr[i] == 0; i--) {
        }
    }
    BCryptDestroyKey(kh);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    const std::wstring out = argc > 1 ? argv[1] : L"amiibo_test";
#else
int main(int argc, char** argv) {
    std::wstring out = L"amiibo_test";
    if (argc > 1) out = std::wstring(argv[1], argv[1] + strlen(argv[1]));
#endif
    using namespace amiibo;
#ifdef _WIN32
    // --show <key file> <dump>: what the dump says, for checking a file NSW-Home changed.
    if (argc == 4 && out == L"--show") {
        std::string raw, dump, plain;
        Keys k;
        if (!ReadBin(argv[2], raw) || !ParseKeys(raw, k).empty() || !ReadBin(argv[3], dump)) return 2;
        printf("size %zu, amiibo layout %s, id %s\n", dump.size(), LooksLikeTag(dump) ? "yes" : "no", IdHex(dump).c_str());
        if (!Decode(k, dump, plain)) {
            printf("signatures do not match these keys\n");
            return 1;
        }
        const Facts f = Read(plain);
        printf("signatures right; registered %s, nickname \"%ls\", owner \"%ls\", game data %s (%016llX), writes %u, set up %ls, written %ls\n",
               f.registered ? "yes" : "no", f.nickname.c_str(), f.owner.c_str(), f.gameData ? "yes" : "no",
               static_cast<unsigned long long>(f.appId), f.writes, f.setUp.c_str(), f.written.c_str());
        return 0;
    }
#endif
    Mk(out);

    printf("cipher and HMAC, published vectors\n");
    // FIPS-197 appendix C.1: one block is the first block of counter mode.
    Check(frc::Hex(CtrForTest(Bytes("000102030405060708090a0b0c0d0e0f"), Bytes("00112233445566778899aabbccddeeff"), std::string(16, '\0'))) ==
              "69c4e0d86a7b0430d8cdb78070b4c55a",
          "AES-128 FIPS-197 C.1");
    // NIST SP 800-38A F.5.1, CTR-AES128.Encrypt, four blocks.
    Check(frc::Hex(CtrForTest(Bytes("2b7e151628aed2a6abf7158809cf4f3c"), Bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"),
                              Bytes("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                                    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710"))) ==
              "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
              "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee",
          "AES-128-CTR SP 800-38A F.5.1");
    // The counter carrying across bytes: ff..ff wraps to 00..00.
    {
        const std::string k = Bytes("2b7e151628aed2a6abf7158809cf4f3c"), z(32, '\0');
        const std::string two = CtrForTest(k, std::string(16, '\xff'), z);
        Check(two.substr(16) == CtrForTest(k, std::string(16, '\0'), std::string(16, '\0')), "counter wraps over all 16 bytes");
    }
    // RFC 4231 test case 2.
    Check(frc::Hex(frc::HmacSha256("Jefe", "what do ya want for nothing?")) ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "HMAC-SHA-256 RFC 4231 case 2");
#ifdef _WIN32
    {
        bool same = true;
        for (int i = 0; i < 200 && same; i++) {
            const std::string k = frc::RandomBytes(16), iv = frc::RandomBytes(16), d = frc::RandomBytes(1 + i * 7);
            same = CtrForTest(k, iv, d) == CngCtr(k, iv, d);
        }
        Check(same, "200 random messages: ours == Windows CNG AES");
    }
#endif

    printf("made-up keys and tags\n");
    const std::string keyRaw = FakeKeys(), otherRaw = FakeKeys();
    Keys keys, other, swapped;
    Check(ParseKeys(keyRaw, keys).empty(), "made-up key file accepted");
    Check(ParseKeys(keyRaw.substr(80) + keyRaw.substr(0, 80), swapped).empty() && !memcmp(&swapped, &keys, sizeof keys),
          "the two halves in the other order are put right");
    Check(!ParseKeys(frc::RandomBytes(160), other).empty(), "160 random bytes refused");
    Check(!ParseKeys(keyRaw.substr(0, 80), other).empty(), "80 bytes refused");
    ParseKeys(otherRaw, other);

    const std::string plainA = MakePlain("0000000000340102", true, L"Prueba", L"Jugador", true);
    const std::string tagA = Encode(keys, plainA);
    Check(LooksLikeTag(plainA) && LooksLikeTag(tagA), "made-up tag has an amiibo's fixed layout");
    Check(IdHex(tagA) == "0000000000340102" && Series(tagA) == 0x01, "identification block read without keys: " + IdHex(tagA));
    Check(tagA.compare(0x14, 0x20, plainA, 0x14, 0x20) && tagA.compare(0xA0, 0x168, plainA, 0xA0, 0x168),
          "settings and data area are encrypted on the tag");
    Check(!tagA.compare(0x54, 0x0C, plainA, 0x54, 0x0C) && !tagA.compare(0, 0x10, plainA, 0, 0x10),
          "serial number and identification stay readable");
    std::string back;
    Check(Decode(keys, tagA, back), "decodes with its keys");
    std::string expect = plainA;
    expect.replace(0x34, 32, back.substr(0x34, 32));
    expect.replace(0x80, 32, back.substr(0x80, 32));
    Check(back == expect, "round trip gives the plain tag back (signatures filled in)");
    Check(Encode(keys, back) == tagA, "encoding the decoded tag gives the same bytes");
    Check(TagOrderCheck(keyRaw, tagA, plainA), "an independent tag-order reading (as Ryubing's) verifies and decrypts it");
    Check(!Decode(other, tagA, back), "other keys do not open it");
    for (size_t at : {size_t(0x11), size_t(0x14), size_t(0x40), size_t(0x60), size_t(0x100), size_t(0x200), size_t(0x0)}) {
        std::string bad = tagA;
        bad[at] ^= 1;
        char what[64];
        snprintf(what, sizeof what, "one bit changed at 0x%03zx is caught", at);
        Check(!Decode(keys, bad, back), what);
    }
    {
        std::string bad = tagA;
        bad[0x0C] = 0;
        Check(!LooksLikeTag(bad), "a wrong capability container is not a tag");
        Check(!LooksLikeTag(tagA.substr(0, 539)), "539 bytes is not a tag");
    }

    printf("operations (which bytes of the decrypted tag change)\n");
    Decode(keys, tagA, back);
    const Facts fa = Read(back);
    Check(fa.registered && fa.gameData && fa.nickname == L"Prueba" && fa.owner == L"Jugador" && fa.appId == 0x0100f2c0000bb000ULL &&
              fa.writes == 7 && fa.setUp == L"17/05/2024" && fa.written == L"03/02/2025",
          "facts read: registered, nickname, owner, game, writes, dates");
    {
        std::string p = back;
        Check(SetNickname(p, L"Nuevo", 2026, 11, 26).empty(), "nickname set");
        std::string why;
        const auto ch = Changed(back, p);
        Check(Within(ch, {{0x11, 0x13}, {0x1A, 0x1C}, {0x20, 0x34}}, why), "nickname changes only counter, date, name: " + Ranges(ch) + " " + why);
        Check(Read(p).nickname == L"Nuevo" && Read(p).writes == 8 && Read(p).written == L"26/11/2026", "nickname, counter, date as meant");
        Check(SetNickname(p, L"ElevenChars", 2026, 11, 26) == "BAD_NAME", "eleven characters refused");
    }
    {
        std::string p = back;
        Check(DeleteGameData(p, 2026, 11, 26).empty(), "game data deleted");
        std::string why;
        const auto ch = Changed(back, p);
        Check(Within(ch, {{0x11, 0x13}, {0x14, 0x15}, {0x1A, 0x1C}, {0x100, 0x10F}, {0x12C, 0x130}, {0x130, 0x208}}, why),
              "delete changes only counter, flag, date, game ids, CRC, data area: " + Ranges(ch) + " " + why);
        const Facts f = Read(p);
        Check(!f.gameData && f.registered && f.nickname == L"Prueba", "no game data; owner and nickname kept");
        std::string crc = p.substr(0xA0, 0x60);
        crc += p[0x10E];
        crc += p[0x10F];
        crc += p.substr(0x110, 8);
        crc += p.substr(0x118, 20);
        const uint32_t c = Crc32(crc);
        Check((static_cast<uint8_t>(p[0x12C]) << 24 | static_cast<uint8_t>(p[0x12D]) << 16 | static_cast<uint8_t>(p[0x12E]) << 8 |
               static_cast<uint8_t>(p[0x12F])) == static_cast<int>(c),
              "register-info CRC still right after the game id byte changed");
        Check(DeleteGameData(p, 2026, 11, 26) == "NO_DATA", "deleting again: nothing to delete");
    }
    {
        std::string p = back;
        Check(Format(p, 2026, 11, 26).empty(), "formatted");
        std::string why;
        const auto ch = Changed(back, p);
        Check(Within(ch, {{0x11, 0x13}, {0x14, 0x15}, {0x18, 0x1C}, {0x20, 0x34}, {0xA0, 0x100}, {0x100, 0x10F}, {0x10F, 0x110},
                          {0x118, 0x120}, {0x12C, 0x130}, {0x130, 0x208}},
                     why),
              "format changes only what belongs to owner and data: " + Ranges(ch) + " " + why);
        const Facts f = Read(p);
        Check(!f.registered && !f.gameData, "formatted: unregistered, no data");
        Check(Format(p, 2026, 11, 26) == "BLANK", "formatting a blank tag: nothing to do");
        std::string q;
        Check(Decode(keys, Encode(keys, p), q) && LooksLikeTag(Encode(keys, p)), "the formatted tag encodes, signs and opens again");
        Check(TagOrderCheck(keyRaw, Encode(keys, p), p), "and the independent reading agrees");
    }

    printf("collection, files and the stored keys\n");
    gData = out + L"\\tooldata";
    Mk(gData);
    // Start clean every run.
    for (const Entry& e : List()) Remove(e.file);
    ForgetKeys();
    const std::wstring in = out + L"\\in", keysDir = out + L"\\keys";
    Mk(in);
    Mk(in + L"\\set");
    Mk(in + L"\\set\\sub");
    Mk(keysDir);
    WriteBin(keysDir + L"\\key_retail_fake.bin", keyRaw);
    WriteBin(keysDir + L"\\otras_claves.bin", otherRaw);
    WriteBin(keysDir + L"\\no_son_claves.bin", frc::RandomBytes(160));
    const std::string tagB = Encode(keys, MakePlain("0100000000040002", true, L"Sin datos", L"Jugador", false));
    const std::string tagC = Encode(keys, MakePlain("0008000100430002", false, L"", L"", false));
    const std::string tagD = Encode(keys, MakePlain("00c0000003ab0e02", true, L"Corto", L"Jugador", true));
    const std::string tagE = Encode(keys, MakePlain("0101000000690f02", true, L"Largo", L"Jugador", true));
    WriteBin(in + L"\\Figura de prueba.bin", tagA);
    WriteBin(in + L"\\set\\Sin datos.bin", tagB);
    WriteBin(in + L"\\set\\En blanco.bin", tagC);
    WriteBin(in + L"\\set\\sub\\Corto (532).bin", tagD.substr(0, 532));
    WriteBin(in + L"\\set\\sub\\Largo (572).bin", tagE + frc::RandomBytes(32));
    WriteBin(in + L"\\set\\zz copia.bin", tagB); // the same bytes as another, listed after it
    WriteBin(in + L"\\set\\notas.txt", "nothing to see here");     // not a dump
    WriteBin(in + L"\\set\\roto.bin", tagC.substr(0, 0x0C) + std::string(4, '\0') + tagC.substr(0x10)); // layout wrong
    WriteBin(in + L"\\set\\grande.bin", frc::RandomBytes(2048));
    WriteBin(in + L"\\set\\sub\\otra clave.bin", otherRaw); // 160 bytes
    // The same figure written with other keys: a dump these keys do not open.
    WriteBin(in + L"\\set\\Otra consola.bin", Encode(other, MakePlain("0000000000340102", true, L"Ajena", L"Otro", true)));

    Imported r = ImportFile(in + L"\\Figura de prueba.bin");
    Check(r.added == 1 && r.name == L"Figura de prueba", "one file imported");
    std::string orig;
    ReadBin(in + L"\\Figura de prueba.bin", orig);
    Check(orig == tagA, "the original is left as it was");
    r = ImportFile(in + L"\\Figura de prueba.bin");
    Check(r.added == 0 && r.had == 1, "the same file again: already there");
    r = ImportFile(in + L"\\set\\notas.txt");
    Check(r.added == 0 && r.skipped == 1, "a text file is refused");
    r = ImportFolder(in + L"\\set");
    printf("        folder: %d added, %d skipped, %d already there, %d failed\n", r.added, r.skipped, r.had, r.failed);
    Check(r.added == 5 && r.skipped == 4 && r.had == 1 && r.failed == 0, "folder with subfolders: 5 added, 4 skipped, 1 duplicate");
    std::vector<Entry> list = List();
    Check(list.size() == 6, "six entries in the collection");
    std::wstring shortFile, longFile, aFile, otherFile;
    for (const Entry& e : list) {
        if (e.name == L"Corto (532)") shortFile = e.file;
        if (e.name == L"Largo (572)") longFile = e.file;
        if (e.name == L"Figura de prueba") aFile = e.file;
        if (e.name == L"Otra consola") otherFile = e.file;
    }
    Check(!shortFile.empty() && !longFile.empty() && !aFile.empty(), "entries named after their files");

    Check(Apply(aFile, Op::Nickname, L"Nuevo") == "NO_KEYS", "without keys nothing is changed");
    Check(SetKeys(keysDir + L"\\no_son_claves.bin") == "KEYS_INVALID" && !HaveKeys(), "random bytes are not taken as keys");
    Check(SetKeys(keysDir + L"\\key_retail_fake.bin").empty() && HaveKeys(), "key file stored in the data folder");
    std::string stored;
    ReadBin(KeyFile(), stored);
    Check(stored == keyRaw, "stored copy is the file given");
    Check(Apply(otherFile, Op::Format) == "BAD_KEYS", "a dump the keys do not open is left alone");

    std::string before;
    ReadBin(aFile, before);
    Check(Apply(aFile, Op::Nickname, L"Nuevo").empty(), "nickname written to the entry");
    std::string after, bak;
    ReadBin(aFile, after);
    ReadBin(aFile + L".bak", bak);
    Check(bak == before && after != before && after.size() == 540, ".bak holds the entry as it was");
    Facts f;
    Check(Open(after, f) && f.nickname == L"Nuevo" && f.writes == 8, "entry opens with the new nickname");
    Check(Apply(aFile, Op::DeleteData).empty() && Open((ReadBin(aFile, after), after), f) && !f.gameData, "game data deleted in the entry");
    ReadBin(aFile + L".bak", bak);
    Check(bak == before, ".bak still the first state after a second change");
    Check(Apply(aFile, Op::Format).empty() && Open((ReadBin(aFile, after), after), f) && !f.registered, "entry formatted");
    Check(Apply(aFile, Op::Format) == "BLANK", "formatting again: nothing to do");

    std::string s0, s1;
    ReadBin(shortFile, s0);
    Check(Apply(shortFile, Op::DeleteData).empty(), "a 532-byte dump changed");
    ReadBin(shortFile, s1);
    Check(s1.size() == 532 && Open(s1, f) && !f.gameData, "it stays 532 bytes and opens");
    ReadBin(longFile, s0);
    Check(Apply(longFile, Op::Nickname, L"Otro").empty(), "a 572-byte dump changed");
    ReadBin(longFile, s1);
    Check(s1.size() == 572 && s0.substr(540) == s1.substr(540) && Open(s1, f) && f.nickname == L"Otro",
          "it stays 572 bytes, its last 32 bytes untouched");

    Check(Rename(aFile, L"Mi figura").empty(), "renamed");
    const std::wstring renamed = Dir() + L"\\Mi figura.bin";
    Check(Has(renamed) && Has(renamed + L".bak") && !Has(aFile) && !Has(aFile + L".bak"), "the .bak follows the new name");
    Check(Rename(renamed, L"Sin datos") == "NAME_TAKEN", "a name in use is refused");
    Check(Rename(in + L"\\Figura de prueba.bin", L"x") == "NOT_OURS" && Remove(in + L"\\Figura de prueba.bin") == "NOT_OURS" &&
              Has(in + L"\\Figura de prueba.bin"),
          "nothing outside the collection is renamed or removed");

    // Ryujinx's write-back beside the dump it was given.
    {
        std::string m;
        Decode(keys, tagB, m);
        SetNickname(m, L"Ryu", 2026, 11, 26);
        const std::string mod = Encode(keys, m);
        const std::wstring bFile = Dir() + L"\\Sin datos.bin";
        std::string b0;
        ReadBin(bFile, b0);
        WriteBin(Dir() + L"\\Sin datos_modified.bin", mod);
        list = List();
        std::string b1, bb;
        ReadBin(bFile, b1);
        ReadBin(bFile + L".bak", bb);
        Check(b1 == mod && bb == b0 && !Has(Dir() + L"\\Sin datos_modified.bin") && list.size() == 6,
              "<name>_modified.bin folded into its entry, old one kept as .bak");
    }
    // A 572-byte entry goes to Ryujinx's forks as a 540-byte copy; their
    // write-back beside that copy comes back into the entry, tail kept.
    {
        const std::wstring copy = ForEmulator(longFile, 'r');
        std::string c, l0, l1, m;
        ReadBin(copy, c);
        ReadBin(longFile, l0);
        Check(ForEmulator(longFile, 'y') == longFile && copy != longFile && c.size() == 540 && c == l0.substr(0, 540),
              "Ryujinx family gets a 540-byte copy of a 572-byte entry, the yuzu family the entry");
        Decode(keys, c, m);
        SetNickname(m, L"Vuelta", 2026, 11, 26);
        WriteBin(Dir() + L"\\envio\\Largo (572)_modified.bin", Encode(keys, m));
        List();
        ReadBin(longFile, l1);
        Check(l1.size() == 572 && l1.substr(540) == l0.substr(540) && Open(l1, f) && f.nickname == L"Vuelta" &&
                  !Has(Dir() + L"\\envio\\Largo (572)_modified.bin"),
              "its write-back folded into the 572-byte entry, last 32 bytes kept");
    }
    Check(Remove(renamed).empty() && !Has(renamed) && !Has(renamed + L".bak") && List().size() == 5, "removed with its .bak");
    ForgetKeys();
    Check(!HaveKeys(), "keys forgotten");

    // A large made-up collection for the list's own test (scrolling, groups):
    // 300 dumps over 12 series, names "Figura 001"...
    {
        Mk(out + L"\\many");
        int made = 0;
        for (int i = 0; i < 300; i++) {
            char id[17];
            snprintf(id, sizeof id, "%02x%02x%04x%04x%02x02", i % 7, i % 3, 0x10 + i, 0x100 + i, (i * 5) % 12);
            wchar_t name[40];
            swprintf(name, 40, L"\\many\\Figura %03d.bin", i + 1);
            WriteBin(out + name, Encode(keys, MakePlain(id, i % 4 != 0, L"Fig", L"Jugador", i % 3 == 0)));
            made++;
        }
        Check(made == 300, "300 made-up dumps for the large-list test");
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
