// The friends pages of the user page: "Lista de amigos", a friend's profile,
// "Buscar por clave de amigo" with its number pad and the request it sends,
// "Solicitudes de amistad recibidas" / "enviadas", and the switch that turns
// sharing on and off. Included by ui.cpp inside its namespace (the pages are
// drawn with that file's helpers and the user page's own rows); what goes
// over the network is in friends.h.
//
// None of this was captured on the console with friends or requests in it
// (the owner's list was not recorded, his request lists were empty, and the
// search pages need a connection to Nintendo), so the layout here is
// NSW-Home's, built from the page's own parts: the rows and headings of
// "Ajustes de usuario", the friend list's cards, the disc and name of the
// picture chooser, the box cursor, the dialogs.

enum class FSub { None, Pad, Found, Profile, Received, Sent, Asker, Asked };
FSub fSub = FSub::None;
std::wstring fPad;             // the digits typed so far
int fPadSel = 0;               // the key under the cursor: 1-9, then delete, 0, accept
friends::Card fFound;          // whose card the search found
std::string fProfile;          // the code of the friend whose profile is open
int fSavedRow = 0;             // the page's focus, put back when the profile closes
bool fSavedIn = false;
float fSavedScroll = 0;
double fLookAt = -1;           // when the "searching" box came up
double fRefreshAt = -10;       // when "Actualizar" was last pressed
bool fInited = false;
bool fListValid = false;
unsigned fListVersion = 0;
std::string fListOwner;
std::vector<friends::Friend> fList;
// The request lists: the card under the cursor, the scroll, the request open.
int fReqSel = 0;
float fReqScroll = 0;
Glide fReqGlide;
std::string fReqCode;
int fReqBtn = 0;               // on a received request's card: 0 Aceptar, 1 Rechazar

// The friend code is made from the user's identity (friends.h): whatever
// code the user was given before is replaced by it, once.
void FriendsSyncCodes() {
    if (userTest) return; // a --shot run keeps the codes of the data folder's config
    bool changed = false;
    for (User& user : cfg.users) {
        const std::string code = friends::OwnCode(user.id);
        if (friends::ValidCode(code) && code != user.friendCode) {
            user.friendCode = code;
            changed = true;
        }
    }
    if (changed) SaveConfig();
}

void FriendsEnsure() {
    if (fInited) return;
    fInited = true;
    // A --shot / --shotseq run stays off the network and off the disk.
    friends::Init(Narrow(DataDir()), userTest);
    FriendsSyncCodes();
}

std::wstring FriendCodeText(const std::string& code) {
    const std::wstring d = Widen(code);
    return d.size() != 12 ? std::wstring(L"—") : L"NH-" + d.substr(0, 4) + L"-" + d.substr(4, 4) + L"-" + d.substr(8);
}

// Text that came from someone else's status: one line, no control characters.
std::wstring FriendText(const std::string& utf8, size_t maxChars) {
    std::wstring w = Widen(utf8), out;
    for (wchar_t c : w)
        if (c >= 32 && c != 0x7F && out.size() < maxChars) out += c;
    lang::Ignore(out); // their words, not the menu's: never translated or reported
    return out;
}
std::wstring NickText(const std::string& nick) {
    std::wstring n = FriendText(nick, 20);
    return n.empty() ? std::wstring(L"Usuario") : n;
}
std::wstring FriendNick(const friends::Friend& f) {
    return NickText(f.st.nick); // from their request until a status is read
}

// A user's friends, the ones who are there first, then by when they last were.
const std::vector<friends::Friend>& FriendsOf(int u) {
    FriendsEnsure();
    const std::string& owner = cfg.users[u].id;
    unsigned v = friends::Version();
    if (!fListValid || v != fListVersion || owner != fListOwner) {
        fList = friends::List(owner);
        const int64_t t = friends::UnixNow();
        std::stable_sort(fList.begin(), fList.end(), [t](const friends::Friend& a, const friends::Friend& b) {
            bool ao = friends::Online(a, t), bo = friends::Online(b, t);
            if (ao != bo) return ao;
            if (ao) return a.st.state > b.st.state; // playing before in the menu
            return a.seen > b.seen;
        });
        fListValid = true;
        fListVersion = v;
        fListOwner = owner;
    }
    return fList;
}

bool FriendsSharing(int u) {
    FriendsEnsure();
    return friends::Sharing(cfg.users[u].id);
}

// With sharing off nothing is read any more, so nobody is shown as present
// on the strength of what was last known.
int FriendsOnline(int u) {
    if (!FriendsSharing(u)) return 0;
    int n = 0;
    const int64_t t = friends::UnixNow();
    for (const auto& f : FriendsOf(u)) n += friends::Online(f, t) ? 1 : 0;
    return n;
}

void FriendsNoteDisc(int u, int colour) {
    FriendsEnsure();
    friends::SetColour(cfg.users[u].id, colour);
}

// The request lists, newest first.
std::vector<friends::Request> FriendsRequests(int u, bool received) {
    FriendsEnsure();
    if (u < 0 || u >= static_cast<int>(cfg.users.size())) return {};
    std::vector<friends::Request> list = received ? friends::Received(cfg.users[u].id) : friends::Outgoing(cfg.users[u].id);
    std::stable_sort(list.begin(), list.end(), [](const friends::Request& a, const friends::Request& b) { return a.at > b.at; });
    return list;
}
int FriendsWaiting(int u) {
    return static_cast<int>(FriendsRequests(u, true).size());
}
// The value of the two request rows: how many there are (nothing for none).
std::wstring FriendsRequestsValue(int u, bool received) {
    size_t n = FriendsRequests(u, received).size();
    return n ? std::to_wstring(n) : std::wstring();
}
// The grey line under "Añadir un amigo" while requests wait, as "En línea: 2" under the list.
std::wstring FriendsRequestsSub(int u) {
    int n = FriendsWaiting(u);
    return n ? L"Solicitudes: " + std::to_wstring(n) : std::wstring();
}

// HOME: a user with requests waiting gets a small dot at the top right of
// the picture (where the console puts its own mark), in the accent colour.
void FriendsMarkUser(int u, float cx, float cy, float r) {
    if (u < 0 || u >= static_cast<int>(cfg.users.size()) || FriendsWaiting(u) == 0) return;
    Theme T = Th();
    const float d = r * 0.72f, dot = (std::max)(5.0f, r * 0.2f);
    Circle(cx + d, cy - d, dot + 2.5f, T.bg);
    Circle(cx + d, cy - d, dot, T.accent);
}

// "Hace 23 horas", as the console's own list words it.
std::wstring FriendAgo(int64_t s) {
    if (s < 60) return L"Hace un momento";
    int64_t m = s / 60, h = m / 60, d = h / 24;
    if (m < 60) return L"Hace " + std::to_wstring(m) + (m == 1 ? L" minuto" : L" minutos");
    if (h < 24) return L"Hace " + std::to_wstring(h) + (h == 1 ? L" hora" : L" horas");
    return L"Hace " + std::to_wstring(d) + (d == 1 ? L" día" : L" días");
}

