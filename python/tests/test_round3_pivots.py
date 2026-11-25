# SPDX-License-Identifier: Apache-2.0
"""Round 3 (pivots): PivotTables, slicers and timelines used the way a person uses Excel's: the field list, Field Settings,
Value Field Settings, dragging a label, the Analyze tab's Active Field box and Enable Selection, Report Connections,
table slicers, the timeline's handles.

Run: python -m unittest tests.test_round3_pivots (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")

# Date (two years), Region, Product, Sales
ROWS = [["Date", "Region", "Product", "Sales"],
        [45296, "East", "A", 10], [45332, "West", "B", 20], [45400, "East", "B", 30], [45500, "North", "A", 40],
        [45700, "West", "A", 50], [45720, "East", "A", 60], [45760, "North", "B", 70], [45780, "West", "B", 80]]


class Round3PivotsTest(unittest.TestCase):
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
        self.ws.Range("A1:D9").Value = ROWS
        self.ws.Range("A2:A9").NumberFormat = "yyyy-mm-dd"
        self.keys("w:2")

    def tearDown(self):
        from yc import ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def click_at(self, x, y):
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:6")

    def items(self, label=None, ident=None):
        from yc import ui

        hits = [i for i in ui.items() if (label is None or i["label"] == label) and (ident is None or i["id"] == ident)]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def click(self, label=None, ident=None, nth=0):
        hits = self.items(label, ident)
        self.assertGreater(len(hits), nth, f"{label or ident!r} isn't on the screen")
        r = hits[nth]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def test_create_pivottable_shows_the_data_model_box_disabled(self):
        # ADR 0012: no Data Model; Excel's box is in its place, off and disabled, so a person sees why it isn't there
        from yc import ui

        self.keys("g:A1:D9|w:2")
        self.assertTrue(ui.command("InsertPivotTable"))
        self.keys("w:6")
        box = self.items("Add this data to the Data Model")
        self.assertTrue(box, "Create PivotTable shows Excel's Add this data to the Data Model box")
        self.assertTrue(box[0]["disabled"])
        self.keys("k:Escape|w:4")

    def pivot(self, dest="F3", name=None):
        from yc import excel

        pc = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, self.ws.Name + "!A1:D9")
        return pc.CreatePivotTable(self.ws.Name + "!" + dest, name)

    def asserts_unchanged(self, before):
        from yc import ui

        self.assertEqual(ui.state().get("imgui_asserts", 0), before, "ImGui checks failed")

    # ---- H3: a date field checked in the field list goes to Rows grouped by Years, Quarters and Months ----
    def test_h3_checking_a_date_field_groups_it(self):
        from yc import ui

        before = ui.state().get("imgui_asserts", 0)
        pt = self.pivot()
        self.keys("A:F3|w:6")
        self.click("Date")
        self.keys("w:6")
        d = pt._get()
        self.assertEqual(len(d["groups"]), 1, d["groups"])
        self.assertEqual(d["groups"][0]["periods"], 64 | 32 | 16, "Years, Quarters and Months")
        names = [d["fields"][e["field"]] for e in d["rows"]]
        self.assertEqual(names, ["Years", "Quarters", "Date"])
        self.assertEqual(d["data"], [], "nothing went to Values")
        self.assertEqual(self.ws.Range("F4").Value, "2024")
        self.asserts_unchanged(before)

    # ---- M7: Field Settings > Layout & Print and Subtotals & Filters ----
    def test_m7_field_settings_tabular_form_and_new_items(self):
        from yc import excel, ui

        pt = self.pivot()
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.PivotFields("Product").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        self.keys("A:F4|w:4")  # East
        self.assertTrue(ui.command("PivotFieldSettings"))
        self.keys("w:6")
        self.assertTrue(ui.state().get("dialog"), "Field Settings opens")
        self.click("Include new items in manual filter")
        self.click("Layout & Print")
        self.click("Show item labels in tabular form")
        self.click("Insert page break after each item")
        self.click("OK")
        self.keys("w:6")
        f = pt.PivotFields("Region")
        self.assertEqual(f.LayoutForm, excel.xlTabular)
        self.assertTrue(f.IncludeNewItemsInFilter)
        self.assertTrue(f.LayoutPageBreak)
        # tabular: Region's labels in F, Product's in G on the same row
        self.assertEqual(self.ws.Range("F4").Value, "East")
        self.assertEqual(self.ws.Range("G4").Value, "A")
        # Python, with Excel's names
        f.LayoutForm = excel.xlOutline
        f.LayoutSubtotalLocation = 2  # xlAtBottom
        self.assertEqual(f.LayoutForm, excel.xlOutline)
        self.assertEqual(f.LayoutSubtotalLocation, 2)

    # ---- M7 / L9: Value Field Settings > Number Format; values are General until a format is set ----
    def test_l9_values_are_general_and_number_format_opens_over_value_field_settings(self):
        from yc import excel, ui

        self.ws.Range("D2:D9").NumberFormat = "$#,##0.00"
        pt = self.pivot()
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        self.keys("w:4")
        self.assertEqual(self.ws.Range("G4").NumberFormat, "General", "a value doesn't take the source's format (Excel)")
        self.keys("A:G4|w:4")
        self.assertTrue(ui.command("PivotFieldSettings"))  # a value cell: Value Field Settings
        self.keys("w:6")
        btn = [i for i in self.items("Number Format") if "Value Field Settings" in i.get("window", "")]
        self.assertTrue(btn, "Value Field Settings has a Number Format button")
        r = btn[0]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.keys("w:6")
        nested = lambda: [i for i in ui.items() if "Format Cells" in i.get("window", "")]  # noqa: E731
        self.assertTrue(nested(), "Format Cells opens over Value Field Settings")
        self.keys("k:Escape|w:6")
        self.assertTrue(ui.state().get("dialog"), "Escape closed Format Cells only")
        self.assertFalse(nested())
        self.click("OK")
        self.keys("w:4")
        pt.DataFields(1).NumberFormat = "0.0"
        self.keys("w:3")
        self.assertEqual(self.ws.Range("G4").NumberFormat, "0.0")

    # ---- L5: drag a label to another label's place ----
    def test_l5_dragging_a_label_moves_the_item(self):
        from yc import excel, ui

        pt = self.pivot()
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        self.keys("w:4")
        self.assertEqual([self.ws.Range(a).Value for a in ("F4", "F5", "F6")], ["East", "North", "West"])
        self.keys("A:F6|w:4")
        r6, r4 = ui.cell_rect("F6"), ui.cell_rect("F4")
        x, y = r6[2] - 1, (r6[1] + r6[3]) / 2
        self.keys(f"m:{x:.0f},{y:.0f}|w:6|d:{x:.0f},{y:.0f},{x:.0f},{(r4[1] + r4[3]) / 2:.0f}|w:10")
        self.assertEqual([self.ws.Range(a).Value for a in ("F4", "F5", "F6")], ["West", "East", "North"])
        self.assertEqual(pt.PivotFields("Region").PivotItems("West").Position, 1)

    # ---- M12: Active Field box, Enable Selection, Alt+Shift+Right groups the selected items ----
    def test_m12_active_field_box_and_enable_selection(self):
        from yc import excel, ui

        pt = self.pivot()
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        self.keys("A:F4|w:4")
        tabs = ui.state()["ribbon"]["tabs"]
        x, y = tabs["PivotTable Analyze"]
        self.click_at(x, y)
        box = self.items("Active Field")
        self.assertTrue(box, "the Active Field box is on the Analyze tab")
        b = box[0]["rect"]
        self.click_at(b[0] + 12, (b[1] + b[3]) / 2)
        self.keys("k:c-A|t:Area|k:Enter|w:6")
        self.assertEqual(pt.PivotFields("Region").Caption, "Area")
        # Enable Selection: the left edge of West's label selects its row of the report
        self.assertTrue(pt.EnableSelection)
        r = ui.cell_rect("F6")
        self.keys(f"m:{r[0] + 2:.0f},{(r[1] + r[3]) / 2:.0f}|w:4|c:{r[0] + 2:.0f},{(r[1] + r[3]) / 2:.0f}|w:6")
        self.assertEqual(excel.Selection.Address.replace("$", ""), "F6:G6")
        # Data > Group's key in a report: Group Selection
        self.keys("g:F4:F5|w:3|k:s-a-Right|w:6")
        self.assertIn("Region2", pt._get()["fields"])

    # ---- L10: Report Connections; a table's slicer ----
    def test_l10_report_connections_and_table_slicers(self):
        from yc import excel, ui

        pt1 = self.pivot("F3", "RcA")
        pt1.PivotFields("Region").Orientation = excel.xlRowField
        pt1.AddDataField(pt1.PivotFields("Sales"))
        pt2 = self.pivot("F20", "RcB")
        pt2.PivotFields("Product").Orientation = excel.xlRowField
        pt2.AddDataField(pt2.PivotFields("Sales"))
        cache = excel.ActiveWorkbook.SlicerCaches.Add2(pt1, "Region")
        sl = cache.Slicers.Add(self.ws, None, None, None, 20, 400, 150, 180)
        self.keys("w:4")
        a1, b2 = ui.cell_rect("A1"), ui.cell_rect("B2")
        sx = (b2[0] - a1[0]) / (self.ws.Columns(1).Width * 4 / 3)
        hx, hy = a1[0] + (sl.Left + 30) * 4 / 3 * sx, a1[1] + (sl.Top + 8) * 4 / 3 * sx
        self.keys(f"m:{hx:.0f},{hy:.0f}|w:3|r:|w:6")
        self.click("Report Connections...")
        self.keys("w:6")
        self.assertTrue(ui.state().get("dialog"), "Report Connections opens from the slicer's menu")
        self.click(f"RcB   ({self.ws.Name})")
        self.click("OK")
        self.keys("w:6")
        self.assertEqual(cache.PivotTables.Count, 2)
        # a click on the slicer's West button filters both PivotTables
        cache.SlicerItems("West").Selected = False
        self.keys("w:3")
        self.assertIn("West", [f["hidden"] for f in pt2._get()["slicer_filters"]][0])
        cache.PivotTables.RemovePivotTable(pt2)
        self.assertEqual(cache.PivotTables.Count, 1)
        self.assertEqual(pt2._get()["slicer_filters"], [])
        # a table's slicer: Insert > Slicer with a cell of the table selected
        self.ws.Range("K30:L34").Value = [["Sym", "Qty"], ["TXF", 1], ["MXF", 2], ["TXF", 3], ["TE", 4]]
        self.ws.ListObjects.Add(1, self.ws.Range("K30:L34"), None, 1)
        self.keys("g:K31|w:3")
        self.assertTrue(ui.command("InsertSlicer"))
        self.keys("w:6")
        self.click("Sym")
        self.click("OK")
        self.keys("w:6")
        tcache = [c for c in excel.ActiveWorkbook.SlicerCaches if c.ListObject is not None]
        self.assertEqual(len(tcache), 1)
        self.assertEqual([i.Name for i in tcache[0].SlicerItems()], ["MXF", "TE", "TXF"])
        tcache[0].SlicerItems("MXF").Selected = False
        self.keys("w:3")
        self.assertTrue(self.ws.Rows(32).Hidden, "the slicer filters the table")
        tcache[0].ClearManualFilter()
        self.assertFalse(self.ws.Rows(32).Hidden)

    # ---- L10: the timeline's edge handles and Show options ----
    def test_l10_timeline_handles_and_show_options(self):
        from yc import excel, ui

        pt = self.pivot()
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        tl = excel.ActiveWorkbook.SlicerCaches.Add2(pt, "Date", None, 2).Slicers.Add(self.ws, None, None, None, 20, 400, 330, 120)
        cache = tl.SlicerCache
        cache.TimelineState.SetFilterDateRange("2024-03-01", "2024-04-30")
        self.keys("w:6")
        handles = self.items("Timeline End Handle")
        self.assertTrue(handles, "the selection has its handles")
        h = handles[0]["rect"]
        hx, hy = (h[0] + h[2]) / 2, (h[1] + h[3]) / 2
        self.keys(f"m:{hx:.0f},{hy:.0f}|w:4|d:{hx:.0f},{hy:.0f},{hx + 90:.0f},{hy:.0f}|w:10")
        end = cache.TimelineState.EndDate
        self.assertGreater((end.year, end.month), (2024, 4), "dragging the end handle moved the end")
        self.assertEqual(cache.TimelineState.StartDate.month, 3, "the start stayed")
        vs = tl.TimelineViewState
        vs.ShowTimeLevel = False
        self.keys("w:3")
        self.assertFalse(self.items("Timeline Level"))
        vs.ShowTimeLevel = True
        self.keys("w:3")
        self.assertTrue(self.items("Timeline Level"))

    # ---- L10: Ctrl+V on the sheet pastes a copied slicer, at the active cell (Home > Paste too) ----
    def test_l10_ctrl_v_on_the_sheet_pastes_a_copied_slicer(self):
        from yc import excel, ui

        pt = self.pivot()
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        cache = excel.ActiveWorkbook.SlicerCaches.Add2(pt, "Region")
        sl = cache.Slicers.Add(self.ws, None, None, None, 200, 10, 140, 160)
        self.keys("w:4")
        a1, b2 = ui.cell_rect("A1"), ui.cell_rect("B2")
        sx = (b2[0] - a1[0]) / (self.ws.Columns(1).Width * 4 / 3)
        sy = (b2[1] - a1[1]) / (self.ws.Rows(1).Height * 4 / 3)
        hx, hy = a1[0] + (sl.Left + 30) * 4 / 3 * sx, a1[1] + (sl.Top + 8) * 4 / 3 * sy
        self.click_at(hx, hy)
        self.keys("k:c-C|w:3")
        # a click on a cell lets the slicer go; Ctrl+V there pastes the slicer, not cells
        self.keys("A:J20|w:3|k:c-V|w:8")
        self.assertEqual(cache.Slicers.Count, 2, "Ctrl+V on the sheet pasted the copied slicer")
        new = [x for x in cache.Slicers if x.Name != sl.Name][0]
        self.assertAlmostEqual(new.Top, self.ws.Range("J20").Top, delta=1)
        self.assertAlmostEqual(new.Left, self.ws.Range("J20").Left, delta=1)
        # Home > Paste as well; copying cells afterwards makes Paste paste cells again
        self.keys("A:J40|w:3")
        self.assertTrue(ui.command("Paste"))
        self.keys("w:6")
        self.assertEqual(cache.Slicers.Count, 3)
        self.ws.Range("A1").Copy()
        self.keys("A:L2|w:3|k:c-V|w:6")
        self.assertEqual(cache.Slicers.Count, 3)
        self.assertEqual(self.ws.Range("L2").Value, "Date")

    # ---- M14: Multi-level Category Labels (a range of two category columns, a PivotChart of two row fields) ----
    def test_m14_multi_level_category_labels_on_the_screen(self):
        from yc import excel, ui

        self.ws.Range("F1:H6").Value = [["Region", "Product", "Qty"], ["East", "A", 1], ["", "B", 2], ["West", "A", 3], ["", "B", 4], ["", "C", 5]]
        co = self.ws.ChartObjects().Add(20, 200, 360, 220)
        ch = co.Chart
        se = ch.SeriesCollection().NewSeries()
        se.Values = self.ws.Range("H2:H6")
        se.XValues = self.ws.Range("F2:G6")
        self.keys("g:A1|w:8")
        levels = lambda: sorted(i["label"] for i in ui.items() if i["label"].startswith("Category Level"))  # noqa: E731
        self.assertEqual(levels(), ["Category Level 1: East", "Category Level 1: West"])
        east = [i for i in ui.items() if i["label"] == "Category Level 1: East"][0]["rect"]
        west = [i for i in ui.items() if i["label"] == "Category Level 1: West"][0]["rect"]
        self.assertLess(east[0], west[0], "East's group is left of West's")
        self.assertAlmostEqual(east[1], west[1], delta=1, msg="one row for the outer level")
        self.assertTrue(ch.Axes(1).TickLabels.MultiLevel)
        ch.Axes(1).TickLabels.MultiLevel = False
        self.keys("w:6")
        self.assertEqual(levels(), [], "unchecked: one row of labels")
        # a PivotChart of Region and Product
        pt = self.pivot("K3", "PTlevels")
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.PivotFields("Product").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        pt.RowGrand = False
        pt.ColumnGrand = False
        self.keys("A:K5|w:4")
        self.assertTrue(ui.command("PivotChart"))
        self.keys("w:6|k:Enter|w:8|g:A1|w:6")
        self.assertIn("Category Level 1: East", levels())
        self.assertIn("Category Level 1: North", levels())


if __name__ == "__main__":
    unittest.main()
