// Drawing backend for Linux and macOS: every function of gfx.h on top of
// Cairo (shapes, gradients, groups, clips), FreeType (glyph outlines),
// fontconfig (system fonts) and libjpeg. It takes the place of gfx.cpp, which
// is the Direct2D / DirectWrite / WIC backend; that file is the specification
// this one follows:
//   * everything is drawn in a 1280x720 space, letterboxed into the target;
//   * text is filled glyph outlines, never a hinted rasteriser, with an
//     optional thickening band around the outlines (SetTextEmbolden);
//   * images are decoded on first use and scaled to cover their box;
//   * the target is a plain memory picture. The window code (main_sdl.cpp)
//     fetches it with gfx::FramePixels and puts it on screen itself.
#include "gfx.h"
#include "port.h"

#include <cairo.h>
#if !defined(__ANDROID__)
#include <fontconfig/fontconfig.h>
#endif
#include <ft2build.h>
#include FT_FREETYPE_H
#if defined(__ANDROID__)
#include FT_MULTIPLE_MASTERS_H
#include <dirent.h>
#endif
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <strings.h>
#include <unordered_map>

#include <jpeglib.h>

namespace gfx {

namespace {

const double kPi = 3.14159265358979323846;

// The decoded picture behind Img::bmp.
struct Bitmap final : ID2D1Bitmap {
    cairo_surface_t* surface = nullptr;
    int w = 0, h = 0;
    ULONG Release() override {
        if (surface) cairo_surface_destroy(surface);
        delete this;
        return 0;
    }
};

cairo_surface_t* target = nullptr;
cairo_t* cr = nullptr;
bool windowTarget = false;
HWND window = nullptr;
uint32_t generation = 1;
float scale = 1.0f, offX = 0.0f, offY = 0.0f;
cairo_matrix_t base, cur; // design space -> pixels; and with the pushed transforms applied
std::vector<cairo_matrix_t> xformStack;
std::vector<float> layerOpacity;

FT_Library ft = nullptr;
float textScale = 1.0f;
float embolden = 0.0f;
// Accessibility, on top of the two above (see gfx.h).
float accessScale = 1.0f, accessBold = 0.0f;
int colorFilter = 0;
TextTap textTap = nullptr;
TextFilter textFilter = nullptr;
// What a text is drawn and measured as (the menu's language, see SetTextFilter).
const std::wstring& Filtered(const std::wstring& s) {
    return textFilter ? textFilter(s) : s;
}
float viewZoom = 1.0f, viewX = W / 2, viewY = H / 2;

void Apply() {
    if (cr) cairo_set_matrix(cr, &cur);
}

// The menu's colours as the accessibility setting wants them shown.
Color Filtered(Color c) {
    if (colorFilter == 1) return {1.0f - c.r, 1.0f - c.g, 1.0f - c.b, c.a};
    if (colorFilter == 2) {
        float y = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
        return {y, y, y, c.a};
    }
    return c;
}

void Source(Color c) {
    if (colorFilter) c = Filtered(c);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

// The layout fitted to the target, through the enlarged view when there is one.
cairo_matrix_t ViewBase() {
    if (viewZoom == 1.0f) return base;
    cairo_matrix_t m;
    const float s = scale * viewZoom;
    cairo_matrix_init(&m, s, 0, 0, s, offX + (W / 2 - viewX * viewZoom) * scale, offY + (H / 2 - viewY * viewZoom) * scale);
    return m;
}

} // namespace

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

// ---- fonts ----------------------------------------------------------------

namespace {

// One glyph as a Cairo path in em units (y down), ready to be scaled and placed.
struct Glyph {
    std::vector<cairo_path_data_t> path;
    double advance = 0; // em
};

struct Face {
    FT_Face face = nullptr;
    std::vector<uint8_t> data; // the file's bytes, for a font given in memory
    std::string file;
    double upem = 1000;
    // Line metrics in em, chosen the way DirectWrite chooses them.
    double ascent = 0.8, descent = 0.2, lineGap = 0;
    bool kerning = false;
    bool bold = false; // the file itself is a bold or semi-bold design
    std::unordered_map<uint64_t, Glyph> glyphs;

