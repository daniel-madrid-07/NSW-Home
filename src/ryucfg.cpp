// Ryujinx Config.json: value-level reads and writes, and the settings table.
#include "ryucfg.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace ryucfg {

const wchar_t* const kConsola = L"Consola";
const wchar_t* const kPantallas = L"Pantallas";
const wchar_t* const kSonido = L"Sonido";
const wchar_t* const kInternet = L"Internet";
const wchar_t* const kMandos = L"Mandos y accesorios";
const wchar_t* const kDatos = L"Gestión de datos";
const wchar_t* const kEmulador = L"Emulador";

namespace {

// ---- the file -------------------------------------------------------------

enum class Tok { Bool, Number, String, Null, Other };

struct Span {
    size_t from = 0, to = 0; // value text, quotes included for strings
    Tok tok = Tok::Other;
};

struct File {
    std::wstring path;
    std::string text;
    std::map<std::string, Span> keys; // dotted path -> where its value is
    bool open = false;
};
File g;
// Every value each key has been seen to hold in the file; see Set.
std::map<std::string, std::set<std::string>> seen;

bool ReadFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// Temp file in the same folder, flushed, then swapped in: the file is always
// either the old one or the new one, never half of either.
bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".dockside-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size();
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Walks the JSON text and notes where each value sits, without building a
// tree. Strict enough that a damaged file is refused rather than patched.
struct Scanner {
    const std::string& s;
    size_t p = 0;
    std::map<std::string, Span>& out;

    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) p++;
    }
    bool string() { // at the opening quote; leaves p after the closing one
        for (p++; p < s.size(); p++) {
            if (s[p] == '\\') p++;
            else if (s[p] == '"') {
                p++;
                return true;
            }
        }
        return false;
    }
    bool literal(const char* word) {
        size_t n = strlen(word);
        if (s.compare(p, n, word) != 0) return false;
        p += n;
        return true;
    }
    bool number() {
        size_t from = p;
        while (p < s.size() && (isdigit(static_cast<unsigned char>(s[p])) || strchr("+-.eE", s[p]))) p++;
        if (p == from) return false;
        std::string t = s.substr(from, p - from);
        char* e = nullptr;
        strtod(t.c_str(), &e);
        return *e == 0;
    }
    // `path` empty: a value inside an array, which is checked but not recorded.
    bool value(const std::string& path, bool record, int depth) {
        ws();
        if (p >= s.size() || depth > 64) return false;
        Span sp;
        sp.from = p;
        char c = s[p];
        if (c == '{') {
            p++;
            ws();
            if (p < s.size() && s[p] == '}') p++;
            else
                for (;;) {
                    ws();
                    if (p >= s.size() || s[p] != '"') return false;
                    size_t k0 = p + 1;
                    if (!string()) return false;
                    std::string name = s.substr(k0, p - 1 - k0);
                    ws();
                    if (p >= s.size() || s[p] != ':') return false;
                    p++;
                    // A name with a dot or an escape cannot be addressed; skip it.
                    bool plain = name.find_first_of(".\\") == std::string::npos;
                    if (!value(path.empty() ? name : path + "." + name, record && plain, depth + 1)) return false;
                    ws();
                    if (p < s.size() && s[p] == ',') { p++; continue; }
                    if (p < s.size() && s[p] == '}') { p++; break; }
                    return false;
                }
        } else if (c == '[') {
            p++;
            ws();
            if (p < s.size() && s[p] == ']') p++;
            else
                for (;;) {
                    if (!value({}, false, depth + 1)) return false;
                    ws();
                    if (p < s.size() && s[p] == ',') { p++; continue; }
                    if (p < s.size() && s[p] == ']') { p++; break; }
                    return false;
                }
        } else if (c == '"') {
            if (!string()) return false;
            sp.tok = Tok::String;
        } else if (c == 't' || c == 'f') {
            if (!literal(c == 't' ? "true" : "false")) return false;
            sp.tok = Tok::Bool;
        } else if (c == 'n') {
            if (!literal("null")) return false;
            sp.tok = Tok::Null;
        } else {
            if (!number()) return false;
            sp.tok = Tok::Number;
        }
        sp.to = p;
        if (record && !path.empty()) out[path] = sp;
        return true;
    }
};

void Utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string Unquote(const std::string& q) { // q includes both quotes
    std::string out;
    auto hex4 = [&](size_t at) {
        unsigned v = 0;
        for (size_t i = at; i < at + 4 && i < q.size(); i++) {
            char c = q[i];
            unsigned d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 0;
            v = v * 16 + d;
        }
        return v;
    };
    for (size_t i = 1; i + 1 < q.size(); i++) {
        if (q[i] != '\\') { out += q[i]; continue; }
        char c = q[++i];
        switch (c) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            unsigned cp = hex4(i + 1);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 2 < q.size() && q[i + 1] == '\\' && q[i + 2] == 'u') {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4(i + 3) - 0xDC00);
                i += 6;
            }
            Utf8(out, cp);
            break;
        }
        default: out += c;
        }
    }
    return out;
}

std::string Quote(const std::string& v) {
    std::string o = "\"";
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04X", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

std::string ValueOf(const File& f, const Span& sp) {
    std::string raw = f.text.substr(sp.from, sp.to - sp.from);
    switch (sp.tok) {
    case Tok::String: return Unquote(raw);
    case Tok::Bool:
    case Tok::Number: return raw;
    default: return {};
    }
}

bool Load(const std::wstring& path, File& f) {
    f = File();
    if (!ReadFile(path, f.text)) return false;
    Scanner sc{f.text, 0, f.keys};
    if (f.text.size() >= 3 && !f.text.compare(0, 3, "\xEF\xBB\xBF")) sc.p = 3;
    sc.ws();
    if (sc.p >= f.text.size() || f.text[sc.p] != '{' || !sc.value({}, true, 0)) return false;
    sc.ws();
    if (sc.p != f.text.size()) return false; // something after the closing brace
    f.path = path;
    f.open = true;
    for (const auto& kv : f.keys)
        if (kv.second.tok != Tok::Other && kv.second.tok != Tok::Null) seen[kv.first].insert(ValueOf(f, kv.second));
    return true;
}

bool ParseNumber(const std::string& v, double& out) {
    if (v.empty() || v.find_first_not_of("0123456789+-.eE") != std::string::npos) return false;
    char* e = nullptr;
    out = strtod(v.c_str(), &e);
    return *e == 0 && std::isfinite(out);
}

// Shortest text for a number: no trailing zeros, no exponent for these ranges.
std::string Fmt(double v, bool whole) {
    char b[40];
    if (whole) snprintf(b, sizeof b, "%.0f", std::round(v));
    else snprintf(b, sizeof b, "%.10g", v);
    if (!strcmp(b, "-0")) return "0";
    return b;
}

Tok TokFor(Type t) {
    return t == Type::Bool ? Tok::Bool : t == Type::String ? Tok::String : Tok::Number;
}

// Text to put in the file for `value` at `key`, or false if it must not go in.
bool Encode(const File& f, const std::string& key, const std::string& value, std::string& out) {
    auto it = f.keys.find(key);
    if (it == f.keys.end()) return false;
    const Tok tok = it->second.tok;
    const Setting* s = Find(key);
    if (s && TokFor(s->type) != tok) return false;

    double num = 0;
    switch (tok) {
    case Tok::Bool:
        if (value != "true" && value != "false") return false;
        out = value;
        break;
    case Tok::Number:
        if (!ParseNumber(value, num)) return false;
        if (s && s->type == Type::Int && num != std::floor(num)) return false;
        out = value;
        break;
    case Tok::String:
        out = Quote(value);
        break;
    default:
        return false; // null, objects and arrays are left alone
    }
    if (!s) return true;

    auto was = seen.find(key);
    if (was != seen.end() && was->second.count(value)) return true;
    if (s->kind == Kind::Choice) {
        for (const auto& c : s->choices)
            if (c.value == value) return true;
        return false;
    }
    if (s->kind == Kind::Number) return num >= s->min && num <= s->max;
    return true;
}

} // namespace

// ---- the settings table ---------------------------------------------------
//
// Where the names come from: the keys and their JSON types are those of a real
// Config.json written by Ryujinx 1.1.1403 (version 51). The enum names were
// each found inside that release's Ryujinx.exe. For the two enums stored as
// numbers (hide_cursor, multiplayer_mode) the numbering is from the source as
// remembered; 1.1.1403's own default file has hide_cursor = 1 (OnIdle), which
// agrees. Entries with verified = false are explained where they appear.
//
// An enum name Ryujinx does not know makes it reject the whole file and start
// again from defaults, which is why Set refuses values outside these lists.

