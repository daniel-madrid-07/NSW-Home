// HOME Menu sound effects, read at runtime from the user's own installed firmware
// (system title 0100000000001000, "qlaunch") with the user's own prod.keys.
// Nothing is written to disk here; the caller decides whether to cache the WAVs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fwsound {

struct Sound {
    std::string name;          // name from the archive's string table, e.g. "SeBtnDecide"
    std::vector<uint8_t> wav;  // complete RIFF WAV, 16-bit PCM, mono or stereo, usually 48 kHz
};

// registeredDir = ...\bis\system\Contents\registered, keysDir holds prod.keys.
// Fills `out` with every sound effect in the HOME Menu archive, in archive order. The level the
// archive assigns to each sound is already applied, so all of them play back at the console's
// relative loudness with no extra gain (entries the archive lists at level 0 keep the level of
// their wave). Looped waves are delivered as one pass. Entries whose wave is pure silence and
// the archive's empty sequence stubs are left out.
// Never throws; returns false and a short error code on failure:
//   NO_KEYS       prod.keys missing or empty
//   MISSING_KEY   a needed key (header_key, key_area_key_*) is not in prod.keys
//   BAD_KEYS      no NCA header could be decrypted with header_key
//   NO_FIRMWARE   registeredDir missing or empty
//   NO_MENU_NCA   firmware present, but no HOME Menu program content in it
//   NO_ARCHIVE    the HOME Menu has no .bfsar
//   BAD_ARCHIVE   the sound archive could not be parsed
//   NO_SOUNDS     the archive parsed, but no wave sound could be decoded
//   FILE_ERROR, BAD_FORMAT, BAD_NCA, UNSUPPORTED, CRYPTO, INTERNAL
bool LoadMenuSounds(const std::wstring& registeredDir, const std::wstring& keysDir, std::vector<Sound>& out,
                    std::string& err);

// Display version of the installed firmware ("19.0.0"), from system title 0100000000000809.
// Same error codes; NO_VERSION if that title is missing.
bool FirmwareVersion(const std::wstring& registeredDir, const std::wstring& keysDir, std::string& version,
                     std::string& err);

}  // namespace fwsound
