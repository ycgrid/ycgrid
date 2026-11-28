# SPDX-License-Identifier: Apache-2.0
"""Round 8: tables' Sort and Range.ListObject, the active sheet after Delete / Visible, Copy of hidden sheets and the
outline (ShowDetail, ClearOutline, Ungroup, eight levels) measured against Excel 16 through COM, each test failing
without its fix.

Run: python -m unittest tests.test_round8_sheets (with the bundled Python).
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
class SheetsRound8Test(unittest.TestCase):
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

    def table(self):
        from yc import excel

        ws = self.ws
        ws.Range("A1:C4").Value = [["Name", "Qty", "Price"], ["a", 1, 10], ["b", 2, 20], ["c", 3, 30]]
        return ws.ListObjects.Add(excel.xlSrcRange, ws.Range("A1:C4"), XlListObjectHasHeaders=excel.xlYes)

    def col(self, ref):
        return [r[0] for r in self.ws.Range(ref).Value]

    def levels(self, n, cols=False):
        if cols:
            return [int(self.ws.Cells(1, c).EntireColumn.OutlineLevel) for c in range(1, n + 1)]
        return [int(self.ws.Cells(r, 1).EntireRow.OutlineLevel) for r in range(1, n + 1)]

    def hidden(self, n):
        return [r for r in range(1, n + 1) if self.ws.Cells(r, 1).EntireRow.Hidden]

    def sheets(self, *names):
        """New sheets after this test's sheet, in order."""
        from yc import excel

        out, after = [], self.ws
        for n in names:
            w = excel.Worksheets.Add(After=after)
            w.Name = n
            out.append(w)
            after = w
        return out

    # ---- B04: ListObject.Sort as a recorded macro writes it, Range.ListObject ----
    def test_table_sort_as_a_recorded_macro_writes_it(self):
        from yc import excel

        lo = self.table()
        name = lo.Name
        ws = self.ws
        ws.ListObjects(name).Sort.SortFields.Clear()
        ws.ListObjects(name).Sort.SortFields.Add(ws.Range("C2:C4"), excel.xlSortOnValues, excel.xlDescending)
        s = ws.ListObjects(name).Sort
        s.Header = excel.xlYes
        s.MatchCase = False
        s.Orientation = excel.xlTopToBottom
        s.Apply()
        self.assertEqual(self.col("A1:A4"), ["Name", "c", "b", "a"])  # Excel: by Price, descending; the header stays
        self.assertEqual(ws.ListObjects(name).Sort.SortFields.Count, 1)  # the fields stay after Apply, as in Excel
        self.assertEqual(ws.ListObjects(name).Sort.Rng.Address, "$A$1:$C$4")
        self.assertEqual(ws.ListObjects(name).Sort.Header, excel.xlYes)
        with self.assertRaises(excel.ApplicationDefinedError):
            ws.ListObjects(name).Sort.Header = excel.xlNo  # Excel refuses it for a table with its header row
        ws.ListObjects(name).Sort.SortFields.Clear()
        ws.ListObjects(name).Sort.SortFields.Add(Key=ws.Range("A1:A4"), SortOn=excel.xlSortOnValues, Order=excel.xlAscending,
                                                 DataOption=excel.xlSortNormal)
        ws.ListObjects(name).Sort.Apply()
        self.assertEqual(self.col("A1:A4"), ["Name", "a", "b", "c"])

    def test_table_sort_keeps_the_totals_row(self):
        from yc import excel

        lo = self.table()
        lo.ShowTotals = True
        total = self.ws.Range("C5").Formula
        lo.Sort.SortFields.Clear()
        lo.Sort.SortFields.Add(self.ws.Range("B2:B4"), 0, excel.xlDescending)
        lo.Sort.Apply()
        self.assertEqual(self.col("B2:B4"), [3, 2, 1])
        self.assertEqual(self.ws.Range("C5").Formula, total)

    def test_range_list_object(self):
        lo = self.table()
        self.assertEqual(self.ws.Range("B3").ListObject.Name, lo.Name)
        self.assertEqual(self.ws.Range("C4:E6").ListObject.Name, lo.Name)  # Excel: the first cell's table
        self.assertIsNone(self.ws.Range("F9").ListObject)
        self.assertIsNone(self.ws.Range("D1:F2").ListObject)

    def test_save_as_xlsm_is_refused(self):
        import os
        import tempfile

        from yc import excel
        from yc.errors import UnsupportedError

        path = os.path.join(tempfile.gettempdir(), "yc_round8_refused.xlsm")
        with self.assertRaises(UnsupportedError):
            excel.ActiveWorkbook.SaveAs(path, FileFormat=52)  # Excel refused the file it wrote
        with self.assertRaises(UnsupportedError):
            excel.ActiveWorkbook.SaveAs(path)
        self.assertFalse(os.path.exists(path))

    def test_a_table_without_data_rows_keeps_its_insert_row(self):
        lo = self.table()
        for _ in range(3):
            lo.ListRows(1).Delete()
        self.assertEqual(lo.ListRows.Count, 0)
        self.assertIsNone(lo.DataBodyRange)
        self.assertEqual(lo.Range.Address, "$A$1:$C$2")  # Excel: the header and the empty insert row

    def test_list_rows_add_takes_positions_up_to_count_plus_one(self):
        from yc.errors import SubscriptOutOfRangeError

        lo = self.table()
        with self.assertRaises(SubscriptOutOfRangeError):
            lo.ListRows.Add(9)  # Excel: DISP_E_BADINDEX
        with self.assertRaises(SubscriptOutOfRangeError):
            lo.ListRows.Add(0)
        self.assertEqual(lo.ListRows.Count, 3)
        lo.ListRows.Add(4)
        self.assertEqual(lo.ListRows.Count, 4)

    # ---- B17: the active sheet after deleting or hiding it ----
    def test_deleting_the_active_sheet_activates_the_next_one_at_once(self):
        from yc import excel

        a, b, c = self.sheets("DelA", "DelB", "DelC")
        b.Activate()
        b.Delete()
        self.assertEqual(excel.Application.ActiveSheet.Name, "DelC")  # Excel: the next sheet, right away
        c.Delete()
        self.assertEqual(excel.Application.ActiveSheet.Name, "DelA")  # the last one: the sheet before it

    def test_deleting_skips_hidden_sheets(self):
        from yc import excel

        a, b, c = self.sheets("SkipA", "SkipB", "SkipC")
        c.Visible = False
        b.Activate()
        b.Delete()
        self.assertEqual(excel.Application.ActiveSheet.Name, "SkipA")  # SkipC is hidden: the one before

    def test_hiding_the_active_sheet_activates_the_next_one(self):
        from yc import excel

        a, b, c = self.sheets("HidA", "HidB", "HidC")
        b.Activate()
        b.Visible = False
        self.assertEqual(excel.Application.ActiveSheet.Name, "HidC")
        self.assertEqual(excel.Application.ActiveSheet.Visible, excel.xlSheetVisible)
        c.Visible = False
        self.assertEqual(excel.Application.ActiveSheet.Name, "HidA")

    def test_activate_of_a_hidden_sheet_does_nothing(self):
        from yc import excel

        (h,) = self.sheets("ActHidden")
        self.ws.Activate()
        h.Visible = False
        h.Activate()  # Excel 16 (measured): no error, the active sheet stays
        self.assertEqual(excel.Application.ActiveSheet.Name, self.ws.Name)
        h.Visible = excel.xlSheetVeryHidden
        h.Activate()
        self.assertEqual(excel.Application.ActiveSheet.Name, self.ws.Name)
        with self.assertRaises(excel.ApplicationDefinedError):
            h.Select()  # Select raises, as in Excel

    # ---- B18: Copy of hidden sheets ----
    def test_copy_of_a_hidden_sheet_is_hidden_and_very_hidden_ones_are_refused(self):
        from yc import excel

        (h,) = self.sheets("CopyHid")
        h.Range("A1").Value = 5
        self.ws.Activate()
        h.Visible = False
        n = excel.Worksheets.Count
        c = h.Copy(After=self.ws)  # Excel: copied, the copy hidden too, the active sheet stays
        self.assertEqual(excel.Worksheets.Count, n + 1)
        self.assertEqual(c.Visible, excel.xlSheetHidden)
        self.assertEqual(c.Range("A1").Value, 5)
        self.assertEqual(excel.Application.ActiveSheet.Name, self.ws.Name)
        h.Visible = excel.xlSheetVeryHidden
        with self.assertRaises(excel.ApplicationDefinedError):
            h.Copy(After=self.ws)  # Excel: Copy method of Worksheet class failed
        self.assertEqual(excel.Worksheets.Count, n + 1)
        v = self.ws.Copy(After=self.ws)  # a visible sheet's copy is the active sheet
        self.assertEqual(excel.Application.ActiveSheet.Name, v.Name)

    # ---- B21: ShowDetail follows SummaryRow / SummaryColumn ----
    def test_show_detail_with_the_summary_row_above(self):
        from yc import excel

        ws = self.ws
        ws.Outline.SummaryRow = excel.xlSummaryAbove
        ws.Range("A2:A4").EntireRow.Group()
        ws.Range("A1").EntireRow.ShowDetail = False
        self.assertEqual(self.hidden(5), [2, 3, 4])  # Excel: row 1 sums up rows 2:4
        self.assertFalse(ws.Range("A1").EntireRow.ShowDetail)
        self.assertTrue(ws.Range("A5").EntireRow.ShowDetail)
        ws.Range("A1").EntireRow.ShowDetail = True
        self.assertEqual(self.hidden(5), [])

    def test_show_detail_above_takes_the_group_a_row_is_in(self):
        from yc import excel

        ws = self.ws
        ws.Outline.SummaryRow = excel.xlSummaryAbove
        ws.Range("A2:A6").EntireRow.Group()
        ws.Range("A3:A4").EntireRow.Group()
        ws.Range("A5").EntireRow.ShowDetail = False  # not the inner group's summary row when summaries are above
        self.assertEqual(self.hidden(7), [2, 3, 4, 5, 6])

    def test_show_detail_with_the_summary_column_on_the_left(self):
        from yc import excel

        ws = self.ws
        ws.Outline.SummaryColumn = excel.xlSummaryOnLeft
        ws.Range("C1:D1").EntireColumn.Group()
        ws.Range("B1").EntireColumn.ShowDetail = False
        self.assertTrue(ws.Range("C1").EntireColumn.Hidden)
        self.assertTrue(ws.Range("D1").EntireColumn.Hidden)
        self.assertFalse(ws.Range("E1").EntireColumn.Hidden)
        self.assertFalse(ws.Range("B1").EntireColumn.ShowDetail)
        self.assertTrue(ws.Range("E1").EntireColumn.ShowDetail)
        ws.Range("B1").EntireColumn.ShowDetail = True
        self.assertFalse(ws.Range("C1").EntireColumn.Hidden)

    def test_show_detail_reads_true_on_rows_that_sum_nothing_and_refuses_them(self):
        from yc import excel

        ws = self.ws
        self.assertTrue(ws.Range("A5").EntireRow.ShowDetail)  # Excel: True for a row in no group
        with self.assertRaises(excel.ApplicationDefinedError):
            ws.Range("A5").EntireRow.ShowDetail = False
        with self.assertRaises(excel.ApplicationDefinedError):
            ws.Range("A5").EntireRow.ShowDetail = True
        ws.Range("A2:A4").EntireRow.Group()
        ws.Range("A5").EntireRow.ShowDetail = False
        self.assertFalse(ws.Range("A5").EntireRow.ShowDetail)
        self.assertTrue(ws.Range("A3").EntireRow.ShowDetail)  # Excel: a row inside the group reads True

    # ---- B24: ClearOutline ----
    def test_clear_outline_of_one_cell_clears_the_sheet(self):
        ws = self.ws
        ws.Range("A2:A4").EntireRow.Group()
        ws.Range("C1:E1").EntireColumn.Group()
        ws.Range("A3").ClearOutline()  # Excel: one cell clears the whole outline, rows and columns
        self.assertEqual(self.levels(5), [1, 1, 1, 1, 1])
        self.assertEqual(self.levels(5, cols=True), [1, 1, 1, 1, 1])

    def test_clear_outline_of_a_range_clears_its_rows_and_columns(self):
        ws = self.ws
        ws.Range("A2:A5").EntireRow.Group()
        ws.Range("C1:E1").EntireColumn.Group()
        ws.Range("A3:A4").ClearOutline()
        self.assertEqual(self.levels(6), [1, 2, 1, 1, 2, 1])
        self.assertEqual(self.levels(5, cols=True), [1, 1, 2, 2, 2])
        ws.Range("D7:E8").ClearOutline()
        self.assertEqual(self.levels(5, cols=True), [1, 1, 2, 1, 1])

    def test_clear_outline_shows_collapsed_rows_and_ungroup_keeps_them_hidden(self):
        # Excel 16, measured (Range.ClearOutline, Data > Ungroup > Clear Outline, Alt+Shift+Left, Range.Ungroup):
        # Clear Outline shows the rows a collapsed group hid, Ungroup leaves them hidden. This test said the rows
        # stay hidden after Clear Outline without a measurement
        ws = self.ws
        ws.Range("A2:A4").EntireRow.Group()
        ws.Outline.ShowLevels(RowLevels=1)
        ws.Range("A1:A11").ClearOutline()
        self.assertEqual(self.levels(5), [1, 1, 1, 1, 1])
        self.assertEqual(self.hidden(5), [])
        ws.Range("A2:A4").EntireRow.Hidden = False
        ws.Range("A2:A4").EntireRow.Group()
        ws.Outline.ShowLevels(RowLevels=1)
        ws.Range("A2:A4").EntireRow.Ungroup()
        self.assertEqual(self.hidden(5), [2, 3, 4])
        ws.Range("A2:A4").EntireRow.Hidden = False
        ws.Range("A2:A4").EntireRow.Group()
        ws.Range("A5").EntireRow.ShowDetail = False
        ws.Range("A3").EntireRow.Ungroup()  # a row taken out of a collapsed group stays hidden too
        self.assertEqual(self.levels(5), [1, 2, 1, 2, 1])
        self.assertEqual(self.hidden(5), [2, 3, 4])

    # ---- B29: eight levels, Ungroup without a group ----
    def test_outline_levels_go_up_to_eight(self):
        from yc import excel

        ws = self.ws
        for _ in range(7):
            ws.Range("A2:A9").EntireRow.Group()
        self.assertEqual(ws.Range("A5").EntireRow.OutlineLevel, 8)
        with self.assertRaises(excel.ApplicationDefinedError):
            ws.Range("A2:A9").EntireRow.Group()  # Excel: Group method of Range class failed
        self.assertEqual(ws.Range("A5").EntireRow.OutlineLevel, 8)
        ws.Range("A1:A12").ClearOutline()
        ws.Range("A3").EntireRow.OutlineLevel = 8
        ws.Range("A2:A4").EntireRow.Group()  # one row at 8 stays there, the others group
        self.assertEqual(self.levels(5), [1, 2, 8, 2, 1])
        for bad in (9, 0):
            with self.assertRaises(excel.ApplicationDefinedError):
                ws.Range("A6:A7").EntireRow.OutlineLevel = bad
        self.assertEqual(self.levels(8)[5:7], [1, 1])

    def test_ungroup_of_rows_in_no_group_raises(self):
        from yc import excel

        ws = self.ws
        with self.assertRaises(excel.ApplicationDefinedError):
            ws.Range("A2").EntireRow.Ungroup()
        ws.Range("A2:A4").EntireRow.Group()
        with self.assertRaises(excel.ApplicationDefinedError):
            ws.Range("A5:A6").EntireRow.Ungroup()
        ws.Range("A4:A6").EntireRow.Ungroup()  # partly grouped: fine
        self.assertEqual(self.levels(6), [1, 2, 2, 1, 1, 1])


if __name__ == "__main__":
    unittest.main()
