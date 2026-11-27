// See padcfg.h.
#include "padcfg.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace padcfg {

namespace {

// ---- files ------------------------------------------------------------------

bool ReadText(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// The first write keeps the file as the emulator left it; the new text goes
// to a temporary file that then replaces the original in one step.
bool WriteText(const std::wstring& path, const std::string& text) {
    const std::wstring backup = path + L".dockside-backup", tmp = path + L".dockside-tmp";
    CopyFileW(path.c_str(), backup.c_str(), TRUE); // fails when it exists already: that is the point
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// ---- who goes where ---------------------------------------------------------

struct Entry {
    int slot = 0; // player number now, 1..8
    Want::What what = Want::What::Unknown;
    uint8_t guid[16] = {};
    bool sdl3 = false;
    int index = 0;
};

int Hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// 32 hex digits -> 16 bytes.
bool HexBytes(const std::string& hex, uint8_t out[16]) {
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; i++) {
        const int a = Hex(hex[i * 2]), b = Hex(hex[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = static_cast<uint8_t>(a * 16 + b);
    }
    return true;
}

// The drivers SDL uses for Xbox-type pads on Windows sign their GUIDs with
// 'x' (XInput), 'r' (raw input) or 'w' (Windows.Gaming.Input).
bool XboxGuid(const uint8_t g[16]) {
    return g[14] == 'x' || g[14] == 'r' || g[14] == 'w';
}

bool Matches(const Want& w, const Entry& e) {
    if (w.what != e.what) return false;
    if (w.what == Want::What::Keyboard || w.what == Want::What::Xbox) return true;
    if (w.what != Want::What::Sdl) return false;
    if (memcmp(w.guid + 4, e.guid + 4, 12) != 0 || w.index != e.index) return false;
    // Bus: SDL 2 writes 3 whatever the link; SDL 3 writes 5 for Bluetooth.
    const int bus = e.guid[0] | (e.guid[1] << 8);
    if (e.sdl3 ? bus != (w.bluetooth ? 5 : 3) : !(bus == 3 || (bus == 5 && w.bluetooth))) return false;
    // Name CRC: cleared by the yuzu family and by Ryubing, kept by the first Ryujinx.
    const bool fileHasCrc = e.guid[2] || e.guid[3];
    return !fileHasCrc || !w.crcKnown || (e.guid[2] == w.guid[2] && e.guid[3] == w.guid[3]);
}

// target[i] = the player number entry i ends up with. False when the wanted
// order cannot be had (then nothing may be written).
bool Plan(const std::vector<Entry>& entries, std::vector<Want> wants, int slots, std::vector<int>& target,
          std::vector<int>& missing) {
    std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.player < b.player; });
    target.assign(entries.size(), 0);
    // XInput gives pads no identity: only one such pad and one such entry can
    // be told to be each other.
    int xboxEntries = 0, xboxWants = 0;
    for (const auto& e : entries) xboxEntries += e.what == Want::What::Xbox;
    for (const auto& w : wants) xboxWants += w.what == Want::What::Xbox;
    std::set<int> taken;
    for (const auto& w : wants) {
        if (w.player < 1 || w.player > slots || taken.count(w.player)) return false;
        int found = -1;
        if (w.what != Want::What::Xbox || (xboxEntries == 1 && xboxWants == 1)) {
            for (size_t i = 0; i < entries.size(); i++) {
                if (target[i] || !Matches(w, entries[i])) continue;
                // The one already in place first, so that nothing moves needlessly.
                if (found < 0 || entries[i].slot == w.player) found = static_cast<int>(i);
                if (entries[i].slot == w.player) break;
            }
        }
        if (found < 0) {
            missing.push_back(w.player);
            continue;
        }
        target[found] = w.player;
        taken.insert(w.player);
    }
    // The rest stay where they are when they can.
    for (size_t i = 0; i < entries.size(); i++)
        if (!target[i] && !taken.count(entries[i].slot)) {
            target[i] = entries[i].slot;
            taken.insert(entries[i].slot);
        }
    for (size_t i = 0; i < entries.size(); i++) {
        if (target[i]) continue;
        int free = 1;
        while (free <= slots && taken.count(free)) free++;
        if (free > slots) return false;
        target[i] = free;
        taken.insert(free);
    }
    return true;
}

// ---- Ryujinx: a little JSON reading -----------------------------------------------

size_t SkipSpace(const std::string& t, size_t i) {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n' || t[i] == ',')) i++;
    return i;
}

// `i` on the opening quote; returns the position after the closing one.
size_t SkipString(const std::string& t, size_t i) {
    for (i++; i < t.size() && t[i] != '"'; i++)
        if (t[i] == '\\') i++;
    return i < t.size() ? i + 1 : std::string::npos;
}

size_t SkipValue(const std::string& t, size_t i) {
    if (i >= t.size()) return std::string::npos;
    if (t[i] == '"') return SkipString(t, i);
    if (t[i] == '{' || t[i] == '[') {
        int depth = 0;
        while (i < t.size()) {
            if (t[i] == '"') {
                i = SkipString(t, i);
                if (i == std::string::npos) return i;
                continue;
            }
            if (t[i] == '{' || t[i] == '[') depth++;
            if (t[i] == '}' || t[i] == ']') {
                if (--depth == 0) return i + 1;
            }
            i++;
        }
        return std::string::npos;
    }
    while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']' && t[i] != ' ' && t[i] != '\r' && t[i] != '\n') i++;
    return i;
}

struct Member {
    std::string key;
    size_t from = 0, to = 0; // the value's text
};

// The members of the object that opens at `i`. False on anything unexpected.
bool Members(const std::string& t, size_t i, std::vector<Member>& out) {
    if (i >= t.size() || t[i] != '{') return false;
    for (i = SkipSpace(t, i + 1); i < t.size() && t[i] != '}'; i = SkipSpace(t, i)) {
        if (t[i] != '"') return false;
        const size_t keyEnd = SkipString(t, i);
        if (keyEnd == std::string::npos) return false;
        Member m;
        m.key = t.substr(i + 1, keyEnd - i - 2);
        i = SkipSpace(t, keyEnd);
        if (i >= t.size() || t[i] != ':') return false;
        m.from = SkipSpace(t, i + 1);
        m.to = SkipValue(t, m.from);
        if (m.to == std::string::npos) return false;
        out.push_back(m);
        i = m.to;
    }
    return i < t.size();
}

struct Span {
    size_t from = 0, to = 0; // a value's text; nothing when the file has no such value
    bool has() const { return to > from; }
};

struct RyuEntry {
    Entry e;
    bool movable = false;
    size_t from = 0, to = 0; // the "player_index" value, quotes included
    Span zoneL, zoneR, rumble; // "deadzone_left", "deadzone_right", "rumble"."enable_rumble"
};

// "0-56570003-054c-0000-e60c-000000016800": index, then a GUID as .NET prints
// one (the first three groups are numbers, so their bytes come out reversed).
bool ParseRyuId(const std::string& id, int& index, uint8_t g[16]) {
    const size_t dash = id.find('-');
    if (dash == std::string::npos || dash == 0 || id.size() != dash + 37) return false;
    for (size_t i = 0; i < dash; i++)
        if (id[i] < '0' || id[i] > '9') return false;
    index = atoi(id.c_str());
    std::string hex;
    for (size_t i = dash + 1; i < id.size(); i++)
        if (id[i] != '-') hex += id[i];
    uint8_t p[16];
    if (!HexBytes(hex, p)) return false;
    const int from[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; i++) g[i] = p[from[i]];
    return true;
}

bool ReadRyujinx(const std::string& text, std::vector<RyuEntry>& out) {
    std::vector<Member> top;
    if (!Members(text, text.find('{'), top)) return false;
    for (const auto& m : top) {
        if (m.key != "input_config") continue;
        if (text[m.from] != '[') return false;
        for (size_t i = SkipSpace(text, m.from + 1); i < m.to && text[i] != ']'; i = SkipSpace(text, i)) {
            std::vector<Member> fields;
            if (!Members(text, i, fields)) return false;
            RyuEntry r;
            std::string backend, id, player;
            for (const auto& f : fields) {
                if (f.key == "deadzone_left") r.zoneL = {f.from, f.to};
                else if (f.key == "deadzone_right") r.zoneR = {f.from, f.to};
                else if (f.key == "rumble" && text[f.from] == '{') {
                    std::vector<Member> inner;
                    if (!Members(text, f.from, inner)) return false;
                    for (const auto& in : inner)
                        if (in.key == "enable_rumble") r.rumble = {in.from, in.to};
                }
                if (text[f.from] != '"') continue;
                const std::string v = text.substr(f.from + 1, f.to - f.from - 2);
                if (f.key == "backend") backend = v;
                else if (f.key == "id") id = v;
                else if (f.key == "player_index") player = v, r.from = f.from, r.to = f.to;
            }
            if (player.size() == 7 && player.compare(0, 6, "Player") == 0 && player[6] >= '1' && player[6] <= '8') {
                r.movable = true;
                r.e.slot = player[6] - '0';
            }
            if (backend == "WindowKeyboard") {
                r.e.what = Want::What::Keyboard;
            } else if ((backend == "GamepadSDL2" || backend == "GamepadSDL3") && ParseRyuId(id, r.e.index, r.e.guid)) {
                r.e.what = XboxGuid(r.e.guid) ? Want::What::Xbox : Want::What::Sdl;
                r.e.sdl3 = backend == "GamepadSDL3";
            }
            out.push_back(r);
            i = SkipValue(text, i);
            if (i == std::string::npos) return false;
        }
        return true;
    }
    return false;
}

// ---- yuzu family: the [Controls] lines -------------------------------------------

struct Line {
    size_t from = 0, to = 0; // without the line ending
    int player = -1;         // N of "player_N_", -1 for other lines
    std::string name;        // after "player_N_", up to "\default" or "="
    bool isDefault = false;  // the "\default" companion
    std::string value;       // after "="
    size_t digits = 0, digitsLen = 0; // where N is written
    std::string key;         // everything before "=", for the lines that are not a player's
};

std::string Unquoted(const std::string& v) {
    return v.size() >= 2 && v.front() == '"' && v.back() == '"' ? v.substr(1, v.size() - 2) : v;
}

std::string Param(const std::string& binding, const char* key) {
    const std::string k = std::string(key) + ":";
    size_t at = 0;
    while (at < binding.size()) {
        size_t end = binding.find(',', at);
        if (end == std::string::npos) end = binding.size();
        if (binding.compare(at, k.size(), k) == 0) return binding.substr(at + k.size(), end - at - k.size());
        at = end + 1;
    }
    return {};
}

// What each player's entry of [Controls] is bound to.
struct Group {
    bool any = false, pad = false, mixed = false, keyboard = false, handheld = false;
    std::string guid, port;
    int connectedLine = -1, connectedDefault = -1;
};

struct YuzuFile {
    std::vector<Line> lines; // of [Controls]
    Group groups[8];
    std::vector<Entry> entries; // one per player that has lines, in player order
};

void ReadYuzu(const std::string& text, YuzuFile& y) {
    std::vector<Line>& lines = y.lines;
    Group* groups = y.groups;
    bool inside = false;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        const size_t next = end == std::string::npos ? text.size() : end + 1;
        if (end == std::string::npos) end = text.size();
        if (end > at && text[end - 1] == '\r') end--;
        const std::string s = text.substr(at, end - at);
        if (!s.empty() && s[0] == '[') {
            inside = s == "[Controls]";
        } else if (inside) {
            Line l;
            l.from = at, l.to = end;
            const size_t eq = s.find('=');
            if (eq != std::string::npos) l.key = s.substr(0, eq);
            if (eq != std::string::npos && s.compare(0, 7, "player_") == 0) {
                size_t d = 7;
                while (d < eq && s[d] >= '0' && s[d] <= '9') d++;
                if (d > 7 && d < eq && s[d] == '_') {
                    l.player = atoi(s.c_str() + 7);
                    l.digits = at + 7, l.digitsLen = d - 7;
                    l.name = s.substr(d + 1, eq - d - 1);
                    const std::string mark = "\\default";
                    if (l.name.size() > mark.size() && l.name.compare(l.name.size() - mark.size(), mark.size(), mark) == 0) {
                        l.isDefault = true;
                        l.name.resize(l.name.size() - mark.size());
                    }
                    l.value = s.substr(eq + 1);
                }
            }
            lines.push_back(l);
        }
        at = next;
    }

