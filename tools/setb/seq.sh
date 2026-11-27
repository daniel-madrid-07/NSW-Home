#!/bin/bash
# The scripted runs behind the setB report. Each works on shots/d_run (a copy of data\),
# writes frames to shots/b/<name>/ and keeps only the frames named after the script.
#   keep <name> <frame>...   deletes every other frame of that run
. "$(dirname "${BASH_SOURCE[0]}")/t.sh"
keep() { local d="$ROOT/shots/b/$1"; shift; local k=" $* "; for f in "$d"/f_*.png; do n=$(basename "$f" .png); n=$((10#${n#f_})); [[ "$k" == *" $n "* ]] || rm -f "$f"; done; }
cons() { python - "$NSWHOME_DATA" <<'PY'
import json,sys,os
c=json.load(open(os.path.join(sys.argv[1],'config.json'),encoding='utf-8'))['console']
print('   console:', {k:v for k,v in c.items() if not k.startswith('removed.')})
PY
}
# setcos <key> <value|-> : writes (or removes) one console key in the run's config
setcos() { python - "$NSWHOME_DATA" "$1" "$2" <<'PY'
import json,sys,os
p=os.path.join(sys.argv[1],'config.json'); c=json.load(open(p,encoding='utf-8'))
if sys.argv[3]=='-': c['console'].pop(sys.argv[2],None)
else: c['console'][sys.argv[2]]=sys.argv[3]
json.dump(c,open(p,'w',encoding='utf-8'),ensure_ascii=False,indent=1)
PY
}
