#!/bin/bash
# Runs a --shotseq of the first-start assistant on a throw-away copy of the
# data folder and tiles every <step>th frame into one picture.
#   tools/setup_seq.sh <name> <state> <frames> "<script>" [step] [emudata folder]
# Output: shots/seq_<name>.jpg, the config the run left in shots/run_<name>/config.json.
# The real emulator folders are only ever read; pass an emudata folder (a
# stand-in for %APPDATA%) for any run that changes users or copies keys.
set -e
cd "$(dirname "$0")/.."
name=$1; state=$2; frames=$3; script=$4; step=${5:-20}; emudata=$6
run=shots/run_$name
rm -rf "$run" "shots/seq_$name"
mkdir -p "$run" "shots/seq_$name"
cp -r data/avatars data/pack data/icons "$run/" 2>/dev/null || true
[ -n "$KEEP_CONFIG" ] && cp data/config.json data/library.json "$run/"
[ -n "$CONFIG" ] && cp "$CONFIG" "$run/config.json"
export NSWHOME_DATA="$(cygpath -w "$PWD/$run")"
[ -n "$emudata" ] && export NSWHOME_EMUDATA="$(cygpath -w "$emudata")"
./build/dev.exe --shotseq "$state" "$(cygpath -w "$PWD/shots/seq_$name")" "$frames" "$script"
echo "exit $?"
python - "$name" "$step" <<'PY'
import sys, glob
from PIL import Image
name, step = sys.argv[1], int(sys.argv[2])
files = sorted(glob.glob('shots/seq_%s/f_*.png' % name))[::step]
cols = 4
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (480 * cols, 270 * rows))
for i, f in enumerate(files):
    sheet.paste(Image.open(f).resize((480, 270)), ((i % cols) * 480, (i // cols) * 270))
sheet.save('shots/seq_%s.jpg' % name, quality=85)
print(len(files), 'frames tiled')
PY
rm -rf "shots/seq_$name"
