// See pads.h. Report layouts: Sony's as in SDL's SDL_hidapi_ps4.c / _ps5.c and
// the Linux hid-playstation driver (which agree); Nintendo's as in
// SDL_hidapi_switch.c and the community's Switch controller notes.
#include "pads.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

namespace pads {

namespace {

constexpr uint16_t kSony = 0x054C, kNintendo = 0x057E;

// Direction-pad "hat": 0 = up, then clockwise in eighths; anything else = centred.
uint32_t HatDirs(int hat) {
    static const uint32_t dirs[8] = {kUp, kUp | kRight, kRight, kRight | kDown, kDown, kDown | kLeft, kLeft, kLeft | kUp};
    return hat >= 0 && hat < 8 ? dirs[hat] : 0;
}

float Axis8(uint8_t v) {
    return (static_cast<int>(v) - 128) / 127.0f;
}

// The three button bytes every Sony report shares.
uint32_t SonyButtons(uint8_t b0, uint8_t b1, uint8_t b2, bool hasMute) {
    uint32_t out = HatDirs(b0 & 0x0F);
    if (b0 & 0x10) out |= kWest;  // square
    if (b0 & 0x20) out |= kSouth; // cross
    if (b0 & 0x40) out |= kEast;  // circle
    if (b0 & 0x80) out |= kNorth; // triangle
    if (b1 & 0x01) out |= kL;
    if (b1 & 0x02) out |= kR;
    if (b1 & 0x04) out |= kZL;
    if (b1 & 0x08) out |= kZR;
    if (b1 & 0x10) out |= kMinus; // Share / Create
    if (b1 & 0x20) out |= kPlus;  // Options
    if (b1 & 0x40) out |= kLS;    // L3
    if (b1 & 0x80) out |= kRS;    // R3
    if (b2 & 0x01) out |= kHome;  // PS
    // The DualSense has no capture button; its microphone button is the one
    // the emulators' own default mapping uses for capture (SDL's "misc1").
    if (hasMute && (b2 & 0x04)) out |= kCapture;
    return out;
}

bool ParseDualSense(Link link, const uint8_t* r, size_t n, State& out) {
    size_t at; // where the state starts
    bool simple = false;
    if (link == Link::Usb && n >= 11 && r[0] == 0x01) at = 1;
    else if (link == Link::Bluetooth && n >= 12 && r[0] == 0x31) at = 2;
    else if (link == Link::Bluetooth && n >= 10 && r[0] == 0x01) at = 1, simple = true;
    else return false;
    State s;
    s.lx = Axis8(r[at]);
    s.ly = -Axis8(r[at + 1]);
    s.rx = Axis8(r[at + 2]);
    s.ry = -Axis8(r[at + 3]);
    s.right = s.analog = true;
    if (simple) {
        // x, y, x, y, three button bytes, two triggers.
        s.buttons = SonyButtons(r[at + 4], r[at + 5], r[at + 6] & 0x03, false);
        s.lt = r[at + 7] / 255.0f, s.rt = r[at + 8] / 255.0f;
    } else {
        // x, y, x, y, two triggers, a counter, then the button bytes.
        s.buttons = SonyButtons(r[at + 7], r[at + 8], r[at + 9], true);
        s.lt = r[at + 4] / 255.0f, s.rt = r[at + 5] / 255.0f;
        if (n > at + 52) {
            const int level = r[at + 52] & 0x0F, status = r[at + 52] >> 4;
            s.battery = status == 2 ? 100 : (std::min)(level * 10 + 5, 100);
            s.charging = status == 1 || status == 2;
        }
    }
    out = s;
    return true;
}

bool ParseDualShock4(Link link, const uint8_t* r, size_t n, State& out) {
    size_t at;
    bool full = true;
    if (n >= 10 && r[0] == 0x01) at = 1, full = link == Link::Usb; // Bluetooth's short report ends after the triggers
    else if (link == Link::Bluetooth && n >= 12 && r[0] >= 0x11 && r[0] <= 0x19 && (r[1] & 0x80)) at = 3;
    else return false;
    State s;
    s.lx = Axis8(r[at]);
    s.ly = -Axis8(r[at + 1]);
    s.rx = Axis8(r[at + 2]);
    s.ry = -Axis8(r[at + 3]);
    s.right = s.analog = true;
    // x, y, x, y, three button bytes, two triggers.
    s.buttons = SonyButtons(r[at + 4], r[at + 5], r[at + 6] & 0x03, false);
    s.lt = r[at + 7] / 255.0f, s.rt = r[at + 8] / 255.0f;
    if (full && n > at + 29) {
        const int level = r[at + 29] & 0x0F;
        s.charging = (r[at + 29] & 0x10) != 0; // cable in
        s.battery = level >= 10 ? 100 : level * 10 + 5;
    }
    out = s;
    return true;
}

float Axis12(int v) {
    // Uncalibrated sticks swing about 1400 either side of 2048.
    return (std::max)(-1.0f, (std::min)(1.0f, (v - 2048) / 1400.0f));
}

bool ParseSwitch(Kind kind, const uint8_t* r, size_t n, State& out) {
    State s;
    if (n >= 12 && (r[0] == 0x30 || r[0] == 0x31 || r[0] == 0x21)) {
        // The pad's full report: timer, battery, right / shared / left button
        // bytes, then each stick as two 12-bit numbers in three bytes.
        const uint8_t right = r[3], shared = r[4], left = r[5];
        if (kind != Kind::JoyConL) {
            if (right & 0x01) s.buttons |= kWest;  // Y
            if (right & 0x02) s.buttons |= kNorth; // X
            if (right & 0x04) s.buttons |= kSouth; // B
            if (right & 0x08) s.buttons |= kEast;  // A
            if (right & 0x40) s.buttons |= kR;
            if (right & 0x80) s.buttons |= kZR;
            if (shared & 0x02) s.buttons |= kPlus;
            if (shared & 0x10) s.buttons |= kHome;
            if (shared & 0x04) s.buttons |= kRS;
            if (kind == Kind::JoyConR) {
                if (right & 0x10) s.buttons |= kSR;
                if (right & 0x20) s.buttons |= kSL;
            }
        }
        if (kind != Kind::JoyConR) {
            if (left & 0x01) s.buttons |= kDown;
            if (left & 0x02) s.buttons |= kUp;
            if (left & 0x04) s.buttons |= kRight;
            if (left & 0x08) s.buttons |= kLeft;
            if (left & 0x40) s.buttons |= kL;
            if (left & 0x80) s.buttons |= kZL;
            if (shared & 0x01) s.buttons |= kMinus;
            if (shared & 0x20) s.buttons |= kCapture;
            if (shared & 0x08) s.buttons |= kLS;
            if (kind == Kind::JoyConL) {
                if (left & 0x10) s.buttons |= kSR;
                if (left & 0x20) s.buttons |= kSL;
            }
        }
        const uint8_t* st = kind == Kind::JoyConR ? r + 9 : r + 6;
        s.lx = Axis12(st[0] | ((st[1] & 0x0F) << 8));
        s.ly = Axis12((st[1] >> 4) | (st[2] << 4));
        if (kind == Kind::SwitchPro) {
            s.rx = Axis12(r[9] | ((r[10] & 0x0F) << 8));
            s.ry = Axis12((r[10] >> 4) | (r[11] << 4));
            s.right = true;
        }
        // High nibble: 8 full, 6, 4, 2, 0 empty; its lowest bit says charging.
        s.battery = ((r[2] >> 4) & 0x0E) * 100 / 8;
        s.charging = (r[2] & 0x10) != 0;
        out = s;
        return true;
    }
    if (n < 4 || r[0] != 0x3F) return false;
    // The short report a pad sends over Bluetooth until a program asks for the
    // full one: two button bytes and a hat; the Pro Controller adds its sticks.
    const uint8_t b1 = r[1], b2 = r[2];
    if (kind == Kind::SwitchPro) {
        if (b1 & 0x01) s.buttons |= kSouth; // B
        if (b1 & 0x02) s.buttons |= kEast;  // A
        if (b1 & 0x04) s.buttons |= kWest;  // Y
        if (b1 & 0x08) s.buttons |= kNorth; // X
        if (b1 & 0x10) s.buttons |= kL;
        if (b1 & 0x20) s.buttons |= kR;
        if (b1 & 0x40) s.buttons |= kZL;
        if (b1 & 0x80) s.buttons |= kZR;
        if (b2 & 0x01) s.buttons |= kMinus;
        if (b2 & 0x02) s.buttons |= kPlus;
        if (b2 & 0x10) s.buttons |= kHome;
        if (b2 & 0x20) s.buttons |= kCapture;
        if (b2 & 0x04) s.buttons |= kLS;
        if (b2 & 0x08) s.buttons |= kRS;
        s.buttons |= HatDirs(r[3]);
        if (n >= 8) {
            s.lx = ((r[4] | (r[5] << 8)) - 32768) / 32768.0f;
            s.ly = -((r[6] | (r[7] << 8)) - 32768) / 32768.0f;
        }
        if (n >= 12) {
            s.rx = ((r[8] | (r[9] << 8)) - 32768) / 32768.0f;
            s.ry = -((r[10] | (r[11] << 8)) - 32768) / 32768.0f;
            s.right = true;
        }
        out = s;
        return true;
    }
    // A Joy-Con's short report is laid out for the sideways hold: the first
    // four bits are the buttons at the bottom, right, left and top as held
    // that way, and the hat is the stick. Turned back upright here: the left
    // Joy-Con lies a quarter turn anticlockwise, the right one clockwise.
    static const float hx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, hy[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    float sx = 0, sy = 0;
    if (r[3] < 8) sx = hx[r[3]], sy = hy[r[3]];
    if (kind == Kind::JoyConL) {
        if (b1 & 0x01) s.buttons |= kLeft;
        if (b1 & 0x02) s.buttons |= kDown;
        if (b1 & 0x04) s.buttons |= kUp;
        if (b1 & 0x08) s.buttons |= kRight;
        if (b2 & 0x01) s.buttons |= kMinus;
        if (b2 & 0x04) s.buttons |= kLS;
        if (b2 & 0x20) s.buttons |= kCapture;
        if (b2 & 0x40) s.buttons |= kL;
        if (b2 & 0x80) s.buttons |= kZL;
        s.lx = sy, s.ly = -sx;
    } else {
        if (b1 & 0x01) s.buttons |= kEast;  // A
        if (b1 & 0x02) s.buttons |= kNorth; // X
        if (b1 & 0x04) s.buttons |= kSouth; // B
        if (b1 & 0x08) s.buttons |= kWest;  // Y
        if (b2 & 0x02) s.buttons |= kPlus;
        if (b2 & 0x08) s.buttons |= kRS;
        if (b2 & 0x10) s.buttons |= kHome;
        if (b2 & 0x40) s.buttons |= kR;
        if (b2 & 0x80) s.buttons |= kZR;
        s.lx = -sy, s.ly = sx;
    }
    if (b1 & 0x10) s.buttons |= kSL;
    if (b1 & 0x20) s.buttons |= kSR;
    out = s;
    return true;
}

} // namespace

bool Known(uint16_t vendor, uint16_t product, Kind& kind) {
    if (vendor == kSony) {
        if (product == 0x0CE6 || product == 0x0DF2) return kind = Kind::DualSense, true;
        if (product == 0x05C4 || product == 0x09CC) return kind = Kind::DualShock4, true;
    } else if (vendor == kNintendo) {
        if (product == 0x2009) return kind = Kind::SwitchPro, true;
        if (product == 0x2006) return kind = Kind::JoyConL, true;
        if (product == 0x2007) return kind = Kind::JoyConR, true;
    }
    return false;
}

bool Parse(Kind kind, Link link, const uint8_t* r, size_t n, State& out) {
    if (!r || !n) return false;
    switch (kind) {
    case Kind::DualSense: return ParseDualSense(link, r, n, out);
    case Kind::DualShock4: return ParseDualShock4(link, r, n, out);
    case Kind::SwitchPro:
    case Kind::JoyConL:
    case Kind::JoyConR: return ParseSwitch(kind, r, n, out);
    default: return false;
    }
}

State Sideways(Kind joyCon, const State& u) {
    State s = u;
    s.buttons &= ~(kUp | kDown | kLeft | kRight | kSouth | kEast | kWest | kNorth | kL | kR | kSL | kSR | kMinus | kLS | kRS);
    const auto move = [&](uint32_t from, uint32_t to) {
        if (u.buttons & from) s.buttons |= to;
    };
    if (joyCon == Kind::JoyConL) {
        move(kLeft, kSouth), move(kDown, kEast), move(kRight, kNorth), move(kUp, kWest);
        move(kMinus, kPlus); // its only menu button stands in for +
        s.lx = -u.ly, s.ly = u.lx;
    } else {
        move(kEast, kSouth), move(kNorth, kEast), move(kWest, kNorth), move(kSouth, kWest);
        s.lx = u.ly, s.ly = -u.lx;
    }
    move(kSL, kL), move(kSR, kR);
    move(kLS, kLS), move(kRS, kLS); // held alone, its stick is the only one
    return s;
}

// ---- SDL's names ----------------------------------------------------------------

uint16_t SdlCrc16(const char* text) {
    // SDL_crc16: the reflected CRC-16 with polynomial A001, starting from 0.
    uint16_t crc = 0;
    for (const char* p = text; p && *p; p++) {
        uint8_t r = static_cast<uint8_t>(crc) ^ static_cast<uint8_t>(*p);
        uint16_t c = 0;
        for (int i = 0; i < 8; i++) {
            c = static_cast<uint16_t>((((c ^ r) & 1) ? 0xA001 : 0) ^ (c >> 1));
            r >>= 1;
        }
        crc = static_cast<uint16_t>(c ^ (crc >> 8));
    }
    return crc;
}

void SdlGuid(uint16_t vendor, uint16_t product, uint16_t version, const char* name, uint8_t out[16]) {
    const uint16_t crc = name ? SdlCrc16(name) : 0;
    const uint16_t words[7] = {0x0003, crc, vendor, 0, product, 0, version};
    for (int i = 0; i < 7; i++) {
        out[i * 2] = static_cast<uint8_t>(words[i] & 0xFF);
        out[i * 2 + 1] = static_cast<uint8_t>(words[i] >> 8);
    }
    out[14] = 'h'; // SDL's HIDAPI drivers
    out[15] = 0;
}

const char* SdlName(Kind kind, uint16_t product) {
    // HIDAPI_SetDeviceName in SDL_hidapi_ps5.c / _ps4.c (official pads).
    if (kind == Kind::DualSense) return product == 0x0DF2 ? "DualSense Edge Wireless Controller" : "DualSense Wireless Controller";
    if (kind == Kind::DualShock4) return "PS4 Controller";
    return nullptr;
}

std::string RyujinxId(const uint8_t g[16], int index) {
    char text[64];
    snprintf(text, sizeof text, "%d-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", index, g[3], g[2],
             g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return text;
}

std::string YuzuGuid(const uint8_t g[16]) {
    std::string out;
    for (int i = 0; i < 16; i++) {
        char hex[4];
        snprintf(hex, sizeof hex, "%02x", (i == 2 || i == 3) ? 0 : g[i]);
        out += hex;
    }
    return out;
}

// ---- the connected pads ---------------------------------------------------------

namespace {

struct Dev {
    uint64_t handle = 0;
    DeviceInfo info;
    State st;
    uint32_t latch = 0; // presses not yet seen by Poll (a press can be shorter than a frame)
    uint32_t shown = 0; // the same for Snapshot, which has its own readers
    uint64_t seq = 0;   // arrival order
};

struct Slot {
    bool present = false;
    uint16_t buttons = 0, prev = 0, suppress = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    uint64_t seq = 0;
    int echo = 0, own = 0; // presses that followed / did not follow one on a pad read here
};

std::mutex mu;
std::vector<Dev> devs;
Slot slots[4];
uint64_t nextSeq = 1;
std::vector<std::string> order;
std::set<std::string> singles;                          // Joy-Con chosen to be used alone
std::vector<std::pair<std::string, std::string>> pairs; // Joy-Con chosen to be used together
bool listening = false, faking = false;
std::vector<std::string> chords;
double lastHidPress = -1e9;
uint32_t prevHid = 0;

// Remapping and the pages that read pads themselves (see pads.h).
int remap[kActions] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
double hushUntil = -1e9, homeSince = -1;
uint32_t carry = 0, prevActs = 0; // actions held over from a hushed page / held at the last Poll
bool stickCarry = false, homeTaken = false;
std::atomic<bool> proWired{false};

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Logical {
    Pad pad;
    State st; // as the player holds it
    bool nintendo = false;
    uint64_t seq = 0;
};

Dev* FindDev(const std::string& id, Kind kind) {
    for (auto& d : devs)
        if (d.info.id == id && d.info.kind == kind) return &d;
    return nullptr;
}

bool ShadowSlot(const Slot& s) {
    return s.echo >= 2 && s.own == 0;
}

std::string PairId(const Dev& l, const Dev& r) {
    return "pair:" + l.info.id + "+" + r.info.id;
}

// An XInput slot's buttons by where they sit, like every other pad's.
uint32_t SlotButtons(uint16_t x, float lt, float rt) {
    static const struct {
        uint16_t from;
        uint32_t to;
    } map[] = {{0x0001, kUp},    {0x0002, kDown},  {0x0004, kLeft}, {0x0008, kRight}, {0x0010, kPlus},
               {0x0020, kMinus}, {0x0040, kLS},    {0x0080, kRS},   {0x0100, kL},     {0x0200, kR},
               {0x0400, kHome},  {0x1000, kSouth}, {0x2000, kEast}, {0x4000, kWest},  {0x8000, kNorth}};
    uint32_t out = 0;
    for (const auto& m : map)
        if (x & m.from) out |= m.to;
    if (lt > 0.5f) out |= kZL;
    if (rt > 0.5f) out |= kZR;
    return out;
}

// Whose pending presses a Build takes with it: nobody's, the menu's (Poll) or
// the pages' (Snapshot).
enum class Take { None, Menu, Page };

// Caller holds the lock.
std::vector<Logical> Build(Take take) {
    std::vector<Logical> out;
    std::set<const Dev*> used;
    const auto state = [&](Dev& d) {
        State s = d.st;
        s.buttons |= take == Take::Page ? d.shown : d.latch;
        if (take == Take::Menu) d.latch = 0;
        if (take == Take::Page) d.shown = 0;
        return s;
    };
    const auto fill = [](Logical& l, const Dev& d) {
        l.pad.link = d.info.link;
        l.pad.vendor = d.info.vendor, l.pad.product = d.info.product, l.pad.version = d.info.version;
        l.seq = d.seq;
    };
    const auto pair = [&](Dev* l, Dev* r) {
        if (!l || !r || used.count(l) || used.count(r)) return;
        used.insert(l), used.insert(r);
        Logical p;
        fill(p, *l);
        p.pad.kind = Kind::JoyConPair;
        p.pad.id = PairId(*l, *r);
        p.nintendo = true;
        const State a = state(*l), b = state(*r);
        p.st = a;
        p.st.rx = b.lx, p.st.ry = b.ly, p.st.right = true;
        p.st.buttons = (a.buttons | b.buttons) & ~(kSL | kSR);
        p.pad.battery = a.battery, p.pad.battery2 = b.battery;
        p.pad.charging = a.charging || b.charging;
        p.seq = (std::min)(l->seq, r->seq);
        out.push_back(p);
    };
    for (const auto& p : pairs) pair(FindDev(p.first, Kind::JoyConL), FindDev(p.second, Kind::JoyConR));
    for (const auto& id : order) {
        if (id.compare(0, 5, "pair:") != 0) continue;
        const size_t plus = id.find('+');
        if (plus == std::string::npos) continue;
        const std::string a = id.substr(5, plus - 5), b = id.substr(plus + 1);
        if (!singles.count(a) && !singles.count(b)) pair(FindDev(a, Kind::JoyConL), FindDev(b, Kind::JoyConR));
    }
    // Loose Joy-Con: a left and a right one go together unless told otherwise.
    const auto loose = [&](const Dev& d) {
        return !used.count(&d) && !singles.count(d.info.id) && std::find(order.begin(), order.end(), d.info.id) == order.end();
    };
    for (auto& l : devs) {
        if (l.info.kind != Kind::JoyConL || !loose(l)) continue;
        for (auto& r : devs)
            if (r.info.kind == Kind::JoyConR && loose(r)) {
                pair(&l, &r);
                break;
            }
    }
    for (auto& d : devs) {
        if (used.count(&d)) continue;
        Logical l;
        fill(l, d);
        l.pad.kind = d.info.kind;
        l.pad.id = d.info.id;
        const bool joyCon = d.info.kind == Kind::JoyConL || d.info.kind == Kind::JoyConR;
        l.nintendo = joyCon || d.info.kind == Kind::SwitchPro;
        l.st = joyCon ? Sideways(d.info.kind, state(d)) : state(d);
        l.pad.battery = l.st.battery;
        l.pad.charging = l.st.charging;
        for (const auto& o : devs)
            if (&o != &d && o.seq < d.seq && o.info.vendor == d.info.vendor && o.info.product == d.info.product &&
                o.info.version == d.info.version)
                l.pad.sameIndex++;
        out.push_back(l);
    }
    for (int i = 0; i < 4; i++) {
        if (!slots[i].present || ShadowSlot(slots[i])) continue;
        Logical l;
        l.pad.kind = Kind::Xbox;
        l.pad.link = Link::XInput;
        l.pad.id = "xinput:" + std::to_string(i);
        l.seq = slots[i].seq;
        l.st.buttons = SlotButtons(slots[i].buttons, slots[i].lt, slots[i].rt);
        l.st.lx = slots[i].lx, l.st.ly = slots[i].ly, l.st.rx = slots[i].rx, l.st.ry = slots[i].ry;
        l.st.lt = slots[i].lt, l.st.rt = slots[i].rt;
        l.st.right = l.st.analog = true;
        for (int k = 0; k < i; k++)
            if (slots[k].present && !ShadowSlot(slots[k])) l.pad.sameIndex++;
        out.push_back(l);
    }
    // Player order: remembered pads first, in that order; the rest as they came.
    const auto rank = [&](const Logical& l) {
        const auto it = std::find(order.begin(), order.end(), l.pad.id);
        return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
    };
    std::stable_sort(out.begin(), out.end(), [&](const Logical& a, const Logical& b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : a.seq < b.seq;
    });
    return out;
}

void Chord(const std::string& id) {
    if (listening && std::find(chords.begin(), chords.end(), id) == chords.end()) chords.push_back(id);
}

// Caller holds the lock; `was` is the device's state before this report.
void WatchChord(Dev& d, const State& was) {
    if (!listening) return;
    const uint32_t b = d.st.buttons, p = was.buttons;
    const auto both = [](uint32_t v, uint32_t m) { return (v & m) == m; };
    if (d.info.kind == Kind::JoyConL || d.info.kind == Kind::JoyConR) {
        const bool left = d.info.kind == Kind::JoyConL;
        if (both(b, kSL | kSR) && !both(p, kSL | kSR)) {
            singles.insert(d.info.id);
            pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [&](const std::pair<std::string, std::string>& x) {
                            return x.first == d.info.id || x.second == d.info.id;
                        }), pairs.end());
            Chord(d.info.id);
            return;
        }
        const uint32_t mine = left ? kL : kR, theirs = left ? kR : kL;
        if (!(b & mine) || (p & mine)) return;
        for (auto& o : devs) {
            if (o.info.kind != (left ? Kind::JoyConR : Kind::JoyConL) || !(o.st.buttons & theirs)) continue;
            Dev& l = left ? d : o;
            Dev& r = left ? o : d;
            for (const Dev* x : {&l, &r}) {
                singles.erase(x->info.id);
                pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [&](const std::pair<std::string, std::string>& y) {
                                return y.first == x->info.id || y.second == x->info.id;
                            }), pairs.end());
            }
            pairs.push_back({l.info.id, r.info.id});
            Chord(PairId(l, r));
            return;
        }
    } else if (both(b, kL | kR) && !both(p, kL | kR)) {
        Chord(d.info.id);
    }
}

} // namespace

