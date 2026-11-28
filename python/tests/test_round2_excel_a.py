# SPDX-License-Identifier: Apache-2.0
"""Round 2, cells / formatting / formulas / Data tab work like Excel, each checked the way a
person does it: the mouse, the keys and the dialogs. Every test fails without its fix.

Run: python -m unittest tests.test_round2_excel_a (with the bundled Python, from python/).
"""
from __future__ import annotations

import datetime as _dt
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


class _Base(unittest.TestCase):
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

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        excel.Application.CutCopyMode = False
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:2")

    def tearDown(self):
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 15000}, timeout=30)
        time.sleep(0.25)

    def state(self):
        from yc import ui

        return ui.state()

    def rect(self, addr):
        from yc import ui

        return ui.cell_rect(addr)

    def center(self, addr):
        r = self.rect(addr)
        return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)

    def items(self, window=""):
        from yc import ui

        return ui.items(window)

    def click(self, label, window="", nth=0, by="label"):
        hits = [i for i in self.items(window) if i[by] == label]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        self.assertGreater(len(hits), nth, f"{label!r} isn't on the screen")
        r = hits[nth]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def labels(self, window=""):
        return [i["label"] for i in self.items(window)]

    def cursor_at(self, x, y):
        self.keys(f"m:{x},{y}|w:4")
        return self.state().get("cursor")


class MouseTest(_Base):
    def test_excel_cursors_over_cells_headers_fill_handle_and_border(self):
        # L8: the default arrow over cells, a four-headed arrow on the fill handle and a hand on the border
        self.keys("g:B2|w:3")
        x, y = self.center("D5")
        self.assertEqual(self.cursor_at(x, y), "cell", "the thick white cross over cells")
        r = self.rect("B2")
        self.assertEqual(self.cursor_at(int(r[2]), int(r[3])), "fill", "the thin black cross on the fill handle")
        self.assertEqual(self.cursor_at(int((r[0] + r[2]) / 2), int(r[1])), "move", "the four-headed arrow on the border")
        c = self.rect("D1")
        self.assertEqual(self.cursor_at(int((c[0] + c[2]) / 2), int(c[1]) - 8), "column_header", "the down arrow over a column header")
        a = self.rect("A6")
        self.assertEqual(self.cursor_at(int(a[0]) - 14, int((a[1] + a[3]) / 2)), "row_header", "the right arrow over a row header")
        self.assertEqual(self.cursor_at(int(c[2]), int(c[1]) - 8), "resize_ew", "a column boundary still resizes")

    def test_hidden_column_is_dragged_back_from_its_double_line(self):
        # L1: a hidden column had no mark, and dragging at the boundary resized the column on the left
        self.ws.Columns("C").Hidden = True
        a_width = self.ws.Columns("B").ColumnWidth
        d = self.rect("D1")
        x, y = int(d[0]), int(d[1]) - 8  # the A|D boundary in the header row, B hidden
        self.assertEqual(self.cursor_at(x + 2, y), "column_unhide", "the split cursor just right of the double line")
        self.keys(f"m:{x + 2},{y}|w:4|d:{x + 2},{y},{x + 70},{y}|w:6")
        self.assertFalse(self.ws.Columns("C").Hidden, "the drag unhides C")
        self.assertGreater(self.ws.Columns("C").ColumnWidth, 5)
        self.assertEqual(self.ws.Columns("B").ColumnWidth, a_width, "B keeps its width")

    def test_hidden_row_comes_back_with_a_double_click(self):
        # L1: a double-click just below hidden rows' line unhides the last of them (fitted to its contents)
        self.ws.Range("A3").Value = "back"
        self.ws.Rows(3).Hidden = True
        r = self.rect("A4")
        x, y = int(r[0]) - 14, int(r[1]) + 2
        self.assertEqual(self.cursor_at(x, y), "row_unhide")
        self.keys(f"m:{x},{y}|w:4|D:{x},{y}|w:6")
        self.assertFalse(self.ws.Rows(3).Hidden)
        self.assertGreater(self.ws.Rows(3).RowHeight, 5)


