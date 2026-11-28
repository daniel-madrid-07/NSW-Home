// Program entry, main loop, input, sound and emulator process control for
// Linux and macOS, on SDL2. It takes the place of main.cpp (the Win32 one) and
// provides the same services to the rest of the program; see "app (main.cpp)"
// in app.h. main.cpp is the specification for the behaviour here.
//
// What cannot be the same as on Windows, and what happens instead:
//   * The emulator is a child process started with fork/exec. Its windows
//     belong to the desktop, not to us: there is no hiding it behind the launch
//     screen, no taking its taskbar button away and no moving it to the menu's
//     monitor. The launch screen shows for three seconds and then the menu
//     gets out of the way (it minimises itself where the desktop lets a
//     program un-minimise again, i.e. not on Wayland).
//   * There is no system-wide HOME key. While a game is on screen the menu
//     comes back with the controller's Guide button (if the desktop still
//     delivers controller input to a window in the background) or by
//     switching to the menu's window.
//   * "Close software" asks the emulator to quit (SIGTERM to its process
//     group) and only kills it if it has not gone after three seconds.
//   * File pickers are the desktop's own helper programs (zenity or kdialog,
//     osascript on macOS); without one the pickers return nothing.
#include "app.h"
#include "friends.h"
#include "fwfont.h"
#include "fwsound.h"
#include "port.h"

#include <SDL.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <cctype>
#include <climits>
#endif

std::vector<Game> games;
std::string runningId;
bool scanning = false;
bool ui::heldRepeat = false;

namespace {

bool simulate = false;  // offscreen or scripted run: no emulator is started and nothing opens on the desktop
bool offscreen = false; // --shot / --shotseq: there is no window and no event loop at all
bool logging = false;  // --log: append diagnostics to <data folder>/debug.log

void Log(const char* fmt, ...) {
    if (!logging) return;
    FILE* f = fopen(port::NativePath(DataDir() + L"\\debug.log").c_str(), "a");
    if (!f) return;
    fprintf(f, "%8llu  ", static_cast<unsigned long long>(GetTickCount64() % 100000000ULL));
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
SDL_Texture* frameTex = nullptr;
int texW = 0, texH = 0;
bool quit = false;
bool fullscreen = false;
bool windowVisible = true;  // false while a game has the screen: the menu stops drawing
bool forceForeground = false; // scripted run: behave as the focused window whatever the desktop says
bool canUnminimise = true;
Uint32 scanDoneEvent = 0;
std::thread scanThread;
volatile sig_atomic_t raiseRequested = 0;

double NowMs() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double, std::milli>(steady_clock::now() - start).count();
}

// ---- helper programs ------------------------------------------------------

bool IsExecutableFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

#ifdef __APPLE__
// Resolves a macOS .app bundle directory to its actual executable via
// Info.plist (CFBundleExecutable), falling back to Contents/MacOS/<name>.
std::string ResolveMacAppExecutable(const std::string& path) {
    if (path.size() <= 4) return path;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".app") return path;

    if (CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.data()),
            static_cast<CFIndex>(path.size()), true)) {
        std::string resolved;
        if (CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url)) {
            if (CFURLRef exeUrl = CFBundleCopyExecutableURL(bundle)) {
                if (CFURLRef absUrl = CFURLCopyAbsoluteURL(exeUrl)) {
                    char buf[PATH_MAX];
                    if (CFURLGetFileSystemRepresentation(absUrl, true, reinterpret_cast<UInt8*>(buf), sizeof buf))
                        resolved = buf;
                    CFRelease(absUrl);
                }
                CFRelease(exeUrl);
            }
            CFRelease(bundle);
        }
        CFRelease(url);
        if (!resolved.empty() && IsExecutableFile(resolved)) return resolved;
    }

    size_t slash = path.find_last_of('/');
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    return path + "/Contents/MacOS/" + name.substr(0, name.size() - 4);
}
#endif

// Full path of a program on PATH, or empty.
std::string FindProgram(const std::string& name) {
    const char* path = getenv("PATH");
    std::string dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t at = 0;
    while (at <= dirs.size()) {
        size_t colon = dirs.find(':', at);
        std::string dir = dirs.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        if (!dir.empty() && IsExecutableFile(dir + "/" + name)) return dir + "/" + name;
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    return {};
}

std::vector<char*> ArgvOf(std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    return argv;
}

#ifndef __APPLE__
// Runs a helper and returns what it printed; `ok` is false if it could not be
// started or exited with an error (a cancelled dialog does that).
std::string RunCapture(std::vector<std::string> args, bool& ok) {
    ok = false;
    int fds[2];
    if (args.empty() || pipe(fds) != 0) return {};
    std::vector<char*> argv = ArgvOf(args);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return {};
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, STDERR_FILENO);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
#endif

// Starts a helper and forgets about it (it is handed to init, so it never
// lingers as a zombie of ours).
void RunDetached(std::vector<std::string> args) {
    if (args.empty()) return;
    std::vector<char*> argv = ArgvOf(args);
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, STDIN_FILENO);
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
            }
            execv(argv[0], argv.data());
        }
        _exit(0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

// ---- sound ----------------------------------------------------------------

struct Note {
    float freq, start, len, vol;
};
std::vector<uint8_t> sfx[static_cast<int>(Sfx::Count)];    // RIFF WAV, as on Windows
std::vector<uint8_t> sfxPcm[static_cast<int>(Sfx::Count)]; // the same, converted to the device's format
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
    // The extra events reuse the nearest synthetic sound unless the firmware
    // provides its own below.
    auto same = [](Sfx to, Sfx from) { sfx[static_cast<int>(to)] = sfx[static_cast<int>(from)]; };
    same(Sfx::MoveGame, Sfx::Move);
    same(Sfx::UserMove, Sfx::Move);
    for (Sfx e : {Sfx::Resume, Sfx::Dialog, Sfx::UserPick, Sfx::Settings, Sfx::Album, Sfx::Pads, Sfx::UserPage})
        same(e, Sfx::Ok);
    same(Sfx::Sleep, Sfx::Back);

    // With the owner's console firmware installed, use the menu's own sounds.
    // They are decoded from that firmware at start-up and never stored here.
    std::wstring fw = FirmwareDir(), keys = KeysDir();
    std::vector<fwsound::Sound> real;
    std::string err;
    if (!fw.empty() && !keys.empty() && fwsound::LoadMenuSounds(fw, keys, real, err)) {
        static const struct {
            Sfx event;
            const char* name;
        } map[] = {
            {Sfx::Move, "SeBtnFocus"},        {Sfx::Ok, "SeBtnDecide"},           {Sfx::Back, "SeFooterDecideBack"},
            {Sfx::Bump, "SeGameIconLimit"},   {Sfx::Launch, "SeGameIconDecide"},  {Sfx::Error, "SeKeyError"},
            {Sfx::Home, "StartupMenu_Game"},  {Sfx::MoveGame, "SeGameIconFocus"}, {Sfx::Resume, "SeGameIconDecideSupend"},
            {Sfx::Dialog, "SeDialogOpen"},    {Sfx::UserMove, "SeAccountFocus"},  {Sfx::UserPick, "SeAccountDecide"},
            {Sfx::Settings, "StartupSet"},    {Sfx::Album, "StartupAlbum"},       {Sfx::Pads, "StartupController"},
            {Sfx::UserPage, "StartupMypage"}, {Sfx::Sleep, "StartupSleep"},
        };
        for (const auto& m : map)
            for (auto& snd : real)
                if (snd.name == m.name && !snd.wav.empty()) sfx[static_cast<int>(m.event)] = snd.wav;
        Log("firmware sounds: %zu", real.size());
    } else if (!err.empty()) {
        Log("firmware sounds not used: %s", err.c_str());
    }
}

