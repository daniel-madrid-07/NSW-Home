// Handing an amiibo dump to the game running in an emulator (see amiibo.h).
//
// No emulator takes an amiibo from outside: no command-line option, setting,
// watched file or IPC (their sources, read 2026-11-26: Ryubing and Kenji-NX
// MainWindow.axaml / MainWindowViewModel.cs, the yuzu family's
// main_window.cpp OnLoadAmiibo). Each has a key that opens its own file dialog
// for a dump while a game is waiting for one:
//   yuzu family (Eden, Suyu, Citron...)  F2. If an amiibo is already loaded it
//       removes it and says so instead (a message box); the next F2 opens the
//       dialog. A game that is not looking for one gets a message box.
//   Ryubing (recent canary), Kenji-NX (current)  Ctrl+B. Nothing at all happens
//       when the game is not looking; Ryujinx 1.1 and the stable Ryubing 1.3.3
//       and Kenji-NX 2.0.5 have no such key.
// Both dialogs are the system's own file dialog, owned by the emulator's main
// window. While a game runs that window is owned by NSW-Home's cloaked window,
// so the dialog inherits the cloak and is never composed on screen (main.cpp,
// "game viewer"). Here the key is posted to the emulator's window, the dialog
// is found among the emulator's windows, the dump's path is written into its
// file name box (control 1148, as in every common file dialog) and Open
// (IDOK) is pressed. A message box instead is read through UI Automation and
// answered with its only button.
#include "amiibo.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uiautomation.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "oleaut32.lib")

namespace amiibo {
namespace {

struct Target {
    DWORD pid = 0;
    HWND emu = nullptr, menu = nullptr;
    char family = 0;
    bool live = true;       // false in the self-test: nothing is ever activated
    DWORD waitMs = 5000;    // for the emulator's dialog to appear
};

struct Outcome {
    std::string code; // "" NOT_LOOKING NO_DIALOG FILL CANCELLED
    std::wstring said;
    // For the self-test's report: what the dialog was and whether it was cloaked.
    std::wstring dialogClass;
    DWORD cloak = 0;
    RECT rect = {};
    int boxes = 0;
    bool viaUia = false; // the last box was answered through UI Automation
};

std::atomic<bool> cancelAsked{false};

struct Collect {
    DWORD pid;
    HWND skip;
    std::set<HWND>* out;
};
BOOL CALLBACK CollectProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<Collect*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT rc;
    // The emulator's dialogs: its visible windows that have an owner (a tool
    // tip or a list's drop-down is too small to be one).
    if (pid == c->pid && h != c->skip && IsWindowVisible(h) && GetWindow(h, GW_OWNER) && GetWindowRect(h, &rc) &&
        rc.right - rc.left >= 120 && rc.bottom - rc.top >= 60)
        c->out->insert(h);
    return TRUE;
}
std::set<HWND> Dialogs(const Target& t) {
    std::set<HWND> s;
    Collect c{t.pid, t.emu, &s};
    EnumWindows(CollectProc, reinterpret_cast<LPARAM>(&c));
    return s;
}

HWND WaitNew(const Target& t, const std::set<HWND>& before, DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end && !cancelAsked) {
        for (HWND h : Dialogs(t))
            if (!before.count(h)) return h;
        Sleep(30);
    }
    return nullptr;
}

bool Gone(HWND h, DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    do {
        if (!IsWindow(h) || !IsWindowVisible(h)) return true;
        Sleep(30);
    } while (GetTickCount64() < end);
    return false;
}

struct FindCtx {
    int id;
    HWND found;
};
BOOL CALLBACK FindIdProc(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<FindCtx*>(lp);
    if (GetDlgCtrlID(h) != f->id) return TRUE;
    f->found = h;
    return FALSE;
}
HWND FindId(HWND root, int id) {
    FindCtx f{id, nullptr};
    EnumChildWindows(root, FindIdProc, reinterpret_cast<LPARAM>(&f));
    return f.found;
}

std::wstring ClassOf(HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return cls;
}

