"""Compare the colours of NSW-Home's Switch 2 style with the owner's captures.

Each patch names a rectangle of a capture frame and the same rectangle of a
--shot render, and how to read a colour from it. The table printed at the end
is the audit: capture colour, our colour, difference per channel.

    python tools/palette_check.py                 the whole table
    python tools/palette_check.py dark|light      one theme
    python tools/palette_check.py <text>          patches whose name contains <text>
    python tools/palette_check.py brief [...]     the same in fewer columns
    python tools/palette_check.py probe cap  <capture> <seconds> x y w h [mode]
    python tools/palette_check.py probe our  <screen>            x y w h [mode]
    python tools/palette_check.py frame <capture> <seconds>     extract one frame, print its path
    python tools/palette_check.py shot <screen>                 render one screen, print its path

Reading a colour ("mode"):
    flat    median of the rectangle, per channel (flat areas).
    hline   a thin horizontal rule crossing the rectangle: the row that differs
            most from the rectangle's first and last rows, median along it.
    vline   the same for a vertical rule (columns).
    ink     text or a thin glyph: the pixels furthest from the rectangle's
            most common colour (its ground); median of the furthest 2 %.
            Only meaningful where a stroke is thick enough to have a core.
    core    text with thick strokes: the pixels more than 70 % of the way
            from the ground to the furthest colour, median of those (the flat
            inside of the strokes rather than their single most extreme pixel).
    dark / bright
            median of the darkest / brightest 5 % of the rectangle.

Captures are H.264 4:2:2, frames are extracted as PNG with ffmpeg's default
conversion and an exact two-step seek. In 4:2:2 the colour of anything thinner
than a few pixels is smeared sideways: on the capture itself thin blue wording
reads 2 to 4 levels less blue than a flat patch of the same blue on the same
page (accent bar against section name, card mark against title), and thin
grey wording loses its slight tint. The flat patches say what a colour is;
the text patches are there to catch a wrong colour, not to settle the last
levels of a right one. Rectangles are in 1080p pixels (layout
units x 1.5). Renders go through build/dev.exe --shot with the project copy's
data folder (set NSWHOME_EXE to check another build, e.g. the one from before
a change); "<screen>|<frames>|<script>" renders with --shotseq and takes the
last frame (for states that need button presses). A screen name starting with
"nopack:" is rendered with a copy of the data folder that has no icon pack, so
that the drawn fallbacks (controller mark, status row, bar pictures) can be
checked as well.
"""
import os
import shutil
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The captures are kept out of the repository, in ref/sw2 beside tools/ (see .gitignore).
REF = os.environ.get("NSWHOME_REF") or os.path.join(ROOT, "ref", "sw2")
CACHE = os.path.join(ROOT, "shots", "pal")
EXE = os.environ.get("NSWHOME_EXE") or os.path.join(ROOT, "build", "dev.exe")
TOL = 2


def frame(capture, t):
    """Path of the PNG of `capture` at `t` seconds (extracted once)."""
    os.makedirs(CACHE, exist_ok=True)
    out = os.path.join(CACHE, "cap_%s_%07.3f.png" % (os.path.splitext(capture)[0], t))
    if not os.path.exists(out):
        pre = max(0.0, t - 5.0)
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", "%.3f" % pre, "-i",
                        os.path.join(REF, capture), "-ss", "%.3f" % (t - pre), "-frames:v", "1", out], check=True)
    return out


_rendered = {}


def shot(screen):
    """Path of our render of `screen` (rendered once per run)."""
    if screen in _rendered:
        return _rendered[screen]
    os.makedirs(CACHE, exist_ok=True)
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in screen)
    out = os.path.join(CACHE, "our_%s.png" % safe)
    # The data folder to render with: NSWHOME_DATA when set, else the project copy's own.
    data = os.environ.get("NSWHOME_DATA") or os.environ.get("DOCKSIDE_DATA") or os.path.join(ROOT, "data")  # the second: historical name
    if screen.startswith("nopack:"):
        screen_arg = screen[len("nopack:"):]
        bare = os.path.join(CACHE, "data_nopack")
        if not os.path.isdir(bare):
            shutil.copytree(data, bare, ignore=shutil.ignore_patterns("pack"))
        data = bare
    else:
        screen_arg = screen
    env = dict(os.environ, NSWHOME_DATA=data)
    if "|" in screen_arg:
        name, frames, script = screen_arg.split("|")
        seq = os.path.join(CACHE, "seq_" + safe)
        os.makedirs(seq, exist_ok=True)
        subprocess.run([EXE, "--shotseq", name, seq, frames, script], check=True, env=env)
        last = os.path.join(seq, "f_%03d.png" % (int(frames) - 1))
        Image.open(last).save(out)
        for f in os.listdir(seq):
            os.remove(os.path.join(seq, f))
        os.rmdir(seq)
    else:
        subprocess.run([EXE, "--shot", screen_arg, out], check=True, env=env)
    _rendered[screen] = out
    return out


