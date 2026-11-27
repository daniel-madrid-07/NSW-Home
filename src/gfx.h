// Thin drawing layer over Direct2D / DirectWrite / WIC.
// Everything is drawn in a fixed 1280x720 design space; the render target
// transform maps that onto the real window, so screens never deal with DPI.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>
#include <cstdint>

namespace gfx {

constexpr float W = 1280.0f;
constexpr float H = 720.0f;

struct Color {
    float r, g, b, a;
};
Color rgb(uint32_t hex, float a = 1.0f);
Color mix(Color a, Color b, float t);
Color alpha(Color c, float a);

enum Align { Left, Center, Right };

// An image decoded on first use and re-created automatically if the device is
// lost. Source is either a file path or bytes held in memory.
struct Img {
    std::wstring path;
    std::vector<uint8_t> mem;
    int maxSize = 0; // decode no larger than this (longest side); 0 = native
    ID2D1Bitmap* bmp = nullptr;
    uint32_t generation = 0;
    bool failed = false;
    void reset();
    bool empty() const { return path.empty() && mem.empty(); }
};

bool Init();
// Use this TrueType/OpenType file for all text instead of the system font.
// Characters it lacks still fall back to system fonts. Takes ownership.
bool SetFont(std::vector<uint8_t>&& ttf);
// A second font used only for Symbol(): the console's button-glyph font.
bool SetSymbolFont(std::vector<uint8_t>&& ttf);
bool HasSymbolFont();
// The menu's language, for its script. `ttf` (may be empty; ownership taken) is a font tried
// before the main font for every character it has, the way the console puts a language's own
// shared font first (Korean, Chinese). Without one, `systemFamily` (may be empty) names an
// installed font to stand in for it. `locale` (BCP-47: "ja-JP", "zh-CN"...; empty for none) is
// the language of the text, which picks the right forms of the characters the system's font
// fallback supplies. Everything empty: as before. Returns false if the font could not be used.
bool SetLanguageFont(std::vector<uint8_t>&& ttf, const std::wstring& systemFamily, const std::wstring& locale);
// One glyph from the symbol font, centred on (cx, cy). False if there is no
// symbol font, so the caller can draw its own.
bool Symbol(wchar_t codepoint, float cx, float cy, float size, Color c);
// Multiplies every text size; set when a font runs larger or smaller per
// point than the layout was measured for.
void SetTextScale(float factor);
// Thickens every stroke of the main font by this fraction of the text size,
// for a font that is the right design but a lighter weight than wanted.
// Letter spacing is left alone. 0 = as drawn.
void SetTextEmbolden(float fraction);
void Shutdown();

// Window target. Resize re-fits the 1280x720 space (letterboxed).
bool AttachWindow(HWND hwnd);
void Resize(UINT w, UINT h);
// The console's "adjust screen size": the whole 1280x720 space is drawn at
// this fraction of the size it would have, centred, the rest left black.
// 1 = full size. Mouse positions follow (ToDesign).
void SetOverscan(float fraction);
// Offscreen target used by the --shot debug mode.
bool AttachOffscreen(UINT w, UINT h);
bool SaveOffscreenPng(const std::wstring& file);

// Windows only: runs `draw` into a transparent picture of w x h pixels in
// which one layout unit is `unitPx` pixels and (0, 0) is the top left corner,
// and returns it as premultiplied BGRA, top row first. The window or offscreen
// target is left as it was. For the small notice shown over a game; `draw`
// may use shapes and text, not images.
bool Snapshot(UINT w, UINT h, float unitPx, void (*draw)(), std::vector<uint8_t>& premulBgra);

void Begin(Color clear);
// Returns false when the device was lost; the target is rebuilt and the
// caller should just draw the next frame.
bool End();

// Design-space <-> window pixels (for mouse hit testing).
void ToDesign(int px, int py, float& x, float& y);

void Rect(float x, float y, float w, float h, Color c);
void RRect(float x, float y, float w, float h, float r, Color c);
void RRectStroke(float x, float y, float w, float h, float r, Color c, float sw);
// Rounded-rectangle outline painted with a linear gradient through `stops`,
// running across the box at `angle` degrees. Radius >= half the size gives a ring.
void RRectStrokeGrad(float x, float y, float w, float h, float r, const Color* stops, int count, float angle,
                     float sw, float opacity);
void Circle(float cx, float cy, float r, Color c);
void CircleStroke(float cx, float cy, float r, Color c, float sw);
void Line(float x0, float y0, float x1, float y1, Color c, float sw);
// Angles in degrees, 0 = right, clockwise (screen space).
void Arc(float cx, float cy, float r, float a0, float a1, Color c, float sw);
void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);

// Single-line text, vertically centred in h, trimmed with an ellipsis.
void Text(const std::wstring& s, float x, float y, float w, float h, float size, Color c,
          Align align = Left, int weight = 400);
// Multi-line wrapped text, top aligned.
void TextWrap(const std::wstring& s, float x, float y, float w, float h, float size, Color c,
              Align align = Left, int weight = 400);
float TextWidth(const std::wstring& s, float size, int weight = 400);
// Single line with extra space between characters (design units per character).
void TextSpaced(const std::wstring& s, float x, float y, float w, float h, float size, Color c, float spacing,
                Align align = Left);
float TextSpacedWidth(const std::wstring& s, float size, float spacing);

// shape: 0 = rectangle (optionally rounded by radius), 1 = circle.
// Images are scaled to fill the box, cropping the overflow.
void Image(Img& img, float x, float y, float w, float h, float opacity = 1.0f, int shape = 0,
           float radius = 0.0f);