void Arrive(uint64_t handle, const DeviceInfo& info) {
    std::lock_guard<std::mutex> lock(mu);
    if (faking) return;
    for (auto& d : devs)
        if (d.handle == handle) return;
    Dev d;
    d.handle = handle;
    d.info = info;
    d.seq = nextSeq++;
    devs.push_back(d);
}

void Remove(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mu);
    if (faking) return;
    devs.erase(std::remove_if(devs.begin(), devs.end(), [&](const Dev& d) { return d.handle == handle; }), devs.end());
}

void Feed(uint64_t handle, const uint8_t* report, size_t size) {
    std::lock_guard<std::mutex> lock(mu);
    if (faking) return;
    for (auto& d : devs) {
        if (d.handle != handle) continue;
        State next = d.st;
        if (!Parse(d.info.kind, d.info.link, report, size, next)) return;
        // A report without the battery keeps the last level that was seen.
        if (next.battery < 0) next.battery = d.st.battery, next.charging = d.st.charging;
        const State was = d.st;
        d.st = next;
        d.latch |= next.buttons & ~was.buttons;
        d.shown |= next.buttons & ~was.buttons;
        WatchChord(d, was);
        return;
    }
}

void SetXInput(int slot, bool present, uint16_t buttons, float lx, float ly) {
    if (slot < 0 || slot > 3) return;
    std::lock_guard<std::mutex> lock(mu);
    if (faking) return;
    Slot& s = slots[slot];
    if (present && !s.present) {
        s = Slot();
        s.seq = nextSeq++;
    }
    s.present = present;
    const uint16_t was = s.buttons;
    s.buttons = present ? buttons : 0;
    s.lx = present ? lx : 0, s.ly = present ? ly : 0;
    if (!present) s.rx = s.ry = s.lt = s.rt = 0;
    const uint16_t lr = 0x0100 | 0x0200; // both shoulder buttons
    if (present && !ShadowSlot(s) && (s.buttons & lr) == lr && (was & lr) != lr) Chord("xinput:" + std::to_string(slot));
}

