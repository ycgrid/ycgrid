# SPDX-License-Identifier: Apache-2.0
"""Round 10: bugs found by driving the object model the way recorded macros and scripts do, each with what Excel 16
does.

Run: python -m unittest tests.test_round10_bugs (with the bundled Python).
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
class Round10Test(unittest.TestCase):
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

    def test_a_font_set_on_the_cell_reaches_every_character(self):
        ws = self.ws
        ws.Range("A1").Value = "abc"
        ws.Range("A1").Characters(2, 1).Font.Bold = True
        self.assertIsNone(ws.Range("A1").Font.Bold)  # the characters differ: Excel's Null
        self.assertIs(ws.Range("A1").Characters(1, 1).Font.Bold, False)
        ws.Range("A1").Font.Bold = True
        self.assertIs(ws.Range("A1").Font.Bold, True)
        self.assertIs(ws.Range("A1").Characters(1, 3).Font.Bold, True)
        ws.Range("A1").Font.Bold = False  # the "b" stayed bold
        self.assertIs(ws.Range("A1").Characters(2, 1).Font.Bold, False)
        self.assertIs(ws.Range("A1").Font.Bold, False)
        ws.Range("A2").Value = "xyz"
        ws.Range("A2").Characters(1, 1).Font.Color = 255
        self.assertIsNone(ws.Range("A2").Font.Color)
        self.assertIsNone(ws.Range("A1:A2").Font.Color)
        self.assertIs(ws.Range("A1:A2").Font.Bold, False)
        ws.Range("A2").Font.Color = 0xFF0000
        self.assertEqual(ws.Range("A2").Characters(1, 1).Font.Color, 0xFF0000)

    def test_a_series_name_linked_to_a_cell_follows_it(self):
        from yc import excel

        ws = self.ws
        ws.Range("A1:B3").Value = [["x", "y"], [1, 2], [3, 4]]
        co = ws.ChartObjects().Add(100, 100, 300, 200)
        co.Chart.SetSourceData(ws.Range("A1:B3"))
        n = ws.Name
        ws.Rows(1).Insert()  # Excel 16: the name moves with the values
        self.assertEqual(co.Chart.SeriesCollection(1).Formula, f"=SERIES({n}!$A$2,,{n}!$A$3:$A$4,1)")
        ws.Name = "Quotes R10"
        self.assertEqual(co.Chart.SeriesCollection(2).Formula, "=SERIES('Quotes R10'!$B$2,,'Quotes R10'!$B$3:$B$4,2)")
        self.assertEqual(co.Chart.SeriesCollection(1).Name, "x")
        ws.Copy(After=ws)
        cp = excel.ActiveSheet
        c2 = cp.ChartObjects(1).Chart
        self.assertEqual(c2.SeriesCollection(1).Formula, f"=SERIES('{cp.Name}'!$A$2,,'{cp.Name}'!$A$3:$A$4,1)")

    def test_rows_fit_a_font_size_or_wrap_text_set_from_python(self):
        ws = self.ws
        h0 = ws.Rows(1).RowHeight
        ws.Range("A1").Value = 1
        ws.Range("A1").Font.Size = 20  # Excel 16: the row grows to 26.25
        self.assertGreater(ws.Rows(1).RowHeight, 20)
        ws.Range("A1").Font.Size = 11
        self.assertEqual(ws.Rows(1).RowHeight, h0)
        ws.Range("C2").Font.Size = 24  # an empty cell grows its row too
        self.assertGreater(ws.Rows(2).RowHeight, 24)
        ws.Columns("B").ColumnWidth = 8
        ws.Range("B3").Value = "line one is long enough to wrap around"
        ws.Range("B3").WrapText = True
        self.assertGreater(ws.Rows(3).RowHeight, 2 * h0)
        ws.Rows(5).RowHeight = 9  # a height set by hand stays
        ws.Range("A5").Value = "x"
        ws.Range("A5").Font.Size = 30
        self.assertEqual(ws.Rows(5).RowHeight, 9)

    def test_worksheet_paste_without_a_destination_fills_the_selection(self):
        from yc import excel

        ws = self.ws
        ws.Range("A1").Value = "x"
        ws.Range("A1").Copy()
        ws.Range("B1:B3").Select()
        ws.Paste()  # Excel 16: one copied cell fills the selected cells
        self.assertEqual([ws.Range(f"B{i}").Value2 for i in range(1, 4)], ["x"] * 3)
        ws.Range("A1:A2").Value = [[1], [2]]
        ws.Range("A1:A2").Copy()
        ws.Range("D3:D6").Select()
        excel.ActiveCell  # the selection's first cell is active
        excel.ActiveSheet.Paste()
        self.assertEqual([ws.Range(f"D{i}").Value2 for i in range(3, 7)], [1, 2, 1, 2])
        excel.Application.CutCopyMode = False

    def test_ctrl_shift_l_beside_a_table_filters_the_list_and_keeps_the_tables(self):
        import time

        from yc import excel

        ws = self.ws
        ws.Range("A1:A4").Value = [["k"], ["x"], ["y"], ["x"]]
        ws.Range("D6:D8").Value = [["v"], [1], [2]]
        lo = ws.ListObjects.Add(excel.xlSrcRange, ws.Range("A1:A4"), XlListObjectHasHeaders=excel.xlYes)
        lo.Range.AutoFilter(Field=1, Criteria1="x")
        self.assertTrue(ws.Rows(3).Hidden)

        def keys(script):
            self.conn.request("ui.input", {"script": "m:2,2|w:2|" + script + "|w:4", "timeout_ms": 15000}, timeout=30)
            time.sleep(0.2)

        keys("g:D6|k:c-s-L")  # it turned the table's filter off and made no AutoFilter for D6:D8
        self.assertTrue(ws.AutoFilterMode)
        self.assertEqual(ws.AutoFilter.Range.Address, "$D$6:$D$8")
        self.assertTrue(lo.ShowAutoFilter)
        self.assertTrue(ws.Rows(3).Hidden)  # the table's criteria stay
        keys("g:D6|k:c-s-L")
        self.assertFalse(ws.AutoFilterMode)
        self.assertTrue(lo.ShowAutoFilter)
        self.assertTrue(ws.Rows(3).Hidden)

    def test_a_form_controls_linked_cell_and_list_follow_their_cells(self):
        from yc import excel

        ws = self.ws
        ws.Range("B1:B3").Value = [["x"], ["y"], ["z"]]
        box = ws.Shapes.AddFormControl(excel.xlCheckBox, 200, 50, 80, 20)
        box.ControlFormat.LinkedCell = "$A$5"
        dd = ws.Shapes.AddFormControl(excel.xlDropDown, 200, 100, 80, 20)
        dd.ControlFormat.ListFillRange = "$B$1:$B$3"
        dd.ControlFormat.LinkedCell = "$C$1"
        ws.Rows(1).Insert()  # Excel 16: $A$6, $B$2:$B$4, $C$2
        self.assertEqual(box.ControlFormat.LinkedCell, "$A$6")
        self.assertEqual(dd.ControlFormat.ListFillRange, "$B$2:$B$4")
        self.assertEqual(dd.ControlFormat.LinkedCell, "$C$2")
        other = excel.Worksheets.Add(After=ws)
        far = other.Shapes.AddFormControl(excel.xlCheckBox, 200, 50, 80, 20)
        far.ControlFormat.LinkedCell = f"'{ws.Name}'!$D$4"
        ws.Name = "Inputs R10"
        self.assertEqual(far.ControlFormat.LinkedCell, "'Inputs R10'!$D$4")
        ws.Columns(1).Delete()
        self.assertEqual(box.ControlFormat.LinkedCell, "#REF!")

    def test_label_cells_and_a_sparkline_date_axis_follow_their_cells(self):
        ws = self.ws
        n = ws.Name
        ws.Range("A1:C4").Value = [["x", "y", "note"], [1, 2, "a"], [3, 4, "b"], [5, 6, "c"]]
        ws.Range("E2:E4").Value = [[45300], [45310], [45330]]
        co = ws.ChartObjects().Add(300, 100, 300, 200)
        co.Chart.SetSourceData(ws.Range("A1:B4"))
        se = co.Chart.SeriesCollection(1)
        se.HasDataLabels = True
        se.DataLabels().Format.TextFrame2.TextRange.InsertChartField(7, f"={n}!$C$2:$C$4")
        g = ws.Range("G2").SparklineGroups.Add(1, "A2:A4")
        g.DateRange = "$E$2:$E$4"
        ws.Rows(1).Insert()
        self.assertEqual(se._get()["label_range"], f"{n}!$C$3:$C$5")
        self.assertEqual(ws.Range("G3").SparklineGroups(1).DateRange, "$E$3:$E$5")

    def test_one_copied_cell_pasted_over_a_filtered_list_fills_the_rows_it_shows(self):
        import time

        from yc import excel

        ws = self.ws
        ws.Range("A1:B5").Value = [["k", "v"], ["a", 1], ["b", 2], ["a", 3], ["b", 4]]
        ws.Range("D1:D2").Value = [["z"], ["q"]]
        ws.Range("A1:B5").AutoFilter(Field=1, Criteria1="a")

        def col_b():
            return [ws.Range(f"B{i}").Value2 for i in range(2, 6)]

        def reset():
            ws.Range("B2:B5").Value = [[1], [2], [3], [4]]

        # Excel 16, measured: Ctrl+V of one cell over B2:B5 writes B2 and B4 only
        self.conn.request("ui.input", {"script": "m:2,2|w:2|g:D1|k:c-C|g:B2:B5|k:c-V|w:4", "timeout_ms": 15000}, timeout=30)
        time.sleep(0.2)
        self.assertEqual(col_b(), ["z", 2, "z", 4])
        reset()
        ws.Range("D1").Copy()
        ws.Range("B2:B5").Select()
        ws.Paste()
        self.assertEqual(col_b(), ["z", 2, "z", 4])
        reset()
        ws.Range("D1").Copy(ws.Range("B2:B5"))
        self.assertEqual(col_b(), ["z", 2, "z", 4])
        reset()
        ws.Range("D1:D2").Copy(ws.Range("B2:B5"))  # a block goes into every row, hidden or not
        self.assertEqual(col_b(), ["z", "q", "z", "q"])
        excel.Application.CutCopyMode = False
        ws.AutoFilterMode = False

    def test_a_data_table_moves_with_an_inserted_row(self):
        from yc import excel

        ws = self.ws
        ws.Range("A1").Value = 2
        ws.Range("D2").Formula = "=A1*10"
        ws.Range("C3:C5").Value = [[1], [2], [3]]
        ws.Range("C2:D5").Table(ColumnInput=ws.Range("A1"))
        ws.Rows(1).Insert()  # Excel 16: {=TABLE(,A2)} over D4:D6
        self.assertEqual(ws.Range("D4").Formula, "=TABLE(,A2)")
        ws.Range("C5").Value = 7
        excel.Application.Calculate()
        self.assertEqual([ws.Range(f"D{i}").Value2 for i in (3, 4, 5, 6)], [20, 10, 70, 30])


if __name__ == "__main__":
    unittest.main()
