// Program entry, main loop, input, sound and emulator control for Android, on
// SDL2. It takes the place of main.cpp (Win32) and of ../main_sdl.cpp (Linux,
// macOS) and provides the same services to the rest of the program; see
// "app (main.cpp)" in app.h. main.cpp is the specification for the behaviour.
//
// What cannot be the same as on a PC, and what happens instead:
//   * An emulator is another app. It is found by asking the package manager
//     and a game is started in it with an intent; the emulator's activity then
//     simply comes to the front and NSW-Home is paused by the system. There is
//     no hiding it behind the launch screen and no HOME key that leads back.
//   * Whether a game is still running in the emulator cannot be known. A
//     session therefore ends the moment NSW-Home is in front again, and the
//     time in between is what is recorded as play time. `runningId` is only
//     set between the press and that moment, so the menu never shows a game
//     as suspended and never offers to close one.
//   * Shared storage is reached through the system's folder picker. The
//     pickers are other activities: while one is open this app is paused, and
//     the call that opened it waits (inside SDL's event pump) for the answer.
//   * Other apps' data is out of reach: no firmware (so the drawn icons, the
//     synthesised sounds and the system's font are used) and no emulator
//     settings or profiles to read or change.
#include "app.h"
#include "friends.h"
#include "android/android_jni.h"
#include "port.h"

#include <SDL.h>
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <unistd.h>

std::vector<Game> games;
std::string runningId;
bool scanning = false;
bool ui::heldRepeat = false;

namespace {

namespace droid = port::android;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "NSW-Home", fmt, args);
    va_end(args);
}

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
SDL_Texture* frameTex = nullptr;
int texW = 0, texH = 0;   // the picture gfx draws
SDL_Rect safe = {0, 0, 0, 0}; // where it goes in the window: the part no display cut-out reaches into
int outW = 0, outH = 0;   // the window, in pixels
bool quit = false;
bool inFront = true;      // false between the system's pause and resume
bool redrawOnce = true;
Uint32 scanDoneEvent = 0, wakeEvent = 0, padKeyEvent = 0;
std::thread scanThread;
// A phone's screen has more pixels than its processor can fill 60 times a
// second with a software renderer; the picture is drawn no taller than this
// and stretched by the graphics chip. Lowered once if drawing proves slow.
int renderCap = 1080;

double NowMs() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double, std::milli>(steady_clock::now() - start).count();
}

// ---- sound ----------------------------------------------------------------
// The same synthesised sounds as the desktop builds use without firmware.

struct Note {
    float freq, start, len, vol;
};
std::vector<uint8_t> sfx[static_cast<int>(Sfx::Count)];    // RIFF WAV
std::vector<uint8_t> sfxPcm[static_cast<int>(Sfx::Count)]; // the same, in the device's format
SDL_AudioDeviceID audioDev = 0;
SDL_AudioSpec audioSpec;

std::vector<uint8_t> Synth(const std::vector<Note>& notes) {
    const int rate = 44100;
    float total = 0;
    for (const auto& n : notes) total = (std::max)(total, n.start + n.len);
    int count = static_cast<int>(total * rate);
    std::vector<float> mixBuf(static_cast<size_t>(count), 0.0f);
    for (const auto& n : notes) {
        int s0 = static_cast<int>(n.start * rate), len = static_cast<int>(n.len * rate);
        for (int i = 0; i < len && s0 + i < count; i++) {
            float t = static_cast<float>(i) / rate;
            // Fast attack, exponential decay: a soft "blip" rather than a beep.
            float env = (std::min)(1.0f, t / 0.004f) * expf(-t * 5.0f / n.len);
            mixBuf[static_cast<size_t>(s0 + i)] += sinf(6.2831853f * n.freq * t) * env * n.vol;
        }
    }
    std::vector<uint8_t> wav(static_cast<size_t>(44 + count * 2));
    auto put32 = [&](int off, uint32_t v) {
        for (int i = 0; i < 4; i++) wav[static_cast<size_t>(off + i)] = static_cast<uint8_t>(v >> (8 * i));
    };
    auto put16 = [&](int off, uint16_t v) {
        for (int i = 0; i < 2; i++) wav[static_cast<size_t>(off + i)] = static_cast<uint8_t>(v >> (8 * i));
    };
    memcpy(&wav[0], "RIFF", 4);
    put32(4, static_cast<uint32_t>(36 + count * 2));
    memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&wav[36], "data", 4);
    put32(40, static_cast<uint32_t>(count * 2));
    for (int i = 0; i < count; i++) {
        float v = (std::max)(-1.0f, (std::min)(1.0f, mixBuf[static_cast<size_t>(i)]));
        put16(44 + i * 2, static_cast<uint16_t>(static_cast<int16_t>(v * 32000)));
    }
    return wav;
}

