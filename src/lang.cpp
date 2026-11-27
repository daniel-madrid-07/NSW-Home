// The menu's language. See lang.h; the table is lang_strings.inc.

#include "lang.h"

#include "fwfont.h"
#include "fwtext.h"
#include "gfx.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace lang {
namespace {

// The console's own list (Settings > System > Language), in its order. The clock and number
// conventions are the ones the console sets for a language's region when it starts for the
// first time; dates are written in the region's usual order. The news pages answered 200 on
// 2026-11-26; the console maker has no Russian site any more, Russian gets the European one.
const std::vector<Language> kLanguages = {
    {"JPja", L"日本語", "Japanese", "0", L"ja-JP", 0, true, L'.', 2, L"/", L"https://www.nintendo.com/jp/topics/"},
    {"USen", L"English (United States)", "AmericanEnglish", "1", L"en-US", 0, false, L'.', 1, L"/", L"https://www.nintendo.com/us/whatsnew/"},
    {"EUen", L"English (United Kingdom)", "BritishEnglish", "12", L"en-GB", 0, true, L'.', 0, L"/", L"https://www.nintendo.com/en-gb/News/News-Updates-11145.html"},
    {"EUfr", L"Français", "French", "2", L"fr-FR", 0, true, L',', 0, L"/", L"https://www.nintendo.com/fr-fr/News/News-et-nouveautes-11145.html"},
    {"USfr", L"Français (Canada)", "CanadianFrench", "13", L"fr-CA", 0, true, L',', 2, L"-", L"https://www.nintendo.com/fr-ca/whatsnew/"},
    {"EUde", L"Deutsch", "German", "3", L"de-DE", 0, true, L',', 0, L".", L"https://www.nintendo.com/de-de/News/Neuigkeiten-Updates-11145.html"},
    {"EUit", L"Italiano", "Italian", "4", L"it-IT", 0, true, L',', 0, L"/", L"https://www.nintendo.com/it-it/Notizie/Notizie-e-aggiornamenti-11145.html"},
    {"EUes", L"Español (España)", "Spanish", "5", L"es-ES", 0, true, L',', 0, L"/", L"https://www.nintendo.com/es-es/Noticias/Noticias-y-novedades-11145.html"},
    {"USes", L"Español (Latinoamérica)", "LatinAmericanSpanish", "14", L"es-419", 0, false, L'.', 0, L"/", L"https://www.nintendo.com/es-mx/whatsnew/"},
    {"CNzh", L"简体中文", "SimplifiedChinese", "15", L"zh-CN", 5, true, L'.', 2, L"/", L"https://www.nintendoswitch.com.cn/"},
    {"TWzh", L"繁體中文", "TraditionalChinese", "16", L"zh-TW", 4, true, L'.', 2, L"/", L"https://www.nintendo.com/tw/topics"},
    {"KRko", L"한국어", "Korean", "7", L"ko-KR", 3, false, L'.', 2, L". ", L"https://www.nintendo.com/kr/news"},
    {"EUnl", L"Nederlands", "Dutch", "8", L"nl-NL", 0, true, L',', 0, L"-", L"https://www.nintendo.com/nl-nl/Nieuws/Nieuws-en-updates-11145.html"},
    {"EUpt", L"Português", "Portuguese", "9", L"pt-PT", 0, true, L',', 0, L"/", L"https://www.nintendo.com/pt-pt/Noticias/Noticias-e-atualizacoes-11145.html"},
    {"USpt", L"Português (Brasil)", "BrazilianPortuguese", "17", L"pt-BR", 0, true, L',', 0, L"/", L"https://www.nintendo.com/pt-br/whatsnew/"},
    {"EUru", L"Русский", "Russian", "10", L"ru-RU", 0, true, L',', 0, L".", L"https://www.nintendo.com/en-gb/News/News-Updates-11145.html"},
};
const int kSpanish = 7;

// Columns of an O line, after the Spanish: the languages other than Spanish (Spain), in this
// order. A column left empty takes the one named in kFallback (the same language of the other
// region), or, for Latin American Spanish, the Spanish text itself.
enum Col { cEnUS, cEnGB, cFr, cFrCA, cDe, cIt, cEs419, cJa, cKo, cZhHans, cZhHant, cNl, cPt, cPtBR, cRu, kCols };
// Language index (kLanguages) -> its column; -1 for Spanish (Spain).
const int kColumnOf[] = {cJa, cEnUS, cEnGB, cFr, cFrCA, cDe, cIt, -1, cEs419, cZhHans, cZhHant, cKo, cNl, cPt, cPtBR, cRu};
// Column -> the column it borrows from when empty (-1 none, -2 the Spanish text).
const int kFallback[kCols] = {-1, cEnUS, -1, cFr, -1, -1, -2, -1, -1, -1, -1, -1, -1, cPt, -1};

// The table is UTF-8 (half the size of wide literals in the program); a language's column is
// widened when the language is chosen.
struct FwLine {
    const char* es;
    const char* id;  // "<file>/<label>" of the firmware's message
};
struct OurLine {
    const char* es;
    const char* t[kCols];
};

#define F(es, id) FwLine{es, id},
#define O(...)
#define K(es)
const FwLine kFw[] = {
#include "lang_strings.inc"
    FwLine{nullptr, nullptr}};
#undef F
#undef O
#undef K
#define F(es, id)
#define O(es, ...) OurLine{es, {__VA_ARGS__}},
#define K(es)
const OurLine kOurs[] = {
#include "lang_strings.inc"
    OurLine{nullptr, {}}};
#undef F
#undef O
#undef K
#define F(es, id)
#define O(...)
#define K(es) es,
const char* const kKeep[] = {
#include "lang_strings.inc"
    nullptr};
#undef F
#undef O
#undef K

struct Entry {
    std::wstring text;
    char src;  // 'f' firmware, 'o' ours, 'k' kept
};
struct Pattern {
    std::vector<std::wstring> lits;  // the literal parts around %1..%n (n + 1 of them)
    std::vector<int> slot;           // slot[i]: which placeholder follows lits[i] (1-based)
    std::vector<bool> number;        // that part must be digits (%#1)
    Entry out;                       // with %1..%n where the parts go
};

int current = 1;                  // English (United States) until told otherwise
bool loaded = false;
std::wstring loadedFw, loadedKeys;
int loadedIndex = -2;
bool haveFw = false;
std::string fwError;
std::unordered_map<std::wstring, Entry> exact;
std::vector<Pattern> patterns;
std::unordered_map<std::wstring, std::wstring> memo;
std::unordered_map<std::wstring, char> memoSrc;
std::set<std::wstring> missing, ignored;
// Translations given out lately. Text wrapped by hand is translated whole and then measured and
// drawn a line at a time: those lines have no entries, and are not missing either.
std::vector<std::wstring> given;
std::wstring recordFile;
int verbatim = 0;

std::wstring Wide(const char* u) {
    std::wstring w;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(u);
    while (*p) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        for (; more > 0 && (*p & 0xC0) == 0x80; more--) c = (c << 6) | (*p++ & 0x3F);
        if (c >= 0x10000 && sizeof(wchar_t) == 2) {
            c -= 0x10000;
            w += static_cast<wchar_t>(0xD800 + (c >> 10));
            w += static_cast<wchar_t>(0xDC00 + (c & 0x3FF));
        } else {
            w += static_cast<wchar_t>(c);
        }
    }
    return w;
}

