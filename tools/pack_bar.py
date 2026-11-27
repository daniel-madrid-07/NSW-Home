"""Cut the pressed-button animations of the HOME menu's system bar out of YOUR
OWN capture of YOUR OWN console into the local icon pack that NSW-Home loads
if present.

On the console every bar button has its own short animation when A is pressed
on it (the settings wheel turns, the game card opens, the coloured ones end up
filled in). Those shapes cannot be drawn from the still icons, so NSW-Home
plays them from the pack and, without a pack, just dips the still icon.

The pack is written to %NSWHOME_DATA%\\pack (or %APPDATA%\\NSW-Home\\pack when
that variable is not set) and stays on this PC: it is not part of NSW-Home and
must not be committed or redistributed.

Usage:  python pack_bar.py <recording> [--times T0,T1,...,T9]

The recording must be 60 frames a second, in the DARK theme, and show A being
pressed on each of the ten bar buttons with the focus already resting on it.
--times gives, for buttons 0..9 from left to right, a time in seconds within
half a second of each press (default: the times in the owner's anim.mkv); the
exact frame is found automatically. Use 0 to skip a button.

Writes bar_<button>_<frame 00..20>_d.png, and the same with _l for the light
theme (recoloured with the colour of that theme's still icon sys_<button>_l.png
from make_icon_pack.py, when that file is in the pack).
Needs ffmpeg on PATH and Pillow.
"""
import argparse
import math
import os
import subprocess
import sys
import tempfile

from PIL import Image

SCALE = 1.5  # capture is 1920x1080, NSW-Home lays out in 1280x720
SYS_X = [269.5] + [360.6 + i * 81.3 for i in range(9)]
SYS_Y, HALF = 559.7, 36.0  # same 72-unit square as the still icons
FRAMES = 21                # frames 0..20 after the press; the menu starts to fade on 21
DEFAULT_TIMES = [279.67, 277.38, 275.27, 272.78, 269.93, 267.32, 264.47, 262.10, 259.50, 256.37]
# Between the picture and the focus ring there is only the flat ground the
# picture sits on: its colour is read here on every frame (capture pixels).
GROUND_R0, GROUND_R1 = 50.5, 53.5
KEEP_R = 54.5  # everything further out (the ring) is dropped


