// Exercises src\ryucfg.cpp on a copy of a Ryujinx Config.json.
//
//   ryucfg_test.exe [source Config.json] [work folder]
//
// The source is only read. It is copied into the work folder (default
// build\ryucfg_test) and everything happens on that copy: every curated
// setting is printed with its value, changed to another allowed value, read
// back, and restored; at the end the copy must equal the source byte for byte.
#include "../src/ryucfg.h"
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

bool Spit(const std::wstring& path, const std::string& data) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Number of bytes that differ once the common start and end are removed: for
// a single value change this is the length of the longer of the two values.
size_t ChangedBytes(const std::string& a, const std::string& b) {
    size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) head++;
    size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) tail++;
    size_t la = a.size() - head - tail, lb = b.size() - head - tail;
    return la > lb ? la : lb;
}

const char* KindName(ryucfg::Kind k) {
    return k == ryucfg::Kind::Toggle ? "toggle" : k == ryucfg::Kind::Choice ? "choice" : "number";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::wstring source = argc > 1 ? argv[1] : L"data\\ryujinx\\Config.json";
    const std::wstring work = argc > 2 ? argv[2] : L"build\\ryucfg_test";
    wchar_t full[MAX_PATH * 2] = {};
    GetFullPathNameW(work.c_str(), ARRAYSIZE(full), full, nullptr);
    std::wstring lower = full;
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    if (lower.find(L"\\appdata\\") != std::wstring::npos) {
        printf("refusing to work inside AppData: the emulator's own files are not test data\n");
        return 2;
    }
    CreateDirectoryW(work.c_str(), nullptr);
    const std::wstring path = work + L"\\Config.json";
    const std::wstring backup = path + L".dockside-backup";
    const std::wstring tmp = path + L".dockside-tmp";

    std::string original;
    if (!Slurp(source, original) || original.empty()) {
        printf("cannot read %s\n", U8(source).c_str());
        return 2;
    }
    DeleteFileW(backup.c_str());
    DeleteFileW(tmp.c_str());
    if (!Spit(path, original)) {
        printf("cannot write %s\n", U8(path).c_str());
        return 2;
    }

    Check(!ryucfg::Open(work + L"\\no-such-file.json"), "Open fails on a missing file");
    Check(!ryucfg::Has("docked_mode") && ryucfg::Get("docked_mode").empty() && !ryucfg::Set("docked_mode", "true"),
          "nothing works while no file is open");
    if (!ryucfg::Open(path)) {
        printf("cannot open %s\n", U8(path).c_str());
        return 2;
    }
    printf("source  %s (%zu bytes)\ncopy    %s\nversion %d\n\n", U8(source).c_str(), original.size(), U8(path).c_str(),
           ryucfg::Version());

    // ---- 1. list -----------------------------------------------------------
    printf("%-28s %-20s %-7s %-4s %-22s %s\n", "key", "section", "kind", "ok?", "value", "shown as");
    int offered = 0, absent = 0;
    for (const auto& s : ryucfg::Settings()) {
        bool av = ryucfg::Available(s);
        std::string value = ryucfg::Get(s.key);
        printf("%-28s %-20s %-7s %-4s %-22s %s\n", s.key.c_str(), U8(s.section).c_str(), KindName(s.kind),
               !ryucfg::Has(s.key) ? "-" : av ? (s.verified ? "yes" : "yes*") : "no",
               ryucfg::Has(s.key) ? value.c_str() : "(not in this file)",
               ryucfg::Has(s.key) ? (U8(s.label) + ": " + U8(ryucfg::Display(s, value))).c_str() : "");
        av ? offered++ : absent++;
    }
    printf("\n%d settings offered for this file, %d not (key absent, other type or other version); * = unverified values\n\n",
           offered, absent);

    // ---- 2. change, re-read, restore, one setting at a time -----------------
    for (const auto& s : ryucfg::Settings()) {
        if (!ryucfg::Available(s)) {
            Check(!ryucfg::Has(s.key) ? !ryucfg::Set(s.key, "true") : true, s.key + ": Set refuses a key that is not in the file");
            continue;
        }
        const std::string before = ryucfg::Get(s.key);
        // Every other allowed value in turn, not just one.
        std::vector<std::string> values;
        if (s.kind == ryucfg::Kind::Choice) {
            for (const auto& c : s.choices)
                if (c.value != before) values.push_back(c.value);
        } else {
            std::string up = ryucfg::Next(s, before, +1), down = ryucfg::Next(s, before, -1);
            if (up != before) values.push_back(up);
            if (down != before && down != up) values.push_back(down);
            if (s.kind == ryucfg::Kind::Number) {
                values.push_back(ryucfg::Next(s, ryucfg::Next(s, before, +1), +1));
                if (values.back() == before) values.pop_back();
            }
        }
        Check(!values.empty(), s.key + ": has another value to try");
        for (const auto& v : values) {
            bool set = ryucfg::Set(s.key, v);
            Check(set, s.key + " = " + v + ": Set");
            Check(ryucfg::Open(path) && ryucfg::Get(s.key) == v, s.key + " = " + v + ": read back after reopening");
            std::string now;
            Slurp(path, now);
            // Only the value's own text may differ (quotes stay for strings).
            size_t longest = before.size() > v.size() ? before.size() : v.size();
            Check(now != original && ChangedBytes(original, now) <= longest,
                  s.key + " = " + v + ": nothing else in the file changed");
            Check(!Exists(tmp), s.key + ": no temp file left behind");
            for (const auto& other : ryucfg::Settings())
                if (other.key != s.key && ryucfg::Has(other.key) && ryucfg::Get(other.key).empty())
                    Check(false, s.key + " = " + v + ": " + other.key + " lost its value");
        }
        Check(ryucfg::Set(s.key, before), s.key + ": restore " + before);
        std::string now;
        Slurp(path, now);
        Check(now == original, s.key + ": file identical to the original after restoring");
    }

    // ---- 3. what must be refused -------------------------------------------
    std::string now;
    auto unchanged = [&](const char* what) {
        Slurp(path, now);
        Check(now == original, std::string(what) + ": file untouched");
    };
    Check(!ryucfg::Set("graphics_backend", "Vulkn"), "an unknown enum name is refused");
    Check(!ryucfg::Set("system_language", "spanish"), "enum names are case-sensitive");
    Check(!ryucfg::Set("docked_mode", "1"), "a number is refused for a boolean");
    Check(!ryucfg::Set("docked_mode", "True"), "True is refused for a boolean");
    Check(!ryucfg::Set("res_scale", "2.5"), "a fraction is refused for an integer");
    Check(!ryucfg::Set("res_scale", "two"), "text is refused for a number");
    Check(!ryucfg::Set("res_scale", "7"), "a value outside the choices is refused");
    Check(!ryucfg::Set("audio_volume", "1.5"), "a number above max is refused");
    Check(!ryucfg::Set("audio_volume", "-0.1"), "a number below min is refused");
    Check(!ryucfg::Set("audio_volume", ""), "an empty number is refused");
    Check(!ryucfg::Set("no_such_key", "true"), "a key that is not in the file is refused");
    Check(!ryucfg::Set("game_dirs", "x"), "an array is refused");
    Check(!ryucfg::Set("window_startup", "x"), "an object is refused");
    Check(!ryucfg::SetMany({{"docked_mode", "false"}, {"graphics_backend", "Nope"}}), "SetMany is all or nothing");
    Check(ryucfg::Get("docked_mode") == "true", "SetMany: the valid half was not written");
    Check(ryucfg::Set("docked_mode", ryucfg::Get("docked_mode")), "writing the value already there succeeds");
    unchanged("after the refused writes");

    // ---- 4. keys inside objects, several at once, uncurated keys -------------
    Check(ryucfg::Get("window_startup.window_size_width") == "2048" || ryucfg::Has("window_startup.window_size_width"),
          "a key inside an object is found");
    Check(!ryucfg::Has("input_config") || ryucfg::Get("input_config").empty(), "an array has no single value");
    if (ryucfg::Has("window_startup.window_position_x") && ryucfg::Has("show_console")) {
        std::string x = ryucfg::Get("window_startup.window_position_x"), y = ryucfg::Get("window_startup.window_position_y"),
                    con = ryucfg::Get("show_console");
        Check(ryucfg::SetMany({{"show_console", con == "true" ? "false" : "true"},
                               {"window_startup.window_position_x", "1234"},
                               {"window_startup.window_position_y", "-56"}}),
              "SetMany: three keys, one of them first in the file, two nested");
        Check(ryucfg::Open(path) && ryucfg::Get("window_startup.window_position_x") == "1234" &&
                  ryucfg::Get("window_startup.window_position_y") == "-56" && ryucfg::Get("show_console") != con,
              "SetMany: all three read back");
        Check(ryucfg::SetMany({{"window_startup.window_position_y", y}, {"show_console", con}, {"window_startup.window_position_x", x}}),
              "SetMany: restore");
        unchanged("after SetMany and back");
    }
    if (ryucfg::Has("hotkeys.screenshot")) {
        std::string was = ryucfg::Get("hotkeys.screenshot");
        const std::string odd = "a\"b\\c\td";
        Check(ryucfg::Set("hotkeys.screenshot", odd) && ryucfg::Open(path) && ryucfg::Get("hotkeys.screenshot") == odd,
              "a string with quote, backslash and tab survives the round trip");
        Check(ryucfg::Set("hotkeys.screenshot", was), "restore it");
        unchanged("after the odd string");
    }

    // ---- 5. a value this module does not know, as a newer fork might write ---
    {
        std::string forked = original;
        size_t at = forked.find("\"Bilinear\"");
        if (at != std::string::npos) {
            forked.replace(at, 10, "\"Area\"");
            Spit(path, forked);
            const ryucfg::Setting* sf = ryucfg::Find("scaling_filter");
            Check(ryucfg::Open(path) && ryucfg::Get("scaling_filter") == "Area", "an unknown enum value is read as it is");
            Check(sf && ryucfg::Display(*sf, "Area") == L"Area", "and shown as it is");
            Check(ryucfg::Set("scaling_filter", "Fsr") && ryucfg::Set("scaling_filter", "Area"),
                  "and can be put back after changing it, because the file held it");
            Slurp(path, now);
            Check(now == forked, "file identical to the forked one");
            Check(ryucfg::Set("scaling_filter", "Bilinear"), "back to Bilinear");
            unchanged("after the unknown-value test");
        }
    }

    // ---- 6. the emulator rewrote the file between Open and Set ---------------
    {
        ryucfg::Open(path);
        std::string external = original;
        size_t at = external.find("\"audio_volume\": 1,");
        size_t first = external.find("\"enable_file_log\": true");
        if (at != std::string::npos && first != std::string::npos) {
            external.replace(at, 18, "\"audio_volume\": 0.35,");
            external.replace(first, 23, "\"enable_file_log\": false"); // shifts everything after it
            Spit(path, external);
            Check(ryucfg::Set("docked_mode", "false"), "Set after an outside change");
            Slurp(path, now);
            Check(now.find("\"audio_volume\": 0.35,") != std::string::npos &&
                      now.find("\"enable_file_log\": false") != std::string::npos &&
                      now.find("\"docked_mode\": false") != std::string::npos,
                  "the outside changes are kept and the new value is in the right place");
            Check(ryucfg::Get("audio_volume") == "0.35", "Get sees the outside change after Set");
            Spit(path, original);
            Check(ryucfg::Open(path), "reopen the original");
        }
    }

    // ---- 7. damaged files are refused, not patched ----------------------------
    {
        const std::wstring bad = work + L"\\Broken.json";
        const char* cases[] = {"", "[1,2]", "{\"a\": tru}", "{\"a\": 1,}", "{\"a\": 1} x", "{\"a\": \"unterminated}",
                               "{\"a\": 1 \"b\": 2}", "{\"a\": 1"};
        for (const char* c : cases) {
            Spit(bad, c);
            Check(!ryucfg::Open(bad), std::string("refused: ") + (*c ? c : "(empty file)"));
        }
        Spit(bad, "\xEF\xBB\xBF{\"a\":true,\"b\":{\"c\":[1,{\"d\":2}],\"e\":-1.5e3},\"f\":\"x\"}");
        Check(ryucfg::Open(bad) && ryucfg::Get("a") == "true" && ryucfg::Get("b.e") == "-1.5e3" && ryucfg::Get("f") == "x" &&
                  !ryucfg::Has("b.c.d") && !ryucfg::Has("d"),
              "compact JSON with a byte-order mark is read; values inside arrays are not addressable");
        Check(ryucfg::Set("a", "false") && ryucfg::Set("f", "y"), "and written");
        Slurp(bad, now);
        Check(now == "\xEF\xBB\xBF{\"a\":false,\"b\":{\"c\":[1,{\"d\":2}],\"e\":-1.5e3},\"f\":\"y\"}", "with nothing else touched");
        DeleteFileW(bad.c_str());
        DeleteFileW((bad + L".dockside-backup").c_str());
    }

    // ---- 8. the backup --------------------------------------------------------
    {
        std::string kept;
        Check(Slurp(backup, kept) && kept == original, "the backup exists and is the file as it was before the first write");
        Spit(backup, "sentinel");
        ryucfg::Open(path);
        Check(ryucfg::Set("docked_mode", "false") && ryucfg::Set("docked_mode", "true"), "two more writes");
        Slurp(backup, kept);
        Check(kept == "sentinel", "an existing backup is never overwritten");
    }

    // ---- 9. Next / Display ----------------------------------------------------
    {
        const ryucfg::Setting* vol = ryucfg::Find("audio_volume");
        const ryucfg::Setting* off = ryucfg::Find("system_time_offset");
        const ryucfg::Setting* lang = ryucfg::Find("system_language");
        Check(vol && ryucfg::Next(*vol, "1", +1) == "1" && ryucfg::Next(*vol, "1", -1) == "0.95" &&
                  ryucfg::Next(*vol, "0", -1) == "0" && ryucfg::Next(*vol, "0.3", +1) == "0.35",
              "Next: numbers step and stop at the ends");
        Check(off && ryucfg::Next(*off, "-78", +1) == "0" && ryucfg::Next(*off, "-78", -1) == "-3600",
              "Next: a value between marks moves to the nearest mark in that direction");
        Check(lang && ryucfg::Next(*lang, lang->choices.back().value, +1) == lang->choices.front().value &&
                  ryucfg::Next(*lang, lang->choices.front().value, -1) == lang->choices.back().value,
              "Next: choices wrap around");
        Check(vol && ryucfg::Display(*vol, "0.35") == L"35 %" && ryucfg::Display(*vol, "1") == L"100 %", "Display: volume");
        Check(off && ryucfg::Display(*off, "-78") == L"-0,02 h" && ryucfg::Display(*off, "7200") == L"2 h", "Display: clock");
    }

    // ---- 10. the end state ------------------------------------------------------
    Slurp(path, now);
    bool identical = now == original;
    Check(identical, "FINAL: the copy is byte-identical to the source");
    Check(!Exists(tmp), "FINAL: no temp file left");
    std::string src2;
    Check(Slurp(source, src2) && src2 == original, "FINAL: the source file was never modified");

    printf("\n%d checks passed, %d failed\n", passed, failed);
    printf("final file vs original: %s\n", identical ? "byte-identical" : "DIFFERENT");
    printf("%s\n", failed == 0 ? "RESULT: PASS" : "RESULT: FAIL");
    return failed == 0 ? 0 : 1;
}
