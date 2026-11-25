# SPDX-License-Identifier: Apache-2.0
"""Basic operations measured in Excel: the cases that scripts/measure_excel_basics.py runs in a hidden Excel and
python/tests/test_excel_basics.py replays in a hidden YCGrid.

Each case starts from a fresh sheet named T, writes a start layout (cells, number formats, defined names, cells on the
sheet named Other that point at T), runs a few steps through Excel's object model and reads a fixed window back
(Formula in en-US, Value2, NumberFormat in en-US, MergeCells for the merge cases, the RefersTo of the names).

A step is one of:
  ("call", "B3:C3", "Insert", {"Shift": -4121})   Range("B3:C3").Insert(Shift:=xlShiftDown); a value {"range": "D1"}
                                                   is the Range D1 of T ("Other!D1" for the sheet Other)
  ("set", "A1", "Formula", "=1+1")                Range("A1").Formula = "=1+1" (Formula, Value, Value2, NumberFormat)

Every case ends with Application.CutCopyMode = False. Ranges without a sheet are on T.
"""
from __future__ import annotations

import hashlib
import json
import re

# Excel constants (the values of Excel's object model)
xlShiftDown, xlShiftToRight, xlShiftUp, xlShiftToLeft = -4121, -4161, -4162, -4159
xlPasteAll, xlPasteValues, xlPasteFormats, xlPasteFormulas = -4104, -4163, -4122, -4123
xlNone, xlAdd, xlSubtract, xlMultiply, xlDivide = -4142, 2, 3, 4, 5
(xlFillDefault, xlFillCopy, xlFillSeries, xlFillFormats, xlFillValues, xlFillDays, xlFillWeekdays, xlFillMonths, xlFillYears,
 xlLinearTrend, xlGrowthTrend) = range(11)
xlAscending, xlDescending = 1, 2
xlYes, xlNo, xlGuess = 1, 2, 0

FILL_TYPE_NAMES = {0: "default", 1: "copy", 2: "series", 3: "formats", 4: "values", 5: "days", 6: "weekdays", 7: "months",
                   8: "years", 9: "linear", 10: "growth"}

# The sheet Other holds these in every run (written once, before the cases).
OTHER_STATIC = {"A1": "100", "B1": "200", "A2": "other"}
# Cells of Other that cases write (cleared before each case).
OTHER_CLEAR = "D1:H20"

# A cell is "formula text" (typed with Range.Formula) or ("formula text", "number format"); the format is set first.
LAYOUTS = {
    "base": {
        "cells": {
            "A1": "Item", "B1": "Qty", "C1": "Price", "D1": "Total", "E1": "Calc", "F1": "Misc", "G1": "Ref",
            "A2": "Apple", "B2": "10", "C2": ("1.5", "0.00"), "D2": "=B2*C2", "E2": "=$B$2+B2", "F2": '=A2&"!"', "G2": "=C8*2",
            "A3": "Pear", "B3": "20", "C3": ("2.25", "0.00"), "D3": "=B3*C3", "E3": "=$B3*C$2", "F3": ("0.25", "0.00%"),
            "G3": "=SUM(B2:C3)",
            "C4": ("3", "0.00"), "D4": "=B4*C4", "E4": "=SUM(B:B)", "F4": ("1234.5", "#,##0.00"), "G4": "=D6-D2",
            "A5": "Fig", "B5": "40", "C5": ("4", "0.00"), "D5": "=B5*C5", "E5": "=Other!A1+B5", "F5": ("45000", "yyyy-mm-dd"),
            "G5": "=SUM(2:2)",
            "A6": "Total", "B6": "=SUM(B2:B5)", "C6": "=AVERAGE(C2:C5)", "D6": "=SUM(D2:D5)", "E6": "=Rate*2", "F6": "TRUE",
            "G6": "=COUNTA(A1:A6)",
            "A8": "Note", "B8": "=B2+B3", "C8": "=SUM(B2:D3)", "D8": "=$D$6", "E8": "=INDEX(B2:B5,2)", "F8": "=Rate",
        },
        "names": {"Rate": "=T!$B$2"},
        "other": {"D1": "=SUM(T!B2:B4)", "D2": "=T!C3"},
        "read": ["A1:J14", "Other!D1:D2"],
    },
    "merge": {
        "cells": {"A1": "x", "B1": ("5", "0.00"), "C1": "z", "B2": "y", "C2": ("7", "0%"), "A3": "=B1*2", "D1": "=B1",
                  "D2": "=SUM(A1:C2)"},
        "read": ["A1:E5"],
        "merge": True,
    },
    "sort": {
        "cells": {
            "A1": "Name", "B1": "Score", "C1": "Group", "D1": "Calc", "E1": "Abs", "F1": "100", "G1": "=B2",
            "A2": "bob", "B2": "30", "C2": "x", "D2": "=B2*2", "E2": "=$F$1+B2",
            "A3": "Alice", "B3": "10", "C3": "y", "D3": "=B3*2", "E3": "=$F$1+B3",
            "A4": "carol", "C4": "x", "D4": "=B4*2", "E4": "=$F$1+B4",
            "A5": "alice", "B5": "20", "C5": "y", "D5": "=B5*2", "E5": "=$F$1+B5",
            "A6": "Dave", "B6": "n/a", "C6": "x", "D6": "=B6*2", "E6": "=$F$1+B6",
            "A7": "10", "B7": "5", "C7": "y", "D7": "=B7*2", "E7": "=$F$1+B7",
            "A8": "TRUE", "B8": "15", "C8": "x", "D8": "=B8*2", "E8": "=$F$1+B8",
        },
        "read": ["A1:G9"],
    },
    "blank": {"cells": {}, "read": ["A1:B2"]},
    # an array entered with Ctrl+Shift+Enter (A8:B10, set by the cases' first step) over numbers
    "cse": {
        "cells": {"A1": "1", "B1": "2", "A2": "3", "B2": "4", "A3": "5", "B3": "6", "C1": "7", "D1": "8", "C2": "9", "D2": "10"},
        "read": ["A1:G14"],
    },
}


def R(addr):
    """A Range argument."""
    return {"range": addr}


CASES: list[dict] = []
_ids: set[str] = set()


def add(cid, steps, layout="base", cells=None, read=None, skip=(), note=None):
    """One case. cells / read override the layout's; skip lists what isn't compared ("numfmt": the code a typed date
    gets is the region's short date)."""
    assert cid not in _ids, cid
    _ids.add(cid)
    c = {"id": cid, "layout": layout, "steps": [list(s) for s in steps]}
    if cells is not None:
        c["cells"] = cells
    if read is not None:
        c["read"] = read
    if skip:
        c["skip"] = list(skip)
    if note:
        c["note"] = note
    CASES.append(c)


def call(target, method, **kw):
    return ("call", target, method, kw)


def setp(target, prop, value):
    return ("set", target, prop, value)


# ---- Rows and columns: Insert / Delete -------------------------------------------------------------------------
for rows in ("1:1", "2:2", "3:3", "4:4", "5:5", "6:6", "7:7", "8:8", "9:9", "2:3", "3:5", "1:6"):
    add(f"rows_insert_{rows.replace(':', '_')}", [call(rows, "Insert")])
    add(f"rows_delete_{rows.replace(':', '_')}", [call(rows, "Delete")])
for cols in ("A:A", "B:B", "C:C", "D:D", "E:E", "G:G", "I:I", "B:C", "A:D"):
    add(f"cols_insert_{cols.replace(':', '_')}", [call(cols, "Insert")])
    add(f"cols_delete_{cols.replace(':', '_')}", [call(cols, "Delete")])

# ---- Cells: Insert / Delete with a shift ----------------------------------------------------------------------
for rng in ("B3", "B3:C3", "A2:G2", "B2:B5", "C4:D5", "B7", "B6", "A1:H1", "E2:E3"):
    add(f"insert_down_{rng.replace(':', '_')}", [call(rng, "Insert", Shift=xlShiftDown)])
for rng in ("B3", "B2:B3", "A4:A6", "C1:C8", "B2:C6", "H2"):
    add(f"insert_right_{rng.replace(':', '_')}", [call(rng, "Insert", Shift=xlShiftToRight)])
for rng in ("B3", "B3:C3", "B2:B5", "B6", "D2:D3", "B2", "C8", "A1:G1", "B4"):
    add(f"delete_up_{rng.replace(':', '_')}", [call(rng, "Delete", Shift=xlShiftUp)])
for rng in ("B3", "C2:C5", "B8", "A1:A8", "B2:B6", "C3:D3"):
    add(f"delete_left_{rng.replace(':', '_')}", [call(rng, "Delete", Shift=xlShiftToLeft)])
for rng in ("B3:C3", "B2:B5"):
    add(f"insert_noshift_{rng.replace(':', '_')}", [call(rng, "Insert")])
    add(f"delete_noshift_{rng.replace(':', '_')}", [call(rng, "Delete")])

# ---- Copy with a Destination ----------------------------------------------------------------------------------
COPIES = [
    ("D2", "G10"), ("D2", "D10"), ("E2:E3", "H5"), ("E2:E3", "E2:E3"), ("D2:D3", "I2:I7"), ("D2:D3", "I2:I6"),
    ("D2", "H2:I4"), ("B6", "C10"), ("E4", "F10"), ("E5", "F9"), ("E6", "G10"), ("D2", "A1"), ("E3", "E1"),
    ("A1:F6", "A20"), ("F3:F5", "H1"), ("D2:E3", "H10:I13"), ("G5", "H9"), ("B2:C3", "C3"), ("A2:A3", "I10"),
    ("D2", "Other!F5"), ("C2:C3", "J1:J4"), ("D8", "E10"),
]
for src, dst in COPIES:
    read = ["A1:J14", "Other!D1:D2"]
    if dst == "A20":
        read.append("A20:F25")
    if dst.startswith("Other!"):
        read.append("Other!F5:F6")
    add(f"copy_{src.replace(':', '_')}_to_{dst.replace(':', '_').replace('!', '_')}", [call(src, "Copy", Destination=R(dst))], read=read)

# ---- Cut with a Destination ------------------------------------------------------------------------------------
CUTS = [
    ("B2", "I2"), ("B2:B5", "I2"), ("D2:D3", "D10"), ("A1:G6", "A20"), ("F4", "C8"), ("B6", "B10"), ("D6", "D10"),
    ("B2", "B3"), ("B3", "Other!F5"), ("E2:E3", "E3"), ("B5", "B4"), ("C2:C5", "I1"), ("B3", "J14"), ("G3", "G2"),
    ("D2:D5", "H2"), ("B4", "B9"),
]
for src, dst in CUTS:
    read = ["A1:J14", "Other!D1:D2"]
    if dst == "A20":
        read.append("A20:G25")
    if dst.startswith("Other!"):
        read.append("Other!F5:F6")
    add(f"cut_{src.replace(':', '_')}_to_{dst.replace(':', '_').replace('!', '_')}", [call(src, "Cut", Destination=R(dst))], read=read)

# ---- PasteSpecial ----------------------------------------------------------------------------------------------
def paste(cid, src, dst, **kw):
    add(cid, [call(src, "Copy"), call(dst, "PasteSpecial", **kw)])


paste("ps_values_D2D3_I2", "D2:D3", "I2", Paste=xlPasteValues)
paste("ps_values_F3F5_I6", "F3:F5", "I6", Paste=xlPasteValues)
paste("ps_values_A1D6_A9", "A1:D6", "A9", Paste=xlPasteValues)
paste("ps_formats_F3F5_B10", "F3:F5", "B10", Paste=xlPasteFormats)
paste("ps_formats_C2C3_D2", "C2:C3", "D2", Paste=xlPasteFormats)
paste("ps_formats_F3_A1A3", "F3", "A1:A3", Paste=xlPasteFormats)
paste("ps_formulas_D2E3_H4", "D2:E3", "H4", Paste=xlPasteFormulas)
paste("ps_formulas_C2C3_I10", "C2:C3", "I10", Paste=xlPasteFormulas)
paste("ps_formulas_G5_H10", "G5", "H10", Paste=xlPasteFormulas)
paste("ps_all_D2_I2I5", "D2", "I2:I5", Paste=xlPasteAll)
paste("ps_skipblanks_B2B5_C2", "B2:B5", "C2", Paste=xlPasteAll, SkipBlanks=True)
paste("ps_skipblanks_values_A2A5_F2", "A2:A5", "F2", Paste=xlPasteValues, SkipBlanks=True)
paste("ps_noskip_B2B5_C2", "B2:B5", "C2", Paste=xlPasteAll, SkipBlanks=False)
paste("ps_transpose_A1D3_A10", "A1:D3", "A10", Transpose=True)
paste("ps_transpose_B2E3_B10", "B2:E3", "B10", Transpose=True)
paste("ps_transpose_values_A1D2_F10", "A1:D2", "F10", Paste=xlPasteValues, Transpose=True)
paste("ps_transpose_D2D5_E10", "D2:D5", "E10", Transpose=True)
for opname, op in (("add", xlAdd), ("sub", xlSubtract), ("mul", xlMultiply), ("div", xlDivide)):
    paste(f"ps_{opname}_B2_C2C5", "B2", "C2:C5", Paste=xlPasteValues, Operation=op)
    paste(f"ps_{opname}_B2_A2A5", "B2", "A2:A5", Paste=xlPasteValues, Operation=op)
    paste(f"ps_{opname}_B2_D2D3", "B2", "D2:D3", Paste=xlPasteValues, Operation=op)
    paste(f"ps_{opname}_B2_F3F6", "B2", "F3:F6", Paste=xlPasteValues, Operation=op)
    paste(f"ps_{opname}_B4_C2C3", "B4", "C2:C3", Paste=xlPasteValues, Operation=op)
    paste(f"ps_{opname}_B2B3_C4C5", "B2:B3", "C4:C5", Paste=xlPasteAll, Operation=op)
