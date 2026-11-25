# SPDX-License-Identifier: Apache-2.0
"""Take the README screenshots in docs/images, in English and Traditional Chinese: the quote board, the Immediate
window and the PivotTable report built here, and the gallery of example screens (GALLERY: each opens its workbook in
examples/, waits for its live data, docks its charts beside the cells and takes the picture with the ribbon collapsed).

Usage (repo root, with the bundled Python; the animated picture is tools/make_readme_gif.py):
    dist_release\\runtime\\python\\python.exe -E -s tools/make_readme_shots.py [--exe dist_release\\ycgrid.exe] [--only NAME ...]

Each screen runs in its own hidden YCGrid at 2000 x 1221, scaled down to 1600 wide. The data is simulated with a fixed
seed, so the pictures only change when YCGrid's look changes. A picture in the repository must not show a personal
path: YCGrid's messages show the user's folders as %TEMP%, %LOCALAPPDATA%... (the Immediate window prints the folder of
its Python host), and the run stops when the console still holds the user's profile folder.
"""
from __future__ import annotations

import argparse
import math
import os
import random
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SIZE = (2000, 1221)  # the window; pictures are scaled down to 1600 wide (the old pictures: 1600 x 977)
WIDTH = 1600
SYMBOLS = [("2330 TSMC", 284.5), ("2317 Hon Hai", 603.0), ("2454 MediaTek", 414.0), ("2308 Delta", 671.4),
           ("2382 Quanta", 718.4), ("3711 ASE", 98.2), ("2412 Chunghwa", 43.4), ("2881 Fubon", 905.1),
           ("2882 Cathay", 312.8), ("1301 Formosa", 272.4), ("2002 China Steel", 1080.2), ("2603 Evergreen", 533.5),
           ("AAPL", 931.6), ("MSFT", 540.2), ("NVDA", 714.9), ("AMZN", 193.1), ("GOOGL", 691.7), ("META", 1019.1),
           ("TSLA", 599.2), ("AVGO", 826.7)]


class Session:
    """One hidden YCGrid: launch, wait for Workbook_Open, close on exit."""

    def __init__(self, exe: str, lang: str):
        import yc
        self.proc = yc.launch(hidden=True, exe=exe, lang=lang, size=SIZE)
        self.conn = yc.connect(target=self.proc.pid)
        end = time.time() + 60
        while not self.conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
            time.sleep(0.2)

    def inp(self, script: str, wait: float = 0.3) -> None:
        self.conn.request("ui.input", {"script": script, "timeout_ms": 15000}, timeout=30)
        time.sleep(wait)

    def expand_ribbon(self) -> None:
        from yc import ui
        rb = ui.state()["ribbon"]
        if rb.get("collapsed"):
            x, y = rb["arrow"]
            self.inp(f"c:{int(x)},{int(y)}|w:8")
        self.inp(f"m:20,{SIZE[1] - 12}|w:6")  # the mouse on the status bar: no tooltip or crosshair in the picture

    def check_no_personal_path(self) -> None:
        """Stop before a picture shows the user's profile folder (it holds the user name)."""
        home = os.path.expanduser("~").lower()
        for line in self.conn.request("console.read", {"max": 5000}).get("lines", []):
            if home in str(line.get("text", "")).lower():
                raise SystemExit(f"the console shows the user's folder: {line['text']!r}; fix the message first")

    def shot(self, path: str) -> None:
        from yc import ui
        os.makedirs(os.path.dirname(path), exist_ok=True)
        ui.screenshot(path, max_width=WIDTH)
        print("wrote", os.path.relpath(path, ROOT), flush=True)

    def close(self) -> None:
        try:
            self.proc.terminate()
        except Exception:
            pass


