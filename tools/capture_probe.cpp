// Test tool for src\wincap.cpp: proves that a capture returns another program's real pixels.
//
//   capture_probe stand <gdi|d3d|gl> <x> <y> <w> <h> <seconds>
//       A stand-in for a game: a borderless window that is never activated, with a child
//       window that draws a known pattern (a 8 x 6 grid of fixed colours and, in the bottom
//       row, a white bar that moves 4 pixels a frame). d3d presents through a flip-model swap
//       chain, gl through an OpenGL context: the two ways the emulators put a picture up.
//   capture_probe cover <x> <y> <w> <h> <seconds> [thumb]
//       A plain top-most magenta window (never activated) to lay over the stand-in. With
//       "thumb" it shows the stand-in's surface inside itself through a DWM thumbnail: the
//       way NSW-Home shows a game whose emulator window stays underneath its own.
//   capture_probe grab <out.jpg|-> [noscreen]
//       Run from another process: finds the stand-in, captures its child's rectangle twice
//       0.3 s apart, compares both with the pattern, prints the result, saves the first.
//       Exit code 0 only when every cell matches and the bar moved between the two.
//   capture_probe notice <x> <y>
//       Shows a 300 x 60 notice for a second and prints the foreground window before, during
//       and after: all three must be the same.
#include "../src/wincap.h"
#include <d3d11_1.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <gl/GL.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {

constexpr int kCols = 8, kRows = 6, kBar = 16;
const wchar_t* const kTitle = L"NSWHomeCaptureStandIn";

void CellColour(int i, int j, int& r, int& g, int& b) {
    r = 30 + i * 28, g = 40 + j * 36, b = (i * 37 + j * 91 + 20) % 256;
}

// Cell (i, j) of a w x h picture.
RECT Cell(int i, int j, int w, int h) {
    return {i * w / kCols, j * h / kRows, (i + 1) * w / kCols, (j + 1) * h / kRows};
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        ValidateRect(h, nullptr);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND MakeWindow(const wchar_t* title, DWORD ex, DWORD style, int x, int y, int w, int h, HWND parent, HBRUSH bg = nullptr) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NSWHomeProbe";
        RegisterClassExW(&wc);
        reg = true;
    }
    HWND wnd = CreateWindowExW(ex, L"NSWHomeProbe", title, style, x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (bg) SetClassLongPtrW(wnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(bg));
    return wnd;
}

