# SPDX-License-Identifier: Apache-2.0
"""YCGrid demo project: quote table, button, combo box, charts, custom panel, conditional formatting, data validation, filter, live updates.

This file = Excel's ThisWorkbook. When YCGrid opens demo.ycg in this folder it loads
main.py -> sheets\\*.py -> modules\\*.py, then calls Workbook_Open().
Save after editing and YCGrid reloads automatically (no restart needed).
"""
import random

from yc.excel import *

UI_VERSION = 3  # bump the version to rebuild the whole UI (see build_ui)
PRODUCTS = [("2330", "TSMC", 1050.0), ("2317", "Hon Hai", 205.0), ("2454", "MediaTek", 1380.0), ("2308", "Delta", 395.0),
            ("2412", "Chunghwa Tel", 128.5), ("2881", "Fubon FHC", 92.3), ("2882", "Cathay FHC", 66.8), ("3711", "ASE", 168.0),
            ("2603", "Evergreen", 210.5), ("1301", "Formosa Plastics", 48.2)]

running = True


def Workbook_Open():
    if ThisWorkbook.Store.get("ui.version") != UI_VERSION:
        build_ui()
        ThisWorkbook.Store["ui.version"] = UI_VERSION
    Application.StatusBar = "Demo project loaded: press \"Randomize\" or use the panel on the right"
    schedule_tick()


def build_ui():
    ws = Worksheets(1)
    ws.Name = "Quotes"
    ws.Range("A1:J30").FormatConditions.Delete()
    ws.Range("A1:J30").Validation.Delete()
    ws.Range("A1:J30").Clear()
    ws.Range("A1:G1").Value = [["Symbol", "Name", "Price", "Change", "Qty", "Amount", "Side"]]
    for i, (code, name, price) in enumerate(PRODUCTS, start=2):
        ws.Range(f"A{i}:E{i}").Value = [["'" + code, name, price, 0, random.randint(1, 50) * 10]]
        ws.Range(f"F{i}").Formula = f"=C{i}*E{i}"
    ws.Range("A1:G1").Font.Bold = True
    ws.Range("A1:G1").Interior.Color = RGB(221, 235, 247)
    ws.Range("A1:G11").Borders.LineStyle = xlContinuous
    ws.Range("C2:C11").NumberFormat = "#,##0.00"
    ws.Range("D2:D11").NumberFormat = "0.00%;[Red]-0.00%"
    ws.Range("E2:F11").NumberFormat = "#,##0"
    ws.Range("A13").Value = "Total"
    ws.Range("F13").Formula = "=SUM(F2:F11)"
    ws.Range("F13").NumberFormat = "#,##0"
    ws.Range("A13:F13").Font.Bold = True
    ws.Range("A1").AddComment("Quotes update every second (tick in main.py)")

    # conditional formatting: color scale for the change, data bars for the quantity
    ws.Range("D2:D11").FormatConditions.AddColorScale(3)
    ws.Range("E2:E11").FormatConditions.AddDatabar()
    # data validation: Side can only be Buy / Sell / Hold
    v = ws.Range("G2:G11").Validation
    v.Add(xlValidateList, Formula1="Buy,Sell,Hold")
    v.InputTitle = "Side"
    v.InputMessage = "Pick Buy, Sell or Hold from the list"
    # AutoFilter
    ws.Range("A1:G11").AutoFilter()
    # column widths: AutoFit after AutoFilter so the filter buttons are counted,
    # then leave room for prices, changes and the total that grow while live
    ws.Columns("A:G").AutoFit()
    ws.Columns("C:D").ColumnWidth = 11
    ws.Columns("F:F").ColumnWidth = 12

    # controls (their events are in sheets\Sheet1.py)
    for o in list(ws.OLEObjects):
        o.Delete()
    btn = ws.OLEObjects.Add("Forms.CommandButton.1", Left=10, Top=230, Width=100, Height=26)
    btn.Caption = "Randomize"
    cb = ws.OLEObjects.Add("Forms.ComboBox.1", Left=120, Top=232, Width=110, Height=22)
    cb.List = ["All", "Up", "Down"]
    cb.LinkedCell = "I2"
    cb.Style = fmStyleDropDownList
    ws.Range("I1").Value = "Show"
    ws.Range("I2").Value = "All"

    # charts: column chart of the amounts, line chart of the live price
    for c in list(ws.ChartObjects):
        c.Delete()
    bar = ws.ChartObjects.Add(530, 0, 360, 200, Name="Amount")
    bs = bar.Chart.SeriesCollection.NewSeries()
    bs.Values = "Quotes!$F$2:$F$11"
    bs.XValues = "Quotes!$B$2:$B$11"
    bs.Name = "Amount"
    bar.Chart.ChartType = xlColumnClustered
    bar.Chart.HasTitle = True
    bar.Chart.ChartTitle.Text = "Amount by Stock"
    live = ws.ChartObjects.Add(530, 210, 360, 200, Name="Live")
    s = live.Chart.SeriesCollection.NewSeries()
    s.Name = "TSMC"
    s.SetData(y=[PRODUCTS[0][2]], Capacity=300)
    live.Chart.ChartType = xlLine
    live.Chart.HasTitle = True
    live.Chart.ChartTitle.Text = "TSMC Live Price"

    # custom panel
    if "Console" in Application.Panels:
        Application.Panels("Console").Delete()
    p = Application.Panels.Add("Console", Caption="Demo Console", Dock=ycDockRight, Width=240)
    p.Controls.Add(ycLabel, "Info", Caption="Showing: ", LinkedCell="I2")
    p.Controls.Add(ycButton, "Pause", Caption="Pause / Resume Live Updates", OnAction=toggle_running)
    p.Controls.Add(ycSlider, "Volatility", Caption="Volatility (%)", Min=0, Max=5, Value=1)
    p.Controls.Add(ycSeparator, "Line", Caption="Tips")
    p.Controls.Add(ycLabel, "Tips", Caption="Alt+F11 opens the code in VS Code; saving main.py reloads it automatically.")


def toggle_running(ctrl=None):
    global running
    running = not running
    Application.StatusBar = "Live updates: " + ("running" if running else "paused")
    if running:
        schedule_tick()


def volatility():
    try:
        return float(Application.Panels("Console").Volatility.Value) / 100.0
    except Exception:  # noqa: BLE001
        return 0.01


def randomize(scale=None):
    """Every price drifts randomly; the change is computed from the opening price."""
    ws = Worksheets("Quotes")
    scale = volatility() if scale is None else scale
    prices = ws.Range("C2:C11").Value
    rows = []
    for i, row in enumerate(prices):
        price = float(row[0]) * (1 + random.uniform(-scale, scale))
        open_price = PRODUCTS[i][2]  # opening price (rows are in the same order as PRODUCTS)
        rows.append([round(price, 2), round(price / open_price - 1, 4)])
    ws.Range("C2:D11").Value = rows
    return rows


def tick():
    if not running:
        return
    rows = randomize()
    ws = Worksheets("Quotes")
    ws.ChartObjects("Live").Chart.SeriesCollection(1).AppendData(y=rows[0][0])
    schedule_tick()


def schedule_tick():
    Application.OnTime(Now() + TimeValue("00:00:01"), "tick")


def Workbook_BeforeClose(Cancel):
    global running
    running = False
