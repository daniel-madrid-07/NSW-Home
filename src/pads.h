// Controllers as NSW-Home itself sees them: what each well-known pad's HID
// reports mean, which pads are connected, who is player 1 to 4, and the
// identifier the emulators (through SDL) give each pad.
//
// Stands alone (no app.h, no windows.h), so the test tool links it without the
// rest of the app and the other platforms compile it unchanged. The part that
// talks to devices is pads_win.cpp; it only feeds this module.
//
// Nothing is ever sent to a Sony pad, so a DualSense or DualShock 4 on
// Bluetooth stays in the short report it starts with unless another program
// has switched it; that report has every button the menu needs but no battery.
//
// Thread-safe: the device thread feeds, the UI thread reads.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pads {

enum class Kind { Xbox, DualSense, DualShock4, SwitchPro, JoyConL, JoyConR, JoyConPair, Keyboard };
enum class Link { Usb, Bluetooth, XInput, None };

// Buttons by where they sit, as on the console's own pad: East is the console's
// A, South its B, North its X, West its Y.
enum : uint32_t {
    kUp = 1 << 0, kDown = 1 << 1, kLeft = 1 << 2, kRight = 1 << 3,
    kSouth = 1 << 4, kEast = 1 << 5, kWest = 1 << 6, kNorth = 1 << 7,
    kL = 1 << 8, kR = 1 << 9, kZL = 1 << 10, kZR = 1 << 11,
    kPlus = 1 << 12, kMinus = 1 << 13, kHome = 1 << 14, kCapture = 1 << 15,
    kSL = 1 << 16, kSR = 1 << 17,
    kLS = 1 << 18, kRS = 1 << 19, // the sticks pressed in
};

struct State {
    uint32_t buttons = 0;
    float lx = 0, ly = 0; // left stick, -1..1, up is positive
    float rx = 0, ry = 0; // right stick; stays 0 where the report has none (`right` false)
    float lt = 0, rt = 0; // how far ZL and ZR are pressed, 0..1; only where the pad measures it (`analog`)
    bool right = false, analog = false;
    int battery = -1;     // percent, -1 when the report does not carry it
    bool charging = false;
};

// Which pad a USB/Bluetooth vendor and product number is; false for anything
// that is not one of the pads read here (every other HID joystick is ignored).
bool Known(uint16_t vendor, uint16_t product, Kind& kind);

// One input report exactly as Windows hands it over (first byte = report
// number). False when it is not a state report of that pad; `out` is then
// left alone. A single Joy-Con is reported as held upright (its own arrows or
// letters); Sideways() turns that into the sideways hold.
bool Parse(Kind kind, Link link, const uint8_t* report, size_t size, State& out);
State Sideways(Kind joyCon, const State& upright);

// ---- what SDL calls a pad ---------------------------------------------------

// SDL's 16-byte joystick GUID for a pad its HIDAPI drivers handle, as SDL 2.26
// to 2.32 on Windows build it (SDL_CreateJoystickGUID, SDL_hidapijoystick.c):
// bus, CRC-16 of the name, vendor, 0, product, 0, version (each 16 bits, low
// byte first), 'h', 0. The bus is always 3 (USB) there, Bluetooth included.
// `version` is what HidD_GetAttributes reports. `name` is the name SDL ends up
// with ("DualSense Wireless Controller"); nullptr leaves the CRC zero.
void SdlGuid(uint16_t vendor, uint16_t product, uint16_t version, const char* name, uint8_t out[16]);
uint16_t SdlCrc16(const char* text);
// The name SDL's driver gives the pad, nullptr when it depends on strings read
// from the device (the Nintendo pads).
const char* SdlName(Kind kind, uint16_t product);
// Ryujinx's id for the pad ("0-56570003-054c-0000-e60c-000000016800"): an index
// among pads with the same GUID, then the GUID printed the way .NET prints one.
std::string RyujinxId(const uint8_t guid[16], int index);
// The yuzu family's: 32 hex digits with the CRC cleared.
std::string YuzuGuid(const uint8_t guid[16]);

// ---- the pads that are connected ------------------------------------------------

struct DeviceInfo {
    Kind kind = Kind::Xbox;
    Link link = Link::Usb;
    uint16_t vendor = 0, product = 0, version = 0;
    // Stable identity: the pad's own address where it can be read without
    // disturbing it (always on Bluetooth), else the port it is plugged into.
    std::string id;
};

// Device layer -> here. `handle` is whatever names the device to the caller.
void Arrive(uint64_t handle, const DeviceInfo& info);
void Remove(uint64_t handle);
void Feed(uint64_t handle, const uint8_t* report, size_t size);
// XInput slots are polled by the app (it had them before any of this).
void SetXInput(int slot, bool present, uint16_t buttons, float lx, float ly);
// The rest of a slot's state, for the pages that show a pad live.
void SetXInputMore(int slot, float rx, float ry, float lt, float rt);

// A pad as the player thinks of it: two Joy-Con used together are one.
struct Pad {
    Kind kind = Kind::Xbox;
    Link link = Link::None;
    std::string id;
    int player = 0;                 // 1..4, 0 = has no number
    int battery = -1, battery2 = -1; // battery2: the right Joy-Con of a pair
    bool charging = false;
    // What the emulators need to find it; see padcfg.h.
    uint16_t vendor = 0, product = 0, version = 0;
    int sameIndex = 0; // how many pads with the same vendor/product/version come before it
};
// Connected pads in player order, then the keyboard (always present).
std::vector<Pad> List();

