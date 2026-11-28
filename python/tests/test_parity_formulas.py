# SPDX-License-Identifier: Apache-2.0
"""Formulas working as in Excel: typing formulas, pointing, reference boxes, AutoSum,
circular references, auditing, spills, the Name Box. Each test is done the way a person does it and fails without
its fix.

Run: python -m unittest tests.test_parity_formulas (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402


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


EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


class FormulaParityTest(unittest.TestCase):
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

    def state(self):
        from yc import ui

        return ui.state()

    def edit_text(self):
        return (self.state().get("edit") or {}).get("text")

    def rect(self, addr):
        from yc import ui

        return ui.cell_rect(addr)

    def center(self, addr):
        r = self.rect(addr)
        return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)

    def labels(self, window=""):
        from yc import ui

        return [i["label"] for i in ui.items(window)]

    def click_label(self, label):
        from yc import ui

        hits = [i for i in ui.items() if i["label"] == label]
        self.assertTrue(hits, f"{label!r} isn't on the screen")
        r = hits[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def shot(self):
        from yc import ui

        path = os.path.join(tempfile.mkdtemp(prefix="yc_parity_f_"), "shot.png")
        ui.screenshot(path, max_width=8000)   # one pixel per window pixel, as cell_rect gives them
        return read_png(path)

    # ---- High ----
    def test_equals_shows_no_list_so_a_click_points_at_the_cell_below(self):
        # H1: the whole function list opened under the cell after "=", and the click on A2 landed on it
        self.ws.Range("A2").Value = 5
        self.keys("g:A1|w:2|t:=|w:3")
        self.assertNotIn("ABS", self.labels())
        self.keys("A:A2|w:3|k:Enter|w:4")
        self.assertEqual(self.ws.Range("A1").Formula, "=A2")
        self.assertEqual(self.ws.Range("A1").Value, 5)
        self.keys("g:B1|w:2|t:=SU|w:3")   # a letter typed: the list shows, as in Excel
        self.assertIn("SUM", self.labels())
        self.keys("k:Escape|w:2|k:Escape|w:2")

    def test_autosum_on_an_empty_total_row_and_column(self):
        # H2: an empty row under a block wrote one total at its right end; an empty column one at its bottom
        ws = self.ws
        ws.Range("A1:C3").Value = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
        self.keys("g:A4:C4|w:2|k:a-Equal|w:6")
        self.assertEqual([ws.Range(a).Formula for a in ("A4", "B4", "C4")], ["=SUM(A1:A3)", "=SUM(B1:B3)", "=SUM(C1:C3)"])
        self.assertEqual([ws.Range(a).Value for a in ("A4", "B4", "C4")], [12, 15, 18])
        self.keys("g:D1:D3|w:2|k:a-Equal|w:6")
        self.assertEqual(ws.Range("D1").Formula, "=SUM(A1:C1)")
        self.assertEqual(ws.Range("D3").Value, 24)

    def test_a_circular_reference_brings_the_warning_once(self):
        # H3: typing =A1+1 into A1 showed only a 0
        from yc import ui

        def warnings():  # a hidden instance answers message boxes itself and lists them in ui.errors()
            return sum("circular references" in str(e) for e in ui.errors())

        n0 = warnings()
        self.keys("g:A1|w:2|t:=A1+1|k:Enter|w:10")
        self.assertEqual(warnings(), n0 + 1, "no circular reference warning")
        self.assertEqual(self.ws.CircularReference.Address(), "$A$1")
        # L13: the status bar names the cell on its own sheet, and only says "Circular References" elsewhere
        self.assertEqual(self.state().get("circular_status"), "Circular References: A1")
        self.keys("g:B1|w:2|t:=B1*2|k:Enter|w:10")   # a second one: no second message
        self.assertEqual(warnings(), n0 + 1)
        from yc import excel

        other = excel.Worksheets.Add()
        other.Activate()
        self.keys("w:4")
        self.assertEqual(self.state().get("circular_status"), "Circular References")
        self.ws.Activate()
        self.ws.Range("A1:B1").ClearContents()
        self.keys("w:4")
        self.assertIsNone(self.ws.CircularReference)

    def test_ctrl_arrows_and_ctrl_space_point_like_the_active_cell_moves(self):
        # H6: Ctrl was ignored while pointing; Ctrl+Space typed nothing useful
        ws = self.ws
        ws.Range("A1:A50").Value = [[i] for i in range(1, 51)]
        self.keys("g:C1|w:2|t:=SUM(|k:LeftArrow|k:LeftArrow|k:c-s-DownArrow|w:2|t:)|k:Enter|w:4")
        self.assertEqual(ws.Range("C1").Formula, "=SUM(A1:A50)")
        self.keys("g:C2|w:2|t:=SUM(|k:LeftArrow|k:LeftArrow|k:c-Space|w:2|t:)|k:Enter|w:4")
        self.assertEqual(ws.Range("C2").Formula, "=SUM(A:A)")
        self.keys("g:C3|w:2|t:=SUM(|k:LeftArrow|k:LeftArrow|k:s-Space|w:2|t:)|k:Enter|w:4")
        self.assertEqual(ws.Range("C3").Formula, "=SUM(3:3)")
        self.keys("g:E5|w:2|t:=|k:LeftArrow|k:c-Home|w:2|k:Enter|w:4")
        self.assertEqual(ws.Range("E5").Formula, "=A1")

    def test_the_formula_bar_expands_to_show_the_lines_of_a_formula(self):
        # H4: the bar was one line high, line breaks were flattened, no Ctrl+Shift+U
        from yc import excel

        self.ws.Range("A1").Formula = "=LET(x,1,\ny,2,\nx+y)"
        top = self.rect("A2")[1]
        self.keys("g:A1|w:2|k:c-s-U|w:4")
        try:
            self.assertEqual(excel.Application.FormulaBarHeight, 3)
            self.assertGreater(self.rect("A2")[1], top + 20)   # the cells moved down under the taller bar
        finally:
            self.keys("k:c-s-U|w:4")
        self.assertEqual(excel.Application.FormulaBarHeight, 1)
        self.assertLess(abs(self.rect("A2")[1] - top), 2)

    def test_background_error_checking_marks_numbers_stored_as_text(self):
        # H5: no green triangles, no error options button
        from yc import constants as k

        ws = self.ws
        ws.Range("A1").Value = "'123"
        ws.Range("B1").Formula = "=1/0"
        self.keys("w:4")
        self.assertTrue(ws.Range("A1").Errors(k.xlNumberAsText).Value)
        self.assertTrue(ws.Range("B1").Errors(k.xlEvaluateToError).Value)
        self.assertFalse(ws.Range("A1").Errors(k.xlEvaluateToError).Value)
        w, h, px = self.shot()
        r = self.rect("A1")
        greens = sum(1 for dy in range(1, 5) for dx in range(1, 5)
                     if (lambda c: c[1] > 100 and c[1] - c[0] > 50 and c[1] - c[2] > 30)(px(int(r[0]) + dx, int(r[1]) + dy)))
        self.assertGreater(greens, 2, "no green triangle at A1's corner")
        self.keys("g:A1|w:4")
        self.click_label("Error Options")
        self.click_label("Convert to Number")
        self.keys("w:4")
        self.assertEqual(ws.Range("A1").Value, 123)
        self.assertNotIsInstance(ws.Range("A1").Value, str)
        ws.Range("B1").Errors(k.xlEvaluateToError).Ignore = True
        self.assertTrue(ws.Range("B1").Errors(k.xlEvaluateToError).Ignore)
        self.keys("g:B1|w:4")
        self.assertNotIn("Error Options", self.labels())

    # ---- Medium ----
    def test_references_are_colored_in_the_edit_box(self):
        # M1: the text of each reference is drawn in its box's color (A1 blue, B2 red)
        self.keys("g:E3|w:2|t:=A1+B2|w:6")
        w, h, px = self.shot()
        r = self.rect("E3")
        blue = red = 0
        for y in range(int(r[1]) + 3, int(r[3]) - 2):
            for x in range(int(r[0]) + 3, min(w, int(r[2]) + 60)):
                cr, cg, cb = px(x, y)
                blue += cb > 150 and cb - cr > 60 and cb - cg > 30
                red += cr > 150 and cr - cb > 60 and cr - cg > 60
        self.keys("k:Escape|w:2")
        self.assertGreater(blue, 3, "no blue text for A1")
        self.assertGreater(red, 3, "no red text for B2")

    def test_a_reference_box_drags_to_resize_and_move_the_reference(self):
        # M2: the boxes had no handles; dragging a corner or the border did nothing
        self.keys("g:C1|w:2|t:=SUM(A1:A3)|w:4")
        r3 = self.rect("A3")
        x5, y5 = self.center("A5")
        self.keys(f"p:{int(r3[2])},{int(r3[3])}|w:2|m:{x5},{y5 - 3}|w:2|m:{x5},{y5}|w:3|u:|w:3")
        self.assertEqual(self.edit_text(), "=SUM(A1:A5)")
        r2, rb = self.rect("A2"), self.rect("B2")
        y2 = int((r2[1] + r2[3]) / 2)
        self.keys(f"p:{int(r2[0]) + 1},{y2}|w:2|m:{int(rb[0]) + 6},{y2}|w:2|m:{int(rb[0]) + 10},{y2}|w:3|u:|w:3")
        self.assertEqual(self.edit_text(), "=SUM(B1:B5)")
        self.keys("k:Escape|w:2|m:2,2|w:2")

    def test_shift_click_while_pointing(self):
        # M3: Shift+click replaced the pointed reference
        ws = self.ws
        self.keys("g:C1|w:2|t:=SUM(|A:A1|w:2|H:shift|A:A5|w:2|h:|t:)|k:Enter|w:4")
        self.assertEqual(ws.Range("C1").Formula, "=SUM(A1:A5)")

    def test_a_header_click_while_pointing_inserts_the_whole_column(self):
        # M4: a header click changed the selection instead of inserting B:B
        ws = self.ws
        ws.Range("B1:B3").Value = [[1], [2], [3]]
        self.keys("g:D1|w:2|t:=SUM(|w:2")
        r = self.rect("B1")
        x, y = int((r[0] + r[2]) / 2), int(r[1]) - 8
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4|t:)|k:Enter|w:4")
        self.assertEqual(ws.Range("D1").Formula, "=SUM(B:B)")
        self.assertEqual(ws.Range("D1").Value, 6)
        r = self.rect("A2")
        x, y = int(r[0]) - 12, int((r[1] + r[3]) / 2)
        self.keys(f"g:D2|w:2|t:=SUM(|w:2|m:{x},{y}|w:2|c:{x},{y}|w:4|t:)|k:Enter|w:4")
        self.assertEqual(ws.Range("D2").Formula, "=SUM(2:2)")

    def test_f9_replaces_the_selected_part_with_its_value(self):
        # M5: F9 did nothing while editing
        ws = self.ws
        ws.Range("B2:C2").Value = [[6, 7]]
        self.keys("g:D2|w:2|t:=1+B2*C2|w:2|k:F2|w:2" + "|k:s-LeftArrow" * 5 + "|w:2|k:F9|w:4")
        self.assertEqual(self.edit_text(), "=1+42")
        self.keys("k:Escape|w:3")
        self.assertIsNone(ws.Range("D2").Formula or None)
        self.keys("g:D3|w:2|t:=B2&\"x\"|w:2|k:F9|w:4")   # nothing selected: the whole formula
        self.assertEqual(self.edit_text(), "=\"6x\"")
        self.keys("k:Escape|w:3")

    def test_tracer_arrows_go_when_rows_are_inserted_or_the_formula_changes(self):
        # M8: the arrows stayed and pointed at the wrong cells
        ws = self.ws
        ws.Range("A1").Value = 1
        ws.Range("B1").Formula = "=A1*2"
        self.keys("g:B1|w:2|x:TracePrecedents|w:4")
        self.assertEqual(len(self.conn.request("audit.arrows", {}, timeout=10)), 1)
        ws.Rows(1).Insert()
        self.keys("w:6")
        self.assertEqual(len(self.conn.request("audit.arrows", {}, timeout=10)), 0)
        self.keys("g:B2|w:2|x:TracePrecedents|w:4")
        self.assertEqual(len(self.conn.request("audit.arrows", {}, timeout=10)), 1)
        self.keys("g:B2|w:2|t:=A2*3|k:Enter|w:6")
        self.assertEqual(ws.Range("B2").Formula, "=A2*3")
        self.assertEqual(len(self.conn.request("audit.arrows", {}, timeout=10)), 0)

    def test_select_obstructing_cells_of_a_spill(self):
        # M9: nothing named or selected what blocked a #SPILL! formula
        ws = self.ws
        ws.Range("A2").Value = "x"
        ws.Range("A1").Formula = "=SEQUENCE(3)"
        self.keys("w:4")
        self.assertEqual(ws.Range("A1").Text, "#SPILL!")
        self.keys("R:A1|w:6")
        self.click_label("Select Obstructing Cells")
        self.assertEqual(self.state()["selection"], "A2")

    def test_fx_opens_the_arguments_of_the_call_at_the_caret(self):
        # M6: editing, fx opened Insert Function and OK added a second call
        ws = self.ws
        ws.Range("A1:A3").Value = [[1], [2], [3]]
        self.keys("g:C1|w:2|t:=ROUND(SUM(A1:A2|w:2|k:s-F3|w:8")
        self.assertTrue(self.state().get("dialog"))
        self.assertTrue(any(t.startswith("Number1:") for t in self.labels()))   # SUM's first argument
        # OK enters the formula as Excel's does; ROUND still short of an argument gets Enter's message and the entry
        # stays, its caret at the end, so the typing goes on
        self.keys("k:c-A|t:A1:A3|w:2|k:Enter|w:4|t:,0)|k:Enter|w:4")
        self.assertEqual(ws.Range("C1").Formula, "=ROUND(SUM(A1:A3),0)")
        ws.Range("C2").Formula = "=SUM(A1:A2)*2"
        self.keys("g:C2|w:2|k:s-F3|w:8")   # not editing: the formula's first call
        self.assertTrue(self.state().get("dialog"))
        self.keys("k:c-A|t:A1:A3|w:2|k:Enter|w:6")
        self.assertEqual(ws.Range("C2").Formula, "=SUM(A1:A3)*2")
        self.assertEqual(ws.Range("C2").Value, 12)
        # Ctrl+Shift+A after a function's name types its argument names, without the optional ones' brackets (Excel
        # 16, measured by keys 2026-11-28: =SUM(number1,number2,...)); Ctrl+A opens its Function Arguments
        self.keys("g:C3|w:2|t:=SUM|w:2|k:c-s-A|w:3")
        self.assertEqual(self.edit_text(), "=SUM(number1,number2,...)")
        self.keys("k:Escape|w:3|g:C3|w:2|t:=ROUND(|w:2|k:c-s-A|w:3")
        self.assertEqual(self.edit_text(), "=ROUND(number,num_digits)")
        self.keys("k:Escape|w:3|g:C3|w:2|t:=VLOOKUP|w:2|k:c-s-A|w:3")
        self.assertEqual(self.edit_text(), "=VLOOKUP(lookup_value,table_array,col_index_num,range_lookup)")
        self.keys("k:Escape|w:3|g:C4|w:2|t:=ROUND(|w:2|k:c-A|w:8")
        self.assertTrue(self.state().get("dialog"))
        self.keys("k:Escape|w:3|k:Escape|w:3")
        # OK enters the formula, as Excel's Function Arguments does: the call went back into the editor and the
        # entry waited for another Enter
        self.keys("g:C5|w:2|t:=SUM|w:2|k:c-A|w:8|t:A1:A3|w:2|k:Enter|w:6")
        self.assertFalse(self.state().get("dialog"))
        self.assertIsNone(self.state().get("edit"), "the entry is over")
        self.assertEqual(ws.Range("C5").Formula, "=SUM(A1:A3)")
        self.assertEqual(ws.Range("C5").Value, 6)

    def test_function_library_menus_open_function_arguments(self):
        # M7: the Function Library had no category menus
        from yc import ui

        self.keys("g:C1|w:2")
        tx, ty = (int(v) for v in self.state()["ribbon"]["tabs"]["Formulas"])
        self.keys(f"m:{tx},{ty}|w:2|c:{tx},{ty}|w:5")
        r = next((i["rect"] for i in ui.ribbon_items() if i["cmd"] == "FunctionsMath"), None)
        self.assertIsNotNone(r, "Formulas > Math & Trig isn't on the ribbon")
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:5")
        entry = next((i["rect"] for i in ui.ribbon_items() if i["cmd"] == "InsertFn_ABS"), None)
        self.assertIsNotNone(entry, "ABS isn't in the Math & Trig menu")
        x, y = int((entry[0] + entry[2]) / 2), int((entry[1] + entry[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:8")
        self.assertTrue(self.state().get("dialog"))
        self.keys("k:c-A|t:-3|w:2|k:Enter|w:6")
        self.assertEqual(self.ws.Range("C1").Formula, "=ABS(-3)")
        self.assertEqual(self.ws.Range("C1").Value, 3)

    def test_show_formulas_belongs_to_the_sheet_and_drops_number_formats(self):
        # M10: Show Formulas was global and kept number formats
        from yc import excel

        ws = self.ws
        ws.Range("A1").Value = 45000
        ws.Range("A1").NumberFormat = "yyyy-mm-dd"
        self.keys("g:A1|w:2|k:c-GraveAccent|w:4")
        try:
            self.assertTrue(excel.ActiveWindow.DisplayFormulas)
            ws2 = excel.Worksheets.Add()
            ws2.Activate()
            self.keys("w:4")
            self.assertFalse(excel.ActiveWindow.DisplayFormulas)
            ws.Activate()
            self.keys("w:4")
            self.assertTrue(excel.ActiveWindow.DisplayFormulas)
        finally:
            self.keys("g:A1|w:2|k:c-GraveAccent|w:4")
        self.assertFalse(excel.ActiveWindow.DisplayFormulas)

    def test_table_names_and_columns_in_autocomplete_and_the_name_box(self):
        # M11: tables were missing from AutoComplete and the Name Box
        ws = self.ws
        ws.Range("A1:B3").Value = [["Qty", "Px"], [1, 2], [3, 4]]
        lo = ws.ListObjects.Add(1, ws.Range("A1:B3"), None, 1)
        lo.Name = "SalesF"
        self.keys("w:4|g:H1|w:2|t:=SalesF|w:4")
        self.assertIn("SalesF", self.labels())
        self.keys("k:Tab|w:2|t:[|w:4")
        got = self.labels()
        self.keys("k:Escape|w:2|k:Escape|w:2")
        self.assertIn("Qty", got)
        self.assertIn("Px", got)
        self.assertIn("#Headers", got)

    def test_numbers_written_below_or_right_of_a_table_grow_it(self):
        # text grew a table but numbers (the fast write path) didn't, as in Excel's AutoExpandListRange
        ws = self.ws
        ws.Range("A1:B3").Value = [["Qty", "Px"], [1, 2], [3, 4]]
        lo = ws.ListObjects.Add(1, ws.Range("A1:B3"), None, 1)
        ws.Range("A4").Value = 5
        self.assertEqual(lo.Range.Address, "$A$1:$B$4")
        ws.Range("A5:B6").Value = [[6, 7], [8, 9]]
        self.assertEqual(lo.Range.Address, "$A$1:$B$6")
        ws.Range("C2").Value = 1.5
        self.assertEqual(lo.Range.Address, "$A$1:$C$6")
        lo.Unlist()

    def test_function_arguments_describe_the_argument_being_typed(self):
        # M12: nothing said what col_index_num expects
        self.keys("g:C1|w:2|k:s-F3|w:6|t:vlookup|w:2|k:Enter|w:8")
        self.assertTrue(self.state().get("dialog"))
        self.keys("k:Tab|w:2|k:Tab|w:3")
        got = self.labels()
        self.keys("k:Escape|w:3")
        self.assertTrue(any(t.startswith("Col_index_num:") for t in got), [t for t in got if ":" in t][:10])

    # ---- Low ----
    def test_plus_starts_a_formula_that_points(self):
        # L4: "+" then a click committed the "+" and selected the cell
        ws = self.ws
        ws.Range("A1:A2").Value = [[2], [3]]
        self.keys("g:C1|w:2|t:+|A:A1|w:2|t:+|A:A2|w:2|k:Enter|w:4")
        self.assertEqual(ws.Range("C1").Formula, "=+A1+A2")
        self.assertEqual(ws.Range("C1").Value, 5)

    def test_the_same_reference_twice_has_one_color(self):
        # L5: the second A1 drew a third (purple) box over the first
        self.keys("g:E1|w:2|t:=A1+B1+A1|w:6")
        w, h, px = self.shot()
        r = self.rect("A1")
        x = int((r[0] + r[2]) / 2)
        purple = blue = 0
        for y in range(int(r[3]) - 4, int(r[3]) + 4):   # A1's bottom border (B1's box shares its right one)
            cr, cg, cb = px(x, y)
            purple += cr > 0x60 and cb > 0x80 and cg < 0x58 and cr - cg > 30   # kRefColors[2]
            blue += cb - cr > 60 and cg > 0x58 and cb > 0xA0                      # kRefColors[0]
        self.keys("k:Escape|w:2")
        self.assertEqual(purple, 0)
        self.assertGreater(blue, 0)

    def test_ctrl_slash_selects_the_current_array(self):
        # L7: no Ctrl+/. It selects an array entered with Ctrl+Shift+Enter; a dynamic array's spill finds no cells
        # (Excel 16, measured by keys)
        self.ws.Range("A1:A3").FormulaArray = "=ROW(A1:A3)"
        self.keys("g:A2|w:2|k:c-Slash|w:3")
        self.assertEqual(self.state()["selection"], "A1:A3")

    def test_f3_with_no_names_opens_nothing(self):
        # L8: F3 opened an empty Paste Name dialog
        from yc import excel

        for n in list(excel.ActiveWorkbook.Names):
            n.Delete()
        self.keys("g:C1|w:2|k:F3|w:4")
        self.assertEqual(self.state().get("dialog"), 0)

    def test_remove_precedent_arrows_leaves_the_dependent_ones(self):
        # L9: one Remove Arrows for both kinds
        ws = self.ws
        ws.Range("A1").Value = 1
        ws.Range("B1").Formula = "=A1"
        ws.Range("C1").Formula = "=B1"
        self.keys("g:B1|w:2|x:TracePrecedents|w:3|x:TraceDependents|w:3")
        self.assertEqual(len(self.conn.request("audit.arrows", {}, timeout=10)), 2)
        self.keys("x:RemovePrecedentArrows|w:3")
        arrows = self.conn.request("audit.arrows", {}, timeout=10)
        self.assertEqual(len(arrows), 1)
        self.assertEqual(arrows[0][2], 1)   # from B1 (column 1) to C1: the dependent one
        self.keys("x:RemoveArrows|w:2")

    def test_autosum_writes_subtotal_on_a_filtered_list(self):
        # L12: AutoSum under a filtered list summed the hidden rows too
        ws = self.ws
        ws.Range("A1").Value = "Qty"
        ws.Range("A2:A10").Value = [[i] for i in range(1, 10)]
        ws.Range("A1:A10").AutoFilter(1, ">4")
        self.keys("w:4|g:A11|w:2|k:a-Equal|w:3|k:Enter|w:4")
        self.assertTrue(str(ws.Range("A11").Formula).startswith("=SUBTOTAL(9,"), ws.Range("A11").Formula)
        self.assertEqual(ws.Range("A11").Value, sum(range(5, 10)))

    def test_name_box_lists_only_names_of_ranges(self):
        # L14: a constant name (nowhere to go) and a "Name Manager..." entry were listed
        from yc import excel, ui

        wb = excel.ActiveWorkbook
        wb.Names.Add("RateF", "=0.05")
        wb.Names.Add("DataF", f"='{self.ws.Name}'!$A$1:$A$3")
        dd = None
        for win in {i["window"] for i in ui.items()}:
            dd = dd or next((i for i in ui.items(win) if i["id"] == "##namebox_dd"), None)
        self.assertIsNotNone(dd)
        r = dd["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:5")
        got = self.labels()
        self.keys("k:Escape|w:2|A:J20|w:3")   # the click on the Name Box took the keyboard: back to the sheet
        wb.Names("RateF").Delete()
        wb.Names("DataF").Delete()
        self.assertIn("DataF", got)
        self.assertNotIn("RateF", got)
        self.assertNotIn("Name Manager...", got)

    def test_a_click_in_the_formula_bar_puts_the_caret_there(self):
        # L6: the caret went to the end
        from yc import ui

        self.ws.Range("A1").Formula = "=SUM(A2:A3)"
        self.keys("g:A1|w:3")
        bar = next((i for i in ui.items() if i["id"] == "##fbar_click"), None)
        if bar is None:
            for win in {i["window"] for i in ui.items()}:
                bar = bar or next((i for i in ui.items(win) if i["id"] == "##fbar_click"), None)
        self.assertIsNotNone(bar)
        r = bar["rect"]
        # just after "=": the bar's text starts 6 px in; "=" is about 7 px wide
        x, y = int(r[0]) + 6 + 8, int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4|t:X|w:3")
        self.assertEqual(self.edit_text(), "=XSUM(A2:A3)")
        self.keys("k:Escape|w:3")

    def test_the_name_box_lists_functions_while_a_formula_is_typed(self):
        # L1: the Name Box kept listing names while a formula was typed
        from yc import ui

        self.keys("g:C1|w:2|t:=|w:3")
        dd = None
        for win in {i["window"] for i in ui.items()}:
            dd = dd or next((i for i in ui.items(win) if i["id"] == "##namebox_dd"), None)
        self.assertIsNotNone(dd)
        r = dd["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:5")
        got = self.labels()
        self.keys("k:Escape|w:2|k:Escape|w:3|A:J20|w:3")   # the click on the Name Box took the keyboard: back to the sheet
        self.assertIn("SUM", got)
        self.assertIn("More Functions...", got)

    def test_f4_cycles_r1c1_references(self):
        # L3: F4 did nothing with R1C1 references
        from yc import excel

        excel.Application.ReferenceStyle = -4150  # xlR1C1
        try:
            self.keys("g:C3|w:2|t:=R[-1]C[-1]|w:2|k:F4|w:3")
            self.assertEqual(self.edit_text(), "=R2C2")
            self.keys("k:F4|w:3")
            self.assertEqual(self.edit_text(), "=R[-1]C2")
            self.keys("k:Escape|w:3")
        finally:
            excel.Application.ReferenceStyle = 1  # xlA1
            self.keys("w:3")

    def test_fx_on_a_cell_shows_the_call_being_built(self):
        # L16: the cell wasn't touched until OK
        self.ws.Range("A1:A3").Value = [[1], [2], [3]]
        self.keys("g:C1|w:2|k:s-F3|w:6")
        self.assertEqual(self.edit_text(), "=")
        self.keys("k:c-A|t:SUM|w:2|k:Enter|w:6|t:A1:A3|w:4")
        self.assertEqual(self.edit_text(), "=SUM(A1:A3)")
        self.keys("k:Escape|w:4")
        self.assertEqual(self.state().get("dialog"), 0)
        self.assertNotIn("edit", self.state())
        self.assertIsNone(self.ws.Range("C1").Formula or None)
        self.keys("g:C1|w:2|k:s-F3|w:6|k:c-A|t:SUM|w:2|k:Enter|w:6|t:A1:A3|w:3|k:Enter|w:6")
        self.assertEqual(self.ws.Range("C1").Formula, "=SUM(A1:A3)")

    def test_formula_autocomplete_can_be_turned_off(self):
        # L15: no Application.DisplayFormulaAutoComplete
        from yc import excel

        excel.Application.DisplayFormulaAutoComplete = False
        try:
            self.keys("g:C1|w:2|t:=SU|w:4")
            self.assertNotIn("SUM", self.labels())
            self.keys("k:Escape|w:3")
        finally:
            excel.Application.DisplayFormulaAutoComplete = True
        self.keys("g:C1|w:2|t:=SU|w:4")
        self.assertIn("SUM", self.labels())
        self.keys("k:Escape|w:3")


if __name__ == "__main__":
    unittest.main()