    // A value only counts when its "\default" line says false; otherwise the
    // emulator uses its built-in mapping, which is the keyboard's for every
    // player.
    const auto usesDefault = [&](const Line& l) {
        for (const auto& d : lines)
            if (d.isDefault && d.player == l.player && d.name == l.name) return d.value == "true";
        return true; // no companion line: the emulator ignores the value
    };
    for (size_t i = 0; i < lines.size(); i++) {
        const Line& l = lines[i];
        if (l.player < 0 || l.player > 7) continue;
        Group& g = groups[l.player];
        g.any = true;
        if (l.name == "connected") {
            (l.isDefault ? g.connectedDefault : g.connectedLine) = static_cast<int>(i);
            continue;
        }
        if (l.isDefault) continue;
        if (l.name == "type" && !usesDefault(l) && l.value == "4") g.handheld = true;
        const bool binding = l.name.compare(0, 7, "button_") == 0 || l.name == "lstick" || l.name == "rstick";
        if (!binding) continue;
        const std::string v = usesDefault(l) ? "engine:keyboard" : Unquoted(l.value);
        const std::string engine = Param(v, "engine");
        if (engine == "sdl") {
            const std::string guid = Param(v, "guid"), port = Param(v, "port");
            if (g.pad && (guid != g.guid || port != g.port)) g.mixed = true;
            g.pad = true, g.guid = guid, g.port = port;
        } else if (engine == "keyboard" || engine == "analog_from_button") {
            g.keyboard = true;
        } else if (!engine.empty()) {
            g.mixed = true; // some other driver (the emulator's own Joy-Con one, a mouse...)
        }
    }
    for (int n = 0; n < 8; n++) {
        const Group& g = groups[n];
        if (!g.any) continue;
        Entry e;
        e.slot = n + 1;
        if (g.pad && !g.mixed && HexBytes(g.guid, e.guid)) {
            e.what = XboxGuid(e.guid) ? Want::What::Xbox : Want::What::Sdl;
            e.index = atoi(g.port.c_str());
        } else if (g.keyboard && !g.pad && !g.mixed) {
            e.what = Want::What::Keyboard;
        }
        y.entries.push_back(e);
    }
}

} // namespace

