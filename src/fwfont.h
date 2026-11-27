// The console's shared system fonts, read at runtime from the user's own installed firmware
// with the user's own prod.keys. Nothing is written to disk here; the caller decides whether
// to cache the decoded font (only ever under %APPDATA%\NSW-Home\, never in the project).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fwfont {

// `family` is the name-table family (Windows/English name ID 16 if present, else ID 1). Verified
// with tools\dump_fwfont on firmware 19.0.0: DirectWrite reports and resolves exactly this name
// only in a TYPOGRAPHIC-model collection:
//     IDWriteFactory6::CreateFontCollectionFromFontSet(set, DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &c)
// The older IDWriteFactory3/5 overload builds a weight-stretch-style collection, which renames
// these fonts ("nintendo_udsg-r_std_003" -> "nintendo_udsg std_003", "NintendoExt003" ->
// "NintendoExt003 R") and does NOT find `family`. With that overload, take the name from
// collection->GetFontFamily(0)->GetFamilyNames() instead (one font per collection).
// Both extended fonts carry the same family name: do not put 1 and 2 in the same font set.
struct Font {
    std::wstring family;        // e.g. "nintendo_udsg-r_std_003", "NintendoExt003"
    std::vector<uint8_t> data;  // complete .ttf/.otf, ready for a DirectWrite in-memory font file
    std::string source;         // RomFS file it came from, e.g. "nintendo_udsg-r_std_003.bfttf" (for logs)
};

// registeredDir = ...\bis\system\Contents\registered, keysDir holds prod.keys.
// which: 0 = standard (Latin/Japanese UI text)        title 0100000000000811, nintendo_udsg-r_std_*.bfttf
//        1 = extended (button glyph symbols, U+E0xx)   title 0100000000000810, nintendo_ext_*.bfttf
//        2 = extended 2 (more symbols, U+E1xx)         title 0100000000000810, nintendo_ext2_*.bfttf
//        3 = Korean                                    title 0100000000000812, nintendo_udsg-r_ko_*.bfttf
//        4 = Chinese, traditional                      title 0100000000000813, nintendo_udjxh-db_zh-tw_*.bfttf
//        5 = Chinese, simplified                       title 0100000000000814, nintendo_udsg-r_org_zh-cn_*.bfttf
//            (the same title's nintendo_udsg-r_ext_zh-cn_*.bfttf, rare characters, is not offered)
// Never throws; returns false and a short error code on failure:
//   NO_KEYS       prod.keys missing or empty
//   MISSING_KEY   a needed key (header_key, key_area_key_system_*) is not in prod.keys
//   BAD_KEYS      no NCA header could be decrypted with header_key
//   NO_FIRMWARE   registeredDir missing or empty
//   NO_FONT_NCA   firmware present, but the shared font title is not in it
//   NO_FONT       the title holds no matching .bfttf
//   BAD_FONT      the .bfttf did not decode to a valid TrueType/OpenType file
//   BAD_ARG       `which` out of range
//   FILE_ERROR, BAD_FORMAT, BAD_NCA, UNSUPPORTED, CRYPTO, INTERNAL
bool Load(const std::wstring& registeredDir, const std::wstring& keysDir, int which, Font& out, std::string& err);

}  // namespace fwfont