void InitSounds() {
    sfx[static_cast<int>(Sfx::Move)] = Synth({{1400, 0, 0.045f, 0.22f}});
    sfx[static_cast<int>(Sfx::Ok)] = Synth({{880, 0, 0.07f, 0.25f}, {1320, 0.05f, 0.12f, 0.25f}});
    sfx[static_cast<int>(Sfx::Back)] = Synth({{700, 0, 0.06f, 0.22f}, {470, 0.045f, 0.11f, 0.22f}});
    sfx[static_cast<int>(Sfx::Bump)] = Synth({{170, 0, 0.11f, 0.4f}});
    sfx[static_cast<int>(Sfx::Launch)] =
        Synth({{660, 0, 0.12f, 0.2f}, {880, 0.07f, 0.12f, 0.2f}, {1320, 0.14f, 0.16f, 0.22f}, {1760, 0.21f, 0.5f, 0.2f}});
    sfx[static_cast<int>(Sfx::Error)] = Synth({{330, 0, 0.12f, 0.3f}, {262, 0.11f, 0.22f, 0.3f}});
    sfx[static_cast<int>(Sfx::Home)] = Synth({{1050, 0, 0.08f, 0.22f}, {700, 0.06f, 0.16f, 0.22f}});
    auto same = [](Sfx to, Sfx from) { sfx[static_cast<int>(to)] = sfx[static_cast<int>(from)]; };
    same(Sfx::MoveGame, Sfx::Move);
    same(Sfx::UserMove, Sfx::Move);
    for (Sfx e : {Sfx::Resume, Sfx::Dialog, Sfx::UserPick, Sfx::Settings, Sfx::Album, Sfx::Pads, Sfx::UserPage})
        same(e, Sfx::Ok);
    same(Sfx::Sleep, Sfx::Back);
}

// Opens the output device and converts every sound to its format once, so
// playing one is a plain copy into the device's queue. SDL stops the device
// by itself while the app is not in front.
void InitAudio() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        Log("no audio: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &audioSpec, 0);
    if (!audioDev) {
        Log("no audio device: %s", SDL_GetError());
        return;
    }
    for (int i = 0; i < static_cast<int>(Sfx::Count); i++) {
        const auto& wav = sfx[i];
        if (wav.empty()) continue;
        SDL_AudioSpec spec;
        Uint8* data = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(wav.data(), static_cast<int>(wav.size())), 1, &spec, &data, &len)) continue;
        SDL_AudioCVT cvt;
        int need = SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, audioSpec.format, audioSpec.channels,
                                     audioSpec.freq);
        if (need == 0) {
            sfxPcm[i].assign(data, data + len);
        } else if (need > 0) {
            std::vector<uint8_t> buf(static_cast<size_t>(len) * static_cast<size_t>(cvt.len_mult));
            memcpy(buf.data(), data, len);
            cvt.buf = buf.data();
            cvt.len = static_cast<int>(len);
            if (SDL_ConvertAudio(&cvt) == 0) {
                buf.resize(static_cast<size_t>(cvt.len_cvt));
                sfxPcm[i] = std::move(buf);
            }
        }
        SDL_FreeWAV(data);
    }
    SDL_PauseAudioDevice(audioDev, 0);
}

// ---- window ---------------------------------------------------------------

// Matches the picture gfx draws into, and the texture that carries it to the
// screen, to the largest 16:9 box that fits in the part of the window no
// display cut-out reaches into. Only that box is drawn and uploaded; the bars
// around it are the renderer's black.
void FitToWindow() {
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0 || w <= 0 || h <= 0) return;
    int inset[4];
    droid::SafeInsets(inset);
    SDL_Rect area = {inset[0], inset[1], w - inset[0] - inset[2], h - inset[1] - inset[3]};
    if (area.w < w / 2 || area.h < h / 2) area = {0, 0, w, h}; // insets that make no sense
    SDL_Rect box = area;
    if (static_cast<long long>(area.w) * 9 > static_cast<long long>(area.h) * 16) box.w = area.h * 16 / 9;
    else box.h = area.w * 9 / 16;
    box.x = area.x + (area.w - box.w) / 2;
    box.y = area.y + (area.h - box.h) / 2;
    area = box;
    int pw = area.w, ph = area.h;
    if (ph > renderCap) {
        pw = (std::max)(1, static_cast<int>(static_cast<long long>(pw) * renderCap / ph));
        ph = renderCap;
    }
    outW = w;
    outH = h;
    safe = area;
    if (pw == texW && ph == texH && frameTex) return;
    if (frameTex) SDL_DestroyTexture(frameTex);
    frameTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
    texW = pw;
    texH = ph;
    gfx::Resize(static_cast<UINT>(pw), static_cast<UINT>(ph));
    redrawOnce = true;
}

void Present() {
    int w = 0, h = 0, stride = 0;
    const uint8_t* pixels = gfx::FramePixels(w, h, stride);
    if (!pixels || !frameTex || w != texW || h != texH) return;
    SDL_UpdateTexture(frameTex, nullptr, pixels, stride);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, frameTex, nullptr, &safe);
    SDL_RenderPresent(renderer);
}

// A touch position (0..1 across the window) in the layout's 1280x720 space.
void TouchToDesign(float nx, float ny, float& x, float& y) {
    float px = nx * outW - safe.x, py = ny * outH - safe.y;
    int tx = safe.w > 0 ? static_cast<int>(px * texW / safe.w) : 0;
    int ty = safe.h > 0 ? static_cast<int>(py * texH / safe.h) : 0;
    gfx::ToDesign(tx, ty, x, y);
}

// ---- emulators ------------------------------------------------------------

