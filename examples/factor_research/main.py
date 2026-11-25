# SPDX-License-Identifier: Apache-2.0
"""Factor research: does a cross-sectional score predict next month's returns?

300 simulated stocks over ten years; the score is persistent and carries a small true edge (an information
coefficient near 0.05). Each month the stocks are sorted into quintiles by score; the example measures the quintile
returns, the rank IC and its stability, turnover and decay (a few lines of numpy below), writes them to a bound table,
runs the Analysis ToolPak's Descriptive Statistics on the IC, and plots the cumulative quintile returns, the IC and
its distribution.

Open factor_research.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import math

import numpy as np
import pandas as pd

from yc import analysis, plot
from yc.data import bind

from ex_util import fresh_sheet, header, title, write


def simulate(n_stocks: int = 300, months: int = 120, edge: float = 0.0045, seed: int = 11):
    rng = np.random.default_rng(seed)
    score = np.empty((months, n_stocks))
    score[0] = rng.standard_normal(n_stocks)
    for t in range(1, months):
        score[t] = 0.8 * score[t - 1] + 0.6 * rng.standard_normal(n_stocks)
    market = rng.normal(0.006, 0.045, months)
    beta = rng.uniform(0.6, 1.4, n_stocks)
    ret = np.empty((months, n_stocks))
    ret[0] = np.nan
    for t in range(1, months):  # next month's return depends on this month's score
        ret[t] = beta * market[t] + edge * score[t - 1] / score[t - 1].std() + rng.normal(0, 0.08, n_stocks)
    dates = pd.date_range("2016-01-31", periods=months, freq="ME")
    return dates, score, ret


def ranks(x):
    r = np.empty_like(x)
    r[np.argsort(x)] = np.arange(len(x))
    return r


def rank_ic(score, ret, h: int = 1) -> np.ndarray:
    """Spearman correlation of each month's score with the return h months later (NaN where there is none)."""
    out = np.full(len(score), np.nan)
    for t in range(len(score) - h):
        out[t] = np.corrcoef(ranks(score[t]), ranks(ret[t + h]))[0, 1]
    return out


def quintiles(score, ret, q: int = 5):
    """Equal-weighted returns of the q score buckets (month t's score, month t + 1's return), the top-minus-bottom
    spread, and the share of the top bucket that changes each month."""
    rows, turnover, prev = [], [], None
    for t in range(len(score) - 1):
        order = np.argsort(score[t])
        buckets = np.array_split(order, q)
        r = [float(np.nanmean(ret[t + 1][b])) for b in buckets]
        rows.append(r + [r[-1] - r[0]])
        top = set(buckets[-1].tolist())
        if prev is not None:
            turnover.append(1 - len(top & prev) / max(len(top), 1))
        prev = top
    cols = [f"Q{i + 1}" for i in range(q)] + [f"Q{q}-Q1"]
    return pd.DataFrame(rows, columns=cols), float(np.mean(turnover)) if turnover else float("nan")


def run():
    dates, score, ret = simulate()
    # the score of month t against the return of month t + 1
    df, turnover = quintiles(score, ret)
    df.insert(0, "Month", dates[1:])  # labelled by the month the return was earned
    df["IC"] = rank_ic(score, ret)[:-1]
    decay = {h: float(np.nanmean(rank_ic(score, ret, h))) for h in range(1, 13)}
    ic = df["IC"].to_numpy()
    ls = df["Q5-Q1"].to_numpy()
    ws = fresh_sheet("Factor")
    title(ws, "Factor research: quintile portfolios and information coefficient (simulated, 300 stocks)",
          "Each month: sort by the score, hold equal-weighted quintiles for a month. IC = rank correlation of score and next return.")
    summary = [
        ["Mean IC", float(ic.mean())],
        ["IC std", float(ic.std(ddof=1))],
        ["ICIR (annual)", float(ic.mean() / ic.std(ddof=1) * math.sqrt(12))],
        ["t-stat of IC", float(ic.mean() / ic.std(ddof=1) * math.sqrt(len(ic)))],
        ["IC > 0 (months)", float((ic > 0).mean())],
        ["Q5-Q1 annual return", float((1 + ls).prod() ** (12 / len(ls)) - 1)],
        ["Q5-Q1 annual Sharpe", float(ls.mean() / ls.std(ddof=1) * math.sqrt(12))],
        ["Top quintile turnover", turnover],
        ["IC 6 months ahead", float(decay[6])],
    ]
    header(write(ws, "A4", [["Statistic", "Value"]]))
    write(ws, "A5", summary)
    ws.Range("B5:B7").NumberFormat = "0.000"
    ws.Range("B8").NumberFormat = "0.00"
    ws.Range("B9:B10").NumberFormat = "0.0%"
    ws.Range("B11").NumberFormat = "0.00"
    ws.Range("B12").NumberFormat = "0.0%"
    ws.Range("B13").NumberFormat = "0.000"
    ws.Columns(1).ColumnWidth = 22
    ws.Range("A15").Value = "Monthly quintile returns"
    ws.Range("A15").Font.Bold = True
    t = bind(ws, df, key="Month", name="Factor", at="A16", record=False,
             formats={"Month": "yyyy-mm", **{c: "0.00%" for c in ["Q1", "Q2", "Q3", "Q4", "Q5", "Q5-Q1"]}, "IC": "0.000"})
    # the Analysis ToolPak on the IC column, next to the summary
    ic_col = t.list_object.ListColumns("IC").Range
    ws.Range("K4").Value = "Analysis ToolPak: Descriptive Statistics of the IC"
    ws.Range("K4").Font.Bold = True
    analysis.Descr(ic_col, ws.Range("K5"), "C", True, True, 95)
    # charts
    fig = plot.figure("Factor research", rows=2, cols=2, size=(1200, 760))
    cum = (1 + df[["Q1", "Q2", "Q3", "Q4", "Q5"]]).cumprod()
    colors = ["#c62828", "#ef6c00", "#9e9e9e", "#43a047", "#1b5e20"]
    for q, col in zip(["Q1", "Q2", "Q3", "Q4", "Q5"], colors):
        fig[0].line(df["Month"], cum[q], label=q, color=col, width=2 if q in ("Q1", "Q5") else 1.2)
    fig[0].set(title="Growth of 1 by quintile", ylabel="x")
    fig[1].bar(df["Month"], df["IC"], width=20 * 86400, label="IC", color="#90a4ae")
    fig[1].line(df["Month"], df["IC"].rolling(12).mean(), label="12-month mean", color="#1565c0", width=2)
    fig[1].hline([0], color="#000000")
    fig[1].set(title="Rank IC by month")
    fig[2].hist(ic, bins=24, label="IC", color="#5c6bc0")
    fig[2].vline([float(ic.mean())], label="mean", color="#c62828")
    fig[2].set(title="IC distribution")
    ann = [(1 + df[q]).prod() ** (12 / len(df)) - 1 for q in ["Q1", "Q2", "Q3", "Q4", "Q5"]]
    fig[3].bar([1, 2, 3, 4, 5], np.array(ann) * 100, label="Annual return %", color="#26a69a")
    fig[3].set(title="Annual return by quintile (%)", xlabel="quintile")
    print(f"[example] mean IC {ic.mean():.3f}, ICIR {summary[2][1]:.2f}, Q5-Q1 {summary[5][1]:.1%} a year")
    return t, fig


def Workbook_Open():
    run()
