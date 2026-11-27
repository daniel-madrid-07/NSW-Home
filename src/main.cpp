// Window, main loop, input, sound and emulator process control.
#include "app.h"
#include "a11y.h"
#include "amiibo.h"
#include "pcsys.h"
#include "friends.h"
#include "fwfont.h"
#include "fwsound.h"
#include "wincap.h"
#include "ryucfg.h"
#include "update.h"
#include "webview.h"
#include <mmsystem.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <uiautomation.h>
#include <wincodec.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <map>
#include <memory>
#include <thread>

#pragma comment(lib, "advapi32.lib") // OpenObs reads where OBS Studio is installed
#pragma comment(lib, "version.lib") // ProgramInfo reads a program's version block

// For the game viewer (thumbnail, cloak; BSTR of the accessibility interface).
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "oleaut32.lib")

std::vector<Game> games;
std::string runningId;
bool scanning = false;
bool ui::heldRepeat = false;

namespace {

constexpr UINT WM_SCAN_DONE = WM_APP + 1;

// --log: append launch diagnostics to %APPDATA%\NSW-Home\debug.log.
bool simulate = false; // offscreen rendering: no window, no emulator
bool logging = false;
void Log(const wchar_t* fmt, ...) {
    if (!logging) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (DataDir() + L"\\debug.log").c_str(), L"a, ccs=UTF-8") || !f) return;
    fwprintf(f, L"%8llu  ", GetTickCount64() % 100000000ULL);
    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);
    fputwc(L'\n', f);
    fclose(f);
}
constexpr int HOTKEY_HOME = 1;
constexpr int HOTKEY_CAPTURE = 2;
constexpr UINT WM_CAPTURE_DONE = WM_APP + 20; // wParam: saved or not; lParam: how the pixels were read (a string literal)

HWND hwnd = nullptr;
bool quit = false;
bool fullscreen = false;
bool windowVisible = true;
WINDOWPLACEMENT windowedPlacement = {sizeof(WINDOWPLACEMENT)};

// Running emulator.
HANDLE gameProc = nullptr;
HANDLE gameJob = nullptr;
DWORD gamePid = 0;
DWORD gamePidForFocus() { return gameProc ? gamePid : 0; }
ULONGLONG gameStart = 0;
ULONGLONG gameWindowSeen = 0; // tick when the emulator window first showed up
ULONGLONG gameReadyAt = 0;    // tick when the game was first seen drawing (the loading mark is fading out)
ULONGLONG renderSince = 0;    // tick since which the emulator's render surface has been on screen without a break
bool menuHiddenForGame = false; // the game has been shown; the launch screen is done
const wchar_t* gameReadyChild = nullptr;
HWND gameHwnd = nullptr;    // the emulator's main window, once found
bool gamePc = false;        // what is running is a PC game, not an emulator (see "PC games" below)
HWND PcWindow(bool hidden);
// Invisible window that owns the emulator's windows. An owned window gets no
// taskbar button and no Alt+Tab entry, and unlike hiding, changing the owner
// takes effect at once even while the emulator is too busy to answer messages.
HWND ownerWnd = nullptr;

// One of the emulator's own top-level windows (as opposed to a dialog it opened).
bool IsEmulatorTopLevel(HWND h) {
    HWND owner = GetWindow(h, GW_OWNER);
    return !owner || owner == ownerWnd;
}

// --log: says when a call into another program's window held the menu up.
struct Slow {
    const wchar_t* what;
    ULONGLONG t = GetTickCount64();
    ~Slow() {
        if (GetTickCount64() - t > 100) Log(L"slow: %s took %llu ms", what, GetTickCount64() - t);
    }
};

void AdoptGameWindow(HWND h) {
    if (!ownerWnd || GetWindow(h, GW_OWNER)) return;
    Slow slow{L"taking ownership of the emulator window"};
    SetWindowLongPtrW(h, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerWnd));
}
ITaskbarList* taskbar = nullptr;
ULONGLONG gameGoneSince = 0;  // the game picture has been missing since this tick

// ---- sound ----------------------------------------------------------------

struct Note {
    float freq, start, len, vol;
};
std::vector<uint8_t> sfx[static_cast<int>(Sfx::Count)];

std::vector<uint8_t> Synth(const std::vector<Note>& notes) {
    const int rate = 44100;
    float total = 0;
    for (const auto& n : notes) total = (std::max)(total, n.start + n.len);
    int count = static_cast<int>(total * rate);
    std::vector<float> mixBuf(count, 0.0f);
    for (const auto& n : notes) {
        int s0 = static_cast<int>(n.start * rate), len = static_cast<int>(n.len * rate);
        for (int i = 0; i < len && s0 + i < count; i++) {
            float t = static_cast<float>(i) / rate;
            // Fast attack, exponential decay: a soft "blip" rather than a beep.
            float env = (std::min)(1.0f, t / 0.004f) * expf(-t * 5.0f / n.len);
            mixBuf[s0 + i] += sinf(6.2831853f * n.freq * t) * env * n.vol;
        }
    }
    std::vector<uint8_t> wav(44 + count * 2);
    auto put32 = [&](int off, uint32_t v) { memcpy(&wav[off], &v, 4); };
    auto put16 = [&](int off, uint16_t v) { memcpy(&wav[off], &v, 2); };
    memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + count * 2);
    memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&wav[36], "data", 4);
    put32(40, count * 2);
    for (int i = 0; i < count; i++) {
        float v = (std::max)(-1.0f, (std::min)(1.0f, mixBuf[i]));
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
    // The capture sound: two short high clicks, a shutter in the other sounds'
    // voice. It has no firmware counterpart below: the HOME Menu's archive
    // (153 sounds, none for a capture) is not where the console keeps it.
    sfx[static_cast<int>(Sfx::Capture)] = Synth({{2300, 0, 0.028f, 0.26f}, {1750, 0.075f, 0.05f, 0.24f}});

    // With the owner's console firmware installed, use the menu's own sounds.
    // They are decoded from that firmware at start-up and never stored here.
    std::wstring fw = FirmwareDir(), keys = KeysDir();
    if (fw.empty() || keys.empty()) return;
    std::vector<fwsound::Sound> real;
    std::string err;
    if (!fwsound::LoadMenuSounds(fw, keys, real, err)) return;
    static const struct {
        Sfx event;
        const char* name;
    } map[] = {
        {Sfx::Move, "SeBtnFocus"},          {Sfx::Ok, "SeBtnDecide"},          {Sfx::Back, "SeFooterDecideBack"},
        {Sfx::Bump, "SeGameIconLimit"},     {Sfx::Launch, "SeGameIconDecide"}, {Sfx::Error, "SeKeyError"},
        {Sfx::Home, "StartupMenu_Game"},    {Sfx::MoveGame, "SeGameIconFocus"}, {Sfx::Resume, "SeGameIconDecideSupend"},
        {Sfx::Dialog, "SeDialogOpen"},      {Sfx::UserMove, "SeAccountFocus"}, {Sfx::UserPick, "SeAccountDecide"},
        {Sfx::Settings, "StartupSet"},      {Sfx::Album, "StartupAlbum"},      {Sfx::Pads, "StartupController"},
        {Sfx::UserPage, "StartupMypage"},   {Sfx::Sleep, "StartupSleep"},
    };
    for (const auto& m : map)
        for (auto& snd : real)
            if (snd.name == m.name && !snd.wav.empty()) sfx[static_cast<int>(m.event)] = snd.wav;
}

// ---- window ---------------------------------------------------------------

DWORD gamePidForFocus(); // defined with the emulator state below

// NSW-Home (menu plus the game it started) counts as one application.
bool OursHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() || (pid && pid == gamePidForFocus());
}

// Focus moves between the menu and the game only while one of them already
// has it. If the person is working in another program, nothing is taken from
// them: the window is shown without being activated. `userAsked` is for the
// HOME key and a second start of the app, where pressing it is the request.
void GiveFocus(HWND h, bool userAsked = false) {
    if (!h) return;
    if (userAsked || OursHasFocus()) {
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
    } else {
        ShowWindow(h, SW_SHOWNA);
    }
}

HWND GameWindow();
void HideGameWindow(HWND game);
// The game viewer (emulators only; see "game viewer" below).
extern bool viewing;
void ViewerShowMenu(bool userAsked);
void ViewerOnShow(HWND h);
void ViewerGameEnded();

void ShowMenu(bool userAsked = false) {
    if (viewing) {
        ViewerShowMenu(userAsked);
        return;
    }
    windowVisible = true;
    // Decide before hiding the game, which is what holds the focus now.
    bool take = userAsked || OursHasFocus();
    if (gamePid && gameProc) HideGameWindow(GameWindow());
    GiveFocus(hwnd, take);
}

BOOL CALLBACK MonitorProc(HMONITOR, HDC, LPRECT rc, LPARAM lp) {
    reinterpret_cast<std::vector<RECT>*>(lp)->push_back(*rc);
    return TRUE;
}

// All monitors, ordered left to right (then top to bottom).
std::vector<RECT> Monitors() {
    std::vector<RECT> out;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&out));
    std::sort(out.begin(), out.end(), [](const RECT& a, const RECT& b) {
        return a.left != b.left ? a.left < b.left : a.top < b.top;
    });
    return out;
}

// The monitor the menu (and the game) should be on.
RECT TargetMonitor() {
    std::vector<RECT> all = Monitors();
    if (cfg.monitor >= 0 && cfg.monitor < static_cast<int>(all.size())) return all[cfg.monitor];
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

struct QtMonitor {
    std::wstring device;
    RECT rc;
    bool primary;
    HMONITOR handle;
};
BOOL CALLBACK QtMonitorProc(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(h, &mi))
        reinterpret_cast<std::vector<QtMonitor>*>(lp)->push_back(
            {mi.szDevice, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, h});
    return TRUE;
}

// The number Qt gives the screen `m` and that screen's dots per inch. Qt
// lists the main screen first and the others in the order Windows lists
// them (tried on this three-screen PC: with the other candidate order Eden
// refused the saved place).
void QtScreen(const RECT& m, int& number, int& dpi) {
    std::vector<QtMonitor> all;
    EnumDisplayMonitors(nullptr, nullptr, QtMonitorProc, reinterpret_cast<LPARAM>(&all));
    std::stable_sort(all.begin(), all.end(), [](const QtMonitor& a, const QtMonitor& b) { return a.primary && !b.primary; });
    for (size_t i = 0; i < all.size(); i++) {
        if (!EqualRect(&all[i].rc, &m)) continue;
        number = static_cast<int>(i);
        // GetDpiForMonitor lives in a library of its own; asked for by name
        // so the program still starts where it is missing.
        using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        HMODULE lib = LoadLibraryW(L"shcore.dll");
        Fn fn = lib ? reinterpret_cast<Fn>(GetProcAddress(lib, "GetDpiForMonitor")) : nullptr;
        UINT dx = 96, dy = 96;
        if (fn && SUCCEEDED(fn(all[i].handle, 0, &dx, &dy)) && dx) dpi = static_cast<int>(dx);
    }
}

void CenterWindowed(const RECT& m) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(hwnd, nullptr, m.left + (m.right - m.left - w) / 2, m.top + (m.bottom - m.top - h) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ApplyFullscreen(bool on) {
    if (on == fullscreen) return;
    fullscreen = on;
    if (on) {
        GetWindowPlacement(hwnd, &windowedPlacement);
        RECT m = TargetMonitor();
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &windowedPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

// ---- emulator process -----------------------------------------------------

struct FindCtx {
    DWORD pid;
    HWND found;
};

// Big enough to be a main window rather than a helper or message window.
bool LooksLikeMainWindow(HWND h) {
    if (!IsEmulatorTopLevel(h)) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    // Ryujinx also owns a log console; that is not the game.
    if (!wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"PseudoConsoleWindow")) return false;
    RECT rc;
    return GetWindowRect(h, &rc) && rc.right - rc.left >= 320 && rc.bottom - rc.top >= 240;
}

bool InPcGame(DWORD pid); // with the PC games, below

BOOL CALLBACK FindWindowProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    // Visibility is not required: we hide the window ourselves as soon as it
    // appears, so only ever having seen it shown would mean never finding it.
    // It must have been shown at least once though (WS_CAPTION-less helper
    // windows that were never shown have no taskbar presence to care about).
    if (!(pid == ctx->pid || InPcGame(pid)) || !LooksLikeMainWindow(h)) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    ctx->found = h;
    return FALSE;
}

// The emulator's main window. It is looked up while visible and remembered,
// because from then on it spends most of its time hidden by us.
HWND GameWindow() {
    if (gameHwnd && IsWindow(gameHwnd)) return gameHwnd;
    FindCtx ctx = {gamePid, nullptr};
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&ctx));
    gameHwnd = ctx.found;
    // A PC game may have replaced the window that was remembered while the
    // menu had it hidden: then the one to show again is not visible.
    if (!gameHwnd && gamePc && menuHiddenForGame) gameHwnd = PcWindow(true);
    return gameHwnd;
}

// To the desktop there is only NSW-Home: the emulator gets no taskbar button
// and no Alt+Tab entry. Hidden windows have neither, and once the game is on
// screen its window is owned by ours, which has the same effect.
void HideGameWindow(HWND game) {
    if (!game) return;
    AdoptGameWindow(game);
    // Removing the taskbar button is a request to the shell, so it works
    // even while the emulator is too busy loading to answer its own window
    // messages. Hiding is posted, never sent: a sent message would freeze the
    // menu until the emulator got round to it.
    if (taskbar) taskbar->DeleteTab(game);
    if (IsWindowVisible(game)) ShowWindowAsync(game, SW_HIDE);
}

BOOL CALLBACK HideAllProc(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    // Owned windows are the emulator's dialogs; those are left alone so an
    // error can still be noticed and shown.
    if ((pid == static_cast<DWORD>(lp) || InPcGame(pid)) && IsWindowVisible(h) && IsEmulatorTopLevel(h)) HideGameWindow(h);
    return TRUE;
}

// Every top-level window the emulator has up, not only the main one.
void HideAllGameWindows() {
    EnumWindows(HideAllProc, static_cast<LPARAM>(gamePid));
}

// While a game starts, its window is left shown but out of sight: it sits
// under the launch screen and, being owned, has no taskbar button. It is not
// hidden outright, because the emulator only reveals its render surface (our
// "the game is drawing" signal) inside a visible window.
void ConcealGameWindow(HWND h) {
    Slow slow{L"removing the emulator's taskbar button"};
    // The button first: that is a request to the shell and returns at once,
    // while taking ownership can wait several hundred milliseconds on an
    // emulator that is busy starting (and the button would show meanwhile).
    if (taskbar) taskbar->DeleteTab(h);
    AdoptGameWindow(h);
    if (taskbar) taskbar->DeleteTab(h);
}

BOOL CALLBACK ConcealAllProc(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if ((pid == static_cast<DWORD>(lp) || InPcGame(pid)) && IsWindowVisible(h) && IsEmulatorTopLevel(h)) ConcealGameWindow(h);
    return TRUE;
}

void ConcealAllGameWindows() {
    EnumWindows(ConcealAllProc, static_cast<LPARAM>(gamePid));
}

bool GameMayShow() {
    return menuHiddenForGame && !windowVisible;
}

// Emulators reopen wherever they were last (the yuzu family, which never gets
// to save its place because it is ended rather than closed, on the main
// screen); bring the game to the menu's screen so it feels like one console
// and its window is under the launch screen.
std::atomic<bool> placing{false};

void PlaceOnMenuScreen(HWND game) {
    RECT m, cur;
    if (!game || !GetWindowRect(game, &cur)) return;
    m = TargetMonitor();
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &mi);
    const bool filled = cur.right - cur.left >= mi.rcMonitor.right - mi.rcMonitor.left;
    int x = m.left + 40, y = m.top + 40, w = 0, h = 0;
    UINT flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
    if (!EqualRect(&mi.rcMonitor, &m)) {
        if (filled) {
            x = m.left, y = m.top, w = m.right - m.left, h = m.bottom - m.top;
            flags &= ~static_cast<UINT>(SWP_NOSIZE);
        }
    } else if (filled && (cur.left != m.left || cur.top != m.top)) {
        // It went full screen while that move was still on its way and kept
        // the windowed position: a full-screen picture shifted by 40 pixels.
        x = m.left, y = m.top;
    } else if (!(GetWindowLongW(game, GWL_STYLE) & WS_CAPTION) && cur.right - cur.left >= 320 &&
               (cur.right - cur.left < m.right - m.left || cur.bottom - cur.top < m.bottom - m.top)) {
        // A window without a frame is the emulator's full-screen mode. Suyu
        // sizes it for the screen it started on; moved to a screen with
        // another scale it ends up smaller than the screen it is on.
        x = m.left, y = m.top, w = m.right - m.left, h = m.bottom - m.top;
        flags &= ~static_cast<UINT>(SWP_NOSIZE);
    } else {
        return;
    }
    // Moving a window across screens waits for its program even when asked
    // not to (measured: one to two seconds with the yuzu family, which is
    // busy starting the game). On a thread of its own, so the menu keeps
    // drawing; one request at a time.
    if (placing.exchange(true)) return;
    std::thread([=] {
        SetWindowPos(game, nullptr, x, y, w, h, flags);
        placing = false;
    }).detach();
}

// Called by Windows the moment the emulator shows a window, so it can be
// hidden (or adopted) before it has a chance to reach the taskbar.
HWINEVENTHOOK showHook = nullptr;
void ShowGameWindow(HWND game);

void CALLBACK OnEmulatorShow(HWINEVENTHOOK, DWORD, HWND h, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || !h || !gameProc) return;
    if (GetAncestor(h, GA_ROOT) != h || !IsEmulatorTopLevel(h)) return;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"ConsoleWindowClass")) return;
    if (viewing) {
        ViewerOnShow(h);
        return;
    }
    // (Adopting the window earlier, when it is created, was tried so that the
    // taskbar would never list it: the call then waits on the emulator while it
    // is busiest, the menu froze for up to 84 s in two starts out of five, and
    // the momentary taskbar button still appeared. Do not move this.)
    if (taskbar && !GameMayShow()) taskbar->DeleteTab(h); // at once, before the slower call below
    AdoptGameWindow(h);
    // The first proper window the emulator shows is the game window.
    if (!gameHwnd && LooksLikeMainWindow(h)) gameHwnd = h;
    if (GameMayShow()) {
        if (h == gameHwnd) ShowGameWindow(h); // re-shown by the emulator: keep it ours
    } else if (menuHiddenForGame) {
        HideGameWindow(h); // the menu is up over a running game
    } else {
        ConcealGameWindow(h); // still starting, under the launch screen
        if (!gamePc) PlaceOnMenuScreen(h); // (a splash window as much as the main one)
    }
}

void ShowGameWindow(HWND game) {
    if (!game) return;
    AdoptGameWindow(game);
    // SW_SHOWNA when someone else has the focus, so appearing never steals it.
    ShowWindowAsync(game, OursHasFocus() ? SW_SHOW : SW_SHOWNA);
    if (taskbar) taskbar->DeleteTab(game);
}

