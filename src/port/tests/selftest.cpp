// Checks the Windows stand-ins (src/port/win, compat.cpp) against published
// test vectors and against the behaviour the shared sources rely on.
// Exit code 0 = everything passed.
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <wincrypt.h>

#include "port.h"

#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0, checks = 0;

static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL  %s\n", what);
    }
}

static std::vector<uint8_t> Hex(const char* s) {
    std::vector<uint8_t> out;
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (; s[0] && s[1]; s += 2) out.push_back(static_cast<uint8_t>(v(s[0]) * 16 + v(s[1])));
    return out;
}

// ---- AES ----------------------------------------------------------------------

static void AesCase(const char* name, const char* key, const char* plain, const char* cipher) {
    std::vector<uint8_t> k = Hex(key), p = Hex(plain), c = Hex(cipher);
    // Through the same calls nsw.cpp makes, in place.
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE h = nullptr;
    wchar_t mode[] = BCRYPT_CHAIN_MODE_ECB;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode), sizeof(mode), 0)) &&
              BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &h, nullptr, 0, k.data(), static_cast<ULONG>(k.size()), 0));
    std::string label = std::string("AES ") + name;
    Check(ok, (label + ": key set-up").c_str());
    if (!ok) return;
    std::vector<uint8_t> buf = p;
    ULONG done = 0;
    ok = BCRYPT_SUCCESS(BCryptEncrypt(h, buf.data(), static_cast<ULONG>(buf.size()), nullptr, nullptr, 0, buf.data(),
                                      static_cast<ULONG>(buf.size()), &done, 0));
    Check(ok && done == buf.size() && buf == c, (label + ": encrypt").c_str());
    ok = BCRYPT_SUCCESS(BCryptDecrypt(h, buf.data(), static_cast<ULONG>(buf.size()), nullptr, nullptr, 0, buf.data(),
                                      static_cast<ULONG>(buf.size()), &done, 0));
    Check(ok && done == buf.size() && buf == p, (label + ": decrypt").c_str());
    BCryptDestroyKey(h);
    BCryptCloseAlgorithmProvider(alg, 0);
}

static void TestAes() {
    // FIPS-197 appendix B and appendix C.1-C.3.
    AesCase("FIPS-197 B", "2b7e151628aed2a6abf7158809cf4f3c", "3243f6a8885a308d313198a2e0370734",
            "3925841d02dc09fbdc118597196a0b32");
    AesCase("FIPS-197 C.1 (128)", "000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff",
            "69c4e0d86a7b0430d8cdb78070b4c55a");
    AesCase("FIPS-197 C.2 (192)", "000102030405060708090a0b0c0d0e0f1011121314151617", "00112233445566778899aabbccddeeff",
            "dda97ca4864cdfe06eaf70a0ec0d7191");
    AesCase("FIPS-197 C.3 (256)", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
            "00112233445566778899aabbccddeeff", "8ea2b7ca516745bfeafc49904b496089");
    // NIST SP 800-38A F.1.1 / F.1.5: four blocks in one call.
    AesCase("SP 800-38A ECB-AES128", "2b7e151628aed2a6abf7158809cf4f3c",
            "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
            "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
            "3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf"
            "43b1cd7f598ece23881b00e3ed0306887b0c785e27e8ad3f8223207104725dd4");
    AesCase("SP 800-38A ECB-AES256", "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
            "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
            "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
            "f3eed1bdb5d2a03c064b5a7e3db181f8591ccb10d410ed26dc5ba74a31362870"
            "b6ed21b99ca6f4f9f153e7b1beafed1d23304b7a39f9f3ff067d8d8f9e24ecc7");

    // What the API must refuse.
    BCRYPT_ALG_HANDLE alg = nullptr;
    Check(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, L"DES", nullptr, 0)), "AES: unknown algorithm refused");
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    wchar_t cbc[] = L"ChainingModeCBC";
    Check(!BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(cbc), sizeof(cbc), 0)),
          "AES: chaining modes other than ECB refused");
    BCRYPT_KEY_HANDLE h = nullptr;
    uint8_t key[16] = {}, block[20] = {};
    Check(!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &h, nullptr, 0, key, 15, 0)), "AES: 15-byte key refused");
    BCryptGenerateSymmetricKey(alg, &h, nullptr, 0, key, 16, 0);
    ULONG done = 0;
    Check(!BCRYPT_SUCCESS(BCryptEncrypt(h, block, 20, nullptr, nullptr, 0, block, 20, &done, 0)),
          "AES: length that is not a multiple of 16 refused");
    BCryptDestroyKey(h);

    uint8_t a[16] = {}, b[16] = {};
    Check(BCRYPT_SUCCESS(BCryptGenRandom(nullptr, a, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) &&
              BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) && memcmp(a, b, 16) != 0,
          "random: two draws differ");
}