    ~Face() {
        if (face) FT_Done_Face(face);
    }
};

std::unique_ptr<Face> userFace;                       // SetFont
std::unique_ptr<Face> symbolFace;                     // SetSymbolFont
std::unique_ptr<Face> langFace;                       // SetLanguageFont, a font given in memory
Face* langSystemFace = nullptr;                       // ... or an installed one (owned by fileFaces)
std::string langTag;                                  // its language for fontconfig ("zh-cn"), "" none
Face* systemFaces[3] = {};                            // regular, semi-bold, bold (owned by fileFaces)
bool systemTried[3] = {};
std::map<std::string, std::unique_ptr<Face>> fileFaces; // fallback fonts, by file and index
std::map<uint32_t, Face*> fallbackFor;                // code point -> font that has it (or null)
#if !defined(__ANDROID__)
// fontconfig reads the whole font list when it starts, which can take from
// half a second to several (first run on a Mac). That is started in the
// background by Init so the first character that needs a system font does not
// stall a frame; whoever needs the answer first waits for it.
std::shared_future<bool> fcStarting;
bool fcAsked = false, fcReady = false;

bool FontconfigReady() {
    if (!fcAsked) {
        fcAsked = true;
        fcReady = fcStarting.valid() ? fcStarting.get() : FcInit() != FcFalse;
    }
    return fcReady;
}
#endif

void ReadMetrics(Face& f) {
    FT_Face ft2 = f.face;
    f.upem = ft2->units_per_EM > 0 ? ft2->units_per_EM : 1000;
    f.kerning = FT_HAS_KERNING(ft2) != 0;
    f.bold = (ft2->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    double asc = ft2->ascender, desc = -ft2->descender, gap = ft2->height - (ft2->ascender - ft2->descender);
    // DirectWrite: OS/2 typographic metrics when the font asks for them
    // (fsSelection bit 7), otherwise the Windows ascent/descent, with whatever
    // the hhea table's line is taller by as the gap.
    const TT_OS2* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(ft2, FT_SFNT_OS2));
    const TT_HoriHeader* hhea = static_cast<const TT_HoriHeader*>(FT_Get_Sfnt_Table(ft2, FT_SFNT_HHEA));
    if (os2 && os2->version != 0xFFFF) {
        if (os2->usWeightClass >= 600) f.bold = true;
        if ((os2->fsSelection & 0x80) && (os2->sTypoAscender || os2->sTypoDescender)) {
            asc = os2->sTypoAscender;
            desc = -os2->sTypoDescender;
            gap = os2->sTypoLineGap;
        } else if (os2->usWinAscent || os2->usWinDescent) {
            asc = os2->usWinAscent;
            desc = os2->usWinDescent;
            gap = hhea ? (hhea->Ascender - hhea->Descender + hhea->Line_Gap) - (asc + desc) : 0;
        }
    }
    f.ascent = asc / f.upem;
    f.descent = desc / f.upem;
    f.lineGap = (std::max)(0.0, gap) / f.upem;
}

std::unique_ptr<Face> FaceFromMemory(std::vector<uint8_t>&& bytes) {
    auto f = std::make_unique<Face>();
    f->data = std::move(bytes);
    f->file = "(memory)";
    if (!ft || f->data.empty() ||
        FT_New_Memory_Face(ft, f->data.data(), static_cast<FT_Long>(f->data.size()), 0, &f->face) != 0)
        return nullptr;
    if (!FT_IS_SCALABLE(f->face)) return nullptr;
    FT_Select_Charmap(f->face, FT_ENCODING_UNICODE);
    ReadMetrics(*f);
    return f;
}

Face* FaceFromFile(const std::string& file, int index) {
    std::string key = file + "#" + std::to_string(index);
    auto it = fileFaces.find(key);
    if (it != fileFaces.end()) return it->second.get();
    auto f = std::make_unique<Face>();
    f->file = file;
    if (!ft || FT_New_Face(ft, file.c_str(), index, &f->face) != 0 || !FT_IS_SCALABLE(f->face)) {
        fileFaces[key] = nullptr;
        return nullptr;
    }
    FT_Select_Charmap(f->face, FT_ENCODING_UNICODE);
    ReadMetrics(*f);
    return (fileFaces[key] = std::move(f)).get();
}

#if defined(__ANDROID__)
// Android has no fontconfig. Its fonts are plain files in /system/fonts:
// Roboto is the interface font (one variable file since Android 12, a file
// per weight before that), and a character Roboto lacks is looked for in the
// other files there, the Noto families first.

// A variable font set to `weight` on its weight axis, or null if the file has
// no such axis.
Face* VariableFace(const std::string& file, int weight) {
    std::string key = file + "@" + std::to_string(weight);
    auto it = fileFaces.find(key);
    if (it != fileFaces.end()) return it->second.get();
    auto f = std::make_unique<Face>();
    f->file = file;
    FT_MM_Var* mm = nullptr;
    bool set = false;
    if (ft && FT_New_Face(ft, file.c_str(), 0, &f->face) == 0 && FT_IS_SCALABLE(f->face) &&
        FT_Get_MM_Var(f->face, &mm) == 0 && mm) {
        std::vector<FT_Fixed> coords(mm->num_axis);
        for (FT_UInt i = 0; i < mm->num_axis; i++) {
            coords[i] = mm->axis[i].def;
            if (mm->axis[i].tag != FT_MAKE_TAG('w', 'g', 'h', 't')) continue;
            FT_Fixed want = static_cast<FT_Fixed>(weight) * 65536;
            coords[i] = (std::max)(mm->axis[i].minimum, (std::min)(mm->axis[i].maximum, want));
            set = true;
        }
        set = set && FT_Set_Var_Design_Coordinates(f->face, mm->num_axis, coords.data()) == 0;
        FT_Done_MM_Var(ft, mm);
    }
    if (!set) {
        fileFaces[key] = nullptr;
        return nullptr;
    }
    FT_Select_Charmap(f->face, FT_ENCODING_UNICODE);
    ReadMetrics(*f);
    f->bold = weight >= 600; // the weight class in the file is the default instance's
    return (fileFaces[key] = std::move(f)).get();
}

// Every font file of the system, the likeliest sources of a missing character first.
const std::vector<std::string>& SystemFontFiles() {
    static std::vector<std::string> files;
    static bool listed = false;
    if (listed) return files;
    listed = true;
    const std::string dir = "/system/fonts/";
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            size_t dot = name.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string ext = name.substr(dot);
            if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") files.push_back(name);
        }
        closedir(d);
    }
    auto rank = [](const std::string& n) {
        if (n.find("Serif") != std::string::npos || n.find("Mono") != std::string::npos) return 4;
        if (n.compare(0, 11, "NotoSansCJK") == 0) return 0;
        if (n.compare(0, 9, "NotoSans-") == 0 || n.compare(0, 16, "NotoSansSymbols") == 0) return 1;
        if (n.compare(0, 8, "NotoSans") == 0) return 2;
        return 3;
    };
    std::sort(files.begin(), files.end(), [&](const std::string& a, const std::string& b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : a < b;
    });
    for (std::string& f : files) f = dir + f;
    return files;
}

// The system's interface font at an OpenType weight, or, with `codepoint`, a
// font that can draw that character.
Face* MatchSystemFont(int weight, uint32_t codepoint) {
    const std::string roboto = "/system/fonts/Roboto-Regular.ttf";
    if (!codepoint) {
        if (weight >= 600) {
            if (Face* f = VariableFace(roboto, weight)) return f;
            // Separate files: Medium is the nearest to the semi-bold the
            // layout asks for with 600. Neither needs its strokes thickened.
            Face* f = FaceFromFile(weight >= 700 ? "/system/fonts/Roboto-Bold.ttf" : "/system/fonts/Roboto-Medium.ttf", 0);
            if (f) {
                f->bold = true;
                return f;
            }
        }
        if (Face* f = FaceFromFile(roboto, 0)) return f;
        for (const std::string& file : SystemFontFiles()) // a system without Roboto: the first font that loads
            if (Face* f = FaceFromFile(file, 0)) return f;
        return nullptr;
    }
    for (const std::string& file : SystemFontFiles()) {
        Face* f = FaceFromFile(file, 0);
        if (f && FT_Get_Char_Index(f->face, codepoint)) return f;
    }
    return nullptr;
}
#else
// Asks fontconfig for the system's sans-serif font at an OpenType weight, or,
// with `codepoint`, for a font that can draw that character.
Face* MatchSystemFont(int weight, uint32_t codepoint) {
    if (!FontconfigReady()) return nullptr;
    FcPattern* pat = FcPatternCreate();
    if (!pat) return nullptr;
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif"));
    FcPatternAddInteger(pat, FC_WEIGHT, FcWeightFromOpenType(weight));
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    // The menu's language picks the forms of the characters several scripts share (Han).
    if (!langTag.empty()) FcPatternAddString(pat, FC_LANG, reinterpret_cast<const FcChar8*>(langTag.c_str()));
    if (codepoint) {
        FcCharSet* cs = FcCharSetCreate();
        FcCharSetAddChar(cs, codepoint);
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
        FcCharSetDestroy(cs);
    }
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    Face* out = nullptr;
    if (match) {
        FcChar8* file = nullptr;
        int index = 0;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            FcPatternGetInteger(match, FC_INDEX, 0, &index);
            out = FaceFromFile(reinterpret_cast<const char*>(file), index);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return out;
}
#endif

int WeightSlot(int weight) {
    return weight >= 700 ? 2 : weight >= 600 ? 1 : 0;
}

Face* SystemFace(int weight) {
    int slot = WeightSlot(weight);
    if (!systemTried[slot]) {
        systemTried[slot] = true;
        static const int ask[3] = {400, 600, 700};
        systemFaces[slot] = MatchSystemFont(ask[slot], 0);
    }
    return systemFaces[slot];
}

// The font a text of this weight is set in, and whether its strokes have to be
// thickened to stand in for a bold cut that does not exist.
Face* MainFace(int weight, bool& fakeBold) {
    Face* f = userFace ? userFace.get() : SystemFace(weight);
    fakeBold = f && weight >= 600 && !f->bold;
    return f;
}

// DirectWrite's bold simulation, measured on the Windows build with the test
// card (tests/gfxprobe.cpp): every character advances further by a fiftieth
// of the em, counted in whole font units, and the strokes grow by the same
// amount in total.
double FakeBold(const Face& f) {
    return std::floor(f.upem / 50.0) / f.upem;
}

struct PathSink {
    std::vector<cairo_path_data_t>* out;
    double k; // outline units -> em
    bool open = false;
    FT_Vector last{0, 0};

    void point(const FT_Vector* v) {
        cairo_path_data_t d;
        d.point.x = v->x * k;
        d.point.y = -v->y * k;
        out->push_back(d);
    }
    void header(cairo_path_data_type_t type, int length) {
        cairo_path_data_t d;
        d.header.type = type;
        d.header.length = length;
        out->push_back(d);
    }
    void close() {
        if (open) header(CAIRO_PATH_CLOSE_PATH, 1);
        open = false;
    }
};

int SinkMove(const FT_Vector* to, void* user) {
    PathSink* s = static_cast<PathSink*>(user);
    s->close();
    s->header(CAIRO_PATH_MOVE_TO, 2);
    s->point(to);
    s->open = true;
    s->last = *to;
    return 0;
}
int SinkLine(const FT_Vector* to, void* user) {
    PathSink* s = static_cast<PathSink*>(user);
    s->header(CAIRO_PATH_LINE_TO, 2);
    s->point(to);
    s->last = *to;
    return 0;
}
int SinkCubic(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
    PathSink* s = static_cast<PathSink*>(user);
    s->header(CAIRO_PATH_CURVE_TO, 4);
    s->point(c1);
    s->point(c2);
    s->point(to);
    s->last = *to;
    return 0;
}
int SinkConic(const FT_Vector* c, const FT_Vector* to, void* user) {
    // Quadratic -> cubic, exactly.
    PathSink* s = static_cast<PathSink*>(user);
    cairo_path_data_t d;
    s->header(CAIRO_PATH_CURVE_TO, 4);
    d.point.x = (s->last.x + 2.0 * c->x) / 3.0 * s->k;
    d.point.y = -(s->last.y + 2.0 * c->y) / 3.0 * s->k;
    s->out->push_back(d);
    d.point.x = (to->x + 2.0 * c->x) / 3.0 * s->k;
    d.point.y = -(to->y + 2.0 * c->y) / 3.0 * s->k;
    s->out->push_back(d);
    s->point(to);
    s->last = *to;
    return 0;
}

enum : int { kBand = 1, kBold = 2 };

const Glyph& GetGlyph(Face& f, uint32_t index, int variant) {
    uint64_t key = (static_cast<uint64_t>(variant) << 32) | index;
    auto it = f.glyphs.find(key);
    if (it != f.glyphs.end()) return it->second;
    Glyph g;
    // Font units, no hinting: the outline exactly as designed.
    if (FT_Load_Glyph(f.face, index, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) == 0) {
        FT_GlyphSlot slot = f.face->glyph;
        g.advance = slot->metrics.horiAdvance / f.upem;
        if (slot->format == FT_GLYPH_FORMAT_OUTLINE && slot->outline.n_points > 0) {
            FT_Outline* o = &slot->outline;
            // Work in 1/64 font units so a band of 1 % of the em is not
            // rounded to a whole unit.
            for (int i = 0; i < o->n_points; i++) {
                o->points[i].x *= 64;
                o->points[i].y *= 64;
            }
            // FT_Outline_Embolden moves every edge outwards by half of the
            // strength, then shifts the result so the left and bottom stay
            // where they were. For fake bold that is the effect wanted:
            // DirectWrite's bold also keeps the left edge and the baseline and
            // grows to the right and upwards (measured on the test card). The
            // band is a stroke centred on the outline, so its share of the
            // shift is taken back.
            const double band = (variant & kBand) ? embolden + accessBold : 0.0, bold = (variant & kBold) ? FakeBold(f) : 0.0;
            if (band + bold > 0) {
                FT_Outline_Embolden(o, static_cast<FT_Pos>(std::lround((band + bold) * f.upem * 64.0)));
                const FT_Pos back = static_cast<FT_Pos>(std::lround(band * f.upem * 32.0));
                FT_Outline_Translate(o, -back, -back);
            }
            PathSink sink{&g.path, 1.0 / (64.0 * f.upem)};
            FT_Outline_Funcs funcs = {SinkMove, SinkLine, SinkConic, SinkCubic, 0, 0};
            FT_Outline_Decompose(o, &funcs, &sink);
            sink.close();
        }
        if ((variant & kBold) && !g.path.empty()) g.advance += FakeBold(f); // a space stays as wide as it was
    }
    return f.glyphs[key] = std::move(g);
}

void DropGlyphs() {
    if (userFace) userFace->glyphs.clear();
    if (symbolFace) symbolFace->glyphs.clear();
    if (langFace) langFace->glyphs.clear();
    for (auto& kv : fileFaces)
        if (kv.second) kv.second->glyphs.clear();
}

// ---- text layout ----------------------------------------------------------

struct TextSpec {
    float w, h, size;
    int weight;
    Align align;
    bool wrap;     // multi-line, top aligned; otherwise one line, vertically centred, trimmed
    float spacing; // extra space per character
    bool symbol;   // use the symbol font, unscaled and never emboldened
};

struct LaidText {
    std::vector<cairo_path_data_t> path; // layout units, relative to the box's top left
    float width = 0;
};

std::map<std::wstring, LaidText> textCache;
// The Windows backend keeps one text format per (size in eighths, weight) and
// reuses it for every size that lands on the same key, so those all get the
// size of whichever came first. Kept the same here so the two builds place
// text identically.
std::map<int, float> formatSize, symbolFormatSize;

std::wstring CacheKey(const std::wstring& s, const TextSpec& t) {
    wchar_t meta[128];
    swprintf_s(meta, L"\x1f%d|%d|%d|%d|%d|%d|%d|%d", static_cast<int>(t.size * 16), t.weight, static_cast<int>(t.w * 4),
               static_cast<int>(t.h * 4), static_cast<int>(t.align), t.wrap ? 1 : 0, static_cast<int>(t.spacing * 100),
               t.symbol ? 1 : 0);
    return s + meta;
}

float FormatSize(float size, int weight) {
    size *= textScale * accessScale;
    int key = static_cast<int>(size * 8.0f) * 1000 + weight;
    auto it = formatSize.find(key);
    if (it != formatSize.end()) return it->second;
    return formatSize[key] = size;
}

float SymbolFormatSize(float size) {
    int key = static_cast<int>(size * 8.0f);
    auto it = symbolFormatSize.find(key);
    if (it != symbolFormatSize.end()) return it->second;
    return symbolFormatSize[key] = size;
}

struct Item {
    Face* face = nullptr;
    uint32_t glyph = 0;
    uint32_t cp = 0;
    int variant = 0;
    double advance = 0; // layout units, kerning and extra spacing included
};

bool IsSpace(uint32_t cp) {
    return cp == ' ' || cp == 0x3000 || cp == '\t';
}

// A line may be broken before one of these without a space (CJK text).
bool BreaksAnywhere(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || cp >= 0x20000;
}

// The font and glyph for one character: the text's own font, else the system
// font, else whatever installed font has it.
void Resolve(uint32_t cp, Face* primary, int weight, bool symbol, Face*& face, uint32_t& glyph) {
    // The language's own font comes before the main one, as on the console.
    Face* lang = langFace ? langFace.get() : langSystemFace;
    if (lang && lang != primary && !symbol && cp >= 0x20) {
        if (uint32_t g = FT_Get_Char_Index(lang->face, cp)) {
            face = lang;
            glyph = g;
            return;
        }
    }
    face = primary;
    glyph = FT_Get_Char_Index(primary->face, cp);
    if (glyph || symbol || cp < 0x20) return;
    Face* sys = SystemFace(weight);
    if (sys && sys != primary) {
        if (uint32_t g = FT_Get_Char_Index(sys->face, cp)) {
            face = sys;
            glyph = g;
            return;
        }
    }
    auto it = fallbackFor.find(cp);
    Face* fb = nullptr;
    if (it != fallbackFor.end()) {
        fb = it->second;
    } else {
        fb = MatchSystemFont(weight, cp);
        if (fb && !FT_Get_Char_Index(fb->face, cp)) fb = nullptr;
        fallbackFor[cp] = fb;
    }
    if (fb) {
        face = fb;
        glyph = FT_Get_Char_Index(fb->face, cp);
    }
}

std::vector<Item> Shape(const std::vector<uint32_t>& text, Face* primary, const TextSpec& t, double size, bool fakeBold) {
    std::vector<Item> items;
    items.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        Item it;
        it.cp = text[i];
        if (it.cp == '\n' || it.cp == '\r') { // line breaks carry no glyph
            it.face = primary;
            items.push_back(it);
            continue;
        }
        Resolve(it.cp, primary, t.weight, t.symbol, it.face, it.glyph);
        if (!t.symbol) {
            // The band belongs to the main font only. A fallback font that is
            // already bold needs no fake bold.
            if (embolden + accessBold > 0 && (it.face == primary || (langFace && it.face == langFace.get())))
                it.variant |= kBand;
            if (fakeBold && !it.face->bold) it.variant |= kBold;
        }
        it.advance = GetGlyph(*it.face, it.glyph, it.variant).advance * size + t.spacing;
        // The accessibility weight needs room: each letter grows by the width of its band.
        if (!t.symbol) it.advance += accessBold * size;
        if (!items.empty() && items.back().face == it.face && it.face->kerning && items.back().glyph && it.glyph) {
            FT_Vector k{0, 0};
            if (FT_Get_Kerning(it.face->face, items.back().glyph, it.glyph, FT_KERNING_UNSCALED, &k) == 0)
                items.back().advance += k.x / it.face->upem * size;
        }
        items.push_back(it);
    }
    return items;
}

struct Line {
    size_t from = 0, to = 0; // items [from, to)
    bool ellipsis = false;
};

double Width(const std::vector<Item>& items, size_t from, size_t to, bool trimTrailingSpace) {
    if (trimTrailingSpace)
        while (to > from && IsSpace(items[to - 1].cp)) to--;
    double w = 0;
    for (size_t i = from; i < to; i++) w += items[i].advance;
    return w;
}

LaidText Build(const std::wstring& s, const TextSpec& t) {
    LaidText out;
    bool fakeBold = false;
    Face* primary = t.symbol ? symbolFace.get() : MainFace(t.weight, fakeBold);
    if (!primary) return out;
    const double size = t.symbol ? SymbolFormatSize(t.size) : FormatSize(t.size, t.weight);

    std::vector<uint32_t> text;
    text.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = static_cast<uint32_t>(s[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) { // UTF-16 pair left in a wide string
            uint32_t lo = static_cast<uint32_t>(s[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        text.push_back(cp);
    }
    std::vector<Item> items = Shape(text, primary, t, size, fakeBold);

    // Break into lines: always at a line feed, and in wrapped text wherever the
    // box is full.
    std::vector<Line> lines;
    const double limit = t.w + 0.01;
    size_t start = 0;
    while (start <= items.size()) {
        size_t end = start, lastBreak = 0;
        bool haveBreak = false, hard = false;
        double x = 0;
        while (end < items.size()) {
            const Item& it = items[end];
            if (it.cp == '\n') {
                hard = true;
                break;
            }
            if (t.wrap && end > start && !IsSpace(it.cp) && x + it.advance - t.spacing > limit) {
                if (haveBreak) end = lastBreak;
                break; // without a break point the word is split right here
            }
            x += it.advance;
            end++;
            if (end < items.size() && (IsSpace(it.cp) || it.cp == '-' || BreaksAnywhere(items[end].cp) || BreaksAnywhere(it.cp))) {
                lastBreak = end;
                haveBreak = true;
            }
        }
        lines.push_back({start, end, false});
        if (end >= items.size()) break;
        start = hard ? end + 1 : end;
        if (!hard)
            while (start < items.size() && items[start].cp == ' ') start++; // spaces at a wrap hang off the line
    }

    // One-line text that is too wide ends in an ellipsis.
    std::vector<Item> ellipsis;
    double ellipsisWidth = 0;
    if (!t.wrap && !t.symbol) {
        for (Line& ln : lines) {
            if (Width(items, ln.from, ln.to, true) <= limit) continue;
            if (ellipsis.empty()) {
                TextSpec plain = t;
                plain.spacing = 0;
                ellipsis = Shape({0x2026}, primary, plain, size, fakeBold);
                ellipsisWidth = Width(ellipsis, 0, ellipsis.size(), false);
            }
            double x = 0;
            size_t keep = ln.from;
            while (keep < ln.to && x + items[keep].advance + ellipsisWidth <= limit) x += items[keep++].advance;
            while (keep > ln.from && IsSpace(items[keep - 1].cp)) keep--; // the dots follow a letter, not a gap
            ln.to = keep;
            ln.ellipsis = true;
        }
    }

    const double lineHeight = (primary->ascent + primary->descent + primary->lineGap) * size;
    const double top = t.wrap ? 0.0 : (t.h - lineHeight * static_cast<double>(lines.size())) * 0.5;
    auto emit = [&](const Item& it, double x, double y) {
        if (it.cp < 0x20) return;
        const Glyph& g = GetGlyph(*it.face, it.glyph, it.variant);
        for (size_t i = 0; i < g.path.size();) {
            cairo_path_data_t head = g.path[i];
            out.path.push_back(head);
            for (int k = 1; k < head.header.length; k++) {
                cairo_path_data_t d;
                d.point.x = g.path[i + static_cast<size_t>(k)].point.x * size + x;
                d.point.y = g.path[i + static_cast<size_t>(k)].point.y * size + y;
                out.path.push_back(d);
            }
            i += static_cast<size_t>(head.header.length);
        }
    };
    for (size_t li = 0; li < lines.size(); li++) {
        const Line& ln = lines[li];
        double full = Width(items, ln.from, ln.to, false) + (ln.ellipsis ? ellipsisWidth : 0);
        double inked = Width(items, ln.from, ln.to, !ln.ellipsis) + (ln.ellipsis ? ellipsisWidth : 0);
        out.width = (std::max)(out.width, static_cast<float>(full));
        double x = t.align == Center ? (t.w - inked) * 0.5 : t.align == Right ? t.w - inked : 0.0;
        // DirectWrite puts a font's line gap above the letters, not below.
        double y = top + lineHeight * static_cast<double>(li) + (primary->lineGap + primary->ascent) * size;
        for (size_t i = ln.from; i < ln.to; i++) {
            emit(items[i], x, y);
            x += items[i].advance;
        }
        if (ln.ellipsis)
            for (const Item& it : ellipsis) {
                emit(it, x, y);
                x += it.advance;
            }
    }
    return out;
}

LaidText* Lay(const std::wstring& s, const TextSpec& t) {
    std::wstring key = CacheKey(s, t);
    auto it = textCache.find(key);
    if (it != textCache.end()) return &it->second;
    if (textCache.size() > 600) textCache.clear(); // strings that change (the clock) would pile up otherwise
    return &(textCache[key] = Build(s, t));
}

void FillLaid(LaidText* t, float x, float y, Color c) {
    if (!t || t->path.empty() || !cr) return;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_path_t path;
    path.status = CAIRO_STATUS_SUCCESS;
    path.data = t->path.data();
    path.num_data = static_cast<int>(t->path.size());
    cairo_new_path(cr);
    cairo_append_path(cr, &path);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    Source(c);
    cairo_fill(cr);
    cairo_restore(cr);
}

void DropText() {
    textCache.clear();
}

} // namespace

bool Init() {
    if (!ft && FT_Init_FreeType(&ft) != 0) {
        ft = nullptr;
        return false;
    }
    cairo_matrix_init_identity(&base);
    cur = base;
#if !defined(__ANDROID__)
    if (!fcStarting.valid() && !fcAsked)
        fcStarting = std::async(std::launch::async, [] { return FcInit() != FcFalse; }).share();
#endif
    return true;
}

void Shutdown() {
    DropText();
    formatSize.clear();
    symbolFormatSize.clear();
    fallbackFor.clear();
    for (int i = 0; i < 3; i++) {
        systemTried[i] = false;
        systemFaces[i] = nullptr;
    }
    layerOpacity.clear();
    userFace.reset();
    symbolFace.reset();
    langFace.reset();
    langSystemFace = nullptr;
    langTag.clear();
    fileFaces.clear();
    if (cr) cairo_destroy(cr);
    cr = nullptr;
    if (target) cairo_surface_destroy(target);
    target = nullptr;
    if (ft) FT_Done_FreeType(ft);
    ft = nullptr;
    embolden = 0.0f;
#if !defined(__ANDROID__)
    // fontconfig is left initialised: FcFini asserts if anything else in the
    // process still holds a reference. Its start-up must have finished, though.
    if (fcStarting.valid()) fcStarting.wait();
#endif
}

bool SetFont(std::vector<uint8_t>&& ttf) {
    std::unique_ptr<Face> f = FaceFromMemory(std::move(ttf));
    if (!f) return false;
    userFace = std::move(f);
    formatSize.clear();
    DropText();
    return true;
}

bool SetSymbolFont(std::vector<uint8_t>&& ttf) {
    std::unique_ptr<Face> f = FaceFromMemory(std::move(ttf));
    if (!f) return false;
    symbolFace = std::move(f);
    symbolFormatSize.clear();
    DropText();
    return true;
}

bool HasSymbolFont() {
    return symbolFace != nullptr;
}

bool SetLanguageFont(std::vector<uint8_t>&& ttf, const std::wstring& systemFamily, const std::wstring& locale) {
    bool ok = true;
    langFace.reset();
    langSystemFace = nullptr;
    langTag.clear();
    for (wchar_t c : locale) langTag += static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c);
    if (!ttf.empty()) {
        langFace = FaceFromMemory(std::move(ttf));
        ok = langFace != nullptr;
    }
#if !defined(__ANDROID__)
    // An installed font of that name, only if fontconfig really has it (it always answers something).
    if (!langFace && !systemFamily.empty() && FontconfigReady()) {
        std::string family = port::Utf8(systemFamily);
        if (FcPattern* pat = FcPatternCreate()) {
            FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
            FcConfigSubstitute(nullptr, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);
            FcResult result = FcResultNoMatch;
            if (FcPattern* match = FcFontMatch(nullptr, pat, &result)) {
                FcChar8* got = nullptr;
                FcChar8* file = nullptr;
                int index = 0;
                if (FcPatternGetString(match, FC_FAMILY, 0, &got) == FcResultMatch && got &&
                    !strcasecmp(reinterpret_cast<const char*>(got), family.c_str()) &&
                    FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
                    FcPatternGetInteger(match, FC_INDEX, 0, &index);
                    langSystemFace = FaceFromFile(reinterpret_cast<const char*>(file), index);
                }
                FcPatternDestroy(match);
            }
            FcPatternDestroy(pat);
        }
    }
#else
    (void)systemFamily;
#endif
    fallbackFor.clear(); // chosen for the previous language
    formatSize.clear();
    DropText();
    return ok;
}

void SetTextScale(float factor) {
    textScale = factor;
    formatSize.clear();
    SetTextEmbolden(embolden); // drops the laid-out text, which was built at the old scale
}

void SetTextEmbolden(float fraction) {
    embolden = fraction;
    DropGlyphs();
    DropText();
}

void SetTextAccess(float factor, float extraEmbolden) {
    if (factor == accessScale && extraEmbolden == accessBold) return;
    accessScale = factor;
    accessBold = extraEmbolden;
    formatSize.clear();
    SetTextEmbolden(embolden); // drops the glyphs and the laid-out text
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

void SetView(float zoom, float cx, float cy) {
    viewZoom = zoom;
    viewX = cx;
    viewY = cy;
}

void ToLayout(float& x, float& y) {
    double px = x, py = y;
    cairo_matrix_transform_point(&cur, &px, &py);
    x = (static_cast<float>(px) - offX) / scale;
    y = (static_cast<float>(py) - offY) / scale;
    if (viewZoom != 1.0f) {
        x = (x - W / 2) / viewZoom + viewX;
        y = (y - H / 2) / viewZoom + viewY;
    }
}

std::string FontDescription() {
    bool fake = false;
    Face* f = MainFace(400, fake);
    return f ? f->file : std::string("(none)");
}

// ---- targets ----------------------------------------------------------------

namespace {

float overscan = 1.0f, fitW = 0.0f, fitH = 0.0f; // see SetOverscan

void Fit(float pw, float ph) {
    fitW = pw;
    fitH = ph;
    scale = (std::min)(pw / W, ph / H) * overscan;
    offX = (pw - W * scale) * 0.5f;
    offY = (ph - H * scale) * 0.5f;
    cairo_matrix_init(&base, scale, 0, 0, scale, offX, offY);
    cur = base;
    xformStack.clear();
}

bool CreateTarget(int w, int h, cairo_format_t format) {
    if (cr) cairo_destroy(cr);
    cr = nullptr;
    if (target) cairo_surface_destroy(target);
    target = cairo_image_surface_create(format, w, h);
    if (cairo_surface_status(target) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(target);
        target = nullptr;
        return false;
    }
    cr = cairo_create(target);
    Fit(static_cast<float>(w), static_cast<float>(h));
    return true;
}

} // namespace

void SetOverscan(float fraction) {
    fraction = (std::max)(0.5f, (std::min)(1.0f, fraction));
    if (fraction == overscan) return;
    overscan = fraction;
    if (fitW > 0) Fit(fitW, fitH);
}

bool AttachWindow(HWND hwnd) {
    // The window itself belongs to main_sdl.cpp. This only sets up a picture
    // to draw into; Resize gives it the window's real size.
    window = hwnd;
    windowTarget = true;
    return CreateTarget(static_cast<int>(W), static_cast<int>(H), CAIRO_FORMAT_RGB24);
}

void Resize(UINT w, UINT h) {
    if (!windowTarget || w == 0 || h == 0) return;
    if (target && cairo_image_surface_get_width(target) == static_cast<int>(w) &&
        cairo_image_surface_get_height(target) == static_cast<int>(h))
        return;
    CreateTarget(static_cast<int>(w), static_cast<int>(h), CAIRO_FORMAT_RGB24);
}

bool AttachOffscreen(UINT w, UINT h) {
    windowTarget = false;
    return CreateTarget(static_cast<int>(w), static_cast<int>(h), CAIRO_FORMAT_ARGB32);
}

bool SaveOffscreenPng(const std::wstring& file) {
    if (!target) return false;
    cairo_surface_flush(target);
    return cairo_surface_write_to_png(target, port::NativePath(file).c_str()) == CAIRO_STATUS_SUCCESS;
}

const uint8_t* FramePixels(int& w, int& h, int& stride) {
    if (!target) return nullptr;
    cairo_surface_flush(target);
    w = cairo_image_surface_get_width(target);
    h = cairo_image_surface_get_height(target);
    stride = cairo_image_surface_get_stride(target);
    return cairo_image_surface_get_data(target);
}

void Begin(Color clear) {
    if (!target) return;
    // A fresh context each frame: no clip, layer or transform can leak from
    // the frame before.
    if (cr) cairo_destroy(cr);
    cr = cairo_create(target);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cur = ViewBase();
    xformStack.clear();
    layerOpacity.clear();
    Apply();
    Rect(0, 0, W, H, clear);
}

bool End() {
    if (!target) return false;
    cairo_surface_flush(target);
    return true;
}

void ToDesign(int px, int py, float& x, float& y) {
    x = (px - offX) / scale;
    y = (py - offY) / scale;
    if (viewZoom != 1.0f) {
        x = (x - W / 2) / viewZoom + viewX;
        y = (y - H / 2) / viewZoom + viewY;
    }
}

// ---- shapes -----------------------------------------------------------------

namespace {

// Rounded rectangle with the corner radius limited to half of each side, which
// turns a box with a large radius into a pill or a circle.
void RoundedPath(float x, float y, float w, float h, float r) {
    double rx = (std::min)(static_cast<double>(r), w * 0.5), ry = (std::min)(static_cast<double>(r), h * 0.5);
    cairo_new_path(cr);
    if (rx <= 0 || ry <= 0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    auto corner = [&](double cx, double cy, double a0, double a1) {
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1, a0, a1);
        cairo_restore(cr);
    };
    corner(x + w - rx, y + ry, -kPi / 2, 0);
    corner(x + w - rx, y + h - ry, 0, kPi / 2);
    corner(x + rx, y + h - ry, kPi / 2, kPi);
    corner(x + rx, y + ry, kPi, kPi * 1.5);
    cairo_close_path(cr);
}

void Stroke(float sw, cairo_line_cap_t cap = CAIRO_LINE_CAP_BUTT) {
    cairo_set_line_width(cr, sw);
    cairo_set_line_cap(cr, cap);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_stroke(cr);
}

} // namespace

void Rect(float x, float y, float w, float h, Color c) {
    if (!cr) return;
    cairo_new_path(cr);
    cairo_rectangle(cr, x, y, w, h);
    Source(c);
    cairo_fill(cr);
}

void RRect(float x, float y, float w, float h, float r, Color c) {
    if (!cr) return;
    RoundedPath(x, y, w, h, r);
    Source(c);
    cairo_fill(cr);
}

void RRectStroke(float x, float y, float w, float h, float r, Color c, float sw) {
    if (!cr) return;
    RoundedPath(x, y, w, h, r);
    Source(c);
    Stroke(sw);
}

void RRectStrokeGrad(float x, float y, float w, float h, float r, const Color* stops, int count, float angle, float sw,
                     float opacity) {
    if (!cr || opacity <= 0.0f || count < 2 || count > 8) return;
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    float half = 0.5f * sqrtf(w * w + h * h);
    float a = angle * 3.14159265f / 180.0f;
    float dx = cosf(a) * half, dy = sinf(a) * half;
    cairo_pattern_t* grad = cairo_pattern_create_linear(cx - dx, cy - dy, cx + dx, cy + dy);
    for (int i = 0; i < count; i++) {
        const Color stop = colorFilter ? Filtered(stops[i]) : stops[i];
        cairo_pattern_add_color_stop_rgba(grad, static_cast<double>(i) / (count - 1), stop.r, stop.g, stop.b,
                                          stop.a * opacity);
    }
    RoundedPath(x, y, w, h, r);
    cairo_set_source(cr, grad);
    Stroke(sw);
    cairo_pattern_destroy(grad);
}

void Circle(float cx, float cy, float r, Color c) {
    if (!cr) return;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * kPi);
    cairo_close_path(cr);
    Source(c);
    cairo_fill(cr);
}

void CircleStroke(float cx, float cy, float r, Color c, float sw) {
    if (!cr) return;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * kPi);
    cairo_close_path(cr);
    Source(c);
    Stroke(sw);
}

void Line(float x0, float y0, float x1, float y1, Color c, float sw) {
    if (!cr) return;
    cairo_new_path(cr);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    Source(c);
    Stroke(sw);
}

void Arc(float cx, float cy, float r, float a0, float a1, Color c, float sw) {
    if (!cr) return;
    const double k = kPi / 180.0;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, a0 * k, a1 * k); // increasing angle = clockwise on screen
    Source(c);
    Stroke(sw, CAIRO_LINE_CAP_ROUND);
}