paste("ps_add_D2_C2", "D2", "C2", Paste=xlPasteAll, Operation=xlAdd)
paste("ps_add_D2_C2_formulas", "D2", "C2", Paste=xlPasteFormulas, Operation=xlAdd)
paste("ps_add_A2_C2", "A2", "C2", Paste=xlPasteValues, Operation=xlAdd)
paste("ps_mul_F3_B2B5", "F3", "B2:B5", Paste=xlPasteValues, Operation=xlMultiply)
paste("ps_add_skipblanks_B2B5_C2", "B2:B5", "C2", Paste=xlPasteValues, Operation=xlAdd, SkipBlanks=True)

# ---- Insert copied cells -----------------------------------------------------------------------------------------
add("insert_copied_D2D3_G5_down", [call("D2:D3", "Copy"), call("G5:G6", "Insert", Shift=xlShiftDown)])
add("insert_copied_B2C2_A8_down", [call("B2:C2", "Copy"), call("A8:B8", "Insert", Shift=xlShiftDown)])

# ---- FillDown / FillRight / FillUp / FillLeft --------------------------------------------------------------------
for rng, method in (("B8:B11", "FillDown"), ("E2:E5", "FillDown"), ("F3:F6", "FillDown"), ("D2:D8", "FillDown"),
                    ("B6:E6", "FillRight"), ("A2:F2", "FillRight"), ("C8:H8", "FillRight"), ("D2:D6", "FillUp"),
                    ("F2:F4", "FillUp"), ("A8:D8", "FillLeft"), ("A3:C3", "FillLeft"), ("B2:C5", "FillDown"),
                    ("B2:E3", "FillRight")):
    add(f"{method.lower()}_{rng.replace(':', '_')}", [call(rng, method)])

# ---- AutoFill ------------------------------------------------------------------------------------------------------
_COLS = "ABCDEFGH"


def _seed_cells(seeds, direction, fmt, n):
    """The seed cells for a fill of n cells; the source range and the destination."""
    k = len(seeds)
    cells = {}
    if direction in ("down", "up"):
        first = 1 if direction == "down" else n - k + 1
        addrs = [f"A{first + i}" for i in range(k)]
        dest = f"A1:A{n}"
    else:
        first = 0 if direction == "right" else n - k
        addrs = [f"{_COLS[first + i]}1" for i in range(k)]
        dest = f"A1:{_COLS[n - 1]}1"
    for a, s in zip(addrs, seeds):
        if s == "":
            continue
        cells[a] = (s, fmt) if fmt else s
    return cells, f"{addrs[0]}:{addrs[-1]}" if k > 1 else addrs[0], dest


def fill(cid, seeds, direction="down", typ=xlFillDefault, fmt=None, n=8, extra=None, skip=()):
    cells, src, dest = _seed_cells(seeds, direction, fmt, n)
    if extra:
        cells.update(extra)
    read = ["A1:C10"] if direction in ("down", "up") else ["A1:H3"]
    add(f"fill_{cid}_{direction}_{FILL_TYPE_NAMES[typ]}", [call(src, "AutoFill", Destination=R(dest), Type=typ)], layout="blank",
        cells=cells, read=read, skip=skip)


HELP_COL = {f"C{i}": str(i * 10) for i in range(1, 11)}  # =C1*2 and the like fill down over these
HELP_ROW = {f"{_COLS[i]}3": str((i + 1) * 10) for i in range(8)}  # =A3*2 fills right over these
DATE = "yyyy-mm-dd"
SEEDS = {
    "n1": (["1"], None), "n2": (["1", "3"], None), "n3": (["1", "2", "4"], None), "n10_7": (["10", "7"], None),
    "dec": (["0.5", "0.75"], None), "neg": (["-1"], None), "n2_5": (["2.5"], None), "n3lin": (["1", "3", "5"], None),
    "abc": (["abc"], None), "ab": (["a", "b"], None), "item1": (["Item1"], None), "item1_3": (["Item1", "Item3"], None),
    "item9": (["Item 9"], None), "q1": (["Q1"], None), "q4": (["Q4"], None), "1abc": (["1abc"], None), "a1b2": (["A1B2"], None),
    "xm1": (["x-1"], None), "item_x": (["Item1", "x"], None), "1st": (["1st"], None), "item09": (["Item09"], None),
    "mon": (["Mon"], None), "monday": (["Monday"], None), "sat": (["Sat"], None), "mon_wed": (["Mon", "Wed"], None),
    "jan": (["Jan"], None), "january": (["January"], None), "dec_m": (["Dec"], None), "jan_apr": (["Jan", "Apr"], None),
    "mon_lc": (["mon"], None), "mon_uc": (["MON"], None), "jan_q1": (["Jan", "Q1"], None),
    "d1": (["45292"], DATE), "d2": (["45292", "45293"], DATE), "dmonth": (["45292", "45323"], DATE), "dleap": (["45351"], DATE),
    "djan31": (["45322"], DATE), "dfri": (["45296"], DATE), "dweek": (["45292", "45299"], DATE),
    "t1": (["0.375"], "h:mm"), "t2": (["0.375", "0.5"], "h:mm"), "p1": (["0.1"], "0%"), "p2": (["0.1", "0.2"], "0%"),
    "bool": (["TRUE"], None), "mix1a": (["1", "a"], None), "mixa1": (["a", "1"], None), "mix1b3": (["1", "", "5"], None),
    "mix3": (["1", "a", "3"], None), "dn": (["45292", "7"], None),
}
for name, (seeds, fmt) in SEEDS.items():
    fill(name, seeds, fmt=fmt)
FORMULA_DOWN = {"f_rel": ["=C1*2"], "f_abs": ["=$C$1+C1"], "f_mixed": ["=C$1+$C1"], "f_two": ["=C1", "=C2*2"],
                "f_text": ['=C1&"x"'], "f_sum": ["=SUM($C$1:C1)"]}
for name, seeds in FORMULA_DOWN.items():
    fill(name, seeds, extra=HELP_COL)
    fill(name, seeds, typ=xlFillCopy, extra=HELP_COL)
fill("f_rel_r", ["=A3*2"], direction="right", extra=HELP_ROW)
fill("f_abs_r", ["=$A$3+A3"], direction="right", extra=HELP_ROW)
fill("f_mixed_r", ["=A$3+$A3"], direction="right", extra=HELP_ROW)
fill("f_rel_up", ["=C8*2"], direction="up", extra=HELP_COL)
for name in ("n1", "n2", "n3", "item1", "item1_3", "mon", "jan", "d1", "d2", "p1", "mix1a", "abc"):
    seeds, fmt = SEEDS[name]
    fill(name, seeds, direction="right", fmt=fmt)
for name in ("n1", "n2", "item1", "item1_3", "mon", "jan", "d2", "abc"):
    seeds, fmt = SEEDS[name]
    fill(name, seeds, direction="up", fmt=fmt)
for name in ("n2", "item1_3", "monday", "d1"):
    seeds, fmt = SEEDS[name]
    fill(name, seeds, direction="left", fmt=fmt)
for name in ("n1", "n2", "item1", "mon", "d1", "p2", "bool"):
    seeds, fmt = SEEDS[name]
    fill(name, seeds, typ=xlFillCopy, fmt=fmt)
for name in ("n1", "n2", "n3", "abc", "item1", "mon", "jan", "d1", "t1", "p1", "mix1a"):
    seeds, fmt = SEEDS[name]
    fill(name, seeds, typ=xlFillSeries, fmt=fmt)
PREFILLED = {f"A{i}": str(i * 100) for i in range(3, 9)}
for name in ("p2", "d2", "n2"):
    seeds, fmt = SEEDS[name]
    fill(name + "_over", seeds, typ=xlFillFormats, fmt=fmt or "0.0", extra=PREFILLED)
    fill(name, seeds, typ=xlFillValues, fmt=fmt or "0.0", extra={f"A{i}": ("0", "0.000") for i in range(3, 9)})
for name in ("n1", "n2", "n3", "dec", "n3lin", "n10_7"):
    seeds, fmt = SEEDS[name]
    fill(name, seeds, typ=xlLinearTrend, fmt=fmt)
for name in ("n1", "n2", "n3", "n3lin"):
    seeds, fmt = SEEDS[name]
    fill(name, seeds, typ=xlGrowthTrend, fmt=fmt)
fill("g2_4", ["2", "4"], typ=xlGrowthTrend)
for name in ("d1", "d2", "dmonth", "djan31", "dfri", "dleap"):
    seeds, fmt = SEEDS[name]
    for typ in (xlFillDays, xlFillWeekdays, xlFillMonths, xlFillYears):
        if name in ("d2", "dleap") and typ == xlFillWeekdays:
            continue
        fill(name, seeds, typ=typ, fmt=fmt)

# ---- Clear -----------------------------------------------------------------------------------------------------------
for rng, method in (("B2:C3", "ClearContents"), ("C2:F5", "ClearFormats"), ("D2:D3", "Clear"), ("A1:H12", "ClearContents"),
                    ("B6", "Clear"), ("F5", "ClearFormats"), ("B2", "ClearContents"), ("A1:J14", "Clear")):
    add(f"{method.lower()}_{rng.replace(':', '_')}", [call(rng, method)])

# ---- Formats of whole columns, rows and the sheet: a cell set back to the default, and Clear over whole lines -----------
_HFMT = setp("H:H", "NumberFormat", "0.0%")
for cid, steps in (("cell_clearformats", [_HFMT, call("H5", "ClearFormats")]),
                   ("cell_general", [_HFMT, setp("H6", "NumberFormat", "General")]),
                   ("cell_paste_plain", [_HFMT, call("A1", "Copy", Destination=R("H7"))]),
                   ("row_clearformats", [_HFMT, call("9:9", "ClearFormats")]),
                   ("col_clearformats", [_HFMT, call("H:H", "ClearFormats")]),
                   ("col_clear", [_HFMT, call("G:I", "Clear")]),
                   ("row_then_cell", [setp("4:4", "NumberFormat", "0.0%"), call("I4", "ClearFormats")]),
                   ("sheet_clear", [setp("A1:XFD1048576", "NumberFormat", "0.0%"), call("A1:XFD1048576", "Clear")]),
                   ("sheet_then_cols", [setp("A1:XFD1048576", "NumberFormat", "0.0%"), setp("B:D", "NumberFormat", "0.000"),
                                        call("C3", "ClearFormats")]),
                   ("rows_but_last", [setp("1:1048575", "NumberFormat", "0.0%"), setp("H:H", "NumberFormat", "0.000")]),
                   ("cut_plain_in", [_HFMT, call("A2", "Cut", Destination=R("H5"))]),
                   ("cut_out", [_HFMT, setp("H3", "Value", 7), call("H3", "Cut", Destination=R("J3"))]),
                   ("cut_formatted_in", [_HFMT, call("C2", "Cut", Destination=R("H6"))]),
                   ("copy_from_column", [_HFMT, setp("H3", "Value", 7), call("H3", "Copy", Destination=R("J3"))]),
                   ("filldown_plain", [_HFMT, call("A2:I4", "FillDown")]),
                   ("autofill_plain", [_HFMT, call("A2", "AutoFill", Destination=R("A2:I2"))])):
    add(f"linefmt_{cid}", steps)

# ---- Whole columns and rows as the target: what a person selects with a header click ---------------------------------
for cid, steps in (("filldown_col_b", [call("B:B", "FillDown")]),
                   ("filldown_cols_bd", [call("B:D", "FillDown")]),
                   ("fillright_row_3", [call("3:3", "FillRight")]),
                   ("fillright_rows_2_4", [call("2:4", "FillRight")]),
                   ("copy_col_b_to_h", [call("B:B", "Copy", Destination=R("H:H"))]),
                   ("copy_row_2_to_10", [call("2:2", "Copy", Destination=R("10:10"))]),
                   ("clearcontents_col_d", [call("D:D", "ClearContents")]),
                   ("clear_row_3", [call("3:3", "Clear")]),
                   ("replace_col_a", [call("A:A", "Replace", What="Pear", Replacement="Plum", LookAt=2, SearchOrder=1, MatchCase=False,
                                           SearchFormat=False, ReplaceFormat=False)]),
                   ("sort_cols_a_g", [call("A1:G5", "Sort", Key1=R("B1"), Order1=xlDescending, Header=xlYes)]),
                   ("autofill_col_b", [call("B2:B3", "AutoFill", Destination=R("B2:B12"))]),
                   ("numberformat_col_b", [setp("B:B", "NumberFormat", "0.0")]),
                   ("numberformat_row_2", [setp("2:2", "NumberFormat", "0%")])):
    add(f"wholeline_{cid}", steps)

