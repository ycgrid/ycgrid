# SPDX-License-Identifier: Apache-2.0
"""Round 4: bugs found by driving YCGrid from Python the way Excel macros do (sheets, notes, merges, validation, opening
files), each test failing without its fix.

Run: python -m unittest tests.test_round4_bugs (with the bundled Python).
"""
from __future__ import annotations

import os
import json
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ObjectModelRound4Test(unittest.TestCase):
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

    def test_validation_value_sees_a_rule_and_value_just_written(self):
        # Validation.Value read the published snapshot: right after the writes it still said True
        from yc.excel import xlBetween, xlValidateList, xlValidateWholeNumber

        ws = self.ws
        ws.Range("A1").Validation.Add(Type=xlValidateWholeNumber, Operator=xlBetween, Formula1="1", Formula2="10")
        ws.Range("A1").Value = 50
        self.assertFalse(ws.Range("A1").Validation.Value)
        ws.Range("A1").Value = 5
        self.assertTrue(ws.Range("A1").Validation.Value)
        ws.Range("B1").Validation.Add(Type=xlValidateList, Formula1="a,b,c")
        ws.Range("B1").Value = "d"
        self.assertFalse(ws.Range("B1").Validation.Value)

    def test_merge_cells_of_a_partly_merged_range_reads_none(self):
        ws = self.ws
        ws.Range("A1:B2").Merge()
        self.assertTrue(ws.Range("A1:B2").MergeCells)
        self.assertTrue(ws.Range("B2").MergeCells)
        self.assertIsNone(ws.Range("A1:C1").MergeCells)  # some cells merged, some not: Excel's Null (it read True)
        self.assertFalse(ws.Range("D1:E2").MergeCells)
        ws.Range("D1:E1").Merge()
        self.assertTrue(excel_union(ws, "A1:B2", "D1:E1").MergeCells)
        ws.Range("A1:C1").MergeCells = None  # read back: nothing changes
        self.assertTrue(ws.Range("A1:B2").MergeCells)

    def test_a_range_called_with_indexes_is_its_cell(self):
        ws = self.ws
        self.assertEqual(ws.Range("B2:D4")(2, 2).Address, "$C$3")
        self.assertEqual(ws.Range("B2:D4")(4).Address, "$B$3")

    def test_add_comment_refuses_a_second_note(self):
        from yc.errors import ApplicationDefinedError

        ws = self.ws
        ws.Range("B2").AddComment("hello")
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("B2").AddComment("again")
        self.assertEqual(ws.Range("B2").Comment.Text(), "hello")  # it replaced the note
        ws.Range("B2").ClearComments()
        ws.Range("B2").AddComment("new")
        self.assertEqual(ws.Range("B2").Comment.Text(), "new")
        ws.Range("B2").Clear()  # Clear takes the note too: a macro that clears and adds again works
        ws.Range("B2").AddComment("after clear")
        self.assertEqual(ws.Range("B2").Comment.Text(), "after clear")

    def test_copy_to_a_destination_brings_notes_and_validation(self):
        from yc.excel import xlValidateList

        ws = self.ws
        ws.Range("B3").Value = "x"
        ws.Range("B3").AddComment("hello")
        ws.Range("B3").Validation.Add(Type=xlValidateList, Formula1="x,y")
        ws.Range("B3").Copy(ws.Range("E5"))
        self.assertEqual(ws.Range("E5").Value, "x")
        self.assertIsNotNone(ws.Range("E5").Comment)  # Range.Copy Destination left the note behind
        self.assertEqual(ws.Range("E5").Comment.Text(), "hello")
        self.assertEqual(ws.Range("E5").Validation.Formula1, "x,y")
        self.assertEqual(ws.Comments.Count, 2)

    def test_a_copied_sheet_keeps_its_sheet_level_names(self):
        from yc import excel

        ws = self.ws
        ws.Range("A1").Value = 5
        ws.Names.Add("Local", RefersTo=f"={ws.Name}!$A$1")
        ws.Range("A3").Formula = "=Local+1"
        ws.Copy(After=ws)
        c = excel.Worksheets(ws.Index + 1)
        self.assertEqual(c.Range("A3").Value, 6)  # it was #NAME?
        c.Range("A1").Value = 100
        self.assertEqual((c.Range("A3").Value, ws.Range("A3").Value), (101, 6))

    def test_worksheets_add_goes_before_the_active_sheet(self):
        from yc import excel

        ws = self.ws
        i = ws.Index
        n = excel.Worksheets.Add()
        self.assertEqual(n.Index, i)  # before the active sheet, as in Excel (it went to the end)
        self.assertEqual(ws.Index, i + 1)
        last = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.assertEqual(last.Index, excel.Worksheets.Count)

    # ---- round 2 ----
    def test_evaluate_and_worksheet_functions_give_whole_arrays(self):
        # an array result came back as its first value only
        from yc import excel

        ws = self.ws
        ws.Range("B1:B3").Value = [[1], [2], [3]]
        r = excel.Evaluate("{1,2,3}*2")
        self.assertEqual([list(x) for x in r], [[2, 4, 6]])
        self.assertEqual(r(1, 3), 6)  # 1-based, like Range.Value
        t = excel.WorksheetFunction.Transpose(ws.Range("B1:B3"))
        self.assertEqual([list(x) for x in t], [[1, 2, 3]])
        self.assertEqual(excel.Evaluate("SUM(B1:B3)"), 6)  # a single value stays a value

    def test_a_one_row_or_one_column_array_repeats_like_excel(self):
        ws = self.ws
        ws.Range("D1:D3").Value = [7, 8, 9]  # Excel: a 1-D array is a row, repeated down (it was 7, #N/A, #N/A)
        self.assertEqual([list(x) for x in ws.Range("D1:D3").Value], [[7], [7], [7]])
        ws.Range("E1:G2").Value = [[1], [2]]
        self.assertEqual([list(x) for x in ws.Range("E1:G2").Value], [[1, 1, 1], [2, 2, 2]])
        ws.Range("E5:G5").Value = [1, 2, 3]
        self.assertEqual([list(x) for x in ws.Range("E5:G5").Value], [[1, 2, 3]])
        ws.Range("A10:C11").Value = [[1, 2], [3, 4]]  # a larger range than a 2-D array: #N/A past it
        self.assertEqual(str(ws.Range("C10").Value), "#N/A")

    def test_window_zoom_refuses_a_value_excel_refuses(self):
        from yc import excel
        from yc.errors import ApplicationDefinedError

        excel.ActiveWindow.Zoom = 150
        self.assertEqual(excel.ActiveWindow.Zoom, 150)
        for bad in (5, 401):
            with self.assertRaises(ApplicationDefinedError):  # it was clamped to 10 / 400
                excel.ActiveWindow.Zoom = bad
        self.assertEqual(excel.ActiveWindow.Zoom, 150)
        excel.ActiveWindow.Zoom = 100

    def test_series_values_are_values(self):
        # Series.Values / XValues returned the range's text: Excel gives the values (the range is in Formula)
        ws = self.ws
        ws.Range("A1:C4").Value = [["", "S1", "S2"], ["x", 1, 4], ["y", 2, 5], ["z", 3, 6]]
        ch = ws.ChartObjects.Add(100, 50, 300, 200).Chart
        ch.SetSourceData(ws.Range("A1:C4"))
        s2 = ch.SeriesCollection(2)
        self.assertEqual(s2.Values, (4, 5, 6))
        self.assertEqual(s2.XValues, ("x", "y", "z"))
        ws.Range("C2").Value = 40
        self.assertEqual(ch.SeriesCollection(2).Values[0], 40)
        self.assertIn("$C$2:$C$4", s2.Formula)
        s2.ApplyDataLabels()
        self.assertEqual(s2.Points(1).DataLabel.Text, "40")  # a label shows its value (it read "")

    def test_a_new_table_column_is_column1(self):
        from yc.excel import xlSrcRange, xlYes

        ws = self.ws
        ws.Range("A1:C2").Value = [["Name", "Qty", "Price"], ["a", 1, 10]]
        lo = ws.ListObjects.Add(xlSrcRange, ws.Range("A1:C2"), XlListObjectHasHeaders=xlYes)
        lo.ListColumns.Add()
        self.assertEqual(lo.ListColumns(4).Name, "Column1")  # it was Column4

    def test_offset_resize_and_cells_past_the_last_row_raise(self):
        # they made a range below row 1,048,576 instead of Excel's error 1004
        from yc.errors import ApplicationDefinedError

        ws = self.ws
        last = ws.Range("A1048576")
        for make in (lambda: last.Offset(1, 0), lambda: ws.Range("A1048575").Resize(3, 1), lambda: ws.Cells(1048577, 1),
                     lambda: ws.Range("XFD1").Resize(1, 2), lambda: ws.Rows(5).Offset(0, 1)):
            with self.assertRaises(ApplicationDefinedError):
                make()
        self.assertEqual(ws.Range("A1048575").Offset(1, 0).Address, "$A$1048576")
        self.assertEqual(ws.Cells(1048576, 16384).Address, "$XFD$1048576")
        ws.RowLimit = 2000000  # a sheet with more rows ([YC]): those rows are on it
        try:
            self.assertEqual(last.Offset(1, 0).Address, "$A$1048577")
        finally:
            ws.RowLimit = 1048576

    # ---- round 4 ----
    def test_collections_excel_macros_use_without_parentheses(self):
        # Excel's methods that return collections are written without () in recorded macros: PivotCaches.Create,
        # Scenarios.Add, Trendlines.Add, DataLabels.ShowValue (they raised AttributeError on a bound method)
        from yc import excel
        from yc.excel import xlDatabase, xlRowField, xlSum

        ws = self.ws
        ws.Range("A1:B4").Value = [["k", "v"], ["a", 1], ["b", 2], ["a", 3]]
        pc = excel.ActiveWorkbook.PivotCaches.Create(SourceType=xlDatabase, SourceData=ws.Range("A1:B4"))
        pt = pc.CreatePivotTable(TableDestination=ws.Range("D1"), TableName="PTr4")
        pt.PivotFields("k").Orientation = xlRowField
        pt.AddDataField(pt.PivotFields("v"), "Sum of v", xlSum)
        self.assertEqual(ws.PivotTables.Count, 1)
        self.assertEqual(ws.PivotTables("PTr4").Name, "PTr4")
        self.assertEqual(ws.PivotTables().Count, 1)  # the old way still works
        ws.Range("H1").Value = 1
        ws.Range("I1").Formula = "=H1*10"
        ws.Scenarios.Add(Name="Up", ChangingCells=ws.Range("H1"), Values=[5])
        self.assertEqual(ws.Scenarios.Count, 1)
        ws.Scenarios("Up").Show()
        self.assertEqual(ws.Range("I1").Value, 50)
        ch = ws.ChartObjects.Add(300, 10, 300, 200).Chart
        ch.SetSourceData(ws.Range("A1:B4"))
        s1 = ch.SeriesCollection(1)
        s1.Trendlines.Add()
        self.assertEqual(s1.Trendlines.Count, 1)
        self.assertEqual(s1.Trendlines().Count, 1)
        s1.DataLabels.ShowValue = True
        self.assertTrue(s1.DataLabels().ShowValue)

    # ---- round 5 ----
    def test_a_ranges_number_format_and_text_read_as_one_value(self):
        # a range's NumberFormat and Text came back as a list per cell: Excel gives one value, or Null when they differ
        ws = self.ws
        ws.Range("B1:B2").Value = 5
        self.assertEqual(ws.Range("B1:B2").NumberFormat, "General")
        self.assertEqual(ws.Range("B1:B2").Text, "5")
        ws.Range("B1").NumberFormat = "0.00"
        self.assertIsNone(ws.Range("B1:B2").NumberFormat)
        self.assertIsNone(ws.Range("B1:B2").Text)
        ws.Range("B1:B2").NumberFormat = None  # read back: nothing changes (it wrote the format "None")
        self.assertEqual((ws.Range("B1").NumberFormat, ws.Range("B2").NumberFormat), ("0.00", "General"))
        self.assertEqual(ws.Range("B1").Text, "5.00")

    def test_superscript_of_cells_that_differ_reads_none(self):
        ws = self.ws
        ws.Range("A1").Font.Superscript = True
        self.assertTrue(ws.Range("A1").Font.Superscript)
        self.assertIsNone(ws.Range("A1:A2").Font.Superscript)  # it read False
        self.assertIsNone(ws.Range("A1:A2").Font.Subscript)
        ws.Range("A1:A2").Font.Superscript = None
        self.assertTrue(ws.Range("A1").Font.Superscript)
        self.assertFalse(ws.Range("A2").Font.Superscript)

    def test_font_style_and_pattern_color_of_cells_that_differ(self):
        from yc.excel import xlGray50

        ws = self.ws
        self.assertEqual(ws.Range("A1:A2").Font.FontStyle, "Regular")
        ws.Range("A1").Font.Italic = True
        self.assertIsNone(ws.Range("A1:A2").Font.FontStyle)  # it raised KeyError
        ws.Range("A1:A2").Font.FontStyle = None
        self.assertEqual(ws.Range("A1").Font.FontStyle, "Italic")
        ws.Range("C1:C2").Interior.Pattern = xlGray50
        ws.Range("C1").Interior.PatternColor = 255
        self.assertIsNone(ws.Range("C1:C2").Interior.PatternColor)  # it read 0
        self.assertIsNone(ws.Range("C1:C2").Interior.PatternColorIndex)
        ws.Range("C1:C2").Interior.PatternColor = None  # it cleared the pattern color of every cell
        self.assertEqual(ws.Range("C1").Interior.PatternColor, 255)


