// Shared state and the interfaces between the modules:
//   data.cpp  settings, emulator detection, game library
//   ui.cpp    screens and drawing
//   main.cpp  window, input, emulator process control
#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "gfx.h"
#include "pads.h"

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

// ---- data -----------------------------------------------------------------

struct EmuDef {
    const char* id;
    const wchar_t* label;
    std::vector<const wchar_t*> exes;
    const wchar_t* argsFmt; // one %s = quoted game path
    const wchar_t* dataDir; // folder name under %APPDATA%
    const wchar_t* keysSub; // subfolder holding prod.keys
    const wchar_t* profileFmt; // optional, one %s = quoted user name; goes before argsFmt
    // Class-name prefix of the child window the emulator only shows while a
    // game is actually rendering; nullptr when there is no such signal.
    // With a leading '>': a window of that class at least three levels below
    // the main window (see GameRendering).
    const wchar_t* readyChild;
    // 'r': Ryujinx and its forks (Config.json, Profiles.json, data in
    // "portable" beside the program or the shared Ryujinx folder).
    // 'y': the yuzu family (qt-config.ini, profiles.dat, data in "user"
    // beside the program or a folder of its own name).
    char family;
};
extern const std::vector<EmuDef> kEmus;
const EmuDef* FindEmu(const std::string& id);

struct Played {
    std::string last; // ISO 8601 UTC
    uint64_t seconds = 0;
};

// A player. `id` is the user's own 128-bit ID (hex): play time and the picture
// are kept under it, and so is the profile NSW-Home makes for the user in an
// emulator. `name` may be empty: a user who chose no nickname. `emu` names, per
// emulator id, the profile of the emulator's own that this user stands for
// (see "which of an emulator's profiles a user is" in data.cpp for the rule).
struct User {
    std::string id;
    std::wstring name;
    std::map<std::string, std::string> emu;
    // Twelve decimal digits, made with the user and never changed; shown as
    // NH-1234-5678-9012. Kept and shown only: nothing of it goes to an emulator.
    std::string friendCode;
};

// A game that is an ordinary program of this system (a native port), added by
// the user one by one. `id` is "pc:" and a number made from the program's path.
struct PcGame {
    std::string id;
    std::wstring exe, name;
    std::wstring args; // extra words for its command line; only ever set by hand in config.json
};

struct Config {
    bool dark = false;
    bool clock24 = false;
    bool sounds = true;
    bool swapAB = false; // confirm with the right face button, like the console
    bool fullscreen = true;
    int monitor = -1; // index left to right; -1 = primary
    int menuStyle = 1; // 1 = first console, 2 = second console
    // "Identical" mode: every page lists everything the console lists, whether
    // or not it can do anything on a PC. Off: only what works here is shown.
    bool identical = true;
    // The first-start assistant has been completed (or skipped) once.
    bool setupDone = false;
    // The system-wide key that takes a picture of the game while one is in
    // front: "PrintScreen", "F9", "Ctrl+F9", "Pause", "Insert", "ScrollLock",
    // a letter or a digit, with any of Ctrl+ Alt+ Shift+ before it. Empty: none.
    std::string captureKey = "PrintScreen";
    // Console options that have no equivalent on a PC but are remembered as the
    // console would ("sound.mute_on_unplug" -> "on"), and emulator settings
    // chosen while a game was open, waiting to be written ("ryu.<key>").
    std::map<std::string, std::string> console;
    std::vector<std::wstring> gameDirs;
    std::map<std::string, std::wstring> emulators; // id -> exe path
    std::string defaultEmu;
    std::map<std::string, std::string> perGame; // game id -> emulator id
    std::map<std::string, Played> played; // key: "<user id>/<game id>"
    std::vector<User> users; // never empty after LoadConfig
    int currentUser = 0;
    std::vector<PcGame> pcGames;
    // The user's own console firmware (the folder holding its .nca entries) and
    // the folder holding prod.keys: where the user keeps them, not copies. Used
    // when no emulator supplies them (see KeysDir, FirmwareDir).
    std::wstring ownFirmware, ownKeys;
    std::wstring title; // the window's name (taskbar, Alt+Tab); empty: "NSW-Home"
};
extern Config cfg;

struct Game {
    std::string id; // title ID, "file:<path>" when it could not be read, or a PcGame's id
    bool pc = false; // a program of this system: started directly, no emulator (`file` is the program)
    std::wstring args; // pc: see PcGame
    std::string titleId;
    std::wstring name, publisher, version, file;
    uint64_t size = 0;
    std::string last; // most recent session by anyone
    uint64_t seconds = 0;
    std::string ryuLast; // what Ryujinx itself recorded before users existed
    uint64_t ryuSeconds = 0;
    std::string error;
    gfx::Img icon;
};

