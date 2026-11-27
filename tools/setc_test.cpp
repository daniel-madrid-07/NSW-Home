// Checks of the backends behind the Settings rows of the sections Internet,
// Control parental, Gestión de datos and Sonido (src\pcsys.cpp, src\emudata.cpp,
// src\parental.cpp). Built by tools\build_setc_test.bat into build\setc_test.exe.
//
//   setc_test pure                 hashes, the day's play time, the test tone (in memory)
//   setc_test standin <folder>     builds a stand-in emulator tree in <folder> (which must not
//                                  exist) and proves the cache removal, the save copies and the
//                                  game move on it
//   setc_test tree <folder>        only builds that tree (for the menu's own test runs: <data>\standin)
//   setc_test sizes <r|y> <data folder>   lists cache and save sizes of a real data folder; reads only
//   setc_test net                  the connection: which values could be read (never the values)
//                                  and the connection test
//   setc_test audio                default devices, microphone level read and written back unchanged
//   setc_test mixer <folder>       starts itself as a silent stand-in "game" and drives the
//                                  mute-on-unplug and volume-cap rules against its mixer entry
//   setc_test silent <seconds>     (the stand-in: plays silence)
//
// Nothing here makes a sound, opens the microphone or changes a setting of the
// system: the only mixer entry touched is the stand-in's own.
#include "../src/app.h"
#include "../src/emudata.h"
#include "../src/parental.h"
#include "../src/pcsys.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

// What pcsys.cpp takes from the program.
Config cfg;
static std::wstring dataDir;
std::wstring DataDir() {
    return dataDir;
}

static int failures = 0;
static void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void Put(const fs::path& p, size_t bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    std::string block(bytes, 'x');
    f.write(block.data(), static_cast<std::streamsize>(block.size()));
}

static std::set<std::wstring> Listing(const fs::path& root) {
    std::set<std::wstring> out;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it)
        out.insert(fs::relative(it->path(), root).wstring() + (it->is_directory() ? L"\\" : L""));
    return out;
}

