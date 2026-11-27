# SPDX-License-Identifier: Apache-2.0
"""Live candles: simulated ticks aggregated into 5-second bars, drawn live with an EMA, volume and RSI.

A market simulator (yc.sim) produces trades in a background thread; yc.bars builds the bars, the forming one
included; yc.plot redraws four times a second and follows the last two minutes; pandas computes the indicators on the
bars. The sheet shows the last trade and the current bar, updated in place.

Open live_candles.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

import numpy as np

from yc import bars, plot, safety, sim

import ex_math as qm
from ex_util import T, chart, fresh_sheet, header, thread, title, write

_state: dict = {}


def run(seconds: float = 0, bar_seconds: float = 5.0):
    """seconds: how long it runs (0 = until YCGrid closes or run() is called again)."""
    if "stop" in _state:
        _state["stop"].set()
    mkt = sim.Market(spot=23000, seed=4)
    bb = bars.BarBuilder(seconds=bar_seconds, keep=2000)
    ws = fresh_sheet("Live Candles", trading=True)
    title(ws, f"Live candles: ticks into {bar_seconds:g}-second bars (simulated TXF)", "The chart follows the last two minutes.")
    header(write(ws, "A4", [["Last", "Time", "Bar open", "Bar high", "Bar low", "Bar volume", "Ticks"]]))
    ws.Range("B5").NumberFormat = "hh:mm:ss"
    fig = plot.figure("TXF live candles", rows=3, link_x=True, size=(900, 640), pos=(80, 160), theme="dark")
    fig.Dock("right", Ratio=0.6)  # beside the sheet: floating, it covered the cells the screen shows
    for ax in fig._axes:
        ax.set(follow=120, time=True)
    ema_item = fig[0].line([], [], label="EMA 12", color=T["yellow"], width=2)
    rsi_item = fig[2].line([], [], label="RSI 14", color=T["magenta"], width=1.5)
    fig[2].hline([30, 70], color="#5a5a5a")
    stop = threading.Event()
    _state["stop"] = stop
    rng = np.random.default_rng(7)

    def loop():
        hb = safety.heartbeat("example candles", timeout=3)
        t_end = time.time() + seconds if seconds else None
        last_draw = 0.0
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            mkt.step(0.05)
            now = time.time()
            bb.add(now, float(mkt.last), float(rng.integers(1, 20)))
            if now - last_draw >= 0.25:
                last_draw = now
                with chart():
                    bb.draw(fig[0], fig[1])
                    df = bb.frame()
                    t = [x.start for x in bb.all()]
                    ema_item.set_data(t, qm.ema(df.Close, 12).to_numpy())
                    rsi_item.set_data(t, qm.rsi(df.Close, 14).to_numpy())
                c = bb.current
                ws.Range("A5:G5").Value = [[float(mkt.last), now / 86400 + 25569 + _tz_days(), c.open, c.high, c.low, c.volume, c.ticks]]
                hb.beat()
            time.sleep(0.05)
        hb.stop()

    thread(loop, "example-candles")
    print("[example] live candles are running")
    return bb, fig


def _tz_days() -> float:
    """The local time zone's offset in days (sheet times are local wall-clock times)."""
    return -time.timezone / 86400 if not time.localtime().tm_isdst else -time.altzone / 86400


def Workbook_Open():
    run()