const std::vector<Setting>& Settings() {
    static const std::vector<Setting> list = [] {
        std::vector<Setting> v;
        auto toggle = [&](const char* key, const wchar_t* sec, const wchar_t* label, const wchar_t* help,
                          bool verified = true) {
            v.push_back({key, sec, label, help, Kind::Toggle, Type::Bool, {}, 0, 1, 1, 1, L"", verified, 0});
        };
        auto choice = [&](const char* key, const wchar_t* sec, const wchar_t* label, const wchar_t* help, Type type,
                          std::vector<Choice> choices, bool verified = true, int maxVersion = 0) {
            v.push_back({key, sec, label, help, Kind::Choice, type, std::move(choices), 0, 0, 0, 1, L"", verified,
                         maxVersion});
        };
        auto number = [&](const char* key, const wchar_t* sec, const wchar_t* label, const wchar_t* help, Type type,
                          double min, double max, double step, double shown, const wchar_t* unit) {
            v.push_back({key, sec, label, help, Kind::Number, type, {}, min, max, step, shown, unit, true, 0});
        };

        // -- Consola --
        choice("system_language", kConsola, L"Idioma", L"Idioma en el que se muestran los juegos.", Type::String,
               {{"Spanish", L"Español"},
                {"LatinAmericanSpanish", L"Español (Latinoamérica)"},
                {"AmericanEnglish", L"English (US)"},
                {"BritishEnglish", L"English (UK)"},
                {"French", L"Français"},
                {"CanadianFrench", L"Français (Canada)"},
                {"German", L"Deutsch"},
                {"Italian", L"Italiano"},
                {"Portuguese", L"Português"},
                {"BrazilianPortuguese", L"Português (Brasil)"},
                {"Dutch", L"Nederlands"},
                {"Russian", L"Русский"},
                {"Japanese", L"日本語"},
                {"Korean", L"한국어"},
                {"SimplifiedChinese", L"简体中文"},
                {"TraditionalChinese", L"繁體中文"},
                {"Chinese", L"中文 (antiguo)"},
                {"Taiwanese", L"中文 (Taiwán, antiguo)"}});
        choice("system_region", kConsola, L"Región", L"Región de la consola. Algunos juegos cambian de contenido según la región.",
               Type::String,
               {{"Europe", L"Europa"},
                {"USA", L"América"},
                {"Japan", L"Japón"},
                {"Australia", L"Australia y Nueva Zelanda"},
                {"China", L"China"},
                {"Korea", L"Corea"},
                {"Taiwan", L"Taiwán"}});
        // Unverified: any string is stored; the emulator checks it against the
        // zone list of the installed firmware when a game starts and falls back
        // to UTC if it is not there. These are standard zone names that the
        // console's list is expected to contain; none was tried.
        choice("system_time_zone", kConsola, L"Zona horaria", L"Zona horaria que ven los juegos.", Type::String,
               {{"UTC", L"UTC"},
                {"Europe/Madrid", L"Madrid"},
                {"Atlantic/Canary", L"Canarias"},
                {"Europe/London", L"Londres"},
                {"Europe/Lisbon", L"Lisboa"},
                {"Europe/Paris", L"París"},
                {"Europe/Berlin", L"Berlín"},
                {"Europe/Rome", L"Roma"},
                {"Europe/Athens", L"Atenas"},
                {"Europe/Moscow", L"Moscú"},
                {"America/New_York", L"Nueva York"},
                {"America/Chicago", L"Chicago"},
                {"America/Denver", L"Denver"},
                {"America/Los_Angeles", L"Los Ángeles"},
                {"America/Mexico_City", L"Ciudad de México"},
                {"America/Bogota", L"Bogotá"},
                {"America/Lima", L"Lima"},
                {"America/Santiago", L"Santiago de Chile"},
                {"America/Argentina/Buenos_Aires", L"Buenos Aires"},
                {"America/Sao_Paulo", L"São Paulo"},
                {"Asia/Tokyo", L"Tokio"},
                {"Asia/Seoul", L"Seúl"},
                {"Asia/Shanghai", L"Shanghái"},
                {"Australia/Sydney", L"Sídney"}},
               false);
        number("system_time_offset", kConsola, L"Ajuste del reloj",
               L"Diferencia entre la hora de la consola y la del PC.", Type::Int, -2592000, 2592000, 3600,
               1.0 / 3600, L"h");

        // -- Pantallas --
        toggle("docked_mode", kPantallas, L"Modo televisor",
               L"Activado: la consola está en la base (más resolución). Desactivado: modo portátil.");
        choice("res_scale", kPantallas, L"Resolución", L"Multiplica la resolución a la que se dibuja el juego.",
               Type::Int,
               {{"1", L"Original (720p/1080p)"},
                {"2", L"×2 (1440p/2160p)"},
                {"3", L"×3 (2160p/3240p)"},
                {"4", L"×4 (2880p/4320p)"},
                {"-1", L"Personalizada"}});
        number("res_scale_custom", kPantallas, L"Resolución personalizada",
               L"Multiplicador que se usa cuando Resolución es Personalizada.", Type::Float, 0.5, 4, 0.25, 1, L"×");
        choice("aspect_ratio", kPantallas, L"Formato de imagen", L"Proporción de la imagen en la pantalla.",
               Type::String,
               {{"Fixed16x9", L"16:9"},
                {"Fixed4x3", L"4:3"},
                {"Fixed16x10", L"16:10"},
                {"Fixed21x9", L"21:9"},
                {"Fixed32x9", L"32:9"},
                {"Stretched", L"Ajustar a la ventana"}});
        choice("anti_aliasing", kPantallas, L"Suavizado de bordes", L"Reduce los dientes de sierra en los bordes.",
               Type::String,
               {{"None", L"Desactivado"},
                {"Fxaa", L"FXAA"},
                {"SmaaLow", L"SMAA bajo"},
                {"SmaaMedium", L"SMAA medio"},
                {"SmaaHigh", L"SMAA alto"},
                {"SmaaUltra", L"SMAA ultra"}});
        choice("scaling_filter", kPantallas, L"Filtro de escalado",
               L"Cómo se adapta la imagen al tamaño de la pantalla.", Type::String,
               {{"Bilinear", L"Bilineal"}, {"Nearest", L"Vecino más cercano"}, {"Fsr", L"FSR 1.0"}});
        number("scaling_filter_level", kPantallas, L"Nitidez de FSR",
               L"Solo se usa cuando el filtro de escalado es FSR.", Type::Int, 0, 100, 5, 1, L"%");
        choice("max_anisotropy", kPantallas, L"Filtrado anisotrópico",
               L"Nitidez de las texturas vistas en ángulo.", Type::Float,
               {{"-1", L"Automático"}, {"2", L"×2"}, {"4", L"×4"}, {"8", L"×8"}, {"16", L"×16"}});

        // -- Sonido --
        number("audio_volume", kSonido, L"Volumen", L"Volumen del sonido de los juegos.", Type::Float, 0, 1, 0.05, 100,
               L"%");
        choice("audio_backend", kSonido, L"Salida de sonido", L"Sistema que reproduce el sonido.", Type::String,
               {{"SDL2", L"SDL2"}, {"OpenAl", L"OpenAL"}, {"SoundIo", L"SoundIO"}, {"Dummy", L"Sin sonido"}});

        // -- Internet --
        toggle("enable_internet_access", kInternet, L"Acceso a internet",
               L"Permite que los juegos se conecten a internet.");
        // Stored as a number. In 1.1.1403 the list is Disabled, LdnMitm; forks
        // with their own LDN insert a value in between, hence the version cap.
        // Unverified: the numbering comes from the source as remembered.
        choice("multiplayer_mode", kInternet, L"Juego local en red",
               L"Juego inalámbrico local con otras consolas a través de ldn_mitm.", Type::Int,
               {{"0", L"Desactivado"}, {"1", L"ldn_mitm"}}, false, 51);

        // -- Mandos y accesorios --
        toggle("enable_keyboard", kMandos, L"Teclado USB", L"Los juegos ven el teclado del PC como un teclado USB.");
        toggle("enable_mouse", kMandos, L"Ratón USB", L"Los juegos ven el ratón del PC como un ratón USB.");
        choice("hide_cursor", kMandos, L"Ocultar el puntero", L"Cuándo se esconde el puntero del ratón sobre el juego.",
               Type::Int, {{"0", L"Nunca"}, {"1", L"Al dejar de moverlo"}, {"2", L"Siempre"}});

        // -- Gestión de datos --
        toggle("enable_fs_integrity_checks", kDatos, L"Comprobar la integridad de los datos",
               L"Detecta archivos de juego dañados al leerlos.");

        // -- Emulador --
        choice("graphics_backend", kEmulador, L"Motor gráfico", L"Vulkan es el recomendado.", Type::String,
               {{"Vulkan", L"Vulkan"}, {"OpenGl", L"OpenGL"}});
        toggle("enable_vsync", kEmulador, L"Sincronización vertical",
               L"Mantiene la velocidad original del juego. Desactivada, el juego va tan rápido como pueda.");
        choice("backend_threading", kEmulador, L"Gráficos en varios hilos",
               L"Reparte el trabajo gráfico en otro hilo para reducir tirones.", Type::String,
               {{"Auto", L"Automático"}, {"On", L"Activado"}, {"Off", L"Desactivado"}});
        toggle("enable_shader_cache", kEmulador, L"Caché de sombreadores",
               L"Guarda los sombreadores en disco para reducir tirones en las siguientes partidas.");
        toggle("enable_ptc", kEmulador, L"Caché de traducción (PPTC)",
               L"Guarda el código traducido para que los juegos arranquen antes.");
        toggle("enable_texture_recompression", kEmulador, L"Recomprimir texturas",
               L"Usa menos memoria gráfica a costa de algo de calidad.");
        toggle("enable_macro_hle", kEmulador, L"Macros de GPU rápidas", L"Acelera ciertas operaciones gráficas.");
        choice("memory_manager_mode", kEmulador, L"Gestor de memoria",
               L"Cómo se emula la memoria de la consola. El más rápido es Host sin comprobaciones.", Type::String,
               {{"HostMappedUnsafe", L"Host sin comprobaciones (más rápido)"},
                {"HostMapped", L"Host"},
                {"SoftwarePageTable", L"Por software (más lento)"}});
        toggle("expand_ram", kEmulador, L"Ampliar la memoria",
               L"Da a la consola más memoria de la que tiene. Solo para mods de texturas; puede dar problemas.");
        toggle("ignore_missing_services", kEmulador, L"Ignorar servicios que faltan",
               L"Ayuda a arrancar algunos juegos; puede causar errores.");
        toggle("show_confirm_exit", kEmulador, L"Confirmar al cerrar", L"Pregunta antes de cerrar el emulador.");
        toggle("enable_discord_integration", kEmulador, L"Mostrar en Discord",
               L"Muestra en Discord a qué estás jugando.");

        // Keys that only newer forks (Ryubing) write. None of them is in the
        // 1.1.1403 file, so they never show there; names and values are from
        // memory of the fork's source and were not checked against a real file.
        choice("vsync_mode", kEmulador, L"Sincronización vertical",
               L"Consola: velocidad original. Sin límite: tan rápido como pueda.", Type::String,
               {{"Switch", L"Consola"}, {"Unbounded", L"Sin límite"}, {"Custom", L"Personalizada"}}, false);
        choice("dram_size", kEmulador, L"Memoria de la consola",
               L"Más de 4 GB solo hace falta para mods de texturas; puede dar problemas.", Type::String,
               {{"MemoryConfiguration4GiB", L"4 GB (original)"},
                {"MemoryConfiguration6GiB", L"6 GB"},
                {"MemoryConfiguration8GiB", L"8 GB"},
                {"MemoryConfiguration12GiB", L"12 GB"}},
               false);
        toggle("match_system_time", kConsola, L"Usar la hora del PC",
               L"El reloj de la consola sigue siempre al del PC.", false);
        toggle("enable_low_power_ptc", kEmulador, L"PPTC de bajo consumo",
               L"Usa menos núcleos al cargar la caché de traducción.", false);
        return v;
    }();
    return list;
}

