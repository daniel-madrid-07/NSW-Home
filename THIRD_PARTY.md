# Third-party code

## In every build

| Component | Where | License |
|---|---|---|
| TweetNaCl (Ed25519 / X25519 arithmetic, adapted) | `src/fr_crypto.cpp` | Public domain |

Cryptographic primitives written from their specifications (SHA-1/256/512, HMAC, HKDF, PBKDF2,
ChaCha20, AES-128) carry no third-party code. The amiibo data layout follows the public description
of the amiitool project (MIT); the code in `src/amiibo.cpp` is NSW-Home's own.

## Windows build

| Component | Where | License |
|---|---|---|
| Microsoft Edge WebView2 SDK (header and static loader) | `third_party/webview2/` | See `third_party/webview2/LICENSE.txt` (BSD-style) |

Everything else on Windows comes from the operating system (Win32, Direct2D, DirectWrite, WIC,
XInput, CNG, WinHTTP).

## Linux, macOS and Android builds

These link to the following libraries (system packages on Linux/macOS; downloaded and built by
`android/fetch_deps.sh` for Android):

| Library | License |
|---|---|
| SDL2 | zlib |
| Cairo | LGPL-2.1 or MPL-1.1 |
| pixman | MIT |
| FreeType | FreeType License (FTL) or GPL-2.0 |
| libpng | libpng License |
| libjpeg-turbo | IJG / Modified BSD / zlib |
| fontconfig (Linux/macOS) | MIT-style |
| zlib | zlib |