void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int Stand(const wchar_t* mode, int x, int y, int w, int h, int seconds) {
    HWND top = MakeWindow(kTitle, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP | WS_CLIPCHILDREN, x, y, w, h, nullptr);
    HWND child = MakeWindow(L"surface", 0, WS_CHILD | WS_VISIBLE, 0, 0, w, h, top);
    ShowWindow(top, SW_SHOWNOACTIVATE);

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11DeviceContext1* ctx1 = nullptr;
    IDXGISwapChain1* chain = nullptr;
    HDC gdc = nullptr;
    const bool d3d = !wcscmp(mode, L"d3d"), gl = !wcscmp(mode, L"gl");
    if (d3d) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
            return 2;
        IDXGIDevice* xdev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        dev->QueryInterface(&xdev);
        xdev->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = w, sd.Height = h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(dev, child, &sd, nullptr, nullptr, &chain))) return 3;
        factory->MakeWindowAssociation(child, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(ctx->QueryInterface(&ctx1))) return 4;
    } else if (gl) {
        gdc = GetDC(child);
        PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
        SetPixelFormat(gdc, ChoosePixelFormat(gdc, &pfd), &pfd);
        HGLRC rc = wglCreateContext(gdc);
        if (!rc || !wglMakeCurrent(gdc, rc)) return 5;
    }

    const ULONGLONG end = GetTickCount64() + seconds * 1000ull;
    for (int frame = 0; GetTickCount64() < end; frame++) {
        Pump();
        const int barX = (frame * 4) % (w - kBar);
        const RECT last = Cell(0, kRows - 1, w, h);
        const RECT bar = {barX, last.top, barX + kBar, h};
        if (d3d) {
            ID3D11Texture2D* back = nullptr;
            ID3D11RenderTargetView* view = nullptr;
            chain->GetBuffer(0, IID_PPV_ARGS(&back));
            dev->CreateRenderTargetView(back, nullptr, &view);
            for (int j = 0; j < kRows; j++)
                for (int i = 0; i < kCols; i++) {
                    int r, g, b;
                    CellColour(i, j, r, g, b);
                    const float c[4] = {r / 255.0f, g / 255.0f, b / 255.0f, 1};
                    const RECT rc = Cell(i, j, w, h);
                    ctx1->ClearView(view, c, &rc, 1);
                }
            const float white[4] = {1, 1, 1, 1};
            ctx1->ClearView(view, white, &bar, 1);
            view->Release();
            back->Release();
            chain->Present(1, 0);
        } else if (gl) {
            glEnable(GL_SCISSOR_TEST);
            auto fill = [&](const RECT& rc, float r, float g, float b) {
                glScissor(rc.left, h - rc.bottom, rc.right - rc.left, rc.bottom - rc.top); // OpenGL counts rows from the bottom
                glClearColor(r, g, b, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            };
            for (int j = 0; j < kRows; j++)
                for (int i = 0; i < kCols; i++) {
                    int r, g, b;
                    CellColour(i, j, r, g, b);
                    fill(Cell(i, j, w, h), r / 255.0f, g / 255.0f, b / 255.0f);
                }
            fill(bar, 1, 1, 1);
            SwapBuffers(gdc);
            Sleep(8);
        } else {
            HDC dc = GetDC(child);
            for (int j = 0; j < kRows; j++)
                for (int i = 0; i < kCols; i++) {
                    int r, g, b;
                    CellColour(i, j, r, g, b);
                    const RECT rc = Cell(i, j, w, h);
                    HBRUSH br = CreateSolidBrush(RGB(r, g, b));
                    FillRect(dc, &rc, br);
                    DeleteObject(br);
                }
            FillRect(dc, &bar, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            ReleaseDC(child, dc);
            Sleep(16);
        }
    }
    return 0;
}

int Cover(int x, int y, int w, int h, int seconds, bool thumb) {
    HWND top = MakeWindow(L"NSWHomeCaptureCover", WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WS_POPUP, x, y, w, h, nullptr);
    ShowWindow(top, SW_SHOWNOACTIVATE);
    HWND stand = FindWindowW(L"NSWHomeProbe", kTitle);
    HTHUMBNAIL th = nullptr;
    if (thumb && stand && SUCCEEDED(DwmRegisterThumbnail(top, stand, &th))) {
        RECT sr;
        GetClientRect(stand, &sr);
        DWM_THUMBNAIL_PROPERTIES tp = {};
        tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        tp.rcDestination = {w / 4, h / 4, w, h}; // the top and left quarters stay magenta, to tell the two apart
        tp.rcSource = sr;
        tp.fVisible = TRUE;
        tp.fSourceClientAreaOnly = TRUE;
        puts(SUCCEEDED(DwmUpdateThumbnailProperties(th, &tp)) ? "thumbnail: shown" : "thumbnail: FAILED");
        fflush(stdout);
    }
    HBRUSH br = CreateSolidBrush(RGB(255, 0, 255));
    const ULONGLONG end = GetTickCount64() + seconds * 1000ull;
    while (GetTickCount64() < end) {
        Pump();
        HDC dc = GetDC(top);
        RECT rc = {0, 0, w, h};
        if (!th) FillRect(dc, &rc, br);
        else {
            // (Painting under a thumbnail is harmless, but keep it to the bare part.)
            RECT a = {0, 0, w, h / 4}, b = {0, 0, w / 4, h};
            FillRect(dc, &a, br);
            FillRect(dc, &b, br);
        }
        ReleaseDC(top, dc);
        Sleep(30);
    }
    return 0;
}

// Compares a capture with the pattern. Returns the number of cells that are
// wrong (the middle of each of the top five rows' cells, and its corners 3
// pixels in); `barX` is where the white bar was found, -1 if nowhere.
int Check(const wincap::Picture& p, int& barX, int& worst) {
    int bad = 0;
    worst = 0;
    auto at = [&](int x, int y) { return &p.bgra[(static_cast<size_t>(y) * p.w + x) * 4]; };
    for (int j = 0; j < kRows - 1; j++)
        for (int i = 0; i < kCols; i++) {
            int r, g, b;
            CellColour(i, j, r, g, b);
            const RECT c = Cell(i, j, p.w, p.h);
            const POINT pts[5] = {{(c.left + c.right) / 2, (c.top + c.bottom) / 2},
                                  {c.left + 3, c.top + 3},
                                  {c.right - 4, c.top + 3},
                                  {c.left + 3, c.bottom - 4},
                                  {c.right - 4, c.bottom - 4}};
            bool wrong = false;
            for (const POINT& q : pts) {
                const uint8_t* px = at(q.x, q.y);
                const int d = (std::max)((std::max)(abs(px[2] - r), abs(px[1] - g)), abs(px[0] - b));
                worst = (std::max)(worst, d);
                wrong = wrong || d > 1;
            }
            bad += wrong;
        }
    barX = -1;
    const int y = p.h - p.h / kRows / 2;
    for (int x = 0; x + kBar <= p.w && barX < 0; x++) {
        bool white = true;
        for (int k = 0; k < kBar && white; k++) {
            const uint8_t* px = at(x + k, y);
            white = px[0] > 250 && px[1] > 250 && px[2] > 250;
        }
        if (white) barX = x;
    }
    return bad;
}

int GrabTest(const wchar_t* out, bool allowScreen) {
    HWND top = FindWindowW(L"NSWHomeProbe", kTitle);
    HWND child = top ? GetWindow(top, GW_CHILD) : nullptr;
    if (!child) {
        printf("no stand-in window\n");
        return 2;
    }
    RECT crop;
    GetWindowRect(child, &crop);
    HWND fgBefore = GetForegroundWindow();
    int result = 0, bars[2] = {-1, -1};
    for (int k = 0; k < 2; k++) {
        wincap::Picture p;
        const ULONGLONG t0 = GetTickCount64();
        if (!wincap::Grab(top, crop, allowScreen, p)) {
            printf("grab %d: FAILED\n", k);
            return 3;
        }
        const ULONGLONG t1 = GetTickCount64();
        int worst = 0;
        const int bad = Check(p, bars[k], worst);
        printf("grab %d: %dx%d at (%ld,%ld) via %s in %llu ms; wrong cells %d of %d (largest difference %d); bar at x=%d\n", k, p.w,
               p.h, crop.left, crop.top, p.method, t1 - t0, bad, kCols * (kRows - 1), worst, bars[k]);
        if (bad) result = 1;
        if (k == 0 && wcscmp(out, L"-")) {
            const ULONGLONG s0 = GetTickCount64();
            const bool saved = wincap::SaveJpeg(p, out);
            printf("saved: %s in %llu ms\n", saved ? "yes" : "NO", GetTickCount64() - s0);
            if (!saved) result = 1;
        }
        if (k == 0) Sleep(300);
    }
    if (bars[0] < 0 || bars[1] < 0 || bars[0] == bars[1]) {
        printf("the bar did not move between the two captures: not a live picture\n");
        result = 1;
    }
    printf("foreground window unchanged: %s\n", fgBefore == GetForegroundWindow() ? "yes" : "NO");
    printf(result ? "FAIL\n" : "PASS\n");
    return result;
}

int NoticeTest(int x, int y) {
    const int w = 300, h = 60;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) px[i] = px[i + 1] = px[i + 2] = 46, px[i + 3] = 255;
    HWND before = GetForegroundWindow();
    wincap::ShowNotice(px.data(), w, h, x, y, 1000);
    HWND during = nullptr;
    bool shown = false;
    const ULONGLONG end = GetTickCount64() + 1300;
    while (GetTickCount64() < end) {
        Pump();
        if (HWND n = wincap::NoticeWindow()) {
            shown = shown || IsWindowVisible(n);
            during = GetForegroundWindow();
        }
        Sleep(5);
    }
    HWND after = GetForegroundWindow();
    const bool gone = !wincap::NoticeWindow();
    printf("notice shown: %s, removed itself: %s\n", shown ? "yes" : "NO", gone ? "yes" : "NO");
    printf("foreground before %p, during %p, after %p\n", static_cast<void*>(before), static_cast<void*>(during),
           static_cast<void*>(after));
    const bool ok = shown && gone && before == during && before == after;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto num = [&](int i) { return _wtoi(argv[i]); };
    if (argc >= 8 && !wcscmp(argv[1], L"stand")) return Stand(argv[2], num(3), num(4), num(5), num(6), num(7));
    if (argc >= 7 && !wcscmp(argv[1], L"cover")) return Cover(num(2), num(3), num(4), num(5), num(6), argc >= 8 && !wcscmp(argv[7], L"thumb"));
    if (argc >= 3 && !wcscmp(argv[1], L"grab")) return GrabTest(argv[2], !(argc >= 4 && !wcscmp(argv[3], L"noscreen")));
    if (argc >= 4 && !wcscmp(argv[1], L"notice")) return NoticeTest(num(2), num(3));
    printf("usage: see the top of tools\\capture_probe.cpp\n");
    return 64;
}
