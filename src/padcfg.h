// Telling an emulator who is player 1, 2, 3, 4.
//
// Both families keep one controller entry per player in their settings file,
// with the pad named by SDL's identifier and a whole button mapping attached.
// This module never writes a mapping or an identifier of its own making: it
// only MOVES entries that are already in the file (set up once by the user in
// the emulator) to the player numbers NSW-Home's order gives their pads.
//
//   Ryujinx family, Config.json: each object of "input_config" has "backend",
//   "id" and "player_index". Only the text of "player_index" values changes.
//   An entry whose pad is not connected does nothing in the emulator, so there
//   is no "not connected" to write.
//
//   yuzu family, qt-config.ini, [Controls]: every line "player_N_..." (with its
//   "\default" companion) except "player_N_connected" belongs to the entry of
//   player N; moving an entry renames that N on its lines, in place. Then
//   "player_N_connected" is made true for players that have a pad and false
//   for the rest (the pair "\default=false" + value, and only where the value
//   in effect differs: its built-in default is true for player 0 only).
//
// A pad of NSW-Home's that has no entry in the file is left for the user to
// set up in the emulator: nothing is written for it and it is reported in
// `missing`. Entries nobody asked for keep their player number, or take the
// lowest free one when theirs was given away.
//
// The guarantees are ryucfg's: every other byte of the file stays as it was;
// writes go through a temporary file; the first write keeps
// "<file>.dockside-backup"; never call this while the emulator runs.
#pragma once
#include "pads.h"
#include <string>
#include <vector>

namespace padcfg {

// One of NSW-Home's players.
struct Want {
    int player = 0; // 1..8
    enum class What { Keyboard, Xbox, Sdl, Unknown } what = What::Unknown;
    // What::Sdl: SDL's GUID as derived by pads::SdlGuid.
    uint8_t guid[16] = {};
    bool crcKnown = false;  // the name part of the GUID could be derived
    bool bluetooth = false; // (SDL 3 puts the bus in the GUID; SDL 2 does not)
    int index = 0;          // which of several pads with that GUID
};

// From pads::List(): one Want per pad that has a player number. Joy-Con come
// out as Unknown: each emulator names them through a driver of its own.
std::vector<Want> WantsFrom(const std::vector<pads::Pad>& pads);

struct Result {
    bool ok = false;      // the file was read and understood
    bool changed = false; // something was (or, checking only, would be) written
    std::vector<int> missing; // players whose pad has no entry in the file
};

// `checkOnly`: work everything out but write nothing.
Result ApplyRyujinx(const std::wstring& configJson, const std::vector<Want>& wants, bool checkOnly = false);
Result ApplyYuzu(const std::wstring& qtConfigIni, const std::vector<Want>& wants, bool checkOnly = false);

// ---- single values of the same files ---------------------------------------------
//
// The same rules: only the text of the value in question changes, the first
// write keeps the backup, never while the emulator runs. Nothing is ever
// added: where the file has no such value (the emulator never wrote it) the
// getters say so and the setters fail without touching the file. Setting what
// is already there succeeds without a write.
//
// Where each name comes from (the owner's own files in <data>, and the
// emulators' sources):
//   Ryujinx family, Config.json
//     input_config[].rumble.enable_rumble      RumbleConfigController.EnableRumble; NpadController.UpdateRumble
//     input_config[].deadzone_left / _right    GenericControllerInputConfig.DeadzoneLeft / DeadzoneRight, 0..1
//     enable_keyboard, enable_mouse            ConfigurationFileFormat.EnableKeyboard / EnableMouse ("direct access")
//   yuzu family, qt-config.ini, [Controls] (pairs "name\default" + "name",
//   written as the emulator writes them: the flag says "this is the built-in value")
//     vibration_enabled                        common/settings.h, built-in true; checked before every vibration
//                                              in hid_core/frontend/emulated_controller.cpp
//     keyboard_enabled, mouse_enabled          common/settings.h, built-in false (the emulated USB keyboard / mouse)
//     mouse_panning_x_sensitivity, _y_         common/settings.h, 1..100, built-in 50; input_common/drivers/mouse.cpp
//     player_N_lstick / _rstick "...,deadzone:0.150000"   input_common/input_poller.cpp, 0..1
enum class Family { Ryujinx, Yuzu };

// Vibration. Ryujinx keeps a switch in each pad entry (a keyboard entry has
// none): `entries` of them, `on` of which are on; Set gives all of them the
// same state. The yuzu family has one switch for all pads, which is what is
// read and written there (entries = 1); its per-player switches are left as
// the user set them in the emulator.
struct Rumble {
    bool ok = false; // the file was read and understood
    int entries = 0, on = 0;
};
Rumble RumbleGet(const std::wstring& file, Family family);
bool RumbleSet(const std::wstring& file, Family family, bool on);

// Whether games are handed the PC's keyboard / mouse as a USB one.
enum class Switch { Keyboard, Mouse };
int SwitchGet(const std::wstring& file, Family family, Switch which); // 1, 0, or -1: not in the file
bool SwitchSet(const std::wstring& file, Family family, Switch which, bool on);

// How fast the mouse moves a stick it stands in for, 1..100. Only the yuzu
// family has such a number: -1 / false for Ryujinx.
int MouseSpeedGet(const std::wstring& file, Family family);
bool MouseSpeedSet(const std::wstring& file, Family family, int value);

// The dead zones (0..1) of the entry the file has for this pad. `found`: there
// is such an entry; a stick whose number the file does not hold stays at -1
// and cannot be set.
struct Zones {
    bool found = false;
    double left = -1, right = -1;
};
Zones ZonesGet(const std::wstring& file, Family family, const Want& pad);
bool ZoneSet(const std::wstring& file, Family family, const Want& pad, bool rightStick, double value);

} // namespace padcfg
