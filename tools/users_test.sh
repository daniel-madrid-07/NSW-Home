#!/bin/bash
# Checks the rule for users and emulators' profiles (data.cpp, "which of an
# emulator's profiles a user is") end to end, offscreen, on stand-in emulators:
#   bash tools/users_test.sh
# shots/ut/emus holds a portable "Ryujinx" (empty program file; profiles
# RyuPlayer and Invitado, last used as RyuPlayer) and a "Citron" with its data
# beside it (one user, "citron"), both made from the copies in data/. NSW-Home
# runs on an empty data folder (a first start) and is driven through the
# assistant, a game start per emulator, naming the user, and adding users both
# ways. After each step the two profile files are compared with how the
# emulators left them. Nothing real is touched: NSWHOME_EMUDATA stands in
# for %APPDATA% and the emulators' folders are the stand-ins.
cd "$(dirname "$0")/.."
T=shots/ut
rm -rf $T
mkdir -p $T/data $T/appdata $T/emus/Ryujinx/portable/system $T/emus/Citron/user/nand/system/save/8000000000000010/su/avators $T/games
cp -r data/pack $T/data/ 2>/dev/null
RYU=$T/emus/Ryujinx/portable/system/Profiles.json
CIT=$T/emus/Citron/user/nand/system/save/8000000000000010/su/avators/profiles.dat
: > $T/emus/Ryujinx/Ryujinx.exe
: > $T/emus/Citron/citron.exe
head -c 4096 /dev/zero > "$T/games/Juego de prueba.nsp"
python - "$RYU" "$CIT" "$(cygpath -w "$PWD/$T/games")" "$T/emus/Ryujinx/portable/Config.json" <<'PY'
import json, sys
ryu, cit, games, cfg = sys.argv[1:5]
src = json.load(open('data/ryujinx/system/Profiles.json', encoding='utf-8-sig'))
keep = [p for p in src['profiles'] if p['user_id'] in ('00000000000000010000000000000000', 'b59d728aef8ec4b8623ffea599178912')]
for p in keep:
    if p['user_id'].startswith('0000'): p['name'] = 'RyuPlayer'
