#!/bin/bash
# Runs tools/emu_test.ps1 with the given arguments as soon as no other NSW-Home / emulator is running
# (other tests share this screen); output in shots/run_<name>.txt. Usage: run.sh <name> <emu_test arguments...>
cd "$(dirname "$0")/../.."
name=$1; shift
for try in $(seq 1 60); do
  bash tools/viewer_probe/idle.sh 900 || { echo "still busy"; exit 1; }
  powershell -NoProfile -ExecutionPolicy Bypass -File tools/emu_test.ps1 "$@" > "shots/run_$name.txt" 2>&1
  grep -q "^NOT STARTED: already running" "shots/run_$name.txt" || exit 0
  sleep $((RANDOM % 5 + 2))
done
exit 1