// How a game is handed to each emulator of data.cpp's table: the activity that
// runs games, the intent action it is declared with, and, where the game is
// not taken from the intent's data, the string extra that carries it.
// Sources: the manifests of the current APKs and the projects' sources; see
// the Android section of the emulator catalogue.
struct Target {
    const char* id;
    const char* activity;
    const char* action;
    const char* extra;
};
const char kYuzuActivity[] = "org.yuzu.yuzu_emu.activities.EmulationActivity";
const char kNfcAction[] = "android.nfc.action.TECH_DISCOVERED"; // the yuzu family's "open this game" action
const char kViewAction[] = "android.intent.action.VIEW";
const Target kTargets[] = {
    {"eden", kYuzuActivity, kNfcAction, ""},
    {"suyu", kYuzuActivity, kNfcAction, ""},
    {"lemon", "dev.lemon.lemon_emu.activities.EmulationActivity", kNfcAction, ""},
    {"kenjinx", "org.kenjinx.android.MainActivity", "org.kenjinx.android.LAUNCH_GAME", "bootPath"},
    {"citron", "org.citron.citron_emu.activities.EmulationActivity", kNfcAction, ""},
    {"sudachi", "org.sudachi.sudachi_emu.activities.EmulationActivity", kNfcAction, ""},
    {"yuzu", kYuzuActivity, kNfcAction, ""},
    {"skyline", "emu.skyline.EmulationActivity", kViewAction, ""},
    {"strato", "org.stratoemu.strato.EmulationActivity", kViewAction, ""},
};

const Target* FindTarget(const std::string& id) {
    for (const Target& t : kTargets)
        if (id == t.id) return &t;
    return nullptr;
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// Brings cfg.emulators in line with what is installed right now. Run at start
// and every time the app comes to the front: apps come and go behind its back.
void RefreshEmulators() {
    std::map<std::string, std::wstring> found;
    for (const auto& emu : kEmus) {
        const Target* t = FindTarget(emu.id);
        if (!t) continue;
        for (const wchar_t* exe : emu.exes) {
            std::string package = port::Utf8(exe), label;
            if (!droid::EmulatorInstalled(package, t->activity, label)) continue;
            // Eden and Suyu both publish a build under the same borrowed
            // package name with the same activity; the app's own name tells
            // them apart.
            if (package.compare(0, 19, "com.miHoYo.Yuanshen") == 0 && !strcmp(t->activity, kYuzuActivity)) {
                bool suyu = LowerAscii(label).find("suyu") != std::string::npos;
                if (suyu != !strcmp(emu.id, "suyu")) continue;
            }
            found[emu.id] = port::Wide(package);
            break;
        }
    }
    bool changed = found != cfg.emulators;
    if (changed) cfg.emulators = found;
    if (!cfg.emulators.count(cfg.defaultEmu)) {
        std::string first;
        for (const auto& emu : kEmus)
            if (first.empty() && cfg.emulators.count(emu.id)) first = emu.id;
        if (first != cfg.defaultEmu) {
            cfg.defaultEmu = first;
            changed = true;
        }
    }
    if (changed) {
        SaveConfig();
        Log("emulators installed: %zu, default %s", cfg.emulators.size(), cfg.defaultEmu.empty() ? "-" : cfg.defaultEmu.c_str());
    }
}

// One game session, from the press to the return to the menu.
enum class Stage { None, Covering, FadingOut, InFront };
struct Session {
    Stage stage = Stage::None;
    std::string id, package, file;
    const Target* target = nullptr;
    ULONGLONG at = 0;       // when the current stage began
    bool leftFront = false; // the system has paused us since the emulator was started
} session;

// The launch screen plays this long before the emulator is called: long
// enough for the console's opening animation, with nothing to wait for behind it.
constexpr ULONGLONG kCoverMs = 1500;
// The loading mark's fade-out (12 frames), as on the desktop builds.
constexpr ULONGLONG kFadeMs = 220;

void EndSession(bool failed) {
    const ULONGLONG elapsed = session.stage == Stage::InFront ? GetTickCount64() - session.at : 0;
    const bool played = session.stage == Stage::InFront && !failed;
    std::string id = session.id;
    session = Session();
    runningId.clear();
    if (played) {
        Played& p = cfg.played[CurUser().id + "/" + id];
        p.last = NowIso();
        p.seconds += elapsed / 1000;
        SaveConfig();
        Log("back in front after %llu ms", static_cast<unsigned long long>(elapsed));
    }
    // An emulator that hands the screen back within seconds did not start the
    // game: the menu says so, as it does on a PC.
    ui::OnGameExit(id, failed || (played && elapsed < 4000));
    if (played) RescanAsync();
    redrawOnce = true;
}

// Calls the emulator. From here on the system takes this app out of the front.
void Fire() {
    std::string err = droid::Launch(session.package, session.target->activity, session.target->action,
                                    session.target->extra, session.file);
    if (!err.empty()) {
        Log("could not start %s: %s", session.package.c_str(), err.c_str());
        EndSession(true);
        return;
    }
    session.stage = Stage::InFront;
    session.at = GetTickCount64();
    session.leftFront = false;
}

void TrackGame() {
    if (session.stage == Stage::None) return;
    ULONGLONG now = GetTickCount64();
    if (session.stage == Stage::Covering && now - session.at >= kCoverMs) {
        session.stage = Stage::FadingOut;
        session.at = now;
        ui::OnGameReady();
    } else if (session.stage == Stage::FadingOut && now - session.at >= kFadeMs) {
        Fire();
    } else if (session.stage == Stage::InFront && !session.leftFront && now - session.at > 5000) {
        // The start was accepted but nothing took the screen.
        EndSession(true);
    }
}

void OnBackground() {
    inFront = false;
    if (session.stage == Stage::InFront) session.leftFront = true;
}

void OnForeground() {
    inFront = true;
    redrawOnce = true;
    droid::RefreshGrants();
    RefreshEmulators();
    if (session.stage == Stage::InFront && session.leftFront) EndSession(false);
}

// ---- input ----------------------------------------------------------------

SDL_GameController* sdlPads[4] = {};
bool padConnected[4] = {};

enum : unsigned {
    kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32, kX = 64, kY = 128,
    kStart = 256, kBack = 512, kLB = 1024, kRB = 2048, kGuide = 4096,
};

void PadAdded(int deviceIndex) {
    if (!SDL_IsGameController(deviceIndex)) return;
    SDL_GameController* c = SDL_GameControllerOpen(deviceIndex);
    if (!c) return;
    for (int i = 0; i < 4; i++) {
        if (sdlPads[i] == c) return; // SDL hands out the same object for an already open pad
        if (!sdlPads[i]) {
            sdlPads[i] = c;
            padConnected[i] = true;
            Log("controller %d: %s", i + 1, SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?");
            return;
        }
    }
    SDL_GameControllerClose(c); // a fifth controller: the menu shows four slots
}

void PadRemoved(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!sdlPads[i] || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sdlPads[i])) != id) continue;
        SDL_GameControllerClose(sdlPads[i]);
        sdlPads[i] = nullptr;
        padConnected[i] = false;
    }
}