void SetXInputMore(int slot, float rx, float ry, float lt, float rt) {
    if (slot < 0 || slot > 3) return;
    std::lock_guard<std::mutex> lock(mu);
    if (faking || !slots[slot].present) return;
    slots[slot].rx = rx, slots[slot].ry = ry, slots[slot].lt = lt, slots[slot].rt = rt;
}

namespace {

// Caller holds the lock. The pads of `all` with their player numbers, in the
// same order, then the keyboard.
std::vector<Pad> Numbered(const std::vector<Logical>& all) {
    std::vector<Pad> out;
    const auto keyAt = std::find(order.begin(), order.end(), "keyboard");
    Pad keyboard;
    keyboard.kind = Kind::Keyboard;
    keyboard.id = "keyboard";
    int player = 0;
    bool keyPlaced = false;
    // The keyboard takes its remembered place: before the first pad that
    // comes later in the order (or is not in it).
    for (const auto& l : all) {
        const auto at = std::find(order.begin(), order.end(), l.pad.id);
        if (keyAt != order.end() && !keyPlaced && at > keyAt) {
            keyboard.player = ++player <= 4 ? player : 0;
            keyPlaced = true;
        }
        Pad p = l.pad;
        p.player = ++player <= 4 ? player : 0;
        out.push_back(p);
    }
    if (!keyPlaced && (keyAt != order.end() || all.empty())) keyboard.player = ++player <= 4 ? player : 0;
    out.push_back(keyboard);
    return out;
}

} // namespace

