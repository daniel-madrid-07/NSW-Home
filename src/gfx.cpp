#include "gfx.h"
#include <d2d1helper.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <map>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace gfx {

static ComPtr<ID2D1Factory> d2d;
static ComPtr<IDWriteFactory> dw;
static ComPtr<IWICImagingFactory> wic;
static ComPtr<ID2D1HwndRenderTarget> hwndRt;
static ComPtr<ID2D1RenderTarget> rt;
static ComPtr<IWICBitmap> offscreen;
static ComPtr<ID2D1SolidColorBrush> brush;
static HWND window = nullptr;
static uint32_t generation = 1; // bumped whenever device resources are rebuilt
static float scale = 1.0f, offX = 0.0f, offY = 0.0f;
static D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Identity();
static std::vector<D2D1_MATRIX_3X2_F> xformStack;
static std::map<int, ComPtr<IDWriteTextFormat>> formats;
static std::vector<uint8_t> fontData;
static ComPtr<IDWriteFontCollection1> fontCollection;
static ComPtr<IDWriteInMemoryFontFileLoader> fontLoader;
static std::wstring fontFamily = L"Segoe UI";
static std::vector<uint8_t> symbolData;
static ComPtr<IDWriteFontCollection1> symbolCollection;
static ComPtr<IDWriteInMemoryFontFileLoader> symbolLoader;
static std::wstring symbolFamily;
// SetLanguageFont: the language's own font (in memory, or an installed one when langSystem),
// put before the main font, and the fallback that leads from it to the main font and then to
// the system's fonts.
static std::vector<uint8_t> langData;
static ComPtr<IDWriteFontCollection1> langCollection;
static ComPtr<IDWriteInMemoryFontFileLoader> langLoader;
static std::wstring langFamily;
static bool langSystem = false;
static std::wstring textLocale;
static ComPtr<IDWriteFontFallback> langFallback;
static float textScale = 1.0f;
static float embolden = 0.0f;
// Accessibility, on top of the two above (see gfx.h).
static float accessScale = 1.0f, accessBold = 0.0f;
static int colorFilter = 0;
static TextTap textTap = nullptr;
static TextFilter textFilter = nullptr;
// What a text is drawn and measured as (the menu's language, see SetTextFilter).
static const std::wstring& Filtered(const std::wstring& s) {
    return textFilter ? textFilter(s) : s;
}
static float viewZoom = 1.0f, viewX = W / 2, viewY = H / 2;
void ReleaseTextResources();

