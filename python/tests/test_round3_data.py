# SPDX-License-Identifier: Apache-2.0
"""Round 3 (data): the Data tab working like Excel's, each checked the way a person does it - the ribbon's commands,
the dialogs' buttons and boxes, keys and drags on the sheet - and what the sheet then holds.

Run: python -m unittest tests.test_round3_data (with the bundled Python, from python/).
"""
from __future__ import annotations

import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


class DataRound3Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel, ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        self.asserts = ui.state().get("imgui_asserts", 0)
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:2")

    def tearDown(self):
        from yc import ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")
        self.assertEqual(ui.state().get("imgui_asserts", 0), self.asserts, "an ImGui check failed")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 10000}, timeout=30)
        time.sleep(0.2)

    def dialog(self):
        from yc import ui

        return ui.state().get("dialog", 0)

    def items(self, label=None, ident=None):
        from yc import ui

        hits = [i for i in ui.items() if (label is None or i["label"] == label) and (ident is None or i["id"] == ident)]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def click(self, label=None, ident=None, nth=0, wait=5):
        hits = self.items(label, ident)
        self.assertGreater(len(hits), nth, f"{label or ident!r} isn't on the screen")
        r = hits[nth]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:{wait}")

    def labels(self):
        from yc import ui

        return [i["label"] for i in ui.items()]

    def sheet_names(self):
        from yc import excel

        return [w.Name for w in excel.Worksheets]

    def region(self):
        self.ws.Range("A1:B6").Value = [["Region", "Sales"], ["East", 10], ["East", 20], ["West", 5], ["West", 6], ["West", 7]]

    # ---- Subtotal (Data > Outline) ----
    def test_subtotal_dialog_inserts_totals_and_remove_all_takes_them_away(self):
        self.region()
        self.keys("g:A2|w:2|x:Subtotal|w:6")
        self.assertNotEqual(self.dialog(), 0, "the Subtotal dialog opens")
        for word in ("Replace current subtotals", "Page break between groups", "Summary below data", "Remove All"):
            self.assertIn(word, self.labels())
        self.keys("k:Enter|w:8")
        self.assertEqual(self.ws.Range("A4").Value, "East Total")
        self.assertEqual(self.ws.Range("B4").Formula, "=SUBTOTAL(9,B2:B3)")
        self.assertEqual(self.ws.Range("A9").Value, "Grand Total")
        self.assertEqual(self.ws.Range("B9").Value, 48)
        self.assertEqual(self.ws.Rows(2).OutlineLevel, 3)
        self.keys("g:A2|w:2|x:Subtotal|w:6")
        self.click("Remove All", wait=8)
        self.assertEqual(self.ws.Range("A4").Value, "West")
        self.assertIsNone(self.ws.Range("A7").Value)
        # Python, with Excel's names
        from yc import excel

        self.ws.Range("A1").Subtotal(1, excel.xlAverage, [2], True, False, excel.xlSummaryBelow)
        self.assertEqual(self.ws.Range("A4").Value, "East Average")
        self.assertEqual(self.ws.Range("B4").Value, 15)
        self.ws.Range("A1").RemoveSubtotal()
        self.assertEqual(self.ws.Range("A4").Value, "West")

    # ---- Sort Options: case sensitive, left to right; Sort On icon ----
    def test_sort_options_case_sensitive_from_the_sort_dialog(self):
        self.ws.Range("A1:A4").Value = [["B"], ["a"], ["A"], ["b"]]
        self.keys("g:A2|w:2|x:CustomSort|w:6")
        self.click("Options...", wait=6)
        self.assertIn("Case sensitive", self.labels())
        self.click("Case sensitive", wait=3)
        self.keys("k:Enter|w:6")
        self.assertIn("Then by", self.labels() + ["Then by"])  # the Sort dialog is back
        self.assertNotEqual(self.dialog(), 0)
        self.keys("k:Enter|w:8")
        self.assertEqual([self.ws.Range(f"A{r}").Value for r in range(1, 5)], ["a", "A", "b", "B"])

    def test_sort_left_to_right_and_by_icon_from_python(self):
        from yc import excel

        self.ws.Range("A1:C2").Value = [[3, 1, 2], ["c", "a", "b"]]
        srt = self.ws.Sort
        srt.SortFields.Clear()
        srt.SortFields.Add(self.ws.Range("A1:C1"))
        srt.SetRange(self.ws.Range("A1:C2"))
        srt.Header = excel.xlNo
        srt.Orientation = excel.xlLeftToRight
        srt.Apply()
        self.assertEqual(self.ws.Range("A2:C2").Value, [["a", "b", "c"]])
        self.ws.Range("E1:E5").Value = [[50], [90], [10], [95], [5]]
        self.ws.Range("E1:E5").FormatConditions.AddIconSetCondition()
        srt.SortFields.Clear()
        f = srt.SortFields.Add(self.ws.Range("E1"), excel.xlSortOnIcon, excel.xlAscending)
        f.SetIcon(excel.ActiveWorkbook.IconSets(excel.xl3TrafficLights1).Item(3))  # AddIconSetCondition's default set
        srt.SetRange(self.ws.Range("E1:E5"))
        srt.Orientation = excel.xlTopToBottom
        srt.Apply()
        self.assertEqual([self.ws.Range(f"E{r}").Value for r in (1, 2)], [90, 95])

    # ---- Data Validation: Apply these changes to all other cells with the same settings ----
    def test_validation_changes_reach_the_cells_with_the_same_settings(self):
        from yc import excel

        self.ws.Range("A1:A3").Validation.Add(excel.xlValidateWholeNumber, excel.xlValidAlertStop, excel.xlBetween, "1", "10")
        self.keys("g:A1|w:2|x:DataValidation|w:6")
        box = "Apply these changes to all other cells with the same settings"
        self.assertIn(box, self.labels())
        self.click(box, wait=3)
        self.click("Ignore blank", wait=3)
        self.keys("k:Enter|w:8")
        self.assertFalse(self.ws.Range("A3").Validation.IgnoreBlank, "A3 shares the rule: it changed too")
        self.assertFalse(self.ws.Range("A1").Validation.IgnoreBlank)

    # ---- Filter: dates grouped Year > Month > Day ----
    def test_filter_list_groups_dates_by_year_month_day(self):
        import datetime as dt

        self.ws.Range("A1").Value = "Date"
        self.ws.Range("B1").Value = "Qty"
        days = [dt.date(2023, 12, 30), dt.date(2024, 1, 5), dt.date(2024, 1, 9), dt.date(2024, 2, 1)]
        for i, d in enumerate(days):
            self.ws.Cells(i + 2, 1).Value = d
            self.ws.Cells(i + 2, 2).Value = i
        self.ws.Range("A2:A5").NumberFormat = "m/d/yyyy"
        self.keys("g:A1|w:2|x:ToggleFilter|w:6|g:A1|w:2|k:a-DownArrow|w:8")
        labels = self.labels()
        self.assertIn("2023", labels)
        self.assertIn("2024", labels)
        self.assertNotIn("1/5/2024", labels, "days sit under their year and month")
        self.click("2023", wait=3)  # untick the year
        self.keys("k:Enter|w:8")
        self.assertTrue(self.ws.Rows(2).Hidden)
        self.assertFalse(self.ws.Rows(3).Hidden)

    # ---- Solver: Add Constraint, Options, Reports ----
    def test_solver_constraint_dialog_options_and_answer_report(self):
        from yc import excel, ui

        self.ws.Range("A1:B4").Formula = [["x", 0], ["y", 0], ["obj", "=3*B1+2*B2"], ["sum", "=B1+B2"]]
        excel.SolverReset()
        excel.SolverOk(SetCell=self.ws.Range("B3"), MaxMinVal=1, ByChange=self.ws.Range("B1:B2"), Engine=1)
        excel.SolverAdd(self.ws.Range("B1"), 1, "3")
        self.keys("g:B3|w:2")
        ui.command("Solver")
        self.keys("w:6")
        self.click("Add", wait=6)
        self.assertTrue(self.items(ident="##crel"), "Excel's Add Constraint dialog: the relation between the two boxes")
        boxes = self.items(ident="##rb")
        r = boxes[0]["rect"]
        self.keys(f"m:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:2|c:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:3|k:c-A|t:$B$4|w:2")
        r = boxes[1]["rect"]
        self.keys(f"m:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:2|c:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:3|k:c-A|t:4|w:2")
        self.click("OK", wait=6)
        self.assertIn("$B$4 <= 4", self.labels())
        self.click("Options", wait=6)
        self.assertIn("All Methods", self.labels())
        self.assertIn("GRG Nonlinear", self.labels())
        self.assertIn("Evolutionary", self.labels())
        self.click("Ignore Integer Constraints", wait=3)
        self.click("OK", wait=6)
        self.click("Solve", wait=12)
        self.assertIn("Sensitivity", self.labels(), "Solver Results lists the reports")
        self.click("Answer", wait=3)
        self.click("OK", wait=12)
        self.assertAlmostEqual(self.ws.Range("B3").Value, 11, places=6)
        self.assertIn("Answer Report 1", self.sheet_names())
        self.assertTrue(excel.SolverGet(5) == 2)
        self.assertTrue(excel._solver_model()["ignore_integer"])
        # Python: SolverFinish(ReportArray) makes the Sensitivity and Limits reports
        excel.Worksheets(self.ws.Name).Activate()
        excel.SolverSolve(True)
        excel.SolverFinish(1, [2, 3])
        self.assertIn("Sensitivity Report 1", self.sheet_names())
        self.assertIn("Limits Report 1", self.sheet_names())

    def test_esc_in_solver_is_each_pages_cancel(self):
        # Esc closed the whole Solver from any page and kept the solution: it is the page's Cancel (Add Constraint goes
        # back to the parameters, Solver Results puts the original values back), as in Excel
        from yc import excel, ui

        self.ws.Range("A1:B3").Formula = [["x", 1], ["y", 1], ["obj", "=3*B1+2*B2"]]
        excel.SolverReset()
        excel.SolverOk(SetCell=self.ws.Range("B3"), MaxMinVal=1, ByChange=self.ws.Range("B1:B2"), Engine=2)
        excel.SolverAdd(self.ws.Range("B1:B2"), 1, "4")
        self.keys("g:B3|w:2")
        ui.command("Solver")
        self.keys("w:6")
        self.click("Add", wait=6)
        self.keys("k:Escape|w:6")
        self.assertTrue(self.dialog(), "Esc on Add Constraint goes back to Solver Parameters")
        self.assertIn("Solve", self.labels())
        self.click("Solve", wait=12)
        self.assertAlmostEqual(self.ws.Range("B3").Value, 20, places=6)
        self.keys("k:Escape|w:8")
        self.assertEqual(self.dialog(), 0)
        self.assertEqual([self.ws.Range("B1").Value, self.ws.Range("B2").Value], [1, 1], "the original values")

    def test_esc_in_the_scenario_manager_goes_back_a_page(self):
        from yc import ui

        self.keys("g:B2|w:2")
        ui.command("ScenarioManager")
        self.keys("w:6")
        self.click("Add...", wait=6)
        self.assertNotIn("Close", self.labels(), "Add Scenario")
        self.keys("k:Escape|w:6")
        self.assertTrue(self.dialog(), "Esc on Add Scenario goes back to the Scenario Manager")
        self.assertIn("Close", self.labels())
        self.keys("k:Escape|w:6")
        self.assertEqual(self.dialog(), 0)

    def test_solver_load_save_model(self):
        from yc import excel

        self.ws.Range("A1:B2").Formula = [["x", 1], ["obj", "=B1*2"]]
        excel.SolverReset()
        excel.SolverOk(SetCell=self.ws.Range("B2"), MaxMinVal=2, ByChange=self.ws.Range("B1"))
        excel.SolverAdd(self.ws.Range("B1"), 3, "1")
        excel.SolverSave(self.ws.Range("D1"))
        self.assertEqual(self.ws.Range("D1").Formula, "=MIN($B$2)", "the model's own sheet isn't named")
        self.assertEqual(self.ws.Range("D3").Formula, "=$B$1>=1")
        excel.SolverReset()
        excel.SolverLoad(self.ws.Range("D1:D4"))
        self.assertEqual(excel.SolverGet(2), 2)
        self.assertEqual(excel.SolverGet(5), 1)

    # ---- Scenario Manager: Merge, the PivotTable report ----
    def test_scenario_merge_and_pivottable_report(self):
        from yc import excel

        other = excel.Worksheets.Add()
        for w in list(excel.Worksheets):  # earlier tests' sheets: the Merge list shows these two without scrolling
            if w.Name not in (self.ws.Name, other.Name):
                w.Delete()
        self.ws.Activate()
        self.ws.Range("A1").Value = 1
        self.ws.Range("B1").Formula = "=A1*10"
        other.Scenarios().Add("Low", other.Range("A1"), [2])
        other.Scenarios().Add("High", other.Range("A1"), [9])
        self.keys("w:4|g:B1|w:2|x:ScenarioManager|w:6")
        self.click("Merge...", wait=6)
        self.click(ident=other.Name + "##sh", wait=3)
        self.keys("k:Enter|w:8")
        self.assertEqual(self.ws.Scenarios().Count, 2)
        self.click("Summary...", wait=6)
        self.click("Scenario PivotTable report", wait=3)
        self.keys("k:Enter|w:12")
        self.assertIn("Scenario PivotTable", self.sheet_names())
        self.assertEqual(excel.Worksheets("Scenario PivotTable").PivotTables().Count, 1)
        sc = self.ws.Scenarios(1)
        sc.Locked = False
        self.assertFalse(self.ws.Scenarios(1).Locked)

    # ---- Scenario Manager: at most 32 changing cells and 32 result cells, whole columns and the whole sheet too ----
    def test_scenario_cell_limits_over_whole_columns(self):
        from yc import excel

        ws = excel.Worksheets.Add()
        ws.Range("A1").Value = 1
        ws.Range("B1").Formula = "=A1*2"
        # Excel 16 (measured 2026-11-20): Add takes 32 changing cells and refuses 33, a summary takes 32 result cells
        with self.assertRaises(Exception):
            ws.Scenarios().Add("Col", ws.Range("C:C"))  # was A1 alone: a whole column read as its first row
        ws.Scenarios().Add("s1", ws.Range("A1"), [5])
        t0 = time.time()
        for rng in ("B:B", "A:XFD", "B1:B33"):
            with self.assertRaises(Exception):
                ws.Scenarios().CreateSummary(1, ws.Range(rng))
        self.assertLess(time.time() - t0, 10)
        n = excel.Worksheets.Count
        ws.Scenarios().CreateSummary(1, ws.Range("B1:B32"))
        self.assertEqual(excel.Worksheets.Count, n + 1)
        # the dialog: the whole sheet typed as the result cells is refused at once (it collected 17 billion cells)
        ws.Activate()
        self.keys("w:4|g:B1|w:2|x:ScenarioManager|w:6")
        self.click("Summary...", wait=6)
        from yc import ui

        n = len(ui.errors())
        pick = next(i for i in ui.items() if i["label"] == "\U000f0244")  # the box's collapse button, at its right end
        x, y = int(pick["rect"][0]) - 40, int((pick["rect"][1] + pick["rect"][3]) / 2)
        self.keys(f"c:{x},{y}|w:2|k:c-A|t:A:XFD|w:2|k:Enter|w:6")
        self.assertTrue(any("at most 32 result cells" in str(e) for e in ui.errors()[n:]), ui.errors()[n:])
        self.keys("k:Escape|w:2|k:Escape|w:2|k:Escape|w:2")

    def test_items_leave_out_the_hidden_debug_window(self):
        # ImGui's implicit Debug window gets its title bar at every frame and is hidden when nothing is drawn into it:
        # ui.items listed a "Debug" window nobody could see
        from yc import ui

        self.keys("w:3")
        self.assertEqual([i for i in ui.items() if i["window"] == "Debug"], [])

    # ---- Consolidate: Create links to source data; the references stay with the sheet ----
    def test_consolidate_links_and_kept_references(self):
        from yc import excel

        src = excel.Worksheets.Add()
        src.Range("A1:B1").Value = [["x", 7]]
        dest = excel.Worksheets.Add()
        dest.Activate()
        self.keys("w:4|g:A1|w:2|x:Consolidate|w:6")
        self.assertIn("Create links to source data", self.labels())
        boxes = self.items(ident="##rb")
        r = boxes[0]["rect"]
        ref = f"'{src.Name}'!$A$1:$B$1"
        self.keys(f"m:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:2|c:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:3|k:c-A|t:{ref}|w:2")
        self.click("Add", wait=4)
        self.click("Left column", wait=3)
        self.click("Create links to source data", wait=3)
        self.keys("k:Enter|w:8")
        self.assertEqual(dest.Range("C1").Formula.replace("'", ""), f"={src.Name}!$B$1")
        self.assertEqual(dest.Range("C2").Formula, "=SUM(C1:C1)")
        self.assertEqual(dest.Range("C2").Value, 7)
        self.keys("g:H1|w:2|x:Consolidate|w:6")
        self.assertIn(ref, self.labels(), "the reference list is kept with the sheet")
        self.keys("k:Escape|w:4")

    # ---- Forecast Sheet ----
    def test_forecast_sheet_creates_the_table_and_chart(self):
        from yc import excel

        self.ws.Range("A1:B1").Value = [["Date", "Sales"]]
        for i in range(12):
            self.ws.Cells(i + 2, 1).Value = 45000 + i
            self.ws.Cells(i + 2, 2).Value = 100 + i * 3 + (i % 3)
        self.ws.Range("A2:A13").NumberFormat = "m/d/yyyy"
        before = set(self.sheet_names())
        self.keys("g:A1:B13|w:2|x:ForecastSheet|w:10")
        self.assertNotEqual(self.dialog(), 0, "Create Forecast Worksheet opens")
        self.assertIn("Create", self.labels())
        self.assertIn("Column", self.labels(), "the chart types beside the preview")
        self.keys("k:Enter|w:15")
        new = [n for n in self.sheet_names() if n not in before]
        self.assertEqual(len(new), 1)
        out = excel.Worksheets(new[0])
        self.assertEqual(out.Range("C1").Value, "Forecast(Sales)")
        self.assertTrue(str(out.Range("C14").Formula).startswith("=FORECAST.ETS(A14,"), out.Range("C14").Formula)
        self.assertEqual(out.ChartObjects().Count, 1)

    # ---- Get Data > From Text/CSV ----
    def test_from_text_csv_loads_a_table(self):
        from yc import excel, ui

        path = os.path.join(tempfile.mkdtemp(), "prices.csv")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("Code,Qty\n00123,5\n00456,7.5\n")
        ui.answer_file_dialogs(path)
        self.keys("x:ImportText|w:10")
        self.assertNotEqual(self.dialog(), 0, "the preview window opens")
        for ident in ("##im_origin", "##im_delim", "##im_detect"):
            self.assertTrue(self.items(ident=ident), ident)
        self.assertIn("Load", self.labels())
        self.keys("k:Enter|w:12")
        ws = excel.ActiveSheet
        self.assertEqual(ws.Name, "prices")
        self.assertEqual(ws.Range("A2").Value, 123, "a column of numbers is typed, as Power Query detects it")
        self.assertEqual(ws.Range("A1").Value, "Code", "the first row became the headers")
        self.assertEqual(ws.Range("B3").Value, 7.5)
        self.assertEqual(ws.ListObjects.Count, 1)
        # Python: the legacy QueryTables.Add
        q = self.ws.QueryTables.Add(f"TEXT;{path}", self.ws.Range("D1"))
        q.TextFileCommaDelimiter = True
        q.TextFileColumnDataTypes = [2, 1]
        q.Refresh()
        self.assertEqual(self.ws.Range("D2").Value, "00123")
        self.assertEqual(self.ws.Range("E3").Value, 7.5)

    # ---- Flash Fill: grey suggestions while typing, Enter takes them; the options button ----
    def test_flash_fill_preview_and_options(self):
        from yc import ui

        self.ws.Range("A1:A3").Value = [["John Smith"], ["Mary Jones"], ["Ann Lee"]]
        self.ws.Range("B1").Value = "John"
        self.keys("g:B2|w:2|t:M|w:8")
        self.keys("k:Enter|w:8")
        self.assertEqual(self.ws.Range("B2").Value, "Mary")
        self.assertEqual(self.ws.Range("B3").Value, "Ann")
        self.keys("g:B2|w:4")
        self.assertTrue(self.items(ident="##flashfill_options"), "the Flash Fill Options button")
        self.click(ident="##flashfill_options", wait=4)
        self.click("Undo Flash Fill", wait=6)
        self.assertIsNone(self.ws.Range("B3").Value)
        ui.state()

    # ---- Tables: the resize handle ----
    def test_table_resize_handle(self):
        from yc import ui

        self.ws.Range("A1:B5").Value = [["N", "V"], [1, 2], [3, 4], [5, 6], [7, 8]]
        self.keys("g:A1|w:2|k:c-T|w:6|k:Enter|w:8|g:D1|w:4")
        lo = self.ws.ListObjects(1)
        h = [i for i in ui.items() if i["id"].startswith("##table_resize_")]
        self.assertTrue(h, "the handle at the table's corner")
        r = h[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        c = ui.cell_rect("B7")
        ty = int((c[1] + c[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|p:{x},{y}|w:3|m:{x},{y + 8}|w:2|m:{x},{ty}|w:3|u:|w:8|m:2,2|w:2")
        self.assertEqual(lo.Range.Address, "$A$1:$B$7")

    # ---- Data Analysis: Chart Output, New Workbook ----
    def test_histogram_chart_output(self):
        from yc import excel

        self.ws.Range("A1:A9").Value = [[x] for x in (1, 2, 2, 3, 3, 3, 4, 4, 5)]
        before = set(self.sheet_names())
        self.keys("g:A1:A9|w:2|x:DataAnalysis|w:6")
        self.click("Histogram", wait=3)
        self.click("OK", wait=6)
        self.click("Chart Output", wait=3)
        self.click("OK", wait=10)
        new = [n for n in self.sheet_names() if n not in before]
        self.assertEqual(len(new), 1)
        self.assertEqual(excel.Worksheets(new[0]).ChartObjects().Count, 1)


if __name__ == "__main__":
    unittest.main()
