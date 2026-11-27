// What Settings needs from the machine NSW-Home runs on: the monitor's facts
// and brightness, Windows' own settings pages, and plain file work on
// NSW-Home's own data folder. The Windows half is in pchost.cpp; the same file
// answers "not available" for the display part on every other system.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pchost {

// Offscreen test runs: nothing real is changed and nothing is opened on the
// desktop. Reading stays real; a brightness written in this mode goes to a
// stand-in value that the next read returns.
void SetOffscreen(bool on);
bool Offscreen();

// The monitor NSW-Home's window is on (the primary one when there is no window).
struct Display {
    bool ok = false;
    int width = 0, height = 0; // pixels of the current mode
    int hz = 0;                // refresh rate, rounded
    int scale = 0;             // Windows' scaling in per cent; 0 = unknown
    int hdr = -1;              // 1 on, 0 off, -1 unknown
    bool hdrCapable = false;   // the monitor accepts HDR at all
    int bits = 0;              // bits per colour channel; 0 = unknown
    bool builtIn = false;      // a laptop's own panel
};
Display DisplayNow();

// Name of the graphics adapter that drives the desktop; empty when unknown.
std::wstring AdapterName();

// Brightness of that monitor, 0-100. False when the monitor does not answer
// (no DDC/CI, a television, another system).
bool BrightnessGet(int& percent);
bool BrightnessSet(int percent);

// Opens Windows' display settings (`hdr`: the page with the HDR switch where
// Windows has one of its own). False when nothing was opened.
bool OpenDisplaySettings(bool hdr);

// ---- files of NSW-Home's own ------------------------------------------------
struct Entry {
    std::wstring name;
    bool dir = false;
};
bool Exists(const std::wstring& path);
bool ReadText(const std::wstring& path, std::string& out);
bool WriteText(const std::wstring& path, const std::string& text);
bool Copy(const std::wstring& from, const std::wstring& to);
bool Remove(const std::wstring& file);
bool MakeDir(const std::wstring& dir);
std::vector<Entry> List(const std::wstring& dir);
// Copies the files of `from` into `to` (created if missing), folders included.
bool CopyTree(const std::wstring& from, const std::wstring& to);
// Removes everything inside `dir` and then the folder itself.
bool RemoveTree(const std::wstring& dir);

// ---- the console lock's secret ----------------------------------------------
std::string Sha256Hex(const std::string& data);
// `bytes` random bytes from the system's generator, as hex.
std::string RandomHex(int bytes);

} // namespace pchost