# ---- Merge / UnMerge -------------------------------------------------------------------------------------------------
add("merge_A1B2", [call("A1:B2", "Merge")], layout="merge")
add("merge_B1C2", [call("B1:C2", "Merge")], layout="merge")
add("merge_A2C2", [call("A2:C2", "Merge")], layout="merge")
add("merge_A1A2", [call("A1:A2", "Merge")], layout="merge")
add("merge_A1C2_across", [call("A1:C2", "Merge", Across=True)], layout="merge")
add("merge_unmerge_A1B2", [call("A1:B2", "Merge"), call("A1", "UnMerge")], layout="merge")
add("merge_clear_A1B2", [call("A1:B2", "Merge"), call("A1:B2", "ClearContents")], layout="merge")
add("merge_insert_row2", [call("A1:B2", "Merge"), call("2:2", "Insert")], layout="merge")
add("merge_delete_colA", [call("B1:C2", "Merge"), call("A:A", "Delete")], layout="merge")
add("merge_set_value", [call("B1:C2", "Merge"), setp("B1", "Formula", "=1+2")], layout="merge")
add("merge_copy_to_E4", [call("A1:B2", "Merge"), call("A1:B2", "Copy", Destination=R("D4"))], layout="merge", read=["A1:F6"])
# a merged block of two rows and two columns in a list (round 12): what is done through, beside and over it
LAYOUTS["merge2"] = {"cells": {"A1": "k", "A2": "a", "A3": "b", "A4": "c", "A5": "d", "B2": "m", "D2": "5", "D3": "3", "D4": "9",
                               "D5": "1", "E2": "=SUM(B2:C3)", "E3": "=B2&\"!\""}, "read": ["A1:G8"], "merge": True}
_M2 = call("B2:C3", "Merge")
for cid, steps in (("row_insert_3", [_M2, call("3:3", "Insert")]), ("row_delete_3", [_M2, call("3:3", "Delete")]),
                   ("row_delete_2_3", [_M2, call("2:3", "Delete")]), ("col_insert_c", [_M2, call("C:C", "Insert")]),
                   ("col_delete_c", [_M2, call("C:C", "Delete")]), ("cells_up_b3", [_M2, call("B3", "Delete", Shift=xlShiftUp)]),
                   ("cells_down_a2", [_M2, call("A2:C2", "Insert", Shift=xlShiftDown)]), ("cells_right_a3", [_M2, call("A3", "Insert", Shift=xlShiftToRight)]),
                   ("write_c3", [_M2, setp("C3", "Formula", "zz")]), ("write_block", [_M2, setp("B2:C3", "Formula", "=1")]),
                   ("clear_c3", [_M2, call("C3", "ClearContents")]), ("sort_d", [_M2, call("A1:D5", "Sort", Key1=R("D1"), Order1=xlAscending,
                                                                                                 Header=xlYes)]),
                   ("copy_to_f5", [_M2, call("B2:C3", "Copy", Destination=R("F5"))]), ("cut_to_f5", [_M2, call("B2:C3", "Cut", Destination=R("F5"))]),
                   ("filldown_b", [_M2, call("B2:B5", "FillDown")]), ("unmerge_after_insert", [_M2, call("3:3", "Insert"), call("B2", "UnMerge")]),
                   ("merge_over_merge", [_M2, call("B3:D4", "Merge")]), ("autofill_down", [_M2, call("B2:C3", "AutoFill", Destination=R("B2:C7"))])):
    add(f"merge2_{cid}", steps, layout="merge2")

# ---- Sort --------------------------------------------------------------------------------------------------------------
add("sort_name_asc_header", [call("A1:E8", "Sort", Key1=R("A1"), Order1=xlAscending, Header=xlYes)], layout="sort")
add("sort_name_desc_header", [call("A1:E8", "Sort", Key1=R("A1"), Order1=xlDescending, Header=xlYes)], layout="sort")
add("sort_score_asc_header", [call("A1:E8", "Sort", Key1=R("B1"), Order1=xlAscending, Header=xlYes)], layout="sort")
add("sort_score_desc_header", [call("A1:E8", "Sort", Key1=R("B1"), Order1=xlDescending, Header=xlYes)], layout="sort")
add("sort_group_score", [call("A1:E8", "Sort", Key1=R("C1"), Order1=xlAscending, Key2=R("B1"), Order2=xlDescending, Header=xlYes)],
    layout="sort")
add("sort_group_desc_name", [call("A1:E8", "Sort", Key1=R("C1"), Order1=xlDescending, Key2=R("A1"), Order2=xlAscending, Header=xlYes)],
    layout="sort")
add("sort_noheader_name", [call("A2:E8", "Sort", Key1=R("A2"), Order1=xlAscending, Header=xlNo)], layout="sort")
add("sort_noheader_all", [call("A1:E8", "Sort", Key1=R("A1"), Order1=xlAscending, Header=xlNo)], layout="sort")
add("sort_matchcase", [call("A1:E8", "Sort", Key1=R("A1"), Order1=xlAscending, Header=xlYes, MatchCase=True)], layout="sort")
add("sort_matchcase_desc", [call("A1:E8", "Sort", Key1=R("A1"), Order1=xlDescending, Header=xlYes, MatchCase=True)], layout="sort")
add("sort_guess", [call("A1:E8", "Sort", Key1=R("B1"), Order1=xlAscending, Header=xlGuess)], layout="sort")
add("sort_two_cols_only", [call("A2:B8", "Sort", Key1=R("B2"), Order1=xlAscending, Header=xlNo)], layout="sort")
add("sort_formula_col", [call("A1:E8", "Sort", Key1=R("D1"), Order1=xlDescending, Header=xlYes)], layout="sort")

# ---- Typed input ---------------------------------------------------------------------------------------------------------
INPUTS = ["007", "1,234", "1,234.5", "$5", "$1,234.50", "12%", "12.5%", "1e3", "1E+20", "(5)", "-5", "+5", " 5", "5 ", ".5",
          "5.", "1.50", "0 1/2", "1 1/4", "9:30", "9:30:15", "13:45", "9:30 PM", "TRUE", "true", "False", "=1+1",
          '="a"&"b"', "'abc", "'123", "'=1+1", "abc", "#N/A", "#DIV/0!", "123456789012345678", "12345678901234567890",
          "5-", "1 000", "1.2.3", "-$5", "$-5", "1e", "-0", "0.1", "1/0", "100%", "-12%", "2 PM", "12:00 AM", "0:00",
          "25:00", "1.5E-3"]
REGIONAL = ["2024/1/7", "2024-01-07", "2024/1/7 9:30"]  # the code a typed date gets is the region's short date
_SLUG = {"%": "pct", "$": "usd", "-": "m", "+": "p", ".": "d", ",": "c", " ": "_", "/": "s", ":": "h", "=": "eq", "'": "q",
         '"': "dq", "&": "amp", "(": "lp", ")": "rp", "#": "hash", "!": "bang", "@": "at"}


def slug(text):
    return "".join(ch if ch.isalnum() else _SLUG.get(ch, "x") for ch in text) or "blank"


for text in INPUTS:
    add(f"type_formula_{slug(text)}", [setp("A1", "Formula", text)], layout="blank")
for text in REGIONAL:
    add(f"type_formula_{slug(text)}", [setp("A1", "Formula", text)], layout="blank", skip=["numfmt"])
for text in ("007", "12%", "$5", "1e3", "9:30", "TRUE", "=1+1", "'abc", "abc", "(5)", "0 1/2", "1,234"):
    add(f"type_value_{slug(text)}", [setp("A1", "Value", text)], layout="blank")
for text in ("007", "=1+1", "12%", "1e3", "TRUE", "2024/1/7", "'abc", "-5", "9:30"):
    add(f"type_text_cell_{slug(text)}", [setp("A1", "NumberFormat", "@"), setp("A1", "Formula", text)], layout="blank")
for text in ("007", "=1+1", "12%"):
    add(f"type_text_cell_value_{slug(text)}", [setp("A1", "NumberFormat", "@"), setp("A1", "Value", text)], layout="blank")
for text, fmt in (("5", "0.00"), ("12%", "0.00"), ("0.5", "0%"), ("45000", DATE), ("1234", "#,##0")):
    add(f"type_formatted_{slug(text)}_{slug(fmt)}", [setp("A1", "NumberFormat", fmt), setp("A1", "Formula", text)], layout="blank")


# ---- Range edges: deleted cells trim a range, cut cells move its edge ------------------------------------------------
# The formulas on Other (and three on T, far from the cells changed) read ranges whose edges the steps delete or move;
# the names hold the same kind of ranges.
_EDGE_FORMULAS = [
    "=SUM(T!B2:C3)", "=SUM(T!A2:B3)", "=SUM(T!B2:C5)", "=SUM(T!B3:C4)", "=SUM(T!B1:C3)", "=SUM(T!B2:C6)", "=SUM(T!$B$2:$C$3)",
    "=SUM(T!A2:C3)", "=SUM(T!2:2)", "=SUM(T!B:C)", "=SUM(T!B2:D3)", "=SUM(T!B2:D2)", "=SUM(T!B2:B5)", "=SUM(T!$B$2:$B$5)",
    "=SUM(T!B3:B6)", "=SUM(T!B4:B5)", "=SUM(T!B2:C9)", "=SUM(T!B3:D4)", "=SUM(T!B1:D2)", "=SUM(T!B2:D5)",
    "=SUM(T!B1:D4)", "=SUM(T!C3:D4)", "=SUM(T!B:D)", "=SUM(T!B4:B8)", "=SUM(T!B1:B3)", "=SUM(T!B2:D4)", "=SUM(T!A:B)",
    "=SUM(T!B2:B$5)", "=SUM(T!C2:C5)", "=INDEX(T!B2:B5,1)", "=SUM(T!2:3)", "=SUM(T!1:3)", "=ROWS(T!B2:B5)",
    "=COLUMNS(T!B2:D3)", "=SUM(T!A2:A5)", "=SUM(T!A1:A3)", "=SUM(T!B5:B9)",
]
_EDGE_CELLS = {f"{_COLS[c]}{r}": str(10 * r + c + 1) for r in range(1, 11) for c in range(6)}
_EDGE_CELLS.update({"L20": "=SUM(B2:C3)", "M20": "=SUM(B2:B5)", "N20": "=SUM(B2:D3)"})
LAYOUTS["edges"] = {
    "cells": _EDGE_CELLS,
    "names": {"NA": "=T!$B$2:$C$3", "NB": "=T!$B$2:$B$5", "NC": "=T!$B$2:$D$3", "ND": "=T!$B$3:$D$4"},
    "other": {f"{'DEF'[i // 20]}{i % 20 + 1}": f for i, f in enumerate(_EDGE_FORMULAS)},
    "read": ["A1:N20", "Other!D1:H20"],
}
for rng, shift, tag in (("B2:B5", xlShiftUp, "up"), ("A2:A5", xlShiftUp, "up"), ("B2:C3", xlShiftUp, "up"),
                        ("D2:D3", xlShiftUp, "up"), ("B2:D3", xlShiftToLeft, "left"), ("B2:B5", xlShiftToLeft, "left"),
                        ("B3:D3", xlShiftToLeft, "left"), ("B2:B5", xlShiftDown, "insdown"), ("B2:D3", xlShiftToRight, "insright")):
    method = "Insert" if tag.startswith("ins") else "Delete"
    add(f"edge_{tag}_{rng.replace(':', '_')}", [call(rng, method, Shift=shift)], layout="edges")
for src, dst in (("B2", "B3"), ("B2", "B5"), ("B2", "B7"), ("B2", "C2"), ("B2", "A2"), ("B2:B3", "B4"), ("B2:C3", "B6"),
                 ("C3", "C4"), ("B2:B5", "C2"), ("D2:D3", "F2"), ("D2:D3", "A2"), ("D2:D3", "C2"), ("B5", "B12"),
                 ("B2", "Other!H10"), ("D:D", "F:F"), ("2:2", "12:12"), ("B2:D2", "B3")):
    add(f"edge_cut_{src.replace(':', '_')}_to_{dst.replace(':', '_').replace('!', '_')}", [call(src, "Cut", Destination=R(dst))],
        layout="edges")
# an edge moved onto or past the opposite edge, and cut to another sheet
LAYOUTS["edges2"] = {
    "cells": {f"{_COLS[c]}{r}": str(10 * r + c + 1) for r in range(1, 11) for c in range(7)},
    "names": {"NE": "=T!$B$2:$B$6", "NF": "=T!$B$3:$B$6"},
    "other": {f"D{i + 1}": f for i, f in enumerate([
        "=SUM(T!B2:B6)", "=SUM(T!B3:B6)", "=SUM(T!B2:B5)", "=SUM(T!B4:B6)", "=SUM(T!B2:F2)", "=SUM(T!C2:F2)", "=SUM(T!B2:C6)",
        "=SUM(T!B1:B6)", "=SUM(T!B2:E2)"])},
    "read": ["A1:H12", "Other!D1:H12"],
}
for src, dst in (("B2", "B5"), ("B2", "B6"), ("B2:B3", "B5"), ("B2:B3", "B6"), ("B6", "B2"), ("B6", "B3"), ("B3", "B1"),
                 ("B3", "B2"), ("B2:B3", "B3"), ("F2", "B2"), ("B2", "F2"), ("B2", "E2"), ("B2", "Other!H10"), ("B6", "Other!H10"),
                 ("B2:B6", "Other!H10"), ("B2:C2", "Other!H10"), ("B2:B3", "Other!H10")):
    add(f"edge2_cut_{src.replace(':', '_')}_to_{dst.replace(':', '_').replace('!', '_')}", [call(src, "Cut", Destination=R(dst))],
        layout="edges2")
