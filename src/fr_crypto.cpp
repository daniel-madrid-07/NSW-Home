// See fr_crypto.h. The hashes, HMAC, PBKDF2 and ChaCha20 are written from
// their specifications. The Ed25519 arithmetic is adapted from TweetNaCl
// (version 20140427, by Daniel J. Bernstein, Bernard van Gastel, Wesley
// Janssen, Tanja Lange, Peter Schwabe and Sjaak Smetsers; public domain,
// https://tweetnacl.cr.yp.to): the field and group functions are theirs with
// the names spelled out, the signing interface is reshaped to detached
// signatures over std::string, and verification also refuses a signature
// whose S is not below the group order (RFC 8032, 5.1.7).
#include "fr_crypto.h"
#include <cstring>
#include <random>

namespace frc {
namespace {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

inline u32 Rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
inline u32 Ror(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
inline u64 Ror64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

// ---- SHA-256 (a context, so that HMAC can reuse the keyed states) ----------

const u32 kK256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

struct Sha256Ctx {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u8 buf[64] = {};
    size_t fill = 0;
    u64 total = 0;

    void Block(const u8* p) {
        u32 w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (static_cast<u32>(p[i * 4]) << 24) | (static_cast<u32>(p[i * 4 + 1]) << 16) |
                   (static_cast<u32>(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            u32 s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            u32 s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            u32 t1 = hh + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + kK256[i] + w[i];
            u32 t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void Update(const u8* p, size_t n) {
        total += n;
        while (n) {
            size_t take = 64 - fill < n ? 64 - fill : n;
            memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) {
                Block(buf);
                fill = 0;
            }
        }
    }
    void Final(u8 out[32]) {
        u64 bits = total * 8;
        u8 pad[72] = {0x80};
        size_t padLen = (fill < 56 ? 56 : 120) - fill;
        Update(pad, padLen);
        u8 len[8];
        for (int i = 0; i < 8; i++) len[i] = static_cast<u8>(bits >> (56 - 8 * i));
        Update(len, 8);
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<u8>(h[i] >> (24 - 8 * j));
    }
};

const u8* B(const std::string& s) { return reinterpret_cast<const u8*>(s.data()); }

// ---- SHA-512 ----------------------------------------------------------------

const u64 kK512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

void Sha512Raw(const u8* data, size_t n, u8 out[64]) {
    u64 h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    std::string m(reinterpret_cast<const char*>(data), n);
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 128 != 112) m.push_back(0);
    u64 bits = static_cast<u64>(n) * 8;
    for (int i = 0; i < 8; i++) m.push_back(0); // the length's upper 64 bits
    for (int i = 0; i < 8; i++) m.push_back(static_cast<char>(bits >> (56 - 8 * i)));
    for (size_t off = 0; off < m.size(); off += 128) {
        const u8* p = B(m) + off;
        u64 w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j];
        }
        for (int i = 16; i < 80; i++) {
            u64 s0 = Ror64(w[i - 15], 1) ^ Ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            u64 s1 = Ror64(w[i - 2], 19) ^ Ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u64 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; i++) {
            u64 t1 = hh + (Ror64(e, 14) ^ Ror64(e, 18) ^ Ror64(e, 41)) + ((e & f) ^ (~e & g)) + kK512[i] + w[i];
            u64 t2 = (Ror64(a, 28) ^ Ror64(a, 34) ^ Ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) out[i * 8 + j] = static_cast<u8>(h[i] >> (56 - 8 * j));
}

// ---- Ed25519 (TweetNaCl) ------------------------------------------------------

typedef i64 gf[16];
const gf gf0 = {0};
const gf gf1 = {1};
const gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
               0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const gf kX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
               0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const gf kY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
               0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const gf kI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
               0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
// The group order, least significant byte first.
const u64 kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

void Set(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}
void Carry(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
void Select(gf p, gf q, int b) {
    i64 c = ~(static_cast<i64>(b) - 1);
    for (int i = 0; i < 16; i++) {
        i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}
void Pack(u8* o, const gf n) {
    gf m, t;
    Set(t, n);
    Carry(t);
    Carry(t);
    Carry(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        Select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = static_cast<u8>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<u8>(t[i] >> 8);
    }
}
bool Differ(const u8* a, const u8* b, size_t n) {
    u32 d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d != 0;
}
bool Neq(const gf a, const gf b) {
    u8 c[32], d[32];
    Pack(c, a);
    Pack(d, b);
    return Differ(c, d, 32);
}
u8 Parity(const gf a) {
    u8 d[32];
    Pack(d, a);
    return d[0] & 1;
}
void Unpack(gf o, const u8* n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + (static_cast<i64>(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}
void Add(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}
void Sub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}
void Mul(gf o, const gf a, const gf b) {
    i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    Carry(o);
    Carry(o);
}
void Sqr(gf o, const gf a) {
    Mul(o, a, a);
}
void Invert(gf o, const gf in) {
    gf c;
    Set(c, in);
    for (int a = 253; a >= 0; a--) {
        Sqr(c, c);
        if (a != 2 && a != 4) Mul(c, c, in);
    }
    Set(o, c);
}
void Pow2523(gf o, const gf in) {
    gf c;
    Set(c, in);
    for (int a = 250; a >= 0; a--) {
        Sqr(c, c);
        if (a != 1) Mul(c, c, in);
    }
    Set(o, c);
}
void PointAdd(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Sub(a, p[1], p[0]);
    Sub(t, q[1], q[0]);
    Mul(a, a, t);
    Add(b, p[0], p[1]);
    Add(t, q[0], q[1]);
    Mul(b, b, t);
    Mul(c, p[3], q[3]);
    Mul(c, c, kD2);
    Mul(d, p[2], q[2]);
    Add(d, d, d);
    Sub(e, b, a);
    Sub(f, d, c);
    Add(g, d, c);
    Add(h, b, a);
    Mul(p[0], e, f);
    Mul(p[1], h, g);
    Mul(p[2], g, f);
    Mul(p[3], e, h);
}
void PointSwap(gf p[4], gf q[4], u8 b) {
    for (int i = 0; i < 4; i++) Select(p[i], q[i], b);
}
void PointPack(u8* r, gf p[4]) {
    gf tx, ty, zi;
    Invert(zi, p[2]);
    Mul(tx, p[0], zi);
    Mul(ty, p[1], zi);
    Pack(r, ty);
    r[31] ^= static_cast<u8>(Parity(tx) << 7);
}
void ScalarMult(gf p[4], gf q[4], const u8* s) {
    Set(p[0], gf0);
    Set(p[1], gf1);
    Set(p[2], gf1);
    Set(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        u8 b = (s[i / 8] >> (i & 7)) & 1;
        PointSwap(p, q, b);
        PointAdd(q, p);
        PointAdd(p, p);
        PointSwap(p, q, b);
    }
}
void ScalarBase(gf p[4], const u8* s) {
    gf q[4];
    Set(q[0], kX);
    Set(q[1], kY);
    Set(q[2], gf1);
    Mul(q[3], kX, kY);
    ScalarMult(p, q, s);
}
void ModL(u8* r, i64 x[64]) {
    i64 carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        int j;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * static_cast<i64>(kL[j - (i - 32)]);
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * static_cast<i64>(kL[j]);
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= carry * static_cast<i64>(kL[j]);
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<u8>(x[i] & 255);
    }
}
void Reduce(u8* r) {
    i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = static_cast<i64>(r[i]);
    for (int i = 0; i < 64; i++) r[i] = 0;
    ModL(r, x);
}
bool UnpackNeg(gf r[4], const u8 p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    Set(r[2], gf1);
    Unpack(r[1], p);
    Sqr(num, r[1]);
    Mul(den, num, kD);
    Sub(num, num, r[2]);
    Add(den, r[2], den);
    Sqr(den2, den);
    Sqr(den4, den2);
    Mul(den6, den4, den2);
    Mul(t, den6, num);
    Mul(t, t, den);
    Pow2523(t, t);
    Mul(t, t, num);
    Mul(t, t, den);
    Mul(t, t, den);
    Mul(r[0], t, den);
    Sqr(chk, r[0]);
    Mul(chk, chk, den);
    if (Neq(chk, num)) Mul(r[0], r[0], kI);
    Sqr(chk, r[0]);
    Mul(chk, chk, den);
    if (Neq(chk, num)) return false;
    if (Parity(r[0]) == (p[31] >> 7)) Sub(r[0], gf0, r[0]);
    Mul(r[3], r[0], r[1]);
    return true;
}
// The secret scalar and the nonce prefix of a seed.
void Expand(const std::string& seed, u8 d[64]) {
    Sha512Raw(B(seed), seed.size(), d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

// X25519 (RFC 7748): TweetNaCl's crypto_scalarmult, the Montgomery ladder
// over the same field arithmetic as the signatures.
const gf k121665 = {0xDB41, 1};
void ScalarMultX(u8* q, const u8* n, const u8* p) {
    u8 z[32];
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = static_cast<u8>((n[31] & 127) | 64);
    z[0] &= 248;
    gf x, a, b, c, d, e, f;
    Unpack(x, p); // (clears the top bit of u, as RFC 7748 5 asks)
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        Select(a, b, r);
        Select(c, d, r);
        Add(e, a, c);
        Sub(a, a, c);
        Add(c, b, d);
        Sub(b, b, d);
        Sqr(d, e);
        Sqr(f, a);
        Mul(a, c, a);
        Mul(c, b, e);
        Add(e, a, c);
        Sub(a, a, c);
        Sqr(b, a);
        Sub(c, d, f);
        Mul(a, c, k121665);
        Add(a, a, d);
        Mul(c, c, a);
        Mul(a, d, f);
        Mul(d, b, x);
        Sqr(b, e);
        Select(a, b, r);
        Select(c, d, r);
    }
    Invert(c, c);
    Mul(a, a, c);
    Pack(q, a);
}

inline u32 Le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

} // namespace

std::string Sha1(const std::string& data) {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 64 != 56) m.push_back(0);
    u64 bits = static_cast<u64>(data.size()) * 8;
    for (int i = 0; i < 8; i++) m.push_back(static_cast<char>(bits >> (56 - 8 * i)));
    for (size_t off = 0; off < m.size(); off += 64) {
        const u8* p = B(m) + off;
        u32 w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (static_cast<u32>(p[i * 4]) << 24) | (static_cast<u32>(p[i * 4 + 1]) << 16) |
                   (static_cast<u32>(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            u32 f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            u32 t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out(20, 0);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<char>(h[i] >> (24 - 8 * j));
    return out;
}

std::string Sha256(const std::string& data) {
    Sha256Ctx c;
    c.Update(B(data), data.size());
    u8 out[32];
    c.Final(out);
    return std::string(reinterpret_cast<char*>(out), 32);
}

std::string Sha512(const std::string& data) {
    u8 out[64];
    Sha512Raw(B(data), data.size(), out);
    return std::string(reinterpret_cast<char*>(out), 64);
}

namespace {
// The two keyed states of HMAC, made once and copied for every message.
struct Hmac {
    Sha256Ctx inner, outer;
    explicit Hmac(const std::string& keyIn) {
        std::string key = keyIn.size() > 64 ? Sha256(keyIn) : keyIn;
        u8 ipad[64], opad[64];
        for (size_t i = 0; i < 64; i++) {
            u8 k = i < key.size() ? static_cast<u8>(key[i]) : 0;
            ipad[i] = k ^ 0x36;
            opad[i] = k ^ 0x5c;
        }
        inner.Update(ipad, 64);
        outer.Update(opad, 64);
    }
    void Mac(const u8* data, size_t n, u8 out[32]) const {
        Sha256Ctx a = inner;
        a.Update(data, n);
        u8 mid[32];
        a.Final(mid);
        Sha256Ctx b = outer;
        b.Update(mid, 32);
        b.Final(out);
    }
};
} // namespace

std::string HmacSha256(const std::string& key, const std::string& data) {
    u8 out[32];
    Hmac(key).Mac(B(data), data.size(), out);
    return std::string(reinterpret_cast<char*>(out), 32);
}

std::string Pbkdf2Sha256(const std::string& password, const std::string& salt, uint32_t iterations) {
    Hmac h(password);
    std::string first = salt + std::string("\0\0\0\1", 4);
    u8 u[32], t[32];
    h.Mac(B(first), first.size(), u);
    memcpy(t, u, 32);
    for (uint32_t i = 1; i < iterations; i++) {
        h.Mac(u, 32, u);
        for (int j = 0; j < 32; j++) t[j] ^= u[j];
    }
    return std::string(reinterpret_cast<char*>(t), 32);
}

std::string ChaCha20(const std::string& key, const std::string& nonce, uint32_t counter, const std::string& data) {
    if (key.size() != 32 || nonce.size() != 12) return {};
    u32 st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) st[4 + i] = Le32(B(key) + i * 4);
    st[12] = counter;
    for (int i = 0; i < 3; i++) st[13 + i] = Le32(B(nonce) + i * 4);
    std::string out = data;
    for (size_t off = 0; off < data.size(); off += 64) {
        u32 x[16];
        memcpy(x, st, sizeof x);
        auto quarter = [&x](int a, int b, int c, int d) {
            x[a] += x[b]; x[d] = Rol(x[d] ^ x[a], 16);
            x[c] += x[d]; x[b] = Rol(x[b] ^ x[c], 12);
            x[a] += x[b]; x[d] = Rol(x[d] ^ x[a], 8);
            x[c] += x[d]; x[b] = Rol(x[b] ^ x[c], 7);
        };
        for (int round = 0; round < 10; round++) {
            quarter(0, 4, 8, 12); quarter(1, 5, 9, 13); quarter(2, 6, 10, 14); quarter(3, 7, 11, 15);
            quarter(0, 5, 10, 15); quarter(1, 6, 11, 12); quarter(2, 7, 8, 13); quarter(3, 4, 9, 14);
        }
        for (size_t i = 0; i < 64 && off + i < data.size(); i++) {
            u32 word = x[i / 4] + st[i / 4];
            out[off + i] = static_cast<char>(static_cast<u8>(data[off + i]) ^ static_cast<u8>(word >> (8 * (i % 4))));
        }
        st[12]++;
    }
    return out;
}

std::string Ed25519Public(const std::string& seed) {
    if (seed.size() != 32) return {};
    u8 d[64], pk[32];
    gf p[4];
    Expand(seed, d);
    ScalarBase(p, d);
    PointPack(pk, p);
    return std::string(reinterpret_cast<char*>(pk), 32);
}

std::string Ed25519Sign(const std::string& seed, const std::string& pub, const std::string& msg) {
    if (seed.size() != 32 || pub.size() != 32) return {};
    u8 d[64], r[64], hram[64], sig[64];
    gf p[4];
    Expand(seed, d);
    std::string buf(reinterpret_cast<char*>(d) + 32, 32);
    buf += msg;
    Sha512Raw(B(buf), buf.size(), r);
    Reduce(r);
    ScalarBase(p, r);
    PointPack(sig, p);
    buf.assign(reinterpret_cast<char*>(sig), 32);
    buf += pub;
    buf += msg;
    Sha512Raw(B(buf), buf.size(), hram);
    Reduce(hram);
    i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = static_cast<i64>(r[i]);
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += static_cast<i64>(hram[i]) * static_cast<i64>(d[j]);
    ModL(sig + 32, x);
    return std::string(reinterpret_cast<char*>(sig), 64);
}

bool Ed25519Verify(const std::string& pub, const std::string& msg, const std::string& sig) {
    if (pub.size() != 32 || sig.size() != 64) return false;
    const u8* s = B(sig) + 32;
    // S must be below the group order (compared from the most significant byte).
    for (int i = 31; i >= 0; i--) {
        if (s[i] < kL[i]) break;
        if (s[i] > kL[i] || i == 0) return false;
    }
    gf p[4], q[4];
    if (!UnpackNeg(q, B(pub))) return false;
    std::string buf = sig.substr(0, 32) + pub + msg;
    u8 h[64], t[32];
    Sha512Raw(B(buf), buf.size(), h);
    Reduce(h);
    ScalarMult(p, q, h);
    ScalarBase(q, s);
    PointAdd(p, q);
    PointPack(t, p);
    return !Differ(B(sig), t, 32);
}

std::string X25519(const std::string& scalar, const std::string& u) {
    if (scalar.size() != 32 || u.size() != 32) return {};
    u8 q[32];
    ScalarMultX(q, B(scalar), B(u));
    return std::string(reinterpret_cast<char*>(q), 32);
}

std::string X25519Base(const std::string& scalar) {
    return X25519(scalar, std::string("\x09", 1) + std::string(31, '\0'));
}

std::string Hkdf(const std::string& salt, const std::string& ikm, const std::string& info, size_t length) {
    // RFC 5869: extract, then expand; an empty salt is HashLen zeros.
    const std::string prk = HmacSha256(salt.empty() ? std::string(32, '\0') : salt, ikm);
    std::string out, t;
    for (unsigned char i = 1; out.size() < length && i != 0; i++) {
        t = HmacSha256(prk, t + info + std::string(1, static_cast<char>(i)));
        out += t;
    }
    out.resize(length < 255 * 32 ? length : 255 * 32);
    return out;
}

bool SameBytes(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return !Differ(B(a), B(b), a.size());
}

std::string RandomBytes(size_t n) {
    // The system's generator on every platform NSW-Home builds for (rand_s on
    // Windows, the kernel's on Linux, macOS and Android).
    static std::random_device rd;
    std::string out(n, 0);
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = rd();
        for (size_t j = 0; j < 4 && i + j < n; j++) out[i + j] = static_cast<char>(v >> (8 * j));
    }
    return out;
}

std::string Hex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

std::string Unhex(const std::string& hex) {
    auto val = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    if (hex.size() % 2) return {};
    std::string out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        int a = val(hex[i]), b = val(hex[i + 1]);
        if (a < 0 || b < 0) return {};
        out += static_cast<char>(a * 16 + b);
    }
    return out;
}

} // namespace frc
