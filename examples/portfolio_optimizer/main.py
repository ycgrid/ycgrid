# SPDX-License-Identifier: Apache-2.0
"""Portfolio optimizer: the mean-variance frontier, and the maximum Sharpe portfolio found by Solver on the sheet.

Eight simulated assets: the Analysis ToolPak's Covariance writes the covariance matrix; the sheet holds the weights,
the portfolio return, risk and Sharpe ratio as formulas (MMULT); Excel's Solver (SolverOk / SolverAdd / SolverSolve,
the same calls as in Excel) maximizes the Sharpe ratio with long-only weights that add up to 1. numpy draws the
frontier for comparison.

Open portfolio_optimizer.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import numpy as np

from yc import analysis, excel, plot

from ex_util import fresh_sheet, header, title, write

ASSETS = ["TW Equity", "US Equity", "EU Equity", "EM Equity", "Gov Bonds", "Credit", "Gold", "REITs"]
MU = np.array([0.085, 0.080, 0.065, 0.095, 0.025, 0.040, 0.045, 0.070])  # annual expected returns
VOL = np.array([0.20, 0.16, 0.17, 0.23, 0.05, 0.08, 0.15, 0.19])
RF = 0.015


def run(seed: int = 3, months: int = 120):
    rng = np.random.default_rng(seed)
    corr = np.full((8, 8), 0.45)
    corr[4, :] = corr[:, 4] = -0.1
    corr[6, :] = corr[:, 6] = 0.05
    corr[5, 4] = corr[4, 5] = 0.5
    np.fill_diagonal(corr, 1.0)
    cov_true = np.outer(VOL, VOL) * corr / 12
    rets = rng.multivariate_normal(MU / 12, cov_true, months)
    ws = fresh_sheet("Portfolio")
    title(ws, "Portfolio optimizer: covariance by the Analysis ToolPak, maximum Sharpe by Solver",
          f"Monthly returns of 8 simulated assets ({months} months); risk-free rate {RF:.1%}. Weights are long only and add up to 1.")
    # monthly returns from row 30
    header(write(ws, "A30", [["Month"] + ASSETS]))
    write(ws, "A31", [[i + 1] + [float(x) for x in row] for i, row in enumerate(rets)])
    ws.Range(f"B31:I{30 + months}").NumberFormat = "0.00%"
    # covariance (ToolPak, population like Excel's tool) -> annualized below
    ws.Range("K29").Value = "Analysis ToolPak: Covariance (monthly)"
    ws.Range("K29").Font.Bold = True
    analysis.Mcovar(ws.Range(f"B30:I{30 + months}"), ws.Range("K30"), "C", True)
    # model block: weights, expected returns, formulas
    header(write(ws, "A4", [["Asset", "Exp. return", "Weight"]]))
    write(ws, "A5", [[a, float(m), 1 / 8] for a, m in zip(ASSETS, MU)])
    ws.Range("B5:C12").NumberFormat = "0.0%"
    # the full symmetric covariance, annualized, from the ToolPak's lower triangle
    header(write(ws, "E4", [["Covariance (annual)"] + ASSETS]))
    for i in range(8):
        ws.Cells(5 + i, 5).Value = ASSETS[i]
        for j in range(8):
            a, b = max(i, j), min(i, j)
            ws.Cells(5 + i, 6 + j).Formula = f"=12*{excel.Range._from(ws._id, [(30 + a, 11 + b, 30 + a, 11 + b)]).Address()}"
    ws.Range("F5:M12").NumberFormat = "0.0000"
    labels = [["Sum of weights", "=SUM(C5:C12)"], ["Portfolio return", "=SUMPRODUCT(B5:B12,C5:C12)"],
              ["Portfolio risk", "=SQRT(MMULT(TRANSPOSE(C5:C12),MMULT(F5:M12,C5:C12)))"],
              ["Risk-free rate", RF], ["Sharpe ratio", "=(B16-B18)/B17"]]
    for i, (name, f) in enumerate(labels):
        ws.Cells(15 + i, 1).Value = name
        if isinstance(f, str):
            ws.Cells(15 + i, 2).Formula = f
        else:
            ws.Cells(15 + i, 2).Value = f
    ws.Range("B16:B18").NumberFormat = "0.00%"
    ws.Range("B19").NumberFormat = "0.000"
    ws.Range("A19:B19").Font.Bold = True
    ws.Columns(1).ColumnWidth = 18
    ws.Columns(5).ColumnWidth = 18
    # Solver: maximize the Sharpe ratio, weights >= 0 and summing to 1 (Excel's Solver calls)
    excel.SolverReset()
    excel.SolverOk(SetCell="$B$19", MaxMinVal=1, ByChange="$C$5:$C$12", Engine=2)
    excel.SolverAdd(CellRef="$B$15", Relation=2, FormulaText="1")
    excel.SolverAdd(CellRef="$C$5:$C$12", Relation=3, FormulaText="0")
    code = excel.SolverSolve(UserFinish=True)
    excel.SolverFinish(KeepFinal=1)
    w_solver = np.array([float(ws.Cells(5 + i, 3).Value) for i in range(8)])
    ws.Range("A21").Value = f"Solver result code {code} (0 = solution found). Weights above are Solver's."
    # the frontier with numpy: 20,000 random long-only portfolios
    cov = np.cov(rets.T, ddof=0) * 12
    mu = MU
    w = rng.dirichlet(np.ones(8) * 0.6, 20000)
    pr = w @ mu
    pv = np.sqrt(np.einsum("ij,jk,ik->i", w, cov, w))
    # the long-only frontier: the best return the random portfolios reach in each band of risk
    bands = np.linspace(pv.min(), pv.max(), 41)
    which = np.digitize(pv, bands)
    front = [(float(pv[which == i].mean()), float(pr[which == i].max())) for i in range(1, len(bands)) if (which == i).any()]
    fv, fr = np.array(front).T
    fr = np.maximum.accumulate(fr)  # the efficient part only rises
    fig = plot.figure("Portfolio optimizer", rows=1, cols=2, size=(1200, 520))
    fig[0].scatter(pv * 100, pr * 100, label="Random portfolios", color="#b0bec5", size=1.5)
    fig[0].scatter(np.sqrt(np.diag(cov)) * 100, mu * 100, label="Assets", color="#1565c0", size=6, marker="s")
    for a, x, y in zip(ASSETS, np.sqrt(np.diag(cov)) * 100, mu * 100):
        fig[0].text(x, y + 0.25, a)
    sv = float(np.sqrt(w_solver @ cov @ w_solver)) * 100
    sr = float(w_solver @ mu) * 100
    fig[0].line(fv * 100, fr * 100, label="Long-only frontier (random portfolios' edge)", color="#37474f", width=2)
    fig[0].scatter([sv], [sr], label="Max Sharpe (Solver)", color="#c62828", size=9, marker="D")
    fig[0].line([0, sv * 1.8], [RF * 100, RF * 100 + (sr - RF * 100) * 1.8], label="Capital market line", color="#c62828", width=1)
    fig[0].set(title="Risk and return (% a year)", xlabel="risk %", ylabel="return %")
    fig[1].bar(list(range(1, 9)), w_solver * 100, label="Weight %", color="#26a69a")
    for i, a in enumerate(ASSETS):
        fig[1].text(i + 1, w_solver[i] * 100 + 1.5, a)
    fig[1].set(title="Maximum Sharpe weights (%)")
    print(f"[example] Solver: return {sr:.2f}%, risk {sv:.2f}%, Sharpe {(sr - RF * 100) / sv:.2f}")
    return fig


def Workbook_Open():
    run()