// The file name box of a common file dialog (control 1148: an edit, or the
// edit inside a combo box), or nullptr when this is not such a dialog.
HWND FileNameBox(HWND dlg) {
    if (ClassOf(dlg) != L"#32770") return nullptr;
    HWND c = FindId(dlg, 1148);
    if (!c) return nullptr;
    if (ClassOf(c) == L"ComboBoxEx32") c = FindWindowExW(c, nullptr, L"ComboBox", nullptr);
    if (c && ClassOf(c) == L"ComboBox") c = FindWindowExW(c, nullptr, L"Edit", nullptr);
    return c && ClassOf(c) == L"Edit" ? c : nullptr;
}

bool Fill(HWND dlg, HWND box, const std::wstring& path) {
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(box, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()), SMTO_ABORTIFHUNG, 2000, &r)) return false;
    wchar_t back[1024] = {};
    SendMessageTimeoutW(box, WM_GETTEXT, 1024, reinterpret_cast<LPARAM>(back), SMTO_ABORTIFHUNG, 2000, &r);
    if (path != back) return false;
    // Open: the dialog's own IDOK command; then a click on the button itself.
    HWND ok = GetDlgItem(dlg, IDOK);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(ok));
    if (Gone(dlg, 2500)) return true;
    if (ok) SendMessageTimeoutW(ok, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 2000, &r);
    return Gone(dlg, 1500);
}

void CancelDialog(HWND dlg) {
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg, IDCANCEL)));
}

// A message box through UI Automation, which every toolkit offers (the yuzu
// family's are Qt windows with no Win32 controls inside): its text, and its
// first button pressed. A plain Windows box is answered with its button's
// command when that did not work.
void WalkBox(IUIAutomationTreeWalker* w, IUIAutomationElement* el, int depth, std::wstring& text, IUIAutomationElement*& button) {
    if (!el || depth > 12) return;
    CONTROLTYPEID type = 0;
    el->get_CurrentControlType(&type);
    if (type == UIA_TitleBarControlTypeId || type == UIA_MenuBarControlTypeId) return;
    BSTR raw = nullptr;
    el->get_CurrentName(&raw);
    const std::wstring name = raw ? raw : L"";
    if (raw) SysFreeString(raw);
    if (type == UIA_ButtonControlTypeId) {
        if (!button && !name.empty()) {
            button = el;
            el->AddRef();
        }
        return;
    }
    if (type == UIA_TextControlTypeId && !name.empty()) text += (text.empty() ? L"" : L"\n") + name;
    IUIAutomationElement* child = nullptr;
    w->GetFirstChildElement(el, &child);
    while (child) {
        WalkBox(w, child, depth + 1, text, button);
        IUIAutomationElement* next = nullptr;
        w->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;
    }
}

BOOL CALLBACK StaticTextProc(HWND h, LPARAM lp) {
    auto* out = reinterpret_cast<std::wstring*>(lp);
    wchar_t cls[16] = {}, text[512] = {};
    GetClassNameW(h, cls, 16);
    if (!_wcsicmp(cls, L"Static") && GetWindowTextW(h, text, 512)) *out += (out->empty() ? L"" : L"\n") + std::wstring(text);
    return TRUE;
}