Color rgb(uint32_t hex, float a) {
    return {((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a};
}
Color mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
Color alpha(Color c, float a) {
    c.a *= a;
    return c;
}

void Img::reset() {
    if (bmp) bmp->Release();
    bmp = nullptr;
    failed = false;
}

bool Init() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;
    return true;
}

void Shutdown() {
    formats.clear();
    SetTextEmbolden(0.0f);
    ReleaseTextResources();
    fontCollection.Reset();
    symbolCollection.Reset();
    langCollection.Reset();
    langFallback.Reset();
    ComPtr<IDWriteInMemoryFontFileLoader>* loaders[] = {&fontLoader, &symbolLoader, &langLoader};
    for (auto* loader : loaders) {
        ComPtr<IDWriteFactory5> f5;
        if (*loader && SUCCEEDED(dw.As(&f5))) f5->UnregisterFontFileLoader(loader->Get());
        loader->Reset();
    }
    brush.Reset();
    rt.Reset();
    hwndRt.Reset();
    offscreen.Reset();
    wic.Reset();
    dw.Reset();
    d2d.Reset();
    CoUninitialize();
}

// Wraps a font held in memory as a one-family DirectWrite collection.
static bool BuildCollection(const std::vector<uint8_t>& data, ComPtr<IDWriteInMemoryFontFileLoader>& loaderOut,
                            ComPtr<IDWriteFontCollection1>& collOut, std::wstring& nameOut) {
    ComPtr<IDWriteFactory5> f5;
    if (data.empty() || FAILED(dw.As(&f5))) return false; // needs Windows 10 1703+
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    ComPtr<IDWriteFontFile> file;
    ComPtr<IDWriteFontSetBuilder1> builder;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> coll;
    if (FAILED(f5->CreateInMemoryFontFileLoader(&loader)) || FAILED(f5->RegisterFontFileLoader(loader.Get())))
        return false;
    bool ok = SUCCEEDED(loader->CreateInMemoryFontFileReference(f5.Get(), data.data(), static_cast<UINT32>(data.size()),
                                                                nullptr, &file)) &&
              SUCCEEDED(f5->CreateFontSetBuilder(&builder)) && SUCCEEDED(builder->AddFontFile(file.Get())) &&
              SUCCEEDED(builder->CreateFontSet(&set)) &&
              SUCCEEDED(f5->CreateFontCollectionFromFontSet(set.Get(), &coll)) && coll->GetFontFamilyCount() > 0;
    // This collection model can rename the family, so ask it what it is called.
    ComPtr<IDWriteFontFamily1> family;
    ComPtr<IDWriteLocalizedStrings> names;
    wchar_t name[128] = {};
    ok = ok && SUCCEEDED(coll->GetFontFamily(0, &family)) && SUCCEEDED(family->GetFamilyNames(&names)) &&
         SUCCEEDED(names->GetString(0, name, ARRAYSIZE(name)));
    if (!ok) {
        f5->UnregisterFontFileLoader(loader.Get());
        return false;
    }
    loaderOut = loader;
    collOut = coll;
    nameOut = name;
    return true;
}

bool SetFont(std::vector<uint8_t>&& ttf) {
    std::vector<uint8_t> data = std::move(ttf);
    if (!BuildCollection(data, fontLoader, fontCollection, fontFamily)) return false;
    fontData = std::move(data); // the loader reads from this buffer for as long as it lives
    formats.clear();
    return true;
}

bool SetSymbolFont(std::vector<uint8_t>&& ttf) {
    std::vector<uint8_t> data = std::move(ttf);
    if (!BuildCollection(data, symbolLoader, symbolCollection, symbolFamily)) return false;
    symbolData = std::move(data);
    return true;
}

bool HasSymbolFont() {
    return symbolCollection != nullptr;
}

bool SetLanguageFont(std::vector<uint8_t>&& ttf, const std::wstring& systemFamily, const std::wstring& locale) {
    std::vector<uint8_t> data = std::move(ttf);
    ComPtr<IDWriteFactory5> f5;
    if (langLoader && SUCCEEDED(dw.As(&f5))) f5->UnregisterFontFileLoader(langLoader.Get());
    langLoader.Reset();
    langCollection.Reset();
    langFallback.Reset();
    langData.clear();
    langFamily.clear();
    langSystem = false;
    textLocale = locale;
    bool ok = true;
    if (!data.empty()) {
        ok = BuildCollection(data, langLoader, langCollection, langFamily);
        if (ok) langData = std::move(data); // the loader reads from this buffer
    }
    if (langFamily.empty() && !systemFamily.empty()) {
        ComPtr<IDWriteFontCollection> sys;
        UINT32 at = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(dw->GetSystemFontCollection(&sys)) && SUCCEEDED(sys->FindFamilyName(systemFamily.c_str(), &at, &exists)) &&
            exists) {
            langFamily = systemFamily;
            langSystem = true;
        }
    }
    // Every character the language's font lacks: the main font, then the system's fallback.
    ComPtr<IDWriteFactory2> f2;
    ComPtr<IDWriteFontFallbackBuilder> builder;
    ComPtr<IDWriteFontFallback> system;
    if (!langFamily.empty() && SUCCEEDED(dw.As(&f2)) && SUCCEEDED(f2->CreateFontFallbackBuilder(&builder)) &&
        SUCCEEDED(f2->GetSystemFontFallback(&system))) {
        DWRITE_UNICODE_RANGE all = {0, 0x10FFFF};
        const wchar_t* main[] = {fontFamily.c_str()};
        builder->AddMapping(&all, 1, main, 1, fontCollection.Get(), nullptr, nullptr, 1.0f);
        builder->AddMappings(system.Get());
        builder->CreateFontFallback(&langFallback);
    }
    if (!langFallback) langFamily.clear(); // with no way back to the main font it would lose every other letter
    formats.clear();
    ReleaseTextResources();
    return ok;
}

void SetTextScale(float factor) {
    textScale = factor;
    formats.clear();
    SetTextEmbolden(embolden); // drops the laid-out text, which was built at the old scale
}

static float overscan = 1.0f, fitW = 0.0f, fitH = 0.0f;

static void Fit(float pw, float ph) {
    fitW = pw;
    fitH = ph;
    scale = (std::min)(pw / W, ph / H) * overscan;
    offX = (pw - W * scale) * 0.5f;
    offY = (ph - H * scale) * 0.5f;
    base = D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(offX, offY);
}

// The layout fitted to the target, through the enlarged view when there is one.
static D2D1_MATRIX_3X2_F ViewBase() {
    if (viewZoom == 1.0f) return base;
    return D2D1::Matrix3x2F::Translation(-viewX, -viewY) * D2D1::Matrix3x2F::Scale(viewZoom, viewZoom) *
           D2D1::Matrix3x2F::Translation(W / 2, H / 2) * base;
}

