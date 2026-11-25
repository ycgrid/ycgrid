# SPDX-License-Identifier: Apache-2.0
"""Option payoff builder: a strategy's legs in cells, its payoff at expiry as formulas, and an Excel chart.

Everything after the setup is ordinary spreadsheet modelling: change a leg (type C / P / F, strike, quantity,
premium) or add one in the empty rows, and the payoff column, the summary and the chart recalculate at once. The
starting strategy is an iron condor on the index.

Open option_payoff.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

from yc import excel

from ex_util import fresh_sheet, header, title, write

LEGS = [  # type, strike, qty (+ buy / - sell), premium (points)
    ("P", 21500, 1, 38),
    ("P", 22000, -1, 85),
    ("C", 24000, -1, 92),
    ("C", 24500, 1, 41),
]


def run(low: int = 20000, high: int = 26000, step: int = 100, multiplier: float = 50.0):
    ws = fresh_sheet("Payoff")
    title(ws, "Option payoff builder: edit the legs (yellow cells) and everything follows",
          "Type: C call, P put, F future (Strike = entry price). Qty: + bought, - sold. Premium in index points.")
    header(write(ws, "A4", [["Type", "Strike", "Qty", "Premium"]]))
    write(ws, "A5", [list(leg) for leg in LEGS])
    legs = ws.Range("A5:D12")
    legs.Interior.Color = 0xCCF2FF  # light yellow: inputs
    ws.Range("F4").Value = "Multiplier"
    ws.Range("G4").Value = multiplier
    ws.Range("G4").Interior.Color = 0xCCF2FF
    # summary formulas
    n = (high - low) // step + 1
    last = 4 + n
    summary = [("Net premium (points)", "=-SUMPRODUCT(C5:C12,D5:D12)"),
               ("Max profit (TWD)", f"=MAX(K5:K{last})"),
               ("Max loss (TWD)", f"=MIN(K5:K{last})"),
               # the break-evens: between the table's prices where the P&L changes sign (the payoff is linear there)
               ("Profitable range", f'=IFERROR(LET(x,I5:I{last},y,K5:K{last},a,XMATCH(TRUE,y>0),b,XMATCH(TRUE,y>0,0,-1),'
                                    f'lo,IF(a=1,INDEX(x,1),INDEX(x,a-1)-INDEX(y,a-1)*(INDEX(x,a)-INDEX(x,a-1))/(INDEX(y,a)-INDEX(y,a-1))),'
                                    f'hi,IF(b=ROWS(x),INDEX(x,b),INDEX(x,b)+INDEX(y,b)*(INDEX(x,b+1)-INDEX(x,b))/(INDEX(y,b)-INDEX(y,b+1))),'
                                    f'TEXT(lo,"#,##0")&" to "&TEXT(hi,"#,##0")),"none")')]
    for i, (name, f) in enumerate(summary):
        ws.Cells(6 + i, 6).Value = name
        ws.Cells(6 + i, 7).Formula = f
    ws.Range("G6").NumberFormat = "#,##0.0"
    ws.Range("G7:G8").NumberFormat = "#,##0;[Red]-#,##0"
    ws.Columns(6).ColumnWidth = 20
    ws.Columns(7).ColumnWidth = 16
    # the payoff table: one formula per price, over all legs (blank legs count as nothing)
    header(write(ws, "I4", [["Index at expiry", "P&L (points)", "P&L (TWD)"]]))
    ws.Range(f"I5:I{last}").Value = [[low + i * step] for i in range(n)]
    ws.Range(f"J5:J{last}").Formula = ('=SUMPRODUCT($C$5:$C$12,($A$5:$A$12="C")*(I5>$B$5:$B$12)*(I5-$B$5:$B$12)'
                                      '+($A$5:$A$12="P")*(I5<$B$5:$B$12)*($B$5:$B$12-I5)'
                                      '+($A$5:$A$12="F")*(I5-$B$5:$B$12)-($A$5:$A$12<>"F")*$D$5:$D$12)')
    ws.Range(f"K5:K{last}").Formula = "=J5*$G$4"
    ws.Range(f"I5:I{last}").NumberFormat = "#,##0"
    ws.Range(f"J5:K{last}").NumberFormat = "#,##0;[Red]-#,##0"
    neg = ws.Range(f"K5:K{last}").FormatConditions.Add(excel.xlCellValue, excel.xlLess, "0")
    neg.Interior.Color = 0xDCE6FF
    pos = ws.Range(f"K5:K{last}").FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "0")
    pos.Interior.Color = 0xD8F0E2
    ws.Columns(9).ColumnWidth = 15
    ws.Columns(10).ColumnWidth = 13
    ws.Columns(11).ColumnWidth = 13
    # an Excel chart of the TWD payoff, fed by the cells
    top = ws.Range("A14")  # under the legs and the summary, beside the payoff table (not over it)
    ch = ws.Shapes.AddChart2(-1, excel.xlLine, Left=top.Left, Top=top.Top, Width=ws.Range("A14:G14").Width, Height=300).Chart
    s = ch.SeriesCollection.NewSeries()
    s.Name = "P&L at expiry (TWD)"
    s.XValues = ws.Range(f"I5:I{last}")
    s.Values = ws.Range(f"K5:K{last}")
    ch.HasTitle = True
    ch.ChartTitle.Text = "Payoff at expiry"
    ch.HasLegend = False
    print(f"[example] net premium {ws.Range('G6').Value:,.1f} points; max profit {ws.Range('G7').Value:,.0f}, "
          f"max loss {ws.Range('G8').Value:,.0f}")
    return ch


def Workbook_Open():
    run()