// Alt+F4 ends NSW-Home, game included. Windows will not hand Alt+F4 out as a
// hot key, so while a game has the keyboard the emulator receives it and
// closes: the main loop looks at the keys itself, and a game that ends with
// Alt+F4 just pressed ends NSW-Home instead of reading as a failure.
static ULONGLONG altF4At = 0;
static bool AltF4Seen() {
    const bool now = (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F4) & 0x8001);
    if (now) altF4At = GetTickCount64();
    return now || (altF4At && GetTickCount64() - altF4At < 3000);
}

// A start that died before the game drew is tried again (see OnGameExited).
static int launchTries = 0;      // how many times the game now starting has been started again
static bool relaunching = false; // LaunchGame is being called for such a new try
static bool closeAsked = false;  // the end of the running program was asked for from the menu

bool InPcGame(DWORD pid); // with the PC games, below

// The started program of a PC game ended while another process of its job is
// still running (a launcher that has started the real game): that one is the
// game from now on. True when the game goes on.
static bool FollowLaunchedGame() {
    if (!gamePc || !gameJob) return false;
    if (closeAsked) {
        // Closing was asked for: whatever the launcher left running goes with it.
        TerminateJobObject(gameJob, 0);
        return false;
    }
    struct {
        JOBOBJECT_BASIC_PROCESS_ID_LIST head;
        ULONG_PTR more[63];
    } list = {};
    list.head.NumberOfAssignedProcesses = 64;
    if (!QueryInformationJobObject(gameJob, JobObjectBasicProcessIdList, &list, sizeof list, nullptr)) return false;
    for (DWORD i = 0; i < list.head.NumberOfProcessIdsInList; i++) {
        const DWORD pid = static_cast<DWORD>(list.head.ProcessIdList[i]);
        if (pid == gamePid) continue;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
        if (!h) continue;
        if (WaitForSingleObject(h, 0) != WAIT_TIMEOUT) {
            CloseHandle(h);
            continue;
        }
        Log(L"the program started (%lu) has ended; following the game it started (%lu)", gamePid, pid);
        CloseHandle(gameProc);
        gameProc = h;
        gamePid = pid;
        pcsys::GameStarted(gamePid);
        if (showHook) UnhookWinEvent(showHook);
        showHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnEmulatorShow, gamePid, 0,
                                   WINEVENT_OUTOFCONTEXT);
        return true;
    }
    return false;
}

void OnGameExited() {
    if (FollowLaunchedGame()) return;
    ULONGLONG elapsed = GetTickCount64() - gameStart;
    Log(L"game exited after %llu ms", elapsed);
    const bool wasPc = gamePc;
    CloseHandle(gameProc);
    if (gameJob) CloseHandle(gameJob);
    gameProc = gameJob = nullptr;
    gameHwnd = nullptr;
    gamePc = false;
    if (showHook) UnhookWinEvent(showHook);
    showHook = nullptr;
    UnregisterHotKey(hwnd, HOTKEY_HOME);
    ViewerGameEnded();

    // An emulator that dies while it is still starting is started again behind
    // the launch screen, so that the player only sees a longer load. (Citron
    // 0.6.1 does it in about half of its starts of one game here, 6.5 s in,
    // always at the same place of its own log: "Invalid mapping length" from
    // its memory code while the game boots. Measured over 14 starts; with five
    // tries that leaves about one launch in thirty failing.) Not when the end
    // was asked for, not once the game has drawn, and not for a PC program,
    // which may simply have handed over to another one.
    constexpr int kTries = 5;
    if (!wasPc && !gameReadyAt && !menuHiddenForGame && !closeAsked && elapsed < 20000 && launchTries < kTries - 1) {
        launchTries++;
        Log(L"ended before the game drew: starting it again (try %d of %d)", launchTries + 1, kTries);
        const std::string again = runningId;
        runningId.clear();
        relaunching = true;
        const std::string err = LaunchGame(again);
        relaunching = false;
        if (err.empty()) return;
        Log(L"could not start it again (%hs)", err.c_str());
        runningId = again; // the ordinary end, below
    }

    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    std::string id = runningId;
    runningId.clear();
    Played& p = cfg.played[CurUser().id + "/" + id];
    p.last = NowIso();
    p.seconds += elapsed / 1000;
    SaveConfig();
    if (AltF4Seen()) {
        Log(L"the game ended with Alt+F4: ending NSW-Home");
        quit = true;
        return;
    }

    ShowMenu();
    ui::OnGameExit(id, elapsed < 4000);
    RescanAsync();
}

struct ChildCtx {
    const wchar_t* prefix;
    bool found;
    HWND top;
    bool deep;
    HWND hit = nullptr; // the window that matched
};

BOOL CALLBACK ChildProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<ChildCtx*>(lp);
    wchar_t cls[96] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (!wcsncmp(cls, ctx->prefix, wcslen(ctx->prefix)) && IsWindowVisible(h)) {
        if (ctx->deep) {
            // The surface: a window with no children, alone in its parent (the
            // render window), three levels down or more...
            HWND p = GetAncestor(h, GA_PARENT), pp = p ? GetAncestor(p, GA_PARENT) : nullptr;
            if (GetWindow(h, GW_CHILD) || !p || p == ctx->top || !pp || pp == ctx->top) return TRUE;
            if (GetWindow(h, GW_HWNDNEXT) || GetWindow(h, GW_HWNDPREV)) return TRUE;
            // ... and nothing beside the render window is showing: its
            // neighbours are the loading screen and the game list. (Eden only
            // shows the render window when the loading screen has gone; Suyu
            // shows it from the start, under the loading screen.)
            int beside = 0;
            for (HWND q = GetWindow(p, GW_HWNDFIRST); q; q = GetWindow(q, GW_HWNDNEXT)) {
                if (q == p) continue;
                if (IsWindowVisible(q)) return TRUE;
                beside++;
            }
            if (beside < 2) return TRUE;
        }
        ctx->found = true;
        ctx->hit = h;
        return FALSE;
    }
    return TRUE;
}

// The emulator's render surface (the child window the game's picture is drawn
// in) while it is on screen, i.e. while the game itself is drawing.
HWND RenderSurface(HWND game) {
    if (!game || !gameReadyChild) return nullptr;
    // Ryujinx creates a window of a class of its own for the game's picture.
    // The yuzu family's windows all share Qt's one class; there the picture
    // is recognised by where it sits (see ChildProc), which a prefix starting
    // with '>' asks for.
    const bool deep = gameReadyChild[0] == L'>';
    ChildCtx ctx = {gameReadyChild + (deep ? 1 : 0), false, game, deep, nullptr};
    EnumChildWindows(game, ChildProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found ? ctx.hit : nullptr;
}

bool GameRendering(HWND game) {
    return RenderSurface(game) != nullptr;
}

// ---- game viewer ----------------------------------------------------------
//
// With an emulator, what the player looks at is always NSW-Home's own window.
// The emulator's window is never on screen and cannot be reached:
//   - it is owned by `ownerWnd`, and that window is cloaked while an emulator
//     runs. An owned window inherits the cloak, so Windows composes none of
//     the emulator's windows (main window, dialogs) onto the screen and sends
//     them no mouse input, whatever state they are in;
//   - it is kept under NSW-Home's window and inside the picture's rectangle
//     anyway, for the moment between a window appearing and its adoption;
//   - the game's picture is a DWM thumbnail of the emulator's window, cropped
//     to its render surface and drawn by the system inside NSW-Home's window.
// The emulator still holds the keyboard focus while a game is showing, since
// both families read the keyboard only as the focused window (pads are read
// regardless). The keys that would change its window or stop emulation never
// reach it: they are registered as hotkeys of ours for as long as it has the
// focus.
// Settled with tools\viewer_probe before it was built: the thumbnail of a
// covered, cloaked window is live at the screen's full rate with a Vulkan and
// with an OpenGL swap chain, the copy is exact when the sizes match, and the
// source is not slowed down.

bool viewing = false; // the running program is an emulator shown through the viewer
char viewFamily = 0;  // EmuDef::family of that emulator
HTHUMBNAIL viewThumb = nullptr;
HWND viewSource = nullptr;        // the window the thumbnail is of
DWM_THUMBNAIL_PROPERTIES viewProps = {}; // as last given to the system
bool viewFresh = false;           // just registered: nothing given yet
bool viewOn = false;              // the picture should be showing
float viewOpacity = 0;            // where its fade is, 0 to 1
double viewTick = 0;
bool viewBlank = false;           // our own window is black under the picture
HWINEVENTHOOK layoutHook = nullptr;
bool keysBlocked = false;
ULONGLONG fullKeyAt = 0;          // when the emulator is next asked to go back to full screen
int fullKeyTries = 0;             // how often it has been asked without staying there
// The console cuts between the loading screen, the game and the menu; a short
// fade hides that the emulator's picture and ours are not swapped in the same
// screen refresh.
constexpr double kViewFadeMs = 120;
constexpr int HOTKEY_BLOCK = 100;
constexpr UINT WM_EMU_DIALOG = WM_APP + 40;

double NowMs(); // with the input code below
void ApplyFullscreen(bool on);

bool GameShowing() {
    return viewing && gameProc && menuHiddenForGame && !windowVisible;
}

// The 16:9 area of our window that the menu is drawn in (gfx letterboxes the
// same way), in client pixels.
RECT PictureRect() {
    RECT c;
    GetClientRect(hwnd, &c);
    const float s = (std::min)(c.right / gfx::W, c.bottom / gfx::H);
    const LONG w = std::lround(gfx::W * s), h = std::lround(gfx::H * s);
    const LONG x = (c.right - w) / 2, y = (c.bottom - h) / 2;
    return {x, y, x + w, y + h};
}

void ViewShow(bool on) {
    viewOn = on;
    if (!on) viewBlank = false; // the menu draws again
}

bool ViewFading() {
    return viewThumb && viewOpacity != (viewOn ? 1.0f : 0.0f);
}

// Once per pass of the main loop: the thumbnail follows the render surface
// (which moves inside the emulator's window when that shows or hides its own
// bars) and our own picture area, and fades.
void ViewerUpdate() {
    const double t = NowMs();
    const double dt = viewTick ? t - viewTick : 0;
    viewTick = t;
    HWND game = viewing && gameProc && gameHwnd && IsWindow(gameHwnd) ? gameHwnd : nullptr;
    if (viewThumb && game != viewSource) {
        DwmUnregisterThumbnail(viewThumb);
        viewThumb = nullptr;
    }
    if (!game) {
        viewOpacity = 0;
        return;
    }
    if (!viewThumb) {
        HRESULT hr = DwmRegisterThumbnail(hwnd, game, &viewThumb);
        if (FAILED(hr)) {
            Log(L"viewer: no thumbnail of the emulator window (%08lx)", hr);
            viewThumb = nullptr;
            return;
        }
        viewSource = game;
        viewFresh = true;
    }
    const float step = static_cast<float>(dt / kViewFadeMs);
    if (viewOn) viewOpacity = (std::min)(1.0f, viewOpacity + step);
    else viewOpacity = (std::max)(0.0f, viewOpacity - step);

    DWM_THUMBNAIL_PROPERTIES p = {};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
    HWND surface = viewOpacity > 0 && !IsIconic(game) ? RenderSurface(game) : nullptr;
    RECT top = {}, s = {};
    // Only while the surface fills the emulator's window (its full-screen
    // mode): then there is nothing else in that window that a crop could
    // catch. While it has a frame or bars of its own showing (it was made to
    // leave full screen and is being sent back, see ViewerTrack), the surface
    // moves about inside the window a moment before or after the bars do, and
    // a crop taken from its reported place can hold a piece of them: nothing
    // is shown for that moment instead.
    // (Should it not go back, the picture returns once its layout has stood
    // still for a while: a crop of a layout at rest is exact.)
    static RECT restTop = {}, restSurface = {};
    static double restSince = 0;
    bool trusted = false;
    if (surface && GetWindowRect(game, &top) && GetWindowRect(surface, &s) && s.right > s.left && s.bottom > s.top) {
        if (!EqualRect(&top, &restTop) || !EqualRect(&s, &restSurface)) restTop = top, restSurface = s, restSince = t;
        trusted = (abs(s.left - top.left) <= 2 && abs(s.top - top.top) <= 2 && abs(s.right - top.right) <= 2 && abs(s.bottom - top.bottom) <= 2) ||
                  t - restSince > 2500;
    }
    if (trusted) {
        p.fVisible = TRUE;
        p.opacity = static_cast<BYTE>(std::lround(viewOpacity * 255));
        p.rcSource = {s.left - top.left, s.top - top.top, s.right - top.left, s.bottom - top.top};
        const RECT pic = PictureRect();
        const LONG pw = pic.right - pic.left, ph = pic.bottom - pic.top, sw = s.right - s.left, sh = s.bottom - s.top;
        LONG w = pw, h = MulDiv(pw, sh, sw);
        if (h > ph) h = ph, w = MulDiv(ph, sw, sh);
        if (pw - w <= 2 && ph - h <= 2) w = pw, h = ph; // the same shape: edge to edge
        p.rcDestination = {pic.left + (pw - w) / 2, pic.top + (ph - h) / 2, pic.left + (pw - w) / 2 + w, pic.top + (ph - h) / 2 + h};
    }
    const bool same = !viewFresh && p.fVisible == viewProps.fVisible && p.opacity == viewProps.opacity &&
                      EqualRect(&p.rcSource, &viewProps.rcSource) && EqualRect(&p.rcDestination, &viewProps.rcDestination);
    if (!same) {
        if (viewFresh || p.fVisible != viewProps.fVisible || !EqualRect(&p.rcSource, &viewProps.rcSource) ||
            !EqualRect(&p.rcDestination, &viewProps.rcDestination))
            Log(L"viewer: picture %s, source %ld,%ld %ldx%ld of the emulator window, shown at %ld,%ld %ldx%ld", p.fVisible ? L"on" : L"off",
                p.rcSource.left, p.rcSource.top, p.rcSource.right - p.rcSource.left, p.rcSource.bottom - p.rcSource.top, p.rcDestination.left,
                p.rcDestination.top, p.rcDestination.right - p.rcDestination.left, p.rcDestination.bottom - p.rcDestination.top);
        DwmUpdateThumbnailProperties(viewThumb, &p);
        viewProps = p;
        viewFresh = false;
    }
    // Under the picture our own window is black, so that a moment without a
    // render surface (the yuzu family rebuilding shaders) shows black and not
    // the last frame the menu drew.
    if (GameShowing() && viewOpacity >= 1 && !viewBlank) {
        gfx::Begin(gfx::rgb(0x000000));
        gfx::End();
        viewBlank = true;
    }
}

// The emulator's main window is kept exactly under the picture: inside our
// window in every state, and with a render surface of the picture's size, so
// that the copy is exact. (Emulators open wherever they last were, and their
// full-screen mode takes the whole screen even when NSW-Home is a window.)
void ViewerFit(HWND game) {
    RECT cur;
    if (!game || !GetWindowRect(game, &cur)) return;
    if (IsIconic(game)) {
        // (Minimised along with everything else, by "show desktop".)
        if (!IsIconic(hwnd)) ShowWindowAsync(game, SW_SHOWNOACTIVATE);
        return;
    }
    if (IsIconic(hwnd)) return;
    RECT pic = PictureRect();
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&pic), 2);
    const bool framed = (GetWindowLongW(game, GWL_STYLE) & WS_CAPTION) != 0;
    // A window with a frame is the emulator still starting, or one that left
    // its full-screen mode: it only has to lie inside.
    const bool inside = cur.left >= pic.left && cur.top >= pic.top && cur.right <= pic.right && cur.bottom <= pic.bottom;
    if (framed ? inside : EqualRect(&cur, &pic) != 0) return;
    // Moving another program's window waits for that program even when asked
    // not to (see PlaceOnMenuScreen): on a thread of its own, one at a time.
    if (placing.exchange(true)) return;
    const int x = pic.left, y = pic.top, w = pic.right - pic.left, h = pic.bottom - pic.top;
    std::thread([=] {
        SetWindowPos(game, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        placing = false;
    }).detach();
}

// Keys the emulators give a meaning of their own that would change their
// window, stop or pause emulation, or open one of their dialogs, none of which
// the player could see. While the emulator has the focus they are hotkeys of
// ours, so the emulator never gets them. (Defaults of both families, from
// their sources: MainWindow.axaml and the "hotkeys" block of Config.json for
// Ryujinx and its forks, the default_hotkeys table for the yuzu family.)
struct BlockKey {
    UINT mod, vk;
    char family; // 'r', 'y', or 0 for both
    char does;   // 'f': our own full screen on/off, 'h': the HOME menu, 'q': end NSW-Home, 0: nothing
};
const BlockKey kBlockKeys[] = {
    {0, VK_F11, 0, 'f'},           // full screen on/off
    {MOD_ALT, VK_RETURN, 0, 'f'},  // the same
    {0, VK_ESCAPE, 0, 0},          // leave full screen; Ryujinx then: stop emulation
    {MOD_ALT, VK_F4, 0, 'q'},      // ends NSW-Home (Windows usually refuses it as a hot key: see AltF4Seen)
    {0, VK_F4, 0, 0},              // Ryujinx: show its bars; yuzu: pause
    {0, VK_F5, 0, 0},              // Ryujinx: pause; yuzu: stop emulation
    {MOD_CONTROL, 'A', 'r', 0},    // Ryubing, Kenji-NX: amiibo window
    {MOD_CONTROL, 'B', 0, 0},      // Ryubing canary, Kenji-NX master: amiibo .bin picker (amiibo_emu.cpp); yuzu: lobby browser
    {MOD_CONTROL, VK_OEM_COMMA, 0, 0}, // settings window
    {MOD_CONTROL, VK_OEM_PERIOD, 'y', 0},
    {0, VK_F6, 'y', 0},            // restart emulation
    {0, VK_F2, 'y', 0},            // amiibo file dialog
    {MOD_CONTROL, 'Q', 'y', 0},    // exit (asks)
    {MOD_CONTROL, 'O', 'y', 0},    // open a file
    {MOD_CONTROL, 'P', 'y', 0},    // screenshot (can ask where to save)
    {MOD_CONTROL, 'N', 'y', 0},    // multiplayer windows
    {MOD_CONTROL, 'C', 'y', 0},
    {MOD_CONTROL, 'L', 'y', 0},
    {MOD_CONTROL, 'R', 'y', 0},
    {MOD_CONTROL, 'F', 'y', 0},    // its filter bar, status bar, game grid
    {MOD_CONTROL, 'S', 'y', 0},
    {MOD_CONTROL, 'G', 'y', 0},
    {MOD_CONTROL, VK_F5, 'y', 0},  // TAS
    {MOD_CONTROL, VK_F6, 'y', 0},
    {MOD_CONTROL, VK_F7, 'y', 0},
};

void BlockKeys(bool on) {
    if (on == keysBlocked) return;
    keysBlocked = on;
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(kBlockKeys)); i++) {
        const BlockKey& k = kBlockKeys[i];
        if (k.family && k.family != viewFamily) continue;
        if (on) RegisterHotKey(hwnd, HOTKEY_BLOCK + i, k.mod | MOD_NOREPEAT, k.vk);
        else UnregisterHotKey(hwnd, HOTKEY_BLOCK + i);
    }
}