void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    if (!cr) return;
    cairo_new_path(cr);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_close_path(cr);
    Source(c);
    cairo_fill(cr);
}

// ---- text -------------------------------------------------------------------

// Tells the tap where this text is: across, as far as the laid-out width
// reaches from the side it is aligned to; down, its box.
static void Tapped(const std::wstring& s, LaidText* t, float x, float y, float w, float h, Align align) {
    if (!t) return;
    const float tw = (std::min)(t->width, w);
    float x0 = align == Left ? x : align == Center ? x + (w - tw) * 0.5f : x + w - tw, x1 = x0 + tw;
    float y0 = y, y1 = y + h;
    ToLayout(x0, y0);
    ToLayout(x1, y1);
    textTap(s, x0, y0, x1, y1);
}

void Text(const std::wstring& text, float x, float y, float w, float h, float size, Color c, Align align, int weight) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return;
    LaidText* t = Lay(s, {w, h, size, weight, align, false, 0.0f, false});
    FillLaid(t, x, y, c);
    if (textTap) Tapped(s, t, x, y, w, h, align);
}

void TextWrap(const std::wstring& text, float x, float y, float w, float h, float size, Color c, Align align, int weight) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return;
    PushClip(x - 2, y - 2, w + 4, h + 4);
    LaidText* t = Lay(s, {w, h, size, weight, align, true, 0.0f, false});
    FillLaid(t, x, y, c);
    PopClip();
    if (textTap) Tapped(s, t, x, y, w, h, align);
}

