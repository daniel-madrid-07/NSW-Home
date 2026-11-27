// The web page inside NSW-Home's window (see webview.h): Microsoft Edge
// WebView2 as a child window of the menu's window.
//
// The SDK's static loader (third_party\webview2, from the Microsoft.Web.WebView2
// NuGet package, BSD licence in third_party\webview2\LICENSE.txt) is linked
// into the program, so nothing new goes beside the exe: it finds the installed
// runtime (the Evergreen one Windows keeps up to date) the documented way.
// Without the runtime, Available() is false and the menu opens the browser.
#include "webview.h"
#include "app.h"
#include "gfx.h"
#include <WebView2.h>
#include <wrl/client.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

using Microsoft::WRL::ComPtr;

namespace web {
namespace {

// One COM handler from a lambda: WebView2 reports everything through small
// interfaces with a single Invoke.
template <class I, class Sig>
class Cb;
template <class I, class... A>
class Cb<I, HRESULT(A...)> : public I {
public:
    explicit Cb(std::function<HRESULT(A...)> f) : f_(std::move(f)) {}
    virtual ~Cb() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return f_(a...); }

private:
    std::function<HRESULT(A...)> f_;
    LONG ref_ = 1;
};
template <class I, class Sig, class F>
ComPtr<I> Make(F f) {
    ComPtr<I> p;
    p.Attach(new Cb<I, Sig>(std::move(f)));
    return p;
}

std::wstring Take(LPWSTR s) { // a string WebView2 allocated, freed here
    std::wstring out = s ? s : L"";
    if (s) CoTaskMemFree(s);
    return out;
}

HWND host = nullptr;
ComPtr<ICoreWebView2Environment> env;
bool envStarting = false;
bool envWanted = false; // a page is open or about to be: keep the environment when it arrives
ComPtr<ICoreWebView2Controller> ctl;
ComPtr<ICoreWebView2> view;
State state = State::Closed;
unsigned gen = 0; // bumped by every Open and Close: late answers for an older one are dropped
std::wstring wantUrl;
std::string wantProfile;
uint32_t wantGround = 0;
bool loading = false, shown = false, fullScreen = false, canBack = false, canForward = false;
std::wstring title, url;
RECT bounds = {}; // in the host's client pixels
POINT hostAt = {LONG_MIN, LONG_MIN};
std::vector<uint8_t> snap;
double padLast = 0;
float wheelX = 0, wheelY = 0, moveX = 0, moveY = 0;

void (*logFn)(const wchar_t*) = nullptr; // SelfTest's log; nothing otherwise
void Note(const wchar_t* fmt, ...) {
    if (!logFn) return;
    wchar_t b[1024];
    va_list a;
    va_start(a, fmt);
    vswprintf_s(b, fmt, a);
    va_end(a);
    logFn(b);
}

double Seconds() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

// A profile name WebView2 takes: letters and digits, at most 64 characters.
std::wstring ProfileName(const std::string& p) {
    std::wstring out = L"u";
    for (char c : p)
        if (isalnum(static_cast<unsigned char>(c)) && out.size() < 60) out += static_cast<wchar_t>(c);
    return out;
}

void ApplyBounds() {
    if (!ctl) return;
    RECT r = bounds;
    if (fullScreen && host) GetClientRect(host, &r);
    ctl->put_Bounds(r);
}

void SetupView();

void MakeController() {
    if (!env || !host || wantUrl.empty() || ctl) return;
    const unsigned g = gen;
    auto done = Make<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT(HRESULT, ICoreWebView2Controller*)>(
        [g](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
            if (g != gen) { // closed (or opened again) meanwhile
                if (c) c->Close();
                return S_OK;
            }
            if (FAILED(hr) || !c) {
                Note(L"controller failed: 0x%08lx", static_cast<unsigned long>(hr));
                state = State::Failed;
                return S_OK;
            }
            ctl = c;
            SetupView();
            return S_OK;
        });
    // One browser profile per user: what each signs in to stays theirs.
    ComPtr<ICoreWebView2Environment10> env10;
    ComPtr<ICoreWebView2ControllerOptions> opts;
    HRESULT hr = E_NOINTERFACE;
    if (SUCCEEDED(env.As(&env10)) && SUCCEEDED(env10->CreateCoreWebView2ControllerOptions(&opts)) && opts) {
        opts->put_ProfileName(ProfileName(wantProfile).c_str());
        hr = env10->CreateCoreWebView2ControllerWithOptions(host, opts.Get(), done.Get());
    } else {
        hr = env->CreateCoreWebView2Controller(host, done.Get());
    }
    if (FAILED(hr)) {
        Note(L"controller not started: 0x%08lx", static_cast<unsigned long>(hr));
        state = State::Failed;
    }
}

void StartEnvironment() {
    if (env || envStarting || !host) return;
    envStarting = true;
    // The user data (cookies, logins, cache) in NSW-Home's data folder, never
    // beside the program.
    const std::wstring dir = DataDir() + L"\\web";
    auto done = Make<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT(HRESULT, ICoreWebView2Environment*)>(
        [](HRESULT hr, ICoreWebView2Environment* e) -> HRESULT {
            envStarting = false;
            if (!envWanted || env) return S_OK; // closed meanwhile (or a second answer): let it go
            if (FAILED(hr) || !e) {
                Note(L"environment failed: 0x%08lx", static_cast<unsigned long>(hr));
                if (state == State::Starting) state = State::Failed;
                return S_OK;
            }
            env = e;
            MakeController();
            return S_OK;
        });
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, dir.c_str(), nullptr, done.Get());
    if (FAILED(hr)) {
        envStarting = false;
        Note(L"environment not started: 0x%08lx", static_cast<unsigned long>(hr));
        if (state == State::Starting) state = State::Failed;
    }
}

