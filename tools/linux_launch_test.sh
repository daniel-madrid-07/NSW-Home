#!/usr/bin/env bash
# Checks, on Linux, how the SDL build finds, identifies and starts emulators,
# and what it writes into their folders: with stand-in programs in a throw-away
# home folder, no window (SDL's dummy drivers) and no sound.
#
#   bash tools/linux_launch_test.sh <nsw-home binary>
#
# Two runs. First with no emulator folder anywhere: nothing may be written
# outside NSW-Home's own folder. Then with the folders an installed Ryujinx
# fork and Eden would have: users are mirrored and the window place is written.
set -u
bin="$1"
home="$(mktemp -d /tmp/nswhome_launch_XXXXXX)"
trap 'rm -rf "${home:?}"' EXIT
mkdir -p "$home/.local/bin" "$home/games" "$home/.config/NSW-Home"
: > "$home/games/Juego de prueba.nsp"

# A stand-in records the arguments it was started with. The one called Ryujinx
# carries a fork's project address the way a real build does (UTF-16), after
# the script's end, so the shared identification has something to find.
for name in Ryujinx eden; do
    printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "%s/args_%s.txt"\nsleep 1\nexit 0\n' "$home" "$name" > "$home/.local/bin/$name"
    chmod +x "$home/.local/bin/$name"
done
printf 'ryubing/ryujinx' | iconv -f UTF-8 -t UTF-16LE >> "$home/.local/bin/Ryujinx"

former=DOCKSIDE # historical name of the variables, still read by the program
run() { # $1 = default emulator to force ("" = leave)
    if [ -n "$1" ]; then sed -i "s/\"defaultEmu\": \"[a-z]*\"/\"defaultEmu\": \"$1\"/" "$home/.config/NSW-Home/config.json"; fi
    env -u NSWHOME_DATA -u NSWHOME_EMUDATA -u ${former}_DATA -u ${former}_EMUDATA -u XDG_CONFIG_HOME -u XDG_DATA_HOME HOME="$home" PATH="$home/.local/bin:/usr/bin:/bin" \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NSWHOME_TEST_SPAWN=1 \
        timeout 60 "$bin" --no-setup --frames 600 "A@100,A@170" > "$home/out.txt" 2>&1
    tail -1 "$home/out.txt" | cut -c1-120
}
args() { for f in "$home"/args_*.txt; do [ -f "$f" ] && echo "  $(basename "$f" .txt): $(tr '\n' '|' < "$f")" && rm -f "$f"; done; }
tree() { (cd "$home" && find . -type f ! -path './.local/bin/*' ! -name out.txt ! -name 'args_*' | sort | sed 's/^/  /'); }

cat > "$home/.config/NSW-Home/config.json" <<EOF
{"setupDone": true, "emuIdent": 1, "defaultEmu": "", "gameDirs": ["$home/games"], "users": [{"id": "00000000000000010000000000000000", "name": "Ana María"}, {"id": "00000000000000020000000000000000", "name": "Luis"}], "currentUser": 1}
EOF

# Ryujinx wants a picture for every profile, so the users have one (any JPEG does).
mkdir -p "$home/.config/NSW-Home/avatars"
printf 'ÿØÿÙ' > "$home/.config/NSW-Home/avatars/00000000000000010000000000000000.jpg"
cp "$home/.config/NSW-Home/avatars/00000000000000010000000000000000.jpg" "$home/.config/NSW-Home/avatars/00000000000000020000000000000000.jpg"

echo "== run 1: no emulator folders"
run ""
grep -E '"(defaultEmu|emulators)"' "$home/.config/NSW-Home/config.json" | sed "s|$home|~|g; s/^/ /"
args
echo " files:"; tree

echo "== run 2: the fork's and Eden's folders exist; started in the fork"
mkdir -p "$home/.config/Ryujinx/system" "$home/.local/share/eden/config"
run ryubing
args
echo "== run 3: started in Eden"
run eden
args
echo " files:"; tree
echo " users in Profiles.json: $(grep -c '"user_id"' "$home/.config/Ryujinx/system/Profiles.json" 2>/dev/null)"