void TextSpaced(const std::wstring& text, float x, float y, float w, float h, float size, Color c, float spacing,
                Align align) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return;
    LaidText* t = Lay(s, {w, h, size, 400, align, false, spacing, false});
    FillLaid(t, x, y, c);
    if (textTap) Tapped(s, t, x, y, w, h, align);
}

float TextSpacedWidth(const std::wstring& text, float size, float spacing) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return 0.0f;
    // No gap after the last character.
    return Lay(s, {10000.0f, 100.0f, size, 400, Left, false, spacing, false})->width - spacing;
}

bool Symbol(wchar_t codepoint, float cx, float cy, float size, Color c) {
    if (!symbolFace) return false;
    float box = size * 3.0f;
    FillLaid(Lay(std::wstring(1, codepoint), {box, box, size, 400, Center, false, 0.0f, true}), cx - box / 2, cy - box / 2, c);
    return true;
}

float TextWidth(const std::wstring& text, float size, int weight) {
    const std::wstring& s = Filtered(text);
    if (s.empty()) return 0.0f;
    return Lay(s, {10000.0f, 100.0f, size, weight, Left, false, 0.0f, false})->width;
}

// ---- images -----------------------------------------------------------------

namespace {

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(port::NativePath(path).c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return !out.empty();
}

struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
};

