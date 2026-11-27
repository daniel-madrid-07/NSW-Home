// See netplay.h.
#include "netplay.h"
#include "fr_bencode.h"
#include "fr_crypto.h"
#include "friends.h"
#include "ryucfg.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

namespace netplay {

const char* const kFamilyRyuLdn = "ryuldn";

namespace {

constexpr size_t kNonceBytes = 16;
constexpr size_t kMaxText = 60; // bytes of a game's or an emulator's name

// Cuts UTF-8 text to at most n bytes without splitting a character.
std::string Clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

// A test run of the menu gives its own clock (frames, not the wall clock).
double testClock = -1;
double Mono() {
    if (testClock >= 0) return testClock;
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

// ---- the format ---------------------------------------------------------------

std::string Encode(const Msg& m) {
    ben::Val d = ben::Val::MakeDict();
    d.Set("v", 1).Set("k", m.kind).Set("n", m.nonce).Set("t", m.time);
    if (m.kind == kInvite) {
        d.Set("x", m.expires).Set("g", frc::Unhex(m.titleId)).Set("m", Clip(m.game, kMaxText)).Set("gv", Clip(m.gameVersion, 24));
        d.Set("f", m.family).Set("e", Clip(m.emulator, 16)).Set("ev", Clip(m.emuVersion, 24)).Set("p", m.passphrase);
    } else if (m.kind == kAnswer) {
        d.Set("a", m.yes ? 1 : 0).Set("e", Clip(m.emulator, 16)).Set("ev", Clip(m.emuVersion, 24)).Set("gv", Clip(m.gameVersion, 24));
    }
    return ben::Encode(d);
}

bool Decode(const std::string& body, Msg& m) {
    ben::Val d;
    if (!ben::Decode(body, d) || d.type != ben::Val::Dict || d.Int_("v") != 1) return false;
    Msg out;
    out.kind = static_cast<int>(d.Int_("k"));
    out.nonce = d.Str_("n");
    out.time = d.Int_("t");
    if (out.nonce.size() != kNonceBytes || out.time <= 0) return false;
    if (out.kind == kInvite) {
        out.expires = d.Int_("x");
        const std::string g = d.Str_("g");
        out.titleId = g.size() == 8 ? frc::Hex(g) : std::string();
        out.game = Clip(d.Str_("m"), kMaxText);
        out.gameVersion = Clip(d.Str_("gv"), 24);
        out.family = d.Str_("f");
        out.emulator = Clip(d.Str_("e"), 16);
        out.emuVersion = Clip(d.Str_("ev"), 24);
        out.passphrase = d.Str_("p");
        if (out.titleId.empty() || out.family.empty() || !ValidPassphrase(out.passphrase)) return false;
        // A lifetime of its own between a second and an hour; ours are ten minutes.
        if (out.expires <= out.time || out.expires - out.time > 3600) return false;
    } else if (out.kind == kAnswer) {
        if (!d.Has("a", ben::Val::Int)) return false;
        out.yes = d.Int_("a") == 1;
        out.emulator = Clip(d.Str_("e"), 16);
        out.emuVersion = Clip(d.Str_("ev"), 24);
        out.gameVersion = Clip(d.Str_("gv"), 24);
    } else if (out.kind != kCancel) {
        return false;
    }
    m = out;
    return true;
}

std::string NewNonce() {
    return frc::RandomBytes(kNonceBytes);
}

std::string NewPassphrase() {
    // (As Ryubing's own "generate" button does: Ryujinx- and a random 32-bit
    // number in lowercase hex, SettingsNetworkView.axaml.cs.)
    return "Ryujinx-" + frc::Hex(frc::RandomBytes(4));
}

bool ValidPassphrase(const std::string& p) {
    if (p.size() != 16 || p.compare(0, 8, "Ryujinx-") != 0) return false;
    for (size_t i = 8; i < 16; i++)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return true;
}

bool Fresh(const Msg& inv, int64_t receivedAt, int64_t now) {
    if (inv.kind != kInvite) return false;
    if (inv.time > now + kSkew) return false;         // from the future, beyond clock trouble
    if (now > inv.expires + kSkew) return false;      // gone by the writer's clock
    return now - receivedAt <= inv.expires - inv.time; // gone by ours
}

int64_t SecondsLeft(const Msg& inv, int64_t receivedAt, int64_t now) {
    if (!Fresh(inv, receivedAt, now)) return 0;
    const int64_t byOurs = receivedAt + (inv.expires - inv.time) - now;
    const int64_t byTheirs = inv.expires + kSkew - now;
    return (std::max)(int64_t{0}, (std::min)(byOurs, byTheirs));
}

// ---- netplay.dat ------------------------------------------------------------------

namespace {

struct State {
    bool inited = false, offline = false;
    std::string dir;
    // Nonces of invitations dealt with (answered, refused, given up), until when.
    std::map<std::string, int64_t> handled;
    // What Apply replaced, waiting to be put back.
    bool restore = false;
    std::wstring restorePath;
    std::string restoreEmu;
    std::vector<std::pair<std::string, std::string>> restoreValues;
    // The session.
    Session session;
    std::vector<Incoming> testIncoming;
    int testAnswer = 1;
    double testDelay = 4, testAt = 0, testWrite = 1.5;
} st;

std::string Utf8(const std::wstring& w) {
    std::string o;
    for (size_t i = 0; i < w.size(); i++) {
        uint32_t c = static_cast<uint32_t>(w[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(w[++i]) - 0xDC00);
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) o += static_cast<char>(0xC0 | (c >> 6)), o += static_cast<char>(0x80 | (c & 63));
        else if (c < 0x10000)
            o += static_cast<char>(0xE0 | (c >> 12)), o += static_cast<char>(0x80 | ((c >> 6) & 63)), o += static_cast<char>(0x80 | (c & 63));
        else
            o += static_cast<char>(0xF0 | (c >> 18)), o += static_cast<char>(0x80 | ((c >> 12) & 63)),
                o += static_cast<char>(0x80 | ((c >> 6) & 63)), o += static_cast<char>(0x80 | (c & 63));
    }
    return o;
}

std::wstring Wide(const std::string& u8) {
    std::wstring out;
    for (size_t i = 0; i < u8.size();) {
        unsigned char c = static_cast<unsigned char>(u8[i]);
        uint32_t cp = c;
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n > 1) cp = c & (0xFF >> (n + 1));
        for (size_t k = 1; k < n && i + k < u8.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(u8[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000 && sizeof(wchar_t) == 2) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<wchar_t>(cp);
        }
    }
    return out;
}

FILE* OpenFile(const std::string& path, bool write) {
#ifdef _WIN32
    FILE* f = nullptr;
    return _wfopen_s(&f, Wide(path).c_str(), write ? L"wb" : L"rb") == 0 ? f : nullptr;
#else
    return fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

bool MoveOver(const std::string& from, const std::string& to) {
#ifdef _WIN32
    if (_wrename(Wide(from).c_str(), Wide(to).c_str()) == 0) return true;
    _wremove(Wide(to).c_str());
    return _wrename(Wide(from).c_str(), Wide(to).c_str()) == 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

std::string StorePath() {
#ifdef _WIN32
    return st.dir + "\\netplay.dat";
#else
    return st.dir + "/netplay.dat";
#endif
}

void Save() {
    if (st.offline || st.dir.empty()) return;
    ben::Val root = ben::Val::MakeDict();
    root.Set("v", 1);
    ben::Val h = ben::Val::MakeList();
    for (const auto& kv : st.handled) {
        ben::Val e = ben::Val::MakeList();
        e.list.push_back(kv.first);
        e.list.push_back(kv.second);
        h.list.push_back(std::move(e));
    }
    root.Set("h", std::move(h));
    if (st.restore) {
        ben::Val r = ben::Val::MakeDict();
        r.Set("p", Utf8(st.restorePath)).Set("e", st.restoreEmu);
        ben::Val k = ben::Val::MakeList();
        for (const auto& kv : st.restoreValues) {
            ben::Val e = ben::Val::MakeList();
            e.list.push_back(kv.first);
            e.list.push_back(kv.second);
            k.list.push_back(std::move(e));
        }
        r.Set("k", std::move(k));
        root.Set("r", std::move(r));
    }
    const std::string text = ben::Encode(root), path = StorePath(), tmp = path + ".tmp";
    FILE* f = OpenFile(tmp, true);
    if (!f) return;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (ok) MoveOver(tmp, path);
}

void Load() {
    FILE* f = OpenFile(StorePath(), false);
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    ben::Val root;
    if (!ben::Decode(text, root) || root.type != ben::Val::Dict) return;
    if (const ben::Val* h = root.Get("h"))
        if (h->type == ben::Val::List)
            for (const auto& e : h->list)
                if (e.type == ben::Val::List && e.list.size() == 2 && e.list[0].type == ben::Val::Str && e.list[1].type == ben::Val::Int)
                    st.handled[e.list[0].s] = e.list[1].i;
    if (const ben::Val* r = root.Get("r"))
        if (r->type == ben::Val::Dict) {
            st.restorePath = Wide(r->Str_("p"));
            st.restoreEmu = r->Str_("e");
            if (const ben::Val* k = r->Get("k"))
                if (k->type == ben::Val::List)
                    for (const auto& e : k->list)
                        if (e.type == ben::Val::List && e.list.size() == 2 && e.list[0].type == ben::Val::Str && e.list[1].type == ben::Val::Str)
                            st.restoreValues.emplace_back(e.list[0].s, e.list[1].s);
            st.restore = !st.restorePath.empty() && !st.restoreValues.empty();
        }
}

void Handled(const std::string& nonce, int64_t until) {
    const int64_t now = friends::UnixNow();
    for (auto it = st.handled.begin(); it != st.handled.end();)
        it = it->second < now ? st.handled.erase(it) : std::next(it);
    st.handled[nonce] = until + kSkew;
    Save();
}

void SetPhase(Phase p) {
    st.session.phase = p;
    st.session.since = friends::UnixNow();
}

bool Send(const std::string& owner, const std::string& code, const Msg& m) {
    if (st.offline) return true;
    return friends::SendMessage(owner, code, Encode(m));
}

Msg Reply(int kind, const Msg& to) {
    Msg m;
    m.kind = kind;
    m.nonce = to.nonce;
    m.time = friends::UnixNow();
    return m;
}

} // namespace

// ---- emulators ---------------------------------------------------------------------

// The value of `multiplayer_mode` that selects LdnRyu. Both forks number the
// modes Disabled, LdnRyu, LdnMitm (src/Ryujinx.Common/Configuration/
// Multiplayer/MultiplayerMode.cs in Ryubing and in Kenji-NX), stored as a
// number (the enum has no string converter, unlike the others of the file).
// Ryujinx 1.1.1403 has Disabled, LdnMitm only: there 1 is ldn_mitm, which
// only reaches consoles on the same network, so it gets none.
const char* LdnModeValue(const std::string& emuId) {
    if (emuId == "ryubing" || emuId == "kenjinx") return "1";
    return nullptr;
}

namespace {
// The four keys and what a session writes (ConfigurationFileFormat.cs of
// both forks, names in the file's snake_case):
//   multiplayer_mode            LdnRyu
//   multiplayer_ldn_passphrase  the session's: the server shows a game only
//                               to players with the same passphrase
//   ldn_server                  "" = the default, ldn.ryujinx.app
//   multiplayer_disable_p2p     true: this PC never hosts the game's traffic
//                               itself (no port opened through the router,
//                               nothing listening), everything goes through
//                               the server (LdnMasterProxyClient.cs)
const char* const kKeys[] = {"multiplayer_mode", "multiplayer_ldn_passphrase", "ldn_server", "multiplayer_disable_p2p"};

bool Numeric(const std::string& v) {
    return !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
}
} // namespace

Support CanPlay(const std::string& emuId, const std::wstring& emuLabel, const std::wstring& configPath) {
    Support s;
    if (emuId.empty()) {
        s.why = L"No hay ningún emulador configurado para este juego.";
        return s;
    }
    if (emuId == "ryujinx") {
        s.why = L"Ryujinx no está disponible para jugar en línea: su juego en red\nsolo llega a consolas de la misma red local. Usa Ryubing o Kenji-NX.";
        return s;
    }
    if (!LdnModeValue(emuId)) {
        s.why = emuLabel + L" no está disponible para jugar en línea desde NSW-Home.\nSolo Ryubing y Kenji-NX pueden por ahora.";
        return s;
    }
    if (configPath.empty() || !ryucfg::Open(configPath)) {
        s.why = L"No se puede leer la configuración de " + emuLabel + L".\nInicia el emulador una vez y vuelve a intentarlo.";
        return s;
    }
    // A build from before the fork had its online play writes none of these.
    const std::string mode = ryucfg::Get("multiplayer_mode"), p2p = ryucfg::Get("multiplayer_disable_p2p");
    if (!ryucfg::Has("multiplayer_ldn_passphrase") || !ryucfg::Has("ldn_server") || !Numeric(mode) || (p2p != "true" && p2p != "false")) {
        s.why = L"Esta versión de " + emuLabel + L" no tiene juego en línea.\nActualiza el emulador.";
        return s;
    }
    s.ok = true;
    s.family = kFamilyRyuLdn;
    return s;
}

bool RestorePending() {
    return st.restore;
}

bool Restore(std::string* error) {
    if (!st.restore) return true;
    if (!ryucfg::Open(st.restorePath)) {
        if (error) *error = "CONFIG"; // kept for the next time (the file may be in use)
        return false;
    }
    bool ok = ryucfg::SetMany(st.restoreValues);
    if (!ok) {
        // One value refused must not hold the others back.
        ok = true;
        for (const auto& kv : st.restoreValues)
            if (!ryucfg::Set(kv.first, kv.second)) {
                ok = false;
                if (error) *error += (error->empty() ? "" : ",") + kv.first;
            }
    }
    st.restore = false;
    st.restoreValues.clear();
    st.restorePath.clear();
    Save();
    return ok;
}

bool Apply(const std::wstring& configPath, const std::string& emuId, const std::string& passphrase, std::string& error) {
    const char* mode = LdnModeValue(emuId);
    if (!mode || !ValidPassphrase(passphrase)) {
        error = "UNSUPPORTED";
        return false;
    }
    if (st.restore && !Restore(&error)) {
        if (error == "CONFIG") return false;
    }
    if (!ryucfg::Open(configPath)) {
        error = "CONFIG";
        return false;
    }
    std::vector<std::pair<std::string, std::string>> before;
    for (const char* k : kKeys) {
        if (!ryucfg::Has(k)) {
            error = "KEYS";
            return false;
        }
        before.emplace_back(k, ryucfg::Get(k));
    }
    // Kept on disk first: whatever happens next, it can be put back.
    st.restore = true;
    st.restorePath = configPath;
    st.restoreEmu = emuId;
    st.restoreValues = before;
    Save();
    if (!ryucfg::SetMany({{"multiplayer_mode", mode}, {"multiplayer_ldn_passphrase", passphrase}, {"ldn_server", ""}, {"multiplayer_disable_p2p", "true"}})) {
        error = "WRITE";
        st.restore = false;
        st.restoreValues.clear();
        Save();
        return false;
    }
    return true;
}

// ---- the session ------------------------------------------------------------------

void Init(const std::string& dataDir, bool offline) {
    if (st.inited) return;
    st.inited = true;
    st.offline = offline;
    st.dir = dataDir;
    if (!offline) Load();
}

bool Invite(const std::string& owner, const std::string& code, Msg inv) {
    Cancel();
    inv.kind = kInvite;
    inv.nonce = NewNonce();
    inv.time = friends::UnixNow();
    inv.expires = inv.time + kLifetime;
    inv.passphrase = NewPassphrase();
    if (inv.family.empty()) inv.family = kFamilyRyuLdn;
    if (!Send(owner, code, inv)) return false;
    st.session = Session();
    st.session.host = true;
    st.session.owner = owner;
    st.session.code = code;
    st.session.inv = inv;
    st.session.sendState = "writing";
    SetPhase(Phase::Inviting);
    st.testAt = Mono();
    if (!st.offline) friends::Expect(owner, code, true);
    return true;
}

void Cancel() {
    Session& s = st.session;
    if (s.phase == Phase::Inviting || s.phase == Phase::Joining) {
        // The friend's list drops it (host), or the host hears that the guest left.
        Send(s.owner, s.code, Reply(kCancel, s.inv));
    }
    if (!st.offline && !s.owner.empty()) friends::Expect(s.owner, s.code, false);
    st.session = Session();
}

bool Answer(const std::string& owner, const std::string& code, bool join, const Msg& mine) {
    Incoming found;
    bool have = false;
    for (const Incoming& in : Invitations(owner))
        if (in.code == code) found = in, have = true;
    if (!have) return false;
    Msg a = Reply(kAnswer, found.inv);
    a.yes = join;
    a.emulator = mine.emulator;
    a.emuVersion = mine.emuVersion;
    a.gameVersion = mine.gameVersion;
    if (join) Cancel(); // one session at a time: an invitation of our own is withdrawn
    Handled(found.inv.nonce, found.inv.expires);
    st.testIncoming.erase(std::remove_if(st.testIncoming.begin(), st.testIncoming.end(), [&](const Incoming& x) { return x.code == code; }),
                          st.testIncoming.end());
    if (!Send(owner, code, a)) return false;
    if (!join) return true;
    st.session = Session();
    st.session.host = false;
    st.session.owner = owner;
    st.session.code = code;
    st.session.inv = found.inv;
    SetPhase(Phase::Joining);
    st.testAt = Mono();
    if (!st.offline) friends::Expect(owner, code, true); // a withdrawal that crossed the answer
    return true;
}

std::vector<Incoming> Invitations(const std::string& owner) {
    std::vector<Incoming> out;
    const int64_t now = friends::UnixNow();
    auto take = [&](const Incoming& in) {
        if (!Fresh(in.inv, in.at, now) || st.handled.count(in.inv.nonce)) return;
        // The friend we are already playing with or answering is the session, not a list entry.
        if (st.session.phase != Phase::Idle && !st.session.host && st.session.code == in.code && st.session.inv.nonce == in.inv.nonce) return;
        out.push_back(in);
    };
    if (st.offline) {
        for (const Incoming& in : st.testIncoming) take(in); // (a test run has one user's worth)
    } else {
        for (const friends::Message& m : friends::Messages(owner)) {
            Incoming in;
            in.code = m.code;
            in.at = m.at;
            if (Decode(m.body, in.inv) && in.inv.kind == kInvite) take(in);
        }
    }
    std::sort(out.begin(), out.end(), [](const Incoming& a, const Incoming& b) { return a.inv.time > b.inv.time; });
    return out;
}

const Session& Current() {
    return st.session;
}

bool Poll() {
    Session& s = st.session;
    const int64_t now = friends::UnixNow();
    const Phase was = s.phase;
    const std::string sendWas = s.sendState;
    if (s.phase != Phase::Inviting && s.phase != Phase::Joining) return false;

    // What the friend last wrote to us: the answer (host) or a withdrawal (guest).
    Msg theirs;
    bool got = false;
    if (st.offline) {
        if (st.testAnswer >= 0 && Mono() - st.testAt >= st.testDelay && s.host) {
            theirs = Reply(kAnswer, s.inv);
            theirs.yes = st.testAnswer == 1;
            got = true;
        }
    } else {
        for (const friends::Message& m : friends::Messages(s.owner))
            if (m.code == s.code && Decode(m.body, theirs) && theirs.nonce == s.inv.nonce) got = true;
    }
    // (An answer written long after the invitation had gone is not one.)
    if (got && theirs.time > s.inv.expires + kSkew) got = false;

    const friends::MsgState sent = st.offline ? (Mono() - st.testAt >= st.testWrite ? friends::MsgState::Stored : friends::MsgState::Writing)
                                              : friends::MessageState(s.owner, s.code);
    if (s.host) {
        s.sendState = sent == friends::MsgState::Stored ? "stored" : "writing";
        if (got && theirs.kind == kAnswer) {
            SetPhase(theirs.yes ? Phase::Ready : Phase::Declined);
        } else if (got && theirs.kind == kCancel) {
            SetPhase(Phase::Declined); // accepted, then left before we saw it
        } else if (sent == friends::MsgState::Failed) {
            SetPhase(Phase::Unsent);
        } else if (now > s.inv.expires) {
            Send(s.owner, s.code, Reply(kCancel, s.inv)); // so that it leaves the friend's list too
            SetPhase(Phase::NoAnswer);
        }
    } else {
        if (got && theirs.kind == kCancel) SetPhase(Phase::Withdrawn);
        else if (sent == friends::MsgState::Stored) SetPhase(Phase::Ready);
        else if (sent == friends::MsgState::Failed || now > s.inv.expires + kSkew) SetPhase(Phase::Unsent);
    }
    if (s.phase != Phase::Inviting && s.phase != Phase::Joining) {
        if (!st.offline) friends::Expect(s.owner, s.code, false);
        Handled(s.inv.nonce, s.inv.expires);
    }
    return s.phase != was || s.sendState != sendWas;
}

void Launched() {
    if (st.session.phase == Phase::Ready) SetPhase(Phase::Playing);
}

void Ended() {
    if (st.session.phase == Phase::Playing || st.session.phase == Phase::Ready) st.session = Session();
}

void Dismiss() {
    const Phase p = st.session.phase;
    if (p == Phase::Declined || p == Phase::NoAnswer || p == Phase::Unsent || p == Phase::Withdrawn) st.session = Session();
}

void TestAnswer(int answer, double seconds, double writeSeconds) {
    st.testAnswer = answer;
    st.testDelay = seconds;
    st.testWrite = writeSeconds;
}

void TestTime(double seconds) {
    testClock = seconds;
}

void TestAddIncoming(const std::string& owner, const Incoming& in) {
    (void)owner;
    st.testIncoming.push_back(in);
}

void TestSetSession(const Session& s) {
    st.session = s;
    st.testAt = Mono();
}

size_t TestHandledCount() {
    return st.handled.size();
}

} // namespace netplay
