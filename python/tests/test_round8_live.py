# SPDX-License-Identifier: Apache-2.0
"""Round 8: read-only mode while a cell is being edited, writes into merged cells and visible cells of one cell,
each test failing without its fix.

Run: python -m unittest tests.test_round8_live (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ReadOnlyModeTest(unittest.TestCase):
    """Trading > Operator Mode off (read-only) while the user is typing in a cell, then the basic operations."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel, safety

        safety.set_operator_mode(True)
        self.keys("k:Escape|m:2,2|w:2")
        excel.Application.CutCopyMode = False
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()
        self.ws.Range("B2").Value2 = 5
        self.ws.Range("C3").Value2 = 7
        self.errors0 = len(yc.ui.errors())

    def tearDown(self):
        from yc import safety

        safety.set_operator_mode(True)
        self.keys("k:Escape|m:2,2|w:2")

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def new_errors(self):
        return yc.ui.errors()[self.errors0:]

    def test_an_entry_in_progress_is_dropped_when_read_only_mode_turns_on(self):
        from yc import safety

        self.keys("g:A1|w:2|t:=SUM(|w:3")
        self.assertTrue(yc.ui.state().get("edit"), "A1 should be in edit mode")
        safety.set_operator_mode(False)
        self.keys("w:4")
        self.assertFalse(yc.ui.state().get("edit"), "the entry stayed open in read-only mode")
        # the basic operations: click a cell, type, Enter, Delete, F2, double-click, paste
        self.keys("A:C3|w:4|t:abc|w:2|k:Enter|w:4")
        self.keys("A:B2|w:4|k:Delete|w:4|k:F2|w:3|k:Escape|w:2|Q:B2|w:4|k:Escape|w:2")
        self.keys("k:c-C|w:3|A:D4|w:3|k:c-V|w:4|k:Escape|w:2")
        self.assertEqual(self.new_errors(), [], "read-only mode raised error messages")
        self.assertEqual(yc.ui.state()["dialog"], 0)
        self.assertFalse(yc.ui.state().get("edit"))
        self.assertEqual(self.ws.Range("A1").Formula, "")
        self.assertEqual(self.ws.Range("B2").Value2, 5)
        self.assertEqual(self.ws.Range("C3").Value2, 7)
        self.assertIsNone(self.ws.Range("D4").Value2)
        self.assertIn("Read-only", yc.ui.state()["status"])
        self.assertEqual(yc.ui.state()["active_cell"], "D4")  # the clicks still select
        # back in operator mode typing works at once
        safety.set_operator_mode(True)
        self.keys("w:3|A:E5|w:3|t:42|k:Enter|w:4")
        self.assertEqual(self.ws.Range("E5").Value2, 42)

    def test_read_only_mode_offers_no_drag_and_drop(self):
        from yc import safety

        safety.set_operator_mode(False)
        self.keys("w:3|A:B2|w:3")
        x0, y0, x1, y1 = yc.ui.cell_rect("B2")
        tx0, ty0, tx1, ty1 = yc.ui.cell_rect("C3")
        # the selection's right border dragged onto C3 (it holds 7): no "There's already data here" question
        bx, by = int(x1) - 1, int((y0 + y1) / 2)
        self.keys(f"m:{bx},{by}|w:4|d:{bx},{by},{int((tx0 + tx1) / 2)},{int((ty0 + ty1) / 2)}|w:8")
        self.assertEqual(yc.ui.state()["dialog"], 0, "a question opened for a move read-only mode refuses")
        self.assertEqual(self.new_errors(), [])
        self.assertEqual(self.ws.Range("B2").Value2, 5)
        self.assertEqual(self.ws.Range("C3").Value2, 7)
        # the fill handle drags nothing either
        self.keys(f"A:B2|w:3|m:{int(x1) - 1},{int(y1) - 1}|w:4|d:{int(x1) - 1},{int(y1) - 1},{int(x1) - 1},{int(ty1) + 20}|w:8|m:2,2")
        self.assertIsNone(self.ws.Range("B3").Value2)
        self.assertEqual(self.new_errors(), [])

    def test_escape_puts_the_paste_options_button_away(self):
        # the button sits over the cell below and to the right of the paste: left there after Escape, it took the next
        # click on that cell (E5 stayed unselected and typing went nowhere)
        self.keys("A:B2|w:3|k:c-C|w:3|A:D4|w:3|k:c-V|w:4|k:Escape|w:3")
        self.keys("A:E5|w:4|t:42|k:Enter|w:4")
        self.assertEqual(self.ws.Range("E5").Value2, 42)
        self.assertEqual(self.ws.Range("D4").Value2, 5)
        self.assertEqual(self.new_errors(), [])

    def test_no_stray_debug_window(self):
        # ImGui's implicit Debug window shows over the sheet only when something draws into it
        names = [w["name"] for w in yc.ui.state()["windows"]]
        self.assertNotIn("Debug##Default", names)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class UiOpsRound8Test(unittest.TestCase):
    """Everyday keys and clicks a UI tester found misbehaving (round 8, uiops): each as a person does it."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.keys("k:Escape|w:2|k:Escape|m:2,2|w:2")
        excel.Application.CutCopyMode = False
        # the earlier tests' sheets go: with many sheets the tabs clicked here scroll out of the tab bar
        excel.Application.DisplayAlerts = False
        while excel.Worksheets.Count > 1:
            excel.Worksheets(excel.Worksheets.Count).Delete()
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.other = excel.Worksheets.Add(After=self.ws)
        self.ws.Activate()
        self.keys("w:3")
        self.errors0 = len(yc.ui.errors())

    def tearDown(self):
        self.keys("k:Escape|w:2|k:Escape|m:2,2|w:2")

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def tab(self, ws):
        hits = [i for i in yc.ui.items("") if i["id"] == ws.Name + "##sheettab"]
        self.assertTrue(hits, "the sheet's tab is shown")
        x0, y0, x1, y1 = hits[0]["rect"]
        return int((x0 + x1) / 2), int((y0 + y1) / 2)

    def test_row_and_column_differences_and_current_array_by_keys(self):
        from yc import excel

        self.ws.Range("B2:E6").Value = [[1, 1, 2, 1], [2, 3, 2, 2], [5, 5, 5, 6], [7, 8, 7, 7], [9, 9, 9, 9]]
        self.ws.Range("J2:K3").FormulaArray = "=B2:C3*2"
        self.ws.Range("M2").Formula = "=SEQUENCE(3,2)"

        def found(sel, act, key):
            self.ws.Range(sel).Select()
            self.ws.Range(act).Activate()
            self.keys(f"k:{key}|w:5")
            return excel.Selection.Address, excel.ActiveCell.Address

        def no_cells(sel, act, key):
            n = len(yc.ui.errors())
            self.assertEqual(found(sel, act, key), (self.ws.Range(sel).Address, self.ws.Range(act).Address), (sel, key))
            self.assertTrue(any(str(e.get("text")).startswith("No cells were found.") for e in yc.ui.errors()[n:]), (sel, key))

        # the active cell is the first cell found, top to bottom (Excel 16, measured by keys posted to its sheet window)
        self.assertEqual(found("B2:E6", "B2", "c-Backslash")[1], "$D$2")
        self.assertEqual(found("B2:E6", "E6", "c-Backslash")[1], "$D$2")
        self.assertEqual(found("B2:E6", "B2", "c-s-Backslash")[1], "$B$3")
        self.assertEqual(found("B2:E6", "C4", "c-s-Backslash")[1], "$B$2")
        # one cell selected finds none, though its region differs
        no_cells("C3", "C3", "c-Backslash")
        no_cells("C3", "C3", "c-s-Backslash")
        # Ctrl+/: an array entered with Ctrl+Shift+Enter, its first cell active; a dynamic array's spill finds none
        self.assertEqual(found("K3", "K3", "c-Slash"), ("$J$2:$K$3", "$J$2"))
        no_cells("N3", "N3", "c-Slash")
        no_cells("B2", "B2", "c-Slash")

    def test_end_mode_with_enter_and_escape(self):
        from yc import excel

        for r in range(2, 7):
            self.ws.Range(f"B{r}:E{r}").Value = [[r * 10 + c for c in range(2, 6)]]
        self.ws.Range("H9").Value = 1  # the used area ends at H9

        def keys_from(start, script):
            self.keys(f"g:{start}|w:2|{script}|w:3")
            return excel.Selection.Address, excel.ActiveCell.Address

        # End, Enter: the used area's last column in the row, an empty row too; Shift grows the selection to it
        # (Excel 16, measured by keys posted to its sheet window)
        self.assertEqual(keys_from("B3", "k:End|w:2|k:Enter"), ("$H$3", "$H$3"))
        self.assertEqual(keys_from("B20", "k:End|w:2|k:Enter"), ("$H$20", "$H$20"))
        self.assertEqual(keys_from("B3", "k:End|w:2|k:s-Enter"), ("$B$3:$H$3", "$B$3"))
        # Escape ends Extend Selection (F8) but not End mode
        self.assertEqual(keys_from("B2", "k:End|w:2|k:Escape|w:2|k:DownArrow"), ("$B$6", "$B$6"))
        self.assertEqual(keys_from("B2", "k:F8|w:2|k:Escape|w:2|k:RightArrow"), ("$C$2", "$C$2"))
        self.assertEqual(keys_from("B2", "k:End|w:2|k:End|w:2|k:DownArrow"), ("$B$3", "$B$3"))

    def test_selection_keys_take_a_tables_part_first(self):
        from yc import excel

        self.ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.5]]
        self.ws.ListObjects.Add(1, self.ws.Range("B2:D6"), None, 1).ShowTotals = True

        def presses(start, key, n):
            self.keys(f"g:{start}|w:3")
            out = []
            for _ in range(n):
                self.keys(f"k:{key}|w:4")
                out.append(excel.Selection.Address)
            self.assertEqual(excel.ActiveCell.Address, self.ws.Range(start).Address)
            return out

        # Excel 16, measured by keys posted to its sheet window
        self.assertEqual(presses("C4", "c-Space", 3), ["$C$3:$C$6", "$C$2:$C$7", "$C:$C"])
        self.assertEqual(presses("C2", "c-Space", 2), ["$C$2:$C$7", "$C:$C"])
        self.assertEqual(presses("C7", "c-Space", 2), ["$C$2:$C$7", "$C:$C"])
        self.assertEqual(presses("C4", "s-Space", 2), ["$B$4:$D$4", "$4:$4"])
        self.assertEqual(presses("C4", "c-A", 3), ["$B$3:$D$6", "$B$2:$D$7", "$1:$1048576"])
        self.assertEqual(presses("C2", "c-A", 2), ["$B$2:$D$7", "$1:$1048576"])
        self.assertEqual(presses("C4", "c-s-Space", 3), ["$B$3:$D$6", "$B$2:$D$7", "$1:$1048576"])
        # outside the table they work as before
        self.assertEqual(presses("F4", "c-Space", 1), ["$F:$F"])

    def test_insert_and_delete_in_a_table_act_on_its_rows_and_columns(self):
        from yc import excel

        excel.Application.DisplayAlerts = False

        def run(sel, script, act="C4"):
            while excel.Worksheets.Count > 1:
                excel.Worksheets(excel.Worksheets.Count).Delete()
            ws = excel.Worksheets.Add(After=excel.Worksheets(1))
            ws.Activate()
            ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
            for r in range(3, 7):
                ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.5]]
            ws.Range("F2").Value = "side"
            ws.Range("F4").Value = "keep4"
            ws.ListObjects.Add(1, ws.Range("B2:D6"), None, 1)
            ws.Range(sel).Select()
            ws.Range(act).Activate()
            self.keys(f"m:2,2|w:3|{script}|w:6")
            self.assertFalse(yc.ui.state().get("dialog"), (sel, script))
            lo = ws.ListObjects(1) if ws.ListObjects.Count else None
            col_b = [ws.Range(f"B{r}").Value for r in range(3, 8)]
            return (lo.Range.Address if lo else None), col_b, ws.Range("F4").Value, ws.Range("E4").Value

        # Ctrl+- and Ctrl+Shift+= in a table, without a dialog: columns when the selection is taller than wide, else
        # rows; deleting across every column deletes rows; only the table's cells move (Excel 16, measured by keys)
        rows_gone = ("$B$2:$D$5", ["S3", "S5", "S6", None, None], "keep4", None)
        self.assertEqual(run("C4", "k:c-Minus"), rows_gone)
        self.assertEqual(run("B4:D4", "k:c-Minus"), rows_gone)
        self.assertEqual(run("C4:D4", "k:c-Minus"), rows_gone)
        self.assertEqual(run("C3:C5", "k:c-Minus"), ("$B$2:$C$6", ["S3", "S4", "S5", "S6", None], None, "keep4"))
        self.assertEqual(run("B3:D6", "k:c-Minus")[:2], ("$B$2:$D$3", [None, None, None, None, None]))
        self.assertEqual(run("C3:D4", "k:c-Minus")[:2], ("$B$2:$D$4", ["S5", "S6", None, None, None]))
        self.assertEqual(run("C2", "k:c-Minus", "C2")[0], "$B$2:$D$6")  # the header row alone: nothing
        self.assertEqual(run("C4", "k:c-s-Equal")[:2], ("$B$2:$D$7", ["S3", None, "S4", "S5", "S6"]))
        self.assertEqual(run("C3:D4", "k:c-s-Equal")[0], "$B$2:$D$8")
        self.assertEqual(run("B3:D6", "k:c-s-Equal")[0], "$B$2:$G$6")
        # the cell menu's Insert and Delete are the table's in a table: Shift+F10, I, A and D, C
        self.assertEqual(run("C4", "k:s-F10|w:4|k:I|w:4|k:A")[0], "$B$2:$D$7")
        self.assertEqual(run("C4", "k:s-F10|w:4|k:D|w:4|k:C")[0], "$B$2:$C$6")
        # Undo takes a table row back in one step
        run("C3:D4", "k:c-Minus")
        self.keys("k:c-Z|w:5")
        self.assertEqual(excel.ActiveSheet.ListObjects(1).Range.Address, "$B$2:$D$6")
        self.assertEqual(excel.ActiveSheet.Range("B4").Value, "S4")

    def test_ctrl_enter_over_a_new_table_column_types_the_data_cells(self):
        self.ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.5]]
        self.ws.ListObjects.Add(1, self.ws.Range("B2:D6"), None, 1)
        self.ws.Range("E2:E6").Select()
        # 1 Ctrl+Enter beside the table grows it: E2 becomes the header "1", E3:E6 the number 1 (Excel 16, measured by
        # keys); the header made from the entry was copied as text into every cell
        self.keys("m:2,2|w:3|t:1|w:2|k:c-Enter|w:6")
        self.assertEqual(self.ws.ListObjects(1).Range.Address, "$B$2:$E$6")
        self.assertEqual([self.ws.Range(f"E{r}").Value for r in range(2, 7)], ["1", 1, 1, 1, 1])
        # the first Ctrl+Z takes back the growth (Excel 16, measured on x Ctrl+Enter under a table), the second the entry
        self.keys("k:c-Z|w:6")
        self.assertEqual(self.ws.ListObjects(1).Range.Address, "$B$2:$D$6")
        self.assertEqual([self.ws.Range(f"E{r}").Value for r in range(3, 7)], [1, 1, 1, 1])
        self.keys("k:c-Z|w:6")
        self.assertEqual([self.ws.Range(f"E{r}").Value for r in range(2, 7)], [None] * 5)

    def test_text_being_typed_is_entered_when_another_tab_is_clicked(self):
        x, y = self.tab(self.other)
        self.keys(f"g:B1|w:2|t:hello|w:2|m:{x},{y}|w:3|c:{x},{y}|w:5")
        self.assertFalse(yc.ui.state().get("edit"), "the entry stayed open on the other sheet")
        self.assertEqual(self.ws.Range("B1").Value2, "hello")
        self.keys("g:C2|w:2|t:7|k:Enter|w:4")
        self.assertEqual(self.other.Range("C2").Value2, 7)  # typing works on the sheet clicked

    def test_a_formula_points_at_another_sheet_and_enter_goes_back(self):
        from yc import excel

        self.other.Range("B3").Value2 = 42
        x, y = self.tab(self.other)
        self.keys(f"g:C1|w:2|t:=|w:3|m:{x},{y}|w:3|c:{x},{y}|w:5|A:B3|w:4|k:Enter|w:6")
        self.assertFalse(yc.ui.state().get("edit"), "Enter did nothing on the other sheet")
        self.assertEqual(self.ws.Range("C1").Formula, "=" + self.other.Name + "!B3")
        self.assertEqual(self.ws.Range("C1").Value2, 42)
        self.assertEqual(excel.Application.ActiveSheet.Name, self.ws.Name)  # back where the entry is
        # Escape cancels and comes back too
        x, y = self.tab(self.other)
        self.keys(f"g:D1|w:2|t:=|w:3|m:{x},{y}|w:3|c:{x},{y}|w:5|A:B3|w:4|k:Escape|w:6")
        self.assertFalse(yc.ui.state().get("edit"))
        self.assertIsNone(self.ws.Range("D1").Value2)
        self.assertEqual(excel.Application.ActiveSheet.Name, self.ws.Name)
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_pointing_at_a_tables_parts_writes_structured_references(self):
        from yc import excel

        self.ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.5]]
        name = self.ws.ListObjects.Add(1, self.ws.Range("B2:D6"), None, 1).Name

        def point(cell, keys):
            self.ws.Range(cell).ClearContents()
            self.keys(f"m:2,2|g:{cell}|w:3|t:=|w:2|" + "|w:2|".join(keys.split("|")) + "|w:2|k:Enter|w:6")
            return str(self.ws.Range(cell).Formula).replace(name, "T")

        L3, SD, SL = "k:LeftArrow|k:LeftArrow|k:LeftArrow", "k:s-DownArrow", "k:s-LeftArrow"
        # Excel 16 with "Use table names in formulas" (its default), measured by keys posted to its sheet window
        self.assertEqual(point("G3", L3), "=T[@Px]")
        self.assertEqual(point("G3", f"{L3}|{SD}|{SD}|{SD}"), "=T[Px]")
        self.assertEqual(point("G3", f"{L3}|{SD}|{SD}|{SD}|{SL}"), "=T[[Qty]:[Px]]")
        self.assertEqual(point("G3", f"{L3}|k:UpArrow|{SD}|{SD}|{SD}|{SD}"), "=T[[#All],[Px]]")
        self.assertEqual(point("G3", f"{L3}|k:UpArrow|{SD}|{SD}|{SD}|{SD}|{SL}|{SL}"), "=T[#All]")
        self.assertEqual(point("G3", f"{L3}|{SD}|{SD}|{SD}|{SL}|{SL}"), "=T")
        self.assertEqual(point("G3", f"{L3}|{SL}"), "=T[@[Qty]:[Px]]")
        self.assertEqual(point("G3", f"{L3}|{SL}|{SL}"), "=T[@]")
        self.assertEqual(point("G3", f"{L3}|k:DownArrow"), "=D4")  # another row: the cell
        self.assertEqual(point("G3", f"{L3}|{SD}|{SD}"), "=D3:D5")  # part of a column: the cells
        # in the table, or a column it grows into: one column without the name, the header with it
        self.assertEqual(point("D3", "k:LeftArrow"), "=[@Qty]")
        self.assertEqual(point("C3", "k:UpArrow"), "=T[[#Headers],[Qty]]")
        self.assertEqual(point("E3", "k:LeftArrow|t:*|k:LeftArrow|k:LeftArrow"), "=[@Px]*[@Qty]")
        self.assertEqual(str(self.ws.Range("E5").Formula), "=[@Px]*[@Qty]")  # a calculated column
        # the option off (Application.GenerateTableRefs = xlGenerateTableRefsA1): the cells' address
        excel.Application.GenerateTableRefs = 0
        try:
            self.assertEqual(excel.Application.GenerateTableRefs, 0)
            self.assertEqual(point("G3", L3), "=D3")
        finally:
            excel.Application.GenerateTableRefs = 1

    def test_a_validation_warning_answers_no_to_enter(self):
        # Excel 16's Warning alert (read through UI Automation): "This value doesn't match the data validation
        # restrictions defined for this cell." and Continue?, with No as the default button, so Enter goes back to the
        # entry; YCGrid's default was Yes and Enter took the value. A hidden instance answers what Enter would.
        self.ws.Range("A1").Validation.Add(1, 2, 1, "1", "5")  # whole number 1..5, xlValidAlertWarning
        self.ws.Range("A1").Value = 3
        n = len(yc.ui.errors())
        self.keys("m:2,2|g:A1|w:3|t:9|w:2|k:Enter|w:6")
        texts = [str(e.get("text")) for e in yc.ui.errors()[n:]]
        self.assertTrue(any(t.startswith("This value doesn't match the data validation restrictions defined for this cell.") for t in texts), texts)
        self.assertTrue(yc.ui.state().get("edit"), "No goes back to the entry")
        self.assertEqual(self.ws.Range("A1").Value, 3)
        self.keys("k:Escape|w:4")
        self.assertEqual(self.ws.Range("A1").Value, 3)

    def test_create_table_guesses_the_header_row_as_excel(self):
        from yc import excel

        excel.Application.DisplayAlerts = False

        def made(rows, by_keys):
            while excel.Worksheets.Count > 1:
                excel.Worksheets(excel.Worksheets.Count).Delete()
            ws = excel.Worksheets.Add(After=excel.Worksheets(1))
            ws.Activate()
            ws.Range(f"B2:D{len(rows) + 1}").Value = rows
            if by_keys:
                self.keys("m:2,2|g:C4|w:3|k:c-T|w:6|k:Enter|w:6")
            else:
                ws.ListObjects.Add(1, ws.Range("B2:D6"))  # xlGuess
            return ws.ListObjects(1).Range.Address

        # Excel 16, measured through its dialog and ListObjects.Add(xlGuess): text over numbers is a header, all text
        # or a header with a blank is not (a header row is added), a year among text headers still is
        text_nums = [["Sym", "Qty", "Px"]] + [[f"S{r}", r, r * 2] for r in range(3, 7)]
        all_text = [[f"t{r}{c}" for c in range(3)] for r in range(5)]
        blank = [["Sym", None, "Px"]] + [[f"S{r}", r, r * 2] for r in range(3, 7)]
        year = [["Sym", 2024, "Px"]] + [[f"S{r}", r, r * 2] for r in range(3, 7)]
        for by_keys in (True, False):
            self.assertEqual(made(text_nums, by_keys), "$B$2:$D$6", by_keys)
            self.assertEqual(made(all_text, by_keys), "$B$2:$D$7", by_keys)
            self.assertEqual(made(blank, by_keys), "$B$2:$D$7", by_keys)
            self.assertEqual(made(year, by_keys), "$B$2:$D$6", by_keys)

    def test_a_note_left_empty_stays(self):
        # Excel 16, measured by keys: Shift+F2 then Escape leaves a note on the cell, and a note emptied by Backspace
        # in its box stays; YCGrid made no note and deleted the emptied one
        self.keys("m:2,2|g:C3|w:3|k:s-F2|w:6|k:Escape|w:6")
        self.assertIsNotNone(self.ws.Range("C3").Comment, "a new note left empty stays")
        self.assertEqual(self.ws.Range("C3").Comment.Text(), "")
        self.ws.Range("B2").AddComment("abc")
        self.keys("g:B2|w:3|k:s-F2|w:6|k:Backspace|k:Backspace|k:Backspace|w:3|k:Escape|w:6")
        self.assertIsNotNone(self.ws.Range("B2").Comment, "an emptied note stays")
        self.assertEqual(self.ws.Range("B2").Comment.Text(), "")
        self.keys("k:c-Z|w:6")
        self.assertEqual(self.ws.Range("B2").Comment.Text(), "abc")

    def test_the_filter_search_box_takes_wildcards_as_the_whole_value(self):
        # Excel 16, measured through its filter menu: text is found inside the values, a pattern with * or ? is matched
        # against the whole value; YCGrid looked for "t?" inside them and found nothing
        syms = ["Sym", "TXF", "MTX", "TE", "txo", "2330", "TXF", "ABC"]
        for text, want in (("tx", ["TXF", "MTX", "txo", "TXF"]), ("t?", ["TE"]), ("*x", ["MTX"]), ("23", ["2330"])):
            if self.ws.AutoFilterMode:
                self.ws.AutoFilterMode = False
            self.ws.Range("A1:B8").Value = [[s, i] for i, s in enumerate(syms)]
            self.ws.Range("A1:B8").AutoFilter()
            self.keys(f"m:2,2|g:A1|w:3|k:a-DownArrow|w:8|k:E|w:4|t:{text}|w:6|k:Enter|w:8")
            shown = [self.ws.Cells(r, 1).Text for r in range(2, 9) if not self.ws.Rows(r).Hidden]
            self.assertEqual(shown, want, text)

    def test_a_calculated_column_and_a_growth_are_undo_steps_of_their_own(self):
        # Excel 16, measured by keys: =c3*d3 typed beside a table grows it and fills the column; the first Ctrl+Z
        # takes back the fill, the second the growth, the third the typing. YCGrid took all three back at once.
        self.ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.5]]
        lo = self.ws.ListObjects.Add(1, self.ws.Range("B2:D6"), None, 1)
        self.keys("m:2,2|g:E3|w:3|t:=c3*d3|w:2|k:Enter|w:6")
        col = lambda: [str(self.ws.Range(f"E{r}").Formula or "") for r in range(3, 7)]  # noqa: E731
        self.assertEqual(col(), ["=C3*D3", "=C4*D4", "=C5*D5", "=C6*D6"])
        self.assertEqual(lo.Range.Address, "$B$2:$E$6")
        self.keys("k:c-Z|w:6")
        self.assertEqual(col(), ["=C3*D3", "", "", ""])
        self.assertEqual(lo.Range.Address, "$B$2:$E$6")
        self.keys("k:c-Z|w:6")
        self.assertEqual(col(), ["=C3*D3", "", "", ""])
        self.assertEqual(lo.Range.Address, "$B$2:$D$6")
        self.keys("k:c-Z|w:6")
        self.assertEqual(col(), ["", "", "", ""])

    def test_a_typed_address_becomes_a_link_in_a_step_of_its_own(self):
        from yc import excel

        # Excel 16, measured by keys: AutoFormat As You Type makes the link and the Hyperlink style; the first Ctrl+Z
        # takes back the link and keeps the text, the second the text
        self.keys("m:2,2|g:B2|w:3|t:www.example.com|w:2|k:Enter|w:6")
        self.assertEqual(self.ws.Hyperlinks.Count, 1)
        self.assertEqual(self.ws.Hyperlinks(1).Address, "http://www.example.com/")
        self.assertEqual(self.ws.Range("B2").Style.Name, "Hyperlink")
        self.keys("k:c-Z|w:6")
        self.assertEqual(self.ws.Hyperlinks.Count, 0)
        self.assertEqual(self.ws.Range("B2").Value, "www.example.com")
        self.keys("k:c-Z|w:6")
        self.assertIsNone(self.ws.Range("B2").Value)
        self.keys("g:B3|w:3|t:example.com|w:2|k:Enter|w:6")  # no scheme, no www.: text
        self.assertEqual(self.ws.Hyperlinks.Count, 0)
        excel.Application.AutoFormatAsYouTypeReplaceHyperlinks = False
        try:
            self.keys("g:B4|w:3|t:a@b.com|w:2|k:Enter|w:6")
            self.assertEqual(self.ws.Hyperlinks.Count, 0)
        finally:
            excel.Application.AutoFormatAsYouTypeReplaceHyperlinks = True

    def test_a_row_typed_under_a_list_takes_its_formats_and_formulas(self):
        from yc import excel

        # Excel 16, measured by keys (Extend data range formats and formulas): four rows of a trade list with a bold
        # symbol, a 0.00 price and =B*C; the row typed under it takes the bold, the 0.00 and, once B and C are
        # typed, the formula. Two rows are too few. YCGrid extended nothing.
        def make(last):
            self.ws.Cells.Clear()
            self.ws.Range("A1:D1").Value = [["Sym", "Qty", "Px", "Amt"]]
            for r in range(2, last + 1):
                self.ws.Range(f"A{r}:C{r}").Value = [[f"s{r}", r, r * 10]]
                self.ws.Range(f"D{r}").Formula = f"=B{r}*C{r}"
                self.ws.Range(f"C{r}").NumberFormat = "0.00"
                self.ws.Range(f"A{r}").Font.Bold = True

        make(5)
        self.keys("m:2,2|g:A6|w:2|t:x|k:Tab|w:3|t:7|k:Tab|w:3|t:3|k:Tab|w:5")
        self.assertTrue(self.ws.Range("A6").Font.Bold)
        self.assertEqual(self.ws.Range("C6").NumberFormat, "0.00")
        self.assertEqual(self.ws.Range("D6").Formula, "=B6*C6")
        self.assertEqual(self.ws.Range("D6").Value, 21)
        make(3)
        self.keys("g:A4|w:2|t:x|k:Tab|w:3|t:7|k:Tab|w:3|t:3|k:Tab|w:5")
        self.assertFalse(self.ws.Range("A4").Font.Bold)
        self.assertIsNone(self.ws.Range("D4").Formula or None)
        excel.Application.ExtendList = False
        try:
            make(5)
            self.keys("g:A6|w:2|t:x|k:Enter|w:5")
            self.assertFalse(self.ws.Range("A6").Font.Bold)
        finally:
            excel.Application.ExtendList = True

    def test_the_sort_dialog_guesses_the_header_row_as_excel(self):
        from yc import excel

        # Excel 16's Sort dialog (its check box read through UI Automation) and Range.Sort(Header=xlGuess) guess as
        # Create Table does: a year among text headers is a header row, a header row with a blank is not
        def guess(rows):
            self.ws.Cells.Clear()
            self.ws.Range("B2:D6").Value = rows
            self.keys("m:2,2|g:C4|w:3")
            yc.ui.command("CustomSort")
            self.keys("w:6")
            box = [i for i in yc.ui.items("") if i.get("label") == "My data has headers"]
            self.keys("k:Escape|w:4")
            self.assertTrue(box)
            return box[0].get("checked")

        nums = [[f"S{r}", r, r * 2] for r in range(3, 7)]
        self.assertTrue(guess([["Sym", 2024, "Px"]] + nums))
        self.assertFalse(guess([["Sym", None, "Px"]] + nums))
        self.assertTrue(guess([["Sym", "Qty", "Px"]] + nums))
        self.assertFalse(guess([[f"t{r}{c}" for c in range(3)] for r in range(5)]))
        # the object model's xlGuess: the year row stays on top of a descending sort
        self.ws.Cells.Clear()
        self.ws.Range("B2:D6").Value = [["Sym", 2024, "Px"], ["a", 1, 2], ["b", 5, 3], ["c", 3, 4], ["d", 2, 5]]
        self.ws.Range("B2:D6").Sort(Key1=self.ws.Range("C2"), Order1=excel.xlDescending, Header=excel.xlGuess)
        self.assertEqual(self.ws.Range("C2").Value, 2024)
        self.assertEqual(self.ws.Range("C3").Value, 5)

    def test_autosum_under_a_table_makes_its_totals_row(self):
        from yc import excel

        excel.Application.DisplayAlerts = False

        def table():
            while excel.Worksheets.Count > 1:
                excel.Worksheets(excel.Worksheets.Count).Delete()
            ws = excel.Worksheets.Add(After=excel.Worksheets(1))
            ws.Activate()
            ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
            for r in range(3, 7):
                ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.5]]
            return ws, ws.ListObjects.Add(1, ws.Range("B2:D6"), None, 1)

        # Excel 16, measured by keys: Alt+= right under a table makes the row its totals row, nothing else written
        # in it; YCGrid grew the data over the total and =SUM(C3:C6) became the circular =SUM(C3:C7)
        ws, lo = table()
        self.keys("m:2,2|g:C7|w:3|k:a-Equal|w:6|k:Enter|w:6")
        self.assertEqual(ws.Range("C7").Formula, "=SUBTOTAL(109,[Qty])")
        self.assertTrue(lo.ShowTotals)
        self.assertEqual(lo.Range.Address, "$B$2:$D$7")
        self.assertIsNone(ws.Range("B7").Value)
        self.assertEqual(ws.Range("C7").Value, 18)
        ws, lo = table()
        self.keys("g:C7:D7|w:3|k:a-Equal|w:6")
        self.assertEqual([ws.Range("C7").Formula, ws.Range("D7").Formula], ["=SUBTOTAL(109,C3:C6)", "=SUBTOTAL(109,D3:D6)"])
        self.assertTrue(lo.ShowTotals)
        # a formula typed into the totals row stays when a table option changes (Banded Rows cleared it)
        self.keys("g:C7|w:2|t:=MAX(C3:C6)|k:Enter|w:5|g:C4|w:2|x:TblBandedRows|w:5")
        self.assertEqual(ws.Range("C7").Formula, "=MAX(C3:C6)")

    def test_tab_in_the_last_cell_adds_a_row_above_the_totals_row(self):
        # Excel 16, measured by keys: Tab in the last data cell of a table with a totals row adds a data row above it
        # (the totals move down) and goes to its first cell; YCGrid left the table for the cell beside it
        self.ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.5]]
        lo = self.ws.ListObjects.Add(1, self.ws.Range("B2:D6"), None, 1)
        lo.ShowTotals = True
        from yc import excel

        self.keys("m:2,2|g:D6|w:3|k:Tab|w:6")
        self.assertEqual(excel.ActiveCell.Address, "$B$7")
        self.assertEqual(lo.Range.Address, "$B$2:$D$8")
        self.assertEqual(self.ws.Range("B8").Value, "Total")
        self.keys("g:D8|w:3|k:Tab|w:4")  # the totals row's last cell: the cell beside it
        self.assertEqual(excel.ActiveCell.Address, "$E$8")

    def test_rules_on_a_tables_column_grow_with_it(self):
        # Excel 16, measured by keys: a conditional format and a validation on C3:C6 of B2:D6 apply to C3:C7 once a row
        # is typed under it; one on C3:C5 stays. YCGrid left them on the old rows
        self.ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.5]]
        self.ws.ListObjects.Add(1, self.ws.Range("B2:D6"), None, 1)
        self.ws.Range("C3:C6").FormatConditions.Add(1, 5, "=3")
        self.ws.Range("D3:D5").FormatConditions.Add(1, 5, "=3")
        self.ws.Range("C3:C6").Validation.Add(1, 1, 1, "0", "100")
        self.keys("m:2,2|g:B7|w:3|t:x|k:Enter|w:6")
        self.assertEqual(sorted(fc.AppliesTo.Address for fc in self.ws.Cells.FormatConditions), ["$C$3:$C$7", "$D$3:$D$5"])
        self.assertEqual(self.ws.Range("C7").Validation.Type, 1)
        self.keys("k:c-Z|w:6")  # the growth's step: the rules go back with it
        self.assertEqual(sorted(fc.AppliesTo.Address for fc in self.ws.Cells.FormatConditions), ["$C$3:$C$6", "$D$3:$D$5"])

    def test_the_column_headings_show_a_tables_names_when_its_header_scrolls_away(self):
        from yc import ui

        # Excel shows a table's column names in the column headings while the active cell is in the table and its
        # header row is out of view (Microsoft documents it); YCGrid kept the letters
        self.ws.Range("B2:D2").Value = [["Symbol", "Quantity", "Price"]]
        self.ws.Range("B3:D200").Value = [[f"S{r}", r, r * 1.5] for r in range(3, 201)]
        self.ws.ListObjects.Add(1, self.ws.Range("B2:D200"), None, 1).Name = "Trades"
        self.keys("m:2,2|g:C3|w:4")
        self.assertEqual(ui.state()["table_headings"], "")
        self.keys("g:C150|w:6")
        self.assertEqual(ui.state()["table_headings"], "Trades")
        self.keys("g:G150|w:6")
        self.assertEqual(ui.state()["table_headings"], "")
        self.keys("k:c-Home|w:4")

    def test_go_to_and_the_name_box_take_a_tables_references(self):
        from yc import excel, ui

        # Excel 16, measured through Application.Goto: a table's name is its data, and its structured references
        # their cells; YCGrid called them invalid (Goto, Range("Trades[Qty]")) or tried to define them as names
        self.ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 2]]
        lo = self.ws.ListObjects.Add(1, self.ws.Range("B2:D6"), None, 1)
        lo.Name = "Trades"
        lo.ShowTotals = True
        want = {"Trades": "$B$3:$D$6", "Trades[Qty]": "$C$3:$C$6", "Trades[#All]": "$B$2:$D$7", "Trades[#Totals]": "$B$7:$D$7"}
        for ref, addr in want.items():
            self.ws.Range("A1").Select()
            excel.Application.Goto(ref)
            self.assertEqual(excel.Selection.Address, addr, ref)
            self.assertEqual(self.ws.Range(ref).Address, addr, ref)
            self.ws.Range("A1").Select()
            box = [i for i in ui.items("") if i.get("id") == "##namebox" or i.get("label") == "##namebox"][0]["rect"]
            x, y = int((box[0] + box[2]) / 2), int((box[1] + box[3]) / 2)
            self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3|k:c-A|t:{ref}|k:Enter|w:5")
            self.assertEqual(excel.Selection.Address, addr, ref)

    def test_rules_refuse_structured_references_as_excel(self):
        from yc import excel

        # Excel 16, measured through COM: Validation.Add with =Trades[Sym] and FormatConditions.Add with [@Qty] raise
        # (1004); INDIRECT("Trades[Sym]") and a name on the table are taken. YCGrid took them all
        self.ws.Range("B2:C2").Value = [["Sym", "Qty"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:C{r}").Value = [[f"S{r}", r]]
        self.ws.ListObjects.Add(1, self.ws.Range("B2:C6"), None, 1).Name = "Trades"
        with self.assertRaises(Exception):
            self.ws.Range("F2").Validation.Add(3, 1, 1, "=Trades[Sym]")
        with self.assertRaises(Exception):
            self.ws.Range("C3:C6").FormatConditions.Add(2, 0, "=[@Qty]>3")
        self.ws.Range("F3").Validation.Add(3, 1, 1, '=INDIRECT("Trades[Sym]")')
        self.assertEqual(self.ws.Range("F3").Validation.Type, 3)
        excel.ActiveWorkbook.Names.Add("QtyCol", "=Trades[Qty]")

    def test_the_totals_row_shows_the_columns_number_format(self):
        # Excel 16, measured through ShowTotals: the totals cell under a column of 0.00, $#,##0.00 or 0% takes that
        # format (22.50, $22.50, 2250%); YCGrid showed General (22.5)
        self.ws.Range("B2:D2").Value = [["Sym", "Qty", "Px"]]
        for r in range(3, 7):
            self.ws.Range(f"B{r}:D{r}").Value = [[f"S{r}", r, r * 1.25]]
        self.ws.Range("D3:D6").NumberFormat = "$#,##0.00"
        lo = self.ws.ListObjects.Add(1, self.ws.Range("B2:D6"), None, 1)
        lo.ShowTotals = True
        self.assertEqual(self.ws.Range("D7").NumberFormat, "$#,##0.00")
        self.assertEqual(self.ws.Range("D7").Text, "$22.50")

    def test_word_keys_in_the_cell_editor(self):
        # Excel 16, measured by keys after F2: Ctrl+Left / Ctrl+Right go by words (punctuation is one), Ctrl+Delete
        # deletes to the end, and Ctrl+Backspace deletes nothing (YCGrid deleted a word)
        for text, keys, want in (("abc def ghi", "k:c-LeftArrow|w:2|k:c-LeftArrow", "abc xdef ghi"),
                                 ("abc.def/ghi", "k:c-LeftArrow|w:2|k:c-LeftArrow", "abc.defx/ghi"),
                                 ("abc def ghi", "k:c-Backspace", "abc def ghix")):
            self.ws.Range("B2").ClearContents()
            self.keys(f"m:2,2|g:B2|w:3|t:{text}|w:2|k:F2|w:2|{keys}|w:2|t:x|w:2|k:Enter|w:4")
            self.assertEqual(self.ws.Range("B2").Value, want, keys)

    def test_typing_in_a_data_table_says_only_why_it_is_refused(self):
        # Excel shows "Cannot change part of a data table." alone; YCGrid showed an empty "There's a problem with this
        # formula" first
        self.ws.Range("A1").Value = 5
        self.ws.Range("A2").Value = 2
        self.ws.Range("B4").Formula = "=A1*A2"
        self.ws.Range("A5:A7").Value = [[1], [2], [3]]
        self.ws.Range("A4:B7").Table(None, self.ws.Range("A1"))
        n = len(yc.ui.errors())
        self.keys("m:2,2|g:B6|w:3|t:9|k:Enter|w:5|k:Escape|w:3")
        texts = [str(e.get("text")) for e in yc.ui.errors()[n:]]
        self.assertEqual(len(texts), 1, texts)
        self.assertTrue(texts[0].startswith("Cannot change part of a data table."), texts)
        self.assertEqual(self.ws.Range("B6").Formula, "=TABLE(,A1)")

    def test_whole_rows_and_columns_copied_bring_their_sizes(self):
        # Excel 16, measured 2026-11-28 through COM: Rows(1).Copy Rows(5) gives row 5 row 1's height, Columns(1).Copy
        # Columns(4) column D column A's width, a cut moves them (the rows left take the standard height), a hidden row
        # copied is hidden; cells copied bring neither
        ws = self.ws
        std_h, std_w = ws.Rows(30).RowHeight, ws.Columns(30).ColumnWidth
        ws.Range("A1").Value = "x"
        ws.Rows(1).RowHeight = 30
        ws.Columns(1).ColumnWidth = 20
        ws.Rows(1).Copy(ws.Rows(5))
        self.assertEqual(ws.Rows(5).RowHeight, 30)
        ws.Range("A1:C1").Copy(ws.Range("A7"))
        self.assertEqual(ws.Rows(7).RowHeight, std_h)
        ws.Columns(1).Copy(ws.Columns(4))
        self.assertAlmostEqual(ws.Columns(4).ColumnWidth, ws.Columns(1).ColumnWidth, delta=0.01)
        ws.Range("A1:A3").Copy(ws.Range("F1"))
        self.assertAlmostEqual(ws.Columns(6).ColumnWidth, std_w, delta=0.01)
        ws.Rows(1).Cut(ws.Rows(9))
        self.assertEqual((ws.Rows(9).RowHeight, ws.Rows(1).RowHeight), (30, std_h))
        ws.Rows(9).Copy(ws.Rows("11:12"))
        self.assertEqual((ws.Rows(11).RowHeight, ws.Rows(12).RowHeight), (30, 30))
        ws.Rows(9).Hidden = True
        ws.Rows(9).Copy(ws.Rows(14))
        self.assertTrue(ws.Rows(14).Hidden)
        ws.Rows(9).Hidden = False
        # Ctrl+C on row 9's header, Ctrl+V on row 20's
        self.keys("m:2,2|g:9:9|w:2|k:c-C|w:3|g:20:20|w:2|k:c-V|w:5|k:Escape|w:2")
        self.assertEqual(ws.Rows(20).RowHeight, 30)

    def test_autofit_columns_leaves_wrapped_and_multi_line_text_at_its_width(self):
        # Excel 16, measured 2026-11-28: AutoFit fits one line of 26 letters, but text with a line break or wrapped
        # text keeps its column near the standard width (its widest line as laid out now)
        ws = self.ws
        std = ws.Columns(9).ColumnWidth
        ws.Range("A1").Value = "ab\ncdefghijklmnopqrstuvwxyz"
        ws.Range("B1").Value = "abcdefghijklmnopqrstuvwxyz"
        ws.Range("C1").Value = "abcdefghijklmnopqrstuvwxyz"
        ws.Range("C1").WrapText = True
        self.keys("m:2,2|g:A1:C1|w:2|x:AutoFitColumns|w:5")
        self.assertLessEqual(ws.Columns(1).ColumnWidth, std + 0.01)
        self.assertGreater(ws.Columns(2).ColumnWidth, 2 * std)
        self.assertLessEqual(ws.Columns(3).ColumnWidth, std + 0.01)

    def test_a_row_that_sizes_itself_shrinks_back_when_its_lines_are_cleared(self):
        # Excel 16, measured 2026-11-28 by keys and COM: two lines typed with Alt+Enter, then Delete: the standard
        # height again; Ctrl+Z brings the two lines' height back; a 28 pt cell cleared keeps its height; a height set
        # by hand stays
        ws = self.ws
        std = ws.Rows(1).RowHeight
        self.keys("m:2,2|g:A1|w:2|t:a|k:a-Enter|t:b|k:Enter|w:5")
        two = ws.Rows(1).RowHeight
        self.assertGreater(two, std)
        self.keys("g:A1|w:2|k:Delete|w:5")
        self.assertEqual(ws.Rows(1).RowHeight, std)
        self.keys("k:c-Z|w:5")
        self.assertEqual(ws.Rows(1).RowHeight, two)
        ws.Range("A1").ClearContents()
        self.assertEqual(ws.Rows(1).RowHeight, std)
        self.keys("g:A1|w:2|t:a|k:a-Enter|t:b|k:Enter|w:5")
        self.assertEqual(ws.Rows(1).RowHeight, two)
        ws.Range("A1").Value = "c"  # a script's shorter value: the standard height again (Excel's Range.Value)
        self.assertEqual(ws.Rows(1).RowHeight, std)
        ws.Range("A3").Value = "x"
        ws.Range("A3").Font.Size = 28
        tall = ws.Rows(3).RowHeight
        self.keys("g:A3|w:2|k:Delete|w:5")
        self.assertEqual(ws.Rows(3).RowHeight, tall)
        self.keys("g:A4|w:2|t:a|k:a-Enter|t:b|k:Enter|w:5")
        ws.Rows(4).RowHeight = 40
        hand = ws.Rows(4).RowHeight
        self.keys("g:A4|w:2|k:Delete|w:5")
        self.assertEqual(ws.Rows(4).RowHeight, hand)

    def test_font_size_buttons_step_through_excels_sizes_and_stop_at_72(self):
        # Excel 16, measured 2026-11-28 through ExecuteMso: 6 then the Font Size list; past 72 and under 6 nothing changes
        ws = self.ws
        ws.Range("A1").Value = "x"
        for start, cmd, want in ((72, "FontSizeIncrease", 72), (80, "FontSizeIncrease", 80), (1, "FontSizeIncrease", 6),
                                 (8, "FontSizeDecrease", 6), (5, "FontSizeDecrease", 5), (100, "FontSizeDecrease", 72),
                                 (13, "FontSizeIncrease", 14)):
            ws.Range("A1").Font.Size = start
            self.keys(f"m:2,2|g:A1|w:2|x:{cmd}|w:4")
            self.assertEqual(ws.Range("A1").Font.Size, want, (start, cmd))

    def test_indent_buttons_act_on_several_cells_as_a_block(self):
        # Excel 16, measured 2026-11-28 through ExecuteMso: one Right-aligned cell keeps Right when indented, several
        # cells all become Left; Decrease over several changes nothing while one of them is at 0
        ws = self.ws
        ws.Range("A1").Value = 5
        ws.Range("A2").Value = "r"
        ws.Range("A2").HorizontalAlignment = -4152
        self.keys("m:2,2|g:A2|w:2|x:IndentIncrease|w:4")
        self.assertEqual((ws.Range("A2").HorizontalAlignment, ws.Range("A2").IndentLevel), (-4152, 1))
        ws.Range("A2").IndentLevel = 0
        self.keys("g:A1:A2|w:2|x:IndentIncrease|w:4")
        self.assertEqual([(ws.Range(a).HorizontalAlignment, ws.Range(a).IndentLevel) for a in ("A1", "A2")], [(-4131, 1), (-4131, 1)])
        ws.Range("A2").IndentLevel = 0
        self.keys("g:A1:A2|w:2|x:IndentDecrease|w:4")
        self.assertEqual([ws.Range(a).IndentLevel for a in ("A1", "A2")], [1, 0])
        ws.Range("A2").IndentLevel = 2
        self.keys("g:A1:A2|w:2|x:IndentDecrease|w:4")
        self.assertEqual([ws.Range(a).IndentLevel for a in ("A1", "A2")], [0, 1])
        # the object model alike: one cell keeps Right, several become Left
        ws.Range("A1:A2").IndentLevel = 0
        ws.Range("A2").HorizontalAlignment = -4152
        ws.Range("A2").InsertIndent(1)
        self.assertEqual((ws.Range("A2").HorizontalAlignment, ws.Range("A2").IndentLevel), (-4152, 1))
        ws.Range("A2").IndentLevel = 0
        ws.Range("A1:A2").IndentLevel = 1
        self.assertEqual(ws.Range("A2").HorizontalAlignment, -4131)

    def test_arrows_nudge_a_shape_a_pixel_and_shift_scales_it_about_its_center(self):
        # Excel 16, measured 2026-11-28 by keys: an arrow (with Ctrl too) moves a shape one screen pixel, Shift+Right
        # makes it 10% wider about its center, Shift+Down 10% lower; a shape a script selected takes the keys
        shp = self.ws.Shapes.AddShape(1, 100, 100, 80, 40)
        x0, _, x1, _ = yc.ui.cell_rect("A1")
        px = 1.0 / ((x1 - x0) / self.ws.Range("A1").Width)  # points in a screen pixel
        try:
            for keys, want in (("k:RightArrow", (100 + px, 100, 80, 40)), ("k:c-DownArrow", (100, 100 + px, 80, 40)),
                               ("k:s-RightArrow", (96, 100, 88, 40)), ("k:s-DownArrow", (100, 100 + (40 - 40 / 1.1) / 2, 80, 40 / 1.1)),
                               ("k:c-s-RightArrow", (99.6, 100, 80.8, 40))):
                shp.Left, shp.Top, shp.Width, shp.Height = 100, 100, 80, 40
                shp.Select()
                self.keys(f"w:3|{keys}|w:4")
                for got, exp in zip((shp.Left, shp.Top, shp.Width, shp.Height), want):
                    self.assertAlmostEqual(got, exp, delta=0.02, msg=keys)
        finally:
            self.keys("k:Escape|w:2")
            shp.Delete()

    def test_keys_walk_the_unlocked_cells_of_a_protected_sheet(self):
        # Excel 16, measured 2026-11-28 by keys: locked cells not selectable, the keys go round the unlocked ones:
        # Right / Left in reading order, Down / Up and Enter in column order, Home the first; the other moves stay
        from yc import excel

        ws = self.ws
        ws.Range("A1:F10").Value = 1
        for a in ("B2", "D2", "B4", "C5", "E7"):
            ws.Range(a).Locked = False
        cases = [("B2", "k:RightArrow|w:2|k:RightArrow", "B4"), ("B2", "k:LeftArrow", "E7"), ("D2", "k:DownArrow", "E7"),
                 ("E7", "k:UpArrow", "D2"), ("B4", "k:s-Enter", "B2"), ("C5", "k:Home", "B2"), ("C5", "k:c-Home", "C5"),
                 ("B2", "k:End|w:2|k:RightArrow", "B2"), ("E7", "k:PageDown", "E7"), ("B2", "k:s-RightArrow", "B2"),
                 ("B2", "k:c-A", "B2"), ("B2", "k:s-Space", "B2")]
        try:
            for start, keys, want in cases:
                ws.Unprotect()
                ws.Range(start).Select()
                ws.Protect()
                ws.EnableSelection = excel.xlUnlockedCells
                self.keys(f"m:2,2|w:2|{keys}|w:3")
                self.assertEqual((excel.Selection.Address(False, False), excel.ActiveCell.Address(False, False)), (want, want), (start, keys))
        finally:
            ws.Unprotect()

    def test_a_cell_emptied_loses_its_hyperlink_and_a_copy_brings_one(self):
        # Excel 16, measured 2026-11-28: Delete, Backspace and Enter and ClearContents take the link and keep the
        # formats; typing over keeps it; Copy, FillDown and a cut bring it, a plain cell copied over removes it;
        # Hyperlinks.Add gives the Hyperlink style and Hyperlinks.Delete takes the formats too
        ws = self.ws

        def link(a):
            return ws.Range(a).Hyperlinks.Count

        def setup():
            ws.Cells.Clear()
            ws.Range("B4").Value = "lnk"
            ws.Hyperlinks.Add(ws.Range("B4"), "http://example.com")
            ws.Range("B4").Font.Bold = True

        for keys in ("k:Delete", "k:Backspace|w:2|k:Enter"):
            setup()
            self.keys(f"m:2,2|g:B4|w:3|{keys}|w:5")
            self.assertEqual(link("B4"), 0, keys)
            self.assertTrue(ws.Range("B4").Font.Bold, keys)
        setup()
        self.keys("g:B4|w:3|t:a|k:Enter|w:5")
        self.assertEqual(link("B4"), 1)
        setup()
        ws.Range("B4").ClearContents()
        self.assertEqual(link("B4"), 0)
        setup()
        ws.Range("B4").Value = ""
        self.assertEqual(link("B4"), 0)
        setup()
        ws.Range("B4").Copy(ws.Range("D4"))
        ws.Range("C4").Value = 5
        ws.Range("C4").Copy(ws.Range("B4"))
        self.assertEqual((link("D4"), link("B4")), (1, 0))
        setup()
        ws.Range("B4:B6").FillDown()
        self.assertEqual((link("B5"), link("B6")), (1, 1))
        setup()
        ws.Range("B4").Cut(ws.Range("D7"))
        self.assertEqual((link("B4"), link("D7")), (0, 1))
        setup()
        self.keys("g:B4|w:2|k:c-C|w:3|g:F2|w:2|k:c-V|w:5|k:Escape|w:2")
        self.assertEqual(link("F2"), 1)
        ws.Cells.Clear()
        ws.Range("B8").Value = "z"
        ws.Range("B8").NumberFormat = "0.00"
        ws.Range("B8").Font.Bold = True
        ws.Hyperlinks.Add(ws.Range("B8"), "http://a.com")
        self.assertEqual(ws.Range("B8").Style.Name, "Hyperlink")
        self.assertFalse(ws.Range("B8").Font.Bold)
        self.assertEqual(ws.Range("B8").NumberFormat, "0.00")
        ws.Range("B8").Hyperlinks.Delete()
        self.assertEqual((link("B8"), ws.Range("B8").NumberFormat, ws.Range("B8").Value), (0, "General", "z"))

    def test_ctrl_page_down_points_at_the_next_sheet_while_typing_a_formula(self):
        from yc import excel

        self.other.Range("C2").Value2 = 5
        self.keys("g:C1|w:2|t:=|w:3|k:c-PageDown|w:4|k:Down|w:3|k:Enter|w:6")
        self.assertEqual(self.ws.Range("C1").Formula, "=" + self.other.Name + "!C2")
        self.assertEqual(self.ws.Range("C1").Value2, 5)
        self.assertEqual(excel.Application.ActiveSheet.Name, self.ws.Name)

    def test_ctrl_page_down_leaves_the_sheet_it_leaves_where_it_was(self):
        # Ctrl+PgDn also paged the sheet it left: back on it, the active cell was a screen further down each time
        from yc import excel

        self.keys("g:B3|w:2|k:c-PageDown|w:4")
        self.assertEqual(excel.Application.ActiveSheet.Name, self.other.Name)
        self.keys("k:c-PageUp|w:4")
        self.assertEqual(excel.Application.ActiveSheet.Name, self.ws.Name)
        self.assertEqual(excel.Application.ActiveCell.Address, "$B$3")
        self.assertEqual(excel.Application.ActiveWindow.ScrollRow, 1)

    def test_shift_f8_adds_areas_from_the_keyboard(self):
        # Shift+F8 (Add to Selection) changed nothing for the arrows: the next arrow dropped B2:B4. Excel 16, measured by
        # keys posted to its sheet window: the arrows move an area of its own beside the areas from before, Shift+arrows
        # extend it, and that ends the mode
        sel = lambda: yc.excel.Application.Selection.Address
        self.keys("g:B2:B4|w:2|k:s-F8|w:2|k:RightArrow|w:2|k:RightArrow|w:3")
        self.assertEqual(sel(), "$B$2:$B$4,$D$2")
        self.keys("k:s-DownArrow|w:3")
        self.assertEqual(sel(), "$B$2:$B$4,$D$2:$D$3")
        self.keys("k:RightArrow|w:3")
        self.assertEqual(sel(), "$E$2")
        self.keys("g:B2:B4|w:2|k:s-F8|w:2|k:s-F8|w:2|k:RightArrow|w:3")  # pressed twice: off again
        self.assertEqual(sel(), "$C$2")

    def test_enter_and_home_with_ctrl_or_alt_leave_the_active_cell(self):
        # outside an entry Alt+Enter moved down as Enter and Alt+Home went to column A as Home; Excel 16 does nothing
        # for either, nor for Ctrl+Enter and Ctrl+Shift+Enter (measured by keys posted to its sheet window)
        from yc import excel

        self.ws.Range("B3").Value2 = 7
        for chord in ("a-Enter", "c-Enter", "c-s-Enter", "a-s-Enter", "a-Home"):
            self.keys(f"g:B3|w:2|k:{chord}|w:3|k:Escape|w:2")
            self.assertEqual(excel.Application.ActiveCell.Address, "$B$3", chord)
            self.assertEqual(self.ws.Range("B3").Value2, 7, chord)
        self.keys("g:B3|w:2|k:Enter|w:3")
        self.assertEqual(excel.Application.ActiveCell.Address, "$B$4")
        self.keys("g:B3|w:2|k:Home|w:3")
        self.assertEqual(excel.Application.ActiveCell.Address, "$A$3")

    def test_ctrl_enter_gives_every_cell_the_entry_format(self):
        self.keys("g:F2:F5|w:2|t:5%|k:c-Enter|w:5")
        self.assertEqual([self.ws.Range(f"F{r}").Text for r in range(2, 6)], ["5%"] * 4)  # F3:F5 showed 0.05
        self.keys("g:G2:G4|w:2|t:2024/1/7|k:c-Enter|w:5")
        self.assertEqual(self.ws.Range("G4").Text, self.ws.Range("G2").Text)

    def test_f2_enter_keeps_a_long_number(self):
        self.ws.Range("B1").Value2 = 123456789012
        self.ws.Range("B2").Value2 = 1.2345678901234567e-15
        self.keys("g:B1|w:2|k:F2|w:3|k:Enter|w:4|g:B2|w:2|k:F2|w:3|k:Enter|w:4")
        self.assertEqual(self.ws.Range("B1").Value2, 123456789012)  # was 123457000000
        self.assertEqual(self.ws.Range("B1").NumberFormat, "General")
        self.assertAlmostEqual(self.ws.Range("B2").Value2 / 1.23456789012346e-15, 1, places=13)

    def test_delete_formatting_and_fill_end_copy_mode(self):
        self.ws.Range("A1:A3").Value = [[1], [2], [3]]
        self.ws.Range("B5:B6").Value = [["x"], ["y"]]
        for k in ("k:Delete", "k:c-B", "k:c-D", "k:c-s-4"):
            self.ws.Range("B5:B6").Value = [["x"], ["y"]]
            self.keys(f"g:A1:A3|w:2|k:c-C|w:2|g:B5:B6|w:2|{k}|w:3")
            self.assertFalse(yc.ui.state()["copy_mode"], k + " should end copy mode")
            self.keys("k:Enter|w:3")
            self.assertNotEqual(self.ws.Range("B7").Value2, 3, k + ": Enter pasted the copied cells")

    def test_cut_cells_paste_once(self):
        self.ws.Range("A1:A2").Value = [[1], [2]]
        self.keys("g:A1:A2|w:2|k:c-X|w:2|g:D1|w:2|k:c-V|w:4|g:F1|w:2|k:c-V|w:4")
        self.assertEqual(self.ws.Range("D2").Value2, 2)
        self.assertIsNone(self.ws.Range("F1").Value2)  # was pasted again from the clipboard's text

    def test_escape_that_closes_a_menu_keeps_the_copied_cells(self):
        self.ws.Range("A1:A2").Value = [[1], [2]]
        self.keys("g:A1:A2|w:2|k:c-C|w:2|R:C3|w:5|k:Escape|w:3")
        self.assertTrue(yc.ui.state()["copy_mode"], "the first Escape only closes the menu (Excel)")
        self.keys("g:C3|w:2|k:c-V|w:4")
        self.assertEqual(self.ws.Range("C4").Value2, 2)
        self.keys("k:Escape|w:2|k:Escape|w:2")
        self.assertFalse(yc.ui.state()["copy_mode"])

    def test_a_column_pasted_on_a_row_is_repeated_across_it(self):
        self.ws.Range("A1:A2").Value = [[1], [2]]
        self.keys("g:A1:A2|w:2|k:c-C|w:2|g:I1:J1|w:2|k:c-V|w:4")
        self.assertEqual([self.ws.Range(a).Value2 for a in ("I1", "J1", "I2", "J2")], [1, 1, 2, 2])

    def test_a_paste_that_cuts_a_merged_cell_is_refused(self):
        self.ws.Range("A1:B2").Value = [[1, 2], [3, 4]]
        self.ws.Range("H2:H3").Merge()
        self.keys("g:A1:B2|w:2|k:c-C|w:2|g:H1|w:2|k:c-V|w:5")
        self.assertTrue(self.ws.Range("H2").MergeCells)
        self.assertIsNone(self.ws.Range("H1").Value2)
        self.assertTrue(any("merged cell" in e["text"] for e in yc.ui.errors()[self.errors0:]))

    def test_escape_ends_drawing_a_form_control(self):
        before = self.ws.Shapes.Count
        x0, y0, _, _ = yc.ui.cell_rect("C3")
        _, _, x1, y1 = yc.ui.cell_rect("E6")
        self.keys(f"x:InsertFormButton|w:4|k:Escape|w:3|m:{int(x0) + 4},{int(y0) + 4}|w:3|d:{int(x0) + 4},{int(y0) + 4},{int(x1) - 4},{int(y1) - 4}|w:6|m:2,2")
        self.assertEqual(self.ws.Shapes.Count, before)
        self.assertEqual(self.ws.Range("C3:E6").Address, yc.excel.Application.Selection.Address)

    def test_f4_repeats_inserting_whole_rows(self):
        self.ws.Range("A1:A6").Value = [[1], [2], [3], [4], [5], [6]]
        self.keys("g:A2|w:2|k:s-Space|w:2|k:c-s-Equal|w:4|g:B5|w:2|k:F4|w:5")
        self.assertEqual(yc.ui.state()["dialog"], 0, "F4 opened the Insert dialog")
        self.assertEqual([self.ws.Range(f"A{i}").Value2 for i in range(1, 9)], [1, None, 2, 3, None, 4, 5, 6])


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class DialogsRound8Test(unittest.TestCase):
    """Dialogs used the way a person does (round 8, dialogs): range boxes, drop-downs, Go To, filters, Sort."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.keys("k:Escape|w:2|k:Escape|m:2,2|w:2")
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()
        self.keys("w:3")
        self.errors0 = len(yc.ui.errors())

    def tearDown(self):
        self.keys("k:Escape|w:2|k:Escape|w:2|k:Escape|m:2,2|w:2")

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def item(self, ident, window=""):
        hits = sorted([i for i in yc.ui.items(window) if i["id"] == ident], key=lambda i: (i["rect"][1], i["rect"][0]))
        self.assertTrue(hits, ident + " is shown")
        return hits[0]

    def click(self, rect, dx=None):
        x0, y0, x1, y1 = rect
        x, y = (int((x0 + x1) / 2) if dx is None else int(x0 + dx)), int((y0 + y1) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def test_picking_cells_for_a_range_box_keeps_the_sheet_selection(self):
        for r in range(2, 6):
            self.ws.Cells(r, 1).Value = r
        self.keys("g:D2|w:2|x:CfGreater|w:8")
        self.assertNotEqual(yc.ui.state()["dialog"], 0)
        rb = self.item("##rb")
        bx, by = int(rb["rect"][2] + 22), int((rb["rect"][1] + rb["rect"][3]) / 2)
        self.keys(f"m:{bx},{by}|w:3|c:{bx},{by}|w:6")
        x0, y0, x1, y1 = yc.ui.cell_rect("A3")
        self.keys(f"m:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:3|c:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:5|k:Enter|w:6")
        self.assertEqual(yc.ui.state()["active_cell"], "D2", "picking moved the sheet's selection (Excel puts it back)")
        self.keys("k:Enter|w:6")
        self.assertEqual(self.ws.Range("D2").FormatConditions.Count, 1)
        self.assertEqual(self.ws.Range("A3").FormatConditions.Count, 0)  # the rule went to the cells picked

    def test_escape_on_a_drop_down_in_a_dialog_closes_the_drop_down_only(self):
        self.ws.Range("A1:B3").Value = [["Name", "Qty"], ["b", 2], ["a", 1]]
        self.keys("g:A2|w:2|x:CustomSort|w:8")
        self.assertNotEqual(yc.ui.state()["dialog"], 0)
        self.click(self.item("##ord0")["rect"])
        self.keys("k:Escape|w:4")
        self.assertNotEqual(yc.ui.state()["dialog"], 0, "Escape on the open Order list closed the Sort dialog")
        self.keys("k:Escape|w:4")
        self.assertEqual(yc.ui.state()["dialog"], 0)

    def test_go_to_offers_where_the_last_go_to_started_selected(self):
        self.keys("g:A1|w:2|k:F5|w:6|t:C3:D4|k:Enter|w:5")
        self.assertEqual(yc.ui.state()["active_cell"], "C3")
        self.keys("k:F5|w:6|t:E9|k:Enter|w:5")  # typing replaces what the box offers
        self.assertEqual(yc.ui.state()["active_cell"], "E9")
        self.keys("k:F5|w:6|k:Enter|w:5")  # F5, Enter: back to where the last Go To started
        self.assertEqual(yc.ui.state()["active_cell"], "C3")
        # ...with the whole selection it left, every area (Excel 16, measured: PreviousSelections(1) after Goto
        # from B2:B8 is $B$2:$B$8, from A3:A4,A6,A8 the three areas); only the active cell came back
        self.assertEqual(yc.excel.Application.Selection.Address, "$C$3:$D$4")
        self.keys("g:A3:A4,A6,A8|w:2|k:c-G|w:6|t:H30|k:Enter|w:5")
        self.assertEqual(yc.ui.state()["active_cell"], "H30")
        self.keys("k:F5|w:6|k:Enter|w:5")
        self.assertEqual(yc.excel.Application.Selection.Address, "$A$3:$A$4,$A$6,$A$8")
        self.keys("g:B:C|w:2|k:c-G|w:6|t:Z9|k:Enter|w:5|k:F5|w:6|k:Enter|w:5")
        self.assertEqual(yc.excel.Application.Selection.Address, "$B:$C")
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_a_click_on_a_status_bar_statistic_copies_it(self):
        # Excel 365: a click on Sum: 18 in the status bar copies 18; YCGrid's statistics were text only
        self.ws.Range("A1:A3").Value = [[3], [5], [10]]
        self.keys("g:A1:A3|w:4")
        hits = [i for i in yc.ui.items("") if i["label"] == "Sum: 18"]
        self.assertTrue(hits, "the Sum statistic is listed")
        self.click(hits[0]["rect"])
        self.keys("g:C1|w:2|k:c-V|w:5")
        self.assertEqual(self.ws.Range("C1").Value, 18)

    def test_the_outline_buttons_are_listed_and_clicked(self):
        # the outline's level buttons and +/- were drawn only: ui.items didn't list them, so no test clicked them
        self.ws.Range("A1:A6").Value = [["Q1"], ["Jan"], ["Feb"], ["Mar"], ["Q2"], ["Apr"]]
        self.ws.Range("A2:A4").Rows.Group()
        self.keys("g:A1|w:4")
        self.click(self.item("##outline_rows_1")["rect"])
        self.assertEqual([self.ws.Rows(r).Hidden for r in range(2, 5)], [True, True, True])
        self.click(self.item("##outline_row_5")["rect"])
        self.assertEqual([self.ws.Rows(r).Hidden for r in range(2, 5)], [False, False, False])
        self.ws.Range("A2:A4").Rows.Ungroup()
        # Alt+Shift+Left over collapsed rows ungroups them and leaves them hidden, as Excel 16 does (measured); they
        # were shown. Clear Outline shows them (measured too)
        self.ws.Range("A2:A4").Rows.Group()
        self.keys("w:4")
        self.click(self.item("##outline_rows_1")["rect"])
        self.keys("g:A1:A5|w:2|k:s-Space|w:2|k:a-s-LeftArrow|w:5")
        self.assertEqual([self.ws.Rows(r).OutlineLevel for r in range(2, 5)], [1, 1, 1])
        self.assertEqual([self.ws.Rows(r).Hidden for r in range(2, 5)], [True, True, True])
        self.ws.Rows("2:4").Hidden = False
        self.ws.Range("A2:A4").Rows.Group()
        self.keys("w:4")
        self.click(self.item("##outline_rows_1")["rect"])
        self.ws.Range("A1").ClearOutline()
        self.assertEqual([self.ws.Rows(r).Hidden for r in range(2, 5)], [False, False, False])

    def test_task_panes_sit_beside_the_sheets(self):
        # the Clipboard pane floated over columns A:D and the cells it was for; Excel's task panes sit beside the grid.
        # Its entries were labeled with the copied text's line break (alpha\r), so nothing found them by their text
        self.ws.Range("A1:A2").Value = [["alpha"], ["beta"]]
        yc.ui.command("ClipboardPane")
        self.keys("w:6|g:A1|w:2|k:c-C|w:4|g:A2|w:2|k:c-C|w:4|k:Escape|w:3")
        wins = {w["name"].split("###")[-1]: w for w in yc.ui.windows()}
        pane, grid = wins.get("yc_clipboard"), wins.get("yc_gridhost")
        self.assertTrue(pane and pane["docked"] and not pane.get("floating"), pane)
        self.assertLessEqual(pane["rect"][2], grid["rect"][0] + 4, "the pane is beside the sheets, not over them")
        self.keys("g:C5|w:2")
        self.click(self.item("##clipitem", "Clipboard")["rect"]) if False else None
        hits = [i for i in yc.ui.items("") if i["label"] == "alpha" and i["window"] == "Clipboard"]
        self.assertTrue(hits, "the entry is found by its text")
        self.click(hits[0]["rect"])
        self.assertEqual(self.ws.Range("C5").Value, "alpha")
        yc.ui.command("ClipboardPane")
        self.keys("w:4")

    def test_the_sort_dialog_selects_what_it_sorts(self):
        # Excel selects the cells the Sort dialog sorts behind it (the header row out while My data has headers is on);
        # the sheet went on showing the one cell, and its combo's column names were missing from ui.items
        self.ws.Range("A1:C4").Value = [["Sym", "Side", "Qty"], ["B", "Buy", 3], ["A", "Sell", 5], ["C", "Buy", 1]]
        self.keys("g:B3|w:2|x:CustomSort|w:8")
        self.assertEqual(yc.excel.Application.Selection.Address, "$A$2:$C$4")
        self.assertEqual(yc.ui.state()["active_cell"], "B3")
        self.click(self.item("My data has headers")["rect"])
        self.keys("w:4")
        self.assertEqual(yc.excel.Application.Selection.Address, "$A$1:$C$4")
        self.click(self.item("My data has headers")["rect"])
        self.click(self.item("##col0")["rect"])
        self.keys("w:4")
        self.assertTrue([i for i in yc.ui.items("") if i["label"] == "Qty"], "the column names in the drop-down are listed")
        self.keys("k:Escape|w:3|k:Escape|w:4")
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_cancelling_the_sort_dialog_puts_the_selection_back(self):
        # Excel 16 puts back the cell selected before the Sort dialog when it is cancelled (measured); the sheet kept
        # the cells the dialog had shown, and the next key or format acted on the whole list
        self.ws.Range("A1:C4").Value = [["Sym", "Side", "Qty"], ["B", "Buy", 3], ["A", "Sell", 5], ["C", "Buy", 1]]
        sel = lambda: yc.excel.Application.Selection.Address
        self.keys("g:B3|w:2|x:CustomSort|w:8")
        self.assertEqual(sel(), "$A$2:$C$4")
        self.keys("k:Escape|w:4")
        self.assertEqual(sel(), "$B$3")
        self.keys("x:CustomSort|w:8")
        self.click(self.item("Cancel")["rect"])
        self.keys("w:4")
        self.assertEqual(sel(), "$B$3")
        self.keys("x:CustomSort|w:8")  # Options... and back is still the dialog
        self.click(self.item("Options...")["rect"])
        self.keys("w:4|k:Enter|w:4")
        self.assertEqual(sel(), "$A$2:$C$4")
        self.keys("k:Enter|w:6")  # OK keeps the sorted cells selected
        self.assertEqual(sel(), "$A$2:$C$4")
        self.assertEqual([r[0] for r in self.ws.Range("B2:B4").Value], ["Buy", "Buy", "Sell"])
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_sort_and_remove_duplicates_take_excels_letters(self):
        # Excel 16's letters, read through UI Automation's AccessKey: Sort's Add Level A, Delete Level D, My data has
        # headers H; Remove Duplicates' Select All A, Unselect All U, My data has headers M. None of them worked
        self.ws.Range("A1:C4").Value = [["Sym", "Side", "Qty"], ["B", "Buy", 3], ["A", "Sell", 5], ["B", "Buy", 3]]
        checked = lambda label: [i.get("checked") for i in yc.ui.items("") if i["label"] == label]
        self.keys("g:B3|w:2|x:CustomSort|w:8|k:A|w:4")
        self.assertTrue([i for i in yc.ui.items("") if i["id"] == "##col1"], "A adds a level")
        self.keys("k:D|w:4")
        self.assertFalse([i for i in yc.ui.items("") if i["id"] == "##col1"], "D deletes it")
        self.keys("k:a-H|w:4")
        self.assertEqual(checked("My data has headers"), [False])
        self.assertEqual(yc.excel.Application.Selection.Address, "$A$1:$C$4")
        self.keys("k:Escape|w:4|g:B3|w:2|x:RemoveDuplicates|w:8|k:U|w:4")
        self.assertEqual(checked("Sym") + checked("Side") + checked("Qty"), [False, False, False])
        self.keys("k:A|w:4")
        self.assertEqual(checked("Sym") + checked("Side") + checked("Qty"), [True, True, True])
        self.keys("k:M|w:4")  # without headers the columns are named by their letters
        self.assertEqual(checked("My data has headers"), [False])
        self.keys("k:Escape|w:4")
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_table_zoom_subtotal_and_move_or_copy_take_excels_letters(self):
        # Excel 16's letters (UI Automation's AccessKey): Create Table's My table has headers M (Alt+M from its range
        # box), Zoom's 7 for 75%, Subtotal's Summary below data S, Move or Copy's Create a copy C
        checked = lambda label: [i.get("checked") for i in yc.ui.items("") if i["label"] == label]
        self.ws.Range("A1:B3").Value = [["Sym", "Qty"], ["A", 1], ["B", 2]]
        self.keys("g:A1|w:2|k:c-T|w:8|k:a-M|w:4")
        self.assertEqual(checked("My table has headers"), [False])
        self.keys("k:Escape|w:4|x:ZoomDialog|w:8|k:7|w:3")
        self.assertEqual(checked("75%") + checked("100%"), [True, False])  # option buttons tell ui.items which is on
        self.keys("k:Enter|w:6")
        self.assertEqual(yc.excel.ActiveWindow.Zoom, 75)
        yc.excel.ActiveWindow.Zoom = 100
        self.keys("g:A1|w:2|x:Subtotal|w:8")
        before = checked("Summary below data")
        self.keys("k:S|w:4")
        self.assertEqual(checked("Summary below data"), [not before[0]])
        self.keys("k:Escape|w:4")
        n = yc.excel.Worksheets.Count
        self.keys("x:CopySheet|w:8|k:C|w:3|k:Enter|w:6")
        self.assertEqual(yc.excel.Worksheets.Count, n + 1)
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_the_ribbon_shows_no_imgui_ids(self):
        # the Sparkline tab showed "Group##spark" and "Ungroup##spark", its Axis menu "...Sparkline##min" twice
        # without the headings that tell them apart: the ribbon draws its labels itself and never cut the id off
        self.ws.Range("A1:C2").Value = [[1, 3, 2], [3, 1, 2]]
        yc.excel.ActiveSheet.Range("D1:D2").SparklineGroups.Add(1, "A1:C2")
        self.keys("g:D1|w:6")
        tx, ty = (int(v) for v in yc.ui.state()["ribbon"]["tabs"]["Sparkline"])  # its tab shows; Excel doesn't switch
        self.keys(f"m:{tx},{ty}|w:2|c:{tx},{ty}|w:6")
        shown = yc.ui.ribbon_items()
        self.assertIn("Group", [i["label"] for i in shown if i["cmd"] == "SparkGroup"])
        axis = [i for i in shown if i["cmd"] == "SparkAxisMenu"][0]["rect"]
        x, y = int((axis[0] + axis[2]) / 2), int((axis[1] + axis[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:6")
        shown = yc.ui.ribbon_items()
        self.assertIn("Automatic for Each Sparkline", [i["label"] for i in shown if i["cmd"] == "SparkMinAuto"])
        self.assertEqual([i["label"] for i in shown if "##" in i["label"]], [])
        self.keys("k:Escape|w:3|k:Escape|w:3")
        for name, (tx, ty) in yc.ui.state()["ribbon"]["tabs"].items():  # every tab's buttons
            if name == "File":
                continue
            self.keys(f"m:{int(tx)},{int(ty)}|w:2|c:{int(tx)},{int(ty)}|w:4")
            self.assertEqual([i["label"] for i in yc.ui.ribbon_items() if "##" in i["label"]], [], name)
        self.keys("k:Escape|w:3")
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_name_manager_keeps_close_in_view_and_answers_its_question_by_keys(self):
        # a row selected (Refers to) and Delete's question pushed Close, Yes and No under the dialog's edge (it
        # scrolled), and Enter didn't answer the question; Escape answers No and leaves the Name Manager open, as
        # Excel's question box does
        wb = yc.excel.ActiveWorkbook
        wb.Names.Add("PxA", "=" + self.ws.Name + "!$A$1:$A$3")
        wb.Names.Add("PxB", "=" + self.ws.Name + "!$B$1")
        self.keys("g:C1|w:2|k:c-F3|w:8")
        self.click(self.item("PxA")["rect"])
        self.click(self.item("Delete")["rect"])
        self.keys("w:4")
        self.assertIn("Yes", [i["label"] for i in yc.ui.items("")])
        self.assertEqual(yc.ui.state()["clipped_windows"], [])
        self.keys("k:Escape|w:4")  # No: the name stays, the Name Manager too
        self.assertIn("Close", [i["label"] for i in yc.ui.items("")])
        self.assertIn("PxA", [n.Name for n in wb.Names])
        self.click(self.item("Delete")["rect"])
        self.keys("w:4|k:Enter|w:6")  # Yes
        self.assertNotIn("PxA", [n.Name for n in wb.Names])
        self.keys("k:Escape|w:4")
        wb.Names("PxB").Delete()
        # Insert Function: a function chosen showed its syntax and description and pushed OK and Cancel out of view
        self.keys("g:C2|w:2|k:s-F3|w:8")
        self.click(self.item("SUM")["rect"])
        self.keys("w:4")
        self.assertIn("OK", [i["label"] for i in yc.ui.items("")])
        self.assertNotIn("Insert Function", yc.ui.state()["clipped_windows"])
        self.keys("k:Escape|w:3|k:Escape|w:3")
        # New Name: an invalid name's error, shown after OK, pushed OK and Cancel under the dialog's edge (chaos 7873)
        self.keys("g:C2|w:2|x:DefineName|w:8|k:c-A|t:1bad name|k:Enter|w:6")
        self.assertIn("OK", [i["label"] for i in yc.ui.items("")])
        self.assertNotIn("New Name", " ".join(yc.ui.state()["clipped_windows"]))
        self.keys("k:Escape|w:3|k:Escape|w:3")
        # Function Arguments: the help of range_lookup (two lines) cut OK and Cancel under the dialog's edge
        self.keys('g:C2|w:2|t:=VLOOKUP(|w:3|k:c-A|w:8|t:"y"|k:Tab|w:2|t:A1:B3|k:Tab|w:2|t:2|k:Tab|w:2|t:FALSE|w:6')
        self.assertNotIn("Function Arguments", yc.ui.state()["clipped_windows"])
        self.keys("k:Escape|w:3|k:Escape|w:3")
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_shift_f10_on_whole_rows_or_columns_opens_their_menu(self):
        # Shift+F10 with whole rows selected opened the cell's menu (no Row Height, Hide, Unhide); Excel 16 opens the
        # row menu, and the column menu for whole columns (read through UI Automation), each with its letters
        self.ws.Range("A1:A5").Value = [[1], [2], [3], [4], [5]]
        self.keys("g:A3|w:2|k:s-Space|w:2|k:s-F10|w:6|k:R|w:6")
        self.assertIn("Row Height", [i["label"] for i in yc.ui.items("")])
        self.keys("k:c-A|t:30|k:Enter|w:6")
        self.assertEqual(self.ws.Rows(3).RowHeight, 30)
        self.keys("g:A3|w:2|k:s-Space|w:2|k:s-F10|w:6|k:H|w:6")
        self.assertTrue(self.ws.Rows(3).Hidden)
        self.keys("g:A2:A4|w:2|k:s-Space|w:2|k:s-F10|w:6|k:U|w:6")
        self.assertFalse(self.ws.Rows(3).Hidden)
        self.keys("g:B1|w:2|k:c-Space|w:2|k:s-F10|w:6|k:W|w:6|k:c-A|t:20|k:Enter|w:6")
        self.assertEqual(self.ws.Columns(2).ColumnWidth, 20)
        self.keys("g:B2|w:2|k:s-F10|w:6")  # one cell: the cell's menu, its Pick From Drop-down List
        self.assertIn("Pick From Drop-down List...", [i["label"] for i in yc.ui.items("")])
        self.keys("k:Escape|w:3")
        self.ws.Rows(3).RowHeight = 15
        self.ws.Columns(2).ColumnWidth = 8.43
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_the_filter_submenus_take_excels_letters(self):
        # F (Number / Text / Date Filters) did nothing, nor the submenus' letters (Excel 16's AccessKey, read through
        # UI Automation): Alt+Down, F, T is Top 10, F, A Above Average; in Text Filters A is Contains
        self.ws.Range("A1:B13").Value = [["Sym", "Vol"]] + [[f"S{i}", i * 10] for i in range(1, 13)]
        self.ws.Range("A1:B13").AutoFilter()
        hidden = lambda: [r for r in range(2, 14) if self.ws.Rows(r).Hidden]
        self.keys("g:B1|w:2|k:a-DownArrow|w:6|k:F|w:4|k:T|w:6|k:Enter|w:6")
        self.assertEqual(hidden(), [2, 3])
        self.keys("g:B1|w:2|k:a-DownArrow|w:6|k:F|w:4|k:A|w:6")
        self.assertEqual(hidden(), [2, 3, 4, 5, 6, 7])
        self.keys("g:A1|w:2|k:a-DownArrow|w:6|k:F|w:4|k:A|w:6")  # Contains...: the Custom AutoFilter dialog
        self.assertIn("Custom AutoFilter", [i["label"] for i in yc.ui.items("")])
        self.keys("t:S1|k:Enter|w:6")
        self.assertEqual(hidden(), [2, 3, 4, 5, 6, 7, 8, 9, 10])  # S1, S10, S11, S12 (with Vol above average)
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_the_filter_menu_takes_excels_letters(self):
        # Alt+Down, E, a few letters, Enter: Excel's letters in the filter menu (S, O sort, C clears, E searches) did
        # nothing, and what was typed went nowhere
        self.ws.Range("A1:B6").Value = [["Sym", "Qty"], ["A2330", 3], ["A2317", 5], ["B2454", 1], ["T", 4], ["C2603", 7]]
        self.ws.Range("A1:B6").AutoFilter()
        hidden = lambda: [r for r in range(2, 7) if self.ws.Rows(r).Hidden]
        self.keys("g:A1|w:2|k:a-DownArrow|w:6|k:E|w:3|t:23|w:3|k:Enter|w:6")
        self.assertEqual(hidden(), [4, 5, 6])
        self.keys("g:A1|w:2|k:a-DownArrow|w:6|k:C|w:6")
        self.assertEqual(hidden(), [])
        self.keys("g:A1|w:2|k:a-DownArrow|w:6|k:O|w:6")
        self.assertEqual([r[0] for r in self.ws.Range("A2:A6").Value], ["T", "C2603", "B2454", "A2330", "A2317"])
        self.keys("g:A1|w:2|k:a-DownArrow|w:6|k:S|w:6")
        self.assertEqual([r[0] for r in self.ws.Range("A2:A6").Value], ["A2317", "A2330", "B2454", "C2603", "T"])
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_tab_goes_along_a_tables_rows_and_adds_one_at_its_end(self):
        # Excel: Tab wraps from a table's last column to the next row's first, and its last cell adds a row; Tab left
        # the table to the right, where the next typing grew it a column at a time
        self.ws.Range("A1:C4").Value = [["Sym", "Qty", "Px"], ["2330", 3, 600], ["2317", 5, 105], ["0050", 2, 150]]
        self.ws.ListObjects.Add(1, self.ws.Range("A1:C4"), None, 1)
        active = lambda: yc.excel.ActiveCell.Address.replace("$", "")
        self.keys("g:C2|w:2|k:Tab|w:3")
        self.assertEqual(active(), "A3")
        self.keys("k:s-Tab|w:3")
        self.assertEqual(active(), "C2", "Shift+Tab from the first column goes back to the row before's last")
        self.keys("g:C4|w:2|k:Tab|w:6")
        self.assertEqual((active(), self.ws.ListObjects(1).Range.Address), ("A5", "$A$1:$C$5"))
        self.keys("t:1101|k:Tab|w:2|t:1|k:Tab|w:2|t:50|k:Tab|w:6")
        self.assertEqual([self.ws.Range(a).Value for a in ("A5", "B5", "C5")], [1101, 1, 50])
        self.assertEqual((active(), self.ws.ListObjects(1).Range.Address), ("A6", "$A$1:$C$6"), "typed with Tab: the next row")
        self.keys("k:Escape|w:2|k:c-Z|w:6")
        self.assertEqual(self.ws.ListObjects(1).Range.Address, "$A$1:$C$5", "Undo takes the added row back")

    def test_tab_after_an_entry_on_a_protected_sheet_goes_to_the_next_unlocked_cell(self):
        # Tab on the sheet went to the next unlocked cell, Tab ending an entry to the locked cell beside it
        self.ws.Range("B2,E2,B3").Locked = False
        self.ws.Protect()
        try:
            self.keys("g:B2|w:2|t:5|k:Tab|w:4")
            self.assertEqual((self.ws.Range("B2").Value, yc.excel.ActiveCell.Address), (5, "$E$2"))
            self.keys("t:6|k:Tab|w:4")
            self.assertEqual(yc.excel.ActiveCell.Address, "$B$3", "on to the next row's unlocked cell")
            self.keys("t:7|k:s-Tab|w:4")
            self.assertEqual(yc.excel.ActiveCell.Address, "$E$2", "Shift+Tab back")
            # typing on a locked cell: the refusal is in the Error Log, marked as one (tools tell it from a fault)
            self.keys("g:A1|w:2|t:9|w:4")
            last = yc.ui.errors()[-1]
            self.assertTrue(last.get("refusal") and "protected sheet" in last["text"], last)
        finally:
            self.keys("k:Escape|w:2")
            self.ws.Unprotect()

    def test_insert_delete_and_text_to_columns_take_excels_letters(self):
        # Ctrl+-, R, Enter (delete the row) did what Enter alone does: the dialogs took none of their letters
        self.ws.Range("A1:B4").Value = [[1, 10], [2, 20], [3, 30], [4, 40]]
        self.keys("g:A2|w:2|k:c-Minus|w:6|k:R|w:3|k:Enter|w:6")
        self.assertEqual([self.ws.Range(a).Value for a in ("A2", "B2")], [3, 30], "Entire row")
        self.keys("g:B1|w:2|k:c-s-Equal|w:6|k:C|w:3|k:Enter|w:6")
        self.assertEqual([self.ws.Range(a).Value for a in ("B1", "C1")], [None, 10], "Entire column")
        self.ws.Range("E1").Value = "2330,TSMC"
        self.keys("g:E1|w:2|m:2,2|k:LeftAlt|w:3|t:A|w:3|t:E|w:8|k:Enter|w:6|k:a-C|w:3|k:Enter|w:6|k:Enter|w:8")
        self.assertEqual([self.ws.Range(a).Value for a in ("E1", "F1")], [2330, "TSMC"], "Alt+C ticks Comma")
        # F5, Alt+S, K, Enter, then = Up Ctrl+Enter: each blank takes the value above (the letters did nothing, and the
        # formula went into every cell of the range)
        self.ws.Range("H1:H5").Value = [["Semi"], [None], [None], ["Fin"], [None]]
        self.ws.Range("I1:I5").Value = [[1], [2], [3], [4], [5]]
        self.keys("g:H1:I5|w:2|k:F5|w:6|k:a-S|w:6|k:K|w:3|k:Enter|w:6")
        self.assertEqual(set(yc.excel.Selection.Address.replace("$", "").split(",")), {"H2:H3", "H5"})
        self.keys("t:=|k:UpArrow|w:2|k:c-Enter|w:6")
        self.assertEqual([self.ws.Range(f"H{r}").Value for r in range(1, 6)], ["Semi", "Semi", "Semi", "Fin", "Fin"])
        self.assertEqual(self.ws.Range("I2").Formula, "2", "the cells with data are left alone")

    def test_find_and_replace_takes_its_alt_letters(self):
        # Ctrl+H, what, Tab, with, Alt+A: Alt+A went to the ribbon's KeyTips (the Data tab) behind the dialog
        self.ws.Range("A1:A3").Value = [["AAPL"], ["MSFT"], ["AAPL"]]
        self.keys("g:A1|w:2|k:c-H|w:6|k:c-A|t:AAPL|k:Tab|w:2|k:c-A|t:Apple|w:2|k:a-A|w:8")
        try:
            self.assertEqual([self.ws.Range(f"A{r}").Value for r in (1, 2, 3)], ["Apple", "MSFT", "Apple"])
            self.assertEqual(yc.ui.state()["ribbon"]["keytips"], 0, "no KeyTips behind the dialog")
            self.keys("k:s-Tab|w:2|k:c-A|t:MSFT|w:2|k:a-I|w:6")
            self.assertTrue([i for i in yc.ui.items("Find and Replace") if i["label"] == self.ws.Name + "!$A$2"], "Alt+I: Find All")
        finally:
            self.keys("k:Escape|w:3|k:Escape|w:3")

    def test_alt_in_a_dialog_that_isnt_modal_stays_with_the_dialog(self):
        # Symbol, Select Data Source, Error Checking...: Alt there lit the ribbon's KeyTips behind the dialog
        self.keys("g:B2|w:2")
        yc.ui.command("InsertSymbol")
        self.keys("w:6")
        try:
            title = [w for w in yc.ui.windows() if w.get("name", "").startswith("Symbol")] if hasattr(yc.ui, "windows") else []
            items = [i for i in yc.ui.items("Symbol") if i.get("rect")]
            self.assertTrue(items, "the Symbol dialog is open")
            r = items[0]["rect"]
            x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
            self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3|k:LeftAlt|w:4")
            self.assertEqual(yc.ui.state()["ribbon"]["keytips"], 0, title)
        finally:
            self.keys("k:Escape|w:3|k:Escape|w:3")

    def test_a_dialogs_combo_boxes_are_found_by_their_ids(self):
        # ImGui's combo boxes told the item recorder no label: Data Validation's Allow was a nameless control
        def click(item):
            r = item["rect"]
            x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
            self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:5")

        self.keys("g:C2:C4|w:2|m:2,2|k:LeftAlt|w:3|t:A|w:3|t:V|w:4|t:V|w:8")
        try:
            allow = [i for i in yc.ui.items("Data Validation") if i["id"] == "##type"]
            self.assertTrue(allow, "Allow is listed by its id")
            click(allow[0])
            click([i for i in yc.ui.items("") if i["label"] == "List"][0])
            source = [i for i in yc.ui.items("Data Validation") if i["id"] == "##rb.dv.source"]
            self.assertTrue(source, "the Source box is listed by its key")
            click(source[0])
            self.keys("t:Buy,Sell|w:2|k:Enter|w:6")
        finally:
            self.keys("k:Escape|w:3")
        self.assertEqual((self.ws.Range("C3").Validation.Type, self.ws.Range("C3").Validation.Formula1), (3, "Buy,Sell"))

    def test_select_all_shows_mixed_when_some_values_are_ticked(self):
        # (Select All) read empty while 2026's dates were ticked, where Excel shows its filled square
        self.ws.Range("A1").Value = "Date"
        self.ws.Range("A2:A4").Formula = [["=DATE(2025,12,30)"], ["=DATE(2026,1,2)"], ["=DATE(2026,2,3)"]]
        self.ws.Range("A2:A4").NumberFormat = "yyyy/m/d"
        self.ws.Range("A1:A4").AutoFilter()

        def box(label):
            found = [i for i in yc.ui.items("") if i.get("label") == label]
            self.assertTrue(found, label)
            return found[0]

        self.keys("g:A1|w:2|k:a-DownArrow|w:6")
        try:
            self.assertIs(box("(Select All)").get("checked"), True)
            r = box("2025")["rect"]
            x, y = int(r[0] + 8), int((r[1] + r[3]) / 2)
            self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
            self.assertIs(box("2025").get("checked"), False)
            self.assertEqual(box("(Select All)").get("checked"), "mixed")
            r = box("(Select All)")["rect"]
            x, y = int(r[0] + 8), int((r[1] + r[3]) / 2)
            self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
            self.assertIs(box("2025").get("checked"), True, "a click on the mixed box ticks every value")
        finally:
            self.keys("k:Escape|w:3")

    def test_menus_over_the_cells_keep_the_ui_font_when_zoomed(self):
        # the sheet draws its overlays in the cells' font, zoomed: the filter menu and the Paste Options menu grew with
        # the zoom (headings 30 pixels high at 200%), where Excel's menus keep their size
        self.ws.Range("A1:A3").Value = [["Sym"], ["X"], ["Y"]]
        self.ws.Range("A1:A3").AutoFilter()
        win = yc.excel.ActiveWindow

        def heights():
            self.keys("g:A1|w:2|k:a-DownArrow|w:6")
            sort = [i for i in yc.ui.items("") if i.get("label") == "Sort A to Z"]
            self.keys("k:Escape|w:3|g:A2:A3|w:2|k:c-C|w:3|g:C2|w:2|k:c-V|w:6|k:LeftCtrl|w:6")
            items = yc.ui.items("")
            first = [i for i in items if i.get("label") == "Paste"]
            last = [i for i in items if i.get("label") == "Paste Link"]
            self.keys("k:Escape|w:3|k:Escape|w:3")
            self.assertTrue(sort and first and last, "both menus open")
            # the gallery's first and last rows have two headings between them
            return sort[0]["rect"][3] - sort[0]["rect"][1], last[0]["rect"][1] - first[-1]["rect"][1]

        try:
            at100 = heights()
            win.Zoom = 200
            self.keys("w:4")
            at200 = heights()
        finally:
            win.Zoom = 100
        self.assertAlmostEqual(at200[0], at100[0], delta=2, msg="the filter menu's entries")
        self.assertAlmostEqual(at200[1], at100[1], delta=2, msg="the Paste Options menu's headings")

    def test_the_cell_menu_takes_excels_letters(self):
        # Shift+F10 (or the Menu key), then V pastes values: the menus ignored every letter
        self.ws.Range("A1").Formula = "=2+3"
        self.keys("g:A1|w:2|k:c-C|w:3|g:C1|w:2|k:s-F10|w:6|k:V|w:6")
        self.assertEqual((self.ws.Range("C1").Formula, self.ws.Range("C1").Value), ("5", 5))
        self.keys("k:Escape|w:3|g:C1|w:2|k:s-F10|w:6|k:D|w:6")
        self.assertNotEqual(yc.ui.state()["dialog"], 0, "D opens Delete...")
        self.keys("k:Escape|w:4")
        self.assertEqual(yc.ui.state()["dialog"], 0)
        self.ws.Range("E1").Value = "x"
        x0, y0, x1, y1 = yc.ui.cell_rect("E1")
        hx, hy = int((x0 + x1) / 2), int(y0) - 10
        self.keys(f"m:{hx},{hy}|w:2|c:{hx},{hy}|w:3|r:{hx},{hy}|w:6|k:H|w:6")
        self.assertTrue(self.ws.Columns("E").Hidden, "H in a column's menu hides it")
        self.ws.Columns("E").Hidden = False
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_custom_autofilter_takes_the_value_typed_at_once(self):
        self.ws.Range("A1:A5").Value = [["Name"], ["Apple"], ["Banana"], ["Avocado"], ["Cherry"]]
        self.ws.Range("A1:A5").AutoFilter()
        self.keys("g:A1|w:2|k:a-Down|w:8")
        for label in ("Text Filters", "Begins With..."):
            hits = sorted([i for i in yc.ui.items("") if i["label"] == label], key=lambda i: (i["rect"][1], i["rect"][0]))
            self.assertTrue(hits, label + " is shown")
            self.click(hits[0]["rect"])
            self.keys("w:4")
        self.assertNotEqual(yc.ui.state()["dialog"], 0, "Custom AutoFilter opened")
        self.keys("t:A|k:Enter|w:6")
        self.assertEqual([self.ws.Rows(r).Hidden for r in range(2, 6)], [False, True, False, True])

    def test_function_arguments_take_cells_picked_with_the_range_button(self):
        self.ws.Range("B2:B5").Value = [[1], [2], [3], [4]]
        self.keys("g:C7|w:2|k:s-F3|w:8|t:SUM|w:3|k:Enter|w:8")
        rb = self.item("##rb", "Function Arguments")
        bx, by = int(rb["rect"][2] + 20), int((rb["rect"][1] + rb["rect"][3]) / 2)
        self.keys(f"m:{bx},{by}|w:3|c:{bx},{by}|w:6")
        a, b = yc.ui.cell_rect("B2"), yc.ui.cell_rect("B5")
        x0, y0, x1, y1 = int((a[0] + a[2]) / 2), int((a[1] + a[3]) / 2), int((b[0] + b[2]) / 2), int((b[1] + b[3]) / 2)
        self.keys(f"m:{x0},{y0}|w:4|d:{x0},{y0},{x1},{y1}|w:8|k:Enter|w:6")
        self.assertTrue([w for w in yc.ui.state()["windows"] if w["name"].startswith("Function Arguments")], "the dialog came back")
        self.keys("k:Enter|w:8")
        self.assertEqual(self.ws.Range("C7").Formula, "=SUM(B2:B5)")
        self.assertEqual(self.ws.Range("C7").Value2, 10)
        self.assertEqual(yc.ui.errors()[self.errors0:], [])

    def test_sorting_a_lone_empty_cell_says_so(self):
        self.ws.Range("A1:B3").Value = [["Name", "Qty"], ["b", 2], ["a", 1]]
        self.keys("g:F9|w:2|x:CustomSort|w:6")
        self.assertEqual(yc.ui.state()["dialog"], 0)
        self.assertTrue(any("can't be applied to the selected range" in e["text"] for e in yc.ui.errors()[self.errors0:]))


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class MergedAndVisibleRound8Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()

    def test_a_write_to_a_merged_range_fills_its_first_cell_only(self):
        self.ws.Range("A1:B2").Merge()
        self.ws.Range("A1:B2").Value2 = 7
        self.ws.Range("D1").Formula = "=SUM(A1:B2)"
        self.assertEqual(self.ws.Range("D1").Value2, 7)  # was 28: every hidden cell held 7
        self.ws.Range("B2").Value2 = 9  # a cell under the merge takes nothing (Excel)
        self.assertEqual(self.ws.Range("D1").Value2, 7)
        self.ws.Range("A1:B2").Formula = "=1+1"
        self.assertEqual(self.ws.Range("D1").Value2, 2)
        self.ws.Range("A1:B2").UnMerge()
        self.assertIsNone(self.ws.Range("B2").Value2)
        self.assertIsNone(self.ws.Range("B1").Value2)

    def test_visible_cells_of_one_cell_and_of_empty_columns(self):
        for i in range(1, 5):
            self.ws.Range(f"A{i}").Value2 = i
        self.ws.Rows("2:3").Hidden = True
        # one cell: the visible cells of the whole sheet once rows are hidden (Excel 16, measured; $A$1 with none)
        self.assertEqual(self.ws.Range("A1").SpecialCells(12).Address, "$1:$1,$4:$1048576")
        self.assertEqual(self.ws.Range("Z99").SpecialCells(12).Address, "$1:$1,$4:$1048576")
        self.assertEqual(self.ws.Range("A1:B4").SpecialCells(12).Address, "$A$1:$B$1,$A$4:$B$4")
        self.assertEqual(self.ws.Range("C1:C4").SpecialCells(12).Address, "$C$1,$C$4")
        self.ws.Rows("2:3").Hidden = False
        self.assertEqual(self.ws.Range("A1").SpecialCells(12).Address, "$A$1")


if __name__ == "__main__":
    unittest.main()