void SetupView() {
    ctl->get_CoreWebView2(&view);
    if (!view) {
        state = State::Failed;
        return;
    }
    const unsigned g = gen;
    ComPtr<ICoreWebView2Controller2> ctl2;
    if (SUCCEEDED(ctl.As(&ctl2))) {
        // The page's ground until it paints: the menu's, not a white flash.
        COREWEBVIEW2_COLOR c = {255, static_cast<BYTE>(wantGround >> 16), static_cast<BYTE>(wantGround >> 8),
                                static_cast<BYTE>(wantGround)};
        ctl2->put_DefaultBackgroundColor(c);
    }
    ComPtr<ICoreWebView2Settings> st;
    if (SUCCEEDED(view->get_Settings(&st)) && st) {
        st->put_AreDevToolsEnabled(FALSE);
        st->put_IsStatusBarEnabled(FALSE); // a console page has no status line in its corner
    }
    ctl->put_IsVisible(shown ? TRUE : FALSE);
    ApplyBounds();
    EventRegistrationToken tok;
    view->add_NavigationStarting(
        Make<ICoreWebView2NavigationStartingEventHandler, HRESULT(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*)>(
            [g](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                if (g == gen) loading = true;
                return S_OK;
            })
            .Get(),
        &tok);
    view->add_NavigationCompleted(
        Make<ICoreWebView2NavigationCompletedEventHandler, HRESULT(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*)>(
            [g](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                if (g != gen) return S_OK;
                BOOL ok = FALSE;
                if (a) a->get_IsSuccess(&ok);
                Note(L"navigation completed (%s)", ok ? L"success" : L"error page");
                loading = false;
                if (state == State::Loading) state = State::Ready; // an error page is a page too
                return S_OK;
            })
            .Get(),
        &tok);
    ComPtr<ICoreWebView2_2> view2;
    if (SUCCEEDED(view.As(&view2)))
        view2->add_DOMContentLoaded(
            Make<ICoreWebView2DOMContentLoadedEventHandler, HRESULT(ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*)>(
                [g](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
                    // The page can be shown once its text is there; pictures
                    // and the rest arrive in front of the user.
                    if (g == gen && state == State::Loading) state = State::Ready;
                    return S_OK;
                })
                .Get(),
            &tok);
    // A link or a script that wants a new window: the same view goes there.
    view->add_NewWindowRequested(
        Make<ICoreWebView2NewWindowRequestedEventHandler, HRESULT(ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*)>(
            [](ICoreWebView2* s, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                if (!a) return S_OK;
                a->put_Handled(TRUE);
                LPWSTR u = nullptr;
                if (SUCCEEDED(a->get_Uri(&u))) {
                    const std::wstring to = Take(u);
                    Note(L"new window asked for: %s (opened in the same view)", to.c_str());
                    if (s && !to.empty()) s->Navigate(to.c_str());
                }
                return S_OK;
            })
            .Get(),
        &tok);
    view->add_DocumentTitleChanged(Make<ICoreWebView2DocumentTitleChangedEventHandler, HRESULT(ICoreWebView2*, IUnknown*)>(
                                       [g](ICoreWebView2* s, IUnknown*) -> HRESULT {
                                           LPWSTR t = nullptr;
                                           if (g == gen && s && SUCCEEDED(s->get_DocumentTitle(&t))) title = Take(t);
                                           return S_OK;
                                       })
                                       .Get(),
                                   &tok);
    view->add_SourceChanged(Make<ICoreWebView2SourceChangedEventHandler, HRESULT(ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*)>(
                                [g](ICoreWebView2* s, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                    LPWSTR u = nullptr;
                                    if (g == gen && s && SUCCEEDED(s->get_Source(&u))) url = Take(u);
                                    return S_OK;
                                })
                                .Get(),
                            &tok);
    view->add_HistoryChanged(Make<ICoreWebView2HistoryChangedEventHandler, HRESULT(ICoreWebView2*, IUnknown*)>(
                                 [g](ICoreWebView2* s, IUnknown*) -> HRESULT {
                                     if (g != gen || !s) return S_OK;
                                     BOOL b = FALSE, f = FALSE;
                                     s->get_CanGoBack(&b);
                                     s->get_CanGoForward(&f);
                                     canBack = b != FALSE;
                                     canForward = f != FALSE;
                                     return S_OK;
                                 })
                                 .Get(),
                             &tok);
    view->add_ContainsFullScreenElementChanged(
        Make<ICoreWebView2ContainsFullScreenElementChangedEventHandler, HRESULT(ICoreWebView2*, IUnknown*)>(
            [g](ICoreWebView2* s, IUnknown*) -> HRESULT {
                BOOL f = FALSE;
                if (g != gen || !s) return S_OK;
                s->get_ContainsFullScreenElement(&f);
                fullScreen = f != FALSE;
                ApplyBounds();
                return S_OK;
            })
            .Get(),
        &tok);
    view->add_ProcessFailed(
        Make<ICoreWebView2ProcessFailedEventHandler, HRESULT(ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*)>(
            [g](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND k = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                if (a) a->get_ProcessFailedKind(&k);
                Note(L"process failed, kind %d", static_cast<int>(k));
                // A page's own process can be restarted by reloading; the
                // browser's or a frozen page's cannot: the page closes.
                if (g == gen && (k == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED ||
                                 k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                                 k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE))
                    state = State::Failed;
                return S_OK;
            })
            .Get(),
        &tok);
    // Keys the page does not keep: Esc leaves (as B), F11 is the menu's own
    // full-screen key. Both go to the menu's window as if pressed there. Esc
    // stays the page's while it shows something in full screen (a video).
    ctl->add_AcceleratorKeyPressed(
        Make<ICoreWebView2AcceleratorKeyPressedEventHandler, HRESULT(ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs*)>(
            [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind;
                UINT key = 0;
                COREWEBVIEW2_PHYSICAL_KEY_STATUS ks = {};
                if (!a || FAILED(a->get_KeyEventKind(&kind)) || FAILED(a->get_VirtualKey(&key))) return S_OK;
                a->get_PhysicalKeyStatus(&ks);
                const bool esc = key == VK_ESCAPE && !fullScreen;
                if ((esc || key == VK_F11) && (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN)) {
                    a->put_Handled(TRUE);
                    if (!ks.WasKeyDown && host) PostMessageW(host, WM_KEYDOWN, key, 1);
                }
                return S_OK;
            })
            .Get(),
        &tok);
    state = State::Loading;
    loading = true;
    url = wantUrl;
    view->Navigate(wantUrl.c_str());
    if (shown) ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

// The view's rectangle on the screen; false when there is no view shown.
bool ViewOnScreen(RECT& r) {
    if (!ctl || !shown || !host) return false;
    r = bounds;
    if (fullScreen) GetClientRect(host, &r);
    MapWindowPoints(host, nullptr, reinterpret_cast<POINT*>(&r), 2);
    return r.right > r.left && r.bottom > r.top;
}
// The pointer is over the page itself (not over another window in front of it).
bool PointerOnPage(POINT& p) {
    RECT r;
    if (!ViewOnScreen(r) || !GetCursorPos(&p) || !PtInRect(&r, p)) return false;
    HWND under = WindowFromPoint(p);
    return under && IsChild(host, under);
}

void SendMouse(DWORD flags, LONG data) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = static_cast<DWORD>(data);
    SendInput(1, &in, sizeof in);
}

} // namespace

