# SPDX-License-Identifier: Apache-2.0
"""Round 3 (format): the Home tab's formatting, Format Cells, styles, conditional formatting and Find & Replace working
like Excel's, each checked the way a person does it.

Run: python -m unittest tests.test_round3_format (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402
from tests.test_round2_excel_b import near, read_png  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")

xlEdgeLeft, xlEdgeTop, xlEdgeBottom, xlEdgeRight = 7, 8, 9, 10
xlContinuous, xlLineStyleNone = 1, -4142


class FormatRound3Test(unittest.TestCase):
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

    def items(self, label=None, ident=None, window=""):
        from yc import ui

        hits = [i for i in ui.items(window) if (label is None or i["label"] == label) and (ident is None or i["id"] == ident)]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def click_at(self, x, y):
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:6")

    def click(self, label=None, ident=None, window="", nth=0):
        hits = self.items(label, ident, window)
        self.assertGreater(len(hits), nth, f"{label or ident!r} isn't on the screen")
        r = hits[nth]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def ribbon(self, cmd, where=None):
        from yc import ui

        hits = [i for i in ui.ribbon_items() if i["cmd"] == cmd and (where is None or i["where"] == where)]
        self.assertTrue(hits, f"{cmd} isn't on the ribbon now")
        return hits[0]["rect"]

    def tab(self, name):
        from yc import ui

        tabs = ui.state()["ribbon"]["tabs"]
        x, y = tabs[name]
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:6")

    def open_menu(self, cmd):
        from yc import ui

        r = self.ribbon(cmd)
        self.click_at((r[0] + r[2]) / 2, r[3] - 8 if r[3] - r[1] > 50 else (r[1] + r[3]) / 2)
        self.assertTrue(ui.state()["ribbon"].get("popup_open"), f"{cmd}'s menu opens")

    def cell(self, addr):
        from yc import ui

        return ui.cell_rect(addr)

    def shot(self, name):
        from yc import ui

        png = os.path.join(tempfile.gettempdir(), f"yc_r3fmt_{name}.png")
        ui.screenshot(png, max_width=4000)
        return read_png(png)

    def dialog(self):
        from yc import ui

        return ui.state().get("dialog", 0)

    # ---- Paste Special: Comments and Notes, Validation ----
    def test_paste_special_comments_and_validation(self):
        ws = self.ws
        ws.Range("A1").Value = 7
        ws.Range("A1").AddComment("check this")
        ws.Range("A1").Validation.Add(1, 1, 5, "0")  # whole number greater than 0
        self.keys("g:A1|w:2|k:c-C|w:3|g:C1|w:2|k:c-a-V|w:6")
        self.click("Comments and Notes", window="Paste Special")
        self.keys("k:Enter|w:6")
        self.assertIsNone(ws.Range("C1").Value, "only the note is pasted")
        self.assertIsNotNone(ws.Range("C1").Comment, "the note is pasted")
        self.keys("g:A1|w:2|k:c-C|w:3|g:E1|w:2|k:c-a-V|w:6")
        self.keys("k:N|w:3|k:Enter|w:6")  # Excel's letter for Validation
        self.assertIsNone(ws.Range("E1").Value)
        self.assertEqual(ws.Range("E1").Validation.Type, 1)
        # All carries the note and the validation, as Excel's paste does
        self.keys("g:A1|w:2|k:c-C|w:3|g:G1|w:2|k:c-V|w:6")
        self.assertEqual(ws.Range("G1").Value, 7)
        self.assertIsNotNone(ws.Range("G1").Comment)
        # Python, with Excel's names
        from yc.constants import xlPasteComments

        ws.Range("A1").Copy()
        ws.Range("I1").PasteSpecial(Paste=xlPasteComments)
        self.keys("w:4")
        self.assertIsNotNone(ws.Range("I1").Comment)
        self.assertIsNone(ws.Range("I1").Value)

    # ---- Alignment tab: the orientation dial, the vertical Text box, Degrees, Text direction ----
    def test_alignment_dial_and_text_direction(self):
        ws = self.ws
        ws.Range("B2").Value = "turn"
        self.keys("g:B2|w:2|k:c-1|w:6")
        self.click("Alignment", window="Format Cells")
        dial = self.items(ident="##orient_dial", window="Format Cells")
        self.assertTrue(dial, "the dial is drawn")
        x0, y0, x1, y1 = dial[0]["rect"]
        # a click straight up from the dial's center: 90 degrees (Excel's dial)
        cx = x0 + 6 + 2
        self.click_at(cx, y0 + 10)
        self.keys("k:Enter|w:6")
        self.assertEqual(ws.Range("B2").Orientation, -4171)  # xlUpward
        # the vertical Text box stacks the letters
        self.keys("g:B2|w:2|k:c-1|w:6")
        self.click(ident="##orient_vertical", window="Format Cells")
        self.keys("k:Enter|w:6")
        self.assertEqual(ws.Range("B2").Orientation, -4166)  # xlVertical
        # Text direction: Right-to-Left
        self.keys("g:B2|w:2|k:c-1|w:6")
        self.click(ident="##text_direction", window="Format Cells")
        self.click("Right-to-Left")
        self.keys("k:Enter|w:6")
        from yc.constants import xlRTL

        self.assertEqual(ws.Range("B2").ReadingOrder, xlRTL)

    # ---- Find & Replace: Format... (find and replace by format) ----
    def test_find_and_replace_by_format(self):
        ws = self.ws
        ws.Range("A1:A3").Value = [["x"], ["x"], ["y"]]
        ws.Range("A2:A3").Font.Bold = True
        self.keys("g:A1|w:2|k:c-H|w:6")
        if self.items("Options >>"):
            self.click("Options >>")
        # Find what: x, Format...: bold (Find Format, Font tab)
        self.keys("k:c-A|t:x|w:2")
        self.click("Format...", window="Find and Replace", nth=0)
        self.assertTrue(self.items("Font", window="Find Format"), "the Find Format dialog opens")
        self.click("Font", window="Find Format")
        self.click(ident="##font_style", window="Find Format")
        self.click("Bold")
        self.keys("k:Enter|w:6")
        self.assertTrue(self.items(ident="##find_format"), "the format's preview is beside Find what")
        self.click("Replace All", window="Find and Replace")
        self.keys("w:6|k:Enter|w:4")
        self.assertEqual(ws.Range("A1").Value, "x")
        self.assertIn(ws.Range("A2").Value, (None, ""), "the bold x was replaced; the plain one wasn't")
        self.assertEqual(ws.Range("A3").Value, "y")
        self.keys("k:Escape|w:4")
        # Python: Application.FindFormat / ReplaceFormat with SearchFormat / ReplaceFormat
        from yc import excel

        excel.Application.FindFormat.Clear()
        excel.Application.FindFormat.Font.Bold = True
        hit = ws.Range("A1:A3").Find("", SearchFormat=True)
        self.assertIsNotNone(hit)
        self.assertEqual(hit.Address, "$A$2")
        excel.Application.ReplaceFormat.Clear()
        excel.Application.ReplaceFormat.Interior.Color = 0x0000FF
        ws.Range("A1:A3").Replace("", "", SearchFormat=True, ReplaceFormat=True)
        self.assertEqual(ws.Range("A3").Interior.Color, 0x0000FF)
        self.assertNotEqual(ws.Range("A1").Interior.Color, 0x0000FF)
        excel.Application.FindFormat.Clear()
        excel.Application.ReplaceFormat.Clear()

    # ---- Borders: Draw Border, Draw Border Grid, Erase Border, Line Style ----
    def test_draw_borders(self):
        ws = self.ws
        self.keys("g:H20|w:3")
        from yc import ui

        self.assertTrue(ui.command("BorderDraw"))
        self.keys("w:3")
        # a click near B2's bottom edge draws that edge; the selection stays on H20
        x0, y0, x1, y1 = self.cell("B2")
        self.click_at((x0 + x1) / 2, y1 - 2)
        self.assertEqual(ws.Range("B2").Borders(xlEdgeBottom).LineStyle, xlContinuous)
        self.assertEqual(ws.Range("B2").Borders(xlEdgeTop).LineStyle, xlLineStyleNone)
        self.assertEqual(ui.state().get("active_cell", "H20"), "H20") if "active_cell" in ui.state() else None
        # Draw Border Grid: a drag draws every line of the cells
        self.assertTrue(ui.command("BorderDrawGrid"))
        a, b = self.cell("D2"), self.cell("E3")
        self.keys(f"m:{(a[0] + a[2]) // 2},{(a[1] + a[3]) // 2}|w:3|d:{(a[0] + a[2]) // 2},{(a[1] + a[3]) // 2},{(b[0] + b[2]) // 2},{(b[1] + b[3]) // 2}|w:6")
        for addr in ("D2", "E3"):
            for edge in (xlEdgeLeft, xlEdgeTop, xlEdgeBottom, xlEdgeRight):
                self.assertEqual(ws.Range(addr).Borders(edge).LineStyle, xlContinuous, (addr, edge))
        # Erase Border: a click on D2's left edge erases it
        self.assertTrue(ui.command("BorderErase"))
        a = self.cell("D2")
        self.click_at(a[0] + 2, (a[1] + a[3]) / 2)
        self.assertEqual(ws.Range("D2").Borders(xlEdgeLeft).LineStyle, xlLineStyleNone)
        # Line Style: Thick turns Draw Border on with that line
        self.assertTrue(ui.command("BorderLineStyle12"))
        a = self.cell("G5")
        self.click_at((a[0] + a[2]) / 2, a[1] + 2)
        self.assertEqual(ws.Range("G5").Borders(xlEdgeTop).Weight, 4)  # xlThick
        self.keys("k:Escape|w:3")
        self.assertFalse(ui.state()["ribbon"].get("popup_open"))
        # Esc ended the mode: a click selects again
        a = self.cell("J5")
        self.click_at((a[0] + a[2]) / 2, a[1] + 2)
        self.assertEqual(ws.Range("J5").Borders(xlEdgeTop).LineStyle, xlLineStyleNone)
        # the Borders menu has Excel's Draw Borders entries
        r = self.ribbon("Borders")
        self.click_at(r[2] - 5, (r[1] + r[3]) / 2)
        for cmd in ("BorderDraw", "BorderDrawGrid", "BorderErase"):
            self.ribbon(cmd, "menu")
        self.keys("k:Escape|w:3")

    # ---- Format as Table: Excel's gallery of style pictures; New Table Style; Table Name box ----
    def test_format_as_table_gallery_and_new_style(self):
        from yc import excel, ui

        ws = self.ws
        ws.Range("A1:B4").Value = [["Name", "Qty"], ["a", 1], ["b", 2], ["c", 3]]
        self.keys("g:A1:B4|w:3")
        self.tab("Home")
        self.open_menu("FormatAsTable")
        tiles = [i for i in ui.ribbon_items() if i["where"] == "tile" and i["cmd"].startswith("TableStyle")]
        self.assertGreaterEqual(len({t["cmd"] for t in tiles}), 20, "the gallery shows the styles as tiles")
        r = [t for t in tiles if t["cmd"] == "TableStyleLight9"]
        self.assertTrue(r, "Light 9 is in the gallery")
        self.assertGreater(r[0]["rect"][3] - r[0]["rect"][1], 30, "a style is a picture")
        self.click_at((r[0]["rect"][0] + r[0]["rect"][2]) / 2, (r[0]["rect"][1] + r[0]["rect"][3]) / 2)
        self.keys("w:4|k:Enter|w:6")  # Create Table: OK
        self.assertEqual(ws.ListObjects.Count, 1)
        self.assertEqual(ws.ListObjects(1).TableStyle, "TableStyleLight9")
        # the header is drawn with Light 9's fill (accent 1)
        x0, y0, x1, y1 = self.cell("A1")
        self.keys("g:D10|w:4")
        _, _, px = self.shot("light9")
        self.assertTrue(near(px(int(x0) + 4, int((y0 + y1) / 2)), (0x44, 0x72, 0xC4), 60), px(int(x0) + 4, int((y0 + y1) / 2)))
        # Table Design > Table Name: typed in the ribbon's box
        self.keys("g:A2|w:4")
        self.tab("Table Design")
        box = self.items(ident="##table_name")
        self.assertTrue(box, "the Table Name box is on the ribbon")
        b = box[0]["rect"]
        self.click_at((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)
        self.keys("k:c-A|t:Sales|k:Enter|w:6")
        self.assertEqual(ws.ListObjects(1).Name, "Sales")
        # New Table Style...: a workbook style, saved in Workbook.TableStyles
        n = excel.ActiveWorkbook.TableStyles.Count
        self.assertTrue(ui.command("NewTableStyle"))
        self.keys("w:6|k:c-A|t:Mine|w:2")
        self.click("Header Row")
        self.keys("k:Enter|w:6")
        self.assertEqual(excel.ActiveWorkbook.TableStyles.Count, n + 1)
        st = excel.ActiveWorkbook.TableStyles("Mine")
        self.assertFalse(st.BuiltIn)
        st.TableStyleElements(1).Interior.Color = 0x00FF00  # xlHeaderRow
        ws.ListObjects(1).TableStyle = "Mine"
        self.keys("g:D10|w:6")
        _, _, px = self.shot("mine")
        self.assertTrue(near(px(int(x0) + 4, int((y0 + y1) / 2)), (0, 0xFF, 0), 60))
        # Clear: the table stays, without a style
        self.keys("g:A2|w:3")
        self.assertTrue(ui.command("TableStyleClear"))
        self.keys("w:4")
        self.assertEqual(ws.ListObjects.Count, 1)
        self.assertIn(ws.ListObjects(1).TableStyle, ("", "TableStyleNone"))
        # built-in styles are read-only from Python, as in Excel
        with self.assertRaises(Exception):
            excel.ActiveWorkbook.TableStyles("TableStyleMedium2").TableStyleElements(0).Interior.Color = 0

    # ---- Conditional formatting: data bar options, Format..., Clear Rules from This Table, manager scope ----
    def test_conditional_formatting_bars_format_and_scope(self):
        from yc import excel, ui

        ws = self.ws
        ws.Range("A1:A4").Value = [[1], [2], [3], [4]]
        self.keys("g:A1:A4|w:3")
        self.tab("Home")
        self.open_menu("ConditionalFormatting")
        from yc import ui as u2

        sub = [i for i in u2.ribbon_items() if i["cmd"] == "CfDataBars"]
        self.assertTrue(sub)
        r = sub[0]["rect"]
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:8")
        self.ribbon("CfBarSolidGreen", "menu")  # Excel's Solid Fill bars are in the gallery
        rr = self.ribbon("CfBarSolidGreen", "menu")
        self.click_at((rr[0] + rr[2]) / 2, (rr[1] + rr[3]) / 2)
        fc = ws.Range("A1").FormatConditions(1)
        self.assertEqual(fc.BarFillType, 0)  # solid
        fc.ShowValue = False
        fc.MinPoint.Modify(0, 2)  # xlConditionValueNumber
        self.assertEqual(fc.MinPoint.Type, 0)
        self.assertFalse(fc.ShowValue)
        # a rule's number format (Format... > Number) shows its cells in it
        ws.Range("C1").Value = 0.25
        r2 = ws.Range("C1").FormatConditions.Add(2, Formula1="=TRUE")  # xlExpression
        r2.NumberFormat = "0.0%"
        self.keys("w:4")
        self.assertEqual(ws.Range("C1").Text.strip(), "25.0%")
        # Clear Rules from This Table: only shown enabled inside a table
        ws.ListObjects.Add(1, ws.Range("E1:E3"))
        ws.Range("E2:E3").FormatConditions.AddDatabar()
        self.keys("g:E2|w:4")
        self.assertTrue(ui.command("CfClearTable"))
        self.keys("w:4")
        self.assertEqual(ws.Range("E2").FormatConditions.Count, 0)
        # the manager shows another sheet's rules
        other = excel.Worksheets.Add()
        other.Range("A1").FormatConditions.AddDatabar()
        ws.Activate()
        self.keys("w:3")
        self.assertTrue(ui.command("CfManageRules"))
        self.keys("w:6")
        self.click(ident="##scope")
        self.click(other.Name)
        self.keys("w:4")
        self.assertTrue(self.items("Data Bar") or any("Bar" in i["label"] for i in ui.items()), "the other sheet's rule is listed")
        self.keys("k:Escape|w:4")

    # ---- Highlight Cells Rules: the rule is previewed on the sheet while its dialog is open ----
    def test_quick_rule_previews_while_open(self):
        ws = self.ws
        ws.Range("B2:B3").Value = [[10], [1]]
        self.keys("g:B2:B3|w:3")
        from yc import ui

        self.assertTrue(ui.command("CfGreater"))
        self.keys("w:6|k:c-A|t:5|w:8")
        x0, y0, x1, y1 = self.cell("B2")
        b3 = self.cell("B3")
        _, _, px = self.shot("preview")
        c = px(int(x0) + 4, int((y0 + y1) / 2))
        plain = px(int(b3[0]) + 4, int((b3[1] + b3[3]) / 2))
        # (the modal dialog dims the sheet: B2 is a dimmed light red, B3 a dimmed white)
        self.assertGreater(c[0] - c[1], 20, f"B2 shows the light red fill while the dialog is open: {c}")
        self.assertLess(abs(plain[0] - plain[1]), 8, f"B3 (1 isn't greater than 5) has none: {plain}")
        self.keys("k:Escape|w:6")
        self.assertEqual(ws.Range("B2").FormatConditions.Count, 0, "Cancel leaves no rule")

    # ---- Home > Format > Tab Color: the palette in the submenu ----
    def test_tab_color_submenu_palette(self):
        self.tab("Home")
        self.open_menu("FormatCellsMenu")
        sub = [i for i in __import__("yc").ui.ribbon_items() if i["cmd"] == "TabColor"]
        self.assertTrue(sub)
        r = sub[0]["rect"]
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:8")
        self.click("Accent 2")
        self.assertNotIn(self.ws.Tab.Color, (None, -4142, 0))

    # ---- Cell Styles: the command opens the gallery; Merge Styles is at its end ----
    def test_cell_styles_command_opens_gallery(self):
        from yc import ui

        self.assertTrue(ui.command("CellStyles"))
        self.keys("w:6")
        self.assertTrue(ui.state()["ribbon"].get("popup_open"), "the Cell Styles gallery opens")
        self.assertTrue(self.items("Merge Styles..."), "Merge Styles... ends the gallery")
        self.keys("k:Escape|w:3")
        self.assertIsNone(self.ws.Range("A1").Value)
        self.assertEqual(str(self.ws.Range("A1").Style), "Normal", "no style was applied by opening the gallery")

    # ---- Number tab > Custom: Delete a workbook's own code ----
    def test_custom_number_format_delete(self):
        ws = self.ws
        ws.Range("A1").Value = 3
        ws.Range("A1").NumberFormat = "0.000\" kg\""
        self.keys("g:A1|w:2|k:c-1|w:6")
        self.click("Number", window="Format Cells")
        self.click("Custom", window="Format Cells")
        self.click("Delete", window="Format Cells")
        self.keys("k:Escape|w:6")
        self.assertEqual(ws.Range("A1").NumberFormat, "General")

    # ---- Format Cells: a new font name fits the row (item 9) ----
    def test_font_name_fits_the_row(self):
        ws = self.ws
        ws.Range("A1").Value = "tall"
        before = ws.Range("A1").RowHeight
        self.keys("g:A1|w:2|k:c-1|w:6")
        self.click("Font", window="Format Cells")
        self.click(ident="##ff", window="Format Cells")
        self.keys("k:c-A|t:Gabriola|w:3")
        # a new font and a bigger size in one go: the row is measured in the new font
        from yc import ui

        size = [i for i in ui.items("Format Cells") if i["id"] == "##fsize"]
        self.assertTrue(size, "the Size box")
        if size:
            r = size[0]["rect"]
            self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
            self.keys("k:c-A|t:20|w:2")
        self.keys("k:Enter|w:8")
        self.assertEqual(ws.Range("A1").Font.Name, "Gabriola")
        if size:
            self.assertGreater(ws.Range("A1").RowHeight, before, "the row grows with the new font")

    # ---- New Formatting Rule > Format...: Format Cells' Number tab (item 16) ----
    def test_rule_format_button_number_tab(self):
        from yc import ui

        ws = self.ws
        ws.Range("A1").Value = 0.5
        self.keys("g:A1|w:2")
        self.assertTrue(ui.command("CfNewRule"))
        self.keys("w:6")
        self.click("Use a formula to determine which cells to format")
        self.keys("w:3")
        self.click(ident="##rb")
        self.keys("k:c-A|t:=TRUE|w:2")
        self.click("Format...")
        self.keys("w:6")
        self.click("Number", window="Format Cells")
        self.click("Percentage", window="Format Cells")
        self.keys("k:Enter|w:8")
        self.assertTrue(self.items("Use a formula to determine which cells to format"), "the rule editor comes back")
        self.keys("k:Enter|w:6")
        self.assertEqual(ws.Range("A1").FormatConditions.Count, 1)
        self.assertEqual(ws.Range("A1").FormatConditions(1).NumberFormat, "0.00%")
        self.assertEqual(ws.Range("A1").Text.strip(), "50.00%")


    # ---- Format as Table: hovering a style previews it on the table; the workbook doesn't change ----
    def test_table_style_live_preview(self):
        from yc import ui

        ws = self.ws
        ws.Range("A1:B4").Value = [["Name", "Qty"], ["a", 1], ["b", 2], ["c", 3]]
        ws.ListObjects.Add(1, ws.Range("A1:B4"), TableStyleName="TableStyleLight1")
        self.keys("g:A2|w:4")
        x0, y0, x1, y1 = self.cell("A1")
        self.tab("Home")
        r = self.ribbon("FormatAsTable")
        self.click_at((r[0] + r[2]) / 2, r[3] - 8)
        tile = [i for i in ui.ribbon_items() if i["where"] == "tile" and i["cmd"] == "TableStyleMedium3"]
        self.assertTrue(tile, "Medium 3 is in the gallery")
        t = tile[0]["rect"]
        self.keys(f"m:{int((t[0] + t[2]) / 2)},{int((t[1] + t[3]) / 2)}|w:8")
        _, _, px = self.shot("tablepreview")
        c = px(int(x0) + 4, int((y0 + y1) / 2))
        self.assertTrue(near(c, (0xED, 0x7D, 0x31), 70), f"the header shows Medium 3's orange while hovered: {c}")
        self.assertEqual(ws.ListObjects(1).TableStyle, "TableStyleLight1", "only a preview")
        self.keys("k:Escape|w:3|m:2,2|w:3")

    # ---- New Table Style: Set as default table style for this document (Ctrl+T uses it; saved) ----
    def test_default_table_style(self):
        from yc import excel, ui

        ws = self.ws
        wb = excel.ActiveWorkbook
        ws.Range("A1:B3").Value = [["h1", "h2"], [1, 2], [3, 4]]
        self.assertTrue(ui.command("NewTableStyle"))
        self.keys("w:6|k:c-A|t:Default3|w:2")
        self.click("Set as default table style for this document")
        self.keys("k:Enter|w:6")
        self.assertEqual(wb.DefaultTableStyle.Name, "Default3")
        self.keys("g:A1|w:2|k:c-T|w:6|k:Enter|w:6")
        self.assertEqual(ws.ListObjects(1).TableStyle, "Default3", "Ctrl+T starts with the default style")
        lo = ws.ListObjects.Add(1, ws.Range("D1:E3"))
        self.assertEqual(lo.TableStyle, "Default3", "ListObjects.Add too")
        wb.DefaultTableStyle = "TableStyleLight9"
        self.assertEqual(wb.TableStyles.DefaultTableStyle.Name, "TableStyleLight9")
        wb.DefaultTableStyle = "TableStyleMedium2"

    # ---- Merge Styles: asked of the other running YCGrid, its unsaved changes included ----
    def test_merge_styles_from_running_instance(self):
        from yc import excel

        other = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            path = os.path.join(tempfile.gettempdir(), f"yc_r3fmt_merge_{os.getpid()}.ycg")
            from yc import _conn

            c2 = yc.connect(target=other.pid)
            _conn.set_current(c2)
            excel.ActiveWorkbook.SaveAs(path)
            excel.ActiveWorkbook.Styles.Add("FromOtherUnsaved")  # after the save: only the running instance has it
            _conn.set_current(self.conn)
            excel.ActiveWorkbook.Styles.Merge(path)
            names = [s.Name for s in excel.ActiveWorkbook.Styles]
            self.assertIn("FromOtherUnsaved", names, "the unsaved style came over the other YCGrid's pipe")
        finally:
            from yc import _conn

            _conn.set_current(self.conn)
            other.terminate()
            other.wait(10)

    # ---- Item 9: a taller font re-fits the row by its line height, even at the same size ----
    def test_row_follows_font_line_height(self):
        ws = self.ws
        ws.Range("A1").Value = "tall"
        before = ws.Range("A1").RowHeight
        self.keys("g:A1|w:2|k:c-1|w:6")
        self.click("Font", window="Format Cells")
        self.click(ident="##ff", window="Format Cells")
        self.keys("k:c-A|t:Gabriola|w:3|k:Enter|w:8")
        self.assertEqual(ws.Range("A1").Font.Name, "Gabriola")
        self.assertGreater(ws.Range("A1").RowHeight, before, "Gabriola's taller lines grow the row at the same size")
        ws.Range("B1").Value = "x"
        ws.Range("B1").Font.Name = "Gabriola"
        ws.Rows(1).AutoFit()
        self.assertGreater(ws.Range("A1").RowHeight, before, "AutoFit Row Height measures the font's line height too")


if __name__ == "__main__":
    unittest.main()
