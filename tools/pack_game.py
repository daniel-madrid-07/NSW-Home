r"""Cut the console's loading mark out of the owner's own capture into the local
icon pack that NSW-Home loads if present (launch_00 .. launch_72).

The pack is written to %NSWHOME_DATA%\pack (or %APPDATA%\NSW-Home\pack when
that variable is unset) and stays on this PC: it is not part of NSW-Home and
must not be committed or redistributed. Without it NSW-Home draws its own mark.

Usage:  python tools/pack_game.py <capture of a game start> [--black SECONDS]

--black is the time of the first completely black frame after the menu fades
out (19.567 in w5.mkv, the default). Needs ffmpeg on the PATH and Pillow.

Frames, counted from that black frame at 60 a second (the mark itself moves
at 30 a second, so they come in pairs):
  launch_00        +56          the still mark
  launch_01..55    +58..+112    the mark draws itself
  launch_56..72    +342..+358   it folds back into the still mark
"""
import argparse
import os
import subprocess
import sys

from PIL import Image

BOX = (1560, 770, 330, 300)  # x, y, w, h of the mark in a 1920x1080 frame


def grab(video, seconds, count):
    x, y, w, h = BOX
    raw = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-ss", "%.4f" % seconds, "-i", video,
         "-frames:v", str(count), "-vf", "crop=%d:%d:%d:%d" % (w, h, x, y), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE, check=True).stdout
    size = w * h * 3
    return [Image.frombytes("RGB", (w, h), raw[i * size:(i + 1) * size]) for i in range(len(raw) // size)]


def black_to_alpha(img, floor=0.04):
    """The mark is light on black: brightness becomes opacity."""
    out = Image.new("RGBA", img.size)
    src, dst = img.load(), out.load()
    for yy in range(img.height):
        for xx in range(img.width):
            r, g, b = src[xx, yy]
            a = max(r, g, b) / 255.0
            if a <= floor:
                dst[xx, yy] = (0, 0, 0, 0)
            else:
                dst[xx, yy] = (min(255, int(r / a)), min(255, int(g / a)), min(255, int(b / a)), int(a * 255))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("--black", type=float, default=19.5667)
    args = ap.parse_args()
    base = os.environ.get("NSWHOME_DATA") or os.environ.get("DOCKSIDE_DATA") or os.path.join(os.environ["APPDATA"], "NSW-Home")  # DOCKSIDE_DATA: historical name
    # Until the program has moved it at its first start, the data folder still has the former name.
    if not os.path.isdir(base) and os.path.isdir(os.path.join(os.environ.get("APPDATA", ""), "Dockside")):  # historical name
        base = os.path.join(os.environ.get("APPDATA", ""), "Dockside")  # historical name
    out_dir = os.path.join(base, "pack")
    os.makedirs(out_dir, exist_ok=True)
    frames = grab(args.video, args.black + 56 / 60.0, 1)
    frames += grab(args.video, args.black + 58 / 60.0, 55)
    frames += grab(args.video, args.black + 342 / 60.0, 17)
    if len(frames) != 73:
        sys.exit("expected 73 frames, got %d: is --black right?" % len(frames))
    for i, frame in enumerate(frames):
        black_to_alpha(frame).save(os.path.join(out_dir, "launch_%02d.png" % i))
    print("73 frames written to", out_dir)


if __name__ == "__main__":
    main()
