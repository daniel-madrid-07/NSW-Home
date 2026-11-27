r"""Cut the strip of pictures at the top of the console's "change the order"
screen (which ways a controller can be held) out of the owner's own capture
into the local icon pack that NSW-Home loads if present (pads_order_d).

The pack is written to %NSWHOME_DATA%\pack (or %APPDATA%\NSW-Home\pack when
that variable is unset) and stays on this PC: it is not part of NSW-Home and
must not be committed or redistributed. Without it the strip stays empty.

Usage:  python tools/pack_pads.py <walkthrough capture> [--at SECONDS]

--at is a moment with the order screen up and no controller chosen yet
(84.5 in walkthrough.mp4, the default; the screen is only captured in the
dark theme). Needs ffmpeg on the PATH and Pillow.
"""
import argparse
import os
import subprocess

from PIL import Image

BOX = (500, 20, 920, 206)  # x, y, w, h in a 1920x1080 frame; ui.cpp draws it at the same place


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("--at", type=float, default=84.5)
    args = ap.parse_args()
    x, y, w, h = BOX
    raw = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-ss", "%.4f" % args.at, "-i", args.video,
         "-frames:v", "1", "-vf", "crop=%d:%d:%d:%d" % (w, h, x, y), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE, check=True).stdout
    base = os.environ.get("NSWHOME_DATA") or os.environ.get("DOCKSIDE_DATA") or os.path.join(os.environ["APPDATA"], "NSW-Home")  # DOCKSIDE_DATA: historical name
    # Until the program has moved it at its first start, the data folder still has the former name.
    if not os.path.isdir(base) and os.path.isdir(os.path.join(os.environ.get("APPDATA", ""), "Dockside")):  # historical name
        base = os.path.join(os.environ.get("APPDATA", ""), "Dockside")  # historical name
    out_dir = os.path.join(base, "pack")
    os.makedirs(out_dir, exist_ok=True)
    Image.frombytes("RGB", (w, h), raw).save(os.path.join(out_dir, "pads_order_d.png"))
    print("pads_order_d.png written to", out_dir)


if __name__ == "__main__":
    main()
