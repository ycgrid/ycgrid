# SPDX-License-Identifier: Apache-2.0
"""Pairs trading: a hedge ratio by regression, the spread's z-score and a mean-reversion backtest.

Two simulated stocks share a common trend; their spread mean-reverts. The Analysis ToolPak's Regression (the same
call as Excel's Application.Run "ATPVBAEN.XLAM!Regress") estimates the hedge ratio on the sheet; the strategy sells
the spread when its z-score is above `entry`, buys it below -`entry` and exits inside +-`exit`. The report and the
z-score chart follow.

Open pairs_trading.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from yc import excel, plot

from ex_backtest import report
from ex_util import fresh_sheet, header, title, write


def simulate(days: int = 750, seed: int = 4):
    rng = np.random.default_rng(seed)
    idx = pd.bdate_range("2023-01-02", periods=days)
    common = np.cumsum(rng.normal(0.0003, 0.012, days))
    spread = np.zeros(days)
    for t in range(1, days):  # Ornstein-Uhlenbeck: half-life about 10 days
        spread[t] = spread[t - 1] * 0.93 + rng.normal(0, 0.012)
    x = 100 * np.exp(common + rng.normal(0, 0.002, days))
    y = 0.85 * x + 12 + 100 * spread
    return pd.DataFrame({"Date": idx, "Y": y, "X": x})


def half_life(x) -> float:
    """Mean-reversion half-life in bars, from the regression of x's change on its level (inf if it doesn't revert)."""
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    b = np.polyfit(x[:-1], np.diff(x), 1)[0]
    return float(-np.log(2) / np.log1p(b)) if -1 < b < 0 else float("inf")


def run(entry: float = 2.0, exit: float = 0.5, lookback: int = 60, units: float = 1000.0):  # noqa: A002
    df = simulate()
    ws = fresh_sheet("Pairs")
    title(ws, "Pairs trading: hedge ratio by regression (Analysis ToolPak), z-score signals",
          f"Entry |z| > {entry}, exit |z| < {exit}, lookback {lookback} days; {units:,.0f} units of Y per spread.")
    header(write(ws, "A4", [["Date", "Y", "X"]]))
    write(ws, "A5", [[d.to_pydatetime(), float(y), float(x)] for d, y, x in zip(df["Date"], df["Y"], df["X"])])
    n = len(df)
    ws.Range(f"A5:A{4 + n}").NumberFormat = "yyyy-mm-dd"
    ws.Range(f"B5:C{4 + n}").NumberFormat = "0.00"
    ws.Columns(1).ColumnWidth = 11
    # Excel's ToolPak call, unchanged
    out = excel.Application.Run("ATPVBAEN.XLAM!Regress", ws.Range(f"B4:B{4 + n}"), ws.Range(f"C4:C{4 + n}"), False, True, None,
                                ws.Range("E4"))
    beta = float(out.Cells(18, 2).Value)
    alpha = float(out.Cells(17, 2).Value)
    ws.Range("E30").Value = "Hedge ratio (X coefficient)"
    ws.Range("F30").Value = beta
    ws.Range("E30:F30").Font.Bold = True
    spread = df["Y"] - beta * df["X"] - alpha
    hl = half_life(spread)
    write(ws, "E32", [["Half-life of the spread (days)", hl]])
    ws.Range("F32").NumberFormat = "0.0"
    z = (spread - spread.rolling(lookback).mean()) / spread.rolling(lookback).std()
    pos = np.zeros(n)
    for t in range(1, n):
        p = pos[t - 1]
        if p == 0 and z.iloc[t] > entry:
            p = -1
        elif p == 0 and z.iloc[t] < -entry:
            p = 1
        elif p != 0 and abs(z.iloc[t]) < exit:
            p = 0
        pos[t] = p
    held = np.r_[0, pos[:-1]]  # decided at the close, held from the next day
    pnl = held * spread.diff().fillna(0).to_numpy() * units
    equity = pd.Series(1_000_000 + np.cumsum(pnl), index=pd.DatetimeIndex(df["Date"]), name="Equity")
    trades, open_at = [], None
    for t in range(1, n):
        if pos[t] != pos[t - 1]:
            if pos[t - 1] != 0 and open_at is not None:
                trades.append({"Time": df["Date"].iloc[t], "Side": "Buy" if pos[t - 1] < 0 else "Sell", "Qty": units,
                               "Price": float(spread.iloc[t]), "PnL": float((spread.iloc[t] - spread.iloc[open_at]) * pos[t - 1] * units)})
                open_at = None
            if pos[t] != 0:
                trades.append({"Time": df["Date"].iloc[t], "Side": "Buy" if pos[t] > 0 else "Sell", "Qty": units,
                               "Price": float(spread.iloc[t]), "PnL": None})
                open_at = t
    stats, _, _ = report(fresh_sheet("Pairs Report"), equity, pd.DataFrame(trades, columns=["Time", "Side", "Qty", "Price", "PnL"]),
                         title="Pairs trading: spread mean reversion", name="PairTrades")
    fig = plot.figure("Spread z-score", rows=2, link_x=True, size=(980, 520))
    fig[0].line(df["Date"], df["Y"], label="Y", color="#1565c0")
    fig[0].line(df["Date"], beta * df["X"] + alpha, label="beta X + alpha", color="#ef6c00")
    fig[0].set(title=f"Prices (hedge ratio {beta:.3f})")
    fig[1].line(df["Date"], z, label="z-score", color="#37474f")
    fig[1].hline([entry, -entry], label="entry", color="#c62828")
    fig[1].hline([exit, -exit], label="exit", color="#2e7d32")
    fig[1].set(title="Spread z-score")
    print(f"[example] hedge ratio {beta:.3f}; half-life {hl:.1f} days; Sharpe {stats.get('Sharpe', float('nan')):.2f}, "
          f"{len(trades)} trades")
    return stats, fig


def Workbook_Open():
    run()
