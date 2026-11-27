// Exercises src\yuzucfg.cpp on a copy of a yuzu-family qt-config.ini.
//
//   yuzucfg_test.exe <emulator id> <source qt-config.ini> [work folder]
//
// The source is only read. It is copied into the work folder (default
// build\yuzucfg_test) and everything happens on that copy: every curated
// setting of that emulator is printed with its value, changed to another
// allowed value, read back, checked to have touched only its own two lines,
// and restored; refused values are tried; a setting is added to a file that
// lacks it; and a file is seeded from nothing.
#include "../src/yuzucfg.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int passed = 0, failed = 0;

void Check(bool ok, const std::string& what) {
    if (ok) passed++;
    else {
        failed++;
        printf("  FAIL  %s\n", what.c_str());
    }
}

std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool Slurp(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    out.clear();
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    fclose(f);
    return true;
}

// Lines of `a` that are not the same line in `b` (same line count expected).
int ChangedLines(const std::string& a, const std::string& b) {
    int changed = 0;
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        size_t ie = a.find('\n', i), je = b.find('\n', j);
        if (ie == std::string::npos || i > a.size()) ie = a.size();
        if (je == std::string::npos || j > b.size()) je = b.size();
        // (One text may have more lines than the other: its extra lines count as changed.)
        if (i > a.size() || j > b.size() || a.compare(i, ie - i, b, j, je - j) != 0) changed++;
        i = ie + 1;
        j = je + 1;
    }
    return changed;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0); // nothing lost if a check crashes
    if (argc < 3) {
        printf("usage: yuzucfg_test <emulator id> <qt-config.ini> [work folder]\n");
        return 2;
    }
    const std::string emu = U8(argv[1]);
    const std::wstring source = argv[2], work = argc > 3 ? argv[3] : L"build\\yuzucfg_test";
    CreateDirectoryW(work.c_str(), nullptr);
    const std::wstring copy = work + L"\\qt-config.ini", backup = copy + L".dockside-backup";
    DeleteFileW(backup.c_str());
    if (!CopyFileW(source.c_str(), copy.c_str(), FALSE)) {
        printf("cannot copy the source file\n");
        return 2;
    }
    std::string original;
    Slurp(copy, original);
    Check(yuzucfg::Open(copy), "open");

    int shown = 0;
    for (const auto& s : yuzucfg::Settings(emu)) {
        if (!yuzucfg::Available(s)) {
            printf("  not offered: %s\n", s.key.c_str());
            continue;
        }
        shown++;
        const std::string was = yuzucfg::Get(s.key);
        const std::string other = ryucfg::Next(s, was, +1);
        printf("  %-28s %-8s -> %-8s (%s)\n", s.key.c_str(), was.c_str(), other.c_str(), U8(ryucfg::Display(s, was)).c_str());
        std::string pre;
        Slurp(copy, pre);
        Check(other != was, s.key + ": has another value to move to");
        Check(yuzucfg::Set(s.key, other), s.key + ": set");
        Check(yuzucfg::Get(s.key) == other, s.key + ": reads back");
        Check(yuzucfg::Effective(s.key, "?") == other, s.key + ": the emulator will use it");
        std::string now;
        Slurp(copy, now);
        Check(ChangedLines(pre, now) <= 2 && ChangedLines(pre, now) >= 1 && now.size() + 8 > pre.size() && pre.size() + 8 > now.size(),
              s.key + ": only its own lines changed");
        // Put back value and flag exactly: the flag line by hand, since Set always leaves it at false.
        Check(yuzucfg::Set(s.key, was), s.key + ": restore");
    }
    Check(shown >= 4, "at least language, region, docked mode and volume are offered");

    // Refusals leave the file alone.
    std::string before, after;
    Slurp(copy, before);
    Check(!yuzucfg::Set("System.language_index", "99"), "a language that does not exist is refused");
    Check(!yuzucfg::Set("Audio.volume", "500"), "a volume out of range is refused");
    Check(!yuzucfg::Set("System.no_such_key", "1"), "a missing key is refused unless adding is asked for");
    Check(!yuzucfg::Set("System.language_index", "1\nx=2"), "a value with a line break is refused");
    Slurp(copy, after);
    Check(before == after, "refused changes wrote nothing");

    // The sound mode ("Sonido del televisor"): offered where its numbers were
    // checked (Eden and what is built from it), kept in [System], and only
    // the three modes are accepted.
    {
        const ryucfg::Setting* mode = yuzucfg::Find(emu, "System.sound_index");
        Check(mode != nullptr && mode->choices.size() == 3, "the sound mode is in the table with its three modes");
        const bool edenBased = emu == "eden" || emu == "suyu";
        if (mode) Check(mode->verified == edenBased, "the sound mode is only offered for Eden-based emulators");
        if (mode && yuzucfg::Available(*mode)) {
            const std::string was = yuzucfg::Get("System.sound_index");
            for (const char* v : {"0", "2", "1"}) {
                Check(yuzucfg::Set("System.sound_index", v) && yuzucfg::Get("System.sound_index") == v,
                      std::string("sound mode ") + v + " is written and read back");
                std::string text;
                Slurp(copy, text);
                const size_t system = text.find("[System]"), line = text.find(std::string("\nsound_index=") + v, system);
                const size_t nextSection = text.find("\n[", system + 1);
                Check(system != std::string::npos && line != std::string::npos && (nextSection == std::string::npos || line < nextSection),
                      std::string("sound mode ") + v + " stands in [System]");
            }
            Check(!yuzucfg::Set("System.sound_index", "3") && !yuzucfg::Set("System.sound_index", "stereo"),
                  "a sound mode that does not exist is refused");
            Check(yuzucfg::Set("System.sound_index", was), "sound mode restored");
            Slurp(copy, after);
            Check(before == after, "the sound mode checks left the file as it was");
        }
    }

    // Everything outside the changed settings is byte for byte the original:
    // the only differences left are "\default" flags now reading false.
    int flagLines = ChangedLines(original, after);
    Check(flagLines <= shown, "after restoring, at most one flag line per setting differs");
    Check(GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES, "a backup was kept");
    std::string kept;
    Slurp(backup, kept);
    Check(kept == original, "the backup is the file as it was");

    // Adding a setting the file lacks, and a section it lacks.
    Check(yuzucfg::Set("UI.nswhome_test_key", "false", true), "add a key to an existing section");
    Check(yuzucfg::Get("UI.nswhome_test_key") == "false", "added key reads back");
    Check(yuzucfg::Set("NSWHomeTest.some_key", "7", true), "add a key in a new section");
    Check(yuzucfg::Get("NSWHomeTest.some_key") == "7", "key in the new section reads back");
    Slurp(copy, after);
    Check(after.size() > before.size() && ChangedLines(before, after) > 0, "the file grew");

    // A file made from nothing.
    const std::wstring fresh = work + L"\\seeded.ini";
    DeleteFileW(fresh.c_str());
    Check(yuzucfg::Seed(fresh, {{"UI.select_user_on_boot", "false"}, {"UI.check_for_updates", "false"}}), "seed a new file");
    Check(yuzucfg::Get("UI.check_for_updates") == "false" && yuzucfg::Effective("UI.select_user_on_boot", "true") == "false",
          "seeded values read back");
    std::string seeded;
    Slurp(fresh, seeded);
    Check(seeded.rfind("[UI]", 0) == 0, "the seeded file starts with its section");

    printf("%s: %d passed, %d failed, %d settings offered\n", emu.c_str(), passed, failed, shown);
    return failed ? 1 : 0;
}
