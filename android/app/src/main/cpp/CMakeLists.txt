# Native part of NSW-Home for Android: the shared sources (../../../../../src),
# the same Cairo + FreeType drawing backend the Linux and macOS builds use, and
# the libraries that backend needs, all built from source by the NDK.
#
# The library sources are not in the project: android/fetch_deps.sh downloads
# them from their projects' own release pages into android/third_party.
cmake_minimum_required(VERSION 3.22)
project(NSWHomeAndroid LANGUAGES C CXX ASM)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)   # the project root (holds src/)
set(TP ${ROOT}/android/third_party)
set(SDL_DIR ${TP}/SDL2-2.32.10)
set(FT_DIR ${TP}/freetype-VER-2-13-3)
set(PNG_DIR ${TP}/libpng-1.6.50)
set(JPEG_DIR ${TP}/libjpeg-turbo-3.1.2)
set(PIXMAN_DIR ${TP}/pixman-0.46.4)
set(CAIRO_DIR ${TP}/cairo-1.18.4)
foreach(dir ${SDL_DIR} ${FT_DIR} ${PNG_DIR} ${JPEG_DIR} ${PIXMAN_DIR} ${CAIRO_DIR})
    if(NOT EXISTS ${dir})
        message(FATAL_ERROR "${dir} is missing: run android/fetch_deps.sh first")
    endif()
endforeach()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)
find_package(ZLIB REQUIRED) # the NDK's own

# ---- SDL2: activity glue, input, audio, the window --------------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} sdl2 EXCLUDE_FROM_ALL)

# ---- FreeType: glyph outlines -------------------------------------------------
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE) # its own copy; only compressed bitmap fonts use it
add_subdirectory(${FT_DIR} freetype EXCLUDE_FROM_ALL)

# ---- libpng -------------------------------------------------------------------
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(${PNG_DIR} libpng EXCLUDE_FROM_ALL)

# ---- libjpeg-turbo ------------------------------------------------------------
# Its build refuses to be included with add_subdirectory, so it is configured
# and built as a project of its own with the same toolchain. Its x86-64 SIMD
# code needs NASM, which the NDK does not have: that ABI (the virtual device)
# uses the plain C routines; arm64 uses NEON through the compiler.
include(ExternalProject)
set(JPEG_BUILD ${CMAKE_CURRENT_BINARY_DIR}/jpeg)
set(JPEG_SIMD ON)
if(ANDROID_ABI STREQUAL "x86_64")
    set(JPEG_SIMD OFF)
endif()
set(JPEG_POOL_ARGS "")
if(CMAKE_JOB_POOLS)
    set(JPEG_POOL_ARGS -DCMAKE_JOB_POOLS=${CMAKE_JOB_POOLS} -DCMAKE_JOB_POOL_COMPILE=${CMAKE_JOB_POOL_COMPILE}
                       -DCMAKE_JOB_POOL_LINK=${CMAKE_JOB_POOL_LINK})
endif()
ExternalProject_Add(jpeg_build
    SOURCE_DIR ${JPEG_DIR}
    BINARY_DIR ${JPEG_BUILD}
    CMAKE_GENERATOR ${CMAKE_GENERATOR}
    CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} -DANDROID_ABI=${ANDROID_ABI}
               -DANDROID_PLATFORM=${ANDROID_PLATFORM} -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
               -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_POSITION_INDEPENDENT_CODE=ON
               -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_SIMD=${JPEG_SIMD}
               -DWITH_TOOLS=OFF -DWITH_TESTS=OFF ${JPEG_POOL_ARGS}
    BUILD_COMMAND ${CMAKE_COMMAND} --build ${JPEG_BUILD} --target jpeg-static
    BUILD_BYPRODUCTS ${JPEG_BUILD}/libjpeg.a
    INSTALL_COMMAND "")
add_library(jpeg STATIC IMPORTED)
set_target_properties(jpeg PROPERTIES IMPORTED_LOCATION ${JPEG_BUILD}/libjpeg.a)
# jconfig.h is written into the build folder; the other headers are in src/.
file(MAKE_DIRECTORY ${JPEG_BUILD})
target_include_directories(jpeg INTERFACE ${JPEG_BUILD} ${JPEG_DIR}/src)
add_dependencies(jpeg jpeg_build)

