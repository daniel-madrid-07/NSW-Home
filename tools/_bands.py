"""bands <capture> <seconds> <screen> x y w h [tol]: the rows of the rectangle that hold ink (text lines), as bands."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import palette_check as pc
import numpy as np
from PIL import Image
def bands(path, rect, tol=50, vertical=False):
    x, y, w, h = rect
    a = np.asarray(Image.open(path).convert("RGB").crop((x, y, x + w, y + h))).astype(int)
    flat = a.reshape(-1, 3)
    vals, counts = np.unique(flat, axis=0, return_counts=True)
    ground = vals[counts.argmax()]
    ink = (np.abs(a - ground).max(axis=2) > tol)
    prof = ink.any(axis=0 if vertical else 1)
    out, start = [], None
    for i in range(len(prof) + 1):
        on = i < len(prof) and prof[i]
        if on and start is None: start = i
        elif not on and start is not None:
            out.append((start + (x if vertical else y), i - 1 + (x if vertical else y))); start = None
    return out
if __name__ == "__main__":
    a = sys.argv[1:]
    vertical = a[0] == "v"
    if vertical: a = a[1:]
    rect = tuple(int(v) for v in a[3:7]); tol = int(a[7]) if len(a) > 7 else 50
    if a[0] != "-": print("console ", " ".join("%d-%d" % b for b in bands(pc.frame(a[0], float(a[1])), rect, tol, vertical)))
    if a[2] != "-": print("nswhome", " ".join("%d-%d" % b for b in bands(pc.shot(a[2]), rect, tol, vertical)))