// A held direction repeats the way the console's does (see main_sdl.cpp).
constexpr double kRepeatDelayMs = 13.0 * 1000.0 / 60.0;
constexpr double kRepeatEveryMs = 3.76 * 1000.0 / 60.0;

struct Repeat {
    bool down = false;
    double next = 0;
};
Repeat dirRepeat[4];  // Up, Down, Left, Right
bool keyHeld[4] = {}; // arrow keys currently held (keyboard, or a D-pad that reports as one)
unsigned keyButtons = 0; // gamepad buttons held that arrived as plain key events
// ... and the ones pressed since the last poll: an injected key goes down and
// up within the same batch of events, and must still count as one press.
unsigned keyLatch = 0;
const SDL_Scancode kDirKey[4] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
const Btn kDirBtn[4] = {Btn::Up, Btn::Down, Btn::Left, Btn::Right};

void DirectionDown(int i) {
    dirRepeat[i] = {true, NowMs() + kRepeatDelayMs};
    ui::OnButton(kDirBtn[i]);
}

void PollPads() {
    static unsigned prev = 0;
    unsigned buttons = keyButtons | keyLatch;
    keyLatch = 0;
    float lx = 0, ly = 0;
    for (SDL_GameController* c : sdlPads) {
        if (!c) continue;
        static const struct {
            SDL_GameControllerButton sdl;
            unsigned bit;
        } map[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_UP, kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight},
            {SDL_CONTROLLER_BUTTON_A, kA},              {SDL_CONTROLLER_BUTTON_B, kB},
            {SDL_CONTROLLER_BUTTON_X, kX},              {SDL_CONTROLLER_BUTTON_Y, kY},
            {SDL_CONTROLLER_BUTTON_START, kStart},      {SDL_CONTROLLER_BUTTON_BACK, kBack},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kLB},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kRB},
            {SDL_CONTROLLER_BUTTON_GUIDE, kGuide},
        };
        for (const auto& m : map)
            if (SDL_GameControllerGetButton(c, m.sdl)) buttons |= m.bit;
        float x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        float y = -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f; // SDL: down is positive
        if (fabsf(x) > fabsf(lx)) lx = x;
        if (fabsf(y) > fabsf(ly)) ly = y;
    }

    unsigned pressed = buttons & ~prev;
    prev = buttons;
    if (!inFront) {
        for (int i = 0; i < 4; i++) dirRepeat[i].down = keyHeld[i] = false;
        return;
    }

    bool padDir[4] = {
        (buttons & kUp) || ly > 0.6f,
        (buttons & kDown) || ly < -0.6f,
        (buttons & kLeft) || lx < -0.6f,
        (buttons & kRight) || lx > 0.6f,
    };
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    double now = NowMs();
    for (int i = 0; i < 4; i++) {
        if (keyHeld[i] && !keys[kDirKey[i]]) keyHeld[i] = false;
        bool held = padDir[i] || keyHeld[i];
        if (!held) {
            dirRepeat[i].down = false;
        } else if (!dirRepeat[i].down) {
            DirectionDown(i);
        } else if (now >= dirRepeat[i].next) {
            dirRepeat[i].next += kRepeatEveryMs;
            if (dirRepeat[i].next < now) dirRepeat[i].next = now + kRepeatEveryMs;
            ui::heldRepeat = true;
            ui::OnButton(kDirBtn[i]);
            ui::heldRepeat = false;
        }
    }
    bool swap = cfg.swapAB;
    if (pressed & kA) ui::OnButton(swap ? Btn::B : Btn::A);
    if (pressed & kB) ui::OnButton(swap ? Btn::A : Btn::B);
    if (pressed & kX) ui::OnButton(swap ? Btn::Y : Btn::X);
    if (pressed & kY) ui::OnButton(swap ? Btn::X : Btn::Y);
    if (pressed & kStart) ui::OnButton(Btn::Plus);
    if (pressed & kBack) ui::OnButton(Btn::Minus);
    if (pressed & kLB) ui::OnButton(Btn::L);
    if (pressed & kRB) ui::OnButton(Btn::R);
    if (pressed & kGuide) ui::OnHomePressed();
}

