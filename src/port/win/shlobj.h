// Stand-in for <shlobj.h>: the user's Pictures folder.
#pragma once
#include <windows.h>

struct KNOWNFOLDERID {
    int which;
};
constexpr KNOWNFOLDERID FOLDERID_Pictures = {1};

// Linux: XDG_PICTURES_DIR from ~/.config/user-dirs.dirs, else ~/Pictures.
// macOS: ~/Pictures. The result is released with CoTaskMemFree.
HRESULT SHGetKnownFolderPath(const KNOWNFOLDERID& id, DWORD flags, HANDLE token, wchar_t** path);
void CoTaskMemFree(void* p);

// Creates a folder and every missing folder above it. ERROR_SUCCESS, or
// ERROR_ALREADY_EXISTS when it was there already, or another error code.
int SHCreateDirectoryExW(HWND owner, const wchar_t* path, const SECURITY_ATTRIBUTES* unused);
