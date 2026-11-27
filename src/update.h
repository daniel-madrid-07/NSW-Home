// "Actualizar la consola" on a PC: NSW-Home looks at the releases of its own
// GitHub repository and, on Windows, can put a newer one in its own place.
//
// What leaves the machine: one HTTPS GET to api.github.com for the latest
// release (User-Agent "NSW-Home/<version>", no cookies, no credentials, no
// query, nothing about the machine or the person) and, only if the user then
// chooses to update, one HTTPS GET for the release's file (GitHub sends that
// on to its file server). Both happen only when the user asks: nothing here
// runs at start or on a timer.
//
// What is trusted: GitHub's certificate (checked by the system) and whoever
// can publish releases in kRepository. There is no code signature: the checks
// below catch a broken, cut short or swapped download, not a release published
// by someone who has taken over that account.
//
// The file a release must carry to be installable from here is kAssetName
// (the one-file Windows x64 program, exactly as build.bat makes it). A release
// without it is still announced, with its page offered instead.
//
// Asking and installing are Windows only. Everywhere else Available() is
// false and the functions answer "UNAVAILABLE"; the parts that only read text
// and bytes (versions, the release's description, the checks) are the same on
// every system.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace update {

extern const char* const kRepository; // "owner/repository"
extern const char* const kAssetName;  // the file of a release that is this program
// Nothing larger is ever downloaded (the program is about 2 MB).
constexpr uint64_t kMaxAsset = 64ull << 20;

bool Available();  // the latest release can be asked for on this system
bool CanInstall(); // and a newer program put in place of this one

// ---- the same on every system -------------------------------------------------

// "1.2.3" or "v1.2.3" (one to four numbers) as numbers. False for anything
// else: a tag with a suffix ("1.3.0-beta") is not a version that gets offered.
bool ParseVersion(const std::string& tag, std::vector<int>& out);
// <0, 0, >0. Missing parts count as zero ("1.2" equals "1.2.0").
int CompareVersions(const std::vector<int>& a, const std::vector<int>& b);

struct Asset {
    std::string name, url;
    uint64_t size = 0;
    std::string sha256;     // lower-case hex, from the release's "digest"; empty when GitHub states none
    bool digestOdd = false; // a digest is stated, but not one that can be checked here
};
struct Release {
    std::string tag, version;       // version: the tag without its "v"
    std::string notes;              // the first lines of its description as plain text (UTF-8), "\n" between them
    std::vector<std::string> files; // the names of every file the release carries
    bool hasAsset = false;
    Asset asset;
};
enum class Answer {
    Latest,  // nothing newer is published
    Newer,   // `release` is newer than this program
    None,    // no release published yet, or no such repository (GitHub answers 404 to both)
    Offline, // no answer at all
    Refused, // GitHub is not taking more anonymous requests for now (60 an hour per address)
    Odd      // an answer that is not what GitHub's API gives
};
struct Check {
    Answer answer = Answer::Offline;
    Release release;
    int status = 0; // the HTTP status, 0 without an answer
};
// What an answer of GitHub's "latest release" call for `repository` means for
// a program of version `current` whose file in a release is `assetName`.
// `status` 0: nothing came back.
Check Interpret(int status, const std::string& body, const std::string& current, const std::string& repository,
                const char* assetName);

// Whether these bytes are a Windows program for this kind of processor (an
// executable image, 64-bit, not a library).
bool IsProgram(const std::string& bytes);
// "" when the bytes are the release's file: the size GitHub states, its
// SHA-256 when it states one, and a Windows program. Otherwise SIZE, DIGEST or
// NOT_PROGRAM.
std::string VerifyBytes(const std::string& bytes, const Asset& asset);

// The address of a release's page (of the list of releases when the tag is
// not one that can go in an address unchanged). Always built here from
// kRepository: an address that came over the network is never opened.
std::string PageUrl(const std::string& tag);

// Where a download is kept until it is installed: <data>\update\<kAssetName>.download.
std::wstring DownloadPath(const std::wstring& dataDir);

// ---- Windows only ---------------------------------------------------------------

// An offscreen run: nothing below reaches the network, opens anything or
// starts anything, whatever calls it.
void SetOffscreen(bool on);

// Asks GitHub for the latest release. Blocks (seconds at most): for a worker
// thread. `cancel` set: gives up at the next step.
Check Ask(const std::string& repository, const std::string& current, const char* assetName, const std::atomic<bool>* cancel);

// Downloads the release's file to `file` (its folder is made). `got` counts
// the bytes so far. "" or CANCELLED, REFUSED (not an address of `repository`'s
// releases on github.com, or too large), NETWORK, IO, UNAVAILABLE. A file that
// did not arrive whole is deleted.
std::string Download(const Asset& asset, const std::string& repository, const std::wstring& file, const std::string& current,
                     std::atomic<uint64_t>* got, const std::atomic<bool>* cancel);

// This program's own file.
std::wstring ProgramPath();
// Where the program being replaced is kept until the next start: beside it,
// "<name>.old.exe" (still a program that can be started by hand).
std::wstring AsidePath(const std::wstring& program);
// Whether a file can be made beside `program` (checked before downloading).
bool FolderWritable(const std::wstring& program);

// Puts the downloaded file in place of `program`, which may be running:
//   1. the download is read and checked (VerifyBytes): nothing is touched on failure;
//   2. it is written beside the program as "<program>.new" and read back;
//   3. the program is renamed aside (AsidePath);
//   4. the new one is renamed to the program's name; if that fails, the old
//      one is renamed back (or, failing that, copied back).
// At every step the folder holds a complete program under the program's name,
// except after ASIDE. "" or SIZE / DIGEST / NOT_PROGRAM (the download is not
// the release's file), IO (the download cannot be read), NOT_WRITABLE (nothing
// can be written in the program's folder), LOCKED (the program, or an older
// one set aside, cannot be moved: nothing was changed), ASIDE (step 4 failed
// and so did both ways back: the program is now the file AsidePath names),
// UNAVAILABLE. The download is deleted in every case but IO.
std::string Install(const std::wstring& program, const std::wstring& download, const Asset& asset);

// Starts the program again, told to wait for this one to end first (only one
// NSW-Home runs at a time). The caller then quits. False if it did not start.
bool Restart();

// The one call main.cpp makes, before it looks for another NSW-Home: waits
// for the NSW-Home that started this one after updating to end, then removes
// the program it set aside and anything left of a download in `dataDir`.
void AtStart(const std::wstring& dataDir);

// Opens PageUrl(tag) in the browser. False if it could not (or offscreen).
bool OpenPage(const std::string& tag);

} // namespace update