// Android key codes of the gamepad buttons (android/keycodes.h).
unsigned PadBitForKeyCode(int code) {
    switch (code) {
    case 96: return kA;      // BUTTON_A
    case 97: return kB;      // BUTTON_B
    case 99: return kX;      // BUTTON_X
    case 100: return kY;     // BUTTON_Y
    case 102: return kLB;    // BUTTON_L1
    case 103: return kRB;    // BUTTON_R1
    case 108: return kStart; // BUTTON_START
    case 109: return kBack;  // BUTTON_SELECT
    case 110: return kGuide; // BUTTON_MODE
    default: return 0;
    }
}

// Called on Android's input thread: hand the key to the main loop.
void PadKeyFromJava(int keyCode, bool down) {
    if (!padKeyEvent) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = padKeyEvent;
    e.user.code = keyCode;
    e.user.data1 = down ? &padKeyEvent : nullptr; // only its being null or not matters
    SDL_PushEvent(&e);
}

void WakeFromJava() {
    if (!wakeEvent) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = wakeEvent;
    SDL_PushEvent(&e);
}

bool textOn = false; // the system keyboard has been asked for

void OnKey(const SDL_Keysym& key, bool autoRepeat) {
    if (ui::WantsText()) {
        // Text entry: characters arrive as text events; these do not.
        if (key.sym == SDLK_RETURN || key.sym == SDLK_KP_ENTER) {
            if (!autoRepeat) ui::OnChar(L'\r');
        } else if (key.sym == SDLK_ESCAPE || key.sym == SDLK_AC_BACK) {
            if (!autoRepeat) ui::OnChar(27);
        } else if (key.sym == SDLK_BACKSPACE) {
            ui::OnChar(L'\b');
        }
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (key.scancode != kDirKey[i]) continue;
        if (!autoRepeat) {
            keyHeld[i] = true;
            DirectionDown(i);
        }
        return;
    }
    if (autoRepeat) return; // a held button is one press
    switch (key.sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SELECT: // the centre of a D-pad that reports as a keyboard
    case SDLK_SPACE: ui::OnButton(Btn::A); break;
    case SDLK_AC_BACK: // the system's Back button or gesture
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE: ui::OnButton(Btn::B); break;
    case SDLK_x: ui::OnButton(Btn::X); break;
    case SDLK_y: ui::OnButton(Btn::Y); break;
    case SDLK_TAB:
    case SDLK_PLUS:
    case SDLK_EQUALS:
    case SDLK_KP_PLUS: ui::OnButton(Btn::Plus); break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS: ui::OnButton(Btn::Minus); break;
    case SDLK_PAGEUP: ui::OnButton(Btn::L); break;
    case SDLK_PAGEDOWN: ui::OnButton(Btn::R); break;
    case SDLK_HOME: ui::OnHomePressed(); break;
    default: break;
    }
}

void OnText(const char* utf8) {
    if (!ui::WantsText()) return;
    for (wchar_t c : port::Wide(utf8))
        if (c >= 32) ui::OnChar(c);
}