std::vector<Pad> List() {
    std::lock_guard<std::mutex> lock(mu);
    return Numbered(Build(Take::None));
}

void SetOrder(const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lock(mu);
    order = ids;
    // What the order says about Joy-Con (alone or together) is now the rule.
    singles.clear();
    pairs.clear();
}

std::vector<std::string> Order() {
    std::lock_guard<std::mutex> lock(mu);
    return order;
}

std::string JoinOrder(const std::vector<std::string>& ids) {
    std::string out;
    for (const auto& id : ids) out += (out.empty() ? "" : "|") + id;
    return out;
}

std::vector<std::string> SplitOrder(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size() && !text.empty()) {
        const size_t bar = text.find('|', at);
        const std::string id = text.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
        if (!id.empty()) out.push_back(id);
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    return out;
}

void Listen(bool on) {
    std::lock_guard<std::mutex> lock(mu);
    listening = on;
    chords.clear();
    if (!on) {
        // Choices made on the order screen that were not confirmed are dropped.
        singles.clear();
        pairs.clear();
    }
}

bool TakeChord(std::string& id) {
    std::lock_guard<std::mutex> lock(mu);
    if (chords.empty()) return false;
    id = chords.front();
    chords.erase(chords.begin());
    return true;
}

bool HoldingShoulder() {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& d : devs)
        if (d.st.buttons & (kL | kR | kSL | kSR)) return true;
    for (const auto& s : slots)
        if (s.present && (s.buttons & 0x0300)) return true;
    return false;
}

