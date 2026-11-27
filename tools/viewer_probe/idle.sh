#!/bin/bash
# Waits (up to $1 seconds, default 600) until no NSW-Home, test build or emulator is running. Exit 1 if still busy.
for i in $(seq 1 ${1:-600}); do
  n=$(powershell -NoProfile -Command "(Get-Process -ErrorAction SilentlyContinue | Where-Object { 'Ryujinx','Ryujinx.Ava','citron','eden','suyu','sudachi','yuzu','NSW-Home','dev' -contains \$_.ProcessName } | Measure-Object).Count" | tr -d '\r')
  [ "$n" = "0" ] && exit 0
  sleep 2
done
exit 1