void JpegFail(j_common_ptr info) {
    longjmp(reinterpret_cast<JpegError*>(info->err)->jump, 1);
}
void JpegQuiet(j_common_ptr, int) {}

// JPEG -> a new RGB24 Cairo picture, or null. No C++ objects live across the
// decoder's error jump.
cairo_surface_t* DecodeJpeg(const uint8_t* data, size_t size) {
    jpeg_decompress_struct info;
    JpegError err;
    cairo_surface_t* volatile surface = nullptr;
    uint8_t* volatile row = nullptr;
    info.err = jpeg_std_error(&err.base);
    err.base.error_exit = JpegFail;
    err.base.emit_message = JpegQuiet;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&info);
        free(row);
        if (surface) cairo_surface_destroy(surface);
        return nullptr;
    }
    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, const_cast<unsigned char*>(data), static_cast<unsigned long>(size));
    jpeg_read_header(&info, TRUE);
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);
    const int w = static_cast<int>(info.output_width), h = static_cast<int>(info.output_height);
    if (w <= 0 || h <= 0 || info.output_components != 3) longjmp(err.jump, 1);
    surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) longjmp(err.jump, 1);
    row = static_cast<uint8_t*>(malloc(static_cast<size_t>(w) * 3));
    if (!row) longjmp(err.jump, 1);
    uint8_t* pixels = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    while (info.output_scanline < info.output_height) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(pixels + static_cast<size_t>(info.output_scanline) * stride);
        JSAMPROW rows[1] = {row};
        jpeg_read_scanlines(&info, rows, 1);
        for (int x = 0; x < w; x++)
            dst[x] = 0xFF000000u | (static_cast<uint32_t>(row[x * 3]) << 16) | (static_cast<uint32_t>(row[x * 3 + 1]) << 8) |
                     row[x * 3 + 2];
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    free(row);
    cairo_surface_mark_dirty(surface);
    return surface;
}