def excel_union(ws, a, b):
    from yc import excel

    return excel.Union(ws.Range(a), ws.Range(b))


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class KeyboardRound4Test(unittest.TestCase):
    """Excel's keys on the sheet, typed as a person types them."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)

    def setUp(self):
        from yc import excel

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()
        self.keys("w:3")

    def tearDown(self):
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")

    def test_shift_enter_and_shift_tab_move_back(self):
        # on one cell they extended the selection instead (B4:B5, the active cell left at B5)
        from yc import excel

        self.keys("g:B5|w:3|k:s-Enter|w:3")
        self.assertEqual((excel.ActiveCell.Address, excel.Selection.Address), ("$B$4", "$B$4"))
        self.keys("g:C5|w:3|k:s-Tab|w:3")
        self.assertEqual((excel.ActiveCell.Address, excel.Selection.Address), ("$B$5", "$B$5"))
        self.keys("g:C5|w:3|k:s-Down|w:3")  # Shift+arrow still extends
        self.assertEqual((excel.ActiveCell.Address, excel.Selection.Address), ("$C$5", "$C$5:$C$6"))
        self.keys("g:B2:D4|w:3|k:s-Enter|w:3")  # in a selection: back through it, the selection kept
        self.assertEqual((excel.ActiveCell.Address, excel.Selection.Address), ("$D$4", "$B$2:$D$4"))

    def test_whole_columns_and_the_whole_sheet_read_as_excel_writes_them(self):
        from yc import excel

        self.keys("g:C3|w:2|k:c-Space|w:3")
        self.assertEqual(excel.Selection.Address, "$C:$C")  # it read $C$1:$C$1048576
        self.keys("g:C3|w:2|k:s-Space|w:3")
        self.assertEqual(excel.Selection.Address, "$3:$3")
        self.keys("g:C3|w:2|k:c-A|w:2|k:c-A|w:3")
        self.assertEqual(excel.Selection.Address, "$1:$1048576")
        self.assertEqual(self.ws.Cells.Address, "$1:$1048576")  # it read $A:$XFD
        self.assertEqual(self.ws.Range("A:XFD").Address, "$1:$1048576")
        self.assertEqual(self.ws.Range("B:D").Address, "$B:$D")

    def test_alt_down_picks_from_the_column(self):
        # Alt+Down was only written on the menu: nothing opened
        ws = self.ws
        ws.Range("B1:B3").Value = [["x"], ["y"], ["x"]]
        # x, y (each once, A to Z); the list begins on none, so Down, Down is y (Excel 16, measured by keys)
        self.keys("g:B4|w:2|k:a-Down|w:6|k:Down|w:3|k:Down|w:3|k:Enter|w:4")
        self.assertEqual(ws.Range("B4").Value, "y")
        # a cell with a validation list still opens its own list
        from yc.excel import xlValidateList

        ws.Range("D1").Validation.Add(Type=xlValidateList, Formula1="Buy,Sell")
        self.keys("g:D1|w:2|k:a-Down|w:6|k:Down|w:3|k:Down|w:3|k:Enter|w:4")  # nothing chosen yet: Buy, then Sell
        self.assertEqual(ws.Range("D1").Value, "Sell")

    def test_status_bar_counts_the_cells_shown(self):
        # hidden rows counted in the status bar's Sum, Count and Average (Excel shows the visible cells only)
        from yc import ui

        ws = self.ws
        ws.Range("A1:A4").Value = [[1], [2], [4], [8]]
        ws.Rows(2).Hidden = True
        self.keys("g:A1:A4|w:4")
        stats = ui.state()["status_stats"]
        self.assertIn("Sum: 13", stats)
        self.assertIn("Count: 3", stats)
        ws.Rows(2).Hidden = False
        self.keys("g:A1:A3|w:2|g:A1:A4|w:4")
        self.assertIn("Sum: 15", ui.state()["status_stats"])


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ChangeTargetTest(unittest.TestCase):
    """Worksheet_Change gets the Target Excel gives: one range, no repeated or overlapping areas."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="yc_target_")
        proj = os.path.join(cls.tmp, "proj")
        cls.log = os.path.join(cls.tmp, "log.jsonl")
        py = os.path.join(os.path.dirname(EXE), "runtime", "python", "python.exe")
        subprocess.run([py, "-E", "-s", "-c", f"import sys;sys.path.insert(0,r'{os.path.join(ROOT, 'python')}');from yc.cli import main;sys.exit(main())",
                        "init", proj, "--sheet", "Sheet1=Sheet1"], check=True, capture_output=True)
        with open(os.path.join(proj, "sheets", "Sheet1.py"), "a", encoding="utf-8") as f:
            f.write(f"""

def Worksheet_Change(Target):
    import json
    with open(r"{cls.log}", "a", encoding="utf-8") as fh:
        fh.write(json.dumps([Target.Address, Target.Count]) + "\\n")
""")
        book = os.path.join(proj, "Book.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            from yc import excel

            yc.connect(target=p0.pid)
            excel.ThisWorkbook.SaveAs(book)
        finally:
            p0.terminate()
            p0.wait(10)
        cls.proc = yc.launch(book, hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        t0 = time.time()
        while time.time() - t0 < 30:
            st = cls.conn.request("python.status", {})
            if st.get("ready") and st.get("workbook_open_done"):
                break
            time.sleep(0.2)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)

    def targets(self, want_count):
        deadline = time.time() + 10
        out = []
        while time.time() < deadline:
            if os.path.exists(self.log):
                with open(self.log, encoding="utf-8") as f:
                    out = [json.loads(x) for x in f if x.strip()]
                if len(out) >= want_count:
                    break
            time.sleep(0.1)
        time.sleep(0.3)
        open(self.log, "w").close()
        return out

    def test_targets_of_ctrl_enter_fill_and_sort(self):
        from yc import excel

        ws = excel.Worksheets("Sheet1")
        ws.Activate()
        self.keys("k:Escape|w:2|m:2,2|w:2")
        self.targets(0)
        self.keys("g:F1:F2|w:2|t:x|k:c-Enter|w:6")
        self.assertEqual(self.targets(1), [["$F$1:$F$2", 2]])  # it was "$F$1,$F$1:$F$2", Count 3
        ws.Range("B10").Value = 7
        self.targets(1)
        self.keys("g:B10:B12|w:2|k:c-D|w:6")
        self.assertEqual(self.targets(1), [["$B$10:$B$12", 3]])  # the range filled (it was $B$11:$B$12)
        self.keys("g:B13|w:2|k:c-D|w:6")
        self.assertEqual(self.targets(1), [["$B$13", 1]])  # one row: the row above is copied, not selected
        ws.Range("J1:J3").Value = [[3], [1], [2]]
        self.targets(1)
        ws.Range("J1:J3").Sort(Key1=ws.Range("J1"), Header=2)
        self.assertEqual(self.targets(1), [["$J$1:$J$3", 3]])  # it was "$J$1,$J$2,$J$3"


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class OpenFailureTest(unittest.TestCase):
    def test_opening_a_broken_file_beside_a_workbook_in_use_raises(self):
        # with data in this window the file opens in a new one: that window's failure came back as its blank Book1,
        # and its Save would have written over the file
        from yc import excel
        from yc.errors import ApplicationDefinedError

        tmp = tempfile.mkdtemp(prefix="yc_open_fail_")
        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=proc.pid)
            excel.ActiveSheet.Range("A1").Value = "in use"
            bad = os.path.join(tmp, "bad.xlsx")
            with open(bad, "wb") as f:
                f.write(b"not a workbook")
            with self.assertRaises(ApplicationDefinedError) as cm:
                excel.Workbooks.Open(bad)
            self.assertIn("ZIP", str(cm.exception))
            # the failed window closes: no blank window is left behind (other YCGrids may run beside this test)
            from yc.excel import _other_instances

            def failed_windows():
                return [i for i in _other_instances() if os.path.normcase(i.get("open_failed") or "") == os.path.normcase(bad)]

            deadline = time.monotonic() + 15
            while failed_windows() and time.monotonic() < deadline:
                time.sleep(0.2)
            self.assertEqual(failed_windows(), [])
            with open(bad, "rb") as f:
                self.assertEqual(f.read(), b"not a workbook")
            self.assertEqual(excel.ActiveSheet.Range("A1").Value, "in use")  # still talking to this window
        finally:
            proc.terminate()
            proc.wait(10)
            shutil.rmtree(tmp, ignore_errors=True)


class ClosedWindowTest(unittest.TestCase):
    """No YCGrid needed: another window whose connection is gone."""

    def test_a_window_closed_while_asked_is_left_out(self):
        # Workbooks.Open asks each other window for its file: one closed meanwhile raised YcConnectionError out of
        # Open (parallel tests saw it whenever another test's YCGrid exited at that moment)
        from yc import excel
        from yc._conn import YcConnectionError

        class Gone:
            closed = False

            def request(self, *a, **k):
                raise YcConnectionError()

        self.assertIsNone(excel._workbook_names(excel.Workbook(Gone(), 4321)))


if __name__ == "__main__":
    unittest.main()