// Shows or hides the system keyboard to follow the menu's text box.
void FollowTextEntry() {
    bool want = inFront && ui::WantsText();
    // Putting the keyboard away (Back) also makes SDL stop delivering text;
    // the text box is still open and a real keyboard must keep typing into it.
    if (want && SDL_EventState(SDL_TEXTINPUT, SDL_QUERY) != SDL_ENABLE) SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
    if (want == textOn) return;
    textOn = want;
    if (want) {
        // The text box sits in the middle of the layout; telling the system
        // where lets it move the picture clear of the keyboard.
        SDL_Rect box = {safe.x + safe.w / 4, safe.y + safe.h * 3 / 10, safe.w / 2, safe.h / 5};
        SDL_SetTextInputRect(&box);
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
    redrawOnce = true;
}

// One finger is the mouse's left button: the menu drags the list under it,
// with the finger, and takes a touch that did not slide away as a tap on
// release (ui::OnMouse / OnMouseUp; the tap's slop is the menu's).
struct TouchState {
    bool down = false;
    SDL_FingerID finger = 0;
} touch;

void OnFinger(const SDL_TouchFingerEvent& f) {
    float x, y;
    TouchToDesign(f.x, f.y, x, y);
    if (f.type == SDL_FINGERDOWN) {
        if (touch.down) return; // a second finger does nothing
        touch = {true, f.fingerId};
        ui::OnMouse(x, y, false);
        ui::OnMouse(x, y, true);
        // A text box whose keyboard was put away: a touch brings it back.
        if (ui::WantsText() && !SDL_IsTextInputActive()) SDL_StartTextInput();
        return;
    }
    if (!touch.down || f.fingerId != touch.finger) return;
    if (f.type == SDL_FINGERMOTION) {
        ui::OnMouse(x, y, false);
    } else if (f.type == SDL_FINGERUP) {
        ui::OnMouseUp(x, y);
        touch.down = false;
    }
}

// ---- debug driver ---------------------------------------------------------
// Commands for automated runs, sent with
//   adb shell am broadcast -a app.nswhome.DEBUG -p app.nswhome --es <command> <argument>
// or given as extras when the activity is started. The Java side only passes
// them on in a debuggable build; in a release build nothing arrives here.
//   goto <screen>    ui::DebugGoto (home, settings, user, dialog, setup-keys ...)
//   btn <name>       A B X Y UP DOWN LEFT RIGHT PLUS MINUS L R HOME
//   tap <x,y>        a tap at that point of the 1280x720 layout
//   text <chars>     typed into an open text box; "\n" accepts
//   keys <file>      install a key file the app can read
//   gamedir <dir>    add a folder the app can read by path to the library
//   rescan <any>     search the library again
//   setup <any>      open the first-start assistant
//   stats <any>      log frame timings

struct FrameStats {
    int frames = 0;
    double drawMs = 0, presentMs = 0, worstMs = 0;
} stats;

void RunDebug(const std::string& command, const std::string& argument) {
    std::wstring arg = port::Wide(argument);
    if (command == "goto") {
        ui::DebugGoto(arg);
    } else if (command == "btn") {
        static const struct {
            const wchar_t* name;
            Btn btn;
        } buttons[] = {{L"A", Btn::A},       {L"B", Btn::B},         {L"X", Btn::X},       {L"Y", Btn::Y},
                       {L"UP", Btn::Up},     {L"DOWN", Btn::Down},   {L"LEFT", Btn::Left}, {L"RIGHT", Btn::Right},
                       {L"PLUS", Btn::Plus}, {L"MINUS", Btn::Minus}, {L"L", Btn::L},       {L"R", Btn::R}};
        for (const auto& b : buttons)
            if (arg == b.name) ui::OnButton(b.btn);
        if (arg == L"HOME") ui::OnHomePressed();
    } else if (command == "tap") {
        float x = 0, y = 0;
        if (sscanf(argument.c_str(), "%f,%f", &x, &y) == 2) {
            ui::OnMouse(x, y, false);
            ui::OnMouse(x, y, true);
            ui::OnMouseUp(x, y); // a tap acts on release there
        }
    } else if (command == "text") {
        for (wchar_t c : arg) {
            if (!ui::WantsText()) break;
            ui::OnChar(c == L'\n' ? L'\r' : c);
        }
    } else if (command == "keys") {
        std::string err = InstallKeys(arg);
        Log("debug keys: %s", err.empty() ? "installed" : err.c_str());
        RescanAsync();
    } else if (command == "gamedir") {
        bool have = false;
        for (const auto& d : cfg.gameDirs) have = have || d == arg;
        if (!have) cfg.gameDirs.push_back(arg);
        SaveConfig();
        RescanAsync();
    } else if (command == "rescan") {
        RescanAsync();
    } else if (command == "setup") {
        cfg.setupDone = false;
        ui::FirstStart();
    } else if (command == "stats") {
        Log("stats: %d frames, draw %.2f ms average, %.2f ms worst, upload+present %.2f ms average, picture %dx%d in %dx%d, "
            "games %zu, emulators %zu, pads %d",
            stats.frames, stats.frames ? stats.drawMs / stats.frames : 0.0, stats.worstMs,
            stats.frames ? stats.presentMs / stats.frames : 0.0, texW, texH, outW, outH, games.size(), cfg.emulators.size(),
            padConnected[0] + padConnected[1] + padConnected[2] + padConnected[3]);
        for (const auto& g : games)
            Log("stats: game %s \"%s\" %s", g.id.c_str(), port::Utf8(g.name).c_str(), g.error.empty() ? "ok" : g.error.c_str());
        stats = FrameStats();
    }
    redrawOnce = true;
}

// ---- events ---------------------------------------------------------------

void HandleEvent(const SDL_Event& e) {
    if (e.type == scanDoneEvent && scanDoneEvent) {
        auto* result = static_cast<std::vector<Game>*>(e.user.data1);
        games = std::move(*result);
        delete result;
        scanning = false;
        ui::OnLibraryChanged();
        redrawOnce = true;
        return;
    }
    if (e.type == padKeyEvent && padKeyEvent) {
        unsigned bit = PadBitForKeyCode(e.user.code);
        if (e.user.data1) keyButtons |= bit, keyLatch |= bit;
        else keyButtons &= ~bit;
        return;
    }
    if (e.type == wakeEvent && wakeEvent) {
        std::string command, argument;
        while (droid::PopDebug(command, argument)) RunDebug(command, argument);
        return;
    }
    switch (e.type) {
    case SDL_QUIT:
    case SDL_APP_TERMINATING:
        quit = true;
        break;
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_DIDENTERBACKGROUND:
        OnBackground();
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        OnForeground();
        break;
    case SDL_RENDER_DEVICE_RESET:
        // The graphics context was lost while the app was away: the texture went with it.
        if (frameTex) SDL_DestroyTexture(frameTex);
        frameTex = nullptr;
        FitToWindow();
        break;
    case SDL_WINDOWEVENT:
        switch (e.window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_EXPOSED:
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            FitToWindow();
            redrawOnce = true;
            break;
        default: break;
        }
        break;
    case SDL_KEYDOWN:
        if (inFront) OnKey(e.key.keysym, e.key.repeat != 0);
        break;
    case SDL_KEYUP:
        for (int i = 0; i < 4; i++)
            if (e.key.keysym.scancode == kDirKey[i]) keyHeld[i] = false;
        break;
    case SDL_TEXTINPUT:
        if (inFront) OnText(e.text.text);
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        if (inFront) OnFinger(e.tfinger);
        break;
    case SDL_CONTROLLERDEVICEADDED:
        PadAdded(e.cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        PadRemoved(e.cdevice.which);
        break;
    default: break;
    }
}

// ---- pickers --------------------------------------------------------------

// Copies of picked files (a picture, the key file) made in the app's cache.
// Whoever asked for the file reads it before returning to the main loop, which
// then deletes them: the key file in particular is not left lying about.
std::vector<std::string> pickedCopies;

void DropPickedCopies() {
    for (const std::string& file : pickedCopies) unlink(file.c_str());
    pickedCopies.clear();
}

std::wstring PickWithSystem(droid::PickKind kind) {
    droid::PickStart(kind);
    std::string path;
    // The picker is another activity: the system pauses this one, and SDL's
    // event pump is where a paused app waits. By the time it returns, the
    // answer is in.
    while (!droid::PickPoll(path)) {
        SDL_PumpEvents();
        SDL_Delay(20);
    }
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT); // input meant for the picker
    SDL_FlushEvents(SDL_FINGERDOWN, SDL_FINGERMOTION);
    touch.down = false;
    redrawOnce = true;
    if (path.empty()) return {};
    if (kind == droid::kPickFolder) droid::RefreshGrants();
    else pickedCopies.push_back(path);
    return port::Wide(path);
}

} // namespace

