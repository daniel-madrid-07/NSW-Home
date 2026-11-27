"""Compare the still geometry of NSW-Home's Switch 2 style with the owner's captures.

    python tools/geom_check.py cmp  <a.png> <b.png> x y w h <out.png> [zoom]
        a | b | amplified absolute difference of the same rectangle, side by side.
    python tools/geom_check.py scan <png> h <y> <x0> <x1> [tolerance]
    python tools/geom_check.py scan <png> v <x> <y0> <y1> [tolerance]
        the runs of (nearly) one colour along a scan line: where each starts,
        how long it is and its colour. An edge is where one run ends; a rule is
        a run one pixel long. Anti-aliased edges show as short in-between runs.
    python tools/geom_check.py both <capture> <seconds> <screen> h|v <fixed> <from> <to> [tolerance]
        the same scan in a capture frame and in our render, one after the other.

    python tools/geom_check.py edges <capture> <seconds> <screen> h|v <fixed> <from> <to> [tolerance]
        only the stretches that leave the ground colour, in both pictures
        ("-" for the capture or the screen skips that one).

    python tools/geom_check.py bbox <capture> <seconds> <screen> x y w h [tolerance]
        the box around everything in the rectangle that is not its ground
        (a line of text, an icon), in both pictures.

    python tools/geom_check.py lines h|v <capture> <seconds> <screen> [x y w h [least]]
        every long straight horizontal (vertical) edge in the rectangle, in
        both pictures: "row:from-to(grey before>grey after)". The quickest way
        to list the rules and box edges of a page.

Frames and renders come from tools/palette_check.py (same cache, same exact
seek), so "<capture> <seconds>" and "<screen>" mean what they mean there.
Coordinates are 1080p pixels; divide by 1.5 for layout units.
"""
import os
import sys

from PIL import Image, ImageChops

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import palette_check as pc  # noqa: E402


def runs(path, axis, fixed, a, b, tol=6):
    im = Image.open(path).convert("RGB")
    px = im.load()
    out, start, ref = [], a, None
    for i in range(a, b):
        p = px[i, fixed] if axis == "h" else px[fixed, i]
        if ref is None:
            ref = p
        elif max(abs(p[c] - ref[c]) for c in range(3)) > tol:
            out.append((start, i - start, ref))
            start, ref = i, p
    out.append((start, b - start, ref))
    return out


def show(path, axis, fixed, a, b, tol=6):
    print("%s  %s=%d  %d..%d" % (os.path.basename(path), "y" if axis == "h" else "x", fixed, a, b))
    line = []
    for s, n, c in runs(path, axis, fixed, a, b, tol):
        line.append("%d+%d(%s)" % (s, n, "%d" % c[0] if max(c) - min(c) < 6 else "%d,%d,%d" % c))
    print("  " + "  ".join(line))


