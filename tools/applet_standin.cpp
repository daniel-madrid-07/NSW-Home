// Stand-in for the emulators' windows that NSW-Home's pause and applet code
// talks to (see "pause on HOME, and the games' applets" in src\main.cpp), for
// `dev.exe --applet-test <mode>` (tools\applet_test.sh runs the lot).
//
//   applet_standin.exe <mode> <log file>
//
// Plain Win32 controls with the automation shape of the real windows: the
// automation ids Avalonia (Ryujinx) and Qt (yuzu family) give their controls
// are put on the controls with the system's own annotation service
// (IAccPropServices), so UI Automation reads "Input", "PrimaryButton",
// "QtSoftwareKeyboardDialog.button_ok"... exactly as it would there, and the
// controls behave as the emulators' do in the respects NSW-Home relies on:
//   ryu-kb, ryu-kb-off, ryu-kb-cancel   Ryujinx's software keyboard: OK only
//       looks at the text again on a key typed into the box (so text set from
//       outside leaves it as it was); "-off" starts empty with OK off.
//   yuzu-kb, yuzu-kb-cancel   yuzu's keyboard: the box's text updates the
//       "n/12" counter and OK; closing the window is its Cancel.
//   yuzu-inline   yuzu's inline keyboard: no box, only keys; the text the game
//       holds ("abc") is changed by pressing them.
//   ryu-pad, yuzu-pad   the controller applets.
//   ryu-pause, ryu-pause-active   Ryujinx's pause hotkey (F5), seen only when
//       held for 2 ms or more; with "-active" only while this window is the
//       active one, as Ryujinx's (it never is here: the test shows a key posted
//       to a window in the background does nothing then).
//   yuzu-pause   yuzu's Emulation > Pause item, a button named "&Pausar" that
//       becomes "&Continuar".
// Every window is off screen (-32000), never activated (WS_EX_NOACTIVATE,
// SW_SHOWNOACTIVATE), has no taskbar button (WS_EX_TOOLWINDOW) and is not
// cloaked (see Cloak); the program ends by itself after 20 s (10 s for a pause
// test), or 1 s after its dialog has been answered. What reaches it goes to
// the log file.
#include <windows.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

// UI Automation's AutomationId and HelpText properties (UIAutomationClient.h).
const GUID kAutomationId = {0xc82c0500, 0xb60e, 0x4310, {0xa2, 0x67, 0x30, 0x3c, 0x53, 0x1f, 0x8e, 0xe5}};
const GUID kHelpText = {0x08555685, 0x0977, 0x45c7, {0xa7, 0xa6, 0xab, 0xaf, 0x56, 0x84, 0x12, 0x1a}};
// The annotation service (oleacc.h; no import library defines it).
const CLSID kAccPropServices = {0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};

std::wstring mode, logPath;
IAccPropServices* props = nullptr;
HWND mainWnd = nullptr, dlgWnd = nullptr;
ULONGLONG quitAt = 0;
bool paused = false, keyDown = false;
LARGE_INTEGER downAt = {};
const wchar_t* kRyuTitle = L"Ryujinx 1.1.1403 - Stand-in v1.0.0 (0100000000001000) (64-bit)";

void Log(const wchar_t* fmt, ...) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, logPath.c_str(), L"a, ccs=UTF-8") || !f) return;
    va_list a;
    va_start(a, fmt);
    vfwprintf(f, fmt, a);
    va_end(a);
    fputwc(L'\n', f);
    fclose(f);
}

bool Is(const wchar_t* m) {
    return mode == m;
}
bool Starts(const wchar_t* m) {
    return mode.rfind(m, 0) == 0;
}

void Annotate(HWND h, const wchar_t* id, const wchar_t* help = nullptr) {
    if (!props) return;
    if (id) props->SetHwndPropStr(h, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, kAutomationId, id);
    if (help) props->SetHwndPropStr(h, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, kHelpText, help);
}