# cells written over at a range's edge by cells that were not in it
LAYOUTS["edges3"] = {
    "cells": {f"{_COLS[c]}{r}": str(10 * r + c + 1) for r in range(1, 11) for c in range(7)},
    "names": {},
    "other": {f"D{i + 1}": f for i, f in enumerate([
        "=SUM(T!B4:B6)", "=SUM(T!B2:B4)", "=SUM(T!B5:B9)", "=SUM(T!B4:C6)", "=SUM(T!C2:E2)", "=SUM(T!B3:B4)", "=SUM(T!B6:B7)"])},
    "read": ["A1:H12", "Other!D1:H12"],
}
for src, dst in (("B5", "B4"), ("B6", "B4"), ("B7", "B6"), ("B3", "B4"), ("B2", "B4"), ("B8", "B6"), ("B9", "B4"), ("B1", "B6"),
                 ("F2", "E2"), ("B7:C7", "B6"), ("B3:C3", "B4"), ("B8", "B7")):
    add(f"edge3_cut_{src.replace(':', '_')}_to_{dst.replace(':', '_').replace('!', '_')}", [call(src, "Cut", Destination=R(dst))],
        layout="edges3")


# ---- Data operations: Remove Duplicates, AutoFilter, Replace, Text to Columns, Subtotal, Sort (round 9) ------------------
# A trading list with a header, duplicates that differ in case, a blank, numbers and formulas beside; H holds
# =SUBTOTAL(103,...) of each row, so what a filter hides shows as 0 in the cells read back.
xlAnd, xlOr, xlTop10Items, xlBottom10Items, xlFilterValues = 1, 2, 3, 4, 7
xlPart, xlWhole = 2, 1
xlDelimited, xlFixedWidth = 1, 2
xlTextQualifierDoubleQuote, xlTextQualifierNone = 1, -4142
xlSum, xlCount, xlAverage = -4157, -4112, -4106
xlSortRows, xlSortColumns = 2, 1
_DATA = {
    "A1": "Sym", "B1": "Side", "C1": "Qty", "D1": "Px", "E1": "Note", "F1": "Value", "G1": "Next",
    "A2": "TXF", "B2": "Buy", "C2": "2", "D2": "23000", "E2": "a",
    "A3": "MXF", "B3": "Sell", "C3": "1", "D3": "23010",
    "A4": "txf", "B4": "Buy", "C4": "2", "D4": "23000", "E4": "a",
    "A5": "TXO", "B5": "Buy", "C5": "5", "D5": "300", "E5": "b",
    "A6": "MXF", "B6": "Sell", "C6": "1", "D6": "23010",
    "A7": "TXF", "B7": "Sell", "C7": "3", "D7": "22990", "E7": "c",
    "B8": "Buy", "C8": "4", "D8": "23005",
    "A9": "TE", "B9": "Buy", "C9": "10", "D9": ("1000.5", "#,##0.00"), "E9": "d",
}
for _r in range(2, 10):
    _DATA[f"F{_r}"] = f"=C{_r}*D{_r}"
    _DATA[f"G{_r}"] = f"=C{_r + 1}"
for _r in range(1, 11):
    _DATA[f"H{_r}"] = f"=SUBTOTAL(103,B{_r})"
_DATA["J1"] = "=SUM(C2:C9)"
_DATA["J2"] = "=SUBTOTAL(9,C2:C9)"
_DATA["J3"] = "=COUNTIF(A2:A9,\"TXF\")"
LAYOUTS["data"] = {"cells": _DATA, "read": ["A1:K12"]}

for cid, kw in (("a", {"Columns": 1, "Header": xlYes}), ("ab", {"Columns": [1, 2], "Header": xlYes}),
                ("abcd", {"Columns": [1, 2, 3, 4], "Header": xlYes}), ("b_noheader", {"Columns": 2, "Header": xlNo}),
                ("e", {"Columns": 5, "Header": xlYes}), ("guess", {"Columns": 1, "Header": xlGuess})):
    add(f"dedupe_{cid}", [call("A1:E9", "RemoveDuplicates", **kw)], layout="data")
add("dedupe_subset_ab", [call("A1:B9", "RemoveDuplicates", Columns=[1, 2], Header=xlYes)], layout="data")

for cid, kw in (("sym_txf", {"Field": 1, "Criteria1": "TXF"}), ("side_buy", {"Field": 2, "Criteria1": "Buy"}),
                ("qty_gt2", {"Field": 3, "Criteria1": ">2"}), ("sym_blank", {"Field": 1, "Criteria1": "="}),
                ("sym_nonblank", {"Field": 1, "Criteria1": "<>"}), ("sym_t_star", {"Field": 1, "Criteria1": "T*"}),
                ("sym_q", {"Field": 1, "Criteria1": "?XF"}), ("note_ne_a", {"Field": 5, "Criteria1": "<>a"}),
                ("px_and", {"Field": 4, "Criteria1": ">=23000", "Operator": xlAnd, "Criteria2": "<23010"}),
                ("px_or", {"Field": 4, "Criteria1": "<1000", "Operator": xlOr, "Criteria2": ">23005"}),
                ("qty_top3", {"Field": 3, "Criteria1": "3", "Operator": xlTop10Items}),
                ("qty_bottom2", {"Field": 3, "Criteria1": "2", "Operator": xlBottom10Items}),
                ("sym_values", {"Field": 1, "Criteria1": ["TXF", "TE"], "Operator": xlFilterValues}),
                ("sym_contains", {"Field": 1, "Criteria1": "*X*"}), ("px_num_text", {"Field": 4, "Criteria1": "23000"})):
    add(f"filter_{cid}", [call("A1:E9", "AutoFilter", **kw)], layout="data")
add("filter_two_fields", [call("A1:E9", "AutoFilter", Field=2, Criteria1="Buy"), call("A1:E9", "AutoFilter", Field=3, Criteria1=">=4")],
    layout="data")
add("filter_then_off", [call("A1:E9", "AutoFilter", Field=2, Criteria1="Sell"), call("A1:E9", "AutoFilter")], layout="data")

for cid, kw in (("part", {"What": "TXF", "Replacement": "TX", "LookAt": xlPart, "MatchCase": False}),
                ("whole", {"What": "TXF", "Replacement": "TX", "LookAt": xlWhole, "MatchCase": False}),
                ("case", {"What": "TXF", "Replacement": "TX", "LookAt": xlPart, "MatchCase": True}),
                ("star", {"What": "T*", "Replacement": "Z", "LookAt": xlWhole, "MatchCase": False}),
                ("q", {"What": "?XF", "Replacement": "Q", "LookAt": xlWhole, "MatchCase": False}),
                ("number", {"What": "23000", "Replacement": "24000", "LookAt": xlWhole, "MatchCase": False}),
                ("in_number", {"What": "0", "Replacement": "9", "LookAt": xlPart, "MatchCase": False}),
                ("formula_ref", {"What": "D", "Replacement": "C", "LookAt": xlPart, "MatchCase": False}),
                ("to_empty", {"What": "a", "Replacement": "", "LookAt": xlWhole, "MatchCase": False}),
                ("tilde", {"What": "~*", "Replacement": "x", "LookAt": xlPart, "MatchCase": False}),
                ("buy_lower", {"What": "buy", "Replacement": "long", "LookAt": xlWhole, "MatchCase": False})):
    add(f"replace_{cid}", [call("A1:G9", "Replace", **kw)], layout="data")
add("replace_tilde_star", [setp("E3", "Formula", "a*b"), call("E1:E9", "Replace", What="~*", Replacement="x", LookAt=xlPart)],
    layout="data")

_SPLIT = {"A1": "a,b,c", "A2": "1;2;3", "A3": "x  y", "A4": '"p,q",r', "A5": "k|l|m", "A6": "1,2.5,abc", "A7": "007,0x1,1e3",
          "A8": ",lead,,trail,", "A9": "2024/1/7,9:30", "C1": "old", "D2": "old"}
LAYOUTS["split"] = {"cells": _SPLIT, "read": ["A1:F10"]}
# (every delimiter is given: Excel's Text to Columns keeps the last call's settings for those left out)
_NO_DELIM = {"Tab": False, "Semicolon": False, "Comma": False, "Space": False, "Other": False, "ConsecutiveDelimiter": False,
             "TextQualifier": xlTextQualifierDoubleQuote}
for cid, kw in (("comma", {"Comma": True}), ("semicolon", {"Semicolon": True}),
                ("space_consecutive", {"Space": True, "ConsecutiveDelimiter": True}), ("space", {"Space": True}),
                ("quote", {"Comma": True}), ("no_quote", {"Comma": True, "TextQualifier": xlTextQualifierNone}),
                ("other_bar", {"Other": True, "OtherChar": "|"}), ("comma_consecutive", {"Comma": True, "ConsecutiveDelimiter": True})):
    add(f"split_{cid}", [call("A1:A9", "TextToColumns", Destination=R("A1"), DataType=xlDelimited, **dict(_NO_DELIM, **kw))],
        layout="split", skip=["numfmt"] if cid in ("comma", "quote", "no_quote", "comma_consecutive") else ())
add("split_to_c", [call("A1:A3", "TextToColumns", Destination=R("C1"), DataType=xlDelimited, **dict(_NO_DELIM, Comma=True, Semicolon=True))],
    layout="split")

# Subtotal over the list sorted by side (its labels in column B are the UI language's, Chinese in the Excel measured,
# so B is left out of the cells read)
add("subtotal_side_sum", [call("A1:E9", "Sort", Key1=R("B1"), Order1=xlAscending, Header=xlYes),
                          call("A1:E9", "Subtotal", GroupBy=2, Function=xlSum, TotalList=[3, 4])], layout="data", read=["A1:A14", "C1:K14"])

# Sort: what moves with the rows and how references in them follow
add("sort_data_qty", [call("A1:G9", "Sort", Key1=R("C1"), Order1=xlAscending, Header=xlYes)], layout="data")
add("sort_data_sym_px", [call("A1:G9", "Sort", Key1=R("A1"), Order1=xlAscending, Key2=R("D1"), Order2=xlDescending, Header=xlYes)],
    layout="data")
add("sort_data_rows", [call("C2:E3", "Sort", Key1=R("C2"), Order1=xlDescending, Orientation=xlSortRows, Header=xlNo)], layout="data")
add("sort_data_note_blank", [call("A1:E9", "Sort", Key1=R("E1"), Order1=xlDescending, Header=xlYes)], layout="data")

# ---- Data tools (round 12): Subtotal's other functions, Remove Duplicates on numbers, Advanced Filter, Data Series ------
xlMax, xlMin, xlCountNums = -4136, -4139, -4113
xlSummaryAbove, xlSummaryBelow = 0, 1
_SORTED = call("A1:E9", "Sort", Key1=R("B1"), Order1=xlAscending, Header=xlYes)
for cid, fn, totals in (("count_note", xlCount, [5]), ("average_px", xlAverage, [4]), ("max_qty", xlMax, [3, 4]),
                        ("min_qty", xlMin, [3]), ("countnums_note", xlCountNums, [5])):
    add(f"data12_subtotal_{cid}", [_SORTED, call("A1:E9", "Subtotal", GroupBy=2, Function=fn, TotalList=totals)], layout="data",
        read=["A1:A14", "C1:K14"])
add("data12_subtotal_above", [_SORTED, call("A1:E9", "Subtotal", GroupBy=2, Function=xlSum, TotalList=[3], SummaryBelowData=xlSummaryAbove)],
    layout="data", read=["A1:A14", "C1:K14"])
add("data12_subtotal_twice", [_SORTED, call("A1:E9", "Subtotal", GroupBy=2, Function=xlSum, TotalList=[3]),
                              call("A1:E12", "Subtotal", GroupBy=2, Function=xlMax, TotalList=[4], Replace=False)],
    layout="data", read=["A1:A16", "C1:K16"])
add("data12_dedupe_qty_px", [call("A1:E9", "RemoveDuplicates", Columns=[3, 4], Header=xlYes)], layout="data")
add("data12_dedupe_px", [call("A1:E9", "RemoveDuplicates", Columns=4, Header=xlYes)], layout="data")

# Advanced Filter with a criteria range in M1:O3 (the cells read reach it)
xlFilterInPlace, xlFilterCopy = 1, 2
_ADV = dict(_DATA)
_ADV.update({"M1": "Side", "N1": "Qty", "O1": "Sym", "M2": "Sell"})
LAYOUTS["adv"] = {"cells": _ADV, "read": ["A1:P20"]}
add("data12_adv_inplace_sell", [call("A1:E9", "AdvancedFilter", Action=xlFilterInPlace, CriteriaRange=R("M1:M2"))], layout="adv")
add("data12_adv_copy_unique_side", [call("B1:B9", "AdvancedFilter", Action=xlFilterCopy, CopyToRange=R("A12"), Unique=True)],
    layout="adv")
add("data12_adv_copy_qty_gt2", [setp("N2", "Formula", ">2"), call("A1:E9", "AdvancedFilter", Action=xlFilterCopy,
                                                                  CriteriaRange=R("N1:N2"), CopyToRange=R("A12"))], layout="adv")
add("data12_adv_or_rows", [setp("O3", "Formula", "TE"), call("A1:E9", "AdvancedFilter", Action=xlFilterInPlace, CriteriaRange=R("M1:O3"))],
    layout="adv")
add("data12_adv_and_cols", [setp("N2", "Formula", "1"), call("A1:E9", "AdvancedFilter", Action=xlFilterInPlace, CriteriaRange=R("M1:N2"))],
    layout="adv")
