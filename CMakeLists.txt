# Linux and macOS build of NSW-Home. Windows is built with build.bat and does
# not use this file.
#
#   cmake -S . -B build/unix -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/unix -j4
#   ctest --test-dir build/unix          (AES, base64, UTF and file-layer checks)
#
# Needs SDL2, Cairo, FreeType, fontconfig and libjpeg with their headers, and
# pkg-config to find them:
#   Ubuntu/Debian: sudo apt install build-essential cmake pkg-config libsdl2-dev
#                  libcairo2-dev libfreetype-dev libfontconfig1-dev libjpeg-dev
#   macOS:         brew install cmake pkg-config sdl2 cairo freetype fontconfig jpeg-turbo
cmake_minimum_required(VERSION 3.16)
project(NSW-Home LANGUAGES CXX)

if(WIN32)
    message(FATAL_ERROR "On Windows, build with build.bat. This file is for Linux and macOS.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)

add_library(nswhome_warnings INTERFACE)
target_compile_options(nswhome_warnings INTERFACE -Wall -Wextra)

# The Windows calls the shared sources make (src/port/win/*.h), implemented
# for this system. src/port/win must come before everything else on the
# include path: that is what makes `#include <windows.h>` find the stand-in.
add_library(nswhome_compat STATIC src/port/compat.cpp)
target_include_directories(nswhome_compat BEFORE PUBLIC src/port/win src/port src)
target_link_libraries(nswhome_compat PUBLIC nswhome_warnings Threads::Threads)
if(APPLE)
    target_link_libraries(nswhome_compat PUBLIC "-framework CoreFoundation" "-framework IOKit")
endif()

# Everything except the screens and the program entry: the shared data and
# dump-reading code, unchanged, plus the Cairo drawing backend.
add_library(nswhome_core STATIC
    src/data.cpp
    src/nsw.cpp
    src/fwsound.cpp
    src/fwfont.cpp
    src/ryucfg.cpp
    src/yuzucfg.cpp
    src/pads.cpp
    src/padcfg.cpp
    src/padsys.cpp
    src/a11y.cpp
    src/pchost.cpp
    src/pcsys.cpp
    src/emudata.cpp
    src/parental.cpp
    src/update.cpp
    src/amiibo.cpp
    src/amiibo_emu.cpp
    src/content.cpp
    src/fwtext.cpp
    src/lang.cpp
    src/friends.cpp
    src/fr_dht.cpp
    src/fr_net.cpp
    src/fr_crypto.cpp
    src/netplay.cpp
    src/port/gfx_cairo.cpp)
target_link_libraries(nswhome_core PUBLIC nswhome_compat PkgConfig::CAIRO PkgConfig::FREETYPE PkgConfig::FONTCONFIG
                                           PkgConfig::JPEG)

add_executable(nsw-home src/ui.cpp src/console_menus.cpp src/port/main_sdl.cpp)
target_link_libraries(nsw-home PRIVATE nswhome_core PkgConfig::SDL2)
# ui.cpp is shared with the Windows build and written against MSVC's /W4. Two
# things GCC and Clang report there and MSVC does not are switched off for
# that one file: helpers nothing calls (yet), and table rows that leave their
# trailing fields to the defaults. Every other warning stays on for it.
set_source_files_properties(src/ui.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-missing-field-initializers")

# Checks of the compatibility layer against published test vectors.
add_executable(nswhome_selftest src/port/tests/selftest.cpp)
target_link_libraries(nswhome_selftest PRIVATE nswhome_compat)

# Draws a test card and prints text measurements through gfx.h only; the same
# source builds on Windows against gfx.cpp (src/port/tests/build_gfxprobe_win.bat),
# so the two drawing backends can be compared picture against picture.
add_executable(nswhome_gfxprobe src/port/tests/gfxprobe.cpp)
target_link_libraries(nswhome_gfxprobe PRIVATE nswhome_core)

# The friends module against published vectors (SHA, HMAC, PBKDF2, ChaCha20,
# Ed25519, BEP 44) and its own item format. No network.
add_executable(nswhome_friends_test tools/friends_test.cpp src/friends.cpp src/fr_dht.cpp src/fr_net.cpp src/fr_crypto.cpp)
target_link_libraries(nswhome_friends_test PRIVATE nswhome_warnings Threads::Threads)

enable_testing()
add_test(NAME compat COMMAND nswhome_selftest)
add_test(NAME friends COMMAND nswhome_friends_test)
