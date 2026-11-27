# SPDX-License-Identifier: Apache-2.0
"""The examples' backtest: simulated daily futures prices, a positions backtest, a moving-average crossover, and
the report (statistics, monthly returns, the trade list as a bound table, and a chart window linked to it), made of
YCGrid's building blocks. Used by backtest_report, strategy_lab, pairs_trading and live_strategy.
"""
from __future__ import annotations

import math

import numpy as np
import pandas as pd

from yc import excel, plot
from yc.data import bind

import ex_math as qm
from ex_util import header, rgb, write

_MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
_PCT = {"Total Return", "CAGR", "Annual Volatility", "Max Drawdown", "Best Period", "Worst Period", "Win Rate"}


def simulate_prices(days: int = 756, start: float = 17000.0, seed: int = 3) -> pd.DataFrame:
    """Daily OHLC of a trending, volatile index future (geometric Brownian motion with regimes)."""
    rng = np.random.default_rng(seed)
    idx = pd.bdate_range("2023-01-02", periods=days)
    drift = np.repeat(rng.normal(0.0004, 0.0012, days // 63 + 1), 63)[:days]  # quarterly regimes
    ret = drift + rng.standard_normal(days) * 0.012
    close = start * np.exp(np.cumsum(ret))
    open_ = np.r_[start, close[:-1]] * (1 + rng.standard_normal(days) * 0.002)
    high = np.maximum(open_, close) * (1 + np.abs(rng.standard_normal(days)) * 0.004)
    low = np.minimum(open_, close) * (1 - np.abs(rng.standard_normal(days)) * 0.004)
    return pd.DataFrame({"Open": open_, "High": high, "Low": low, "Close": close}, index=idx)


def positions_backtest(prices: pd.DataFrame, target, point_value: float = 200.0, cost: float = 1.0, capital: float = 4_000_000.0):
    """Equity and trades of a target position per bar (decided at the close, traded at the next open)."""
    target = pd.Series(np.asarray(target, dtype=float), index=prices.index).fillna(0)
    pos = target.shift(1).fillna(0)  # held from the next bar
    o = prices["Open"]
    pnl = pos * (o.shift(-1).fillna(prices["Close"]) - o) * point_value
    costs = pos.diff().abs().fillna(pos.abs()) * cost * point_value
    equity = (capital + (pnl - costs).cumsum()).rename("Equity")
    trades, entry, held = [], None, 0.0
    for t, p in enumerate(pos):
        if p == held:
            continue
        px = float(o.iloc[t])
        qty = abs(p - held)
        side = "Buy" if p > held else "Sell"
        closed = None
        if held != 0 and entry is not None and (p == 0 or np.sign(p) != np.sign(held)):
            closed = (px - entry) * np.sign(held) * abs(held) * point_value - 2 * cost * abs(held) * point_value
        trades.append({"Time": prices.index[t], "Side": side, "Qty": qty, "Price": px, "PnL": closed})
        entry = px if p != 0 and (held == 0 or np.sign(p) != np.sign(held)) else entry
        held = p
    return equity, pd.DataFrame(trades, columns=["Time", "Side", "Qty", "Price", "PnL"])


def crossover(prices: pd.DataFrame, fast: int, slow: int) -> np.ndarray:
    """Long (1) when the fast average is above the slow one, flat (0) otherwise."""
    if fast >= slow:
        return np.zeros(len(prices))
    c = prices["Close"]
    return (qm.sma(c, fast) > qm.sma(c, slow)).astype(int).to_numpy()


def _cell(v):
    return None if isinstance(v, float) and not math.isfinite(v) else v


def report(ws, equity, trades=None, prices=None, benchmark=None, title: str = "Backtest", name: str = "Trades",
           periods_per_year: float = 252):
    """The report on ws: statistics (A3), monthly returns (E3), the trade list (a bound table under them) and a linked
    chart window. Returns (stats, table, figure)."""
    pnl = trades["PnL"] if trades is not None and "PnL" in trades else None
    stats = qm.perf_stats(equity, periods_per_year, trades=pnl)
    ws.Range("A1").Value = title
    ws.Range("A1").Font.Bold = True
    ws.Range("A1").Font.Size = 14
    header(write(ws, "A3", [["Metric", "Value"]]))
    rows = [[k, _cell(v)] for k, v in stats.items()]
    write(ws, "A4", rows)
    for i, (k, _) in enumerate(rows):
        ws.Cells(4 + i, 2).NumberFormat = ("0.00%" if k in _PCT else "yyyy-mm-dd" if k in ("Start", "End") else
                                           "#,##0" if k in ("Trades", "Max Drawdown Length") else "#,##0.00")
    ws.Columns(1).ColumnWidth = 20
    ws.Columns(2).ColumnWidth = 14
    # monthly returns: years x months with a color scale
    mrows = 0
    if getattr(equity.index, "dtype", None) is not None and equity.index.dtype.kind == "M":
        m = qm.monthly_returns(equity)
        header(write(ws, "E3", [["Year"] + _MONTHS + ["Year"]]))
        body = [[int(y)] + [_cell(float(x)) if pd.notna(x) else None for x in row] for y, row in zip(m.index, m.values)]
        mrows = len(body)
        if body:
            write(ws, "E4", body)
            vals = ws.Range(ws.Cells(4, 6), ws.Cells(3 + mrows, 18))
            vals.NumberFormat = "0.0%"
            cs = vals.FormatConditions.AddColorScale(3)
            cs.ColorScaleCriteria(1).FormatColor.Color = rgb("#f8696b")
            cs.ColorScaleCriteria(2).FormatColor.Color = rgb("#ffffff")
            cs.ColorScaleCriteria(3).FormatColor.Color = rgb("#63be7b")
    # the trade list: a bound table (formulas and filters can use it)
    table = None
    tr = 6 + max(len(rows), mrows)
    if trades is not None and len(trades):
        t = trades.copy()
        if "PnL" in t.columns:
            t["Cum PnL"] = t["PnL"].fillna(0).cumsum()
        t.insert(0, "#", range(1, len(t) + 1))
        ws.Cells(tr - 1, 1).Value = "Trades"
        ws.Cells(tr - 1, 1).Font.Bold = True
        table = bind(ws, t.reset_index(drop=True), key="#", name=name, at=ws.Cells(tr, 1), record=False,
                     formats={"Time": "yyyy-mm-dd", "Price": "#,##0.00", "PnL": "#,##0;[Red]-#,##0", "Cum PnL": "#,##0;[Red]-#,##0"})
        if "PnL" in t.columns:
            fc = table.list_object.ListColumns("PnL").DataBodyRange.FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "0")
            fc.Font.Color = rgb("#2e7d32")
    # the charts: price with buy / sell marks, equity, drawdown; linked to the trade list
    n = 3 if prices is not None else 2
    fig = plot.figure(title, rows=n, link_x=True, size=(980, 720 if n == 3 else 520))
    fig.Dock("right", Ratio=0.6)  # beside the sheet: floating, it covered the cells the screen shows
    i = 0
    if prices is not None:
        ax = fig[0]
        ax.candles(prices.index, prices["Open"], prices["High"], prices["Low"], prices["Close"], label="Price")
        if trades is not None and len(trades):
            at = pd.DatetimeIndex(trades["Time"])
            bar = prices.reindex(at, method="nearest")
            buy = trades["Side"].str.startswith("B").to_numpy()
            # marks just under the bar's low for buys, over its high for sells: never on the candles
            price = np.where(buy, bar["Low"].to_numpy(), bar["High"].to_numpy())
            ax.signals(at, price, np.where(buy, 1, -1), labels=[f"{'B' if b else 'S'} {q:g}" for b, q in zip(buy, trades["Qty"])],
                       label="trades")
        ax.set(title="Price and trades")
        i = 1
    fig[i].line(equity.index, equity, label="Equity", color="#1565c0", width=2)
    if benchmark is not None:
        fig[i].line(benchmark.index, benchmark, label="Benchmark", color="#7f7f7f")
    fig[i].set(title="Equity")
    dd = qm.drawdown(equity)
    fig[i + 1].fill_between(dd.index, dd, label="Drawdown", color="#c62828")
    fig[i + 1].set(title="Drawdown")
    if table is not None:
        fig.link_table(name, "Time")
    return stats, table, fig
