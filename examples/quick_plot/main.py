# SPDX-License-Identifier: Apache-2.0
"""Fast plots: ten million points, a million-value histogram and a correlation heatmap.

The same calls as matplotlib (plot.line, plot.hist, heatmap), but the data go to YCGrid through shared memory and the
lines are downsampled on the GPU side per pixel, so panning and zooming ten million points stays smooth. The timings
printed are the Python side (making the data and sending it).

Open quick_plot.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import time

import numpy as np

from yc import plot


def run(points: int = 10_000_000, seed: int = 2):
    rng = np.random.default_rng(seed)
    t0 = time.perf_counter()
    x = np.arange(points, dtype=np.float64)
    y = np.cumsum(rng.standard_normal(points))
    fig = plot.figure(f"{points:,} points", size=(900, 420))
    fig[0].line(x, y, label="random walk")
    c = np.cumsum(np.r_[0.0, y])
    ma = np.full(points, np.nan)
    ma[4999:] = (c[5000:] - c[:-5000]) / 5000  # a 5,000-point moving average with cumulative sums
    fig[0].line(x, ma, label="moving average", color="#ef6c00")
    fig[0].set(title="Zoom with the wheel, drag to pan, double-click to fit")
    t1 = time.perf_counter()
    r = rng.standard_t(4, 1_000_000) * 0.01
    h = plot.figure("1,000,000 returns", size=(620, 380))
    h[0].hist(r, bins=200, label="Student t (4)")
    h[0].set(title="Histogram of a million values")
    t2 = time.perf_counter()
    names = ["TXF", "TSMC", "MTK", "HONHAI", "FUBON", "CTBC", "USDTWD", "GOLD", "US10Y", "SPX"]
    f = rng.standard_normal((3, 2000))
    load = rng.normal(0, 1, (len(names), 3))
    data = load @ f + rng.standard_normal((len(names), 2000))
    c = np.corrcoef(data)
    hm = plot.figure("Correlation", size=(620, 560))
    hm[0].heatmap(c, xlabels=names, ylabels=names, cmap="rdbu", vmin=-1, vmax=1)
    hm[0].set(title="Correlation matrix")
    print(f"[example] {points:,} points sent in {t1 - t0:.2f} s; histogram {t2 - t1:.2f} s")
    return fig, h, hm


def Workbook_Open():
    run()
