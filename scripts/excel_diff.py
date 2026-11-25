# SPDX-License-Identifier: Apache-2.0
r"""The same object-model calls in a hidden Excel of the script's own and in a hidden YCGrid, compared.

    <YCGrid>\runtime\python\python.exe -E -s scripts\excel_diff.py excel  OUT.json [--state S] [--ranges R|R] [--ops calls|reads]
    <YCGrid>\runtime\python\python.exe -E -s scripts\excel_diff.py ycgrid OUT.json [--state S] [--ranges R|R] [--ops calls|reads]
    <YCGrid>\runtime\python\python.exe -E -s scripts\excel_diff.py compare EXCEL.json YCGRID.json

Each call runs on a sheet of its own holding a small list (A1:E6 with a formula column), set up in a state (plain,
table, filter, merged, protected, arrays, hidden), over each range given ("A1|B2:C3|B:B|A1:B2,D4:E5"). What is
recorded: whether the call raised, what it returned, and the formulas of A1:J12 after it. `compare` prints every
difference; the ones that matter are a call one side refuses, cells only one side changed, or a value read
differently. Expect noise to set aside: Excel's methods return True where YCGrid returns None, and a Chinese Excel's
regional defaults (row height 17, Calibri 11 vs PMingLiU 12, sheet names, NumberFormat in the UI's language), listed in
docs/excel-parity-backlog.md.

The Excel side needs comtypes (pure Python): install it into a folder, `pip install comtypes --target <dir>`, and give
the folder with --comtypes. The Excel is started hidden by the script and killed by its own process id at the end; it
uses no clipboard (Copy and Cut go to a Destination). Measured on 2026-11-20 this found 40-odd differences in ranges of
several areas, single cells, filtered lists, tables, merged cells, protected sheets and arrays (docs/random-findings.csv,
"excel_diff").
"""
from __future__ import annotations

import argparse
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "python"))

DATA = [["Name", "Qty", "Price", "Total", "When"], ["AAPL", 10, 1.5, None, 45000], ["MSFT", 20, 2.25, None, 45001],
        ["TSMC", 30, 3, None, 45002], ["NVDA", 40, 4.75, None, 45003], ["AMZN", 50, 5, None, 45004]]

CALLS = {
    "ClearContents": lambda ws, r: r.ClearContents(),
    "Clear": lambda ws, r: r.Clear(),
    "Value=5": lambda ws, r: setattr(r, "Value2", 5),
    "Formula=ROW()": lambda ws, r: setattr(r, "Formula", "=ROW()*10+COLUMN()"),
    "Formula rel": lambda ws, r: setattr(r, "Formula", "=A1"),
    "FillDown": lambda ws, r: r.FillDown(),
    "FillRight": lambda ws, r: r.FillRight(),
    "FillUp": lambda ws, r: r.FillUp(),
    "FillLeft": lambda ws, r: r.FillLeft(),
    "Merge": lambda ws, r: r.Merge(),
    "Delete": lambda ws, r: r.Delete(),
    "Delete up": lambda ws, r: r.Delete(-4162),
    "Delete left": lambda ws, r: r.Delete(-4159),
    "Insert": lambda ws, r: r.Insert(),
    "Insert right": lambda ws, r: r.Insert(-4161),
    "Insert down": lambda ws, r: r.Insert(-4121),
    "EntireRow.Delete": lambda ws, r: r.EntireRow.Delete(),
    "EntireColumn.Insert": lambda ws, r: r.EntireColumn.Insert(),
    "Replace": lambda ws, r: r.Replace("A", "#", 2),
    "Sort": lambda ws, r: r.Sort(r.Cells(1, 1)),
    "AutoFilter": lambda ws, r: r.AutoFilter(),
    "Copy to H1": lambda ws, r: r.Copy(ws.Range("H1")),
    "Cut to H1": lambda ws, r: r.Cut(ws.Range("H1")),
    "Calculate": lambda ws, r: r.Calculate(),
    "Address": lambda ws, r: r.Address(),
    "Count": lambda ws, r: r.Count,
    "Rows.Count": lambda ws, r: r.Rows.Count,
    "Columns.Count": lambda ws, r: r.Columns.Count,
    "Areas.Count": lambda ws, r: r.Areas.Count,
    "Value read": lambda ws, r: r.Value2,
    "Text read": lambda ws, r: r.Text,
    "Row": lambda ws, r: r.Row,
    "Column": lambda ws, r: r.Column,
    "EntireRow.Address": lambda ws, r: r.EntireRow.Address(),
    "Offset.Address": lambda ws, r: r.Offset(1, 1).Address(),
    "Resize.Address": lambda ws, r: r.Resize(2, 2).Address(),
    "CurrentRegion": lambda ws, r: r.CurrentRegion.Address(),
    "SpecialCells const": lambda ws, r: r.SpecialCells(2).Address(),
    "SpecialCells blanks": lambda ws, r: r.SpecialCells(4).Address(),
    "Cells(3).Address": lambda ws, r: r.Cells(3).Address(),
    "Item(2,2)": lambda ws, r: r.Item(2, 2).Address(),
    "HasFormula": lambda ws, r: r.HasFormula,
    "Bold=True": lambda ws, r: setattr(r.Font, "Bold", True),
    "Locked=False": lambda ws, r: setattr(r, "Locked", False),
    "Rows.Group": lambda ws, r: r.Rows.Group(),
    "Columns.Group": lambda ws, r: r.Columns.Group(),
    "Columns.AutoFit": lambda ws, r: r.Columns.AutoFit(),
    "Validation.Add": lambda ws, r: r.Validation.Add(1, 1, 1, "0", "10"),
    "FormatConditions.Add": lambda ws, r: r.FormatConditions.Add(1, 5, "=2"),
    "AddComment": lambda ws, r: r.AddComment("n"),
    "BorderAround": lambda ws, r: r.BorderAround(1, 2),
    "Select+Selection": lambda ws, r: (r.Select(), ws.Application.Selection.Address())[1],
}

