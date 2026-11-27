// Settles, with standin.exe as the "emulator", what NSW-Home's game viewer is
// built on: a DWM thumbnail of a window that lies under ours and is cloaked.
//
//   probe.exe <gl|vk> <x> <y> <w> <h> [cloak] [framed] [direct] [novsync] [f11] [secs N] [scale S]
//
// x y w h: the viewer's rectangle in real screen pixels (a top-most window that
// is never activated). The stand-in gets the same rectangle, so it is covered
// completely. cloak: the stand-in's owner (a hidden window of ours) is cloaked,
// which the stand-in inherits. framed: the stand-in has a title bar and its
// picture is a child between two bars. direct: no viewer and no thumbnail, the
// stand-in is measured as it appears on screen by itself (the baseline).
// scale S: the viewer is S percent of the stand-in's size (scaling check).
// f11: after measuring, the stand-in is told to switch between popup and
// framed, and the time the picture is wrong on screen is measured.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Shared {
    volatile LONG frame, fpsTimes10, keys, clicks, active, framed;
    volatile LONGLONG qpc;
};

static HWND viewer, ownerWnd, standTop, standChild;
static HTHUMBNAIL thumb;
static RECT lastSrc;

static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATEANDEAT;
    return DefWindowProcW(h, m, w, l);
}

static BOOL CALLBACK FindChild(HWND h, LPARAM) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcsncmp(cls, L"NativeWindow", 12)) {
        standChild = h;
        return FALSE;
    }
    return TRUE;
}

static double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

// The picture's rectangle inside its top-level window, and the thumbnail set to it.
static bool UpdateThumb(int vw, int vh) {
    RECT t, c;
    GetWindowRect(standTop, &t);
    GetWindowRect(standChild, &c);
    RECT src = {c.left - t.left, c.top - t.top, c.right - t.left, c.bottom - t.top};
    if (EqualRect(&src, &lastSrc)) return false;
    lastSrc = src;
    DWM_THUMBNAIL_PROPERTIES p = {};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcDestination = {0, 0, vw, vh};
    p.rcSource = src;
    p.opacity = 255;
    p.fVisible = TRUE;
    p.fSourceClientAreaOnly = FALSE;
    HRESULT hr = DwmUpdateThumbnailProperties(thumb, &p);
    if (FAILED(hr)) printf("DwmUpdateThumbnailProperties failed %08lx\n", hr);
    return true;
}