bool HasPlaceholder(const std::wstring& s) {
    for (size_t i = 0; i + 1 < s.size(); i++)
        if (s[i] == L'%' && ((s[i + 1] >= L'1' && s[i + 1] <= L'9') || s[i + 1] == L'#')) return true;
    return false;
}

bool Letter(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x52F) || (c >= 0x3040 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3);
}

bool Wordy(const std::wstring& s) {
    int run = 0;
    for (wchar_t c : s) {
        run = Letter(c) ? run + 1 : 0;
        if (run >= 2) return true;
    }
    return false;
}

// ---- the firmware's texts into ours

// The value tokens of a firmware text, in order (fwtext.h). Group 1 is the console's layout
// (paragraph spacing, indents, no-break hints): not a value.
std::vector<std::wstring> ValueTokens(const std::wstring& t) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] != 0xE000) continue;
        size_t end = t.find(wchar_t(0xE001), i);
        if (end == std::wstring::npos) break;
        std::wstring tok = t.substr(i, end - i + 1);
        if (tok.size() > 1 && tok[1] != L'1') out.push_back(tok);
        i = end;
    }
    return out;
}

// The console writes its buttons as characters of its own symbol font; NSW-Home spells them
// (ui.cpp's Spell does the same for the transcribed rows).
void SpellGlyph(wchar_t c, std::wstring& out) {
    static const wchar_t* const names[] = {L"A", L"B", L"X", L"Y", L"L", L"R", L"ZL", L"ZR"};
    if (c >= 0xE0E0 && c <= 0xE0E7) out += names[c - 0xE0E0];
    else if (c == 0xE0F4) out += L"HOME";
    else if (c < 0xE000 || c > 0xF8FF) out += c;
    // any other private character: a picture NSW-Home has no name for, left out
}

