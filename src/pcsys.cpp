// See pcsys.h.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "pcsys.h"
#include <cmath>
#include <cstring>

namespace pcsys {

const wchar_t* const kProbeHost = L"www.msftconnecttest.com";
const wchar_t* const kProbePath = L"/connecttest.txt";

// ---- test tone (every system) ------------------------------------------------

std::vector<uint8_t> ToneWav() {
    const int rate = 48000;
    const int frames = static_cast<int>(kToneLength * rate);
    std::vector<uint8_t> wav(44 + static_cast<size_t>(frames) * 4);
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) wav[at + i] = static_cast<uint8_t>(v >> (i * 8));
    };
    auto put16 = [&](size_t at, uint32_t v) {
        wav[at] = static_cast<uint8_t>(v);
        wav[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    memcpy(&wav[0], "RIFF", 4);
    put32(4, static_cast<uint32_t>(wav.size() - 8));
    memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, 2);
    put32(24, rate);
    put32(28, rate * 4);
    put16(32, 4);
    put16(34, 16);
    memcpy(&wav[36], "data", 4);
    put32(40, static_cast<uint32_t>(frames) * 4);
    // 660 Hz at a quarter of full scale, with 30 ms ramps so that it neither
    // clicks nor startles.
    auto level = [&](double t, double from, double to) {
        if (t < from || t >= to) return 0.0;
        const double edge = 0.03, in = (t - from) / edge, out = (to - t) / edge;
        const double env = in < 1 ? in : out < 1 ? out : 1.0;
        return 0.25 * env * std::sin(2 * 3.14159265358979323846 * 660.0 * (t - from));
    };
    for (int i = 0; i < frames; i++) {
        const double t = static_cast<double>(i) / rate;
        const int l = static_cast<int>(level(t, kToneLeftFrom, kToneLeftTo) * 32767.0);
        const int r = static_cast<int>(level(t, kToneRightFrom, kToneRightTo) * 32767.0);
        put16(44 + static_cast<size_t>(i) * 4, static_cast<uint32_t>(l & 0xFFFF));
        put16(46 + static_cast<size_t>(i) * 4, static_cast<uint32_t>(r & 0xFFFF));
    }
    return wav;
}

} // namespace pcsys

