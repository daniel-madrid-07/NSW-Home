// See pchost.h. Display facts, brightness and Windows' settings pages are
// Windows calls; the file work and the hash only use what the compatibility
// layer of the other builds also provides.
#include "pchost.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <winioctl.h>
#else
#include <unistd.h>
#include "port.h"
#endif

namespace pchost {

namespace {
bool offscreen = false;
int standIn = -1; // brightness "written" during an offscreen run
} // namespace

void SetOffscreen(bool on) {
    offscreen = on;
}
bool Offscreen() {
    return offscreen;
}

// ---- display ------------------------------------------------------------------

#ifdef _WIN32
namespace {

HMONITOR OurMonitor() {
    // (The window class is main.cpp's; an offscreen run has no window.)
    if (HWND w = FindWindowW(L"NSWHomeWindow", nullptr)) return MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY);
    const POINT origin = {0, 0};
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

// The laptop panel's own brightness device (ntddvdeo.h; the values are part
// of the documented interface and spelt out here so that the driver kit's
// header is not needed).
constexpr DWORD kLcdQuery = CTL_CODE(FILE_DEVICE_VIDEO, 0x126, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kLcdSet = CTL_CODE(FILE_DEVICE_VIDEO, 0x127, METHOD_BUFFERED, FILE_ANY_ACCESS);
struct LcdBrightness {
    UCHAR policy, ac, dc; // policy 3 = on mains and on battery alike
};

bool LcdGet(int& percent) {
    HANDLE h = CreateFileW(L"\\\\.\\LCD", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LcdBrightness b = {};
    DWORD got = 0;
    const bool ok = DeviceIoControl(h, kLcdQuery, nullptr, 0, &b, sizeof b, &got, nullptr) && got >= sizeof b;
    CloseHandle(h);
    if (!ok) return false;
    SYSTEM_POWER_STATUS ps = {};
    percent = GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0 ? b.dc : b.ac;
    return percent >= 0 && percent <= 100;
}

bool LcdSet(int percent) {
    HANDLE h = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LcdBrightness b = {3, static_cast<UCHAR>(percent), static_cast<UCHAR>(percent)};
    DWORD got = 0;
    const bool ok = DeviceIoControl(h, kLcdSet, &b, sizeof b, nullptr, 0, &got, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

// The monitor's own control channel (DDC/CI). A monitor is asked through the
// first physical monitor behind the screen: mirrored screens are not told apart.
template <typename Fn>
bool WithPhysical(Fn fn) {
    DWORD n = 0;
    HMONITOR m = OurMonitor();
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(m, &n) || n == 0) return false;
    std::vector<PHYSICAL_MONITOR> mons(n);
    if (!GetPhysicalMonitorsFromHMONITOR(m, n, mons.data())) return false;
    const bool ok = fn(mons[0].hPhysicalMonitor);
    DestroyPhysicalMonitors(n, mons.data());
    return ok;
}

// The monitor's own scale, as its last answer gave it: a write needs it, and
// asking again would double the time a step of the slider takes (each
// exchange with the monitor is some 60 ms).
HMONITOR rangeOf = nullptr;
DWORD rangeLo = 0, rangeHi = 0;

bool DdcGet(int& percent) {
    return WithPhysical([&](HANDLE h) {
        DWORD lo = 0, cur = 0, hi = 0;
        if (!GetMonitorBrightness(h, &lo, &cur, &hi) || hi <= lo) return false;
        percent = static_cast<int>((cur - lo) * 100.0 / (hi - lo) + 0.5);
        rangeOf = OurMonitor();
        rangeLo = lo;
        rangeHi = hi;
        return true;
    });
}

} // namespace

Display DisplayNow() {
    Display d;
    HMONITOR m = OurMonitor();
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(m, &mi)) return d;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return d;
    d.ok = true;
    d.width = static_cast<int>(dm.dmPelsWidth);
    d.height = static_cast<int>(dm.dmPelsHeight);
    d.hz = static_cast<int>(dm.dmDisplayFrequency);
    UINT dx = 0, dy = 0;
    if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx > 0)
        d.scale = static_cast<int>(dx * 100.0 / 96.0 + 0.5);

    // HDR and the kind of connection are only told by the display
    // configuration interface, path by path.
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) return d;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return d;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice)) continue;
        const auto tech = paths[i].targetInfo.outputTechnology;
        d.builtIn = tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL || tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS ||
                    tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                    tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO col = {};
        col.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        col.header.size = sizeof col;
        col.header.adapterId = paths[i].targetInfo.adapterId;
        col.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&col.header) == ERROR_SUCCESS) {
            d.hdrCapable = col.advancedColorSupported != 0;
            d.hdr = col.advancedColorEnabled ? 1 : 0;
            d.bits = static_cast<int>(col.bitsPerColorChannel);
        }
        break;
    }
    return d;
}

std::wstring AdapterName() {
    DISPLAY_DEVICEW dev = {};
    dev.cb = sizeof dev;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); i++)
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return dev.DeviceString;
    return {};
}

