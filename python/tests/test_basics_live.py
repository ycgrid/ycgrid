# SPDX-License-Identifier: Apache-2.0
"""Basic operations scripts/basics.py found broken in some state, tried the way a person does it (keys one frame apart,
as a fast typist or a busy frame gives them); each test fails without its fix.

Run: python -m unittest tests.test_basics_live (with the bundled Python).
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
class BasicsLiveTest(unittest.TestCase):
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

    def tearDown(self):
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2")

    def keys(self, script, timeout=20):
        return self.conn.request("ui.input", {"script": script + "|w:2", "timeout_ms": int(timeout * 1000)}, timeout=timeout + 20)

    def edit_text(self):
        from yc import ui

        return ui.state().get("edit", {}).get("text")

    def test_an_arrow_right_after_a_typed_comma_points(self):
        # each step is one frame: the arrow came while the caret still stood before the ",", and committed =SUM(F2,)
        self.keys("g:F3|t:=SUM(|k:UpArrow|w:3|t:,|k:UpArrow")
        self.assertEqual(self.edit_text(), "=SUM(F2,F2")
        self.keys("k:Escape")
        self.keys("g:J5|t:=SUM(|k:UpArrow|t:=|k:UpArrow")
        self.assertEqual(self.edit_text(), "=SUM(J4=J4")

    def test_f4_right_after_pointing_cycles_the_reference(self):
        self.keys("g:D4|t:=|k:UpArrow|k:F4")
        self.assertEqual(self.edit_text(), "=$D$3")

    def test_unhiding_every_row_of_whole_columns_doesnt_freeze(self):
        self.ws.Range("A1").Value2 = 1
        t = time.time()
        self.keys("g:B:C|k:c-9|w:3|k:c-s-9|w:3", timeout=30)  # froze YCGrid for minutes
        self.assertLess(time.time() - t, 10)
        self.assertFalse(self.ws.Rows(500000).Hidden)
        self.assertFalse(self.ws.Rows(1).Hidden)

    def test_copying_a_cell_the_filter_hides_copies_it(self):
        from yc import ui

        self.ws.Range("A1:B4").Value = [["Name", "Qty"], ["AAPL", 10], ["MSFT", 20], ["TSMC", 30]]
        self.ws.Range("A1:B4").AutoFilter(1, "<>MSFT")
        self.assertTrue(self.ws.Rows(3).Hidden)
        last = ui.errors()[-1]["id"] if ui.errors() else 0
        self.keys("g:B3|k:c-C|g:D1|k:c-V")  # Excel 16, measured: B3 is copied (it said the selection was too large)
        self.assertEqual(self.ws.Range("D1").Value2, 20)
        self.assertEqual([e["text"] for e in ui.errors(after=last)], [])

    def test_mergecells_true_keeps_the_upper_left_format(self):
        # Excel 16, measured: A5 (0%, bold) takes B5's 3 and keeps its own format; B5 and C5 go back to General
        ws = self.ws
        ws.Range("A5").NumberFormat = "0%"
        ws.Range("A5").Font.Bold = True
        ws.Range("B5").Value2 = 3
        ws.Range("B5").NumberFormat = "0.00"
        ws.Range("C5").Value2 = "x"
        ws.Range("A5:C5").MergeCells = True
        self.assertTrue(ws.Range("A5").MergeCells)
        self.assertEqual(ws.Range("A5").Value2, 3)
        self.assertEqual(ws.Range("A5").NumberFormat, "0%")
        self.assertTrue(ws.Range("A5").Font.Bold)
        self.assertEqual(ws.Range("B5").NumberFormat, "General")
        self.assertIsNone(ws.Range("C5").Value2)

    def test_autosum_proposes_what_excel_proposes(self):
        # Excel 16, measured (the formula Alt+= leaves in the cell editor, read from Excel's formula bar)
        cases = [
            ({"B1": "Qty", "B2": 10, "B3": 20, "B4": 30, "B5": 40, "B6": 50, "B7": 60}, "B9", "=SUM(B2:B8)"),
            ({"B1": "Qty", "B2": 10, "B3": 20, "B4": 30, "B5": 40, "B6": 50, "B7": 60}, "B8", "=SUM(B2:B7)"),
            ({"B1": "Qty", "B2": 10, "B3": 20, "B4": 30, "B5": 40, "B6": 50, "B7": "=SUM(B2:B6)"}, "B9", "=SUM(B7)"),
            ({"B1": "Qty", "B2": 10, "B3": 20, "B4": "=SUM(B2:B3)", "B5": 5, "B6": 6, "B7": "=SUM(B5:B6)"}, "B8", "=SUM(B7,B4)"),
            ({"B1": "Qty", "B2": 10, "B3": "txt", "B4": 30, "B5": 40}, "B6", "=SUM(B4:B5)"),
            ({"A5": "x", "B5": 1, "C5": 2, "D5": 3}, "F5", "=SUM(B5:E5)"),
            ({"B1": "Qty", "B2": 10, "B3": 20}, "C3", "=SUM(B3)"),
            ({"A1": 1, "A2": 2, "B1": 3, "B2": 4}, "C2", "=SUM(A2:B2)"),
            ({"B1": "Qty", "B2": 10}, "B3", "=SUM(B2)"),
            ({"B2": 10, "B4": 20}, "B6", "=SUM(B4:B5)"),
            ({"B1": 2, "B2": 4, "B3": 6, "B4": "=SUM(B1:B3)", "B5": 5, "B6": 6}, "B8", "=SUM(B5:B7)"),
            ({"B1": 1, "B2": 2, "B3": "=SUM(B1:B2)", "B5": 7}, "B6", "=SUM(B5)"),
            ({"A1": 1, "B1": 2, "C1": "=SUM(A1:B1)", "D1": 5}, "E1", "=SUM(D1)"),
        ]
        for cells, target, want in cases:
            with self.subTest(target=target, want=want):
                self.ws.Cells.Clear()
                for a, v in cells.items():
                    if isinstance(v, str) and v.startswith("="):
                        self.ws.Range(a).Formula = v
                    else:
                        self.ws.Range(a).Value2 = v
                self.keys(f"k:Escape|g:{target}|k:a-Equal|w:3")
                self.assertEqual(self.edit_text(), want)  # was =SUM() under a blank row or a total, and B2:B7 for B9
                self.keys("k:Escape")

    def test_undo_of_a_bigger_font_on_an_empty_cell_takes_the_row_height_back(self):
        # the rows a bigger font grew were a step of their own that Undo never took back (basics.py --commands)
        h = self.ws.Rows(2).RowHeight
        self.keys("g:F2|x:FontSizeIncrease|w:6")
        self.assertGreater(self.ws.Rows(2).RowHeight, h)
        self.keys("k:c-Z|w:4")
        self.assertEqual(self.ws.Rows(2).RowHeight, h)
        self.keys("k:c-Y|w:4")
        self.assertGreater(self.ws.Rows(2).RowHeight, h)

    def _keys_reach_the_hidden_cell(self, hide):
        # Excel 16, measured: with every column (or every row) hidden, typing, F2, Delete, Escape and Undo still act on
        # the active cell, and Enter and Tab leave it where it is; no key reached the sheet (the grid returned early)
        from yc import excel

        self.keys(hide + "|w:3")
        self.keys("g:Z50|t:probe|w:3")
        self.assertEqual(self.edit_text(), "probe")  # the entry is live (the formula bar shows it)
        self.keys("k:Enter|w:3")
        self.assertEqual(self.ws.Range("Z50").Value2, "probe")
        self.assertEqual(excel.ActiveCell.Address, "$Z$50")
        self.keys("k:F2|t:x|k:Enter|w:3")
        self.assertEqual(self.ws.Range("Z50").Value2, "probex")
        self.keys("t:pq|k:Tab|w:3")
        self.assertEqual(self.ws.Range("Z50").Value2, "pq")
        self.assertEqual(excel.ActiveCell.Address, "$Z$50")
        self.keys("t:zz|k:Escape|w:3")
        self.assertEqual(self.ws.Range("Z50").Value2, "pq")
        self.keys("k:Delete|w:3")
        self.assertIsNone(self.ws.Range("Z50").Value2)
        self.keys("k:c-Z|w:4")
        self.assertEqual(self.ws.Range("Z50").Value2, "pq")
        self.keys("k:Home|w:3")  # Home goes to column A even so (measured)
        self.assertEqual(excel.ActiveCell.Address, "$A$50")
        self.keys("g:Z50|w:2")
        return excel

    def test_keys_reach_the_sheet_with_every_column_hidden(self):
        excel = self._keys_reach_the_hidden_cell("g:3:4|k:c-0")
        self.assertTrue(self.ws.Columns(1).Hidden and self.ws.Columns(16384).Hidden)
        # the rows are still shown: Down and Up move, Right and Left have nowhere to go (measured)
        self.keys("k:DownArrow|w:2|k:DownArrow|w:2|k:RightArrow|w:2|k:UpArrow|w:2|k:LeftArrow|w:3")
        self.assertEqual(excel.ActiveCell.Address, "$Z$51")
        self.keys("k:c-RightArrow|w:3")
        self.assertEqual(excel.ActiveCell.Address, "$Z$51")

    def test_keys_reach_the_sheet_with_every_row_hidden(self):
        excel = self._keys_reach_the_hidden_cell("g:B:C|k:c-9")
        self.assertTrue(self.ws.Rows(1).Hidden and self.ws.Rows(1048576).Hidden)
        # no row is shown: no arrow, page or Ctrl+arrow key moves the active cell (measured)
        for k in ("DownArrow", "UpArrow", "RightArrow", "LeftArrow", "PageDown", "PageUp", "c-DownArrow", "c-RightArrow"):
            with self.subTest(key=k):
                self.keys(f"k:{k}|w:3")
                self.assertEqual(excel.ActiveCell.Address, "$Z$50")

    def test_a_number_format_excel_refuses_is_refused(self):
        # Excel refuses "0;@;0", "[Blue", "0_"... (Range.NumberFormat raises, Format Cells says so and stays: measured);
        # YCGrid took every code, showed "[Blue" as ####### and '"abc' as -abc
        from yc import excel, ui

        r = self.ws.Range("B2")
        r.Value2 = -1
        r.NumberFormat = "0.00"
        for code in ("0;@;0", "[Blue", '"abc', "0_", "0;0;0;0;0", "[>100][<200]0"):
            with self.assertRaises(yc.YcError, msg=code):
                r.NumberFormat = code
            self.assertEqual(r.NumberFormat, "0.00")
        r.NumberFormat = "[Red]0;0;@"  # a code Excel takes still goes in
        self.assertEqual(r.NumberFormat, "[Red]0;0;@")
        r.NumberFormat = "0.00"
        # the other places a code comes in: a conditional format's and a style's
        fc = r.FormatConditions.Add(1, 5, "=0")
        fc.NumberFormat = "0.0"
        with self.assertRaises(yc.YcError):
            fc.NumberFormat = "0;@;0"
        self.assertEqual(fc.NumberFormat, "0.0")
        r.FormatConditions.Delete()
        st = excel.ActiveWorkbook.Styles.Add("BadCodeTest")
        with self.assertRaises(yc.YcError):
            st.NumberFormat = "[Blue"
        self.assertNotEqual(st.NumberFormat, "[Blue")
        st.Delete()
        # Format Cells > Custom: OK on a refused code shows the message, and the dialog comes back with the code
        self.keys("m:2,2|g:B2|w:2|x:FormatCellsDialog|w:8")
        dlg = ui.state()["dialog"]
        self.assertTrue(dlg)

        def click(label):
            item = [i for i in ui.items() if i["label"] == label]
            self.assertTrue(item, label)
            x0, y0, x1, y1 = item[0]["rect"]
            x, y = int((x0 + x1) / 2), int((y0 + y1) / 2)
            self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

        click("Custom")
        custom = [i for i in ui.items() if i["id"].endswith("##custom")] or [i for i in ui.items() if i["label"] == "##custom"]
        self.assertTrue(custom)
        x0, y0, x1, y1 = custom[0]["rect"]
        last = (ui.errors() or [{"id": 0}])[-1]["id"]
        self.keys(f"c:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:3|k:c-A|t:0;@;0|w:2")
        click("OK")
        shown = [e["text"] for e in ui.errors(after=last)]
        self.assertTrue(any("cannot use the number format" in s for s in shown), shown)
        self.keys("k:Enter|w:8")  # the message's OK
        self.assertEqual(ui.state()["dialog"], dlg, "Format Cells is back")
        self.assertEqual(r.NumberFormat, "0.00")
        self.keys("k:Escape|w:6")
        self.assertEqual(ui.state()["dialog"], 0)
        self.assertEqual(r.NumberFormat, "0.00")

    def test_hiding_the_window_during_an_entry_writes_it_first(self):
        # View > Hide during an entry left its editor open in the hidden window, where no Escape reached it
        from yc import excel, ui

        try:
            self.keys("m:2,2|g:B2|w:2|t:12|w:2|x:HideWindow|w:4")
            self.assertFalse(excel.ActiveWindow.Visible)
            self.assertNotIn("edit", ui.state())
            self.assertEqual(self.ws.Range("B2").Value2, 12)
        finally:
            excel.ActiveWindow.Visible = True
        self.keys("w:4")
        self.assertTrue(excel.ActiveWindow.Visible)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class SecondWindowTest(unittest.TestCase):
    """View > New Window / Side by Side: what the Name Box and Python select is where the keys go (a hidden YCGrid of
    its own: the second window stays open)."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def keys(self, script, timeout=20):
        return self.conn.request("ui.input", {"script": script + "|w:2", "timeout_ms": int(timeout * 1000)}, timeout=timeout + 20)

    def test_the_name_box_and_select_act_on_the_window_the_keys_go_to(self):
        # the Name Box selected Z50 in the first window and "probe" went into the second window's A1 (basics.py
        # --tab others: View Side by Side, then typing into Z50)
        from yc import excel, ui

        ws = excel.ActiveSheet
        ws.Range("A1").Value2 = "keep"
        self.keys("m:2,2|g:B2|w:2|x:ViewSideBySide|w:8|k:Escape|w:2")
        self.assertTrue(ui.state().get("focus_window"), "the second window has the keys")
        self.keys("g:Z50|w:2|t:probe|k:Enter|w:3")
        self.assertEqual(ws.Range("Z50").Value2, "probe")
        self.assertEqual(ws.Range("A1").Value2, "keep")
        self.assertEqual(excel.ActiveCell.Address, "$Z$51")
        # Python's Select and Selection are the active window's, as Excel's
        ws.Range("C3").Select()
        self.assertEqual(excel.Selection.Address, "$C$3")
        self.keys("t:x|k:Enter|w:3")
        self.assertEqual(ws.Range("C3").Value2, "x")
        self.assertEqual(ws.Range("A1").Value2, "keep")


if __name__ == "__main__":
    unittest.main()