// ---- base64 -------------------------------------------------------------------

static std::string Encode(const std::string& data, DWORD flags) {
    DWORD len = 0;
    const BYTE* p = reinterpret_cast<const BYTE*>(data.data());
    if (!CryptBinaryToStringA(p, static_cast<DWORD>(data.size()), flags, nullptr, &len)) return "<size query failed>";
    std::string out(len, '#');
    if (!CryptBinaryToStringA(p, static_cast<DWORD>(data.size()), flags, &out[0], &len)) return "<encode failed>";
    if (len + 1 != out.size() || out[len] != 0) return "<length contract broken>";
    out.resize(len);
    return out;
}

static bool Decode(const std::string& text, std::string& out) {
    DWORD len = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &len, nullptr,
                              nullptr))
        return false;
    std::vector<BYTE> bin(len + 1);
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, bin.data(), &len, nullptr,
                              nullptr))
        return false;
    out.assign(reinterpret_cast<const char*>(bin.data()), len);
    return true;
}

static void TestBase64() {
    // RFC 4648 section 10.
    static const char* const rfc[][2] = {{"", ""},           {"f", "Zg=="},         {"fo", "Zm8="},        {"foo", "Zm9v"},
                                         {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    for (const auto& v : rfc) {
        Check(Encode(v[0], flags) == v[1], (std::string("base64 encode \"") + v[0] + "\"").c_str());
        std::string back;
        Check(Decode(v[1], back) && back == v[0], (std::string("base64 decode \"") + v[1] + "\"").c_str());
    }
    std::string all;
    for (int i = 0; i < 256; i++) all += static_cast<char>(i);
    for (int extra = 0; extra < 3; extra++) {
        std::string data = all + std::string(static_cast<size_t>(extra), 'x'), back;
        Check(Decode(Encode(data, flags), back) && back == data, "base64 round trip of every byte value");
        // The default form breaks lines; decoding has to skip the breaks.
        std::string broken = Encode(data, CRYPT_STRING_BASE64);
        Check(broken.find("\r\n") == 64 && broken.size() >= 2 && broken.compare(broken.size() - 2, 2, "\r\n") == 0,
              "base64 with line breaks: CR LF after 64 characters and at the end");
        Check(Decode(broken, back) && back == data, "base64 with line breaks decodes");
    }
    std::string out;
    Check(Decode(" Zm9v\r\nYmFy\n", out) && out == "foobar", "base64 decode skips white space");
    Check(!Decode("Zm9v!mFy", out), "base64 decode refuses a foreign character");
    Check(!Decode("Zm9vY", out), "base64 decode refuses a dangling character");
    Check(!Decode("Zg==Zg==", out), "base64 decode refuses data after padding");
    BYTE small[2];
    DWORD len = sizeof small;
    Check(!CryptStringToBinaryA("Zm9vYmFy", 8, CRYPT_STRING_BASE64, small, &len, nullptr, nullptr) && len == 6,
          "base64 decode into a short buffer fails and reports the size");
}

// ---- UTF conversions ----------------------------------------------------------

static std::wstring W(std::initializer_list<uint32_t> cps) {
    std::wstring w;
    for (uint32_t c : cps) w += static_cast<wchar_t>(c);
    return w;
}

static void TestUtf() {
    Check(sizeof(wchar_t) == 4, "wchar_t is 32 bits on this system (the layer assumes UTF-32)");
    struct {
        const char* utf8;
        std::wstring wide;
        const char* what;
    } pairs[] = {
        {"A", W({0x41}), "ASCII"},
        {"\xC3\xB1", W({0xF1}), "two bytes (U+00F1)"},
        {"\xE2\x82\xAC", W({0x20AC}), "three bytes (U+20AC)"},
        {"\xF0\x9D\x84\x9E", W({0x1D11E}), "four bytes (U+1D11E), one wide character"},
        {"\xE5\xA6\x96\xE6\x80\xAA", W({0x5996, 0x602A}), "CJK"},
        {"\xEF\xBF\xBD", W({0xFFFD}), "U+FFFD itself"},
        {"\xF4\x8F\xBF\xBF", W({0x10FFFF}), "last code point"},
        {"a\xC3\xA9z", W({0x61, 0xE9, 0x7A}), "mixed"},
    };
    for (const auto& p : pairs) {
        Check(port::Wide(p.utf8) == p.wide, (std::string("UTF-8 -> wide: ") + p.what).c_str());
        Check(port::Utf8(p.wide) == p.utf8, (std::string("wide -> UTF-8: ") + p.what).c_str());
    }
    // Malformed input: one U+FFFD per bad byte, the rest kept.
    Check(port::Wide("\xC3\x28") == W({0xFFFD, 0x28}), "UTF-8 -> wide: broken continuation");
    Check(port::Wide("\xC0\xAF") == W({0xFFFD, 0xFFFD}), "UTF-8 -> wide: overlong form refused");
    Check(port::Wide("\xED\xA0\x80") == W({0xFFFD, 0xFFFD, 0xFFFD}), "UTF-8 -> wide: encoded surrogate refused");
    Check(port::Wide("ab\xE2\x82") == W({0x61, 0x62, 0xFFFD, 0xFFFD}), "UTF-8 -> wide: truncated at the end");
    Check(port::Wide("\xF8\x88\x80\x80\x80") == W({0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD}), "UTF-8 -> wide: five-byte form refused");
    // Wide text assembled from UTF-16 units (fwfont.cpp builds names that way).
    Check(port::Utf8(W({0xD834, 0xDD1E})) == "\xF0\x9D\x84\x9E", "wide -> UTF-8: surrogate pair joined");
    Check(port::Utf8(W({0xD834})) == "\xEF\xBF\xBD", "wide -> UTF-8: lone surrogate replaced");
    Check(port::Utf8(W({0x110000})) == "\xEF\xBF\xBD", "wide -> UTF-8: value past U+10FFFF replaced");

    // The Windows calling convention.
    wchar_t wbuf[8];
    Check(MultiByteToWideChar(CP_UTF8, 0, "h\xC3\xA9", -1, nullptr, 0) == 3, "MultiByteToWideChar: -1 counts the NUL");
    Check(MultiByteToWideChar(CP_UTF8, 0, "h\xC3\xA9", 3, nullptr, 0) == 2, "MultiByteToWideChar: size query");
    Check(MultiByteToWideChar(CP_UTF8, 0, "h\xC3\xA9", -1, wbuf, 8) == 3 && wbuf[1] == 0xE9 && wbuf[2] == 0,
          "MultiByteToWideChar: converts with the NUL");
    Check(MultiByteToWideChar(CP_UTF8, 0, "abcd", 4, wbuf, 2) == 0, "MultiByteToWideChar: buffer too small fails");
    Check(MultiByteToWideChar(CP_UTF8, 0, "", 0, wbuf, 8) == 0, "MultiByteToWideChar: empty input");
    char buf[8];
    Check(WideCharToMultiByte(CP_UTF8, 0, L"h\u00e9", -1, nullptr, 0, nullptr, nullptr) == 4, "WideCharToMultiByte: -1 counts the NUL");
    Check(WideCharToMultiByte(CP_UTF8, 0, L"h\u00e9", 2, buf, 8, nullptr, nullptr) == 3 && !memcmp(buf, "h\xC3\xA9", 3),
          "WideCharToMultiByte: converts");
    Check(WideCharToMultiByte(CP_UTF8, 0, L"h\u00e9", 2, buf, 2, nullptr, nullptr) == 0, "WideCharToMultiByte: buffer too small fails");
}

// ---- strings ------------------------------------------------------------------

static void TestStrings() {
    wchar_t b[64];
    swprintf_s(b, L"bar_%d_%02d_%s", 3, 7, L"d");
    Check(!wcscmp(b, L"bar_3_07_d"), "swprintf_s: %s takes a wide string");
    swprintf_s(b, L"%s|%c|%S|%ls|%5s|%-4s|", L"w\u00edde", L'x', "narrow", L"ls", L"ab", L"cd");
    Check(!wcscmp(b, L"w\u00edde|x|narrow|ls|   ab|cd  |"), "swprintf_s: Microsoft format conventions");
    swprintf_s(b, L"%02d:%02d %d%% %.1f GB %llu min", 6, 5, 100, 12.34, static_cast<uint64_t>(5000000000ULL));
    Check(!wcscmp(b, L"06:05 100% 12.3 GB 5000000000 min"), "swprintf_s: numbers, %% and a decimal point");
    wchar_t tiny[4];
    swprintf_s(tiny, L"%s", L"abcdefgh");
    Check(tiny[3] == 0, "swprintf_s: an overlong result is cut, not written past the buffer");

    Check(_wcsicmp(L"Ryujinx.EXE", L"ryujinx.exe") == 0 && _wcsicmp(L"abc", L"abd") < 0 && _wcsicmp(L"b", L"A") > 0 &&
              _wcsicmp(L"ab", L"abc") < 0,
          "_wcsicmp");
    Check(_wcsicmp(L"\u00c1RBOL", L"\u00e1rbol") == 0, "_wcsicmp: beyond ASCII");
    std::wstring s = L"C:\\ROMS\\\u00d1AND\u00da.NSP";
    CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    Check(s == L"c:\\roms\\\u00f1and\u00fa.nsp", "CharLowerBuffW");
    Check(towupper(L'\u00f1') == L'\u00d1', "towupper beyond ASCII (avatar initials)");
    Check(_wtoi(L"120") == 120 && _wtoi(L"7x") == 7 && _wtoi(L"x") == 0, "_wtoi");
    unsigned h = 0, m = 0, sec = 0;
    Check(sscanf_s("01:02:03", "%u:%u:%u", &h, &m, &sec) == 3 && h == 1 && m == 2 && sec == 3, "sscanf_s");
    uint8_t secret[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    SecureZeroMemory(secret, sizeof secret);
    Check(secret[0] == 0 && secret[7] == 0, "SecureZeroMemory");
}

// ---- files, environment, time -------------------------------------------------

static void TestFiles() {
    const char* tmp = getenv("TMPDIR");
    std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/nswhome_selftest_XXXXXX";
    const char* made = mkdtemp(&tmpl[0]);
    Check(made != nullptr, "temporary folder");
    if (!made) return;
    const std::string root = made;
    const std::wstring wroot = port::Wide(root);

    Check(port::NativePath(L"/a\\b\\c.txt") == "/a/b/c.txt", "NativePath: backslashes become slashes");
    Check(port::NativePath(L"relative\\x") == "relative/x", "NativePath: relative path");
    // Drive letters map to <root>/<letter> only where that folder exists.
    mkdir((root + "/drives").c_str(), 0755);
    mkdir((root + "/drives/q").c_str(), 0755);
    setenv("NSWHOME_DRIVE_ROOT", (root + "/drives").c_str(), 1);
    Check(port::NativePath(L"Q:\\Games\\a.nsp") == root + "/drives/q/Games/a.nsp", "NativePath: mounted drive letter");
    Check(port::NativePath(L"Z:\\Games\\a.nsp") == "Z:/Games/a.nsp", "NativePath: unknown drive letter left alone");

    // Folders (parents included), files, attributes.
    const std::wstring dir = wroot + L"\\one\\two";
    Check(CreateDirectoryW(dir.c_str(), nullptr) == TRUE, "CreateDirectoryW creates missing parents");
    Check(CreateDirectoryW(dir.c_str(), nullptr) == FALSE, "CreateDirectoryW on an existing folder reports failure");
    Check(GetFileAttributesW(dir.c_str()) == FILE_ATTRIBUTE_DIRECTORY, "GetFileAttributesW: folder");
    Check(GetFileAttributesW((dir + L"\\nothing").c_str()) == INVALID_FILE_ATTRIBUTES, "GetFileAttributesW: missing");

    const std::wstring file = dir + L"\\se\u00f1al.bin";
    FILE* f = nullptr;
    Check(_wfopen_s(&f, file.c_str(), L"wb") == 0 && f, "_wfopen_s: create");
    std::string payload(100000, 0);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = static_cast<char>(i * 31 + 7);
    if (f) {
        fwrite(payload.data(), 1, payload.size(), f);
        fclose(f);
    }
    Check(GetFileAttributesW(file.c_str()) == FILE_ATTRIBUTE_NORMAL, "GetFileAttributesW: file");
    Check(_wfopen_s(&f, (dir + L"\\missing.bin").c_str(), L"rb") != 0 && !f, "_wfopen_s: missing file fails");
    Check(_wfopen_s(&f, (dir + L"\\log.txt").c_str(), L"a, ccs=UTF-8") == 0 && f, "_wfopen_s: mode with a ccs= suffix");
    if (f) fclose(f);

    // Positional reads, as nsw.cpp does them.
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(h != INVALID_HANDLE_VALUE, "CreateFileW");
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        Check(GetFileSizeEx(h, &size) && size.QuadPart == static_cast<LONGLONG>(payload.size()), "GetFileSizeEx");
        char buf[64];
        OVERLAPPED ov = {};
        ov.Offset = 77777;
        DWORD got = 0;
        Check(ReadFile(h, buf, sizeof buf, &got, &ov) && got == sizeof buf && !memcmp(buf, payload.data() + 77777, sizeof buf),
              "ReadFile at an offset");
        ov.Offset = static_cast<DWORD>(payload.size() - 10);
        Check(ReadFile(h, buf, sizeof buf, &got, &ov) && got == 10, "ReadFile: short read at the end");
        ov.Offset = static_cast<DWORD>(payload.size());
        ReadFile(h, buf, sizeof buf, &got, &ov);
        Check(got == 0, "ReadFile: nothing past the end");
        Check(CloseHandle(h) == TRUE, "CloseHandle");
    }
    Check(CreateFileW(dir.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) ==
              INVALID_HANDLE_VALUE,
          "CreateFileW refuses a folder");

    // Modification time as a FILETIME: 2020-01-01T00:00:00.1234567Z.
    timespec times[2] = {{1577836800, 123456700}, {1577836800, 123456700}};
    utimensat(AT_FDCWD, port::NativePath(file).c_str(), times, 0);

    // Listing.
    mkdir(port::NativePath(dir + L"\\sub").c_str(), 0755);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    Check(find != INVALID_HANDLE_VALUE, "FindFirstFileExW");
    int dots = 0, others = 0;
    bool sawFile = false, sawSub = false;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") dots++;
            else others++;
            if (name == L"se\u00f1al.bin") {
                uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                uint64_t mtime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                sawFile = size == payload.size() && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                          mtime == 132223104001234567ULL;
                if (!sawFile) printf("      size %llu mtime %llu\n", static_cast<unsigned long long>(size), static_cast<unsigned long long>(mtime));
            }
            if (name == L"sub") sawSub = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    Check(dots == 2 && others == 3, "listing: . and .. plus the three entries");
    Check(sawFile, "listing: file size and modification time (100 ns ticks since 1601)");
    Check(sawSub, "listing: folder flag");
    Check(FindFirstFileExW((dir + L"\\nope\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0) ==
              INVALID_HANDLE_VALUE,
          "FindFirstFileExW: missing folder");

    // Copy, replace, delete.
    const std::wstring copy = dir + L"\\copy.bin", fresh = copy + L".tmp";
    Check(CopyFileW(file.c_str(), copy.c_str(), TRUE) == TRUE, "CopyFileW");
    Check(CopyFileW(file.c_str(), copy.c_str(), TRUE) == FALSE, "CopyFileW: existing target refused when asked to");
    Check(CopyFileW(file.c_str(), copy.c_str(), FALSE) == TRUE, "CopyFileW: existing target replaced otherwise");
    _wfopen_s(&f, fresh.c_str(), L"wb");
    if (f) {
        fputs("new", f);
        fclose(f);
    }
    Check(MoveFileExW(fresh.c_str(), copy.c_str(), 0) == FALSE, "MoveFileExW: existing target refused without the flag");
    Check(MoveFileExW(fresh.c_str(), copy.c_str(), MOVEFILE_REPLACE_EXISTING) == TRUE, "MoveFileExW: replace");
    char three[4] = {};
    _wfopen_s(&f, copy.c_str(), L"rb");
    size_t n = f ? fread(three, 1, 4, f) : 0;
    if (f) fclose(f);
    Check(n == 3 && !memcmp(three, "new", 3) && GetFileAttributesW(fresh.c_str()) == INVALID_FILE_ATTRIBUTES,
          "MoveFileExW: the target now holds the new content");
    Check(DeleteFileW(copy.c_str()) == TRUE && DeleteFileW(copy.c_str()) == FALSE, "DeleteFileW");

    // The reason a refused copy gives (ryucfg.cpp tells "already backed up" from "cannot back up" by it).
    SetLastError(0);
    CopyFileW(file.c_str(), copy.c_str(), TRUE);
    Check(CopyFileW(file.c_str(), copy.c_str(), TRUE) == FALSE && GetLastError() == ERROR_FILE_EXISTS,
          "CopyFileW: an existing target reports ERROR_FILE_EXISTS");
    Check(CopyFileW((dir + L"\\nothing").c_str(), (dir + L"\\x").c_str(), TRUE) == FALSE && GetLastError() == ERROR_FILE_NOT_FOUND,
          "CopyFileW: a missing source reports ERROR_FILE_NOT_FOUND");

    // Writing a file through a handle and swapping it in, as ryucfg.cpp saves the emulator's settings.
    const std::wstring target = dir + L"\\Config.json", staged = target + L".dockside-tmp";
    const std::string first = "{\n  \"version\": 1\n}", second(70000, 'z');
    for (const std::string* text : {&first, &second, &first}) { // longer, then shorter again: no stale tail
        HANDLE w = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD put = 0;
        bool ok = w != INVALID_HANDLE_VALUE && WriteFile(w, text->data(), static_cast<DWORD>(text->size()), &put, nullptr) &&
                  put == text->size() && FlushFileBuffers(w);
        if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
        ok = ok && MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        // Read back in one sequential read (no OVERLAPPED), again as ryucfg.cpp does.
        std::string back;
        HANDLE r = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size;
        DWORD got = 0;
        if (r != INVALID_HANDLE_VALUE && GetFileSizeEx(r, &size)) {
            back.resize(static_cast<size_t>(size.QuadPart));
            ok = ok && ReadFile(r, &back[0], static_cast<DWORD>(back.size()), &got, nullptr) && got == back.size();
            DWORD more = 1;
            char one;
            ok = ok && ReadFile(r, &one, 1, &more, nullptr) && more == 0; // the position moved to the end
        }
        if (r != INVALID_HANDLE_VALUE) CloseHandle(r);
        Check(ok && back == *text, "CreateFileW/WriteFile/FlushFileBuffers, swap in, sequential ReadFile");
    }
    Check(GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES, "the staging file is gone after the swap");
    Check(CreateFileW((dir + L"\\no\\such\\folder\\f").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) ==
                  INVALID_HANDLE_VALUE &&
              GetLastError() == ERROR_FILE_NOT_FOUND,
          "CreateFileW for writing fails in a missing folder");

    // Folder paths in one call (the key file's folder).
    const std::wstring deep = dir + L"\\Ryujinx\\system\\deeper";
    Check(SHCreateDirectoryExW(nullptr, deep.c_str(), nullptr) == ERROR_SUCCESS && GetFileAttributesW(deep.c_str()) == FILE_ATTRIBUTE_DIRECTORY,
          "SHCreateDirectoryExW creates the whole path");
    Check(SHCreateDirectoryExW(nullptr, deep.c_str(), nullptr) == ERROR_ALREADY_EXISTS, "SHCreateDirectoryExW: already there");
    Check(CreateDirectoryW(deep.c_str(), nullptr) == FALSE && GetLastError() == ERROR_ALREADY_EXISTS,
          "CreateDirectoryW: already there reports ERROR_ALREADY_EXISTS");

    // Free space.
    ULARGE_INTEGER avail, all, freeAll;
    avail.QuadPart = all.QuadPart = freeAll.QuadPart = 0;
    Check(GetDiskFreeSpaceExW(dir.c_str(), &avail, &all, &freeAll) && all.QuadPart > 0 && avail.QuadPart <= freeAll.QuadPart &&
              freeAll.QuadPart <= all.QuadPart,
          "GetDiskFreeSpaceExW: free <= total");
    Check(GetDiskFreeSpaceExW(dir.c_str(), &avail, &all, nullptr) == TRUE, "GetDiskFreeSpaceExW: optional outputs");
    Check(GetDiskFreeSpaceExW((dir + L"\\nothing\\here").c_str(), &avail, &all, nullptr) == FALSE, "GetDiskFreeSpaceExW: missing path");
    printf("      disk: %.1f GB free of %.1f GB\n", avail.QuadPart / 1e9, all.QuadPart / 1e9);

    // Environment.
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    setenv("NSWHOME_TEST_VALUE", "hola", 1);
    wchar_t buf[MAX_PATH * 2];
    DWORD len = GetEnvironmentVariableW(L"NSWHOME_TEST_VALUE", buf, ARRAYSIZE(buf));
    Check(len == 4 && !wcscmp(buf, L"hola"), "GetEnvironmentVariableW: plain variable");
    Check(GetEnvironmentVariableW(L"NSWHOME_NOT_SET", buf, ARRAYSIZE(buf)) == 0, "GetEnvironmentVariableW: unset");
    Check(GetEnvironmentVariableW(L"NSWHOME_TEST_VALUE", buf, 3) == 5, "GetEnvironmentVariableW: small buffer reports the size");
    len = GetEnvironmentVariableW(L"APPDATA", buf, ARRAYSIZE(buf));
#ifdef __APPLE__
    Check(len > 0 && std::wstring(buf).find(L"/Library/Application Support") != std::wstring::npos, "APPDATA is ~/Library/Application Support");
#else
    Check(len > 0 && port::Utf8(buf) == root, "APPDATA follows XDG_CONFIG_HOME");
    unsetenv("XDG_CONFIG_HOME");
    GetEnvironmentVariableW(L"APPDATA", buf, ARRAYSIZE(buf));
    Check(port::Utf8(buf) == port::HomeDir() + "/.config", "APPDATA defaults to ~/.config");
#endif
    Check(GetEnvironmentVariableW(L"USERPROFILE", buf, ARRAYSIZE(buf)) > 0 && port::Utf8(buf) == port::HomeDir(), "USERPROFILE is the home folder");
    wchar_t* pics = nullptr;
    Check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)) && pics && pics[0] == L'/', "Pictures folder");
    CoTaskMemFree(pics);

    // Clocks.
    SYSTEMTIME utc, local;
    time_t before = time(nullptr);
    GetSystemTime(&utc);
    GetLocalTime(&local);
    tm t{};
    gmtime_r(&before, &t);
    Check(utc.wYear == t.tm_year + 1900 && utc.wMonth == t.tm_mon + 1 && utc.wDay == t.tm_mday && utc.wHour == t.tm_hour,
          "GetSystemTime agrees with the C library");
    localtime_r(&before, &t);
    Check(local.wHour == t.tm_hour && local.wDay == t.tm_mday, "GetLocalTime agrees with the C library");
    ULONGLONG t0 = GetTickCount64();
    usleep(30000);
    ULONGLONG t1 = GetTickCount64();
    Check(t1 - t0 >= 25 && t1 - t0 < 2000, "GetTickCount64 counts milliseconds");
    SYSTEM_POWER_STATUS ps;
    Check(GetSystemPowerStatus(&ps) && ((ps.BatteryFlag & 128) || ps.BatteryLifePercent <= 100), "GetSystemPowerStatus");
    printf("      power: %s\n", (ps.BatteryFlag & 128) ? "no battery" : (std::to_string(ps.BatteryLifePercent) + " %").c_str());

    std::string cleanup = "rm -rf '" + root + "'";
    if (root.find("/nswhome_selftest_") != std::string::npos && system(cleanup.c_str()) != 0) printf("      (could not remove %s)\n", root.c_str());
}

static void TestCommandLine() {
    printf("command lines\n");
    auto split = port::SplitCommandLine(L"--profile \"Ana Mar\u00eda\" --hide-updates --fullscreen \"/juegos/Mi juego (ES).nsp\"");
    Check(split.size() == 5 && split[0] == "--profile" && split[1] == "Ana Mar\xc3\xad" "a" && split[2] == "--hide-updates" &&
              split[3] == "--fullscreen" && split[4] == "/juegos/Mi juego (ES).nsp",
          "quoted words keep their spaces and lose their quotes");
    split = port::SplitCommandLine(L"-u 2 -gamer -f -g \"/a b/c.xci\"  ");
    Check(split.size() == 6 && split[1] == "2" && split[5] == "/a b/c.xci", "runs of spaces separate once");
    split = port::SplitCommandLine(L"--profile \"\" x");
    Check(split.size() == 3 && split[1].empty(), "an empty quoted word is an argument");
    Check(port::SplitCommandLine(L"").empty() && port::SplitCommandLine(L"   ").empty(), "nothing gives no arguments");
    int major = 0, minor = 0;
    Check(swscanf_s(L"1.3.207", L"%d.%d", &major, &minor) == 2 && major == 1 && minor == 3, "swscanf_s reads numbers");
}

int main() {
    TestCommandLine();
    TestAes();
    TestBase64();
    TestUtf();
    TestStrings();
    TestFiles();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
