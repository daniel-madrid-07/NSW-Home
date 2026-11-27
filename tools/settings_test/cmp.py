# usage: cmp.py out.jpg ours.png ref.png [ours ref ...]  -> pairs side by side (ours left, capture right), half size
import sys
from PIL import Image
out=sys.argv[1]; a=sys.argv[2:]
n=len(a)//2
s=Image.new('RGB',(1930,n*545),(255,0,255))
for i in range(n):
    o=Image.open(a[2*i]).convert('RGB').resize((960,540),Image.LANCZOS)
    r=Image.open(a[2*i+1]).convert('RGB').resize((960,540),Image.LANCZOS)
    s.paste(o,(0,i*545)); s.paste(r,(970,i*545))
s.save(out,quality=92)