READS = {
    "Address rel": lambda ws, r: r.Address(False, False),
    "Address R1C1": lambda ws, r: r.Address(True, True, -4150),
    "Address R1C1 rel": lambda ws, r: r.Address(False, False, -4150, False, ws.Range("C3")),
    "FormulaR1C1 read": lambda ws, r: r.Cells(1, 1).FormulaR1C1,
    "End down": lambda ws, r: r.End(-4121).Address(),
    "End right": lambda ws, r: r.End(-4161).Address(),
    "End up": lambda ws, r: r.End(-4162).Address(),
    "End left": lambda ws, r: r.End(-4159).Address(),
    "MergeCells": lambda ws, r: r.MergeCells,
    "HasArray": lambda ws, r: r.HasArray,
    "Hyperlinks.Count": lambda ws, r: r.Hyperlinks.Count,
    "HorizontalAlignment": lambda ws, r: r.HorizontalAlignment,
    "WrapText": lambda ws, r: r.WrapText,
    "Font.Bold": lambda ws, r: r.Font.Bold,
    "Borders LineStyle": lambda ws, r: r.Borders(9).LineStyle,
    "Locked": lambda ws, r: r.Locked,
    "Cells(2,2)": lambda ws, r: r.Cells(2, 2).Address(),
    "Range(A1:B2) rel": lambda ws, r: r.Range("A1:B2").Address(),
    "Next": lambda ws, r: r.Next.Address(),
    "EntireColumn": lambda ws, r: r.EntireColumn.Address(),
    "Value2 of first": lambda ws, r: r.Cells(1, 1).Value2,
    "Text of first": lambda ws, r: r.Cells(1, 1).Text,
    "PrefixCharacter": lambda ws, r: r.Cells(1, 1).PrefixCharacter,
    "Dependents": lambda ws, r: r.Cells(1, 1).Dependents.Address(),
    "DirectPrecedents": lambda ws, r: r.Cells(1, 1).DirectPrecedents.Address(),
    "CountLarge": lambda ws, r: r.CountLarge,
    "Areas(1)": lambda ws, r: r.Areas(1).Address(),
}
# reads whose answer for a whole column or row is a million values: left out there
BIG_READS = {"Value read", "Text read", "HasFormula"}


def norm(v):
    if v is None or isinstance(v, (bool, str)):
        return v
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, (list, tuple)):
        return [norm(x) for x in v]
    try:
        return [norm(x) for x in list(v)]
    except Exception:  # noqa: BLE001
        return "<object>"


def setup(ws, side, state, missing):
    if side == "ycgrid":
        ws.Range("A1:E6").Value2 = DATA
    else:  # comtypes takes no nested tuple as a 2-D array
        for ri, row in enumerate(DATA):
            for ci, v in enumerate(row):
                if v is not None:
                    ws.Cells(ri + 1, ci + 1).Value2 = v
    ws.Range("D2:D6").Formula = "=B2*C2"
    if state == "table":
        ws.ListObjects.Add(1, ws.Range("A1:E6"), missing, 1)
    elif state == "filter":
        ws.Range("A1:E6").AutoFilter(2, ">15")
    elif state == "merged":
        ws.Range("B3:C4").Merge()
    elif state == "protected":
        ws.Range("B:D").Locked = False
        ws.Protect()
    elif state == "arrays":
        ws.Range("G2:G4").FormulaArray = "=B2:B4*2"
        ws.Range("I2").Formula2 = "=SEQUENCE(3)"
    elif state == "hidden":
        ws.Rows(3).Hidden = True
        ws.Columns(3).Hidden = True


