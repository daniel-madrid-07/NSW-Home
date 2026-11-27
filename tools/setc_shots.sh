#!/usr/bin/env bash
# Offscreen runs of the Settings rows of Audio por Bluetooth, Internet, Control
# parental, Gestión de datos, Perfiles de usuario and Sonido: one scripted run
# per row, each on a fresh copy of the data folder holding a stand-in emulator
# tree (tools\setc_test.cpp, mode "tree"), with the files before and after
# listed beside the pictures. Nothing real is touched: see scTest in ui.cpp.
#
#   bash tools/setc_shots.sh            (needs build\dev.exe and build\setc_test.exe)
#
# Output: shots\setc\<run>\sheet.jpg (every 12th frame), f_*.png of the frames
# named in the run, and before/after listings.
set -u
cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="$ROOT/shots/setc"
mkdir -p "$OUT"

fresh() { # a new data folder with the stand-in tree; echoes its path
    local d="$ROOT/build/data_seq"
    rm -rf "$d"
    cp -r "$ROOT/data" "$d"
    "$ROOT/build/setc_test.exe" tree "$(cygpath -w "$d/standin")" >/dev/null
    echo "$d"
}

listing() { # relative paths and sizes of everything under the stand-in tree, and the config's console keys
    (cd "$1/standin" && find . -type f -printf '%s\t%p\n' | sort -k2) >"$2.files.txt"
    python - "$1/config.json" >"$2.config.txt" <<'EOF'
import json, sys
c = json.load(open(sys.argv[1], encoding="utf-8"))
for k, v in sorted(c.get("console", {}).items()):
    if k.startswith("parental.pin"):
        v = "<salt>:<hash> (%d characters)" % len(v)
    print("console[%s] = %s" % (k, v))
print("users =", [u["name"] for u in c.get("users", [])])
EOF
}

run() { # run <name> <screen> <frames> <script> [frames to keep...]
    local name="$1" screen="$2" frames="$3" script="$4"
    shift 4
    local d
    d="$(fresh)"
    [ -n "${PREP:-}" ] && eval "$PREP"
    local dir="$OUT/$name"
    rm -rf "$dir"
    mkdir -p "$dir/all"
    listing "$d" "$dir/before"
    NSWHOME_DATA="$(cygpath -w "$d")" "$ROOT/build/dev.exe" --shotseq "$screen" "$(cygpath -w "$dir/all")" "$frames" "$script"
    echo "$name: exit $?"
    listing "$d" "$dir/after"
    diff "$dir/before.files.txt" "$dir/after.files.txt" >"$dir/files.diff"
    diff "$dir/before.config.txt" "$dir/after.config.txt" >"$dir/config.diff"
    ffmpeg -hide_banner -loglevel error -y -start_number 0 -i "$dir/all/f_%03d.png" \
        -vf "select='not(mod(n\,12))',scale=384:-1,tile=5x6" -frames:v 1 -update 1 "$dir/sheet.jpg"
    for f in "$@"; do cp "$dir/all/f_$(printf %03d "$f").png" "$dir/"; done
    rm -rf "$dir/all"
}

still() { # still <name> <screen>
    local d
    d="$(fresh)"
    [ -n "${PREP:-}" ] && eval "$PREP"
    NSWHOME_DATA="$(cygpath -w "$d")" "$ROOT/build/dev.exe" --shot "$2" "$(cygpath -w "$OUT/$1.png")"
}

# An Eden-family set-up for "Sonido del televisor": Eden as the emulator games
# start with, and a copy of an Eden settings file where a test run looks for it.
eden_prep='python - "$d" <<EOF2
import json, os, shutil, sys
d = sys.argv[1]
p = os.path.join(d, "config.json")
c = json.load(open(p, encoding="utf-8"))
c["defaultEmu"] = "eden"
json.dump(c, open(p, "w", encoding="utf-8"), ensure_ascii=False)
os.makedirs(os.path.join(d, "eden", "config"), exist_ok=True)
shutil.copy(os.environ["EDEN_INI"], os.path.join(d, "eden", "config", "qt-config.ini"))
EOF2'

"$@"