std::vector<Want> WantsFrom(const std::vector<pads::Pad>& all) {
    std::vector<Want> out;
    for (const auto& p : all) {
        if (p.player < 1) continue;
        Want w;
        w.player = p.player;
        switch (p.kind) {
        case pads::Kind::Keyboard: w.what = Want::What::Keyboard; break;
        case pads::Kind::Xbox: w.what = Want::What::Xbox; break;
        case pads::Kind::DualSense:
        case pads::Kind::DualShock4:
        case pads::Kind::SwitchPro: {
            w.what = Want::What::Sdl;
            const char* name = pads::SdlName(p.kind, p.product);
            pads::SdlGuid(p.vendor, p.product, p.version, name, w.guid);
            w.crcKnown = name != nullptr;
            w.bluetooth = p.link == pads::Link::Bluetooth;
            w.index = p.sameIndex;
            break;
        }
        default: break; // Joy-Con: Unknown
        }
        out.push_back(w);
    }
    return out;
}

Result ApplyRyujinx(const std::wstring& path, const std::vector<Want>& wants, bool checkOnly) {
    Result res;
    std::string text;
    std::vector<RyuEntry> all;
    if (!ReadText(path, text) || !ReadRyujinx(text, all)) return res;
    std::vector<Entry> entries;
    std::vector<size_t> which;
    for (size_t i = 0; i < all.size(); i++)
        if (all[i].movable) entries.push_back(all[i].e), which.push_back(i);
    std::vector<int> target;
    if (!Plan(entries, wants, 8, target, res.missing)) return res;
    res.ok = true;
    // From the end of the file backwards, so earlier positions stay valid.
    std::string out = text;
    for (size_t k = entries.size(); k-- > 0;) {
        if (target[k] == entries[k].slot) continue;
        const RyuEntry& r = all[which[k]];
        out.replace(r.from, r.to - r.from, "\"Player" + std::to_string(target[k]) + "\"");
    }
    res.changed = out != text;
    if (res.changed && !checkOnly && !WriteText(path, out)) res.ok = res.changed = false;
    return res;
}