std::wstring AnswerBox(HWND box, bool& viaUia) {
    std::wstring text;
    bool pressed = false;
    IUIAutomation* ua = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ua))) && ua) {
        IUIAutomationTreeWalker* walker = nullptr;
        IUIAutomationElement* root = nullptr;
        IUIAutomationElement* button = nullptr;
        ua->get_ControlViewWalker(&walker);
        if (walker && SUCCEEDED(ua->ElementFromHandle(box, &root)) && root) {
            WalkBox(walker, root, 0, text, button);
            root->Release();
        }
        if (button) {
            IUIAutomationInvokePattern* invoke = nullptr;
            if (SUCCEEDED(button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke) {
                pressed = SUCCEEDED(invoke->Invoke());
                invoke->Release();
            }
            button->Release();
        }
        if (walker) walker->Release();
        ua->Release();
    }
    viaUia = pressed;
    if (text.empty()) EnumChildWindows(box, StaticTextProc, reinterpret_cast<LPARAM>(&text));
    if (!pressed || !Gone(box, 1500)) {
        HWND b = FindWindowExW(box, nullptr, L"Button", nullptr);
        PostMessageW(box, WM_COMMAND, MAKEWPARAM(b ? GetDlgCtrlID(b) : IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(b));
        Gone(box, 1500);
    }
    return text;
}

// The emulator's amiibo key, posted straight to its window (as main.cpp posts
// F11): our own hotkeys, which keep the real keyboard's F2 and Ctrl+B from the
// emulator, do not see posted keys. Ctrl+B needs Ctrl to read as held where
// the emulator's toolkit asks for it (the keyboard state of its input thread):
// this thread shares that state while it waits for the dialog, then lets go.
struct KeySent {
    DWORD tid = 0;
    bool attached = false;
    BYTE ctrlBefore = 0, lctrlBefore = 0;
};
KeySent SendKey(const Target& t) {
    KeySent k;
    if (t.family == 'y') {
        PostMessageW(t.emu, WM_KEYDOWN, VK_F2, 0x003C0001);
        PostMessageW(t.emu, WM_KEYUP, VK_F2, 0xC03C0001);
        return k;
    }
    k.tid = GetWindowThreadProcessId(t.emu, nullptr);
    k.attached = AttachThreadInput(GetCurrentThreadId(), k.tid, TRUE) != 0;
    BYTE ks[256];
    if (k.attached && GetKeyboardState(ks)) {
        k.ctrlBefore = ks[VK_CONTROL];
        k.lctrlBefore = ks[VK_LCONTROL];
        ks[VK_CONTROL] |= 0x80;
        ks[VK_LCONTROL] |= 0x80;
        SetKeyboardState(ks);
    }
    PostMessageW(t.emu, WM_KEYDOWN, 'B', 0x00300001);
    PostMessageW(t.emu, WM_KEYUP, 'B', 0xC0300001);
    return k;
}
void ReleaseKey(KeySent& k) {
    if (!k.attached) return;
    BYTE ks[256];
    if (GetKeyboardState(ks)) {
        ks[VK_CONTROL] = k.ctrlBefore;
        ks[VK_LCONTROL] = k.lctrlBefore;
        SetKeyboardState(ks);
    }
    AttachThreadInput(GetCurrentThreadId(), k.tid, FALSE);
    k.attached = false;
}

// The keyboard back to the menu after a failure. Only the program in front
// may hand it on, and that is the emulator now: for the call this thread
// shares its input queue (as main.cpp's TakeFocusFromGame does).
void TakeBack(const Target& t) {
    if (!t.live || !t.menu) return;
    const DWORD tid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool joined = tid && AttachThreadInput(GetCurrentThreadId(), tid, TRUE);
    SetForegroundWindow(t.menu);
    if (joined) AttachThreadInput(GetCurrentThreadId(), tid, FALSE);
}

Outcome Run(const Target& t, const std::wstring& file) {
    Outcome o;
    if (t.live) {
        // The emulator gets the keyboard (its toolkit only acts on keys while
        // its window is active). NSW-Home's window stays on top of it with the
        // menu showing, as while any game is in front.
        SetForegroundWindow(t.emu);
        const ULONGLONG end = GetTickCount64() + 800;
        while (GetForegroundWindow() != t.emu && GetTickCount64() < end) Sleep(20);
        Sleep(120);
    }
    // Two rounds for the yuzu family: the first F2 may only take away the
    // amiibo loaded before.
    for (int round = 0; round < (t.family == 'y' ? 2 : 1); round++) {
        const std::set<HWND> before = Dialogs(t);
        KeySent k = SendKey(t);
        HWND d = WaitNew(t, before, t.waitMs);
        ReleaseKey(k);
        if (cancelAsked) {
            if (d && FileNameBox(d)) CancelDialog(d);
            o.code = "CANCELLED";
            break;
        }
        if (!d) {
            o.code = round == 0 ? "NO_DIALOG" : "NOT_LOOKING";
            break;
        }
        o.dialogClass = ClassOf(d);
        DwmGetWindowAttribute(d, DWMWA_CLOAKED, &o.cloak, sizeof(o.cloak));
        GetWindowRect(d, &o.rect);
        if (HWND box = FileNameBox(d)) {
            if (Fill(d, box, file)) {
                o.code.clear();
            } else {
                CancelDialog(d);
                o.code = "FILL";
            }
            break;
        }
        o.boxes++;
        o.said = AnswerBox(d, o.viaUia);
        o.code = "NOT_LOOKING"; // unless the next round opens the dialog
    }
    if (!o.code.empty()) TakeBack(t);
    return o;
}

std::mutex mu;
std::atomic<int> state{0}; // 0 nothing, 1 working, 2 finished
std::string doneCode;
std::wstring doneSaid;

} // namespace

