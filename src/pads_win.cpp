// Windows only: finds the pads pads.h knows and hands their reports over.
//
// Raw Input on a thread of its own. Windows delivers every HID report of the
// registered kinds of device to a hidden window, whichever program has the
// focus (needed for HOME while a game is in front), and announces devices as
// they come and go, so nothing is polled and nothing can stall the menu. The
// thread only parses; the menu reads the result once a frame (pads::Poll).
//
// Why not Windows.Gaming.Input or GameInput: the first hides HOME and capture
// and gives no report-level battery or identity (and needs WinRT activation
// from a plain Win32 program); the second is not part of Windows 10 19045
// without its redistributable.
//
// Devices are opened without read or write access, only to ask Windows for
// the serial number (Bluetooth: the pad's address, kept by Windows itself)
// or, on USB, for a Sony pad's address report. Nothing is written to a pad,
// with one exception that is from the documentation and has never met a real
// pad here: a Switch Pro Controller on USB sends no reports at all until it is
// told to (the two commands below). That is only done while the user has the
// console's "Conexión por cable del mando Pro" switched on (pads::ProWired);
// otherwise such a pad is left alone and not listed: it is read over Bluetooth.
#include "pads.h"
#include <windows.h>
#include <algorithm>
#include <mutex>
#include <thread>
extern "C" {
#include <hidsdi.h>
}

namespace pads {

namespace {

std::thread worker;
HWND window = nullptr;

std::wstring DeviceName(HANDLE dev) {
    UINT len = 0;
    GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, nullptr, &len);
    std::wstring name(len, L'\0');
    if (!len || GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, name.data(), &len) == static_cast<UINT>(-1)) return {};
    name.resize(wcslen(name.c_str()));
    return name;
}

bool Contains(std::wstring hay, const wchar_t* needle) {
    std::transform(hay.begin(), hay.end(), hay.begin(), towupper);
    return hay.find(needle) != std::wstring::npos;
}

std::string HexOnly(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s)
        if (iswxdigit(c)) out += static_cast<char>(towlower(c));
    return out;
}

// The pad's Bluetooth address when it can be had (the same over USB and
// Bluetooth for a Sony pad), else the place it is plugged into.
std::string Identity(const std::wstring& name, Kind kind, Link link) {
    std::string id;
    HANDLE h = CreateFileW(name.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        if (link == Link::Bluetooth) {
            wchar_t serial[128] = {};
            if (HidD_GetSerialNumberString(h, serial, sizeof serial)) id = HexOnly(serial);
        } else if (kind == Kind::DualSense || kind == Kind::DualShock4) {
            // Feature report 0x09 (DualSense) / 0x12 (DualShock 4): the pad's
            // address in bytes 1 to 6, last byte first.
            uint8_t buf[64] = {};
            buf[0] = kind == Kind::DualSense ? 0x09 : 0x12;
            if (HidD_GetFeature(h, buf, kind == Kind::DualSense ? 20 : 16)) {
                char hex[16];
                snprintf(hex, sizeof hex, "%02x%02x%02x%02x%02x%02x", buf[6], buf[5], buf[4], buf[3], buf[2], buf[1]);
                id = hex;
            }
        }
        CloseHandle(h);
    }
    if (id.size() == 12 && id != "000000000000") return "mac:" + id;
    // The device path names the port (USB) or the paired pad (Bluetooth).
    uint32_t hash = 2166136261u;
    for (wchar_t c : name) hash = (hash ^ static_cast<uint32_t>(towupper(c))) * 16777619u;
    char text[32];
    snprintf(text, sizeof text, "port:%08x", hash);
    return text;
}

// A Switch Pro Controller on USB stays silent until asked: "handshake" (80 02)
// and "talk HID over USB" (80 04), each a 64-byte output report. Given up on
// after 200 ms so that a pad that does not answer cannot hold the thread.
void WakeSwitchUsb(const std::wstring& name) {
    HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    for (uint8_t command : {uint8_t(0x02), uint8_t(0x04)}) {
        uint8_t report[64] = {0x80, command};
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD put = 0;
        if (!WriteFile(h, report, sizeof report, &put, &ov) && GetLastError() == ERROR_IO_PENDING &&
            WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0)
            CancelIo(h);
        CloseHandle(ov.hEvent);
    }
    CloseHandle(h);
}

bool Describe(HANDLE dev, DeviceInfo& info, std::wstring& name) {
    RID_DEVICE_INFO di = {};
    di.cbSize = sizeof di;
    UINT size = sizeof di;
    if (GetRawInputDeviceInfoW(dev, RIDI_DEVICEINFO, &di, &size) == static_cast<UINT>(-1) || di.dwType != RIM_TYPEHID) return false;
    if (di.hid.usUsagePage != 1 || (di.hid.usUsage != 4 && di.hid.usUsage != 5)) return false;
    info.vendor = static_cast<uint16_t>(di.hid.dwVendorId);
    info.product = static_cast<uint16_t>(di.hid.dwProductId);
    // The number SDL puts in its identifier (hidapi reads the same attribute).
    info.version = static_cast<uint16_t>(di.hid.dwVersionNumber);
    name = DeviceName(dev);
    // Windows lists Bluetooth pads under its Bluetooth enumerator, whose
    // device paths carry the HID service's number.
    info.link = Contains(name, L"BTHENUM") || Contains(name, L"00001124-0000-1000-8000-00805F9B34FB") ||
                Contains(name, L"BTHLE") ? Link::Bluetooth : Link::Usb;
    return true;
}