std::wstring DataDir();
std::wstring KeysDir();
// Installed console firmware (system contents), or empty when there is none.
std::wstring FirmwareDir();
// The user's own firmware folder and key file, for an install with no emulator
// to supply them. Each takes what a picker returned (for the keys: prod.keys
// itself or the folder holding it; for the firmware: the folder of .nca
// entries or one above it), remembers where it is and saves; an empty path
// forgets it. Return "" or NO_FIRMWARE / KEYS_INVALID.
std::string SetOwnFirmware(const std::wstring& folder);
std::string SetOwnKeys(const std::wstring& fileOrFolder);
// Whether what KeysDir / FirmwareDir return are the user's own (not an emulator's).
bool UsingOwnKeys();
bool UsingOwnFirmware();
// PC games. PcGameId: the id the program at `exe` has or would have.
// AddPcGame returns "" or PC_EXISTS / PC_MISSING; an empty name gives the file's.
// PcIconFile: where its picture is kept (a PNG; missing when it has none).
// Removing one forgets it and its picture; the program is never touched.
std::string PcGameId(const std::wstring& exe);
std::string AddPcGame(const std::wstring& exe, const std::wstring& name);
std::wstring PcIconFile(const std::string& id);
void RenamePcGame(const std::string& id, const std::wstring& name);
std::string SetPcGameImage(const std::string& id, const std::wstring& imageFile);
void RemovePcGame(const std::string& id);
// Turns off the emulator's own start-up windows (log console, update check)
// and makes it open at (screenX, screenY), i.e. behind the launch screen, so
// nothing of it shows before the game does.
void PrepareEmulator(const std::string& emuId, int screenX, int screenY);
// The yuzu family opens its window where it last closed it, and a program that
// is always ended never records that: this writes the place into its window
// file (only that entry; backup kept), so the window opens under the launch
// screen. x, y and screenWidth are that screen's origin and width as the
// emulator's toolkit counts them (width divided by the screen's scale);
// screenNumber is the toolkit's number for the screen.
void PlaceEmulatorWindow(const std::string& emuId, int x, int y, int screenWidth, int screenNumber);
void LoadConfig();
void SaveConfig();
// Blocking; meant for a worker thread. Does not touch `cfg` except to read it.
std::vector<Game> ScanLibrary();
User& CurUser();
std::wstring AvatarPath(const std::string& userId);
// One user's history with a game.
Played UserPlayed(int user, const Game& g);
// User management. Each returns "" or an error code (NAME_TAKEN, LAST_USER, IO).
// Changes are mirrored into Ryujinx's profile list.
std::string AddUser(const std::wstring& name);
std::string RenameUser(int idx, const std::wstring& name);
std::string SetUserImage(int idx, const std::wstring& imageFile);
// The picture framed as the user chose (see gfx::PicFrame). The file itself is
// never changed: a copy is kept in the avatars folder (AvatarSource) with the
// framing (AvatarFrameOf), so it can be framed again later. SetUserImage is
// this with the picture as it comes, cropped to fill.
std::string SetUserImageFramed(int idx, const std::wstring& imageFile, const gfx::PicFrame& frame);
// The kept original of the user's picture; empty when there is none (a disc,
// a profile's picture, or one set before originals were kept).
std::wstring AvatarSource(const std::string& userId);
// How the kept original was last framed. False when it never was.
bool AvatarFrameOf(const std::string& userId, gfx::PicFrame& f);
std::string RemoveUser(int idx);
// The emulators' own profiles that no user stands for yet: what "add a user"
// offers beside making a new one. `emus` lists the emulators that have it.
struct FreeProfile {
    std::string id;
    std::wstring name, emus;
};
std::vector<FreeProfile> FreeProfiles();
// Makes one of them a user (with its name and picture). "" or an error code.
std::string AdoptProfile(const std::string& profileId);
// The name of the emulator's own profile user `idx` stands for there; empty
// when the user has none of the emulator's (NSW-Home keeps one for them).
std::wstring AdoptedProfileName(int idx, const std::string& emuId);
// Ready-made profile pictures: a coloured disc with the user's initial.
// AvatarColour(i) is 0xRRGGBB; SetUserDisc makes user idx's picture from colour i.
int AvatarColours();
uint32_t AvatarColour(int i);
std::string SetUserDisc(int idx, int colour);
// Every picture there is, newest first: NSW-Home's own captures and what the
// emulators saved in their own folders.
std::vector<std::wstring> ListScreenshots();
// NSW-Home's own captures: <data>\captures, one JPEG each, named
// "YYYYMMDD-HHMMSS-cc_<tag>.jpg" (local time to the hundredth of a second,
// then the game). CaptureTag is the game's library id made safe for a file
// name: letters, digits, '.' and '-' as they are, any other byte as %XX; an
// id that would come out longer than 80 characters is "~" and 16 hex digits
// (its FNV-1a hash) instead, which still tells the games of the library apart.
std::wstring CaptureDir();
std::string CaptureTag(const std::string& gameId);
// A new file name in CaptureDir for a picture of that game, taken now.
std::wstring NewCaptureFile(const std::string& gameId);
// The tag in the name of one of NSW-Home's own captures; "" for any other file.
std::string CaptureTagOf(const std::wstring& file);
// Deletes one of NSW-Home's own captures (and nothing else: a file anywhere
// but in CaptureDir is refused). True when it is gone.
bool DeleteCapture(const std::wstring& file);
// First-start assistant. FindEmulators looks for the known emulators in the
// usual places (slow: it walks folders) and adds what it finds to cfg without
// saving. FindEmulatorIn takes a folder or a program file and returns the id
// of the emulator found there ("" if none) and its executable.
void FindEmulators();
std::string FindEmulatorIn(const std::wstring& where, std::wstring& exe);
// Makes this the emulator games start with, takes over the game folders (when
// there are none yet), makes the first user the profile it was using, and saves.
void AdoptEmulator(const std::string& id, const std::wstring& exe);
// Copies the user's own key file to where the emulator expects it.
// Returns "" or an error code (KEYS_INVALID, IO).
// `emuId`: the emulator that gets them; empty for the one games start with.
std::string InstallKeys(const std::wstring& file, const std::string& emuId = {});
// Several emulators can be set up at once; each is told apart by what the
// program really is, not by its file name (three are called Ryujinx).
// IdentifyEmulator: the id of the emulator this program file is, "" if none.
std::string IdentifyEmulator(const std::wstring& exe);
struct EmuStatus {
    bool found = false;     // the program is still where the configuration says
    bool ownFolder = false; // keeps its data beside the program, shared with nothing
    bool keys = false, firmware = false;
    std::wstring dataDir, version;
    std::string sharedWith;   // another configured emulator using the same data folder (same saves)
    std::string keysFrom;     // a configured emulator that has the keys this one lacks
    std::string firmwareFrom; // one of the same family that has the firmware this one lacks
};
EmuStatus EmuInfo(const std::string& id);
// Adds the program under the id it really is (default if there was none) and
// saves. Returns that id, "" if the file is not a known emulator.
std::string AddEmulator(const std::wstring& exe);
// Copies keys / installed firmware from the emulator EmuInfo names.
// Return "" or an error code (NO_KEYS, NO_FIRMWARE, KEYS_INVALID, IO).
std::string GiveKeys(const std::string& toId);
std::string GiveFirmware(const std::string& toId);
// Everything after the program on the command line that starts `gameFile`
// for the current user.
std::wstring EmuLaunchArgs(const std::string& emuId, const std::wstring& gameFile);
// The settings file of the emulator games start with. A test run pointed at
// its own data folder uses the copy in <data>/<emulator id> when there is one.
std::wstring EmuConfigPath();
// "" when that file is a Ryujinx-family Config.json (ryucfg); otherwise the
// id of the yuzu-family emulator whose qt-config.ini it is (yuzucfg).
std::string EmuConfigKind();
// Where the emulator keeps save data and screenshots; empty when missing.
std::wstring SaveDataDir();
std::wstring ScreenshotDir();
// Free and total bytes of the disk holding `path`; false if unknown.
bool DiskSpace(const std::wstring& path, uint64_t& free, uint64_t& total);
std::string NowIso();
// Controllers (pads.h is the interface to the pads themselves). The players'
// order is kept in the configuration ("pads.order") and handed to the
// emulator's own settings by PrepareEmulator; see padcfg.h for what is written.
void PadOrderLoad(); // once; later calls do nothing
void PadOrderSave(const std::vector<std::string>& ids);
// The players whose pad the emulator games start with has no entry for (to
// be set up there once), and that emulator's name. False when its settings
// cannot be read.
bool PadsUnknown(std::vector<int>& players, std::wstring& emulator);