void SetView(float zoom, float cx, float cy) {
    viewZoom = zoom;
    viewX = cx;
    viewY = cy;
}

static bool CreateWindowTarget() {
    RECT rc;
    GetClientRect(window, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    // Fixed 96 DPI: the design space is scaled by hand, so D2D must not
    // apply the monitor DPI a second time.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    hwndRt.Reset();
    brush.Reset();
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(window, size),
                                           hwndRt.GetAddressOf())))
        return false;
    rt = hwndRt;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush.GetAddressOf());
    generation++;
    Fit(static_cast<float>(size.width), static_cast<float>(size.height));
    return true;
}

void SetOverscan(float fraction) {
    fraction = (std::max)(0.5f, (std::min)(1.0f, fraction));
    if (fraction == overscan) return;
    overscan = fraction;
    if (fitW > 0) Fit(fitW, fitH);
}

bool AttachWindow(HWND hwnd) {
    window = hwnd;
    return CreateWindowTarget();
}

void Resize(UINT w, UINT h) {
    if (!hwndRt || w == 0 || h == 0) return;
    hwndRt->Resize(D2D1::SizeU(w, h));
    Fit(static_cast<float>(w), static_cast<float>(h));
}

bool AttachOffscreen(UINT w, UINT h) {
    if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                 offscreen.GetAddressOf())))
        return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (FAILED(d2d->CreateWicBitmapRenderTarget(offscreen.Get(), props, rt.ReleaseAndGetAddressOf())))
        return false;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush.ReleaseAndGetAddressOf());
    generation++;
    Fit(static_cast<float>(w), static_cast<float>(h));
    return true;
}

bool Snapshot(UINT w, UINT h, float unitPx, void (*draw)(), std::vector<uint8_t>& premulBgra) {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> paint;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (!w || !h || FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &paint)))
        return false;
    // The drawing functions paint on `rt` with `brush` through `base`: lend
    // them this picture for one call. (Laid-out text is geometry and belongs
    // to no target; images do, which is why they are not for use here.)
    std::swap(rt, target);
    std::swap(brush, paint);
    const D2D1_MATRIX_3X2_F savedBase = base;
    const std::vector<D2D1_MATRIX_3X2_F> savedStack = xformStack;
    base = D2D1::Matrix3x2F::Scale(unitPx, unitPx);
    xformStack.clear();
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->SetTransform(base);
    draw();
    const bool ok = SUCCEEDED(rt->EndDraw());
    std::swap(rt, target);
    std::swap(brush, paint);
    base = savedBase;
    xformStack = savedStack;
    premulBgra.resize(static_cast<size_t>(w) * h * 4);
    return ok && SUCCEEDED(bitmap->CopyPixels(nullptr, w * 4, static_cast<UINT>(premulBgra.size()), premulBgra.data()));
}

static bool SaveBitmap(IWICBitmap* bitmap, const std::wstring& file, REFGUID container);

bool SaveOffscreenPng(const std::wstring& file) {
    return SaveBitmap(offscreen.Get(), file, GUID_ContainerFormatPng);
}

static bool SaveBitmap(IWICBitmap* bitmap, const std::wstring& file, REFGUID container) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(wic->CreateEncoder(container, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->WriteSource(bitmap, nullptr))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

void Begin(Color clear) {
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(D2D1::ColorF(0, 0, 0));
    rt->SetTransform(ViewBase());
    xformStack.clear();
    Rect(0, 0, W, H, clear);
}

bool End() {
    HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET && window) {
        CreateWindowTarget();
        return false;
    }
    return SUCCEEDED(hr);
}

void ToDesign(int px, int py, float& x, float& y) {
    x = (px - offX) / scale;
    y = (py - offY) / scale;
    if (viewZoom != 1.0f) {
        x = (x - W / 2) / viewZoom + viewX;
        y = (y - H / 2) / viewZoom + viewY;
    }
}

// The menu's colours as the accessibility setting wants them shown.
static Color Filtered(Color c) {
    if (colorFilter == 1) return {1.0f - c.r, 1.0f - c.g, 1.0f - c.b, c.a};
    if (colorFilter == 2) {
        float y = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
        return {y, y, y, c.a};
    }
    return c;
}

static ID2D1SolidColorBrush* B(Color c) {
    if (colorFilter) c = Filtered(c);
    brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    return brush.Get();
}