// ---- services for the UI --------------------------------------------------

void PlaySfx(Sfx s) {
    if (!cfg.sounds || !audioDev || !inFront) return;
    const auto& pcm = sfxPcm[static_cast<int>(s)];
    if (pcm.empty()) return;
    // One voice, like PlaySound on Windows: a new sound cuts the previous one.
    SDL_ClearQueuedAudio(audioDev);
    SDL_QueueAudio(audioDev, pcm.data(), static_cast<Uint32>(pcm.size()));
}

std::string LaunchGame(const std::string& id) {
    if (session.stage != Stage::None) return "RUNNING";
    const Game* game = nullptr;
    for (const auto& g : games)
        if (g.id == id) game = &g;
    if (!game) return "NO_GAME";

    std::string emuId = cfg.defaultEmu;
    auto per = cfg.perGame.find(id);
    if (per != cfg.perGame.end() && cfg.emulators.count(per->second)) emuId = per->second;
    if (!cfg.emulators.count(emuId)) {
        if (cfg.emulators.empty()) return "NO_EMULATOR";
        emuId = cfg.emulators.begin()->first;
    }
    const Target* target = FindTarget(emuId);
    std::string package = port::Utf8(cfg.emulators[emuId]), label;
    // It may have been removed while the menu was open.
    if (!target || !droid::EmulatorInstalled(package, target->activity, label)) return "EMULATOR_MISSING";
    std::string file = port::NativePath(game->file);
    // The emulator is handed the game through the folder's grant; a file that
    // is only reachable by path could not be passed on.
    if (!droid::Granted(file)) return "NO_GRANT";

    session = Session();
    session.stage = Stage::Covering;
    session.id = id;
    session.package = package;
    session.file = file;
    session.target = target;
    session.at = GetTickCount64();
    runningId = id;
    Log("starting %s in %s", id.c_str(), package.c_str());
    return {};
}

void ResumeGame() {
    // Never reached: no game is shown as running while the menu is in front.
}

bool LaunchCovering() {
    return session.stage == Stage::Covering || session.stage == Stage::FadingOut;
}

void RevealGame() {
    if (LaunchCovering()) Fire();
}

void CloseGame() {
    // B on the launch screen, before the emulator has been called: nothing
    // was started, so there is nothing to close but the session.
    if (LaunchCovering()) EndSession(false);
}

void RescanAsync() {
    if (scanning) return;
    scanning = true;
    if (scanThread.joinable()) scanThread.join(); // the previous scan has delivered its result already
    scanThread = std::thread([] {
        auto* result = new std::vector<Game>(ScanLibrary());
        SDL_Event e;
        SDL_zero(e);
        e.type = scanDoneEvent;
        e.user.data1 = result;
        if (SDL_PushEvent(&e) != 1) delete result;
    });
}

void SetFullscreen(bool) {} // always full screen
int MonitorCount() {
    return 1;
}
void ApplyMonitor() {}
void QuitApp() {
    quit = true;
}
void ApplyTitle() {} // an app has no window title
std::wstring PickFolder() {
    return PickWithSystem(droid::kPickFolder);
}
std::wstring PickExe() {
    return {}; // emulators are apps here, not programs to point at
}
std::wstring PickImage() {
    return PickWithSystem(droid::kPickImage);
}
std::wstring PickKeys() {
    return PickWithSystem(droid::kPickKeys);
}
void RevealFile(const std::wstring&) {} // no file manager to show a file in
bool OpenUrl(const std::wstring&) {
    return false; // [android] not wired to an intent yet
}
int OpenObs() {
    return 0; // no OBS on Android
}
std::wstring ClipboardText() {
    std::wstring out;
    if (char* t = SDL_GetClipboardText()) {
        out = port::Wide(t);
        SDL_free(t);
    }
    return out;
}
bool OpenFolder(const std::wstring&) {
    return false; // the library is a folder grant, not a path a file manager can be pointed at
}
// Taking a picture of another program's window is done on Windows only so far.
bool RequestCapture() {
    return false;
}
bool ProgramInfo(const std::wstring&, std::wstring&, const std::wstring&) {
    return false; // no PC games here
}
std::vector<bool> ConnectedPads() {
    return {padConnected[0], padConnected[1], padConnected[2], padConnected[3]};
}

// ---- entry ----------------------------------------------------------------

