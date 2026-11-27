// Checks src/content.cpp on copies and stand-ins: nothing of the emulators'
// own folders is written, no game data is made or copied.
//
//   content_test detect <keys folder> <cache file> <folder>...
//       What every dump in those folders holds (names, ids, versions), read only.
//   content_test write <work folder> <ryujinx games folder to copy JSON from> <yuzu ini>...
//       Every writer, turned on and back off, on copies inside the work folder;
//       prints PASS/FAIL and, for each file, its bytes before / after.
#include "../src/content.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace content;

static int fails = 0;

static void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static std::string Slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void Put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static std::wstring Ws(const fs::path& p) {
    std::wstring s = p.wstring();
#ifndef _WIN32
    for (wchar_t& c : s)
        if (c == L'/') c = L'\\';
#endif
    return s;
}

static unsigned long long Fnv(const std::string& s) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

static void Show(const char* label, const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        printf("    %s: (no file)\n", label);
        return;
    }
    const std::string s = Slurp(p);
    printf("    %s: %zu bytes, fnv %016llx\n", label, s.size(), Fnv(s));
}

// The file's bytes before, after a change and after taking it back.
static void RoundTrip(const char* what, const fs::path& file, const std::function<bool()>& on, const std::function<bool()>& off,
                      const std::function<void()>& between = nullptr) {
    std::error_code ec;
    const bool existed = fs::exists(file, ec);
    const std::string before = existed ? Slurp(file) : std::string();
    printf("-- %s\n", what);
    Show("before", file);
    const bool a = on();
    Show("changed", file);
    const std::string mid = fs::exists(file, ec) ? Slurp(file) : std::string("<none>");
    if (between) between();
    const bool b = off();
    Show("back", file);
    const bool same = existed ? fs::exists(file, ec) && Slurp(file) == before : !fs::exists(file, ec);
    const bool sidecar = !fs::exists(fs::path(file.wstring() + L".dockside-made"), ec);
    Check(a && b, (std::string(what) + ": both writes succeed").c_str());
    Check(mid != (existed ? before : std::string("<none>")), (std::string(what) + ": the change reached the file").c_str());
    Check(same, (std::string(what) + ": back to the original bytes").c_str());
    Check(sidecar, (std::string(what) + ": nothing left in .dockside-made").c_str());
}

static int Detect(int argc, char** argv) {
    const std::wstring keys = fs::path(argv[2]).wstring();
    SetCache(fs::path(argv[3]).wstring());
    std::vector<std::wstring> dirs;
    for (int i = 4; i < argc; i++) dirs.push_back(fs::path(argv[i]).wstring());
    for (const Dump& d : Collect(dirs, 2, keys)) {
        printf("%s\n", ToUtf8(fs::path(d.file).filename().wstring()).c_str());
        if (!d.error.empty()) printf("    error %s\n", d.error.c_str());
        for (const Meta& m : d.metas) {
            const char* t = m.type == kGame ? "game" : m.type == kUpdate ? "update" : m.type == kDlc ? "dlc" : "other";
            printf("    %-6s id %s  for %s  v%u", t, Hex16(m.id).c_str(), Hex16(m.app).c_str(), m.version);
            if (!m.display.empty()) printf("  \"%s\"", ToUtf8(m.display).c_str());
            if (!m.nca.empty()) printf("  data %s (title %s)", m.nca.c_str(), Hex16(m.ncaTitle).c_str());
            if (m.type == kDlc) printf("  name \"%s\"", ToUtf8(DlcName(d.file)).c_str());
            printf("\n");
        }
        printf("    %s\n", AddOnOnly(d) ? "not a game of its own" : "a game");
    }
    SaveCache();
    return 0;
}

