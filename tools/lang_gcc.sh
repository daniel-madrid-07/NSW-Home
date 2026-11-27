#!/bin/bash
# Compiles the language work's sources with GCC (the Linux/macOS and Android builds compile the
# same files): wsl -d Ubuntu -- bash /mnt/c/.../tools/lang_gcc.sh
cd "$(dirname "$0")/.." || exit 1
out=$(mktemp -d)
status=0
for f in src/lang.cpp src/fwtext.cpp src/fwfont.cpp src/port/gfx_cairo.cpp src/ui.cpp; do
    extra=""
    [ "$f" = src/ui.cpp ] && extra="-Wno-unused-function -Wno-missing-field-initializers"
    if g++ -std=c++17 -Wall -Wextra $extra -c -Isrc/port/win -Isrc/port -Isrc -I/usr/include/freetype2 \
        -I/usr/include/cairo "$f" -o "$out/$(basename "$f").o" 2> "$out/err.txt"; then
        echo "ok   $f ($(grep -c 'warning:' "$out/err.txt") warnings)"
    else
        echo "FAIL $f"
        status=1
    fi
    grep -E 'warning:|error:' "$out/err.txt" | head -20
done
rm -rf "$out"
exit $status