struct FLine {
    std::wstring a, b;
    bool online = false;
};
FLine FriendLine(const friends::Friend& f, bool reading = true) {
    const int64_t t = friends::UnixNow();
    if (!f.known) return {L"Desconectado", L"", false};
    if (reading && friends::Online(f, t)) {
        if (f.st.state == friends::Playing) return {L"Jugando a", FriendText(f.st.game, 60), true};
        return {L"En el menú", L"", true};
    }
    return {L"Desconectado", FriendAgo((std::max)(int64_t{0}, t - f.seen)), false};
}
std::wstring FriendLineJoined(const friends::Friend& f) {
    FLine l = FriendLine(f);
    if (l.b.empty()) return l.a;
    return l.online ? l.a + L" " + l.b : l.a + L" · " + l.b;
}

// No picture travels: someone is the disc of the colour they chose with their
// initial, or one picked by their code when they use a picture.
void DrawPersonDisc(const std::string& code, const std::string& nick, int colour, float cx, float cy, float r) {
    int c = colour;
    if (c < 0) {
        c = 0;
        for (char ch : code) c = c * 7 + (ch - '0');
    }
    DrawDisc(cx, cy, r, rgb(AvatarColour((std::max)(0, c) % AvatarColours())), InitialOf(NickText(nick)));
}
void DrawFriendDisc(const friends::Friend& f, float cx, float cy, float r) {
    DrawPersonDisc(f.code, f.st.nick, f.st.colour, cx, cy, r);
}

const friends::Friend* FriendByCode(int u, const std::string& code) {
    for (const auto& f : FriendsOf(u))
        if (f.code == code) return &f;
    return nullptr;
}

int UserSectionIndex(const char* id) {
    const std::vector<USection> secs = UserSections();
    for (int i = 0; i < static_cast<int>(secs.size()); i++)
        if (secs[i].id == id) return i;
    return -1;
}

void FriendsCloseSub() {
    if (fSub == FSub::Profile) {
        userRow = fSavedRow;
        userInRows = fSavedIn;
        userScroll = fSavedScroll;
        userGlide = Glide();
    }
    fSub = FSub::None;
}

// ---- consent, the switch ----------------------------------------------------

// Said once, before anything of the user's leaves the PC; `then` runs if they agree.
void FriendsConsent(int u, std::function<void()> then) {
    if (FriendsSharing(u)) {
        then();
        return;
    }
    Ask(L"Quienes tengan tu clave de amigo podrán ver tu apodo\ny el color de tu icono, y enviarte solicitudes de amistad.\n\n"
        L"Solo los amigos que aceptes verán a qué juegas,\ntu tiempo de juego y si estás conectado.\n\n"
        L"Se comparte a través de una red pública entre pares,\nque puede ver tu dirección IP.",
        {L"Ahora no", L"Aceptar"}, [u, then](int b) {
            if (b != 1 || u >= static_cast<int>(cfg.users.size())) return;
            friends::SetSharing(cfg.users[u].id, true);
            then();
        });
    dlg.sel = 0; // agreeing to send something off the PC is never the button a stray press lands on
}

void FriendsToggleSharing(int u) {
    if (!FriendsSharing(u)) {
        FriendsConsent(u, [] {});
        return;
    }
    Ask(L"¿Quieres dejar de compartir tu actividad?\n\nTus amigos te verán sin conexión, nadie podrá encontrarte\n"
        L"con tu clave ni enviarte solicitudes, y el estado de tus\namigos dejará de actualizarse.",
        {L"Cancelar", L"Desactivar"}, [u](int b) {
            if (b == 1 && u < static_cast<int>(cfg.users.size())) friends::SetSharing(cfg.users[u].id, false);
        });
}

// ---- searching by code ------------------------------------------------------------

void FriendsSearch(int u) {
    FriendsConsent(u, [] {
        fPad.clear();
        fPadSel = 0;
        fSub = FSub::Pad;
    });
}

void FriendLookup() {
    const int u = pageUser;
    const std::string code = Narrow(fPad);
    if (!friends::ValidCode(code)) return;
    for (const auto& user : cfg.users)
        if (user.friendCode == code) {
            Info(L"Esa clave de amigo es de un usuario de esta consola.");
            return;
        }
    if (const friends::Friend* f = FriendByCode(u, code)) {
        Info(FriendNick(*f) + L" ya está en tu lista de amigos.");
        return;
    }
    friends::StartLookup(code);
    fLookAt = now;
    Ask(L"Buscando...", {L"Cancelar"}, [](int) {
        friends::CancelLookup();
        fLookAt = -1;
    });
    dlg.busy = true;
}

// The search's answer, once the box has been up for a second (an answer that
// came at once would otherwise flash it).
void FriendLookupPoll() {
    if (fLookAt < 0) return;
    Touch();
    if (!dlg.open || !dlg.busy || stack.back() != Scr::User) {
        // The box was closed from outside (HOME): nobody is waiting any more.
        friends::CancelLookup();
        fLookAt = -1;
        return;
    }
    if (now - fLookAt < 1.0) return;
    friends::Card c;
    friends::Look l = friends::LookupState(&c);
    if (l == friends::Look::Busy) return;
    fLookAt = -1;
    dlg.open = false;
    friends::CancelLookup();
    if (l == friends::Look::Found) {
        fFound = c;
        fSub = FSub::Found;
        PlaySfx(Sfx::Ok);
    } else if (l == friends::Look::NoNetwork) {
        Info(L"No se ha podido establecer la conexión.\n\nComprueba la conexión a internet e inténtalo de nuevo.");
    } else if (l == friends::Look::Unverified) {
        Info(L"No se ha podido comprobar quién tiene esta clave de amigo.\n\n"
             L"Puede que otra persona esté escribiendo en su lugar.\nInténtalo de nuevo más tarde.");
    } else {
        Info(L"No se ha encontrado ningún usuario con esa clave de amigo.\n\n"
             L"Comprueba la clave. Tu amigo debe usar NSW-Home\ny haber aceptado compartir su actividad.");
    }
}

void FriendPadKey(int key) {
    if (key == 9) {
        if (!fPad.empty()) fPad.pop_back();
    } else if (key == 11) {
        if (fPad.size() == 12) FriendLookup();
        else PlaySfx(Sfx::Bump);
    } else if (fPad.size() < 12) {
        fPad += static_cast<wchar_t>(key == 10 ? L'0' : L'1' + key);
    } else {
        PlaySfx(Sfx::Bump);
    }
}