static int Write(int argc, char** argv) {
    const fs::path work = fs::path(argv[2]);
    const fs::path ownGames = fs::path(argv[3]);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work);

    // ---- Ryujinx family on a stand-in data folder
    const fs::path ryu = work / "ryujinx";
    const std::wstring R = Ws(ryu);
    const uint64_t mk8 = 0x0100152000022000ull, ykw = 0x0100c0000ceea000ull, none = 0x0100aaaa00000000ull;
    // The emulator's own files for two games (copied: settings, not game data).
    for (const char* f : {"0100152000022000/updates.json", "0100152000022000/dlc.json", "0100c0000ceea000/mods.json",
                          "0100c0000ceea000/updates.json", "0100c0000ceea000/dlc.json"})
        if (fs::exists(ownGames / f, ec)) Put(ryu / "games" / f, Slurp(ownGames / f));
    // Stand-in dumps: empty files with the names such files have.
    const fs::path lib = work / "lib";
    const fs::path upd2 = lib / "Mario Kart 8 Deluxe [0100152000022800][v3014656].nsp";
    const fs::path dlc2 = lib / "Mario Kart 8 Deluxe [DLC Test Pack][0100152000023002][v0][EU].nsp";
    Put(upd2, "");
    Put(dlc2, "");

    // Every per-game file the emulator wrote (copies in a second stand-in data
    // folder): a change and its undoing rewrite the whole file, so getting
    // the original bytes back shows the writer writes as the emulator does.
    {
        const fs::path all = work / "ryujinx_all";
        const std::wstring A = Ws(all);
        for (auto& g : fs::directory_iterator(ownGames, ec)) {
            uint64_t tid = 0;
            if (!ParseHex16(ToUtf8(g.path().filename().wstring()), tid)) continue;
            const std::string t = ToUtf8(g.path().filename().wstring());
            for (const char* f : {"updates.json", "dlc.json", "mods.json"})
                if (fs::exists(g.path() / f, ec)) Put(all / "games" / t / f, Slurp(g.path() / f));
            if (fs::exists(all / "games" / t / "updates.json", ec)) {
                const std::wstring sel = ryu::Read(A, tid).selected;
                RoundTrip(("every game: " + t + " updates.json").c_str(), all / "games" / t / "updates.json",
                          [&] { return ryu::SelectUpdate(A, tid, Ws(upd2)); }, [&] { return ryu::SelectUpdate(A, tid, sel); });
            }
            const ryu::State s = ryu::Read(A, tid);
            for (const ryu::State::Dlc& d : s.dlc)
                RoundTrip(("every game: " + t + " dlc.json item " + Hex16(d.id)).c_str(), all / "games" / t / "dlc.json",
                          [&] { return ryu::SetDlc(A, tid, d.file, d.nca, d.id, !d.on); },
                          [&] { return ryu::SetDlc(A, tid, d.file, d.nca, d.id, d.on); });
            if (fs::exists(all / "games" / t / "mods.json", ec)) {
                Put(all / "mods/contents" / t / "StandIn/romfs/a.txt", "x");
                for (const Mod& m : ryu::Mods(A, tid))
                    RoundTrip(("every game: " + t + " mods.json").c_str(), all / "games" / t / "mods.json",
                              [&] { return ryu::SetMod(A, tid, m, false); }, [&] { return ryu::SetMod(A, tid, m, true); });
            }
        }
    }

    const ryu::State st0 = ryu::Read(R, mk8);
    printf("MK8 now: update \"%s\", %zu listed, %zu DLC\n", ToUtf8(st0.selected).c_str(), st0.paths.size(), st0.dlc.size());
    const std::wstring origSel = st0.selected;

    RoundTrip("Ryujinx: another update chosen, then the first again", ryu / "games/0100152000022000/updates.json",
              [&] { return ryu::SelectUpdate(R, mk8, Ws(upd2)); }, [&] { return ryu::SelectUpdate(R, mk8, origSel); },
              [&] {
                  const ryu::State s = ryu::Read(R, mk8);
                  Check(s.selected == Ws(upd2), "Ryujinx: selected is the new update");
              });
    RoundTrip("Ryujinx: no update, then the first again", ryu / "games/0100152000022000/updates.json",
              [&] { return ryu::SelectUpdate(R, mk8, L""); }, [&] { return ryu::SelectUpdate(R, mk8, origSel); });
    RoundTrip("Ryujinx: a game without updates.json gets one, then none", ryu / "games/0100aaaa00000000/updates.json",
              [&] { return ryu::SelectUpdate(R, none, Ws(upd2)); }, [&] { return ryu::SelectUpdate(R, none, L""); });
    Check(!fs::exists(ryu / "games/0100aaaa00000000", ec), "Ryujinx: the folder made for it is gone again");

    if (!st0.dlc.empty()) {
        const ryu::State::Dlc d = st0.dlc[0];
        RoundTrip("Ryujinx: a listed DLC off, then on", ryu / "games/0100152000022000/dlc.json",
                  [&] { return ryu::SetDlc(R, mk8, d.file, d.nca, d.id, false); },
                  [&] { return ryu::SetDlc(R, mk8, d.file, d.nca, d.id, true); },
                  [&] { Check(!ryu::Read(R, mk8).dlc[0].on, "Ryujinx: the DLC reads as off"); });
    }
    RoundTrip("Ryujinx: an unlisted DLC on, then off", ryu / "games/0100152000022000/dlc.json",
              [&] { return ryu::SetDlc(R, mk8, Ws(dlc2), "/0123456789abcdef0123456789abcdef.nca", 0x0100152000023002ull, true); },
              [&] { return ryu::SetDlc(R, mk8, Ws(dlc2), "/0123456789abcdef0123456789abcdef.nca", 0x0100152000023002ull, false); });
    RoundTrip("Ryujinx: a game without dlc.json", ryu / "games/0100aaaa00000000/dlc.json",
              [&] { return ryu::SetDlc(R, none, Ws(dlc2), "/00.nca", 0x0100aaaa00001001ull, true); },
              [&] { return ryu::SetDlc(R, none, Ws(dlc2), "/00.nca", 0x0100aaaa00001001ull, false); });

    // Stand-in mods and cheats where the emulator looks for them.
    Put(ryu / "mods/contents/0100c0000ceea000/TestMod/romfs/readme.txt", "stand-in\n");
    Put(ryu / "mods/contents/0100c0000ceea000/TestMod/cheats/0123456789ABCDEF.txt",
        "[Infinite HP]\r\n04000000 00000000 00000064\r\n\r\n[Moon Jump]\r\n04000000 00000010 00000001\r\n");
    Put(ryu / "sdcard/atmosphere/contents/0100C0000CEEA000/Pack/Nested Mod/exefs/main.ips", "PATCHEOF");
    Put(ryu / "sdcard/atmosphere/contents/0100C0000CEEA000/Pack/Mod+Plus/romfs/a.bin", "x");
    const std::vector<Mod> mods = ryu::Mods(R, ykw);
    printf("YKW mods found: %zu\n", mods.size());
    for (const Mod& m : mods) printf("    %s [%s] %s  %s\n", ToUtf8(m.name).c_str(), ToUtf8(m.kinds).c_str(), m.on ? "on" : "off", ToUtf8(m.path).c_str());
    Check(mods.size() == 3, "Ryujinx: three stand-in mods found (nested ones too)");
    for (const Mod& m : mods)
        RoundTrip(("Ryujinx: mod " + ToUtf8(m.name) + " off, then on").c_str(), ryu / "games/0100c0000ceea000/mods.json",
                  [&] { return ryu::SetMod(R, ykw, m, false); }, [&] { return ryu::SetMod(R, ykw, m, true); },
                  [&] {
                      for (const Mod& x : ryu::Mods(R, ykw))
                          if (x.path == m.path) Check(!x.on, "Ryujinx: the mod reads as off");
                  });
    {
        // The game without mods.json: one made, then taken away.
        Put(ryu / "mods/contents/0100aaaa00000000/Solo/exefs/main.ips", "PATCHEOF");
        const std::vector<Mod> solo = ryu::Mods(R, none);
        Check(solo.size() == 1, "Ryujinx: the stand-in mod of the game without mods.json");
        if (!solo.empty())
            RoundTrip("Ryujinx: mods.json made and taken away", ryu / "games/0100aaaa00000000/mods.json",
                      [&] { return ryu::SetMod(R, none, solo[0], false); }, [&] { return ryu::SetMod(R, none, solo[0], true); });
    }
    const std::vector<Cheat> cheats = ryu::Cheats(R, ykw);
    for (const Cheat& c : cheats) printf("    cheat \"%s\" key \"%s\" %s\n", ToUtf8(c.name).c_str(), c.key.c_str(), c.on ? "on" : "off");
    Check(cheats.size() == 2 && cheats[0].key == "0123456789ABCDEF-<Infinite HP Cheat>", "Ryujinx: cheats and their keys");
    if (cheats.size() == 2) {
        RoundTrip("Ryujinx: enabled.txt made for a cheat, then taken away", ryu / "mods/contents/0100c0000ceea000/cheats/enabled.txt",
                  [&] { return ryu::SetCheat(R, ykw, cheats[1].key, true); }, [&] { return ryu::SetCheat(R, ykw, cheats[1].key, false); },
                  [&] { Check(ryu::Cheats(R, ykw)[1].on, "Ryujinx: the cheat reads as on"); });
        Check(!fs::exists(ryu / "mods/contents/0100c0000ceea000/cheats", ec), "Ryujinx: the cheats folder made for it is gone");
        Put(ryu / "mods/contents/0100c0000ceea000/cheats/enabled.txt", "0123456789ABCDEF-<Infinite HP Cheat>\r\n");
        RoundTrip("Ryujinx: a cheat added to an enabled.txt there was", ryu / "mods/contents/0100c0000ceea000/cheats/enabled.txt",
                  [&] { return ryu::SetCheat(R, ykw, cheats[1].key, true); }, [&] { return ryu::SetCheat(R, ykw, cheats[1].key, false); });
        RoundTrip("Ryujinx: a cheat that was on, off and on", ryu / "mods/contents/0100c0000ceea000/cheats/enabled.txt",
                  [&] { return ryu::SetCheat(R, ykw, cheats[0].key, false); }, [&] { return ryu::SetCheat(R, ykw, cheats[0].key, true); });
    }

    // ---- yuzu family on copies of the settings files
    for (int i = 4; i < argc; i++) {
        const fs::path ini = work / ("yuzu" + std::to_string(i)) / "config" / "qt-config.ini";
        Put(ini, Slurp(argv[i]));
        const std::wstring I = Ws(ini), D = Ws(ini.parent_path().parent_path());
        printf("== %s\n", argv[i]);
        const uint64_t botw = 0x01007ef00011e000ull;
        RoundTrip("yuzu: Update@196608 off, then on", ini, [&] { return yuzu::SetDisabled(I, mk8, "Update@196608", true); },
                  [&] { return yuzu::SetDisabled(I, mk8, "Update@196608", false); },
                  [&] {
                      const yuzu::State s = yuzu::Read(I, mk8);
                      Check(s.disabled.size() == 1 && s.disabled[0] == "Update@196608", "yuzu: reads back as off");
                  });
        RoundTrip("yuzu: two off, taken back in the same order", ini,
                  [&] { return yuzu::SetDisabled(I, mk8, "DLC", true) && yuzu::SetDisabled(I, mk8, "My Mod; v2", true); },
                  [&] { return yuzu::SetDisabled(I, mk8, "DLC", false) && yuzu::SetDisabled(I, mk8, "My Mod; v2", false); },
                  [&] {
                      const yuzu::State s = yuzu::Read(I, mk8);
                      Check(s.disabled.size() == 2 && s.disabled[1] == "My Mod; v2", "yuzu: a name with ; reads back");
                  });
        RoundTrip("yuzu: a game whose entry is there and empty (Citron makes them)", ini,
                  [&] { return yuzu::SetDisabled(I, botw, "DLC", true); }, [&] { return yuzu::SetDisabled(I, botw, "DLC", false); });
        // One the emulator put there: on and back off gives it its old place.
        yuzu::SetDisabled(I, ykw, "First", true);
        yuzu::SetDisabled(I, ykw, "Second", true);
        // (forget that NSW-Home added them, as if the emulator had)
        fs::remove(fs::path(ini.wstring() + L".dockside-made"), ec);
        RoundTrip("yuzu: one of the emulator's turned on and back off", ini,
                  [&] { return yuzu::SetDisabled(I, ykw, "First", false); }, [&] { return yuzu::SetDisabled(I, ykw, "First", true); },
                  [&] { Check(yuzu::Read(I, ykw).disabled.size() == 1, "yuzu: one left off"); });
        RoundTrip("yuzu: an external content folder added and taken away", ini,
                  [&] { return yuzu::SetExtDir(I, Ws(lib), true); }, [&] { return yuzu::SetExtDir(I, Ws(lib), false); },
                  [&] {
                      const yuzu::State s = yuzu::Read(I, mk8);
                      Check(s.extDirs.size() == 1, "yuzu: the folder reads back");
                      if (!s.extDirs.empty()) printf("    folder as written: %s\n", ToUtf8(s.extDirs[0]).c_str());
                  });
        // Mods and cheats in the family's load folder.
        Put(ini.parent_path().parent_path() / "load/0100C0000CEEA000/ModA/romfs/a.txt", "x");
        Put(ini.parent_path().parent_path() / "load/0100C0000CEEA000/ModB/exefs/main.ips", "PATCHEOF");
        Put(ini.parent_path().parent_path() / "load/0100C0000CEEA000/ModB/cheats/0123456789ABCDEF.txt", "{Master}\n04000000 00000000 00000000\n[Inf HP]\n04000000 00000000 00000064\n");
        Put(ini.parent_path().parent_path() / "load/0100C0000CEEA000/OnlyCheats/cheats/0123456789abcdef.txt", "[Moon Jump]\n04000000 00000010 00000001\n");
        Put(ini.parent_path().parent_path() / "load/0100C0000CEEA000/Empty/romfs/.keep", "");
        fs::remove(ini.parent_path().parent_path() / "load/0100C0000CEEA000/Empty/romfs/.keep", ec);
        Put(ini.parent_path().parent_path() / "load/0100C0000CEEA000/cheat_loose.txt", "[Loose]\n04000000 00000000 00000001\n");
        const std::vector<Mod> ym = yuzu::Mods(D, ykw, {});
        for (const Mod& m : ym) printf("    mod %s [%s]%s\n", ToUtf8(m.name).c_str(), ToUtf8(m.kinds).c_str(), m.cheatsOnly ? " cheats only" : "");
        Check(ym.size() == 3, "yuzu: three folders that change something (the empty one is left out)");
        const std::vector<Cheat> yc = yuzu::Cheats(D, ykw, {}, true);
        for (const Cheat& c : yc) printf("    cheat \"%s\" switch \"%s\"\n", ToUtf8(c.name).c_str(), c.key.c_str());
        Check(yc.size() == 4, "yuzu: master code, two cheats and the loose file's");
        RoundTrip("yuzu: a mod folder off, then on", ini, [&] { return yuzu::SetDisabled(I, ykw, "ModB", true); },
                  [&] { return yuzu::SetDisabled(I, ykw, "ModB", false); },
                  [&] { Check(!yuzu::Mods(D, ykw, yuzu::Read(I, ykw).disabled)[1].on, "yuzu: the mod reads as off"); });
    }
    printf("%s (%d failed)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}

// What a Ryujinx-family data folder has for one game, read only.
static int List(char** argv) {
    const std::wstring dir = fs::path(argv[2]).wstring();
    uint64_t tid = 0;
    ParseHex16(argv[3], tid);
    const ryu::State st = ryu::Read(dir, tid);
    printf("update in use: %s\n", st.selected.empty() ? "(none)" : ToUtf8(fs::path(st.selected).filename().wstring()).c_str());
    for (const auto& d : st.dlc) printf("dlc %s %s\n", Hex16(d.id).c_str(), d.on ? "on" : "off");
    for (const Mod& m : ryu::Mods(dir, tid)) printf("mod %-45s %-3s [%s]\n", ToUtf8(m.name).c_str(), m.on ? "on" : "off", ToUtf8(m.kinds).c_str());
    for (const Cheat& c : ryu::Cheats(dir, tid)) printf("cheat %s %s\n", c.key.c_str(), c.on ? "on" : "off");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "list")) return List(argv);
    if (argc >= 4 && !strcmp(argv[1], "detect")) return Detect(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "write")) return Write(argc, argv);
    fprintf(stderr, "usage: content_test detect <keys> <cache> <folder>... | write <work> <ryujinx games> <ini>...\n");
    return 2;
}
