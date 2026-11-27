#!/usr/bin/env bash
# Renders a scripted run offscreen and tiles every Nth frame into one picture,
# to look over a whole sequence of screens for glitches.
#
#   bash tools/tour.sh <screen> <frames> <out.jpg> "<script>" [step=5] [cell px=320] [columns=10]
#
# Run from the project folder. Uses build/dev.exe when present.
set -e
STEP="${5:-5}"
CELL="${6:-320}"
COLS="${7:-10}"
EXE=build/dev.exe
[ -f "$EXE" ] || EXE=build/NSW-Home.exe
rm -rf shots/tour && mkdir -p shots/tour
"$EXE" --shotseq "$1" "$(cygpath -w "$PWD/shots/tour")" "$2" "$4"
sleep 1
ROWS=$(( ($2 / STEP + COLS - 1) / COLS ))
F="select='not(mod(n,$STEP))',scale=$CELL:-1,drawtext=text='%{eif\:n*$STEP\:d}':x=2:y=2:fontsize=14:fontcolor=yellow:box=1:boxcolor=black@0.5:fontfile='C\:/Windows/Fonts/arial.ttf',tile=${COLS}x$ROWS"
ffmpeg -hide_banner -loglevel error -y -framerate 60 -i shots/tour/f_%03d.png -frames:v 1 -update 1 -vf "$F" "$3"
echo "wrote $3"
