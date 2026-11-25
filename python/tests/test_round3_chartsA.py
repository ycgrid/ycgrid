# SPDX-License-Identifier: Apache-2.0
"""Round 3 (chartsA): formatting chart elements as Excel does, each checked the way a person does it: the Home tab's font
buttons on a selected title or axis, the Format pane's Fill & Line and Text Options, the Format tab's Current
Selection box, second clicks on a legend entry and a data label, typing = in the formula bar with a title selected,
the + flyout's live preview, cells pasted into a chart and a chart pasted as a picture.

Run: python -m unittest tests.test_round3_chartsA (with the bundled Python).
"""
from __future__ import annotations

import os
import struct
import sys
import tempfile
import time
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402
from tests.test_round2_excel_b import near, read_png  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


def write_png(path, w, h, rgb):
    """A plain RGB PNG of one color (a picture to fill a chart with)."""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class ChartsATest(unittest.TestCase):
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
        self.asserts0 = ui.state().get("imgui_asserts", 0)

    def tearDown(self):
        from yc import ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")
        self.assertEqual(ui.state().get("imgui_asserts", 0), self.asserts0, "no ImGui check failed")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def click_at(self, x, y, double=False):
        self.keys(f"m:{int(x)},{int(y)}|w:3|{'D' if double else 'c'}:{int(x)},{int(y)}|w:6")

    def items(self, label=None, ident=None, window=""):
        from yc import ui

        hits = [i for i in ui.items(window) if (label is None or i["label"] == label) and (ident is None or i["id"] == ident)]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def click(self, label=None, ident=None, window="", nth=0):
        hits = self.items(label, ident, window)
        self.assertGreater(len(hits), nth, f"{label or ident!r} isn't on the screen")
        r = hits[nth]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def tab(self, name):
        from yc import ui

        tabs = ui.state()["ribbon"]["tabs"]
        self.assertIn(name, tabs, f"the {name} tab isn't shown")
        x, y = tabs[name]
        time.sleep(0.55)
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:6")

    def ribbon(self, cmd, where=None):
        from yc import ui

        hits = [i for i in ui.ribbon_items() if i["cmd"] == cmd and (where is None or i["where"] == where)]
        self.assertTrue(hits, f"{cmd} isn't on the ribbon now")
        return hits[0]["rect"]

    def press_ribbon(self, cmd):
        r = self.ribbon(cmd)
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def insert(self, cmd, addr):
        from yc import ui

        self.keys(f"g:{addr}|w:2")
        self.assertTrue(ui.command(cmd))
        self.keys("w:6")
        return self.ws.ChartObjects(self.ws.ChartObjects().Count)

    def pane_title(self):
        from yc import ui

        names = [w["name"] for w in ui.windows() if "yc_props" in w["name"]]
        return names[0].split("###")[0] if names else ""

    def format_selection(self):
        from yc import ui

        ui.command("ChartFormatSelection")
        self.keys("w:6")
        return self.pane_title()

    def pane_click(self, label):
        """Click a control of the Format pane, scrolling the pane down to it as a person does."""
        from yc import ui

        for _ in range(12):
            if self.items(label):
                break
            pane = [w for w in ui.windows() if "yc_props" in w["name"]]
            self.assertTrue(pane, "the Format pane is open")
            r = pane[0]["rect"]
            self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:2|W:-3|w:4")
        self.click(label)

    def chart_rect(self):
        obj = self.items(ident="##obj")
        self.assertTrue(obj, "the chart on the screen")
        return max(obj, key=lambda i: (i["rect"][2] - i["rect"][0]) * (i["rect"][3] - i["rect"][1]))["rect"]

    def shot(self, name):
        from yc import ui

        png = os.path.join(tempfile.gettempdir(), f"yc_r3ca_{name}.png")
        ui.screenshot(png, max_width=4000)
        return read_png(png)

    def quotes(self):
        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]

    # ---- L11: Home > Font on the selected chart element; each axis its own font ----
    def test_home_font_buttons_format_the_selected_title_and_axis(self):
        self.quotes()
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        ch.ChartTitle.Select()
        self.keys("w:4")
        self.tab("Home")
        self.press_ribbon("Italic")
        self.assertTrue(ch.ChartTitle.Font.Italic, "Home > Italic slants the selected title")
        self.keys("k:c-B|w:4")
        self.assertTrue(ch.ChartTitle.Font.Bold, "Ctrl+B bolds the selected title")
        self.assertFalse(self.ws.Range("A1").Font.Italic, "the cells under the chart keep their font")
        # the value axis: its own size; the category axis keeps 9 pt
        ch.Axes(2).Select()
        self.keys("w:4")
        self.press_ribbon("FontSizeIncrease")
        self.assertEqual(ch.Axes(2).TickLabels.Font.Size, 10)
        self.assertEqual(ch.Axes(1).TickLabels.Font.Size, 9)
        ch.Axes(1).TickLabels.Font.Name = "Georgia"
        self.assertEqual(ch.Axes(1).TickLabels.Font.Name, "Georgia")
        self.assertEqual(ch.Axes(2).TickLabels.Font.Name, "Calibri")
        ch.Legend.Font.Underline = 2
        self.assertEqual(ch.Legend.Font.Underline, 2)
        ch.Legend.Font.FontStyle = "Bold Italic"
        self.assertTrue(ch.Legend.Font.Bold and ch.Legend.Font.Italic)

    # ---- H5: Format Axis > Fill & Line, Number > Linked to source, Label distance ----
    def test_format_axis_line_number_linked_and_label_distance(self):
        self.quotes()
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        ax = ch.Axes(2)
        ax.Select()
        self.assertIn("Axis", self.format_selection())
        self.assertTrue(ax.TickLabels.NumberFormatLinked)
        self.pane_click("Linked to source")
        self.keys("w:3")
        self.assertFalse(ax.TickLabels.NumberFormatLinked, "unticking Linked to source keeps a format of its own")
        self.pane_click("Solid line")
        self.keys("w:3")
        self.assertEqual(ax.Format.Line.Visible, -1)
        self.assertEqual(ax.Format.Line.ForeColor.RGB, 0xBFBFBF)
        ax.Format.Line.ForeColor.RGB = 0x0000FF
        ax.Format.Line.Weight = 2
        self.assertEqual(ax.Format.Line.Weight, 2)
        ch.Axes(1).TickLabels.Offset = 300
        self.assertEqual(ch.Axes(1).TickLabels.Offset, 300)
        ax.Format.Line.Visible = False
        self.assertEqual(ax.Format.Line.Visible, 0)

    # ---- M9: Fill & Line: gradient, picture, pattern, compound lines, arrows, vary colors by point ----
    def test_series_gradient_pattern_picture_and_line_ends(self):
        self.ws.Range("A1:B4").Value = [["M", "V"], ["a", 5], ["b", 6], ["c", 7]]
        co = self.insert("InsertChartColumn", "A1:B4")
        s = co.Chart.SeriesCollection(1)
        s.Select()
        self.assertEqual(self.format_selection(), "Format Data Series")
        self.click("Gradient fill")
        self.keys("w:3")
        self.assertEqual(s.Format.Fill.Type, 3, "msoFillGradient")
        s.Format.Fill.ForeColor.RGB = 0x000080      # dark red at the top
        s.Format.Fill.BackColor.RGB = 0xFFFFFF      # white at the bottom
        self.keys("g:J30|w:6")
        x0, y0, x1, y1 = self.chart_rect()
        _, _, px = self.shot("gradient")
        # the columns run from the first color to the second: dark red, then shades between it and white
        pts = [px(x, y) for y in range(int(y0) + 2, int(y1) - 2, 2) for x in range(int(x0) + 2, int(x1) - 2, 2)]
        dark = sum(1 for c in pts if near(c, (0x80, 0, 0), 60))
        mid = sum(1 for c in pts if 0xA0 <= c[0] <= 0xF0 and 0x30 <= c[1] <= 0xD0 and abs(c[1] - c[2]) < 24)
        self.assertGreater(dark, 5, "the gradient's first color")
        self.assertGreater(mid, 20, "shades between the two colors")
        s.Format.Fill.Patterned(14)  # msoPatternDarkVertical
        self.assertEqual(s.Format.Fill.Type, 2)
        self.assertEqual(s.Format.Fill.Pattern, 14)
        pic = os.path.join(tempfile.gettempdir(), "yc_r3ca_fill.png")
        write_png(pic, 8, 8, (0, 200, 0))
        s.Format.Fill.UserPicture(pic)
        self.assertEqual(s.Format.Fill.Type, 6)
        co.Chart.ChartGroups(1).VaryByCategories = True
        self.assertTrue(co.Chart.ChartGroups(1).VaryByCategories)
        self.assertEqual(co.Chart.Legend.LegendEntries().Count, 3, "one series colored by point: its legend lists the categories")
        # a line: triple compound, an arrow at its end; the pane lists Excel's line options
        line = self.insert("InsertChartLine", "A1:B4").Chart.SeriesCollection(1)
        line.Format.Line.Style = 5
        line.Format.Line.EndArrowheadStyle = 2
        self.assertEqual(line.Format.Line.Style, 5)
        self.assertEqual(line.Format.Line.EndArrowheadStyle, 2)
        line.Select()
        self.assertEqual(self.format_selection(), "Format Data Series")
        labels = {i["label"] for i in self.items()}
        for want in ("Compound type", "Cap type", "Join type", "Begin Arrow type", "End Arrow type"):
            self.assertIn(want, labels)

    # ---- M6: a second click selects one legend entry; Delete takes it out of the legend ----
    def test_second_click_selects_a_legend_entry_and_delete_removes_it(self):
        self.quotes()
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        self.keys("g:J30|w:4")
        entry = [i for i in self.items("Ask") if i["rect"][1] > 0]
        self.assertTrue(entry, "the legend entry Ask is on the screen")
        r = entry[-1]["rect"]
        x, y = (r[0] + r[2]) / 2, (r[1] + r[3]) / 2
        self.click_at(x, y)  # the chart and its legend
        self.click_at(x, y)  # the entry
        self.assertEqual(self.format_selection(), "Format Legend Entry")
        self.keys("m:2,2|w:2")
        self.ws.ChartObjects(1).Select()
        ch.Legend.LegendEntries(2).Select()
        self.keys("w:3|k:Delete|w:4")
        self.assertEqual(ch.SeriesCollection().Count, 2, "the series stays")
        self.assertEqual(ch.Legend.LegendEntries().Count, 1, "its entry left the legend")
        # one data label: selected, deleted on its own
        s = ch.SeriesCollection(1)
        s.HasDataLabels = True
        s.DataLabels(2).Select()
        self.assertEqual(self.format_selection(), "Format Data Label")
        self.keys("k:Escape|w:2")
        s.DataLabels(2).Select()
        self.keys("w:3|k:Delete|w:4")
        self.assertFalse(s.Points(2).HasDataLabel)
        self.assertTrue(s.HasDataLabels)
        s.DataLabels(3).Offset = (0.1, -0.1)
        self.assertAlmostEqual(s.DataLabels(3).Offset[0], 0.1, places=4)
        s.DataLabels().ShowLegendKey = True
        self.assertTrue(s.DataLabels().ShowLegendKey)
        s.DataLabels().Font.Italic = True
        self.assertTrue(s.DataLabels().Font.Italic)

    # ---- M6: Format > Current Selection: the Chart Elements box ----
    def test_current_selection_box_lists_the_charts_elements(self):
        self.quotes()
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        self.ws.ChartObjects(1).Select()
        self.keys("w:4")
        self.tab("Format")
        box = self.items("Chart Elements")
        self.assertTrue(box, "the Chart Elements box is on the Format tab")
        r = box[0]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.click('Series "Ask"')
        self.keys("w:3")
        self.assertEqual(self.format_selection(), "Format Data Series")
        self.assertTrue(ch.SeriesCollection(2).Name == "Ask")

    # ---- H6: = typed with the title selected links it to the cell clicked ----
    def test_typing_equals_with_the_title_selected_links_it_to_a_cell(self):
        self.quotes()
        self.ws.Range("E1").Value = "Quotes"
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        ch.ChartTitle.Select()
        self.keys("w:4|t:=|w:4|A:E1|w:6|k:Enter|w:6")
        self.assertEqual(ch.ChartTitle.Formula.replace("'", ""), "=" + self.ws.Name + "!$E$1")
        self.assertEqual(ch.ChartTitle.Text, "Quotes")
        self.ws.Range("E1").Value = "Quotes 2"
        self.assertEqual(ch.ChartTitle.Text, "Quotes 2", "the title follows its cell")

    # ---- M5: the + flyout previews a choice under the mouse ----
    def test_plus_flyout_previews_the_legend_on_the_left(self):
        self.quotes()
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        fly = sorted(self.items(ident="##fly"), key=lambda i: i["rect"][1])
        self.assertTrue(fly)
        r = fly[0]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        arrow = self.items(ident="##legend_more")
        self.assertTrue(arrow)
        a = arrow[0]["rect"]
        self.keys(f"m:{int((a[0] + a[2]) / 2)},{int((a[1] + a[3]) / 2)}|w:10")
        left = self.items("Left")
        self.assertTrue(left, "the legend's places are listed")
        lr = left[-1]["rect"]
        self.keys(f"m:{int((lr[0] + lr[2]) / 2)},{int((lr[1] + lr[3]) / 2)}|w:10")
        self.assertEqual(ch.Legend.Position, -4107, "hovering changes nothing yet")
        entry = [i for i in self.items("Bid") if i["rect"][1] > 0]
        x0, y0, x1, y1 = self.chart_rect()
        self.assertTrue(any(e["rect"][0] < x0 + (x1 - x0) * 0.3 for e in entry), "the legend is shown at the left while Left is hovered")
        self.keys("k:Escape|w:2|k:Escape|w:6")
        entry = [i for i in self.items("Bid") if i["rect"][1] > 0]
        self.assertFalse(any(e["rect"][0] < x0 + (x1 - x0) * 0.2 and e["rect"][1] < y0 + (y1 - y0) * 0.7 for e in entry), "back at the bottom")

    # ---- H3: cells pasted into a selected chart; a chart pasted as a picture; Chart.Export ----
    def test_paste_cells_into_a_chart_and_a_chart_as_picture(self):
        from yc import ui

        self.quotes()
        co = self.insert("InsertChartColumn", "A1:B4")
        self.keys("g:C1:C4|w:2|k:c-C|w:3")  # the Name Box lets go of the new chart, then Ctrl+C copies the cells
        co.Select()
        self.keys("w:4|k:c-V|w:6")
        self.assertEqual(co.Chart.SeriesCollection().Count, 2, "the copied cells became a series")
        self.assertEqual(co.Chart.SeriesCollection(2).Name, "Ask")
        self.keys("k:Escape|w:2")
        co.Select()
        self.keys("w:4|k:c-C|w:10|g:H20|w:3")
        n = self.ws.Shapes.Count
        self.assertTrue(ui.command("PasteAsPicture"))
        self.keys("w:6")
        self.assertEqual(self.ws.Shapes.Count, n + 1, "the chart came in as a picture")
        png = os.path.join(tempfile.gettempdir(), "yc_r3ca_export.png")
        if os.path.exists(png):
            os.remove(png)
        self.assertTrue(co.Chart.Export(png))
        w, h, _ = read_png(png)
        self.assertGreater(w, 100)
        self.assertGreater(h, 60)

    # ---- the Name Box lets go of a selected chart (Excel) ----
    def test_the_name_box_lets_go_of_a_selected_chart(self):
        from yc import ui

        self.quotes()
        self.insert("InsertChartColumn", "A1:C4")
        self.assertTrue(self.conn.request("shape.selection", {}), "the new chart is selected")
        self.keys("g:E1:E3|w:3")
        self.assertFalse(self.conn.request("shape.selection", {}), "selecting cells in the Name Box lets go of it")
        self.keys("k:c-C|w:3")
        self.assertTrue(ui.state().get("copy_mode"), "Ctrl+C then copies the cells")

    # ---- a legend entry's own font ----
    def test_a_legend_entry_has_a_font_of_its_own(self):
        self.quotes()
        ch = self.insert("InsertChartColumn", "A1:C4").Chart
        ch.Legend.LegendEntries(2).Font.Size = 16
        self.assertEqual(ch.Legend.LegendEntries(2).Font.Size, 16)
        self.assertEqual(ch.Legend.LegendEntries(1).Font.Size, 9, "the other entry keeps the legend's font")
        self.assertEqual(ch.Legend.Font.Size, 9)
        self.keys("g:J30|w:4")
        bid = [i for i in self.items("Bid") if i["rect"][1] > 0][-1]
        ask = [i for i in self.items("Ask") if i["rect"][1] > 0][-1]
        self.assertGreater(ask["rect"][3] - ask["rect"][1], bid["rect"][3] - bid["rect"][1], "Ask's entry is drawn larger")
        # selected by a second click, Home > Italic italicizes only that entry
        r = bid["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.tab("Home")
        self.press_ribbon("Italic")
        self.assertTrue(ch.Legend.LegendEntries(1).Font.Italic)
        self.assertFalse(ch.Legend.LegendEntries(2).Font.Italic)
        self.assertFalse(ch.Legend.Font.Italic)
        # a pie: its entries are its categories, each with its own font
        self.keys("k:Escape|w:2")
        pie = self.insert("InsertChartPie", "A1:B4").Chart
        self.assertEqual(pie.Legend.LegendEntries().Count, 3)
        pie.Legend.LegendEntries(3).Font.Bold = True
        self.assertTrue(pie.Legend.LegendEntries(3).Font.Bold)
        self.assertFalse(pie.Legend.LegendEntries(1).Font.Bold)
        pie.Legend.LegendEntries(2).Delete()
        self.assertEqual(pie.Legend.LegendEntries().Count, 2, "a category's entry left the legend")
        self.assertTrue(pie.Legend.LegendEntries(2).Font.Bold, "the third category is now the second entry")

    # ---- one data label: its own text (typed in place or in the formula bar, or a cell's), options and font ----
    def test_one_data_label_has_its_own_text_options_and_font(self):
        self.quotes()
        self.ws.Range("E1").Value = "Note"
        ch = self.insert("InsertChartColumn", "A1:B4").Chart
        s = ch.SeriesCollection(1)
        s.HasDataLabels = True
        s.DataLabels(2).Font.Bold = True
        self.assertTrue(s.DataLabels(2).Font.Bold)
        self.assertFalse(s.DataLabels(1).Font.Bold, "the other labels keep the series' font")
        self.assertFalse(s.DataLabels().Font.Bold)
        s.DataLabels(3).ShowSeriesName = True
        self.assertTrue(s.DataLabels(3).ShowSeriesName)
        self.assertFalse(s.DataLabels(1).ShowSeriesName)
        # typed in place: the selected label clicked again
        s.DataLabels(2).Select()
        self.keys("w:4")
        lab = [i for i in self.items("3") if i["rect"][1] > 0]
        self.assertTrue(lab, "the label 3 is on the screen")
        r = lab[-1]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.keys("k:c-A|t:Peak|k:Enter|w:6")
        self.assertEqual(s.Points(2).DataLabel.Text, "Peak")
        self.assertTrue(self.items("Peak"), "the label shows its own text")
        # its own box: Fill & Line over the series' labels'
        s.DataLabels().Format.Fill.ForeColor.RGB = 0xEEEEEE
        s.DataLabels(2).Format.Fill.ForeColor.RGB = 0x00FFFF  # yellow
        self.assertEqual(s.DataLabels(2).Format.Fill.ForeColor.RGB, 0x00FFFF)
        self.assertEqual(s.DataLabels(1).Format.Fill.ForeColor.RGB, 0xEEEEEE)
        self.keys("g:J30|w:6")
        r = [i for i in self.items("Peak") if i["rect"][1] > 0][-1]["rect"]
        _, _, px = self.shot("label_box")
        yellow = sum(1 for x in range(int(r[0]), int(r[2])) for y in range(int(r[1]), int(r[3])) if near(px(x, y), (255, 255, 0), 60))
        self.assertGreater(yellow, 10, "the label's box is yellow")
        # linked to a cell in the formula bar: select it, type =, click the cell, Enter
        s.DataLabels(1).Select()
        self.keys("w:4|t:=|w:4|A:E1|w:6|k:Enter|w:6")
        self.assertEqual(s.DataLabels(1).Formula.replace("'", ""), "=" + self.ws.Name + "!$E$1")
        self.assertEqual(s.DataLabels(1).Text, "Note")
        self.ws.Range("E1").Value = "Note 2"
        self.keys("g:J30|w:6")
        self.assertTrue(self.items("Note 2"), "the label follows its cell")

    # ---- a tiled picture fills a pie slice too ----
    def test_a_tiled_picture_fills_a_pie(self):
        self.ws.Range("A1:B3").Value = [["M", "V"], ["a", 1], ["b", 1]]
        co = self.insert("InsertChartPie", "A1:B3")
        s = co.Chart.SeriesCollection(1)
        pic = os.path.join(tempfile.gettempdir(), "yc_r3ca_stripes.png")
        raw = b"".join(b"\x00" + (bytes((255, 0, 0)) * 4 + bytes((0, 0, 255)) * 4) for _ in range(8))

        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

        with open(pic, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        s.Format.Fill.UserTextured(pic)
        self.assertTrue(s.Format.Fill.TextureTile)
        s.Format.Fill.TextureHorizontalScale = 2
        self.assertAlmostEqual(s.Format.Fill.TextureHorizontalScale, 2)
        s.Format.Fill.TextureMirrorType = 1
        self.keys("g:J30|w:6")
        x0, y0, x1, y1 = self.chart_rect()
        _, _, px = self.shot("tiled_pie")
        y = int((y0 + y1) / 2)
        row = [px(x, y) for x in range(int(x0) + 2, int(x1) - 2)]
        kinds = ["r" if near(c, (255, 0, 0), 90) else "b" if near(c, (0, 0, 255), 90) else "-" for c in row]
        seq = [k for k in kinds if k != "-"]
        changes = sum(1 for k1, k2 in zip(seq, seq[1:]) if k1 != k2)
        self.assertGreater(changes, 4, "red and blue stripes repeat across the pie")

    # ---- a label dragged with its series and on its own moves once, on the screen and on paper ----
    def test_a_labels_offset_counts_once_on_screen_and_on_paper(self):
        self.ws.Range("A1:B4").Value = [["M", "V"], ["a", 1], ["b", 7], ["c", 2]]
        co = self.insert("InsertChartColumn", "A1:B4")
        s = co.Chart.SeriesCollection(1)
        s.Points(2).HasDataLabel = True
        self.keys("g:J30|w:6")

        def label_x():
            hits = [i for i in self.items("7") if i["rect"][1] > 0]
            self.assertTrue(hits, "the label 7 is on the screen")
            r = hits[-1]["rect"]
            return (r[0] + r[2]) / 2

        x0, _, x1, _ = self.chart_rect()
        w = x1 - x0
        base = label_x()
        s._set(label_dx=0.1)  # the series' labels dragged together (chartsB)
        self.keys("w:6")
        self.assertAlmostEqual((label_x() - base) / w, 0.1, delta=0.02)
        s.DataLabels(2).Offset = (0.05, 0.0)  # then this label on its own
        self.keys("w:6")
        self.assertAlmostEqual((label_x() - base) / w, 0.15, delta=0.02, msg="each offset counts once")

        # on paper (the sheet's picture is drawn by the printing code): the same shift
        def grays(path):
            pw, ph, pp = read_png(path)
            return {(x, y) for y in range(ph) for x in range(pw) if max(pp(x, y)) < 0x90 and max(pp(x, y)) - min(pp(x, y)) < 24}

        pa = os.path.join(tempfile.gettempdir(), "yc_r3ca_paper_a.png")
        pb = os.path.join(tempfile.gettempdir(), "yc_r3ca_paper_b.png")
        self.ws.ExportImage(pb)
        s._set(label_dx=0.0)
        s.DataLabels(2).Offset = (0.0, 0.0)
        self.ws.ExportImage(pa)
        ga, gb = grays(pa), grays(pb)
        only_a, only_b = ga - gb, gb - ga
        self.assertTrue(only_a and only_b, "the label moved on paper")
        shift = sum(x for x, _ in only_b) / len(only_b) - sum(x for x, _ in only_a) / len(only_a)
        chart_px = co.Width * 96 / 72
        self.assertAlmostEqual(shift / chart_px, 0.15, delta=0.04)


    def test_a_chart_over_whole_columns_or_the_whole_sheet_is_made_in_a_moment(self):
        """scale_sweep: with a value in XFD1048576 a chart over B:D took 25 s (a find per row, a million squared
        steps), and one over the whole sheet never ended. Excel 16 gives B:D the same series, rows 2 to 1048576
        (measured); over the whole sheet it didn't answer for minutes, and YCGrid inserts an empty chart."""
        from yc import ui

        ws = self.ws
        ws.Range("A1:D1").Value = [["Name", "Qty", "Price", "Total"]]
        ws.Range("A2:C6").Value = [["AAPL", 10, 1.5], ["MSFT", 20, 2.25], ["TSMC", 30, 3], ["NVDA", 40, 4.75], ["AMZN", 50, 5]]
        ws.Range("XFD1048576").Value = "far"
        self.keys("g:B:D|w:2")
        t0 = time.time()
        self.assertTrue(ui.command("InsertChartColumn"))
        self.keys("w:3")
        self.assertLess(time.time() - t0, 5.0)
        ch = ws.ChartObjects(ws.ChartObjects().Count).Chart
        self.assertEqual(ch.SeriesCollection().Count, 3)
        self.assertIn("$B$2:$B$1048576", ch.SeriesCollection(1).Formula)
        self.keys("k:Escape|w:2|g:A1|w:2|k:c-A|k:c-A|w:2")
        t0 = time.time()
        self.assertTrue(ui.command("InsertChartLine"))
        self.keys("w:3")
        self.assertLess(time.time() - t0, 20.0)
        ch = ws.ChartObjects(ws.ChartObjects().Count).Chart
        self.assertEqual(ch.SeriesCollection().Count, 0)  # 255 series of a million points: an empty chart to fill
        t0 = time.time()
        self.keys("k:Escape|w:2|g:H20|w:3")  # the sheet stays live with the chart on it
        self.assertLess(time.time() - t0, 10.0)

if __name__ == "__main__":
    unittest.main()
