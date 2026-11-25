# SPDX-License-Identifier: Apache-2.0
"""UI options that had no Python member, or whose Python member didn't do what the UI does.

Each test does the change from Python and checks what the UI shows, or changes it in the UI and reads it back from
Python: the two must be one state. Run: python -m unittest tests.test_round2_pyapi (with the bundled Python).
"""
from __future__ import annotations

import decimal
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


def cells_of(rng):
    """The (row, col) cells of every area of a Range."""
    out = set()
    for a in rng._areas:
        for r in range(a[0], a[2] + 1):
            for c in range(a[1], a[3] + 1):
                out.add((r, c))
    return out


class PythonApiTest(unittest.TestCase):
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

        excel.Application.DisplayAlerts = True
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:2")

    def tearDown(self):
        from yc import ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")
        if ui.state().get("ribbon", {}).get("backstage"):
            self.keys("k:Escape|w:4")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def dialog(self):
        from yc import ui

        return ui.state().get("dialog", 0)

    def click_at(self, x, y):
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:5")

    def find(self, label, window="", by="label"):
        from yc import ui

        hits = [i for i in ui.items(window) if i[by] == label]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def click(self, label, window="", nth=0, by="label"):
        hits = self.find(label, window, by)
        self.assertGreater(len(hits), nth, f"{label!r} isn't on the screen")
        r = hits[nth]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def pane(self):
        from yc import ui

        return next((w for w in ui.windows() if "yc_props" in w["name"]), None)

    def click_in_pane(self, label):
        """A control of the Format pane, scrolled into view first as a person does."""
        for _ in range(12):
            p = self.pane()
            self.assertIsNotNone(p, "the Format pane is open")
            r = p["rect"]
            # (a control half cut off by the pane's edge is scrolled further first, as a person does)
            seen = [i for i in self.find(label) if r[1] <= (i["rect"][1] + i["rect"][3]) / 2 <= r[3]]
            if seen:
                h = seen[0]["rect"]
                self.click_at((h[0] + h[2]) / 2, (h[1] + h[3]) / 2)
                return
            self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:2|W:-3|w:3")
        self.fail(f"{label!r} isn't in the Format pane")

    # ---- Go To Special > Conditional formats / Data validation: All and Same ----
    def test_special_cells_same_rules_and_rules_on_empty_cells(self):
        from yc import excel, ui

        ws = self.ws
        ws.Range("A1").Value = 1  # the used range is A1 only: the rules sit on empty cells
        ws.Range("B2:B4").FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "=5").Interior.Color = 0x00FFFF
        ws.Range("D2:D3").FormatConditions.Add(excel.xlCellValue, excel.xlLess, "=0").Interior.Color = 0x0000FF
        ws.Range("F2:F5").Validation.Add(excel.xlValidateWholeNumber, excel.xlValidAlertStop, excel.xlBetween, "1", "9")
        # All, from one cell: every rule of the sheet (it raised "No cells were found": only the used range was searched)
        got = ws.Range("A1").SpecialCells(excel.xlCellTypeAllFormatConditions)
        self.assertEqual(cells_of(got), cells_of(ws.Range("B2:B4,D2:D3")))
        self.assertEqual(cells_of(ws.Range("A1").SpecialCells(excel.xlCellTypeAllValidation)), cells_of(ws.Range("F2:F5")))
        # Same: the cells sharing the cell's rule (it returned the blank cells)
        same = ws.Range("B3").SpecialCells(excel.xlCellTypeSameFormatConditions)
        self.assertEqual(cells_of(same), cells_of(ws.Range("B2:B4")))
        self.assertEqual(cells_of(ws.Range("F4").SpecialCells(excel.xlCellTypeSameValidation)), cells_of(ws.Range("F2:F5")))
        # the UI does the same: Go To Special > Conditional formats > Same on B3
        self.keys("g:B3|w:2")
        ui.command("GoToSpecial")
        self.keys("w:6")
        self.click("Conditional formats")
        self.click("Same", by="label")
        self.keys("k:Enter|w:6")
        self.assertEqual(cells_of(excel.Application.Selection), cells_of(same))

    # ---- Chart: Select Data > Hidden and Empty Cells > Show #N/A as an empty cell ----
    def test_chart_display_na_as_blank_is_the_dialogs_box(self):
        from yc import ui

        self.ws.Range("A1:B4").Value = [["Day", "Close"], ["Mon", 10], ["Tue", 20], ["Wed", 30]]
        self.keys("g:A1:B4|w:2")
        self.assertTrue(ui.command("InsertChartLine"))
        self.keys("w:6")
        co = self.ws.ChartObjects(1)
        ch = co.Chart
        self.assertTrue(ch.DisplayNAAsBlank, "Excel's default: #N/A is plotted as an empty cell")
        ch.DisplayNAAsBlank = False
        self.assertFalse(ch.DisplayNAAsBlank)
        co.Select()
        self.keys("w:2")
        ui.command("ChartSelectData")
        self.keys("w:6")
        self.click("Hidden and Empty Cells")
        self.click("Show #N/A as an empty cell")  # the box showed Python's False: a click ticks it
        self.click("OK##hidden", by="id")
        self.keys("k:Enter|w:6")
        self.assertTrue(ch.DisplayNAAsBlank)

    # ---- Chart: Format Data Series > Show connector lines (waterfall) ----
    def test_waterfall_show_connector_lines(self):
        from yc import ui

        self.ws.Range("A1:B5").Value = [["Step", "Amount"], ["Start", 100], ["Sales", 40], ["Costs", -30], ["End", 110]]
        self.keys("g:A1:B5|w:2")
        self.assertTrue(ui.command("InsertChartWaterfall"))
        self.keys("w:6")
        ch = self.ws.ChartObjects(1).Chart
        grp = ch.ChartGroups(1)
        self.assertTrue(grp.ShowConnectorLines)
        ch.SeriesCollection(1).Select()
        self.keys("w:3")
        ui.command("ChartFormatSelection")
        self.keys("w:6")
        self.click_in_pane("Show connector lines")
        self.assertFalse(grp.ShowConnectorLines, "the pane's box and Python are one setting")
        grp.ShowConnectorLines = True
        self.assertTrue(ch._get()["connectors"])

    # ---- Ctrl+C / Ctrl+X / Ctrl+D on a selected chart ----
    def test_chart_object_copy_cut_duplicate(self):
        from yc import excel, ui

        self.ws.Range("A1:B4").Value = [["Day", "Close"], ["Mon", 10], ["Tue", 20], ["Wed", 30]]
        self.keys("g:A1:B4|w:2")
        self.assertTrue(ui.command("InsertChartColumn"))
        self.keys("w:6")
        co = self.ws.ChartObjects(1)
        left, top = co.Left, co.Top
        dup = co.Duplicate()
        self.keys("w:3")
        self.assertEqual(self.ws.ChartObjects().Count, 2)
        self.assertEqual(dup.Name, self.ws.ChartObjects(2).Name)
        self.assertGreater(dup.Left, left)
        self.assertGreater(dup.Top, top)
        self.assertEqual(dup.Chart.SeriesCollection().Count, co.Chart.SeriesCollection().Count)
        # Copy, then paste on another sheet: a chart there too
        co.Copy()
        other = excel.Worksheets.Add()
        other.Activate()
        self.keys("w:3")
        other.Paste()
        self.keys("w:3")
        self.assertEqual(other.ChartObjects().Count, 1)
        # Cut: the chart goes at once and Paste brings it back
        self.ws.Activate()
        self.keys("w:3")
        dup.Cut()
        self.keys("w:3")
        self.assertEqual(self.ws.ChartObjects().Count, 1)
        self.ws.Paste()
        self.keys("w:3")
        self.assertEqual(self.ws.ChartObjects().Count, 2)

    # ---- Format Shape > Properties > Print object ----
    def test_shape_print_object(self):
        from yc import ui

        sp = self.ws.Shapes.AddShape(1, 100, 40, 120, 60)
        self.assertTrue(sp.PrintObject)
        sp.PrintObject = False
        self.assertFalse(sp.PrintObject)
        sp.Select()
        self.keys("w:3")
        ui.command("FormatShapePane")
        self.keys("w:6")
        self.click_in_pane("Print object")  # showed Python's False: a click ticks it
        self.assertTrue(sp.PrintObject)

    # ---- the test tool sees what a person sees ----
    def test_items_leave_out_controls_scrolled_out_of_a_pane(self):
        from yc import ui

        sp = self.ws.Shapes.AddShape(1, 100, 40, 120, 60)
        sp.Select()
        self.keys("w:3")
        ui.command("FormatShapePane")
        self.keys("w:6")
        p = self.pane()
        self.assertIsNotNone(p, "the Format pane is open")
        r = p["rect"]
        mine = [i for i in ui.items() if i["window"] == p["label"]]
        self.assertTrue(mine, "the pane's controls are listed")
        outside = [i["label"] for i in mine if i["rect"][3] <= r[1] or i["rect"][1] >= r[3]]
        self.assertEqual(outside, [], "controls scrolled out of the pane's view are listed as if a person could click them")

    # ---- Format Control > Current value reads a decimal value as a number ----
    def test_format_control_reads_a_decimal_value(self):
        from yc import excel, ui

        sp = self.ws.Shapes.AddFormControl(excel.xlSpinner, 200, 40, 20, 40)
        cf = sp.ControlFormat
        cf.Min, cf.Max = 0, 10
        cf._set(value="1.5")  # a value saved as text with a decimal point
        r = ui.cell_rect("A1")
        k = (r[2] - r[0]) / self.ws.Columns(1).Width  # pixels per point
        x, y = int(r[0] + 210 * k), int(r[1] + 60 * k)
        self.keys(f"m:{x},{y}|w:2|r:|w:4")  # right-click the spinner > Format Control...
        self.click("Format Control...")
        self.keys("w:8")
        self.click("Control")  # Excel's dialog: the Control tab holds Current value
        self.keys("w:4")
        self.assertTrue(self.find("Current value"), "the Format Control dialog's Control tab is shown")
        self.keys("k:Enter|w:6")  # OK without touching anything
        # read digit by digit it was 15, past Max: the dialog clamped it to 10 and wrote that back
        self.assertIn(cf.Value, (1, 2))

    # ---- PivotTable Options > Display > Field List ----
    def test_pivot_field_list_sort_ascending(self):
        from yc import excel, ui

        ws = self.ws
        ws.Range("A1:C5").Value = [["Region", "Product", "Sales"], ["East", "A", 1], ["West", "B", 2], ["East", "B", 3], ["North", "A", 4]]
        pc = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, ws.Name + "!A1:C5")
        pt = pc.CreatePivotTable(ws.Name + "!F3", "PTlist")
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        self.keys("w:6")
        self.assertFalse(pt.FieldListSortAscending, "data source order by default")
        pt.FieldListSortAscending = True
        self.assertTrue(pt.FieldListSortAscending)
        self.keys("g:F5|w:4")
        ui.command("PivotOptions")
        self.keys("w:6")
        self.assertTrue(self.dialog())
        self.click("Display")
        self.click("Sort in data source order")
        self.keys("k:Enter|w:6")
        self.assertFalse(pt.FieldListSortAscending)

    # ---- File > Options > Calculation ----
    def test_truncate_to_15_digits(self):
        from yc import excel

        app = excel.Application
        before = app.TruncateTo15Digits
        try:
            app.TruncateTo15Digits = True
            self.assertTrue(app.TruncateTo15Digits)
            self.keys("g:B2|w:2|t:1234567890123456789|k:Enter|w:4")
            v = self.ws.Range("B2").Value
            self.assertNotIsInstance(v, decimal.Decimal)
            # 15 significant digits kept, the rest cut to 0 as Excel does (not rounded: ...3460000)
            self.assertIsInstance(v, float)
            self.assertEqual(v, 1234567890123450000.0)
            app.TruncateTo15Digits = False
            self.keys("g:C2|w:2|t:1234567890123456789|k:Enter|w:4")
            self.assertEqual(decimal.Decimal(str(self.ws.Range("C2").Value)), decimal.Decimal("1234567890123456789"))
        finally:
            app.TruncateTo15Digits = before

    def open_options(self):
        from yc import ui

        x, y = ui.state()["ribbon"]["tabs"]["File"]
        self.click_at(x, y)
        self.assertTrue(ui.state()["ribbon"].get("backstage"))
        page = next(i for i in ui.ribbon_items() if i["where"] == "backstage" and i["cmd"] == "BackstageOptions")
        r = page["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.assertTrue(self.find("Automatic except for data tables"), "the Options page is shown")

    def test_options_show_the_calculation_python_set(self):
        from yc import excel

        app = excel.Application
        try:
            app.Calculation = excel.xlCalculationManual
            self.assertEqual(app.Calculation, excel.xlCalculationManual)
            self.open_options()
            # the page showed Automatic (the saved setting), so OK without a change turned Python's Manual back off
            self.click("OK")
            self.keys("w:4")
            self.assertEqual(app.Calculation, excel.xlCalculationManual, "the page showed Manual; OK kept it")
        finally:
            app.Calculation = excel.xlCalculationAutomatic


if __name__ == "__main__":
    unittest.main()
