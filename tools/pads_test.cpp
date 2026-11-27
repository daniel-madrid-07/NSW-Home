// Exercises src\pads.cpp (report layouts, SDL identifiers, player order, the
// menu's remapping, the readers the test pages use) and src\padcfg.cpp (moving
// the emulators' controller entries; their vibration, keyboard, mouse and
// dead-zone values) without any pad: reports are built here from the
// documented layouts, and the emulator files are copies.
//
//   pads_test.exe [data folder] [work folder]     the checks
//   pads_test.exe probe                           what is connected right now
//
// The data folder (default: data) is only read: the settings files of the six
// emulators under it are copied into the work folder (default
// build\pads_test) and changed there.
// `probe` sends nothing to any device.
#include "../src/padcfg.h"
#include "../src/pads.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pads;

namespace {

int passed = 0, failed = 0;

void Check(bool ok, const std::string& what) {
    if (ok) passed++;
    else {
        failed++;
        printf("  FAIL  %s\n", what.c_str());
    }
}

bool Slurp(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    out.clear();
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    fclose(f);
    return true;
}

bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<std::string> Lines(const std::string& t) {
    std::vector<std::string> out;
    for (size_t at = 0; at < t.size();) {
        size_t end = t.find('\n', at);
        if (end == std::string::npos) end = t.size() - 1;
        out.push_back(t.substr(at, end - at + 1));
        at = end + 1;
    }
    return out;
}

size_t BytesDiffering(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return static_cast<size_t>(-1);
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i++) n += a[i] != b[i];
    return n;
}

struct Report {
    uint8_t b[78] = {};
    size_t n;
    Report(size_t size, uint8_t id) : n(size) { b[0] = id; }
};

bool Near(float a, float b) {
    return fabsf(a - b) < 0.05f;
}

// ---- report layouts -----------------------------------------------------------