void Rect(float x, float y, float w, float h, Color c) {
    rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), B(c));
}
void RRect(float x, float y, float w, float h, float r, Color c) {
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), B(c));
}
void RRectStroke(float x, float y, float w, float h, float r, Color c, float sw) {
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), B(c), sw);
}
void RRectStrokeGrad(float x, float y, float w, float h, float r, const Color* stops, int count, float angle,
                     float sw, float opacity) {
    if (opacity <= 0.0f || count < 2 || count > 8) return;
    D2D1_GRADIENT_STOP gs[8];
    for (int i = 0; i < count; i++) {
        gs[i].position = static_cast<float>(i) / (count - 1);
        const Color stop = colorFilter ? Filtered(stops[i]) : stops[i];
        gs[i].color = D2D1::ColorF(stop.r, stop.g, stop.b, stop.a);
    }
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(rt->CreateGradientStopCollection(gs, count, &coll))) return;
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    float half = 0.5f * sqrtf(w * w + h * h);
    float a = angle * 3.14159265f / 180.0f;
    float dx = cosf(a) * half, dy = sinf(a) * half;
    ComPtr<ID2D1LinearGradientBrush> gb;
    if (FAILED(rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - dx, cy - dy), D2D1::Point2F(cx + dx, cy + dy)),
            coll.Get(), &gb)))
        return;
    gb->SetOpacity(opacity);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), gb.Get(), sw);
}

void Circle(float cx, float cy, float r, Color c) {
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), B(c));
}
void CircleStroke(float cx, float cy, float r, Color c, float sw) {
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), B(c), sw);
}
void Line(float x0, float y0, float x1, float y1, Color c, float sw) {
    rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), B(c), sw);
}

void Arc(float cx, float cy, float r, float a0, float a1, Color c, float sw) {
    const float k = 3.14159265f / 180.0f;
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2d->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(cx + r * cosf(a0 * k), cy + r * sinf(a0 * k)), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * cosf(a1 * k), cy + r * sinf(a1 * k)), D2D1::SizeF(r, r),
                                  0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  (a1 - a0) > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ComPtr<ID2D1StrokeStyle> round;
    d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                           &round);
    rt->DrawGeometry(geo.Get(), B(c), sw, round.Get());
}

void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2d->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(x0, y0), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x1, y1));
    sink->AddLine(D2D1::Point2F(x2, y2));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    rt->FillGeometry(geo.Get(), B(c));
}

// ---- text -----------------------------------------------------------------
// Text is drawn as filled outlines, not through DirectWrite's glyph
// rasteriser. That gives the same result in the window and offscreen (no
// ClearType, no hinting that bends letters at small sizes), and it lets the
// weight be adjusted by widening the outlines. Outlines are built once per
// distinct string and kept, so a steady screen costs a fill per string.

