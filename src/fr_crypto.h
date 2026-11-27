// Hashes, signatures and the stream cipher the friends module needs. NSW-Home
// links no third-party library, so they are here: SHA-1, SHA-256 and SHA-512
// (FIPS 180-4), HMAC-SHA-256 and PBKDF2 (RFC 2104, RFC 8018), ChaCha20
// (RFC 8439) and Ed25519 (RFC 8032). tools/friends_test.cpp checks each
// against the published vectors. Nothing here depends on the system.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace frc {

// Byte strings are std::string throughout the friends module.
std::string Sha1(const std::string& data);   // 20 bytes
std::string Sha256(const std::string& data); // 32 bytes
std::string Sha512(const std::string& data); // 64 bytes
std::string HmacSha256(const std::string& key, const std::string& data);
// One output block (32 bytes) of PBKDF2-HMAC-SHA-256.
std::string Pbkdf2Sha256(const std::string& password, const std::string& salt, uint32_t iterations);

// ChaCha20 with a 32-byte key, a 12-byte nonce and the block counter starting
// at `counter`. Encrypting and decrypting are the same operation.
std::string ChaCha20(const std::string& key, const std::string& nonce, uint32_t counter, const std::string& data);

// Ed25519. `seed` is the 32-byte private key of RFC 8032.
std::string Ed25519Public(const std::string& seed);                                           // 32 bytes
std::string Ed25519Sign(const std::string& seed, const std::string& pub, const std::string& msg); // 64 bytes
bool Ed25519Verify(const std::string& pub, const std::string& msg, const std::string& sig);

// X25519 (RFC 7748): Diffie-Hellman on Curve25519. 32-byte scalar and u
// coordinate; the scalar is clamped here.
std::string X25519(const std::string& scalar, const std::string& u);
std::string X25519Base(const std::string& scalar); // the public key: u = 9

// HKDF-SHA-256 (RFC 5869), at most 8160 bytes.
std::string Hkdf(const std::string& salt, const std::string& ikm, const std::string& info, size_t length);

// Compares in a time that does not depend on where the strings differ.
bool SameBytes(const std::string& a, const std::string& b);

// From the system's random source.
std::string RandomBytes(size_t n);

std::string Hex(const std::string& bytes);
std::string Unhex(const std::string& hex); // "" if it is not hex

} // namespace frc