void TestReports() {
    printf("report layouts\n");
    State s;
    {
        Report r(64, 0x01); // DualSense on USB
        r.b[1] = 0x00, r.b[2] = 0x80, r.b[8] = 0x08 | 0x20, r.b[9] = 0x03, r.b[10] = 0x01 | 0x04, r.b[53] = 0x18;
        Check(Parse(Kind::DualSense, Link::Usb, r.b, r.n, s), "DualSense USB report is read");
        Check(s.buttons == (kSouth | kL | kR | kHome | kCapture), "DualSense USB: cross, L1, R1, PS, microphone");
        Check(Near(s.lx, -1) && Near(s.ly, 0), "DualSense USB: stick fully left");
        Check(s.battery == 85 && s.charging, "DualSense USB: battery 85 %, charging");
    }
    {
        Report r(78, 0x31); // DualSense on Bluetooth, full report
        r.b[2] = 0x80, r.b[3] = 0x00, r.b[9] = 0x00 | 0x40, r.b[10] = 0x20, r.b[54] = 0x05;
        Check(Parse(Kind::DualSense, Link::Bluetooth, r.b, r.n, s), "DualSense Bluetooth full report is read");
        Check(s.buttons == (kUp | kEast | kPlus), "DualSense Bluetooth: up, circle, Options");
        Check(Near(s.ly, 1), "DualSense Bluetooth: stick fully up");
        Check(s.battery == 55 && !s.charging, "DualSense Bluetooth: battery 55 %");
        Check(!Parse(Kind::DualSense, Link::Usb, r.b, r.n, s), "a Bluetooth report is not taken for a USB one");
    }
    {
        Report r(78, 0x01); // DualSense on Bluetooth, the short report (padded by Windows to the longest)
        r.b[1] = 0x80, r.b[2] = 0x80, r.b[5] = 0x04 | 0x80, r.b[6] = 0x10, r.b[7] = 0x01 | 0xFC;
        Check(Parse(Kind::DualSense, Link::Bluetooth, r.b, r.n, s), "DualSense Bluetooth short report is read");
        Check(s.buttons == (kDown | kNorth | kMinus | kHome), "DualSense short: down, triangle, Create, PS (counter bits ignored)");
        Check(s.battery == -1, "DualSense short: no battery");
    }
    {
        Report r(64, 0x01); // DualShock 4 on USB
        r.b[1] = 0xFF, r.b[2] = 0x80, r.b[5] = 0x08 | 0x10, r.b[6] = 0x20, r.b[7] = 0x01, r.b[30] = 0x1B;
        Check(Parse(Kind::DualShock4, Link::Usb, r.b, r.n, s), "DualShock 4 USB report is read");
        Check(s.buttons == (kWest | kPlus | kHome), "DualShock 4 USB: square, Options, PS");
        Check(Near(s.lx, 1), "DualShock 4 USB: stick fully right");
        Check(s.battery == 100 && s.charging, "DualShock 4 USB: full, on the cable");
    }
    {
        Report r(78, 0x11); // DualShock 4 on Bluetooth, full report
        r.b[1] = 0xC0, r.b[3] = 0x80, r.b[4] = 0x80, r.b[7] = 0x06 | 0x20, r.b[8] = 0x01 | 0x02, r.b[32] = 0x05;
        Check(Parse(Kind::DualShock4, Link::Bluetooth, r.b, r.n, s), "DualShock 4 Bluetooth full report is read");
        Check(s.buttons == (kLeft | kSouth | kL | kR), "DualShock 4 Bluetooth: left, cross, L1, R1");
        Check(s.battery == 55 && !s.charging, "DualShock 4 Bluetooth: battery 55 %");
        Report q(78, 0x01); // and its short report
        q.b[1] = 0x80, q.b[2] = 0xFF, q.b[5] = 0x08 | 0x40, q.b[30] = 0x1B;
        Check(Parse(Kind::DualShock4, Link::Bluetooth, q.b, q.n, s), "DualShock 4 Bluetooth short report is read");
        Check(s.buttons == kEast && Near(s.ly, -1) && s.battery == -1, "DualShock 4 short: circle, stick down, no battery");
        Report bad(78, 0x05);
        Check(!Parse(Kind::DualShock4, Link::Bluetooth, bad.b, bad.n, s), "another report number is refused");
    }
    {
        Report r(12, 0x3F); // Switch Pro Controller, short report
        r.b[1] = 0x02 | 0x10 | 0x20, r.b[2] = 0x10 | 0x20, r.b[3] = 2;
        r.b[4] = 0x00, r.b[5] = 0x80, r.b[6] = 0x00, r.b[7] = 0x00;
        Check(Parse(Kind::SwitchPro, Link::Bluetooth, r.b, r.n, s), "Pro Controller short report is read");
        Check(s.buttons == (kEast | kL | kR | kHome | kCapture | kRight), "Pro short: A, L, R, HOME, capture, right");
        Check(Near(s.lx, 0) && Near(s.ly, 1), "Pro short: stick fully up");
    }
    {
        Report r(49, 0x30); // Switch Pro Controller, full report
        r.b[2] = 0x90, r.b[3] = 0x08 | 0x40, r.b[4] = 0x02 | 0x20, r.b[5] = 0x02 | 0x40;
        const int x = 2048, y = 3500;
        r.b[6] = x & 0xFF, r.b[7] = static_cast<uint8_t>((x >> 8) | ((y & 0x0F) << 4)), r.b[8] = static_cast<uint8_t>(y >> 4);
        Check(Parse(Kind::SwitchPro, Link::Usb, r.b, r.n, s), "Pro Controller full report is read");
        Check(s.buttons == (kEast | kR | kPlus | kCapture | kUp | kL), "Pro full: A, R, +, capture, up, L");
        Check(Near(s.lx, 0) && Near(s.ly, 1), "Pro full: stick fully up");
        Check(s.battery == 100 && s.charging, "Pro full: battery full, charging");
    }
    {
        Report r(12, 0x3F); // left Joy-Con, short report (laid out for the sideways hold)
        r.b[1] = 0x01, r.b[2] = 0x01, r.b[3] = 0;
        Check(Parse(Kind::JoyConL, Link::Bluetooth, r.b, r.n, s), "left Joy-Con short report is read");
        Check(s.buttons == (kLeft | kMinus) && Near(s.lx, 1) && Near(s.ly, 0), "left Joy-Con short, upright: left arrow, -, stick right");
        State side = Sideways(Kind::JoyConL, s);
        Check(side.buttons == (kSouth | kPlus) && Near(side.ly, 1), "left Joy-Con sideways: bottom button, +, stick up");
        r.b[1] = 0x10 | 0x20, r.b[2] = 0, r.b[3] = 8;
        Parse(Kind::JoyConL, Link::Bluetooth, r.b, r.n, s);
        Check(Sideways(Kind::JoyConL, s).buttons == (kL | kR), "left Joy-Con sideways: SL and SR are L and R");
    }
    {
        Report r(12, 0x3F); // right Joy-Con, short report
        r.b[1] = 0x01, r.b[2] = 0x10 | 0x40, r.b[3] = 2;
        Check(Parse(Kind::JoyConR, Link::Bluetooth, r.b, r.n, s), "right Joy-Con short report is read");
        Check(s.buttons == (kEast | kHome | kR) && Near(s.ly, 1), "right Joy-Con short, upright: A, HOME, R, stick up");
        State side = Sideways(Kind::JoyConR, s);
        Check(side.buttons == (kSouth | kHome) && Near(side.lx, 1), "right Joy-Con sideways: bottom button, HOME, stick right");
    }
    {
        Report r(49, 0x30); // Joy-Con, full report
        r.b[2] = 0x40, r.b[4] = 0x01, r.b[5] = 0x08 | 0x10 | 0x20 | 0x40;
        r.b[6] = 0x00, r.b[7] = 0x08, r.b[8] = 0x80; // x 2048, y 2048
        Check(Parse(Kind::JoyConL, Link::Bluetooth, r.b, r.n, s), "left Joy-Con full report is read");
        Check(s.buttons == (kLeft | kSL | kSR | kL | kMinus), "left Joy-Con full: left arrow, SL, SR, L, -");
        Check(s.battery == 50, "left Joy-Con full: battery half");
        Check(Sideways(Kind::JoyConL, s).buttons == (kSouth | kL | kR | kPlus), "left Joy-Con full, sideways");
        Report q(49, 0x30);
        q.b[3] = 0x08 | 0x80, q.b[4] = 0x02 | 0x10;
        const int x = 3500, y = 2048;
        q.b[9] = x & 0xFF, q.b[10] = static_cast<uint8_t>((x >> 8) | ((y & 0x0F) << 4)), q.b[11] = static_cast<uint8_t>(y >> 4);
        Check(Parse(Kind::JoyConR, Link::Bluetooth, q.b, q.n, s), "right Joy-Con full report is read");
        Check(s.buttons == (kEast | kZR | kPlus | kHome) && Near(s.lx, 1), "right Joy-Con full: A, ZR, +, HOME, its stick right");
    }
    // Right stick, triggers and stick clicks (what the test and calibration pages show).
    {
        Report r(64, 0x01); // DualSense on USB: x, y, x, y, L2, R2, counter, buttons
        r.b[1] = r.b[2] = 0x80, r.b[3] = 0xFF, r.b[4] = 0x00, r.b[5] = 0xFF, r.b[6] = 0x40, r.b[8] = 0x08, r.b[9] = 0x04 | 0x40 | 0x80;
        Parse(Kind::DualSense, Link::Usb, r.b, r.n, s);
        Check(s.right && Near(s.rx, 1) && Near(s.ry, 1), "DualSense USB: right stick fully right and up");
        Check(s.analog && Near(s.lt, 1) && Near(s.rt, 0.25f), "DualSense USB: L2 fully in, R2 a quarter");
        Check(s.buttons == (kZL | kLS | kRS), "DualSense USB: L2 as a button, both sticks pressed in");
        Report q(78, 0x01); // its short Bluetooth report: x, y, x, y, three button bytes, L2, R2
        q.b[1] = q.b[2] = 0x80, q.b[3] = 0x00, q.b[4] = 0x80, q.b[5] = 0x08, q.b[6] = 0x80, q.b[8] = 0x80, q.b[9] = 0xFF;
        Parse(Kind::DualSense, Link::Bluetooth, q.b, q.n, s);
        Check(Near(s.rx, -1) && Near(s.ry, 0) && Near(s.lt, 0.5f) && Near(s.rt, 1) && s.buttons == kRS,
              "DualSense short: right stick left, L2 half, R2 full, R3");
    }
    {
        Report r(64, 0x01); // DualShock 4 on USB: x, y, x, y, three button bytes, L2, R2
        r.b[1] = r.b[2] = 0x80, r.b[3] = 0x80, r.b[4] = 0xFF, r.b[5] = 0x08, r.b[6] = 0x40, r.b[8] = 0x33, r.b[9] = 0xCC;
        Parse(Kind::DualShock4, Link::Usb, r.b, r.n, s);
        Check(s.right && Near(s.rx, 0) && Near(s.ry, -1) && Near(s.lt, 0.2f) && Near(s.rt, 0.8f) && s.buttons == kLS,
              "DualShock 4 USB: right stick down, L2 a fifth, R2 four fifths, L3");
        Report q(78, 0x11); // Bluetooth, full report (two bytes more in front)
        q.b[1] = 0xC0, q.b[3] = q.b[4] = 0x80, q.b[5] = 0xFF, q.b[6] = 0x80, q.b[7] = 0x08, q.b[10] = 0xFF;
        Parse(Kind::DualShock4, Link::Bluetooth, q.b, q.n, s);
        Check(Near(s.rx, 1) && Near(s.lt, 1) && Near(s.rt, 0), "DualShock 4 Bluetooth: right stick right, L2 full");
    }
    {
        Report r(49, 0x30); // Switch Pro Controller, full report: left stick in bytes 6-8, right in 9-11
        const int lx = 2048, ly = 2048, rx = 600, ry = 2048;
        r.b[4] = 0x04 | 0x08;
        r.b[6] = lx & 0xFF, r.b[7] = static_cast<uint8_t>((lx >> 8) | ((ly & 0x0F) << 4)), r.b[8] = static_cast<uint8_t>(ly >> 4);
        r.b[9] = rx & 0xFF, r.b[10] = static_cast<uint8_t>((rx >> 8) | ((ry & 0x0F) << 4)), r.b[11] = static_cast<uint8_t>(ry >> 4);
        Parse(Kind::SwitchPro, Link::Usb, r.b, r.n, s);
        Check(s.right && Near(s.lx, 0) && Near(s.rx, -1) && Near(s.ry, 0), "Pro full: right stick fully left, left one at rest");
        Check(s.buttons == (kLS | kRS) && !s.analog, "Pro full: both sticks pressed in; its ZL and ZR are plain buttons");
        Report q(12, 0x3F); // short report: four 16-bit numbers after the hat
        q.b[2] = 0x04 | 0x08, q.b[3] = 8, q.b[5] = q.b[7] = 0x80, q.b[8] = 0xFF, q.b[9] = 0xFF, q.b[11] = 0x80;
        Parse(Kind::SwitchPro, Link::Bluetooth, q.b, q.n, s);
        Check(s.right && Near(s.rx, 1) && Near(s.ry, 0) && s.buttons == (kLS | kRS), "Pro short: right stick right, both sticks pressed in");
        Report j(49, 0x30); // right Joy-Con: its stick pressed in
        j.b[4] = 0x04, j.b[9] = 0x00, j.b[10] = 0x08, j.b[11] = 0x80;
        Parse(Kind::JoyConR, Link::Bluetooth, j.b, j.n, s);
        Check(s.buttons == kRS && !s.right, "right Joy-Con full: stick pressed in (one stick only)");
        Check(Sideways(Kind::JoyConR, s).buttons == kLS, "right Joy-Con sideways: that stick is the pad's only one");
    }
    Kind k;
    Check(Known(0x054C, 0x0CE6, k) && k == Kind::DualSense && Known(0x057E, 0x2009, k) && k == Kind::SwitchPro,
          "vendor and product numbers are recognised");
    Check(!Known(0x3434, 0x0100, k) && !Known(0x045E, 0x028E, k), "other HID joysticks are not");
}