// The remembered order: identities, first = player 1 ("keyboard" may be one).
// A connected pad that is not in it comes after those that are, in the order
// they were connected. The keyboard is player 1 while no pad is connected and
// has no number otherwise, unless the order names it.
void SetOrder(const std::vector<std::string>& ids);
std::vector<std::string> Order();
std::string JoinOrder(const std::vector<std::string>& ids);
std::vector<std::string> SplitOrder(const std::string& text);

// The order screen: while listening, a pad on which L and R go down together
// (SL and SR on a single Joy-Con; L on a left and R on a right Joy-Con makes
// them a pair) is queued. TakeChord hands out the next one's identity.
void Listen(bool on);
bool TakeChord(std::string& id);
// Some pad is holding L or R (so an L or R press did not come from the keyboard).
bool HoldingShoulder();

// What the menu gets each frame: all pads merged into one XInput-style button
// word (so the app's existing handling stays as it is), with the face buttons
// already turned so that the console's A is the app's A on a Nintendo pad
// whatever `swapAB` says. A pad that also shows up as an XInput slot (wrapped
// by another program) acts once: the slot's press is dropped when it follows
// a press on a pad read here within a tenth of a second.
struct Menu {
    uint16_t buttons = 0; // XINPUT_GAMEPAD_* bits; 0x0400 = HOME
    float lx = 0, ly = 0;
    bool capture = false; // a capture button went down since the last call
};
Menu Poll(bool swapAB);

// ---- the menu's buttons, and which of them does what ----------------------------
//
// A pad's buttons under the names the menu's pages use, after the turn Poll
// describes (so "A" is the button that confirms on that pad before any
// remapping, wherever it sits). The first kActions of them are also the
// menu's actions, each on the button of the same name unless remapped.
enum : uint32_t {
    mUp = 1 << 0, mDown = 1 << 1, mLeft = 1 << 2, mRight = 1 << 3,
    mA = 1 << 4, mB = 1 << 5, mX = 1 << 6, mY = 1 << 7,
    mPlus = 1 << 8, mMinus = 1 << 9, mL = 1 << 10, mR = 1 << 11,
    mHome = 1 << 12, mCapture = 1 << 13,
    mZL = 1 << 14, mZR = 1 << 15, mLS = 1 << 16, mRS = 1 << 17,
};
constexpr int kActions = 14, kMenuButtons = 18;
constexpr int kConfirm = 4, kBack = 5; // the actions on mA and mB
uint32_t MenuButtons(const State& held, bool nintendo, bool swapAB);

// The remapping: for each action, the number (bit position) of the menu
// button it is on. Poll applies it to every pad, XInput ones included; the
// keyboard is not touched by it, so the menu can always be driven from there.
// Confirm and back may only sit on a button every pad has (A B X Y L R +),
// and no two actions share a button.
bool RemapValid(const int map[kActions]);
// Puts `action` on `button`. The action that was there takes the button this
// one leaves. False, with `map` untouched, when that would break a rule above.
bool RemapAssign(int map[kActions], int action, int button);
void SetRemap(const int map[kActions]); // an invalid one is ignored
void GetRemap(int map[kActions]);
std::string RemapText(const int map[kActions]); // "0,1,2,..." for the configuration
bool RemapParse(const std::string& text, int map[kActions]);

// ---- pages that read the pads themselves ----------------------------------------

// Every pad as List() gives them (the keyboard left out), with what it holds
// right now: `st` as the player holds the pad, `menu` the same buttons under
// the menu's names. A press shorter than a frame shows in the call after it.
struct Live {
    Pad pad;
    State st;
    uint32_t menu = 0;
};
std::vector<Live> Snapshot(bool swapAB);
// A page that puts every button under test calls this each frame it is on
// screen: for the next quarter of a second Poll hands the menu nothing, and
// what is still held when that ends does not count as a press.
void Hush();
// HOME was held for a second (once per hold); never while hushed.
bool TakeHomeHold();

// A Switch Pro Controller on a cable only talks after being told to (the one
// thing ever written to a pad; pads_win.cpp). Off, the console's default:
// it is not told, and such a pad is read over Bluetooth only.
void SetProWired(bool on);
bool ProWired();
// Whether this build reads pads itself at all (Windows only so far).
bool Reads();

// The device layer (pads_win.cpp; empty on the other platforms, whose own
// front ends keep reading pads their way). Start is safe to call twice.
void Start();
void Stop();
// For the test tool: every HID joystick or game pad Windows lists right now
// and what this module makes of it, one line each.
std::vector<std::string> Probe();

// Test states: replaces the connected pads by made-up ones ("ds5 pro jc xbox",
// each optionally followed by ":<battery>"); DebugChord presses L+R on the
// n-th of them.
void DebugFake(const std::string& spec);
bool DebugFaking();
void DebugChord(int index);
// Puts the n-th made-up pad (XInput ones after the others) in this state.
void DebugHold(int index, const State& held);

} // namespace pads