// Pro Controllers on a cable, told to talk or not (the device thread adds and
// removes them, the menu's thread flips the switch).
struct Cabled {
    uint64_t handle = 0;
    std::wstring name;
    DeviceInfo info;
    bool talking = false;
};
std::mutex cabledLock;
std::vector<Cabled> cabled;

// Caller holds cabledLock.
void Follow(Cabled& c) {
    if (ProWired() && !c.talking) {
        WakeSwitchUsb(c.name);
        Arrive(c.handle, c.info);
        c.talking = true;
    } else if (!ProWired() && c.talking) {
        // (The pad itself goes on talking over the cable until it is unplugged;
        // there is no need to write to it again to stop listening.)
        Remove(c.handle);
        c.talking = false;
    }
}

void Add(HANDLE dev) {
    DeviceInfo info;
    std::wstring name;
    if (!Describe(dev, info, name) || !Known(info.vendor, info.product, info.kind)) return;
    info.id = Identity(name, info.kind, info.link);
    if (info.kind == Kind::SwitchPro && info.link == Link::Usb) {
        std::lock_guard<std::mutex> lock(cabledLock);
        for (const auto& c : cabled)
            if (c.handle == reinterpret_cast<uint64_t>(dev)) return;
        cabled.push_back({reinterpret_cast<uint64_t>(dev), name, info, false});
        Follow(cabled.back());
        return;
    }
    Arrive(reinterpret_cast<uint64_t>(dev), info);
}

void Gone(uint64_t handle) {
    {
        std::lock_guard<std::mutex> lock(cabledLock);
        cabled.erase(std::remove_if(cabled.begin(), cabled.end(), [&](const Cabled& c) { return c.handle == handle; }), cabled.end());
    }
    Remove(handle);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        // Reports are at most a few hundred bytes; the header says how many came together.
        alignas(8) static uint8_t buf[4096];
        UINT size = sizeof buf;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
            const RAWINPUT* in = reinterpret_cast<const RAWINPUT*>(buf);
            if (in->header.dwType == RIM_TYPEHID)
                for (DWORD i = 0; i < in->data.hid.dwCount; i++)
                    Feed(reinterpret_cast<uint64_t>(in->header.hDevice), in->data.hid.bRawData + i * in->data.hid.dwSizeHid,
                         in->data.hid.dwSizeHid);
        }
    } else if (msg == WM_INPUT_DEVICE_CHANGE) {
        if (wp == GIDC_ARRIVAL) Add(reinterpret_cast<HANDLE>(lp));
        else if (wp == GIDC_REMOVAL) Gone(static_cast<uint64_t>(lp));
        return 0;
    } else if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Run() {
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NSWHomePads";
    RegisterClassExW(&wc);
    window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!window) return;
    // Joysticks and game pads (generic desktop page, usages 4 and 5), also
    // while another program has the focus, with arrival and removal notices
    // (which are also sent for the devices that are there already).
    RAWINPUTDEVICE want[2] = {{1, 4, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window}, {1, 5, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window}};
    RegisterRawInputDevices(want, 2, sizeof want[0]);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    window = nullptr;
}

} // namespace

void WiredChanged() {
    std::lock_guard<std::mutex> lock(cabledLock);
    for (auto& c : cabled) Follow(c);
}

bool Reads() {
    return true;
}

void Start() {
    if (worker.joinable() || DebugFaking()) return;
    worker = std::thread(Run);
}

void Stop() {
    if (!worker.joinable()) return;
    // The window may not exist yet (or any more); the thread's queue always does.
    for (int i = 0; i < 100 && !window; i++) Sleep(10);
    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
    worker.join();
}

std::vector<std::string> Probe() {
    std::vector<std::string> out;
    UINT count = 0;
    GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (!count || GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) return out;
    for (UINT i = 0; i < count; i++) {
        DeviceInfo info;
        std::wstring name;
        if (list[i].dwType != RIM_TYPEHID || !Describe(list[i].hDevice, info, name)) continue;
        const bool known = Known(info.vendor, info.product, info.kind);
        static const char* const kinds[] = {"Xbox", "DualSense", "DualShock 4", "Switch Pro", "Joy-Con (L)", "Joy-Con (R)", "", ""};
        char line[512];
        snprintf(line, sizeof line, "%04x:%04x version %04x %s  %s", info.vendor, info.product, info.version,
                 info.link == Link::Bluetooth ? "Bluetooth" : "USB",
                 known ? kinds[static_cast<int>(info.kind)] : Contains(name, L"IG_") ? "(XInput device, read through XInput)" : "(ignored)");
        std::string text = line;
        if (known) {
            uint8_t guid[16];
            SdlGuid(info.vendor, info.product, info.version, SdlName(info.kind, info.product), guid);
            text += "  id " + Identity(name, info.kind, info.link) + "  ryujinx " + RyujinxId(guid, 0) + "  yuzu " + YuzuGuid(guid);
        }
        out.push_back(text);
    }
    return out;
}

} // namespace pads