Result ApplyYuzu(const std::wstring& path, const std::vector<Want>& wants, bool checkOnly) {
    Result res;
    std::string text;
    if (!ReadText(path, text)) return res;
    YuzuFile y;
    ReadYuzu(text, y);
    const std::vector<Line>& lines = y.lines;
    const Group* groups = y.groups;
    const std::vector<Entry>& entries = y.entries;
    std::vector<int> target;
    if (entries.empty() || !Plan(entries, wants, 8, target, res.missing)) return res;
    // "Handheld" only exists for player 1: such an entry is never moved.
    for (size_t k = 0; k < entries.size(); k++)
        if (target[k] != entries[k].slot && groups[entries[k].slot - 1].handheld) return res;
    res.ok = true;

    int moveTo[8]; // player N's lines become player moveTo[N]'s
    bool connected[8], decided[8];
    for (int n = 0; n < 8; n++) moveTo[n] = n, connected[n] = false, decided[n] = true;
    for (size_t k = 0; k < entries.size(); k++) moveTo[entries[k].slot - 1] = target[k] - 1;
    for (const auto& w : wants)
        if (w.player >= 1 && w.player <= 8) connected[w.player - 1] = true;
    // A player whose pad the file does not know keeps whatever it says now.
    for (int p : res.missing) decided[p - 1] = false;

    // Rebuild the text line by line (from the end, positions stay valid).
    std::string out = text;
    for (size_t i = lines.size(); i-- > 0;) {
        const Line& l = lines[i];
        if (l.player < 0 || l.player > 7) continue;
        if (l.name == "connected") {
            const Group& g = groups[l.player];
            if (!decided[l.player] || g.connectedLine < 0 || g.connectedDefault < 0) continue;
            const bool now = lines[g.connectedDefault].value == "true" ? l.player == 0 : lines[g.connectedLine].value == "true";
            if (now == connected[l.player]) continue;
            const size_t eq = out.find('=', l.from);
            out.replace(eq + 1, l.to - eq - 1, l.isDefault ? "false" : connected[l.player] ? "true" : "false");
        } else if (moveTo[l.player] != l.player) {
            out.replace(l.digits, l.digitsLen, std::to_string(moveTo[l.player]));
        }
    }
    res.changed = out != text;
    if (res.changed && !checkOnly && !WriteText(path, out)) res.ok = res.changed = false;
    return res;
}