// The keyboard types into the pad directly.
bool FriendPadOpen() {
    return fSub == FSub::Pad && uSub == USub::None && !stack.empty() && stack.back() == Scr::User && !dlg.open;
}
bool FriendPadChar(wchar_t c) {
    if (!FriendPadOpen()) return false;
    if (c >= L'0' && c <= L'9') {
        if (fPad.size() < 12) fPad += c;
    } else if (c == L'\b') {
        if (!fPad.empty()) fPad.pop_back();
    } else if (c == L'\r') {
        PlaySfx(Sfx::Ok);
        FriendPadKey(11);
    } else if (c == 27) {
        PlaySfx(Sfx::Back);
        fSub = FSub::None;
    }
    return true;
}

constexpr float kFPadKeyW = 150, kFPadKeyH = 58, kFPadGap = 10, kFPadX = (1280 - 3 * kFPadKeyW - 2 * kFPadGap) / 2, kFPadY = 312;

void DrawFriendPad() {
    Theme T = Th();
    UserHeader(pageUser, L"Buscar por clave de amigo");
    Text(L"Introduce la clave de amigo de 12 cifras.", 0, 150, W, 36, 21.6f, T.text, Center);
    // The field: every character in a place of its own, so that the code
    // does not shift as it is typed.
    const float fw = 520, fh = 70, fx = (W - fw) / 2, fy = 208, cell = 22;
    RRect(fx, fy, fw, fh, 8, T.focusBg);
    std::wstring shown = L"NH-";
    for (size_t i = 0; i < 12; i++) {
        if (i && i % 4 == 0) shown += L'-';
        shown += i < fPad.size() ? fPad[i] : L'_';
    }
    float tx = W / 2 - cell * static_cast<float>(shown.size()) / 2;
    for (size_t i = 0; i < shown.size(); i++) {
        bool typed = shown[i] != L'_';
        Text(std::wstring(1, shown[i]), tx + cell * static_cast<float>(i), fy, cell, fh, 31, typed ? T.text : T.sub, Center);
    }
    static const wchar_t* const labels[12] = {L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"Borrar", L"0", L"Aceptar"};
    for (int i = 0; i < 12; i++) {
        float x = kFPadX + (i % 3) * (kFPadKeyW + kFPadGap), y = kFPadY + (i / 3) * (kFPadKeyH + kFPadGap);
        bool word = i == 9 || i == 11, dim = (i == 11 && fPad.size() < 12) || (i == 9 && fPad.empty());
        RRect(x, y, kFPadKeyW, kFPadKeyH, 6, S2() ? T.focusBg : T.btn);
        Text(labels[i], x, y, kFPadKeyW, kFPadKeyH, word ? 20.0f : 27.0f, dim ? T.sub : T.text, Center);
        if (i == fPadSel) {
            CursorFill(S2() ? T.focusBg : T.btn);
            Cursor(x, y, kFPadKeyW, kFPadKeyH, 6);
        }
        AddHit(x, y, kFPadKeyW, kFPadKeyH, [i] {
            fPadSel = i;
            PlaySfx(Sfx::Ok);
            FriendPadKey(i);
        });
    }
    Footer({{L"B", fPad.empty() ? L"Atrás" : L"Borrar", Btn::B}, {L"+", L"Aceptar", Btn::Plus}, {L"A", L"Confirmar", Btn::A}});
    Touch();
}

void FriendPadButton(Btn b) {
    int col = fPadSel % 3, row = fPadSel / 3, to = fPadSel;
    if (b == Btn::Left && col > 0) to--;
    else if (b == Btn::Right && col < 2) to++;
    else if (b == Btn::Up && row > 0) to -= 3;
    else if (b == Btn::Down && row < 3) to += 3;
    if (to != fPadSel) {
        fPadSel = to;
        PlaySfx(Sfx::Move);
    } else if (b == Btn::Left || b == Btn::Right || b == Btn::Up || b == Btn::Down) {
        PlaySfx(Sfx::Bump);
        BumpCursor(b == Btn::Left ? -1.0f : b == Btn::Right ? 1.0f : 0.0f, b == Btn::Up ? -1.0f : b == Btn::Down ? 1.0f : 0.0f);
    } else if (b == Btn::A) {
        PlaySfx(Sfx::Ok);
        FriendPadKey(fPadSel);
    } else if (b == Btn::Plus) {
        PlaySfx(Sfx::Ok);
        FriendPadKey(11);
    } else if (b == Btn::B) {
        PlaySfx(Sfx::Back);
        if (fPad.empty()) fSub = FSub::None;
        else fPad.pop_back();
    }
}

// A person's card in the middle of the page: whom a request is about.
void DrawPersonCard(const friends::Card& c, const std::wstring& line) {
    Theme T = Th();
    Circle(W / 2, 236, 86, T.avRing);
    DrawPersonDisc(c.code, c.nick, c.colour, W / 2, 236, 80);
    Text(NickText(c.nick), 0, 338, W, 46, 32, T.text, Center);
    Text(FriendCodeText(c.code), 0, 388, W, 30, 19.5f, T.sub, Center);
    if (!line.empty()) Text(line, 140, 418, W - 280, 30, kUNote, T.sub, Center);
}

// Buttons in a row under the card; `sel` is the focused one.
void DrawCardButtons(const std::vector<std::wstring>& labels, int sel, float w) {
    Theme T = Th();
    const float gap = 20, h = 62, ry = 476;
    const float total = static_cast<float>(labels.size()) * w + static_cast<float>(labels.size() - 1) * gap;
    for (size_t i = 0; i < labels.size(); i++) {
        const float x = (W - total) / 2 + static_cast<float>(i) * (w + gap);
        RRect(x, ry, w, h, 8, S2() ? T.focusBg : T.btn);
        Text(labels[i], x, ry, w, h, 22.5f, T.text, Center);
        if (static_cast<int>(i) == sel) {
            CursorFill(S2() ? T.focusBg : T.btn);
            Cursor(x, ry, w, h, 8);
        }
        const int k = static_cast<int>(i);
        AddHit(x, ry, w, h, [k] {
            fReqBtn = k;
            OnButtonImpl(Btn::A);
        });
    }
}

// Who the code belongs to: what anyone with the code may see (nickname and
// disc), and the request.
void DrawFriendFound() {
    Theme T = Th();
    UserHeader(pageUser, L"Añadir un amigo");
    DrawPersonCard(fFound, L"");
    DrawCardButtons({L"Enviar solicitud de amistad"}, 0, 420);
    Text(L"La solicitud puede tardar un par de minutos en llegar.", 0, 556, W, 30, kUNote, T.sub, Center);
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}});
}

void FriendsGoSection(const char* id) {
    int i = UserSectionIndex(id);
    if (i >= 0) {
        UserGoSection(i);
        userShown = i;
    }
}

