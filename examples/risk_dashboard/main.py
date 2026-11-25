# SPDX-License-Identifier: Apache-2.0
"""Option risk dashboard: positions with Greeks, limits, and a spot x volatility scenario grid of P&L.

A book of TXO-style index options and futures is valued with Black-Scholes (_math, plain numpy). The screen is made
of building blocks: the positions are a bound table with a GROUPBY formula for the totals by expiry, the limits a
bound table whose usage column is a formula with data bars and a red rule beyond 100%, and a scenario grid reprices
the whole book for spot moves of -10% to +10% and volatility shifts of -5 to +5 points: a heatmap on the sheet and
in a chart.

Open risk_dashboard.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from yc import plot
from yc.data import bind

import ex_math as qm
from ex_util import DARK, PNL_FORMAT, add_rule, color_scale, fresh_sheet, header, title, write, dark_table

SPOT, RATE, MULT = 23000.0, 0.015, 50.0
BOOK = [  # contract, type, strike, days, qty, vol
    ("TXO 202611 C23500", "C", 23500, 24, -40, 0.17),
    ("TXO 202611 C24000", "C", 24000, 24, 60, 0.165),
    ("TXO 202611 P22000", "P", 22000, 24, -50, 0.21),
    ("TXO 202611 P21500", "P", 21500, 24, 30, 0.225),
    ("TXO 202612 C23000", "C", 23000, 52, 25, 0.18),
    ("TXO 202612 P22500", "P", 22500, 52, -20, 0.20),
    ("TXF 202611", "F", 0, 24, -2, 0.0),
]


def value(spot, vol_shift=0.0):
    """Book value (points x multiplier) and Greeks per position at a spot and a volatility shift."""
    rows = []
    for name, t, k, days, q, vol in BOOK:
        T = days / 365
        if t == "F":
            px, g = spot, {"delta": 1.0, "gamma": 0.0, "vega": 0.0, "theta": 0.0}
            mult = 200.0
        else:
            v = max(vol + vol_shift, 0.01)
            px = float(qm.bs_price(spot, k, T, RATE, v, t))
            g = {kk: float(vv) for kk, vv in qm.greeks(spot, k, T, RATE, v, t).items()}
            mult = MULT
        rows.append((name, t, k, days, q, px, g, mult))
    return rows


def run():
    rows = value(SPOT)
    ws = fresh_sheet("Risk", trading=True)
    title(ws, f"Option risk dashboard (index {SPOT:,.0f}; simulated book)", "Greeks in TWD per index point / vol point / day.")
    data = {"Contract": [], "Product": [], "Expiry": [], "Type": [], "Strike": [], "Qty": [], "Avg": [], "Last": [], "PnL": [],
            "Delta": [], "Gamma": [], "Vega": [], "Theta": []}
    for name, t, k, days, q, px, g, mult in rows:
        data["Contract"].append(name)
        data["Product"].append(name.split()[0])
        data["Expiry"].append(name.split()[1])
        data["Type"].append({"C": "Call", "P": "Put", "F": "Future"}[t])
        data["Strike"].append(k or None)
        data["Qty"].append(q)
        data["Avg"].append(round(px * 1.02, 1))
        data["Last"].append(round(px, 1))
        data["PnL"].append((px - px * 1.02) * q * mult)
        data["Delta"].append(g["delta"] * q * mult)
        data["Gamma"].append(g["gamma"] * q * mult)
        data["Vega"].append(g["vega"] * q * mult)
        data["Theta"].append(g["theta"] * q * mult)
    # positions: a bound table (a live feed would call pos.update / pos.sync with the same columns)
    ws.Range("A3").Value = "Positions"
    signed = "[Red]#,##0.00;[Green]-#,##0.00;0.00"
    pos = bind(ws, pd.DataFrame(data), key="Contract", name="RiskPos", at="A4", record=False, flash=["Last", "PnL"],
               formats={"Strike": "#,##0", "Qty": "[Red]#,##0;[Green]-#,##0;0", "Avg": "#,##0.00", "Last": "#,##0.00",
                        "PnL": PNL_FORMAT, "Delta": signed, "Gamma": "0.0000", "Vega": "#,##0.0", "Theta": signed})
    dark_table(pos)
    # totals by expiry: one dynamic-array formula over the table, recalculated as the table changes
    ws.Range("P3").Value = "Totals by expiry"
    ws.Range("P4").Formula = "=GROUPBY(ExRiskPos[[#All],[Expiry]],CHOOSECOLS(ExRiskPos[#All],6,9,10,11,12,13),SUM,3)"
    for col, fmt in zip("QRSTUV", ("#,##0", PNL_FORMAT, signed, "0.0000", "#,##0.0", signed)):  # the spill keeps these
        ws.Range(f"{col}5:{col}12").NumberFormat = fmt
    net = {k: float(sum(data[k])) for k in ("Delta", "Gamma", "Vega", "Theta")}
    # limits: Used = |Value| / Limit as a formula, data bars, and a red rule beyond 100%
    limits = {"Net delta": 30000, "Net gamma": 400, "Net vega": 150000, "Theta": 60000}
    ws.Range("A16").Value = "Risk limits"
    lim = bind(ws, pd.DataFrame({"Name": list(limits), "Value": [abs(net["Delta"]), abs(net["Gamma"]), abs(net["Vega"]), abs(net["Theta"])],
                                 "Limit": list(limits.values())}), key="Name", name="RiskLimits", at="A17", record=False,
               formats={"Value": "#,##0.0", "Limit": "#,##0"})
    dark_table(lim)
    ws.Range("D17").Value = "Used"
    used = ws.Range("D18:D21")
    used.Formula = "=IFERROR(ABS(B18)/C18,0)"
    used.NumberFormat = "0%"
    used.FormatConditions.AddDatabar()
    add_rule(used, "=D18>1", fill=DARK["alert_bg"], font=DARK["up"], bold=True)
    for i, w in enumerate((18, 10, 10, 10)):
        ws.Columns(i + 1).ColumnWidth = w
    ws.Range("L:M").ColumnWidth = 12  # Vega / Theta in the hundred thousands
    ws.Range("R:V").ColumnWidth = 11
    # the scenario grid
    moves = np.round(np.arange(-0.10, 0.1001, 0.02), 2)
    shifts = np.arange(-0.05, 0.0501, 0.01)
    base = sum(r[5] * r[4] * r[7] for r in rows)
    grid = np.empty((len(shifts), len(moves)))
    for i, dv in enumerate(shifts):
        for j, m in enumerate(moves):
            v = value(SPOT * (1 + m), dv)
            grid[i, j] = sum(r[5] * r[4] * r[7] for r in v) - base
    r0 = 25
    ws.Cells(r0, 1).Value = "Scenario P&L (TWD): rows = volatility shift, columns = index move"
    ws.Cells(r0, 1).Font.Bold = True
    header(write(ws, f"A{r0 + 1}", [["Vol \\ Spot"] + [float(m) for m in moves]]))
    write(ws, f"A{r0 + 2}", [[float(dv)] + [float(x) for x in row] for dv, row in zip(shifts, grid)])
    ws.Range(ws.Cells(r0 + 1, 2), ws.Cells(r0 + 1, 1 + len(moves))).NumberFormat = "+0%;-0%;0%"
    ws.Range(ws.Cells(r0 + 2, 1), ws.Cells(r0 + 1 + len(shifts), 1)).NumberFormat = "+0%;-0%;0%"
    body = ws.Range(ws.Cells(r0 + 2, 2), ws.Cells(r0 + 1 + len(shifts), 1 + len(moves)))
    body.NumberFormat = "#,##0,K;-#,##0,K"
    color_scale(body)  # losses green, profits red
    fig = plot.figure("Scenario P&L", size=(760, 520), theme="dark")
    lim_k = float(np.abs(grid).max()) / 1e3 or 1.0
    fig[0].heatmap(grid[::-1] / 1e3, xlabels=[f"{m:+.0%}" for m in moves], ylabels=[f"{s:+.0%}" for s in shifts[::-1]],
                   cmap="pnl", vmin=-lim_k, vmax=lim_k, fmt="%.0f")
    fig[0].set(title="P&L (thousands) by index move (x) and vol shift (y)")
    print(f"[example] net delta {net['Delta']:,.0f}, gamma {net['Gamma']:,.1f}, vega {net['Vega']:,.0f}, theta {net['Theta']:,.0f}")
    return pos, lim, fig


def Workbook_Open():
    run()
