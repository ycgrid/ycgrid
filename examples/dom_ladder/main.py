# SPDX-License-Identifier: Apache-2.0
"""Click-to-trade ladder: a depth-of-market ladder on a simulated futures book, trading through yc.trading.

The ladder is built from the pieces: a bound table keyed by price (My Bid | Bid | Price | Ask | My Ask) recentred
around the last trade, conditional formats for the last price's row and the prices above / below it, and click
actions on its columns (yc.interact.on_click). Left click a size in the Bid column to buy at that price, in the Ask
column to sell; click your own working order (My Bid / My Ask) to cancel it. Orders go through an OrderManager with
pre-trade risk limits (at most 5 lots an order, a position of 10, prices within 1% of the market) to a paper gateway
(yc.sim): nothing reaches a real market. The blotter follows OrderManager.on_order. The Ladder itself is in
../lib/ex_trading.py, shared with time_machine and txo_market_maker.

Open dom_ladder.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

from yc import excel, safety, trading
from yc.interact import on_click

from ex_trading import Ladder
from ex_util import blotter, fresh_sheet, in_host, positions, thread, title

_state: dict = {}


def run(seconds: float = 0):
    """seconds: how long the screen updates (0 = until YCGrid closes or run() is called again)."""
    if "stop" in _state:
        _state["stop"].set()
        _state["gw"].close()
    gw = trading.PaperGateway(spot=23000, seed=9, step=0.2)
    ws = fresh_sheet("Ladder", trading=True)
    title(ws, "Click-to-trade ladder (paper gateway, risk-checked)",
          "Click Bid sizes to buy, Ask sizes to sell, My Bid / My Ask to cancel. Limits: 5 lots an order, position 10, 1% band.")
    om = trading.OrderManager(gw, risk=trading.RiskLimits(max_qty=5, max_position=10, price_band=0.01), require_operator=False)
    pos = positions(ws, at="H4", name="LadderPos")  # one row, above the blotter: the blotter grows down
    blotter(ws, om, at="H8", name="LadderBlotter")
    lad = Ladder(ws, at="B5", name="Ladder")

    def price_of(ctx):
        px = (ctx.get("record") or {}).get("Price")
        return None if px is None else float(px)

    def trade(side):
        def h(ctx):
            px = price_of(ctx)
            if px is None:
                return
            try:
                om.submit("TXF", side, 1, px)
            except trading.Rejected as e:
                excel.Application.StatusBar = f"Order refused: {e}"
        return h

    def cancel(side):
        def h(ctx):
            px = price_of(ctx)
            for o in om.orders(open_only=True):
                if o.side == side and o.price == px:
                    om.cancel(o.id)
        return h

    if in_host():  # click actions run in the YCGrid host
        on_click(lad.column(1), trade("buy"))
        on_click(lad.column(3), trade("sell"))
        on_click(lad.column(0), cancel("buy"))
        on_click(lad.column(4), cancel("sell"))
    stop = threading.Event()
    _state.update(stop=stop, gw=gw, om=om)

    def loop():
        hb = safety.heartbeat("example ladder", timeout=3)
        t_end = time.time() + seconds if seconds else None
        mkt = gw.market
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            lad.update(float(mkt.last), *mkt.book(levels=12), *Ladder.working(om.orders(open_only=True)))
            q, avg = om.position("TXF"), om.avg_cost("TXF")
            pos.update({"Contract": ["TXF"], "Qty": [q], "Avg": [avg], "Last": [mkt.last],
                        "PnL": [(mkt.last - avg) * q * 200 if q and avg else 0.0]})
            hb.beat()
            time.sleep(0.2)
        hb.stop()

    thread(loop, "example-ladder")
    print("[example] the ladder is live: click Bid / Ask sizes to trade (paper gateway)")
    return om


def Workbook_Open():
    run()