void FriendFoundButton(Btn b) {
    if (b == Btn::A) {
        PlaySfx(Sfx::Ok);
        const std::wstring nick = NickText(fFound.nick);
        friends::Sent r = pageUser < static_cast<int>(cfg.users.size()) ? friends::SendRequest(cfg.users[pageUser].id, fFound)
                                                                         : friends::Sent::Failed;
        fSub = FSub::None;
        switch (r) {
        case friends::Sent::Sent:
            Info(L"Se ha enviado una solicitud de amistad a " + nick + L".\n\n"
                 L"Puede tardar un par de minutos en llegar. Cuando la acepte,\naparecerá en tu lista de amigos.");
            break;
        case friends::Sent::Already: Info(L"Ya has enviado una solicitud de amistad a " + nick + L"."); break;
        case friends::Sent::Friends: Info(nick + L" ya está en tu lista de amigos."); break;
        case friends::Sent::Accepted:
            // They had asked first: sending is accepting.
            FriendsGoSection("friends");
            Info(nick + L" ya te había enviado una solicitud de amistad.\n\nAhora " + nick + L" y tú sois amigos.");
            break;
        case friends::Sent::Failed: Info(L"No se ha podido enviar la solicitud de amistad."); break;
        }
    } else if (b == Btn::B) {
        PlaySfx(Sfx::Back);
        fSub = FSub::Pad;
    }
}

// ---- the request lists ------------------------------------------------------------

constexpr int kRCols = 5;
constexpr float kRCardW = 200, kRCardH = 186, kRTop = 104, kRBottom = 586;

void FriendsOpenRequests(int u, bool received) {
    pageUser = u;
    fSub = received ? FSub::Received : FSub::Sent;
    fReqSel = 0;
    fReqScroll = 0;
    fReqGlide = Glide();
}
void FriendsOpenReceived(int u) {
    FriendsOpenRequests(u, true);
}
void FriendsOpenSent(int u) {
    FriendsOpenRequests(u, false);
}

const friends::Request* FriendRequestByCode(const std::vector<friends::Request>& list, const std::string& code) {
    for (const auto& r : list)
        if (r.who.code == code) return &r;
    return nullptr;
}

void DrawRequestList(bool received) {
    Theme T = Th();
    UserHeader(pageUser, received ? L"Solicitudes de amistad recibidas" : L"Solicitudes de amistad enviadas");
    const auto list = FriendsRequests(pageUser, received);
    const int n = static_cast<int>(list.size());
    const int64_t t = friends::UnixNow();
    std::vector<std::wstring> notes;
    const bool sharing = FriendsSharing(pageUser);
    if (received) {
        const friends::Trouble tr = friends::Troubles(cfg.users[pageUser].id);
        if (!sharing) notes.push_back(L"Activa «Compartir mi actividad» en «Añadir un amigo» para recibir solicitudes.");
        else notes.push_back(L"Una solicitud puede tardar un par de minutos en llegar.");
        if (tr.inbox)
            notes.push_back(L"Alguien que tiene tu clave de amigo está ocupando el sitio donde llegan tus solicitudes: alguna puede tardar más.");
        if (tr.card) notes.push_back(L"Alguien que tiene tu clave de amigo ha escrito sobre tu ficha pública. NSW-Home la vuelve a escribir.");
    } else {
        notes.push_back(L"Cuando acepten una solicitud, aparecerá en tu lista de amigos. Hasta entonces sigue aquí.");
    }
    if (n == 0) {
        Text(received ? L"No hay ninguna solicitud de amistad." : L"No has enviado ninguna solicitud de amistad.", 0, 300, W, 40, 19.9f,
             T.sub, Center);
    } else {
        fReqSel = (std::max)(0, (std::min)(fReqSel, n - 1));
        const float view = kRBottom - kRTop;
        float target = fReqGlide.t0 < 0 ? fReqScroll : fReqGlide.to;
        const float top = static_cast<float>(fReqSel / kRCols) * kRCardH;
        if (top < target) target = top;
        if (top + kRCardH > target + view) target = top + kRCardH - view;
        GlideTo(fReqGlide, fReqScroll, target);
        const float x0 = (W - kRCols * kRCardW) / 2;
        PushClip(0, kRTop - 6, W, view + 12);
        for (int i = 0; i < n; i++) {
            const friends::Request& r = list[i];
            const float x = x0 + static_cast<float>(i % kRCols) * kRCardW, y = kRTop + static_cast<float>(i / kRCols) * kRCardH - fReqScroll;
            if (y > kRBottom || y + kRCardH < kRTop - 6) continue;
            const bool focused = i == fReqSel;
            const float fx = x + 6, fy = y, fw = kRCardW - 12, fh = kRCardH - 10;
            if (focused) RRect(fx, fy, fw, fh, 10, T.focusBg);
            DrawPersonDisc(r.who.code, r.who.nick, r.who.colour, x + kRCardW / 2, y + 56, 40);
            Text(NickText(r.who.nick), x + 10, y + 104, kRCardW - 20, 30, 19.5f, T.text, Center);
            Text(FriendAgo((std::max)(int64_t{0}, t - r.at)), x + 10, y + 134, kRCardW - 20, 24, 15.6f, T.sub, Center);
            if (focused) {
                CursorFill(T.focusBg);
                Cursor(fx, fy, fw, fh, 10);
            }
            AddHit(fx, fy, fw, fh, [i] {
                bool was = fReqSel == i;
                fReqSel = i;
                if (was) OnButtonImpl(Btn::A);
                else PlaySfx(Sfx::Move);
            });
        }
        PopClip();
    }
    float ny = 600 - 26.0f * static_cast<float>(notes.size() - 1);
    for (const auto& line : notes) {
        Text(line, 60, ny, W - 120, 26, 16.6f, T.sub, Center);
        ny += 26;
    }
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}});
}

void RequestListButton(Btn b, bool received) {
    const auto list = FriendsRequests(pageUser, received);
    const int n = static_cast<int>(list.size());
    if (b == Btn::B) {
        PlaySfx(Sfx::Back);
        fSub = FSub::None;
        return;
    }
    if (n == 0) {
        if (b == Btn::A) PlaySfx(Sfx::Bump);
        return;
    }
    fReqSel = (std::max)(0, (std::min)(fReqSel, n - 1));
    const int col = fReqSel % kRCols;
    int to = fReqSel;
    switch (b) {
    case Btn::Left:
        if (col > 0) to--;
        break;
    case Btn::Right:
        if (col < kRCols - 1 && fReqSel + 1 < n) to++;
        break;
    case Btn::Up:
        if (fReqSel >= kRCols) to -= kRCols;
        break;
    case Btn::Down:
        if (fReqSel + kRCols < n) to += kRCols;
        else if (fReqSel / kRCols < (n - 1) / kRCols) to = n - 1;
        break;
    case Btn::A:
        PlaySfx(Sfx::Ok);
        fReqCode = list[fReqSel].who.code;
        fReqBtn = 0;
        fSub = received ? FSub::Asker : FSub::Asked;
        return;
    default: return;
    }
    if (to != fReqSel) {
        fReqSel = to;
        PlaySfx(Sfx::Move);
    } else {
        PlaySfx(Sfx::Bump);
        BumpCursor(b == Btn::Left ? -1.0f : b == Btn::Right ? 1.0f : 0.0f, b == Btn::Up ? -1.0f : b == Btn::Down ? 1.0f : 0.0f);
    }
}