// A firmware text as NSW-Home shows it: values in the places the Spanish pattern `es` numbers
// (lined up through the Spanish firmware text `ref`), layout tags out, buttons spelt.
std::wstring FromFirmware(const std::wstring& es, const std::wstring& ref, const std::wstring& t) {
    std::vector<std::wstring> slots = ValueTokens(ref);
    size_t wanted = 0;  // how many values NSW-Home's text has (%1..%n)
    for (size_t i = 0; i + 1 < es.size(); i++)
        if (es[i] == L'%' && es[i + 1] >= L'1' && es[i + 1] <= L'9')
            wanted = (std::max)(wanted, static_cast<size_t>(es[i + 1] - L'0'));
    if (slots.size() > wanted) slots.resize(wanted);
    std::wstring s;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == 0xE000) {
            size_t end = t.find(wchar_t(0xE001), i);
            if (end == std::wstring::npos) break;
            std::wstring tok = t.substr(i, end - i + 1);
            for (size_t k = 0; k < slots.size() && k < 9; k++)
                if (slots[k] == tok) {
                    s += L'%';
                    s += static_cast<wchar_t>(L'1' + k);
                    break;
                }
            i = end;
            continue;
        }
        SpellGlyph(t[i], s);
    }
    // Line breaks the console puts in to fit its own boxes go when NSW-Home's text is one line.
    // (Between Chinese or Japanese characters, which have no spaces, it just goes.)
    if (es.find(L'\n') == std::wstring::npos) {
        std::wstring joined;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] != L'\n') {
                joined += s[i];
            } else if (!(i > 0 && s[i - 1] >= 0x3000 && s[i - 1] <= 0xFFEF) &&
                       !(i + 1 < s.size() && s[i + 1] >= 0x3000 && s[i + 1] <= 0xFFEF)) {
                joined += L' ';
            }
        }
        s = joined;
    }
    size_t a = s.find_first_not_of(L" 　");
    size_t b = s.find_last_not_of(L" 　");
    s = a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
    // Two spaces where a tag stood between words.
    std::wstring tidy;
    for (wchar_t c : s)
        if (!(c == L' ' && !tidy.empty() && tidy.back() == L' ')) tidy += c;
    return tidy;
}

// The console is called "Switch" in everything this program shows (the
// owner's choice for the published program), in every language. The
// firmware's own wording, read at run time, says "Nintendo Switch" (in Latin
// letters in every language, Japanese, Chinese and Korean included), so every
// translation loses the maker's name here, once, as it goes into the table:
// what Tr gives is then the same text for measuring, wrapping and drawing,
// and names that are data (a game called "Nintendo Switch Sports") never pass
// through here. Any run of spaces between the two words counts, the
// no-break ones the firmware puts there included (French has one), and none
// at all (a layout tag that stood between them). Japanese names the parental
// controls app "Nintendo みまもり Switch" where the other languages say
// "Nintendo Switch Parental Controls": it loses the maker's name the same
// way. "Nintendo eShop", "Nintendo Co., Ltd." and the rest stay.
std::wstring ShortConsoleName(std::wstring s) {
    static const std::wstring kMaker = L"Nintendo", kConsole = L"Switch", kParental = L"みまもり";
    auto spaces = [&s](size_t k) {
        while (k < s.size() && (s[k] == L' ' || s[k] == 0xA0 || s[k] == 0x2009 || s[k] == 0x202F || s[k] == 0x3000)) k++;
        return k;
    };
    for (size_t at = s.find(kMaker); at != std::wstring::npos; at = s.find(kMaker, at)) {
        const size_t k = spaces(at + kMaker.size());
        size_t c = k;
        if (s.compare(c, kParental.size(), kParental) == 0) c = spaces(c + kParental.size());
        if (s.compare(c, kConsole.size(), kConsole) == 0) {
            s.erase(at, k - at);
            at += c - k + kConsole.size();
        } else {
            at = k;
        }
    }
    return s;
}

