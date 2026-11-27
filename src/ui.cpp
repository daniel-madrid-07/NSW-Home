// Screens and drawing. Everything is immediate mode: each frame the current
// screen draws itself and registers its clickable regions.
#include "app.h"
#include "a11y.h"
#include "amiibo.h"
#include "console_menus.h"
#include "content.h"
#include "emudata.h"
#include "friends.h"
#include "fwsound.h"
#include "lang.h"
#include "netplay.h"
#include "padcfg.h"
#include "padsys.h"
#include "padwish.h"
#include "pchost.h"
#include "parental.h"
#include "pcsys.h"
#include "ryucfg.h"
#include "update.h"
#include "webview.h"
#include "yuzucfg.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <thread>

using namespace gfx;

namespace ui {
namespace {

// [android] True in the Android build. What differs there is what the pages
// can offer, not how they look: emulators are installed apps (no folder of
// theirs to point at, no settings or profiles of theirs to read or change),
// and there is no file manager to show a file in. The few places that differ
// ask this.
bool OnAndroid() {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

// ---- theme ----------------------------------------------------------------

struct Theme {
    Color bg, text, sub, line, title, accent, tile, btn, icon, dialog, sep, shadow;
    Color panel;   // system-button bar
    Color focusBg; // fill behind the focused list row
    Color avRing;  // ring behind profile pictures
    Color allMark = {}; // second console: the mark on the round "all software" button
};

// Accessibility (Settings > Accesibilidad; the block above AddConsoleRow):
// what the rest of the menu asks of it.
bool A11yContrast();                                 // the high-contrast palette is wanted
Theme A11yTheme();
void A11yCursor(float x, float y, float w, float h); // the focused element's frame, where it is drawn
void A11yTitle(const std::wstring& title);           // the page's heading, as it is drawn
int A11yTextEpoch();                                 // changes whenever text starts to measure differently

// The second console's menu is a different design, not a recolour: it has its
// own palette, shapes and layout.
bool S2() {
    return cfg.menuStyle == 2;
}

// The second console's colours are read off the owner's captures as PNG frames
// (never through JPEG: an earlier, lossy route gave the dark theme a green
// cast and the light theme 8 levels too much). tools\palette_check.py holds
// the patches and prints console against NSW-Home for each; where and when
// each value was read is in its list. What the console shows:
//   - both themes use one colour for every rule (header, footer, the column
//     divider, the rows), so `line` and `sep` are the same there;
//   - names and values share one blue (`title` = `accent`);
//   - `dialog` is the ground of everything laid over the menu: dialogs, the
//     user sheet, the options panel. In the dark theme it equals the bar's
//     `panel`; in the light theme the bar is lighter than the sheet.
//   - `tile` is the cell a picture has not arrived in yet (software grid and
//     album, dark theme), `btn` the round "all software" button, `icon` the
//     grey of the bar's pictures. The mark on the round button is paler than
//     those in the dark theme, hence `allMark`.
// Not in any capture, and derived: the light theme's `tile` (as far below the
// page as the old value was), `btn` (the dark theme's equals its picture
// ring, so the light one does too) and `allMark` (the bar's grey).
Theme Th() {
    if (A11yContrast()) return A11yTheme();
    if (S2()) {
        if (cfg.dark)
            return {rgb(0x1F1F1F), rgb(0xD2CFD3), rgb(0xA4A3A4), rgb(0x656565), rgb(0x28BCEF), rgb(0x28BCEF),
                    rgb(0x4C4C4C), rgb(0x424242), rgb(0xA4A3AB), rgb(0x2E2E2E), rgb(0x656565), rgb(0x000000, 0.0f),
                    rgb(0x2E2E2E), rgb(0x3B3B3B), rgb(0x424242), rgb(0xB3B2B9)};
        return {rgb(0xDCD9DD), rgb(0x161616), rgb(0x646369), rgb(0x979598), rgb(0x0C5FCA), rgb(0x0C5FCA),
                rgb(0xC8C5C9), rgb(0xEFECF0), rgb(0x78757E), rgb(0xE0DDE1), rgb(0x979598), rgb(0x000000, 0.0f),
                rgb(0xE6E2E6), rgb(0xE9E6EA), rgb(0xEFECF0), rgb(0x78757E)};
    }
    if (cfg.dark)
        return {rgb(0x2D2D2D), rgb(0xFFFFFF), rgb(0xA0A0A0), rgb(0xFFFFFF), rgb(0x1FD8F2), rgb(0x00FFC9),
                rgb(0x484848), rgb(0x505050), rgb(0xE6E6E6), rgb(0x464646), rgb(0x4D4D4D), rgb(0x000000, 0.25f),
                rgb(0x505050), rgb(0x000000, 0.0f), rgb(0x000000, 0.0f)};
    return {rgb(0xEBEBEB), rgb(0x2D2D2D), rgb(0x6E6E6E), rgb(0x2D2D2D), rgb(0x0FAFCB), rgb(0x3250F0),
            rgb(0xD4D4D4), rgb(0xFFFFFF), rgb(0x5A5A5A), rgb(0xF0F0F0), rgb(0xCDCDCD), rgb(0x000000, 0.13f),
            rgb(0xFFFFFF), rgb(0x000000, 0.0f), rgb(0x000000, 0.0f)};
}

// ---- state ----------------------------------------------------------------

enum class Scr { Home, All, Settings, Options, Album, Viewer, Pads, User, Who, Launch, Sleep, Setup, Page, Lock, Extra, PadTest, PadCal, Remap, Frame, Web, Amiibo, Netplay };

std::vector<Scr> stack = {Scr::Home};
double now = 0;
double activeUntil = 0;
double lastMouseMove = -10;
float fade = 0; // 1 right after a screen change, decays to 0

// Home
int homeRow = 1; // 0 user, 1 games, 2 system buttons
int gi = 0, si = 0;
float homeScroll = 0;
double pressAt = -1; // start of the tile "press" animation before a launch
std::string pressId;
bool selectFirstAfterScan = false;

// All software
int allSel = 0;
float allScroll = 0;

// List pages (settings, game options)
// What a row is, as the console draws them (walkthrough.mp4 123.6-181.2 s):
//   Plain    a label, optionally a value at the right end; A runs `act`
//   Toggle   a label and a switch
//   Radio    one of several exclusive choices: a colour sample, a radio button
//   Slider   a label over a track with a knob; left and right call `step`
//   Text     a paragraph (`on`: alone on the page, centred)
//   Panel    a box with a picture (`icon` 1: help, 2: current audio devices)
//   Table    a line of a striped status table (`on`: its title line)
//   Gap      the blank space between groups of rows
//   Storage  the two disk-space bars with their legend
//   Users    a line of up to four user tiles (`cells`), A calls `step(tile)`
enum class RK { Plain, Toggle, Radio, Slider, Text, Panel, Table, Gap, Storage, Users };
struct Row {
    std::wstring label, value;
    std::function<void()> act;
    bool heading = false;
    RK kind = RK::Plain;
    bool on = false;
    std::wstring note;   // grey description under the row
    bool disabled = false;
    bool real = false;   // does something on a PC: kept when identical mode is off
    float pos = 0;       // Slider: 0..1
    int icon = 0;        // Slider: 1 loudspeaker, 2 speed, 3 microphone; Radio: 1 white, 2 black; 9: a video still
    std::function<void(int)> step;
    std::vector<int> cells;
};
struct Cat {
    std::wstring name;
    std::function<std::vector<Row>()> rows;
    bool ruleAfter = false; // a rule under it in the left column
    std::string id;
};
struct ListPage {
    std::wstring title;
    int icon = 5;
    std::vector<Cat> cats;
    int cat = 0, row = 0;
    bool inRows = false;
    float scroll = 0;
    float catScroll = -1;        // left column; -1 = place it without moving
    int sub = 0;                 // the tile within a line of user tiles
    float over = -1, overFrom = 0; // reading on below the last row: where the page has been moved to
};
ListPage settings, options;
std::string optionsGame;

// Album
std::vector<Img> shots;
int shotSel = 0;
float shotScroll = 0;

// Launch
std::string launchId;
double launchAt = 0;

// Dialog
struct Dialog {
    bool open = false;
    std::wstring msg;
    std::vector<std::wstring> btns;
    std::function<void(int)> done;
    int sel = 0;
    float anim = 0;
    std::string iconGame; // show this game's picture beside the text
    // Second console's menu: the dialog runs on a timeline instead of `anim`.
    double openAt = 0;    // when the menu starts to darken (after the pressed button's own reaction)
    double pressAt = -1;  // a choice was made at this time; the dialog is on its way out
    bool tinted = false;  // made with A: the chosen button shows its pressed colour for two frames first
    int result = 0;       // the choice, handed to `done` once the box is gone
    int lastSel = -1;     // the button the focus just left, whose fill is still fading
    double selAt = -1;
    bool busy = false;    // a waiting box: a spinner under the text
} dlg;

// Text entry
struct TextEntry {
    bool open = false;
    std::wstring title, text;
    std::function<void(const std::wstring&)> done;
    size_t max = 10; // longest text taken: a nickname's unless the opener says otherwise; back to this once closed
    // What a game's software keyboard asks for besides (OnEmulatorText); all
    // of it goes back to these values once the entry is closed, as `max` does.
    std::wstring sub, guide;      // the game's second line; its grey hint while the box is empty
    size_t min = 0;               // shortest text Enter takes
    bool numeric = false;         // digits only
    bool counter = false;         // "3/12" at the end of the box
    bool pad = false;             // A and B answer as well: a game is waiting, likely with a controller in hand
    std::function<void()> cancel; // told when the entry is backed out of with Esc or B
    void Plain() {
        max = 10;
        sub.clear();
        guide.clear();
        min = 0;
        numeric = counter = pad = false;
        cancel = nullptr;
    }
} entry;

// Profile pictures, decoded on demand and keyed by user id.
std::map<std::string, Img> avatars;
int homeUser = 0; // which avatar the cursor is on (home row 0)
int pageUser = 0; // whose page is open
int whoSel = 0;   // "who is playing?" selection
std::string whoGame;
extern double whoAt;
// The user page and the "new user" steps, defined with the page below.
void OpenUserPage(int user);
void StartAddUser(std::function<void(int newIndex)> done);
// The profile picture's framing page, before a picture file becomes the
// user's picture (defined after the "new user" steps).
bool OpenAvatarFrame(int user, const std::wstring& name, const std::wstring& file, const PicFrame& start,
                     std::function<void(const PicFrame&)> save, std::function<void()> cancel);
bool UserReframe(int u);
bool HomePlusShown();

// A user may have no nickname. Where the console prints just the name, nothing
// is printed; where a sentence or a title needs a word for the user, these.
std::wstring PageOf(int user) {
    const std::wstring& name = cfg.users[user].name;
    return name.empty() ? std::wstring(L"Tu página") : L"Página de " + name;
}
std::wstring UserLabel(int user) { // alone, as a title or a row's label
    return cfg.users[user].name.empty() ? std::wstring(L"Usuario") : cfg.users[user].name;
}
std::wstring UserNoun(int user) { // after "a": "¿Eliminar a ...?"
    return cfg.users[user].name.empty() ? std::wstring(L"este usuario") : cfg.users[user].name;
}
void FriendsMarkUser(int user, float cx, float cy, float r); // ui_friends.inc: requests waiting
// The friend code as it is shown: NH-1234-5678-9012.
std::wstring FriendCode(int user) {
    const std::wstring d = Widen(cfg.users[user].friendCode);
    return d.size() != 12 ? std::wstring(L"—") : L"NH-" + d.substr(0, 4) + L"-" + d.substr(4, 4) + L"-" + d.substr(8);
}

void DrawAvatar(int user, float cx, float cy, float r) {
    const User& u = cfg.users[user];
    Img& im = avatars[u.id];
    if (im.path.empty()) {
        im.path = AvatarPath(u.id);
        im.maxSize = 256;
    }
    Image(im, cx - r, cy - r, r * 2, r * 2, 1, 1);
    if (im.failed) Circle(cx, cy, r, rgb(0x8C8C8C));
}

// Optional local icon pack (%APPDATA%\NSW-Home\pack\<name>.png), made by the
// owner from their own capture with tools\make_icon_pack.py. Returns nullptr
// when the pack has no such image, and the caller draws the built-in icon.
Img* PackIcon(const std::wstring& name) {
    static std::map<std::wstring, Img> pack;
    auto it = pack.find(name);
    if (it == pack.end()) {
        Img im;
        std::wstring path = DataDir() + L"\\pack\\" + name + L".png";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) im.path = path;
        it = pack.emplace(name, std::move(im)).first;
    }
    return it->second.empty() || it->second.failed ? nullptr : &it->second;
}

void RefreshAvatar(int user) {
    avatars[cfg.users[user].id].reset();
}

// Clickable regions, rebuilt every frame.
struct Hit {
    float x, y, w, h;
    std::function<void()> click;
    int scroll = -1;  // a list that a press here can drag (an index into `scrollers`, see the mouse block below)
    bool bar = false; // that list's scroll bar
};
std::vector<Hit> hits;

void AddHit(float x, float y, float w, float h, std::function<void()> click) {
    hits.push_back({x, y, w, h, std::move(click)});
}

void Touch() {
    activeUntil = now + 1.0;
}

void Smooth(float& v, float target, float dt, float speed = 0.0f) {
    // Default pace: the second console's menu settles in about 100 ms.
    if (speed <= 0.0f) speed = cfg.menuStyle == 2 ? 30.0f : 16.0f;
    v += (target - v) * (1.0f - expf(-speed * dt));
    if (fabsf(target - v) < 0.05f) v = target;
}

// ---- mouse: dragging lists and their scroll bars ---------------------------
// A page that scrolls registers, while it draws, the area its list can be
// dragged in (AddScroll, before the list's own clickable rows, which stay on
// top for clicks) and, where it draws one, its scroll bar (AddScrollBar). A
// press on a list waits: once the pointer has moved a few units the list
// follows it, as on a touch screen, and coasts a little when let go; a press
// that never moved that far is a click on what is under it, on release.
// While the mouse moves a list, MouseOwns(&position) is true and the page
// leaves the position alone instead of bringing it to its focus; the
// scroller's `set` moves the focus into what is shown, so that when the mouse
// lets go the page settles the way it does after a stick move.
struct Scroller {
    const void* key = nullptr; // the page's scroll position: the same from frame to frame
    bool horiz = false;
    float lo = 0, hi = 0;      // the range of the position
    std::function<float()> get;
    std::function<void(float)> set; // the list to there, and the focus into what is then shown
    // The scroll bar, along the list's axis: where its track starts, its
    // length, the thumb's length, and the position that puts the thumb at the end.
    float barA = 0, barLen = 0, thumb = 0, barMax = 1;
};
std::vector<Scroller> scrollers; // this frame's, cleared with `hits`

// The usual case: a position kept in a float.
Scroller ScrollOf(float& pos, float hi, std::function<void(float)> follow, bool horiz = false) {
    Scroller s;
    s.key = &pos;
    s.horiz = horiz;
    s.hi = (std::max)(0.0f, hi);
    s.get = [&pos] { return pos; };
    s.set = [&pos, follow](float v) {
        pos = v;
        if (follow) follow(v);
    };
    return s;
}

int AddScroll(float x, float y, float w, float h, Scroller s) {
    if (s.hi <= s.lo) return -1; // it all fits: nothing to drag
    scrollers.push_back(std::move(s));
    Hit hit{x, y, w, h, nullptr};
    hit.scroll = static_cast<int>(scrollers.size()) - 1;
    hits.push_back(std::move(hit));
    return static_cast<int>(scrollers.size()) - 1;
}

// The bar of list `s` (AddScroll's result), standing at x (a vertical list's;
// the only kind that has one). It is 4 units wide: the area that takes the
// pointer reaches 14 units to each side of it, which still leaves it clear of the rows.
void AddScrollBar(int s, float x, float barA, float barLen, float thumb, float barMax) {
    if (s < 0) return;
    Scroller& sc = scrollers[s];
    sc.barA = barA;
    sc.barLen = barLen;
    sc.thumb = thumb;
    sc.barMax = (std::max)(1.0f, barMax);
    Hit hit{x - 14, barA - 6, 32, barLen + 12, nullptr};
    hit.scroll = s;
    hit.bar = true;
    hits.push_back(std::move(hit));
}

// The item nearest `cur` for which `fits` holds (what the focus moves to when
// the list moves it off screen); -1 when none does.
int NearestFit(int cur, int count, const std::function<bool(int)>& fits) {
    if (count <= 0) return -1;
    if (cur >= 0 && cur < count && fits(cur)) return cur;
    const int c = (std::max)(0, (std::min)(cur, count - 1));
    for (int d = 0; d <= count; d++) {
        if (c - d >= 0 && fits(c - d)) return c - d;
        if (c + d < count && fits(c + d)) return c + d;
    }
    return -1;
}

enum class MDrag { None, Press, Drag, Thumb, Coast };
struct MouseDrag {
    bool down = false;
    MDrag mode = MDrag::None;
    float x0 = 0, y0 = 0;         // the press
    std::function<void()> click;  // Press: what a click there does, run on release
    Scroller s;                   // the list in hand (refreshed every frame it is drawn)
    float from = 0;               // its position at the press
    float pos = 0, last = 0;      // the position last asked of it; on the frame before
    float grab = 0;               // Thumb: where along the thumb it was taken
    float vel = 0;                // units a second
    bool ending = false;          // Coast: the frame being drawn is its last
} mdrag;

// A press that moves less than this, in units, is a click. The mouse: about
// nine pixels of a full-HD screen. A finger is less precise: Android's
// front end used 14 for its taps before the menu took drags itself.
float DragSlop() {
    return OnAndroid() ? 14.0f : 6.0f;
}

bool MouseOwns(const void* key) {
    return mdrag.s.key == key && (mdrag.mode == MDrag::Drag || mdrag.mode == MDrag::Thumb || mdrag.mode == MDrag::Coast);
}

void MouseSetList(float v) {
    v = (std::max)(mdrag.s.lo, (std::min)(mdrag.s.hi, v));
    mdrag.pos = v;
    if (mdrag.s.set) mdrag.s.set(v);
    Touch();
}

float ThumbStart(const Scroller& s) {
    return s.barA + (std::max)(0.0f, (std::min)(1.0f, s.get() / s.barMax)) * (s.barLen - s.thumb);
}

void MouseThumb(float along) {
    const Scroller& s = mdrag.s;
    const float room = s.barLen - s.thumb;
    if (room <= 0) return;
    MouseSetList((std::max)(0.0f, (std::min)(1.0f, (along - mdrag.grab - s.barA) / room)) * s.barMax);
}

void MousePress(float x, float y) {
    // A press that stops a list still coasting does only that.
    const bool caught = mdrag.mode == MDrag::Coast && fabsf(mdrag.vel) > 150;
    mdrag = MouseDrag();
    mdrag.down = true;
    mdrag.x0 = x;
    mdrag.y0 = y;
    std::function<void()> click;
    bool onClick = false;
    // Later regions are drawn on top, so they win; a list lies under its rows.
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        if (!(x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h)) continue;
        if (it->scroll < 0) {
            if (!onClick) click = it->click;
            onClick = true;
            continue;
        }
        if (it->bar && onClick) continue;
        mdrag.s = scrollers[it->scroll];
        mdrag.from = mdrag.pos = mdrag.last = mdrag.s.get();
        if (it->bar) {
            // On the thumb it is taken where it was pressed; on the track it
            // jumps under the pointer, and from there it is held the same way.
            mdrag.mode = MDrag::Thumb;
            const float along = mdrag.s.horiz ? x : y, start = ThumbStart(mdrag.s);
            const bool on = along >= start && along < start + mdrag.s.thumb;
            mdrag.grab = on ? along - start : mdrag.s.thumb / 2;
            if (!on) MouseThumb(along);
        } else {
            mdrag.mode = MDrag::Press;
            if (!caught) mdrag.click = click;
        }
        Touch();
        return;
    }
    if (!onClick || !click) return;
    if (OnAndroid()) { // a tap acts when the finger is lifted without having slid away
        mdrag.mode = MDrag::Press;
        mdrag.click = click;
        return;
    }
    click(); // elsewhere a click acts on the press, as it always has
}

void MouseMove(float x, float y) {
    if (!mdrag.down) return;
    if (mdrag.mode == MDrag::Press) {
        if (fabsf(x - mdrag.x0) < DragSlop() && fabsf(y - mdrag.y0) < DragSlop()) return;
        mdrag.click = nullptr;
        mdrag.mode = mdrag.s.key ? MDrag::Drag : MDrag::None;
    }
    // The list keeps the point that was pressed under the pointer.
    if (mdrag.mode == MDrag::Drag) MouseSetList(mdrag.from - (mdrag.s.horiz ? x - mdrag.x0 : y - mdrag.y0));
    else if (mdrag.mode == MDrag::Thumb) MouseThumb(mdrag.s.horiz ? x : y);
}

void MouseRelease(float x, float y) {
    if (!mdrag.down) return;
    mdrag.down = false;
    if (mdrag.mode == MDrag::Press) {
        auto fn = std::move(mdrag.click);
        mdrag.click = nullptr;
        mdrag.mode = MDrag::None;
        if (fn && fabsf(x - mdrag.x0) < DragSlop() && fabsf(y - mdrag.y0) < DragSlop()) fn();
    } else if (mdrag.mode == MDrag::Drag || mdrag.mode == MDrag::Thumb) {
        // Let go while still moving, the list carries on a little; either way
        // it stays the mouse's until the next frame has been drawn where it
        // was left (MouseFrame, MouseCheck), or the page would glide back to
        // where it saw it last.
        if (mdrag.mode == MDrag::Thumb || fabsf(mdrag.vel) <= 90) mdrag.vel = 0;
        mdrag.mode = MDrag::Coast;
    }
}

// A button or the wheel takes over from the mouse.
void MouseStop() {
    if (mdrag.mode != MDrag::Press) mdrag.mode = MDrag::None;
}

// Once a frame, before the screen is drawn: the speed of a list in hand, and
// the coast of one let go. The coast loses about 11 % of its speed a frame
// (e^-7 a second): a fast throw of 2000 units a second travels some 280 more.
void MouseFrame(float dt) {
    if (dt <= 0) return;
    if (mdrag.mode == MDrag::Drag || mdrag.mode == MDrag::Thumb) {
        mdrag.vel = (std::max)(-4000.0f, (std::min)(4000.0f, 0.4f * mdrag.vel + 0.6f * (mdrag.pos - mdrag.last) / dt));
        mdrag.last = mdrag.pos;
    } else if (mdrag.mode == MDrag::Coast) {
        const float want = mdrag.pos + mdrag.vel * dt;
        MouseSetList(want);
        mdrag.last = mdrag.pos;
        mdrag.vel *= expf(-7.0f * dt);
        if (mdrag.pos != want || fabsf(mdrag.vel) < 25) mdrag.ending = true; // at an end, or spent: this frame is its last
    }
}

// Once a frame, after the screen is drawn: the list in hand as the page has
// just laid it out, or the end of the drag when it is no longer there to
// take the pointer (another screen, a dialog over it).
void MouseCheck() {
    if (mdrag.ending) mdrag.mode = MDrag::None;
    mdrag.ending = false;
    if (mdrag.mode == MDrag::None || !mdrag.s.key) return;
    for (const Hit& h : hits)
        if (h.scroll >= 0 && !h.bar && scrollers[h.scroll].key == mdrag.s.key) {
            mdrag.s = scrollers[h.scroll];
            return;
        }
    mdrag.mode = MDrag::None;
    mdrag.click = nullptr;
}

// ---- the console's pixel grid ------------------------------------------------
// The second console draws its menu at 1920x1080, which is 1.5 pixels to a
// layout unit, and everything thin on it (rules, bars, rings) is a whole
// number of those pixels thick and sits on whole pixel rows and columns.
// Measured there, a position is "pixel row 127"; Px() turns that into layout
// units and Snap() moves a computed position (a row of a scrolled list) onto
// the grid, so that nothing thin is ever spread over two rows as a 1-unit line
// centred on a half unit is. The console cuts fractions off rather than
// rounding them (its section list, 122 1/3 px a step, puts its marks on rows
// 178, 301, 423, 545; the rows of a game's options, 112.5 px each, on 475,
// 587, 700), so Snap() does too. The grid belongs to the layout, not to the
// window: at other window sizes the same fractions of a unit are scaled.
constexpr float Px(float pixels) {
    return pixels / 1.5f;
}
float Snap(float units) {
    return floorf(units * 1.5f + 0.01f) / 1.5f; // (the 0.01 keeps a whole pixel whole through float rounding)
}
// A console hairline: one pixel (2/3 unit) thick, on the pixel row y falls
// in, from x0 to x1 (both taken to the grid as well).
void HRule(float x0, float x1, float y, Color c) {
    x0 = Snap(x0);
    Rect(x0, Snap(y), Snap(x1) - x0, Px(1), c);
}
// The same standing up, on the pixel column x falls in.
void VRule(float x, float y0, float y1, Color c) {
    y0 = Snap(y0);
    Rect(Snap(x), y0, Px(1), Snap(y1) - y0, c);
}

Color CursorColor() {
    float p = 0.5f + 0.5f * sinf(static_cast<float>(now) * 4.8f);
    return mix(rgb(0x00C4DE), rgb(0x86F5E4), p);
}

void PushNow(Scr s) {
    stack.push_back(s);
    fade = cfg.menuStyle == 2 ? 0.0f : 1.0f;
    Touch();
}
void PopNow() {
    if (stack.size() > 1) stack.pop_back();
    fade = cfg.menuStyle == 2 ? 0.0f : 1.0f;
    Touch();
}

// On the second console a screen change is a short sequence, not a cut. Read
// frame by frame from the capture (anim.mkv 256-281 s, every bar item opened
// and closed; 60 frames a second, frame 0 = the first frame showing the press):
//   opening from HOME: the pressed button reacts; from frame 21 to 30 the menu
//             fades out, slowly at first (covered = u*u), and from frame 23 it
//             also shrinks about the screen centre by 0.52 % a frame down to
//             97.2 %. The screen then stays empty (footer only) for a time
//             that depends on the page: until frame 52 for Settings, 45 for
//             Controllers, 47 for the Album, 44 for the game-cards sheet. The
//             page fades in over 8 frames, fast at first, without scaling.
//   going back: the page's cursor goes at once and nothing else moves for 12.4
//             frames; the page fades out evenly over 7.2 frames; one frame
//             empty; from frame 21 the menu fades in over 7 frames (fast at
//             first) while it grows from 97.5 % to full size over 9.5 frames.
// The footer stays put throughout.
struct Transition {
    bool on = false;
    bool applied = false;
    bool quiet = false;   // only a delay before `apply`: nothing fades, the screen stays
    bool homeOut = false; // the HOME menu is the screen leaving
    bool homeIn = false;  // the HOME menu is the screen arriving
    double t0 = 0;
    float outAt = 0, outDur = 0, blank = 0, inDur = 0;
    std::function<void()> apply;
} trans;
float coverNow = 0; // this frame's TransitionCover(), for code that fades with the screen
double allShownAt = -1; // when the software grid was first drawn after being opened (-1: not yet)

void OnScreenSwitched(); // cursor and footer bookkeeping, defined with them below

constexpr float kFrame = 1.0f / 60.0f; // the capture's frame, the unit the timings were read in
// Input is handled between frames, with `now` still at the last frame drawn,
// and "frame 0" of every measured timeline is the first frame that shows the
// press. So a reaction is timed from the frame after the input.
double ReactionStart() {
    return now + kFrame;
}
constexpr float kHomeGrow = 9.5f * kFrame;

// A value read off the capture once per frame, at any time in between.
float FrameTable(const float* v, int n, float seconds) {
    float f = seconds * 60.0f;
    if (f <= 0) return v[0];
    if (f >= n - 1) return v[n - 1];
    int i = static_cast<int>(f);
    return v[i] + (v[i + 1] - v[i]) * (f - i);
}

void StartTransition(bool opening, std::function<void()> apply) {
    bool fromHome = stack.back() == Scr::Home;
    trans.on = true;
    trans.applied = false;
    trans.quiet = false;
    trans.homeOut = opening && fromHome;
    trans.homeIn = !opening && stack.size() == 2 && stack[0] == Scr::Home;
    trans.t0 = ReactionStart();
    if (opening) {
        trans.outAt = fromHome ? 21 * kFrame : 0.41f;
        trans.outDur = fromHome ? 9 * kFrame : 0.085f;
        trans.blank = 20 * kFrame; // Push() sets the measured time for the pages that have one
        trans.inDur = 8 * kFrame;
    } else {
        trans.outAt = 12.4f * kFrame;
        trans.outDur = 7.2f * kFrame;
        trans.blank = 1.4f * kFrame;
        trans.inDur = (trans.homeIn ? 7 : 8) * kFrame;
    }
    trans.apply = std::move(apply);
    Touch();
}

// Run `apply` after `delay` seconds with input held, the screen staying as it
// is: the console lets a pressed button play its reaction before the dialog
// it opens appears.
void StartDelay(float delay, std::function<void()> apply) {
    trans = Transition();
    trans.on = true;
    trans.quiet = true;
    trans.t0 = ReactionStart();
    trans.outAt = delay;
    trans.apply = std::move(apply);
    Touch();
}

// How much of the screen (above the footer) is covered by background colour.
float TransitionCover() {
    if (!trans.on) return 0;
    // (A hair over, so a step timed for frame n is not missed by rounding and taken on n + 1.)
    float t = static_cast<float>(now - trans.t0) + 0.0005f;
    float inAt = trans.outAt + trans.outDur + trans.blank;
    if (t >= inAt && !trans.applied) {
        trans.applied = true;
        if (trans.quiet) {
            trans.on = false;
            auto fn = std::move(trans.apply); // it may start another transition
            if (fn) fn();
            return 0;
        }
        if (trans.apply) trans.apply();
        OnScreenSwitched();
    }
    // The menu is still growing for a moment after it has faded in.
    if (t >= inAt + (trans.homeIn ? (std::max)(trans.inDur, kHomeGrow) : trans.inDur)) {
        trans.on = false;
        return 0;
    }
    if (t < trans.outAt) return 0;
    // Covered fraction frame by frame, measured (how much of the picture's
    // difference from the background is left, corrected for the scaling).
    static const float homeOut[] = {0, 0.02f, 0.05f, 0.11f, 0.20f, 0.32f, 0.474f, 0.67f, 0.883f, 1};
    static const float homeIn[] = {1, 0.65f, 0.445f, 0.29f, 0.17f, 0.095f, 0.033f, 0};
    static const float pageIn[] = {1, 0.82f, 0.555f, 0.31f, 0.185f, 0.085f, 0.05f, 0.015f, 0};
    if (t < trans.outAt + trans.outDur) {
        if (trans.homeOut) return FrameTable(homeOut, 10, t - trans.outAt);
        return (t - trans.outAt) / trans.outDur; // a page leaves evenly
    }
    if (t < inAt) return 1;
    if (t >= inAt + trans.inDur) return 0;
    return trans.homeIn ? FrameTable(homeIn, 8, t - inAt) : FrameTable(pageIn, 9, t - inAt);
}

// Scale of the HOME menu (about the screen centre, footer excluded) while it
// leaves or returns; 1 at any other time.
float HomeZoom() {
    if (!trans.on || trans.quiet) return 1;
    float t = static_cast<float>(now - trans.t0);
    if (trans.homeOut && !trans.applied) {
        float f = (t - trans.outAt) / kFrame - 2; // the shrinking starts two frames into the fade
        return 1 - (std::min)(0.028f, 0.0052f * (std::max)(0.0f, f));
    }
    if (trans.homeIn && trans.applied) {
        float v = (t - (trans.outAt + trans.outDur + trans.blank)) / kHomeGrow;
        return 1 - 0.0247f * (1 - (std::min)(1.0f, (std::max)(0.0f, v)));
    }
    return 1;
}

void Push(Scr s) {
    // Sheets, the launch screen, sleep and the picture viewer have their own entrances.
    bool plain = cfg.menuStyle != 2 || s == Scr::Who || s == Scr::Launch || s == Scr::Sleep || s == Scr::Viewer;
    if (plain) {
        PushNow(s);
        return;
    }
    StartTransition(true, [s] { stack.push_back(s); });
    // How long the console shows nothing before each page (frames from fully
    // faded out, frame 30, to the first frame with the page in it).
    if (s == Scr::Settings) trans.blank = 22 * kFrame;
    else if (s == Scr::Pads) trans.blank = 15 * kFrame;
    else if (s == Scr::Album) trans.blank = 17 * kFrame;
    else if (s == Scr::All) {
        // The software grid arrives in two steps (walkthrough.mp4 100.67 s):
        // its tab strip from frame 34, the pictures later (see DrawAll).
        trans.blank = 4 * kFrame;
        allShownAt = -1;
    }
}
void Pop() {
    if (cfg.menuStyle != 2 || stack.back() == Scr::Who || stack.back() == Scr::Viewer || stack.back() == Scr::Sleep) PopNow();
    else StartTransition(false, [] { if (stack.size() > 1) stack.pop_back(); });
}

Game* FindGame(const std::string& id) {
    for (auto& g : games)
        if (g.id == id) return &g;
    return nullptr;
}

bool PressJustStarted(); // with the dialog code, below

void Ask(const std::wstring& msg, std::vector<std::wstring> btns, std::function<void(int)> done = nullptr) {
    dlg.open = true;
    dlg.msg = msg;
    dlg.btns = std::move(btns);
    dlg.done = std::move(done);
    dlg.sel = static_cast<int>(dlg.btns.size()) - 1;
    // With three choices the second console puts the third on a row of its
    // own above the other two, and starts on the lower right one.
    if (cfg.menuStyle == 2 && dlg.btns.size() >= 3) dlg.sel = 1;
    dlg.anim = 0;
    dlg.iconGame.clear();
    // Measured on the capture (power button, anim.mkv 256.35 s): the pressed
    // button reacts first and the menu starts to darken 18 frames after it.
    dlg.openAt = now + (cfg.menuStyle == 2 && PressJustStarted() ? 18 / 60.0 : 0.0);
    dlg.pressAt = -1;
    dlg.tinted = false;
    dlg.result = 0;
    dlg.lastSel = -1;
    dlg.selAt = -1;
    dlg.busy = false;
    PlaySfx(Sfx::Dialog);
    Touch();
}

void Info(const std::wstring& msg) {
    Ask(msg, {L"Aceptar"});
}

// ---- formatting -----------------------------------------------------------

std::wstring PlayTime(uint64_t s) {
    if (s < 60) return L"Sin apenas jugar";
    wchar_t b[64];
    if (s < 3600) swprintf_s(b, L"%llu min", s / 60);
    else if (s < 36000) swprintf_s(b, L"%llu h %llu min", s / 3600, (s % 3600) / 60);
    else swprintf_s(b, L"%llu horas", s / 3600);
    return b;
}

std::wstring DateOf(const std::string& iso) {
    if (iso.size() < 10) return L"Nunca";
    return lang::Date(atoi(iso.substr(0, 4).c_str()), atoi(iso.substr(5, 2).c_str()), atoi(iso.substr(8, 2).c_str()));
}

std::wstring SizeOf(uint64_t bytes) {
    double gb = bytes / 1073741824.0;
    if (gb >= 1) return lang::Number(gb, 1) + L" GB";
    return lang::Number(bytes / 1048576.0, 0) + L" MB";
}

std::wstring EmuLabel(const std::string& id) {
    const EmuDef* e = FindEmu(id);
    return e ? e->label : L"Ninguno";
}

// ---- shared pieces --------------------------------------------------------

// kind: 0 online, 1 news, 2 shop, 3 album, 4 controllers, 5 settings,
//       6 power, 7 user, 8 all software
void SysIcon(int kind, float cx, float cy, Color c) {
    switch (kind) {
    case 0:
        CircleStroke(cx, cy, 13, c, 2.4f);
        Line(cx - 13, cy, cx + 13, cy, c, 2.2f);
        Arc(cx - 17, cy, 21, -38, 38, c, 2.2f);
        Arc(cx + 17, cy, 21, 142, 218, c, 2.2f);
        break;
    case 1:
        RRectStroke(cx - 14, cy - 12, 28, 20, 3, c, 2.6f);
        Line(cx - 8, cy - 5, cx + 8, cy - 5, c, 2.4f);
        Line(cx - 8, cy + 1, cx + 3, cy + 1, c, 2.4f);
        Triangle(cx - 6, cy + 8, cx + 2, cy + 8, cx - 6, cy + 15, c);
        break;
    case 2:
        RRect(cx - 13, cy - 6, 26, 19, 3, c);
        Arc(cx, cy - 6, 6.5f, 180, 360, c, 2.6f);
        break;
    case 3:
        RRectStroke(cx - 14, cy - 10, 28, 21, 3, c, 2.6f);
        Triangle(cx - 10, cy + 7, cx - 2, cy - 3, cx + 5, cy + 7, c);
        Triangle(cx + 1, cy + 7, cx + 6, cy + 1, cx + 11, cy + 7, c);
        Circle(cx + 7, cy - 4, 2.4f, c);
        break;
    case 4:
        RRectStroke(cx - 15, cy - 9, 30, 19, 8, c, 2.6f);
        Line(cx - 10, cy, cx - 4, cy, c, 2.4f);
        Line(cx - 7, cy - 3, cx - 7, cy + 3, c, 2.4f);
        Circle(cx + 7, cy, 2.6f, c);
        break;
    case 5:
        CircleStroke(cx, cy, 7, c, 3.0f);
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7853982f;
            Line(cx + cosf(a) * 10.5f, cy + sinf(a) * 10.5f, cx + cosf(a) * 14.5f, cy + sinf(a) * 14.5f, c, 3.6f);
        }
        break;
    case 6:
        Arc(cx, cy + 1, 11, -55, 235, c, 2.8f);
        Line(cx, cy - 13, cx, cy - 2, c, 2.8f);
        break;
    case 7:
        Circle(cx, cy - 5, 6, c);
        Arc(cx, cy + 14, 12, 200, 340, c, 5.0f);
        break;
    case 8:
        for (int i = 0; i < 4; i++) RRect(cx - 12 + (i % 2) * 13.0f, cy - 12 + (i / 2) * 13.0f, 11, 11, 2, c);
        break;
    case 9: // chat
        RRectStroke(cx - 12, cy - 11, 24, 22, 3, c, 2.6f);
        Arc(cx + 1, cy, 5.5f, 60, 300, c, 2.6f);
        break;
    case 10: // share
        RRectStroke(cx - 13, cy - 2, 26, 13, 2, c, 2.6f);
        Line(cx - 10, cy - 7, cx + 10, cy - 7, c, 2.6f);
        Line(cx - 7, cy - 12, cx + 7, cy - 12, c, 2.6f);
        break;
    case 11: // game cards
        RRectStroke(cx - 10, cy - 13, 20, 26, 2.5f, c, 2.6f);
        Line(cx - 4, cy + 7, cx + 4, cy + 7, c, 2.6f);
        break;
    case 12: // outlined 2x2 grid
        for (int i = 0; i < 4; i++)
            RRectStroke(cx - 13 + (i % 2) * 15.0f, cy - 13 + (i / 2) * 15.0f, 11, 11, 1.5f, c, 2.0f);
        break;
    }
}

struct Hint {
    const wchar_t* glyph;
    std::wstring label;
    Btn btn;
};

void OnButtonImpl(Btn b);

// What the footer showed last frame, and what it showed before the last
// screen change, so the old hints can fade out while the new ones fade in.
std::vector<Hint> shownHints, fadingHints;
bool shownLine = false, fadingLine = false;
double hintFadeAt = -1;

bool SameHints(const std::vector<Hint>& a, const std::vector<Hint>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].label != b[i].label || wcscmp(a[i].glyph, b[i].glyph)) return false;
    return true;
}

void DrawHints(const std::vector<Hint>& hints, float a, bool clickable) {
    Theme T = Th();
    // Places measured on walkthrough.mp4 at 96 s ("Opciones", "Confirmar"): the
    // last wording ends on pixel column 1823, each disc is centred 34.5 px
    // left of its wording's first stroke and 62.5 px right of the wording before.
    float x = 1215 + Px(2);
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        float tw = TextWidth(it->label, 23.4f);
        float left = x - tw - 33.5f;
        Text(it->label, x - tw, 660, tw + 4, 40, 23.4f, alpha(T.text, a));
        // Filled button glyphs from the console's symbol font when it is
        // available: U+E0E0.. are A B X Y, U+E0EF/E0F0 are plus and minus.
        wchar_t g = it->glyph[0];
        wchar_t sym = g == L'A' ? 0xE0E0 : g == L'B' ? 0xE0E1 : g == L'X' ? 0xE0E2 : g == L'Y' ? 0xE0E3
                    : g == L'+' ? 0xE0EF : g == L'-' ? 0xE0F0 : 0;
        // (The plus disc sits a pixel lower than the lettered ones there.)
        if (!sym || !Symbol(sym, left + 11.5f, 673.4f + (g == L'+' ? Px(1) : 0.0f), 24.45f, alpha(T.text, a))) {
            Circle(left + 11.5f, 680, 11.4f, alpha(T.text, a));
            if (g == L'+') {
                Rect(left + 5.5f, 678.7f, 12, 2.6f, alpha(T.bg, a));
                Rect(left + 10.2f, 674, 2.6f, 12, alpha(T.bg, a));
            } else {
                Text(it->glyph, left - 0.5f, 668.5f, 24, 24, 16, alpha(T.bg, a), Center, 700);
            }
        }
        if (clickable) {
            Btn b = it->btn;
            AddHit(left - 6, 658, tw + 46, 46, [b] { OnButtonImpl(b); });
        }
        x = left + 11.5f - Px(61.5f);
    }
}

void Footer(const std::vector<Hint>& hints, bool line = true) {
    Theme T = Th();
    if (S2()) {
        // 1 once the new hints are fully in; below 1 while cross-fading.
        float p = 1;
        if (hintFadeAt >= 0) {
            p = static_cast<float>((now - hintFadeAt) / 0.085);
            if (p >= 1) {
                p = 1;
                hintFadeAt = -1;
            } else {
                Touch();
            }
        }
        bool crossfade = p < 1 && !SameHints(hints, fadingHints);
        float lineA = line ? ((p < 1 && !fadingLine) ? p : 1.0f) : ((p < 1 && fadingLine) ? 1 - p : 0.0f);
        if (lineA > 0) HRule(48, 1232, Px(968), alpha(T.line, lineA)); // pixel row 968
        // Player lights and the controller pair, bottom left.
        if (Img* pad = PackIcon(cfg.dark ? L"pad_d" : L"pad_l")) {
            Image(*pad, 46, 654, 68, 54);
        } else {
            // Player lights as on the capture: the lit one is another green in each theme.
            for (int i = 0; i < 4; i++)
                Rect(51, 661 + i * 10.0f, 6, 6, rgb(cfg.dark ? (i == 0 ? 0x82ED0D : 0x616161) : (i == 0 ? 0x12C007 : 0xB4B1B5)));
            RRect(67, 660, 19, 42, 8, T.text);
            RRect(90, 660, 19, 42, 8, T.text);
            Circle(76.5f, 672, 3, T.bg);
            Circle(99.5f, 690, 3, T.bg);
        }
        if (crossfade) DrawHints(fadingHints, 1 - p, false);
        DrawHints(hints, crossfade ? p : 1.0f, true);
        shownHints = hints;
        shownLine = line;
        return;
    }
    if (line) Line(30, 647.5f, 1250, 647.5f, T.line, 1);
    // Controller indicator, bottom left: the console shows the controller in
    // use as a small filled figure with its four player lights. Drawn here
    // from the console maker's own description of the variants: a pad when
    // one is connected (lights in a row above it), otherwise the pair of
    // detached controllers (lights in a column beside it).
    {
        const Color lit = rgb(0xA8E619), dim = rgb(cfg.dark ? 0x6E6E6E : 0xB9B9B9), ink = T.text, hole = T.bg;
        bool pad = false;
        for (bool on : ConnectedPads()) pad = pad || on;
        if (pad) {
            const float ox = 54, oy = 675; // the figure is 44 x 31
            for (int i = 0; i < 4; i++) Rect(ox + 3.5f + i * 10.2f, oy - 13, 6.5f, 6.5f, i == 0 ? lit : dim);
            RRect(ox + 4, oy + 1, 36, 23, 8, ink);
            PushRotate(13, ox + 7, oy + 11);
            RRect(ox + 0.5f, oy + 6, 12.5f, 25, 6.2f, ink);
            PopTransform();
            PushRotate(-13, ox + 37, oy + 11);
            RRect(ox + 31, oy + 6, 12.5f, 25, 6.2f, ink);
            PopTransform();
            Circle(ox + 10.3f, oy + 9, 3.1f, hole);                 // left stick
            Rect(ox + 12.6f, oy + 16.3f, 6.8f, 1.9f, hole);          // direction buttons
            Rect(ox + 15.05f, oy + 13.85f, 1.9f, 6.8f, hole);
            Circle(ox + 27.6f, oy + 17.2f, 3.1f, hole);              // right stick
            for (int i = 0; i < 4; i++)                              // the four face buttons
                Circle(ox + 33.6f + (i == 1 ? 3.0f : i == 3 ? -3.0f : 0.0f), oy + 9 + (i == 0 ? -3.0f : i == 2 ? 3.0f : 0.0f),
                       1.35f, hole);
        } else {
            const float ox = 54, oy = 665.25f; // lights, then two figures 16.5 x 41.5
            for (int i = 0; i < 4; i++) Rect(ox, oy + 2.5f + i * 10.0f, 7.5f, 7.5f, i == 0 ? lit : dim);
            const float lx = ox + 18, rx = ox + 42;
            RRect(lx, oy, 16.5f, 41.5f, 8, ink); // rounded outside, straight where the two meet
            Rect(lx + 8.5f, oy, 8, 41.5f, ink);
            RRect(rx, oy, 16.5f, 41.5f, 8, ink);
            Rect(rx, oy, 8, 41.5f, ink);
            Circle(lx + 8, oy + 10.5f, 3.3f, hole);
            Circle(rx + 8.5f, oy + 26, 3.3f, hole);
            for (int i = 0; i < 4; i++) {
                const float dx = i == 1 ? 3.2f : i == 3 ? -3.2f : 0.0f, dy = i == 0 ? -3.2f : i == 2 ? 3.2f : 0.0f;
                Circle(lx + 8 + dx, oy + 25.5f + dy, 1.35f, hole);
                Circle(rx + 8.5f + dx, oy + 11 + dy, 1.35f, hole);
            }
        }
    }

    float x = 1238;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        float tw = TextWidth(it->label, 21);
        float left = x - tw - 36;
        Text(it->label, x - tw, 664, tw + 4, 36, 21, T.text);
        Circle(left + 13, 682, 12.5f, T.text);
        Text(it->glyph, left, 669, 26, 25, 16, T.bg, Center, 700);
        Btn b = it->btn;
        AddHit(left - 6, 660, tw + 48, 44, [b] { OnButtonImpl(b); });
        x = left - 26;
    }
}

void Header(int icon, const std::wstring& title) {
    Theme T = Th();
    A11yTitle(title);
    if (S2()) {
        PushScale(1.15f, 98, 50);
        SysIcon(icon, 98, 50, T.text);
        PopTransform();
        Text(title, 123, 28, 900, 46, 28.6f, T.text); // width-matched to the capture's header
        HRule(48, 1232, Px(127), T.line); // pixel row 127 on every page of the console
        return;
    }
    SysIcon(icon, 76, 46, T.text);
    Text(title, 112, 20, 800, 52, 28, T.text);
    Line(30, 88.5f, 1250, 88.5f, T.line, 1);
}

/// ---- cursor ---------------------------------------------------------------
// One cursor is drawn per frame. On the second console's menu it does not
// jump: the new outline is there at once, at full strength, while the old one
// (with the pale fill behind it) fades out where it was.

struct CursorRect {
    float x, y, w, h, r;
};
struct Ghost {
    CursorRect c;
    float a;
    double t0 = 0;       // when the cursor left this spot
    Color fill = {};     // the focus fill that was behind it, fading with it
    bool hasFill = false;
};
std::vector<Ghost> ghosts;
CursorRect curNow = {}, curLast = {};
bool curDrawn = false, hadLast = false, suppressCursor = false;
float curAlpha = 1;

// The fill behind the cursor, announced by the caller with CursorFill() just
// before Cursor(), so that the ghost can take it along.
Color curFill = {}, curLastFill = {};
bool curHasFill = false, curLastHasFill = false;
double curJumpAt = -1; // when the cursor last moved to another item
// Reaching the end of a list nudges the cursor in that direction.
double bumpAt = -1;
float bumpDx = 0, bumpDy = 0;

// The outline's colours. Measured on the capture (anim.mkv 100-108 s for the
// ring, walkthrough.mp4 246-252.6 s for a box): the colours run round the
// outline as an angular gradient, not across it; the whole pattern turns
// clockwise at 54 degrees a second, and four times per turn (every 100 frames)
// it swells from a deeper to a paler set of colours and back. On a box the
// angle is taken in the box's own proportions (a square pattern stretched over
// it), so the long sides carry most of the colours. 30 stops, 12 degrees apart.
const uint32_t kRingDeep[30] = {0xE4A3BA, 0xD19AC1, 0xB28DCB, 0x867BD6, 0x2663E0, 0x1460DF, 0x1460E0, 0x1461E0,
                                0x467CE8, 0x6EA0EE, 0x8BBEEE, 0xA3DAEF, 0xB6EAF0, 0xBDEDF0, 0xACE3F2, 0x97CBF1,
                                0x7EA5F1, 0x6B7FF0, 0x6563ED, 0x7061EE, 0x7361EE, 0x6C61EE, 0x6964EA, 0x8C76E0,
                                0xB48ACD, 0xD79ABC, 0xE7A3B7, 0xECA8B4, 0xEDAAB3, 0xEBA8B5};
const uint32_t kRingPale[30] = {0xF3C9E7, 0xF4BFF0, 0xDCB0F5, 0xA89AF6, 0x317DF3, 0x177BF3, 0x177BF3, 0x167AF2,
                                0x579AF0, 0x88C5EF, 0xAAE7EF, 0xC5EFF0, 0xDDEDEF, 0xE5EDF0, 0xCFEFF0, 0xB7EFF1,
                                0x9BCAF1, 0x859FF1, 0x7D7DF0, 0x8C7AEF, 0x927BF0, 0x887BF0, 0x857FEF, 0xB196F2,
                                0xE2AFF4, 0xF4C3ED, 0xF2CDE4, 0xF2D2E0, 0xF2D3DF, 0xF2D0E1};
const uint32_t kBoxPale[30] = {0xC59CF3, 0xA391F4, 0x6782F4, 0x407CF4, 0x5984F5, 0xB39EF6, 0xF4B8F4, 0xFAC7EE,
                               0xF2D1E3, 0xF1D7DF, 0xF1D9DD, 0xF3D0E4, 0xF0BBF3, 0xB3A6F5, 0x599AF3, 0x5C9EF2,
                               0x4892F1, 0x2C81F1, 0x217BF0, 0x3D8BF0, 0x6FAFF0, 0x92CFF0, 0xA3E2F0, 0xAEECF0,
                               0xB6F1F0, 0xB1EEF0, 0xB0D4F1, 0xBFADF3, 0xCBA0F3, 0xCEA0F2};
const uint32_t kBoxDeep[30] = {0x796DE6, 0x776FE2, 0x8E77DF, 0xA27DDC, 0x987DD9, 0x7D78D6, 0x907FCF, 0xC190C3,
                               0xE6A2B7, 0xEFAAAF, 0xEFA9AF, 0xE7A2B5, 0xC895C1, 0x7679D7, 0x2569E3, 0x286AE3,
                               0x3472E4, 0x4C81E7, 0x5D8CEA, 0x5B8FEC, 0x558CED, 0x6397EF, 0x7BAEEF, 0x8EC2F0,
                               0x91C5F0, 0x85B6F0, 0x99A9EC, 0x9D8DE0, 0x9679E0, 0x8871E6};

// Colour of the outline at `angle` degrees (0 = right, clockwise) for the
// pattern turned by `phase` degrees.
Color CursorHue(bool ring, float angle, float phase) {
    const float k = 3.14159265f / 180.0f;
    // How far the swell is towards the "deep" set: the two are locked to the
    // turn (ring palest at phase 38, box deepest at phase 54, every 90).
    float deep = ring ? 0.5f - 0.5f * cosf(4 * (phase - 38.4f) * k) : 0.5f + 0.5f * cosf(4 * (phase - 54.0f) * k);
    float a = fmodf(angle - phase, 360.0f);
    if (a < 0) a += 360.0f;
    int i0 = static_cast<int>(a / 12.0f) % 30, i1 = (i0 + 1) % 30;
    float f = a / 12.0f - floorf(a / 12.0f);
    const uint32_t* pale = ring ? kRingPale : kBoxPale;
    const uint32_t* dark = ring ? kRingDeep : kBoxDeep;
    return mix(mix(rgb(pale[i0]), rgb(pale[i1]), f), mix(rgb(dark[i0]), rgb(dark[i1]), f), deep);
}

// `rest`: the first frame of a cursor that has just moved shows the pattern in
// its starting position (phase 138) before it joins the running turn.
void StrokeCursor(const CursorRect& c, float a, bool rest = false) {
    if (!S2()) {
        RRectStroke(c.x, c.y, c.w, c.h, c.r, alpha(CursorColor(), a), 5);
        return;
    }
    if (a <= 0) return;
    const float k = 3.14159265f / 180.0f;
    const float cx = c.x + c.w / 2, cy = c.y + c.h / 2;
    const bool ring = fabsf(c.w - c.h) < 1.0f;
    float phase = static_cast<float>(fmod(now * 54.0, 360.0));
    if (rest && !ring) phase = 138.0f;
    if (a < 1) PushLayer(a); // the pieces overlap, so they must fade as one
    if (ring && c.r >= c.w / 2 - 0.5f) {
        // Measured: 6.4 px wide on a 60 px radius.
        for (int i = 0; i < 120; i++) {
            float a0 = i * 3.0f;
            Arc(cx, cy, c.w / 2, a0, a0 + 3.0f, CursorHue(true, a0 + 1.5f, phase), 4.2f);
        }
    } else {
        // Measured: 6 px (4 units) wide. Straight runs in short pieces, each
        // coloured by where it lies; corners as three arcs each.
        const float sw = 4.0f, r = (std::min)(c.r, (std::min)(c.w, c.h) / 2);
        auto hue = [&](float px, float py) {
            return CursorHue(ring, atan2f((py - cy) / c.h, (px - cx) / c.w) / k, phase);
        };
        auto run = [&](float x0, float y0, float x1, float y1) {
            float len = fabsf(x1 - x0) + fabsf(y1 - y0);
            if (len < 0.5f) return;
            int n = (std::max)(1, static_cast<int>(len / 5.0f));
            float ux = (x1 - x0) / len, uy = (y1 - y0) / len;
            for (int i = 0; i < n; i++) {
                float t0 = len * i / n - 0.4f, t1 = len * (i + 1) / n + 0.4f, tm = (t0 + t1) / 2;
                Line(x0 + ux * t0, y0 + uy * t0, x0 + ux * t1, y0 + uy * t1, hue(x0 + ux * tm, y0 + uy * tm), sw);
            }
        };
        auto corner = [&](float ox, float oy, float from) {
            for (int i = 0; i < 3; i++) {
                float a0 = from + i * 30.0f, am = (a0 + 15.0f) * k;
                Arc(ox, oy, r, a0, a0 + 30.0f, hue(ox + cosf(am) * r, oy + sinf(am) * r), sw);
            }
        };
        const float x0 = c.x, y0 = c.y, x1 = c.x + c.w, y1 = c.y + c.h;
        run(x0 + r, y0, x1 - r, y0);
        run(x1, y0 + r, x1, y1 - r);
        run(x1 - r, y1, x0 + r, y1);
        run(x0, y1 - r, x0, y0 + r);
        if (r > 0.5f) {
            corner(x1 - r, y0 + r, 270);
            corner(x1 - r, y1 - r, 0);
            corner(x0 + r, y1 - r, 90);
            corner(x0 + r, y0 + r, 180);
        }
    }
    if (a < 1) PopLayer();
}

// Call just before Cursor() when the focused item has a pale fill behind it.
void CursorFill(Color fill) {
    curFill = fill;
    curHasFill = true;
}

// The cursor could not move that way (dx, dy: -1, 0 or 1).
void BumpCursor(float dx, float dy) {
    bumpAt = now + 1.0 / 60.0; // called from a button handler, where `now` is still the last frame's
    bumpDx = dx;
    bumpDy = dy;
    Touch();
}

void Cursor(float x, float y, float w, float h, float r) {
    bool filled = curHasFill;
    Color fill = curFill;
    curHasFill = false;
    if (suppressCursor) return;
    A11yCursor(x, y, w, h);
    CursorRect c = {x, y, w, h, r};
    if (!S2()) {
        StrokeCursor(c, 1);
        return;
    }
    // A small shift is scrolling; a jump is a move to another item.
    bool jumped = hadLast && (fabsf(c.x - curLast.x) > 30 || fabsf(c.y - curLast.y) > 30 ||
                              fabsf(c.w - curLast.w) > 30 || fabsf(c.h - curLast.h) > 30);
    if (jumped) {
        Ghost g;
        g.c = curLast;
        g.a = 1;
        g.t0 = now;
        g.fill = curLastFill;
        g.hasFill = curLastHasFill;
        ghosts.push_back(g);
        curLast = c; // only one ghost per jump
        curJumpAt = now;
    }
    curLastFill = fill;
    curLastHasFill = filled;
    CursorRect shown = c;
    if (bumpAt >= 0) {
        // Measured (walkthrough.mp4 141.73 s, Down on the last row): the box
        // shifts 5 px that way over 4 frames and is back after 9.
        float t = static_cast<float>(now - bumpAt) * 60.0f + 1.0f;
        float p = t < 4 ? t / 4 : (std::max)(0.0f, 1 - (t - 4) / 5);
        if (t >= 9) bumpAt = -1;
        else Touch();
        shown.x += bumpDx * 3.4f * p;
        shown.y += bumpDy * 3.4f * p;
    }
    StrokeCursor(shown, 1, now - curJumpAt < 0.008);
    curNow = c;
    curDrawn = true;
}

void CircleCursor(float cx, float cy, float r) {
    Cursor(cx - r, cy - r, r * 2, r * 2, r);
}

void EndCursors(float dt) {
    (void)dt;
    if (curDrawn) {
        curLast = curNow;
        hadLast = true;
    } else {
        hadLast = false;
    }
    curAlpha = 1;
    curDrawn = false;
    curHasFill = false;
    for (auto& g : ghosts) {
        // Measured on Settings (walkthrough.mp4 127.117 s and 128.683 s): in
        // the frame of the move the old outline is at 0.89, in the next at
        // 0.62, then gone; the fill behind it at 0.85 and 0.48.
        float t = (static_cast<float>(now - g.t0) + 1.0f / 60.0f) / 0.05f;
        g.a = t >= 0.999f ? 0.0f : 1.0f - powf(t, 2.2f);
        if (g.a > 0 && coverNow <= 0 && S2()) {
            if (g.hasFill) RRect(g.c.x, g.c.y, g.c.w, g.c.h, g.c.r, alpha(g.fill, g.fill.a * powf(g.a, 1.35f)));
            StrokeCursor(g.c, g.a);
        }
    }
    ghosts.erase(std::remove_if(ghosts.begin(), ghosts.end(), [](const Ghost& g) { return g.a <= 0; }), ghosts.end());
    if (!ghosts.empty()) Touch();
    // The pattern keeps turning for as long as a cursor is on screen.
    if (hadLast && S2()) Touch();
}

void OnScreenSwitched() {
    // The new screen's cursor is not a move from the old one's.
    ghosts.clear();
    hadLast = false;
    curAlpha = 1;
    fadingHints = shownHints;
    fadingLine = shownLine;
    hintFadeAt = now;
}

// ---- press feedback -------------------------------------------------------
// Pressing A on something makes its picture react, and a disc in the picture's
// own colour swells out from under it. Measured on the capture (anim.mkv, all
// ten bar buttons pressed between 256 and 280 s, 60 frames a second, frame 0 =
// the first frame that differs): the disc's edge and strength are the same for
// every button except the power button, whose disc runs faster.

struct PressFx {
    bool on = false;
    double t0 = 0;
    CursorRect rect = {};
} pressFx;

void StartPress() {
    if (!S2() || !hadLast) return;
    pressFx.on = true;
    pressFx.t0 = ReactionStart();
    pressFx.rect = curLast;
    Touch();
}

float PressAge() {
    return static_cast<float>(now - pressFx.t0);
}

// True while the element whose cursor box is centred on (cx, cy) is reacting to a press.
bool PressedAt(float cx, float cy) {
    if (!pressFx.on) return false;
    if (PressAge() > 0.32f) { // measured: the disc is gone 19 frames after the press
        pressFx.on = false;
        return false;
    }
    const CursorRect& r = pressFx.rect;
    return fabsf(r.x + r.w / 2 - cx) < 3 && fabsf(r.y + r.h / 2 - cy) < 3;
}

// Scale of a pressed picture `age` seconds after the press, for pictures that
// have no animation of their own. Measured on the settings wheel: 75 % for
// four frames, back to full size by frame 7, 2 % over until frame 17.
float PressDipAt(float age) {
    static const float k[] = {0.75f, 0.78f, 0.76f, 0.79f, 0.83f, 0.93f, 0.985f, 1.015f, 1.025f, 1.02f,
                              1.015f, 1.02f, 1.02f, 1.02f, 1.015f, 1.015f, 1.005f, 1.0f};
    return FrameTable(k, 18, age);
}
float PressDip() {
    return PressDipAt(PressAge());
}

// The swelling disc for a round element whose cursor ring has radius r0; draw
// it before the element. `tint` is the element's own colour: the disc of a red
// icon is red. Measured per frame: how far the edge is outside the ring's
// radius (ring 41 -> edge 43.2 on frame 0, 56.5 at the end) and the opacity
// (0.42 at its strongest in the dark theme, 0.20 in the light one).
void PressDiscAt(float cx, float cy, float r0, float age, Color tint, bool quick = false) {
    static const float grow[] = {2.2f, 4.8f, 7.5f, 9.3f, 11.5f, 12.8f, 13.3f, 14.1f, 14.2f, 14.2f,
                                 14.5f, 14.6f, 14.6f, 14.6f, 14.6f, 15.4f, 15.5f, 15.5f, 15.5f};
    static const float strong[] = {0.24f, 0.42f, 0.41f, 0.405f, 0.39f, 0.38f, 0.36f, 0.34f, 0.31f, 0.285f,
                                 0.26f, 0.23f, 0.19f, 0.165f, 0.125f, 0.09f, 0.055f, 0.02f, 0.0f};
    // The power button, which opens a dialog instead of a page.
    static const float growQ[] = {2.5f, 7.3f, 11.3f, 13.3f, 14.0f, 14.0f, 14.0f, 14.6f, 14.6f, 14.7f,
                                  14.7f, 14.7f, 14.7f, 14.7f};
    static const float strongQ[] = {0.25f, 0.43f, 0.42f, 0.41f, 0.39f, 0.345f, 0.31f, 0.28f, 0.24f, 0.18f,
                                  0.13f, 0.105f, 0.06f, 0.0f};
    float a = quick ? FrameTable(strongQ, 14, age) : FrameTable(strong, 19, age);
    float r = r0 + (quick ? FrameTable(growQ, 14, age) : FrameTable(grow, 19, age));
    if (!cfg.dark) a *= 0.48f; // measured on the light theme's power button (walkthrough.mp4, 289.7 s)
    if (a > 0) Circle(cx, cy, r, alpha(tint, a));
    Touch();
}
void PressDisc(float cx, float cy, float r0) {
    PressDiscAt(cx, cy, r0, PressAge(), Th().icon);
}

// The console sets the clock and the battery percentage in numerals of its
// own, which are not in the system font. With the owner's local pack
// (tools\pack_digits.py) they are drawn from pictures, each in the fixed cell
// it has on the console; without it, the system font is used.
Img* PackNumeral(wchar_t ch) {
    wchar_t key = ch == L':' ? L'c' : ch == L'%' ? L'p' : ch;
    return PackIcon(std::wstring(L"dig_") + key + (cfg.dark ? L"_d" : L"_l"));
}

bool HavePackNumerals(const std::wstring& s) {
    for (wchar_t ch : s)
        if (!PackNumeral(ch)) return false;
    return true;
}

// `w` is the picture's width in layout units; all are 24 high from y = 64.
void DrawPackNumeral(wchar_t ch, float cx, float w = 16.0f) {
    if (Img* img = PackNumeral(ch)) Image(*img, cx - w / 2, 64, w, 24);
}

// Defined with the PC meanings of the console's rows, further down.
bool PcBatteryShown();
void PcLogError(const std::string& gameId, const std::string& code);

void DrawStatus() {
    Theme T = Th();
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t tb[16];
    std::wstring suffix; // the 12-hour clock's AM / PM, in the menu's language
    swprintf_s(tb, L"%ls", lang::Clock(st.wHour, st.wMinute, cfg.clock24, S2(), suffix).c_str());
    const bool periodFirst = lang::PeriodFirst(); // 午後 3:45
    SYSTEM_POWER_STATUS ps = {};
    float level = 1.0f; // real charge on laptops, full on desktops
    if (GetSystemPowerStatus(&ps) && ps.BatteryLifePercent <= 100 && !(ps.BatteryFlag & 128))
        level = ps.BatteryLifePercent / 100.0f;

    if (S2()) {
        // Digits are set wide apart on the console; sizes and spacing were
        // fitted to the capture (clock 73 units wide, "100" 40 units).
        const float size = 26, gap = 1.7f;
        wchar_t pct[8];
        swprintf_s(pct, L"%d", static_cast<int>(level * 100 + 0.5f));
        // Cell centres measured on the capture (1080p pixels / 1.5).
        const bool packed = suffix.empty() && wcslen(tb) == 5 && HavePackNumerals(tb) &&
                            HavePackNumerals(std::wstring(pct) + L"%");
        if (packed) {
            // The pack's pictures are 24 px wide and were cut from pixel columns
            // 1466, 1490, 1533 and 1558 (colon: 14 px from 1516); the console
            // spaces its numerals by fractions of a pixel, the nearest whole
            // column keeps a picture as sharp as it was cut.
            static const float cell[4] = {Px(1478), Px(1502), Px(1545), Px(1570)};
            DrawPackNumeral(tb[0], cell[0]);
            DrawPackNumeral(tb[1], cell[1]);
            DrawPackNumeral(L':', 1015.33f, 9.33f);
            DrawPackNumeral(tb[3], cell[2]);
            DrawPackNumeral(tb[4], cell[3]);
        } else {
            float sw = suffix.empty() ? 0 : TextWidth(suffix, 15, 600) + 5;
            float tw = TextSpacedWidth(tb, size, gap);
            float right = 1052;
            if (!suffix.empty()) Text(suffix, periodFirst ? right - tw - sw : right - sw + 5, 64, sw, 30, 15, T.text, Left, 600);
            TextSpaced(tb, periodFirst ? right - tw : right - sw - tw, 56, tw + 40, 40, size, T.text, gap);
        }
        if (Img* wifi = PackIcon(cfg.dark ? L"wifi_d" : L"wifi_l")) {
            Image(*wifi, 1066.67f, 60, 40, 30);
        } else {
            float wx = 1086, wy = 84;
            Circle(wx, wy, 2.2f, T.text);
            for (int i = 1; i <= 3; i++) Arc(wx, wy, i * 5.0f, 225, 315, T.text, 2.2f);
        }
        if (!PcBatteryShown()) {
            // "Batería de la consola (%)" is off: the symbol alone
        } else if (packed) {
            // Right-aligned in three cells, the sign after them.
            // "100 %" on the capture (walkthrough.mp4 96 s) with these pictures:
            // cells from pixel columns 1676, 1699 and 1723, the sign from 1746.
            static const float cell[3] = {Px(1688), Px(1711), Px(1735)};
            int len = static_cast<int>(wcslen(pct));
            for (int i = 0; i < len && i < 3; i++) DrawPackNumeral(pct[len - 1 - i], cell[2 - i]);
            DrawPackNumeral(L'%', Px(1760), 18.67f);
        } else {
            float pw = TextSpacedWidth(pct, size, -1.0f);
            TextSpaced(pct, 1121, 56, pw + 40, 40, size, T.text, -1.0f);
            Text(L"%", 1123 + pw, 62.5f, 30, 30, 18, T.text, Left, 600);
        }
        Img* batt = level >= 0.995f ? PackIcon(cfg.dark ? L"batt_d" : L"batt_l") : nullptr;
        if (batt) {
            Image(*batt, 1192, 61.33f, 45.33f, 29.33f);
        } else {
            RRectStroke(1197, 67, 30, 18, 4, T.text, 2);
            Rect(1229, 72, 3, 8, T.text);
            RRect(1200.5f, 70.5f, 23 * level, 11, 1.5f, T.text);
        }
        return;
    }
    float sw = suffix.empty() ? 0 : TextWidth(suffix, 15, 600) + 5;
    float tw = TextWidth(tb, 26);
    float right = 1112;
    if (!suffix.empty()) Text(suffix, periodFirst ? right - tw - sw : right - sw + 5, 50, sw, 30, 15, T.text, Left, 600);
    Text(tb, periodFirst ? right - tw : right - sw - tw, 42, tw + 4, 40, 26, T.text);
    float wx = 1148, wy = 73;
    Circle(wx, wy, 2.6f, T.text);
    for (int i = 1; i <= 3; i++) Arc(wx, wy, i * 6.5f, 225, 315, T.text, 2.6f);
    RRectStroke(1188, 52, 36, 20, 3, T.text, 2);
    Rect(1226, 58, 3, 8, T.text);
    Rect(1192, 56, 28 * level, 12, T.text);
}

// ---- home -----------------------------------------------------------------

struct SysBtn {
    const wchar_t* label;
    int icon;
    uint32_t color; // 0 = theme grey
    int action;     // 0 not available, 3 album, 4 controllers, 5 settings, 6 power, 7 all software,
                    // 8 the shop: a web address of the user's choosing,
                    // 9 the console maker's news page, 10 OBS Studio (GameChat: streaming and recording),
                    // 11 playing online with friends (netplay)
    uint32_t disc = 0; // colour of the disc that swells under it when pressed (second console)
    // Place in the console's full bar. The local pack names a button's
    // pictures by it, whichever buttons are on screen.
    int index = 0;
};

// The buttons of the bar that are on screen: all of the console's in identical
// mode, otherwise only the ones that do something here.
const std::vector<SysBtn>& SysList() {
    static const std::vector<SysBtn> first = {
        // Picture colours from the owner's description of an early firmware.
        {L"En línea", 0, 0xFFFFFF, 11}, {L"Noticias", 1, 0xE8412F, 9}, {L"Tienda", 2, 0xF29100, 8},
        {L"Álbum", 3, 0x2F86E8, 3},    {L"Mandos", 4, 0, 4},          {L"Configuración de la consola", 5, 0, 5},
        {L"Modo de espera", 6, 0, 6},
    };
    static const std::vector<SysBtn> second = {
        // Names as the console shows them under each button (walkthrough.mp4).
        // Icon colours sampled from the capture (dark theme); disc colours
        // fitted to the disc each button shows when pressed there.
        {L"Switch Online", 0, 0xFFFFFF, 11, 0xE01C27}, {L"GameChat", 9, 0xCD6722, 10, 0xD15B2E},
        {L"Noticias", 1, 0x24A25C, 9, 0x2A9E53},               {L"Nintendo eShop", 2, 0xD44639, 8, 0xDE3C3B},
        {L"Álbum", 3, 0x478CDE, 3, 0x3F86EC},                  {L"GameShare", 10, 0x25A1A5, 0, 0x279BA4},
        {L"Mandos", 4, 0, 4, 0xA59DAE},                        {L"Tarjetas de juego virtuales", 11, 0, 7, 0xA59DAE},
        {L"Configuración de la consola", 5, 0, 5, 0xA59DAE},   {L"Modo de espera", 6, 0, 6, 0xA59DAE},
    };
    static std::vector<SysBtn> shown[4];
    const std::vector<SysBtn>& all = S2() ? second : first;
    std::vector<SysBtn>& out = shown[(S2() ? 2 : 0) + (cfg.identical ? 1 : 0)];
    if (out.empty())
        for (size_t i = 0; i < all.size(); i++) {
            if (!cfg.identical && all[i].action == 0) continue;
            out.push_back(all[i]);
            out.back().index = static_cast<int>(i);
        }
    return out;
}
int SysCount() {
    return static_cast<int>(SysList().size());
}

constexpr float kTile = 256;
// Game row geometry, measured on each console's menu.
float TileY() { return S2() ? 200.0f : 192.0f; }
float TileX0() { return S2() ? 106.0f : 100.0f; }
float TileGap() { return S2() ? Px(22) : 14.0f; } // second console: tiles 384 px wide, 406 px apart
// Centre of system button i of the n on screen. On the second console the
// first of the full bar (larger) stands further apart; values measured on the
// capture (anim.mkv, the focus ring on each button in turn: centred on pixel
// columns 541 + 122 k for the nine alike, the red disc of the first on 404).
// A shorter bar keeps the 122-pixel step and is centred as a group, like the
// console's own.
float SysX(int i, int n) {
    if (S2() && n == 10) return i == 0 ? Px(404) : Px(541) + (i - 1) * Px(122);
    if (S2()) return W / 2 + (i - (n - 1) / 2.0f) * Px(122);
    return W / 2 + (i - (n - 1) / 2.0f) * 90;
}
// The second console's bar behind n buttons: 70 pixels beyond the centres of
// its end buttons (pixel columns 333 to 1587 for all ten, whose first is larger).
float SysBarX(int n) {
    return n == 10 ? 222.0f : SysX(0, n) - Px(70);
}
float SysBarW(int n) {
    return n == 10 ? 836.0f : (n - 1) * Px(122) + Px(140);
}
// Its rows: the panel is pixel rows 770 to 910, the buttons are centred on 840.
constexpr float kBarCy = 560.0f;

bool ShowAllTile() {
    return games.size() > 12;
}
int HomeCount() {
    return static_cast<int>((std::min)(games.size(), static_cast<size_t>(12))) + (ShowAllTile() ? 1 : 0);
}

void OpenSettings(int cat);
void OpenSetup(); // the first-start assistant
void StartAddUser(std::function<void(int newIndex)> done); // with the user page
// The "NSW-Home" section, the last one whatever the mode.
constexpr int kNSWHomeCat = -1;

// ---- game launch flow -----------------------------------------------------
// Everything between pressing A on a game and the black loading screen, and
// the way back. All frame counts below were read from the owner's capture of
// the console at 60 frames a second (w5.mkv, light theme).

struct LaunchFlow {
    double bareAt = -1;   // the row lost its ring and title (A on a game, close confirmed)
    double sheetAt = -1;  // A on a game: the menu dims and the user sheet arrives
    double leaveAt = -1;  // the sheet was dismissed
    bool picked = false;  // ...because a user was picked (the game starts)
    bool closing = false; // "close" confirmed, the game has not ended yet
    double closeAt = -1;  // when to end it
    double endAt = -1;    // the running game ended with the menu showing: its mark fades
    std::string endedId;
    double homeAt = -1;   // the menu came back over a game
    double xAt = -1;      // X on the running game: the tile reacts, then the dialog opens
} flow;

// Buttons are read between frames, when `now` is still the time of the frame
// already on screen. An animation started by a button counts from the next
// frame, so that frame is its frame 0, as in the counts read off the capture.
double NextFrame() {
    return now + 1.0 / 60.0;
}
float FramesSince(double t) {
    return static_cast<float>((now - t) * 60.0);
}
float SmoothStep(float x) {
    x = (std::max)(0.0f, (std::min)(1.0f, x));
    return x * x * (3 - 2 * x);
}
// Value of a per-frame table at a fractional frame number.
float Lerp(const float* table, int count, float n) {
    if (n <= 0) return table[0];
    if (n >= count - 1) return table[count - 1];
    int i = static_cast<int>(n);
    return table[i] + (table[i + 1] - table[i]) * (n - i);
}

// How far the home menu is dimmed behind the user sheet, 0..1.
// Measured: A on a game darkens the menu to 66 % (black at 0.336) along a
// smooth step 7.5 frames long (w5 16.35 s: 0.03 0.11 0.23 0.43 0.65 0.84 0.95 1).
// After a user is picked it stays dark for 16 frames and clears along the same
// curve (w5 18.93 s: 0.05 0.16 0.35 0.57 0.77 0.89 0.97 1).
float HomeDim() {
    if (!S2()) return 0;
    if (flow.leaveAt >= 0) {
        float n = FramesSince(flow.leaveAt);
        return 1 - SmoothStep((n - (flow.picked ? 16.0f : 0.0f)) / 7.5f);
    }
    if (flow.sheetAt >= 0) return SmoothStep((FramesSince(flow.sheetAt) + 0.6f) / 7.5f);
    return stack.back() == Scr::Who ? 1.0f : 0.0f;
}

// Test states for the game row and the launch flow.
//   run      home menu with the first game running (suspended)
//   more     home menu with more than twelve games, so the row ends in the round button
//   last     the same, with the round button focused
//   ingame   the first game running and in front (black here); HOME returns to the menu
//   launchout  the loading screen fading out as the game takes over
bool DebugGame(const std::wstring& name);

void BeginPress(const std::string& id) {
    PlaySfx(Sfx::Launch);
    pressAt = now;
    pressId = id;
    if (S2() && flow.bareAt < 0) flow.bareAt = NextFrame();
    Touch();
}

void StartLaunch(const std::string& id) {
    if (!runningId.empty()) {
        if (runningId == id) {
            PlaySfx(Sfx::Resume);
            ResumeGame();
            return;
        }
        Game* other = FindGame(runningId);
        Ask(L"Hay otro programa abierto:\n" + (other ? other->name : L"") +
                L"\n\nCiérralo antes de iniciar este.",
            {L"Cancelar", L"Cerrar programa"}, [](int i) {
                if (i == 1) CloseGame();
            });
        return;
    }
    Game* wanted = FindGame(id);
    if (cfg.emulators.empty() && !(wanted && wanted->pc)) { // (a PC game needs none)
        PlaySfx(Sfx::Error);
        Ask(OnAndroid() ? L"No hay ningún emulador instalado.\nInstala Eden, Suyu, Lemon o Kenji-NX en este dispositivo." // [android]
                        : L"No hay ningún emulador configurado.\nIndica dónde está en Configuración de la consola.",
            {L"Cancelar", L"Abrir configuración"}, [](int i) {
                if (i == 1) OpenSettings(kNSWHomeCat);
            });
        return;
    }
    // With several users the console asks who is playing before it starts.
    if (cfg.users.size() > 1) {
        whoGame = id;
        whoSel = cfg.currentUser;
        whoAt = now;
        if (S2()) {
            // The press frame itself only starts the dimming; the sheet is
            // there one frame later.
            flow.bareAt = flow.sheetAt = NextFrame();
            flow.leaveAt = -1;
            whoAt = NextFrame() + 1.0 / 60.0;
        }
        Push(Scr::Who);
        return;
    }
    BeginPress(id);
    // No sheet: the ring gets its two frames to fade, then the picture grows.
    if (S2()) pressAt = NextFrame() + 2.0 / 60.0;
}

bool ScParentalHolds(const std::string& id); // the daily play-time limit, with the Settings rows
void CtBeforeLaunch(const std::string& gameId); // a game's updates, DLC, mods and cheats, above OpenOptions
// Playing together online (the netplay block, further down).
bool NetplayBeforeLaunch(const std::string& id);
void NetplayLaunchFailed();
void NetplayOpen(int cat);
bool NetplayWatching();

void FinishLaunch() {
    std::string id = pressId;
    pressAt = -1;
    flow.bareAt = flow.sheetAt = flow.leaveAt = -1;
    if (ScParentalHolds(id)) return;
    CtBeforeLaunch(id);
    if (!NetplayBeforeLaunch(id)) return; // an online session's game: its emulator set up first
    std::string err = LaunchGame(id);
    if (!err.empty()) {
        NetplayLaunchFailed();
        PlaySfx(Sfx::Error);
        PcLogError(id, err);
        if (err == "EMULATOR_MISSING")
            Info(OnAndroid() ? L"El emulador ya no está instalado." // [android]
                             : L"No se encuentra el emulador.\nRevisa su ubicación en Configuración de la consola.");
        else if (err == "PROGRAM_MISSING")
            Info(L"No se encuentra el programa.\nPuede que se haya movido o borrado.");
        else
            Info(L"No se ha podido iniciar el programa.\n(" + Widen(err) + L")");
        return;
    }
    launchId = id;
    launchAt = now;
    Push(Scr::Launch);
}

void OpenCloseDialog() {
    Game* g = FindGame(runningId);
    if (!g) return;
    std::wstring question = S2() ? std::wstring(L"¿Quieres cerrar el programa suspendido?")
                                 : L"¿Quieres cerrar " + g->name + L"?";
    Ask(question + L"\nSe perderán los datos no guardados.", {L"Cancelar", L"Cerrar"}, [](int i) {
        if (i != 1) return;
        if (!S2()) {
            CloseGame();
            return;
        }
        // On the console the program ends 22 frames after "close" is
        // confirmed (w5 35.22 s -> 35.58 s): the dialog and the dimming leave
        // first (9 frames), the row waits without ring or title (13 frames),
        // and only then does the "in use" mark fade.
        flow.closing = true;
        flow.closeAt = NextFrame() + 21.0 / 60.0; // drawn one frame later
        flow.bareAt = NextFrame();
        Touch();
    });
    dlg.iconGame = g->id;
}

void AskClose() {
    if (!FindGame(runningId)) return;
    if (!S2()) {
        OpenCloseDialog();
        return;
    }
    // w5 33.27 s: X first makes the tile react (it dips 6 px a side and is
    // tinted pale blue, back to normal after seven frames), the title goes on
    // the 8th frame and the dialog starts on the 11th. See DrawHome.
    if (flow.xAt < 0 && !flow.closing) flow.xAt = NextFrame();
    Touch();
}

// Y on the running game ("Cambiar de usuario"). The emulator fixes the user
// when a program starts, so changing user means closing it and starting it
// again for someone else: after the close the user sheet comes up for it.
std::string relaunchId; // start this game again once it has closed
void ChangeUser() {
    Game* g = FindGame(runningId);
    if (!g) return;
    if (cfg.users.size() < 2) {
        PlaySfx(Sfx::Bump);
        return;
    }
    PlaySfx(Sfx::Ok);
    std::string id = g->id;
    Ask(L"Para cambiar de usuario hay que cerrar el programa.\nSe perderán los datos no guardados.",
        {L"Cancelar", L"Cerrar"}, [id](int i) {
            if (i != 1) return;
            relaunchId = id;
            CloseGame();
        });
    dlg.iconGame = id;
}

void OpenOptions(const std::string& id);
void OpenAlbum();

// Test states for the system-button bar (--shot / --shotseq screen names).
// Return true when `name` was handled.
bool DebugBar(const std::wstring& name) {
    // "bar0" .. "bar9": the home menu with the focus on that bar button.
    if (name.size() == 4 && name.compare(0, 3, L"bar") == 0 && name[3] >= L'0' && name[3] <= L'9') {
        homeRow = 2;
        si = (std::min)(static_cast<int>(name[3] - L'0'), SysCount() - 1); // wchar_t is unsigned on ARM
        return true;
    }
    return false;
}

// What the bar is doing besides sitting there: the button that was pressed
// (its picture plays a short animation and holds the last frame until the menu
// has gone or the dialog has closed), and the ring the focus has just left.
struct BarPress {
    int i = -1;
    double t0 = 0;
} barPress;
struct BarGhost {
    int i = -1;
    double t0 = 0;
} barGhost;
int barFocus = -1; // the bar button that had the focus on the previous frame
bool barRing = false; // ... and whether its ring was showing (no dialog over the menu)

// The pressed pictures are 21 frames each on the console (every button has its
// own: the wheel turns, the card opens, the coloured ones end up filled in).
// They cannot be drawn from the still picture, so they come from the local
// pack (tools\pack_bar.py); without it the still picture dips instead.
constexpr int kBarFrames = 21;
Img* BarFrame(int button, int frame) {
    wchar_t name[32];
    swprintf_s(name, L"bar_%d_%02d_%s", button, (std::min)(frame, kBarFrames - 1), cfg.dark ? L"d" : L"l");
    return PackIcon(name);
}

// Without the pack the settings wheel still turns as it dips. Degrees per
// frame, measured on the capture (the wheel has eight teeth, so 45 = 0).
float GearTurn(float age) {
    static const float k[] = {3.4f, 11.7f, 22.5f, 34.6f, 44.8f, 40.0f, 30.2f, 20.0f, 19.5f, 18.5f, 16.9f,
                              14.9f, 12.5f, 10.0f, 7.5f, 5.1f, 3.1f, 1.4f, 0.4f, 0.0f};
    return FrameTable(k, 20, age);
}

// (with the settings rows' state and the choice list, further down)
std::string Cos(const std::string& id, const std::string& def);
void SetCos(const std::string& id, const std::string& v);
void Pick(const std::wstring& title, std::vector<std::wstring> items, int cur, std::function<void(int)> done);

// ---- the web page: the shop and the news inside NSW-Home ---------------------
// The bar's "Nintendo eShop" (the address the user chose) and "Noticias" open
// as a page of the menu: the console's header and footer, and between them a
// web view (webview.h: WebView2 on Windows; elsewhere there is none and the
// buttons open the system's browser as before).
//
// How the console opens them, frame by frame (anim.mkv, dark theme: the shop
// pressed at 272.80 s, the news at 275.27 s; frame 0 = the first frame after
// the press, as everywhere in this file). Both buttons behave the same:
//   0-20   the button plays its reaction (the bar's own code)
//   21-30  the menu fades out and shrinks as for any page (StartTransition)
//   30-43  nothing but the ground and the footer (14 frames)
//   44     with several users, the sheet "who is going to use ..." arrives:
//          the panel opaque at once, 13 px low, rising 13 11 9 7 5 3 2 1 0;
//          its contents fade in fast at first (0.17 0.31 0.46 0.58 0.70 0.80
//          0.89 0.95 0.98 1); the ground above it darkens to black 0.48 in
//          seven frames (0.13 0.33 0.53 0.73 0.87 0.93 1 of that).
//   B      (the next frame is 0) the sheet drops 5.6 units a frame from frame
//          1, its contents fading (0.94 0.78 0.54 0.28 0.07); the panel is at
//          0.56 on frame 5 and gone on 6; the darkening clears in frames 1-4
//          (0.73 0.47 0.27 0.07 0); the ground alone on 6-8; the menu fades
//          and grows back from frame 9 exactly as when leaving a page.
// The console's shop and news programs themselves never opened in the
// captures (the owner always backed out of the sheet), so what follows a
// pick is NSW-Home's: the sheet leaves as for B, and where the menu would
// come back the page's frame fades in as any page does (8 frames), with the
// bar's picture in its colour and a turning mark where the page will be
// until the page has something to show. With one user there is no sheet:
// the frame comes on frame 44. The first console style has the same timeline
// in its own colours and shapes, except that the fade starts at once (it has
// no 21-frame button reaction to wait for).
// Closing (B, HOME, Esc) is the console's way back from any page: 12.4
// frames still, the page fades out evenly, the menu comes back. The page
// itself is a window of the browser's and cannot fade, so a picture of it is
// taken as the closing starts and fades in its place.
struct WebPage {
    bool open = false;    // the page (or its sheet) is on the screen stack
    int action = 0;       // the bar's action: 8 the shop, 9 the news
    int icon = 2;         // the bar button's picture (SysIcon) and colour
    uint32_t color = 0;
    std::wstring label;   // the header's title, the button's name
    std::wstring url;
    bool asking = false;  // several users: the sheet comes first
    int sel = 0;          // the sheet's focus (the user count = the plus)
    double sheetAt = -1;  // the sheet's first frame
    double outAt = -1;    // the sheet started to leave (B or a pick)
    bool picked = false;
    double pageAt = -1;   // the page's frame started to fade in
    bool ownFade = false; // ... drawn by the page (after the sheet), not by the transition
    double closeAt = -1;  // leaving the page
    bool failed = false;
    Img snap;             // the page as it was when leaving
    size_t snapBytes = 0;
} webp;

constexpr float kWebSheetFrames = 9; // from the sheet's B or pick to the menu's (or page's) return
float WebTop() { return S2() ? Px(128) : 89.0f; }    // the page between the header's rule...
float WebBottom() { return S2() ? Px(968) : 647.0f; } // ... and the footer's

// The question on the sheet, as the console words it for each program.
std::wstring WebQuestion() {
    return webp.action == 9 ? L"¿Quién va a consultar las noticias?" : L"¿Quién va a utilizar Nintendo eShop?";
}

// The page's ground for the web view until it paints, as 0xRRGGBB.
uint32_t WebGround() {
    const Color c = Th().bg;
    auto b = [](float v) { return static_cast<uint32_t>(lroundf((std::max)(0.0f, (std::min)(1.0f, v)) * 255)); };
    return b(c.r) << 16 | b(c.g) << 8 | b(c.b);
}

// Opens the page for the bar's action 8 or 9 at `url`: false when there is no
// web view on this system (the caller opens the browser instead).
// `afterPress`: the bar button is playing its 21-frame reaction first.
bool WebGo(int action, const std::wstring& url, bool afterPress) {
    if (!web::Available() || url.empty()) return false;
    webp.snap.reset();
    webp = WebPage();
    webp.action = action;
    webp.url = url;
    for (const SysBtn& b : SysList())
        if (b.action == action) {
            webp.icon = b.icon;
            webp.color = b.color;
            webp.label = b.label;
        }
    if (webp.label.empty()) webp.label = action == 9 ? L"Noticias" : L"Nintendo eShop";
    webp.asking = cfg.users.size() > 1;
    webp.sel = (std::min)(cfg.currentUser, static_cast<int>(cfg.users.size()) - 1);
    web::Prepare(); // the browser starts while the menu fades and the sheet is up
    StartTransition(true, [] {
        stack.push_back(Scr::Web);
        webp.open = true;
        if (webp.asking) {
            webp.sheetAt = now;
        } else {
            webp.pageAt = now;
            web::Open(webp.url, cfg.users[webp.sel].id, WebGround());
        }
    });
    if (!afterPress || !S2()) trans.outAt = 0;
    trans.blank = 14 * kFrame;
    if (webp.asking) trans.inDur = 0; // the sheet arrives on its own timeline, opaque
    return true;
}

// The small pictures in front of the sheet's question (anim.mkv 273.5 s and
// 276.2 s, pixel-measured; one unit = 1.5 pixels): the shop's bag and the
// news' folded page, drawn as outlines three pixels wide. (x, y) is the top
// left corner of the picture's box, 25 units across.
void WebSheetIcon(float x, float y, Color c) {
    const float sw = Px(3);
    if (webp.action == 9) {
        RRectStroke(x + Px(11.5f), y + Px(4.5f), Px(26), Px(33), Px(3), c, sw); // the page
        RRectStroke(x + Px(1.5f), y + Px(20.5f), Px(10), Px(17), Px(4), c, sw); // the fold at its foot, left
        for (int i = 0; i < 3; i++) Rect(x + Px(17), y + Px(12 + i * 8.0f), Px(16), Px(2), c);
        return;
    }
    // The bag widens a little towards its foot (34 px across the top, 38 at the foot).
    Rect(x + Px(3), y + Px(9), Px(35), sw, c);
    Rect(x + Px(1), y + Px(33.5f), Px(39), sw, c);
    Line(x + Px(4.5f), y + Px(9), x + Px(2.5f), y + Px(36.5f), c, sw);
    Line(x + Px(36.5f), y + Px(9), x + Px(38.5f), y + Px(36.5f), c, sw);
    PushClip(x, y, Px(40), Px(18)); // the handle goes into the bag to its third row
    RRectStroke(x + Px(13.5f), y + Px(2.5f), Px(13), Px(30), Px(4), c, sw);
    PopClip();
}

void DrawWebSheet() {
    Theme T = Th();
    const int n = static_cast<int>(cfg.users.size());
    const float m = FramesSince(webp.sheetAt);
    const bool leaving = webp.outAt >= 0;
    const float out = leaving ? FramesSince(webp.outAt) : 0;
    Touch();
    if (out >= 5.5f) { // gone: the ground alone until the menu (or the page) comes
        Footer({});
        return;
    }
    if (!S2()) {
        // The first console's own sheet, as its game sheet: centred, plain.
        float a = leaving ? (std::max)(0.0f, 1 - out / 5) : (std::min)(1.0f, (m + 1) / 6);
        if (a < 1) PushLayer(a);
        Text(WebQuestion(), 0, 150, W, 50, 30, T.text, Center);
        float step = 150, x0 = W / 2 - (n - 1) * step / 2;
        for (int i = 0; i < n; i++) {
            float cx = x0 + i * step, cy = 330;
            DrawAvatar(i, cx, cy, 56);
            if (i == webp.sel) {
                CircleCursor(cx, cy, 63);
                Text(cfg.users[i].name, cx - 200, 408, 400, 40, 24, T.title, Center);
            }
            if (!leaving)
                AddHit(cx - 60, cy - 60, 120, 120, [i] {
                    webp.sel = i;
                    OnButtonImpl(Btn::A);
                });
        }
        if (a < 1) PopLayer();
        Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Aceptar", Btn::A}});
        return;
    }
    // The ground above the sheet darkens (black 0.48 measured in the dark
    // theme; the light theme's was never captured with this sheet, so it
    // takes the game sheet's 0.336 from w5).
    static const float dimIn[] = {0.13f, 0.33f, 0.53f, 0.73f, 0.87f, 0.93f, 1};
    static const float dimOut[] = {1, 0.73f, 0.47f, 0.27f, 0.067f, 0};
    const float dim = (leaving ? Lerp(dimOut, 6, out) : Lerp(dimIn, 7, m)) * (cfg.dark ? 0.484f : 0.336f);
    if (dim > 0) Rect(0, 0, W, H, rgb(0x000000, dim));
    static const float rise[] = {13, 11, 9, 7, 5, 3, 2, 1, 0};
    static const float inkIn[] = {0.17f, 0.31f, 0.46f, 0.58f, 0.70f, 0.80f, 0.89f, 0.95f, 0.98f, 1};
    static const float inkOut[] = {1, 0.94f, 0.78f, 0.54f, 0.28f, 0.07f, 0};
    const float top = 300 + Lerp(rise, 9, m) / 1.5f + (std::min)(out, 5.0f) * 5.6f;
    const float a = leaving ? Lerp(inkOut, 7, out) : Lerp(inkIn, 10, m);
    const float panel = out < 4.5f ? 1.0f : 0.56f;
    if (panel < 1) PushLayer(panel);
    RRect(0, top, W, 500, 22, T.dialog);
    if (a > 0) {
        if (a < 1) PushLayer(a);
        // Wording 56 px right of the game sheet's (176 against 120), the
        // picture in front of it from pixel column 124, rows 506-544.
        WebSheetIcon(Px(123), top + Px(55), T.text);
        Text(WebQuestion(), 80 + Px(56), top + 31 - Px(3.5f), 900, 44, 24.15f, T.text); // 614 px wide there
        const int shown = (std::min)(n, 7);
        for (int i = 0; i < shown; i++) {
            float cx = Px(268) + i * Px(212), cy = top + Px(274);
            Circle(cx, cy, Px(96), T.avRing);
            DrawAvatar(i, cx, cy, Px(90));
            if (leaving && webp.picked && i == webp.sel) Circle(cx, cy, Px(90), rgb(0x8BAFFF, 0.36f));
            if (i == webp.sel && !leaving) CircleCursor(cx, cy, Px(98.5f));
            Text(cfg.users[i].name, cx - 70, top + 257, 140, 42, 23.4f, T.text, Center);
            if (!leaving)
                AddHit(cx - 64, cy - 64, 128, 128, [i] {
                    webp.sel = i;
                    OnButtonImpl(Btn::A);
                });
        }
        const float px = Px(268) + shown * Px(212) - Px(20), py = top + Px(274);
        CircleStroke(px, py, Px(41), T.sub, Px(2));
        Rect(px - Px(20), py - Px(3.5f), Px(40), Px(7), T.sub);
        Rect(px - Px(3.5f), py - Px(20), Px(7), Px(40), T.sub);
        if (webp.sel >= shown && !leaving) CircleCursor(px, py, 31);
        if (!leaving)
            AddHit(px - 30, py - 30, 60, 60, [shown] {
                webp.sel = shown;
                OnButtonImpl(Btn::A);
            });
        if (a < 1) PopLayer();
    }
    Rect(0, 646, W, H - 646, T.dialog);
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}});
    if (panel < 1) PopLayer();
}

// The page's host name, for the header (where the page is, as a browser says).
std::wstring WebHost(const std::wstring& u) {
    size_t s = u.find(L"://");
    if (s == std::wstring::npos) return {};
    s += 3;
    size_t e = u.find_first_of(L"/?#", s);
    std::wstring h = u.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
    if (h.rfind(L"www.", 0) == 0) h = h.substr(4);
    return h;
}

// A turning mark (the page is loading).
void WebSpinner(float cx, float cy, float r, Color c) {
    const float spin = static_cast<float>(fmod(now * 360.0, 360.0));
    CircleStroke(cx, cy, r, alpha(c, 0.25f), r * 0.22f);
    Arc(cx, cy, r, spin, spin + 110, c, r * 0.22f);
    Touch();
}

void DrawWebPage() {
    Theme T = Th();
    const bool s2 = S2();
    float a = 1;
    if (webp.ownFade) {
        static const float pageIn[] = {1, 0.82f, 0.555f, 0.31f, 0.185f, 0.085f, 0.05f, 0.015f, 0}; // as any page (StartTransition)
        a = 1 - Lerp(pageIn, 9, FramesSince(webp.pageAt));
        if (a < 1) Touch();
    }
    if (a <= 0) return;
    if (a < 1) PushLayer(a);
    const Color ic = webp.color ? rgb(webp.color) : T.icon;
    A11yTitle(webp.label);
    float hostRight = s2 ? 1232.0f : 1250.0f;
    if (s2) {
        PushScale(1.15f, 98, 50);
        SysIcon(webp.icon, 98, 50, ic);
        PopTransform();
        Text(webp.label, 123, 28, 700, 46, 28.6f, T.text);
        HRule(48, 1232, Px(127), T.line);
    } else {
        SysIcon(webp.icon, 76, 46, ic);
        Text(webp.label, 112, 20, 700, 52, 28, T.text);
        Line(30, 88.5f, 1250, 88.5f, T.line, 1);
    }
    // Where the page is, at the right of the header, with the turning mark
    // while it loads.
    const std::wstring host = WebHost(web::Url().empty() ? webp.url : web::Url());
    const float hy = s2 ? 51.0f : 46.0f;
    float tw = host.empty() ? 0 : (std::min)(420.0f, TextWidth(host, 20));
    if (!host.empty()) Text(host, hostRight - tw, hy - 18, tw + 4, 36, 20, T.sub);
    const web::State st = web::Now();
    if (web::Loading() || st == web::State::Starting || st == web::State::Loading)
        WebSpinner(hostRight - tw - (host.empty() ? 10.0f : 24.0f), hy, 9, T.sub);

    // The page's place: the picture taken on leaving, or (until the page has
    // something to show, and in offscreen runs, which have no view) the
    // bar's picture in its colour over a turning mark.
    const float y0 = WebTop(), y1 = WebBottom();
    if (webp.closeAt >= 0 && webp.snapBytes) {
        Image(webp.snap, 0, y0, W, y1 - y0);
    } else if (!web::Shown()) {
        const float cy = (y0 + y1) / 2 - 18;
        PushScale(3.0f, W / 2, cy);
        SysIcon(webp.icon, W / 2, cy, ic);
        PopTransform();
        if (!webp.failed && webp.closeAt < 0) WebSpinner(W / 2, cy + 92, 14, T.sub);
    }
    if (a < 1) PopLayer();

    std::vector<Hint> hints;
    if (web::CanBack()) hints.push_back({L"L", L"Atrás", Btn::L});
    if (web::CanForward()) hints.push_back({L"R", L"Adelante", Btn::R});
    hints.push_back({L"Y", L"Recargar", Btn::Y});
    hints.push_back({L"B", L"Cerrar", Btn::B});
    Footer(hints);
}

void DrawWeb() {
    const bool sheet = webp.sheetAt >= 0 && (webp.pageAt < 0 || now < webp.pageAt);
    if (sheet) DrawWebSheet();
    else DrawWebPage();
}

// The page as it was when leaving: the picture asked for by WebLeave, once it
// has arrived, replaces the view for the fade.
void WebTakeSnapshot() {
    const std::vector<uint8_t>& png = web::SnapshotPng();
    if (png.empty() || webp.snapBytes) return;
    webp.snap.reset();
    webp.snap.path.clear();
    webp.snap.mem = png;
    webp.snapBytes = png.size();
}

void WebLeave(bool home) {
    if (trans.on || webp.closeAt >= 0) return;
    PlaySfx(home ? Sfx::Home : Sfx::Back);
    web::Snapshot();
    webp.closeAt = now;
    Pop();
}

// Every frame, after the screens have drawn: the view follows the page, hides
// under a dialog or while leaving, and goes away with the page.
void WebFrame() {
    const bool inStack = std::find(stack.begin(), stack.end(), Scr::Web) != stack.end();
    if (!inStack) {
        if (webp.open) {
            web::Close();
            webp.open = false;
            webp.snap.reset();
            webp.snap.mem.clear();
            webp.snapBytes = 0;
        }
        return;
    }
    const web::State st = web::Now();
    const bool top = stack.back() == Scr::Web;
    const bool framed = webp.pageAt >= 0 && FramesSince(webp.pageAt) >= (webp.ownFade ? 8.0f : 0.0f) && !trans.on;
    if (st == web::State::Failed && !webp.failed && webp.closeAt < 0) {
        // The browser could not start, or went away: the page closes and, as
        // before there was one, the system's browser gets the address.
        webp.failed = true;
        web::Show(false);
        const std::wstring u = web::Url().empty() ? webp.url : web::Url();
        webp.closeAt = now;
        Pop();
        if (!OpenUrl(u)) Info(L"No se ha podido abrir la página.");
        return;
    }
    if (web::FullScreen()) web::Place(0, 0, W, H);
    else web::Place(0, WebTop(), W, WebBottom() - WebTop());
    bool show;
    if (webp.closeAt >= 0) {
        WebTakeSnapshot();
        // Kept until its picture has arrived or the fade begins.
        show = web::Shown() && !webp.snapBytes && coverNow <= 0;
    } else {
        // A page that never says it is ready is shown anyway after a while:
        // what has arrived of it is better than the turning mark.
        const bool ready = st == web::State::Ready || (st == web::State::Loading && FramesSince(webp.pageAt) > 8 * 60);
        show = top && framed && ready && !dlg.open && !entry.open;
    }
    web::Show(show);
}

// HOME: on the page it closes as B does; on the sheet the menu's own HOME
// (a cut back to the menu) does.
bool WebHome() {
    if (stack.back() != Scr::Web || webp.pageAt < 0 || now < webp.pageAt) return false;
    if (!trans.on) WebLeave(true);
    return true;
}

void WebButton(Btn b) {
    const bool sheet = webp.sheetAt >= 0 && webp.pageAt < 0;
    if (sheet) {
        const int n = static_cast<int>(cfg.users.size());
        const int last = n + (S2() ? 1 : 0) - 1; // the second console's sheet ends in the plus
        if (webp.outAt >= 0) return;            // on its way out
        if (b == Btn::Left || b == Btn::Right) {
            const int to = webp.sel + (b == Btn::Left ? -1 : 1);
            if (to < 0 || to > last) {
                PlaySfx(Sfx::Bump);
                return;
            }
            webp.sel = to;
            PlaySfx(Sfx::UserMove);
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            webp.outAt = NextFrame();
            StartTransition(false, [] {
                if (stack.size() > 1) stack.pop_back();
            });
            trans.outAt = kWebSheetFrames * kFrame;
            trans.outDur = trans.blank = 0;
        } else if (b == Btn::A && webp.sel >= n) {
            // The plus: a new user, then back to this sheet with them chosen.
            PlaySfx(Sfx::Ok);
            PopNow();
            webp.open = false; // (the frame code must not take this for the page closing)
            StartAddUser([](int i) {
                if (i >= 0) webp.sel = i;
                webp.sheetAt = now;
                webp.open = true;
                PushNow(Scr::Web);
            });
        } else if (b == Btn::A) {
            PlaySfx(Sfx::UserPick);
            webp.picked = true;
            webp.outAt = NextFrame();
            webp.pageAt = webp.outAt + kWebSheetFrames * kFrame;
            webp.ownFade = true;
            web::Open(webp.url, cfg.users[webp.sel].id, WebGround());
        }
        return;
    }
    if (webp.closeAt >= 0) return;
    switch (b) {
    case Btn::B: WebLeave(false); break;
    case Btn::A: web::Click(); break;
    case Btn::Up: web::Scroll(0, 1); break;
    case Btn::Down: web::Scroll(0, -1); break;
    case Btn::Left: web::Scroll(-1, 0); break;
    case Btn::Right: web::Scroll(1, 0); break;
    case Btn::L:
        if (web::CanBack()) web::Back();
        else PlaySfx(Sfx::Bump);
        break;
    case Btn::R:
        if (web::CanForward()) web::Forward();
        else PlaySfx(Sfx::Bump);
        break;
    case Btn::Y: web::Reload(); break;
    default: break;
    }
}

// Test states (--shot / --shotseq):
//   web, web-news         the page settled (offscreen: no view, the placeholder)
//   web-who, web-who-news the sheet settled
//   web-bar, web-bar-news the menu with the focus on that bar button and a
//                         test address for the shop, so that A opens it
//   ...-one               any of them with only the first user (no sheet)
//   ...-s1, ...-s1l       in the first console's style, dark / light
bool DebugWeb(const std::wstring& name) {
    if (name.rfind(L"web", 0) != 0) return false;
    std::wstring base = name;
    for (const wchar_t* style : {L"-s1", L"-s1l"}) {
        const size_t k = wcslen(style);
        if (base.size() > k && base.compare(base.size() - k, k, style) == 0) {
            base.resize(base.size() - k);
            cfg.menuStyle = 1;
            cfg.dark = k == 3;
        }
    }
    if (base.size() > 4 && base.compare(base.size() - 4, 4, L"-one") == 0) {
        base.resize(base.size() - 4);
        cfg.users.resize(1); // (in memory: a test run saves nothing)
        cfg.currentUser = 0;
    }
    const bool news = base.size() > 5 && base.compare(base.size() - 5, 5, L"-news") == 0;
    if (news) base.resize(base.size() - 5);
    const int action = news ? 9 : 8;
    cfg.console["dockside.shop_url"] = "https://www.example.com/"; // in memory only: nothing is saved
    const std::wstring url = news ? L"https://www.nintendo.com/es-es/Noticias/" : L"https://www.example.com/";
    if (base == L"web-bar") {
        homeRow = 2;
        for (int i = 0; i < SysCount(); i++)
            if (SysList()[i].action == action) si = i;
        return true;
    }
    if (base != L"web" && base != L"web-who") return false;
    if (!WebGo(action, url, false)) {
        // No web view on this system: the page is drawn all the same.
        webp = WebPage();
        webp.action = action;
        webp.url = url;
        webp.label = news ? L"Noticias" : L"Nintendo eShop";
        webp.icon = news ? 1 : 2;
        webp.color = news ? 0x24A25C : 0xD44639;
    }
    trans = Transition();
    stack = {Scr::Home, Scr::Web};
    webp.open = true;
    if (base == L"web-who") {
        webp.sheetAt = now; // (settled by the time a --shot is taken)
        webp.asking = true;
    } else {
        webp.pageAt = now;
        web::Open(webp.url, cfg.users[0].id, WebGround());
    }
    return true;
}

// The shop's web address, typed in the text entry. `thenOpen`: open it once
// given (the first press of the shop button). An address without its scheme
// gets "https://"; anything that is not a web address is refused.
void AskShopUrl(bool thenOpen) {
    entry.open = true;
    entry.max = 200;
    entry.title = L"Dirección de la tienda";
    entry.text = Widen(Cos("dockside.shop_url", ""));
    entry.done = [thenOpen](std::wstring s) {
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
        if (s.empty()) {
            SetCos("dockside.shop_url", "");
            return;
        }
        std::wstring low = s;
        for (wchar_t& c : low) c = static_cast<wchar_t>(towlower(c));
        if (low.rfind(L"http://", 0) != 0 && low.rfind(L"https://", 0) != 0) {
            if (low.find(L"://") != std::wstring::npos || low.find(L'.') == std::wstring::npos || low.find(L' ') != std::wstring::npos) {
                Info(L"Eso no es una dirección web.\nEscribe algo como www.ejemplo.com.");
                return;
            }
            s = L"https://" + s;
        }
        SetCos("dockside.shop_url", Narrow(s));
        if (thenOpen && !WebGo(8, s, false) && !OpenUrl(s)) Info(L"No se ha podido abrir el navegador.");
    };
}

// The library's folder, from the virtual game cards' screen (Y): a dialog,
// since the choice list is only drawn on the list pages. Open it, or choose
// another (it takes the first place; other folders stay, and are managed in
// Settings > NSW-Home).
void GameFolderMenu() {
    auto change = [] {
        const std::wstring dir = PickFolder();
        if (dir.empty()) return;
        for (size_t k = 0; k < cfg.gameDirs.size(); k++)
            if (!_wcsicmp(cfg.gameDirs[k].c_str(), dir.c_str())) cfg.gameDirs.erase(cfg.gameDirs.begin() + k--);
        cfg.gameDirs.insert(cfg.gameDirs.begin(), dir);
        SaveConfig();
        RescanAsync();
    };
    if (cfg.gameDirs.empty()) {
        Ask(L"Todavía no has elegido la carpeta de tus juegos.", {L"Cancelar", L"Elegir carpeta"}, [change](int i) {
            if (i == 1) change();
        });
        return;
    }
    Ask(L"Carpeta de juegos:\n" + cfg.gameDirs[0], {L"Cancelar", L"Cambiar carpeta", L"Abrir carpeta"}, [change](int i) {
        if (i == 1) change();
        if (i == 2 && (cfg.gameDirs.empty() || !OpenFolder(cfg.gameDirs[0])))
            Info(L"No se ha podido abrir la carpeta de juegos.");
    });
}

void SysAction(int i) {
    if (trans.on) return; // a click during a screen change
    const SysBtn& b = SysList()[i];
    const bool s2 = S2();
    PlaySfx(b.action == 3 ? Sfx::Album : b.action == 4 ? Sfx::Pads : b.action == 5 ? Sfx::Settings : Sfx::Ok);
    if (s2) {
        barPress.i = i;
        barPress.t0 = ReactionStart();
        Touch();
    }
    switch (b.action) {
    case 0: {
        std::wstring msg = std::wstring(b.label) + L" no está disponible.\nNSW-Home solo inicia tus propios juegos.";
        // The console leaves the menu 21 frames after the press, once the
        // button has played its animation; the notice waits as long.
        if (s2) StartDelay(21 * kFrame, [msg] { Info(msg); });
        else Info(msg);
        break;
    }
    case 3: OpenAlbum(); break;
    case 4: Push(Scr::Pads); break;
    case 5: OpenSettings(0); break;
    case 11: NetplayOpen(0); break;
    case 6: {
        auto ask = [] {
            Ask(L"¿Qué quieres hacer?", {L"Cancelar", L"Modo de espera", L"Salir de NSW-Home"}, [](int choice) {
                if (choice == 1) {
                    PlaySfx(Sfx::Sleep);
                    Push(Scr::Sleep);
                }
                if (choice == 2) QuitApp();
            });
        };
        // Measured: the focus leaves the power button on frame 15 after the
        // press (its label goes), and the dialog starts to show on frame 17.
        if (s2) StartDelay(15 * kFrame, ask);
        else ask();
        break;
    }
    case 7:
        allSel = 0;
        Push(Scr::All);
        break;
    case 8: {
        // The shop: a web page of the user's own choosing, in the browser. The
        // first time, the address is asked for; it is changed in Settings >
        // NSW-Home ("Página de la tienda"). Inside NSW-Home when it can show
        // a web page (see "the web page").
        if (!Cos("dockside.shop_url", "").empty() && WebGo(8, Widen(Cos("dockside.shop_url", "")), s2)) break;
        auto open = [] {
            const std::wstring url = Widen(Cos("dockside.shop_url", ""));
            if (url.empty()) AskShopUrl(true);
            else if (!OpenUrl(url)) Info(L"No se ha podido abrir el navegador.");
        };
        if (s2) StartDelay(21 * kFrame, open);
        else open();
        break;
    }
    case 9: {
        // The news: the console maker's own news page, inside NSW-Home or in the browser.
        if (WebGo(9, L"https://www.nintendo.com/es-es/Noticias/", s2)) break;
        auto open = [] {
            if (!OpenUrl(lang::Cur().news)) // the regional page of the menu's language
                Info(L"No se ha podido abrir el navegador.");
        };
        if (s2) StartDelay(21 * kFrame, open);
        else open();
        break;
    }
    case 10: {
        // GameChat: talking, streaming and recording are OBS Studio's on a PC,
        // when it is installed.
        auto open = [] {
            const int r = OpenObs();
            if (r == 0)
                Info(L"Para usar GameChat, instala OBS Studio.\n"
                     L"NSW-Home lo abrirá desde aquí cuando esté en este PC.");
            else if (r < 0)
                Info(L"No se ha podido abrir OBS Studio.");
        };
        if (s2) StartDelay(21 * kFrame, open);
        else open();
        break;
    }
    }
}

// Corner radius of a game tile: square on the first console's menu, rounded on
// the second's (w5: the picture's corner is 18 px on a 384 px tile, and the
// ring around it 30 px outside, the same centre).
float TileRadius(float size) {
    return S2() ? size * 0.047f : 0.0f;
}

void DrawTile(Game& g, float x, float y, float size) {
    Theme T = Th();
    float r = TileRadius(size);
    // The cell colour is for a picture that is not there (yet). Under a picture
    // it would show through the soft edge of the rounded corners as a pale rim.
    if (!S2() || g.icon.empty() || g.icon.failed || !g.icon.bmp) RRect(x, y, size, size, r, T.tile);
    if (!g.icon.empty()) Image(g.icon, x, y, size, size, 1, 0, r);
    if (g.icon.empty() || g.icon.failed)
        TextWrap(g.name, x + 14, y + size * 0.36f, size - 28, size * 0.5f, size * 0.085f, T.text, Center, 600);
}

// The band between a focused tile's picture and its ring: black in the dark
// theme, near white in the light one (w5 2.67 s: 240 237 239 where the page
// reads 220; the "in use" mark has the same edge).
Color TileFrame() {
    return cfg.dark ? rgb(0x000000) : rgb(0xF0EDF0);
}

// The mark on a running game's tile: the player's picture and an "in use"
// pill across the foot of the tile. Measured on w5 at 32.9 s, in pixels from
// the tile's corner: pill 254 x 80 at (108, 283) with a light edge 4 wide,
// picture centred on (62.5, 322.5), 83 across including its edge of 4.
// "En uso" is 65.6 units wide, which is size 22.8 here.
void DrawRunningMark(float x, float y, float a) {
    if (a <= 0) return;
    Color edge = rgb(0xF0EDF0); // w5 32.9 s: 240 237 240
    if (a < 1) PushLayer(a);
    Circle(x + Px(62.5f), y + Px(322.5f), Px(41.5f), edge);
    if (cfg.currentUser >= 0 && cfg.currentUser < static_cast<int>(cfg.users.size()))
        DrawAvatar(cfg.currentUser, x + Px(62.5f), y + Px(322.5f), Px(37.5f));
    RRect(x + Px(108), y + Px(283), Px(254), Px(80), Px(40), edge);
    RRect(x + Px(112), y + Px(287), Px(246), Px(72), Px(36), rgb(0x000000));
    Text(L"En uso", x + 72.3f, y + 188.7f, 168.7f, 53, 22.8f, rgb(0xEBE6EA), Center);
    if (a < 1) PopLayer();
}

// Moving along the row, measured on walkthrough.mp4 96.8-99.6 s (seven steps,
// all alike) and bounce.mkv. The row slides one tile (406 px) in
// 108 92 75 58 41 25 9 px per frame, and under a held direction at a steady
// 108 px a frame until it stops dead on its mark: it runs at no more than 72
// units a frame and, for a single press, brakes at 10.93 units a frame, each
// frame, so as to stop on its mark. The ring is at full
// strength on the new tile at once; on the tile it left it shows two more
// frames (0.92, 0.66), moving with that tile. The title is replaced at once
// and rides with its tile.
// While a direction is held the row has no title (bounce.mkv frames 270-307,
// 452-489, 876-919...): it goes with the first repeat and is back two frames
// after the row has come to rest (308, 490, 920, 1076).
// A held direction that reaches either end, or finds the focus resting there,
// sounds the limit and knocks the ring, and nothing but the ring, 10 px that
// way; it springs back within ten frames (bounce.mkv frames 348, 534, 920,
// 1076, 1227, 1682: every time the same, on the first game and on the round
// button alike). Tiles, the black band under the ring, title and everything
// else on the screen stay where they are. It happens once: the direction can
// stay held for seconds and neither the ring nor the sound comes again. When
// the row is still sliding the knock waits for it: first frame on the mark,
// one more at rest, then the knock (918/920, 1074/1076, 1225/1227, 1680/1682,
// 532/534).
// A new press (not a repeat) towards the end the focus is already on goes
// round to the other end (bounce.mkv frames 588, 634, 702) with the ordinary
// move sound. The row slides there in eight frames, every tile in between
// passing by: 1195 898 644 431 262 134 50 7 px of 3621, which leaves
// ((7 - n) / 8) cubed of the way to go on frame n. The ring is on the new item
// at once and rides in with it, as does the round button's name; a game's
// title is back on the frame after the row has stopped (596, 710).
struct RowFx {
    float scrollTo = 0;            // where the row is heading
    float lastScroll = 0;          // homeScroll as this code last left it
    int lastGi = -1, lastRow = -1; // what was focused on the previous frame
    int fromGi = -1;               // the tile the ring has just left
    double moveAt = -1;
    bool run = false;   // the last move came from a held direction: no braking
    double kickAt = -1; // the ring is being knocked towards the end of the row
    float kickDir = 0;
    bool kickWant = false; // a knock is due as soon as the row has come to rest
    bool endDone = false;  // this hold has had its knock: no more until a new press
    double restAt = -100;  // the first frame that showed the row on its mark
    double heldAt = -1;    // the last move made by a held direction; -1 when the title is back
    double wrapAt = -1;    // the row is going round to its other end
    float wrapFrom = 0;    // homeScroll when it set off
} rowFx;

void RowKnock(int dir) {
    rowFx.endDone = rowFx.kickWant = true;
    rowFx.kickDir = static_cast<float>(dir);
    PlaySfx(Sfx::Bump);
    Touch();
}

// Left or Right on the row of games in the second console's style. True when
// the press has been dealt with here.
bool RowHeld(int dir, int count) {
    if (!S2() || count <= 0) return false;
    if (!heldRepeat) {
        // A new press: the hold, if there was one, is over.
        rowFx.endDone = rowFx.kickWant = false;
        rowFx.kickAt = rowFx.heldAt = -1;
        if (count < 2 || gi != (dir < 0 ? 0 : count - 1)) return false;
        gi = dir < 0 ? count - 1 : 0;
        rowFx.run = false;
        rowFx.wrapAt = NextFrame();
        rowFx.wrapFrom = homeScroll;
        PlaySfx(Sfx::MoveGame);
        Touch();
        return true;
    }
    int to = gi + dir;
    if (to < 0 || to >= count) {
        if (!rowFx.endDone) RowKnock(dir);
        return true;
    }
    rowFx.heldAt = NextFrame();
    rowFx.endDone = false;
    if (to != 0 && to != count - 1) return false;
    // Onto the last item: the console sounds the limit in place of the tick
    // (bounce.mkv, sound at 15.23 s, 17.83 s, 20.35 s, 27.93 s) and knocks
    // the ring when the row gets there.
    gi = to;
    rowFx.run = true;
    if (homeScroll == rowFx.scrollTo) rowFx.restAt = NextFrame(); // no slide to wait for
    RowKnock(dir);
    return true;
}

// Offset of the ring while it is knocked, in units.
float RowKick() {
    if (rowFx.kickAt < 0) return 0;
    static const float px[] = {10, 2, -3, -2, 0, 1, 2, 1, 0, -1, 0};
    float n = FramesSince(rowFx.kickAt);
    if (n >= 10) {
        rowFx.kickAt = -1;
        return 0;
    }
    Touch();
    return rowFx.kickDir * Lerp(px, 11, n) / 1.5f;
}

// The picture that grows out of the tile when its game starts, per frame from
// the frame before it first shows. Fitted on w5 19.08-19.55 s by laying a
// scaled copy of the tile over the menu: scale about the tile's centre, and
// opacity of the copy (the console blends in linear light).
const float kGhostScale[] = {1.00f, 1.21f, 1.31f, 1.38f, 1.43f, 1.45f, 1.47f, 1.48f, 1.49f, 1.50f,
                             1.51f, 1.52f, 1.52f, 1.53f, 1.53f, 1.54f, 1.55f, 1.55f, 1.55f, 1.56f,
                             1.56f, 1.56f, 1.56f, 1.56f, 1.57f, 1.57f, 1.57f, 1.57f, 1.57f, 1.57f};
const float kGhostAlpha[] = {0.000f, 0.228f, 0.236f, 0.236f, 0.230f, 0.224f, 0.218f, 0.210f, 0.205f, 0.192f,
                             0.180f, 0.170f, 0.161f, 0.146f, 0.132f, 0.119f, 0.106f, 0.098f, 0.082f, 0.072f,
                             0.061f, 0.047f, 0.040f, 0.031f, 0.022f, 0.015f, 0.009f, 0.005f, 0.000f, 0.000f};

// ---- first console's system bar ------------------------------------------
// The first console's bar is its own design, not the second's pictures on a
// disc: line drawings in the button's colour on a round white (dark theme:
// dark grey) disc with a hairline edge and a soft shadow. Geometry measured on
// ref\home.png (a 421 px wide still of that menu, scaled 3.04 to layout units):
// discs 23 px across (70 units), centres 29.6 px apart (90 units, SysX), the
// row centred on pixel row 178 (544 units); pictures about 11 px wide (33
// units) in strokes about a pixel thick (2.6 units). Colours follow the owner's
// description of an early firmware (News red, eShop orange; the later still
// in ref has a green News and a red eShop); the greys are read off ref.
constexpr float kS1DiscR = 35.0f;
constexpr float kS1Stroke = 2.6f;

// A stroke with round ends, so that the polylines below join without notches
// (gfx's Line has flat caps).
void S1Seg(float x0, float y0, float x1, float y1, Color c, float sw = kS1Stroke) {
    Line(x0, y0, x1, y1, c, sw);
    Circle(x0, y0, sw / 2, c);
    Circle(x1, y1, sw / 2, c);
}

// index: the button's place in the first console's full bar (SysBtn::index).
// `disc` is the ground the picture sits on (the News tail cuts the outline).
void S1BarIcon(int index, float cx, float cy, Color c, Color disc) {
    const float sw = kS1Stroke;
    switch (index) {
    case 0: { // Switch Online: the console's logo is not drawn (asset policy); a globe over the word
        CircleStroke(cx, cy - 6, 9, c, 2.2f);
        Line(cx - 9, cy - 6, cx + 9, cy - 6, c, 1.8f);
        Arc(cx - 11, cy - 6, 14, -40, 40, c, 1.8f);
        Arc(cx + 11, cy - 6, 14, 140, 220, c, 1.8f);
        Text(L"ONLINE", cx - 30, cy + 5, 60, 14, 9, c, Center, 700);
        break;
    }
    case 1: { // News: a speech bubble with its tail at the lower left and two lines of text
        RRectStroke(cx - 16, cy - 13, 32, 22, 3, c, sw);
        Rect(cx - 8.6f, cy + 9 - sw, 5.4f, sw * 2, disc); // the bubble opens into its tail
        S1Seg(cx - 9, cy + 9, cx - 9, cy + 15, c);
        S1Seg(cx - 9, cy + 15, cx - 3, cy + 9, c);
        S1Seg(cx - 9, cy - 5, cx + 9, cy - 5, c);
        S1Seg(cx - 9, cy + 1, cx + 3, cy + 1, c);
        break;
    }
    case 2: // Nintendo eShop: a shopping bag, handle on top
        RRectStroke(cx - 13, cy - 6, 26, 20, 2.5f, c, sw);
        S1Seg(cx - 6, cy - 6, cx - 6, cy - 9, c);
        S1Seg(cx + 6, cy - 6, cx + 6, cy - 9, c);
        Arc(cx, cy - 9, 6, 180, 360, c, sw);
        break;
    case 3: // Album: a frame with a landscape line rising from the lower left and falling to the right
        RRectStroke(cx - 17, cy - 12, 34, 24, 2.5f, c, sw);
        S1Seg(cx - 11, cy + 12, cx - 11, cy - 2, c);
        Arc(cx - 7, cy - 2, 4, 180, 270, c, sw);
        S1Seg(cx - 7, cy - 6, cx - 3, cy - 6, c);
        S1Seg(cx - 3, cy - 6, cx + 11, cy + 12, c);
        break;
    case 4: // Controllers: one controller half lying at 45 degrees, stick at the lower end
        PushRotate(-45, cx, cy);
        RRectStroke(cx - 20, cy - 7.5f, 40, 15, 7.5f, c, sw);
        CircleStroke(cx - 10.5f, cy, 3.4f, c, 2.0f);
        Circle(cx + 4, cy, 1.5f, c);
        Circle(cx + 10, cy, 1.5f, c);
        Circle(cx + 7, cy - 3, 1.5f, c);
        Circle(cx + 7, cy + 3, 1.5f, c);
        PopTransform();
        break;
    case 5: // System Settings: a ring with eight short teeth, like a sun
        CircleStroke(cx, cy, 8, c, sw);
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7853982f, ca = cosf(a), sa = sinf(a);
            Triangle(cx + ca * 9.5f - sa * 2.8f, cy + sa * 9.5f + ca * 2.8f, cx + ca * 9.5f + sa * 2.8f, cy + sa * 9.5f - ca * 2.8f,
                     cx + ca * 13.5f, cy + sa * 13.5f, c);
        }
        break;
    case 6: // Sleep Mode: the power symbol
        Arc(cx, cy + 1, 12, -58, 238, c, sw);
        S1Seg(cx, cy - 14, cx, cy - 2, c);
        break;
    }
}

// One button of the first console's bar: shadow, disc, picture and, when it
// has the focus, the ring and its name under it.
void DrawS1SysButton(const SysBtn& b, float cx, float cy, bool sel) {
    Theme T = Th();
    bool hc = A11yContrast();
    bool online = b.index == 0;
    // A soft shadow: three widening rings, a little below the disc.
    for (int k = 3; k >= 1; k--) Circle(cx, cy + 1.5f, kS1DiscR + k * 1.0f, alpha(rgb(0x000000), cfg.dark ? 0.09f : 0.025f));
    Color disc = online ? rgb(0xE60012) : hc ? T.btn : rgb(cfg.dark ? 0x464646 : 0xFFFFFF);
    Circle(cx, cy, kS1DiscR, disc);
    if (!online) CircleStroke(cx, cy, kS1DiscR - 0.5f, hc ? T.line : rgb(cfg.dark ? 0x525252 : 0xE2E2E2), 1.0f);
    Color ic = online ? rgb(0xFFFFFF) : b.color ? rgb(b.color) : hc ? T.icon : rgb(cfg.dark ? 0xC8C8C8 : 0x828387);
    S1BarIcon(b.index, cx, cy, ic, disc);
    if (sel) {
        CircleCursor(cx, cy, kS1DiscR + 6.5f);
        float tw = TextWidth(b.label, 20) + 20;
        float lx = (std::max)(20.0f, (std::min)(cx - tw / 2, W - 20 - tw));
        Text(b.label, lx, 594, tw, 36, 20, T.title, Center);
    }
}

// ---- game-card mark (PC games) ---------------------------------------------
// Before a title the console puts a downloaded game's mark or, for a game on a
// game card, a small red card. A PC game (a recompiled game's own program) is
// shown as the card: it is there to be started like an inserted card, while a
// dump played through an emulator is a file, like a downloaded game.
// Measured on walkthrough.mp4, "The Legend of Zelda: Ocarina of Time" focused:
// 94.75 s (dark) and 287.25 s (light) on the HOME row, 102.0 s (dark) in the
// all-software balloon; the same size in all three, in pixels:
//   body     27.8 x 39.6, corners 3, red D60016 in both themes;
//   rim      a grey 767476 ring 1.6 wide around the body, dark theme only
//            (in light the red stands straight on the ground);
//   recess   2.0 in from the sides, 2.65 from the top, 23.8 x 29.6, dark red
//            8D0012 1.7 wide around a red band 6.3 tall, the grey D2CFD3 label
//            16 tall and a black stripe 3.95 tall;
//   notch    a white F0EEF0 point-down triangle under the recess, 9.4 wide,
//            from 32.55 to 36.05 below the body's top.
// The text after it starts where it does after the downloaded mark (the title
// keeps its place: 78 px in both frames).
void CardMark(float x, float y) {
    const float w = Px(27.8f), h = Px(39.6f);
    if (cfg.dark) RRect(x - Px(1.6f), y - Px(1.6f), w + Px(3.2f), h + Px(3.2f), Px(4.6f), rgb(0x767476));
    RRect(x, y, w, h, Px(3), rgb(0xD60016));
    RRect(x + Px(2), y + Px(2.65f), Px(23.8f), Px(29.6f), Px(1), rgb(0x8D0012));
    const float ix = x + Px(3.7f), iw = Px(20.4f);
    Rect(ix, y + Px(4.25f), iw, Px(6.3f), rgb(0xD60016));
    Rect(ix, y + Px(10.55f), iw, Px(16), rgb(0xD2CFD3));
    Rect(ix, y + Px(26.55f), iw, Px(3.95f), rgb(0x000000));
    const float cx = x + w / 2;
    Triangle(cx - Px(4.7f), y + Px(32.55f), cx + Px(4.7f), y + Px(32.55f), cx, y + Px(36.05f), rgb(0xF0EEF0));
}

// Test states: "cardpc" puts a made-up PC game (a recompiled game's program,
// with the picture of a Zelda from the library when there is one) first in the
// row and focuses it; "cardall" opens all software on it. Nothing is saved.
bool DebugCard(const std::wstring& name) {
    if (name != L"cardpc" && name != L"cardall") return false;
    Game g;
    g.id = "pc:cardtest";
    g.pc = true;
    g.file = L"C:\\Juegos\\zelda-recomp\\zelda.exe";
    g.name = L"The Legend of Zelda: Ocarina of Time";
    for (const Game& o : games)
        if (g.icon.empty() && o.name.find(L"Zelda") != std::wstring::npos) {
            g.icon.path = o.icon.path;
            g.icon.mem = o.icon.mem;
            g.icon.maxSize = o.icon.maxSize;
        }
    games.insert(games.begin(), std::move(g));
    homeRow = 1;
    gi = 0;
    if (name == L"cardall") {
        allSel = 0;
        Push(Scr::All);
    }
    return true;
}

void DrawHome(float dt) {
    Theme T = Th();
    const bool s2 = S2();
    const float tileY = TileY(), x0 = TileX0(), step = kTile + TileGap();
    // Coming back from a game the whole menu is 4 % too large and settles in
    // nine frames (the fade that goes with it is at the end of this function).
    float back = flow.homeAt >= 0 ? (std::max)(0.0f, 1 - FramesSince(flow.homeAt) / 9) : 0.0f;
    PushScale(HomeZoom() * (1 + 0.04f * back * back), W / 2, H / 2);

    // Users
    int userCount = static_cast<int>(cfg.users.size());
    if (homeUser >= userCount + (HomePlusShown() ? 1 : 0)) homeUser = 0;
    for (int i = 0; i < userCount && i < 8; i++) {
        // Second console (walkthrough.mp4 96 s): pictures 80 px across, 112 px
        // apart, in a ring 6 px wide (74..166 px for the first).
        float ax = s2 ? 80 + i * Px(112) : 92 + i * 76.0f, ay = s2 ? 76.0f : 62.0f;
        if (s2) Circle(ax, ay, Px(46), T.avRing);
        DrawAvatar(i, ax, ay, s2 ? Px(40) : 30.0f);
        FriendsMarkUser(i, ax, ay, s2 ? Px(40) : 30.0f);
        if (homeRow == 0 && i == homeUser) CircleCursor(ax, ay, s2 ? 35.5f : 36.0f);
        AddHit(ax - 34, ay - 34, 68, 68, [i] {
            homeRow = 0;
            homeUser = i;
            OnButtonImpl(Btn::A);
        });
    }
    if (HomePlusShown()) {
        // "Add a user", after the last picture: drawn like the one on the user sheet.
        float px = (s2 ? 80 + userCount * Px(112) : 92 + userCount * 76.0f) - 6, py = s2 ? 76.0f : 62.0f;
        CircleStroke(px, py, 17, T.sub, 1.6f);
        Line(px - 6.5f, py, px + 6.5f, py, T.sub, 2);
        Line(px, py - 6.5f, px, py + 6.5f, T.sub, 2);
        if (homeRow == 0 && homeUser == userCount) CircleCursor(px, py, 22.5f);
        AddHit(px - 22, py - 22, 44, 44, [userCount] {
            homeRow = 0;
            homeUser = userCount;
            OnButtonImpl(Btn::A);
        });
    }
    if (homeRow == 0)
        Text(homeUser < userCount ? PageOf(homeUser) : std::wstring(L"Añadir usuario"),
             s2 ? 52.0f : 66.0f, s2 ? 116.0f : 100.0f, 500, 34, 20, T.title);
    DrawStatus();

    // Games
    int count = HomeCount();
    int shown = static_cast<int>((std::min)(games.size(), static_cast<size_t>(12)));
    if (gi >= count) gi = (std::max)(0, count - 1);
    if (s2) {
        // At the right end the focused tile rests 104.2 units from the edge
        // (walkthrough.mp4: ring from 1368 px) and the round button 84 (its
        // ring ends at 1709 px); at the left end the tile rests on x0.
        const float rightEdge = W - (gi < shown ? 104.2f : 84) - kTile;
        if (homeScroll != rowFx.lastScroll) rowFx.scrollTo = homeScroll; // set from outside
        float selLeft = x0 + gi * step - rowFx.scrollTo;
        if (selLeft < x0) rowFx.scrollTo = gi * step;
        if (selLeft > rightEdge) rowFx.scrollTo = x0 + gi * step - rightEdge;
        if (MouseOwns(&homeScroll)) { // the mouse has the row
            rowFx.scrollTo = homeScroll;
            rowFx.wrapAt = -1;
        }
        float left = fabsf(rowFx.scrollTo - homeScroll);
        // Nothing to slide when the whole row fits: the focus just changes ends.
        if (rowFx.wrapAt >= 0 && fabsf(rowFx.wrapFrom - rowFx.scrollTo) < 0.5f) rowFx.wrapAt = -1;
        if (rowFx.wrapAt >= 0) {
            float n = FramesSince(rowFx.wrapAt), r = (std::max)(0.0f, (7 - n) / 8);
            homeScroll = rowFx.scrollTo + (rowFx.wrapFrom - rowFx.scrollTo) * r * r * r;
            if (r > 0) rowFx.restAt = 1e18;
            else if (rowFx.restAt > now) rowFx.restAt = now;
            if (n > 7.5f) rowFx.wrapAt = -1;
            Touch();
        } else if (left > 0) {
            const float brake = 10.93f;
            float v = (rowFx.run ? 72.0f : (std::min)(72.0f, sqrtf(2 * brake * left + brake * brake / 4) - brake / 2)) * dt * 60;
            if (v >= left - 0.5f) homeScroll = rowFx.scrollTo;
            else homeScroll += rowFx.scrollTo > homeScroll ? v : -v;
            rowFx.restAt = homeScroll == rowFx.scrollTo ? now : 1e18;
            Touch();
        }
        rowFx.lastScroll = homeScroll;
        if (homeRow == 1 && rowFx.lastRow == 1 && rowFx.lastGi >= 0 && gi != rowFx.lastGi) {
            rowFx.fromGi = rowFx.lastGi;
            rowFx.moveAt = now;
            Touch();
        }
    } else {
        float target = homeScroll;
        float selLeft = x0 + gi * step - homeScroll;
        if (selLeft < x0) target = gi * step;
        if (selLeft > W - x0 - kTile) target = x0 + gi * step - (W - x0 - kTile);
        if (MouseOwns(&homeScroll)) target = homeScroll;
        Smooth(homeScroll, target, dt);
        if (homeScroll != target) Touch();
    }

    // While a game is being started or closed, and under any dialog, the row
    // has no title and no ring (w5 16.35 s, 17.2-19.6 s, 33.4 s, 35.2-35.6 s).
    // The title goes at once; the ring takes two more frames (0.85, 0.45).
    const float xFrames = s2 && flow.xAt >= 0 ? FramesSince(flow.xAt) : -1;
    const bool bare = s2 && (stack.back() == Scr::Who || pressAt >= 0 || flow.closing || dlg.open || xFrames >= 7.5f ||
                             (flow.endAt >= 0 && FramesSince(flow.endAt) < 0.5f));
    if (!bare) flow.bareAt = -1;
    else if (xFrames >= 7.5f && flow.bareAt < 0) flow.bareAt = now;
    float ringOut = 0;
    if (bare && flow.bareAt >= 0) {
        float n = FramesSince(flow.bareAt);
        ringOut = n < 0.5f ? 0.85f : n < 1.5f ? 0.45f : 0.0f;
        if (ringOut > 0) Touch();
    }
    float ringLeft = 0; // strength of the ring on the tile the focus has just left
    if (s2 && rowFx.moveAt >= 0) {
        float n = FramesSince(rowFx.moveAt);
        ringLeft = n < 0.5f ? 0.92f : n < 1.5f ? 0.66f : 0.0f;
        if (ringLeft > 0) Touch();
        else rowFx.moveAt = -1;
    }

    if (games.empty()) {
        for (int i = 0; i < 5; i++)
            RRectStroke(x0 + i * step + 1, tileY + 1, kTile - 2, kTile - 2, 2 + TileRadius(kTile), T.sep, 2);
        Text(scanning ? L"Buscando juegos…" : L"Añade una carpeta de juegos en Configuración de la consola",
             0, 130, W, 40, 24, T.sub, Center);
    }
    const bool rested = FramesSince(rowFx.restAt) > 1.5f;
    if (rowFx.kickWant) {
        if (rested) {
            rowFx.kickWant = false;
            rowFx.kickAt = now;
        }
        Touch();
    }
    // No title while a direction is held. A release is not seen here, only
    // that the repeats (one every 3.76 frames) have stopped coming.
    bool quiet = false;
    if (s2 && rowFx.heldAt >= 0) {
        if (rested && (gi == 0 || gi == count - 1 || FramesSince(rowFx.heldAt) > 5.5f)) rowFx.heldAt = -1;
        else quiet = true;
        Touch();
    }
    const float kick = s2 ? RowKick() : 0.0f;
    if (count > 1 && !bare) {
        // The row drags sideways. The focus stays on a tile that is where the
        // row's own scrolling would leave it, between x0 and the right edge.
        auto rightOf = [=](int i) { return s2 ? W - (i < shown ? 104.2f : 84) - kTile : W - x0 - kTile; };
        const float most = x0 + (count - 1) * step - rightOf(count - 1);
        AddScroll(0, tileY - 12, W, kTile + 24, ScrollOf(homeScroll, most, [=](float v) {
            int to = NearestFit(gi, count, [&](int i) {
                const float left = x0 + i * step - v;
                return left >= x0 - 0.5f && left <= rightOf(i) + 0.5f;
            });
            if (to >= 0) gi = to;
        }, true));
    }
    for (int i = 0; i < count; i++) {
        float x = x0 + i * step - homeScroll;
        if (x > W || x + kTile < 0) continue;
        bool sel = homeRow == 1 && i == gi;
        float sc = 1.0f;
        if (!s2 && i < shown && pressAt >= 0 && games[i].id == pressId) {
            float p = (std::max)(0.0f, (std::min)(1.0f, static_cast<float>((now - pressAt) / 0.33)));
            sc = 1.0f + 0.06f * sinf(p * 3.14159f);
        }
        // X on the running game (w5 33.27 s, per frame): the tile, ring and
        // mark included, dips 6 6 4 3 1 1 0 px a side under a pale blue tint
        // of strength 1 1 0.87 0.76 0.49 0.36 0.12.
        float tint = 0;
        if (s2 && xFrames >= 0 && i < shown && games[i].id == runningId) {
            static const float dip[] = {6, 6, 4, 3, 1, 1, 0, 0}, strength[] = {1, 1, 0.87f, 0.76f, 0.49f, 0.36f, 0.12f, 0};
            sc = 1 - 2 * Lerp(dip, 8, xFrames) / 384;
            tint = Lerp(strength, 8, xFrames);
        }
        PushScale(sc, x + kTile / 2, tileY + kTile / 2);
        if (i < shown && s2) {
            // Ring strength: full on the focused tile, fading on the one the
            // focus left and on the focused one when the row goes bare.
            float ring = sel ? (bare ? ringOut : 1.0f) : (i == rowFx.fromGi && !bare && homeRow == 1) ? ringLeft : 0.0f;
            if (ring > 0) RRect(x - 4, tileY - 4, kTile + 8, kTile + 8, TileRadius(kTile) + 4, alpha(TileFrame(), ring));
            DrawTile(games[i], x, tileY, kTile);
            bool running = !runningId.empty() && games[i].id == runningId;
            if (running) {
                DrawRunningMark(x, tileY, 1);
            } else if (flow.endAt >= 0 && games[i].id == flow.endedId) {
                // The game ended: the mark fades over six frames, linear in
                // light (w5 35.58 s: 0.56 0.39 0.27 0.16 0.07 0 of it left).
                static const float left[] = {0.56f, 0.39f, 0.27f, 0.16f, 0.07f, 0.0f};
                DrawRunningMark(x, tileY, Lerp(left, 6, FramesSince(flow.endAt)));
            }
            // Black reads 40 52 82 under the tint, a light pixel loses a third.
            if (tint > 0) RRect(x, tileY, kTile, kTile, TileRadius(kTile), rgb(0x7493EA, 0.35f * tint));
            CursorRect c = {x - 6 + (sel ? kick : 0.0f), tileY - 6, kTile + 12, kTile + 12, TileRadius(kTile) + 6};
            if (sel && !bare) {
                // Within the row the ring never cross-fades from where it was
                // (see RowFx), so this is not a jump for Cursor().
                // (Not while a panel or dialog on top owns the cursor: its
                // outline would then look like a move away from this tile.)
                if (rowFx.lastRow == 1 && hadLast && !suppressCursor) curLast = c;
                Cursor(c.x, c.y, c.w, c.h, c.r);
                if (kick != 0) {
                    // The knocked ring passes over the black band but under
                    // the picture (frame 348: the picture is whole where the
                    // ring's far side has come 4 px inside its edge).
                    DrawTile(games[i], x, tileY, kTile);
                    if (running) DrawRunningMark(x, tileY, 1);
                }
            } else if (ring > 0 && ring < 1) {
                StrokeCursor(c, ring);
            }
            if (sel && running) {
                // "Change user" under the tile (w5 32.3 s: glyph centred on
                // 129.3, 485.3; wording from x 150.7, 203.5 wide). It stays
                // while the title and ring are away and goes with the game.
                Color blue = T.title;
                // (w5 32.9 s: the disc covers pixel rows 711 to 745, columns from 176.)
                if (!Symbol(0xE0E3, x + 23.0f, tileY + 279.2f, 24.45f, blue)) {
                    Circle(x + 23.3f, tileY + 285.3f, 11, blue);
                    Text(L"Y", x + 11.3f, tileY + 273.3f, 24, 24, 15, T.bg, Center, 700);
                }
                Text(L"Cambiar de usuario", x + 42.7f, tileY + 265.3f, 320, 40, 22.9f, blue);
            }
        } else if (i < shown) {
            DrawTile(games[i], x, tileY, kTile);
            if (sel) Cursor(x - 7.5f, tileY - 7.5f, kTile + 15, kTile + 15, 3);
        } else if (s2) {
            // "All software": a round button after the last game, with its
            // name just above it when focused (walkthrough.mp4 100.2 s:
            // button 78.5 in radius, ring 86.7 outside, wording centred on y 217).
            float cx = x + 113, cy = tileY + kTile / 2;
            if (sel && !bare) Circle(cx, cy, 82.5f, TileFrame());
            Circle(cx, cy, 78.5f, T.btn);
            PushScale(1.9f, cx, cy);
            SysIcon(12, cx, cy, T.allMark);
            PopTransform();
            if (sel && !bare) {
                // Riding in with the row is not a jump for Cursor(), as on the tiles.
                if (rowFx.lastRow == 1 && hadLast && !suppressCursor) curLast = {cx + kick - 85, cy - 85, 170, 170, 85};
                // On the round button the knocked ring lies over the button (frame 920).
                CircleCursor(cx + kick, cy, 85);
                if (!quiet) Text(L"Ver más", cx - 150, 196, 300, 42, 24, T.title, Center);
            }
        } else {
            Circle(x + kTile / 2, tileY + kTile / 2, 62, T.btn);
            SysIcon(8, x + kTile / 2, tileY + kTile / 2, T.icon);
            if (sel) Cursor(x - 7.5f, tileY - 7.5f, kTile + 15, kTile + 15, 3);
        }
        PopTransform();
        AddHit(x, tileY, kTile, kTile, [i] {
            if (homeRow == 1 && gi == i) OnButtonImpl(Btn::A);
            else {
                homeRow = 1;
                gi = i;
                rowFx.run = false;
                PlaySfx(Sfx::MoveGame);
            }
        });
    }
    rowFx.lastRow = homeRow;
    rowFx.lastGi = gi;
    if (homeRow == 1 && count > 0 && !bare && !quiet && !(s2 && rowFx.wrapAt >= 0)) {
        std::wstring name = gi < shown ? games[gi].name : L"Todos los programas";
        if (s2 && gi < shown) {
            // A small card mark, then the name, centred over the tile but kept
            // on screen (w5: mark from x 24.7, wording 27.3 further; walkthrough.mp4:
            // a long name ends at 1263.3; "The Legend of Zelda: Ocarina of Time"
            // is 395.3 wide, size 22.7 here). The place is worked out for where
            // the tile will come to rest, and the title rides there with the tile.
            float tw = (std::min)(TextWidth(name, 22.7f), 700.0f), total = tw + 27.3f;
            float rest = x0 + gi * step - rowFx.scrollTo + kTile / 2 - total / 2;
            float left = (std::max)(24.7f, (std::min)(rest, W - 16.7f - total)) + (rowFx.scrollTo - homeScroll);
            if (games[gi].pc) {
                CardMark(left + Px(0.45f), Px(234.75f)); // body from pixel column 37.5, row 234.75 (94.75 s)
            } else {
                // Downloaded-game mark: a card 18.7 x 23.3 with a small arrow near its foot.
                // (walkthrough.mp4 96 s: the mark covers 29 x 35.5 px from pixel row 235.5.)
                RRect(left, Px(235.5f), Px(29), Px(35.5f), 2.5f, T.title);
                Triangle(left + 5.3f, 173.6f, 11.6f + left, 173.6f, left + 8.45f, 177.2f, T.bg);
            }
            // The capitals stand on pixel row 266 (same frame, and 96.6 s for a name both have).
            Text(name, left + 27.3f, 149.5f - Px(2), tw + 6, 42, 22.7f, T.title);
        } else if (!s2) {
            float tx = x0 + gi * step - homeScroll;
            float tw = TextWidth(name, 28);
            if (tw <= kTile + 66) Text(name, tx - 40, 122, kTile + 80, 44, 28, T.title, Center);
            else Text(name, tx - 33, 122, 600, 44, 28, T.title);
        }
    }

    // System buttons
    const auto& sys = SysList();
    int n = static_cast<int>(sys.size());
    if (si >= n) si = n - 1;
    // Measured on the capture: ring centred 40.1 from the button's centre.
    const float ringR = 40.1f;
    if (s2) {
        RRect(SysBarX(n), Px(770), SysBarW(n), Px(140), Px(70), T.panel);
        // The pressed picture goes back to rest once the menu has gone (or the dialog has closed).
        bool held = (trans.on && !trans.applied) || dlg.open;
        if (barPress.i >= 0 && !held && now - barPress.t0 > kBarFrames * kFrame) barPress.i = -1;
        if (barPress.i >= 0) Touch();
    }
    if (s2 && homeRow == 2) {
        // Focus moving along the bar, measured (frame 0 = first frame after
        // the move): the new ring, its lighter ground and the label are there
        // in full on frame 0; the old ring and ground are at 89 %, then 56 %,
        // and gone on frame 2. Nothing else on the button changes.
        if (barFocus >= 0 && barFocus != si && !suppressCursor) {
            barGhost.i = barFocus;
            barGhost.t0 = now;
            // So the shared cursor does not fade this move its own way.
            curLast = {SysX(si, n) - ringR, kBarCy - ringR, ringR * 2, ringR * 2, ringR};
            curAlpha = 1;
        }
        // Arriving from the game row (walkthrough.mp4, 113.63 s): the ring and
        // its ground are also there in full on the first frame. The outline
        // that was left fades the shared way.
        if (barFocus < 0 && hadLast && !suppressCursor) {
            ghosts.push_back({curLast, curAlpha});
            curLast = {SysX(si, n) - ringR, kBarCy - ringR, ringR * 2, ringR * 2, ringR};
            curAlpha = 1;
        }
        // A dialog opening takes the focus off the bar the same way (measured
        // on the power button: label gone at once, ring gone two frames
        // later, before the dialog starts to show).
        if (suppressCursor && barRing) {
            barGhost.i = si;
            barGhost.t0 = now;
            hadLast = false; // the dialog's own cursor is not a move from this ring
            curAlpha = 1;
        }
    }
    for (int i = 0; i < n; i++) {
        float cx = SysX(i, n);
        float cy = s2 ? kBarCy : 544.0f;
        bool sel = homeRow == 2 && i == si;
        Color ic = sys[i].color ? rgb(sys[i].color) : T.icon;
        if (s2 && !cfg.dark) {
            // The drawn pictures (no local pack) in the light theme: the console's
            // are deeper there (w5: chat, shop, album and share).
            static const uint32_t light[10] = {0, 0xE68119, 0, 0xDB3B2E, 0x3368DA, 0x1AA5B2, 0, 0, 0, 0};
            if (sys[i].index < 10 && light[sys[i].index]) ic = rgb(light[sys[i].index]);
        }
        if (s2) {
            CursorRect ring = {cx - ringR, cy - ringR, ringR * 2, ringR * 2, ringR};
            float ghost = 0;
            if (barGhost.i == i && (!sel || suppressCursor)) {
                float u = (static_cast<float>(now - barGhost.t0) + kFrame) / 0.05f;
                ghost = 1 - u * u;
                if (ghost <= 0) barGhost.i = -1;
                else Touch();
            }
            bool pressed = barPress.i == i;
            float age = pressed ? static_cast<float>(now - barPress.t0) : 0.0f;
            // Measured: under the press disc the lighter ground shows at about
            // half strength at first and is back in full when the disc has gone.
            float ground = pressed ? 0.5f + 0.5f * (std::min)(1.0f, (std::max)(0.0f, age) / (18 * kFrame)) : 1.0f;
            if (sel && !suppressCursor) Circle(cx, cy, ringR, alpha(T.focusBg, curAlpha * ground));
            else if (ghost > 0) Circle(cx, cy, ringR, alpha(T.focusBg, ghost));

            // The disc takes the picture's colour (the grey pictures measure
            // 78757E in the light theme, where no coloured press was captured).
            Color tint = rgb(!cfg.dark && !sys[i].color ? 0x78757E : sys[i].disc);
            if (pressed && age < 0.32f) PressDiscAt(cx, cy, 41, age, tint, sys[i].action == 6);

            Img* packed = pressed ? BarFrame(sys[i].index, static_cast<int>(age * 60 + 0.5f)) : nullptr;
            float pop = 1.0f, turn = 0.0f;
            // (A pack animation that is shorter than the rest ends on the still picture.)
            if (pressed && !packed && !BarFrame(sys[i].index, 0)) {
                pop = PressDipAt(age);
                if (sys[i].icon == 5) turn = GearTurn(age);
            }
            if (!packed) packed = PackIcon(L"sys_" + std::to_wstring(sys[i].index) + (cfg.dark ? L"_d" : L"_l"));
            PushScale(pop, cx, cy);
            if (turn != 0) PushRotate(-turn, cx, cy);
            if (packed) {
                // The pack's pictures were cut on the capture's pixel grid
                // (1.5 pixels a unit); drawn on the same grid they stay sharp.
                // The last button's were cut one pixel left of its box (the
                // cutting tool placed it by an older, slightly short step).
                Image(*packed, Snap(cx - 36) - (sys[i].index == 9 ? Px(1) : 0.0f), Snap(cy - 36), 72, 72);
            } else {
                if (sys[i].index == 0) Circle(cx, cy, 32, rgb(0xD50017)); // the disc's red on the capture, both themes
                PushScale(sys[i].index == 0 ? 1.15f : 1.35f, cx, cy);
                SysIcon(sys[i].icon, cx, cy, ic);
                PopTransform();
            }
            if (turn != 0) PopTransform();
            PopTransform();
            if (ghost > 0) StrokeCursor(ring, ghost);
            if (sel && !suppressCursor) {
                CircleCursor(cx, cy, ringR);
                // Same size as the footer hints (width-matched to the capture).
                float tw = TextWidth(sys[i].label, 23.4f) + 20;
                float lx = (std::max)(20.0f, (std::min)(cx - tw / 2, W - 20 - tw));
                Text(sys[i].label, lx, 606.67f, tw, 38, 23.4f, T.title, Center);
            }
        } else {
            DrawS1SysButton(sys[i], cx, cy, sel);
        }
        AddHit(cx - 36, cy - 36, 72, 72, [i] {
            homeRow = 2;
            si = i;
            SysAction(i);
        });
    }
    barFocus = homeRow == 2 ? si : -1;
    barRing = barFocus >= 0 && !suppressCursor;

    PopTransform();
    std::vector<Hint> hints;
    const wchar_t* ok = s2 ? L"Confirmar" : L"Iniciar";
    if (homeRow == 1 && gi < shown) {
        bool isRunning = !runningId.empty() && games[gi].id == runningId;
        if (s2) {
            hints.push_back({L"+", L"Opciones", Btn::Plus});
            if (isRunning) hints.push_back({L"X", L"Cerrar el programa", Btn::X});
            hints.push_back({L"A", ok, Btn::A});
        } else {
            if (isRunning) hints.push_back({L"X", L"Cerrar", Btn::X});
            hints.push_back({L"+", L"Opciones", Btn::Plus});
            hints.push_back({L"A", isRunning ? L"Reanudar" : ok, Btn::A});
        }
    } else {
        hints.push_back({L"A", L"Aceptar", Btn::A});
    }
    Footer(hints, !s2); // the second console's home has no footer rule

    if (!s2) {
        if (pressAt >= 0 && now - pressAt > 0.52) FinishLaunch();
        if (pressAt >= 0) {
            Touch();
            float p = static_cast<float>((now - pressAt - 0.33) / 0.17);
            if (p > 0) Rect(0, 0, W, H, rgb(0x000000, (std::min)(p, 1.0f)));
        }
        return;
    }

    // Behind the user sheet the whole menu, footer included, is darkened.
    float dim = HomeDim();
    if (dim > 0) {
        Rect(0, 0, W, H, rgb(cfg.dark ? 0x000000 : 0x000006, 0.336f * dim)); // the light theme's veil has a trace of blue, as under a dialog
        Touch();
    }
    if (flow.leaveAt >= 0 && stack.back() != Scr::Who && FramesSince(flow.leaveAt) > (flow.picked ? 24.0f : 8.0f))
        flow.leaveAt = flow.sheetAt = -1;

    // Starting a game (w5 19.08 s on): a copy of the tile's picture grows out
    // of the tile over everything else while it fades; the tile, the rest of
    // the menu and the footer stay as they are. From the 17th frame the whole
    // screen goes to black in 12 frames, linear in light, and is black on the
    // 29th.
    if (pressAt >= 0) {
        Touch();
        float fr = FramesSince(pressAt);
        Game* g = FindGame(pressId);
        int idx = -1;
        for (int i = 0; i < shown; i++)
            if (games[i].id == pressId) idx = i;
        if (fr >= 28.5f) {
            Rect(0, 0, W, H, rgb(0x000000));
            FinishLaunch();
        } else if (fr > 0 && g && idx >= 0) {
            float x = x0 + idx * step - homeScroll;
            float sc = Lerp(kGhostScale, 30, fr), a = Lerp(kGhostAlpha, 30, fr);
            auto copy = [&](float opacity) {
                PushLayer(opacity);
                PushScale(sc, x + kTile / 2, tileY + kTile / 2);
                DrawTile(*g, x, tileY, kTile);
                PopTransform();
                PopLayer();
            };
            // The console's blend is linear in light, ours is not, so the
            // strength is set per region to read the same (fitted on w5 with
            // the same picture on the tile): over its own picture the copy
            // reads about 1.25 times its opacity, over the pale page of the
            // light theme about 0.55 times. Over the dark page a lighter picture reads
            // stronger; that factor is worked out, not measured. The copy's
            // edge is soft: it only reaches full strength 14.5 % of its width
            // in (w5: a ramp of 65 to 85 px as it grows), drawn here as six
            // steps along a smooth curve.
            // Both factors drift a little with the opacity itself.
            float inside = (1.42f - 0.75f * a) * a, outside = (cfg.dark ? 1.6f : 0.30f + 1.2f * a) * a;
            float half = kTile / 2 * sc, cx = x + kTile / 2, cy = tileY + kTile / 2;
            float feather = 0.29f * half;
            float pass = 1 - powf(1 - outside, 1 / 6.0f);
            static const float steps[] = {0.19f, 0.33f, 0.44f, 0.56f, 0.67f, 0.81f};
            for (float q : steps) {
                float h = half - feather * q;
                PushClip(cx - h, cy - h, h * 2, h * 2);
                copy(pass);
                PopClip();
            }
            if (inside > outside) {
                PushClip(x, tileY, kTile, kTile);
                copy(1 - (1 - inside) / (1 - outside));
                PopClip();
            }
            if (fr > 17) Rect(0, 0, W, H, rgb(0x000000, 1 - powf((std::max)(0.0f, 1 - (fr - 17) / 12), 1 / 2.2f)));
        }
    }

    // Closing the running game from the menu.
    if (flow.xAt >= 0) {
        Touch();
        if (FramesSince(flow.xAt) >= 10.5f) {
            flow.xAt = -1;
            OpenCloseDialog();
        }
    }
    if (flow.closing) {
        Touch();
        if (runningId.empty()) {
            flow.closing = false;
        } else if (flow.closeAt >= 0 && now >= flow.closeAt) {
            flow.closeAt = -1;
            CloseGame(); // ends in OnGameExit
        }
    }
    if (flow.endAt >= 0) {
        Touch();
        if (FramesSince(flow.endAt) > 6) flow.endAt = -1;
    }

    // Back from a game (w5 31.62 s): the menu is there on the next frame,
    // 4 % too large and see-through, and settles in nine frames. Opacity read
    // off the page colour: 0.56 0.70 0.78 0.83 0.89 0.92 0.95 0.97 0.99; size
    // 1 + 0.04 (1 - n/9)^2 about the centre of the screen. The game is another
    // window here, so the menu fades in from black instead of over the game.
    // (The size is applied at the top of this function.)
    if (flow.homeAt >= 0) {
        float fr = FramesSince(flow.homeAt);
        if (fr > 9.5f) {
            flow.homeAt = -1;
        } else {
            static const float cover[] = {0.44f, 0.30f, 0.22f, 0.17f, 0.11f, 0.08f, 0.05f, 0.03f, 0.01f, 0.0f};
            Rect(0, 0, W, H, rgb(0x000000, Lerp(cover, 10, fr)));
            Touch();
        }
    }
}

void HomeButton(Btn b) {
    int count = HomeCount();
    int shown = static_cast<int>((std::min)(games.size(), static_cast<size_t>(12)));
    if (pressAt >= 0) return;
    auto move = [](int& v, int d, int n, Sfx tick = Sfx::Move) {
        if (v + d < 0 || v + d >= n) PlaySfx(Sfx::Bump);
        else {
            v += d;
            PlaySfx(tick);
        }
    };
    switch (b) {
    case Btn::Left:
        if (homeRow == 0) move(homeUser, -1, static_cast<int>(cfg.users.size()) + HomePlusShown(), Sfx::UserMove);
        else if (homeRow == 1) {
            if (RowHeld(-1, count)) break;
            rowFx.run = heldRepeat;
            move(gi, -1, count, Sfx::MoveGame);
        } else move(si, -1, SysCount());
        break;
    case Btn::Right:
        if (homeRow == 0) move(homeUser, 1, static_cast<int>(cfg.users.size()) + HomePlusShown(), Sfx::UserMove);
        else if (homeRow == 1) {
            if (RowHeld(1, count)) break;
            rowFx.run = heldRepeat;
            move(gi, 1, count, Sfx::MoveGame);
        } else move(si, 1, SysCount());
        break;
    case Btn::Up:
        if (homeRow == 0) PlaySfx(Sfx::Bump);
        else {
            homeRow = (homeRow == 2 && count > 0) ? 1 : 0;
            PlaySfx(Sfx::Move);
        }
        break;
    case Btn::Down:
        if (homeRow == 2) PlaySfx(Sfx::Bump);
        else {
            homeRow = (homeRow == 0 && count > 0) ? 1 : 2;
            PlaySfx(Sfx::Move);
        }
        break;
    case Btn::A:
        if (homeRow == 0 && homeUser >= static_cast<int>(cfg.users.size())) {
            PlaySfx(Sfx::Ok);
            StartAddUser([](int i) {
                if (i >= 0) homeUser = i; // back on the row, on the new picture
            });
        } else if (homeRow == 0) {
            PlaySfx(Sfx::UserPage);
            OpenUserPage(homeUser);
        } else if (homeRow == 1 && count > 0) {
            if (gi < shown) StartLaunch(games[gi].id);
            else {
                PlaySfx(Sfx::Ok);
                allSel = 0;
                Push(Scr::All);
            }
        } else if (homeRow == 2) {
            SysAction(si);
        }
        break;
    case Btn::Plus:
        if (homeRow == 1 && gi < shown) {
            PlaySfx(Sfx::Ok);
            OpenOptions(games[gi].id);
        }
        break;
    case Btn::X:
        if (homeRow == 1 && gi < shown && games[gi].id == runningId) AskClose();
        break;
    case Btn::Y:
        if (homeRow == 1 && gi < shown && games[gi].id == runningId) ChangeUser();
        break;
    default: break;
    }
}

/// ---- all software ---------------------------------------------------------

constexpr int kAllCols = 6;
struct AllLay {
    float tile, gap, x0, y0, top, bottom;
};
// Second console, measured on walkthrough.mp4 (102.2 s): tiles 238 px every
// 254 px from x 254; the first row starts at y 196 px under a one-line label.
AllLay AL() {
    if (S2()) return {158.67f, 10.67f, 169.33f, 130.67f, 75, 632};
    return {176, 20, 62, 118, 89, 630};
}

// A scroll position that glides to its target. Measured on the capture
// (settings rows 144.57 s, categories 130.03 s, software grid 102.38 s): the
// distance is covered in 7 frames, slowing down - what is left after frame k
// is (1 - k/7)^2 - and the first step is taken in the frame of the move.
struct Glide {
    float from = 0, to = 0;
    double t0 = -1;
};
void GlideTo(Glide& g, float& v, float target) {
    if (g.t0 < 0 || target != g.to) {
        g.from = v;
        g.to = target;
        g.t0 = now;
    }
    float t = (static_cast<float>(now - g.t0) + 1.0f / 60.0f) / (7.0f / 60.0f);
    if (t >= 1 || g.from == g.to) {
        v = g.to;
    } else {
        v = g.to + (g.from - g.to) * (1 - t) * (1 - t);
        Touch();
    }
}
Glide listGlide, allGlide, userGlide;

void DrawAll(float dt) {
    Theme T = Th();
    const bool s2 = S2();
    int n = static_cast<int>(games.size());
    if (allSel >= n) allSel = (std::max)(0, n - 1);
    int row = allSel / kAllCols;
    float step = AL().tile + AL().gap;
    if (s2) {
        // Measured (101.5-103.4 s): going down, a row whose top would lie
        // below y 516 px is brought up to exactly there; the glide is the
        // same 7-frame one as in the lists.
        float target = allGlide.t0 < 0 ? allScroll : allGlide.to;
        float top = AL().y0 + row * step - target;
        if (top > 344) target = AL().y0 + row * step - 344;
        if (top < AL().y0) target = row * step;
        if (MouseOwns(&allScroll)) target = allScroll;
        GlideTo(allGlide, allScroll, target);
    } else {
        float target = allScroll;
        float top = AL().y0 + row * step - allScroll;
        if (top < AL().y0) target = row * step;
        if (top + AL().tile > AL().bottom) target = AL().y0 + row * step + AL().tile - AL().bottom;
        if (MouseOwns(&allScroll)) target = allScroll;
        Smooth(allScroll, target, dt);
        if (allScroll != target) Touch();
    }

    // Opening (walkthrough.mp4 100.67 s, frames counted from the press): the
    // tab strip fades in alone over frames 34-41, the pictures over 45-50, and
    // the cursor, the name and the footer hints are there from frame 50. The
    // page is first drawn on frame 34, so those are 11 and 16 frames in.
    if (allShownAt < 0) allShownAt = now;
    const float shownFor = s2 ? static_cast<float>((now - allShownAt) * 60.0) : 1000.0f;
    const float tilesIn = (std::max)(0.0f, (std::min)(1.0f, (shownFor - 11) / 5));
    const bool settled = shownFor >= 16;
    if (!settled) Touch();

    // Dragged, the focus keeps to a row whose top is where the grid's own
    // scrolling lets a row stand (between y0 and 344, or the bottom edge).
    const int allRows = (n + kAllCols - 1) / kAllCols;
    const float rowMax = s2 ? 344.0f : AL().bottom - AL().tile;
    const int allDrag = !settled ? -1 :
        AddScroll(0, AL().top, W, 646 - AL().top, ScrollOf(allScroll, AL().y0 + (allRows - 1) * step - rowMax, [=](float v) {
            int r = NearestFit(allSel / kAllCols, allRows, [&](int i) {
                const float top = AL().y0 + i * step - v;
                return top >= AL().y0 - 0.5f && top <= rowMax + 0.5f;
            });
            if (r >= 0) allSel = (std::min)(n - 1, r * kAllCols + allSel % kAllCols);
        }));
    PushClip(0, AL().top, W, 646 - AL().top);
    if (tilesIn < 1) PushLayer(tilesIn);
    if (s2) Text(L"Usados recientemente", 772.67f, 88 - allScroll, 400, 30, 19.45f, T.sub, Right); // 300 px wide at 102.2 s
    for (int i = 0; i < n; i++) {
        float x = AL().x0 + (i % kAllCols) * step, y = AL().y0 + (i / kAllCols) * step - allScroll;
        if (y > 660 || y + AL().tile < 60) continue;
        // A black rim separates the focused tile from its outline (4 px wide, 102.2 s).
        if (i == allSel && s2 && settled) RRect(x - Px(4), y - Px(4), AL().tile + Px(8), AL().tile + Px(8), 7, rgb(0x000000));
        DrawTile(games[i], x, y, AL().tile);
        if (i == allSel && settled) {
            // Outline 6 px wide with its outer edge 10 px outside the tile.
            if (s2) Cursor(x - Px(7), y - Px(7), AL().tile + Px(14), AL().tile + Px(14), 9);
            else Cursor(x - 7, y - 7, AL().tile + 14, AL().tile + 14, 3.0f);
        }
        AddHit(x, y, AL().tile, AL().tile, [i] {
            if (allSel == i) OnButtonImpl(Btn::A);
            else {
                allSel = i;
                PlaySfx(Sfx::Move);
            }
        });
    }
    if (tilesIn < 1) PopLayer();
    PopClip();
    if (s2) {
        // Tab strip instead of a title, and the name in a balloon by the tile.
        Text(L"Programas", 384, 24, 224, 40, 21, T.accent, Center);
        Rect(384, Px(88), 224, Px(4), T.accent); // pixel rows 88 to 91
        HRule(48, 1232, Px(111), T.line);        // pixel row 111
        int rows = (n + kAllCols - 1) / kAllCols;
        float content = AL().y0 + rows * step, view = 646 - AL().top;
        if (content > view + 20) {
            float track = 357, len = (std::max)(30.0f, track * view / content);
            float maxScroll = (std::max)(1.0f, AL().y0 + (rows - 1) * step - 344);
            RRect(1254, 82 + (std::min)(1.0f, allScroll / maxScroll) * (track - len), 4, len, 2,
                  cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7));
            AddScrollBar(allDrag, 1254, 82, track, len, maxScroll);
        }
        if (n > 0 && settled) {
            // Measured (102.2 s): the balloon is 85 px tall and starts 44 px
            // left of the tile; its foot is 30 px above the tile, with a small
            // pointer over the tile's middle. In the top row there is no room,
            // and it hangs 29 px under the tile instead. It moves with the tile
            // while the grid glides and jumps, without any fade, on a move.
            float tx = AL().x0 + (allSel % kAllCols) * step;
            float ty = AL().y0 + (allSel / kAllCols) * step - allScroll;
            // (Its edge is one pixel wide: rows 335 and 420 over a tile at 450.)
            float tw = (std::min)(TextWidth(games[allSel].name, 22.3f), 620.0f), bw = Snap(tw + 93.33f), bh = Px(86);
            float bx = Snap((std::max)(40.0f, (std::min)(tx - 29.33f, W - 40 - bw)));
            bool above = ty - 76.67f >= AL().top + 4;
            float by = Snap(above ? ty - 76.67f : ty + AL().tile + 19.33f);
            float cx = tx + AL().tile / 2;
            // Measured in the dark theme (walkthrough.mp4 102.2 s): ground 373737, edge
            // 5A5A5A, the scroll bar's grey. The light theme's grid is in no capture: its
            // balloon is placed between the bar and the focus fill as the dark one is,
            // with the light scroll bar's grey for its edge.
            Color ground = cfg.dark ? rgb(0x373737) : rgb(0xE8E5E9);
            PushClip(0, AL().top, W, 646 - AL().top);
            RRect(bx, by, bw, bh, 14, ground);
            RRectStroke(bx + Px(0.5f), by + Px(0.5f), bw - Px(1), bh - Px(1), 13.5f, cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7),
                        Px(1));
            if (above) Triangle(cx - 9, by + bh - 0.5f, cx + 9, by + bh - 0.5f, cx, by + bh + 10.5f, ground);
            else Triangle(cx - 9, by + 0.5f, cx + 9, by + 0.5f, cx, by - 10.5f, ground);
            if (games[allSel].pc) {
                CardMark(bx + Px(53.5f), by + Px(23.75f)); // 102.0 s: balloon edge on column 210, row 463
            } else {
                RRect(bx + 34.67f, by + 16.67f, 19.33f, 23.33f, 2.5f, T.title);
                Triangle(bx + 40.2f, by + 33.6f, bx + 46.5f, by + 33.6f, bx + 43.35f, by + 37.2f, ground);
            }
            Text(games[allSel].name, bx + 63.33f, by, tw + 8, bh, 22.3f, T.title);
            PopClip();
        }
    } else {
        Rect(0, 0, W, 88, T.bg);
        Header(8, L"Todos los programas");
        if (n > 0) Text(games[allSel].name, 560, 20, 660, 52, 22, T.title, Right);
    }
    if (!settled) Footer({});
    else Footer({{L"Y", L"Carpeta de juegos", Btn::Y}, {L"+", L"Opciones", Btn::Plus}, {L"B", L"Atrás", Btn::B},
                 {L"A", s2 ? L"Confirmar" : L"Iniciar", Btn::A}});
}

void AllButton(Btn b) {
    int n = static_cast<int>(games.size());
    auto go = [&](int d) {
        int v = allSel + d;
        bool blocked = v < 0 || v >= n;
        if (blocked) {
            PlaySfx(Sfx::Bump);
            if (S2()) BumpCursor(d == 1 ? 1.0f : d == -1 ? -1.0f : 0.0f, d > 1 ? 1.0f : d < -1 ? -1.0f : 0.0f);
        } else {
            allSel = v;
            PlaySfx(Sfx::Move);
        }
    };
    switch (b) {
    case Btn::Left: go(-1); break;
    case Btn::Right: go(1); break;
    case Btn::Up: go(-kAllCols); break;
    case Btn::Down: go(kAllCols); break;
    case Btn::A:
        if (n > 0) {
            std::string id = games[allSel].id;
            PopNow();
            for (int i = 0; i < n; i++)
                if (games[i].id == id && i < 12) gi = i;
            homeRow = 1;
            StartLaunch(id);
        }
        break;
    case Btn::Plus:
        if (n > 0) OpenOptions(games[allSel].id);
        else GameFolderMenu();
        break;
    case Btn::Y:
        PlaySfx(Sfx::Ok);
        GameFolderMenu();
        break;
    case Btn::B:
        PlaySfx(Sfx::Back);
        Pop();
        break;
    default: break;
    }
}

// ---- list pages -----------------------------------------------------------

struct ListLay {
    float catX, catW, catY0, catH, catText, catBar;
    float rowX, rowW, rowY0, rowH, headH;
    float top, bottom; // visible band for the rows
    float radius;
    float divX, barX; // divider between the columns, scroll bar (second console only)
};
// Second console, measured on walkthrough.mp4 (124.7 s): a category's outline
// is 476 x 124 px with its outer edge at x 130, so the line down the middle
// of the stroke starts at 88.67 units and is 313.33 wide.
ListLay LL() {
    // Rows (124.7 s and every section after): rules on pixel columns 704 to
    // 1727, 113 px apart from row 192. Sections: 122 1/3 px apart, the first
    // centred on 221.75 px; the accent bar on columns 162-167; the divider on
    // column 639.
    if (S2())
        return {88.67f, 313.33f, Px(160.583f), Px(122.333f), 44.67f, 19.33f, Px(704), Px(1024), 128, Px(113), 56, 86, 645,
                8, Px(639), 1254};
    return {60, 350, 112, 70, 42, 22, 470, 750, 104, 70, 58, 89.5f, 646.5f, 4, 0, 0};
}
// The same list inside the game options panel (walkthrough.mp4 240.5 s): the
// columns sit 53.3 units further right and 21.3 lower, the rows are narrower
// and, unlike those of Settings, 112.5 px apart from 223.6 px down (their rules
// are on rows 223 and 336 in one section, 475, 587 and 700 in another); the
// divider is on pixel column 727, the scroll bar on 1749-1754.
ListLay PanelLay() {
    return {142, 313.33f, Px(192.583f), Px(122.333f), 44.67f, 19.33f, 528, 576, Px(223.6f), 75, 56, 107, 645, 8, Px(727),
            1166};
}

// Which category's rows are on screen. Measured (walkthrough.mp4 127.1-130.2 s):
// the right-hand side keeps showing the old category for 4-5 frames after the
// cursor has moved, then is replaced in a single frame, with no fade.
const ListPage* listFor = nullptr;
int listShown = -1, listWanted = -1;
double listWantedAt = 0, listDrawnAt = -10;
float listHeldScroll = 0; // scroll of the category that is still on screen

// The game options panel (second console): when it was opened and closed.
double optOpenAt = -10, optCloseAt = -1;
bool OptionsBusy() {
    return S2() && (now - optOpenAt < 0.38 || optCloseAt >= 0);
}

// The user page (second console): focus and scroll.
int userSec = 0, userRow = 0;
bool userInRows = false;
float userScroll = 0;
double userScriptAt = -1; // test states: when the scripted moves start
int userScript = 0;

void UserButton(Btn b);

// ---- settings: what the rows are made of ----------------------------------

std::vector<std::wstring> WrapText(const std::wstring& s, float size, float maxW); // with the dialog code

// The left column scrolls by itself on a long page (see DrawListBody).
Glide catGlide;
const ListPage* catFor = nullptr;

// Said, in the console's dialog, by every console option that leads to a page
// nobody captured and that has nothing to act on in a PC.
const wchar_t* const kOnlyConsole = L"Esta función solo está disponible en la consola.";
const wchar_t* const kNSWHomeVersion = L"0.1.0";

void Notice() {
    Info(kOnlyConsole);
}

// Console options with no equivalent here keep their state like the console does.
std::string Cos(const std::string& id, const std::string& def) {
    auto it = cfg.console.find(id);
    return it == cfg.console.end() ? def : it->second;
}
void SetCos(const std::string& id, const std::string& v) {
    cfg.console[id] = v;
    SaveConfig();
}

// The console writes its buttons as pictures inside the text. They are
// private characters of its own font, so here they are spelt out.
std::wstring Spell(const wchar_t* s) {
    std::wstring out;
    for (; *s; s++) {
        if (*s == 0xE0E0) out += L"A";
        else if (*s == 0xE0E1) out += L"B";
        else if (*s == 0xE0F4) out += L"HOME";
        else out += *s;
    }
    return out;
}

// -- the emulator's own settings (ryucfg) --

// The emulator games start with decides the file and the module: Ryujinx and
// its forks keep a Config.json (ryucfg), the yuzu family a qt-config.ini
// (yuzucfg). Both describe a setting the same way, so the pages only ever go
// through these.
std::string emuKind; // "" for ryucfg; otherwise the yuzu-family emulator's id
bool EmuOpenFile() {
    emuKind = EmuConfigKind();
    return emuKind.empty() ? ryucfg::Open(EmuConfigPath()) : yuzucfg::Open(EmuConfigPath());
}
const std::vector<ryucfg::Setting>& EmuList() {
    return emuKind.empty() ? ryucfg::Settings() : yuzucfg::Settings(emuKind);
}
const ryucfg::Setting* EmuFind(const std::string& key) {
    return emuKind.empty() ? ryucfg::Find(key) : yuzucfg::Find(emuKind, key);
}
bool EmuAvail(const ryucfg::Setting& s) {
    return emuKind.empty() ? ryucfg::Available(s) : yuzucfg::Available(s);
}
std::string EmuRaw(const std::string& key) {
    return emuKind.empty() ? ryucfg::Get(key) : yuzucfg::Get(key);
}
bool EmuWrite(const std::vector<std::pair<std::string, std::string>>& changes) {
    return emuKind.empty() ? ryucfg::SetMany(changes) : yuzucfg::SetMany(changes);
}
// Changes waiting for the game to close are kept per kind of file.
std::string EmuWaitKey() {
    return emuKind.empty() ? "ryu." : "yuzu.";
}

// What a console row stands for, named by the Ryujinx key, as the key of the
// module in use; empty where the yuzu family has no such setting (the time
// zone) or none whose values are known here. The values need no translating
// (each module's own list says what they mean), except for the internet:
// Ryujinx stores "games may use it", the yuzu family its opposite, airplane
// mode (EmuNetOn, EmuNetSet).
bool EmuReady();
std::string EmuKey(const char* meaning) {
    EmuReady();
    if (emuKind.empty()) return meaning;
    static const char* const map[][2] = {{"system_language", "System.language_index"},
                                         {"system_region", "System.region_index"},
                                         {"res_scale", "Renderer.resolution_setup"},
                                         {"audio_backend", "Audio.output_engine"},
                                         {"tv_sound", "System.sound_index"},
                                         {"enable_internet_access", "Services.airplane_mode"}};
    for (const auto& m : map)
        if (!strcmp(m[0], meaning)) return m[1];
    return {};
}

bool emuTried = false, emuOpen = false;
// Chosen while a game was open: the emulator may write its settings back over
// the file when it closes, so these wait until it has (see EmuFlush).
std::map<std::string, std::string> emuPending;

bool emuDebugMissing = false; // test states: as if the emulator's settings file were not there

bool EmuReady() {
    if (emuDebugMissing) return false;
    if (!emuTried) {
        emuTried = true;
        emuOpen = EmuOpenFile();
        emuPending.clear();
        const std::string wait = EmuWaitKey();
        for (const auto& kv : cfg.console)
            if (kv.first.compare(0, wait.size(), wait) == 0) emuPending[kv.first.substr(wait.size())] = kv.second;
    }
    return emuOpen;
}

// Writes what was chosen while a game was open. Called when the game has
// closed, and whenever Settings is opened with no game running.
void EmuFlush() {
    if (!runningId.empty()) return;
    emuTried = false; // read the file again: the emulator may have rewritten it
    if (!EmuReady() || emuPending.empty()) return;
    std::vector<std::pair<std::string, std::string>> changes(emuPending.begin(), emuPending.end());
    // One bad value must not hold the others back for ever.
    if (!EmuWrite(changes))
        for (const auto& c : changes) EmuWrite({c});
    for (const auto& c : changes) cfg.console.erase(EmuWaitKey() + c.first);
    emuPending.clear();
    SaveConfig();
}

const ryucfg::Setting* EmuSetting(const std::string& key) {
    if (!EmuReady()) return nullptr;
    const ryucfg::Setting* s = EmuFind(key);
    return s && EmuAvail(*s) ? s : nullptr;
}

std::string EmuGet(const std::string& key) {
    auto it = emuPending.find(key);
    return it != emuPending.end() ? it->second : EmuRaw(key);
}

void EmuSet(const std::string& key, const std::string& value) {
    if (!EmuReady()) return;
    if (!runningId.empty()) {
        emuPending[key] = value;
        cfg.console[EmuWaitKey() + key] = value;
        SaveConfig();
    } else if (!EmuWrite({{key, value}})) {
        Info(L"No se ha podido guardar el cambio.");
    }
}

bool EmuNetOn() {
    const std::string v = EmuGet(EmuKey("enable_internet_access"));
    return emuKind.empty() ? v == "true" : v == "false";
}
void EmuNetSet(bool on) {
    EmuSet(EmuKey("enable_internet_access"), on == emuKind.empty() ? "true" : "false");
}

// -- choosing one of several values --
// No capture shows the console's own list of choices, so this is a box in the
// style of its dialogs: one option to a row and a tick on the current one.
struct Picker {
    bool open = false;
    std::wstring title;
    std::vector<std::wstring> items;
    int cur = -1, sel = 0, first = 0;
    std::function<void(int)> done;
    double at = 0;
} picker;

void Pick(const std::wstring& title, std::vector<std::wstring> items, int cur, std::function<void(int)> done) {
    picker.open = true;
    picker.title = title;
    picker.items = std::move(items);
    picker.cur = cur;
    picker.sel = (std::max)(0, cur);
    picker.first = 0;
    picker.done = std::move(done);
    picker.at = now;
    Touch();
}

void PickerChoose(int i) {
    picker.open = false;
    auto done = picker.done;
    picker.done = nullptr;
    if (done) done(i);
    Touch();
}

constexpr int kPickRows = 7;

void DrawPicker() {
    Theme T = Th();
    const int n = static_cast<int>(picker.items.size());
    const int vis = (std::min)(n, kPickRows);
    float a = (std::min)(1.0f, (static_cast<float>(now - picker.at) + kFrame) / 0.1f);
    if (a < 1) Touch();
    Rect(0, 0, W, H, rgb(0x000000, 0.6f * a));
    const float rowH = 60, titleH = picker.title.empty() ? 0.0f : 62.0f;
    const float bw = 560, bh = titleH + vis * rowH + 16, bx = (W - bw) / 2, by = (H - bh) / 2;
    if (picker.sel < picker.first) picker.first = picker.sel;
    if (picker.sel >= picker.first + vis) picker.first = picker.sel - vis + 1;
    if (a < 1) PushLayer(a);
    RRect(bx, by, bw, bh, S2() ? 8.0f : 5.0f, T.dialog);
    if (titleH > 0) {
        Text(picker.title, bx + 30, by + 4, bw - 60, titleH - 4, 21.5f, T.text, Center);
        Line(bx, by + titleH + 0.5f, bx + bw, by + titleH + 0.5f, T.sep, 1);
    }
    // The list drags a whole row at a time (it only ever shows whole rows);
    // the focus stays on a row in view.
    Scroller pick;
    pick.key = &picker.first;
    pick.hi = static_cast<float>(n - vis) * rowH;
    pick.get = [rowH] { return static_cast<float>(picker.first) * rowH; };
    pick.set = [rowH, n, vis](float v) {
        picker.first = (std::max)(0, (std::min)(n - vis, static_cast<int>(lroundf(v / rowH))));
        picker.sel = (std::max)(picker.first, (std::min)(picker.first + vis - 1, picker.sel));
    };
    const int pickDrag = a >= 1 ? AddScroll(bx, by + titleH + 8, bw, vis * rowH, std::move(pick)) : -1;
    for (int v = 0; v < vis; v++) {
        int i = picker.first + v;
        float y = by + titleH + 8 + v * rowH;
        bool focused = i == picker.sel;
        if (v > 0) Line(bx + 24, y + 0.5f, bx + bw - 24, y + 0.5f, T.sep, 1);
        if (focused && S2()) RRect(bx + 10, y + 2, bw - 20, rowH - 3, 6, T.focusBg);
        Text(picker.items[i], bx + 36, y, bw - 120, rowH, 21.5f, i == picker.cur ? T.accent : T.text);
        if (i == picker.cur) {
            float cx = bx + bw - 52, cy = y + rowH / 2;
            Line(cx - 9, cy, cx - 3, cy + 7, T.accent, 2.6f);
            Line(cx - 3, cy + 7, cx + 10, cy - 8, T.accent, 2.6f);
        }
        if (focused && a >= 1) {
            if (S2()) CursorFill(T.focusBg);
            Cursor(bx + 10, y + 2, bw - 20, rowH - 3, 6);
        }
        AddHit(bx, y, bw, rowH, [i] {
            PlaySfx(Sfx::Ok);
            PickerChoose(i);
        });
    }
    if (n > vis) {
        float track = vis * rowH - 12, len = track * vis / n;
        RRect(bx + bw - 9, by + titleH + 14 + (track - len) * picker.first / static_cast<float>(n - vis), 3, len, 1.5f,
              T.sub);
        AddScrollBar(pickDrag, bx + bw - 9, by + titleH + 14, track, len, static_cast<float>(n - vis) * rowH);
    }
    if (a < 1) PopLayer();
}

void PickerButton(Btn b) {
    const int n = static_cast<int>(picker.items.size());
    switch (b) {
    case Btn::Up:
    case Btn::Down: {
        int to = picker.sel + (b == Btn::Up ? -1 : 1);
        if (to < 0 || to >= n) {
            PlaySfx(Sfx::Bump);
            if (S2()) BumpCursor(0, b == Btn::Up ? -1.0f : 1.0f);
        } else {
            picker.sel = to;
            PlaySfx(Sfx::Move);
        }
        break;
    }
    case Btn::A:
        PlaySfx(Sfx::Ok);
        PickerChoose(picker.sel);
        break;
    case Btn::B:
        PlaySfx(Sfx::Back);
        picker.open = false;
        break;
    default: break;
    }
}

// -- row geometry --

bool CanFocus(const Row& r) {
    if (r.heading) return false;
    return r.kind == RK::Plain || r.kind == RK::Toggle || r.kind == RK::Radio || r.kind == RK::Slider ||
           r.kind == RK::Users;
}

// A description under a row: paragraphs, and the console's bullet lines
// (a diamond, with the text hanging to its right).
struct NoteLine {
    std::wstring text;
    bool bullet = false, hang = false;
};
constexpr float kNoteSize = 17.4f, kNotePitch = 26.67f, kTextSize = 21.5f, kTextPitch = 29.6f;

// Line pitch of a description: 40 px on the first console's pages; on the
// second console's nine lines cover 8 x 39 1/3 px (135 s, the Bluetooth page).
float NotePitch() {
    return S2() ? Px(39.333f) : kNotePitch;
}
// Its size: a line 836 px wide there (159.6 s) is 821 at the first console's size.
float NoteSize() {
    return S2() ? 17.7f : kNoteSize;
}

const std::vector<NoteLine>& NoteLines(const std::wstring& noteEs, float w) {
    const std::wstring note = lang::Tr(noteEs); // whole, before it is cut into lines
    static std::map<std::wstring, std::vector<NoteLine>> cache;
    std::wstring key = note + L'\x1' + std::to_wstring(static_cast<int>(w)) + L'\x1' + std::to_wstring(A11yTextEpoch());
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 600) cache.clear();
    std::vector<NoteLine> out;
    size_t at = 0;
    while (at <= note.size()) {
        size_t nl = note.find(L'\n', at);
        std::wstring para = note.substr(at, nl == std::wstring::npos ? std::wstring::npos : nl - at);
        bool bullet = para.compare(0, 2, L"◆ ") == 0;
        if (bullet) para.erase(0, 2);
        bool firstLine = true;
        for (const std::wstring& line : WrapText(para, NoteSize(), bullet ? w - 23 : w)) {
            NoteLine n;
            n.text = line;
            n.bullet = bullet && firstLine;
            n.hang = bullet;
            out.push_back(n);
            firstLine = false;
        }
        if (nl == std::wstring::npos) break;
        at = nl + 1;
    }
    return cache[key] = out;
}

const std::vector<std::wstring>& TextLines(const std::wstring& sEs, float w) {
    const std::wstring s = lang::Tr(sEs); // whole, before it is cut into lines
    static std::map<std::wstring, std::vector<std::wstring>> cache;
    std::wstring key = s + L'\x1' + std::to_wstring(static_cast<int>(w)) + L'\x1' + std::to_wstring(A11yTextEpoch());
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 300) cache.clear();
    return cache[key] = WrapText(s, kTextSize, w);
}

// Measured on walkthrough.mp4 (157 s): the first line of a description is
// centred 29 px under the row's rule, lines are 40 px apart and the next row
// starts 66 px under the last line.
float NoteH(const Row& r, const ListLay& L) {
    if (r.note.empty()) return 0;
    // Second console, re-measured over many pages (rule to rule round a
    // description of 1, 2, 3 and 4 lines: 97, 136-137, 176, 215 px): the first
    // line costs 97 px and each further one 39 1/3.
    if (S2()) return Px(57.667f) + NotePitch() * static_cast<float>(NoteLines(r.note, L.rowW - 34).size());
    return 36.7f + kNotePitch * static_cast<float>(NoteLines(r.note, L.rowW - 34).size());
}

struct RowBox {
    float y, h, fh; // top, whole height, height of the part the cursor frames
};

// Row heights, measured on walkthrough.mp4 (units = px / 1.5):
//   a row 113 px; a blank gap between groups 82 px (46 px when a heading
//   follows); a heading 82 px, 64 px at the top of a page; a slider 169 px;
//   a status-table line 53 px under a 72 px title line; the storage bars
//   244 px; a line of user tiles 336 px.
std::vector<RowBox> LayRows(const std::vector<Row>& rows, const ListLay& L, float& total) {
    std::vector<RowBox> box(rows.size());
    float y = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        float h = L.rowH, fh = L.rowH;
        if (r.heading) {
            // (Second console: straight under a description the heading takes 80 px, 160 s.)
            h = !S2() ? L.headH : i == 0 ? Px(64) : !rows[i - 1].note.empty() ? Px(80) : Px(82);
        } else {
            switch (r.kind) {
            case RK::Plain:
            case RK::Toggle:
            case RK::Radio:
                if (r.icon == 9) fh = h = 128.7f; // a picture beside a two-line label
                h += NoteH(r, L);
                break;
            case RK::Slider:
                h = (S2() ? Px(169) : 112.7f) + NoteH(r, L);
                break;
            case RK::Text: {
                float n = static_cast<float>(TextLines(r.label, L.rowW - 34).size());
                h = r.on ? L.bottom - L.rowY0 - 40 : 24 + kTextPitch * n + (r.note.empty() ? 0.0f : NoteH(r, L) - 30);
                break;
            }
            // (The fact box of a game's options: the row after it starts 251.6 px lower.)
            case RK::Panel: h = r.icon == 1 ? 300.0f : r.icon == 3 ? Px(251.6f) : 158.0f; break;
            case RK::Table: h = r.on ? 48.0f : S2() ? Px(53) : 35.5f; break;
            case RK::Gap:
                // Under a description the gap is only 36 px more (144 s).
                // Second console: a two-line description and this gap are 170 px
                // together (143 s); between groups the next rule is 83 px on in
                // Settings (164 s, 176 s) and 81.5 in the narrower list of a
                // game's options (241 s: 700 to 781 between rows of 112.5).
                h = i > 0 && !rows[i - 1].note.empty() && CanFocus(rows[i - 1]) ? (S2() ? Px(33.667f) : 24.0f)
                    : i + 1 < rows.size() && rows[i + 1].heading ? 32.0f
                    : !S2() ? 55.0f : L.rowW < 600 ? Px(81.5f) : Px(83);
                break;
            case RK::Storage: h = S2() ? Px(244) : 162.7f; break;
            case RK::Users: h = 224; fh = S2() ? Px(320) : 213.3f; break;
            }
        }
        box[i] = {y, h, fh};
        y += h;
    }
    // Measured at the end of several pages: the last row's rule stops at
    // 904 px, 42 px short of where a row in the middle of a page may reach.
    total = y + 28.3f;
    return box;
}

int NextRow(const std::vector<Row>& rows, int from, int d) {
    for (int i = from + d; i >= 0 && i < static_cast<int>(rows.size()); i += d)
        if (CanFocus(rows[i])) return i;
    return -1;
}

// -- row widgets --

// Colours sampled on walkthrough.mp4 (144-157 s, dark theme). Settings was
// only captured in the dark theme: the light values follow that theme's greys.
struct WidgetLook {
    Color off, on, knob, note, dim;
};
WidgetLook WL() {
    Theme T = Th();
    if (!S2()) return {T.sep, T.accent, rgb(0xFFFFFF), T.sub, alpha(T.sub, 0.6f)};
    if (A11yContrast()) // high contrast: the palette's small print, switches that stand out from the page
        // (On the white page the knob is black: a slider's knob has nothing but the page behind it.)
        return {cfg.dark ? rgb(0x8C8C8C) : rgb(0x9A9A9A), cfg.dark ? rgb(0x0B7FCC) : rgb(0x4F8BFF),
                cfg.dark ? rgb(0xFFFFFF) : rgb(0x000000), T.sub, cfg.dark ? rgb(0xB4B4B4) : rgb(0x4A4A4A)};
    if (cfg.dark) return {rgb(0x5A5A5A), rgb(0x33A2D5), rgb(0xEDEAEE), rgb(0xA3A3A3), rgb(0x6A6A6A)};
    return {rgb(0xB3B1B4), rgb(0x0C5FCA), rgb(0xFFFFFF), rgb(0x646369), rgb(0xA9A7AA)};
}

// The switch: 74 x 38 px, its right end 24 px inside the row; the knob is 34 px.
void DrawSwitch(float right, float cy, bool on, float opacity) {
    WidgetLook K = WL();
    const float w = 49.33f, h = 25.33f, x = right - w;
    RRect(x, cy - h / 2, w, h, h / 2, alpha(on ? K.on : K.off, opacity));
    Circle(on ? x + w - 13.33f : x + 13.33f, cy, 11.33f, alpha(K.knob, opacity));
}

// The small pictures in front of a slider. Drawn, not taken from the console.
void SliderIcon(int icon, float x, float cy, Color c) {
    if (icon == 2) { // faster
        Triangle(x, cy - 10, x, cy + 10, x + 12, cy, c);
        Triangle(x + 13, cy - 10, x + 13, cy + 10, x + 25, cy, c);
    } else if (icon == 3) { // microphone
        RRect(x + 7, cy - 12, 9, 17, 4.5f, c);
        Arc(x + 11.5f, cy - 1, 8.5f, 0, 180, c, 2);
        Line(x + 11.5f, cy + 7.5f, x + 11.5f, cy + 12, c, 2);
    } else if (icon == 1) { // loudspeaker
        Rect(x, cy - 4.5f, 7, 9, c);
        Triangle(x + 4, cy, x + 14, cy - 10, x + 14, cy + 10, c);
        Arc(x + 13, cy, 6.5f, -45, 45, c, 2);
        Arc(x + 13, cy, 11.5f, -45, 45, c, 2);
    }
}

void DrawNote(const Row& r, float x, float y, const ListLay& L) {
    WidgetLook K = WL();
    float ly = y + 19.3f;
    for (const NoteLine& n : NoteLines(r.note, L.rowW - 34)) {
        if (n.bullet) {
            PushRotate(45, x + 8.5f, ly);
            Rect(x + 4.5f, ly - 4, 8, 8, K.note);
            PopTransform();
        }
        Text(n.text, x + (n.hang ? 23.0f : 0.0f), ly - 14, L.rowW - 20, 28, NoteSize(), K.note);
        ly += NotePitch();
    }
}

// Free space on the disk the games are on, for the storage bars.
struct DiskInfo {
    double at = -100;
    bool ok = false;
    uint64_t free = 0, total = 0, library = 0;
} disk;
const DiskInfo& Disk() {
    if (now - disk.at > 2) {
        disk.at = now;
        disk.ok = DiskSpace(cfg.gameDirs.empty() ? DataDir() : cfg.gameDirs[0], disk.free, disk.total);
        disk.library = 0;
        for (const Game& g : games) disk.library += g.size;
    }
    return disk;
}

// Everything a row shows, with its cursor. `ry` is the row's top; `sub` is
// the tile the cursor is on in a line of user tiles.
void DrawRow(const Row& r, const RowBox& b, float ry, const ListLay& L, Color lines, bool focused, int sub) {
    Theme T = Th();
    WidgetLook K = WL();
    const bool s2 = S2();
    const float x = L.rowX, right = L.rowX + L.rowW;
    if (r.heading) {
        if (!s2) {
            Text(r.label, x + 4, ry + 18, L.rowW, L.headH - 22, 18, T.sub);
            return;
        }
        // Measured: the bar is 34 px tall and centred 37 px above the rule
        // of the row that follows.
        // (Re-measured at 159.6 s: the bar is 6 x 36 px and starts 53 px above
        // that rule; "Consola" is 112 px wide, which is size 20.5 here.)
        float cy = ry + b.h - 24.7f;
        Rect(x, Snap(ry + b.h) - Px(53), Px(6), Px(36), T.text);
        Text(r.label, x + 14.33f, cy - 17, L.rowW, 34, 20.5f, T.text);
        return;
    }
    switch (r.kind) {
    case RK::Gap: return;
    case RK::Text: {
        const auto& para = TextLines(r.label, L.rowW - 34);
        float n = static_cast<float>(para.size());
        float ly = r.on ? ry + (b.h - n * kTextPitch) / 2 : ry + 12.7f;
        for (const std::wstring& line : para) {
            Text(line, x + 16, ly - 16, L.rowW - 20, 32, kTextSize, T.text, r.on ? Center : Left);
            ly += kTextPitch;
        }
        if (!r.note.empty()) DrawNote(r, x + 16, ly - 14, L);
        return;
    }
    case RK::Table: {
        // A striped table: the title line is lighter and full width, the
        // lines under it are indented by 40 px.
        float tx = r.on ? x - 5 : x + 21.3f;
        if (r.on || r.icon == 1) Rect(tx, ry, right + 5 - tx, b.h, T.tile);
        Text(r.label, tx + (r.on ? 21.0f : 11.0f), ry, L.rowW * 0.6f, b.h, r.on ? 20.5f : 18.5f, r.on ? T.text : K.note);
        Text(r.value, x + L.rowW * 0.4f, ry, L.rowW * 0.6f - 6, b.h, r.on ? 20.5f : 18.5f, r.on ? T.text : K.note,
             Right);
        return;
    }
    case RK::Panel: {
        if (r.icon == 3) {
            // A game's kind of program (walkthrough.mp4 241 s): a 186 px box
            // with the name of the fact over the fact itself.
            if (!s2) {
                RRect(x, ry, L.rowW, 124, 8, T.tile);
                Text(r.label, x + 16, ry + 16, L.rowW - 30, 32, 17.4f, T.text);
                Text(r.value, x + 27, ry + 64, L.rowW - 40, 34, 18.5f, T.text);
                return;
            }
            // The console's: pixel rows 224 to 411, its own grey (dark 60 60 60,
            // light 236 232 236 at 296 s); "Tipo de programa" is 246 px wide
            // from column 816, the fact starts on column 850.
            const float top = Snap(ry + Px(0.5f));
            RRect(x, top, L.rowW, Px(188), 8, cfg.dark ? rgb(0x3C3C3C) : rgb(0xECE8EC));
            Text(r.label, x + 16, top + 18.67f, L.rowW - 30, 32, 20.0f, T.text);
            Text(r.value, x + 37.67f, top + 65.67f, L.rowW - 40, 34, 20.5f, T.text);
            return;
        }
        RRect(x, ry + (r.icon == 1 ? 0.0f : 4.0f), L.rowW, r.icon == 1 ? 276.0f : 142.0f, 8, T.tile);
        if (r.icon == 1) {
            // Where the console shows an illustration and a QR code: plain
            // placeholders, since neither picture is ours to ship.
            float cx = x + L.rowW / 2;
            RRect(cx - 118, ry + 26, 97, 97, 8, alpha(T.sub, 0.35f));
            Circle(cx - 69.5f, ry + 60, 17, alpha(T.text, 0.5f));
            RRect(cx - 96, ry + 84, 53, 39, 12, alpha(T.text, 0.5f));
            RRect(cx + 20, ry + 26, 97, 97, 3, rgb(0xEDEAEE));
            for (int i = 0; i < 3; i++) {
                float qx = cx + 28 + (i == 1 ? 61.0f : 0.0f), qy = ry + 34 + (i == 2 ? 61.0f : 0.0f);
                Rect(qx, qy, 20, 20, rgb(0x1F1F1F));
                Rect(qx + 4, qy + 4, 12, 12, rgb(0xEDEAEE));
                Rect(qx + 7, qy + 7, 6, 6, rgb(0x1F1F1F));
            }
            Text(r.label, x, ry + 172, L.rowW, 30, 18.5f, T.text, Center);
            float ly = ry + 210;
            for (const NoteLine& n : NoteLines(r.note, L.rowW - 34)) {
                Text(n.text, x + 76, ly - 14, L.rowW - 100, 28, kNoteSize, K.note);
                ly += 26.67f;
            }
        } else {
            Text(r.label, x + 16, ry + 16, L.rowW - 30, 30, 17.4f, T.text);
            SliderIcon(1, x + 34, ry + 92, T.text);
            Text(r.value, x + 74, ry + 76, 220, 32, 17.4f, T.text);
            SliderIcon(3, x + 244, ry + 92, T.text);
            Text(r.note, x + 280, ry + 76, 300, 32, 17.4f, T.text);
        }
        return;
    }
    case RK::Storage: {
        // Two bars side by side (476 px each, 72 px apart): the console's
        // memory and its card. Here: the disk the games are on.
        const DiskInfo& d = Disk();
        const float bw = 317.33f;
        auto bar = [&](float bx, const std::wstring& title, const std::wstring& left, const std::wstring& amount,
                       bool live) {
            Color c = live ? T.text : K.dim;
            Text(title, bx + 1, ry, bw, 28, 16.5f, c);
            RRectStroke(bx + 1, ry + 37, bw - 2, 10.67f, 5.33f, c, 2);
            if (live && d.ok && d.total > 0) {
                float used = static_cast<float>(static_cast<double>(d.total - d.free) / static_cast<double>(d.total));
                float lib = (std::min)(
                    used, static_cast<float>(static_cast<double>(d.library) / static_cast<double>(d.total)));
                float in = bw - 8;
                Rect(bx + 4, ry + 40, in * used, 4.67f, rgb(0x8C8C8C));
                Rect(bx + 4, ry + 40, in * lib, 4.67f, rgb(0x1565D8));
            }
            Text(left, bx + 1, ry + 56, bw, 28, 19, c);
            Text(amount, bx, ry + 56, bw - 2, 28, 21, c, Right);
        };
        std::wstring freeText = d.ok ? SizeOf(d.free) : L"—";
        for (wchar_t& ch : freeText)
            if (ch == L'.') ch = L','; // the console writes 59,4 GB
        bar(x, r.label, L"Espacio libre", freeText, true);
        if (!r.value.empty()) bar(x + 365.33f, r.value, r.note, L"", false);
        // Legend: 16 px squares.
        struct Key {
            const wchar_t* name;
            uint32_t colour;
        };
        const Key keys[] = {{L"Programas", 0x1565D8},
                            {L"Datos de guardado", 0x6FA0E0},
                            {L"Imagen/vídeo", 0xE0504E},
                            {L"Otros", 0x8C8C8C}};
        float kx = x;
        for (const Key& k : keys) {
            // Outside identical mode only what the bar really shows is listed.
            if (r.value.empty() && (k.colour == 0x6FA0E0 || k.colour == 0xE0504E)) continue;
            Rect(kx, ry + 110.5f, 16.67f, 16.67f, rgb(k.colour));
            Text(k.name, kx + 24, ry + 104, 240, 30, 18.5f, T.text);
            kx += 24 + TextWidth(k.name, 18.5f) + 21;
        }
        return;
    }
    case RK::Users: {
        // Tiles of 240 x 320 px, 16 px apart, the picture 178 px across.
        for (size_t c = 0; c < r.cells.size(); c++) {
            float tx = x + 5.67f + static_cast<float>(c) * 170.67f;
            int u = r.cells[c];
            if (u < 0 || u >= static_cast<int>(cfg.users.size())) continue;
            // (Second console, dark theme: the cards read 66 66 66 at 150 s, lighter than a focus fill.)
            RRect(tx, ry, 160, 213.33f, 5.33f, !s2 ? T.tile : cfg.dark ? rgb(0x424242) : T.focusBg);
            DrawAvatar(u, tx + 80, ry + 80.67f, 59.33f);
            Text(cfg.users[u].name, tx + 8, ry + 158, 144, 38, 21.5f, T.text, Center);
            if (focused && static_cast<int>(c) == sub) {
                if (s2) CursorFill(T.focusBg);
                Cursor(tx - 2, ry - 2, 164, 217.33f, 6);
            }
        }
        return;
    }
    default: break;
    }

    // Rows with a label: a rule above and below, the label, and at the right
    // end a value, a switch or a radio button.
    const float rh = b.fh;
    Color label = r.disabled ? K.dim : T.text;
    if (r.kind == RK::Slider) {
        // Label centred 72 px under the top, the track 139 px under it: from
        // 128 px inside the row to 32 px from its end, 10 px thick, knob 46 px.
        Text(r.label, x + 16, ry + 28, L.rowW - 200, 40, 22, label);
        if (!r.value.empty()) Text(r.value, right - 216, ry + 28, 200, 40, 21, r.disabled ? K.dim : T.accent, Right);
        const float cy = ry + 92.67f, x0 = x + 85.67f, x1 = right - 21.33f;
        float p = (std::max)(0.0f, (std::min)(1.0f, r.pos));
        float kx = x0 + 15.33f + (x1 - x0 - 30.67f) * p;
        SliderIcon(r.icon, x + 24, cy, label);
        RRect(x0, cy - 3.33f, x1 - x0, 6.67f, 3.33f, K.off);
        if (!r.disabled) RRect(x0, cy - 3.33f, kx - x0, 6.67f, 3.33f, K.on);
        if (focused && s2) Circle(kx, cy, 19.5f, T.focusBg);
        Circle(kx, cy, 15.33f, r.disabled ? K.dim : K.knob);
        if (focused) CircleCursor(kx, cy, s2 ? 19.5f : 18.0f);
        if (!r.note.empty()) DrawNote(r, x + 16, ry + 112.7f, L);
        return;
    }
    // Measured on the second console (138.7 s): the outline of the focused row
    // covers pixel columns 690 to 1741 and reaches 5 px above the row's rule
    // and 6 below its lower one, 6 px wide; the box here is the middle of it.
    const CursorRect frame = {x - Px(11), Snap(ry) - Px(2), L.rowW + Px(22), Px(floorf(rh * 1.5f + 0.5f) + 5), L.radius};
    if (s2) {
        HRule(x, right, ry, lines);
        HRule(x, right, ry + rh, lines);
        if (focused) RRect(frame.x, frame.y, frame.w, frame.h, frame.r, T.focusBg);
    } else {
        Line(x, ry + 0.5f, right, ry + 0.5f, T.sep, 1);
        Line(x, ry + rh + 0.5f, right, ry + rh + 0.5f, T.sep, 1);
    }
    // (Wording 25 px inside the row there, and set a little larger than on the first console.)
    float lx = x + (s2 ? 16.33f : 16.0f), used = 22;
    const float labelSize = s2 ? 22.8f : 22.0f;
    if (r.kind == RK::Radio) {
        // A theme: its colour (97 x 67 px) in front, a 48 px radio button at the end.
        if (r.icon) {
            RRect(x + 15, ry + rh / 2 - 22.33f, 66, 44.67f, 1.5f, T.sub);
            RRect(x + 16, ry + rh / 2 - 21.33f, 64, 42.67f, 1, r.icon == 1 ? rgb(0xEFEDEE) : rgb(0x000000));
            lx = x + 101.67f;
        }
        float cx = right - 42.67f, cy = ry + rh / 2;
        if (r.on) {
            Circle(cx, cy, 16, K.on);
            Circle(cx, cy, 5.33f, K.knob);
            if (s2) label = T.accent;
        } else {
            CircleStroke(cx, cy, 15.33f, K.dim, 1.33f);
        }
        used = 80;
    } else if (r.kind == RK::Toggle) {
        DrawSwitch(right - 16, ry + rh / 2, r.on, r.disabled ? 0.45f : 1.0f);
        used = 80;
    } else if (!r.value.empty()) {
        // (Second console: "Normal" is 116 px wide at 143 s, size 23.2 here.)
        const float valueSize = s2 ? 23.2f : 21.0f;
        float vw = (std::min)(TextWidth(r.value, valueSize), 440.0f);
        Text(r.value, right - vw - 22, ry, vw + 6, rh, valueSize, r.disabled ? K.dim : r.act ? T.accent : T.sub, Right);
        used = vw + 40;
    }
    if (r.icon == 9) {
        // A video's still picture on the console (258 x 146 px): a plain frame here.
        RRect(x + 15, ry + rh / 2 - 48.67f, 172, 97.33f, 2, T.tile);
        Triangle(x + 91, ry + rh / 2 - 14, x + 91, ry + rh / 2 + 14, x + 115, ry + rh / 2, T.sub);
        const auto& two = TextLines(r.label, L.rowW - 230);
        float ly = ry + rh / 2 - (two.size() > 1 ? 16.67f : 0.0f);
        for (size_t i = 0; i < two.size() && i < 2; i++, ly += 33.33f)
            Text(two[i], x + 208, ly - 18, L.rowW - 224, 36, 22, label);
    } else {
        Text(r.label, lx, ry, right - lx - used - 16, rh, labelSize, label);
    }
    if (focused) {
        if (s2) {
            CursorFill(T.focusBg);
            Cursor(frame.x, frame.y, frame.w, frame.h, frame.r);
        } else {
            Cursor(x - 4, ry - 3, L.rowW + 8, rh + 7, L.radius);
        }
    }
    if (!r.note.empty()) DrawNote(r, x + 16, ry + rh, L);
}

int CatById(const ListPage& p, const std::string& id) {
    for (size_t i = 0; i < p.cats.size(); i++)
        if (p.cats[i].id == id) return static_cast<int>(i);
    return -1;
}

void BuildSettings();

// Test states for list pages, the software grid and the user page.
//   settings-cat      Settings, first category focused (A/DOWN/RIGHT from there)
//   settings-rows     Settings, the "NSW-Home" section with its first row focused
//   settings-last     Settings, the last section with its last row focused (long page, scrolled)
//   settings-sec-<id> Settings with the section of that id focused in the left column; ids are
//                     console_menus' ("help", "airplane", "sound", "system"...) and "nswhome"
//   settings-in-<id>  the same with its first row focused; settings-end-<id>: its last row
//   off-<state>       any state (of any screen) with identical mode off; on-<state>: on
//   nocfg-<state>     any state as if the emulator's settings file could not be found
//   s1-<state>        any state in the first console's style, whatever the saved one
//   sys               the home menu with the focus on the Settings button of the bar
//   options-cat       a game's options, first section focused
//   options-in-<n>    a game's options, section n (from 0) with its first row focused
//   options-cal       the same with the capture's title, for measuring text
//   options-open      the home menu on the first game: PLUS opens the panel
//   all-open          the software grid, first tile
//   user-rows         the user page with the profile card focused
//   user-move         the user page; the cursor walks down the history by itself
//   (more user-page states: see DebugUser)
bool DebugLists(const std::wstring& name) {
    auto section = [&](const wchar_t* prefix, int mode) {
        size_t n = wcslen(prefix);
        if (name.compare(0, n, prefix) != 0) return false;
        BuildSettings();
        int cat = CatById(settings, Narrow(name.substr(n)));
        if (cat < 0) return false;
        OpenSettings(cat);
        settings.inRows = mode > 0;
        if (mode == 2) settings.row = 1000; // clamped to the last row by the page
        return true;
    };
    if (name.compare(0, 4, L"off-") == 0 || name.compare(0, 3, L"on-") == 0) {
        cfg.identical = name[1] == L'n';
        BuildSettings();
        DebugGoto(name.substr(cfg.identical ? 3 : 4));
    } else if (name.compare(0, 3, L"s1-") == 0) {
        cfg.menuStyle = 1;
        DebugGoto(name.substr(3));
    } else if (name.compare(0, 6, L"nocfg-") == 0) {
        emuDebugMissing = true;
        DebugGoto(name.substr(6));
    } else if (name == L"sys") {
        homeRow = 2;
        for (int i = 0; i < SysCount(); i++)
            if (SysList()[i].action == 5) si = i;
    } else if (name == L"settings-cat") {
        OpenSettings(0);
    } else if (name == L"settings-rows") {
        OpenSettings(kNSWHomeCat);
        settings.inRows = true;
    } else if (name == L"settings-last") {
        OpenSettings(kNSWHomeCat);
        settings.inRows = true;
        settings.row = 1000; // clamped to the last row by the page
    } else if (section(L"settings-sec-", 0) || section(L"settings-in-", 1) || section(L"settings-end-", 2)) {
    } else if (name == L"options-cat" && !games.empty()) {
        OpenOptions(games[0].id);
        optOpenAt = -10;
    } else if (name.compare(0, 11, L"options-in-") == 0 && name.size() == 12 && !games.empty()) {
        OpenOptions(games[0].id);
        optOpenAt = -10;
        options.cat = (std::min)(static_cast<int>(options.cats.size()) - 1, static_cast<int>(name[11] - L'0')); // wchar_t is unsigned on ARM
        options.inRows = true;
    } else if (name == L"options-cal" && !games.empty()) {
        // For checking text sizes against the capture, which shows this title.
        OpenOptions(games[0].id);
        optOpenAt = -10;
        options.title = L"The Legend of Zelda: Ocarina of Time";
    } else if (name == L"options-open") {
        homeRow = 1;
        gi = 0;
    } else if (name == L"all-open") {
        allSel = 0;
        allScroll = 0;
        Push(Scr::All);
    } else if (name == L"user-rows" || name == L"user-move") {
        pageUser = 0;
        userSec = 0;
        userRow = 0;
        userInRows = true;
        userScroll = 0;
        userScript = name == L"user-move" ? 1 : 0;
        userScriptAt = -1;
        Push(Scr::User);
    } else {
        return false;
    }
    return true;
}

void RowPress(ListPage& p, const Row& r) {
    if (r.kind == RK::Users && r.step) {
        PlaySfx(Sfx::Ok);
        r.step(p.sub);
    } else if (r.act && !r.disabled) {
        PlaySfx(Sfx::Ok);
        r.act();
    } else {
        PlaySfx(Sfx::Bump);
    }
}

// The two columns of a list page: categories on the left, the rows of the
// chosen one on the right. `lines` is the colour of the rules.
void DrawListBody(ListPage& p, float dt, const ListLay& L, bool cursor, Color lines, Color bar) {
    Theme T = Th();
    const bool s2 = S2();
    const int nc = static_cast<int>(p.cats.size());
    if (nc == 0) return;
    if (p.cat >= nc) p.cat = nc - 1;
    if (picker.open) cursor = false;
    // The divider: pixel rows 139 to 956 in Settings, 174 to 954 in a game's options.
    const bool panel = &p == &options;
    if (s2) VRule(L.divX, Px(panel ? 174.0f : 139.0f), Px(panel ? 955.0f : 957.0f), lines);

    // The left column of a long page scrolls. Measured on walkthrough.mp4
    // (Settings, 18 sections, 124-177 s): going down, the focused section
    // stays centred 695 px down the screen; going up, 399 px; the list stops
    // with its last section centred at 841 px. Sections are 122.25 px apart.
    {
        // (Second console, the accent bar at rest all through 124-177 s: rows
        // 654-739 going down, 356-441 going up, 800-885 at the end, so the
        // centres are 697, 399.5 and 843.5 px.)
        const float lo = L.top + (s2 ? 180.33f : 180.0f), hi = L.bottom - (s2 ? 180.28f : 181.3f);
        float most = (std::max)(0.0f, L.catY0 + (nc - 0.5f) * L.catH - (L.bottom - (s2 ? 82.67f : 84.0f)));
        float centre = L.catY0 + (p.cat + 0.5f) * L.catH;
        if (catFor != &p) {
            catFor = &p;
            catGlide.t0 = -1;
        }
        float target = catGlide.t0 < 0 || p.catScroll < 0 ? (std::max)(0.0f, p.catScroll) : catGlide.to;
        if (centre - target > hi) target = centre - hi;
        if (centre - target < lo) target = centre - lo;
        target = (std::max)(0.0f, (std::min)(most, target));
        if (MouseOwns(&p.catScroll)) target = p.catScroll;
        if (p.catScroll < 0) {
            // Just opened: no movement.
            p.catScroll = target;
            catGlide.from = catGlide.to = target;
            catGlide.t0 = now;
        } else if (s2) {
            GlideTo(catGlide, p.catScroll, target);
        } else {
            Smooth(p.catScroll, target, dt);
            if (p.catScroll != target) Touch();
        }
        // Dragged, the focus goes with the sections: to the nearest one centred
        // where the column's own scrolling lets the focused one stand.
        AddScroll(L.catX - 12, L.top, L.catW + 24, L.bottom - L.top, ScrollOf(p.catScroll, most, [&p, L, lo, hi, most, nc](float v) {
            int to = NearestFit(p.cat, nc, [&](int i) {
                const float c = L.catY0 + (i + 0.5f) * L.catH - v;
                return (c >= lo - 0.5f || v <= 0.5f) && (c <= hi + 0.5f || v >= most - 0.5f);
            });
            if (to < 0 || to == p.cat) return;
            listHeldScroll = p.scroll;
            p.cat = to;
            p.row = 0;
            p.scroll = 0;
            p.over = -1;
            p.inRows = false;
        }));
    }

    // On the second console the page shows from the row under the header's
    // rule to the row above the footer's (150 s: a tile cut at rows 128 and 967).
    const float clipTop = s2 ? Px(panel ? 160.0f : 128.0f) : L.top + 1, clipH = s2 ? Px(968) - clipTop : L.bottom - L.top - 1;
    PushClip(L.catX - 12, clipTop, L.catW + 24, clipH);
    for (int i = 0; i < nc; i++) {
        float y = L.catY0 + i * L.catH - p.catScroll;
        if (y > L.bottom || y + L.catH < L.top) continue;
        bool sel = i == p.cat;
        bool focused = sel && !p.inRows;
        // Second console: the outline is 124 px tall and starts on the pixel
        // row 62 px above the section's centre, so neighbours overlap; the
        // accent bar is 6 x 86 px and starts 43 px above the centre.
        const float mid = y + L.catH / 2;
        const CursorRect frame = {L.catX, Snap(mid - Px(62)) + Px(3), L.catW, Px(118), L.radius};
        if (focused && s2 && cursor) RRect(frame.x, frame.y, frame.w, frame.h, frame.r, T.focusBg);
        if (sel && s2) Rect(L.catX + L.catBar, Snap(mid - Px(43)), Px(6), Px(86), T.accent);
        else if (sel) Rect(L.catX + L.catBar, y + 12, 4.0f, L.catH - 24, T.accent);
        // Rules between groups of sections: 400 px long from x 168 px.
        if (p.cats[i].ruleAfter && i + 1 < nc && !(focused && cursor) && !(i + 1 == p.cat && !p.inRows && cursor)) {
            if (s2) HRule(L.catX + 23.33f, L.catX + 290, y + L.catH, lines);
            else Line(L.catX + 23.33f, y + L.catH + 0.5f, L.catX + 290, y + L.catH + 0.5f, T.sep, 1);
        }
        const float tw = L.catW - L.catText - 14, size = s2 ? 21.5f : 22.0f;
        const std::wstring& name = p.cats[i].name;
        if (name.find(L'\n') != std::wstring::npos || TextWidth(name, size) > tw) {
            // The console writes a long name on two lines, 48 px apart.
            std::vector<std::wstring> two = WrapText(name, size, tw);
            for (size_t k = 0; k < two.size() && k < 2; k++)
                Text(two[k], L.catX + L.catText, y + (two.size() > 1 ? (k ? 16.0f : -16.0f) : 0.0f), tw, L.catH, size,
                     sel ? T.accent : T.text);
        } else {
            Text(name, L.catX + L.catText, y, tw, L.catH, size, sel ? T.accent : T.text);
        }
        if (focused && cursor) {
            if (s2) {
                CursorFill(T.focusBg);
                Cursor(frame.x, frame.y, frame.w, frame.h, frame.r);
            } else {
                Cursor(L.catX, y, L.catW, L.catH, L.radius);
            }
        }
        if (y + L.catH / 2 < L.top || y + L.catH / 2 > L.bottom) continue;
        AddHit(L.catX, y, L.catW, L.catH, [&p, i] {
            if (p.cat != i) PlaySfx(Sfx::Move);
            if (p.cat != i) listHeldScroll = p.scroll;
            p.cat = i;
            p.row = 0;
            p.scroll = 0;
            p.over = -1;
            p.inRows = false;
        });
    }
    PopClip();

    // On the second console the rows lag behind the category cursor.
    int shownCat = p.cat;
    if (s2) {
        if (listFor != &p || listShown < 0 || listShown >= nc || now - listDrawnAt > 0.1) {
            listFor = &p;
            listShown = listWanted = p.cat;
        }
        listDrawnAt = now;
        if (listWanted != p.cat) {
            listWanted = p.cat;
            listWantedAt = now;
        }
        if (listShown != p.cat) {
            if (p.inRows || now - listWantedAt >= 0.075) listShown = p.cat;
            else Touch();
        }
        shownCat = listShown;
    }
    const bool stale = shownCat != p.cat;

    std::vector<Row> rows = p.cats[shownCat].rows();
    const int nr = static_cast<int>(rows.size());
    if (!stale && (p.row >= nr || (p.row >= 0 && nr > 0 && !CanFocus(rows[p.row])))) {
        p.row = s2 && p.row >= nr && p.row > 0 ? NextRow(rows, nr, -1) : NextRow(rows, -1, 1);
    }
    // Lay out once to find the selected row, then scroll it into view.
    float total = 0;
    std::vector<RowBox> box = LayRows(rows, L, total);
    const float view = L.bottom - L.rowY0 - 14, most = (std::max)(0.0f, total - view);
    float scroll = p.scroll;
    if (stale) {
        scroll = listHeldScroll;
    } else {
        float target = s2 ? listGlide.to : p.scroll;
        if (s2 && listGlide.t0 < 0) target = p.scroll;
        if (p.over >= 0) {
            // Reading on past the last row (see ListButton).
            target = (std::min)(most, p.over);
        } else if (p.row >= 0 && p.row < nr) {
            float top = box[p.row].y - target, fh = box[p.row].fh;
            if (top < 0) target = box[p.row].y - (p.row > 0 && rows[p.row - 1].heading ? box[p.row - 1].h : 0);
            if (top + fh > view) target = box[p.row].y + fh - view;
            // The first row brings whatever stands above it back with it,
            // and the last one whatever follows it, as far as it fits.
            if (NextRow(rows, p.row, 1) < 0 && most <= box[p.row].y) target = most;
            if (NextRow(rows, p.row, -1) < 0 && box[p.row].y + fh <= view) target = 0;
        } else {
            target = 0;
        }
        if (MouseOwns(&p.scroll)) target = p.scroll;
        if (s2) {
            if (!p.inRows && p.scroll == 0) target = 0; // a category that has just been chosen starts at its top
            GlideTo(listGlide, p.scroll, target);
        } else {
            Smooth(p.scroll, target, dt);
            if (p.scroll != target) Touch();
        }
        scroll = p.scroll;
    }
    // Dragged, the focus moves onto the nearest row wholly in view (into the
    // rows, if it was on the sections); where no row is, the page is read on
    // as with Down past the last row.
    int rowDrag = -1;
    if (!stale) {
        std::vector<std::pair<float, float>> fit(nr, {-1e9f, 0.0f}); // focusable rows: top and height
        for (int i = 0; i < nr; i++)
            if (CanFocus(rows[i])) fit[i] = {box[i].y, box[i].fh};
        rowDrag = AddScroll(L.rowX - 14, clipTop, L.rowW + 28, clipH, ScrollOf(p.scroll, most, [&p, fit, nr, view](float v) {
            int to = NearestFit(p.row, nr, [&](int i) { return fit[i].first - v >= -0.5f && fit[i].first + fit[i].second - v <= view + 0.5f; });
            p.over = -1;
            if (to < 0) {
                p.over = p.overFrom = v;
            } else if (to != p.row) {
                p.row = to;
                p.inRows = true;
            }
        }));
    }

    PushClip(L.rowX - 14, clipTop, L.rowW + 28, clipH);
    // On the second console the focused row is drawn last: its outline lies
    // over the rules of the rows next to it.
    const int onTop = s2 && p.inRows && !stale && cursor && p.row >= 0 && p.row < nr ? p.row : -1;
    for (int k = 0; k < nr + (onTop >= 0 ? 1 : 0); k++) {
        const int i = k < nr ? k : onTop;
        if (k < nr && i == onTop) continue;
        const Row& r = rows[i];
        float ry = L.rowY0 + box[i].y - scroll;
        if (ry > L.bottom + 4 || ry + box[i].h < L.top - 4) continue;
        bool focused = p.inRows && i == p.row && !stale && cursor;
        if (r.kind == RK::Users) p.sub = (std::max)(0, (std::min)(p.sub, static_cast<int>(r.cells.size()) - 1));
        DrawRow(r, box[i], ry, L, lines, focused, p.sub);
        if (stale || !CanFocus(r)) continue;
        if (r.kind == RK::Users) {
            for (size_t c = 0; c < r.cells.size(); c++)
                AddHit(L.rowX + 5.67f + static_cast<float>(c) * 170.67f, ry, 160, 213.33f, [&p, i, c, r] {
                    p.inRows = true;
                    p.row = i;
                    p.sub = static_cast<int>(c);
                    p.over = -1;
                    RowPress(p, r);
                });
            continue;
        }
        AddHit(L.rowX, ry, L.rowW, box[i].fh, [&p, i, r] {
            p.inRows = true;
            p.row = i;
            p.over = -1;
            if (r.kind != RK::Slider) RowPress(p, r);
        });
    }
    PopClip();
    if (s2 && total > L.bottom - L.rowY0) {
        // Scroll position: a 4-unit bar that is always there on a long page
        // (measured at x 1881-1886 px, from 141 to 957 px).
        // Its track is pixel rows 139 to 956 in Settings, 171 to 954 in a game's options.
        const float trackTop = Px(panel ? 171.0f : 139.0f), track = Px(panel ? 955.0f : 957.0f) - trackTop;
        float visible = (L.bottom - L.rowY0) / total;
        float len = (std::max)(30.0f, track * visible), maxScroll = (std::max)(1.0f, total - view);
        float by = Snap(trackTop + (std::min)(1.0f, scroll / maxScroll) * (track - len));
        RRect(L.barX, by, 4, Snap(by + len) - by, 2, bar);
        AddScrollBar(rowDrag, L.barX, trackTop, track, len, maxScroll);
    }
}

void DrawScreen(Scr s, float dt);

// A game's options on the second console: a panel over the menu it was opened
// from, which stays visible, darkened, at both sides. Measured on
// walkthrough.mp4 (dark 238.9-253.0 s, light 294-303 s):
//   panel from x 136 to 1784 px, full height, 0x2E2E2E (light 0xE0DDE1);
//   outside it the menu is darkened by 45 % black (light 36 %).
//   opening (frame 0 = the + press): nothing for 13 frames, except that the
//   menu's cursor goes at about 11; then the panel fades in where it stands,
//   without sliding or growing: its ground in 6 frames, its contents and the
//   darkening in 9; its own cursor cuts in at frame 23.
//   closing (frame 0 = B): the cursor goes, from frame 3 everything fades out
//   (panel 9 frames, darkening 7), and the menu's cursor is back at frame 15.
void DrawOptionsPanel(ListPage& p, float dt) {
    Theme T = Th();
    if (optCloseAt >= 0 && now - optCloseAt >= 0.25 - 0.004) {
        optCloseAt = -1;
        PopNow();
        DrawScreen(stack.back(), dt);
        return;
    }
    float t = static_cast<float>(now - optOpenAt);
    float in = (std::max)(0.0f, (std::min)(1.0f, (t - 0.2f) / 0.15f)); // darkening: even
    float inC = (std::max)(0.0f, (std::min)(1.0f, (t - 0.2f) / (10.0f / 60.0f)));
    inC = inC * inC * (3 - 2 * inC); // contents: measured 0.02 0.11 0.25 0.41 0.55 0.70 0.84 0.91 0.96 0.98
    float ground = (std::max)(0.0f, (std::min)(1.0f, (t - 0.2f) / 0.11f));
    ground = 1 - (1 - ground) * (1 - ground);
    float out = 1, outDim = 1;
    if (optCloseAt >= 0) {
        // Measured from the B press: panel at 0.80 0.60 0.47 0.40 0.20 0.20 0.07
        // in frames 3-9, the darkening gone two frames sooner.
        float tc = static_cast<float>(now - optCloseAt);
        out = powf(1 - (std::max)(0.0f, (std::min)(1.0f, (tc - 2.0f / 60.0f) / (9.0f / 60.0f))), 1.5f);
        outDim = powf(1 - (std::max)(0.0f, (std::min)(1.0f, (tc - 2.0f / 60.0f) / (7.0f / 60.0f))), 1.5f);
    }
    if (t < 0.4f || optCloseAt >= 0) Touch();

    // The menu underneath, without its cursor once the panel is on its way.
    if (stack.size() >= 2) {
        bool was = suppressCursor;
        suppressCursor = was || optCloseAt >= 0 || t >= 11.0f / 60.0f - 0.004f;
        DrawScreen(stack[stack.size() - 2], 0);
        suppressCursor = was;
        hits.clear();
    }
    if (in * out <= 0) return;
    Rect(0, 0, W, H, rgb(cfg.dark ? 0x02000C : 0x000006, (cfg.dark ? 0.52f : 0.336f) * in * outDim));
    const float px = 90.67f, pw = 1098.67f;
    Rect(px, 0, pw, H, alpha(cfg.dark ? rgb(0x2E2E2E) : rgb(0xE0DDE1), ground * out));
    Color lines = cfg.dark ? rgb(0x666666) : rgb(0x979498);

    const bool whole = inC * out >= 1;
    if (!whole) PushLayer(inC * out);
    const ListLay L = PanelLay();
    // Header: the game's picture (104 px), its name, version and publisher.
    Game* g = FindGame(optionsGame);
    if (g) {
        RRect(144, 24, 69.33f, 69.33f, 0, T.tile);
        if (!g->icon.empty()) Image(g->icon, 144, 24, 69.33f, 69.33f);
        Text(p.title, 229.33f, 26, 900, 42, 24.2f, T.text); // 630 px wide for the capture's title
        std::wstring sub = g->pc ? std::wstring(L"Programa de PC") : L"Versión: " + g->version; // (a PC game: what it is)
        if (!g->publisher.empty()) sub += L"  |  " + g->publisher;
        Text(sub, 229.33f, 61, 900, 26, 17.4f, T.sub);
    }
    HRule(122.67f, 1157.33f, Px(159), lines); // pixel rows 159 and 968, columns 184 to 1735
    DrawListBody(p, dt, L, whole && optCloseAt < 0 && t >= 23.0f / 60.0f - 0.004f, lines,
                 cfg.dark ? rgb(0x5F5F5F) : rgb(0xC9C8CD));
    // Its own footer, inset with the panel: the controller mark 74.7 units
    // further in on the left, the hints 90.7 on the right.
    HRule(122.67f, 1157.33f, Px(968), lines);
    if (Img* pad = PackIcon(cfg.dark ? L"pad_d" : L"pad_l")) {
        Image(*pad, 120.67f, 654, 68, 54);
    } else {
        for (int i = 0; i < 4; i++) Rect(125.67f, 661 + i * 10.0f, 6, 6, rgb(cfg.dark ? (i == 0 ? 0x82ED0D : 0x616161) : (i == 0 ? 0x12C007 : 0xB4B1B5)));
        RRect(141.67f, 660, 19, 42, 8, T.text);
        RRect(164.67f, 660, 19, 42, 8, T.text);
    }
    size_t firstHit = hits.size();
    PushOffset(-89.33f, 0);
    DrawHints({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}}, 1, true);
    PopTransform();
    for (size_t i = firstHit; i < hits.size(); i++) hits[i].x -= 89.33f;
    if (!whole) PopLayer();
    if (picker.open) {
        hits.clear(); // the list of choices blocks the page under it
        DrawPicker();
    }
}

void DrawList(ListPage& p, float dt) {
    Theme T = Th();
    const bool s2 = S2();
    if (s2 && &p == &options) {
        DrawOptionsPanel(p, dt);
        return;
    }
    Header(p.icon, p.title);
    DrawListBody(p, dt, LL(), true, T.sep, cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7));
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", s2 ? L"Confirmar" : L"Aceptar", Btn::A}});
    if (picker.open) {
        hits.clear(); // the list of choices blocks the page under it
        DrawPicker();
    }
}

void ListButton(ListPage& p, Btn b) {
    const bool s2 = S2();
    if (s2 && &p == &options && OptionsBusy()) return; // the panel is still on its way in or out
    if (picker.open) {
        PickerButton(b);
        return;
    }
    if (p.cats.empty()) {
        if (b == Btn::B) Pop();
        return;
    }
    std::vector<Row> rows = p.cats[p.cat].rows();
    const int nr = static_cast<int>(rows.size());
    const Row* cur = p.inRows && p.row >= 0 && p.row < nr ? &rows[p.row] : nullptr;
    switch (b) {
    case Btn::Up:
    case Btn::Down: {
        int d = b == Btn::Up ? -1 : 1;
        if (p.inRows) {
            int n = NextRow(rows, p.row, d);
            // Text that continues under the last row is read by going on
            // down: the page moves instead of the cursor.
            const ListLay L = s2 && &p == &options ? PanelLay() : LL();
            float total = 0;
            LayRows(rows, L, total);
            const float most = (std::max)(0.0f, total - (L.bottom - L.rowY0 - 14));
            if (d < 0 && p.over >= 0) {
                p.over -= 150;
                if (p.over <= p.overFrom) p.over = -1;
                Touch();
            } else if (n >= 0) {
                p.row = n;
                p.over = -1;
                PlaySfx(Sfx::Move);
            } else if (d > 0 && (p.over >= 0 ? p.over : p.scroll) < most - 1) {
                if (p.over < 0) p.over = p.overFrom = p.scroll;
                p.over = (std::min)(most, p.over + 150);
                Touch();
            } else {
                PlaySfx(Sfx::Bump);
                if (s2) BumpCursor(0, static_cast<float>(d));
            }
        } else {
            int n = p.cat + d;
            if (n < 0 || n >= static_cast<int>(p.cats.size())) {
                PlaySfx(Sfx::Bump);
                if (s2) BumpCursor(0, static_cast<float>(d));
            } else {
                listHeldScroll = p.scroll;
                p.cat = n;
                p.row = 0;
                p.scroll = 0;
                p.over = -1;
                PlaySfx(Sfx::Move);
            }
        }
        break;
    }
    case Btn::Right:
    case Btn::A:
        if (!p.inRows) {
            if (NextRow(rows, -1, 1) >= 0) {
                p.inRows = true;
                p.row = NextRow(rows, -1, 1);
                p.sub = 0;
                p.over = -1;
                PlaySfx(b == Btn::A ? Sfx::Ok : Sfx::Move);
            } else {
                PlaySfx(Sfx::Bump);
                if (s2 && b == Btn::Right) BumpCursor(1, 0);
            }
        } else if (cur && b == Btn::Right && cur->kind == RK::Slider) {
            if (cur->step && !cur->disabled) cur->step(1);
            else PlaySfx(Sfx::Bump);
        } else if (cur && b == Btn::Right && cur->kind == RK::Users) {
            if (p.sub + 1 < static_cast<int>(cur->cells.size())) {
                p.sub++;
                PlaySfx(Sfx::Move);
            } else {
                PlaySfx(Sfx::Bump);
                if (s2) BumpCursor(1, 0);
            }
        } else if (cur && b == Btn::A) {
            // A slider is moved with left and right; A does nothing on it.
            if (cur->kind == RK::Slider) PlaySfx(Sfx::Bump);
            else RowPress(p, *cur);
        }
        break;
    case Btn::Left:
        if (cur && cur->kind == RK::Slider && cur->step && !cur->disabled) {
            cur->step(-1);
        } else if (cur && cur->kind == RK::Users && p.sub > 0) {
            p.sub--;
            PlaySfx(Sfx::Move);
        } else if (p.inRows) {
            p.inRows = false;
            p.over = -1;
            PlaySfx(Sfx::Move);
        }
        break;
    case Btn::B:
        PlaySfx(Sfx::Back);
        if (p.inRows) {
            p.inRows = false;
            p.over = -1;
        } else if (s2 && &p == &options) {
            optCloseAt = now + 1.0 / 60.0; // the panel fades out by itself, then the screen is popped
            Touch();
        } else Pop();
        break;
    default: break;
    }
}

const wchar_t* Check(bool on) {
    return on ? L"✓" : L"";
}

// ---- settings: the rows ----------------------------------------------------

Row HeadRow(const std::wstring& label) {
    Row r;
    r.label = label;
    r.heading = true;
    return r;
}
Row GapRow() {
    Row r;
    r.kind = RK::Gap;
    return r;
}
Row ActRow(const std::wstring& label, std::function<void()> fn, const std::wstring& note = {}) {
    Row r;
    r.label = label;
    r.act = std::move(fn);
    r.note = note;
    r.real = true;
    return r;
}
Row ValRow(const std::wstring& label, const std::wstring& value, std::function<void()> fn = nullptr) {
    Row r = ActRow(label, std::move(fn));
    r.value = value;
    return r;
}
Row TogRow(const std::wstring& label, bool on, std::function<void()> fn, const std::wstring& note = {}) {
    Row r = ActRow(label, std::move(fn), note);
    r.kind = RK::Toggle;
    r.on = on;
    return r;
}

void AddNote(Row& r, const wchar_t* line) {
    if (!r.note.empty()) r.note += L"\n";
    r.note += line;
}

// What an emulator-backed row says besides its own description. Every such
// change only reaches the next game start; one made while a game is open is
// not even written until that game has closed, which the row then says.
void EmuNote(Row& r, const ryucfg::Setting& s) {
    if (!s.verified) AddNote(r, L"◆ Si el emulador no acepta este valor, usará el predeterminado.");
    if (emuPending.count(s.key)) AddNote(r, L"◆ El cambio se aplicará cuando cierres el programa abierto.");
}

void EmuPick(const std::string& key, const std::wstring& title) {
    const ryucfg::Setting* s = EmuSetting(key);
    if (!s) return;
    std::vector<std::wstring> items;
    int cur = -1;
    const std::string v = EmuGet(key);
    for (size_t i = 0; i < s->choices.size(); i++) {
        items.push_back(s->choices[i].label);
        if (s->choices[i].value == v) cur = static_cast<int>(i);
    }
    Pick(title, items, cur, [key](int i) {
        const ryucfg::Setting* now2 = EmuSetting(key);
        if (now2 && i >= 0 && i < static_cast<int>(now2->choices.size())) EmuSet(key, now2->choices[i].value);
    });
}

// A row for one of the emulator's settings: a switch, a value that opens the
// list of choices, or a slider for a number.
Row EmuRow(const ryucfg::Setting& s, const std::wstring& label, const std::wstring& note) {
    Row r;
    r.label = label;
    r.note = note;
    r.real = true;
    const std::string key = s.key, v = EmuGet(key);
    switch (s.kind) {
    case ryucfg::Kind::Toggle:
        r.kind = RK::Toggle;
        r.on = v == "true";
        r.act = [key] {
            if (const ryucfg::Setting* p = EmuSetting(key)) EmuSet(key, ryucfg::Next(*p, EmuGet(key), 1));
        };
        break;
    case ryucfg::Kind::Choice:
        r.value = ryucfg::Display(s, v);
        r.act = [key, label] { EmuPick(key, label); };
        break;
    case ryucfg::Kind::Number:
        r.kind = RK::Slider;
        r.value = ryucfg::Display(s, v);
        r.pos = s.max > s.min ? static_cast<float>((atof(v.c_str()) - s.min) / (s.max - s.min)) : 0.0f;
        r.icon = key == "audio_volume" ? 1 : 0;
        r.step = [key](int d) {
            const ryucfg::Setting* p = EmuSetting(key);
            if (!p) return;
            const std::string was = EmuGet(key), next = ryucfg::Next(*p, was, d);
            if (next == was) {
                PlaySfx(Sfx::Bump);
                return;
            }
            PlaySfx(Sfx::Move);
            EmuSet(key, next);
        };
        break;
    }
    EmuNote(r, s);
    return r;
}

// The emulator's settings file could not be found: the row stays, greyed, and says why.
void EmuMissing(Row& r) {
    r.kind = r.kind == RK::Toggle ? RK::Toggle : RK::Plain;
    r.disabled = true;
    r.real = true;
    r.act = nullptr;
    r.note = OnAndroid() ? L"En Android este ajuste se cambia en el propio emulador." // [android]
                         : L"No se encuentra la configuración del emulador.";
}

// Emulator settings that already stand behind one of the console's own rows.
bool EmuClaimed(const std::string& key) {
    static const char* const keys[] = {"system_language", "system_region", "system_time_zone",
                                       "res_scale",       "audio_backend", "enable_internet_access", "tv_sound"};
    for (const char* k : keys)
        if (key == EmuKey(k)) return true;
    return false;
}

// The emulator settings of one section that no console row stands for.
std::vector<Row> EmuExtras(const wchar_t* section) {
    std::vector<Row> out;
    if (!EmuReady()) return out;
    for (const ryucfg::Setting& s : EmuList())
        if (s.section == section && !EmuClaimed(s.key) && EmuAvail(s)) out.push_back(EmuRow(s, s.label, s.help));
    return out;
}

// The console leaves a blank gap above these rows. console_menus records the
// gaps only as comments, so they are listed here by the row that follows.
bool GapAbove(const std::string& id) {
    static const char* const ids[] = {
        "internet.status",       "parental.settings",        "accessibility.text_size",
        "accessibility.mono",    "accessibility.tts_heading", "accessibility.gamechat_speech_text",
        "data.software_heading", "data.media_heading",       "controllers.calibrate_sticks",
        "sound.input_heading",   "mii.qr",                   "system.screen_lock",
        "system.serials",        "system.auto_update",       "system.transfer",
        "system.format",         "info.support"};
    for (const char* k : ids)
        if (id == k) return true;
    return false;
}

const std::wstring& FirmwareText() {
    static bool tried = false;
    static std::wstring text = L"—";
    if (!tried) {
        tried = true;
        std::string version, err;
        const std::wstring dir = FirmwareDir(), keys = KeysDir();
        if (!dir.empty() && !keys.empty() && fwsound::FirmwareVersion(dir, keys, version, err)) text = Widen(version);
    }
    return text;
}

// -- users (the console's "Perfiles de usuario") --

// Profiles are mirrored into the emulator, which only reads them at start.
void UsersGuard(const std::function<void()>& fn) {
    if (!runningId.empty()) Info(L"Cierra el programa abierto antes de cambiar los usuarios.");
    else fn();
}
void UserReport(const std::string& err) {
    if (err == "NAME_TAKEN") Info(L"Ya hay un usuario con ese apodo.");
    else if (err == "LAST_USER") Info(L"No se puede eliminar al único usuario.");
    else if (err == "IO") Info(L"No se ha podido guardar el cambio.");
}
void UserAdd() {
    UsersGuard([] { StartAddUser(nullptr); });
}
// A on a user's tile: what can be changed about that user.
void UserMenu(int u) {
    if (u < 0 || u >= static_cast<int>(cfg.users.size())) return;
    std::vector<std::wstring> items = {L"Cambiar apodo", L"Cambiar icono"};
    if (cfg.users.size() > 1) items.push_back(L"Eliminar usuario");
    Pick(UserLabel(u), items, -1, [u](int choice) {
        UsersGuard([u, choice] {
            if (u >= static_cast<int>(cfg.users.size())) return;
            if (choice == 0) {
                entry.open = true;
                entry.title = L"Apodo";
                entry.text = cfg.users[u].name;
                entry.done = [u](const std::wstring& s) {
                    if (!s.empty() && u < static_cast<int>(cfg.users.size())) UserReport(RenameUser(u, s));
                };
            } else if (choice == 1) {
                std::wstring file = PickImage();
                if (file.empty()) return;
                OpenAvatarFrame(u, cfg.users[u].name, file, PicFrame(), [u, file](const PicFrame& f) {
                    if (u >= static_cast<int>(cfg.users.size())) return;
                    RefreshAvatar(u); // the picture on screen keeps its file open: let go of it first
                    UserReport(SetUserImageFramed(u, file, f));
                    RefreshAvatar(u);
                }, nullptr);
            } else if (choice == 2) {
                Ask(L"¿Eliminar a " + UserNoun(u) +
                        (OnAndroid() ? L"?\nDejará de aparecer en la consola." // [android] no saves are kept per user there
                                     : L"?\nSus datos de guardado se quedan en el disco,\npero dejará de aparecer en la consola."),
                    {L"Cancelar", L"Eliminar"}, [u](int b) {
                        if (b == 1 && u < static_cast<int>(cfg.users.size())) UserReport(RemoveUser(u));
                    });
            }
        });
    });
}

void OpenSaveFolder() {
    const std::wstring dir = SaveDataDir();
    if (dir.empty()) {
        Info(OnAndroid() ? L"Los datos de guardado están en el emulador.\nNSW-Home no puede abrirlos." // [android]
                         : L"No hay datos de guardado.");
        return;
    }
    Ask(L"Los datos de guardado están en la carpeta del emulador.\nNSW-Home no borra ni mueve nada.",
        {L"Cancelar", L"Abrir carpeta"}, [dir](int b) {
            if (b == 1) RevealFile(dir);
        });
}

// A console option with two or three known values and nothing to act on.
void CosPick(Row& r, const std::string& id, std::vector<std::wstring> values) {
    r.value = Widen(Cos(id, Narrow(r.value)));
    std::wstring title = r.label;
    int cur = -1;
    for (size_t i = 0; i < values.size(); i++)
        if (values[i] == r.value) cur = static_cast<int>(i);
    r.act = [id, title, values, cur] {
        Pick(title, values, cur, [id, values](int i) {
            if (i >= 0 && i < static_cast<int>(values.size())) SetCos(id, Narrow(values[i]));
        });
    };
}

// ---- settings: what some console rows mean on a PC ---------------------------
// The rows of Audio por Bluetooth, Internet, Control parental, Gestión de
// datos, Perfiles de usuario and Sonido that have a meaning of their own here:
// their state, their pages and what they call (pcsys: the system; emudata: the
// emulators' files; parental: the PIN). The pages are NSW-Home's own, not the
// console's: boxes over the darkened page, in the manner of the list of choices.

// An offscreen test run (every --shot and --shotseq goes through DebugSc):
// nothing of the machine is touched or shown. The connection's values are made
// up (they identify the owner's network and would end up in pictures and in
// the settings listing), nothing opens on the desktop, no sound is played, the
// microphone is not opened and its level is not changed.
bool scTest = false;
int scFakeNet = 1;     // test runs: the made-up connection (0 none, 1 Wi-Fi, 2 cable)
bool scFakeMic = true; // test runs: whether there is a microphone

// A run that must not reach the emulators' real folders: a test run, or one
// pointed at a data folder of its own (where data.cpp, too, keeps away from
// them). The data rows then work on <data>\standin, if someone built one.
bool ScSafe() {
    const std::wstring data = DataDir();
    return scTest || EmuConfigPath().compare(0, data.size(), data) == 0;
}

// (The same word UserLabel gives a user without a name; this one takes the user itself.)
std::wstring ScUserName(const User& u) {
    return u.name.empty() ? std::wstring(L"Usuario") : u.name;
}

std::wstring ScLower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// 59,4 GB as the console writes it; small amounts in MB and KB.
std::wstring ScSize(uint64_t bytes) {
    wchar_t b[40];
    if (bytes >= (1ull << 30)) swprintf_s(b, L"%.1f GB", bytes / 1073741824.0);
    else if (bytes >= (1ull << 20)) swprintf_s(b, L"%.0f MB", bytes / 1048576.0);
    else swprintf_s(b, L"%.0f KB", bytes / 1024.0);
    for (wchar_t& c : b)
        if (c == L'.') c = lang::Cur().decimal;
    return b;
}

std::wstring ScDuration(uint64_t seconds) {
    const uint64_t h = seconds / 3600, m = (seconds % 3600) / 60;
    if (h == 0) return std::to_wstring(m) + L" min";
    if (m == 0) return std::to_wstring(h) + (h == 1 ? L" hora" : L" horas");
    return std::to_wstring(h) + L" h " + std::to_wstring(m) + L" min";
}

std::wstring ScGameName(const std::string& titleId) {
    for (const Game& g : games)
        if (ScLower(Widen(g.titleId)) == ScLower(Widen(titleId))) return g.name;
    return Widen(titleId);
}

// The data folders of the configured emulators, each once (Ryujinx and its
// forks share one). `mainOnly`: just that of the emulator games start with.
std::vector<emudata::Root> ScRoots(bool mainOnly) {
    std::vector<emudata::Root> out;
    const bool safe = ScSafe();
    for (const EmuDef& e : kEmus) {
        const std::string id = e.id;
        if (mainOnly && id != cfg.defaultEmu) continue;
        std::wstring dir;
        if (safe) {
            dir = DataDir() + L"\\standin\\" + Widen(id);
        } else {
            if (!cfg.emulators.count(id)) continue;
            const EmuStatus st = EmuInfo(id);
            if (st.found) dir = st.dataDir;
        }
        if (!emudata::IsDir(dir)) continue;
        bool twice = false;
        for (const emudata::Root& r : out) twice = twice || ScLower(r.dir) == ScLower(dir);
        if (!twice) out.push_back({id, e.family, dir});
    }
    return out;
}

// -- the boxes --

enum class ScKind { None, Net, Tone, Mic, Pin, Order, Busy };

struct ScNetRun {
    std::atomic<int> stage{0}; // 1: the connection has been read; 2: the test address has answered or not
    int kind = 0;
    bool address = false;
    int probe = 1;
};

struct ScSheet {
    ScKind kind = ScKind::None;
    double at = 0;
    std::shared_ptr<ScNetRun> net;
    double toneAt = -1;
    bool micOn = false, micFailed = false;
    float micLevel = 0;
    std::wstring micName;
    std::wstring pinTitle, pinNote;
    int digit[4] = {0, 0, 0, 0};
    int pos = 0;
    std::function<void(const std::string&)> pinDone;
    int sel = 0;
    bool held = false;
    std::wstring busyText;
    bool hold = false; // test states: the waiting box stays up for its picture
} sc;

// Work that can take minutes (copying saves, moving a game) runs beside the
// menu, which shows a waiting box meanwhile and then calls `after`.
struct ScJob {
    std::shared_ptr<std::atomic<bool>> done; // set by the work's own thread, which nobody waits for at exit
    std::function<void()> after;
} scJob;

void ScWaitJob() {
    while (scJob.done && !scJob.done->load()) std::this_thread::yield();
}

void ScOpen(ScKind kind) {
    if (sc.kind == ScKind::Mic) pcsys::MicMeterStop();
    if (sc.kind == ScKind::Tone) pcsys::StopTone();
    const bool onTop = stack.back() == Scr::Extra;
    sc = ScSheet();
    sc.kind = kind;
    sc.at = now;
    picker.open = false;
    if (!onTop) PushNow(Scr::Extra);
    Touch();
}

void ScClose() {
    if (sc.kind == ScKind::Mic) pcsys::MicMeterStop();
    if (sc.kind == ScKind::Tone) pcsys::StopTone();
    sc = ScSheet();
    if (stack.back() == Scr::Extra) PopNow();
}

void ScRun(const std::wstring& text, std::function<void()> work, std::function<void()> after) {
    ScWaitJob(); // one left behind by HOME: let it finish first
    auto done = std::make_shared<std::atomic<bool>>(false);
    scJob.done = done;
    scJob.after = std::move(after);
    std::thread([work, done] {
        work();
        *done = true;
    }).detach();
    ScOpen(ScKind::Busy);
    sc.busyText = text;
}

void ScStartTone() {
    ScOpen(ScKind::Tone);
}

void ScStartMic() {
    ScOpen(ScKind::Mic);
    if (scTest) {
        sc.micOn = scFakeMic;
        sc.micFailed = !scFakeMic;
        sc.micName = scFakeMic ? L"Micrófono (ejemplo)" : L"";
        return;
    }
    sc.micName = pcsys::ReadAudio().input;
    sc.micOn = pcsys::MicMeterStart();
    sc.micFailed = !sc.micOn;
}

void ScStartNet() {
    ScOpen(ScKind::Net);
    auto run = std::make_shared<ScNetRun>();
    sc.net = run;
    const bool test = scTest;
    const int fake = scFakeNet;
    auto work = [run, test, fake] {
        if (test) {
            run->kind = fake;
            run->address = fake != 0;
        } else {
            const pcsys::Net n = pcsys::ReadNet();
            run->kind = n.kind;
            run->address = !n.ipv4.empty() || !n.ipv6.empty();
        }
        run->stage = 1;
        // Without a network there is nobody to ask.
        if (run->kind != 0) run->probe = pcsys::Probe();
        run->stage = 2;
    };
    // A test run draws its frames faster than time passes: it waits here instead.
    if (test) work();
    else std::thread(work).detach();
}

void ScAskPin(const std::wstring& title, std::function<void(const std::string&)> done) {
    ScOpen(ScKind::Pin);
    sc.pinTitle = title;
    sc.pinDone = std::move(done);
}

void ScPinWrong() {
    PlaySfx(Sfx::Error);
    sc.pinNote = L"PIN incorrecto.";
    for (int& d : sc.digit) d = 0;
    sc.pos = 0;
}

void ScSwapUsers(int a, int b) {
    std::swap(cfg.users[a], cfg.users[b]);
    // Whoever was chosen stays chosen.
    for (int* who : {&cfg.currentUser, &homeUser, &pageUser, &whoSel}) *who = *who == a ? b : *who == b ? a : *who;
}

// The box, its title and the line of help at its foot. Everything drawn until
// ScFrameEnd fades in with it. `h`: the height of what goes between the two.
struct ScBox {
    float x, y, w, h; // the area under the title
    float a;
};
ScBox ScFrame(const std::wstring& title, float w, float h, const std::wstring& hint) {
    Theme T = Th();
    if (stack.size() >= 2) {
        const bool was = suppressCursor;
        suppressCursor = true;
        DrawScreen(stack[stack.size() - 2], 0);
        suppressCursor = was;
        hits.clear();
    }
    const float a = (std::min)(1.0f, (static_cast<float>(now - sc.at) + kFrame) / 0.1f);
    if (a < 1) Touch();
    Rect(0, 0, W, H, rgb(0x000000, 0.6f * a));
    const float titleH = 62, hintH = hint.empty() ? 16.0f : 52.0f;
    const float bh = titleH + h + hintH, bx = (W - w) / 2, by = (H - bh) / 2;
    AddHit(0, 0, W, H, [] { OnButtonImpl(Btn::B); });
    AddHit(bx, by, w, bh, [] {});
    PushLayer(a);
    RRect(bx, by, w, bh, S2() ? 8.0f : 5.0f, T.dialog);
    Text(title, bx + 30, by + 4, w - 60, titleH - 4, 21.5f, T.text, Center);
    Line(bx, by + titleH + 0.5f, bx + w, by + titleH + 0.5f, T.sep, 1);
    if (!hint.empty()) {
        Line(bx, by + titleH + h + 0.5f, bx + w, by + titleH + h + 0.5f, T.sep, 1);
        Text(hint, bx + 20, by + titleH + h, w - 40, hintH, 17.4f, T.sub, Center);
    }
    return {bx, by + titleH, w, h, a};
}
void ScFrameEnd() {
    PopLayer();
}

void ScSpinner(float cx, float cy, Color c) {
    const float spin = static_cast<float>(fmod(now * 360.0, 360.0));
    CircleStroke(cx, cy, 11, alpha(c, 0.25f), 2.4f);
    Arc(cx, cy, 11, spin, spin + 110, c, 2.4f);
    Touch();
}

void ScDrawNet() {
    Theme T = Th();
    const ScBox b = ScFrame(L"Prueba de conexión", 660, 3 * 56 + 150, L"A / B  Cerrar");
    const ScNetRun* run = sc.net.get();
    const int stage = run ? run->stage.load() : 0;
    const float t = static_cast<float>(now - sc.at);
    // One line every half second, as the console lists its results, and none before it is known.
    const int shown = (std::min)(stage >= 2 ? 3 : stage >= 1 ? 2 : 0, static_cast<int>(t / 0.5f));
    const std::wstring labels[3] = {L"Red", L"Dirección IP", L"Conexión a internet"};
    std::wstring values[3];
    bool good[3] = {false, false, false};
    if (run && stage >= 1) {
        values[0] = run->kind == 1 ? L"Wi-Fi" : run->kind == 2 ? L"Cable" : L"Sin conexión";
        values[1] = run->address ? L"Asignada" : L"Sin dirección";
        good[0] = run->kind != 0;
        good[1] = run->address;
    }
    if (run && stage >= 2) {
        values[2] = run->probe == 0 ? L"Correcta" : run->probe == 2 ? L"Con inicio de sesión" : L"Sin respuesta";
        good[2] = run->probe == 0;
    }
    for (int i = 0; i < 3; i++) {
        const float y = b.y + 8 + i * 56.0f;
        if (i > 0) Line(b.x + 24, y + 0.5f, b.x + b.w - 24, y + 0.5f, T.sep, 1);
        Text(labels[i], b.x + 36, y, 320, 56, 21.5f, T.text);
        if (i < shown) Text(values[i], b.x + b.w - 336, y, 300, 56, 21.5f, good[i] ? T.accent : T.text, Right);
        else if (i == shown) ScSpinner(b.x + b.w - 50, y + 28, T.sub);
    }
    const float ry = b.y + 8 + 3 * 56.0f;
    Line(b.x + 24, ry + 0.5f, b.x + b.w - 24, ry + 0.5f, T.sep, 1);
    if (shown >= 3 && run) {
        const std::wstring verdict = run->probe == 0   ? L"Prueba de conexión finalizada."
                                     : run->kind == 0  ? L"No hay ninguna red conectada."
                                     : run->probe == 2 ? L"La red pide iniciar sesión en un navegador."
                                                       : L"No se ha podido conectar a internet.";
        Text(verdict, b.x + 20, ry + 10, b.w - 40, 44, 21.5f, T.text, Center);
    } else {
        Touch();
    }
    // Which address is asked, since it is the only thing sent anywhere.
    Text(std::wstring(L"Se consulta http://") + pcsys::kProbeHost + pcsys::kProbePath + L",", b.x + 20, ry + 62, b.w - 40, 26, 16.5f,
         T.sub, Center);
    Text(L"la dirección con la que Windows comprueba la conexión. No se envía nada más.", b.x + 20, ry + 88, b.w - 40, 26, 16.5f,
         T.sub, Center);
    ScFrameEnd();
}

void ScDrawTone() {
    Theme T = Th();
    const ScBox b = ScFrame(L"Prueba de salida de audio", 660, 250, L"A  Repetir      B  Atrás");
    // The press's own sound gets a quarter of a second before the tone takes its place.
    if (sc.toneAt < 0 && now - sc.at >= 0.25) {
        sc.toneAt = now;
        if (!scTest) pcsys::PlayTone();
    }
    const double t = sc.toneAt < 0 ? -1 : now - sc.toneAt;
    const bool left = t >= pcsys::kToneLeftFrom && t < pcsys::kToneLeftTo;
    const bool right = t >= pcsys::kToneRightFrom && t < pcsys::kToneRightTo;
    const bool over = t >= pcsys::kToneLength;
    if (!over) Touch();
    for (int i = 0; i < 2; i++) {
        const bool on = i == 0 ? left : right;
        const float cw = 250, cx = b.x + b.w / 2 + (i == 0 ? -cw - 15 : 15), cy = b.y + 24;
        RRect(cx, cy, cw, 140, 8, on ? T.focusBg : T.tile);
        if (on) RRectStroke(cx, cy, cw, 140, 8, T.accent, 3);
        const Color c = on ? T.accent : T.sub;
        // A loudspeaker, with its waves while it sounds.
        const float sx = cx + cw / 2 - 22, sy = cy + 56;
        Rect(sx, sy - 9, 14, 18, c);
        Triangle(sx + 8, sy, sx + 28, sy - 20, sx + 28, sy + 20, c);
        if (on) {
            Arc(sx + 26, sy, 13, -45, 45, c, 3);
            Arc(sx + 26, sy, 23, -45, 45, c, 3);
        }
        Text(i == 0 ? L"Izquierdo" : L"Derecho", cx, cy + 92, cw, 40, 21.5f, on ? T.accent : T.text, Center);
    }
    const wchar_t* status = left ? L"Suena el altavoz izquierdo." : right ? L"Suena el altavoz derecho." : over ? L"Prueba finalizada." : L"";
    Text(status, b.x + 20, b.y + 184, b.w - 40, 44, 21.5f, T.text, Center);
    ScFrameEnd();
}

// How long the microphone stays open for one test.
constexpr double kScMicSeconds = 8;

void ScDrawMic(float dt) {
    Theme T = Th();
    const ScBox b = ScFrame(L"Prueba de micrófono", 660, 230, L"A  Repetir      B  Atrás");
    if (sc.micOn && now - sc.at >= kScMicSeconds) {
        pcsys::MicMeterStop();
        sc.micOn = false;
    }
    float level = 0;
    if (sc.micOn) {
        // (Test runs never open the microphone: a made-up level moves the bar.)
        level = scTest ? 0.35f + 0.3f * sinf(static_cast<float>(now) * 5.0f) : pcsys::MicMeterLevel();
        if (level < 0) {
            sc.micOn = false; // it was stopped behind our back
            level = 0;
        }
        Touch();
    }
    sc.micLevel = (std::max)(level, sc.micLevel - dt * 1.2f);
    if (!sc.micOn) sc.micLevel = 0;
    Text(sc.micFailed ? std::wstring(L"No se detecta ningún micrófono.") : sc.micName, b.x + 30, b.y + 18, b.w - 60, 36, 19, T.sub,
         Center);
    // Twenty-four segments; a level is heard far below full scale, hence the root.
    const int cells = 24;
    const float bw = 560, x0 = b.x + (b.w - bw) / 2, y0 = b.y + 78, cw = bw / cells;
    const int lit = static_cast<int>(sqrtf((std::min)(1.0f, sc.micLevel)) * cells + 0.5f);
    for (int i = 0; i < cells; i++)
        RRect(x0 + i * cw + 2, y0, cw - 4, 34, 3, i < lit ? (i >= cells - 3 ? rgb(0xE0504E) : T.accent) : T.tile);
    const wchar_t* status = sc.micFailed ? L"" : sc.micOn ? L"Habla por el micrófono: la barra debe moverse." : L"Prueba finalizada.";
    Text(status, b.x + 20, b.y + 132, b.w - 40, 40, 21.5f, T.text, Center);
    if (!sc.micFailed) Text(L"No se graba nada: solo se mide el nivel.", b.x + 20, b.y + 172, b.w - 40, 30, 16.5f, T.sub, Center);
    ScFrameEnd();
}

void ScDrawPin() {
    Theme T = Th();
    const ScBox b = ScFrame(sc.pinTitle, 560, 216, L"↑↓  Cambiar cifra      A  Siguiente      B  Cancelar");
    Text(sc.pinNote, b.x + 20, b.y + 10, b.w - 40, 34, 19, T.accent, Center);
    for (int i = 0; i < 4; i++) {
        const float cw = 70, ch = 92, cx = b.x + b.w / 2 + (i - 2) * 92.0f + 11, cy = b.y + 66;
        const bool focused = i == sc.pos;
        RRect(cx, cy, cw, ch, 8, focused ? T.focusBg : T.tile);
        // The cell in hand shows its figure; the others only that they hold one.
        const std::wstring text = focused ? std::to_wstring(sc.digit[i]) : i < sc.pos ? L"●" : L"";
        Text(text, cx, cy, cw, ch, focused ? 38.0f : 22.0f, T.text, Center);
        if (focused) {
            Triangle(cx + cw / 2 - 9, cy - 8, cx + cw / 2 + 9, cy - 8, cx + cw / 2, cy - 19, T.sub);
            Triangle(cx + cw / 2 - 9, cy + ch + 8, cx + cw / 2 + 9, cy + ch + 8, cx + cw / 2, cy + ch + 19, T.sub);
            if (b.a >= 1) {
                if (S2()) CursorFill(T.focusBg);
                Cursor(cx, cy, cw, ch, 8);
            }
        }
        AddHit(cx, cy - 20, cw, ch + 40, [i] {
            if (sc.pos == i) OnButtonImpl(Btn::Up);
            sc.pos = i;
        });
    }
    ScFrameEnd();
}

void ScDrawOrder() {
    Theme T = Th();
    const int n = static_cast<int>(cfg.users.size());
    sc.sel = (std::max)(0, (std::min)(sc.sel, n - 1));
    const float rowH = 58;
    const ScBox b = ScFrame(L"Cambiar el orden", 560, n * rowH + 16,
                            sc.held ? L"↑↓  Mover      A  Soltar" : L"A  Coger      B  Atrás");
    for (int i = 0; i < n; i++) {
        const float y = b.y + 8 + i * rowH;
        const bool focused = i == sc.sel, held = focused && sc.held;
        if (i > 0) Line(b.x + 24, y + 0.5f, b.x + b.w - 24, y + 0.5f, T.sep, 1);
        if (focused && S2()) RRect(b.x + 10, y + 2, b.w - 20, rowH - 3, 6, T.focusBg);
        Text(std::to_wstring(i + 1), b.x + 26, y, 30, rowH, 19, T.sub, Center);
        DrawAvatar(i, b.x + 86, y + rowH / 2, 21);
        Text(ScUserName(cfg.users[i]), b.x + 124, y, b.w - 220, rowH, 21.5f, held ? T.accent : T.text);
        if (held) {
            // In hand: the two ways it can go.
            const float mx = b.x + b.w - 50, my = y + rowH / 2;
            Triangle(mx - 8, my - 4, mx + 8, my - 4, mx, my - 14, T.accent);
            Triangle(mx - 8, my + 4, mx + 8, my + 4, mx, my + 14, T.accent);
        }
        if (focused && b.a >= 1) {
            if (S2()) CursorFill(T.focusBg);
            Cursor(b.x + 10, y + 2, b.w - 20, rowH - 3, 6);
        }
        AddHit(b.x, y, b.w, rowH, [i] {
            if (sc.held) return;
            sc.sel = i;
            Touch();
        });
    }
    ScFrameEnd();
}

void ScDrawBusy() {
    Theme T = Th();
    const ScBox b = ScFrame(L"Un momento…", 620, 150, L"");
    Text(sc.busyText, b.x + 30, b.y + 14, b.w - 60, 60, 21.5f, T.text, Center);
    ScSpinner(b.x + b.w / 2, b.y + 104, T.sub);
    ScFrameEnd();
    // (A test run shows the box for a fifth of a second and then waits for the work.)
    const bool ready = scTest ? now - sc.at >= 0.2 : scJob.done && scJob.done->load();
    if (!ready || sc.hold) return;
    ScWaitJob();
    auto after = std::move(scJob.after);
    scJob.after = nullptr;
    ScClose();
    if (after) after();
}

void DrawExtra(float dt) {
    switch (sc.kind) {
    case ScKind::Net: ScDrawNet(); break;
    case ScKind::Tone: ScDrawTone(); break;
    case ScKind::Mic: ScDrawMic(dt); break;
    case ScKind::Pin: ScDrawPin(); break;
    case ScKind::Order: ScDrawOrder(); break;
    case ScKind::Busy: ScDrawBusy(); break;
    case ScKind::None:
        PopNow(); // nothing to show (the page was left some other way)
        DrawScreen(stack.back(), dt);
        break;
    }
}

void ExtraButton(Btn b) {
    switch (sc.kind) {
    case ScKind::Net:
        if (b == Btn::A || b == Btn::B) {
            PlaySfx(Sfx::Back);
            ScClose();
        }
        break;
    case ScKind::Tone:
        if (b == Btn::A) {
            pcsys::StopTone();
            sc.at = now - 1; // the box is already there: only the tone starts again
            sc.toneAt = -1;
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            ScClose();
        }
        break;
    case ScKind::Mic:
        if (b == Btn::A && !sc.micOn) ScStartMic();
        else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            ScClose();
        }
        break;
    case ScKind::Pin:
        if (b == Btn::Up || b == Btn::Down) {
            sc.digit[sc.pos] = (sc.digit[sc.pos] + (b == Btn::Up ? 1 : 9)) % 10;
            PlaySfx(Sfx::Move);
        } else if (b == Btn::Left && sc.pos > 0) {
            sc.pos--;
            PlaySfx(Sfx::Move);
        } else if ((b == Btn::Right || b == Btn::A) && sc.pos < 3) {
            sc.pos++;
            PlaySfx(Sfx::Move);
        } else if (b == Btn::A) {
            PlaySfx(Sfx::Ok);
            std::string pin;
            for (int d : sc.digit) pin += static_cast<char>('0' + d);
            auto done = sc.pinDone; // it may open another box, or this one again
            if (done) done(pin);
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            ScClose();
        } else if (b == Btn::Left || b == Btn::Right) {
            PlaySfx(Sfx::Bump);
        }
        break;
    case ScKind::Order: {
        const int n = static_cast<int>(cfg.users.size());
        if (b == Btn::Up || b == Btn::Down) {
            const int to = sc.sel + (b == Btn::Up ? -1 : 1);
            if (to < 0 || to >= n) {
                PlaySfx(Sfx::Bump);
                if (S2()) BumpCursor(0, b == Btn::Up ? -1.0f : 1.0f);
            } else {
                if (sc.held) ScSwapUsers(sc.sel, to);
                sc.sel = to;
                PlaySfx(Sfx::Move);
            }
        } else if (b == Btn::A || (b == Btn::B && sc.held)) {
            PlaySfx(Sfx::Ok);
            sc.held = !sc.held && b == Btn::A;
            if (!sc.held) SaveConfig(); // set down: that is the order now
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            ScClose();
        }
        break;
    }
    case ScKind::Busy: break; // it ends by itself
    case ScKind::None: ScClose(); break;
    }
}

// -- Audio por Bluetooth --

void ScBluetooth() {
    if (scTest) return; // nothing may open on the desktop
    if (!pcsys::OpenBluetoothSettings()) Info(L"No se ha podido abrir la configuración de Bluetooth de Windows.");
}

// -- Internet --

// The connection's values, read when the section comes on screen: the rows are
// built every frame, so a reading is kept while they keep being asked for and
// taken again after three seconds without anyone looking.
struct ScNetNow {
    pcsys::Net net;
    double used = -100;
} scNet;
const pcsys::Net& ScNet() {
    pcsys::Net& net = scNet.net;
    double& used = scNet.used;
    if (now - used > 3 || now < used) {
        if (scTest) {
            // Addresses reserved for documentation: nobody's.
            net = pcsys::Net();
            net.kind = scFakeNet;
            if (scFakeNet != 0) {
                net.mac = L"00-00-5E-00-53-2A";
                net.device = scFakeNet == 1 ? L"Red-de-ejemplo" : L"Adaptador Ethernet de ejemplo";
                net.ipv4 = L"192.0.2.25";
                net.subnet = L"255.255.255.0";
                net.gateway4 = net.dns1_4 = L"192.0.2.1";
                net.ipv6 = L"2001:db8::25/64";
                net.gateway6 = L"fe80::1";
                net.dns1_6 = L"2001:db8::1";
            }
            if (scFakeNet == 1) {
                net.security = L"WPA2-Personal";
                net.band = L"5 GHz";
                net.channel = L"36";
                net.signal = 80;
            }
        } else {
            net = pcsys::ReadNet();
        }
    }
    used = now;
    return net;
}

// What a line of the status table says. A line with nothing to say (the Wi-Fi
// lines on a cable, a second DNS that is not set) shows a dash: no capture
// shows what the console puts there.
std::wstring ScNetValue(const std::string& id) {
    const pcsys::Net& n = ScNet();
    std::wstring v;
    if (id == "internet.status") return n.kind == 1 ? L"Conexión con red Wi-Fi" : n.kind == 2 ? L"Conexión por cable" : L"Sin conexión";
    if (id == "internet.mac") v = n.mac;
    else if (id == "internet.network_name") v = n.device;
    else if (id == "internet.security") v = n.security;
    else if (id == "internet.band") v = n.band;
    else if (id == "internet.channel") v = n.channel;
    else if (id == "internet.signal") v = n.signal >= 0 ? std::to_wstring(n.signal) + L" %" : L"";
    else if (id == "internet.ipv4") v = n.ipv4;
    else if (id == "internet.subnet") v = n.subnet;
    else if (id == "internet.gateway4") v = n.gateway4;
    else if (id == "internet.dns1_4") v = n.dns1_4;
    else if (id == "internet.dns2_4") v = n.dns2_4;
    else if (id == "internet.ipv6") v = n.ipv6;
    else if (id == "internet.gateway6") v = n.gateway6;
    else if (id == "internet.dns1_6") v = n.dns1_6;
    else if (id == "internet.dns2_6") v = n.dns2_6;
    return v.empty() ? L"—" : v;
}

// -- Control parental (NSW-Home's own) --
// A daily play-time limit per user behind a PIN, kept in cfg.console:
//   parental.pin            "<salt>:<hash>" (parental.h); its presence is "on"
//   parental.limit.<user>   minutes a day; absent = no limit
//   parental.day, parental.base.<user>   the day being counted and what each
//                           user's total play time was when it began
// Play time is what main.cpp already adds up per user and game when a game
// closes, so the limit is checked when a game is about to start; nothing can
// be shown over a game that is already running.

bool ScPinSet() {
    return cfg.console.count("parental.pin") != 0;
}

int ScLimit(const std::string& userId) {
    return atoi(Cos("parental.limit." + userId, "0").c_str());
}

std::string ScToday() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

uint64_t ScPlayedToday(const std::string& userId) {
    const std::string today = ScToday(), prefix = userId + "/", key = "parental.base." + userId;
    bool changed = false;
    if (Cos("parental.day", "") != today) {
        for (auto it = cfg.console.begin(); it != cfg.console.end();)
            it = it->first.compare(0, 14, "parental.base.") == 0 ? cfg.console.erase(it) : std::next(it);
        cfg.console["parental.day"] = today;
        changed = true;
    }
    uint64_t total = 0;
    for (const auto& kv : cfg.played)
        if (kv.first.compare(0, prefix.size(), prefix) == 0) total += kv.second.seconds;
    auto it = cfg.console.find(key);
    uint64_t base = it == cfg.console.end() ? total : strtoull(it->second.c_str(), nullptr, 10);
    std::string day = today;
    const uint64_t played = parental::PlayedToday(total, base, day, today);
    if (it == cfg.console.end() || it->second != std::to_string(base)) {
        cfg.console[key] = std::to_string(base);
        changed = true;
    }
    if (changed) SaveConfig();
    return played;
}

void FinishLaunch();
std::string scPassFor; // the PIN was given for this game: its next start goes through

// Called as a game is about to start. True: it must not (a notice is up).
bool ScParentalHolds(const std::string& id) {
    if (!ScPinSet() || cfg.currentUser < 0 || cfg.currentUser >= static_cast<int>(cfg.users.size())) return false;
    const User& u = cfg.users[cfg.currentUser];
    const int limit = ScLimit(u.id);
    if (limit <= 0) return false;
    if (scPassFor == id) {
        scPassFor.clear();
        return false;
    }
    const uint64_t played = ScPlayedToday(u.id);
    if (played < static_cast<uint64_t>(limit) * 60) return false;
    PlaySfx(Sfx::Error);
    Ask(L"Se ha alcanzado el límite de tiempo de juego de hoy.\n\n" + ScUserName(u) + L": " + ScDuration(played) + L" de " +
            ScDuration(static_cast<uint64_t>(limit) * 60) + L".\nPara seguir jugando hace falta el PIN del control parental.",
        {L"Aceptar", L"Introducir PIN"}, [id](int b) {
            if (b != 1) return;
            ScAskPin(L"Introduce el PIN", [id](const std::string& pin) {
                if (!parental::CheckPin(Cos("parental.pin", ""), pin)) {
                    ScPinWrong();
                    return;
                }
                ScClose();
                scPassFor = id;
                pressId = id;
                FinishLaunch();
            });
        });
    return true;
}

void ScParentalMenu();

void ScParentalLimit(int user) {
    if (user < 0 || user >= static_cast<int>(cfg.users.size())) return;
    static const int minutes[] = {0, 15, 30, 60, 90, 120, 180, 240, 360};
    std::vector<std::wstring> items;
    int cur = 0;
    const std::string id = cfg.users[user].id;
    for (size_t i = 0; i < sizeof minutes / sizeof minutes[0]; i++) {
        items.push_back(minutes[i] == 0 ? std::wstring(L"Sin límite") : ScDuration(static_cast<uint64_t>(minutes[i]) * 60));
        if (minutes[i] == ScLimit(id)) cur = static_cast<int>(i);
    }
    Pick(L"Tiempo de juego diario de " + ScUserName(cfg.users[user]), items, cur, [id](int i) {
        if (i < 0 || i >= static_cast<int>(sizeof minutes / sizeof minutes[0])) return;
        if (minutes[i] == 0) cfg.console.erase("parental.limit." + id);
        else cfg.console["parental.limit." + id] = std::to_string(minutes[i]);
        SaveConfig();
        ScParentalMenu();
    });
}

// Asks for a new PIN twice and stores it.
void ScNewPin(std::function<void()> then) {
    ScAskPin(L"Elige un PIN de 4 cifras", [then](const std::string& first) {
        ScAskPin(L"Repite el PIN", [then, first](const std::string& second) {
            if (second != first) {
                ScNewPin(then);
                sc.pinNote = L"Los dos PIN no coinciden. Elige uno de nuevo.";
                PlaySfx(Sfx::Error);
                return;
            }
            ScClose();
            SetCos("parental.pin", parental::MakePin(first));
            if (then) then();
        });
    });
}

void ScParentalMenu() {
    std::vector<std::wstring> items;
    const int n = static_cast<int>(cfg.users.size());
    for (const User& u : cfg.users) {
        const int limit = ScLimit(u.id);
        items.push_back(ScUserName(u) + L": " + (limit > 0 ? ScDuration(static_cast<uint64_t>(limit) * 60) + L" al día" : L"sin límite"));
    }
    items.push_back(L"Cambiar el PIN");
    items.push_back(L"Desactivar el control parental");
    Pick(L"Ajustes del control parental", items, -1, [n](int i) {
        if (i < n) {
            ScParentalLimit(i);
        } else if (i == n) {
            ScNewPin([] { Info(L"El PIN se ha cambiado."); });
        } else {
            Ask(L"¿Desactivar el control parental?\nSe borrarán el PIN y los límites de tiempo de juego.", {L"Cancelar", L"Desactivar"},
                [](int b) {
                    if (b != 1) return;
                    for (auto it = cfg.console.begin(); it != cfg.console.end();)
                        it = it->first.compare(0, 9, "parental.") == 0 ? cfg.console.erase(it) : std::next(it);
                    SaveConfig();
                });
        }
    });
}

void ScParental() {
    if (ScPinSet()) {
        ScAskPin(L"Introduce el PIN", [](const std::string& pin) {
            if (!parental::CheckPin(Cos("parental.pin", ""), pin)) {
                ScPinWrong();
                return;
            }
            ScClose();
            ScParentalMenu();
        });
        return;
    }
    Ask(L"El control parental de NSW-Home limita el tiempo de juego diario de cada usuario.\n\n"
        L"Al alcanzar el límite, iniciar un programa pide un PIN.\nPara empezar, elige un PIN de 4 cifras.",
        {L"Cancelar", L"Continuar"}, [](int b) {
            if (b == 1) ScNewPin([] { ScParentalMenu(); });
        });
}

// -- Gestión de datos --

void ScReport(const emudata::Outcome& o, const std::wstring& done) {
    if (o.ok) Info(done);
    else if (o.error == "NO_SPACE") Info(L"No hay espacio suficiente en el destino.");
    else if (o.error == "EXISTS") Info(L"Ya existe en el destino. No se ha cambiado nada.");
    else if (o.error == "KEPT")
        Info(L"El archivo se ha copiado, pero el original no se ha podido borrar.\n\nEstá en los dos sitios:\n" + o.where);
    else if (o.error == "REFUSED") Info(L"Ese destino no se puede usar. No se ha cambiado nada.");
    else Info(L"No se ha podido completar. No se ha perdido ningún dato.");
}

bool ScGameOpen() {
    if (runningId.empty()) return false;
    Info(L"Cierra el programa abierto antes de continuar.");
    return true;
}

// Borrado rápido: the emulators' caches, per game or all at once.
void ScQuickArchive() {
    struct Entry {
        std::string title; // "" = every game
        uint64_t bytes = 0;
    };
    const std::vector<emudata::Root> roots = ScRoots(false);
    auto all = std::make_shared<std::vector<std::pair<emudata::Root, emudata::Cache>>>();
    std::vector<Entry> entries(1);
    for (const emudata::Root& r : roots)
        for (const emudata::Cache& c : emudata::ListCaches(r)) {
            all->push_back({r, c});
            entries[0].bytes += c.bytes;
            auto it = std::find_if(entries.begin() + 1, entries.end(), [&](const Entry& e) { return e.title == c.titleId; });
            if (it == entries.end()) entries.push_back({c.titleId, c.bytes});
            else it->bytes += c.bytes;
        }
    if (all->empty()) {
        Info(L"No hay nada que borrar.\n\nLos emuladores no tienen cachés guardadas.");
        return;
    }
    std::sort(entries.begin() + 1, entries.end(), [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
    std::vector<std::wstring> items;
    for (const Entry& e : entries)
        items.push_back(ScSize(e.bytes) + L"  ·  " + (e.title.empty() ? std::wstring(L"Todos los programas") : ScGameName(e.title))); // the size first: a long name is cut at its end
    Pick(L"Borrado rápido: cachés del emulador", items, -1, [entries, all](int i) {
        if (i < 0 || i >= static_cast<int>(entries.size())) return;
        const Entry e = entries[i];
        Ask(L"¿Borrar las cachés del emulador?\n" + (e.title.empty() ? std::wstring(L"Todos los programas") : ScGameName(e.title)) +
                L"\n\nSe liberan " + ScSize(e.bytes) +
                L". Son cachés de sombreadores y de código traducido: el emulador las vuelve a crear "
                L"y la primera partida puede ir a tirones. Los datos de guardado no se tocan.",
            {L"Cancelar", L"Borrar"}, [e, all](int b) {
                if (b != 1 || ScGameOpen()) return;
                // One list of folders per emulator; each is checked again where it is removed.
                emudata::Outcome sum;
                sum.ok = true;
                for (size_t k = 0; k < all->size(); k++) {
                    const emudata::Root& r = (*all)[k].first;
                    bool first = true;
                    for (size_t j = 0; j < k; j++) first = first && (*all)[j].first.dir != r.dir;
                    if (!first) continue;
                    std::vector<std::wstring> dirs;
                    for (const auto& rc : *all)
                        if (rc.first.dir == r.dir && (e.title.empty() || rc.second.titleId == e.title)) dirs.push_back(rc.second.dir);
                    if (dirs.empty()) continue;
                    const emudata::Outcome o = emudata::RemoveCaches(r, dirs);
                    sum.bytes += o.bytes;
                    sum.ok = sum.ok && o.ok;
                }
                disk.at = -100; // the free space shown above has changed
                if (sum.ok) Info(L"Se han liberado " + ScSize(sum.bytes) + L".");
                else Info(L"No se ha podido borrar todo.\nSe han liberado " + ScSize(sum.bytes) + L".");
            });
    });
}

// Where copies go: the folder the user chose. A test run cannot ask (no
// window may open) and uses a folder inside its own data.
std::wstring ScChooseFolder() {
    if (!ScSafe()) return PickFolder();
    const std::wstring dir = DataDir() + L"\\standin\\copias";
    return emudata::MakeDir(dir) ? dir : std::wstring();
}

void ScCloudMenu();

void ScCloudCopy() {
    const std::wstring dir = Widen(Cos("data.backup_dir", ""));
    const std::vector<emudata::Root> roots = ScRoots(false);
    uint64_t bytes = 0;
    std::wstring names;
    for (const emudata::Root& r : roots) {
        const uint64_t n = emudata::SavesSize(r);
        if (n == 0) continue;
        bytes += n;
        names += (names.empty() ? L"" : L", ") + EmuLabel(r.emu);
    }
    if (bytes == 0) {
        Info(L"No hay datos de guardado que copiar.");
        return;
    }
    const std::wstring dest = dir + L"\\NSW-Home partidas " + emudata::Stamp();
    Ask(L"¿Copiar los datos de guardado?\n" + names + L": " + ScSize(bytes) + L"\n\nSe copian a:\n" + dest +
            L"\nSolo las partidas guardadas: ni claves, ni firmware, ni ajustes.",
        {L"Cancelar", L"Copiar"}, [roots, dest](int b) {
            if (b != 1) return;
            auto sum = std::make_shared<emudata::Outcome>();
            ScRun(L"Copiando los datos de guardado…",
                  [roots, dest, sum] {
                      sum->ok = emudata::MakeDir(dest);
                      if (!sum->ok) sum->error = "IO";
                      for (const emudata::Root& r : roots) {
                          if (!sum->ok || emudata::SavesSize(r) == 0) continue;
                          const emudata::Outcome o = emudata::BackupSaves(r, dest + L"\\" + Widen(r.emu));
                          sum->bytes += o.bytes;
                          sum->ok = o.ok;
                          sum->error = o.error;
                      }
                  },
                  [sum, dest] {
                      ScReport(*sum, L"Copia terminada: " + ScSize(sum->bytes) + L".\n\n" + dest +
                                         L"\nNSW-Home no restaura copias: para recuperar una, copia a mano su contenido "
                                         L"sobre la carpeta de datos del emulador, con el emulador cerrado.");
                  });
        });
}

// Guardado de datos en la nube: a dated copy of the saves in a folder of the
// user's choosing (one that a cloud program keeps in step, for instance).
void ScCloudMenu() {
    const std::wstring dir = Widen(Cos("data.backup_dir", ""));
    std::vector<std::wstring> items = {L"Hacer una copia ahora", dir.empty() ? std::wstring(L"Elegir la carpeta de las copias…")
                                                                             : L"Cambiar de carpeta  ·  " + dir};
    if (!dir.empty()) items.push_back(L"Abrir la carpeta de las copias");
    Pick(L"Copia de los datos de guardado", items, -1, [dir](int i) {
        if (i == 0 && !dir.empty() && emudata::IsDir(dir)) {
            ScCloudCopy();
        } else if (i == 0 || i == 1) {
            const std::wstring chosen = ScChooseFolder();
            if (chosen.empty()) return;
            SetCos("data.backup_dir", Narrow(chosen));
            if (i == 0) ScCloudCopy();
            else ScCloudMenu();
        } else if (i == 2) {
            RevealFile(dir);
        }
    });
}

// Transferir datos de guardado: where the current user's saves are, and a
// copy of just that user's when the emulator's files say whose is whose.
void ScTransfer() {
    const std::vector<emudata::Root> roots = ScRoots(true);
    if (roots.empty() || emudata::SaveParts(roots[0]).empty() || cfg.currentUser >= static_cast<int>(cfg.users.size())) {
        Info(L"No hay datos de guardado.");
        return;
    }
    const emudata::Root r = roots[0];
    const User u = cfg.users[cfg.currentUser];
    std::string err;
    const auto saves = emudata::UserSaves(r, u.id, err);
    uint64_t bytes = 0;
    for (const auto& s : saves) bytes += emudata::TreeSize(s.second);
    const std::wstring own = emudata::UserSaveFolder(r, u.id);
    const std::wstring folder = !own.empty() && emudata::IsDir(own) ? own : r.dir + L"\\" + emudata::SaveParts(r)[0];
    std::wstring text = L"Datos de guardado de " + ScUserName(u) + L" (" + EmuLabel(r.emu) + L")\n\n" + folder + L"\n";
    const bool canExport = err.empty() && !saves.empty();
    if (err == "NO_INDEX")
        text += L"Este emulador numera las partidas y anota en un índice de quién es cada una. "
                L"Ese índice no se puede leer, así que no es posible exportar solo las de este usuario.";
    else if (saves.empty()) text += L"Este usuario todavía no tiene partidas guardadas.";
    else
        text += std::to_wstring(saves.size()) + (saves.size() == 1 ? L" programa, " : L" programas, ") + ScSize(bytes) +
                (r.family == 'r' ? L". Este emulador numera las partidas; su índice dice cuáles son de este usuario."
                                 : L". Este emulador guarda una carpeta por usuario.");
    std::vector<std::wstring> buttons = {L"Cerrar", L"Abrir carpeta"};
    if (canExport) buttons.push_back(L"Exportar una copia");
    Ask(text, buttons, [r, u, folder](int b) {
        if (b == 1) RevealFile(folder);
        if (b != 2) return;
        const std::wstring dir = ScChooseFolder();
        if (dir.empty()) return;
        const std::wstring dest = dir + L"\\NSW-Home " + ScUserName(u) + L" " + emudata::Stamp();
        auto out = std::make_shared<emudata::Outcome>();
        ScRun(L"Copiando los datos de guardado…", [r, u, dest, out] { *out = emudata::ExportUserSaves(r, u.id, dest); },
              [out, dest] {
                  ScReport(*out, L"Copia terminada: " + ScSize(out->bytes) + L".\n\n" + dest +
                                     L"\nUna carpeta por programa, con su identificador como nombre.");
              });
    });
}

// Mover datos...: one game's file from one folder of the library to another.
struct ScMovable {
    std::wstring name, file;
    uint64_t size = 0;
    int dir = 0; // index into ScLibraryDirs
};

std::vector<std::wstring> ScLibraryDirs() {
    if (!ScSafe()) return cfg.gameDirs;
    // A test run never moves a real game: two stand-in folders, if they are there.
    std::vector<std::wstring> dirs;
    for (const wchar_t* name : {L"lib1", L"lib2"}) {
        const std::wstring d = DataDir() + L"\\standin\\" + name;
        if (emudata::IsDir(d)) dirs.push_back(d);
    }
    return dirs;
}

std::vector<ScMovable> ScMovables(const std::vector<std::wstring>& dirs) {
    std::vector<ScMovable> out;
    if (ScSafe()) {
        for (size_t d = 0; d < dirs.size(); d++)
            for (const auto& f : emudata::ListFiles(dirs[d])) out.push_back({f.first, dirs[d] + L"\\" + f.first, f.second, static_cast<int>(d)});
        return out;
    }
    for (const Game& g : games) {
        const std::wstring file = ScLower(g.file);
        int best = -1;
        // The folder it is listed under (the longest one that holds it).
        for (size_t d = 0; d < dirs.size(); d++) {
            const std::wstring dir = ScLower(dirs[d]);
            if (file.size() > dir.size() + 1 && file.compare(0, dir.size(), dir) == 0 && (file[dir.size()] == L'\\' || file[dir.size()] == L'/') &&
                (best < 0 || dirs[d].size() > dirs[static_cast<size_t>(best)].size()))
                best = static_cast<int>(d);
        }
        if (best >= 0) out.push_back({g.name, g.file, g.size, best});
    }
    return out;
}

void ScMove() {
    const std::vector<std::wstring> dirs = ScLibraryDirs();
    const std::vector<ScMovable> list = ScMovables(dirs);
    if (dirs.size() < 2 || list.empty()) {
        Info(L"No hay nada que mover.");
        return;
    }
    std::vector<std::wstring> items;
    for (const ScMovable& m : list) items.push_back(ScSize(m.size) + L"  ·  " + m.name);
    Pick(L"¿Qué programa quieres mover?", items, -1, [dirs, list](int i) {
        if (i < 0 || i >= static_cast<int>(list.size())) return;
        const ScMovable m = list[i];
        std::vector<std::wstring> places;
        std::vector<int> index;
        for (size_t d = 0; d < dirs.size(); d++) {
            if (static_cast<int>(d) == m.dir) continue;
            uint64_t free = 0;
            const bool known = emudata::FreeSpace(dirs[d], free);
            places.push_back((known ? ScSize(free) + L" libres  ·  " : L"") + dirs[d]);
            index.push_back(static_cast<int>(d));
        }
        Pick(L"¿A qué carpeta?", places, -1, [dirs, index, m](int k) {
            if (k < 0 || k >= static_cast<int>(index.size())) return;
            const std::wstring to = dirs[static_cast<size_t>(index[k])];
            uint64_t free = 0;
            const bool known = emudata::FreeSpace(to, free);
            if (known && free < m.size + (16u << 20)) {
                Info(L"No hay espacio suficiente en esa carpeta.\n\nHacen falta " + ScSize(m.size) + L" y quedan " + ScSize(free) + L".");
                return;
            }
            Ask(L"¿Mover este programa?\n" + m.name + L" (" + ScSize(m.size) + L")\n\nDe: " + dirs[static_cast<size_t>(m.dir)] + L"\nA: " + to +
                    (known ? L"\nEspacio libre en el destino: " + ScSize(free) : L"") +
                    L"\nPrimero se copia y se comprueba; solo entonces se borra el original.",
                {L"Cancelar", L"Mover"}, [m, to](int b) {
                    if (b != 1 || ScGameOpen()) return;
                    auto out = std::make_shared<emudata::Outcome>();
                    ScRun(L"Moviendo el programa…", [m, to, out] { *out = emudata::MoveGameFile(m.file, to); }, [out] {
                        ScReport(*out, L"El programa se ha movido.\n\n" + out->where);
                        disk.at = -100;
                        RescanAsync();
                    });
                });
        });
    });
}

// -- Perfiles de usuario --

void ScReorder() {
    UsersGuard([] { ScOpen(ScKind::Order); });
}

// -- Sonido --

// The default devices and the microphone's level, asked once a second at most
// while the section is on screen.
struct ScAudioNow {
    double at = -100;
    pcsys::Audio dev;
    bool hasMic = false;
    float mic = 1;
} scAudio;
float scTestMic = 1; // test runs: the slider moves this, never the real level

const ScAudioNow& ScAudio() {
    ScAudioNow& a = scAudio;
    if (now - a.at > 1 || now < a.at) {
        a.at = now;
        if (scTest) {
            a.dev.ok = true;
            a.dev.output = L"Auriculares (ejemplo)";
            a.dev.input = scFakeMic ? L"Micrófono (ejemplo)" : L"";
            a.dev.headphones = true;
            a.hasMic = scFakeMic;
            a.mic = scTestMic;
        } else {
            a.dev = pcsys::ReadAudio();
            a.hasMic = pcsys::MicVolume(a.mic);
        }
    }
    return a;
}

void ScMicStep(int d) {
    ScAudio();
    ScAudioNow& a = scAudio;
    // Whole steps of 5 %, from wherever Windows has it.
    const float to = (std::max)(0.0f, (std::min)(1.0f, roundf(a.mic * 20.0f + static_cast<float>(d)) / 20.0f));
    if (!a.hasMic || fabsf(to - a.mic) < 0.001f) {
        PlaySfx(Sfx::Bump);
        return;
    }
    PlaySfx(Sfx::Move);
    if (scTest) scTestMic = a.mic = to;
    else if (pcsys::SetMicVolume(to)) a.mic = to;
}

void ScSoundSwitch(const std::string& id, bool on) {
    SetCos(id, on ? "on" : "off");
    pcsys::SetAudioPolicy(Cos("sound.mute_on_unplug", "off") == "on", Cos("sound.limit_headphones", "off") == "on");
}

// The rows this block stands behind. Returns false for any other row.
bool ScConsoleRow(Row& r, const std::string& id, bool all) {
    const bool pc = pcsys::Available();
    if (id == "bluetooth.add_device" && pc) {
        r.act = ScBluetooth;
        r.real = true;
        r.note = L"Abre la página de Bluetooth de la configuración de Windows, donde se emparejan los auriculares.\n"
                 L"◆ El audio por Bluetooth lo gestiona Windows: NSW-Home solo abre esa página.";
    } else if (id == "internet.test_connection" && pc) {
        r.act = ScStartNet;
        r.real = true;
    } else if (id.compare(0, 9, "internet.") == 0 && (r.kind == RK::Table || id == "internet.status") && pc) {
        // (The table is laid out by the caller; here only what its lines say.
        // They are real in both modes, so the title keeps its table under it.)
        r.kind = RK::Table;
        r.on = id == "internet.status";
        r.value = ScNetValue(id);
        r.real = true;
    } else if (id == "parental.settings") {
        r.act = ScParental;
        r.real = true;
        r.value = ScPinSet() ? L"Activado" : L"";
        r.note = L"Control parental de NSW-Home: un límite de tiempo de juego diario para cada usuario, protegido con un PIN.\n"
                 L"◆ Al alcanzar el límite, iniciar un programa muestra un aviso y pide el PIN.\n"
                 L"◆ NSW-Home no puede mostrar avisos sobre un programa en marcha: el límite solo se comprueba al iniciarlo.";
    } else if (id == "data.quick_archive") {
        r.act = ScQuickArchive;
        r.real = true;
        r.note = all ? L"" : L"Borra las cachés que el emulador puede volver a crear. No toca los datos de guardado.";
    } else if (id == "data.cloud_saves") {
        r.act = ScCloudMenu;
        r.real = true;
    } else if (id == "data.transfer_saves") {
        r.act = ScTransfer;
        r.real = true;
    } else if (id == "data.move" && ScLibraryDirs().size() >= 2) {
        r.act = ScMove;
        r.real = true;
    } else if (id == "users.reorder") {
        r.act = ScReorder;
        r.real = true;
        r.disabled = cfg.users.size() < 2;
    } else if (id == "sound.tv_sound" && !emuKind.empty()) {
        // The yuzu family's sound mode; Ryujinx has no such setting.
        if (const ryucfg::Setting* s = EmuSetting(EmuKey("tv_sound"))) {
            Row made = EmuRow(*s, r.label, r.note);
            made.kind = RK::Plain;
            r = made;
        }
    } else if ((id == "sound.mute_on_unplug" || id == "sound.limit_headphones") && pc) {
        r.on = Cos(id, "off") == "on";
        r.real = true;
        const bool on = r.on, mute = id == "sound.mute_on_unplug";
        r.act = [id, on] { ScSoundSwitch(id, !on); };
        if (!mute) {
            r.note = L"Con unos auriculares como salida predeterminada de Windows, el volumen del programa en el mezclador "
                     L"de Windows no pasa del 50 % (6 dB menos).";
        } else if (pcsys::GameMuted()) {
            r.note = L"◆ El programa está silenciado porque se han desconectado los auriculares. Pulsa A para devolverle el sonido.";
            r.act = [] { pcsys::GameUnmute(); };
        } else {
            r.note = L"Si la salida predeterminada de Windows deja de ser unos auriculares mientras hay un programa abierto, "
                     L"este se silencia en el mezclador de Windows hasta que vuelvan.";
        }
    } else if (id == "sound.test_output" && pc) {
        r.act = ScStartTone;
        r.real = true;
    } else if (id == "sound.mic_input" && pc) {
        const ScAudioNow& a = ScAudio();
        r.value = a.dev.input.empty() ? L"Ninguno" : a.dev.input;
        r.act = nullptr;
        r.real = true;
        r.note = L"El micrófono predeterminado de Windows. Se cambia en la configuración de sonido de Windows.";
    } else if (id == "sound.mic_volume" && pc) {
        const ScAudioNow& a = ScAudio();
        r.pos = a.hasMic ? a.mic : 0.0f;
        r.disabled = !a.hasMic;
        r.step = ScMicStep;
        r.real = true;
    } else if (id == "sound.test_mic" && pc) {
        r.act = ScStartMic;
        r.real = true;
    } else {
        return false;
    }
    return true;
}

// Test states for this block (every --shot / --shotseq name passes through
// here first, which is what marks the run as a test run):
//   wifi-<state> cable-<state> nonet-<state>   any state with that made-up connection
//   nomic-<state>        any state as if there were no microphone
//   users3-<state>       any state with two more users (not saved)
//   limit-<state>        any state with the parental control on (PIN 1234) and the first
//                        user past a one-hour limit: A on a game shows the notice
//   pin1234-<state>      any state with the parental control on (PIN 1234), no limit
//   sc-net sc-tone sc-mic sc-order sc-busy sc-pin   Settings with that box open
//   sc-reached           the home menu with the limit notice up
bool DebugSc(const std::wstring& name) {
    if (!scTest) {
        // Whatever was read before the run was known to be a test is dropped.
        scTest = true;
        scNet = ScNetNow();
        scAudio = ScAudioNow();
    }
    auto prefixed = [&](const wchar_t* prefix) {
        const size_t n = wcslen(prefix);
        if (name.compare(0, n, prefix) != 0) return false;
        DebugGoto(name.substr(n));
        return true;
    };
    auto openIn = [](const char* section) {
        BuildSettings();
        OpenSettings(CatById(settings, section));
        settings.inRows = true;
        // The page is there at once, so that the box can go on top of it.
        if (trans.on) {
            auto apply = trans.apply;
            trans = Transition();
            if (apply) apply();
        }
    };
    if (name.compare(0, 5, L"wifi-") == 0 || name.compare(0, 6, L"cable-") == 0 || name.compare(0, 6, L"nonet-") == 0) {
        scFakeNet = name[0] == L'w' ? 1 : name[0] == L'c' ? 2 : 0;
        DebugGoto(name.substr(name.find(L'-') + 1));
    } else if (name.compare(0, 6, L"nomic-") == 0) {
        scFakeMic = false;
        DebugGoto(name.substr(6));
    } else if (name.compare(0, 7, L"users3-") == 0) {
        cfg.users.push_back({"b59d728aef8ec4b8623ffea599178912", L"Invitado"});
        cfg.users.push_back({"b92807fcc079e41376351c589a0bd4cc", L""});
        DebugGoto(name.substr(7));
    } else if (name.compare(0, 6, L"limit-") == 0 || name.compare(0, 8, L"pin1234-") == 0) {
        const bool limit = name[0] == L'l';
        cfg.console["parental.pin"] = parental::MakePin("1234");
        if (limit && !cfg.users.empty()) {
            const std::string id = cfg.users[0].id;
            // An hour and five minutes played today against a limit of an hour.
            uint64_t total = 0;
            for (const auto& kv : cfg.played)
                if (kv.first.compare(0, id.size() + 1, id + "/") == 0) total += kv.second.seconds;
            if (total < 3900) {
                cfg.played[id + "/test"].seconds = 3900 - total;
                total = 3900;
            }
            cfg.console["parental.limit." + id] = "60";
            cfg.console["parental.day"] = ScToday();
            cfg.console["parental.base." + id] = std::to_string(total - 3900);
        }
        return prefixed(limit ? L"limit-" : L"pin1234-");
    } else if (name == L"sc-net") {
        openIn("internet");
        ScStartNet();
    } else if (name == L"sc-tone") {
        openIn("sound");
        ScStartTone();
    } else if (name == L"sc-mic") {
        openIn("sound");
        ScStartMic();
    } else if (name == L"sc-order") {
        openIn("users");
        ScOpen(ScKind::Order);
    } else if (name == L"sc-pin") {
        openIn("parental");
        ScAskPin(L"Introduce el PIN", [](const std::string&) { ScPinWrong(); });
        sc.digit[0] = 4;
        sc.digit[1] = 7;
        sc.pos = 1;
    } else if (name == L"sc-busy") {
        openIn("data");
        ScRun(L"Copiando los datos de guardado…", [] {}, nullptr);
        sc.hold = true;
    } else if (name == L"sc-reached") {
        DebugGoto(L"limit-home");
        if (!games.empty()) ScParentalHolds(games[0].id);
    } else {
        return false;
    }
    return true;
}

// ---- accessibility -----------------------------------------------------------
// Settings > Accesibilidad, every row but button remapping. All of it is about
// how the MENU looks and speaks; nothing here reaches into a running game.
// The state lives in cfg.console under the console's own row ids, so it is
// saved with the configuration like any other console option:
//   accessibility.text_size       "Normal" | "Grande" | "Muy grande"
//   accessibility.bold            "on" | "off"
//   accessibility.high_contrast   "on" | "off"
//   accessibility.zoom            "on" | "off"   (the function, not the view)
//   accessibility.menu_colours    "Estándar" | "Invertir colores" | "Escala de grises"
//   accessibility.mono            "on" | "off"
//   accessibility.tts             "on" | "off"
//   accessibility.tts_voice       the voice's name as the system gives it
//   accessibility.tts_speed       0..1, the slider
//   accessibility.tts_volume      0..1, the slider
// It is read back every frame (ten look-ups), so whatever changes it, a row or
// a test state, takes effect on the next frame with nothing to notify.

const wchar_t* const kTextSizes[] = {L"Normal", L"Grande", L"Muy grande"};
// How much larger. The pages were laid out for the console's one size: rows
// are 75 units high around 21.5-unit text and descriptions are 26 units
// apart at 17.7, which is what bounds the largest size (see the report for
// the pages that were looked at).
const float kTextScales[] = {1.0f, 1.12f, 1.25f};
const wchar_t* const kMenuColours[] = {L"Estándar", L"Invertir colores", L"Escala de grises"};
// Extra stroke width of bold text, as a fraction of the text size. The
// console font's own weight is matched with about 0.01 (main.cpp); twice
// that again reads as the bold of a font that has one.
constexpr float kBoldBand = 0.03f;
// Two presses of HOME this close together are the zoom gesture.
constexpr double kHomePair = 0.35;

struct A11yTap {
    std::wstring s;
    float x0, y0, x1, y1;
};

struct A11yState {
    bool loaded = false;
    int textSize = 0, colours = 0;
    bool bold = false, contrast = false, zoomOn = false, mono = false, tts = false;
    float ttsSpeed = 0.15f, ttsVolume = 0.2f;
    std::string voice; // as stored: the name, UTF-8
    int textEpoch = 0;

    // The zoomed view. `zoom` and (cx, cy) are what is on screen now and run
    // after their targets.
    bool zoomed = false, adjusting = false;
    float zoom = 1, zoomTo = 2, cx = W / 2, cy = H / 2, tx = W / 2, ty = H / 2;
    double homeAt = -1;    // a HOME press waiting to see whether a second one follows
    bool homePass = false; // that press, let through after all

    // What the frame showed, for the reader and for the view to follow.
    bool focusSeen = false, hadFocus = false;
    float fx = 0, fy = 0, fw = 0, fh = 0;
    std::wstring title;
    std::vector<A11yTap> taps;

    // The reader: what it last said, and what is waiting to have stayed put.
    std::wstring saidOverlay, saidPage, saidItem, saidLabel;
    std::wstring waitKey;
    double waitSince = 0;
} acc;

bool A11yContrast() {
    return acc.contrast;
}

int A11yTextEpoch() {
    return acc.textEpoch;
}

// The high-contrast palettes. What they change against the console's: the
// page goes to pure black or white and text to the opposite; the second
// colour of text (descriptions, values that are not set) comes most of the
// way up to it; every rule is drawn in the text's colour instead of a grey;
// the accent is a blue 7:1 or better against the page. Grounds laid over the
// page (bar, dialog, focused row) keep a step of their own so that they still
// read as surfaces.
Theme A11yTheme() {
    if (S2()) {
        if (cfg.dark)
            return {rgb(0x000000), rgb(0xFFFFFF), rgb(0xE4E4E4), rgb(0xFFFFFF), rgb(0x5CD6FF), rgb(0x5CD6FF),
                    rgb(0x3A3A3A), rgb(0x2C2C2C), rgb(0xFFFFFF), rgb(0x1E1E1E), rgb(0xFFFFFF), rgb(0x000000, 0.0f),
                    rgb(0x1E1E1E), rgb(0x333333), rgb(0x8A8A8A), rgb(0xFFFFFF)};
        return {rgb(0xFFFFFF), rgb(0x000000), rgb(0x2A2A2A), rgb(0x000000), rgb(0x00339A), rgb(0x00339A),
                rgb(0xBDBDBD), rgb(0xFFFFFF), rgb(0x000000), rgb(0xF1F1F1), rgb(0x000000), rgb(0x000000, 0.0f),
                rgb(0xEDEDED), rgb(0xDFDFDF), rgb(0x767676), rgb(0x000000)};
    }
    if (cfg.dark)
        return {rgb(0x000000), rgb(0xFFFFFF), rgb(0xE0E0E0), rgb(0xFFFFFF), rgb(0x5CF0FF), rgb(0x5CFFD9),
                rgb(0x3A3A3A), rgb(0x2C2C2C), rgb(0xFFFFFF), rgb(0x242424), rgb(0xB4B4B4), rgb(0x000000, 0.25f),
                rgb(0x2C2C2C), rgb(0x000000, 0.0f), rgb(0x000000, 0.0f)};
    return {rgb(0xFFFFFF), rgb(0x000000), rgb(0x2A2A2A), rgb(0x000000), rgb(0x005566), rgb(0x0A2BD0),
            rgb(0xC0C0C0), rgb(0xFFFFFF), rgb(0x000000), rgb(0xFFFFFF), rgb(0x5A5A5A), rgb(0x000000, 0.13f),
            rgb(0xFFFFFF), rgb(0x000000, 0.0f), rgb(0x000000, 0.0f)};
}

void A11yTapText(const std::wstring& s, float x0, float y0, float x1, float y1) {
    acc.taps.push_back({s, x0, y0, x1, y1});
}

// One stored value, without building a key string every frame.
const std::string* A11yStored(const std::string& key) {
    auto it = cfg.console.find(key);
    return it == cfg.console.end() ? nullptr : &it->second;
}

// Reads the settings and hands each to whatever carries it out.
void A11yLoad() {
    static const std::string kSize = "accessibility.text_size", kBold = "accessibility.bold",
                             kContrast = "accessibility.high_contrast", kZoom = "accessibility.zoom",
                             kColours = "accessibility.menu_colours", kMono = "accessibility.mono",
                             kTts = "accessibility.tts", kVoice = "accessibility.tts_voice",
                             kSpeed = "accessibility.tts_speed", kVolume = "accessibility.tts_volume";
    auto on = [](const std::string& key) {
        const std::string* v = A11yStored(key);
        return v && *v == "on";
    };
    // A choice is stored as the words shown; a test state may give its number.
    auto choice = [](const std::string& key, const wchar_t* const* names, int count) {
        const std::string* v = A11yStored(key);
        if (!v) return 0;
        for (int i = 0; i < count; i++)
            if (*v == Narrow(names[i]) || (v->size() == 1 && (*v)[0] == '0' + i)) return i;
        return 0;
    };
    auto slider = [](const std::string& key, float def) {
        const std::string* v = A11yStored(key);
        return v ? (std::max)(0.0f, (std::min)(1.0f, static_cast<float>(atof(v->c_str())))) : def;
    };
    const int size = choice(kSize, kTextSizes, 3);
    const bool bold = on(kBold);
    if (size != acc.textSize || bold != acc.bold || !acc.loaded) {
        acc.textSize = size;
        acc.bold = bold;
        acc.textEpoch++; // lines wrapped by measuring are wrapped again
        SetTextAccess(kTextScales[size], bold ? kBoldBand : 0.0f);
    }
    acc.contrast = on(kContrast);
    acc.colours = choice(kColours, kMenuColours, 3);
    SetColorFilter(acc.colours);
    acc.zoomOn = on(kZoom);
    if (!acc.zoomOn) {
        acc.zoomed = false; // switched off while enlarged: back to the whole screen
        acc.homeAt = -1;
    }
    acc.mono = on(kMono);
    a11y::SetMono(acc.mono);
    const bool tts = on(kTts) && a11y::SpeechAvailable();
    if (tts != acc.tts || !acc.loaded) {
        if (acc.tts && !tts) a11y::Silence();
        acc.tts = tts;
        SetTextTap(tts ? A11yTapText : nullptr);
        acc.saidOverlay.clear();
        acc.saidPage.clear();
        acc.saidItem.clear();
        acc.saidLabel.clear();
        acc.waitKey.clear();
    }
    acc.ttsSpeed = slider(kSpeed, 0.15f);
    acc.ttsVolume = slider(kVolume, 0.2f);
    const std::string* voice = A11yStored(kVoice);
    if (voice ? *voice != acc.voice : !acc.voice.empty()) acc.voice = voice ? *voice : std::string();
    acc.loaded = true;
}

// -- the voice --

// The voice in use: the stored one while it is installed, else the first
// (Spanish ones come first). -1 when there is none.
int A11yVoiceIndex() {
    const std::vector<a11y::Voice>& all = a11y::Voices();
    for (size_t i = 0; i < all.size(); i++)
        if (Narrow(all[i].name) == acc.voice) return static_cast<int>(i);
    return all.empty() ? -1 : 0;
}

// The sliders stand where the console's do when untouched (speed 15 %, volume
// 20 %), and those positions have to mean "normal" here too: the voice's own
// pace, and a level that is heard without being the loudest the system can
// make. From there the rest of each track is spread evenly: speed down to -3
// and up to +10 on the engine's scale (-10..10), volume 0 to 80 to 100 (the
// engine's 50 came out a quarter of the full amplitude, too faint beside the
// menu's sounds; 80 is about two thirds).
int A11yRate() {
    const float p = acc.ttsSpeed;
    return static_cast<int>(lroundf(p < 0.15f ? (p - 0.15f) / 0.15f * 3.0f : (p - 0.15f) / 0.85f * 10.0f));
}
int A11yVolume() {
    const float p = acc.ttsVolume;
    return static_cast<int>(lroundf(p < 0.2f ? p / 0.2f * 80.0f : 80.0f + (p - 0.2f) / 0.8f * 20.0f));
}

void A11ySay(const std::wstring& text) {
    const int v = A11yVoiceIndex();
    if (v < 0 || text.empty()) return;
    // As it is said, not as it is set: a line break is a pause, and the
    // diamond in front of a remark is not read out.
    std::wstring said;
    for (wchar_t c : text) {
        if (c == L'\n') said += said.empty() || said.back() == L'.' || said.back() == L' ' ? L" " : L". ";
        else if (c != L'◆') said += c;
    }
    a11y::Speak(said, a11y::Voices()[v].id, A11yRate(), A11yVolume());
}

std::wstring A11yJoin(const std::wstring& a, const std::wstring& b, const wchar_t* sep = L". ") {
    if (a.empty() || b.empty()) return a.empty() ? b : a;
    // (A sentence that ends itself gets no second full stop.)
    const bool ended = sep[0] == L'.' && wcschr(L".?!:", a.back()) != nullptr;
    return a + (ended ? L" " : sep) + b;
}

// What a list row says: its label, then its value or state. `note` gets the
// description under it, read only when the row is first reached.
std::wstring A11yRowText(const ListPage& p, const Row& r, std::wstring& label, std::wstring& note) {
    label = r.label;
    note = r.note;
    std::wstring state;
    switch (r.kind) {
    case RK::Toggle: state = r.on ? L"activado" : L"desactivado"; break;
    case RK::Radio: state = r.on ? L"seleccionado" : L"no seleccionado"; break;
    case RK::Slider: state = std::to_wstring(static_cast<int>(lroundf(r.pos * 100))) + L" por ciento"; break;
    case RK::Users:
        if (p.sub >= 0 && p.sub < static_cast<int>(r.cells.size()) && r.cells[p.sub] >= 0 &&
            r.cells[p.sub] < static_cast<int>(cfg.users.size()))
            label = cfg.users[r.cells[p.sub]].name;
        break;
    default: state = r.value; break;
    }
    if (r.disabled) state = A11yJoin(state, L"no disponible", L", ");
    return A11yJoin(label, state, L", ");
}

// Text drawn inside the cursor's frame, top to bottom and left to right: what
// the focused element says on a page that keeps no list of its own for it.
std::wstring A11yUnderCursor() {
    if (!acc.focusSeen) return {};
    std::vector<const A11yTap*> in;
    for (const A11yTap& t : acc.taps) {
        const float mx = (t.x0 + t.x1) / 2, my = (t.y0 + t.y1) / 2;
        if (mx >= acc.fx - 1 && mx <= acc.fx + acc.fw + 1 && my >= acc.fy - 1 && my <= acc.fy + acc.fh + 1) in.push_back(&t);
    }
    std::stable_sort(in.begin(), in.end(), [](const A11yTap* a, const A11yTap* b) {
        const float ay = (a->y0 + a->y1) / 2, by = (b->y0 + b->y1) / 2;
        return fabsf(ay - by) > 6 ? ay < by : a->x0 < b->x0;
    });
    std::wstring out;
    for (const A11yTap* t : in)
        if (out.size() < t->s.size() || out.compare(out.size() - t->s.size(), t->s.size(), t->s) != 0) out = A11yJoin(out, t->s, L", ");
    return out;
}

// The heading of a page that has no Header() call of its own (the second
// console's software grid and user page draw theirs): the text nearest the
// top left of the band the headings are in.
std::wstring A11yHeading(float bandBottom) {
    const A11yTap* best = nullptr;
    for (const A11yTap& t : acc.taps)
        if ((t.y0 + t.y1) / 2 < bandBottom && (!best || t.y0 < best->y0 - 6 || (fabsf(t.y0 - best->y0) <= 6 && t.x0 < best->x0)))
            best = &t;
    return best ? best->s : std::wstring();
}

// Everything written between the header and the footer, in reading order: what
// a page says when it has nothing to move a cursor over (the controllers
// page, an empty album).
std::wstring A11yBody() {
    std::vector<const A11yTap*> in;
    for (const A11yTap& t : acc.taps)
        if (t.y0 >= 84 && t.y1 <= 648) in.push_back(&t);
    std::stable_sort(in.begin(), in.end(), [](const A11yTap* a, const A11yTap* b) {
        return fabsf(a->y0 - b->y0) > 6 ? a->y0 < b->y0 : a->x0 < b->x0;
    });
    std::wstring out;
    for (const A11yTap* t : in)
        if (out.size() < 400) out = A11yJoin(out, t->s);
    return out;
}

// Decides what the reader has to say after this frame, and says it once it
// has stayed the same for a moment (a held direction runs through a list
// without a word; the item it stops on is read).
void A11yRead() {
    std::wstring overlay, page, item, label, note;
    const Scr top = stack.back();
    if (top == Scr::Home) page = L"Menú HOME";
    else if (top == Scr::Settings) page = settings.title;
    else if (top == Scr::Options) page = options.title;
    else if (top == Scr::Who) page = A11yHeading(640); // the question, the first thing on its sheet
    else if (top == Scr::Launch || top == Scr::Sleep) page.clear();
    else page = acc.title.empty() ? A11yHeading(82) : acc.title;

    if (dlg.open) {
        overlay = dlg.msg;
        if (dlg.sel >= 0 && dlg.sel < static_cast<int>(dlg.btns.size())) item = dlg.btns[dlg.sel];
    } else if (entry.open) {
        overlay = entry.title;
        item = entry.text;
    } else if (picker.open && (top == Scr::Settings || top == Scr::Options)) {
        overlay = picker.title;
        if (picker.sel >= 0 && picker.sel < static_cast<int>(picker.items.size()))
            item = A11yJoin(picker.items[picker.sel], picker.sel == picker.cur ? L"seleccionado" : L"", L", ");
    } else if (top == Scr::Settings || top == Scr::Options) {
        const ListPage& p = top == Scr::Settings ? settings : options;
        if (p.cat >= 0 && p.cat < static_cast<int>(p.cats.size())) {
            if (!p.inRows) {
                item = p.cats[p.cat].name;
            } else {
                const std::vector<Row> rows = p.cats[p.cat].rows();
                if (p.row >= 0 && p.row < static_cast<int>(rows.size())) item = A11yRowText(p, rows[p.row], label, note);
            }
        }
    } else if (top == Scr::Home) {
        const int users = static_cast<int>(cfg.users.size());
        if (homeRow == 0) item = homeUser < users ? L"Página de " + cfg.users[homeUser].name : std::wstring(L"Añadir usuario");
        else if (homeRow == 2 && si >= 0 && si < SysCount()) item = SysList()[si].label;
        else if (homeRow == 1 && gi >= 0 && gi < static_cast<int>(games.size()) && gi < 12)
            item = A11yJoin(games[gi].name, games[gi].id == runningId ? L"en uso" : L"", L", ");
        else if (homeRow == 1 && ShowAllTile()) item = L"Todos los programas";
    } else if (top == Scr::All) {
        if (allSel >= 0 && allSel < static_cast<int>(games.size())) item = games[allSel].name;
    } else if (top == Scr::Who) {
        const int users = static_cast<int>(cfg.users.size());
        item = whoSel >= 0 && whoSel < users ? cfg.users[whoSel].name : std::wstring(L"Añadir usuario");
    } else if ((top == Scr::Album || top == Scr::Viewer) && !shots.empty()) {
        // Pictures say nothing; their place in the album does.
        item = L"Captura " + std::to_wstring(shotSel + 1) + L" de " + std::to_wstring(shots.size());
    } else if (acc.focusSeen) {
        item = A11yUnderCursor();
    } else if (!trans.on && fade <= 0 && top != Scr::Launch && top != Scr::Sleep) {
        item = A11yBody();
    }
    if (label.empty()) label = item;

    if (overlay.empty()) acc.saidOverlay.clear();
    std::wstring say;
    if (!overlay.empty() && overlay != acc.saidOverlay) say = overlay;
    if (overlay.empty() && !page.empty() && page != acc.saidPage) say = page;
    const bool newItem = item != acc.saidItem;
    if (!say.empty() || newItem) say = A11yJoin(say, item);
    // The description is read with the row when it is reached, not again each
    // time its switch is flipped.
    if (newItem && label != acc.saidLabel) say = A11yJoin(say, note);
    // Nothing is said while one screen is giving way to another: what is on
    // it then is neither page, and the heading would be cut short by the
    // first item a moment later.
    if (say.empty() || trans.on || fade > 0) {
        acc.waitKey.clear();
        return;
    }
    if (say != acc.waitKey) {
        acc.waitKey = say;
        acc.waitSince = now;
    }
    Touch(); // keep the frames coming until it has been said
    if (now - acc.waitSince < 0.09) return;
    A11ySay(say);
    acc.waitKey.clear();
    acc.saidOverlay = overlay;
    if (overlay.empty()) acc.saidPage = page;
    acc.saidItem = item;
    acc.saidLabel = label;
}

// -- the hooks the menu calls --

void A11yCursor(float x, float y, float w, float h) {
    if (!acc.tts && !acc.zoomOn) return;
    float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    ToLayout(x0, y0);
    ToLayout(x1, y1);
    acc.fx = x0;
    acc.fy = y0;
    acc.fw = x1 - x0;
    acc.fh = y1 - y0;
    acc.focusSeen = true;
}

void A11yTitle(const std::wstring& title) {
    if (acc.tts) acc.title = title;
}

// Keeps the view's centre where the whole screen still fills the window.
void A11yClampView(float zoom, float& x, float& y) {
    const float hw = W / (2 * zoom), hh = H / (2 * zoom);
    x = (std::max)(hw, (std::min)(W - hw, x));
    y = (std::max)(hh, (std::min)(H - hh, y));
}

// Points the view at the focused element: on its centre, or, when it is wider
// than the view (a row of a list), on its left end, where its label starts.
void A11yAim() {
    if (!acc.hadFocus) return;
    const float vw = W / acc.zoomTo;
    acc.tx = acc.fw > vw * 0.9f ? acc.fx - 24 / acc.zoomTo + vw / 2 : acc.fx + acc.fw / 2;
    acc.ty = acc.fy + acc.fh / 2;
}

void A11yZoomToggle() {
    acc.zoomed = !acc.zoomed;
    acc.adjusting = acc.zoomed;
    if (acc.zoomed) A11yAim();
    PlaySfx(acc.zoomed ? Sfx::Ok : Sfx::Back);
    Touch();
}

// HOME was pressed. True when the press is the zoom's and not the menu's.
// With the function on, one press cannot be told from the first of two until
// a moment has passed, so the menu's own reaction waits that long (as it does
// on the console). While a game has the screen there is nothing to enlarge.
bool A11yHome() {
    if (acc.homePass) return false;
    A11yLoad();
    if (!acc.zoomOn || stack.back() == Scr::Launch || stack.back() == Scr::Sleep) return false;
    if (acc.homeAt >= 0) {
        acc.homeAt = -1;
        A11yZoomToggle();
    } else {
        acc.homeAt = now;
        Touch();
    }
    return true;
}

// A button was pressed. While the view is being set, the buttons are its own:
// the directions move it, X and Y bring it closer and further, A (or one press
// of HOME) keeps it and gives the buttons back to the menu, B leaves it.
bool A11yButton(Btn b) {
    if (!acc.zoomed || !acc.adjusting) return false;
    const float step = 90 / acc.zoomTo;
    switch (b) {
    case Btn::Up: acc.ty -= step; break;
    case Btn::Down: acc.ty += step; break;
    case Btn::Left: acc.tx -= step; break;
    case Btn::Right: acc.tx += step; break;
    case Btn::X: acc.zoomTo = (std::min)(4.0f, acc.zoomTo + 0.5f); break;
    case Btn::Y: acc.zoomTo = (std::max)(1.5f, acc.zoomTo - 0.5f); break;
    case Btn::A: acc.adjusting = false; break;
    case Btn::B:
        acc.zoomed = false;
        PlaySfx(Sfx::Back);
        break;
    default: break;
    }
    A11yClampView(acc.zoomTo, acc.tx, acc.ty);
    Touch();
    return true;
}

// Start of a frame, before anything is drawn or measured.
void A11yFrame(float dt) {
    A11yLoad();
    if (acc.homeAt >= 0) {
        Touch();
        if (now - acc.homeAt >= kHomePair) {
            // No second press came. Enlarged and still being set: this press
            // keeps the view. Otherwise it is the menu's HOME, late.
            acc.homeAt = -1;
            if (acc.zoomed && acc.adjusting) {
                acc.adjusting = false;
            } else {
                acc.homePass = true;
                OnHomePressed();
                acc.homePass = false;
            }
        }
    }
    // Once kept, the view follows the cursor round the menu.
    if (acc.zoomed && !acc.adjusting) A11yAim();
    const float want = acc.zoomed ? acc.zoomTo : 1.0f;
    if (acc.zoom != want || (acc.zoomed && (acc.cx != acc.tx || acc.cy != acc.ty))) {
        const float k = 1.0f - expf(-16.0f * dt);
        acc.zoom += (want - acc.zoom) * k;
        if (fabsf(want - acc.zoom) < 0.003f) acc.zoom = want;
        A11yClampView(acc.zoomTo, acc.tx, acc.ty);
        acc.cx += (acc.tx - acc.cx) * k;
        acc.cy += (acc.ty - acc.cy) * k;
        if (fabsf(acc.tx - acc.cx) < 0.05f) acc.cx = acc.tx;
        if (fabsf(acc.ty - acc.cy) < 0.05f) acc.cy = acc.ty;
        Touch();
    }
    if (acc.zoom > 1.0f) {
        float x = acc.cx, y = acc.cy;
        A11yClampView(acc.zoom, x, y);
        SetView(acc.zoom, x, y);
    } else {
        SetView(1.0f, W / 2, H / 2);
        acc.cx = acc.tx = W / 2;
        acc.cy = acc.ty = H / 2;
    }
    acc.focusSeen = false;
    acc.taps.clear();
    acc.title.clear();
}

// End of a frame, over everything else.
void A11yEndFrame() {
    if (acc.focusSeen) acc.hadFocus = true;
    if (acc.zoom > 1.0f) {
        // The frame that says the view is enlarged: green while it is being
        // set, grey once kept. Drawn in the page's own units round what is
        // visible, so sizes are divided by the enlargement.
        float x = acc.cx, y = acc.cy;
        A11yClampView(acc.zoom, x, y);
        const float z = acc.zoom, vw = W / z, vh = H / z, vx = x - vw / 2, vy = y - vh / 2, sw = 5 / z;
        const float a = (std::min)(1.0f, (z - 1.0f) * 4.0f);
        RRectStroke(vx + sw / 2, vy + sw / 2, vw - sw, vh - sw, 0, alpha(acc.adjusting ? rgb(0x2ECC71) : rgb(0x9A9A9A), a), sw);
        if (acc.adjusting && acc.zoomed) {
            const std::wstring help = L"Cruceta: mover     X: acercar     Y: alejar     A: fijar     B: salir";
            const float size = 15 / z, tw = TextWidth(help, size) + 28 / z, th = 30 / z;
            const float bx = vx + (vw - tw) / 2, by = vy + vh - th - 12 / z;
            RRect(bx, by, tw, th, th / 2, rgb(0x000000, 0.78f * a));
            Text(help, bx, by, tw, th, size, rgb(0xFFFFFF, a), Center);
        }
    }
    if (acc.tts) A11yRead();
}

// A row of the Accesibilidad section (`r` already made as the console's kind
// of row; its switch or slider already remembers its state under `id`).
void A11yRow(Row& r, const std::string& id) {
    const std::wstring title = r.label;
    const bool speech = a11y::SpeechAvailable();
    if (id == "accessibility.text_size") {
        r.value = kTextSizes[acc.textSize];
        r.act = [title] {
            Pick(title, {kTextSizes[0], kTextSizes[1], kTextSizes[2]}, acc.textSize,
                 [](int i) { SetCos("accessibility.text_size", Narrow(kTextSizes[i])); });
        };
        r.real = true;
    } else if (id == "accessibility.bold" || id == "accessibility.high_contrast") {
        r.real = true;
    } else if (id == "accessibility.zoom") {
        r.real = true;
        AddNote(r, L"◆ Aquí, HOME es el botón Guía del mando o la tecla Inicio del teclado. Con la imagen ampliada: "
                   L"cruceta para moverla, X e Y para acercarla y alejarla, A para fijarla y seguir usando el menú.");
        AddNote(r, L"◆ Solo amplía el menú, no los programas.");
    } else if (id == "accessibility.menu_colours") {
        r.value = kMenuColours[acc.colours];
        r.act = [title] {
            Pick(title, {kMenuColours[0], kMenuColours[1], kMenuColours[2]}, acc.colours,
                 [](int i) { SetCos("accessibility.menu_colours", Narrow(kMenuColours[i])); });
        };
        r.real = true;
        r.note = L"Cambia los colores del menú, imágenes incluidas. No cambia los de los programas.";
    } else if (id == "accessibility.mono") {
        r.real = true;
        r.note = L"Los sonidos del menú se reproducen por un solo canal.\n"
                 L"◆ El sonido de los programas depende del emulador y no cambia con esta opción.";
    } else if (id == "accessibility.tts" && speech) {
        r.real = true;
        // (The console's second line, about holding a stick down to hear the
        // text again, has nothing behind it here and is left out.)
        r.note = L"Se leerá en voz alta el texto que se muestra en pantalla, con las voces instaladas en el sistema.\n"
                 L"◆ Esta función podría no estar disponible en algunas pantallas.";
    } else if (id == "accessibility.tts_voice" && speech) {
        const std::vector<a11y::Voice>& all = a11y::Voices();
        const int cur = A11yVoiceIndex();
        r.value = cur >= 0 ? all[cur].name : std::wstring();
        r.act = [title, cur] {
            std::vector<std::wstring> names;
            for (const a11y::Voice& v : a11y::Voices()) names.push_back(v.language.empty() ? v.name : v.name + L" (" + v.language + L")");
            Pick(title, names, cur, [](int i) {
                const std::vector<a11y::Voice>& voices = a11y::Voices();
                if (i >= 0 && i < static_cast<int>(voices.size())) SetCos("accessibility.tts_voice", Narrow(voices[i].name));
            });
        };
        r.real = true;
    } else if ((id == "accessibility.tts_speed" || id == "accessibility.tts_volume") && speech) {
        r.real = true;
    }
}

// Test states.
//   a11y:<key>=<value>[,<key>=<value>...]:<state>
//       any state with those Accesibilidad settings (keys without the
//       "accessibility." prefix; nothing is saved), e.g.
//       "a11y:bold=on,text_size=2:settings-in-accessibility-2". A choice may be
//       given by its number.
//   zoomed-<state>          any state with the view enlarged and kept (needs zoom=on)
//   a11yvoices:<file>       the installed voices, and for each the length of a
//                           sentence spoken into memory (nothing is played)
//   a11ywav:<file>          "Configuración de la consola" in the current voice,
//                           speed and volume, as a WAV file
//   a11ymono:<file>         each menu sound of the installed firmware before and
//                           after the mono fold-down
bool DebugA11y(const std::wstring& name) {
    auto save = [](const std::wstring& file, const std::string& bytes) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, file.c_str(), L"wb") && f) {
            fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
        }
    };
    if (name.compare(0, 5, L"a11y:") == 0) {
        const size_t end = name.find(L':', 5);
        if (end == std::wstring::npos) return false;
        std::wstring list = name.substr(5, end - 5);
        for (size_t at = 0; at < list.size();) {
            size_t comma = list.find(L',', at);
            if (comma == std::wstring::npos) comma = list.size();
            const std::wstring item = list.substr(at, comma - at);
            const size_t eq = item.find(L'=');
            if (eq != std::wstring::npos) cfg.console["accessibility." + Narrow(item.substr(0, eq))] = Narrow(item.substr(eq + 1));
            at = comma + 1;
        }
        A11yLoad();
        DebugGoto(name.substr(end + 1));
        return true;
    }
    if (name.compare(0, 7, L"zoomed-") == 0) {
        DebugGoto(name.substr(7));
        acc.zoomed = true;
        acc.adjusting = false;
        return true;
    }
    if (name.compare(0, 11, L"a11yvoices:") == 0) {
        std::string out = "name\tlanguage\tbytes\tseconds\n";
        for (const a11y::Voice& v : a11y::Voices()) {
            std::vector<uint8_t> wav;
            const bool ok = a11y::SpeakToWav(L"Configuración de la consola", v.id, 0, 100, wav);
            char line[64];
            snprintf(line, sizeof(line), "\t%d\t%.2f\n", ok ? static_cast<int>(wav.size()) : -1, ok ? (wav.size() - 44) / 44100.0 : 0.0);
            out += Narrow(v.name) + "\t" + Narrow(v.language) + line;
        }
        save(name.substr(11), out);
        return true;
    }
    if (name.compare(0, 8, L"a11ywav:") == 0) {
        A11yLoad();
        std::vector<uint8_t> wav;
        const int v = A11yVoiceIndex();
        if (v >= 0 && a11y::SpeakToWav(L"Configuración de la consola", a11y::Voices()[v].id, A11yRate(), A11yVolume(), wav))
            save(name.substr(8), std::string(wav.begin(), wav.end()));
        return true;
    }
    if (name.compare(0, 9, L"a11ymono:") == 0) {
        std::string out = "sound\tchannels\tbytes\tmono channels\tmono bytes\tworst error\n";
        std::vector<fwsound::Sound> sounds;
        std::string err;
        if (!fwsound::LoadMenuSounds(FirmwareDir(), KeysDir(), sounds, err)) out += "no firmware sounds: " + err + "\n";
        // One made here as well, so that the check does not depend on the firmware: left and right differ.
        fwsound::Sound made;
        made.name = "(made: L 1000, R -3000)";
        made.wav.assign(44 + 400, 0);
        auto put = [&made](size_t at, uint32_t v, int bytes) { memcpy(&made.wav[at], &v, bytes); };
        memcpy(&made.wav[0], "RIFF", 4);
        put(4, 36 + 400, 4);
        memcpy(&made.wav[8], "WAVEfmt ", 8);
        put(16, 16, 4);
        put(20, 1, 2);
        put(22, 2, 2);
        put(24, 44100, 4);
        put(28, 44100 * 4, 4);
        put(32, 4, 2);
        put(34, 16, 2);
        memcpy(&made.wav[36], "data", 4);
        put(40, 400, 4);
        for (int i = 0; i < 100; i++) {
            put(44 + i * 4, 1000, 2);
            put(46 + i * 4, static_cast<uint16_t>(static_cast<int16_t>(-3000)), 2);
        }
        sounds.push_back(made);
        a11y::SetMono(true);
        for (const fwsound::Sound& s : sounds) {
            if (s.wav.size() < 44) continue;
            const std::vector<uint8_t>& m = a11y::Mono(s.wav);
            uint16_t ch = 0, mch = 0;
            memcpy(&ch, &s.wav[22], 2);
            memcpy(&mch, &m[22], 2);
            // Every mono sample against the average of the original's two.
            int worst = 0;
            if (ch == 2 && mch == 1)
                for (size_t f = 0; 44 + f * 4 + 3 < s.wav.size() && 44 + f * 2 + 1 < m.size(); f++) {
                    int16_t l, r, v;
                    memcpy(&l, &s.wav[44 + f * 4], 2);
                    memcpy(&r, &s.wav[46 + f * 4], 2);
                    memcpy(&v, &m[44 + f * 2], 2);
                    worst = (std::max)(worst, abs((l + r) / 2 - v));
                }
            out += s.name + "\t" + std::to_string(ch) + "\t" + std::to_string(s.wav.size()) + "\t" + std::to_string(mch) + "\t" +
                   std::to_string(m.size()) + "\t" + std::to_string(worst) + "\n";
        }
        a11y::SetMono(acc.mono);
        save(name.substr(9), out);
        return true;
    }
    return false;
}

// ---- the console's rows on a PC -------------------------------------------
// What the rows of Ayuda, Brillo de la pantalla, Pantallas, Modo de espera and
// Consola do here, with the pages they lead to. Everything of this lives in
// this block: PcRow() gives a console row its PC meaning (AddConsoleRow asks
// it first), Scr::Page is a page of NSW-Home's own drawn with the rows of
// Settings in one column, Scr::Lock is the number pad of "Bloquear la
// consola", and PcFrame / PcInput are the two hooks (ui::Draw, the input
// path) behind dimming, automatic standby and the lock. Calls to the system
// go through pchost.h.

void SetupBegin(bool first); // the first-start assistant, below

bool pcTest = false;     // an offscreen run: DebugGoto was called
float pcTimeScale = 1;   // test states: time without input counts this many times over
double pcLastInput = 0;  // the last time anything was pressed or the mouse moved
double pcDrawnAt = -1;   // the last frame drawn
bool pcDimOn = false;    // "Protector de pantalla" has darkened the menu
bool pcDeepOn = false;   // automatic standby has darkened it much further
float pcDim = 0;         // how dark it is drawn now (the black's opacity)
bool pcLocked = false;   // the password is being asked for and must be given

// -- small things the rows share --

std::wstring PcPickFolder() {
    // Offscreen runs cannot open the folder dialog: they get a folder inside the data folder.
    if (!pcTest) return PickFolder();
    const std::wstring dir = DataDir() + L"\\transfer";
    pchost::MakeDir(dir);
    return dir;
}

// The monitor's facts, asked again every two seconds while something shows them.
double pcDisplayAt = -100;
const pchost::Display& PcDisplay() {
    static pchost::Display d;
    if (now - pcDisplayAt > 2 || now < pcDisplayAt) {
        d = pchost::DisplayNow();
        pcDisplayAt = now;
    }
    return d;
}

// The monitor answers a brightness question in tens of milliseconds, far too
// slowly to be asked every frame: it is asked when the row comes into view
// (nobody has asked for a second) and the answer is at least five seconds old.
struct PcBright {
    double asked = -100, read = -100;
    bool known = false, ok = false;
    int pct = 0;
} pcBright;
bool PcBrightness(int& pct) {
    if (!pcBright.known || (now - pcBright.asked > 1 && now - pcBright.read > 5)) {
        pcBright.ok = pchost::BrightnessGet(pcBright.pct);
        pcBright.known = true;
        pcBright.read = now;
    }
    pcBright.asked = now;
    pct = pcBright.pct;
    return pcBright.ok;
}

void PcOpenDisplay(bool hdr) {
    if (!pchost::OpenDisplaySettings(hdr) && !pchost::Offscreen()) Info(L"No se ha podido abrir la configuración de Windows.");
}

bool PcBatteryShown() {
    return Cos("system.battery_percent", "on") == "on";
}

void PcApplyOverscan() {
    const int pct = atoi(Cos("display.screen_size", "100").c_str());
    gfx::SetOverscan((std::max)(90, (std::min)(100, pct)) / 100.0f);
}

// -- a page of NSW-Home's own --

struct InfoPage {
    std::wstring title;
    std::function<std::vector<Row>()> rows;
    int row = -1;
    float scroll = 0, want = 0;
} pcPage;
Glide pageGlide;

// The rows of Settings, in one column in the middle of the screen.
ListLay PageLay() {
    ListLay L = LL();
    L.rowX = (W - L.rowW) / 2;
    return L;
}

void OpenPage(const std::wstring& title, std::function<std::vector<Row>()> rows) {
    pcPage.title = title;
    pcPage.rows = std::move(rows);
    pcPage.row = NextRow(pcPage.rows(), -1, 1);
    pcPage.scroll = pcPage.want = 0;
    pageGlide.t0 = -1;
    picker.open = false;
    Push(Scr::Page);
}

void PagePress(const Row& r) {
    if (r.act && !r.disabled && r.kind != RK::Slider) {
        PlaySfx(Sfx::Ok);
        r.act();
    } else {
        PlaySfx(Sfx::Bump);
    }
}

void DrawPage(float dt) {
    Theme T = Th();
    const bool s2 = S2();
    const ListLay L = PageLay();
    Header(5, pcPage.title);
    std::vector<Row> rows = pcPage.rows ? pcPage.rows() : std::vector<Row>();
    const int nr = static_cast<int>(rows.size());
    if (pcPage.row >= nr || (pcPage.row >= 0 && !CanFocus(rows[pcPage.row]))) pcPage.row = NextRow(rows, -1, 1);
    float total = 0;
    std::vector<RowBox> box = LayRows(rows, L, total);
    const float view = L.bottom - L.rowY0 - 14, most = (std::max)(0.0f, total - view);
    if (pcPage.row >= 0) {
        // The focused row is kept in view with its description; the first
        // and the last bring along what stands above and below them.
        const RowBox& b = box[pcPage.row];
        if (b.y - pcPage.want < 0) pcPage.want = b.y - (pcPage.row > 0 && rows[pcPage.row - 1].heading ? box[pcPage.row - 1].h : 0);
        if (b.y + b.h - pcPage.want > view) pcPage.want = b.y + b.h - view;
        if (NextRow(rows, pcPage.row, 1) < 0 && most <= b.y) pcPage.want = most;
        if (NextRow(rows, pcPage.row, -1) < 0 && b.y + b.h <= view) pcPage.want = 0;
    }
    if (MouseOwns(&pcPage.scroll)) pcPage.want = pcPage.scroll;
    pcPage.want = (std::max)(0.0f, (std::min)(most, pcPage.want));
    if (s2) {
        GlideTo(pageGlide, pcPage.scroll, pcPage.want);
    } else {
        Smooth(pcPage.scroll, pcPage.want, dt);
        if (pcPage.scroll != pcPage.want) Touch();
    }

    const float clipTop = s2 ? Px(128) : L.top + 1, clipH = s2 ? Px(968) - clipTop : L.bottom - L.top - 1;
    // Dragged, the focus moves onto the nearest row wholly in view; a page of
    // text with none simply stays where it was left (`want`).
    std::vector<std::pair<float, float>> fit(nr, {-1e9f, 0.0f});
    for (int i = 0; i < nr; i++)
        if (CanFocus(rows[i])) fit[i] = {box[i].y, box[i].h};
    const int pageDrag = AddScroll(L.rowX - 14, clipTop, L.rowW + 28, clipH, ScrollOf(pcPage.scroll, most, [fit, nr, view](float v) {
        pcPage.want = v;
        int to = NearestFit(pcPage.row, nr, [&](int i) { return fit[i].first - v >= -0.5f && fit[i].first + fit[i].second - v <= view + 0.5f; });
        if (to >= 0) pcPage.row = to;
    }));
    PushClip(L.rowX - 14, clipTop, L.rowW + 28, clipH);
    const bool cursor = !picker.open;
    const int onTop = s2 && cursor && pcPage.row >= 0 && pcPage.row < nr ? pcPage.row : -1; // its outline lies over its neighbours' rules
    bool any = false;
    for (int k = 0; k < nr + (onTop >= 0 ? 1 : 0); k++) {
        const int i = k < nr ? k : onTop;
        if (k < nr && i == onTop) continue;
        const Row& r = rows[i];
        const float ry = L.rowY0 + box[i].y - pcPage.scroll;
        any = any || CanFocus(r);
        if (ry > L.bottom + 4 || ry + box[i].h < L.top - 4) continue;
        DrawRow(r, box[i], ry, L, T.sep, cursor && i == pcPage.row, 0);
        if (!CanFocus(r)) continue;
        AddHit(L.rowX, ry, L.rowW, box[i].fh, [i, r] {
            pcPage.row = i;
            PagePress(r);
        });
    }
    PopClip();
    if (s2 && total > L.bottom - L.rowY0) {
        const float trackTop = Px(139), track = Px(957) - trackTop;
        const float len = (std::max)(30.0f, track * (L.bottom - L.rowY0) / total);
        const float by = Snap(trackTop + (std::min)(1.0f, pcPage.scroll / (std::max)(1.0f, most)) * (track - len));
        RRect(L.barX, by, 4, Snap(by + len) - by, 2, cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7));
        AddScrollBar(pageDrag, L.barX, trackTop, track, len, most);
    }
    std::vector<Hint> hints = {{L"B", L"Atrás", Btn::B}};
    if (any) hints.push_back({L"A", s2 ? L"Confirmar" : L"Aceptar", Btn::A});
    Footer(hints);
    if (picker.open) {
        hits.clear();
        DrawPicker();
    }
}

void PageButton(Btn b) {
    if (picker.open) {
        PickerButton(b);
        return;
    }
    std::vector<Row> rows = pcPage.rows ? pcPage.rows() : std::vector<Row>();
    const Row* cur = pcPage.row >= 0 && pcPage.row < static_cast<int>(rows.size()) ? &rows[pcPage.row] : nullptr;
    switch (b) {
    case Btn::Up:
    case Btn::Down: {
        const int d = b == Btn::Up ? -1 : 1, n = NextRow(rows, pcPage.row, d);
        float total = 0;
        const ListLay L = PageLay();
        LayRows(rows, L, total);
        const float most = (std::max)(0.0f, total - (L.bottom - L.rowY0 - 14));
        if (n >= 0) {
            pcPage.row = n;
            PlaySfx(Sfx::Move);
        } else if (!cur && d > 0 && pcPage.want < most - 1) {
            pcPage.want = (std::min)(most, pcPage.want + 150); // a page of text is read by moving it
            Touch();
        } else if (!cur && d < 0 && pcPage.want > 0) {
            pcPage.want = (std::max)(0.0f, pcPage.want - 150);
            Touch();
        } else {
            PlaySfx(Sfx::Bump);
            if (S2() && cur) BumpCursor(0, static_cast<float>(d));
        }
        break;
    }
    case Btn::Left:
    case Btn::Right:
        if (cur && cur->kind == RK::Slider && cur->step && !cur->disabled) cur->step(b == Btn::Left ? -1 : 1);
        break;
    case Btn::A:
        if (cur) PagePress(*cur);
        break;
    case Btn::B:
        PlaySfx(Sfx::Back);
        Pop();
        break;
    default: break;
    }
}

Row PcText(const std::wstring& text) {
    Row r;
    r.kind = RK::Text;
    r.label = text;
    return r;
}

// A striped table like the console's connection status: a title line, then
// one line per fact.
void PcTable(std::vector<Row>& out, const std::wstring& title, const std::wstring& titleValue,
             const std::vector<std::pair<std::wstring, std::wstring>>& lines) {
    Row t;
    t.kind = RK::Table;
    t.on = true;
    t.label = title;
    t.value = titleValue;
    out.push_back(t);
    int n = 0;
    for (const auto& l : lines) {
        Row r;
        r.kind = RK::Table;
        r.label = l.first;
        r.value = l.second;
        r.icon = n++ % 2 == 0 ? 0 : 1;
        out.push_back(r);
    }
}

// -- Ayuda --

std::vector<Row> PcHealthRows() {
    std::vector<Row> rows;
    rows.push_back(HeadRow(L"Qué es NSW-Home"));
    rows.push_back(PcText(L"NSW-Home es un menú para tu PC: muestra tus juegos y los inicia en el emulador que tengas "
                          L"instalado. Solo inicia tus propios juegos, a partir de las copias que tú hayas hecho de ellos."));
    rows.push_back(HeadRow(L"Al jugar"));
    rows.push_back(PcText(L"Haz una pausa de 10 a 15 minutos por cada hora de juego, aunque no te parezca necesario."));
    rows.push_back(PcText(L"Si notas cansancio o molestias en los ojos, las manos o los brazos, o si te mareas, deja de "
                          L"jugar y descansa antes de continuar."));
    rows.push_back(PcText(L"Juega en un lugar bien iluminado y a una distancia cómoda de la pantalla."));
    rows.push_back(PcText(L"Si usas auriculares, mantén el volumen a un nivel moderado."));
    rows.push_back(HeadRow(L"Menores"));
    rows.push_back(PcText(L"Conviene que un adulto acompañe a los niños mientras juegan y decida cuánto tiempo lo hacen."));
    return rows;
}

// Failed starts, newest first, kept in a file of their own in the data folder.
struct PcError {
    std::wstring when, game, emu;
    std::string code;
};
std::vector<PcError> pcErrors;
bool pcErrorsLoaded = false;
constexpr size_t kPcErrorsKept = 50;

std::wstring PcErrorsPath() {
    return DataDir() + L"\\errors.tsv";
}

void PcLoadErrors() {
    if (pcErrorsLoaded) return;
    pcErrorsLoaded = true;
    pcErrors.clear();
    std::string text;
    if (!pchost::ReadText(PcErrorsPath(), text)) return;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        for (size_t p = 0;;) {
            size_t tab = line.find('\t', p);
            f.push_back(line.substr(p, tab == std::string::npos ? std::string::npos : tab - p));
            if (tab == std::string::npos) break;
            p = tab + 1;
        }
        if (f.size() == 4) pcErrors.push_back({Widen(f[0]), Widen(f[1]), Widen(f[2]), f[3]});
    }
}

void PcSaveErrors() {
    std::string text;
    for (const PcError& e : pcErrors) text += Narrow(e.when) + "\t" + Narrow(e.game) + "\t" + Narrow(e.emu) + "\t" + e.code + "\n";
    pchost::WriteText(PcErrorsPath(), text);
}

std::wstring PcErrorText(const std::string& code) {
    if (code == "QUICK") return L"El programa se cerró nada más empezar.";
    if (code == "EMULATOR_MISSING") return L"No se encontró el emulador.";
    if (code == "NO_EMULATOR") return L"No había ningún emulador configurado.";
    if (code == "SPAWN") return L"No se pudo iniciar el emulador.";
    return L"No se pudo iniciar el programa (" + Widen(code) + L").";
}

// Called where a start fails (FinishLaunch) and where a game ends as soon as
// it has started (OnGameExit).
void PcLogError(const std::string& gameId, const std::string& code) {
    PcLoadErrors();
    auto flat = [](std::wstring s) {
        for (wchar_t& c : s)
            if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        return s;
    };
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t when[40];
    swprintf_s(when, L"%02d-%02d-%04d %02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    const Game* g = FindGame(gameId);
    auto own = cfg.perGame.find(gameId);
    PcError e;
    e.when = when;
    e.game = flat(g ? g->name : Widen(gameId));
    e.emu = flat(EmuLabel(own != cfg.perGame.end() ? own->second : cfg.defaultEmu));
    e.code = code;
    for (char& c : e.code)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    pcErrors.insert(pcErrors.begin(), e);
    if (pcErrors.size() > kPcErrorsKept) pcErrors.resize(kPcErrorsKept);
    PcSaveErrors();
}

std::vector<Row> PcErrorRows() {
    PcLoadErrors();
    std::vector<Row> rows;
    if (pcErrors.empty()) {
        Row none = PcText(L"No se ha registrado ningún error.");
        none.on = true; // alone on the page, centred
        rows.push_back(none);
        return rows;
    }
    rows.push_back(ActRow(L"Borrar el historial", [] {
        Ask(L"¿Quieres borrar el historial de errores?", {L"Cancelar", L"Borrar"}, [](int b) {
            if (b != 1) return;
            pcErrors.clear();
            pchost::Remove(PcErrorsPath());
        });
    }));
    rows.push_back(GapRow());
    for (const PcError& e : pcErrors) {
        Row r;
        r.label = lang::DayFirst(e.when, cfg.clock24) + L"   " + e.game; // the date in the language's order
        lang::Ignore(r.label);
        r.note = e.emu + L": " + PcErrorText(e.code);
        rows.push_back(r);
    }
    return rows;
}

// -- Pantallas --

std::vector<Row> PcVideoRows() {
    const pchost::Display& d = PcDisplay();
    std::vector<Row> rows;
    std::vector<std::pair<std::wstring, std::wstring>> lines;
    lines.push_back({L"Resolución", std::to_wstring(d.width) + L" x " + std::to_wstring(d.height)});
    lines.push_back({L"Frecuencia de actualización", std::to_wstring(d.hz) + L" Hz"});
    lines.push_back({L"HDR", d.hdr == 1 ? L"Activado" : d.hdr == 0 ? (d.hdrCapable ? L"Desactivado" : L"No compatible") : L"—"});
    if (d.bits > 0) lines.push_back({L"Profundidad de color", std::to_wstring(d.bits) + L" bits por canal"});
    lines.push_back({L"Escala de Windows", d.scale > 0 ? std::to_wstring(d.scale) + L" %" : std::wstring(L"—")});
    PcTable(rows, L"Señal de vídeo actual", L"", lines);
    rows.push_back(GapRow());
    rows.push_back(PcText(L"Datos del monitor en el que se muestra NSW-Home, leídos de Windows."));
    return rows;
}

std::vector<Row> PcSizeRows() {
    const int pct = (std::max)(90, (std::min)(100, atoi(Cos("display.screen_size", "100").c_str())));
    std::vector<Row> rows;
    auto set = [](int to) {
        SetCos("display.screen_size", std::to_string(to));
        PcApplyOverscan();
    };
    Row s;
    s.kind = RK::Slider;
    s.label = L"Tamaño de la imagen";
    s.value = std::to_wstring(pct) + L" %";
    s.pos = (pct - 90) / 10.0f;
    s.step = [pct, set](int d) {
        const int to = (std::max)(90, (std::min)(100, pct + d));
        if (to == pct) {
            PlaySfx(Sfx::Bump);
            return;
        }
        PlaySfx(Sfx::Move);
        set(to);
    };
    s.note = L"Si el borde del menú queda fuera de la pantalla, reduce el tamaño hasta que se vea entero.\n"
             L"◆ Solo cambia el tamaño del menú de NSW-Home, no el de los juegos.";
    rows.push_back(s);
    Row reset = ActRow(L"Restablecer", [set] { set(100); });
    reset.disabled = pct == 100;
    rows.push_back(reset);
    return rows;
}

// -- Modo de espera --

// NSW-Home's own choices: the capture only shows the value in use.
const wchar_t* const kPcSleepLabels[] = {L"1 min", L"3 min", L"5 min", L"10 min", L"15 min", L"30 min", L"Nunca"};
const int kPcSleepMinutes[] = {1, 3, 5, 10, 15, 30, 0};
const wchar_t* const kPcSilent = L"Sin sonido";

int PcSleepChoice() {
    const std::wstring v = Widen(Cos("sleep.auto_tv", "15 min"));
    for (int i = 0; i < 7; i++)
        if (v == kPcSleepLabels[i]) return i;
    return 4;
}

// -- Consola: the lock --
// A numeric password, kept as a salted hash in a file of its own so that
// forgetting it costs no more than deleting that file: it keeps a child out,
// not somebody who can open the data folder.

const wchar_t* const kLockFile = L"consolelock.txt";
constexpr int kLockRounds = 5000;

std::wstring LockPath() {
    return DataDir() + L"\\" + kLockFile;
}

int lockKnown = -1; // whether the file is there; -1 = look again
bool LockIsSet() {
    if (lockKnown < 0) lockKnown = pchost::Exists(LockPath()) ? 1 : 0;
    return lockKnown == 1;
}

std::string LockHash(const std::string& salt, const std::string& digits) {
    std::string h = salt + ":" + digits;
    for (int i = 0; i < kLockRounds; i++) h = pchost::Sha256Hex(h + salt);
    return h;
}

bool LockSave(const std::string& digits) {
    const std::string salt = pchost::RandomHex(16);
    lockKnown = -1;
    return pchost::WriteText(LockPath(), "1\n" + salt + "\n" + LockHash(salt, digits) + "\n");
}

bool LockMatches(const std::string& digits) {
    std::string text;
    if (!pchost::ReadText(LockPath(), text)) return true; // the file was deleted: no lock any more
    const size_t a = text.find('\n'), b = a == std::string::npos ? a : text.find('\n', a + 1);
    if (b == std::string::npos) return true; // not a lock file: the same
    const std::string salt = text.substr(a + 1, b - a - 1);
    std::string hash = text.substr(b + 1);
    while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r')) hash.pop_back();
    return LockHash(salt, digits) == hash;
}

enum class LockMode { Start, Wake, New, Repeat, Remove };
struct LockState {
    LockMode mode = LockMode::Start;
    std::string digits, first;
    int key = 0;           // the pad: 0-8 are 1-9, then delete, 0, accept
    int fails = 0;
    double waitUntil = -1; // after a wrong password nothing is accepted until then
    std::wstring msg;
} lockS;

bool LockCancellable() {
    return lockS.mode == LockMode::New || lockS.mode == LockMode::Repeat || lockS.mode == LockMode::Remove;
}

void LockBegin(LockMode mode) {
    lockS = LockState();
    lockS.mode = mode;
    pcLocked = mode == LockMode::Start || mode == LockMode::Wake;
}

void OpenLock(LockMode mode) {
    LockBegin(mode);
    Push(Scr::Lock);
}

void LockSubmit() {
    LockState& s = lockS;
    const std::string digits = s.digits;
    s.digits.clear();
    if (s.mode == LockMode::New) {
        if (digits.size() < 4) {
            PlaySfx(Sfx::Error);
            s.msg = L"La contraseña debe tener entre 4 y 8 cifras.";
            return;
        }
        PlaySfx(Sfx::Ok);
        s.first = digits;
        s.mode = LockMode::Repeat;
        s.msg.clear();
        return;
    }
    if (s.mode == LockMode::Repeat) {
        if (digits != s.first) {
            PlaySfx(Sfx::Error);
            s.mode = LockMode::New;
            s.first.clear();
            s.msg = L"Las contraseñas no coinciden. Vuelve a empezar.";
            return;
        }
        if (!LockSave(digits)) {
            PlaySfx(Sfx::Error);
            s.mode = LockMode::New;
            s.msg = L"No se ha podido guardar la contraseña.";
            return;
        }
        PlaySfx(Sfx::Ok);
        Pop();
        return;
    }
    if (!LockMatches(digits)) {
        PlaySfx(Sfx::Error);
        s.fails++;
        s.waitUntil = now + (std::min)(30, 3 * s.fails);
        s.msg.clear();
        Touch();
        return;
    }
    PlaySfx(Sfx::Ok);
    if (s.mode == LockMode::Remove) {
        pchost::Remove(LockPath());
        lockKnown = -1;
        Pop();
        return;
    }
    pcLocked = false;
    pcLastInput = now;
    if (stack.size() > 1) PopNow();
    else stack = {Scr::Home};
}

void LockKey(int k) {
    LockState& s = lockS;
    if (now < s.waitUntil) {
        PlaySfx(Sfx::Bump);
        return;
    }
    if (k == 11) {
        LockSubmit();
    } else if (k == 9) {
        if (s.digits.empty()) {
            PlaySfx(Sfx::Bump);
        } else {
            s.digits.pop_back();
            PlaySfx(Sfx::Back);
        }
    } else if (s.digits.size() >= 8) {
        PlaySfx(Sfx::Bump);
    } else {
        s.digits += k == 10 ? '0' : static_cast<char>('1' + k);
        s.msg.clear();
        PlaySfx(Sfx::Ok);
    }
    Touch();
}

void LockCancel() {
    PlaySfx(Sfx::Back);
    Pop();
}

void LockButton(Btn b) {
    LockState& s = lockS;
    const int col = s.key % 3, row = s.key / 3;
    int to = s.key;
    switch (b) {
    case Btn::Left: to = col > 0 ? s.key - 1 : -1; break;
    case Btn::Right: to = col < 2 ? s.key + 1 : -1; break;
    case Btn::Up: to = row > 0 ? s.key - 3 : -1; break;
    case Btn::Down: to = row < 3 ? s.key + 3 : -1; break;
    case Btn::A: LockKey(s.key); return;
    case Btn::Plus: LockKey(11); return;
    case Btn::B:
        if (s.digits.empty() && LockCancellable()) LockCancel();
        else LockKey(9);
        return;
    default: return;
    }
    if (to < 0) {
        PlaySfx(Sfx::Bump);
        if (S2()) BumpCursor(b == Btn::Left ? -1.0f : b == Btn::Right ? 1.0f : 0.0f, b == Btn::Up ? -1.0f : b == Btn::Down ? 1.0f : 0.0f);
    } else {
        s.key = to;
        PlaySfx(Sfx::Move);
    }
}

// The keyboard types the password directly.
void LockChar(wchar_t c) {
    if (c >= L'1' && c <= L'9') LockKey(static_cast<int>(c - L'1'));
    else if (c == L'0') LockKey(10);
    else if (c == L'\r') LockKey(11);
    else if (c == L'\b') LockKey(9);
    else if (c == 27 && LockCancellable()) LockCancel();
}

void DrawLock() {
    Theme T = Th();
    const bool s2 = S2();
    LockState& s = lockS;
    const bool asking = !LockCancellable();
    if (asking) {
        // A padlock where the other pages have their title.
        const float cx = W / 2, y = 62;
        Arc(cx, y, 11, 180, 360, T.text, 4);
        RRect(cx - 17, y, 34, 27, 5, T.text);
    } else {
        Header(5, L"Bloquear la consola");
    }
    const wchar_t* prompt = s.mode == LockMode::New      ? L"Elige una contraseña de 4 a 8 cifras."
                            : s.mode == LockMode::Repeat ? L"Introduce de nuevo la contraseña para confirmarla."
                            : s.mode == LockMode::Remove ? L"Introduce la contraseña para quitar el bloqueo."
                                                         : L"Introduce la contraseña.";
    Text(prompt, 0, 136, W, 40, 24, T.text, Center);
    // One mark per figure; the figures themselves are never shown.
    for (int i = 0; i < 8; i++) {
        const float cx = W / 2 + (i - 3.5f) * 30, cy = 208;
        if (i < static_cast<int>(s.digits.size())) Circle(cx, cy, 7.5f, T.text);
        else CircleStroke(cx, cy, 7, alpha(T.sub, 0.7f), 1.5f);
    }
    std::wstring msg = s.msg;
    if (now < s.waitUntil) {
        msg = L"La contraseña no es correcta. Espera " + std::to_wstring(static_cast<int>(ceil(s.waitUntil - now))) + L" s.";
        Touch();
    } else if (s.fails > 0 && msg.empty() && s.digits.empty()) {
        msg = L"La contraseña no es correcta. Inténtalo de nuevo.";
    }
    Text(msg, 0, 234, W, 34, 19, T.accent, Center);

    static const wchar_t* const labels[12] = {L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"Borrar", L"0", L"Aceptar"};
    const float kw = 150, kh = 62, gap = 10, x0 = (W - 3 * kw - 2 * gap) / 2, y0 = 284;
    const Color key = !s2 ? T.tile : cfg.dark ? rgb(0x3C3C3C) : rgb(0xECE8EC);
    const bool waiting = now < s.waitUntil;
    for (int i = 0; i < 12; i++) {
        const float x = x0 + (i % 3) * (kw + gap), y = y0 + (i / 3) * (kh + gap);
        RRect(x, y, kw, kh, 8, key);
        Text(labels[i], x, y, kw, kh, i == 9 || i == 11 ? 21.0f : 26.0f, waiting ? alpha(T.text, 0.4f) : T.text, Center);
        if (i == s.key) {
            if (s2) CursorFill(key);
            Cursor(x - 2, y - 2, kw + 4, kh + 4, 9);
        }
        AddHit(x, y, kw, kh, [i] {
            lockS.key = i;
            LockKey(i);
        });
    }
    if (!asking)
        Text(std::wstring(L"Si la olvidas, borra el archivo «") + kLockFile + L"» de la carpeta de datos de NSW-Home.", 0, 592, W, 30,
             17.4f, T.sub, Center);
    Footer({{L"+", L"Aceptar", Btn::Plus},
            {L"B", s.digits.empty() && !asking ? L"Atrás" : L"Borrar", Btn::B},
            {L"A", s2 ? L"Confirmar" : L"Aceptar", Btn::A}});
}

// -- Modo de espera: NSW-Home's standby screen --

int wakeCount = 0, wakeWith = -100; // "Bloquear la pantalla": the same button, three times
double wakeAt = -10;
bool pcRebuild = false; // Settings' sections are to be listed again

void PcWake() {
    wakeCount = 0;
    pcLastInput = now;
    if (Cos("sleep.wake_sound", "Normal") != Narrow(kPcSilent)) PlaySfx(Sfx::Home);
    if (LockIsSet()) {
        LockBegin(LockMode::Wake);
        stack.back() = Scr::Lock;
        Touch();
    } else {
        Pop();
    }
}

// A press while the standby screen is up. `code` tells buttons apart (HOME is -1).
void SleepPress(int code) {
    if (Cos("system.screen_lock", "off") == "on") {
        if (code == wakeWith && now - wakeAt < 3) wakeCount++;
        else wakeCount = 1;
        wakeWith = code;
        wakeAt = now;
        Touch();
        if (wakeCount < 3) return;
    }
    PcWake();
}

void DrawSleep() {
    Rect(0, 0, W, H, rgb(0x000000));
    if (wakeCount <= 0 || now - wakeAt >= 3) {
        wakeCount = 0;
        return;
    }
    // The screen is locked: say what opens it and how far along the presses are.
    Text(L"Pulsa el mismo botón tres veces.", 0, 320, W, 40, 24, rgb(0xD2CFD3), Center);
    for (int i = 0; i < 3; i++) {
        const float cx = W / 2 + (i - 1) * 34.0f;
        if (i < wakeCount) Circle(cx, 392, 8, rgb(0xD2CFD3));
        else CircleStroke(cx, 392, 7.5f, rgb(0x808080), 1.5f);
    }
    Touch();
}

void PcSleepNow() {
    dlg.open = false;
    entry.open = false;
    picker.open = false;
    wakeCount = 0;
    PushNow(Scr::Sleep);
}

// -- the two hooks --

// Anything pressed, typed or moved. True when it only brought the darkened
// menu back and must not do anything else.
bool PcInput() {
    pcLastInput = now;
    if (!pcDimOn && !pcDeepOn) return false;
    pcDimOn = pcDeepOn = false;
    Touch();
    return true;
}

// HOME: on the standby screen it is a press like any other; while the
// password is being asked for it does not lead to the menu.
bool PcHome() {
    if (PcInput()) return true;
    if (stack.back() == Scr::Sleep) {
        SleepPress(-1);
        return true;
    }
    return pcLocked && stack.back() == Scr::Lock;
}

bool PcWantsText() {
    return stack.back() == Scr::Lock && !dlg.open && !trans.on;
}

bool PcChar(wchar_t c) {
    if (PcInput()) return true;
    if (!PcWantsText() || entry.open) return false;
    LockChar(c);
    return true;
}

// How dark: the screen saver leaves the menu readable; automatic standby
// leaves only its shapes.
constexpr float kPcDimShade = 0.72f, kPcDeepShade = 0.93f;

// Once a frame, after everything else has been drawn: the timers behind
// automatic standby and the screen saver, and their darkening.
void PcFrame(float dt) {
    if (pcDrawnAt >= 0 && now - pcDrawnAt > 1) {
        // Nothing was drawn for a while (another program was in front): that
        // is not time spent idle in the menu, and what Windows told us is stale.
        pcLastInput += now - pcDrawnAt;
        pcDisplayAt = pcBright.asked = pcBright.read = -100;
    }
    pcDrawnAt = now;
    if (pcRebuild) {
        pcRebuild = false;
        BuildSettings();
    }
    // The password cannot be walked round (a game that ends resets the screens).
    if (pcLocked && stack.back() != Scr::Lock) stack.push_back(Scr::Lock);
    const Scr top = stack.back();
    if (top == Scr::Launch || top == Scr::Setup) pcLastInput = now; // a game is in front; the assistant is never interrupted
    const double idle = (now - pcLastInput) * pcTimeScale;
    const int minutes = kPcSleepMinutes[PcSleepChoice()];
    // Automatic standby never switches the menu off (a black screen with
    // nothing on it reads as a program that has stopped working: the owner
    // asked for this on 2026-11-26): the menu is darkened much further and
    // stays where it is, and the first press brings it back. The black standby
    // screen is only what the power button's "Modo de espera" shows. With a
    // password set, that press leads to the password.
    if (minutes > 0 && idle >= minutes * 60.0 && top != Scr::Sleep && top != Scr::Lock && !trans.on && !pcDeepOn) {
        pcDeepOn = true;
        if (LockIsSet()) {
            dlg.open = false;
            entry.open = false;
            picker.open = false;
            LockBegin(LockMode::Wake);
            stack.push_back(Scr::Lock);
        }
    }
    // The console's own note gives the time: five minutes.
    const bool dim = idle >= 300 && stack.back() != Scr::Sleep && Cos("display.burn_in", "on") == "on";
    if (dim) pcDimOn = true;
    else if (stack.back() == Scr::Sleep) pcDimOn = pcDeepOn = false;
    const float target = pcDeepOn ? kPcDeepShade : pcDimOn ? kPcDimShade : 0.0f;
    if (pcDim != target) {
        pcDim = target > pcDim ? (std::min)(target, pcDim + dt / 0.7f) : (std::max)(target, pcDim - dt / 0.2f);
        Touch();
    }
    if (pcDim > 0) Rect(0, 0, W, H, rgb(0x000000, pcDim));
}

// At start: the saved picture size, the library cache, the password.
void PcInit() {
    PcApplyOverscan();
    // "Actualización automática de programas": every dump is read again at
    // each start instead of being taken from what was read before.
    if (Cos("system.auto_update", "off") == "on") pchost::Remove(DataDir() + L"\\library.json");
    if (LockIsSet()) {
        LockBegin(LockMode::Start);
        stack.push_back(Scr::Lock);
    }
}

// An offscreen run (DebugGoto is only ever called by one): nothing real is
// changed or opened, and the password is only asked for by the states that
// are about it.
void PcTestBegin() {
    if (pcTest) return;
    pcTest = true;
    pchost::SetOffscreen(true);
    if (pcLocked) {
        pcLocked = false;
        stack = {Scr::Home};
    }
}

// -- Consola: pages --

std::vector<Row> PcImportantRows() {
    std::vector<Row> rows;
    rows.push_back(PcText(L"NSW-Home es un programa independiente que muestra un menú en tu PC e inicia tus juegos mediante "
                          L"un emulador. No es un producto de ningún fabricante de consolas ni está afiliado, patrocinado o "
                          L"aprobado por ninguno de ellos."));
    rows.push_back(PcText(L"NSW-Home no contiene programas, tipos de letra, sonidos ni ilustraciones de ningún fabricante de "
                          L"consolas. Tampoco incluye juegos, claves ni firmware."));
    rows.push_back(PcText(L"Los juegos, las claves y el firmware que utiliza son los que tú hayas extraído de tu propia "
                          L"consola y de tus propios juegos. NSW-Home solo los lee: no los modifica ni los envía a ningún sitio."));
    rows.push_back(PcText(L"NSW-Home no es un emulador: para iniciar un juego necesita que haya uno instalado en este equipo."));
    return rows;
}

std::vector<Row> PcLicenceRows() {
    std::vector<Row> rows;
    rows.push_back(HeadRow(L"Windows"));
    rows.push_back(PcText(L"La versión para Windows solo utiliza componentes del propio sistema. No incluye programas de terceros."));
    rows.push_back(HeadRow(L"Linux, macOS y Android"));
    PcTable(rows, L"Biblioteca", L"Licencia",
            {{L"SDL2", L"zlib"},
             {L"Cairo", L"LGPL 2.1 o MPL 1.1"},
             {L"pixman", L"MIT"},
             {L"FreeType", L"FreeType (FTL) o GPL 2"},
             {L"libpng", L"libpng (PNG Reference Library License 2)"},
             {L"libjpeg-turbo", L"IJG y BSD modificada"},
             {L"fontconfig", L"de tipo MIT"}});
    rows.push_back(GapRow());
    rows.push_back(PcText(L"Los nombres de productos y las marcas que aparecen en NSW-Home pertenecen a sus respectivos propietarios."));
    return rows;
}

std::vector<Row> PcSerialRows() {
    // (Looked up once a second at most: the emulator's version is read from its program file.)
    static std::vector<Row> rows;
    static double at = -100;
    if (now - at < 1 && now >= at && !rows.empty()) return rows;
    at = now;
    rows.clear();
    const std::wstring emuVersion = cfg.defaultEmu.empty() ? std::wstring() : EmuInfo(cfg.defaultEmu).version;
    const std::wstring adapter = pchost::AdapterName();
    std::vector<std::pair<std::wstring, std::wstring>> lines;
    lines.push_back({L"Emulador", cfg.defaultEmu.empty() ? std::wstring(L"—") : EmuLabel(cfg.defaultEmu)});
    lines.push_back({L"Versión del emulador", emuVersion.empty() ? std::wstring(L"—") : emuVersion});
    lines.push_back({L"Versión del firmware", FirmwareText()});
    if (!adapter.empty()) lines.push_back({L"Adaptador gráfico", adapter});
    PcTable(rows, L"Versión de NSW-Home", kNSWHomeVersion, lines);
    rows.push_back(GapRow());
    rows.push_back(PcText(L"NSW-Home no tiene número de serie. Aquí no se muestra nada que identifique este equipo ni a quien lo usa."));
    return rows;
}

// Pictures on screen keep their files open: they are let go of before the
// files are replaced or removed.
void PcDropPictures() {
    for (auto& kv : avatars) kv.second.reset();
    avatars.clear();
    for (Game& g : games) g.icon.reset();
}

void PcReload() {
    cfg = Config();
    LoadConfig();
    lockKnown = -1;
    pcErrorsLoaded = false;
    PcApplyOverscan();
    BuildSettings();
    RescanAsync();
}

const wchar_t* const kPcExportDir = L"NSW-Home-datos";
// What exports were called before the program's rename: still found by Importar.
const wchar_t* const kPcFormerExportDir = L"Dockside-datos"; // historical name

void PcExport() {
    const std::wstring dir = PcPickFolder();
    if (dir.empty()) return;
    const std::wstring dst = dir + L"\\" + kPcExportDir;
    const bool there = pchost::Exists(dst + L"\\config.json");
    Ask(there ? L"En esa carpeta ya hay datos de NSW-Home.\n¿Quieres sustituirlos?\n\n" + dst
              : L"Se guardarán la configuración, las imágenes y el historial de juego de NSW-Home.\n\n" + dst,
        {L"Cancelar", there ? L"Sustituir" : L"Exportar"}, [dst](int b) {
            if (b != 1) return;
            SaveConfig();
            const std::wstring src = DataDir();
            bool ok = pchost::MakeDir(dst) && pchost::Copy(src + L"\\config.json", dst + L"\\config.json") &&
                      pchost::CopyTree(src + L"\\avatars", dst + L"\\avatars");
            if (ok && pchost::Exists(src + L"\\pack")) ok = pchost::CopyTree(src + L"\\pack", dst + L"\\pack");
            Info(ok ? L"Los datos de NSW-Home se han exportado." : L"No se han podido guardar los datos en esa carpeta.");
        });
}

void PcImport() {
    const std::wstring dir = PcPickFolder();
    if (dir.empty()) return;
    std::wstring src = dir;
    for (const wchar_t* sub : {kPcExportDir, kPcFormerExportDir})
        if (src == dir && pchost::Exists(dir + L"\\" + sub + L"\\config.json")) src = dir + L"\\" + sub;
    if (!pchost::Exists(src + L"\\config.json")) {
        Info(L"En esa carpeta no hay datos de NSW-Home.");
        return;
    }
    if (!runningId.empty()) {
        Info(L"Cierra el programa abierto antes de importar los datos.");
        return;
    }
    Ask(L"La configuración, las imágenes y el historial de juego de este NSW-Home se sustituirán por los de la carpeta.\n\n" + src,
        {L"Cancelar", L"Importar"}, [src](int b) {
            if (b != 1) return;
            const std::wstring dst = DataDir();
            PcDropPictures();
            bool ok = pchost::Copy(src + L"\\config.json", dst + L"\\config.json");
            if (ok && pchost::Exists(src + L"\\avatars")) ok = pchost::CopyTree(src + L"\\avatars", dst + L"\\avatars");
            // The icon pack's pictures are on screen and cannot be replaced
            // while they are: only the ones missing here are brought over.
            if (ok && pchost::Exists(src + L"\\pack") && pchost::MakeDir(dst + L"\\pack"))
                for (const pchost::Entry& e : pchost::List(src + L"\\pack"))
                    if (!e.dir && !pchost::Exists(dst + L"\\pack\\" + e.name))
                        pchost::Copy(src + L"\\pack\\" + e.name, dst + L"\\pack\\" + e.name);
            PcReload();
            Info(ok ? L"Los datos de NSW-Home se han importado." : L"No se han podido leer los datos de esa carpeta.");
        });
    dlg.sel = 0; // what replaces or removes data starts on "Cancelar"
}

std::vector<Row> PcTransferRows() {
    std::vector<Row> rows;
    rows.push_back(PcText(L"Los datos de NSW-Home (su configuración, las imágenes de los usuarios y el historial de juego) "
                          L"se pueden guardar en una carpeta y recuperar después, en este equipo o en otro."));
    rows.push_back(PcText(L"No se incluyen los datos de los emuladores, las partidas guardadas, los juegos ni las claves."));
    rows.push_back(GapRow());
    rows.push_back(ActRow(L"Exportar a una carpeta", PcExport));
    rows.push_back(ActRow(L"Importar desde una carpeta", PcImport));
    return rows;
}

// Back to the first start: NSW-Home's own files go, nothing else is touched.
void PcFormat() {
    const std::wstring d = DataDir();
    PcDropPictures();
    games.clear();
    pchost::Remove(d + L"\\config.json");
    pchost::Remove(d + L"\\library.json");
    pchost::Remove(PcErrorsPath());
    pchost::Remove(LockPath());
    pchost::RemoveTree(d + L"\\icons");
    pchost::RemoveTree(d + L"\\avatars");
    pchost::MakeDir(d + L"\\icons");
    pchost::MakeDir(d + L"\\avatars");
    pcErrors.clear();
    pcLocked = false;
    PcReload();
    dlg.open = false;
    picker.open = false;
    homeRow = 1;
    gi = 0;
    SetupBegin(true);
    stack = {Scr::Home, Scr::Setup};
    Touch();
}

void PcAskFormat() {
    if (!runningId.empty()) {
        Info(L"Cierra el programa abierto antes de restaurar NSW-Home.");
        return;
    }
    Ask(L"Se borrarán la configuración de NSW-Home, su caché de la biblioteca, las imágenes de los usuarios, el historial "
        L"de juego y el de errores.\n\nNo se tocan los juegos ni los datos de los emuladores (partidas guardadas, claves, "
        L"firmware).",
        {L"Cancelar", L"Siguiente"}, [](int b) {
            if (b != 1) return;
            Ask(L"¿Restaurar NSW-Home?\nEsta acción no se puede deshacer.\n\nNSW-Home volverá a empezar con el asistente "
                L"inicial. Los juegos y los datos de los emuladores no se tocan.",
                {L"Cancelar", L"Restaurar"}, [](int c) {
                    if (c == 1) PcFormat();
                });
            dlg.sel = 0;
        });
    dlg.sel = 0;
}

std::vector<Row> PcFormatRows() {
    std::vector<Row> rows;
    rows.push_back(ActRow(L"Restaurar NSW-Home", PcAskFormat,
                          L"NSW-Home vuelve a su primer inicio: se borran su configuración, su caché de la biblioteca, las "
                          L"imágenes de los usuarios, el historial de juego y el de errores.\n"
                          L"◆ No se tocan los juegos ni los datos de los emuladores (partidas guardadas, claves, firmware).\n"
                          L"◆ Se conserva el paquete de iconos local."));
    return rows;
}

// -- the rows themselves --

// Gives the console row `id` its meaning on a PC. False: the row stays as
// AddConsoleRow made it (remembered but without effect, or console-only).
bool PcRow(Row& r, const std::string& id) {
    const std::wstring title = r.label;
    auto opens = [&](std::vector<Row> (*rows)()) {
        r.act = [title, rows] { OpenPage(title, rows); };
    };
    if (id == "help.health_safety") {
        opens(PcHealthRows);
    } else if (id == "help.error_history") {
        opens(PcErrorRows);
    } else if (id == "brightness.docked_notice") {
        int pct = 0;
        if (!PcBrightness(pct)) return false; // the console's sentence stays
        r = Row();
        r.kind = RK::Slider;
        r.label = L"Brillo de la pantalla";
        r.value = std::to_wstring(pct) + L" %";
        r.pos = pct / 100.0f;
        r.note = L"Brillo del monitor en el que se muestra NSW-Home.";
        r.step = [pct](int d) {
            const int to = (std::max)(0, (std::min)(100, pct + d * 5));
            if (to == pct) {
                PlaySfx(Sfx::Bump);
            } else if (pchost::BrightnessSet(to)) {
                pcBright.pct = to;
                PlaySfx(Sfx::Move);
            } else {
                pcBright.known = false; // the monitor stopped answering: ask again
                PlaySfx(Sfx::Error);
            }
        };
    } else if (id == "display.hdr_console" || id == "display.hdr_output" || id == "display.hdr_settings") {
        const pchost::Display& d = PcDisplay();
        if (!d.ok || d.hdr < 0) return false;
        r.act = [] { PcOpenDisplay(true); };
        if (id == "display.hdr_console") {
            r.on = d.hdr == 1;
            r.note = L"Muestra si el HDR está activado en Windows para este monitor. NSW-Home no lo cambia: al pulsar A "
                     L"se abre la configuración de pantalla de Windows.";
        } else if (id == "display.hdr_output") {
            r.value = d.hdr == 1 ? L"Activada" : d.hdrCapable ? L"Desactivada" : L"No compatible";
            r.note = L"Estado actual del HDR de este monitor. Para cambiarlo se abre la configuración de pantalla de Windows.";
        } else {
            r.note = L"Abre los ajustes de HDR de Windows.";
        }
    } else if (id == "display.120hz") {
        const pchost::Display& d = PcDisplay();
        if (!d.ok || d.hz <= 0) return false;
        r.kind = RK::Plain;
        r.on = false;
        r.act = nullptr;
        r.value = std::wstring(d.hz >= 120 ? L"Sí" : L"No") + L" (" + std::to_wstring(d.hz) + L" Hz)";
        r.note = L"Indica si el monitor en el que se muestra NSW-Home funciona a 120 Hz o más. NSW-Home no cambia el "
                 L"modo de pantalla.";
    } else if (id == "display.video_output_info") {
        if (!PcDisplay().ok) return false;
        opens(PcVideoRows);
        r.note = L"Permite consultar la resolución, la frecuencia, el HDR y la escala del monitor en el que se muestra NSW-Home.";
    } else if (id == "display.screen_size") {
        opens(PcSizeRows);
    } else if (id == "display.burn_in" || id == "system.battery_percent") {
        // The switch AddConsoleRow made is the whole of it: PcFrame and DrawStatus read its state.
    } else if (id == "sleep.auto_tv") {
        const int cur = PcSleepChoice();
        r.value = kPcSleepLabels[cur];
        r.act = [title, cur] {
            Pick(title, std::vector<std::wstring>(std::begin(kPcSleepLabels), std::end(kPcSleepLabels)), cur, [](int i) {
                if (i >= 0 && i < 7) SetCos("sleep.auto_tv", Narrow(kPcSleepLabels[i]));
            });
        };
        AddNote(r, L"◆ Tras ese tiempo sin usar el menú, NSW-Home oscurece mucho la pantalla, sin apagarla; la primera "
                   L"pulsación la devuelve. No se aplica mientras hay un juego en primer plano.");
    } else if (id == "sleep.wake_sound") {
        const bool silent = Cos(id, "Normal") == Narrow(kPcSilent);
        r.value = silent ? kPcSilent : L"Normal";
        r.act = [title, silent] {
            Pick(title, {L"Normal", kPcSilent}, silent ? 1 : 0,
                 [](int i) { SetCos("sleep.wake_sound", i == 1 ? Narrow(kPcSilent) : "Normal"); });
        };
    } else if (id == "system.nickname") {
        r.value = Widen(Cos(id, "NSW-Home"));
        r.act = [title] {
            entry.open = true;
            entry.title = title;
            entry.text = Widen(Cos("system.nickname", "NSW-Home"));
            entry.done = [](const std::wstring& s) {
                if (!s.empty()) SetCos("system.nickname", Narrow(s));
            };
        };
    } else if (id == "system.screen_lock") {
        // (Off unless chosen: on the console, too, it does nothing while docked.)
        r.on = Cos(id, "off") == "on";
        const bool on = r.on;
        r.act = [on] { SetCos("system.screen_lock", on ? "off" : "on"); };
        r.note = L"Al salir del modo de espera, pulsa el mismo botón tres veces para continuar.";
    } else if (id == "system.console_lock") {
        r.on = LockIsSet();
        const bool on = r.on;
        r.act = [on] { OpenLock(on ? LockMode::Remove : LockMode::New); };
        AddNote(r, L"◆ La contraseña es numérica, de 4 a 8 cifras, y también se pide al iniciar NSW-Home.");
        AddNote(r, (std::wstring(L"◆ Si la olvidas, borra el archivo «") + kLockFile + L"» de la carpeta de datos de NSW-Home.").c_str());
    } else if (id == "system.serials") {
        opens(PcSerialRows);
    } else if (id == "system.important_info") {
        opens(PcImportantRows);
    } else if (id == "system.ip_notices") {
        opens(PcLicenceRows);
    } else if (id == "system.auto_update") {
        r.note = L"Al activar esta opción, NSW-Home vuelve a leer todos los juegos de la biblioteca cada vez que se inicia, "
                 L"en lugar de usar lo que ya había leído. El inicio tarda más.";
    } else if (id == "system.transfer") {
        opens(PcTransferRows);
        r.note = L"Exporta los datos de NSW-Home a una carpeta o los importa desde ella.";
    } else if (id == "system.format") {
        opens(PcFormatRows);
    } else {
        return false;
    }
    r.real = true;
    return true;
}

// Test states (--shot / --shotseq):
//   quick-<state>   any state, with idle time counted 600 times over (a frame is ten seconds)
//   pcrow:<id>      Settings with the console row of that id focused
//   pcact:<id>      the same, and the row pressed (its page opens)
//   pc-sleep        the standby screen
//   pc-lock-start   the password asked for as at start
//   pcpin:<digits>  sets that password; "pcpin:" removes it
//   pcerr           records two failed starts of the first game in the error history
// pcpin and pcerr can stand before another state with '~': "pcpin:1234~pc-lock-start".
bool DebugPc(const std::wstring& name) {
    const size_t tilde = name.find(L'~');
    if (tilde != std::wstring::npos && name.rfind(L"pc", 0) == 0) {
        DebugPc(name.substr(0, tilde));
        DebugGoto(name.substr(tilde + 1));
        return true;
    }
    auto ends = [&](const wchar_t* s) {
        const size_t n = wcslen(s);
        return name.size() > n && name.compare(name.size() - n, n, s) == 0;
    };
    if (name.rfind(L"quick-", 0) == 0) {
        pcTimeScale = 600;
        DebugGoto(name.substr(6));
        return true;
    }
    if (name.rfind(L"pcpin:", 0) == 0) {
        const std::string digits = Narrow(name.substr(6));
        if (digits.empty()) pchost::Remove(LockPath());
        else LockSave(digits);
        lockKnown = -1;
        return true;
    }
    if (name == L"pcerr") {
        if (!games.empty()) {
            PcLogError(games[0].id, "EMULATOR_MISSING");
            PcLogError(games[0].id, "QUICK");
        }
        return true;
    }
    if (ends(L"-2") || ends(L"-2l")) return false; // DebugGoto takes the style off the name first
    if (name == L"pc-sleep") {
        PushNow(Scr::Sleep);
        return true;
    }
    if (name == L"pc-lock-start") {
        if (LockIsSet()) {
            LockBegin(LockMode::Start);
            stack.push_back(Scr::Lock);
        }
        return true;
    }
    const bool act = name.rfind(L"pcact:", 0) == 0;
    if (!act && name.rfind(L"pcrow:", 0) != 0) return false;
    const std::string id = Narrow(name.substr(6));
    BuildSettings();
    for (const console::Section& sec : console::Settings().sections)
        for (const console::Row& cr : sec.rows) {
            if (id != cr.id) continue;
            const int cat = CatById(settings, sec.id);
            if (cat < 0) return true;
            OpenSettings(cat);
            settings.inRows = true;
            const std::vector<Row> rows = settings.cats[cat].rows();
            // (The brightness row replaces the console's sentence, so it is found by position.)
            int at = id == "brightness.docked_notice" ? NextRow(rows, -1, 1) : -1;
            for (size_t i = 0; i < rows.size() && at < 0; i++)
                if (!rows[i].heading && rows[i].label == Spell(cr.label)) at = static_cast<int>(i);
            if (at < 0) return true;
            settings.row = at;
            if (act && rows[at].act && !rows[at].disabled) rows[at].act();
            return true;
        }
    return true;
}

// ---- controllers and remapping: what their Settings rows do on a PC ----------
//
// One block: the rows of "Mandos y accesorios" and the two remapping rows of
// "Accesibilidad". The console's own pages behind these rows are in no
// capture, so the three pages here are NSW-Home's, in the menu's manner: the
// input test, the sticks, the menu's buttons. Test states: DebugPadRows.
//
// Where things are kept:
//   cfg.console["pads.remap"]                 the menu's remapping ("0,1,2..."; absent: stock)
//   cfg.console["controllers.pro_wired"]      on / off, the console's row of that id
//   cfg.console["accessibility.remap_quick"]  on / off, likewise
//   cfg.console["pads.wish.*"]                emulator values waiting to be written (padwish.h)

const wchar_t* PadName(pads::Kind k); // these five: with the Controllers page, below
void PadLights(float cx, float top, int player);
void PadShape(pads::Kind kind, float cx, float cy, Color body, Color edge);
void KeyboardPicture(float cx, float cy, Color body, Color edge);
std::vector<pads::Pad> PadsNow();

// The configuration's say over the pads themselves, handed over at start.
void PadsConfigApply() {
    int map[pads::kActions];
    if (pads::RemapParse(Cos("pads.remap", ""), map)) pads::SetRemap(map);
    // (On by default, unlike the console's switch: there a pad on a cable still works over Bluetooth;
    // on a PC a Pro Controller that is only on a cable would do nothing at all until this is found.)
    pads::SetProWired(Cos("controllers.pro_wired", "on") == "on");
}

// -- the emulator's values behind four of the rows --

// Read at most once a second: the rows are made anew every frame.
struct PadVals {
    double at = -10;
    bool file = false; // the emulator's settings were found and understood
    padcfg::Family family = padcfg::Family::Ryujinx;
    std::wstring path, emu, layout;
    padcfg::Rumble rumble;
    int keyboard = -1, mouse = -1, speed = -1;
} padVals;

std::string PadWish(const char* key) {
    return Cos(std::string("pads.wish.") + key, "");
}

const PadVals& PadValsNow() {
    PadVals& v = padVals;
    if (now >= v.at && now - v.at < 1.0) return v;
    v = PadVals();
    v.at = now;
    v.emu = PadEmuName();
    padsys::KeyboardLayout(v.layout);
    if (emuDebugMissing || OnAndroid()) return v;
    if (runningId.empty()) {
        // What was chosen while a game was open is written now that it has
        // closed. A wish that still stands after that could not be written:
        // it is dropped, so that the row goes back to showing the file.
        PadWishesApplyMain();
        bool failed = false;
        for (auto it = cfg.console.begin(); it != cfg.console.end();) {
            const bool wish = it->first.compare(0, 10, "pads.wish.") == 0;
            failed = failed || wish;
            it = wish ? cfg.console.erase(it) : std::next(it);
        }
        if (failed) {
            SaveConfig();
            Info(L"No se ha podido guardar el cambio.");
        }
    }
    v.path = EmuConfigPath();
    v.family = EmuConfigKind().empty() ? padcfg::Family::Ryujinx : padcfg::Family::Yuzu;
    v.rumble = padcfg::RumbleGet(v.path, v.family);
    v.keyboard = padcfg::SwitchGet(v.path, v.family, padcfg::Switch::Keyboard);
    v.mouse = padcfg::SwitchGet(v.path, v.family, padcfg::Switch::Mouse);
    v.speed = padcfg::MouseSpeedGet(v.path, v.family);
    v.file = v.rumble.ok || v.keyboard >= 0;
    return v;
}

void PadWishSet(const char* key, const std::string& value) {
    cfg.console[std::string("pads.wish.") + key] = value;
    SaveConfig();
    padVals.at = -10; // read again: with no game open that also writes it
}

const wchar_t* const kPadWaits = L"◆ El cambio se aplicará cuando cierres el programa abierto.";

// -- shared drawing --

// The menu's buttons and actions by name (pads.h: mUp ... mRS, in that order).
const wchar_t* const kButtonName[pads::kMenuButtons] = {L"↑", L"↓", L"←", L"→", L"A", L"B", L"X", L"Y", L"+",
                                                         L"−", L"L", L"R", L"HOME", L"Captura", L"ZL", L"ZR", L"Palanca L", L"Palanca R"};
const wchar_t* const kActionName[pads::kActions] = {L"Arriba",          L"Abajo",          L"Izquierda",           L"Derecha",
                                                    L"Confirmar",       L"Atrás",          L"Función de X",        L"Función de Y",
                                                    L"Opciones (+)",    L"Función de −",   L"Página anterior (L)", L"Página siguiente (R)",
                                                    L"Menú HOME",       L"Captura de pantalla"};

// A button of a pad as a lozenge with its name: filled while it is held.
void PadChip(const wchar_t* label, float x, float y, float w, float h, bool on, bool marked = false) {
    Theme T = Th();
    if (on) RRect(x, y, w, h, h / 2, T.accent);
    else RRectStroke(x + 0.5f, y + 0.5f, w - 1, h - 1, h / 2 - 0.5f, marked ? T.accent : T.sub, 1.2f);
    Text(label, x, y, w, h, h * 0.52f, on ? T.bg : marked ? T.accent : T.text, Center, on ? 700 : 400);
}

// A trigger: its name in a box that fills as far as it is pressed.
void PadTrigger(const wchar_t* label, float x, float y, float w, float h, float amount) {
    Theme T = Th();
    amount = (std::max)(0.0f, (std::min)(1.0f, amount));
    if (amount > 0.01f) {
        PushClip(x, y, w * amount, h);
        RRect(x, y, w, h, 5, T.accent);
        PopClip();
    }
    RRectStroke(x + 0.5f, y + 0.5f, w - 1, h - 1, 5, amount > 0.5f ? T.accent : T.sub, 1.2f);
    Text(label, x, y, w, h, h * 0.52f, amount > 0.5f ? T.bg : T.text, Center, amount > 0.5f ? 700 : 400);
}

// A stick seen from above: its travel, the dead zone when one is known
// (`zone` >= 0), where it is now, and a thicker rim while it is pressed in.
void PadStick(float cx, float cy, float r, float x, float y, bool pressed, float zone = -1) {
    Theme T = Th();
    Circle(cx, cy, r, T.focusBg.a > 0 ? T.focusBg : T.tile);
    if (zone > 0) Circle(cx, cy, r * (std::min)(1.0f, zone), alpha(T.sub, 0.38f));
    Line(cx - r, cy, cx + r, cy, alpha(T.sub, 0.45f), 1);
    Line(cx, cy - r, cx, cy + r, alpha(T.sub, 0.45f), 1);
    CircleStroke(cx, cy, r, pressed ? T.accent : T.sub, pressed ? 3.4f : 1.4f);
    // A corner of the square of raw values lies outside the circle: kept on the rim.
    const float len = sqrtf(x * x + y * y), k = len > 1 ? 1 / len : 1.0f;
    Circle(cx + x * k * r, cy - y * k * r, (std::max)(4.0f, r * 0.13f), T.accent);
}

// -- "Probar los dispositivos de entrada" --
//
// Every connected pad on a line of its own with all it reports, live, and the
// keyboard under them. While the page is up the pads do not drive the menu
// (pads::Hush), every button being under test; it is left as the console
// leaves its own test, by holding any button for a second, or with Esc.

struct PadTestUi {
    double holdAt = -1;   // since when a pad button has been held
    bool armed = false;   // every button has been let go once since the page opened
    bool leaving = false;
    bool debugHold = false;              // test states: a made-up pad keeps a button down
    std::vector<std::wstring> debugKeys; // test states: keys shown as held
} padTest;

void OpenPadTest() {
    padTest = PadTestUi();
    Push(Scr::PadTest);
}

void PadTestLeave() {
    if (padTest.leaving) return;
    padTest.leaving = true;
    PlaySfx(Sfx::Back);
    Pop();
}

void DrawPadTest() {
    Theme T = Th();
    pads::Hush();
    Touch();
    Header(4, L"Probar los dispositivos de entrada");
    PadsNow();
    const std::vector<pads::Live> all = pads::Snapshot(cfg.swapAB);
    const float top = 96, rowH = 98, y0 = top + 34;
    Text(L"Pulsa los botones, aprieta los gatillos y mueve las palancas: lo que llega de cada mando se ilumina.", 64, top, 1152, 30, 17.5f,
         T.sub, Center);

    const int n = (std::min)(4, static_cast<int>(all.size()));
    bool any = false;
    for (int i = 0; i < n; i++) {
        const pads::Live& l = all[i];
        const pads::State& s = l.st;
        const float y = y0 + i * rowH, cy = y + rowH / 2;
        any = any || s.buttons != 0;
        HRule(64, 1216, Snap(y + rowH), T.line);
        PadLights(159, y + 12, l.pad.player);
        Text(PadName(l.pad.kind), 64, y + 28, 190, 30, 20.0f, T.text, Center);
        Text(l.pad.link == pads::Link::Bluetooth ? L"Bluetooth" : l.pad.link == pads::Link::XInput ? L"XInput" : L"USB", 64, y + 56, 190, 24,
             15.5f, T.sub, Center);
        PadStick(306, cy, 34, s.lx, s.ly, (s.buttons & pads::kLS) != 0);
        if (s.right) PadStick(396, cy, 34, s.rx, s.ry, (s.buttons & pads::kRS) != 0);
        PadTrigger(L"ZL", 456, cy - 36, 92, 30, s.analog ? s.lt : (s.buttons & pads::kZL) ? 1.0f : 0.0f);
        PadTrigger(L"ZR", 456, cy + 6, 92, 30, s.analog ? s.rt : (s.buttons & pads::kZR) ? 1.0f : 0.0f);
        // Two lines of seven: directions and the menu buttons, letters and shoulders.
        static const int line1[7] = {0, 1, 2, 3, 8, 9, 12}, line2[7] = {4, 5, 6, 7, 10, 11, 13};
        for (int k = 0; k < 7; k++) {
            PadChip(kButtonName[line1[k]], 580 + k * 91.0f, cy - 36, 82, 30, (l.menu & (1u << line1[k])) != 0);
            PadChip(kButtonName[line2[k]], 580 + k * 91.0f, cy + 6, 82, 30, (l.menu & (1u << line2[k])) != 0);
        }
    }
    if (!n) {
        Text(L"No hay ningún mando conectado.", 64, y0 + 150, 1152, 40, 21.6f, T.sub, Center);
        Text(L"Conecta un mando para probarlo. El teclado se prueba aquí abajo.", 64, y0 + 190, 1152, 30, 17.5f, T.sub, Center);
    }

    // The keyboard: the keys held down, by the names on their caps.
    const float ky = y0 + 4 * rowH + 4;
    PushScale(0.62f, 159, ky + 36);
    KeyboardPicture(169, ky + 36, T.focusBg.a > 0 ? T.focusBg : T.tile, T.sub);
    PopTransform();
    Text(L"Teclado", 64, ky + 64, 190, 24, 15.5f, T.sub, Center);
    std::vector<std::wstring> keys;
    const bool can = padsys::KeysHeld(keys) || !padTest.debugKeys.empty();
    if (!padTest.debugKeys.empty()) keys = padTest.debugKeys;
    if (!can) {
        Text(L"En este sistema no se pueden mostrar las teclas.", 272, ky + 20, 900, 34, 18.5f, T.sub);
    } else if (keys.empty()) {
        Text(L"Pulsa cualquier tecla para verla aquí.", 272, ky + 20, 900, 34, 18.5f, T.sub);
    } else {
        float x = 272;
        for (const auto& k : keys) {
            const float w = (std::max)(64.0f, TextWidth(k, 16.6f) + 30);
            if (x + w > 1216) break;
            PadChip(k.c_str(), x, ky + 21, w, 32, true);
            x += w + 10;
        }
    }

    // Leaving: a pad button kept down for a second (not the one still held
    // from opening the page).
    if (pads::DebugFaking()) any = padTest.debugHold;
    if (!any) padTest.armed = true, padTest.holdAt = -1;
    else if (padTest.armed && padTest.holdAt < 0) padTest.holdAt = now;
    const float held = padTest.holdAt < 0 ? 0.0f : (std::min)(1.0f, static_cast<float>(now - padTest.holdAt));
    Footer({{L"B", L"Atrás", Btn::B}});
    const wchar_t* how = L"Para terminar, mantén pulsado un botón del mando o pulsa Esc.";
    Text(how, 140, 660, 760, 40, 18.6f, T.text);
    if (held > 0) Rect(140, 699, TextWidth(how, 18.6f) * held, 3, T.accent);
    if (held >= 1) PadTestLeave();
}

void PadTestButton(Btn b) {
    // Only the keyboard (and the mouse, on the footer) gets here: the pads are hushed.
    if (b == Btn::B) PadTestLeave();
    else if (b == Btn::Y && pads::DebugFaking()) padTest.debugHold = true; // test states: a pad button is held from now on
}

// -- "Calibrar las palancas" --
//
// No calibration can be written to a pad from here. What there is: the sticks
// of the pad that last pressed a button, live; how far off centre they rest
// and how far they reach, measured over a few seconds; and the dead zone the
// emulator's own entry for that pad uses, which can be changed (padcfg.h).

struct PadCalUi {
    std::string id;    // the pad on show
    int sel = 0;       // 0 measure, 1 left dead zone, 2 right dead zone
    int phase = 0;     // 0 nothing, 1 the sticks are let go, 2 they are turned
    double phaseAt = 0;
    int samples = 0;
    float sum[4] = {};
    float rest[2] = {};     // per stick: how far from the centre it rests
    float reach[2] = {};    // per stick: the shortest of its four ways out
    float ext[2][4] = {};
    bool measured = false, first = true, single = false, haveWant = false;
    padcfg::Want want;
    padcfg::Zones zones;
    double zonesAt = -10;
    std::map<std::string, uint32_t> held; // each pad's buttons at the last frame
} padCal;

constexpr float kCalRest = 1.5f, kCalTurn = 4.0f; // seconds of each part of the measurement

void OpenPadCal() {
    padCal = PadCalUi();
    Push(Scr::PadCal);
}

padcfg::Want PadWantOf(pads::Pad p) {
    p.player = (std::max)(1, p.player); // (a pad beyond the fourth has no number, but may have an entry)
    const std::vector<padcfg::Want> w = padcfg::WantsFrom({p});
    return w.empty() ? padcfg::Want() : w[0];
}

float PadCalSuggested(int stick) {
    return (std::min)(0.5f, ceilf((padCal.rest[stick] * 1.5f + 0.03f) * 100.0f) / 100.0f);
}

void PadCalSetZone(bool right, double value) {
    PadCalUi& c = padCal;
    const double was = right ? c.zones.right : c.zones.left;
    if (!c.haveWant || !c.zones.found || was < 0) {
        PlaySfx(Sfx::Bump);
        return;
    }
    if (!runningId.empty()) {
        Info(L"Cierra el programa abierto antes de cambiar la zona muerta.");
        return;
    }
    value = (std::max)(0.0, (std::min)(0.5, floor(value * 100.0 + 0.5) / 100.0));
    if (fabs(value - was) < 0.001) {
        PlaySfx(Sfx::Bump);
        return;
    }
    const PadVals& v = PadValsNow();
    if (!padcfg::ZoneSet(v.path, v.family, c.want, right, value)) {
        Info(L"No se ha podido guardar el cambio.");
        return;
    }
    PlaySfx(Sfx::Move);
    c.zonesAt = -10;
}

void DrawPadCal() {
    Theme T = Th();
    PadCalUi& c = padCal;
    Header(4, L"Calibrar las palancas");
    PadsNow();
    if (c.phase) pads::Hush(); // turning a stick must not walk the menu
    Touch();

    // Test states: a made-up pad moves by itself while it is being measured.
    if (c.phase && pads::DebugFaking()) {
        pads::State s;
        const float t = static_cast<float>(now - c.phaseAt);
        if (c.phase == 1) s.lx = 0.045f, s.ly = -0.02f, s.rx = -0.03f, s.ry = 0.06f;
        else s.lx = 0.045f + 0.93f * cosf(t * 7), s.ly = -0.02f + 0.93f * sinf(t * 7), s.rx = -0.03f + 0.97f * cosf(t * 9), s.ry = 0.06f + 0.97f * sinf(t * 9);
        s.right = s.analog = true;
        pads::DebugHold(0, s);
    }

    const std::vector<pads::Live> all = pads::Snapshot(cfg.swapAB);
    const pads::Live* cur = nullptr;
    for (const auto& l : all) {
        // The pad that pressed a button is the one on show (at first: the one
        // still holding the button that opened the page).
        const uint32_t before = c.first ? 0 : c.held[l.pad.id];
        if ((l.st.buttons & ~before) && !c.phase && l.pad.id != c.id) {
            if (!c.id.empty()) c.measured = false; // another pad's numbers
            c.id = l.pad.id;
            c.zonesAt = -10;
        }
        c.held[l.pad.id] = l.st.buttons;
    }
    c.first = false;
    if (c.id.empty() && all.size() == 1) c.id = all[0].pad.id;
    for (const auto& l : all)
        if (l.pad.id == c.id) cur = &l;
    if (!cur && !c.id.empty()) {
        c.id.clear(); // it was disconnected
        c.phase = 0;
        c.measured = false;
    }

    const PadVals& v = PadValsNow();
    if (cur && now - c.zonesAt > 1.0) {
        c.zonesAt = now;
        c.want = PadWantOf(cur->pad);
        c.haveWant = c.want.what != padcfg::Want::What::Unknown;
        c.zones = c.haveWant && v.file ? padcfg::ZonesGet(v.path, v.family, c.want) : padcfg::Zones();
        c.single = !cur->st.right;
    }
    if (!cur) c.zones = padcfg::Zones(), c.haveWant = false;

    // The measurement.
    if (cur && c.phase) {
        const pads::State& s = cur->st;
        const float t = static_cast<float>(now - c.phaseAt);
        const float at[4] = {s.lx, s.ly, s.rx, s.ry};
        if (c.phase == 1) {
            for (int k = 0; k < 4; k++) c.sum[k] += at[k];
            c.samples++;
            if (t >= kCalRest) {
                for (int k = 0; k < 2; k++) {
                    const float x = c.sum[k * 2] / c.samples, y = c.sum[k * 2 + 1] / c.samples;
                    c.rest[k] = sqrtf(x * x + y * y);
                    for (int d = 0; d < 4; d++) c.ext[k][d] = 0;
                }
                c.phase = 2;
                c.phaseAt = now;
            }
        } else {
            for (int k = 0; k < 2; k++) {
                const float x = at[k * 2], y = at[k * 2 + 1];
                c.ext[k][0] = (std::max)(c.ext[k][0], x), c.ext[k][1] = (std::max)(c.ext[k][1], -x);
                c.ext[k][2] = (std::max)(c.ext[k][2], y), c.ext[k][3] = (std::max)(c.ext[k][3], -y);
            }
            if (t >= kCalTurn) {
                for (int k = 0; k < 2; k++) c.reach[k] = (std::min)((std::min)(c.ext[k][0], c.ext[k][1]), (std::min)(c.ext[k][2], c.ext[k][3]));
                c.phase = 0;
                c.measured = true;
                PlaySfx(Sfx::Ok);
            }
        }
    }

    // Left: the sticks.
    const float mid = 400;
    if (all.empty()) {
        Text(L"No hay ningún mando conectado.", 64, 300, 672, 40, 21.6f, T.sub, Center);
        Text(L"Conecta un mando para ver sus palancas.", 64, 340, 672, 30, 17.5f, T.sub, Center);
    } else if (!cur) {
        Text(L"Pulsa cualquier botón en el mando", 64, 300, 672, 40, 21.6f, T.text, Center);
        Text(L"cuyas palancas quieras comprobar.", 64, 336, 672, 40, 21.6f, T.text, Center);
    } else {
        const pads::State& s = cur->st;
        std::wstring who = PadName(cur->pad.kind);
        if (cur->pad.player > 0) who += L" · jugador " + std::to_wstring(cur->pad.player);
        Text(who, 64, 100, 672, 34, 20.5f, T.text, Center);
        const int sticks = s.right ? 2 : 1;
        for (int k = 0; k < sticks; k++) {
            const float cx = sticks == 1 ? mid : (k ? mid + 160 : mid - 160), cy = 300, r = 118;
            const float x = k ? s.rx : s.lx, y = k ? s.ry : s.ly;
            const double zone = k ? c.zones.right : c.zones.left;
            Text(sticks == 1 ? L"Palanca" : k ? L"Palanca derecha" : L"Palanca izquierda", cx - 150, 140, 300, 30, 17.5f, T.sub, Center);
            PadStick(cx, cy, r, x, y, (s.buttons & (k ? pads::kRS : pads::kLS)) != 0, static_cast<float>(zone));
            wchar_t line[96];
            swprintf_s(line, L"Posición: %+.2f, %+.2f", x, y);
            Text(line, cx - 150, 430, 300, 26, 16.6f, T.text, Center);
            if (c.measured) {
                swprintf_s(line, L"En reposo: %.0f %%   Alcance: %.0f %%", c.rest[k] * 100, c.reach[k] * 100);
                Text(line, cx - 150, 456, 300, 26, 16.6f, T.text, Center);
                swprintf_s(line, L"Zona muerta sugerida: %.0f %%", PadCalSuggested(k) * 100);
                Text(line, cx - 150, 482, 300, 26, 16.6f, T.accent, Center);
            }
        }
        if (c.phase) {
            const float total = c.phase == 1 ? kCalRest : kCalTurn, p = (std::min)(1.0f, static_cast<float>(now - c.phaseAt) / total);
            Text(c.phase == 1 ? L"Suelta las palancas y no las toques…" : L"Gira las palancas hasta el tope, dando vueltas completas.", 64, 528,
                 672, 36, 20.5f, T.text, Center);
            RRect(mid - 200, 574, 400, 8, 4, alpha(T.sub, 0.4f));
            RRect(mid - 200, 574, 400 * p, 8, 4, T.accent);
        } else if (!c.measured) {
            Text(L"Mide las palancas para ver cuánto se desvían en reposo y hasta dónde llegan.", 64, 528, 672, 36, 17.5f, T.sub, Center);
        }
    }

    // Right: the three rows and what they are.
    VRule(Px(1152), Px(150), Px(950), T.line);
    const float rx = 800, rw = 416, rh = 62, ry = 112;
    const wchar_t* const labels[3] = {L"Medir las palancas", L"Zona muerta (izquierda)", L"Zona muerta (derecha)"};
    for (int i = 0; i < 3; i++) {
        const float y = ry + i * rh;
        const double zone = i == 1 ? c.zones.left : c.zones.right;
        const bool off = i == 0 ? !cur : (zone < 0 || (i == 2 && c.single));
        if (i == c.sel && !c.phase) {
            if (S2()) RRect(rx, y, rw, rh, 6, T.focusBg);
            CursorFill(T.focusBg);
            Cursor(rx, y, rw, rh, 6);
        } else if (i + 1 != c.sel || c.phase) {
            HRule(rx + 10, rx + rw - 10, Snap(y + rh), T.line);
        }
        Text(labels[i], rx + 22, y, 280, rh, 19.5f, off ? T.sub : T.text);
        if (i > 0 && zone >= 0) {
            wchar_t value[32];
            swprintf_s(value, L"‹  %.0f %%  ›", zone * 100);
            Text(value, rx + rw - 150, y, 130, rh, 19.5f, off ? T.sub : T.accent, Right);
        }
        AddHit(rx, y, rw, rh, [i] {
            padCal.sel = i;
            OnButtonImpl(Btn::A);
        });
    }
    std::wstring note = L"La zona muerta es lo que la palanca puede moverse desde el centro sin que cuente. Aquí se cambia la que usa " + v.emu +
                        L" para este mando; la calibración del propio mando no se toca.";
    if (cur && !c.zones.found)
        note += L"\n◆ " + v.emu + L" no tiene una entrada para este mando: configúralo una vez en el emulador.";
    else if (cur && (c.zones.left < 0 || (!c.single && c.zones.right < 0)))
        note += L"\n◆ El emulador no ha guardado la zona muerta de esa palanca: usa la suya predeterminada.";
    if (!runningId.empty()) note += L"\n◆ Con un programa abierto no se puede cambiar.";
    TextWrap(note, rx + 22, ry + 3 * rh + 18, rw - 40, 320, 16.0f, T.sub);

    if (c.phase) Footer({{L"B", L"Cancelar", Btn::B}});
    else if (c.sel == 0) Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Medir", Btn::A}});
    else if (c.measured) Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Usar la sugerida", Btn::A}});
    else Footer({{L"B", L"Atrás", Btn::B}});
}

void PadCalButton(Btn b) {
    PadCalUi& c = padCal;
    if (c.phase) {
        if (b == Btn::B) {
            c.phase = 0;
            PlaySfx(Sfx::Back);
        }
        return;
    }
    switch (b) {
    case Btn::B:
        PlaySfx(Sfx::Back);
        Pop();
        break;
    case Btn::Up:
    case Btn::Down: {
        const int to = c.sel + (b == Btn::Down ? 1 : -1);
        if (to < 0 || to > 2) {
            if (S2() && heldRepeat) return;
            BumpCursor(0, b == Btn::Down ? 1.0f : -1.0f);
            PlaySfx(Sfx::Bump);
        } else {
            c.sel = to;
            PlaySfx(Sfx::Move);
        }
        break;
    }
    case Btn::Left:
    case Btn::Right:
        if (c.sel > 0) PadCalSetZone(c.sel == 2, (c.sel == 2 ? c.zones.right : c.zones.left) + (b == Btn::Right ? 0.01 : -0.01));
        break;
    case Btn::A:
        if (c.sel == 0) {
            if (c.id.empty()) {
                PlaySfx(Sfx::Bump);
                break;
            }
            PlaySfx(Sfx::Ok);
            c.phase = 1;
            c.phaseAt = now;
            c.samples = 0;
            c.measured = false;
            for (float& s : c.sum) s = 0;
        } else if (c.measured) {
            PadCalSetZone(c.sel == 2, PadCalSuggested(c.sel - 1));
        }
        break;
    default: break;
    }
}

// -- "Cambiar la asignación de los botones" --
//
// The menu's own buttons, for every pad at once (pads.h has the rules and
// applies them in Poll). Inside a game the mapping is the emulator's, which
// NSW-Home never writes. The keyboard's keys stay as they are, so there is
// always a way through the menu.

struct RemapUi {
    int sel = 0;      // 0..13: the actions, in two columns of seven; 14: restore
    int column = 0;   // the column the cursor last was in
    int capture = -1; // the action waiting for its new button
    double captureAt = 0;
    bool first = true;
    std::map<std::string, uint32_t> held; // each pad's buttons at the last frame
    int debugRelease = 0;                 // test states: frames until the made-up press ends
} remapUi;

constexpr double kRemapWait = 8.0; // seconds a new button is waited for

void OpenRemap() {
    remapUi = RemapUi();
    Push(Scr::Remap);
}

void RemapSave(const int map[pads::kActions]) {
    pads::SetRemap(map);
    bool stock = true;
    for (int a = 0; a < pads::kActions; a++) stock = stock && map[a] == a;
    if (stock) cfg.console.erase("pads.remap");
    else cfg.console["pads.remap"] = pads::RemapText(map);
    SaveConfig();
}

void RemapTake(int button) {
    int map[pads::kActions];
    pads::GetRemap(map);
    const int action = remapUi.capture;
    remapUi.capture = -1;
    if (!pads::RemapAssign(map, action, button)) {
        PlaySfx(Sfx::Error);
        Info(L"Esa acción solo puede estar en un botón que\ntengan todos los mandos: A, B, X, Y, L, R o +.\nLa asignación no ha cambiado.");
        return;
    }
    RemapSave(map);
    PlaySfx(Sfx::Ok);
}

void DrawRemap() {
    Theme T = Th();
    RemapUi& u = remapUi;
    Header(5, L"Cambiar la asignación de los botones");
    int map[pads::kActions];
    pads::GetRemap(map);
    const float top = 96, y0 = top + 36, rh = 53, cw = 556;
    Text(L"Botones del menú de NSW-Home. Dentro de los programas, la asignación es la del emulador, que NSW-Home no cambia.", 64, top, 1152,
         30, 17.0f, T.sub, Center);
    const auto row = [&](int i, float x, float y, float w, const wchar_t* label) {
        const bool focused = i == u.sel && u.capture < 0;
        if (focused) {
            if (S2()) RRect(x, y, w, rh, 6, T.focusBg);
            CursorFill(T.focusBg);
            Cursor(x, y, w, rh, 6);
        } else {
            HRule(x + 10, x + w - 10, Snap(y + rh), T.line);
        }
        Text(label, x + 22, y, w - 190, rh, 19.5f, T.text);
        AddHit(x, y, w, rh, [i] {
            remapUi.sel = i;
            OnButtonImpl(Btn::A);
        });
    };
    for (int a = 0; a < pads::kActions; a++) {
        const float x = a < 7 ? 64.0f : 660.0f, y = y0 + (a % 7) * rh;
        row(a, x, y, cw, kActionName[a]);
        const wchar_t* name = kButtonName[map[a]];
        const float w = (std::max)(64.0f, TextWidth(name, 16.6f) + 34);
        PadChip(name, x + cw - 22 - w, y + (rh - 32) / 2, w, 32, false, map[a] != a);
    }
    row(pads::kActions, 64, y0 + 7 * rh + 8, 1152, L"Restablecer la asignación predeterminada");
    Text(L"◆ «Confirmar» y «Atrás» solo pueden estar en A, B, X, Y, L, R o +. Las teclas del teclado no cambian.", 64, y0 + 8 * rh + 14, 1152, 28,
         16.0f, T.sub, Center);

    if (u.capture < 0) {
        Footer({{L"B", L"Atrás", Btn::B}, {L"A", u.sel == pads::kActions ? L"Confirmar" : L"Cambiar", Btn::A}});
        return;
    }
    // Waiting for the new button: every pad is listened to, none drives the menu.
    pads::Hush();
    Touch();
    if (u.debugRelease > 0 && --u.debugRelease == 0) pads::DebugHold(0, pads::State());
    Footer({{L"B", L"Cancelar", Btn::B}});
    hits.clear(); // the box blocks the page under it
    Rect(0, 0, W, H, rgb(0x000000, 0.6f));
    const float bw = 620, bh = 196, bx = (W - bw) / 2, by = 230;
    AddHit(bx, by + 144, bw, 44, [] { OnButtonImpl(Btn::B); });
    RRect(bx, by, bw, bh, S2() ? 8.0f : 5.0f, T.dialog);
    Text(L"Pulsa en un mando el botón para", bx, by + 30, bw, 36, 21.5f, T.text, Center);
    Text(std::wstring(L"«") + kActionName[u.capture] + L"»", bx, by + 70, bw, 44, 26.0f, T.accent, Center);
    const float left = 1 - (std::min)(1.0f, static_cast<float>((now - u.captureAt) / kRemapWait));
    RRect(bx + 110, by + 134, bw - 220, 6, 3, alpha(T.sub, 0.4f));
    RRect(bx + 110, by + 134, (bw - 220) * left, 6, 3, T.accent);
    Text(L"Esc: cancelar", bx, by + 150, bw, 30, 16.0f, T.sub, Center);
    int pressed = -1;
    for (const auto& l : pads::Snapshot(cfg.swapAB)) {
        // (What was already down when the wait began, the confirming button
        // above all, is not a choice.)
        const uint32_t before = u.first ? l.menu : u.held[l.pad.id];
        const uint32_t fresh = l.menu & ~before;
        u.held[l.pad.id] = l.menu;
        for (int k = 0; k < pads::kMenuButtons && pressed < 0; k++)
            if (fresh & (1u << k)) pressed = k;
    }
    u.first = false;
    if (pressed >= 0) {
        RemapTake(pressed);
    } else if (left <= 0) {
        u.capture = -1;
        PlaySfx(Sfx::Back);
    }
}

void RemapButton(Btn b) {
    RemapUi& u = remapUi;
    if (u.capture >= 0) {
        // Only the keyboard gets here (the pads are hushed) and, in test
        // states, the script, whose buttons stand for a made-up pad's.
        if (b == Btn::B) {
            u.capture = -1;
            PlaySfx(Sfx::Back);
        } else if (pads::DebugFaking()) {
            pads::State s;
            const uint32_t none = 0, x = pads::kWest, y = pads::kNorth, l = pads::kL, zr = pads::kZR, plus = pads::kPlus, home = pads::kHome;
            s.buttons = b == Btn::X ? x : b == Btn::Y ? y : b == Btn::L ? l : b == Btn::R ? zr : b == Btn::Plus ? plus : b == Btn::Minus ? home : none;
            pads::DebugHold(0, s);
            u.debugRelease = 3;
        }
        return;
    }
    const auto bump = [&](float dx, float dy) {
        if (S2() && heldRepeat) return;
        BumpCursor(dx, dy);
        PlaySfx(Sfx::Bump);
    };
    const auto go = [&](int to) {
        u.sel = to;
        if (to < pads::kActions) u.column = to / 7;
        PlaySfx(Sfx::Move);
    };
    const bool last = u.sel == pads::kActions;
    switch (b) {
    case Btn::B:
        PlaySfx(Sfx::Back);
        Pop();
        break;
    case Btn::Up:
        if (last) go(u.column * 7 + 6);
        else if (u.sel % 7 > 0) go(u.sel - 1);
        else bump(0, -1);
        break;
    case Btn::Down:
        if (last) bump(0, 1);
        else if (u.sel % 7 < 6) go(u.sel + 1);
        else go(pads::kActions);
        break;
    case Btn::Left:
        if (!last && u.sel >= 7) go(u.sel - 7);
        else bump(-1, 0);
        break;
    case Btn::Right:
        if (!last && u.sel < 7) go(u.sel + 7);
        else bump(1, 0);
        break;
    case Btn::A:
        if (last) {
            Ask(L"¿Quieres restablecer la asignación predeterminada\nde los botones del menú?", {L"Cancelar", L"Restablecer"}, [](int choice) {
                if (choice != 1) return;
                int stock[pads::kActions];
                for (int a = 0; a < pads::kActions; a++) stock[a] = a;
                RemapSave(stock);
            });
        } else {
            PlaySfx(Sfx::Ok);
            u.capture = u.sel;
            u.captureAt = now;
            u.first = true;
            u.held.clear();
        }
        break;
    default: break;
    }
}

// Once a frame, whatever page is up: HOME held for a second on a pad opens the
// remapping, when the console's "configuración de acceso rápido" row is on
// (the console opens its quick settings that way; NSW-Home has no such panel).
void PadsFrame() {
    if (!pads::TakeHomeHold() || Cos("accessibility.remap_quick", "off") != "on") return;
    const Scr top = stack.back();
    if (trans.on || dlg.open || entry.open || top == Scr::Remap || top == Scr::Setup || top == Scr::Launch || top == Scr::Sleep ||
        top == Scr::Who || top == Scr::Viewer)
        return;
    OpenRemap();
}

// -- "Desincronizar los mandos" --

// NSW-Home cannot unpair a Bluetooth device and should not: what it can forget
// is what it remembers itself about the pads.
void PadsForget() {
    Ask(L"¿Quieres que NSW-Home olvide los mandos?\nSe olvidarán el orden de los jugadores\ny las parejas de Joy-Con.",
        {L"Cancelar", L"Desincronizar"}, [](int choice) {
            if (choice != 1) return;
            PadOrderSave({});
            cfg.console.erase("pads.order");
            SaveConfig();
            Info(std::wstring(L"NSW-Home ha olvidado los mandos.\nLa sincronización por Bluetooth se quita\nen la configuración de Bluetooth ") +
                 (pads::Reads() ? L"de Windows." : L"del sistema."));
        });
}

// -- the rows --

// What one of this block's console rows is here. Left as it came (cosmetic, or
// "only on the console") where this system has no backend for it.
void PadsConsoleRow(Row& r, const std::string& id) {
    const bool reads = pads::Reads() || pads::DebugFaking();
    if (id == "controllers.test_input") {
        if (!reads) return;
        r.act = [] { OpenPadTest(); };
        r.real = true;
        r.note = L"Verifica si los mandos y el teclado funcionan correctamente.";
    } else if (id == "controllers.calibrate_sticks") {
        if (!reads) return;
        r.act = [] { OpenPadCal(); };
        r.real = true;
    } else if (id == "accessibility.remap") {
        if (!reads) return;
        r.act = [] { OpenRemap(); };
        r.real = true;
    } else if (id == "accessibility.remap_quick") {
        if (!reads) return;
        r.real = true;
        r.note = L"Al activar esta opción, podrás abrir la asignación de los botones del menú desde cualquier pantalla "
                 L"manteniendo pulsado HOME en un mando durante un segundo.";
    } else if (id == "controllers.pro_wired") {
        if (!reads) return;
        r.on = Cos(id, "on") == "on";
        const bool on = r.on;
        r.act = [id, on] {
            SetCos(id, on ? "off" : "on");
            pads::SetProWired(!on);
        };
        r.real = true;
        r.note = L"Si activas esta opción, NSW-Home pedirá a un mando Pro conectado con el cable USB que se comunique por cable: "
                 L"es lo único que NSW-Home envía a un mando. Con la opción desactivada, el mando Pro solo se lee por Bluetooth.";
    } else if (id == "controllers.disconnect") {
        r.act = PadsForget;
        r.real = true;
    } else if (id == "controllers.rumble" || id == "controllers.mouse_mode" || id == "controllers.mouse_sensitivity" ||
               id == "controllers.usb_keyboard") {
        if (OnAndroid()) return;
        const PadVals& v = PadValsNow();
        const bool toggle = r.kind == RK::Toggle;
        const auto none = [&](const std::wstring& why) {
            r.on = false;
            r.disabled = true;
            r.act = nullptr;
            r.note = why;
            if (!toggle) r.value = L"—";
        };
        r.real = true;
        if (!v.file) {
            EmuMissing(r);
            if (!toggle) r.value.clear();
            r.on = false;
            return;
        }
        const std::wstring title = r.label;
        if (id == "controllers.rumble") {
            const std::string wish = PadWish("rumble");
            if (!v.rumble.entries) return none(v.emu + L" no tiene ningún mando con vibración configurado. Configura el mando una vez en el emulador.");
            r.on = wish.empty() ? v.rumble.on == v.rumble.entries : wish == "on";
            const bool on = r.on;
            r.act = [on] { PadWishSet("rumble", on ? "off" : "on"); };
            r.note = L"Activa o desactiva la vibración de los mandos en los programas. Es el ajuste de vibración de " + v.emu + L".";
            if (!wish.empty()) AddNote(r, kPadWaits);
        } else if (id == "controllers.mouse_mode") {
            const std::string wish = PadWish("mouse");
            if (v.mouse < 0) return none(v.emu + L" no tiene este ajuste.");
            r.on = wish.empty() ? v.mouse == 1 : wish == "on";
            const bool on = r.on;
            r.act = [on] { PadWishSet("mouse", on ? "off" : "on"); };
            r.note = L"Con esta opción activada, los programas reciben el ratón del PC como un ratón conectado a la consola. Es el ajuste de " +
                     v.emu + L".";
            if (!wish.empty()) AddNote(r, kPadWaits);
        } else if (id == "controllers.mouse_sensitivity") {
            const std::string wish = PadWish("mouse_speed");
            if (v.speed < 0) {
                // Real only where the emulator has the number: elsewhere the row
                // is kept, greyed, in identical mode and says why.
                r.real = false;
                return none(v.emu + L" no tiene un ajuste de sensibilidad del ratón: solo existe en Eden, Citron, Suyu y los demás emuladores de su familia.");
            }
            const int cur = wish.empty() ? v.speed : atoi(wish.c_str());
            r.value = std::to_wstring(cur) + L" %";
            r.act = [title, cur] {
                static const int steps[] = {10, 25, 50, 75, 100};
                std::vector<std::wstring> items;
                int at = -1;
                for (int i = 0; i < 5; i++) {
                    items.push_back(std::to_wstring(steps[i]) + (steps[i] == 50 ? L" % (normal)" : L" %"));
                    if (steps[i] == cur) at = i;
                }
                Pick(title, items, at, [](int i) {
                    if (i >= 0 && i < 5) PadWishSet("mouse_speed", std::to_string(steps[i]));
                });
            };
            r.note = L"Velocidad con la que el ratón mueve una palanca en " + v.emu + L", del 1 al 100. Es un ajuste del propio emulador.";
            if (!wish.empty()) AddNote(r, kPadWaits);
        } else {
            const std::string wish = PadWish("keyboard");
            if (v.keyboard < 0) return none(v.emu + L" no tiene este ajuste.");
            const bool on = wish.empty() ? v.keyboard == 1 : wish == "on";
            r.value = !on ? L"Desactivado" : v.layout.empty() ? L"Activado" : v.layout;
            r.act = [title, on] {
                Pick(title, {L"No pasar el teclado a los programas", L"Pasar el teclado a los programas"}, on ? 1 : 0, [](int i) {
                    if (i == 0 || i == 1) PadWishSet("keyboard", i == 1 ? "on" : "off");
                });
            };
            r.note = L"Decide si los programas ven el teclado del PC como un teclado USB. Es el ajuste de " + v.emu + L".";
            if (!v.layout.empty()) r.note += L" La distribución es la del sistema: " + v.layout + L".";
            if (!wish.empty()) AddNote(r, kPadWaits);
        }
    }
}

// Test states of this block (each also with -2 / -2l, like every state):
//   padtest        the input test with what is really connected
//   padtest-set    with four made-up pads holding buttons, sticks and triggers, and keys held;
//                  in a script, Y makes a pad keep a button down (the way out)
//   padtest-none   with no pad
//   padcal         the stick page with what is really connected
//   padcal-set     with a made-up DualSense on show; A on the first row measures (the pad moves by itself)
//   padcal-done    the same once measured
//   padcal-none    with no pad
//   remap          the remapping page
//   remap-set      with made-up pads: in a script, after A on an action, X Y L R PLUS MINUS are a pad's
//                  X, Y, L, ZR, + and HOME
//   remap-capture  waiting for the new button of "Confirmar"
//   remap-changed  with a changed mapping shown (in memory only)
bool DebugPadRows(const std::wstring& name) {
    const auto held = [](uint32_t buttons, float lx, float ly, float rx, float ry, float lt, float rt) {
        pads::State s;
        s.buttons = buttons, s.lx = lx, s.ly = ly, s.rx = rx, s.ry = ry, s.lt = lt, s.rt = rt;
        s.right = true;
        s.analog = lt > 0 || rt > 0;
        return s;
    };
    if (name == L"padtest" || name == L"padtest-set" || name == L"padtest-none") {
        if (name != L"padtest") pads::DebugFake(name == L"padtest-none" ? "" : "ds5:85 pro:50 jc:100 xbox");
        if (name == L"padtest-set") {
            pads::DebugHold(0, held(pads::kSouth | pads::kL | pads::kZL | pads::kLS, 0.62f, 0.48f, -0.3f, -0.8f, 0.72f, 0.18f));
            pads::DebugHold(1, held(pads::kNorth | pads::kUp | pads::kPlus | pads::kZR, -0.9f, 0.1f, 0.2f, 0.25f, 0, 0));
            pads::DebugHold(3, held(pads::kHome | pads::kWest, 0.5f, -0.5f, 0, 0, 0, 0)); // the right Joy-Con of the pair
            pads::DebugHold(4, held(pads::kEast | pads::kR | pads::kRS, 0, -1.0f, 0.7f, 0.7f, 0.05f, 1.0f));
        }
        OpenPadTest();
        if (name == L"padtest-set") padTest.debugKeys = {L"A", L"Espacio", L"Flecha arriba", L"Mayús"};
        return true;
    }
    if (name == L"padcal" || name == L"padcal-set" || name == L"padcal-done" || name == L"padcal-none") {
        if (name != L"padcal") pads::DebugFake(name == L"padcal-none" ? "" : "ds5:85 pro:50");
        if (name == L"padcal-set" || name == L"padcal-done") pads::DebugHold(0, held(pads::kSouth, 0.36f, 0.22f, -0.12f, 0.64f, 0, 0));
        OpenPadCal();
        if (name == L"padcal-done") {
            padCal.measured = true;
            padCal.rest[0] = 0.049f, padCal.rest[1] = 0.067f;
            padCal.reach[0] = 0.93f, padCal.reach[1] = 0.97f;
        }
        return true;
    }
    if (name == L"remap" || name == L"remap-set" || name == L"remap-capture" || name == L"remap-changed") {
        if (name != L"remap") pads::DebugFake("ds5:85 pro:50");
        if (name == L"remap-changed") {
            int map[pads::kActions];
            pads::GetRemap(map);
            pads::RemapAssign(map, pads::kConfirm, 6);
            pads::RemapAssign(map, 10, 14);
            pads::SetRemap(map);
        }
        OpenRemap();
        if (name == L"remap-capture") {
            remapUi.capture = pads::kConfirm;
            remapUi.captureAt = now;
            remapUi.sel = pads::kConfirm;
        }
        return true;
    }
    return false;
}

// ==== the menu's language (lang.h, lang_strings.inc) ==========================
// Every text reaches the screen through gfx's text calls, which hand it to
// LangText first: the Spanish this file is written in comes out in the language
// chosen in Settings > Consola > Idioma, the console's own wording read from
// the owner's firmware where the console has the row, ours elsewhere. A new
// Spanish string needs a line in lang_strings.inc and nothing here. Out of
// reach: words inside pictures (the local pack), a Spanish text that would
// need two translations in two places, and text that is built from parts the
// table has no pattern for (tools/lang_missing.py lists what is left).

// The language a config with none has: Spanish when it was written before
// NSW-Home had languages (its first-start assistant has been through: the
// owner's own), English (United States) for a new install.
std::string LangCode() {
    auto it = cfg.console.find("system.language");
    if (it != cfg.console.end() && lang::Find(it->second) >= 0) return it->second;
    return cfg.setupDone ? "EUes" : "USen";
}

const std::wstring& LangText(const std::wstring& s) {
    return lang::Tr(s);
}

void LangApply() {
    lang::Use(lang::Find(LangCode()), FirmwareDir(), KeysDir());
}

// Called once, before anything is drawn.
void LangInit() {
    // Kept with the config from the next save on, so that the default does
    // not change when the assistant marks the install as set up.
    cfg.console.emplace("system.language", LangCode());
    LangApply();
    gfx::SetTextFilter(LangText);
    // Names are data, not NSW-Home's wording: never reported as missing.
    for (const Game& g : games) {
        lang::Ignore(g.name);
        lang::Ignore(g.publisher);
    }
    for (const User& u : cfg.users) lang::Ignore(u.name);
    lang::RecordFromEnv();
}

// The games start in the same language, where the emulator has it.
void LangToEmulator(const lang::Language& l) {
    const std::string key = EmuKey("system_language");
    const ryucfg::Setting* s = key.empty() ? nullptr : EmuSetting(key);
    if (!s) return;
    const std::string want = emuKind.empty() ? l.ryujinx : l.number;
    for (const ryucfg::Choice& c : s->choices)
        if (c.value == want) {
            if (EmuGet(key) != want) EmuSet(key, want);
            return;
        }
}

// `firstStart`: as the console's first start does, the language also brings
// its region's clock (12-hour in the United States).
void LangChoose(int i, bool firstStart) {
    if (i < 0 || i >= static_cast<int>(lang::All().size())) return;
    const lang::Language& l = lang::All()[i];
    cfg.console["system.language"] = l.code;
    if (firstStart) cfg.clock24 = l.clock24;
    SaveConfig();
    LangApply();
    LangToEmulator(l);
}

std::vector<std::wstring> LangNames() {
    std::vector<std::wstring> names;
    for (const lang::Language& l : lang::All()) names.push_back(l.name);
    return names;
}

// Settings > Consola > Idioma.
void LangRow(Row& r) {
    r.kind = RK::Plain;
    r.value = lang::Cur().name;
    const std::wstring title = r.label;
    r.act = [title] { Pick(title, LangNames(), lang::Current(), [](int i) { LangChoose(i, false); }); };
    r.real = true;
}

// Test states:
//   lang:<code>:<state>   any state with the menu in that language ("lang:USen:home-2"), the
//                         clock that language's own; "lang:<code>!:<state>" as if there were no
//                         firmware to read the console's wording and fonts from
//   langdump:<file>       every Settings row's label, value and description, in every language:
//                         language, section, field, Spanish, as shown, where it comes from
//                         (f firmware, o ours, k a name, u untranslated); add -2 / -1 for a style
//   langrow... / langrows:<file>   every row of every page pressed once: DebugLangRows, below
bool DebugLangRows(const std::wstring& name);
bool DebugLang(const std::wstring& name) {
    if (DebugLangRows(name)) return true;
    if (name.rfind(L"lang:", 0) == 0) {
        size_t colon = name.find(L':', 5);
        if (colon == std::wstring::npos) return false;
        std::string code = Narrow(name.substr(5, colon - 5));
        const bool noFirmware = !code.empty() && code.back() == '!'; // "lang:CNzh!:...": as without firmware
        if (noFirmware) code.pop_back();
        const int i = lang::Find(code);
        if (i < 0) return false;
        cfg.console["system.language"] = lang::All()[i].code;
        if (noFirmware) lang::Use(i, L"", L"");
        else LangApply();
        DebugGoto(name.substr(colon + 1));
        cfg.clock24 = lang::All()[i].clock24;
        return true;
    }
    if (name.rfind(L"langdump:", 0) != 0) return false;
    auto ends = [&name](const wchar_t* s) {
        size_t k = wcslen(s);
        return name.size() > k && name.compare(name.size() - k, k, s) == 0;
    };
    if (ends(L"-2") || ends(L"-2l")) return false; // DebugGoto sets the style and comes back
    std::wstring file = name.substr(9);
    if (ends(L"-1")) {
        cfg.menuStyle = 1;
        file.resize(file.size() - 2);
    }
    cfg.identical = true;
    std::string out = "language\tsection\tfield\tspanish\tshown\tsource\n";
    auto flat = [](std::wstring t) {
        for (wchar_t& c : t)
            if (c == L'\n' || c == L'\t') c = L' ';
        return Narrow(t);
    };
    const std::string keep = cfg.console["system.language"];
    for (const lang::Language& l : lang::All()) {
        cfg.console["system.language"] = l.code;
        LangApply();
        BuildSettings();
        auto put = [&](const std::wstring& sec, const char* field, const std::wstring& es) {
            if (es.empty()) return;
            const char src = lang::Source(es);
            out += std::string(l.code) + "\t" + flat(sec) + "\t" + field + "\t" + flat(es) + "\t" + flat(lang::Tr(es)) + "\t" + src + "\n";
        };
        put(L"", "title", settings.title);
        for (const Cat& c : settings.cats) {
            put(c.name, "section", c.name);
            for (const Row& r : c.rows()) {
                if (r.kind == RK::Gap) continue;
                put(c.name, r.heading ? "heading" : "label", r.label);
                put(c.name, "value", r.value);
                put(c.name, "note", r.note);
            }
        }
    }
    cfg.console["system.language"] = keep;
    LangApply();
    FILE* f = nullptr;
    if (!_wfopen_s(&f, file.c_str(), L"wb") && f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
    return true;
}

// ---- settings: "Actualizar la consola" looks for a newer NSW-Home ----------------
// The row asks GitHub for the latest release of NSW-Home's own repository
// (update.cpp: what is sent, what is checked, how the program is replaced) and
// answers in the console's dialogs: the waiting box with [Cancelar] while it
// asks or downloads, then one box with the answer. Nothing is asked unless the
// row is pressed. Offscreen test runs never reach the network: their answers
// are made up here (see DebugUpd), and update.cpp refuses as well.

struct UpdJob {
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<uint64_t> got{0};
    // A download that has arrived is installed only if it was not cancelled
    // first, and a cancel only counts if the installing has not begun:
    // 0 running, 1 cancelled, 2 installing.
    std::atomic<int> gate{0};
    update::Check check; // a check's answer
    std::string result;  // a download's outcome: "" installed, or one of update.h's codes
};

enum class UpdStage { Idle, Asking, Fetching };

struct UpdState {
    UpdStage stage = UpdStage::Idle;
    std::shared_ptr<UpdJob> job;
    std::wstring shown;  // the waiting box's text as last put up: tells our box from any other
    double at = 0;       // when the stage began (test runs time their made-up answers by it)
    update::Release rel; // the newer release on offer
    // Test runs: what the made-up GitHub answers and how a download ends.
    bool test = false, hold = false; // hold: a test state's waiting box stays as it is
    update::Answer fake = update::Answer::Latest;
    bool fakeAsset = true;
    std::string fakeResult; // "" installed, or the code the download or Install would give
} upd;

std::string UpdVersion() {
    return Narrow(kNSWHomeVersion);
}

// One of the answers. Put up in place of the waiting box, the menu stays
// darkened instead of brightening and darkening again.
void UpdSay(const std::wstring& text, std::vector<std::wstring> btns = {L"Aceptar"}, std::function<void(int)> done = nullptr) {
    const bool swap = dlg.open;
    Ask(text, std::move(btns), std::move(done));
    if (swap) {
        dlg.openAt = now - 1;
        dlg.anim = 1;
    }
}

void UpdWait(const std::wstring& text);

void UpdOpenPage(const std::string& tag) {
    if (upd.test) return; // nothing opens on the desktop in a test run
    if (!update::OpenPage(tag)) UpdSay(L"No se ha podido abrir el navegador.\n\n" + Widen(update::PageUrl(tag)));
}

// The release's page instead, for when this program cannot be replaced from here.
void UpdOfferPage(const std::wstring& text) {
    const std::string tag = upd.rel.tag;
    UpdSay(text, {L"Cerrar", L"Abrir la página"}, [tag](int b) {
        if (b == 1) UpdOpenPage(tag);
    });
}

void UpdRestart() {
    if (upd.test) return;
    if (!runningId.empty()) {
        UpdSay(L"Hay un programa en uso.\nCiérralo para reiniciar NSW-Home.");
        return;
    }
    if (update::Restart()) QuitApp();
    else UpdSay(L"No se ha podido reiniciar NSW-Home.\n\nLa nueva versión se usará la próxima vez que lo abras.");
}

void UpdOutcome(const std::string& result) {
    const std::wstring ver = Widen(upd.rel.version);
    if (result.empty()) {
        UpdSay(L"NSW-Home se ha actualizado a la versión " + ver + L".\nLa nueva versión se usará al reiniciar NSW-Home.",
               {L"Más tarde", L"Reiniciar"}, [](int b) {
                   if (b == 1) UpdRestart();
               });
    } else if (result == "CANCELLED") {
        UpdSay(L"La descarga se ha cancelado."); // the console's own words (dlg-one)
    } else if (result == "SIZE" || result == "DIGEST" || result == "NOT_PROGRAM") {
        UpdSay(L"El archivo descargado no coincide con la versión publicada y no se ha instalado.\n\nNo se ha cambiado nada.");
    } else if (result == "NETWORK") {
        UpdSay(L"No se ha podido completar la descarga.\n\nNo se ha cambiado nada. Comprueba la conexión a internet.");
    } else if (result == "NOT_WRITABLE") {
        UpdOfferPage(L"No se puede escribir en la carpeta de NSW-Home, así que no se puede actualizar desde aquí.\n\n"
                     L"Puedes descargar la nueva versión desde su página.");
    } else if (result == "LOCKED") {
        UpdOfferPage(L"No se ha podido sustituir el programa.\n\nNo se ha cambiado nada. Puedes descargar la nueva versión desde su página.");
    } else if (result == "ASIDE") {
        std::wstring aside = update::AsidePath(update::ProgramPath());
        aside = aside.substr(aside.find_last_of(L"\\/") + 1);
        UpdSay(L"No se ha podido completar la actualización.\n\nEl programa está ahora en su carpeta con el nombre " + aside + L".");
    } else if (result == "REFUSED") {
        UpdSay(L"La dirección de la descarga no es la de las versiones de NSW-Home.\n\nNo se ha descargado nada.");
    } else {
        UpdSay(L"No se ha podido guardar la descarga.\n\nNo se ha cambiado nada.");
    }
}

void UpdStartFetch() {
    if (!runningId.empty()) {
        // Replacing the program means starting it again, which would end the game.
        UpdSay(L"Hay un programa en uso.\nCiérralo para poder actualizar NSW-Home.");
        return;
    }
    const std::wstring program = update::ProgramPath();
    if (upd.test ? upd.fakeResult == "NOT_WRITABLE" : !update::FolderWritable(program)) {
        UpdOutcome("NOT_WRITABLE"); // known before anything is downloaded
        return;
    }
    upd.stage = UpdStage::Fetching;
    upd.at = now;
    UpdWait(L"Descargando la actualización…");
    if (upd.test) return;
    auto job = std::make_shared<UpdJob>();
    upd.job = job;
    const update::Asset asset = upd.rel.asset;
    const std::wstring file = update::DownloadPath(DataDir());
    const std::string ver = UpdVersion();
    std::thread([job, asset, file, program, ver] {
        std::string r = update::Download(asset, update::kRepository, file, ver, &job->got, &job->cancel);
        int running = 0;
        if (r.empty()) r = job->gate.compare_exchange_strong(running, 2) ? update::Install(program, file, asset) : "CANCELLED";
        job->result = r;
        job->done = true;
    }).detach();
}

void UpdShowAnswer(const update::Check& c) {
    upd.stage = UpdStage::Idle;
    const std::wstring cur = kNSWHomeVersion;
    switch (c.answer) {
    case update::Answer::Latest: UpdSay(L"NSW-Home ya tiene la última versión.\n\nVersión actual: " + cur); break;
    case update::Answer::None:
        UpdSay(L"Todavía no se ha publicado ninguna versión de NSW-Home.\n\nSe ha consultado github.com/" + Widen(update::kRepository) + L".");
        break;
    case update::Answer::Offline: UpdSay(L"No se ha podido conectar con el servidor.\n\nComprueba la conexión a internet."); break;
    case update::Answer::Refused: UpdSay(L"El servidor no admite más consultas por ahora.\n\nVuelve a intentarlo dentro de un rato."); break;
    case update::Answer::Odd: UpdSay(L"No se ha podido comprobar si hay una versión nueva.\n\nLa respuesta del servidor no es válida."); break;
    case update::Answer::Newer: {
        upd.rel = c.release;
        std::wstring text = L"Hay una nueva versión de NSW-Home: " + Widen(c.release.version) + L".\nVersión actual: " + cur + L".";
        const std::wstring notes = c.release.notes.empty() ? std::wstring() : L"\n\n" + Widen(c.release.notes);
        // The release's own notes, as published: not the menu's wording, never reported as untranslated.
        for (size_t at = 2, nl; at < notes.size(); at = nl + 1) {
            nl = notes.find(L'\n', at);
            if (nl == std::wstring::npos) nl = notes.size();
            lang::Ignore(notes.substr(at, nl - at));
        }
        if (c.release.hasAsset && update::CanInstall()) {
            UpdSay(text + notes, {L"Más tarde", L"Actualizar"}, [](int b) {
                if (b == 1) UpdStartFetch();
            });
        } else {
            const std::string tag = c.release.tag;
            UpdSay(text + L"\nNo incluye el programa para este sistema: puedes descargarla desde su página." + notes,
                   {L"Más tarde", L"Abrir la página"}, [tag](int b) {
                       if (b == 1) UpdOpenPage(tag);
                   });
        }
        break;
    }
    }
}

// Cancelar on the waiting box. A download that is already being installed
// is not stopped halfway: its outcome is shown when it is there.
void UpdCancel() {
    if (upd.stage == UpdStage::Fetching && upd.job) {
        int running = 0;
        if (!upd.job->gate.compare_exchange_strong(running, 1)) {
            UpdWait(L"Instalando la actualización…");
            return;
        }
    }
    if (upd.job) upd.job->cancel = true;
    const bool fetching = upd.stage == UpdStage::Fetching;
    upd.job.reset();
    upd.stage = UpdStage::Idle;
    if (fetching) UpdSay(L"La descarga se ha cancelado.");
}

void UpdWait(const std::wstring& text) {
    UpdSay(text, {L"Cancelar"}, [](int) { UpdCancel(); });
    dlg.busy = true;
    upd.shown = text;
}

// What a test run's GitHub would answer.
update::Check UpdFakeCheck() {
    update::Check c;
    c.answer = upd.fake;
    if (c.answer == update::Answer::Newer) {
        c.release.tag = "v0.2.0";
        c.release.version = "0.2.0";
        c.release.notes = "\xC2\xB7 Actualizar la consola busca versiones nuevas de NSW-Home.\n\xC2\xB7 Correcci\xC3\xB3n de errores.";
        c.release.hasAsset = upd.fakeAsset;
        if (upd.fakeAsset) {
            c.release.files = {update::kAssetName};
            c.release.asset.name = update::kAssetName;
            c.release.asset.size = 2u << 20;
        }
    }
    return c;
}

// Every frame: the waiting box's progress, and the answer once it is there.
void UpdFrame() {
    if (upd.stage == UpdStage::Idle) return;
    const bool ours = dlg.open && dlg.busy && dlg.msg == upd.shown;
    if (!ours) {
        // The box went some other way (HOME, a game that ended): so does the work.
        if (upd.job) {
            int running = 0;
            if (upd.job->gate.compare_exchange_strong(running, 1)) upd.job->cancel = true;
        }
        upd.job.reset();
        upd.stage = UpdStage::Idle;
        return;
    }
    if (dlg.pressAt >= 0) return; // Cancelar was chosen: the box is on its way out
    Touch();
    const double t = now - upd.at;
    const bool ready = upd.test ? !upd.hold && t >= (upd.stage == UpdStage::Asking ? 0.8 : 1.5) : upd.job && upd.job->done.load();
    if (upd.stage == UpdStage::Fetching && !ready) {
        // How much has arrived; a test run makes it up from the time.
        const uint64_t size = upd.rel.asset.size;
        const double part = upd.test ? (upd.hold ? 0.45 : t / 1.5) : size ? static_cast<double>(upd.job->got.load()) / size : 0.0;
        const std::wstring text = L"Descargando la actualización…  " + std::to_wstring(static_cast<int>((std::min)(part, 1.0) * 100)) + L" %";
        if (upd.shown.compare(0, 6, L"Instal") != 0 && text != upd.shown) dlg.msg = upd.shown = text;
        return;
    }
    if (!ready) return;
    if (upd.stage == UpdStage::Asking) {
        const update::Check c = upd.test ? UpdFakeCheck() : upd.job->check;
        upd.job.reset();
        UpdShowAnswer(c);
    } else {
        const std::string r = upd.test ? upd.fakeResult : upd.job->result;
        upd.job.reset();
        upd.stage = UpdStage::Idle;
        UpdOutcome(r);
    }
}

void UpdStartCheck() {
    if (upd.stage != UpdStage::Idle) return;
    upd.test = scTest;
    if (upd.test) update::SetOffscreen(true); // a second wall: update.cpp refuses too
    upd.stage = UpdStage::Asking;
    upd.at = now;
    UpdWait(L"Comprobando si hay actualizaciones…");
    if (upd.test) return;
    auto job = std::make_shared<UpdJob>();
    upd.job = job;
    const std::string ver = UpdVersion();
    std::thread([job, ver] {
        job->check = update::Ask(update::kRepository, ver, update::kAssetName, &job->cancel);
        job->done = true;
    }).detach();
}

// The row. Where nothing can be asked (no way to make the request on this
// system) it stays the console's: today's behaviour.
void UpdRow(Row& r, bool all) {
    if (!update::Available()) {
        r.note = all ? L"Versión actual: " + FirmwareText() : std::wstring();
        return;
    }
    r.note = std::wstring(L"Versión actual: ") + kNSWHomeVersion;
    r.act = UpdStartCheck;
    r.real = true;
}

// Test states (each also with -2 / -2l). None of them reaches the network.
//   updfake-<k>-<state>   any state, with GitHub's made-up answer and the download's made-up outcome:
//                         latest none offline refused odd | newer (installs) noasset |
//                         bad (fails the checks) ro (folder not writable) locked aside net (download cut)
//   upd-row               Consola with the cursor on "Actualizar la consola" (A then asks)
//   upd-check             the waiting box while it asks
//   upd-<answer>          the answer: latest newer noasset none offline refused odd
//   upd-fetch             the download at 45 %
//   upd-<outcome>         done bad ro locked aside net cancelled game
//   upd-<...>-1 / -1l     any upd- state in the first console's style, dark / light
bool DebugUpd(const std::wstring& name) {
    if (name.compare(0, 8, L"updfake-") == 0) {
        const size_t dash = name.find(L'-', 8);
        if (dash == std::wstring::npos) return false;
        const std::wstring k = name.substr(8, dash - 8);
        upd.fake = k == L"latest"    ? update::Answer::Latest
                   : k == L"none"    ? update::Answer::None
                   : k == L"offline" ? update::Answer::Offline
                   : k == L"refused" ? update::Answer::Refused
                   : k == L"odd"     ? update::Answer::Odd
                                     : update::Answer::Newer;
        upd.fakeAsset = k != L"noasset";
        upd.fakeResult = k == L"bad" ? "DIGEST" : k == L"ro" ? "NOT_WRITABLE" : k == L"locked" ? "LOCKED" : k == L"aside" ? "ASIDE"
                         : k == L"net" ? "NETWORK" : "";
        DebugGoto(name.substr(dash + 1));
        return true;
    }
    if (name.compare(0, 4, L"upd-") != 0) return false;
    if ((name.size() > 2 && name.compare(name.size() - 2, 2, L"-2") == 0) || (name.size() > 3 && name.compare(name.size() - 3, 3, L"-2l") == 0))
        return false; // DebugGoto takes the style off the name first
    std::wstring k = name.substr(4);
    for (const wchar_t* first : {L"-1", L"-1l"}) { // the first console's style, dark or light
        const size_t n = wcslen(first);
        if (k.size() > n && k.compare(k.size() - n, n, first) == 0) {
            cfg.menuStyle = 1;
            cfg.dark = n == 2;
            k.resize(k.size() - n);
        }
    }
    DebugGoto(L"pcrow:system.update");
    if (trans.on) { // the page is there at once, so that the box can go on top of it
        auto apply = trans.apply;
        trans = Transition();
        if (apply) apply();
    }
    upd.test = true;
    update::SetOffscreen(true);
    // The release a state after the answer is about (what upd-newer offers).
    upd.rel = update::Release();
    upd.rel.tag = "v0.2.0";
    upd.rel.version = "0.2.0";
    upd.rel.hasAsset = true;
    upd.rel.asset.name = update::kAssetName;
    upd.rel.asset.size = 2u << 20;
    auto answer = [](update::Answer a, bool asset) {
        upd.fake = a;
        upd.fakeAsset = asset;
        UpdShowAnswer(UpdFakeCheck());
    };
    if (k == L"row") {
    } else if (k == L"check") {
        UpdStartCheck();
        upd.hold = true;
    } else if (k == L"latest") answer(update::Answer::Latest, true);
    else if (k == L"newer") answer(update::Answer::Newer, true);
    else if (k == L"noasset") answer(update::Answer::Newer, false);
    else if (k == L"none") answer(update::Answer::None, true);
    else if (k == L"offline") answer(update::Answer::Offline, true);
    else if (k == L"refused") answer(update::Answer::Refused, true);
    else if (k == L"odd") answer(update::Answer::Odd, true);
    else if (k == L"fetch") {
        UpdStartFetch();
        upd.hold = true;
    } else if (k == L"done") UpdOutcome("");
    else if (k == L"bad") UpdOutcome("DIGEST");
    else if (k == L"ro") UpdOutcome("NOT_WRITABLE");
    else if (k == L"locked") UpdOutcome("LOCKED");
    else if (k == L"aside") UpdOutcome("ASIDE");
    else if (k == L"net") UpdOutcome("NETWORK");
    else if (k == L"cancelled") UpdOutcome("CANCELLED");
    else if (k == L"game") UpdSay(L"Hay un programa en uso.\nCiérralo para poder actualizar NSW-Home.");
    else return false;
    return true;
}

// ---- amiibo ---------------------------------------------------------------
// The amiibo section of Settings and "Usar un amiibo" in a running game's
// options. On the console each of the section's rows starts by asking for a
// figure to be held to the controller; here a figure is a file, so wherever
// the console would ask, NSW-Home shows its collection of dumps
// (<data>\amiibo, see amiibo.h) with "Añadir archivo…" and "Añadir carpeta…"
// first. Everything of this lives in this block: AmiiboRow() gives the
// section's rows their meaning (AddConsoleRow), AmiiboUseRow() adds the row
// to a running game's options, Scr::Amiibo is the collection and the page of
// one amiibo, drawn with the rows of Settings in one column like Scr::Page.

enum class AmMode { Register, DeleteData, Format, Use };
struct AmPage {
    bool one = false; // the page of one amiibo (its nickname and owner); else the collection
    AmMode mode = AmMode::Register;
    std::wstring title, file;
    int row = -1;
    float scroll = 0, want = 0;
    Glide glide;
};
std::vector<AmPage> amPages; // one per Scr::Amiibo on the stack, in order
std::vector<amiibo::Entry> amList;
bool amListed = false;
int amKeysState = -1; // -1 not read yet, 0 none, 1 stored and valid
amiibo::Keys amKeys;
std::map<std::string, std::pair<bool, amiibo::Facts>> amFacts; // by the dump's bytes: opened with the keys?
bool amWaiting = false; // a hand-over to the emulator is under way
bool amTestOn = false;  // an amiibo test state is open
std::vector<std::wstring> amRowFile; // the file of each row of the page drawn last (empty: not an entry)

// Test states: the pickers answer with the made-up files that
// tools\amiibo_test.cpp leaves in <copy>\local\amiibo_test.
std::wstring AmTestDir() {
    const std::wstring data = DataDir();
    return data.substr(0, data.find_last_of(L"\\/")) + L"\\local\\amiibo_test";
}

bool amTest() { // no system picker in any offscreen run
    return amTestOn || pcTest;
}

void AmRefresh() {
    amList = amiibo::List();
    amListed = true;
    for (const amiibo::Entry& e : amList) lang::Ignore(e.name); // names are data, not the menu's wording
    amKeysState = amiibo::LoadKeys(amKeys) ? 1 : 0;
    amFacts.clear();
}
const std::vector<amiibo::Entry>& AmList() {
    if (!amListed) AmRefresh();
    return amList;
}
bool AmHaveKeys() {
    if (amKeysState < 0) AmRefresh();
    return amKeysState == 1;
}
const amiibo::Entry* AmEntry(const std::wstring& file) {
    for (const amiibo::Entry& e : AmList())
        if (e.file == file) return &e;
    return nullptr;
}
// What the stored keys read from a dump (nullptr without keys, or keys that do not open it).
const amiibo::Facts* AmFactsOf(const amiibo::Entry& e) {
    if (!AmHaveKeys()) return nullptr;
    auto it = amFacts.find(e.bytes);
    if (it == amFacts.end()) {
        std::string plain;
        const bool ok = amiibo::Decode(amKeys, e.bytes, plain);
        it = amFacts.emplace(e.bytes, std::make_pair(ok, ok ? amiibo::Read(plain) : amiibo::Facts())).first;
        lang::Ignore(it->second.second.nickname);
        lang::Ignore(it->second.second.owner);
    }
    return it->second.first ? &it->second.second : nullptr;
}

int AmDepth() {
    return static_cast<int>(std::count(stack.begin(), stack.end(), Scr::Amiibo));
}
AmPage* AmTop() {
    const int d = AmDepth();
    return d > 0 && d <= static_cast<int>(amPages.size()) ? &amPages[d - 1] : nullptr;
}

void AmOpen(AmPage p) {
    // Pages past the stack's own count were left behind by going back.
    amPages.resize((std::min)(amPages.size(), static_cast<size_t>(AmDepth())));
    amPages.push_back(std::move(p));
    picker.open = false;
    AmRefresh();
    Push(Scr::Amiibo);
}

void AmOpenCollection(AmMode mode, const std::wstring& title) {
    AmPage p;
    p.mode = mode;
    p.title = title;
    AmOpen(p);
}

std::wstring AmPickDump() {
    return amTest() ? AmTestDir() + L"\\in\\Figura de prueba.bin" : amiibo::PickDump();
}
std::wstring AmPickFolder() {
    return amTest() ? AmTestDir() + L"\\in\\set" : PickFolder();
}
std::wstring AmPickKeys() {
    return amTest() ? AmTestDir() + L"\\keys\\key_retail_fake.bin" : amiibo::PickKeyFile();
}

std::wstring AmCount(int n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

void AmAddFile() {
    const std::wstring file = AmPickDump();
    if (file.empty()) return;
    const amiibo::Imported r = amiibo::ImportFile(file);
    AmRefresh();
    if (r.added) Info(L"Se ha añadido «" + r.name + L"» a la colección.");
    else if (r.had) Info(L"Ese amiibo ya está en la colección.");
    else if (r.failed) Info(L"No se ha podido copiar el archivo.");
    else Info(L"Ese archivo no es un amiibo: no tiene el tamaño ni la forma de un volcado de amiibo.");
}

void AmAddFolder() {
    const std::wstring dir = AmPickFolder();
    if (dir.empty()) return;
    const amiibo::Imported r = amiibo::ImportFolder(dir);
    AmRefresh();
    if (!r.added && !r.had && !r.failed) {
        Info(L"No se ha encontrado ningún amiibo en esa carpeta.");
        return;
    }
    std::wstring msg = r.added ? L"Se han añadido " + AmCount(r.added, L"amiibo", L"amiibo") + L" a la colección."
                               : std::wstring(L"No se ha añadido ningún amiibo.");
    if (r.had) msg += L"\nYa estaban en la colección: " + std::to_wstring(r.had) + L".";
    if (r.skipped) msg += L"\nArchivos que no son amiibo: " + std::to_wstring(r.skipped) + L".";
    if (r.failed) msg += L"\nNo se han podido copiar: " + std::to_wstring(r.failed) + L".";
    Info(msg);
}

void AmSetKeys() {
    const std::wstring file = AmPickKeys();
    if (file.empty()) return;
    const std::string err = amiibo::SetKeys(file);
    AmRefresh();
    if (err == "KEYS_INVALID")
        Info(L"Ese archivo no contiene las claves de amiibo.\n\nEl archivo que hace falta suele llamarse key_retail.bin y tiene 160 bytes.");
    else if (!err.empty())
        Info(L"No se han podido guardar las claves.");
    else
        Info(L"Claves de amiibo guardadas.\nNSW-Home guarda una copia en su carpeta de datos.");
}

void AmKeysMenu() {
    Pick(L"Claves de amiibo", {L"Cambiar el archivo de claves…", L"Olvidar las claves"}, -1, [](int i) {
        if (i == 0) AmSetKeys();
        if (i == 1)
            Ask(L"¿Olvidar las claves de amiibo?\n\nSe borrará la copia que guarda NSW-Home. Tu archivo no se toca.", {L"Cancelar", L"Olvidar"},
                [](int b) {
                    if (b != 1) return;
                    amiibo::ForgetKeys();
                    AmRefresh();
                });
    });
}

std::wstring AmError(const std::string& err) {
    if (err == "NO_KEYS") return L"Faltan las claves de amiibo (key_retail.bin).";
    if (err == "BAD_KEYS")
        return L"Las claves de amiibo no abren este archivo: no son las que lo firmaron o el archivo está dañado.\nNo se ha cambiado nada.";
    if (err == "VERIFY") return L"El resultado no ha pasado la comprobación.\nNo se ha cambiado nada.";
    if (err == "NOT_REGISTERED") return L"Este amiibo no está registrado.";
    if (err == "NO_DATA") return L"Este amiibo no tiene datos de juego.";
    if (err == "BLANK") return L"Este amiibo no tiene datos que borrar.";
    if (err == "BAD_NAME") return L"El apodo puede tener de 1 a 10 caracteres.";
    if (err == "NOT_OURS") return L"Ese archivo no está en la colección.";
    return L"No se ha podido guardar el archivo.";
}

void AmApply(const std::wstring& file, amiibo::Op op, const std::wstring& nick, const std::wstring& done) {
    const std::string err = amiibo::Apply(file, op, nick);
    AmRefresh();
    Info(err.empty() ? done : AmError(err));
}

// The facts of an entry for an operation, or a message saying why there are none.
const amiibo::Facts* AmReady(const amiibo::Entry& e) {
    if (!AmHaveKeys()) {
        Info(L"Para leer y cambiar los datos de un amiibo hacen falta las claves de amiibo (key_retail.bin).");
        return nullptr;
    }
    const amiibo::Facts* f = AmFactsOf(e);
    if (!f) Info(AmError("BAD_KEYS"));
    return f;
}

std::wstring AmGame(uint64_t id) {
    wchar_t b[24];
    swprintf_s(b, L"%016llX", static_cast<unsigned long long>(id));
    return b;
}

void AmUse(const std::wstring& file);

// A on an entry: what the page was opened for.
void AmChoose(AmMode mode, const std::wstring& file) {
    const amiibo::Entry* e = AmEntry(file);
    if (!e) return;
    const std::wstring name = e->name;
    const std::wstring backup = L"Antes del primer cambio se guarda una copia del archivo (.bak).";
    if (mode == AmMode::Use) {
        AmUse(file);
    } else if (const amiibo::Facts* f = AmReady(*e)) {
        if (mode == AmMode::Register) {
            AmPage p;
            p.one = true;
            p.mode = mode;
            p.title = L"Registrar apodo y propietario";
            p.file = file;
            AmOpen(p);
        } else if (mode == AmMode::DeleteData) {
            if (!f->gameData) {
                Info(AmError("NO_DATA"));
                return;
            }
            Ask(L"¿Borrar los datos de juego de «" + name + L"»?\n\nSe borrarán los datos que el programa " + AmGame(f->appId) +
                    L" ha guardado en este amiibo. " + backup,
                {L"Cancelar", L"Borrar"}, [file](int b) {
                    if (b == 1) AmApply(file, amiibo::Op::DeleteData, {}, L"Se han borrado los datos de juego.");
                });
        } else {
            if (!f->registered && !f->gameData) {
                Info(AmError("BLANK"));
                return;
            }
            Ask(L"¿Formatear «" + name + L"»?\n\nSe borrarán el apodo, el propietario y los datos de juego guardados en este amiibo. " + backup,
                {L"Cancelar", L"Formatear"}, [file](int b) {
                    if (b == 1) AmApply(file, amiibo::Op::Format, {}, L"Se ha formateado el amiibo.");
                });
        }
    }
}

// X on an entry: its name and its place in the collection.
void AmManage(const std::wstring& file) {
    const amiibo::Entry* e = AmEntry(file);
    if (!e) return;
    const std::wstring name = e->name;
    Pick(name, {L"Cambiar el nombre", L"Quitar de la colección"}, -1, [file, name](int i) {
        if (i == 0) {
            entry.open = true;
            entry.max = 40;
            entry.title = L"Nombre del amiibo";
            entry.text = name;
            entry.done = [file](const std::wstring& s) {
                if (s.empty()) return;
                const std::string err = amiibo::Rename(file, s);
                AmRefresh();
                if (err == "NAME_TAKEN") Info(L"Ya hay un amiibo con ese nombre en la colección.");
                else if (err == "BAD_NAME") Info(L"Ese nombre no se puede usar.");
                else if (!err.empty()) Info(L"No se ha podido cambiar el nombre.");
            };
        } else if (i == 1) {
            Ask(L"¿Quitar «" + name + L"» de la colección?\n\nSe borrará el archivo de la carpeta amiibo de NSW-Home (y su copia .bak, si la hay).",
                {L"Cancelar", L"Quitar"}, [file](int b) {
                    if (b != 1) return;
                    const std::string err = amiibo::Remove(file);
                    AmRefresh();
                    if (!err.empty()) Info(L"No se ha podido borrar el archivo.");
                });
        }
    });
}

void AmAskNickname(const std::wstring& file, const std::wstring& current) {
    entry.open = true;
    entry.max = 10; // the tag holds ten characters
    entry.title = L"Apodo del amiibo";
    entry.text = current;
    entry.done = [file, current](const std::wstring& s) {
        if (s.empty() || s == current) return;
        Ask(L"¿Guardar «" + s + L"» como apodo de este amiibo?\n\nAntes del primer cambio se guarda una copia del archivo (.bak).",
            {L"Cancelar", L"Guardar"}, [file, s](int b) {
                if (b == 1) AmApply(file, amiibo::Op::Nickname, s, L"Se ha guardado el apodo.");
            });
    };
}

std::wstring AmSeries(int s) {
    wchar_t b[24];
    swprintf_s(b, L"Serie %02X", s);
    return b;
}

std::vector<Row> AmRows(const AmPage& p, std::vector<std::wstring>& files) {
    std::vector<Row> rows;
    files.clear();
    auto add = [&](Row r, const std::wstring& file = {}) {
        rows.push_back(std::move(r));
        files.push_back(file);
    };
    const bool keys = AmHaveKeys();
    if (p.one) {
        const amiibo::Entry* e = AmEntry(p.file);
        const amiibo::Facts* f = e ? AmFactsOf(*e) : nullptr;
        if (!e || !f) {
            add(PcText(L"Este amiibo ya no está en la colección o las claves ya no lo abren."));
            return rows;
        }
        const std::wstring file = e->file, nick = f->nickname;
        if (f->registered) {
            add(PcText(L"Aquí puedes cambiar el apodo de este amiibo. El propietario es un Mii, y NSW-Home no puede crear ni cambiar Mii."));
            add(ValRow(L"Apodo", nick, [file, nick] { AmAskNickname(file, nick); }));
            add(ValRow(L"Propietario", f->owner.empty() ? L"Mii sin nombre" : f->owner));
        } else {
            add(PcText(L"Este amiibo no está registrado. Para registrarlo hay que elegir un Mii como propietario, y NSW-Home no puede "
                       L"crear Mii: regístralo en la consola o en un emulador y después podrás cambiar aquí su apodo."));
        }
        add(HeadRow(L"Este amiibo"));
        add(ValRow(L"Nombre en la colección", e->name));
        add(ValRow(L"Identificación", Widen(e->id)));
        add(ValRow(L"Datos de juego", f->gameData ? L"Del programa " + AmGame(f->appId) : std::wstring(L"Ninguno")));
        if (!f->setUp.empty()) add(ValRow(L"Registrado el", lang::DayFirst(f->setUp, cfg.clock24)));
        if (!f->written.empty()) add(ValRow(L"Última escritura", lang::DayFirst(f->written, cfg.clock24)));
        add(ValRow(L"Escrituras", std::to_wstring(f->writes)));
        add(ValRow(L"Copia de antes de los cambios", e->backup ? L"Guardada (.bak)" : L"Ninguna"));
        return rows;
    }

    static const wchar_t* const ask[] = {L"Elige el amiibo cuyo apodo quieres ver o cambiar.",
                                          L"Elige el amiibo cuyos datos de juego quieres borrar.", L"Elige el amiibo que quieres formatear.",
                                          L"Elige el amiibo que quieres usar en el juego."};
    add(PcText(std::wstring(ask[static_cast<int>(p.mode)]) +
               L" En la consola se acerca la figura al mando; aquí cada amiibo es un archivo de tu colección."));
    if (p.mode != AmMode::Use && !keys) {
        add(PcText(L"Para leer y cambiar los datos de un amiibo hacen falta las claves de amiibo (key_retail.bin), que solo puedes aportar tú."));
        add(ActRow(L"Indicar el archivo de claves…", AmSetKeys));
    }
    add(ActRow(L"Añadir archivo…", AmAddFile));
    add(ActRow(L"Añadir carpeta…", AmAddFolder));
    const std::vector<amiibo::Entry>& list = AmList();
    if (list.empty()) {
        add(GapRow());
        add(PcText(L"No hay ningún amiibo en la colección."));
    }
    // Grouped by the series byte of the identification, in the order of the
    // series; by name within each.
    std::vector<int> series;
    for (const amiibo::Entry& e : list)
        if (std::find(series.begin(), series.end(), e.series) == series.end()) series.push_back(e.series);
    std::sort(series.begin(), series.end());
    const AmMode mode = p.mode;
    for (int s : series) {
        if (series.size() > 1 || list.size() > 8) {
            int n = 0;
            for (const amiibo::Entry& e : list) n += e.series == s;
            add(HeadRow(AmSeries(s) + L" · " + std::to_wstring(n)));
        }
        for (const amiibo::Entry& e : list) {
            if (e.series != s) continue;
            const std::wstring file = e.file;
            Row r = ValRow(e.name, Widen(e.id), [mode, file] { AmChoose(mode, file); });
            if (const amiibo::Facts* f = AmFactsOf(e)) {
                r.note = f->registered ? L"Apodo: " + f->nickname : std::wstring(L"Sin registrar");
                if (f->gameData) r.note += L" · Con datos de juego";
            }
            add(r, file);
        }
    }
    add(HeadRow(L"Colección"));
    if (keys) {
        Row k = ValRow(L"Claves de amiibo", keys ? L"Guardadas" : L"No indicadas", keys ? std::function<void()>(AmKeysMenu) : AmSetKeys);
        add(k);
    }
    add(PcText(L"El número de cada amiibo es su identificación (los 8 bytes en 0x54 del archivo), escrita como la muestran los "
               L"emuladores. NSW-Home no incluye ni descarga nombres de amiibo ni archivos de amiibo: solo copia a su carpeta los "
               L"que elijas, sin cambiar el original."));
    return rows;
}

// -- the running game --

std::string AmRunningEmu() {
    auto it = cfg.perGame.find(runningId);
    return it != cfg.perGame.end() && cfg.emulators.count(it->second) ? it->second : cfg.defaultEmu;
}

void AmUse(const std::wstring& file) {
    const Game* g = FindGame(runningId);
    if (runningId.empty() || !g || g->pc) {
        Info(L"No hay ningún juego abierto en un emulador.");
        return;
    }
    const std::string emu = AmRunningEmu();
    const EmuDef* def = FindEmu(emu);
    if (emu == "ryujinx") {
        Info(L"Ryujinx 1.1 no puede cargar archivos de amiibo.\n\nPueden hacerlo las versiones recientes de Ryubing y Kenji-NX y los "
             L"emuladores de la familia de yuzu, como Eden.");
        return;
    }
    if (def && !amiibo::EmulatorCanRead(EmuInfo(emu).dataDir, def->family)) {
        Info(EmuLabel(emu) + L" necesita el archivo key_retail.bin en su carpeta «system» para leer archivos de amiibo.");
        return;
    }
    // The yuzu family writes what the game saves into the file it was given.
    amiibo::Backup(file);
    const std::string err = amiibo::HandOverStart(file);
    if (err == "BUSY") return;
    if (!err.empty()) {
        Info(L"No hay ningún juego abierto en un emulador.");
        return;
    }
    amWaiting = true;
    Ask(L"Acercando el amiibo al juego…", {L"Cancelar"}, [](int) { amiibo::HandOverCancel(); });
    dlg.busy = true;
}

void AmPoll() {
    if (!amWaiting) return;
    Touch();
    std::string code;
    std::wstring said;
    if (!amiibo::HandOverDone(code, said)) return;
    amWaiting = false;
    if (dlg.open && dlg.busy) dlg.open = false;
    AmRefresh(); // the emulator may have written to the dump
    if (code == "CANCELLED") return;
    if (code.empty()) {
        // The game has it: back to the game, as after the console's own scan.
        while (stack.size() > 1 && stack.back() == Scr::Amiibo) PopNow();
        ResumeGame();
        return;
    }
    const std::string emu = AmRunningEmu();
    const EmuDef* def = FindEmu(emu);
    std::wstring msg;
    if (code == "NOT_LOOKING") {
        msg = L"El juego no está esperando un amiibo ahora.\nUsa esta opción cuando el juego te pida acercar uno.";
        if (!said.empty()) msg += L"\n\nEl emulador dice: «" + said + L"»";
    } else if (code == "NO_DIALOG" && def && def->family == 'r') {
        msg = EmuLabel(emu) + L" no ha abierto su selector de amiibo. Puede que el juego no esté esperando uno ahora, o que esta versión "
                              L"no tenga la tecla Ctrl+B para cargar archivos de amiibo (la tienen las recientes de Ryubing y Kenji-NX).";
    } else if (code == "NO_DIALOG") {
        msg = EmuLabel(emu) + L" no ha respondido.";
    } else {
        msg = L"No se ha podido indicar el archivo al emulador.";
    }
    Info(msg);
}

// A running emulator game's options: one row more.
void AmiiboUseRow(std::vector<Row>& rows) {
    const Game* g = FindGame(runningId);
    if (!amiibo::HandOverAvailable() || !g || g->pc) return;
    rows.push_back({L"Usar un amiibo", L"", [] { AmOpenCollection(AmMode::Use, L"Usar un amiibo"); }});
}

// -- the section's rows --

void AmiiboRow(Row& r, const std::string& id) {
    AmMode mode;
    if (id == "amiibo.register") mode = AmMode::Register;
    else if (id == "amiibo.delete_data") mode = AmMode::DeleteData;
    else if (id == "amiibo.reset") mode = AmMode::Format;
    else {
        r.real = id == "amiibo.intro"; // the section's own words, kept with its rows
        return;
    }
    const std::wstring title = r.label;
    r.act = [mode, title] { AmOpenCollection(mode, title); };
    r.real = true;
}

// -- the page --

void DrawAmiibo(float dt) {
    AmPoll();
    AmPage* pg = AmTop();
    if (!pg) return;
    AmPage& p = *pg;
    Theme T = Th();
    const bool s2 = S2();
    const ListLay L = PageLay();
    Header(5, p.title);
    std::vector<Row> rows = AmRows(p, amRowFile);
    const int nr = static_cast<int>(rows.size());
    if (p.row >= nr || p.row < 0 || !CanFocus(rows[p.row])) p.row = NextRow(rows, (std::min)(p.row, nr) - 1, 1);
    if (p.row < 0) p.row = NextRow(rows, -1, 1);
    float total = 0;
    std::vector<RowBox> box = LayRows(rows, L, total);
    const float view = L.bottom - L.rowY0 - 14, most = (std::max)(0.0f, total - view);
    if (p.row >= 0) {
        const RowBox& b = box[p.row];
        if (b.y - p.want < 0) p.want = b.y - (p.row > 0 && rows[p.row - 1].heading ? box[p.row - 1].h : 0);
        if (b.y + b.h - p.want > view) p.want = b.y + b.h - view;
        if (NextRow(rows, p.row, 1) < 0 && most <= b.y) p.want = most;
        if (NextRow(rows, p.row, -1) < 0 && b.y + b.h <= view) p.want = 0;
    }
    p.want = (std::max)(0.0f, (std::min)(most, p.want));
    if (s2) {
        GlideTo(p.glide, p.scroll, p.want);
    } else {
        Smooth(p.scroll, p.want, dt);
        if (p.scroll != p.want) Touch();
    }
    const float clipTop = s2 ? Px(128) : L.top + 1, clipH = s2 ? Px(968) - clipTop : L.bottom - L.top - 1;
    PushClip(L.rowX - 14, clipTop, L.rowW + 28, clipH);
    const bool cursor = !picker.open;
    const int onTop = s2 && cursor && p.row >= 0 && p.row < nr ? p.row : -1;
    for (int k = 0; k < nr + (onTop >= 0 ? 1 : 0); k++) {
        const int i = k < nr ? k : onTop;
        if (k < nr && i == onTop) continue;
        const float ry = L.rowY0 + box[i].y - p.scroll;
        if (ry > L.bottom + 4 || ry + box[i].h < L.top - 4) continue;
        DrawRow(rows[i], box[i], ry, L, T.sep, cursor && i == p.row, 0);
        if (!CanFocus(rows[i])) continue;
        const Row r = rows[i];
        AddHit(L.rowX, ry, L.rowW, box[i].fh, [i, r] {
            if (AmPage* q = AmTop()) q->row = i;
            PagePress(r);
        });
    }
    PopClip();
    if (s2 && total > L.bottom - L.rowY0) {
        const float trackTop = Px(139), track = Px(957) - trackTop;
        const float len = (std::max)(30.0f, track * (L.bottom - L.rowY0) / total);
        const float by = Snap(trackTop + (std::min)(1.0f, p.scroll / (std::max)(1.0f, most)) * (track - len));
        RRect(L.barX, by, 4, Snap(by + len) - by, 2, cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7));
    }
    std::vector<Hint> hints = {{L"B", L"Atrás", Btn::B}};
    const bool onEntry = p.row >= 0 && p.row < static_cast<int>(amRowFile.size()) && !amRowFile[p.row].empty();
    if (onEntry) hints.push_back({L"X", L"Opciones", Btn::X});
    if (p.row >= 0) hints.push_back({L"A", s2 ? L"Confirmar" : L"Aceptar", Btn::A});
    Footer(hints);
    if (picker.open) {
        hits.clear();
        DrawPicker();
    }
}

void AmiiboButton(Btn b) {
    if (picker.open) {
        PickerButton(b);
        return;
    }
    AmPage* pg = AmTop();
    if (!pg) return;
    AmPage& p = *pg;
    std::vector<std::wstring> files;
    std::vector<Row> rows = AmRows(p, files);
    const Row* cur = p.row >= 0 && p.row < static_cast<int>(rows.size()) ? &rows[p.row] : nullptr;
    switch (b) {
    case Btn::Up:
    case Btn::Down: {
        const int d = b == Btn::Up ? -1 : 1, n = NextRow(rows, p.row, d);
        if (n >= 0) {
            p.row = n;
            PlaySfx(Sfx::Move);
        } else {
            PlaySfx(Sfx::Bump);
            if (S2() && cur) BumpCursor(0, static_cast<float>(d));
        }
        break;
    }
    case Btn::L:
    case Btn::R: {
        // A long collection: to the first amiibo of the previous or next series.
        int to = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); i++) {
            if (!rows[i].heading) continue;
            const int first = NextRow(rows, i, 1);
            if (first < 0 || files[first].empty()) continue;
            if (b == Btn::R && first > p.row) {
                to = first;
                break;
            }
            if (b == Btn::L && first < p.row) to = first;
        }
        if (to >= 0) {
            p.row = to;
            PlaySfx(Sfx::Move);
        } else {
            PlaySfx(Sfx::Bump);
        }
        break;
    }
    case Btn::X:
        if (p.row >= 0 && p.row < static_cast<int>(files.size()) && !files[p.row].empty()) {
            PlaySfx(Sfx::Ok);
            AmManage(files[p.row]);
        }
        break;
    case Btn::A:
        if (cur) PagePress(*cur);
        break;
    case Btn::B:
        PlaySfx(Sfx::Back);
        Pop();
        break;
    default: break;
    }
}

// Test states. "amiibo-empty" clears the collection and the stored keys
// first; "amiibo-fill" brings in the made-up dumps and key file of
// tools\amiibo_test.cpp; "amiibo-many" adds its 300 more. Each then opens a
// page: "-register" (also the default), "-delete", "-format", "-use" (with
// the first game marked running), "-one" (the page of "Figura de prueba").
// "amiibo-handtest:<file>" runs the hand-over against stand-in windows and
// writes the report there.
bool DebugAmiibo(const std::wstring& full) {
    if (full.compare(0, 7, L"amiibo-") != 0) return false;
    amTestOn = true;
    if (full.compare(0, 16, L"amiibo-handtest:") == 0) {
        std::string report;
        const bool ok = amiibo::HandOverSelfTest(AmTestDir() + L"\\in\\Figura de prueba.bin", report);
        report += ok ? "PASSED\n" : "FAILED\n";
        FILE* f = nullptr;
        if (!_wfopen_s(&f, full.substr(16).c_str(), L"wb") && f) {
            fwrite(report.data(), 1, report.size(), f);
            fclose(f);
        }
        return true;
    }
    // The style suffixes: "-2" and "-2l" are DebugGoto's own (it strips them
    // and comes back); "-1" is the first console's style.
    auto ends = [&](const wchar_t* tail) {
        const size_t n = wcslen(tail);
        return full.size() > n && full.compare(full.size() - n, n, tail) == 0;
    };
    if (ends(L"-2") || ends(L"-2l")) return false;
    if (ends(L"-1")) {
        cfg.menuStyle = 1;
        DebugGoto(full.substr(0, full.size() - 2));
        return true;
    }
    std::wstring name = full.substr(7);
    auto take = [&](const wchar_t* word) {
        const size_t n = wcslen(word);
        if (name.compare(0, n, word) != 0) return false;
        name.erase(0, n);
        if (!name.empty() && name[0] == L'-') name.erase(0, 1);
        return true;
    };
    if (take(L"empty")) {
        for (const amiibo::Entry& e : amiibo::List()) amiibo::Remove(e.file);
        amiibo::ForgetKeys();
    } else if (take(L"fill")) {
        amiibo::SetKeys(AmTestDir() + L"\\keys\\key_retail_fake.bin");
        amiibo::ImportFile(AmTestDir() + L"\\in\\Figura de prueba.bin");
        amiibo::ImportFolder(AmTestDir() + L"\\in\\set");
    } else if (take(L"many")) {
        amiibo::SetKeys(AmTestDir() + L"\\keys\\key_retail_fake.bin");
        amiibo::ImportFolder(AmTestDir() + L"\\many");
    }
    AmRefresh();
    if (name == L"options" && !games.empty()) {
        // A running game's options, on the section with "Usar un amiibo".
        runningId = games[0].id;
        OpenOptions(games[0].id);
        optOpenAt = -10;
        options.cat = static_cast<int>(options.cats.size()) - 1;
        options.inRows = true;
        options.row = 1000; // clamped to the last row by the page
        return true;
    }
    BuildSettings();
    const int cat = CatById(settings, "amiibo");
    if (cat >= 0) {
        OpenSettings(cat);
        settings.inRows = true;
    }
    stack = {Scr::Home, Scr::Settings};
    if (name == L"delete") AmOpenCollection(AmMode::DeleteData, L"Borrar datos de juego de un amiibo");
    else if (name == L"format") AmOpenCollection(AmMode::Format, L"Formatear un amiibo");
    else if (name == L"use") {
        if (!games.empty()) runningId = games[0].id;
        stack = {Scr::Home};
        AmOpenCollection(AmMode::Use, L"Usar un amiibo");
    } else {
        AmOpenCollection(AmMode::Register, L"Registrar apodo y propietario");
    }
    if (name == L"one")
        for (const amiibo::Entry& e : AmList())
            if (e.name == L"Figura de prueba") {
                AmPage p;
                p.one = true;
                p.title = L"Registrar apodo y propietario";
                p.file = e.file;
                AmOpen(p);
            }
    return true;
}

// -- the console's rows --

// One row of console_menus as a row of the page. `all` is identical mode:
// every row is kept. Otherwise only the rows that do something here.
// See the table in the settings notes for what each console row does.
void AddConsoleRow(std::vector<Row>& out, const console::Row& cr, bool all, int& tableLine) {
    const std::string id = cr.id;
    Row r;
    r.label = Spell(cr.label);
    r.note = Spell(cr.note);
    switch (cr.kind) {
    case console::RowKind::Heading: r.heading = true; break;
    case console::RowKind::Action: r.act = Notice; break;
    case console::RowKind::Value:
        r.value = cr.value;
        r.act = Notice;
        break;
    case console::RowKind::Toggle: {
        r.kind = RK::Toggle;
        r.on = Cos(id, cr.on ? "on" : "off") == "on";
        const bool on = r.on;
        r.act = [id, on] { SetCos(id, on ? "off" : "on"); };
        break;
    }
    case console::RowKind::Slider: {
        // Knob positions as they stand in the capture.
        const bool speed = id == "accessibility.tts_speed", mic = id == "sound.mic_volume";
        r.kind = RK::Slider;
        r.icon = speed ? 2 : mic ? 3 : 1;
        r.pos = static_cast<float>(atof(Cos(id, speed ? "0.15" : mic ? "1" : "0.2").c_str()));
        const float pos = r.pos;
        r.step = [id, pos](int d) {
            float to = (std::max)(0.0f, (std::min)(1.0f, pos + d * 0.05f));
            if (to == pos) {
                PlaySfx(Sfx::Bump);
                return;
            }
            PlaySfx(Sfx::Move);
            SetCos(id, std::to_string(to));
        };
        break;
    }
    case console::RowKind::Text: r.kind = RK::Text; break;
    case console::RowKind::Picture: r.kind = RK::Panel; break;
    }

    auto emu = [&](const char* key) -> const ryucfg::Setting* {
        const ryucfg::Setting* s = EmuSetting(key);
        if (!s && !EmuReady()) EmuMissing(r);
        return s;
    };
    auto emuChoice = [&](const char* key) {
        if (const ryucfg::Setting* s = emu(key)) {
            Row made = EmuRow(*s, r.label, r.note);
            made.kind = RK::Plain; // the console shows these as a value, whatever the emulator stores
            r = made;
        }
    };

    if (PcRow(r, id)) {
        // a row with a meaning of its own on a PC
    } else if (id == "airplane.enabled") {
        // Airplane mode on = the games get no internet.
        if (const ryucfg::Setting* s = emu(EmuKey("enable_internet_access").c_str())) {
            r.on = !EmuNetOn();
            const bool on = r.on;
            r.act = [on] { EmuNetSet(on); };
            r.real = true;
            r.note = L"Con el modo avión activado, los programas no pueden conectarse a internet.";
            EmuNote(r, *s);
        } else if (!r.real) {
            r.disabled = true; // as on the console when it is docked
        }
    } else if (id == "brightness.docked_notice") {
        r.on = true; // alone on its page, centred
    } else if (id == "internet.settings") {
        if (const ryucfg::Setting* s = emu(EmuKey("enable_internet_access").c_str())) {
            const std::wstring title = r.label;
            const int cur = EmuNetOn() ? 0 : 1;
            r.act = [title, cur] {
                Pick(title, {L"Conectar a internet", L"No conectar"}, cur,
                     [](int i) { EmuNetSet(i == 0); });
            };
            r.real = true;
            EmuNote(r, *s);
        }
    } else if (id.compare(0, 9, "internet.") == 0 && cr.kind == console::RowKind::Value) {
        r.kind = RK::Table;
        r.act = nullptr;
        r.on = id == "internet.status";
        r.icon = tableLine++ % 2 == 0 && !r.on ? 1 : 0;
        if (r.on) {
            tableLine = 1;
            if (EmuSetting(EmuKey("enable_internet_access"))) {
                r.value = EmuNetOn() ? L"Conexión activada" : L"Sin conexión";
                r.real = true;
            }
            if (!all) r.kind = RK::Plain; // without its table under it: an ordinary line
        }
    } else if (id == "parental.intro_video") {
        r.icon = 9;
    } else if (id == "help.qr") {
        r.icon = 1;
        r.note = L"Proveedor de servicios en línea (solo productos de Nintendo):\nNintendo Co., Ltd. Kioto 601-8501, Japón";
    } else if (id == "data.system_storage") {
        r.kind = RK::Storage;
        r.note = L"No se detecta ninguna.";
        r.value = all ? L"Tarjeta microSD Express" : L"";
        r.real = true;
    } else if (id == "data.sd_storage" || id.compare(0, 12, "data.legend_") == 0) {
        return; // drawn by the storage row
    } else if (id == "data.manage_software") {
        r.act = [] {
            allSel = 0;
            Push(Scr::All);
        };
        r.real = true;
    } else if (id == "data.delete_saves") {
        r.act = OpenSaveFolder;
        r.real = true;
    } else if (id == "data.manage_media") {
        r.act = [] { OpenAlbum(); };
        r.real = true;
    } else if (id == "users.grid") {
        // Four tiles to a line.
        for (int u = 0; u < static_cast<int>(cfg.users.size()); u += 4) {
            Row line;
            line.kind = RK::Users;
            line.real = true;
            for (int k = u; k < u + 4 && k < static_cast<int>(cfg.users.size()); k++) line.cells.push_back(k);
            const int base = u;
            line.step = [base](int tile) { UserMenu(base + tile); };
            out.push_back(line);
        }
        return;
    } else if (id == "users.add") {
        r.act = UserAdd;
        r.real = true;
        r.disabled = cfg.users.size() >= 8;
    } else if (id == "controllers.grip_order") {
        r.act = [] { Push(Scr::Pads); };
        r.real = true;
    } else if (id == "controllers.swap_ab") {
        const std::wstring title = r.label;
        r.act = [title] {
            Pick(title, {L"No intercambiar", L"Intercambiar"}, cfg.swapAB ? 1 : 0, [](int i) {
                cfg.swapAB = i == 1;
                SaveConfig();
            });
        };
        r.real = true;
    } else if (id == "sound.current") {
        r.icon = 2;
        r.value = L"Televisor";
        r.note = L"Micrófono (consola)";
    } else if (id == "sound.output_priority") {
        emuChoice(EmuKey("audio_backend").c_str());
    } else if (id == "sound.pro_audio_mode") {
        CosPick(r, id, {L"Estable", L"Baja latencia"}); // both named in the console's own description
    } else if (id == "display.hdr_output") {
        CosPick(r, id, {L"Siempre activada", L"En programas compatibles"});
    } else if (id == "display.tv_resolution") {
        emuChoice(EmuKey("res_scale").c_str());
    } else if (id == "themes.basic_white" || id == "themes.basic_black") {
        const bool dark = id == "themes.basic_black";
        r.kind = RK::Radio;
        r.icon = dark ? 2 : 1;
        r.on = cfg.dark == dark;
        r.act = [dark] {
            cfg.dark = dark;
            SaveConfig();
        };
        r.real = true;
    } else if (id == "system.update") {
        UpdRow(r, all); // NSW-Home's own version, looked for on GitHub (see the block above)
    } else if (id == "system.nickname") {
        r.value = L"NSW-Home";
    } else if (id == "system.language") {
        LangRow(r); // NSW-Home's own language, and the games' (see the language block)
    } else if (id == "system.region") {
        emuChoice(EmuKey("system_region").c_str());
    } else if (id == "system.date_time") {
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::wstring period, clock = lang::Clock(st.wHour, st.wMinute, cfg.clock24, true, period);
        if (!period.empty()) clock = lang::PeriodFirst() ? period + L" " + clock : clock + L" " + period;
        r.note = L"Fecha y hora actuales: " + lang::Date(st.wYear, st.wMonth, st.wDay) + L" " + clock;
        if (const ryucfg::Setting* s = emu(EmuKey("system_time_zone").c_str())) {
            r.act = [] { EmuPick("system_time_zone", L"Zona horaria"); };
            r.real = true;
            EmuNote(r, *s);
        }
    } else if (id == "system.additional_info") {
        r.act = [] {
            Info(std::wstring(L"NSW-Home ") + kNSWHomeVersion + L"\nVersión de la consola: " + FirmwareText() +
                 L"\nEmulador: " + EmuLabel(cfg.defaultEmu));
        };
        r.real = true;
    } else if (id.compare(0, 7, "amiibo.") == 0) {
        AmiiboRow(r, id); // (the amiibo block above)
    } else if (id.compare(0, 12, "controllers.") == 0 || id.compare(0, 19, "accessibility.remap") == 0) {
        PadsConsoleRow(r, id); // (the order and the A/B rows are above)
    } else if (id.compare(0, 14, "accessibility.") == 0) {
        A11yRow(r, id); // (button remapping and GameChat are not among the ids it knows)
    }

    ScConsoleRow(r, id, all); // the rows with a meaning of their own on a PC (see the block above)

    if (!all && !r.real && !r.heading) return;
    if (all && GapAbove(id)) out.push_back(GapRow());
    out.push_back(r);
}

// Headings that are left with nothing under them go.
void PruneHeadings(std::vector<Row>& rows) {
    for (size_t i = rows.size(); i-- > 0;)
        if (rows[i].heading && (i + 1 == rows.size() || rows[i + 1].heading)) rows.erase(rows.begin() + i);
}

const wchar_t* EmuSectionFor(const std::string& consoleId) {
    if (consoleId == "system") return ryucfg::kConsola;
    if (consoleId == "display") return ryucfg::kPantallas;
    if (consoleId == "sound") return ryucfg::kSonido;
    if (consoleId == "internet") return ryucfg::kInternet;
    if (consoleId == "controllers") return ryucfg::kMandos;
    if (consoleId == "data") return ryucfg::kDatos;
    return nullptr;
}

std::vector<Row> ConsoleRows(const console::Section& sec) {
    std::vector<Row> rows;
    const bool all = cfg.identical;
    int tableLine = 0;
    for (const console::Row& cr : sec.rows) AddConsoleRow(rows, cr, all, tableLine);
    if (!all) {
        // Outside identical mode the emulator's other settings of this kind
        // join the section; in identical mode the page stays the console's
        // and they are listed under "NSW-Home".
        PruneHeadings(rows);
        if (const wchar_t* es = EmuSectionFor(sec.id))
            for (Row& r : EmuExtras(es)) rows.push_back(std::move(r));
    }
    return rows;
}

// NSW-Home's own section, last in the list in both modes: what the program
// needs and the console has no place for.
// -- several emulators --
// Each row of the emulator lists says where that emulator keeps its data,
// because that is what decides whether two of them share saves.

// (Just the name: the owner asked for the version number to go from these lists.)
std::wstring EmuTitle(const std::string& id) {
    return EmuLabel(id);
}

std::wstring EmuDataNote(const EmuStatus& st) {
    if (!st.found) return L"Ya no está en su carpeta";
    if (st.ownFolder) return L"Datos propios, junto al programa";
    if (!st.sharedWith.empty()) return L"Comparte datos y partidas con " + EmuLabel(st.sharedWith);
    return L"Datos en la carpeta del usuario";
}

void EmuReport(const std::string& err) {
    if (!err.empty()) Info(L"No se ha podido copiar a la carpeta del emulador.");
}

// -- PC games, and the console's firmware and keys kept outside any emulator --
// Shared by Settings, the first-start assistant and a PC game's options.

std::wstring SetupFit(const std::wstring& path, float size, float maxW);
std::wstring UserPickImage();

// Makes the program a picker returned a PC game: its name and picture are
// taken from the program itself. False when nothing was added.
bool PcAdd(const std::wstring& exe) {
    if (exe.empty()) return false;
    const std::string id = PcGameId(exe);
    for (const PcGame& g : cfg.pcGames)
        if (g.id == id) {
            Info(L"Ese programa ya está en NSW-Home.");
            return false;
        }
    if (!IdentifyEmulator(exe).empty()) {
        Info(L"Ese programa es un emulador.\nLos emuladores se añaden en su propio apartado.");
        return false;
    }
    std::wstring name;
    ProgramInfo(exe, name, PcIconFile(id));
    if (!AddPcGame(exe, name).empty()) {
        Info(L"No se encuentra ese programa.");
        return false;
    }
    return true;
}

void PcAskRemove(const std::string& id, std::function<void()> then) {
    std::wstring name;
    for (const PcGame& g : cfg.pcGames)
        if (g.id == id) name = g.name;
    if (id == runningId) {
        Info(L"Cierra el programa antes de quitarlo.");
        return;
    }
    Ask(L"¿Quitar este programa de NSW-Home?\n" + name + L"\n\nNo se borra ningún archivo.", {L"Cancelar", L"Quitar"},
        [id, then](int b) {
            if (b != 1) return;
            if (Game* g = FindGame(id)) g->icon.reset(); // lets go of the picture's file, which goes too
            RemovePcGame(id);
            if (then) then();
        });
}

// Takes the folder or file a picker returned as the user's own firmware
// (`firmware`) or keys. False when it was not that.
bool TakeOwn(bool firmware, const std::wstring& path) {
    if (path.empty()) return false;
    const std::string err = firmware ? SetOwnFirmware(path) : SetOwnKeys(path);
    if (err == "NO_FIRMWARE")
        Info(L"En esa carpeta no está el firmware de la consola.\n\nElige la carpeta que contiene sus archivos .nca.");
    else if (err == "KEYS_INVALID")
        Info(L"Ese archivo no contiene las claves de la consola.\n\nEl archivo que hace falta se llama prod.keys.");
    return err.empty();
}

// Where the firmware (or the keys) in use come from, for a row's value.
std::wstring OwnValue(bool firmware, float maxW) {
    const bool have = !(firmware ? FirmwareDir() : KeysDir()).empty();
    if (!have) return firmware ? L"No indicado" : L"No indicadas";
    if (firmware ? UsingOwnFirmware() : UsingOwnKeys()) return SetupFit(firmware ? cfg.ownFirmware : cfg.ownKeys, 21, maxW);
    return firmware ? L"El del emulador" : L"Las del emulador";
}

// A PC game's options: what is true of a program started as it is. Nothing
// about emulators, updates or saves in the cloud, in either mode.
void PcOptions() {
    auto information = [] {
        std::vector<Row> rows;
        Game* game = FindGame(optionsGame);
        if (!game) return rows;
        rows.push_back({L"Tipo", L"Programa de PC", nullptr});
        rows.push_back({L"Ubicación", SetupFit(game->file, 21, 330), nullptr});
        rows.push_back({L"Tiempo de juego", PlayTime(game->seconds), nullptr});
        rows.push_back({L"Última partida", DateOf(game->last), nullptr});
        return rows;
    };
    auto manage = [] {
        std::vector<Row> rows;
        Game* game = FindGame(optionsGame);
        if (!game) return rows;
        const std::string id = game->id;
        const std::wstring file = game->file;
        rows.push_back({L"Cambiar nombre", L"", [id] {
            Game* g = FindGame(id);
            entry.open = true;
            entry.max = 40;
            entry.title = L"Nombre del programa";
            entry.text = g ? g->name : L"";
            entry.done = [id](const std::wstring& s) {
                if (s.empty()) return;
                RenamePcGame(id, s);
                if (Game* named = FindGame(id)) named->name = s;
                if (optionsGame == id) options.title = s;
            };
        }});
        rows.push_back({L"Cambiar imagen", L"", [id] {
            const std::wstring image = UserPickImage();
            Game* g = FindGame(id);
            if (image.empty() || !g) return;
            g->icon.reset(); // the picture on show keeps its file open: let go of it first
            if (!SetPcGameImage(id, image).empty()) Info(L"No se ha podido guardar la imagen.");
            if (GetFileAttributesW(PcIconFile(id).c_str()) != INVALID_FILE_ATTRIBUTES) g->icon.path = PcIconFile(id);
            g->icon.failed = false;
        }});
        if (!OnAndroid()) rows.push_back({L"Mostrar el archivo en el Explorador", L"", [file] { RevealFile(file); }});
        rows.push_back({L"Quitar de NSW-Home", L"", [id] {
            PcAskRemove(id, [] {
                // The panel of a game that is gone has nothing to show.
                if (stack.back() == Scr::Options) {
                    if (S2()) optCloseAt = now + 1.0 / 60.0;
                    else Pop();
                }
                RescanAsync();
            });
        }});
        if (optionsGame == runningId) rows.push_back({L"Cerrar programa", L"", [] { AskClose(); }});
        return rows;
    };
    options.cats.push_back({L"Información", information});
    options.cats.push_back({L"Gestión del programa", manage});
}

// What can be done with one configured emulator.
void EmuManage(const std::string& id) {
    const EmuStatus st = EmuInfo(id);
    std::vector<std::wstring> items;
    std::vector<std::function<void()>> acts;
    auto add = [&](const std::wstring& label, std::function<void()> fn) {
        items.push_back(label);
        acts.push_back(std::move(fn));
    };
    if (cfg.defaultEmu != id && st.found)
        add(L"Usar como predeterminado", [id] {
            cfg.defaultEmu = id;
            SaveConfig();
        });
    if (!st.keys && !st.keysFrom.empty())
        add(L"Copiar las claves de " + EmuLabel(st.keysFrom), [id] { EmuReport(GiveKeys(id)); });
    if (!st.firmware && !st.firmwareFrom.empty())
        add(L"Copiar el firmware de " + EmuLabel(st.firmwareFrom), [id] { EmuReport(GiveFirmware(id)); });
    add(L"Mostrar su carpeta de datos", [st] { RevealFile(st.dataDir); });
    add(L"Quitar de NSW-Home", [id] {
        cfg.emulators.erase(id);
        if (cfg.defaultEmu == id) cfg.defaultEmu = cfg.emulators.empty() ? std::string() : cfg.emulators.begin()->first;
        for (auto it = cfg.perGame.begin(); it != cfg.perGame.end();)
            it = it->second == id ? cfg.perGame.erase(it) : std::next(it);
        SaveConfig();
    });
    Pick(EmuTitle(id) + L" · Claves: " + (st.keys ? L"sí" : L"no") + L" · Firmware: " + (st.firmware ? L"sí" : L"no"), items,
         -1, [acts](int i) {
             if (i >= 0 && i < static_cast<int>(acts.size())) acts[i]();
         });
}

std::vector<Row> NSWHomeRows() {
    std::vector<Row> rows;
    rows.push_back(TogRow(L"Modo idéntico", cfg.identical,
                          [] {
                              cfg.identical = !cfg.identical;
                              SaveConfig();
                              BuildSettings();
                              settings.cat = static_cast<int>(settings.cats.size()) - 1;
                          },
                          L"Activado: se muestran todas las secciones, opciones y aplicaciones de la consola, "
                          L"aunque aquí no hagan nada.\nDesactivado: solo se muestra lo que funciona en NSW-Home."));
    rows.push_back(ValRow(L"Estilo del menú", cfg.menuStyle == 2 ? L"Switch 2" : L"Switch", [] {
        Pick(L"Estilo del menú", {L"Switch", L"Switch 2"}, cfg.menuStyle - 1, [](int i) {
            cfg.menuStyle = i == 1 ? 2 : 1;
            SaveConfig();
        });
    }));
    rows.push_back(TogRow(L"Reloj de 24 horas", cfg.clock24, [] {
        cfg.clock24 = !cfg.clock24;
        SaveConfig();
    }));
    rows.push_back(TogRow(L"Sonidos del menú", cfg.sounds, [] {
        cfg.sounds = !cfg.sounds;
        SaveConfig();
    }));
    if (!OnAndroid()) { // [android] always full screen, on the one display: neither row
    rows.push_back(TogRow(L"Pantalla completa", cfg.fullscreen, [] {
        cfg.fullscreen = !cfg.fullscreen;
        SetFullscreen(cfg.fullscreen);
        SaveConfig();
    }));
    {
        // A list to choose from, as the console's own choices are: "Principal"
        // (the system's main screen), then each screen numbered left to right.
        auto name = [](int m) {
            const int n = MonitorCount();
            if (m < 0) return std::wstring(L"Principal");
            std::wstring s = L"Monitor " + std::to_wstring(m + 1);
            if (n >= 2 && m == 0) s += L" (izquierda)";
            else if (n >= 2 && m == n - 1) s += L" (derecha)";
            return s;
        };
        rows.push_back(ValRow(L"Monitor", name(cfg.monitor < MonitorCount() ? cfg.monitor : -1), [name] {
            std::vector<std::wstring> items;
            for (int m = -1; m < MonitorCount(); m++) items.push_back(name(m));
            const int cur = cfg.monitor < MonitorCount() ? cfg.monitor + 1 : 0;
            Pick(L"Monitor", items, cur, [](int i) {
                if (i < 0 || i - 1 == cfg.monitor) return;
                cfg.monitor = i - 1;
                ApplyMonitor();
                SaveConfig();
            });
        }));
    }
    }
    if (!OnAndroid()) // [android] an app has no window title to change
    rows.push_back(ValRow(L"Nombre en la barra de tareas", cfg.title.empty() ? L"NSW-Home" : cfg.title, [] {
        entry.open = true;
        entry.max = 32;
        entry.title = L"Nombre en la barra de tareas";
        entry.text = cfg.title;
        entry.done = [](std::wstring s) {
            while (!s.empty() && s.back() == L' ') s.pop_back();
            while (!s.empty() && s.front() == L' ') s.erase(s.begin());
            cfg.title = s; // empty: the program's own name
            SaveConfig();
            ApplyTitle();
        };
    }));
    {
        const std::wstring url = Widen(Cos("dockside.shop_url", ""));
        rows.push_back(ValRow(L"Página de la tienda", url.empty() ? L"Sin elegir" : url, [] { AskShopUrl(false); }));
    }
    rows.push_back(ActRow(L"Volver a ejecutar el asistente inicial", [] { OpenSetup(); }));

    rows.push_back(HeadRow(L"Emuladores"));
    std::function<void()> chooseEmu = [] {
        std::vector<std::wstring> names;
        std::vector<std::string> ids;
        int cur = -1;
        for (const auto& kv : cfg.emulators) {
            if (kv.first == cfg.defaultEmu) cur = static_cast<int>(ids.size());
            ids.push_back(kv.first);
            names.push_back(EmuLabel(kv.first));
        }
        Pick(L"Emulador predeterminado", names, cur, [ids](int i) {
            if (i < 0 || i >= static_cast<int>(ids.size())) return;
            cfg.defaultEmu = ids[i];
            SaveConfig();
        });
    };
    rows.push_back(ValRow(L"Emulador predeterminado", EmuLabel(cfg.defaultEmu), cfg.emulators.empty() ? nullptr : chooseEmu));
    for (const auto& e : kEmus) {
        std::string id = e.id;
        if (!cfg.emulators.count(id)) continue;
        if (OnAndroid()) { // [android] the installed emulators are listed; there is no program to choose
            rows.push_back(ValRow(e.label, L"Instalado"));
            continue;
        }
        rows.push_back(ValRow(EmuTitle(id), EmuDataNote(EmuInfo(id)), [id] { EmuManage(id); }));
    }
    if (!OnAndroid()) // [android] emulators are installed apps there, found by themselves
    rows.push_back(ActRow(L"Añadir emulador…", [] {
        std::wstring exe = PickExe();
        if (!exe.empty() && AddEmulator(exe).empty()) Info(L"Ese programa no es un emulador conocido.");
    }));

    rows.push_back(HeadRow(L"Carpetas de juegos"));
    for (size_t i = 0; i < cfg.gameDirs.size(); i++) {
        rows.push_back(ValRow(cfg.gameDirs[i], L"Quitar", [i] {
            Ask(L"¿Quitar esta carpeta de la biblioteca?\n" + cfg.gameDirs[i] + L"\n\nNo se borra ningún archivo.",
                {L"Cancelar", L"Quitar"}, [i](int b) {
                    if (b != 1 || i >= cfg.gameDirs.size()) return;
                    cfg.gameDirs.erase(cfg.gameDirs.begin() + i);
                    SaveConfig();
                    RescanAsync();
                });
        }));
    }
    rows.push_back(ActRow(L"Añadir carpeta…", [] {
        std::wstring dir = PickFolder();
        if (dir.empty()) return;
        cfg.gameDirs.push_back(dir);
        SaveConfig();
        RescanAsync();
    }));
    rows.push_back(ValRow(L"Juegos encontrados", scanning ? L"Buscando…" : std::to_wstring(games.size())));
    if (OnAndroid()) rows.push_back(ValRow(L"Claves de la consola", KeysDir().empty() ? L"No encontradas" : L"Encontradas"));
    rows.push_back(ActRow(L"Volver a buscar juegos", [] { RescanAsync(); }));

    if (!OnAndroid()) { // [android] no programs to start there, and the keys are a private copy
        rows.push_back(HeadRow(L"Programas de PC"));
        for (const PcGame& g : cfg.pcGames) {
            const std::string id = g.id;
            rows.push_back(ValRow(g.name, L"Quitar", [id] { PcAskRemove(id, [] { RescanAsync(); }); }));
        }
        rows.push_back(ActRow(L"Añadir programa de PC…", [] {
            if (PcAdd(PickExe())) RescanAsync();
        }));

        rows.push_back(HeadRow(L"Firmware y claves de la consola"));
        for (const bool firmware : {true, false}) {
            const std::wstring label = firmware ? L"Firmware de la consola" : L"Claves de la consola";
            Row r = ValRow(label, OwnValue(firmware, 300), [firmware, label] {
                std::vector<std::wstring> items = {firmware ? L"Elegir carpeta…" : L"Elegir archivo…"};
                if (!(firmware ? cfg.ownFirmware : cfg.ownKeys).empty())
                    items.push_back(firmware ? L"Olvidar la carpeta elegida" : L"Olvidar el archivo elegido");
                Pick(label, items, -1, [firmware](int i) {
                    if (i == 0 && TakeOwn(firmware, firmware ? PickFolder() : PickKeys()) && !firmware) RescanAsync();
                    if (i == 1) firmware ? SetOwnFirmware({}) : SetOwnKeys({});
                });
            });
            if (!firmware)
                r.note = L"Dan a NSW-Home los sonidos y la fuente exactos de la consola. Se leen de donde los tengas, sin "
                         L"copiarlos, cuando ningún emulador los tiene. Los cambios se ven al volver a abrir NSW-Home.";
            rows.push_back(r);
        }
    }

    // The emulator's settings with no place on the console; in identical
    // mode also the ones kept out of the console's own sections.
    if (OnAndroid()) {
        // [android] another app's settings cannot be read or changed: no such rows
    } else if (!EmuReady()) {
        rows.push_back(HeadRow(ryucfg::kEmulador));
        Row none;
        none.label = L"Ajustes del emulador";
        EmuMissing(none);
        rows.push_back(none);
    } else {
        const wchar_t* const sections[] = {ryucfg::kEmulador, ryucfg::kConsola,  ryucfg::kPantallas, ryucfg::kSonido,
                                           ryucfg::kInternet, ryucfg::kMandos,   ryucfg::kDatos};
        for (const wchar_t* es : sections) {
            if (!cfg.identical && es != ryucfg::kEmulador) continue;
            std::vector<Row> extra = EmuExtras(es);
            if (extra.empty()) continue;
            rows.push_back(HeadRow(es == ryucfg::kEmulador ? std::wstring(es) : std::wstring(L"Emulador: ") + es));
            for (Row& r : extra) rows.push_back(std::move(r));
        }
    }

    rows.push_back(HeadRow(L"Información"));
    rows.push_back(ValRow(L"Versión de NSW-Home", kNSWHomeVersion));
    return rows;
}

void BuildSettings() {
    const console::Menu& menu = console::Settings();
    settings.title = menu.title;
    settings.icon = 5;
    settings.cats.clear();
    for (const console::Section& sec : menu.sections) {
        const console::Section* s = &sec;
        // Outside identical mode a section with nothing that works here is left out.
        if (!cfg.identical && ConsoleRows(sec).empty()) continue;
        Cat c;
        c.name = sec.name;
        c.id = sec.id;
        c.rows = [s] { return ConsoleRows(*s); };
        settings.cats.push_back(c);
    }
    // Rules between groups of sections, measured on walkthrough.mp4: after
    // Ayuda, Audio por Bluetooth, Gestión de datos and Sonido.
    if (cfg.identical)
        for (Cat& c : settings.cats)
            c.ruleAfter = c.id == "help" || c.id == "bluetooth" || c.id == "data" || c.id == "sound";
    if (!settings.cats.empty()) settings.cats.back().ruleAfter = true;
    Cat own;
    own.name = L"NSW-Home";
    own.id = "nswhome";
    own.rows = [] { return NSWHomeRows(); };
    settings.cats.push_back(own);
    if (settings.cat >= static_cast<int>(settings.cats.size())) settings.cat = static_cast<int>(settings.cats.size()) - 1;
}

void OpenSettings(int cat) {
    // Changes waiting for a game to close are written now if it has, and the
    // emulator's file is read again in case it changed behind our back.
    EmuFlush();
    BuildSettings();
    const int n = static_cast<int>(settings.cats.size());
    settings.cat = cat < 0 || cat >= n ? n - 1 : cat;
    settings.row = 0;
    settings.sub = 0;
    settings.scroll = 0;
    settings.over = -1;
    settings.catScroll = -1;
    settings.inRows = false;
    picker.open = false;
    Push(Scr::Settings);
}

// -- a game's options --

void AddOptionRow(std::vector<Row>& out, const console::Row& cr) {
    const std::string id = cr.id;
    Game* g = FindGame(optionsGame);
    Row r;
    r.label = Spell(cr.label);
    r.note = Spell(cr.note);
    r.act = Notice;
    if (cr.kind == console::RowKind::Text) {
        r.kind = RK::Text;
        r.act = nullptr;
    } else if (cr.kind == console::RowKind::Value) {
        r.value = cr.value;
    }
    if (id == "info.type") {
        r.value = L"Programa de Switch"; // the emulators here run the first console's programs
        r.act = nullptr;
        r.kind = RK::Panel;
        r.icon = 3;
    } else if (id == "info.important") {
        r.act = [] {
            Game* game = FindGame(optionsGame);
            if (!game) return;
            Info(game->name + L"\nEditor: " + game->publisher + L"\nTiempo de juego: " + PlayTime(game->seconds) +
                 L"\nÚltima partida: " + DateOf(game->last) + L"\nTamaño: " + SizeOf(game->size) + L"\nId.: " +
                 Widen(game->titleId));
        };
    } else if (id == "data.transfer_saves") {
        r.act = OpenSaveFolder;
        r.note = OnAndroid() ? L"Los datos de guardado se gestionan en el emulador." // [android]
                             : L"Abre la carpeta donde el emulador guarda los datos de guardado.";
    } else if (id == "cloud.user_not_backed_up") {
        // One row per user, as on the console; nothing here is saved to a cloud.
        for (int u = 0; u < static_cast<int>(cfg.users.size()); u++) {
            Row line;
            line.label = UserLabel(u);
            const bool played = g && UserPlayed(u, *g).seconds > 0;
            line.note = played ? L"Guardado rápido no efectuado" : L"Sin datos de guardado";
            line.disabled = !played;
            out.push_back(line);
        }
        return;
    } else if (id.compare(0, 11, "cloud.user_") == 0) {
        return;
    }
    if (GapAbove(id)) out.push_back(GapRow());
    out.push_back(r);
}

// ---- a game's updates, downloadable content, mods and cheats ---------------
// "Datos de actualización", "Gestionar contenido descargable", "Mods" and
// "Trucos" in a game's options, for the emulator the game starts with (the
// files and their formats are content.h's). What the user chooses is kept in
// cfg.console:
//   content.<title id>.update                    "base" or the update's version number
//   content.<title id>.dlc.<dlc id>              "on" / "off"
//   content.<emulator>.<title id>.mod.<key>      "on" / "off" (Ryujinx family: the
//                                                mod's folder below the data folder;
//                                                yuzu family: the folder's name)
//   content.<emulator>.<title id>.cheat.<key>    "on" / "off" (Ryujinx family)
// and written into the emulator's own files at once when no game is open,
// and again just before every start (FinishLaunch): a choice made while a
// game runs, or something changed behind NSW-Home's back, is put right
// before the game reads it. A game with no choice made keeps whatever its
// emulator has. The updates and DLC on offer are the dumps in the folders of
// games and in the ones the emulators were told of; they are looked for on a
// thread of their own, as the library is.

struct CtUpd {
    std::wstring file, text;
    uint32_t version = 0;
};
struct CtDlc {
    std::wstring file, name;
    std::string nca;
    uint64_t id = 0, ncaTitle = 0;
    bool on = false;
};
struct CtModel {
    std::string game, emu;
    uint64_t tid = 0;
    char family = 'r';
    bool have = false;     // the emulator's data folder is there
    std::wstring dir, ini; // that folder (a test run's copy) and, yuzu family, its qt-config.ini
    std::vector<CtUpd> upd; // newest first
    int sel = -1;          // the update in use: an index of `upd`; -1 the game without
    std::vector<CtDlc> dlc;
    bool dlcAll = true;    // yuzu family: the one switch for all of them
    std::vector<content::Mod> mods;
    std::vector<content::Cheat> cheats;
    std::vector<std::string> disabled;
};
CtModel ct;
std::vector<content::Dump> ctDumps; // what the folders hold
double ctDumpsAt = -1000;           // when that was looked for (`now`); -1000 never
bool ctBuilding = false;
std::atomic<std::vector<content::Dump>*> ctFresh{nullptr};

const EmuDef* CtEmu(const std::string& gameId) {
    // The same choice a start makes (main.cpp, GameCommand).
    std::string id = cfg.defaultEmu;
    auto per = cfg.perGame.find(gameId);
    if (per != cfg.perGame.end() && cfg.emulators.count(per->second)) id = per->second;
    if (!cfg.emulators.count(id)) {
        if (cfg.emulators.empty()) return nullptr;
        id = cfg.emulators.begin()->first;
    }
    return FindEmu(id);
}

bool CtExtDirs(const std::string& emu) {
    return emu == "eden" || emu == "suyu"; // the two whose source has external_content_dirs
}

std::wstring CtIni(const EmuDef& e) {
    return EmuFilesFor(e.id) + L"\\config\\qt-config.ini";
}

// What to look through: the folders of games, the ones the emulators were
// told of, and every file the Ryujinx family lists.
void CtSources(std::vector<std::wstring>& dirs, std::vector<std::wstring>& files) {
    dirs = cfg.gameDirs;
    for (const auto& kv : cfg.emulators) {
        const EmuDef* e = FindEmu(kv.first);
        if (!e) continue;
        if (e->family == 'r') {
            for (const std::wstring& d : content::ryu::AutoloadDirs(EmuFilesFor(e->id))) dirs.push_back(d);
            for (const std::wstring& f : content::ryu::ListedFiles(EmuFilesFor(e->id))) files.push_back(f);
        } else if (CtExtDirs(e->id)) {
            for (const std::wstring& d : content::yuzu::Read(CtIni(*e), 0).extDirs) dirs.push_back(d);
        }
    }
}

void CtLoad(const std::string& gameId);

// Looks again (on a thread, or here when `wait`); the answer is taken by CtPoll.
void CtSearch(bool wait) {
    if (ctBuilding) return;
    content::SetCache(DataDir() + L"\\content.json");
    std::vector<std::wstring> dirs, files;
    CtSources(dirs, files);
    const std::wstring keys = KeysDir();
    ctDumpsAt = now;
    if (wait) {
        ctDumps = content::Collect(dirs, 2, keys, files);
        content::SaveCache();
        return;
    }
    ctBuilding = true;
    std::thread([dirs, files, keys] {
        auto* found = new std::vector<content::Dump>(content::Collect(dirs, 2, keys, files));
        content::SaveCache();
        delete ctFresh.exchange(found);
    }).detach();
}

// Takes what the thread found; true when there was something new.
bool CtPoll() {
    std::vector<content::Dump>* found = ctFresh.exchange(nullptr);
    if (!found) return false;
    ctDumps = std::move(*found);
    delete found;
    ctBuilding = false;
    if (!ct.game.empty()) CtLoad(ct.game);
    Touch(); // the rows change: draw them
    return true;
}

std::string CtKey(const CtModel& m, const char* what, const std::string& key) {
    return "content." + m.emu + "." + content::Hex16(m.tid) + "." + what + "." + key;
}

std::string CtModKey(const CtModel& m, const content::Mod& mod) {
    if (m.family != 'r') return content::yuzu::AddOnName(mod.name);
    std::wstring rel = mod.path;
    if (rel.size() > m.dir.size() && _wcsnicmp(rel.c_str(), m.dir.c_str(), m.dir.size()) == 0) rel = rel.substr(m.dir.size() + 1);
    return Narrow(rel);
}

// The choice kept for this game; "" when none was made.
std::string CtWant(const std::string& key) {
    return Cos(key, "");
}

CtModel CtBuild(const std::string& gameId) {
    CtModel m;
    m.game = gameId;
    Game* g = FindGame(gameId);
    const EmuDef* e = CtEmu(gameId);
    if (!g || g->pc || !e || !content::ParseHex16(g->titleId, m.tid)) return m;
    m.emu = e->id;
    m.family = e->family;
    m.dir = EmuFilesFor(e->id);
    m.have = emudata::IsDir(m.dir);
    if (m.family != 'r') m.ini = CtIni(*e);
    // What the folders hold for this game.
    for (const content::Dump& d : ctDumps)
        for (const content::Meta& x : d.metas) {
            if (x.app != m.tid) continue;
            if (x.type == content::kUpdate) {
                m.upd.push_back({d.file, content::VersionText(x), x.version});
            } else if (x.type == content::kDlc) {
                CtDlc c;
                c.file = d.file;
                c.name = content::DlcName(d.file);
                c.nca = x.nca;
                c.id = x.id;
                c.ncaTitle = x.ncaTitle ? x.ncaTitle : x.id;
                m.dlc.push_back(c);
            }
        }
    std::stable_sort(m.upd.begin(), m.upd.end(), [](const CtUpd& a, const CtUpd& b) { return a.version > b.version; });
    // One file reached from two places (a folder of games that an emulator also lists) counts once.
    auto same = [](std::wstring a, std::wstring b) {
        std::replace(a.begin(), a.end(), L'/', L'\\');
        std::replace(b.begin(), b.end(), L'/', L'\\');
        return !_wcsicmp(a.c_str(), b.c_str());
    };
    for (size_t i = 0; i < m.upd.size(); i++)
        for (size_t j = m.upd.size(); j-- > i + 1;)
            if (same(m.upd[i].file, m.upd[j].file)) m.upd.erase(m.upd.begin() + static_cast<long>(j));
    for (size_t i = 0; i < m.dlc.size(); i++)
        for (size_t j = m.dlc.size(); j-- > i + 1;)
            if (m.dlc[i].id == m.dlc[j].id && same(m.dlc[i].file, m.dlc[j].file)) m.dlc.erase(m.dlc.begin() + static_cast<long>(j));
    if (!m.have) return m;
    // What the emulator will use.
    if (m.family == 'r') {
        const content::ryu::State st = content::ryu::Read(m.dir, m.tid);
        if (!st.selected.empty()) {
            for (size_t i = 0; i < m.upd.size() && m.sel < 0; i++)
                if (!_wcsicmp(m.upd[i].file.c_str(), st.selected.c_str())) m.sel = static_cast<int>(i);
            if (m.sel < 0) {
                // A file the emulator lists that could not be read here.
                const size_t s = st.selected.find_last_of(L"\\/");
                m.upd.push_back({st.selected, st.selected.substr(s == std::wstring::npos ? 0 : s + 1), 0});
                m.sel = static_cast<int>(m.upd.size()) - 1;
            }
        }
        for (const auto& d : st.dlc) {
            auto it = std::find_if(m.dlc.begin(), m.dlc.end(),
                                   [&](const CtDlc& c) { return c.ncaTitle == d.id && !_wcsicmp(c.file.c_str(), d.file.c_str()); });
            if (it == m.dlc.end()) {
                CtDlc c;
                c.file = d.file;
                c.name = content::DlcName(d.file);
                c.nca = d.nca;
                c.id = c.ncaTitle = d.id;
                m.dlc.push_back(c);
                it = m.dlc.end() - 1;
            }
            it->on = d.on;
        }
        m.mods = content::ryu::Mods(m.dir, m.tid);
        m.cheats = content::ryu::Cheats(m.dir, m.tid);
    } else {
        const content::yuzu::State st = content::yuzu::Read(m.ini, m.tid);
        m.disabled = st.disabled;
        auto off = [&](const std::string& n) { return std::find(m.disabled.begin(), m.disabled.end(), n) != m.disabled.end(); };
        if (CtExtDirs(m.emu)) {
            // The newest one not switched off (Eden: patch_manager.cpp, the
            // external provider's versions newest first).
            for (size_t i = 0; i < m.upd.size() && m.sel < 0; i++)
                if (!off("Update") && !off("Update@" + std::to_string(m.upd[i].version))) m.sel = static_cast<int>(i);
        } else if (!m.upd.empty() && !off("Update")) {
            m.sel = 0; // Citron: one switch; which file it takes is its own choice
        }
        m.dlcAll = !off("DLC");
        for (CtDlc& c : m.dlc) c.on = m.dlcAll;
        for (const content::Mod& x : content::yuzu::Mods(m.dir, m.tid, m.disabled))
            if (!x.cheatsOnly) m.mods.push_back(x);
        m.cheats = content::yuzu::Cheats(m.dir, m.tid, m.disabled, CtExtDirs(m.emu));
    }
    // A choice waiting to be written (a game was open) shows as made.
    const std::string up = CtWant("content." + content::Hex16(m.tid) + ".update");
    if (up == "base") m.sel = -1;
    else if (!up.empty())
        for (size_t i = 0; i < m.upd.size(); i++)
            if (std::to_string(m.upd[i].version) == up) {
                m.sel = static_cast<int>(i);
                break;
            }
    for (CtDlc& c : m.dlc) {
        const std::string w = CtWant("content." + content::Hex16(m.tid) + ".dlc." + content::Hex16(c.id));
        if (!w.empty()) c.on = w == "on";
    }
    if (m.family != 'r' && !m.dlc.empty()) m.dlcAll = std::any_of(m.dlc.begin(), m.dlc.end(), [](const CtDlc& c) { return c.on; });
    for (content::Mod& x : m.mods) {
        const std::string w = CtWant(CtKey(m, "mod", CtModKey(m, x)));
        if (!w.empty()) x.on = w == "on";
    }
    for (content::Cheat& c : m.cheats) {
        const std::string w = m.family == 'r' ? CtWant(CtKey(m, "cheat", c.key)) : CtWant(CtKey(m, "mod", c.key));
        if (!w.empty()) c.on = w == "on";
    }
    // Names that come from the files (a mod's folder, a cheat's section, a DLC's): data, not wording.
    for (const CtDlc& c : m.dlc) lang::Ignore(c.name);
    for (const content::Mod& x : m.mods) lang::Ignore(x.name);
    for (const content::Cheat& c : m.cheats) lang::Ignore(c.name);
    return m;
}

void CtLoad(const std::string& gameId) {
    ct = CtBuild(gameId);
}

// The folders Eden / Suyu must be told of so that every choice made for any
// game can be loaded.
std::vector<std::wstring> CtNeededDirs() {
    std::vector<std::wstring> out;
    auto folder = [&out](const std::wstring& f) {
        const std::wstring d = f.substr(0, f.find_last_of(L"\\/"));
        if (std::none_of(out.begin(), out.end(), [&](const std::wstring& o) { return !_wcsicmp(o.c_str(), d.c_str()); })) out.push_back(d);
    };
    for (const auto& kv : cfg.console) {
        const std::string& k = kv.first;
        if (k.compare(0, 8, "content.") != 0 || k.size() < 25 || k[24] != '.') continue;
        uint64_t tid = 0;
        if (!content::ParseHex16(k.substr(8, 16), tid)) continue;
        const std::string rest = k.substr(25);
        for (const content::Dump& d : ctDumps)
            for (const content::Meta& x : d.metas) {
                if (x.app != tid) continue;
                if (rest == "update" && x.type == content::kUpdate && std::to_string(x.version) == kv.second) folder(d.file);
                if (rest == "dlc." + content::Hex16(x.id) && x.type == content::kDlc && kv.second == "on") folder(d.file);
            }
    }
    return out;
}

// Writes the choices kept for this game into its emulator's files. Only
// ever called while no game is open. False when a file could not be changed.
bool CtWrite(const std::string& gameId) {
    if (OnAndroid()) return true; // [android] another app's files are out of reach
    CtModel m = CtBuild(gameId);
    if (m.emu.empty() || !m.have) return true;
    const std::string base = "content." + content::Hex16(m.tid) + ".";
    const std::string up = CtWant(base + "update");
    bool ok = true;
    if (m.family == 'r') {
        if (up == "base") ok = content::ryu::SelectUpdate(m.dir, m.tid, L"") && ok;
        else if (!up.empty())
            for (const CtUpd& u : m.upd)
                if (std::to_string(u.version) == up) {
                    ok = content::ryu::SelectUpdate(m.dir, m.tid, u.file) && ok;
                    break;
                }
        for (const CtDlc& c : m.dlc) {
            const std::string w = CtWant(base + "dlc." + content::Hex16(c.id));
            if (!w.empty()) ok = content::ryu::SetDlc(m.dir, m.tid, c.file, c.nca, c.ncaTitle, w == "on") && ok;
        }
        for (const content::Mod& x : m.mods) {
            const std::string w = CtWant(CtKey(m, "mod", CtModKey(m, x)));
            if (!w.empty()) ok = content::ryu::SetMod(m.dir, m.tid, x, w == "on") && ok;
        }
        for (const content::Cheat& c : m.cheats) {
            const std::string w = CtWant(CtKey(m, "cheat", c.key));
            if (!w.empty()) ok = content::ryu::SetCheat(m.dir, m.tid, c.key, w == "on") && ok;
        }
    } else {
        auto set = [&](const std::string& name, bool disabled) { ok = content::yuzu::SetDisabled(m.ini, m.tid, name, disabled) && ok; };
        // A version whose file is gone is left to the emulator.
        const bool known = up == "base" || std::any_of(m.upd.begin(), m.upd.end(), [&](const CtUpd& u) { return std::to_string(u.version) == up; });
        if (!up.empty() && known) {
            if (CtExtDirs(m.emu)) {
                // One version on, the others off, as Eden's own list does.
                for (const CtUpd& u : m.upd) set("Update@" + std::to_string(u.version), up == "base" || std::to_string(u.version) != up);
                set("Update", false);
            } else {
                set("Update", up == "base");
            }
        }
        bool anyWant = false, anyOn = false;
        for (const CtDlc& c : m.dlc) {
            const std::string w = CtWant(base + "dlc." + content::Hex16(c.id));
            anyWant = anyWant || !w.empty();
            anyOn = anyOn || w == "on";
        }
        if (anyWant) set("DLC", !anyOn);
        for (const content::Mod& x : content::yuzu::Mods(m.dir, m.tid, m.disabled)) {
            const std::string w = CtWant(CtKey(m, "mod", CtModKey(m, x)));
            if (!w.empty()) set(content::yuzu::AddOnName(x.name), w == "off");
        }
        if (CtExtDirs(m.emu)) {
            const std::vector<std::wstring> need = CtNeededDirs();
            const std::vector<std::wstring> have = content::yuzu::Read(m.ini, m.tid).extDirs;
            for (const std::wstring& d : need) {
                // A folder already listed (or one above it: the emulator looks
                // through every folder below) needs nothing more.
                const bool listed = std::any_of(have.begin(), have.end(), [&](const std::wstring& h) {
                    std::wstring a = h, b = d;
                    std::replace(a.begin(), a.end(), L'/', L'\\');
                    return b.size() >= a.size() && !_wcsnicmp(a.c_str(), b.c_str(), a.size()) &&
                           (b.size() == a.size() || b[a.size()] == L'\\');
                });
                if (!listed) ok = content::yuzu::SetExtDir(m.ini, d, true) && ok;
            }
            for (const std::wstring& d : content::yuzu::ExtDirsMade(m.ini)) {
                std::wstring w = d;
                std::replace(w.begin(), w.end(), L'/', L'\\');
                const bool needed = std::any_of(need.begin(), need.end(), [&](const std::wstring& n) { return !_wcsicmp(n.c_str(), w.c_str()); });
                if (!needed) ok = content::yuzu::SetExtDir(m.ini, w, false) && ok;
            }
        }
    }
    return ok;
}

// Just before a game starts: its emulator is not running yet.
void CtBeforeLaunch(const std::string& gameId) {
    Game* g = FindGame(gameId);
    if (!g || g->pc || g->titleId.empty()) return;
    const std::string base = "content." + g->titleId + ".";
    bool any = false;
    for (const auto& kv : cfg.console) any = any || kv.first.compare(0, base.size(), base) == 0 || kv.first.find("." + g->titleId + ".") != std::string::npos;
    if (!any) return;
    CtPoll();
    // A version kept by number needs the files: look for them now if that never happened.
    if (ctDumps.empty() && !ctBuilding) CtSearch(true);
    CtWrite(gameId);
}

// A choice from one of the pages: kept, and written now unless a game is open.
void CtChoose(const std::string& key, const std::string& value) {
    SetCos(key, value);
    if (runningId.empty() && !CtWrite(ct.game)) Info(L"No se ha podido cambiar el archivo del emulador.");
    CtLoad(ct.game);
}

std::wstring CtEmuName() {
    return EmuLabel(ct.emu);
}

// The note under a page's first line: whose files these are, and when a
// change made now takes effect.
std::wstring CtWhen() {
    std::wstring s = L"Lo que se elige aquí se aplica a " + CtEmuName() + L".";
    if (!runningId.empty()) s += L" Los cambios se aplicarán la próxima vez que se inicie el programa.";
    return s;
}

std::wstring CtFileName(const std::wstring& f) {
    const size_t s = f.find_last_of(L"\\/");
    return s == std::wstring::npos ? f : f.substr(s + 1);
}

std::wstring CtCount(int on, int n) {
    if (n == 0) return L"Ninguno";
    return std::to_wstring(on) + L" de " + std::to_wstring(n);
}

std::wstring CtKinds(const std::wstring& kinds) {
    std::wstring out;
    size_t at = 0;
    while (at < kinds.size()) {
        size_t c = kinds.find(L", ", at);
        const std::wstring k = kinds.substr(at, c == std::wstring::npos ? std::wstring::npos : c - at);
        const wchar_t* t = k == L"romfs" || k == L"romfslite" || k == L"romfs_ext" ? L"archivos del juego"
                           : k == L"exefs" ? L"código"
                           : k == L"cheats" ? L"trucos"
                           : k.c_str();
        if (out.find(t) == std::wstring::npos) out += (out.empty() ? L"" : L", ") + std::wstring(t);
        at = c == std::wstring::npos ? kinds.size() : c + 2;
    }
    if (!out.empty()) out[0] = static_cast<wchar_t>(towupper(out[0]));
    return out;
}

void CtOpenFolder(const std::wstring& dir) {
    if (!ct.have) return;
    emudata::MakeDir(dir); // the emulator makes it too, the first time it looks for mods
    if (!OpenFolder(dir)) Info(L"No se ha podido abrir la carpeta.");
}

std::wstring CtModsDir() {
    return ct.family == 'r' ? content::ryu::ModsDir(ct.dir, ct.tid) : content::yuzu::ModsDir(ct.dir, ct.tid);
}

// -- the pages --

std::vector<Row> CtUpdateRows() {
    CtPoll();
    if (ctBuilding) Touch(); // keep drawing until the search is in
    std::vector<Row> rows;
    Game* g = FindGame(ct.game);
    const std::string key = "content." + content::Hex16(ct.tid) + ".update";
    if (ct.family != 'r' && !CtExtDirs(ct.emu)) {
        // Citron reads one switch for every update (patch_manager.cpp, "Update").
        Row r = TogRow(L"Usar los datos de actualización", ct.sel >= 0,
                       [key] { CtChoose(key, ct.sel >= 0 ? "base" : std::to_string(ct.upd.empty() ? 0 : ct.upd[0].version)); },
                       CtWhen() + L" " + CtEmuName() + L" no deja elegir la versión: usa la que encuentra en sus carpetas de juegos o la "
                                                       L"instalada en su memoria.");
        r.disabled = ct.upd.empty();
        rows.push_back(r);
        if (!ct.upd.empty()) rows.push_back(HeadRow(L"Encontrados"));
        for (const CtUpd& u : ct.upd) {
            Row v = ValRow(L"Versión " + u.text, L"");
            v.act = nullptr;
            v.note = CtFileName(u.file);
            rows.push_back(v);
        }
    } else {
        Row none = ValRow(L"Sin actualizar", Check(ct.sel < 0), [key] { CtChoose(key, "base"); });
        none.note = (g && !g->version.empty() ? L"Versión " + g->version + L" del programa. " : L"") + CtWhen();
        rows.push_back(none);
        for (size_t i = 0; i < ct.upd.size(); i++) {
            const CtUpd u = ct.upd[i];
            Row r = ValRow(L"Versión " + u.text, Check(ct.sel == static_cast<int>(i)),
                           u.version ? std::function<void()>([key, u] { CtChoose(key, std::to_string(u.version)); }) : nullptr);
            r.note = CtFileName(u.file);
            if (!u.version) r.note += L"\nNo se ha podido leer este archivo.";
            rows.push_back(r);
        }
    }
    if (ct.upd.empty())
        rows.push_back(PcText(ctBuilding ? L"Buscando…"
                                         : L"No hay datos de actualización de este programa en las carpetas de juegos."));
    if (!OnAndroid())
        rows.push_back(ActRow(L"Añadir una carpeta de juegos…", [] {
            const std::wstring dir = PcPickFolder();
            if (dir.empty()) return;
            cfg.gameDirs.push_back(dir);
            SaveConfig();
            RescanAsync();
            ctDumpsAt = -1000; // looked through again at the latest when the options next open
            CtSearch(pcTest);
        }, L"Las actualizaciones y el contenido descargable se buscan en las mismas carpetas que los juegos."));
    return rows;
}

std::vector<Row> CtDlcRows() {
    CtPoll();
    if (ctBuilding) Touch();
    std::vector<Row> rows;
    const std::string base = "content." + content::Hex16(ct.tid) + ".dlc.";
    if (ct.family != 'r') {
        // The yuzu family has one switch for all of a game's DLC ("DLC").
        Row all = TogRow(L"Usar el contenido descargable", ct.dlcAll, [base] {
            const bool on = !ct.dlcAll;
            for (const CtDlc& c : ct.dlc) SetCos(base + content::Hex16(c.id), on ? "on" : "off");
            CtChoose(base + content::Hex16(ct.dlc.empty() ? 0 : ct.dlc[0].id), on ? "on" : "off");
        }, CtWhen() + L" " + CtEmuName() + L" lo activa o desactiva todo a la vez.");
        all.disabled = ct.dlc.empty();
        rows.push_back(all);
        if (!ct.dlc.empty()) rows.push_back(HeadRow(L"Contenido descargable"));
        for (const CtDlc& c : ct.dlc) {
            Row v = ValRow(c.name, L"");
            v.act = nullptr;
            v.note = CtFileName(c.file);
            rows.push_back(v);
        }
    } else {
        for (size_t i = 0; i < ct.dlc.size(); i++) {
            const CtDlc c = ct.dlc[i];
            Row r = TogRow(c.name, c.on, [base, c] { CtChoose(base + content::Hex16(c.id), c.on ? "off" : "on"); });
            r.note = CtFileName(c.file);
            // Ryujinx opens a DLC file by its extension, in small letters only (ContentManager.cs).
            const size_t dot = c.file.find_last_of(L'.');
            if (dot != std::wstring::npos && c.file.compare(dot, std::wstring::npos, L".nsp") != 0 &&
                c.file.compare(dot, std::wstring::npos, L".xci") != 0)
                r.note += L"\n" + CtEmuName() + L" no abre este archivo: la extensión tiene que estar en minúsculas.";
            if (i == 0) r.note += L"\n" + CtWhen();
            rows.push_back(r);
        }
    }
    if (ct.dlc.empty())
        rows.push_back(PcText(ctBuilding ? L"Buscando…"
                                         : L"No hay contenido descargable de este programa en las carpetas de juegos."));
    return rows;
}

std::vector<Row> CtModRows() {
    std::vector<Row> rows;
    for (size_t i = 0; i < ct.mods.size(); i++) {
        const content::Mod x = ct.mods[i];
        const std::string key = CtKey(ct, "mod", CtModKey(ct, x));
        Row r = TogRow(x.name, x.on, [key, x] { CtChoose(key, x.on ? "off" : "on"); });
        r.note = CtKinds(x.kinds);
        if (i == 0) r.note += L"\n" + CtWhen();
        rows.push_back(r);
    }
    if (ct.mods.empty())
        rows.push_back(PcText(L"Este programa no tiene mods en " + CtEmuName() +
                              L".\nCada mod va en una carpeta propia dentro de la carpeta de mods."));
    if (!OnAndroid()) rows.push_back(ActRow(L"Abrir la carpeta de mods", [] { CtOpenFolder(CtModsDir()); }));
    return rows;
}

std::vector<Row> CtCheatRows() {
    std::vector<Row> rows;
    if (ct.family == 'r') {
        for (size_t i = 0; i < ct.cheats.size(); i++) {
            const content::Cheat c = ct.cheats[i];
            const std::string key = CtKey(ct, "cheat", c.key);
            Row r = TogRow(c.name, c.on, [key, c] { CtChoose(key, c.on ? "off" : "on"); });
            r.note = CtFileName(c.file);
            if (i == 0) r.note += L"\n" + CtWhen();
            rows.push_back(r);
        }
    } else {
        // The yuzu family switches a folder's cheats with the folder itself.
        std::vector<std::string> done;
        for (const content::Cheat& c : ct.cheats) {
            if (std::find(done.begin(), done.end(), c.key) != done.end()) continue;
            done.push_back(c.key);
            std::wstring names;
            for (const content::Cheat& o : ct.cheats)
                if (o.key == c.key) names += (names.empty() ? L"" : L", ") + o.name;
            bool alsoMod = false;
            for (const content::Mod& x : ct.mods) alsoMod = alsoMod || content::yuzu::AddOnName(x.name) == c.key;
            const std::string key = CtKey(ct, "mod", c.key);
            Row r = TogRow(Widen(c.key), c.on, [key, c] { CtChoose(key, c.on ? "off" : "on"); });
            r.note = names + (alsoMod ? L"\nEsta carpeta también lleva un mod: se activan o desactivan juntos." : L"");
            if (done.size() == 1) r.note += L"\n" + CtWhen();
            rows.push_back(r);
        }
    }
    if (ct.cheats.empty())
        rows.push_back(PcText(L"Este programa no tiene trucos en " + CtEmuName() + L"."));
    if (!OnAndroid())
        rows.push_back(ActRow(L"Abrir la carpeta de trucos", [] {
            CtOpenFolder(ct.family == 'r' ? CtModsDir() + L"\\cheats" : CtModsDir());
        }));
    return rows;
}

// The four rows in the game's options ("Gestión de datos").
std::vector<Row> CtRows() {
    std::vector<Row> rows;
    Game* g = FindGame(optionsGame);
    if (OnAndroid() || !g || g->pc || g->titleId.empty()) return rows; // [android] the emulator's files are its own
    CtPoll();
    if (ctBuilding) Touch();
    if (ct.game != optionsGame) CtLoad(optionsGame);
    if (ct.emu.empty()) return rows;
    const bool busy = ctBuilding && ctDumps.empty();
    auto open = [](const wchar_t* title, std::vector<Row> (*page)()) {
        return [title, page] {
            if (!ct.have) {
                Info(L"No se encuentra la carpeta de datos de " + CtEmuName() + L".\nInicia un programa con " + CtEmuName() +
                     L" una vez para que la cree.");
                return;
            }
            CtLoad(ct.game);
            OpenPage(title, page);
        };
    };
    int dOn = 0, mOn = 0, cOn = 0;
    for (const CtDlc& c : ct.dlc) dOn += c.on ? 1 : 0;
    for (const content::Mod& x : ct.mods) mOn += x.on ? 1 : 0;
    for (const content::Cheat& c : ct.cheats) cOn += c.on ? 1 : 0;
    Row updRow = ValRow(L"Datos de actualización",
                     busy ? L"Buscando…" : ct.sel >= 0 ? L"Versión " + ct.upd[static_cast<size_t>(ct.sel)].text : L"Sin actualizar",
                     open(L"Datos de actualización", CtUpdateRows));
    updRow.note = L"Las actualizaciones, el contenido descargable, los mods y los trucos que usará " + CtEmuName() + L".";
    rows.push_back(updRow);
    rows.push_back(ValRow(L"Gestionar contenido descargable",
                          busy ? L"Buscando…" : ct.family != 'r' && !ct.dlc.empty() ? (ct.dlcAll ? L"Activado" : L"Desactivado")
                                                                                   : CtCount(dOn, static_cast<int>(ct.dlc.size())),
                          open(L"Gestionar contenido descargable", CtDlcRows)));
    rows.push_back(ValRow(L"Mods", CtCount(mOn, static_cast<int>(ct.mods.size())), open(L"Mods", CtModRows)));
    rows.push_back(ValRow(L"Trucos", CtCount(cOn, static_cast<int>(ct.cheats.size())), open(L"Trucos", CtCheatRows)));
    return rows;
}

// When the options of a game open: what its emulator has now, and the folders
// looked through again when that was long ago.
void CtOptionsOpened(const std::string& gameId) {
    Game* g = FindGame(gameId);
    if (OnAndroid() || !g || g->pc) return;
    CtPoll();
    if (now - ctDumpsAt > 30) CtSearch(pcTest); // a test run waits for it: its picture is taken at once
    CtLoad(gameId);
}

// Test states: the rows and pages, on the first game of the library that has
// any of them; "dumpoptions:<file>" writes every row of that game's options
// and of the four pages (as dumpsettings does for Settings).
bool DebugContent(const std::wstring& full) {
    if (full.rfind(L"content", 0) != 0 && full.rfind(L"dumpoptions", 0) != 0) return false;
    auto ends = [&full](const wchar_t* s) {
        const size_t k = wcslen(s);
        return full.size() > k && full.compare(full.size() - k, k, s) == 0;
    };
    if (full.rfind(L"content", 0) == 0 && (ends(L"-2") || ends(L"-2l"))) return false; // DebugGoto sets the style and comes back
    // "dumpoptions[+...]:<file>": the options before the colon, the file after it.
    std::wstring name = full, file;
    if (full.rfind(L"dumpoptions", 0) == 0) {
        const size_t colon = full.find(L':');
        file = full.substr(colon + 1);
        name = full.substr(0, colon);
    }
    if (full.rfind(L"content", 0) == 0 && (ends(L"-1") || ends(L"-1l"))) {
        cfg.menuStyle = 1;
        cfg.dark = ends(L"-1");
        cfg.clock24 = true;
        name = full.substr(0, full.size() - (ends(L"-1") ? 2 : 3));
    }
    // "+<title id>" picks the game; "+<emulator>" the emulator it starts with.
    std::string pickTid, pickEmu;
    for (size_t plus = name.find(L'+'); plus != std::wstring::npos;) {
        const size_t end = name.find(L'+', plus + 1);
        const std::string opt = Narrow(name.substr(plus + 1, end == std::wstring::npos ? std::wstring::npos : end - plus - 1));
        if (opt.size() == 16) pickTid = opt;
        else pickEmu = opt;
        name.erase(plus, end == std::wstring::npos ? std::wstring::npos : end - plus);
        plus = name.find(L'+');
    }
    CtSearch(true);
    std::string id;
    for (const Game& g : games)
        if (!g.pc && (pickTid.empty() ? id.empty() : g.titleId == pickTid)) id = g.id;
    if (id.empty()) return true;
    if (!pickEmu.empty() && cfg.emulators.count(pickEmu)) cfg.perGame[id] = pickEmu;
    OpenOptions(id);
    // The options panel's "Gestión de datos" (identical mode) or NSW-Home's own one.
    for (size_t c = 0; c < options.cats.size(); c++)
        if (options.cats[c].id == "data" || options.cats[c].name == L"Gestión de datos") options.cat = static_cast<int>(c);
    options.inRows = true;
    if (S2()) optOpenAt = now - 1; // already open
    std::vector<Row> rows = options.cats.empty() ? std::vector<Row>() : options.cats[options.cat].rows();
    // The cursor on the first of the four rows; for the still of the panel on
    // the last, which brings all four into view.
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].label == (name == L"content-last" ? L"Trucos" : L"Datos de actualización")) options.row = static_cast<int>(i);
    if (name == L"content-upd") OpenPage(L"Datos de actualización", CtUpdateRows);
    else if (name == L"content-dlc") OpenPage(L"Gestionar contenido descargable", CtDlcRows);
    else if (name == L"content-mods") OpenPage(L"Mods", CtModRows);
    else if (name == L"content-cheats") OpenPage(L"Trucos", CtCheatRows);
    if (name == L"dumpoptions") {
        static const char* const kinds[] = {"plain", "toggle", "radio", "slider", "text", "panel", "table", "gap", "storage", "users"};
        auto flat = [](std::wstring t) {
            for (wchar_t& ch : t)
                if (ch == L'\n' || ch == L'\t') ch = L' ';
            return Narrow(t);
        };
        std::string out = "page\tkind\tlabel\tvalue\treal\taction\tdisabled\ton\tnote\n";
        auto dump = [&](const std::wstring& page, const std::vector<Row>& rs) {
            for (const Row& r : rs) {
                if (r.kind == RK::Gap) continue;
                out += flat(page) + "\t" + (r.heading ? "heading" : kinds[static_cast<int>(r.kind)]) + "\t" + flat(r.label) + "\t" +
                       flat(r.value) + "\t" + (r.real ? "1" : "0") + "\t" + (r.act || r.step ? "1" : "0") + "\t" +
                       (r.disabled ? "1" : "0") + "\t" + (r.on ? "1" : "0") + "\t" + flat(r.note) + "\n";
            }
        };
        out += "# " + Narrow(FindGame(id)->name) + " (" + FindGame(id)->titleId + ") with " + ct.emu + "\n";
        for (const Cat& c : options.cats) dump(c.name, c.rows());
        dump(L"Datos de actualización", CtUpdateRows());
        dump(L"Gestionar contenido descargable", CtDlcRows());
        dump(L"Mods", CtModRows());
        dump(L"Trucos", CtCheatRows());
        FILE* f = nullptr;
        if (!_wfopen_s(&f, file.c_str(), L"wb") && f) {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }
    }
    return true;
}

void OpenOptions(const std::string& id) {
    Game* g = FindGame(id);
    if (!g) return;
    optionsGame = id;
    options.title = g->name;
    options.icon = 8;
    options.cat = options.row = 0;
    options.sub = 0;
    options.scroll = 0;
    options.over = -1;
    options.catScroll = -1;
    options.inRows = false;
    options.cats.clear();
    picker.open = false;
    CtOptionsOpened(id);

    auto emulator = [] {
        std::vector<Row> rows;
        auto cur = cfg.perGame.find(optionsGame);
        bool isDefault = cur == cfg.perGame.end();
        rows.push_back({L"Predeterminado (" + EmuLabel(cfg.defaultEmu) + L")", Check(isDefault), [] {
            cfg.perGame.erase(optionsGame);
            SaveConfig();
        }});
        for (const auto& kv : cfg.emulators) {
            std::string emu = kv.first;
            rows.push_back({EmuLabel(emu), Check(!isDefault && cur->second == emu), [emu] {
                cfg.perGame[optionsGame] = emu;
                SaveConfig();
            }});
        }
        return rows;
    };
    auto information = [] {
        std::vector<Row> rows;
        Game* game = FindGame(optionsGame);
        if (!game) return rows;
        rows.push_back({L"Editor", game->publisher, nullptr});
        rows.push_back({L"Versión", game->version, nullptr});
        rows.push_back({L"Tiempo de juego", PlayTime(game->seconds), nullptr});
        rows.push_back({L"Última partida", DateOf(game->last), nullptr});
        rows.push_back({L"Tamaño", SizeOf(game->size), nullptr});
        rows.push_back({L"Id. del programa", Widen(game->titleId), nullptr});
        return rows;
    };
    auto files = [] {
        std::vector<Row> rows;
        Game* game = FindGame(optionsGame);
        if (!game) return rows;
        std::wstring file = game->file;
        if (OnAndroid()) {
            // [android] no file manager to show the file in: the row says where it is
            // (the last two folders; the whole path would push the label out of the row)
            std::wstring dir = file.substr(0, file.find_last_of(L"\\/"));
            size_t cut = dir.find_last_of(L"\\/");
            if (cut != std::wstring::npos && cut > 0) cut = dir.find_last_of(L"\\/", cut - 1);
            if (cut != std::wstring::npos && cut > 0) dir = L"…" + dir.substr(cut);
            rows.push_back({L"Carpeta", dir, nullptr});
        } else {
            rows.push_back({L"Mostrar el archivo en el Explorador", L"", [file] { RevealFile(file); }});
        }
        if (optionsGame == runningId) AmiiboUseRow(rows);
        if (optionsGame == runningId) rows.push_back({L"Cerrar programa", L"", [] { AskClose(); }});
        return rows;
    };

    if (g->pc) {
        PcOptions();
    } else if (cfg.identical) {
        // The console's own sections, then everything NSW-Home adds in one of its own.
        for (const console::Section& sec : console::GameOptions().sections) {
            const console::Section* s = &sec;
            Cat c;
            c.name = sec.name;
            c.id = sec.id;
            // The console breaks these two names where the wrap would not.
            if (c.id == "info") c.name = L"Información\ndel programa";
            if (c.id == "cloud") c.name = L"Guardado de datos\nen la nube";
            c.rows = [s] {
                std::vector<Row> rows;
                for (const console::Row& cr : s->rows) AddOptionRow(rows, cr);
                if (std::string(s->id) == "data") {
                    std::vector<Row> more = CtRows();
                    if (!more.empty()) rows.push_back(GapRow());
                    for (Row& r : more) rows.push_back(std::move(r));
                }
                return rows;
            };
            options.cats.push_back(c);
        }
        Cat own;
        own.name = L"NSW-Home";
        own.id = "nswhome";
        own.rows = [emulator, information, files] {
            std::vector<Row> rows;
            rows.push_back(HeadRow(L"Emulador"));
            for (Row& r : emulator()) rows.push_back(std::move(r));
            rows.push_back(HeadRow(L"Información"));
            for (Row& r : information()) rows.push_back(std::move(r));
            rows.push_back(HeadRow(L"Archivo"));
            for (Row& r : files()) rows.push_back(std::move(r));
            return rows;
        };
        options.cats.push_back(own);
    } else {
        options.cats.push_back({L"Emulador", emulator});
        options.cats.push_back({L"Información", information});
        options.cats.push_back({L"Gestión de datos", [files] {
                                    std::vector<Row> rows = CtRows();
                                    for (Row& r : files()) rows.push_back(std::move(r));
                                    return rows;
                                }});
    }
    if (S2()) {
        // A panel over the menu it was opened from: no screen change, the
        // panel brings its own entrance (see DrawOptionsPanel).
        optOpenAt = now + 1.0 / 60.0; // a button arrives before the frame's time is set: `now` is the last frame's
        optCloseAt = -1;
        PushNow(Scr::Options);
    } else {
        Push(Scr::Options);
    }
}

/// ---- album ----------------------------------------------------------------

// Grid geometry. The second console's album (anim.mkv 271 s, walkthrough.mp4
// 214 s) has five 288 x 162 px pictures to a row, 16 px apart, starting at
// (256, 192) in the 1080p capture; in layout units that is the second line.
struct AlbumLay {
    int cols;
    float w, h, gap, x0, y0;
};

AlbumLay AlbL() {
    if (S2()) return {5, 192.0f, 108.0f, 10.67f, 170.67f, 128.0f};
    return {4, 272.0f, 153.0f, 20.0f, 66.0f, 116.0f};
}

// How the second console's album opens, counted in frames from the first one
// on which the page is drawn (anim.mkv, title first visible at 270.73 s):
//   17-24  the empty cells fade in (4, 13, 22, 37, 57, 76, 91, 100 %)
//   25     the focus outline appears on the first cell
//   28-    pictures arrive one per frame, each fading in over five frames
double albumAt = -1;          // first frame of this visit; -1 until the page is drawn
std::vector<double> shotAt;   // when each picture starts to appear: -1 not yet, -2 seen before
double shotNext = 0;          // earliest moment the next picture may start
float albScrollFrom = 0, albScrollTo = 0;
double albScrollAt = -1;

bool albumLeaving = false; // B was pressed: free the pictures when the page is covered
bool albumDemo = false; // test states: fill the album with the library's own pictures

// ---- captures: the notice, and what the album knows about its pictures -------

// The notice shown over a game when a capture has been saved. No capture of
// the console shows its own (the owner's recordings never leave the menu), so
// this is a quiet one in the menu's terms: the ground and corner of a dialog
// box, the one-pixel rule round it, a dialog's small print, the album's mark.
constexpr float kNoticeH = 44.0f, kNoticeSize = 17.7f;

const wchar_t* NoticeText(bool ok) {
    return ok ? L"Captura guardada" : L"No se ha podido guardar la captura";
}

float NoticeW(bool ok) {
    return 52.0f + TextWidth(NoticeText(ok), kNoticeSize) + 20.0f;
}

void DrawNotice(bool ok) {
    Theme T = Th();
    const float w = NoticeW(ok);
    RRect(0, 0, w, kNoticeH, 11.0f, T.dialog);
    RRectStroke(Px(0.5f), Px(0.5f), w - Px(1), kNoticeH - Px(1), 11.0f, T.line, Px(1));
    PushScale(0.72f, 28, kNoticeH / 2);
    SysIcon(3, 28, kNoticeH / 2, T.text);
    PopTransform();
    Text(NoticeText(ok), 52, 0, w - 52, kNoticeH, kNoticeSize, T.text);
}

int debugNotice = 0; // test states: 1 the notice, 2 the one for a failed capture, in its corner of the menu

// Album: each picture's tag (see CaptureTag; "" for an emulator's own file),
// kept beside `shots`.
std::vector<std::string> shotTag;
bool albOldest = false; // "Por antigüedad"
bool albRail = false;   // the focus is on the sort button, left of the pictures
bool albMenu = false;   // the sort menu is open
int albMenuSel = 0;
double viewerAt = 0;    // last button in the viewer: its caption goes away after a while

const wchar_t* AlbSortName() {
    return albOldest ? L"Por antigüedad" : L"Por novedad";
}

Game* ShotGame(int i) {
    if (i < 0 || i >= static_cast<int>(shotTag.size()) || shotTag[i].empty()) return nullptr;
    for (auto& g : games)
        if (CaptureTag(g.id) == shotTag[i]) return &g;
    return nullptr;
}

// "26/11/2026 12:15" out of one of NSW-Home's own file names, else the file's name.
std::wstring ShotLabel(int i) {
    const std::wstring& p = shots[i].path;
    std::wstring name = p.substr(p.find_last_of(L"\\/") + 1);
    if (shotTag[i].empty() || name.size() < 19 || name[8] != L'-' || name[15] != L'-' || name[18] != L'_') return name;
    return lang::DayFirst(name.substr(6, 2) + L"/" + name.substr(4, 2) + L"/" + name.substr(0, 4) + L" " + name.substr(9, 2) + L":" +
                              name.substr(11, 2),
                          cfg.clock24); // in the language's order
}

void AlbumRestart() {
    shotSel = 0;
    shotScroll = 0;
    albScrollFrom = albScrollTo = 0;
    albScrollAt = -1;
    shotAt.assign(shots.size(), -1.0);
    shotNext = 0;
}

// Called by OpenAlbum once `shots` is filled, newest first.
void AlbumLoaded() {
    shotTag.clear();
    for (size_t i = 0; i < shots.size(); i++) {
        // Test album: the pictures are the library's, so each one's game is known.
        if (albumDemo && !games.empty()) shotTag.push_back(CaptureTag(games[i % games.size()].id));
        else shotTag.push_back(CaptureTagOf(shots[i].path));
    }
    albOldest = cfg.console["album.sort"] == "old";
    if (albOldest) {
        std::reverse(shots.begin(), shots.end());
        std::reverse(shotTag.begin(), shotTag.end());
    }
    albRail = albMenu = false;
}

void AlbumSort(bool oldest) {
    if (oldest != albOldest) {
        albOldest = oldest;
        cfg.console["album.sort"] = oldest ? "old" : "new";
        SaveConfig();
        for (auto& s : shots) s.reset();
        std::reverse(shots.begin(), shots.end());
        std::reverse(shotTag.begin(), shotTag.end());
        AlbumRestart();
    }
    albMenu = false;
}

// The sort button's part of the album's buttons. True when the press was used.
bool AlbumRailButton(Btn b) {
    const int n = static_cast<int>(shots.size());
    if (!S2()) {
        // First console's style: no column of buttons, Y changes the order.
        if (b != Btn::Y || n < 2) return false;
        PlaySfx(Sfx::Ok);
        AlbumSort(!albOldest);
        return true;
    }
    if (albMenu) {
        if (b == Btn::Up || b == Btn::Down) {
            const int v = albMenuSel + (b == Btn::Down ? 1 : -1);
            if (v < 0 || v > 1) {
                PlaySfx(Sfx::Bump);
                BumpCursor(0, b == Btn::Down ? 1.0f : -1.0f);
            } else {
                albMenuSel = v;
                PlaySfx(Sfx::Move);
            }
        } else if (b == Btn::A) {
            PlaySfx(Sfx::Ok);
            AlbumSort(albMenuSel == 1);
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            albMenu = false;
        }
        return true;
    }
    if (albRail) {
        if (b == Btn::Right) {
            albRail = false;
            PlaySfx(Sfx::Move);
        } else if (b == Btn::A) {
            albMenu = true;
            albMenuSel = albOldest ? 1 : 0;
            PlaySfx(Sfx::Ok);
        } else if (b == Btn::B) {
            return false; // leaves the album as from the pictures
        } else if (b == Btn::Left || b == Btn::Up || b == Btn::Down) {
            PlaySfx(Sfx::Bump);
            BumpCursor(b == Btn::Left ? -1.0f : 0.0f, b == Btn::Up ? -1.0f : b == Btn::Down ? 1.0f : 0.0f);
        }
        return true;
    }
    if (b == Btn::Left && n > 0 && shotSel % AlbL().cols == 0) {
        albRail = true;
        PlaySfx(Sfx::Move);
        return true;
    }
    return false;
}

// The console's album has a column of four round buttons left of the pictures
// (walkthrough.mp4 217-222 s: several at once, filter, sort, upload settings),
// centred on pixel column 144. Sorting is the one that means something here,
// and stands where the console has it (pixel row 448). Focused, a button has
// the focus ring round a disc and its name beside it; A opens its menu
// (219.3 s): a box from (229, 386) to (833, 618) px with two rows 94 px apart,
// each ending in a radio button.
void DrawAlbumRail() {
    Theme T = Th();
    const WidgetLook K = WL();
    const float cx = Px(144), cy = Px(448);
    const bool lit = albRail || albMenu;
    if (lit) Circle(cx, cy, Px(48), T.focusBg);
    const Color c = lit ? T.accent : T.text;
    // Two arrows, one down and one up, 36 px high.
    const float ax[2] = {cx - Px(9), cx + Px(9)};
    for (int i = 0; i < 2; i++) {
        const float tip = i == 0 ? cy + Px(18) : cy - Px(18), back = i == 0 ? -Px(9) : Px(9);
        Line(ax[i], cy - Px(17), ax[i], cy + Px(17), c, Px(3.5f));
        Line(ax[i] - Px(9), tip + back, ax[i], tip, c, Px(3.5f));
        Line(ax[i] + Px(9), tip + back, ax[i], tip, c, Px(3.5f));
    }
    if (albRail && !albMenu) {
        CircleCursor(cx, cy, Px(48));
        // Its name, in a rounded box pointing at the button (218.0 s).
        const float tw = TextWidth(L"Ordenar", 24.5f), bx = Px(220);
        Triangle(Px(204), cy, bx + 1, cy - Px(12), bx + 1, cy + Px(12), T.dialog);
        RRect(bx, cy - Px(42), tw + Px(88), Px(84), Px(28), T.dialog);
        Text(L"Ordenar", bx + Px(44), cy - Px(42), tw + 8, Px(84), 24.5f, T.accent);
    }
    AddHit(cx - Px(48), cy - Px(48), Px(96), Px(96), [] {
        if (albRail && !albMenu) OnButtonImpl(Btn::A);
        else if (!albMenu) {
            albRail = true;
            PlaySfx(Sfx::Move);
        }
    });
    if (!albMenu) return;
    AddHit(0, 0, W, H, [] { OnButtonImpl(Btn::B); }); // a click beside the menu closes it
    RRect(Px(229), Px(386), Px(604), Px(232), Px(22), T.dialog);
    RRectStroke(Px(229.5f), Px(386.5f), Px(603), Px(231), Px(22), alpha(T.line, 0.55f), Px(1));
    static const wchar_t* const names[2] = {L"Por novedad", L"Por antigüedad"};
    for (int i = 0; i < 2; i++) {
        const float ry = Px(406) + i * Px(94), rcy = ry + Px(49);
        const bool on = (i == 1) == albOldest;
        if (i == albMenuSel) {
            RRect(Px(249), ry, Px(564), Px(98), Px(8), T.focusBg);
            CursorFill(T.focusBg);
            Cursor(Px(249), ry, Px(564), Px(98), Px(8));
        }
        Text(names[i], Px(283), ry, Px(400), Px(98), 24.5f, on ? T.accent : T.text);
        if (on) {
            Circle(Px(755), rcy, 16, K.on);
            Circle(Px(755), rcy, 5.33f, K.knob);
        } else {
            CircleStroke(Px(755), rcy, 15.33f, K.dim, 1.33f);
        }
        AddHit(Px(249), ry, Px(564), Px(98), [i] {
            albMenuSel = i;
            OnButtonImpl(Btn::A);
        });
    }
}

void OpenAlbum() {
    shots.clear();
    for (auto& f : ListScreenshots()) {
        Img im;
        im.path = std::move(f);
        im.maxSize = 1280; // thumbnails and the viewer share one decode
        shots.push_back(std::move(im));
    }
    if (albumDemo) {
        shots.clear();
        for (int i = 0; i < 38 && !games.empty(); i++) {
            Img im;
            im.path = games[i % games.size()].icon.path;
            im.maxSize = 1280;
            if (!im.path.empty()) shots.push_back(std::move(im));
        }
    }
    AlbumLoaded();
    shotSel = 0;
    shotScroll = 0;
    albumLeaving = false;
    albumAt = -1;
    shotAt.assign(shots.size(), -1.0);
    shotNext = 0;
    albScrollFrom = albScrollTo = 0;
    albScrollAt = -1;
    Push(Scr::Album);
}

void DrawAlbumS2() {
    Theme T = Th();
    const AlbumLay L = AlbL();
    const int n = static_cast<int>(shots.size());
    const float stepY = L.h + L.gap;
    if (albumLeaving && coverNow >= 0.999f) {
        shots.clear();
        albumLeaving = false;
        return;
    }
    if (shotAt.size() != shots.size()) shotAt.assign(shots.size(), -1.0);
    if (albumAt < 0) albumAt = now;
    const float k = static_cast<float>((now - albumAt) * 60.0) + 0.001f;
    if (k < 40) Touch();

    // Scrolling, measured on walkthrough.mp4 215.3 s: the focused row may go
    // down to the fourth line of the grid's rhythm (top edge at 597 px), then
    // the grid moves instead, one row in 7 frames, easing out ((1-t)^2).
    const int row = n ? shotSel / L.cols : 0;
    const float rowTop = L.y0 + row * stepY;
    if (MouseOwns(&shotScroll)) {
        albScrollTo = shotScroll;
        albScrollAt = -1;
    }
    float target = albScrollTo;
    if (rowTop - target > 398.0f) target = rowTop - 398.0f;
    if (rowTop - target < L.y0) target = rowTop - L.y0;
    if (target != albScrollTo) {
        albScrollFrom = shotScroll;
        albScrollTo = target;
        albScrollAt = now - 1 / 60.0; // the first frame after the press already shows movement
    }
    if (albScrollAt >= 0) {
        float t = static_cast<float>((now - albScrollAt) * 60.0) / 7.0f;
        if (t >= 1) {
            shotScroll = albScrollTo;
            albScrollAt = -1;
        } else {
            shotScroll = albScrollTo + (albScrollFrom - albScrollTo) * (1 - t) * (1 - t);
            Touch();
        }
    }

    static const float cellIn[] = {0.04f, 0.13f, 0.22f, 0.37f, 0.57f, 0.76f, 0.91f};
    static const float shotIn[] = {0.05f, 0.23f, 0.57f, 0.84f};
    const int ci = static_cast<int>(k) - 17;
    const float cells = ci < 0 ? 0.0f : ci < 7 ? cellIn[ci] : 1.0f;
    // Empty cell: the theme's cell colour (4C4C4C in the dark theme, measured
    // here and on the software grid; the light theme's album is in no capture).
    const Color empty = T.tile;

    // Dragged, the focus keeps to a row whose top is where the grid's own
    // scrolling lets the focused row stand (between y0 and 398).
    const int albRows = (n + L.cols - 1) / L.cols;
    const int albDrag = cells < 1 ? -1 :
        AddScroll(0, 86, W, 559.5f, ScrollOf(shotScroll, L.y0 + (albRows - 1) * stepY - 398.0f, [L, n, albRows, stepY](float v) {
            int r = NearestFit(shotSel / L.cols, albRows, [&](int i) {
                const float top = L.y0 + i * stepY - v;
                return top >= L.y0 - 0.5f && top <= 398.5f;
            });
            if (r >= 0) shotSel = (std::min)(n - 1, r * L.cols + shotSel % L.cols);
        }));
    PushClip(0, 86, W, 559.5f);
    for (int i = 0; i < n && cells > 0; i++) {
        float x = L.x0 + (i % L.cols) * (L.w + L.gap), y = L.y0 + (i / L.cols) * stepY - shotScroll;
        if (y > 660 || y + L.h < 80) {
            // Off-screen pictures give their memory back.
            shots[i].reset();
            if (shotAt[i] >= 0) shotAt[i] = -2;
            continue;
        }
        Rect(x, y, L.w, L.h, alpha(empty, cells));
        if (k >= 28) {
            if (shotAt[i] == -2) shotAt[i] = now - 1; // seen before: no second entrance
            if (shotAt[i] < 0) {
                shotAt[i] = (std::max)(now, shotNext);
                shotNext = shotAt[i] + 1 / 60.0;
            }
            int si2 = static_cast<int>((now - shotAt[i]) * 60.0 + 0.001);
            if (si2 >= 0) Image(shots[i], x, y, L.w, L.h, si2 < 4 ? shotIn[si2] : 1.0f);
            if (si2 < 4) Touch();
        }
        // Round the focused picture (216 s, a cell on rows 600-761): a black
        // band from 4 px outside it to 1 px inside, then the outline, 6 px wide.
        if (i == shotSel && k >= 25 && !albRail) {
            RRectStroke(x - Px(1.5f), y - Px(1.5f), L.w + Px(3), L.h + Px(3), 3, rgb(0x000000), Px(5));
            Cursor(x - Px(7), y - Px(7), L.w + Px(14), L.h + Px(14), 5);
        }
        AddHit(x, y, L.w, L.h, [i] {
            if (shotSel == i) OnButtonImpl(Btn::A);
            else {
                shotSel = i;
                PlaySfx(Sfx::Move);
            }
        });
    }
    PopClip();

    // Scroll mark at the right edge, as long as the share of the album in view.
    const float rows = static_cast<float>((n + L.cols - 1) / L.cols);
    const float content = L.y0 - 86 + rows * stepY, view = 559.5f;
    if (content > view && cells > 0) {
        const float track0 = 92.0f, track = 546.7f;
        float len = (std::max)(20.0f, track * view / content);
        float pos = (std::min)(1.0f, shotScroll / (content - view));
        // (On pixel columns 1885 to 1890.)
        RRect(Px(1885), Snap(track0 + (track - len) * pos), Px(6), Snap(len), 2, alpha(cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7), cells));
        AddScrollBar(albDrag, Px(1885), track0, track, len, content - view);
    }

    Header(3, L"Álbum");
    if (n == 0) {
        Text(L"No hay capturas", 0, 300, W, 50, 30, T.text, Center);
        Text(L"Las capturas que hagas mientras juegas aparecerán aquí.", 0, 356, W, 36, 21, T.sub, Center);
        Footer({{L"B", L"Atrás", Btn::B}});
        return;
    }
    // Top right, in the console's place and words for what NSW-Home really
    // shows: newest first, every screenshot the emulators have saved.
    wchar_t count[64];
    swprintf_s(count, L"Todas las capturas (%d)", n);
    const float size = 19.8f, right = 1198.0f + Px(2); // the wording ends on pixel column 1796
    float cw = TextWidth(count, size), sw = TextWidth(AlbSortName(), size);
    Text(count, right - cw, 30, cw + 8, 40, size, T.sub);
    Rect(Snap(right - cw - 12.0f), Px(58), Px(2), Px(37), T.sub); // 2 px wide, rows 58 to 94
    Text(AlbSortName(), right - cw - 24.0f - sw, 30, sw + 8, 40, size, T.sub);
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}});
    if (cells >= 1) DrawAlbumRail();
}

void DrawAlbum(float dt) {
    if (S2()) {
        DrawAlbumS2();
        return;
    }
    Theme T = Th();
    const AlbumLay L = AlbL();
    int n = static_cast<int>(shots.size());
    float stepY = L.h + L.gap;
    int row = n ? shotSel / L.cols : 0;
    float target = shotScroll, top = L.y0 + row * stepY - shotScroll;
    if (top < L.y0) target = row * stepY;
    if (top + L.h > 630) target = L.y0 + row * stepY + L.h - 630;
    if (MouseOwns(&shotScroll)) target = shotScroll;
    Smooth(shotScroll, target, dt);
    if (shotScroll != target) Touch();

    const int albRows = (n + L.cols - 1) / L.cols; // dragged, as in the second console's album
    AddScroll(0, 89, W, 558, ScrollOf(shotScroll, L.y0 + (albRows - 1) * stepY + L.h - 630, [L, n, albRows, stepY](float v) {
        int r = NearestFit(shotSel / L.cols, albRows, [&](int i) {
            const float rt = L.y0 + i * stepY - v;
            return rt >= L.y0 - 0.5f && rt + L.h <= 630.5f;
        });
        if (r >= 0) shotSel = (std::min)(n - 1, r * L.cols + shotSel % L.cols);
    }));
    PushClip(0, 89, W, 558);
    for (int i = 0; i < n; i++) {
        float x = L.x0 + (i % L.cols) * (L.w + L.gap), y = L.y0 + (i / L.cols) * stepY - shotScroll;
        if (y > 660 || y + L.h < 80) {
            // Off-screen pictures give their memory back.
            shots[i].reset();
            continue;
        }
        Rect(x, y, L.w, L.h, T.tile);
        Image(shots[i], x, y, L.w, L.h);
        if (i == shotSel) Cursor(x - 7, y - 7, L.w + 14, L.h + 14, 3);
        AddHit(x, y, L.w, L.h, [i] {
            if (shotSel == i) OnButtonImpl(Btn::A);
            else {
                shotSel = i;
                PlaySfx(Sfx::Move);
            }
        });
    }
    PopClip();
    Header(3, L"Álbum");
    if (n == 0) {
        Text(L"No hay capturas", 0, 300, W, 50, 30, T.text, Center);
        Text(L"Las capturas que hagas mientras juegas aparecerán aquí.", 0, 356, W, 36, 21, T.sub, Center);
        Footer({{L"B", L"Atrás", Btn::B}});
    } else {
        Footer({{L"Y", AlbSortName(), Btn::Y}, {L"B", L"Atrás", Btn::B}, {L"A", L"Ver", Btn::A}});
    }
}

void AlbumButton(Btn b) {
    if (AlbumRailButton(b)) return;
    int n = static_cast<int>(shots.size());
    const int cols = AlbL().cols;
    auto go = [&](int d) {
        int v = shotSel + d;
        if (v < 0 || v >= n) PlaySfx(Sfx::Bump);
        else {
            shotSel = v;
            PlaySfx(Sfx::Move);
        }
    };
    switch (b) {
    case Btn::Left: go(-1); break;
    case Btn::Right: go(1); break;
    case Btn::Up: go(-cols); break;
    case Btn::Down: go(cols); break;
    case Btn::A:
        if (n > 0) {
            PlaySfx(Sfx::Ok);
            viewerAt = now;
            Push(Scr::Viewer);
        }
        break;
    case Btn::B:
        PlaySfx(Sfx::Back);
        Pop();
        // In the second console's style the page stays up while it fades
        // out, pictures included; they are let go once it is covered.
        if (S2()) albumLeaving = true;
        else shots.clear();
        break;
    default: break;
    }
}

// The viewer is in none of the owner's captures. The picture fills the screen;
// a caption over its foot names the game and the moment (as far as the file's
// name says) and what the buttons do, and goes away after three quiet seconds.
void DrawViewer() {
    Rect(0, 0, W, H, rgb(0x000000));
    const int n = static_cast<int>(shots.size());
    if (shotSel >= n) return;
    Image(shots[shotSel], 0, 0, W, H);
    AddHit(0, 0, W, H, [] { OnButtonImpl(Btn::B); });
    const float age = static_cast<float>(now - viewerAt);
    const float a = age < 3.0f ? 1.0f : (std::max)(0.0f, 1.0f - (age - 3.0f) / 0.3f);
    if (a <= 0) return;
    if (age < 3.4f) Touch();
    const Color ink = rgb(0xFFFFFF, a);
    Rect(0, 660, W, 60, rgb(0x000000, 0.62f * a));
    Game* g = shotSel < static_cast<int>(shotTag.size()) ? ShotGame(shotSel) : nullptr;
    std::wstring what = shotSel < static_cast<int>(shotTag.size()) ? ShotLabel(shotSel) : std::wstring();
    if (g) what = g->name + L"   ·   " + what;
    wchar_t pos[32];
    swprintf_s(pos, L"%d/%d   ", shotSel + 1, n);
    Text(pos + what, 48, 660, 620, 60, 19.8f, ink);
    // Deleting is only offered for NSW-Home's own files.
    const bool own = shotSel < static_cast<int>(shotTag.size()) && !shotTag[shotSel].empty();
    struct VHint {
        wchar_t sym;
        const wchar_t* letter;
        const wchar_t* label;
        Btn btn;
    };
    const VHint hints[3] = {{0xE0E1, L"B", L"Atrás", Btn::B}, {0xE0E3, L"Y", L"Mostrar en la carpeta", Btn::Y},
                            {0xE0E2, L"X", L"Borrar", Btn::X}};
    float x = 1232;
    for (int i = 0; i < (own ? 3 : 2); i++) {
        const float tw = TextWidth(hints[i].label, 19.8f), left = x - tw - 30;
        Text(hints[i].label, x - tw, 660, tw + 4, 60, 19.8f, ink);
        if (!Symbol(hints[i].sym, left + 10, 684.5f, 21.0f, ink)) {
            Circle(left + 10, 690, 10, ink);
            Text(hints[i].letter, left - 2, 678, 24, 24, 14, rgb(0x000000, a), Center, 700);
        }
        const Btn b = hints[i].btn;
        AddHit(left - 4, 664, tw + 40, 52, [b] { OnButtonImpl(b); });
        x = left - 26;
    }
}

void ViewerButton(Btn b) {
    int n = static_cast<int>(shots.size());
    viewerAt = now;
    const bool own = shotSel < static_cast<int>(shotTag.size()) && !shotTag[shotSel].empty();
    if (b == Btn::B || b == Btn::A) {
        PlaySfx(Sfx::Back);
        Pop();
    } else if ((b == Btn::Left || b == Btn::L) && shotSel > 0) {
        shotSel--;
        PlaySfx(Sfx::Move);
    } else if ((b == Btn::Right || b == Btn::R) && shotSel + 1 < n) {
        shotSel++;
        PlaySfx(Sfx::Move);
    } else if (b == Btn::Y && shotSel < n) {
        PlaySfx(Sfx::Ok);
        RevealFile(shots[shotSel].path);
    } else if (b == Btn::X && own) {
        Ask(L"¿Quieres borrar esta captura?\n\nEl archivo se eliminará del ordenador.", {L"Cancelar", L"Borrar"}, [](int choice) {
            const int i = shotSel;
            if (choice != 1 || i >= static_cast<int>(shots.size())) return;
            shots[i].reset();
            if (!DeleteCapture(shots[i].path)) {
                PlaySfx(Sfx::Error);
                Info(L"No se ha podido borrar la captura.");
                return;
            }
            shots.erase(shots.begin() + i);
            shotTag.erase(shotTag.begin() + i);
            shotAt.assign(shots.size(), -2.0); // the album's pictures do not make a second entrance
            if (shotSel >= static_cast<int>(shots.size())) shotSel = (std::max)(0, static_cast<int>(shots.size()) - 1);
            if (shots.empty()) Pop(); // nothing left to look at: back to the album
        });
    }
}

// ---- controllers, user, launch, sleep -------------------------------------

// A game controller as plain shapes, centred on (cx, cy), about 78 x 52.
void PadPicture(float cx, float cy, Color body, Color edge, Color mark) {
    RRect(cx - 39, cy - 26, 78, 52, 20, body);
    RRectStroke(cx - 39, cy - 26, 78, 52, 20, edge, 1.6f);
    Rect(cx - 27, cy - 3.5f, 16, 3, mark);
    Rect(cx - 20.5f, cy - 10, 3, 16, mark);
    Circle(cx + 22, cy - 8, 3.4f, mark);
    Circle(cx + 14, cy + 2, 3.4f, mark);
    Circle(cx + 4, cy + 10, 5.0f, mark);
    Circle(cx - 6, cy - 12, 5.0f, mark);
}

// ---- controllers: the page, the order screen, test states -------------------
//
// One block. The pads themselves are read in pads.cpp / pads_win.cpp; this is
// only what is shown and the order the players choose. Test states: "pads"
// (what is really connected), "pads-set" (a DualSense, a Pro Controller, a
// Joy-Con pair, an Xbox pad), "pads-none", "pads-order" (the order screen with
// that set; in a script Y presses L+R on the next made-up pad, L then R is
// the keyboard's L+R), "pads-on" / "pads-off" (the older two).

void ConsoleOnly(); // with the user page, below

int debugPads = -1; // older test states: non-zero = two XInput pads, 0 = none, -1 = the real ones

struct PadsUi {
    int sel = 0;        // focused row of the page's right column
    bool order = false; // the order screen is up
    std::vector<std::string> picked; // identities, in the order L+R was pressed
    std::vector<double> pickedAt;
    double keyL = -10, keyR = -10; // when the keyboard's L and R were last pressed
    int debugNext = 0;
    double drawnAt = -10; // when the page was last on screen
    std::string noteKey; // what the emulator note below was worked out for
    std::vector<int> unknown;
    std::wstring emuName;
} padsUi;

// The connected pads in player order, the keyboard last.
std::vector<pads::Pad> PadsNow() {
    PadOrderLoad();
    if (debugPads >= 0) {
        pads::DebugFake(debugPads ? "xbox xbox" : "");
        debugPads = -1;
    }
#if !defined(_WIN32)
    // The other front ends read pads their own way and only say which slots are in use.
    std::vector<bool> slots = ConnectedPads();
    for (int i = 0; i < 4 && i < static_cast<int>(slots.size()); i++) pads::SetXInput(i, slots[i], 0, 0, 0);
#endif
    return pads::List();
}

const wchar_t* PadName(pads::Kind k) {
    switch (k) {
    case pads::Kind::DualSense: return L"DualSense";
    case pads::Kind::DualShock4: return L"DUALSHOCK 4";
    case pads::Kind::SwitchPro: return L"Mando Pro";
    case pads::Kind::JoyConL: return L"Joy-Con (L)";
    case pads::Kind::JoyConR: return L"Joy-Con (R)";
    case pads::Kind::JoyConPair: return L"Joy-Con";
    case pads::Kind::Keyboard: return L"Teclado";
    default: return L"Mando Xbox";
    }
}

// Keyboard and mouse, centred on (cx, cy).
void KeyboardPicture(float cx, float cy, Color body, Color edge) {
    float kx = cx - 96, ky = cy - 28;
    RRect(kx, ky, 150, 56, 7, body);
    RRectStroke(kx, ky, 150, 56, 7, edge, 1.6f);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 11; c++) Rect(kx + 10 + c * 12.0f, ky + 9 + r * 11.0f, 8, 7, edge);
    Rect(kx + 34, ky + 42, 82, 6, edge);
    float mx = cx + 70;
    RRect(mx, ky + 4, 30, 48, 15, body);
    RRectStroke(mx, ky + 4, 30, 48, 15, edge, 1.6f);
    Line(mx + 15, ky + 4, mx + 15, ky + 22, edge, 1.6f);
}

// Each kind of pad as plain shapes of its own outline (no console artwork).
void PadShape(pads::Kind kind, float cx, float cy, Color body, Color edge) {
    const auto half = [&](float x, bool left) { // one upright Joy-Con, 30 x 72
        RRect(x, cy - 36, 30, 72, 13, body);
        RRectStroke(x, cy - 36, 30, 72, 13, edge, 1.6f);
        Circle(x + 15, cy + (left ? -14.0f : 12.0f), 5.5f, edge);
        for (int i = 0; i < 4; i++)
            Circle(x + 15 + (i == 0 ? -6.0f : i == 1 ? 6.0f : 0.0f), cy + (left ? 12.0f : -14.0f) + (i == 2 ? -6.0f : i == 3 ? 6.0f : 0.0f),
                   2.3f, edge);
    };
    switch (kind) {
    case pads::Kind::JoyConPair:
        half(cx - 35, true);
        half(cx + 5, false);
        break;
    case pads::Kind::JoyConL:
    case pads::Kind::JoyConR:
        // Held sideways.
        RRect(cx - 36, cy - 15, 72, 30, 13, body);
        RRectStroke(cx - 36, cy - 15, 72, 30, 13, edge, 1.6f);
        Circle(cx - 14, cy, 5.5f, edge);
        for (int i = 0; i < 4; i++)
            Circle(cx + 14 + (i == 0 ? -6.0f : i == 1 ? 6.0f : 0.0f), cy + (i == 2 ? -6.0f : i == 3 ? 6.0f : 0.0f), 2.3f, edge);
        break;
    case pads::Kind::Keyboard:
        PushScale(0.62f, cx, cy);
        KeyboardPicture(cx + 10, cy, body, edge);
        PopTransform();
        break;
    default: PadPicture(cx, cy, body, edge, edge); break;
    }
}

// Player lights: 14 px squares, 26 px apart, the group centred on cx; the lit
// ones sit on a darker green square 19 px wide (anim.mkv 265.5 s).
void PadLights(float cx, float top, int player) {
    for (int k = 0; k < 4; k++) {
        float lx = cx - 30.67f + k * 17.33f;
        if (k < player) {
            Rect(lx - 1.67f, top - 1.67f, 12.67f, 12.67f, cfg.dark ? rgb(0x3A8800) : rgb(0x98EB63));
            Rect(lx, top, 9.33f, 9.33f, cfg.dark ? rgb(0x82ED0A) : rgb(0x12C007));
        } else {
            Rect(lx, top, 9.33f, 9.33f, cfg.dark ? rgb(0x616161) : rgb(0xB4B1B5));
        }
    }
}

// The console's battery mark: 54 x 31 px with its nub (walkthrough.mp4 82.5 s),
// centred on (cx, cy), filled as far as the level.
void PadBattery(float cx, float cy, int percent, bool charging) {
    Theme T = Th();
    const float w = 33, h = 20.67f, x = cx - 18, y = cy - h / 2;
    RRectStroke(x + 1, y + 1, w - 2, h - 2, 3.3f, T.text, 2.0f);
    RRect(x + w, cy - 3.67f, 3, 7.33f, 1, T.text);
    const float inner = (w - 9.33f) * (std::max)(0.08f, (std::min)(1.0f, percent / 100.0f));
    RRect(x + 4.67f, y + 4.67f, inner, h - 9.33f, 1.3f, charging ? T.accent : T.text);
}

void PadsOpenOrder() {
    padsUi.order = true;
    padsUi.picked.clear();
    padsUi.pickedAt.clear();
    padsUi.keyL = padsUi.keyR = -10;
    padsUi.debugNext = 0;
    padsUi.drawnAt = -1; // not drawn yet
    pads::Listen(true);
    fade = 1;
}

void PadsCloseOrder(bool keep) {
    if (keep && !padsUi.picked.empty()) PadOrderSave(padsUi.picked);
    pads::Listen(false);
    padsUi.order = false;
    padsUi.noteKey.clear();
    fade = 1;
}

void PadsPick(const std::string& id) {
    if (padsUi.picked.size() >= 4 || std::find(padsUi.picked.begin(), padsUi.picked.end(), id) != padsUi.picked.end()) return;
    // A Joy-Con picked again another way (alone, then as half of a pair, or
    // the other way round) takes its new form.
    const auto shares = [&](const std::string& other) {
        const bool idPair = id.compare(0, 5, "pair:") == 0, otherPair = other.compare(0, 5, "pair:") == 0;
        if (idPair == otherPair) return false;
        return idPair ? id.find(other) != std::string::npos : other.find(id) != std::string::npos;
    };
    for (size_t i = 0; i < padsUi.picked.size(); i++)
        if (shares(padsUi.picked[i])) {
            padsUi.picked.erase(padsUi.picked.begin() + i);
            padsUi.pickedAt.erase(padsUi.pickedAt.begin() + i);
            break;
        }
    padsUi.picked.push_back(id);
    padsUi.pickedAt.push_back(now);
    PlaySfx(Sfx::Ok);
    Touch();
}

// A button glyph of the console's font, or a drawn stand-in.
void PadGlyph(wchar_t symbol, const wchar_t* letter, float cx, float cy, float size, Color c, Color ink) {
    if (Symbol(symbol, cx, cy - size * 0.262f, size, c)) return;
    if (symbol == 0xE0E4 || symbol == 0xE0E5) RRect(cx - size * 0.52f, cy - size * 0.45f, size * 1.04f, size * 0.9f, size * 0.16f, c);
    else Circle(cx, cy, size * 0.467f, c);
    Text(letter, cx - size, cy - size * 0.5f, size * 2, size, size * 0.62f, ink, Center, 700);
}

// "Pulsa L + R en el mando que quieras usar." (walkthrough.mp4 84.5 s, the
// only capture of this screen, dark theme). Pixel positions from that frame:
// bands 0-243 and 968-1079 in 070713, the panel between in the sheet colour;
// four 366 px boxes from x 183, 396 apart, rows 429 to 794, outlined in A3A3A3;
// a chosen pad's box is filled 424242 and three pixels taller either way.
void DrawPadsOrder() {
    Theme T = Th();
    // (The light theme is in no capture: its bands take the bar's colour.)
    const Color band = cfg.dark ? rgb(0x070713) : T.panel, panel = T.dialog;
    Rect(0, 0, W, H, band);
    Rect(0, Px(244), W, Px(968 - 244), panel);
    // The console's pictures of how a pad can be held, from the owner's capture
    // when the local pack has them (tools\pack_pads.py: pixels 500-1419, 20-225).
    if (cfg.identical && cfg.dark)
        if (Img* strip = PackIcon(L"pads_order_d")) Image(*strip, Px(500), Px(20), Px(920), Px(206));

    std::string id;
    while (pads::TakeChord(id)) PadsPick(id);
    const std::vector<pads::Pad> all = PadsNow();

    // The line of text: "Pulsa" from pixel 523, the L chip 635-705, "+" centred
    // on 742, the R chip 779-849, the rest from 871 to 1396; centred on row 334.
    const float size = 25.4f, cy = Px(335);
    Text(L"Pulsa", Px(523), cy - 22, 100, 44, size, T.text);
    PadGlyph(0xE0E4, L"L", Px(670), cy, 48.0f, T.accent, band);
    Text(L"+", Px(742) - 20, cy - 23, 40, 44, size + 4, T.text, Center);
    PadGlyph(0xE0E5, L"R", Px(814), cy, 48.0f, T.accent, band);
    Text(L"en el mando que quieras usar.", Px(871), cy - 22, 500, 44, size, T.text);

    for (int i = 0; i < 4; i++) {
        const float x = Px(183.0f + i * 396.0f), y = Px(429), s = Px(366);
        if (i >= static_cast<int>(padsUi.picked.size())) {
            RRectStroke(x + Px(0.5f), y + Px(0.5f), s - Px(1), s - Px(1), 16, cfg.dark ? rgb(0xA3A3A3) : T.sub, Px(1));
            wchar_t digit[2] = {static_cast<wchar_t>(L'1' + i), 0};
            Text(digit, x, y, s, s, 37.5f, T.sub, Center); // digits 40 px tall
            continue;
        }
        // The box comes up over 6 frames.
        const float a = (std::min)(1.0f, static_cast<float>((now - padsUi.pickedAt[i]) / (6 * kFrame)));
        if (a < 1) Touch();
        PushLayer(a);
        RRect(x, y - Px(3), s, s + Px(6), 16, T.btn);
        const pads::Pad* pad = nullptr;
        for (const auto& p : all)
            if (p.id == padsUi.picked[i]) pad = &p;
        const float cx = x + s / 2;
        PadLights(cx, Px(476), i + 1); // lights rows 473-490
        const pads::Kind kind = pad ? pad->kind : padsUi.picked[i] == "keyboard" ? pads::Kind::Keyboard : pads::Kind::Xbox;
        // Pictures are 1.28 times the page's here (140 px against 109).
        PushScale(1.28f, cx, Px(611));
        PadShape(kind, cx, Px(611), T.focusBg, cfg.dark ? rgb(0x8A8A8A) : rgb(0x8A888B));
        PopTransform();
        // Battery marks centred on row 749; two for a pair, 82 px apart.
        if (pad && pad->battery >= 0 && pad->battery2 >= 0) {
            PadBattery(cx - Px(41), Px(749.5f), pad->battery, pad->charging);
            PadBattery(cx + Px(41), Px(749.5f), pad->battery2, pad->charging);
        } else if (pad && pad->battery >= 0) {
            PadBattery(cx, Px(749.5f), pad->battery, pad->charging);
        }
        PopLayer();
    }

    // "Pulsa (A) para empezar.": "Pulsa" from pixel 749, the disc centred on
    // (886.5, 880.5) and 61 px across, the rest from 938.
    const float by = Px(880.5f);
    Text(L"Pulsa", Px(749), by - 21, 100, 44, 24.1f, T.text);
    PadGlyph(0xE0E0, L"A", Px(886.5f), by, 43.6f, T.accent, panel);
    Text(L"para empezar.", Px(938), by - 21, 300, 44, 24.1f, T.text);

    // Footer. The console's two "-/+" hints are its own; they are kept in
    // identical mode and answer as every console-only function does.
    if (cfg.identical) {
        const auto pair = [&](float px, const wchar_t* note, float notePx, const wchar_t* label, float labelPx) {
            PadGlyph(0xE0F0, L"-", Px(px), Px(1019), 24.45f, T.text, band);
            Text(L"/", Px(px + 34.5f) - 10, 660, 20, 40, 23.4f, T.text, Center);
            PadGlyph(0xE0EF, L"+", Px(px + 69), Px(1019), 24.45f, T.text, band);
            if (note) Text(note, Px(notePx), 661, 120, 40, 18.6f, T.text);
            Text(label, Px(labelPx), 660, 300, 40, 22.9f, T.text);
            AddHit(Px(px) - 14, 658, Px(labelPx - px) + TextWidth(label, 22.9f) + 18, 46, [] { ConsoleOnly(); });
        };
        pair(619.5f, nullptr, 0, L"El mando no responde", 724);
        pair(1139.5f, L"(mantener)", 1248, L"Buscar mandos", 1404);
    }
    DrawHints({{L"B", L"Atrás", Btn::B}}, 1, true);
}

// The page's right column: the console's five rows (anim.mkv 265.5 s,
// walkthrough.mp4 82.5 s). Only the first does something here.
struct PadsRow {
    const wchar_t* line1;
    const wchar_t* line2;
    float width; // of the longer line on the console, in pixels: its long labels are set smaller to fit
};
const PadsRow kPadsRows[] = {{L"Cambiar el orden", L"o el modo de sujeción", 327},
                             {L"Buscar mandos", nullptr, 224},
                             {L"Cómo sincronizar mandos nuevos", nullptr, 431},
                             {L"Cómo usar los botones GL y GR", nullptr, 402},
                             {L"Cómo usar el modo ratón", nullptr, 375}};

int PadsRowCount() {
    return cfg.identical ? 5 : 1;
}

// Which of the players' pads the emulator has no entry for (they have to be
// set up there once). Worked out again only when the pads or their order change.
void PadsEmuNote(const std::vector<pads::Pad>& all) {
    std::string key;
    for (const auto& p : all) key += p.id + "=" + std::to_string(p.player) + ";";
    if (key == padsUi.noteKey) return;
    padsUi.noteKey = key;
    padsUi.unknown.clear();
    PadsUnknown(padsUi.unknown, padsUi.emuName);
}

void DrawPadsS2() {
    Theme T = Th();
    Header(4, L"Mandos");
    const Color rule = T.line;
    // The console's controller pictures are filled with the focus grey (3B3B3B, anim.mkv
    // 265.5 s). The light theme's page is in no capture: its lights are the footer
    // mark's (w5 2.67 s), the bodies take the same theme colour as in the dark one.
    const Color body = T.focusBg, edge = cfg.dark ? rgb(0x8A8A8A) : rgb(0x8A888B);
    const float mid = 428.67f; // centre of the left column (643 px)
    const float titleSize = 18.1f;
    const std::vector<pads::Pad> all = PadsNow();

    // Keyboard and mouse: always there, where the console shows itself. Its
    // lights are lit when it is one of the players.
    Text(L"Teclado y ratón", mid - 200, 114, 400, 40, titleSize, T.sub, Center);
    if (!all.empty() && all.back().player > 0) PadLights(mid, 164.33f, all.back().player);
    KeyboardPicture(mid, 224, body, edge);
    Line(90.67f, 367.0f, 768.0f, 367.0f, rule, 0.667f);

    // Connected controllers in player order, each under its lights (rows 695
    // to 709), its battery mark centred on row 885 where the pad reports one.
    Text(L"Mandos", mid - 200, 394, 400, 40, titleSize, T.sub, Center);
    int shown = 0;
    for (const auto& p : all) {
        if (p.kind == pads::Kind::Keyboard || shown >= 4) continue;
        float cx = 199.33f + shown * 152.0f;
        PadLights(cx, 463.33f, p.player);
        PadShape(p.kind, cx, 523.0f, body, edge);
        if (p.battery >= 0 && p.battery2 >= 0) {
            PadBattery(cx - Px(35), Px(885.5f), p.battery, p.charging); // marks 70 px apart
            PadBattery(cx + Px(35), Px(885.5f), p.battery2, p.charging);
        } else if (p.battery >= 0) {
            PadBattery(cx, Px(885.5f), p.battery, p.charging);
        }
        shown++;
    }
    if (!shown) Text(L"No hay ningún mando conectado.", mid - 300, 505, 600, 40, 21.6f, T.sub, Center);
    PadsEmuNote(all);
    if (!padsUi.unknown.empty() && shown) {
        std::wstring who;
        for (size_t i = 0; i < padsUi.unknown.size(); i++)
            who += (i ? (i + 1 == padsUi.unknown.size() ? L" y " : L", ") : L"") + std::to_wstring(padsUi.unknown[i]);
        Text((padsUi.unknown.size() > 1 ? L"Para jugar con los mandos " : L"Para jugar con el mando ") + who +
                 (padsUi.unknown.size() > 1 ? L", configúralos una vez en " : L", configúralo una vez en ") + padsUi.emuName + L".",
             90.67f, 608, 677.33f, 30, 17.7f, T.sub, Center);
    }

    // On pixel column 1279, as on the capture: centred on a pixel edge the one-pixel
    // rule is shared between two columns and shows at two thirds of its colour.
    VRule(Px(1279), Px(140), Px(956), rule); // pixel column 1279, rows 140 to 955 (anim.mkv 265.5 s)

    // Rows 129 px apart from row 225, a rule under each from pixel 1320 to
    // 1807, labels from pixel 1345; the cursor's outside edge is 1306-1821.
    const int count = PadsRowCount();
    padsUi.sel = (std::min)(padsUi.sel, count - 1);
    for (int i = 0; i < count; i++) {
        const PadsRow& r = kPadsRows[i];
        const float top = Px(225.0f + i * 129.0f), h = Px(129);
        if (i == padsUi.sel) {
            RRect(Px(1309), top - Px(2), Px(509), h + Px(4), 6, T.focusBg);
            CursorFill(T.focusBg);
            Cursor(Px(1309), top - Px(2), Px(509), h + Px(4), 6);
        } else if (i + 1 != padsUi.sel) {
            HRule(Px(1320), Px(1808), top + h, rule);
        }
        // The size that gives the label the width it has on the console.
        const wchar_t* longest = r.line2 ? r.line2 : r.line1;
        const float size = 23.4f * Px(r.width) / TextWidth(longest, 23.4f);
        if (r.line2) {
            Text(r.line1, Px(1344), top + h / 2 - Px(23.5f) - 20, 340, 40, size, T.text); // lines 47 px apart
            Text(r.line2, Px(1344), top + h / 2 + Px(23.5f) - 20, 340, 40, size, T.text);
        } else {
            Text(r.line1, Px(1344), top + h / 2 - 20, 340, 40, size, T.text);
        }
        AddHit(Px(1311), top, Px(506), h, [i] {
            padsUi.sel = i;
            OnButtonImpl(Btn::A);
        });
    }
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}});
}

void DrawPads() {
    // Left by HOME with the order screen up: it does not come back by itself.
    if (padsUi.order && padsUi.drawnAt >= 0 && now - padsUi.drawnAt > 0.5) PadsCloseOrder(false);
    padsUi.drawnAt = now;
    if (padsUi.order) {
        DrawPadsOrder();
        return;
    }
    if (S2()) {
        DrawPadsS2();
        return;
    }
    Theme T = Th();
    Header(4, L"Mandos");
    const std::vector<pads::Pad> all = PadsNow();
    int connected = 0;
    for (int i = 0; i < 4; i++) {
        float x = 150 + i * 250.0f, y = 230;
        const pads::Pad* pad = nullptr;
        for (const auto& p : all)
            if (p.player == i + 1) pad = &p;
        bool on = pad != nullptr;
        connected += on && pad->kind != pads::Kind::Keyboard;
        Color c = on ? T.text : T.sep;
        RRectStroke(x, y, 200, 130, 46, c, 4);
        Line(x + 42, y + 64, x + 78, y + 64, c, 5);
        Line(x + 60, y + 46, x + 60, y + 82, c, 5);
        Circle(x + 150, y + 52, 9, c);
        Circle(x + 132, y + 78, 9, c);
        // Player lights
        for (int k = 0; k < 4; k++) Rect(x + 74 + k * 14.0f, y + 150, 10, 10, on && k <= i ? rgb(0x7CE000) : T.sep);
        Text(on ? PadName(pad->kind) : L"Sin mando", x, y + 176, 200, 30, 19, on ? T.text : T.sub, Center);
    }
    Text(connected ? L"Mandos listos para jugar."
         : OnAndroid() ? L"Conecta un mando. También puedes usar la pantalla táctil." // [android]
                       : L"Conecta un mando. También puedes usar el teclado y el ratón.",
         0, 500, W, 36, 21, T.sub, Center);
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Cambiar el orden", Btn::A}});
}

void PadsButton(Btn b) {
    if (padsUi.order) {
        if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            PadsCloseOrder(false);
        } else if (b == Btn::A) {
            PlaySfx(Sfx::Ok);
            PadsCloseOrder(true);
        } else if ((b == Btn::L || b == Btn::R) && !pads::HoldingShoulder()) {
            // No pad is holding L or R, so this came from the keyboard
            // (Re Pág / Av Pág): both within half a second is its L+R.
            (b == Btn::L ? padsUi.keyL : padsUi.keyR) = now;
            if (fabs(padsUi.keyL - padsUi.keyR) < 0.5) PadsPick("keyboard");
        } else if ((b == Btn::Plus || b == Btn::Minus) && cfg.identical) {
            ConsoleOnly();
        } else if (b == Btn::Y && pads::DebugFaking()) {
            pads::DebugChord(padsUi.debugNext++); // test states: L+R on the next made-up pad
        }
        return;
    }
    const int count = S2() ? PadsRowCount() : 1;
    if (b == Btn::B) {
        PlaySfx(Sfx::Back);
        Pop();
    } else if (b == Btn::A) {
        if (!S2() || padsUi.sel == 0) {
            PlaySfx(Sfx::Ok);
            PadsOpenOrder();
        } else {
            ConsoleOnly();
        }
    } else if (b == Btn::Up || b == Btn::Down) {
        const int to = padsUi.sel + (b == Btn::Down ? 1 : -1);
        if (to < 0 || to >= count) {
            if (S2() && heldRepeat) return;
            BumpCursor(0, b == Btn::Down ? 1.0f : -1.0f);
            PlaySfx(Sfx::Bump);
        } else {
            padsUi.sel = to;
            PlaySfx(Sfx::Move);
        }
    }
}

bool DebugPads(const std::wstring& name) {
    if (name == L"pads-set" || name == L"pads-order" || name == L"pads-none" || name == L"pads-jc") {
        pads::DebugFake(name == L"pads-none" ? "" : name == L"pads-jc" ? "jcl jcr:50 pro" : "ds5:85 pro:50 jc:100 xbox");
        Push(Scr::Pads);
        if (name == L"pads-order") PadsOpenOrder();
        return true;
    }
    return false;
}

// ---- the user page ---------------------------------------------------------
//
// One block: the page itself in both styles, its sub-pages (the picture
// chooser and the "new user" steps), and the test states.

// What a user has played, most recent first. The console's history is in that
// order, not by time played (walkthrough.mp4 305 s: 85 h, 15 h, 20 h, 15 h,
// 460 h going down the list).
std::vector<std::pair<Played, Game*>> UserHistory(int user) {
    std::vector<std::pair<Played, Game*>> list;
    for (auto& g : games) {
        Played p = UserPlayed(user, g);
        if (!p.last.empty() || p.seconds) list.push_back({p, &g});
    }
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        if (a.first.last != b.first.last) return a.first.last > b.first.last; // ISO dates sort as text
        return a.first.seconds > b.first.seconds;
    });
    return list;
}

// The console's line under a game in the history. Seen in the capture: "un
// rato" under an hour, then whole hours (1, 2, 5, 8) and, from 10 up, steps
// of five (10, 15, 20, 30, 45, 85, 90, 95, 230, 460).
std::wstring PlayedLine(uint64_t seconds) {
    uint64_t h = seconds / 3600;
    if (h < 1) return L"He jugado un rato.";
    if (h == 1) return L"He jugado 1 hora o más.";
    if (h >= 10) h -= h % 5;
    return L"He jugado " + std::to_wstring(h) + L" horas o más.";
}

// The console prints the line in blue for the games played most. The capture
// has 45 hours in grey and 85 in blue and nothing in between, so the exact
// boundary is not known; 50 is a guess inside that gap.
constexpr uint64_t kPlayedBlue = 50 * 3600;

// Greedy word wrap, so that each line can be placed at the console's pitch.
std::vector<std::wstring> WrapLines(const std::wstring& sEs, float size, float w) {
    const std::wstring s = lang::Tr(sEs); // whole, before it is cut into lines
    lang::Verbatim pieces;                // which are measured as they are
    std::vector<std::wstring> out;
    std::wstring line;
    size_t at = 0;
    while (at <= s.size()) {
        size_t end = s.find(L'\n', at);
        if (end == std::wstring::npos) end = s.size();
        for (const lang::Word& wd : lang::Words(s.substr(at, end - at))) { // spaces, or between Chinese and Japanese characters
            std::wstring tried = line.empty() ? wd.text : line + (wd.space ? L" " : L"") + wd.text;
            if (!line.empty() && TextWidth(tried, size) > w) {
                out.push_back(line);
                line = wd.text;
            } else {
                line = tried;
            }
        }
        if (end < s.size()) {
            out.push_back(line);
            line.clear();
        }
        at = end + 1;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

// -- state --

enum class USub { None, Icon, NewName, NewIcon, NewDone, NewFrom };
USub uSub = USub::None;
int iconSel = 0;                     // picture chooser: 0-7 a disc, 8 the "from a file" row
std::wstring newName;                // new-user steps: the name typed so far
int newIndex = -1;                   // the user they created
bool newWantEntry = false;           // open the text entry as soon as the step is on screen
std::function<void(int)> addDone;    // who asked for the new user
std::vector<FreeProfile> newFrom;    // new-user steps: the emulators' profiles that are nobody's yet
int newFromSel = 0;                  // ... and the row chosen: 0 a new user, then one per profile
int userShown = 0;                   // the section whose contents are on screen (lags the cursor)
int userWanted = -1;
double userWantedAt = 0;
double userSecAt = -10;              // when the section's contents were (re)loaded
bool userTest = false;               // --shot / --shotseq run: never open a system dialog
std::wstring userTestText, userTestImage;
double userTypeAt = -1;

// The friends pages (ui_friends.inc, included further down): what the code
// before them needs.
std::wstring FriendCode(int user);
int FriendsOnline(int user);
bool FriendsSharing(int user);
void FriendsSearch(int user);
void FriendsToggleSharing(int user);
void FriendsNoteDisc(int user, int colour);
void FriendsCloseSub();
void FriendsOpenReceived(int user);
void FriendsOpenSent(int user);
std::wstring FriendsRequestsValue(int user, bool received);
std::wstring FriendsRequestsSub(int user);

// -- actions --

constexpr int kMaxUsers = 8; // what the HOME menu's row of pictures has room for

void ConsoleOnly() {
    Info(L"Esta función solo está disponible en la consola.");
}


// The emulator reads its profile list when it starts, so users stay as they
// are while a game is open.
bool UsersLocked() {
    if (runningId.empty()) return false;
    Info(L"Cierra el programa abierto antes de cambiar los usuarios.");
    return true;
}

std::wstring UserPickImage() {
    return userTest ? userTestImage : PickImage();
}

bool UserNameTaken(const std::wstring& name) {
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    };
    for (const auto& u : cfg.users)
        if (lower(u.name) == lower(name)) return true;
    return false;
}

void UserRename(int u) {
    if (UsersLocked()) return;
    entry.open = true;
    entry.title = L"Apodo";
    entry.text = cfg.users[u].name;
    entry.done = [u](const std::wstring& s) {
        if (!s.empty() && u < static_cast<int>(cfg.users.size()) && s != cfg.users[u].name)
            UserReport(RenameUser(u, s));
    };
}

void UserChooseIcon(int u) {
    if (UsersLocked()) return;
    pageUser = u;
    iconSel = 0;
    uSub = USub::Icon;
}

void UserDelete(int u) {
    if (UsersLocked()) return;
    if (cfg.users.size() <= 1) {
        UserReport("LAST_USER");
        return;
    }
    Ask(L"¿Quieres eliminar a " + UserNoun(u) +
            (OnAndroid() ? L"?\n\nDejará de aparecer en la consola." // [android] no saves are kept per user there
                         : L"?\n\nSus datos de guardado en el emulador no se borran."),
        {L"Cancelar", L"Eliminar"}, [u](int b) {
            if (b != 1 || u >= static_cast<int>(cfg.users.size())) return;
            std::string id = cfg.users[u].id;
            avatars[id].reset(); // a decoded picture keeps its file open, and the file is about to go
            std::string err = RemoveUser(u);
            if (err != "LAST_USER") {
                avatars.erase(id);
                homeUser = 0;
                pageUser = 0;
                // The page of a user who is gone has nothing to show.
                if (stack.back() == Scr::User) Pop();
            }
            UserReport(err);
        });
}

void OpenUserPage(int user) {
    FriendsCloseSub();
    pageUser = user;
    uSub = USub::None;
    addDone = nullptr;
    userSec = userRow = 0;
    userInRows = false;
    userScroll = 0;
    userGlide = Glide();
    userShown = 0;
    userSecAt = -10;
    Push(Scr::User);
}

constexpr size_t kNewFromRows = 5; // what the first step has room for under "a new user"

// The "new user" steps: (whose profile,) name, picture, done. `done` gets the new user's index,
// or -1 if the steps were left without creating one; by then the steps' page
// is gone and the screen that asked is on top again.
void StartAddUser(std::function<void(int newIndex)> done) {
    if (UsersLocked()) return;
    if (static_cast<int>(cfg.users.size()) >= kMaxUsers) {
        Info(L"No se pueden añadir más usuarios.");
        return;
    }
    addDone = std::move(done);
    newName.clear();
    newIndex = -1;
    iconSel = 0;
    // An emulator's own profiles only become users here, when asked for: with
    // any that are nobody's yet, the first step offers them beside a new user.
    newFrom = FreeProfiles();
    if (newFrom.size() > kNewFromRows) newFrom.resize(kNewFromRows);
    newFromSel = 0;
    newWantEntry = newFrom.empty();
    uSub = newFrom.empty() ? USub::NewName : USub::NewFrom;
    Push(Scr::User);
}

void EndAddUser(int idx) {
    auto leave = [idx] {
        if (stack.size() > 1 && stack.back() == Scr::User) stack.pop_back();
        uSub = USub::None;
        auto done = std::move(addDone);
        addDone = nullptr;
        if (done) done(idx);
    };
    if (S2()) {
        StartTransition(false, leave);
    } else {
        leave();
        fade = 1;
        Touch();
    }
}

bool HomePlusShown() {
    // The console's HOME menu has no such button, so identical mode leaves it out.
    return !cfg.identical && static_cast<int>(cfg.users.size()) < kMaxUsers;
}

void NewUserAskName() {
    entry.open = true;
    entry.title = L"Apodo del nuevo usuario";
    entry.text = newName;
    entry.done = [](const std::wstring& s) {
        if (s.empty()) return;
        if (UserNameTaken(s)) {
            UserReport("NAME_TAKEN");
            return;
        }
        newName = s;
        iconSel = static_cast<int>(cfg.users.size()) % AvatarColours(); // the colour a new user would get anyway
        uSub = USub::NewIcon;
    };
}

// The picture chooser's A: a disc, a file from the PC (framed first: B on
// the framing page leaves everything as it was), or the present picture framed again.
void IconApply(bool creating, const std::wstring& file, const PicFrame& frame);
void IconConfirm() {
    const bool creating = uSub == USub::NewIcon;
    if (!creating && iconSel == AvatarColours() + 1) {
        UserReframe(pageUser);
        return;
    }
    if (iconSel >= AvatarColours()) {
        const std::wstring file = UserPickImage();
        if (file.empty()) return;
        OpenAvatarFrame(creating ? -1 : pageUser, creating ? newName : cfg.users[pageUser].name, file, PicFrame(),
                        [creating, file](const PicFrame& f) { IconApply(creating, file, f); }, nullptr);
        return;
    }
    IconApply(creating, L"", PicFrame());
}

void IconApply(bool creating, const std::wstring& file, const PicFrame& frame) {
    int u = pageUser;
    if (creating) {
        std::string err = AddUser(newName);
        if (err == "NAME_TAKEN") {
            UserReport(err);
            uSub = USub::NewName;
            return;
        }
        u = newIndex = static_cast<int>(cfg.users.size()) - 1;
    }
    RefreshAvatar(u); // lets go of the old file before it is written over
    std::string err = file.empty() ? SetUserDisc(u, iconSel) : SetUserImageFramed(u, file, frame);
    FriendsNoteDisc(u, file.empty() ? iconSel : -1); // a friend sees the disc's colour, never the picture
    RefreshAvatar(u);
    uSub = creating ? USub::NewDone : USub::None;
    UserReport(err);
}

// -- the page's sections and rows --

struct USection {
    std::string id;
    std::wstring name, sub;
    bool ruleAfter = false;
};

// In identical mode the console's five, in its order; otherwise the two that
// do something here.
std::vector<USection> UserSections() {
    std::vector<USection> out;
    for (const auto& s : console::UserPage().sections) {
        std::string id = s.id;
        // (The friends sections do something here, so they stay outside identical mode.)
        if (!cfg.identical && id != "profile" && id != "user_settings" && id != "friends" && id != "add_friend") continue;
        USection u;
        u.id = id;
        u.name = s.name;
        if (id == "friends") u.sub = L"En línea: " + std::to_wstring(FriendsOnline(pageUser));
        if (id == "add_friend") u.sub = FriendsRequestsSub(pageUser); // requests waiting
        // The console rules off "Perfil" and the friends group.
        u.ruleAfter = id == "profile" || id == "add_friend";
        out.push_back(u);
    }
    return out;
}

struct UNote {
    std::wstring text;
    int bullet = 0; // 1: a "◆" line, 2: its continuation
};
struct URow {
    enum Kind { Head, Item, Para, Small, Gap } kind = Item;
    std::wstring label, value;
    bool picture = false;            // the value is the user's picture
    std::vector<std::wstring> lines; // Para: the text, wrapped
    std::vector<UNote> notes;        // the grey explanation under the row
    std::function<void()> act;
    float y = 0, h = 0;
    bool Focusable() const { return kind == Item; }
};

// Measured on walkthrough.mp4, 314.7-320.6 s, in layout units (px / 1.5):
// rows every 113 px; a heading takes 64 px, after 65 px of space unless it is
// the first thing in the list; the blank row in "Añadir un amigo" is 83 px;
// a note's first line sits 31 px under the row with 40 px between lines and
// 66 px after the last; a paragraph's lines are 50 px apart.
constexpr float kURowH = 75.33f, kUHeadH = 42.67f, kUHeadGap = 43.33f, kUGap = 55.33f;
constexpr float kUNoteTop = 20.33f, kUNotePitch = 26.67f, kUNoteEnd = 44.33f;
constexpr float kUParaTop = 16, kUParaPitch = 33.33f, kUParaNote = 35.33f, kUParaEnd = 11.33f;
constexpr float kULabel = 22, kUValue = 21, kUHead = 19.5f, kUNote = 17.6f;
constexpr float kUTextW = 651; // labels and notes run from x 728 to 1704 px

std::vector<UNote> WrapNote(const std::wstring& noteEs) {
    const std::wstring note = lang::Tr(noteEs); // whole, before it is cut into lines
    std::vector<UNote> out;
    size_t at = 0;
    while (at < note.size()) {
        size_t end = note.find(L'\n', at);
        if (end == std::wstring::npos) end = note.size();
        std::wstring para = note.substr(at, end - at);
        bool bullet = para.rfind(L"◆ ", 0) == 0;
        if (bullet) para = para.substr(2);
        // A bullet's text starts 45 px further in than a plain note.
        auto lines = WrapLines(para, kUNote, kUTextW - (bullet ? 30.0f : 0.0f));
        for (size_t i = 0; i < lines.size(); i++) out.push_back({lines[i], bullet ? (i ? 2 : 1) : 0});
        at = end + 1;
    }
    return out;
}

std::vector<URow> UserRows(const std::string& section, int u) {
    std::vector<URow> rows;
    auto head = [&](const std::wstring& label) {
        URow r;
        r.kind = URow::Head;
        r.label = label;
        rows.push_back(r);
    };
    auto item = [&](const std::wstring& label, const std::wstring& value, std::function<void()> act,
                    const wchar_t* note = L"") {
        URow r;
        r.label = label;
        r.value = value;
        r.act = std::move(act);
        r.notes = WrapNote(note);
        rows.push_back(r);
    };
    auto icon = [&] {
        item(L"Icono", L"", [u] { UserChooseIcon(u); });
        rows.back().picture = true;
    };
    auto nickname = [&] { item(L"Apodo", cfg.users[u].name, [u] { UserRename(u); }); };
    auto remove = [&] { item(L"Eliminar usuario", L"", [u] { UserDelete(u); }); };
    auto ownCode = [&](const std::wstring& label) {
        URow r;
        r.kind = URow::Small;
        r.label = label + L" " + FriendCode(u); // NSW-Home's own code for the user
        rows.push_back(r);
    };
    // Not a console row: sharing through a public network is NSW-Home's, and
    // so is the switch that stops it.
    auto sharing = [&] {
        head(L"NSW-Home");
        item(L"Compartir mi actividad", FriendsSharing(u) ? L"Activado" : L"Desactivado", [u] { FriendsToggleSharing(u); },
             L"Quienes tengan tu clave de amigo ven tu apodo y el color de tu icono. Solo tus amigos ven a qué juegas y tu tiempo de juego.");
    };

    if (!cfg.identical) {
        if (section == "add_friend") {
            item(L"Solicitudes de amistad recibidas", FriendsRequestsValue(u, true), [u] { FriendsOpenReceived(u); });
            rows.push_back(URow());
            rows.back().kind = URow::Gap;
            item(L"Buscar por clave de amigo", L"", [u] { FriendsSearch(u); });
            item(L"Solicitudes de amistad enviadas", FriendsRequestsValue(u, false), [u] { FriendsOpenSent(u); });
            ownCode(L"Tu clave de amigo:");
            sharing();
            return rows;
        }
        if (section != "user_settings") return rows;
        head(L"Ajustes del perfil");
        icon();
        item(L"Ajustar imagen", L"", [u] { UserReframe(u); });
        nickname();
        head(L"Usuario");
        remove();
        return rows;
    }
    for (const auto& s : console::UserPage().sections) {
        if (section != s.id) continue;
        for (const auto& c : s.rows) {
            const std::string id = c.id;
            if (id == "add_friend.by_code") {
                URow gap;
                gap.kind = URow::Gap;
                rows.push_back(gap);
            }
            if (id == "user_settings.icon") {
                icon();
            } else if (id == "user_settings.nickname") {
                nickname();
            } else if (id == "add_friend.own_code") {
                ownCode(c.label);
            } else if (id == "add_friend.by_code") {
                item(c.label, c.value, [u] { FriendsSearch(u); }, c.note);
            } else if (id == "add_friend.received" || id == "add_friend.sent") {
                const bool received = id == "add_friend.received";
                item(c.label, FriendsRequestsValue(u, received), [u, received] { received ? FriendsOpenReceived(u) : FriendsOpenSent(u); }, c.note);
            } else if (c.kind == console::RowKind::Heading) {
                head(c.label);
            } else if (c.kind == console::RowKind::Text) {
                URow r;
                r.kind = URow::Para;
                r.lines = WrapLines(c.label, kULabel, kUTextW - 20);
                r.notes = WrapNote(c.note);
                rows.push_back(r);
            } else {
                // Everything else leads to a page of the console's that
                // nobody captured and that has no counterpart on a PC.
                item(c.label, c.value, [] { ConsoleOnly(); }, c.note);
            }
        }
    }
    if (section == "add_friend") sharing();
    if (section == "user_settings") {
        // Not on the console's page (it deletes users from its settings):
        // kept under a heading of its own, after everything the console lists.
        head(L"NSW-Home");
        remove();
    }
    return rows;
}

float LayoutURows(std::vector<URow>& rows) {
    float y = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        URow& r = rows[i];
        float notes = static_cast<float>(r.notes.size());
        r.y = y;
        switch (r.kind) {
        case URow::Head: r.h = (i ? kUHeadGap : 0.0f) + kUHeadH; break;
        case URow::Item: r.h = kURowH + (notes > 0 ? kUNoteTop + (notes - 1) * kUNotePitch + kUNoteEnd : 0.0f); break;
        case URow::Para:
            r.h = kUParaTop + (static_cast<float>(r.lines.size()) - 1) * kUParaPitch +
                  (notes > 0 ? kUParaNote + (notes - 1) * kUNotePitch : 0.0f) + kUParaEnd;
            break;
        case URow::Small: r.h = 40; break;
        case URow::Gap: r.h = kUGap; break;
        }
        y += r.h;
    }
    return y;
}

int NextURow(const std::vector<URow>& rows, int from, int d) {
    for (int i = from + d; i >= 0 && i < static_cast<int>(rows.size()); i += d)
        if (rows[i].Focusable()) return i;
    return -1;
}

void UserGoSection(int i) {
    userSec = i;
    userRow = 0;
    userInRows = false;
    userScroll = 0;
    userGlide = Glide();
}

// A filled disc with a letter, as gfx::MakeAvatar paints the ready-made pictures.
void DrawDisc(float cx, float cy, float r, Color c, const std::wstring& initial) {
    Circle(cx, cy, r, c);
    float d = r * 2;
    Text(initial, cx - r, cy - r - d * 8 / 256, d, d, d * 150 / 256, rgb(0xFFFFFF), Center, 600);
}

std::wstring InitialOf(const std::wstring& name) {
    return name.empty() ? L"" : std::wstring(1, static_cast<wchar_t>(towupper(name[0]))); // no nickname: the disc alone
}

void UserNotes(const std::vector<UNote>& notes, float x, float mid, Color c) {
    for (const auto& n : notes) {
        // The diamond is 13 px in from the text edge, its line 45 px.
        if (n.bullet == 1) Text(L"◆", x + 3, mid - 14, 20, 28, 11.5f, c);
        Text(n.text, x + (n.bullet ? 30.0f : 0.0f), mid - 15, kUTextW + 8, 30, kUNote, c);
        mid += kUNotePitch;
    }
}

// The rows of a list section ("Añadir un amigo", "Ajustes de usuario").
void DrawUserRows(std::vector<URow>& rows, int u, bool live) {
    Theme T = Th();
    const ListLay L = LL();
    const float total = LayoutURows(rows);
    const float bx = L.rowX - Px(11), bw = L.rowW + Px(22); // the outline, as on a row of Settings
    if (live) {
        if (userRow < 0 || userRow >= static_cast<int>(rows.size())) {
            userRow = (std::max)(0, NextURow(rows, -1, 1));
        } else if (!rows[userRow].Focusable()) {
            int next = NextURow(rows, userRow, 1);
            userRow = (std::max)(0, next >= 0 ? next : NextURow(rows, userRow, -1));
        }
        float target = userGlide.t0 < 0 ? userScroll : userGlide.to;
        if (!userInRows || rows.empty()) {
            target = 0;
        } else {
            const URow& r = rows[userRow];
            float view = L.bottom - L.rowY0 - 14;
            // A heading right above the row comes into view with it.
            float above = userRow > 0 && rows[userRow - 1].kind == URow::Head ? rows[userRow - 1].h : 0.0f;
            if (NextURow(rows, userRow, -1) < 0) above = r.y;
            if (r.y - above < target) target = r.y - above;
            if (r.y + r.h > target + view) target = r.y + r.h - view;
            if (NextURow(rows, userRow, 1) < 0) target = (std::max)(target, total - view);
            target = (std::max)(0.0f, target);
        }
        if (MouseOwns(&userScroll)) target = userScroll;
        GlideTo(userGlide, userScroll, target);
    }
    // Dragged, the focus moves into the rows, onto the nearest one wholly in view.
    std::vector<std::pair<float, float>> fit(rows.size(), {-1e9f, 0.0f});
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].Focusable()) fit[i] = {rows[i].y, rows[i].h};
    const float fitView = L.bottom - L.rowY0 - 14;
    const int userDrag = !live ? -1 :
        AddScroll(L.rowX - 14, Px(128), L.rowW + 28, Px(968) - Px(128), ScrollOf(userScroll, total - fitView, [fit, fitView](float v) {
            int to = NearestFit(userRow, static_cast<int>(fit.size()), [&](int i) {
                return fit[i].first - v >= -0.5f && fit[i].first + fit[i].second - v <= fitView + 0.5f;
            });
            if (to >= 0) {
                userRow = to;
                userInRows = true;
            }
        }));
    PushClip(L.rowX - 14, Px(128), L.rowW + 28, Px(968) - Px(128));
    // The focused row is drawn last: its outline lies over its neighbours' rules.
    const int onTop = live && userInRows && userRow >= 0 && userRow < static_cast<int>(rows.size()) ? userRow : -1;
    for (size_t k = 0; k < rows.size() + (onTop >= 0 ? 1 : 0); k++) {
        const size_t i = k < rows.size() ? k : static_cast<size_t>(onTop);
        if (k < rows.size() && static_cast<int>(i) == onTop) continue;
        const URow& r = rows[i];
        float ry = L.rowY0 + r.y - userScroll;
        if (ry > 660 || ry + r.h < 60) continue;
        int idx = static_cast<int>(i);
        switch (r.kind) {
        case URow::Head: {
            float top = ry + r.h - kUHeadH;
            // The bar: 6 x 35 px, from 53 px above the rule that follows (306 s: rows 533-567).
            Rect(L.rowX, Snap(ry + r.h) - Px(53), Px(6), Px(35), T.text);
            Text(r.label, L.rowX + 15.67f, top + 1, L.rowW - 20, 34, kUHead, T.text);
            break;
        }
        case URow::Item: {
            bool focused = live && userInRows && idx == userRow;
            const CursorRect frame = {bx, Snap(ry) - Px(2), bw, Px(118), L.radius};
            HRule(L.rowX, L.rowX + L.rowW, ry, T.sep);
            HRule(L.rowX, L.rowX + L.rowW, ry + kURowH, T.sep);
            if (!r.notes.empty()) HRule(L.rowX, L.rowX + L.rowW, ry + r.h, T.sep);
            if (focused) RRect(frame.x, frame.y, frame.w, frame.h, frame.r, T.focusBg);
            float vw = r.picture ? 60.0f : (std::min)(TextWidth(r.value, kUValue), 400.0f);
            Text(r.label, L.rowX + 16.33f, ry, L.rowW - vw - 50, kURowH, kULabel, T.text);
            // The picture is a 80 px disc centred 64 px from the row's right end.
            if (r.picture) DrawAvatar(u, L.rowX + L.rowW - 42.67f, ry + kURowH / 2, 26.67f);
            else Text(r.value, L.rowX + L.rowW - vw - 21, ry, vw + 6, kURowH, kUValue, T.accent, Right);
            UserNotes(r.notes, L.rowX + 16.33f, ry + kURowH + kUNoteTop, T.sub);
            if (focused) {
                CursorFill(T.focusBg);
                Cursor(frame.x, frame.y, frame.w, frame.h, frame.r);
            }
            if (live)
                AddHit(L.rowX, ry, L.rowW, kURowH, [idx] {
                    bool was = userInRows && userRow == idx;
                    userInRows = true;
                    userRow = idx;
                    if (was) OnButtonImpl(Btn::A);
                    else PlaySfx(Sfx::Move);
                });
            break;
        }
        case URow::Para: {
            float mid = ry + kUParaTop;
            for (const auto& line : r.lines) {
                Text(line, L.rowX + 16.33f, mid - 18, kUTextW + 8, 36, kULabel, T.text);
                mid += kUParaPitch;
            }
            UserNotes(r.notes, L.rowX + 16.33f, mid - kUParaPitch + kUParaNote, T.sub);
            break;
        }
        case URow::Small:
            Text(r.label, L.rowX, ry + 5, L.rowW - 16, 30, kUNote, T.sub, Right);
            break;
        case URow::Gap: break;
        }
    }
    PopClip();
    float view = L.bottom - L.rowY0;
    if (total > view) {
        float track = L.bottom - L.top - 16, len = (std::max)(30.0f, track * view / total);
        float maxScroll = (std::max)(1.0f, total - (view - 14));
        RRect(L.barX, L.top + 8 + (std::min)(1.0f, userScroll / maxScroll) * (track - len), 4, len, 2,
              cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7));
        AddScrollBar(userDrag, L.barX, L.top + 8, track, len, maxScroll);
    }
}

// "Invitaciones" and "Lista de amigos": nothing to list on a PC. Timed on
// walkthrough.mp4 310.5-312.3 s from the move onto "Invitaciones": the side
// stays empty for 33 frames, a turning ring (73 px across, centred on 1215,
// 547) shows until frame 96, and the sentence fades in from frame 102.
void DrawUserNotice(const std::string& section) {
    Theme T = Th();
    const float cx = 810.33f, cy = 364.67f;
    float f = static_cast<float>(now - userSecAt) * 60.0f;
    auto ramp = [](float v) { return (std::max)(0.0f, (std::min)(1.0f, v)); };
    float ring = ramp((f - 33) / 4) * (1 - ramp((f - 96) / 6));
    float text = ramp((f - 102) / 6);
    if (text < 1) Touch();
    if (ring > 0) {
        float spin = static_cast<float>(fmod(now * 360.0, 360.0));
        CircleStroke(cx, cy, 22.8f, alpha(T.sub, 0.25f * ring), 2.6f);
        Arc(cx, cy, 22.8f, spin, spin + 110, alpha(T.sub, ring), 2.6f);
    }
    if (text > 0) {
        // The console's own sentence for invitations. Its wording for an empty
        // friend list was not captured (the owner has friends), so a plain line.
        std::wstring line = L"No hay ningún amigo registrado.";
        if (section == "invitations")
            for (const auto& s : console::UserPage().sections)
                if (section == s.id && !s.rows.empty()) line = s.rows[0].label;
        Text(line, cx - 400, cy - 20, 800, 40, 19.9f, alpha(cfg.dark ? T.sub : rgb(0x757374), text), Center); // (117, 115, 116)
    }
}

// The profile: a card, then the history. Laid out from walkthrough.mp4 (light
// theme, 305.0-306.3 s): the card between rules at y 192 and 405 px, the
// friend-code line at 439, the "history" heading at 549 with a round "?" at
// its right end, then one row per game every 138.75 px with a 112 px picture,
// the name and a line under it.
// (Pixel rows: the card's rules on 192 and 405, the history's on 586 and every 139 after.)
constexpr float kUserCardY = 128, kUserCardH = 142, kUserHeadY = 355, kUserRowsY = Px(586), kUserRowH = Px(139);

void DrawUserProfile(int u, bool live) {
    Theme T = Th();
    const ListLay L = LL();
    const User& user = cfg.users[u];
    auto list = UserHistory(u);
    const float bx = L.rowX - Px(11), bw = L.rowW + Px(22);
    if (live) {
        userRow = (std::max)(0, (std::min)(userRow, static_cast<int>(list.size())));
        // Scroll: the card needs none; a history row lower than y 532 px is
        // brought up to there (measured on the second row: 193 px).
        float target = userGlide.t0 < 0 ? userScroll : userGlide.to;
        if (!userInRows || userRow == 0) {
            target = 0;
        } else {
            float top = kUserRowsY + (userRow - 1) * kUserRowH - target;
            if (top > 354.67f) target = kUserRowsY + (userRow - 1) * kUserRowH - 354.67f;
            if (top < L.top + 8) target = kUserRowsY + (userRow - 1) * kUserRowH - (L.top + 8);
        }
        if (MouseOwns(&userScroll)) target = userScroll;
        GlideTo(userGlide, userScroll, target);
    }
    // Dragged, the focus keeps to a history row whose top is where the page's
    // own scrolling lets one stand; with the list at its top, the card.
    const int nh = static_cast<int>(list.size());
    const int profDrag = !live ? -1 :
        AddScroll(L.rowX - 14, Px(128), L.rowW + 28, Px(968) - Px(128),
                  ScrollOf(userScroll, kUserRowsY + (nh - 1) * kUserRowH - 354.67f, [L, nh](float v) {
                      if (v < 0.5f && userRow == 0) return;
                      int r = NearestFit(userRow - 1, nh, [&](int i) {
                          const float top = kUserRowsY + i * kUserRowH - v;
                          return top >= L.top + 7.5f && top <= 355.17f;
                      });
                      userRow = r + 1; // none in reach: the card (-1 + 1)
                      userInRows = true;
                  }));
    const Color grey = cfg.dark ? T.sub : rgb(0x636162); // the pencil and the "?" (99, 97, 98 in the capture)

    PushClip(L.rowX - 14, Px(128), L.rowW + 28, Px(968) - Px(128));
    float cy = kUserCardY - userScroll;
    bool cardFocus = live && userInRows && userRow == 0;
    // An outline reaches 5 px above the rule over its row and 6 below the one
    // under it (308 s: rows 613 to 762 round a history row of 618 to 757).
    const CursorRect cardFrame = {bx, Snap(cy) - Px(2), bw, kUserCardH + Px(5), L.radius};
    HRule(L.rowX, L.rowX + L.rowW, cy, T.sep);
    HRule(L.rowX, L.rowX + L.rowW, cy + kUserCardH, T.sep);
    if (cardFocus) RRect(cardFrame.x, cardFrame.y, cardFrame.w, cardFrame.h, cardFrame.r, T.focusBg);
    DrawAvatar(u, 541, cy + 71.5f, 56);
    uint64_t total = 0;
    for (auto& e : list) total += e.first.seconds;
    Text(user.name, 611, cy + 30, 480, 46, 32, T.text);
    // Where the console has the online status, the one figure NSW-Home has.
    Text(list.empty() ? std::wstring(L"Todavía no hay actividad.") : L"Tiempo de juego: " + PlayTime(total), 611,
         cy + 80, 480, 32, 21, T.sub);
    {
        // The pencil that says the card leads to the profile's settings:
        // 44 px, centred on (1674, 297), pointing down to the left.
        float px = 1116, py = cy + 70;
        PushRotate(45, px, py);
        RRect(px - 5, py - 17.5f, 10, 5.5f, 1.4f, grey);
        Rect(px - 5, py - 10.5f, 10, 19, grey);
        Triangle(px - 5, py + 9.5f, px + 5, py + 9.5f, px, py + 18, grey);
        PopTransform();
    }
    if (cardFocus) {
        CursorFill(T.focusBg);
        Cursor(cardFrame.x, cardFrame.y, cardFrame.w, cardFrame.h, cardFrame.r);
    }
    if (live)
        AddHit(bx, cy, bw, kUserCardH, [] {
            bool was = userInRows && userRow == 0;
            userInRows = true;
            userRow = 0;
            if (was) OnButtonImpl(Btn::A);
        });

    // The friend code: NSW-Home's own for this user (the console's is not copied).
    Text(L"Clave de amigo:  " + FriendCode(u), L.rowX + 17, cy + 148.67f, 500, 30, kUHead, T.text);

    float hy = kUserHeadY - userScroll;
    Rect(L.rowX, Snap(kUserRowsY - userScroll) - Px(53), Px(6), Px(35), T.text); // 6 x 35 px, rows 533-567
    Text(L"Historial de programas usados", L.rowX + 15, hy - 6, L.rowW - 80, 34, kUHead, T.text);
    if (cfg.identical) {
        // 51 px ring centred on (1671.5, 549.5).
        CircleStroke(1114.33f, hy + 11.33f, 16.33f, grey, 1.33f);
        Text(L"?", 1114.33f - 15, hy + 11.33f - 15, 30, 30, 21, grey, Center);
    }
    HRule(L.rowX, L.rowX + L.rowW, kUserRowsY - userScroll, T.sep);
    uint64_t most = 0;
    for (auto& e : list) most = (std::max)(most, e.first.seconds);
    // (The focused row last, so that its outline lies over the rules.)
    const int n = static_cast<int>(list.size()), onTop = live && userInRows && userRow >= 1 && userRow <= n ? userRow - 1 : -1;
    for (int k = 0; k < n + (onTop >= 0 ? 1 : 0); k++) {
        const int i = k < n ? k : onTop;
        if (k < n && i == onTop) continue;
        float ry = kUserRowsY + i * kUserRowH - userScroll;
        if (ry > 660 || ry + kUserRowH < 60) continue;
        Game& g = *list[i].second;
        const Played& p = list[i].first;
        bool focused = live && userInRows && userRow == i + 1;
        const CursorRect frame = {bx, Snap(ry) - Px(2), bw, kUserRowH + Px(5), L.radius};
        HRule(L.rowX, L.rowX + L.rowW, ry + kUserRowH, T.sep);
        if (focused) RRect(frame.x, frame.y, frame.w, frame.h, frame.r, T.focusBg);
        // The picture: 112 px square from 14 px under the rule, 24 px inside the row.
        // Its corners are square there, unlike a tile's.
        if (!g.icon.empty() && !g.icon.failed) Image(g.icon, L.rowX + 16, Snap(ry) + Px(14), Px(112), Px(112));
        else DrawTile(g, L.rowX + 16, Snap(ry) + Px(14), Px(112));
        Text(g.name, 582, ry + 12, 550, 36, 24.5f, T.text);
        // The line under the name. Identical mode words it as the console
        // does; otherwise the exact time and the day of the last session.
        if (cfg.identical)
            Text(PlayedLine(p.seconds), 582, ry + 50, 550, 28, 17.8f, p.seconds >= kPlayedBlue ? T.accent : T.sub);
        else
            Text(L"Tiempo de juego: " + PlayTime(p.seconds) + L"  ·  " + DateOf(p.last), 582, ry + 50, 550, 28, 17.8f,
                 p.seconds == most ? T.accent : T.sub);
        if (focused) {
            CursorFill(T.focusBg);
            Cursor(frame.x, frame.y, frame.w, frame.h, frame.r);
        }
        if (live)
            AddHit(L.rowX, ry, L.rowW, kUserRowH, [i] {
                bool was = userInRows && userRow == i + 1;
                userInRows = true;
                userRow = i + 1;
                if (was) OnButtonImpl(Btn::A);
                else PlaySfx(Sfx::Move);
            });
    }
    if (list.empty())
        Text(L"Aquí aparecerán los programas que uses.", L.rowX + 16, kUserRowsY - userScroll + 18, L.rowW, 34, 19.4f,
             T.sub);
    PopClip();
    float content = kUserRowsY + list.size() * kUserRowH - L.top, view = L.bottom - L.top;
    if (content > view) {
        float track = view - 16, len = (std::max)(30.0f, track * view / content);
        float maxScroll = (std::max)(1.0f, kUserRowsY + (static_cast<float>(list.size()) - 1) * kUserRowH - 354.67f);
        // (On pixel columns 1881-1886 from row 139, as in Settings.)
        RRect(1254, Snap(Px(139) + (std::min)(1.0f, userScroll / maxScroll) * (track - len)), 4, Snap(len), 2,
              cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7));
        AddScrollBar(profDrag, 1254, Px(139), track, len, maxScroll);
    }
}

// What a history row tells when chosen: the figures the console's line rounds away.
void UserHistoryInfo(const Played& p, const Game& g) {
    std::wstring time = p.seconds < 60 ? std::wstring(L"menos de 1 min") : PlayTime(p.seconds);
    Info(g.name + L"\n\nTiempo de juego: " + time + L"\nÚltima partida: " + DateOf(p.last));
}

// The page's own header: the user's picture where other pages have their icon.
void UserHeader(int user, const std::wstring& title) {
    Theme T = Th();
    if (!S2()) {
        Header(7, title);
        return;
    }
    if (user >= 0) {
        DrawAvatar(user, 107, 48, 27);
        Text(title, 150, 28, 900, 46, 28.6f, T.text);
        HRule(48, 1232, Px(127), T.line);
    } else {
        Header(7, title);
    }
}

// The picture chooser, for an existing user and as the second "new user"
// step. The console's own chooser was not captured and its pictures are not
// NSW-Home's to offer: this page is NSW-Home's, with the discs it paints itself.
constexpr int kIconCols = 4;
void DrawUserIcons() {
    Theme T = Th();
    const ListLay L = LL();
    const bool creating = uSub == USub::NewIcon;
    const int n = AvatarColours();
    const std::wstring& name = creating ? newName : cfg.users[pageUser].name;
    UserHeader(creating ? -1 : pageUser, creating ? L"Añadir usuario" : name.empty() ? L"Icono" : L"Icono de " + name);
    if (S2()) Line(L.divX, L.top + 6, L.divX, 632, T.sep, 1);

    // Left: what the user would look like.
    const float px = 258, py = 300;
    Circle(px, py, 100, T.avRing);
    if (iconSel < n) DrawDisc(px, py, 94, rgb(AvatarColour(iconSel)), InitialOf(name));
    else if (!creating) DrawAvatar(pageUser, px, py, 94);
    else DrawDisc(px, py, 94, rgb(0x8C8C8C), InitialOf(name));
    Text(name, px - 160, py + 116, 320, 44, 28, T.text, Center);

    Rect(L.rowX + 0.33f, L.rowY0 + 7.33f, 3.33f, 23.33f, T.text);
    Text(L"Elige un icono", L.rowX + 15.67f, L.rowY0 + 1, L.rowW, 34, kUHead, T.text);
    for (int i = 0; i < n; i++) {
        float cx = L.rowX + 86 + (i % kIconCols) * 170.0f, cy = 252 + (i / kIconCols) * 132.0f;
        DrawDisc(cx, cy, 50, rgb(AvatarColour(i)), InitialOf(name));
        if (i == iconSel) CircleCursor(cx, cy, 58);
        AddHit(cx - 58, cy - 58, 116, 116, [i] {
            bool was = iconSel == i;
            iconSel = i;
            if (was) OnButtonImpl(Btn::A);
            else PlaySfx(Sfx::Move);
        });
    }
    float ry = 470;
    bool focused = iconSel == n;
    Line(L.rowX, ry + 0.5f, L.rowX + L.rowW, ry + 0.5f, T.sep, 1);
    Line(L.rowX, ry + kURowH + 0.5f, L.rowX + L.rowW, ry + kURowH + 0.5f, T.sep, 1);
    if (focused && S2()) RRect(L.rowX - 7, ry - 2, L.rowW + 13.67f, kURowH + 5, L.radius, T.focusBg);
    Text(L"Usar una imagen del PC", L.rowX + 16.33f, ry, L.rowW - 40, kURowH, kULabel, T.text);
    if (focused) {
        CursorFill(T.focusBg);
        Cursor(L.rowX - 7, ry - 2, L.rowW + 13.67f, kURowH + 5, L.radius);
    }
    AddHit(L.rowX, ry, L.rowW, kURowH, [n] {
        bool was = iconSel == n;
        iconSel = n;
        if (was) OnButtonImpl(Btn::A);
        else PlaySfx(Sfx::Move);
    });
    if (!creating) {
        // The present picture, framed again (rotated, zoomed, moved).
        ry += kURowH;
        focused = iconSel == n + 1;
        Line(L.rowX, ry + kURowH + 0.5f, L.rowX + L.rowW, ry + kURowH + 0.5f, T.sep, 1);
        if (focused && S2()) RRect(L.rowX - 7, ry - 2, L.rowW + 13.67f, kURowH + 5, L.radius, T.focusBg);
        Text(L"Ajustar imagen", L.rowX + 16.33f, ry, L.rowW - 40, kURowH, kULabel, T.text);
        if (focused) {
            CursorFill(T.focusBg);
            Cursor(L.rowX - 7, ry - 2, L.rowW + 13.67f, kURowH + 5, L.radius);
        }
        AddHit(L.rowX, ry, L.rowW, kURowH, [n] {
            bool was = iconSel == n + 1;
            iconSel = n + 1;
            if (was) OnButtonImpl(Btn::A);
            else PlaySfx(Sfx::Move);
        });
    }
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", S2() ? L"Confirmar" : L"Aceptar", Btn::A}});
}

// First "new user" step: the name. The text entry comes up by itself; this is
// what is behind it, and what is left if the entry is cancelled.
void DrawNewUserName() {
    Theme T = Th();
    UserHeader(-1, L"Añadir usuario");
    if (newWantEntry && !trans.on) {
        newWantEntry = false;
        NewUserAskName();
    }
    DrawDisc(W / 2, 236, 76, rgb(0x8C8C8C), InitialOf(newName));
    Text(L"Escribe un apodo para el nuevo usuario.", 0, 344, W, 40, 21.6f, T.text, Center);
    const float x = 298.5f, w = 683, ry = 420;
    Line(x, ry + 0.5f, x + w, ry + 0.5f, T.sep, 1);
    Line(x, ry + kURowH + 0.5f, x + w, ry + kURowH + 0.5f, T.sep, 1);
    if (S2()) RRect(x - 7, ry - 2, w + 13.67f, kURowH + 5, 8, T.focusBg);
    Text(L"Apodo", x + 16.33f, ry, 200, kURowH, kULabel, T.text);
    Text(newName, x + w - 421, ry, 400, kURowH, kUValue, T.accent, Right);
    CursorFill(T.focusBg);
    Cursor(x - 7, ry - 2, w + 13.67f, kURowH + 5, 8);
    AddHit(x, ry, w, kURowH, [] { OnButtonImpl(Btn::A); });
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", S2() ? L"Confirmar" : L"Aceptar", Btn::A}});
}

// The step before the name, when an emulator has profiles that are nobody's:
// a new user, or one of those, who comes with their saves. NSW-Home's page
// (the console has nothing like it), in the look of the name step.
void DrawNewUserFrom() {
    Theme T = Th();
    UserHeader(-1, L"Añadir usuario");
    Text(L"Crea un usuario nuevo o añade un perfil que ya existe en un emulador.", 0, 124, W, 40, 21.6f, T.text, Center);
    Text(L"Un perfil de un emulador conserva sus datos de guardado.", 0, 158, W, 32, kUNote, T.sub, Center);
    const float x = 298.5f, w = 683;
    const int n = static_cast<int>(newFrom.size()) + 1;
    newFromSel = (std::max)(0, (std::min)(newFromSel, n - 1));
    for (int i = 0; i < n; i++) {
        const float ry = 208 + i * kURowH;
        Line(x, ry + 0.5f, x + w, ry + 0.5f, T.sep, 1);
        Line(x, ry + kURowH + 0.5f, x + w, ry + kURowH + 0.5f, T.sep, 1);
        if (i == newFromSel && S2()) RRect(x - 7, ry - 2, w + 13.67f, kURowH + 5, 8, T.focusBg);
        Text(i == 0 ? std::wstring(L"Crear un usuario nuevo") : newFrom[i - 1].name, x + 16.33f, ry, 380, kURowH, kULabel, T.text);
        if (i > 0) Text(L"Perfil de " + newFrom[i - 1].emus, x + w - 321, ry, 300, kURowH, kUNote, T.sub, Right);
        if (i == newFromSel) {
            CursorFill(T.focusBg);
            Cursor(x - 7, ry - 2, w + 13.67f, kURowH + 5, 8);
        }
        AddHit(x, ry, w, kURowH, [i] {
            const bool was = newFromSel == i;
            newFromSel = i;
            if (was) OnButtonImpl(Btn::A);
            else PlaySfx(Sfx::Move);
        });
    }
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", S2() ? L"Confirmar" : L"Aceptar", Btn::A}});
}

// Last "new user" step: the user as it will appear.
void DrawNewUserDone() {
    Theme T = Th();
    UserHeader(-1, L"Añadir usuario");
    if (newIndex < 0 || newIndex >= static_cast<int>(cfg.users.size())) return;
    Circle(W / 2, 250, 86, T.avRing);
    DrawAvatar(newIndex, W / 2, 250, 80);
    Text(cfg.users[newIndex].name, 0, 352, W, 46, 32, T.text, Center);
    Text(L"Se ha añadido el usuario.", 0, 408, W, 36, 21.6f, T.sub, Center);
    const float w = 360, x = (W - w) / 2, ry = 486, h = 62;
    RRect(x, ry, w, h, 8, S2() ? T.focusBg : T.btn);
    Text(L"Aceptar", x, ry, w, h, 22.5f, T.text, Center);
    CursorFill(S2() ? T.focusBg : T.btn);
    Cursor(x, ry, w, h, 8);
    AddHit(x, ry, w, h, [] { OnButtonImpl(Btn::A); });
    Footer({{L"A", S2() ? L"Confirmar" : L"Aceptar", Btn::A}});
}

// ---- the profile picture's framing page ("Ajustar imagen") ------------------
// Every place that takes a picture file for a user (the picture chooser, the
// HOME menu's user menu, the first-start assistant) shows this page first: the
// picture under the round mask of the user icon and the square it is saved
// as, to be moved, zoomed and turned. A saves the square (SetUserImageFramed,
// which keeps a copy of the original so the page can frame it again), B leaves
// the old picture as it was. NSW-Home's own page: the console has no such
// step (it only offers its own pictures). Mouse: drag the picture, the wheel
// zooms (AvatarFrameMouse / AvatarFrameWheel, called by OnMouse and OnWheel).

struct AvatarFramer {
    Img img;
    std::wstring file;
    int user = -1;                   // whose picture; -1 while the user is still being created
    std::wstring name;               // shown under the preview
    bool fromSquare = false;         // only the saved square was there to frame again
    PicFrame want, shown;            // where the buttons put it / what is on screen (eases after it)
    float wantTurn = 0, shownTurn = 0; // the turn in degrees, unwrapped so that it eases the short way
    float iw = 0, ih = 0;            // the picture's own size
    bool snap = true;                // the next frame shows `want` at once
    bool dragging = false;
    float dragX = 0, dragY = 0;
    PicFrame dragFrom;
    std::function<void(const PicFrame&)> save;
    std::function<void()> cancel;
} afr;

// Layout units. The viewport is the left part of the page under the header;
// the square is the size of the picture chooser's preview area, with room
// round it for the dimmed rest of the picture.
// Edges fall on whole pixels at 1920x1080 (units * 1.5), so that the dimmed
// bands meet without a seam.
constexpr float kAfVx = 48, kAfVy = 102, kAfVw = 716, kAfVh = 528, kAfSide = 400;
constexpr float kAfSx = kAfVx + (kAfVw - kAfSide) / 2, kAfSy = kAfVy + (kAfVh - kAfSide) / 2;
constexpr float kAfPx = 812, kAfPw = 420; // the right-hand column
constexpr float kAfStep = 0.025f;         // one press of a direction: a fortieth of the square
constexpr float kAfZoom = 1.15f;          // one press of L / R, one notch of the wheel

bool OpenAvatarFrame(int user, const std::wstring& name, const std::wstring& file, const PicFrame& start,
                     std::function<void(const PicFrame&)> save, std::function<void()> cancel) {
    afr.img.reset();
    afr.img = Img();
    afr.img.path = file;
    afr.img.maxSize = 1600; // sharp at the largest zoom the viewport shows
    afr.iw = afr.ih = 0;
    if (file.empty() || !ImageSize(afr.img, afr.iw, afr.ih) || afr.iw <= 0 || afr.ih <= 0) {
        afr.img.reset();
        Info(L"No se ha podido abrir la imagen.");
        return false;
    }
    afr.file = file;
    afr.user = user;
    afr.name = name;
    afr.fromSquare = user >= 0 && file == AvatarPath(cfg.users[user].id);
    afr.want = start;
    ClampPicFrame(afr.want, afr.iw, afr.ih);
    afr.shown = afr.want;
    afr.wantTurn = afr.shownTurn = 90.0f * afr.want.quarter;
    afr.snap = true;
    afr.dragging = false;
    afr.save = std::move(save);
    afr.cancel = std::move(cancel);
    Push(Scr::Frame);
    return true;
}

// The user's present picture, framed again: from the original kept with it,
// or, for a picture set before originals were kept (or a profile's own
// picture), from the saved square, which then becomes the kept original.
bool UserReframe(int u) {
    if (UsersLocked() || u < 0 || u >= static_cast<int>(cfg.users.size())) return false;
    const std::string id = cfg.users[u].id;
    std::wstring src = AvatarSource(id);
    PicFrame start;
    if (src.empty()) src = AvatarPath(id);
    else AvatarFrameOf(id, start);
    return OpenAvatarFrame(u, cfg.users[u].name, src, start, [u, src, id](const PicFrame& f) {
        if (u >= static_cast<int>(cfg.users.size()) || cfg.users[u].id != id) return;
        RefreshAvatar(u); // the picture on screen keeps its file open: let go of it first
        UserReport(SetUserImageFramed(u, src, f));
        RefreshAvatar(u);
        if (uSub == USub::Icon) uSub = USub::None; // from the chooser: back to the page, as a choice there does
    }, nullptr);
}

void AvatarFrameClose(bool saved) {
    auto save = std::move(afr.save);
    auto cancel = std::move(afr.cancel);
    afr.save = nullptr;
    afr.cancel = nullptr;
    afr.dragging = false;
    const PicFrame f = afr.want;
    afr.img.reset(); // the file is let go of before it is copied
    Pop();
    if (saved && save) save(f);
    else if (!saved && cancel) cancel();
}

// One press: true when the picture moved.
bool AvatarFrameMove(float dx, float dy) {
    PicFrame f = afr.want;
    f.panX += dx;
    f.panY += dy;
    ClampPicFrame(f, afr.iw, afr.ih);
    const bool moved = fabsf(f.panX - afr.want.panX) > 1e-5f || fabsf(f.panY - afr.want.panY) > 1e-5f;
    afr.want = f;
    return moved;
}

// Zooms about the middle of the square: what is there stays there.
bool AvatarFrameZoom(float factor) {
    PicFrame f = afr.want;
    f.zoom *= factor;
    ClampPicFrame(f, afr.iw, afr.ih);
    if (fabsf(f.zoom - afr.want.zoom) < 1e-4f) return false;
    const float k = f.zoom / afr.want.zoom;
    f.panX = afr.want.panX * k;
    f.panY = afr.want.panY * k;
    ClampPicFrame(f, afr.iw, afr.ih);
    afr.want = f;
    return true;
}

// A quarter turn about the middle of the square, clockwise for +1.
void AvatarFrameTurn(int dir) {
    PicFrame f = afr.want;
    f.quarter += dir;
    const float x = f.panX, y = f.panY;
    f.panX = dir > 0 ? -y : y;
    f.panY = dir > 0 ? x : -x;
    ClampPicFrame(f, afr.iw, afr.ih);
    afr.want = f;
    afr.wantTurn += 90.0f * dir;
}

void AvatarFrameButton(Btn b) {
    switch (b) {
    case Btn::Up:
    case Btn::Down:
    case Btn::Left:
    case Btn::Right: {
        const float dx = b == Btn::Left ? -kAfStep : b == Btn::Right ? kAfStep : 0.0f;
        const float dy = b == Btn::Up ? -kAfStep : b == Btn::Down ? kAfStep : 0.0f;
        if (AvatarFrameMove(dx, dy)) {
            if (!heldRepeat) PlaySfx(Sfx::Move);
        } else if (!heldRepeat) {
            PlaySfx(Sfx::Bump);
        }
        break;
    }
    case Btn::R:
    case Btn::L:
        PlaySfx(AvatarFrameZoom(b == Btn::R ? kAfZoom : 1 / kAfZoom) ? Sfx::Move : Sfx::Bump);
        break;
    case Btn::X:
    case Btn::Y:
        PlaySfx(Sfx::Move);
        AvatarFrameTurn(b == Btn::X ? 1 : -1);
        break;
    case Btn::Minus:
        PlaySfx(Sfx::Move);
        afr.want = PicFrame();
        afr.wantTurn = roundf(afr.wantTurn / 360.0f) * 360.0f; // back upright the short way
        break;
    case Btn::A:
        PlaySfx(Sfx::Ok);
        AvatarFrameClose(true);
        break;
    case Btn::B:
        PlaySfx(Sfx::Back);
        AvatarFrameClose(false);
        break;
    default: break;
    }
    Touch();
}

bool AvatarFrameLive() {
    return !stack.empty() && stack.back() == Scr::Frame && !dlg.open && !entry.open && !trans.on;
}

bool AvatarFrameMouse(float x, float y, int phase) {
    if (phase == 2 || !AvatarFrameLive()) {
        const bool was = afr.dragging;
        afr.dragging = false;
        return was && phase == 2;
    }
    if (phase == 0) {
        if (x < kAfVx || x >= kAfVx + kAfVw || y < kAfVy || y >= kAfVy + kAfVh) return false;
        afr.dragging = true;
        afr.dragX = x;
        afr.dragY = y;
        afr.dragFrom = afr.want;
        return true;
    }
    if (!afr.dragging) return false;
    PicFrame f = afr.dragFrom;
    f.panX += (x - afr.dragX) / kAfSide;
    f.panY += (y - afr.dragY) / kAfSide;
    f.quarter = afr.want.quarter;
    f.zoom = afr.want.zoom;
    ClampPicFrame(f, afr.iw, afr.ih);
    afr.want = afr.shown = f; // the picture stays under the pointer
    Touch();
    return true;
}

bool AvatarFrameWheel(float delta) {
    if (!AvatarFrameLive()) return false;
    AvatarFrameZoom(powf(kAfZoom, delta));
    Touch();
    return true;
}

// A button as the footer draws it, for the right-hand column's list.
void AvatarFrameGlyph(wchar_t g, float cx, float cy, Color ink, Color hole) {
    if (g == L'L' || g == L'R') {
        RRect(cx - 17, cy - 11, 34, 22, 11, ink);
        Text(std::wstring(1, g), cx - 17, cy - 12, 34, 24, 15, hole, Center, 700);
        return;
    }
    const wchar_t sym = g == L'X' ? 0xE0E2 : g == L'Y' ? 0xE0E3 : g == L'-' ? 0xE0F0 : 0;
    if (sym && Symbol(sym, cx, cy - 6.6f, 24.45f, ink)) return;
    Circle(cx, cy, 11.4f, ink);
    if (g == L'-') Rect(cx - 6, cy - 1.3f, 12, 2.6f, hole);
    else Text(std::wstring(1, g), cx - 12, cy - 11.5f, 24, 24, 16, hole, Center, 700);
}

void DrawAvatarFrame(float dt) {
    Theme T = Th();
    // Eases as the console's lists glide: most of the way in about 7 frames.
    const float k = afr.snap ? 1.0f : 1.0f - expf(-(std::max)(dt, 0.0f) / 0.04f);
    afr.snap = false;
    auto ease = [k](float& v, float to, float eps) {
        v += (to - v) * k;
        if (fabsf(to - v) < eps) v = to;
        return v != to;
    };
    bool moving = ease(afr.shown.panX, afr.want.panX, 1e-4f);
    moving = ease(afr.shown.panY, afr.want.panY, 1e-4f) || moving;
    moving = ease(afr.shown.zoom, afr.want.zoom, 1e-4f) || moving;
    moving = ease(afr.shownTurn, afr.wantTurn, 0.05f) || moving;
    if (moving) Touch();
    PicFrame f = afr.shown;
    f.quarter = afr.want.quarter;

    UserHeader(afr.user >= 0 && afr.user < static_cast<int>(cfg.users.size()) ? afr.user : -1, L"Ajustar imagen");

    // The picture, the part outside the square dimmed most, the corners the
    // round icon leaves out less.
    const float R = kAfSide / 2, cx = kAfSx + R, cy = kAfSy + R;
    PushClip(kAfVx, kAfVy, kAfVw, kAfVh);
    Rect(kAfVx, kAfVy, kAfVw, kAfVh, cfg.dark ? rgb(0x101010) : rgb(0xB9B6BA));
    ImageFramed(afr.img, kAfSx, kAfSy, kAfSide, f, 1.0f, afr.shownTurn);
    const Color outside = rgb(0x000000, 0.62f), corners = rgb(0x000000, 0.4f);
    Rect(kAfVx, kAfVy, kAfVw, kAfSy - kAfVy, outside);
    Rect(kAfVx, kAfSy + kAfSide, kAfVw, kAfVy + kAfVh - kAfSy - kAfSide, outside);
    Rect(kAfVx, kAfSy, kAfSx - kAfVx, kAfSide, outside);
    Rect(kAfSx + kAfSide, kAfSy, kAfVx + kAfVw - kAfSx - kAfSide, kAfSide, outside);
    PushClip(kAfSx, kAfSy, kAfSide, kAfSide);
    const float band = R * 0.42f + 4; // from the circle out past the square's corners (R * 0.414)
    CircleStroke(cx, cy, R + band / 2, corners, band);
    PopClip();
    RRectStroke(kAfSx, kAfSy, kAfSide, kAfSide, 0, rgb(0xFFFFFF, 0.55f), 1.5f);
    CircleStroke(cx, cy, R, rgb(0xFFFFFF, 0.9f), 2);
    PopClip();
    RRectStroke(kAfVx - 6, kAfVy - 6, kAfVw + 12, kAfVh + 12, 14, T.bg, 12); // rounds the viewport's corners (clear of the header rule)

    // Right: the icon as it will look, and what each button does.
    const float pr = 82, pcx = kAfPx + kAfPw / 2, pcy = 192;
    PushClip(pcx - pr, pcy - pr, pr * 2, pr * 2);
    ImageFramed(afr.img, pcx - pr, pcy - pr, pr * 2, f, 1.0f, afr.shownTurn);
    const float pband = pr * 0.42f + 4;
    CircleStroke(pcx, pcy, pr + pband / 2, T.bg, pband);
    PopClip();
    CircleStroke(pcx, pcy, pr + 3, S2() ? T.avRing : T.sep, 6);
    Text(afr.name, kAfPx, pcy + pr + 10, kAfPw, 38, 24, T.text, Center);
    TextWrap(afr.fromSquare ? L"La imagen original no se guardó: se ajusta el icono actual."
                            : L"Mueve la imagen con los botones de dirección o arrastrándola.",
             kAfPx + 10, pcy + pr + 50, kAfPw - 20, 52, 16.5f, T.sub, Center);

    static const struct {
        const wchar_t* label;
        wchar_t glyph;
        Btn btn;
    } rows[] = {{L"Girar a la izquierda", L'Y', Btn::Y},
                {L"Girar a la derecha", L'X', Btn::X},
                {L"Acercar", L'R', Btn::R},
                {L"Alejar", L'L', Btn::L},
                {L"Restablecer", L'-', Btn::Minus}};
    const float rh = 46, ry0 = 394;
    int i = 0;
    for (const auto& r : rows) {
        const float ry = ry0 + rh * i++;
        Line(kAfPx, ry + 0.5f, kAfPx + kAfPw, ry + 0.5f, T.sep, 1);
        Text(r.label, kAfPx + 16, ry, kAfPw - 80, rh, 20, T.text);
        AvatarFrameGlyph(r.glyph, kAfPx + kAfPw - 30, ry + rh / 2, T.text, T.bg);
        const Btn b = r.btn;
        AddHit(kAfPx, ry, kAfPw, rh, [b] { OnButtonImpl(b); });
    }
    Line(kAfPx, ry0 + rh * i + 0.5f, kAfPx + kAfPw, ry0 + rh * i + 0.5f, T.sep, 1);

    Footer({{L"Y", L"Girar a la izquierda", Btn::Y},
            {L"X", L"Girar a la derecha", Btn::X},
            {L"B", L"Cancelar", Btn::B},
            {L"A", L"Guardar", Btn::A}});
}

// Test states (--shot / --shotseq): "avatar-frame:<file>" opens the page on
// that picture for user 0, as the picture chooser does; "avatar-reframe"
// frames user 0's (or, after "u<n>+", that user's) present picture again. Saving writes the data folder's
// avatars\<id>.jpg (and the kept original beside it).
bool DebugAvatarFrame(const std::wstring& name) {
    for (const wchar_t* style : {L"-1", L"-1l", L"-2", L"-2l"}) {
        const size_t n = wcslen(style);
        if (name.size() > n && name.compare(name.size() - n, n, style) == 0) return false; // set first, then this comes back
    }
    if (name.rfind(L"avatar-frame:", 0) == 0) {
        const std::wstring file = name.substr(13);
        OpenUserPage(0);
        const bool opened = OpenAvatarFrame(0, cfg.users[0].name, file, PicFrame(), [file](const PicFrame& fr) {
            RefreshAvatar(0);
            UserReport(SetUserImageFramed(0, file, fr));
            RefreshAvatar(0);
        }, nullptr);
        trans = Transition(); // straight onto the page, over the user's page
        stack = {Scr::Home, Scr::User};
        if (opened) stack.push_back(Scr::Frame);
        return true;
    }
    if (name == L"avatar-reframe") {
        const int u = pageUser; // "u<n>+" picks another user
        OpenUserPage(u);
        const bool opened = UserReframe(u);
        trans = Transition();
        stack = {Scr::Home, Scr::User};
        if (opened) stack.push_back(Scr::Frame);
        return true;
    }
    return false;
}

// Test runs have no keyboard: the text a test state brought is "typed" three
// quarters of a second after an entry opens. Several texts, separated by '|',
// are used one entry after another.
void UserTestTyping() {
    if (!userTest || !entry.open || userTestText.empty()) {
        if (!entry.open) userTypeAt = -1;
        return;
    }
    Touch();
    if (userTypeAt < 0) userTypeAt = now;
    if (now - userTypeAt < 0.75) return;
    size_t bar = userTestText.find(L'|');
    entry.text = userTestText.substr(0, bar);
    userTestText = bar == std::wstring::npos ? std::wstring() : userTestText.substr(bar + 1);
    entry.open = false;
    userTypeAt = -1;
    auto fn = entry.done;
    if (fn) fn(entry.text);
}

bool DrawUserSub() {
    switch (uSub) {
    case USub::None: return false;
    case USub::Icon:
    case USub::NewIcon: DrawUserIcons(); break;
    case USub::NewName: DrawNewUserName(); break;
    case USub::NewDone: DrawNewUserDone(); break;
    case USub::NewFrom: DrawNewUserFrom(); break;
    }
    return true;
}

void UserButton(Btn b);

#include "ui_friends.inc"

void DrawUserS2() {
    Theme T = Th();
    const ListLay L = LL();
    if (pageUser >= static_cast<int>(cfg.users.size())) pageUser = 0;
    const std::vector<USection> secs = UserSections();
    const int count = static_cast<int>(secs.size());
    if (userSec >= count) UserGoSection(count - 1);

    // Test state "user-move": Down every half second, from frame 0 of a --shotseq.
    if (userScript > 0) {
        if (userScriptAt < 0) userScriptAt = now;
        int due = static_cast<int>(floor((now - userScriptAt - 2.0 + 0.004) / 0.5)) + 2;
        while (userScript < due && userScript < 6) {
            UserButton(Btn::Down);
            userScript++;
        }
        Touch();
    }

    UserHeader(pageUser, PageOf(pageUser));
    // Pixel-measured on walkthrough.mp4 306-318 s: the divider on column 639,
    // rows 147 to 947; the sections stand where those of Settings do (outline
    // on rows 282-405 for the second, 649-772 for the fifth), their rules on
    // rows 282 and 649.
    VRule(L.divX, Px(147), Px(948), T.sep);

    for (int i = 0; i < count; i++) {
        float y = L.catY0 + i * L.catH;
        bool sel = i == userSec, focused = sel && !userInRows;
        const float mid = y + L.catH / 2;
        const CursorRect frame = {L.catX, Snap(mid - Px(62)) + Px(3), L.catW, Px(118), L.radius};
        if (focused) RRect(frame.x, frame.y, frame.w, frame.h, frame.r, T.focusBg);
        if (sel) Rect(L.catX + L.catBar, Snap(mid - Px(43)), Px(6), Px(86), T.accent);
        if (secs[i].sub.empty()) {
            Text(secs[i].name, L.catX + L.catText, y, L.catW - L.catText - 14, L.catH, 21.5f, sel ? T.accent : T.text);
        } else {
            // Two lines: the name 17 px above the middle, the grey line 23 px under it.
            Text(secs[i].name, L.catX + L.catText, y - 11.5f, L.catW - L.catText - 14, L.catH, 21.5f,
                 sel ? T.accent : T.text);
            Text(secs[i].sub, L.catX + L.catText + 1, y + 15, L.catW - L.catText - 14, L.catH, 16.6f,
                 cfg.dark ? T.sub : rgb(0x5F6067));
        }
        if (focused) {
            CursorFill(T.focusBg);
            Cursor(frame.x, frame.y, frame.w, frame.h, frame.r);
        }
        AddHit(L.catX, y, L.catW, L.catH, [i] {
            if (userSec != i) PlaySfx(Sfx::Move);
            UserGoSection(i);
        });
        // A rule closes a group of sections (x 168-568 px); the outline of a
        // focused neighbour lies over it.
        if (secs[i].ruleAfter && i + 1 < count && !focused && !(i + 1 == userSec && !userInRows))
            HRule(112, 378.67f, y + L.catH, T.sep);
    }

    // As on the Settings page, the right-hand side keeps the old section for
    // a few frames after the cursor has moved, then changes at once.
    if (userShown >= count) userShown = userSec;
    if (userWanted != userSec) {
        userWanted = userSec;
        userWantedAt = now;
    }
    if (userShown != userSec) {
        if (userInRows || now - userWantedAt >= 0.075) {
            userShown = userSec;
            userSecAt = userWantedAt; // the section's own timeline runs from the move
        } else {
            Touch();
        }
    }
    const bool live = userShown == userSec;
    const std::string& id = secs[userShown].id;
    std::vector<Hint> hints;
    if (id == "profile") {
        DrawUserProfile(pageUser, live);
    } else if (id == "invitations" || id == "friends") {
        if (id == "friends") DrawFriendList(pageUser, live);
        else DrawUserNotice(id);
        if (id == "invitations") hints.push_back({L"+", L"¿No puedes encontrar una sala de chat?", Btn::Plus});
        hints.push_back({L"Y", L"Actualizar", Btn::Y});
    } else {
        std::vector<URow> rows = UserRows(id, pageUser);
        DrawUserRows(rows, pageUser, live);
    }
    hints.push_back({L"B", L"Atrás", Btn::B});
    hints.push_back({L"A", L"Confirmar", Btn::A});
    Footer(hints);
}

// The picture chooser and the "new user" steps.
void UserSubButton(Btn b) {
    const int n = AvatarColours();
    switch (uSub) {
    case USub::Icon:
    case USub::NewIcon: {
        int to = iconSel;
        if (b == Btn::Left && iconSel < n && iconSel % kIconCols > 0) to--;
        else if (b == Btn::Right && iconSel < n && iconSel % kIconCols < kIconCols - 1 && iconSel + 1 < n) to++;
        else if (b == Btn::Up && iconSel > n) to = n;
        else if (b == Btn::Up && iconSel == n) to = n - kIconCols;
        else if (b == Btn::Up && iconSel >= kIconCols) to -= kIconCols;
        else if (b == Btn::Down && iconSel + kIconCols < n) to += kIconCols;
        else if (b == Btn::Down && iconSel < n) to = n;
        else if (b == Btn::Down && iconSel == n && uSub == USub::Icon) to = n + 1; // "Ajustar imagen"
        if (to != iconSel) {
            iconSel = to;
            PlaySfx(Sfx::Move);
        } else if (b == Btn::Left || b == Btn::Right || b == Btn::Up || b == Btn::Down) {
            PlaySfx(Sfx::Bump);
            BumpCursor(b == Btn::Left ? -1.0f : b == Btn::Right ? 1.0f : 0.0f,
                       b == Btn::Up ? -1.0f : b == Btn::Down ? 1.0f : 0.0f);
        } else if (b == Btn::A) {
            PlaySfx(Sfx::Ok);
            IconConfirm();
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            if (uSub == USub::NewIcon) {
                uSub = USub::NewName;
                newWantEntry = true;
            } else {
                uSub = USub::None;
            }
        }
        break;
    }
    case USub::NewName:
        if (b == Btn::A) {
            PlaySfx(Sfx::Ok);
            NewUserAskName();
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            if (!newFrom.empty()) uSub = USub::NewFrom; // back to the step before
            else EndAddUser(-1);
        }
        break;
    case USub::NewFrom: {
        const int rows = static_cast<int>(newFrom.size()) + 1;
        if ((b == Btn::Up && newFromSel > 0) || (b == Btn::Down && newFromSel + 1 < rows)) {
            newFromSel += b == Btn::Up ? -1 : 1;
            PlaySfx(Sfx::Move);
        } else if (b == Btn::Up || b == Btn::Down) {
            PlaySfx(Sfx::Bump);
            BumpCursor(0, b == Btn::Up ? -1.0f : 1.0f);
        } else if (b == Btn::A && newFromSel == 0) {
            PlaySfx(Sfx::Ok);
            uSub = USub::NewName;
            newWantEntry = true;
        } else if (b == Btn::A && newFromSel < rows) {
            PlaySfx(Sfx::Ok);
            if (AdoptProfile(newFrom[newFromSel - 1].id).empty()) {
                newIndex = static_cast<int>(cfg.users.size()) - 1;
                RefreshAvatar(newIndex);
                uSub = USub::NewDone;
            } else {
                UserReport("IO");
            }
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            EndAddUser(-1);
        }
        break;
    }
    case USub::NewDone:
        if (b == Btn::A || b == Btn::B) {
            PlaySfx(Sfx::Ok);
            EndAddUser(newIndex);
        }
        break;
    case USub::None: break;
    }
}

// The first console's page: three plain rows under the picture.
constexpr int kPlainRows = 3;
void PlainUserAct(int row) {
    if (row == 0) UserRename(pageUser);
    else if (row == 1) UserChooseIcon(pageUser);
    else UserDelete(pageUser);
}

void UserButton(Btn b) {
    if (uSub != USub::None) {
        UserSubButton(b);
        return;
    }
    if (FriendsButton(b)) return;
    if (!S2()) {
        if ((b == Btn::Up && userRow > 0) || (b == Btn::Down && userRow + 1 < kPlainRows)) {
            userRow += b == Btn::Up ? -1 : 1;
            PlaySfx(Sfx::Move);
        } else if (b == Btn::Up || b == Btn::Down) {
            PlaySfx(Sfx::Bump);
        } else if (b == Btn::A) {
            PlaySfx(Sfx::Ok);
            PlainUserAct((std::min)(userRow, kPlainRows - 1));
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            Pop();
        }
        return;
    }
    const std::vector<USection> secs = UserSections();
    const int count = static_cast<int>(secs.size());
    if (userSec >= count) userSec = count - 1;
    const std::string id = secs[userSec].id;
    const bool profile = id == "profile", notice = id == "invitations" || id == "friends";
    auto history = profile ? UserHistory(pageUser) : std::vector<std::pair<Played, Game*>>();
    std::vector<URow> rows;
    if (!profile && !notice) rows = UserRows(id, pageUser);

    switch (b) {
    case Btn::Up:
    case Btn::Down: {
        int d = b == Btn::Up ? -1 : 1, to = -1;
        if (!userInRows) to = userSec + d < count ? userSec + d : -1;
        else if (profile) to = userRow + d <= static_cast<int>(history.size()) ? userRow + d : -1;
        else to = NextURow(rows, userRow, d);
        if (to < 0) {
            PlaySfx(Sfx::Bump);
            BumpCursor(0, static_cast<float>(d));
        } else {
            if (userInRows) userRow = to;
            else UserGoSection(to);
            PlaySfx(Sfx::Move);
        }
        break;
    }
    case Btn::Right:
    case Btn::A:
        if (!userInRows) {
            if (profile || NextURow(rows, -1, 1) >= 0) {
                userInRows = true;
                userRow = profile ? 0 : NextURow(rows, -1, 1);
                PlaySfx(b == Btn::A ? Sfx::Ok : Sfx::Move);
            } else {
                PlaySfx(Sfx::Bump);
                if (b == Btn::Right) BumpCursor(1, 0);
            }
        } else if (b == Btn::A && profile && userRow == 0) {
            // The card leads to the profile's settings, as its pencil says.
            PlaySfx(Sfx::Ok);
            for (int i = 0; i < count; i++)
                if (secs[i].id == "user_settings") {
                    UserGoSection(i);
                    userInRows = true;
                }
        } else if (b == Btn::A && profile) {
            PlaySfx(Sfx::Ok);
            if (userRow - 1 < static_cast<int>(history.size()))
                UserHistoryInfo(history[userRow - 1].first, *history[userRow - 1].second);
        } else if (b == Btn::A && userRow >= 0 && userRow < static_cast<int>(rows.size()) && rows[userRow].act) {
            PlaySfx(Sfx::Ok);
            rows[userRow].act();
        } else if (b == Btn::A) {
            PlaySfx(Sfx::Bump);
        }
        break;
    case Btn::Left:
        if (userInRows) {
            userInRows = false;
            PlaySfx(Sfx::Move);
        }
        break;
    case Btn::Y:
        if (notice) {
            PlaySfx(Sfx::Ok);
            userSecAt = now; // "Actualizar": the ring turns again; there is still nothing to list
            Touch();
        }
        break;
    case Btn::Plus:
        if (id == "invitations") {
            PlaySfx(Sfx::Ok);
            ConsoleOnly();
        }
        break;
    case Btn::B:
        PlaySfx(Sfx::Back);
        if (userInRows) userInRows = false;
        else Pop();
        break;
    default: break;
    }
}

void DrawUser() {
    UserTestTyping();
    if (DrawUserSub()) return;
    if (DrawFriendsSub()) return;
    if (S2()) {
        DrawUserS2();
        return;
    }
    Theme T = Th();
    if (pageUser >= static_cast<int>(cfg.users.size())) pageUser = 0;
    const User& user = cfg.users[pageUser];
    Header(7, PageOf(pageUser));
    DrawAvatar(pageUser, 180, 200, 84);
    Text(user.name, 40, 296, 280, 44, 26, T.text, Center, 600);
    static const wchar_t* const labels[kPlainRows] = {L"Cambiar apodo", L"Cambiar icono", L"Eliminar usuario"};
    userRow = (std::max)(0, (std::min)(userRow, kPlainRows - 1));
    for (int i = 0; i < kPlainRows; i++) {
        float y = 366 + i * 60.0f;
        Line(60, y + 0.5f, 300, y + 0.5f, T.sep, 1);
        Line(60, y + 60.5f, 300, y + 60.5f, T.sep, 1);
        Text(labels[i], 76, y, 220, 60, 20, T.text);
        if (i == userRow) Cursor(56, y - 3, 248, 67, 4);
        AddHit(60, y, 240, 60, [i] {
            bool was = userRow == i;
            userRow = i;
            if (was) OnButtonImpl(Btn::A);
            else PlaySfx(Sfx::Move);
        });
    }

    Text(L"Actividad de juego", 360, 110, 600, 36, 20, T.sub);
    std::vector<std::pair<Played, Game*>> list = UserHistory(pageUser);
    float y = 156;
    for (size_t i = 0; i < list.size() && i < 6; i++, y += 78) {
        Game& g = *list[i].second;
        Line(360, y + 0.5f, 1220, y + 0.5f, T.sep, 1);
        DrawTile(g, 372, y + 9, 60);
        Text(g.name, 452, y + 8, 520, 34, 22, T.text);
        Text(L"Última partida: " + DateOf(list[i].first.last), 452, y + 40, 520, 28, 17, T.sub);
        Text(PlayTime(list[i].first.seconds), 960, y, 250, 78, 20, T.accent, Right);
    }
    if (list.empty()) Text(L"Todavía no hay actividad.", 360, 170, 600, 36, 22, T.text);
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Aceptar", Btn::A}});
}

// Test states for the user page (--shot / --shotseq screen names). Options
// first, each ending in '+', then a state; anything else after the options is
// handed on (so "id0+home" is the HOME menu with identical mode off):
//   id0+ / id1+         identical mode off / on
//   st1+                the first console's style
//   u<n>+               whose page (index)
//   type:<text>+        typed into the next text entries ('|' between several)
//   img:<file>+         what "Usar una imagen del PC" picks
//   user-sec:<n>        the page with section n focused
//   user-row:<n>:<r>    section n with its row r focused (r counts every row of
//                       a list section, headings included; one that cannot
//                       take the cursor gives the next one that can)
//   user-icon           the picture chooser
//   user-new            the "new user" steps from the start
//   user-plus           the HOME menu with the cursor on its "+"
bool DebugUser(const std::wstring& full) {
    userTest = true;
    // "<state>-2" and "-2l" come back here once the style has been set.
    if (full.size() > 2 && (full.compare(full.size() - 2, 2, L"-2") == 0 || full.compare(full.size() - 3, 3, L"-2l") == 0))
        return false;
    std::wstring name = full;
    bool hadOptions = false;
    for (size_t plus; (plus = name.find(L'+')) != std::wstring::npos; name = name.substr(plus + 1)) {
        std::wstring o = name.substr(0, plus);
        if (o == L"id0" || o == L"id1") cfg.identical = o == L"id1";
        else if (o == L"st1") cfg.menuStyle = 1;
        else if (o.size() == 2 && o[0] == L'u') // (wchar_t is unsigned on ARM)
            pageUser = (std::min)(static_cast<int>(o[1] - L'0'), static_cast<int>(cfg.users.size()) - 1);
        else if (o.rfind(L"type:", 0) == 0) userTestText = o.substr(5);
        else if (o.rfind(L"img:", 0) == 0) userTestImage = o.substr(4);
        else break;
        hadOptions = true;
    }
    auto open = [](int sec, int row, bool inRows) {
        int u = pageUser;
        OpenUserPage(u);
        userSec = userShown = sec;
        userRow = row;
        userInRows = inRows;
    };
    if (name.rfind(L"user-sec:", 0) == 0) {
        open(static_cast<int>(wcstol(name.c_str() + 9, nullptr, 10)), 0, false);
    } else if (name.rfind(L"user-row:", 0) == 0) {
        size_t colon = name.find(L':', 9);
        open(static_cast<int>(wcstol(name.c_str() + 9, nullptr, 10)), colon == std::wstring::npos ? 0 : static_cast<int>(wcstol(name.c_str() + colon + 1, nullptr, 10)), true);
    } else if (name == L"user-icon") {
        open(0, 0, false);
        uSub = USub::Icon;
        iconSel = 0;
    } else if (name == L"user-new") {
        StartAddUser([](int i) {
            if (i >= 0) OpenUserPage(i);
        });
    } else if (name == L"user-plus") {
        homeRow = 0;
        homeUser = static_cast<int>(cfg.users.size());
    } else if (hadOptions) {
        DebugGoto(name);
    } else {
        return false;
    }
    return true;
}

double whoAt = 0; // first frame of the sheet

void DrawWho() {
    Theme T = Th();
    int n = static_cast<int>(cfg.users.size());
    if (whoSel >= n + (S2() ? 1 : 0)) whoSel = 0;
    if (S2()) {
        // A sheet over the dimmed home menu. Layout measured on the capture:
        // sheet top 300, wording from x 81, pictures centred on y 482.5 every
        // 141.2, names at 578. Motion, w5 17.67 s (arriving) and 18.67 s (a
        // user picked), frame by frame:
        //   arriving: the panel is there at once, opaque, 8.7 units low, and
        //             rises 13 11 9 7 5 3 2 1 0 px; what is on it fades in over
        //             ten frames (0.03 0.06 0.13 0.22 0.31 0.42 0.56 0.71 0.87 1).
        //             Its footer (B, A) is there from the first frame.
        //   leaving:  the picked picture is tinted on the press frame; then the
        //             sheet drops 5.6 units a frame for five frames while its
        //             contents fade (0.68 0.39 0.17 0.05 0 left); the bare
        //             panel, footer included, shows through at 0.56 for the
        //             fifth and sixth frame and is gone on the seventh.
        float m = FramesSince(whoAt);
        bool leaving = flow.leaveAt >= 0;
        float out = leaving ? FramesSince(flow.leaveAt) : 0;
        if (leaving && out >= 6.5f) {
            PopNow();
            DrawHome(0);
            return;
        }
        suppressCursor = true;
        DrawHome(0);
        suppressCursor = false;
        hits.clear();
        if (m < -0.5f) {
            Touch();
            return; // the press frame: the menu has only begun to dim
        }
        static const float rise[] = {13, 11, 9, 7, 5, 3, 2, 1, 0};
        float top = 300 + Lerp(rise, 9, m) / 1.5f + (std::min)(out, 5.0f) * 5.6f;
        float a = powf((std::min)(1.0f, (m + 1) / 10), 1.6f);
        if (leaving) {
            float k = (std::max)(0.0f, 1 - out / 5);
            a *= k * k;
        }
        float panel = out < 4.5f ? 1.0f : 0.56f;
        if (m < 10 || leaving) Touch();

        if (panel < 1) PushLayer(panel);
        RRect(0, top, W, 500, 22, T.dialog); // the ground of a dialog, not the bar's
        if (a > 0) {
            if (a < 1) PushLayer(a);
            // Pixel-measured at rest (anim.mkv 278.3 s, walkthrough2 21.4 s):
            // pictures 180 px across in a ring 6 px wide, centred on pixel row
            // 724 and on columns 268 + 212 i; the outline hugs the ring. The
            // wording covers rows 511 to 547.
            Text(L"¿Quién va a usar el programa?", 80, top + 31 - Px(3.5f), 900, 44, 23.8f, T.text); // 332.7 wide on w5
            int shown = (std::min)(n, 7);
            for (int i = 0; i < shown; i++) {
                float cx = Px(268) + i * Px(212), cy = top + Px(274);
                Circle(cx, cy, Px(96), T.avRing);
                DrawAvatar(i, cx, cy, Px(90));
                // The picked picture goes pale blue (w5: black reads 50 63 97,
                // a mid orange 228 196 165).
                if (leaving && flow.picked && i == whoSel) Circle(cx, cy, Px(90), rgb(0x8BAFFF, 0.36f));
                if (i == whoSel) CircleCursor(cx, cy, Px(98.5f));
                Text(cfg.users[i].name, cx - 70, top + 257, 140, 42, 23.4f, T.text, Center);
                if (!leaving)
                    AddHit(cx - 64, cy - 64, 128, 128, [i] {
                        whoSel = i;
                        OnButtonImpl(Btn::A);
                    });
            }
            // "Add a user", after the last picture.
            // (Same frame: a ring 84 px across and 2 px wide, 20 px left of
            // the next picture's place; a cross of two 40 x 7 px bars.)
            float px = Px(268) + shown * Px(212) - Px(20), py = top + Px(274);
            CircleStroke(px, py, Px(41), T.sub, Px(2));
            Rect(px - Px(20), py - Px(3.5f), Px(40), Px(7), T.sub);
            Rect(px - Px(3.5f), py - Px(20), Px(7), Px(40), T.sub);
            if (whoSel == shown) CircleCursor(px, py, 31);
            if (!leaving)
                AddHit(px - 30, py - 30, 60, 60, [shown] {
                    whoSel = shown;
                    OnButtonImpl(Btn::A);
                });
            if (a < 1) PopLayer();
        }
        // The sheet reaches the foot of the screen and carries its own footer.
        Rect(0, 646, W, H - 646, T.dialog);
        Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}});
        if (panel < 1) PopLayer();
        return;
    }
    Text(L"¿Quién va a jugar?", 0, 150, W, 50, 30, T.text, Center);
    float step = 150, x0 = W / 2 - (n - 1) * step / 2;
    for (int i = 0; i < n; i++) {
        float cx = x0 + i * step, cy = 330;
        DrawAvatar(i, cx, cy, 56);
        if (i == whoSel) {
            CircleCursor(cx, cy, 63);
            Text(cfg.users[i].name, cx - 200, 408, 400, 40, 24, T.title, Center);
        }
        AddHit(cx - 60, cy - 60, 120, 120, [i] {
            whoSel = i;
            OnButtonImpl(Btn::A);
        });
    }
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Aceptar", Btn::A}});
}

void WhoButton(Btn b) {
    int n = static_cast<int>(cfg.users.size());
    if (S2() && flow.leaveAt >= 0) return; // the sheet is on its way out
    if (b == Btn::Left && whoSel > 0) {
        whoSel--;
        PlaySfx(Sfx::UserMove);
    } else if (b == Btn::Right && whoSel + 1 < n + (S2() ? 1 : 0)) {
        whoSel++;
        PlaySfx(Sfx::UserMove);
    } else if (b == Btn::Left || b == Btn::Right) {
        PlaySfx(Sfx::Bump);
    } else if (b == Btn::B) {
        PlaySfx(Sfx::Back);
        if (S2()) {
            // Not in the capture: the sheet leaves as it does after a pick and
            // the menu clears at the same time.
            flow.leaveAt = NextFrame();
            flow.picked = false;
            Touch();
        } else {
            Pop();
        }
    } else if (b == Btn::A && whoSel >= n) {
        // The plus: add a user, then back to this sheet with the new one chosen.
        PlaySfx(Sfx::Ok);
        flow.sheetAt = flow.leaveAt = -1;
        PopNow();
        StartAddUser([](int i) {
            whoSel = i >= 0 ? i : cfg.currentUser;
            whoAt = now;
            PushNow(Scr::Who);
        });
    } else if (b == Btn::A) {
        cfg.currentUser = whoSel;
        SaveConfig();
        if (S2()) {
            // w5 from 18.67 s, counted from the press: the sheet is gone on
            // frame 7, the menu stays dark until 16 and is clear by 24, the
            // picture starts to grow on 26 and the screen is black on 54.
            flow.leaveAt = NextFrame();
            flow.picked = true;
            BeginPress(whoGame);
            pressAt = NextFrame() + 25.0 / 60.0;
        } else {
            PopNow();
            BeginPress(whoGame);
            pressAt = now + 0.30;
        }
    }
}

double launchOutAt = -1; // the game is ready: the loading mark fades out

void DrawLaunch() {
    // Nothing but black and a small busy mark in the corner, as on the
    // console. B cancels and Y shows the emulator, without any on-screen hint.
    Rect(0, 0, W, H, rgb(0x000000));
    float a = (std::min)(1.0f, static_cast<float>(now - launchAt) / 0.3f);
    if (S2() && PackIcon(L"launch_00")) {
        // The owner's own loading mark, cut by tools\pack_game.py. Timed on w5
        // from the first black frame (19.567 s), in frames:
        //   0-41    black
        //   42-47   the still mark fades in (linear in light, 5 frames)
        //   then a loop of 320 frames (5.33 s) that began on frame 39:
        //     0-18 still, 19-73 the mark draws itself, 74-302 held,
        //     303-319 it folds back into the still mark.
        // When the game is ready everything fades out in 12 frames, linear in
        // light (w5 27.05 s).
        float f = FramesSince(launchAt);
        if (f >= 41.5f) {
            int c = static_cast<int>(f - 39 + 0.5f) % 320;
            int frame = c < 19 ? 0 : c < 74 ? c - 18 : c < 303 ? 55 : c - 247;
            float lit = (std::min)(1.0f, (f - 42) / 5.0f);
            if (launchOutAt >= 0) lit *= (std::max)(0.0f, 1 - FramesSince(launchOutAt) / 12);
            wchar_t name[16];
            swprintf_s(name, L"launch_%02d", frame);
            if (Img* img = PackIcon(name)) Image(*img, 1040, 513.33f, 220, 200, powf((std::max)(0.0f, lit), 1 / 2.2f));
        }
    } else if (PackIcon(L"load_00")) {
        // From the owner's local pack: their console's loading mark, as a
        // short intro that holds, fades and repeats (timed on the capture:
        // 30 frames a second, 5.2 s per loop).
        constexpr int kFrames = 36;
        float cycle = static_cast<float>(fmod(now - launchAt, 5.2));
        int frame = (std::min)(kFrames - 1, static_cast<int>(cycle * 30.0f));
        float out = cycle > 4.95f ? 1.0f - (cycle - 4.95f) / 0.25f : 1.0f;
        wchar_t name[16];
        swprintf_s(name, L"load_%02d", frame);
        if (Img* img = PackIcon(name)) Image(*img, 1040, 513.33f, 220, 200, (std::max)(0.0f, out));
    } else if (Img* mark = PackIcon(L"loading")) {
        Image(*mark, 960, 520, 320, 200, a);
    } else {
        float spin = static_cast<float>(fmod(now * 300.0, 360.0));
        CircleStroke(1196, 656, 13, rgb(0xFFFFFF, 0.18f * a), 3);
        Arc(1196, 656, 13, spin, spin + 100, rgb(0xFFFFFF, a), 3);
    }
    Touch();
}

bool DebugGame(const std::wstring& name) {
    if (name == L"run" && !games.empty()) {
        runningId = games[0].id;
        return true;
    }
    if ((name == L"more" || name == L"last") && !games.empty()) {
        size_t have = games.size();
        for (size_t i = 0; games.size() < 13; i++) games.push_back(games[i % have]);
        if (name == L"last") {
            gi = HomeCount() - 1;
            homeScroll = TileX0() + gi * (kTile + TileGap()) - (W - 84 - kTile);
        }
        return true;
    }
    if (name == L"ingame" && !games.empty()) {
        // The first game is running and in front: HOME brings the menu back.
        runningId = launchId = games[0].id;
        launchAt = -100;
        Push(Scr::Launch);
        return true;
    }
    if (name == L"launchout" && !games.empty()) {
        launchId = games[0].id;
        launchAt = -4; // well into the loop
        launchOutAt = 2 + 10.0 / 60.0;
        Push(Scr::Launch);
        return true;
    }
    return false;
}

/// ---- overlays -------------------------------------------------------------
// Dialogs and the text-entry box of the second console's menu, read from
// captures frame by frame (anim.mkv 256.3-258.9 s dark, w5.mkv 5.8-14 s and
// 33.4-35.4 s light). What the console does:
//   - nothing moves or scales. The menu darkens and the box fades in over it,
//     both easing out (1-(1-t)^2): the darkening over 11 frames, the box over 10;
//   - the box is opaque once it is in, but while it fades the console blends
//     it in linear light, not on the stored values, so a dark box shows up
//     quickly over the dark background and slowly over a bright picture. That
//     is the "see-through" look, and LinVeil() below reproduces it;
//   - the focus outline and the focused button's lighter fill appear on the
//     frame after the box is fully in;
//   - on A the button takes its pressed colour for two frames, then box and
//     darkening fade out together over 10 frames ((1-t)^2), and only then does
//     whatever was chosen start.

bool PressJustStarted() {
    return pressFx.on && PressAge() < 0.5f / 60.0f;
}

float ToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}
float ToSrgb(float v) {
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

// Flat drawing can only do "keep (1-a) of what is there and add k". A veil is
// the (a, k) that comes closest to blending colour c at opacity `op` in
// linear light over a surface whose stored values run from lo to hi: exact at
// both ends and within a few levels in between.
struct Veil {
    float a;
    float k[3];
};
struct Surface {
    float lo, hi;
};

float Unit(float v) {
    return (std::max)(0.0f, (std::min)(1.0f, v));
}

Veil LinVeil(Color c, float op, Surface under) {
    const float lo = under.lo, hi = (std::max)(under.hi, under.lo + 0.02f);
    const float ch[3] = {c.r, c.g, c.b};
    float flo[3], fhi[3], slope = 0;
    for (int i = 0; i < 3; i++) {
        flo[i] = ToSrgb((1 - op) * ToLinear(lo) + op * ToLinear(ch[i]));
        fhi[i] = ToSrgb((1 - op) * ToLinear(hi) + op * ToLinear(ch[i]));
        slope += (fhi[i] - flo[i]) / (hi - lo) / 3;
    }
    Veil v;
    v.a = Unit(1 - slope);
    for (int i = 0; i < 3; i++) v.k[i] = ((flo[i] - (1 - v.a) * lo) + (fhi[i] - (1 - v.a) * hi)) / 2;
    return v;
}

// The colour to draw so that `v` is what lands on the surface.
Color VeilPaint(const Veil& v) {
    if (v.a < 0.004f) return {0, 0, 0, 0};
    return {Unit(v.k[0] / v.a), Unit(v.k[1] / v.a), Unit(v.k[2] / v.a), v.a};
}

// The surface once `v` has been painted on it.
Surface Covered(Surface s, const Veil& v) {
    float k = (v.k[0] + v.k[1] + v.k[2]) / 3;
    return {s.lo * (1 - v.a) + k, s.hi * (1 - v.a) + k};
}

float EaseOut2(float t) {
    t = Unit(t);
    return 1 - (1 - t) * (1 - t);
}

// Colours measured on the captures (anim.mkv 257.7 s dark, walkthrough2.mkv
// 18.2 s light; see tools\palette_check.py). Box, text and small print are
// the theme's own; the rule is a touch darker in the light theme's box than
// on its page.
struct DlgLook {
    Color box, focus, pressed, sep, text, sub, label, dim;
    float dimKeep; // how much of the menu's brightness is left under a dialog
};

DlgLook DL() {
    if (A11yContrast()) { // high contrast: as A11yTheme, with the menu darkened further behind the box
        if (cfg.dark)
            return {rgb(0x1E1E1E), rgb(0x333333), rgb(0x24405A), rgb(0xFFFFFF), rgb(0xFFFFFF), rgb(0xE4E4E4),
                    rgb(0x5CD6FF), rgb(0x01010C), 0.30f};
        return {rgb(0xFFFFFF), rgb(0xDFDFDF), rgb(0xC9D3EE), rgb(0x000000), rgb(0x000000), rgb(0x2A2A2A),
                rgb(0x00339A), rgb(0x000006), 0.50f};
    }
    if (cfg.dark)
        return {rgb(0x2E2E2E), rgb(0x3B3B3B), rgb(0x394451), rgb(0x656565), rgb(0xD2CFD3), rgb(0xA4A3A4),
                rgb(0x28BCEF), rgb(0x01010C), 0.477f};
    return {rgb(0xE0DDE1), rgb(0xE9E6EA), rgb(0xDEDFEA), rgb(0x979498), rgb(0x161616), rgb(0x646369),
            rgb(0x0C5FCA), rgb(0x000006), 0.664f};
}

// The darkening behind a dialog, q = 0..1. Outside the box every stored value
// is simply multiplied (measured: x0.477 dark, x0.664 light, plus a trace of
// blue in the dark theme); in between it follows a black veil in linear light.
// Returns the darkened menu's range of values: background and bright artwork.
Surface DrawDim(float q) {
    DlgLook L = DL();
    float keep = powf(1 - (1 - powf(L.dimKeep, 2.4f)) * Unit(q), 1 / 2.4f);
    Rect(0, 0, W, H, alpha(L.dim, 1 - keep));
    return {(cfg.dark ? 0.12f : 0.20f) * keep, (cfg.dark ? 0.80f : 0.87f) * keep};
}

// Dialog geometry in layout units, from the captures (pixels / 1.5): the box
// is 1072 px wide and at least 424 px high, each button row 112 px plus a
// 1 px rule, text lines 46 px apart (38 px for the small print).
constexpr float kDlgW = 714.67f, kDlgRow = 74.67f, kDlgRule = 0.667f, kDlgMinH = 282.67f, kDlgRadius = 11.0f;
constexpr float kDlgMainSize = 20.6f, kDlgSubSize = 17.7f, kDlgLabelSize = 21.8f;
constexpr float kDlgMainPitch = 30.67f, kDlgSubPitch = 25.33f, kDlgSubGap = 3.33f;

// One line with the middle of its capital letters at `capMid`.
void CapText(const std::wstring& s, float x, float capMid, float w, float size, Color c, Align align = Left) {
    Text(s, x, capMid - 20 - size * 0.0f, w, 40, size, c, align);
}

std::vector<std::wstring> WrapText(const std::wstring& sEs, float size, float maxW) {
    const std::wstring s = lang::Tr(sEs); // whole, before it is cut into lines
    lang::Verbatim pieces;                // which are measured as they are
    std::vector<std::wstring> out;
    size_t at = 0;
    while (at <= s.size()) {
        size_t nl = s.find(L'\n', at);
        std::wstring para = s.substr(at, nl == std::wstring::npos ? std::wstring::npos : nl - at);
        std::wstring line;
        for (const lang::Word& w : lang::Words(para)) { // spaces, or between Chinese and Japanese characters
            std::wstring tried = line.empty() ? w.text : line + (w.space ? L" " : L"") + w.text;
            if (!line.empty() && TextWidth(tried, size) > maxW) {
                out.push_back(line);
                line = w.text;
            } else {
                line = tried;
            }
        }
        out.push_back(line);
        if (nl == std::wstring::npos) break;
        at = nl + 1;
    }
    return out;
}

// Rows of buttons: one or two choices share a row; from three on, the first
// two share the bottom row and each further one gets a full row above it
// (update notice, w5.mkv 6.3 s: "start" over "cancel | download").
int DlgRows(int n) {
    return n <= 2 ? (n > 0 ? 1 : 0) : n - 1;
}

CursorRect DlgCell(int i, int n, float x, float rowsTop) {
    const float pitch = kDlgRow + kDlgRule;
    float bottom = rowsTop + (DlgRows(n) - 1) * pitch + kDlgRule;
    if (n == 1) return {x, bottom, kDlgW, kDlgRow, 0};
    if (i < 2) return {x + i * kDlgW / 2, bottom, kDlgW / 2, kDlgRow, 0};
    return {x, rowsTop + (n - 1 - i) * pitch + kDlgRule, kDlgW, kDlgRow, 0};
}

// Test states for dialogs, text entry, the album and the controllers page.
bool DebugOverlays(const std::wstring& full) {
    // "<state>@" opens the dialog by itself 18 frames into a --shotseq run,
    // as if A had been pressed on a menu button at frame 0.
    const bool timed = !full.empty() && full.back() == L'@';
    const std::wstring name = timed ? full.substr(0, full.size() - 1) : full;
    if (name == L"dlg-sleep") {
        // The console's own standby wording, to lay our box over the capture.
        Ask(L"Se va a activar el modo de espera.\nPara salir de este modo, pulsa \xE0F4.\n\n"
            L"También puedes activarlo mediante la configuración de acceso rápido (mantén pulsado \xE0F4).",
            {L"Cancelar", L"Modo de espera"});
    } else if (name == L"dlg-upd") {
        Ask(L"Se ha encontrado una nueva actualización.\nSe va a descargar ahora.",
            {L"Cancelar", L"Descargar", L"Iniciar el programa"});
    } else if (name == L"dlg-one") {
        Info(L"La descarga se ha cancelado.");
    } else if (name == L"dlg-wait") {
        Ask(L"Preparando...", {L"Cancelar"});
        dlg.busy = true;
    } else if (name == L"dlg-close" && !games.empty()) {
        Ask(L"¿Quieres cerrar el programa suspendido?\nSe perderán los datos no guardados.", {L"Cancelar", L"Cerrar"});
        dlg.iconGame = games[0].id;
    } else if (name == L"dlg-power") {
        homeRow = 2;
        si = SysCount() - 1; // the power button: A opens NSW-Home's three-choice dialog
        return true;
    } else if (name == L"album-empty") {
        OpenAlbum();
        shots.clear();
        return true;
    } else if (name == L"album-demo" || name == L"album-go") {
        // A full album without any screenshot on the machine. "album-go"
        // waits on the album button so that A at frame 0 opens it.
        albumDemo = true;
        if (name == L"album-demo") {
            OpenAlbum();
        } else {
            homeRow = 2;
            for (int i = 0; i < SysCount(); i++)
                if (SysList()[i].action == 3) si = i;
        }
        return true;
    } else if (name == L"pads-on") {
        debugPads = 0b0101; // players 1 and 3
        Push(Scr::Pads);
        return true;
    } else if (name == L"pads-off") {
        debugPads = 0;
        Push(Scr::Pads);
        return true;
    } else if (name.rfind(L"dlg:", 0) == 0) {
        // Any one-button dialog: "dlg:first line|second line".
        std::wstring text = name.substr(4);
        std::replace(text.begin(), text.end(), L'|', L'\n');
        Info(text);
    } else if (name == L"entry") {
        entry.open = true;
        entry.title = L"Apodo";
        entry.text = L"Jugador";
        entry.done = nullptr;
    } else {
        return false;
    }
    // --shotseq draws 120 frames before its frame 0.
    dlg.openAt = timed ? 137 / 60.0 : now;
    return true;
}

void CloseDialog(int result) {
    dlg.open = false;
    auto done = std::move(dlg.done);
    dlg.done = nullptr;
    if (done) done(result);
    Touch();
}

void DrawDialogClassic(float dt) {
    Theme T = Th();
    Smooth(dlg.anim, 1, dt, 22);
    if (dlg.anim < 1) Touch();
    int lines = 1 + static_cast<int>(std::count(dlg.msg.begin(), dlg.msg.end(), L'\n'));
    float w = 770.0f, btnH = 72.0f;
    float msgH = 96.0f + lines * 36.0f, h = msgH + btnH;
    float x = (W - w) / 2, y = (H - h) / 2;
    Rect(0, 0, W, H, rgb(0x000000, 0.6f * dlg.anim));
    PushScale(0.94f + 0.06f * dlg.anim, W / 2, H / 2);
    RRect(x, y, w, h, 5.0f, alpha(T.dialog, dlg.anim));
    TextWrap(dlg.msg, x + 50, y + 48, w - 100, msgH - 48, 24, alpha(T.text, dlg.anim), Center);
    Line(x, y + msgH + 0.5f, x + w, y + msgH + 0.5f, T.sep, 1);
    int n = static_cast<int>(dlg.btns.size());
    float bw = w / n;
    for (int i = 0; i < n; i++) {
        float bx = x + i * bw;
        if (i > 0) Line(bx + 0.5f, y + msgH, bx + 0.5f, y + h, T.sep, 1);
        Text(dlg.btns[i], bx + 8, y + msgH, bw - 16, btnH, 23.0f, T.accent, Center);
        if (i == dlg.sel) Cursor(bx + 1, y + msgH + 1, bw - 2, btnH - 2, 4.0f);
        AddHit(bx, y + msgH, bw, btnH, [i] {
            dlg.sel = i;
            OnButtonImpl(Btn::A);
        });
    }
    PopTransform();
}

void DrawDialog(float dt) {
    if (!S2()) {
        DrawDialogClassic(dt);
        return;
    }
    const DlgLook L = DL();
    const int n = static_cast<int>(dlg.btns.size());

    // Where we are, in frames of the capture (60 a second).
    float kf = static_cast<float>((now - dlg.openAt) * 60.0) + 0.001f;
    if (kf < 0.5f) {
        // The pressed button is still reacting, and keeps its outline until
        // the menu starts to darken.
        if (PressAge() < 0.4f && pressFx.rect.w > 0) StrokeCursor(pressFx.rect, 1);
        Touch();
        return;
    }
    float dim = EaseOut2(kf / 10.5f), box = EaseOut2(kf / 10);
    bool focusShown = kf >= 10.5f && !dlg.busy;
    if (dlg.pressAt >= 0) {
        float kc = static_cast<float>((now - dlg.pressAt) * 60.0) + 0.001f - (dlg.tinted ? 2.0f : 0.0f);
        if (kc >= 10) {
            CloseDialog(dlg.result);
            return;
        }
        if (kc > 0) {
            // The box is gone half a frame before the darkening.
            float left = 1 - kc / 10, boxLeft = (std::max)(0.0f, 1 - kc / 9.5f);
            dim *= left * left;
            box *= boxLeft * boxLeft;
        }
    }
    if (kf < 11.5f || dlg.pressAt >= 0 || dlg.busy) Touch();

    // Text: the part after a blank line is small print. Beside a picture the
    // first line is the question and the rest is small print, as on the console.
    Game* pictured = dlg.iconGame.empty() ? nullptr : FindGame(dlg.iconGame);
    const bool pic = pictured && !pictured->icon.empty();
    const float textMax = pic ? 329.0f : 580.0f;
    const std::wstring msg = lang::Tr(dlg.msg); // whole, before it is cut into parts
    std::wstring mainText = msg, subText;
    size_t cut = msg.find(L"\n\n");
    if (cut != std::wstring::npos) {
        mainText = msg.substr(0, cut);
        subText = msg.substr(cut + 2);
    } else if (pic && (cut = msg.find(L'\n')) != std::wstring::npos) {
        mainText = msg.substr(0, cut);
        subText = msg.substr(cut + 1);
    }
    std::vector<std::wstring> mainL = WrapText(mainText, kDlgMainSize, textMax), subL;
    if (!subText.empty()) subL = WrapText(subText, kDlgSubSize, textMax);
    const int nm = static_cast<int>(mainL.size()), ns = static_cast<int>(subL.size());

    // Box: the text area is as high as its lines plus 116 px, the picture
    // makes it at least 435 px, and the whole box is never under 424 px.
    float contentH = nm * kDlgMainPitch + (ns ? kDlgSubGap + ns * kDlgSubPitch : 0.0f) + (dlg.busy ? 70.67f : 0.0f);
    float rowsH = DlgRows(n) * (kDlgRow + kDlgRule);
    float areaH = (std::max)(77.33f + contentH, pic ? 290.0f : 0.0f);
    areaH = (std::max)(areaH, kDlgMinH - rowsH);
    float h = areaH + rowsH, x = (W - kDlgW) / 2, y = (H - h) / 2;
    // The lines sit 10 px below centre (10 px above it beside a picture).
    float top = y + (areaH - contentH) / 2 + (pic ? -6.0f : 7.33f);
    float blockW = 0;
    for (auto& l : mainL) blockW = (std::max)(blockW, TextWidth(l, kDlgMainSize));
    for (auto& l : subL) blockW = (std::max)(blockW, TextWidth(l, kDlgSubSize));
    float textX = pic ? x + 316.0f : x + (kDlgW - blockW) / 2;

    const Surface menu = DrawDim(dim);
    const Veil boxVeil = LinVeil(L.box, box, menu);
    if (!cfg.dark && box > 0) {
        // The light theme's box casts a soft shadow, the dark theme's none
        // (walkthrough2 18.2 s, w5 33.9 s: the page loses 18 levels of 146 at
        // the foot of the box, 11 fourteen pixels lower, 7 at 26, 1 at 46, and
        // 8 at its sides). That is the box's own shape 14 px lower, blurred
        // by about 18 px, at 16 %: drawn as twelve nested shapes whose edges
        // are spread the way such a blur spreads an edge.
        static const float spread[12] = {-1.73f, -1.15f, -0.81f, -0.55f, -0.32f, -0.10f, 0.10f, 0.32f, 0.55f, 0.81f, 1.15f, 1.73f};
        const Color shade = rgb(0x000000, 0.0144f * box);
        for (float z : spread) {
            const float o = z * Px(18);
            RRect(x - o, y + Px(14) - o, kDlgW + 2 * o, h + 2 * o, (std::max)(2.0f, kDlgRadius + o), shade);
        }
    }
    RRect(x, y, kDlgW, h, kDlgRadius, VeilPaint(boxVeil));
    // What is in the box comes in later and leaves sooner than the box
    // itself: measured against the box, its strength follows a power of the
    // box's opacity (again blended in linear light). The powers were fitted
    // to the text's contrast in each frame of both captures.
    const Surface onBox = Covered(menu, boxVeil);
    const float ink = powf(box, dlg.pressAt >= 0 ? 2.5f : 3.4f);
    auto Ink = [&](Color c, Surface on) { return VeilPaint(LinVeil(c, ink, on)); };
    if (pic) {
        // Picture 276 px square, 95 px in from the left and 76 px down, with
        // the player's picture overlapping its lower edge.
        float s = LinVeil(rgb(0x808080), ink, onBox).a;
        if (s > 0) {
            PushLayer(s);
            Image(pictured->icon, x + Px(96), y + 50.67f, 184.0f, 184.0f, 1, 0, 10); // pixel columns 520 to 795 (w5 33.9 s)
            if (cfg.currentUser < static_cast<int>(cfg.users.size())) {
                Circle(x + 155.33f, y + 229.33f, 29.3f, rgb(0xF0EDF0));
                DrawAvatar(cfg.currentUser, x + 155.33f, y + 229.33f, 27.3f);
            }
            PopLayer();
        }
    }
    float capMid = top + kDlgMainPitch / 2;
    for (int i = 0; i < nm; i++, capMid += kDlgMainPitch)
        CapText(mainL[i], textX, capMid, 700, kDlgMainSize, Ink(L.text, onBox));
    if (dlg.busy) {
        // The waiting mark: an open ring that turns, 91 px under the text
        // (under its last line when it has more than one).
        float cx = W / 2, cy = top + kDlgMainPitch * (static_cast<float>(nm) - 0.5f) + 58.67f, spin = static_cast<float>(fmod(now * 360.0, 360.0));
        Color c = Ink(L.sub, onBox);
        CircleStroke(cx, cy, 20.7f, alpha(c, 0.25f), 3.2f);
        Arc(cx, cy, 20.7f, spin, spin + 110, c, 3.2f);
    }
    capMid += kDlgSubGap - (kDlgMainPitch - kDlgSubPitch) / 2;
    for (int i = 0; i < ns; i++, capMid += kDlgSubPitch)
        CapText(subL[i], textX, capMid, 700, kDlgSubSize, Ink(L.sub, onBox));

    // Buttons.
    const float rowsTop = y + areaH;
    const Color rule = Ink(L.sep, onBox);
    for (int r = 0; r < DlgRows(n); r++) {
        float ry = rowsTop + r * (kDlgRow + kDlgRule) + kDlgRule / 2;
        Line(x, ry, x + kDlgW, ry, rule, kDlgRule);
    }
    // (The upright one is the last pixel column of the left half.)
    if (n >= 2) VRule(W / 2 - Px(1), rowsTop + rowsH - kDlgRow, y + h, rule);
    for (int i = 0; i < n; i++) {
        CursorRect c = DlgCell(i, n, x, rowsTop);
        Surface under = onBox;
        bool focused = i == dlg.sel && (focusShown || (dlg.busy && dlg.tinted));
        // The fill of the button the focus left is gone two frames later
        // (58 % and 8 % of it on the way).
        float fill = focused ? 1.0f : 0.0f;
        if (!focused && i == dlg.lastSel && focusShown) {
            float since = static_cast<float>((now - dlg.selAt) * 60.0) + 0.001f;
            fill = since < 1.5f ? 0.58f : since < 2.5f ? 0.08f : 0.0f;
            if (fill > 0) Touch();
        }
        if (fill > 0) {
            Color fc = mix(L.box, focused && dlg.tinted ? L.pressed : L.focus, fill);
            Veil fv = LinVeil(fc, ink, onBox);
            if (dlg.busy) Rect(c.x, c.y, c.w, c.h, VeilPaint(fv)); // no outline: the whole row lights
            else RRect(c.x, c.y, c.w, c.h, 4, VeilPaint(fv));
            under = Covered(onBox, fv);
        }
        CapText(dlg.btns[i], c.x + 8, c.y + c.h / 2, c.w - 16, kDlgLabelSize, Ink(L.label, under), Center);
        if (focused && !dlg.busy) {
            // The outline lies wholly outside the cell, so over the box's edge
            // and the rules (anim.mkv 257.7 s: pixel rows 634 to 757 round a
            // cell of 640 to 751): the box here is the middle of its 6 px.
            CursorRect o = {c.x - Px(3), c.y - Px(3), c.w + Px(6), c.h + Px(6), 6.0f};
            if (box >= 0.999f) Cursor(o.x, o.y, o.w, o.h, o.r);
            else StrokeCursor(o, box); // the outline leaves with the box, not with the text
        }
        if (dlg.pressAt < 0)
            AddHit(c.x, c.y, c.w, c.h, [i] {
                dlg.sel = i;
                OnButtonImpl(Btn::A);
            });
    }
}

void DialogButton(Btn b) {
    int n = static_cast<int>(dlg.btns.size());
    if (!S2()) {
        if (b == Btn::Left && dlg.sel > 0) {
            dlg.sel--;
            PlaySfx(Sfx::Move);
        } else if (b == Btn::Right && dlg.sel + 1 < n) {
            dlg.sel++;
            PlaySfx(Sfx::Move);
        } else if (b == Btn::A) {
            PlaySfx(Sfx::Ok);
            CloseDialog(dlg.sel);
        } else if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            CloseDialog(0); // every dialog puts its safe choice first
        }
        return;
    }
    if (dlg.pressAt >= 0) return; // already chosen; the box is on its way out
    // Buttons 0 and 1 share the bottom row; 2 and up are full rows above it,
    // the highest number at the top.
    int to = dlg.sel;
    if (b == Btn::Left && dlg.sel == 1) to = 0;
    else if (b == Btn::Right && dlg.sel == 0 && n > 1) to = 1;
    else if (b == Btn::Up && dlg.sel < 2 && n > 2) to = 2;
    else if (b == Btn::Up && dlg.sel >= 2 && dlg.sel + 1 < n) to = dlg.sel + 1;
    else if (b == Btn::Down && dlg.sel > 2) to = dlg.sel - 1;
    else if (b == Btn::Down && dlg.sel == 2) to = dlg.lastSel == 0 ? 0 : 1;
    if (to != dlg.sel) {
        dlg.lastSel = dlg.sel;
        dlg.selAt = now;
        dlg.sel = to;
        PlaySfx(Sfx::Move);
    } else if (b == Btn::A && n > 0) {
        PlaySfx(Sfx::Ok);
        dlg.pressAt = now;
        dlg.tinted = true;
        dlg.result = dlg.sel;
    } else if (b == Btn::B) {
        PlaySfx(Sfx::Back);
        dlg.pressAt = now;
        dlg.tinted = false;
        dlg.result = 0; // every dialog puts its safe choice first
    }
}

// The text-entry box is NSW-Home's own (the console brings up a keyboard
// instead). In the second console's style it borrows the dialog's box,
// colours and entrance. It has no exit of its own: it is closed from outside
// this code and is simply no longer drawn.
double entryAt = -1, entrySeen = -1; // when this showing began; the last frame it was drawn

// ---- in-game text entry (a game's software keyboard; see OnEmulatorText) ----

// The text as the box shows it: with the caret, and when it is longer than
// the box (a game may take 500 characters, a nickname 10), its end, which is
// where the typing is.
std::wstring EntryShown(float w, float size) {
    const bool caret = fmod(now, 1.0) < 0.5;
    std::wstring s = entry.text + (caret ? L"|" : L"");
    if (TextWidth(s, size) <= w) return s;
    // The fewest characters dropped from the front that make it fit (a
    // halving search: a long text is measured a handful of times per frame).
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (TextWidth(L"…" + s.substr(mid), size) > w) lo = mid + 1;
        else hi = mid;
    }
    return L"…" + s.substr(lo);
}

// "3/12" at the end of the box, when the game gave a limit.
std::wstring EntryCount() {
    if (!entry.counter) return {};
    return std::to_wstring(entry.text.size()) + L"/" + std::to_wstring(entry.max);
}

// The small print under the box.
std::wstring EntryHint() {
    std::wstring what = entry.numeric ? L"Escribe números con el teclado." : L"Escribe con el teclado.";
    if (OnAndroid()) return what + L"  Intro: aceptar  ·  Atrás: cancelar"; // [android]
    if (entry.pad) return what + L"  Intro o A: aceptar  ·  Esc o B: cancelar";
    return what + L"  Intro: aceptar  ·  Esc: cancelar";
}

// Enter / A, Esc / B. A text shorter than the game allows is not taken.
void EntryAnswer(bool ok) {
    if (ok && entry.text.size() < entry.min) {
        PlaySfx(Sfx::Error);
        return;
    }
    entry.open = false;
    if (ok) {
        if (entry.done) entry.done(entry.text);
        PlaySfx(Sfx::Ok);
    } else {
        if (entry.cancel) entry.cancel();
        PlaySfx(Sfx::Back);
    }
}

// A controller's A and B, for an entry a game is waiting on.
bool EntryButton(Btn b) {
    if (!entry.open || !entry.pad) return false;
    if (b == Btn::A) EntryAnswer(true);
    else if (b == Btn::B) EntryAnswer(false);
    return true; // the rest does nothing here, as before
}

// ---- end of in-game text entry ----

void DrawEntry() {
    Theme T = Th();
    if (S2()) {
        if (entrySeen < 0 || now - entrySeen > 1.5 / 60.0) entryAt = now - 1 / 60.0;
        entrySeen = now;
        const DlgLook L = DL();
        float kf = static_cast<float>((now - entryAt) * 60.0) + 0.001f;
        float box = EaseOut2(kf / 10);
        const Surface menu = DrawDim(EaseOut2(kf / 10.5f));
        const Veil boxVeil = LinVeil(L.box, box, menu);
        const Surface onBox = Covered(menu, boxVeil);
        const float ink = powf(box, 3.4f);
        auto Ink = [&](Color c, Surface on) { return VeilPaint(LinVeil(c, ink, on)); };
        // (A game's second line goes under its first, one small-print line further down.)
        const float extra = entry.sub.empty() ? 0.0f : kDlgSubPitch + kDlgSubGap;
        const float h = kDlgMinH + extra, x = (W - kDlgW) / 2, y = (H - h) / 2, rowTop = y + h - kDlgRow - kDlgRule;
        RRect(x, y, kDlgW, h, kDlgRadius, VeilPaint(boxVeil));
        CapText(entry.title, x + 68.67f, y + 58, kDlgW - 137, kDlgSubSize, Ink(L.sub, onBox));
        if (!entry.sub.empty()) CapText(entry.sub, x + 68.67f, y + 58 + extra, kDlgW - 137, kDlgSubSize, Ink(L.sub, onBox));
        // The field, lit and outlined like a focused button.
        CursorRect f = {x + 68.67f, y + 88 + extra, kDlgW - 137.33f, 62, 6};
        Veil fv = LinVeil(L.focus, ink, onBox);
        RRect(f.x, f.y, f.w, f.h, 5, VeilPaint(fv));
        if (kf >= 10.5f) StrokeCursor(f, 1);
        const Surface onField = Covered(onBox, fv);
        const std::wstring count = EntryCount();
        const float countW = count.empty() ? 0.0f : TextWidth(count, kDlgSubSize) + 16;
        if (!count.empty()) CapText(count, f.x + f.w - 18 - countW, f.y + f.h / 2, countW, kDlgSubSize, Ink(L.sub, onField), Right);
        const float textW = f.w - 36 - countW;
        if (entry.text.empty() && !entry.guide.empty()) { // the game's hint, after the caret
            const bool caret = fmod(now, 1.0) < 0.5;
            const float bar = TextWidth(L"|", kDlgMainSize + 2);
            if (caret) CapText(L"|", f.x + 18, f.y + f.h / 2, bar + 2, kDlgMainSize + 2, Ink(L.text, onField));
            CapText(entry.guide, f.x + 18 + bar, f.y + f.h / 2, textW - bar, kDlgMainSize + 2, Ink(L.sub, onField));
        } else {
            CapText(EntryShown(textW, kDlgMainSize + 2), f.x + 18, f.y + f.h / 2, textW, kDlgMainSize + 2, Ink(L.text, onField));
        }
        Line(x, rowTop + kDlgRule / 2, x + kDlgW, rowTop + kDlgRule / 2, Ink(L.sep, onBox), kDlgRule);
        CapText(EntryHint(), x, rowTop + kDlgRule + kDlgRow / 2, kDlgW, kDlgSubSize, Ink(L.sub, onBox), Center);
        Touch(); // the caret blinks and the outline's colours turn
        return;
    }
    const float extra = entry.sub.empty() ? 0.0f : 30.0f; // a game's second line
    float w = 770, h = 250 + extra, x = (W - w) / 2, y = (H - h) / 2;
    Rect(0, 0, W, H, rgb(0x000000, 0.6f));
    RRect(x, y, w, h, 5, T.dialog);
    Text(entry.title, x + 50, y + 26, w - 100, 40, 22, T.sub);
    if (!entry.sub.empty()) Text(entry.sub, x + 50, y + 26 + extra, w - 100, 40, 19, T.sub);
    RRect(x + 50, y + 78 + extra, w - 100, 64, 4, T.bg);
    RRectStroke(x + 50, y + 78 + extra, w - 100, 64, 4, CursorColor(), 3);
    const std::wstring count = EntryCount();
    const float countW = count.empty() ? 0.0f : TextWidth(count, 19) + 16;
    if (!count.empty()) Text(count, x + w - 68 - countW, y + 78 + extra, countW, 64, 19, T.sub, Right);
    if (entry.text.empty() && !entry.guide.empty()) {
        const bool caret = fmod(now, 1.0) < 0.5;
        const float bar = TextWidth(L"|", 26);
        if (caret) Text(L"|", x + 68, y + 78 + extra, bar + 2, 64, 26, T.text);
        Text(entry.guide, x + 68 + bar, y + 78 + extra, w - 136 - countW - bar, 64, 26, T.sub);
    } else {
        Text(EntryShown(w - 136 - countW, 26), x + 68, y + 78 + extra, w - 136 - countW, 64, 26, T.text);
    }
    Line(x, y + h - 72 + 0.5f, x + w, y + h - 72 + 0.5f, T.sep, 1);
    Text(EntryHint(), x, y + h - 72, w, 72, 19, T.sub, Center);
}

// ---- first-start assistant ------------------------------------------------
// A short run of pages shown before the HOME menu the first time NSW-Home
// starts (cfg.setupDone false), and again whenever it is opened from Settings.
// There is no capture of the console's own first start, so the pages borrow
// what the captures do show: the header and its rule, Settings' rows and box
// cursor, the footer hints. One decision per page; every page starts from
// what is already configured or was found, so pressing A all the way through
// changes nothing. Nothing is saved before a choice is made on a page.

void OpenSetup();

// After the emulator page there are two ways on, which meet again at the
// games: with an emulator, its keys and firmware (kSuKeys); without one ("my
// games are PC programs"), what NSW-Home itself would like of the console's (kSuOwn).
// kSuLang comes before the first page of the earlier assistant, so that every page after it keeps its number.
enum SetupStep { kSuLang = -1, kSuStyle, kSuTheme, kSuEmu, kSuKeys, kSuOwn, kSuGames, kSuUser, kSuDone };

struct SetupRow {
    std::wstring label, value;
    std::function<void()> act; // none: the row only reports something
    bool gap = false;          // set apart from the rows above it (the row that moves on)
    bool quiet = false;        // the value is a detail, not what A does: drawn like small print
    bool mark = false;         // a tick at the right-hand end
    bool pathLabel = false;    // the label is a folder: when too long it loses its start, not its end
    int avatar = -1;           // this user's picture before the label
    int valueAvatar = -1;      // ... or before the value
};

struct SetupEmu {
    std::string id;
    std::wstring exe;
    bool ok; // the program is still where the configuration says
};

struct SetupState {
    int step = kSuStyle, sel = 0;
    float scroll = 0;
    bool first = false;   // the first start: there is nothing behind the first page
    int editUser = -1;    // the user whose nickname and picture page is open
    bool pc = false;      // "I use no emulator": the pages that only matter with one are left out
    int origStyle = 1;    // what to put back when a previewed look is not confirmed
    bool origDark = false;
    bool unsaved = false; // emulators were found by searching and are not in the file yet
    bool rescan = false;  // a search for games is wanted as soon as the running one ends
    std::vector<SetupEmu> emus;
    bool keys = false, firmware = false;
    EmuStatus status; // of the chosen emulator
} su;
Glide setupGlide;

// Test states: what cannot be done offscreen (native pickers, typing) or
// should not depend on this machine (what is installed) is supplied here.
struct SetupDebug {
    bool on = false, noKeys = false, noFw = false;
    std::vector<std::wstring> picks, names; // used up in order
} sudbg;

void SetupRefresh() {
    su.emus.clear();
    for (const auto& e : kEmus) {
        auto it = cfg.emulators.find(e.id);
        if (it == cfg.emulators.end()) continue;
        std::wstring exe;
        su.emus.push_back({e.id, it->second, FindEmulatorIn(it->second, exe) == e.id});
    }
    // What the chosen emulator itself has; with none chosen yet, what there is.
    su.status = EmuInfo(cfg.defaultEmu);
    const bool chosen = cfg.emulators.count(cfg.defaultEmu) != 0;
    su.keys = !sudbg.noKeys && (chosen ? su.status.keys : !KeysDir().empty());
    su.firmware = !sudbg.noFw && (chosen ? su.status.firmware : !FirmwareDir().empty());
}

bool SetupAnyEmu() {
    for (const auto& e : su.emus)
        if (e.ok) return true;
    return false;
}

void SetupRescan() {
    if (scanning) su.rescan = true; // RescanAsync ignores a call made during a search
    else RescanAsync();
}

// kind: 0 folder, 1 program, 2 picture, 3 key file. Empty when cancelled.
std::wstring SetupPick(int kind) {
    if (sudbg.on) {
        if (sudbg.picks.empty()) return {};
        std::wstring p = sudbg.picks.front();
        sudbg.picks.erase(sudbg.picks.begin());
        return p;
    }
    return kind == 0 ? PickFolder() : kind == 1 ? PickExe() : kind == 2 ? PickImage() : PickKeys();
}

void SetupAskName(const std::wstring& title, const std::wstring& text, std::function<void(const std::wstring&)> done) {
    if (sudbg.on && !sudbg.names.empty()) {
        std::wstring typed = sudbg.names.front();
        sudbg.names.erase(sudbg.names.begin());
        done(typed);
        return;
    }
    entry.open = true;
    entry.title = title;
    entry.text = text;
    entry.done = std::move(done);
}

// The one user a first start has: nobody has given it a nickname yet.
bool SetupPlaceholderUser() {
    return cfg.users.size() == 1 && cfg.users[0].name.empty();
}

// The page before `step` (see SetupStep for the two ways).
int SetupPrev(int step) {
    if (step == kSuGames) return su.pc ? kSuOwn : kSuKeys;
    return step == kSuOwn ? kSuEmu : step - 1;
}

void SetupEnter(int step) {
    su.step = step;
    su.scroll = 0;
    setupGlide = Glide();
    su.origStyle = cfg.menuStyle;
    su.origDark = cfg.dark;
    SetupRefresh();
    // The focus starts on what is already so; where nothing is missing, on the row that moves on.
    const int last = 1000; // clamped to the last row when the rows are built
    switch (step) {
    case kSuLang: su.sel = lang::Current(); break;
    case kSuStyle: su.sel = cfg.menuStyle == 2 ? 1 : 0; break;
    case kSuTheme: su.sel = cfg.dark ? 1 : 0; break;
    case kSuEmu:
        su.sel = 0;
        for (int i = 0; i < static_cast<int>(su.emus.size()); i++)
            if (su.emus[i].ok && (su.emus[i].id == cfg.defaultEmu || !su.emus[su.sel].ok)) su.sel = i;
        if (!SetupAnyEmu()) su.sel = static_cast<int>(su.emus.size());
        if (su.pc) su.sel = static_cast<int>(su.emus.size()) + 2; // the "no emulator" row, after the two that choose one
        break;
    case kSuKeys: su.sel = su.keys ? last : 0; break;
    case kSuOwn: su.sel = !FirmwareDir().empty() && !KeysDir().empty() ? last : 0; break;
    case kSuGames: su.sel = (su.pc ? cfg.pcGames.empty() : cfg.gameDirs.empty()) ? 0 : last; break;
    case kSuUser:
        if (su.editUser >= static_cast<int>(cfg.users.size())) su.editUser = -1;
        su.sel = su.editUser >= 0 || SetupPlaceholderUser() ? 0 : last;
        break;
    default: su.sel = 0; break;
    }
}

// Another page of the assistant. In the second console's style the page
// leaves and the next arrives as a page does on the way back to a menu (the
// only page-to-page change the captures time); `before` runs unseen in between.
// `sel`: the row to arrive on, when it is not the page's usual one.
void SetupGo(int step, int editUser = -1, std::function<void()> before = nullptr, int sel = -1) {
    auto apply = [step, editUser, before, sel] {
        if (before) before();
        su.editUser = editUser;
        SetupEnter(step);
        if (sel >= 0) su.sel = sel;
    };
    if (S2()) {
        StartTransition(false, apply);
        trans.homeIn = false; // the HOME menu under the first start's assistant is not what arrives
        trans.inDur = 8 * kFrame;
    } else {
        apply();
        fade = 1;
        Touch();
    }
}

void SetupToHome() {
    auto apply = [] {
        stack = {Scr::Home};
        homeRow = 1;
    };
    if (S2()) {
        StartTransition(false, apply);
        trans.homeIn = true;
        trans.inDur = 7 * kFrame;
    } else {
        apply();
        fade = 1;
        Touch();
    }
}

void SetupReport(const std::string& err) {
    if (err == "NAME_TAKEN") Info(L"Ya hay un usuario con ese apodo.");
    else if (err == "IO") Info(L"No se ha podido guardar el cambio.");
}

// What follows a path coming back from a picker.

void SetupTakeEmulator(const std::wstring& where) {
    if (where.empty()) return;
    std::wstring exe;
    std::string id = FindEmulatorIn(where, exe);
    if (id.empty()) {
        Info(L"Ahí no se ha encontrado ningún emulador.\n\nElige la carpeta o el programa de Ryujinx, Ryubing, Kenji-NX, Eden, Suyu o Citron.");
        return;
    }
    AdoptEmulator(id, exe);
    su.pc = false;
    su.unsaved = false;
    SetupRefresh();
    for (int i = 0; i < static_cast<int>(su.emus.size()); i++)
        if (su.emus[i].id == id) su.sel = i;
    SetupRescan();
}

void SetupTakeKeys(const std::wstring& file) {
    if (file.empty()) return;
    std::string err = InstallKeys(file);
    if (err == "KEYS_INVALID") {
        Info(L"Ese archivo no contiene las claves de la consola.\n\nEl archivo que hace falta se llama prod.keys.");
    } else if (!err.empty()) {
        Info(OnAndroid() ? L"No se ha podido guardar la copia del archivo." // [android]
                         : L"No se ha podido copiar el archivo a la carpeta del emulador.");
    } else {
        SetupRefresh();
        su.sel = 1000;
        SetupRescan(); // the games can be read now
    }
}

void SetupTakeGameDir(const std::wstring& dir) {
    if (dir.empty()) return;
    for (const auto& d : cfg.gameDirs)
        if (!_wcsicmp(d.c_str(), dir.c_str())) return;
    cfg.gameDirs.push_back(dir);
    SaveConfig();
    su.sel = 1000;
    SetupRescan();
}

void SetupTakeImage(int user, const std::wstring& file) {
    if (file.empty() || user >= static_cast<int>(cfg.users.size())) return;
    // Let go of the picture on show first: while it is held its file can be
    // open (it is in offscreen runs) and could not be replaced.
    const std::string id = cfg.users[user].id;
    OpenAvatarFrame(user, cfg.users[user].name, file, PicFrame(), [user, file, id](const PicFrame& f) {
        if (user >= static_cast<int>(cfg.users.size()) || cfg.users[user].id != id) return;
        RefreshAvatar(user);
        SetupReport(SetUserImageFramed(user, file, f));
        RefreshAvatar(user);
    }, nullptr);
}

// The longest tail of a path that fits: where a folder is matters more at its end.
std::wstring SetupFit(const std::wstring& path, float size, float maxW) {
    if (TextWidth(path, size) <= maxW) return path;
    size_t at = 0;
    while ((at = path.find(L'\\', at + 1)) != std::wstring::npos) {
        std::wstring s = L"…" + path.substr(at);
        if (TextWidth(s, size) <= maxW) return s;
    }
    return path;
}

void SetupText(std::wstring& title, std::wstring& note) {
    const wchar_t* later = L"Podrás cambiarlo cuando quieras en Configuración de la consola.";
    switch (su.step) {
    case kSuLang:
        title = su.first ? L"Te damos la bienvenida a NSW-Home.\nElige un idioma." : L"Elige un idioma.";
        note = later;
        break;
    case kSuStyle:
        title = su.first ? L"Te damos la bienvenida a NSW-Home.\nElige el estilo del menú HOME." : L"Elige el estilo del menú HOME.";
        note = later;
        break;
    case kSuTheme:
        title = L"Elige un tema.";
        note = later;
        break;
    case kSuEmu: {
        int ok = 0;
        std::wstring name;
        for (const auto& e : su.emus)
            if (e.ok) {
                ok++;
                name = EmuLabel(e.id);
            }
        if (ok == 0) {
            title = L"No se ha encontrado ningún emulador.";
            note = OnAndroid() ? L"NSW-Home abre los juegos con Eden, Suyu, Lemon o Kenji-NX. Instala uno en este dispositivo " // [android]
                                 L"y aparecerá aquí."
                               : L"NSW-Home abre los juegos de la consola con Ryujinx (o con Citron, Eden o yuzu): indica dónde "
                                 L"está en este PC. Los juegos que son programas de PC no necesitan ninguno.";
        } else if (ok == 1) {
            title = L"Los juegos se abrirán con " + name + L".";
            note = OnAndroid() ? L"Si instalas otro emulador, aparecerá aquí." // [android]
                               : L"Si prefieres otro emulador, indica dónde está.";
        } else {
            title = L"Elige el emulador con el que se abrirán los juegos.";
            note = OnAndroid() ? L"Los emuladores que instales aparecerán aquí." // [android]
                               : L"Si prefieres otro, indica dónde está.";
        }
        break;
    }
    case kSuKeys: {
        const wchar_t* fw = OnAndroid() ? L"En Android el firmware de la consola queda dentro del emulador, fuera del alcance de " // [android]
                                          L"NSW-Home, que usa sus propios iconos y sonidos y una fuente del sistema."
                                        : L"El firmware se instala desde el emulador. Sin él, NSW-Home usa sus propios iconos y sonidos y "
                                          L"una fuente del sistema.";
        if (!su.keys) {
            title = L"Faltan las claves de la consola.";
            note = OnAndroid() ? L"Las claves (prod.keys) se obtienen de tu propia consola. NSW-Home las necesita para leer el " // [android]
                                 L"nombre y la imagen de tus juegos: elige el archivo y guardará una copia privada. "
                                 L"El emulador usa las suyas, que se instalan en él."
                               : L"Las claves (prod.keys) se obtienen de tu propia consola y sin ellas los juegos no se pueden abrir. "
                                 L"Elige el archivo y se copiará a la carpeta del emulador.";
            if (!su.firmware) note += std::wstring(L"\n") + fw;
        } else if (!su.firmware) {
            title = L"Se han encontrado las claves de la consola.";
            note = fw;
        } else {
            title = L"Se han encontrado las claves y el firmware de la consola.";
            note = L"NSW-Home los lee de la carpeta del emulador. No hay que hacer nada más.";
        }
        break;
    }
    case kSuOwn:
        if (!FirmwareDir().empty() && !KeysDir().empty()) {
            title = L"NSW-Home usará los sonidos y la fuente de tu consola.";
            note = L"Los lee de donde los tienes, sin copiarlos. El cambio se verá la próxima vez que abras NSW-Home.";
        } else {
            title = L"Indica, si quieres, el firmware y las claves de tu consola.";
            note = L"Con los dos, NSW-Home usa los sonidos y la fuente exactos de la consola. Sin ellos usa sus propios "
                   L"sonidos y una fuente del sistema.\nLos juegos de PC no los necesitan.";
        }
        break;
    case kSuGames:
        if (su.pc) {
            title = L"Añade tus juegos de PC.";
            note = cfg.pcGames.empty() ? L"Elige el programa de cada juego. Podrás añadir más cuando quieras en Configuración de la consola."
                                       : L"Programas añadidos: " + std::to_wstring(cfg.pcGames.size());
            break;
        }
        title = L"Indica dónde están tus juegos.";
        if (cfg.gameDirs.empty())
            note = OnAndroid() ? L"Añade la carpeta donde los guardas. Puedes añadir más de una." // [android]
                               : L"Añade la carpeta donde los guardas; puedes añadir más de una. Los juegos que son programas "
                                 L"de PC se añaden uno a uno.";
        else if (scanning || su.rescan) note = L"Buscando juegos…";
        else note = L"Juegos encontrados: " + std::to_wstring(games.size());
        break;
    case kSuUser:
        if (su.editUser >= 0 && su.editUser < static_cast<int>(cfg.users.size())) {
            title = cfg.users[su.editUser].name.empty() ? L"Usuario sin apodo" : cfg.users[su.editUser].name;
            note = L"Cambia su apodo o su icono.";
        } else if (cfg.users.size() > 1) {
            title = L"Estos son los usuarios de la consola.";
            note = OnAndroid() ? L"Cada uno tiene su propio tiempo de juego. Elige un usuario para cambiar su apodo o su icono." // [android]
                               : L"Cada uno tiene sus propios datos de guardado. Elige un usuario para cambiar su apodo o su icono.";
        } else if (SetupPlaceholderUser()) {
            title = L"Elige un apodo y un icono, si quieres.";
            note = OnAndroid() ? L"No hacen falta. Tu tiempo de juego se guarda con tu usuario." // [android]
                               : L"No hacen falta. Tus datos de guardado y tu tiempo de juego se guardan con tu usuario.";
        } else {
            title = L"Este es tu usuario.";
            note = L"Puedes cambiar su apodo y su icono.";
        }
        // Whose saves the one user has: the profile the emulator was being used with.
        if (su.editUser < 0 && cfg.users.size() == 1 && !AdoptedProfileName(0, cfg.defaultEmu).empty())
            note += L"\nEn " + EmuLabel(cfg.defaultEmu) + L" es el perfil «" + AdoptedProfileName(0, cfg.defaultEmu) +
                    L"», con los datos de guardado que ya tenía.";
        break;
    default: {
        std::vector<std::wstring> missing;
        if (su.pc) { // nothing of an emulator's is wanted: only the games
            if (cfg.pcGames.empty() && cfg.gameDirs.empty()) missing.push_back(L"los juegos");
        } else {
            if (!SetupAnyEmu()) missing.push_back(L"el emulador");
            if (!su.keys) missing.push_back(L"las claves de la consola");
            if (cfg.gameDirs.empty()) missing.push_back(L"la carpeta de juegos");
        }
        title = L"La configuración inicial ha terminado.";
        if (missing.empty()) {
            note = L"Puedes cambiar estos ajustes cuando quieras en Configuración de la consola.";
        } else {
            note = L"Falta por configurar: ";
            for (size_t i = 0; i < missing.size(); i++)
                note += (i == 0 ? L"" : i + 1 == missing.size() ? L" y " : L", ") + missing[i];
            note += L". Puedes hacerlo más adelante en Configuración de la consola.";
        }
        break;
    }
    }
}

std::vector<SetupRow> SetupRows() {
    std::vector<SetupRow> rows;
    auto next = [&rows](const wchar_t* label, int step) {
        SetupRow r;
        r.label = label;
        r.gap = true;
        r.act = [step] { SetupGo(step); };
        rows.push_back(r);
    };
    // Profiles are mirrored into the emulator, which only reads them at start.
    auto guarded = [](std::function<void()> fn) {
        return [fn] {
            if (!runningId.empty()) Info(L"Cierra el programa abierto antes de cambiar los usuarios.");
            else fn();
        };
    };
    // The user page's own steps: a new user, or a profile an emulator already has.
    auto addUser = [] {
        StartAddUser([](int i) {
            if (i < 0) return;
            su.editUser = -1;
            su.sel = i; // back on the list, on the new user
        });
    };
    switch (su.step) {
    case kSuLang:
        // Each in its own language, as the console lists them; A takes it and moves on.
        for (int i = 0; i < static_cast<int>(lang::All().size()); i++) {
            SetupRow r;
            r.label = lang::All()[i].name;
            r.mark = i == lang::Current();
            r.act = [i] {
                LangChoose(i, true);
                SetupGo(kSuStyle);
            };
            rows.push_back(r);
        }
        break;
    case kSuStyle:
    case kSuTheme: {
        // Moving the cursor already shows the look; A keeps it.
        const bool style = su.step == kSuStyle;
        auto keep = [style] {
            if (style ? cfg.menuStyle != su.origStyle : cfg.dark != su.origDark) SaveConfig();
            SetupGo(style ? kSuTheme : kSuEmu);
        };
        rows.push_back({style ? L"Switch" : L"Blanco básico", L"", keep});
        rows.push_back({style ? L"Switch 2" : L"Negro básico", L"", keep});
        break;
    }
    case kSuEmu: {
        const bool any = SetupAnyEmu();
        for (const auto& e : su.emus) {
            SetupRow r;
            r.label = EmuTitle(e.id);
            r.quiet = true;
            if (e.ok) {
                r.value = OnAndroid() ? e.exe : EmuDataNote(EmuInfo(e.id)); // [android] the app's package; it has no data folder to describe
                r.mark = e.id == cfg.defaultEmu;
                std::string id = e.id;
                r.act = [id] {
                    if (cfg.defaultEmu != id || su.unsaved) {
                        cfg.defaultEmu = id;
                        SaveConfig();
                        su.unsaved = false;
                    }
                    su.pc = false;
                    SetupGo(kSuKeys);
                };
            } else {
                r.value = L"Ya no está en su carpeta";
            }
            rows.push_back(r);
        }
        if (!OnAndroid()) { // [android] installed apps are found by themselves: nothing to point at
            rows.push_back({any ? L"Buscar en otra carpeta…" : L"Elegir la carpeta del emulador…", L"",
                            [] { SetupTakeEmulator(SetupPick(0)); }});
            rows.push_back({L"Elegir el programa del emulador…", L"", [] { SetupTakeEmulator(SetupPick(1)); }});
            // The other way on: no emulator at all (it can still be added later, and so can PC games with one).
            SetupRow none;
            none.label = L"No uso ningún emulador";
            none.value = L"Mis juegos son programas de PC";
            none.quiet = true;
            none.mark = su.pc;
            none.act = [] {
                su.pc = true;
                SetupGo(kSuOwn);
            };
            rows.push_back(none);
        }
        if (!any) {
            next(L"Ahora no", kSuKeys);
            rows.back().act = [] {
                su.pc = false;
                SetupGo(kSuKeys);
            };
        }
        break;
    }
    case kSuOwn:
        for (const bool firmware : {true, false}) {
            SetupRow r;
            r.label = firmware ? L"Firmware de la consola" : L"Claves de la consola";
            const bool have = !(firmware ? FirmwareDir() : KeysDir()).empty();
            r.value = have ? OwnValue(firmware, 1000) : firmware ? L"Elegir carpeta…" : L"Elegir archivo…";
            r.quiet = have; // where it is: a detail (cut at its start when long)
            r.act = [firmware] {
                if (TakeOwn(firmware, SetupPick(firmware ? 0 : 3)) && !firmware) SetupRescan(); // dumps can be read now
            };
            rows.push_back(r);
        }
        next(!FirmwareDir().empty() && !KeysDir().empty() ? L"Siguiente" : L"Ahora no", kSuGames);
        break;
    case kSuKeys:
        // Each emulator looks in its own folder; another one that already
        // has what this one lacks can hand it over.
        if (su.keys) rows.push_back({L"Claves de la consola", L"Encontradas", nullptr});
        else if (!su.status.keysFrom.empty() && !sudbg.noKeys)
            rows.push_back({L"Claves de la consola", L"Copiar las de " + EmuLabel(su.status.keysFrom), [] {
                                EmuReport(GiveKeys(cfg.defaultEmu));
                                SetupRefresh();
                                SetupRescan();
                            }});
        else rows.push_back({L"Claves de la consola", L"Elegir archivo…", [] { SetupTakeKeys(SetupPick(3)); }});
        if (OnAndroid()) { // [android] the firmware is out of reach there: no row for it
        } else if (!su.firmware && !su.status.firmwareFrom.empty() && !sudbg.noFw)
            rows.push_back({L"Firmware de la consola", L"Copiar el de " + EmuLabel(su.status.firmwareFrom), [] {
                                EmuReport(GiveFirmware(cfg.defaultEmu));
                                SetupRefresh();
                            }});
        else rows.push_back({L"Firmware de la consola", su.firmware ? L"Instalado" : L"No instalado", nullptr});
        next(L"Siguiente", kSuGames);
        break;
    case kSuGames:
        for (size_t i = 0; i < cfg.gameDirs.size(); i++) {
            rows.push_back({cfg.gameDirs[i], L"Quitar", [i] {
                Ask(L"¿Quitar esta carpeta de la biblioteca?\n" + cfg.gameDirs[i] + L"\n\nNo se borra ningún archivo.",
                    {L"Cancelar", L"Quitar"}, [i](int b) {
                        if (b != 1 || i >= cfg.gameDirs.size()) return;
                        cfg.gameDirs.erase(cfg.gameDirs.begin() + i);
                        SaveConfig();
                        SetupRescan();
                    });
            }});
            rows.back().pathLabel = true;
        }
        if (!su.pc || !cfg.gameDirs.empty()) rows.push_back({L"Añadir carpeta…", L"", [] { SetupTakeGameDir(SetupPick(0)); }});
        if (!OnAndroid()) { // [android] no programs to start there
            for (const PcGame& g : cfg.pcGames) {
                const std::string id = g.id;
                rows.push_back({g.name, L"Quitar", [id] { PcAskRemove(id, [] { SetupRescan(); }); }});
            }
            rows.push_back({L"Añadir programa de PC…", L"", [] {
                if (!PcAdd(SetupPick(1))) return;
                su.sel++; // stays on this row, which has moved down by the one added
                SetupRescan();
            }});
        }
        next(L"Siguiente", kSuUser);
        break;
    case kSuUser: {
        const int n = static_cast<int>(cfg.users.size());
        if (su.editUser < 0 && n > 1) {
            for (int i = 0; i < n; i++) {
                SetupRow r;
                r.label = cfg.users[i].name.empty() ? L"Sin apodo" : cfg.users[i].name;
                r.avatar = i;
                r.act = [i] { SetupGo(kSuUser, i); };
                rows.push_back(r);
            }
            if (n < 8) rows.push_back({L"Añadir usuario", L"", guarded(addUser)});
            next(L"Siguiente", kSuDone);
            break;
        }
        const int u = su.editUser >= 0 && su.editUser < n ? su.editUser : 0;
        rows.push_back({L"Apodo", cfg.users[u].name.empty() ? L"Elegir…" : cfg.users[u].name, guarded([u] {
            SetupAskName(L"Apodo", cfg.users[u].name, [u](const std::wstring& s) {
                if (!s.empty() && u < static_cast<int>(cfg.users.size())) SetupReport(RenameUser(u, s));
            });
        })});
        SetupRow icon;
        icon.label = L"Icono";
        icon.value = L"Cambiar…";
        icon.valueAvatar = u;
        icon.act = guarded([u] { SetupTakeImage(u, SetupPick(2)); });
        rows.push_back(icon);
        if (su.editUser < 0) {
            rows.push_back({L"Añadir otro usuario", L"", guarded(addUser)});
            next(L"Siguiente", kSuDone);
        } else {
            SetupRow r;
            r.label = L"Aceptar";
            r.gap = true;
            r.act = [u] { SetupGo(kSuUser, -1, nullptr, u); }; // back on the list, on the user that was changed
            rows.push_back(r);
        }
        break;
    }
    default:
        rows.push_back({L"Ir al menú HOME", L"", [] {
            cfg.setupDone = true;
            SaveConfig();
            SetupToHome();
        }});
        break;
    }
    return rows;
}

// The look under the cursor on the two pages that choose one.
void SetupPreview() {
    if (su.step == kSuStyle) cfg.menuStyle = su.sel == 1 ? 2 : 1;
    else if (su.step == kSuTheme) cfg.dark = su.sel == 1;
}

struct SetupLay {
    float x, w, rowH, radius;
};
// The rows are Settings' rows (same height, rules, cursor), in one column
// across the middle of the page instead of beside a list of categories.
SetupLay SuL() {
    if (S2()) return {298.67f, 682.67f, 75, 8};
    return {265, 750, 70, 4};
}

void DrawSetup(float dt) {
    Theme T = Th();
    const bool s2 = S2();
    const SetupLay L = SuL();
    if (su.rescan && !scanning) {
        su.rescan = false;
        RescanAsync();
    }
    Header(5, L"Configuración inicial");

    std::wstring title, note;
    SetupText(title, note);
    const float titleSize = 24.0f, titlePitch = 34.0f, noteSize = kDlgSubSize, notePitch = kDlgSubPitch;
    float capMid = 112 + titlePitch / 2;
    for (const auto& l : WrapText(title, titleSize, L.w)) {
        CapText(l, L.x, capMid, L.w + 8, titleSize, T.text);
        capMid += titlePitch;
    }
    capMid += 6 - (titlePitch - notePitch) / 2;
    if (!note.empty())
        for (const auto& l : WrapText(note, noteSize, L.w)) {
            CapText(l, L.x, capMid, L.w + 8, noteSize, T.sub);
            capMid += notePitch;
        }

    std::vector<SetupRow> rows = SetupRows();
    const int n = static_cast<int>(rows.size());
    su.sel = (std::max)(0, (std::min)(su.sel, n - 1));
    const float gap = 22.0f, top = capMid - notePitch / 2 + 30, bottom = 640, view = bottom - top;
    std::vector<float> ys(rows.size());
    float total = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].gap && i > 0) total += gap;
        ys[i] = total;
        total += L.rowH;
    }
    // Scroll the focused row into view (a long list of folders or users).
    float target = s2 && setupGlide.t0 >= 0 ? setupGlide.to : su.scroll;
    if (n > 0) {
        if (ys[su.sel] < target) target = su.sel == 0 ? 0 : ys[su.sel];
        if (ys[su.sel] + L.rowH > target + view) target = ys[su.sel] + L.rowH - view;
    }
    if (total <= view) target = 0;
    if (MouseOwns(&su.scroll)) target = su.scroll;
    if (s2) {
        GlideTo(setupGlide, su.scroll, target);
    } else {
        Smooth(su.scroll, target, dt);
        if (su.scroll != target) Touch();
    }

    // On the way to another page the cursor goes first, as it does on the console.
    const bool leaving = trans.on && !trans.applied;
    // Dragged, the focus moves onto the nearest row wholly in view.
    const float rowH = L.rowH;
    const int setupDrag = leaving ? -1 :
        AddScroll(L.x - 14, top - 6, L.w + 28, 645 - (top - 6), ScrollOf(su.scroll, total - view, [ys, n, rowH, view](float v) {
            int to = NearestFit(su.sel, n, [&](int i) { return ys[i] - v >= -0.5f && ys[i] + rowH - v <= view + 0.5f; });
            if (to < 0 || to == su.sel) return;
            su.sel = to;
            SetupPreview();
        }));
    PushClip(L.x - 14, top - 6, L.w + 28, 645 - (top - 6));
    for (int i = 0; i < n; i++) {
        const SetupRow& r = rows[i];
        const float ry = top + ys[i] - su.scroll;
        if (ry > 650 || ry + L.rowH < top - 10) continue;
        const bool focused = i == su.sel && !leaving;
        Line(L.x, ry + 0.5f, L.x + L.w, ry + 0.5f, T.sep, 1);
        Line(L.x, ry + L.rowH + 0.5f, L.x + L.w, ry + L.rowH + 0.5f, T.sep, 1);
        if (focused && s2) RRect(L.x - 7, ry - 2, L.w + 13.67f, L.rowH + 5, L.radius, T.focusBg);

        float right = L.x + L.w - 22;
        if (r.mark) {
            Text(L"✓", right - 24, ry, 28, L.rowH, 21, T.accent, Right);
            right -= 40;
        }
        float lx = L.x + 16;
        if (r.avatar >= 0 && r.avatar < static_cast<int>(cfg.users.size())) {
            DrawAvatar(r.avatar, lx + 24, ry + L.rowH / 2, 24);
            lx += 64;
        }
        const float labelW = TextWidth(r.label, 22);
        const float valueSize = r.quiet ? 18.0f : 21.0f;
        // A path is cut at its start, anything else at its end.
        const float valueMax = (std::max)(120.0f, right - lx - (std::min)(labelW, 260.0f) - 40);
        const std::wstring value = r.quiet ? SetupFit(r.value, valueSize, valueMax) : r.value;
        const float vw = (std::min)(TextWidth(value, valueSize), valueMax);
        const float labelMax = right - lx - vw - 18;
        Text(r.pathLabel ? SetupFit(r.label, 22, labelMax - 4) : r.label, lx, ry, labelMax, L.rowH, 22, T.text);
        Text(value, right - vw - 6, ry, vw + 8, L.rowH, valueSize, r.act && !r.quiet ? T.accent : T.sub, Right);
        if (r.valueAvatar >= 0 && r.valueAvatar < static_cast<int>(cfg.users.size()))
            DrawAvatar(r.valueAvatar, right - vw - 44, ry + L.rowH / 2, 24);

        if (focused) {
            if (s2) {
                CursorFill(T.focusBg);
                Cursor(L.x - 7, ry - 2, L.w + 13.67f, L.rowH + 5, L.radius);
            } else {
                Cursor(L.x - 4, ry - 3, L.w + 8, L.rowH + 7, L.radius);
            }
        }
        if (!leaving && ry + L.rowH > top && ry < bottom)
            AddHit(L.x, ry, L.w, L.rowH, [i] {
                if (trans.on) return;
                su.sel = i;
                SetupPreview();
                OnButtonImpl(Btn::A);
            });
    }
    PopClip();
    if (s2 && total > view) {
        float track = view - 8, len = (std::max)(30.0f, track * view / total);
        RRect(L.x + L.w + 28, top + 4 + (std::min)(1.0f, su.scroll / (total - view)) * (track - len), 4, len, 2,
              cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7));
        AddScrollBar(setupDrag, L.x + L.w + 28, top + 4, track, len, total - view);
    }

    std::vector<Hint> hints;
    if (su.step > kSuLang || !su.first) hints.push_back({L"B", L"Atrás", Btn::B});
    hints.push_back({L"A", s2 ? L"Confirmar" : L"Aceptar", Btn::A});
    Footer(hints);
}

void SetupButton(Btn b) {
    const bool s2 = S2();
    std::vector<SetupRow> rows = SetupRows();
    const int n = static_cast<int>(rows.size());
    su.sel = (std::max)(0, (std::min)(su.sel, n - 1));
    switch (b) {
    case Btn::Up:
    case Btn::Down: {
        int to = su.sel + (b == Btn::Up ? -1 : 1);
        if (to < 0 || to >= n) {
            PlaySfx(Sfx::Bump);
            if (s2) BumpCursor(0, b == Btn::Up ? -1.0f : 1.0f);
        } else {
            su.sel = to;
            PlaySfx(Sfx::Move);
            SetupPreview();
        }
        break;
    }
    case Btn::A:
        if (n > 0 && rows[su.sel].act) {
            PlaySfx(Sfx::Ok);
            rows[su.sel].act();
        } else {
            PlaySfx(Sfx::Bump);
        }
        break;
    case Btn::B: {
        // A look that was only being tried goes back to what it was, once the page is out of sight.
        auto restore = [] {
            cfg.menuStyle = su.origStyle;
            cfg.dark = su.origDark;
        };
        if (su.step == kSuUser && su.editUser >= 0) {
            PlaySfx(Sfx::Back);
            SetupGo(kSuUser, -1, nullptr, su.editUser);
        } else if (su.step > kSuLang) {
            PlaySfx(Sfx::Back);
            SetupGo(SetupPrev(su.step), -1, restore);
        } else if (!su.first) {
            PlaySfx(Sfx::Back);
            if (s2) {
                Pop();
                auto pop = trans.apply;
                trans.apply = [restore, pop] {
                    restore();
                    pop();
                };
            } else {
                restore();
                Pop();
            }
        } else {
            PlaySfx(Sfx::Bump);
        }
        break;
    }
    default: break;
    }
}

void SetupBegin(bool first) {
    su = SetupState();
    su.first = first;
    // Look once more for emulators, also when some are known already: one may
    // have been installed since (the search only adds, and keeps the default).
    if (!sudbg.on) {
        const size_t had = cfg.emulators.size();
        FindEmulators();
        su.unsaved = cfg.emulators.size() != had;
    }
    // Opened again by someone who has PC games and no emulator: still that way.
    su.pc = cfg.emulators.empty() && !cfg.pcGames.empty();
    SetupEnter(kSuLang);
}

void OpenSetup() {
    SetupBegin(false);
    Push(Scr::Setup);
}

// Test states: "setup[-<page>][+<option>...]", e.g. "setup-emu+noemu-2l".
//   pages    style (default) theme emu keys own games user form done; "form" is
//            the first user's nickname-and-picture page, "own" the firmware
//            and keys of the way without an emulator
//   options  first        as at the first start (nothing behind the first page)
//            pushed       opened with OpenSetup() from Settings, as its row there does
//            noemu        no emulator configured        nodirs   no game folders
//            nokeys nofw  keys / firmware not found     fresh    only the first start's user
//            pc           the way without an emulator has been chosen
//            pick=<path>  what the next native picker returns (none: cancelled);
//            name=<text>  what the next text entry returns (none: the box opens);
//                         both may be given several times and are used in order
//   A trailing -1 or -1l shows the first console's style, dark or light.
bool DebugSetup(const std::wstring& full) {
    if (full.rfind(L"setup", 0) != 0) return false;
    auto ends = [&full](const wchar_t* s) {
        size_t k = wcslen(s);
        return full.size() > k && full.compare(full.size() - k, k, s) == 0;
    };
    if (ends(L"-2") || ends(L"-2l")) return false; // DebugGoto sets the style and comes back
    std::wstring name = full;
    if (ends(L"-1") || ends(L"-1l")) {
        cfg.menuStyle = 1;
        cfg.dark = ends(L"-1");
        cfg.clock24 = true;
        name = full.substr(0, full.size() - (ends(L"-1") ? 2 : 3));
    }
    sudbg = SetupDebug();
    sudbg.on = true;
    bool first = false, fresh = false, noEmu = false, noDirs = false, pushed = false, find = false, pc = false;
    size_t plus = name.find(L'+');
    std::wstring page = name.substr(0, plus);
    while (plus != std::wstring::npos) {
        size_t end = name.find(L'+', plus + 1);
        std::wstring opt = name.substr(plus + 1, end == std::wstring::npos ? std::wstring::npos : end - plus - 1);
        if (opt == L"first") first = true;
        else if (opt == L"fresh") fresh = true;
        else if (opt == L"pushed") pushed = true;
        else if (opt == L"pc") pc = true;
        else if (opt == L"noemu") noEmu = true;
        else if (opt == L"nodirs") noDirs = true;
        else if (opt == L"find") find = true; // run the real search for installed emulators
        else if (opt == L"nokeys") sudbg.noKeys = true;
        else if (opt == L"nofw") sudbg.noFw = true;
        else if (opt.rfind(L"pick=", 0) == 0) sudbg.picks.push_back(opt.substr(5));
        else if (opt.rfind(L"name=", 0) == 0) sudbg.names.push_back(opt.substr(5));
        plus = end;
    }
    if (noEmu) {
        cfg.emulators.clear();
        cfg.defaultEmu.clear();
    }
    if (noDirs) {
        cfg.gameDirs.clear();
        games.clear();
    }
    if (fresh) {
        cfg.users.resize(1);
        cfg.users[0].name.clear();
        cfg.currentUser = 0;
        cfg.setupDone = false;
    }
    if (pushed) {
        stack = {Scr::Home, Scr::Settings};
        OpenSetup();
        return true;
    }
    SetupBegin(first);
    if (pc) su.pc = true;
    if (find) {
        FindEmulators();
        SetupRefresh();
    }
    stack = {Scr::Home, Scr::Setup};
    static const struct {
        const wchar_t* name;
        int step;
    } pages[] = {{L"setup-lang", kSuLang}, {L"setup-style", kSuStyle}, {L"setup-theme", kSuTheme}, {L"setup-emu", kSuEmu},
                 {L"setup-keys", kSuKeys},   {L"setup-games", kSuGames}, {L"setup-user", kSuUser},
                 {L"setup-form", kSuUser},   {L"setup-done", kSuDone},   {L"setup-own", kSuOwn}};
    for (const auto& p : pages)
        if (page == p.name) {
            su.editUser = page == L"setup-form" ? 0 : -1;
            SetupEnter(p.step);
        }
    return true;
}

// ---- playing together online (netplay.h) -------------------------------------
// The bar's "Switch Online" button opens "Jugar en línea con amigos":
// a page of NSW-Home's own (the console's own online pages need its servers
// and were never captured past the user sheet, anim.mkv 279.5 s), built from
// the Settings page's parts: on the left "Invitaciones", "Cómo funciona" and
// the user's games; on the right, for a game, the friends who are online,
// those playing or owning it first, each with "Invitar". An invitation and its
// answer travel as friends messages; the waiting box says where things stand;
// when both have agreed, each NSW-Home starts the game as one started from
// HOME, with the emulator set up for the session just before (FinishLaunch)
// and put back as it was once the game has ended (OnGameExit).

ListPage netp;
bool npInited = false;
bool npBoxUp = false;          // the waiting box is ours
std::wstring npShown;          // ... with this text
std::string npLaunchGame;      // the game the session starts (FinishLaunch sets the emulator up for it)
bool npApplied = false;        // the session's values are in the emulator's file now
std::map<std::string, netplay::Support> npSupport; // per emulator id, read when the page opens
std::map<std::string, bool> npAnnounced; // invitations already told about on HOME
double npTickAt = 0;

void NetplayEnsure() {
    if (npInited) return;
    npInited = true;
    // A --shot / --shotseq run keeps nothing on disk and simulates the friend.
    netplay::Init(Narrow(DataDir()), userTest);
    // An NSW-Home closed while a session's game was open: put the emulator back now.
    if (netplay::RestorePending() && runningId.empty()) {
        netplay::Restore();
        emuTried = false;
    }
}

int NetplayUser() {
    return (std::max)(0, (std::min)(cfg.currentUser, static_cast<int>(cfg.users.size()) - 1));
}
std::string NetplayOwner() {
    return cfg.users[NetplayUser()].id;
}

std::string NetplayLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// The user's own game with that title ID; nullptr when it is not in the library.
Game* NetplayGameByTitle(const std::string& titleId) {
    for (auto& g : games)
        if (!g.pc && !g.titleId.empty() && NetplayLower(g.titleId) == NetplayLower(titleId)) return &g;
    return nullptr;
}

// Whether the emulator this game starts with can play online, and why not.
const netplay::Support& NetplaySupportFor(const Game& g) {
    const std::string emu = GameEmulator(g.id);
    auto it = npSupport.find(emu);
    if (it == npSupport.end()) {
        it = npSupport.emplace(emu, netplay::CanPlay(emu, EmuLabel(emu), EmuConfigPathOf(emu))).first;
        emuTried = false; // (ryucfg's one open file was this one: Settings reads its own again)
    }
    return it->second;
}

// "Ryubing 1.3.3": the emulator and the version its program carries (read
// once per page: finding the version reads the whole program file).
std::map<std::string, std::wstring> npEmuName;
std::wstring NetplayEmuName(const std::string& emu) {
    auto it = npEmuName.find(emu);
    if (it == npEmuName.end()) {
        const std::wstring v = EmuInfo(emu).version;
        it = npEmuName.emplace(emu, EmuLabel(emu) + (v.empty() ? L"" : L" " + v)).first;
    }
    return it->second;
}

std::wstring NetplayNick(const std::string& code) {
    for (const auto& f : FriendsOf(NetplayUser()))
        if (f.code == code) return FriendNick(f);
    return L"tu amigo";
}

// 2: playing this game now, 1: has played it, 0: neither.
int NetplayRank(const friends::Friend& f, const std::string& titleId) {
    const std::string t = NetplayLower(titleId);
    if (f.st.state == friends::Playing && NetplayLower(f.st.gameId) == t) return 2;
    for (const auto& g : f.st.games)
        if (!g.id.empty() && NetplayLower(g.id) == t) return 1;
    return 0;
}

std::vector<friends::Friend> NetplayOnline() {
    std::vector<friends::Friend> out;
    const int64_t t = friends::UnixNow();
    for (const auto& f : FriendsOf(NetplayUser()))
        if (friends::Online(f, t)) out.push_back(f);
    return out;
}

std::wstring NetplayMinutes(int64_t seconds) {
    const int64_t m = (seconds + 59) / 60;
    return m <= 1 ? std::wstring(L"Queda 1 min.") : L"Quedan " + std::to_wstring(m) + L" min.";
}

Row NetplayText(const std::wstring& text) {
    Row r;
    r.kind = RK::Text;
    r.label = text;
    r.real = true;
    return r;
}

// -- the waiting box --

std::wstring NetplayWaitText() {
    const netplay::Session& s = netplay::Current();
    const std::wstring nick = NetplayNick(s.code);
    if (s.phase == netplay::Phase::Joining) return L"Uniéndote a la partida de " + nick + L"...\nPuede tardar un minuto.";
    if (s.sendState != "stored") return L"Enviando la invitación a " + nick + L"...";
    return L"Esperando a " + nick + L"...\nPuede tardar un minuto.";
}

void NetplayBox() {
    const std::wstring text = NetplayWaitText();
    Ask(text, {L"Cancelar"}, [](int) {
        npBoxUp = false;
        netplay::Cancel();
    });
    dlg.busy = true;
    npShown = text;
    npBoxUp = true;
}

// Back to HOME with the game chosen, and it starts as one started there does
// (the tile's press, the launch screen); FinishLaunch asks NetplayBeforeLaunch.
void NetplayStart(const std::string& id) {
    npLaunchGame = id;
    stack = {Scr::Home};
    homeRow = 1;
    for (int i = 0; i < static_cast<int>(games.size()) && i < 12; i++)
        if (games[i].id == id) gi = i;
    BeginPress(id);
    if (S2()) pressAt = NextFrame() + 2.0 / 60.0;
}

// -- inviting --

void NetplayInvite(const std::string& code, const std::string& gameId) {
    Game* g = FindGame(gameId);
    if (!g) return;
    if (!runningId.empty()) {
        Info(L"Cierra el programa abierto antes de invitar a alguien.");
        return;
    }
    const netplay::Support& sup = NetplaySupportFor(*g);
    if (!sup.ok) {
        Info(sup.why);
        return;
    }
    const std::string emu = GameEmulator(gameId);
    const std::wstring nick = NetplayNick(code);
    Ask(L"¿Quieres invitar a " + nick + L" a jugar a\n" + g->name + L"?\n\nSe jugará con " + NetplayEmuName(emu) + L".", {L"Cancelar", L"Invitar"},
        [code, gameId, emu](int b) {
            Game* gg = FindGame(gameId);
            if (b != 1 || !gg) return;
            netplay::Msg m;
            m.titleId = gg->titleId;
            m.game = Narrow(gg->name);
            m.gameVersion = Narrow(gg->version);
            m.family = netplay::kFamilyRyuLdn;
            m.emulator = emu;
            m.emuVersion = Narrow(EmuInfo(emu).version);
            if (!netplay::Invite(NetplayOwner(), code, m)) {
                Info(L"No se ha podido enviar la invitación.");
                return;
            }
            NetplayBox();
        });
}

// -- answering --

void NetplayAnswerBox(const netplay::Incoming& in) {
    const std::wstring nick = NetplayNick(in.code), game = FriendText(in.inv.game, 60);
    const std::wstring head = nick + L" te invita a jugar a\n" + game + L".";
    const std::string code = in.code;
    auto refuseOnly = [&](const std::wstring& why) {
        Ask(head + L"\n\n" + why, {L"Cerrar", L"Rechazar"}, [code](int b) {
            if (b == 1) netplay::Answer(NetplayOwner(), code, false);
        });
        dlg.sel = 0;
    };
    Game* mine = NetplayGameByTitle(in.inv.titleId);
    if (!mine) return refuseOnly(L"No tienes este juego en tu biblioteca.");
    const netplay::Support& sup = NetplaySupportFor(*mine);
    if (!sup.ok) return refuseOnly(sup.why);
    if (sup.family != in.inv.family)
        return refuseOnly(nick + L" usa un emulador que no puede jugar con el tuyo.");
    std::wstring extra;
    const std::wstring theirs = FriendText(in.inv.gameVersion, 24);
    if (!theirs.empty() && !mine->version.empty() && theirs != mine->version)
        extra = L"\n\nTu versión del juego es la " + mine->version + L" y la de " + nick + L", la " + theirs +
                L".\nCon versiones distintas puede que no os encontréis.";
    const int64_t left = netplay::SecondsLeft(in.inv, in.at, friends::UnixNow());
    const std::string gameId = mine->id;
    Ask(head + extra + L"\n\n" + NetplayMinutes(left), {L"Rechazar", L"Unirse"}, [code, gameId](int b) {
        if (b == 0) {
            netplay::Answer(NetplayOwner(), code, false);
            return;
        }
        if (!runningId.empty()) {
            Info(L"Cierra el programa abierto antes de unirte.");
            return;
        }
        Game* g = FindGame(gameId);
        netplay::Msg m;
        const std::string emu = GameEmulator(gameId);
        m.emulator = emu;
        m.emuVersion = Narrow(EmuInfo(emu).version);
        m.gameVersion = g ? Narrow(g->version) : std::string();
        if (!netplay::Answer(NetplayOwner(), code, true, m)) {
            Info(L"Esta invitación ya no es válida.");
            return;
        }
        NetplayBox();
    });
}

// -- the page --

std::vector<Row> NetplayHomeRows() {
    std::vector<Row> rows;
    const int u = NetplayUser();
    if (!FriendsSharing(u)) {
        rows.push_back(NetplayText(L"Para jugar con amigos, primero tienes que compartir tu actividad con ellos."));
        rows.push_back(ActRow(L"Compartir mi actividad", [u] { FriendsConsent(u, [] {}); }));
        return rows;
    }
    const netplay::Session& s = netplay::Current();
    if (s.phase == netplay::Phase::Inviting || s.phase == netplay::Phase::Joining) {
        rows.push_back(HeadRow(L"En curso"));
        Row r = ValRow(s.host ? L"Invitación a " + NetplayNick(s.code) : L"Partida de " + NetplayNick(s.code),
                       s.phase == netplay::Phase::Joining ? L"Uniéndote" : s.sendState == "stored" ? L"Esperando" : L"Enviando",
                       [] { NetplayBox(); });
        r.note = FriendText(s.inv.game, 60);
        rows.push_back(r);
        rows.push_back(GapRow());
    }
    rows.push_back(HeadRow(L"Invitaciones recibidas"));
    const std::vector<netplay::Incoming> in = netplay::Invitations(NetplayOwner());
    if (in.empty()) rows.push_back(NetplayText(L"No tienes invitaciones."));
    const int64_t t = friends::UnixNow();
    for (const auto& i : in) {
        Row r = ValRow(NetplayNick(i.code) + L" te invita", NetplayMinutes(netplay::SecondsLeft(i.inv, i.at, t)), [i] { NetplayAnswerBox(i); });
        r.note = FriendText(i.inv.game, 60);
        rows.push_back(r);
    }
    return rows;
}

std::vector<Row> NetplayHowRows() {
    std::vector<Row> rows;
    rows.push_back(HeadRow(L"Qué juegos funcionan"));
    rows.push_back(NetplayText(L"Los que tienen un modo de juego local inalámbrico (o en red local). "
                               L"Lo que usa los servidores de la consola, como el juego en línea del propio juego, no funciona."));
    rows.push_back(HeadRow(L"Cómo se juega"));
    rows.push_back(NetplayText(L"1. Elige un juego e invita a un amigo que esté en línea.\n"
                               L"2. Cuando acepte, el juego se abrirá en los dos PC.\n"
                               L"3. En el juego, abrid su modo local inalámbrico: uno crea la partida y el otro se une a ella."));
    rows.push_back(HeadRow(L"Qué hace falta"));
    rows.push_back(NetplayText(L"Los dos necesitáis la misma versión del juego y un emulador de la familia de Ryujinx "
                               L"(Ryubing o Kenji-NX). Ryujinx 1.1, Eden, Citron y Suyu no pueden por ahora.\n"
                               L"La partida pasa por el servidor de juego en red de Ryujinx (ldn.ryujinx.app)."));
    return rows;
}

std::vector<Row> NetplayGameRows(const std::string& id) {
    std::vector<Row> rows;
    Game* g = FindGame(id);
    if (!g) return rows;
    const int u = NetplayUser();
    const netplay::Support& sup = NetplaySupportFor(*g);
    if (!sup.ok) {
        rows.push_back(NetplayText(sup.why));
        return rows;
    }
    if (!FriendsSharing(u)) {
        rows.push_back(NetplayText(L"Para jugar con amigos, primero tienes que compartir tu actividad con ellos."));
        rows.push_back(ActRow(L"Compartir mi actividad", [u] { FriendsConsent(u, [] {}); }));
        return rows;
    }
    rows.push_back(HeadRow(L"Amigos en línea"));
    std::vector<friends::Friend> on = NetplayOnline();
    std::stable_sort(on.begin(), on.end(), [&](const friends::Friend& a, const friends::Friend& b) {
        return NetplayRank(a, g->titleId) > NetplayRank(b, g->titleId);
    });
    if (on.empty())
        rows.push_back(NetplayText(FriendsOf(u).empty() ? L"Aún no tienes amigos. Puedes añadirlos desde tu página de usuario."
                                                        : L"Ninguno de tus amigos está en línea ahora."));
    const bool busy = netplay::Current().phase != netplay::Phase::Idle;
    for (const auto& f : on) {
        const int rank = NetplayRank(f, g->titleId);
        const std::string code = f.code;
        Row r = ValRow(L"Invitar a " + FriendNick(f), rank == 2 ? L"Está jugando" : rank == 1 ? L"Lo tiene" : L"", [code, id] {
            if (netplay::Current().phase != netplay::Phase::Idle) NetplayBox();
            else NetplayInvite(code, id);
        });
        r.disabled = busy;
        rows.push_back(r);
    }
    rows.push_back(GapRow());
    rows.push_back(NetplayText(L"Se jugará con " + NetplayEmuName(GameEmulator(id)) + L"."));
    return rows;
}

void NetplayOpen(int cat) {
    NetplayEnsure();
    npSupport.clear(); // read again: the emulator or its version may have changed
    npEmuName.clear();
    netp = ListPage();
    netp.title = L"Jugar en línea con amigos";
    netp.icon = 0;
    Cat c;
    c.name = L"Invitaciones";
    c.id = "np.invitations";
    c.rows = [] { return NetplayHomeRows(); };
    netp.cats.push_back(c);
    c.name = L"Cómo funciona";
    c.id = "np.how";
    c.rows = [] { return NetplayHowRows(); };
    c.ruleAfter = true;
    netp.cats.push_back(c);
    // The user's games, those friends online play or have first.
    std::vector<const Game*> list;
    for (const auto& g : games)
        if (!g.pc && g.titleId.size() == 16) list.push_back(&g);
    const std::vector<friends::Friend> on = NetplayOnline();
    auto score = [&](const Game* g) {
        int n = 0;
        for (const auto& f : on) n += NetplayRank(f, g->titleId);
        return n;
    };
    std::stable_sort(list.begin(), list.end(), [&](const Game* a, const Game* b) { return score(a) > score(b); });
    for (const Game* g : list) {
        Cat gc;
        gc.name = g->name;
        gc.id = g->id;
        const std::string id = g->id;
        gc.rows = [id] { return NetplayGameRows(id); };
        netp.cats.push_back(gc);
    }
    netp.cat = (std::max)(0, (std::min)(cat, static_cast<int>(netp.cats.size()) - 1));
    Push(Scr::Netplay);
}

void NetplayButton(Btn b) {
    ListButton(netp, b);
}

// -- every frame --

// While the page is open its friends are read more often (FriendsFrame).
bool NetplayWatching() {
    return !stack.empty() && stack.back() == Scr::Netplay;
}

void NetplayFrame() {
    NetplayEnsure();
    if (userTest) netplay::TestTime(now); // (the simulated friend keeps to the frames)
    const bool changed = netplay::Poll();
    const netplay::Session& s = netplay::Current();
    const bool waiting = s.phase == netplay::Phase::Inviting || s.phase == netplay::Phase::Joining;
    if (npBoxUp) {
        const bool ours = dlg.open && dlg.busy && dlg.msg == npShown;
        if (!ours) {
            // The box went some other way (HOME): so does the invitation.
            npBoxUp = false;
            if (waiting) netplay::Cancel();
        } else if (waiting && NetplayWaitText() != npShown) {
            npShown = dlg.msg = NetplayWaitText();
            Touch();
        }
    }
    if (changed) {
        Touch();
        const std::wstring nick = NetplayNick(s.code);
        auto close = [] {
            if (npBoxUp && dlg.open && dlg.msg == npShown) dlg.open = false;
            npBoxUp = false;
        };
        switch (s.phase) {
        case netplay::Phase::Ready: {
            close();
            Game* g = NetplayGameByTitle(s.inv.titleId);
            if (!g || !runningId.empty()) {
                netplay::Cancel();
                Info(!g ? L"No se ha encontrado el juego." : L"Cierra el programa abierto antes de empezar.");
                break;
            }
            NetplayStart(g->id);
            break;
        }
        case netplay::Phase::Declined:
            close();
            Info(nick + L" no puede jugar ahora.");
            netplay::Dismiss();
            break;
        case netplay::Phase::NoAnswer:
            close();
            Info(nick + L" no ha respondido a tiempo.");
            netplay::Dismiss();
            break;
        case netplay::Phase::Unsent:
            close();
            Info(L"No se ha podido establecer la conexión.\n\nComprueba la conexión a internet e inténtalo de nuevo.");
            netplay::Dismiss();
            break;
        case netplay::Phase::Withdrawn:
            close();
            Info(nick + L" ha retirado la invitación.");
            netplay::Dismiss();
            break;
        default: break;
        }
    }
    // A new invitation while HOME is all there is: said once.
    if (stack.size() == 1 && stack.back() == Scr::Home && !dlg.open && !entry.open && !trans.on && runningId.empty() && pressAt < 0 &&
        !cfg.users.empty()) {
        for (const auto& in : netplay::Invitations(NetplayOwner())) {
            if (!npAnnounced.insert({in.inv.nonce, true}).second) continue;
            Ask(NetplayNick(in.code) + L" te invita a jugar a\n" + FriendText(in.inv.game, 60) + L".", {L"Ahora no", L"Ver"}, [](int b) {
                if (b == 1) NetplayOpen(0);
            });
            break;
        }
    }
    // The minutes left on the page and in the box.
    if ((waiting || NetplayWatching()) && now - npTickAt >= 1.0) {
        npTickAt = now;
        Touch();
    }
}

// -- the session's game --

// Called by FinishLaunch right before a game starts: for the session's game,
// the emulator gets the session's values. False: it must not start.
bool NetplayBeforeLaunch(const std::string& id) {
    if (npLaunchGame.empty() || npLaunchGame != id) return true;
    const netplay::Session& s = netplay::Current();
    if (s.phase != netplay::Phase::Ready) {
        npLaunchGame.clear(); // (left meanwhile: an ordinary start)
        return true;
    }
    const std::string emu = GameEmulator(id);
    std::string err;
    const bool ok = netplay::Apply(EmuConfigPathOf(emu), emu, s.inv.passphrase, err);
    emuTried = false;
    if (!ok) {
        netplay::Cancel();
        npLaunchGame.clear();
        PlaySfx(Sfx::Error);
        Info(L"No se ha podido preparar " + EmuLabel(emu) + L" para jugar en línea.\n(" + Widen(err) + L")");
        return false;
    }
    npApplied = true;
    netplay::Launched();
    return true;
}

// The game of the session did not start, or has ended: the emulator's values go back.
void NetplayGameEnded(const std::string& id) {
    if (npLaunchGame.empty() || (!id.empty() && id != npLaunchGame)) return;
    if (npApplied) netplay::Restore();
    npApplied = false;
    emuTried = false;
    npLaunchGame.clear();
    netplay::Ended();
}
void NetplayLaunchFailed() {
    NetplayGameEnded({});
}

// Test states (--shot / --shotseq), with made-up friends and the friend's
// answer simulated (netplay::Init offline):
//   netplay              the page, "Invitaciones" focused, one invitation received
//   netplay-game         a game's friends, the cursor on the first ("Invitar a ...")
//   netplay-confirm      the "¿Quieres invitar...?" box
//   netplay-sending      the waiting box while the invitation is written
//   netplay-wait         the waiting box: "Esperando a ... puede tardar un minuto"
//   netplay-answer       an invitation received, opened: [Rechazar] [Unirse]
//   netplay-joining      after "Unirse"
//   netplay-declined / -noanswer / -unsent / -withdrawn   the outcomes
//   netplay-unsupported  a game that starts with Ryujinx 1.1: the reason
//   netplay-noshare      sharing off
//   netplay-how          "Cómo funciona"
//   netplay-popup        HOME, an invitation arrives
//   netplay-bar          HOME with the cursor on the button (netplay-bar-off: identical mode off)
//   netplay-run          the page on a game's first friend, for a scripted run:
//                        A (invite) A (confirm), the friend accepts after 4 s,
//                        the game starts (simulated); EXIT puts the emulator back
// The session's game starts with Ryubing (the copy in <data>\ryubing).
bool DebugNetplay(const std::wstring& name) {
    if (name.rfind(L"netplay", 0) != 0) return false;
    if (name.size() > 2 && (name.compare(name.size() - 2, 2, L"-2") == 0 || name.compare(name.size() - 3, 3, L"-2l") == 0 ||
                            name.compare(name.size() - 2, 2, L"-1") == 0 || name.compare(name.size() - 3, 3, L"-1l") == 0))
        return false; // DebugGoto sets the style and comes back
    userTest = true;
    FriendsEnsure();
    NetplayEnsure();
    netplay::TestTime(now);
    const std::string owner = NetplayOwner();
    const int64_t t = friends::UnixNow();
    // Two Switch games of the library: the first starts with Ryubing, the second with Ryujinx 1.1.
    std::vector<Game*> sw;
    for (auto& g : games)
        if (!g.pc && g.titleId.size() == 16) sw.push_back(&g);
    if (sw.empty()) return false;
    Game* g0 = sw[0];
    Game* g1 = sw.size() > 1 ? sw[1] : sw[0];
    cfg.perGame[g0->id] = "ryubing"; // (in memory: a test run saves nothing)
    if (g1 != g0) cfg.perGame[g1->id] = "ryujinx";
    auto fake = [&](const char* code, const char* nick, int colour, int state, const Game* playing, const Game* owns, int64_t ago) {
        friends::Friend f;
        f.code = code;
        f.known = true;
        f.seen = t - ago;
        f.st.nick = nick;
        f.st.colour = colour;
        f.st.state = state;
        f.st.time = t - ago;
        if (playing) f.st.game = Narrow(playing->name), f.st.gameId = playing->titleId;
        if (owns) f.st.games = {{owns->titleId, Narrow(owns->name), 7200}};
        return f;
    };
    friends::TestAddFriend(owner, fake("100000000002", "Prueba B", 2, friends::InMenu, nullptr, nullptr, 60));
    friends::TestAddFriend(owner, fake("100000000003", "Prueba C", 4, friends::InMenu, nullptr, g0, 200));
    friends::TestAddFriend(owner, fake("100000000001", "Prueba A", 1, friends::Playing, g0, g0, 30));
    friends::TestAddFriend(owner, fake("100000000004", "Prueba D", 5, friends::SignedOff, nullptr, g0, 3 * 3600));
    friends::SetSharing(owner, name != L"netplay-noshare");
    auto incoming = [&](const char* code, const Game* g, int64_t ago) {
        netplay::Incoming in;
        in.code = code;
        in.at = t - ago;
        in.inv.kind = netplay::kInvite;
        in.inv.nonce = std::string(16, static_cast<char>('a' + ago % 20));
        in.inv.time = t - ago;
        in.inv.expires = in.inv.time + netplay::kLifetime;
        in.inv.titleId = g->titleId;
        in.inv.game = Narrow(g->name);
        in.inv.gameVersion = name == L"netplay-answer" ? "1.2.0" : Narrow(g->version);
        in.inv.family = netplay::kFamilyRyuLdn;
        in.inv.emulator = "kenjinx";
        in.inv.emuVersion = "2.0.5";
        in.inv.passphrase = "Ryujinx-0a1b2c3d";
        return in;
    };
    const netplay::Incoming inv = incoming("100000000001", g0, 75);
    if (name == L"netplay" || name == L"netplay-answer" || name == L"netplay-popup" || name == L"netplay-joining")
        netplay::TestAddIncoming(owner, inv);
    if (name == L"netplay") netplay::TestAddIncoming(owner, incoming("100000000003", g1, 320));
    stack = {Scr::Home};
    if (name == L"netplay-popup") return true; // NetplayFrame says it on the first frame
    if (name == L"netplay-bar" || name == L"netplay-bar-off") {
        if (name == L"netplay-bar-off") cfg.identical = false; // (in memory) the button stays: it works here
        homeRow = 2;
        for (int i = 0; i < SysCount(); i++)
            if (SysList()[i].action == 11) si = i;
        return true;
    }
    trans = Transition();
    NetplayOpen(0);
    trans = Transition();
    stack = {Scr::Home, Scr::Netplay};
    auto gameCat = [&](const Game* g) {
        for (int i = 0; i < static_cast<int>(netp.cats.size()); i++)
            if (netp.cats[i].id == g->id) netp.cat = i;
    };
    auto session = [&](netplay::Phase p, bool host, const char* sendState) {
        netplay::Session s;
        s.phase = p;
        s.host = host;
        s.owner = owner;
        s.code = "100000000002";
        s.inv = incoming("100000000002", g0, 10).inv;
        s.since = t;
        s.sendState = sendState;
        netplay::TestSetSession(s);
    };
    if (name == L"netplay") netp.inRows = false;
    else if (name == L"netplay-how") netp.cat = 1;
    else if (name == L"netplay-game" || name == L"netplay-run" || name == L"netplay-confirm" || name == L"netplay-noshare") {
        gameCat(g0);
        netp.inRows = name != L"netplay-noshare";
        netp.row = 1;
        if (name == L"netplay-confirm") NetplayInvite("100000000001", g0->id);
    } else if (name == L"netplay-unsupported") {
        gameCat(g1);
    } else if (name == L"netplay-answer" || name == L"netplay-joining") {
        netp.inRows = true;
        netp.row = 1;
        if (name == L"netplay-answer") NetplayAnswerBox(inv);
        else {
            netplay::TestAnswer(-1, 0, 1e9); // the answer is still being written
            session(netplay::Phase::Joining, false, "writing");
            NetplayBox();
        }
    } else if (name == L"netplay-sending" || name == L"netplay-wait") {
        // Nobody answers: the box stays (and, "sending", the write never ends).
        netplay::TestAnswer(-1, 0, name == L"netplay-sending" ? 1e9 : 0);
        gameCat(g0);
        netp.inRows = true;
        netp.row = 1;
        session(netplay::Phase::Inviting, true, name == L"netplay-wait" ? "stored" : "writing");
        NetplayBox();
    } else if (name == L"netplay-declined" || name == L"netplay-noanswer" || name == L"netplay-unsent" || name == L"netplay-withdrawn") {
        gameCat(g0);
        netp.inRows = true;
        netp.row = 1;
        const netplay::Phase p = name == L"netplay-declined"  ? netplay::Phase::Declined
                                 : name == L"netplay-noanswer" ? netplay::Phase::NoAnswer
                                 : name == L"netplay-unsent"   ? netplay::Phase::Unsent
                                                               : netplay::Phase::Withdrawn;
        session(p, p != netplay::Phase::Withdrawn, "stored");
        const std::wstring nick = NetplayNick("100000000002");
        Info(p == netplay::Phase::Declined   ? nick + L" no puede jugar ahora."
             : p == netplay::Phase::NoAnswer ? nick + L" no ha respondido a tiempo."
             : p == netplay::Phase::Unsent   ? std::wstring(L"No se ha podido establecer la conexión.\n\nComprueba la conexión a internet e inténtalo de nuevo.")
                                             : nick + L" ha retirado la invitación.");
        netplay::Dismiss();
    }
    return true;
}

void DrawScreen(Scr s, float dt) {
    switch (s) {
    case Scr::Home: DrawHome(dt); break;
    case Scr::All: DrawAll(dt); break;
    case Scr::Settings: DrawList(settings, dt); break;
    case Scr::Options: DrawList(options, dt); break;
    case Scr::Album: DrawAlbum(dt); break;
    case Scr::Viewer: DrawViewer(); break;
    case Scr::Pads: DrawPads(); break;
    case Scr::User: DrawUser(); break;
    case Scr::Who: DrawWho(); break;
    case Scr::Launch: DrawLaunch(); break;
    case Scr::Sleep: DrawSleep(); break;
    case Scr::Setup: DrawSetup(dt); break;
    case Scr::Page: DrawPage(dt); break;
    case Scr::Lock: DrawLock(); break;
    case Scr::Extra: DrawExtra(dt); break;
    case Scr::PadTest: DrawPadTest(); break;
    case Scr::PadCal: DrawPadCal(); break;
    case Scr::Remap: DrawRemap(); break;
    case Scr::Frame: DrawAvatarFrame(dt); break;
    case Scr::Web: DrawWeb(); break;
    case Scr::Amiibo: DrawAmiibo(dt); break;
    case Scr::Netplay: DrawList(netp, dt); break;
    }
}

void OnButtonImpl(Btn b) {
    if (PcInput()) return;
    Touch();
    if (A11yButton(b)) return;
    if (EntryButton(b)) return;
    if (entry.open || trans.on) return;
    if (b == Btn::A) StartPress();
    if (dlg.open) {
        DialogButton(b);
        return;
    }
    switch (stack.back()) {
    case Scr::Home: HomeButton(b); break;
    case Scr::All: AllButton(b); break;
    case Scr::Settings: ListButton(settings, b); break;
    case Scr::Options: ListButton(options, b); break;
    case Scr::Album: AlbumButton(b); break;
    case Scr::Viewer: ViewerButton(b); break;
    case Scr::Who: WhoButton(b); break;
    case Scr::User: UserButton(b); break;
    case Scr::Pads: PadsButton(b); break;
    case Scr::Launch:
        if (b == Btn::B) {
            PlaySfx(Sfx::Back);
            CloseGame();
        } else if (b == Btn::Y) {
            RevealGame();
        }
        break;
    case Scr::Sleep: SleepPress(static_cast<int>(b)); break;
    case Scr::Setup: SetupButton(b); break;
    case Scr::Page: PageButton(b); break;
    case Scr::Lock: LockButton(b); break;
    case Scr::Extra: ExtraButton(b); break;
    case Scr::PadTest: PadTestButton(b); break;
    case Scr::PadCal: PadCalButton(b); break;
    case Scr::Remap: RemapButton(b); break;
    case Scr::Frame: AvatarFrameButton(b); break;
    case Scr::Web: WebButton(b); break;
    case Scr::Amiibo: AmiiboButton(b); break;
    case Scr::Netplay: NetplayButton(b); break;
    }
}

} // namespace

// ---- public ---------------------------------------------------------------

void Init() {
    LangInit();
    BuildSettings();
    PcInit();
    PadsConfigApply();
}

void FirstStart() {
    if (cfg.setupDone) return;
    SetupBegin(true);
    stack = {Scr::Home, Scr::Setup};
}

void OnButton(Btn b) {
    MouseStop();
    OnButtonImpl(b);
}

void OnMouse(float x, float y, bool click) {
    if (PcInput()) return;
    lastMouseMove = now;
    Touch();
    if (AvatarFrameMouse(x, y, click ? 0 : 1)) return; // dragging the picture being framed
    if (!click) {
        MouseMove(x, y);
        return;
    }
    if (entry.open) return;
    MousePress(x, y);
}

void OnMouseUp(float x, float y) {
    PcInput();
    lastMouseMove = now;
    Touch();
    AvatarFrameMouse(x, y, 2); // the end of a drag of the picture being framed
    MouseRelease(x, y);
}

void OnWheel(float delta) {
    if (AvatarFrameWheel(delta)) return; // zooms the picture being framed
    MouseStop();
    Btn b = delta > 0 ? Btn::Up : Btn::Down;
    if (!dlg.open && stack.back() == Scr::Home && homeRow == 1) b = delta > 0 ? Btn::Left : Btn::Right;
    OnButtonImpl(b);
}

void Draw(double t, float dt) {
    now = t;
    A11yFrame(dt);
    FriendsFrame();
    NetplayFrame();
    UpdFrame();
    Theme T = Th();
    hits.clear();
    scrollers.clear();
    MouseFrame(dt);
    Begin(T.bg);
    PadsFrame();
    // Only the top layer shows a cursor.
    float cover = coverNow = TransitionCover(); // may switch screens, so before drawing
    suppressCursor = dlg.open || entry.open;
    DrawScreen(stack.back(), dt);
    suppressCursor = false;
    WebFrame();
    if (trans.on) {
        Touch();
        if (cover > 0) Rect(0, 0, W, 645, alpha(T.bg, cover));
    }
    if (fade > 0) {
        Rect(0, 0, W, H, alpha(stack.back() == Scr::Launch || stack.back() == Scr::Viewer ? rgb(0x000000) : T.bg, fade));
        fade = (std::max)(0.0f, fade - dt / (S2() ? 0.10f : 0.16f));
        Touch();
    }
    if (dlg.open) {
        hits.clear(); // a dialog blocks everything under it
        DrawDialog(dt);
    }
    if (entry.open) {
        hits.clear();
        DrawEntry();
    } else {
        entry.Plain();
    }
    if (debugNotice) { // (the real one is a window of its own over the game: see RequestCapture)
        PushOffset(24, 24);
        DrawNotice(debugNotice == 1);
        PopTransform();
    }
    MouseCheck();
    EndCursors(dt);
    PcFrame(dt);
    A11yEndFrame();
}

bool Animating() {
    return now < activeUntil;
}

void OnLibraryChanged() {
    if (selectFirstAfterScan) {
        gi = 0;
        homeScroll = 0;
        selectFirstAfterScan = false;
    }
    Touch();
}

void OnGameExit(const std::string& id, bool quick) {
    // Second console: when the menu is what is on screen (the game was closed
    // from it), nothing fades but the game's "in use" mark; when the game
    // itself was in front, the menu comes back as it does for HOME.
    bool s2 = cfg.menuStyle == 2;
    bool menuShowing = stack.size() == 1 && stack.back() == Scr::Home;
    bool fromGame = stack.back() == Scr::Launch;
    stack = {Scr::Home};
    homeRow = 1;
    pressAt = -1;
    flow.sheetAt = flow.leaveAt = flow.bareAt = flow.xAt = -1;
    flow.closing = false;
    launchOutAt = -1;
    if (s2 && menuShowing) {
        flow.endAt = now + 1.0 / 60.0;
        flow.endedId = id;
    } else if (s2 && fromGame) {
        flow.homeAt = now + 1.0 / 60.0;
    } else {
        fade = 1;
    }
    dlg.open = false;
    // The game just played moves to the front of the row.
    selectFirstAfterScan = true;
    NetplayGameEnded(id); // an online session's values leave the emulator's file
    EmuFlush(); // emulator settings chosen while the game was open
    if (quick) {
        Game* g = FindGame(id);
        PlaySfx(Sfx::Error);
        PcLogError(id, "QUICK");
        Info(L"El programa se ha cerrado nada más empezar.\n" + (g ? g->name : L"") +
             (g && g->pc ? L"" : L"\n\nComprueba el emulador en Configuración de la consola."));
    }
    // Closed in order to change user: ask who plays and start it again.
    if (!quick && !relaunchId.empty() && relaunchId == id) {
        StartDelay(0.5f, [id] {
            if (FindGame(id)) StartLaunch(id);
        });
    }
    relaunchId.clear();
    Touch();
}

void OnHomePressed() {
    if (A11yHome()) return; // the first of two presses, or the second: the zoom's
    if (LaunchCovering()) return; // nothing to go back from yet
    if (PcHome()) return;
    if (WebHome()) return; // the web page closes the console's way
    PlaySfx(Sfx::Home);
    dlg.open = false;
    entry.open = false;
    pressAt = -1;
    flow.sheetAt = flow.leaveAt = flow.bareAt = flow.xAt = -1;
    if (cfg.menuStyle == 2 && stack.back() == Scr::Launch) flow.homeAt = now + 1.0 / 60.0; // back from the game
    else if (stack.size() > 1 || stack.back() != Scr::Home) fade = 1;
    stack = {Scr::Home};
    homeRow = 1;
    // Land on the running game, like the console does.
    for (int i = 0; i < static_cast<int>(games.size()) && i < 12; i++)
        if (games[i].id == runningId) gi = i;
    Touch();
}

// The game's first picture is about to be shown: the loading mark fades out
// (12 frames). The window of the game is shown once that has finished.
void OnGameReady() {
    if (launchOutAt < 0) launchOutAt = now + 1.0 / 60.0;
    Touch();
}

// The emulator's own message as a dialog of ours (main.cpp, the game viewer,
// reads it and presses the emulator's button for the one chosen here).
static std::wstring emuDialogMsg;

void OnEmulatorDialog(const std::wstring& text, const std::vector<std::wstring>& buttons, std::function<void(int)> done) {
    entry.open = false;
    // The emulator's own message, in its own language: not the table's to report. (Its buttons
    // are short words that NSW-Home's own boxes use too: those stay reported.)
    lang::Ignore(text);
    Ask(text, buttons, std::move(done));
    dlg.sel = 0; // the choice that changes least, which the emulator's own dialog also starts on
    emuDialogMsg = dlg.msg;
}

void CloseEmulatorDialog() {
    if (dlg.open && !emuDialogMsg.empty() && dlg.msg == emuDialogMsg) dlg.open = false;
    if (entry.open && entry.pad) entry.open = false; // the game's keyboard went by itself (answered with the controller in the emulator)
    emuDialogMsg.clear();
    Touch();
}

// ---- in-game text entry: the public side ----

// A game opened its software keyboard (main.cpp reads it from the emulator's
// window): NSW-Home's own entry, with what the game asked for.
void OnEmulatorText(const EmuTextAsk& ask, std::function<void(bool ok, const std::wstring& text)> done) {
    dlg.open = false;
    entry.Plain();
    entry.open = true;
    // With no first line from the game, the console's keyboard shows none
    // either; ours needs a title over the box.
    entry.title = ask.header.empty() ? std::wstring(L"Escribe el texto") : ask.header;
    // The game's own lines, in the game's language: not the table's to report.
    for (const std::wstring* s : {&ask.header, &ask.sub, &ask.guide, &ask.text}) lang::Ignore(*s);
    entry.sub = ask.sub;
    entry.guide = ask.guide;
    entry.text = ask.text;
    // A game may give no limit at all (Ryujinx then says nothing about one):
    // the console's longest single entry is 500 characters.
    entry.max = ask.maxLen ? ask.maxLen : 500;
    entry.counter = ask.maxLen != 0;
    if (entry.text.size() > entry.max) entry.text.resize(entry.max);
    entry.min = ask.minLen;
    entry.numeric = ask.numeric;
    entry.pad = true;
    entry.done = [done](const std::wstring& s) {
        if (done) done(true, s);
    };
    entry.cancel = [done] {
        if (done) done(false, std::wstring());
    };
    PlaySfx(Sfx::Dialog);
    Touch();
}

// Test states of the in-game entry: "swkbd" a name with the game's two lines
// and a limit; "swkbd-guide" an empty box showing the game's hint; "swkbd-num"
// a number; "swkbd-long" a text longer than the box.
static bool DebugIngame(const std::wstring& name) {
    EmuTextAsk a;
    if (name == L"swkbd") {
        a.header = L"¿Cómo quieres que te llamen?";
        a.sub = L"Podrás cambiarlo más adelante en el menú de opciones.";
        a.text = L"Link";
        a.maxLen = 10;
        a.minLen = 1;
    } else if (name == L"swkbd-guide") {
        a.header = L"Buscar";
        a.guide = L"Escribe el nombre de una canción";
        a.maxLen = 40;
    } else if (name == L"swkbd-num") {
        a.header = L"Introduce el código de la sala.";
        a.text = L"4821";
        a.maxLen = 8;
        a.minLen = 8;
        a.numeric = true;
    } else if (name == L"swkbd-long") {
        a.header = L"Escribe un mensaje para tus amigos.";
        a.text = L"Hoy he conseguido por fin la última estrella del mundo de hielo, ¿alguien sabe dónde está la llave dorada?";
    } else {
        return false;
    }
    OnEmulatorText(a, nullptr);
    return true;
}

// ---- end of in-game text entry: the public side ----

bool WantsText() {
    // The friend-code pad takes digits from the keyboard too. [android] Not
    // there: asking for text would bring the screen keyboard up over the pad.
    return entry.open || PcWantsText() || (!OnAndroid() && FriendPadOpen());
}

void OnChar(wchar_t c) {
    if (PcChar(c)) return;
    Touch();
    if (!entry.open && FriendPadChar(c)) return;
    if (c == L'\r') {
        EntryAnswer(true);
    } else if (c == 27) {
        EntryAnswer(false);
    } else if (c == L'\b') {
        if (!entry.text.empty()) entry.text.pop_back();
    } else if (c == 0x16 && entry.open) { // Ctrl+V: what the clipboard holds, on one line, up to the limit
        for (wchar_t p : ClipboardText()) {
            if (entry.text.size() >= entry.max) break;
            if (p == L'\r' || p == L'\n' || p == L'\t') p = L' ';
            if (p >= 32 && (!entry.numeric || (p >= L'0' && p <= L'9'))) entry.text += p;
        }
        while (!entry.text.empty() && entry.text.back() == L' ') entry.text.pop_back();
    } else if (entry.numeric && (c < L'0' || c > L'9')) {
        if (c >= 32) PlaySfx(Sfx::Error); // a game that wants a number takes nothing else
    } else if (c >= 32 && entry.text.size() < entry.max) {
        entry.text += c;
    }
}

bool MouseActive() {
    return now - lastMouseMove < 2.5;
}

void DrawCaptureNotice(bool ok) {
    DrawNotice(ok);
}

void CaptureNoticeSize(bool ok, float& w, float& h) {
    w = NoticeW(ok);
    h = kNoticeH;
}

// Test states of the capture work: "capture-notice" / "capture-failed" show
// the notice in its corner of the menu; "album-sort" is the test album with
// the sort button focused. (The viewer: A on "album-demo" in a --shotseq run.)
static bool DebugCapture(const std::wstring& name) {
    if (name == L"capture-notice" || name == L"capture-failed") {
        debugNotice = name == L"capture-notice" ? 1 : 2;
        return true;
    }
    if (name == L"album-sort") {
        albumDemo = true;
        OpenAlbum();
        albRail = true;
        return true;
    }
    return false;
}

// Test states of the mouse work, for dragging lists in a --shotseq run:
// "mouse-all" the software grid with the library repeated to 40 entries;
// "mouse-pick" Settings with a list of 16 choices open over it.
static bool DebugMouse(const std::wstring& name) {
    if (name == L"mouse-all" && !games.empty()) {
        for (size_t i = 0, have = games.size(); games.size() < 40; i++) games.push_back(games[i % have]);
        allSel = 0;
        allScroll = 0;
        PushNow(Scr::All);
        return true;
    }
    if (name == L"mouse-pick") {
        OpenSettings(kNSWHomeCat);
        std::vector<std::wstring> items;
        for (int i = 1; i <= 16; i++) items.push_back(L"Opción " + std::to_wstring(i));
        Pick(L"Elige una", items, 2, nullptr);
        return true;
    }
    return false;
}

// ---- the language tool's walk through every row (tools/lang_missing.py) ----
// A random walk reaches a page deep in the list only by luck, and texts that a
// row shows only once it is pressed (its picker, its page, its box) not at all:
// these states press each row once, so that every one of them is drawn.
//   langrows:<file>              how many rows the cursor can take in each section of
//                                Settings ("s"), of the first game's options ("o") and
//                                of the first user's page ("u"): "s <section> <rows>" lines
//   langrow:<s|o|u>:<n>:<k>      that page, section n, its k-th such row focused and
//                                pressed as A would; what it opens is what gets drawn
//   langrow:<s|o|u>:<n>:<k>:<j>  the same, and then the j-th choice of what that opened
//                                (an item of its list, a row of its page, a button of its box)
//   langrow:<s|o|u>:<n>:<k>@<file>  the press, and the number of those choices written there
namespace { // (declared beside DebugLang, which hands these states on)
std::vector<int> LangRowsOf(const std::vector<Row>& rows) {
    std::vector<int> out;
    for (size_t i = 0; i < rows.size(); i++)
        if (!rows[i].heading && rows[i].kind != RK::Gap) out.push_back(static_cast<int>(i));
    return out;
}

std::vector<int> LangRowsOf(const std::vector<URow>& rows) {
    std::vector<int> out;
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].Focusable()) out.push_back(static_cast<int>(i));
    return out;
}

// The second console's pages come in after a transition: the next press needs them there.
void LangSettle() {
    while (trans.on) {
        auto apply = trans.apply;
        trans = Transition();
        if (apply) apply();
    }
}

// What a press opened offers this many choices (a box with one button: nothing behind it).
int LangRowChoices() {
    if (picker.open) return static_cast<int>(picker.items.size());
    if (dlg.open) return dlg.btns.size() > 1 ? static_cast<int>(dlg.btns.size()) : 0;
    if (stack.back() == Scr::Page && pcPage.rows) return static_cast<int>(LangRowsOf(pcPage.rows()).size());
    return 0;
}

void LangRowChoose(int j) {
    if (j < 0 || j >= LangRowChoices()) return;
    if (picker.open) {
        PickerChoose(j);
    } else if (dlg.open) {
        CloseDialog(j);
    } else {
        const std::vector<Row> rows = pcPage.rows();
        pcPage.row = LangRowsOf(rows)[j];
        PagePress(rows[pcPage.row]);
    }
    LangSettle();
}

bool DebugLangRows(const std::wstring& full) {
    const bool count = full.rfind(L"langrows:", 0) == 0;
    if (!count && full.rfind(L"langrow:", 0) != 0) return false;
    const size_t n = full.size();
    if ((n > 2 && full.compare(n - 2, 2, L"-2") == 0) || (n > 3 && full.compare(n - 3, 3, L"-2l") == 0))
        return false; // DebugGoto sets the style and comes back
    // Through every other test block once with nothing to do: that is what
    // marks the run as a test (no tone, no microphone, nothing on the desktop).
    DebugGoto(L"");
    auto write = [](const std::wstring& file, const std::string& out) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, file.c_str(), L"wb") && f) {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }
    };
    if (count) {
        std::string out;
        BuildSettings();
        for (size_t c = 0; c < settings.cats.size(); c++)
            out += "s " + std::to_string(c) + " " + std::to_string(LangRowsOf(settings.cats[c].rows()).size()) + "\n";
        if (!games.empty()) {
            OpenOptions(games[0].id);
            for (size_t c = 0; c < options.cats.size(); c++)
                out += "o " + std::to_string(c) + " " + std::to_string(LangRowsOf(options.cats[c].rows()).size()) + "\n";
        }
        if (!cfg.users.empty()) {
            const std::vector<USection> secs = UserSections();
            for (size_t s = 0; s < secs.size(); s++)
                out += "u " + std::to_string(s) + " " + std::to_string(LangRowsOf(UserRows(secs[s].id, 0)).size()) + "\n";
        }
        write(full.substr(9), out);
        return true;
    }
    const size_t at = full.find(L'@');
    const std::wstring name = full.substr(0, at);
    const wchar_t kind = name.size() > 8 ? name[8] : L's';
    std::vector<int> num; // section, row, choice
    for (size_t i = 9; i < name.size() && num.size() < 3; i++)
        if (name[i] == L':') num.push_back(static_cast<int>(wcstol(name.c_str() + i + 1, nullptr, 10)));
    const int sec = num.size() > 0 ? num[0] : 0, k = num.size() > 1 ? num[1] : 0;
    bool pressed = false;
    if (kind == L'u') {
        const std::vector<USection> secs = cfg.users.empty() ? std::vector<USection>() : UserSections();
        if (sec >= 0 && sec < static_cast<int>(secs.size())) {
            OpenUserPage(0);
            LangSettle();
            userSec = userShown = sec;
            userInRows = true;
            const std::vector<URow> rows = UserRows(secs[sec].id, 0);
            const std::vector<int> rowAt = LangRowsOf(rows);
            if (k >= 0 && k < static_cast<int>(rowAt.size())) {
                userRow = rowAt[k];
                if (rows[userRow].act) rows[userRow].act();
                pressed = true;
            }
        }
    } else {
        ListPage* page = &settings;
        if (kind == L'o') {
            if (!games.empty()) OpenOptions(games[0].id);
            if (S2()) optOpenAt = now - 1; // already open
            page = &options;
        } else {
            BuildSettings();
            if (sec >= 0 && sec < static_cast<int>(settings.cats.size())) OpenSettings(sec);
        }
        LangSettle();
        if (sec >= 0 && sec < static_cast<int>(page->cats.size()) && (kind != L'o' || !games.empty())) {
            page->cat = sec;
            page->inRows = true;
            const std::vector<Row> rows = page->cats[sec].rows();
            const std::vector<int> rowAt = LangRowsOf(rows);
            if (k >= 0 && k < static_cast<int>(rowAt.size())) {
                page->row = rowAt[k];
                if (rows[page->row].act && !rows[page->row].disabled) rows[page->row].act();
                pressed = true;
            }
        }
    }
    LangSettle();
    if (at != std::wstring::npos) write(full.substr(at + 1), std::to_string(pressed ? LangRowChoices() : 0) + "\n");
    else if (pressed && num.size() > 2) LangRowChoose(num[2]);
    return true;
}
} // namespace
// ---- end of the language tool's walk ----

void DebugGoto(const std::wstring& name) {
    PcTestBegin();
    if (DebugLang(name) || DebugMouse(name) || DebugSc(name) || DebugUser(name) || DebugFriends(name) || DebugBar(name) || DebugGame(name) || DebugLists(name) || DebugOverlays(name) ||
        DebugSetup(name) || DebugCapture(name) || DebugPads(name) || DebugA11y(name) || DebugPc(name) || DebugPadRows(name) || DebugUpd(name) || DebugAvatarFrame(name) || DebugWeb(name) || DebugCard(name) || DebugIngame(name) || DebugAmiibo(name) || DebugContent(name) || DebugNetplay(name)) {
        fade = 0;
        return;
    }
    if (name == L"home-dark") cfg.dark = true;
    else if (name.size() > 2 && name.compare(name.size() - 2, 2, L"-2") == 0) {
        // "<screen>-2" renders that screen in the second console's style, dark.
        cfg.menuStyle = 2;
        cfg.dark = true;
        cfg.clock24 = true;
        DebugGoto(name.substr(0, name.size() - 2));
        return;
    } else if (name.size() > 3 && name.compare(name.size() - 3, 3, L"-2l") == 0) {
        cfg.menuStyle = 2;
        cfg.dark = false; // "<screen>-2l" is the light theme
        cfg.clock24 = true;
        DebugGoto(name.substr(0, name.size() - 3));
        return;
    } else if ((name.size() > 2 && name.compare(name.size() - 2, 2, L"-1") == 0) ||
               (name.size() > 3 && name.compare(name.size() - 3, 3, L"-1l") == 0)) {
        // "<screen>-1" / "-1l": the first console's style, dark / light.
        bool light = name.back() == L'l';
        cfg.menuStyle = 1;
        cfg.dark = !light;
        DebugGoto(name.substr(0, name.size() - (light ? 3 : 2)));
        return;
    }
    else if (name == L"sys") { homeRow = 2; si = S2() ? 8 : 5; } // the settings button in either style
    else if (name == L"settings") { OpenSettings(kNSWHomeCat); settings.inRows = true; } // NSW-Home's own section
    else if (name == L"settings-dark") { cfg.dark = true; OpenSettings(0); }
    else if (name == L"options" && !games.empty()) { OpenOptions(games[0].id); options.cat = 1; }
    else if (name == L"album") OpenAlbum();
    else if (name == L"pads") Push(Scr::Pads);
    else if (name == L"user") OpenUserPage(0);
    else if (name == L"who") Push(Scr::Who);
    else if (name == L"users") DebugLists(L"settings-in-users");
    else if (name.rfind(L"dumpsettings:", 0) == 0) {
        // Every row of every Settings section, one per line, to the named file:
        // section, kind, label, value, whether it does something on a PC, whether
        // A does anything, state. Used to tell the owner exactly what works.
        BuildSettings();
        std::string out = "section\tkind\tlabel\tvalue\treal\taction\tdisabled\ton\n";
        static const char* const kinds[] = {"plain", "toggle", "radio", "slider", "text", "panel", "table", "gap", "storage", "users"};
        auto flat = [](std::wstring t) {
            for (wchar_t& c : t)
                if (c == L'\n' || c == L'\t') c = L' ';
            return Narrow(t);
        };
        for (const Cat& c : settings.cats)
            for (const Row& r : c.rows()) {
                if (r.kind == RK::Gap) continue;
                out += flat(c.name) + "\t" + (r.heading ? "heading" : kinds[static_cast<int>(r.kind)]) + "\t" + flat(r.label) +
                       "\t" + flat(r.value) + "\t" + (r.real ? "1" : "0") + "\t" + (r.act || r.step ? "1" : "0") + "\t" +
                       (r.disabled ? "1" : "0") + "\t" + (r.on ? "1" : "0") + "\n";
            }
        std::wstring file = name.substr(13);
        FILE* f = nullptr;
        if (!_wfopen_s(&f, file.c_str(), L"wb") && f) {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }
    }
    else if (name.rfind(L"adduser:", 0) == 0) AddUser(name.substr(8));
    else if (name == L"all") Push(Scr::All);
    else if (name == L"dialog") Ask(L"¿Quieres cerrar este programa?\nSe perderán los datos no guardados.", {L"Cancelar", L"Cerrar"});
    else if (name == L"launch" && !games.empty()) { launchId = games[0].id; Push(Scr::Launch); }
    fade = 0;
}

} // namespace ui