// ---- SDL identifiers ----------------------------------------------------------

void TestIds(const std::wstring& data) {
    printf("SDL identifiers\n");
    Check(SdlCrc16("DualSense Wireless Controller") == 0x5657, "CRC of the DualSense's SDL name is 5657");
    uint8_t usb[16], other[16];
    SdlGuid(0x054C, 0x0CE6, 0x0100, SdlName(Kind::DualSense, 0x0CE6), usb);
    SdlGuid(0x054C, 0x0CE6, 0x0000, SdlName(Kind::DualSense, 0x0CE6), other);
    std::string ryu, ini;
    if (Slurp(data + L"\\ryujinx\\Config.json", ryu)) {
        // The owner's own entry: the id of the first SDL pad in input_config.
        size_t at = ryu.find("\"backend\": \"GamepadSDL2\"");
        at = at == std::string::npos ? at : ryu.find("\"id\": \"", at);
        std::string id = at == std::string::npos ? "" : ryu.substr(at + 7, ryu.find('"', at + 7) - at - 7);
        printf("  Ryujinx file has   %s\n  derived (version 0100) %s\n", id.c_str(), RyujinxId(usb, 0).c_str());
        Check(!id.empty() && id == RyujinxId(usb, 0), "derivation reproduces the id in the owner's Ryujinx settings");
    } else {
        Check(false, "data\\ryujinx\\Config.json could not be read");
    }
    if (Slurp(data + L"\\citron\\config\\qt-config.ini", ini)) {
        Check(ini.find("player_1_button_a=\"engine:sdl,port:0,guid:" + YuzuGuid(usb) + ",") != std::string::npos,
              "derivation (version 0100) reproduces Citron's player 2 identifier");
        Check(ini.find("player_0_button_a=\"engine:sdl,port:0,guid:" + YuzuGuid(other) + ",") != std::string::npos,
              "derivation (version 0000) reproduces Citron's player 1 identifier");
    } else {
        Check(false, "data\\citron\\config\\qt-config.ini could not be read");
    }
}

// ---- connected pads, order, merging ---------------------------------------------

void Press(uint64_t handle, Kind kind, uint32_t buttons) {
    // Reports that Parse turns into exactly these buttons.
    if (kind == Kind::DualSense) {
        Report r(64, 0x01);
        r.b[1] = r.b[2] = 0x80;
        r.b[8] = 0x08 | ((buttons & kSouth) ? 0x20 : 0) | ((buttons & kEast) ? 0x40 : 0) | ((buttons & kNorth) ? 0x80 : 0) |
                 ((buttons & kWest) ? 0x10 : 0);
        r.b[9] = ((buttons & kL) ? 0x01 : 0) | ((buttons & kR) ? 0x02 : 0) | ((buttons & kZL) ? 0x04 : 0);
        r.b[10] = ((buttons & kHome) ? 0x01 : 0) | ((buttons & kCapture) ? 0x04 : 0);
        Feed(handle, r.b, r.n);
    } else {
        Report r(12, 0x3F);
        r.b[5] = r.b[7] = 0x80;
        r.b[3] = 8;
        if (kind == Kind::SwitchPro) {
            r.b[1] = ((buttons & kEast) ? 0x02 : 0) | ((buttons & kSouth) ? 0x01 : 0) | ((buttons & kL) ? 0x10 : 0) | ((buttons & kR) ? 0x20 : 0);
        } else {
            r.b[1] = ((buttons & kSL) ? 0x10 : 0) | ((buttons & kSR) ? 0x20 : 0);
            r.b[2] = (buttons & (kL | kR)) ? 0x40 : 0;
        }
        Feed(handle, r.b, r.n);
    }
}

