# SPDX-License-Identifier: Apache-2.0
"""Round 8: Increase / Decrease Decimal, the Format Cells dialog's category and combos, and the Python format
properties (IndentLevel, a border's Weight, Interior.Pattern), each measured against Excel 16 and failing without
its fix.

Run: python -m unittest tests.test_round8_formats (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"

xlGeneral, xlLeft, xlCenter, xlRight = 1, -4131, -4108, -4152
xlContinuous, xlDot, xlDouble, xlDash = 1, -4118, -4119, -4115
xlHairline, xlThin, xlMedium, xlThick = 1, 2, -4138, 4
xlEdgeBottom = 9
xlSolid, xlNone = 1, -4142


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class FormatsTest(unittest.TestCase):
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
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
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

    def find(self, text, window="", by="label", where=None):
        from yc import ui

        hits = [i for i in ui.items(window) if i[by] == text and (where is None or where in i.get("window", ""))]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        self.assertTrue(hits, f"{text!r} isn't on the screen")
        return hits[0]

    def has(self, text, window="", by="label"):
        from yc import ui

        return any(i[by] == text for i in ui.items(window))

    def click(self, text, window="", by="label", where=None):
        r = self.find(text, window, by, where)["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def pick(self, combo_id, text):
        """Open a combo of Format Cells (noted by its id) and click an entry of its list: the one right below it."""
        from yc import ui

        box = self.find(combo_id, "Format Cells", by="id")["rect"]
        self.click(combo_id, "Format Cells", by="id")
        hits = [i for i in ui.items("") if i["label"] == text and abs(i["rect"][0] - box[0]) < 20 and i["rect"][1] >= box[3] - 2]
        self.assertTrue(hits, f"{text!r} isn't in the list of {combo_id}")
        r = hits[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def decimals(self, fmt, value, *steps):
        """The code Increase / Decrease Decimal leave on a cell with fmt and value (steps: +1 / -1)."""
        from yc import ui

        r = self.ws.Range("B2")
        r.Clear()
        r.NumberFormat = fmt
        r.Value2 = value
        self.keys("g:B2|w:2")
        for st in steps:
            ui.command("DecimalIncrease" if st > 0 else "DecimalDecrease")
            self.keys("w:3")
        return r.NumberFormat

    # ---- Increase / Decrease Decimal ----
    def test_decimals_change_only_the_mantissa_of_scientific_codes(self):
        # the exponent's digits were taken for decimals: 0.00E+000, then 0.0E+ (an invalid code)
        self.assertEqual(self.decimals("0.00E+00", -1234.5678, 1), "0.000E+00")
        self.assertEqual(self.decimals("0.00E+00", -1234.5678, -1), "0.0E+00")
        self.assertEqual(self.decimals("0.00E+00", -1234.5678, -1, -1, -1), "0E+00")
        self.assertEqual(self.decimals("##0.0E+0", -1234.5678, 1), "##0.00E+0")
        self.assertEqual(self.decimals("##0.0E+0", -1234.5678, -1), "##0E+0")

    def test_decimals_leave_fractions_and_times_alone(self):
        # # ?/? became # ?/?.0, mm:ss.0 became mm:ss.00 / mm:ss
        self.assertEqual(self.decimals("# ?/?", -1234.5678, 1), "# ?/?")
        self.assertEqual(self.decimals("# ??/??", -1234.5678, 1), "# ??/??")
        self.assertEqual(self.decimals("mm:ss.0", 45300.5123, 1), "mm:ss.0")
        self.assertEqual(self.decimals("mm:ss.0", 45300.5123, -1), "mm:ss.0")
        self.assertEqual(self.decimals("h:mm:ss", 45300.5123, 1), "h:mm:ss")

    def test_decimals_repeat_the_last_decimal_placeholder(self):
        # 0.? became 0.?0 and #.## became #.##0
        self.assertEqual(self.decimals("0.?", -1234.5678, 1), "0.??")
        self.assertEqual(self.decimals("#.##", -1234.5678, 1), "#.###")
        self.assertEqual(self.decimals("0.0?", 1.5, 1), "0.0??")
        self.assertEqual(self.decimals("0.0?", 1.5, 1, -1, -1), "0.0")

    def test_decimals_keep_a_plain_dash_zero_section(self):
        # a "-" zero section was taken for accounting's and got a ?
        self.assertEqual(self.decimals('#,##0.00;-#,##0.00;"-"', -1234.5678, 1), '#,##0.000;-#,##0.000;"-"')
        self.assertEqual(self.decimals('0.00;-0.00;"zero"', 5, 1), '0.000;-0.000;"zero"')
        # accounting's own zero section still follows the decimals
        self.assertEqual(self.decimals('_($* #,##0.00_);_($* (#,##0.00);_($* "-"??_);_(@_)', 5, -1),
                         '_($* #,##0.0_);_($* (#,##0.0);_($* "-"?_);_(@_)')

    def test_decimals_on_general_follow_what_general_shows(self):
        # E notation stays E notation; no decimals to take away and text keep General
        self.assertEqual(self.decimals("General", 2.5e-10, 1), "0.00E+00")
        self.assertEqual(self.decimals("General", 2.5e-10, -1), "0E+00")
        self.assertEqual(self.decimals("General", 1e20, 1), "0.0E+00")
        self.assertEqual(self.decimals("General", 1e20, -1), "General")
        self.assertEqual(self.decimals("General", 123456789012, 1), "0.000000E+00")
        self.assertEqual(self.decimals("General", 123456789012, -1), "0.0000E+00")
        for v in (1, 100, 0):
            self.assertEqual(self.decimals("General", v, -1), "General", v)
        self.assertEqual(self.decimals("General", "abc", 1), "General")
        self.assertEqual(self.decimals("General", "abc", -1), "General")
        # what already matched Excel stays
        for v, inc, dec in ((1.5, "0.00", "0"), (0.333333333333333, "0.0000000000", "0.00000000"), (1234.5678, "0.00000", "0.000"),
                            (-2.25, "0.000", "0.0"), (0.1, "0.00", "0"), (1e-05, "0.000000", "0.0000"), (1, "0.0", None)):
            self.assertEqual(self.decimals("General", v, 1), inc, v)
            if dec:
                self.assertEqual(self.decimals("General", v, -1), dec, v)

    # ---- Format Cells ----
    def test_format_cells_opens_a_date_code_not_in_the_list_on_custom(self):
        # dd/mm/yyyy opened on Date with the first type shown; clicking that type and OK kept dd/mm/yyyy
        r = self.ws.Range("B14")
        r.Value2 = 45298
        r.NumberFormat = "dd/mm/yyyy"
        self.keys("g:B14|w:2|k:c-1|w:6")
        self.assertTrue(self.dialog())
        self.click("Number", "Format Cells")
        self.assertTrue(self.has("##custom", "Format Cells", by="id"), "the Number tab didn't open on Custom")
        self.click("Date", "Format Cells")
        self.click("2024/1/7##yyyy/m/d", "Format Cells", by="id")
        self.click("OK", "Format Cells")
        self.assertEqual(self.dialog(), 0)
        self.assertEqual(r.NumberFormat, "yyyy/m/d")

    def test_format_cells_opens_a_scaled_number_on_custom(self):
        # #,##0.0,, opened as Number, and OK after changing the decimals dropped the scaling
        r = self.ws.Range("C3")
        r.Value2 = 2500000
        r.NumberFormat = "#,##0.0,,"
        self.keys("g:C3|w:2|k:c-1|w:6")
        self.click("Number", "Format Cells")
        self.assertTrue(self.has("##custom", "Format Cells", by="id"), "the Number tab didn't open on Custom")
        self.click("OK", "Format Cells")
        self.assertEqual(r.NumberFormat, "#,##0.0,,")

    def test_format_cells_alignment_and_underline_combos_are_found_and_work(self):
        # the Horizontal / Vertical / Underline combos weren't noted for ui.items
        r = self.ws.Range("D4")
        r.Value2 = "x"
        self.keys("g:D4|w:2|k:c-1|w:6")
        self.click("Alignment", "Format Cells")
        self.assertTrue(self.has("##valign", "Format Cells", by="id"))
        self.pick("##halign", "Center")
        self.click("Font", "Format Cells")
        self.pick("##underline", "Double")
        self.click("OK", "Format Cells")
        self.assertEqual(self.dialog(), 0)
        self.assertEqual(r.HorizontalAlignment, xlCenter)
        self.assertEqual(r.Font.Underline, -4119)

    # ---- Python properties ----
    def test_indent_level_makes_general_and_centered_cells_left(self):
        r = self.ws.Range("C1")
        r.HorizontalAlignment = xlGeneral
        r.IndentLevel = 2
        self.assertEqual((r.HorizontalAlignment, r.IndentLevel), (xlLeft, 2))
        r.IndentLevel = 0
        r.HorizontalAlignment = xlCenter
        r.IndentLevel = 2
        self.assertEqual((r.HorizontalAlignment, r.IndentLevel), (xlLeft, 2))
        r.IndentLevel = 0
        r.HorizontalAlignment = xlRight
        r.IndentLevel = 3
        self.assertEqual((r.HorizontalAlignment, r.IndentLevel), (xlRight, 3))

    def test_border_weight_a_style_has_no_line_for_makes_it_continuous(self):
        b = self.ws.Range("C1").Borders(xlEdgeBottom)
        b.LineStyle = xlDot
        b.Weight = xlHairline
        self.assertEqual((b.LineStyle, b.Weight), (xlContinuous, xlHairline))
        b.LineStyle = xlDouble
        b.Weight = xlThin
        self.assertEqual((b.LineStyle, b.Weight), (xlContinuous, xlThin))
        # the weights a style comes in keep it
        b.LineStyle = xlDash
        b.Weight = xlMedium
        self.assertEqual((b.LineStyle, b.Weight), (xlDash, xlMedium))
        b.LineStyle = xlDot
        b.Weight = xlThin
        self.assertEqual((b.LineStyle, b.Weight), (xlDot, xlThin))
        b.LineStyle = xlDouble
        b.Weight = xlThick
        self.assertEqual((b.LineStyle, b.Weight), (xlDouble, xlThick))

    def test_interior_pattern_solid_on_a_cell_without_fill_is_white(self):
        c = self.ws.Range("E5")
        c.Interior.Pattern = xlSolid
        self.assertEqual((c.Interior.Pattern, c.Interior.Color), (xlSolid, 16777215))
        # a filled cell keeps its color
        d = self.ws.Range("E6")
        d.Interior.Color = 255
        d.Interior.Pattern = xlSolid
        self.assertEqual((d.Interior.Pattern, d.Interior.Color), (xlSolid, 255))
        c.Interior.Pattern = xlNone
        self.assertEqual(c.Interior.Pattern, xlNone)


if __name__ == "__main__":
    unittest.main()
