// Tests of src\update.cpp (built by tools\build_update_test.bat into build\update_test.exe).
// Run from the project folder:
//   build\update_test.exe pure      versions, GitHub's answers (canned), the checks, the notes
//   build\update_test.exe install   the replacing of a program, in build\upd_sandbox (stand-in programs only)
//   build\update_test.exe net       three real requests: the placeholder repository (expects "not found"),
//                                   mikefarah/yq's latest release, and that release's Windows program
//                                   (downloaded into build\upd_sandbox, checked, never run, deleted)
// Internal modes, used by "install": sleep <ms>, atstart <dataDir> (with --after-update <pid>).
#include <windows.h>
#include "../src/update.h"
#include "../src/fr_crypto.h"
#include <cstdio>
#include <string>

using namespace update;

static int failures = 0;
static void Expect(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static std::string Hex(const std::string& b) {
    static const char d[] = "0123456789abcdef";
    std::string o;
    for (unsigned char c : b) o += d[c >> 4], o += d[c & 15];
    return o;
}

static std::string ReadFile(const std::wstring& f) {
    std::string out;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, f.c_str(), L"rb") || !fp) return out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, n);
    fclose(fp);
    return out;
}
static bool WriteFile(const std::wstring& f, const std::string& b) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, f.c_str(), L"wb") || !fp) return false;
    const bool ok = fwrite(b.data(), 1, b.size(), fp) == b.size();
    fclose(fp);
    return ok;
}
static bool Exists(const std::wstring& f) {
    return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring Self() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// ---- pure --------------------------------------------------------------------

static std::string ReleaseJson(const std::string& tag, const std::string& extra, const std::string& assets) {
    return "{\"url\":\"x\",\"tag_name\":\"" + tag + "\",\"draft\":false,\"prerelease\":false" + extra + ",\"assets\":[" + assets + "]}";
}
static std::string AssetJson(const std::string& name, const std::string& url, const std::string& size, const std::string& digest) {
    return "{\"name\":\"" + name + "\",\"state\":\"uploaded\",\"size\":" + size + ",\"digest\":" + digest +
           ",\"browser_download_url\":\"" + url + "\"}";
}

static void Pure() {
    std::vector<int> v;
    Expect(ParseVersion("v1.2.3", v) && v == std::vector<int>({1, 2, 3}), "v1.2.3 is a version");
    Expect(ParseVersion("0.1.0", v) && v.size() == 3, "0.1.0 is a version");
    Expect(!ParseVersion("1.3.0-beta", v), "1.3.0-beta is not offered");
    Expect(!ParseVersion("jq-1.8.2", v), "jq-1.8.2 is not a version");
    Expect(!ParseVersion("v", v) && !ParseVersion("", v) && !ParseVersion("1..2", v) && !ParseVersion("1.2.", v), "empty parts refused");
    Expect(!ParseVersion("1.2.3.4.5", v), "five numbers refused");
    Expect(!ParseVersion("1234567.0", v), "a number too long refused");
    std::vector<int> a, b;
    ParseVersion("1.2", a), ParseVersion("1.2.0", b);
    Expect(CompareVersions(a, b) == 0, "1.2 == 1.2.0");
    ParseVersion("0.10.0", a), ParseVersion("0.9.9", b);
    Expect(CompareVersions(a, b) > 0, "0.10.0 > 0.9.9 (numbers, not text)");

    const std::string repo = "daniel-madrid-07/NSW-Home";
    const std::string base = "https://github.com/daniel-madrid-07/NSW-Home/releases/download/v0.2.0/";
    const std::string digest = "\"sha256:" + std::string(64, 'A') + "\"";

    Check c = Interpret(0, "", "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Offline, "no answer: Offline");
    c = Interpret(404, "{\"message\":\"Not Found\"}", "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::None, "404: None");
    c = Interpret(403, "{}", "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Refused, "403: Refused");
    c = Interpret(500, "", "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Odd, "500: Odd");
    c = Interpret(200, "{\"tag_name\":\"v0.2.0\"", "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Odd, "cut-short JSON: Odd");
    c = Interpret(200, "[1,2]", "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Odd, "not an object: Odd");
    c = Interpret(200, ReleaseJson("v0.1.0", "", ""), "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Latest, "same version: Latest");
    c = Interpret(200, ReleaseJson("v0.0.9", "", ""), "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Latest, "older version: Latest");
    c = Interpret(200, "{\"tag_name\":\"v0.2.0\",\"prerelease\":true,\"assets\":[]}", "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Latest, "pre-release: not offered");
    c = Interpret(200, "{\"tag_name\":\"v0.2.0\",\"draft\":true,\"assets\":[]}", "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Latest, "draft: not offered");
    c = Interpret(200, ReleaseJson("v0.2.0-rc1", "", ""), "0.1.0", repo, kAssetName);
    Expect(c.answer == Answer::Latest, "suffixed tag: not offered");

    c = Interpret(200, ReleaseJson("v0.2.0", "", AssetJson("NSW-Home.exe", base + "NSW-Home.exe", "2000000", digest)), "0.1.0", repo,
                  kAssetName);
    Expect(c.answer == Answer::Newer && c.release.version == "0.2.0" && c.release.hasAsset, "newer with the program");
    Expect(c.release.asset.size == 2000000 && c.release.asset.sha256 == std::string(64, 'a') && !c.release.asset.digestOdd,
           "size and digest read (lower-cased)");
    c = Interpret(200, ReleaseJson("0.2.0", "", AssetJson("nsw-home.EXE", base + "nsw-home.EXE", "2000000", "null")), "0.1.0", repo,
                  kAssetName);
    Expect(c.answer == Answer::Newer && c.release.hasAsset && c.release.asset.sha256.empty(), "tag without v; no digest stated");
    c = Interpret(200, ReleaseJson("v0.2.0", "", AssetJson("NSW-Home.zip", base + "NSW-Home.zip", "2000000", digest)), "0.1.0", repo,
                  kAssetName);
    Expect(c.answer == Answer::Newer && !c.release.hasAsset && c.release.files.size() == 1, "newer without the program");
    c = Interpret(200, ReleaseJson("v0.2.0", "", AssetJson("NSW-Home.exe", "https://evil.example/NSW-Home.exe", "2000000", digest)),
                  "0.1.0", repo, kAssetName);
    Expect(!c.release.hasAsset, "a file elsewhere than this repository's releases is refused");
    c = Interpret(200,
                  ReleaseJson("v0.2.0", "",
                          AssetJson("NSW-Home.exe", "https://github.com/someone-else/NSW-Home/releases/download/v0.2.0/NSW-Home.exe",
                                    "2000000", digest)),
                  "0.1.0", repo, kAssetName);
    Expect(!c.release.hasAsset, "another repository's file is refused");
    c = Interpret(200, ReleaseJson("v0.2.0", "", AssetJson("NSW-Home.exe", base + "../../x/NSW-Home.exe", "2000000", digest)), "0.1.0",
                  repo, kAssetName);
    Expect(!c.release.hasAsset, "an address that climbs out is refused");
    c = Interpret(200, ReleaseJson("v0.2.0", "", AssetJson("NSW-Home.exe", base + "NSW-Home.exe", "999999999999", digest)), "0.1.0",
                  repo, kAssetName);
    Expect(!c.release.hasAsset, "a file too large is refused");
    c = Interpret(200, ReleaseJson("v0.2.0", "", AssetJson("NSW-Home.exe", base + "NSW-Home.exe", "2000000", "\"sha512:abc\"")), "0.1.0",
                  repo, kAssetName);
    Expect(c.release.hasAsset && c.release.asset.digestOdd, "a digest of another kind is marked");

    // The notes: Markdown and hidden characters out, three lines, cut at 64 characters.
    const std::string body = "\"## What's new\\r\\n\\r\\n- Faster **menu** [see](https://x.y/z) `code`\\r\\n"
                             "- Hidden \\u202Ereversed\\u202C and \\u0007bell\\r\\n"
                             "- " +
                             std::string(80, 'w') + "\\r\\n- fourth line\"";
    c = Interpret(200, ReleaseJson("v0.2.0", ",\"body\":" + body, ""), "0.1.0", repo, kAssetName);
    printf("      notes: [%s]\n", c.release.notes.c_str());
    Expect(c.release.notes.find("What's new\n\xC2\xB7 Faster menu see code\n\xC2\xB7 Hidden reversed and bell") == 0,
           "notes as plain text");
    Expect(c.release.notes.find("fourth") == std::string::npos, "three lines at most");
    Expect(c.release.notes.find("\xE2\x80\xA6") == std::string::npos, "(the third line is the hidden one)");
    c = Interpret(200, ReleaseJson("v0.2.0", ",\"body\":\"" + std::string(80, 'w') + "\"", ""), "0.1.0", repo, kAssetName);
    Expect(c.release.notes == std::string(64, 'w') + "\xE2\x80\xA6", "a long line is cut with an ellipsis");

    // The checks on the bytes.
    const std::string self = ReadFile(Self());
    Asset as;
    as.size = self.size();
    as.sha256 = Hex(frc::Sha256(self));
    Expect(IsProgram(self), "this program is a Windows program");
    Expect(VerifyBytes(self, as).empty(), "right size, digest and kind: accepted");
    Asset noDigest = as;
    noDigest.sha256.clear();
    Expect(VerifyBytes(self, noDigest).empty(), "no digest stated: size and kind suffice");
    Asset wrongSize = as;
    wrongSize.size++;
    Expect(VerifyBytes(self, wrongSize) == "SIZE", "wrong size: SIZE");
    std::string changed = self;
    changed[changed.size() / 2] ^= 1;
    Expect(VerifyBytes(changed, as) == "DIGEST", "one bit changed: DIGEST");
    Asset odd = as;
    odd.digestOdd = true;
    Expect(VerifyBytes(self, odd) == "DIGEST", "an unknown digest is never taken as checked");
    const std::string text(4096, 't');
    Asset textAsset;
    textAsset.size = text.size();
    textAsset.sha256 = Hex(frc::Sha256(text));
    Expect(VerifyBytes(text, textAsset) == "NOT_PROGRAM", "a file of text: NOT_PROGRAM");
    std::string dll = self;
    const size_t pe = static_cast<unsigned char>(dll[0x3C]) | static_cast<unsigned char>(dll[0x3D]) << 8;
    dll[pe + 23] |= 0x20; // the library flag
    Expect(!IsProgram(dll), "a library is not a program");
    std::string x86 = self;
    x86[pe + 4] = 0x4C, x86[pe + 5] = 0x01;
    Expect(!IsProgram(x86), "a 32-bit program is not this kind");
    Expect(!IsProgram("MZ"), "two bytes are not a program");

    Expect(PageUrl("v0.2.0") == "https://github.com/daniel-madrid-07/NSW-Home/releases/tag/v0.2.0", "page of a release");
    Expect(PageUrl("v0.2.0/../../x") == "https://github.com/daniel-madrid-07/NSW-Home/releases", "odd tag: list of releases");
    Expect(PageUrl("a b\"&c") == "https://github.com/daniel-madrid-07/NSW-Home/releases", "tag with odd characters: list");
    Expect(AsidePath(L"C:\\x\\NSW-Home.exe") == L"C:\\x\\NSW-Home.old.exe", "the old program's name");
    Expect(DownloadPath(L"D:\\data") == L"D:\\data\\update\\NSW-Home.exe.download", "the download's name");
}

// ---- install -------------------------------------------------------------------

static std::wstring sandbox;

static Asset AssetOf(const std::string& bytes) {
    Asset a;
    a.size = bytes.size();
    a.sha256 = Hex(frc::Sha256(bytes));
    return a;
}

// A stand-in "old program" and a stand-in download: this test program with a mark at its end.
static std::string StandIn(const char* mark) {
    return ReadFile(Self()) + mark;
}

static void Fresh(const std::wstring& dir) {
    CreateDirectoryW(sandbox.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    for (const wchar_t* f : {L"\\NSW-Home.exe", L"\\NSW-Home.old.exe", L"\\NSW-Home.exe.new", L"\\dl"}) {
        SetFileAttributesW((dir + f).c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW((dir + f).c_str());
    }
}

static HANDLE StartHidden(const std::wstring& exe, const std::wstring& args, DWORD* pid = nullptr) {
    std::wstring line = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    if (pid) *pid = pi.dwProcessId;
    return pi.hProcess;
}

static void Install() {
    const std::string oldB = StandIn("OLD"), newB = StandIn("NEW");

    // 1. The normal case, with the old program RUNNING (a hidden stand-in that sleeps).
    {
        const std::wstring dir = sandbox + L"\\ok", prog = dir + L"\\NSW-Home.exe", dl = dir + L"\\dl";
        Fresh(dir);
        WriteFile(prog, oldB);
        WriteFile(dl, newB);
        HANDLE running = StartHidden(prog, L"sleep 4000");
        Expect(running != nullptr, "stand-in old program started (hidden, sleeps)");
        Sleep(300);
        Expect(FolderWritable(prog) && !Exists(prog + L".new"), "folder writable, probe file gone");
        const std::string r = Install(prog, dl, AssetOf(newB));
        Expect(r.empty(), ("install over a running program: \"" + r + "\"").c_str());
        Expect(ReadFile(prog) == newB, "the program is now the new one");
        Expect(ReadFile(AsidePath(prog)) == oldB, "the old one is beside it, whole");
        Expect(!Exists(dl) && !Exists(prog + L".new"), "download and .new gone");
        Expect(!DeleteFileW(AsidePath(prog).c_str()), "the running old one cannot be deleted yet (AtStart's job)");
        if (running) {
            WaitForSingleObject(running, 10000);
            CloseHandle(running);
        }
        // A second update while the first one's old program is still there (no restart in between).
        WriteFile(dl, oldB);
        const std::string r2 = Install(prog, dl, AssetOf(oldB));
        Expect(r2.empty() && ReadFile(prog) == oldB && ReadFile(AsidePath(prog)) == newB, "a second update replaces the set-aside file");
    }
    // 2. Verification fails: nothing is touched, the download goes.
    {
        const std::wstring dir = sandbox + L"\\bad", prog = dir + L"\\NSW-Home.exe", dl = dir + L"\\dl";
        Fresh(dir);
        WriteFile(prog, oldB);
        std::string broken = newB;
        broken[1000] ^= 0x55;
        WriteFile(dl, broken);
        const std::string r = Install(prog, dl, AssetOf(newB));
        Expect(r == "DIGEST", ("swapped bytes: \"" + r + "\"").c_str());
        Expect(ReadFile(prog) == oldB && !Exists(AsidePath(prog)) && !Exists(prog + L".new") && !Exists(dl),
               "program untouched, download deleted");
        WriteFile(dl, newB.substr(0, newB.size() - 10));
        Expect(Install(prog, dl, AssetOf(newB)) == "SIZE" && ReadFile(prog) == oldB, "cut short: SIZE, untouched");
        const std::string text(5000, 'x');
        WriteFile(dl, text);
        Expect(Install(prog, dl, AssetOf(text)) == "NOT_PROGRAM" && ReadFile(prog) == oldB, "not a program: NOT_PROGRAM, untouched");
        Expect(Install(prog, dl + L"-missing", AssetOf(newB)) == "IO" && ReadFile(prog) == oldB, "no download: IO, untouched");
    }
    // 3. The program's folder cannot be written (a deny rule on the throw-away folder, removed afterwards).
    {
        const std::wstring dir = sandbox + L"\\ro", prog = dir + L"\\NSW-Home.exe";
        const std::wstring dl = sandbox + L"\\ro_dl";
        Fresh(dir);
        WriteFile(prog, oldB);
        WriteFile(dl, newB);
        wchar_t user[256];
        DWORD n = 256;
        GetUserNameW(user, &n);
        const std::wstring deny = L"icacls \"" + dir + L"\" /deny \"" + user + L"\":(WD,AD,WA,WEA) >nul";
        const std::wstring undo = L"icacls \"" + dir + L"\" /remove:d \"" + user + L"\" >nul";
        Expect(_wsystem(deny.c_str()) == 0, "deny rule set on the throw-away folder");
        Expect(!FolderWritable(prog), "FolderWritable sees it");
        const std::string r = Install(prog, dl, AssetOf(newB));
        Expect(r == "NOT_WRITABLE", ("read-only folder: \"" + r + "\"").c_str());
        Expect(ReadFile(prog) == oldB && !Exists(AsidePath(prog)), "program untouched");
        Expect(_wsystem(undo.c_str()) == 0, "deny rule removed");
        DeleteFileW(dl.c_str());
    }
    // 4. The old program cannot be renamed (held open without sharing deletion).
    {
        const std::wstring dir = sandbox + L"\\locked", prog = dir + L"\\NSW-Home.exe", dl = dir + L"\\dl";
        Fresh(dir);
        WriteFile(prog, oldB);
        WriteFile(dl, newB);
        HANDLE hold = CreateFileW(prog.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        const std::string r = Install(prog, dl, AssetOf(newB));
        Expect(r == "LOCKED", ("old program cannot be renamed: \"" + r + "\"").c_str());
        CloseHandle(hold);
        Expect(ReadFile(prog) == oldB && !Exists(AsidePath(prog)) && !Exists(prog + L".new"), "program untouched, nothing left");
        // An older program set aside that cannot be removed.
        WriteFile(AsidePath(prog), oldB);
        WriteFile(dl, newB);
        hold = CreateFileW(AsidePath(prog).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        const std::string r2 = Install(prog, dl, AssetOf(newB));
        CloseHandle(hold);
        Expect(r2 == "LOCKED" && ReadFile(prog) == oldB && !Exists(prog + L".new"), ("stuck older one: \"" + r2 + "\", untouched").c_str());
    }
    // 5. AtStart: waits for the NSW-Home that started it, then removes what an update left.
    {
        const std::wstring dir = sandbox + L"\\start", prog = dir + L"\\NSW-Home.exe", data = dir + L"\\data";
        Fresh(dir);
        CreateDirectoryW(data.c_str(), nullptr);
        CreateDirectoryW((data + L"\\update").c_str(), nullptr);
        WriteFile(prog, ReadFile(Self()));
        WriteFile(AsidePath(prog), oldB);
        WriteFile(prog + L".new", newB);
        WriteFile(DownloadPath(data), newB);
        DWORD pid = 0;
        CopyFileW(Self().c_str(), (dir + L"\\sleeper.exe").c_str(), FALSE);
        HANDLE sleeper = StartHidden(dir + L"\\sleeper.exe", L"sleep 1500", &pid);
        const ULONGLONG t0 = GetTickCount64();
        HANDLE start = StartHidden(prog, L"atstart \"" + data + L"\" --after-update " + std::to_wstring(pid));
        DWORD code = 1;
        if (start) {
            WaitForSingleObject(start, 60000);
            GetExitCodeProcess(start, &code);
            CloseHandle(start);
        }
        const ULONGLONG took = GetTickCount64() - t0;
        printf("      AtStart took %llu ms (the one it waited for slept 1500 ms)\n", took);
        Expect(code == 0 && took >= 1200, "AtStart waited for the earlier program to end");
        Expect(!Exists(AsidePath(prog)) && !Exists(prog + L".new") && !Exists(DownloadPath(data)), "set-aside, .new and download removed");
        Expect(Exists(prog), "the program itself stays");
        if (sleeper) CloseHandle(sleeper);
    }
}

// ---- net ---------------------------------------------------------------------------

static const char* Name(Answer a) {
    switch (a) {
    case Answer::Latest: return "Latest";
    case Answer::Newer: return "Newer";
    case Answer::None: return "None";
    case Answer::Offline: return "Offline";
    case Answer::Refused: return "Refused";
    case Answer::Odd: return "Odd";
    }
    return "?";
}

static void Net() {
    std::atomic<bool> cancel{false};
    Check c = Ask(kRepository, "0.1.0", kAssetName, &cancel);
    printf("      %s: HTTP %d, %s\n", kRepository, c.status, Name(c.answer));
    Expect(c.answer == Answer::None && c.status == 404, "placeholder repository: not found");

    // A well-known public repository with numbered tags and a Windows program in each release.
    const char* repo = "mikefarah/yq";
    const char* file = "yq_windows_amd64.exe";
    c = Ask(repo, "0.0.1", file, &cancel);
    printf("      %s: HTTP %d, %s, tag %s, %zu files\n", repo, c.status, Name(c.answer), c.release.tag.c_str(), c.release.files.size());
    printf("      notes: [%s]\n", c.release.notes.c_str());
    printf("      asset: %s, %llu bytes, sha256 %s%s\n      from %s\n", c.release.asset.name.c_str(),
           static_cast<unsigned long long>(c.release.asset.size), c.release.asset.sha256.empty() ? "(none stated)" : c.release.asset.sha256.c_str(),
           c.release.asset.digestOdd ? " (odd)" : "", c.release.asset.url.c_str());
    Expect(c.answer == Answer::Newer && !c.release.notes.empty(), "real answer parsed: newer, with notes");
    Expect(c.release.hasAsset && c.release.asset.size > 0 && !c.release.asset.sha256.empty(), "real asset found with size and digest");
    if (!c.release.hasAsset) return;

    CreateDirectoryW(sandbox.c_str(), nullptr);
    const std::wstring dl = sandbox + L"\\net\\yq.download";
    std::atomic<uint64_t> got{0};
    const ULONGLONG t0 = GetTickCount64();
    const std::string r = Download(c.release.asset, repo, dl, "0.0.1", &got, &cancel);
    printf("      download: \"%s\", %llu bytes in %llu ms\n", r.c_str(), static_cast<unsigned long long>(got.load()), GetTickCount64() - t0);
    Expect(r.empty(), "real download arrived whole");
    const std::string bytes = ReadFile(dl);
    Expect(VerifyBytes(bytes, c.release.asset).empty(), "size, SHA-256 and kind of the real file check out");
    DeleteFileW(dl.c_str());
    RemoveDirectoryW((sandbox + L"\\net").c_str());
    // An address of another repository is refused before anything is asked.
    Expect(Download(c.release.asset, kRepository, dl, "0.0.1", &got, &cancel) == "REFUSED", "the file of another repository: REFUSED");
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && !wcscmp(argv[1], L"sleep")) {
        Sleep(static_cast<DWORD>(_wtoi(argv[2])));
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"atstart")) {
        AtStart(argv[2]);
        return 0;
    }
    std::wstring self = Self();
    sandbox = self.substr(0, self.find_last_of(L'\\')) + L"\\upd_sandbox";
    const std::wstring mode = argc >= 2 ? argv[1] : L"pure";
    if (mode == L"pure") Pure();
    else if (mode == L"install") Install();
    else if (mode == L"net") Net();
    else {
        printf("modes: pure install net\n");
        return 2;
    }
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