def build_quote_board(rng: random.Random) -> None:
    from yc.excel import (ActiveWorkbook, RGB, Worksheets, xlColumns, xlLine, xlSparkLine, xlCenter)
    ws = Worksheets(1)
    ws.Name = "Quotes"
    hist = ActiveWorkbook.Worksheets.Add(After=ws)
    hist.Name = "Paths"  # "History" is reserved, as in Excel
    ws.Activate()
    head = ["Symbol", "Bid", "Ask", "Last", "Change", "Chg%", "Volume", "Position", "P&L", "Trend"]
    ws.Range("A1:J1").Value = [head]
    rows, paths = [], []
    for i, (name, px) in enumerate(SYMBOLS):
        path = [px]
        for _ in range(39):
            path.append(path[-1] * (1 + rng.gauss(0, 0.004)))
        last = round(path[-1], 2)
        chg = round(last - px, 2)
        side = 1 if (chg >= 0) == (rng.random() < 0.75) else -1  # most positions on the right side: a winning day
        pos = side * rng.randint(1, 5) * 1000
        rows.append([name, round(last - 0.05, 2), round(last + 0.05, 2), last, chg, chg / px,
                     rng.randint(12, 62) * 1000 + rng.randint(0, 999), pos, round(chg * pos)])
        paths.append(path)
    n = len(rows)
    ws.Range(f"A2:I{n + 1}").Value = rows
    hist.Range(f"A1:AN{n}").Value = paths
    ws.Range(f"A{n + 2}").Value = "Total"
    ws.Range(f"I{n + 2}").Formula = f"=SUM(I2:I{n + 1})"
    hdr = ws.Range("A1:J1")
    hdr.Interior.Color = RGB(31, 56, 100)
    hdr.Font.Color = RGB(255, 255, 255)
    hdr.Font.Bold = True
    ws.Range(f"A{n + 2}:J{n + 2}").Font.Bold = True
    ws.Range(f"B2:E{n + 1}").NumberFormat = "#,##0.00"
    ws.Range(f"F2:F{n + 1}").NumberFormat = "0.00%"
    ws.Range(f"G2:H{n + 2}").NumberFormat = "#,##0"
    ws.Range(f"I2:I{n + 2}").NumberFormat = "#,##0;[Red]-#,##0"
    ws.Range("B1:I1").HorizontalAlignment = xlCenter
    ws.Range(f"F2:F{n + 1}").FormatConditions.AddColorScale(3)
    ws.Range(f"I2:I{n + 1}").FormatConditions.AddDatabar()
    ws.Range(f"J2:J{n + 1}").SparklineGroups.Add(xlSparkLine, f"Paths!A1:AN{n}")
    ws.Columns("A").ColumnWidth = 16
    ws.Columns("B:I").ColumnWidth = 10
    ws.Columns("J").ColumnWidth = 14
    # the live chart: the last price of the first symbol, streamed point by point
    left = ws.Range("L3").Left
    top = ws.Range("L3").Top
    ch = ws.Shapes.AddChart2(-1, xlLine, Left=left, Top=top, Width=350, Height=260).Chart
    ch.HasTitle = True
    ch.ChartTitle.Text = SYMBOLS[0][0] + "  last price (live)"
    s = ch.SeriesCollection.NewSeries()
    s.Name = "Last"
    p = SYMBOLS[0][1]
    xs, ys = [], []
    for k in range(500):
        p += rng.gauss(0, 0.35) + 0.02 * math.sin(k / 40)
        xs.append(k)
        ys.append(round(p, 2))
    s.AppendData(xs, ys)
    ws.Range("A1").Select()


def shot_quote_board(exe: str, lang: str, out: str) -> None:
    s = Session(exe, lang)
    try:
        build_quote_board(random.Random(7))
        s.expand_ribbon()
        s.inp("g:A1|w:6", 0.6)
        s.shot(out)
    finally:
        s.close()


def shot_immediate(exe: str, lang: str, out: str) -> None:
    from yc import ui
    s = Session(exe, lang)
    try:
        build_quote_board(random.Random(7))
        s.expand_ribbon()
        s.inp("g:A1|w:4")
        ui.pane("immediate", True)
        time.sleep(4)  # the Immediate window starts its Python
        for line in ['?Range("I22").Value', "for i in range(2, 22): Cells(i, 11).Value = Cells(i, 9).Value > 0",
                     "?[ws.Name for ws in Worksheets]"]:
            s.inp("t:" + line + "|k:Enter|w:6", 1.5)
        s.check_no_personal_path()
        s.shot(out)
    finally:
        s.close()