#ifdef _WIN32
// ============================================================================
// Windows
// ============================================================================
#include "app.h" // cfg (the two sound switches) and DataDir
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <iphlpapi.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wlanapi.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace pcsys {

bool Available() {
    return true;
}

namespace {

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
    explicit operator bool() const { return p != nullptr; }
};

// The sound interfaces work from any thread once COM is up on it. The menu's
// thread already has it; a thread of ours, or a test program, may not.
void NeedCom() {
    thread_local bool done = false;
    if (done) return;
    done = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // "already set up differently" is fine too
}

// ---- network ---------------------------------------------------------------

std::wstring Ip(const SOCKADDR* a) {
    wchar_t buf[80] = {};
    if (a && a->sa_family == AF_INET)
        InetNtopW(AF_INET, &reinterpret_cast<const sockaddr_in*>(a)->sin_addr, buf, ARRAYSIZE(buf));
    else if (a && a->sa_family == AF_INET6)
        InetNtopW(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(a)->sin6_addr, buf, ARRAYSIZE(buf));
    return buf;
}

bool LinkLocal6(const SOCKADDR* a) {
    const unsigned char* b = reinterpret_cast<const sockaddr_in6*>(a)->sin6_addr.s6_addr;
    return b[0] == 0xFE && (b[1] & 0xC0) == 0x80;
}

const wchar_t* SecurityName(DOT11_AUTH_ALGORITHM a) {
    switch (static_cast<int>(a)) {
    case 1: return L"Abierta";          // 802.11 open
    case 2: return L"WEP";              // shared key
    case 3: return L"WPA-Enterprise";
    case 4: return L"WPA-Personal";
    case 6: return L"WPA2-Enterprise";
    case 7: return L"WPA2-Personal";
    case 8: return L"WPA3-Enterprise";  // also 192-bit (10), 11 below
    case 9: return L"WPA3-Personal";
    case 10: return L"OWE";
    case 11: return L"WPA3-Enterprise";
    default: return L"";
    }
}

// wlanapi.dll is absent from Windows editions without the wireless feature,
// so it is looked up when needed instead of being required to start.
void ReadWifi(const char* adapterGuid, Net& net) {
    HMODULE lib = LoadLibraryW(L"wlanapi.dll");
    if (!lib) return;
    auto open = reinterpret_cast<DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE)>(
        reinterpret_cast<void*>(GetProcAddress(lib, "WlanOpenHandle")));
    auto close = reinterpret_cast<DWORD(WINAPI*)(HANDLE, PVOID)>(reinterpret_cast<void*>(GetProcAddress(lib, "WlanCloseHandle")));
    auto query = reinterpret_cast<DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*,
                                                 PWLAN_OPCODE_VALUE_TYPE)>(
        reinterpret_cast<void*>(GetProcAddress(lib, "WlanQueryInterface")));
    auto release = reinterpret_cast<VOID(WINAPI*)(PVOID)>(reinterpret_cast<void*>(GetProcAddress(lib, "WlanFreeMemory")));
    GUID guid = {};
    wchar_t wide[64] = {};
    MultiByteToWideChar(CP_ACP, 0, adapterGuid, -1, wide, ARRAYSIZE(wide));
    HANDLE h = nullptr;
    DWORD version = 0;
    if (open && close && query && release && SUCCEEDED(CLSIDFromString(wide, &guid)) &&
        open(2, nullptr, &version, &h) == ERROR_SUCCESS) {
        DWORD size = 0;
        PVOID data = nullptr;
        if (query(h, &guid, wlan_intf_opcode_current_connection, nullptr, &size, &data, nullptr) == ERROR_SUCCESS && data) {
            const auto* c = static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(data);
            const DOT11_SSID& ssid = c->wlanAssociationAttributes.dot11Ssid;
            wchar_t name[80] = {};
            MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(ssid.ucSSID), static_cast<int>(ssid.uSSIDLength), name,
                                ARRAYSIZE(name) - 1);
            if (name[0]) net.device = name;
            net.signal = static_cast<int>(c->wlanAssociationAttributes.wlanSignalQuality);
            net.security = SecurityName(c->wlanSecurityAttributes.dot11AuthAlgorithm);
            release(data);
        }
        data = nullptr;
        if (query(h, &guid, wlan_intf_opcode_channel_number, nullptr, &size, &data, nullptr) == ERROR_SUCCESS && data) {
            const ULONG ch = *static_cast<const ULONG*>(data);
            net.channel = std::to_wstring(ch);
            // The channel numbers of the 2.4 and 5 GHz bands do not overlap.
            // (6 GHz reuses low numbers; it cannot be told apart from here.)
            net.band = ch >= 1 && ch <= 14 ? L"2,4 GHz" : ch >= 32 ? L"5 GHz" : L"";
            release(data);
        }
        close(h, nullptr);
    }
    FreeLibrary(lib);
}

} // namespace

Net ReadNet() {
    Net net;
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != ERROR_SUCCESS) return net;
    // The connection in use: up, with a way out (a gateway), and the one the
    // system prefers (lowest metric). Virtual switches and tunnels have none.
    const IP_ADAPTER_ADDRESSES* best = nullptr;
    for (const IP_ADAPTER_ADDRESSES* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        if (!a->FirstGatewayAddress) continue;
        if (!best || a->Ipv4Metric < best->Ipv4Metric) best = a;
    }
    if (!best) return net;
    net.kind = best->IfType == IF_TYPE_IEEE80211 ? 1 : 2;
    if (best->PhysicalAddressLength >= 6) {
        wchar_t mac[40];
        const BYTE* m = best->PhysicalAddress;
        swprintf_s(mac, L"%02X-%02X-%02X-%02X-%02X-%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        net.mac = mac;
    }
    net.device = best->Description ? best->Description : L"";
    bool v6Local = false;
    for (const IP_ADAPTER_UNICAST_ADDRESS* u = best->FirstUnicastAddress; u; u = u->Next) {
        const SOCKADDR* sa = u->Address.lpSockaddr;
        if (!sa) continue;
        if (sa->sa_family == AF_INET && net.ipv4.empty()) {
            net.ipv4 = Ip(sa);
            const unsigned bits = u->OnLinkPrefixLength > 32 ? 32u : u->OnLinkPrefixLength;
            const uint32_t mask = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
            wchar_t m[32];
            swprintf_s(m, L"%u.%u.%u.%u", mask >> 24, (mask >> 16) & 255, (mask >> 8) & 255, mask & 255);
            net.subnet = m;
        } else if (sa->sa_family == AF_INET6 && (net.ipv6.empty() || (v6Local && !LinkLocal6(sa)))) {
            // A routable address says more than the link's own (the console lists both).
            net.ipv6 = Ip(sa) + L"/" + std::to_wstring(u->OnLinkPrefixLength);
            v6Local = LinkLocal6(sa);
        }
    }
    for (const IP_ADAPTER_GATEWAY_ADDRESS* g = best->FirstGatewayAddress; g; g = g->Next) {
        const SOCKADDR* sa = g->Address.lpSockaddr;
        if (!sa) continue;
        if (sa->sa_family == AF_INET && net.gateway4.empty()) net.gateway4 = Ip(sa);
        if (sa->sa_family == AF_INET6 && net.gateway6.empty()) net.gateway6 = Ip(sa);
    }
    for (const IP_ADAPTER_DNS_SERVER_ADDRESS* d = best->FirstDnsServerAddress; d; d = d->Next) {
        const SOCKADDR* sa = d->Address.lpSockaddr;
        if (!sa) continue;
        std::wstring& first = sa->sa_family == AF_INET ? net.dns1_4 : net.dns1_6;
        std::wstring& second = sa->sa_family == AF_INET ? net.dns2_4 : net.dns2_6;
        if (first.empty()) first = Ip(sa);
        else if (second.empty()) second = Ip(sa);
    }
    if (net.kind == 1) ReadWifi(best->AdapterName, net);
    return net;
}

