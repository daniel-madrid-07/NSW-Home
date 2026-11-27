// Stand-in for <windows.h> on Linux and macOS.
//
// The shared sources (ui.cpp, data.cpp, nsw.cpp, fwsound.cpp, app.h, gfx.h)
// are written against the Windows API. This folder is put first on the include
// path of non-Windows builds and declares just the part of that API they use;
// the implementations are in ../compat.cpp. It is never seen by the Windows
// build.
//
// Things that differ from the real API on purpose:
//   * wchar_t is 32 bits here, so "wide" strings are UTF-32, not UTF-16.
//   * Paths may arrive with backslashes; every file call converts them
//     (see port::NativePath in ../port.h).
#pragma once
#ifdef _WIN32
#error "src/port/win is for non-Windows builds only"
#endif

// The Windows header drags most of the C library in with it, and the shared
// sources rely on that.
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

// ---- basic types ------------------------------------------------------------

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef int32_t HRESULT;
typedef int32_t NTSTATUS;
typedef wchar_t WCHAR;
typedef wchar_t* PWSTR;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef const wchar_t* PCWSTR;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef BYTE* PUCHAR;
typedef BYTE* PBYTE;
typedef void* PVOID;
typedef void* LPVOID;
typedef void* HANDLE;
typedef void* HWND;
typedef void* HINSTANCE;
typedef void* HMODULE;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define MAX_PATH 260
#define WINAPI
#define CALLBACK

#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))

#define S_OK (static_cast<HRESULT>(0))
#define E_FAIL (static_cast<HRESULT>(0x80004005))
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183

// The reason the last failed file call on this thread gave. Only the calls
// whose callers look at it set it: CopyFileW, CreateFileW, CreateDirectoryW,
// SHCreateDirectoryExW.
DWORD GetLastError();
void SetLastError(DWORD code);

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED;

typedef struct _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES;

// ---- strings ----------------------------------------------------------------

#define CP_ACP 0
#define CP_UTF8 65001

// UTF-8 <-> wide. Same calling convention as on Windows (a null/zero output
// asks for the size; a length of -1 means NUL-terminated and counts the NUL),
// but the wide side is UTF-32 because that is what wchar_t holds here.
// Malformed UTF-8 becomes U+FFFD. UTF-16 surrogate pairs found in wide input
// are joined, so text assembled from 16-bit units still comes out right.
int MultiByteToWideChar(UINT codePage, DWORD flags, const char* src, int srcLen, wchar_t* dst, int dstLen);
int WideCharToMultiByte(UINT codePage, DWORD flags, const wchar_t* src, int srcLen, char* dst, int dstLen,
                        const char* defaultChar, BOOL* usedDefault);

// Lowercases in place; returns the number of characters processed.
DWORD CharLowerBuffW(wchar_t* s, DWORD len);
int _wcsicmp(const wchar_t* a, const wchar_t* b);
int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n);
int _wtoi(const wchar_t* s);

// The Microsoft wide printf family reads %s and %c as WIDE arguments; the
// standard one reads them as narrow. This rewrites the format accordingly
// (%s -> %ls, %c -> %lc, %S -> %s, %C -> %c) so the shared format strings
// mean the same thing on every platform.
namespace port {
const wchar_t* MsWideFormat(const wchar_t* fmt, wchar_t* scratch, size_t scratchLen);
}

template <size_t N, typename... Args>
int swprintf_s(wchar_t (&buf)[N], const wchar_t* fmt, Args... args) {
    wchar_t scratch[512];
    int n = swprintf(buf, N, port::MsWideFormat(fmt, scratch, 512), args...);
    if (n < 0) {
        // Did not fit. The Windows function would abort; keep whatever fitted.
        buf[N - 1] = 0;
        n = static_cast<int>(wcslen(buf));
    }
    return n;
}

#define sscanf_s sscanf // only used with numeric conversions
#define swscanf_s swscanf // likewise (a %s here would mean different things on the two systems)