// One request, opened from its list.
void DrawRequestCard(bool received) {
    Theme T = Th();
    const auto list = FriendsRequests(pageUser, received);
    const friends::Request* r = FriendRequestByCode(list, fReqCode);
    if (!r) {
        // Withdrawn (or answered elsewhere) while open: back to the list.
        fSub = received ? FSub::Received : FSub::Sent;
        return;
    }
    UserHeader(pageUser, received ? L"Solicitud de amistad" : L"Solicitud de amistad enviada");
    std::wstring ago = FriendAgo((std::max)(int64_t{0}, friends::UnixNow() - r->at));
    ago[0] = static_cast<wchar_t>(towlower(ago[0]));
    DrawPersonCard(r->who, (received ? L"Recibida " : L"Enviada ") + ago);
    if (received) {
        DrawCardButtons({L"Aceptar", L"Rechazar"}, fReqBtn, 260);
        Text(L"Si la rechazas, no se le avisará.", 0, 556, W, 30, kUNote, T.sub, Center);
    } else {
        DrawCardButtons({L"Cancelar la solicitud"}, 0, 420);
        Text(L"Pendiente de respuesta. Puede que aún no le haya llegado: tarda un par de minutos.", 0, 556, W, 30, kUNote, T.sub, Center);
    }
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}});
}

void RequestCardButton(Btn b, bool received) {
    const auto list = FriendsRequests(pageUser, received);
    const friends::Request* r = FriendRequestByCode(list, fReqCode);
    if (b == Btn::B || !r) {
        PlaySfx(Sfx::Back);
        fSub = received ? FSub::Received : FSub::Sent;
        return;
    }
    const std::wstring nick = NickText(r->who.nick);
    const std::string code = r->who.code;
    const int u = pageUser;
    if (received && (b == Btn::Left || b == Btn::Right)) {
        int to = b == Btn::Left ? 0 : 1;
        if (to != fReqBtn) {
            fReqBtn = to;
            PlaySfx(Sfx::Move);
        } else {
            PlaySfx(Sfx::Bump);
            BumpCursor(b == Btn::Left ? -1.0f : 1.0f, 0);
        }
        return;
    }
    if (b != Btn::A) return;
    PlaySfx(Sfx::Ok);
    if (received && fReqBtn == 0) {
        if (u < static_cast<int>(cfg.users.size())) friends::Accept(cfg.users[u].id, code);
        fSub = FSub::Received;
        Info(L"Ahora " + nick + L" y tú sois amigos.");
    } else if (received) {
        Ask(L"¿Quieres rechazar la solicitud de amistad de " + nick + L"?\n\nNo se le avisará: para " + nick + L" seguirá pendiente.",
            {L"Cancelar", L"Rechazar"}, [u, code](int k) {
                if (k != 1 || u >= static_cast<int>(cfg.users.size())) return;
                friends::Decline(cfg.users[u].id, code);
                fSub = FSub::Received;
            });
    } else {
        Ask(L"¿Quieres cancelar la solicitud de amistad\nenviada a " + nick + L"?", {L"Volver", L"Cancelar la solicitud"}, [u, code](int k) {
            if (k != 1 || u >= static_cast<int>(cfg.users.size())) return;
            friends::Withdraw(cfg.users[u].id, code);
            fSub = FSub::Sent;
        });
    }
}

// ---- the list -------------------------------------------------------------------

constexpr int kFCols = 4;        // four cards a row, as the console's list
constexpr float kFCardH = 190;

void FriendOpenProfile(const std::string& code) {
    fProfile = code;
    fSavedRow = userRow;
    fSavedIn = userInRows;
    fSavedScroll = userScroll;
    fSub = FSub::Profile;
    userRow = -1; // the first row that can take the cursor
    userInRows = true;
    userScroll = 0;
    userGlide = Glide();
}

void DrawFriendList(int u, bool live) {
    Theme T = Th();
    const ListLay L = LL();
    const auto& list = FriendsOf(u);
    const int n = static_cast<int>(list.size());
    if (n == 0) {
        DrawUserNotice("friends");
        return;
    }
    const bool sharing = FriendsSharing(u);
    const float cw = L.rowW / kFCols, top0 = L.rowY0 + 10;
    const float view = L.bottom - top0 - 6;
    if (live) {
        userRow = (std::max)(0, (std::min)(userRow, n - 1));
        float target = userGlide.t0 < 0 ? userScroll : userGlide.to;
        if (!userInRows) {
            target = 0;
        } else {
            float top = static_cast<float>(userRow / kFCols) * kFCardH;
            if (top < target) target = top;
            if (top + kFCardH > target + view) target = top + kFCardH - view;
        }
        if (MouseOwns(&userScroll)) target = userScroll;
        GlideTo(userGlide, userScroll, target);
    }
    // Dragged (see the mouse block in ui.cpp), the focus keeps to a line of cards wholly in view.
    const int lines = (n + kFCols - 1) / kFCols;
    const int listDrag = !live ? -1 :
        AddScroll(L.rowX - 14, Px(128), L.rowW + 28, Px(968) - Px(128), ScrollOf(userScroll, lines * kFCardH - view, [n, lines, view](float v) {
            int r = NearestFit(userRow / kFCols, lines, [&](int i) { return i * kFCardH - v >= -0.5f && (i + 1) * kFCardH - v <= view + 0.5f; });
            if (r < 0) return;
            userRow = (std::min)(n - 1, r * kFCols + userRow % kFCols);
            userInRows = true;
        }));
    PushClip(L.rowX - 14, Px(128), L.rowW + 28, Px(968) - Px(128));
    for (int i = 0; i < n; i++) {
        const friends::Friend& f = list[i];
        const float x = L.rowX + static_cast<float>(i % kFCols) * cw, y = top0 + static_cast<float>(i / kFCols) * kFCardH - userScroll;
        if (y > 660 || y + kFCardH < 60) continue;
        const bool focused = live && userInRows && userRow == i;
        const CursorRect frame = {x + 5, y, cw - 10, kFCardH - 10, L.radius};
        const FLine line = FriendLine(f, sharing);
        const float cx = x + cw / 2;
        if (focused) RRect(frame.x, frame.y, frame.w, frame.h, frame.r, T.focusBg);
        DrawFriendDisc(f, cx, y + 56, 40);
        // Present: a ring in the accent colour round the disc.
        if (line.online) CircleStroke(cx, y + 56, 45, T.accent, 2.4f);
        Text(FriendNick(f), x + 12, y + 104, cw - 24, 30, 19.5f, T.text, Center);
        Text(line.a, x + 10, y + 132, cw - 20, 24, 15.6f, line.online ? T.accent : T.sub, Center);
        if (!line.b.empty()) Text(line.b, x + 10, y + 153, cw - 20, 24, 15.6f, T.sub, Center);
        if (focused) {
            CursorFill(T.focusBg);
            Cursor(frame.x, frame.y, frame.w, frame.h, frame.r);
        }
        if (live)
            AddHit(frame.x, frame.y, frame.w, frame.h, [i] {
                bool was = userInRows && userRow == i;
                userInRows = true;
                userRow = i;
                if (was) OnButtonImpl(Btn::A);
                else PlaySfx(Sfx::Move);
            });
    }
    PopClip();
    // "Actualizar": the page's ring turns, small, while the friends are read again.
    if (now - fRefreshAt < 1.0 || (now - fRefreshAt < 30 && friends::Refreshing())) {
        float spin = static_cast<float>(fmod(now * 360.0, 360.0));
        CircleStroke(1206, 92, 11, alpha(T.sub, 0.25f), 2.2f);
        Arc(1206, 92, 11, spin, spin + 110, T.sub, 2.2f);
        Touch();
    }
    if (!sharing)
        Text(L"Has dejado de compartir tu actividad: el estado de tus amigos no se actualiza.", L.rowX, L.bottom - 34,
             L.rowW, 30, 16.6f, T.sub, Center);
    const float total = static_cast<float>((n + kFCols - 1) / kFCols) * kFCardH;
    if (total > view) {
        float track = L.bottom - L.top - 16, len = (std::max)(30.0f, track * view / total);
        RRect(L.barX, L.top + 8 + (std::min)(1.0f, userScroll / (std::max)(1.0f, total - view)) * (track - len), 4, len, 2,
              cfg.dark ? rgb(0x5A5A5A) : rgb(0xC6C2C7));
        AddScrollBar(listDrag, L.barX, L.top + 8, track, len, total - view);
    }
}

