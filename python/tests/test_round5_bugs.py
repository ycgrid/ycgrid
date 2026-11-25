# SPDX-License-Identifier: Apache-2.0
"""Round 5: bugs found in number formats, typed values, filters and conditional formats, each test failing without
its fix.

Run: python -m unittest tests.test_round5_bugs (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class FormatAndFilterRound5Test(unittest.TestCase):
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
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)

    def test_typed_times_keep_am_pm_and_hours_past_24(self):
        self.keys("k:Escape|m:2,2|g:B2|w:2|t:10:30 pm|k:Enter|w:3|g:B3|w:2|t:25:00|k:Enter|w:3")
        self.assertEqual(self.ws.Range("B2").Text, "10:30 PM")
        self.assertEqual(self.ws.Range("B2").NumberFormat, "h:mm AM/PM")
        self.assertEqual(self.ws.Range("B3").Text, "25:00:00")
        self.assertAlmostEqual(self.ws.Range("B3").Value2, 25 / 24)

    def test_negative_zero_and_negative_dates_show_as_excel(self):
        c = self.ws.Range("C2")
        c.Value = -0.4
        c.NumberFormat = "0"
        self.assertEqual(c.Text, "0")  # Excel 16 drops the minus when every digit shows as 0
        d = self.ws.Range("C3")
        d.Value = -1
        d.NumberFormat = "yyyy-mm-dd"
        self.assertTrue(d.Text and set(d.Text) == {"#"}, d.Text)

    def test_filter_by_values_is_case_insensitive_and_filters_reads_each_column(self):
        from yc.excel import xlFilterValues, xlOr, xlAnd, xlTop10Items

        ws = self.ws
        ws.Range("A1:C5").Value = [["Sym", "Qty", "Side"], ["AAPL", 10, "Buy"], ["MSFT", -5, "Sell"],
                                   ["aapl", 3, "buy"], ["NVDA", 12, "Buy"]]
        ws.Range("A1:C5").AutoFilter(Field=1, Criteria1=["AAPL", "NVDA"], Operator=xlFilterValues)
        self.assertEqual([r for r in range(2, 6) if not ws.Rows(r).Hidden], [2, 4, 5])
        f = ws.AutoFilter.Filters
        self.assertEqual(f.Count, 3)
        self.assertTrue(f(1).On)
        self.assertFalse(f(2).On)
        self.assertEqual(f(1).Operator, xlOr)
        self.assertEqual((f(1).Criteria1, f(1).Criteria2), ("=AAPL", "=NVDA"))
        ws.Range("A1:C5").AutoFilter(Field=2, Criteria1=">0", Operator=xlAnd, Criteria2="<11")
        self.assertEqual((f.Item(2).Operator, f.Item(2).Criteria1, f.Item(2).Criteria2), (xlAnd, ">0", "<11"))
        ws.Range("A1:C5").AutoFilter(Field=3, Criteria1="2", Operator=xlTop10Items)
        self.assertEqual((f(3).Operator, f(3).Criteria1), (xlTop10Items, 2))
        self.assertEqual(len(f), 3)  # the filtered columns' settings, as before
        with self.assertRaises(IndexError):
            f(4)

    def test_text_contains_rule_takes_wildcards(self):
        from yc.excel import xlTextString, xlContains

        ws = self.ws
        ws.Range("E1:E3").Value = [["abc"], ["xyz"], ["ac"]]
        fc = ws.Range("E1:E3").FormatConditions.Add(xlTextString, String="a*c", TextOperator=xlContains)
        fc.Interior.Color = 0x0000FF
        self.keys("g:H1|w:4")
        import tempfile
        from yc import ui
        from tests.test_excel_parity import read_png

        rects = {a: ui.cell_rect(a) for a in ("E1", "E2", "E3")}
        self.keys("w:3")
        path = os.path.join(tempfile.mkdtemp(prefix="yc_r5_"), "cf.png")
        ui.screenshot(path, max_width=8000)
        _, _, px = read_png(path)

        def red(a):
            p = px(int(rects[a][0]) + 3, int(rects[a][1]) + 3)
            return p[0] > 200 and p[1] < 80 and p[2] < 80

        self.assertEqual([red("E1"), red("E2"), red("E3")], [True, False, True])

    def test_launch_with_a_file_returns_once_the_file_is_read(self):
        # yc.launch(path) returned while the file was still being read: the instance file named no workbook yet, so
        # another YCGrid's CompareSideBySideWith("Big.ycg") found no window to pair with
        import tempfile

        from yc import excel

        # enough formulas that reading the file takes longer than launch() takes to see the window
        self.ws.Range("A1:A200000").Value = [[i] for i in range(200000)]
        self.ws.Range("B1:E1").Formula = "=$A1*2+COLUMN()"
        self.ws.Range("B1:E200000").FillDown()
        path = os.path.join(tempfile.mkdtemp(prefix="yc_r5_"), "Big.ycg")
        excel.ActiveWorkbook.SaveCopyAs(path)
        other = yc.launch(path, hidden=True, exe=EXE, lang="en")
        try:
            mine = [i for i in yc.find_instances() if i.get("pid") == other.pid]
            self.assertTrue(mine)
            self.assertEqual(os.path.normcase(mine[0].get("file", "")), os.path.normcase(path))
            self.assertFalse(mine[0].get("opening"))
        finally:
            other.terminate()
            other.wait(10)
            yc.connect(target=self.proc.pid)

    def test_very_hidden_sheets_and_the_last_visible_sheet(self):
        from yc import excel
        from yc.errors import YcError
        from yc.excel import xlSheetHidden, xlSheetVeryHidden, xlSheetVisible

        extra = excel.Worksheets.Add(After=self.ws)
        extra.Visible = xlSheetVeryHidden
        self.assertEqual(extra.Visible, xlSheetVeryHidden)
        extra.Activate()  # Excel 16 (measured): a hidden sheet can't be activated, and Activate raises nothing
        self.assertEqual(extra.Visible, xlSheetVeryHidden)
        extra.Visible = xlSheetHidden
        self.assertEqual(extra.Visible, xlSheetHidden)
        extra.Visible = xlSheetVisible
        self.assertEqual(extra.Visible, xlSheetVisible)
        shown = [w for w in excel.Worksheets if w.Visible == xlSheetVisible]
        for w in shown[:-1]:
            w.Visible = False
        try:
            with self.assertRaises(YcError):
                shown[-1].Visible = False  # a workbook keeps one visible sheet
            self.assertEqual(shown[-1].Visible, xlSheetVisible)
        finally:
            for w in shown:
                w.Visible = True
            self.ws.Activate()

    def test_rule_formulas_follow_inserted_rows_and_a_copy_names_itself(self):
        from yc import excel
        from yc.excel import xlBetween, xlValidAlertStop, xlValidateWholeNumber

        ws = self.ws
        ws.Range("A1:A10").Value = [[i] for i in range(1, 11)]
        ws.Range("C3:C6").Validation.Add(xlValidateWholeNumber, xlValidAlertStop, xlBetween, "1", "=$A$10")
        ws.Rows("4:5").Insert()
        self.assertEqual(ws.Range("C3").Validation.Formula2, "=$A$12")
        ws.Range("B1").Formula = f"='{ws.Name}'!A1+1"
        excel.Application.DisplayAlerts = False
        ws.Copy(After=ws)
        copy = excel.Application.ActiveSheet
        try:
            self.assertNotEqual(copy.Name, ws.Name)
            self.assertIn(copy.Name, copy.Range("B1").Formula)
            copy.Range("A1").Value = 100
            self.assertEqual(copy.Range("B1").Value, 101)
            self.assertEqual(ws.Range("B1").Value, 2)
        finally:
            copy.Delete()
            ws.Activate()

    def test_note_text_start_and_overwrite(self):
        ws = self.ws
        ws.Range("G1").AddComment("second")
        c = ws.Range("G1").Comment
        self.assertEqual(c.Text(" more", Start=7, Overwrite=False), "second more")
        self.assertEqual(c.Text("SE", Start=1, Overwrite=True), "SEcond more")
        self.assertEqual(c.Text(">", 1), ">SEcond more")
        self.assertEqual(ws.Range("G1").Comment.Text(), ">SEcond more")
        self.assertEqual(c.Text("all new"), "all new")

    def test_union_joins_touching_areas_and_keeps_overlaps(self):
        from yc import excel

        ws = self.ws
        self.assertEqual(excel.Union(ws.Range("A1"), ws.Range("A2"), ws.Range("A3")).Address, "$A$1:$A$3")
        self.assertEqual(excel.Union(ws.Range("A1:A2"), ws.Range("B1:B2")).Address, "$A$1:$B$2")
        self.assertEqual(excel.Union(ws.Range("A1"), ws.Range("C1")).Address, "$A$1,$C$1")
        u = excel.Union(ws.Range("A1:A2"), ws.Range("A2:A3"))
        self.assertEqual(u.Address, "$A$1:$A$3")  # overlapping areas that make one rectangle are joined (Excel 16, measured)
        u = excel.Union(ws.Range("A1:B2"), ws.Range("B2:C3"))
        self.assertEqual((u.Areas.Count, u.Count), (2, 8))  # others stay, as in Excel (B2 counted twice)
        self.assertEqual(excel.Union(ws.Range("A1"), ws.Range("A1")).Address, "$A$1")

    def test_circular_reference_without_iteration_shows_zero(self):
        from yc import excel

        ws = self.ws
        ws.Range("H1").Formula = "=H1+1"
        self.assertEqual(ws.Range("H1").Value, 0)
        excel.Application.CalculateFull()
        self.assertEqual(ws.Range("H1").Value, 0)  # it read 2 after a full calculation
        ws.Range("H1").ClearContents()

    def test_traced_cells_are_one_set_as_in_excel(self):
        ws = self.ws
        ws.Range("J1:L1").Value = [[1, 2, 3]]
        ws.Range("J3").Formula = "=J1+K1+SUM(J1:L1)"
        self.assertEqual(ws.Range("J3").DirectPrecedents.Address, "$J$1:$L$1")  # it was $J$1,$K$1,$J$1:$L$1
        ws.Range("J4").Formula = "=J1+L1"
        self.assertEqual(ws.Range("J4").Precedents.Address, "$J$1,$L$1")
        self.assertEqual(ws.Range("J1").Dependents.Address, "$J$3:$J$4")


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class KeyboardRound5Test(unittest.TestCase):
    """Keys over hidden rows and merged cells, as a person presses them (and Range.End, which follows Ctrl+arrow)."""

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

        self.keys("k:Escape|w:2|m:2,2|w:2")
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()
        self.keys("w:3")

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)

    @staticmethod
    def active():
        from yc import excel

        return excel.Application.ActiveCell.Address

    @staticmethod
    def selection():
        from yc import excel

        return excel.Application.Selection.Address

    def test_ctrl_arrows_and_end_pass_over_hidden_rows(self):
        from yc.excel import xlDown, xlToRight

        ws = self.ws
        ws.Range("A1:A10").Value = [[i] for i in range(1, 11)]
        ws.Rows("9:10").Hidden = True
        self.keys("g:A1|w:2|k:c-Down|w:3")
        self.assertEqual(self.active(), "$A$8")  # it stopped on the hidden A10
        self.assertEqual(ws.Range("A1").End(xlDown).Address, "$A$8")
        self.keys("g:A1|w:2|k:c-s-Down|w:3")
        self.assertEqual(self.selection(), "$A$1:$A$8")
        ws.Range("C2:F2").Value = [[1, 2, 3, 4]]
        ws.Columns("E:F").Hidden = True
        self.assertEqual(ws.Range("C2").End(xlToRight).Address, "$D$2")

    def test_enter_in_a_selection_passes_over_hidden_rows_and_merged_cells(self):
        ws = self.ws
        ws.Range("C1:C5").Value = [[1], [2], [3], [4], [5]]
        ws.Rows(2).Hidden = True
        self.keys("g:C1:C3|w:2|k:Enter|w:2")
        self.assertEqual(self.active(), "$C$3")
        ws.Rows(2).Hidden = False
        ws.Range("E2:E3").Merge()
        self.keys("g:E1:E5|w:2|k:Enter|w:2|k:Enter|w:2")
        self.assertEqual(self.active(), "$E$4")  # E1, E2 (the merge), then E4

    def test_selections_take_in_whole_merged_cells(self):
        ws = self.ws
        ws.Range("B2:C3").Merge()
        self.keys("g:B2|w:2|k:s-Space|w:3")
        self.assertEqual(self.selection(), "$2:$3")
        self.keys("g:B2|w:2|k:c-Space|w:3")
        self.assertEqual(self.selection(), "$B:$C")
        self.keys("g:A1|w:2|k:s-Down|w:2|k:s-Right|w:3")
        self.assertEqual(self.selection(), "$A$1:$C$3")
        from yc import ui

        a, b = ui.cell_rect("A1"), ui.cell_rect("B2")
        x1, y1 = (a[0] + a[2]) / 2, (a[1] + a[3]) / 2
        x2, y2 = b[0] + 6, b[1] + 6
        self.keys(f"g:E8|w:2|m:{x1},{y1}|w:4|d:{x1},{y1},{x2},{y2}|w:4|m:2,2|w:2")  # a drag into the merged cell
        self.assertEqual(self.selection(), "$A$1:$C$3")

    def test_autocomplete_from_the_first_character_and_beyond_ascii(self):
        ws = self.ws
        name = "\u738b\u5c0f\u660e"  # a three-character Chinese name
        ws.Range("A1:A3").Value = [["Apple"], [name], ["\u00c9clair"]]
        self.keys("g:A4|w:2|t:\u738b|w:3|k:Enter|w:3")  # one character typed (as an IME commits it)
        self.assertEqual(ws.Range("A4").Value, name)
        self.keys("g:A5|w:2|t:a|w:3|k:Enter|w:3")  # the first letter completes, as in Excel
        self.assertEqual(ws.Range("A5").Value, "Apple")
        self.keys("g:A6|w:2|t:\u00e9|w:3|t:c|w:3|k:Enter|w:3")  # case ignored beyond ASCII
        self.assertEqual(ws.Range("A6").Value, "\u00c9clair")
        self.keys("g:A1|w:2|k:F2|w:3|k:Enter|w:3")  # editing a cell's own text completes nothing
        self.assertEqual(ws.Range("A1").Value, "Apple")


if __name__ == "__main__":
    unittest.main()