// The keyboard goes to the emulator, which lies under our window.
void FocusGame() {
    HWND game = GameWindow();
    if (!game) return;
    // Top-most first: bringing the emulator to the front then leaves it under us.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(game);
}

// The keyboard comes back to the menu. Windows lets only the program in front
// hand the focus on, and that is the emulator, whose window can no longer be
// hidden to give it up (hiding would stop the picture): for the moment of the
// call a thread of ours shares the emulator's input queue, which counts as
// being in front. On a thread of its own, so that an emulator that is not
// answering cannot hold the menu up.
void TakeFocusFromGame() {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return;
    DWORD pid = 0;
    const DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
    if (!tid || pid != gamePidForFocus()) {
        SetForegroundWindow(hwnd);
        return;
    }
    const HWND me = hwnd;
    std::thread([me, tid] {
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE); // gives this thread an input queue
        const bool joined = AttachThreadInput(GetCurrentThreadId(), tid, TRUE) != 0;
        SetForegroundWindow(me);
        if (joined) AttachThreadInput(GetCurrentThreadId(), tid, FALSE);
    }).detach();
}

// A window in front that ours can be put behind: not the desktop or the
// taskbar (behind those is nowhere), and not one that is top-most itself.
bool OrdinaryWindow(HWND h) {
    if (!h || !IsWindowVisible(h) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST)) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    return wcscmp(cls, L"Progman") && wcscmp(cls, L"WorkerW") && wcsncmp(cls, L"Shell_", 6);
}

// Once per pass: who has the keyboard, and the order of the windows.
void GuardGame() {
    HWND fg = GetForegroundWindow();
    DWORD fpid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fpid);
    // Our window was activated while a game is showing (a click on it, Alt+Tab,
    // its taskbar button): the game gets the keyboard. Not while a mouse
    // button is down, which may be the player moving the window by its title.
    if (GameShowing() && fg == hwnd && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
        FocusGame();
        fg = GetForegroundWindow();
        fpid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fpid);
    }
    bool ours = fg && (fpid == GetCurrentProcessId() || fpid == gamePid);
    // The emulators activate their window when they show it, and again with
    // every dialog. If the person was in another program at that moment, that
    // program gets the keyboard back: nothing of ours takes the focus by
    // itself. (Nobody can have chosen the emulator's window: it is not on
    // screen, not on the taskbar and not in Alt+Tab; coming to us goes through
    // our own window.) Handing the focus on is only allowed to whoever has it,
    // hence the shared input queue, as in TakeFocusFromGame.
    static HWND otherFront = nullptr; // the other program's window that was in front last
    if (fg && !ours) {
        otherFront = fg;
    } else if (fpid == gamePid && otherFront && IsWindow(otherFront)) {
        Log(L"foreground: the emulator took it from another program; handing it back");
        const HWND back = otherFront;
        const DWORD tid = GetWindowThreadProcessId(fg, nullptr);
        std::thread([back, tid] {
            MSG m;
            PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
            const bool joined = AttachThreadInput(GetCurrentThreadId(), tid, TRUE) != 0;
            SetForegroundWindow(back);
            if (joined) AttachThreadInput(GetCurrentThreadId(), tid, FALSE);
        }).detach();
        otherFront = nullptr; // once; if Windows refuses, it is not fought over
        ours = false;
        fpid = 0;
    } else if (fg == hwnd || !fg) {
        otherFront = nullptr; // the person came to us
    }
    static int lastFront = -1; // --log: who has the keyboard
    const int front = !ours ? 0 : fpid == gamePid ? 2 : 1;
    if (front != lastFront) Log(L"foreground: %s", front == 2 ? L"the emulator" : front == 1 ? L"NSW-Home" : L"another program");
    lastFront = front;
    // Above everything while the game starts (as before) and while the person
    // is with us; an ordinary window otherwise, so that another program they
    // switch to is not covered.
    const bool wantTop = !menuHiddenForGame || ours;
    const bool isTop = (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (wantTop != isTop) {
        SetWindowPos(hwnd, wantTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!wantTop && OrdinaryWindow(fg)) SetWindowPos(hwnd, fg, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // Any window of the emulator that got above ours goes back under it.
    int looked = 0;
    for (HWND h = GetWindow(hwnd, GW_HWNDPREV); h && looked < 400; h = GetWindow(h, GW_HWNDPREV), looked++) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == gamePid && IsWindowVisible(h))
            SetWindowPos(h, hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    BlockKeys(fpid == gamePid);
}

// ---- the emulator's own dialogs, shown as ours ----

struct AppletText; // see "pause on HOME, and the games' applets"
struct DialogText {
    HWND wnd;
    std::wstring text;
    std::vector<std::wstring> buttons;
    std::shared_ptr<AppletText> applet; // when it is a game's applet (software keyboard...) and not a message
};

struct EmuDialog {
    HWND wnd = nullptr;
    ULONGLONG since = 0; // tick it was first seen (or from which it counts again)
    bool reading = false, shown = false;
    bool resume = false; // it took the screen from the game, which gets it back
} emuDlg;

struct DialogFind {
    DWORD pid;
    HWND main, found;
    bool loose; // also windows the emulator gave no owner
};

BOOL CALLBACK DialogFindProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<DialogFind*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != ctx->pid || h == ctx->main || !IsWindowVisible(h)) return TRUE;
    RECT rc, mainRc = {};
    if (!GetWindowRect(h, &rc) || rc.right - rc.left < 120 || rc.bottom - rc.top < 60) return TRUE; // a tool tip, a list
    if (IsEmulatorTopLevel(h)) {
        // Not a window of another window of its: a second window, which is a
        // message only if it is smaller than the main one and has a title.
        if (!ctx->loose || !ctx->main || !GetWindowRect(ctx->main, &mainRc)) return TRUE;
        if (!(GetWindowLongW(h, GWL_STYLE) & WS_CAPTION)) return TRUE;
        if (rc.right - rc.left >= mainRc.right - mainRc.left || rc.bottom - rc.top >= mainRc.bottom - mainRc.top) return TRUE;
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        if (!wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"PseudoConsoleWindow")) return TRUE;
    }
    ctx->found = h;
    return FALSE;
}

// A message box or dialog the emulator has up.
HWND EmulatorDialog() {
    DialogFind ctx = {gamePid, gameHwnd, nullptr, false};
    EnumWindows(DialogFindProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.found && gameHwnd && menuHiddenForGame) {
        ctx.loose = true;
        EnumWindows(DialogFindProc, reinterpret_cast<LPARAM>(&ctx));
    }
    return ctx.found;
}

// Reads a dialog through the accessibility interface every toolkit offers
// (UI Automation): its text and the names of its buttons; or, with `press`,
// presses the button of that name.
void WalkDialog(IUIAutomationTreeWalker* walker, IUIAutomationElement* el, int depth, DialogText& out, const std::wstring* press, bool& pressed,
                int& budget) {
    if (!el || depth > 16 || budget-- <= 0) return;
    CONTROLTYPEID type = 0;
    el->get_CurrentControlType(&type);
    if (type == UIA_TitleBarControlTypeId || type == UIA_MenuBarControlTypeId) return; // the window's own buttons
    BSTR raw = nullptr;
    el->get_CurrentName(&raw);
    std::wstring name = raw ? raw : L"";
    if (raw) SysFreeString(raw);
    if (type == UIA_ButtonControlTypeId) {
        if (name.empty()) return;
        if (!press) {
            out.buttons.push_back(name);
        } else if (!pressed && name == *press) {
            IUIAutomationInvokePattern* invoke = nullptr;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke) {
                pressed = SUCCEEDED(invoke->Invoke());
                invoke->Release();
            }
        }
        return; // the label inside a button is not the message
    }
    if (type == UIA_TextControlTypeId && !press && !name.empty()) {
        if (!out.text.empty()) out.text += L"\n";
        out.text += name;
    }
    IUIAutomationElement* child = nullptr;
    walker->GetFirstChildElement(el, &child);
    while (child) {
        WalkDialog(walker, child, depth + 1, out, press, pressed, budget);
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;
    }
}

BOOL CALLBACK PlainDialogProc(HWND h, LPARAM lp) {
    auto* out = reinterpret_cast<DialogText*>(lp);
    wchar_t cls[32] = {}, text[512] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (!IsWindowVisible(h) || !GetWindowTextW(h, text, ARRAYSIZE(text))) return TRUE;
    if (!_wcsicmp(cls, L"Static")) out->text += (out->text.empty() ? L"" : L"\n") + std::wstring(text);
    else if (!_wcsicmp(cls, L"Button")) out->buttons.push_back(text);
    return TRUE;
}

// ---- pause on HOME, and the games' applets ----------------------------------
//
// Pause. The console stops a game while its HOME menu is up; the emulators are
// told to do the same with their own pause, which is clean in both families:
//   - Ryujinx, Ryubing, Kenji-NX: AppHost.Pause() (src/Ryujinx/Systems/AppHost.cs
//     956 in Ryubing, AppHost.cs 922 in Kenji-NX) calls Horizon.TogglePauseEmulation
//     (src/Ryujinx.HLE/HOS/Horizon.cs 537), which stops the game's threads
//     (SetActivity), holds the audio output (its pause event) and stops the
//     guest clock (TickSource.Suspend), and adds " (<Paused>)" to the window's
//     title (TitleHelper.ActiveApplicationTitle), which is how it is seen
//     here. It is reached only through its hotkey, F5 by default ("hotkeys":
//     {"pause"} in Config.json, ConfigurationState.cs 276): the menu item
//     (MainMenuBarView.axaml 106) is hidden in full screen and an Avalonia
//     MenuItem has no Invoke for UI Automation (MenuItemAutomationPeer). The
//     hotkey is read once a millisecond (AppHost.MainLoop / UpdateFrame 1298)
//     from the keys Avalonia saw, and only while its window is the active
//     one (`_viewModel.IsActive`; a window that is not drops every key):
//     so the key is posted while the emulator still has the keyboard, at the
//     HOME press, held 60 ms, and the menu takes the keyboard once the title
//     says paused. Going back, the key is posted once the game has the
//     keyboard again.
//   - Eden, Suyu, Citron (yuzu): the "Pause"/"Continue" item of its Emulation
//     menu (action_Pause, src/yuzu/main.ui 284 in Eden, src/citron/main.ui 79;
//     OnPauseContinueGame, main_window.cpp 9050) stops the emulation thread:
//     System::Pause (src/core/core.cpp 232) pauses core timing and the
//     kernel, and its audio streams write silence while paused
//     (src/audio_core/sink/sink_stream.cpp 174). The item is pressed through
//     UI Automation (Qt's accessible menu item triggers its action when
//     invoked, whether or not the menu is open or the window active); its
//     name changes from "Pause" to "Continue" (UpdateMenuState, 7671), which
//     is how it is seen (in any language: only the change is looked at).
//     Should the item not be found, its hotkey F4 (src/qt_common/config/
//     uisettings.cpp 43, "Continue/Pause Emulation", a window shortcut: only
//     while the window is active) is posted as for Ryujinx, unconfirmed.
// Only once the game has drawn and been shown (the first-picture problem
// behind PrepareEmulator's UI.pauseWhenInBackground=false cannot come back),
// never while a dialog of the emulator's is up, and never by taking the
// foreground: the emulator is only ever in front when it already was.
// Why not NtSuspendProcess, which needs no focus and no key: the game's clock
// would not stop (on resume the game sees the whole time in the menu go by at
// once: timers, physics, the audio renderer catching up); its audio stream
// would underrun instead of being paused; its windows stop answering, so after
// 5 s Windows treats them as hung and may put up a "not responding" ghost
// window, which is not cloaked; its pad and network threads freeze mid-call;
// and every call NSW-Home makes into its windows (UI Automation, moving them,
// sharing its input queue to hand the focus over) would hang. (GPU work
// already queued completes; a suspended process causes no GPU timeout.)
// Closing the game while it is paused is unchanged: the job is ended.
//
// Applets. A game's software keyboard, controller applet and so on open a
// window of the emulator's; the dialog code below finds it as it finds any
// dialog, and ReadApplet tells which it is from its automation names:
//   - Software keyboard. Ryujinx family: SwkbdAppletDialog (src/Ryujinx/UI/
//     Applet/SwkbdAppletDialog.axaml(.cs), shown by AvaHostUIHandler.
//     DisplayInputDialog, AvaHostUIHandler.cs 127), a content dialog with the
//     game's two lines, a text box named "Input", a line named "Error" with the
//     length limits and the kind ("0-9" for numbers), OK ("PrimaryButton") and
//     Cancel ("CloseButton"). Its OK is only re-enabled by a key typed into the
//     box (Message_TextInput / Message_KeyUp), not by a text set from outside:
//     when it stays off, the box is given the focus and one harmless key (End).
//     yuzu family: QtSoftwareKeyboardDialog (src/yuzu/applets/
//     qt_software_keyboard.cpp, Citron src/citron/applets/), a frameless window
//     over the picture with label_header, label_sub, label_characters ("3/12"),
//     line_edit_osk (or text_edit_osk for several lines), every key of its own
//     on-screen keyboard as a button and button_ok; Cancel is its Esc / close
//     (reject() = the X button, 513). Setting the box's text updates its OK
//     (textChanged, 724).
//     Inline keyboards (the game draws the text itself): Ryujinx has no window
//     at all (AvaloniaDynamicTextInputHandler takes the typing straight from
//     the main window, which already has the keyboard while a game shows; a
//     controller cannot type there). yuzu shows the same window with only the
//     keys (ShowInlineKeyboard, 436): the text is typed by pressing its keys.
//   - Controller applet. Ryujinx family (ControllerAppletDialog.axaml,
//     "SaveButton" = open its settings window, "CancelButton" = close): only
//     shown when its players do not suit the game; Close lets the game go on
//     with the players NSW-Home set up. yuzu family (QtControllerSelectorDialog,
//     qt_controller.cpp): players 1.. are switched on for as many controllers
//     as NSW-Home sees, then OK.
//   - Profile selector (Ryujinx ProfileSelectorDialog, yuzu
//     QtProfileSelectionDialog), error applet (Ryujinx ErrorAppletWindow, yuzu
//     OverlayDialog) and the yuzu web applet's messages: ordinary dialogs,
//     shown as ours with their buttons (OnDialogRead). The profile selector
//     opens on the user NSW-Home started the game as, so its OK keeps that one.

bool appletTest = false;                    // --applet-test: stand-in windows, nothing is ever brought to the front
constexpr UINT WM_EMU_PAUSED = WM_APP + 41; // lParam: PauseDone*
constexpr UINT WM_EMU_NOTE = WM_APP + 42;   // lParam: std::wstring*, a line for the log from a worker thread

void Note(std::wstring s) {
    auto* p = new std::wstring(std::move(s));
    if (!PostMessageW(hwnd, WM_EMU_NOTE, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

std::wstring Bstr(BSTR b) {
    std::wstring s = b ? b : L"";
    if (b) SysFreeString(b);
    return s;
}

// A window's elements, flattened in tree order.
struct UiNode {
    IUIAutomationElement* el = nullptr;
    int parent = -1;
    CONTROLTYPEID type = 0;
    std::wstring name, id, help;
    bool offscreen = false, enabled = true;
};

// An automation id is `name`, or ends in ".name": Qt joins the object names of
// an element and its parents with dots ("MainWindow.action_Pause").
bool IdIs(const std::wstring& id, const wchar_t* name) {
    const size_t n = wcslen(name);
    return id == name || (id.size() > n && id[id.size() - n - 1] == L'.' && !id.compare(id.size() - n, n, name));
}

struct UiTree {
    std::vector<UiNode> nodes;
    UiTree() = default;
    UiTree(const UiTree&) = delete;
    UiTree& operator=(const UiTree&) = delete;
    ~UiTree() {
        for (auto& n : nodes)
            if (n.el) n.el->Release();
    }
    int Find(const wchar_t* id, CONTROLTYPEID type = 0, bool shownOnly = false) const {
        for (size_t i = 0; i < nodes.size(); i++)
            if (IdIs(nodes[i].id, id) && (!type || nodes[i].type == type) && (!shownOnly || !nodes[i].offscreen)) return static_cast<int>(i);
        return -1;
    }
};

void UiCollect(IUIAutomationTreeWalker* walker, IUIAutomationElement* el, int parent, int depth, UiTree& t, int& budget) {
    if (!el || depth > 24 || budget-- <= 0) return;
    UiNode n;
    n.el = el;
    el->AddRef();
    n.parent = parent;
    el->get_CurrentControlType(&n.type);
    BSTR b = nullptr;
    el->get_CurrentName(&b);
    n.name = Bstr(b);
    b = nullptr;
    el->get_CurrentAutomationId(&b);
    n.id = Bstr(b);
    b = nullptr;
    el->get_CurrentHelpText(&b);
    n.help = Bstr(b);
    BOOL off = FALSE, on = TRUE;
    el->get_CurrentIsOffscreen(&off);
    el->get_CurrentIsEnabled(&on);
    n.offscreen = off != FALSE;
    n.enabled = on != FALSE;
    const int me = static_cast<int>(t.nodes.size());
    t.nodes.push_back(std::move(n));
    IUIAutomationElement* child = nullptr;
    walker->GetFirstChildElement(el, &child);
    while (child) {
        UiCollect(walker, child, me, depth + 1, t, budget);
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;
    }
}

// One UI Automation client on the calling (worker) thread.
struct UiSession {
    IUIAutomation* ua = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    bool com = false;
    UiSession() {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (com && SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ua))) && ua)
            ua->get_ControlViewWalker(&walker);
    }
    UiSession(const UiSession&) = delete;
    UiSession& operator=(const UiSession&) = delete;
    ~UiSession() {
        if (walker) walker->Release();
        if (ua) ua->Release();
        if (com) CoUninitialize();
    }
    bool Read(HWND w, UiTree& t, int budget) {
        IUIAutomationElement* root = nullptr;
        if (!walker || FAILED(ua->ElementFromHandle(w, &root)) || !root) return false;
        UiCollect(walker, root, -1, 0, t, budget);
        root->Release();
        return !t.nodes.empty();
    }
};

std::wstring UiValue(IUIAutomationElement* el) {
    std::wstring s;
    IUIAutomationValuePattern* v = nullptr;
    if (el && SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&v))) && v) {
        BSTR b = nullptr;
        v->get_CurrentValue(&b);
        s = Bstr(b);
        v->Release();
    }
    return s;
}

bool UiSetValue(IUIAutomationElement* el, const std::wstring& text) {
    IUIAutomationValuePattern* v = nullptr;
    bool ok = false;
    if (el && SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&v))) && v) {
        BSTR b = SysAllocString(text.c_str());
        ok = b && SUCCEEDED(v->SetValue(b));
        SysFreeString(b);
        v->Release();
    }
    return ok;
}

bool UiInvoke(IUIAutomationElement* el) {
    IUIAutomationInvokePattern* p = nullptr;
    bool ok = false;
    if (el && SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&p))) && p) {
        ok = SUCCEEDED(p->Invoke());
        p->Release();
    }
    return ok;
}

