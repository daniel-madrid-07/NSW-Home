// Stand-in for <wincrypt.h>: base64 only.
#pragma once
#include <windows.h>

#define CRYPT_STRING_BASE64 0x00000001
#define CRYPT_STRING_NOCRLF 0x40000000

// Base64 text -> bytes. Whitespace is skipped. With out == nullptr, *outLen
// receives the size needed. FALSE on malformed input or a buffer too small.
BOOL CryptStringToBinaryA(const char* text, DWORD textLen, DWORD flags, BYTE* out, DWORD* outLen, DWORD* skip,
                          DWORD* usedFlags);
// Bytes -> base64 text. With out == nullptr, *outLen receives the size needed
// INCLUDING the terminating NUL; otherwise the text is written with its NUL and
// *outLen receives the length WITHOUT it (that asymmetry is the Windows
// contract). Without CRYPT_STRING_NOCRLF a CR LF follows every 64 characters
// and the end.
BOOL CryptBinaryToStringA(const BYTE* data, DWORD dataLen, DWORD flags, char* out, DWORD* outLen);