// The list's own buttons; false: the page's usual handling applies.
bool FriendListButton(Btn b, int u) {
    const int n = static_cast<int>(FriendsOf(u).size());
    if (b == Btn::Y) {
        PlaySfx(Sfx::Ok);
        friends::RefreshNow();
        fRefreshAt = now;
        if (n == 0) userSecAt = now; // the empty page's ring turns again
        Touch();
        return true;
    }
    if (!userInRows) {
        if ((b == Btn::Right || b == Btn::A) && n > 0) {
            userInRows = true;
            userRow = 0;
            PlaySfx(b == Btn::A ? Sfx::Ok : Sfx::Move);
            return true;
        }
        return false;
    }
    if (n == 0) return false;
    userRow = (std::max)(0, (std::min)(userRow, n - 1));
    const int col = userRow % kFCols;
    int to = userRow;
    switch (b) {
    case Btn::Left:
        if (col == 0) return false; // back to the sections
        to--;
        break;
    case Btn::Right:
        if (col < kFCols - 1 && userRow + 1 < n) to++;
        break;
    case Btn::Up:
        if (userRow >= kFCols) to -= kFCols;
        break;
    case Btn::Down:
        if (userRow + kFCols < n) to += kFCols;
        else if (userRow / kFCols < (n - 1) / kFCols) to = n - 1; // a shorter last row
        break;
    case Btn::A:
        PlaySfx(Sfx::Ok);
        FriendOpenProfile(FriendsOf(u)[userRow].code);
        return true;
    default: return false;
    }
    if (to != userRow) {
        userRow = to;
        PlaySfx(Sfx::Move);
    } else {
        PlaySfx(Sfx::Bump);
        BumpCursor(b == Btn::Right ? 1.0f : 0.0f, b == Btn::Up ? -1.0f : b == Btn::Down ? 1.0f : 0.0f);
    }
    return true;
}

// ---- a friend's profile ---------------------------------------------------------

void FriendRemove(int u, const std::string& code) {
    const friends::Friend* f = FriendByCode(u, code);
    if (!f) return;
    const std::wstring nick = FriendNick(*f);
    Ask(L"¿Quieres eliminar a " + nick + L" de tu lista de amigos?\n\nTambién desaparecerás de la suya, y " + nick +
            L" no podrá ver\nnada de lo que compartas a partir de ahora.",
        {L"Cancelar", L"Eliminar"}, [u, code](int b) {
            if (b != 1 || u >= static_cast<int>(cfg.users.size())) return;
            friends::Remove(cfg.users[u].id, code);
            FriendsCloseSub();
            userRow = 0;
        });
}

std::vector<URow> FriendProfileRows(int u, const friends::Friend& f) {
    std::vector<URow> rows;
    auto head = [&](const std::wstring& label) {
        URow r;
        r.kind = URow::Head;
        r.label = label;
        rows.push_back(r);
    };
    head(L"Historial de programas usados");
    const std::wstring nick = FriendNick(f);
    for (const auto& g : f.st.games) {
        if (!f.known) break;
        URow r;
        r.label = FriendText(g.name, 60);
        r.value = g.seconds < 60 ? std::wstring(L"Menos de 1 min") : PlayTime(g.seconds);
        const std::wstring label = r.label, value = r.value;
        r.act = [label, value, nick] { Info(label + L"\n\nTiempo de juego de " + nick + L": " + value); };
        rows.push_back(r);
    }
    if (rows.back().kind == URow::Head) {
        URow r;
        r.kind = URow::Para;
        r.lines = {L"Todavía no hay actividad."};
        rows.push_back(r);
    }
    head(L"Amigo");
    URow rm;
    rm.label = L"Eliminar amigo";
    const std::string code = f.code;
    rm.act = [u, code] { FriendRemove(u, code); };
    rows.push_back(rm);
    return rows;
}

void DrawFriendProfile() {
    Theme T = Th();
    const ListLay L = LL();
    const friends::Friend* fp = FriendByCode(pageUser, fProfile);
    if (!fp) {
        FriendsCloseSub();
        return;
    }
    const friends::Friend f = *fp;
    const FLine line = FriendLine(f, FriendsSharing(pageUser));
    // The header of the user page, with the friend's disc where the user's picture is.
    DrawFriendDisc(f, 107, 48, 27);
    Text(L"Perfil de " + FriendNick(f), 150, 28, 900, 46, 28.6f, T.text);
    HRule(48, 1232, Px(127), T.line);
    VRule(L.divX, Px(147), Px(948), T.sep);

    // Left: who it is and what they are doing, laid out as the picture chooser's preview.
    const float px = 258, py = 262;
    Circle(px, py, 100, line.online ? T.accent : T.avRing);
    DrawFriendDisc(f, px, py, 94);
    Text(FriendNick(f), px - 190, py + 114, 380, 44, 28, T.text, Center);
    Text(line.a, px - 190, py + 160, 380, 30, 19.5f, line.online ? T.accent : T.sub, Center);
    if (!line.b.empty()) Text(line.b, px - 190, py + 188, 380, 30, 19.5f, line.online ? T.text : T.sub, Center);
    if (f.known)
        Text(L"Tiempo de juego: " + (f.st.total < 60 ? std::wstring(L"menos de 1 min") : PlayTime(f.st.total)), px - 190,
             py + 226, 380, 30, kUNote, T.sub, Center);
    Text(L"Clave de amigo: " + FriendCodeText(f.code), px - 190, py + 254, 380, 30, kUNote, T.sub, Center);

    std::vector<URow> rows = FriendProfileRows(pageUser, f);
    DrawUserRows(rows, pageUser, true);
    Footer({{L"B", L"Atrás", Btn::B}, {L"A", L"Confirmar", Btn::A}});
    Touch();
}