def shot_pivot(exe: str, lang: str, out: str) -> None:
    from yc import ui
    from yc.excel import (ActiveWorkbook, Worksheets, xlColumnClustered, xlColumnField, xlDatabase, xlRowField, xlSum)
    s = Session(exe, lang)
    try:
        rng = random.Random(11)
        rep = Worksheets(1)
        rep.Name = "Report"
        tr = ActiveWorkbook.Worksheets.Add(After=rep)
        tr.Name = "Trades"
        rows = [["Date", "Symbol", "Side", "Trader", "Qty", "P&L"]]
        for d in range(240):
            rows.append([f"2026-{1 + d // 24:02d}-{1 + d % 24:02d}", rng.choice(["2317", "2330", "2454", "NVDA", "AAPL"]),
                         rng.choice(["Buy", "Sell"]), rng.choice(["Alice", "Bob", "Carol"]), rng.randint(1, 20) * 100,
                         round(rng.gauss(4000, 9000))])
        tr.Range(f"A1:F{len(rows)}").Value = rows
        rep.Activate()
        rep.Range("A1").Value = "P&L by symbol and trader"
        rep.Range("A1").Font.Bold = True
        rep.Range("A1").Font.Size = 14
        pc = ActiveWorkbook.PivotCaches().Create(xlDatabase, f"Trades!A1:F{len(rows)}")
        pt = pc.CreatePivotTable("Report!A3", "PnL")
        pt.PivotFields("Symbol").Orientation = xlRowField
        pt.PivotFields("Trader").Orientation = xlColumnField
        df = pt.AddDataField(pt.PivotFields("P&L"), "Sum of P&L", xlSum)
        df.NumberFormat = "#,##0;[Red]-#,##0"
        sc = ActiveWorkbook.SlicerCaches.Add2(pt, "Side")
        sc.Slicers.Add(rep, None, "Side", "Side", rep.Range("A13").Top, rep.Range("A13").Left, 120, 120)
        sc2 = ActiveWorkbook.SlicerCaches.Add2(pt, "Symbol")
        sc2.Slicers.Add(rep, None, "Symbol", "Symbol", rep.Range("C13").Top, rep.Range("C13").Left, 130, 210)
        sc2.SlicerItems("AAPL").Selected = False
        tbl = pt.TableRange1  # the chart starts right of the report (its width differs by language)
        ch = rep.Shapes.AddChart2(-1, xlColumnClustered, Left=tbl.Left + tbl.Width + 20, Top=rep.Range("F3").Top,
                                  Width=320, Height=250).Chart
        ch.SetSourceData(pt.TableRange1)
        rep.Columns("A:E").ColumnWidth = 12
        s.expand_ribbon()
        s.inp("g:B5|w:4")
        ui.pane("pivot_fields", True)
        s.inp("w:10", 1.0)
        s.shot(out)
    finally:
        s.close()


class ExampleSession(Session):
    """A hidden YCGrid that opens examples/<name>/<name>.ycg as a person does: its Workbook_Open builds the screen."""

    def __init__(self, exe: str, lang: str, name: str):
        import yc
        folder = os.path.join(ROOT, "examples", name)
        ycg = next(f for f in os.listdir(folder) if f.endswith(".ycg"))
        self.proc = yc.launch(os.path.join(folder, ycg), hidden=True, exe=exe, lang=lang, size=SIZE)
        self.conn = yc.connect(target=self.proc.pid)
        end = time.time() + 90
        while not self.conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
            time.sleep(0.2)

    def collapse_ribbon(self) -> None:
        """Tabs only (Ctrl+F1), as a trader keeps it: the screen gets the room."""
        from yc import ui
        if not ui.state()["ribbon"].get("collapsed"):
            self.inp("k:c-F1|w:8")

    def dock_figures(self, ratio: float) -> None:
        """The floating figures docked right of the sheet, so the cells they come from show beside them: the first
        beside the sheet, a second under it, a third right of the second (tiled, never stacked over each other)."""
        figs = [f for f in self.conn.request("plot.figures", {}) if not f.get("docked")]
        for i, f in enumerate(figs):
            p = {"fig": f["id"], "position": "right", "ratio": float(ratio)}
            if i == 1:
                p.update(position="bottom", ratio=0.5, anchor=figs[0]["id"])
            elif i >= 2:
                p.update(position="right", ratio=0.5, anchor=figs[1]["id"])
            self.conn.request("plot.dock", p)
            time.sleep(0.3)

    def click_cell(self, address: str, wait: float = 0.6) -> None:
        from yc import ui
        x0, y0, x1, y1 = ui.cell_rect(address)
        x, y = (x0 + x1) / 2, (y0 + y1) / 2
        self.inp(f"m:{x:.0f},{y:.0f}|w:2|c:{x:.0f},{y:.0f}|w:4", wait)


