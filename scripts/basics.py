# SPDX-License-Identifier: Apache-2.0
r"""Basic operations in every state, checked by their result: the matrix behind "every basic operation works".

    <YCGrid>\runtime\python\python.exe -E -s scripts\basics.py [--exe path] [--shard K/N (K from 0)] [--only state:op,...]
                                                               [--states a,b] [--ops x,y] [--list]
                                                               [--sequences N --seed S] [--lang en|zh-TW]
                                                               [--commands]

Each case starts a fresh sheet with the same small table, puts YCGrid into one STATE the way a person gets there
(editing a cell, pointing in a formula, a range or whole rows selected, merged cells, a filter, frozen panes, a
protected sheet, read-only mode, read-only mode switched on while a cell is edited, grouped sheets, cells copied or
cut, a table, a spill range, a shape selected, Page Layout view, zoom...), runs one basic OPERATION with the keys or
the mouse a person uses (typing, F2, Enter / Tab / arrows, Delete, copy / cut / paste, Undo / Redo, fill, insert /
delete, formats, merge, sort, the context menu, the fill handle, dragging cells...), and checks what every basic
operation must keep, whatever it does:

  1. no message a person shouldn't see: every message YCGrid showed is in ALLOWED for that state and operation
     (with the reason Excel shows it too); any other message fails the case;
  2. no ImGui check failed (`imgui_asserts`), the UI answered, `ui.input` finished;
  3. two Escapes leave nothing behind: no cell editor, no dialog, no menu;
  4. Undo gives back exactly what was there before the operation (`debug.dump`, io::ContentDump: values, formulas,
     formats, rows, columns, merges, names, rules, objects), and Redo exactly what it made; an operation that changed
     the content without an Undo step fails (unless Excel can't undo it either: NO_UNDO);
  5. read-only mode changes nothing at all, whatever the keys.

--sequences N runs N seeded random sequences instead: a state, then eight operations one after another (each checked
for messages and ImGui), then Undo back to the start (the dump must be the first one again) and Redo to the end (the
dump after the last operation): what goes wrong only after several operations, such as an Undo step one of them
recorded badly.

--commands runs every command of the Home tab instead (the tab's buttons and their menus' entries, as the ribbon lists
them: `tab` in ui.commands()), each in COMMAND_STATES, with the same checks (a dialog it opens is cancelled): a command
added to Home is tried without a line here, and none is left out unless SKIP_COMMANDS says why.

--lang zh-TW runs it all with the Traditional Chinese UI (the messages are read back into English through
i18n/zh-TW.json, so ALLOWED holds one list for both languages).

A failure prints the case, what broke, the dump difference and a screenshot under %TEMP%\yc_basics\. Exit code 1 when
any case failed. A bug found in a basic operation is fixed with the case that shows it here: add the state or the
operation it needed (AGENTS.md, "Basic operations").
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
import random
import re
import sys
import tempfile
import time
import traceback

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "python"))

import yc  # noqa: E402
from yc import excel, safety, ui  # noqa: E402

OUT = os.path.join(tempfile.gettempdir(), "yc_basics")

# The table every case starts from (A1:E7) and a cell outside it
TABLE = [["Name", "Qty", "Price", "Total", "When"],
         ["AAPL", 10, 1.5, None, 45000],
         ["MSFT", 20, 2.25, None, 45001],
         ["TSMC", 30, 3, None, 45002],
         ["NVDA", 40, 4.75, None, 45003],
         ["AMZN", 50, 5, None, 45004],
         ["Sum", None, None, None, None]]


# ---------------------------------------------------------------------------
# States: how a person gets there. Each is (python setup, keys) run on the fresh sheet; the keys go through ui.input.
# ---------------------------------------------------------------------------

def _st_merged(ws):
    ws.Range("B9:C10").Merge()


def _st_filter(ws):
    ws.Range("A1:E6").AutoFilter(1, "<>MSFT")


def _st_hidden(ws):
    ws.Rows(4).Hidden = True


def _st_protected(ws):
    ws.Range("F1:H20").Locked = False
    ws.Protect()


def _st_grouped(ws):
    other = excel.Worksheets.Add(After=ws)
    other.Name = ws.Name + "g"
    ws.Activate()


def _st_table(ws):
    ws.ListObjects.Add(1, ws.Range("A1:E6"), None, 1)


def _st_spill(ws):
    ws.Range("G1").Formula2 = "=SEQUENCE(4)"


def _st_cse(ws):
    ws.Range("H2:H4").FormulaArray = "=B2:B4*2"


def _st_shape(ws):
    ws.Shapes.AddShape(1, 300, 20, 80, 40)


def _st_note(ws):
    ws.Range("B3").AddComment("check")


def _st_zoom(ws):
    excel.ActiveWindow.Zoom = 200


def _formula_bar_keys():
    fx = [i for i in ui.items("") if i.get("id") == "##fb_fx" or i.get("label") == "##fb_fx"]
    if not fx:
        return "g:B3|k:F2"
    r = fx[0]["rect"]
    x, y = int(r[2]) + 120, int((r[1] + r[3]) / 2)
    return f"g:B3|w:2|m:{x},{y}|w:2|c:{x},{y}|w:4|k:End"


def _st_hidden_col(ws):
    ws.Columns(3).Hidden = True


def _st_outline(ws):
    ws.Rows("3:5").Group()
    ws.Outline.ShowLevels(1)


def _st_manual(ws):
    excel.Application.Calculation = excel.xlCalculationManual
    ws.Range("B2").Value2 = 99  # D2 waits to be calculated


def _st_list(ws):
    ws.Range("F2:F6").Validation.Add(3, 1, 1, "Buy,Sell,Hold")


def _st_chart(ws):
    co = ws.ChartObjects.Add(320, 20, 240, 150)
    co.Chart.SetSourceData(ws.Range("A1:B6"))
    co.Select()


def _st_long_text(ws):
    ws.Range("B3").Value2 = "a long note about the position that runs past the column " * 3
    ws.Range("B3").WrapText = True


def _st_used_to_end(ws):
    # a value in the sheet's last cell: the used range is the whole sheet, and an operation over whole columns, rows or
    # the sheet that walks its cells one by one meets 17 billion of them (borders, Merge, Fill Right, Remove Duplicates
    # and Column Differences each froze so, found one by one)
    ws.Range("XFD1048576").Value2 = "far"


def _st_text_number(ws):
    ws.Range("B3").NumberFormat = "@"
    ws.Range("B3").Value2 = "00123"


def _st_link(ws):
    ws.Hyperlinks.Add(ws.Range("B3"), "https://example.com", TextToDisplay="site")


def _st_totals(ws):
    lo = ws.ListObjects.Add(1, ws.Range("A1:E6"), None, 1)
    lo.ShowTotals = True


def _st_rules(ws):
    ws.Range("B2:B6").Validation.Add(1, 1, 1, "0", "1000")  # whole numbers 0..1000
    ws.Range("C2:C6").FormatConditions.Add(1, 5, "=2")


STATES = {
    "plain": (None, "g:B3"),
    "range": (None, "g:B2:C4"),
    "rows": (None, "g:3:4"),
    "cols": (None, "g:B:C"),
    "areas": (None, "g:B2|H:ctrl|A:D4|A:D5|h:"),
    "all": (None, "g:A1|k:c-A"),
    "typing": (None, "g:F3|t:12"),
    "typing_text": (None, "g:B3|t:abc"),
    "f2": (None, "g:A3|k:F2|k:End"),
    "pointing": (None, "g:F3|t:=SUM(|k:UpArrow|k:s-UpArrow"),
    "formula_bar": (None, _formula_bar_keys),
    "merged": (_st_merged, "g:B9"),
    "filtered": (_st_filter, "g:B3"),
    "hidden_row": (_st_hidden, "g:B3"),
    "frozen": (None, "g:B2|x:FreezePanes|g:C4"),
    "protected": (_st_protected, "g:B3"),
    "protected_unlocked": (_st_protected, "g:G3"),
    "read_only": (None, "g:B3|x:OperatorMode"),
    "read_only_typing": (None, "g:F3|t:12|x:OperatorMode"),
    "read_only_f2": (None, "g:A3|k:F2|x:OperatorMode"),
    "read_only_pointing": (None, "g:F3|t:=SUM(|k:UpArrow|x:OperatorMode"),
    "grouped": (_st_grouped, "g:B3|k:c-s-PageDown|w:2|k:c-PageUp|g:B3"),  # Ctrl+Shift+PgDn groups the next sheet
    "copied": (None, "g:B2:B3|k:c-C|g:F2"),
    "cut": (None, "g:B2:B3|k:c-X|g:F2"),
    "copied_row": (None, "g:3:3|k:c-C|g:B9"),
    "table": (_st_table, "g:B3"),
    "spill": (_st_spill, "g:G2"),
    "array": (_st_cse, "g:H3"),
    "shape": (_st_shape, "g:B3|x:SelectObjects"),
    "note": (_st_note, "g:B3"),
    "rules": (_st_rules, "g:B3"),
    "page_layout": (None, "x:ViewPageLayout|g:B3"),
    "zoomed": (_st_zoom, "g:B3"),
    "after_paste": (None, "g:B2|k:c-C|g:F5|k:c-V"),
    "far": (None, "g:XFD1048576"),
    "ime": (None, "g:F3|I:\u53f0"),  # a CJK composition in progress
    "hidden_col": (_st_hidden_col, "g:B3"),
    "outline": (_st_outline, "g:B2"),
    "manual_calc": (_st_manual, "g:B3"),
    "list": (_st_list, "g:F3"),
    "autocomplete": (None, "g:F3|t:=SU"),
    "chart": (_st_chart, ""),
    "long_text": (_st_long_text, "g:B3"),
    "text_number": (_st_text_number, "g:B3"),
    "link": (_st_link, "g:B3"),
    "totals": (_st_totals, "g:B3"),
    "used_to_end": (_st_used_to_end, "g:B3"),
    "used_to_end_all": (_st_used_to_end, "g:A1|k:c-A|k:c-A"),
    "used_to_end_cols": (_st_used_to_end, "g:B:C"),
}

# ---------------------------------------------------------------------------
# Operations: the keys or the mouse a person uses. A callable builds its script from the live state (pixels).
# ---------------------------------------------------------------------------


def _cell_center(addr):
    r = ui.cell_rect(addr)
    return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)


def _fill_handle_drag(st):
    sel = st.get("selection") or st.get("active_cell")
    last = sel.split(":")[-1]
    if not re.fullmatch(r"[A-Z]{1,3}\d+", last):
        return "k:Escape"  # whole rows / columns: no handle to drag
    r = ui.cell_rect(last)
    x, y = int(r[2]) - 1, int(r[3]) - 1
    h = int(r[3] - r[1])
    return f"m:{x},{y}|w:2|d:{x},{y},{x},{y + 2 * h + h // 2}|w:4"


def _border_drag(st):
    sel = st.get("active_cell")
    if not re.fullmatch(r"[A-Z]{1,3}\d+", sel or ""):
        return "k:Escape"
    r = ui.cell_rect(sel)
    x, y = int(r[0]) + 1, int((r[1] + r[3]) / 2)
    w = int(r[2] - r[0])
    return f"m:{x},{y}|w:2|d:{x},{y},{x + 3 * w},{y}|w:4"


def _col_resize(st):
    r = ui.cell_rect("B1")
    x = int(r[2])
    y = int(r[1]) - 8  # the column header, just above row 1
    return f"m:{x},{y}|w:2|d:{x},{y},{x + 40},{y}|w:4"


def _right_click(st):
    sel = st.get("active_cell") or "B3"
    if not re.fullmatch(r"[A-Z]{1,3}\d+", sel):
        return "k:Escape"
    return f"R:{sel}|w:4|k:Escape"


def _double_click(st):
    return "Q:C3|w:4"


def _drag_select(st):
    x0, y0 = _cell_center("B2")
    x1, y1 = _cell_center("D5")
    return f"m:{x0},{y0}|w:2|d:{x0},{y0},{x1},{y1}|w:4"


def _row_header(st):
    r = ui.cell_rect("A5")
    return f"m:{int(r[0]) - 12},{int((r[1] + r[3]) / 2)}|w:2|c:{int(r[0]) - 12},{int((r[1] + r[3]) / 2)}|w:3"


def _col_header(st):
    r = ui.cell_rect("C1")
    return f"m:{int((r[0] + r[2]) / 2)},{int(r[1]) - 8}|w:2|c:{int((r[0] + r[2]) / 2)},{int(r[1]) - 8}|w:3"


def _autofit(st):
    r = ui.cell_rect("B1")
    x, y = int(r[2]), int(r[1]) - 8
    return f"m:{x},{y}|w:3|D:{x},{y}|w:4"


def _tab_rename(st):
    name = st.get("active_sheet", "")
    tabs = [i for i in ui.items("") if i.get("label") == f"{name}##sheettab" or i.get("id") == f"{name}##sheettab"]
    if not tabs:
        return "k:Escape"
    r = tabs[0]["rect"]
    x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
    return f"m:{x},{y}|w:2|D:{x},{y}|w:4|k:c-A|t:{name}x|k:Enter|w:3"


def _right_drag_fill(st):
    sel = st.get("selection") or st.get("active_cell")
    last = sel.split(":")[-1]
    if not re.fullmatch(r"[A-Z]{1,3}\d+", last):
        return "k:Escape"
    r = ui.cell_rect(last)
    x, y = int(r[2]) - 1, int(r[3]) - 1
    h = int(r[3] - r[1])
    return f"m:{x},{y}|w:2|P:{x},{y}|m:{x},{y + h}|w:2|m:{x},{y + 2 * h + h // 2}|w:2|U:|w:4|k:Escape"


def _ctrl_drag_copy(st):
    sel = st.get("active_cell")
    if not re.fullmatch(r"[A-Z]{1,3}\d+", sel or ""):
        return "k:Escape"
    r = ui.cell_rect(sel)
    x, y = int(r[0]) + 1, int((r[1] + r[3]) / 2)
    w = int(r[2] - r[0])
    return f"m:{x},{y}|w:2|H:ctrl|d:{x},{y},{x + 3 * w},{y}|h:|w:4"


def _format_painter(st):
    x, y = _cell_center("H8")
    return f"x:FormatPainter|w:2|m:{x},{y}|w:2|c:{x},{y}|w:3"


OPS = {
    "type_number": "t:42|k:Enter",
    "type_text": "t:hello|k:Enter",
    "type_formula": "t:=E9*2|k:Enter",  # E9 is the active cell only after a Ctrl+drag copy (ALLOWED: its circular warning)
    "type_ref_formula": "t:=|k:UpArrow|t:+1|k:Enter",
    "type_tab": "t:7|k:Tab",
    "type_escape": "t:99|k:Escape",
    "ctrl_enter": "t:5|k:c-Enter",
    "alt_enter": "t:a|k:a-Enter|t:b|k:Enter",
    "f2_edit": "k:F2|t:9|k:Enter",
    "f2_escape": "k:F2|k:BackSpace|k:Escape",
    "backspace": "k:BackSpace|k:Enter",
    "delete": "k:Delete",
    "enter": "k:Enter",
    "shift_enter": "k:s-Enter",
    "tab": "k:Tab",
    "shift_tab": "k:s-Tab",
    "arrows": "k:DownArrow|k:RightArrow|k:UpArrow|k:LeftArrow",
    "ctrl_arrows": "k:c-DownArrow|k:c-RightArrow",
    "shift_arrows": "k:s-DownArrow|k:s-RightArrow",
    "ctrl_shift_arrows": "k:c-s-DownArrow",
    "home_end": "k:c-End|k:c-Home",
    "page_down": "k:PageDown|k:PageUp",
    "copy_paste": "k:c-C|g:J2|k:c-V",
    "copy_paste_enter": "k:c-C|g:J2|k:Enter",
    "cut_paste": "k:c-X|g:J2|k:c-V",
    "paste_here": "k:c-V",
    "paste_values": "k:c-C|g:J2|k:c-a-V|w:6|k:V|k:Enter",
    "undo": "k:c-Z",
    "undo_redo": "k:c-Z|k:c-Y",
    "fill_down": "k:c-D",
    "fill_right": "k:c-R",
    "bold": "k:c-B",
    "italic_underline": "k:c-I|k:c-U",
    "currency": "k:c-s-4",
    "percent": "k:c-s-5",
    "number_format": "k:c-s-1",
    "clear_formats": "x:ClearFormats",
    "clear_all": "x:ClearAll",
    "insert_cells": "k:c-s-Equal|w:6|k:Enter",
    "delete_cells": "k:c-Minus|w:6|k:Enter",
    "insert_rows": "x:InsertRows",
    "delete_rows": "x:DeleteRows",
    "hide_unhide_row": "k:c-9|k:c-s-9",
    "autosum": "k:a-Equal|k:Enter",
    "today": "k:c-Semicolon|k:Enter",
    "select_all": "k:c-A",
    "format_dialog": "k:c-1|w:8|k:Escape",
    "format_dialog_ok": "k:c-1|w:8|k:Enter",
    "find": "k:c-F|w:8|t:MSFT|k:Enter|w:4|k:Escape",
    "go_to": "k:F5|w:8|t:D5|k:Enter",
    "right_click": _right_click,
    "double_click": _double_click,
    "fill_handle": _fill_handle_drag,
    "drag_cells": _border_drag,
    "col_resize": _col_resize,
    "merge_center": "x:MergeCenter",
    "wrap": "x:WrapText",
    "sort_asc": "x:SortAscending",
    "repeat_f4": "k:c-B|k:DownArrow|k:F4",
    "sheet_switch": "k:c-PageDown|w:3|k:c-PageUp",
    "new_sheet": "k:s-F11|w:3",
    "escape": "k:Escape",
    "recalc": "k:F9",
    "wheel": "m:400,400|W:-5|W:5",
    "drag_select": _drag_select,
    "shift_click": "H:shift|A:D5|h:",
    "ctrl_click": "H:ctrl|A:D5|h:",
    "row_header": _row_header,
    "col_header": _col_header,
    "autofit": _autofit,
    "tab_rename": _tab_rename,
    "filter_toggle": "k:c-s-L|w:3",
    "table_create": "k:c-T|w:8|k:Enter|w:3",
    "note": "k:s-F2|w:4|t:check this|k:Escape",
    "replace_all": "k:c-H|w:8|t:MSFT|k:Tab|t:IBM|k:a-A|w:6|k:Enter|w:3|k:Escape",
    # Shift+F10 and a letter: the cell's, row's or column's menu by what is selected (T, Cut, in all three)
    "menu_key_cut": "k:s-F10|w:6|k:T|w:4|k:Escape",
    "paste_special_dialog": "k:c-C|g:J2|k:c-a-V|w:6|k:Enter",
    "f4_in_edit": "t:=E9|k:F4|k:Enter",  # E9 is the active cell only after a Ctrl+drag copy (ALLOWED: its circular warning)
    "undo3": "k:c-Z|k:c-Z|k:c-Z",
    "cut_escape_paste": "k:c-X|k:Escape|g:J2|k:c-V",
    "copy_formula_above": "k:c-'|k:Enter",
    "strikethrough": "k:c-5",
    "outline_border": "k:c-s-7",
    "type_cjk": "t:\u53f0\u5317|k:Enter",
    "ime_type": "I:\u53f0|J:\u53f0\u5317|k:Enter",
    "right_drag_fill": _right_drag_fill,
    "ctrl_drag_copy": _ctrl_drag_copy,
    "format_painter": _format_painter,
    "spill_formula": "t:=SEQUENCE(3)|k:Enter",
    "delete_row_keys": "k:s-Space|k:c-Minus",
    "insert_row_keys": "k:s-Space|k:c-s-Equal",
    "zoom_wheel": "m:400,400|H:ctrl|W:2|W:-2|h:",
}

# --commands: the states every Home command is tried in, and the commands left out (with the reason)
# used_to_end: a value in XFD1048576 makes the used range the whole sheet, where a command that walks it cell by cell
# freezes (F7 did, chaos seed 6125)
COMMAND_STATES = ["plain", "range", "rows", "typing", "read_only", "protected", "merged", "filtered", "table", "cut",
                  "used_to_end"]
SKIP_COMMANDS = {
    "ScreenshotSaveLast": "saves a picture file in the user's folders",
    "Share": "opens the share menu of screenshots (Windows' clipboard)",
}
# --tab other than Home: what ui_sweep leaves out (leaving the session, printing, files, installs, recording), and
# of a family that differs only by its number or function (Paper9, InsertShape12, InsertFn_SUM) the first three
OTHER_TAB_SKIP = re.compile(r"(Quit|Exit|FileClose|CloseAll|FileOpen|SaveAs|^Save$|FileSave|FileNew|NewWindow|Print|"
                            r"Install|Uninstall|Register|Update|Shutdown|Restart|Reload|Registry|Kill|OperatorMode|"
                            r"Record|Replay|LayoutSave|LayoutLoad|VSCode|ViewCode|ExportPdf)", re.I)


def command_family(cid):
    return "InsertFn_" if cid.startswith("InsertFn_") else re.sub(r"\d+$", "", cid)


COMMAND_NO_UNDO = {"InsertSheet", "DeleteSheet", "RenameSheet", "HideSheet", "UnhideSheet", "CopySheet", "ClipboardPane",
                   "SelectionPane"}  # sheets and panes: Excel's Undo doesn't take them back either

# Operations Excel can't undo either: a content change without an Undo step is right for them
NO_UNDO = {"new_sheet", "tab_rename"}  # Excel can't undo inserting or renaming a sheet
# and in one state only: F9 with manual calculation calculates, which no Undo takes back (Excel neither)
NO_UNDO_IN = {("manual_calc", "recalc"),
              # Protect Sheet on a protected sheet unprotects it (no password): Excel's Undo doesn't take it back
              ("protected", "cmd:ProtectSheet"), ("protected_unlocked", "cmd:ProtectSheet")}
# the probe afterwards (typing into Z50) doesn't fit these: the operation changed what the state was about
PROBE_SKIP = {("protected", "cmd:ProtectSheet"): "the sheet is no longer protected",
              ("protected_unlocked", "cmd:ProtectSheet"): "the sheet is no longer protected"}
# ...in every state: no sheet takes the typing, as in Excel (reset() shows the window again)
PROBE_SKIP_OPS = {"cmd:HideWindow": "the workbook's window is hidden"}

# Messages a person sees in Excel too, by state and operation ("*" for any): (regex on title + text, the reason)
ALLOWED = [
    ("protected*", "*", r"protected sheet|is protected|Unprotect", "Excel: the cell or chart is on a protected sheet"),
    ("read_only*", "*", r"[Rr]ead-only", "YCGrid's trading read-only mode refuses changes (says why)"),
    ("table", "merge_center", r"merge|table", "Excel: cells in a table can't be merged"),
    ("*", "insert_cells", r"shift cells in a filtered range",
     "Excel 16 refuses inserting cells while an AutoFilter filters, and inside its list with buttons only (measured: data16_, 2026-11-21)"),
    ("spill", "*", r"part of an array|spill", "Excel: you can't change part of an array"),
    ("array", "*", r"part of an array", "Excel: you can't change part of an array"),
    ("array", "table_create", r"Multi-cell array formulas aren't allowed in tables",
     "Excel 16 refuses a table over any of a multi-cell array with this message (measured: ListObjects.Add)"),
    ("*", "type_ref_formula", r"circular", "a formula pointing at its own cell is a circular reference, as in Excel"),
    ("*", "f4_in_edit", r"circular", "=$E$9 typed in E9 (the active cell after a Ctrl+drag copy) is its own cell: Excel warns too (seq310230)"),
    ("*", "find", r"couldn't find", "Find searches the selection when several cells are selected: Excel says so too"),
    ("*", "replace_all", r"All done\. We made \d+ replacements|couldn't find anything to replace",
     "Replace All reports what it replaced, or that it found nothing, as Excel's Find and Replace does (Alt+A reached the ribbon behind the dialog before)"),
    ("rules", "*", r"must enter a (whole )?number|outside the allowed range|doesn't match", "a value the cell's data validation refuses: Excel stops it too"),
    ("*", "sort_asc", r"can't be applied to the selected range", "Sort on an empty cell: Excel's own message"),
    ("*", "sort_asc", r"shift cells in a table",
     "Sort over cells that meet a table they aren't inside: Excel 16 refuses Range.Sort and the Sort A to Z button there (measured 2026-11-21: tblrange_ cases; ExecuteMso over row 3 crossing a table fails)"),
    ("used_to_end*", "*", r"can't shift nonblank cells off of the worksheet",
     "a column or cells inserted with a value in XFD1048576 would push it off the sheet: Excel refuses with this message"),
    ("*", "ctrl_enter", r"too large to fill", "Ctrl+Enter over billions of cells (whole rows of the sheet): refused with the reason"),
    ("*", "fill_down", r"too large", "Ctrl+D over billions of cells: refused with the reason"),
    ("*", "fill_right", r"too large", "Ctrl+R over billions of cells: refused with the reason"),
    ("*", "format_painter", r"too large to (paint|copy the formats)",
     "Format Painter over billions of cells with a copy larger than one cell: refused with the reason, as a fill that large is"),
    ("*", "delete_cells", r"shift cells in a table", "shifting cells inside a table: Excel's own refusal"),
    ("*", "insert_cells", r"shift cells in a table", "shifting cells inside a table: Excel's own refusal"),
    ("*", "delete_rows", r"shift cells in a table",
     "whole rows with a table's header row but not all its rows: Excel refuses (measured: Rows(\"3:4\").Delete)"),
    ("*", "delete_row_keys", r"shift cells in a table",
     "whole rows with a table's header row but not all its rows: Excel refuses (measured: Rows(\"3:4\").Delete)"),
    ("*", "autosum", r"wrong number of arguments", "AutoSum with nothing next to the cell gives =SUM(); Enter is refused, as Excel's"),
    ("*", "f2_edit", r"sheet name must be followed by a cell reference",
     "F2 then 9 after =#REF!*#REF! (columns deleted under it) is =#REF!*#REF!9: Excel 16 refuses it too (measured)"),
    ("*", "f2_edit", r"extra content after the formula", "F2 then 9 after =SUM(B2:B6) is =SUM(B2:B6)9: Excel refuses it too"),
    ("*", "copy_paste", r"can't be used on multiple selections", "a Ctrl+click left areas Excel can't copy together"),
    ("*", "copy_paste_enter", r"can't be used on multiple selections", "a Ctrl+click left areas Excel can't copy together"),
    ("*", "cut_paste", r"can't be used on multiple selections", "a Ctrl+click left areas Excel can't cut together"),
    ("*", "cut_escape_paste", r"can't be used on multiple selections", "a Ctrl+click left areas Excel can't cut together"),
    ("*", "paste_values", r"can't be used on multiple selections", "a Ctrl+click left areas Excel can't copy together"),
    ("*", "paste_special_dialog", r"can't be used on multiple selections", "a Ctrl+click left areas Excel can't copy together"),
    ("*", "cmd:Select*", r"No cells were found", "Go To Special's kinds with nothing of that kind: Excel's own message"),
    ("*", "cmd:DeleteSheet", r"at least one visible sheet", "deleting the only sheet: Excel refuses with this message"),
    ("*", "cmd:FlashFill", r"Flash Fill", "Flash Fill without an example to follow: Excel asks for one"),
    ("*", "cmd:FillJustify", r"[Cc]annot justify", "Fill > Justify on numbers or formulas: Excel refuses"),
    ("rows", "cmd:InsertColumns", r"prevent possible loss of data", "inserting 16,384 columns pushes data off the sheet: Excel refuses"),
    ("*", "insert_cells", r"prevent possible loss of data",
     "Ctrl+A over whole rows selects the sheet: inserting all its rows pushes data off it, Excel refuses"),
    # the other tabs' commands (--tab others): the answer Excel gives where the command has nothing to work on
    ("*", "cmd:AutoOutline", r"Cannot create an outline", "Auto Outline without formulas to outline: Excel's own message"),
    ("*", "cmd:DataTableDialog", r"range of the data table", "Data Table without a range selected: Excel asks for one"),
    ("*", "cmd:ErrorChecking", r"error check is complete", "Error Checking on a sheet without errors: Excel says so"),
    ("*", "cmd:EvaluateFormula", r"doesn't contain a formula", "Evaluate Formula on a constant: Excel says there's no formula"),
    ("*", "cmd:TraceError", r"doesn't contain an error", "Trace Error on a cell that isn't an error: Excel says so"),
    ("*", "cmd:Spelling", r"spelling check is complete", "Spelling: the check ends with Excel's message"),
    ("*", "cmd:ForecastSheet", r"timeline", "Forecast Sheet needs a timeline and values: Excel refuses without them"),
    ("*", "cmd:InsertSlicer", r"PivotTable or a table", "a slicer needs a table or a PivotTable: Excel says so"),
    ("*", "cmd:InsertTimeline", r"PivotTable", "a timeline needs a PivotTable: Excel says so"),
    ("table", "cmd:Subtotal", r"can't be added to a table", "Subtotal inside a table: Excel refuses (convert it to a range)"),
    ("*", "cmd:TextToColumns", r"one column at a time", "Text to Columns over several columns: Excel converts one column at a time"),
    ("*", "cmd:ImportText", r"file dialog was cancelled", "a hidden YCGrid cancels file dialogs (nobody can pick a file)"),
    ("*", "cmd:InsertPicture", r"file dialog was cancelled", "a hidden YCGrid cancels file dialogs (nobody can pick a file)"),
    ("*", "cmd:SheetBackground", r"file dialog was cancelled", "a hidden YCGrid cancels file dialogs (nobody can pick a file)"),
    ("list", "*", r"isn't in the list", "a value the cell's list validation doesn't hold: Excel stops it too"),
    ("protected*", "table_create", r"table can't be created", "a table on a protected sheet: Excel refuses too"),
    ("*", "paste*", r"can't do that to a merged cell",
     "a paste onto part of a merged cell (a Merge & Center earlier in a sequence, then the copy pasted over a corner of "
     "it, seq329702): Excel 16 refuses with this message (measured: Range.Copy onto part of a merged area)"),
    ("*", "copy_paste_enter", r"can't do that to a merged cell",
     "Enter pastes too: a merged J3:J6 copied onto J2 (part of itself) is refused with this message, as Excel 16's "
     "Range.Copy J2 (measured 2026-11-21, seq437536)"),
    ("*", "table_create", r"merged cells","a table over merged cells (the merged state, or a Merge & Center earlier in a sequence): Excel refuses (A table can't overlap merged cells)"),
    ("*", "table_create", r"can't overlap another table",
     "Ctrl+T a second time over a region holding the table made earlier in the sequence: Excel refuses a table over "
     "another table too (its wording not measured)"),
    ("*", "*", r"merged cells need to be the same size",
     "a sort over a list holding a merged cell among single cells (the merged state's B9:C10, or a Merge & Center "
     "earlier in a sequence): Excel refuses with this message (MergeSize)"),
    ("*", "*", r"too large to copy",
     "kept on purpose (docs/excel-parity-backlog.md): with a value at XFD1048576 (the far state, or Ctrl+arrows there and "
     "an insert) a selection from it to B3 holds 17 billion cells of the used range, past the 20,000,000 a copy holds; "
     "Excel copies it"),
    ("*", "*", r"We can't paste here because the Copy area and paste area aren't the same size",
     "whole rows or columns copied (3:3, B:B) and pasted at a cell past column A or row 1: the copy area runs past the "
     "sheet's edge from there, and Excel 16 refuses with this message (measured: B5:XFD6 to J2, a column to row 2)"),
    ("*", "type_formula", r"circular references",
     "a Ctrl+drag copy to E9 makes E9 the active cell, and =E9*2 typed there refers to itself: Excel warns too"),
    ("*", "copy_formula_above", r"circular references",
     "=E9*2 typed in E8, then Ctrl+' in E9 copies it as it is: E9 refers to itself, and Excel warns too (seq419233)"),
    ("*", "autosum", r"circular references",
     "C3 dragged to F3 (D3 now =B3*F3), then AutoSum in F3 proposes =SUM(D3:E3) in place, as Excel 16 does over a filled "
     "cell (measured): a real cycle, and Excel shows this warning too"),
    ("*", "table_create", r"shift cells in a filtered range",
     "a table without headers beside a filtered list: its header row is inserted across the rows the filter hides, "
     "which Excel 16 refuses (measured: ListObjects.Add over G2:H3 beside a filtered A1:E7 raised)"),
    ("*", "table_create", r"change a filtered range",
     "a table over part of an AutoFilter's list (B2:E7 after AutoSum, B2:D5 dragged after Ctrl+Shift+L in any state): "
     "Excel 16 refuses with this message (measured, ListObjects.Add)"),
    ("autocomplete", "alt_enter", r"problem with this formula", "=SUa, a line break, b: Excel refuses it too"),
    ("outline", "autosum", r"problem with this formula", "AutoSum in B2 under the header: nothing to add, =SUM() is refused"),
    ("areas", "*", r"can't be used on multiple selections",
     "B2 and D4:D5 share neither rows nor columns: Excel refuses to copy or cut them with this message"),
    ("far", "autosum", r"problem with this formula", "AutoSum at XFD1048576, nothing to add: =SUM() is refused, as Excel's"),
    ("pointing", "alt_enter", r"problem with this formula", "=SUM(F1:F2a, a line break, b: Excel refuses it too"),
    ("pointing", "recalc", r"problem with this formula", "F9 on the unfinished =SUM(F1:F2: Excel can't calculate it either"),
    ("pointing", "*", r"circular", "=SUM(F1:F2 then typing 42 is =SUM(F1:F242), which holds its own cell: Excel warns too"),
]


# ---------------------------------------------------------------------------


def _english_back():
    """zh-TW text -> English, longest translations first (UI messages are assembled from translated pieces)."""
    with open(os.path.join(ROOT, "i18n", "zh-TW.json"), encoding="utf-8") as f:
        table = json.load(f)
    # the table keeps texts as the C sources write them ("\\n" for a line break); pieces are matched without the line
    # breaks around them (the Error Log keeps a message's breaks as CRLF)
    def unescape(s):
        return s.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\")

    pairs = [(unescape(zh).strip(), unescape(en).strip()) for en, zh in table.items() if isinstance(zh, str) and len(zh.strip()) > 1]
    pairs.sort(key=lambda x: -len(x[0]))
    return pairs


class Basics:
    def __init__(self, a):
        self.a = a
        self.lang = getattr(a, "lang", None) or "en"
        self.back = _english_back() if self.lang != "en" else []
        self.fail = []
        self.n = 0
        os.makedirs(OUT, exist_ok=True)
        exe = a.exe or os.path.join(ROOT, "dist_release", "ycgrid.exe")
        self.proc = yc.launch(hidden=True, exe=exe, lang=self.lang)
        self.conn = yc.connect(target=self.proc.pid)
        excel.Application.DisplayAlerts = False
        self.seq = 0
        self.last_err = self._last_error_id()

    def restart(self):
        self.close()
        exe = self.a.exe or os.path.join(ROOT, "dist_release", "ycgrid.exe")
        self.proc = yc.launch(hidden=True, exe=exe, lang=self.lang)
        self.conn = yc.connect(target=self.proc.pid)
        excel.Application.DisplayAlerts = False
        self.last_err = self._last_error_id()

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(10)
        except Exception:  # noqa: BLE001
            pass

    def _last_error_id(self):
        e = ui.errors()
        return e[-1]["id"] if e else 0

    def keys(self, script, timeout_ms=15000):
        t = time.time()
        r = self.conn.request("ui.input", {"script": script + "|w:2", "timeout_ms": timeout_ms}, timeout=timeout_ms / 1000 + 20)
        took = time.time() - t
        return bool(r.get("done", True)) if isinstance(r, dict) else True, took

    def dump(self):
        return self.conn.request("debug.dump", {})

    def state(self):
        return ui.state()

    def reset(self):
        """A fresh sheet with the table, nothing left of the case before."""
        self.keys("k:Escape|k:Escape|m:2,2|x:ViewNormal|x:Zoom100")
        if not excel.ActiveWindow.Visible:  # View > Hide (--tab others) hid the workbook's window
            excel.ActiveWindow.Visible = True
        safety.set_operator_mode(True)
        excel.Application.Calculation = excel.xlCalculationAutomatic
        old = [excel.Worksheets(i) for i in range(1, excel.Worksheets.Count + 1)]
        self.seq += 1
        ws = excel.Worksheets.Add(After=old[-1])
        ws.Name = f"B{self.seq}"
        ws.Activate()
        for o in old:
            try:
                o.Unprotect()
            except Exception:  # noqa: BLE001
                pass
            o.Delete()
        for n in list(excel.ActiveWorkbook.Names):
            try:
                n.Delete()
            except Exception:  # noqa: BLE001
                pass
        ws.Range("A1:E7").Value = TABLE
        ws.Range("D2:D6").Formula = "=B2*C2"
        ws.Range("B7").Formula = "=SUM(B2:B6)"
        ws.Range("C2:C6").NumberFormat = "0.00"
        ws.Range("E2:E6").NumberFormat = "yyyy-mm-dd"
        ws.Range("A1:E1").Font.Bold = True
        return ws

    def allowed(self, state, op, text):
        if op.startswith("cmd:"):  # an ALLOWED entry may name a group of commands: cmd:Select*
            for s, o, rx, _why in ALLOWED:
                if o.endswith("*") and o.startswith("cmd:") and op.startswith(o[:-1]) and \
                        (s == "*" or s == state or (s.endswith("*") and state.startswith(s[:-1]))) and re.search(rx, text):
                    return True
        text = text.replace("\r\n", "\n")
        for zh, en in self.back:  # the Chinese UI's message, in the English ALLOWED matches
            if zh in text:
                text = text.replace(zh, en)
        for s, o, rx, _why in ALLOWED:
            if (s == "*" or s == state or (s.endswith("*") and state.startswith(s[:-1]))) and \
                    (o == "*" or o == op or (o.endswith("*") and op.startswith(o[:-1]))) and re.search(rx, text):
                return True
        return False

    def problem(self, case, what, detail=""):
        self.fail.append((case, what))
        shot = os.path.join(OUT, re.sub(r"[^A-Za-z0-9_.-]", "_", case) + ".png")
        try:
            ui.screenshot(shot)
        except Exception:  # noqa: BLE001
            shot = ""
        print(f"FAIL {case}: {what}", flush=True)
        if detail:
            print("   " + detail.replace("\n", "\n   ")[:3000], flush=True)
        if shot:
            print(f"   screenshot {shot}", flush=True)

    @staticmethod
    def diff(a, b):
        return "\n".join(list(difflib.unified_diff(a.splitlines(), b.splitlines(), "before", "after", lineterm="", n=0))[2:40])

    def probe(self, case, state):
        """After the operation and its checks: typing into a cell still works the plain way (the read-only bug left every
        later cell operation answering "There's a problem with this formula" or "There's already data here")."""
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2")
        ws = excel.ActiveSheet
        before = ws.Range("Z50").Value2
        self.keys("g:Z50|t:probe|k:Enter|w:2")
        st = self.state()
        # Z50 inside a merged cell (the whole sheet merged): the entry goes to the merged cell's first cell, as Excel's
        got = ws.Range("Z50").MergeArea.Cells(1, 1).Value2
        refused = state.startswith("read_only") or state.startswith("protected")
        if "edit" in st:
            self.problem(case, f"afterwards, typing into Z50 left the editor open ({st['edit'].get('text')!r})")
            self.keys("k:Escape|k:Escape")
        elif not refused and got != "probe":
            self.problem(case, f"afterwards, typing probe into Z50 gave {got!r}")
        elif refused and got != before:
            self.problem(case, f"afterwards, {state} let Z50 change to {got!r}")
        for e in ui.errors(after=self.last_err):
            text = f"{e.get('title', '')}: {e.get('text', '')}"
            if not self.allowed(state, "probe", text):
                self.problem(case, "afterwards, typing into Z50 showed: " + text[:300])
        self.last_err = self._last_error_id()

    def run_case(self, state, op):
        self.checks(state, op)
        if (state, op) not in PROBE_SKIP and op not in PROBE_SKIP_OPS:
            self.probe(f"{state}:{op}", state)

    def checks(self, state, op):
        case = f"{state}:{op}"
        self.n += 1
        setup, keys = STATES[state]
        ws = self.reset()
        if setup:
            setup(ws)
        if callable(keys):
            keys = keys()
        ok, _ = self.keys(keys or "w:2")
        if not ok:
            self.problem(case, "the state's keys didn't finish", keys)
            return
        self.last_err = self._last_error_id()
        st0 = self.state()
        asserts0 = st0.get("imgui_asserts", 0)
        depth0 = st0["undo"]["depth"]
        before = self.dump()
        read_only = state.startswith("read_only")
        # the operation
        script = OPS[op](st0) if callable(OPS[op]) else OPS[op]
        ok, took = self.keys(script)
        if not ok:
            self.problem(case, "the operation's keys didn't finish (a window that never closed?)", script)
        if took > 8:
            self.problem(case, f"the operation took {took:.1f} s", script)
        # 3. two Escapes leave nothing behind
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2")
        st = self.state()
        left = []
        if "edit" in st:
            left.append(f"a cell editor ({st['edit'].get('cell')}: {st['edit'].get('text')!r})")
        if st.get("dialog"):
            left.append(f"dialog {st['dialog']}")
        if st.get("ribbon", {}).get("popup_open"):
            left.append("a menu")
        if left:
            self.problem(case, "two Escapes left " + ", ".join(left))
            self.keys("k:Escape|k:Escape|k:Escape")
        # 1. messages
        for e in ui.errors(after=self.last_err):
            text = f"{e.get('title', '')}: {e.get('text', '')}"
            if not self.allowed(state, op, text):
                self.problem(case, "unexpected message: " + text[:300])
        self.last_err = self._last_error_id()
        # 2. ImGui checks
        if st.get("imgui_asserts", 0) != asserts0:
            self.problem(case, f"ImGui checks failed ({st.get('imgui_asserts', 0) - asserts0})")
        if st.get("shrinking_windows") or st.get("clipped_windows"):
            self.problem(case, f"windows shrinking / clipped: {st.get('shrinking_windows')} {st.get('clipped_windows')}")
        after = self.dump()
        steps = st["undo"]["depth"] - depth0
        # 5. read-only mode changes nothing
        if read_only:
            if after != before:
                self.problem(case, "read-only mode let the content change", self.diff(before, after))
            return
        # 4. Undo / Redo
        if steps < 0:  # the operation undid steps (Ctrl+Z): Redo must give back exactly what was there
            self.keys("|".join(["k:c-Y|w:3"] * -steps))
            redone = self.dump()
            if redone != before:
                self.problem(case, f"Redo after Undo ({-steps} step{'s' if steps < -1 else ''}) didn't give back what was there", self.diff(before, redone))
            return
        if steps == 0:
            if after != before and op not in NO_UNDO and (state, op) not in NO_UNDO_IN and op[4:] not in COMMAND_NO_UNDO:
                self.problem(case, "the content changed without an Undo step", self.diff(before, after))
            return
        self.keys("|".join(["k:c-Z|w:3"] * steps))
        undone = self.dump()
        if undone != before:
            self.problem(case, f"Undo ({steps} step{'s' if steps > 1 else ''}) didn't give back what was there", self.diff(before, undone))
            return
        self.keys("|".join(["k:c-Y|w:3"] * steps))
        redone = self.dump()
        if redone != after:
            self.problem(case, "Redo didn't give back what the operation made", self.diff(after, redone))
        for e in ui.errors(after=self.last_err):
            text = f"{e.get('title', '')}: {e.get('text', '')}"
            if not self.allowed(state, op, text):
                self.problem(case, "unexpected message during Undo / Redo: " + text[:300])
        self.last_err = self._last_error_id()

    def run_sequence(self, seed, given=None):
        """A state, then eight operations, then Undo to the start and Redo to the end (--sequences); given: (state,
        [operations]) to replay one (--sequence state:op,op,...: the case name a failure prints)."""
        if given:
            state, chosen = given
        else:
            rnd = random.Random(seed)
            states = [s for s in STATES if not s.startswith("read_only")]
            ops = [o for o in OPS if o not in NO_UNDO and not o.startswith("undo") and o not in ("recalc",)]
            state = rnd.choice(states)
            chosen = [rnd.choice(ops) for _ in range(8)]
        case = f"seq{seed}:{state}:" + ",".join(chosen)
        self.n += 1
        setup, keys = STATES[state]
        ws = self.reset()
        if setup:
            setup(ws)
        if callable(keys):
            keys = keys()
        self.keys(keys or "w:2")
        self.last_err = self._last_error_id()
        st0 = self.state()
        asserts0 = st0.get("imgui_asserts", 0)
        depth0 = st0["undo"]["depth"]
        start = self.dump()
        for op in chosen:
            st = self.state()
            script = OPS[op](st) if callable(OPS[op]) else OPS[op]
            ok, took = self.keys(script)
            if os.environ.get("YC_BASICS_TIMES"):  # each step's time, to find the slow one of a sequence
                print(f"    {op}: {took:.2f} s", flush=True)
            if not ok or took > 8:
                self.problem(case, f"{op}: the keys didn't finish in time ({took:.1f} s)", script)
                return
            self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2")
            for e in ui.errors(after=self.last_err):
                text = f"{e.get('title', '')}: {e.get('text', '')}"
                if not self.allowed(state, op, text) and not self.allowed("*", op, text):
                    self.problem(case, f"{op}: unexpected message: " + text[:300])
            self.last_err = self._last_error_id()
        st = self.state()
        if st.get("imgui_asserts", 0) != asserts0:
            self.problem(case, "ImGui checks failed")
        end = self.dump()
        steps = st["undo"]["depth"] - depth0
        if steps <= 0:
            if end != start:
                self.problem(case, "the content changed without Undo steps", self.diff(start, end))
            return
        # a circular reference without iteration isn't calculated: its cells and what reads them keep the values they
        # show, which depend on the order things happened (the lesson in AGENTS.md), so values aren't compared while a
        # sheet has one, as doc_fuzz does; formulas and formats still are (seq306507: =C1+1 typed in C1, then read)
        cyclic = self.cyclic()
        self.keys("|".join(["k:c-Z|w:3"] * steps))
        undone = self.dump()
        cyclic = cyclic or self.cyclic()
        if self.same(undone, start, cyclic) is False:
            self.problem(case, f"Undo of all {steps} steps didn't give back the start", self.diff(start, undone))
            return
        self.keys("|".join(["k:c-Y|w:3"] * steps))
        redone = self.dump()
        cyclic = cyclic or self.cyclic()
        if self.same(redone, end, cyclic) is False:
            self.problem(case, f"Redo of all {steps} steps didn't give back the end", self.diff(end, redone))
        self.probe(case, state)

    def cyclic(self):
        try:
            return any(ws.CircularReference is not None for ws in excel.Worksheets)
        except Exception:  # noqa: BLE001 - a chart sheet or a sheet just removed
            return False

    @staticmethod
    def same(a, b, cyclic):
        if a == b:
            return True
        if not cyclic:
            return False

        def formulas(dump):
            out = []
            for line in str(dump).splitlines():
                if line.startswith("chunk CELL"):
                    continue  # its hash takes in the values
                parts = line.split("\t")
                out.append("\t".join(parts[:2]) if len(parts) >= 3 and parts[1].startswith("=") else line)
            return out

        return formulas(a) == formulas(b)

    def cases(self):
        states = self.a.states.split(",") if self.a.states else list(STATES)
        ops = self.a.ops.split(",") if self.a.ops else list(OPS)
        out = [(s, o) for s in states for o in ops]
        if self.a.only:
            want = set(self.a.only.split(","))
            out = [c for c in out if f"{c[0]}:{c[1]}" in want]
        if self.a.shard:
            k, n = (int(x) for x in self.a.shard.split("/"))
            out = [c for i, c in enumerate(out) if i % n == k]  # K counts from 0, as feature_check's
        return out

    def check_ids(self):
        known = {c["id"] for c in ui.commands()}
        bad = set()
        for _, keys in STATES.values():
            if callable(keys):
                continue
            bad |= {x[2:] for x in keys.split("|") if x.startswith("x:") and x[2:] not in known}
        for v in OPS.values():
            if isinstance(v, str):
                bad |= {x[2:] for x in v.split("|") if x.startswith("x:") and x[2:] not in known}
        if bad:
            self.problem("setup", "unknown command ids in basics.py: " + ", ".join(sorted(bad)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe")
    ap.add_argument("--shard")
    ap.add_argument("--only")
    ap.add_argument("--states")
    ap.add_argument("--ops")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--sequences", type=int, default=0, help="random sequences of operations instead of the matrix")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--lang", default="en", help="the UI language: en or zh-TW")
    ap.add_argument("--commands", action="store_true", help="every command of a ribbon tab in COMMAND_STATES")
    ap.add_argument("--tab", default="Home", help="--commands: the tab whose commands run (its English name), or others (every tab but Home)")
    ap.add_argument("--sequence", help="replay one sequence: state:op,op,... (as a failure names it, without seqN:)")
    a = ap.parse_args()
    if a.list:
        print("states:", " ".join(STATES))
        print("operations:", " ".join(OPS))
        return 0
    b = Basics(a)
    t0 = time.time()
    try:
        b.check_ids()
        if a.commands:
            home = sorted(c["id"] for c in ui.commands() if c["id"] not in SKIP_COMMANDS and
                          (c.get("tab") == a.tab or (a.tab == "others" and c.get("tab") not in (None, "", "Home"))))
            if a.tab != "Home":
                seen = {}
                home = [c for c in home if not OTHER_TAB_SKIP.search(c) and
                        seen.setdefault(command_family(c), []).append(c) is None and len(seen[command_family(c)]) <= 3]
            if a.tab == "Home" and len(home) < 100:
                b.problem("setup", f"only {len(home)} Home commands listed (ui.commands()['tab'])")
            for cid in home:
                OPS["cmd:" + cid] = f"x:{cid}|w:6|k:Escape|w:2|k:Escape|w:2"
            states = a.states.split(",") if a.states else COMMAND_STATES
            cases = [(s, "cmd:" + cid) for s in states for cid in home]
            if a.shard:
                k, n = (int(x) for x in a.shard.split("/"))
                cases = [c for i, c in enumerate(cases) if i % n == k]
            print(f"{len(home)} commands of {a.tab}", flush=True)
        elif a.sequence:
            st_name, _, ops_text = a.sequence.partition(":")
            cases = [("given", (st_name, ops_text.split(",")))]
        elif a.sequences:
            cases = [("seq", a.seed + k) for k in range(a.sequences)]
        else:
            cases = b.cases()
        print(f"{len(cases)} cases", flush=True)
        for i, (s, o) in enumerate(cases):
            try:
                if s == "given":
                    b.run_sequence(0, o)
                elif s == "seq":
                    b.run_sequence(o)
                else:
                    b.run_case(s, o)
            except Exception as e:  # noqa: BLE001
                froze = getattr(e, "code", "") == "E_TIMEOUT" or "E_TIMEOUT" in str(e)
                gone = b.proc.poll() is not None
                if froze:
                    b.fail.append((f"{s}:{o}", "YCGrid froze (no answer for 35 s)"))
                    print(f"FAIL {s}:{o}: YCGrid froze (no answer for 35 s)", flush=True)
                elif gone:
                    b.fail.append((f"{s}:{o}", f"YCGrid exited (code {b.proc.poll()})"))
                    print(f"FAIL {s}:{o}: YCGrid exited (code {b.proc.poll()})", flush=True)
                else:
                    b.problem(f"{s}:{o}", f"raised {type(e).__name__}: {e}", traceback.format_exc()[-1500:])
                if froze or gone or isinstance(e, ConnectionError):
                    # our own hidden instance: stop it (a frozen one never answers again) and start a new one
                    print("starting a new YCGrid", flush=True)
                    try:
                        b.proc.kill()
                    except Exception:  # noqa: BLE001
                        pass
                    b.restart()
            if (i + 1) % 50 == 0:
                print(f"  {i + 1}/{len(cases)} cases, {len(b.fail)} problems, {time.time() - t0:.0f} s", flush=True)
    finally:
        b.close()
    print(f"{b.n} cases in {time.time() - t0:.0f} s: {len(b.fail)} problems", flush=True)
    kinds = {}
    for case, what in b.fail:
        kinds.setdefault(re.sub(r"\(.*", "", what)[:70], []).append(case)
    for k, v in sorted(kinds.items(), key=lambda x: -len(x[1])):
        print(f"  {len(v):4d}  {k}: {', '.join(v[:8])}{' ...' if len(v) > 8 else ''}")
    return 1 if b.fail else 0


if __name__ == "__main__":
    sys.exit(main())
