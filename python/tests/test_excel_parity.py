# SPDX-License-Identifier: Apache-2.0
"""Places where YCGrid worked differently from Excel, each checked the way a person does it (docs/excel-parity-backlog.md).

Every test fails without its fix. Run: python -m unittest tests.test_excel_parity (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
PICK = "\U000f0244"  # the range box button's icon


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


class ExcelParityTest(unittest.TestCase):
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

        self.keys("k:Escape|w:2|m:2,2|w:2")
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:2")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.25)

    def dialog(self):
        from yc import ui

        return ui.state().get("dialog", 0)

    def click(self, label, window="", nth=0):
        from yc import ui

        hits = [i for i in ui.items(window) if i["label"] == label]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        self.assertGreater(len(hits), nth, f"{label!r} isn't on the screen")
        r = hits[nth]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def cell_center(self, addr):
        from yc import ui

        r = ui.cell_rect(addr)
        return (r[0] + r[2]) // 2, (r[1] + r[3]) // 2

    # ---- tests ----
    def test_format_cells_ok_keeps_the_number_format(self):
        # opening Format Cells and pressing OK rewrote a date format as yyyy/m/d
        r = self.ws.Range("B2")
        r.Value = 46000
        r.NumberFormat = "mmm-yy"
        self.keys("g:B2|w:2|k:c-1|w:6")
        self.assertTrue(self.dialog())
        self.keys("k:Enter|w:6")
        self.assertEqual(self.dialog(), 0)
        self.assertEqual(r.NumberFormat, "mmm-yy")

    def test_a_link_on_a_number_keeps_the_number(self):
        # Edit Hyperlink wrote the shown text back: 1234 became the text "1,234", formulas were lost
        r = self.ws.Range("C3")
        r.Value = 1234
        r.NumberFormat = "#,##0"
        self.ws.Range("C4").Formula = "=C3*2"
        for addr in ("C3", "C4"):
            self.keys(f"g:{addr}|w:2|k:c-K|w:6|t:https://example.com|w:2|k:Enter|w:6")
        self.assertEqual(self.ws.Range("C3").Value, 1234)
        self.assertEqual(self.ws.Range("C4").Formula, "=C3*2")
        self.assertEqual(self.ws.Hyperlinks.Count, 2)

    def test_ctrl_minus_then_enter_shifts_cells_up(self):
        # the Delete dialog started on Entire row: Ctrl+- and Enter deleted the whole row
        self.ws.Range("A1:C3").Value = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
        self.keys("g:B2|w:2|k:c-Minus|w:6")
        self.assertTrue(self.dialog())
        self.keys("k:Enter|w:6")
        self.assertEqual([self.ws.Range("A2").Value, self.ws.Range("B2").Value, self.ws.Range("C2").Value], [4, 8, 6])

    def test_autosum_fills_the_empty_last_row_and_column(self):
        # AutoSum on a range wrote below it, over what was there, even with an empty total row selected
        self.ws.Range("A1:B2").Value = [[1, 2], [3, 4]]
        self.ws.Range("A4").Value = "keep"
        self.keys("g:A1:C3|w:2|k:a-Equal|w:6")
        self.assertEqual(self.ws.Range("A3").Formula, "=SUM(A1:A2)")
        self.assertEqual(self.ws.Range("C1").Formula, "=SUM(A1:B1)")
        self.assertEqual(self.ws.Range("C3").Value, 10)
        self.assertEqual(self.ws.Range("A4").Value, "keep")
        self.ws.Range("E1:G1").Value = [[1, 2, 3]]
        self.keys("g:E1:G1|w:2|k:a-Equal|w:6")
        self.assertEqual(self.ws.Range("H1").Formula, "=SUM(E1:G1)")  # one row: the total to its right

    def test_validation_clear_all_waits_for_ok(self):
        from yc import excel

        self.ws.Range("D2").Validation.Add(excel.xlValidateWholeNumber, excel.xlValidAlertStop, excel.xlBetween, "1", "9")
        self.keys("g:D2|w:2")
        from yc import ui

        ui.command("DataValidation")
        self.keys("w:6")
        self.click("Clear All")
        self.assertTrue(self.dialog(), "the dialog stays open")
        self.keys("k:Escape|w:6")  # Cancel: the rule is still there
        self.assertEqual(self.ws.Range("D2").Validation.Type, excel.xlValidateWholeNumber)

    def test_filter_search_shows_only_the_results(self):
        self.ws.Range("A1:A6").Value = [["Sym"], ["TXF"], ["MXF"], ["TXO"], ["ABC"], ["TXF"]]
        self.keys("g:A1:A6|w:2|k:c-s-L|w:4|g:A1|w:2|k:a-DownArrow|w:6")
        from yc import ui

        box = [i for i in ui.items("") if i["id"] == "##search"]  # the filter menu's search box
        self.assertTrue(box)
        r = box[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:3|t:TX|w:4")      # search: TXF, TXO

        ok = [i for i in ui.items() if i["label"] == "OK"]
        self.assertTrue(ok)
        r = ok[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        hidden = [self.ws.Rows(i).Hidden for i in range(2, 7)]
        self.assertEqual(hidden, [False, True, False, True, False])

    def test_unhide_sheet_lists_the_hidden_sheets(self):
        from yc import excel, ui

        a = excel.Worksheets.Add()
        a.Name = "HiddenA"
        b = excel.Worksheets.Add()
        b.Name = "HiddenB"
        self.ws.Activate()
        a.Visible = False
        b.Visible = False
        self.keys("w:3")
        ui.command("UnhideSheet")
        self.keys("w:6")
        self.assertTrue(self.dialog())
        self.click("HiddenB")
        self.click("OK")
        self.assertTrue(excel.Worksheets("HiddenB").Visible)
        self.assertFalse(excel.Worksheets("HiddenA").Visible)
        excel.Worksheets("HiddenA").Visible = True
        a.Delete()
        b.Delete()

    def test_move_or_copy_makes_a_copy_where_chosen(self):
        from yc import excel, ui

        n = excel.Worksheets.Count
        self.ws.Range("A1").Value = "orig"
        ui.command("CopySheet")
        self.keys("w:6")
        self.assertTrue(self.dialog(), "Move or Copy is a dialog")
        self.click(self.ws.Name, "")                      # before itself
        self.click("Create a copy")
        self.click("OK")
        self.assertEqual(excel.Worksheets.Count, n + 1)
        copy = excel.Application.ActiveSheet
        self.assertNotEqual(copy.Name, self.ws.Name)
        self.assertEqual(copy.Range("A1").Value, "orig")
        self.assertEqual(copy.Index, self.ws.Index - 1, "the copy sits before the sheet chosen")
        copy.Delete()

    def test_hide_and_autofit_act_on_every_selected_row(self):
        self.keys("g:2:3,6:6|w:2|k:c-9|w:4")
        self.assertEqual([self.ws.Rows(i).Hidden for i in (2, 3, 4, 6)], [True, True, False, True])
        self.keys("g:1:7|w:2|k:c-s-9|w:4")
        self.assertFalse(any(self.ws.Rows(i).Hidden for i in (2, 3, 6)))
        self.ws.Range("A2").Value = "two\nlines"
        self.ws.Range("A3").Value = "two\nlines"
        self.ws.Range("A2:A3").WrapText = True
        self.ws.Rows("2:3").RowHeight = 15
        from yc import ui

        self.keys("g:A2:A3|w:2")
        ui.command("AutoFitRows")
        self.keys("w:4")
        self.assertGreater(self.ws.Rows(3).RowHeight, 20, "the second row is fitted too")

    def test_go_to_lists_names_and_has_special(self):
        from yc import excel, ui

        excel.ActiveWorkbook.Names.Add("Target", "=" + self.ws.Name + "!$C$9")
        self.keys("k:F5|w:6")
        self.assertTrue(self.dialog())
        self.assertTrue([i for i in ui.items() if i["label"] == "Special..."])
        self.click("Target")
        self.keys("k:Enter|w:6")
        self.assertEqual(ui.state()["active_cell"], "C9")
        excel.ActiveWorkbook.Names("Target").Delete()

    def test_goal_seek_cells_picked_on_the_sheet(self):
        # every range box of a dialog takes cells picked on the sheet (Excel's RefEdit)
        from yc import ui

        self.ws.Range("A1").Value = 2
        self.ws.Range("B1").Formula = "=A1*3"
        self.keys("g:D4|w:2")
        ui.command("GoalSeek")
        self.keys("w:6")
        dlg = self.dialog()
        picks = sorted([i for i in ui.items() if i["label"] == PICK], key=lambda i: i["rect"][1])
        self.assertEqual(len(picks), 2, "Set cell and By changing cell have their buttons")
        for pick, cell in ((picks[0], "B1"), (picks[1], "A1")):
            r = pick["rect"]
            x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
            self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")
            cx, cy = self.cell_center(cell)
            self.keys(f"m:{cx},{cy}|w:3|c:{cx},{cy}|w:4|k:Enter|w:6")
            self.assertEqual(self.dialog(), dlg)
            picks = sorted([i for i in ui.items() if i["label"] == PICK], key=lambda i: i["rect"][1])
        self.keys("k:Tab|w:1")
        # To value: the second box
        boxes = sorted([i for i in ui.items() if i["id"].startswith("##") and "To value" in i["id"]], key=lambda i: i["rect"][1])
        self.assertTrue(boxes)
        r = boxes[0]["rect"]
        self.keys(f"m:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:2|c:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:3|k:c-A|t:12|w:2|k:Enter|w:10")
        self.keys("k:Enter|w:6")
        self.assertAlmostEqual(self.ws.Range("A1").Value, 4, places=4)


    def test_function_arguments_box_per_argument(self):
        # Excel: Insert Function -> Function Arguments (a box per argument, its value beside it) -> the cell
        from yc import ui

        self.ws.Range("A1:A3").Value = [[1], [2], [3]]
        self.keys("g:C1|w:2|k:s-F3|w:6|t:sum|w:3|k:Enter|w:8")
        self.assertTrue(self.dialog(), "Function Arguments follows Insert Function")
        self.assertGreaterEqual(len([i for i in ui.items("") if i["id"] == "##rb"]), 2, "a box for each argument (number1, number2)")
        self.keys("t:A1:A3|w:3")
        ui.screenshot(os.path.join(os.environ.get("TEMP", "."), "yc_fnargs.png"), max_width=1600)
        self.assertEqual(len([i for i in ui.items("") if i["id"] == "##rb"]), 2, "number2 stays free")
        self.keys("k:Tab|w:1|t:A2|w:3")                       # number2 filled: number3 appears
        self.assertGreaterEqual(len([i for i in ui.items("") if i["id"] == "##rb"]), 3, "a filled last box adds another")
        self.keys("k:c-A|k:Delete|w:3")
        self.keys("k:Enter|w:6")
        self.assertEqual(self.ws.Range("C1").Formula, "=SUM(A1:A3)")
        # fx on a cell holding one call: its arguments come back in the boxes
        self.keys("g:C1|w:2|k:s-F3|w:8")
        self.assertTrue(self.dialog())
        self.keys("k:c-A|t:A1:A2|w:2|k:Enter|w:6")
        self.assertEqual(self.ws.Range("C1").Formula, "=SUM(A1:A2)")
        self.assertEqual(self.ws.Range("C1").Value, 3)

    def test_paste_options_button_pastes_another_way(self):
        from yc import ui

        self.ws.Range("A1:B1").Value = [[2, "=A1*10"]]
        self.keys("g:A1:B1|w:2|k:c-C|w:2|g:A3|w:2|k:c-V|w:6")
        self.assertEqual(self.ws.Range("B3").Formula, "=A3*10")
        btn = [i for i in ui.items("") if i["id"] == "##btn" and i["window"] in ("Workbook", "")]
        self.assertTrue(btn, "the Paste Options button is beside the pasted cells")
        self.keys("k:LeftCtrl|w:4")                            # Ctrl alone opens the options, as in Excel
        self.click("Values")
        self.assertFalse(str(self.ws.Range("B3").Formula).startswith("="), "pasted again as values")
        self.assertEqual(self.ws.Range("B3").Value, 20)
        self.keys("k:LeftCtrl|w:4")
        self.click("Transpose")
        self.assertEqual((self.ws.Range("A3").Value, self.ws.Range("A4").Formula), (2, "=A3*10"))

    def test_dropping_cells_onto_data_asks_first(self):
        from yc import ui

        self.ws.Range("A1:A2").Value = [[1], [2]]
        self.ws.Range("C1").Value = "keep"
        self.keys("g:A1:A2|w:2")
        r = ui.cell_rect("A1")
        x0, y0 = int(r[2]) - 1, int((r[1] + r[3]) / 2)          # the selection's right border
        c = ui.cell_rect("C1")
        x1, y1 = int((c[0] + c[2]) / 2), int((c[1] + c[3]) / 2)
        self.keys(f"m:{x0},{y0}|w:6|d:{x0},{y0},{x1},{y1}|w:8")
        self.assertTrue(self.dialog(), "Excel asks: There's already data here")
        self.keys("k:Escape|w:6")
        self.assertEqual((self.ws.Range("A1").Value, self.ws.Range("C1").Value), (1, "keep"))
        self.keys("g:A1:A2|w:2")
        self.keys(f"m:{x0},{y0}|w:6|d:{x0},{y0},{x1},{y1}|w:8|k:Enter|w:8")
        self.assertEqual((self.ws.Range("A1").Value, self.ws.Range("C1").Value, self.ws.Range("C2").Value), (None, 1, 2))

    def test_fill_handle_dragged_back_clears_only_those_cells(self):
        from yc import ui

        self.ws.Range("B1:B4").Value = [[1], [2], [3], [4]]
        self.keys("g:B1:B4|w:2")
        r = ui.cell_rect("B4")
        hx, hy = int(r[2]) - 1, int(r[3]) - 1
        t2 = ui.cell_rect("B2")
        self.keys(f"m:{hx},{hy}|w:6|d:{hx},{hy},{int((t2[0] + t2[2]) / 2)},{int((t2[1] + t2[3]) / 2)}|w:8")
        self.assertEqual([self.ws.Range(f"B{i}").Value for i in range(1, 5)], [1, 2, None, None])

    def test_format_shape_pane(self):
        from yc import ui

        sp = self.ws.Shapes.AddShape(1, 100, 40, 120, 60)  # a rectangle
        self.keys("w:4")
        objs = [i for i in ui.items("Workbook") if i["id"] == "##obj"]
        self.assertTrue(objs)
        r = objs[-1]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:3|r:|w:6")
        self.click("Format Shape...")
        self.assertTrue(any("Format Shape" in w["name"] for w in ui.windows()), [w["name"] for w in ui.windows()])
        self.click("No fill")
        self.assertFalse(sp.Fill.Visible)
        sp.Delete()

    def test_border_tab_outline_and_inside(self):
        # Outline goes on the selection's rim only, Inside between its cells (it used to box every cell)
        from yc import excel, ui

        self.keys("g:B2:C3|w:2|k:c-1|w:6")
        self.click("Border")
        ui.screenshot(os.path.join(os.environ.get("TEMP", "."), "yc_border_tab.png"), max_width=1600)
        self.click("Outline")
        self.click("OK")
        r = self.ws.Range
        top, bottom, left, right = excel.xlEdgeTop, excel.xlEdgeBottom, excel.xlEdgeLeft, excel.xlEdgeRight
        none = excel.xlLineStyleNone
        self.assertNotEqual(r("B2").Borders(top).LineStyle, none)
        self.assertNotEqual(r("C3").Borders(right).LineStyle, none)
        self.assertEqual(r("B2").Borders(bottom).LineStyle, none, "no line inside")
        self.assertEqual(r("B2").Borders(right).LineStyle, none, "no line inside")
        self.keys("g:B2:C3|w:2|k:c-1|w:6")
        self.click("Border")
        self.click("Inside")
        self.click("OK")
        self.assertNotEqual(r("B2").Borders(bottom).LineStyle, none)
        self.assertNotEqual(r("B2").Borders(right).LineStyle, none)
        # B1's bottom is the outline's top edge, which B1 shows too (Excel 16, measured); its top stays empty
        self.assertNotEqual(r("B1").Borders(bottom).LineStyle, none)
        self.assertEqual(r("B1").Borders(top).LineStyle, none)

    def test_pivot_report_right_click_menu(self):
        # Excel's right-click on a PivotTable report: Summarize Values By, Sort, Remove "field"...
        from yc import excel

        self.ws.Range("A1:B5").Value = [["Region", "Sales"], ["East", 10], ["West", 20], ["East", 30], ["North", 5]]
        pc = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, self.ws.Name + "!A1:B5")
        pt = pc.CreatePivotTable(self.ws.Name + "!D3", "PTmenu")
        pt.PivotFields("Region").Orientation = excel.xlRowField
        df = pt.AddDataField(pt.PivotFields("Sales"), None, excel.xlSum)
        self.keys("w:6")
        # a value cell (E4: East's sum) -> Summarize Values By -> Average
        self.keys("m:2,2|R:E4|w:6")
        self.click("Summarize Values By")
        self.click("Average")
        self.assertEqual(pt.DataFields(1).Function, excel.xlAverage)
        # a row label -> Sort -> Sort Z to A
        self.keys("m:2,2|R:D4|w:6")
        self.click("Sort")
        self.click("Sort Z to A")
        self.keys("w:6")
        self.assertEqual(self.ws.Range("D4").Value, "West")
        del df

    def test_header_footer_presets_and_insert_buttons(self):
        # Excel: the Header list ("Page 1 of 1"...), a preview, and Custom Header's buttons insert codes at the caret
        from yc import ui

        ui.command("PageSetupDialog")
        self.keys("w:6")
        self.click("Header/Footer")
        # the Header list sits just above Custom Header... (combo boxes carry no label for the item recorder)
        ch = [i for i in ui.items() if i["label"] == "Custom Header..."][0]["rect"]
        x, y = int(ch[0]) + 40, int(ch[1] - (ch[3] - ch[1]) * 0.75)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:5")
        self.click("Page 1 of 1")
        self.click("Custom Footer...")
        boxes = sorted([i for i in ui.items("Page Setup") if i["id"] == "##sec"], key=lambda i: i["rect"][0])
        self.assertEqual(len(boxes), 3)
        r = boxes[2]["rect"]                                   # the right section: type, then insert the sheet name
        self.keys(f"m:{int(r[0]) + 10},{int(r[1]) + 10}|w:2|c:{int(r[0]) + 10},{int(r[1]) + 10}|w:3|t:Sheet |w:2")
        self.click("Sheet Name")
        ui.screenshot(os.path.join(os.environ.get("TEMP", "."), "yc_header_footer.png"), max_width=1600)
        self.click("OK")
        self.assertEqual(self.ws.PageSetup.CenterHeader, "Page &P of &N")
        self.assertEqual(self.ws.PageSetup.RightFooter, "Sheet &A")

    def test_new_formatting_rule_types_and_manager(self):
        # Excel's New Formatting Rule: six rule types (it had only "Use a formula"); the manager edits and duplicates
        from yc import excel, ui

        self.ws.Range("A1:A6").Value = [[5], [9], [1], [7], [3], [8]]
        self.keys("g:A1:A6|w:2")
        ui.command("CfNewRule")
        self.keys("w:6")
        self.click("Format only top or bottom ranked values")
        ui.screenshot(os.path.join(os.environ.get("TEMP", "."), "yc_cf_rule.png"), max_width=1600)
        self.click("OK")
        fcs = self.ws.Range("A1:A6").FormatConditions
        self.assertEqual(fcs.Count, 1)
        self.assertEqual(fcs(1).Type, excel.xlTop10)
        ui.command("CfManageRules")
        self.keys("w:6")
        rows = [i for i in ui.items() if "Top" in i["label"]]
        self.assertTrue(rows, "the rule is listed")
        self.click(rows[0]["label"])
        self.click("Duplicate Rule")
        self.assertEqual(self.ws.Range("A1:A6").FormatConditions.Count, 1, "Excel's manager changes the sheet at OK / Apply")
        self.click("Apply")
        self.assertEqual(self.ws.Range("A1:A6").FormatConditions.Count, 2)
        self.click("OK")
        self.ws.Range("A1:A6").FormatConditions.Delete()

    def test_text_to_columns_fixed_width_ruler(self):
        # Excel's fixed width: breaks suggested at the spaces, and a click on the ruler adds one (they were typed as "3, 8")
        from yc import ui

        self.ws.Range("A1:A2").Value = [["AA BB12345"], ["CC DD67890"]]
        self.keys("g:A1:A2|w:2")
        ui.command("TextToColumns")
        self.keys("w:6")
        self.click("Fixed width")
        self.click("Next >")  # step 2 of Excel's wizard: the break lines
        ruler = [i for i in ui.items("") if i["id"] == "##ruler"]
        self.assertTrue(ruler, "the ruler is shown")
        ui.screenshot(os.path.join(os.environ.get("TEMP", "."), "yc_ttc.png"), max_width=1600)
        self.click("Finish")
        self.assertEqual([self.ws.Range(c).Value for c in ("A1", "B1")], ["AA", "BB12345"], "split at the suggested break")

    def test_spelling_starts_at_the_active_cell_and_selects_the_word(self):
        from yc import ui

        self.ws.Range("A1").Value = "speling"
        self.ws.Range("A5").Value = "wrnog"
        self.keys("g:A3|w:2|k:F7|w:8")
        if not any("yc_spelling" in w["name"] for w in ui.windows()):
            self.skipTest("no Windows spell checker for English here")
        self.assertEqual(ui.state()["active_cell"], "A5", "the first word after the active cell, selected")
        self.click("Cancel")

    def test_note_is_typed_beside_its_cell(self):
        # Excel: Shift+F2 opens the note's yellow box at the cell (no dialog); a click elsewhere keeps the text
        from yc import ui

        self.keys("g:B3|w:2|k:s-F2|w:4")
        self.assertEqual(self.dialog(), 0, "no dialog")
        self.assertTrue([i for i in ui.items("") if i["id"].endswith("##note_inplace")], "the note box is on the sheet")
        self.keys("t:first line|w:2")
        x, y = self.cell_center("F12")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        c = self.ws.Range("B3").Comment
        self.assertIsNotNone(c, "the note is kept")
        self.assertIn("first line", c.Text() if callable(getattr(c, "Text", None)) else c.Text)
        self.assertEqual(ui.state()["active_cell"], "F12", "the click also selects the cell")
        # editing again shows the text, and typing adds to it
        self.keys("g:B3|w:2|k:s-F2|w:4|k:c-End|t: more|w:2")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        c = self.ws.Range("B3").Comment
        self.assertIn("first line more", c.Text() if callable(getattr(c, "Text", None)) else c.Text)
        # ...and the caret starts after the note's text, as Excel's: without Ctrl+End the typing went before it
        # (once another box had had the keyboard, a cell's entry here: ImGui keeps the last box's caret otherwise)
        self.keys("g:D9|w:2|t:x|k:Enter|w:3|g:B3|w:2|k:s-F2|w:6|t: again|w:2|k:Escape|w:4")
        c = self.ws.Range("B3").Comment
        self.assertEqual(c.Text() if callable(getattr(c, "Text", None)) else c.Text, "first line more again")
        # Escape also ends the editing and keeps what was typed
        self.keys("g:C4|w:2|k:s-F2|w:4|t:kept|k:Escape|w:4")
        c = self.ws.Range("C4").Comment
        self.assertIsNotNone(c, "Escape keeps the note")
        self.assertFalse([i for i in ui.items("") if i["id"].endswith("##note_inplace")], "and closes the box")

    def test_paste_special_operation_and_link(self):
        # Excel: copy a number, Paste Special > Multiply scales the selection; Paste Link writes a formula to the copied cell
        self.ws.Range("E1").Value = 10
        self.ws.Range("A1:A3").Value = [[1], ["=A1+1"], ["x"]]
        self.keys("g:E1|w:2|k:c-C|w:2|g:A1:A3|w:2|k:c-a-V|w:4")
        self.assertTrue(self.dialog(), "Paste Special opens")
        self.keys("k:M|w:2|k:Enter|w:4")  # M: Multiply, as Excel's underlined letter
        self.assertEqual(self.ws.Range("A1").Value, 10)
        self.assertEqual(self.ws.Range("A2").Formula, "=(A1+1)*10", "a formula is wrapped, not replaced by its value")
        self.assertEqual(self.ws.Range("A3").Value, "x", "text stays")
        self.keys("g:E1|w:2|k:c-C|w:2|g:C5|w:2|k:c-a-V|w:4")
        self.click("Paste Link")
        self.assertEqual(self.ws.Range("C5").Formula, "=$E$1")
        self.assertEqual(self.ws.Range("C5").Value, 10)

    def _status_bar(self):
        from yc import ui

        # the status bar is a fixed window (not in ui.windows()): its zoom percentage spans its height
        pct = [i for i in ui.items("") if i["id"] == "##zoom_pct"]
        self.assertTrue(pct, "the status bar's zoom percentage")
        r = pct[0]["rect"]
        return [0, r[1], r[0], r[3]]

    def test_status_bar_statistics_chosen_by_right_click(self):
        # Excel: right-click the status bar to show Min / Max / Numerical Count; the menu shows each value
        from yc import ui

        self.ws.Range("A1:A3").Value = [[4], [9], ["x"]]
        self.keys("g:A1:A3|w:3")
        st = ui.state()["status_stats"]
        self.assertIn("Average: 6.5", st)
        self.assertIn("Count: 3", st)
        self.assertIn("Sum: 13", st)
        self.assertNotIn("Max", st)
        r = self._status_bar()
        x, y = int(r[0] + (r[2] - r[0]) * 0.5), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|r:|w:4")
        self.click("Max")
        self.assertIn("Max: 9", ui.state()["status_stats"])
        self.keys(f"m:{x},{y}|w:2|r:|w:4")
        self.click("Max")  # and off again (the next tests see the default)
        self.assertNotIn("Max", ui.state()["status_stats"])

    def test_status_bar_zoom_slider(self):
        from yc import ui

        plus = [i for i in ui.items("") if i["id"] == "##zoom_in"]
        self.assertTrue(plus, "the + button of the zoom slider")
        r = plus[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        from yc import excel

        excel.ActiveWindow.Zoom = 100
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3")
        self.assertEqual(excel.ActiveWindow.Zoom, 110, "+ zooms in by 10%")
        slider = [i for i in ui.items("") if i["id"] == "##zoom_slider"][0]["rect"]
        sx, sy = int((slider[0] + slider[2]) / 2), int((slider[1] + slider[3]) / 2)
        self.keys(f"m:{sx + 30},{sy}|w:2|d:{sx + 30},{sy},{sx},{sy}|w:3")
        self.assertEqual(excel.ActiveWindow.Zoom, 100, "the slider's middle is 100%")

    def test_find_is_modeless_with_excel_options(self):
        # Excel: the sheet can be clicked and typed in while Find stays open; Look in starts on Formulas;
        # Shift+Enter finds the previous match; notes can be searched
        from yc import ui

        self.ws.Range("A1").Value = "=1+1"
        self.ws.Range("B2").Value = "apple"
        self.ws.Range("B5").Value = "apple pie"
        self.ws.Range("D9").AddComment("apple note")
        self.keys("g:A1|w:2|k:c-F|w:4")
        self.assertEqual(self.dialog(), 0, "no modal dialog")
        self.assertTrue(any("yc_find" in w["name"] for w in ui.windows()), "the Find window")
        self.keys("k:c-A|t:apple|k:Enter|w:3")
        self.assertEqual(ui.state()["active_cell"], "B2")
        self.keys("k:Enter|w:3")
        self.assertEqual(ui.state()["active_cell"], "B5")
        self.keys("k:s-Enter|w:3")
        self.assertEqual(ui.state()["active_cell"], "B2", "Shift+Enter goes back")
        # the sheet takes clicks and typing with the window open
        x, y = self.cell_center("F3")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3|t:typed|k:Enter|w:3")
        self.assertEqual(self.ws.Range("F3").Value, "typed")
        self.assertTrue(any("yc_find" in w["name"] for w in ui.windows()), "Find stays open")
        # Options >>: Look in starts on Formulas (=1+1 is found by "1+1"), Notes finds the note
        self.click("Options >>")
        combos = sorted([i for i in ui.items("") if i["id"] == "##find_look"], key=lambda i: i["rect"][1])
        self.assertTrue(combos, "the Look in box")
        r = combos[0]["rect"]
        self.keys(f"m:{(r[0] + r[2]) // 2},{(r[1] + r[3]) // 2}|w:2|c:{(r[0] + r[2]) // 2},{(r[1] + r[3]) // 2}|w:4")
        self.click("Notes")
        box = [i for i in ui.items("") if i["id"] == "##find_what"][0]["rect"]
        self.keys(f"c:{(box[0] + box[2]) // 2},{(box[1] + box[3]) // 2}|w:2|k:c-A|t:note|k:Enter|w:3")
        self.assertEqual(ui.state()["active_cell"], "D9", "the cell with the note")
        self.click("Close")
        self.assertFalse(any("yc_find" in w["name"] for w in ui.windows()))

    def test_number_tab_lists_negatives_and_date_types(self):
        # Excel: the Number tab offers its choices as lists shown as they look, not codes to type
        self.ws.Range("A1").Value = -5
        self.keys("g:A1|w:2|k:c-1|w:5")
        self.click("Number")  # Ctrl+1 opens on the last tab used (Excel): the Number tab first
        self.keys("w:3")
        self.click("Number", nth=1)  # the category (the tab of the same name is above it)
        self.click("(1234.10)", nth=1)  # the fourth negative format: in parentheses and red
        self.click("OK")
        self.assertEqual(self.ws.Range("A1").NumberFormat, "0.00_);[Red](0.00)")
        self.ws.Range("B1").Value = 45000  # 2023-03-15
        self.keys("g:B1|w:2|k:c-1|w:5")
        self.click("Date")
        self.click("15-Mar")
        self.click("OK")
        self.assertEqual(self.ws.Range("B1").NumberFormat, "d-mmm")
        # opened again, the dialog shows Date with that type: OK keeps it
        self.keys("g:B1|w:2|k:c-1|w:5")
        self.click("OK")
        self.assertEqual(self.ws.Range("B1").NumberFormat, "d-mmm")
        # Currency with the euro sign
        self.ws.Range("C1").Value = 12.5
        self.keys("g:C1|w:2|k:c-1|w:5")
        self.click("Currency")
        from yc import ui

        sym = [i for i in ui.items("") if i["id"] == "##symbol"][0]["rect"]
        x, y = int((sym[0] + sym[2]) / 2), int((sym[1] + sym[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        self.click("\u20ac")
        self.click("OK")
        self.assertEqual(self.ws.Range("C1").NumberFormat, '"\u20ac"#,##0.00')

    def test_keyboard_modes_f8_shift_f8_and_end(self):
        from yc import ui

        self.ws.Range("A1:A6").Value = [[1], [2], [3], [4], [5], [6]]
        # F8: the arrows extend the selection until Escape
        self.keys("g:A1|w:2|k:F8|w:2|k:Down|k:Down|k:Right|w:3")
        self.assertEqual(ui.state()["selection"].replace("$", ""), "A1:B3")
        self.keys("k:Escape|w:2|k:Down|w:3")
        self.assertEqual(ui.state()["selection"].replace("$", ""), "A2", "Escape ends Extend Selection")
        # Shift+F8: a click adds an area
        self.keys("g:A1|w:2|k:s-F8|w:2|A:C5|w:3|k:Escape|w:2")
        from yc import excel

        self.assertEqual(excel.Selection.Address, "$A$1,$C$5")  # ui.state() shows the last area only
        # End, then Down: as Ctrl+Down
        self.keys("g:A1|w:2|k:End|w:2|k:Down|w:3")
        self.assertEqual(ui.state()["active_cell"], "A6")
        self.keys("k:Down|w:3")
        self.assertEqual(ui.state()["active_cell"], "A7", "End lasts for one arrow")

    def test_double_click_selection_edge_jumps_to_the_data_end(self):
        from yc import ui

        self.ws.Range("A1:A8").Value = [[1], [2], [3], [4], [5], [6], [7], [8]]
        self.keys("g:A2|w:2")
        r = ui.cell_rect("A2")
        x, y = (r[0] + r[2]) // 2, r[3] - 1  # the bottom edge of the selected cell
        self.keys(f"m:{x},{y}|w:3|D:{x},{y}|w:4")
        self.assertEqual(ui.state()["active_cell"], "A8")
        self.assertEqual(self.ws.Range("A2").Value, 2, "nothing moved")
        self.keys("m:2,2|w:2")

    def _tab(self, name):
        from yc import ui

        hits = [i for i in ui.items("") if i["id"] == name + "##sheettab"]
        self.assertTrue(hits, f"the tab {name!r}")
        r = hits[0]["rect"]
        return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)

    def test_sheet_tab_renamed_on_the_tab(self):
        # Excel: a double-click on the tab edits its name right there; Enter keeps it, Escape doesn't
        from yc import ui

        x, y = self._tab(self.ws.Name)
        self.keys(f"m:{x},{y}|w:2|D:{x},{y}|w:4")
        self.assertEqual(self.dialog(), 0, "no dialog")
        self.assertTrue([i for i in ui.items("") if i["id"].endswith("##tabname")], "the box on the tab")
        self.keys("k:c-A|t:Prices|k:Enter|w:4")
        self.assertEqual(self.ws.Name, "Prices")
        x, y = self._tab("Prices")
        self.keys(f"m:{x},{y}|w:2|D:{x},{y}|w:4|k:c-A|t:Nope|k:Escape|w:4")
        self.assertEqual(self.ws.Name, "Prices", "Escape keeps the old name")
        self.keys("m:2,2|w:2")

    def test_sheet_tab_ctrl_drag_copies(self):
        from yc import excel

        from yc import ui

        n = excel.Worksheets.Count
        self.ws.Range("A1").Value = "copied"
        # onto the leftmost tab on the screen (with many sheets the first ones are scrolled out of view)
        # (each tab's button is also listed by its bare id "##sheettab", at the same place: only the named ones count)
        tabs = sorted([i for i in ui.items("") if i["id"].endswith("##sheettab") and i["id"] not in ("##sheettab", self.ws.Name + "##sheettab")],
                      key=lambda i: i["rect"][0])
        self.assertTrue(tabs, "another tab on the screen")
        first = tabs[0]["id"][: -len("##sheettab")]
        x0, y0 = self._tab(self.ws.Name)
        x1, y1 = self._tab(first)
        # Ctrl held while dragging the tab onto the first one: a copy goes before it
        self.keys(f"m:{x0},{y0}|w:2|p:{x0},{y0}|w:2|H:ctrl|m:{x0 - 20},{y0}|w:2|m:{x1},{y1}|w:3|u:|w:2|h:|w:4|m:2,2|w:2")
        self.assertEqual(excel.Worksheets.Count, n + 1)
        at = [excel.Worksheets(k).Name for k in range(1, excel.Worksheets.Count + 1)].index(first)
        copy = excel.Worksheets(at)  # the copy went just before the tab it was dropped on
        self.assertEqual(copy.Range("A1").Value, "copied")
        self.assertNotEqual(copy.Name, self.ws.Name)

    def test_undo_list_undoes_several_actions(self):
        # Excel: the arrow beside Undo lists the actions; choosing the third undoes the last three
        from yc import ui

        self.keys("g:A1|w:2|t:1|k:Enter|t:2|k:Enter|t:3|k:Enter|w:4")
        self.assertEqual(self.ws.Range("A3").Value, 3)
        arrow = [i for i in ui.items("") if i["id"] == "##undo_hist"]
        self.assertTrue(arrow, "the arrow beside Undo")
        r = arrow[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        rows = [i for i in ui.items("") if i["label"] == 'Typing "1" in A1']
        self.assertTrue(rows, "the actions are named as in Excel: Typing \"1\" in A1")
        r = rows[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        self.assertEqual([self.ws.Range(a).Value for a in ("A1", "A2", "A3")], [None, None, None])
        self.keys("m:2,2|w:2")

    def test_typing_after_a_click_on_a_pane_stays_out_of_the_cells(self):
        # chaos 101: a pane over the sheet was clicked, then typing went into the active cell and Ctrl+Enter filled the
        # whole selection. Excel: a clicked task pane keeps the keyboard
        from yc import ui

        self.keys("g:B2:B5|w:2")
        ui.command("AlertRules")
        self.keys("w:6")
        w = [w for w in ui.windows() if "yc_alert_rules" in w["name"]]
        self.assertTrue(w, "the Alert Rules pane")
        r = w[0]["rect"]
        x, y = int(r[0] + (r[2] - r[0]) * 0.5), int(r[3] - 40)  # its empty lower part
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3|t:=1+1|k:c-Enter|w:4")
        self.assertEqual([self.ws.Range(a).Value for a in ("B2", "B3")], [None, None], "nothing typed into the cells")
        ui.command("AlertRules")  # closed again
        self.keys("w:3|m:2,2|w:2")
        # a click on the sheet gives the keyboard back
        self.keys("A:B2|w:2|t:7|k:Enter|w:3")
        self.assertEqual(self.ws.Range("B2").Value, 7)

    def test_a_chart_inserted_on_blank_cells_is_empty(self):
        # Excel: with a blank cell selected, Insert > Scatter makes an empty chart that asks for its data
        from yc import ui

        self.keys("g:D10|w:2")
        ui.command("InsertChartScatter")
        self.keys("w:6")
        self.assertEqual(self.ws.ChartObjects().Count, 1)
        self.assertEqual(self.ws.ChartObjects(1).Chart.SeriesCollection().Count, 0, "no series made of a blank cell")
        self.keys("k:Escape|w:2")

    def test_stock_chart_takes_the_first_of_five_columns_as_its_dates(self):
        # Date | Open | High | Low | Close with the dates as plain numbers: four price series, the first column labels them
        from yc import ui

        self.ws.Range("A1:E4").Value = [["Date", "Open", "High", "Low", "Close"], [45000, 10, 12, 9, 11], [45001, 11, 13, 10, 12], [45002, 12, 14, 11, 13]]
        self.keys("g:A1:E4|w:2")
        ui.command("InsertChartStock")
        self.keys("w:6")
        ch = self.ws.ChartObjects(1).Chart
        names = [ch.SeriesCollection(i).Name for i in range(1, ch.SeriesCollection().Count + 1)]
        self.assertEqual(names, ["Open", "High", "Low", "Close"])
        self.keys("k:Escape|w:2")

    def test_escape_on_a_chart_flyout_keeps_the_chart_selected(self):
        # Excel: Escape closes the + (Chart Elements) flyout; the chart stays selected, so Format Selection still works
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        self.keys("g:A1:C4|w:2")
        ui.command("InsertChartColumn")
        self.keys("w:6")
        fly = sorted([i for i in ui.items("") if i["id"] == "##fly"], key=lambda i: i["rect"][1])
        self.assertEqual(len(fly), 3, "the +, Chart Styles and filter buttons beside the chart")
        r = fly[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4|k:Escape|w:3")
        self.assertTrue(ui.command("ChartSelSeries"), "the chart is still selected")
        ui.command("ChartFormatSelection")
        self.keys("w:6")
        self.assertTrue([w for w in ui.windows() if "yc_props" in w["name"]], "the Format pane of the series")
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")

    def test_chartobject_select_selects_the_chart_on_the_sheet(self):
        from yc import ui

        self.ws.Range("A1:B3").Value = [["M", "Bid"], ["Jan", 1], ["Feb", 3]]
        self.ws.ChartObjects().Add(200, 50, 300, 200).Chart.SetSourceData(self.ws.Range("A1:B3"))
        self.keys("g:E10|w:3")
        self.ws.ChartObjects(1).Select()
        self.keys("w:3")
        ui.command("ChartFormatSelection")
        self.keys("w:6")
        self.assertTrue([w for w in ui.windows() if "yc_props" in w["name"]], "Select() selected it: its Format pane opens")
        self.keys("k:Escape|w:2|m:2,2|w:2")

    def test_series_pane_shows_what_the_chart_type_draws(self):
        # Excel: a pie slice has no line width or marker; a line series has both
        from yc import ui

        self.ws.Range("A1:B4").Value = [["M", "Bid"], ["Jan", 1], ["Feb", 3], ["Mar", 2]]
        for cmd, want_marker in (("InsertChartPie", False), ("InsertChartLine", True)):
            self.keys("g:A1:B4|w:2")
            ui.command(cmd)
            self.keys("w:6")
            ui.command("ChartSelSeries")
            ui.command("ChartFormatSelection")
            self.keys("w:6")
            labels = [i["label"] for i in ui.items("")]
            self.assertEqual("Marker" in labels, want_marker, cmd)
            self.assertEqual("Width (pt)" in labels, want_marker, cmd)
            self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
            self.ws.ChartObjects(1).Delete()
            self.keys("w:3")

    def test_series_options_as_excel_sliders_and_names(self):
        # Excel's Series Options: Gap Width / Series Overlap sliders for columns, Smoothed line for lines, first slice
        # angle for pies; the same through ChartGroups(1) and Series, kept when the file is saved and opened again
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "Bid", "Ask"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 2, 5]]
        self.keys("g:A1:C4|w:2")
        ui.command("InsertChartColumn")
        self.keys("w:6")
        ui.command("ChartSelSeries")
        ui.command("ChartFormatSelection")
        self.keys("w:6")
        labels = [i["label"] for i in ui.items("")]
        self.assertIn("Gap Width", labels)
        self.assertIn("Series Overlap", labels)
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        ch = self.ws.ChartObjects(1).Chart
        self.assertEqual((ch.ChartGroups(1).GapWidth, ch.ChartGroups(1).Overlap), (219, -27), "Excel's defaults")
        ch.ChartGroups(1).GapWidth = 50
        ch.ChartGroups(1).Overlap = 100
        ch.SeriesCollection(1).Smooth = True
        ch.ChartGroups(1).FirstSliceAngle = 45
        self.assertEqual((ch.ChartGroups(1).GapWidth, ch.ChartGroups(1).Overlap, ch.ChartGroups(1).FirstSliceAngle), (50, 100, 45))
        self.assertTrue(ch.SeriesCollection(1).Smooth)
        ch.SeriesCollection(1).ShowInnerPoints = True
        ch.SeriesCollection(1).ShowOutlierPoints = False
        self.assertEqual((ch.SeriesCollection(1).ShowInnerPoints, ch.SeriesCollection(1).ShowOutlierPoints, ch.SeriesCollection(1).ShowMeanMarkers),
                         (True, False, True))

    def test_pie_slices_go_clockwise_from_twelve_oclock(self):
        # Excel: the first slice starts at 12 o'clock and the slices follow clockwise (Jan just right of the top)
        import math
        import tempfile

        from yc import ui

        self.ws.Range("A1:B7").Value = [["M", "V"]] + [[m, 10] for m in ["Jan", "Feb", "Mar", "Apr", "May", "Jun"]]
        self.keys("g:A1:B7|w:2")
        ui.command("InsertChartPie")
        self.keys("w:4")
        ch = self.ws.ChartObjects(1).Chart
        ch.HasTitle = False
        ch.HasLegend = False
        self.keys("k:Escape|w:2|g:A20|w:6")
        obj = [i for i in ui.items("") if i["id"] == "##obj"]
        self.assertTrue(obj, "the chart on the screen")
        r = obj[-1]["rect"]
        png = os.path.join(tempfile.gettempdir(), "yc_pie_cw.png")
        ui.screenshot(png, max_width=4000)
        _, _, px = read_png(png)
        cx, cy, rad = (r[0] + r[2]) / 2, (r[1] + r[3]) / 2, min(r[2] - r[0], r[3] - r[1]) * 0.22

        def at(deg):  # degrees clockwise from 12 o'clock
            return px(int(cx + rad * math.sin(math.radians(deg))), int(cy - rad * math.cos(math.radians(deg))))

        jan, jun = at(30), at(330)
        self.assertNotEqual(jan, jun)
        self.assertLess(sum(abs(a - b) for a, b in zip(jan, (0x44, 0x72, 0xC4))), 60, f"Jan (the first color) right of the top: {jan}")


if __name__ == "__main__":
    unittest.main()