// A check box or a checkable group switched on (if it is not already).
bool UiToggleOn(IUIAutomationElement* el) {
    IUIAutomationTogglePattern* p = nullptr;
    bool ok = false;
    if (el && SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&p))) && p) {
        ToggleState s = ToggleState_Off;
        p->get_CurrentToggleState(&s);
        ok = s == ToggleState_On || SUCCEEDED(p->Toggle());
        p->Release();
    }
    return ok;
}

std::wstring UiName(IUIAutomationElement* el) {
    BSTR b = nullptr;
    if (el) el->get_CurrentName(&b);
    return Bstr(b);
}

bool UiEnabled(IUIAutomationElement* el) {
    BOOL on = FALSE;
    return el && SUCCEEDED(el->get_CurrentIsEnabled(&on)) && on;
}

// A key pressed on another program's window by message (the keyboard's own
// keys of this kind are hotkeys of ours while a game shows, see BlockKeys;
// a posted key does not go through those). Held for `holdMs`: Ryujinx only
// sees a key that is still down when it looks (once a millisecond).
void PostKey(HWND w, UINT vk, int holdMs) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const bool extended = vk == VK_END || vk == VK_HOME || vk == VK_INSERT || vk == VK_DELETE || vk == VK_PRIOR || vk == VK_NEXT;
    const LPARAM down = static_cast<LPARAM>(1u | (scan << 16) | (extended ? 1u << 24 : 0u));
    PostMessageW(w, WM_KEYDOWN, vk, down);
    if (holdMs > 0) Sleep(static_cast<DWORD>(holdMs));
    PostMessageW(w, WM_KEYUP, vk, down | static_cast<LPARAM>(0xC0000000u));
}

std::vector<size_t> Numbers(const std::wstring& s) {
    std::vector<size_t> out;
    for (size_t i = 0; i < s.size();) {
        if (!iswdigit(s[i])) {
            i++;
            continue;
        }
        size_t v = 0;
        for (; i < s.size() && iswdigit(s[i]); i++) v = v * 10 + (s[i] - L'0');
        out.push_back(v);
    }
    return out;
}

// --log: what a dialog of the emulator's is made of (control type, automation
// id, and whether it is off screen or disabled), so that a window that is not
// recognised can be told apart afterwards. Names are left out: they can hold
// what the player typed.
std::wstring UiDump(const UiTree& t) {
    std::wstring s = L"dialog elements:";
    int shown = 0;
    for (const auto& n : t.nodes) {
        if (n.id.empty() && n.type != UIA_EditControlTypeId) continue;
        if (++shown > 80) {
            s += L" ...";
            break;
        }
        s += L" " + std::to_wstring(n.type - UIA_ButtonControlTypeId) + L":" + (n.id.empty() ? L"-" : n.id) + (n.offscreen ? L"(off)" : L"") +
             (n.enabled ? L"" : L"(disabled)");
    }
    return s + L" (" + std::to_wstring(t.nodes.size()) + L" in all; types counted from Button=0, Edit=4, Text=20)";
}

struct AppletText {
    char kind = 0; // 'k' software keyboard, 'c' controller applet
    bool inlineKb = false, numeric = false;
    std::wstring header, sub, guide, initial;
    size_t minLen = 0, maxLen = 0;
};

// Which applet the window `t` is, if any, and what it asks for.
std::shared_ptr<AppletText> ReadApplet(const UiTree& t, char family) {
    auto a = std::make_shared<AppletText>();
    const auto& n = t.nodes;
    if (family == 'r') {
        if (t.Find(L"SaveButton", UIA_ButtonControlTypeId) >= 0 && t.Find(L"CancelButton", UIA_ButtonControlTypeId) >= 0) {
            a->kind = 'c';
            return a;
        }
        int edit = t.Find(L"Input", UIA_EditControlTypeId);
        for (size_t i = 0; edit < 0 && i < n.size(); i++)
            if (n[i].type == UIA_EditControlTypeId) edit = static_cast<int>(i);
        if (edit < 0 || t.Find(L"PrimaryButton", UIA_ButtonControlTypeId) < 0) return nullptr;
        a->kind = 'k';
        // The dialog's own title ("Software Keyboard") comes first, then the
        // game's two lines (MainText, SecondaryText), then the box.
        std::vector<std::wstring> lines;
        for (int i = 0; i < edit; i++)
            if (n[i].type == UIA_TextControlTypeId && !n[i].name.empty() && !n[i].offscreen && !IdIs(n[i].id, L"Error")) lines.push_back(n[i].name);
        if (lines.size() >= 3) a->header = lines[lines.size() - 2], a->sub = lines.back();
        else if (lines.size() == 2) a->header = lines[1];
        // "Must be between 1 and 12 characters long" / "at least 1"
        // (SetInputLengthValidation), and "0-9" for a number (SetInputValidation).
        if (const int err = t.Find(L"Error", UIA_TextControlTypeId); err >= 0) {
            const std::vector<size_t> v = Numbers(n[err].name);
            if (n[err].name.find(L"0-9") != std::wstring::npos) a->numeric = true;
            std::vector<size_t> lim;
            for (size_t x : v)
                if (!(a->numeric && x <= 9)) lim.push_back(x); // not the "0" and "9" of "0-9"
            if (lim.size() >= 2) a->minLen = lim[0], a->maxLen = lim[1];
            else if (lim.size() == 1) a->minLen = lim[0];
        }
        a->initial = UiValue(n[edit].el);
        a->guide = n[edit].help;
        return a;
    }
    if (t.Find(L"groupPlayer1Connected") >= 0) {
        a->kind = 'c';
        return a;
    }
    const int ok = t.Find(L"button_ok");
    if (ok < 0) return nullptr;
    a->kind = 'k';
    const int line = t.Find(L"line_edit_osk", 0, true), box = t.Find(L"text_edit_osk", 0, true);
    const int edit = box >= 0 ? box : line;
    a->inlineKb = edit < 0; // only its keys are showing: the game draws the text
    if (const int h = t.Find(L"label_header", 0, true); h >= 0) a->header = n[h].name;
    if (const int s = t.Find(L"label_sub", 0, true); s >= 0) a->sub = n[s].name;
    if (const int c = t.Find(edit == box && box >= 0 ? L"label_characters_box" : L"label_characters", 0, true); c >= 0) {
        const std::vector<size_t> v = Numbers(n[c].name); // "3/12"
        if (v.size() == 2) a->maxLen = v[1];
    }
    const int num = t.Find(L"button_ok_num", 0, true);
    a->numeric = num >= 0;
    if (edit >= 0) {
        a->initial = UiValue(n[edit].el);
        a->guide = n[edit].help; // Qt gives the box's placeholder as its description
    }
    // Its OK is off while the text is too short (SetBackspaceOkEnabled).
    const int shownOk = num >= 0 ? num : ok;
    if (!n[shownOk].enabled) a->minLen = a->initial.size() + 1;
    return a;
}

struct AppletAnswer {
    HWND dialog;
    char family, kind;
    bool inlineKb, ok;
    std::wstring text;
    size_t maxLen;
    int players; // controller applet: how many players to switch on
};

// The OK of the yuzu family's keyboard: the one of the page showing.
int YuzuOk(const UiTree& t) {
    for (const wchar_t* id : {L"button_ok_num", L"button_ok_shift", L"button_ok"}) {
        const int i = t.Find(id, 0, true);
        if (i >= 0 && UiEnabled(t.nodes[i].el)) return i;
    }
    return t.Find(L"button_ok");
}

// On a thread of its own: answers the applet in the emulator's window.
void AppletWorker(AppletAnswer a) {
    UiSession s;
    UiTree t;
    if (!s.ua || !s.Read(a.dialog, t, 3000)) {
        Note(L"applet: the emulator's window could not be read to answer it");
        return;
    }
    auto& n = t.nodes;
    std::wstring how;
    if (a.kind == 'c') {
        if (a.family == 'r') {
            const int close = t.Find(L"CancelButton", UIA_ButtonControlTypeId);
            how = close >= 0 && UiInvoke(n[close].el) ? L"controller applet: Close pressed" : L"controller applet: Close could NOT be pressed";
        } else {
            int on = 0;
            for (int p = 1; p <= a.players && p <= 8; p++) {
                const std::wstring id = L"groupPlayer" + std::to_wstring(p) + L"Connected";
                const int g = t.Find(id.c_str());
                if (g >= 0 && n[g].enabled && UiToggleOn(n[g].el)) on++;
            }
            // Its OK is the first button of its button box (Windows puts OK before Cancel).
            const int bb = t.Find(L"buttonBox");
            int okb = -1;
            for (size_t i = 0; bb >= 0 && okb < 0 && i < n.size(); i++)
                if (n[i].parent == bb && n[i].type == UIA_ButtonControlTypeId) okb = static_cast<int>(i);
            Sleep(60);
            const bool pressed = okb >= 0 && UiEnabled(n[okb].el) && UiInvoke(n[okb].el);
            how = L"controller applet: " + std::to_wstring(on) + L" player(s) switched on, OK " + (pressed ? L"pressed" : L"NOT pressed (still off)");
        }
    } else if (a.family == 'r') {
        int edit = t.Find(L"Input", UIA_EditControlTypeId);
        for (size_t i = 0; edit < 0 && i < n.size(); i++)
            if (n[i].type == UIA_EditControlTypeId) edit = static_cast<int>(i);
        const int okb = t.Find(L"PrimaryButton", UIA_ButtonControlTypeId), cancel = t.Find(L"CloseButton", UIA_ButtonControlTypeId);
        if (!a.ok) {
            if (cancel >= 0 && UiInvoke(n[cancel].el)) {
                how = L"software keyboard: Cancel pressed";
            } else {
                PostKey(a.dialog, VK_ESCAPE, 20);
                how = L"software keyboard: Esc posted (no Cancel to press)";
            }
        } else {
            const bool set = edit >= 0 && UiSetValue(n[edit].el, a.text);
            bool pressed = okb >= 0 && UiInvoke(n[okb].el);
            how = std::wstring(L"software keyboard: text ") + (set ? L"set" : L"NOT set");
            if (!pressed) {
                // Its OK only looks at the text again when a key is typed into
                // the box: the box gets the focus and an End key, which moves
                // nothing but the caret. The window has to be in front for that;
                // the game is about to have the screen back anyway.
                if (!appletTest) {
                    SetForegroundWindow(a.dialog);
                    Sleep(120);
                    if (edit >= 0) n[edit].el->SetFocus();
                    Sleep(40);
                }
                PostKey(a.dialog, VK_END, 20);
                Sleep(120);
                pressed = okb >= 0 && UiInvoke(n[okb].el);
                how += L", OK was off: End key typed";
                if (!pressed) {
                    PostKey(a.dialog, VK_RETURN, 20); // Message_KeyUp: Enter takes the text when OK is on
                    how += L", OK still off: Enter posted";
                }
            }
            if (pressed) how += L", OK pressed";
        }
    } else if (!a.inlineKb) {
        if (!a.ok) {
            PostMessageW(a.dialog, WM_CLOSE, 0, 0); // closeEvent -> reject() -> its X button: cancel
            how = L"software keyboard: closed (its Cancel)";
        } else {
            int edit = t.Find(L"text_edit_osk", 0, true);
            if (edit < 0) edit = t.Find(L"line_edit_osk", 0, true);
            const bool set = edit >= 0 && UiSetValue(n[edit].el, a.text);
            Sleep(60); // its OK follows the text (textChanged)
            const int okb = YuzuOk(t);
            const bool pressed = okb >= 0 && UiEnabled(n[okb].el) && UiInvoke(n[okb].el);
            if (!pressed) PostKey(a.dialog, VK_RETURN, 20); // returnPressed: its Plus, which submits
            how = std::wstring(L"software keyboard: text ") + (set ? L"set" : L"NOT set") + (pressed ? L", OK pressed" : L", Enter posted");
        }
    } else {
        if (!a.ok) {
            PostMessageW(a.dialog, WM_CLOSE, 0, 0);
            how = L"inline keyboard: closed (its Cancel)";
        } else {
            // What the game shows is not readable: it is emptied with its own
            // backspace key (off once there is nothing left before the cursor),
            // then typed key by key.
            const int back = t.Find(L"button_backspace");
            int erased = 0;
            while (back >= 0 && erased < 600 && UiEnabled(n[back].el) && UiInvoke(n[back].el)) {
                erased++;
                Sleep(2);
            }
            const int space = t.Find(L"button_space");
            std::map<wchar_t, int> keys;
            for (size_t i = 0; i < n.size(); i++) {
                if (n[i].type != UIA_ButtonControlTypeId || n[i].name.size() != 1 || !n[i].enabled) continue;
                const wchar_t c = n[i].name[0];
                // (The number pad repeats the digits; any one of them will do.)
                if (!keys.count(c)) keys[c] = static_cast<int>(i);
            }
            size_t typed = 0, missing = 0;
            for (wchar_t c : a.text) {
                const int k = c == L' ' ? space : keys.count(c) ? keys[c] : -1;
                if (k >= 0 && UiInvoke(n[k].el)) typed++;
                else missing++;
                Sleep(4);
            }
            const int okb = YuzuOk(t);
            const bool pressed = okb >= 0 && UiInvoke(n[okb].el);
            how = L"inline keyboard: " + std::to_wstring(erased) + L" erased, " + std::to_wstring(typed) + L" typed, " +
                  std::to_wstring(missing) + L" without a key of its own" + (pressed ? L", OK pressed" : L", OK NOT pressed");
        }
    }
    Note(how);
}

// The controller applet of a dialog that was answered once already and is
// still there is shown to the player as an ordinary dialog instead.
HWND appletAnsweredOnce = nullptr;

// OnDialogRead's step for an applet. False: shown as an ordinary dialog.
bool OnEmulatorApplet(const DialogText& d) {
    const AppletText& a = *d.applet;
    const HWND wnd = d.wnd;
    const char fam = viewFamily;
    if (a.kind == 'c') {
        if (appletAnsweredOnce == wnd) return false;
        appletAnsweredOnce = wnd;
        int players = 0;
        for (bool on : ConnectedPads()) players += on ? 1 : 0;
        players = (std::max)(players, 1);
        Log(L"controller applet: answered from NSW-Home's own players (%d)", players);
        std::thread(AppletWorker, AppletAnswer{wnd, fam, 'c', false, true, {}, 0, players}).detach();
        emuDlg.shown = false; // looked at again in a while, in case it is still there
        emuDlg.since = GetTickCount64() + 1500;
        return true;
    }
    if (a.kind != 'k') return false;
    Log(L"software keyboard%s: %zu-%zu characters%s, header %zu characters, text already there %zu", a.inlineKb ? L" (inline)" : L"",
        a.minLen, a.maxLen, a.numeric ? L", numbers" : L"", a.header.size(), a.initial.size());
    emuDlg.shown = true;
    // It takes the screen as the emulator's other dialogs do (OnDialogRead).
    if (GameShowing()) {
        emuDlg.resume = true;
        const bool take = OursHasFocus();
        windowVisible = true;
        ViewShow(false);
        if (take) TakeFocusFromGame();
    } else if (OursHasFocus() && GetForegroundWindow() != hwnd) {
        TakeFocusFromGame();
    }
    ui::EmuTextAsk ask;
    ask.header = a.header;
    ask.sub = a.sub;
    ask.guide = a.guide;
    ask.text = a.initial;
    ask.minLen = a.minLen;
    ask.maxLen = a.maxLen;
    ask.numeric = a.numeric;
    const bool inl = a.inlineKb;
    const size_t maxLen = a.maxLen;
    ui::OnEmulatorText(ask, [wnd, fam, inl, maxLen](bool ok, const std::wstring& text) {
        Log(ok ? L"software keyboard: confirmed (%zu characters)" : L"software keyboard: backed out of", text.size());
        std::thread(AppletWorker, AppletAnswer{wnd, fam, 'k', inl, ok, text, maxLen, 0}).detach();
        // As for the other dialogs: normally the emulator now takes it away;
        // if it is still there in a while, it is shown again.
        emuDlg.shown = false;
        emuDlg.since = GetTickCount64() + 1500;
    });
    return true;
}

// -- pause --

struct EmuPause {
    bool want = false;        // the menu is over the game: it should be stopped
    bool paused = false;      // it is, as far as the emulator has shown
    bool busy = false;        // a worker is at it
    char how = 0;             // how it was stopped: 'u' its menu item, 'k' its hotkey
    int tries = 0;            // failed attempts at the step being taken
    bool told = false;        // --log: that the step has to wait has been said
    bool focusAfter = false;  // the menu takes the keyboard once the game has stopped...
    ULONGLONG focusBy = 0;    // ... or at this tick at the latest
    ULONGLONG frontSince = 0; // the emulator has had the keyboard since this tick
    ULONGLONG retryAt = 0;
    UINT key = 0;             // its pause key (read once per game)
    std::wstring base;        // Ryujinx: its title while running
    std::wstring runName;     // yuzu: its menu item's name while running ("Pause")
} emuPause;
int pauseGen = 0; // which game a worker's answer is about

struct PauseStep {
    int gen;
    bool pause;
    char family, how; // how: 0 any, 'u' / 'k' only that way
    bool keyOk;       // the emulator has the keyboard, so its hotkey may be used
    HWND game;
    UINT key;
    std::wstring base, runName;
};

struct PauseDone {
    int gen;
    bool pause, ok;
    char how;
    std::wstring base, runName, note;
};

std::wstring TitleOf(HWND w) {
    wchar_t t[512] = {};
    InternalGetWindowText(w, t, ARRAYSIZE(t)); // the stored title: never waits for the emulator
    return t;
}

// The yuzu family's pause item. Searched in its menu bar first: the rest of
// its window (the game list) can hold thousands of elements.
int FindPauseItem(UiSession& s, HWND game, UiTree& t) {
    IUIAutomationElement* root = nullptr;
    if (!s.walker || FAILED(s.ua->ElementFromHandle(game, &root)) || !root) return -1;
    IUIAutomationElement* child = nullptr;
    s.walker->GetFirstChildElement(root, &child);
    while (child) {
        CONTROLTYPEID type = 0;
        child->get_CurrentControlType(&type);
        if (type == UIA_MenuBarControlTypeId) {
            int budget = 800;
            UiCollect(s.walker, child, -1, 0, t, budget);
        }
        IUIAutomationElement* next = nullptr;
        s.walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;
    }
    int item = t.Find(L"action_Pause");
    if (item < 0) {
        int budget = 4000;
        UiCollect(s.walker, root, -1, 0, t, budget);
        item = t.Find(L"action_Pause");
    }
    root->Release();
    return item;
}