uint32_t MenuButtons(const State& held, bool nintendo, bool swapAB) {
    const uint32_t b = held.buttons;
    uint32_t m = 0;
    const auto on = [&](uint32_t bit, uint32_t to) {
        if (b & bit) m |= to;
    };
    on(kUp, mUp), on(kDown, mDown), on(kLeft, mLeft), on(kRight, mRight);
    on(kPlus, mPlus), on(kMinus, mMinus), on(kL, mL), on(kR, mR), on(kHome, mHome), on(kCapture, mCapture);
    on(kZL, mZL), on(kZR, mZR), on(kLS, mLS), on(kRS, mRS);
    // A Nintendo pad's letters are where the console has them; on the others
    // the bottom button confirms unless the user asked for the console's way.
    if (nintendo || swapAB) on(kEast, mA), on(kSouth, mB), on(kNorth, mX), on(kWest, mY);
    else on(kSouth, mA), on(kEast, mB), on(kWest, mX), on(kNorth, mY);
    return m;
}

bool RemapValid(const int map[kActions]) {
    uint32_t seen = 0;
    for (int a = 0; a < kActions; a++) {
        if (map[a] < 0 || map[a] >= kMenuButtons || (seen & (1u << map[a]))) return false;
        seen |= 1u << map[a];
    }
    // What every pad has, a Joy-Con held sideways included.
    const uint32_t everywhere = mA | mB | mX | mY | mL | mR | mPlus;
    return ((1u << map[kConfirm]) & everywhere) && ((1u << map[kBack]) & everywhere);
}