static int Pure() {
    Check(parental::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of \"abc\"");
    Check(parental::Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    Check(parental::Sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "SHA-256 of a thousand a's (several blocks)");
    const std::string a = parental::MakePin("1234"), b = parental::MakePin("1234");
    Check(a.size() == 97 && a != b, "the same PIN stored twice differs (salt)");
    Check(a.find("1234") == std::string::npos, "the stored form does not contain the PIN");
    Check(parental::CheckPin(a, "1234") && parental::CheckPin(b, "1234"), "the right PIN is accepted");
    Check(!parental::CheckPin(a, "1235") && !parental::CheckPin(a, "") && !parental::CheckPin("", "1234") &&
              !parental::CheckPin("zz:zz", "1234"),
          "a wrong PIN, an empty one and a malformed stored form are refused");

    uint64_t base = 0;
    std::string day;
    Check(parental::PlayedToday(5000, base, day, "2026-11-26") == 0 && base == 5000, "a new day starts at zero");
    Check(parental::PlayedToday(5600, base, day, "2026-11-26") == 600, "ten minutes later: 600 s");
    Check(parental::PlayedToday(5600, base, day, "2026-11-27") == 0 && day == "2026-11-27", "the next day starts at zero again");
    Check(parental::PlayedToday(100, base, day, "2026-11-27") == 0 && base == 100, "a history that shrank starts again");

    const std::vector<uint8_t> wav = pcsys::ToneWav();
    const int rate = 48000;
    auto energy = [&](double from, double to, int channel) {
        double sum = 0;
        for (int i = static_cast<int>(from * rate); i < static_cast<int>(to * rate); i++) {
            const size_t at = 44 + static_cast<size_t>(i) * 4 + static_cast<size_t>(channel) * 2;
            const int16_t v = static_cast<int16_t>(wav[at] | (wav[at + 1] << 8));
            sum += static_cast<double>(v) * v;
        }
        return sum;
    };
    Check(wav.size() == 44 + static_cast<size_t>(pcsys::kToneLength * rate) * 4 && !memcmp(wav.data(), "RIFF", 4), "the tone is a WAV of the stated length");
    Check(energy(pcsys::kToneLeftFrom, pcsys::kToneLeftTo, 0) > 1e9 && energy(pcsys::kToneLeftFrom, pcsys::kToneLeftTo, 1) == 0,
          "first the left channel only");
    Check(energy(pcsys::kToneRightFrom, pcsys::kToneRightTo, 1) > 1e9 && energy(pcsys::kToneRightFrom, pcsys::kToneRightTo, 0) == 0,
          "then the right channel only");
    Check(energy(0, pcsys::kToneLeftFrom, 0) + energy(0, pcsys::kToneLeftFrom, 1) + energy(pcsys::kToneLeftTo, pcsys::kToneRightFrom, 0) +
                  energy(pcsys::kToneLeftTo, pcsys::kToneRightFrom, 1) + energy(pcsys::kToneRightTo, pcsys::kToneLength, 1) == 0,
          "silence before, between and after");
    int peak = 0;
    for (size_t at = 44; at + 1 < wav.size(); at += 2) peak = (std::max)(peak, abs(static_cast<int16_t>(wav[at] | (wav[at + 1] << 8))));
    Check(peak > 7000 && peak < 8300, "peak at a quarter of full scale");
    return failures;
}

// A Ryujinx save index with the given entries: (program, user id as stored, kind, folder number).
struct IndexLine {
    uint64_t program;
    const char* userHex; // 32 digits as NSW-Home writes them
    int type;
    uint64_t save;
};
static void WriteIndex(const fs::path& file, const std::vector<IndexLine>& lines) {
    fs::create_directories(file.parent_path());
    std::string d = "IMKV";
    auto u32 = [&](uint32_t v) { d.append(reinterpret_cast<const char*>(&v), 4); };
    u32(0);
    u32(static_cast<uint32_t>(lines.size()));
    for (const IndexLine& l : lines) {
        d += "IMEN";
        u32(0x40);
        u32(0x40);
        std::string key(0x40, '\0'), value(0x40, '\0');
        memcpy(&key[0], &l.program, 8);
        for (int half = 0; half < 2; half++) {
            const uint64_t v = strtoull(std::string(l.userHex + half * 16, 16).c_str(), nullptr, 16);
            memcpy(&key[8 + half * 8], &v, 8);
        }
        key[0x20] = static_cast<char>(l.type);
        memcpy(&value[0], &l.save, 8);
        value[0x18] = 1;
        d += key + value;
    }
    std::ofstream(file, std::ios::binary).write(d.data(), static_cast<std::streamsize>(d.size()));
}

static int Standin(const std::wstring& where, bool buildOnly) {
    const fs::path top = where;
    if (fs::exists(top)) {
        printf("the folder already exists: choose a new one\n");
        return 2;
    }
    const char* userA = "00000000000000010000000000000000";
    const char* userB = "b59d728aef8ec4b8623ffea599178912";

    // -- a Ryujinx-family data folder --
    const fs::path ryu = top / L"ryujinx";
    Put(ryu / L"system/prod.keys", 64);
    Put(ryu / L"system/Profiles.json", 80);
    Put(ryu / L"Config.json", 300);
    Put(ryu / L"bis/system/Contents/registered/0123.nca/00", 5000); // firmware
    for (const wchar_t* t : {L"0100152000022000", L"01007ef00011e000"}) {
        const fs::path g = ryu / L"games" / t;
        Put(g / L"cache/cpu/0/default.cache", 9000);
        Put(g / L"cache/cpu/0/default.info", 100);
        Put(g / L"cache/cpu/1/default.cache", 4000);
        Put(g / L"cache/shader/guest.toc", 2000);
        Put(g / L"cache/shader/shared.data", 7000);
        Put(g / L"cache/keep_me.txt", 10); // not a cache folder of ours: must survive
        Put(g / L"updates.json", 50);
        Put(g / L"dlc.json", 50);
        Put(g / L"Config.json", 70);
        Put(g / L"gui/state.json", 30);
    }
    Put(ryu / L"games/not-a-title/cache/cpu/x.cache", 500); // wrong name: not listed, not removable
    for (int n : {1, 2, 3}) {
        wchar_t name[20];
        swprintf_s(name, L"%016x", n);
        Put(ryu / L"bis/user/save" / name / L"0/save.bin", 1000 * static_cast<size_t>(n));
        Put(ryu / L"bis/user/save" / name / L"1/save.bin", 1000 * static_cast<size_t>(n));
        Put(ryu / L"bis/user/saveMeta" / name / L"00000001.meta", 16);
    }
    WriteIndex(ryu / L"bis/system/save/8000000000000000/0/imkvdb.arc",
               {{0x0100152000022000ull, userA, 1, 1}, {0x01007ef00011e000ull, userA, 1, 2}, {0x01007ef00011e000ull, userB, 1, 3}});
    fs::create_directories(ryu / L"bis/system/save/8000000000000000/1");
    fs::copy_file(ryu / L"bis/system/save/8000000000000000/0/imkvdb.arc", ryu / L"bis/system/save/8000000000000000/1/imkvdb.arc");
    // A junction inside a cache folder pointing at the saves: removing the
    // cache must take the junction away and leave what it points at.
    const fs::path trap = ryu / L"games/0100152000022000/cache/shader/trap";
    const std::wstring mk = L"cmd /c mklink /J \"" + trap.wstring() + L"\" \"" + (ryu / L"bis/user/save").wstring() + L"\" >nul 2>nul";
    const bool junction = _wsystem(mk.c_str()) == 0 && fs::exists(trap);
    printf("      (junction trap %s)\n", junction ? "in place" : "could not be made: that check is skipped");

    // -- a yuzu-family data folder --
    const fs::path yuzu = top / L"eden";
    Put(yuzu / L"keys/prod.keys", 64);
    Put(yuzu / L"keys/title.keys", 64);
    Put(yuzu / L"config/qt-config.ini", 400);
    Put(yuzu / L"nand/system/Contents/registered/aa.nca", 5000);
    Put(yuzu / L"shader/0100152000022000/vulkan.bin", 6000);
    Put(yuzu / L"shader/0100152000022000/vulkan_pipelines.bin", 3000);
    Put(yuzu / L"shader/01007ef00011e000/vulkan.bin", 2500);
    Put(yuzu / L"shader/nvidia/GLCache/x.bin", 800); // the graphics driver's own: not ours to remove
    Put(yuzu / L"nand/user/save/0000000000000000/00000000000000000000000000000000/0100152000022000/common.sav", 300);
    Put(yuzu / L"nand/user/save/0000000000000000/12891799A5FE3F62B8C48EEF8A729DB5/0100152000022000/slot0.sav", 1200);
    Put(yuzu / L"nand/user/save/0000000000000000/12891799A5FE3F62B8C48EEF8A729DB5/01007ef00011e000/slot0.sav", 700);
    Put(yuzu / L"nand/user/save/cache/0000000000000000/0100152000022000/c.bin", 90);

    // -- a library of two folders --
    const fs::path lib1 = top / L"lib1", lib2 = top / L"lib2";
    Put(lib1 / L"Stand-in Game [0100152000022000].nsp", 123456);
    fs::create_directories(lib2);
    if (buildOnly) {
        printf("stand-in tree built\n");
        return 0;
    }

    const emudata::Root R{"ryujinx", 'r', ryu.wstring()}, Y{"eden", 'y', yuzu.wstring()};

    // -- caches --
    const auto rc = emudata::ListCaches(R), yc = emudata::ListCaches(Y);
    uint64_t rBytes = 0, yBytes = 0;
    for (const auto& c : rc) rBytes += c.bytes;
    for (const auto& c : yc) yBytes += c.bytes;
    Check(rc.size() == 4 && rBytes == 2 * (9000 + 100 + 4000 + 2000 + 7000), "Ryujinx: four cache folders listed with their sizes");
    Check(yc.size() == 2 && yBytes == 6000 + 3000 + 2500, "yuzu family: two cache folders listed with their sizes");
    Check(!emudata::IsCacheDir(R, (ryu / L"bis/user/save").wstring()) && !emudata::IsCacheDir(R, (ryu / L"system").wstring()) &&
              !emudata::IsCacheDir(R, (ryu / L"bis/system/Contents/registered").wstring()) &&
              !emudata::IsCacheDir(R, (ryu / L"games/0100152000022000").wstring()) &&
              !emudata::IsCacheDir(R, (ryu / L"games/0100152000022000/cache").wstring()) &&
              !emudata::IsCacheDir(R, (ryu / L"games/0100152000022000/gui").wstring()) &&
              !emudata::IsCacheDir(R, (ryu / L"games/not-a-title/cache/cpu").wstring()) && !emudata::IsCacheDir(R, ryu.wstring()),
          "Ryujinx: saves, keys, firmware, the game's folder and misnamed folders are not cache folders");
    Check(!emudata::IsCacheDir(R, (ryu / L"games/0100152000022000/cache/cpu/../../../../bis/user/save").wstring()) &&
              !emudata::IsCacheDir(R, (yuzu / L"shader/0100152000022000").wstring()) &&
              !emudata::IsCacheDir(Y, (ryu / L"games/0100152000022000/cache/cpu").wstring()),
          "a path that climbs out, and another emulator's cache, are refused");
    if (junction)
        Check(!emudata::IsCacheDir(R, trap.wstring()), "a junction is not a cache folder");
    Check(!emudata::IsCacheDir(Y, (yuzu / L"shader").wstring()) && !emudata::IsCacheDir(Y, (yuzu / L"shader/nvidia").wstring()) &&
              !emudata::IsCacheDir(Y, (yuzu / L"nand/user/save").wstring()) && !emudata::IsCacheDir(Y, (yuzu / L"keys").wstring()) &&
              !emudata::IsCacheDir(Y, (yuzu / L"nand/user/save/cache").wstring()),
          "yuzu family: the shader folder itself, the driver's cache, saves and keys are not cache folders");

    const auto before = Listing(top);
    {
        const auto mixed = emudata::RemoveCaches(R, {rc[0].dir, (ryu / L"bis/user/save").wstring()});
        Check(!mixed.ok && mixed.error == "REFUSED" && Listing(top) == before, "a list with one wrong folder removes nothing at all");
    }
    // One game's caches first, then everything.
    std::vector<std::wstring> one, expectGone;
    for (const auto& c : rc)
        if (c.titleId == "0100152000022000") one.push_back(c.dir);
    const auto r1 = emudata::RemoveCaches(R, one);
    Check(r1.ok && r1.items == 2 && r1.bytes == 9000 + 100 + 4000 + 2000 + 7000, "one game's two cache folders removed, bytes counted");
    std::vector<std::wstring> rest;
    for (const auto& c : emudata::ListCaches(R)) rest.push_back(c.dir);
    Check(rest.size() == 2 && emudata::RemoveCaches(R, rest).ok, "the other game's removed");
    std::vector<std::wstring> ydirs;
    for (const auto& c : yc) ydirs.push_back(c.dir);
    Check(emudata::RemoveCaches(Y, ydirs).ok, "yuzu family: both removed");

    // What must be gone is exactly the cache folders and what was in them.
    const auto after = Listing(top);
    std::set<std::wstring> gone, wrong;
    for (const auto& p : before)
        if (!after.count(p)) gone.insert(p);
    for (const auto& p : gone) {
        const bool inRyuCache = p.find(L"ryujinx\\games\\0100") == 0 && (p.find(L"\\cache\\cpu\\") != std::wstring::npos || p.find(L"\\cache\\shader\\") != std::wstring::npos);
        const bool inYuzuCache = p.find(L"eden\\shader\\0100") == 0;
        if (!inRyuCache && !inYuzuCache) wrong.insert(p);
    }
    for (const auto& p : wrong) printf("      removed but should not have been: %ls\n", p.c_str());
    Check(wrong.empty() && !gone.empty(), "nothing outside the cache folders was removed");
    Check(after.count(L"ryujinx\\games\\0100152000022000\\cache\\keep_me.txt") && after.count(L"ryujinx\\games\\0100152000022000\\updates.json") &&
              after.count(L"ryujinx\\games\\0100152000022000\\Config.json") && after.count(L"ryujinx\\system\\prod.keys") &&
              after.count(L"eden\\keys\\prod.keys") && after.count(L"eden\\shader\\nvidia\\GLCache\\x.bin") &&
              after.count(L"ryujinx\\games\\not-a-title\\cache\\cpu\\x.cache") &&
              after.count(L"ryujinx\\bis\\system\\Contents\\registered\\0123.nca\\00"),
          "keys, firmware, per-game files and look-alike folders are all still there");
    Check(after.count(L"ryujinx\\bis\\user\\save\\0000000000000001\\0\\save.bin") && after.count(L"ryujinx\\bis\\user\\save\\0000000000000003\\1\\save.bin") &&
              after.count(L"eden\\nand\\user\\save\\0000000000000000\\12891799A5FE3F62B8C48EEF8A729DB5\\0100152000022000\\slot0.sav"),
          "every save is still there (also the ones the junction pointed at)");
    Check(emudata::ListCaches(R).empty() && emudata::ListCaches(Y).empty(), "no cache is left to list");

    // -- backup of the saves --
    const fs::path out = top / L"copies";
    fs::create_directories(out);
    const auto ryuBefore = Listing(ryu), yuzuBefore = Listing(yuzu);
    Check(emudata::SavesSize(R) == 2 * (1000 + 2000 + 3000) + 3 * 16 + 2 * fs::file_size(ryu / L"bis/system/save/8000000000000000/0/imkvdb.arc"),
          "Ryujinx: size of the saves, their metadata and their index");
    const auto b1 = emudata::BackupSaves(R, (out / L"r1").wstring());
    Check(b1.ok && b1.bytes == emudata::SavesSize(R) && b1.items == 11, "Ryujinx: all of it copied");
    const auto copy = Listing(out / L"r1");
    bool onlySaves = true;
    for (const auto& p : copy)
        if (p.find(L"bis\\user\\save") != 0 && p.find(L"bis\\system\\save\\8000000000000000") != 0 && p != L"bis\\" && p != L"bis\\user\\" &&
            p != L"bis\\system\\" && p != L"bis\\system\\save\\")
            onlySaves = false;
    Check(onlySaves && !copy.count(L"system\\prod.keys") && !copy.count(L"Config.json"), "the copy holds saves only: no keys, firmware or settings");
    Check(!emudata::BackupSaves(R, (out / L"r1").wstring()).ok, "an existing copy is never written over");
    Check(emudata::BackupSaves(R, (ryu / L"inside").wstring()).error == "REFUSED", "a copy inside the emulator's own folder is refused");
    const auto b2 = emudata::BackupSaves(Y, (out / L"y1").wstring());
    Check(b2.ok && b2.bytes == 300 + 1200 + 700 + 90 && fs::exists(out / L"y1/nand/user/save/0000000000000000/12891799A5FE3F62B8C48EEF8A729DB5/01007ef00011e000/slot0.sav") &&
              !fs::exists(out / L"y1/keys"),
          "yuzu family: the save folder copied with its layout, nothing else");
    Check(Listing(ryu) == ryuBefore && Listing(yuzu) == yuzuBefore && fs::file_size(ryu / L"bis/user/save/0000000000000002/0/save.bin") == 2000,
          "the originals are untouched");

    // -- one user's saves --
    std::string err;
    const auto ua = emudata::UserSaves(R, userA, err), ub = emudata::UserSaves(R, userB, err);
    Check(err.empty() && ua.size() == 2 && ub.size() == 1 && ub[0].first == "01007ef00011e000" &&
              ub[0].second.find(L"0000000000000003") != std::wstring::npos,
          "Ryujinx: the index says which numbered saves are each user's");
    const auto e1 = emudata::ExportUserSaves(R, userB, (out / L"userB").wstring());
    Check(e1.ok && e1.bytes == 6000 && fs::exists(out / L"userB/01007ef00011e000/0/save.bin") && !fs::exists(out / L"userB/0100152000022000"),
          "Ryujinx: one user's saves exported by title, the other user's left out");
    fs::remove(ryu / L"bis/system/save/8000000000000000/0/imkvdb.arc");
    fs::remove(ryu / L"bis/system/save/8000000000000000/1/imkvdb.arc");
    emudata::UserSaves(R, userA, err);
    Check(err == "NO_INDEX", "Ryujinx without its index: said, not guessed");
    Check(fs::path(emudata::UserSaveFolder(Y, userB)) == (yuzu / L"nand/user/save/0000000000000000/12891799A5FE3F62B8C48EEF8A729DB5").make_preferred(),
          "yuzu family: the user's folder is the id back to front");
    const auto e2 = emudata::ExportUserSaves(Y, userB, (out / L"userBy").wstring());
    Check(e2.ok && e2.bytes == 1900 && e2.items == 2, "yuzu family: that folder exported");
    Check(emudata::ExportUserSaves(Y, userA, (out / L"userAy").wstring()).error == "MISSING", "a user with no saves: nothing made");

    // -- moving a game's file --
    const auto m1 = emudata::MoveGameFile((lib1 / L"Stand-in Game [0100152000022000].nsp").wstring(), lib2.wstring());
    Check(m1.ok && m1.bytes == 123456 && !fs::exists(lib1 / L"Stand-in Game [0100152000022000].nsp") &&
              fs::file_size(lib2 / L"Stand-in Game [0100152000022000].nsp") == 123456 && Listing(lib2).size() == 1,
          "moved: there with its full size, gone from the old place, no temporary file left");
    Put(lib1 / L"Stand-in Game [0100152000022000].nsp", 500);
    const auto m2 = emudata::MoveGameFile((lib1 / L"Stand-in Game [0100152000022000].nsp").wstring(), lib2.wstring());
    Check(!m2.ok && m2.error == "EXISTS" && fs::file_size(lib1 / L"Stand-in Game [0100152000022000].nsp") == 500 &&
              fs::file_size(lib2 / L"Stand-in Game [0100152000022000].nsp") == 123456,
          "a file of that name already there: refused, both files as they were");
    Check(emudata::MoveGameFile((lib1 / L"nothing.nsp").wstring(), lib2.wstring()).error == "MISSING" &&
              emudata::MoveGameFile((lib1 / L"Stand-in Game [0100152000022000].nsp").wstring(), (top / L"no-such").wstring()).error == "MISSING" &&
              emudata::MoveGameFile((lib1 / L"Stand-in Game [0100152000022000].nsp").wstring(), lib1.wstring()).error == "REFUSED",
          "a missing file, a missing folder and the folder it is already in are refused");
    return failures;
}

static const char* Size(uint64_t b, char* buf, size_t n) {
    snprintf(buf, n, "%8.1f MB", b / 1048576.0);
    return buf;
}

static int Sizes(wchar_t family, const std::wstring& dir) {
    const emudata::Root r{"real", static_cast<char>(family), dir};
    char buf[40];
    uint64_t total = 0;
    for (const auto& c : emudata::ListCaches(r)) {
        printf("%s  %s  ...%ls\n", Size(c.bytes, buf, sizeof buf), c.titleId.c_str(), c.dir.substr(dir.size()).c_str());
        total += c.bytes;
    }
    printf("%s  caches in all\n", Size(total, buf, sizeof buf));
    for (const auto& p : emudata::SaveParts(r)) printf("%s  %ls\n", Size(emudata::TreeSize(dir + L"\\" + p), buf, sizeof buf), p.c_str());
    printf("%s  saves in all\n", Size(emudata::SavesSize(r), buf, sizeof buf));
    std::string err;
    for (const User& u : {User{"00000000000000010000000000000000", L""}}) {
        const auto saves = emudata::UserSaves(r, u.id, err);
        printf("first user: %zu save folders%s%s\n", saves.size(), err.empty() ? "" : ", error ", err.c_str());
    }
    return 0;
}

static int NetTest() {
    const pcsys::Net n = pcsys::ReadNet();
    // The values identify the owner's machine and network: only whether each could be read.
    auto has = [](const std::wstring& s) { return s.empty() ? "-" : "read"; };
    printf("kind %d (0 none, 1 Wi-Fi, 2 cable)\n", n.kind);
    printf("mac %s  device %s  security %s  band %s  channel %s  signal %s\n", has(n.mac), has(n.device), has(n.security), has(n.band),
           has(n.channel), n.signal >= 0 ? "read" : "-");
    printf("ipv4 %s  subnet %s  gateway4 %s  dns1_4 %s  dns2_4 %s\n", has(n.ipv4), has(n.subnet), has(n.gateway4), has(n.dns1_4), has(n.dns2_4));
    printf("ipv6 %s  gateway6 %s  dns1_6 %s  dns2_6 %s\n", has(n.ipv6), has(n.gateway6), has(n.dns1_6), has(n.dns2_6));
    Check(n.kind != 0 ? !n.mac.empty() && !n.ipv4.empty() && !n.subnet.empty() && !n.gateway4.empty() : true,
          "a connection has an address, a mask and a gateway");
    Check(n.mac.empty() || n.mac.size() == 17, "the MAC address has the usual shape");
    const int p = pcsys::Probe();
    printf("connection test against http://%ls%ls: %d (0 answered as expected, 1 no answer, 2 another answer)\n", pcsys::kProbeHost,
           pcsys::kProbePath, p);
    Check(p >= 0, "the test ran");
    return failures;
}

static int AudioTest() {
    const pcsys::Audio a = pcsys::ReadAudio();
    printf("playback device: %s%s   recording device: %s\n", a.output.empty() ? "none" : "named", a.headphones ? " (headphones)" : "",
           a.input.empty() ? "none" : "named");
    Check(a.ok, "the sound devices could be asked");
    float level = -1;
    if (pcsys::MicVolume(level)) {
        printf("microphone level %.0f %%\n", level * 100.0);
        Check(pcsys::SetMicVolume(level), "the same level written back");
        float again = -1;
        Check(pcsys::MicVolume(again) && fabsf(again - level) < 0.006f, "and it reads the same afterwards");
    } else {
        printf("no recording device: the level could not be read\n");
    }
    Check(pcsys::MicMeterLevel() < 0, "the level meter is not running (it is never started here)");
    return failures;
}

// The stand-in "game": a shared playback stream fed with silence.
static int Silent(int seconds) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
        FAILED(client->GetMixFormat(&fmt)) || FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, fmt, nullptr)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))))
        return 3;
    UINT32 size = 0;
    client->GetBufferSize(&size);
    client->Start();
    for (int i = 0; i < seconds * 10; i++) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        BYTE* data = nullptr;
        if (size > padding && SUCCEEDED(render->GetBuffer(size - padding, &data))) render->ReleaseBuffer(size - padding, AUDCLNT_BUFFERFLAGS_SILENT);
        Sleep(100);
    }
    client->Stop();
    return 0;
}

