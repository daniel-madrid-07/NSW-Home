#!/usr/bin/env bash
# Downloads the libraries the Android build compiles from source into
# android/third_party, each from its project's own release page, and checks
# them against the checksums of the files this build was made with.
#
#   bash android/fetch_deps.sh        (Git Bash on Windows, or any Unix shell)
#
# After that the project builds from Android Studio (open the android/ folder)
# or from a shell:
#
#   cd android && ./gradlew assembleDebug        -> app/build/outputs/apk/debug/app-debug.apk
#
# Needed from the Android SDK: platform 35, build-tools 36.1.0, NDK
# 28.2.13676358 and CMake 3.22.1 (Android Studio offers to install the last
# two when they are missing; `sdkmanager "ndk;28.2.13676358" "cmake;3.22.1"`
# does the same). local.properties must name the SDK (sdk.dir=...); Android
# Studio writes it when the project is opened.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p third_party/.download
cd third_party

# name | folder the archive unpacks to | URL | SHA-256
deps=(
  "SDL2-2.32.10.tar.gz|SDL2-2.32.10|https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz|5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
  "freetype-VER-2-13-3.tar.gz|freetype-VER-2-13-3|https://gitlab.freedesktop.org/freetype/freetype/-/archive/VER-2-13-3/freetype-VER-2-13-3.tar.gz|bc5c898e4756d373e0d991bab053036c5eb2aa7c0d5c67e8662ddc6da40c4103"
  "libpng-1.6.50.tar.gz|libpng-1.6.50|https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.50.tar.gz|71158e53cfdf2877bc99bcab33641d78df3f48e6e0daad030afe9cb8c031aa46"
  "libjpeg-turbo-3.1.2.tar.gz|libjpeg-turbo-3.1.2|https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.2/libjpeg-turbo-3.1.2.tar.gz|8f0012234b464ce50890c490f18194f913a7b1f4e6a03d6644179fa0f867d0cf"
  "pixman-0.46.4.tar.gz|pixman-0.46.4|https://www.cairographics.org/releases/pixman-0.46.4.tar.gz|d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c"
  "cairo-1.18.4.tar.xz|cairo-1.18.4|https://www.cairographics.org/releases/cairo-1.18.4.tar.xz|445ed8208a6e4823de1226a74ca319d3600e83f6369f99b14265006599c32ccb"
)

for dep in "${deps[@]}"; do
  IFS='|' read -r file dir url sum <<<"$dep"
  if [ -d "$dir" ]; then
    echo "have $dir"
    continue
  fi
  if [ ! -f ".download/$file" ]; then
    echo "downloading $file"
    curl -fsSL -A "curl" -o ".download/$file.part" "$url"
    mv ".download/$file.part" ".download/$file"
  fi
  got=$(sha256sum ".download/$file" | cut -d' ' -f1)
  if [ "$got" != "$sum" ]; then
    echo "checksum mismatch for $file: $got" >&2
    exit 1
  fi
  # (pixman's archive holds a few symbolic links among its CI files, which a
  # Windows tar cannot create; nothing the build uses is affected.)
  tar xf ".download/$file" || [ -d "$dir" ]
  echo "unpacked $dir"
done
echo "done"