int Probe() {
    int result = 1;
    HINTERNET session = WinHttpOpen(L"NSW-Home", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
    HINTERNET conn = WinHttpConnect(session, kProbeHost, INTERNET_DEFAULT_HTTP_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", kProbePath, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                         : nullptr;
    if (req) {
        // No cookies, no credentials, and a redirect (a sign-in page) is an answer in itself.
        DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            char body[64] = {};
            DWORD got = 0;
            WinHttpReadData(req, body, sizeof body - 1, &got);
            result = !strncmp(body, "Microsoft Connect Test", 22) ? 0 : 2;
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

bool OpenBluetoothSettings() {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

// ---- sound devices -----------------------------------------------------------

namespace {

// PKEY_AudioEndpoint_FormFactor and PKEY_Device_FriendlyName, spelt out so
// that no header has to define storage for them.
const PROPERTYKEY kFormFactor = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::atomic<int> testHeadphones{-1};

bool Enumerator(IMMDeviceEnumerator** out) {
    NeedCom();
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(out)));
}

bool DefaultDevice(IMMDeviceEnumerator* en, EDataFlow flow, IMMDevice** out) {
    return SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, out)) && *out;
}

std::wstring DeviceName(IMMDevice* dev) {
    Com<IPropertyStore> props;
    std::wstring name;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return name;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

// The device calls itself headphones or a headset. Many sound cards present a
// 3.5 mm socket as "speakers" whatever is plugged in: those cannot be told.
bool IsHeadphones(IMMDevice* dev) {
    Com<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    bool yes = false;
    if (SUCCEEDED(props->GetValue(kFormFactor, &v)) && v.vt == VT_UI4) yes = v.ulVal == Headphones || v.ulVal == Headset;
    PropVariantClear(&v);
    return yes;
}

// 1 headphones, 0 not, -1 no playback device at all.
int HeadphonesNow(IMMDeviceEnumerator* en) {
    const int forced = testHeadphones.load();
    if (forced >= 0) return forced;
    Com<IMMDevice> dev;
    if (!DefaultDevice(en, eRender, &dev)) return -1;
    return IsHeadphones(dev.p) ? 1 : 0;
}

// The mixer entries of one process, on every active playback device (after a
// device change the emulator's sound may be on the old one or the new one).
std::vector<ISimpleAudioVolume*> Sessions(IMMDeviceEnumerator* en, DWORD pid) {
    std::vector<ISimpleAudioVolume*> out;
    Com<IMMDeviceCollection> devices;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return out;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        Com<IMMDevice> dev;
        Com<IAudioSessionManager2> manager;
        Com<IAudioSessionEnumerator> list;
        if (FAILED(devices->Item(i, &dev))) continue;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager)))) continue;
        if (FAILED(manager->GetSessionEnumerator(&list))) continue;
        int n = 0;
        list->GetCount(&n);
        for (int k = 0; k < n; k++) {
            Com<IAudioSessionControl> control;
            Com<IAudioSessionControl2> control2;
            if (FAILED(list->GetSession(k, &control))) continue;
            if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2)))) continue;
            DWORD owner = 0;
            if (FAILED(control2->GetProcessId(&owner)) || owner != pid) continue;
            ISimpleAudioVolume* volume = nullptr;
            if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume))) && volume)
                out.push_back(volume);
        }
    }
    return out;
}