// errno_t-style: 0 on success. `mode` may carry a ", ccs=..." suffix, which is
// dropped (text is written as UTF-8 through the process locale).
int _wfopen_s(FILE** out, const wchar_t* path, const wchar_t* mode);

void* SecureZeroMemory(void* p, size_t n);

// ---- environment ------------------------------------------------------------

// APPDATA, LOCALAPPDATA, ProgramFiles and USERPROFILE are answered with this
// system's equivalents (see compat.cpp); any other name is a plain getenv.
DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buf, DWORD size);

// ---- files ------------------------------------------------------------------

#define INVALID_FILE_ATTRIBUTES (static_cast<DWORD>(-1))
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_WRITE_THROUGH 0x00000008 // accepted; rename() is already atomic
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3

typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    wchar_t cFileName[MAX_PATH];
} WIN32_FIND_DATAW;

enum FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic };
enum FINDEX_SEARCH_OPS { FindExSearchNameMatch };
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002

DWORD GetFileAttributesW(const wchar_t* path);
// Creates missing parent folders too (the Windows call does not; the data
// folder's parent, ~/.config, may not exist yet on a fresh account).
BOOL CreateDirectoryW(const wchar_t* path, SECURITY_ATTRIBUTES* unused);
BOOL MoveFileExW(const wchar_t* from, const wchar_t* to, DWORD flags);
BOOL CopyFileW(const wchar_t* from, const wchar_t* to, BOOL failIfExists);
BOOL DeleteFileW(const wchar_t* path);

// Only the "<folder>\*" pattern is supported: it lists the folder, "." and
// ".." included, as on Windows.
HANDLE FindFirstFileExW(const wchar_t* pattern, FINDEX_INFO_LEVELS level, WIN32_FIND_DATAW* out,
                        FINDEX_SEARCH_OPS op, void* filter, DWORD flags);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* out);
BOOL FindClose(HANDLE h);

// Two ways of opening a file are supported, the two the shared sources use:
// GENERIC_READ + OPEN_EXISTING, and GENERIC_WRITE + CREATE_ALWAYS (create or
// empty). Sharing flags are ignored: nothing here locks a file.
HANDLE CreateFileW(const wchar_t* path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa, DWORD disposition,
                   DWORD flags, HANDLE templateFile);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size);
// With an OVERLAPPED the read is positional and leaves the file position
// alone; without one it reads from the current position and advances it.
// Either way it keeps reading until `len` bytes or the end of the file.
BOOL ReadFile(HANDLE h, void* buf, DWORD len, DWORD* got, OVERLAPPED* ov);
// Sequential only (ov must be null).
BOOL WriteFile(HANDLE h, const void* buf, DWORD len, DWORD* put, OVERLAPPED* ov);
BOOL FlushFileBuffers(HANDLE h);
BOOL CloseHandle(HANDLE h);

// Free and total bytes of the file system holding `path`, as available to
// this user. Any of the three outputs may be null.
BOOL GetDiskFreeSpaceExW(const wchar_t* path, ULARGE_INTEGER* freeForCaller, ULARGE_INTEGER* total,
                         ULARGE_INTEGER* totalFree);

// ---- time and power ---------------------------------------------------------

void GetLocalTime(SYSTEMTIME* st);
void GetSystemTime(SYSTEMTIME* st);
ULONGLONG GetTickCount64();

typedef struct _SYSTEM_POWER_STATUS {
    BYTE ACLineStatus;       // 0 battery, 1 mains, 255 unknown
    BYTE BatteryFlag;        // bit 128: no system battery; 255 unknown
    BYTE BatteryLifePercent; // 0-100, 255 unknown
    BYTE SystemStatusFlag;
    DWORD BatteryLifeTime;
    DWORD BatteryFullLifeTime;
} SYSTEM_POWER_STATUS;

// Real battery charge where the system has one (Linux: /sys/class/power_supply,
// macOS: IOKit power sources); "no battery" otherwise. Re-read every 10 s.
BOOL GetSystemPowerStatus(SYSTEM_POWER_STATUS* out);