bool RemapAssign(int map[kActions], int action, int button) {
    if (action < 0 || action >= kActions || button < 0 || button >= kMenuButtons) return false;
    int next[kActions];
    std::copy(map, map + kActions, next);
    for (int a = 0; a < kActions; a++)
        if (a != action && next[a] == button) next[a] = next[action];
    next[action] = button;
    if (!RemapValid(next)) return false;
    std::copy(next, next + kActions, map);
    return true;
}

void SetRemap(const int map[kActions]) {
    if (!RemapValid(map)) return;
    std::lock_guard<std::mutex> lock(mu);
    std::copy(map, map + kActions, remap);
}

void GetRemap(int map[kActions]) {
    std::lock_guard<std::mutex> lock(mu);
    std::copy(remap, remap + kActions, map);
}

std::string RemapText(const int map[kActions]) {
    std::string out;
    for (int a = 0; a < kActions; a++) out += (a ? "," : "") + std::to_string(map[a]);
    return out;
}

bool RemapParse(const std::string& text, int map[kActions]) {
    int next[kActions], n = 0;
    for (size_t at = 0; at < text.size() && n < kActions;) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        if (end == at || end - at > 2) return false;
        for (size_t i = at; i < end; i++)
            if (text[i] < '0' || text[i] > '9') return false;
        next[n++] = atoi(text.c_str() + at);
        at = end + 1;
    }
    if (n != kActions || !RemapValid(next)) return false;
    std::copy(next, next + kActions, map);
    return true;
}

