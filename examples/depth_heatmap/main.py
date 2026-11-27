# SPDX-License-Identifier: Apache-2.0
"""Market depth heatmap: resting liquidity by price over time, with the last price and the trades drawn on top.

A simulated futures book keeps its liquidity between snapshots: orders rest and slowly change, large orders sit at
round prices, and levels the price trades through are consumed. Every 0.25 s the book becomes a new column of the
heatmap (brighter = more lots resting at that price); the last price and the trades (dot size = trade size) are drawn
over it in the same price and time coordinates (heatmap extent), and the volume traded per snapshot is below.

Open depth_heatmap.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

import numpy as np

from yc import plot, safety, sim

from ex_util import T, chart, fresh_sheet, header, thread, title, write

_state: dict = {}


class _Book:
    """Resting size per price level on a fixed grid around the start price."""

    def __init__(self, center: float, tick: float, levels: int, rng):
        self.tick, self.rng = tick, rng
        self.prices = center + tick * np.arange(-levels, levels + 1)
        self.size = rng.gamma(2.0, 6.0, len(self.prices))
        walls = (np.round(self.prices) % 25 == 0)
        self.size[walls] += rng.uniform(120, 260, walls.sum())  # large orders at round prices
        self.walls = walls

    def step(self, last: float):
        rng = self.rng
        # orders come and go; large orders stay
        self.size = 0.94 * self.size + rng.gamma(1.2, 1.6, len(self.size))
        self.size[self.walls] = np.maximum(self.size[self.walls], 60) + rng.normal(0, 4, self.walls.sum())
        # liquidity thins out right at the touch, the price eats what it trades through
        near = np.abs(self.prices - last) <= self.tick * 0.5
        self.size[near] *= 0.35
        return np.clip(self.size, 0, None)


def run(seconds: float = 0, history: float = 60.0, tick: float = 1.0):
    """seconds: how long it runs (0 = until YCGrid closes or run() is called again); history: seconds on screen."""
    if "stop" in _state:
        _state["stop"].set()
    rng = np.random.default_rng(17)
    mkt = sim.Market(spot=23000, vol=0.25, tick=tick, seed=17)
    book = _Book(float(mkt.last), tick, 60, rng)
    dt = 0.25
    ncols = int(history / dt)
    ws = fresh_sheet("Depth", trading=True)
    title(ws, "Market depth heatmap (simulated TXF book)",
          "Brighter cells: more resting lots. Lines and dots: the last price and the trades, in the same coordinates.")
    header(write(ws, "A4", [["Last", "Best bid size", "Best ask size", "Largest bid wall", "Largest ask wall", "Snapshots"]]))
    fig = plot.figure("TXF market depth", rows=2, link_x=True, size=(980, 680), pos=(60, 150), theme="dark")
    fig.Dock("right", Ratio=0.6)  # beside the sheet: floating, it covered the cells the screen shows
    lo, hi = float(book.prices[0]) - tick / 2, float(book.prices[-1]) + tick / 2
    fig[0].set(title="Resting liquidity by price", time=True, ylabel="price", follow=history, ylim=(lo, hi))
    fig[1].set(title="Traded lots per snapshot", time=True, follow=history)
    z = np.full((len(book.prices), 1), np.nan)
    t0 = time.time()
    heat = fig[0].heatmap(z, cmap="hot", vmin=0, vmax=180, extent=(t0, t0 + dt, lo, hi), label="resting lots")
    last_item = fig[0].line([], [], label="last", color=T["cyan"], width=2, capacity=ncols * 4)
    trades_item = fig[0].scatter([], [], label="trades", color="#ffffff", size=3)
    vol_item = fig[1].bar([], [], label="traded", color=T["yellow"], width=dt * 0.8)
    stop = threading.Event()
    _state["stop"] = stop

    def loop():
        hb = safety.heartbeat("example depth", timeout=3)
        t_end = time.time() + seconds if seconds else None
        cols: list[np.ndarray] = []
        times: list[float] = []
        trade_t, trade_p, vol_t, vol_q = [], [], [], []
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            now = time.time()
            traded = 0
            for _ in range(5):  # a few trades between snapshots
                mkt.step(dt / 5)
                q = int(rng.integers(1, 15))
                traded += q
                trade_t.append(now)
                trade_p.append(float(mkt.last))
            last = float(mkt.last)
            sizes = book.step(last)
            cols.append(sizes[::-1].copy())  # row 0 = the highest price (top)
            times.append(now)
            if len(cols) > ncols:
                cols.pop(0)
                times.pop(0)
            vol_t.append(now)
            vol_q.append(traded)
            keep = now - history
            while trade_t and trade_t[0] < keep:
                trade_t.pop(0)
                trade_p.pop(0)
            while vol_t and vol_t[0] < keep:
                vol_t.pop(0)
                vol_q.pop(0)
            m = np.column_stack(cols)
            with chart():
                heat.set_data(m, extent=(times[0] - dt / 2, times[-1] + dt / 2, lo, hi))
                last_item.append([now], [last])
                trades_item.set_data(trade_t, trade_p)
                vol_item.set_data(vol_t, vol_q)
            bid_side = book.prices < last
            ask_side = ~bid_side
            ib = np.argmax(np.where(bid_side, sizes, -1))
            ia = np.argmax(np.where(ask_side, sizes, -1))
            nb = int(np.searchsorted(book.prices, last)) - 1
            ws.Range("A5:F5").Value = [[last, round(float(sizes[max(nb, 0)])), round(float(sizes[min(nb + 1, len(sizes) - 1)])),
                                        f"{book.prices[ib]:.0f} x {sizes[ib]:.0f}", f"{book.prices[ia]:.0f} x {sizes[ia]:.0f}", len(cols)]]
            hb.beat()
            time.sleep(max(0.0, dt - (time.time() - now)))
        hb.stop()

    thread(loop, "example-depth")
    print("[example] the depth heatmap is live")
    return fig


def Workbook_Open():
    run()