bool Available() {
    static int known = -1;
    if (known < 0) {
        LPWSTR v = nullptr;
        known = SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &v)) && v ? 1 : 0;
        Take(v);
    }
    return known == 1;
}

void Attach(HWND h) {
    host = h;
}

void Prepare() {
    if (!Available()) return;
    envWanted = true;
    StartEnvironment();
}

namespace {
void CloseView() {
    gen++;
    if (ctl) {
        if (shown && host) SetFocus(host); // the keyboard back to the menu before its holder goes
        ctl->Close();
    }
    view.Reset();
    ctl.Reset();
    state = State::Closed;
    loading = shown = fullScreen = canBack = canForward = false;
    wantUrl.clear();
    title.clear();
    url.clear();
}
} // namespace

bool Open(const std::wstring& u, const std::string& profile, uint32_t ground) {
    if (!Available()) return false;
    CloseView();
    envWanted = true;
    wantUrl = u;
    wantProfile = profile;
    wantGround = ground;
    title.clear();
    url = u;
    if (!host) {
        state = State::Offscreen;
        return true;
    }
    state = State::Starting;
    if (env) MakeController();
    else StartEnvironment();
    return true;
}

void Close() {
    CloseView();
    // Letting go of the environment as well ends the browser's processes; the
    // next page starts them again (a second or so, hidden by the opening).
    env.Reset();
    envWanted = false;
}