bool HandOverAvailable() {
    return true;
}

bool HandOverBusy() {
    return state == 1;
}

std::string HandOverStart(const std::wstring& file) {
    Target t;
    void* emu = nullptr;
    void* menu = nullptr;
    uint32_t pid = 0;
    if (!EmuLink(pid, emu, menu, t.family) || (t.family != 'y' && t.family != 'r')) return "NO_GAME";
    int idle = 0;
    if (!state.compare_exchange_strong(idle, 1)) return "BUSY";
    t.pid = pid;
    t.emu = static_cast<HWND>(emu);
    t.menu = static_cast<HWND>(menu);
    cancelAsked = false;
    const std::wstring give = ForEmulator(file, t.family);
    std::thread([t, give] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Outcome o = Run(t, give);
        if (SUCCEEDED(co)) CoUninitialize();
        std::lock_guard<std::mutex> lock(mu);
        doneCode = o.code;
        doneSaid = o.said;
        state = 2;
    }).detach();
    return {};
}

bool HandOverDone(std::string& code, std::wstring& said) {
    if (state != 2) return false;
    std::lock_guard<std::mutex> lock(mu);
    code = doneCode;
    said = doneSaid;
    state = 0;
    return true;
}

void HandOverCancel() {
    cancelAsked = true;
}

// ---- the self-test: the same steps against stand-in windows ----
//
// A window that behaves like an emulator's main window (F2, or B with Ctrl
// held, opens the system's file dialog for a .bin; or first a message box),
// owned by a cloaked window that is never shown, as NSW-Home's owner window
// is. Everything is placed off screen and activation is refused, so nothing
// can take the keyboard from whoever is at the machine. Should a window of the
// stand-in thread ever be about to show uncloaked, it is cloaked on the spot
// and the report says so (that would mean the cloak is not inherited).

namespace {

struct Stand {
    HWND owner = nullptr, mid = nullptr;
    DWORD tid = 0;
    int boxesFirst = 0;        // message boxes before the dialog opens
    bool ignore = false;       // keys do nothing (a game not looking, Ryujinx-style)
    bool plainB = false;       // B arrived without Ctrl
    std::wstring folder, picked, boxText;
    std::atomic<int> forced{0};    // windows that had to be cloaked by hand
    std::atomic<int> shown{0};     // top-level windows shown
    std::atomic<unsigned> seen{0}; // every DWMWA_CLOAKED value met as a window was about to show, or-ed
    std::atomic<int> inside{0};    // a key is being handled (its dialog or box is up)
    std::atomic<bool> ready{false}, quit{false};
} stand;

HHOOK cbtHook = nullptr, wndHook = nullptr;

LRESULT CALLBACK CbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) return 1; // never activated: the keyboard stays where it is
    return CallNextHookEx(cbtHook, code, wp, lp);
}

LRESULT CALLBACK WndHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const CWPSTRUCT* m = reinterpret_cast<const CWPSTRUCT*>(lp);
        const bool showing = (m->message == WM_SHOWWINDOW && m->wParam) ||
                             (m->message == WM_WINDOWPOSCHANGING && (reinterpret_cast<const WINDOWPOS*>(m->lParam)->flags & SWP_SHOWWINDOW));
        if (showing && !(GetWindowLongW(m->hwnd, GWL_STYLE) & WS_CHILD) && m->hwnd != stand.owner) {
            stand.shown++;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(m->hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            stand.seen |= cloaked;
            if (!cloaked) {
                BOOL on = TRUE;
                DwmSetWindowAttribute(m->hwnd, DWMWA_CLOAK, &on, sizeof(on));
                stand.forced++;
            }
        }
    }
    return CallNextHookEx(wndHook, code, wp, lp);
}