struct Grab {
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = nullptr;
    uint32_t* bits = nullptr;
    int w = 0, h = 0;
    void Size(int nw, int nh) {
        if (bmp) DeleteObject(bmp);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = nw;
        bi.bmiHeader.biHeight = -nh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        SelectObject(mem, bmp);
        w = nw, h = nh;
    }
    void At(int x, int y) {
        BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
        GdiFlush();
    }
};

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 6) return 2;
    std::wstring mode = argv[1];
    int x = _wtoi(argv[2]), y = _wtoi(argv[3]), w = _wtoi(argv[4]), h = _wtoi(argv[5]);
    bool cloak = false, framed = false, direct = false, novsync = false, f11 = false, fit = false;
    int secs = 3, scale = 100;
    for (int i = 6; i < argc; i++) {
        if (!wcscmp(argv[i], L"cloak")) cloak = true;
        if (!wcscmp(argv[i], L"framed")) framed = true;
        if (!wcscmp(argv[i], L"direct")) direct = true;
        if (!wcscmp(argv[i], L"novsync")) novsync = true;
        if (!wcscmp(argv[i], L"f11")) f11 = true;
        if (!wcscmp(argv[i], L"fit")) fit = true; // the viewer takes the picture's own size (exact-copy check of a framed stand-in)
        if (!wcscmp(argv[i], L"secs") && i + 1 < argc) secs = _wtoi(argv[++i]);
        if (!wcscmp(argv[i], L"scale") && i + 1 < argc) scale = _wtoi(argv[++i]);
    }
    int vw = w * scale / 100, vh = h * scale / 100;
    HWND fgBefore = GetForegroundWindow();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    ownerWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (cloak) {
        BOOL on = TRUE;
        HRESULT hr = DwmSetWindowAttribute(ownerWnd, DWMWA_CLOAK, &on, sizeof(on));
        printf("cloak the hidden owner: hr=%08lx\n", hr);
    }
    if (!direct) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = inst;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"NSWHomeProbeViewer";
        RegisterClassW(&wc);
        viewer = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"probe", WS_POPUP, x, y, vw, vh, nullptr,
                                 nullptr, inst, nullptr);
        ShowWindow(viewer, SW_SHOWNOACTIVATE);
    }

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\'));
    wchar_t cmd[512];
    swprintf_s(cmd, L"\"%s\\standin.exe\" %s %d %d %d %d %s %s", dir.c_str(), mode.c_str(), x, y, w, h, framed ? L"" : L"popup",
               novsync ? L"novsync" : L"");
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("could not start standin.exe\n");
        return 1;
    }
    Shared* sh = nullptr;
    for (int i = 0; i < 500 && !standTop; i++) {
        Sleep(10);
        standTop = FindWindowW(L"NSWHomeStandinTop", nullptr);
    }
    if (!standTop) {
        printf("no stand-in window\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }
    // As NSW-Home does with an emulator: our hidden window becomes its owner.
    double t0 = Now();
    SetWindowLongPtrW(standTop, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerWnd));
    if (!direct) SetWindowPos(standTop, viewer, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    printf("adopted in %.1f ms\n", Now() - t0);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\NSWHomeStandin");
    sh = map ? static_cast<Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    if (!sh) {
        printf("no shared state\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }
    for (int i = 0; i < 500 && sh->frame < 30; i++) Sleep(10);
    EnumChildWindows(standTop, FindChild, 0);
    DWORD cloaked = 0;
    DwmGetWindowAttribute(standTop, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    RECT tr, cr;
    GetWindowRect(standTop, &tr);
    GetWindowRect(standChild, &cr);
    printf("stand-in: window %ldx%ld@%ld,%ld picture %ldx%ld@%ld,%ld cloaked=%lu visible=%d frames so far %ld\n", tr.right - tr.left,
           tr.bottom - tr.top, tr.left, tr.top, cr.right - cr.left, cr.bottom - cr.top, cr.left, cr.top, cloaked, IsWindowVisible(standTop),
           sh->frame);

    if (!direct && fit) {
        vw = cr.right - cr.left, vh = cr.bottom - cr.top;
        SetWindowPos(viewer, nullptr, 0, 0, vw, vh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!direct) {
        HRESULT hr = DwmRegisterThumbnail(viewer, standTop, &thumb);
        SIZE sz = {};
        if (SUCCEEDED(hr)) DwmQueryThumbnailSourceSize(thumb, &sz);
        printf("DwmRegisterThumbnail hr=%08lx source size %ldx%ld\n", hr, sz.cx, sz.cy);
        if (FAILED(hr)) {
            TerminateProcess(pi.hProcess, 0);
            return 1;
        }
        UpdateThumb(vw, vh);
    }
    Sleep(700);

    // Where the picture is on screen: the viewer, or the stand-in's own child.
    POINT origin = {x, y};
    int pw = vw, ph = vh;
    if (direct) origin = {cr.left, cr.top}, pw = cr.right - cr.left, ph = cr.bottom - cr.top;

    Grab one;
    one.Size(1, 1);
    struct Sample {
        double t;
        LONG now, scr;
        bool picture;
    };
    std::vector<Sample> samples;
    double start = Now();
    MSG msg;
    while (Now() - start < secs * 1000.0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        one.At(origin.x + pw / 2, origin.y + ph / 2);
        LONG now = sh->frame;
        uint32_t px = one.bits[0];
        LONG r = (px >> 16) & 255, g = (px >> 8) & 255, b = px & 255;
        samples.push_back({Now() - start, now, r | (g << 8), b == 0x55});
    }
    int changes = 0, bad = 0;
    std::vector<int> lag;
    for (size_t i = 0; i < samples.size(); i++) {
        if (!samples[i].picture) {
            bad++;
            continue;
        }
        if (i && samples[i].scr != samples[i - 1].scr) changes++;
        lag.push_back(((samples[i].now & 0xFFFF) - samples[i].scr) & 0xFFFF);
    }
    std::sort(lag.begin(), lag.end());
    printf("%s %s%s%s: %zu samples in %d s (%.0f/s); picture changes on screen %.1f/s; stand-in presents %.1f/s; samples without the picture %d\n",
           direct ? "DIRECT" : "THUMBNAIL", cloak ? "cloaked " : "", framed ? "framed " : "popup ", novsync ? "no-vsync" : "vsync", samples.size(), secs,
           samples.size() / static_cast<double>(secs), changes / static_cast<double>(secs), sh->fpsTimes10 / 10.0, bad);
    if (!lag.empty())
        printf("  frames between 'presented' and 'on screen': min %d  median %d  90%% %d  max %d\n", lag.front(), lag[lag.size() / 2],
               lag[lag.size() * 9 / 10], lag.back());

    // Exact copy: the 64x64 one-pixel checkerboard in the picture's corner (gl only).
    if (mode == L"gl") {
        Grab g;
        g.Size(64, 64);
        g.At(origin.x, origin.y);
        int exact = 0;
        for (int yy = 0; yy < 63; yy++)
            for (int xx = 0; xx < 63; xx++) {
                uint32_t a = g.bits[yy * 64 + xx] & 0xFFFFFF, r = g.bits[yy * 64 + xx + 1] & 0xFFFFFF, d = g.bits[(yy + 1) * 64 + xx] & 0xFFFFFF;
                if ((a == 0 || a == 0xFFFFFF) && r == (a ^ 0xFFFFFF) && d == (a ^ 0xFFFFFF)) exact++;
            }
        printf("  one-pixel checkerboard: %d of %d cells exact (%s)\n", exact, 63 * 63, exact == 63 * 63 ? "1:1 copy" : "scaled or filtered");
    }
    auto scan = [&](const char* when) {
        Grab g;
        g.Size(pw, ph);
        g.At(origin.x, origin.y);
        long picture = 0, magenta = 0, black = 0, other = 0;
        for (int i = 0; i < pw * ph; i++) {
            uint32_t p = g.bits[i] & 0xFFFFFF;
            if ((p & 255) == 0x55) picture++;
            else if (p == 0xFF00FF) magenta++;
            else if (p == 0 || p == 0xFFFFFF) black++; // the checkerboard, or nothing drawn
            else other++;
        }
        printf("  %s: of %d pixels in the viewer: picture %ld, stand-in's own client (magenta) %ld, black/white %ld, anything else (title bar...) %ld\n", when,
               pw * ph, picture, magenta, black, other);
    };
    scan("whole rectangle");
    POINT mid = {origin.x + pw / 2, origin.y + ph / 2};
    HWND at = WindowFromPoint(mid);
    printf("  WindowFromPoint(centre) = %s; stand-in got clicks=%ld keys=%ld active=%ld; foreground unchanged=%d\n",
           at == viewer ? "the viewer" : at == standTop || at == standChild ? "THE STAND-IN" : "another window", sh->clicks, sh->keys, sh->active,
           GetForegroundWindow() == fgBefore);

    if (f11 && !direct) {
        // The stand-in changes its own layout, as an emulator leaving full
        // screen does. How long is something other than its picture on screen?
        for (int round = 0; round < 6; round++) {
            Grab s;
            s.Size(1, 1);
            PostMessageW(standTop, WM_KEYDOWN, VK_F11, 0);
            double from = Now(), firstBad = -1, lastBad = -1, updated = -1;
            int badSamples = 0, total = 0;
            while (Now() - from < 700) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
                if (UpdateThumb(vw, vh)) updated = Now() - from;
                // Top row, where a title bar or the stand-in's own client would be scaled into view.
                s.At(origin.x + pw / 2, origin.y + 3);
                total++;
                if ((s.bits[0] & 255) != 0x55) {
                    badSamples++;
                    if (firstBad < 0) firstBad = Now() - from;
                    lastBad = Now() - from;
                }
            }
            printf("  layout change %d: crop updated after %.1f ms; samples of the top row that were not the picture: %d of %d (from %.1f to %.1f ms)\n",
                   round + 1, updated, badSamples, total, firstBad, lastBad);
        }
        scan("after the layout changes");
    }

    if (thumb) DwmUnregisterThumbnail(thumb);
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 2000);
    if (viewer) DestroyWindow(viewer);
    DestroyWindow(ownerWnd);
    return 0;
}
