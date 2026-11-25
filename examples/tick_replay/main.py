# SPDX-License-Identifier: Apache-2.0
"""Live quotes, record and replay: twenty symbols streamed into a bound table, a live chart, then a replay.

A feed thread sends quotes at 20 updates a second through shared memory (yc.data); the table flashes changes and a
chart follows one symbol's mid price. After `live` seconds the example records `record` seconds (Trading > Record
does the same), then replays them at 4x into the same table and chart (yc.replay), and goes back to live data. The
feed never stops: during the replay its frames are ignored, so the screen's code doesn't change.

Open tick_replay.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import os
import tempfile
import threading
import time

import numpy as np
import pandas as pd

from yc import plot, replay
from yc.data import bind

from ex_util import T, dark_table, fresh_sheet, thread, title

SYMBOLS = ["TXF", "MXF", "TSMC", "MTK", "HONHAI", "UMC", "ASE", "DELTA", "FUBON", "CATHAY", "CTBC", "MEGA", "CHT", "FET", "TWM",
           "NANYA", "FPC", "CSC", "EVA", "YM"]


def run(live: float = 4.0, record: float = 8.0, speed: float = 4.0, hz: float = 20.0):
    rng = np.random.default_rng(1)
    ws = fresh_sheet("Quotes", trading=True)
    title(ws, "Live quotes into a bound table (simulated feed); record and replay into the same screen",
          "Watch the status bar: REC while recording, REPLAY while the recording plays back.")
    px = rng.uniform(20, 1000, len(SYMBOLS)).round(1)
    px[0], px[1] = 23000, 23000
    df = pd.DataFrame({"Symbol": SYMBOLS, "Bid": px - 0.5, "Ask": px + 0.5, "Last": px, "Volume": np.zeros(len(SYMBOLS), int),
                       "Time": [pd.Timestamp.now()] * len(SYMBOLS)})
    t = bind(ws, df, key="Symbol", name="LiveQuotes", at="A4", flash=["Bid", "Ask", "Last"], stale_after=2,
             formats={"Bid": "#,##0.0", "Ask": "#,##0.0", "Last": "#,##0.0", "Volume": "#,##0", "Time": "hh:mm:ss.000"})
    dark_table(t)
    ws.Columns(6).ColumnWidth = 13
    fig = plot.figure("TXF mid (live)", size=(520, 300), pos=(560, 120), theme="dark")  # beside the table
    mid = fig[0].line([], [], label="TXF mid", color=T["yellow"], capacity=20000)
    fig[0].set(follow=30, time=True)
    stop = threading.Event()
    vol = np.zeros(len(SYMBOLS), int)

    def feed():
        nonlocal px
        while not stop.is_set():
            k = rng.integers(3, 9)
            idx = rng.choice(len(SYMBOLS), k, replace=False)
            px[idx] = np.maximum(0.1, px[idx] * (1 + rng.normal(0, 0.0008, k))).round(1)
            vol[idx] += rng.integers(1, 50, k)
            now = pd.Timestamp.now()
            t.update({"Symbol": [SYMBOLS[i] for i in idx], "Bid": px[idx] - 0.5, "Ask": px[idx] + 0.5, "Last": px[idx],
                      "Volume": vol[idx], "Time": [now] * k})
            time.sleep(1 / hz)

    def chart():
        # the chart reads the table (live or replayed: the same code)
        while not stop.is_set():
            try:
                row = t.to_frame().set_index("Symbol").loc["TXF"]
                mid.append([time.time()], [(row["Bid"] + row["Ask"]) / 2])
            except Exception:  # noqa: BLE001 - the workbook may be closing
                pass
            time.sleep(0.1)

    def script():
        time.sleep(live)
        path = replay.record(os.path.join(tempfile.gettempdir(), "yc_example_quotes.yctape"), tables=["LiveQuotes"])
        print(f"[example] recording to {path}")
        time.sleep(record)
        replay.stop_recording()
        print(f"[example] replaying at {speed:g}x (the live feed keeps running and is ignored)")
        replay.play(path, speed=speed)
        replay.wait(timeout=record / max(speed, 0.1) + 10)
        time.sleep(1.0)
        replay.live()
        print("[example] back to live data")

    for f in (feed, chart, script):
        thread(f, f"example-quotes-{f.__name__}")
    return t


def Workbook_Open():
    run()
