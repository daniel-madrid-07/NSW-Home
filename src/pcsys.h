// What some of the console's Settings rows mean on a PC, asked of the system:
// the state of the network connection and a connection test, the default
// sound devices, the microphone's level, a test tone, the Bluetooth page of
// the system's own settings, and the running game's entry in the sound mixer.
//
// Windows only. On every other system the same functions exist and report
// "not available" (Available() is false, readings come back empty), and the
// rows that depend on them stay as they were.
//
// Nothing read here is ever written to a file or a log: the network values
// identify the owner's machine and home network.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pcsys {

bool Available();

// ---- network ---------------------------------------------------------------

struct Net {
    int kind = 0; // 0 no connection, 1 Wi-Fi, 2 cable
    std::wstring mac, device; // device: the Wi-Fi network's name, or the adapter's for a cable
    std::wstring security, band, channel; // Wi-Fi only
    int signal = -1;                      // Wi-Fi only, 0..100
    std::wstring ipv4, subnet, gateway4, dns1_4, dns2_4, ipv6, gateway6, dns1_6, dns2_6;
};
// The connection the PC reaches the internet through, read afresh.
Net ReadNet();

// The address asked by the connection test: the one Windows itself asks to
// decide whether there is internet (its "network connectivity status
// indicator"). A plain HTTP GET with no cookies, no query and nothing about
// the user; the answer is a fixed line of text.
extern const wchar_t* const kProbeHost; // "www.msftconnecttest.com"
extern const wchar_t* const kProbePath; // "/connecttest.txt"
// Blocks for up to a few seconds. 0: the expected answer came; 1: no answer;
// 2: something else answered (a hotel-style sign-in page); -1: not available.
int Probe();

// ---- Bluetooth ---------------------------------------------------------------

// Opens the Bluetooth page of the system's settings. False if it could not.
bool OpenBluetoothSettings();

// ---- sound devices -----------------------------------------------------------

struct Audio {
    bool ok = false;
    std::wstring output, input; // names of the default playback and recording devices; empty if none
    bool headphones = false;    // the playback device says it is headphones or a headset
};
Audio ReadAudio();

// Level of the default recording device, 0..1.
bool MicVolume(float& level);
bool SetMicVolume(float level);

// A level meter on the default recording device. Opens the microphone, which
// the system shows to the user, so: only on request, and stopped when the
// page closes. Nothing is kept: what arrives is thrown away unread, and the
// level is the system's own peak meter. Level is 0..1, or -1 when not running.
bool MicMeterStart();
float MicMeterLevel();
void MicMeterStop();

// ---- test tone ---------------------------------------------------------------

// A tone on the left channel, a pause, the same on the right (seconds).
constexpr double kToneLeftFrom = 0.35, kToneLeftTo = 1.35, kToneRightFrom = 1.70, kToneRightTo = 2.70, kToneLength = 2.90;
// The tone as a WAV file in memory (16-bit stereo). The same on every system.
std::vector<uint8_t> ToneWav();
bool PlayTone();
void StopTone();

// ---- the running game's sound ------------------------------------------------

// What "Bajar el volumen máximo de los auriculares" lowers the game to in the
// mixer: half, which is 6 dB down. The mixer has no decibels of sound
// pressure to offer (it does not know the headphones), so this is a fixed,
// clearly audible step rather than a safety figure.
constexpr float kHeadphoneCap = 0.5f;

// The one call main.cpp makes, right after starting the emulator: from then
// until that process ends its mixer entry is watched. Reads the two switches
// from the configuration.
void GameStarted(unsigned long pid);
// The switches were changed in the menu.
void SetAudioPolicy(bool muteOnUnplug, bool limitHeadphones);
// The game is muted because the headphones went away.
bool GameMuted();
void GameUnmute();
// Stops watching (the watcher also stops by itself when the process ends).
void GameStopped();

// ---- for the tests -----------------------------------------------------------

// Stands in for the default playback device's kind: -1 the real one, 0 not
// headphones, 1 headphones.
void TestHeadphones(int state);
// Delivers the system's "default device changed" notification by hand.
void TestDeviceChanged();
// One process's mixer entry, read only. False when it has none.
bool SessionState(unsigned long pid, bool& muted, float& volume);

} // namespace pcsys
