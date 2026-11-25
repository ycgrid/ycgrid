# SPDX-License-Identifier: Apache-2.0
"""Quote board: forty symbols streaming into a bound table whose columns YCGrid draws itself every frame - bid / ask
size bars, a two-sided change bar, volume heat and each row's recent prices as a mini trend line (simulated feed).

The drawing is the table's column renderers (BoundTable.set_render), so it stays fast at any update rate and works on
any table you bind: copy the render= line.

Open quote_board.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

import numpy as np
import pandas as pd

from yc import safety
from yc.data import bind

from ex_util import dark_table, fresh_sheet, thread, title

_state: dict = {}


def run(seconds: float = 0, symbols: int = 40):
    if "stop" in _state:
        _state["stop"].set()
    rng = np.random.default_rng(11)
    names = [f"S{i:02d}" for i in range(symbols)]
    names[:3] = ["TXF", "MXF", "TXO23000C"]
    px = rng.uniform(50, 1000, symbols).round(1)
    px[:2] = 23000
    base = px.copy()
    ws = fresh_sheet("Quote Board", trading=True)
    title(ws, "Quote board with column renderers: size bars, change bars, volume heat, trend lines (simulated)",
          "Bars, heat and trends are drawn by YCGrid over the cells every frame: BoundTable.set_render / bind(render=...).")
    df = pd.DataFrame({"Symbol": names, "BidSize": rng.integers(1, 200, symbols), "Bid": px - 0.5, "Ask": px + 0.5,
                       "AskSize": rng.integers(1, 200, symbols), "Last": px, "Chg": np.zeros(symbols), "Volume": np.zeros(symbols, int),
                       "Trend": px})
    t = bind(ws, df, key="Symbol", name="QuoteBoard", at="A4", flash=["Bid", "Ask", "Last"], record=False,
             formats={"Bid": "#,##0.0", "Ask": "#,##0.0", "Last": "#,##0.0", "Chg": "+0.00;-0.00;0.00", "Volume": "#,##0", "Trend": ";;;"},
             render={"BidSize": {"type": "bar", "color": "#ff3b3b"}, "AskSize": {"type": "bar", "color": "#00d25b"},
                     "Chg": {"type": "bar2", "color": "#ff3b3b", "neg": "#00d25b"}, "Volume": {"type": "heat", "color": "#ffd400", "neg": "#202020"},
                     "Trend": {"type": "trend", "length": 80, "color": "#00c8ff"}})
    dark_table(t)
    ws.Columns(9).ColumnWidth = 22  # the trend column
    stop = threading.Event()
    _state["stop"] = stop

    def feed():
        nonlocal px
        hb = safety.heartbeat("example quote board", timeout=3)
        vol = np.zeros(symbols, int)
        t_end = time.time() + seconds if seconds else None
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            k = rng.integers(0, symbols, 8)  # a few symbols tick each time
            px[k] = np.maximum(0.1, px[k] * (1 + rng.normal(0, 0.0015, len(k)))).round(1)
            vol[k] += rng.integers(1, 50, len(k))
            try:
                t.update({"Symbol": [names[i] for i in k], "BidSize": rng.integers(1, 200, len(k)), "Bid": px[k] - 0.5, "Ask": px[k] + 0.5,
                          "AskSize": rng.integers(1, 200, len(k)), "Last": px[k], "Chg": (px[k] / base[k] - 1) * 100, "Volume": vol[k],
                          "Trend": px[k]})
            except Exception:  # noqa: BLE001 - the workbook is closing
                break
            hb.beat()
            time.sleep(0.05)
        hb.stop()

    thread(feed, "example-quote-board")
    print("[example] the quote board is live")
    return t


def Workbook_Open():
    run()