def median(values):
    values = sorted(values)
    return values[len(values) // 2]


def ground(img, cx, cy):
    px = img.load()
    ring = []
    for y in range(img.height):
        for x in range(img.width):
            r = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            if GROUND_R0 <= r <= GROUND_R1:
                ring.append(px[x, y])
    return tuple(median([p[c] for p in ring]) for c in range(3))


def icon_colour(img, bg, cx, cy):
    """The picture's own colour: the usual colour of the pixels far from the
    ground. (Not the very furthest: next to a saturated stroke the capture's
    halved colour resolution leaves a column of bright grey pixels.)"""
    px = img.load()
    far = []
    for y in range(img.height):
        for x in range(img.width):
            if math.hypot(x + 0.5 - cx, y + 0.5 - cy) < 40:
                p = px[x, y]
                far.append((sum(abs(p[c] - bg[c]) for c in range(3)), p))
    limit = 0.6 * max(d for d, _ in far)
    top = [p for d, p in far if d >= limit]
    return tuple(median([p[c] for p in top]) for c in range(3))


def edge(x, y, cx, cy):
    """1 inside the kept circle, 0 outside, one pixel of feather."""
    return max(0.0, min(1.0, KEEP_R - math.hypot(x + 0.5 - cx, y + 0.5 - cy)))


def matte(img, bg, colour, cx, cy):
    """A one-colour picture over a flat ground: how much of `colour` each pixel holds."""
    d = [colour[c] - bg[c] for c in range(3)]
    dd = float(sum(v * v for v in d)) or 1.0
    out = Image.new("L", img.size)
    src, dst = img.load(), out.load()
    for y in range(img.height):
        for x in range(img.width):
            p = src[x, y]
            a = sum((p[c] - bg[c]) * d[c] for c in range(3)) / dd
            a = max(0.0, min(1.0, a)) * edge(x, y, cx, cy)
            dst[x, y] = 0 if a < 0.04 else round(a * 255)
    return out


def color_to_alpha(img, bg, cx, cy, floor=0.07):
    """A many-coloured picture over a flat ground: every pixel becomes the
    colour and opacity that, laid back over `bg`, reproduces the capture."""
    out = Image.new("RGBA", img.size)
    src, dst = img.load(), out.load()
    for y in range(img.height):
        for x in range(img.width):
            p = src[x, y]
            a = 0.0
            for c in range(3):
                if p[c] > bg[c]:
                    a = max(a, (p[c] - bg[c]) / (255.0 - bg[c]))
                elif p[c] < bg[c] and bg[c] > 0:
                    a = max(a, (bg[c] - p[c]) / float(bg[c]))
            if a < floor:
                dst[x, y] = (0, 0, 0, 0)
                continue
            rgb = tuple(max(0, min(255, round(bg[c] + (p[c] - bg[c]) / a))) for c in range(3))
            dst[x, y] = rgb + (round(a * edge(x, y, cx, cy) * 255),)
    return out


LIGHT_BAR = (230, 226, 230)  # the light-theme bar colour, measured on the capture


def still_colour(path):
    """Colour a still icon from make_icon_pack.py shows on the light theme's
    bar, so the light frames match the still icon they replace."""
    if not os.path.exists(path):
        return None
    icon = Image.open(path).convert("RGBA")
    shown = Image.new("RGBA", icon.size, LIGHT_BAR + (255,))
    shown.alpha_composite(icon)
    return icon_colour(shown.convert("RGB"), LIGHT_BAR, icon.width / 2, icon.height / 2)


def cut(video, button, about, out_dir, tmp):
    cx, cy = SYS_X[button] * SCALE, SYS_Y * SCALE
    h = HALF * SCALE
    left, top = round(cx - h), round(cy - h)
    cx, cy = cx - left, cy - top
    size = round(2 * h)
    start = about - 0.75
    pattern = os.path.join(tmp, "b%d_%%03d.png" % button)
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", "%.4f" % start, "-t", "1.9", "-i", video,
         "-vf", "scale=1920:1080,crop=%d:%d:%d:%d" % (size, size, left, top), pattern], check=True)
    frames = []
    while os.path.exists(pattern % (len(frames) + 1)):
        frames.append(Image.open(pattern % (len(frames) + 1)).convert("RGB"))
    grounds = [ground(f, cx, cy) for f in frames]
    # The press: the ground jumps (the disc appears under the picture) by more
    # than the 13 levels of the focus arriving, and goes on changing.
    press = None
    for j in range(2, len(frames) - FRAMES):
        jump = max(abs(grounds[j][c] - grounds[j - 1][c]) for c in range(3))
        nxt = max(abs(grounds[j + 1][c] - grounds[j - 1][c]) for c in range(3))
        if jump > 16 and nxt > 16:
            press = j
            break
    if press is None:
        print("button %d: no press found near %.2f s, skipped" % (button, about))
        return
    print("button %d: press at %.3f s" % (button, start + press / 60.0))
    # The colour is read where the picture is most solid: at rest, or on the
    # last frame when it ends up filled in (thin outlines are mostly blends).
    def spread(j):
        c = icon_colour(frames[j], grounds[j], cx, cy)
        return sum(abs(c[n] - grounds[j][n]) for n in range(3)), c
    colour = max(spread(press - 2), spread(press + FRAMES - 1))[1]
    light = still_colour(os.path.join(out_dir, "sys_%d_l.png" % button))
    # The power button's dialog starts dimming the menu on frame 17; its
    # picture is back at rest by then, so its animation stops at frame 15.
    for k in range(16 if button == 9 else FRAMES):
        frame, bg = frames[press + k], grounds[press + k]
        if button == 0:
            # Red disc with a white mark: the same in both themes.
            img = color_to_alpha(frame, bg, cx, cy)
            img.save(os.path.join(out_dir, "bar_0_%02d_d.png" % k))
            img.save(os.path.join(out_dir, "bar_0_%02d_l.png" % k))
            continue
        a = matte(frame, bg, colour, cx, cy)
        for tag, rgb in (("d", colour), ("l", light)):
            if rgb is None:
                continue
            img = Image.new("RGBA", frame.size, rgb + (0,))
            img.putalpha(a)
            img.save(os.path.join(out_dir, "bar_%d_%02d_%s.png" % (button, k, tag)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("--times", help="ten comma-separated press times in seconds, buttons left to right")
    args = ap.parse_args()
    times = DEFAULT_TIMES
    if args.times:
        times = [float(v) for v in args.times.split(",")]
        if len(times) != 10:
            sys.exit("--times needs ten values")
    base = os.environ.get("NSWHOME_DATA") or os.environ.get("DOCKSIDE_DATA") or os.path.join(os.environ["APPDATA"], "NSW-Home")  # DOCKSIDE_DATA: historical name
    # Until the program has moved it at its first start, the data folder still has the former name.
    if not os.path.isdir(base) and os.path.isdir(os.path.join(os.environ.get("APPDATA", ""), "Dockside")):  # historical name
        base = os.path.join(os.environ.get("APPDATA", ""), "Dockside")  # historical name
    out_dir = os.path.join(base, "pack")
    os.makedirs(out_dir, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for button, about in enumerate(times):
            if about > 0:
                cut(args.video, button, about, out_dir, tmp)
    print("pack written to", out_dir)


if __name__ == "__main__":
    main()