static IDWriteTextFormat* Format(float size, int weight) {
    size *= textScale * accessScale;
    int key = static_cast<int>(size * 8.0f) * 1000 + weight;
    auto it = formats.find(key);
    if (it != formats.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    // The console font ships in one weight; DirectWrite emboldens it for the
    // few bold labels.
    const bool lang = !langFamily.empty();
    IDWriteFontCollection* coll = lang ? (langSystem ? nullptr : langCollection.Get()) : fontCollection.Get();
    dw->CreateTextFormat(lang ? langFamily.c_str() : fontFamily.c_str(), coll, static_cast<DWRITE_FONT_WEIGHT>(weight),
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, textLocale.c_str(), &f);
    ComPtr<IDWriteTextFormat1> f1;
    if (f && lang && SUCCEEDED(f.As(&f1))) f1->SetFontFallback(langFallback.Get());
    formats[key] = f;
    return f.Get();
}

static DWRITE_TEXT_ALIGNMENT DwAlign(Align a) {
    return a == Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                       : a == Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
}

namespace {

// Receives the glyph runs of a laid-out text and turns them into geometry.
// Lives on the stack, so reference counting is a formality.
class OutlineCollector final : public IDWriteTextRenderer {
public:
    std::vector<ComPtr<ID2D1Geometry>> parts;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
            riid == __uuidof(IDWriteTextRenderer)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 1; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* p) override {
        *p = 1.0f;
        return S_OK;
    }
    IFACEMETHODIMP DrawGlyphRun(void*, FLOAT bx, FLOAT by, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(d2d->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return S_OK;
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                                          run->glyphCount, run->isSideways, run->bidiLevel % 2, sink.Get());
        sink->Close();
        ComPtr<ID2D1TransformedGeometry> placed;
        if (SUCCEEDED(d2d->CreateTransformedGeometry(path.Get(), D2D1::Matrix3x2F::Translation(bx, by), &placed)))
            parts.push_back(placed);
        return S_OK;
    }
    IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    IFACEMETHODIMP DrawInlineObject(void* ctx, FLOAT x, FLOAT y, IDWriteInlineObject* obj, BOOL sideways, BOOL rtl,
                                    IUnknown* effect) override {
        // The ellipsis that replaces trimmed text arrives this way.
        return obj ? obj->Draw(ctx, this, x, y, sideways, rtl, effect) : S_OK;
    }
};

struct TextSpec {
    float w, h, size;
    int weight;
    Align align;
    bool wrap;     // multi-line, top aligned; otherwise one line, vertically centred, trimmed
    float spacing; // extra space per character, on top of the embolden spacing
    bool symbol;   // use the symbol font, unscaled and never emboldened
};

struct LaidText {
    ComPtr<ID2D1Geometry> geo;
    float width = 0, height = 0;
};

std::map<std::wstring, LaidText> textCache;
std::map<std::wstring, float> widthCache;
std::map<int, ComPtr<IDWriteTextFormat>> symbolFormats;
ComPtr<ID2D1StrokeStyle> roundJoin;

std::wstring CacheKey(const std::wstring& s, const TextSpec& t) {
    wchar_t meta[128];
    swprintf_s(meta, L"\x1f%d|%d|%d|%d|%d|%d|%d|%d", static_cast<int>(t.size * 16), t.weight, static_cast<int>(t.w * 4),
               static_cast<int>(t.h * 4), static_cast<int>(t.align), t.wrap ? 1 : 0, static_cast<int>(t.spacing * 100),
               t.symbol ? 1 : 0);
    return s + meta;
}

IDWriteTextFormat* SymbolFormat(float size) {
    int key = static_cast<int>(size * 8.0f);
    auto it = symbolFormats.find(key);
    if (it != symbolFormats.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    dw->CreateTextFormat(symbolFamily.c_str(), symbolCollection.Get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    symbolFormats[key] = f;
    return f.Get();
}

ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& s, const TextSpec& t) {
    IDWriteTextFormat* f = t.symbol ? SymbolFormat(t.size) : Format(t.size, t.weight);
    ComPtr<IDWriteTextLayout> layout;
    if (!f || FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, t.w, t.h, &layout))) return nullptr;
    layout->SetTextAlignment(DwAlign(t.align));
    layout->SetParagraphAlignment(t.wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    layout->SetWordWrapping(t.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!t.wrap && !t.symbol) {
        // The ellipsis goes on this layout, never on the shared format. A sign
        // made from a format that already carries one keeps the old one alive
        // inside it; doing that every frame built a chain thousands long, and
        // releasing it overflowed the stack inside DirectWrite.
        ComPtr<IDWriteInlineObject> ellipsis;
        dw->CreateEllipsisTrimmingSign(f, &ellipsis);
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trim, ellipsis.Get());
    }
    // No extra letter spacing for the thicker strokes: measured against the
    // console, its heavier text has the same advance widths as the regular font.
    float spacing = t.spacing;
    // The accessibility weight is much heavier than that, and does need room:
    // each letter grows by the width of its band.
    if (!t.symbol) spacing += accessBold * t.size * textScale * accessScale;
    ComPtr<IDWriteTextLayout1> l1;
    if (spacing != 0.0f && SUCCEEDED(layout.As(&l1)))
        l1->SetCharacterSpacing(0, spacing, 0, {0, static_cast<UINT32>(s.size())});
    return layout;
}

