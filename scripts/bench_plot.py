# SPDX-License-Identifier: Apache-2.0
"""Plot benchmark: yc.plot with millions of points, and matplotlib for comparison when it is installed.

For each size (default 100 thousand, 1 million and 10 million points of a random walk) it measures:
- yc.plot: the time to send the line to YCGrid, then the frame time while the view zooms and pans (YCGrid's own
  counters, perf.stats), i.e. what a user feels when exploring the data;
- matplotlib (Agg): the time to draw the figure once and to redraw after a zoom.

    runtime\\python\\python.exe -E -s scripts\\bench_plot.py [--pid N] [--sizes 100000,1000000,10000000] [--out report.json]

Start YCGrid first (or pass --launch to start a hidden one).
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))

import numpy as np  # noqa: E402

import yc  # noqa: E402
from yc import plot, ui  # noqa: E402


def bench_yc(n: int) -> dict:
    rng = np.random.default_rng(1)
    x = np.arange(n, dtype=np.float64)
    y = np.cumsum(rng.standard_normal(n))
    t0 = time.perf_counter()
    fig = plot.figure(f"bench {n:,}", size=(1000, 500))
    fig[0].line(x, y)
    send = time.perf_counter() - t0
    time.sleep(1.0)  # the first frames build the downsampling pyramid
    ui._rq("perf.stats")
    frames = 0
    t1 = time.perf_counter()
    while time.perf_counter() - t1 < 3.0:  # zoom in and out, pan
        k = frames % 60
        span = n * (0.02 + 0.98 * abs(30 - k) / 30)
        a = (n - span) * (k / 60)
        fig[0].set(xlim=(a, a + span))
        frames += 1
        time.sleep(1 / 60)
    st = ui._rq("perf.stats", last=200)
    fig.close()
    return {"points": n, "send_s": round(send, 3), "frame_cpu_avg_ms": round(st["frame_cpu_avg_ms"], 2),
            "frame_cpu_p99_ms": round(st["frame_cpu_p99_ms"], 2)}


def bench_mpl(n: int):
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return None
    rng = np.random.default_rng(1)
    x = np.arange(n, dtype=np.float64)
    y = np.cumsum(rng.standard_normal(n))
    t0 = time.perf_counter()
    fig, ax = plt.subplots(figsize=(10, 5), dpi=100)
    ax.plot(x, y)
    fig.canvas.draw()
    first = time.perf_counter() - t0
    t1 = time.perf_counter()
    ax.set_xlim(n * 0.25, n * 0.5)
    fig.canvas.draw()
    redraw = time.perf_counter() - t1
    plt.close(fig)
    return {"points": n, "first_draw_s": round(first, 3), "redraw_after_zoom_ms": round(redraw * 1000, 1)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int)
    ap.add_argument("--launch", action="store_true")
    ap.add_argument("--sizes", default="100000,1000000,10000000")
    ap.add_argument("--out")
    ns = ap.parse_args()
    proc = yc.launch(hidden=True) if ns.launch else None
    if not proc:
        yc.connect(ns.pid)
    try:
        sizes = [int(s) for s in ns.sizes.split(",")]
        report = {"yc.plot": [bench_yc(n) for n in sizes], "matplotlib": [r for r in (bench_mpl(n) for n in sizes) if r]}
    finally:
        if proc:
            proc.terminate()
    for k, rows in report.items():
        print(k)
        for r in rows:
            print("  " + ", ".join(f"{a} {b}" for a, b in r.items()))
    if not report["matplotlib"]:
        print("matplotlib isn't installed in this Python: run the script with one that has it to compare")
    if ns.out:
        with open(ns.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)


if __name__ == "__main__":
    main()
