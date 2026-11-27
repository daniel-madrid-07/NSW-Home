// Windows only: a picture of another program's window, taken from outside it,
// and the small notice shown over that program afterwards. Used by main.cpp
// and by tools\capture_probe.cpp, which checks both against a known window.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace wincap {

struct Picture {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra; // top row first, 4 bytes a pixel, opaque
    const char* method = "";   // "window" or "screen": which way the pixels came
};

// The part `crop` (screen coordinates, real pixels) of the top-level window
// `top`, as the desktop compositor holds it: what the window shows, whether or
// not something else is lying over it. When that comes back empty (a program
// drawing past the compositor) and `allowScreen` is set, the same rectangle is
// read from the screen instead, which is only right while the window is in
// front. Blocking (tens of milliseconds); meant for a worker thread.
bool Grab(HWND top, RECT crop, bool allowScreen, Picture& out);

// Writes a JPEG (quality 92). The file appears under its name only when it is
// complete. Initialises COM for the calling thread itself.
bool SaveJpeg(const Picture& p, const std::wstring& file);

// Shows a w x h picture (premultiplied BGRA, top row first) at screen position
// (x, y) above every window for `ms` milliseconds, fading in and out, then
// removes it. It never takes the focus and lets the mouse through. Needs a
// message loop on the calling thread. A notice still showing is replaced.
void ShowNotice(const uint8_t* premulBgra, int w, int h, int x, int y, int ms);
// Removes the notice at once (before reading the screen).
void HideNotice();
// The notice window while it is up, else nullptr.
HWND NoticeWindow();

} // namespace wincap