void FriendProfileButton(Btn b) {
    const friends::Friend* f = FriendByCode(pageUser, fProfile);
    if (!f) {
        FriendsCloseSub();
        return;
    }
    std::vector<URow> rows = FriendProfileRows(pageUser, *f);
    if (b == Btn::Up || b == Btn::Down) {
        int d = b == Btn::Up ? -1 : 1, to = NextURow(rows, userRow, d);
        if (to < 0) {
            PlaySfx(Sfx::Bump);
            BumpCursor(0, static_cast<float>(d));
        } else {
            userRow = to;
            PlaySfx(Sfx::Move);
        }
    } else if (b == Btn::A) {
        if (userRow >= 0 && userRow < static_cast<int>(rows.size()) && rows[userRow].act) {
            PlaySfx(Sfx::Ok);
            rows[userRow].act();
        }
    } else if (b == Btn::B) {
        PlaySfx(Sfx::Back);
        FriendsCloseSub();
    }
}

// ---- hooks ------------------------------------------------------------------------

bool DrawFriendsSub() {
    switch (fSub) {
    case FSub::None: return false;
    case FSub::Pad: DrawFriendPad(); break;
    case FSub::Found: DrawFriendFound(); break;
    case FSub::Profile: DrawFriendProfile(); break;
    case FSub::Received: DrawRequestList(true); break;
    case FSub::Sent: DrawRequestList(false); break;
    case FSub::Asker: DrawRequestCard(true); break;
    case FSub::Asked: DrawRequestCard(false); break;
    }
    return true;
}

// Before the user page's own handling; true: the press was for the friends pages.
bool FriendsButton(Btn b) {
    switch (fSub) {
    case FSub::Pad: FriendPadButton(b); return true;
    case FSub::Found: FriendFoundButton(b); return true;
    case FSub::Profile: FriendProfileButton(b); return true;
    case FSub::Received: RequestListButton(b, true); return true;
    case FSub::Sent: RequestListButton(b, false); return true;
    case FSub::Asker: RequestCardButton(b, true); return true;
    case FSub::Asked: RequestCardButton(b, false); return true;
    case FSub::None: break;
    }
    if (!S2()) return false;
    const std::vector<USection> secs = UserSections();
    if (userSec < 0 || userSec >= static_cast<int>(secs.size()) || secs[userSec].id != "friends") return false;
    return FriendListButton(b, pageUser);
}

// What NSW-Home says about whoever is using it, from what it already knows:
// the current user, the game that is open, the play records. The other users
// of this PC who share are shown signed off.
void FriendsSelf() {
    if (cfg.users.empty() || (scanning && games.empty())) return;
    const int u = (std::max)(0, (std::min)(cfg.currentUser, static_cast<int>(cfg.users.size()) - 1));
    for (int i = 0; i < static_cast<int>(cfg.users.size()); i++) {
        const User& user = cfg.users[i];
        friends::Status st;
        st.nick = Narrow(user.name);
        st.colour = friends::Colour(user.id, i % AvatarColours());
        if (i != u) {
            st.state = friends::SignedOff;
            friends::SetSelf(user.id, st);
            continue;
        }
        if (!runningId.empty()) {
            st.state = friends::Playing;
            if (Game* g = FindGame(runningId)) {
                st.game = Narrow(g->name);
                st.gameId = g->titleId.size() == 16 ? g->titleId : std::string();
            }
        }
        for (const auto& g : games) {
            Played p = UserPlayed(i, g);
            if (!p.seconds) continue;
            st.total += p.seconds;
            st.games.push_back({g.titleId.size() == 16 ? g.titleId : std::string(), Narrow(g.name), p.seconds});
        }
        std::stable_sort(st.games.begin(), st.games.end(), [](const friends::GameTime& a, const friends::GameTime& b) { return a.seconds > b.seconds; });
        friends::SetSelf(user.id, st);
    }
}

// Once a frame.
void FriendsFrame() {
    FriendsEnsure();
    static double selfAt = -10;
    static unsigned seen = 0;
    if (now - selfAt >= 1.0 || now < selfAt) {
        selfAt = now;
        FriendsSyncCodes(); // a user added since has a code made for them
        FriendsSelf();
    }
    FriendLookupPoll();
    // The friends pages read more often while someone is looking at them.
    bool watching = false;
    if (S2() && !stack.empty() && stack.back() == Scr::User && uSub == USub::None) {
        if (fSub != FSub::None && fSub != FSub::Pad && fSub != FSub::Found) watching = true;
        else if (fSub == FSub::None) watching = UserSectionIndex("friends") == userShown || UserSectionIndex("add_friend") == userShown;
    }
    friends::Watching(watching || NetplayWatching());
    unsigned v = friends::Version();
    if (v != seen || watching) {
        seen = v;
        Touch();
    }
}