bool EncodeJpeg(cairo_surface_t* surface, const std::string& file, int quality) {
    cairo_surface_flush(surface);
    const int w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const uint8_t* pixels = cairo_image_surface_get_data(surface);
    FILE* volatile f = fopen(file.c_str(), "wb");
    if (!f) return false;
    uint8_t* volatile row = static_cast<uint8_t*>(malloc(static_cast<size_t>(w) * 3));
    jpeg_compress_struct info;
    JpegError err;
    info.err = jpeg_std_error(&err.base);
    err.base.error_exit = JpegFail;
    err.base.emit_message = JpegQuiet;
    if (!row || setjmp(err.jump)) {
        if (row) jpeg_destroy_compress(&info);
        free(row);
        fclose(f);
        return false;
    }
    jpeg_create_compress(&info);
    jpeg_stdio_dest(&info, f);
    info.image_width = static_cast<JDIMENSION>(w);
    info.image_height = static_cast<JDIMENSION>(h);
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, quality, TRUE);
    jpeg_start_compress(&info, TRUE);
    for (int y = 0; y < h; y++) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(pixels + static_cast<size_t>(y) * stride);
        for (int x = 0; x < w; x++) {
            row[x * 3] = static_cast<uint8_t>(src[x] >> 16);
            row[x * 3 + 1] = static_cast<uint8_t>(src[x] >> 8);
            row[x * 3 + 2] = static_cast<uint8_t>(src[x]);
        }
        JSAMPROW rows[1] = {row};
        jpeg_write_scanlines(&info, rows, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    free(row);
    return fclose(f) == 0;
}