void Add(const std::wstring& es, const Entry& e) {
    if (!HasPlaceholder(es)) {
        exact[es] = e;
        return;
    }
    Pattern p;
    p.out = e;
    std::wstring lit;
    for (size_t i = 0; i < es.size(); i++) {
        // %#1: a part that is a number and nothing else ("%#1 de %#2", not every "x de y")
        const bool number = es[i] == L'%' && i + 2 < es.size() && es[i + 1] == L'#' && es[i + 2] >= L'1' && es[i + 2] <= L'9';
        if (number || (es[i] == L'%' && i + 1 < es.size() && es[i + 1] >= L'1' && es[i + 1] <= L'9')) {
            if (number) i++;
            p.lits.push_back(lit);
            p.slot.push_back(es[i + 1] - L'0');
            p.number.push_back(number);
            lit.clear();
            i++;
        } else {
            lit += es[i];
        }
    }
    p.lits.push_back(lit);
    // Two parts with nothing between them cannot be told apart.
    for (size_t i = 1; i + 1 < p.lits.size(); i++)
        if (p.lits[i].empty()) return;
    patterns.push_back(std::move(p));
}

void Build(const std::vector<std::map<std::string, std::wstring>>& fw) {
    exact.clear();
    patterns.clear();
    memo.clear();
    memoSrc.clear();
    missing.clear();
    given.clear();
    if (current == kSpanish) return;
    const int col = kColumnOf[current];

    // Ours first, so that the firmware's wording replaces it wherever the firmware has the row.
    for (const OurLine* o = kOurs; o->es; o++) {
        int c = col;
        const char* t = o->t[c];
        while ((!t || !*t) && c >= 0 && kFallback[c] >= 0) {
            c = kFallback[c];
            t = o->t[c];
        }
        if ((!t || !*t) && c >= 0 && kFallback[c] == -2) t = o->es;
        if (t && *t) Add(Wide(o->es), {ShortConsoleName(Wide(t)), 'o'});
    }
    for (const char* const* k = kKeep; *k; k++) Add(Wide(*k), {Wide(*k), 'k'});
    if (fw.size() == 2) {
        const auto& ref = fw[0];
        const auto& mine = fw[1];
        for (const FwLine* f = kFw; f->es; f++) {
            auto r = ref.find(f->id);
            auto m = mine.find(f->id);
            if (r == ref.end() || m == mine.end()) continue;
            const std::wstring es = Wide(f->es);
            std::wstring t = FromFirmware(es, r->second, m->second);
            if (!t.empty()) Add(es, {ShortConsoleName(t), 'f'});
        }
    }
}

// ---- looking up

const Entry* Exact(const std::wstring& s) {
    auto it = exact.find(s);
    return it == exact.end() ? nullptr : &it->second;
}

// The part that starts at `at` runs up to an occurrence of lits[i] (the last literal: up to the
// end, which it must be). Parts are never empty.
bool MatchFrom(const Pattern& p, const std::wstring& s, size_t i, size_t at, std::vector<std::wstring>& parts) {
    const std::wstring& lit = p.lits[i];
    if (i + 1 == p.lits.size()) {
        if (s.size() < at + 1 + lit.size() || s.compare(s.size() - lit.size(), lit.size(), lit) != 0) return false;
        parts.push_back(s.substr(at, s.size() - lit.size() - at));
        return true;
    }
    for (size_t k = s.find(lit, at + 1); k != std::wstring::npos; k = s.find(lit, k + 1)) {
        parts.push_back(s.substr(at, k - at));
        if (MatchFrom(p, s, i + 1, k + lit.size(), parts)) return true;
        parts.pop_back();
    }
    return false;
}

