#define _CRT_SECURE_NO_WARNINGS // getenv and fopen, for the optional log file
#include "a11y.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace a11y {

// ---- one channel ------------------------------------------------------------

namespace {

bool monoOn = false;

uint32_t Get32(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v;
    memcpy(&v, &b[at], 4);
    return v;
}
uint16_t Get16(const std::vector<uint8_t>& b, size_t at) {
    uint16_t v;
    memcpy(&v, &b[at], 2);
    return v;
}
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    memcpy(&b[at], &v, 4);
}
void Put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    memcpy(&b[at], &v, 2);
}

// A 16-bit PCM WAV with several channels as one with a single channel, each
// sample the average of that moment's. Empty if `wav` is not such a file.
std::vector<uint8_t> Downmix(const std::vector<uint8_t>& wav) {
    std::vector<uint8_t> out;
    if (wav.size() < 44 || memcmp(&wav[0], "RIFF", 4) || memcmp(&wav[8], "WAVE", 4)) return out;
    size_t fmt = 0, data = 0, dataLen = 0;
    for (size_t at = 12; at + 8 <= wav.size();) {
        const uint32_t len = Get32(wav, at + 4);
        if (!memcmp(&wav[at], "fmt ", 4)) fmt = at + 8;
        if (!memcmp(&wav[at], "data", 4)) {
            data = at + 8;
            dataLen = (std::min)(static_cast<size_t>(len), wav.size() - data);
            break;
        }
        at += 8 + static_cast<size_t>(len) + (len & 1);
    }
    if (!fmt || !data || fmt + 16 > wav.size()) return out;
    const uint16_t format = Get16(wav, fmt), channels = Get16(wav, fmt + 2), bits = Get16(wav, fmt + 14);
    const uint32_t rate = Get32(wav, fmt + 4);
    if (format != 1 || bits != 16 || channels < 2) return out;
    const size_t frames = dataLen / (2u * channels);
    out.resize(44 + frames * 2);
    memcpy(&out[0], "RIFF", 4);
    Put32(out, 4, static_cast<uint32_t>(36 + frames * 2));
    memcpy(&out[8], "WAVEfmt ", 8);
    Put32(out, 16, 16);
    Put16(out, 20, 1);
    Put16(out, 22, 1);
    Put32(out, 24, rate);
    Put32(out, 28, rate * 2);
    Put16(out, 32, 2);
    Put16(out, 34, 16);
    memcpy(&out[36], "data", 4);
    Put32(out, 40, static_cast<uint32_t>(frames * 2));
    for (size_t f = 0; f < frames; f++) {
        int sum = 0;
        for (uint16_t c = 0; c < channels; c++) sum += static_cast<int16_t>(Get16(wav, data + (f * channels + c) * 2));
        Put16(out, 44 + f * 2, static_cast<uint16_t>(static_cast<int16_t>(sum / channels)));
    }
    return out;
}

} // namespace

void SetMono(bool on) {
    monoOn = on;
}

const std::vector<uint8_t>& Mono(const std::vector<uint8_t>& wav) {
    if (!monoOn || wav.empty()) return wav;
    // Keyed by where the sound lives: the menu's sounds are loaded once and
    // stay put. The size is checked in case one is ever replaced in place.
    struct Made {
        size_t from = 0;
        std::vector<uint8_t> wav;
    };
    static std::map<const uint8_t*, Made> made;
    Made& m = made[wav.data()];
    if (m.from != wav.size()) {
        m.from = wav.size();
        m.wav = Downmix(wav);
    }
    return m.wav.empty() ? wav : m.wav;
}

void Log(const std::string& line) {
    const char* file = getenv("NSWHOME_A11Y_LOG");
    if (!file || !*file) file = getenv("DOCKSIDE_A11Y_LOG"); // historical name, still honoured
    if (!file || !*file) return;
    FILE* f = fopen(file, "ab");
    if (!f) return;
    fwrite(line.data(), 1, line.size(), f);
    fputc('\n', f);
    fclose(f);
}

} // namespace a11y

// ---- speech -----------------------------------------------------------------

#ifdef _WIN32

