// See parental.h.
#include "parental.h"
#include <cstring>
#include <random>

namespace parental {

namespace {

struct Sha {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[64];
    size_t fill = 0;
    uint64_t bits = 0;
};

uint32_t Ror(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

void Compress(Sha& s) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (static_cast<uint32_t>(s.block[i * 4]) << 24) | (static_cast<uint32_t>(s.block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(s.block[i * 4 + 2]) << 8) | s.block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    memcpy(v, s.h, sizeof v);
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = Ror(v[4], 6) ^ Ror(v[4], 11) ^ Ror(v[4], 25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
        uint32_t s0 = Ror(v[0], 2) ^ Ror(v[0], 13) ^ Ror(v[0], 22);
        uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = s0 + maj;
        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = v[3] + t1;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s.h[i] += v[i];
}

void Feed(Sha& s, const unsigned char* p, size_t n) {
    s.bits += static_cast<uint64_t>(n) * 8;
    for (size_t i = 0; i < n; i++) {
        s.block[s.fill++] = p[i];
        if (s.fill == 64) {
            Compress(s);
            s.fill = 0;
        }
    }
}

void Finish(Sha& s, unsigned char out[32]) {
    const uint64_t bits = s.bits;
    const unsigned char one = 0x80, zero = 0;
    Feed(s, &one, 1);
    while (s.fill != 56) Feed(s, &zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) len[i] = static_cast<unsigned char>(bits >> (56 - i * 8));
    Feed(s, len, 8);
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 4; b++) out[i * 4 + b] = static_cast<unsigned char>(s.h[i] >> (24 - b * 8));
}

std::string Hex(const unsigned char* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; i++) {
        out += digits[p[i] >> 4];
        out += digits[p[i] & 15];
    }
    return out;
}

// Repeated so that trying every PIN against a stolen file takes seconds
// rather than nothing; more would only slow the menu down for no real gain
// (see the header: four digits cannot be protected by hashing).
constexpr int kRounds = 2000;

std::string Derive(const std::string& salt, const std::string& pin) {
    unsigned char digest[32];
    Sha first;
    Feed(first, reinterpret_cast<const unsigned char*>(salt.data()), salt.size());
    Feed(first, reinterpret_cast<const unsigned char*>(pin.data()), pin.size());
    Finish(first, digest);
    for (int i = 1; i < kRounds; i++) {
        Sha s;
        Feed(s, digest, sizeof digest);
        Feed(s, reinterpret_cast<const unsigned char*>(salt.data()), salt.size());
        Finish(s, digest);
    }
    return Hex(digest, sizeof digest);
}

} // namespace

std::string Sha256Hex(const std::string& data) {
    unsigned char digest[32];
    Sha s;
    Feed(s, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    Finish(s, digest);
    return Hex(digest, sizeof digest);
}

std::string MakePin(const std::string& pin) {
    std::random_device rd;
    unsigned char salt[16];
    for (unsigned char& c : salt) c = static_cast<unsigned char>(rd());
    const std::string hex = Hex(salt, sizeof salt);
    return hex + ":" + Derive(hex, pin);
}

bool CheckPin(const std::string& stored, const std::string& pin) {
    const size_t colon = stored.find(':');
    if (colon != 32 || stored.size() != 32 + 1 + 64) return false;
    const std::string want = stored.substr(colon + 1), got = Derive(stored.substr(0, colon), pin);
    // Every digit is compared whatever the outcome, so the time taken says nothing.
    unsigned char diff = 0;
    for (size_t i = 0; i < want.size(); i++) diff |= static_cast<unsigned char>(want[i] ^ got[i]);
    return diff == 0;
}

uint64_t PlayedToday(uint64_t total, uint64_t& base, std::string& baseDay, const std::string& today) {
    if (baseDay != today || total < base) {
        baseDay = today;
        base = total;
    }
    return total - base;
}

} // namespace parental