add("data12_adv_wildcard", [setp("O2", "Formula", "T*"), call("A1:E9", "AdvancedFilter", Action=xlFilterCopy, CriteriaRange=R("O1:O2"),
                                                                CopyToRange=R("A12"))], layout="adv")
add("data12_adv_formula", [setp("P1", "Formula", "Big"), setp("P2", "Formula", "=C2*D2>40000"),
                           call("A1:E9", "AdvancedFilter", Action=xlFilterCopy, CriteriaRange=R("P1:P2"), CopyToRange=R("A12"))],
    layout="adv")

# Data Series (Home > Fill > Series) from seeds in A1 and C1
xlRows, xlColumns = 1, 2
xlDataSeriesLinear, xlGrowth, xlChronological = -4132, 2, 3
xlDay, xlWeekday, xlMonth, xlYear = 1, 2, 3, 4
LAYOUTS["series"] = {"cells": {"A1": "1", "C1": ("45296", "yyyy-mm-dd"), "E1": "3", "E2": "5", "E3": "8"}, "read": ["A1:F12"]}
for cid, rng, kw in (("linear_step2", "A1:A8", {"Rowcol": xlColumns, "Type": xlDataSeriesLinear, "Step": 2}),
                     ("linear_stop", "A1:A10", {"Rowcol": xlColumns, "Type": xlDataSeriesLinear, "Step": 3, "Stop": 10}),
                     ("growth", "A1:A6", {"Rowcol": xlColumns, "Type": xlGrowth, "Step": 3}),
                     ("growth_stop", "A1:A10", {"Rowcol": xlColumns, "Type": xlGrowth, "Step": 2, "Stop": 20}),
                     ("days", "C1:C6", {"Rowcol": xlColumns, "Type": xlChronological, "Date": xlDay, "Step": 7}),
                     ("weekdays", "C1:C8", {"Rowcol": xlColumns, "Type": xlChronological, "Date": xlWeekday, "Step": 1}),
                     ("months", "C1:C6", {"Rowcol": xlColumns, "Type": xlChronological, "Date": xlMonth, "Step": 1}),
                     ("years", "C1:C4", {"Rowcol": xlColumns, "Type": xlChronological, "Date": xlYear, "Step": 2}),
                     ("negative", "A1:A6", {"Rowcol": xlColumns, "Type": xlDataSeriesLinear, "Step": -1.5}),
                     ("trend", "E1:E8", {"Rowcol": xlColumns, "Type": xlDataSeriesLinear, "Trend": True}),
                     ("growth_trend", "E1:E8", {"Rowcol": xlColumns, "Type": xlGrowth, "Trend": True})):
    add(f"data12_series_{cid}", [call(rng, "DataSeries", **kw)], layout="series")

# ---- FormulaR1C1 written (round 12): what each R1C1 form becomes in A1, read back as Formula -------------------------
LAYOUTS["r1c1"] = {"cells": {"A1": "1", "B1": "2", "C1": "3", "A2": "4", "B2": "5", "C2": "6", "A3": "7", "B3": "8", "C3": "9"},
                   "read": ["A1:H9"]}
for i, f in enumerate(["=R[-1]C+1", "=SUM(R1C:R[-1]C)", "=RC[-1]*R2C3", "=SUM(R[-4]C[-3]:R[-2]C[-1])", "=SUM(C1)", "=SUM(R2)",
                       "=SUM(C[-3]:C[-2])", "=SUM(R[-3]:R[-2])", "=R1C1&\"R1C1\"", "=Other!R1C1+R[-4]C[-4]", "=R[-4]C[-4]:R[-3]C[-3]",
                       "=SUM(R1C1:R3C3)/R1C[-3]", "=SUM(R)", "=SUM(C)", "=R[1048571]C", "=RC[16380]", "=ROWS(R1:R3)",
                       "=COLUMNS(C1:C3)", "=R1C1+R1C2*-R[-4]C[-2]^2", "=IF(R1C1>0,R[-4]C,\"\")"]):
    add(f"r1c1_{i:02d}", [setp("D5", "FormulaR1C1", f)], layout="r1c1", note=f)
add("r1c1_block", [setp("E5:F6", "FormulaR1C1", "=R[-4]C[-4]+R1C[-3]")], layout="r1c1")
# a formula whose result is a range, written through the legacy Formula / FormulaR1C1 (implicit intersection) or Formula2
for cid, prop, f in (("formula_range_row", "Formula", "=A1:C1"), ("formula_range_col", "Formula", "=A1:A3"),
                     ("formula_range_block", "Formula", "=A1:C3"), ("formula2_range", "Formula2", "=A1:A3"),
                     ("r1c1_range", "FormulaR1C1", "=R1C1:R3C1"), ("formula_index", "Formula", "=INDEX(A1:C3,0,2)"),
                     ("formula_sequence", "Formula", "=SEQUENCE(3)"), ("formula_offset", "Formula", "=OFFSET(A1,0,0,2,1)")):
    add(f"ii_{cid}", [setp("D2", prop, f)], layout="r1c1")
    add(f"ii_{cid}_row5", [setp("D5", prop, f)], layout="r1c1")
# ranges that span the whole sheet's width or height: how Excel writes them back
for cid, f in (("full_width", "=SUM(A1:XFD2)"), ("full_both", "=COUNTA(A1:XFD1048576)"),
               ("full_width_abs", "=SUM($A$1:$XFD$2)"), ("full_mixed", "=SUM(A$1:XFD$2)"), ("not_full", "=SUM(A1:XFC2)"),
               ("full_sheet_ref", "=SUM(Other!A1:XFD1)")):
    add(f"norm_{cid}", [setp("D5", "Formula", f)], layout="r1c1")
# a range typed with its ends in reverse order, one end absolute and one relative
for cid, f in (("rev_mixed_cols", "=SUM($C1:B2)"), ("rev_mixed_rows", "=SUM(C$3:D1)"), ("rev_rel", "=SUM(C3:B1)"),
               ("rev_abs", "=SUM($C$3:$B$1)")):
    add(f"norm_{cid}", [setp("D5", "Formula", f)], layout="r1c1")

# ---- Paste Special's other kinds (round 12): formulas / values with number formats, all but borders, transposed formulas,
# tiling a larger destination, an operation by a blank cell ---------------------------------------------------------------
xlPasteAllExceptBorders, xlPasteFormulasAndNumberFormats, xlPasteValuesAndNumberFormats = 7, 11, 12
for cid, src, dst, kw in (("formulas_numfmt", "C2:F5", "H2", {"Paste": xlPasteFormulasAndNumberFormats}),
                          ("values_numfmt", "C2:F5", "H2", {"Paste": xlPasteValuesAndNumberFormats}),
                          ("all_but_borders", "C2:F5", "H2", {"Paste": xlPasteAllExceptBorders}),
                          ("formulas_transpose", "B2:D3", "H2", {"Paste": xlPasteFormulas, "Transpose": True}),
                          ("all_transpose", "B2:F3", "H2", {"Paste": xlPasteAll, "Transpose": True}),
                          ("values_numfmt_transpose", "C2:F3", "H2", {"Paste": xlPasteValuesAndNumberFormats, "Transpose": True}),
                          ("values_tile", "B2:C2", "H2:K3", {"Paste": xlPasteValues}),
                          ("formulas_tile", "D2:D3", "H2:H7", {"Paste": xlPasteFormulas}),
                          ("divide_by_blank", "B4", "B2:B3", {"Paste": xlPasteValues, "Operation": xlDivide}),
                          ("add_text_onto_numbers", "A2", "B2:B3", {"Paste": xlPasteValues, "Operation": xlAdd}),
                          ("add_numbers_onto_text", "B2", "A2:A3", {"Paste": xlPasteValues, "Operation": xlAdd}),
                          ("multiply_formulas_all", "C2", "D2:D3", {"Paste": xlPasteAll, "Operation": xlMultiply}),
                          ("skip_blanks_transpose", "A4:C5", "H2", {"Paste": xlPasteAll, "SkipBlanks": True, "Transpose": True})):
    add(f"data17_ps_{cid}", [call(src, "Copy"), call(dst, "PasteSpecial", **kw)], read=["A1:L14"])

# ---- Dynamic arrays and what is done around and inside them (round 12) -------------------------------------------------
LAYOUTS["spill"] = {"cells": {"A1": "1", "A2": "2", "A3": "3", "A4": "4", "A5": "5", "C1": "=SEQUENCE(5)", "D1": "=SUM(C1#)",
                              "D2": "=ROWS(C1#)", "E1": "=A1:A5*10", "F1": "=TRANSPOSE(A1:A3)", "D3": "=C3"},
                    "read": ["A1:I9"]}
for cid, steps in (("rows_insert_3", [call("3:3", "Insert")]), ("rows_delete_3", [call("3:3", "Delete")]),
                   ("rows_insert_1", [call("1:1", "Insert")]), ("rows_delete_1", [call("1:1", "Delete")]),
                   ("cols_insert_c", [call("C:C", "Insert")]), ("cols_insert_d", [call("D:D", "Insert")]),
                   ("cols_delete_b", [call("B:B", "Delete")]), ("write_in_spill", [setp("C3", "Formula", "x")]),
                   ("write_then_clear", [setp("C3", "Formula", "x"), call("C3", "ClearContents")]),
                   ("cut_anchor", [call("C1", "Cut", Destination=R("H1"))]), ("copy_anchor", [call("C1", "Copy", Destination=R("H1"))]),
                   ("copy_spill_part", [call("C2:C3", "Copy", Destination=R("H1"))]), ("source_grows", [setp("A5", "Formula", "=A4+5")]),
                   ("cells_down_in_source", [call("A2", "Insert", Shift=xlShiftDown)]), ("delete_anchor", [call("C1", "ClearContents")]),
                   ("fill_down_anchor", [call("C1:C2", "FillDown")]), ("sort_source", [call("A1:A5", "Sort", Key1=R("A1"), Order1=xlDescending,
                                                                                         Header=xlNo)])):
    add(f"spill_{cid}", steps, layout="spill")

# ---- Rows and columns hidden by hand (round 12): every operation sees them (unlike a filter's) ---------------------------
_HIDE = setp("3:4", "RowHeight", 0)
_HIDE_COL = setp("C:C", "ColumnWidth", 0)
for cid, steps in (("copy", [_HIDE, call("A1:E9", "Copy", Destination=R("A12"))]), ("cut", [_HIDE, call("A2:E5", "Cut", Destination=R("A12"))]),
                   ("autofill", [_HIDE, call("C2:C3", "AutoFill", Destination=R("C2:C9"))]), ("sort", [_HIDE, call("A1:E9", "Sort", Key1=R("C1"),
                                                                                                     Order1=xlDescending, Header=xlYes)]),
                   ("ps_values", [_HIDE, call("C2:C6", "Copy"), call("G2", "PasteSpecial", Paste=xlPasteValues)]),
                   ("delete_rows", [_HIDE, call("2:5", "Delete")]), ("replace", [_HIDE, call("A1:E9", "Replace", What="Sell", Replacement="S",
                                                                                         LookAt=xlWhole)]),
                   ("col_copy", [_HIDE_COL, call("A1:E3", "Copy", Destination=R("A12"))]), ("col_filldown", [_HIDE_COL, call("A2:E4", "FillDown")]),
                   ("col_value", [_HIDE_COL, setp("B2:D3", "Formula", "q")]), ("dedupe", [_HIDE, call("A1:E9", "RemoveDuplicates", Columns=2,
                                                                                                  Header=xlYes)])):
    add(f"hidden_{cid}", steps, layout="data", read=["A1:K22"])

# ---- Defined names following structure changes and moves (round 12) --------------------------------------------------
LAYOUTS["named"] = {"cells": {"A1": "h", "B1": "Qty", "B2": "1", "B3": "2", "B4": "3", "B5": "4", "C2": "=SUM(Qty)", "C3": "=Corner",
                              "C4": "=SUM(Wide)", "C5": "=Rel", "D1": "x", "D2": "5", "D3": "6"},
                    "names": {"Qty": "=T!$B$2:$B$5", "Corner": "=T!$A$1", "Wide": "=T!$B$2:$D$3", "Rel": "=T!B2", "T!Local": "=T!$D$2:$D$3",
                              "Konst": "=10*2", "Mixed": "=T!$B2:B$5"},
                    "read": ["A1:H8"]}
for cid, steps in (("rows_insert_3", [call("3:3", "Insert")]), ("rows_insert_2", [call("2:2", "Insert")]),
                   ("rows_insert_6", [call("6:6", "Insert")]), ("rows_delete_3", [call("3:3", "Delete")]),
                   ("rows_delete_2_5", [call("2:5", "Delete")]), ("rows_delete_1", [call("1:1", "Delete")]),
                   ("cols_insert_b", [call("B:B", "Insert")]), ("cols_insert_c", [call("C:C", "Insert")]),
                   ("cols_delete_b", [call("B:B", "Delete")]), ("cols_delete_d", [call("D:D", "Delete")]),
                   ("cut_qty", [call("B2:B5", "Cut", Destination=R("F2"))]), ("cut_half", [call("B2:B3", "Cut", Destination=R("F2"))]),
                   ("cells_up", [call("B3:B4", "Delete", Shift=xlShiftUp)]), ("cells_down", [call("B3", "Insert", Shift=xlShiftDown)]),
                   ("cells_left", [call("A2:A3", "Delete", Shift=xlShiftToLeft)]), ("sort_qty_desc", [call("B1:B5", "Sort", Key1=R("B1"),
                                                                                                         Order1=xlDescending, Header=xlYes)])):
    add(f"named_{cid}", steps, layout="named")

