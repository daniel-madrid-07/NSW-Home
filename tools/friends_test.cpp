// Checks the friends module's building blocks against published vectors and
// its own item format, without any network:
//   SHA-1 / SHA-256 / SHA-512   FIPS 180-4 examples (and the million-'a' ones)
//   HMAC-SHA-256                RFC 4231, cases 1 and 2
//   PBKDF2-HMAC-SHA-256         the "password"/"salt" vectors (1, 2, 4096 rounds)
//   ChaCha20                    RFC 8439, 2.4.2
//   Ed25519                     RFC 8032, 7.1 (tests 1, 2, 3 and SHA(abc))
//   X25519                      RFC 7748, 5.2 (both vectors, 1 and 1000 iterations) and 6.1
//   HKDF-SHA-256                RFC 5869, A.1 and A.3
//   BEP 44                      the specification's two mutable-item examples
//   bencoding; the card, request, note and status formats and their forgeries;
//   and the whole protocol between several users of one module over a network
//   inside the process: request, accept, both see each other, removal,
//   decline, withdrawal, two requests at once, someone who only has the code,
//   and a version 1 friends.dat.
//
// Build and run: tools\build_friends.bat   (or, anywhere:
//   g++ -std=c++17 -Wall -Wextra -O2 tools/friends_test.cpp src/fr_crypto.cpp src/fr_dht.cpp src/fr_net.cpp src/friends.cpp -lpthread)
#define _CRT_SECURE_NO_WARNINGS // (fopen and getenv: a test, not the program)
#include "../src/fr_bencode.h"
#include "../src/fr_crypto.h"
#include "../src/fr_dht.h"
#include "../src/friends.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