bool Match(const Pattern& p, const std::wstring& s, std::vector<std::wstring>& parts) {
    const std::wstring& head = p.lits[0];
    return s.size() > head.size() && s.compare(0, head.size(), head) == 0 && MatchFrom(p, s, 1, head.size(), parts);
}

bool Lookup(const std::wstring& s, std::wstring& out, char& src);
void Remember(const std::wstring& s);

// Text that is data, not wording, wherever it stands (a part of a pattern, a line of a dialog): a
// name the program said to leave alone (Ignore), or a file's path.
bool Data(const std::wstring& s) {
    if (ignored.count(s)) return true;
    const bool drive = s.size() > 2 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/');
    if (drive || s.rfind(L"\\\\", 0) == 0 || s.find(L"://") != std::wstring::npos) return true;
    // A path or an address without spaces ("owner/repository", capitals allowed beside small
    // letters: "daniel-madrid-07/NSW-Home"; "ON/OFF" is still wording), a number in hexadecimal.
    if (s.find(L'/') != std::wstring::npos && s.find_first_of(L"abcdefghijklmnopqrstuvwxyz0123456789") != std::wstring::npos &&
        s.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/") == std::wstring::npos)
        return true;
    if (s.size() >= 8 && s.find_first_not_of(L"0123456789ABCDEFabcdef") == std::wstring::npos) return true;
    // A file's or a web site's name: "dev.old.exe", "example.com".
    bool host = s.find(L'.') != std::wstring::npos && s.front() != L'.' && s.back() != L'.';
    for (wchar_t c : s)
        host = host && ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_');
    if (host) return true;
    // Only names the table keeps as they are, between numbers and signs: "11/26/2026 10:17 PM",
    // "Ryujinx, Eden".
    std::wstring run;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i < s.size() && Letter(s[i])) {
            run += s[i];
            continue;
        }
        if (run.empty()) continue;
        const Entry* e = Exact(run);
        if (!e || e->src != 'k') return false;
        run.clear();
    }
    return true;
}

// A part of a text that a pattern matched: a text of its own where the table has it (whole, by a
// pattern again, or line by line); else kept as it is (a name, a number), and reported like any
// text without an entry when it is wording, so that a sentence a pattern took by mistake (any
// "<text>: <text> de <text>." fits "%1: %2 de %3.") still shows up in the test tool's list.
std::wstring PartText(const std::wstring& s) {
    std::wstring t;
    char src = 0;
    const bool found = Lookup(s, t, src);
    if (!found) Remember(s);
    return found || (src == 'u' && !t.empty()) ? t : s;
}

bool ByPattern(const std::wstring& s, std::wstring& out, char& src) {
    // Of the patterns that fit, the one with the most text of its own: it is the more particular
    // ("Captura %1 de %2" before "%#1 de %#2", a whole sentence before "%1 (%2)").
    const Pattern* best = nullptr;
    std::vector<std::wstring> bestParts;
    size_t bestLen = 0;
    for (const Pattern& p : patterns) {
        size_t len = 0;
        for (const std::wstring& l : p.lits) len += l.size();
        if (len == 0 || (best && len <= bestLen)) continue;
        std::vector<std::wstring> parts;
        if (!Match(p, s, parts) || parts.size() != p.slot.size()) continue;
        // A part runs over several lines only where the pattern itself has several: otherwise the
        // text is a pattern's line followed by others, looked up line by line below.
        bool lines = false;
        for (const std::wstring& l : p.lits) lines = lines || l.find(L'\n') != std::wstring::npos;
        bool digits = true;
        for (size_t k = 0; k < parts.size(); k++)
            if ((p.number[k] && parts[k].find_first_not_of(L"0123456789") != std::wstring::npos) ||
                (!lines && parts[k].find(L'\n') != std::wstring::npos))
                digits = false;
        if (!digits) continue;
        best = &p;
        bestParts = std::move(parts);
        bestLen = len;
    }
    if (!best) return false;
    std::wstring r;
    const std::wstring& t = best->out.text;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == L'%' && i + 1 < t.size() && t[i + 1] >= L'1' && t[i + 1] <= L'9') {
            int n = t[i + 1] - L'0';
            for (size_t k = 0; k < best->slot.size(); k++)
                if (best->slot[k] == n) r += PartText(bestParts[k]);
            i++;
        } else {
            r += t[i];
        }
    }
    out = r;
    src = best->out.src;
    return true;
}

