#!/bin/bash
# Stills of the given build against the untouched one (build/base.exe), taken
# in the same minute (the status bar shows the clock). Prints "same" or the
# box that differs.
cd "$(dirname "$0")/.."
export NSWHOME_DATA="$(cygpath -w "$PWD/data")"
mkdir -p shots/same
for s in "$@"; do
  while [ "$(date +%S)" -gt 50 ]; do :; done
  ./build/base.exe --shot "$s" "$(cygpath -w "$PWD/shots/same/base_$s.png")"
  ./build/dev.exe --shot "$s" "$(cygpath -w "$PWD/shots/same/dev_$s.png")"
  if cmp -s "shots/same/base_$s.png" "shots/same/dev_$s.png"; then echo "same  $s"; else
    python -c "
from PIL import Image, ImageChops
import sys
a=Image.open(sys.argv[1]).convert('RGB'); b=Image.open(sys.argv[2]).convert('RGB')
print('DIFF ', sys.argv[3], ImageChops.difference(a,b).getbbox())" "shots/same/base_$s.png" "shots/same/dev_$s.png" "$s"; fi
done
