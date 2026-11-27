"""Cut the menu icons out of YOUR OWN capture of YOUR OWN console into a local
icon pack that NSW-Home loads if present.

The pack is written to %APPDATA%\\NSW-Home\\pack and stays on this PC: it is not
part of NSW-Home and must not be committed or redistributed. Without a pack,
NSW-Home draws its own icons.

Usage:  python make_icon_pack.py <recording.mp4> [--dark SECONDS] [--light SECONDS]

--dark / --light are timestamps of a frame showing the HOME menu in that theme
with the cursor on a game (so no system button is highlighted).
Needs ffmpeg on PATH and Pillow.
"""
import argparse
import os
import subprocess
import sys
import tempfile

from PIL import Image

SCALE = 1.5  # capture is 1920x1080, NSW-Home lays out in 1280x720
SYS_COUNT = 10
# Centres measured on the capture: the first button stands further apart.
SYS_X = [269.5] + [360.6 + i * 81.3 for i in range(9)]
SYS_Y, SYS_HALF = 559.7, 36.0
PAD_BOX = (46, 654, 114, 708)
# Status row icons, in layout units (x0, y0, x1, y1).
WIFI_BOX = (1066.67, 60, 1106.67, 90)
BATT_BOX = (1192, 61.33, 1237.33, 90.67)


def grab(video, seconds, out):
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", str(seconds), "-i", video,
         "-frames:v", "1", "-vf", "scale=1920:1080", out],
        check=True)
    return Image.open(out).convert("RGB")


def median_color(img, box):
    px = sorted(img.crop(box).getdata())
    return px[len(px) // 2]


def color_to_alpha(img, bg, floor=0.07):
    """Remove a flat background: every pixel becomes the colour and opacity
    that, laid back over `bg`, reproduces the capture."""
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
            dst[x, y] = rgb + (round(a * 255),)
    return out


def circle_mask(img, radius):
    """Clear everything outside a centred circle (drops the notification dot)."""
    cx, cy = img.width / 2, img.height / 2
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            if (x - cx) ** 2 + (y - cy) ** 2 > radius ** 2:
                px[x, y] = (0, 0, 0, 0)
    return img


def build(frame, tag, out_dir):
    for i in range(SYS_COUNT):
        cx, cy = SYS_X[i] * SCALE, SYS_Y * SCALE
        h = SYS_HALF * SCALE
        crop = frame.crop((round(cx - h), round(cy - h), round(cx + h), round(cy + h)))
        # Bottom-left corner of the crop is always plain bar colour.
        bg = median_color(crop, (0, crop.height - 8, 8, crop.height))
        icon = color_to_alpha(crop, bg)
        if i == 0:
            icon = circle_mask(icon, 34 * SCALE)
        icon.save(os.path.join(out_dir, f"sys_{i}_{tag}.png"))
    box = tuple(round(v * SCALE) for v in PAD_BOX)
    crop = frame.crop(box)
    bg = median_color(crop, (crop.width - 8, 0, crop.width, 8))
    color_to_alpha(crop, bg).save(os.path.join(out_dir, f"pad_{tag}.png"))
    for name, layout_box in (("wifi", WIFI_BOX), ("batt", BATT_BOX)):
        crop = frame.crop(tuple(round(v * SCALE) for v in layout_box))
        bg = median_color(crop, (0, 0, 6, 6))
        color_to_alpha(crop, bg, floor=0.1).save(os.path.join(out_dir, f"{name}_{tag}.png"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("--dark", type=float)
    ap.add_argument("--light", type=float)
    ap.add_argument("--loading", type=float,
                    help="timestamp of the black loading screen shown while a game starts")
    ap.add_argument("--loading-anim", type=float,
                    help="timestamp where the loading mark starts to appear (animated, 36 frames)")
    args = ap.parse_args()
    if all(v is None for v in (args.dark, args.light, args.loading, args.loading_anim)):
        sys.exit("give at least one of --dark, --light, --loading, --loading-anim")

    out_dir = os.path.join(os.environ["APPDATA"], "NSW-Home", "pack")
    os.makedirs(out_dir, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for tag, seconds in (("d", args.dark), ("l", args.light)):
            if seconds is None:
                continue
            build(grab(args.video, seconds, os.path.join(tmp, tag + ".png")), tag, out_dir)
        if args.loading is not None:
            # Bottom-right corner of the loading screen, black turned transparent.
            frame = grab(args.video, args.loading, os.path.join(tmp, "loading.png"))
            corner = frame.crop((round(960 * SCALE), round(520 * SCALE), 1920, 1080))
            color_to_alpha(corner, (0, 0, 0), floor=0.04).save(os.path.join(out_dir, "loading.png"))
        if args.loading_anim is not None:
            # 36 frames at 30 a second of the mark's corner (330x300 at 1560,770).
            pattern = os.path.join(tmp, "la_%02d.png")
            subprocess.run(
                ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", str(args.loading_anim), "-i", args.video,
                 "-frames:v", "36", "-vf", "fps=30,scale=1920:1080,crop=330:300:1560:770", "-start_number", "0",
                 pattern], check=True)
            for i in range(36):
                frame = Image.open(pattern % i).convert("RGB")
                color_to_alpha(frame, (0, 0, 0), floor=0.04).save(os.path.join(out_dir, "load_%02d.png" % i))
    print("pack written to", out_dir)


if __name__ == "__main__":
    main()