// ---- single values of the controller settings -------------------------------------

namespace {

std::string Cut(const std::string& text, const Span& s) {
    return text.substr(s.from, s.to - s.from);
}

bool IsBool(const std::string& v) {
    return v == "true" || v == "false";
}

bool IsNumber(const std::string& v) {
    if (v.empty()) return false;
    for (char c : v)
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) return false;
    return true;
}

struct Change {
    Span at;
    std::string text;
};

// Puts the new texts in place (whatever order they come in) and writes the
// file when that made a difference.
bool Rewrite(const std::wstring& path, const std::string& text, std::vector<Change> changes) {
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.at.from > b.at.from; });
    std::string out = text;
    for (const auto& c : changes) out.replace(c.at.from, c.at.to - c.at.from, c.text);
    return out == text || WriteText(path, out);
}

// A top-level value of Config.json.
bool RyuTop(const std::string& text, const char* key, Span& out) {
    std::vector<Member> top;
    if (!Members(text, text.find('{'), top)) return false;
    for (const auto& m : top)
        if (m.key == key) {
            out = {m.from, m.to};
            return true;
        }
    return false;
}

// One setting of [Controls]: where its value and its "\default" flag are written.
struct Pair {
    Span value, flag;
    bool has() const { return value.has() && flag.has(); }
};

Pair YuzuPair(const YuzuFile& y, const std::string& name) {
    Pair p;
    for (const auto& l : y.lines) {
        const size_t eq = l.key.size(); // the "=" follows the key
        if (l.key == name) p.value = {l.from + eq + 1, l.to};
        else if (l.key == name + "\\default") p.flag = {l.from + eq + 1, l.to};
    }
    return p;
}

// What the emulator will use: the value, unless its flag says "default".
bool YuzuEffective(const std::string& text, const YuzuFile& y, const std::string& name, const char* builtin, std::string& out) {
    const Pair p = YuzuPair(y, name);
    if (!p.has()) return false;
    out = Cut(text, p.flag) == "true" ? builtin : Cut(text, p.value);
    return true;
}

