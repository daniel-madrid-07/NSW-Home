// Test CLI for src/fwsound.cpp.
// Usage: dump_fwsounds <outDir> [--registered <dir>] [--keys <dir>]
// Decodes every HOME Menu sound effect from the installed firmware and writes <outDir>\<name>.wav.
// Prints one line per sound: name, sample rate, channels, duration, peak and RMS level.
// Defaults: %APPDATA%\Ryujinx\bis\system\Contents\registered and %APPDATA%\Ryujinx\system.
// <outDir> must be outside the project: the WAVs are firmware assets.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/fwsound.h"

static std::wstring envVar(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

static uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

struct Stats {
    bool valid = false;
    uint32_t rate = 0;
    unsigned channels = 0;
    double seconds = 0, peakDb = -200, rmsDb = -200;
    double clipped = 0;  // share of samples at full scale
};

// Checks the RIFF header the module produced and measures the samples.
static Stats measure(const std::vector<uint8_t>& w) {
    Stats s;
    if (w.size() < 44 || std::memcmp(&w[0], "RIFF", 4) != 0 || std::memcmp(&w[8], "WAVEfmt ", 8) != 0 ||
        std::memcmp(&w[36], "data", 4) != 0)
        return s;
    if (le32(&w[4]) != w.size() - 8 || le32(&w[16]) != 16 || le16(&w[20]) != 1 || le16(&w[34]) != 16) return s;
    s.channels = le16(&w[22]);
    s.rate = le32(&w[24]);
    uint32_t dataSize = le32(&w[40]);
    if (s.channels < 1 || s.channels > 2 || s.rate == 0 || dataSize != w.size() - 44 || dataSize % (s.channels * 2) != 0)
        return s;
    if (le16(&w[32]) != s.channels * 2 || le32(&w[28]) != s.rate * s.channels * 2) return s;

    size_t count = dataSize / 2;
    double sum = 0;
    int peak = 0;
    size_t clipped = 0;
    for (size_t i = 0; i < count; ++i) {
        int v = static_cast<int16_t>(le16(&w[44 + i * 2]));
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        if (a >= 32767) ++clipped;
        sum += static_cast<double>(v) * v;
    }
    s.seconds = static_cast<double>(count / s.channels) / s.rate;
    if (peak > 0) s.peakDb = 20.0 * std::log10(peak / 32768.0);
    if (count > 0 && sum > 0) s.rmsDb = 10.0 * std::log10(sum / static_cast<double>(count) / (32768.0 * 32768.0));
    s.clipped = count ? static_cast<double>(clipped) / static_cast<double>(count) : 0;
    s.valid = true;
    return s;
}

static bool writeFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

// Archive names are plain identifiers; anything else becomes '_' before it is used as a file name.
static std::wstring safeName(const std::string& name) {
    std::wstring w;
    for (char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        w += ok ? static_cast<wchar_t>(c) : L'_';
    }
    return w.empty() ? std::wstring(L"_") : w.substr(0, 80);
}

static double now() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

int wmain(int argc, wchar_t** argv) {
    std::wstring outDir;
    std::wstring appData = envVar(L"APPDATA");
    std::wstring registered = appData + L"\\Ryujinx\\bis\\system\\Contents\\registered";
    std::wstring keys = appData + L"\\Ryujinx\\system";
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--registered" && i + 1 < argc) registered = argv[++i];
        else if (a == L"--keys" && i + 1 < argc) keys = argv[++i];
        else if (outDir.empty()) outDir = a;
    }
    if (outDir.empty()) {
        std::fprintf(stderr, "usage: dump_fwsounds <outDir> [--registered <dir>] [--keys <dir>]\n");
        return 2;
    }
    CreateDirectoryW(outDir.c_str(), nullptr);

    std::string err, version;
    double t0 = now();
    bool haveVersion = fwsound::FirmwareVersion(registered, keys, version, err);
    double t1 = now();
    std::printf("firmware: %s (%.1f ms)\n", haveVersion ? version.c_str() : err.c_str(), (t1 - t0) * 1000.0);

    std::vector<fwsound::Sound> sounds;
    t0 = now();
    bool ok = fwsound::LoadMenuSounds(registered, keys, sounds, err);
    t1 = now();
    if (!ok) {
        std::fprintf(stderr, "LoadMenuSounds failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("sounds: %zu, load time %.1f ms\n", sounds.size(), (t1 - t0) * 1000.0);
    std::printf("%-34s %6s %2s %8s %8s %8s  %s\n", "name", "rate", "ch", "seconds", "peak dB", "rms dB", "notes");

    int bad = 0;
    size_t bytes = 0;
    for (const fwsound::Sound& s : sounds) {
        Stats st = measure(s.wav);
        std::string notes;
        if (!st.valid) notes += " BAD_WAV";
        if (st.valid && st.peakDb < -60) notes += " SILENT";
        if (st.valid && st.clipped > 0.01) notes += " CLIPPING";
        if (st.valid && st.seconds > 1.5) notes += " long";
        if (st.valid && st.seconds < 0.03) notes += " short";
        if (!st.valid || st.peakDb < -60 || st.clipped > 0.01) ++bad;
        std::printf("%-34s %6u %2u %8.3f %8.1f %8.1f %s\n", s.name.c_str(), static_cast<unsigned>(st.rate), st.channels,
                    st.seconds, st.peakDb, st.rmsDb, notes.c_str());
        if (!writeFile(outDir + L"\\" + safeName(s.name) + L".wav", s.wav)) {
            std::fprintf(stderr, "cannot write %s.wav\n", s.name.c_str());
            ++bad;
        }
        bytes += s.wav.size();
    }
    std::printf("total %zu bytes of WAV, %d problem(s)\n", bytes, bad);
    return bad ? 1 : 0;
}
