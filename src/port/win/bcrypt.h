// Stand-in for <bcrypt.h> (Windows CNG): the system random generator and
// AES in ECB mode, which is all the shared sources use. The AES is a small
// self-contained implementation in ../compat.cpp, checked against the
// FIPS-197 vectors by the self-test.
#pragma once
#include <windows.h>

typedef void* BCRYPT_ALG_HANDLE;
typedef void* BCRYPT_KEY_HANDLE;

#define BCRYPT_SUCCESS(status) (static_cast<NTSTATUS>(status) >= 0)
#define BCRYPT_AES_ALGORITHM L"AES"
#define BCRYPT_CHAINING_MODE L"ChainingMode"
#define BCRYPT_CHAIN_MODE_ECB L"ChainingModeECB"
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002

// Only "AES" is known.
NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* out, const wchar_t* algorithm, const wchar_t* impl, ULONG flags);
// Only ChainingMode = ChainingModeECB is accepted.
NTSTATUS BCryptSetProperty(void* object, const wchar_t* property, PUCHAR value, ULONG valueLen, ULONG flags);
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE alg, ULONG flags);
// 16, 24 or 32 byte keys.
NTSTATUS BCryptGenerateSymmetricKey(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE* out, PUCHAR keyObject, ULONG keyObjectLen,
                                    PUCHAR secret, ULONG secretLen, ULONG flags);
NTSTATUS BCryptDestroyKey(BCRYPT_KEY_HANDLE key);
// ECB, no padding, no IV: inLen must be a multiple of 16; in and out may be the same buffer.
NTSTATUS BCryptEncrypt(BCRYPT_KEY_HANDLE key, PUCHAR in, ULONG inLen, void* padding, PUCHAR iv, ULONG ivLen, PUCHAR out,
                       ULONG outLen, ULONG* done, ULONG flags);
NTSTATUS BCryptDecrypt(BCRYPT_KEY_HANDLE key, PUCHAR in, ULONG inLen, void* padding, PUCHAR iv, ULONG ivLen, PUCHAR out,
                       ULONG outLen, ULONG* done, ULONG flags);
// Bytes from the operating system's generator.
NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE alg, PUCHAR buf, ULONG len, ULONG flags);
