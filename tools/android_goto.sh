#!/usr/bin/env bash
# Starts the Android debug build afresh on a named screen (ui::DebugGoto, as
# --shot does on a PC) and takes a picture of it.
#   bash tools/android_goto.sh <screen> [name]    -> shots/<name>.png, shots/<name>_s.jpg
# <screen> "-" starts the app as a user would (first-start assistant on a new install).
set -euo pipefail
cd "$(dirname "$0")/.."
ADB="${ADB:-$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe}"
screen="$1"
name="${2:-a_$(echo "$screen" | tr '+' '_')}"
"$ADB" shell am force-stop app.nswhome
if [ "$screen" = "-" ]; then
  "$ADB" shell am start -n app.nswhome/.NSWHomeActivity >/dev/null
else
  "$ADB" shell "am start -n app.nswhome/.NSWHomeActivity --es nosetup 1 --es goto '$screen'" >/dev/null
fi
sleep "${WAIT:-4}"
bash tools/android_shot.sh "$name"
