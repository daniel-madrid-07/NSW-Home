// The menu's language. NSW-Home's own text is written in Spanish (Spain) throughout the sources;
// the words a screen draws are looked up here, at the moment they are drawn or measured, in the
// table of the language chosen in Settings > Consola > Idioma.
//
// Where a translation comes from, in this order:
//   1. the console's own wording, read at runtime from the owner's firmware (fwtext.h): the table
//      in lang_strings.inc maps a Spanish text of NSW-Home's to the id of the firmware message
//      that is the same row or dialog (F lines). Only ids are in our sources, never the texts.
//   2. our own translation, written for NSW-Home (O lines), for everything the firmware has no
//      message for (the second console's own rows, NSW-Home's own pages), and for the
//      firmware's rows when there is no firmware to read them from.
//   3. the Spanish text itself (nothing found; `Missing` lists these for the test tool).
//
// Text built at run time from parts is matched against patterns: a Spanish entry with %1..%9 in
// it ("Versión actual: %1") stands for every text that has those words around anything; the
// parts are put into the translation in the places its own %1..%9 mark, each part looked up on
// its own first. %#1..%#9 match only digits ("%#1 de %#2": "1 de 3", but not every "x de y").
// A text of several lines that has no entry as a whole is looked up line by line.
//
// Portable C++ (the firmware reading is nsw_core's); the fonts go through gfx.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace lang {

struct Language {
    const char* code;        // the firmware's folder of messages for it: "USen", "EUes", "JPja"...
    const wchar_t* name;     // as the console lists it, in its own language
    const char* ryujinx;     // Ryujinx's name for the console's language ("AmericanEnglish")
    const char* number;      // the console's own language number, as the yuzu family stores it
    const wchar_t* locale;   // BCP-47: steers the system's font fallback and line breaking
    int font;                // the console's shared font for its script (fwfont::Load's `which`); 0 = standard
    bool clock24;            // its usual clock when a language is chosen: the console's defaults
    wchar_t decimal;         // decimal separator
    int dateOrder;           // 0 day/month/year, 1 month/day/year, 2 year/month/day
    const wchar_t* dateSep;  // between the parts of a date
    const wchar_t* news;     // the console maker's news page for its region (the bar's News button)
};

// Every language the console offers, in the order its own list shows them.
const std::vector<Language>& All();
// Index in All() of a firmware code ("USen"), -1 if unknown.
int Find(const std::string& code);
int Current();
const Language& Cur();
// The menu is in Spanish (Spain), the language the sources are written in: nothing to look up.
bool Spanish();

// Makes All()[index] the menu's language: that language's console messages are read from the
// firmware (with the Spanish ones they are lined up against) and its font is put in place.
// `firmwareDir` / `keysDir` may be empty (then only our own translations are used). Cheap when
// nothing changes; about a tenth of a second when it does.
void Use(int index, const std::wstring& firmwareDir, const std::wstring& keysDir);
// Whether the console's own wording was read for the language in use.
bool HaveFirmware();
// The firmware's error code when it could not be read ("" when it was, or was not asked for).
const std::string& FirmwareError();

// The text to draw for a Spanish text of NSW-Home's. The reference stays valid until the next
// call to Use (or until enough distinct texts have been asked for that the memory is cleared:
// copy it if it has to be kept across frames).
const std::wstring& Tr(const std::wstring& es);

// While one of these lives, Tr gives every text back as it is: for code that measures pieces of
// a text it has already translated whole (word wrapping by hand).
struct Verbatim {
    Verbatim();
    ~Verbatim();
    Verbatim(const Verbatim&) = delete;
    Verbatim& operator=(const Verbatim&) = delete;
};

// Where Tr's answer for `es` comes from, for the listing of every row: 'f' the firmware, 'o' our
// own translation, 'k' the same in every language (a name), 'u' untranslated, '-' Spanish in use.
char Source(const std::wstring& es);

// Strings asked for that had no entry, since the language was last chosen (the test tool's
// list). A text without two letters in a row is never counted.
const std::set<std::wstring>& Missing();
// Also append each new one, as one UTF-8 line, to this file ("" = off).
void RecordTo(const std::wstring& file);
// RecordTo the file named by the environment variable NSWHOME_LANGMISS, if it is set.
void RecordFromEnv();
// A text that is data, not NSW-Home's wording (a game's or a user's name): not to be reported.
void Ignore(const std::wstring& s);

// ---- line breaking
// The places a line of text may be broken: at spaces, and in Chinese and Japanese text, which
// has none, between any two characters (but never before a closing mark such as 。 or 」, nor
// after an opening one). `space`: the word had a space before it.
struct Word {
    std::wstring text;
    bool space;
};
std::vector<Word> Words(const std::wstring& line);

// ---- dates, times and numbers in the language's way
// Time of day; with h24 false, a 12-hour clock and `period` set to its "AM" / "PM" word ("" otherwise).
std::wstring Clock(int hour, int minute, bool h24, bool padHour, std::wstring& period);
// Whether the language writes the period word before the time ("午後 3:45").
bool PeriodFirst();
std::wstring Date(int year, int month, int day);
// A date that was written down day first ("26/11/2026", "26-11-2026 22:04": a file's, a figure's),
// in the language's order, with its time of day in the given clock; anything else as it is.
std::wstring DayFirst(const std::wstring& text, bool h24);
// A number with `decimals` places and the language's decimal separator.
std::wstring Number(double value, int decimals);

}  // namespace lang