static bool WaitState(DWORD pid, bool wantMute, float wantVolume, int ms = 4000) {
    for (int waited = 0; waited < ms; waited += 50) {
        bool m = false;
        float v = -1;
        if (pcsys::SessionState(pid, m, v) && m == wantMute && (wantVolume < 0 || fabsf(v - wantVolume) < 0.01f)) return true;
        Sleep(50);
    }
    return false;
}

static PROCESS_INFORMATION StartSilent(int seconds) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" silent " + std::to_wstring(seconds);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    CreateProcessW(self, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    return pi;
}

static int Mixer(const std::wstring& folder) {
    dataDir = folder;
    fs::create_directories(folder);
    const std::wstring undo = folder + L"\\mixer_undo.txt";
    fs::remove(undo);
    PROCESS_INFORMATION pi = StartSilent(40);
    if (!pi.hProcess) return 2;
    bool m = false;
    float v = -1;
    for (int i = 0; i < 100 && !pcsys::SessionState(pi.dwProcessId, m, v); i++) Sleep(50);
    Check(pcsys::SessionState(pi.dwProcessId, m, v), "the stand-in has a mixer entry");
    printf("      stand-in entry at the start: muted %d, volume %.2f\n", m, static_cast<double>(v));
    const float startVolume = v;

    // Both switches on, headphones the default device.
    cfg.console["sound.mute_on_unplug"] = "on";
    cfg.console["sound.limit_headphones"] = "on";
    pcsys::TestHeadphones(1);
    pcsys::GameStarted(pi.dwProcessId);
    if (startVolume > pcsys::kHeadphoneCap) {
        Check(WaitState(pi.dwProcessId, false, pcsys::kHeadphoneCap), "headphones + cap on: the volume comes down to the cap");
        Check(fs::exists(undo), "what was changed is noted, in case the game ends like this");
    }
    Check(!pcsys::GameMuted(), "not muted while the headphones are there");

    // The headphones go: the notification arrives, the default is now something else.
    pcsys::TestHeadphones(0);
    pcsys::TestDeviceChanged();
    Check(WaitState(pi.dwProcessId, true, startVolume), "headphones gone: muted, and the cap lifted (no headphones to protect)");
    Check(pcsys::GameMuted(), "the menu can tell that it is muted");

    // The user unmutes from the menu.
    pcsys::GameUnmute();
    Check(WaitState(pi.dwProcessId, false, -1) && !pcsys::GameMuted(), "unmuted from the menu");
    Sleep(1300);
    Check(pcsys::SessionState(pi.dwProcessId, m, v) && !m, "and it stays unmuted while nothing else changes");

    // Back on, off again, then the device comes back by itself.
    pcsys::TestHeadphones(1);
    pcsys::TestDeviceChanged();
    Check(WaitState(pi.dwProcessId, false, startVolume > pcsys::kHeadphoneCap ? pcsys::kHeadphoneCap : -1.0f), "headphones back: capped again");
    pcsys::TestHeadphones(0);
    pcsys::TestDeviceChanged();
    Check(WaitState(pi.dwProcessId, true, -1), "gone again: muted again");
    pcsys::TestHeadphones(1);
    pcsys::TestDeviceChanged();
    Check(WaitState(pi.dwProcessId, false, -1) && !pcsys::GameMuted(), "the device comes back: unmuted by itself");

    // The switch turned off in the menu while capped.
    pcsys::SetAudioPolicy(true, false);
    Check(WaitState(pi.dwProcessId, false, startVolume), "cap switched off: the volume is back where it was");
    Sleep(300); // the note is dropped just after the volume is put back
    Check(!fs::exists(undo), "nothing left to undo: the note is gone");

    // With the mute switch off nothing happens on unplugging.
    pcsys::SetAudioPolicy(false, false);
    pcsys::TestHeadphones(0);
    pcsys::TestDeviceChanged();
    Sleep(600);
    Check(pcsys::SessionState(pi.dwProcessId, m, v) && !m && fabsf(v - startVolume) < 0.01f, "both switches off: unplugging changes nothing");

    // The game ends while muted: the entry must not stay muted for its next start.
    pcsys::SetAudioPolicy(true, false);
    pcsys::TestHeadphones(1);
    pcsys::TestDeviceChanged();
    Sleep(300);
    pcsys::TestHeadphones(0);
    pcsys::TestDeviceChanged();
    Check(WaitState(pi.dwProcessId, true, -1), "muted once more");
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    Sleep(1500);
    pcsys::GameStopped();
    const bool noted = fs::exists(undo);
    printf("      after the stand-in ended while muted: %s\n", noted ? "a note is left for the next start" : "its entry was unmuted at once");
    cfg.console["sound.limit_headphones"] = "off"; // as the menu left it
    PROCESS_INFORMATION again = StartSilent(8);
    for (int i = 0; i < 100 && !pcsys::SessionState(again.dwProcessId, m, v); i++) Sleep(50);
    printf("      the next stand-in's entry as Windows remembered it: muted %d, volume %.2f\n", m, static_cast<double>(v));
    pcsys::TestHeadphones(1);
    pcsys::GameStarted(again.dwProcessId);
    Check(WaitState(again.dwProcessId, false, startVolume, 6000), "the next start is not muted and has its volume");
    Sleep(1200);
    Check(!fs::exists(undo), "and no note is left");
    pcsys::GameStopped();
    TerminateProcess(again.hProcess, 0);
    pcsys::TestHeadphones(-1);
    return failures;
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    int rc = 2;
    if (mode == L"pure") rc = Pure();
    else if (mode == L"standin" && argc > 2) rc = Standin(argv[2], false);
    else if (mode == L"tree" && argc > 2) rc = Standin(argv[2], true);
    else if (mode == L"sizes" && argc > 3) rc = Sizes(argv[2][0], argv[3]);
    else if (mode == L"net") rc = NetTest();
    else if (mode == L"audio") rc = AudioTest();
    else if (mode == L"mixer" && argc > 2) rc = Mixer(argv[2]);
    else if (mode == L"silent" && argc > 2) return Silent(_wtoi(argv[2]));
    else printf("usage: see the top of tools\\setc_test.cpp\n");
    if (rc == 0) printf("all passed\n");
    else if (failures) printf("%d FAILED\n", failures);
    return rc;
}
