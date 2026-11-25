# SPDX-License-Identifier: Apache-2.0
"""A million live cells: a 100,000 x 10 bound table updated as fast as possible, with throughput and latency.

A thread rewrites a random 10% of the rows (100,000 cells) in each update for `seconds`; YCGrid applies them by key
and redraws only what is visible. The status cells show the cells written per second and the latency from Python to
the screen (perf.stats), the same numbers as scripts\\bench.py.

Open million_cells.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import time

import numpy as np
import pandas as pd

from yc import ui
from yc.data import bind

from ex_util import fresh_sheet, header, thread, title, write


def run(rows: int = 100_000, cols: int = 10, seconds: float = 15.0, fraction: float = 0.1):
    rng = np.random.default_rng(0)
    ws = fresh_sheet("Million")
    title(ws, f"A million live cells: {rows:,} rows x {cols} columns, {fraction:.0%} of the rows rewritten per update",
          "Scroll anywhere while it runs: the grid stays responsive. Results appear on the right.")
    data = {"Key": np.arange(rows)}
    for j in range(cols - 1):
        data[f"C{j + 1}"] = rng.standard_normal(rows)
    t = bind(ws, pd.DataFrame(data), key="Key", name="Million", at="A4", record=False,
             formats={f"C{j + 1}": "0.0000" for j in range(cols - 1)})
    header(write(ws, "M4", [["Measure", "Value"]]))
    write(ws, "M5", [["Updates", 0], ["Cells written / s", 0], ["Latency p50 (ms)", 0], ["Latency p99 (ms)", 0], ["Status", "running"]])
    ws.Columns(13).ColumnWidth = 20
    ws.Columns(14).ColumnWidth = 14
    ws.Range("N6").NumberFormat = "#,##0"

    def loop():
        n = int(rows * fraction)
        ui._rq("perf.stats")
        t0 = time.perf_counter()
        sent = updates = 0
        while time.perf_counter() - t0 < seconds:
            keys = rng.choice(rows, n, replace=False)
            upd = {"Key": keys}
            for j in range(cols - 1):
                upd[f"C{j + 1}"] = rng.standard_normal(n)
            t.update(upd)
            sent += n * cols
            updates += 1
        dt = time.perf_counter() - t0
        st = ui._rq("perf.stats")
        ws.Range("N5:N9").Value = [[updates], [sent / dt], [round(st.get("latency_p50_ms", 0), 1)], [round(st.get("latency_p99_ms", 0), 1)],
                                   ["done"]]
        print(f"[example] {sent / dt / 1e6:.1f} million cells/s; latency p50 {st.get('latency_p50_ms', 0):.0f} ms, "
              f"p99 {st.get('latency_p99_ms', 0):.0f} ms")

    thread(loop, "example-million")
    return t


def Workbook_Open():
    run()
