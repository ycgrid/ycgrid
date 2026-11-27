# SPDX-License-Identifier: Apache-2.0
"""Strategy lab: a parameter sweep, its heatmap, and an out-of-sample check against overfitting.

The moving-average crossover runs for every fast / slow pair on the first 70% of four years of simulated daily
futures (in sample); the heatmap shows the Sharpe ratio of each pair. The best pair is then run on the last 30% (out
of sample), which it has never seen: a large drop from in-sample to out-of-sample Sharpe is the sign of an
overfitted parameter choice. A walk-forward run re-chooses the pair every 125 days on the 250 days before and trades
only the days after each choice. The sweep and the walk-forward are plain loops over backtest_report's backtest;
YCGrid draws the heatmap, the equity curves and the indicators.

Open strategy_lab.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from yc import plot
from yc.data import write_frame

import ex_math as qm
from ex_util import T, fresh_sheet, header, title, write
from ex_backtest import crossover, positions_backtest, report, simulate_prices

FAST = range(5, 55, 5)
SLOW = range(20, 220, 20)
OPTS = dict(point_value=200.0, cost=1.0, capital=4_000_000.0)


def backtest(prices, fast, slow):
    equity, trades = positions_backtest(prices, crossover(prices, fast, slow), **OPTS)
    return equity, trades, qm.perf_stats(equity, trades=trades["PnL"])


def sweep(prices) -> pd.DataFrame:
    """Every fast < slow pair: its statistics, best Sharpe first."""
    rows = []
    for f in FAST:
        for s in SLOW:
            if f < s:
                st = backtest(prices, f, s)[2]
                rows.append({"fast": f, "slow": s, **{k: st.get(k) for k in ("Sharpe", "CAGR", "Max Drawdown", "Trades")}})
    return pd.DataFrame(rows).sort_values("Sharpe", ascending=False, na_position="last").reset_index(drop=True)


def walk_forward(prices, train: int = 250, test: int = 125):
    """Choose the pair on `train` days, trade the next `test` days with it, move on; the out-of-sample days only."""
    folds, pieces = [], []
    for start in range(0, len(prices) - train - 1, test):
        tr, te = prices.iloc[start:start + train], prices.iloc[start + train:start + train + test]
        if len(te) < 2:
            break
        best = sweep(tr).iloc[0]
        f, s = int(best["fast"]), int(best["slow"])
        # the signal needs the history before the test window: run on train + test, keep the test days
        eq, _, _ = backtest(prices.iloc[start:start + train + test], f, s)
        daily = eq.diff().iloc[train:]
        oos = qm.perf_stats(OPTS["capital"] + daily.cumsum())
        folds.append({"Fold": len(folds) + 1, "Test Start": te.index[0], "Test End": te.index[-1], "fast": f, "slow": s,
                      "IS Sharpe": best["Sharpe"], "OOS Sharpe": oos.get("Sharpe")})
        pieces.append(daily)
    pnl = pd.concat(pieces)
    return pd.DataFrame(folds), (OPTS["capital"] + pnl.cumsum()).rename("Equity")


def run(split: float = 0.7):
    prices = simulate_prices(days=1000, seed=11)
    cut = int(len(prices) * split)
    ins, oos = prices.iloc[:cut], prices.iloc[cut:]
    table = sweep(ins)
    best = table.iloc[0]
    f, s = int(best["fast"]), int(best["slow"])
    st_in, st_out = backtest(ins, f, s)[2], backtest(oos, f, s)[2]
    ws = fresh_sheet("Strategy Lab")
    title(ws, "Strategy lab: MA crossover sweep in sample, checked out of sample (simulated TXF)",
          f"In sample: {ins.index[0]:%Y-%m-%d} to {ins.index[-1]:%Y-%m-%d}; out of sample: {oos.index[0]:%Y-%m-%d} to {oos.index[-1]:%Y-%m-%d}.")
    header(write(ws, "A4", [["", "In sample", "Out of sample"]]))
    rows = [["Fast / slow", f"{f} / {s}", f"{f} / {s}"]]
    for k in ("Sharpe", "CAGR", "Max Drawdown", "Trades", "Win Rate"):
        rows.append([k, st_in.get(k), st_out.get(k)])
    write(ws, "A5", rows)
    ws.Range("B6:C6").NumberFormat = "0.00"
    ws.Range("B7:C8").NumberFormat = "0.0%"
    ws.Range("B10:C10").NumberFormat = "0.0%"
    ws.Columns(1).ColumnWidth = 16
    ws.Columns(2).ColumnWidth = 13
    ws.Columns(3).ColumnWidth = 14
    ws.Range("A12").Value = "All pairs, best Sharpe first (in sample)"
    ws.Range("A12").Font.Bold = True
    write_frame(ws, table, at="A13")
    ws.Range(f"C14:E{13 + len(table)}").NumberFormat = "0.00"
    # the heatmap: Sharpe by fast (across) and slow (down)
    grid = table.pivot(index="slow", columns="fast", values="Sharpe").reindex(index=list(SLOW), columns=list(FAST))
    lim = float(np.nanmax(np.abs(grid.to_numpy()))) or 1.0
    fig = plot.figure("In-sample Sharpe by fast (across) and slow (down)", size=(760, 560))
    fig[0].heatmap(np.nan_to_num(grid.to_numpy(), nan=0.0), xlabels=[str(x) for x in FAST], ylabels=[str(y) for y in SLOW],
                   cmap="rdbu", vmin=-lim, vmax=lim, fmt="%.2f")
    # walk-forward
    folds, wf_equity = walk_forward(prices)
    ws.Range("I4").Value = "Walk-forward: the pair re-chosen every 125 days on the 250 before"
    ws.Range("I4").Font.Bold = True
    write_frame(ws, folds, at="I5")
    ws.Range(f"N6:O{5 + len(folds)}").NumberFormat = "0.00"
    r = 7 + len(folds)
    wf_sharpe = qm.perf_stats(wf_equity).get("Sharpe")
    is_mean = float(folds["IS Sharpe"].mean())
    write(ws, f"I{r}", [["Out-of-sample Sharpe (all folds)", wf_sharpe],
                        ["Walk-forward efficiency (OOS / IS)", wf_sharpe / is_mean if is_mean else "n/a"]])
    ws.Range(f"J{r}:J{r + 1}").NumberFormat = "0.00"
    ws.Columns(9).ColumnWidth = 30
    wfig = plot.figure("Walk-forward equity (out of sample only)", size=(760, 360))
    wfig[0].line(wf_equity.index, wf_equity.values, label="walk-forward", color="#2e7d32", width=2)
    fixed, _, _ = backtest(prices.iloc[250:], f, s)
    wfig[0].line(fixed.index, fixed.values, label=f"fixed {f}/{s} (chosen with hindsight)", color="#9e9e9e")
    eq, trades, _ = backtest(prices, f, s)
    _, _, rfig = report(fresh_sheet("Lab Report"), eq, trades, prices, title=f"MA {f}/{s}: in and out of sample", name="LabTrades")
    ind = plot.figure(f"Indicators: SMA {f} / {s}, RSI 14", rows=2, link_x=True, size=(900, 560), theme="dark")
    ind[0].candles(prices.index, prices.Open, prices.High, prices.Low, prices.Close, label="Price")
    ind[0].line(prices.index, qm.sma(prices.Close, f), label=f"SMA {f}", color=T["yellow"])
    ind[0].line(prices.index, qm.sma(prices.Close, s), label=f"SMA {s}", color=T["magenta"])
    ind[0].vline([oos.index[0]], label="out of sample", color=T["cyan"], width=2)
    ind[1].line(prices.index, qm.rsi(prices.Close, 14), label="RSI 14", color=T["white"])
    ind[1].hline([30, 70], color="#5a5a5a")
    # docked beside the sheet, the Sharpe heatmap on top and the others as pages under it: floating, each window
    # opened over the one before and the report covered everything
    fig.Dock("right", Ratio=0.65)
    wfig.Dock("bottom", Ratio=0.5, Anchor=fig)
    ind.Dock("tab", Anchor=wfig)
    rfig.Dock("tab", Anchor=wfig)
    print(f"[example] best {f}/{s}: Sharpe {st_in.get('Sharpe', float('nan')):.2f} in sample, "
          f"{st_out.get('Sharpe', float('nan')):.2f} out of sample")
    ws.Activate()
    return table, fig


def Workbook_Open():
    run()
