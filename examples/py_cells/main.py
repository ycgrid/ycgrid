# SPDX-License-Identifier: Apache-2.0
"""Python in cells: =PY formulas that run pandas and numpy on ranges and spill the results - statistics, rolling
means, a regression - recalculated when the data change, sharing the Immediate window's namespace.

Open py_cells.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import numpy as np

from ex_util import fresh_sheet, header, title, write


def run(days: int = 250):
    rng = np.random.default_rng(8)
    r1 = rng.normal(0.0004, 0.012, days)
    r2 = 0.6 * r1 + rng.normal(0, 0.008, days)
    p1 = 100 * np.exp(np.cumsum(r1))
    p2 = 50 * np.exp(np.cumsum(r2))
    ws = fresh_sheet("PY Cells")
    title(ws, "Python in cells: =PY runs pandas and numpy on ranges and spills the result (change a price and they recalculate)",
          "x is the first range (a header row makes it a DataFrame), _2 the second. Functions defined in the Immediate window work here too.")
    header(write(ws, "A4", [["Day", "TXF", "Stock"]]))
    write(ws, "A5", [[i + 1, float(a), float(b)] for i, (a, b) in enumerate(zip(p1, p2))])
    end = 4 + days
    rows = [  # the code is a formula string: single quotes inside, statements separated by ";"
        ("Summary statistics", f'=PY("x.describe().round(2)", A4:C{end})'),
        ("Daily returns: correlation", f"=PY(\"x[['TXF', 'Stock']].pct_change().corr().round(3)\", A4:C{end})"),
        ("Beta of the stock to TXF", f"=PY(\"import numpy as np; r = x[['TXF', 'Stock']].pct_change().dropna(); "
                                     f"np.polyfit(r.TXF, r.Stock, 1)[0]\", A4:C{end})"),
        ("20-day mean of TXF (last 5)", f'=PY("x.TXF.rolling(20).mean().tail(5).round(1)", A4:C{end})'),
    ]
    r = 4
    for label, formula in rows:
        ws.Range(f"E{r}").Value = label
        ws.Range(f"E{r}").Font.Bold = True
        ws.Range(f"E{r + 1}").Formula = formula
        r += 12
    ws.Columns(5).ColumnWidth = 30
    print("[example] =PY formulas written in E5, E17, E29 and E41")
    return ws


def Workbook_Open():
    run()
