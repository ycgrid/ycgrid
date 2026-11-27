# SPDX-License-Identifier: Apache-2.0
"""History playback: a CSV of past ticks played into a live quote table and candles at 60x speed.

The example writes a CSV of three symbols' ticks (simulated, two hours of a session) to a temporary folder, reads it
back with pandas like any file of yours, and plays it into a bound quote table with change flashes, while yc.bars
builds one-minute candles of TXF from the same rows. The player is a small class below (a thread that sends the rows
whose time has come, every 100 ms, as one update of the table): pause, resume and seek are a few lines each. YCGrid's
own recordings (.yctape) play back with yc.replay instead.

Open history_playback.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import os
import tempfile
import threading
import time

import numpy as np
import pandas as pd

from yc import bars, plot, safety
from yc.data import bind

from ex_util import chart, dark_table, fresh_sheet, thread, title

_state: dict = {}


def make_csv(path: str, seed: int = 41, hours: float = 2.0) -> str:
    """Two hours of 1-second ticks for TXF, MXF and TE (bid, ask, last, size) as a CSV."""
    rng = np.random.default_rng(seed)
    n = int(hours * 3600)
    t = pd.date_range("2026-10-23 08:45:00", periods=n, freq="s")
    frames = []
    for sym, p0, tick, vol in (("TXF", 23000.0, 1.0, 0.00012), ("MXF", 23000.0, 1.0, 0.00012), ("TE", 1180.0, 0.05, 0.00015)):
        px = p0 * np.exp(np.cumsum(rng.normal(0, vol, n)))
        last = np.round(px / tick) * tick
        frames.append(pd.DataFrame({"time": t, "symbol": sym, "bid": last - tick, "ask": last + tick, "last": last,
                                    "size": rng.integers(1, 30, n)}))
    df = pd.concat(frames).sort_values(["time", "symbol"], kind="stable")
    df.to_csv(path, index=False)
    return path


class Player:
    """Plays rows (sorted by `time`) into a bound table at `speed` x real time; on_rows(rows) sees every batch."""

    def __init__(self, rows: pd.DataFrame, table, speed: float = 60.0, batch: float = 0.1, columns=None, on_rows=None):
        self.rows, self.table, self.speed, self.batch = rows.reset_index(drop=True), table, speed, batch
        self.columns = columns or [c for c in rows.columns if c != "time"]
        self.t = (rows["time"] - rows["time"].iloc[0]).dt.total_seconds().to_numpy()
        self.on_rows, self.i, self.clock = on_rows, 0, 0.0  # next row; data seconds played
        self._paused, self._stop = threading.Event(), threading.Event()

    def start(self):
        thread(self._run, "example-playback")
        return self

    def pause(self):
        self._paused.set()

    def resume(self):
        self._paused.clear()

    def stop(self):
        self._stop.set()

    def seek(self, when):
        """Jump to a time (a datetime, or seconds from the start)."""
        s = (pd.Timestamp(when) - self.rows["time"].iloc[0]).total_seconds() if not isinstance(when, (int, float)) else float(when)
        self.clock, self.i = s, int(np.searchsorted(self.t, s))

    @property
    def done(self) -> bool:
        return self.i >= len(self.t)

    def _run(self):
        last = time.time()
        while not self._stop.is_set() and not self.done:
            time.sleep(self.batch)
            now = time.time()
            if not self._paused.is_set():
                self.clock += (now - last) * self.speed
                j = int(np.searchsorted(self.t, self.clock, side="right"))
                if j > self.i:
                    rows = self.rows.iloc[self.i:j]
                    self.i = j
                    # one update by key: the last row of each symbol in the batch
                    self.table.update(rows.groupby("symbol").tail(1)[self.columns])
                    if self.on_rows is not None:
                        self.on_rows(rows)
            last = now


def run(speed: float = 60.0):
    if "player" in _state:
        _state["player"].stop()
    path = make_csv(os.path.join(tempfile.gettempdir(), "ycgrid_history_ticks.csv"))
    ticks = pd.read_csv(path, parse_dates=["time"])  # the same call as for your own files
    ws = fresh_sheet("Playback", trading=True)
    title(ws, f"History playback: {len(ticks):,} ticks from a CSV at {speed:g}x",
          f"File: {path}. The quote table and the candles are fed by the player, exactly as live data would be.")
    cols = ["symbol", "bid", "ask", "last", "size"]
    quotes = bind(ws, ticks.groupby("symbol").head(1)[cols].reset_index(drop=True), key="symbol", name="Playback", at="A4",
                  formats={"bid": {"tick": 0.05}, "ask": {"tick": 0.05}, "last": {"tick": 0.05}, "size": "#,##0"},
                  flash=["bid", "ask", "last"], record=False)
    dark_table(quotes)
    bb = bars.BarBuilder(seconds=60, keep=500)
    fig = plot.figure("TXF from the CSV, 1-minute candles", rows=2, link_x=True, size=(960, 600), pos=(80, 200), theme="dark")
    fig.Dock("right", Ratio=0.6)  # beside the sheet: floating, it covered the cells the screen shows
    fig[0].set(time=True, follow=3600, title="TXF")
    fig[1].set(time=True, follow=3600, title="Volume")
    hb = safety.heartbeat("example playback", timeout=5)

    def on_rows(rows):  # every played row, with its time: TXF trades into 1-minute candles
        txf = rows[rows["symbol"] == "TXF"]
        for t, px, q in zip(txf["time"], txf["last"], txf["size"]):
            bb.add(t, float(px), float(q))
        if bb.all():
            with chart():
                bb.draw(fig[0], fig[1])
        hb.beat()

    player = Player(ticks, quotes, speed=speed, columns=cols, on_rows=on_rows).start()
    _state["player"] = player
    print(f"[example] playing {len(ticks):,} ticks at {speed:g}x from {path}")
    return player


def Workbook_Open():
    run()