State Now() {
    return state;
}
bool Loading() {
    return loading;
}

void Place(float x, float y, float w, float h) {
    if (!host) return;
    // Layout units to the window's pixels, read off the drawing layer's own
    // mapping (which knows the letterboxing, the screen-size setting and the zoom).
    float x0, y0, x1, y1;
    gfx::ToDesign(0, 0, x0, y0);
    gfx::ToDesign(1000, 1000, x1, y1);
    if (x1 == x0 || y1 == y0) return;
    const float sx = 1000.0f / (x1 - x0), sy = 1000.0f / (y1 - y0);
    RECT r = {lroundf((x - x0) * sx), lroundf((y - y0) * sy), lroundf((x + w - x0) * sx), lroundf((y + h - y0) * sy)};
    if (!EqualRect(&r, &bounds)) {
        bounds = r;
        ApplyBounds();
    }
    // Pop-ups the page opens (a select box) are placed from the window's place.
    RECT wr;
    GetWindowRect(host, &wr);
    if (ctl && (wr.left != hostAt.x || wr.top != hostAt.y)) ctl->NotifyParentWindowPositionChanged();
    hostAt = {wr.left, wr.top};
}

void Show(bool on) {
    if (on == shown) return;
    shown = on;
    if (!ctl) return;
    if (on && host) {
        // The menu's window must not paint over its child (the full-screen
        // switch rewrites its style).
        const LONG_PTR style = GetWindowLongPtrW(host, GWL_STYLE);
        if (!(style & WS_CLIPCHILDREN)) SetWindowLongPtrW(host, GWL_STYLE, style | WS_CLIPCHILDREN);
    }
    ctl->put_IsVisible(on ? TRUE : FALSE);
    if (on) ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    else if (host && GetFocus() != host && IsChild(host, GetFocus())) SetFocus(host);
}
bool Shown() {
    return shown && ctl;
}
bool FullScreen() {
    return fullScreen;
}
std::wstring Title() {
    return title;
}
std::wstring Url() {
    return url;
}
bool CanBack() {
    return canBack;
}
bool CanForward() {
    return canForward;
}
void Back() {
    if (view && canBack) view->GoBack();
}
void Forward() {
    if (view && canForward) view->GoForward();
}
void Reload() {
    if (view) view->Reload();
}

