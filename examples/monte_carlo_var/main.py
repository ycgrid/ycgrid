# SPDX-License-Identifier: Apache-2.0
"""Monte Carlo VaR: 100,000 correlated one-day scenarios of a multi-asset portfolio.

Correlated normal shocks with fat tails (a Student-t mixture) reprice the positions; the sheet shows the positions,
the VaR and expected shortfall at 95% and 99% next to the parametric (normal) VaR, and the worst scenarios; the chart
shows the P&L distribution with the VaR lines.

Open monte_carlo_var.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import numpy as np

from yc import plot

from ex_util import fresh_sheet, header, title, write

POSITIONS = [  # name, market value, daily vol
    ("TAIEX futures", 12_000_000, 0.013),
    ("TSMC", 8_000_000, 0.018),
    ("US equity ETF", 6_000_000, 0.011),
    ("Gov bonds", 10_000_000, 0.003),
    ("USD/TWD", -5_000_000, 0.004),
    ("Gold", 3_000_000, 0.010),
]


def cornish_fisher_var(mean: float, std: float, conf: float, skew: float, kurt: float) -> float:
    """The normal VaR with its quantile corrected for skewness and excess kurtosis (Cornish-Fisher)."""
    z = {0.95: -1.6449, 0.99: -2.3263}[conf]
    zc = z + (z * z - 1) * skew / 6 + (z ** 3 - 3 * z) * kurt / 24 - (2 * z ** 3 - 5 * z) * skew * skew / 36
    return -(mean + zc * std)


def run(scenarios: int = 100_000, seed: int = 21, dof: float = 4.0):
    rng = np.random.default_rng(seed)
    n = len(POSITIONS)
    corr = np.full((n, n), 0.35)
    corr[3, :] = corr[:, 3] = -0.15
    corr[4, :] = corr[:, 4] = -0.25
    corr[0, 1] = corr[1, 0] = 0.75
    np.fill_diagonal(corr, 1.0)
    L = np.linalg.cholesky(corr)
    vals = np.array([p[1] for p in POSITIONS], dtype=float)
    vols = np.array([p[2] for p in POSITIONS])
    z = rng.standard_normal((scenarios, n)) @ L.T
    # fat tails: scale by a chi-square mixing variable (multivariate t), variance kept at 1
    w = np.sqrt((dof - 2) / rng.chisquare(dof, scenarios))[:, None]
    pnl_by = vals * vols * z * w
    pnl = pnl_by.sum(axis=1)
    var = {c: -np.quantile(pnl, 1 - c) for c in (0.95, 0.99)}
    es = {c: -pnl[pnl <= -var[c]].mean() for c in (0.95, 0.99)}
    sigma = float(np.sqrt((vals * vols) @ corr @ (vals * vols)))
    # the normal VaR corrected for the sample's skewness and fat tails
    zs = (pnl - pnl.mean()) / pnl.std()
    cf = [cornish_fisher_var(float(pnl.mean()), float(pnl.std()), c, float((zs ** 3).mean()), float((zs ** 4).mean() - 3))
          for c in (0.95, 0.99)]
    ws = fresh_sheet("VaR")
    title(ws, f"Monte Carlo VaR: {scenarios:,} one-day scenarios (multivariate t, {dof:g} degrees of freedom)",
          "Positions in TWD; P&L = value x daily vol x correlated shock.")
    header(write(ws, "A4", [["Position", "Value", "Daily vol", "Standalone VaR 99%", "Contribution to ES 99%"]]))
    tail = pnl <= -var[0.99]
    rows = []
    for i, (name, v, vol) in enumerate(POSITIONS):
        rows.append([name, v, vol, float(-np.quantile(pnl_by[:, i], 0.01)), float(-pnl_by[tail, i].mean())])
    write(ws, "A5", rows)
    ws.Range(f"B5:B{4 + n}").NumberFormat = "#,##0"
    ws.Range(f"C5:C{4 + n}").NumberFormat = "0.0%"
    ws.Range(f"D5:E{4 + n}").NumberFormat = "#,##0"
    r = 6 + n
    header(write(ws, f"A{r}", [["Measure", "95%", "99%"]]))
    write(ws, f"A{r + 1}", [["Monte Carlo VaR", float(var[0.95]), float(var[0.99])],
                           ["Expected shortfall", float(es[0.95]), float(es[0.99])],
                           ["Normal (parametric) VaR", 1.6449 * sigma, 2.3263 * sigma],
                           ["Cornish-Fisher VaR", *cf]])
    ws.Range(f"B{r + 1}:C{r + 4}").NumberFormat = "#,##0"
    ws.Range(f"A{r + 6}").Value = ("The fat tails make the 99% numbers larger than the normal VaR; ES adds what lies beyond the VaR. "
                               "Cornish-Fisher corrects the normal VaR with the sample skewness and kurtosis; with tails this fat "
                               "(a t with 4 degrees of freedom has no finite kurtosis) it overshoots at 99%.")
    ws.Columns(1).ColumnWidth = 24
    ws.Columns(4).ColumnWidth = 18
    ws.Columns(5).ColumnWidth = 22
    fig = plot.figure("Monte Carlo VaR", rows=1, cols=2, size=(1200, 480))
    fig[0].hist(pnl / 1e6, bins=120, label="P&L (millions)", color="#5c6bc0")
    fig[0].vline([-var[0.95] / 1e6], label="VaR 95%", color="#ef6c00", width=2)
    fig[0].vline([-var[0.99] / 1e6], label="VaR 99%", color="#c62828", width=2)
    fig[0].vline([-es[0.99] / 1e6], label="ES 99%", color="#6a1b9a", width=2)
    fig[0].set(title="One-day P&L distribution", xlabel="TWD millions")
    contrib = np.array([row[4] for row in rows]) / 1e6
    fig[1].bar(list(range(1, n + 1)), contrib, label="ES 99% contribution (millions)", color="#26a69a")
    for i, (name, *_r) in enumerate(POSITIONS):
        fig[1].text(i + 1, contrib[i] + 0.02 * max(abs(contrib).max(), 1e-9), name)
    fig[1].set(title="Where the tail loss comes from")
    print(f"[example] VaR 99% {var[0.99]:,.0f}, ES 99% {es[0.99]:,.0f}, normal VaR 99% {2.3263 * sigma:,.0f}")
    return fig


def Workbook_Open():
    run()