int main(int, char**) {
    // The app's private folder holds everything: the config and caches
    // (NSWHOME_DATA), and the copy of the key file, in the place the shared
    // code expects an emulator's data folder to be (NSWHOME_EMUDATA). With
    // NSWHOME_DATA set the shared code also leaves every emulator's own
    // settings and profile files alone, which is right here: there are none.
    std::string files = droid::FilesDir();
    if (files.empty()) return 1;
    std::string data = files + "/data";
    bool noSetup = false;
    std::vector<std::pair<std::string, std::string>> startCommands;
    for (const std::string& extra : droid::DebugExtras()) { // empty in a release build
        size_t eq = extra.find('=');
        if (eq == std::string::npos) continue;
        std::string key = extra.substr(0, eq), value = extra.substr(eq + 1);
        if (key == "data") data = value;
        else if (key == "nosetup") noSetup = true;
        else startCommands.emplace_back(key, value);
    }
    setenv("HOME", files.c_str(), 1);
    setenv("NSWHOME_DATA", data.c_str(), 1);
    setenv("NSWHOME_EMUDATA", (files + "/emu").c_str(), 1);

    if (!gfx::Init()) return 1;
    LoadConfig();
    droid::RefreshGrants();
    RefreshEmulators();

    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); // Back is the menu's B, not "leave the app"
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); // touches are handled as touches (OnFinger)
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    // Controllers through Android's own input devices only: SDL's direct USB
    // and Bluetooth drivers would ask for permissions the menu has no use for.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        Log("cannot start video: %s", SDL_GetError());
        gfx::Shutdown();
        return 1;
    }
    scanDoneEvent = SDL_RegisterEvents(3);
    wakeEvent = scanDoneEvent + 1;
    padKeyEvent = scanDoneEvent + 2;
    droid::SetDebugWake(WakeFromJava);
    droid::SetPadKeySink(PadKeyFromJava);

    window = SDL_CreateWindow("NSW-Home", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                              SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!window || !renderer || !gfx::AttachWindow(window)) {
        Log("cannot open the window: %s", SDL_GetError());
        SDL_Quit();
        gfx::Shutdown();
        return 1;
    }
    SDL_RendererInfo info;
    SDL_zero(info);
    SDL_GetRendererInfo(renderer, &info);
    const bool vsync = (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0;

    InitSounds();
    InitAudio();
    ui::Init();
    if (!noSetup) ui::FirstStart();
    FitToWindow();
    Log("renderer %s%s, picture %dx%d in %dx%d, text in %s", info.name ? info.name : "?", vsync ? " (vsync)" : "", texW, texH,
        outW, outH, gfx::FontDescription().c_str());
    RescanAsync();
    for (const auto& c : startCommands) RunDebug(c.first, c.second);

    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now(), last = start;
    double clockDue = 0; // the status bar's clock is redrawn once a second even when nothing moves
    int warmFrames = 0, capFrames = 0;
    double capMs = 0;

    while (!quit) {
        bool render = inFront && (ui::Animating() || redrawOnce);
        // SDL parks a paused app inside the event pump, so nothing below runs
        // (no drawing, no sound) until the system brings it to the front again.
        if (!render) SDL_WaitEventTimeout(nullptr, session.stage != Stage::None ? 50 : 250);

        SDL_Event e;
        while (SDL_PollEvent(&e)) HandleEvent(e);
        DropPickedCopies();
        PollPads();
        TrackGame();
        FollowTextEntry();

        Clock::time_point now = Clock::now();
        if (inFront && NowMs() >= clockDue) {
            clockDue = NowMs() + 1000;
            redrawOnce = true;
        }
        render = inFront && (ui::Animating() || redrawOnce);
        if (!render) {
            last = now;
            continue;
        }
        redrawOnce = false;
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        FitToWindow();
        ui::Draw(std::chrono::duration<double>(now - start).count(), (std::min)(dt, 0.1f));
        gfx::End();
        double drawn = std::chrono::duration<double, std::milli>(Clock::now() - now).count();
        Present();
        double spent = std::chrono::duration<double, std::milli>(Clock::now() - now).count();
        stats.frames++;
        stats.drawMs += drawn;
        stats.presentMs += spent - drawn;
        stats.worstMs = (std::max)(stats.worstMs, drawn);
        // Drawing that cannot keep up with the display at this size is done
        // smaller from here on. Judged once, on two seconds of frames after
        // the first ones (those also load fonts and lay text out for the first time).
        if (capFrames >= 0 && renderCap > 720 && ++warmFrames > 30) {
            capFrames++;
            capMs += drawn;
            if (capFrames == 120) {
                Log("drawing takes %.1f ms a frame at %dx%d", capMs / capFrames, texW, texH);
                if (capMs / capFrames > 13.0) {
                    renderCap = 720;
                    Log("the picture is drawn 720 pixels tall from now on");
                }
                capFrames = -1;
            }
        }
        // Presenting normally waits for the display; where it does not, hold
        // the loop to 60 frames a second instead of spinning.
        if ((!vsync || spent - drawn < 2.0) && spent < 16.0) SDL_Delay(static_cast<Uint32>(16.0 - spent));
    }

    if (scanThread.joinable()) scanThread.join(); // a scan in flight still reads the config
    {
        SDL_Event e; // its result, if it is still in the queue, will not be used
        while (SDL_PollEvent(&e))
            if (e.type == scanDoneEvent) delete static_cast<std::vector<Game>*>(e.user.data1);
    }
    droid::SetDebugWake(nullptr);
    droid::SetPadKeySink(nullptr);
    DropPickedCopies();
    for (SDL_GameController*& c : sdlPads)
        if (c) SDL_GameControllerClose(c), c = nullptr;
    if (audioDev) SDL_CloseAudioDevice(audioDev);
    if (frameTex) SDL_DestroyTexture(frameTex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    friends::Shutdown(); // a user who shares says goodbye to their friends (a few seconds at most), the window already gone
    SDL_Quit();
    gfx::Shutdown();
    return 0;
}