# ---- More data tools (round 12): AutoFilter by percent and average, Text to Columns with FieldInfo, Apply Names -------
xlTop10Percent, xlBottom10Percent, xlFilterDynamic = 5, 6, 11
xlFilterAboveAverage, xlFilterBelowAverage = 33, 34
for cid, kw in (("px_top25pct", {"Field": 4, "Criteria1": "25", "Operator": xlTop10Percent}),
                ("qty_bottom40pct", {"Field": 3, "Criteria1": "40", "Operator": xlBottom10Percent}),
                ("qty_above_avg", {"Field": 3, "Criteria1": xlFilterAboveAverage, "Operator": xlFilterDynamic}),
                ("px_below_avg", {"Field": 4, "Criteria1": xlFilterBelowAverage, "Operator": xlFilterDynamic})):
    add(f"data13_filter_{cid}", [call("A1:E9", "AutoFilter", **kw)], layout="data")
xlGeneralFormat, xlTextFormat, xlMDYFormat, xlYMDFormat, xlSkipColumn = 1, 2, 3, 5, 9
LAYOUTS["fixed"] = {"cells": {"A1": "ABC 007 2024/01/07", "A2": "DEF 345 2023/12/31", "A3": "GH  1.5 2024/02/29", "A4": "007,1/2,abc"},
                    "read": ["A1:F6"]}
add("data13_split_fixed_text_skip", [call("A1:A3", "TextToColumns", Destination=R("B1"), DataType=xlFixedWidth,
                                           FieldInfo=[[0, xlGeneralFormat], [4, xlTextFormat], [8, xlSkipColumn]])], layout="fixed")
add("data13_split_fixed_ymd", [call("A1:A3", "TextToColumns", Destination=R("B1"), DataType=xlFixedWidth,
                                    FieldInfo=[[0, xlGeneralFormat], [4, xlGeneralFormat], [8, xlYMDFormat]])], layout="fixed",
    skip=["numfmt"])
add("data13_split_delim_fieldinfo", [call("A4", "TextToColumns", Destination=R("B4"), DataType=xlDelimited,
                                          FieldInfo=[[1, xlTextFormat], [2, xlGeneralFormat], [3, xlSkipColumn]],
                                          **dict(_NO_DELIM, Comma=True))], layout="fixed", skip=["numfmt"])
add("data13_apply_names_rate", [call("A1:J14", "ApplyNames", Names=["Rate"])])
add("data13_apply_names_absolute_only", [call("A1:J14", "ApplyNames", Names=["Rate"], IgnoreRelativeAbsolute=False)])

# ---- What the object model does on a filtered list (round 12): Side = Buy shows rows 2, 4, 5, 8, 9 ---------------------
_BUY = call("A1:E9", "AutoFilter", Field=2, Criteria1="Buy")
add("data14_filtered_copy", [_BUY, call("A1:E9", "Copy", Destination=R("A12"))], layout="data", read=["A1:K22"])
add("data14_filtered_filldown", [_BUY, call("E2:E9", "FillDown")], layout="data")
add("data14_filtered_clearcontents", [_BUY, call("C3:C8", "ClearContents")], layout="data")
add("data14_filtered_rows_delete", [_BUY, call("3:6", "Delete")], layout="data")
add("data14_filtered_value", [_BUY, setp("E2:E9", "Formula", "z")], layout="data")
add("data14_filtered_sort", [_BUY, call("A1:E9", "Sort", Key1=R("C1"), Order1=xlDescending, Header=xlYes)], layout="data")
add("data14_filtered_cut", [_BUY, call("A3:E4", "Cut", Destination=R("A12"))], layout="data", read=["A1:K14"])
add("data14_filtered_insert_row", [_BUY, call("4:4", "Insert")], layout="data")
add("data15_filtered_formula", [_BUY, setp("E2:E9", "Formula", "=C2*2")], layout="data")
add("data15_filtered_autofill", [_BUY, call("E2", "AutoFill", Destination=R("E2:E9"))], layout="data")
add("data15_filtered_replace", [_BUY, call("A1:E9", "Replace", What="Sell", Replacement="Short", LookAt=xlWhole)], layout="data")
add("data15_filtered_numberformat", [_BUY, setp("C2:C9", "NumberFormat", "0.00")], layout="data")
add("data15_filtered_clearformats", [_BUY, call("D2:D9", "ClearFormats")], layout="data")
add("data15_filtered_insert_cells", [_BUY, call("C3:C4", "Insert", Shift=xlShiftDown)], layout="data")
add("data15_filtered_delete_cells", [_BUY, call("C3:C4", "Delete", Shift=xlShiftUp)], layout="data")
add("data15_filtered_dedupe", [_BUY, call("A1:E9", "RemoveDuplicates", Columns=2, Header=xlYes)], layout="data")
add("data15_filtered_paste_values", [_BUY, call("D2:D9", "Copy"), call("E2:E9", "PasteSpecial", Paste=xlPasteValues)],
    layout="data")
add("data15_filtered_fillright", [_BUY, call("C2:E9", "FillRight")], layout="data")
add("data15_hidden_rows_clear", [setp("3:4", "RowHeight", 0), call("C2:C6", "ClearContents")], layout="data")
add("data15_hidden_rows_filldown", [setp("3:4", "RowHeight", 0), call("E2:E6", "FillDown")], layout="data")
add("data16_filtered_insert_shown_cells", [_BUY, call("C4:C5", "Insert", Shift=xlShiftDown)], layout="data")
add("data16_filtered_insert_right", [_BUY, call("C3:C4", "Insert", Shift=xlShiftToRight)], layout="data")
add("data16_filtered_delete_left", [_BUY, call("C3:C4", "Delete", Shift=xlShiftToLeft)], layout="data")
add("data16_filtered_delete_shown_up", [_BUY, call("C8:C9", "Delete", Shift=xlShiftUp)], layout="data")
add("data16_filtered_formula_abs", [_BUY, setp("E2:E9", "Formula", "=$C$2+C2")], layout="data")
add("data16_filtered_value2", [_BUY, setp("E2:E9", "Value2", 7)], layout="data")
add("data16_hidden_rows_formula", [setp("3:4", "RowHeight", 0), setp("E2:E6", "Formula", "=C2*2")], layout="data")
add("data16_hidden_rows_numberformat", [setp("3:4", "RowHeight", 0), setp("C2:C6", "NumberFormat", "0.00")], layout="data")

# ---- Data Table (What-If Analysis) with one and two inputs ------------------------------------------------------------------
LAYOUTS["whatif"] = {"cells": {"A1": "5", "A2": "2", "B4": "=A1*A2", "A5": "1", "A6": "2", "A7": "3", "C4": "=A1+A2",
                               "E4": "=A1^A2", "F4": "1", "G4": "2", "E5": "3", "E6": "4"}, "read": ["A1:H8"]}
add("data14_table_column", [call("A4:C7", "Table", ColumnInput=R("A1"))], layout="whatif")
add("data14_table_two", [call("E4:G6", "Table", RowInput=R("A2"), ColumnInput=R("A1"))], layout="whatif")


# ---- Database functions (DSUM...DVARP) over the data list: fields by name and number, criteria, refusals -------------
_DB = dict(_DATA)
_DB.update({"M1": "Side", "M2": "Buy", "N1": "Qty", "N2": ">1", "O1": "Sym", "O2": "XYZ", "P1": "Side", "P2": "Sell",
            "P3": "Buy"})
_DB_FORMULAS = [
    "=DVAR(A1:E9,\"Px\",M1:M2)", "=DSTDEVP(A1:E9,\"Qty\",M1:M2)", "=DVARP(A1:E9,4,M1:M2)", "=DSTDEV(A1:E9,\"qty\",M1:N2)",
    "=DSUM(A1:E9,3,P1:P3)", "=DAVERAGE(A1:E9,0,M1:M2)", "=DMAX(A1:E9,\"Nope\",M1:M2)", "=DMIN(A1:E9,9,M1:M2)",
    "=DCOUNT(A1:E9,,M1:M2)", "=DCOUNTA(A1:E9,\"Note\",M1:M2)", "=DGET(A1:E9,\"Px\",M1:M2)", "=DPRODUCT(A1:E9,\"Qty\",M1:N2)",
    "=DVAR(A1:E9,\"Px\",O1:O2)", "=DSTDEVP(A1:E9,\"Px\",O1:O2)", "=DSUM(A1:E9,1/0,M1:M2)", "=DSTDEVP(A1:E9,\"Qty\",N1:N2)",
    "=DCOUNT(A1:E9,\"Sym\",M1:M2)", "=DGET(A1:E9,\"Px\",N1:N2)", "=DSUM(A1:E9,2.9,M1:M2)", "=DMAX(A1:E9,\"Px\",A1:A1)",
]
for _i, _f in enumerate(_DB_FORMULAS):
    _DB[f"R{_i + 1}"] = _f
LAYOUTS["dbfn"] = {"cells": _DB, "read": [f"R1:R{len(_DB_FORMULAS)}"]}
add("dbfn_all", [], layout="dbfn")
add("dbfn_after_edit", [setp("D5", "Value2", 400), setp("C2", "Value2", "x")], layout="dbfn")


# ---- Remove Duplicates over error values: formulas' errors, error constants, text that reads as one (rand3_010) -------
LAYOUTS["dedupe_err"] = {
    "cells": {"A1": "Key", "B1": "Val", "A2": "=NA()", "A3": "=NA()", "A4": "=1/0", "A5": "=1/0", "A6": "#N/A",
              "A7": "#N/A", "A8": "'#N/A", "A9": "x", "A10": "x", "A11": "=1/0",
              **{f"B{r}": str(r) for r in range(2, 12)}, "C2": "1", "C3": "1", "C4": "2", "C5": "2", "C6": "3",
              "C7": "3", "C8": "3", "C9": "4", "C10": "4", "C11": "2"},
    "read": ["A1:C12"],
}
add("dedupe_err_key", [call("A1:B11", "RemoveDuplicates", Columns=1, Header=xlYes)], layout="dedupe_err")
add("dedupe_err_two", [call("A1:C11", "RemoveDuplicates", Columns=[1, 3], Header=xlYes)], layout="dedupe_err")
add("dedupe_err_other", [call("A1:C11", "RemoveDuplicates", Columns=3, Header=xlYes)], layout="dedupe_err")
# the errors that came with dynamic arrays: a blocked spill (#SPILL!), an empty FILTER (#CALC!), and one read through
add("dedupe_err_spill", [call("A1:B9", "RemoveDuplicates", Columns=1, Header=xlYes)], layout="dedupe_err",
    cells={"A1": "Key", "B1": "Val", "A2": "=SEQUENCE(2)", "C3": "x", "A4": "=SEQUENCE(2)", "A6": "=A2", "A7": "=FILTER(B2:B3,B2:B3>99)",
           "A8": "=FILTER(B2:B3,B2:B3>99)", "A9": "=A7", **{f"B{r}": str(r) for r in range(2, 10)}, "A3": "1", "A5": "1"},
    read=["A1:C10"])
# rand3_010: a row whose error comes through a formula reading the first row's error, in the base layout
_RD3 = call("A1:G6", "RemoveDuplicates", Columns=3, Header=xlYes)
add("dedupe_err_base_spill_cleared", [call("C5", "Clear"), setp("C2", "Formula", "=SEQUENCE(3)"), _RD3])
add("dedupe_err_base_spill", [setp("C2", "Formula", "=SEQUENCE(3)"), _RD3])
add("dedupe_err_base_hidden", [setp("8:8", "RowHeight", 0), setp("C2", "Formula", "=SEQUENCE(3)"), _RD3])
add("dedupe_err_base_na", [setp("C2", "Formula", "=NA()"), setp("C5", "Formula", "=NA()"), _RD3])
add("dedupe_err_base_div", [setp("C2", "Formula", "=1/0"), _RD3])
add("dedupe_err_base_typed", [setp("C2", "Formula", "#N/A"), setp("C6", "Formula", "#N/A"), _RD3])
add("dedupe_base_last_row", [setp("C3", "Formula", "9"), setp("C6", "Formula", "9"), _RD3])
add("dedupe_base_mid_row", [setp("C3", "Formula", "9"), setp("C5", "Formula", "9"), _RD3])
add("dedupe_base_typed_mid", [setp("C2", "Formula", "#N/A"), setp("C5", "Formula", "#N/A"), _RD3])
add("dedupe_base_with_blank", [setp("C2", "Formula", "#N/A"), setp("C6", "Formula", "#N/A"),
                               call("A1:G7", "RemoveDuplicates", Columns=3, Header=xlYes)])
_TOT = {"A1": "K", "B1": "V", "C1": "W", "A2": "a", "A3": "b", "A4": "c", "A5": "d", "B2": "1", "B3": "2", "B4": "3", "B5": "4",
        "C2": "5", "C3": "6", "C4": "7", "C5": "8", "A6": "d", "B6": "9", "C6": "9"}
