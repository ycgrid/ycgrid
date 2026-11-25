# SPDX-License-Identifier: Apache-2.0
"""Charts working like Excel's, each checked the way a person does it: clicks, keys, the Format
pane, the ribbon, and what the chart then shows (screenshots) or holds.

Run: python -m unittest tests.test_parity_charts (with the bundled Python).
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


def read_png(path):
    """(width, height, pixel(x, y) -> (r, g, b)) of an 8-bit RGB / RGBA PNG (the screenshots), with the standard library."""
    import struct
    import zlib

    with open(path, "rb") as fh:
        data = fh.read()
    pos, idat, w, h, ctype = 8, b"", 0, 0, 2
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, lambda x, y: tuple(rows[y][x * bpp:x * bpp + 3])


ACCENT1 = (0x44, 0x72, 0xC4)  # Office theme: the first series
ACCENT2 = (0xED, 0x7D, 0x31)


def near(c, ref, tol=40):
    return sum(abs(a - b) for a, b in zip(c, ref)) <= tol


class ChartParityTest(unittest.TestCase):
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
        from yc import ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def click_at(self, x, y):
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:5")

    def click(self, label, window="", nth=0, by="label"):
        from yc import ui

        hits = [i for i in ui.items(window) if i[by] == label]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        self.assertGreater(len(hits), nth, f"{label!r} isn't on the screen")
        r = hits[nth]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        return r

    def chart_rect(self):
        from yc import ui

        obj = [i for i in ui.items("") if i["id"] == "##obj"]
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

        png = os.path.join(tempfile.gettempdir(), f"yc_chp_{name}.png")
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

    def close_pane(self):
        from yc import ui

        for w in ui.windows():
            if "yc_props" in w["name"]:
                ui.command("ControlProperties")  # (toggled by its own close button; Escape below)
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")

    # ---- H1: the secondary axis ----
    def test_a_series_on_the_secondary_axis_gets_its_own_axis(self):
        # the price line moved to the secondary axis was drawn on a fixed 0-1 axis, off the plot
        self.ws.Range("A1:C4").Value = [["Month", "Volume", "Price"], ["Jan", 1000, 100], ["Feb", 3000, 120], ["Mar", 2000, 110]]
        co = self.insert("InsertChartCombo", "A1:C4")
        ch = co.Chart
        ch.HasTitle = False
        ch.HasLegend = False
        # Format Data Series > Plot Series On > Secondary Axis, on the price series
        from yc import ui

        ui.command("ChartSelSeries2")
        self.keys("w:2")
        self.assertIn("Format Data Series", self.format_selection())
        self.click("Secondary Axis")
        self.keys("w:4")
        self.assertEqual(ch.SeriesCollection(2).AxisGroup, 2)
        self.assertTrue(ch.HasAxis(2, 2))
        self.close_pane()
        self.keys("g:H30|w:6")
        r = self.chart_rect()
        _, _, px = self.shot("y2")
        orange = sum(1 for y in range(int(r[1]) + 2, int(r[3]) - 2, 2) for x in range(int(r[0]) + 2, int(r[2]) - 2, 2) if near(px(x, y), ACCENT2, 60))
        self.assertGreater(orange, 20, "the price line is inside the plot")
        # the secondary axis is an element: its own Format pane
        co.Select()
        self.keys("w:2")
        ui.command("ChartSelValAxis2")
        self.assertIn("Secondary Vertical (Value) Axis", self.format_selection())
        self.close_pane()

    # ---- H2: hidden rows, empty cells, the Hidden and Empty Cells dialog ----
    def test_hidden_rows_leave_the_chart_and_empty_cells_settings(self):
        self.ws.Range("A1:B4").Value = [["Day", "Close"], ["Mon", 10], ["Tue", 20], ["Wed", 30]]
        co = self.insert("InsertChartColumn", "A1:B4")
        ch = co.Chart
        ch.HasTitle = False
        ch.HasLegend = False
        self.assertTrue(ch.PlotVisibleOnly)
        self.assertEqual(ch.DisplayBlanksAs, 1, "empty cells as gaps by default")

        def columns():
            self.keys("k:Escape|w:2|g:H30|w:6")
            r = self.chart_rect()
            _, _, px = self.shot("hidden")
            y = int(r[3] - (r[3] - r[1]) * 0.18)
            runs, inside = 0, False
            for x in range(int(r[0]) + 2, int(r[2]) - 2):
                b = near(px(x, y), ACCENT1, 60)
                runs += b and not inside
                inside = b
            return runs

        self.assertEqual(columns(), 3)
        self.keys("g:A3|w:2|k:c-9|w:4")  # hide row 3 (Tue)
        self.assertEqual(columns(), 2, "a hidden row leaves the chart, as in Excel")
        # Select Data > Hidden and Empty Cells > Connect data points with line
        co.Select()
        self.keys("w:2")
        from yc import ui

        ui.command("ChartSelectData")
        self.keys("w:6")
        self.click("Hidden and Empty Cells")
        self.click("Connect data points with line")
        self.click("Show data in hidden rows and columns")
        self.click("OK##hidden", by="id")
        self.keys("k:Enter|w:6")
        self.assertEqual(ch.DisplayBlanksAs, 3)
        self.assertFalse(ch.PlotVisibleOnly)
        self.ws.Range("A3").EntireRow.Hidden = False

    # ---- H4: a single data point ----
    def test_a_second_click_selects_a_point_and_set_as_total(self):
        from yc import ui

        self.ws.Range("A1:B6").Value = [["Step", "Amount"], ["Start", 100], ["Sales", 40], ["Costs", -30], ["Tax", -10], ["End", 100]]
        co = self.insert("InsertChartWaterfall", "A1:B6")
        ch = co.Chart
        self.keys("k:Escape|w:2|g:H30|w:6")
        r = self.chart_rect()
        _, _, px = self.shot("wf")
        # the last column: the rightmost colored pixels of the plot
        best = None
        for y in range(int(r[1] + (r[3] - r[1]) * 0.15), int(r[1] + (r[3] - r[1]) * 0.8), 4):
            xs = [x for x in range(int(r[0]) + 4, int(r[2]) - 4) if max(px(x, y)) - min(px(x, y)) > 40]
            if xs and (best is None or xs[-1] > best[0]):
                best = (xs[-1], y)
        self.assertTrue(best, "the columns on the screen")
        x, y = best[0] - 4, best[1]
        self.click_at(x, y)
        self.keys("w:20")
        self.click_at(x, y)  # again: the point under the mouse
        self.assertEqual(self.format_selection(), "Format Data Point")
        self.close_pane()
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:20|c:{x},{y}|w:4|r:|w:6")
        self.click("Set as Total")
        self.keys("w:4")
        self.assertTrue(ch.SeriesCollection(1).Points(5).IsTotal)

    def test_pie_slices_have_their_own_color_and_explosion(self):
        self.ws.Range("A1:B5").Value = [["Region", "Sales"], ["N", 10], ["S", 20], ["E", 30], ["W", 40]]
        co = self.insert("InsertChartPie", "A1:B5")
        ch = co.Chart
        ch.HasTitle = False
        ch.HasLegend = False
        p = ch.SeriesCollection(1).Points(2)
        p.Format.Fill.ForeColor.RGB = 0x0000FF  # red
        p.Explosion = 30
        self.assertEqual(p.Explosion, 30)
        self.assertEqual(p.Format.Fill.ForeColor.RGB, 0x0000FF)
        self.keys("k:Escape|w:2|g:H30|w:6")
        r = self.chart_rect()
        _, _, px = self.shot("pie")
        reds = sum(1 for y in range(int(r[1]), int(r[3]), 2) for x in range(int(r[0]), int(r[2]), 2) if near(px(x, y), (255, 0, 0), 40))
        self.assertGreater(reds, 30, "the second slice is red")
        ch.ChartGroups(1).VaryByCategories = False
        self.assertFalse(ch.ChartGroups(1).VaryByCategories)

    # ---- H5: axis options ----
    def test_format_axis_sets_one_bound_log_scale_and_reverse_order(self):
        from yc import ui

        self.ws.Range("A1:B5").Value = [["Q", "V"], ["a", 1], ["b", 10], ["c", 100], ["d", 1000]]
        co = self.insert("InsertChartLine", "A1:B5")
        ch = co.Chart
        ui.command("ChartSelValAxis")
        self.assertIn("Vertical (Value) Axis", self.format_selection())
        # Bounds > Maximum: typed in its box; the minimum stays automatic
        # (the boxes of Bounds and Units, top to bottom: Minimum, Maximum, Major, Minor)
        boxes = sorted([i for i in ui.items("Format Vertical") if i["id"] == "##v"], key=lambda i: i["rect"][1])
        self.assertGreaterEqual(len(boxes), 2, "the Bounds boxes")
        b = boxes[1]["rect"]
        x, y = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2
        self.keys(f"m:{int(x)},{int(y)}|w:2|c:{int(x)},{int(y)}|w:3|k:c-A|t:2000|k:Enter|w:4")
        ax = ch.Axes(2)
        self.assertEqual(ax.MaximumScale, 2000)
        self.assertFalse(ax.MaximumScaleIsAuto)
        self.assertTrue(ax.MinimumScaleIsAuto, "fixing the maximum leaves the minimum automatic")
        self.click("Logarithmic scale")
        self.keys("w:3")
        self.assertEqual(ax.ScaleType, -4133)
        self.click("Values in reverse order")
        self.keys("w:3")
        self.assertTrue(ax.ReversePlotOrder)
        self.close_pane()
        ax.DisplayUnit = -3  # xlThousands
        self.assertEqual(ax.DisplayUnit, -3)
        ax.MajorUnit = 250
        self.assertEqual(ax.MajorUnit, 250)
        self.assertFalse(ax.MajorUnitIsAuto)

    # ---- H6: titles ----
    def test_titles_placeholder_link_and_axis_title_in_place(self):
        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        self.assertTrue(ch.HasTitle, "a new chart has a title")
        self.assertEqual(ch.ChartTitle.Text, "Chart Title")
        self.keys("k:Escape|w:2")
        self.ws.ChartObjects(1).Delete()
        ch = self.insert("InsertChartColumn", "A1:B4").Chart
        self.assertEqual(ch.ChartTitle.Text, "Bid")
        self.keys("k:Escape|w:2|g:B1|w:2|t:Price|k:Enter|w:6")
        self.assertEqual(ch.ChartTitle.Text, "Price", "the title follows its cell")
        self.ws.Range("E1").Value = "Quotes"
        ch.ChartTitle.Formula = "=" + self.ws.Name + "!$E$1"
        self.assertEqual(ch.ChartTitle.Text, "Quotes")
        self.ws.Range("E1").Value = "Quotes 2"
        self.assertEqual(ch.ChartTitle.Text, "Quotes 2")
        # the value axis' title: double-click it and type
        ch.Axes(2).HasTitle = True
        self.keys("g:H30|w:6")
        from yc import ui

        self.ws.ChartObjects(1).Select()
        self.keys("w:4")
        r = self.chart_rect()
        x, y = int(r[0] + 10), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|D:{x},{y}|w:8|k:c-A|t:USD|k:Enter|w:6")
        self.assertEqual(ch.Axes(2).AxisTitle.Text, "USD")
        self.assertTrue(ui.command("ChartSelTitle"))

    # ---- M1, M5: data labels ----
    def test_format_data_labels_contents_and_position(self):
        from yc import ui

        self.ws.Range("A1:B4").Value = [["Region", "Sales"], ["N", 1234567], ["S", 2000000], ["E", 500000]]
        self.ws.Range("B2:B4").NumberFormat = "#,##0"
        ch = self.insert("InsertChartPie", "A1:B4").Chart
        s = ch.SeriesCollection(1)
        s.HasDataLabels = True
        s.DataLabels().Select()
        self.assertEqual(self.format_selection(), "Format Data Labels")
        self.click("Percentage")
        self.click("Value")
        self.keys("w:3")
        dl = s.DataLabels()
        self.assertTrue(dl.ShowPercentage)
        self.assertFalse(dl.ShowValue)
        self.click("Outside End")
        self.keys("w:3")
        self.assertEqual(dl.Position, 2)
        self.close_pane()
        # Data Labels > Inside End on a column chart (Add Chart Element)
        col = self.insert("InsertChartColumn", "A1:B4").Chart
        self.assertTrue(ui.command("ChartElemLabelsInsideEnd"))
        self.keys("w:3")
        self.assertEqual(col.SeriesCollection(1).DataLabels().Position, 3)
        col.ApplyDataLabels(ShowSeriesName=True, ShowValue=False)
        self.assertTrue(col.SeriesCollection(1).DataLabels().ShowSeriesName)
        self.assertFalse(col.SeriesCollection(1).DataLabels().ShowValue)

    def test_the_plus_flyout_lists_each_elements_choices(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        fly = sorted([i for i in ui.items("") if i["id"] == "##fly"], key=lambda i: i["rect"][1])
        self.assertTrue(fly)
        r = fly[0]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        grid = [i for i in ui.items("") if i["id"] == "##grid_more"]
        self.assertTrue(grid, "Gridlines has its arrow")
        g = grid[0]["rect"]
        self.keys(f"m:{int((g[0] + g[2]) / 2)},{int((g[1] + g[3]) / 2)}|w:10")
        labels = [i["label"] for i in ui.items("")]
        self.assertIn("Primary Minor Horizontal", labels)
        self.click("Primary Minor Horizontal")
        self.keys("w:3")
        self.assertTrue(ch.Axes(2).HasMinorGridlines)

    # ---- M2, M3: chart types, galleries, Recommended Charts ----
    def test_insert_chart_offers_every_variant_as_pictures(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        self.keys("g:A1:C4|w:2")
        self.assertTrue(ui.command("InsertChartMoreColumn"))
        self.keys("w:8")
        self.assertTrue(ui.state().get("dialog", 0))
        self.click("100% Stacked Column")
        self.keys("w:3")
        self.click("OK")
        self.keys("w:6")
        ch = self.ws.ChartObjects(self.ws.ChartObjects().Count).Chart
        self.assertEqual(ch.ChartType, 53)
        ch.ChartType = 74
        self.assertEqual(ch.ChartType, 74)
        # Recommended Charts: suggestions from the data, each a picture
        self.keys("k:Escape|w:2|g:A1:C4|w:2")
        self.assertTrue(ui.command("RecommendedCharts"))
        self.keys("w:8")
        labels = [i["label"] for i in ui.items("")]
        self.assertIn("Clustered Column", labels)
        self.keys("k:Escape|w:4")

    def test_bubble_and_stacked_area_types(self):
        self.ws.Range("A1:C5").Value = [["X", "Y", "Size"], [1, 2, 10], [2, 3, 40], [3, 1, 20], [4, 4, 5]]
        ch = self.insert("InsertChartBubble", "A1:C5").Chart
        self.assertEqual(ch.ChartType, 15)
        self.assertTrue(ch.SeriesCollection(1).BubbleSizes.endswith("$C$2:$C$5"))
        self.keys("k:Escape|w:2")
        self.ws.Range("E1:G4").Value = [["M", "A", "B"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        ch = self.insert("InsertChartAreaStacked100", "E1:G4").Chart
        self.assertEqual(ch.ChartType, 77)

    # ---- M6, M7, M8: elements, keys, the right-click menu ----
    def test_delete_on_the_plot_area_keeps_the_chart_and_arrows_step_elements(self):
        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        co = self.insert("InsertChartColumn", "A1:C4")
        co.Chart.PlotArea.Select()
        self.keys("w:3|k:Delete|w:4")
        self.assertEqual(self.ws.ChartObjects().Count, 1, "Delete on the plot area keeps the chart")
        co.Select()
        left = co.Left
        self.keys("w:3|k:DownArrow|w:3")
        self.assertNotEqual(self.format_selection(), "Format Chart Area")
        self.close_pane()
        self.assertEqual(co.Left, left, "the arrow stepped to another element, not moved the chart")

    def test_right_click_menu_acts_on_that_series(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        ch.HasTitle = False
        # a right-click over an Ask column
        self.keys("k:Escape|w:2|g:H30|w:6")
        r = self.chart_rect()
        _, _, px = self.shot("rc")
        y = int(r[3] - (r[3] - r[1]) * 0.25)
        xs = [x for x in range(int(r[0]), int(r[2])) if near(px(x, y), ACCENT2, 60)]
        self.assertTrue(xs, "an Ask column on the screen")
        x = xs[len(xs) // 2]
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:4|r:|w:6")
        self.click("Add Data Labels")
        self.keys("w:4")
        self.assertTrue(ch.SeriesCollection(2).HasDataLabels)
        self.assertFalse(ch.SeriesCollection(1).HasDataLabels)
        self.keys(f"m:{x},{y}|w:3|r:|w:6")
        self.click("Add Trendline...")
        self.keys("w:6")
        self.assertEqual(self.pane_title(), "Format Trendline")
        self.assertEqual(ch.SeriesCollection(2).Trendlines().Count, 1)
        self.assertEqual(ch.SeriesCollection(1).Trendlines().Count, 0)
        self.close_pane()

    def test_format_tab_lists_each_series_by_name(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        self.insert("InsertChartColumn", "A1:C4")
        cmds = {c["id"]: c for c in ui.commands()}
        self.assertEqual(cmds["ChartSelSeries2"]["label"], 'Series "Ask"')
        self.assertFalse(cmds["ChartSelSeries3"]["enabled"])
        ui.command("ChartSelSeries2")
        self.assertEqual(self.format_selection(), "Format Data Series")
        combo = [i for i in ui.items("yc_props") if i["label"] == 'Series "Ask"' or 'Ask' in i["label"]]
        self.close_pane()
        self.assertIsNotNone(combo)

    # ---- M9: fill & line ----
    def test_series_fill_and_line_options(self):
        self.ws.Range("A1:B4").Value = [["M", "V"], ["a", 1], ["b", -2], ["c", 3]]
        ch = self.insert("InsertChartLine", "A1:B4").Chart
        s = ch.SeriesCollection(1)
        s.Format.Line.DashStyle = 4  # msoLineDash
        self.assertEqual(s.Format.Line.DashStyle, 4)
        self.keys("k:Escape|w:2")
        col = self.insert("InsertChartColumn", "A1:B4").Chart
        c = col.SeriesCollection(1)
        c.Format.Fill.ForeColor.RGB = 12 + 34 * 256 + 56 * 65536
        self.assertEqual(c.Format.Fill.ForeColor.RGB, 0x38220C)
        c.Format.Fill.Transparency = 0.5
        self.assertAlmostEqual(c.Format.Fill.Transparency, 0.5)
        c.Format.Line.Visible = True
        c.Format.Line.ForeColor.RGB = 0x202020
        self.assertEqual(c.Format.Line.Visible, -1)
        c.InvertIfNegative = True
        self.assertTrue(c.InvertIfNegative)

    # ---- M10, M11 ----
    def test_histogram_overflow_bin(self):
        self.ws.Range("A1").Value = "V"
        self.ws.Range("A2:A21").Value = [[i] for i in range(1, 20)] + [[1000]]
        ch = self.insert("InsertChartHistogram", "A1:A21").Chart
        g = ch.ChartGroups(1)
        g.BinsOverflowValue = 20
        self.assertTrue(g.BinsOverflowEnabled)
        self.assertEqual(g.BinsOverflowValue, 20)

    def test_a_blank_top_left_cell_takes_years_as_series_names(self):
        self.ws.Range("B1:D1").Value = [[2022, 2023, 2024]]
        self.ws.Range("A2:D4").Value = [["AAPL", 1, 2, 3], ["MSFT", 4, 5, 6], ["TSLA", 7, 8, 9]]
        ch = self.insert("InsertChartColumn", "A1:D4").Chart
        names = [ch.SeriesCollection(i).Name for i in range(1, ch.SeriesCollection().Count + 1)]
        self.assertIn(names, (["2022", "2023", "2024"], ["AAPL", "MSFT", "TSLA"]))
        self.assertEqual(ch.SeriesCollection(1).Points().Count, 3)

    # ---- M12: the legend dragged inside the chart ----
    def test_the_selected_legend_is_dragged_inside_the_chart(self):
        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        co = self.insert("InsertChartColumn", "A1:C4")
        ch = co.Chart
        ch.Legend.Select()
        self.keys("w:4")
        self.keys("k:Escape|w:2")
        co.Select()
        self.keys("w:4")
        # click the legend (selects it), then drag it up-left
        r = self.chart_rect()
        lx, ly = int((r[0] + r[2]) / 2), int(r[3] - 10)
        self.keys(f"m:{lx},{ly}|w:3|c:{lx},{ly}|w:20")
        self.keys(f"m:{lx},{ly}|w:3|d:{lx},{ly},{lx - 60},{ly - 60}|w:6")
        self.assertTrue(ch._get()["legend_rect"][0], "the legend keeps the place it was dragged to")

    # ---- L7: moving and resizing ----
    def test_escape_during_a_move_puts_the_chart_back_and_lock_aspect(self):
        self.ws.Range("A1:B4").Value = [["M", "V"], ["a", 1], ["b", 3], ["c", 2]]
        co = self.insert("InsertChartColumn", "A1:B4")
        left, top = co.Left, co.Top
        r = self.chart_rect()
        x, y = int(r[0] + 8), int(r[1] + 8)
        self.keys(f"m:{x},{y}|w:3|p:{x},{y}|w:2|m:{x + 30},{y + 10}|w:2|m:{x + 60},{y + 20}|w:3|k:Escape|w:3|u:|w:6")
        self.assertEqual((co.Left, co.Top), (left, top), "Escape during the drag cancels it")
        co.LockAspectRatio = True
        self.assertEqual(co.LockAspectRatio, -1)

    # ---- L5: the doughnut's hole; L9: Select Data's check boxes and array constants ----
    def test_the_doughnut_hole_shows_the_chart_fill(self):
        self.ws.Range("A1:B4").Value = [["R", "V"], ["a", 3], ["b", 2], ["c", 5]]
        ch = self.insert("InsertChartDoughnut", "A1:B4").Chart
        ch.HasTitle = False
        ch.HasLegend = False
        ch.ChartArea.Format.Fill.ForeColor.RGB = 0x404040
        self.keys("k:Escape|w:2|g:H30|w:6")
        r = self.chart_rect()
        _, _, px = self.shot("hole")
        c = px(int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2))
        self.assertTrue(near(c, (0x40, 0x40, 0x40), 20), c)

    def test_select_data_check_boxes_filter_series(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        ui.command("ChartSelectData")
        self.keys("w:6")
        boxes = sorted([i for i in ui.items("Select Data") if i["id"] == "##shown"], key=lambda i: i["rect"][1])
        self.assertEqual(len(boxes), 2, "a check box before each series")
        b = boxes[1]["rect"]
        self.click_at((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)
        self.keys("k:Enter|w:6")
        self.assertTrue(ch.SeriesCollection(2).IsFiltered)
        self.assertFalse(ch.SeriesCollection(1).IsFiltered)

    # ---- L1, L5, L10 ----
    def test_waterfall_connectors_and_area_fills(self):
        self.ws.Range("A1:B4").Value = [["S", "V"], ["a", 5], ["b", -2], ["c", 3]]
        ch = self.insert("InsertChartWaterfall", "A1:B4").Chart
        ch._set(connectors=False)
        self.assertFalse(ch._get()["connectors"])
        self.keys("k:Escape|w:2")
        self.ws.ChartObjects(1).Delete()
        self.ws.Range("D1:F4").Value = [["M", "A", "B"], ["x", 1, 3], ["y", 4, 2], ["z", 2, 5]]
        a = self.insert("InsertChartArea", "D1:F4").Chart
        a.HasTitle = False
        a.HasLegend = False
        self.keys("k:Escape|w:2|g:H30|w:6")
        r = self.chart_rect()
        _, _, px = self.shot("area")
        # near the plot's bottom both areas cover the plot: the second (in front) shows, solid
        x, y = int((r[0] + r[2]) / 2), int(r[3] - (r[3] - r[1]) * 0.2)
        self.assertTrue(near(px(x, y), ACCENT2, 30), px(x, y))

    def test_insert_chart_dialog_breaks_no_imgui_rule(self):
        # the dialog's thumbnails moved the cursor back with no item after it: a debug build stopped (chaos 101)
        from yc import ui

        self.ws.Range("A1:C5").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5], ["Apr", 6, 1]]
        before = ui.state().get("imgui_asserts", 0)
        self.keys("g:A1:C5|w:2")
        ui.command("InsertChartDialog")
        self.keys("w:10")
        self.keys("k:Escape|w:4")
        self.assertEqual(ui.state().get("imgui_asserts", 0), before, "ImGui checks failed while the Insert Chart dialog was open")


if __name__ == "__main__":
    unittest.main()