class TextToColumnsTest(_Base):
    def test_the_three_step_wizard_with_a_date_column(self):
        # L11: one page instead of Excel's three steps; no Date column format, no Advanced
        from yc import excel, ui

        self.ws.Range("A1:A2").Value = [["31/01/2026,7-"], ["15/02/2026,3"]]
        self.keys("g:A1:A2|w:2")
        ui.command("TextToColumns")
        self.keys("w:6")
        self.assertIn("Delimited", self.labels())
        self.assertNotIn("Comma", self.labels(), "step 1 only asks for the data type")
        self.keys("k:Enter|w:4")  # Next >
        self.assertIn("Comma", self.labels(), "step 2 has the delimiters")
        self.click("Comma")
        self.keys("k:Enter|w:4")  # Next >
        self.assertIn("Do not import column (skip)", self.labels(), "step 3 has the column data format")
        self.click("Date:")  # the first column is chosen
        self.click("##t2c_date", by="id")
        self.click("DMY")
        self.keys("k:Enter|w:6")  # Finish
        self.assertEqual(self.state()["dialog"], 0)
        self.assertEqual(self.ws.Range("A1").Value2, (_dt.date(2026, 1, 31) - _dt.date(1899, 12, 30)).days)
        self.assertEqual(self.ws.Range("B1").Value, -7, "a trailing minus is a negative number (Advanced's default)")
        self.assertEqual(excel.ActiveSheet.Range("B2").Value, 3)

    def test_back_returns_to_the_first_step_and_skip_leaves_a_column_out(self):
        from yc import ui

        self.ws.Range("A1").Value = "a;b;c"
        self.keys("g:A1|w:2")
        ui.command("TextToColumns")
        self.keys("w:6")
        self.click("Next >")
        self.click("Semicolon")
        self.click("< Back")
        self.assertIn("Fixed width", self.labels())
        self.click("Next >")
        self.click("Next >")
        general = sorted([i for i in self.items() if i["label"] == "General"], key=lambda i: (i["rect"][1], i["rect"][0]))
        hdr = general[1:]  # the option button first, then the preview's headers (each column's format)
        self.assertEqual(len(hdr), 3, "the preview's headers name each column's format")
        mid = hdr[1]["rect"]  # click the second column in the preview, then Do not import
        x, y = int((mid[0] + mid[2]) / 2), int((mid[1] + mid[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:4")
        self.click("Do not import column (skip)")
        self.click("Finish")
        self.assertEqual([self.ws.Range(c).Value for c in ("A1", "B1")], ["a", "c"])


class SortDialogTest(_Base):
    def _data(self):
        from yc import excel

        self.ws.Range("A1:B6").Value = [["Name", "Qty"], ["a", 3], ["b", 1], ["c", 5], ["d", 2], ["e", 4]]
        red = excel.RGB(255, 0, 0)
        self.ws.Range("B4").Interior.Color = red
        self.ws.Range("B6").Interior.Color = red
        self.keys("g:B2|w:2")

    def test_order_words_follow_the_column_and_largest_first_sorts(self):
        # the order list said "A to Z (smallest to largest)" for every column
        from yc import ui

        self._data()
        ui.command("CustomSort")
        self.keys("w:6")
        self.click("##ord0", by="id")
        labels = self.labels()
        self.assertIn("Smallest to Largest", labels)
        self.assertNotIn("A to Z", labels)
        self.click("Largest to Smallest")
        self.keys("k:Enter|w:6")
        self.assertEqual([self.ws.Range(f"B{i}").Value for i in range(2, 7)], [5, 4, 3, 2, 1])

    def test_sort_on_cell_color_puts_that_color_on_top(self):
        from yc import ui

        self._data()
        ui.command("CustomSort")
        self.keys("w:6")
        self.click("##on0", by="id")
        self.click("Cell Color")
        self.keys("k:Enter|w:6")
        self.assertEqual([self.ws.Range(f"A{i}").Value for i in range(2, 7)], ["c", "e", "a", "b", "d"],
                         "the red rows first, the others in their order")

    def test_add_copy_delete_and_move_levels(self):
        from yc import ui

        self._data()
        ui.command("CustomSort")
        self.keys("w:6")
        self.click("Copy Level")
        self.assertIn("Then by", self.labels())
        self.click("Delete Level")
        self.assertNotIn("Then by", self.labels())
        self.click("Add Level")
        self.assertIn("Then by", self.labels())
        self.keys("k:Escape|w:4")


class FormattingTest(_Base):
    def test_highlight_rule_red_border_and_custom_format(self):
        # the "with" list had no Red Border and no Custom Format...
        from yc import excel, ui

        self.ws.Range("A1:A3").Value = [[1], [5], [9]]
        self.keys("g:A1:A3|w:2")
        ui.command("CfGreater")
        self.keys("w:6|k:c-A|t:4|w:2")
        self.click("##preset", by="id")
        self.assertIn("Custom Format...", self.labels())
        self.click("Red Border")
        self.keys("k:Enter|w:6")
        fc = self.ws.Range("A1:A3").FormatConditions
        self.assertEqual(fc.Count, 1)
        self.assertEqual(fc.Item(1).Borders.Color, excel.RGB(255, 0, 0))
        self.assertIsNone(fc.Item(1).Interior.Color)
        # Custom Format... opens the rule with its own format (fill, font, border)
        self.keys("g:B1:B3|w:2")
        ui.command("CfGreater")
        self.keys("w:6|k:c-A|t:2|w:2")
        self.click("##preset", by="id")
        self.click("Custom Format...")
        self.keys("w:6")
        self.assertNotEqual(self.state()["dialog"], 0, "the rule editor is open")
        labels = self.labels()
        self.assertIn("New Formatting Rule", labels)
        self.assertIn("Strikethrough", labels, "with the rule's own format")
        self.keys("k:Escape|w:4")

    def test_font_size_is_picked_from_a_list(self):
        # Format Cells > Font: the size was a number box with +/- buttons, not Excel's list of sizes
        from yc import ui

        self.ws.Range("B2").Value = "x"
        self.keys("g:B2|w:2")
        ui.command("FormatCellsFontDialog")
        self.keys("w:6")
        self.click("14")
        self.keys("k:Enter|w:6")
        self.assertEqual(self.ws.Range("B2").Font.Size, 14)

    def test_number_format_list_shows_the_cell_in_each_format(self):
        from yc import ui

        self.ws.Range("B2").Value = 1234.5
        self.keys("g:B2|w:3")
        self.keys("k:Alt|w:3|k:H|w:3|k:N|w:6")  # Home > Number Format, by its KeyTips
        hits = [i for i in self.items() if i["label"] == "Currency"]
        if not hits:  # the KeyTips didn't reach it: click the box
            box = [i for i in ui.ribbon_items() if i["cmd"] == "NumberFormat"]
            self.assertTrue(box)
            r = box[0]["rect"]
            x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
            self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
            hits = [i for i in self.items() if i["label"] == "Currency"]
        self.assertTrue(hits, "the list is open")
        r = hits[0]["rect"]
        self.assertGreater(r[3] - r[1], 30, "each entry has the value's sample under its name")
        self.keys("k:Escape|w:3")


class SeriesTest(_Base):
    def test_date_series_stops_at_a_date(self):
        # Fill Series read the stop value with atof: "2026/1/5" was 2026
        from yc import ui

        self.ws.Range("A1").Value = "2026/1/1"
        self.keys("g:A1:A10|w:2")
        ui.command("FillSeries")
        self.keys("w:6")
        self.click("##stop", by="id")
        self.keys("t:2026/1/5|w:2|k:Enter|w:6")
        base = (_dt.date(2026, 1, 1) - _dt.date(1899, 12, 30)).days
        self.assertEqual([self.ws.Range(f"A{i}").Value2 for i in range(1, 6)], [base + k for k in range(5)])
        self.assertIn(self.ws.Range("A6").Value, (None, ""))

    def test_trend_and_autofill(self):
        from yc import ui

        self.ws.Range("A1:A3").Value = [[1], [3], [4]]
        self.keys("g:A1:A5|w:2")
        ui.command("FillSeries")
        self.keys("w:6")
        self.click("Trend")
        self.keys("k:Enter|w:6")
        self.assertAlmostEqual(self.ws.Range("A5").Value, 7.5 - 1 / 3, places=9)
        self.ws.Range("C1").Value = "Mon"
        self.keys("g:C1:C3|w:2")
        ui.command("FillSeries")
        self.keys("w:6")
        self.click("AutoFill")
        self.keys("k:Enter|w:6")
        self.assertEqual([self.ws.Range(f"C{i}").Value for i in (1, 2, 3)], ["Mon", "Tue", "Wed"])


class FormulaTest(_Base):
    def test_a_function_inserted_into_text_puts_the_equal_sign_in_front(self):
        # the "=" went in at the caret, in the middle of the text being typed
        from yc import ui

        self.keys("g:A1|w:2|t:5+|w:2")
        ui.command("InsertFn_ABS")
        self.keys("w:6|k:Enter|w:6")
        st = self.state()
        self.assertIn("edit", st)
        self.assertEqual(st["edit"]["text"].count("="), 1, st["edit"]["text"])
        self.assertTrue(st["edit"]["text"].startswith("=5+ABS("), st["edit"]["text"])
        self.keys("k:Escape|w:3")

    def test_name_manager_filter_menu(self):
        # the Name Manager had no Filter menu (names with errors, scope)
        from yc import excel, ui

        excel.ActiveWorkbook.Names.Add("GoodName", "=1")
        excel.ActiveWorkbook.Names.Add("BadName", "=1/0")
        ui.command("NameManager")
        self.keys("w:6")
        self.assertIn("GoodName", self.labels())
        self.click("Filter")
        self.click("Names with Errors")
        labels = self.labels()
        self.assertIn("BadName", labels)
        self.assertNotIn("GoodName", labels)
        self.click("Filter *")
        self.click("Clear Filter")
        self.assertIn("GoodName", self.labels())
        self.keys("k:Escape|w:4")

    def test_custom_autofilter_names_the_heading(self):
        # the dialog said "Show rows where: A" (the column letter) instead of the heading
        self.ws.Range("A1:A4").Value = [["Price"], [1], [2], [3]]
        self.keys("g:A1|w:2|x:ToggleFilter|w:4|g:A1|w:2|k:a-DownArrow|w:6")
        hits = [i for i in self.items() if i["label"] == "Number Filters"]
        self.assertTrue(hits)
        r = hits[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:10")
        self.click("Greater Than...")
        self.keys("w:4")
        labels = self.labels()
        self.assertIn("Price", labels)
        self.assertNotIn("Show rows where: A", labels)
        self.keys("k:Escape|w:4")

class GoToSpecialTest(_Base):
    def test_row_differences_from_the_dialog_and_python(self):
        # Go To Special had no Row differences / Column differences
        from yc import ui

        self.ws.Range("A1:C3").Value = [[1, 1, 2], [5, 6, 5], [7, 7, 7]]
        self.keys("g:A1:C3|w:2")
        ui.command("GoToSpecial")
        self.keys("w:6")
        self.click("Row differences")
        self.keys("k:Enter|w:6")
        from yc import excel

        self.assertEqual(set(excel.Selection.Address.replace("$", "").split(",")), {"C1", "B2"})
        diff = self.ws.Range("A1:C3").ColumnDifferences(self.ws.Range("A1"))
        self.assertEqual(diff.Count, 6)
        self.assertNotIn("A1", diff.Address.replace("$", ""))


class ValidationCirclesTest(_Base):
    def test_circle_invalid_data_and_a_corrected_cell_loses_its_circle(self):
        # Data Validation had no Circle Invalid Data / Clear Validation Circles
        from yc import ui

        self.ws.Range("A1:A3").Value = [[1], [50], [70]]
        self.ws.Range("A1:A3").Validation.Add(1, 1, 1, "1", "10")
        self.keys("g:C1|w:2")
        ui.command("CircleInvalidData")
        self.keys("w:3")
        self.assertEqual(self.state()["validation_circles"], 2)
        self.keys("g:A2|w:2|t:5|k:Enter|w:6")  # typing a valid value: its circle goes
        self.assertEqual(self.state()["validation_circles"], 1)
        ui.command("ClearValidationCircles")
        self.keys("w:3")
        self.assertEqual(self.state()["validation_circles"], 0)
        self.assertEqual(self.ws.CircleInvalid(), 1)
        self.ws.ClearCircles()
        self.assertEqual(self.state()["validation_circles"], 0)

    def test_the_input_message_floats_over_a_circle_below_it(self):
        # the input message was drawn before the circles, so the circle around the cell below crossed it
        import tempfile

        from tests.test_excel_parity import read_png
        from yc import ui

        self.ws.Range("A1:A4").Value = [[1], [2], [3], [99]]
        v = self.ws.Range("A1:A4").Validation
        v.Add(1, 1, 1, "1", "10")
        v.InputTitle = "Qty"
        v.InputMessage = "1 to 10 lots"
        ui.command("CircleInvalidData")
        self.keys("g:A3|w:6")
        box = [i for i in self.items() if (i.get("label") or "") == "Qty\n1 to 10 lots"]
        self.assertTrue(box, "the input message is on the screen while A3 is selected")
        x0, y0, x1, y1 = (int(round(v)) for v in box[-1]["rect"])
        png = os.path.join(tempfile.gettempdir(), "yc_input_msg_over_circle.png")
        ui.screenshot(png, max_width=4000)
        _, _, px = read_png(png)
        red = [(x, y) for x in range(x0 + 2, x1 - 2) for y in range(y0 + 2, y1 - 2) if px(x, y)[0] > 180 and px(x, y)[1] < 90]
        self.assertFalse(red, "no part of A4's circle shows through the input message")
        ui.command("ClearValidationCircles")
        self.keys("g:C1|w:6")
        self.assertFalse([i for i in self.items() if (i.get("label") or "") == "Qty\n1 to 10 lots"], "it goes with the selection")


class PythonMembersTest(_Base):
    def test_text_to_columns_dates_and_separators_from_python(self):
        from yc import excel

        self.ws.Range("A1").Value = "2026/12/31;1.234,5;9-"
        self.ws.Range("A1").TextToColumns(Semicolon=True, FieldInfo=[[1, excel.xlYMDFormat]], DecimalSeparator=",", ThousandsSeparator=".")
        self.assertEqual(self.ws.Range("A1").Value2, (_dt.date(2026, 12, 31) - _dt.date(1899, 12, 30)).days)
        self.assertEqual(self.ws.Range("B1").Value, 1234.5)
        self.assertEqual(self.ws.Range("C1").Value, -9)

    def test_data_series_trend_and_condition_borders(self):
        from yc import excel

        self.ws.Range("A1:A2").Value = [[2], [4]]
        self.ws.Range("A1:A4").DataSeries(Rowcol=excel.xlColumns, Type=excel.xlGrowth, Trend=True)
        self.assertAlmostEqual(self.ws.Range("A4").Value, 16.0, places=9)
        fc = self.ws.Range("B1:B3").FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "=1")
        fc.Borders.Color = excel.RGB(0, 0, 255)
        self.assertEqual(fc.Borders.Color, excel.RGB(0, 0, 255))
        fc.Borders.LineStyle = excel.xlNone
        self.assertIsNone(fc.Borders.Color)


if __name__ == "__main__":
    unittest.main()
