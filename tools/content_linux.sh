#!/bin/bash
# Builds src/content.cpp and tools/content_test.cpp with GCC (with nsw.cpp and
# the port's compatibility layer, as the Linux build links them) and runs the
# writer checks on Linux, on copies: the Ryujinx family's per-game JSON as
# Linux spells paths, and the yuzu family's ini files.
#   wsl -d Ubuntu -- bash tools/content_linux.sh <ryujinx games copy> <ini>...
set -e
cd "$(dirname "$0")/.."
out=$(mktemp -d)
flags="-std=c++17 -Wall -Wextra"
g++ $flags -c src/content.cpp -o "$out/content.o"
g++ $flags -c tools/content_test.cpp -o "$out/test.o"
g++ -std=c++17 -Isrc/port/win -Isrc/port -Isrc -c src/nsw.cpp -o "$out/nsw.o"
g++ -std=c++17 -Isrc/port/win -Isrc/port -Isrc -c src/port/compat.cpp -o "$out/compat.o"
g++ "$out"/*.o -lpthread -o "$out/content_test"
games="$1"
shift
# The same files as the emulator writes them on Linux: forward slashes, LF.
cp -r "$games" "$out/games"
find "$out/games" -name '*.json' -exec sed -i -e 's#C:\\\\#/#g' -e 's#\\\\#/#g' -e 's/\r$//' {} \;
"$out/content_test" write "$out/work" "$out/games" "$@"
