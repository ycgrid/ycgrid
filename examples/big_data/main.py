# SPDX-License-Identifier: Apache-2.0
"""Big data: twenty million simulated ticks written once as column files, then drawn as one line and opened in a Data
View that scrolls, sorts and filters all of them - neither goes through cells, the data channel or Python objects.

Open big_data.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import os
import tempfile
import time

import numpy as np

from yc import plot
from yc.data import save_columns, view

from ex_util import T, fresh_sheet, title


def run(rows: int = 20_000_000):
    t0 = time.perf_counter()
    rng = np.random.default_rng(3)
    ts = np.datetime64("2026-01-02T08:45:00", "ns") + np.cumsum(rng.integers(1, 40, rows)).astype("timedelta64[ms]").astype("timedelta64[ns]")
    price = 23000 + np.cumsum(rng.choice([-1.0, 0.0, 1.0], rows, p=[0.3, 0.4, 0.3]))
    size = rng.integers(1, 50, rows)
    side = np.where(rng.random(rows) < 0.5, "B", "S")
    folder = os.path.join(tempfile.gettempdir(), "ycgrid_big_data")
    files = save_columns({"Time": ts, "Price": price, "Size": size, "Side": side}, folder)
    t1 = time.perf_counter()
    ws = fresh_sheet("Big Data")
    title(ws, f"{rows:,} ticks: one line and a Data View, read from column files by YCGrid",
          f"Files: {folder}. In the Data View: click a header to sort, type >23050 or B in the filter row, To Sheet copies the shown rows.")
    fig = plot.figure(f"{rows:,} ticks", size=(980, 520), pos=(80, 140), theme="dark")
    fig[0].plot_files("line", x=files["Time"], y=files["Price"], label="price", color=T["yellow"])
    fig[0].set(title="Price, every tick (zoom in: the detail is all there)")
    dv = view(files, title="Ticks")  # the window adds the row count
    t2 = time.perf_counter()
    print(f"[example] wrote {rows:,} rows in {t1 - t0:.1f} s, opened the chart and the Data View in {t2 - t1:.2f} s")
    return fig, dv


def Workbook_Open():
    run()