def trade_on_desk(s: ExampleSession) -> None:
    """A few orders on the trading desk, so its position, fills and chart marks show something: market buys, a sell,
    and a limit order resting in the ladder on each side."""
    for cell, wait in (("C4", 6), ("C4", 7), ("D4", 6), ("C4", 4)):
        s.click_cell(cell, wait)
    s.click_cell("B26", 1)   # a bid in the ladder, below the market: a working buy
    s.click_cell("D10", 1)   # an ask above it: a working sell
    s.inp("m:20,{}|w:4".format(SIZE[1] - 12), 1)


# README gallery: (example, seconds for its live data, dock ratio for floating figures or None, action)
GALLERY = {
    "market-watch": ("market_watch", 10, None, None),
    "option-chain": ("option_chain", 8, None, None),
    "technical-chart": ("technical_chart", 3, None, None),
    "trading-desk": ("trading_desk", 2, None, trade_on_desk),
    "options-desk": ("options_desk", 8, 0.42, None),
    "quote-renderers": ("quote_board", 10, None, None),
    "order-flow": ("order_flow", 120, 0.66, None),
    "depth-heatmap": ("depth_heatmap", 45, 0.66, None),
    "vol-surface": ("vol_surface", 4, 0.7, None),
    "factor-research": ("factor_research", 4, 0.66, None),
    "portfolio-optimizer": ("portfolio_optimizer", 4, 0.62, None),
    "monte-carlo-var": ("monte_carlo_var", 4, 0.62, None),
    "backtest": ("backtest_report", 4, 0.55, None),
    "quick-plot": ("quick_plot", 4, 0.97, None),
}


def shot_gallery(exe: str, key: str, lang: str, out: str) -> None:
    name, settle, ratio, action = GALLERY[key]
    s = ExampleSession(exe, lang, name)
    try:
        s.collapse_ribbon()
        if ratio:
            s.dock_figures(ratio)
        if action:
            action(s)
        time.sleep(settle)
        s.inp(f"m:20,{SIZE[1] - 12}|w:6", 0.5)  # the mouse on the status bar: no hover in the picture
        s.shot(out)
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", default=os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    ap.add_argument("--only", nargs="*", default=[],
                    help="quote-board, immediate, pivot, or a gallery picture: " + ", ".join(GALLERY))
    ap.add_argument("--lang", nargs="*", default=["en", "zh-TW"], help="the UI languages to take (en, zh-TW)")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(a.exe)), "python"))
    img = os.path.join(ROOT, "docs", "images")
    langs = [lg for lg in ("en", "zh-TW") if lg in a.lang]
    jobs = {
        "quote-board": lambda: [shot_quote_board(a.exe, lg, os.path.join(img, lg, "quote-board.png")) for lg in langs],
        "immediate": lambda: [shot_immediate(a.exe, lg, os.path.join(img, lg, "immediate.png")) for lg in langs],
        "pivot": lambda: [shot_pivot(a.exe, lg, os.path.join(img, lg, "pivot.png")) for lg in langs],
    }
    for key in GALLERY:
        jobs[key] = (lambda key=key: [shot_gallery(a.exe, key, lg, os.path.join(img, lg, key + ".png")) for lg in langs])
    for name, job in jobs.items():
        if not a.only or name in a.only:
            job()
    return 0


if __name__ == "__main__":
    sys.exit(main())