void PauseWorker(PauseStep s) {
    auto* d = new PauseDone{s.gen, s.pause, false, s.how, s.base, s.runName, {}};
    if (s.family == 'r') {
        const std::wstring before = TitleOf(s.game);
        if (s.pause) d->base = before;
        if (!s.pause && before == d->base) {
            d->ok = true;
            d->note = L"it was running already";
        } else if (!s.key) {
            d->note = L"its pause key is not set";
        } else {
            PostKey(s.game, s.key, 60);
            for (const ULONGLONG end = GetTickCount64() + 700; !d->ok && GetTickCount64() < end;) {
                Sleep(10);
                const std::wstring now = TitleOf(s.game);
                d->ok = s.pause ? now != d->base : now == d->base;
            }
            d->note = d->ok ? L"its title says so" : L"its title did not change";
        }
        d->how = 'k';
    } else {
        UiSession ses;
        UiTree t;
        const int item = s.how != 'k' && ses.ua ? FindPauseItem(ses, s.game, t) : -1;
        if (item >= 0) {
            IUIAutomationElement* el = t.nodes[item].el;
            const std::wstring before = UiName(el);
            if (s.pause) d->runName = before;
            if (!s.pause && before == d->runName) {
                d->ok = true;
                d->note = L"it was running already";
            } else if (!UiInvoke(el)) {
                d->note = L"its menu item could not be pressed";
            } else {
                for (const ULONGLONG end = GetTickCount64() + 500; !d->ok && GetTickCount64() < end;) {
                    Sleep(10);
                    d->ok = UiName(el) != before;
                }
                d->note = d->ok ? L"its menu item says so" : L"its menu item did not change";
            }
            d->how = 'u';
        } else if (s.how != 'u' && s.keyOk) {
            PostKey(s.game, VK_F4, 40);
            d->ok = true;
            d->how = 'k';
            d->note = L"menu item not found: its hotkey F4 posted, unconfirmed";
        } else {
            d->note = L"its menu item was not found";
        }
    }
    if (!PostMessageW(hwnd, WM_EMU_PAUSED, 0, reinterpret_cast<LPARAM>(d))) delete d;
}

bool EmulatorInFront() {
    if (appletTest) return true; // (a stand-in that does not check)
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    return pid && pid == gamePid;
}

// Ryujinx's pause key, from the Config.json it reads ("hotkeys": {"pause":
// "F5"}); F5, its default, when that cannot be read; 0 when it is unbound.
UINT RyuPauseKey() {
    wchar_t exe[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    const std::wstring path = gameProc && QueryFullProcessImageNameW(gameProc, 0, exe, &len) ? ryucfg::ConfigPath(exe) : ryucfg::ConfigPath();
    std::string json;
    if (FILE* f = nullptr; !_wfopen_s(&f, path.c_str(), L"rb") && f) {
        char buf[16384];
        for (size_t got; (got = fread(buf, 1, sizeof buf, f)) > 0;) json.append(buf, got);
        fclose(f);
    }
    const size_t hk = json.find("\"hotkeys\"");
    const size_t at = hk == std::string::npos ? hk : json.find("\"pause\"", hk);
    if (at == std::string::npos) return VK_F5;
    const size_t q1 = json.find('"', json.find(':', at)), q2 = q1 == std::string::npos ? q1 : json.find('"', q1 + 1);
    if (q2 == std::string::npos) return VK_F5;
    const std::string k = json.substr(q1 + 1, q2 - q1 - 1);
    if (k == "Unbound") return 0;
    if (k.size() >= 2 && k[0] == 'F' && isdigit(static_cast<unsigned char>(k[1]))) {
        const int f = atoi(k.c_str() + 1);
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return static_cast<UINT>(k[0]);
    if (k.size() == 7 && !k.compare(0, 6, "Number")) return static_cast<UINT>(k[6]);
    static const struct {
        const char* name;
        UINT vk;
    } named[] = {{"Pause", VK_PAUSE}, {"Home", VK_HOME}, {"End", VK_END},       {"Insert", VK_INSERT},
                 {"Delete", VK_DELETE}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Tab", VK_TAB}};
    for (const auto& nk : named)
        if (k == nk.name) return nk.vk;
    return VK_F5;
}

// The game may be stopped now: it has drawn and is shown, nothing of the
// emulator's is waiting for an answer, and it is not being closed.
bool PauseAllowed() {
    return viewing && gameProc && gameHwnd && IsWindow(gameHwnd) && gameReadyAt && menuHiddenForGame && !emuDlg.wnd && !closeAsked;
}

void PauseFocusMenu(const wchar_t* why) {
    if (!emuPause.focusAfter) return;
    emuPause.focusAfter = false;
    Log(L"pause: the menu takes the keyboard (%s)", why);
    if (windowVisible && gameProc) TakeFocusFromGame();
}

// Once per pass while an emulator runs: takes the next step towards what is
// wanted, one at a time.
void PauseTrack(ULONGLONG now) {
    EmuPause& p = emuPause;
    const bool front = EmulatorInFront();
    if (!front) {
        p.frontSince = 0;
    } else if (!p.frontSince) {
        p.frontSince = now;
        if (!p.want) p.tries = 0, p.told = false; // each time the game gets the keyboard back is a new chance to resume it
    }
    if (p.focusAfter && now >= p.focusBy) PauseFocusMenu(L"the game did not say it stopped in time");
    if (p.busy || p.want == p.paused || !gameHwnd || now < p.retryAt) return;
    if (p.want && !PauseAllowed()) return;
    if (p.tries >= 3) {
        if (!p.told) Log(L"pause: given up after 3 tries; the game %s", p.paused ? L"stays stopped" : L"keeps running");
        p.told = true;
        return;
    }
    // Ryujinx's hotkey (and yuzu's, when it was used) needs the emulator to
    // have the keyboard; going back, for long enough to know it (Avalonia
    // marks its window active a moment after Windows does).
    const bool needsFront = viewFamily == 'r' || (!p.want && p.how == 'k');
    if (needsFront && (!front || (!p.want && now - p.frontSince < 80))) {
        if (!p.told && !front) Log(L"pause: waiting for the game to have the keyboard before %s it", p.want ? L"stopping" : L"resuming");
        p.told = !front;
        return;
    }
    if (viewFamily == 'r' && !p.key) p.key = RyuPauseKey();
    p.busy = true;
    PauseStep s{pauseGen, p.want, viewFamily, p.want ? '\0' : p.how, front, gameHwnd, p.key, p.base, p.runName};
    Log(L"pause: %s the game (%s)", p.want ? L"stopping" : L"resuming", viewFamily == 'r' ? L"its pause key" : L"its menu item");
    std::thread(PauseWorker, s).detach();
}

void OnPauseDone(PauseDone* raw) {
    std::unique_ptr<PauseDone> d(raw);
    EmuPause& p = emuPause;
    if (d->gen != pauseGen) return; // about a game that has ended
    p.busy = false;
    Log(L"pause: %s %s (%s)", d->pause ? L"stopping" : L"resuming", d->ok ? L"done" : L"FAILED", d->note.c_str());
    if (d->ok) {
        p.paused = d->pause;
        p.how = d->how;
        p.base = d->base;
        p.runName = d->runName;
        p.tries = 0;
        p.told = false;
    } else {
        p.tries++;
        p.retryAt = GetTickCount64() + 400;
    }
    if (d->pause) PauseFocusMenu(d->ok ? L"the game has stopped" : L"the game could not be stopped");
}

// ViewerShowMenu's part: true when the keyboard is handed to the menu later
// (once the game has stopped, see PauseFocusMenu).
bool PauseOnMenu(bool take) {
    if (!PauseAllowed()) return false;
    EmuPause& p = emuPause;
    p.want = true;
    p.tries = 0;
    p.told = false;
    const bool wait = take && EmulatorInFront() && !appletTest;
    if (wait) {
        p.focusAfter = true;
        p.focusBy = GetTickCount64() + 700;
    }
    PauseTrack(GetTickCount64());
    return wait;
}

// ViewerResume's part.
void PauseOnResume() {
    EmuPause& p = emuPause;
    p.want = false;
    p.focusAfter = false;
    p.tries = 0;
    p.told = false;
}

void PauseGameEnded() {
    emuPause = EmuPause();
    pauseGen++;
    appletAnsweredOnce = nullptr;
}

// ---- end of pause on HOME, and the games' applets ----

// On a thread of its own: these calls wait for the emulator to answer.
void DialogWorker(HWND dialog, std::wstring press) {
    auto* out = new DialogText{dialog, {}, {}};
    bool pressed = false;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        IUIAutomation* ua = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ua))) && ua) {
            IUIAutomationTreeWalker* walker = nullptr;
            IUIAutomationElement* root = nullptr;
            ua->get_ControlViewWalker(&walker);
            if (walker && SUCCEEDED(ua->ElementFromHandle(dialog, &root)) && root) {
                int budget = 600;
                WalkDialog(walker, root, 0, *out, press.empty() ? nullptr : &press, pressed, budget);
                if (press.empty()) { // a game's applet, perhaps (see "pause on HOME, and the games' applets")
                    UiTree tree;
                    int more = 3000;
                    UiCollect(walker, root, -1, 0, tree, more);
                    out->applet = ReadApplet(tree, viewFamily);
                    if (logging) Note(UiDump(tree));
                }
                root->Release();
            }
            if (walker) walker->Release();
            ua->Release();
        }
        CoUninitialize();
    }
    if (!press.empty()) {
        // A plain Windows message box takes the button's own command.
        if (!pressed)
            if (HWND b = FindWindowExW(dialog, nullptr, L"Button", press.c_str())) PostMessageW(dialog, WM_COMMAND, GetDlgCtrlID(b), reinterpret_cast<LPARAM>(b));
        delete out;
        return;
    }
    if (out->text.empty() && out->buttons.empty()) EnumChildWindows(dialog, PlainDialogProc, reinterpret_cast<LPARAM>(out));
    if (!PostMessageW(hwnd, WM_EMU_DIALOG, 0, reinterpret_cast<LPARAM>(out))) delete out;
}

// The dialog has been read: it becomes a dialog of ours, with the emulator's
// words. Its own buttons are offered (pressing one presses the emulator's),
// and closing the program always is.
void OnDialogRead(DialogText* raw) {
    std::unique_ptr<DialogText> d(raw);
    emuDlg.reading = false;
    if (!gameProc || d->wnd != emuDlg.wnd || !IsWindowVisible(d->wnd)) return;
    if (d->applet && OnEmulatorApplet(*d)) return; // a software keyboard, a controller applet
    if (IsEmulatorTopLevel(d->wnd) && d->buttons.empty()) {
        // A second window of the emulator's without a single button is not a
        // question to the player (a splash, an overlay): left alone, unseen.
        emuDlg.since = GetTickCount64() + 60000;
        return;
    }
    wchar_t title[160] = {}, mainTitle[160] = {};
    GetWindowTextW(d->wnd, title, ARRAYSIZE(title));
    if (gameHwnd) GetWindowTextW(gameHwnd, mainTitle, ARRAYSIZE(mainTitle));
    std::wstring msg;
    // (Ryujinx's dialogs are a window of this name around the real box.)
    if (title[0] && wcscmp(title, L"ContentDialogOverlayWindow") && wcscmp(title, mainTitle) && d->text.find(title) == std::wstring::npos)
        msg = std::wstring(title) + L"\n\n";
    msg += d->text;
    if (msg.size() > 480) msg = msg.substr(0, 477) + L"...";
    if (d->text.empty()) msg += L"El emulador ha mostrado un mensaje que no se puede leer desde aquí.";
    // Ours put the safe choice first (B picks it); theirs put it last.
    std::vector<std::wstring> theirs;
    if (d->buttons.size() <= 2) theirs.assign(d->buttons.rbegin(), d->buttons.rend());
    std::vector<std::wstring> labels;
    for (const auto& b : theirs) {
        std::wstring label = b;
        label.erase(std::remove(label.begin(), label.end(), L'&'), label.end());
        labels.push_back(label);
    }
    labels.push_back(L"Cerrar programa");
    Log(L"emulator dialog shown as ours: %d of its buttons, text: %.200s", static_cast<int>(theirs.size()), msg.c_str());
    emuDlg.shown = true;
    if (GameShowing()) {
        emuDlg.resume = true;
        const bool take = OursHasFocus();
        windowVisible = true;
        ViewShow(false);
        if (take) TakeFocusFromGame();
    } else if (OursHasFocus() && GetForegroundWindow() != hwnd) {
        TakeFocusFromGame(); // the dialog took the keyboard from the menu
    }
    const HWND wnd = d->wnd;
    ui::OnEmulatorDialog(msg, labels, [wnd, theirs](int i) {
        if (i < 0 || i >= static_cast<int>(theirs.size())) {
            CloseGame();
            return;
        }
        std::thread(DialogWorker, wnd, theirs[i]).detach();
        // Normally the emulator now takes its dialog away, which is noticed
        // below; if it is still there in a while, it is shown again.
        emuDlg.shown = false;
        emuDlg.since = GetTickCount64() + 1500;
    });
}

void TrackEmuDialog(ULONGLONG now) {
    if (amiibo::HandOverBusy()) return; // amiibo hand-over: its file dialog and messages are answered there (amiibo_emu.cpp)
    HWND d = EmulatorDialog();
    if (d != emuDlg.wnd) {
        const bool resume = emuDlg.resume;
        if (emuDlg.wnd) {
            Log(L"emulator dialog gone");
            ui::CloseEmulatorDialog();
        }
        emuDlg = EmuDialog();
        emuDlg.wnd = d;
        emuDlg.since = now;
        if (d) {
            emuDlg.resume = resume; // one dialog after another: still the game's turn afterwards
            Log(L"emulator dialog appeared");
        } else if (resume && gameProc && windowVisible) {
            ResumeGame();
        }
        return;
    }
    // (A dialog that is only there for a moment, like the yuzu family's
    // "closing" notice, is never shown.)
    if (!d || emuDlg.reading || emuDlg.shown || now < emuDlg.since + 500) return;
    emuDlg.reading = true;
    std::thread(DialogWorker, d, std::wstring()).detach();
}

BOOL CALLBACK AdoptNewProc(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == static_cast<DWORD>(lp) && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) ConcealGameWindow(h);
    return TRUE;
}

// Wakes the main loop the moment the emulator moves, shows, hides or destroys
// a window, so the picture's crop follows at once (the loop itself only comes
// round ten times a second while a game is showing).
void CALLBACK OnEmulatorLayout(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
}

// The emulator showed a window (see OnEmulatorShow).
void ViewerOnShow(HWND h) {
    // Owned from here on, which also cloaks it; under the launch screen and
    // inside our window until then.
    ConcealGameWindow(h);
    if (!gameHwnd && LooksLikeMainWindow(h)) gameHwnd = h;
    if (h == gameHwnd) ViewerFit(h);
}

void ViewerGameStarted(char family) {
    viewing = true;
    viewFamily = family;
    viewOn = false;
    viewOpacity = 0;
    viewBlank = false;
    fullKeyAt = 0;
    fullKeyTries = 0;
    emuDlg = EmuDialog();
    BOOL on = TRUE;
    HRESULT hr = DwmSetWindowAttribute(ownerWnd, DWMWA_CLOAK, &on, sizeof(on));
    if (FAILED(hr)) Log(L"viewer: the emulator's windows could not be cloaked (%08lx); they stay under ours", hr);
    layoutHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnEmulatorLayout, gamePid, 0, WINEVENT_OUTOFCONTEXT);
}

void ViewerGameEnded() {
    if (!viewing) return;
    if (viewThumb) DwmUnregisterThumbnail(viewThumb);
    viewThumb = nullptr;
    viewSource = nullptr;
    viewOn = false;
    viewOpacity = 0;
    viewBlank = false;
    if (layoutHook) UnhookWinEvent(layoutHook);
    layoutHook = nullptr;
    BlockKeys(false);
    if (emuDlg.wnd) ui::CloseEmulatorDialog();
    emuDlg = EmuDialog();
    PauseGameEnded();
    BOOL off = FALSE;
    DwmSetWindowAttribute(ownerWnd, DWMWA_CLOAK, &off, sizeof(off));
    viewing = false;
}

void ViewerShowMenu(bool userAsked) {
    const bool take = userAsked || OursHasFocus(); // before anything changes hands
    windowVisible = true;
    ViewShow(false);
    if (PauseOnMenu(take)) return; // the game stops; the menu takes the keyboard once it has (PauseFocusMenu)
    if (take) TakeFocusFromGame();
    else ShowWindow(hwnd, SW_SHOWNA);
}

void ViewerResume() {
    Log(L"resume");
    const bool take = OursHasFocus();
    windowVisible = false; // the menu stops drawing; the picture fades in over its last frame
    ViewShow(true);
    if (emuDlg.wnd) { // a dialog of the emulator's is still waiting for its answer
        emuDlg.shown = false;
        emuDlg.since = GetTickCount64();
    }
    PauseOnResume(); // the game goes on once it has the keyboard again (PauseTrack)
    if (take) FocusGame();
}

void ViewerReveal() {
    Log(L"revealed");
    const bool take = OursHasFocus();
    menuHiddenForGame = true;
    windowVisible = false;
    ViewShow(true);
    if (take) FocusGame();
}

void Reveal(HWND game);

// Once per pass of the main loop while an emulator runs (TrackGameWindow).
void ViewerTrack() {
    HWND game;
    {
        Slow slow{L"looking for the emulator window"};
        game = GameWindow();
    }
    const ULONGLONG now = GetTickCount64();
    {
        // The emulator recreates windows as it sets up; while it starts, its
        // taskbar buttons are taken away again every pass (as before), later
        // only a new window is dealt with.
        Slow slow{L"concealing the emulator's windows"};
        if (!menuHiddenForGame) ConcealAllGameWindows();
        else EnumWindows(AdoptNewProc, static_cast<LPARAM>(gamePid));
    }
    ViewerFit(game);
    TrackEmuDialog(now);
    PauseTrack(now);

    HWND surface = RenderSurface(game);
    if (menuHiddenForGame) {
        // Emulation stopped inside the emulator leaves no picture: the game
        // counts as closed. (The yuzu family takes its picture away for a
        // moment when it rebuilds its shaders in the middle of a game: more
        // patience there. And not while a dialog of its says why.)
        if (!gameReadyChild || !game) {
        } else if (surface || emuDlg.wnd) {
            gameGoneSince = 0;
        } else if (!gameGoneSince) {
            gameGoneSince = now;
        } else if (now - gameGoneSince > (gameReadyChild[0] == L'>' ? 8000u : 2500u)) {
            CloseGame();
        }
        // The emulator has left its full-screen mode (only a key posted
        // straight to its window can do that: the keyboard's are blocked).
        // The picture is still only its render surface, scaled; it is asked to
        // go back so that the picture is the full-size one again.
        RECT top, s;
        const bool framed = game && (GetWindowLongW(game, GWL_STYLE) & WS_CAPTION);
        if (!framed) {
            if (fullKeyAt && now > fullKeyAt + 5000) fullKeyTries = 0; // a new episode, not our own key bouncing
            if (fullKeyTries == 0) fullKeyAt = 0;
        } else if (surface && GetWindowRect(game, &top) && GetWindowRect(surface, &s) && now >= fullKeyAt) {
            // (The first time round only notes it, since the change may still
            // be in progress; the emulators also ignore a second full-screen
            // key that follows the first too closely.)
            if (fullKeyAt && fullKeyTries < 3) {
                fullKeyTries++;
                Log(L"viewer: the emulator is not in full screen (surface %ldx%ld in a window of %ldx%ld): asking it back", s.right - s.left,
                    s.bottom - s.top, top.right - top.left, top.bottom - top.top);
                PostMessageW(game, WM_KEYDOWN, VK_F11, 0x00570001);
                PostMessageW(game, WM_KEYUP, VK_F11, 0xC0570001);
            }
            fullKeyAt = now + (fullKeyAt ? 700 : 400);
        }
    } else if (!gameWindowSeen) {
        if (game) {
            gameWindowSeen = now;
            Log(L"emulator window found");
        }
    } else {
        // The emulator can show its render surface for a moment while it is
        // still setting up, seconds before the game draws (seen in about one
        // start out of five). Only a surface that stays on screen is the game.
        constexpr ULONGLONG kSettleMs = 700;
        if (!surface) {
            if (renderSince) Log(L"render surface gone again after %llu ms", now - renderSince);
            renderSince = 0;
        } else if (!renderSince) {
            renderSince = now;
            Log(L"render surface on screen");
        }
        const bool ready = gameReadyChild ? renderSince && now - renderSince >= kSettleMs : now - gameWindowSeen > 3000;
        if (ready && !gameReadyAt) Log(L"ready: game is rendering");
        // The console fades its loading mark out over 12 frames before the
        // game's picture appears; the picture is shown once that has finished.
        if (ready && !gameReadyAt) {
            gameReadyAt = now;
            ui::OnGameReady();
        } else if (ready && now - gameReadyAt >= 220) {
            Reveal(game);
        }
    }
    GuardGame();
    ViewerUpdate();
}

