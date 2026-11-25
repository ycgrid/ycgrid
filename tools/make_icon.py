# SPDX-License-Identifier: Apache-2.0
"""Generate resources/ycgrid.ico: YCGrid's own icon, a steel blue grid with an amber trend line (rule 10: no
Office artwork; the colors are YCGrid's, not Excel's). The title bar draws the same design (src/ui/titlebar).

Usage (repo root): python tools/make_icon.py
Pure Python, no extra packages; writes a 32-bit BMP icon in six sizes: 16/24/32/48/64/256.
"""
from __future__ import annotations

import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "resources" / "ycgrid.ico"
SIZES = [16, 24, 32, 48, 64, 256]
BLUE = (0x1D, 0x5D, 0x8F)
DARK = (0x16, 0x4A, 0x72)
AMBER = (0xF5, 0xB3, 0x42)
# the trend line, in the grid's 0..1 coordinates (x right, y down)
TREND = [(0.15, 0.725), (0.40, 0.475), (0.575, 0.625), (0.85, 0.225)]


def seg_dist(px: float, py: float, a: tuple[float, float], b: tuple[float, float]) -> float:
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return ((px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2) ** 0.5


def pixel(x: float, y: float, n: int) -> tuple[int, int, int, int]:
    r = n * 0.20
    # coverage of a rounded rectangle (simple 2x2 supersampling)
    def inside(px: float, py: float) -> bool:
        cx = min(max(px, r), n - r)
        cy = min(max(py, r), n - r)
        return (px - cx) ** 2 + (py - cy) ** 2 <= r * r

    cov = sum(inside(x + dx, y + dy) for dx in (0.25, 0.75) for dy in (0.25, 0.75)) / 4
    if cov == 0:
        return (0, 0, 0, 0)
    m = n * 0.22
    lw = max(1.0, n / 22)
    col = BLUE
    if m <= x <= n - m and m <= y <= n - m:
        gx = (x - m) / (n - 2 * m)
        gy = (y - m) / (n - 2 * m)
        if gy < 1 / 3:
            col = DARK  # header row
        for k in (0, 1 / 3, 2 / 3, 1):
            if abs(gx - k) * (n - 2 * m) < lw / 2 + 0.01 or abs(gy - k) * (n - 2 * m) < lw / 2 + 0.01:
                col = tuple(int(c + (255 - c) * 0.6) for c in col)  # grid lines: light, behind the trend
    # the trend line over the whole tile
    tw = max(1.5, n / 11)
    if min(seg_dist(x / n, y / n, a, b) for a, b in zip(TREND, TREND[1:])) * n < tw / 2:
        col = AMBER
    return (*col, int(255 * cov))


def bmp_entry(n: int) -> bytes:
    rows = []
    for y in range(n - 1, -1, -1):  # BMP is bottom-up
        row = bytearray()
        for x in range(n):
            r, g, b, a = pixel(x, y, n)
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    xor = b"".join(rows)
    mask_row = ((n + 31) // 32) * 4
    and_mask = b"\x00" * (mask_row * n)
    header = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + xor + and_mask


def main() -> None:
    images = [bmp_entry(n) for n in SIZES]
    out = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    offset = 6 + 16 * len(SIZES)
    for n, img in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    for img in images:
        out += img
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(bytes(out))
    print(f"Generated {OUT.relative_to(ROOT)} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
