#!/bin/bash
# Scripted runs through every flow of a game's updates, DLC, mods and cheats
# (Splatoon 3, 0100c2500fc20000), offscreen, on the copies in this copy's data
# folder; prints each file's change and checks that undoing it gives back the
# original bytes. Puts config.json and the emulator copies back afterwards.
#   bash tools/content_flows.sh
cd "$(dirname "$0")/.."
export NSWHOME_DATA="$(cygpath -w "$PWD/data")"
T=+0100c2500fc20000
save=$(mktemp -d)
cp data/config.json "$save/"
cp -r data/ryujinx/games "$save/games"
for e in eden citron; do cp data/$e/config/qt-config.ini "$save/$e.ini"; done
G=data/ryujinx/games/0100c2500fc20000
fails=0
seq() { ./build/dev.exe --shotseq "$1" "" "$2" "$3"; }
same() { if cmp -s "$1" "$2"; then echo "PASS  $3"; else echo "FAIL  $3"; fails=$((fails + 1)); fi; }
gone() { if [ ! -e "$1" ]; then echo "PASS  $2"; else echo "FAIL  $2"; fails=$((fails + 1)); fi; }

echo "== Ryujinx: Datos de actualización -> Sin actualizar, then Versión 9.3.0"
seq "content$T-2" 130 "A@10,A@90"; diff "$save/games/0100c2500fc20000/updates.json" $G/updates.json
seq "content$T-2" 140 "A@10,DOWN@90,A@110"; same "$save/games/0100c2500fc20000/updates.json" $G/updates.json "updates.json back to the original"

echo "== Ryujinx: Gestionar contenido descargable -> first DLC off, then on"
seq "content$T-2" 130 "DOWN@10,A@20,A@100"; diff "$save/games/0100c2500fc20000/dlc.json" $G/dlc.json
seq "content$T-2" 130 "DOWN@10,A@20,A@100"; same "$save/games/0100c2500fc20000/dlc.json" $G/dlc.json "dlc.json back to the original"

echo "== Ryujinx: Mods -> first mod off, then on"
seq "content$T-2" 130 "DOWN@10,DOWN@20,A@30,A@110"; cat $G/mods.json; echo
seq "content$T-2" 130 "DOWN@10,DOWN@20,A@30,A@110"; gone $G/mods.json "mods.json made by NSW-Home is gone again"

echo "== Ryujinx: Trucos -> first cheat on, then off"
C=data/ryujinx/mods/contents/0100c2500fc20000/cheats/enabled.txt
seq "content$T-2" 140 "DOWN@10,DOWN@20,DOWN@30,A@40,A@120"; cat $C
seq "content$T-2" 140 "DOWN@10,DOWN@20,DOWN@30,A@40,A@120"; gone $C "enabled.txt made by NSW-Home is gone again"

I=data/citron/config/qt-config.ini
echo "== Citron: Usar los datos de actualización off, then on"
seq "content$T+citron-2" 130 "A@10,A@90"; diff "$save/citron.ini" $I
seq "content$T+citron-2" 130 "A@10,A@90"; same "$save/citron.ini" $I "Citron ini back to the original"

I=data/eden/config/qt-config.ini
echo "== Eden: Sin actualizar (Update@ keys; the folder of the DLC chosen above becomes an external content folder)"
seq "content$T+eden-2" 130 "A@10,A@90"; diff "$save/eden.ini" $I
cp $I "$save/eden_mid.ini"
echo "== Eden: DLC off, mod off, cheats folder off"
seq "content$T+eden-2" 130 "DOWN@10,A@20,A@100"
seq "content$T+eden-2" 130 "DOWN@10,DOWN@20,A@30,A@110"
seq "content$T+eden-2" 140 "DOWN@10,DOWN@20,DOWN@30,A@40,A@120"
diff "$save/eden_mid.ini" $I
echo "== Eden: the three back on"
seq "content$T+eden-2" 130 "DOWN@10,A@20,A@100"
seq "content$T+eden-2" 130 "DOWN@10,DOWN@20,A@30,A@110"
seq "content$T+eden-2" 140 "DOWN@10,DOWN@20,DOWN@30,A@40,A@120"
diff "$save/eden_mid.ini" $I && echo "PASS  Eden ini as before the three changes (ignoring the order of NSW-Home's own folders)"

echo "== A start applies a choice kept while a game was open (Mario Party Superstars, first on HOME)"
M=data/ryujinx/games/01006fe013472000
python - "$(cygpath -w "$PWD/data/config.json")" <<'EOF'
import json, sys
c = json.load(open(sys.argv[1], encoding='utf-8'))
c['console']['content.01006fe013472000.update'] = 'base'
json.dump(c, open(sys.argv[1], 'w', encoding='utf-8'), ensure_ascii=False)
EOF
cp $M/updates.json "$save/mps.json"
seq home-2 200 "A@10,A@70"; diff "$save/mps.json" $M/updates.json

# Everything as it was.
cp "$save/config.json" data/config.json
rm -rf data/ryujinx/games && cp -r "$save/games" data/ryujinx/games
for e in eden citron; do cp "$save/$e.ini" data/$e/config/qt-config.ini; rm -f data/$e/config/qt-config.ini.dockside-made; done
rm -rf "$save"
echo "$fails failed"