# ---- pixman (hand-written list: upstream builds with Meson only) -------------
set(PIXMAN_SRC
    pixman.c pixman-access.c pixman-access-accessors.c pixman-arm.c pixman-bits-image.c pixman-combine32.c
    pixman-combine-float.c pixman-conical-gradient.c pixman-edge.c pixman-edge-accessors.c pixman-fast-path.c
    pixman-filter.c pixman-glyph.c pixman-general.c pixman-gradient-walker.c pixman-image.c
    pixman-implementation.c pixman-linear-gradient.c pixman-matrix.c pixman-mips.c pixman-noop.c pixman-ppc.c
    pixman-radial-gradient.c pixman-region16.c pixman-region32.c pixman-region64f.c pixman-riscv.c
    pixman-solid-fill.c pixman-timer.c pixman-trap.c pixman-utils.c pixman-x86.c)
if(ANDROID_ABI STREQUAL "arm64-v8a")
    list(APPEND PIXMAN_SRC pixman-arm-neon.c pixman-arma64-neon-asm.S pixman-arma64-neon-asm-bilinear.S)
elseif(ANDROID_ABI STREQUAL "x86_64")
    list(APPEND PIXMAN_SRC pixman-sse2.c pixman-ssse3.c)
endif()
list(TRANSFORM PIXMAN_SRC PREPEND ${PIXMAN_DIR}/pixman/)
add_library(pixman STATIC ${PIXMAN_SRC})
target_include_directories(pixman PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pixman ${PIXMAN_DIR}/pixman)
target_compile_definitions(pixman PRIVATE HAVE_CONFIG_H)
target_compile_options(pixman PRIVATE -w -include ${CMAKE_CURRENT_SOURCE_DIR}/pixman/pixman-config.h)
if(ANDROID_ABI STREQUAL "x86_64")
    set_source_files_properties(${PIXMAN_DIR}/pixman/pixman-sse2.c PROPERTIES COMPILE_OPTIONS "-msse2")
    set_source_files_properties(${PIXMAN_DIR}/pixman/pixman-ssse3.c PROPERTIES COMPILE_OPTIONS "-mssse3")
endif()

# ---- cairo (hand-written list, same reason) -----------------------------------
set(CAIRO_SRC
    cairo-analysis-surface.c cairo-arc.c cairo-array.c cairo-atomic.c cairo-base64-stream.c cairo-base85-stream.c
    cairo-bentley-ottmann-rectangular.c cairo-bentley-ottmann-rectilinear.c cairo-bentley-ottmann.c
    cairo-botor-scan-converter.c cairo-boxes-intersect.c cairo-boxes.c cairo-cache.c cairo-clip-boxes.c
    cairo-clip-polygon.c cairo-clip-region.c cairo-clip-surface.c cairo-clip-tor-scan-converter.c cairo-clip.c
    cairo-color.c cairo-composite-rectangles.c cairo-compositor.c cairo-contour.c cairo-damage.c cairo-debug.c
    cairo-default-context.c cairo-device.c cairo-error.c cairo-fallback-compositor.c cairo-fixed.c
    cairo-font-face-twin-data.c cairo-font-face-twin.c cairo-font-face.c cairo-font-options.c cairo-freed-pool.c
    cairo-freelist.c cairo-gstate.c cairo-hash.c cairo-hull.c cairo-image-compositor.c cairo-image-info.c
    cairo-image-source.c cairo-image-surface.c cairo-line.c cairo-lzw.c cairo-mask-compositor.c cairo-matrix.c
    cairo-mempool.c cairo-mesh-pattern-rasterizer.c cairo-misc.c cairo-mono-scan-converter.c cairo-mutex.c
    cairo-no-compositor.c cairo-observer.c cairo-output-stream.c cairo-paginated-surface.c cairo-path-bounds.c
    cairo-path-fill.c cairo-path-fixed.c cairo-path-in-fill.c cairo-path-stroke-boxes.c
    cairo-path-stroke-polygon.c cairo-path-stroke-traps.c cairo-path-stroke-tristrip.c cairo-path-stroke.c
    cairo-path.c cairo-pattern.c cairo-pen.c cairo-polygon-intersect.c cairo-polygon-reduce.c cairo-polygon.c
    cairo-raster-source-pattern.c cairo-recording-surface.c cairo-rectangle.c
    cairo-rectangular-scan-converter.c cairo-region.c cairo-rtree.c cairo-scaled-font.c
    cairo-shape-mask-compositor.c cairo-slope.c cairo-spans-compositor.c cairo-spans.c cairo-spline.c
    cairo-stroke-dash.c cairo-stroke-style.c cairo-surface-clipper.c cairo-surface-fallback.c
    cairo-surface-observer.c cairo-surface-offset.c cairo-surface-snapshot.c cairo-surface-subsurface.c
    cairo-surface-wrapper.c cairo-surface.c cairo-time.c cairo-tor-scan-converter.c
    cairo-tor22-scan-converter.c cairo-toy-font-face.c cairo-traps-compositor.c cairo-traps.c cairo-tristrip.c
    cairo-unicode.c cairo-user-font.c cairo-version.c cairo-wideint.c cairo.c cairo-cff-subset.c
    cairo-scaled-font-subsets.c cairo-truetype-subset.c cairo-type1-fallback.c cairo-type1-glyph-names.c
    cairo-type1-subset.c cairo-type3-glyph-surface.c cairo-pdf-operators.c cairo-pdf-shading.c
    cairo-tag-attributes.c cairo-tag-stack.c cairo-deflate-stream.c
    cairo-png.c)