void TestOrder() {
    printf("connected pads and their order\n");
    DeviceInfo ds, pro, jl, jr;
    ds.kind = Kind::DualSense, ds.link = Link::Usb, ds.vendor = 0x054C, ds.product = 0x0CE6, ds.version = 0x0100, ds.id = "mac:aa";
    pro.kind = Kind::SwitchPro, pro.link = Link::Bluetooth, pro.vendor = 0x057E, pro.product = 0x2009, pro.id = "mac:bb";
    jl.kind = Kind::JoyConL, jl.link = Link::Bluetooth, jl.vendor = 0x057E, jl.product = 0x2006, jl.id = "mac:cc";
    jr.kind = Kind::JoyConR, jr.link = Link::Bluetooth, jr.vendor = 0x057E, jr.product = 0x2007, jr.id = "mac:dd";

    std::vector<Pad> l = List();
    Check(l.size() == 1 && l[0].kind == Kind::Keyboard && l[0].player == 1, "no pad: the keyboard is player 1");
    Arrive(1, ds);
    Arrive(2, pro);
    l = List();
    Check(l.size() == 3 && l[0].id == "mac:aa" && l[0].player == 1 && l[1].id == "mac:bb" && l[1].player == 2 && l[2].player == 0,
          "pads are numbered as they connect; the keyboard then has no number");
    SetOrder({"mac:bb", "keyboard"});
    l = List();
    Check(l[0].id == "mac:bb" && l[0].player == 1 && l[1].id == "mac:aa" && l[1].player == 3 && l[2].player == 2,
          "a remembered order wins: Pro 1, keyboard 2, the DualSense after them");
    Remove(2);
    l = List();
    Check(l.size() == 2 && l[0].id == "mac:aa" && l[0].player == 2 && l[1].player == 1, "with the Pro gone the keyboard is 1, DualSense 2");
    Arrive(2, pro);
    Check(List()[0].id == "mac:bb", "the same pad coming back gets its place back");
    Check(SplitOrder(JoinOrder({"a", "pair:b+c"})) == std::vector<std::string>({"a", "pair:b+c"}), "the order survives being stored as text");

    // L+R on the order screen.
    std::string id;
    Listen(true);
    Press(1, Kind::DualSense, kL);
    Check(!TakeChord(id), "L alone is not a choice");
    Press(1, Kind::DualSense, kL | kR);
    Check(TakeChord(id) && id == "mac:aa", "L+R on the DualSense picks it");
    Press(1, Kind::DualSense, kL | kR);
    Check(!TakeChord(id), "holding L+R picks it once");
    Check(HoldingShoulder(), "a pad holding L or R is noticed");
    Press(1, Kind::DualSense, 0);
    SetXInput(0, true, 0, 0, 0);
    SetXInput(0, true, 0x0300, 0, 0);
    Check(TakeChord(id) && id == "xinput:0", "L+R on an XInput pad picks it");
    SetXInput(0, false, 0, 0, 0);

    // Joy-Con: together by default, apart or together as the players press.
    Arrive(3, jl);
    Arrive(4, jr);
    SetOrder({});
    l = List();
    Check(l.size() == 4 && l[2].kind == Kind::JoyConPair && l[2].id == "pair:mac:cc+mac:dd", "a left and a right Joy-Con are one pad");
    Press(3, Kind::JoyConL, kSL | kSR);
    Check(TakeChord(id) && id == "mac:cc", "SL+SR on the left Joy-Con picks it alone");
    l = List();
    Check(l.size() == 5 && l[2].kind == Kind::JoyConL && l[3].kind == Kind::JoyConR, "then the two are separate pads");
    Press(3, Kind::JoyConL, kL);
    Press(4, Kind::JoyConR, kR);
    Check(TakeChord(id) && id == "pair:mac:cc+mac:dd", "L on the left and R on the right picks them as a pair");
    Check(List().size() == 4, "and they are one pad again");
    Press(3, Kind::JoyConL, 0);
    Press(4, Kind::JoyConR, 0);
    Listen(false);
    Remove(3);
    Remove(4);

    // Merging for the menu.
    Poll(false);
    Press(2, Kind::SwitchPro, kEast);
    Menu m = Poll(false);
    Check(m.buttons == 0x1000, "Nintendo pad: its A (right-hand button) is the app's A");
    Press(2, Kind::SwitchPro, 0);
    Poll(true);
    Press(2, Kind::SwitchPro, kEast);
    m = Poll(true);
    Check(m.buttons == 0x2000, "with A/B swapped, a Nintendo pad's A is still the app's A (sent as XInput B, which the app turns back)");
    Press(2, Kind::SwitchPro, 0);
    Press(1, Kind::DualSense, kSouth);
    Check(Poll(false).buttons == 0x1000, "DualSense: cross is XInput A, left to the swap setting like an Xbox pad");
    // The same press arriving again through an XInput wrapper.
    SetXInput(0, true, 0x1000, 0, 0);
    m = Poll(false);
    Check(m.buttons == 0x1000, "wrapped pad: still one A");
    Press(1, Kind::DualSense, 0);
    m = Poll(false);
    Check(m.buttons == 0, "wrapped pad: the wrapper's copy of the press never counts, even after the pad let go");
    SetXInput(0, true, 0, 0, 0);
    Poll(false);
    SetXInput(0, true, 0x0001, 0, 0);
    Check(Poll(false).buttons == 0x0001, "directions from the wrapper pass (held from both is one direction)");
    SetXInput(0, false, 0, 0, 0);
    Sleep(150);
    SetXInput(1, true, 0x2000, 0, 0);
    Check(Poll(false).buttons == 0x2000, "an XInput pad pressed on its own acts");
    SetXInput(1, false, 0, 0, 0);
    // A press shorter than a frame, and HOME / capture.
    Poll(false);
    Press(1, Kind::DualSense, kHome | kCapture);
    Press(1, Kind::DualSense, 0);
    m = Poll(false);
    Check((m.buttons & 0x0400) && m.capture, "a press and release between two frames is still seen (HOME, capture)");
    m = Poll(false);
    Check(m.buttons == 0 && !m.capture, "and only once");
    Remove(1);
    Remove(2);
}

// ---- remapping, and pages that read the pads themselves ---------------------------