bool Lookup(const std::wstring& s, std::wstring& out, char& src) {
    if (const Entry* e = Exact(s)) {
        out = e->text;
        src = e->src;
        return true;
    }
    if (ByPattern(s, out, src)) return true;
    // Several lines: each on its own (a title and its text joined by the caller).
    if (s.find(L'\n') != std::wstring::npos) {
        std::wstring r, line;
        bool any = false, all = true;
        char lineSrc = 'o';
        size_t at = 0;
        while (true) {
            size_t nl = s.find(L'\n', at);
            line = s.substr(at, nl == std::wstring::npos ? std::wstring::npos : nl - at);
            std::wstring t;
            char ls;
            if (!line.empty() && Lookup(line, t, ls)) {
                r += t;
                any = true;
                if (ls == 'f') lineSrc = 'f';
            } else {
                r += line;
                if (Wordy(line) && !Data(line)) all = false;
            }
            if (nl == std::wstring::npos) break;
            r += L'\n';
            at = nl + 1;
        }
        if (any) {
            out = r;
            src = all ? lineSrc : 'u';
            return all;  // partly translated: shown, but still reported as missing
        }
    }
    return false;
}

void Remember(const std::wstring& s) {
    if (!Wordy(s) || Data(s) || missing.count(s)) return;
    // (Wrapping by hand also joins the words of a line with single spaces.)
    auto single = [](const std::wstring& t) {
        std::wstring o;
        for (wchar_t c : t)
            if (!(c == L' ' && !o.empty() && o.back() == L' ')) o += c;
        return o;
    };
    const std::wstring s1 = single(s);
    for (const std::wstring& g : given)
        if (g.find(s) != std::wstring::npos || (g.find(L"  ") != std::wstring::npos && single(g).find(s1) != std::wstring::npos)) return;
    missing.insert(s);
    if (recordFile.empty()) return;
    std::string u;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c == L'\n') { u += "\\n"; continue; }
        if (c < 0x80) u += static_cast<char>(c);
        else if (c < 0x800) u += static_cast<char>(0xC0 | (c >> 6)), u += static_cast<char>(0x80 | (c & 63));
        else if (c < 0x10000)
            u += static_cast<char>(0xE0 | (c >> 12)), u += static_cast<char>(0x80 | ((c >> 6) & 63)),
                u += static_cast<char>(0x80 | (c & 63));
        else
            u += static_cast<char>(0xF0 | (c >> 18)), u += static_cast<char>(0x80 | ((c >> 12) & 63)),
                u += static_cast<char>(0x80 | ((c >> 6) & 63)), u += static_cast<char>(0x80 | (c & 63));
    }
    u += "\n";
#ifdef _WIN32
    FILE* f = nullptr;
    if (_wfopen_s(&f, recordFile.c_str(), L"ab") || !f) return;
#else
    std::string path;
    for (wchar_t c : recordFile) path += static_cast<char>(c);
    FILE* f = fopen(path.c_str(), "ab");
    if (!f) return;
#endif
    fwrite(u.data(), 1, u.size(), f);
    fclose(f);
}

// ---- fonts

// A system font with the script's glyphs, for when the firmware's is not there. The standard
// font's Han characters are Japanese forms: Chinese and Korean text must not come from it.
const wchar_t* SystemFamily(int font) {
#ifdef _WIN32
    return font == 3 ? L"Malgun Gothic" : font == 4 ? L"Microsoft JhengHei UI" : font == 5 ? L"Microsoft YaHei UI" : L"";
#else
    return font == 3 ? L"Noto Sans CJK KR" : font == 4 ? L"Noto Sans CJK TC" : font == 5 ? L"Noto Sans CJK SC" : L"";
#endif
}

void ApplyFont(const std::wstring& fwDir, const std::wstring& keysDir) {
    const Language& l = kLanguages[current];
    std::vector<uint8_t> data;
    if (l.font && !fwDir.empty() && !keysDir.empty()) {
        fwfont::Font f;
        std::string err;
        if (fwfont::Load(fwDir, keysDir, l.font, f, err)) data = std::move(f.data);
    }
    gfx::SetLanguageFont(std::move(data), l.font ? SystemFamily(l.font) : L"", l.locale);
}

}  // namespace