const Setting* Find(const std::string& key) {
    for (const auto& s : Settings())
        if (s.key == key) return &s;
    return nullptr;
}

// ---- public ---------------------------------------------------------------

std::wstring ConfigPath(const std::wstring& emulatorExe) {
    size_t slash = emulatorExe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring portable = emulatorExe.substr(0, slash) + L"\\portable";
        if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable + L"\\Config.json";
    }
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return {};
    return std::wstring(buf, n) + L"\\Ryujinx\\Config.json";
}

bool Open(const std::wstring& configPath) {
    File f;
    bool ok = Load(configPath, f);
    g = ok ? std::move(f) : File();
    return ok;
}

int Version() {
    return atoi(Get("version").c_str());
}

bool Has(const std::string& key) {
    return g.open && g.keys.count(key) != 0;
}

std::string Get(const std::string& key) {
    if (!g.open) return {};
    auto it = g.keys.find(key);
    return it == g.keys.end() ? std::string() : ValueOf(g, it->second);
}

bool Available(const Setting& s) {
    if (!g.open) return false;
    auto it = g.keys.find(s.key);
    if (it == g.keys.end() || it->second.tok != TokFor(s.type)) return false;
    return s.maxVersion == 0 || Version() <= s.maxVersion;
}

bool SetMany(const std::vector<std::pair<std::string, std::string>>& changes) {
    if (!g.open) return false;
    // Work on what is on disk now, not on what was read at Open.
    File f;
    if (!Load(g.path, f)) return false;

    struct Edit {
        Span at;
        std::string text;
    };
    std::vector<Edit> edits;
    for (const auto& ch : changes) {
        std::string text;
        if (!Encode(f, ch.first, ch.second, text)) {
            g = std::move(f);
            return false;
        }
        const Span& at = f.keys[ch.first];
        // Same value: keep the file's own spelling of it.
        if (ValueOf(f, at) == ch.second) continue;
        for (const auto& e : edits)
            if (e.at.from == at.from) { // same key twice
                g = std::move(f);
                return false;
            }
        edits.push_back({at, text});
    }
    if (edits.empty()) {
        g = std::move(f);
        return true;
    }
    // Back to front, so earlier positions stay valid.
    std::sort(edits.begin(), edits.end(), [](const Edit& x, const Edit& y) { return x.at.from > y.at.from; });
    std::string text = f.text;
    for (const auto& e : edits) text.replace(e.at.from, e.at.to - e.at.from, e.text);

    const std::wstring backup = f.path + L".dockside-backup";
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(f.path.c_str(), backup.c_str(), TRUE) && GetLastError() != ERROR_FILE_EXISTS) {
        g = std::move(f);
        return false; // no backup, no write
    }
    bool ok = WriteFileAtomic(f.path, text);
    const std::wstring path = f.path;
    if (!Load(path, f)) {
        g = File();
        return false;
    }
    g = std::move(f);
    return ok;
}

