#!/usr/bin/env bash
# Screenshot of the virtual device, upright, for looking at the Android build.
#   bash tools/android_shot.sh <name>     -> shots/<name>.png (the 16:9 picture, 1920x1080 on a
#                                            1080-pixel-tall display) and shots/<name>_s.jpg (half size)
# `adb exec-out screencap` returns a black picture when the virtual device runs
# without a window on the host's graphics card, so the emulator's own console
# command takes the picture; it comes out in the display's portrait
# orientation and is turned here.
set -euo pipefail
cd "$(dirname "$0")/.."
ADB="${ADB:-$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe}"
name="$1"
tmp="shots/.emu"
rm -rf "$tmp"
mkdir -p "$tmp"
"$ADB" emu screenrecord screenshot "$(cygpath -w "$PWD/$tmp")" >/dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do
  f=$(ls "$tmp"/*.png 2>/dev/null | head -1 || true)
  [ -n "$f" ] && [ -s "$f" ] && break
  sleep 0.3
done
sleep 0.3
py -3 - "$f" "shots/$name" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
if im.height > im.width:
    im = im.rotate(90, expand=True)
# The layout is 16:9, centred in whatever the display is.
w = im.height * 16 // 9
if w < im.width:
    left = (im.width - w) // 2
    im = im.crop((left, 0, left + w, im.height))
im.save(sys.argv[2] + '.png')
im.resize((im.width // 2, im.height // 2), Image.LANCZOS).save(sys.argv[2] + '_s.jpg', quality=88)
print(im.size)
EOF
rm -rf "$tmp"
