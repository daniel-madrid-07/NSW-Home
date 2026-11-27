import sys
from PIL import Image
out=sys.argv[1]; files=sys.argv[2:]
cols=3; w=640; h=360
rows=(len(files)+cols-1)//cols
s=Image.new('RGB',(cols*w,rows*h))
for i,f in enumerate(files):
    s.paste(Image.open(f).convert('RGB').resize((w,h),Image.LANCZOS),((i%cols)*w,(i//cols)*h))
s.save(out,quality=90)
