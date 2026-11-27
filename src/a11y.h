// What Settings > Accesibilidad needs from the system: a voice that reads the
// menu aloud, and the menu's own sounds folded down to one channel.
// Windows: the speech engine every Windows has had since XP (SAPI 5), which
// needs nothing installed. Elsewhere speech reports "not available" and the
// rows that depend on it stay as the console-only rows they were.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace a11y {

struct Voice {
    std::wstring id;       // what the system knows it by
    std::wstring name;     // "Microsoft Helena Desktop"
    std::wstring language; // "es-ES", or empty when the voice does not say
};

// False where there is no speech engine (or no voice installed).
bool SpeechAvailable();
// The installed voices, Spanish ones first (the menu is in Spanish), then in
// the system's order. Read once.
const std::vector<Voice>& Voices();

// Says `text` now, cutting off whatever was being said. Returns at once: the
// speaking happens on a thread of its own. `rate` is -10 (slowest) to 10,
// 0 being the voice's normal pace; `volume` 0 to 100. An unknown or empty
// voice id means the first of Voices().
// Never makes a sound in an offscreen test run ("--shot", "--shotseq" on the
// command line) or with NSWHOME_A11Y_DRY set: the request is only logged.
void Speak(const std::wstring& text, const std::wstring& voiceId, int rate, int volume);
// Stops talking.
void Silence();
// The same speech as a complete 16-bit mono WAV in memory instead of out of
// the loudspeakers (nothing is played). For tests. False when it failed.
bool SpeakToWav(const std::wstring& text, const std::wstring& voiceId, int rate, int volume, std::vector<uint8_t>& wav);

// With NSWHOME_A11Y_LOG=<file> in the environment, every Speak() request adds
// a line to that file: "speak" or "dry", rate, volume, voice name, text
// (tab-separated, UTF-8). Log() adds a line of the caller's own.
void Log(const std::string& line);

// Menu sounds in one channel. Mono(wav) hands back `wav` itself while the
// setting is off or the sound already has one channel; otherwise a copy with
// the channels averaged, made once per sound and kept (the caller plays from
// memory, so the bytes must stay where they are).
void SetMono(bool on);
const std::vector<uint8_t>& Mono(const std::vector<uint8_t>& wav);

} // namespace a11y