int nextId = 100;
HWND Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, const wchar_t* id,
          bool shown = true, const wchar_t* help = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style | (shown ? WS_VISIBLE : 0), x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(nextId++)), GetModuleHandleW(nullptr), nullptr);
    Annotate(c, id, help);
    return c;
}

std::wstring TextOf(HWND h) {
    wchar_t t[1024] = {};
    GetWindowTextW(h, t, ARRAYSIZE(t));
    return t;
}

void Cloak(HWND h) {
    // Not by default: UI Automation leaves the child windows of a cloaked
    // window out of its tree (measured with this program: 1 element instead of
    // 8), and every control here is a child window. (Avalonia and Qt draw
    // their controls themselves and hand them to UI Automation through their
    // own providers, which is what the real emulators' cloaked windows rely
    // on: see the real-emulator tests.) STANDIN_CLOAK=1 shows the difference.
    if (!GetEnvironmentVariableW(L"STANDIN_CLOAK", nullptr, 0)) return;
    BOOL on = TRUE;
    DwmSetWindowAttribute(h, DWMWA_CLOAK, &on, sizeof(on));
}

void Answered() {
    if (dlgWnd) DestroyWindow(dlgWnd);
    dlgWnd = nullptr;
    quitAt = GetTickCount64() + 1000;
}

// ---- Ryujinx's software keyboard ----
HWND ryuEdit = nullptr, ryuOk = nullptr, ryuCancel = nullptr;
bool ryuEditFocused = false;
WNDPROC editProc = nullptr;

void RyuCheck(const wchar_t* why) {
    const size_t n = TextOf(ryuEdit).size();
    const bool on = n >= 1 && n <= 12;
    EnableWindow(ryuOk, on);
    Log(L"ryu keyboard: OK looked at again (%s): %s", why, on ? L"on" : L"off");
}

LRESULT CALLBACK RyuEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYUP) {
        // Message_KeyUp: Enter with OK on takes the text; any other key looks at the text again.
        if (wp == VK_RETURN && IsWindowEnabled(ryuOk)) {
            Log(L"ryu keyboard: OK by Enter: \"%s\"", TextOf(ryuEdit).c_str());
            Answered();
            return 0;
        }
        RyuCheck(L"key up in the box");
    }
    if (m == WM_SETFOCUS) ryuEditFocused = true;
    if (m == WM_KILLFOCUS) ryuEditFocused = false;
    return CallWindowProcW(editProc, h, m, wp, lp);
}

// ---- yuzu's keyboard ----
HWND yEdit = nullptr, yCount = nullptr, yOk = nullptr;
std::wstring inlineText = L"abc"; // what the game holds, inline
size_t inlineCursor = 3;
std::vector<HWND> keyButtons;
HWND yBack = nullptr, ySpace = nullptr;

void YuzuCount() {
    const size_t n = TextOf(yEdit).size();
    SetWindowTextW(yCount, (std::to_wstring(n) + L"/12").c_str());
    EnableWindow(yOk, n >= 1);
}

// ---- controller applets ----
std::vector<HWND> players;
HWND padOk = nullptr, padBox = nullptr;