Menu Poll(bool swapAB) {
    std::lock_guard<std::mutex> lock(mu);
    Menu m;
    // XInput's bit for each of the app's buttons. The app turns XInput's A into
    // its B when `swapAB` is on, so the button that confirms is sent as
    // whichever of the two comes out as A.
    const uint16_t xStart = 0x0010, xBack = 0x0020, xL = 0x0100, xR = 0x0200, xGuide = 0x0400, xA = 0x1000, xB = 0x2000,
                   xX = 0x4000, xY = 0x8000;
    uint32_t hid = 0, held = 0; // by where the buttons sit (pads read here) / under the menu's names (all pads)
    const auto take = [&](float lx, float ly) {
        if (fabsf(lx) > fabsf(m.lx)) m.lx = lx;
        if (fabsf(ly) > fabsf(m.ly)) m.ly = ly;
    };
    for (const auto& l : Build(Take::Menu)) {
        if (l.pad.link == Link::XInput) continue;
        hid |= l.st.buttons;
        held |= MenuButtons(l.st, l.nintendo, swapAB);
        take(l.st.lx, l.st.ly);
    }
    const double now = NowMs();
    const uint32_t pressed = hid & ~prevHid;
    prevHid = hid;
    if (pressed & ~static_cast<uint32_t>(kUp | kDown | kLeft | kRight)) lastHidPress = now;

    for (auto& s : slots) {
        if (!s.present) continue;
        // Directions are levels and the same from both sources; only presses
        // can count twice (or as the wrong letter, on a wrapped Nintendo pad).
        const uint16_t fresh = static_cast<uint16_t>(s.buttons & ~s.prev & ~0x000F);
        s.prev = s.buttons;
        if (fresh) {
            if (now - lastHidPress < 100.0) s.suppress |= fresh, s.echo++;
            else s.own++;
        }
        s.suppress &= s.buttons;
        State st;
        st.buttons = SlotButtons(static_cast<uint16_t>(s.buttons & ~s.suppress), s.lt, s.rt);
        held |= MenuButtons(st, false, swapAB);
        take(s.lx, s.ly);
    }

    // The one place where a pad's buttons become the menu's.
    uint32_t acts = 0;
    for (int a = 0; a < kActions; a++)
        if (held & (1u << remap[a])) acts |= 1u << a;
    if (now < hushUntil) {
        carry = acts;
        stickCarry = true;
        prevActs = acts;
        homeSince = -1;
        return Menu();
    }
    carry &= acts;
    acts &= ~carry;
    if (stickCarry) {
        if (fabsf(m.lx) < 0.4f && fabsf(m.ly) < 0.4f) stickCarry = false;
        else m.lx = m.ly = 0;
    }
    static const uint16_t plain[kActions] = {0x0001, 0x0002, 0x0004, 0x0008, 0, 0, 0, 0, xStart, xBack, xL, xR, xGuide, 0};
    for (int a = 0; a < kActions; a++)
        if (acts & (1u << a)) m.buttons |= plain[a];
    if (acts & mA) m.buttons |= swapAB ? xB : xA;
    if (acts & mB) m.buttons |= swapAB ? xA : xB;
    if (acts & mX) m.buttons |= swapAB ? xY : xX;
    if (acts & mY) m.buttons |= swapAB ? xX : xY;
    m.capture = (acts & ~prevActs & mCapture) != 0;
    prevActs = acts;
    if (!(acts & mHome)) homeSince = -1;
    else if (homeSince < 0) homeSince = now, homeTaken = false;
    return m;
}

