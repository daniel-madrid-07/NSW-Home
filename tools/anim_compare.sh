#!/bin/bash
# Render one of NSW-Home's animations offscreen and stack it, frame by frame,
# over the same crop of a capture of the console, so the two can be compared.
#
# usage: anim_compare.sh <screen> <frames> <crop w:h:x:y> <reference video> <reference start s> <out.jpg> [script] [step] [cell]
#
#   screen      a --shot screen name, e.g. sys-2 (see ui::DebugGoto)
#   frames      how many frames to render (60 per second)
#   crop        region of the 1920x1080 picture to show, ffmpeg crop syntax
#   reference   capture file and the time, in seconds, of the frame that
#               corresponds to frame 0 of ours (normally the button press)
#   script      --shotseq script, default "A@0"   (e.g. "A@0,B@120")
#   step        show every Nth frame, default 2
#   cell        width in pixels of each cell in the sheet, default 140
#
# Output: top rows = ours, bottom rows = the console, 16 cells per row,
# each cell labelled with its frame number. Uses build/dev.exe (build.bat dev).
set -e
cd "$(dirname "$0")/.."
# A project copy with its own data folder renders from that, not from %APPDATA%.
[ -d data ] && export NSWHOME_DATA="$(cygpath -w "$PWD/data")"
SCRIPT="${7:-A@0}"
STEP="${8:-2}"
CELL="${9:-140}"
EXE=build/dev.exe
[ -f "$EXE" ] || EXE=build/NSW-Home.exe
rm -rf shots/seq && mkdir -p shots/seq
"$EXE" --shotseq "$1" "$(cygpath -w "$PWD/shots/seq")" "$2" "$SCRIPT"
sleep 1
ROWS=$(( ($2 / STEP + 15) / 16 ))
F="select='not(mod(n,$STEP))',crop=$3,scale=$CELL:-1,drawtext=text='%{eif\:n*$STEP\:d}':x=2:y=2:fontsize=16:fontcolor=yellow:box=1:boxcolor=black@0.5:fontfile='C\:/Windows/Fonts/arial.ttf',tile=16x$ROWS"
ffmpeg -hide_banner -loglevel error -y -framerate 60 -i shots/seq/f_%03d.png -frames:v 1 -update 1 -vf "$F" shots/seq_ours.jpg
# Seek in two steps: a fast jump to five seconds before, then exact decoding up
# to the frame. A fast jump alone lands on the wrong frames in some captures.
PRE="$(python -c "print(max(0.0, $5 - 5))")"
FINE="$(python -c "print($5 - max(0.0, $5 - 5))")"
ffmpeg -hide_banner -loglevel error -y -ss "$PRE" -i "$4" -ss "$FINE" -t "$(python -c "print($2/60)")" -frames:v 1 -update 1 -vf "$F" shots/seq_ref.jpg
ffmpeg -hide_banner -loglevel error -y -i shots/seq_ours.jpg -i shots/seq_ref.jpg -filter_complex vstack "$6"
echo "wrote $6"
