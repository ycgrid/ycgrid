# SPDX-License-Identifier: Apache-2.0
"""Round 3 (charts B): chart types, the Insert > Charts galleries, and moving / resizing charts and their parts, each
checked the way a person does it: the ribbon's galleries, clicks and drags on the chart and on the sheet, and what
the screen then shows.

Run: python -m unittest tests.test_round3_charts_b (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

sys.path.insert(0, os.path.dirname(__file__))
from test_round2_excel_b import near, read_png  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


class ChartsBTest(unittest.TestCase):
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
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:2")
        self.asserts = ui.state().get("imgui_asserts", 0)

    def tearDown(self):
        from yc import ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")
        for w in ui.windows():
            if "yc_props" in w["name"]:
                ui.command("ControlProperties")
        self.assertEqual(ui.state().get("imgui_asserts", 0), self.asserts, "no ImGui check failed")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def click_at(self, x, y):
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:5")

    def items(self, label=None, ident=None):
        from yc import ui

        hits = [i for i in ui.items("") if (label is None or i["label"] == label) and (ident is None or i["id"] == ident)]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def click(self, label):
        hits = self.items(label)
        self.assertTrue(hits, f"{label!r} isn't on the screen")
        r = hits[0]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def tab(self, name):
        from yc import ui

        tabs = ui.state()["ribbon"]["tabs"]
        self.assertIn(name, tabs)
        x, y = tabs[name]
        time.sleep(0.55)
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:6")

    def ribbon(self, cmd, where=None):
        from yc import ui

        hits = [i for i in ui.ribbon_items() if i["cmd"] == cmd and (where is None or i["where"] == where)]
        self.assertTrue(hits, f"{cmd} isn't on the ribbon now")
        return hits[0]["rect"]

    def open_menu(self, cmd):
        from yc import ui

        r = self.ribbon(cmd)
        self.click_at((r[0] + r[2]) / 2, r[3] - 8 if r[3] - r[1] > 50 else (r[1] + r[3]) / 2)
        self.assertTrue(ui.state()["ribbon"].get("popup_open"), f"{cmd}'s menu opens")

    def chart_rect(self):
        obj = self.items(ident="##obj")
        self.assertTrue(obj, "the chart on the screen")
        return max(obj, key=lambda i: (i["rect"][2] - i["rect"][0]) * (i["rect"][3] - i["rect"][1]))["rect"]

    def insert(self, cmd, addr):
        from yc import ui

        self.keys(f"g:{addr}|w:2")
        self.assertTrue(ui.command(cmd))
        self.keys("w:6")
        return self.ws.ChartObjects(self.ws.ChartObjects().Count)

    def shot(self, name):
        from yc import ui

        png = os.path.join(tempfile.gettempdir(), f"yc_r3cb_{name}.png")
        ui.screenshot(png, max_width=4000)
        return read_png(png)

    def pane_title(self):
        from yc import ui

        names = [w["name"] for w in ui.windows() if "yc_props" in w["name"]]
        return names[0].split("###")[0] if names else ""

    def format_selection(self):
        from yc import ui

        ui.command("ChartFormatSelection")
        self.keys("w:6")
        return self.pane_title()

    def scroll_pane(self, notches=-30):
        from yc import ui

        pane = [w for w in ui.windows() if "yc_props" in w["name"]]
        self.assertTrue(pane, "the Format pane is open")
        r = pane[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|W:{notches}|w:4")

    def find_pixels(self, rect, color, tol=60, step=2):
        _, _, px = self.shot("find")
        x0, y0, x1, y1 = (int(v) for v in rect)
        return [(x, y) for y in range(y0 + 2, y1 - 2, step) for x in range(x0 + 2, x1 - 2, step) if near(px(x, y), color, tol)]

    # ---- M3: a gallery entry under the mouse previews its chart; a click inserts it ----
    def test_gallery_hover_previews_pie_of_pie_then_click_inserts(self):
        from yc import excel

        self.ws.Range("A1:B7").Value = [["Item", "Share"], ["A", 40], ["B", 30], ["C", 10], ["D", 8], ["E", 7], ["F", 5]]
        self.keys("g:A1:B7|w:2")
        self.tab("Insert")
        self.open_menu("InsertChartPieMenu")
        r = self.ribbon("InsertChartPieOfPie", "menu")
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:8")
        self.assertTrue(self.items("Chart Preview"), "the chart is previewed on the sheet while the mouse rests on it")
        self.assertEqual(self.ws.ChartObjects().Count, 0, "nothing is inserted by a preview")
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.keys("w:6")
        self.assertEqual(self.ws.ChartObjects().Count, 1)
        ch = self.ws.ChartObjects(1).Chart
        self.assertEqual(ch.ChartType, excel.xlPieOfPie)
        self.assertFalse(self.items("Chart Preview"), "the preview goes with the menu")
        # Bar of Pie from the same gallery
        self.keys("g:A1:B7|w:2")
        self.tab("Insert")
        self.open_menu("InsertChartPieMenu")
        r = self.ribbon("InsertChartBarOfPie", "menu")
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.keys("w:6")
        self.assertEqual(self.ws.ChartObjects(2).Chart.ChartType, excel.xlBarOfPie)

    # ---- M2: pie of pie's Series Options ----
    def test_pie_of_pie_series_options(self):
        from yc import excel

        self.ws.Range("A1:B7").Value = [["Item", "Share"], ["A", 40], ["B", 30], ["C", 10], ["D", 8], ["E", 7], ["F", 5]]
        co = self.insert("InsertChartPieOfPie", "A1:B7")
        g = co.Chart.ChartGroups(1)
        self.assertEqual((g.SplitType, g.SplitValue, g.SecondPlotSize, g.GapWidth), (excel.xlSplitByPosition, 3, 75, 100))
        # the Format pane of the series: Split Series By, Second Plot Size
        co.Chart.SeriesCollection(1).Select()
        self.keys("w:4")
        self.assertEqual(self.format_selection(), "Format Data Series")
        self.assertTrue(self.items("Split Series By"), "Split Series By is in the pane")
        self.assertTrue(self.items("Second Plot Size"))
        g.SplitType = excel.xlSplitByValue
        g.SplitValue = 9
        g.SecondPlotSize = 50
        self.assertEqual((g.SplitType, g.SplitValue, g.SecondPlotSize), (2, 9, 50))
        # the second plot shows: the chart has wedges on both sides of its middle
        self.keys("g:H20|w:6")
        r = self.chart_rect()
        mid = (r[0] + r[2]) / 2
        _, _, px = self.shot("ofpie")
        y = int((r[1] + r[3]) / 2)
        left = sum(1 for x in range(int(r[0]) + 4, int(mid)) if not near(px(x, y), (255, 255, 255), 20))
        right = sum(1 for x in range(int(mid), int(r[2]) - 4) if not near(px(x, y), (255, 255, 255), 20))
        self.assertGreater(left, 20)
        self.assertGreater(right, 10, "the second pie is drawn")

    # ---- M2, L4: volume stock charts; up / down bars and high-low lines are elements ----
    def test_volume_stock_chart_up_bars_selected_and_formatted(self):
        from yc import excel

        rows = [["Date", "Volume", "Open", "High", "Low", "Close"]]
        for i, (o, c) in enumerate([(10, 12), (12, 11), (11, 14), (14, 13), (13, 16)]):
            rows.append([46000 + i, 1000 + 100 * i, o, max(o, c) + 1, min(o, c) - 1, c])
        self.ws.Range("A1:F6").Value = rows
        self.ws.Range("A2:A6").NumberFormat = "yyyy/m/d"
        self.keys("g:A1:F6|w:2")
        self.tab("Insert")
        self.open_menu("InsertChartWaterfallMenu")
        r = self.ribbon("InsertChartStockVOHLC", "menu")
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.keys("w:6")
        co = self.ws.ChartObjects(1)
        ch = co.Chart
        self.assertEqual(ch.ChartType, excel.xlStockVOHLC)
        self.assertEqual(ch.SeriesCollection().Count, 5)
        self.assertTrue(ch.ChartGroups(1).HasUpDownBars and ch.ChartGroups(1).HasHiLoLines)
        # the volume columns on the primary axis, the prices on the secondary one
        self.assertTrue(ch.HasAxis(2, 2), "the prices have the secondary axis")
        # click a rising candle (red): Up Bars
        self.keys("g:H20|w:6")
        r = self.chart_rect()
        reds = self.find_pixels(r, (0xE0, 0x30, 0x30), tol=90)
        self.assertTrue(reds, "rising candles are drawn red")
        x, y = reds[len(reds) // 2]
        co.Select()
        self.keys("w:4")
        self.click_at(x, y)
        self.assertEqual(self.format_selection(), "Format Up Bars")
        ch.ChartGroups(1).UpBars.Format.Fill.ForeColor.RGB = 0xFF0000  # blue (BGR)
        self.keys("g:H20|w:6")
        self.assertTrue(self.find_pixels(self.chart_rect(), (0, 0, 255), tol=40), "the up bars are blue now")
        ch.ChartGroups(1).HasUpDownBars = False
        self.assertFalse(ch.ChartGroups(1).HasUpDownBars)
        # Volume-High-Low-Close: four series after the dates
        self.ws.Range("H1:L6").Value = [[r_[0], r_[1], r_[3], r_[4], r_[5]] for r_ in rows]
        co2 = self.insert("InsertChartStockVHLC", "H1:L6")
        self.assertEqual(co2.Chart.ChartType, excel.xlStockVHLC)
        self.assertEqual(co2.Chart.SeriesCollection().Count, 4)

    # ---- date axis ----
    def test_date_axis_places_columns_by_date(self):
        from yc import excel

        self.ws.Range("A1:B4").Value = [["Day", "Qty"], [46000, 5], [46001, 6], [46010, 7]]
        self.ws.Range("A2:A4").NumberFormat = "m/d"
        co = self.insert("InsertChartColumn", "A1:B4")
        ch = co.Chart
        ch.HasLegend = False
        ax = ch.Axes(1)
        self.assertEqual(ax.CategoryType, excel.xlAutomaticScale)
        self.assertEqual(ax.BaseUnit, excel.xlDays)

        def column_centers():
            self.keys("g:H20|w:6")
            r = self.chart_rect()
            _, _, px = self.shot("dateaxis")
            y = int(r[3] - (r[3] - r[1]) * 0.25)
            xs = [x for x in range(int(r[0]) + 4, int(r[2]) - 4) if near(px(x, y), (0x44, 0x72, 0xC4), 50)]
            runs, start = [], None
            for a, b in zip(xs, xs[1:] + [None]):
                start = a if start is None else start
                if b is None or b != a + 1:
                    runs.append((start + a) / 2)
                    start = None
            return runs

        dated = column_centers()
        self.assertEqual(len(dated), 3, dated)
        self.assertGreater(dated[2] - dated[1], (dated[1] - dated[0]) * 4, "the 9 days' gap shows on a date axis")
        ax.CategoryType = excel.xlCategoryScale  # a text axis: evenly spaced
        text = column_centers()
        self.assertEqual(len(text), 3, text)
        self.assertLess(abs((text[2] - text[1]) - (text[1] - text[0])), 4)
        ax.CategoryType = excel.xlTimeScale
        ax.BaseUnit = excel.xlMonths
        self.assertEqual((ax.CategoryType, ax.BaseUnit, ax.BaseUnitIsAuto), (excel.xlTimeScale, excel.xlMonths, False))

    # ---- M12: the legend resized by its handle; an axis title dragged ----
    def test_legend_resized_by_a_handle_and_axis_title_dragged(self):
        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        co = self.insert("InsertChartColumn", "A1:C4")
        ch = co.Chart
        co.Select()
        self.keys("w:4")
        r = self.chart_rect()
        lx, ly = int((r[0] + r[2]) / 2), int(r[3] - 10)
        self.click_at(lx, ly)  # the legend
        boxes = self.items("Selected Chart Element")
        self.assertTrue(boxes, "the legend's box with its handles")
        b = boxes[0]["rect"]
        hx, hy = int(b[2]), int(b[3])  # the bottom-right handle
        w0 = ch.Legend.Width
        self.keys(f"m:{hx},{hy}|w:3|d:{hx},{hy},{hx + 40},{hy}|w:8")
        self.assertTrue(ch._get()["legend_rect"][0])
        self.assertGreater(ch.Legend.Width, w0 + 10, "the legend is wider")
        # the value axis title: selected by a click, then dragged
        ch.Axes(2).HasTitle = True
        self.keys("w:4")
        co.Select()
        self.keys("w:4")
        tx, ty = int(r[0] + 10), int((r[1] + r[3]) / 2)
        self.click_at(tx, ty)
        self.keys(f"m:{tx},{ty}|w:3|d:{tx},{ty},{tx + 50},{ty - 30}|w:8")
        self.assertTrue(ch._get()["val_title_rect"][0], "the axis title keeps the place it was dragged to")
        self.assertGreater(ch.Axes(2).AxisTitle.Left, 0)

    # ---- L6: the data outlines moved by their borders ----
    def test_data_outline_moved_by_its_border(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        co = self.insert("InsertChartColumn", "A1:C4")
        ch = co.Chart
        ch.SeriesCollection(1).Select()
        self.keys("w:6")
        # the blue outline around B2:B4: its left border dragged one column right
        a, b = ui.cell_rect("B3"), ui.cell_rect("C3")
        x, y = int(a[0]), int((a[1] + a[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|d:{x},{y},{x + int(b[0] - a[0])},{y}|w:8")
        self.assertIn("$C$2:$C$4", ch.SeriesCollection(1).Formula, ch.SeriesCollection(1).Formula)

    # ---- L3: several trendlines, intercept, names; error bar end style and X error bars ----
    def test_trendlines_and_error_bar_options(self):
        from yc import excel

        self.ws.Range("A1:B6").Value = [["X", "Y"], [1, 3], [2, 5], [3, 7], [4, 9], [5, 11]]
        co = self.insert("InsertChartScatter", "A1:B6")
        s = co.Chart.SeriesCollection(1)
        s.Trendlines().Add(Type=excel.xlLinear)
        t2 = s.Trendlines().Add(Type=excel.xlMovingAvg, Period=2)
        self.assertEqual(s.Trendlines().Count, 2)
        self.assertEqual(t2.Name, "2 per. Mov. Avg. (Y)")
        t1 = s.Trendlines(1)
        t1.Intercept = 0
        self.assertFalse(t1.InterceptIsAuto)
        t1.Name = "Fit"
        self.assertEqual((s.Trendlines(1).Name, s.Trendlines(1).NameIsAuto), ("Fit", False))
        # the legend lists the series and both trendlines
        co.Chart.HasLegend = True
        s.Trendlines(1).Select()
        self.keys("w:4")
        self.assertEqual(self.format_selection(), "Format Trendline")
        self.assertTrue(self.items("Set Intercept"))
        s.Trendlines(2).Delete()
        self.assertEqual(s.Trendlines().Count, 1)
        # error bars: no cap; X error bars on a scatter chart
        s.ErrorBar(excel.xlY, excel.xlErrorBarIncludeBoth, excel.xlErrorBarTypeFixedValue, 1)
        s.ErrorBars.EndStyle = excel.xlNoCap
        self.assertEqual(s.ErrorBars.EndStyle, excel.xlNoCap)
        s.ErrorBar(excel.xlX, excel.xlErrorBarIncludeBoth, excel.xlErrorBarTypePercent, 10)
        self.assertEqual(s.XErrorBars.Type, excel.xlErrorBarTypePercent)
        self.assertEqual(s.ErrorBars.Type, excel.xlErrorBarTypeFixedValue, "the Y error bars stay")

    # ---- L8: a new chart in the middle of the window ----
    def test_new_chart_in_the_middle_of_the_window(self):
        from yc import ui

        self.ws.Range("A1:B4").Value = [["M", "V"], ["a", 1], ["b", 3], ["c", 2]]
        self.insert("InsertChartColumn", "A1:B4")
        self.keys("g:A1|w:4")
        r = self.chart_rect()
        w, _, _ = self.shot("middle")
        self.assertLess(abs((r[0] + r[2]) / 2 - w / 2), w * 0.12, "centered across the window")
        self.assertGreater(r[1], ui.cell_rect("A3")[1], "not put against the data's first row")

    # ---- L7: Format Chart Area > Properties: Print object, Locked ----
    def test_properties_print_object_and_locked(self):
        self.ws.Range("A1:B4").Value = [["M", "V"], ["a", 1], ["b", 3], ["c", 2]]
        co = self.insert("InsertChartColumn", "A1:B4")
        # Locked off: the chart still moves on a protected sheet
        co.Locked = False
        self.ws.Protect()
        left = co.Left
        self.keys("k:Escape|w:2|g:H20|w:4")
        r = self.chart_rect()
        x, y = int(r[0] + 8), int(r[1] + 8)
        self.keys(f"m:{x},{y}|w:3|d:{x},{y},{x + 60},{y}|w:8")
        self.assertGreater(co.Left, left, "an unlocked chart moves on a protected sheet")
        self.ws.Unprotect()
        self.assertFalse(co.Locked)
        # Format Chart Area > Properties: Print object unticked in the pane
        co.Select()
        self.keys("w:4")
        self.format_selection()
        self.scroll_pane()  # Properties is at the pane's end, as in Excel's Size & Properties
        self.assertTrue(self.items("Print object"))
        self.assertTrue(self.items("Locked"))
        self.click("Print object")
        self.keys("w:4")
        self.assertFalse(co.PrintObject, "unticked in the pane")

    # ---- L12, L2: 3-D presets, floor and walls; treemap, box and whisker and map options ----
    def test_3d_presets_floor_walls_and_type_options(self):
        from yc import excel

        self.ws.Range("A1:B4").Value = [["M", "V"], ["a", 1], ["b", 3], ["c", 2]]
        co = self.insert("InsertChart3DColumn", "A1:B4")
        ch = co.Chart
        co.Select()
        self.keys("w:4")
        self.format_selection()
        self.assertTrue(self.items("Presets"), "3-D Rotation has presets")
        self.click("Presets")
        self.keys("w:4")
        self.click("Isometric Left Down")
        self.keys("w:4")
        self.assertEqual((ch.Rotation, ch.Elevation), (45, 35))
        ch.Walls.Format.Fill.ForeColor.RGB = 0xF0B0A0  # light blue (BGR)
        ch.Floor.Interior.Color = 0x00FFFF  # yellow
        self.assertEqual(ch.Floor.Interior.Color, 0x00FFFF)
        self.keys("g:H20|w:6")
        self.assertTrue(self.find_pixels(self.chart_rect(), (0xA0, 0xB0, 0xF0), tol=30), "the walls are light blue")
        # box and whisker: mean line and inclusive median; treemap labels; map options
        self.ws.Range("D1:D9").Value = [["V"]] + [[v] for v in range(1, 9)]
        bw = self.insert("InsertChartBoxWhisker", "D1:D9").Chart.SeriesCollection(1)
        bw.ShowMeanLine = True
        bw.QuartileCalculationInclusiveMedian = True
        self.assertTrue(bw.ShowMeanLine and bw.QuartileCalculationInclusiveMedian)
        self.ws.Range("F1:G4").Value = [["Region", "V"], ["North", 4], ["South", 3], ["East", 2]]
        tm = self.insert("InsertChartTreemap", "F1:G4").Chart.SeriesCollection(1)
        tm.ParentDataLabelOption = excel.xlParentDataLabelOptionsOverlapping
        self.assertEqual(tm.ParentDataLabelOption, 2)
        self.ws.Range("I1:J4").Value = [["Country", "V"], ["France", 4], ["Germany", 3], ["Spain", 2]]
        mp = self.insert("InsertChartMap", "I1:J4").Chart.SeriesCollection(1)
        mp.GeoProjectionType = excel.xlGeoProjectionTypeRobinson
        mp.GeoMappingLevel = excel.xlGeoMappingLevelWorld
        mp.RegionLabelOption = excel.xlRegionLabelOptionsShowAll
        mp.SeriesColorGradientStyle = excel.xlSeriesColorGradientStyleDiverging
        self.assertEqual((mp.GeoProjectionType, mp.GeoMappingLevel, mp.RegionLabelOption, mp.SeriesColorGradientStyle), (4, 7, 2, 1))
        mp.Select()
        self.keys("w:4")
        self.assertEqual(self.format_selection(), "Format Data Series")
        self.assertTrue(self.items("Map projection") and self.items("Map area") and self.items("Map labels"))


    # ---- the date axis on paper, and on stock charts ----
    def _blue_runs(self, rect=None, color=(0x44, 0x72, 0xC4), tol=50):
        """The centers of the column runs on the scan line that crosses the most of them (3 expected)."""
        w, h, px = self.shot("runs")
        x0, y0, x1, y1 = (int(v) for v in (rect or (0, 0, w, h)))
        best = []
        for y in range(y0 + 2, y1 - 2, 3):
            xs = [x for x in range(x0 + 2, x1 - 2) if near(px(x, y), color, tol)]
            runs, start = [], None
            for a, b in zip(xs, xs[1:] + [None]):
                start = a if start is None else start
                if b is None or b != a + 1:
                    if a - start >= 2:
                        runs.append((start + a) / 2)
                    start = None
            if len(runs) == 3 and (len(best) != 3 or runs[2] - runs[0] > best[2] - best[0]):
                best = runs
        return best

    def test_printed_chart_spaces_the_points_by_date(self):
        from yc import excel, ui

        self.ws.Range("A1:B4").Value = [["Day", "Qty"], [46000, 5], [46001, 6], [46010, 7]]
        self.ws.Range("A2:A4").NumberFormat = "m/d"
        co = self.insert("InsertChartColumn", "A1:B4")
        ch = co.Chart
        ch.HasLegend = False
        ch.HasTitle = False
        cs = ch.Location(excel.xlLocationAsNewSheet, "PrintDates")  # a chart sheet: the chart fills the printed page
        self.keys("w:4")
        ui.command("FilePrint")  # File > Print: the page previewed as it prints
        self.keys("w:12")
        try:
            runs = self._blue_runs()
            self.assertEqual(len(runs), 3, runs)
            self.assertGreater(runs[2] - runs[1], (runs[1] - runs[0]) * 4, "the 9 days' gap shows on the printed page")
        finally:
            self.keys("k:Escape|w:6")
        cs.Axes(1).CategoryType = excel.xlCategoryScale
        ui.command("FilePrint")
        self.keys("w:12")
        try:
            runs = self._blue_runs()
            self.assertEqual(len(runs), 3, runs)
            self.assertLess(abs((runs[2] - runs[1]) - (runs[1] - runs[0])), 6, "a text axis: evenly spaced on paper")
        finally:
            self.keys("k:Escape|w:6")

    def test_stock_chart_shows_weekend_gaps_and_a_text_axis_closes_them(self):
        from yc import excel

        rows = [["Date", "Open", "High", "Low", "Close"]]
        for i, d in enumerate([46000, 46001, 46004]):  # Thursday, Friday, Monday
            rows.append([d, 10, 13 + i, 9 - i, 12])  # rising days at the same height: one scan line crosses them
        self.ws.Range("A1:E4").Value = rows
        self.ws.Range("A2:A4").NumberFormat = "m/d"
        co = self.insert("InsertChartStock", "A1:E4")
        ch = co.Chart
        self.assertEqual(ch.Axes(1).CategoryType, excel.xlAutomaticScale)
        self.keys("g:H20|w:6")
        red = (0xE0, 0x30, 0x30)
        runs = self._blue_runs(self.chart_rect(), red, 90)
        self.assertEqual(len(runs), 3, runs)
        self.assertGreater(runs[2] - runs[1], (runs[1] - runs[0]) * 2, "Excel's date axis: the weekend is a gap")
        ch.Axes(1).CategoryType = excel.xlCategoryScale  # Text axis: one place per trading day
        self.keys("g:H20|w:6")
        runs = self._blue_runs(self.chart_rect(), red, 90)
        self.assertEqual(len(runs), 3, runs)
        self.assertLess(abs((runs[2] - runs[1]) - (runs[1] - runs[0])), 4)

    # ---- M4: the Chart Styles flyout (the brush) and a set of styles per chart type ----
    def test_chart_styles_flyout_and_per_type_sets(self):
        from yc import excel, ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        co = self.insert("InsertChartColumn", "A1:C4")
        ch = co.Chart
        fly = sorted(self.items(ident="##fly"), key=lambda i: i["rect"][1])
        self.assertEqual(len(fly), 3, "+, Chart Styles and Chart Filters beside the chart")
        r = fly[1]["rect"]  # the brush, between + and the filter
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        first = self.items("Style 1")[0]["rect"]
        styles = set()
        for _ in range(6):  # the list scrolls: read it all, as a person scrolls through it
            styles |= {i["label"] for i in ui.items("") if i["label"].startswith("Style ")}
            if "Style 14" in styles:
                break
            self.keys(f"m:{int((first[0] + first[2]) / 2)},{int((first[1] + first[3]) / 2)}|w:2|W:-3|w:3")
        self.assertIn("Style 14", styles, "the column set has 14 styles")
        self.assertNotIn("Style 15", styles)
        self.keys(f"m:{int((first[0] + first[2]) / 2)},{int((first[1] + first[3]) / 2)}|w:2|W:3|w:2|W:3|w:3")  # back up
        # the mouse on Style 8: the chart shows it dark, nothing changes yet
        for _ in range(4):
            if self.items("Style 8"):
                break
            self.keys(f"m:{int((first[0] + first[2]) / 2)},{int((first[1] + first[3]) / 2)}|w:2|W:-2|w:3")
        s8 = self.items("Style 8")[0]["rect"]
        self.keys(f"m:{int((s8[0] + s8[2]) / 2)},{int((s8[1] + s8[3]) / 2)}|w:8")
        rect = self.chart_rect()
        dark = self.find_pixels((rect[0] + 10, rect[1] + 10, rect[0] + 60, rect[1] + 60), (0x40, 0x40, 0x40), tol=20, step=6)
        self.assertTrue(dark, "Style 8 previewed on the chart")
        self.assertEqual(ch.ChartStyle, 201)
        self.click("Style 7")
        self.keys("w:4")
        self.assertEqual(ch.ChartStyle, 207)
        self.assertTrue(ch.SeriesCollection(1).HasDataLabels, "Style 7: values above the columns")
        ch.ChartStyle = 210
        self.assertEqual(ch.SeriesCollection(1).Format.Fill.Type, 3, "Style 10: gradient fills (msoFillGradient)")
        # the Color tab: palettes, applied by a click
        co.Select()
        self.keys("w:4")
        fly = sorted(self.items(ident="##fly"), key=lambda i: i["rect"][1])
        r = fly[1]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.click("Color")
        self.click("Monochromatic Palette 1")
        self.keys("w:4")
        self.assertEqual(ch.ChartColor, 14)
        # per type: a line chart's set (15, numbered from 227), a pie's (12, from 251) on the ribbon too
        ch.ChartType = excel.xlLine
        self.assertEqual(ch.ChartStyle, 227)
        ch.ChartStyle = 233
        self.assertEqual(ch.ChartStyle, 233)
        with self.assertRaises(Exception):
            ch.ChartStyle = 250  # past the line set (227-241)
        co.Select()
        self.keys("w:4")
        self.tab("Chart Design")
        self.open_menu("ChartStylesMenu")
        cmds = {i["cmd"] for i in ui.ribbon_items() if i["where"] == "menu"}
        self.assertIn("ChartStyle15", cmds)
        self.assertNotIn("ChartStyle16", cmds)
        self.keys("k:Escape|w:3")
        ch.ChartType = excel.xlPie
        co.Select()
        self.keys("w:4")
        self.tab("Chart Design")
        self.open_menu("ChartStylesMenu")
        cmds = {i["cmd"] for i in ui.ribbon_items() if i["where"] == "menu"}
        self.assertIn("ChartStyle12", cmds)
        self.assertNotIn("ChartStyle13", cmds)
        r = self.ribbon("ChartStyle3", "menu")
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.keys("w:4")
        self.assertEqual(ch.ChartStyle, 253)
        self.assertEqual(ch.SeriesCollection(1).Explosion, 10, "the pie set's Style 3 pulls the slices out")

    # ---- the mouse wheel over an open flyout or menu is the popup's: the sheet stays, the popup stays open ----
    def test_wheel_over_a_popup_never_scrolls_the_sheet_or_closes_it(self):
        from yc import excel, ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        self.insert("InsertChartColumn", "A1:C4")
        top = excel.ActiveWindow.ScrollRow
        fly = sorted(self.items(ident="##fly"), key=lambda i: i["rect"][1])
        r = fly[1]["rect"]  # Chart Styles
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.assertTrue(self.items("Style 1"), [(i["label"], i["id"]) for i in ui.items("") if i["label"]][-30:])
        f = self.items("Style 1")[0]["rect"]
        x, y = int((f[0] + f[2]) / 2), int((f[1] + f[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|W:-6|w:3|W:-6|w:3|W:-6|w:3|W:-6|w:3")  # past the end of the list
        self.assertTrue(ui.state()["ribbon"]["popup_open"], "the flyout stays open")
        self.assertEqual(excel.ActiveWindow.ScrollRow, top, "the sheet under the flyout didn't scroll")
        self.keys("k:Escape|w:3")
        # a ribbon menu that doesn't scroll (Insert > Pie or Doughnut)
        self.tab("Insert")
        self.open_menu("InsertChartPieMenu")
        m = self.ribbon("InsertChartPie", "menu")
        x, y = int((m[0] + m[2]) / 2), int((m[1] + m[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|W:-6|w:3|W:-6|w:3")
        self.assertTrue(ui.state()["ribbon"]["popup_open"], "the menu stays open")
        self.assertEqual(excel.ActiveWindow.ScrollRow, top, "the sheet under the menu didn't scroll")
        self.keys("k:Escape|w:3")

if __name__ == "__main__":
    unittest.main()