void Snapshot() {
    snap.clear();
    if (!view) return;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return;
    const unsigned g = gen;
    view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
                         Make<ICoreWebView2CapturePreviewCompletedHandler, HRESULT(HRESULT)>([g, stream](HRESULT hr) -> HRESULT {
                             if (g != gen || FAILED(hr)) return S_OK;
                             LARGE_INTEGER zero = {};
                             ULARGE_INTEGER end = {};
                             HGLOBAL mem = nullptr;
                             if (FAILED(stream->Seek(zero, STREAM_SEEK_CUR, &end)) || FAILED(GetHGlobalFromStream(stream.Get(), &mem)))
                                 return S_OK;
                             if (const auto* p = static_cast<const uint8_t*>(GlobalLock(mem))) {
                                 snap.assign(p, p + end.QuadPart);
                                 GlobalUnlock(mem);
                             }
                             return S_OK;
                         }).Get());
}
const std::vector<uint8_t>& SnapshotPng() {
    return snap;
}

bool Pad(float lx, float ly, float rx, float ry) {
    const double t = Seconds();
    const float dt = static_cast<float>((std::min)(0.1, t - padLast));
    padLast = t;
    RECT r;
    if (!ViewOnScreen(r)) return false;
    // Past a dead zone, speed grows with the square of the tilt: slow and
    // precise near the middle, across the page in about a second at full tilt.
    auto curve = [](float v) {
        const float a = fabsf(v);
        if (a < 0.18f) return 0.0f;
        const float u = (a - 0.18f) / 0.82f;
        return (v < 0 ? -1.0f : 1.0f) * u * u;
    };
    const float speed = 1.1f * static_cast<float>(r.bottom - r.top); // pixels a second at full tilt
    moveX += curve(lx) * speed * dt;
    moveY -= curve(ly) * speed * dt; // the stick's up is the screen's up
    const int mx = static_cast<int>(moveX), my = static_cast<int>(moveY);
    if (mx || my) {
        POINT p;
        GetCursorPos(&p);
        // A pointer that is elsewhere starts from the middle of the page.
        if (!PtInRect(&r, p)) p = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        p.x = (std::max)(r.left, (std::min)(r.right - 1, p.x + mx));
        p.y = (std::max)(r.top, (std::min)(r.bottom - 1, p.y + my));
        SetCursorPos(p.x, p.y);
        moveX -= mx;
        moveY -= my;
    }
    // The right stick scrolls: up to 20 wheel notches a second.
    wheelY += curve(ry) * WHEEL_DELTA * 20 * dt;
    wheelX += curve(rx) * WHEEL_DELTA * 20 * dt;
    POINT p;
    const int wy = static_cast<int>(wheelY), wx = static_cast<int>(wheelX);
    if ((wy || wx) && PointerOnPage(p)) {
        if (wy) SendMouse(MOUSEEVENTF_WHEEL, wy);
        if (wx) SendMouse(MOUSEEVENTF_HWHEEL, wx);
    } else if (wy || wx) {
        Scroll(-static_cast<float>(wx) / WHEEL_DELTA, static_cast<float>(wy) / WHEEL_DELTA);
    }
    wheelY -= static_cast<float>(wy);
    wheelX -= static_cast<float>(wx);
    return true;
}

void Click() {
    POINT p;
    if (!PointerOnPage(p)) return;
    SendMouse(MOUSEEVENTF_LEFTDOWN, 0);
    SendMouse(MOUSEEVENTF_LEFTUP, 0);
}