json.dump({'profiles': keep, 'last_opened': '00000000000000010000000000000000'}, open(ryu, 'w', encoding='utf-8'), indent=2)
d = bytearray(open('data/citron/nand/system/save/8000000000000010/su/avators/profiles.dat', 'rb').read())
d[0x10 + 0xC8:] = bytes(len(d) - 0x10 - 0xC8)  # only its own first user
open(cit, 'wb').write(d)
json.dump({'game_dirs': [games], 'show_console': False}, open(cfg, 'w'))
PY
cp $RYU $T/ryu.orig; cp $CIT $T/cit.orig
export NSWHOME_DATA="$(cygpath -w "$PWD/$T/data")" NSWHOME_EMUDATA="$(cygpath -w "$PWD/$T/appdata")" NSWHOME_LIVE=1
R="$(cygpath -w "$PWD/$T/emus/Ryujinx")"; C="$(cygpath -w "$PWD/$T/emus/Citron")"
fails=0
run() { ./build/dev.exe --log --shotseq "$1" "" "$2" "$3" || { echo "  CRASH in $1 ($?)"; fails=$((fails+1)); }; }
same() { if cmp -s "$1" "$2"; then echo "  ok   $3: byte-identical"; else echo "  FAIL $3: changed"; fails=$((fails+1)); fi; }
both() { same $RYU $T/ryu.orig "Ryujinx Profiles.json ($1)"; same $CIT $T/cit.orig "Citron profiles.dat ($1)"; }
expect() { if echo "$2" | grep -qF -- "$3"; then echo "  ok   $1: $3"; else echo "  FAIL $1: wanted [$3] in [$2]"; fails=$((fails+1)); fi; }
users() { python -c "
import json,sys
c=json.load(open(sys.argv[1],encoding='utf-8'))
print(' | '.join('%s %r %s' % (u['id'][:8], u['name'], ','.join(k+'='+v[:8] for k,v in sorted(u.get('emu',{}).items()))) for u in c['users']))" $T/data/config.json; }
# The command line a game start would use, for emulator $1 as the user at position $2 of the user sheet.
launch() {
    sed -i "s/\"defaultEmu\": \"[^\"]*\"/\"defaultEmu\": \"$1\"/" $T/data/config.json
    rm -f $T/data/debug.log
    local n; n=$(python -c "import json;print(len(json.load(open('$T/data/config.json',encoding='utf-8'))['users']))")
    local s="A@5"
    if [ "$n" -gt 1 ]; then s="A@5,LEFT@40,LEFT@44,LEFT@48"; for i in $(seq 1 $2); do s="$s,RIGHT@$((52 + i * 4))"; done; s="$s,A@80"; fi
    run home-2 400 "$s"
    grep -a "would launch" $T/data/debug.log | sed 's/^.*would launch: //; s/^"[^"]*" //; s/ *"[^"]*nsp"$//'
}

echo "1. first start; the assistant is given the two emulators, then goes on to the end"
run "setup-emu+first+pick=$R+pick=$C-2" 420 "A@0,DOWN@20,A@30,UP@50,A@60,DOWN@110,DOWN@115,A@120,DOWN@170,DOWN@175,A@180,DOWN@230,DOWN@235,DOWN@240,A@245,A@300"
expect "one user, no name, standing for each emulator's profile" "$(users)" "00000000 '' citron=b92807fc,ryujinx=00000000"
expect "only one user" "$(users | tr -cd '|' | wc -c)" "0"
both "after the first start"
expect "Ryujinx starts as its own profile" "$(launch ryujinx 0)" '--profile "RyuPlayer"'
expect "Citron starts as its own first record" "$(launch citron 0)" "-u 0 "
both "after game starts"

echo "2. the user gets a nickname and a picture in the assistant"
run "setup-form+name=Dani+pick=$(cygpath -w "$PWD/data/avatar.jpg")-2" 120 "A@0,DOWN@30,A@40"
expect "named" "$(users)" "00000000 'Dani' citron=b92807fc,ryujinx=00000000"
both "after naming the user and giving it a picture"
expect "Ryujinx still starts as its own profile" "$(launch ryujinx 0)" '--profile "RyuPlayer"'

echo "3. add a user: the emulator's existing profile (Invitado, Ryujinx's)"
run "user-new-2" 200 "DOWN@10,A@20,A@80"
expect "Invitado is a user, standing for Ryujinx's profile" "$(users)" "b59d728a 'Invitado' ryujinx=b59d728a"
same $RYU $T/ryu.orig "Ryujinx Profiles.json (its own profile was taken on, nothing to write)"
python - $CIT $T/cit.orig <<'PY'
import sys
new, old = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
rec = lambda d, i: d[0x10 + i * 0xC8: 0x10 + (i + 1) * 0xC8]
ok = len(new) == len(old) and new[:0x10] == old[:0x10] and rec(new, 0) == rec(old, 0) and rec(new, 1)[:16].hex() == 'b59d728aef8ec4b8623ffea599178912' \
     and rec(new, 1)[0x28:0x48].rstrip(b'\0') == b'Invitado' and all(rec(new, i) == rec(old, i) for i in range(2, 8))
print('  ok   Citron: its own record untouched, Invitado added as the second' if ok else '  FAIL Citron records')
sys.exit(0 if ok else 1)
PY
fails=$((fails+$?))
[ -f $CIT.dockside-backup ] && same $CIT.dockside-backup $T/cit.orig "Citron's backup of the file as it was"
expect "Ryujinx as the second user" "$(launch ryujinx 1)" '--profile "Invitado"'
expect "Ryujinx as the first user" "$(launch ryujinx 0)" '--profile "RyuPlayer"'
expect "Citron as the second user" "$(launch citron 1)" "-u 1 "
expect "Citron as the first user" "$(launch citron 0)" "-u 0 "

echo "4. add a user: a new one (Ana)"
run "type:Ana+user-new-2" 400 "A@10,A@120,A@180"
expect "Ana is a user with an id of her own and no profile of an emulator's" "$(users | sed 's/.*| //')" "'Ana' "
python - $RYU $T/ryu.orig $CIT $T/data/config.json <<'PY'
import json, sys
new, old = (json.load(open(f, encoding='utf-8-sig')) for f in sys.argv[1:3])
cfg = json.load(open(sys.argv[4], encoding='utf-8'))
ana = cfg['users'][-1]['id']
by = lambda j: {p['user_id']: p for p in j['profiles']}
n, o = by(new), by(old)
ok = set(n) == set(o) | {ana} and n[ana]['name'] == 'Ana' and all(n[k]['name'] == o[k]['name'] and n[k]['image'] == o[k]['image'] for k in o)
print('  ok   Ryujinx: Ana added; RyuPlayer and Invitado keep their names and pictures' if ok else '  FAIL Ryujinx list: %s' % [(p['user_id'][:8], p['name']) for p in new['profiles']])
d = open(sys.argv[3], 'rb').read()
rec = lambda i: d[0x10 + i * 0xC8: 0x10 + (i + 1) * 0xC8]
ok2 = rec(0)[0x28:0x48].rstrip(b'\0') == b'citron' and rec(2)[:16].hex() == ana and rec(2)[0x28:0x48].rstrip(b'\0') == b'Ana'
print('  ok   Citron: Ana added as the third; "citron" still first' if ok2 else '  FAIL Citron records')
sys.exit(0 if ok and ok2 else 1)
PY
fails=$((fails+$?))
expect "Ryujinx as Ana" "$(launch ryujinx 2)" '--profile "Ana"'
expect "Citron as Ana" "$(launch citron 2)" "-u 2 "
expect "Ryujinx as the first user, still" "$(launch ryujinx 0)" '--profile "RyuPlayer"'

echo "5. a second start imports nobody"
run "home-2" 30 "B@5"
expect "three users" "$(users | tr -cd '|' | wc -c)" "2"
echo
[ $fails = 0 ] && echo "ALL OK" || echo "FAILURES: $fails"
exit $fails
