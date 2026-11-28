# SPDX-License-Identifier: Apache-2.0
"""Cells and editing work like Excel, each checked the way a person does it.

Every test fails without its fix. Run: python -m unittest tests.test_parity_cells (with the bundled Python).
"""
from __future__ import annotations

import datetime as _dt
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


class _Base(unittest.TestCase):
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
        excel.Application.CutCopyMode = False
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:2")

    def tearDown(self):
        self.keys("k:Escape|w:2|m:2,2|w:2")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 15000}, timeout=30)
        time.sleep(0.25)

    def state(self):
        from yc import ui

        return ui.state()

    def rect(self, addr):
        from yc import ui

        return ui.cell_rect(addr)

    def center(self, addr):
        r = self.rect(addr)
        return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)

    def col_header(self, col):
        r = self.rect(f"{col}1")
        return int((r[0] + r[2]) / 2), int(r[1]) - 6

    def row_header(self, row):
        r = self.rect(f"A{row}")
        return int(r[0]) - 12, int((r[1] + r[3]) / 2)

    def click_label(self, label, window="", prefix=False):
        from yc import ui

        hits = [i for i in ui.items(window) if (i["label"].startswith(label) if prefix else i["label"] == label)]
        self.assertTrue(hits, f"{label!r} isn't on the screen")
        r = hits[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")


class SelectionKeysTest(_Base):
    def test_shift_arrow_after_a_column_header_grows_whole_columns(self):
        # H1: Shift+Right after clicking the C header jumped to row 1,048,576 and turned the columns into cells
        from yc import excel

        x, y = self.col_header("C")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4|k:s-Right|w:3")
        sel = self.state()["selection"]
        self.assertTrue(sel.startswith("C1:D") and int(sel[4:]) > 60000, sel)
        self.assertEqual(excel.Application.ActiveWindow.ScrollRow, 1, "the view stays at the top")
        self.keys("k:s-Down|w:3")  # the other direction does nothing
        self.assertEqual(self.state()["selection"], sel)
        x, y = self.row_header(5)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4|k:s-Down|w:3")
        sel = self.state()["selection"]
        self.assertTrue(sel.startswith("A5:") and sel.endswith("6"), sel)
        self.assertEqual(excel.Application.ActiveWindow.ScrollColumn, 1)

    def test_shift_space_then_shift_down_selects_two_rows(self):
        # H1 / H2: Shift+Space typed a space into the cell; Shift+Down after it turned the rows into two cells
        self.ws.Range("B2").Value = "keep"
        self.keys("g:B5|w:2|k:s-Space|w:3")
        st = self.state()
        self.assertNotIn("edit", st, "Shift+Space doesn't type a space")
        self.keys("k:s-Down|w:3")
        sel = self.state()["selection"]
        self.assertTrue(sel.startswith("A5:") and sel.endswith("6") and len(sel) > 6, sel)
        self.keys("g:B2|w:2|k:s-Space|w:3|k:c-s-Equal|w:6")  # then Ctrl++ inserts a row
        self.assertNotIn("edit", self.state())
        self.assertEqual(self.ws.Range("B3").Value, "keep")

    def test_a_whole_column_typed_in_the_name_box_is_deleted_without_a_dialog(self):
        # M13: C:C from Go To / the Name Box acted like cells: Ctrl+- opened the Delete dialog
        self.ws.Range("D1").Value = "d"
        self.keys("g:C:C|w:2|k:c-Minus|w:6")
        self.assertEqual(self.state()["dialog"], 0)
        self.assertEqual(self.ws.Range("C1").Value, "d")

    def test_merged_cells_are_one_cell_for_the_keys(self):
        # M2: Right from merged A1:C1 went to B1 (inside the merge); Enter and Tab cycled inside it
        self.ws.Range("A1:C1").Merge()
        self.keys("g:A1|w:2|k:Right|w:2")
        self.assertEqual(self.state()["active_cell"], "D1")
        self.keys("g:A1|w:2|k:Enter|w:2")
        self.assertEqual(self.state()["active_cell"], "A2")
        self.keys("g:A1|w:2|k:Tab|w:2")
        self.assertEqual(self.state()["active_cell"], "D1")
        self.keys("g:D1|w:2|k:Left|w:2")
        self.assertEqual(self.state()["active_cell"], "A1")

    def test_enter_walks_through_every_area(self):
        # L10: Enter cycled inside the last area only; the other areas were never reached
        from yc import excel

        self.keys("g:A1:A2,C1:C2|w:2")
        start = self.state()["active_cell"]
        self.keys("k:Enter|w:2|k:Enter|w:2")
        self.assertNotEqual(self.state()["active_cell"][0], start[0], "on to the other area")
        self.assertEqual(excel.Selection.Areas.Count, 2)

    def test_ctrl_click_on_a_selected_cell_takes_it_out(self):
        # L4: Ctrl+click on a selected cell selected it a second time
        from yc import excel

        self.keys("g:A1:C3|w:2|H:ctrl|A:B2|w:3|h:|w:2|m:2,2|w:2")
        areas = excel.Selection.Address.replace("$", "").split(",")
        self.assertEqual(sorted(areas), ["A1:C1", "A2", "A3:C3", "C2"])

    def test_end_then_home_goes_to_the_last_cell(self):
        # L2: End, Home moved to column A and left End mode on
        self.ws.Range("E9").Value = 1
        self.keys("g:B2|w:2|k:End|w:2|k:Home|w:3")
        self.assertEqual(self.state()["active_cell"], "E9")

    def test_alt_semicolon_selects_the_visible_cells(self):
        # L2: Alt+; wasn't a key
        from yc import excel

        self.ws.Range("A1:A5").Value = [[1], [2], [3], [4], [5]]
        self.ws.Range("A2:A3").EntireRow.Hidden = True
        self.keys("g:A1:A5|w:2|k:a-Semicolon|w:3")
        self.assertEqual(sorted(excel.Selection.Address.split(",")), ["$A$1", "$A$4:$A$5"])

    def test_shift_f5_opens_find(self):
        from yc import ui

        self.keys("g:A1|w:2|k:s-F5|w:4")
        self.assertTrue(any("yc_find" in w["name"] for w in ui.windows()))
        self.keys("k:Escape|w:3")

    def test_ctrl_shift_space_selects_the_data_first(self):
        # L2: Ctrl+Shift+Space selected the whole sheet at once (Excel: as Ctrl+A)
        self.ws.Range("B2:C3").Value = [[1, 2], [3, 4]]
        self.keys("g:B2|w:2|k:c-s-Space|w:3")
        self.assertEqual(self.state()["selection"], "B2:C3")


class EditingTest(_Base):
    def test_the_editor_keeps_working_after_its_cell_scrolls_away(self):
        # H3: with the edited cell scrolled out of view, Enter, arrows and typing were lost
        self.keys("g:A1|w:2|t:=|w:2")
        self.keys("|".join(["k:Down|w:1"] * 60) + "|w:2|k:Enter|w:4")
        self.assertNotIn("edit", self.state())
        self.assertEqual(self.ws.Range("A1").Formula, "=A61")
        x, y = self.center("C5")
        self.keys(f"g:A1|w:2|t:5|w:2|m:{x},{y}|w:2|W:-40|w:4|t:6|k:Enter|w:4")
        self.assertNotIn("edit", self.state())
        self.assertEqual(self.ws.Range("A1").Value, 56)

    def test_an_arrow_that_ends_an_entry_moves_like_an_arrow(self):
        # M1: Right after typing started Tab's chain (Enter then went back to column A) and kept multi-cell selections
        self.keys("g:A1|w:2|t:1|k:Right|t:2|k:Enter|w:3")
        self.assertEqual(self.state()["active_cell"], "B2")
        self.keys("g:A1:C3|w:2|t:x|k:Up|w:3")
        self.assertEqual(self.state()["selection"], "A1")
        self.assertEqual(self.ws.Range("A1").Value, "x")

    def test_a_double_click_on_the_selection_edge_doesnt_edit(self):
        # M3: the jump also opened the editor on the cell it landed on
        self.ws.Range("A1:A8").Value = [[i] for i in range(1, 9)]
        self.keys("g:A2|w:2")
        r = self.rect("A2")
        x, y = int((r[0] + r[2]) / 2), int(r[3]) - 1
        self.keys(f"m:{x},{y}|w:3|D:{x},{y}|w:4")
        st = self.state()
        self.assertEqual(st["active_cell"], "A8")
        self.assertNotIn("edit", st)

    def test_a_double_click_puts_the_caret_where_it_was_clicked(self):
        # M8: the caret always went to the end
        self.ws.Range("B2").Value = "abcdef"
        self.ws.Range("B2").ColumnWidth = 20
        r = self.rect("B2")
        cy = int((r[1] + r[3]) / 2)
        from yc import ui

        # where the text starts and how wide it is: from the cell editor's own measure, the middle of "abcdef"
        x = int(r[0]) + 3 + 20
        self.keys(f"m:{x},{cy}|w:3|D:{x},{cy}|w:6|t:X|w:2|k:Enter|w:4")
        v = self.ws.Range("B2").Value
        self.assertNotEqual(v, "abcdefX")
        self.assertIn(v, ("abXcdef", "abcXdef", "abcdXef"))
        del ui

    def test_ctrl_semicolon_types_the_date_while_editing(self):
        # M9: Ctrl+; did nothing while typing in a cell
        self.keys("g:A1|w:2|t:x |w:2|k:c-Semicolon|w:2|k:Enter|w:4")
        v = self.ws.Range("A1").Value
        d = _dt.date.today()
        self.assertEqual(v, f"x {d.year}/{d.month}/{d.day}")

    def test_enter_moves_the_way_the_options_say(self):
        # M10: Enter always moved down; Application.MoveAfterReturn(Direction) didn't exist
        from yc import excel

        app = excel.Application
        try:
            app.MoveAfterReturnDirection = excel.xlToRight
            self.assertEqual(app.MoveAfterReturnDirection, excel.xlToRight)
            self.keys("g:A1|w:2|t:1|k:Enter|w:3")
            self.assertEqual(self.state()["active_cell"], "B1")
            self.keys("g:A5|w:2|k:Enter|w:3")  # Ready mode too
            self.assertEqual(self.state()["active_cell"], "B5")
            app.MoveAfterReturn = False
            self.keys("g:A1|w:2|t:2|k:Enter|w:3")
            self.assertEqual(self.state()["active_cell"], "A1")
            self.assertEqual(self.ws.Range("A1").Value, 2)
            app.EnableAutoComplete = False
            self.ws.Range("D1").Value = "apple"
            self.keys("g:D2|w:2|t:a|w:3|t:p|w:3|k:Tab|w:3")
            self.assertEqual(self.ws.Range("D2").Value, "ap", "no AutoComplete")
            app.EditDirectlyInCell = False
            self.keys("g:D1|w:2|k:F2|w:3")
            self.assertTrue(self.state()["edit"]["formula_bar"])
            self.keys("k:Escape|w:2")
        finally:
            app.MoveAfterReturn = True
            app.MoveAfterReturnDirection = excel.xlDown
            app.EnableAutoComplete = True
            app.EditDirectlyInCell = True


class ClipboardTest(_Base):
    def test_escape_ends_cut_mode(self):
        # H6: after Ctrl+X and Esc (or typing in a cell), Ctrl+V still moved the cut cells
        from yc import excel

        self.ws.Range("A1").Value = 5
        self.keys("g:A1|w:2|k:c-X|w:2")
        self.assertEqual(excel.Application.CutCopyMode, excel.xlCut)
        self.keys("k:Escape|w:2|g:C1|w:2|k:c-V|w:4")
        self.assertEqual(self.ws.Range("A1").Value, 5)
        self.assertIsNone(self.ws.Range("C1").Value)
        self.assertFalse(excel.Application.CutCopyMode)
        self.keys("g:A1|w:2|k:c-X|w:2|g:B1|w:2|t:x|k:Enter|w:3|g:C1|w:2|k:c-V|w:4")
        self.assertEqual(self.ws.Range("A1").Value, 5)
        self.assertIsNone(self.ws.Range("C1").Value)
        self.keys("g:A1|w:2|k:c-C|w:2")
        self.assertEqual(excel.Application.CutCopyMode, excel.xlCopy)
        excel.Application.CutCopyMode = False
        self.assertFalse(self.state()["copy_mode"])

    def test_a_copy_keeps_its_size_past_the_data(self):
        # M7: A1:D10 copied with data in A1:B5 pasted a 5 x 2 block
        from yc import excel

        self.ws.Range("A1:B5").Value = [[i, i * 10] for i in range(1, 6)]
        other = excel.Worksheets.Add()
        other.Range("A1:D10").Value = [["old"] * 4 for _ in range(10)]
        src, dst = self.ws.Name, other.Name
        self.keys(f"g:'{src}'!A1:D10|w:2|k:c-C|w:2|g:'{dst}'!A1|w:2|k:c-V|w:5")
        self.assertIsNone(other.Range("C1").Value)
        self.assertIsNone(other.Range("A6").Value)
        self.assertEqual(other.Range("B2").Value, 20)
        self.assertEqual(self.state()["selection"], "A1:D10")

    def test_pasted_text_clears_cells_for_empty_fields(self):
        # M6: tab-separated text from the Clipboard pane kept the old value under an empty field
        from yc import ui

        self.ws.Range("E1:G1").Value = [["a", None, "b"]]
        self.keys("g:E1:G1|w:2|k:c-C|w:2|k:Escape|w:2")
        self.ws.Range("A1:C1").Value = [[1, 2, 3]]
        ui.pane("clipboard", True)
        self.keys("w:3|g:A1|w:2")
        try:
            self.click_label("a  b", "Clipboard", prefix=True)
        finally:
            ui.pane("clipboard", False)
        self.assertEqual(self.ws.Range("A1").Value, "a")
        self.assertIsNone(self.ws.Range("B1").Value)
        self.assertEqual(self.ws.Range("C1").Value, "b")

    def test_copy_two_areas_in_the_same_rows(self):
        # L5: any multiple selection was refused
        self.ws.Range("A1:A3").Value = [[1], [2], [3]]
        self.ws.Range("C1:C3").Value = [[7], [8], [9]]
        self.keys("g:A1:A3,C1:C3|w:2|k:c-C|w:2|g:E1|w:2|k:c-V|w:5")
        self.assertEqual([self.ws.Range(f"E{i}").Value for i in (1, 2, 3)], [1, 2, 3])
        self.assertEqual([self.ws.Range(f"F{i}").Value for i in (1, 2, 3)], [7, 8, 9])

    def test_paste_special_formulas_and_number_formats(self):
        # L11: no Formulas and number formats; Paste Special was offered (and pasted a copy) after Cut
        from yc import excel

        self.ws.Range("A1").Value = 3
        self.ws.Range("B1").Formula = "=A1*2"
        self.ws.Range("B1").NumberFormat = "0.00%"
        self.ws.Range("B1").Interior.Color = 0x00FFFF
        self.keys("g:B1|w:2|k:c-C|w:2|g:B2|w:2|k:c-a-V|w:6")
        self.assertTrue(self.state()["dialog"])
        self.keys("k:R|w:2|k:Enter|w:5")
        self.assertEqual(self.ws.Range("B2").Formula, "=A2*2")
        self.assertEqual(self.ws.Range("B2").NumberFormat, "0.00%")
        self.assertNotEqual(self.ws.Range("B2").Interior.Color, 0x00FFFF)
        self.keys("g:B1|w:2|k:c-X|w:2|g:D1|w:2|k:c-a-V|w:6")
        self.assertFalse(self.state()["dialog"], "no Paste Special for cut cells")
        excel.Application.CutCopyMode = False

    def test_insert_cut_cells_moves_a_column(self):
        # M5: there was no Insert Cut Cells
        self.ws.Range("A1:C1").Value = [["a", "b", "c"]]
        x, y = self.col_header("C")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3|k:c-X|w:3")
        x, y = self.col_header("A")
        self.keys(f"m:{x},{y}|w:2|r:|w:4")
        self.click_label("Insert Cut Cells")
        self.keys("w:4")
        self.assertEqual([self.ws.Range(f"{c}1").Value for c in "ABCD"], ["c", "a", "b", None])

    def test_filtered_rows_are_left_alone(self):
        # H4: Delete, Copy and Ctrl+Enter changed the rows a filter hides too
        self.ws.Range("A1:A6").Value = [["h"], [1], [2], [3], [4], [5]]
        self.ws.Range("A1:A6").AutoFilter(1, ["1", "3", "4", "5"], 7)
        self.assertTrue(self.ws.Range("A3").EntireRow.Hidden)
        self.keys("g:A2:A6|w:2|k:c-C|w:2|g:C1|w:2|k:c-V|w:5")
        self.assertEqual([self.ws.Range(f"C{i}").Value for i in range(1, 5)], [1, 3, 4, 5])
        self.keys("k:Escape|w:2|g:B2:B6|w:2|t:x|k:c-Enter|w:4")
        self.assertEqual(self.ws.Range("B2").Value, "x")
        self.assertIsNone(self.ws.Range("B3").Value)
        self.keys("g:A2:A6|w:2|k:Delete|w:4")
        self.ws.AutoFilterMode = False
        self.assertEqual(self.ws.Range("A3").Value, 2)
        self.assertIsNone(self.ws.Range("A2").Value)


class CommandsTest(_Base):
    def test_autofit_row_height_is_one_step(self):
        # H5: AutoFit Row Height made one undo step per row (and froze on the whole sheet)
        for i in range(1, 6):
            self.ws.Range(f"A{i}").Value = "Tall"
        self.ws.Range("A1:A5").Font.Size = 20
        self.ws.Range("A1:A5").RowHeight = 10
        before = [self.ws.Range(f"A{i}").RowHeight for i in range(1, 6)]
        depth = self.state()["undo"]["depth"]
        self.keys("g:A1:A5|w:2|x:AutoFitRows|w:6")
        after = [self.ws.Range(f"A{i}").RowHeight for i in range(1, 6)]
        self.assertTrue(all(a > b for a, b in zip(after, before)), (before, after))
        self.assertEqual(self.state()["undo"]["depth"], depth + 1)
        self.keys("k:c-Z|w:6")
        self.assertEqual([self.ws.Range(f"A{i}").RowHeight for i in range(1, 6)], before)
        self.keys("g:A1|w:2|k:c-a|w:2|k:c-a|w:2")
        t0 = time.time()
        self.keys("x:AutoFitRows|w:4")
        st = self.state()
        self.assertLess(time.time() - t0, 3.0)
        self.assertEqual(st["undo"]["label"], "AutoFit Row Height")

    def test_small_text_fits_below_the_standard_height(self):
        # L6: AutoFit Row Height never went below 20 px
        self.ws.Range("A3").Value = "tiny"
        self.ws.Range("A3").Font.Size = 8
        self.ws.Range("A3").RowHeight = 30
        self.keys("g:A3|w:2|x:AutoFitRows|w:5")
        self.assertLess(self.ws.Range("A3").RowHeight, 15)

    def test_inserted_row_takes_the_format_above(self):
        # M4: inserted rows were plain inside a formatted block
        self.ws.Range("A2:C2").Interior.Color = 0x00FFFF  # yellow (BGR)
        self.keys("g:A3|w:2|k:s-Space|w:2|k:c-s-Equal|w:5")
        self.assertEqual(self.ws.Range("B3").Interior.Color, 0x00FFFF)
        # the Insert Options button beside them: Clear Formatting redoes the insert plain
        from yc import ui

        btn = [i for i in ui.items("") if i["id"] == "##insbtn"]
        self.assertTrue(btn, "the Insert Options button is beside the inserted row")
        r = btn[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        self.click_label("Clear Formatting")
        self.keys("w:4")
        self.assertNotEqual(self.ws.Range("B3").Interior.Color, 0x00FFFF)
        self.assertEqual(self.ws.Range("B2").Interior.Color, 0x00FFFF)
        from yc import excel

        self.ws.Range("A10").Interior.Color = 0x00FF00
        self.ws.Range("A10").EntireRow.Insert(CopyOrigin=excel.xlFormatFromRightOrBelow)
        self.assertEqual(self.ws.Range("A10").Interior.Color, 0x00FF00)

    def test_ctrl_y_repeats_when_nothing_to_redo(self):
        # L3: Ctrl+Y only redid
        self.keys("g:A1|w:2|x:Bold|w:3|g:B1|w:2|k:c-Y|w:4")
        self.assertTrue(self.ws.Range("B1").Font.Bold)

    def test_fill_handle_steps_a_time_by_hours_and_quarters(self):
        # M12: 9:00 filled down stayed 9:00 (a day later); Q3 went on to Q5
        self.ws.Range("A1").Value = "9:00"
        self.ws.Range("C1").Value = "Q3"
        for col in ("A", "C"):
            self.keys(f"g:{col}1|w:2")
            r = self.rect(f"{col}1")
            hx, hy = int(r[2]) - 1, int(r[3]) - 1
            t = self.rect(f"{col}4")
            self.keys(f"m:{hx},{hy}|w:6|d:{hx},{hy},{int((t[0] + t[2]) / 2)},{int((t[1] + t[3]) / 2)}|w:8")
        self.assertEqual([self.ws.Range(f"A{i}").Text for i in (2, 3)], ["10:00", "11:00"])
        self.assertEqual([self.ws.Range(f"C{i}").Value for i in (2, 3, 4)], ["Q4", "Q1", "Q2"])
        self.keys("m:2,2|w:2")

    def test_replace_reads_the_wildcards_find_reads(self):
        # M11: Replace compared plainly, so a*c changed nothing and ~ had no meaning
        self.ws.Range("B1").Value = "abc"
        self.ws.Range("B2").Value = "a*c"
        self.ws.Range("B1:B2").Replace("a~*c", "Y")
        self.assertEqual([self.ws.Range("B1").Value, self.ws.Range("B2").Value], ["abc", "Y"])
        self.ws.Range("B1:B2").Replace("a*c", "X")
        self.assertEqual(self.ws.Range("B1").Value, "X")

    def test_go_to_special_same_conditional_formats_and_current_array(self):
        # L12: Go To Special had no Current array and no All / Same for conditional formats
        from yc import excel

        self.ws.Range("A1:A3").FormatConditions.Add(1, 5, "=0")
        self.ws.Range("B1:B3").FormatConditions.Add(1, 5, "=1")
        self.keys("g:A1|w:2|x:GoToSpecial|w:6")
        self.click_label("Conditional formats")
        self.click_label("Same")
        self.keys("k:Enter|w:5")
        self.assertEqual(excel.Selection.Address, "$A$1:$A$3")
        self.ws.Range("D1:D3").FormulaArray = "=ROW(D1:D3)"
        self.keys("g:D2|w:2|x:GoToSpecial|w:6")
        self.click_label("Current array")
        self.keys("k:Enter|w:5")
        self.assertEqual(excel.Selection.Address, "$D$1:$D$3")

    def test_find_all_lists_value_formula_and_name(self):
        # L14: Find All showed one of Formula / Value and no Name; the headers didn't sort
        from yc import excel, ui

        self.ws.Range("C2").Formula = "=40+2"
        excel.ActiveWorkbook.Names.Add("Answer", "=" + self.ws.Name + "!$C$2")
        self.keys("g:A1|w:2|k:c-F|w:6|k:c-A|t:40+|w:2")
        self.click_label("Find All")
        labels = [i["label"] for i in ui.items("Find and Replace")]
        try:
            for h in ("Name", "Value", "Formula"):
                self.assertIn(h, labels)
        finally:
            self.keys("k:Escape|w:3")

    def test_a_find_all_row_is_found_by_its_cell_and_a_click_goes_there(self):
        # the rows were listed by their sheet's name only (each also a tab's), and a clicked row wasn't highlighted
        from yc import excel, ui

        self.ws.Range("B2").Value = "needle one"
        self.ws.Range("D5").Value = "needle two"
        self.keys("g:A1|w:2|k:c-F|w:6|k:c-A|t:needle|w:2")
        self.click_label("Find All")
        try:
            row = [i for i in ui.items("Find and Replace") if i.get("label") == self.ws.Name + "!$D$5"]
            self.assertTrue(row, "the second row is listed by its sheet and cell")
            r = row[0]["rect"]
            x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
            self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
            self.assertEqual(excel.ActiveCell.Address, "$D$5")
        finally:
            self.keys("k:Escape|w:3")

    def test_the_editor_of_a_right_aligned_cell_grows_left(self):
        # L7: the editor always started at the cell's left edge
        from yc import ui

        self.ws.Range("D2").HorizontalAlignment = -4152  # xlRight
        self.keys("g:D2|w:2|t:a long text that overflows the cell|w:4")
        try:
            box = [i for i in ui.items("") if i["id"] == "##celledit"]
            self.assertTrue(box, "the cell editor is on the screen")
            self.assertLess(box[0]["rect"][0], self.rect("D2")[0] - 5)
        finally:
            self.keys("k:Escape|w:3")

    def test_select_formulas_from_find_and_select(self):
        # L15: Find & Select had no Formulas, Notes, Constants... entries
        from yc import excel

        self.ws.Range("A1").Formula = "=1+1"
        self.ws.Range("C3").Formula = "=2+2"
        self.ws.Range("B2").Value = 5
        self.keys("g:A1|w:2|x:SelectFormulas|w:3")
        self.assertEqual(sorted(excel.Selection.Address.split(",")), ["$A$1", "$C$3"])
        self.keys("g:A1|w:2|x:SelectConstants|w:3")
        self.assertEqual(excel.Selection.Address, "$B$2")


if __name__ == "__main__":
    unittest.main()