void Scroll(float dx, float dy) {
    if (!view || !shown) return;
    POINT p;
    if (PointerOnPage(p)) {
        // Over the page the wheel reaches whatever is under the pointer (a
        // list inside the page scrolls by itself).
        if (dy != 0) SendMouse(MOUSEEVENTF_WHEEL, static_cast<LONG>(dy * WHEEL_DELTA));
        if (dx != 0) SendMouse(MOUSEEVENTF_HWHEEL, static_cast<LONG>(-dx * WHEEL_DELTA));
        return;
    }
    // Elsewhere (the pointer over the menu's header, or hidden) the page as a
    // whole scrolls. Never the wheel then: it would come back to the menu's
    // window as a wheel turn of its own.
    wchar_t js[96];
    swprintf_s(js, L"window.scrollBy({left:%d,top:%d,behavior:'smooth'})", static_cast<int>(dx * 100), static_cast<int>(-dy * 100));
    view->ExecuteScript(js, nullptr);
}

// ---- --webtest -------------------------------------------------------------

namespace {

FILE* testLog = nullptr;
void TestLogLine(const wchar_t* s) {
    if (!testLog) return;
    fwprintf(testLog, L"%8.3f  %s\n", Seconds(), s);
    fflush(testLog);
}

// The browser processes this program started (directly or through each
// other), and all of the system's, by name.
void CountProcesses(int& ours, int& all) {
    ours = all = 0;
    HANDLE snapH = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapH == INVALID_HANDLE_VALUE) return;
    std::map<DWORD, DWORD> parent;
    std::vector<DWORD> browsers;
    PROCESSENTRY32W e = {sizeof e};
    for (BOOL ok = Process32FirstW(snapH, &e); ok; ok = Process32NextW(snapH, &e)) {
        parent[e.th32ProcessID] = e.th32ParentProcessID;
        if (!_wcsicmp(e.szExeFile, L"msedgewebview2.exe")) browsers.push_back(e.th32ProcessID);
    }
    CloseHandle(snapH);
    const DWORD me = GetCurrentProcessId();
    for (DWORD pid : browsers) {
        all++;
        DWORD p = pid;
        for (int depth = 0; depth < 6 && p; depth++) {
            auto it = parent.find(p);
            if (it == parent.end()) break;
            p = it->second;
            if (p == me) {
                ours++;
                break;
            }
        }
    }
}

// Messages until `done` says so or `seconds` have passed. True when done.
bool PumpUntil(const std::function<bool()>& done, double seconds) {
    const double end = Seconds() + seconds;
    while (!done()) {
        if (Seconds() > end) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return true;
}

std::wstring FileUrl(const std::wstring& path) {
    std::wstring u = L"file:///";
    for (wchar_t c : path) u += c == L'\\' ? L'/' : c;
    return u;
}

} // namespace

