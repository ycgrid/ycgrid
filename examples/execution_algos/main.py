# SPDX-License-Identifier: Apache-2.0
"""Execution algorithms: a TWAP and an iceberg working orders on a simulated futures market, written in the loop.

The algorithms are a few lines each (Twap.step / Iceberg.step below): they send child orders through an OrderManager
with risk limits (10 lots an order at most) to a paper gateway (yc.sim), so nothing reaches a real market. YCGrid
shows them: a bound Algos table with each parent's progress, average price and slippage against the arrival price,
the blotter (OrderManager.on_order), and a chart with the price, every fill as a buy / sell mark and the filled
quantity against the schedule.

Open execution_algos.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

from yc import plot, safety, trading
from yc.data import bind

from ex_util import T, blotter, chart, dark_table, fresh_sheet, thread, title

_state: dict = {}


class Twap:
    """Buy or sell `lots` evenly over `seconds`: at each slice, a marketable child for what the schedule is behind by."""

    def __init__(self, om, side: str, lots: int, seconds: float, slices: int = 12):
        self.om, self.side, self.lots, self.seconds, self.slices = om, side, lots, seconds, slices
        self.t0 = time.time()
        self.name, self.children = "TWAP", set()

    def target(self, t: float) -> int:
        k = min(self.slices, int((t - self.t0) / (self.seconds / self.slices)) + 1)
        return round(self.lots * k / self.slices)

    def step(self, filled: int, last: float):
        behind = self.target(time.time()) - filled - self.working()
        if behind > 0:
            px = last + 2 if self.side == "buy" else last - 2  # marketable limit: crosses the spread, capped
            self.children.add(self.om.submit("TXF", self.side, min(behind, 10), px, tag=self.name))

    def working(self) -> int:
        return sum(o.qty - o.filled for o in self.om.orders(open_only=True) if o.id in self.children)


class Iceberg:
    """Sell (or buy) `lots` at a limit price, showing only `display` lots at a time."""

    def __init__(self, om, side: str, lots: int, price: float, display: int = 4):
        self.om, self.side, self.lots, self.price, self.display = om, side, lots, price, display
        self.name, self.children = "ICE", set()

    def target(self, t: float) -> int:
        return self.lots

    def step(self, filled: int, last: float):
        if self.working() == 0 and filled < self.lots:
            self.children.add(self.om.submit("TXF", self.side, min(self.display, self.lots - filled), self.price, tag=self.name))

    def working(self) -> int:
        return sum(o.qty - o.filled for o in self.om.orders(open_only=True) if o.id in self.children)


def run(seconds: float = 90.0, lots: int = 40):
    """seconds: the length of the TWAP schedule (the example stops a little after it); lots: the TWAP's size."""
    if "stop" in _state:
        _state["stop"].set()
        _state["gw"].close()
    gw = trading.PaperGateway(spot=23000, seed=21, step=0.1, latency=0.05)
    ws = fresh_sheet("Algos", trading=True)
    title(ws, "Execution algorithms on a paper market (TWAP and an iceberg, written in the example)",
          f"Each parent order is worked as child orders through risk limits; the TWAP runs {seconds:g} s.")
    om = trading.OrderManager(gw, risk=trading.RiskLimits(max_qty=10, price_band=0.02), require_operator=False)
    algos_table = bind(ws, None, key="Algo", name="Algos", at="A4", record=False,
                       columns=["Algo", "Side", "Lots", "Filled", "Schedule", "Avg price", "Arrival", "Slippage", "Working"],
                       formats={"Avg price": "#,##0.00", "Arrival": "#,##0", "Slippage": "[Red]0.00;[Green]-0.00;0.00"},
                       flash=["Filled"])
    dark_table(algos_table)
    blotter(ws, om, at="A10", name="AlgoBlotter")
    arrival = float(gw.market.last)
    algos = [Twap(om, "buy", lots, seconds), Iceberg(om, "sell", lots // 2, price=arrival + 1)]
    fills = {a.name: [] for a in algos}  # (time, price, qty)

    def on_fill(f):
        o = next((x for x in om.orders() if x.id == f.order_id), None)
        if o is not None and o.tag in fills:
            fills[o.tag].append((f.time, f.price, f.qty))

    om.on_fill.append(on_fill)
    colors = {"TWAP": T["cyan"], "ICE": T["orange"]}
    fig = plot.figure("Execution algorithms", rows=2, link_x=True, size=(900, 620), pos=(60, 150), theme="dark")
    fig.Dock("right", Ratio=0.6)  # beside the sheet: floating, it covered the cells the screen shows
    fig[0].set(title="TXF and the fills", time=True, follow=seconds * 1.2)
    fig[1].set(title="Filled lots against the schedule", time=True, ylabel="lots", follow=seconds * 1.2)
    price_item = fig[0].line([], [], label="last", color=T["white"], capacity=20000)
    marks = fig[0].signals([], [], [], label="fills", buy_color=T["up"], sell_color=T["down"], size=6)
    done_items = {a.name: fig[1].step([], [], label=f"{a.name} filled", color=colors[a.name], width=2) for a in algos}
    sched = fig[1].line([], [], label="TWAP schedule", color=T["cyan"], width=1)
    stop = threading.Event()
    _state.update(stop=stop, gw=gw, om=om)

    def loop():
        hb = safety.heartbeat("example algos", timeout=3)
        t0 = time.time()
        st = [t0 + seconds * i / 120 for i in range(121)]
        sched.set_data(st, [algos[0].target(t) for t in st])
        history = {a.name: ([t0], [0]) for a in algos}
        while not stop.is_set() and time.time() < t0 + seconds * 1.15:
            now, last = time.time(), float(gw.market.last)
            rows = {k: [] for k in ("Algo", "Side", "Lots", "Filled", "Schedule", "Avg price", "Arrival", "Slippage", "Working")}
            for a in algos:
                got = fills[a.name]
                filled = int(sum(q for _, _, q in got))
                try:
                    a.step(filled, last)
                except trading.Rejected:
                    pass
                avg = sum(p * q for _, p, q in got) / filled if filled else None
                slip = None if avg is None else (avg - arrival) * (1 if a.side == "buy" else -1)
                for k, v in (("Algo", a.name), ("Side", a.side), ("Lots", a.lots), ("Filled", filled),
                             ("Schedule", min(a.target(now), a.lots)), ("Avg price", avg), ("Arrival", arrival),
                             ("Slippage", slip), ("Working", a.working())):
                    rows[k].append(v)
                history[a.name][0].append(now)
                history[a.name][1].append(filled)
            algos_table.update(rows)
            with chart():
                price_item.append([now], [last])
                pts = [(t, p, a.side) for a in algos for t, p, _ in fills[a.name]]
                if pts:
                    marks.set_data([t for t, _, _ in pts], [p for _, p, _ in pts], [1 if s == "buy" else -1 for _, _, s in pts])
                for a in algos:
                    done_items[a.name].set_data(*history[a.name])
            hb.beat()
            time.sleep(0.25)
        om.cancel_all()
        with chart():
            fig.fit()
        hb.stop()
        print("[example] algos finished:", ", ".join(f"{a.name} {int(sum(q for _, _, q in fills[a.name]))}/{a.lots}" for a in algos))

    thread(loop, "example-algos")
    print("[example] a TWAP and an iceberg are working orders on the paper market")
    return om


def Workbook_Open():
    run()
