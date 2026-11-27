// A web page inside NSW-Home's own window: what the bar's "Nintendo eShop"
// and "Noticias" buttons open (ui.cpp, "the web page").
//
// Windows: Microsoft Edge WebView2 (webview.cpp), a child window of the menu's
// window laid over the area the page leaves for it. Everything else the page
// shows (its header, its footer, the opening and closing) is drawn by ui.cpp.
// Other systems have no web view here: Available() is false and the buttons
// keep opening the system's browser.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace web {

enum class State {
    Closed,    // nothing open
    Starting,  // the browser is starting / the view is being made
    Loading,   // the view exists; the first page has not shown anything yet
    Ready,     // the first page is there (later navigations: see Loading())
    Failed,    // could not be made, or the browser went away
    Offscreen, // an offscreen test run: there is no window to put a view in
};

#if defined(_WIN32)

// The WebView2 runtime is installed (asked once).
bool Available();
// main.cpp: the menu's window, which the view is made a child of. Never called
// by the offscreen test runs, whose Open() only says Offscreen.
void Attach(HWND host);
// Starts the browser before the page is known (the button's own reaction and
// the "who" sheet take long enough to hide most of its start).
void Prepare();
// Opens `url` with the browser profile `profile` (one per user: logins are
// kept per user, in <data>\web). `ground` is 0xRRGGBB, shown until the page
// paints. The view stays hidden until Show(true). False: no web view here.
bool Open(const std::wstring& url, const std::string& profile, uint32_t ground);
// Closes the view and lets the browser processes end.
void Close();
State Now();
bool Loading(); // a navigation is under way
// Where the view goes, in layout units (the 1280x720 space).
void Place(float x, float y, float w, float h);
void Show(bool on);
bool Shown();
// The page put an element in full screen (a video): the view should fill the window.
bool FullScreen();
std::wstring Title();
std::wstring Url();
bool CanBack();
bool CanForward();
void Back();
void Forward();
void Reload();
// A picture of the page as it is now, for the closing fade (PNG bytes;
// empty until it has arrived).
void Snapshot();
const std::vector<uint8_t>& SnapshotPng();
// The controller. Pad: the left stick moves the system's pointer over the
// page and the right stick scrolls under it; true while the page is taking
// the sticks (the menu must then not read the left stick as directions).
bool Pad(float lx, float ly, float rx, float ry);
void Click();                   // A: a left click where the pointer is, if it is over the page
void Scroll(float dx, float dy); // the direction buttons: wheel notches at the pointer
// --webtest: the whole path in a hidden window off the screen, logged to
// `log`. 0 when every step passed.
int SelfTest(HINSTANCE inst, const std::wstring& page, int cycles, const std::wstring& log);

#else // the portable builds: no web view

inline bool Available() { return false; }
inline void Prepare() {}
inline bool Open(const std::wstring&, const std::string&, uint32_t) { return false; }
inline void Close() {}
inline State Now() { return State::Closed; }
inline bool Loading() { return false; }
inline void Place(float, float, float, float) {}
inline void Show(bool) {}
inline bool Shown() { return false; }
inline bool FullScreen() { return false; }
inline std::wstring Title() { return {}; }
inline std::wstring Url() { return {}; }
inline bool CanBack() { return false; }
inline bool CanForward() { return false; }
inline void Back() {}
inline void Forward() {}
inline void Reload() {}
inline void Snapshot() {}
inline const std::vector<uint8_t>& SnapshotPng() {
    static const std::vector<uint8_t> none;
    return none;
}
inline bool Pad(float, float, float, float) { return false; }
inline void Click() {}
inline void Scroll(float, float) {}

#endif

} // namespace web
