#!/bin/bash
# The in-game pause and applet code (src/main.cpp, "pause on HOME, and the
# games' applets") against tools/applet_standin.cpp's stand-in windows: off
# screen, cloaked, never activated; no emulator, no NSW-Home window.
#   bash tools/applet_test.sh [mode...]      (default: every mode)
# Needs build/dev.exe (build.bat dev) and build/applet_standin.exe
# (tools/build_applet_standin.bat). Prints, per mode, NSW-Home's log lines and
# what reached the stand-in.
cd "$(dirname "$0")/.."
export NSWHOME_DATA="$(cygpath -w "$PWD/data")"
out="$PWD/shots/applet_test"
mkdir -p "$out"
modes=("$@")
[ ${#modes[@]} -eq 0 ] && modes=(ryu-kb ryu-kb-off ryu-kb-cancel yuzu-kb yuzu-kb-cancel yuzu-inline ryu-pad yuzu-pad ryu-pause ryu-pause-active yuzu-pause)
for m in "${modes[@]}"; do
    case $m in
        yuzu-inline) text="Zelda 64 ñ" ;;
        ryu-kb-off) text="Ana" ;;
        *) text="Señor Ñandú 1" ;;
    esac
    rm -f "$out/$m.standin.log" data/debug.log
    ./build/applet_standin.exe "$m" "$(cygpath -w "$out/$m.standin.log")" &
    sp=$!
    ./build/dev.exe --log --applet-test "$m" "$text"
    rc=$?
    wait $sp
    cp data/debug.log "$out/$m.nswhome.log" 2>/dev/null
    echo "=== $m (exit $rc)"
    sed 's/^ *[0-9]*  /  nswhome: /' "$out/$m.nswhome.log" 2>/dev/null
    sed 's/^/  stand-in: /' "$out/$m.standin.log" 2>/dev/null
done