struct PngSource {
    const uint8_t* data;
    size_t size, at;
};

cairo_status_t PngRead(void* closure, unsigned char* out, unsigned int length) {
    PngSource* s = static_cast<PngSource*>(closure);
    if (s->size - s->at < length) return CAIRO_STATUS_READ_ERROR;
    memcpy(out, s->data + s->at, length);
    s->at += length;
    return CAIRO_STATUS_SUCCESS;
}

// Area-average reduction to tw x th, on premultiplied pixels (the Windows
// backend reduces with WIC's "Fant" filter, which averages the same way).
cairo_surface_t* Shrink(cairo_surface_t* src, int tw, int th) {
    const int sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src);
    cairo_surface_t* dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
    if (cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(dst);
        return nullptr;
    }
    cairo_surface_flush(src);
    const uint8_t* sp = cairo_image_surface_get_data(src);
    uint8_t* dp = cairo_image_surface_get_data(dst);
    const int ss = cairo_image_surface_get_stride(src), ds = cairo_image_surface_get_stride(dst);
    const double fx = static_cast<double>(sw) / tw, fy = static_cast<double>(sh) / th;
    for (int y = 0; y < th; y++) {
        const double y0 = y * fy, y1 = (y + 1) * fy;
        uint32_t* out = reinterpret_cast<uint32_t*>(dp + static_cast<size_t>(y) * ds);
        for (int x = 0; x < tw; x++) {
            const double x0 = x * fx, x1 = (x + 1) * fx;
            double acc[4] = {0, 0, 0, 0}, total = 0;
            for (int yy = static_cast<int>(y0); yy < sh && yy < y1; yy++) {
                const double wy = (std::min)(y1, yy + 1.0) - (std::max)(y0, static_cast<double>(yy));
                if (wy <= 0) continue;
                const uint32_t* in = reinterpret_cast<const uint32_t*>(sp + static_cast<size_t>(yy) * ss);
                for (int xx = static_cast<int>(x0); xx < sw && xx < x1; xx++) {
                    const double wgt = wy * ((std::min)(x1, xx + 1.0) - (std::max)(x0, static_cast<double>(xx)));
                    if (wgt <= 0) continue;
                    const uint32_t p = in[xx];
                    acc[0] += wgt * (p >> 24);
                    acc[1] += wgt * ((p >> 16) & 255);
                    acc[2] += wgt * ((p >> 8) & 255);
                    acc[3] += wgt * (p & 255);
                    total += wgt;
                }
            }
            uint32_t v[4];
            for (int k = 0; k < 4; k++) v[k] = total > 0 ? static_cast<uint32_t>(acc[k] / total + 0.5) : 0;
            out[x] = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
        }
    }
    cairo_surface_mark_dirty(dst);
    return dst;
}

void Decode(Img& img) {
    img.failed = true;
    std::vector<uint8_t> fileBytes;
    const std::vector<uint8_t>* bytes = &img.mem;
    if (img.mem.empty()) {
        if (!ReadFileBytes(img.path, fileBytes)) return;
        bytes = &fileBytes;
    }
    const uint8_t* d = bytes->data();
    const size_t n = bytes->size();
    cairo_surface_t* raw = nullptr;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        PngSource src{d, n, 0};
        raw = cairo_image_surface_create_from_png_stream(PngRead, &src);
    } else if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) {
        raw = DecodeJpeg(d, n);
    }
    if (!raw) return; // other formats (BMP, GIF) are not decoded here
    if (cairo_surface_status(raw) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(raw);
        return;
    }
    // Everything downstream wants 32-bit premultiplied pixels.
    cairo_format_t fmt = cairo_image_surface_get_format(raw);
    int w = cairo_image_surface_get_width(raw), h = cairo_image_surface_get_height(raw);
    if (w <= 0 || h <= 0) {
        cairo_surface_destroy(raw);
        return;
    }
    if (fmt != CAIRO_FORMAT_ARGB32 && fmt != CAIRO_FORMAT_RGB24) {
        cairo_surface_t* conv = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t* c = cairo_create(conv);
        cairo_set_source_surface(c, raw, 0, 0);
        cairo_paint(c);
        cairo_destroy(c);
        cairo_surface_destroy(raw);
        raw = conv;
    } else if (fmt == CAIRO_FORMAT_RGB24) {
        // The unused byte is not guaranteed to be 255; Shrink reads it as alpha.
        cairo_surface_flush(raw);
        uint8_t* p = cairo_image_surface_get_data(raw);
        const int stride = cairo_image_surface_get_stride(raw);
        for (int y = 0; y < h; y++) {
            uint32_t* rowp = reinterpret_cast<uint32_t*>(p + static_cast<size_t>(y) * stride);
            for (int x = 0; x < w; x++) rowp[x] |= 0xFF000000u;
        }
        cairo_surface_mark_dirty(raw);
    }
    int longest = (std::max)(w, h);
    if (img.maxSize > 0 && longest > img.maxSize) {
        int tw = (std::max)(1, w * img.maxSize / longest), th = (std::max)(1, h * img.maxSize / longest);
        if (cairo_surface_t* small = Shrink(raw, tw, th)) {
            cairo_surface_destroy(raw);
            raw = small;
            w = tw;
            h = th;
        }
    }
    if (colorFilter) {
        // The picture as the colour setting wants it: converted here, once.
        // Both formats hold native-endian 0xAARRGGBB, premultiplied (the
        // opaque one had its alpha byte set above, or was drawn opaque).
        cairo_surface_flush(raw);
        uint8_t* p = cairo_image_surface_get_data(raw);
        const int stride = cairo_image_surface_get_stride(raw);
        const bool opaque = cairo_image_surface_get_format(raw) == CAIRO_FORMAT_RGB24;
        for (int y = 0; y < h; y++) {
            uint32_t* rowp = reinterpret_cast<uint32_t*>(p + static_cast<size_t>(y) * stride);
            for (int x = 0; x < w; x++) {
                const uint32_t v = rowp[x], a = opaque ? 255u : v >> 24;
                uint32_t r = (v >> 16) & 255, g = (v >> 8) & 255, bl = v & 255;
                if (colorFilter == 1) {
                    r = a - (std::min)(r, a);
                    g = a - (std::min)(g, a);
                    bl = a - (std::min)(bl, a);
                } else {
                    r = g = bl = (r * 299 + g * 587 + bl * 114 + 500) / 1000;
                }
                rowp[x] = (a << 24) | (r << 16) | (g << 8) | bl;
            }
        }
        cairo_surface_mark_dirty(raw);
    }
    Bitmap* b = new Bitmap;
    b->surface = raw;
    b->w = w;
    b->h = h;
    img.bmp = b;
    img.failed = false;
}

} // namespace