const std::vector<Language>& All() {
    return kLanguages;
}

int Find(const std::string& code) {
    for (size_t i = 0; i < kLanguages.size(); i++)
        if (code == kLanguages[i].code) return static_cast<int>(i);
    return -1;
}

int Current() {
    return current;
}

const Language& Cur() {
    return kLanguages[current];
}

bool Spanish() {
    return current == kSpanish;
}

void Use(int index, const std::wstring& firmwareDir, const std::wstring& keysDir) {
    if (index < 0 || index >= static_cast<int>(kLanguages.size())) index = 1;
    if (loaded && index == loadedIndex && firmwareDir == loadedFw && keysDir == loadedKeys) return;
    const bool fontChange = !loaded || kLanguages[index].font != kLanguages[current].font ||
                            kLanguages[index].locale != kLanguages[current].locale || firmwareDir != loadedFw;
    current = index;
    loaded = true;
    loadedIndex = index;
    loadedFw = firmwareDir;
    loadedKeys = keysDir;
    haveFw = false;
    fwError.clear();
    std::vector<std::map<std::string, std::wstring>> fw;
    if (index != kSpanish && !firmwareDir.empty() && !keysDir.empty()) {
        if (fwtext::LoadMany(firmwareDir, keysDir, {kLanguages[kSpanish].code, kLanguages[index].code}, fw, fwError) &&
            fw.size() == 2 && !fw[1].empty())
            haveFw = true;
        else
            fw.clear();
    }
    Build(fw);
    if (fontChange) ApplyFont(firmwareDir, keysDir);
}

bool HaveFirmware() {
    return haveFw;
}

const std::string& FirmwareError() {
    return fwError;
}

Verbatim::Verbatim() {
    verbatim++;
}

Verbatim::~Verbatim() {
    verbatim--;
}

const std::wstring& Tr(const std::wstring& es) {
    if (current == kSpanish || es.empty() || verbatim > 0) return es;
    auto it = memo.find(es);
    if (it != memo.end()) return it->second;
    // Texts that change all the time (a clock, a counter) would pile up otherwise.
    if (memo.size() > 6000) {
        memo.clear();
        memoSrc.clear();
    }
    std::wstring out;
    char src = 'u';
    if (!Lookup(es, out, src)) {
        if (src != 'u' || out.empty()) out = es;
        src = 'u';
        Remember(es);
    }
    memoSrc[es] = src;
    if (out != es) {
        if (given.size() > 4000) given.clear();
        given.push_back(out);
    }
    return memo[es] = std::move(out);
}

char Source(const std::wstring& es) {
    if (current == kSpanish) return '-';
    Tr(es);
    auto it = memoSrc.find(es);
    return it == memoSrc.end() ? 'u' : it->second;
}

const std::set<std::wstring>& Missing() {
    return missing;
}

void RecordTo(const std::wstring& file) {
    recordFile = file;
}

void RecordFromEnv() {
    for (const char* name : {"NSWHOME_LANGMISS", "DOCKSIDE_LANGMISS"}) { // the second: historical name, still honoured
#ifdef _WIN32
        wchar_t* v = nullptr;
        size_t n = 0;
        const std::wstring wide(name, name + strlen(name));
        if (!_wdupenv_s(&v, &n, wide.c_str()) && v && *v) recordFile = v;
        free(v);
#else
        if (const char* v = std::getenv(name))
            for (recordFile.clear(); *v; v++) recordFile += static_cast<wchar_t>(*v);
#endif
        if (!recordFile.empty()) return;
    }
}

void Ignore(const std::wstring& s) {
    if (ignored.size() < 20000) ignored.insert(s);
}

namespace {
bool Ideographic(wchar_t c) {
    return (c >= 0x3000 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF);
}
bool NoLineStart(wchar_t c) {
    static const wchar_t kClose[] = L"、。，．・：；？！」』）〕］｝〉》】ー…ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ々,.:;?!)]}%";
    for (const wchar_t* k = kClose; *k; k++)
        if (*k == c) return true;
    return false;
}
bool NoLineEnd(wchar_t c) {
    return c == L'「' || c == L'『' || c == L'（' || c == L'〔' || c == L'［' || c == L'｛' || c == L'〈' || c == L'《' ||
           c == L'【' || c == L'(' || c == L'“';
}
}  // namespace

