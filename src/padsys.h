// What the Settings rows about controllers ask the system itself: the keyboard
// layout in use (shown as the value of the console's "Teclado USB") and the
// keys held down (the input test page). Reading only; nothing is changed.
//
// Windows only so far: on the other systems both report "not available" and
// the rows fall back to what they did before.
#pragma once
#include <string>
#include <vector>

namespace padsys {

// The language of the keyboard layout in use, as the system names it in its
// own language ("Español (España)").
bool KeyboardLayout(std::wstring& name);

// The keys held down right now, by the names the layout gives them, at most
// `most` of them. Only while this program's own window has the foreground:
// what is typed into other programs is none of its business. False when key
// state cannot be asked for at all.
bool KeysHeld(std::vector<std::wstring>& names, size_t most = 8);

} // namespace padsys
