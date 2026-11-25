# SPDX-License-Identifier: Apache-2.0
"""[YC] Ticks to bars: OHLC + volume bars by time, for live candlestick charts and strategies.

    from yc import bars, plot
    bb = bars.BarBuilder(seconds=5, keep=500)
    fig = plot.figure("TXF 5s"); candles = None
    for t, px, qty in ticks:                  # t: Unix seconds or a datetime
        closed = bb.add(t, px, qty)           # the bars completed by this tick (usually none or one)
    df = bb.frame()                           # the bars so far, the forming one last (pandas)
    bb.draw(fig[0], fig[1])                   # candles (+ volume) on yc.plot axes, the forming bar included
"""
from __future__ import annotations

import datetime as _dt
import math
from collections import deque
from dataclasses import dataclass
from typing import Optional

__all__ = ["Bar", "BarBuilder"]


@dataclass
class Bar:
    start: float      # Unix seconds of the bar's start
    open: float
    high: float
    low: float
    close: float
    volume: float = 0.0
    ticks: int = 0


def _secs(t) -> float:
    if isinstance(t, (int, float)):
        return float(t)
    if hasattr(t, "to_pydatetime"):  # pandas: a naive Timestamp is local wall-clock time, like a datetime (not UTC)
        t = t.to_pydatetime()
    if hasattr(t, "timestamp"):
        return t.timestamp()
    if hasattr(t, "astype"):  # numpy datetime64: a local wall-clock time, like yc.plot
        return _dt.datetime.fromisoformat(str(t.astype("datetime64[us]"))).timestamp()
    raise TypeError(f"not a time: {t!r}")


class BarBuilder:
    """Aggregates trades (time, price, size) into bars of `seconds`; keeps the last `keep` bars. Empty intervals make
    no bars (fill_gaps=True repeats the last close with zero volume instead)."""

    def __init__(self, seconds: float = 60.0, keep: int = 1000, fill_gaps: bool = False):
        if seconds <= 0:
            raise ValueError("seconds must be positive")
        self.seconds, self.fill_gaps = float(seconds), fill_gaps
        self.bars: deque[Bar] = deque(maxlen=keep)
        self.current: Optional[Bar] = None

    def add(self, t, price: float, size: float = 0.0) -> list:
        """Adds a trade; returns the bars it completed (oldest first)."""
        ts = _secs(t)
        start = math.floor(ts / self.seconds) * self.seconds
        done = []
        cur = self.current
        if cur is not None and start < cur.start:
            start = cur.start  # a late tick counts in the forming bar
        if cur is None or start > cur.start:
            if cur is not None:
                self.bars.append(cur)
                done.append(cur)
                if self.fill_gaps:
                    s = cur.start + self.seconds
                    while s < start:
                        gap = Bar(s, cur.close, cur.close, cur.close, cur.close)
                        self.bars.append(gap)
                        done.append(gap)
                        s += self.seconds
            self.current = cur = Bar(start, price, price, price, price)
        cur.high = max(cur.high, price)
        cur.low = min(cur.low, price)
        cur.close = price
        cur.volume += size
        cur.ticks += 1
        return done

    def all(self, include_forming: bool = True) -> list:
        out = list(self.bars)
        if include_forming and self.current is not None:
            out.append(self.current)
        return out

    def frame(self, include_forming: bool = True):
        """The bars as a pandas DataFrame (index: local start time; Open, High, Low, Close, Volume, Ticks)."""
        import pandas as pd

        b = self.all(include_forming)
        idx = pd.to_datetime([_dt.datetime.fromtimestamp(x.start) for x in b])
        return pd.DataFrame({"Open": [x.open for x in b], "High": [x.high for x in b], "Low": [x.low for x in b],
                             "Close": [x.close for x in b], "Volume": [x.volume for x in b], "Ticks": [x.ticks for x in b]}, index=idx)

    def draw(self, ax, volume_ax=None, label: str = "Price"):
        """Draws (or redraws) the bars as candles on a yc.plot Axes, with volume bars on a second Axes."""
        b = self.all()
        if not b:
            return
        t = [x.start for x in b]
        cols = ([x.open for x in b], [x.high for x in b], [x.low for x in b], [x.close for x in b])
        item = getattr(ax, "_yc_bars_item", None)
        if item is None:
            ax.set(time=True)
            ax._yc_bars_item = ax.candles(t, *cols, label=label)
        else:
            item.set_data(t, *cols)
        if volume_ax is not None:
            # volume in the candle's color: red on up bars, green on down bars (Taiwanese convention)
            dark = getattr(getattr(volume_ax, "fig", None), "theme", "light") in ("dark", "trading")
            up_c, dn_c = ("#ff3b3b", "#00d25b") if dark else ("#e5383b", "#16a34a")
            items = getattr(volume_ax, "_yc_bars_items", None)
            tu = [x.start for x in b if x.close >= x.open]
            vu = [x.volume for x in b if x.close >= x.open]
            td = [x.start for x in b if x.close < x.open]
            vd = [x.volume for x in b if x.close < x.open]
            if items is None:
                volume_ax.set(time=True)
                volume_ax._yc_bars_items = (volume_ax.bar(tu, vu, width=self.seconds * 0.8, label="Volume", color=up_c),
                                            volume_ax.bar(td, vd, width=self.seconds * 0.8, color=dn_c))
            else:
                items[0].set_data(tu, vu)
                items[1].set_data(td, vd)