void ReleaseAll(std::vector<ISimpleAudioVolume*>& list) {
    for (ISimpleAudioVolume* v : list) v->Release();
    list.clear();
}

} // namespace

Audio ReadAudio() {
    Audio a;
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(&en)) return a;
    a.ok = true;
    Com<IMMDevice> out, in;
    if (DefaultDevice(en.p, eRender, &out)) {
        a.output = DeviceName(out.p);
        a.headphones = IsHeadphones(out.p);
    }
    if (DefaultDevice(en.p, eCapture, &in)) a.input = DeviceName(in.p);
    return a;
}

namespace {
bool MicEndpoint(IAudioEndpointVolume** out) {
    Com<IMMDeviceEnumerator> en;
    Com<IMMDevice> dev;
    if (!Enumerator(&en) || !DefaultDevice(en.p, eCapture, &dev)) return false;
    return SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(out)));
}
} // namespace

bool MicVolume(float& level) {
    Com<IAudioEndpointVolume> v;
    return MicEndpoint(&v) && SUCCEEDED(v->GetMasterVolumeLevelScalar(&level));
}

bool SetMicVolume(float level) {
    Com<IAudioEndpointVolume> v;
    level = level < 0 ? 0.0f : level > 1 ? 1.0f : level;
    return MicEndpoint(&v) && SUCCEEDED(v->SetMasterVolumeLevelScalar(level, nullptr));
}

namespace {
struct Meter {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    IAudioMeterInformation* meter = nullptr;
} mic;
std::mutex micLock;
std::atomic<ULONGLONG> micAsked{0}; // when the level was last asked for
std::atomic<int> micRun{0};         // which opening the minder belongs to

void MicCloseLocked() {
    if (mic.client) mic.client->Stop();
    if (mic.meter) mic.meter->Release();
    if (mic.capture) mic.capture->Release();
    if (mic.client) mic.client->Release();
    mic = Meter();
}

// The microphone must not stay open because a page was left in a way that
// never said so (HOME, a game ending, the program hidden): whoever opened it
// has to keep asking for the level, or it is closed after two seconds.
void MicMinder(int run) {
    NeedCom();
    for (;;) {
        Sleep(300);
        std::lock_guard<std::mutex> hold(micLock);
        if (micRun.load() != run || !mic.client) return;
        if (GetTickCount64() - micAsked.load() > 2000) {
            MicCloseLocked();
            return;
        }
    }
}
} // namespace

void MicMeterStop() {
    std::lock_guard<std::mutex> hold(micLock);
    micRun++;
    MicCloseLocked();
}

bool MicMeterStart() {
    std::lock_guard<std::mutex> hold(micLock);
    const int run = ++micRun;
    MicCloseLocked();
    Com<IMMDeviceEnumerator> en;
    Com<IMMDevice> dev;
    if (!Enumerator(&en) || !DefaultDevice(en.p, eCapture, &dev)) return false;
    WAVEFORMATEX* format = nullptr;
    bool ok = SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mic.client))) &&
              SUCCEEDED(mic.client->GetMixFormat(&format)) && format &&
              // The system's peak meter only moves while someone is listening
              // to the device: a shared stream, a fifth of a second of buffer.
              SUCCEEDED(mic.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, format, nullptr)) &&
              SUCCEEDED(mic.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&mic.capture))) &&
              SUCCEEDED(dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mic.meter))) &&
              SUCCEEDED(mic.client->Start());
    if (format) CoTaskMemFree(format);
    if (!ok) {
        MicCloseLocked();
        return false;
    }
    micAsked = GetTickCount64();
    std::thread(MicMinder, run).detach();
    return true;
}

float MicMeterLevel() {
    std::lock_guard<std::mutex> hold(micLock);
    if (!mic.meter || !mic.capture) return -1;
    micAsked = GetTickCount64();
    // What was heard is handed straight back, unread.
    for (int guard = 0; guard < 64; guard++) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (mic.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr) != S_OK) break;
        mic.capture->ReleaseBuffer(frames);
    }
    float peak = 0;
    return SUCCEEDED(mic.meter->GetPeakValue(&peak)) ? peak : -1;
}

// ---- test tone ---------------------------------------------------------------

namespace {
std::vector<uint8_t> toneBuffer; // must outlive the playing
}

