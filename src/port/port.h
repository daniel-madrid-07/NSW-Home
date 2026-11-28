// Shared by the files of the Linux/macOS port (compat.cpp, gfx_cairo.cpp,
// main_sdl.cpp, selftest.cpp). Not used by the Windows build.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace port {

std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& utf8);

// A path as the shared code writes it -> a path this system understands, UTF-8.
//   * backslashes become slashes (the shared code joins with "\\");
//   * "X:\..." becomes "<root>/x/..." when that folder exists, <root> being
//     $NSWHOME_DRIVE_ROOT or /mnt. That is how WSL shows Windows drives, so a
//     config written on Windows keeps working there. Elsewhere there is no
//     such folder and the path is left alone.
// A file name containing a real backslash cannot be addressed.
std::string NativePath(const std::wstring& path);

// A Windows command line (what the shared code builds for an emulator: words
// separated by spaces, a word with spaces in it between double quotes) as the
// list of arguments a program is started with here. No shell is involved, so
// nothing else is special; a quote inside a word cannot be expressed.
std::vector<std::string> SplitCommandLine(const std::wstring& line);

// What the shared code receives when it asks for these Windows variables.
std::string HomeDir();
std::string ConfigHome(); // APPDATA:      $XDG_CONFIG_HOME or ~/.config;      macOS ~/Library/Application Support
std::string DataHome();   // LOCALAPPDATA: $XDG_DATA_HOME or ~/.local/share;   macOS ~/Library/Application Support
std::string AppsDir();    // ProgramFiles: /opt;                               macOS /Applications

#ifdef __APPLE__
// Native macOS NSOpenPanel file/folder picker (implemented in mac_dialog.mm).
std::string MacPickPath(bool folder, bool image, bool keys);
#endif

// AES block cipher behind the bcrypt stand-in, exposed for the self-test.
struct Aes {
    uint32_t enc[60], dec[60];
    int rounds = 0;
    bool SetKey(const uint8_t* key, size_t len); // 16, 24 or 32 bytes
    void EncryptBlock(const uint8_t in[16], uint8_t out[16]) const;
    void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const;
};

} // namespace port

#if defined(__ANDROID__)
// Android. Shared storage is not reachable through paths: the user grants a
// folder through the system's folder picker, and its files are then listed and
// opened through the content resolver. To the shared code such a folder still
// looks like a path (the place it has on the device, "/storage/emulated/0/Roms");
// the file calls in compat.cpp ask here first and go through the grant when the
// path lies inside a granted folder. Implemented in android/android_jni.cpp.
namespace port {
namespace android {

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    int64_t mtimeMs = 0; // milliseconds since 1970
};

// The path (slashes, UTF-8) lies inside a folder granted through the picker.
bool Granted(const std::string& path);
bool List(const std::string& path, std::vector<Entry>& out);
bool Stat(const std::string& path, Entry& out);
// A read-only descriptor that supports pread, to be closed by the caller; -1 on failure.
int OpenRead(const std::string& path);
// Battery charge 0-100 and whether it is charging; false when the device has none.
bool Battery(int& percent, bool& charging);

} // namespace android
} // namespace port
#endif

namespace gfx {
// The picture gfx::End() just finished: 32-bit BGRX rows (native-endian
// 0xXXRRGGBB), `stride` bytes apart. Null when there is no target.
const uint8_t* FramePixels(int& w, int& h, int& stride);
// Name of the font file text is drawn with (the console font shows as
// "(memory)"), for diagnostics.
std::string FontDescription();
} // namespace gfx
