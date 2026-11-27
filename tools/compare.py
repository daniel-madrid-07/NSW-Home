"""Put regions of an NSW-Home --shot render next to the same regions of a
reference capture, enlarged, for checking the menu against the real console.

Usage: python compare.py <ours.png> <reference.png> <out_dir>
Both images are 1920x1080. Each output image shows ours on top, reference below.
"""
import os
import sys

from PIL import Image

REGIONS = {
    "status": (1440, 70, 1880, 160),
    "footer": (1300, 960, 1880, 1070),
    "pad": (50, 960, 200, 1070),
    "avatars": (50, 50, 460, 180),
    "bar_left": (320, 760, 1000, 920),
    "bar_right": (940, 760, 1620, 920),
}


def main():
    ours = Image.open(sys.argv[1]).convert("RGB")
    ref = Image.open(sys.argv[2]).convert("RGB")
    out_dir = sys.argv[3]
    assert ours.size == ref.size == (1920, 1080), (ours.size, ref.size)
    for name, box in REGIONS.items():
        a, b = ours.crop(box), ref.crop(box)
        w, h = a.size
        sheet = Image.new("RGB", (w * 2, h * 4 + 12), (255, 0, 255))
        sheet.paste(a.resize((w * 2, h * 2), Image.NEAREST), (0, 0))
        sheet.paste(b.resize((w * 2, h * 2), Image.NEAREST), (0, h * 2 + 12))
        sheet.save(os.path.join(out_dir, f"cmp_{name}.png"))


if __name__ == "__main__":
    main()
