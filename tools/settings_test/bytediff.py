# Prints the differing regions of two files, with context; exits 0.
import sys, difflib
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
if a==b: print("IDENTICAL"); sys.exit()
sm=difflib.SequenceMatcher(None,a,b,autojunk=False)
n=0
for tag,i1,i2,j1,j2 in sm.get_opcodes():
    if tag=='equal': continue
    n+=1
    ls=a.rfind(b'\n',0,i1)+1; le=a.find(b'\n',i2)
    print(f"change {n}: bytes {i1}-{i2} {a[i1:i2]!r} -> {b[j1:j2]!r}   line: {a[ls:le].strip()!r}")
print("regions changed:",n, "sizes",len(a),len(b))
