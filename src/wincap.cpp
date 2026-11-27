// See wincap.h.
#include "wincap.h"
#include <wincodec.h>
#include <algorithm>

namespace wincap {

namespace {

// A top-down 32-bit bitmap in a memory DC.
struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    uint8_t* bits = nullptr;
    int w = 0, h = 0;
    bool Make(int width, int height) {
        w = width, h = height;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        void* p = nullptr;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        ReleaseDC(nullptr, screen);
        bits = static_cast<uint8_t*>(p);
        if (!dc || !bmp) return false;
        old = static_cast<HBITMAP>(SelectObject(dc, bmp));
        return true;
    }
    ~Dib() {
        if (dc && old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
    }
};

// Copies a rectangle of the bitmap out and says whether anything is in it.
// (A window the compositor has no picture of comes back as zeros.)
bool Take(const Dib& d, int x, int y, int w, int h, Picture& out) {
    out.w = w, out.h = h;
    out.bgra.resize(static_cast<size_t>(w) * h * 4);
    bool any = false;
    for (int r = 0; r < h; r++) {
        const uint8_t* src = d.bits + (static_cast<size_t>(y + r) * d.w + x) * 4;
        uint8_t* dst = &out.bgra[static_cast<size_t>(r) * w * 4];
        memcpy(dst, src, static_cast<size_t>(w) * 4);
        for (int c = 0; c < w; c++) {
            any = any || dst[c * 4] || dst[c * 4 + 1] || dst[c * 4 + 2];
            dst[c * 4 + 3] = 255;
        }
    }
    return any;
}

} // namespace

bool Grab(HWND top, RECT crop, bool allowScreen, Picture& out) {
    RECT wr;
    if (!top || !IsWindow(top) || IsIconic(top) || !GetWindowRect(top, &wr)) return false;
    RECT c;
    if (!IntersectRect(&c, &crop, &wr)) return false;
    const int w = c.right - c.left, h = c.bottom - c.top;

    // PW_RENDERFULLCONTENT (Windows 8.1 on): the compositor's own copy of the
    // window, which includes what a game draws through a swap chain. Nothing
    // shows on screen while it is taken and nothing is asked of the user.
    constexpr UINT kRenderFullContent = 2;
    {
        Dib d;
        if (d.Make(wr.right - wr.left, wr.bottom - wr.top) && PrintWindow(top, d.dc, kRenderFullContent)) {
            GdiFlush();
            if (Take(d, c.left - wr.left, c.top - wr.top, w, h, out)) {
                out.method = "window";
                return true;
            }
        }
    }
    if (!allowScreen) return false;
    Dib d;
    if (!d.Make(w, h)) return false;
    HDC screen = GetDC(nullptr);
    BOOL ok = BitBlt(d.dc, 0, 0, w, h, screen, c.left, c.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    GdiFlush();
    if (!ok) return false;
    Take(d, 0, 0, w, h, out); // a picture that really is black is still a picture
    out.method = "screen";
    return true;
}

bool SaveJpeg(const Picture& p, const std::wstring& file) {
    if (p.w <= 0 || p.h <= 0 || p.bgra.size() < static_cast<size_t>(p.w) * p.h * 4) return false;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring part = file + L".part";
    bool ok = false;
    {
        IWICImagingFactory* wic = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* props = nullptr;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        // 24-bit rows for the encoder, which takes no alpha.
        const UINT stride = (static_cast<UINT>(p.w) * 3 + 3) & ~3u;
        std::vector<uint8_t> rgb(static_cast<size_t>(stride) * p.h);
        for (int y = 0; y < p.h; y++)
            for (int x = 0; x < p.w; x++)
                memcpy(&rgb[static_cast<size_t>(y) * stride + x * 3], &p.bgra[(static_cast<size_t>(y) * p.w + x) * 4], 3);
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
             SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(part.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
             SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, &props));
        if (ok) {
            PROPBAG2 opt = {};
            wchar_t name[] = L"ImageQuality";
            opt.pstrName = name;
            VARIANT v = {};
            v.vt = VT_R4;
            v.fltVal = 0.92f;
            props->Write(1, &opt, &v);
            ok = SUCCEEDED(frame->Initialize(props)) && SUCCEEDED(frame->SetSize(p.w, p.h)) &&
                 SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) &&
                 SUCCEEDED(frame->WritePixels(p.h, stride, static_cast<UINT>(rgb.size()), rgb.data())) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        }
        if (props) props->Release();
        if (frame) frame->Release();
        if (enc) enc->Release();
        if (stream) stream->Release();
        if (wic) wic->Release();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    ok = ok && MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}

// ---- notice ---------------------------------------------------------------

namespace {

HWND notice = nullptr;
ULONGLONG noticeAt = 0;
int noticeMs = 0;

// Opacity at `t` ms: in over 100 ms, out over the last 200 ms.
BYTE NoticeAlpha(ULONGLONG t) {
    float a = (std::min)(1.0f, t / 100.0f);
    if (t + 200 > static_cast<ULONGLONG>(noticeMs)) a = (std::min)(a, (noticeMs - static_cast<float>(t)) / 200.0f);
    return static_cast<BYTE>((std::max)(0.0f, a) * 255.0f);
}

LRESULT CALLBACK NoticeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) {
        const ULONGLONG t = GetTickCount64() - noticeAt;
        if (t >= static_cast<ULONGLONG>(noticeMs)) {
            DestroyWindow(h);
        } else {
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, NoticeAlpha(t), AC_SRC_ALPHA};
            UpdateLayeredWindow(h, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
        }
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCDESTROY && h == notice) notice = nullptr;
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void HideNotice() {
    if (notice) DestroyWindow(notice);
}

HWND NoticeWindow() {
    return notice;
}

void ShowNotice(const uint8_t* premulBgra, int w, int h, int x, int y, int ms) {
    HideNotice();
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = NoticeProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NSWHomeNotice";
        RegisterClassExW(&wc);
        registered = true;
    }
    Dib d;
    if (w <= 0 || h <= 0 || !d.Make(w, h)) return;
    memcpy(d.bits, premulBgra, static_cast<size_t>(w) * h * 4);
    // Layered: the picture's own transparency over whatever is below.
    // No-activate + tool window + transparent to the mouse: it can never get
    // the focus, a click goes to the game, and it has no taskbar button.
    notice = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
                             L"NSWHomeNotice", L"", WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!notice) return;
    noticeAt = GetTickCount64();
    noticeMs = ms;
    POINT src = {0, 0}, pos = {x, y};
    SIZE size = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, NoticeAlpha(0), AC_SRC_ALPHA};
    UpdateLayeredWindow(notice, nullptr, &pos, &size, d.dc, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(notice, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(notice, 1, 16, nullptr);
}

} // namespace wincap