namespace {

int passed = 0, failed = 0;

void Check(bool ok, const std::string& what) {
    if (ok) passed++;
    else {
        failed++;
        printf("  FAIL  %s\n", what.c_str());
    }
}
void Eq(const std::string& got, const std::string& want, const std::string& what) {
    Check(got == want, what + "\n        got  " + got + "\n        want " + want);
}
std::string H(const std::string& hex) {
    return frc::Unhex(hex);
}

void Hashes() {
    using namespace frc;
    const std::string m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const std::string m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    const std::string million(1000000, 'a');
    Eq(Hex(Sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 abc");
    Eq(Hex(Sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 empty");
    Eq(Hex(Sha1(m448)), "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "SHA-1 448 bits");
    Eq(Hex(Sha1(million)), "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "SHA-1 million a");
    Eq(Hex(Sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc");
    Eq(Hex(Sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty");
    Eq(Hex(Sha256(m448)), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 448 bits");
    Eq(Hex(Sha256(million)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 million a");
    Eq(Hex(Sha512("abc")),
       "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
       "SHA-512 abc");
    Eq(Hex(Sha512("")),
       "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
       "SHA-512 empty");
    Eq(Hex(Sha512(m896)),
       "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
       "SHA-512 896 bits");
    Eq(Hex(Sha512(million)),
       "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
       "SHA-512 million a");

    Eq(Hex(HmacSha256(std::string(20, '\x0b'), "Hi There")), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
       "HMAC-SHA-256 RFC 4231 case 1");
    Eq(Hex(HmacSha256("Jefe", "what do ya want for nothing?")), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
       "HMAC-SHA-256 RFC 4231 case 2");
    Eq(Hex(Pbkdf2Sha256("password", "salt", 1)), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b", "PBKDF2 1 round");
    Eq(Hex(Pbkdf2Sha256("password", "salt", 2)), "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43", "PBKDF2 2 rounds");
    Eq(Hex(Pbkdf2Sha256("password", "salt", 4096)), "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a", "PBKDF2 4096 rounds");
}

void Cipher() {
    using namespace frc;
    std::string key = H("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    std::string nonce = H("000000000000004a00000000");
    std::string text = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    std::string want = "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab"
                       "8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42"
                       "874d";
    Eq(Hex(ChaCha20(key, nonce, 1, text)), want, "ChaCha20 RFC 8439 2.4.2");
    Eq(ChaCha20(key, nonce, 1, ChaCha20(key, nonce, 1, text)), text, "ChaCha20 back again");
}

void Signatures() {
    using namespace frc;
    struct V {
        const char *seed, *pub, *msg, *sig;
    } v[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
        {"833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42", "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
         "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"},
    };
    int n = 0;
    for (const V& t : v) {
        std::string name = "Ed25519 RFC 8032 vector " + std::to_string(++n);
        std::string seed = H(t.seed), pub = H(t.pub), msg = H(t.msg), sig = H(t.sig);
        Eq(Hex(Ed25519Public(seed)), t.pub, name + " public key");
        Eq(Hex(Ed25519Sign(seed, pub, msg)), t.sig, name + " signature");
        Check(Ed25519Verify(pub, msg, sig), name + " verifies");
        Check(!Ed25519Verify(pub, msg + "x", sig), name + " other message refused");
        std::string bad = sig;
        bad[10] ^= 1;
        Check(!Ed25519Verify(pub, msg, bad), name + " changed R refused");
        bad = sig;
        bad[40] ^= 1;
        Check(!Ed25519Verify(pub, msg, bad), name + " changed S refused");
        bad = pub;
        bad[3] ^= 4;
        Check(!Ed25519Verify(bad, msg, sig), name + " other key refused");
    }
    // S + L is the same point but not a canonical signature.
    {
        std::string pub = H(v[0].pub), sig = H(v[0].sig);
        static const unsigned char L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                            0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
        unsigned carry = 0;
        for (int i = 0; i < 32; i++) {
            unsigned sum = static_cast<unsigned char>(sig[32 + i]) + L[i] + carry;
            sig[32 + i] = static_cast<char>(sum & 255);
            carry = sum >> 8;
        }
        Check(!Ed25519Verify(pub, "", sig), "Ed25519 S + L refused");
    }
}

void Bep44() {
    // The examples of BEP 44 ("Test vectors", mutable items).
    std::string pub = H("77ff84905a91936367c01360803104f92432fcd904a43511876df5cdf3e7e548");
    std::string value = "Hello World!";
    Eq(dht::SignBuffer("", 1, value), "3:seqi1e1:v12:Hello World!", "BEP 44 buffer without salt");
    Eq(dht::SignBuffer("foobar", 1, value), "4:salt6:foobar3:seqi1e1:v12:Hello World!", "BEP 44 buffer with salt");
    Eq(frc::Hex(dht::Target(pub, "")), "4a533d47ec9c7d95b1ad75f576cffc641853b750", "BEP 44 target without salt");
    Eq(frc::Hex(dht::Target(pub, "foobar")), "411eba73b6f087ca51a3795d9c8c938d365e32c1", "BEP 44 target with salt");
    const char* sigPlain = "305ac8aeb6c9c151fa120f120ea2cfb923564e11552d06a5d856091e5e853cff1260d3f39e4999684aa92eb73ffd136e6f4f3ecbfda0ce53a1608ecd7ae21f01";
    const char* sigSalt = "6834284b6b24c3204eb2fea824d82f88883a3d95e8b4a21b8c0ded553d17d17ddf9a8a7104b1258f30bed3787e6cb896fca78c58f8e03b5f18f14951a87d9a08";
    Check(frc::Ed25519Verify(pub, dht::SignBuffer("", 1, value), H(sigPlain)), "BEP 44 signature without salt verifies");
    Check(frc::Ed25519Verify(pub, dht::SignBuffer("foobar", 1, value), H(sigSalt)), "BEP 44 signature with salt verifies");
    Check(!frc::Ed25519Verify(pub, dht::SignBuffer("foobar", 2, value), H(sigSalt)), "BEP 44 signature refused for another sequence number");
}

void Bencode() {
    ben::Val d = ben::Val::MakeDict();
    d.Set("y", "q").Set("t", "aa").Set("q", "ping");
    ben::Val a = ben::Val::MakeDict();
    a.Set("id", "abcdefghij0123456789");
    d.Set("a", a);
    Eq(ben::Encode(d), "d1:ad2:id20:abcdefghij0123456789e1:q4:ping1:t2:aa1:y1:qe", "bencode: BEP 5's ping, keys sorted");
    ben::Val back;
    Check(ben::Decode("d1:ad2:id20:abcdefghij0123456789e1:q4:ping1:t2:aa1:y1:qe", back), "bencode: decodes");
    Eq(back.Str_("q"), "ping", "bencode: string read back");
    Check(back.Get("a") && back.Get("a")->Str_("id") == "abcdefghij0123456789", "bencode: nested dictionary");
    ben::Val l;
    std::string bin("li-42ei0e3:\0\1\2lee", 17);
    Check(ben::Decode(bin, l) && l.list.size() == 4 && l.list[0].i == -42 && l.list[2].s == std::string("\0\1\2", 3), "bencode: list, negative, binary");
    Check(l.list[2].rawBegin == 9 && l.list[2].rawEnd == 14, "bencode: where a value stood");
    Eq(ben::Encode(l), bin, "bencode: encodes back to the same bytes");
    for (const char* bad : {"", "i12", "ie", "i-e", "i1x2e", "5:abc", "l", "d1:a", "di1e1:ae", "x", "i99999999999999999999999e", "1:ab", "d1:a1:bee"})
        Check(!ben::Decode(bad, l), std::string("bencode: refuses \"") + bad + "\"");
    std::string deep(100, 'l');
    deep += std::string(100, 'e');
    Check(!ben::Decode(deep, l), "bencode: refuses 100 nested lists");
}

void KeyAgreement() {
    using namespace frc;
    // RFC 7748, 5.2.
    Eq(Hex(X25519(H("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                  H("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"))),
       "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", "X25519 RFC 7748 5.2 vector 1");
    Eq(Hex(X25519(H("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
                  H("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"))),
       "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", "X25519 RFC 7748 5.2 vector 2 (top bit of u set)");
    {
        std::string k = H("0900000000000000000000000000000000000000000000000000000000000000"), u = k;
        for (int i = 1; i <= 1000; i++) {
            std::string r = X25519(k, u);
            u = k;
            k = r;
            if (i == 1) Eq(Hex(k), "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", "X25519 RFC 7748 5.2 one iteration");
        }
        Eq(Hex(k), "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", "X25519 RFC 7748 5.2 1000 iterations");
    }
    // RFC 7748, 6.1.
    std::string a = H("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    std::string b = H("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    Eq(Hex(X25519Base(a)), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", "X25519 RFC 7748 6.1 Alice's public key");
    Eq(Hex(X25519Base(b)), "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", "X25519 RFC 7748 6.1 Bob's public key");
    Eq(Hex(X25519(a, X25519Base(b))), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", "X25519 RFC 7748 6.1 shared secret (Alice)");
    Eq(Hex(X25519(b, X25519Base(a))), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", "X25519 RFC 7748 6.1 shared secret (Bob)");
    Eq(Hex(X25519(a, std::string(32, '\0'))), std::string(64, '0'), "X25519 with the zero point gives zero (refused by the module)");

    // RFC 5869, A.1 and A.3.
    Eq(Hex(Hkdf(H("000102030405060708090a0b0c"), std::string(22, '\x0b'), H("f0f1f2f3f4f5f6f7f8f9"), 42)),
       "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", "HKDF RFC 5869 A.1");
    Eq(Hex(Hkdf("", std::string(22, '\x0b'), "", 42)),
       "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", "HKDF RFC 5869 A.3 (no salt, no info)");
    Check(SameBytes("abc", "abc") && !SameBytes("abc", "abd") && !SameBytes("abc", "ab"), "bytes compared");
}

// The module's sealing spelled out again (encrypt-then-MAC: ChaCha20, then
// HMAC-SHA-256 cut to 16 bytes, keys by HKDF), to build forgeries with.
std::string SealLike(const std::string& material, const char* label, const std::string& nonce, const std::string& plain) {
    std::string k = frc::Hkdf("", material, std::string("dockside-v2 ") + label, 64);
    std::string ct = frc::ChaCha20(k.substr(0, 32), nonce, 1, plain);
    return nonce + ct + frc::HmacSha256(k.substr(32), nonce + ct).substr(0, 16);
}
std::string UnsealLike(const std::string& material, const char* label, const std::string& sealed) {
    std::string k = frc::Hkdf("", material, std::string("dockside-v2 ") + label, 64);
    return frc::ChaCha20(k.substr(0, 32), sealed.substr(0, 12), 1, sealed.substr(12, sealed.size() - 28));
}

struct Person {
    std::string seed, pub, dhSecret, dh, code;
    friends::Card card;
};
Person MakePerson(const std::string& name, const std::string& nick, int colour) {
    Person p;
    p.seed = frc::Sha256("identity " + name);
    p.pub = frc::Ed25519Public(p.seed);
    p.dhSecret = frc::Sha256("dh " + name);
    p.dh = frc::X25519Base(p.dhSecret);
    p.code = friends::CodeOf(p.pub);
    p.card.code = p.code;
    p.card.nick = nick;
    p.card.colour = colour;
    p.card.identity = p.pub;
    p.card.dh = p.dh;
    return p;
}

void Formats() {
    using namespace friends;
    Check(ValidCode("123456789012") && !ValidCode("12345678901") && !ValidCode("12345678901a"), "friend code: twelve digits");
    auto t0 = std::chrono::steady_clock::now();
    Person A = MakePerson("A", "Prueba A", 3);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("  an identity's code took %.0f ms\n", ms);
    Person B = MakePerson("B", "Prueba B", 5), C = MakePerson("C", "Prueba C", 1), X = MakePerson("X", "Impostor", 3);
    Check(ValidCode(A.code) && A.code == CodeOf(A.pub) && A.code != B.code, "code: twelve digits, made from the identity, one per identity");
    t0 = std::chrono::steady_clock::now();
    Keys kA = DeriveKeys(A.code);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("  a code's keys took %.0f ms\n", ms);
    Keys kB = DeriveKeys(B.code);
    Check(kA.pub.size() == 32 && kA.pub != kB.pub && kA.seal != kB.seal && DeriveKeys(A.code).pub == kA.pub, "keys: one set per code");
    Check(InboxSalt(20000, 3) == "rq20000.3" && DayOf(86399) == 0 && DayOf(86400) == 1, "inbox: four places a day");

    // ---- the card
    std::string card = SealCard(kA, A.seed, A.card, true);
    Card got;
    bool open = false;
    Check(OpenCard(kA, A.code, card, got, open) && open && got.nick == "Prueba A" && got.colour == 3 && got.identity == A.pub &&
              got.dh == A.dh && got.code == A.code,
          "card: opens with the code, every field back");
    Check(SealCard(kA, A.seed, A.card, true) == card, "card: the same card seals to the same bytes (kept alive without a visible change)");
    Check(card.find("Prueba") == std::string::npos && card.find(A.pub) == std::string::npos, "card: nothing readable without the code");
    Check(!OpenCard(kB, B.code, card, got, open), "card: does not open under another code");
    std::string closed = SealCard(kA, A.seed, A.card, false);
    Check(OpenCard(kA, A.code, closed, got, open) && !open && got.nick.empty(), "card: closed (sharing off) carries no nickname");
    for (size_t at : {size_t{0}, size_t{5}, size_t{40}, card.size() - 1}) {
        std::string bad = card;
        bad[at] ^= 0x10;
        Check(!OpenCard(kA, A.code, bad, got, open), "card: byte " + std::to_string(at) + " changed is refused");
    }
    // Someone with A's code writes a card of their own in its place.
    Card fake = X.card;
    fake.code = A.code;
    std::string forged = SealCard(kA, X.seed, fake, true);
    Check(OpenCard(kA, A.code, forged, got, open, false) && got.identity == X.pub, "forged card: opens (they have the code) and names their identity...");
    Check(!OpenCard(kA, A.code, forged, got, open), "forged card: ...which does not make the code: refused");
    {
        // ...nor can they name A's identity: the signature is not A's.
        std::string plain = UnsealLike(kA.seal, "card", forged.substr(1));
        plain.replace(0, 32, A.pub);
        std::string claimed = std::string(1, 2) + SealLike(kA.seal, "card", forged.substr(1, 12), plain);
        Check(!OpenCard(kA, A.code, claimed, got, open), "forged card: A's identity with another's signature is refused");
    }

    // ---- requests
    Envelope e;
    e.kind = 1;
    e.from = A.card;
    e.time = 1790000000;
    std::string req = SealRequest(B.card, A.seed, e, frc::Sha256("ephemeral 1"));
    Envelope out;
    Check(OpenRequest(B.pub, B.dhSecret, B.dh, req, out) && out.kind == 1 && out.from.code == A.code && out.from.nick == "Prueba A" &&
              out.from.colour == 3 && out.from.identity == A.pub && out.from.dh == A.dh && out.time == e.time,
          "request: the recipient opens it, every field back");
    Check(CodeOf(out.from.identity) == out.from.code, "request: the asker's identity makes the code it gives");
    Check(req.size() <= 800, "request: at most 800 bytes (" + std::to_string(req.size()) + ")");
    Check(req.find("Prueba") == std::string::npos && req.find(A.pub) == std::string::npos && req.find(A.code) == std::string::npos &&
              req.find(A.dh) == std::string::npos,
          "request: says nothing of who sent it to anyone else");
    Check(SealRequest(B.card, A.seed, e, frc::Sha256("ephemeral 2")) != req, "request: two copies do not look alike");
    // A third party who has B's code (and so B's card and inbox keys) cannot read it.
    Check(!OpenRequest(X.pub, X.dhSecret, X.dh, req, out), "request: someone with the recipient's code cannot open it");
    Check(!OpenRequest(B.pub, X.dhSecret, B.dh, req, out), "request: nor with the recipient's public keys and another secret");
    for (size_t at : {size_t{0}, size_t{10}, size_t{40}, size_t{90}, req.size() - 1}) {
        std::string bad = req;
        bad[at] ^= 0x04;
        Check(!OpenRequest(B.pub, B.dhSecret, B.dh, bad, out), "request: byte " + std::to_string(at) + " changed is refused");
    }
    // A third party makes a request "from A": A's identity, their own signature.
    {
        ben::Val d = ben::Val::MakeDict();
        d.Set("v", 2).Set("k", 1).Set("c", A.code).Set("x", X.dh).Set("t", int64_t{1790000000}).Set("n", "Prueba A").Set("o", 3);
        std::string body = ben::Encode(d);
        std::string plain = A.pub + frc::Ed25519Sign(X.seed, X.pub, "dockside-request-v2" + B.pub + body) + body;
        std::string eph = frc::Sha256("ephemeral 3"), ePub = frc::X25519Base(eph), ss = frc::X25519(eph, B.dh);
        std::string v = std::string(1, 2) + ePub + SealLike(frc::Sha256(ss + ePub + B.dh), "request", std::string(12, '\0'), plain);
        Check(!OpenRequest(B.pub, B.dhSecret, B.dh, v, out), "forged request: A's identity without A's key is refused");
    }
    // Their own identity with A's code: it opens, but the identity does not make the code.
    {
        Envelope x = e;
        x.from = X.card;
        x.from.code = A.code;
        std::string v = SealRequest(B.card, X.seed, x, frc::Sha256("ephemeral 4"));
        Check(OpenRequest(B.pub, B.dhSecret, B.dh, v, out) && CodeOf(out.from.identity) != out.from.code,
              "forged request: another identity under A's code is told apart by the code");
    }
    // B passes A's request on to C (or a node replays it there): signed for B only.
    {
        std::string ePub = req.substr(1, 32), ss = frc::X25519(B.dhSecret, ePub);
        std::string plain = UnsealLike(frc::Sha256(ss + ePub + B.dh), "request", req.substr(33));
        std::string eph = frc::Sha256("ephemeral 5"), ePub2 = frc::X25519Base(eph), ss2 = frc::X25519(eph, C.dh);
        std::string v = std::string(1, 2) + ePub2 + SealLike(frc::Sha256(ss2 + ePub2 + C.dh), "request", std::string(12, '\0'), plain);
        Check(!OpenRequest(C.pub, C.dhSecret, C.dh, v, out), "forged request: one meant for B does not pass for C");
    }
    e.kind = 2;
    std::string wd = SealRequest(B.card, A.seed, e, frc::Sha256("ephemeral 6"));
    Check(OpenRequest(B.pub, B.dhSecret, B.dh, wd, out) && out.kind == 2 && out.from.identity == A.pub, "withdrawal: opens, signed by the asker");

    // ---- notes between two friends
    std::string sAB = frc::X25519(A.dhSecret, B.dh), sBA = frc::X25519(B.dhSecret, A.dh);
    Check(sAB == sBA && sAB.size() == 32, "note: both ends work out the same secret");
    PairKeys ab = PairKeysOf(sAB, A.pub), ba = PairKeysOf(sAB, B.pub);
    Check(ab.pub != ba.pub && ab.seal != ba.seal, "note: one place each way");
    Note n;
    n.state = 1;
    n.gen = 4;
    n.key = frc::Sha256("status key");
    n.place = frc::Sha256("status place");
    n.time = 1790000000;
    std::string note = SealNote(ab, n, H("0102030405060708090a0b0c"));
    Note nb;
    Check(OpenNote(ab, note, nb) && nb.state == 1 && nb.gen == 4 && nb.key == n.key && nb.place == n.place, "note: opens, every field back");
    Check(!OpenNote(ba, note, nb), "note: not with the other direction's keys");
    PairKeys xa = PairKeysOf(frc::X25519(X.dhSecret, A.dh), B.pub);
    Check(xa.pub != ba.pub && !OpenNote(xa, note, nb), "note: a stranger works out another place and cannot open it");
    Note over;
    over.state = 0;
    Check(OpenNote(ab, SealNote(ab, over, H("0102030405060708090a0b0d")), nb) && nb.state == 0 && nb.key.empty(), "note: 'over' carries no key");

    // ---- the status
    Status st;
    st.nick = "Prueba A";
    st.state = Playing;
    st.game = "Juego de ejemplo: edici\xC3\xB3n \xC3\xB1";
    st.gameId = "0100abcdef012345";
    st.time = 1790000000;
    st.total = 123456;
    st.colour = 3;
    st.games = {{"0100abcdef012345", "Juego de ejemplo", 100000}, {"", "Otro juego", 23456}};
    const std::string K = frc::Sha256("status key one"), nonce = H("000102030405060708090a0b");
    std::string sealed = SealStatus(K, st, false, nonce);
    Status gs;
    bool moved = true;
    Check(OpenStatus(K, sealed, gs, moved) && !moved, "status: opens with the status key");
    Check(gs.nick == st.nick && gs.state == st.state && gs.game == st.game && gs.gameId == st.gameId && gs.time == st.time &&
              gs.total == st.total && gs.colour == st.colour && gs.games.size() == 2 && gs.games[0].id == st.games[0].id &&
              gs.games[0].seconds == 100000 && gs.games[1].id.empty(),
          "status: every field comes back");
    Check(sealed.find("Prueba") == std::string::npos && sealed.find("Juego") == std::string::npos, "status: nothing readable in the stored bytes");
    // Everything someone who has A's code can work out, tried as a status key.
    bool opened = false;
    for (const std::string& k : {kA.seal, kA.seed, kA.pub, frc::Sha256(A.code), A.dh, A.pub})
        opened = opened || OpenStatus(k, sealed, gs, moved);
    Check(!opened, "status: no key that comes from the code opens it");
    const std::string K2 = frc::Sha256("status key two");
    Check(!OpenStatus(K, SealStatus(K2, st, false, nonce), gs, moved), "status: after a new key the old one opens nothing");
    Check(OpenStatus(K, SealStatus(K, Status(), true, nonce), gs, moved) && moved, "status: 'moved' comes back");
    for (size_t at : {size_t{0}, size_t{5}, size_t{30}, sealed.size() - 1}) {
        std::string bad = sealed;
        bad[at] ^= 0x20;
        Check(!OpenStatus(K, bad, gs, moved), "status: byte " + std::to_string(at) + " changed is refused");
    }
    Status big = st;
    big.nick = std::string(200, 'N');
    big.games.clear();
    for (int i = 0; i < 80; i++)
        big.games.push_back({"0100000000000" + std::to_string(100 + i), "Juego con un nombre bastante largo " + std::to_string(i),
                             1000000u - static_cast<unsigned>(i)});
    std::string bigSealed = SealStatus(K, big, false, nonce);
    ben::Val v(bigSealed);
    Check(bigSealed.size() <= 800 && ben::Encode(v).size() <= dht::kMaxValue,
          "size: the stored value is at most 800 bytes, 1000 allowed (" + std::to_string(ben::Encode(v).size()) + ")");
    Check(OpenStatus(K, bigSealed, gs, moved) && gs.nick.size() == 60 && !gs.games.empty() && gs.games.size() < 80 && gs.games[0].seconds == 1000000u,
          "size: opens, most played first");
    printf("  a full status carries %zu games in %zu bytes\n", gs.games.size(), bigSealed.size());

    Friend f;
    f.known = true;
    f.st = st;
    f.seen = 1000000;
    Check(Online(f, 1000000 + 14 * 60) && !Online(f, 1000000 + 16 * 60), "online: for fifteen minutes after the last sign of life");
    f.st.state = SignedOff;
    Check(!Online(f, 1000001), "online: not after signing off");
}

// ---- the protocol, several users of one module over a network in this process

bool WaitFor(const std::function<bool()>& cond, double seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}
template <class T> bool HasCode(const std::vector<T>& v, const std::string& code) {
    for (const auto& x : v)
        if (x.who.code == code) return true;
    return false;
}
bool FindFriend(const std::string& owner, const std::string& code, friends::Friend& out) {
    for (const auto& f : friends::List(owner))
        if (f.code == code) {
            out = f;
            return true;
        }
    return false;
}
bool IsFriend(const std::string& owner, const std::string& code, bool known = false) {
    friends::Friend f;
    return FindFriend(owner, code, f) && (!known || f.known);
}
std::string GameOf(const std::string& owner, const std::string& code) {
    friends::Friend f;
    return FindFriend(owner, code, f) && f.known && f.st.state == friends::Playing ? f.st.game : std::string();
}
void PutAs(const friends::Keys& k, const std::string& salt, const std::string& value, int64_t seq) {
    friends::MemPut(k.pub, salt, value, seq, frc::Ed25519Sign(k.seed, k.pub, dht::SignBuffer(salt, seq, value)));
}

void Protocol() {
    using namespace friends;
    // A version 1 friends.dat (round 1's trial): its identity and sharing choice stay, its friends go.
    const std::string v1seed = frc::Sha256("version one");
    {
        ben::Val fr = ben::Val::MakeDict();
        fr.Set("c", "123456789012").Set("k", std::string(32, 'k')).Set("s", int64_t{1}).Set("x", 0);
        ben::Val list = ben::Val::MakeList();
        list.list.push_back(fr);
        ben::Val u = ben::Val::MakeDict();
        u.Set("on", 1).Set("id", v1seed).Set("col", 4).Set("f", list);
        ben::Val users = ben::Val::MakeDict();
        users.Set("v1owner", u);
        ben::Val root = ben::Val::MakeDict();
        root.Set("v", 1).Set("users", users);
        std::string text = ben::Encode(root);
        FILE* f = fopen("friends.dat", "wb");
        if (f) {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
        }
    }
    UseMemoryNetwork();
    Tuning t;
    t.pollSeconds = t.watchSeconds = 0.3;
    t.republishSeconds = 2;
    t.inboxSeconds = 0.4;
    t.answerSeconds = 0.2;
    t.noteSeconds = 1;
    t.noteWriteSeconds = 5;
    t.cardSeconds = 1;
    t.resendSeconds = 1;
    t.queriesPerSecond = 1000;
    SetTuning(t);
    if (getenv("FRIENDS_TEST_LOG")) SetLog([](const std::string& l) { printf("    | %s\n", l.c_str()); });
    Init(".", false);
    Check(Sharing("v1owner") && List("v1owner").empty() && Colour("v1owner", -9) == 4, "version 1 file: sharing and colour kept, its friends dropped");
    Check(OwnCode("v1owner") == CodeOf(frc::Ed25519Public(v1seed)), "version 1 file: the identity is kept and the code is made from it");

    auto user = [](const std::string& id, const std::string& nick, int colour) {
        std::string code = OwnCode(id);
        Status st;
        st.nick = nick;
        st.colour = colour;
        SetSelf(id, st);
        SetSharing(id, true);
        return code;
    };
    // (A card is there a moment after its owner starts sharing: looked up again until found, as a person would.)
    auto look = [](const std::string& code, Look* how = nullptr, bool once = false) {
        Card c;
        Look l = Look::Busy;
        for (int tries = 0; tries < (once ? 1 : 40); tries++) {
            StartLookup(code);
            WaitFor([&] { return (l = LookupState(&c)) != Look::Busy; }, 20);
            CancelLookup();
            if (l == Look::Found) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (how) *how = l;
        return c;
    };
    auto play = [](const std::string& id, const std::string& nick, const std::string& game) {
        Status st;
        st.nick = nick;
        st.state = game.empty() ? InMenu : Playing;
        st.game = game;
        st.total = 3600;
        SetSelf(id, st);
    };

    // ---- request, accept, both see each other
    const std::string a = user("A", "Prueba A", 1), b = user("B", "Prueba B", 2);
    Look how;
    Card cb = look(b, &how);
    Check(how == Look::Found && cb.nick == "Prueba B" && cb.colour == 2 && cb.code == b, "flow: the code's card is found (nickname and colour)");
    play("B", "Prueba B", "Secreto de B");
    Check(SendRequest("A", cb) == Sent::Sent && SendRequest("A", cb) == Sent::Already && HasCode(Outgoing("A"), b), "flow: request sent, listed as sent");
    auto t0 = std::chrono::steady_clock::now();
    Check(WaitFor([&] { return HasCode(Received("B"), a); }, 10), "flow: the request arrives");
    printf("  request arrived after %.2f s (inbox read every %.1f s)\n",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), t.inboxSeconds);
    {
        auto r = Received("B");
        Check(r.size() == 1 && r[0].who.nick == "Prueba A" && r[0].who.colour == 1 && r[0].who.code == a, "flow: the request shows who asks");
    }
    std::string k1, p1;
    Check(List("A").empty() && List("B").empty() && !TestPeek("A", b, k1, p1), "flow: nothing shared before it is accepted");
    Check(Accept("B", a), "flow: accepted");
    Check(WaitFor([&] { return GameOf("A", b) == "Secreto de B" && IsFriend("B", a, true); }, 10),
          "flow: both are friends and see each other's status");
    Check(Outgoing("A").empty() && Received("B").empty(), "flow: no longer listed as requests");
    play("A", "Prueba A", "Ronda 1");
    t0 = std::chrono::steady_clock::now();
    Check(WaitFor([&] { return GameOf("B", a) == "Ronda 1"; }, 10), "flow: a change is seen by the friend");
    printf("  a change seen after %.2f s (read every %.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
           t.pollSeconds);

    // ---- someone who only has A's code
    const Keys kA = DeriveKeys(a);
    Person X = MakePerson("X", "Impostor", 3);
    {
        std::string v;
        int64_t seq = 0;
        Card c;
        bool open = false;
        Check(MemGet(kA.pub, kCardSalt, v, seq) && OpenCard(kA, a, v, c, open) && open && c.nick == "Prueba A",
              "stranger: reads the card (nickname and colour)");
        bool anything = false;
        Status st;
        bool moved = false;
        Envelope e;
        const int64_t day = DayOf(UnixNow());
        for (int64_t d = day - 1; d <= day + 1; d++)
            for (int i = 0; i < kInboxSlots; i++)
                if (MemGet(kA.pub, InboxSalt(d, i), v, seq))
                    for (const std::string& k : {kA.seal, kA.seed})
                        anything = anything || OpenStatus(k, v, st, moved) || OpenRequest(X.pub, X.dhSecret, X.dh, v, e) ||
                                   v.find("Ronda") != std::string::npos;
        for (const std::string& k : {kA.seal, kA.seed, kA.pub})
            anything = anything || (MemGet(kA.pub, kStatusSalt, v, seq) && OpenStatus(k, v, st, moved));
        Check(!anything, "stranger: no status, game or request anywhere the code leads");
    }

    // ---- removal: ends it for both, and the removed one reads nothing published after it
    // (A has a second friend, K, who must follow A's status to its new place.)
    const std::string kk = user("K", "Prueba K", 6);
    Check(SendRequest("K", look(a)) == Sent::Sent && WaitFor([&] { return HasCode(Received("A"), kk); }, 10) && Accept("A", kk) &&
              WaitFor([&] { return GameOf("K", a) == "Ronda 1"; }, 10),
          "remove: A has a second friend, K");
    std::string oldKey, oldPlace, bKey, bPlace;
    Check(TestPeek("B", a, oldKey, oldPlace) && TestPeek("A", b, bKey, bPlace), "remove: each holds the other's status key and place");
    Remove("A", b);
    Check(!IsFriend("A", b), "remove: gone from A's list at once");
    bool sawSecret = false;
    Check(WaitFor([&] {
              sawSecret = sawSecret || GameOf("B", a) == "Secreto";
              return !IsFriend("B", a);
          }, 10),
          "remove: gone from B's list too");
    play("A", "Prueba A", "Secreto");
    WaitFor([&] {
        sawSecret = sawSecret || GameOf("B", a) == "Secreto";
        return false;
    }, 2.5);
    std::string newKey, newPlace;
    TestOwnPlace("A", newKey, newPlace);
    {
        std::string v;
        int64_t seq = 0;
        Status st;
        bool moved = false;
        Check(newKey != oldKey && newPlace != oldPlace, "remove: A's status has a new key and a new place");
        Check(MemGet(oldPlace, kStatusSalt, v, seq) && OpenStatus(oldKey, v, st, moved) && moved && st.game.empty() && st.nick.empty(),
              "remove: the old place only says 'moved'");
        Check(MemGet(newPlace, kStatusSalt, v, seq) && !OpenStatus(oldKey, v, st, moved) && OpenStatus(newKey, v, st, moved) && st.game == "Secreto",
              "remove: what A publishes now does not open with the key B had");
    }
    Check(WaitFor([&] { return GameOf("K", a) == "Secreto"; }, 10), "remove: K, still a friend, follows A's status to the new place");
    Check(!sawSecret, "remove: B never showed anything published after the removal");
    {
        std::string kb2, pb2;
        TestOwnPlace("B", kb2, pb2);
        Check(pb2 != bPlace && kb2 != bKey, "remove: B's status moved as well (A's old copy of B's key opens nothing new)");
    }

    // ---- decline: the asker is told nothing
    const std::string c = user("C", "Prueba C", 3);
    Card ca = look(a);
    Check(SendRequest("C", ca) == Sent::Sent, "decline: C asks A");
    Check(WaitFor([&] { return HasCode(Received("A"), c); }, 10), "decline: A sees it");
    Decline("A", c);
    Check(!WaitFor([&] { return HasCode(Received("A"), c); }, 3), "decline: it does not come back with C's copies");
    Check(HasCode(Outgoing("C"), a) && !IsFriend("C", a) && !IsFriend("A", c), "decline: for C it stays waiting; nobody is a friend");

    // ---- withdraw
    const std::string d = user("D", "Prueba D", 4);
    Check(SendRequest("D", ca) == Sent::Sent, "withdraw: D asks A");
    Check(WaitFor([&] { return HasCode(Received("A"), d); }, 10), "withdraw: A sees it");
    Withdraw("D", a);
    Check(Outgoing("D").empty(), "withdraw: gone from D's sent list");
    Check(WaitFor([&] { return !HasCode(Received("A"), d); }, 10), "withdraw: gone from A's list too");

    // ---- two requests at once
    const std::string e = user("E", "Prueba E", 5), f = user("F", "Prueba F", 6), g = user("G", "Prueba G", 7);
    Card cg = look(g);
    Check(SendRequest("E", cg) == Sent::Sent && SendRequest("F", cg) == Sent::Sent, "two at once: E and F ask G together");
    Check(WaitFor([&] { return HasCode(Received("G"), e) && HasCode(Received("G"), f); }, 10), "two at once: both arrive");
    Accept("G", e);
    Accept("G", f);
    Check(WaitFor([&] { return IsFriend("E", g, true) && IsFriend("F", g, true) && IsFriend("G", e, true) && IsFriend("G", f, true); }, 10),
          "two at once: all friends");

    // ---- both ask each other
    const std::string h = user("H", "Prueba H", 1), i = user("I", "Prueba I", 2);
    Card ci = look(i), ch = look(h);
    SendRequest("H", ci);
    Sent second = SendRequest("I", ch);
    Check(WaitFor([&] { return IsFriend("H", i, true) && IsFriend("I", h, true); }, 10),
          std::string("both ask: friends (the second request ") + (second == Sent::Accepted ? "accepted at once)" : "met the first)"));

    // ---- the stranger with A's code tries harder
    {
        const int64_t day = DayOf(UnixNow());
        // Fills one of A's places for today with something that is not a request, under a number nobody can go above.
        PutAs(kA, InboxSalt(day, 0), std::string(1, 2) + frc::RandomBytes(200), INT64_C(4000000000000000000));
        // Leaves a request "from B" (B's identity, their signature) in another.
        ben::Val dd = ben::Val::MakeDict();
        dd.Set("v", 2).Set("k", 1).Set("c", b).Set("x", X.dh).Set("t", UnixNow()).Set("n", "Prueba B").Set("o", 2);
        std::string body = ben::Encode(dd);
        std::string plain = cb.identity + frc::Ed25519Sign(X.seed, X.pub, "dockside-request-v2" + ca.identity + body) + body;
        std::string eph = frc::Sha256("x eph"), ePub = frc::X25519Base(eph), ss = frc::X25519(eph, ca.dh);
        PutAs(kA, InboxSalt(day, 1), std::string(1, 2) + ePub + SealLike(frc::Sha256(ss + ePub + ca.dh), "request", std::string(12, '\0'), plain),
              UnixNow() + 5);
        Check(WaitFor([&] { return Troubles("A").inbox; }, 10), "stranger: A's page can say its inbox holds things that are not requests");
        Check(!HasCode(Received("A"), b), "stranger: the forged request 'from B' is not shown");
        // A real request still gets through the places left.
        const std::string j = user("J", "Prueba J", 3);
        SendRequest("J", ca);
        Check(WaitFor([&] { return HasCode(Received("A"), j); }, 10), "stranger: a real request still arrives in another place");

        // Writes a card of their own over A's.
        std::string cv;
        int64_t cseq = 0;
        MemGet(kA.pub, kCardSalt, cv, cseq);
        Card fake = X.card;
        fake.code = a;
        PutAs(kA, kCardSalt, SealCard(kA, X.seed, fake, true), cseq + 100);
        Look l2;
        Card seen = look(a, &l2, true);
        Check(l2 == Look::Unverified || (l2 == Look::Found && seen.identity == ca.identity), "stranger: their card never passes as A's");
        bool flagged = WaitFor([&] { return Troubles("A").card; }, 10);
        Check(flagged, "stranger: A sees its card was written over");
        Check(WaitFor([&] {
                  std::string v2;
                  int64_t s2 = 0;
                  Card c2;
                  bool o2 = false;
                  return MemGet(kA.pub, kCardSalt, v2, s2) && OpenCard(kA, a, v2, c2, o2) && c2.identity == ca.identity;
              }, 10),
              "stranger: A's card is written back above it");
        // Toward "E and G are no longer friends": a note at the place a stranger with G's code can work out.
        const Keys kG = DeriveKeys(g);
        std::string sv;
        int64_t ss2 = 0;
        Card gcard;
        bool gopen = false;
        Check(MemGet(kG.pub, kCardSalt, sv, ss2) && OpenCard(kG, g, sv, gcard, gopen), "stranger: G's card read");
        Note over;
        over.state = 0;
        PairKeys guess = PairKeysOf(frc::X25519(X.dhSecret, gcard.dh), gcard.identity);
        std::string nv = SealNote(guess, over, frc::RandomBytes(12));
        friends::MemPut(guess.pub, kNoteSalt, nv, UnixNow() + 10, frc::Ed25519Sign(guess.seed, guess.pub, dht::SignBuffer(kNoteSalt, UnixNow() + 10, nv)));
        play("G", "Prueba G", "Sigue aqui");
        Check(WaitFor([&] { return GameOf("E", g) == "Sigue aqui"; }, 10) && IsFriend("G", e), "stranger: cannot end a friendship it is not part of");
    }

    Traffic tr = Counters();
    printf("  in-process network: %llu reads, %llu writes in all\n", static_cast<unsigned long long>(tr.gets), static_cast<unsigned long long>(tr.puts));
    Shutdown();
    remove("friends.dat");
    remove("friends.dat.tmp");
}
} // namespace

int main() {
    Hashes();
    Cipher();
    Signatures();
    KeyAgreement();
    Bep44();
    Bencode();
    Formats();
    Protocol();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