std::vector<Word> Words(const std::wstring& line) {
    std::vector<Word> out;
    bool space = false;
    std::wstring cur;
    auto flush = [&] {
        if (!cur.empty()) out.push_back({cur, space});
        cur.clear();
        space = false;
    };
    for (size_t i = 0; i < line.size(); i++) {
        const wchar_t c = line[i];
        if (c == L' ') {
            flush();
            space = !out.empty();
            continue;
        }
        // A break before this character: between two characters of which one is ideographic.
        if (!cur.empty() && (Ideographic(c) || Ideographic(cur.back())) && !NoLineStart(c) && !NoLineEnd(cur.back())) {
            out.push_back({cur, space});
            cur.clear();
            space = false;
        }
        cur += c;
    }
    flush();
    return out;
}

std::wstring Clock(int hour, int minute, bool h24, bool padHour, std::wstring& period) {
    wchar_t b[16];
    period.clear();
    if (h24) {
        std::swprintf(b, 16, padHour ? L"%02d:%02d" : L"%d:%02d", hour, minute);
        return b;
    }
    const int h = hour % 12;
    std::swprintf(b, 16, L"%d:%02d", h ? h : 12, minute);
    const bool pm = hour >= 12;
    switch (kLanguages[current].font ? kLanguages[current].font : (current == 0 ? 1 : 0)) {
    case 1: period = pm ? L"午後" : L"午前"; break;  // Japanese
    case 3: period = pm ? L"오후" : L"오전"; break;
    case 4:
    case 5: period = pm ? L"下午" : L"上午"; break;
    default: period = pm ? L"PM" : L"AM"; break;
    }
    return b;
}

bool PeriodFirst() {
    return current == 0 || kLanguages[current].font != 0;
}

std::wstring Date(int year, int month, int day) {
    const Language& l = kLanguages[current];
    wchar_t b[40];
    const wchar_t* s = l.dateSep;
    if (l.dateOrder == 2) std::swprintf(b, 40, L"%04d%ls%02d%ls%02d", year, s, month, s, day);
    else if (l.dateOrder == 1) std::swprintf(b, 40, L"%02d%ls%02d%ls%04d", month, s, day, s, year);
    else std::swprintf(b, 40, L"%02d%ls%02d%ls%04d", day, s, month, s, year);
    return b;
}

std::wstring DayFirst(const std::wstring& text, bool h24) {
    // "dd/mm/yyyy" ('-' or '.' between the parts as well), and perhaps " hh:mm" after it.
    size_t i = 0;
    auto number = [&](int& out) {
        const size_t start = i;
        out = 0;
        while (i < text.size() && i - start < 4 && text[i] >= L'0' && text[i] <= L'9') out = out * 10 + (text[i++] - L'0');
        return i > start;
    };
    auto sep = [&](wchar_t c) {
        if (i >= text.size() || text[i] != c) return false;
        i++;
        return true;
    };
    int d = 0, m = 0, y = 0, hh = -1, mm = -1;
    if (!number(d) || i >= text.size()) return text;
    const wchar_t s = text[i];
    if ((s != L'/' && s != L'-' && s != L'.') || !sep(s) || !number(m) || !sep(s) || !number(y)) return text;
    if (i < text.size() && !(sep(L' ') && number(hh) && sep(L':') && number(mm) && i == text.size())) return text;
    if (m < 1 || m > 12 || d < 1 || d > 31) return text;
    std::wstring out = Date(y, m, d);
    if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
        std::wstring period, clock = Clock(hh, mm, h24, true, period);
        if (!period.empty()) clock = PeriodFirst() ? period + L" " + clock : clock + L" " + period;
        out += L" " + clock;
    }
    return out;
}

std::wstring Number(double value, int decimals) {
    wchar_t b[48];
    std::swprintf(b, 48, L"%.*f", decimals, value);
    std::wstring s = b;
    for (wchar_t& c : s)
        if (c == L'.') c = kLanguages[current].decimal;
    return s;
}

}  // namespace lang