LAYOUTS["dedupe_tot"] = {"cells": _TOT, "read": ["A1:C8"]}
_RD1 = call("A1:C6", "RemoveDuplicates", Columns=1, Header=xlYes)
for _cid, _cells in (("plain", {}), ("sum", {"B6": "=SUM(B2:B5)"}), ("avg", {"C6": "=AVERAGE(C2:C5)"}), ("ref", {"B6": "=B5"}),
                     ("const", {"B6": "=1+1"}), ("subtotal", {"B6": "=SUBTOTAL(9,B2:B5)"}), ("part", {"B6": "=SUM(B2:B3)"}),
                     ("max", {"B6": "=MAX(B2:B5)"}), ("count", {"B6": "=COUNT(B2:B5)"}), ("sum_text", {"B6": '=SUM(B2:B5)&""'}),
                     ("both", {"B6": "=SUM(B2:B5)", "C6": "=SUM(C2:C5)"}), ("key_sum", {"A6": "=A5", "B6": "=SUM(B2:B5)"}),
                     ("other_col", {"B6": "=SUM(C2:C5)"}), ("whole_col", {"B6": "=SUM(B:B)"})):
    add(f"dedupe_tot_{_cid}", [_RD1], layout="dedupe_tot", cells={**_TOT, **_cells})
for _cid, _cells in (("mid_dup", {"A4": "d", "B5": "=SUM(B2:B3)", "A6": "x"}), ("outside", {"B6": "=SUM(E1:E2)"}),
                     ("one_cell_fn", {"B6": "=SUM(B2)"}), ("two_refs", {"B6": "=B2+B3"}),
                     ("three", {"A4": "d", "B5": "=SUM(B2:B4)", "B6": "=SUM(B2:B5)"}),
                     ("name_range", {"B6": "=ROWS(B2:B5)"}), ("text_last", {"B6": "=\"x\""}),
                     ("other_sheet", {"B6": "=SUM(Other!A1:A2)"})):
    add(f"dedupe_tot_{_cid}", [_RD1], layout="dedupe_tot", cells={**_TOT, **_cells})
for _fn in ("MIN", "COUNTA", "PRODUCT", "STDEV", "VAR", "MEDIAN", "SUMPRODUCT", "LARGE", "STDEV.S", "VAR.P", "COUNTBLANK",
            "SUMSQ", "AVERAGEA", "MODE", "STDEVP", "VARP", "MAXA", "AGGREGATE", "SUMIF", "COUNTIF", "AVERAGEIF", "MINIFS",
            "SUMIFS", "DSUM", "SUBTOTAL109", "IFSUM"):
    _f = {"LARGE": "=LARGE(B2:B5,1)", "AGGREGATE": "=AGGREGATE(9,0,B2:B5)", "SUMIF": "=SUMIF(B2:B5,\">0\")",
          "COUNTIF": "=COUNTIF(B2:B5,\">0\")", "AVERAGEIF": "=AVERAGEIF(B2:B5,\">0\")", "MINIFS": "=MINIFS(B2:B5,B2:B5,\">0\")",
          "SUMIFS": "=SUMIFS(B2:B5,B2:B5,\">0\")", "DSUM": "=DSUM(A1:C5,2,A1:A2)", "SUBTOTAL109": "=SUBTOTAL(109,B2:B5)",
          "IFSUM": "=IF(TRUE,SUM(B2:B5))"}.get(_fn, f"={_fn}(B2:B5)")
    add(f"dedupe_tot_fn_{_fn.replace('.', '_').lower()}", [_RD1], layout="dedupe_tot", cells={**_TOT, "B6": _f})
add("dedupe_tot_mid", [_RD1], layout="dedupe_tot", cells={**_TOT, "A4": "d", "B4": "=SUM(B2:B3)", "A6": "x"})
add("dedupe_tot_two_cols", [call("A1:C6", "RemoveDuplicates", Columns=[1, 2], Header=xlYes)], layout="dedupe_tot",
    cells={**_TOT, "B5": "=SUM(B2:B4)", "B6": "=SUM(B2:B5)"})
add("dedupe_tot_no_header", [call("A2:C6", "RemoveDuplicates", Columns=1, Header=xlNo)], layout="dedupe_tot",
    cells={**_TOT, "B6": "=SUM(B2:B5)"})
add("dedupe_tot_sort", [call("A1:C6", "Sort", Key1=R("A1"), Order1=xlDescending, Header=xlYes)], layout="dedupe_tot",
    cells={**_TOT, "B6": "=SUM(B2:B5)"})
# Sort and Remove Duplicates over ranges that meet a table (rand3_006, rand3_195), and Sort over a one-cell array (rand3_143)
def _lo(src):
    return call("#sheet", "ListObjects.Add", SourceType=1, Source=R(src), XlListObjectHasHeaders=xlYes)


_SORT = call("A1:G6", "Sort", Key1=R("B1"), Order1=xlDescending, Header=xlYes)
_DD = call("A1:G6", "RemoveDuplicates", Columns=6, Header=xlYes)
for _cid, _tbl in (("inside", "B2:C3"), ("part", "F4:H6"), ("exact", "A1:G6"), ("cols", "A1:B6"), ("rows", "A1:G4"),
                   ("below", "A6:B8")):
    add(f"tblrange_sort_{_cid}", [_lo(_tbl), _SORT])
    add(f"tblrange_dedupe_{_cid}", [_lo(_tbl), _DD])
add("tblrange_sort_body", [_lo("A1:G6"), call("A2:G5", "Sort", Key1=R("B2"), Order1=xlDescending, Header=xlNo)])
add("tblrange_dedupe_body", [_lo("A1:G6"), call("A2:G5", "RemoveDuplicates", Columns=6, Header=xlNo)])
add("tblrange_sort_other", [_lo("I1:J3"), _SORT])
# a fill into the row under a table (rand3_123)
add("tblfill_down_over_value", [_lo("E1:E3"), call("E3:E4", "FillDown")])
add("tblfill_down_empty", [call("E4", "ClearContents"), _lo("E1:E3"), call("E3:E4", "FillDown")])
add("tblfill_down_two_cols", [_lo("E1:F3"), call("E3:F4", "FillDown")])
add("tblfill_down_two_rows", [_lo("E1:E3"), call("E3:E5", "FillDown")])
add("tblfill_autofill", [_lo("E1:E3"), call("E3", "AutoFill", Destination=R("E3:E5"))])
add("tblfill_write_below", [_lo("E1:E3"), setp("E4", "Formula", "7")])
add("tblfill_right_over_value", [_lo("D1:E3"), call("E2:F2", "FillRight")])
add("tblfill_down_inside", [_lo("E1:E5"), call("E3:E4", "FillDown")])
add("tblfill_down_part_width", [_lo("E1:F3"), call("E3:E4", "FillDown")])
add("tblfill_down_then_empty", [call("E5", "ClearContents"), _lo("E1:E3"), call("E3:E5", "FillDown")])
add("tblfill_down_empty_then", [call("E4", "ClearContents"), _lo("E1:E3"), call("E3:E5", "FillDown")])
add("tblfill_down_from_inside", [_lo("E1:E3"), call("E2:E4", "FillDown")])
add("tblfill_down_wider", [_lo("E1:E3"), call("D3:F4", "FillDown")])
# arrays entered with Ctrl+Shift+Enter that read their own cells (rand3_080, rand3_104, rand3_112)
add("cyc_array_col", [setp("C2:C4", "FormulaArray", "=B2:C4*2")])
add("cyc_array_one_cell", [setp("C2", "FormulaArray", "=C2+1")])
add("cyc_plain_one_cell", [setp("C2", "Formula", "=C2+1")])
add("cyc_array_sum", [setp("A1:B2", "FormulaArray", "=SUM(B2:B5)")])
add("cyc_array_self", [setp("C2:D3", "FormulaArray", "=C2:D3+1")])
add("cyc_array_shifted", [setp("C1:D3", "FormulaArray", "=B2:C4*2")])
add("cyc_array_then_edit", [setp("C2:C4", "FormulaArray", "=B2:C4*2"), setp("B2", "Formula", "11")])
# the formats of the cells a table grows over (rand3_077)
add("tblgrow_right_fmt", [_lo("C6:C7"), setp("D6:D7", "Formula", "12.5")])
add("tblgrow_right_own_fmt", [setp("D6", "NumberFormat", "0%"), _lo("C6:C7"), setp("D6:D7", "Formula", "12.5")])
add("tblgrow_down_fmt", [call("C4", "ClearFormats"), _lo("B1:C3"), setp("C4", "Formula", "7")])
add("tblgrow_down_own_fmt", [setp("C4", "NumberFormat", "0%"), _lo("B1:C3"), setp("C4", "Formula", "7")])
_GROW_REFS = ["=SUM(B2:C3)", "=SUM(C2:C3)", "=SUM(C3:C3)", "=C3", "=SUM(C1:C3)", "=SUM(C2:C2)", "=SUM(B2:D3)",
              "=SUM($C$2:$C$3)", "=SUM(C2:C4)", "=ROWS(B2:B3)"]
_GROW_SET = [setp(f"I{i + 1}", "Formula", f) for i, f in enumerate(_GROW_REFS)]
add("tblgrow_refs_write", _GROW_SET + [call("C4", "ClearFormats"), _lo("B1:C3"), setp("C4", "Formula", "7")],
    read=["A1:J14"])
add("tblgrow_refs_fill", _GROW_SET + [_lo("B1:C3"), call("B3:C4", "FillDown")], read=["A1:J14"])
add("tblgrow_refs_other", [setp("Other!E1", "Formula", "=ROWS(T!C2:C3)"), setp("Other!E2", "Formula", "=ROWS(T!C3:C3)"),
                           call("#sheet", "Names.Add", Name="Rng", RefersTo="=T!$C$2:$C$3"), setp("I1", "Formula", "=ROWS(Rng)"),
                           call("#sheet", "Names.Add", Name="Rel", RefersTo="=T!C2:C3"), setp("I2", "Formula", "=ROWS(Rel)"),
                           _lo("B1:C3"), setp("C4", "Formula", "7")], read=["A1:J14", "Other!E1:E2"])
add("tblgrow_refs_right", [setp("I1", "Formula", "=SUM(B2:C3)"), setp("I2", "Formula", "=SUM(B2:B3)"), setp("I3", "Formula", "=SUM(B1:C1)"),
                           call("D1:D3", "ClearContents"), _lo("B1:C3"), setp("D2", "Formula", "7")])
add("tblgrow_refs_resize",_GROW_SET + [_lo("B1:C3"), call("C4", "Copy", Destination=R("C5"))], read=["A1:J14"])
# references an insertion pushes past the sheet's edge (doc_fuzz seed 33240081)
add("edge_insert_col_range", [setp("J10", "Formula", "=SUM(B1:XFD1)"), call("C:C", "Insert")], read=["A1:L14"])
add("edge_insert_col_cell", [setp("J10", "Formula", "=XFD1+1"), call("C:C", "Insert")], read=["A1:L14"])
# cells as wide (tall) as the sheet: inserted to the right (deleted to the left) they are whole rows? (measured)
add("edge_delete_cells_left", [setp("J10", "Formula", "=B13*2"), call("A11:XFD12", "Delete", Shift=xlShiftToLeft)])
add("edge_insert_cells_down", [setp("J10", "Formula", "=D5*2"), call("C1:C1048576", "Insert", Shift=xlShiftDown)],
    read=["A1:L14"])
add("edge_delete_cells_up", [setp("J10", "Formula", "=D5*2"), call("C1:C1048576", "Delete", Shift=xlShiftUp)],
    read=["A1:L14"])
add("edge_insert_cells_right", [setp("J10", "Formula", "=B11*2"), setp("J9", "Formula", "=SUM(B12:XFD12)"),
                                call("A11:XFD12", "Insert", Shift=xlShiftToRight)])
add("edge_insert_rows_range", [setp("J1", "Formula", "=SUM(I2:I1048576)"), setp("J2", "Formula", "=I1048576+1"),
                               call("5:5", "Insert")])
add("tblsort_one_col", [_lo("A1:G6"), call("C2:C4", "Sort", Key1=R("C2"), Order1=xlDescending, Header=xlNo)])
add("tblsort_header_yes", [_lo("A1:G6"), call("A2:G4", "Sort", Key1=R("B2"), Order1=xlDescending, Header=xlYes)])
add("sort_one_cell_array", [setp("F4", "FormulaArray", "=B2:C4*2"), _SORT])
add("sort_one_cell_array_key", [setp("B4", "FormulaArray", "=SUM(C2:C3)"), _SORT])
add("dedupe_one_cell_array", [setp("F4", "FormulaArray", "=B2:C4*2"), _DD])
add("dedupe_base_last_plain", [setp("A6:G6", "Formula", "1"), setp("A5:G5", "Formula", "1"), _RD3])

# ---- A table whose header row holds formulas: the headers become text (rand3_025) --------------------------------------
add("table_formula_headers", [call("#sheet", "ListObjects.Add", SourceType=1, Source=R("A6:F8"), XlListObjectHasHeaders=xlYes)])
add("table_formula_headers_same", [setp("B6:D6", "Formula", "=B2"), setp("E6", "Formula", "=1/0"), setp("F6", "Formula", "=E6"),
                                   call("#sheet", "ListObjects.Add", SourceType=1, Source=R("A6:F8"), XlListObjectHasHeaders=xlYes)])
add("table_formula_headers_typed", [setp("C6", "Formula", "=B2:B4"),
                                    call("#sheet", "ListObjects.Add", SourceType=1, Source=R("B2:E4"), XlListObjectHasHeaders=xlYes)])