// Test states (--shot / --shotseq): made-up friends, one in each state,
// made-up requests, and a search that answers without any network.
//   friends-list      the list with the cursor on the first card
//   friends-sec       the page with "Lista de amigos" focused
//   friends-empty     the same with no friends
//   friends-profile   a friend's profile (friends-profile:<n> for the n-th card)
//   friends-add       "Añadir un amigo" with the cursor on "Buscar por clave de amigo"
//                     (sharing off: A brings the consent box; friends-add-on: sharing on;
//                     friends-add-nf: the search finds nobody; friends-add-net: no connection;
//                     friends-add-unv: only a card that does not make the code)
//   friends-pad       the number pad, half filled
//   friends-found     what the search found, with "Enviar solicitud de amistad"
//   friends-consent   the consent box
//   friends-off       the list with sharing turned off
//   friends-received  "Solicitudes de amistad recibidas" with three waiting
//                     (friends-received-empty; friends-received-trouble: with both warnings)
//   friends-asker     one received request opened: [Aceptar] [Rechazar]
//   friends-sent      "Solicitudes de amistad enviadas" with two
//   friends-asked     one sent request opened: "Cancelar la solicitud"
//   friends-reqsec    "Añadir un amigo" focused in the sections, requests waiting
//   friends-home      HOME, the first user with requests waiting
bool DebugFriends(const std::wstring& name) {
    if (name.rfind(L"friends-", 0) != 0) return false;
    if (name.size() > 2 && (name.compare(name.size() - 2, 2, L"-2") == 0 || name.compare(name.size() - 3, 3, L"-2l") == 0))
        return false; // DebugGoto sets the style and comes back
    userTest = true;
    FriendsEnsure();
    const int u = (std::min)(pageUser, static_cast<int>(cfg.users.size()) - 1);
    const std::string owner = cfg.users[u].id;
    const int64_t t = friends::UnixNow();
    auto fake = [&](const char* code, const char* nick, int colour, int state, const char* game, int64_t ago, uint64_t total) {
        friends::Friend f;
        f.code = code;
        f.known = true;
        f.seen = t - ago;
        f.st.nick = nick;
        f.st.colour = colour;
        f.st.state = state;
        f.st.game = game;
        f.st.time = t - ago;
        f.st.total = total;
        f.st.games = {{"", "Aventura de prueba", total * 5 / 8}, {"", "Carreras de ejemplo", total * 2 / 8}, {"", "Puzles inventados", total / 8}};
        return f;
    };
    auto card = [](const char* code, const char* nick, int colour) {
        friends::Card c;
        c.code = code;
        c.nick = nick;
        c.colour = colour;
        return c;
    };
    auto request = [&](const char* code, const char* nick, int colour, int64_t ago) {
        friends::Request r;
        r.who = card(code, nick, colour);
        r.time = r.at = t - ago;
        return r;
    };
    const bool empty = name == L"friends-empty";
    if (!empty) {
        friends::TestAddFriend(owner, fake("100000000001", "Prueba A", 1, friends::Playing, "Aventura de prueba", 40, 306000));
        friends::TestAddFriend(owner, fake("100000000002", "Prueba B", 2, friends::InMenu, "", 120, 45000));
        friends::TestAddFriend(owner, fake("100000000003", "Prueba C", 4, friends::SignedOff, "", 3 * 3600, 7300));
        friends::TestAddFriend(owner, fake("100000000004", "Prueba D", 5, friends::Playing, "Carreras de ejemplo", 23 * 3600, 990));
        friends::TestAddFriend(owner, fake("100000000005", "Prueba E", -1, friends::InMenu, "", 12 * 86400, 0));
        friends::Friend fresh = fake("100000000006", "Prueba F", 6, friends::InMenu, "", 0, 0);
        fresh.known = false; // accepted a moment ago: no status read yet
        friends::TestAddFriend(owner, fresh);
    }
    const bool requests = name.rfind(L"friends-received", 0) == 0 || name == L"friends-asker" || name == L"friends-sent" ||
                          name == L"friends-asked" || name == L"friends-reqsec" || name == L"friends-home" || name.rfind(L"friends-add", 0) == 0;
    if (requests && name != L"friends-received-empty") {
        friends::TestAddReceived(owner, request("200000000001", "Prueba H", 3, 90));
        friends::TestAddReceived(owner, request("200000000002", "Prueba I", 6, 2 * 3600));
        friends::TestAddReceived(owner, request("200000000003", "Prueba J", -1, 3 * 86400));
        friends::TestAddSent(owner, request("300000000001", "Prueba K", 2, 600));
        friends::TestAddSent(owner, request("300000000002", "Prueba L", 5, 26 * 3600));
    }
    const bool on = name != L"friends-add" && name != L"friends-consent" && name != L"friends-off";
    friends::SetSharing(owner, on);
    if (name == L"friends-received-trouble") {
        friends::Trouble tr;
        tr.card = tr.inbox = true;
        friends::TestSetTrouble(owner, tr);
    }
    friends::TestLookup(name == L"friends-add-nf"    ? friends::Look::NotFound
                        : name == L"friends-add-net" ? friends::Look::NoNetwork
                        : name == L"friends-add-unv" ? friends::Look::Unverified
                                                     : friends::Look::Found,
                        card("123412341234", "Prueba G", 3), 0);
    if (name == L"friends-home") return true; // HOME is where a test run starts
    OpenUserPage(u);
    auto section = [](const char* id, bool inRows) {
        int i = UserSectionIndex(id);
        if (i < 0) return;
        userSec = userShown = i;
        userInRows = inRows;
        userRow = 0;
        userSecAt = -10;
    };
    auto rowLabelled = [u](const wchar_t* label) {
        std::vector<URow> rows = UserRows("add_friend", u);
        for (int i = 0; i < static_cast<int>(rows.size()); i++)
            if (rows[i].label == label) userRow = i;
    };
    if (name == L"friends-list" || name == L"friends-off") {
        section("friends", true);
    } else if (name == L"friends-sec" || empty) {
        section("friends", false);
    } else if (name.rfind(L"friends-profile", 0) == 0) {
        section("friends", true);
        size_t colon = name.find(L':');
        int i = colon == std::wstring::npos ? 0 : static_cast<int>(wcstol(name.c_str() + colon + 1, nullptr, 10));
        const auto& list = FriendsOf(u);
        userRow = (std::max)(0, (std::min)(i, static_cast<int>(list.size()) - 1));
        FriendOpenProfile(list[userRow].code);
    } else if (name.rfind(L"friends-add", 0) == 0 || name == L"friends-consent") {
        section("add_friend", true);
        rowLabelled(L"Buscar por clave de amigo");
        if (name == L"friends-consent") FriendsSearch(u);
    } else if (name == L"friends-reqsec") {
        section("add_friend", false);
    } else if (name == L"friends-pad") {
        section("add_friend", true);
        fPad = L"1234123";
        fPadSel = 3;
        fSub = FSub::Pad;
    } else if (name == L"friends-found") {
        section("add_friend", true);
        rowLabelled(L"Buscar por clave de amigo");
        fPad = L"123412341234";
        fFound = card("123412341234", "Prueba G", 3);
        fSub = FSub::Found;
    } else if (name.rfind(L"friends-received", 0) == 0 || name == L"friends-asker") {
        section("add_friend", true);
        rowLabelled(L"Solicitudes de amistad recibidas");
        FriendsOpenReceived(u);
        if (name == L"friends-asker") {
            fReqCode = FriendsRequests(u, true)[0].who.code;
            fSub = FSub::Asker;
        }
    } else if (name == L"friends-sent" || name == L"friends-asked") {
        section("add_friend", true);
        rowLabelled(L"Solicitudes de amistad enviadas");
        FriendsOpenSent(u);
        if (name == L"friends-asked") {
            fReqCode = FriendsRequests(u, false)[0].who.code;
            fSub = FSub::Asked;
        }
    } else {
        return false;
    }
    return true;
}