// Messages of our window that the viewer answers. True: `out` is the result.
bool ViewerMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) {
    out = 0;
    if (msg == WM_EMU_DIALOG) {
        OnDialogRead(reinterpret_cast<DialogText*>(lp));
        return true;
    }
    if (msg == WM_EMU_PAUSED) {
        OnPauseDone(reinterpret_cast<PauseDone*>(lp));
        return true;
    }
    if (msg == WM_EMU_NOTE) {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        Log(L"%s", line->c_str());
        return true;
    }
    if (msg == WM_HOTKEY && wp >= HOTKEY_BLOCK && wp < HOTKEY_BLOCK + ARRAYSIZE(kBlockKeys)) {
        // A key the emulator must not get. Two of them mean something here.
        const char does = kBlockKeys[wp - HOTKEY_BLOCK].does;
        if (does == 'f') {
            cfg.fullscreen = !fullscreen;
            ApplyFullscreen(cfg.fullscreen);
            SaveConfig();
        } else if (does == 'h' && !LaunchCovering()) {
            ShowMenu(true);
            ui::OnHomePressed();
        } else if (does == 'q') {
            Log(L"Alt+F4: ending NSW-Home");
            quit = true;
        }
        return true;
    }
    if (msg == WM_SIZE) viewBlank = false; // drawn again at the new size
    if (!GameShowing()) return false;
    switch (msg) {
    case WM_MOUSEACTIVATE: {
        // A click on the game's picture does nothing at all: it neither takes
        // the keyboard from the game nor reaches anything of the emulator's.
        DWORD pid = 0;
        if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
        if (LOWORD(lp) == HTCLIENT && pid == gamePid) {
            out = MA_NOACTIVATEANDEAT;
            return true;
        }
        return false;
    }
    case WM_KEYDOWN:
        // The moment before the game has the keyboard back: nothing here may
        // act on the menu, which is not on screen. (F11 is our own full screen.)
        return wp != VK_F11;
    case WM_KEYUP:
    case WM_CHAR: return true;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEWHEEL:
        // A press begun on the menu must not keep the mouse from the game
        // (letting go of it ends the menu's drag: WM_CAPTURECHANGED).
        if (GetCapture() == hwnd) ReleaseCapture();
        return true;
    }
    return false;
}

void Reveal(HWND game) {
    if (viewing) {
        ViewerReveal();
        return;
    }
    Log(L"revealed");
    menuHiddenForGame = true;
    windowVisible = false; // the menu stops drawing; its window stays as the game's owner
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowGameWindow(game);
    if (game && OursHasFocus()) SetForegroundWindow(game);
}

// ---- PC games -------------------------------------------------------------
// A PC game (a native port: an ordinary program) goes through the same
// sequence as an emulator, with these differences. None of it could be tried
// with a real game window when it was written: see the notes on each.
//   - It is started as it is, from its own folder, with no arguments but the
//     ones written by hand into its config entry.
//   - There is no "the game is drawing" signal. The loading screen goes once
//     the program's main window has stayed on screen, not minimised and at
//     one size, for kPcSettleMs: a window that is still being set up (a
//     splash screen giving way to the real one, a window growing to full
//     screen) starts the count again. The emulators' rule without a signal is
//     three seconds from the first window; this is shorter and asks for more.
//   - Its window is left where the game puts it: the emulators' move to the
//     menu's screen also resizes frameless windows, which would stretch a
//     splash screen.
//   - "Close" asks it to close as its own close button would (WM_CLOSE to the
//     main window) and only ends the process if it is still there after
//     kPcCloseMs, since a native game may be saving. A second "close" in that
//     time ends it at once.
// Known limits: only windows of the process that was started are looked at, so
// a program that merely starts another and ends (a launcher) reads as a game
// that closed; a game in exclusive full screen may draw over the loading screen.
constexpr ULONGLONG kPcSettleMs = 700, kPcCloseMs = 5000;
ULONGLONG pcCloseAt = 0; // tick when it was asked to close; 0: it was not
HWND pcSeen = nullptr;   // the window being timed, its size, and since when it has been like that
SIZE pcSize = {};
ULONGLONG pcSince = 0;

struct PcFind {
    DWORD pid;
    bool hidden;
    HWND best;
    LONG area;
};

// A process that belongs to the running PC game: the one started, or one it
// started (they share the game's job object), so that a launcher's game is found.
bool InPcGame(DWORD pid) {
    if (pid == gamePid) return true;
    if (!pid || !gameJob || !gamePc) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    BOOL in = FALSE;
    const bool ok = IsProcessInJob(h, gameJob, &in) && in;
    CloseHandle(h);
    return ok;
}

BOOL CALLBACK PcFindProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<PcFind*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT rc;
    if (!(pid == ctx->pid || InPcGame(pid)) || !LooksLikeMainWindow(h) || (!ctx->hidden && !IsWindowVisible(h)) ||
        !GetWindowRect(h, &rc))
        return TRUE;
    const LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > ctx->area) {
        ctx->best = h;
        ctx->area = area;
    }
    return TRUE;
}

