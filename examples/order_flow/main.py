# SPDX-License-Identifier: Apache-2.0
"""Order flow: a live footprint chart (volume at each price of each bar, sells x buys), the session's volume profile
with its point of control, VWAP, and a limit order you drag on the chart (simulated TXF, paper exchange).

Built from yc.plot's building blocks only: rects (the footprint cells), barh (the profile), line with a tag (VWAP),
drag_line (the order: drag it to move the price, its x button cancels) and annotate (the fills) - copy it and change
what each bar or cell shows.

Open order_flow.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

import numpy as np

from yc import plot, safety, trading

from ex_util import T, chart, fresh_sheet, thread, title

_state: dict = {}
BAR = 5.0  # seconds per bar


def run(seconds: float = 0, bars: int = 24):
    """seconds: how long it runs (0 = until YCGrid closes or run() is called again); bars: how many bars stay on screen."""
    if "stop" in _state:
        _state["stop"].set()
        _state["gw"].close()
    gw = trading.PaperGateway(spot=23000, seed=21, step=0.05)
    om = trading.OrderManager(gw, risk=trading.RiskLimits(max_qty=5, max_position=10, price_band=0.01), require_operator=False)
    ws = fresh_sheet("Order Flow", trading=True)
    title(ws, "Order flow: footprint, volume profile, VWAP and a draggable limit order (paper)",
          "Drag the cyan line to move the order (it is re-sent at the new price), double-click the chart to place it there.")
    fig = plot.figure("TXF order flow", rows=1, cols=2, size=(1100, 640), pos=(60, 120), theme="dark")
    fig.Dock("right", Ratio=0.6)  # beside the sheet: floating, it covered the cells the screen shows
    fp, prof = fig[0], fig[1]
    fp.set(title="Footprint: sell x buy volume per price (red = buyers lead)", time=True, follow=BAR * bars, yfit="stable")
    prof.set(title="Volume profile", xlabel="lots")
    cells = fp.rects([], [], [], [], fmt="%.0f", edge="#202020", label="footprint")
    vwap = fp.line([], [], label="VWAP", color=T["yellow"], width=2, tag=True, fmt="%.1f")
    profile = prof.barh([], [], height=0.8, label="volume")
    poc = prof.drag_line(23000, label="POC", color=T["magenta"], width=1.5)  # moved by the code, not meant to be dragged
    fills = fp.annotate([], [], [], color="#3a3000", label="fills")
    stop = threading.Event()
    lock = threading.Lock()
    order = {"id": None, "price": 22990.0}

    def send(price: float):
        """Cancel the working order and send a limit buy at the new price (the drag's release)."""
        price = round(price)
        with lock:
            if order["id"]:
                om.cancel(order["id"])
            try:
                order["id"] = om.buy("TXF", 1, price, tag="drag")
                order["price"] = price
                line.set(price, label=f"BUY LMT 1 @ {price:,.0f}")
            except trading.Rejected as e:  # the old order is cancelled already: nothing works now
                order["id"] = None
                line.set(order["price"], label=f"refused, no order working: {e}")

    def cancel(_handle=None):
        """The line's x button: cancel the working order (double-click the chart to buy again)."""
        with lock:
            if order["id"]:
                om.cancel(order["id"])
                order["id"] = None
                line.set(order["price"], label="cancelled - double-click to buy")

    # fit=True: the working order stays in view (the footprint fits its prices, the order may sit below them)
    line = fp.drag_line(order["price"], label="BUY LMT 1", color=T["cyan"], width=2, on_release=lambda h: send(h.value),
                        on_close=cancel, fit=True)
    fig.on("dblclick", lambda e: send(e.y))
    send(order["price"])
    fill_pts: list = []

    def filled(f):
        fill_pts.append((f.time, f.price, f"{f.side} {f.qty} @ {f.price:,.0f}"))
        with lock:  # the line no longer stands for a working order: say so, or a trader waits for a fill already done
            if f.order_id == order["id"]:
                order["id"] = None
                line.set(order["price"], label=f"filled @ {f.price:,.0f} - drag or double-click to buy again")

    om.on_fill.append(filled)
    _state.update(stop=stop, gw=gw, om=om, fig=fig)

    def loop():
        hb = safety.heartbeat("example order flow", timeout=3)
        rng = np.random.default_rng(5)
        mkt = gw.market
        t_end = time.time() + seconds if seconds else None
        prev = mkt.last
        bar_cells: dict = {}  # (bar start, price) -> [sell, buy]
        pv = qv = 0.0
        vt, vv = [], []
        last_draw = 0.0
        prof_view = None  # the profile's limits, changed only when the prices or the volume outgrow them
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            now = time.time()
            px = float(mkt.last)
            q = int(rng.integers(1, 12))
            side = 1 if px > prev else 0 if px < prev else int(rng.integers(0, 2))  # up-tick: a buyer lifted the offer
            prev = px
            b0 = now - now % BAR
            bar_cells.setdefault((b0, px), [0, 0])[side] += q
            pv, qv = pv + px * q, qv + q
            if now - last_draw >= 0.25:
                last_draw = now
                keep = b0 - BAR * bars
                for k in [k for k in bar_cells if k[0] < keep]:
                    del bar_cells[k]
                keys = sorted(bar_cells)
                x0 = np.array([k[0] + BAR * 0.04 for k in keys])
                y = np.array([k[1] for k in keys])
                sell = np.array([bar_cells[k][0] for k in keys], dtype=float)
                buy = np.array([bar_cells[k][1] for k in keys], dtype=float)
                delta = buy - sell
                top = max(1.0, float(np.abs(delta).max()))
                colors = [("#%02x2020" % int(60 + 160 * d / top)) if d >= 0 else ("#20%02x40" % int(60 + 150 * -d / top)) for d in delta]
                labels = [f"{int(a)} x {int(b)}" for a, b in zip(sell, buy)]
                vt.append(now)
                vv.append(pv / qv)
                prices = sorted({k[1] for k in keys})
                tot = {p: 0.0 for p in prices}
                dlt = {p: 0.0 for p in prices}
                for k, a, b in zip(keys, sell, buy):
                    tot[k[1]] += a + b
                    dlt[k[1]] += b - a
                with chart():
                    cells.set_data(x0, x0 + BAR * 0.92, y - 0.5, y + 0.5, colors=colors, labels=labels)
                    vwap.set_data(vt[-2000:], vv[-2000:])
                    profile.set_data(prices, [tot[p] for p in prices], colors=[T["up"] if dlt[p] >= 0 else T["down"] for p in prices])
                    if prices:
                        poc.set(max(prices, key=lambda p: tot[p]))
                        top_vol = max(tot.values())
                        if prof_view is None or prices[0] < prof_view[0] or prices[-1] > prof_view[1] or top_vol > prof_view[2]:
                            prof_view = (prices[0] - 3, prices[-1] + 3, top_vol * 1.5)
                            prof.set(xlim=(0, prof_view[2]), ylim=(prof_view[0], prof_view[1]))
                    if fill_pts:
                        fills.set_data([f[0] for f in fill_pts[-20:]], [f[1] for f in fill_pts[-20:]], labels=[f[2] for f in fill_pts[-20:]])
                hb.beat()
            time.sleep(0.05)
        hb.stop()

    thread(loop, "example-order-flow")
    print("[example] order flow is live: drag the cyan line to move the order, double-click to place it")
    return fig


def Workbook_Open():
    run()