// ---- app (main.cpp) -------------------------------------------------------

enum class Btn { Up, Down, Left, Right, A, B, X, Y, Plus, Minus, L, R, Home };
enum class Sfx {
    Move, Ok, Back, Bump, Launch, Error, Home,
    MoveGame, Resume, Dialog, UserMove, UserPick, Settings, Album, Pads, UserPage, Sleep,
    Capture,
    Count
};

extern std::vector<Game> games;
extern std::string runningId; // game currently open in an emulator, or empty
extern bool scanning;

void PlaySfx(Sfx s);
// Returns an error code ("" on success): NO_EMULATOR, EMULATOR_MISSING, SPAWN.
std::string LaunchGame(const std::string& id);
void ResumeGame();
// The emulator is starting behind the launch screen and is not shown yet.
bool LaunchCovering();
// Show the game's picture now, without waiting for it to settle. (The
// emulator's own window is never shown.)
void RevealGame();
void CloseGame();
void RescanAsync();
void SetFullscreen(bool on);
// Monitors are numbered left to right. ApplyMonitor moves the menu to cfg.monitor.
int MonitorCount();
void ApplyMonitor();
void QuitApp();
// Gives the window the name in cfg.title.
void ApplyTitle();
std::wstring PickFolder();
std::wstring PickExe();
std::wstring PickImage();
std::wstring PickKeys();
void RevealFile(const std::wstring& file);
// Opens a folder in the system's file manager. False when it cannot be done
// (no such folder, no file manager); an offscreen run opens nothing and says true.
bool OpenFolder(const std::wstring& dir);
// Opens an address in the default browser. False when it cannot be done.
bool OpenUrl(const std::wstring& url);
// The text the system clipboard holds (empty when it holds none).
std::wstring ClipboardText();
// Starts OBS Studio (or brings the one already open forward): 1 done,
// 0 not installed, -1 installed but it could not be started. An offscreen
// run starts nothing and says 1.
int OpenObs();
// The capture button: saves a picture of the game that is in front (what the
// game is showing, at the size it is shown) to CaptureDir, with the capture
// sound and a notice in the corner of the screen once the file is there.
// Returns at once; false when there is nothing to capture (no game in front:
// the menu itself is never captured) or a capture is still being written.
// Windows only so far; elsewhere it always returns false.
bool RequestCapture();
// What a program file says about itself: its name (product name, else file
// description; left alone when it has neither) and, written to `iconPng` when
// that is not empty, its own icon at the largest size it carries. Windows
// only: elsewhere nothing is found and false is returned.
bool ProgramInfo(const std::wstring& exe, std::wstring& name, const std::wstring& iconPng);
// Gamepad slots currently connected (XInput 0-3).
std::vector<bool> ConnectedPads();