def run(side, out, state, ranges, ops, comtypes_dir, exe):
    res = {}
    if side == "excel":
        import ctypes

        if comtypes_dir:
            sys.path.insert(0, comtypes_dir)
        import comtypes.client
        from comtypes.automation import VARIANT

        missing = VARIANT.missing
        xl = comtypes.client.CreateObject("Excel.Application", dynamic=True)
        xl.Visible = False
        xl.DisplayAlerts = False
        pid = ctypes.c_ulong()
        ctypes.windll.user32.GetWindowThreadProcessId(ctypes.c_void_p(xl.Hwnd), ctypes.byref(pid))
        wb = xl.Workbooks.Add()
    else:
        import yc

        missing = None
        p = yc.launch(hidden=True, exe=exe, lang="en")
        yc.connect(target=p.pid)
        from yc import excel

        wb = excel.ActiveWorkbook
    try:
        for name, op in ops.items():
            for addr in ranges:
                whole = ":" in addr and addr.replace(":", "").replace(",", "").isalnum() and (addr.replace(":", "").isalpha() or addr.replace(":", "").isdigit())
                if whole and name in BIG_READS:
                    continue
                ws = wb.Worksheets.Add()
                setup(ws, side, state, missing)
                try:
                    ret, status = op(ws, ws.Range(addr)), "ok"
                except Exception:  # noqa: BLE001
                    ret, status = None, "error"
                try:
                    cells = norm(ws.Range("A1:J12").Formula)
                except Exception as e:  # noqa: BLE001
                    cells = "dump failed " + str(e)[:80]
                keep = not callable(ret) or hasattr(ret, "__iter__")  # YCGrid's VArray is callable and a value
                res[f"{name} | {addr}"] = {"status": status, "ret": norm(ret) if keep else None, "cells": cells}
                try:
                    if side == "excel":
                        ws.Unprotect()
                    ws.Delete()
                except Exception:  # noqa: BLE001
                    pass
            print(name, flush=True)
    finally:
        if side == "excel":
            try:
                wb.Close(False)
                xl.Quit()
            except Exception:  # noqa: BLE001
                pass
            os.system(f"taskkill /F /PID {pid.value} >nul 2>&1")
        else:
            p.kill()
    with open(out, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=0, default=str)


def compare(a, b):
    with open(a, encoding="utf-8") as f:
        ex = json.load(f)
    with open(b, encoding="utf-8") as f:
        yc_ = json.load(f)
    diffs = 0
    for k, e in ex.items():
        y = yc_.get(k)
        if y is None:
            continue
        why = []
        if e["status"] != y["status"]:
            why.append(f"status excel={e['status']} yc={y['status']}")
        elif e["status"] == "ok" and e["ret"] != y["ret"] and not (e["ret"] is True and y["ret"] is None):
            why.append(f"returned excel={str(e['ret'])[:80]} yc={str(y['ret'])[:80]}")
        if e["status"] == y["status"] and isinstance(e["cells"], list) and isinstance(y["cells"], list):
            cells = []
            for ri in range(min(len(e["cells"]), len(y["cells"]))):
                for ci in range(min(len(e["cells"][ri]), len(y["cells"][ri]))):
                    x, z = e["cells"][ri][ci], y["cells"][ri][ci]
                    if x != z and not (x in (None, "") and z in (None, "")):
                        cells.append(f"{chr(65 + ci)}{ri + 1}: excel={x!r} yc={z!r}")
            if cells:
                why.append("cells " + "; ".join(cells[:6]))
        if why:
            diffs += 1
            print(k, "::", " / ".join(why))
    print("differences:", diffs, "of", len(ex))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("side", choices=("excel", "ycgrid", "compare"))
    ap.add_argument("files", nargs="+")
    ap.add_argument("--state", default="", help="plain (default), table, filter, merged, protected, arrays, hidden")
    ap.add_argument("--ranges", default="A1|B2:C3|B:B|2:3|A1:B2,D4:E5", help="ranges separated by |")
    ap.add_argument("--ops", default="calls", choices=("calls", "reads"))
    ap.add_argument("--comtypes", default="", help="the folder comtypes was installed into (pip install comtypes --target)")
    ap.add_argument("--exe", default=os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    a = ap.parse_args()
    if a.side == "compare":
        compare(a.files[0], a.files[1])
    else:
        run(a.side, a.files[0], a.state, a.ranges.split("|"), CALLS if a.ops == "calls" else READS, a.comtypes, a.exe)


if __name__ == "__main__":
    main()