LRESULT CALLBACK BoxProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COMMAND) return SendMessageW(GetParent(h), m, wp, lp); // its buttons report to the dialog
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK DialogProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN:
    case WM_KEYUP:
        Log(L"dialog: %s %u", m == WM_KEYDOWN ? L"key down" : L"key up", static_cast<unsigned>(wp));
        // Avalonia sends a window's keys to the focused control.
        if (ryuEdit && ryuEditFocused) return SendMessageW(ryuEdit, m, wp, lp);
        if (yEdit && m == WM_KEYDOWN && wp == VK_RETURN) {
            Log(L"yuzu keyboard: Enter in the box: \"%s\"", TextOf(yEdit).c_str());
            Answered();
        }
        return 0;
    case WM_CLOSE:
        Log(L"dialog: closed from outside (its Cancel)");
        Answered();
        return 0;
    case WM_COMMAND: {
        const HWND c = reinterpret_cast<HWND>(lp);
        if (HIWORD(wp) == EN_CHANGE && c == yEdit) {
            YuzuCount();
            return 0;
        }
        if (HIWORD(wp) != BN_CLICKED) return 0;
        if (c == ryuOk) {
            Log(L"ryu keyboard: OK pressed: \"%s\"", TextOf(ryuEdit).c_str());
            Answered();
        } else if (c == ryuCancel) {
            Log(L"ryu keyboard: Cancel pressed");
            Answered();
        } else if (c == yOk && Is(L"yuzu-inline")) {
            Log(L"yuzu inline keyboard: OK: \"%s\"", inlineText.c_str());
            Answered();
        } else if (c == yOk) {
            Log(L"yuzu keyboard: OK pressed: \"%s\"", TextOf(yEdit).c_str());
            Answered();
        } else if (c == yBack) {
            if (inlineCursor > 0) inlineText.erase(--inlineCursor, 1);
            EnableWindow(yBack, inlineCursor > 0);
        } else if (c == ySpace) {
            inlineText.insert(inlineCursor++, 1, L' ');
            EnableWindow(yBack, TRUE);
        } else if (std::find(keyButtons.begin(), keyButtons.end(), c) != keyButtons.end()) {
            inlineText.insert(inlineCursor++, 1, TextOf(c)[0]);
            EnableWindow(yBack, TRUE);
        } else if (std::find(players.begin(), players.end(), c) != players.end()) {
            int on = 0;
            for (HWND p : players) on += SendMessageW(p, BM_GETCHECK, 0, 0) == BST_CHECKED;
            EnableWindow(padOk, on > 0);
            Log(L"yuzu controller applet: player box switched, %d on", on);
        } else if (c == padOk) {
            std::wstring on;
            for (size_t i = 0; i < players.size(); i++)
                if (SendMessageW(players[i], BM_GETCHECK, 0, 0) == BST_CHECKED) on += std::to_wstring(i + 1) + L" ";
            Log(L"yuzu controller applet: OK with players %s", on.c_str());
            Answered();
        } else {
            Log(L"button pressed: \"%s\"", TextOf(c).c_str());
            Answered();
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

HWND pauseItem = nullptr;

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN:
        Log(L"main: key down %u (foreground is this window: %d)", static_cast<unsigned>(wp), GetForegroundWindow() == h);
        if (wp == VK_F5 && Starts(L"ryu-pause")) {
            keyDown = true;
            QueryPerformanceCounter(&downAt);
        }
        return 0;
    case WM_KEYUP:
        Log(L"main: key up %u", static_cast<unsigned>(wp));
        if (wp == VK_F5 && Starts(L"ryu-pause") && keyDown) {
            keyDown = false;
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            const double ms = (now.QuadPart - downAt.QuadPart) * 1000.0 / freq.QuadPart;
            const bool active = !Is(L"ryu-pause-active") || GetForegroundWindow() == h;
            if (ms >= 2 && active) {
                paused = !paused;
                SetWindowTextW(h, paused ? (std::wstring(kRyuTitle) + L" (Pausado)").c_str() : kRyuTitle);
                Log(L"main: F5 held %.0f ms: %s", ms, paused ? L"PAUSED" : L"RESUMED");
            } else {
                Log(L"main: F5 held %.0f ms, window active %d: not seen", ms, active);
            }
        }
        if (wp == VK_F4 && Is(L"yuzu-pause")) Log(L"main: F4 (its hotkey) arrived");
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == pauseItem && HIWORD(wp) == BN_CLICKED) {
            paused = !paused;
            SetWindowTextW(pauseItem, paused ? L"&Continuar" : L"&Pausar");
            Log(L"main: Emulation > Pause item pressed: %s", paused ? L"PAUSED" : L"RESUMED");
        }
        return 0;
    case WM_CLOSE:
        Log(L"main: closed");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 3) return 1;
    mode = argv[1];
    logPath = argv[2];
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoCreateInstance(kAccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&props));
    if (!props) Log(L"stand-in: the annotation service is missing: automation ids will not match");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.hInstance = inst;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"NSWHomeAppletStandIn";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = DialogProc;
    wc.lpszClassName = L"NSWHomeAppletDialog";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = BoxProc;
    wc.lpszClassName = L"NSWHomeAppletBox";
    RegisterClassExW(&wc);

    const DWORD ex = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    const bool ryu = Starts(L"ryu");
    mainWnd = CreateWindowExW(ex, L"NSWHomeAppletStandIn", ryu ? kRyuTitle : L"Eden | Stand-in", WS_POPUP, -32000, -32000, 1280, 720,
                              nullptr, nullptr, inst, nullptr);
    Cloak(mainWnd);
    if (Is(L"yuzu-pause")) pauseItem = Make(mainWnd, L"BUTTON", L"&Pausar", BS_PUSHBUTTON, 10, 10, 120, 30, L"MainWindow.action_Pause");

    const bool dialog = !Starts(L"ryu-pause") && !Is(L"yuzu-pause");
    if (dialog) {
        dlgWnd = CreateWindowExW(ex, L"NSWHomeAppletDialog", ryu ? L"ContentDialogOverlayWindow" : L"", WS_POPUP, -32000, -32000, 900, 600,
                                 mainWnd, nullptr, inst, nullptr);
        Cloak(dlgWnd);
        HWND d = dlgWnd;
        if (Starts(L"ryu-kb")) {
            Make(d, L"STATIC", L"Teclado de software", 0, 10, 10, 400, 20, nullptr);
            Make(d, L"STATIC", L"¿Cómo quieres llamarte?", 0, 10, 40, 400, 20, nullptr);
            Make(d, L"STATIC", L"Podrás cambiarlo más tarde.", 0, 10, 70, 400, 20, nullptr);
            ryuEdit = Make(d, L"EDIT", Is(L"ryu-kb-off") ? L"" : L"Link", WS_BORDER | ES_AUTOHSCROLL, 10, 100, 400, 24, L"Input");
            editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(ryuEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RyuEditProc)));
            Make(d, L"STATIC", L"Debe tener entre 1 y 12 caracteres", 0, 10, 130, 400, 20, L"Error");
            ryuOk = Make(d, L"BUTTON", L"Aceptar", BS_PUSHBUTTON, 10, 160, 120, 30, L"PrimaryButton");
            ryuCancel = Make(d, L"BUTTON", L"Cancelar", BS_PUSHBUTTON, 140, 160, 120, 30, L"CloseButton");
            EnableWindow(ryuOk, !Is(L"ryu-kb-off"));
        } else if (Starts(L"yuzu-kb") || Is(L"yuzu-inline")) {
            const bool inl = Is(L"yuzu-inline");
            Make(d, L"STATIC", L"Introduce un nombre", 0, 10, 10, 400, 20, L"QtSoftwareKeyboardDialog.topOSK.label_header", !inl);
            Make(d, L"STATIC", L"Para tu partida guardada", 0, 10, 40, 400, 20, L"QtSoftwareKeyboardDialog.topOSK.label_sub", !inl);
            yCount = Make(d, L"STATIC", L"4/12", 0, 420, 70, 60, 20, L"QtSoftwareKeyboardDialog.topOSK.label_characters", !inl);
            yEdit = Make(d, L"EDIT", L"Link", WS_BORDER | ES_AUTOHSCROLL, 10, 70, 400, 24, L"QtSoftwareKeyboardDialog.topOSK.line_edit_osk", !inl,
                         L"Nombre del personaje");
            SendMessageW(yEdit, EM_LIMITTEXT, 12, 0);
            Make(d, L"EDIT", L"", WS_BORDER | ES_MULTILINE, 10, 100, 400, 60, L"QtSoftwareKeyboardDialog.topOSK.text_edit_osk", false);
            int x = 10;
            for (const wchar_t* k : {L"q", L"w", L"e", L"r", L"t", L"y", L"z", L"l", L"d", L"a", L"1", L"2", L"3", L"4", L"6"}) {
                const std::wstring id = std::wstring(L"QtSoftwareKeyboardDialog.bottomOSK.button_") + k;
                keyButtons.push_back(Make(d, L"BUTTON", k, BS_PUSHBUTTON, x, 200, 28, 28, id.c_str()));
                x += 30;
            }
            x = 10;
            for (const wchar_t* k : {L"Q", L"Z", L"L", L"D", L"A"}) {
                const std::wstring id = std::wstring(L"QtSoftwareKeyboardDialog.bottomOSK.button_") + k + L"_shift";
                keyButtons.push_back(Make(d, L"BUTTON", k, BS_PUSHBUTTON, x, 240, 28, 28, id.c_str()));
                x += 30;
            }
            ySpace = Make(d, L"BUTTON", L"Espacio", BS_PUSHBUTTON, 10, 280, 200, 28, L"QtSoftwareKeyboardDialog.bottomOSK.button_space");
            yBack = Make(d, L"BUTTON", L"Borrar", BS_PUSHBUTTON, 220, 280, 80, 28, L"QtSoftwareKeyboardDialog.bottomOSK.button_backspace");
            yOk = Make(d, L"BUTTON", L"OK", BS_PUSHBUTTON, 310, 280, 80, 28, L"QtSoftwareKeyboardDialog.bottomOSK.button_ok");
            Make(d, L"BUTTON", L"OK", BS_PUSHBUTTON, 400, 280, 80, 28, L"QtSoftwareKeyboardDialog.bottomOSK.button_ok_num", false);
        } else if (Is(L"ryu-pad")) {
            Make(d, L"STATIC", L"Mandos no compatibles", 0, 10, 10, 400, 20, nullptr);
            Make(d, L"BUTTON", L"Abrir la configuración", BS_PUSHBUTTON, 10, 160, 200, 30, L"SaveButton");
            Make(d, L"BUTTON", L"Cerrar", BS_PUSHBUTTON, 220, 160, 120, 30, L"CancelButton");
        } else if (Is(L"yuzu-pad")) {
            for (int i = 1; i <= 4; i++) {
                const std::wstring id = L"QtControllerSelectorDialog.groupPlayer" + std::to_wstring(i) + L"Connected";
                players.push_back(Make(d, L"BUTTON", (L"Jugador " + std::to_wstring(i)).c_str(), BS_AUTOCHECKBOX, 10, 10 + i * 30, 200, 24,
                                       id.c_str()));
            }
            padBox = Make(d, L"NSWHomeAppletBox", L"", 0, 10, 200, 300, 40, L"QtControllerSelectorDialog.buttonBox");
            padOk = Make(padBox, L"BUTTON", L"Aceptar", BS_PUSHBUTTON, 0, 0, 120, 30, nullptr);
            Make(padBox, L"BUTTON", L"Cancelar", BS_PUSHBUTTON, 130, 0, 120, 30, nullptr);
            EnableWindow(padOk, FALSE); // the game asked for its players to be chosen again (none kept)
        }
    }
    ShowWindow(mainWnd, SW_SHOWNOACTIVATE);
    if (dlgWnd) ShowWindow(dlgWnd, SW_SHOWNOACTIVATE);
    Log(L"stand-in %s: up (pid %lu)", mode.c_str(), GetCurrentProcessId());

    // (A pause test is over in 6 s; nothing answers it, so it ends sooner.)
    const ULONGLONG end = GetTickCount64() + (dialog ? 20000 : 10000);
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const ULONGLONG now = GetTickCount64();
        if (now > end || (quitAt && now > quitAt)) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
done:
    if (Is(L"yuzu-pause") || Starts(L"ryu-pause")) Log(L"stand-in: ends %s", paused ? L"paused" : L"running");
    if (dlgWnd) Log(L"stand-in: ends with its dialog still up");
    if (props) props->Release();
    DestroyWindow(mainWnd);
    CoUninitialize();
    return 0;
}