def spans(path, axis, fixed, a, b, tol=6):
    """Stretches that are not the ground (the colour at the start of the scan):
    first and last pixel of each, its length, and the colour of its first,
    middle and last pixel (a half-covered edge pixel shows as an in-between)."""
    im = Image.open(path).convert("RGB")
    px = im.load()
    get = (lambda i: px[i, fixed]) if axis == "h" else (lambda i: px[fixed, i])
    ground = get(a)
    out, start = [], None
    for i in range(a, b + 1):
        off = i < b and max(abs(get(i)[c] - ground[c]) for c in range(3)) > tol
        if off and start is None:
            start = i
        elif not off and start is not None:
            out.append((start, i - 1))
            start = None
    def g(i):
        c = get(i)
        return "%d" % c[0] if max(c) - min(c) < 6 else "%d,%d,%d" % c
    print("%s  %s=%d  %d..%d  ground %s" % (os.path.basename(path)[:22], "y" if axis == "h" else "x", fixed, a, b, g(a)))
    print("  " + "  ".join("%d-%d[%d](%s|%s|%s)" % (s, e, e - s + 1, g(s), g((s + e) // 2), g(e)) for s, e in out))


def cmp(a, b, rect, out, zoom=1):
    x, y, w, h = rect
    ia = Image.open(a).convert("RGB").crop((x, y, x + w, y + h))
    ib = Image.open(b).convert("RGB").crop((x, y, x + w, y + h))
    d = ImageChops.difference(ia, ib).point(lambda v: min(255, v * 4))
    sheet = Image.new("RGB", (w * 3 + 8, h), (255, 0, 255))
    sheet.paste(ia, (0, 0))
    sheet.paste(ib, (w + 4, 0))
    sheet.paste(d, (2 * w + 8, 0))
    if zoom != 1:
        sheet = sheet.resize((int(sheet.width * zoom), int(sheet.height * zoom)), Image.NEAREST)
    sheet.save(out)


def main():
    a = sys.argv[1:]
    if a[0] == "bbox":
        main_bbox()
    elif a[0] == "lines":
        main_lines(a[1:])
    elif a[0] == "cmp":
        cmp(a[1], a[2], tuple(int(v) for v in a[3:7]), a[7], float(a[8]) if len(a) > 8 else 1)
    elif a[0] == "scan":
        show(a[1], a[2], int(a[3]), int(a[4]), int(a[5]), int(a[6]) if len(a) > 6 else 6)
    elif a[0] == "edges":  # edges <capture> <seconds> <screen> h|v <fixed> <from> <to> [tolerance]
        tol = int(a[8]) if len(a) > 8 else 6
        if a[1] != "-":
            spans(pc.frame(a[1], float(a[2])), a[4], int(a[5]), int(a[6]), int(a[7]), tol)
        if a[3] != "-":
            spans(pc.shot(a[3]), a[4], int(a[5]), int(a[6]), int(a[7]), tol)
    elif a[0] == "both":
        tol = int(a[8]) if len(a) > 8 else 6
        show(pc.frame(a[1], float(a[2])), a[4], int(a[5]), int(a[6]), int(a[7]), tol)
        show(pc.shot(a[3]), a[4], int(a[5]), int(a[6]), int(a[7]), tol)




def bbox(path, rect, tol=40):
    """Bounding box (x0, y0, x1, y1, inclusive) of everything in `rect` that
    differs from the rectangle's most common colour by more than `tol`."""
    x, y, w, h = rect
    im = Image.open(path).convert("RGB").crop((x, y, x + w, y + h))
    px = list(im.getdata())
    counts = {}
    for p in px:
        counts[p] = counts.get(p, 0) + 1
    ground = max(counts, key=counts.get)
    xs, ys = [], []
    for i, p in enumerate(px):
        if max(abs(p[c] - ground[c]) for c in range(3)) > tol:
            xs.append(i % w)
            ys.append(i // w)
    if not xs:
        return None
    return (x + min(xs), y + min(ys), x + max(xs), y + max(ys))


def straight(path, rect, vertical=False, least=80, thr=10):
    """Long straight edges in `rect`: every row (column, if `vertical`) at which
    the picture changes from the row before along a stretch of at least `least`
    pixels. Returns (row, from, to, grey before, grey after). A one-pixel rule
    gives two edges on consecutive rows, a rule spread over two rows gives three."""
    import numpy as np
    x, y, w, h = rect
    a = np.asarray(Image.open(path).convert("RGB").crop((x, y, x + w, y + h))).astype(int)
    if vertical:
        a = a.transpose(1, 0, 2)
    d = np.abs(a[1:] - a[:-1]).max(axis=2) > thr
    out = []
    for r in range(d.shape[0]):
        row, start = d[r], None
        for i in range(len(row) + 1):
            on = i < len(row) and row[i]
            if on and start is None:
                start = i
            elif not on and start is not None:
                if i - start >= least:
                    m = (start + i) // 2
                    out.append((r + 1, start, i - 1, int(a[r, m].mean()), int(a[r + 1, m].mean())))
                start = None
    o0, o1 = (x, y) if vertical else (y, x)
    return [(r + o0, s + o1, e + o1, b, c) for r, s, e, b, c in out]


def main_lines(a):  # lines h|v <capture> <seconds> <screen> [x y w h [least]]
    vertical = a[0] == "v"
    rect = tuple(int(v) for v in a[4:8]) if len(a) >= 8 else (0, 0, 1920, 1080)
    least = int(a[8]) if len(a) > 8 else 80
    for name, path in (("console", pc.frame(a[1], float(a[2])) if a[1] != "-" else None),
                       ("nswhome", pc.shot(a[3]) if a[3] != "-" else None)):
        if path:
            print(name)
            print("  " + "  ".join("%d:%d-%d(%d>%d)" % e for e in straight(path, rect, vertical, least)))


def main_bbox():  # bbox <capture> <seconds> <screen> x y w h [tol]
    a = sys.argv[2:]
    r = tuple(int(v) for v in a[3:7])
    tol = int(a[7]) if len(a) > 7 else 40
    if a[0] != "-":
        print("console ", bbox(pc.frame(a[0], float(a[1])), r, tol))
    if a[2] != "-":
        print("nswhome", bbox(pc.shot(a[2]), r, tol))


if __name__ == "__main__":
    main()