bool BrightnessGet(int& percent) {
    if (offscreen && standIn >= 0) {
        percent = standIn;
        return true;
    }
    if (DdcGet(percent)) return true;
    return DisplayNow().builtIn && LcdGet(percent);
}

bool BrightnessSet(int percent) {
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (offscreen) {
        standIn = percent;
        return true;
    }
    int was = 0;
    if ((rangeOf && rangeOf == OurMonitor()) || DdcGet(was)) {
        const DWORD raw = rangeLo + static_cast<DWORD>((rangeHi - rangeLo) * percent / 100.0 + 0.5);
        return WithPhysical([&](HANDLE h) { return SetMonitorBrightness(h, raw) != 0; });
    }
    return DisplayNow().builtIn && LcdSet(percent);
}

bool OpenDisplaySettings(bool hdr) {
    if (offscreen) return false;
    // Windows 11 has a page of its own for HDR; on Windows 10 the switch
    // ("Windows HD Color") is on the display page itself.
    // (Asked of the kernel library: the documented version calls answer
    // "Windows 8" to a program without a compatibility manifest.)
    bool eleven = false;
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) {
            OSVERSIONINFOW v = {};
            v.dwOSVersionInfoSize = sizeof v;
            eleven = fn(&v) == 0 && v.dwBuildNumber >= 22000;
        }
    const wchar_t* page = hdr && eleven ? L"ms-settings:display-hdr" : L"ms-settings:display";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", page, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

#else // ---- every other system: not available ------------------------------------

Display DisplayNow() {
    return {};
}
std::wstring AdapterName() {
    return {};
}
bool BrightnessGet(int& percent) {
    if (offscreen && standIn >= 0) {
        percent = standIn;
        return true;
    }
    return false;
}
bool BrightnessSet(int percent) {
    if (!offscreen) return false;
    standIn = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    return true;
}
bool OpenDisplaySettings(bool) {
    return false;
}

#endif

// ---- files ----------------------------------------------------------------------

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ReadText(const std::wstring& path, std::string& out) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    fclose(f);
    return true;
}

bool WriteText(const std::wstring& path, const std::string& text) {
    // Through a temporary file, so that a crash never leaves half a file.
    const std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Copy(const std::wstring& from, const std::wstring& to) {
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

bool Remove(const std::wstring& file) {
    return DeleteFileW(file.c_str()) != 0;
}

bool MakeDir(const std::wstring& dir) {
    return CreateDirectoryW(dir.c_str(), nullptr) || Exists(dir);
}

std::vector<Entry> List(const std::wstring& dir) {
    std::vector<Entry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        Entry e;
        e.name = fd.cFileName;
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

bool CopyTree(const std::wstring& from, const std::wstring& to) {
    if (!MakeDir(to)) return false;
    bool ok = true;
    for (const Entry& e : List(from)) {
        const std::wstring a = from + L"\\" + e.name, b = to + L"\\" + e.name;
        ok = (e.dir ? CopyTree(a, b) : Copy(a, b)) && ok;
    }
    return ok;
}

bool RemoveTree(const std::wstring& dir) {
    if (!Exists(dir)) return true;
    bool ok = true;
    for (const Entry& e : List(dir)) {
        const std::wstring p = dir + L"\\" + e.name;
        ok = (e.dir ? RemoveTree(p) : Remove(p)) && ok;
    }
#ifdef _WIN32
    return RemoveDirectoryW(dir.c_str()) && ok;
#else
    return rmdir(port::NativePath(dir).c_str()) == 0 && ok;
#endif
}

// ---- hash -----------------------------------------------------------------------

// SHA-256 (FIPS 180-4), written out because the other builds' stand-in for
// the system's cryptography only has AES.
std::string Sha256Hex(const std::string& data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 7; i >= 0; i--) msg += static_cast<char>((bits >> (i * 8)) & 0xFF);
    auto rot = [](uint32_t v, int n) { return (v >> n) | (v << (32 - n)); };
    for (size_t at = 0; at < msg.size(); at += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data()) + at + i * 4;
            w[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        memcpy(v, h, sizeof v);
        for (int i = 0; i < 64; i++) {
            const uint32_t s1 = rot(v[4], 6) ^ rot(v[4], 11) ^ rot(v[4], 25);
            const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rot(v[0], 2) ^ rot(v[0], 13) ^ rot(v[0], 22);
            const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const uint32_t t2 = s0 + maj;
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + t1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) h[i] += v[i];
    }
    char hex[65];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08x", h[i]);
    return hex;
}

std::string RandomHex(int bytes) {
    std::vector<unsigned char> raw(static_cast<size_t>(bytes > 0 ? bytes : 0));
    if (!raw.empty()) BCryptGenRandom(nullptr, raw.data(), static_cast<ULONG>(raw.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string out;
    char b[3];
    for (unsigned char c : raw) {
        snprintf(b, sizeof b, "%02x", c);
        out += b;
    }
    return out;
}

} // namespace pchost
