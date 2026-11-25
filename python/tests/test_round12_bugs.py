# SPDX-License-Identifier: Apache-2.0
"""Round 12: bugs found at random across the object model, the keys and the file, each with what Excel 16 does
(measured).

Run: python -m unittest tests.test_round12_bugs (with the bundled Python).
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
class Round12Test(unittest.TestCase):
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

        excel.Application.CutCopyMode = False
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()

    def keys(self, script):
        self.conn.request("ui.input", {"script": "m:2,2|w:2|" + script + "|w:4", "timeout_ms": 15000}, timeout=30)
        time.sleep(0.2)

    def test_a_whole_table_cut_and_pasted_goes_with_its_cells(self):
        ws = self.ws
        ws.Range("A3:B5").Value = [["k", "v"], ["a", 1], ["b", 2]]
        lo = ws.ListObjects.Add(1, ws.Range("A3:B5"), XlListObjectHasHeaders=1)
        ws.Range("H1").Formula = f"=SUM({lo.Name}[v])"
        ws.Range("A3:B5").Cut(ws.Range("D10"))  # Excel 16: the table goes to D10:E12, its name and formulas with it
        self.assertEqual((ws.ListObjects.Count, ws.ListObjects(1).Range.Address), (1, "$D$10:$E$12"))
        self.assertEqual((ws.Range("H1").Formula, ws.Range("H1").Value2), (f"=SUM({lo.Name}[v])", 3))
        self.keys("g:D10:E12|k:c-X|w:2|g:A20|k:c-V")  # and with the keys
        self.assertEqual(ws.ListObjects(1).Range.Address, "$A$20:$B$22")
        self.assertEqual(ws.Range("H1").Value2, 3)
        ws.Range("A21:B21").Cut(ws.Range("K1"))  # part of it: the cells go, the table stays
        self.assertEqual(ws.ListObjects(1).Range.Address, "$A$20:$B$22")
        self.assertEqual(ws.Range("H1").Value2, 2)

    def test_sort_order_custom_is_one_more_than_the_list_number(self):
        from yc import excel

        app = excel.Application
        app.AddCustomList(["Low", "Mid", "High"])
        n = app.CustomListCount
        try:
            ws = self.ws
            ws.Range("D1:D4").Value = [["High"], ["Low"], ["Mid"], ["High"]]
            ws.Range("D1:D4").Sort(Key1=ws.Range("D1"), Order1=1, Header=2, OrderCustom=n + 1)  # Excel 16, measured
            self.assertEqual([ws.Range(f"D{i}").Value2 for i in range(1, 5)], ["Low", "Mid", "High", "High"])
            ws.Range("E1:E3").Value = [["Tue"], ["Sun"], ["Mon"]]
            ws.Range("E1:E3").Sort(Key1=ws.Range("E1"), Order1=1, Header=2, OrderCustom=2)  # list 1: Sun, Mon...
            self.assertEqual([ws.Range(f"E{i}").Value2 for i in range(1, 4)], ["Sun", "Mon", "Tue"])
            ws.Range("E1:E3").Sort(Key1=ws.Range("E1"), Order1=1, Header=2, OrderCustom=1)  # the normal order
            self.assertEqual([ws.Range(f"E{i}").Value2 for i in range(1, 4)], ["Mon", "Sun", "Tue"])
        finally:
            app.DeleteCustomList(n)

    def test_note_text_reads_and_writes_from_start_for_length(self):
        ws = self.ws
        ws.Range("B2").AddComment("second")
        r = ws.Range("B2")
        # Excel 16, measured
        self.assertEqual((r.NoteText(Start=2, Length=3), r.NoteText(Start=3), r.NoteText()), ("eco", "cond", "second"))
        r.NoteText("XY", 3, 2)
        self.assertEqual(r.Comment.Text(), "seXYnd")
        r.NoteText("Z", 1)
        self.assertEqual(r.Comment.Text(), "Z")

    def test_create_names_from_labels_that_read_as_references_or_numbers(self):
        from yc import excel

        ws = self.ws
        ws.Range("D10:E15").Value = [["Row1", 5], ["Row 2", 6], ["1st", 7], ["a-b", 8], ["C", 9], [12, 9]]
        ws.Range("D10:E15").CreateNames(Left=True)
        mine = sorted(n.Name for n in excel.ActiveWorkbook.Names if n.RefersTo.startswith(f"={ws.Name}!") or n.RefersTo.startswith(f"='{ws.Name}'!"))
        self.assertEqual(mine, sorted(["Row1_", "Row_2", "_1st", "a_b", "C_"]))  # Excel 16, measured; 12 gives none

    def test_the_dependents_of_an_array_formulas_input_are_all_its_cells(self):
        ws = self.ws
        ws.Range("B2:C2").Value = [[1, 2]]
        ws.Range("B3").Value = 3
        ws.Range("G1:G2").FormulaArray = "=B2:B3*2"
        ws.Range("H1").Formula = "=B2+C2"
        ws.Range("H2").Formula = "=H1*2"
        def cells(rng):
            return sorted(c.Address for a in rng.Areas for c in a.Cells)

        # Excel 16, measured: $G$1:$H$2 and $G$1:$G$2,$H$1 (G2 was left out)
        self.assertEqual(cells(ws.Range("B2").Dependents), ["$G$1", "$G$2", "$H$1", "$H$2"])
        self.assertEqual(cells(ws.Range("B2").DirectDependents), ["$G$1", "$G$2", "$H$1"])

    def test_a_second_tables_filter_keeps_its_hidden_rows_from_delete_ctrl_d_and_python(self):
        ws = self.ws

        def make():
            ws.Cells.Clear()
            for lo in list(ws.ListObjects):
                lo.Delete()
            ws.Range("J1:K3").Value = [["p", "q"], [1, 2], [3, 4]]
            ws.ListObjects.Add(1, ws.Range("J1:K3"), XlListObjectHasHeaders=1)  # the first table: the sheet's buttons
            ws.Range("A1:C6").Value = [["k", "v", "n"], ["a", 1, "x"], ["b", 2, "y"], ["a", 3, "z"], ["b", 4, "w"], ["a", 5, "q"]]
            ws.ListObjects.Add(1, ws.Range("A1:C6"), XlListObjectHasHeaders=1).Range.AutoFilter(Field=1, Criteria1="a")

        def col_c():
            return [ws.Range(f"C{r}").Value2 for r in range(2, 7)]

        make()
        self.keys("g:C2:C6|k:Delete")  # it wiped y and w, which the filter hides
        self.assertEqual(col_c(), [None, "y", None, "w", None])
        make()
        ws.Range("C2").Value = "top"
        self.keys("g:C2:C6|k:c-D")
        self.assertEqual(col_c(), ["top", "y", "top", "w", "top"])
        make()
        ws.Range("C2:C6").Value = "Z"
        self.assertEqual(col_c(), ["Z", "y", "Z", "w", "Z"])

    def test_outline_symbols_and_show_formulas_are_saved_with_the_workbook(self):
        import tempfile

        from yc import excel

        ws = self.ws
        ws.Name = "View12"
        ws.Range("A1").Formula = "=1+1"
        win = excel.ActiveWindow
        win.DisplayOutline = False  # Ctrl+8
        win.DisplayFormulas = True  # Ctrl+`
        path = os.path.join(tempfile.mkdtemp(prefix="yc_r12_"), "view.ycg")
        try:
            excel.ActiveWorkbook.SaveCopyAs(path)
        finally:
            win.DisplayOutline = True
            win.DisplayFormulas = False
        other = yc.launch(path, hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=other.pid)
            excel.Worksheets("View12").Activate()
            # Excel keeps both in the sheet's view; the outline symbols came back after a save
            self.assertEqual((excel.ActiveWindow.DisplayOutline, excel.ActiveWindow.DisplayFormulas), (False, True))
        finally:
            other.terminate()
            other.wait(10)
            yc.connect(target=self.proc.pid)

    def test_split_row_splits_a_window_that_isnt_frozen(self):
        from yc import excel

        win = excel.ActiveWindow
        win.FreezePanes = False
        win.Split = False
        try:
            self.ws.Range("C4").Select()
            win.FreezePanes = True
            self.assertEqual((win.SplitRow, win.SplitColumn, win.FreezePanes), (3, 2, True))
            win.SplitRow = 5  # a frozen window stays frozen at the new row
            self.assertEqual((win.SplitRow, win.FreezePanes), (5, True))
            win.FreezePanes = False
            win.SplitRow = 3  # Excel 16, measured: Split True, FreezePanes False
            win.SplitColumn = 2
            self.assertEqual((win.SplitRow, win.SplitColumn, win.FreezePanes, win.Split), (3, 2, False, True))
        finally:
            win.FreezePanes = False
            win.Split = False

    def test_unfreezing_keeps_the_frozen_rows_and_columns_in_view(self):
        from yc import excel, ui

        win = excel.ActiveWindow
        win.FreezePanes = False
        win.Split = False
        win.ScrollRow, win.ScrollColumn = 1, 1
        self.ws.Range("C4").Select()
        win.FreezePanes = True
        self.assertEqual((win.ScrollRow, win.ScrollColumn), (4, 3))
        win.FreezePanes = False  # Excel 16, measured: the window shows A1 again; rows 1:3 had scrolled away
        self.assertEqual((win.ScrollRow, win.ScrollColumn), (1, 1))
        self.ws.Range("C4").Select()
        win.FreezePanes = True
        ui.command("FreezePanes")  # View > Freeze Panes > Unfreeze Panes
        time.sleep(0.3)
        self.assertEqual((win.FreezePanes, win.ScrollRow, win.ScrollColumn), (False, 1, 1))

    def test_a_split_keeps_the_rows_and_columns_above_and_left_of_it(self):
        from yc import excel

        win = excel.ActiveWindow
        win.FreezePanes = False
        win.Split = False
        win.ScrollRow, win.ScrollColumn = 1, 1
        try:
            win.SplitRow = 3
            win.SplitColumn = 2
            # Excel 16, measured: split at rows 1:3 and columns A:B, the window still showing A1; the second setter
            # started the split at the lower pane's first row and rows 1:3 went out of sight
            self.assertEqual((win.Split, win.SplitRow, win.SplitColumn, win.ScrollRow, win.ScrollColumn), (True, 3, 2, 1, 1))
            win.Split = False
            self.assertEqual((win.ScrollRow, win.ScrollColumn), (1, 1))
            win.ScrollRow = 1
            self.assertEqual(win.ScrollRow, 1)
        finally:
            win.Split = False


if __name__ == "__main__":
    unittest.main()