// The game's largest window: a program may show a smaller one first.
// `hidden`: also one that is not showing (the menu hides the game's windows).
HWND PcWindow(bool hidden) {
    PcFind ctx = {gamePid, hidden, nullptr, 0};
    EnumWindows(PcFindProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// Whether a window shows one flat colour: a game's window before its first
// picture (blank white or black while it loads its renderer). Looked at a few
// times a second on a small grid of its pixels, through the same window
// reading as the captures.
static bool PcWindowFlat(HWND w) {
    RECT rc;
    POINT o = {0, 0};
    if (!GetClientRect(w, &rc) || !ClientToScreen(w, &o) || rc.right < 64 || rc.bottom < 64) return true;
    OffsetRect(&rc, o.x, o.y);
    wincap::Picture p;
    if (!wincap::Grab(w, rc, false, p) || p.w < 8 || p.h < 8) return true;
    const uint8_t* first = nullptr;
    for (int gy = 1; gy < 16; gy++)
        for (int gx = 1; gx < 16; gx++) {
            const uint8_t* px = &p.bgra[(static_cast<size_t>(gy * p.h / 16) * p.w + gx * p.w / 16) * 4];
            if (!first) first = px;
            else if (abs(px[0] - first[0]) + abs(px[1] - first[1]) + abs(px[2] - first[2]) > 24) return false;
        }
    return true;
}

bool PcSettled(HWND& game, ULONGLONG now) {
    HWND w = PcWindow(false);
    RECT rc = {};
    if (w) GetWindowRect(w, &rc);
    const SIZE size = {rc.right - rc.left, rc.bottom - rc.top};
    if (!w || IsIconic(w) || w != pcSeen || size.cx != pcSize.cx || size.cy != pcSize.cy) {
        pcSeen = w;
        pcSize = size;
        pcSince = now;
    }
    if (w) gameHwnd = game = w; // what is shown when the wait ends (or is cut short with Y)
    if (!w || now - pcSince < kPcSettleMs) return false;
    // A window of one flat colour is a game still loading: the launch screen
    // stays until it shows a picture (at most 60 s, for a game that is
    // really meant to start on a plain screen).
    static ULONGLONG lookedAt = 0;
    static bool flat = true;
    if (now - pcSince > 60000) return true;
    if (now - lookedAt >= 400) {
        lookedAt = now;
        flat = PcWindowFlat(w);
        if (!flat) Log(L"the game's window shows a picture");
    }
    return !flat;
}

// What starts `game`: the program, its whole command line and the folder it
// runs in. For a dump that is an emulator (`def`), which is also prepared for
// the start unless `dry` (a test run that only wants to know the command).
// Returns an error code, "" when there is something to start.
std::string GameCommand(const Game& game, bool dry, const EmuDef*& def, std::wstring& exe, std::wstring& cmd,
                        std::wstring& cwd) {
    def = nullptr;
    if (game.pc) {
        exe = game.file;
        if (!dry && GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return "PROGRAM_MISSING";
        cmd = L"\"" + exe + L"\"" + (game.args.empty() ? L"" : L" " + game.args);
    } else {
        std::string emuId = cfg.defaultEmu;
        auto per = cfg.perGame.find(game.id);
        if (per != cfg.perGame.end() && cfg.emulators.count(per->second)) emuId = per->second;
        if (!cfg.emulators.count(emuId)) {
            if (cfg.emulators.empty()) return "NO_EMULATOR";
            emuId = cfg.emulators.begin()->first;
        }
        def = FindEmu(emuId);
        exe = cfg.emulators[emuId];
        if (!def || (!dry && GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)) return "EMULATOR_MISSING";
        if (!dry) {
            RECT screen = TargetMonitor();
            PrepareEmulator(emuId, screen.left, screen.top);
            int number = 0, dpi = 96;
            QtScreen(screen, number, dpi);
            PlaceEmulatorWindow(emuId, screen.left, screen.top, MulDiv(screen.right - screen.left, 96, dpi), number);
        }
        cmd = L"\"" + exe + L"\" " + EmuLaunchArgs(emuId, game.file);
    }
    cwd = exe.substr(0, exe.find_last_of(L"\\/"));
    return {};
}

// The launch screen stays on top of the emulator, hiding its window, loading
// bar and start-up chatter, until the game itself is drawing.
void TrackGameWindow() {
    if (!gameProc) return;
    if (pcCloseAt && GetTickCount64() - pcCloseAt > kPcCloseMs) { // asked to close and still there
        pcCloseAt = 0;
        Log(L"did not close when asked: ended");
        if (gameJob) TerminateJobObject(gameJob, 0);
        else TerminateProcess(gameProc, 0);
    }
    if (viewing) {
        ViewerTrack();
        return;
    }
    HWND game;
    {
        Slow slow{L"looking for the emulator window"};
        game = GameWindow();
    }
    ULONGLONG now = GetTickCount64();

    if (menuHiddenForGame) {
        // Stopping emulation from inside the emulator would leave its own
        // interface on screen; treat a vanished game picture as "closed".
        // (Not while the menu is up: the game window is hidden on purpose.)
        if (windowVisible) {
            HideAllGameWindows();
            gameGoneSince = 0;
            return;
        }
        if (!gameReadyChild || !game) return;
        // (The yuzu family puts its loading screen back for a moment when it
        // rebuilds its shaders in the middle of a game: more patience there.)
        if (GameRendering(game)) gameGoneSince = 0;
        else if (!gameGoneSince) gameGoneSince = now;
        else if (now - gameGoneSince > (gameReadyChild[0] == L'>' ? 8000u : 2500u)) CloseGame();
        return;
    }

    // Keep the emulator off the taskbar while it starts; it recreates windows
    // as it sets up, so this repeats every pass on top of the show hook.
    {
        Slow slow{L"concealing the emulator's windows"};
        ConcealAllGameWindows();
    }
    {
        Slow slow{L"moving the emulator window to the menu's screen"};
        if (!gamePc) PlaceOnMenuScreen(game);
    }
    if (!gameWindowSeen) {
        if (game) {
            gameWindowSeen = now;
            Log(L"emulator window found");
        }
        return;
    }
    // Without a render-surface signal, give the emulator a moment and show it.
    bool ready = gamePc ? PcSettled(game, now) : now - gameWindowSeen > 3000;
    if (gameReadyChild) {
        // The emulator can show its render surface for a moment while it is
        // still setting up, seconds before the game draws (seen in about one
        // start out of five). Only a surface that stays on screen is the game.
        constexpr ULONGLONG kSettleMs = 700;
        if (!GameRendering(game)) {
            if (renderSince) Log(L"render surface gone again after %llu ms", now - renderSince);
            renderSince = 0;
        } else if (!renderSince) {
            renderSince = now;
            Log(L"render surface on screen");
        }
        ready = renderSince && now - renderSince >= kSettleMs;
    }
    if (ready && !gameReadyAt) Log(L"ready: game is rendering");
    if (!ready) return;
    // The console fades its loading mark out over 12 frames before the game's
    // picture appears; the window is shown once that has finished.
    if (!gameReadyAt) {
        gameReadyAt = now;
        ui::OnGameReady();
    } else if (now - gameReadyAt >= 220) {
        Reveal(game);
    }
}

// ---- capture ----------------------------------------------------------------
// NSW-Home's own capture button: a picture of the game, whatever is running it.

// A game (not the menu, not the launch screen) is what the player is looking at.
bool GameInFront() {
    return gameProc && GameMayShow();
}

// Which rectangle of which window is the game's picture: `top` is the
// top-level window that holds it, `rc` the picture's place in screen pixels.
// For an emulator that is its render surface alone, without the menu and
// status bars its window may have round it; for anything else the window's
// client area.
bool GamePicture(HWND& top, RECT& rc) {
    top = GameWindow();
    if (!top || IsIconic(top)) return false;
    if (gameReadyChild) {
        const bool deep = gameReadyChild[0] == L'>';
        ChildCtx ctx = {gameReadyChild + (deep ? 1 : 0), false, top, deep};
        EnumChildWindows(top, ChildProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.hit && GetWindowRect(ctx.hit, &rc) && rc.right > rc.left && rc.bottom > rc.top) return true;
    }
    POINT origin = {0, 0};
    return GetClientRect(top, &rc) && ClientToScreen(top, &origin) && OffsetRect(&rc, origin.x, origin.y) &&
           rc.right > rc.left && rc.bottom > rc.top;
}

// cfg.captureKey ("Ctrl+F9", "PrintScreen"...) as RegisterHotKey wants it.
bool ParseCaptureKey(std::string s, UINT& mods, UINT& vk) {
    mods = MOD_NOREPEAT;
    vk = 0;
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (size_t plus; (plus = s.find('+')) != std::string::npos && plus + 1 < s.size(); s.erase(0, plus + 1)) {
        const std::string m = s.substr(0, plus);
        if (m == "ctrl") mods |= MOD_CONTROL;
        else if (m == "alt") mods |= MOD_ALT;
        else if (m == "shift") mods |= MOD_SHIFT;
        else return false;
    }
    static const struct {
        const char* name;
        UINT vk;
    } named[] = {{"printscreen", VK_SNAPSHOT}, {"pause", VK_PAUSE}, {"insert", VK_INSERT}, {"scrolllock", VK_SCROLL}};
    for (const auto& n : named)
        if (s == n.name) vk = n.vk;
    if (!vk && s.size() == 1 && isalnum(static_cast<unsigned char>(s[0]))) vk = static_cast<UINT>(toupper(s[0]));
    const int f = s.size() >= 2 && s[0] == 'f' ? atoi(s.c_str() + 1) : 0;
    // (F12 belongs to the debugger: Windows does not hand it out reliably.)
    if (!vk && f >= 1 && f <= 24 && f != 12) vk = VK_F1 + f - 1;
    return vk != 0;
}

// The capture key is the system's only while a game is in front: with the
// menu up (or no game at all) the key does whatever it does in Windows.
// Looked at on every pass of the main loop instead of at each place the game
// comes and goes, so it cannot be left registered by a path that forgot it.
void SyncCaptureKey() {
    static bool on = false, tried = false;
    static std::string key;
    const bool want = !simulate && GameInFront() && !cfg.captureKey.empty();
    if (on && (!want || key != cfg.captureKey)) {
        UnregisterHotKey(hwnd, HOTKEY_CAPTURE);
        on = false;
    }
    if (!want) tried = false;
    if (!want || on || (tried && key == cfg.captureKey)) return;
    tried = true; // once per time the game comes to the front: a key someone else holds stays theirs
    key = cfg.captureKey;
    UINT mods = 0, vk = 0;
    on = ParseCaptureKey(key, mods, vk) && RegisterHotKey(hwnd, HOTKEY_CAPTURE, mods, vk);
    Log(L"capture key %hs: %s", key.c_str(), on ? L"registered" : L"NOT available");
}

std::atomic<bool> capturing{false};
RECT captureRect = {}; // where the game's picture was when the capture was asked for

void NoticeSaved() {
    ui::DrawCaptureNotice(true);
}
void NoticeFailed() {
    ui::DrawCaptureNotice(false);
}

// The capture is on disk (or could not be made): the sound, and the notice
// in the top left corner of the game's picture, scaled as the menu would be
// on that screen. It is a window of its own because the game's is another
// program's; it never takes the focus (see wincap::ShowNotice).
void OnCaptureDone(bool ok, const char* method) {
    capturing = false;
    Log(L"capture %s (%hs)", ok ? L"saved" : L"FAILED", method ? method : "");
    PlaySfx(ok ? Sfx::Capture : Sfx::Error);
    const float unit = (std::min)((captureRect.right - captureRect.left) / gfx::W, (captureRect.bottom - captureRect.top) / gfx::H);
    float w = 0, h = 0;
    ui::CaptureNoticeSize(ok, w, h);
    const int pw = static_cast<int>(ceilf(w * unit)), ph = static_cast<int>(ceilf(h * unit));
    std::vector<uint8_t> px;
    if (unit <= 0 || !gfx::Snapshot(pw, ph, unit, ok ? NoticeSaved : NoticeFailed, px)) return;
    wincap::ShowNotice(px.data(), pw, ph, captureRect.left + static_cast<int>(24 * unit),
                       captureRect.top + static_cast<int>(24 * unit), 2200);
}

// ---- input ----------------------------------------------------------------

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn xinputGet = nullptr;
bool padConnected[4] = {};
constexpr WORD kGuide = 0x0400; // only reported by the undocumented ordinal 100

void InitXInput() {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) return;
    // Ordinal 100 is XInputGetStateEx, which also reports the Guide button.
    xinputGet = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, MAKEINTRESOURCEA(100)));
    if (!xinputGet) xinputGet = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
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
Repeat dirRepeat[4];       // Up, Down, Left, Right
bool keyHeld[4] = {};      // arrow keys currently held (keyboard)
const int kDirKey[4] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
const Btn kDirBtn[4] = {Btn::Up, Btn::Down, Btn::Left, Btn::Right};

double NowMs() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// First press of a direction: act at once and arm the repeat.
void DirectionDown(int i) {
    dirRepeat[i] = {true, NowMs() + kRepeatDelayMs};
    ui::OnButton(kDirBtn[i]);
}

void PollPads() {
    static WORD prev = 0;
    static ULONGLONG lastScan = 0;
    static bool present[4] = {true, true, true, true};

    // Asking for an unplugged slot is slow, so empty slots are only re-checked
    // once a second.
    ULONGLONG tick = GetTickCount64();
    bool rescan = tick - lastScan > 1000;
    if (rescan) lastScan = tick;

    WORD buttons = 0;
    float lx = 0, ly = 0;
    float rx = 0, ry = 0; // the right stick: only the web page uses it (it scrolls there)
    for (DWORD i = 0; xinputGet && i < 4; i++) {
        if (!present[i] && !rescan) continue;
        XINPUT_STATE st = {};
        present[i] = xinputGet(i, &st) == ERROR_SUCCESS;
        padConnected[i] = present[i];
        pads::SetXInput(static_cast<int>(i), present[i], st.Gamepad.wButtons, st.Gamepad.sThumbLX / 32767.0f, st.Gamepad.sThumbLY / 32767.0f);
        pads::SetXInputMore(static_cast<int>(i), st.Gamepad.sThumbRX / 32767.0f, st.Gamepad.sThumbRY / 32767.0f,
                            st.Gamepad.bLeftTrigger / 255.0f, st.Gamepad.bRightTrigger / 255.0f);
        if (!present[i]) continue;
        buttons |= st.Gamepad.wButtons;
        if (abs(st.Gamepad.sThumbLX) > abs(static_cast<int>(lx * 32767))) lx = st.Gamepad.sThumbLX / 32767.0f;
        if (abs(st.Gamepad.sThumbLY) > abs(static_cast<int>(ly * 32767))) ly = st.Gamepad.sThumbLY / 32767.0f;
        if (abs(st.Gamepad.sThumbRX) > abs(static_cast<int>(rx * 32767))) rx = st.Gamepad.sThumbRX / 32767.0f;
        if (abs(st.Gamepad.sThumbRY) > abs(static_cast<int>(ry * 32767))) ry = st.Gamepad.sThumbRY / 32767.0f;
    }

    // The pads NSW-Home reads itself (pads.h) join in here, as one more
    // XInput-style word in which a pad that XInput also shows acts once.
    const pads::Menu merged = pads::Poll(cfg.swapAB);
    buttons = merged.buttons, lx = merged.lx, ly = merged.ly;
    if (merged.capture) RequestCapture();

    WORD pressed = buttons & ~prev;
    prev = buttons;

    // While a game has the screen, the pad belongs to the game; only the
    // Guide button is ours.
    if (!windowVisible) {
        if (pressed & kGuide) {
            ShowMenu(true);
            ui::OnHomePressed();
        }
        return;
    }
    // The launch screen keeps taking input even though the emulator window
    // under it may hold the focus.
    bool focused = GetForegroundWindow() == hwnd;
    if (!focused && !LaunchCovering()) {
        for (int i = 0; i < 4; i++) dirRepeat[i].down = keyHeld[i] = false;
        return;
    }
    // On the web page the sticks move the pointer and scroll, so the left
    // one is not also read as the direction buttons.
    if (web::Pad(lx, ly, rx, ry)) lx = ly = 0;

    bool padDir[4] = {
        (buttons & XINPUT_GAMEPAD_DPAD_UP) || ly > 0.6f,
        (buttons & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -0.6f,
        (buttons & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -0.6f,
        (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > 0.6f,
    };
    double now = NowMs();
    for (int i = 0; i < 4; i++) {
        // A key press that arrived as a message counts as held only while the
        // key really is down (a posted test key never gets its key-up).
        if (keyHeld[i] && !(focused && (GetAsyncKeyState(kDirKey[i]) & 0x8000))) keyHeld[i] = false;
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
    if (pressed & XINPUT_GAMEPAD_A) ui::OnButton(swap ? Btn::B : Btn::A);
    if (pressed & XINPUT_GAMEPAD_B) ui::OnButton(swap ? Btn::A : Btn::B);
    if (pressed & XINPUT_GAMEPAD_X) ui::OnButton(swap ? Btn::Y : Btn::X);
    if (pressed & XINPUT_GAMEPAD_Y) ui::OnButton(swap ? Btn::X : Btn::Y);
    if (pressed & XINPUT_GAMEPAD_START) ui::OnButton(Btn::Plus);
    if (pressed & XINPUT_GAMEPAD_BACK) ui::OnButton(Btn::Minus);
    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ui::OnButton(Btn::L);
    if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) ui::OnButton(Btn::R);
    if (pressed & kGuide) ui::OnHomePressed();
}

bool swallowChar = false; // the next character message belongs to the key that opened the text box

void OnKey(WPARAM vk, bool autoRepeat) {
    if (ui::WantsText()) {
        // Text entry gets its characters through WM_CHAR.
        return;
    }
    // Arrow keys repeat at the console's rate, not the keyboard's: Windows'
    // own repeats are dropped and PollPads times them instead.
    for (int i = 0; i < 4; i++) {
        if (vk != static_cast<WPARAM>(kDirKey[i])) continue;
        if (!autoRepeat) {
            keyHeld[i] = true;
            DirectionDown(i);
        }
        return;
    }
    if (autoRepeat) return; // a held button is one press
    switch (vk) {
    case VK_RETURN:
    case VK_SPACE: ui::OnButton(Btn::A); break;
    case VK_ESCAPE:
    case VK_BACK: ui::OnButton(Btn::B); break;
    case 'X': ui::OnButton(Btn::X); break;
    case 'Y': ui::OnButton(Btn::Y); break;
    case VK_TAB:
    case VK_OEM_PLUS:
    case VK_ADD: ui::OnButton(Btn::Plus); break;
    case VK_OEM_MINUS:
    case VK_SUBTRACT: ui::OnButton(Btn::Minus); break;
    case VK_PRIOR: ui::OnButton(Btn::L); break;
    case VK_NEXT: ui::OnButton(Btn::R); break;
    case VK_HOME: ui::OnHomePressed(); break;
    case VK_F11:
        cfg.fullscreen = !fullscreen;
        ApplyFullscreen(cfg.fullscreen);
        SaveConfig();
        break;
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT viewerResult = 0;
    if (ViewerMessage(msg, wp, lp, viewerResult)) return viewerResult;
    switch (msg) {
    case WM_SIZE:
        gfx::Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_ACTIVATE:
        // The menu's taskbar button stands for the whole console: while a game
        // is on screen, activating it means "go to the game".
        // (With an emulator the main loop does this, see GuardGame.)
        if (LOWORD(wp) != WA_INACTIVE && gameProc && menuHiddenForGame && !windowVisible && !viewing) {
            if (HWND game = GameWindow()) SetForegroundWindow(game);
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(h, nullptr);
        return 0;
    case WM_KEYDOWN: {
        // A key that opens a text box (Enter on "change nickname") also sends
        // its character; that character belongs to the press, not to the box.
        bool typing = ui::WantsText();
        OnKey(wp, (lp & (1 << 30)) != 0);
        if (!typing && ui::WantsText()) swallowChar = true;
        return 0;
    }
    case WM_KEYUP:
        swallowChar = false;
        for (int i = 0; i < 4; i++)
            if (wp == static_cast<WPARAM>(kDirKey[i])) keyHeld[i] = false;
        return 0;
    case WM_CHAR:
        if (swallowChar) swallowChar = false;
        else if (ui::WantsText()) ui::OnChar(static_cast<wchar_t>(wp));
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: {
        float x, y;
        gfx::ToDesign(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)), x, y);
        if (msg == WM_LBUTTONDOWN) SetCapture(h); // a drag that leaves the window still comes back to end
        ui::OnMouse(x, y, msg == WM_LBUTTONDOWN);
        return 0;
    }
    case WM_LBUTTONUP: {
        float x, y;
        gfx::ToDesign(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)), x, y);
        ui::OnMouseUp(x, y);
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    }
    case WM_CAPTURECHANGED:
        // The mouse taken away while held (another window came up): the press
        // ends there, far from where it began, so it is no click.
        if (reinterpret_cast<HWND>(lp) != h) ui::OnMouseUp(-1e4f, -1e4f);
        return 0;
    case WM_MOUSEWHEEL:
        ui::OnWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && fullscreen) {
            // Keep the pointer out of the way until the mouse is actually used.
            SetCursor(ui::MouseActive() ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
            return TRUE;
        }
        break;
    case WM_HOTKEY:
        if (wp == HOTKEY_HOME) {
            ShowMenu(true);
            ui::OnHomePressed();
        } else if (wp == HOTKEY_CAPTURE) {
            RequestCapture();
        }
        return 0;
    case WM_CAPTURE_DONE:
        OnCaptureDone(wp != 0, reinterpret_cast<const char*>(lp));
        return 0;
    case WM_SCAN_DONE: {
        auto* result = reinterpret_cast<std::vector<Game>*>(lp);
        games = std::move(*result);
        delete result;
        scanning = false;
        ui::OnLibraryChanged();
        return 0;
    }
    case WM_CLOSE:
        quit = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

std::wstring PickWithDialog(bool folder, bool image = false, bool keys = false) {
    std::wstring out;
    if (simulate) return out; // offscreen test run: nothing may open on the desktop
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder) {
        COMDLG_FILTERSPEC spec = {L"Programas", L"*.exe"};
        if (image) spec = {L"Imágenes", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif"};
        if (keys) spec = {L"Claves de la consola", L"*.keys"};
        dlg->SetFileTypes(1, &spec);
    }
    if (SUCCEEDED(dlg->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

} // namespace

// ---- services for the UI --------------------------------------------------

void PlaySfx(Sfx s) {
    if (!cfg.sounds) return;
    const auto& w = a11y::Mono(sfx[static_cast<int>(s)]);
    if (!w.empty()) PlaySoundW(reinterpret_cast<LPCWSTR>(w.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

std::string LaunchGame(const std::string& id) {
    const Game* game = nullptr;
    for (const auto& g : games)
        if (g.id == id) game = &g;
    const EmuDef* def = nullptr;
    std::wstring exe, cmd, cwd;
    if (simulate) { // offscreen test run: pretend it started
        // (--log: what would have been run, for the tests of who a game starts as)
        if (game && GameCommand(*game, true, def, exe, cmd, cwd).empty()) Log(L"would launch: %s", cmd.c_str());
        runningId = id;
        return {};
    }
    if (gameProc) return "RUNNING";
    if (!game) return "NO_GAME";
    const std::string err = GameCommand(*game, false, def, exe, cmd, cwd);
    if (!err.empty()) return err;
    Log(L"launch %hs: %s", def ? def->id : "pc", cmd.c_str());

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, cwd.c_str(),
                        &si, &pi))
        return "SPAWN";
    // The job lets "close software" end the emulator together with anything
    // it started.
    gameJob = CreateJobObjectW(nullptr, nullptr);
    if (gameJob && !AssignProcessToJobObject(gameJob, pi.hProcess)) {
        CloseHandle(gameJob);
        gameJob = nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    gameProc = pi.hProcess;
    gamePid = pi.dwProcessId;
    pcsys::GameStarted(gamePid); // the headphone rows of Settings > Sonido act on this game's sound
    showHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnEmulatorShow, gamePid, 0,
                               WINEVENT_OUTOFCONTEXT);
    gameStart = GetTickCount64();
    gameWindowSeen = 0;
    gameReadyAt = 0;
    renderSince = 0;
    menuHiddenForGame = false;
    gameReadyChild = def ? def->readyChild : nullptr;
    gamePc = game->pc;
    pcCloseAt = pcSince = 0;
    pcSeen = nullptr;
    gameGoneSince = 0;
    closeAsked = false;
    if (!relaunching) launchTries = 0;
    runningId = id;
    if (def) ViewerGameStarted(def->family); // an emulator: its window is never shown, only its picture (see "game viewer")
    // Stay above the emulator until the game is ready.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RegisterHotKey(hwnd, HOTKEY_HOME, MOD_NOREPEAT, VK_HOME);
    return {};
}

void ResumeGame() {
    if (!gameProc) return;
    HWND g = GameWindow();
    if (!g) return;
    if (viewing) {
        ViewerResume();
        return;
    }
    Log(L"resume");
    windowVisible = false;
    bool take = OursHasFocus();
    ShowGameWindow(g);
    if (take) SetForegroundWindow(g);
}

bool LaunchCovering() {
    return gameProc && !menuHiddenForGame;
}

void RevealGame() {
    if (LaunchCovering()) Reveal(GameWindow());
}

void CloseGame() {
    if (simulate && !runningId.empty()) {
        std::string id = runningId;
        runningId.clear();
        ui::OnGameExit(id, false);
        return;
    }
    if (!gameProc) return;
    Log(L"close requested");
    closeAsked = true;
    if (gamePc && !pcCloseAt) { // asked first, ended later if need be: see "PC games"
        HWND w = GameWindow();
        if (!w) w = PcWindow(true);
        if (w && PostMessageW(w, WM_CLOSE, 0, 0)) {
            pcCloseAt = GetTickCount64();
            return;
        }
    }
    if (gameJob) TerminateJobObject(gameJob, 0);
    else TerminateProcess(gameProc, 0);
}

void RescanAsync() {
    if (scanning) return;
    if (simulate) { // offscreen test run: no window to post the result to
        games = ScanLibrary();
        ui::OnLibraryChanged();
        return;
    }
    scanning = true;
    std::thread([] {
        auto* result = new std::vector<Game>(ScanLibrary());
        PostMessageW(hwnd, WM_SCAN_DONE, 0, reinterpret_cast<LPARAM>(result));
    }).detach();
}

void SetFullscreen(bool on) {
    ApplyFullscreen(on);
}
int MonitorCount() {
    return static_cast<int>(Monitors().size());
}
void ApplyMonitor() {
    RECT m = TargetMonitor();
    if (fullscreen)
        SetWindowPos(hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOOWNERZORDER);
    else
        CenterWindowed(m);
}
void QuitApp() {
    quit = true;
}
void ApplyTitle() {
    if (hwnd) SetWindowTextW(hwnd, cfg.title.empty() ? L"NSW-Home" : cfg.title.c_str());
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
    if (simulate) return; // offscreen test run: nothing may open on the desktop
    std::wstring arg = L"/select,\"" + file + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}
std::wstring ClipboardText() {
    std::wstring out;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out.assign(p, wcsnlen(p, 4096));
            GlobalUnlock(h);
        }
    CloseClipboard();
    return out;
}
bool OpenUrl(const std::wstring& url) {
    if (simulate) return true; // offscreen test run: nothing may open on the desktop
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
// OBS Studio's installer (and its Steam copy) records its folder as the
// default value of HKLM\SOFTWARE\OBS Studio; the program is bin\64bit\obs64.exe
// there, and it must be started from that folder or it cannot find its files.
int OpenObs() {
    if (simulate) return 1; // offscreen test run: nothing may start
    wchar_t dir[MAX_PATH] = {};
    DWORD size = sizeof dir;
    bool found = false;
    for (const wchar_t* key : {L"SOFTWARE\\OBS Studio", L"SOFTWARE\\WOW6432Node\\OBS Studio"}) {
        size = sizeof dir;
        if (!found && RegGetValueW(HKEY_LOCAL_MACHINE, key, nullptr, RRF_RT_REG_SZ, nullptr, dir, &size) == ERROR_SUCCESS)
            found = dir[0] != 0;
    }
    std::wstring bin = std::wstring(dir) + L"\\bin\\64bit";
    if (!found) bin = L"C:\\Program Files\\obs-studio\\bin\\64bit"; // the installer's default
    const std::wstring exe = bin + L"\\obs64.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    // One already open is brought forward instead of starting a second one
    // (OBS asks before running twice).
    struct Find {
        std::wstring exe;
        HWND hit = nullptr;
    } find{exe};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) return TRUE;
            wchar_t path[MAX_PATH] = {};
            DWORD n = MAX_PATH;
            const bool same = QueryFullProcessImageNameW(h, 0, path, &n) && !_wcsicmp(path, f->exe.c_str());
            CloseHandle(h);
            if (same) f->hit = w;
            return same ? FALSE : TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    if (find.hit) {
        ShowWindow(find.hit, IsIconic(find.hit) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(find.hit);
        return 1;
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, bin.c_str(), SW_SHOWNORMAL)) > 32 ? 1 : -1;
}
bool OpenFolder(const std::wstring& dir) {
    if (simulate) return true; // offscreen test run: nothing may open on the desktop
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
std::vector<bool> ConnectedPads() {
    return {padConnected[0], padConnected[1], padConnected[2], padConnected[3]};
}

// For the amiibo hand-over (amiibo_emu.cpp): the emulator shown through the game viewer.
bool amiibo::EmuLink(uint32_t& pid, void*& emuWindow, void*& menuWindow, char& family) {
    if (!viewing || !gameProc || !gameHwnd || !IsWindow(gameHwnd)) return false;
    pid = gamePid, emuWindow = gameHwnd, menuWindow = hwnd, family = viewFamily;
    return true;
}

bool RequestCapture() {
    HWND top = nullptr;
    RECT rc = {};
    if (simulate || !GameInFront() || !GamePicture(top, rc)) return false;
    if (capturing.exchange(true)) return false;
    captureRect = rc;
    wincap::HideNotice(); // the last capture's notice must not be in this one
    const std::wstring file = NewCaptureFile(runningId);
    const HWND menu = hwnd;
    Log(L"capture asked: %s", file.c_str());
    // Reading the window and packing the JPEG take tens of milliseconds, and
    // the first waits on the compositor: not on the thread the menu runs on.
    std::thread([=] {
        wincap::Picture p;
        // The window's own picture first. Reading the screen instead is only
        // tried after a pause, so that the notice just removed is off it.
        bool ok = wincap::Grab(top, rc, false, p);
        if (!ok) {
            Sleep(50);
            ok = wincap::Grab(top, rc, true, p);
        }
        ok = ok && wincap::SaveJpeg(p, file);
        PostMessageW(menu, WM_CAPTURE_DONE, ok, reinterpret_cast<LPARAM>(p.method));
    }).detach();
    return true;
}

// The first name the icon groups of a program are listed under: the icon
// Windows itself shows for the file.
static BOOL CALLBACK FirstIconGroup(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR lp) {
    auto* out = reinterpret_cast<std::pair<bool, std::wstring>*>(lp);
    // (A number is carried as "#<number>", which FindResource reads back.)
    out->second = IS_INTRESOURCE(name) ? L"#" + std::to_wstring(reinterpret_cast<ULONG_PTR>(name)) : std::wstring(name);
    out->first = true;
    return FALSE;
}

bool ProgramInfo(const std::wstring& exe, std::wstring& name, const std::wstring& iconPng) {
    bool found = false;
    // The name: from the version block, in the first language it lists.
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    std::vector<BYTE> block(size);
    if (size && GetFileVersionInfoW(exe.c_str(), 0, size, block.data())) {
        struct Lang {
            WORD language, codePage;
        };
        void* value = nullptr;
        UINT len = 0;
        Lang lang = {0x0409, 1200};
        if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", &value, &len) && len >= sizeof(Lang))
            lang = *static_cast<Lang*>(value);
        for (const wchar_t* key : {L"ProductName", L"FileDescription"}) {
            wchar_t path[80];
            swprintf_s(path, L"\\StringFileInfo\\%04x%04x\\%s", lang.language, lang.codePage, key);
            if (found || !VerQueryValueW(block.data(), path, &value, &len) || !value || len < 2) continue;
            std::wstring text(static_cast<const wchar_t*>(value));
            while (!text.empty() && iswspace(text.back())) text.pop_back();
            while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
            if (text.empty()) continue;
            name = text;
            found = true;
        }
    }
    if (iconPng.empty()) return found;

    // The picture: the program's own icon at the largest size it carries, read
    // from the file as data (nothing of the program runs).
    HICON icon = nullptr;
    int side = 0;
    if (HMODULE mod = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        std::pair<bool, std::wstring> group;
        EnumResourceNamesW(mod, RT_GROUP_ICON, FirstIconGroup, reinterpret_cast<LONG_PTR>(&group));
        HRSRC dir = group.first ? FindResourceW(mod, group.second.c_str(), RT_GROUP_ICON) : nullptr;
        const BYTE* d = dir ? static_cast<const BYTE*>(LockResource(LoadResource(mod, dir))) : nullptr;
        const DWORD dirSize = dir ? SizeofResource(mod, dir) : 0;
        // An icon group: a 6-byte header (the count in its last two bytes),
        // then 14 bytes per picture: width and height (0 means 256), colours,
        // a spare byte, planes, bits per pixel, the size, and the picture's id.
        int best = -1, bestBits = 0;
        const int count = d && dirSize >= 6 ? d[4] | d[5] << 8 : 0;
        for (int i = 0; i < count && 6 + (i + 1) * 14 <= static_cast<int>(dirSize); i++) {
            const BYTE* e = d + 6 + i * 14;
            const int w = e[0] ? e[0] : 256, bits = e[6] | e[7] << 8;
            if (w > side || (w == side && bits > bestBits)) {
                side = w;
                bestBits = bits;
                best = e[12] | e[13] << 8;
            }
        }
        HRSRC res = best >= 0 ? FindResourceW(mod, MAKEINTRESOURCEW(best), RT_ICON) : nullptr;
        if (res)
            icon = CreateIconFromResourceEx(static_cast<PBYTE>(LockResource(LoadResource(mod, res))), SizeofResource(mod, res),
                                            TRUE, 0x00030000, side, side, LR_DEFAULTCOLOR);
        FreeLibrary(mod);
    }
    if (!icon) return found;
    // Painted on a plain dark ground: most icons are a shape on nothing, and a
    // tile is a full square. At its own size; the tile scales it when drawn.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = side;
    bi.bmiHeader.biHeight = -side;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    DWORD* px = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0);
    bool ok = dc && dib && px;
    if (ok) {
        HGDIOBJ old = SelectObject(dc, dib);
        for (int i = 0; i < side * side; i++) px[i] = 0xFF33363B;
        ok = DrawIconEx(dc, 0, 0, icon, side, side, 0, nullptr, DI_NORMAL) != 0;
        GdiFlush();
        for (int i = 0; i < side * side; i++) px[i] |= 0xFF000000; // GDI leaves the alpha of what it paints at zero
        SelectObject(dc, old);
    }
    IWICImagingFactory* wic = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const UINT n = static_cast<UINT>(side);
    ok = ok && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
         SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(iconPng.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(n, n)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WritePixels(n, n * 4, n * n * 4, reinterpret_cast<BYTE*>(px))) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (wic) wic->Release();
    if (dib) DeleteObject(dib);
    if (dc) DeleteDC(dc);
    DestroyIcon(icon);
    if (!ok) DeleteFileW(iconPng.c_str());
    return found || ok;
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

// Debug: render an animation offscreen, one PNG per frame at 60 frames a
// second, starting when A is pressed on the given screen. Used to lay the
// menu's motion next to a capture of the console's.
// One step of a --shotseq script: do `what` just before drawing frame `frame`.
struct ScriptStep {
    int frame;
    std::wstring what;
};

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
        ui::OnMouse(x, y, false); // the pointer gets there first, as a real press
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
// Steps: A B X Y UP DOWN LEFT RIGHT PLUS MINUS L R, plus HOME, RUN and EXIT (see RunStep).
// A direction followed by '+' (LEFT+@24) arrives as a repeat of a held key.
// Mouse steps, at positions in the 1280x720 layout's units (fractions allowed):
//   MDOWN:x:y  the left button pressed there     MMOVE:x:y  the pointer moved there
//   MUP:x:y    the button let go there            MCLICK:x:y press and release in place
//   WHEEL:d    the wheel turned d notches (positive: away from the user, up the list)
//   MDRAG:x0:y0:x1:y1:n[:hold]  press at x0,y0 on the step's frame, move evenly to
//              x1,y1 over the next n frames, stay still `hold` frames (default 0:
//              let go while moving, which throws the list) and let go on the next.
// E.g. "MDRAG:900:500:900:250:10@0" drags a list up 250 units in 10 frames and throws it.
static int RunShotSeq(const std::wstring& screen, const std::wstring& dir, int frames, const std::wstring& script) {
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
            const int f = _wtoi(item.c_str() + mark + 1);
            wchar_t s[64];
            swprintf_s(s, L"MDOWN:%g:%g", x0, y0);
            steps.push_back({f, s});
            for (int k = 1; k <= n; k++) {
                swprintf_s(s, L"MMOVE:%g:%g", x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n);
                steps.push_back({f + k, s});
            }
            swprintf_s(s, L"MUP:%g:%g", x1, y1);
            steps.push_back({f + n + 1 + (std::max)(0, hold), s});
        } else if (mark != std::wstring::npos) {
            steps.push_back({_wtoi(item.c_str() + mark + 1), item.substr(0, mark)});
        }
        if (comma == std::wstring::npos) break;
        at = comma + 1;
    }
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

// Debug (--capture-test, with --log): the whole capture path against a window
// that is not a game, without the menu ever showing: the top-level window of
// class NSWHomeProbe (tools\capture_probe.cpp's stand-in) is treated as a
// game in front, a capture is asked for as the key or a pad would, and the
// log says what was saved, whether the notice came up and went, and whether
// the foreground window stayed what it was. No sound is played.
static int RunCaptureTest(HINSTANCE inst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"NSWHomeCaptureTest";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    gameHwnd = FindWindowW(L"NSWHomeProbe", L"NSWHomeCaptureStandIn");
    if (!hwnd || !gameHwnd) return 2;
    games = ScanLibrary();
    cfg.sounds = false;
    runningId = games.empty() ? "test" : games[0].id;
    gameProc = GetCurrentProcess(); // only ever asked whether there is one
    menuHiddenForGame = true;
    windowVisible = false;
    const HWND before = GetForegroundWindow();
    Log(L"capture test: request %s", RequestCapture() ? L"accepted" : L"REFUSED");
    bool shown = false, pictured = false;
    ULONGLONG shownAt = 0;
    HWND during = nullptr;
    for (ULONGLONG end = GetTickCount64() + 3200; GetTickCount64() < end; Sleep(5)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (HWND n = wincap::NoticeWindow()) {
            if (!shown) {
                RECT r;
                GetWindowRect(n, &r);
                Log(L"capture test: notice at (%ld, %ld), %ld x %ld px", r.left, r.top, r.right - r.left, r.bottom - r.top);
            }
            if (!shown) shownAt = GetTickCount64();
            shown = true;
            during = GetForegroundWindow();
            // What the notice looks like over the window, once it has faded in:
            // read off the screen into notice_test.jpg in the data folder.
            if (!pictured && GetTickCount64() - shownAt > 500) {
                RECT r;
                GetWindowRect(n, &r);
                InflateRect(&r, 40, 40);
                HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
                HBITMAP bmp = CreateCompatibleBitmap(screen, r.right - r.left, r.bottom - r.top);
                HGDIOBJ old = SelectObject(mem, bmp);
                BitBlt(mem, 0, 0, r.right - r.left, r.bottom - r.top, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
                wincap::Picture p;
                p.w = r.right - r.left, p.h = r.bottom - r.top;
                p.bgra.resize(static_cast<size_t>(p.w) * p.h * 4);
                BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), p.w, -p.h, 1, 32}};
                GetDIBits(mem, bmp, 0, p.h, p.bgra.data(), &bi, DIB_RGB_COLORS);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
                ReleaseDC(nullptr, screen);
                wincap::SaveJpeg(p, DataDir() + L"\\notice_test.jpg");
                pictured = true;
            }
        }
    }
    Log(L"capture test: notice shown %d, gone %d; foreground the same during %d, after %d", shown, !wincap::NoticeWindow(),
        during == before, GetForegroundWindow() == before);
    gameProc = nullptr;
    DestroyWindow(hwnd);
    return 0;
}

// Debug (--applet-test <mode> [text], with --log): the pause and applet code
// above against tools\applet_standin.cpp's stand-in windows (class
// NSWHomeAppletStandIn, never activated, off screen and cloaked), with the
// menu never showing: a software keyboard / controller applet is found, read
// and shown as NSW-Home's entry exactly as during a game, `text` is typed into
// it (or it is backed out of, for a mode ending in "-cancel"), and the answer
// goes back; or, for a mode with "-pause" in it, the game is stopped as the
// menu coming up does and resumed as A on the game does. The stand-in's own
// log says what reached it. Nothing is ever brought to the front.
static int RunAppletTest(HINSTANCE inst, const std::wstring& mode, const std::wstring& typed) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"NSWHomeAppletTest";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    for (ULONGLONG end = GetTickCount64() + 4000; !gameHwnd && GetTickCount64() < end; Sleep(50))
        gameHwnd = FindWindowW(L"NSWHomeAppletStandIn", nullptr);
    if (!hwnd || !gameHwnd) return 2;
    GetWindowThreadProcessId(gameHwnd, &gamePid);
    gameProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, gamePid);
    appletTest = true;
    cfg.sounds = false;
    viewing = true;
    viewFamily = mode[0] == L'r' ? 'r' : 'y';
    menuHiddenForGame = true;
    gameReadyAt = 1;
    windowVisible = true; // the menu is up (the game is not showing): nothing to hand over
    const HWND before = GetForegroundWindow();
    auto pump = [](ULONGLONG ms, bool track) {
        for (ULONGLONG end = GetTickCount64() + ms; GetTickCount64() < end; Sleep(5)) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            if (track) PauseTrack(GetTickCount64());
        }
    };
    Log(L"applet test %s: stand-in \"%s\", pid %lu", mode.c_str(), TitleOf(gameHwnd).c_str(), gamePid);
    if (mode.find(L"-pause") != std::wstring::npos) {
        PauseOnMenu(true);
        pump(3500, true);
        Log(L"applet test: after the menu came up: %s; title \"%s\"", emuPause.paused ? L"stopped" : L"RUNNING", TitleOf(gameHwnd).c_str());
        PauseOnResume();
        pump(1500, true);
        Log(L"applet test: after going back: %s; title \"%s\"", emuPause.paused ? L"STOPPED" : L"running", TitleOf(gameHwnd).c_str());
    } else {
        HWND d = nullptr;
        for (ULONGLONG end = GetTickCount64() + 2000; !d && GetTickCount64() < end; Sleep(50)) d = EmulatorDialog();
        Log(L"applet test: dialog %s", d ? L"found" : L"NOT found");
        if (d) {
            emuDlg = EmuDialog();
            emuDlg.wnd = d;
            emuDlg.reading = true;
            std::thread(DialogWorker, d, std::wstring()).join(); // as TrackEmuDialog does (a thread of its own: COM there is not this thread's)
            pump(300, false);
            if (ui::WantsText()) {
                Log(L"applet test: NSW-Home's text entry is open; typing %zu characters", typed.size());
                for (int i = 0; i < 40; i++) ui::OnChar(L'\b');
                for (wchar_t c : typed) ui::OnChar(c);
                const bool cancel = mode.size() > 7 && !mode.compare(mode.size() - 7, 7, L"-cancel");
                ui::OnChar(cancel ? static_cast<wchar_t>(27) : L'\r');
                Log(L"applet test: entry %s", ui::WantsText() ? L"STILL OPEN" : L"answered");
            }
            pump(2500, false);
        }
    }
    Log(L"applet test: foreground the same as before: %d", GetForegroundWindow() == before);
    CloseHandle(gameProc);
    gameProc = nullptr;
    DestroyWindow(hwnd);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool windowed = false, background = false, noSetup = false, captureTest = false;
    std::wstring shotScreen, shotFile, seqScreen, seqDir, seqScript;
    std::wstring appletMode, appletText; // --applet-test
    int seqFrames = 0;
    std::wstring webTestPage, webTestLog; // --webtest <page.html> <log file> [cycles]
    int webTestCycles = 3;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--windowed")) windowed = true;
        else if (!wcscmp(argv[i], L"--background")) background = true;
        else if (!wcscmp(argv[i], L"--log")) logging = true;
        else if (!wcscmp(argv[i], L"--capture-test")) captureTest = true;
        else if (!wcscmp(argv[i], L"--applet-test") && i + 1 < argc) {
            appletMode = argv[++i];
            if (i + 1 < argc && wcsncmp(argv[i + 1], L"--", 2)) appletText = argv[++i];
        }
        else if (!wcscmp(argv[i], L"--no-setup")) noSetup = true; // automated runs: straight to the menu
        else if (!wcscmp(argv[i], L"--webtest") && i + 2 < argc) {
            webTestPage = argv[i + 1];
            webTestLog = argv[i + 2];
            i += 2;
            if (i + 1 < argc && wcsncmp(argv[i + 1], L"--", 2)) webTestCycles = _wtoi(argv[++i]);
        } else if (!wcscmp(argv[i], L"--webtest")) return 64; // never the real program by mistake
        else if (!wcscmp(argv[i], L"--shotseq") && i + 3 < argc) {
            seqScreen = argv[i + 1];
            seqDir = argv[i + 2];
            seqFrames = _wtoi(argv[i + 3]);
            i += 3;
            // Optional script; anything that is not another option.
            if (i + 1 < argc && wcsncmp(argv[i + 1], L"--", 2)) seqScript = argv[++i];
        } else if (!wcscmp(argv[i], L"--shot") && i + 2 < argc) {
            shotScreen = argv[i + 1];
            shotFile = argv[i + 2];
            i += 2;
        } else if (!wcscmp(argv[i], L"--shot") || !wcscmp(argv[i], L"--shotseq")) {
            // A test run with an argument missing must never fall through to
            // the real program: that would put a window on the screen of
            // whoever is sitting there.
            return 64;
        }
    }

    if (!gfx::Init()) return 1;
    LoadConfig();
    {
        // Menu text in the console's own typeface when the owner's firmware is
        // installed; Segoe UI otherwise.
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
        }
    }
    if (!webTestPage.empty()) {
        // The web page's whole path in a hidden window off the screen (webview.cpp).
        int rc = web::SelfTest(inst, webTestPage, webTestCycles, webTestLog);
        gfx::Shutdown();
        return rc;
    }
    if (captureTest) {
        int rc = RunCaptureTest(inst);
        gfx::Shutdown();
        return rc;
    }
    if (!appletMode.empty()) {
        int rc = RunAppletTest(inst, appletMode, appletText);
        gfx::Shutdown();
        return rc;
    }
    simulate = seqFrames > 0 || !shotFile.empty();
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

    update::AtStart(DataDir()); // after an update: wait for the old NSW-Home to end, remove its file
    // One instance only: a second start just brings the first to the front.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"NSW-Home.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"NSWHomeWindow", nullptr)) SetForegroundWindow(other);
        return 0;
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"NSWHomeWindow";
    RegisterClassExW(&wc);

    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"NSW-Home", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd || !gfx::AttachWindow(hwnd)) return 1;
    web::Attach(hwnd); // the web page's view is a child of this window
    ApplyTitle();
    ownerWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))) &&
        FAILED(taskbar->HrInit())) {
        taskbar->Release();
        taskbar = nullptr;
    }
    InitSounds();
    InitXInput();
    pads::Start();
    ui::Init();
    if (!noSetup) ui::FirstStart();
    CenterWindowed(TargetMonitor());
    // --background: open without activating (used when testing next to other work).
    ShowWindow(hwnd, background ? SW_SHOWNA : SW_SHOW);
    if (cfg.fullscreen && !windowed) ApplyFullscreen(true);
    RescanAsync();

    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    const LARGE_INTEGER start = last;

    while (!quit) {
        bool foreground = windowVisible && GetForegroundWindow() == hwnd;
        bool render = windowVisible && !IsIconic(hwnd) && (foreground || ui::Animating());
        // Rendering blocks on vsync, so an active menu needs no extra wait.
        // Hidden behind a game it wakes 10 times a second for the Guide
        // button and the emulator window; otherwise it idles.
        DWORD wait = render || ViewFading() ? 0 : (gameProc ? 100 : 250);
        DWORD n = gameProc ? 1 : 0;
        DWORD r = MsgWaitForMultipleObjects(n, &gameProc, FALSE, wait, QS_ALLINPUT);
        if (n && r == WAIT_OBJECT_0) OnGameExited();

        // --log: a pass that stalls while a game starts is what lets the
        // emulator's window be seen, so say which part of it took long.
        ULONGLONG t0 = GetTickCount64();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ULONGLONG t1 = GetTickCount64();
        PollPads();
        if (gameProc && !quit && AltF4Seen()) {
            Log(L"Alt+F4 during a game: ending NSW-Home");
            quit = true;
        }
        ULONGLONG t2 = GetTickCount64();
        TrackGameWindow();
        SyncCaptureKey();
        ULONGLONG t3 = GetTickCount64();

        if (render) {
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - last.QuadPart) / freq.QuadPart;
            last = now;
            ui::Draw(static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart, (std::min)(dt, 0.1f));
            gfx::End();
            if (gameProc && GetTickCount64() - t0 > 300)
                Log(L"slow pass: messages %llu ms, pads %llu ms, emulator window %llu ms, drawing %llu ms", t1 - t0, t2 - t1,
                    t3 - t2, GetTickCount64() - t3);
        } else {
            QueryPerformanceCounter(&last);
            // The game's picture is fading with nothing of ours being drawn:
            // one step per screen refresh.
            if (ViewFading()) DwmFlush();
        }
    }

    // Turning the console off ends the game too; its window may be hidden and
    // would otherwise be left running with no way to reach it.
    if (gameProc) {
        // (A PC game is asked first and given a moment: it may be saving.)
        HWND w = gamePc ? PcWindow(true) : nullptr;
        if (w && PostMessageW(w, WM_CLOSE, 0, 0)) WaitForSingleObject(gameProc, kPcCloseMs);
        if (gameJob) TerminateJobObject(gameJob, 0);
        else TerminateProcess(gameProc, 0);
    }
    pads::Stop();
    if (taskbar) taskbar->Release();
    web::Close();
    DestroyWindow(hwnd);
    friends::Shutdown(); // a user who shares says goodbye to their friends (a few seconds at most), the window already gone
    gfx::Shutdown();
    if (mutex) CloseHandle(mutex);
    return 0;
}