LaidText* Lay(const std::wstring& s, const TextSpec& t) {
    std::wstring key = CacheKey(s, t);
    auto it = textCache.find(key);
    if (it != textCache.end()) return &it->second;
    if (textCache.size() > 600) textCache.clear(); // strings that change (the clock) would pile up otherwise

    LaidText out;
    ComPtr<IDWriteTextLayout> layout = MakeLayout(s, t);
    if (layout) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        out.width = m.widthIncludingTrailingWhitespace;
        out.height = m.height;
        OutlineCollector collector;
        layout->Draw(nullptr, &collector, 0, 0);
        ComPtr<ID2D1Geometry> shape;
        if (collector.parts.size() == 1) {
            shape = collector.parts[0];
        } else if (!collector.parts.empty()) {
            std::vector<ID2D1Geometry*> raw;
            for (auto& p : collector.parts) raw.push_back(p.Get());
            ComPtr<ID2D1GeometryGroup> group;
            if (SUCCEEDED(d2d->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), static_cast<UINT32>(raw.size()), &group)))
                shape = group;
        }
        // Heavier weight = a band along the edges, proportional to the letter size.
        const float bandWidth = (embolden + accessBold) * t.size * textScale * accessScale;
        if (shape && bandWidth > 0.0f && !t.symbol) {
            // The letter shapes plus that band, merged into one outline so a
            // translucent fill stays even.
            if (!roundJoin)
                d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                   D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                       nullptr, 0, &roundJoin);
            const float tolerance = 0.04f; // layout units; the menu is shown at up to about 2x
            ComPtr<ID2D1PathGeometry> band, merged;
            ComPtr<ID2D1GeometrySink> bandSink, mergedSink;
            if (SUCCEEDED(d2d->CreatePathGeometry(&band)) && SUCCEEDED(band->Open(&bandSink)) &&
                SUCCEEDED(shape->Widen(bandWidth, roundJoin.Get(), nullptr, tolerance, bandSink.Get())) &&
                SUCCEEDED(bandSink->Close()) && SUCCEEDED(d2d->CreatePathGeometry(&merged)) &&
                SUCCEEDED(merged->Open(&mergedSink)) &&
                SUCCEEDED(shape->CombineWithGeometry(band.Get(), D2D1_COMBINE_MODE_UNION, nullptr, tolerance,
                                                     mergedSink.Get())) &&
                SUCCEEDED(mergedSink->Close()))
                shape = merged;
        }
        out.geo = shape;
    }
    return &(textCache[key] = out);
}

void FillLaid(LaidText* t, float x, float y, Color c) {
    if (!t || !t->geo) return;
    D2D1_MATRIX_3X2_F cur;
    rt->GetTransform(&cur);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * cur);
    rt->FillGeometry(t->geo.Get(), B(c));
    rt->SetTransform(cur);
}

float Measure(const std::wstring& s, const TextSpec& t) {
    std::wstring key = CacheKey(s, t);
    auto it = widthCache.find(key);
    if (it != widthCache.end()) return it->second;
    if (widthCache.size() > 600) widthCache.clear();
    float width = 0;
    if (ComPtr<IDWriteTextLayout> layout = MakeLayout(s, t)) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        width = m.widthIncludingTrailingWhitespace;
    }
    return widthCache[key] = width;
}

} // namespace

void SetTextEmbolden(float fraction) {
    embolden = fraction;
    textCache.clear();
    widthCache.clear();
}

void ReleaseTextResources() {
    textCache.clear();
    widthCache.clear();
    symbolFormats.clear();
    roundJoin.Reset();
}

void SetTextAccess(float factor, float extraEmbolden) {
    if (factor == accessScale && extraEmbolden == accessBold) return;
    accessScale = factor;
    accessBold = extraEmbolden;
    formats.clear();
    SetTextEmbolden(embolden); // drops the laid-out text
}

void SetColorFilter(int mode) {
    if (mode == colorFilter) return;
    colorFilter = mode;
    generation++; // pictures are converted as they are decoded: decode them again
}

void SetTextTap(TextTap tap) {
    textTap = tap;
}

void SetTextFilter(TextFilter filter) {
    textFilter = filter;
}

void ToLayout(float& x, float& y) {
    D2D1_MATRIX_3X2_F cur;
    rt->GetTransform(&cur);
    // Through the current transform to target pixels, then back through the
    // one that fits the layout to the target, and through the view.
    const float px = x * cur._11 + y * cur._21 + cur._31, py = x * cur._12 + y * cur._22 + cur._32;
    x = (px - offX) / scale;
    y = (py - offY) / scale;
    if (viewZoom != 1.0f) {
        x = (x - W / 2) / viewZoom + viewX;
        y = (y - H / 2) / viewZoom + viewY;
    }
}

// Tells the tap where this text's ink is: across, as far as the laid-out width
// reaches from the side it is aligned to; down, the box of a single line and
// the laid-out height of a paragraph.
static void Tapped(const std::wstring& s, LaidText* t, float x, float y, float w, float h, Align align, bool wrap) {
    if (!t) return;
    const float tw = (std::min)(t->width, w);
    float x0 = align == Left ? x : align == Center ? x + (w - tw) * 0.5f : x + w - tw, x1 = x0 + tw;
    float y0 = y, y1 = wrap ? y + (std::min)(t->height, h) : y + h;
    ToLayout(x0, y0);
    ToLayout(x1, y1);
    textTap(s, x0, y0, x1, y1);
}

void Text(const std::wstring& text, float x, float y, float w, float h, float size, Color c, Align align, int weight) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return;
    LaidText* t = Lay(s, {w, h, size, weight, align, false, 0.0f, false});
    FillLaid(t, x, y, c);
    if (textTap) Tapped(s, t, x, y, w, h, align, false);
}

