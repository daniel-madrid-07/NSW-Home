// The console's own wording, read at runtime from the user's own installed firmware with the
// user's own prod.keys, as fwsound and fwfont read the sounds and the font. Nothing is written to
// disk here and nothing of it is kept in the program: the menu asks for one language's messages
// when that language is chosen and holds them in memory only.
//
// Where the messages are (verified on firmware 19.0.0): every system applet keeps, in the RomFS
// of its program NCA, one folder per language, message/<Region><lang>/<name>.msbt.szs (Yaz0 over
// a MsgStdBn file). The HOME Menu (0100000000001000) holds qlaunch, setting (System Settings),
// option (a game's options), common, notification, interrupt, cldr (date and time patterns) and
// a few more; the user page (0100000000001013) holds myPage; the album, the player selection,
// the controller applet and the quick settings overlay hold their own.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace fwtext {

// The language folders, in the console's own codes: "JPja", "USen", "EUen", "EUfr", "USfr",
// "EUde", "EUit", "EUes", "USes", "CNzh", "TWzh", "KRko", "EUnl", "EUpt", "USpt", "EUru".
//
// `out` maps "<file>/<label>" ("setting/<label>", "myPage/<label>", ...) to the message's text.
// A message's formatting tags are taken out (ruby readings, colours, sizes); a tag that stands
// for a value filled in at run time (a name, a number) is left in the text as a token
// U+E000 <group> <type> <hex of its parameters> U+E001, so that a caller can line up the values of
// two languages by comparing tokens. When two applets hold a file of the same name, the first
// in the order above wins (the HOME Menu's).
//
// Never throws; returns false and a short error code on failure: NO_KEYS, MISSING_KEY,
// BAD_KEYS, NO_FIRMWARE, NO_MENU_NCA (no HOME Menu in the firmware), NO_LANGUAGE (the HOME
// Menu has no folder of that name), FILE_ERROR, BAD_FORMAT, BAD_NCA, UNSUPPORTED, CRYPTO, INTERNAL.
// Applets other than the HOME Menu are optional: one that is missing is skipped.
bool Load(const std::wstring& registeredDir, const std::wstring& keysDir, const std::string& folder,
          std::map<std::string, std::wstring>& out, std::string& err);

// Several languages in one pass over the firmware (the NCA headers are read once). `out[i]`
// receives folders[i]'s messages; a language the firmware lacks is left empty.
bool LoadMany(const std::wstring& registeredDir, const std::wstring& keysDir, const std::vector<std::string>& folders,
              std::vector<std::map<std::string, std::wstring>>& out, std::string& err);

// For tests: whether a text holds value tokens, and the text with each token replaced by `with`.
std::wstring Plain(const std::wstring& text, const std::wstring& with);

}  // namespace fwtext
