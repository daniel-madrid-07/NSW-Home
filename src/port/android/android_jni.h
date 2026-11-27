// The Android services NSW-Home uses that only exist on the Java side
// (NSWHomeActivity): pickers, granted folders, installed apps, intents.
// The file functions among them are declared in ../port.h, because the
// compatibility layer calls those.
#pragma once
#include <string>
#include <vector>

namespace port {
namespace android {

// The app's private folder (config, cache, the copy of the key file).
std::string FilesDir();
// Debug builds: the "key=value" extras the activity was started with. Empty in a release build.
std::vector<std::string> DebugExtras();
// Debug builds: commands sent with `adb shell am broadcast -a app.nswhome.DEBUG`.
bool PopDebug(std::string& command, std::string& argument);
// Called (on some Java thread) whenever a debug command has been queued.
void SetDebugWake(void (*wake)());
void Quit();
// How far display cut-outs reach into the window: left, top, right, bottom, in pixels.
void SafeInsets(int out[4]);
// Gamepad buttons that arrived as plain key events (Android key code, pressed
// or released): see NSWHomeActivity.dispatchKeyEvent. Called on a Java thread.
void SetPadKeySink(void (*sink)(int keyCode, bool down));

enum PickKind { kPickFolder = 0, kPickFile = 1, kPickImage = 2, kPickKeys = 3 };
// Opens the system picker. PickPoll returns false while it is still open and
// afterwards true with the chosen path ("" when cancelled).
void PickStart(PickKind kind);
bool PickPoll(std::string& result);
// Re-reads the list of granted folders (after a pick, and at start).
void RefreshGrants();

// `package` is installed and has the exported activity `activity`; `label` receives the app's name.
bool EmulatorInstalled(const std::string& package, const std::string& activity, std::string& label);
// Starts the game in the emulator. "" on success, otherwise an error code.
std::string Launch(const std::string& package, const std::string& activity, const std::string& action,
                   const std::string& extra, const std::string& gamePath);

} // namespace android
} // namespace port
