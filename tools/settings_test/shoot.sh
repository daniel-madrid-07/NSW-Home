#!/bin/bash
# usage: shoot.sh <state> [state...]  -> shots/s/<state>.png
cd "$(dirname "$0")/../.."
export NSWHOME_DATA="$(cygpath -w "$PWD/data")"
mkdir -p shots/s
for s in "$@"; do ./build/dev.exe --shot "$s" "$(cygpath -w "$PWD/shots/s/$s.png")"; done
