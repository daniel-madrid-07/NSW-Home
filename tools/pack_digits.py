"""Cut the status-row numerals out of YOUR OWN captures into the local icon pack.

The console sets the clock and the battery percentage in a numeral design of
its own that is not in the system font, so NSW-Home can only show the same
shapes from pictures. Each character becomes an opacity mask, saved once in the
dark theme's colour and once in the light theme's:

    %APPDATA%\\NSW-Home\\pack\\dig_<0-9|c|p>_<d|l>.png      (c = colon, p = percent)

The pack stays on this PC; it is not part of NSW-Home. Without it (or with any
character missing) NSW-Home draws the numbers with the system font.

Usage:  python pack_digits.py            (uses the sources listed in SOURCES)
Needs ffmpeg on PATH and Pillow.
"""
import glob
import os
import subprocess
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, "..", "ref", "sw2")

# Numerals sit in fixed cells. Measured at 1920x1080: the clock's four cells
# are centred on x = 1478, 1501.7, 1545.3 and 1569.6 with the colon at 1523;
# the percentage's on 1687, 1710.5 and 1734.5 with the sign at 1759.5.
CLOCK_X = [1478.0, 1501.7, 1545.3, 1569.6]
COLON_X, PERCENT_X = 1523.0, 1759.5
TOP, BOTTOM = 96, 132          # rows that hold the numerals
CELL = 24                      # width of a numeral cell
PERCENT_W = 28

# character -> (capture file or glob of stills, seconds into it or None, centre x, width)
SOURCES = {
    "0": ("walkthrough.mp4", 97.62, CLOCK_X[0], CELL),    # 04:41
    "4": ("walkthrough.mp4", 97.62, CLOCK_X[1], CELL),
    "1": ("walkthrough.mp4", 97.62, CLOCK_X[3], CELL),
    "c": ("walkthrough.mp4", 97.62, COLON_X, 14),
    "p": ("walkthrough.mp4", 97.62, PERCENT_X, PERCENT_W),
    "3": ("walkthrough.mp4", 230.3, CLOCK_X[3], CELL),    # 04:43
    "6": ("anim.mkv", 255.0, CLOCK_X[1], CELL),           # 06:00
    "5": ("walkthrough2.mkv", 5.0, CLOCK_X[1], CELL),     # 05:18
    "8": ("walkthrough2.mkv", 5.0, CLOCK_X[3], CELL),
    "2": ("w5.mkv", 1.0, CLOCK_X[2], CELL),               # 05:24
    "7": ("clock/clock_062745.png", None, CLOCK_X[3], CELL),  # stills in which the console clock read 06:27 and 06:29
    "9": ("clock/clock_062945.png", None, CLOCK_X[3], CELL),  # (its clock runs about a minute behind this PC)
}

DARK_THEME_INK = (212, 210, 214)   # sampled from the capture
LIGHT_THEME_INK = (22, 22, 22)     # sampled from the capture


def frame(source, seconds, tmp):
    path = os.path.join(REF, source)
    if seconds is None:
        hits = sorted(glob.glob(path))
        return Image.open(hits[len(hits) // 2]).convert("RGB") if hits else None
    out = os.path.join(tmp, "f.png")
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", str(seconds), "-i", path,
                    "-frames:v", "1", "-vf", "scale=1920:1080", out], check=True)
    return Image.open(out).convert("RGB")


def mask(crop):
    """Opacity of the numeral at each pixel: 0 on the background, 255 at full ink."""
    px = crop.load()
    w, h = crop.size
    # Background = the commonest corner colour; ink = whatever is furthest from it.
    corners = [px[0, 0], px[w - 1, 0], px[0, h - 1], px[w - 1, h - 1]]
    bg = sorted(corners)[1]
    dist = lambda p: max(abs(p[i] - bg[i]) for i in range(3))
    full = max(dist(px[x, y]) for y in range(h) for x in range(w))
    out = Image.new("L", crop.size)
    if full < 12:
        return None  # nothing there (blank or unusable frame)
    op = out.load()
    for y in range(h):
        for x in range(w):
            a = dist(px[x, y]) / full
            op[x, y] = 0 if a < 0.06 else min(255, round(a * 255))
    return out


def main():
    base = os.environ.get("NSWHOME_DATA") or os.environ.get("DOCKSIDE_DATA") or os.path.join(os.environ["APPDATA"], "NSW-Home")  # DOCKSIDE_DATA: historical name
    # Until the program has moved it at its first start, the data folder still has the former name.
    if not os.path.isdir(base) and os.path.isdir(os.path.join(os.environ.get("APPDATA", ""), "Dockside")):  # historical name
        base = os.path.join(os.environ.get("APPDATA", ""), "Dockside")  # historical name
    out_dir = os.path.join(base, "pack")
    os.makedirs(out_dir, exist_ok=True)
    made, missing = [], []
    with tempfile.TemporaryDirectory() as tmp:
        for ch, (source, seconds, cx, width) in SOURCES.items():
            img = frame(source, seconds, tmp)
            m = None
            if img is not None:
                left = round(cx - width / 2)
                m = mask(img.crop((left, TOP, left + width, BOTTOM)))
            if m is None:
                missing.append(ch)
                continue
            for tag, ink in (("d", DARK_THEME_INK), ("l", LIGHT_THEME_INK)):
                rgba = Image.new("RGBA", m.size, ink + (0,))
                rgba.putalpha(m)
                rgba.save(os.path.join(out_dir, f"dig_{ch}_{tag}.png"))
            made.append(ch)
    print("written to", out_dir, "-", "".join(made))
    if missing:
        print("no usable source yet for:", " ".join(missing))


if __name__ == "__main__":
    main()