void TestRemap() {
    printf("remapping the menu's buttons\n");
    DeviceInfo ds, pro;
    ds.kind = Kind::DualSense, ds.link = Link::Usb, ds.vendor = 0x054C, ds.product = 0x0CE6, ds.version = 0x0100, ds.id = "mac:aa";
    pro.kind = Kind::SwitchPro, pro.link = Link::Bluetooth, pro.vendor = 0x057E, pro.product = 0x2009, pro.id = "mac:bb";
    Arrive(1, ds);
    Arrive(2, pro);
    const uint16_t xA = 0x1000, xB = 0x2000, xX = 0x4000, xL = 0x0100;
    const int bA = 4, bX = 6, bY = 7, bL = 10, bHome = 12, bCapture = 13, bZL = 14;
    const int aX = 6, aY = 7, aL = 10, aCapture = 13;
    int map[kActions], stock[kActions];
    GetRemap(stock);
    Check(RemapValid(stock) && RemapText(stock) == "0,1,2,3,4,5,6,7,8,9,10,11,12,13", "the stock mapping: each action on its own button");

    // The rules.
    std::copy(stock, stock + kActions, map);
    Check(RemapAssign(map, kConfirm, bX) && map[kConfirm] == bX && map[aX] == bA, "confirm moved to X: X's action takes A (a swap)");
    Check(RemapValid(map), "... and the result is a valid mapping");
    Check(!RemapAssign(map, kConfirm, bZL) && map[kConfirm] == bX, "confirm on ZL (not on every pad) is refused, nothing changes");
    Check(!RemapAssign(map, kBack, bHome) && !RemapAssign(map, kBack, bCapture), "back on HOME or on capture is refused");
    Check(RemapAssign(map, aL, bZL) && map[aL] == bZL, "another action may go to ZL");
    Check(!RemapAssign(map, aL, map[kBack]) && map[aL] == bZL, "... but may not push back onto ZL by taking its button");
    Check(RemapAssign(map, kBack, bL), "back can take the button L left free");
    int twice[kActions];
    std::copy(stock, stock + kActions, twice);
    twice[aY] = bX;
    Check(!RemapValid(twice), "two actions on one button are not a valid mapping");
    int parsed[kActions];
    Check(RemapParse(RemapText(map), parsed) && std::equal(map, map + kActions, parsed), "a mapping survives being stored as text");
    Check(!RemapParse("4,4,4", parsed) && !RemapParse("0,1,2,3,14,5,6,7,8,9,10,11,12,13", parsed) && !RemapParse("", parsed),
          "text that is not a valid mapping is refused (short, confirm on ZL, empty)");

    // What Poll hands the menu. map: confirm on X, X's action on A, back on L, L's action on ZL.
    SetRemap(map);
    Poll(false);
    Sleep(150); // (clear of the rule that drops an XInput copy of a press made just before)
    Press(1, Kind::DualSense, kWest); // square = X on a DualSense
    Check(Poll(false).buttons == xA, "remapped: X confirms (DualSense square)");
    Press(1, Kind::DualSense, kSouth);
    Check(Poll(false).buttons == xX, "remapped: A does what X did");
    Press(1, Kind::DualSense, kL);
    Check(Poll(false).buttons == xB, "remapped: L goes back");
    Press(1, Kind::DualSense, kZL);
    Check(Poll(false).buttons == xL, "remapped: ZL turns the page as L did");
    Press(1, Kind::DualSense, 0);
    Poll(false);
    Sleep(150);
    SetXInput(0, true, 0x4000, 0, 0); // an Xbox pad's X
    Check(Poll(false).buttons == xA, "remapped: an XInput pad's X confirms too");
    SetXInput(0, true, 0, 0, 0);
    SetXInputMore(0, 0, 0, 1.0f, 0);
    Check(Poll(false).buttons == xL, "remapped: an XInput pad's left trigger is ZL");
    SetXInput(0, false, 0, 0, 0);
    Poll(false);
    // "Intercambiar A y B" keeps its meaning: it decides which button is called A.
    Press(1, Kind::DualSense, kNorth); // triangle: X once A and B (and X and Y) are the console's way round
    Check(Poll(true).buttons == xB, "with A/B swapped, the button now called X confirms (sent as XInput B, which the app turns back)");
    Press(1, Kind::DualSense, 0);
    Poll(true);
    // Capture and HOME as actions.
    std::copy(stock, stock + kActions, map);
    Check(RemapAssign(map, aCapture, bY), "capture moved to Y");
    SetRemap(map);
    Press(1, Kind::DualSense, kNorth);
    Menu m = Poll(false);
    Check(m.capture && m.buttons == 0, "remapped: Y takes the picture and does nothing else");
    m = Poll(false);
    Check(!m.capture, "... once per press");
    Press(1, Kind::DualSense, 0);
    Poll(false);
    int bad[kActions] = {0, 1, 2, 3, 14, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    SetRemap(bad);
    GetRemap(parsed);
    Check(std::equal(map, map + kActions, parsed), "an invalid mapping handed to the pads is ignored");
    SetRemap(stock);

    // A page that reads the pads itself.
    Press(2, Kind::SwitchPro, kEast | kL);
    std::vector<Live> live = Snapshot(false);
    Check(live.size() == 2 && live[1].pad.id == "mac:bb" && live[1].st.buttons == (kEast | kL) && live[1].menu == (mA | mL),
          "snapshot: the Pro Controller holds A and L, under the menu's names too");
    Check(live[0].pad.player == 1 && live[1].pad.player == 2, "snapshot: pads carry their player numbers");
    Press(2, Kind::SwitchPro, 0);
    Poll(false);
    Press(1, Kind::DualSense, kSouth);
    Press(1, Kind::DualSense, 0);
    live = Snapshot(false);
    Check(live[0].st.buttons == kSouth && live[0].menu == mA, "snapshot: a press and release between two frames is still shown");
    Check(Snapshot(false)[0].st.buttons == 0, "... once");
    Check(Snapshot(true)[0].menu == 0 && Poll(false).buttons == xA, "the menu still gets that press: the two readers do not take from each other");
    Poll(false);
    Hush();
    Press(1, Kind::DualSense, kEast);
    Check(Poll(false).buttons == 0, "hushed: the menu gets nothing while a page has every button under test");
    Check(Snapshot(false)[0].st.buttons == kEast, "... and the page sees the button");
    Sleep(300);
    Check(Poll(false).buttons == 0, "after the hush a button still held from the page does not act");
    Press(1, Kind::DualSense, 0);
    Poll(false);
    Press(1, Kind::DualSense, kEast);
    Check(Poll(false).buttons == xB, "once let go and pressed again it does");
    Press(1, Kind::DualSense, 0);
    Poll(false);

    // HOME held.
    Press(1, Kind::DualSense, kHome);
    Check((Poll(false).buttons & 0x0400) && !TakeHomeHold(), "HOME pressed: acts at once, not yet a hold");
    Sleep(1050);
    Poll(false);
    Check(TakeHomeHold(), "HOME kept down for a second is a hold");
    Poll(false);
    Check(!TakeHomeHold(), "... reported once");
    Press(1, Kind::DualSense, 0);
    Poll(false);

    Check(!ProWired(), "the wake-up for a Pro Controller on a cable is off unless asked for (the console's default)");
    Remove(1);
    Remove(2);
}

// ---- the emulators' files -------------------------------------------------------

padcfg::Want Sdl(int player, uint16_t version, bool bluetooth = false, uint16_t product = 0x0CE6, uint16_t vendor = 0x054C) {
    padcfg::Want w;
    w.player = player;
    w.what = padcfg::Want::What::Sdl;
    SdlGuid(vendor, product, version, vendor == 0x054C ? "DualSense Wireless Controller" : nullptr, w.guid);
    w.crcKnown = vendor == 0x054C;
    w.bluetooth = bluetooth;
    return w;
}

padcfg::Want Keyboard(int player) {
    padcfg::Want w;
    w.player = player;
    w.what = padcfg::Want::What::Keyboard;
    return w;
}

void TestRyujinx(const std::wstring& src, const std::wstring& work) {
    printf("Ryujinx family (Config.json)\n");
    const std::wstring file = work + L"\\Config.json", backup = file + L".dockside-backup";
    DeleteFileW(backup.c_str());
    std::string original, now;
    if (!CopyFileW(src.c_str(), file.c_str(), FALSE) || !Slurp(file, original)) return Check(false, "copying Config.json");

    // The owner's file: keyboard player 1, DualSense (USB identifier) player 2.
    padcfg::Result r = padcfg::ApplyRyujinx(file, {Sdl(1, 0x0000)});
    Slurp(file, now);
    Check(r.ok && !r.changed && r.missing == std::vector<int>{1} && now == original && !Exists(backup),
          "a pad the file has no entry for (other version number): reported, file and folder untouched");
    r = padcfg::ApplyRyujinx(file, {Sdl(1, 0x0100)}, true);
    Slurp(file, now);
    Check(r.ok && r.changed && now == original && !Exists(backup), "checking only writes nothing");

    r = padcfg::ApplyRyujinx(file, {Sdl(1, 0x0100)});
    Slurp(file, now);
    Check(r.ok && r.changed && r.missing.empty(), "DualSense as player 1: written");
    Check(BytesDiffering(original, now) == 2, "exactly two bytes differ (the digits of two player_index values)");
    Check(now.find("\"id\": \"0-56570003-054c-0000-e60c-000000016800\",\r\n      \"controller_type\": \"ProController\",\r\n      \"player_index\": \"Player1\"") != std::string::npos ||
          now.find("\"id\": \"0-56570003-054c-0000-e60c-000000016800\",\n      \"controller_type\": \"ProController\",\n      \"player_index\": \"Player1\"") != std::string::npos,
          "the DualSense entry now says Player1");
    Check(now.find("\"id\": \"0\",") != std::string::npos && now.find("\"player_index\": \"Player2\"") != std::string::npos, "the keyboard entry took Player2");
    std::string kept;
    Check(Slurp(backup, kept) && kept == original, "the backup holds the file as it was");
    r = padcfg::ApplyRyujinx(file, {Sdl(1, 0x0100)});
    Check(r.ok && !r.changed, "asking for what is already there writes nothing");

    r = padcfg::ApplyRyujinx(file, {Keyboard(1)}); // the pad was switched off: keyboard is player 1 again
    Slurp(file, now);
    Check(r.ok && r.changed && now == original, "the opposite change gives the original file back, byte for byte");
    r = padcfg::ApplyRyujinx(file, {Keyboard(1), Sdl(2, 0x0100)});
    Check(r.ok && !r.changed, "keyboard 1 + DualSense 2 is what the file says: nothing to do");

    // A Ryubing-style id (name CRC cleared) is the same pad; an SDL 3 entry
    // with the USB bus is not a Bluetooth pad.
    std::string edited = original;
    const size_t at = edited.find("0-56570003");
    if (at != std::string::npos) edited.replace(at, 10, "0-00000003");
    FILE* f = nullptr;
    if (!_wfopen_s(&f, file.c_str(), L"wb") && f) fwrite(edited.data(), 1, edited.size(), f), fclose(f);
    r = padcfg::ApplyRyujinx(file, {Sdl(1, 0x0100)}, true);
    Check(r.ok && r.changed && r.missing.empty(), "an id with the name part cleared (Ryubing) is recognised");
    const size_t be = edited.find("GamepadSDL2");
    if (be != std::string::npos) edited.replace(be, 11, "GamepadSDL3");
    if (!_wfopen_s(&f, file.c_str(), L"wb") && f) fwrite(edited.data(), 1, edited.size(), f), fclose(f);
    r = padcfg::ApplyRyujinx(file, {Sdl(1, 0x0100, true)}, true);
    Check(r.ok && !r.changed && r.missing.size() == 1, "SDL 3 entry made over USB, pad now on Bluetooth: not taken for it");
    r = padcfg::ApplyRyujinx(file, {Sdl(1, 0x0100, false)}, true);
    Check(r.ok && r.changed, "SDL 3 entry made over USB, pad on USB: recognised");
}

// Every line that differs between a and b belongs to "player_<n>_" of the given players.
bool OnlyPlayers(const std::string& a, const std::string& b, const char* digits, int& lines) {
    const std::vector<std::string> x = Lines(a), y = Lines(b);
    lines = 0;
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); i++) {
        if (x[i] == y[i]) continue;
        lines++;
        for (const std::string* s : {&x[i], &y[i]})
            if (s->compare(0, 7, "player_") != 0 || !strchr(digits, (*s)[7]) || (*s)[8] != '_') return false;
    }
    return true;
}

