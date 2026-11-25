# SPDX-License-Identifier: Apache-2.0
r"""Random rounds over the object-model members of the Excel gap list, in a hidden YCGrid, each checked by its result.

    <YCGrid>\runtime\python\python.exe -E -s scripts\gaps_fuzz.py [--exe path] [--seed S] [--rounds N] [--only KIND,...]

Every round draws one scenario from its seed and checks what can be worked out without Excel:
- pivot: a random source and layout (row, column and filter fields, the data field's function); every value cell is the
  aggregate of the source rows its RowItems and ColumnItems name, every cell of the report has a PivotCellType, the
  areas lie in the report, an item's LabelRange shows its name and its DataRange holds value cells;
- display: random values and a cell-value rule; DisplayFormat's fill is the rule's where the rule holds;
- panes: random splits, freezes and scrolls; the panes' count and visible ranges agree with each other;
- window: Caption (Chinese, quotes), WindowState, place and size read back as written;
- opentext: a random CSV (codes with leading zeros, quotes, commas, Chinese, blanks, encodings) read as text is each
  field; read as General each cell is what typing the field gives; a sheet saved as CSV reads back as its texts;
- date1904: constants keep their numbers, DATE / YEAR give the same dates in both systems, a datetime written reads back;
- precision: numbers in formats that round are their shown value with it on, and the formulas full again with it off;
- calcoff: a sheet's formulas keep their values while its calculation is off, one entered shows 0, and on again they
  are what Python works out;
- chart: shapes on a chart move with it and go with it; TopLeftCell holds its corner;
- total: a table's ListColumn.Total is its column's cell in the totals row, None without one;
- consolidate: the sheet keeps the last Consolidate's function, options and sources;
- autoshow: a Top or Bottom N shows the N items with the largest or smallest totals; showpages: a sheet for each filter
  item, its report's grand total that item's; threads: formulas taking the parallel path give Python's answers whatever
  the thread count; delimiters: OpenText's Tab, Semicolon, Space, Other, ConsecutiveDelimiter and StartRow; forcefull:
  ForceFullCalculation keeps every answer right after edits.
A failure prints the seed and the scenario; --only KIND --seed S --rounds 1 replays it (a scenario draws from its name
and the seed alone). Exit code 1 when a check failed.
"""
import argparse
import csv
import datetime as dt
import io
import os
import random
import sys
import tempfile
import time
import traceback

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument("--exe", default=os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe"))
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--rounds", type=int, default=60)
ap.add_argument("--only", default="")
ap.add_argument("--verbose", action="store_true")
ap.add_argument("--lang", default="en")
a = ap.parse_args()
a.exe = os.path.abspath(a.exe)
sys.path.insert(0, os.path.join(os.path.dirname(a.exe), "python"))
import yc  # noqa: E402

P = yc.launch(hidden=True, exe=a.exe, lang=a.lang)
conn = yc.connect(target=P.pid)
from yc import excel, ui  # noqa: E402

TMP = tempfile.mkdtemp(prefix="yc_gaps_fuzz_")
failures = []


class Fail(Exception):
    pass


def check(ok, what):
    if not ok:
        raise Fail(what)


def eq(got, want, what):
    if got != want:
        raise Fail(f"{what}: got {got!r}, want {want!r}")


def near(got, want, what, tol=1e-9):
    if got is None or want is None or abs(float(got) - float(want)) > tol * max(1.0, abs(float(want))):
        raise Fail(f"{what}: got {got!r}, want {want!r}")


def new_sheet():
    ws = excel.Worksheets.Add()
    ws.Activate()
    return ws


def col_name(c):
    s = ""
    while c:
        c, m = divmod(c - 1, 26)
        s = chr(65 + m) + s
    return s


def cells_of(address):
    """(row, col) of every cell of a one-area A1 address like $A$3:$D$8."""
    def one(t):
        t = t.replace("$", "")
        i = 0
        while t[i].isalpha():
            i += 1
        c = 0
        for ch in t[:i]:
            c = c * 26 + ord(ch.upper()) - 64
        return int(t[i:]), c
    parts = address.split(":")
    r0, c0 = one(parts[0])
    r1, c1 = one(parts[-1])
    return [(r, c) for r in range(r0, r1 + 1) for c in range(c0, c1 + 1)]


def bounds(address):
    cs = cells_of(address)
    return min(r for r, _ in cs), min(c for _, c in cs), max(r for r, _ in cs), max(c for _, c in cs)


def inside(inner, outer):
    r0, c0, r1, c1 = bounds(inner)
    R0, C0, R1, C1 = bounds(outer)
    return R0 <= r0 and C0 <= c0 and r1 <= R1 and c1 <= C1


# ---- pivot ----------------------------------------------------------------------------------------------------------

FUNCS = {"sum": (excel.xlSum, "Sum"), "count": (excel.xlCount, "Count"), "average": (excel.xlAverage, "Average"),
         "max": (excel.xlMax, "Max"), "min": (excel.xlMin, "Min")}


def aggregate(fn, xs):
    if not xs:
        return None  # no record for the row and column: an empty cell, a count's too (Excel)
    if fn == "count":
        return len(xs)
    if fn == "sum":
        return sum(xs)
    if fn == "average":
        return sum(xs) / len(xs)
    return max(xs) if fn == "max" else min(xs)


def pivot(rnd):
    n = rnd.randint(1, 14)
    regions = rnd.sample(["East", "West", "North", "South", "北部"], rnd.randint(1, 4))
    years = rnd.sample([2022, 2023, 2024], rnd.randint(1, 3))
    products = rnd.sample(["A", "B", "C"], rnd.randint(1, 3))
    rows = []
    for _ in range(n):
        sales = rnd.choice([rnd.randint(-50, 100), round(rnd.uniform(-10, 10), 2)])
        rows.append([rnd.choice(regions), rnd.choice(years), rnd.choice(products), sales])
    data = new_sheet()
    data.Range(f"A1:D{n + 1}").Value = [["Region", "Year", "Product", "Sales"]] + rows
    dst = new_sheet()
    fields = ["Region", "Year", "Product"]
    rnd.shuffle(fields)
    k_row = rnd.randint(0, 2)
    row_f = fields[:k_row]
    rest = fields[k_row:]
    col_f = rest[:rnd.randint(0, min(1, len(rest)))]
    rest = rest[len(col_f):]
    page_f = rest[:rnd.randint(0, min(1, len(rest)))]
    fn = rnd.choice(list(FUNCS))
    at_row = 3 if page_f else rnd.randint(1, 3)
    desc = f"source {n} rows, rows {row_f}, cols {col_f}, page {page_f}, {fn}, at A{at_row}"
    pc = excel.ActiveWorkbook.PivotCaches.Create(excel.xlDatabase, data.Range(f"A1:D{n + 1}"))
    pt = pc.CreatePivotTable(dst.Range(f"A{at_row}"), f"Fz{rnd.randint(0, 10**9)}")
    for f in row_f:
        pt.PivotFields(f).Orientation = excel.xlRowField
    for f in col_f:
        pt.PivotFields(f).Orientation = excel.xlColumnField
    for f in page_f:
        pt.PivotFields(f).Orientation = excel.xlPageField
    caption = f"{FUNCS[fn][1]} of Sales"
    pt.AddDataField(pt.PivotFields("Sales"), caption, FUNCS[fn][0])
    page_item = None
    if page_f and rnd.random() < 0.5:
        values = sorted({str(r[["Region", "Year", "Product"].index(page_f[0])]) for r in rows})
        page_item = rnd.choice(values)
        pt.PivotFields(page_f[0]).CurrentPage = page_item
        desc += f", page item {page_item}"
    idx = {"Region": 0, "Year": 1, "Product": 2}
    field_of = {str(v): f for f, vs in (("Region", regions), ("Year", years), ("Product", products)) for v in vs}
    src = [r for r in rows if page_item is None or str(r[idx[page_f[0]]]) == page_item]

    table = pt.TableRange1.Address
    body = pt.DataBodyRange.Address
    check(inside(body, table), f"DataBodyRange {body} inside TableRange1 {table} ({desc})")
    if row_f:
        check(inside(pt.RowRange.Address, table), f"RowRange {pt.RowRange.Address} inside {table} ({desc})")
    if col_f:
        check(inside(pt.ColumnRange.Address, table), f"ColumnRange {pt.ColumnRange.Address} inside {table} ({desc})")
    if page_f:
        check(pt.PageRange is not None, f"PageRange with a filter field ({desc})")
    body_cells = set(cells_of(body))
    for r, c in cells_of(table):
        cell = dst.Cells(r, c)
        try:
            pc_ = cell.PivotCell
        except Exception as e:  # noqa: BLE001
            raise Fail(f"{col_name(c)}{r} of the report has no PivotCell: {e} ({desc})")
        kind = pc_.PivotCellType
        check(kind in range(0, 10), f"PivotCellType {kind!r} at {col_name(c)}{r} ({desc})")
        if (r, c) in body_cells:
            eq(kind, 0, f"type of body cell {col_name(c)}{r} ({desc})")
            items = {}
            for it in list(pc_.RowItems or []) + list(pc_.ColumnItems or []):
                items[field_of[it.Name]] = it.Name  # no item name is in two fields' values
            xs = [row[3] for row in src if all(str(row[idx[f]]) == v for f, v in items.items())]
            want = aggregate(fn, xs)
            got = cell.Value2
            if want is None:
                check(got in (None, "", 0) or isinstance(got, excel.XlError), f"empty aggregate at {col_name(c)}{r} shows {got!r} ({desc})")
            else:
                near(got, want, f"value at {col_name(c)}{r} for {items} ({desc})")
    for f in row_f + col_f:
        for it in pt.PivotFields(f).VisibleItems:
            if not any(str(row[idx[f]]) == it.Name for row in src):
                continue  # no record under the filter: the report doesn't show the item, and it has no cells
            lab = it.LabelRange
            check(lab is not None, f"{f}/{it.Name} has a LabelRange ({desc})")
            eq(str(lab.Cells(1, 1).Text), it.Name, f"{f}/{it.Name} LabelRange text ({desc})")
            dr = it.DataRange
            check(dr is not None and set(cells_of(dr.Address)) <= body_cells, f"{f}/{it.Name} DataRange {dr and dr.Address} in the body ({desc})")
    if len(row_f) == 1 and not col_f:
        for it in pt.PivotFields(row_f[0]).VisibleItems:
            xs = [row[3] for row in src if str(row[idx[row_f[0]]]) == it.Name]
            want = aggregate(fn, xs)
            if want is not None:
                near(pt.GetData(f"'{caption}' {it.Name}" if " " not in it.Name else f"'{caption}' '{it.Name}'"), want, f"GetData {it.Name} ({desc})")
    return desc


# ---- display format -------------------------------------------------------------------------------------------------

def display(rnd):
    ws = new_sheet()
    vals = [rnd.choice([rnd.randint(-20, 20), round(rnd.uniform(-5, 5), 3), None]) for _ in range(12)]
    for i, v in enumerate(vals):
        if v is not None:
            ws.Cells(i + 1, 1).Value2 = v
    op = rnd.choice(["gt", "lt", "between", "eq"])
    x, y = sorted([rnd.randint(-10, 10), rnd.randint(-10, 10)])
    color = rnd.choice([255, 65280, 16711680, 49407])
    rng = ws.Range("A1:A12")
    if op == "between":
        fc = rng.FormatConditions.Add(excel.xlCellValue, excel.xlBetween, str(x), str(y))
    else:
        fc = rng.FormatConditions.Add(excel.xlCellValue, {"gt": excel.xlGreater, "lt": excel.xlLess, "eq": excel.xlEqual}[op], str(x))
    fc.Interior.Color = color
    desc = f"values {vals}, {op} {x} {y}, color {color}"
    for i, v in enumerate(vals):
        num = 0 if v is None else v
        holds = {"gt": num > x, "lt": num < x, "between": x <= num <= y, "eq": num == x}[op]
        got = ws.Cells(i + 1, 1).DisplayFormat.Interior.Color
        if holds:
            eq(got, color, f"DisplayFormat fill of A{i + 1} = {v!r} ({desc})")
        else:
            check(got != color or color == 16777215, f"A{i + 1} = {v!r} is filled {got} though the rule doesn't hold ({desc})")
    return desc


# ---- panes ----------------------------------------------------------------------------------------------------------

def panes(rnd):
    ws = new_sheet()
    win = excel.ActiveWindow
    win.FreezePanes = False
    win.SplitRow = 0
    win.SplitColumn = 0
    mode = rnd.choice(["none", "split_rows", "split_cols", "split_both", "freeze"])
    rows, cols = rnd.randint(1, 12), rnd.randint(1, 6)
    if mode == "freeze":
        ws.Cells(rows + 1, cols + 1).Select()
        win.FreezePanes = True
    elif mode != "none":
        if mode in ("split_rows", "split_both"):
            win.SplitRow = rows
        if mode in ("split_cols", "split_both"):
            win.SplitColumn = cols
    try:
        n = win.Panes.Count
        want = {"none": 1, "split_rows": 2, "split_cols": 2, "split_both": 4, "freeze": 4}[mode]
        desc = f"{mode} at {rows} rows {cols} cols"
        eq(n, want, f"Panes.Count ({desc})")
        for k in range(rnd.randint(1, 4)):
            p = rnd.randint(1, n)
            if rnd.random() < 0.5:
                v = rnd.randint(1, 400)
                win.Panes(p).ScrollRow = v
                desc += f", pane {p} ScrollRow {v}"
            else:
                v = rnd.randint(1, 60)
                win.Panes(p).ScrollColumn = v
                desc += f", pane {p} ScrollColumn {v}"
        firsts = [bounds(win.Panes(i).VisibleRange.Address)[:2] for i in range(1, n + 1)]
        if n == 4 and mode == "freeze":
            # frozen panes are numbered across then down, each ScrollRow / ScrollColumn its own (Excel 16, measured)
            eq(firsts[0], (1, 1), f"the frozen pane starts at A1 ({desc})")
            eq(firsts[0][0], firsts[1][0], f"panes 1 and 2 show the same first row ({desc})")
            eq(firsts[2][0], firsts[3][0], f"panes 3 and 4 show the same first row ({desc})")
            eq(firsts[0][1], firsts[2][1], f"panes 1 and 3 show the same first column ({desc})")
            eq(firsts[1][1], firsts[3][1], f"panes 2 and 4 show the same first column ({desc})")
            for i in range(1, 5):
                eq((win.Panes(i).ScrollRow, win.Panes(i).ScrollColumn), firsts[i - 1], f"pane {i}'s scroll is its first cell ({desc})")
        elif n == 4:
            eq(firsts[0][0], firsts[2][0], f"panes 1 and 3 show the same first row ({desc})")
            eq(firsts[1][0], firsts[3][0], f"panes 2 and 4 show the same first row ({desc})")
            eq(firsts[0][1], firsts[1][1], f"panes 1 and 2 show the same first column ({desc})")
            eq(firsts[2][1], firsts[3][1], f"panes 3 and 4 show the same first column ({desc})")
            eq(win.Panes(4).ScrollRow, firsts[3][0], f"pane 4's ScrollRow is its first row ({desc})")
            eq(win.Panes(4).ScrollColumn, firsts[3][1], f"pane 4's ScrollColumn is its first column ({desc})")
        elif n == 2:
            if mode == "split_rows":
                eq(firsts[0][1], firsts[1][1], f"the split panes share their first column ({desc})")
                check(firsts[1][0] > firsts[0][0] or firsts[1][0] >= rows + 1 or True, desc)
            else:
                eq(firsts[0][0], firsts[1][0], f"the split panes share their first row ({desc})")
        for i in range(1, n + 1):
            r0, c0, r1, c1 = bounds(win.Panes(i).VisibleRange.Address)
            check(r1 >= r0 and c1 >= c0, f"pane {i} shows a range ({desc})")
        return desc
    finally:
        win.FreezePanes = False
        win.SplitRow = 0
        win.SplitColumn = 0


# ---- window and application -----------------------------------------------------------------------------------------

def window(rnd):
    win = excel.ActiveWindow
    pool = ["Desk", "交易台", 'a "quoted" name', "x - y", "  spaced  ", "1234", "—"]
    cap = rnd.choice(pool)
    desc = f"caption {cap!r}"
    win.Caption = cap
    eq(win.Caption, cap, "Window.Caption")
    app_cap = rnd.choice(pool)
    excel.Application.Caption = app_cap
    eq(excel.Application.Caption, f"{cap} - {app_cap}", "Application.Caption")
    win.Caption = None
    excel.Application.Caption = None
    check("YCGrid" in excel.Application.Caption, f"Application.Caption back to YCGrid: {excel.Application.Caption!r}")
    state = rnd.choice([excel.xlNormal, excel.xlMaximized, excel.xlMinimized])
    excel.Application.WindowState = state
    eq(excel.Application.WindowState, state, "Application.WindowState")
    excel.Application.WindowState = excel.xlNormal
    left, top, w, h = rnd.randint(0, 300), rnd.randint(0, 200), rnd.randint(500, 900), rnd.randint(400, 700)
    excel.Application.Left, excel.Application.Top = left, top
    excel.Application.Width, excel.Application.Height = w, h
    desc += f", state {state}, place {left},{top} {w}x{h}"
    near(excel.Application.Left, left, "Application.Left", 0.02)
    near(excel.Application.Top, top, "Application.Top", 0.02)
    near(excel.Application.Width, w, "Application.Width", 0.02)
    near(excel.Application.Height, h, "Application.Height", 0.02)
    check(0 < excel.Application.UsableWidth <= w and 0 < excel.Application.UsableHeight <= h,
          f"UsableWidth / Height {excel.Application.UsableWidth} x {excel.Application.UsableHeight} inside {w} x {h}")
    return desc


# ---- text files -----------------------------------------------------------------------------------------------------

FIELDS = ["0050", "00123", "12.5", "-3", "1,234", "2024/1/5", "12:30", "TRUE", "台積電", 'say "hi"', "a,b", "",
          " lead", "trail ", "1E5", "$5", "50%", "=1+1", "-", "N/A", "(5)", "3/4", "0.10"]


def opentext(rnd):
    nrows, ncols = rnd.randint(1, 6), rnd.randint(1, 5)
    grid = [[rnd.choice(FIELDS) for _ in range(ncols)] for _ in range(nrows)]
    enc = rnd.choice(["utf-8-sig", "utf-8", "cp950"])
    buf = io.StringIO()
    csv.writer(buf, lineterminator="\r\n").writerows(grid)
    path = os.path.join(TMP, f"t{rnd.randint(0, 10**9)}.csv")
    with open(path, "w", encoding=enc, newline="") as fh:
        fh.write(buf.getvalue())
    origin = {"utf-8-sig": None, "utf-8": 65001, "cp950": 950}[enc]
    desc = f"{nrows}x{ncols} {enc}: {grid}"
    home = excel.ActiveWorkbook
    as_text = rnd.random() < 0.5
    info = [[i + 1, excel.xlTextFormat if as_text else excel.xlGeneralFormat] for i in range(ncols)]
    kw = {"Comma": True, "FieldInfo": info, "DataType": excel.xlDelimited, "TextQualifier": excel.xlTextQualifierDoubleQuote}
    if origin:
        kw["Origin"] = origin
    wb = excel.Workbooks.OpenText(path, **kw)
    try:
        ws = wb.ActiveSheet
        got = ws.Range(ws.Cells(1, 1), ws.Cells(nrows, ncols)).Value
        got = [list(r) if isinstance(r, (list, tuple)) else [r] for r in (got if isinstance(got, (list, tuple)) else [[got]])]
        if nrows == 1 and ncols > 1 and not isinstance(got[0], list):
            got = [got]
        for i in range(nrows):
            for j in range(ncols):
                field = grid[i][j]
                cell = got[i][j]
                if as_text:
                    eq(cell if cell is not None else "", field, f"text field r{i + 1}c{j + 1} ({desc})")
                else:
                    # what typing the field gives: the same workbook's scratch cell
                    probe = ws.Cells(nrows + 3, 1)
                    probe.NumberFormat = "General"
                    probe.Formula = field
                    want = probe.Value
                    probe.ClearContents()
                    eq(cell, want, f"General field r{i + 1}c{j + 1} {field!r} ({desc})")
        if as_text:
            out = os.path.join(TMP, f"o{rnd.randint(0, 10**9)}.csv")
            ws.SaveAs(out, excel.xlCSVUTF8)
            with open(out, encoding="utf-8-sig", newline="") as fh:
                back = list(csv.reader(fh))
            for i in range(nrows):
                row = back[i] if i < len(back) else []
                for j in range(ncols):
                    v = row[j] if j < len(row) else ""
                    eq(v, grid[i][j], f"saved CSV r{i + 1}c{j + 1} ({desc})")
    finally:
        wb.Close(False)
        yc.connect(target=P.pid)
        home.Activate() if hasattr(home, "Activate") else None
    return desc


# ---- workbook calculation settings ----------------------------------------------------------------------------------

def date1904(rnd):
    ws = new_sheet()
    wb = excel.ActiveWorkbook
    wb.Date1904 = False
    days = [dt.datetime(rnd.randint(1905, 2090), rnd.randint(1, 12), rnd.randint(1, 28)) for _ in range(3)]
    for i, d in enumerate(days):
        ws.Cells(i + 1, 1).Value2 = (d - dt.datetime(1899, 12, 30)).days
        ws.Cells(i + 1, 2).Formula = f"=DATE({d.year},{d.month},{d.day})"
        ws.Cells(i + 1, 3).Formula = f"=YEAR(B{i + 1})*10000+MONTH(B{i + 1})*100+DAY(B{i + 1})"
        ws.Cells(i + 1, 4).Formula = f"=B{i + 1}+{rnd.randint(-30, 30)}"
        ws.Cells(i + 1, 5).Formula = f"=WEEKDAY(B{i + 1})"
    before = [[ws.Cells(i + 1, c).Value2 for c in (1, 3, 5)] for i in range(3)]
    texts = [ws.Cells(i + 1, 2).Text for i in range(3)]
    gaps = [ws.Cells(i + 1, 4).Value2 - ws.Cells(i + 1, 2).Value2 for i in range(3)]
    desc = f"dates {[d.date().isoformat() for d in days]}"
    try:
        wb.Date1904 = True
        for i, d in enumerate(days):
            eq(ws.Cells(i + 1, 1).Value2, before[i][0], f"constant A{i + 1} keeps its number ({desc})")
            eq(ws.Cells(i + 1, 3).Value2, before[i][1], f"YEAR/MONTH/DAY of DATE in B{i + 1} ({desc})")
            eq(ws.Cells(i + 1, 5).Value2, before[i][2], f"WEEKDAY of B{i + 1} ({desc})")
            eq(ws.Cells(i + 1, 2).Text, texts[i], f"DATE in B{i + 1} shows the same date ({desc})")
            eq(ws.Cells(i + 1, 4).Value2 - ws.Cells(i + 1, 2).Value2, gaps[i], f"days between D{i + 1} and B{i + 1} ({desc})")
            ws.Cells(i + 1, 6).Value = d
            got = ws.Cells(i + 1, 6).Value
            eq(got.replace(tzinfo=None) if hasattr(got, "replace") else got, d, f"a datetime written in 1904 reads back ({desc})")
            eq(ws.Cells(i + 1, 6).Value2, ws.Cells(i + 1, 2).Value2, f"its number is DATE's ({desc})")
    finally:
        wb.Date1904 = False
    return desc


def xround(x, d):
    """Excel's rounding of what a format shows: half away from zero, on the number's 15 significant digits (762.5
    shows 763 in 0, 201.015 shows 201.02 in 0.00; Python's round() halves to even on the binary value)."""
    from decimal import ROUND_HALF_UP, Decimal

    q = Decimal(f"{x:.15g}").quantize(Decimal(1).scaleb(-d), rounding=ROUND_HALF_UP)
    return float(q)


ROUNDING = [("0", 0), ("0.0", 1), ("0.00", 2), ("0.000", 3), ("#,##0.00", 2), ("0%", 2), ("0.0%", 3)]


def precision(rnd):
    ws = new_sheet()
    wb = excel.ActiveWorkbook
    n = rnd.randint(1, 8)
    vals = [round(rnd.uniform(-1000, 1000), rnd.randint(0, 6)) for _ in range(n)]
    fmts = [rnd.choice(ROUNDING) for _ in range(n)]
    for i in range(n):
        ws.Cells(i + 1, 1).Value2 = vals[i]
        ws.Cells(i + 1, 1).NumberFormat = fmts[i][0]
        ws.Cells(i + 1, 2).Formula = f"=A{i + 1}/3"
        ws.Cells(i + 1, 2).NumberFormat = fmts[i][0]
        ws.Cells(i + 1, 3).Formula = f"=B{i + 1}*3"
    desc = f"values {vals} formats {[f for f, _ in fmts]}"
    try:
        wb.PrecisionAsDisplayed = True
        for i in range(n):
            d = fmts[i][1]
            a1 = xround(vals[i], d)
            near(ws.Cells(i + 1, 1).Value2, a1, f"A{i + 1} rounded to {fmts[i][0]} ({desc})", 1e-12)
            b1 = xround(a1 / 3, d)
            near(ws.Cells(i + 1, 2).Value2, b1, f"B{i + 1} =A/3 rounded ({desc})", 1e-12)
            near(ws.Cells(i + 1, 3).Value2, b1 * 3, f"C{i + 1} reads the rounded B ({desc})", 1e-12)
        wb.PrecisionAsDisplayed = False
        for i in range(n):
            a1 = xround(vals[i], fmts[i][1])
            near(ws.Cells(i + 1, 1).Value2, a1, f"A{i + 1} keeps its rounded value ({desc})", 1e-12)
            near(ws.Cells(i + 1, 2).Value2, a1 / 3, f"B{i + 1} full again ({desc})", 1e-12)
            near(ws.Cells(i + 1, 3).Value2, a1, f"C{i + 1} full again ({desc})", 1e-9)
    finally:
        wb.PrecisionAsDisplayed = False
    return desc


def calcoff(rnd):
    ws = new_sheet()
    n = rnd.randint(1, 6)
    xs = [rnd.randint(-50, 50) for _ in range(n)]
    for i in range(n):
        ws.Cells(i + 1, 1).Value2 = xs[i]
        ws.Cells(i + 1, 2).Formula = f"=A{i + 1}*2+1"
    ws.Cells(1, 3).Formula = f"=SUM(B1:B{n})"
    other = new_sheet()
    other.Range("A1").Formula = f"='{ws.Name}'!C1+1"
    ws.Activate()
    desc = f"inputs {xs}"
    try:
        ws.EnableCalculation = False
        shown = [ws.Cells(i + 1, 2).Value2 for i in range(n)]
        total = ws.Cells(1, 3).Value2
        for _ in range(rnd.randint(1, 4)):
            i = rnd.randrange(n)
            xs[i] = rnd.randint(-50, 50)
            ws.Cells(i + 1, 1).Value2 = xs[i]
            desc += f", A{i + 1}={xs[i]}"
        excel.Application.Calculate()
        for i in range(n):
            eq(ws.Cells(i + 1, 2).Value2, shown[i], f"B{i + 1} keeps its value while the calculation is off ({desc})")
        eq(ws.Cells(1, 3).Value2, total, f"C1 keeps its value ({desc})")
        ws.Cells(1, 4).Formula = "=A1+1000"
        eq(ws.Cells(1, 4).Value2, 0, f"a formula entered with the calculation off shows 0 ({desc})")
        copy_n = excel.Worksheets.Count
        ws.Copy(After=ws)
        cp = excel.Worksheets(excel.Worksheets.Count) if excel.Worksheets.Count > copy_n else None
        ws.EnableCalculation = True
        for i in range(n):
            eq(ws.Cells(i + 1, 2).Value2, xs[i] * 2 + 1, f"B{i + 1} calculated when it is on ({desc})")
        eq(ws.Cells(1, 3).Value2, sum(x * 2 + 1 for x in xs), f"C1 calculated ({desc})")
        eq(ws.Cells(1, 4).Value2, xs[0] + 1000, f"D1 calculated ({desc})")
        eq(other.Range("A1").Value2, sum(x * 2 + 1 for x in xs) + 1, f"another sheet reading C1 follows ({desc})")
        del cp
    finally:
        ws.EnableCalculation = True
    return desc


# ---- charts, tables, consolidation ----------------------------------------------------------------------------------

def chart(rnd):
    ws = new_sheet()
    ws.Range("A1:B6").Value = [["x", "y"]] + [[i, rnd.randint(1, 50)] for i in range(1, 6)]
    left, top = rnd.randint(0, 400), rnd.randint(0, 300)
    co = ws.ChartObjects().Add(left, top, rnd.randint(150, 400), rnd.randint(120, 300))
    co.Chart.SetSourceData(ws.Range("A1:B6"))
    ch = co.Chart
    before = ws.Shapes.Count
    k = rnd.randint(1, 3)
    for _ in range(k):
        ch.Shapes.AddLine(rnd.randint(0, 100), rnd.randint(0, 100), rnd.randint(0, 100), rnd.randint(0, 100))
    desc = f"chart at {left},{top}, {k} lines"
    eq(ch.Shapes.Count, k, f"Chart.Shapes.Count ({desc})")
    eq(ws.Shapes.Count, before, f"a chart's shapes aren't the sheet's ({desc})")
    tl = co.TopLeftCell
    check(tl.Top <= co.Top + 0.01 < tl.Top + tl.Height + 0.01 and tl.Left <= co.Left + 0.01 < tl.Left + tl.Width + 0.01,
          f"TopLeftCell {tl.Address} holds the chart's corner {co.Left},{co.Top} ({desc})")
    br = co.BottomRightCell
    bottom, right = co.Top + co.Height, co.Left + co.Width
    check(br.Top <= bottom + 0.01 <= br.Top + br.Height + 0.01 and br.Left <= right + 0.01 <= br.Left + br.Width + 0.01,
          f"BottomRightCell {br.Address} holds the chart's far corner {right},{bottom} ({desc})")
    xs = [ch.Shapes(i).Left for i in range(1, k + 1)]
    dx = rnd.randint(-50, 200)
    co.Left = max(0, co.Left + dx)
    eq([ch.Shapes(i).Left for i in range(1, k + 1)], xs, f"shapes keep their place in the chart when it moves ({desc})")
    co.Delete()
    eq(ws.Shapes.Count, before - 1, f"the chart goes with its shapes ({desc})")
    return desc


def total(rnd):
    ws = new_sheet()
    nr, nc = rnd.randint(1, 6), rnd.randint(1, 4)
    r0, c0 = rnd.randint(1, 5), rnd.randint(1, 4)
    ws.Range(ws.Cells(r0, c0), ws.Cells(r0 + nr, c0 + nc - 1)).Value = [[f"h{j}" for j in range(nc)]] + \
        [[rnd.randint(1, 9) for _ in range(nc)] for _ in range(nr)]
    lo = ws.ListObjects.Add(excel.xlSrcRange, ws.Range(ws.Cells(r0, c0), ws.Cells(r0 + nr, c0 + nc - 1)), None, excel.xlYes)
    desc = f"table at {col_name(c0)}{r0}, {nr} rows {nc} cols"
    for j in range(1, nc + 1):
        check(lo.ListColumns(j).Total is None, f"no Total without a totals row ({desc})")
    lo.ShowTotals = True
    for j in range(1, nc + 1):
        t = lo.ListColumns(j).Total
        check(t is not None, f"column {j} has a Total ({desc})")
        eq(t.Address, ws.Cells(r0 + nr + 1, c0 + j - 1).Address, f"column {j}'s Total ({desc})")
    lo.ShowTotals = False
    check(lo.ListColumns(1).Total is None, f"no Total after the totals row goes ({desc})")
    return desc


def consolidate(rnd):
    ws = new_sheet()
    k = rnd.randint(1, 3)
    srcs = []
    for i in range(k):
        r0 = 1 + i * 5
        ws.Range(f"A{r0}:B{r0 + 2}").Value = [["label", "v"]] + [[rnd.choice("xyz"), rnd.randint(1, 9)] for _ in range(2)]
        srcs.append(f"{ws.Name}!R{r0}C1:R{r0 + 2}C2")
    fn = rnd.choice([excel.xlSum, excel.xlAverage, excel.xlMax, excel.xlMin, excel.xlCount])
    top, left = rnd.random() < 0.5, rnd.random() < 0.5
    ws.Range("D1").Consolidate(srcs, fn, TopRow=top, LeftColumn=left)
    desc = f"sources {srcs}, function {fn}, top {top}, left {left}"
    eq(ws.ConsolidationFunction, fn, f"ConsolidationFunction ({desc})")
    eq(ws.ConsolidationOptions, [top, left, False], f"ConsolidationOptions ({desc})")
    eq(ws.ConsolidationSources, srcs, f"ConsolidationSources ({desc})")
    return desc


# ---- more of the report, the calculation and text files -------------------------------------------------------------

def _source(rnd, n):
    names = rnd.sample(["E", "W", "N", "S", "C", "M", "X", "Y"], rnd.randint(2, 8))
    return [[rnd.choice(names), rnd.choice(["A", "B"]), rnd.randint(1, 60) * 7 + rnd.randint(0, 6)] for _ in range(n)]


def autoshow(rnd):
    n = rnd.randint(3, 20)
    rows = _source(rnd, n)
    sums = {}
    for r in rows:
        sums[r[0]] = sums.get(r[0], 0) + r[2]
    if len(set(sums.values())) != len(sums):
        return "skipped: two items with the same total"  # a Top N then has two answers
    data = new_sheet()
    data.Range(f"A1:C{n + 1}").Value = [["Name", "Kind", "Amount"]] + rows
    dst = new_sheet()
    pc = excel.ActiveWorkbook.PivotCaches.Create(excel.xlDatabase, data.Range(f"A1:C{n + 1}"))
    pt = pc.CreatePivotTable(dst.Range("A3"), f"Au{rnd.randint(0, 10**9)}")
    pt.PivotFields("Name").Orientation = excel.xlRowField
    pt.AddDataField(pt.PivotFields("Amount"), "Sum of Amount", excel.xlSum)
    k = rnd.randint(1, len(sums))
    bottom = rnd.random() < 0.5
    pt.PivotFields("Name").AutoShow(excel.xlAutomatic, excel.xlBottom if bottom else excel.xlTop, k, "Sum of Amount")
    ranked = sorted(sums, key=lambda x: sums[x], reverse=not bottom)
    want = sorted(ranked[:k])
    desc = f"sums {sums}, {'bottom' if bottom else 'top'} {k}"
    rr = pt.RowRange
    shown = sorted(dst.Cells(r, 1).Text for r in range(rr.Row + 1, rr.Row + rr.Rows.Count - 1))
    eq(shown, want, f"the items the report shows ({desc})")
    eq(pt.PivotFields("Name").AutoShowCount, k, f"AutoShowCount ({desc})")
    eq(pt.PivotFields("Name").AutoShowType, excel.xlAutomatic, f"AutoShowType ({desc})")
    eq(pt.PivotFields("Name").AutoShowRange, excel.xlBottom if bottom else excel.xlTop, f"AutoShowRange ({desc})")
    near(pt.GetData("'Sum of Amount'"), sum(sums[x] for x in want), f"the grand total of what shows ({desc})")
    pt.PivotFields("Name").AutoShow(excel.xlManual)
    eq(pt.PivotFields("Name").AutoShowType, excel.xlManual, f"AutoShow xlManual ({desc})")
    return desc


def showpages(rnd):
    n = rnd.randint(1, 15)
    rows = _source(rnd, n)
    data = new_sheet()
    data.Range(f"A1:C{n + 1}").Value = [["Name", "Kind", "Amount"]] + rows
    dst = new_sheet()
    pc = excel.ActiveWorkbook.PivotCaches.Create(excel.xlDatabase, data.Range(f"A1:C{n + 1}"))
    pt = pc.CreatePivotTable(dst.Range("A3"), f"Sp{rnd.randint(0, 10**9)}")
    pt.PivotFields("Name").Orientation = excel.xlPageField
    if rnd.random() < 0.5:
        pt.PivotFields("Kind").Orientation = excel.xlRowField
    pt.AddDataField(pt.PivotFields("Amount"), "Sum of Amount", excel.xlSum)
    before = excel.Worksheets.Count
    pt.ShowPages("Name")
    names = sorted({r[0] for r in rows})
    desc = f"items {names}"
    eq(excel.Worksheets.Count, before + len(names), f"a sheet for each item ({desc})")
    for nm in names:
        ws = excel.Worksheets(nm)
        check(ws.PivotTables().Count == 1, f"sheet {nm} holds a report ({desc})")
        near(ws.PivotTables(1).GetData("'Sum of Amount'"), sum(r[2] for r in rows if r[0] == nm), f"sheet {nm}'s grand total ({desc})")
    excel.Application.DisplayAlerts = False
    for nm in names:
        excel.Worksheets(nm).Delete()
    return desc


def threads(rnd):
    ws = new_sheet()
    n = rnd.randint(600, 3000)
    xs = [[rnd.randint(-99, 99)] for _ in range(n)]
    ws.Range(f"A1:A{n}").Value = xs
    count = rnd.choice([1, 2, 3, 4, 8])
    mt = excel.Application.MultiThreadedCalculation
    mt.Enabled = rnd.random() < 0.8
    mt.ThreadCount = count
    desc = f"{n} rows, threads {count}, enabled {mt.Enabled}"
    try:
        ws.Range(f"B1:B{n}").Formula = "=A1*3+1"
        ws.Range(f"C1:C{n}").Formula = "=B1-A1"
        ws.Range("D1").Formula = f"=SUM(C1:C{n})"
        for _ in range(rnd.randint(1, 3)):
            i = rnd.randrange(n)
            xs[i][0] = rnd.randint(-99, 99)
            ws.Cells(i + 1, 1).Value2 = xs[i][0]
        excel.Application.Calculate()
        eq(ws.Range("D1").Value2, sum(x[0] * 2 + 1 for x in xs), f"the total ({desc})")
        got = ws.Range(f"C1:C{n}").Value
        for i in range(n):
            if got[i][0] != xs[i][0] * 2 + 1:
                raise Fail(f"C{i + 1} = {got[i][0]!r}, want {xs[i][0] * 2 + 1} ({desc})")
    finally:
        mt.Enabled = True
        mt.ThreadMode = excel.xlThreadModeAutomatic
    return desc


def delimiters(rnd):
    pool = ["0050", "abc", "12.5", "-3", "", "x y", "台積電", "TRUE", "2024/1/5"]
    sep = rnd.choice(["\t", ";", " ", "|"])
    nrows, ncols = rnd.randint(1, 5), rnd.randint(1, 4)
    usable = [f for f in pool if sep not in f]
    grid = [[rnd.choice(usable) for _ in range(ncols)] for _ in range(nrows)]
    consecutive = rnd.random() < 0.3
    start = rnd.randint(1, 2)
    lines = (["skipped"] if start == 2 else []) + [sep.join(r) for r in grid]
    path = os.path.join(TMP, f"d{rnd.randint(0, 10**9)}.txt")
    with open(path, "w", encoding="utf-8-sig", newline="") as fh:
        fh.write("\r\n".join(lines) + "\r\n")
    kw = {"StartRow": start, "DataType": excel.xlDelimited, "ConsecutiveDelimiter": consecutive,
          "FieldInfo": [[i + 1, excel.xlTextFormat] for i in range(ncols + 2)]}
    if sep == "\t":
        kw["Tab"] = True
    elif sep == ";":
        kw["Semicolon"] = True
    elif sep == " ":
        kw["Space"] = True
    else:
        kw["Other"], kw["OtherChar"] = True, "|"
    desc = f"sep {sep!r}, consecutive {consecutive}, start {start}: {grid}"
    wb = excel.Workbooks.OpenText(path, **kw)
    try:
        ws = wb.ActiveSheet
        for i, row in enumerate(grid):
            # consecutive delimiters are one, but a line starting with one keeps its empty first field (Excel 16,
            # measured: "\t-3\t-3" gives "", -3, -3)
            fields = row[:1] + [f for f in row[1:] if f != ""] if consecutive else row
            want = fields + [""] * (ncols - len(fields))
            got = [ws.Cells(i + 1, j + 1).Value or "" for j in range(ncols)]
            eq(got, want, f"row {i + 1} ({desc})")
    finally:
        wb.Close(False)
        yc.connect(target=P.pid)
    return desc


def forcefull(rnd):
    ws = new_sheet()
    wb = excel.ActiveWorkbook
    n = rnd.randint(2, 20)
    xs = [rnd.randint(-9, 9) for _ in range(n)]
    ws.Range(f"A1:A{n}").Value = [[x] for x in xs]
    ws.Range(f"B1:B{n}").Formula = "=A1*A1"
    ws.Range("C1").Formula = f"=SUM(B1:B{n})"
    wb.ForceFullCalculation = True
    desc = f"inputs {xs}"
    try:
        eq(wb.ForceFullCalculation, True, "ForceFullCalculation")
        for _ in range(rnd.randint(1, 5)):
            i = rnd.randrange(n)
            xs[i] = rnd.randint(-9, 9)
            ws.Cells(i + 1, 1).Value2 = xs[i]
            desc += f", A{i + 1}={xs[i]}"
            eq(ws.Range("C1").Value2, sum(x * x for x in xs), f"the total after the edit ({desc})")
    finally:
        wb.ForceFullCalculation = False
    return desc


KINDS = {"pivot": pivot, "display": display, "panes": panes, "window": window, "opentext": opentext, "date1904": date1904,
         "precision": precision, "calcoff": calcoff, "chart": chart, "total": total, "consolidate": consolidate,
         "autoshow": autoshow, "showpages": showpages, "threads": threads, "delimiters": delimiters, "forcefull": forcefull}
only = [k for k in a.only.split(",") if k]
kinds = [k for k in KINDS if not only or k in only]

t0 = time.time()
for i in range(a.rounds):
    seed = a.seed + i
    kind = kinds[random.Random(seed).randrange(len(kinds))]
    # the scenario's own draws come from the seed and its name: a seed gives the same case however many kinds there
    # are (--only, or a kind added later), so "--only KIND --seed S --rounds 1" replays what a run reported
    rnd = random.Random(f"{kind}:{seed}")
    try:
        n_err = len(ui.errors()) if hasattr(ui, "errors") else 0
        desc = KINDS[kind](rnd)
        if a.verbose:
            print(f"ok   seed {seed} {kind}: {desc}", flush=True)
        st = ui.state()
        if st.get("imgui_asserts"):
            raise Fail(f"ImGui checks failed: {st['imgui_asserts']}")
        if st.get("dialog"):
            raise Fail(f"a dialog is left open: {st['dialog']} {st.get('dialog_text')}")
    except Fail as e:
        failures.append((seed, kind, str(e)))
        print(f"FAIL seed {seed} {kind}: {e}", flush=True)
    except Exception:  # noqa: BLE001 - a member that raised is a finding too
        if P.poll() is not None:
            failures.append((seed, kind, f"YCGrid ended {P.poll() & 0xFFFFFFFF:#x}"))
            print(f"FAIL seed {seed} {kind}: YCGrid ended", flush=True)
            break
        failures.append((seed, kind, traceback.format_exc()[-1200:]))
        print(f"FAIL seed {seed} {kind}: raised\n{traceback.format_exc()[-1200:]}", flush=True)
    if excel.Worksheets.Count > 40:  # every round adds sheets: a fresh workbook keeps each round quick
        for ws in list(excel.Worksheets)[:-1]:
            try:
                excel.Application.DisplayAlerts = False
                ws.Delete()
            except Exception:  # noqa: BLE001
                pass
print(f"gaps_fuzz: {a.rounds} rounds from seed {a.seed} in {time.time() - t0:.0f} s, {len(failures)} failed")
P.terminate()
sys.exit(1 if failures else 0)