// The pair as the emulator itself writes it (Eden, frontend_common/config.cpp,
// WriteSettingGeneric): the flag says whether the value is the built-in one.
// Nothing is added when the file has no such lines.
bool YuzuPut(const YuzuFile& y, const std::string& name, const std::string& value, const char* builtin, std::vector<Change>& changes) {
    const Pair p = YuzuPair(y, name);
    if (!p.has()) return false;
    changes.push_back({p.value, value});
    changes.push_back({p.flag, value == builtin ? "true" : "false"});
    return true;
}

// Which of the file's entries is this pad's; -1 when none (or when it cannot
// be told: XInput gives pads no identity, so only a lone such entry counts).
int EntryOf(const std::vector<Entry>& entries, const Want& w) {
    int found = -1, same = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        if (!Matches(w, entries[i])) continue;
        same++;
        if (found < 0 || entries[i].slot == w.player) found = static_cast<int>(i);
    }
    return w.what == Want::What::Xbox && same != 1 ? -1 : found;
}

// The number after "deadzone:" inside a stick's binding.
Span ZoneParam(const std::string& text, const Line& l) {
    const std::string mark = "deadzone:";
    for (size_t at = text.find(mark, l.from); at != std::string::npos && at < l.to; at = text.find(mark, at + 1)) {
        if (text[at - 1] != ',' && text[at - 1] != '"') continue;
        size_t end = at + mark.size();
        while (end < l.to && text[end] != ',' && text[end] != '"') end++;
        return {at + mark.size(), end};
    }
    return {};
}

// The two sticks of the entry `pad` has in a yuzu-family file.
bool YuzuZones(const std::string& text, const YuzuFile& y, const Want& pad, Span& left, Span& right) {
    const int at = EntryOf(y.entries, pad);
    if (at < 0 || y.entries[at].what == Want::What::Keyboard) return false;
    const int player = y.entries[at].slot - 1;
    for (const auto& l : y.lines) {
        if (l.player != player || l.isDefault || (l.name != "lstick" && l.name != "rstick")) continue;
        bool own = false; // the binding is in use (its flag says so) and is the pad's
        for (const auto& d : y.lines)
            if (d.isDefault && d.player == player && d.name == l.name) own = d.value == "false";
        if (!own || Param(Unquoted(l.value), "engine") != "sdl") continue;
        (l.name == "lstick" ? left : right) = ZoneParam(text, l);
    }
    return true;
}

std::string Shortest(double v) {
    // As the emulator's own writer prints such numbers: 0.1, 0.15, 0.
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

} // namespace

Rumble RumbleGet(const std::wstring& file, Family family) {
    Rumble r;
    std::string text;
    if (!ReadText(file, text)) return r;
    if (family == Family::Ryujinx) {
        std::vector<RyuEntry> all;
        if (!ReadRyujinx(text, all)) return r;
        r.ok = true;
        for (const auto& e : all) {
            if (!e.rumble.has() || !IsBool(Cut(text, e.rumble))) continue;
            r.entries++;
            r.on += Cut(text, e.rumble) == "true";
        }
    } else {
        YuzuFile y;
        ReadYuzu(text, y);
        std::string v;
        if (!YuzuEffective(text, y, "vibration_enabled", "true", v)) return r;
        r.ok = true;
        r.entries = 1;
        r.on = v == "true";
    }
    return r;
}

bool RumbleSet(const std::wstring& file, Family family, bool on) {
    std::string text;
    if (!ReadText(file, text)) return false;
    std::vector<Change> changes;
    if (family == Family::Ryujinx) {
        std::vector<RyuEntry> all;
        if (!ReadRyujinx(text, all)) return false;
        for (const auto& e : all)
            if (e.rumble.has() && IsBool(Cut(text, e.rumble))) changes.push_back({e.rumble, on ? "true" : "false"});
        if (changes.empty()) return false;
    } else {
        YuzuFile y;
        ReadYuzu(text, y);
        std::string v;
        if (!YuzuEffective(text, y, "vibration_enabled", "true", v)) return false;
        if ((v == "true") == on) return true;
        YuzuPut(y, "vibration_enabled", on ? "true" : "false", "true", changes);
    }
    return Rewrite(file, text, changes);
}

int SwitchGet(const std::wstring& file, Family family, Switch which) {
    std::string text, v;
    if (!ReadText(file, text)) return -1;
    if (family == Family::Ryujinx) {
        Span s;
        if (!RyuTop(text, which == Switch::Keyboard ? "enable_keyboard" : "enable_mouse", s)) return -1;
        v = Cut(text, s);
    } else {
        YuzuFile y;
        ReadYuzu(text, y);
        if (!YuzuEffective(text, y, which == Switch::Keyboard ? "keyboard_enabled" : "mouse_enabled", "false", v)) return -1;
    }
    return IsBool(v) ? v == "true" : -1;
}