bool PlayTone() {
    PlaySoundW(nullptr, nullptr, 0);
    toneBuffer = ToneWav();
    return PlaySoundW(reinterpret_cast<LPCWSTR>(toneBuffer.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != FALSE;
}

void StopTone() {
    PlaySoundW(nullptr, nullptr, 0);
}

// ---- the running game's sound ------------------------------------------------
//
// One thread per running game. It wakes when the system says the default
// playback device changed, and once a second besides (the emulator opens its
// sound some time after it starts, and may open it again on the new device),
// and makes the game's mixer entry agree with the two switches:
//   - the default device stopped being headphones: mute, until headphones are
//     the default again or the user unmutes from the menu;
//   - headphones are the default and the cap is on: volume no higher than
//     kHeadphoneCap; put back where it was otherwise.
// Windows remembers a program's mixer entry after it closes. A game that ends
// muted or capped would start that way next time, so what was changed is
// noted in a small file and put right when the next game's entry appears (or
// at once, when the entry is still there to be changed).

namespace {

struct Undo {
    bool mute = false;
    float volume = -1; // the volume before the cap; -1 = not capped
};

struct Watch {
    HANDLE proc = nullptr, wake = nullptr;
    DWORD pid = 0;
    std::wstring undoFile;
    std::thread thread;
    std::atomic<bool> stop{false}, muteRule{false}, limitRule{false}, muted{false}, unmute{false};
    std::mutex lock;
    IMMNotificationClient* notify = nullptr; // for TestDeviceChanged
};
Watch* watch = nullptr; // made and ended on the menu's thread only

class Notifier : public IMMNotificationClient {
public:
    explicit Notifier(HANDLE wake) : wake_(wake) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // Called on a thread of the system's: only ring the bell.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) SetEvent(wake_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    virtual ~Notifier() = default;
    HANDLE wake_;
    LONG refs_ = 1;
};

Undo ReadUndo(const std::wstring& file) {
    Undo u;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") || !f) return u;
    int mute = 0;
    float volume = -1;
    if (fscanf_s(f, "mute=%d volume=%f", &mute, &volume) == 2) {
        u.mute = mute != 0;
        u.volume = volume > 1 ? 1.0f : volume;
    }
    fclose(f);
    return u;
}

void WriteUndo(const std::wstring& file, const Undo& u) {
    if (!u.mute && u.volume < 0) {
        DeleteFileW(file.c_str());
        return;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") || !f) return;
    fprintf(f, "mute=%d volume=%.3f\n", u.mute ? 1 : 0, static_cast<double>(u.volume));
    fclose(f);
}

void RunWatch(Watch* w) {
    NeedCom();
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(&en)) return;
    Notifier* notify = new Notifier(w->wake);
    en->RegisterEndpointNotificationCallback(notify);
    {
        std::lock_guard<std::mutex> hold(w->lock);
        w->notify = notify;
    }
    Undo left = ReadUndo(w->undoFile); // from a game that ended muted or capped
    Undo mine;                         // what this run has changed
    Undo noted = left;                 // what the file says
    int last = HeadphonesNow(en.p);
    for (bool first = true;; first = false) {
        const HANDLE handles[2] = {w->proc, w->wake};
        const DWORD r = first ? WAIT_TIMEOUT : WaitForMultipleObjects(2, handles, FALSE, 1000);
        const bool gone = r == WAIT_OBJECT_0 || w->stop.load();
        const int phones = HeadphonesNow(en.p);
        std::vector<ISimpleAudioVolume*> sessions = Sessions(en.p, w->pid);

        if (!sessions.empty() && (left.mute || left.volume >= 0)) {
            for (ISimpleAudioVolume* s : sessions) {
                if (left.mute) s->SetMute(FALSE, nullptr);
                if (left.volume >= 0) s->SetMasterVolume(left.volume, nullptr);
            }
            left = Undo();
        }

        // Unplugged: the default device was headphones and no longer is.
        if (!gone && w->muteRule.load() && last == 1 && phones == 0) w->muted = true;
        const bool asked = w->unmute.exchange(false);
        bool lift = w->muted.load() && (gone || asked || phones == 1 || !w->muteRule.load());
        if (lift) {
            bool done = !sessions.empty();
            for (ISimpleAudioVolume* s : sessions) done = SUCCEEDED(s->SetMute(FALSE, nullptr)) && done;
            w->muted = false;
            // If the entry could not be reached (the game is gone), the note stays for next time.
            if (done || !gone) mine.mute = false;
        } else if (w->muted.load()) {
            for (ISimpleAudioVolume* s : sessions) s->SetMute(TRUE, nullptr);
            mine.mute = true;
        }

        const bool cap = !gone && w->limitRule.load() && phones == 1;
        if (cap) {
            for (ISimpleAudioVolume* s : sessions) {
                float v = 1;
                if (FAILED(s->GetMasterVolume(&v)) || v <= kHeadphoneCap + 0.004f) continue;
                if (mine.volume < 0) mine.volume = v;
                s->SetMasterVolume(kHeadphoneCap, nullptr);
            }
        } else if (mine.volume >= 0) {
            bool done = !sessions.empty();
            for (ISimpleAudioVolume* s : sessions) done = SUCCEEDED(s->SetMasterVolume(mine.volume, nullptr)) && done;
            if (done || !gone) mine.volume = -1;
        }
        Undo note;
        note.mute = left.mute || mine.mute;
        note.volume = left.volume >= 0 ? left.volume : mine.volume;
        if (note.mute != noted.mute || note.volume != noted.volume) WriteUndo(w->undoFile, note);
        noted = note;
        ReleaseAll(sessions);
        if (phones >= 0) last = phones;
        if (gone) break;
    }
    {
        std::lock_guard<std::mutex> hold(w->lock);
        w->notify = nullptr;
    }
    en->UnregisterEndpointNotificationCallback(notify);
    notify->Release();
}

bool Switch(const char* id) {
    auto it = cfg.console.find(id);
    return it != cfg.console.end() && it->second == "on";
}

} // namespace

void GameStopped() {
    if (!watch) return;
    watch->stop = true;
    SetEvent(watch->wake);
    if (watch->thread.joinable()) watch->thread.join();
    if (watch->proc) CloseHandle(watch->proc);
    CloseHandle(watch->wake);
    delete watch;
    watch = nullptr;
}

void GameStarted(unsigned long pid) {
    GameStopped();
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!proc) return;
    watch = new Watch;
    watch->proc = proc;
    watch->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    watch->pid = pid;
    watch->undoFile = DataDir() + L"\\mixer_undo.txt";
    watch->muteRule = Switch("sound.mute_on_unplug");
    watch->limitRule = Switch("sound.limit_headphones");
    watch->thread = std::thread(RunWatch, watch);
}

void SetAudioPolicy(bool muteOnUnplug, bool limitHeadphones) {
    if (!watch) return;
    watch->muteRule = muteOnUnplug;
    watch->limitRule = limitHeadphones;
    SetEvent(watch->wake);
}

bool GameMuted() {
    return watch && watch->muted.load();
}

void GameUnmute() {
    if (!watch) return;
    watch->unmute = true;
    SetEvent(watch->wake);
}

void TestHeadphones(int state) {
    testHeadphones = state;
}

void TestDeviceChanged() {
    if (!watch) return;
    std::lock_guard<std::mutex> hold(watch->lock);
    if (watch->notify) watch->notify->OnDefaultDeviceChanged(eRender, eConsole, L"");
}

bool SessionState(unsigned long pid, bool& muted, float& volume) {
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(&en)) return false;
    std::vector<ISimpleAudioVolume*> sessions = Sessions(en.p, pid);
    if (sessions.empty()) return false;
    BOOL m = FALSE;
    sessions[0]->GetMute(&m);
    sessions[0]->GetMasterVolume(&volume);
    muted = m != FALSE;
    ReleaseAll(sessions);
    return true;
}

} // namespace pcsys

#else
// ============================================================================
// Every other system: not available
// ============================================================================

namespace pcsys {

bool Available() {
    return false;
}
Net ReadNet() {
    return {};
}
int Probe() {
    return -1;
}
bool OpenBluetoothSettings() {
    return false;
}
Audio ReadAudio() {
    return {};
}
bool MicVolume(float&) {
    return false;
}
bool SetMicVolume(float) {
    return false;
}
bool MicMeterStart() {
    return false;
}
float MicMeterLevel() {
    return -1;
}
void MicMeterStop() {}
bool PlayTone() {
    return false;
}
void StopTone() {}
void GameStarted(unsigned long) {}
void SetAudioPolicy(bool, bool) {}
bool GameMuted() {
    return false;
}
void GameUnmute() {}
void GameStopped() {}
void TestHeadphones(int) {}
void TestDeviceChanged() {}
bool SessionState(unsigned long, bool&, float&) {
    return false;
}

} // namespace pcsys
#endif