void TestYuzu(const std::wstring& citron, const std::wstring& eden, const std::wstring& work) {
    printf("yuzu family (qt-config.ini)\n");
    const std::wstring file = work + L"\\qt-config.ini", backup = file + L".dockside-backup";
    DeleteFileW(backup.c_str());
    std::string original, now, swapped;
    if (!CopyFileW(citron.c_str(), file.c_str(), FALSE) || !Slurp(file, original)) return Check(false, "copying Citron's qt-config.ini");

    // Citron's file: player 1 = DualSense (version 0000), player 2 = DualSense
    // (version 0100), both connected.
    padcfg::Result r = padcfg::ApplyYuzu(file, {Sdl(1, 0x0000), Sdl(2, 0x0100)});
    Check(r.ok && !r.changed && !Exists(backup), "the order the file already has: nothing written");
    r = padcfg::ApplyYuzu(file, {Sdl(1, 0x0000, false, 0x2009, 0x057E)});
    Slurp(file, now);
    Check(r.ok && r.missing == std::vector<int>{1}, "a Pro Controller the file has no entry for is reported");
    int lines = 0;
    Check(OnlyPlayers(original, now, "1", lines) && lines == 1 && now.find("player_1_connected=false") != std::string::npos,
          "... its player keeps what the file says; the only change is player 2 (no pad) set to not connected");
    CopyFileW(citron.c_str(), file.c_str(), FALSE);
    DeleteFileW(backup.c_str());

    r = padcfg::ApplyYuzu(file, {Sdl(1, 0x0100), Sdl(2, 0x0000)});
    Slurp(file, swapped);
    Check(r.ok && r.changed && r.missing.empty(), "the two DualSense entries swapped: written");
    Check(swapped.size() == original.size() && OnlyPlayers(original, swapped, "01", lines), "only player_0_ and player_1_ lines differ");
    printf("  %d lines changed\n", lines);
    Check(swapped.find("player_0_button_a=\"engine:sdl,port:0,guid:030000004c050000e60c000000016800,button:1\"") != std::string::npos &&
          swapped.find("player_1_button_a=\"engine:sdl,port:0,guid:030000004c050000e60c000000006800,button:1\"") != std::string::npos,
          "each mapping moved whole, identifiers untouched");
    {
        // The same lines, renumbered back, are the original ones (nothing inside a mapping changed).
        std::vector<std::string> x = Lines(original), y = Lines(swapped);
        bool same = true;
        for (size_t i = 0; i < x.size() && i < y.size(); i++) {
            if (x[i] == y[i]) continue;
            std::string back = y[i];
            back[7] = back[7] == '0' ? '1' : '0';
            same = same && back == x[i];
        }
        Check(same, "each changed line differs in its player digit only");
    }
    Check(swapped.find("player_0_connected\\default=true") != std::string::npos && swapped.find("player_1_connected\\default=false") != std::string::npos,
          "the connected lines stayed with their players");
    std::string kept;
    Check(Slurp(backup, kept) && kept == original, "the backup holds the file as it was");
    r = padcfg::ApplyYuzu(file, {Sdl(1, 0x0000), Sdl(2, 0x0100)});
    Slurp(file, now);
    Check(r.ok && r.changed && now == original, "the opposite change gives the original file back, byte for byte");

    r = padcfg::ApplyYuzu(file, {Sdl(1, 0x0100)}); // only one pad: player 2 has none
    Slurp(file, now);
    Check(r.ok && r.changed && OnlyPlayers(original, now, "01", lines) && now.find("player_1_connected=false") != std::string::npos,
          "one pad as player 1: entries swapped and player 2 marked not connected");
    r = padcfg::ApplyYuzu(file, {Sdl(1, 0x0000), Sdl(2, 0x0100)});
    Slurp(file, now);
    Check(r.ok && now == original, "... and undone exactly");

    r = padcfg::ApplyYuzu(file, {Keyboard(1)}); // no pad at all
    Slurp(file, now);
    Check(r.ok && r.changed && now.find("player_0_button_a=\"engine:keyboard") != std::string::npos &&
          now.find("player_1_connected=false") != std::string::npos && now.find("guid:030000004c050000e60c000000006800") != std::string::npos,
          "keyboard as player 1: a keyboard entry moved to player 1, the pad's entry kept elsewhere, player 2 not connected");
    r = padcfg::ApplyYuzu(file, {Sdl(1, 0x0000), Sdl(2, 0x0100)});
    Slurp(file, now);
    Check(r.ok && now.find("player_0_button_a=\"engine:sdl,port:0,guid:030000004c050000e60c000000006800") != std::string::npos &&
          now.find("player_1_button_a=\"engine:sdl,port:0,guid:030000004c050000e60c000000016800") != std::string::npos &&
          now.find("player_1_connected=true") != std::string::npos,
          "the pads back as players 1 and 2: both entries found again and connected");

    // Eden's file: keyboard only.
    DeleteFileW(backup.c_str());
    if (!CopyFileW(eden.c_str(), file.c_str(), FALSE) || !Slurp(file, original)) return Check(false, "copying Eden's qt-config.ini");
    r = padcfg::ApplyYuzu(file, {Sdl(1, 0x0100)});
    Slurp(file, now);
    Check(r.ok && !r.changed && r.missing == std::vector<int>{1} && now == original && !Exists(backup),
          "Eden, which has never seen the pad: reported, file and folder untouched");
    r = padcfg::ApplyYuzu(file, {Keyboard(1)});
    Slurp(file, now);
    Check(r.ok && !r.changed && now == original, "Eden with the keyboard as player 1: already so, untouched");
}

