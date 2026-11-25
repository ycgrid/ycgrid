# SPDX-License-Identifier: Apache-2.0
"""Round 2 (excel-b): charts, slicers, notes and views working like Excel's, each checked the way a person does it:
the ribbon's galleries and tabs, clicks on the sheet, keys, and what the screen then shows.

Run: python -m unittest tests.test_round2_excel_b (with the bundled Python).
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


def near(c, ref, tol=40):
    return sum(abs(a - b) for a, b in zip(c, ref)) <= tol


class ExcelBTest(unittest.TestCase):
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
        """The rect of a ribbon button or open menu entry, found by its command as a person finds it by its place."""
        from yc import ui

        hits = [i for i in ui.ribbon_items() if i["cmd"] == cmd and (where is None or i["where"] == where)]
        self.assertTrue(hits, f"{cmd} isn't on the ribbon now")
        return hits[0]["rect"]

    def open_menu(self, cmd):
        from yc import ui

        r = self.ribbon(cmd)
        self.click_at((r[0] + r[2]) / 2, r[3] - 8 if r[3] - r[1] > 50 else (r[1] + r[3]) / 2)
        self.assertTrue(ui.state()["ribbon"].get("popup_open"), f"{cmd}'s menu opens")

    def menu_entry(self, cmd):
        r = self.ribbon(cmd, "menu")
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def shot(self, name):
        from yc import ui

        png = os.path.join(tempfile.gettempdir(), f"yc_exb_{name}.png")
        ui.screenshot(png, max_width=4000)
        return read_png(png)

    def chart_rect(self):
        obj = [i for i in self.items(ident="##obj")]
        self.assertTrue(obj, "the chart on the screen")
        return max(obj, key=lambda i: (i["rect"][2] - i["rect"][0]) * (i["rect"][3] - i["rect"][1]))["rect"]

    # ---- a menu ending in a submenu (Add Chart Element > Trendline) ----
    def test_add_chart_element_menu_breaks_no_imgui_rule(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["Month", "Sales", "Cost"], ["Jan", 10, 7], ["Feb", 14, 9], ["Mar", 12, 8]]
        self.keys("g:A1:C4|w:2")
        self.assertTrue(ui.command("InsertChartColumn"))
        self.keys("w:6")
        self.ws.ChartObjects(1).Select()
        self.keys("w:4")
        self.tab("Chart Design")
        before = ui.state().get("imgui_asserts", 0)
        self.open_menu("ChartAddElementMenu")
        self.keys("w:6")
        # the cursor was moved below the last submenu with no item after it: a debug build stopped (chaos 202)
        self.assertEqual(ui.state().get("imgui_asserts", 0), before, "ImGui checks failed while the menu was open")
        self.keys("k:Escape|w:4")

    # ---- Chart Design: Quick Layout and Chart Styles ----
    def test_chart_styles_and_quick_layout_galleries(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["Month", "Sales", "Cost"], ["Jan", 10, 7], ["Feb", 14, 9], ["Mar", 12, 8]]
        self.keys("g:A1:C4|w:2")
        self.assertTrue(ui.command("InsertChartColumn"))
        self.keys("w:6")
        ch = self.ws.ChartObjects(1).Chart
        self.ws.ChartObjects(1).Select()
        self.keys("w:4")
        self.tab("Chart Design")
        # Chart Styles: the gallery's entries are pictures of the chart; Style 8 is the dark one
        self.open_menu("ChartStylesMenu")
        r = self.ribbon("ChartStyle8", "menu")
        self.assertGreater(r[3] - r[1], 40, "a style is shown as a picture, taller than a text entry")
        self.menu_entry("ChartStyle8")
        self.keys("w:4")
        self.assertEqual(ch.ChartStyle, 208)
        self.assertEqual(ch.ChartArea.Format.Fill.ForeColor.RGB, 0x404040)
        self.keys("g:J30|w:6")  # let go of the chart: its outline and handles off the picture
        x0, y0, x1, y1 = self.chart_rect()
        _, _, px = self.shot("style8")
        dark = sum(1 for y in range(int(y0) + 4, int(y1) - 4, 6) for x in range(int(x0) + 4, int(x1) - 4, 6) if near(px(x, y), (0x40, 0x40, 0x40), 30))
        total = len(range(int(y0) + 4, int(y1) - 4, 6)) * len(range(int(x0) + 4, int(x1) - 4, 6))
        self.assertGreater(dark, total // 3, "the chart area is dark on the screen")
        # Quick Layout: Layout 8 has both axis titles and no legend
        self.ws.ChartObjects(1).Select()
        self.keys("w:4")
        self.tab("Chart Design")
        self.open_menu("ChartQuickLayoutMenu")
        self.menu_entry("ChartQuickLayout8")
        self.keys("w:4")
        self.assertFalse(ch.HasLegend)
        self.assertTrue(ch.Axes(1).HasTitle and ch.Axes(2).HasTitle)
        self.assertTrue(ch.HasTitle)
        # the last layout is on the screen too (a gallery taller than the window lost its last entries)
        self.tab("Chart Design")
        self.open_menu("ChartQuickLayoutMenu")
        self.menu_entry("ChartQuickLayout11")
        self.keys("w:4")
        self.assertFalse(ch.HasTitle, "Layout 11 was clicked")
        # Python, with Excel's names
        ch.ApplyLayout(3)
        self.assertTrue(ch.HasLegend)
        self.assertEqual(ch.Legend.Position, -4107)  # xlLegendPositionBottom
        ch.ChartStyle = 201
        self.assertEqual(ch.ChartStyle, 201)
        self.assertEqual(ch.ChartArea.Format.Fill.ForeColor.RGB, 0xFFFFFF)
        with self.assertRaises(Exception):
            ch.ChartStyle = 250

    # ---- Slicers: selected by a click, the Slicer tab, Slicer Settings, Delete ----
    def _slicer_px(self, slicer):
        """The screen rect of a slicer, from its points (Left / Top / Width / Height) and the cells' scale."""
        from yc import ui

        a1, b2 = ui.cell_rect("A1"), ui.cell_rect("B2")
        sx = (b2[0] - a1[0]) / (self.ws.Columns(1).Width * 4 / 3)
        sy = (b2[1] - a1[1]) / (self.ws.Rows(1).Height * 4 / 3)
        x0, y0 = a1[0] + slicer.Left * 4 / 3 * sx, a1[1] + slicer.Top * 4 / 3 * sy
        return x0, y0, x0 + slicer.Width * 4 / 3 * sx, y0 + slicer.Height * 4 / 3 * sy

    def test_slicer_is_selected_by_a_click_and_has_its_settings(self):
        from yc import excel, ui

        self.ws.Range("A1:B7").Value = [["Region", "Sales"], ["East", 10], ["North", 20], ["West", 30], ["East", 5], ["South", 7], ["North", 1]]
        pc = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, self.ws.Name + "!A1:B7")
        pt = pc.CreatePivotTable(self.ws.Name + "!D3", "PTexb")
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.AddDataField(pt.PivotFields("Sales"))
        cache = excel.ActiveWorkbook.SlicerCaches.Add2(pt, "Region")
        sl = cache.Slicers.Add(self.ws, None, None, None, 30, 260, 150, 200)
        self.keys("g:A1|w:6")
        self.assertNotIn("Slicer", ui.state()["ribbon"]["tabs"])
        x0, y0, x1, y1 = self._slicer_px(sl)
        self.click_at((x0 + x1) / 2 - 20, y0 + 8)  # its header
        self.assertIn("Slicer", ui.state()["ribbon"]["tabs"], "a click selects the slicer: its tab shows")
        self.tab("Slicer")
        r = self.ribbon("SlicerSettings")
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.assertTrue(ui.state().get("dialog"), "Slicer Settings opens")
        self.click("Descending (Z to A)")
        self.click("Hide items with no data")
        self.click("Display header")
        self.click("OK")
        self.keys("w:4")
        self.assertEqual(cache.SortItems, excel.xlSlicerSortDescending)
        self.assertFalse(cache.ShowAllItems)
        self.assertFalse(sl.DisplayHeader)
        # descending: the first button is West (the slicer's buttons are drawn top to bottom)
        sl.DisplayHeader = True
        cache.SortItems = excel.xlSlicerSortAscending
        self.assertEqual(cache.SortItems, 2)
        # the Columns box on the Slicer tab
        self.click_at((x0 + x1) / 2 - 20, y0 + 8)
        self.tab("Slicer")
        box = [i for i in ui.items() if i["label"] == "Slicer Columns"]
        self.assertTrue(box, "the Columns box is on the Slicer tab")
        b = box[0]["rect"]
        self.click_at(b[0] + 12, (b[1] + b[3]) / 2)
        self.keys("k:c-A|t:2|k:Enter|w:4")
        self.assertEqual(sl.NumberOfColumns, 2)
        # Delete removes the selected slicer, never the cells under it
        self.click_at((x0 + x1) / 2 - 20, y0 + 8)
        before = self.ws.Range("D4").Value
        self.keys("k:Delete|w:6")
        self.assertEqual(excel.ActiveWorkbook.SlicerCaches.Count, 0, "Delete removed the slicer")
        self.assertEqual(self.ws.Range("D4").Value, before, "the cells stayed")
        self.assertNotIn("Slicer", ui.state()["ribbon"]["tabs"])

    # ---- Notes: Show/Hide Note, Show All Notes, Previous / Next ----
    def test_show_hide_note_keeps_it_on_the_sheet(self):
        from yc import ui

        self.assertNotIn("Chart Design", ui.state()["ribbon"]["tabs"], "a chart selected on another sheet leaves no Chart Design tab")
        self.ws.Range("B3").AddComment("check the open")
        self.ws.Range("E8").AddComment("second")
        self.keys("g:B3|w:2|m:2,2|w:4")
        c = ui.cell_rect("C3")
        _, _, px = self.shot("note_hidden")
        yellow = lambda p: near(p, (0xFF, 0xFF, 0xE1), 12)  # noqa: E731
        spot = (int(c[0]) + 150, int(c[1]) + 50)  # inside the box, clear of its text
        self.assertFalse(yellow(px(*spot)), "a note shows only on hover")
        self.assertTrue(ui.command("ShowHideNote"))
        self.keys("w:4")
        self.assertTrue(self.ws.Range("B3").Comment.Visible)
        _, _, px = self.shot("note_shown")
        self.assertTrue(yellow(px(*spot)), "Show/Hide Note keeps the note's box beside its cell")
        # a click on the shown note edits it in place
        self.click_at(*spot)
        self.keys("k:c-End|t: now|w:2|k:Escape|w:4")
        self.assertEqual(self.ws.Range("B3").Comment.Text(), "check the open now")
        self.assertTrue(self.ws.Range("B3").Comment.Visible, "editing keeps it shown")
        # Show All Notes: both, then neither
        self.assertTrue(ui.command("ShowAllNotes"))
        self.keys("w:2")
        self.assertTrue(self.ws.Range("E8").Comment.Visible)
        self.assertTrue(ui.command("ShowAllNotes"))
        self.keys("w:2")
        self.assertFalse(self.ws.Range("B3").Comment.Visible or self.ws.Range("E8").Comment.Visible)
        # Next / Previous Note, and Comment.Next() in Python
        self.keys("g:A1|w:2")
        ui.command("NextNote")
        self.assertEqual(ui.state()["active_cell"].replace("$", ""), "B3")
        ui.command("NextNote")
        self.assertEqual(ui.state()["active_cell"].replace("$", ""), "E8")
        ui.command("PreviousNote")
        self.assertEqual(ui.state()["active_cell"].replace("$", ""), "B3")
        self.assertEqual(self.ws.Range("B3").Comment.Next().Text(), "second")
        self.assertIsNone(self.ws.Range("E8").Comment.Next())
        # the cell menu offers Show/Hide Note on a note
        self.keys("m:2,2|R:B3|w:6")
        self.assertTrue(self.items("Show/Hide Note"), "the cell menu has Show/Hide Note")
        self.keys("k:Escape|w:3")

    # ---- Page Layout view: a working sheet on the pages ----
    def test_page_layout_view_selects_types_and_takes_the_header_in_place(self):
        from yc import ui

        self.ws.Range("A1:C2").Value = [["Sym", "Bid", "Ask"], ["TXF", 100, 101]]
        self.ws.Columns("A:C").ColumnWidth = 8.43
        self.ws.Rows("1:5").RowHeight = 15
        self.assertTrue(ui.command("ViewPageLayout"))
        self.keys("g:A1|m:2,2|w:12")
        page = self.items("Page 1")
        self.assertTrue(page, "the first page is on the screen")
        x0, y0, x1, y1 = page[0]["rect"]
        ps = self.ws.PageSetup
        # the page's scale: its width in points is the paper's (A4 or Letter, whichever the sheet uses)
        paper_w = 612.0 if ps.PaperSize == 1 else 595.0
        sc = (x1 - x0) / paper_w

        def cell(col, row):  # a cell's center on the page: margins, then 48 x 15 point cells
            return x0 + (ps.LeftMargin + 48 * col + 24) * sc, y0 + (ps.TopMargin + 15 * row + 7.5) * sc

        # a drag over the cells selects the range, as on the normal sheet
        ax, ay = cell(0, 0)
        bx, by = cell(2, 1)
        self.keys(f"m:{int(ax)},{int(ay)}|w:4|d:{int(ax)},{int(ay)},{int(bx)},{int(by)}|w:6")
        self.assertEqual(ui.state()["selection"].replace("$", ""), "A1:C2")
        # a double-click types in the cell on the page
        cx, cy = cell(1, 3)
        self.click_at(cx, cy, double=True)
        self.assertTrue(ui.state().get("edit"), "a double-click on a cell of the page opens its editor")
        self.keys("t:99|k:Enter|w:4")
        self.assertEqual(self.ws.Range("B4").Value, 99)
        # the header is typed on the page: a click in its margin, the center section, a field button
        hx = (x0 + x1) / 2
        hy = y0 + (ps.HeaderMargin + 6) * sc
        self.click_at(hx, hy)
        boxes = self.items("Center section")
        self.assertTrue(boxes, "the header's sections are on the page")
        self.keys("t:Quotes |w:2")
        # the fields are on the Header & Footer tab shown while the header is typed (round 3, as in Excel)
        r = self.ribbon("HfSheetName")
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
        self.keys("w:3|k:Enter|w:4")
        self.assertEqual(ps.CenterHeader, "Quotes &A")
        self.assertFalse(self.items("Center section"), "Enter keeps the header and closes its sections")
        ui.command("ViewNormal")
        self.keys("w:4")


if __name__ == "__main__":
    unittest.main()
