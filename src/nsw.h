// Nintendo Switch dump metadata reader (.nsp, .xci best-effort).
// Reads title name, publisher, display version, title ID and icon using the
// user's own prod.keys / title.keys. Only positional reads; a call touches a few
// hundred KB of the dump.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nsw {

struct Title {
    std::string titleId;                    // 16 lowercase hex chars
    std::wstring name, publisher, version;  // version = NACP display version
    std::vector<uint8_t> icon;              // JPEG, may be empty
};

// keysDir holds prod.keys/title.keys. languages = NACP language indices in preference order
// (0 AmericanEnglish, 1 BritishEnglish, 2 Japanese, 3 French, 4 German, 5 LatinAmericanSpanish,
//  6 Spanish, 7 CanadianFrench, 8 Italian, 9 Dutch, 10 Portuguese, 11 Russian, 12 Korean,
//  13 TraditionalChinese, 14 SimplifiedChinese, 15 BrazilianPortuguese).
// Falls back to the first non-empty entry / first icon present.
// Returns false and fills err on failure. Never throws. Error codes:
//   NO_KEYS, MISSING_KEY, NO_TITLE_KEY, FILE_ERROR, BAD_FORMAT, BAD_NCA,
//   NO_CONTROL_NCA, NO_NACP, UNSUPPORTED, CRYPTO, INTERNAL
bool ReadTitle(const std::wstring& file, const std::wstring& keysDir, const std::vector<int>& languages,
               Title& out, std::string& err);

}  // namespace nsw