list(TRANSFORM CAIRO_SRC PREPEND ${CAIRO_DIR}/src/)
add_library(cairo STATIC ${CAIRO_SRC})
target_include_directories(cairo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cairo ${CAIRO_DIR}/src)
target_compile_definitions(cairo PRIVATE HAVE_CONFIG_H CAIRO_COMPILATION _GNU_SOURCE _REENTRANT)
target_compile_options(cairo PRIVATE -w)
target_link_libraries(cairo PUBLIC pixman png_static ZLIB::ZLIB m)

# ---- NSW-Home -----------------------------------------------------------------
# SDL's Java side loads "libmain.so" and calls SDL_main in it.
add_library(main SHARED
    ${ROOT}/src/ui.cpp
    ${ROOT}/src/console_menus.cpp
    ${ROOT}/src/data.cpp
    ${ROOT}/src/nsw.cpp
    ${ROOT}/src/fwsound.cpp
    ${ROOT}/src/fwfont.cpp
    ${ROOT}/src/ryucfg.cpp
    ${ROOT}/src/yuzucfg.cpp
    ${ROOT}/src/pads.cpp
    ${ROOT}/src/padcfg.cpp
    ${ROOT}/src/padsys.cpp
    ${ROOT}/src/a11y.cpp
    ${ROOT}/src/pchost.cpp
    ${ROOT}/src/pcsys.cpp
    ${ROOT}/src/emudata.cpp
    ${ROOT}/src/parental.cpp
    ${ROOT}/src/update.cpp
    ${ROOT}/src/amiibo.cpp
    ${ROOT}/src/amiibo_emu.cpp
    ${ROOT}/src/content.cpp
    ${ROOT}/src/fwtext.cpp
    ${ROOT}/src/lang.cpp
    ${ROOT}/src/netplay.cpp
    ${ROOT}/src/friends.cpp
    ${ROOT}/src/fr_dht.cpp
    ${ROOT}/src/fr_net.cpp
    ${ROOT}/src/fr_crypto.cpp
    ${ROOT}/src/port/compat.cpp
    ${ROOT}/src/port/gfx_cairo.cpp
    ${ROOT}/src/port/android/android_jni.cpp
    ${ROOT}/src/port/android/main_android.cpp)
# src/port/win first: that is what makes `#include <windows.h>` find the stand-in.
target_include_directories(main BEFORE PRIVATE ${ROOT}/src/port/win ${ROOT}/src/port ${ROOT}/src)
target_compile_options(main PRIVATE -Wall -Wextra)
# Same two exceptions as the Linux build makes for ui.cpp (see ../../../../../CMakeLists.txt).
set_source_files_properties(${ROOT}/src/ui.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-missing-field-initializers")
target_link_libraries(main PRIVATE SDL2 cairo freetype jpeg android log)
