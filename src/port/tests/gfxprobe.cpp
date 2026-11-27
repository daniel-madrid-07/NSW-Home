// Test card for the drawing layer. Uses nothing but gfx.h (plus the font
// loader), so the same file builds against the Direct2D backend on Windows
// (build_gfxprobe_win.bat) and against the Cairo backend elsewhere (CMake
// target nswhome_gfxprobe). Comparing the two outputs shows where the
// backends differ:
//
//   gfxprobe <console|system> <card.png> <metrics.txt>
//
// "console" draws text in the console font from the installed firmware, as
// the program does; "system" uses the system font. The card is 1920x1080;
// the metrics file lists text widths.
#include "app.h"
#include "fwfont.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace gfx;

// main.cpp/main_sdl.cpp own these in the real program.
std::vector<Game> games;
std::string runningId;
bool scanning = false;

static std::wstring FromArg(const char* s) {
    return Widen(s);
}

static void Guide(float x, float y, float w, float h) {
    RRectStroke(x, y, w, h, 0, rgb(0x00FF00, 0.6f), 0.5f);
}

static void Card(bool haveIcons) {
    const Color white = rgb(0xFFFFFF), ink = rgb(0xE6E6E6), red = rgb(0xE60012), blue = rgb(0x0AB9E6);
    Begin(rgb(0x202020));

    // ---- row 1: shapes ----------------------------------------------------
    Rect(20, 20, 60, 40, red);
    Rect(90.5f, 20.5f, 60, 40, blue); // edges on half pixels
    RRect(160, 20, 80, 40, 10, white);
    RRect(250, 20, 80, 40, 999, white); // pill
    RRect(340, 20, 40, 40, 999, white); // circle
    RRectStroke(390, 20, 80, 40, 10, ink, 2);
    RRectStroke(480, 20, 80, 40, 6, ink, 6);
    static const Color stops[] = {rgb(0x8F73DA), rgb(0xEFACF9), rgb(0x5785DD), rgb(0x8FD0F7), rgb(0x8F73DA)};
    RRectStrokeGrad(570, 20, 100, 40, 8, stops, 5, 30, 4, 1.0f);
    RRectStrokeGrad(680, 20, 40, 40, 999, stops, 5, 120, 5, 0.6f);
    Circle(750, 40, 20, red);
    CircleStroke(800, 40, 18, ink, 3);
    Line(830, 20, 890, 60, ink, 1);
    Line(840, 20, 900, 60, ink, 4);
    Line(910, 40, 970, 40, blue, 8);
    for (int i = 1; i <= 3; i++) Arc(1010, 60, i * 9.0f, 225, 315, ink, 3);
    Arc(1080, 40, 18, 0, 270, red, 4);
    Arc(1130, 40, 18, 300, 60, blue, 6);
    Triangle(1170, 60, 1200, 20, 1230, 60, white);

    // ---- row 2: opacity, layers, clips, transforms ------------------------
    Rect(20, 80, 60, 40, rgb(0xFFFFFF, 0.5f));
    Rect(50, 95, 60, 40, rgb(0xE60012, 0.5f));
    PushLayer(0.5f); // both together at half strength: no darker overlap
    Rect(130, 80, 60, 40, white);
    Rect(160, 95, 60, 40, red);
    PopLayer();
    PushClip(240.3f, 80.3f, 50.4f, 50.4f);
    Circle(270, 110, 34, blue);
    PushClip(250, 90, 60, 20); // nested: the intersection
    Rect(230, 70, 100, 100, rgb(0xFFFFFF, 0.7f));
    PopClip();
    PopClip();
    PushScale(1.5f, 350, 110);
    RRect(335, 95, 30, 30, 6, red);
    PopTransform();
    PushRotate(30, 430, 110);
    Rect(405, 95, 50, 30, blue);
    Text(L"Giro", 405, 95, 50, 30, 16, white, Center);
    PopTransform();
    PushOffset(100, 10);
    PushScale(0.5f, 400, 100);
    Rect(380, 80, 40, 40, white); // nested transforms
    PopTransform();
    PopTransform();
    PushLayer(0.7f);
    PushClip(560, 80, 80, 50);
    PushRotate(-15, 600, 105);
    RRect(550, 85, 100, 40, 12, red);
    PopTransform();
    PopClip();
    PopLayer();
    RRectStroke(660.5f, 80.5f, 60, 50, 4, ink, 1); // hairline on pixel centres
    Rect(730, 80, 1, 50, ink);
    Rect(735, 80, 0.5f, 50, ink);
    Rect(740, 80.25f, 20, 0.75f, ink);

    // ---- row 3: images ----------------------------------------------------
    if (haveIcons) {
        static Img icon, wide, pack, numeral;
        icon.path = DataDir() + L"\\icons\\01007ef00011e000.jpg";
        wide.path = icon.path;
        wide.maxSize = 96; // decoded small, drawn large
        pack.path = DataDir() + L"\\pack\\wifi_d.png";
        numeral.path = DataDir() + L"\\pack\\sys_8_d.png";
        static Img icon2;
        icon2.path = icon.path;
        Image(icon, 20, 150, 120, 120);
        Image(icon2, 150, 150, 160, 80);          // cover: crops top and bottom
        Image(icon, 320, 150, 80, 120);           // cover: crops the sides
        Image(icon, 410, 150, 120, 120, 1, 0, 24); // rounded
        Image(icon, 540, 150, 120, 120, 1, 1);     // circle
        Image(icon, 670, 150, 120, 120, 0.4f);     // translucent
        Image(wide, 800, 150, 120, 120);
        Image(pack, 930, 150, 40, 30);
        Image(pack, 980, 150, 120, 90);
        Image(numeral, 1110, 150, 64, 64);
        PushRotate(10, 1220, 210);
        Image(icon, 1180, 170, 80, 80, 1, 0, 12);
        PopTransform();
    }

    // ---- text -------------------------------------------------------------
    const std::wstring sample = L"Hamburgefonstiv \u00c1\u00c9\u00f1 0123";
    float y = 290;
    for (float size : {12.0f, 16.0f, 21.0f, 26.0f, 40.0f}) {
        float h = size * 1.6f;
        Guide(20, y, 600, h);
        Text(sample, 20, y, 600, h, size, ink);
        y += h + 6;
    }
    // Weights.
    Guide(640, 290, 300, 34);
    Text(L"Normal 400 Confirmar", 640, 290, 300, 34, 21, ink, Left, 400);
    Guide(640, 330, 300, 34);
    Text(L"Seminegra 600 Confirmar", 640, 330, 300, 34, 21, ink, Left, 600);
    Guide(640, 370, 300, 34);
    Text(L"Negrita 700 Confirmar", 640, 370, 300, 34, 21, ink, Left, 700);
    // Alignment.
    Guide(960, 290, 300, 34);
    Text(L"Izquierda", 960, 290, 300, 34, 21, ink, Left);
    Guide(960, 330, 300, 34);
    Text(L"Centro", 960, 330, 300, 34, 21, ink, Center);
    Guide(960, 370, 300, 34);
    Text(L"Derecha", 960, 370, 300, 34, 21, ink, Right);
    // Trimming.
    Guide(640, 410, 200, 34);
    Text(L"C:\\Emulators\\Ryujinx\\Ryujinx.exe", 640, 410, 200, 34, 21, blue, Right);
    Guide(860, 410, 140, 34);
    Text(L"The Legend of Zelda: Breath of the Wild", 860, 410, 140, 34, 21, blue);
    Guide(1020, 410, 240, 34);
    Text(L"Super Mario Party Jamboree", 1020, 410, 240, 34, 21, blue, Center);
    // Spaced digits (the status bar), tight and wide.
    Guide(640, 450, 140, 40);
    TextSpaced(L"06:49", 640, 450, 140, 40, 26, ink, 1.7f);
    Guide(800, 450, 140, 40);
    TextSpaced(L"100", 800, 450, 140, 40, 26, ink, -1.0f);
    Guide(960, 450, 300, 40);
    TextSpaced(L"ESPACIADO", 960, 450, 300, 40, 18, ink, 6.0f, Center);
    // Wrapped paragraphs.
    const std::wstring para = L"\u00bfQuieres cerrar este programa?\nSe perder\u00e1n los datos que no hayas guardado. "
                              L"Comprueba el emulador en Configuraci\u00f3n de la consola.";
    Guide(20, 520, 300, 150);
    TextWrap(para, 20, 520, 300, 150, 18, ink);
    Guide(340, 520, 300, 150);
    TextWrap(para, 340, 520, 300, 150, 18, ink, Center);
    Guide(660, 520, 200, 60);
    TextWrap(para, 660, 520, 200, 60, 16, ink, Right); // clipped at the box
    Guide(880, 520, 180, 150);
    TextWrap(L"Supercalifragilisticoespialidoso y palabrascortas", 880, 520, 180, 150, 18, ink);
    // Other scripts (font fallback) and translucent text.
    Guide(660, 600, 200, 34);
    Text(L"\u5996\u602a\u30a6\u30a9\u30c3\u30c1 \u2713 \u2026", 660, 600, 200, 34, 21, ink);
    Guide(660, 640, 200, 34);
    Text(L"Transl\u00facido", 660, 640, 200, 34, 24, rgb(0xFFFFFF, 0.5f), Left, 600);
    // Button glyphs from the symbol font, on crosshairs.
    if (HasSymbolFont()) {
        float x = 1100;
        for (wchar_t g : {wchar_t(0xE0E0), wchar_t(0xE0E1), wchar_t(0xE0E2), wchar_t(0xE0E3), wchar_t(0xE0EF), wchar_t(0xE0F0)}) {
            Line(x - 14, 540, x + 14, 540, rgb(0x00FF00, 0.6f), 0.5f);
            Line(x, 526, x, 554, rgb(0x00FF00, 0.6f), 0.5f);
            Symbol(g, x, 540, 24, ink);
            x += 32;
        }
        Line(1086, 610, 1280, 610, rgb(0x00FF00, 0.6f), 0.5f);
        Symbol(0xE0E0, 1120, 610, 48, ink);
        Symbol(0xE0E1, 1200, 610, 64, blue);
    }
    // Scaled text keeps its outline (the press and zoom animations).
    PushScale(2.0f, 100, 695);
    Text(L"Zoom x2", 60, 685, 80, 20, 10, ink, Center);
    PopTransform();
    End();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        puts("gfxprobe <console|system> <card.png> <metrics.txt>");
        return 2;
    }
    const bool console = std::string(argv[1]) == "console";
    if (!Init()) return 1;
    bool consoleFont = false;
    if (console) {
        std::wstring fw = FirmwareDir(), keys = KeysDir();
        fwfont::Font font, symbols;
        std::string err;
        if (!fw.empty() && !keys.empty() && fwfont::Load(fw, keys, 0, font, err) && SetFont(std::move(font.data))) {
            SetTextScale(0.913f);
            SetTextEmbolden(0.0105f);
            if (fwfont::Load(fw, keys, 1, symbols, err)) SetSymbolFont(std::move(symbols.data));
            consoleFont = true;
        } else {
            printf("console font not available (%s)\n", err.c_str());
        }
    }
    if (!AttachOffscreen(1920, 1080)) return 1;
    Img probe;
    probe.path = DataDir() + L"\\icons\\01007ef00011e000.jpg";
    FILE* exists = nullptr;
    bool haveIcons = _wfopen_s(&exists, probe.path.c_str(), L"rb") == 0 && exists;
    if (exists) fclose(exists);
    Card(haveIcons);
    Card(haveIcons); // twice: the second pass goes through the caches
    bool saved = SaveOffscreenPng(FromArg(argv[2]));
    // Profile pictures, next to the card: a letter on a colour, and a cropped image.
    std::wstring base = FromArg(argv[2]);
    bool avatars = MakeAvatar(L"", rgb(0x0AB9E6), L"Ñ", base + L".letter.jpg");
    if (haveIcons) avatars = MakeAvatar(probe.path, rgb(0x000000), L"", base + L".image.jpg") && avatars;
    Card(haveIcons); // and the main target still draws after the detour
    saved = SaveOffscreenPng(FromArg(argv[2])) && saved && avatars;

    FILE* f = nullptr;
    if (_wfopen_s(&f, FromArg(argv[3]).c_str(), L"wb") != 0 || !f) return 1;
    fprintf(f, "font %s symbol %d\n", consoleFont ? "console" : "system", HasSymbolFont() ? 1 : 0);
    const wchar_t* strings[] = {L"Opciones",   L"Confirmar",   L"Atr\u00e1s", L"Configuraci\u00f3n de la consola",
                                L"The Legend of Zelda: Breath of the Wild", L"06:49", L"100", L"%", L"AVATAR To We LT",
                                L"Hamburgefonstiv \u00c1\u00c9\u00f1 0123", L"\u2026", L" ", L"a b", L"iiii", L"MMMM",
                                L"\u5996\u602a\u30a6\u30a9\u30c3\u30c1"};
    for (const wchar_t* s : strings)
        for (float size : {15.0f, 21.0f, 26.0f, 40.0f})
            for (int weight : {400, 600, 700})
                fprintf(f, "width|%s|%g|%d|%.3f\n", Narrow(s).c_str(), size, weight, TextWidth(s, size, weight));
    for (const wchar_t* s : {L"06:49", L"100", L"ESPACIADO"})
        for (float spacing : {1.7f, -1.0f, 6.0f}) fprintf(f, "spaced|%s|26|%g|%.3f\n", Narrow(s).c_str(), spacing, TextSpacedWidth(s, 26, spacing));
    fclose(f);
    Shutdown();
    return saved ? 0 : 3;
}