bool Set(const std::string& key, const std::string& value) {
    return SetMany({{key, value}});
}

std::string Next(const Setting& s, const std::string& current, int dir) {
    if (s.kind == Kind::Toggle) return current == "true" ? "false" : "true";
    if (s.kind == Kind::Choice) {
        const int n = static_cast<int>(s.choices.size());
        if (n == 0) return current;
        int at = -1;
        for (int i = 0; i < n; i++)
            if (s.choices[i].value == current) at = i;
        if (at < 0) return s.choices[dir > 0 ? 0 : n - 1].value;
        return s.choices[((at + (dir > 0 ? 1 : -1)) % n + n) % n].value;
    }
    double v = 0;
    if (!ParseNumber(current, v)) v = s.min;
    // To the next mark of the min + k*step grid, also from a value between marks.
    double k = (v - s.min) / s.step;
    k = dir > 0 ? std::floor(k + 1e-6) + 1 : std::ceil(k - 1e-6) - 1;
    v = s.min + k * s.step;
    if (v < s.min) v = s.min;
    if (v > s.max) v = s.max;
    return Fmt(v, s.type == Type::Int);
}

std::wstring Display(const Setting& s, const std::string& value) {
    auto widen = [](const std::string& a) {
        int n = MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), nullptr, 0);
        std::wstring w(n, 0);
        if (n) MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), w.data(), n);
        return w;
    };
    if (s.kind == Kind::Toggle) return value == "true" ? L"Sí" : value == "false" ? L"No" : widen(value);
    if (s.kind == Kind::Choice) {
        for (const auto& c : s.choices)
            if (c.value == value) return c.label;
        return widen(value);
    }
    double v = 0;
    if (!ParseNumber(value, v)) return widen(value);
    v *= s.shown;
    wchar_t b[48];
    swprintf(b, ARRAYSIZE(b), L"%.2f", v);
    std::wstring t = b;
    while (t.back() == L'0') t.pop_back();
    if (t.back() == L'.') t.pop_back();
    if (t == L"-0") t = L"0";
    for (auto& c : t)
        if (c == L'.') c = L',';
    if (s.unit == L"×") return s.unit + t;
    return s.unit.empty() ? t : t + L" " + s.unit;
}

} // namespace ryucfg