// Opens the output device and converts every sound to its format once, so
// playing one is a plain copy into the device's queue.
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

struct Display {
    int index;
    SDL_Rect bounds;
};

// All displays, ordered left to right (then top to bottom).
std::vector<Display> Displays() {
    std::vector<Display> out;
    int n = SDL_GetNumVideoDisplays();
    for (int i = 0; i < n; i++) {
        SDL_Rect r;
        if (SDL_GetDisplayBounds(i, &r) == 0) out.push_back({i, r});
    }
    std::sort(out.begin(), out.end(), [](const Display& a, const Display& b) {
        return a.bounds.x != b.bounds.x ? a.bounds.x < b.bounds.x : a.bounds.y < b.bounds.y;
    });
    return out;
}

// The display the menu should be on: cfg.monitor, or the primary one.
Display TargetDisplay() {
    std::vector<Display> all = Displays();
    if (cfg.monitor >= 0 && cfg.monitor < static_cast<int>(all.size())) return all[static_cast<size_t>(cfg.monitor)];
    for (const Display& d : all)
        if (d.index == 0) return d;
    return {0, {0, 0, 1280, 720}};
}

void CenterWindowed(const Display& d) {
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(d.index), SDL_WINDOWPOS_CENTERED_DISPLAY(d.index));
}

