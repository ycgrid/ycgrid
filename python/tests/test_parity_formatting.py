# SPDX-License-Identifier: Apache-2.0
"""Excel parity: cell formatting, each checked the way a person does it.

Every test fails without its fix. Run: python -m unittest tests.test_parity_formatting (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")

xlEdgeLeft, xlEdgeTop, xlEdgeBottom, xlEdgeRight = 7, 8, 9, 10
xlThick, xlDouble, xlContinuous, xlLineStyleNone = 4, -4119, 1, -4142


class FormattingParityTest(unittest.TestCase):
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
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.25)

    def dialog(self):
        from yc import ui

        return ui.state().get("dialog", 0)

    def labels(self, window=""):
        from yc import ui

        return [i["label"] for i in ui.items(window)]

    def find(self, label, window="", nth=0, by="label"):
        from yc import ui

        hits = [i for i in ui.items(window) if i[by] == label]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        self.assertGreater(len(hits), nth, f"{label!r} isn't on the screen")
        return hits[nth]

    def find_part(self, text, window=""):
        from yc import ui

        hits = [i for i in ui.items(window) if text in i["label"]]
        self.assertTrue(hits, f"nothing with {text!r} on the screen")
        return hits[0]

    def click_rect(self, r):
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def click(self, label, window="", nth=0, by="label"):
        self.click_rect(self.find(label, window, nth, by)["rect"])

    def ribbon(self, cmd, where=None):
        from yc import ui

        hits = [i for i in ui.ribbon_items() if i["cmd"] == cmd and (where is None or i["where"] == where)]
        self.assertTrue(hits, f"{cmd} isn't on the ribbon")
        return hits[0]["rect"]

    def ribbon_arrow(self, cmd):
        r = self.ribbon(cmd)
        x, y = int(r[2] - 5), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def cell_center(self, addr):
        from yc import ui

        r = ui.cell_rect(addr)
        return int((r[0] + r[2]) // 2), int((r[1] + r[3]) // 2)

    def run_cmd(self, cid):
        from yc import ui

        self.assertTrue(ui.command(cid), cid)
        self.keys("w:5")

    # ---- 1. Column Width ----
    def test_column_width_ok_keeps_the_width(self):
        self.keys("g:C1|w:2")
        self.run_cmd("ColumnWidth")
        self.assertTrue(self.dialog())
        self.keys("k:Enter|w:5")
        self.assertAlmostEqual(self.ws.Range("C1").ColumnWidth, 8.43, places=2)
        # columns of different widths: the box starts empty and OK leaves both alone
        self.ws.Range("B1").ColumnWidth = 20
        self.keys("g:B1:C1|w:2")
        self.run_cmd("ColumnWidth")
        self.keys("k:Enter|w:5")
        self.assertAlmostEqual(self.ws.Range("B1").ColumnWidth, 20, delta=0.1)
        self.assertAlmostEqual(self.ws.Range("C1").ColumnWidth, 8.43, places=2)

    # ---- 2. Border tab ----
    def test_border_tab_each_edge_keeps_its_own_line(self):
        self.keys("g:B2:C3|w:2|k:c-1|w:6")
        self.click("Border", "Format Cells")
        self.click("Line style: Thick", "Format Cells")
        self.click("Top", "Format Cells")
        self.click("Line style: Double", "Format Cells")
        self.click("Bottom", "Format Cells")
        self.click("OK", "Format Cells")
        self.assertEqual(self.ws.Range("B2").Borders(xlEdgeTop).Weight, xlThick)
        self.assertEqual(self.ws.Range("B3").Borders(xlEdgeBottom).LineStyle, xlDouble)
        # a thin bottom border clicked with Double becomes double (it isn't taken off)
        self.ws.Range("E5").Borders(xlEdgeBottom).LineStyle = xlContinuous
        self.keys("g:E5|w:2|k:c-1|w:6")
        self.click("Border", "Format Cells")
        self.click("Line style: Double", "Format Cells")
        self.click("Bottom", "Format Cells")
        self.click("OK", "Format Cells")
        self.assertEqual(self.ws.Range("E5").Borders(xlEdgeBottom).LineStyle, xlDouble)

    # ---- 3. a style's borders ----
    def test_style_format_keeps_its_borders(self):
        self.keys("g:D4|w:2")
        self.run_cmd("NewCellStyle")
        name_box = self.find("##name", "Style", by="id")
        self.click_rect(name_box["rect"])
        self.keys("k:c-A|t:BoxedParity|w:2")
        self.click("Format...", "Style")
        self.click("Border", "Format Cells")
        self.click("Outline", "Format Cells")
        self.click("OK", "Format Cells")
        self.click("OK", "Style")
        self.ws.Range("F6").Style = "BoxedParity"
        self.keys("w:3")
        self.assertNotEqual(self.ws.Range("F6").Borders(xlEdgeTop).LineStyle, xlLineStyleNone)

    # ---- 4, 20. the ribbon palettes ----
    def test_fill_palette_has_no_fill_and_font_palette_automatic(self):
        r = self.ws.Range("B2")
        r.Interior.Color = 0x00FFFF
        self.keys("g:B2|w:2")
        self.ribbon_arrow("FillColor")
        self.click("No Fill")
        self.assertEqual(r.Interior.ColorIndex, -4142)  # xlColorIndexNone
        c = self.ws.Range("C2")
        c.Value = "x"
        c.Font.Color = 0x0000FF
        self.keys("g:C2|w:2")
        self.ribbon_arrow("FontColor")
        self.click("Automatic")
        self.assertEqual(c.Font.Color, 0)  # Automatic (black), no longer red

    def test_palette_shades_follow_each_column(self):
        self.keys("g:A1|w:2")
        self.ribbon_arrow("FillColor")
        self.click("Background 1, Darker 5%")
        self.assertEqual(self.ws.Range("A1").Interior.Color, 0xF2F2F2)

    # ---- 5. Format Painter ----
    def test_format_painter_double_click_locks_it_on(self):
        self.ws.Range("A1").Value = "a"
        self.ws.Range("A1").Font.Bold = True
        self.keys("g:A1|w:3")
        r = self.ribbon("FormatPainter")
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:1|c:{x},{y}|w:6")
        for addr in ("B5", "C7"):
            cx, cy = self.cell_center(addr)
            self.keys(f"m:{cx},{cy}|w:3|c:{cx},{cy}|w:6")
        self.assertTrue(self.ws.Range("B5").Font.Bold)
        self.assertTrue(self.ws.Range("C7").Font.Bold)
        from yc import ui

        pressed = [c for c in ui.commands() if c["id"] == "FormatPainter"][0]["pressed"]
        self.assertTrue(pressed, "a double-click keeps the painter on")
        self.keys("k:Escape|w:3")

    # ---- 6. Merge & Center ----
    def test_merge_asks_before_data_is_lost_and_keeps_a_lone_value(self):
        self.ws.Range("A1:B1").Value = [[1, 2]]
        self.keys("g:A1:B1|w:2")
        self.run_cmd("MergeCenter")
        self.assertTrue(self.dialog(), "a question first")
        self.keys("k:Escape|w:5")
        self.assertFalse(self.ws.Range("A1").MergeCells)
        self.assertEqual(self.ws.Range("B1").Value, 2)
        self.ws.Range("B2").Value = "Total"
        self.keys("g:A2:C2|w:2")
        self.run_cmd("MergeCenter")
        self.assertFalse(self.dialog())
        self.assertEqual(self.ws.Range("A2").Value, "Total")

    # ---- 7, 8. Number group ----
    def test_comma_style_is_excels_comma_style(self):
        self.ws.Range("A1:A2").Value = [[0], [-5]]
        self.keys("g:A1:A2|w:2")
        self.run_cmd("CommaStyle")
        self.assertEqual(self.ws.Range("A1").Text.strip(), "-")
        self.assertIn("(", self.ws.Range("A2").Text)
        self.assertEqual(str(self.ws.Range("A1").Style), "Comma")

    def test_increase_decimal_keeps_the_accounting_dash(self):
        r = self.ws.Range("A1")
        r.Value = 0
        r.NumberFormat = '_($* #,##0.00_);_($* (#,##0.00);_($* "-"??_);_(@_)'
        self.keys("g:A1|w:2")
        self.run_cmd("DecimalIncrease")
        self.assertEqual(r.NumberFormat, '_($* #,##0.000_);_($* (#,##0.000);_($* "-"???_);_(@_)')
        self.assertNotIn(".0", r.Text)

    # ---- 9. rows fit the new look ----
    def test_wrap_text_and_title_style_fit_the_row(self):
        self.ws.Range("A1").Value = "a long sentence that needs more than one line in a narrow column"
        self.keys("g:A1|w:2|k:c-1|w:6")
        self.click("Alignment", "Format Cells")
        self.click("Wrap text", "Format Cells")
        self.click("OK", "Format Cells")
        self.assertGreater(self.ws.Range("A1").RowHeight, 16)
        self.ws.Range("B3").Value = "Title"
        self.keys("g:B3|w:2")
        self.run_cmd("CellStyleTitle")
        self.assertGreater(self.ws.Range("B3").RowHeight, 16)  # Title is 18 pt: the 15 pt row grows

    # ---- 10. Clear ----
    def test_clear_formats_and_clear_all_take_rules_notes_links(self):
        from yc import excel

        self.ws.Range("B2:B5").FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "=5")
        self.keys("g:B2:B5|w:2")
        self.run_cmd("ClearFormats")
        self.assertEqual(self.ws.Range("B2:B5").FormatConditions.Count, 0)
        self.ws.Range("B2:B5").FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "=5")
        self.ws.Range("B2").AddComment("note")
        self.ws.Hyperlinks.Add(self.ws.Range("B3"), "https://example.com")
        self.keys("g:B2:B5|w:2")
        self.run_cmd("ClearAll")
        self.assertEqual(self.ws.Comments.Count, 0)
        self.assertEqual(self.ws.Hyperlinks.Count, 0)
        self.assertEqual(self.ws.Range("B2:B5").FormatConditions.Count, 0)

    # ---- 11. Format Painter brings the rules ----
    def test_format_painter_copies_conditional_formatting(self):
        from yc import excel

        self.ws.Range("A1:A3").FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "=5")
        self.keys("g:A1|w:2")
        self.run_cmd("FormatPainter")
        x0, y0 = self.cell_center("C1")
        x1, y1 = self.cell_center("C3")
        self.keys(f"m:{x0},{y0}|w:3|d:{x0},{y0},{x1},{y1}|w:6")
        self.assertEqual(self.ws.Range("C1:C3").FormatConditions.Count, 1)
        self.assertEqual(self.ws.Range("C2").FormatConditions(1).Formula1, "=5")

    # ---- 12. Home > Format ----
    def test_format_menu_default_width_lock_cell_tab_color(self):
        self.run_cmd("DefaultWidth")
        self.keys("k:c-A|t:12|k:Enter|w:5")
        self.assertAlmostEqual(self.ws.StandardWidth, 12, delta=0.05)
        self.assertAlmostEqual(self.ws.Range("H1").ColumnWidth, 12, delta=0.1)
        self.keys("g:B2|w:2")
        self.run_cmd("LockCell")
        self.assertFalse(self.ws.Range("B2").Locked)
        self.run_cmd("TabColor")
        self.click("Accent 1", "Tab Color")
        self.assertNotIn(self.ws.Tab.Color, (None, -4142, 0))

    # ---- 13, 14. Conditional Formatting menu and quick dialogs ----
    def test_color_scale_gallery_top_percent_and_dates(self):
        self.ws.Range("A1:A6").Value = [[v] for v in (1, 5, 9, 3, 7, 2)]
        self.keys("g:A1:A6|w:2")
        self.run_cmd("CfScaleRYG")
        fc = self.ws.Range("A1").FormatConditions(1)
        self.assertEqual(fc.ColorScaleCriteria(1).FormatColor.Color, 0x7BBE63)  # the lowest value is green
        self.ws.Range("A1:A6").FormatConditions.Delete()
        self.run_cmd("CfTop10Percent")
        self.keys("k:Enter|w:5")
        self.assertTrue(self.ws.Range("A1").FormatConditions(1).Percent)
        self.ws.Range("A1:A6").FormatConditions.Delete()
        self.run_cmd("CfDateOccurring")
        self.assertTrue(self.dialog())
        self.keys("k:Enter|w:5")
        fc = self.ws.Range("A1").FormatConditions(1)
        self.assertEqual(fc.Type, 11)  # xlTimePeriod
        self.assertEqual(fc.DateOperator, 1)  # xlYesterday, Excel's first choice

    def test_duplicate_values_can_highlight_unique_values(self):
        self.ws.Range("A1:A4").Value = [[1], [1], [2], [3]]
        self.keys("g:A1:A4|w:2")
        self.run_cmd("CfDuplicate")
        self.click_rect(self.find("##dupe", by="id")["rect"])
        self.click("Unique")
        self.click("OK")
        self.assertEqual(self.ws.Range("A1").FormatConditions(1).DupeUnique, 0)  # xlUnique

    def test_quick_rule_dialog_is_titled_and_prefilled(self):
        # Excel 16 proposes halfway between the lowest and highest number (45.5 here, where the average is 33.75),
        # and for Between a quarter and three quarters of the way, rounded (measured)
        self.ws.Range("A1:A8").Value = [[12], [45], [7], [30], [88], [21], [64], [3]]
        self.keys("g:A1:A8|w:2")
        self.run_cmd("CfGreater")
        from yc import ui

        self.assertTrue(any(w for w in [i["window"] for i in ui.items()] if w == "Greater Than"))
        self.keys("k:Enter|w:5")
        self.assertEqual(self.ws.Range("A1").FormatConditions(1).Formula1, "=45.5")
        self.ws.Range("A1:A8").FormatConditions.Delete()
        self.run_cmd("CfBetween")
        self.keys("w:4|k:Enter|w:5")
        fc = self.ws.Range("A1").FormatConditions(1)
        self.assertEqual((fc.Formula1, fc.Formula2), ("=24", "=67"))

    def test_text_that_contains_proposes_the_active_cells_text(self):
        # Excel 16 starts the box with the active cell's text (measured: MSFT with B3 active in B2:B4)
        self.ws.Range("B2:B4").Value = [["AAPL"], ["MSFT"], ["TSLA"]]
        self.keys("g:B2:B4|w:2|k:Enter|w:3")  # B3 active
        self.run_cmd("CfTextContains")
        self.keys("w:4|k:Enter|w:5")
        fc = self.ws.Range("B2").FormatConditions(1)
        self.assertEqual(fc.Text, "MSFT")

    # ---- 15, 16. rule details from Python ----
    def test_text_operators_and_scale_points(self):
        from yc import excel

        fc = self.ws.Range("A1:A3").FormatConditions.Add(excel.xlTextString, String="AB", TextOperator=excel.xlBeginsWith)
        self.assertEqual(fc.TextOperator, excel.xlBeginsWith)
        cs = self.ws.Range("B1:B3").FormatConditions.AddColorScale(3)
        cs.ColorScaleCriteria(2).Type = excel.xlConditionValueNumber
        cs.ColorScaleCriteria(2).Value = 0
        self.assertEqual(cs.ColorScaleCriteria(2).Type, excel.xlConditionValueNumber)
        self.assertEqual(cs.ColorScaleCriteria(2).Value, 0)
        self.assertEqual(cs.ColorScaleCriteria(1).Type, excel.xlConditionValueLowestValue)

    # ---- 17. the Rules Manager ----
    def test_rules_manager_cancel_drops_the_changes(self):
        from yc import excel

        self.ws.Range("A1:A3").FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "=5")
        self.keys("g:A1:A3|w:2")
        self.run_cmd("CfManageRules")
        self.click_rect(self.find_part("greater than")["rect"])
        self.click("Delete Rule")
        self.click("Cancel")
        self.assertEqual(self.ws.Range("A1:A3").FormatConditions.Count, 1)
        self.run_cmd("CfManageRules")
        self.click_rect(self.find_part("greater than")["rect"])
        self.click("Delete Rule")
        self.click("OK")
        self.assertEqual(self.ws.Range("A1:A3").FormatConditions.Count, 0)

    # ---- 18. Fill tab ----
    def test_fill_tab_pattern_is_picked_from_pictures(self):
        self.keys("g:B2|w:2|k:c-1|w:6")
        self.click("Fill", "Format Cells")
        self.click("Pattern: Gray 25%", "Format Cells")
        self.click("OK", "Format Cells")
        self.assertEqual(self.ws.Range("B2").Interior.Pattern, -4124)  # xlPatternGray25

    # ---- 19. the Font box ----
    def test_font_box_has_theme_and_recent_fonts(self):
        self.keys("g:A1|w:2")
        self.click_rect(self.ribbon("FontName"))
        self.keys("t:Arial|w:2|k:Enter|w:5")
        self.assertEqual(self.ws.Range("A1").Font.Name, "Arial")
        self.click_rect(self.ribbon("FontName"))
        got = self.labels()
        self.keys("k:Escape|w:3")
        self.assertTrue(any(x.endswith("(Headings)") for x in got), "Theme Fonts")
        self.assertIn("Arial", got)  # Recently Used Fonts

    # ---- 21. Format Cells > Font: a typed name ----
    def test_font_tab_takes_a_typed_name(self):
        self.ws.Range("A1").Value = "x"
        self.keys("g:A1|w:2|k:c-1|w:6")
        self.click("Font", "Format Cells")
        self.click_rect(self.find("##ff", "Format Cells", by="id")["rect"])
        self.keys("t:Arial|w:2|k:Enter|w:5")
        self.assertEqual(self.ws.Range("A1").Font.Name, "Arial")

    # ---- 22. split buttons ----
    def test_borders_button_repeats_the_last_choice_and_double_underline_toggles(self):
        self.keys("g:B2:C3|w:2")
        self.ribbon_arrow("Borders")
        self.click_rect(self.ribbon("BorderAll", "menu"))
        self.keys("g:E2:F3|w:2|m:2,2|w:2")
        r = self.ribbon("Borders")
        x, y = int(r[0] + 10), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")
        for e in (xlEdgeLeft, xlEdgeTop, xlEdgeRight, xlEdgeBottom):
            self.assertNotEqual(self.ws.Range("E2").Borders(e).LineStyle, xlLineStyleNone)
        self.ws.Range("A1").Value = "u"
        self.keys("g:A1|w:2")
        self.run_cmd("UnderlineDouble")
        self.run_cmd("UnderlineDouble")
        self.assertIn(self.ws.Range("A1").Font.Underline, (-4142, None, False, 0))

    # ---- 23-25, 30. Format Cells details ----
    def test_alignment_tab_and_increase_indent(self):
        r = self.ws.Range("A1")
        r.Value = "x"
        r.HorizontalAlignment = -4108  # xlCenter
        self.keys("g:A1|w:2")
        self.run_cmd("IndentIncrease")
        self.assertEqual(r.HorizontalAlignment, -4131)  # xlLeft
        self.assertEqual(r.IndentLevel, 1)
        self.keys("k:c-1|w:6")
        self.click("Alignment", "Format Cells")
        self.click("Wrap text", "Format Cells")
        shrink = self.find("Shrink to fit", "Format Cells")
        self.assertTrue(shrink["disabled"])

    def test_protection_tab_names_review_protect_sheet(self):
        self.keys("g:A1|w:2|k:c-1|w:6")
        self.click("Protection", "Format Cells")
        from yc import ui

        self.assertTrue(any("Review tab" in i["label"] for i in ui.items("Format Cells")))

    def test_format_cells_reopens_on_the_last_tab(self):
        self.keys("g:A1|w:2|k:c-1|w:6")
        self.click("Fill", "Format Cells")
        self.keys("k:Escape|w:4|k:c-1|w:6")
        self.assertTrue(any(x.startswith("Pattern: ") for x in self.labels("Format Cells")))

    def test_ctrl_tab_goes_through_a_dialogs_tabs(self):
        # Excel's dialogs: Ctrl+Tab / Ctrl+PgDn the next tab, Ctrl+Shift+Tab / Ctrl+PgUp the one before; they did nothing
        self.keys("g:A1|w:2|k:c-1|w:6")
        self.click("Number", "Format Cells")
        try:
            self.keys("k:c-Tab|w:4")
            self.assertIn("Wrap text", self.labels("Format Cells"), "Alignment after Number")
            self.keys("k:c-s-Tab|w:4")
            self.assertIn("Scientific", self.labels("Format Cells"), "back to Number")
            self.keys("k:c-PageUp|w:4")
            self.assertIn("Locked", self.labels("Format Cells"), "from the first tab round to the last, Protection")
            self.keys("k:c-PageDown|w:4")
            self.assertIn("Scientific", self.labels("Format Cells"), "and on to the first")
        finally:
            self.keys("k:Escape|w:4")

    def test_currency_symbols_and_shortcut(self):
        r = self.ws.Range("A1")
        r.Value = -5
        self.keys("g:A1|w:2|k:c-s-4|w:4")
        self.assertEqual(r.Text, "($5.00)")
        self.run_cmd("AccountingEuro")
        self.assertIn("\u20ac", r.NumberFormat)
        self.keys("k:c-1|w:6")
        self.click("Number", "Format Cells")
        self.click("Currency", "Format Cells")
        self.click_rect(self.find("##symbol", "Format Cells", by="id")["rect"])
        self.assertIn("\u20a9", self.labels())  # won: one of the added symbols (the list scrolls on to CHF and kr)

    # ---- 27. AutoFit Column Width ----
    def test_autofit_column_width_fits_the_selected_cells(self):
        self.ws.Range("A1").Value = "a long title that is wider than the data"
        self.ws.Range("A2:A3").Value = [["x"], ["x"]]
        self.keys("g:A2:A3|w:2")
        self.run_cmd("AutoFitColumns")
        self.assertLess(self.ws.Range("A1").ColumnWidth, 5)

    # ---- 29. Convert to Range ----
    def test_convert_to_range_asks_first(self):
        self.ws.Range("A1:B3").Value = [["a", "b"], [1, 2], [3, 4]]
        self.ws.ListObjects.Add(1, self.ws.Range("A1:B3"))
        self.keys("g:A2|w:3")
        self.run_cmd("ConvertToRange")
        self.assertTrue(self.dialog())
        self.keys("k:Escape|w:5")
        self.assertEqual(self.ws.ListObjects.Count, 1)


if __name__ == "__main__":
    unittest.main()