def _median(values):
    s = sorted(values)
    return s[len(s) // 2]


def _med3(pixels):
    return tuple(_median([p[c] for p in pixels]) for c in range(3))


def _luma(p):
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]


def read(path, rect, mode="flat"):
    x, y, w, h = rect
    im = Image.open(path).convert("RGB").crop((x, y, x + w, y + h))
    px = list(im.getdata())
    if mode == "flat":
        return _med3(px)
    if mode in ("hline", "vline"):
        if mode == "vline":
            im = im.transpose(Image.TRANSPOSE)
            px = list(im.getdata())
            w, h = h, w
        rows = [_med3(px[r * w:(r + 1) * w]) for r in range(h)]
        ground = tuple((rows[0][c] + rows[-1][c]) / 2 for c in range(3))
        return max(rows, key=lambda r: sum(abs(r[c] - ground[c]) for c in range(3)))
    if mode == "ink":
        counts = {}
        for p in px:
            counts[p] = counts.get(p, 0) + 1
        ground = max(counts, key=counts.get)
        far = sorted(px, key=lambda p: -sum(abs(p[c] - ground[c]) for c in range(3)))
        return _med3(far[:max(3, len(far) // 50)])
    if mode == "core":
        counts = {}
        for p in px:
            counts[p] = counts.get(p, 0) + 1
        ground = max(counts, key=counts.get)
        dist = [sum(abs(p[c] - ground[c]) for c in range(3)) for p in px]
        top = max(dist)
        return _med3([p for p, d in zip(px, dist) if d > 0.7 * top] or px)
    if mode in ("dark", "bright"):
        s = sorted(px, key=_luma, reverse=(mode == "bright"))
        return _med3(s[:max(3, len(s) // 20)])
    raise ValueError(mode)


def hexs(c):
    return "#%02X%02X%02X" % c


W, W2, W5, AN = "walkthrough.mp4", "walkthrough2.mkv", "w5.mkv", "anim.mkv"

# name, theme, capture, seconds, our screen, rectangle (x, y, w, h), mode[, our rectangle]
PATCHES = [
    # ---- dark: HOME -------------------------------------------------------
    ("home page", "dark", W, 96.0, "home-2", (900, 150, 200, 40), "flat"),
    ("home page (anim.mkv)", "dark", AN, 254.5, "home-2", (900, 150, 200, 40), "flat"),
    ("home bar panel", "dark", W, 96.0, "home-2", (700, 778, 400, 10), "flat"),
    ("home picture ring", "dark", W, 96.0, "home-2", (116, 68, 8, 5), "flat"),
    ("home title mark", "dark", W, 96.0, "home-2", (845, 242, 18, 14), "flat", (43, 242, 18, 14)),
    ("home title", "dark", W, 96.0, "home-2", (880, 235, 600, 40), "core", (80, 237, 640, 40)),
    ("home tile band", "dark", W, 96.0, "home-2", (966, 480, 3, 40), "flat", (154, 480, 3, 40)),
    ("home hint disc", "dark", W, 96.0, "home-2", (1619, 1012, 5, 16), "flat"),
    ("home hint wording", "dark", W, 96.0, "home-2", (1665, 1000, 160, 40), "core", (1663, 1000, 160, 40)),
    ("home clock numerals (pack)", "dark", W, 96.0, "home-2", (1467, 95, 115, 40), "core"),
    ("home wifi (pack)", "dark", W, 96.0, "home-2", (1606, 92, 48, 44), "ink"),
    ("home battery (pack)", "dark", W, 96.0, "home-2", (1800, 108, 30, 12), "flat"),
    ("home pad mark body (pack)", "dark", W, 96.0, "home-2", (104, 1034, 18, 3), "flat"),
    ("home pad mark lit (pack)", "dark", W, 96.0, "home-2", (78, 993, 6, 6), "flat"),
    ("home pad mark unlit (pack)", "dark", W, 96.0, "home-2", (78, 1009, 6, 6), "flat"),
    ("home bar grey picture (pack)", "dark", AN, 254.5, "home-2", (1368, 812, 56, 56), "core"),
    ("bar focus fill", "dark", AN, 259.0, "sys-2", (1507, 790, 20, 8), "flat", (1385, 790, 20, 8)),
    ("bar focus fill (walkthrough)", "dark", W, 93.0, "sys-2", (1141, 790, 20, 8), "flat", (1385, 790, 20, 8)),
    ("bar label", "dark", W, 93.0, "sys-2", (1050, 925, 250, 44), "core", (1100, 925, 600, 44)),
    ("all-software button", "dark", W, 100.3, "last-2", (1520, 380, 100, 40), "flat"),
    ("all-software mark", "dark", W, 100.3, "last-2", (1540, 440, 170, 100), "core"),
    ("all-software name", "dark", W, 100.3, "last-2", (1530, 300, 150, 50), "core"),
    # drawn fallbacks, without the icon pack
    ("no pack: clock", "dark", W, 96.0, "nopack:home-2", (1467, 95, 115, 40), "core"),
    ("no pack: wifi", "dark", W, 96.0, "nopack:home-2", (1606, 92, 48, 44), "ink"),
    ("no pack: battery", "dark", W, 96.0, "nopack:home-2", (1800, 108, 30, 12), "flat", (1805, 108, 25, 10)),
    ("no pack: pad mark body", "dark", W, 96.0, "nopack:home-2", (104, 1034, 18, 3), "flat"),
    ("no pack: pad mark lit", "dark", W, 96.0, "nopack:home-2", (78, 993, 6, 6), "flat"),
    ("no pack: pad mark unlit", "dark", W, 96.0, "nopack:home-2", (78, 1009, 6, 6), "flat"),
    ("no pack: bar grey picture", "dark", AN, 254.5, "nopack:home-2", (1368, 812, 56, 56), "core"),
    ("no pack: online disc", "dark", AN, 254.5, "nopack:home-2", (376, 812, 10, 8), "flat"),
    # ---- dark: Settings ----------------------------------------------------
    ("settings header wording", "dark", W, 135.0, "settings-cat-2", (190, 50, 540, 50), "core"),
    ("settings header rule", "dark", W, 135.0, "settings-cat-2", (300, 120, 400, 14), "hline"),
    ("settings footer rule", "dark", W, 135.0, "settings-cat-2", (300, 961, 400, 14), "hline"),
    ("settings column divider", "dark", W, 135.0, "settings-cat-2", (632, 400, 14, 300), "vline"),
    ("settings focus fill (section)", "dark", W, 135.0, "settings-cat-2", (420, 346, 150, 14), "flat", (420, 172, 150, 12)),
    ("settings accent bar", "dark", W, 135.0, "settings-rows-2", (163, 370, 3, 50), "flat", (164, 440, 2, 50)),
    ("settings section, chosen", "dark", W, 135.0, "settings-rows-2", (200, 370, 320, 50), "core", (200, 440, 150, 50)),
    ("settings section", "dark", W, 135.0, "settings-rows-2", (200, 250, 290, 50), "core", (200, 320, 180, 50)),
    ("settings row rule", "dark", W, 135.0, "settings-rows-2", (800, 299, 400, 12), "hline", (800, 494, 400, 14)),
    ("settings row wording", "dark", W, 135.0, "settings-rows-2", (730, 225, 290, 50), "core", (730, 420, 250, 50)),
    ("settings small print / plain value", "dark", W, 135.0, "settings-rows-2", (730, 315, 900, 40), "ink", (1520, 730, 190, 50)),
    ("settings value", "dark", W, 159.6, "settings-rows-2", (1495, 580, 215, 45), "core", (1610, 310, 100, 50)),
    ("settings heading bar", "dark", W, 159.6, "settings-rows-2", (698, 208, 16, 24), "vline", (698, 230, 16, 26)),
    ("settings heading wording", "dark", W, 159.6, "settings-rows-2", (724, 200, 130, 40), "core", (724, 225, 250, 40)),
    ("settings focus fill (row)", "dark", W, 150.0, "settings-rows-2", (900, 690, 400, 20), "flat", (1100, 285, 300, 14)),
    ("settings scroll bar", "dark", W, 150.0, "settings-last-2", (1882, 500, 3, 300), "flat", (1883, 500, 3, 300)),
    # ---- dark: software grid ----------------------------------------------
    ("grid tab wording", "dark", W, 102.2, "all-open-2", (665, 45, 160, 40), "core"),
    ("grid tab underline", "dark", W, 102.2, "all-open-2", (600, 88, 280, 2), "flat"),
    ("grid rule", "dark", W, 102.2, "all-open-2", (300, 104, 400, 14), "hline"),
    ("grid sort wording", "dark", W, 102.2, "all-open-2", (1455, 138, 305, 36), "ink", (1430, 138, 330, 36)),
    ("grid balloon ground", "dark", W, 102.2, "all-open-2", (225, 348, 20, 56), "flat", (225, 478, 20, 56)),
    ("grid balloon edge", "dark", W, 102.2, "all-open-2", (203, 370, 14, 30), "vline", (203, 495, 14, 30)),
    ("grid balloon mark", "dark", W, 102.2, "all-open-2", (267, 366, 18, 16), "flat", (267, 494, 18, 16)),
    ("grid balloon name", "dark", W, 102.2, "all-open-2", (305, 358, 335, 40), "core", (305, 485, 600, 40)),
    # ---- dark: album ------------------------------------------------------
    ("album header wording", "dark", W, 216.0, "album-demo-2", (185, 50, 130, 50), "core"),
    ("album header rule", "dark", W, 216.0, "album-demo-2", (300, 120, 400, 14), "hline"),
    ("album sort wording", "dark", W, 216.0, "album-demo-2", (1255, 55, 175, 40), "ink", (1265, 55, 175, 40)),
    ("album sort divider", "dark", W, 216.0, "album-demo-2", (1438, 58, 16, 34), "vline", (1450, 58, 16, 34)),
    ("album scroll mark", "dark", W, 216.0, "album-demo-2", (1886, 195, 3, 36), "flat", (1885, 300, 2, 100)),
    ("album empty cell", "dark", AN, 271.15, "album-demo-2", (300, 500, 200, 60), "flat", (300, 760, 200, 60)),
    ("grid empty cell", "dark", W, 101.5, "album-demo-2", (300, 760, 150, 100), "flat", (300, 760, 200, 60)),
    # ---- dark: controllers ------------------------------------------------
    ("pads section title", "dark", AN, 265.5, "pads-on-2", (590, 605, 106, 36), "ink"),
    ("pads rule", "dark", AN, 265.5, "pads-on-2", (400, 543, 400, 14), "hline"),
    ("pads column rule", "dark", AN, 265.5, "pads-on-2", (1272, 400, 14, 300), "vline"),
    ("pads light, lit", "dark", AN, 265.5, "pads-on-2", (255, 698, 9, 9), "flat"),
    ("pads light, unlit", "dark", AN, 265.5, "pads-on-2", (281, 698, 10, 10), "flat"),
    ("pads controller body", "dark", AN, 265.5, "pads-on-2", (425, 370, 30, 30), "flat", (280, 760, 30, 10)),
    # ---- dark: options panel ---------------------------------------------
    ("options panel", "dark", W, 241.0, "options-cat-2", (1000, 10, 400, 20), "flat"),
    ("options menu behind, right", "dark", W, 241.0, "options-cat-2", (1795, 200, 100, 80), "flat"),
    ("options menu behind, left", "dark", W, 241.0, "options-cat-2", (20, 500, 100, 100), "flat"),
    ("options header rule", "dark", W, 241.0, "options-cat-2", (400, 153, 400, 14), "hline"),
    ("options footer rule", "dark", W, 241.0, "options-cat-2", (400, 961, 400, 14), "hline"),
    ("options column divider", "dark", W, 241.0, "options-cat-2", (720, 400, 14, 300), "vline"),
    ("options row rule", "dark", W, 241.0, "options-cat-2", (1000, 694, 400, 14), "hline", (1000, 330, 400, 14)),
    ("options focus fill", "dark", W, 241.0, "options-cat-2", (1200, 490, 300, 80), "flat", (400, 205, 200, 14)),
    ("options title", "dark", W, 241.0, "options-cat-2", (345, 50, 630, 44), "core"),
    ("options small print", "dark", W, 241.0, "options-cat-2", (345, 98, 310, 30), "ink"),
    ("options accent bar", "dark", W, 241.0, "options-cat-2", (243, 215, 4, 70), "flat", (244, 215, 2, 70)),
    ("options section, chosen", "dark", W, 241.0, "options-cat-2", (280, 210, 180, 40), "core", (280, 235, 150, 40)),
    ("options section", "dark", W, 241.0, "options-cat-2", (280, 355, 150, 40), "core", (280, 355, 180, 40)),
    ("options row wording", "dark", W, 241.0, "options-cat-2", (820, 625, 360, 40), "core", (820, 375, 100, 40)),
    ("options hint wording", "dark", W, 241.0, "options-cat-2", (1354, 1000, 80, 40), "core"),
    ("options battery behind", "dark", W, 241.0, "options-cat-2", (1800, 108, 30, 12), "flat"),
    # ---- dark: dialog ------------------------------------------------------
    ("dialog box", "dark", AN, 257.7, "dlg-sleep-2", (900, 345, 400, 40), "flat"),
    ("dialog focus fill", "dark", AN, 257.7, "dlg-sleep-2", (1100, 650, 300, 20), "flat"),
    ("dialog rule", "dark", AN, 257.7, "dlg-sleep-2", (500, 632, 300, 14), "hline"),
    ("dialog wording", "dark", AN, 257.7, "dlg-sleep-2", (527, 410, 490, 44), "core"),
    ("dialog small print", "dark", AN, 257.7, "dlg-sleep-2", (527, 505, 860, 40), "ink"),
    ("dialog button", "dark", AN, 257.7, "dlg-sleep-2", (625, 675, 135, 40), "core"),
    ("dialog button, focused", "dark", AN, 257.7, "dlg-sleep-2", (1105, 675, 245, 40), "core"),
    ("dialog menu behind", "dark", AN, 257.7, "dlg-sleep-2", (900, 150, 300, 100), "flat"),
    ("dialog bar behind", "dark", AN, 257.7, "dlg-sleep-2", (700, 778, 400, 10), "flat"),
    ("dialog battery behind", "dark", AN, 257.7, "dlg-sleep-2", (1800, 108, 30, 12), "flat"),
    # ---- dark: who-is-playing sheet --------------------------------------
    ("sheet ground", "dark", AN, 278.3, "who-2", (900, 480, 400, 30), "flat"),
    ("sheet footer ground", "dark", AN, 278.3, "who-2", (900, 1000, 300, 60), "flat"),
    ("sheet menu behind", "dark", AN, 278.3, "who-2", (900, 150, 300, 100), "flat"),
    ("sheet battery behind", "dark", AN, 278.3, "who-2", (1800, 108, 30, 12), "flat"),
    ("sheet footer rule", "dark", AN, 278.3, "who-2", (400, 961, 400, 14), "hline"),
    ("sheet question", "dark", AN, 278.3, "who-2", (175, 505, 525, 44), "core", (120, 505, 510, 44)),
    ("sheet picture ring", "dark", AN, 278.3, "who-2", (476, 630, 8, 3), "flat"),
    ("sheet add-user ring", "dark", AN, 278.3, "who-2", (1685, 716, 12, 14), "vline", (626, 716, 12, 14)),
    ("sheet add-user cross", "dark", AN, 278.3, "who-2", (1722, 721, 20, 5), "flat", (660, 722, 20, 3)),
    ("sheet name", "dark", AN, 278.3, "who-2", (1500, 850, 80, 40), "core", (415, 850, 130, 40)),
    # ---- light: HOME -------------------------------------------------------
    ("home page", "light", W5, 2.67, "run-2l", (900, 150, 200, 40), "flat"),
    ("home page (walkthrough2)", "light", W2, 5.0, "run-2l", (900, 150, 200, 40), "flat"),
    ("home page (walkthrough)", "light", W, 345.0, "run-2l", (900, 150, 200, 40), "flat"),
    ("home bar panel", "light", W5, 2.67, "run-2l", (700, 778, 400, 10), "flat"),
    ("home picture ring", "light", W5, 2.67, "run-2l", (116, 68, 8, 5), "flat"),
    ("home title mark", "light", W5, 2.67, "run-2l", (673, 242, 18, 14), "flat", (43, 242, 18, 14)),
    ("home title", "light", W5, 2.67, "run-2l", (710, 235, 145, 40), "core", (80, 237, 640, 40)),
    ("home tile band", "light", W5, 2.67, "run-2l", (560, 480, 3, 40), "flat", (154, 480, 3, 40)),
    ("home hint disc", "light", W5, 2.67, "run-2l", (1619, 1012, 5, 16), "flat"),
    ("home hint wording", "light", W5, 2.67, "run-2l", (1665, 1000, 160, 40), "core", (1663, 1000, 160, 40)),
    ("home in-use mark edge", "light", W5, 32.9, "run-2l", (380, 584, 40, 2), "flat", (380, 583, 40, 2)),
    ("home in-use wording", "light", W5, 32.9, "run-2l", (340, 605, 110, 36), "ink"),
    ("home change-user glyph", "light", W5, 32.9, "run-2l", (181, 720, 4, 14), "flat"),
    ("home change-user wording", "light", W5, 32.9, "run-2l", (225, 708, 310, 40), "core"),
    ("home clock numerals (pack)", "light", W5, 2.67, "run-2l", (1467, 95, 115, 40), "core"),
    ("home wifi (pack)", "light", W5, 2.67, "run-2l", (1606, 92, 48, 44), "ink"),
    ("home battery (pack)", "light", W5, 2.67, "run-2l", (1800, 108, 30, 12), "flat"),
    ("home pad mark body (pack)", "light", W5, 2.67, "run-2l", (104, 1034, 18, 3), "flat"),
    ("home pad mark lit (pack)", "light", W5, 2.67, "run-2l", (78, 993, 6, 6), "flat"),
    ("home pad mark unlit (pack)", "light", W5, 2.67, "run-2l", (78, 1009, 6, 6), "flat"),
    ("home bar grey picture (pack)", "light", W5, 2.67, "run-2l", (1490, 812, 56, 56), "core"),
    ("bar focus fill", "light", W5, 1.4, "sys-2l", (1385, 790, 20, 8), "flat"),
    ("bar label", "light", W5, 1.4, "sys-2l", (1250, 925, 400, 44), "core", (1100, 925, 600, 44)),
    ("no pack: clock", "light", W5, 2.67, "nopack:home-2l", (1467, 95, 115, 40), "core"),
    ("no pack: wifi", "light", W5, 2.67, "nopack:home-2l", (1606, 92, 48, 44), "ink"),
    ("no pack: battery", "light", W5, 2.67, "nopack:home-2l", (1800, 108, 30, 12), "flat", (1805, 108, 25, 10)),
    ("no pack: pad mark body", "light", W5, 2.67, "nopack:home-2l", (104, 1034, 18, 3), "flat"),
    ("no pack: pad mark lit", "light", W5, 2.67, "nopack:home-2l", (78, 993, 6, 6), "flat"),
    ("no pack: pad mark unlit", "light", W5, 2.67, "nopack:home-2l", (78, 1009, 6, 6), "flat"),
    ("no pack: bar grey picture", "light", W5, 2.67, "nopack:home-2l", (1490, 812, 56, 56), "core"),
    ("no pack: online disc", "light", W5, 2.67, "nopack:home-2l", (376, 812, 10, 8), "flat"),
    # ---- light: dialog ----------------------------------------------------
    ("dialog box", "light", W2, 18.2, "dlg-upd-2l", (900, 340, 400, 40), "flat"),
    ("dialog focus fill", "light", W2, 18.2, "dlg-upd-2l", (1100, 655, 300, 14), "flat"),
    ("dialog rule", "light", W2, 18.2, "dlg-upd-2l", (500, 524, 300, 14), "hline"),
    ("dialog rule, lower", "light", W2, 18.2, "dlg-upd-2l", (500, 637, 300, 14), "hline"),
    ("dialog wording", "light", W2, 18.2, "dlg-upd-2l", (655, 395, 610, 44), "core"),
    ("dialog button", "light", W2, 18.2, "dlg-upd-2l", (625, 680, 135, 40), "core"),
    ("dialog button, focused", "light", W2, 18.2, "dlg-upd-2l", (1150, 680, 160, 40), "core"),
    ("dialog menu behind", "light", W2, 18.2, "dlg-upd-2l", (900, 150, 300, 100), "flat"),
    ("dialog bar behind (clear of the shadow)", "light", W2, 18.2, "dlg-upd-2l", (700, 900, 400, 6), "flat"),
    ("dialog picture ring behind", "light", W2, 18.2, "dlg-upd-2l", (116, 68, 8, 5), "flat"),
    ("dialog battery behind", "light", W2, 18.2, "dlg-upd-2l", (1800, 108, 30, 12), "flat"),
    ("picture dialog box", "light", W5, 33.9, "dlg-close-2l", (900, 345, 300, 20), "flat"),
    # ---- light: options panel ---------------------------------------------
    ("options panel", "light", W, 296.0, "options-cat-2l", (1000, 10, 400, 20), "flat"),
    ("options menu behind, right", "light", W, 296.0, "options-cat-2l", (1795, 200, 100, 80), "flat"),
    ("options menu behind, left", "light", W, 296.0, "options-cat-2l", (20, 500, 100, 100), "flat"),
    ("options header rule", "light", W, 296.0, "options-cat-2l", (400, 153, 400, 14), "hline"),
    ("options footer rule", "light", W, 296.0, "options-cat-2l", (400, 961, 400, 14), "hline"),
    ("options column divider", "light", W, 296.0, "options-cat-2l", (720, 500, 14, 300), "vline"),
    ("options row rule", "light", W, 296.0, "options-cat-2l", (1000, 330, 400, 14), "hline"),
    ("options focus fill", "light", W, 296.0, "options-cat-2l", (500, 205, 150, 10), "flat"),
    ("options title", "light", W, 296.0, "options-cat-2l", (345, 50, 630, 44), "core"),
    ("options small print", "light", W, 296.0, "options-cat-2l", (345, 98, 310, 30), "core"),
    ("options accent bar", "light", W, 296.0, "options-cat-2l", (243, 215, 4, 70), "flat", (244, 215, 2, 70)),
    ("options section, chosen", "light", W, 296.0, "options-cat-2l", (280, 210, 180, 40), "core", (280, 235, 150, 40)),
    ("options section", "light", W, 296.0, "options-cat-2l", (280, 355, 150, 40), "core", (280, 355, 180, 40)),
    ("options row wording", "light", W, 296.0, "options-cat-2l", (820, 260, 300, 40), "core", (820, 260, 380, 40)),
    ("options hint disc", "light", W, 296.0, "options-cat-2l", (1304, 1012, 5, 16), "flat"),
    ("options battery behind", "light", W, 296.0, "options-cat-2l", (1800, 108, 30, 12), "flat"),
    # ---- light: user page -------------------------------------------------
    ("user header wording", "light", W, 306.0, "user-rows-2l", (225, 50, 290, 50), "core", (225, 50, 390, 50)),
    ("user header rule", "light", W, 306.0, "user-rows-2l", (300, 120, 400, 14), "hline"),
    ("user footer rule", "light", W, 306.0, "user-rows-2l", (300, 961, 400, 14), "hline"),
    ("user column divider", "light", W, 306.0, "user-rows-2l", (632, 400, 14, 300), "vline"),
    ("user section rule", "light", W, 306.0, "user-rows-2l", (250, 276, 200, 12), "hline", (250, 277, 200, 12)),
    ("user row rule", "light", W, 306.0, "user-rows-2l", (900, 580, 400, 12), "hline"),
    ("user row rule, lower", "light", W, 306.0, "user-rows-2l", (900, 719, 400, 12), "hline", (900, 720, 400, 12)),
    ("user accent bar", "light", W, 306.0, "user-rows-2l", (163, 190, 3, 60), "flat", (164, 190, 2, 60)),
    ("user section, chosen", "light", W, 306.0, "user-rows-2l", (200, 200, 75, 40), "core"),
    ("user section", "light", W, 306.0, "user-rows-2l", (200, 325, 180, 40), "core", (200, 325, 280, 40)),
    ("user name", "light", W, 306.0, "user-rows-2l", (917, 250, 110, 50), "core", (917, 250, 215, 50)),
    ("user small print", "light", W, 306.0, "user-rows-2l", (917, 318, 420, 40), "core"),
    ("user heading wording", "light", W, 306.0, "user-rows-2l", (726, 530, 410, 40), "core"),
    ("user history name", "light", W, 306.0, "user-rows-2l", (872, 615, 340, 40), "core"),
    ("user history time, first", "light", W, 306.0, "user-rows-2l", (872, 667, 330, 34), "core"),
    ("user history time", "light", W, 306.0, "user-rows-2l", (872, 805, 330, 36), "core"),
    ("user scroll bar", "light", W, 306.0, "user-rows-2l", (1882, 160, 3, 150), "flat", (1885, 220, 3, 150)),
    ("user focus fill", "light", W, 316.0, "user-rows-2l", (420, 664, 150, 12), "flat", (1000, 205, 300, 12)),
    ("user value", "light", W, 316.0, "user-rows-2l", (1630, 405, 80, 40), "core", (872, 667, 330, 34)),
    ("user hint disc", "light", W, 306.0, "user-rows-2l", (1441, 1012, 5, 16), "flat"),
    # ---- light: who-is-playing sheet -------------------------------------
    ("sheet ground", "light", W2, 21.4, "who-2l", (900, 480, 400, 30), "flat"),
    ("sheet footer ground", "light", W2, 21.4, "who-2l", (900, 1000, 300, 60), "flat"),
    ("sheet menu behind", "light", W2, 21.4, "who-2l", (900, 150, 300, 100), "flat"),
    ("sheet battery behind", "light", W2, 21.4, "who-2l", (1800, 108, 30, 12), "flat"),
    ("sheet footer rule", "light", W2, 21.4, "who-2l", (400, 961, 400, 14), "hline"),
    ("sheet question", "light", W2, 21.4, "who-2l", (120, 505, 510, 44), "core"),
    ("sheet picture ring", "light", W2, 21.4, "who-2l", (476, 630, 8, 3), "flat"),
    ("sheet add-user ring", "light", W2, 21.4, "who-2l", (1685, 716, 12, 14), "vline", (626, 716, 12, 14)),
    ("sheet add-user cross", "light", W2, 21.4, "who-2l", (1722, 721, 20, 5), "flat", (660, 722, 20, 3)),
    ("sheet name", "light", W2, 21.4, "who-2l", (1500, 850, 80, 40), "core", (415, 850, 130, 40)),
]


def main():
    a = sys.argv[1:]
    if a and a[0] == "frame":
        print(frame(a[1], float(a[2])))
        return
    if a and a[0] == "shot":
        print(shot(a[1]))
        return
    if a and a[0] == "probe":
        if a[1] == "cap":
            path, rest = frame(a[2], float(a[3])), a[4:]
        else:
            path, rest = shot(a[2]), a[3:]
        rect = tuple(int(v) for v in rest[:4])
        mode = rest[4] if len(rest) > 4 else "flat"
        c = read(path, rect, mode)
        print("%s  %s  %s" % (c, hexs(c), mode))
        return
    brief = bool(a) and a[0] == "brief"
    if brief:
        a = a[1:]
    want = a[0] if a else ""
    rows, bad = [], 0
    for p in PATCHES:
        name, theme, cap, t, screen, rect, mode = p[:7]
        ours = p[7] if len(p) > 7 else rect
        if want in ("dark", "light"):
            if theme != want:
                continue
        elif want and want not in name:
            continue
        c = read(frame(cap, t), rect, mode)
        o = read(shot(screen), ours, mode)
        d = tuple(o[i] - c[i] for i in range(3))
        ok = max(abs(v) for v in d) <= TOL
        bad += not ok
        rows.append((name, theme, "%s %.2f" % (cap, t), "%d,%d %dx%d" % rect, mode, "%3d %3d %3d" % c, hexs(c),
                     "%3d %3d %3d" % o, hexs(o), "%+d %+d %+d" % d, "ok" if ok else "OFF", screen))
    head = ("patch", "theme", "capture, s", "rectangle", "mode", "console", "", "NSW-Home", "", "difference", "", "screen")
    if brief:
        keep = (0, 1, 2, 3, 4, 5, 7, 9, 10)
        rows = [tuple(r[i] for i in keep) for r in rows]
        head = tuple(head[i] for i in keep)
    widths = [max(len(str(r[i])) for r in rows + [head]) for i in range(len(head))]
    for r in [head] + rows:
        print("  ".join(str(v).ljust(widths[i]) for i, v in enumerate(r)).rstrip())
    print("\n%d patches, %d outside +-%d" % (len(rows), bad, TOL))


if __name__ == "__main__":
    main()
