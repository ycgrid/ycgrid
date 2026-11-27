# SPDX-License-Identifier: Apache-2.0
"""Round 3 (layout): Page Layout, Page Setup, printing, the View tab and the window commands working like Excel's, each
checked the way a person does it: the ribbon, the dialogs, clicks and drags on the pages.

Run: python -m unittest tests.test_round3_layout (with the bundled Python).
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


def write_png(path, w, h, rgb):
    """A solid-color RGB PNG (the standard library only)."""
    import struct
    import zlib

    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def read_png(path):
    """(width, height, pixel(x, y) -> (r, g, b)) of an 8-bit RGB / RGBA PNG (the screenshots)."""
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


class LayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        cls.tmp = tempfile.mkdtemp(prefix="yc_round3_layout_")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        excel.ActiveWindow.View = 1
        self.keys("w:2")

    def tearDown(self):
        from yc import excel, ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")
        excel.ActiveWindow.View = 1
        self.keys("w:2")

    def test_title_bar_buttons_are_clickable_with_a_real_mouse(self):
        # Windows asks WM_NCHITTEST where the mouse is: a title bar button missing from the clickable areas is part of
        # the caption, and a real click on it drags the window. The list had room for 8: with the Quick Access
        # Toolbar's buttons and arrows, minimize and the X fell off, and the X didn't close YCGrid.
        from yc import ui

        clickable = ui.state()["titlebar_clickable"]
        for ident in ("##tb_close", "##tb_min", "##tb_search"):
            item = next((i for i in ui.items() if i.get("id") == ident), None)
            self.assertIsNotNone(item, f"{ident} is on the title bar")
            x, y = (item["rect"][0] + item["rect"][2]) / 2, (item["rect"][1] + item["rect"][3]) / 2
            self.assertTrue(any(r[0] <= x < r[2] and r[1] <= y < r[3] for r in clickable),
                            f"{ident} at {x:.0f},{y:.0f} isn't clickable for Windows: {clickable}")

    def test_the_page_layout_tab_over_a_sheet_to_its_last_cell_keeps_the_frames_quick(self):
        # chaos seed 6803: with Fit To set (Scale Height 2 pages), the Page Layout tab's Scale box laid the sheet out at
        # every frame; a value in the sheet's last cell made that layout seconds long, and the window stopped answering
        from yc import excel

        self.ws.Range("XFD1048576").Value2 = "far"
        self.ws.Range("H15:H30").Value = [[i] for i in range(16)]
        self.rq("ui.command", id="ScaleHeight2")
        self.tab("Page Layout")
        self.rq("perf.stats", last=1)
        self.keys("w:20")
        t0 = time.time()
        self.keys("g:H15|w:2|k:c-I|w:10")
        self.assertLess(time.time() - t0, 10)
        # the layout made every frame: each frame ~190 ms released (seconds on a debug build); kept, it is made once
        self.keys("w:30")
        self.assertLess(self.rq("perf.stats", last=30)["frame_cpu_avg_ms"], 80)
        self.assertTrue(self.ws.Range("H15").Font.Italic)
        self.ws.Range("XFD1048576").ClearContents()
        self.tab("Home")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def rq(self, method, **params):
        return self.conn.request(method, params, timeout=20)

    def click_at(self, x, y, double=False):
        self.keys(f"m:{int(x)},{int(y)}|w:3|{'D' if double else 'c'}:{int(x)},{int(y)}|w:6")

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

    def press(self, cmd, where=None):
        r = self.ribbon(cmd, where)
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def open_menu(self, cmd):
        from yc import ui

        r = self.ribbon(cmd)
        self.click_at((r[0] + r[2]) / 2, r[3] - 8 if r[3] - r[1] > 50 else (r[1] + r[3]) / 2)
        self.assertTrue(ui.state()["ribbon"].get("popup_open"), f"{cmd}'s menu opens")

    def menu_cmds(self):
        from yc import ui

        return [i["cmd"] for i in ui.ribbon_items() if i["where"] == "menu"]

    def dialog(self):
        from yc import ui

        return ui.state().get("dialog", 0)

    def png(self, name="pic.png"):
        from yc import ui

        path = os.path.join(self.tmp, name)
        ui.screenshot(path, max_width=48)
        return path

    def page_rect(self, n=1):
        hits = self.items(f"Page {n}")
        self.assertTrue(hits, f"Page {n} is on the screen")
        return hits[0]["rect"]

    def page_scale(self, rect):
        ps = self.ws.PageSetup
        return (rect[2] - rect[0]) / (612.0 if ps.PaperSize == 1 else 595.0)

    # ---- Page Setup: the Sheet tab (Comments, Cell errors as, Draft quality) ----
    def test_sheet_tab_comments_errors_and_draft(self):
        from yc import constants as k
        from yc import ui

        ws = self.ws
        ws.Range("A1").Value = 1
        ws.Range("A1").AddComment("check this")
        self.assertEqual(ws.PageSetup.Pages.Count, 1)
        self.assertTrue(ui.command("PageSetupSheet"))
        self.keys("w:6")
        self.click("Draft quality")
        self.click(ident="Comments and notes")
        self.click("At end of sheet")
        self.click(ident="Cell errors as")
        self.click("#N/A")
        self.click("OK")
        self.assertFalse(self.dialog(), "OK closes Page Setup")
        ps = ws.PageSetup
        self.assertTrue(ps.Draft)
        self.assertEqual(ps.PrintComments, k.xlPrintSheetEnd)
        self.assertEqual(ps.PrintErrors, k.xlPrintErrorsNA)
        self.assertEqual(ps.Pages.Count, 2, "the notes print on a page after the cells")
        ps.PrintComments = k.xlPrintInPlace
        self.assertEqual(ps.Pages.Count, 1)

    # ---- Page Setup > Margins: the page picture's boxes, the fit check ----
    def test_margins_that_do_not_fit_keep_the_dialog_open(self):
        from yc import ui

        left = self.ws.PageSetup.LeftMargin
        self.assertTrue(ui.command("MarginsCustom"))
        self.keys("w:6")
        for box in ("Top", "Bottom", "Left", "Right", "Header", "Footer"):
            self.assertTrue(self.items(ident=box), f"the {box} box is on the Margins tab")
        self.click(ident="Left")
        self.keys("k:c-A|t:20|k:Tab|w:3")
        self.click("OK")
        self.assertTrue(self.dialog(), "margins wider than the paper: Page Setup stays open (Excel's fit check)")
        self.assertEqual(self.ws.PageSetup.LeftMargin, left)
        self.keys("k:Escape|w:4")

    # ---- Set Print Area over several ranges, Add to Print Area ----
    def test_several_print_areas_and_add_to_print_area(self):
        ws = self.ws
        ws.Range("A1").Value = 1
        ws.Range("A1:B2,D5:E6").Select()
        self.keys("w:3")
        self.tab("Page Layout")
        self.open_menu("PrintAreaMenu")
        self.press("SetPrintArea", "menu")
        self.assertEqual(ws.PageSetup.PrintArea.replace("$", ""), "A1:B2,D5:E6")
        self.assertEqual(ws.PageSetup.Pages.Count, 2, "each range prints on its own page")
        ws.Range("H1:H3").Select()
        self.keys("w:3")
        self.open_menu("PrintAreaMenu")
        self.assertIn("AddToPrintArea", self.menu_cmds(), "Add to Print Area is listed once there is a print area")
        self.press("AddToPrintArea", "menu")
        self.assertEqual(ws.PageSetup.PrintArea.replace("$", ""), "A1:B2,D5:E6,H1:H3")
        self.assertEqual(ws.PageSetup.Pages.Count, 3)

    # ---- Scale to Fit: Width / Height / Scale; Sheet Options' View and Print boxes ----
    def test_scale_to_fit_and_sheet_options(self):
        from yc import excel

        ps = self.ws.PageSetup
        self.tab("Page Layout")
        self.open_menu("ScaleWidthMenu")
        self.press("ScaleWidth1", "menu")
        self.assertEqual(ps.FitToPagesWide, 1)
        self.assertFalse(ps.Zoom)
        self.assertTrue(self.items(ident="Scale"), "the Scale box is in the group")
        self.open_menu("ScaleWidthMenu")
        self.press("ScaleWidth0", "menu")
        self.assertEqual(ps.Zoom, 100, "Width and Height Automatic: the scale again")
        self.click(ident="Gridlines Print")
        self.assertTrue(ps.PrintGridlines)
        before = excel.ActiveWindow.DisplayGridlines
        self.click(ident="Gridlines View")
        self.keys("w:3")
        self.assertNotEqual(excel.ActiveWindow.DisplayGridlines, before)
        excel.ActiveWindow.DisplayGridlines = True

    # ---- Background / Delete Background, Margins > Last Custom Setting ----
    def test_background_and_last_custom_margins(self):
        from yc import ui

        ui.answer_file_dialogs(self.png("bg.png"))
        self.tab("Page Layout")
        self.press("SheetBackground")
        self.keys("w:6")
        self.assertTrue(self.rq("layout.background"), "the sheet has the picture")
        self.press("DeleteBackground")  # the same button, now Delete Background (Excel)
        self.keys("w:4")
        self.assertFalse(self.rq("layout.background"))
        self.ws.SetBackgroundPicture(self.png("bg2.png"))
        self.assertTrue(self.rq("layout.background"))
        self.ws.SetBackgroundPicture("")
        # custom margins chosen in Page Setup become Margins' first entry
        self.ws.PageSetup.LeftMargin = 41
        self.assertTrue(ui.command("PageSetupDialog"))
        self.keys("w:4")
        self.click("OK")
        from yc import excel

        other = excel.Worksheets.Add()
        other.Activate()
        self.keys("w:3")
        self.tab("Page Layout")
        self.open_menu("MarginsMenu")
        self.assertIn("MarginsLast", self.menu_cmds(), "Last Custom Setting is in the Margins menu")
        self.press("MarginsLast", "menu")
        self.assertAlmostEqual(other.PageSetup.LeftMargin, 41, places=1)

    # ---- File > Print: sides, collation, the page box, Show Margins dragged ----
    def test_backstage_print_page_box_and_show_margins(self):
        from yc import ui

        ws = self.ws
        ws.Range("A1:A150").Value = [[i] for i in range(150)]
        self.assertTrue(ui.command("FilePrint"))
        self.keys("w:8")
        for ident in ("Printer", "Print what", "Sides", "Collation", "Current Page"):
            self.assertTrue(self.items(ident=ident), f"{ident} is on File > Print")
        self.assertTrue(self.items("Printer Properties"))
        st = self.rq("layout.print")
        self.assertGreaterEqual(st["pages"], 2)
        self.click(ident="Current Page")
        self.keys("k:c-A|t:2|k:Enter|w:4")
        self.assertEqual(self.rq("layout.print")["page"], 1, "typing 2 in the page box shows page 2")
        self.click("Show Margins")
        handle = self.items(ident="Left margin")
        self.assertTrue(handle, "Show Margins draws the margin lines")
        r = handle[0]["rect"]
        x, y = (r[0] + r[2]) / 2, (r[1] + r[3]) / 2
        left = ws.PageSetup.LeftMargin
        self.keys(f"m:{int(x)},{int(y)}|w:3|d:{int(x)},{int(y)},{int(x + 40)},{int(y)}|w:6")
        self.assertGreater(ws.PageSetup.LeftMargin, left + 5, "the left margin line was dragged right")
        self.keys("k:Escape|w:4")

    def test_print_selected_chart(self):
        from yc import ui

        ws = self.ws
        ws.Range("A1:B4").Value = [["M", "S"], ["a", 1], ["b", 2], ["c", 3]]
        self.keys("g:A1:B4|w:2")
        self.assertTrue(ui.command("InsertChartColumn"))
        self.keys("w:6")
        ws.ChartObjects(1).Select()
        self.keys("w:4")
        self.assertTrue(ui.command("FilePrint"))
        self.keys("w:6")
        st = self.rq("layout.print")
        self.assertGreaterEqual(st["chart"], 0, "Print Selected Chart is chosen")
        self.assertEqual(st["pages"], 1, "the chart alone on one page")
        self.keys("k:Escape|w:4")

    # ---- Page Layout view: rulers, the fill handle, "Click to add data", following the active cell ----
    def test_page_layout_view_rulers_fill_handle_and_added_pages(self):
        from yc import excel, ui

        ws = self.ws
        ws.Range("A1:A2").Value = [[1], [2]]
        ws.Columns("A:C").ColumnWidth = 8.43
        ws.Rows("1:10").RowHeight = 15
        self.assertTrue(ui.command("ViewPageLayout"))
        self.keys("g:A1|m:2,2|w:12")
        self.assertTrue(self.items(ident="Ruler left margin"), "the ruler is shown with its margins")
        self.assertTrue(self.items("Click to add data"), "the pages after the data say Click to add data")
        x0, y0, x1, y1 = self.page_rect()
        sc = self.page_scale((x0, y0, x1, y1))
        ps = ws.PageSetup

        def cell(col, row):
            return x0 + (ps.LeftMargin + 48 * col + 24) * sc, y0 + (ps.TopMargin + 15 * row + 7.5) * sc

        ax, ay = cell(0, 0)
        bx, by = cell(0, 1)
        self.keys(f"m:{int(ax)},{int(ay)}|w:4|d:{int(ax)},{int(ay)},{int(bx)},{int(by)}|w:6")
        self.assertEqual(ui.state()["selection"].replace("$", ""), "A1:A2")
        h = self.items(ident="Fill handle")
        self.assertTrue(h, "the fill handle is at the selection's corner")
        hx, hy = (h[0]["rect"][0] + h[0]["rect"][2]) / 2, (h[0]["rect"][1] + h[0]["rect"][3]) / 2
        tx, ty = cell(0, 3)
        self.keys(f"m:{int(hx)},{int(hy)}|w:4|d:{int(hx)},{int(hy)},{int(tx)},{int(ty)}|w:8")
        self.assertEqual(ws.Range("A3").Value, 3)
        self.assertEqual(ws.Range("A4").Value, 4)
        # the ruler's left margin dragged right
        r = self.items(ident="Ruler left margin")[0]["rect"]
        mx, my = (r[0] + r[2]) / 2, (r[1] + r[3]) / 2
        left = ps.LeftMargin
        self.keys(f"m:{int(mx)},{int(my)}|w:3|d:{int(mx)},{int(my)},{int(mx + 30)},{int(my)}|w:6")
        self.assertGreater(ps.LeftMargin, left + 5)
        # a click on an added page selects its first cell
        self.click("Click to add data")
        self.assertNotEqual(ui.state()["selection"].replace("$", ""), "A1:A4")
        # the keys move the active cell off the page: the view follows it
        self.keys("g:A1|w:4|g:A300|w:10")
        self.assertFalse(self.items("Page 1"), "the view scrolled down to A300 (page 1 holds the first rows only)")
        self.assertTrue(self.items("Click to add data"), "A300's page, after the data, is on the screen")
        self.keys("g:A1|w:10")
        self.assertTrue(self.items("Page 1"), "and back to A1")
        excel.ActiveWindow.View = 1

    # ---- the Header & Footer tab while a header is typed ----
    def test_header_footer_tab_fields_and_options(self):
        from yc import ui

        ws = self.ws
        ws.Range("A1").Value = 1
        self.assertTrue(ui.command("ViewPageLayout"))
        self.keys("g:A1|m:2,2|w:12")
        x0, y0, x1, y1 = self.page_rect()
        sc = self.page_scale((x0, y0, x1, y1))
        ps = ws.PageSetup
        self.click_at((x0 + x1) / 2, y0 + (ps.HeaderMargin + 6) * sc)
        self.assertIn("Header & Footer", ui.state()["ribbon"]["tabs"], "typing a header shows the Header & Footer tab")
        self.press("HfPageNumber")
        self.keys("w:3|k:Enter|w:4")
        self.assertEqual(ps.CenterHeader, "&P")
        self.click_at((x0 + x1) / 2, y0 + (ps.HeaderMargin + 6) * sc)
        self.press("HfDifferentFirst")
        self.keys("w:4")
        self.assertTrue(ps.DifferentFirstPageHeaderFooter)
        self.keys("k:Escape|w:4")
        self.assertNotIn("Header & Footer", ui.state()["ribbon"]["tabs"], "the tab goes when the header is done")

    def test_cell_rect_follows_page_layout_view(self):
        # ui.cell_rect gave the grid's place in Page Layout view, so a click there selected another cell (D8 -> B1)
        from yc import ui

        self.ws.Range("A1").Value = 1
        self.assertTrue(ui.command("ViewPageLayout"))
        self.keys("g:A1|m:2,2|w:12")
        for cell in ("D8", "B3", "F12"):
            x0, y0, x1, y1 = ui.cell_rect(cell)
            self.keys(f"m:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:3|c:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:5")
            self.assertEqual(ui.state()["active_cell"], cell)
        self.assertTrue(ui.command("ViewNormal"))

    def test_header_picture_and_format_text_codes(self):
        ws = self.ws
        ws.Range("A1").Value = 1
        ps = ws.PageSetup
        ps.CenterHeader = '&"Arial,Bold"&14Title &G'
        ps.CenterHeaderPicture.Filename = self.png("logo.png")
        ps.CenterHeaderPicture.Height = 20
        self.assertTrue(ps.CenterHeaderPicture.Filename.endswith("logo.png"))
        self.assertGreater(ps.CenterHeaderPicture.Width, 0)
        out = os.path.join(self.tmp, "hf.pdf")
        ws.ExportAsFixedFormat(0, out)
        self.assertTrue(os.path.exists(out) and os.path.getsize(out) > 500)

    # ---- View: Zoom > Fit selection, Custom Views, Ruler ----
    def test_zoom_fit_selection_and_custom_views(self):
        from yc import excel, ui

        self.keys("g:A1:C60|w:3")
        self.assertTrue(ui.command("ZoomDialog"))
        self.keys("w:4")
        self.click("Fit selection")
        self.click("OK")
        self.assertNotEqual(excel.ActiveWindow.Zoom, 100, "Fit selection zooms to the selected cells")
        excel.ActiveWindow.Zoom = 130
        self.tab("View")
        self.press("CustomViews")
        self.click("Add...")
        self.keys("t:Mine|w:2|k:Enter|w:4")
        wb = excel.ActiveWorkbook
        self.assertIn("Mine", [v.Name for v in wb.CustomViews])
        excel.ActiveWindow.Zoom = 100
        self.press("CustomViews")
        self.click("Mine")
        self.click("Show")
        self.keys("w:3")
        self.assertEqual(excel.ActiveWindow.Zoom, 130)
        wb.CustomViews("Mine").Delete()
        excel.ActiveWindow.Zoom = 100

    # ---- View > Window: Arrange Windows, Hide / Unhide, View Side by Side with Synchronous Scrolling ----
    def test_arrange_hide_unhide_and_side_by_side(self):
        from yc import excel, ui

        self.assertTrue(ui.command("NewWindow"))
        self.keys("w:6")
        self.assertTrue(ui.command("ArrangeAll"))
        self.keys("w:4")
        self.click("Horizontal")
        self.click("OK")
        self.keys("w:6")
        host = next(w for w in ui.windows() if "yc_gridhost" in w["name"])
        xw = [w for w in ui.windows() if "yc_xwin" in w["name"]]
        self.assertTrue(xw)
        self.assertGreater(xw[0]["rect"][1], host["rect"][1] + 50, "Horizontal: the second window below the first")
        self.assertTrue(ui.command("SwitchWindow0"))
        self.assertTrue(ui.command("HideWindow"))
        self.keys("w:4")
        self.assertFalse(excel.ActiveWindow.Visible)
        self.assertTrue(ui.command("UnhideWindow"))
        self.keys("w:4")
        self.click("OK")
        self.assertTrue(excel.ActiveWindow.Visible)
        excel.Windows.CompareSideBySideWith()
        self.keys("w:6")
        self.assertTrue(self.rq("layout.window")["side_by_side"])
        self.assertTrue(excel.Windows.SyncScrollingSideBySide)
        before = self.rq("layout.window")["windows"]
        excel.ActiveWindow.ScrollRow = 40
        self.keys("w:6")
        self.assertTrue(before)
        excel.Windows.BreakSideBySide()
        for w in [w for w in ui.windows() if "yc_xwin" in w["name"]]:
            x1, y0 = int(w["rect"][2]) - 14, int(w["rect"][1]) + 12
            self.keys(f"m:{x1},{y0}|w:2|c:{x1},{y0}|w:4")

    # ---- File > Info's properties; File > Options applied by OK, dropped by Cancel ----
    def test_info_properties_and_options_ok_cancel(self):
        from yc import excel

        self.rq("layout.backstage", page=0)
        self.keys("w:6")
        self.click(ident="Title")
        self.keys("t:Quarterly|k:Tab|w:4")
        wb = excel.ActiveWorkbook
        self.assertEqual(wb.BuiltinDocumentProperties("Title").Value, "Quarterly")
        path = os.path.join(self.tmp, "props.ycg")
        self.keys("k:Escape|w:3")
        wb.SaveAs(path)
        self.assertTrue(wb.BuiltinDocumentProperties("Last Author").Value)
        self.assertIsNotNone(wb.BuiltinDocumentProperties("Creation Date").Value)
        style = excel.Application.ReferenceStyle
        self.rq("layout.backstage", page=4)
        self.keys("w:6")
        self.click("R1C1 reference style")
        self.click("Cancel")
        self.assertEqual(excel.Application.ReferenceStyle, style, "Cancel drops what was changed")
        self.rq("layout.backstage", page=4)
        self.keys("w:6")
        self.click("R1C1 reference style")
        self.click("OK")
        self.assertNotEqual(excel.Application.ReferenceStyle, style, "OK applies it")
        excel.Application.ReferenceStyle = style

    # ---- Format Shape > Effects, Shape Effects / Shape Styles menus, Themes > Effects ----
    def test_shape_effects_menu_pane_and_theme_effects(self):
        from yc import excel, ui

        ws = self.ws
        sp = ws.Shapes.AddShape(1, 60, 60, 120, 60)
        sp.Select()
        self.keys("w:4")
        self.tab("Shape Format")
        self.open_menu("ShapeEffectsMenu")
        sub = [i for i in ui.ribbon_items() if i["cmd"] == "ShapeFxShadowMenu"]
        self.assertTrue(sub, "Shape Effects lists Shadow")
        r = sub[0]["rect"]
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:8")
        self.press("ShapeFxShadow1", "menu")
        self.assertEqual(sp.Shadow.Visible, -1, "Outer: Offset Bottom Right")
        self.assertAlmostEqual(sp.Shadow.OffsetX, sp.Shadow.OffsetY, places=3)
        sp.Glow.Radius = 8
        sp.Glow.Color.RGB = 0x0000FF  # red (BGR)
        sp.SoftEdge.Type = 2
        sp.Reflection.Type = 4
        sp.ThreeD.BevelTopType = 3  # msoBevelCircle
        sp.ThreeD.RotationX = 30
        self.assertEqual(sp.Glow.Radius, 8)
        self.assertEqual(sp.Glow.Color.RGB, 0x0000FF)
        self.assertEqual(sp.SoftEdge.Radius, 2.5)
        self.assertEqual(sp.Reflection.Type, 4)
        self.assertEqual(sp.ThreeD.BevelTopType, 3)
        self.assertEqual(sp.ThreeD.RotationX, 30)
        out = os.path.join(self.tmp, "fx.pdf")
        ws.ExportAsFixedFormat(0, out)  # printed with its effects
        self.assertTrue(os.path.exists(out) and os.path.getsize(out) > 500)
        # Shape Styles: an Intense Effect style takes the theme's effects; Themes > Effects changes them
        sp2 = ws.Shapes.AddShape(1, 260, 60, 120, 60)
        sp2.ShapeStyle = 37  # Intense Effect - Blue, Accent 1
        wb = excel.ActiveWorkbook
        wb.Theme.ThemeEffectScheme.Load("Glow Edge")
        self.assertEqual(wb.Theme.ThemeEffectScheme.Name, "Glow Edge")
        self.assertGreater(sp2.Glow.Radius, 0)
        sp2.Select()
        self.keys("w:3")
        self.tab("Page Layout")
        self.open_menu("ThemeEffectsMenu")
        self.press("ThemeEffects0", "menu")  # Office
        self.assertEqual(wb.Theme.ThemeEffectScheme.Name, "Office")
        self.assertEqual(sp2.Glow.Radius, 0)
        self.assertEqual(sp2.Shadow.Visible, -1, "Office's intense effect is a shadow")
        self.assertEqual(ui.state().get("imgui_asserts", 0), 0)

    # ---- View Side by Side with another YCGrid: Synchronous Scrolling both ways over YCP ----
    def test_side_by_side_scrolls_another_ycgrid(self):
        from yc import excel

        path = os.path.join(self.tmp, "PartnerB.ycg")
        excel.ActiveWorkbook.SaveCopyAs(path)
        other = yc.launch(path, hidden=True, exe=EXE, lang="en")
        try:
            conn_b = yc.connect(target=other.pid)
            yc.connect(target=self.proc.pid)  # back to this one for excel.*
            excel.Windows.CompareSideBySideWith("PartnerB.ycg")
            deadline = time.time() + 10
            while time.time() < deadline and not conn_b.request("layout.window", {}).get("side_by_side"):
                time.sleep(0.2)
            self.assertTrue(conn_b.request("layout.window", {})["side_by_side"], "the other YCGrid is side by side too")
            self.assertEqual(self.rq("layout.window")["partner"], other.pid)
            a0 = self.rq("view.get")["scroll_row"]
            b0 = conn_b.request("view.get", {})["scroll_row"]
            self.rq("view.set", scroll_row=a0 + 40)
            deadline = time.time() + 10
            while time.time() < deadline and conn_b.request("view.get", {})["scroll_row"] != b0 + 40:
                time.sleep(0.2)
            self.assertEqual(conn_b.request("view.get", {})["scroll_row"], b0 + 40, "scrolling here scrolls the other YCGrid")
            conn_b.request("view.set", {"scroll_row": b0 + 70})
            deadline = time.time() + 10
            while time.time() < deadline and self.rq("view.get")["scroll_row"] != a0 + 70:
                time.sleep(0.2)
            self.assertEqual(self.rq("view.get")["scroll_row"], a0 + 70, "and the other way round")
            excel.Windows.BreakSideBySide()
            deadline = time.time() + 10
            while time.time() < deadline and conn_b.request("layout.window", {}).get("side_by_side"):
                time.sleep(0.2)
            self.assertFalse(conn_b.request("layout.window", {})["side_by_side"])
        finally:
            other.terminate()
            other.wait(10)
            yc.connect(target=self.proc.pid)

    def test_picture_reflection_prints(self):
        from yc import ui

        ws = self.ws
        pic = os.path.join(self.tmp, "red.png")
        write_png(pic, 40, 40, (230, 20, 20))
        sp = ws.Shapes.AddPicture(pic, False, True, 20, 20, 150, 90)
        sp.Reflection.Type = 3  # full reflection, touching
        self.assertTrue(ui.command("FilePrint"))
        self.keys("w:12")
        page = self.items(ident="Print preview page")
        self.assertTrue(page, "the print preview is shown")
        x0, y0, x1, y1 = [int(v) for v in page[0]["rect"]]
        shot = os.path.join(self.tmp, "preview.png")
        ui.screenshot(shot, max_width=4000)
        w, h, px = read_png(shot)
        red = lambda c: c[0] > 180 and c[1] < 80 and c[2] < 80  # noqa: E731
        pink = lambda c: c[0] > c[1] + 25 and c[1] > 90  # noqa: E731
        # the picture on the page: its red rows; under its last one, the fading reflection (red over the paper)
        found = None
        for x in range(x0, min(x1, w), 2):
            reds = [y for y in range(y0, min(y1, h)) if red(px(x, y))]
            if reds:
                found = (x, max(reds))
                break
        self.assertTrue(found, "the picture is on the preview page")
        x, bottom = found
        below = [px(x, y) for y in range(bottom + 6, min(bottom + 30, h))]
        self.assertTrue(any(pink(c) for c in below), f"its reflection fades in below it: {below[:6]}")
        self.keys("k:Escape|w:4")

    def test_no_imgui_asserts(self):
        from yc import ui

        self.assertEqual(ui.state().get("imgui_asserts", 0), 0)


if __name__ == "__main__":
    unittest.main()