// ---- ui (ui.cpp) ----------------------------------------------------------

namespace ui {
void Init();
// Opens the first-start assistant if it has never been completed.
void FirstStart();
void OnButton(Btn b);
// True while OnButton is handling a press produced by a held direction
// repeating, as opposed to a fresh press. The console reacts differently to
// the two (pushing against the end of a row only bounces when held).
extern bool heldRepeat;
// The left button (or a finger): `click` true is the press, false a move
// (held or not); OnMouseUp is the release. A press on a list and a move drag
// the list; a press that did not move is a click on release. Layout units.
void OnMouse(float x, float y, bool click);
void OnMouseUp(float x, float y);
void OnWheel(float delta);
void Draw(double t, float dt);
// True while something is moving, so the main loop keeps rendering.
bool Animating();
void OnLibraryChanged();
void OnGameExit(const std::string& id, bool quick);
void OnHomePressed();
// The running game has started to draw: the loading mark fades out (12 frames)
// before the game's window is shown.
void OnGameReady();
// The notice shown over the game after a capture, drawn with its top left
// corner at (0, 0); `ok` false for the one that says it could not be saved.
// NoticeSize is its size in layout units.
void DrawCaptureNotice(bool ok);
void CaptureNoticeSize(bool ok, float& w, float& h);
// A message the emulator put up, shown as a dialog of ours with these buttons;
// `done` gets the number of the one chosen (B chooses the first).
void OnEmulatorDialog(const std::wstring& text, const std::vector<std::wstring>& buttons, std::function<void(int)> done);
// The emulator took that message away by itself.
void CloseEmulatorDialog();
// What a game's software keyboard asks for, as read from the emulator's window.
struct EmuTextAsk {
    std::wstring header, sub, guide, text; // its two lines, the grey hint, the text already there
    size_t minLen = 0, maxLen = 0;         // 0: no limit given
    bool numeric = false;                  // digits only
};
// A game opened its software keyboard: NSW-Home's own text entry with what it
// asked for. `done(true, text)` when confirmed, `done(false, {})` when backed
// out of (Esc or B). Closed without either by CloseEmulatorDialog.
void OnEmulatorText(const EmuTextAsk& ask, std::function<void(bool ok, const std::wstring& text)> done);
// Debug: jump to a named screen for --shot.
void DebugGoto(const std::wstring& name);
// Text entry is open: the keyboard types instead of navigating.
bool WantsText();
void OnChar(wchar_t c);
// The mouse moved recently, so the pointer should be visible.
bool MouseActive();
} // namespace ui
