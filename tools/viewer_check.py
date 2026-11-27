"""Reads the screenshots tools\\emu_test.ps1 took of NSW-Home's window and says, in numbers:

    python tools/viewer_check.py <tag>          tag = ryujinx, eden, ryubing_win ...

  - live: how much of the picture changed between the two in-game shots taken a second apart;
  - edge to edge: for every in-game shot, how many rows / columns at each edge are plain black
    (0 everywhere = the picture fills NSW-Home's 16:9 area);
  - bars: for every in-game shot, the rows where an emulator's title bar, menu bar (top) or status
    bar (bottom) would be: how many of the outer 64 rows at the top and bottom are one flat colour
    from side to side, and how far those rows are from the same rows of the first in-game shot.
    A title, menu or status bar is a run of flat rows that the first shot does not have.

Also writes <tag>_sheet.jpg: all shots side by side, small, to look at.
"""
import glob
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageStat

root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "shots")
tag = sys.argv[1]
files = sorted(glob.glob(os.path.join(root, tag + "_[0-9]_*.png")))
if not files:
    sys.exit("no shots for " + tag)
imgs = {os.path.basename(f)[len(tag) + 1:-4]: Image.open(f).convert("RGB") for f in files}


def picture(im):
    """The 16:9 area NSW-Home draws in (letterboxed in a window of another shape)."""
    w, h = im.size
    s = min(w / 1280, h / 720)
    pw, ph = round(1280 * s), round(720 * s)
    x, y = (w - pw) // 2, (h - ph) // 2
    return im.crop((x, y, x + pw, y + ph))


def black_edges(im):
    w, h = im.size
    g = im.convert("L")
    px = g.load()

    def run(lines):
        n = 0
        for line in lines:
            if max(line) > 12:
                break
            n += 1
        return n

    step = max(1, w // 400)
    rows = lambda ys: ([px[x, y] for x in range(0, w, step)] for y in ys)
    cols = lambda xs: ([px[x, y] for y in range(0, h, step)] for x in xs)
    return run(rows(range(h))), run(rows(range(h - 1, -1, -1))), run(cols(range(w))), run(cols(range(w - 1, -1, -1)))


def flat_rows(im, ys):
    """How many of these rows are one colour from side to side, and their mean colours."""
    w, _ = im.size
    flat, means = 0, []
    for y in ys:
        row = im.crop((0, y, w, y + 1))
        st = ImageStat.Stat(row)
        means.append(st.mean)
        if max(st.stddev) < 2.5:
            flat += 1
    return flat, means


game = [k for k in imgs if k[0] in "2346"]
ref = picture(imgs[game[0]]) if game else None
print(f"{tag}: {len(imgs)} shots, window client {next(iter(imgs.values())).size}, picture area {ref.size if ref else '-'}")
if "2_game_a" in imgs and "3_game_b" in imgs:
    a, b = picture(imgs["2_game_a"]), picture(imgs["3_game_b"])
    diff = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 8 else 0)
    print(f"  live: {ImageStat.Stat(diff).mean[0] / 255 * 100:.1f} % of the picture's pixels differ between the two in-game shots a second apart")
for k in game:
    im = picture(imgs[k])
    t, b, l, r = black_edges(im)
    h = im.size[1]
    ft, mt = flat_rows(im, range(0, 64))
    fb, mb = flat_rows(im, range(h - 64, h))
    line = f"  {k:<16} black rows top/bottom {t}/{b}, black columns left/right {l}/{r}; flat rows in the top 64: {ft}, in the bottom 64: {fb}"
    if ref is not None and im.size == ref.size and k != game[0]:
        _, rt = flat_rows(ref, range(0, 64))
        _, rb = flat_rows(ref, range(h - 64, h))
        dist = lambda m, n: sum(abs(x - y) for p, q in zip(m, n) for x, y in zip(p, q)) / (len(m) * 3)
        whole = ImageStat.Stat(ImageChops.difference(im, ref).convert("L")).mean[0]
        line += f"; those rows differ from the first in-game shot by {dist(mt, rt):.1f} / {dist(mb, rb):.1f} of 255 (whole picture: {whole:.1f})"
    print(line)

# Contact sheet.
thumbs = []
for k, im in imgs.items():
    t = im.copy()
    t.thumbnail((640, 360))
    ImageDraw.Draw(t).text((6, 4), k, fill=(255, 255, 0))
    thumbs.append(t)
cols = 4
tw, th = 640, max(t.size[1] for t in thumbs)
sheet = Image.new("RGB", (cols * tw, ((len(thumbs) + cols - 1) // cols) * th), (40, 40, 40))
for i, t in enumerate(thumbs):
    sheet.paste(t, ((i % cols) * tw, (i // cols) * th))
out = os.path.join(root, tag + "_sheet.jpg")
sheet.save(out, quality=85)
print("  sheet:", out)
