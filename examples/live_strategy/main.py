# SPDX-License-Identifier: Apache-2.0
"""Research to trading: the moving-average rule of the backtest runs live on paper, unchanged.

The same crossover() the backtest uses decides on every completed 3-second bar of a simulated futures market; the
difference between its target and the position goes to the paper gateway through an OrderManager with risk limits.
The loop is a dozen lines (on_bar below): YCGrid supplies the bars, the bound Decisions table, the chart with buy /
sell marks (Axes.signals) and the position, and the safety pieces (risk limits, heartbeat, change log).

Open live_strategy.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

import numpy as np

from yc import bars, plot, safety, trading
from yc.data import bind

import ex_math as qm
from ex_util import T, chart, dark_table, fresh_sheet, header, thread, title, write
from ex_backtest import crossover

_state: dict = {}


def run(seconds: float = 0, bar_seconds: float = 3.0, fast: int = 5, slow: int = 15):
    """seconds: how long it runs (0 = until YCGrid closes or run() is called again)."""
    if "stop" in _state:
        _state["stop"].set()
        _state["gw"].close()
    gw = trading.PaperGateway(spot=23000, seed=31, step=0.05, latency=0.02)
    om = trading.OrderManager(gw, risk=trading.RiskLimits(max_qty=2, max_position=2, price_band=0.02), require_operator=False)
    bb = bars.BarBuilder(seconds=bar_seconds, keep=2000)
    ws = fresh_sheet("Live Strategy", trading=True)
    title(ws, f"The backtest's MA {fast}/{slow} rule, live on paper ({bar_seconds:g}-second bars)",
          "On each completed bar: target = crossover(bars); the difference to the position goes to the OrderManager.")
    header(write(ws, "A4", [["Last", "Position", "Avg cost", "Open P&L", "Bars", "Decisions"]]))
    decisions = bind(ws, None, key="Bar", name="LiveDecisions", at="A8",
                     columns=["Bar", "Time", "Close", "Target", "Position", "Order", "Note"],
                     formats={"Time": "hh:mm:ss", "Close": "#,##0"}, flash=["Target"], record=False)
    dark_table(decisions)
    fig = plot.figure(f"Live MA {fast}/{slow}", rows=2, link_x=True, size=(960, 640), pos=(80, 150), theme="dark")
    fig[0].set(time=True, follow=bar_seconds * 120, title="Candles, averages and decisions")
    fig[1].set(time=True, follow=bar_seconds * 120, title="Position (contracts)")
    f_item = fig[0].line([], [], label=f"SMA {fast}", color=T["yellow"], width=2)
    s_item = fig[0].line([], [], label=f"SMA {slow}", color=T["magenta"], width=2)
    marks = fig[0].signals([], [], [], label="orders", buy_color=T["up"], sell_color=T["down"])  # buy red, sell green
    pos_item = fig[1].step([], [], label="position", color=T["cyan"], width=2)
    stop = threading.Event()
    log: list[dict] = []
    _state.update(stop=stop, gw=gw, om=om, log=log)
    rng = np.random.default_rng(31)

    def on_bar():
        """A bar completed: the backtest's rule on the bars so far, and an order for the difference."""
        b = bb.frame(include_forming=False)
        if len(b) < slow:
            return
        target = int(crossover(b, fast, slow)[-1])
        q = om.position("TXF")
        row = {"Bar": len(b), "Time": b.index[-1] if hasattr(b.index[-1], "hour") else time.time() / 86400 + 25569,
               "Close": float(b.Close.iloc[-1]), "Target": target, "Position": q, "Order": "", "Note": ""}
        if target != q:
            try:
                om.submit("TXF", "buy" if target > q else "sell", abs(target - q), None, tag="MA")
                row["Order"] = f"{target - q:+d}"
            except trading.Rejected as e:
                row["Note"] = str(e)
        log.append({**row, "at": bb.all(include_forming=False)[-1].start + bar_seconds / 2, "low": float(b.Low.iloc[-1]),
                    "high": float(b.High.iloc[-1])})
        decisions.update({k: [v] for k, v in row.items()})

    def loop():
        hb = safety.heartbeat("example live strategy", timeout=3)
        t_end = time.time() + seconds if seconds else None
        last_draw = 0.0
        pos_t, pos_v = [], []
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            now = time.time()
            px = float(gw.market.last)
            if bb.add(now, px, float(rng.integers(1, 10))):
                on_bar()
            if now - last_draw >= 0.25:
                last_draw = now
                b = bb.frame(include_forming=False)
                q = om.position("TXF")
                pos_t.append(now)
                pos_v.append(q)
                with chart():
                    if len(b):
                        bb.draw(fig[0])
                        t = [x.start + bar_seconds / 2 for x in bb.all(include_forming=False)]
                        f_item.set_data(t, qm.sma(b.Close, fast).to_numpy())
                        s_item.set_data(t, qm.sma(b.Close, slow).to_numpy())
                        done = [x for x in log if x["Order"]]
                        buy = [x["Order"].startswith("+") for x in done]
                        marks.set_data([x["at"] for x in done], [x["low"] if bu else x["high"] for x, bu in zip(done, buy)],
                                       [1 if bu else -1 for bu in buy], labels=[x["Order"] for x in done])
                    pos_item.set_data(pos_t, pos_v)
                avg = om.avg_cost("TXF")
                ws.Range("A5:F5").Value = [[px, q, avg, (px - avg) * q * 200 if q and avg else 0.0, len(b), len(log)]]
                hb.beat()
            time.sleep(0.05)
        hb.stop()

    thread(loop, "example-live-strategy")
    print(f"[example] the MA {fast}/{slow} rule is trading on paper ({bar_seconds:g}-second bars)")
    return om


def Workbook_Open():
    run()
