#!/usr/bin/env python3
"""Draws the app icon, a pixel-art golden crescent moon, and writes assets/AppIcon.icns.

The art is a 32x32 grid of "pixels"; each one becomes a 32x32 block in the
1024x1024 master image, and the smaller icon sizes are box-filtered from that.
Uses only the Python standard library plus macOS's iconutil.

Usage: tools/make_icon.py
"""
import math
import os
import shutil
import struct
import subprocess
import tempfile
import zlib

GRID = 32
MASTER = 1024
CELL = MASTER // GRID

CLEAR = (0, 0, 0, 0)
NIGHT_TOP = (34, 30, 74, 255)
NIGHT_BOTTOM = (14, 14, 38, 255)
STAR = (230, 230, 255, 255)
GOLD_DARK = (176, 112, 18, 255)
GOLD = (245, 182, 40, 255)
GOLD_LIGHT = (255, 226, 122, 255)


def inside(x, y, cx, cy, r):
    return (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r


def is_moon_shape(x, y):
    # A circle with a smaller, offset circle cut out of it.
    return inside(x, y, 15.0, 16.5, 10.5) and not inside(x, y, 20.5, 12.0, 9.0)


def moon_mask():
    mask = {(x, y) for y in range(GRID) for x in range(GRID) if is_moon_shape(x, y)}
    # Drop lone bumps: pixels touching only one other moon pixel.
    while True:
        lone = {(x, y) for x, y in mask
                if sum((x + dx, y + dy) in mask for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))) <= 1}
        if not lone:
            return mask
        mask -= lone


def in_background(x, y):
    # macOS-style rounded square with a 3-pixel margin and stepped corners.
    lo, hi, radius = 3, 28, 6
    if not (lo <= x <= hi and lo <= y <= hi):
        return False
    cx = min(max(x, lo + radius), hi - radius)
    cy = min(max(y, lo + radius), hi - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius


def draw_grid():
    grid = [[CLEAR] * GRID for _ in range(GRID)]
    for y in range(GRID):
        t = (y - 3) / 25
        sky = tuple(round(a + (b - a) * t) for a, b in zip(NIGHT_TOP, NIGHT_BOTTOM))
        for x in range(GRID):
            if in_background(x, y):
                grid[y][x] = sky
    for x, y in [(20, 8), (24, 13), (9, 6), (25, 20)]:
        grid[y][x] = STAR
    moon = moon_mask()
    for x, y in moon:
        edge = any((x + dx, y + dy) not in moon for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        # Light comes from the upper left: the outer rim on that side is brighter.
        outer = math.hypot(x + 0.5 - 15.0, y + 0.5 - 16.5)
        lit = outer > 8.0 and (x + y) < 29
        grid[y][x] = GOLD_DARK if edge else GOLD_LIGHT if lit else GOLD
    return grid


def render(grid, size):
    """Returns RGBA rows for an icon of `size` pixels (box filter of the master image)."""
    step = MASTER / size
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            if step <= CELL:
                row.extend(grid[int(py * step) // CELL][int(px * step) // CELL])
                continue
            n = int(step // CELL)  # grid cells per output pixel
            gx, gy = int(px * step) // CELL, int(py * step) // CELL
            cells = [grid[gy + j][gx + i] for j in range(n) for i in range(n)]
            alpha = sum(c[3] for c in cells)
            if alpha == 0:
                row.extend(CLEAR)
                continue
            # Premultiplied average so transparent cells don't darken the edge.
            rgb = [round(sum(c[k] * c[3] for c in cells) / alpha) for k in range(3)]
            row.extend(rgb + [round(alpha / len(cells))])
        rows.append(bytes(row))
    return rows


def write_png(path, rows, width, height=None):
    """Writes RGBA rows (bytes, 4 per pixel) as a PNG. `height` defaults to `width`."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    height = width if height is None else height
    raw = b"".join(b"\x00" + row for row in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets = os.path.join(root, "assets")
    os.makedirs(assets, exist_ok=True)
    grid = draw_grid()
    work = tempfile.mkdtemp()
    try:
        iconset = os.path.join(work, "AppIcon.iconset")
        os.makedirs(iconset)
        for base in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                size = base * scale
                name = f"icon_{base}x{base}{'@2x' if scale == 2 else ''}.png"
                write_png(os.path.join(iconset, name), render(grid, size), size)
        shutil.copy(os.path.join(iconset, "icon_512x512@2x.png"), os.path.join(assets, "AppIcon.png"))
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(assets, "AppIcon.icns")], check=True)
    finally:
        shutil.rmtree(work)
    print("Wrote assets/AppIcon.icns and assets/AppIcon.png")


if __name__ == "__main__":
    main()