INT_PTR CALLBACK BoxProc(HWND h, UINT msg, WPARAM wp, LPARAM) {
    if (msg == WM_INITDIALOG) return TRUE;
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        EndDialog(h, LOWORD(wp));
        return TRUE;
    }
    return FALSE;
}

// A message box of our own making (a plain dialog: MessageBox would play a sound).
void ShowBox(HWND owner, const std::wstring& text) {
    alignas(4) WORD buf[512] = {};
    WORD* p = buf;
    auto words = [&](const wchar_t* s) {
        while (*s) *p++ = static_cast<WORD>(*s++);
        *p++ = 0;
    };
    auto align = [&] { p = reinterpret_cast<WORD*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~ULONG_PTR(3)); };
    DLGTEMPLATE* dt = reinterpret_cast<DLGTEMPLATE*>(p);
    dt->style = WS_POPUP | WS_CAPTION | DS_MODALFRAME | DS_SETFONT;
    dt->cdit = 2;
    dt->x = 0, dt->y = 0, dt->cx = 200, dt->cy = 70;
    p = reinterpret_cast<WORD*>(dt + 1);
    *p++ = 0; // menu
    *p++ = 0; // class
    words(L"Amiibo");
    *p++ = 9;
    words(L"Segoe UI");
    const struct {
        DWORD style;
        short x, y, cx, cy;
        WORD id, cls;
        const wchar_t* text;
    } items[2] = {{WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 8, 184, 30, 100, 0x0082, text.c_str()},
                  {WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 140, 48, 52, 14, IDOK, 0x0080, L"OK"}};
    for (const auto& it : items) {
        align();
        DLGITEMTEMPLATE* di = reinterpret_cast<DLGITEMTEMPLATE*>(p);
        di->style = it.style;
        di->x = it.x, di->y = it.y, di->cx = it.cx, di->cy = it.cy;
        di->id = it.id;
        p = reinterpret_cast<WORD*>(di + 1);
        *p++ = 0xFFFF;
        *p++ = it.cls;
        words(it.text);
        *p++ = 0;
    }
    stand.boxText = text;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dt, owner, BoxProc, 0);
}

void OpenDialog(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC spec[2] = {{L"Amiibo File (*.bin)", L"*.bin"}, {L"All Files (*.*)", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    dlg->SetTitle(L"Load Amiibo");
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(stand.folder.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dlg->SetFolder(folder);
        folder->Release();
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                stand.picked = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

LRESULT CALLBACK StandProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && h == stand.mid && !stand.ignore) {
        if (wp == VK_F2 || (wp == 'B' && (GetKeyState(VK_CONTROL) & 0x8000))) {
            struct Inside {
                Inside() { stand.inside++; }
                ~Inside() { stand.inside--; }
            } in;
            if (stand.boxesFirst > 0) {
                stand.boxesFirst--;
                ShowBox(h, stand.boxesFirst > 50 ? L"The current game is not looking for amiibos" : L"The current amiibo has been removed");
            } else {
                OpenDialog(h);
            }
            return 0;
        }
        if (wp == 'B') stand.plainB = true;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void StandThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    stand.tid = GetCurrentThreadId();
    cbtHook = SetWindowsHookExW(WH_CBT, CbtProc, nullptr, stand.tid);
    wndHook = SetWindowsHookExW(WH_CALLWNDPROC, WndHookProc, nullptr, stand.tid);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = StandProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NSWHomeAmiiboStandIn";
    RegisterClassW(&wc);
    // As main.cpp's owner window: never shown, cloaked; the "emulator" is owned by it.
    stand.owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, -32000, -32000, 1, 1, nullptr,
                                  nullptr, wc.hInstance, nullptr);
    BOOL on = TRUE;
    DwmSetWindowAttribute(stand.owner, DWMWA_CLOAK, &on, sizeof(on));
    stand.mid = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Stand-in emulator", WS_POPUP, -32000, -32000, 640, 360,
                                stand.owner, nullptr, wc.hInstance, nullptr);
    ShowWindow(stand.mid, SW_SHOWNOACTIVATE);
    stand.ready = true;
    MSG m;
    while (!stand.quit) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    DestroyWindow(stand.mid);
    DestroyWindow(stand.owner);
    UnhookWindowsHookEx(wndHook);
    UnhookWindowsHookEx(cbtHook);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
}

} // namespace

