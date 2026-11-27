// Controller settings chosen in NSW-Home's Settings that live in the emulator's
// own file: vibration, the keyboard and the mouse handed to games, the mouse's
// speed (padcfg.h says what is written). A choice is kept in the configuration
// as a wish ("pads.wish.rumble" = on/off, ".keyboard", ".mouse",
// ".mouse_speed" = 1..100) until it has been written:
//   - at once when no game is open (the emulator may not be running);
//   - otherwise just before the next start, with the players' order
//     (PrepareEmulator), for whichever emulator is being started.
// Once the emulator games start with has it, the wish is dropped, so that
// nothing the user later changes in the emulator itself is written over.
#pragma once
#include <string>

// Writes the wishes to the settings of the emulator games start with and drops
// the ones it now holds. Never call it while a game is open.
void PadWishesApplyMain();
// The name of that emulator, for the rows' descriptions.
std::wstring PadEmuName();