#include <windows.h>
#include <objbase.h>
#include <sapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace a11y {
namespace {

// SPDFID_WaveFormatEx, written out so that nothing has to be linked for it.
const GUID kWaveFormatEx = {0xC31ADBAE, 0x527F, 0x4FF5, {0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C}};

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// An offscreen test run must never be heard: the owner is not there to ask
// what the computer is saying.
bool Dry() {
    static const bool dry = wcsstr(GetCommandLineW(), L"--shot") != nullptr ||
                            GetEnvironmentVariableW(L"NSWHOME_A11Y_DRY", nullptr, 0) > 0 ||
                            GetEnvironmentVariableW(L"DOCKSIDE_A11Y_DRY", nullptr, 0) > 0; // historical name
    return dry;
}

struct ComScope {
    HRESULT hr;
    explicit ComScope(DWORD model) : hr(CoInitializeEx(nullptr, model)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::wstring KeyString(ISpDataKey* key, const wchar_t* name) {
    std::wstring out;
    LPWSTR s = nullptr;
    if (key && SUCCEEDED(key->GetStringValue(name, &s)) && s) {
        out = s;
        CoTaskMemFree(s);
    }
    return out;
}

// The voices of one registry category. Windows 10 keeps two: the classic ones
// and those of its newer engine, which this one can speak with as well.
void AddVoices(const wchar_t* category, std::vector<Voice>& out) {
    ComPtr<ISpObjectTokenCategory> cat;
    ComPtr<IEnumSpObjectTokens> tokens;
    if (FAILED(CoCreateInstance(__uuidof(SpObjectTokenCategory), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&cat))) ||
        FAILED(cat->SetId(category, FALSE)) || FAILED(cat->EnumTokens(nullptr, nullptr, &tokens)))
        return;
    ULONG count = 0;
    tokens->GetCount(&count);
    for (ULONG i = 0; i < count; i++) {
        ComPtr<ISpObjectToken> token;
        if (FAILED(tokens->Item(i, &token))) continue;
        Voice v;
        LPWSTR id = nullptr;
        if (FAILED(token->GetId(&id)) || !id) continue;
        v.id = id;
        CoTaskMemFree(id);
        ComPtr<ISpDataKey> attrs;
        token->OpenKey(L"Attributes", &attrs);
        v.name = KeyString(attrs.Get(), L"Name");
        if (v.name.empty()) v.name = KeyString(token.Get(), nullptr);
        if (v.name.empty()) continue;
        // "C0A" or "409;9": hexadecimal language ids, the first the voice's own.
        const std::wstring lang = KeyString(attrs.Get(), L"Language");
        if (!lang.empty()) {
            wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
            if (LCIDToLocaleName(static_cast<LCID>(wcstoul(lang.c_str(), nullptr, 16)), locale, LOCALE_NAME_MAX_LENGTH, 0) > 0)
                v.language = locale;
        }
        bool known = false;
        for (const Voice& o : out) known = known || o.name == v.name;
        if (!known) out.push_back(std::move(v));
    }
}

bool SetVoice(ISpVoice* voice, const std::wstring& id) {
    ComPtr<ISpObjectToken> token;
    return !id.empty() && SUCCEEDED(CoCreateInstance(__uuidof(SpObjectToken), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&token))) &&
           SUCCEEDED(token->SetId(nullptr, id.c_str(), FALSE)) && SUCCEEDED(voice->SetVoice(token.Get()));
}

// The id to speak with: the one asked for when it is installed, else the first.
std::wstring Resolve(const std::wstring& id) {
    const std::vector<Voice>& all = Voices();
    for (const Voice& v : all)
        if (v.id == id) return id;
    return all.empty() ? std::wstring() : all[0].id;
}

std::wstring NameOf(const std::wstring& id) {
    for (const Voice& v : Voices())
        if (v.id == id) return v.name;
    return {};
}

int Clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// The thread that talks. Only the newest request matters: a line that was
// still waiting when the next one arrived is never said.
struct Talker {
    std::mutex m;
    std::condition_variable wake;
    bool pending = false;
    std::wstring text, voiceId;
    int rate = 0, volume = 100;

    void Run() {
        ComScope com(COINIT_MULTITHREADED);
        ComPtr<ISpVoice> voice;
        std::wstring current;
        for (;;) {
            std::wstring say, id;
            int r, vol;
            {
                std::unique_lock<std::mutex> lock(m);
                wake.wait(lock, [this] { return pending; });
                pending = false;
                say = text;
                id = voiceId;
                r = rate;
                vol = volume;
            }
            if (!voice && FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice)))) continue;
            if (id != current && SetVoice(voice.Get(), id)) current = id;
            voice->SetRate(Clamp(r, -10, 10));
            voice->SetVolume(static_cast<USHORT>(Clamp(vol, 0, 100)));
            // Asynchronous: the call returns while the voice is still talking,
            // and the next request purges what is left of this one.
            voice->Speak(say.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
        }
    }
};

