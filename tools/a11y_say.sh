#!/bin/bash
# What the screen reader would say during a scripted run (nothing is played:
# an offscreen run only logs). usage: a11y_say.sh <state> <frames> "<script>" [more a11y settings]
# Works on a throw-away copy of the configuration, so the toggles it presses are not kept.
cd "$(dirname "$0")/.."
export NSWHOME_DATA="$(cygpath -w "$PWD/data")"
cp data/config.json shots/config.keep
log="shots/tts/say.log"; mkdir -p shots/tts; rm -f "$log"
export NSWHOME_A11Y_LOG="$(cygpath -w "$PWD/$log")"
./build/dev.exe --shotseq "a11y:tts=on${4:+,$4}:$1" "" "$2" "$3"
cp shots/config.keep data/config.json
[ -f "$log" ] && cut -f2- "$log" || echo "(silent)"