// Writes a 256x256 JPEG profile picture: the given image cropped to fill, or,
// with no image, a flat colour with a letter on it.
bool MakeAvatar(const std::wstring& srcImage, Color bg, const std::wstring& initial, const std::wstring& outJpeg);

// ---- framing a picture into a square (the profile picture's "Ajustar imagen") ----
// The picture's size as decoded (pixels after maxSize; on Windows in the
// bitmap's own units, which keep its proportions). False if it cannot be read.
bool ImageSize(Img& img, float& w, float& h);
// How a picture sits in a square: turned `quarter` quarter turns clockwise,
// scaled to cover the square and then `zoom` times more, its centre `panX`,
// `panY` widths of the square right of / below the square's centre. Measured
// against the square, so the same framing gives the same crop at any size.
struct PicFrame {
    float panX = 0, panY = 0, zoom = 1;
    int quarter = 0;
};
constexpr float kPicFrameMaxZoom = 5.0f;
// Keeps the picture covering the whole square (iw x ih: its unturned size).
inline void ClampPicFrame(PicFrame& f, float iw, float ih) {
    f.quarter = ((f.quarter % 4) + 4) % 4;
    if (f.zoom < 1) f.zoom = 1;
    if (f.zoom > kPicFrameMaxZoom) f.zoom = kPicFrameMaxZoom;
    if (iw <= 0 || ih <= 0) return;
    const float rw = (f.quarter & 1) ? ih : iw, rh = (f.quarter & 1) ? iw : ih;
    const float cover = 1.0f / (rw < rh ? rw : rh); // square side 1
    const float mx = (rw * cover * f.zoom - 1) / 2, my = (rh * cover * f.zoom - 1) / 2;
    f.panX = f.panX < -mx ? -mx : f.panX > mx ? mx : f.panX;
    f.panY = f.panY < -my ? -my : f.panY > my ? my : f.panY;
}
// MakeAvatar for a picture framed as above: the square is the 256x256 file.
bool MakeAvatarFramed(const std::wstring& srcImage, const PicFrame& f, const std::wstring& outJpeg);

void PushClip(float x, float y, float w, float h);
void PopClip();
// Uniform scale around a point, for press/zoom animations.
void PushScale(float s, float cx, float cy);
// Rotation around a point, in degrees clockwise.
void PushRotate(float degrees, float cx, float cy);
// Shift everything drawn until PopTransform.
void PushOffset(float dx, float dy);
// Undoes the most recent PushScale / PushRotate / PushOffset.
void PopTransform();
// Everything drawn until PopLayer is blended as one group at this opacity,
// so overlapping shapes fade together instead of showing through each other.
void PushLayer(float opacity);
void PopLayer();

// Draws the whole picture as framed into the square (x, y, side): nothing is
// clipped, so what lies outside the square is the caller's to dim or cut.
// `turn` overrides quarter * 90 degrees while a turn is being animated.
// Shared by both drawing backends: only the calls above are used.
inline bool ImageFramed(Img& img, float x, float y, float side, const PicFrame& f, float opacity = 1.0f,
                        float turn = -1000.0f) {
    float iw = 0, ih = 0;
    if (!ImageSize(img, iw, ih) || iw <= 0 || ih <= 0) return false;
    const float rw = (f.quarter & 1) ? ih : iw, rh = (f.quarter & 1) ? iw : ih;
    const float k = side / (rw < rh ? rw : rh) * f.zoom;
    const float cx = x + side / 2 + f.panX * side, cy = y + side / 2 + f.panY * side;
    PushRotate(turn > -999.0f ? turn : 90.0f * f.quarter, cx, cy);
    Image(img, cx - iw * k / 2, cy - ih * k / 2, iw * k, ih * k, opacity);
    PopTransform();
    return true;
}

// ---- accessibility (Settings > Accesibilidad) -------------------------------
// Larger and heavier text on top of what the font itself needs (SetTextScale,
// SetTextEmbolden): a size factor (1 = as laid out) and an extra stroke band as
// a fraction of the text size (0 = none), with the letters set that much
// further apart so that they do not close up.
void SetTextAccess(float scale, float extraEmbolden);
// Everything drawn from now on, pictures included: 0 as given, 1 inverted,
// 2 greyscale. Pictures are converted when decoded, so a steady screen costs
// nothing; changing the mode makes every picture decode again.
void SetColorFilter(int mode);
// Called with every piece of text as it is drawn: the string and the box its
// ink takes on screen, in layout units (through the transforms in force).
// For a screen reader, which has to know what the focused element says.
// nullptr = off (the default).
typedef void (*TextTap)(const std::wstring& s, float x0, float y0, float x1, float y1);
void SetTextTap(TextTap tap);
// Every text given to Text, TextWrap, TextSpaced and the two measuring calls is first passed
// through this (the menu's language: the Spanish the screens are written in -> the text to
// show). The reference it returns must stay valid until the call returns. nullptr = as given.
typedef const std::wstring& (*TextFilter)(const std::wstring& s);
void SetTextFilter(TextFilter filter);
// A point in the current transform -> where it is on the page, in layout units
// (as if nothing were enlarged: see SetView).
void ToLayout(float& x, float& y);
// Enlarges every frame from the next Begin on: the page's point (cx, cy) is
// shown in the middle of the screen, `zoom` times its size. 1 = the whole
// page, as always. ToDesign and ToLayout see through it, so the mouse and
// everything else keep working in the page's own units.
void SetView(float zoom, float cx, float cy);

} // namespace gfx