Talker* TheTalker() {
    // Never torn down: the thread ends with the program.
    static Talker* t = [] {
        Talker* made = new Talker;
        std::thread([made] { made->Run(); }).detach();
        return made;
    }();
    return t;
}

bool talkerStarted = false;

} // namespace

const std::vector<Voice>& Voices() {
    static const std::vector<Voice> voices = [] {
        std::vector<Voice> all;
        ComScope com(COINIT_APARTMENTTHREADED);
        AddVoices(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices", all);
        AddVoices(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices", all);
        std::stable_sort(all.begin(), all.end(), [](const Voice& a, const Voice& b) {
            return (a.language.compare(0, 2, L"es") == 0) > (b.language.compare(0, 2, L"es") == 0);
        });
        return all;
    }();
    return voices;
}

bool SpeechAvailable() {
    return !Voices().empty();
}

void Speak(const std::wstring& text, const std::wstring& voiceId, int rate, int volume) {
    const std::wstring id = Resolve(voiceId);
    Log(std::string(Dry() ? "dry" : "speak") + "\t" + std::to_string(rate) + "\t" + std::to_string(volume) + "\t" +
        Utf8(NameOf(id)) + "\t" + Utf8(text));
    if (Dry() || id.empty()) return;
    Talker* t = TheTalker();
    talkerStarted = true;
    {
        std::lock_guard<std::mutex> lock(t->m);
        t->text = text;
        t->voiceId = id;
        t->rate = rate;
        t->volume = volume;
        t->pending = true;
    }
    t->wake.notify_one();
}

void Silence() {
    if (!talkerStarted) return; // nothing has been said yet: do not start the engine to stop it
    Speak(L"", L"", 0, 0);
}

bool SpeakToWav(const std::wstring& text, const std::wstring& voiceId, int rate, int volume, std::vector<uint8_t>& wav) {
    wav.clear();
    const std::wstring id = Resolve(voiceId);
    if (id.empty()) return false;
    ComScope com(COINIT_APARTMENTTHREADED);
    ComPtr<ISpVoice> voice;
    ComPtr<ISpStream> out;
    ComPtr<IStream> mem;
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = 22050;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    if (FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice))) ||
        FAILED(CoCreateInstance(__uuidof(SpStream), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&out))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) || FAILED(out->SetBaseStream(mem.Get(), kWaveFormatEx, &fmt)) ||
        // The memory stream is the output from before the first word: the
        // audio device is never opened.
        FAILED(voice->SetOutput(out.Get(), TRUE)) || !SetVoice(voice.Get(), id))
        return false;
    voice->SetRate(Clamp(rate, -10, 10));
    voice->SetVolume(static_cast<USHORT>(Clamp(volume, 0, 100)));
    if (FAILED(voice->Speak(text.c_str(), SPF_IS_NOT_XML, nullptr))) return false;
    STATSTG st = {};
    LARGE_INTEGER zero = {};
    if (FAILED(mem->Stat(&st, STATFLAG_NONAME)) || FAILED(mem->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    const ULONG bytes = static_cast<ULONG>(st.cbSize.QuadPart);
    wav.resize(44 + bytes);
    ULONG got = 0;
    if (bytes && (FAILED(mem->Read(&wav[44], bytes, &got)) || got != bytes)) {
        wav.clear();
        return false;
    }
    memcpy(&wav[0], "RIFF", 4);
    Put32(wav, 4, 36 + bytes);
    memcpy(&wav[8], "WAVEfmt ", 8);
    Put32(wav, 16, 16);
    Put16(wav, 20, 1);
    Put16(wav, 22, 1);
    Put32(wav, 24, fmt.nSamplesPerSec);
    Put32(wav, 28, fmt.nAvgBytesPerSec);
    Put16(wav, 32, 2);
    Put16(wav, 34, 16);
    memcpy(&wav[36], "data", 4);
    Put32(wav, 40, bytes);
    return true;
}

} // namespace a11y

#else // ---- no speech engine known here ----------------------------------------

namespace a11y {

bool SpeechAvailable() {
    return false;
}
const std::vector<Voice>& Voices() {
    static const std::vector<Voice> none;
    return none;
}
void Speak(const std::wstring&, const std::wstring&, int, int) {}
void Silence() {}
bool SpeakToWav(const std::wstring&, const std::wstring&, int, int, std::vector<uint8_t>& wav) {
    wav.clear();
    return false;
}

} // namespace a11y

#endif
