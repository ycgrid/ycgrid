# SPDX-License-Identifier: Apache-2.0
"""PivotTables and PivotCharts working like Excel's, checked the way a person uses them.

Every test fails without its fix. Run: python -m unittest tests.test_parity_pivots (with the bundled Python).
"""
from __future__ import annotations

import datetime as dt
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")

# the sales source: Date, Region, Product, Qty, Sales
ROWS = [["Date", "Region", "Product", "Qty", "Sales"],
        [dt.date(2024, 1, 5), "East", "A", 1, 10], [dt.date(2024, 2, 10), "East", "B", 2, 20],
        [dt.date(2024, 3, 1), "West", "A", 3, 30], [dt.date(2024, 7, 9), "West", "B", 4, 40],
        [dt.date(2025, 1, 3), "East", "A", 1, 5], [dt.date(2025, 2, 2), "North", "B", 1, 7],
        [dt.date(2025, 2, 20), "West", "A", 2, 25], [dt.date(2025, 3, 2), "North", "A", 1, 3]]


class PivotParityTest(unittest.TestCase):
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
        self.ws.Range("A1:E9").Value = ROWS
        self.ws.Range("A2:A9").NumberFormat = "yyyy-mm-dd"
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

    def items(self, label=None, ident=None, window=""):
        from yc import ui

        hits = [i for i in ui.items(window) if (label is None or i["label"] == label) and (ident is None or i["id"] == ident)]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def click(self, label=None, nth=0, ident=None, window="", double=False):
        hits = self.items(label, ident, window)
        self.assertGreater(len(hits), nth, f"{label or ident!r} isn't on the screen")
        r = hits[nth]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|{'D' if double else 'c'}:{x},{y}|w:6")

    def pane_click(self, label):
        """A field's check box in the PivotTable Fields pane."""
        hits = [i for i in self.items(label) if "PivotTable Fields" in i.get("window", "")]
        self.assertTrue(hits, f"{label!r} isn't in the field list")
        r = hits[0]["rect"]
        x, y = int(r[0]) + 8, int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")

    def pivot(self, dest="H3", name=None, rows=(), cols=(), pages=(), data=("Sales",)):
        from yc import excel

        pc = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, self.ws.Name + "!A1:E9")
        pt = pc.CreatePivotTable(self.ws.Name + "!" + dest, name)
        for f in rows:
            pt.PivotFields(f).Orientation = excel.xlRowField
        for f in cols:
            pt.PivotFields(f).Orientation = excel.xlColumnField
        for f in pages:
            pt.PivotFields(f).Orientation = excel.xlPageField
        for f in data:
            pt.AddDataField(pt.PivotFields(f))
        self.keys("w:6")
        return pt

    def col(self, letter, first, last):
        return [self.ws.Range(f"{letter}{r}").Value for r in range(first, last + 1)]

    # ---- High ----
    def test_h1_a_growing_report_asks_before_replacing_data(self):
        from yc import excel

        pt = self.pivot("H1", "PTgrow")
        self.ws.Range("H5").Value = 999                # Region on Rows reaches H5 (header, 3 regions, total)
        self.keys("m:2,2|A:H2|w:6")
        self.pane_click("Region")                      # the report would grow over H6
        self.assertTrue(self.dialog(), "Excel asks: There's already data in ... Do you want to replace it?")
        self.keys("k:Escape|w:6")
        self.assertEqual(self.ws.Range("H5").Value, 999)
        self.assertEqual(pt.RowFields.Count, 0)
        self.keys("m:2,2|A:H2|w:6")
        self.pane_click("Region")
        self.keys("k:Enter|w:8")                       # OK: replace
        self.assertEqual(pt.RowFields.Count, 1)
        self.assertNotEqual(self.ws.Range("H5").Value, 999)
        # Python: DisplayAlerts True raises (Excel would ask); False replaces
        self.ws.Range("H10").Value = "keep"            # Product under each region reaches H11
        with self.assertRaises(yc.YcError):
            pt.PivotFields("Product").Orientation = excel.xlRowField
        self.assertEqual(self.ws.Range("H10").Value, "keep")
        excel.Application.DisplayAlerts = False
        pt.PivotFields("Product").Orientation = excel.xlRowField
        self.assertEqual(pt.RowFields.Count, 2)
        # a table under it: refused whatever DisplayAlerts says
        excel.Application.DisplayAlerts = True
        self.ws.Range("K1:L3").Value = [["a", "b"], [1, 2], [3, 4]]
        self.ws.ListObjects.Add(excel.xlSrcRange, self.ws.Range("K1:L3"), None, excel.xlYes)
        with self.assertRaises(yc.YcError):
            pt.PivotFields("Region").Orientation = excel.xlColumnField
        self.assertEqual(self.ws.Range("K2").Value, 1)

    def test_h3_dates_go_to_rows_and_keep_their_format(self):
        pt = self.pivot("H3", "PTdate", data=())
        self.keys("m:2,2|A:H4|w:6")
        self.pane_click("Date")
        # Microsoft 365 groups a date field by Years, Quarters and Months (round 3); ungrouped, the dates keep their format
        self.assertEqual([f.Name for f in pt.RowFields], ["Years", "Quarters", "Date"])
        self.assertEqual(pt.DataFields.Count, 0)
        pt.PivotFields("Date").Ungroup()
        self.keys("w:4")
        self.assertEqual([f.Name for f in pt.RowFields], ["Date"])
        first = self.ws.Range("H4")
        self.assertIsInstance(first.Value2, (int, float))     # the date stays a number
        self.assertEqual(first.Text, "2024-01-05")
        # a number field with a blank: Values as Count
        self.ws.Range("D5").Value = None
        pt.RefreshTable()
        self.keys("m:2,2|A:H4|w:6")
        self.pane_click("Qty")
        self.assertEqual(pt.DataFields(1).Name, "Count of Qty")

    def test_h4_plus_minus_buttons_and_double_click_expand_and_collapse(self):
        from yc import excel

        pt = self.pivot("H3", "PTtree", rows=("Region", "Product"))
        self.assertEqual(self.ws.Range("H4").Value, "East")
        self.assertEqual(self.ws.Range("H5").Value, "A")
        buttons = self.items(ident="##pm", window="Workbook")
        self.assertEqual(len(buttons), 3, "a - left of East, North and West")
        self.click(ident="##pm", window="Workbook")      # East's button
        self.assertFalse(pt.PivotFields("Region").PivotItems("East").ShowDetail)
        self.assertEqual(self.ws.Range("H5").Value, "North")
        self.assertEqual(self.ws.Range("I4").Value, 35)   # East's total on its one row
        self.keys("m:2,2|Q:H5|w:8")                    # double-click North: collapsed too
        self.assertFalse(pt.PivotFields("Region").PivotItems("North").ShowDetail)
        self.keys("m:2,2|Q:H5|w:8")                    # again: expanded
        self.assertTrue(pt.PivotFields("Region").PivotItems("North").ShowDetail)
        # right-click > Expand/Collapse > Collapse Entire Field; Analyze > Expand Field
        self.keys("m:2,2|R:H4|w:6")
        self.click("Expand/Collapse")
        self.click("Collapse Entire Field")
        self.assertEqual(self.col("H", 4, 7), ["East", "North", "West", "Grand Total"])
        self.keys("m:2,2|A:H4|w:4|x:PivotExpandField|w:6")
        self.assertEqual(self.ws.Range("H5").Value, "A")
        # double-click an innermost item: Show Detail adds a field below it
        self.keys("m:2,2|Q:H5|w:8")
        self.assertTrue(self.dialog())
        self.click("Qty", window="Show Detail")
        self.click("OK", window="Show Detail")
        self.assertEqual([f.Name for f in pt.RowFields], ["Region", "Product", "Qty"])
        # saved with the workbook; Python sets it as Excel's does
        pt.PivotFields("Region").PivotItems("West").ShowDetail = False
        self.assertFalse(pt.PivotFields("Region").PivotItems("West").ShowDetail)
        del excel

    def test_h5_sort_by_values_from_a_value_cell(self):
        from yc import excel

        pt = self.pivot("H3", "PTsort", rows=("Region",))
        self.assertEqual(self.col("H", 4, 6), ["East", "North", "West"])   # East 35, North 10, West 95
        self.keys("m:2,2|R:I4|w:6")
        self.click("Sort")
        self.click("Sort Largest to Smallest")
        self.assertEqual(self.col("H", 4, 6), ["West", "East", "North"])
        self.assertEqual(pt.PivotFields("Region").AutoSortField, "Sum of Sales")
        pt.PivotFields("Region").AutoSort(excel.xlAscending, "Sum of Sales")
        self.assertEqual(self.col("H", 4, 6), ["North", "East", "West"])
        pt.PivotFields("Region").AutoSort(excel.xlAscending, "Region")
        self.assertEqual(self.col("H", 4, 6), ["East", "North", "West"])

    def test_h6_slicers_read_the_source_once_per_change(self):
        # a 20,000-row source with three slicers: frames stay short while the mouse moves over the sheet
        from yc import excel

        n = 20000
        self.ws.Range(f"A2:E{n + 1}").Value = [[45000 + i % 700, ("East", "West", "North")[i % 3], "P%d" % (i % 40), i % 7, i % 100] for i in range(n)]
        pc = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, self.ws.Name + f"!A1:E{n + 1}")
        pt = pc.CreatePivotTable(self.ws.Name + "!H3", "PTbig")
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))

        def frames():  # the frames while the mouse moves over the sheet (median: the machine's load moves both runs alike)
            self.keys("w:10")
            self.conn.request("perf.stats", {"last": 240, "reset": True})
            for k in range(40):
                self.keys(f"m:{300 + k * 5},{300 + k * 3}|w:2")
            return self.conn.request("perf.stats", {"last": 60})

        before = frames()
        for k, f in enumerate(("Region", "Product", "Qty")):
            excel.ActiveWorkbook.SlicerCaches.Add2(pt, f).Slicers.Add(self.ws, None, None, None, 40, 330 + 30 * k)
        st = frames()
        # reading the 20,000 rows again for each slicer on every frame made each frame several times longer
        self.assertLess(st["frame_cpu_p50_ms"], before["frame_cpu_p50_ms"] * 2 + 15,
                        f"frames read the pivot source again for every slicer: {before['frame_cpu_p50_ms']:.1f} -> {st}")

    # ---- Medium ----
    def test_m2_double_click_a_caption_opens_value_field_settings(self):
        self.pivot("H3", "PTcap", rows=("Region",))
        self.keys("m:2,2|Q:I3|w:8")                    # "Sum of Sales"
        self.assertTrue(self.dialog())
        self.assertTrue(self.items("Summarize Values By") or self.items("Custom Name"))
        self.keys("k:Escape|w:4")

    def test_m3_show_values_as_difference_from_previous(self):
        from yc import excel

        pt = self.pivot("H3", "PTshow", rows=("Product",), data=("Sales", "Sales"))
        self.assertEqual(self.ws.Range("J3").Value, "Sum of Sales2")      # L8: the same field twice
        df = pt.DataFields(2)
        df.Calculation = excel.xlDifferenceFrom
        df.BaseField = "Product"
        df.BaseItem = "(previous)"
        self.assertIsNone(self.ws.Range("J4").Value)                        # A: no previous
        self.assertEqual(self.ws.Range("J5").Value, 67 - 73)                # B (67) - A (73)
        self.assertEqual(df.Calculation, excel.xlDifferenceFrom)
        # the right-click menu lists Excel's options; a base field opens the dialog
        self.keys("m:2,2|R:I4|w:6")
        self.click("Show Values As")
        self.assertTrue(self.items("Rank Largest to Smallest..."))
        self.click("% of Grand Total")
        self.assertEqual(pt.DataFields(1).Calculation, excel.xlPercentOfTotal)

    def test_m4_report_filter_takes_several_items(self):
        pt = self.pivot("H1", "PTpage", rows=("Product",), pages=("Region",))
        self.click(ident="##pf", window="Workbook")     # the Region filter's button (H1's neighbour)
        self.click("Select Multiple Items")
        self.click("North")
        self.click("OK")
        self.assertEqual(self.ws.Range("I1").Value, "(Multiple Items)")
        self.assertFalse(pt.PivotFields("Region").PivotItems("North").Visible)
        self.assertTrue(pt.PivotFields("Region").EnableMultiplePageItems)

    def test_m5_value_filter_top_and_clear_filter(self):
        pt = self.pivot("H3", "PTtop", rows=("Region",))
        self.click(ident="##pf", window="Workbook")     # Row Labels
        self.click("Value Filters")
        self.click("Top 10...")
        self.assertTrue(self.dialog())
        self.click(ident="##n", window="Top 10 Filter")
        self.keys("k:c-A|t:1|w:2")
        self.click("OK", window="Top 10 Filter")
        self.assertEqual(self.col("H", 4, 5), ["West", "Grand Total"])
        self.click(ident="##pf", window="Workbook")
        self.click('Clear Filter From "Region"')
        self.assertEqual(self.col("H", 4, 7), ["East", "North", "West", "Grand Total"])
        self.assertEqual(pt.PivotFields("Region").PivotFilters.Count, 0)

    def test_m6_slicer_and_the_field_filter_are_one(self):
        from yc import excel

        pt = self.pivot("H3", "PTslice", rows=("Region",))
        cache = excel.ActiveWorkbook.SlicerCaches.Add2(pt, "Region")
        cache.Slicers.Add(self.ws, None, None, None, 40, 330)
        pt.PivotFields("Region").PivotItems("North").Visible = False      # the drop-down's filter
        self.assertFalse(cache.SlicerItems("North").Selected)             # the slicer shows it
        self.keys("w:4")
        self.assertTrue(self.items(ident="##clear", window="Workbook"))
        self.click(ident="##clear", window="Workbook")  # the slicer's Clear Filter
        self.assertTrue(pt.PivotFields("Region").PivotItems("North").Visible)
        self.assertEqual(self.col("H", 4, 6), ["East", "North", "West"])

    def test_m7_field_settings_repeat_labels_and_custom_subtotals(self):
        from yc import excel

        pt = self.pivot("H3", "PTfs", rows=("Region", "Product"))
        pt.RowAxisLayout(excel.xlTabularRow)
        self.keys("w:4")
        self.keys("m:2,2|R:H4|w:6")
        self.click("Field Settings...")
        self.assertTrue(self.dialog())
        self.click("Layout & Print")
        self.click("Repeat item labels")
        self.click("OK")
        self.assertTrue(pt.PivotFields("Region").RepeatLabels)
        self.assertEqual(self.ws.Range("H5").Value, "East")               # B's row shows its region
        pt.PivotFields("Region").Subtotals = [False, True, True] + [False] * 9
        self.assertEqual(self.ws.Range("H6").Value, "East Sum")
        self.assertEqual(self.ws.Range("H7").Value, "East Count")

    def test_m8_grouping_dialog_shows_numbers_as_typed(self):
        pt = self.pivot("H3", "PTgrp", rows=("Sales",), data=("Qty",))
        self.keys("m:2,2|R:H4|w:6")
        self.click("Group...")
        self.assertTrue(self.dialog())
        texts = [i["label"] for i in self.items()]
        self.assertNotIn("0.000000", texts)
        self.click("OK")
        self.assertEqual(len(pt._get()["groups"]), 1)
        self.assertTrue(str(self.ws.Range("H4").Value).startswith("3-"))
        # Group on a text field with one label: Excel says it can't
        pt.PivotFields("Sales").Orientation = 0
        pt.PivotFields("Region").Orientation = 1
        self.keys("w:4|m:2,2|R:H4|w:6")
        self.click("Group...")
        from yc import ui

        msgs = [e["text"] for e in ui.errors()][-3:]
        self.assertTrue(any("Cannot group that selection." in m for m in msgs), msgs)

    def test_m9_group_selection_makes_a_field(self):
        pt = self.pivot("H3", "PTsel", rows=("Region",))
        self.keys("m:2,2|g:H4:H5|w:4")                  # East and North
        self.keys("x:PivotGroupSelection|w:6")
        fields = pt._get()["fields"]
        self.assertIn("Region2", fields)
        self.assertEqual(self.ws.Range("H4").Value, "Group1")
        self.keys("m:2,2|A:H4|w:4|x:PivotUngroup|w:6")
        self.assertNotIn("Region2", pt._get()["fields"])

    def test_m10_options_for_empty_cells_show(self):
        pt = self.pivot("H3", "PTopt", rows=("Region",), cols=("Qty",))
        self.assertIsNone(self.ws.Range("J6").Value)    # North sold no Qty 2
        self.keys("m:2,2|R:I6|w:6")
        self.click("PivotTable Options...")
        self.assertTrue(self.dialog())
        self.click("For empty cells show:")
        self.click(ident="##null", window="PivotTable Options")
        self.keys("t:0|w:2")
        self.click("OK", window="PivotTable Options")
        self.assertEqual(pt.NullString, "0")
        self.assertTrue(pt.DisplayNullString)
        self.assertEqual(self.ws.Range("J6").Value, 0)

    def test_m11_styles_and_banded_rows(self):
        pt = self.pivot("H3", "PTsty", rows=("Region",))
        before = self.ws.Range("H3").Interior.Color
        self.keys("m:2,2|A:H4|w:4|x:PivotStyleMedium9|w:6")
        self.assertEqual(pt.TableStyle2, "PivotStyleMedium9")
        self.assertNotEqual(self.ws.Range("H3").Interior.Color, before)
        pt.ShowTableStyleRowStripes = True
        self.assertNotEqual(self.ws.Range("H5").Interior.Color, self.ws.Range("H4").Interior.Color)

    def test_m12_clear_filters_and_select_values(self):
        pt = self.pivot("H3", "PTclr", rows=("Region",))
        pt.PivotFields("Region").PivotItems("East").Visible = False
        self.keys("m:2,2|A:H4|w:4|x:PivotClearFilters|w:6")
        self.assertTrue(pt.PivotFields("Region").PivotItems("East").Visible)
        self.keys("m:2,2|A:H4|w:4|x:PivotSelectValues|w:4")
        from yc import excel

        self.assertEqual(excel.Selection.Address, pt.DataBodyRange.Address)

    def test_m13_menu_remove_value_and_keep_only_selected(self):
        pt = self.pivot("H3", "PTmenu2", rows=("Region",))
        self.keys("m:2,2|R:H5|w:6")                      # North
        self.click("Filter")
        self.click("Keep Only Selected Items")
        self.assertEqual(self.col("H", 4, 5), ["North", "Grand Total"])
        self.keys("m:2,2|R:I4|w:6")
        self.click('Remove "Sum of Sales"')
        self.assertEqual(pt.DataFields.Count, 0)

    def test_m14_pivotchart_type_and_it_stays_when_the_pivot_goes(self):
        from yc import excel

        pt = self.pivot("H3", "PTchart", rows=("Region",))
        self.keys("m:2,2|A:H4|w:4|x:PivotChart|w:6")
        self.assertTrue(self.dialog())
        self.click("Line")
        self.click("OK")
        ch = self.ws.ChartObjects(self.ws.ChartObjects().Count).Chart
        self.assertEqual(ch.ChartType, excel.xlLine)
        pt.Delete()
        self.keys("w:4")
        self.assertGreater(ch.SeriesCollection().Count, 0)

    def test_m15_field_list_search_and_defer_layout_update(self):
        pt = self.pivot("H3", "PTpane", data=())
        self.keys("m:2,2|A:H4|w:6")
        self.click(ident="##search", window="PivotTable Fields")
        self.keys("t:pro|w:4")
        pane = [i["label"] for i in self.items() if "PivotTable Fields" in i.get("window", "")]
        self.assertIn("Product", pane)
        self.assertNotIn("Region", pane)
        self.keys("k:c-A|k:Delete|w:3")
        self.click("Defer Layout Update")
        self.pane_click("Region")
        self.assertEqual(pt.RowFields.Count, 0)          # waits for Update
        self.click("Update")
        self.assertEqual(pt.RowFields.Count, 1)
        self.click("Defer Layout Update")

    # ---- Low ----
    def test_l1_alt_f5_refreshes(self):
        pt = self.pivot("H3", "PTrefresh", rows=("Region",))
        self.ws.Range("E2").Value = 110                  # East: 35 -> 135
        self.keys("m:2,2|A:H4|w:4|k:a-F5|w:6")
        self.assertEqual(self.ws.Range("I4").Value, 135)
        del pt

    def test_l6_typing_over_a_caption_renames_it(self):
        pt = self.pivot("H3", "PTname", rows=("Region",))
        self.keys("m:2,2|A:I3|w:3|t:Revenue|k:Enter|w:6")
        self.assertEqual(pt.DataFields(1).Caption, "Revenue")
        self.keys("m:2,2|A:H4|w:3|t:E|k:Enter|w:6")
        self.assertEqual(self.ws.Range("H4").Value, "E")


if __name__ == "__main__":
    unittest.main()