# ---- Random sequences: the basic operations in orders nobody wrote down -----------------------------------------------
# Each case is three to six operations picked at random (a fixed seed per case, so the case and its measurement stay
# the same) over the base layout: insert / delete rows, columns and cells, Copy and Cut with a Destination, Clear,
# Fill Down / Right, AutoFill, Sort, and typed values, formulas and formats. Measured in Excel like every case above,
# they find what no single-operation case does (an insert after a cut, a fill over a moved formula). To look further,
# raise RANDOM_CASES and measure the new ones: scripts/measure_excel_basics.py --only rand_
RANDOM_CASES = 400
_R_FORMULAS = ["=B2*2", "=SUM(B2:C4)", "=$B$2+C3", "=A2&B2", "=SUM(B:B)", "=Rate*3", "=IF(B3>15,D2,E2)", "=COUNTA(A1:A8)",
               "=INDEX(B2:D5,2,2)", "=B$3+$C4", "=Other!A1*2", "=D6-D2", "=AVERAGE(B2:B5)", "=MAX(C2:C5)+1"]
_R_VALUES = ["7", "12.5", "-3", "Kiwi", "0.25", "45000", "TRUE", "1E3"]
_R_FORMATS = ["0.00", "0%", "#,##0", "yyyy-mm-dd", "@", "General"]


def _random_step_maker(rng):
    """one() makes a step of the first family (its random draws in the order the cases were measured with)."""

    def cell():
        return f"{col_name(rng.randrange(0, 7))}{rng.randrange(1, 10)}"

    def block():
        r0, c0 = rng.randrange(1, 9), rng.randrange(0, 6)
        r1, c1 = r0 + rng.randrange(0, 3), c0 + rng.randrange(0, 2)
        a, b = f"{col_name(c0)}{r0}", f"{col_name(c1)}{r1}"
        return a if a == b else f"{a}:{b}", (r1 - r0 + 1, c1 - c0 + 1)

    def at(size):
        r0, c0 = rng.randrange(1, 10), rng.randrange(0, 7)
        a, b = f"{col_name(c0)}{r0}", f"{col_name(c0 + size[1] - 1)}{r0 + size[0] - 1}"
        return a if a == b else f"{a}:{b}"

    def one():
        k = rng.randrange(14)
        if k == 0:
            r = rng.randrange(1, 10)
            return (call(f"{r}:{r}", rng.choice(["Insert", "Delete"])))
        elif k == 1:
            c = col_name(rng.randrange(0, 7))
            return (call(f"{c}:{c}", rng.choice(["Insert", "Delete"])))
        elif k == 2:
            b, _ = block()
            return (call(b, "Insert", Shift=rng.choice([xlShiftDown, xlShiftToRight])))
        elif k == 3:
            b, _ = block()
            return (call(b, "Delete", Shift=rng.choice([xlShiftUp, xlShiftToLeft])))
        elif k == 4:
            b, size = block()
            return (call(b, "Copy", Destination=R(at(size))))
        elif k == 5:
            b, size = block()
            return (call(b, "Cut", Destination=R(at(size))))
        elif k == 6:
            b, _ = block()
            return (call(b, rng.choice(["ClearContents", "Clear"])))
        elif k == 7:
            r0, c = rng.randrange(1, 8), col_name(rng.randrange(0, 6))
            return (call(f"{c}{r0}:{c}{r0 + rng.randrange(1, 4)}", "FillDown"))
        elif k == 8:
            r, c0 = rng.randrange(1, 9), rng.randrange(0, 5)
            return (call(f"{col_name(c0)}{r}:{col_name(c0 + rng.randrange(1, 3))}{r}", "FillRight"))
        elif k == 9:
            r0, c = rng.randrange(1, 8), col_name(rng.randrange(0, 6))
            src = f"{c}{r0}" if rng.randrange(2) else f"{c}{r0}:{c}{r0 + 1}"
            return (call(src, "AutoFill", Destination=R(f"{c}{r0}:{c}{r0 + rng.randrange(2, 5)}")))
        elif k == 10:
            key = col_name(rng.randrange(0, 4))
            return (call("A1:G6", "Sort", Key1=R(f"{key}1"), Order1=rng.choice([xlAscending, xlDescending]), Header=xlYes))
        elif k == 11:
            return (setp(cell(), "Formula", rng.choice(_R_FORMULAS)))
        elif k == 12:
            return (setp(cell(), "Formula", rng.choice(_R_VALUES)))
        else:
            b, _ = block()
            return (setp(b, "NumberFormat", rng.choice(_R_FORMATS)))
    return one


def _random_steps(rng):
    one = _random_step_maker(rng)
    return [one() for _ in range(rng.randrange(3, 7))]


# ---- A second family: what the first leaves out (merged cells, a filtered list, fills up and left, Replace, a value
# written over a block), mixed with the first family's steps. Measured 2026-11-21 ----------------------------------------
RANDOM2_CASES = 200


def _random_steps2(rng):
    one = _random_step_maker(rng)

    def block():
        r0, c0 = rng.randrange(1, 9), rng.randrange(0, 6)
        r1, c1 = r0 + rng.randrange(0, 3), c0 + rng.randrange(0, 2)
        a, b = f"{col_name(c0)}{r0}", f"{col_name(c1)}{r1}"
        return a if a == b else f"{a}:{b}"

    steps = []
    for _ in range(rng.randrange(3, 7)):
        k = rng.randrange(10)
        if k < 5:
            steps.append(one())
        elif k == 5:
            r0, c0 = rng.randrange(1, 9), rng.randrange(0, 6)
            steps.append(call(f"{col_name(c0)}{r0}:{col_name(c0 + 1)}{r0 + rng.randrange(0, 2)}", "Merge"))
        elif k == 6:
            if rng.randrange(2):
                r0, c = rng.randrange(1, 8), col_name(rng.randrange(0, 6))
                steps.append(call(f"{c}{r0}:{c}{r0 + rng.randrange(1, 4)}", "FillUp"))
            else:
                r, c0 = rng.randrange(1, 9), rng.randrange(0, 5)
                steps.append(call(f"{col_name(c0)}{r}:{col_name(c0 + rng.randrange(1, 3))}{r}", "FillLeft"))
        elif k == 7:
            steps.append(call("A1:G6", "AutoFilter", Field=rng.randrange(1, 4), Criteria1=rng.choice([">15", "<>Pear", ">=2"])))
        elif k == 8:
            # what each pair makes stays a valid formula: a sheet name or a name changed into one that doesn't exist makes
            # Excel ask for a file to update its values, and with alerts off it stops the Replace there
            what, repl = rng.choice([("5", "9"), ("0", "7"), ("Pear", "Plum"), ("Apple", "Kiwi")])
            steps.append(call(block(), "Replace", What=what, Replacement=repl, LookAt=2))
        else:
            steps.append(setp(block(), "Formula", rng.choice(_R_VALUES + _R_FORMULAS[:4])))
    return steps


# ---- A third family: objects that own cells (dynamic arrays and their spills, a table, an array entered with
# Ctrl+Shift+Enter), hidden rows and columns and Remove Duplicates, mixed with the first family's steps -------------------
RANDOM3_CASES = 200
_R3_SPILLS = ["=SEQUENCE(3)", "=B2:B5*2", "=SORT(A2:A5)", "=UNIQUE(B2:B6)", "=FILTER(B2:B5,B2:B5>15)", "=TRANSPOSE(B2:C3)",
              '=A2:A5&"x"', "=SEQUENCE(2,3)", "=ROWS(H9#)", "=SUM(B2:B5*C2:C5)"]


def _random_steps3(rng):
    one = _random_step_maker(rng)

    def block(rows, cols):
        r0, c0 = rng.randrange(1, 9), rng.randrange(0, 7)
        r1, c1 = r0 + rng.randrange(*rows), c0 + rng.randrange(*cols)
        a, b = f"{col_name(c0)}{r0}", f"{col_name(c1)}{r1}"
        return a if a == b else f"{a}:{b}"

    steps = []
    for _ in range(rng.randrange(3, 7)):
        k = rng.randrange(12)
        if k < 5:
            steps.append(one())
        elif k in (5, 6):
            cell = f"{col_name(rng.randrange(0, 9))}{rng.randrange(1, 12)}"
            steps.append(setp(cell, "Formula", rng.choice(_R3_SPILLS)))
        elif k == 7:
            steps.append(call("#sheet", "ListObjects.Add", SourceType=1, Source=R(block((1, 4), (0, 3))),
                              XlListObjectHasHeaders=xlYes))
        elif k == 8:
            if rng.randrange(2):
                r = rng.randrange(1, 10)
                steps.append(setp(f"{r}:{r}", "RowHeight", 0))
            else:
                c = col_name(rng.randrange(0, 7))
                steps.append(setp(f"{c}:{c}", "ColumnWidth", 0))
        elif k == 9:
            steps.append(setp(block((0, 3), (0, 2)), "FormulaArray", rng.choice(["=B2:C4*2", "=SUM(B2:B5)", "=A2:A4"])))
        elif k == 10:
            steps.append(call("A1:G6", "RemoveDuplicates", Columns=rng.randrange(1, 4), Header=xlYes))
        else:
            steps.append(setp(block((0, 3), (0, 2)), "Formula", rng.choice(_R_VALUES + _R3_SPILLS[:3])))
    return steps


def _add_random_cases():
    import random

    for i in range(RANDOM_CASES):
        add(f"rand_{i:03d}", _random_steps(random.Random(9000 + i)))
    for i in range(RANDOM2_CASES):
        add(f"rand2_{i:03d}", _random_steps2(random.Random(29000 + i)))
    for i in range(RANDOM3_CASES):
        add(f"rand3_{i:03d}", _random_steps3(random.Random(49000 + i)))


# ---- Resolving a case (shared by the measuring script and the replay test) ----------------------------------------------
def col_name(c):
    s = ""
    c += 1
    while c:
        c, r = divmod(c - 1, 26)
        s = chr(65 + r) + s
    return s


def parse_cell(a):
    m = re.fullmatch(r"([A-Z]+)(\d+)", a)
    col = 0
    for ch in m.group(1):
        col = col * 26 + ord(ch) - 64
    return int(m.group(2)) - 1, col - 1


def window_cells(rng):
    """(sheet, top, left, rows, cols, [[addr]]) of a read range such as "A1:J14" or "Other!D1:D2"."""
    sheet, _, ref = rng.rpartition("!")
    a, _, b = ref.partition(":")
    r0, c0 = parse_cell(a)
    r1, c1 = parse_cell(b or a)
    prefix = sheet + "!" if sheet else ""
    grid = [[f"{prefix}{col_name(c)}{r + 1}" for c in range(c0, c1 + 1)] for r in range(r0, r1 + 1)]
    return sheet or "T", r0, c0, r1 - r0 + 1, c1 - c0 + 1, grid


def plan(case):
    """A case resolved against its layout: what both the measuring script and the replay run."""
    layout = LAYOUTS[case["layout"]]
    cells = case.get("cells", layout["cells"])

    def key(a):
        r, c = parse_cell(a)
        return r, c

    flat = []
    for a in sorted(cells, key=key):
        v = cells[a]
        text, fmt = (v if isinstance(v, (list, tuple)) else (v, None))
        flat.append([a, text, fmt])
    names = [[n, r] for n, r in (layout.get("names") or {}).items()] if "cells" not in case else []
    other = [[a, t] for a, t in (layout.get("other") or {}).items()] if "cells" not in case else []
    reads = case.get("read", layout["read"])
    return {"id": case["id"], "cells": flat, "names": names, "other": other, "steps": case["steps"], "read": reads,
            "merge": bool(layout.get("merge")), "skip": case.get("skip", [])}


def plan_hash(p):
    return hashlib.sha1(json.dumps(p, sort_keys=True).encode()).hexdigest()[:12]


# ---- Arrays entered with Ctrl+Shift+Enter: copied, cut and filled whole or in part (doc_fuzz seed 10030116) ----------
_CSE = setp("A8:B10", "FormulaArray", "=A1:B3*2")
for cid, steps in (("copy_part", [_CSE, call("A6:B8", "Copy", Destination=R("C7"))]),
                   ("copy_whole", [_CSE, call("A8:B10", "Copy", Destination=R("D8"))]),
                   ("copy_anchor", [_CSE, call("A8", "Copy", Destination=R("D8"))]),
                   ("copy_member", [_CSE, call("B9", "Copy", Destination=R("D8"))]),
                   ("copy_row", [_CSE, call("A9:B9", "Copy", Destination=R("D12"))]),
                   ("copy_whole_and_more", [_CSE, call("A7:C10", "Copy", Destination=R("D7"))]),
                   ("ps_values_part", [_CSE, call("A8:B9", "Copy"), call("D8", "PasteSpecial", Paste=xlPasteValues)]),
                   ("ps_formulas_whole", [_CSE, call("A8:B10", "Copy"), call("D8", "PasteSpecial", Paste=xlPasteFormulas)]),
                   ("filldown_below", [_CSE, call("A10:B12", "FillDown")]),
                   ("fillright_beside", [_CSE, call("B8:D9", "FillRight")]),
                   ("autofill_below", [_CSE, call("A10:B10", "AutoFill", Destination=R("A10:B12"))]),
                   ("autofill_whole_down", [_CSE, call("A8:B10", "AutoFill", Destination=R("A8:B13"))])):
    add(f"cse_{cid}", steps, layout="cse")

_add_random_cases()  # after col_name, which the sequences use