// ---- single values: vibration, keyboard, mouse, dead zones --------------------------

// The lines of `b` that differ from `a`'s (same number of lines expected), joined.
std::string ChangedLines(const std::string& a, const std::string& b, int& count) {
    const std::vector<std::string> x = Lines(a), y = Lines(b);
    std::string out;
    count = x.size() == y.size() ? 0 : -1;
    for (size_t i = 0; count >= 0 && i < x.size(); i++)
        if (x[i] != y[i]) count++, out += y[i];
    return out;
}

void TestValues(const std::wstring& data, const std::wstring& work) {
    using padcfg::Family;
    using padcfg::Switch;
    std::string original, now, kept;
    int lines = 0;

    printf("single values, Ryujinx family\n");
    {
        const std::wstring file = work + L"\\Config.json", backup = file + L".dockside-backup";
        DeleteFileW(backup.c_str());
        if (!CopyFileW((data + L"\\ryujinx\\Config.json").c_str(), file.c_str(), FALSE) || !Slurp(file, original))
            return Check(false, "copying Ryujinx's Config.json");
        padcfg::Rumble r = padcfg::RumbleGet(file, Family::Ryujinx);
        Check(r.ok && r.entries == 1 && r.on == 0, "vibration: one pad entry has the switch (the keyboard's has none), and it is off");
        Check(padcfg::RumbleSet(file, Family::Ryujinx, false) && !Exists(backup), "vibration off when it is off: nothing written, no backup made");
        Check(padcfg::RumbleSet(file, Family::Ryujinx, true) && Slurp(file, now), "vibration on: written");
        std::string line = ChangedLines(original, now, lines);
        Check(lines == 1 && line.find("\"enable_rumble\": true") != std::string::npos && now.size() + 1 == original.size(),
              "... one line differs, the entry's \"enable_rumble\": true (the file is one byte shorter)");
        Check(Slurp(backup, kept) && kept == original, "... the backup holds the file as it was");
        r = padcfg::RumbleGet(file, Family::Ryujinx);
        Check(r.on == 1, "... and reads back as on");
        Check(padcfg::RumbleSet(file, Family::Ryujinx, false) && Slurp(file, now) && now == original, "vibration off again: the original file, byte for byte");

        Check(padcfg::SwitchGet(file, Family::Ryujinx, Switch::Keyboard) == 0 && padcfg::SwitchGet(file, Family::Ryujinx, Switch::Mouse) == 0,
              "keyboard and mouse are not handed to games in the owner's file");
        Check(padcfg::SwitchSet(file, Family::Ryujinx, Switch::Keyboard, true) && Slurp(file, now), "keyboard on: written");
        line = ChangedLines(original, now, lines);
        Check(lines == 1 && line.find("\"enable_keyboard\": true") == 2, "... one line differs: the top-level \"enable_keyboard\": true");
        Check(padcfg::SwitchSet(file, Family::Ryujinx, Switch::Mouse, true) && Slurp(file, now) &&
              ChangedLines(original, now, lines).find("\"enable_mouse\": true") != std::string::npos && lines == 2, "mouse on: its line too");
        Check(padcfg::SwitchSet(file, Family::Ryujinx, Switch::Keyboard, false) && padcfg::SwitchSet(file, Family::Ryujinx, Switch::Mouse, false) &&
              Slurp(file, now) && now == original, "both off again: the original file, byte for byte");
        Check(padcfg::MouseSpeedGet(file, Family::Ryujinx) == -1 && !padcfg::MouseSpeedSet(file, Family::Ryujinx, 60) && Slurp(file, now) && now == original,
              "Ryujinx has no mouse speed: reported, a write is refused");

        padcfg::Zones z = padcfg::ZonesGet(file, Family::Ryujinx, Sdl(2, 0x0100));
        Check(z.found && Near(static_cast<float>(z.left), 0.1f) && Near(static_cast<float>(z.right), 0.1f), "dead zones of the DualSense's entry: 0.1 and 0.1");
        Check(padcfg::ZoneSet(file, Family::Ryujinx, Sdl(2, 0x0100), false, 0.15) && Slurp(file, now), "left dead zone 0.15: written");
        line = ChangedLines(original, now, lines);
        Check(lines == 1 && line.find("\"deadzone_left\": 0.15,") != std::string::npos, "... one line differs: \"deadzone_left\": 0.15");
        Check(padcfg::ZoneSet(file, Family::Ryujinx, Sdl(2, 0x0100), true, 0.3) && Slurp(file, now) &&
              ChangedLines(original, now, lines).find("\"deadzone_right\": 0.3,") != std::string::npos && lines == 2, "right dead zone 0.3: its line too");
        z = padcfg::ZonesGet(file, Family::Ryujinx, Sdl(2, 0x0100));
        Check(Near(static_cast<float>(z.left), 0.15f) && Near(static_cast<float>(z.right), 0.3f), "... and they read back");
        Check(padcfg::ZoneSet(file, Family::Ryujinx, Sdl(2, 0x0100), false, 0.1) && padcfg::ZoneSet(file, Family::Ryujinx, Sdl(2, 0x0100), true, 0.1) &&
              Slurp(file, now) && now == original, "both back to 0.1: the original file, byte for byte");
        z = padcfg::ZonesGet(file, Family::Ryujinx, Sdl(1, 0x0000));
        Check(!z.found && !padcfg::ZoneSet(file, Family::Ryujinx, Sdl(1, 0x0000), false, 0.2) && Slurp(file, now) && now == original,
              "a pad the file has no entry for: no dead zone to show, a write is refused");
        Check(!padcfg::ZonesGet(file, Family::Ryujinx, Keyboard(1)).found, "the keyboard's entry has no sticks to speak of");
        Check(!padcfg::ZoneSet(file, Family::Ryujinx, Sdl(2, 0x0100), false, 1.5) && Slurp(file, now) && now == original, "a dead zone outside 0..0.9 is refused");

        for (const wchar_t* other : {L"ryubing", L"kenjinx"}) {
            DeleteFileW(backup.c_str());
            if (!CopyFileW((data + L"\\" + other + L"\\Config.json").c_str(), file.c_str(), FALSE) || !Slurp(file, original)) {
                Check(false, "copying a fork's Config.json");
                continue;
            }
            r = padcfg::RumbleGet(file, Family::Ryujinx);
            Check(r.ok && r.entries == 0 && !padcfg::RumbleSet(file, Family::Ryujinx, true) && Slurp(file, now) && now == original && !Exists(backup),
                  "a fork with only a keyboard entry: no vibration switch to set, nothing is added, file untouched");
            Check(padcfg::SwitchSet(file, Family::Ryujinx, Switch::Keyboard, true) && padcfg::SwitchSet(file, Family::Ryujinx, Switch::Keyboard, false) &&
                  Slurp(file, now) && now == original, "... its keyboard switch on and off again gives the original file");
        }
    }

    printf("single values, yuzu family\n");
    for (const wchar_t* emu : {L"citron", L"eden", L"suyu"}) {
        const std::wstring file = work + L"\\qt-config.ini", backup = file + L".dockside-backup";
        DeleteFileW(backup.c_str());
        if (!CopyFileW((data + L"\\" + emu + L"\\config\\qt-config.ini").c_str(), file.c_str(), FALSE) || !Slurp(file, original)) {
            Check(false, "copying a qt-config.ini");
            continue;
        }
        printf("  %ls\n", emu);
        padcfg::Rumble r = padcfg::RumbleGet(file, Family::Yuzu);
        Check(r.ok && r.entries == 1 && r.on == 1, "vibration is on (the built-in value, flagged as such)");
        Check(padcfg::RumbleSet(file, Family::Yuzu, true) && !Exists(backup), "vibration on when it is on: nothing written");
        Check(padcfg::RumbleSet(file, Family::Yuzu, false) && Slurp(file, now), "vibration off: written");
        std::string line = ChangedLines(original, now, lines);
        Check(lines == 2 && line.find("vibration_enabled\\default=false") == 0 && line.find("\nvibration_enabled=false") != std::string::npos,
              "... two lines differ: vibration_enabled\\default=false and vibration_enabled=false");
        Check(now.find("player_0_vibration_enabled=true") != std::string::npos, "... the players' own switches are left alone");
        Check(Slurp(backup, kept) && kept == original, "... the backup holds the file as it was");
        Check(padcfg::RumbleGet(file, Family::Yuzu).on == 0, "... and reads back as off");
        Check(padcfg::RumbleSet(file, Family::Yuzu, true) && Slurp(file, now) && now == original, "vibration on again: the original file, byte for byte");

        Check(padcfg::SwitchGet(file, Family::Yuzu, Switch::Keyboard) == 0 && padcfg::SwitchGet(file, Family::Yuzu, Switch::Mouse) == 0,
              "the emulated keyboard and mouse are off");
        Check(padcfg::SwitchSet(file, Family::Yuzu, Switch::Keyboard, true) && padcfg::SwitchSet(file, Family::Yuzu, Switch::Mouse, true) && Slurp(file, now),
              "keyboard and mouse on: written");
        line = ChangedLines(original, now, lines);
        Check(lines == 4 && line.find("keyboard_enabled\\default=false") != std::string::npos && line.find("\nkeyboard_enabled=true") != std::string::npos &&
              line.find("mouse_enabled\\default=false") != std::string::npos && line.find("\nmouse_enabled=true") != std::string::npos,
              "... four lines differ: the two pairs");
        Check(padcfg::SwitchGet(file, Family::Yuzu, Switch::Keyboard) == 1 && padcfg::SwitchGet(file, Family::Yuzu, Switch::Mouse) == 1, "... and read back as on");
        Check(padcfg::SwitchSet(file, Family::Yuzu, Switch::Keyboard, false) && padcfg::SwitchSet(file, Family::Yuzu, Switch::Mouse, false) &&
              Slurp(file, now) && now == original, "both off again: the original file, byte for byte");

        Check(padcfg::MouseSpeedGet(file, Family::Yuzu) == 50, "mouse speed is the built-in 50");
        Check(padcfg::MouseSpeedSet(file, Family::Yuzu, 75) && Slurp(file, now), "mouse speed 75: written");
        line = ChangedLines(original, now, lines);
        Check(lines == 4 && line.find("mouse_panning_x_sensitivity=75") != std::string::npos && line.find("mouse_panning_y_sensitivity=75") != std::string::npos &&
              now.find("mouse_panning_sensitivity=50") != std::string::npos, "... four lines differ: the x and y pairs (the older single number is left alone)");
        Check(padcfg::MouseSpeedGet(file, Family::Yuzu) == 75 && !padcfg::MouseSpeedSet(file, Family::Yuzu, 0) && !padcfg::MouseSpeedSet(file, Family::Yuzu, 101),
              "... reads back; 0 and 101 are refused");
        Check(padcfg::MouseSpeedSet(file, Family::Yuzu, 50) && Slurp(file, now) && now == original, "mouse speed 50 again: the original file, byte for byte");

        if (!wcscmp(emu, L"citron")) {
            // Player 1 is a DualSense (version 0000) whose sticks carry a dead zone;
            // player 2 (version 0100) has a left stick the emulator never wrote one for.
            padcfg::Zones z = padcfg::ZonesGet(file, Family::Yuzu, Sdl(1, 0x0000));
            Check(z.found && Near(static_cast<float>(z.left), 0.15f) && Near(static_cast<float>(z.right), 0.15f), "dead zones of player 1's pad: 0.15 and 0.15");
            Check(padcfg::ZoneSet(file, Family::Yuzu, Sdl(1, 0x0000), false, 0.2) && Slurp(file, now), "left dead zone 0.2: written");
            line = ChangedLines(original, now, lines);
            Check(lines == 1 && line.find("player_0_lstick=") == 0 && line.find(",deadzone:0.200000\"") != std::string::npos && now.size() == original.size(),
                  "... one line differs, player_0_lstick, in its deadzone number only");
            Check(padcfg::ZoneSet(file, Family::Yuzu, Sdl(1, 0x0000), false, 0.15) && Slurp(file, now) && now == original,
                  "back to 0.15: the original file, byte for byte");
            z = padcfg::ZonesGet(file, Family::Yuzu, Sdl(2, 0x0100));
            Check(z.found && z.left < 0, "player 2's left stick has no dead zone written in the file: said so");
            Check(!padcfg::ZoneSet(file, Family::Yuzu, Sdl(2, 0x0100), false, 0.2) && Slurp(file, now) && now == original, "... and none is invented: the write is refused");
        } else {
            Check(!padcfg::ZonesGet(file, Family::Yuzu, Sdl(1, 0x0100)).found && !padcfg::ZonesGet(file, Family::Yuzu, Keyboard(1)).found,
                  "no pad entry here (keyboard only): no dead zone to show");
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && !wcscmp(argv[1], L"probe")) {
        const std::vector<std::string> found = Probe();
        printf("HID joysticks and game pads Windows lists: %zu\n", found.size());
        for (const auto& line : found) printf("  %s\n", line.c_str());
        using Fn = DWORD(WINAPI*)(DWORD, void*);
        HMODULE lib = LoadLibraryW(L"xinput1_4.dll");
        Fn get = lib ? reinterpret_cast<Fn>(GetProcAddress(lib, "XInputGetState")) : nullptr;
        for (DWORD i = 0; get && i < 4; i++) {
            uint8_t state[32] = {};
            printf("  XInput slot %lu: %s\n", i, get(i, state) == ERROR_SUCCESS ? "connected" : "empty");
        }
        return 0;
    }
    const std::wstring data = argc > 1 ? argv[1] : L"data", work = argc > 2 ? argv[2] : L"build\\pads_test";
    CreateDirectoryW(work.c_str(), nullptr);
    TestReports();
    TestIds(data);
    TestOrder();
    TestRemap();
    TestValues(data, work);
    TestRyujinx(data + L"\\ryujinx\\Config.json", work);
    TestYuzu(data + L"\\citron\\config\\qt-config.ini", data + L"\\eden\\config\\qt-config.ini", work);
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
