#!/bin/bash
# Helpers for the setB checks. Source it:  . tools/setb/t.sh
#   fresh <tag>            a fresh copy of data\ in shots/d_<tag>, made the data folder
#   shot <state> <name>    one still -> shots/b/<name>.png
#   seq <state> <name> <frames> <script>   frames -> shots/b/<name>/f_NNN.png
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE=${EXE:-$ROOT/build/dev.exe}
fresh() { rm -rf "$ROOT/shots/d_$1"; cp -r "$ROOT/data" "$ROOT/shots/d_$1"; export NSWHOME_DATA="$(cygpath -w "$ROOT/shots/d_$1")"; }
use() { export NSWHOME_DATA="$(cygpath -w "$ROOT/shots/d_$1")"; }
shot() { mkdir -p "$ROOT/shots/b"; "$EXE" --shot "$1" "$(cygpath -w "$ROOT/shots/b/$2.png")"; echo "shot $1 -> $? "; }
seq_() { mkdir -p "$ROOT/shots/b/$2"; rm -f "$ROOT/shots/b/$2"/*.png; "$EXE" --shotseq "$1" "$(cygpath -w "$ROOT/shots/b/$2")" "$3" "$4"; echo "seq $1 -> $?"; }
