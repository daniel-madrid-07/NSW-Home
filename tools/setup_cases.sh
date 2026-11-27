#!/bin/bash
# Stills of the first-start assistant's error and edge cases: each is the last
# frame of a short --shotseq on a throw-away copy of the data folder, with the
# fake emulator folders of tools/setup_fake.py. Output: shots/case/<name>.png.
cd "$(dirname "$0")/.."
python tools/setup_fake.py >/dev/null
F="$(cygpath -w "$PWD/shots/fake")"
E="$(cygpath -w "$PWD/shots/fake/appdata")"
mkdir -p shots/case
one() {
    n=$1; st=$2; fr=$3; sc=$4
    rm -rf shots/run_c shots/seq_c
    mkdir -p shots/run_c shots/seq_c
    cp -r data/avatars data/pack data/icons shots/run_c/
    cp "${CFG:-data/config.json}" shots/run_c/config.json
    cp data/library.json shots/run_c/
    NSWHOME_DATA="$(cygpath -w "$PWD/shots/run_c")" NSWHOME_EMUDATA="$E" \
        ./build/dev.exe --shotseq "$st" "$(cygpath -w "$PWD/shots/seq_c")" "$fr" "$sc" || echo "FAIL $n"
    cp "$(ls shots/seq_c/*.png | tail -1)" "shots/case/$n.png"
    rm -rf shots/seq_c
}
DIRS="+pick=D:\\Juegos\\Switch+pick=E:\\Copias de seguridad\\Consola\\Nintendo\\Switch\\Juegos en cartucho volcados\\2024+pick=$F\\a+pick=$F\\b+pick=$F\\c"
ADD5="UP@0,A@5,UP@15,A@20,UP@30,A@35,UP@45,A@50,UP@60,A@65"
one emu_wrong "setup-emu+noemu+pick=$F\\pick" 40 "A@0"
one emu_yuzu "setup-emu+noemu+pick=$F\\yuzu-2l" 40 "A@0"
one keys_wrong "setup-keys+nokeys+pick=$F\\pick\\wrong.keys" 40 "A@0"
one games_add "setup-games+nodirs+pick=$F\\games" 40 "A@0"
one name_taken "setup-user+name=Invitado" 60 "UP@0,A@10"
one entry "setup-form-2l" 40 "A@0"
one entry1 "setup-form-1" 40 "A@0"
one many "setup-games$DIRS" 140 "$ADD5,UP@80,UP@90,UP@100"
one many_top "setup-games$DIRS" 170 "$ADD5,UP@80,UP@90,UP@100,UP@110,UP@120,UP@130,UP@140"
one many1 "setup-games$DIRS-1l" 140 "$ADD5,UP@80,UP@90,UP@100"
CFG=shots/config_gone.json one emu_gone "setup-emu" 10 "X@0"
CFG=shots/config_gone.json one done_gone "setup-done" 10 "X@0"
one rm_dir "setup-games" 60 "UP@0,UP@5,A@10"
rm -rf shots/run_c
python - <<'PY'
from PIL import Image
d = 'shots/case/'
def sheet(names, out):
    ims = [Image.open(d + n + '.png').resize((960, 540)) for n in names]
    W = Image.new('RGB', (1920, 540 * ((len(ims) + 1) // 2)))
    for i, im in enumerate(ims):
        W.paste(im, ((i % 2) * 960, (i // 2) * 540))
    W.save(out, quality=88)
sheet(['emu_wrong', 'emu_yuzu', 'keys_wrong', 'games_add'], 'shots/sheet_f.jpg')
sheet(['name_taken', 'entry', 'entry1', 'many'], 'shots/sheet_g.jpg')
sheet(['many_top', 'emu_gone', 'done_gone', 'rm_dir', 'many1'], 'shots/sheet_h.jpg')
PY
