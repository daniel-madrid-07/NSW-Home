"""Measure the user page: NSW-Home's --shot stills against capture frames.

Usage: python tools/measure_user.py <ours.png> <capture.png> [more pairs...]

For each pair it prints, side by side (ours | capture), in capture pixels:
  - the y of every horizontal rule in the left list and in the right column,
  - the ink box (x0 x1 y0 y1) of what lies in each band between those rules,
    for the left list (x 190-600) and the right column (x 700-1730).
Text boxes differ in width where the two fonts differ; positions (left edge,
vertical middle) and the rules are what is being compared.
"""
import sys

import numpy as np
from PIL import Image


def rules(g, x0, x1, y0, y1, bg):
    """Rows that are a thin line across most of [x0, x1): darker than the ground, 1-3 px."""
    strip = g[y0:y1, x0:x1]
    dark = (np.abs(strip - bg) > 8).mean(axis=1) > 0.9
    ys, y = [], 0
    while y < len(dark):
        if dark[y]:
            e = y
            while e + 1 < len(dark) and dark[e + 1]:
                e += 1
            if e - y < 4:
                ys.append(y0 + (y + e) / 2)
            y = e + 1
        else:
            y += 1
    return ys


def ink_boxes(g, x0, x1, y0, y1, bg, thr=40):
    """Ink boxes of the runs of rows that hold anything much darker or more colourful than the ground."""
    m = np.abs(g[y0:y1, x0:x1] - bg) > thr
    rows = m.any(axis=1)
    out, y = [], 0
    while y < len(rows):
        if rows[y]:
            e = y
            # bridge gaps of up to 6 rows (accents, descenders)
            while e + 1 < len(rows) and (rows[e + 1] or rows[e + 1:e + 7].any()):
                e += 1
            cols = np.nonzero(m[y:e + 1].any(axis=0))[0]
            if e - y >= 6:
                out.append((x0 + cols[0], x0 + cols[-1], y0 + y, y0 + e))
            y = e + 1
        else:
            y += 1
    return out


def measure(path):
    im = Image.open(path).convert("RGB")
    assert im.size == (1920, 1080), im.size
    g = np.asarray(im.convert("L")).astype(int)
    bg = int(np.median(g[140:960, 620:640]))
    return {
        "left rules": rules(g, 180, 560, 135, 965, bg),
        "right rules": rules(g, 720, 1700, 135, 965, bg),
        "left ink": ink_boxes(g, 190, 600, 135, 965, bg),
        "right ink": ink_boxes(g, 700, 1730, 135, 965, bg),
    }


def main():
    args = sys.argv[1:]
    for ours, ref in zip(args[0::2], args[1::2]):
        a, b = measure(ours), measure(ref)
        print(f"== {ours}  |  {ref}")
        for key in a:
            print(f"  {key}:")
            for i in range(max(len(a[key]), len(b[key]))):
                va = a[key][i] if i < len(a[key]) else "-"
                vb = b[key][i] if i < len(b[key]) else "-"
                print(f"    {str(va):34} | {vb}")


if __name__ == "__main__":
    main()