int SelfTest(HINSTANCE inst, const std::wstring& page, int cycles, const std::wstring& logFile) {
    if (_wfopen_s(&testLog, logFile.c_str(), L"w, ccs=UTF-8") || !testLog) return 2;
    logFn = TestLogLine;
    int failures = 0;
    auto check = [&](bool ok, const wchar_t* what) {
        Note(L"%s  %s", ok ? L"PASS" : L"FAIL", what);
        if (!ok) failures++;
    };
    LPWSTR ver = nullptr;
    GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
    Note(L"runtime: %s", ver ? ver : L"(none)");
    Take(ver);
    check(Available(), L"runtime available");
    int ours = 0, all = 0;
    CountProcesses(ours, all);
    Note(L"before: msedgewebview2 processes of ours %d, of the whole system %d", ours, all);
    const int allBefore = all;

    // An NSW-Home-like host: the same kind of top-level window, never shown,
    // never activated, off every screen.
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"NSWHomeWebTest";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"NSW-Home web test",
                             WS_POPUP | WS_CLIPCHILDREN, -32000, -32000, 1280, 720, nullptr, nullptr, inst, nullptr);
    check(h != nullptr && !IsWindowVisible(h), L"hidden host window made");
    if (!h) return 3;
    Attach(h);

    PROCESS_MEMORY_COUNTERS_EX mem0 = {sizeof mem0};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem0), sizeof mem0);
    DWORD handles0 = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles0);
    const DWORD gdi0 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user0 = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);

    const std::wstring dir = logFile.substr(0, logFile.find_last_of(L"\\/") + 1); // the page's picture goes beside the log
    for (int c = 1; c <= cycles; c++) {
        Note(L"---- cycle %d", c);
        const double t0 = Seconds();
        if (c == 1) Prepare(); // as the button press does
        check(Open(FileUrl(page), "test", 0x2D2D2D), L"Open");
        bounds = {0, 85, 1280, 645};
        ApplyBounds();
        const bool ready = PumpUntil([] { return state == State::Ready || state == State::Failed; }, 30);
        check(ready && state == State::Ready, L"first page shown");
        Note(L"ready after %.0f ms", (Seconds() - t0) * 1000);
        Show(true);
        PumpUntil([] { return !loading && title == L"NSW-Home web test 1"; }, 10);
        check(title == L"NSW-Home web test 1", L"its title read");
        CountProcesses(ours, all);
        Note(L"open: msedgewebview2 of ours %d", ours);
        check(ours > 0, L"browser processes running");
        // A link that asks for a new window: the same view goes there.
        if (view) view->ExecuteScript(L"document.getElementById('go').click()", nullptr);
        const bool second = PumpUntil([] { return title == L"NSW-Home web test 2" && !loading; }, 10);
        check(second, L"new-window link opened in the same view");
        check(url.find(L"page2.html") != std::wstring::npos, L"address followed");
        PumpUntil([] { return canBack; }, 5);
        check(canBack, L"history has a way back");
        Back();
        const bool back = PumpUntil([] { return title == L"NSW-Home web test 1" && !loading; }, 10);
        check(back, L"back went back");
        // A plain navigation by script, as a page's own link does.
        if (view) view->ExecuteScript(L"location.href='page2.html#again'", nullptr);
        check(PumpUntil([] { return url.find(L"#again") != std::wstring::npos; }, 10), L"script navigation followed");
        Snapshot();
        const bool pic = PumpUntil([] { return !snap.empty(); }, 10);
        check(pic && snap.size() > 1000, L"picture of the page taken");
        if (pic && c == 1) {
            FILE* f = nullptr;
            if (!_wfopen_s(&f, (dir + L"snapshot.png").c_str(), L"wb") && f) {
                fwrite(snap.data(), 1, snap.size(), f);
                fclose(f);
            }
            Note(L"picture: %u bytes, saved as snapshot.png beside the log", static_cast<unsigned>(snap.size()));
        }
        Show(false);
        Close();
        const double t1 = Seconds();
        const bool freed = PumpUntil([&] {
            CountProcesses(ours, all);
            return ours == 0;
        }, 20);
        check(freed, L"closing ended every browser process");
        Note(L"after close: of ours %d (%.0f ms), whole system %d", ours, (Seconds() - t1) * 1000, all);
    }
    // Opening and closing again in a hurry: closed before the view is made.
    Note(L"---- open and close at once");
    Open(FileUrl(page), "test", 0x2D2D2D);
    PumpUntil([] { return false; }, 0.05);
    Close();
    PumpUntil([&] {
        CountProcesses(ours, all);
        return ours == 0;
    }, 20);
    check(ours == 0, L"a page closed while starting leaves nothing behind");

    PROCESS_MEMORY_COUNTERS_EX mem1 = {sizeof mem1};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem1), sizeof mem1);
    DWORD handles1 = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles1);
    Note(L"this process: private bytes %.1f -> %.1f MB, handles %lu -> %lu, GDI %lu -> %lu, USER %lu -> %lu",
         mem0.PrivateUsage / 1048576.0, mem1.PrivateUsage / 1048576.0, handles0, handles1, gdi0,
         GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user0, GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    CountProcesses(ours, all);
    Note(L"end: msedgewebview2 of ours %d, whole system %d (before: %d)", ours, all, allBefore);
    host = nullptr;
    DestroyWindow(h);
    UnregisterClassW(wc.lpszClassName, inst);
    Note(L"%d failure(s)", failures);
    logFn = nullptr;
    fclose(testLog);
    testLog = nullptr;
    return failures ? 1 : 0;
}

} // namespace web