void TextWrap(const std::wstring& text, float x, float y, float w, float h, float size, Color c, Align align,
              int weight) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return;
    PushClip(x - 2, y - 2, w + 4, h + 4);
    LaidText* t = Lay(s, {w, h, size, weight, align, true, 0.0f, false});
    FillLaid(t, x, y, c);
    PopClip();
    if (textTap) Tapped(s, t, x, y, w, h, align, true);
}

void TextSpaced(const std::wstring& text, float x, float y, float w, float h, float size, Color c, float spacing,
                Align align) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return;
    LaidText* t = Lay(s, {w, h, size, 400, align, false, spacing, false});
    FillLaid(t, x, y, c);
    if (textTap) Tapped(s, t, x, y, w, h, align, false);
}

float TextSpacedWidth(const std::wstring& text, float size, float spacing) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return 0.0f;
    // No gap after the last character.
    return Measure(s, {10000.0f, 100.0f, size, 400, Left, false, spacing, false}) - spacing;
}

bool Symbol(wchar_t codepoint, float cx, float cy, float size, Color c) {
    if (!symbolCollection) return false;
    float box = size * 3.0f;
    FillLaid(Lay(std::wstring(1, codepoint), {box, box, size, 400, Center, false, 0.0f, true}), cx - box / 2, cy - box / 2, c);
    return true;
}

float TextWidth(const std::wstring& text, float size, int weight) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return 0.0f;
    return Measure(s, {10000.0f, 100.0f, size, weight, Left, false, 0.0f, false});
}

static void Decode(Img& img) {
    img.failed = true;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICStream> stream;
    HRESULT hr;
    if (!img.mem.empty()) {
        if (FAILED(wic->CreateStream(&stream))) return;
        if (FAILED(stream->InitializeFromMemory(img.mem.data(), static_cast<DWORD>(img.mem.size())))) return;
        hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    } else {
        hr = wic->CreateDecoderFromFilename(img.path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &dec);
    }
    if (FAILED(hr)) return;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return;
    ComPtr<IWICBitmapSource> src = frame;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    UINT longest = (std::max)(w, h);
    if (img.maxSize > 0 && longest > static_cast<UINT>(img.maxSize)) {
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), w * img.maxSize / longest, h * img.maxSize / longest,
                                         WICBitmapInterpolationModeFant)))
            src = scaler;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return;
    if (FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut)))
        return;
    if (colorFilter) {
        // The picture as the colour setting wants it: converted here, once,
        // rather than every time it is drawn (this render target has no effects).
        UINT cw = 0, ch = 0;
        if (FAILED(conv->GetSize(&cw, &ch)) || cw == 0 || ch == 0 || cw > 16384 || ch > 16384) return;
        std::vector<uint8_t> px(static_cast<size_t>(cw) * ch * 4);
        if (FAILED(conv->CopyPixels(nullptr, cw * 4, static_cast<UINT>(px.size()), px.data()))) return;
        for (size_t i = 0; i < px.size(); i += 4) {
            uint8_t* p = &px[i]; // premultiplied B, G, R, A
            if (colorFilter == 1) {
                for (int k = 0; k < 3; k++) p[k] = static_cast<uint8_t>(p[3] - (std::min)(p[k], p[3]));
            } else {
                p[0] = p[1] = p[2] = static_cast<uint8_t>((p[2] * 299 + p[1] * 587 + p[0] * 114 + 500) / 1000);
            }
        }
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(cw, ch), px.data(), cw * 4,
                                       D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                                D2D1_ALPHA_MODE_PREMULTIPLIED)),
                                       &img.bmp)))
            img.failed = false;
        return;
    }
    if (SUCCEEDED(rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &img.bmp))) img.failed = false;
}

void Image(Img& img, float x, float y, float w, float h, float opacity, int shape, float radius) {
    if (img.empty()) return;
    if (img.generation != generation) {
        img.reset();
        img.generation = generation;
    }
    if (!img.bmp && !img.failed) Decode(img);
    if (!img.bmp) return;

    D2D1_SIZE_F sz = img.bmp->GetSize();
    float s = (std::max)(w / sz.width, h / sz.height); // cover
    float dx = x + (w - sz.width * s) * 0.5f, dy = y + (h - sz.height * s) * 0.5f;

    if (shape == 0 && radius <= 0.0f) {
        // Crop the source instead of clipping the destination.
        float sw = w / s, sh = h / s;
        D2D1_RECT_F srcRc = D2D1::RectF((sz.width - sw) * 0.5f, (sz.height - sh) * 0.5f, (sz.width + sw) * 0.5f,
                                        (sz.height + sh) * 0.5f);
        rt->DrawBitmap(img.bmp, D2D1::RectF(x, y, x + w, y + h), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                       srcRc);
        return;
    }
    ComPtr<ID2D1BitmapBrush> bb;
    if (FAILED(rt->CreateBitmapBrush(img.bmp, &bb))) return;
    bb->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(dx, dy));
    bb->SetOpacity(opacity);
    if (shape == 1)
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + w * 0.5f, y + h * 0.5f), w * 0.5f, h * 0.5f), bb.Get());
    else
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), bb.Get());
}