static std::string Ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

bool HandOverSelfTest(const std::wstring& dump, std::string& report) {
    stand.folder = dump.substr(0, dump.find_last_of(L"\\/"));
    std::thread th(StandThread);
    while (!stand.ready) Sleep(10);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool all = true;
    char line[600];
    auto run = [&](const char* name, char family, int boxes, bool ignore, const char* want, bool wantPicked) {
        stand.boxesFirst = boxes;
        stand.ignore = ignore;
        stand.picked.clear();
        stand.plainB = false;
        Target t;
        t.pid = GetCurrentProcessId();
        t.emu = stand.mid;
        t.family = family;
        t.live = false;
        t.waitMs = ignore ? 1500 : 8000; // the first file dialog of a process takes a while to build
        cancelAsked = false;
        const ULONGLONG t0 = GetTickCount64();
        const Outcome o = Run(t, dump);
        const ULONGLONG ms = GetTickCount64() - t0;
        // The stand-in learns the dialog's answer when its Show returns, a moment after the dialog has gone.
        for (int i = 0; i < 150 && stand.inside; i++) Sleep(20);
        const bool picked = stand.picked == dump;
        const bool ok = o.code == want && picked == wantPicked && !stand.plainB;
        all = all && ok;
        snprintf(line, sizeof line,
                 "%-4s %-34s family %c: result \"%s\" (wanted \"%s\"), file %s, %d message box(es)%s, %llu ms; last dialog %ls, "
                 "DWMWA_CLOAKED=0x%lx%s, at %ld,%ld %ldx%ld%s%s\n",
                 ok ? "ok" : "FAIL", name, family, o.code.c_str(), want, picked ? "handed over" : "not handed over", o.boxes,
                 o.boxes ? (o.viaUia ? " (answered through UI Automation)" : " (answered with its button's command)") : "", ms,
                 o.dialogClass.empty() ? L"-" : o.dialogClass.c_str(), o.cloak, (o.cloak & 4) ? " (inherited)" : "", o.rect.left, o.rect.top,
                 o.rect.right - o.rect.left, o.rect.bottom - o.rect.top, o.said.empty() ? "" : ", box said: ",
                 Ascii(o.said).c_str());
        report += line;
    };
    run("F2, dialog", 'y', 0, false, "", true);
    run("F2, amiibo removed first", 'y', 1, false, "", true);
    run("F2, game not looking", 'y', 99, false, "NOT_LOOKING", false);
    run("Ctrl+B, dialog", 'r', 0, false, "", true);
    run("Ctrl+B, game not looking", 'r', 0, true, "NO_DIALOG", false);
    snprintf(line, sizeof line,
             "stand-in top-level windows about to show: %d; already cloaked then: %d (DWMWA_CLOAKED values met: 0x%x); "
             "not cloaked, so cloaked by the test: %d\n",
             stand.shown.load(), stand.shown.load() - stand.forced.load(), stand.seen.load(), stand.forced.load());
    report += line;
    all = all && stand.forced == 0;
    stand.quit = true;
    th.join();
    stand.ready = false;
    stand.quit = false;
    CoUninitialize();
    return all;
}

} // namespace amiibo

#else // ---- other systems: the emulators' windows cannot be reached this way ----

namespace amiibo {
bool HandOverAvailable() {
    return false;
}
bool HandOverBusy() {
    return false;
}
std::string HandOverStart(const std::wstring&) {
    return "NO_GAME";
}
bool HandOverDone(std::string&, std::wstring&) {
    return false;
}
void HandOverCancel() {}
bool HandOverSelfTest(const std::wstring&, std::string& report) {
    report = "not available on this system\n";
    return false;
}
} // namespace amiibo

#endif