void Image(Img& img, float x, float y, float w, float h, float opacity, int shape, float radius) {
    if (img.empty() || !cr) return;
    if (img.generation != generation) {
        img.reset();
        img.generation = generation;
    }
    if (!img.bmp && !img.failed) Decode(img);
    if (!img.bmp) return;
    Bitmap* b = static_cast<Bitmap*>(img.bmp);

    float s = (std::max)(w / b->w, h / b->h); // cover
    float dx = x + (w - b->w * s) * 0.5f, dy = y + (h - b->h * s) * 0.5f;

    cairo_save(cr);
    if (shape == 1) {
        cairo_new_path(cr);
        cairo_save(cr);
        cairo_translate(cr, x + w * 0.5f, y + h * 0.5f);
        cairo_scale(cr, w * 0.5f, h * 0.5f);
        cairo_arc(cr, 0, 0, 1, 0, 2 * kPi);
        cairo_restore(cr);
    } else if (radius > 0.0f) {
        RoundedPath(x, y, w, h, radius);
    } else {
        cairo_new_path(cr);
        cairo_rectangle(cr, x, y, w, h);
    }
    cairo_clip(cr);
    cairo_pattern_t* pat = cairo_pattern_create_for_surface(b->surface);
    cairo_matrix_t m;
    cairo_matrix_init_scale(&m, 1.0 / s, 1.0 / s);
    cairo_matrix_translate(&m, -dx, -dy);
    cairo_pattern_set_matrix(pat, &m);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR); // Direct2D's linear interpolation
    cairo_set_source(cr, pat);
    cairo_paint_with_alpha(cr, opacity);
    cairo_pattern_destroy(pat);
    cairo_restore(cr);
}

bool ImageSize(Img& img, float& w, float& h) {
    if (img.empty()) return false;
    if (img.generation != generation) {
        img.reset();
        img.generation = generation;
    }
    if (!img.bmp && !img.failed) Decode(img);
    if (!img.bmp) return false;
    const Bitmap* b = static_cast<const Bitmap*>(img.bmp);
    w = static_cast<float>(b->w);
    h = static_cast<float>(b->h);
    return true;
}

// MakeAvatar and MakeAvatarFramed: `frame` null = the picture cropped to fill.
static bool PaintAvatar(const std::wstring& srcImage, Color bg, const std::wstring& initial, const PicFrame* frame,
                        const std::wstring& outJpeg) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 256, 256);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }
    // Borrow the drawing helpers by pointing them at this picture for a moment.
    cairo_t* saved = cr;
    cairo_matrix_t savedCur = cur;
    cr = cairo_create(surface);
    cairo_matrix_init_identity(&cur);
    const int keptFilter = colorFilter; // the file is the user's picture, not a view of it
    colorFilter = 0;
    bool ok = true;
    Rect(0, 0, 256, 256, bg);
    if (!srcImage.empty()) {
        Img im;
        im.path = srcImage;
        // A zoomed framing shows a part of the picture: decoded larger so that
        // part still has the file's 256 pixels' worth (up to 2048).
        im.maxSize = frame ? static_cast<int>((std::min)(2048.0f, 512.0f * frame->zoom)) : 512;
        im.generation = generation;
        if (frame) ImageFramed(im, 0, 0, 256, *frame);
        else Image(im, 0, 0, 256, 256);
        ok = im.bmp != nullptr;
        im.reset();
    } else {
        Text(initial, 0, -8, 256, 256, 150, rgb(0xFFFFFF), Center, 600);
    }
    cairo_destroy(cr);
    cr = saved;
    cur = savedCur;
    colorFilter = keptFilter;
    ok = ok && EncodeJpeg(surface, port::NativePath(outJpeg), 90);
    cairo_surface_destroy(surface);
    return ok;
}

bool MakeAvatar(const std::wstring& srcImage, Color bg, const std::wstring& initial, const std::wstring& outJpeg) {
    return PaintAvatar(srcImage, bg, initial, nullptr, outJpeg);
}

bool MakeAvatarFramed(const std::wstring& srcImage, const PicFrame& f, const std::wstring& outJpeg) {
    return !srcImage.empty() && PaintAvatar(srcImage, rgb(0x000000), L"", &f, outJpeg);
}

// ---- clips, transforms, layers ------------------------------------------------
// Cairo keeps clip and transform in one saved state; Direct2D keeps them
// apart. The transform is therefore tracked here and re-applied whenever a
// Cairo restore would have brought an older one back.

void PushClip(float x, float y, float w, float h) {
    if (!cr) return;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE); // hard edge, like the Windows clip
    cairo_clip(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
}

void PopClip() {
    if (!cr) return;
    cairo_restore(cr);
    Apply();
}

void PushScale(float s, float cx, float cy) {
    xformStack.push_back(cur);
    cairo_matrix_t m;
    cairo_matrix_init(&m, s, 0, 0, s, cx - s * cx, cy - s * cy);
    cairo_matrix_multiply(&cur, &m, &cur); // m first, then what was there
    Apply();
}

void PushRotate(float degrees, float cx, float cy) {
    xformStack.push_back(cur);
    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, cx, cy);
    cairo_matrix_rotate(&m, degrees * kPi / 180.0); // clockwise with y down
    cairo_matrix_translate(&m, -cx, -cy);
    cairo_matrix_multiply(&cur, &m, &cur);
    Apply();
}

void PushOffset(float dx, float dy) {
    xformStack.push_back(cur);
    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, dx, dy);
    cairo_matrix_multiply(&cur, &m, &cur);
    Apply();
}

void PopTransform() {
    if (xformStack.empty()) return;
    cur = xformStack.back();
    xformStack.pop_back();
    Apply();
}

void PushLayer(float opacity) {
    if (!cr) return;
    cairo_push_group(cr);
    layerOpacity.push_back(opacity);
}

void PopLayer() {
    if (!cr || layerOpacity.empty()) return;
    float opacity = layerOpacity.back();
    layerOpacity.pop_back();
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, opacity);
    Apply();
}

} // namespace gfx
