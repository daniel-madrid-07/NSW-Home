#!/usr/bin/env bash
# Renders a list of screens with the Linux build, offscreen, for comparison
# with the Windows build's --shot pictures. Run inside Linux (or WSL):
#
#   [RYUJINX_DATA=<the emulator's data folder>] bash tools/linux_shots.sh <nsw-home binary> <data folder> <out folder> <screen> [<screen> ...]
#
# No window, sound or emulator is started (SDL's dummy drivers are used, and
# --shot never launches anything). The emulator's real folder is never
# written: XDG_CONFIG_HOME points at a throw-away folder. With RYUJINX_DATA
# set, that folder gets copies of the key files and links to the firmware and
# game-metadata folders, so the console font and the game pictures can be read
# as on Windows; it is deleted afterwards.
set -e
bin="$1"; data="$2"; out="$3"; shift 3
mkdir -p "$out"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp:?}"' EXIT
if [ -n "$RYUJINX_DATA" ]; then
    mkdir -p "$tmp/Ryujinx/system"
    cp "$RYUJINX_DATA"/system/*.keys "$tmp/Ryujinx/system/" 2>/dev/null || true
    for d in bis games; do
        [ -d "$RYUJINX_DATA/$d" ] && ln -s "$RYUJINX_DATA/$d" "$tmp/Ryujinx/$d"
    done
fi
export NSWHOME_DATA="$data" XDG_CONFIG_HOME="$tmp" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
for s in "$@"; do
    # A render that hangs must not stay behind.
    timeout 60 "$bin" --shot "$s" "$out/$s.png" && echo "$s ok" || echo "$s FAILED ($?)"
done