bool SwitchSet(const std::wstring& file, Family family, Switch which, bool on) {
    const int now = SwitchGet(file, family, which);
    if (now < 0) return false;
    if ((now == 1) == on) return true;
    std::string text;
    if (!ReadText(file, text)) return false;
    std::vector<Change> changes;
    if (family == Family::Ryujinx) {
        Span s;
        if (!RyuTop(text, which == Switch::Keyboard ? "enable_keyboard" : "enable_mouse", s)) return false;
        changes.push_back({s, on ? "true" : "false"});
    } else {
        YuzuFile y;
        ReadYuzu(text, y);
        if (!YuzuPut(y, which == Switch::Keyboard ? "keyboard_enabled" : "mouse_enabled", on ? "true" : "false", "false", changes))
            return false;
    }
    return Rewrite(file, text, changes);
}

int MouseSpeedGet(const std::wstring& file, Family family) {
    std::string text, v;
    if (family != Family::Yuzu || !ReadText(file, text)) return -1;
    YuzuFile y;
    ReadYuzu(text, y);
    if (!YuzuEffective(text, y, "mouse_panning_x_sensitivity", "50", v) || !IsNumber(v)) return -1;
    return atoi(v.c_str());
}

bool MouseSpeedSet(const std::wstring& file, Family family, int value) {
    const int now = MouseSpeedGet(file, family);
    if (now < 0 || value < 1 || value > 100) return false;
    std::string text;
    if (!ReadText(file, text)) return false;
    YuzuFile y;
    ReadYuzu(text, y);
    std::vector<Change> changes;
    const std::string v = std::to_string(value);
    // Across and up-down are two numbers there; the console has one.
    if (!YuzuPut(y, "mouse_panning_x_sensitivity", v, "50", changes)) return false;
    YuzuPut(y, "mouse_panning_y_sensitivity", v, "50", changes);
    return Rewrite(file, text, changes);
}

Zones ZonesGet(const std::wstring& file, Family family, const Want& pad) {
    Zones z;
    std::string text;
    if (!ReadText(file, text)) return z;
    Span left, right;
    if (family == Family::Ryujinx) {
        std::vector<RyuEntry> all;
        if (!ReadRyujinx(text, all)) return z;
        std::vector<Entry> entries;
        for (const auto& e : all) entries.push_back(e.e);
        const int at = EntryOf(entries, pad);
        if (at < 0 || entries[at].what == Want::What::Keyboard) return z;
        left = all[at].zoneL, right = all[at].zoneR;
    } else {
        YuzuFile y;
        ReadYuzu(text, y);
        if (!YuzuZones(text, y, pad, left, right)) return z;
    }
    z.found = true;
    if (left.has() && IsNumber(Cut(text, left))) z.left = atof(Cut(text, left).c_str());
    if (right.has() && IsNumber(Cut(text, right))) z.right = atof(Cut(text, right).c_str());
    return z;
}

bool ZoneSet(const std::wstring& file, Family family, const Want& pad, bool rightStick, double value) {
    if (value < 0 || value > 0.9) return false;
    std::string text;
    if (!ReadText(file, text)) return false;
    Span left, right;
    char number[32];
    if (family == Family::Ryujinx) {
        std::vector<RyuEntry> all;
        if (!ReadRyujinx(text, all)) return false;
        std::vector<Entry> entries;
        for (const auto& e : all) entries.push_back(e.e);
        const int at = EntryOf(entries, pad);
        if (at < 0) return false;
        left = all[at].zoneL, right = all[at].zoneR;
        snprintf(number, sizeof number, "%s", Shortest(value).c_str());
    } else {
        YuzuFile y;
        ReadYuzu(text, y);
        if (!YuzuZones(text, y, pad, left, right)) return false;
        snprintf(number, sizeof number, "%.6f", value); // as Qt's front end prints a float parameter
    }
    const Span at = rightStick ? right : left;
    if (!at.has() || !IsNumber(Cut(text, at))) return false;
    return Rewrite(file, text, {{at, number}});
}

} // namespace padcfg