std::vector<Live> Snapshot(bool swapAB) {
    std::lock_guard<std::mutex> lock(mu);
    const std::vector<Logical> all = Build(Take::Page);
    const std::vector<Pad> numbered = Numbered(all);
    std::vector<Live> out;
    for (size_t i = 0; i < all.size(); i++) {
        Live l;
        l.pad = numbered[i];
        l.st = all[i].st;
        l.menu = MenuButtons(l.st, all[i].nintendo, swapAB);
        out.push_back(l);
    }
    return out;
}

void Hush() {
    std::lock_guard<std::mutex> lock(mu);
    hushUntil = NowMs() + 250.0;
}

bool TakeHomeHold() {
    std::lock_guard<std::mutex> lock(mu);
    if (homeSince < 0 || homeTaken || NowMs() - homeSince < 1000.0) return false;
    homeTaken = true;
    return true;
}

void WiredChanged(); // pads_win.cpp

void SetProWired(bool on) {
    if (proWired.exchange(on) != on) WiredChanged();
}

bool ProWired() {
    return proWired;
}

void DebugFake(const std::string& spec) {
    std::lock_guard<std::mutex> lock(mu);
    faking = true;
    devs.clear();
    for (auto& s : slots) s = Slot();
    size_t at = 0;
    int n = 0;
    while (at < spec.size()) {
        size_t end = spec.find(' ', at);
        if (end == std::string::npos) end = spec.size();
        std::string word = spec.substr(at, end - at);
        at = end + 1;
        int battery = -1;
        const size_t colon = word.find(':');
        if (colon != std::string::npos) {
            battery = atoi(word.c_str() + colon + 1);
            word.resize(colon);
        }
        const auto add = [&](Kind kind, Link link, uint16_t vendor, uint16_t product, uint16_t version) {
            Dev d;
            d.handle = 1000 + n;
            d.info.kind = kind, d.info.link = link;
            d.info.vendor = vendor, d.info.product = product, d.info.version = version;
            d.info.id = "fake:" + std::to_string(n++);
            d.st.battery = battery;
            d.seq = nextSeq++;
            devs.push_back(d);
        };
        if (word == "ds5") add(Kind::DualSense, Link::Usb, kSony, 0x0CE6, 0x0100);
        else if (word == "ds5bt") add(Kind::DualSense, Link::Bluetooth, kSony, 0x0CE6, 0x0000);
        else if (word == "ds4") add(Kind::DualShock4, Link::Usb, kSony, 0x09CC, 0x0100);
        else if (word == "pro") add(Kind::SwitchPro, Link::Bluetooth, kNintendo, 0x2009, 0x0001);
        else if (word == "jcl" || word == "jc") add(Kind::JoyConL, Link::Bluetooth, kNintendo, 0x2006, 0x0001);
        else if (word == "jcr") add(Kind::JoyConR, Link::Bluetooth, kNintendo, 0x2007, 0x0001);
        else if (word == "xbox") {
            for (auto& s : slots)
                if (!s.present) {
                    s.present = true;
                    s.seq = nextSeq++;
                    break;
                }
        }
        if (word == "jc") add(Kind::JoyConR, Link::Bluetooth, kNintendo, 0x2007, 0x0001);
    }
}

bool DebugFaking() {
    std::lock_guard<std::mutex> lock(mu);
    return faking;
}

void DebugHold(int index, const State& held) {
    std::lock_guard<std::mutex> lock(mu);
    if (index >= 0 && index < static_cast<int>(devs.size())) {
        Dev& d = devs[index];
        State next = held;
        next.battery = d.st.battery, next.charging = d.st.charging;
        d.latch |= next.buttons & ~d.st.buttons;
        d.shown |= next.buttons & ~d.st.buttons;
        d.st = next;
        return;
    }
    index -= static_cast<int>(devs.size());
    for (auto& s : slots) {
        if (!s.present || index-- != 0) continue;
        static const struct {
            uint32_t from;
            uint16_t to;
        } map[] = {{kUp, 0x0001},    {kDown, 0x0002},  {kLeft, 0x0004}, {kRight, 0x0008}, {kPlus, 0x0010},
                   {kMinus, 0x0020}, {kLS, 0x0040},    {kRS, 0x0080},   {kL, 0x0100},     {kR, 0x0200},
                   {kHome, 0x0400},  {kSouth, 0x1000}, {kEast, 0x2000}, {kWest, 0x4000},  {kNorth, 0x8000}};
        s.buttons = 0;
        for (const auto& m : map)
            if (held.buttons & m.from) s.buttons |= m.to;
        s.lx = held.lx, s.ly = held.ly, s.rx = held.rx, s.ry = held.ry, s.lt = held.lt, s.rt = held.rt;
        return;
    }
}

void DebugChord(int index) {
    std::lock_guard<std::mutex> lock(mu);
    std::vector<Logical> all = Build(Take::None);
    // In the order they were made up, not in player order.
    std::stable_sort(all.begin(), all.end(), [](const Logical& a, const Logical& b) { return a.seq < b.seq; });
    if (index >= 0 && index < static_cast<int>(all.size())) Chord(all[index].pad.id);
}

#ifndef _WIN32
void Start() {}
void Stop() {}
void WiredChanged() {}
bool Reads() {
    return false;
}
std::vector<std::string> Probe() {
    return {};
}
#endif

} // namespace pads