bool ImageSize(Img& img, float& w, float& h) {
    if (img.empty()) return false;
    if (img.generation != generation) {
        img.reset();
        img.generation = generation;
    }
    if (!img.bmp && !img.failed) Decode(img);
    if (!img.bmp) return false;
    D2D1_SIZE_F sz = img.bmp->GetSize();
    w = sz.width;
    h = sz.height;
    return true;
}

// MakeAvatar and MakeAvatarFramed: `frame` null = the picture cropped to fill.
static bool PaintAvatar(const std::wstring& srcImage, Color bg, const std::wstring& initial, const PicFrame* frame,
                        const std::wstring& outJpeg) {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> targetBrush;
    if (FAILED(wic->CreateBitmap(256, 256, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target))) return false;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &targetBrush))) return false;

    // Borrow the drawing helpers by pointing them at this target for a moment.
    std::swap(rt, target);
    std::swap(brush, targetBrush);
    generation++;
    const int keptFilter = colorFilter; // the file is the user's picture, not a view of it
    colorFilter = 0;
    bool ok = true;
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    Rect(0, 0, 256, 256, bg);
    if (!srcImage.empty()) {
        Img im;
        im.path = srcImage;
        // A zoomed framing shows a part of the picture: decoded larger so that
        // part still has the file's 256 pixels' worth (up to 2048).
        im.maxSize = frame ? static_cast<int>((std::min)(2048.0f, 512.0f * frame->zoom)) : 512;
        if (frame) ImageFramed(im, 0, 0, 256, *frame);
        else Image(im, 0, 0, 256, 256);
        ok = im.bmp != nullptr;
        im.reset();
    } else {
        Text(initial, 0, -8, 256, 256, 150, rgb(0xFFFFFF), Center, 600);
    }
    ok = SUCCEEDED(rt->EndDraw()) && ok;
    colorFilter = keptFilter;
    std::swap(rt, target);
    std::swap(brush, targetBrush);
    generation++; // images decoded for the temporary target are not valid on the real one
    if (rt) rt->SetTransform(ViewBase());
    return ok && SaveBitmap(bitmap.Get(), outJpeg, GUID_ContainerFormatJpeg);
}

bool MakeAvatar(const std::wstring& srcImage, Color bg, const std::wstring& initial, const std::wstring& outJpeg) {
    return PaintAvatar(srcImage, bg, initial, nullptr, outJpeg);
}

bool MakeAvatarFramed(const std::wstring& srcImage, const PicFrame& f, const std::wstring& outJpeg) {
    return !srcImage.empty() && PaintAvatar(srcImage, rgb(0x000000), L"", &f, outJpeg);
}

void PushClip(float x, float y, float w, float h) {
    rt->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_ALIASED);
}
void PopClip() {
    rt->PopAxisAlignedClip();
}

void PushScale(float s, float cx, float cy) {
    D2D1_MATRIX_3X2_F cur;
    rt->GetTransform(&cur);
    xformStack.push_back(cur);
    rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(cx, cy)) * cur);
}
void PushRotate(float degrees, float cx, float cy) {
    D2D1_MATRIX_3X2_F cur;
    rt->GetTransform(&cur);
    xformStack.push_back(cur);
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, D2D1::Point2F(cx, cy)) * cur);
}
void PushOffset(float dx, float dy) {
    D2D1_MATRIX_3X2_F cur;
    rt->GetTransform(&cur);
    xformStack.push_back(cur);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * cur);
}
void PopTransform() {
    if (xformStack.empty()) return;
    rt->SetTransform(xformStack.back());
    xformStack.pop_back();
}

void PushLayer(float opacity) {
    ComPtr<ID2D1Layer> layer;
    rt->CreateLayer(nullptr, &layer);
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                        D2D1::IdentityMatrix(), opacity),
                  layer.Get());
}
void PopLayer() {
    rt->PopLayer();
}

} // namespace gfx
