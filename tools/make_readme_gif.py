# SPDX-License-Identifier: Apache-2.0
"""Record the animated picture at the top of the README: the market watch example's live quotes and intraday chart.

Usage (repo root, with a Python that has Pillow; it is only a documentation tool):
    python tools/make_readme_gif.py [--exe dist_release\\ycgrid.exe] [--lang en zh-TW] [--seconds 14] [--fps 6]
        [--width 1280]

It opens examples/market_watch in a hidden YCGrid the way tools/make_readme_shots.py takes the gallery (the ribbon
collapsed, the same window size), lets the quotes stream, clicks another symbol half way so the chart follows it, and
writes docs/images/<lang>/live.gif.
"""
from __future__ import annotations

import argparse
import os
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import make_readme_shots as shots  # noqa: E402


def record(exe: str, lang: str, out: str, seconds: float, fps: float, width: int) -> None:
    from PIL import Image
    from yc import ui

    tmp = tempfile.mkdtemp(prefix="ycgif")
    paths = []
    s = shots.ExampleSession(exe, lang, "market_watch")
    try:
        s.collapse_ribbon()
        s.inp(f"m:20,{shots.SIZE[1] - 12}|w:6", 3)  # the mouse on the status bar; the quotes get going
        t0 = time.perf_counter()
        clicked = False
        i = 0
        while time.perf_counter() - t0 < seconds:
            if not clicked and time.perf_counter() - t0 > seconds * 0.45:
                s.click_cell("B6", 0)  # 2330's row: the chart follows the symbol picked
                s.inp(f"m:20,{shots.SIZE[1] - 12}|w:2", 0)
                clicked = True
            path = os.path.join(tmp, f"f{i:04d}.png")
            ui.screenshot(path, target="window", max_width=width)
            paths.append(path)
            i += 1
            time.sleep(max(0.0, (t0 + i / fps) - time.perf_counter()))
    finally:
        s.close()
    frames = [Image.open(p).convert("RGB") for p in paths]
    pal = frames[len(frames) // 2].quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    q = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    q[0].save(out, save_all=True, append_images=q[1:], duration=int(1000 / fps), loop=0, optimize=True)
    print(f"{os.path.relpath(out, ROOT)}: {len(q)} frames, {os.path.getsize(out) / 1e6:.1f} MB", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", default=os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    ap.add_argument("--lang", nargs="*", default=["en", "zh-TW"], help="the UI languages to record (en, zh-TW)")
    ap.add_argument("--seconds", type=float, default=14)
    ap.add_argument("--fps", type=float, default=6)
    ap.add_argument("--width", type=int, default=1280)
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(a.exe)), "python"))
    for lang in ("en", "zh-TW"):
        if lang in a.lang:
            record(a.exe, lang, os.path.join(ROOT, "docs", "images", lang, "live.gif"), a.seconds, a.fps, a.width)
    return 0


if __name__ == "__main__":
    sys.exit(main())
