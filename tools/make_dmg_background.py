#!/usr/bin/env python3
"""Draws the background of the DMG window and writes assets/dmg-background.tiff.

The window is 600x400 points: the app icon sits on the left, the Applications
folder on the right, and a pixel-art golden arrow between them says "drag me
over there". The TIFF holds a normal and a Retina (2x) version.
Uses only the Python standard library plus macOS's tiffutil.

Usage: tools/make_dmg_background.py
"""
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_icon import GOLD, GOLD_DARK, GOLD_LIGHT, NIGHT_TOP, write_png  # noqa: E402

WIDTH, HEIGHT = 600, 400  # window size in points; must match tools/make_dmg.sh
APP_X, APPS_X, ICON_Y = 160, 440, 190  # icon centres; must match tools/make_dmg.sh

SKY_TOP = (251, 246, 233)
SKY_BOTTOM = (241, 228, 196)
TEXT = NIGHT_TOP[:3]

FONT = {  # 5x7 pixel letters
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "G": ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
    "I": ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    " ": ["00000"] * 7,
}


class Canvas:
    """An RGB image addressed in points, drawn at `scale` pixels per point."""

    def __init__(self, scale):
        self.scale = scale
        self.w, self.h = WIDTH * scale, HEIGHT * scale
        self.px = []
        for y in range(self.h):
            t = y / (self.h - 1)
            self.px.append([tuple(round(a + (b - a) * t) for a, b in zip(SKY_TOP, SKY_BOTTOM))] * self.w)

    def rect(self, x, y, w, h, color):
        s = self.scale
        for py in range(round(y * s), round((y + h) * s)):
            row = self.px[py]
            for px in range(round(x * s), round((x + w) * s)):
                row[px] = color

    def rows(self):
        return [bytes(v for c in row for v in (*c, 255)) for row in self.px]


def arrow_cells():
    """The arrow as a set of 4-point cells, pointing right, centred on ICON_Y."""
    cells = set()
    mid = ICON_Y // 4  # the cell row just below the centre line
    for x in range(62, 80):  # shaft
        cells.update((x, y) for y in range(mid - 2, mid + 2))
    for i in range(9):  # head
        cells.update((80 + i, y) for y in range(mid - 9 + i, mid + 9 - i))
    return cells


def draw(scale):
    c = Canvas(scale)
    # A few twinkles, kept away from the icons and the arrow.
    rng = random.Random(7)
    placed = 0
    while placed < 14:
        x, y = rng.randrange(8, WIDTH - 12), rng.randrange(8, HEIGHT - 12)
        if 70 < y < 350 and (abs(x - APP_X) < 100 or abs(x - APPS_X) < 100 or 230 < x < 370):
            continue
        size = rng.choice((2, 2, 4))
        c.rect(x, y, size, size, GOLD_LIGHT[:3] if size == 2 else GOLD[:3])
        placed += 1
    # The arrow: gold, a light top edge, and a dark outline (like the icon's moon).
    cells = arrow_cells()
    for x, y in cells:
        edge = any((x + dx, y + dy) not in cells for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        top = (x, y - 2) not in cells and not edge
        color = GOLD_DARK if edge else GOLD_LIGHT if top else GOLD
        c.rect(x * 4, y * 4, 4, 4, color[:3])
    # Caption under the icons' labels.
    text = "DRAG TO APPLICATIONS"
    dot = 2  # points per font pixel
    advance = 6 * dot
    left = (WIDTH - (len(text) * advance - dot)) / 2
    for i, ch in enumerate(text):
        for row, bits in enumerate(FONT[ch]):
            for col, bit in enumerate(bits):
                if bit == "1":
                    c.rect(left + i * advance + col * dot, 318 + row * dot, dot, dot, TEXT)
    return c


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    work = tempfile.mkdtemp()
    try:
        paths = []
        for scale, name in ((1, "bg.png"), (2, "bg@2x.png")):
            canvas = draw(scale)
            path = os.path.join(work, name)
            write_png(path, canvas.rows(), canvas.w, canvas.h)
            paths.append(path)
        out = os.path.join(root, "assets", "dmg-background.tiff")
        subprocess.run(["tiffutil", "-cathidpicheck", *paths, "-out", out], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        preview = os.path.join(root, "build", "dmg-background-preview.png")
        os.makedirs(os.path.dirname(preview), exist_ok=True)
        os.replace(paths[1], preview)
    finally:
        for name in os.listdir(work):
            os.remove(os.path.join(work, name))
        os.rmdir(work)
    print("Wrote assets/dmg-background.tiff (preview: build/dmg-background-preview.png)")


if __name__ == "__main__":
    main()