void ApplyFullscreen(bool on) {
    if (on == fullscreen) return;
    fullscreen = on;
    // Borderless, at the desktop's own resolution: the display mode is never changed.
    SDL_SetWindowFullscreen(window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

// Matches the picture gfx draws into, and the texture that carries it to the
// screen, to the window's size in real pixels.
void FitToWindow() {
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0 || w <= 0 || h <= 0) return;
    // NSWHOME_RENDER_HEIGHT=<pixels> draws at most that tall and lets the
    // graphics card stretch the result: for a slow machine on a 4K screen.
    static const int cap = [] {
        const char* v = getenv("NSWHOME_RENDER_HEIGHT");
        if (!v) v = getenv("DOCKSIDE_RENDER_HEIGHT"); // historical name, still honoured
        return v ? atoi(v) : 0;
    }();
    if (cap >= 180 && h > cap) {
        w = (std::max)(1, static_cast<int>(static_cast<long long>(w) * cap / h));
        h = cap;
    }
    if (w == texW && h == texH && frameTex) return;
    if (frameTex) SDL_DestroyTexture(frameTex);
    frameTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
    texW = w;
    texH = h;
    gfx::Resize(static_cast<UINT>(w), static_cast<UINT>(h));
}

void Present() {
    int w = 0, h = 0, stride = 0;
    const uint8_t* pixels = gfx::FramePixels(w, h, stride);
    if (!pixels || !frameTex || w != texW || h != texH) return;
    SDL_UpdateTexture(frameTex, nullptr, pixels, stride);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, frameTex, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

void MouseToDesign(int mx, int my, float& x, float& y) {
    // Mouse positions come in window units; the picture is in pixels.
    int ww = 1, wh = 1;
    SDL_GetWindowSize(window, &ww, &wh);
    int px = ww > 0 ? static_cast<int>(static_cast<long long>(mx) * texW / ww) : mx;
    int py = wh > 0 ? static_cast<int>(static_cast<long long>(my) * texH / wh) : my;
    gfx::ToDesign(px, py, x, y);
}

bool MenuFocused() {
    return forceForeground || (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

// ---- emulator process -----------------------------------------------------

pid_t gamePid = 0;             // also its process group
ULONGLONG gameStart = 0;
ULONGLONG gameReadyAt = 0;     // tick at which the game was taken to be drawing
ULONGLONG closeAskedAt = 0;    // "close software" was chosen at this tick
bool closeForced = false;
bool menuHiddenForGame = false; // the launch screen is over; the game has the screen

void ShowMenu() {
    windowVisible = true;
    SDL_RestoreWindow(window);
    SDL_RaiseWindow(window); // a request; desktops with focus-stealing prevention may only flash the task bar
}

void Reveal() {
    menuHiddenForGame = true;
    windowVisible = false; // the menu stops drawing until it is asked for again
    // Get out of the emulator's way. Skipped where the window could not be
    // brought back afterwards.
    if (canUnminimise && !forceForeground) SDL_MinimizeWindow(window);
    Log("reveal: the game has the screen");
}

void OnGameExited() {
    ULONGLONG elapsed = GetTickCount64() - gameStart;
    // Whatever the emulator started goes with it, as with the job object on Windows.
    if (closeAskedAt) kill(-gamePid, SIGKILL);
    gamePid = 0;
    closeAskedAt = 0;
    closeForced = false;
    menuHiddenForGame = false;
    gameReadyAt = 0;

    std::string id = runningId;
    runningId.clear();
    Played& p = cfg.played[CurUser().id + "/" + id];
    p.last = NowIso();
    p.seconds += elapsed / 1000;
    SaveConfig();
    Log("emulator exited after %llu ms", static_cast<unsigned long long>(elapsed));

    ShowMenu();
    ui::OnGameExit(id, elapsed < 4000);
    RescanAsync();
}

void TrackGame() {
    if (!gamePid) return;
    int status = 0;
    pid_t r = waitpid(gamePid, &status, WNOHANG);
    if (r == gamePid || (r < 0 && errno == ECHILD)) {
        OnGameExited();
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (closeAskedAt && !closeForced && now - closeAskedAt > 3000) {
        kill(-gamePid, SIGKILL);
        closeForced = true;
    }
    if (menuHiddenForGame) return;
    // Nothing tells us when the game starts drawing, so the launch screen gets
    // the same grace period the Windows build gives an emulator without a
    // ready signal. Then, as there: the loading mark fades out over 12 frames
    // and the menu steps aside once that has finished.
    if (!gameReadyAt) {
        if (now - gameStart > 3000) {
            gameReadyAt = now;
            ui::OnGameReady();
        }
    } else if (now - gameReadyAt >= 220) {
        Reveal();
    }
}

// Emulators that are not in the config yet are looked for under the names
// they are installed with here: the table in data.cpp lists Windows file
// names ("Ryujinx.exe"), and its search of the usual Windows folders finds
// nothing on these systems. The name only finds a program; which emulator it
// is (three are called Ryujinx) is decided by the shared IdentifyEmulator, so
// a fork is filed under its own id here as on Windows. The forks have no file
// name of their own and are reached through Ryujinx's.
void DetectNativeEmulators() {
    bool changed = false;
    for (const auto& emu : kEmus) {
        if (emu.exes.empty()) continue;
        std::string hit;
        for (const wchar_t* exe : emu.exes) {
            std::string name = port::Utf8(exe);
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
            std::string lower = name;
            for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            std::vector<std::string> candidates;
#ifdef __APPLE__
            for (const std::string& apps : {std::string("/Applications"), port::HomeDir() + "/Applications"}) {
                candidates.push_back(apps + "/" + name + ".app/Contents/MacOS/" + name);
                candidates.push_back(apps + "/" + port::Utf8(emu.label) + ".app/Contents/MacOS/" + name);
            }
#endif
            for (const std::string& n : {name, lower}) {
                candidates.push_back(FindProgram(n));
                candidates.push_back(port::HomeDir() + "/.local/bin/" + n);
                candidates.push_back("/opt/" + n + "/" + n);
                candidates.push_back(port::HomeDir() + "/Applications/" + n + ".AppImage");
            }
            for (const std::string& c : candidates)
                if (hit.empty() && !c.empty() && IsExecutableFile(c)) hit = c;
            if (!hit.empty()) break;
        }
        if (hit.empty()) continue;
        // A file whose name the shared code does not know (an AppImage named
        // after the emulator) is taken for what its name says.
        bool configured = false; // under whatever id: not read and identified again at every start
        for (const auto& kv : cfg.emulators) configured = configured || kv.second == port::Wide(hit);
        if (configured) continue;
        std::string id = IdentifyEmulator(port::Wide(hit));
        if (id.empty()) id = emu.id;
        if (cfg.emulators.count(id)) continue;
        cfg.emulators[id] = port::Wide(hit);
        changed = true;
        Log("found %s at %s", id.c_str(), hit.c_str());
    }
    if (cfg.defaultEmu.empty())
        for (const auto& emu : kEmus)
            if (cfg.emulators.count(emu.id)) {
                cfg.defaultEmu = emu.id;
                changed = true;
                break;
            }
    if (changed) SaveConfig();
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

// A held direction repeats the way the console's does. Measured on a capture
// of the game row with the stick held: the second move comes 13 frames after
// the first, then one move about every 3.76 frames (16 a second).
constexpr double kRepeatDelayMs = 13.0 * 1000.0 / 60.0;
constexpr double kRepeatEveryMs = 3.76 * 1000.0 / 60.0;

struct Repeat {
    bool down = false;
    double next = 0; // when the next repeat is due, in NowMs() time
};
Repeat dirRepeat[4];  // Up, Down, Left, Right
bool keyHeld[4] = {}; // arrow keys currently held (keyboard)
const SDL_Scancode kDirKey[4] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
const Btn kDirBtn[4] = {Btn::Up, Btn::Down, Btn::Left, Btn::Right};

// First press of a direction: act at once and arm the repeat.
void DirectionDown(int i) {
    dirRepeat[i] = {true, NowMs() + kRepeatDelayMs};
    ui::OnButton(kDirBtn[i]);
}

void HomeFromGame() {
    ShowMenu();
    ui::OnHomePressed();
}

void PollPads() {
    static unsigned prev = 0;
    unsigned buttons = 0;
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

    // While a game has the screen, the pad belongs to the game; only the
    // Guide button is ours.
    if (!windowVisible) {
        if (pressed & kGuide) HomeFromGame();
        return;
    }
    // The launch screen keeps taking input even though the emulator window
    // may hold the focus by now.
    bool focused = MenuFocused();
    if (!focused && !LaunchCovering()) {
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
        // A key press counts as held only while the key really is down.
        if (keyHeld[i] && !(focused && keys[kDirKey[i]])) keyHeld[i] = false;
        bool held = padDir[i] || keyHeld[i];
        if (!held) {
            dirRepeat[i].down = false;
        } else if (!dirRepeat[i].down) {
            DirectionDown(i); // pad: the key path has already armed it
        } else if (now >= dirRepeat[i].next) {
            // Step from the due time, not from now, so the rate holds its
            // average even though frames only come every 16.7 ms.
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

// A key that was taken as a button also arrives as text (the space bar is A
// and a space). That text must not land in a text entry the same press just
// opened, so text is only accepted after a key press that found the entry
// already open.
bool textArmed = false;

void OnKey(const SDL_Keysym& key, bool autoRepeat) {
    textArmed = windowVisible && ui::WantsText();
    if (!windowVisible) {
        // The menu is frozen behind a game but its window got a key anyway.
        if (key.sym == SDLK_HOME && !autoRepeat) HomeFromGame();
        return;
    }
    if (ui::WantsText()) {
        // Text entry: characters arrive as text events; these three keys are
        // the ones Windows delivers as characters too.
        if (key.sym == SDLK_RETURN || key.sym == SDLK_KP_ENTER) {
            if (!autoRepeat) ui::OnChar(L'\r');
        } else if (key.sym == SDLK_ESCAPE) {
            if (!autoRepeat) ui::OnChar(27);
        } else if (key.sym == SDLK_BACKSPACE) {
            ui::OnChar(L'\b');
        }
        return;
    }
    // Arrow keys repeat at the console's rate, not the keyboard's: the
    // system's own repeats are dropped and PollPads times them instead.
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
    case SDLK_SPACE: ui::OnButton(Btn::A); break;
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
    case SDLK_F11:
        cfg.fullscreen = !fullscreen;
        ApplyFullscreen(cfg.fullscreen);
        SaveConfig();
        break;
    default: break;
    }
}

void OnText(const char* utf8) {
    if (!windowVisible || !textArmed || !ui::WantsText()) return;
    for (wchar_t c : port::Wide(utf8))
        if (c >= 32) ui::OnChar(c);
}

bool redrawOnce = false; // the window was resized or uncovered: draw even if nothing is moving

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
    switch (e.type) {
    case SDL_QUIT:
        quit = true;
        break;
    case SDL_WINDOWEVENT:
        switch (e.window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_EXPOSED:
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_SHOWN:
            FitToWindow();
            redrawOnce = true;
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            // Switching to the menu's window while a game is on screen is how
            // the menu is called back when there is no Guide button to press.
            if (gamePid && menuHiddenForGame && !windowVisible) HomeFromGame();
            redrawOnce = true;
            break;
        case SDL_WINDOWEVENT_CLOSE:
            quit = true;
            break;
        default: break;
        }
        break;
    case SDL_KEYDOWN:
        OnKey(e.key.keysym, e.key.repeat != 0);
        break;
    case SDL_KEYUP:
        for (int i = 0; i < 4; i++)
            if (e.key.keysym.scancode == kDirKey[i]) keyHeld[i] = false;
        break;
    case SDL_TEXTINPUT:
        OnText(e.text.text);
        break;
    case SDL_MOUSEMOTION: {
        float x, y;
        MouseToDesign(e.motion.x, e.motion.y, x, y);
        if (windowVisible) ui::OnMouse(x, y, false);
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT && windowVisible) {
            float x, y;
            MouseToDesign(e.button.x, e.button.y, x, y);
            SDL_CaptureMouse(SDL_TRUE); // a drag that leaves the window still comes back to end
            ui::OnMouse(x, y, true);
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT) {
            float x, y;
            MouseToDesign(e.button.x, e.button.y, x, y);
            SDL_CaptureMouse(SDL_FALSE);
            ui::OnMouseUp(x, y); // also while hidden: a drag begun on the menu must end
        }
        break;
    case SDL_MOUSEWHEEL:
        if (windowVisible) ui::OnWheel(e.wheel.preciseY);
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

std::wstring PickWithDialog(bool folder, bool image = false, bool keys = false) {
    if (simulate) return {}; // offscreen or scripted run: nothing may open on the desktop
#ifdef __APPLE__
    std::string path = port::MacPickPath(folder, image, keys);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT); // keys typed into the dialog are not for the menu
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path.empty() || path[0] != '/') return {};
    return port::Wide(path);
#else
    std::vector<std::string> cmd;
    std::string zenity = FindProgram("zenity"), kdialog = FindProgram("kdialog");
    if (!zenity.empty()) {
        cmd = {zenity, "--file-selection", "--title=NSW-Home"};
        if (folder) cmd.push_back("--directory");
        if (image) cmd.push_back("--file-filter=Imágenes | *.png *.jpg *.jpeg *.PNG *.JPG *.JPEG");
        if (keys) cmd.push_back("--file-filter=Claves de la consola | *.keys");
    } else if (!kdialog.empty()) {
        if (folder) cmd = {kdialog, "--getexistingdirectory", port::HomeDir()};
        else if (image) cmd = {kdialog, "--getopenfilename", port::HomeDir(), "*.png *.jpg *.jpeg|Imágenes"};
        else if (keys) cmd = {kdialog, "--getopenfilename", port::HomeDir(), "*.keys|Claves de la consola"};
        else cmd = {kdialog, "--getopenfilename", port::HomeDir()};
    }
    if (cmd.empty() || !IsExecutableFile(cmd[0])) return {};
    bool ok = false;
    std::string path = RunCapture(cmd, ok); // the menu waits, as it does behind the Windows dialog
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT); // keys typed into the dialog are not for the menu
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (!ok || path.empty() || path[0] != '/') return {};
    return port::Wide(path);
#endif
}

} // namespace

// ---- services for the UI --------------------------------------------------

void PlaySfx(Sfx s) {
    if (!cfg.sounds || !audioDev) return;
    const auto& pcm = sfxPcm[static_cast<int>(s)];
    if (pcm.empty()) return;
    // One voice, like PlaySound on Windows: a new sound cuts the previous one.
    SDL_ClearQueuedAudio(audioDev);
    SDL_QueueAudio(audioDev, pcm.data(), static_cast<Uint32>(pcm.size()));
}

std::string LaunchGame(const std::string& id) {
    if (simulate) { // offscreen or scripted run: pretend it started
        runningId = id;
        return {};
    }
    if (gamePid) return "RUNNING";
    const Game* game = nullptr;
    for (const auto& g : games)
        if (g.id == id) game = &g;
    if (!game) return "NO_GAME";

    // A PC game is its own program: started as it is, with no emulator, and
    // with no arguments but the ones written by hand into its config entry.
    const EmuDef* def = nullptr;
    std::string exe = port::NativePath(game->file);
    std::vector<std::string> args;
    if (game->pc) {
#ifdef __APPLE__
        exe = ResolveMacAppExecutable(exe);
#endif
        if (!IsExecutableFile(exe)) return "PROGRAM_MISSING";
        args = {exe};
        for (std::string& a : port::SplitCommandLine(game->args)) args.push_back(std::move(a));
    } else {
        std::string emuId = cfg.defaultEmu;
        auto per = cfg.perGame.find(id);
        if (per != cfg.perGame.end() && cfg.emulators.count(per->second)) emuId = per->second;
        if (!cfg.emulators.count(emuId)) {
            if (cfg.emulators.empty()) return "NO_EMULATOR";
            emuId = cfg.emulators.begin()->first;
        }
        def = FindEmu(emuId);
        exe = port::NativePath(cfg.emulators[emuId]);
#ifdef __APPLE__
        exe = ResolveMacAppExecutable(exe);
#endif
        if (!def || !IsExecutableFile(exe)) return "EMULATOR_MISSING";

        Display screen = TargetDisplay();
        // Both only touch an emulator's files where its folders exist; the window
        // place is a request the desktop may overrule (Wayland places windows itself).
        PrepareEmulator(emuId, screen.bounds.x, screen.bounds.y);
        PlaceEmulatorWindow(emuId, screen.bounds.x, screen.bounds.y, screen.bounds.w, screen.index);

        // The same arguments as on Windows (the user by name or by index, the
        // fork's and Suyu's extra switches), from the shared code.
        args = {exe};
        for (std::string& a : port::SplitCommandLine(EmuLaunchArgs(emuId, port::Wide(port::NativePath(game->file)))))
            args.push_back(std::move(a));
    }
    size_t slash = exe.find_last_of('/');
    std::string cwd = slash == std::string::npos || slash == 0 ? "/" : exe.substr(0, slash);

    // The pipe closes by itself when exec succeeds; if exec fails the child
    // writes the reason into it, so a missing or broken program is reported
    // as SPAWN instead of looking like a game that quit at once.
    int fds[2];
    if (pipe(fds) != 0) return "SPAWN";
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    std::vector<char*> argv = ArgvOf(args);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return "SPAWN";
    }
    if (pid == 0) {
        setpgid(0, 0); // its own process group, so it can be ended together with what it starts
        if (chdir(cwd.c_str()) != 0) {
            // not fatal: the emulator just starts in our folder
        }
        execv(argv[0], argv.data());
        int err = errno;
        ssize_t ignored = write(fds[1], &err, sizeof err);
        (void)ignored;
        _exit(127);
    }
    setpgid(pid, pid); // also from this side, in case we get to use the group first
    close(fds[1]);
    int err = 0;
    ssize_t got;
    do got = read(fds[0], &err, sizeof err);
    while (got < 0 && errno == EINTR);
    close(fds[0]);
    if (got > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        Log("exec %s failed: %s", exe.c_str(), strerror(err));
        return "SPAWN";
    }

    gamePid = pid;
    gameStart = GetTickCount64();
    gameReadyAt = 0;
    closeAskedAt = 0;
    closeForced = false;
    menuHiddenForGame = false;
    runningId = id;
    Log("started %s (pid %d) for %s", exe.c_str(), static_cast<int>(pid), id.c_str());
    return {};
}

void ResumeGame() {
    if (!gamePid) return;
    // The emulator's window cannot be raised from here; stepping aside leaves
    // it as the window on top.
    windowVisible = false;
    if (canUnminimise && !forceForeground) SDL_MinimizeWindow(window);
}

bool LaunchCovering() {
    return gamePid && !menuHiddenForGame;
}

void RevealGame() {
    if (LaunchCovering()) Reveal();
}

void CloseGame() {
    if (simulate && !runningId.empty()) {
        std::string id = runningId;
        runningId.clear();
        ui::OnGameExit(id, false);
        return;
    }
    if (!gamePid || closeAskedAt) return;
    kill(-gamePid, SIGTERM);
    closeAskedAt = GetTickCount64();
    if (!closeAskedAt) closeAskedAt = 1;
}

void RescanAsync() {
    if (scanning) return;
    if (offscreen) { // no window and no event loop to hand the result to
        games = ScanLibrary();
        ui::OnLibraryChanged();
        return;
    }
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

void SetFullscreen(bool on) {
    ApplyFullscreen(on);
}
int MonitorCount() {
    return static_cast<int>(Displays().size());
}
void ApplyMonitor() {
    Display d = TargetDisplay();
    if (fullscreen) {
        // A borderless full-screen window follows the display its window is on.
        SDL_SetWindowFullscreen(window, 0);
        CenterWindowed(d);
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        CenterWindowed(d);
    }
}
void QuitApp() {
    quit = true;
}
void ApplyTitle() {
    if (window) SDL_SetWindowTitle(window, cfg.title.empty() ? "NSW-Home" : port::Utf8(cfg.title).c_str());
}
std::wstring PickFolder() {
    return PickWithDialog(true);
}
std::wstring PickExe() {
    return PickWithDialog(false);
}
std::wstring PickImage() {
    return PickWithDialog(false, true);
}
std::wstring PickKeys() {
    return PickWithDialog(false, false, true);
}
void RevealFile(const std::wstring& file) {
    if (simulate) return; // offscreen or scripted run: nothing may open on the desktop
    std::string path = port::NativePath(file);
#ifdef __APPLE__
    RunDetached({"/usr/bin/open", "-R", path});
#else
    // There is no common "show this file" request, so the folder is opened.
    std::string opener = FindProgram("xdg-open");
    size_t slash = path.find_last_of('/');
    if (!opener.empty()) RunDetached({opener, slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash)});
#endif
}
bool OpenUrl(const std::wstring& url) {
    if (simulate) return true; // offscreen or scripted run: nothing may open on the desktop
#ifdef __APPLE__
    RunDetached({"/usr/bin/open", port::Utf8(url)});
    return true;
#else
    std::string opener = FindProgram("xdg-open");
    if (opener.empty()) return false;
    RunDetached({opener, port::Utf8(url)});
    return true;
#endif
}
int OpenObs() {
    if (simulate) return 1;
#ifdef __APPLE__
    if (!IsExecutableFile("/Applications/OBS.app/Contents/MacOS/OBS")) return 0;
    RunDetached({"/usr/bin/open", "-a", "OBS"});
    return 1;
#else
    std::string obs = FindProgram("obs");
    if (obs.empty()) return 0;
    RunDetached({obs});
    return 1;
#endif
}
std::wstring ClipboardText() {
    std::wstring out;
    if (char* t = SDL_GetClipboardText()) {
        out = port::Wide(t);
        SDL_free(t);
    }
    return out;
}
bool OpenFolder(const std::wstring& dir) {
    if (simulate) return true; // offscreen or scripted run: nothing may open on the desktop
    std::string path = port::NativePath(dir);
#ifdef __APPLE__
    RunDetached({"/usr/bin/open", path});
    return true;
#else
    std::string opener = FindProgram("xdg-open");
    if (opener.empty()) return false;
    RunDetached({opener, path});
    return true;
#endif
}
// Taking a picture of another program's window is done on Windows only so far.
bool RequestCapture() {
    return false;
}
std::vector<bool> ConnectedPads() {
    return {padConnected[0], padConnected[1], padConnected[2], padConnected[3]};
}
// A program's own name and icon are read from a Windows program file; here a
// PC game is named after its file and gets the drawn tile.
bool ProgramInfo(const std::wstring&, std::wstring&, const std::wstring&) {
    return false;
}

// ---- entry ----------------------------------------------------------------

static int RunShot(const std::wstring& screen, const std::wstring& out) {
    // Debug: render one screen offscreen to a PNG and exit, without ever
    // creating a window.
    games = ScanLibrary();
    if (!gfx::AttachOffscreen(1920, 1080)) return 2; // same size as the reference captures
    ui::Init();
    ui::DebugGoto(screen);
    for (int i = 0; i < 90; i++) {
        ui::Draw(i / 60.0, 1.0f / 60.0f);
        gfx::End();
    }
    return gfx::SaveOffscreenPng(out) ? 0 : 3;
}

// One step of a --shotseq script: do `what` just before drawing frame `frame`.
struct ScriptStep {
    int frame;
    std::wstring what;
};

static std::vector<ScriptStep> ParseScript(const std::wstring& script) {
    std::vector<ScriptStep> steps;
    size_t at = 0;
    while (at < script.size()) {
        size_t comma = script.find(L',', at);
        std::wstring item = script.substr(at, comma == std::wstring::npos ? std::wstring::npos : comma - at);
        size_t mark = item.find(L'@');
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int n = 0, hold = 0;
        if (mark != std::wstring::npos &&
            swscanf_s(item.c_str(), L"MDRAG:%f:%f:%f:%f:%d:%d", &x0, &y0, &x1, &y1, &n, &hold) >= 5 && n > 0) {
            // A drag, as main.cpp spells it out: press, n moves, a pause, release.
            const int f = _wtoi(item.c_str() + mark + 1);
            wchar_t s[64];
            swprintf_s(s, L"MDOWN:%g:%g", x0, y0);
            steps.push_back({f, s});
            for (int k = 1; k <= n; k++) {
                swprintf_s(s, L"MMOVE:%g:%g", x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n);
                steps.push_back({f + k, s});
            }
            swprintf_s(s, L"MUP:%g:%g", x1, y1);
            steps.push_back({f + n + 1 + (hold > 0 ? hold : 0), s});
        } else if (mark != std::wstring::npos) {
            steps.push_back({_wtoi(item.c_str() + mark + 1), item.substr(0, mark)});
        }
        if (comma == std::wstring::npos) break;
        at = comma + 1;
    }
    return steps;
}

static void RunStep(std::wstring what) {
    // "LEFT+" is LEFT arriving as a repeat of a held key.
    bool repeat = !what.empty() && what.back() == L'+';
    if (repeat) what.pop_back();
    ui::heldRepeat = repeat;
    struct ClearRepeat {
        ~ClearRepeat() { ui::heldRepeat = false; }
    } clearRepeat;
    static const struct {
        const wchar_t* name;
        Btn btn;
    } buttons[] = {{L"A", Btn::A},       {L"B", Btn::B},         {L"X", Btn::X},       {L"Y", Btn::Y},
                   {L"UP", Btn::Up},     {L"DOWN", Btn::Down},   {L"LEFT", Btn::Left}, {L"RIGHT", Btn::Right},
                   {L"PLUS", Btn::Plus}, {L"MINUS", Btn::Minus}, {L"L", Btn::L},       {L"R", Btn::R}};
    for (const auto& b : buttons)
        if (what == b.name) {
            ui::OnButton(b.btn);
            return;
        }
    float x = 0, y = 0, d = 0;
    if (swscanf_s(what.c_str(), L"MDOWN:%f:%f", &x, &y) == 2) {
        ui::OnMouse(x, y, false);
        ui::OnMouse(x, y, true);
    } else if (swscanf_s(what.c_str(), L"MMOVE:%f:%f", &x, &y) == 2) {
        ui::OnMouse(x, y, false);
    } else if (swscanf_s(what.c_str(), L"MUP:%f:%f", &x, &y) == 2) {
        ui::OnMouseUp(x, y);
    } else if (swscanf_s(what.c_str(), L"MCLICK:%f:%f", &x, &y) == 2) {
        ui::OnMouse(x, y, false);
        ui::OnMouse(x, y, true);
        ui::OnMouseUp(x, y);
    } else if (swscanf_s(what.c_str(), L"WHEEL:%f", &d) == 1) {
        ui::OnWheel(d);
    } else if (what == L"HOME") {
        ui::OnHomePressed(); // the HOME button, e.g. to come back from a running game
    } else if (what == L"RUN" && !games.empty()) {
        runningId = games[0].id; // a game is running (suspended) without the launch sequence
    } else if (what == L"EXIT" && !runningId.empty()) {
        std::string id = runningId; // the running game ended
        runningId.clear();
        ui::OnGameExit(id, false);
    } else if (ui::WantsText()) {
        // The keyboard while a text box is open: one letter, accept, cancel.
        if (what == L"TYPE") ui::OnChar(L'x');
        else if (what == L"ENTER") ui::OnChar(L'\r');
        else if (what == L"ESC") ui::OnChar(27);
    }
}

// Debug: render an animation offscreen, one PNG per frame at 60 frames a
// second, to lay the menu's motion next to a capture of the console's.
// `script` is a comma-separated list of <step>@<frame>, e.g. "A@0,RIGHT@40,A@90,B@200".
// Steps: A B X Y UP DOWN LEFT RIGHT PLUS MINUS L R, plus HOME, RUN and EXIT, and TYPE, ENTER
// and ESC while a text box is open (see RunStep).
// A direction followed by '+' (LEFT+@24) arrives as a repeat of a held key.
// Mouse steps (layout units): MDOWN:x:y MMOVE:x:y MUP:x:y MCLICK:x:y WHEEL:d and
// MDRAG:x0:y0:x1:y1:n[:hold], as documented at RunShotSeq in main.cpp.
static int RunShotSeq(const std::wstring& screen, const std::wstring& dir, int frames, const std::wstring& script) {
    std::vector<ScriptStep> steps = ParseScript(script);
    if (steps.empty()) steps.push_back({0, L"A"});

    games = ScanLibrary();
    if (!gfx::AttachOffscreen(1920, 1080)) return 2;
    ui::Init();
    ui::DebugGoto(screen);
    int i = 0;
    for (; i < 120; i++) { // let the screen settle first
        ui::Draw(i / 60.0, 1.0f / 60.0f);
        gfx::End();
    }
    for (int f = 0; f < frames; f++, i++) {
        for (const auto& st : steps)
            if (st.frame == f) RunStep(st.what);
        ui::Draw(i / 60.0, 1.0f / 60.0f);
        gfx::End();
        if (dir.empty()) continue; // no folder: just run the frames (soak and timing runs)
        wchar_t name[32];
        swprintf_s(name, L"\\f_%03d.png", f);
        if (!gfx::SaveOffscreenPng(dir + name)) return 3;
    }
    return 0;
}

// --frames: a key from the script, sent through SDL's event queue so it takes
// the same road as a real key press.
static void PushScriptKey(const std::wstring& what) {
    if (what.compare(0, 5, L"TYPE:") == 0) { // TYPE:abc = those letters typed on the keyboard
        for (wchar_t c : what.substr(5)) {
            SDL_Event e;
            SDL_zero(e);
            e.type = SDL_KEYDOWN;
            e.key.windowID = SDL_GetWindowID(window);
            e.key.state = SDL_PRESSED;
            e.key.keysym.sym = static_cast<SDL_Keycode>(towlower(static_cast<wint_t>(c)));
            SDL_PushEvent(&e);
            SDL_zero(e);
            e.type = SDL_TEXTINPUT;
            e.text.windowID = SDL_GetWindowID(window);
            std::string utf8 = port::Utf8(std::wstring(1, c));
            snprintf(e.text.text, sizeof e.text.text, "%s", utf8.c_str());
            SDL_PushEvent(&e);
        }
        return;
    }
    static const struct {
        const wchar_t* name;
        SDL_Keycode sym;
        SDL_Scancode scan;
    } keys[] = {{L"UP", SDLK_UP, SDL_SCANCODE_UP},           {L"DOWN", SDLK_DOWN, SDL_SCANCODE_DOWN},
                {L"LEFT", SDLK_LEFT, SDL_SCANCODE_LEFT},     {L"RIGHT", SDLK_RIGHT, SDL_SCANCODE_RIGHT},
                {L"A", SDLK_RETURN, SDL_SCANCODE_RETURN},    {L"B", SDLK_ESCAPE, SDL_SCANCODE_ESCAPE},
                {L"X", SDLK_x, SDL_SCANCODE_X},              {L"Y", SDLK_y, SDL_SCANCODE_Y},
                {L"PLUS", SDLK_TAB, SDL_SCANCODE_TAB},       {L"MINUS", SDLK_MINUS, SDL_SCANCODE_MINUS},
                {L"L", SDLK_PAGEUP, SDL_SCANCODE_PAGEUP},    {L"R", SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN},
                {L"HOME", SDLK_HOME, SDL_SCANCODE_HOME},     {L"F11", SDLK_F11, SDL_SCANCODE_F11}};
    for (const auto& k : keys) {
        if (what != k.name) continue;
        for (Uint32 type : {static_cast<Uint32>(SDL_KEYDOWN), static_cast<Uint32>(SDL_KEYUP)}) {
            SDL_Event e;
            SDL_zero(e);
            e.type = type;
            e.key.windowID = SDL_GetWindowID(window);
            e.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
            e.key.keysym.sym = k.sym;
            e.key.keysym.scancode = k.scan;
            SDL_PushEvent(&e);
        }
    }
}

static void OnRaiseSignal(int) {
    raiseRequested = 1;
}

// One instance per data folder: a second start asks the first to come to the
// front and leaves. Returns false when this is that second start.
static bool ClaimSingleInstance(int& lockFd) {
    std::string file = port::NativePath(DataDir() + L"\\nswhome.lock");
    lockFd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lockFd < 0) return true; // cannot tell: carry on
    if (flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK) return true; // locks not supported on this file system
        char buf[32] = {};
        ssize_t n = pread(lockFd, buf, sizeof buf - 1, 0);
        long pid = n > 0 ? atol(buf) : 0;
        if (pid > 1) kill(static_cast<pid_t>(pid), SIGUSR1);
        close(lockFd);
        lockFd = -1;
        return false;
    }
    char buf[32];
    int len = snprintf(buf, sizeof buf, "%ld\n", static_cast<long>(getpid()));
    if (ftruncate(lockFd, 0) == 0 && pwrite(lockFd, buf, static_cast<size_t>(len), 0) < 0) {
        // the lock itself is what matters; the pid is only for the wake-up
    }
    return true;
}

static void Usage() {
    puts("NSW-Home\n"
         "  --windowed                 start in a window even if the config says full screen\n"
         "  --background               do not ask for the focus when the window opens\n"
         "  --log                      append diagnostics to <data folder>/debug.log\n"
         "  --no-setup                 do not open the first-start assistant\n"
         "  --shot <screen> <png>      draw one screen offscreen and exit (no window)\n"
         "  --shotseq <screen> <dir> <frames> [script]\n"
         "                             draw an animation offscreen, one PNG per frame\n"
         "  --frames <n> [png] [script]\n"
         "                             run the real window loop for n frames, never starting an\n"
         "                             emulator, then save the window's picture and exit; the\n"
         "                             script (RIGHT@30,DOWN@60) is sent as key presses\n"
         "Environment: NSWHOME_DATA (data folder), NSWHOME_EMUDATA (stand-in for the folder\n"
         "the emulators keep their data in), NSWHOME_DRIVE_ROOT (where X:\\ paths are\n"
         "mounted, default /mnt), NSWHOME_RENDER_HEIGHT (draw no taller than this many pixels).");
}

int main(int argc, char** argv) {
    bool windowed = false, background = false, noSetup = false;
    std::wstring shotScreen, shotFile, seqScreen, seqDir, seqScript, framesFile, framesScript;
    int seqFrames = 0, runFrames = 0;
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; i++) args.push_back(port::Wide(argv[i]));
    for (int i = 1; i < argc; i++) {
        const std::wstring& a = args[static_cast<size_t>(i)];
        auto next = [&](int k) -> const std::wstring& { return args[static_cast<size_t>(i + k)]; };
        if (a == L"--windowed") windowed = true;
        else if (a == L"--background") background = true;
        else if (a == L"--log") logging = true;
        else if (a == L"--no-setup") noSetup = true; // automated runs: straight to the menu
        else if (a == L"--help" || a == L"-h") {
            Usage();
            return 0;
        } else if (a == L"--shotseq" && i + 3 < argc) {
            seqScreen = next(1);
            seqDir = next(2);
            seqFrames = _wtoi(next(3).c_str());
            i += 3;
            // Optional script; anything that is not another option.
            if (i + 1 < argc && next(1).compare(0, 2, L"--") != 0) seqScript = args[static_cast<size_t>(++i)];
        } else if (a == L"--shot" && i + 2 < argc) {
            shotScreen = next(1);
            shotFile = next(2);
            i += 2;
        } else if (a == L"--frames" && i + 1 < argc) {
            runFrames = _wtoi(next(1).c_str());
            i += 1;
            // Optional picture file, then optional script.
            if (i + 1 < argc && next(1).compare(0, 2, L"--") != 0 && next(1).find(L'@') == std::wstring::npos)
                framesFile = args[static_cast<size_t>(++i)];
            if (i + 1 < argc && next(1).compare(0, 2, L"--") != 0) framesScript = args[static_cast<size_t>(++i)];
        }
    }

    if (!gfx::Init()) return 1;
    LoadConfig();
    bool consoleFont = false;
    {
        // Menu text in the console's own typeface when the owner's firmware is
        // installed; the system's sans-serif font otherwise.
        std::wstring fw = FirmwareDir(), keys = KeysDir();
        fwfont::Font font;
        std::string err;
        if (!fw.empty() && !keys.empty() && fwfont::Load(fw, keys, 0, font, err) && gfx::SetFont(std::move(font.data))) {
            // The console font sets wider than the font the layout was first
            // sized with; 0.913 makes the same labels measure the same as on
            // the console ("Opciones" at 21 instead of 23).
            gfx::SetTextScale(0.913f);
            // The console draws the same letter shapes with heavier strokes and
            // unchanged spacing. Matching the amount of ink in the capture's
            // header and footer text gives about 1 % of the text size.
            gfx::SetTextEmbolden(0.0105f);
            fwfont::Font symbols;
            if (fwfont::Load(fw, keys, 1, symbols, err)) gfx::SetSymbolFont(std::move(symbols.data));
            consoleFont = true;
        } else if (!err.empty()) {
            Log("console font not used: %s", err.c_str());
        }
    }
    // A --frames run never starts an emulator either, unless the test asks
    // for the real launch path (it then points the config at a stand-in program).
    simulate = seqFrames > 0 || !shotFile.empty() || (runFrames > 0 && !getenv("NSWHOME_TEST_SPAWN") &&
                                                              !getenv("DOCKSIDE_TEST_SPAWN")); // historical name
    offscreen = seqFrames > 0 || !shotFile.empty();
    if (seqFrames > 0) {
        int rc = RunShotSeq(seqScreen, seqDir, seqFrames, seqScript);
        gfx::Shutdown();
        return rc;
    }
    if (!shotFile.empty()) {
        int rc = RunShot(shotScreen, shotFile);
        gfx::Shutdown();
        return rc;
    }

    // One instance only: a second start just brings the first to the front.
    int lockFd = -1;
    signal(SIGUSR1, OnRaiseSignal);
    if (!ClaimSingleInstance(lockFd)) {
        gfx::Shutdown();
        return 0;
    }
    if (!simulate) DetectNativeEmulators();

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // the Guide button must reach us behind a game
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");     // a full-screen menu stays put when the game takes the focus
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (background) SDL_SetHint("SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN", "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "NSW-Home: cannot start video: %s\n", SDL_GetError());
        gfx::Shutdown();
        return 1;
    }
    scanDoneEvent = SDL_RegisterEvents(1);
    const char* driver = SDL_GetCurrentVideoDriver();
    canUnminimise = !driver || strcmp(driver, "wayland") != 0;
    forceForeground = runFrames > 0;

    Display home = TargetDisplay();
    window = SDL_CreateWindow("NSW-Home", SDL_WINDOWPOS_CENTERED_DISPLAY(home.index),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(home.index), 1280, 720,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    ApplyTitle(); // the name chosen in Settings, when there is one
    if (window) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!window || !renderer || !gfx::AttachWindow(window)) {
        fprintf(stderr, "NSW-Home: cannot open the window: %s\n", SDL_GetError());
        SDL_Quit();
        gfx::Shutdown();
        return 1;
    }
    SDL_RendererInfo info;
    SDL_zero(info);
    SDL_GetRendererInfo(renderer, &info);
    const bool vsync = (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    Log("video %s, renderer %s%s, text in %s", driver ? driver : "?", info.name ? info.name : "?", vsync ? " (vsync)" : "",
        gfx::FontDescription().c_str());

    InitSounds();
    InitAudio();
    ui::Init();
    if (!noSetup) ui::FirstStart();
    SDL_ShowWindow(window);
    if (cfg.fullscreen && !windowed) ApplyFullscreen(true);
    FitToWindow();
    RescanAsync();

    std::vector<ScriptStep> script = ParseScript(framesScript);
    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now(), last = start;
    int frames = 0;
    double drawMs = 0, worstMs = 0;
    bool cursorShown = true;

    while (!quit) {
        const Uint32 flags = SDL_GetWindowFlags(window);
        bool foreground = windowVisible && MenuFocused();
        bool render = windowVisible && (forceForeground || !(flags & SDL_WINDOW_MINIMIZED)) &&
                      (foreground || ui::Animating() || redrawOnce);
        // An active menu is paced by the display. Hidden behind a game it
        // wakes 10 times a second for the Guide button and the emulator;
        // otherwise it idles until something happens.
        if (!render) SDL_WaitEventTimeout(nullptr, gamePid ? 100 : 250);

        SDL_Event e;
        while (SDL_PollEvent(&e)) HandleEvent(e);
        if (raiseRequested) { // a second start of the program
            raiseRequested = 0;
            if (gamePid && menuHiddenForGame && !windowVisible) HomeFromGame();
            else ShowMenu();
        }
        PollPads();
        TrackGame();

        if (fullscreen) {
            // Keep the pointer out of the way until the mouse is actually used.
            bool want = ui::MouseActive();
            if (want != cursorShown) SDL_ShowCursor(want ? SDL_ENABLE : SDL_DISABLE);
            cursorShown = want;
        } else if (!cursorShown) {
            SDL_ShowCursor(SDL_ENABLE);
            cursorShown = true;
        }

        Clock::time_point now = Clock::now();
        if (render) {
            redrawOnce = false;
            for (const auto& st : script)
                if (st.frame == frames) PushScriptKey(st.what);
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            FitToWindow();
            ui::Draw(std::chrono::duration<double>(now - start).count(), (std::min)(dt, 0.1f));
            gfx::End();
            double ms = std::chrono::duration<double, std::milli>(Clock::now() - now).count();
            drawMs += ms;
            worstMs = (std::max)(worstMs, ms);
            Present();
            frames++;
            if (runFrames > 0 && frames >= runFrames) quit = true;
            // Presenting normally waits for the display. Where it does not (no
            // vsync, or a driver that ignores it) the frame comes back at once:
            // hold the loop to 60 frames a second instead of spinning.
            double spent = std::chrono::duration<double, std::milli>(Clock::now() - now).count();
            if (!vsync || spent < 5.0) {
                if (spent < 16.0) SDL_Delay(static_cast<Uint32>(16.0 - spent));
            }
        } else {
            last = now;
        }
    }

    if (runFrames > 0) {
        int w = 0, h = 0, stride = 0;
        gfx::FramePixels(w, h, stride);
        bool saved = framesFile.empty() || gfx::SaveOffscreenPng(framesFile);
        printf("frames %d, picture %dx%d, draw %.2f ms average, %.2f ms worst, video %s, renderer %s%s, audio %s, "
               "font %s%s, games %zu, pads %d%s\n",
               frames, w, h, frames ? drawMs / frames : 0.0, worstMs, driver ? driver : "?", info.name ? info.name : "?",
               vsync ? " (vsync)" : "", audioDev ? SDL_GetCurrentAudioDriver() : "none", gfx::FontDescription().c_str(),
               consoleFont ? " (console font)" : "", games.size(),
               static_cast<int>(padConnected[0] + padConnected[1] + padConnected[2] + padConnected[3]),
               saved ? "" : ", PICTURE NOT SAVED");
    }

    // Turning the console off ends the game too.
    if (gamePid) {
        kill(-gamePid, SIGTERM);
        bool gone = false;
        for (int i = 0; i < 30 && !gone; i++) {
            int status = 0;
            gone = waitpid(gamePid, &status, WNOHANG) != 0;
            if (!gone) SDL_Delay(100);
        }
        kill(-gamePid, SIGKILL); // whatever is left of its group
        if (!gone) {
            int status = 0;
            while (waitpid(gamePid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    SDL_HideWindow(window);
    if (scanThread.joinable()) scanThread.join(); // a scan in flight still reads the config
    if (scanning) {
        // Its result is in the event queue and will not be used.
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == scanDoneEvent) delete static_cast<std::vector<Game>*>(e.user.data1);
    }
    for (SDL_GameController*& c : sdlPads)
        if (c) SDL_GameControllerClose(c), c = nullptr;
    if (audioDev) SDL_CloseAudioDevice(audioDev);
    if (frameTex) SDL_DestroyTexture(frameTex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    friends::Shutdown(); // a user who shares says goodbye to their friends (a few seconds at most), the window already gone
    SDL_Quit();
    gfx::Shutdown();
    if (lockFd >= 0) close(lockFd);
    return 0;
}
