# SPDX-License-Identifier: Apache-2.0
"""Data tab parity with Excel: sort, AutoFilter, validation, Text to Columns, What-If,
outline and tables, each checked the way a person does it. Every test fails without its fix.

Run: python -m unittest tests.test_parity_data (with the bundled Python, from python/).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


class DataParityTest(unittest.TestCase):
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
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:2")

    def tearDown(self):
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.25)

    def dialog(self):
        from yc import ui

        return ui.state().get("dialog", 0)

    def labels(self, window=""):
        from yc import ui

        return [i["label"] for i in ui.items(window)]

    def click(self, label, window="", nth=0):
        from yc import ui

        hits = [i for i in ui.items(window) if i["label"] == label]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        self.assertGreater(len(hits), nth, f"{label!r} isn't on the screen")
        r = hits[nth]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def asked(self):
        """YCGrid's OK / Cancel question (AskYesNo) is on the screen."""
        from yc import ui

        return any(i["label"] == "OK" and i["window"].startswith("YCGrid") for i in ui.items())

    def col(self, c, n, first=1):
        return [self.ws.Range(f"{c}{i}").Value for i in range(first, first + n)]

    def last_error(self, after):
        from yc import ui

        return " ".join(e["title"] + " " + e["text"] for e in ui.errors(after=after))

    def error_mark(self):
        from yc import ui

        e = ui.errors()
        return e[-1]["id"] if e else 0

    # ---- H1 ----
    def test_sort_of_a_filtered_list_leaves_hidden_rows(self):
        self.ws.Range("A1:B7").Value = [["Name", "Tag"], ["e", "x"], ["d", "y"], ["c", "x"], ["b", "y"], ["a", "x"], ["f", "y"]]
        self.keys("g:A1|w:2|x:ToggleFilter|w:4")
        self.ws.Range("A1").AutoFilter(2, "x")
        self.keys("g:A2|w:2|x:SortAscending|w:6")
        visible = [(self.ws.Range(f"A{r}").Value, self.ws.Range(f"B{r}").Value) for r in range(2, 8) if not self.ws.Rows(r).Hidden]
        self.assertEqual(visible, [("a", "x"), ("c", "x"), ("e", "x")])
        self.assertEqual([self.ws.Range(f"A{r}").Value for r in (3, 5, 7)], ["d", "b", "f"])
        self.keys("x:ClearFilter|w:4")
        self.assertEqual(self.col("A", 6, 2), ["a", "d", "c", "b", "e", "f"])

    # ---- H2 ----
    def test_sort_and_remove_duplicates_in_a_table_keep_header_and_totals(self):
        self.ws.Range("A1:B5").Value = [["Name", "City"], ["d", "x"], ["b", "y"], ["c", "x"], ["a", "y"]]
        self.ws.Range("A1:B1").Font.Bold = True  # all text: Create Table takes a bold first row as the header (Excel)
        self.keys("g:A1|w:2|k:c-T|w:6|k:Enter|w:6")
        lo = self.ws.ListObjects(1)
        lo.ShowTotals = True
        self.keys("w:4|g:A3|w:2|x:SortDescending|w:6")
        self.assertEqual(self.col("A", 1) + self.col("B", 1), ["Name", "City"])
        self.assertEqual(self.col("A", 4, 2), ["d", "c", "b", "a"])
        self.assertTrue(self.ws.ListObjects(1).ShowTotals)
        self.assertTrue(str(self.ws.Range("B6").Formula).startswith("=SUBTOTAL"), self.ws.Range("B6").Formula)
        # rows 2 and 3 the same: Remove Duplicates deletes a table row, the totals row follows
        self.ws.Range("A3:B3").Value = [["d", "x"]]
        self.keys("g:A2|w:2|x:RemoveDuplicates|w:6|k:Enter|w:6")
        self.assertEqual(self.ws.ListObjects(1).Range.Address, "$A$1:$B$5")
        self.assertTrue(str(self.ws.Range("B5").Formula).startswith("=SUBTOTAL"), self.ws.Range("B5").Formula)

    # ---- H3 ----
    def test_sort_warning_expands_the_selection(self):
        from yc import excel

        self.ws.Range("A1:C4").Value = [["k", "v", "w"], [3, "c", 1], [1, "a", 2], [2, "b", 3]]
        self.keys("g:B2:B4|w:2|x:SortAscending|w:6")
        self.assertNotEqual(self.dialog(), 0, "Sort Warning asks first")
        self.keys("k:Enter|w:6")
        rows = [[self.ws.Range(f"{c}{r}").Value for c in "ABC"] for r in range(2, 5)]
        self.assertEqual(rows, [[1, "a", 2], [2, "b", 3], [3, "c", 1]])
        self.assertEqual(self.col("A", 1), ["k"])
        excel.Application.Undo()
        self.keys("w:4|g:B2:B4|w:2|x:SortDescending|w:6|k:DownArrow|w:2|k:Enter|w:6")
        self.assertEqual(self.col("B", 3, 2), ["c", "b", "a"])
        self.assertEqual(self.col("A", 3, 2), [3, 1, 2], "Continue with the current selection: column A stays")

    # ---- H4 ----
    def test_custom_validation_judges_the_new_value(self):
        from yc import excel

        self.ws.Range("A1").Value = 5
        self.ws.Range("A1").Validation.Add(excel.xlValidateCustom, excel.xlValidAlertStop, 1, "=ISNUMBER(A1)")
        mark = self.error_mark()
        self.keys("g:A1|w:2|t:abc|k:Enter|w:6")
        self.assertEqual(self.ws.Range("A1").Value, 5, "text is refused")
        self.assertTrue(self.last_error(mark), "the Stop alert was shown")
        self.keys("k:Escape|w:3")
        self.ws.Range("A1").Value = "abc"
        mark = self.error_mark()
        self.keys("g:A1|w:2|t:7|k:Enter|w:6")
        self.assertEqual(self.ws.Range("A1").Value, 7, "a number is accepted")
        self.assertEqual(self.last_error(mark), "")

    # ---- H5 ----
    def test_goal_seek_error_stays_and_cancel_undoes_nothing(self):
        from yc import ui

        self.keys("g:D1|w:2|t:42|k:Enter|w:4")
        self.ws.Range("B1").Value = 1
        mark = self.error_mark()
        self.keys("g:A1|w:2")
        ui.command("GoalSeek")
        self.keys("w:6")
        boxes = sorted([i for i in ui.items() if "To value" in i["id"] and i["id"].startswith("##")], key=lambda i: i["rect"][1])
        self.assertTrue(boxes)
        r = boxes[0]["rect"]
        self.keys(f"m:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:2|c:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:3|k:c-A|t:5|w:2")
        self.keys("k:Tab|w:2|k:c-A|t:B1|w:2|k:Enter|w:8")  # By changing cell
        self.assertIn("formula", self.last_error(mark))
        self.assertEqual(self.labels().count("󰉄"), 2, "Goal Seek stays on its parameters (the two range boxes)")
        self.click("Cancel")
        self.assertEqual(self.ws.Range("D1").Value, 42, "Cancel didn't undo the earlier entry")

    def test_solver_model_error_keeps_the_earlier_entry(self):
        from yc import ui

        self.keys("g:D1|w:2|t:42|k:Enter|w:4")
        self.ws.Range("A1").Formula = "=B1*2"
        self.ws.Range("C1").Formula = "=A1+1"
        mark = self.error_mark()
        self.keys("g:C1|w:2")
        ui.command("Solver")
        self.keys("w:6")
        boxes = sorted([i for i in ui.items() if i["id"] == "##rb"], key=lambda i: i["rect"][1])
        self.assertGreaterEqual(len(boxes), 2)
        r = boxes[1]["rect"]  # Set Objective, then By Changing Variable Cells
        self.keys(f"m:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:2|c:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:3|k:c-A|t:$A$1|w:2")
        self.click("Solve")
        self.keys("w:6")
        self.assertIn("formula", self.last_error(mark).lower())
        self.assertIn("Solve", self.labels(), "the parameters stay open")
        self.click("Close")
        self.assertEqual(self.ws.Range("D1").Value, 42)
        self.assertEqual(self.ws.Range("A1").Formula, "=B1*2")

    # ---- H6 ----
    def test_date_and_number_criteria(self):
        from yc import excel

        self.ws.Range("A1:B5").Value = [["Date", "Qty"], [None, 500], [None, 1500], [None, 800], [None, 2500]]
        for r, d in ((2, "2023/10/1"), (3, "2024/1/10"), (4, "2024/1/20"), (5, "2024/3/1")):
            self.ws.Range(f"A{r}").Formula = d
        self.ws.Range("A2:A5").NumberFormat = "m/d/yyyy"
        self.ws.Range("A1").AutoFilter(1, ">1/15/2024")
        self.assertEqual([r for r in range(2, 6) if not self.ws.Rows(r).Hidden], [4, 5])
        self.ws.ShowAllData()
        self.ws.Range("A1").AutoFilter(1)  # clear the date column's criterion
        self.ws.Range("A1").AutoFilter(2, ">1,000", excel.xlAnd)
        self.assertEqual([r for r in range(2, 6) if not self.ws.Rows(r).Hidden], [3, 5])

    # ---- M1, M5, L1 ----
    def test_filter_list_follows_other_columns_and_new_rows(self):
        self.ws.Range("A1:C5").Value = [["Fruit", "Color", "Qty"], ["Apple", "Red", 3], ["Pear", "Green", 1], ["apple", "Yellow", 2], ["Pear", "Brown", 5]]
        self.keys("g:A1|w:2|x:ToggleFilter|w:4")
        self.ws.Range("A1").AutoFilter(1, "Apple")
        self.keys("g:B1|w:2|k:a-DownArrow|w:6")
        got = self.labels()
        self.assertIn("Red", got)
        self.assertIn("Yellow", got)
        self.assertNotIn("Green", got)
        self.assertNotIn("Brown", got)
        self.assertIn("Sort A to Z", got)
        self.keys("k:Escape|w:3|g:C1|w:2|k:a-DownArrow|w:6")
        self.assertIn("Sort Smallest to Largest", self.labels())
        self.keys("k:Escape|w:3")
        # a row typed below the list belongs to it
        self.ws.Range("A1").AutoFilter(1)
        self.keys("g:A6|w:2|t:Kiwi|k:Tab|t:Gold|k:Enter|w:4|g:A1|w:2|k:a-DownArrow|w:6")
        self.assertIn("Kiwi", self.labels())
        self.click("Kiwi")
        self.click("OK")
        self.assertTrue(self.ws.Rows(6).Hidden)

    # ---- M2 ----
    def test_filter_ok_is_greyed_with_nothing_ticked(self):
        from yc import ui

        self.ws.Range("A1:A4").Value = [["k"], ["a"], ["b"], ["c"]]
        self.keys("g:A1|w:2|x:ToggleFilter|w:4|g:A1|w:2|k:a-DownArrow|w:6")
        self.click("(Select All)")
        ok = [i for i in ui.items() if i["label"] == "OK"]
        self.assertTrue(ok and ok[0]["disabled"], ok)
        self.click("OK")
        self.assertFalse(self.ws.Rows(2).Hidden)
        self.assertIn("(Select All)", self.labels(), "the menu stays open")

    # ---- M3 ----
    def test_the_sort_is_kept_and_reapplied(self):
        from yc import excel

        self.ws.Range("A1:B6").Value = [["k", "v"], ["a", 1], ["b", 2], ["c", 3], ["d", 4], ["e", 5]]
        self.keys("g:A1|w:2|x:ToggleFilter|w:4|g:B1|w:2|k:a-DownArrow|w:6")
        self.click("Sort Largest to Smallest")
        self.assertEqual(self.col("B", 5, 2), [5, 4, 3, 2, 1])
        f = self.ws.AutoFilter.Sort.SortFields
        self.assertEqual(f.Count, 1)
        self.assertEqual(f(1).Order, excel.xlDescending)
        self.assertEqual(f(1).Key.Column, 2)
        self.ws.Range("B4").Value = 99
        self.keys("g:A3|w:2|k:c-a-L|w:6")
        self.assertEqual(self.ws.Range("B2").Value, 99, "Reapply sorts again")

    # ---- M6 ----
    def test_text_length_counts_characters(self):
        from yc import excel

        self.ws.Range("B1").Validation.Add(excel.xlValidateTextLength, excel.xlValidAlertStop, excel.xlLessEqual, "4")
        mark = self.error_mark()
        self.keys("g:B1|w:2|t:\u53f0\u7a4d\u96fb|k:Enter|w:6")
        self.assertEqual(self.ws.Range("B1").Value, "\u53f0\u7a4d\u96fb")
        self.assertEqual(self.last_error(mark), "")

    # ---- M7 ----
    def test_validation_on_a_mixed_selection_asks(self):
        from yc import excel

        self.ws.Range("B1").Validation.Add(excel.xlValidateWholeNumber, excel.xlValidAlertStop, excel.xlBetween, "1", "10")
        self.keys("g:B1:B2|w:2|x:DataValidation|w:6")
        self.assertTrue(self.asked(), "extend Data Validation to the other cells?")
        self.keys("k:Enter|w:6")
        self.assertIn("Ignore blank", self.labels(), "Yes opens Data Validation with the rule")
        self.keys("k:Escape|w:4")
        self.ws.Range("B2").Validation.Add(excel.xlValidateList, excel.xlValidAlertStop, 1, "a,b")
        self.keys("g:B1:B2|w:2|x:DataValidation|w:6")
        self.assertTrue(self.asked(), "more than one type of validation")
        self.keys("k:Escape|w:4")

    # ---- M8 ----
    def test_text_to_columns_asks_before_writing_over_data(self):
        from yc import ui

        self.ws.Range("A1").Value = "a,b"
        self.ws.Range("B1").Value = "keep"
        self.keys("g:A1|w:2")
        ui.command("TextToColumns")
        self.keys("w:6")
        self.click("Next >")  # step 2 of Excel's wizard has the delimiters
        self.click("Comma")
        self.click("Finish")
        self.assertTrue(self.asked(), "There's already data here")
        self.keys("k:Escape|w:4")
        self.assertEqual(self.ws.Range("B1").Value, "keep")

    # ---- M10 ----
    def test_outline_settings_summary_above_and_auto_outline(self):
        from yc import excel

        self.keys("g:A2:A4|w:2|x:GroupOutline|w:4|k:Enter|w:4")
        self.assertEqual(self.ws.Range("A3").EntireRow.OutlineLevel, 2)
        self.keys("x:OutlineSettings|w:6")
        self.click("Summary rows below detail")
        self.click("OK")
        self.assertEqual(self.ws.Outline.SummaryRow, excel.xlSummaryAbove)
        self.keys("g:A1|w:2|x:HideDetail|w:4")
        self.assertTrue(self.ws.Rows(3).Hidden, "the summary row above the group hides it")
        self.keys("x:ShowDetail|w:4")
        self.assertFalse(self.ws.Rows(3).Hidden)
        # Auto Outline: a SUM below the numbers
        ws2 = excel.Worksheets.Add()
        ws2.Activate()
        ws2.Range("A1:A3").Value = [[1], [2], [3]]
        ws2.Range("A4").Formula = "=SUM(A1:A3)"
        self.keys("w:2|g:A1|w:2|x:AutoOutline|w:4")
        self.assertEqual([ws2.Range(f"A{r}").EntireRow.OutlineLevel for r in range(1, 5)], [2, 2, 2, 1])

    # ---- M4 ----
    def test_every_table_has_its_own_filter_buttons(self):
        from yc import ui

        self.ws.Range("A1:B5").Value = [["k", "n"], ["x", 1], ["y", 2], ["x", 3], ["y", 4]]
        self.ws.Range("D1:E5").Value = [["v", "w"], [10, "b"], [20, "c"], [30, "d"], [40, "e"]]
        self.keys("g:A1|w:2|k:c-T|w:6|k:Enter|w:6|g:D1|w:2|k:c-T|w:6|k:Enter|w:6")
        self.assertEqual(self.ws.ListObjects.Count, 2)
        btns = [i for i in ui.items("") if i["id"] == "##flt"]
        self.assertEqual(len(btns), 4, "both header rows have their buttons")
        self.ws.ListObjects(1).Range.AutoFilter(1, "x")
        self.ws.ListObjects(2).Range.AutoFilter(1, ">25")
        self.assertEqual([r for r in range(2, 6) if self.ws.Rows(r).Hidden], [2, 3, 5])
        self.ws.ListObjects(2).AutoFilter.ShowAllData()
        self.assertEqual([r for r in range(2, 6) if self.ws.Rows(r).Hidden], [3, 5], "the first table's filter stays")
        # the second table's menu filters the second table
        self.keys("g:D1|w:2|k:a-DownArrow|w:6")
        self.assertIn("20", self.labels())
        self.click("20")
        self.click("OK")
        self.assertTrue(self.ws.Rows(3).Hidden and self.ws.Rows(5).Hidden)
        self.assertEqual(len(self.ws.ListObjects(2).AutoFilter.Filters), 1)

    # ---- L3 ----
    def test_filter_button_tooltip_names_the_header(self):
        from yc import ui

        self.ws.Range("A1:B3").Value = [["Name", "Qty"], ["a", 1], ["b", 7]]
        self.keys("g:A1|w:2|x:ToggleFilter|w:4")
        self.ws.Range("A1").AutoFilter(2, ">5")
        btns = sorted([i for i in ui.items("") if i["id"] == "##flt"], key=lambda i: i["rect"][0])
        self.assertEqual(len(btns), 2)
        r = btns[1]["rect"]
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:6")
        tips = [lb for lb in self.labels() if lb.startswith("Qty:")]
        self.assertTrue(tips, self.labels())
        self.assertIn('Greater Than "5"', tips[0])
        self.keys("m:2,2|w:2")

    # ---- L4 ----
    def test_cell_menu_sort_has_custom_sort(self):
        self.ws.Range("A1:A3").Value = [[3], [1], [2]]
        self.keys("g:A2|w:2|k:s-F10|w:4")
        self.click("Sort")
        self.assertIn("Custom Sort...", self.labels())

    # ---- L5 ----
    def test_totals_row_list_opens_with_alt_down(self):
        self.ws.Range("A1:B3").Value = [["k", "v"], ["a", 1], ["b", 2]]
        self.keys("g:A1|w:2|k:c-T|w:6|k:Enter|w:6")
        self.ws.ListObjects(1).ShowTotals = True
        self.keys("w:4|g:B4|w:2|k:a-DownArrow|w:6")
        got = self.labels()
        self.assertIn("More Functions...", got)
        self.assertIn("Sum", got)

    # ---- L13 ----
    def test_list_items_show_hash_text(self):
        from yc import excel

        self.ws.Range("B1").Validation.Add(excel.xlValidateList, excel.xlValidAlertStop, 1, "C##1,D")
        self.keys("g:B1|w:2|k:a-DownArrow|w:6")
        self.assertIn("C##1", self.labels())


if __name__ == "__main__":
    unittest.main()
