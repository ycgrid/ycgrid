# SPDX-License-Identifier: Apache-2.0
"""Round 3 (cells): cells and sheets by mouse and keyboard exactly as in Excel: Shift-drops
that insert, the right-drag menus of the selection's border and the fill handle, Ctrl on the fill handle, Escape
cancelling a drag, whole columns dragged, grouped sheets, the Activate list, the status bar's view buttons, the cell
menu's Filter and Pick From Drop-down List, the keys Ctrl+. Ctrl+2/3/4/6/8 Ctrl+\\ F6 Scroll Lock Ctrl+Y, Insert on
every area, formula range boxes picking at the caret, and the Python members behind them.

Every test fails without its fix. Run: python -m unittest tests.test_round3_cells (with the bundled Python).
"""
from __future__ import annotations

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

        self.keys("h:|k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        excel.Application.CutCopyMode = False
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:3")

    def tearDown(self):
        from yc import ui

        self.keys("h:|U:|u:|k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")
        self.assertEqual(ui.state().get("imgui_asserts", 0), 0, "an ImGui check failed (a debug build stops there)")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 15000}, timeout=30)
        time.sleep(0.2)

    def state(self):
        from yc import ui

        return ui.state()

    def rect(self, addr):
        from yc import ui

        return ui.cell_rect(addr)

    def center(self, addr):
        r = self.rect(addr)
        return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)

    def items(self, label=None, ident=None, window="", prefix=False):
        from yc import ui

        hits = [i for i in ui.items(window) if (label is None or (i["label"].startswith(label) if prefix else i["label"] == label))
                and (ident is None or i["id"] == ident)]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def click_at(self, x, y):
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:6")

    def click(self, label=None, ident=None, window="", prefix=False):
        hits = self.items(label, ident, window, prefix)
        self.assertTrue(hits, f"{label or ident!r} isn't on the screen")
        r = hits[0]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def col_header(self, col):
        r = self.rect(f"{col}1")
        return int((r[0] + r[2]) / 2), int(r[1]) - 6

    def act(self):
        return self.state()["active_cell"].replace("$", "")

    def selection(self):
        return self.state()["selection"].replace("$", "")


class DragTest(_Base):
    def test_shift_drag_of_the_border_inserts_between_rows(self):
        # backlog: a Shift-drop overwrote the cells instead of inserting there
        self.ws.Range("A1:A4").Value = [["a"], ["b"], ["c"], ["d"]]
        self.keys("g:A1|w:2")
        r, t = self.rect("A1"), self.rect("A4")
        x, y = int(r[0]) + 1, int((r[1] + r[3]) / 2)
        mx, ty = int((t[0] + t[2]) / 2), int(t[1]) + 2  # just under the A3 | A4 boundary, mid-column
        self.keys(f"m:{x},{y}|w:4|H:shift|p:{x},{y}|w:2|m:{x + 4},{y + 6}|w:2|m:{mx},{ty}|w:3|m:{mx},{ty}|w:3|u:|w:2|h:|w:5")
        self.assertEqual([self.ws.Range(f"A{i}").Value for i in range(1, 5)], ["b", "c", "a", "d"])
        self.assertEqual(self.selection(), "A3")

    def test_escape_cancels_a_drag_of_the_border(self):
        # backlog: Escape didn't cancel the drag; the cells moved when the button went up
        self.ws.Range("B2").Value = "stay"
        self.keys("g:B2|w:2")
        r, t = self.rect("B2"), self.rect("B6")
        x, y = int(r[0]) + 1, int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:4|p:{x},{y}|w:2|m:{x},{y + 8}|w:2|m:{x},{int((t[1] + t[3]) / 2)}|w:3|k:Escape|w:2|u:|w:5")
        self.assertEqual(self.ws.Range("B2").Value, "stay")
        self.assertIsNone(self.ws.Range("B6").Value)

    def test_right_drag_of_the_border_copies_values_only(self):
        # backlog: no right-drag menu on the border (Move Here, Copy Here as Values Only...)
        self.ws.Range("A1").Formula = "=20+22"
        self.keys("g:A1|w:2")
        r, t = self.rect("A1"), self.rect("C1")
        x, y = int(r[0]) + 1, int((r[1] + r[3]) / 2)
        tx = int((t[0] + t[2]) / 2)
        self.keys(f"m:{x},{y}|w:4|P:{x},{y}|w:2|m:{x + 10},{y}|w:2|m:{tx},{y}|w:3|U:|w:6")
        self.assertTrue(self.items("Copy Here as Values Only"), [i["label"] for i in self.items()][:30])
        self.click("Copy Here as Values Only")
        self.assertEqual(self.ws.Range("C1").Value, 42)
        self.assertFalse(self.ws.Range("C1").HasFormula)
        self.assertEqual(self.ws.Range("A1").Formula, "=20+22")

    def test_right_drag_of_the_fill_handle_fills_a_series(self):
        # backlog: no right-drag menu on the fill handle
        self.ws.Range("A1").Value = 1
        self.keys("g:A1|w:2")
        r, t = self.rect("A1"), self.rect("A4")
        hx, hy = int(r[2]) - 1, int(r[3]) - 1
        self.keys(f"m:{hx},{hy}|w:4|P:{hx},{hy}|w:2|m:{hx},{hy + 8}|w:2|m:{hx},{int(t[3]) - 4}|w:3|U:|w:6")
        self.click("Fill Series")
        self.assertEqual([self.ws.Range(f"A{i}").Value for i in range(1, 5)], [1, 2, 3, 4])

    def test_ctrl_on_the_fill_handle_makes_a_series_of_one_number(self):
        # backlog: no Ctrl toggle on the fill handle (one number is copied; Ctrl makes 1, 2, 3)
        self.ws.Range("A1").Value = 7
        self.keys("g:A1|w:2")
        r, t = self.rect("A1"), self.rect("A3")
        hx, hy = int(r[2]) - 1, int(r[3]) - 1
        self.keys(f"m:{hx},{hy}|w:4|H:ctrl|p:{hx},{hy}|w:2|m:{hx},{hy + 8}|w:2|m:{hx},{int(t[3]) - 4}|w:3|u:|w:2|h:|w:5")
        self.assertEqual([self.ws.Range(f"A{i}").Value for i in range(1, 4)], [7, 8, 9])

    def test_shift_drag_of_the_fill_handle_inserts_cells(self):
        # L9: Shift on the fill handle inserts the dragged band of cells
        self.ws.Range("A1:A3").Value = [[1], [2], [3]]
        self.keys("g:A1|w:2")
        r, t = self.rect("A1"), self.rect("A3")
        hx, hy = int(r[2]) - 1, int(r[3]) - 1
        self.keys(f"m:{hx},{hy}|w:4|H:shift|p:{hx},{hy}|w:2|m:{hx},{hy + 8}|w:2|m:{hx},{int(t[3]) - 4}|w:3|u:|w:2|h:|w:5")
        self.assertEqual([self.ws.Range(f"A{i}").Value for i in range(1, 6)], [1, None, None, 2, 3])

    def test_whole_column_dragged_with_shift_goes_between_columns(self):
        # L9: whole columns couldn't be dragged; Shift puts them in between
        self.ws.Range("A1:D1").Value = [["a", "b", "c", "d"]]
        x, y = self.col_header("B")
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:4")
        self.assertIn(self.selection(), ("B:B", "B1:B1048576"))
        b, e = self.rect("B2"), self.rect("E2")
        bx, by = int(b[0]) + 1, int((b[1] + b[3]) / 2)
        ex = int(e[0]) + 2
        self.keys(f"m:{bx},{by}|w:4|H:shift|p:{bx},{by}|w:2|m:{bx + 10},{by}|w:2|m:{ex},{by}|w:3|m:{ex},{by}|w:3|u:|w:2|h:|w:5")
        self.assertEqual([self.ws.Range(f"{c}1").Value for c in "ABCD"], ["a", "c", "d", "b"])


class GroupTest(_Base):
    def test_ctrl_shift_pgdn_groups_and_typing_goes_to_both(self):
        # backlog: no sheet grouping
        from yc import excel

        other = excel.Worksheets.Add(After=self.ws)
        self.ws.Activate()
        self.keys("w:3|k:c-s-PageDown|w:4")
        self.assertEqual(excel.ActiveSheet.Name, other.Name)
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 2)
        self.keys("g:B2|w:2|t:both|k:Enter|w:4")
        self.assertEqual(self.ws.Range("B2").Value, "both")
        self.assertEqual(other.Range("B2").Value, "both")
        # a sheet outside the group shown ends it (Excel)
        excel.Worksheets(1).Activate()
        self.keys("w:3")
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 1)

    def test_ctrl_click_tabs_then_format_goes_to_both_and_select_all_sheets(self):
        from yc import excel

        other = excel.Worksheets.Add(After=self.ws)
        self.ws.Activate()
        self.keys("w:3")
        tab = [i for i in self.items() if i["id"] == f"{other.Name}##sheettab"]
        self.assertTrue(tab)
        r = tab[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|H:ctrl|c:{x},{y}|w:3|h:|w:3")
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 2)
        self.keys("g:A1|w:2|k:c-B|w:4")
        self.assertTrue(self.ws.Range("A1").Font.Bold)
        self.assertTrue(other.Range("A1").Font.Bold)
        # the tab menu: Ungroup Sheets, then Select All Sheets
        self.keys(f"m:{x},{y}|w:3|r:|w:4")
        self.click("Ungroup Sheets")
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 1)
        self.keys(f"m:{x},{y}|w:3|r:|w:4")
        self.click("Select All Sheets")
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, excel.Worksheets.Count)
        excel.ActiveSheet.Select()
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 1)

    def test_grouped_sheets_gray_out_tables_sorting_and_filters(self):
        # Excel 16 (measured 2026-11-16, GetEnabledMso with two sheets selected) grays out Insert Table, Format as
        # Table, Sort, Filter, Conditional Formatting, the Insert tab's objects and more; Ctrl+T made a table on the
        # active sheet only (basics seq268426)
        from yc import excel, ui

        other = excel.Worksheets.Add(After=self.ws)
        self.ws.Activate()
        self.ws.Range("A1:B3").Value = [["h1", "h2"], [1, 2], [3, 4]]
        self.keys("g:A1|w:2|k:c-s-PageDown|w:3|k:c-PageUp|w:4")
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 2)
        state = {c["id"]: c.get("enabled") for c in ui.commands()}
        for cid in ("InsertTable", "FormatAsTable", "ToggleFilter", "SortAscending", "ConditionalFormatting", "InsertChartColumn",
                    "DataValidation", "TracePrecedents", "ProtectSheet"):
            self.assertIs(state.get(cid), False, cid)
        for cid in ("MergeCenter", "Bold", "InsertCells", "AutoSum"):
            self.assertIs(state.get(cid), True, cid)
        self.keys("g:A1|w:2|k:c-T|w:8|k:Enter|w:4|k:c-s-L|w:4")
        self.assertEqual(self.ws.ListObjects.Count, 0)
        self.assertFalse(self.ws.AutoFilterMode)
        # ungrouped, the commands come back
        self.ws.Select()
        self.keys("w:4")
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 1)
        self.assertIs({c["id"]: c.get("enabled") for c in ui.commands()}.get("InsertTable"), True)
        self.keys("g:A1|w:2|k:c-T|w:8|k:Enter|w:4")
        self.assertEqual(self.ws.ListObjects.Count, 1)

    def test_python_groups_and_fills_across(self):
        from yc import excel

        other = excel.Worksheets.Add(After=self.ws)
        self.ws.Range("C3").Value = 5
        self.ws.Range("C3").Font.Italic = True
        excel.Sheets([self.ws.Name, other.Name]).FillAcrossSheets(self.ws.Range("C3"))
        self.assertEqual(other.Range("C3").Value, 5)
        self.assertTrue(other.Range("C3").Font.Italic)
        excel.Sheets([self.ws.Name, other.Name]).Select()
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 2)
        other.Select(False)
        self.ws.Select()
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 1)

    def test_activate_list_from_the_tab_arrows(self):
        # backlog: no Activate list (right-click on the tab scroll arrows)
        from yc import excel

        target = excel.Worksheets(1).Name
        arrow = self.items(ident="##tabscroll_left")
        self.assertTrue(arrow)
        r = arrow[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|r:|w:6")
        self.assertTrue(self.items(target, window="Activate"))
        self.click(target, window="Activate")
        self.click("OK")
        self.assertEqual(excel.ActiveSheet.Name, target)


class KeysTest(_Base):
    def test_ctrl_period_walks_the_corners(self):
        self.keys("g:B2:D4|w:2|k:c-.|w:2")
        self.assertEqual(self.act(), "D2")
        self.keys("k:c-.|w:2")
        self.assertEqual(self.act(), "D4")
        self.assertEqual(self.selection(), "B2:D4")

    def test_ctrl_2_3_4_bold_italic_underline(self):
        self.keys("g:A1|w:2|k:c-2|w:2|k:c-3|w:2|k:c-4|w:3")
        f = self.ws.Range("A1").Font
        self.assertTrue(f.Bold)
        self.assertTrue(f.Italic)
        self.assertTrue(f.Underline not in (False, None, -4142, 0))

    def test_ctrl_6_hides_objects_and_ctrl_8_outline_symbols(self):
        from yc import excel

        self.keys("g:A1|w:2|k:c-6|w:3")
        self.assertEqual(excel.ActiveWorkbook.DisplayDrawingObjects, excel.xlHide)
        self.keys("k:c-6|w:3")
        self.assertEqual(excel.ActiveWorkbook.DisplayDrawingObjects, excel.xlDisplayShapes)
        self.ws.Rows("2:3").Group()
        self.keys("k:c-8|w:3")
        self.assertFalse(excel.ActiveWindow.DisplayOutline)
        self.keys("k:c-8|w:3")
        self.assertTrue(excel.ActiveWindow.DisplayOutline)

    def test_ctrl_backslash_selects_row_differences(self):
        self.ws.Range("A1:D1").Value = [[1, 1, 2, 1]]
        self.keys("g:A1:D1|w:2|k:c-\\|w:3")
        self.assertEqual(self.selection(), "C1")

    def test_ctrl_y_never_repeats_typing(self):
        # L3 made F4 / Ctrl+Y type the last entry again ("Repeat Typing"), a rule written without a measurement: Excel 16
        # never repeats an entry (measured 2026-11-11), and that one typed a cell's old value over the next cell
        self.keys("g:A1|w:2|t:again|k:Enter|w:3|g:C5|w:2|k:c-Y|w:4")
        self.assertIsNone(self.ws.Range("C5").Value)

    def test_scroll_lock_arrows_scroll_and_leave_the_cell(self):
        # L2: Scroll Lock
        from yc import excel

        self.keys("g:B2|w:2|k:ScrollLock|w:2|k:DownArrow|w:1|k:DownArrow|w:1|k:DownArrow|w:3")
        try:
            self.assertEqual(self.act(), "B2")
            self.assertGreaterEqual(excel.ActiveWindow.ScrollRow, 3)
            self.assertTrue(self.items("Scroll Lock"))
        finally:
            self.keys("k:ScrollLock|w:2")
        self.keys("k:DownArrow|w:2")
        self.assertEqual(self.act(), "B3")

    def test_f6_moves_between_split_panes(self):
        # L2: F6 / Shift+F6
        from yc import excel

        self.keys("g:E10|w:2")
        excel.ActiveWindow.SplitRow = 5
        excel.ActiveWindow.SplitColumn = 2
        excel.ActiveWindow.Split = True
        try:
            self.keys("g:E10|w:2|k:F6|w:3")
            first = self.act()
            self.assertNotEqual(first, "E10")
            self.keys("k:s-F6|w:3")
            self.assertNotEqual(self.act(), first)
        finally:
            excel.ActiveWindow.Split = False

    def test_enter_walks_whole_rows_and_keeps_them(self):
        # L10: whole rows collapsed to one cell on Enter
        self.ws.Range("A2:B3").Value = [[1, 2], [3, 4]]
        self.keys("g:2:3|w:2|k:Enter|w:2")
        self.assertEqual(self.act(), "A3")
        self.keys("k:Enter|w:2")
        self.assertEqual(self.act(), "B2")
        self.assertIn(self.selection(), ("2:3", "A2:XFD3"))

    def test_insert_on_every_area(self):
        # backlog: Insert acted on one area of a multiple selection
        self.ws.Range("A1:A6").Value = [[1], [2], [3], [4], [5], [6]]
        self.keys("g:2:2,5:5|w:2|k:c-s-Equal|w:4")
        self.assertEqual([self.ws.Range(f"A{i}").Value for i in range(1, 9)], [1, None, 2, 3, 4, None, 5, 6])


class MenuTest(_Base):
    def test_status_bar_view_buttons(self):
        # backlog: no view buttons on the status bar
        from yc import excel

        self.click(ident="Page Layout##statusview")
        self.keys("w:4")
        self.assertEqual(excel.ActiveWindow.View, excel.xlPageLayoutView)
        self.click(ident="Page Break Preview##statusview")
        self.assertEqual(excel.ActiveWindow.View, excel.xlPageBreakPreview)
        self.click(ident="Normal##statusview")
        self.assertEqual(excel.ActiveWindow.View, excel.xlNormalView)

    def test_filter_by_selected_cells_value(self):
        # backlog: the cell menu had no Filter > Filter by Selected Cell's Value
        self.ws.Range("A1:A5").Value = [["h"], ["x"], ["y"], ["x"], ["z"]]
        self.keys("R:A2|w:4")
        self.click("Filter")
        self.keys("w:3")
        self.click("Filter by Selected Cell's Value")
        self.keys("w:4")
        self.assertTrue(self.ws.AutoFilterMode)
        self.assertTrue(self.ws.Range("A3").EntireRow.Hidden)
        self.assertTrue(self.ws.Range("A5").EntireRow.Hidden)
        self.assertFalse(self.ws.Range("A4").EntireRow.Hidden)

    def test_pick_from_drop_down_list(self):
        # backlog: the cell menu had no Pick From Drop-down List
        self.ws.Range("A1:A3").Value = [["pear"], ["apple"], ["fig"]]
        self.keys("R:A4|w:4")
        self.click("Pick From Drop-down List...")
        self.keys("w:4")
        self.click("fig")
        self.keys("w:3")
        self.assertEqual(self.ws.Range("A4").Value, "fig")

    def test_find_and_select_select_objects_and_fill_justify(self):
        # L15: Select Objects; Fill > Justify
        from yc import excel

        self.ws.Range("A1").Value = "one two three four five six"
        self.ws.Range("A1").ColumnWidth = 9
        self.ws.Range("A1:A1").Justify()
        self.assertEqual(self.ws.Range("A1").Value, "one two")
        self.assertEqual(self.ws.Range("A2").Value, "three")
        self.keys("x:SelectObjects|w:3")
        try:
            self.assertTrue(self.state().get("ribbon") is not None)
        finally:
            self.keys("k:Escape|w:2")
        del excel

    def test_range_box_picks_at_the_caret(self):
        # cross-cutting: a formula range box replaced its whole text unless it ended with an operator
        self.keys("x:DefineName|w:6")
        names = [i for i in self.items() if i["id"].endswith("##rb")]
        self.assertTrue(names, [i["id"] for i in self.items()][:40])
        box = names[-1]["rect"]
        bx, by = int(box[0]) + 20, int((box[1] + box[3]) / 2)
        self.keys(f"m:{bx},{by}|w:2|c:{bx},{by}|w:3|k:c-A|t:=SUM(A1)|w:2|k:LeftArrow|w:2")
        # the box's own button: the nearest control right of it on its line (the sheet's scroll bar is on that line too)
        pick = sorted((i for i in self.items() if abs((i["rect"][1] + i["rect"][3]) / 2 - by) < 4 and i["rect"][0] >= box[2] - 2),
                      key=lambda i: i["rect"][0])
        self.assertTrue(pick)
        pr = pick[0]["rect"]
        self.click_at((pr[0] + pr[2]) / 2, (pr[1] + pr[3]) / 2)
        self.keys("w:4|g:C3|w:4|k:Enter|w:6")
        nb = self.items(ident="##nm")
        self.assertTrue(nb)
        r = nb[0]["rect"]
        self.keys(f"m:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:2|c:{int(r[0]) + 10},{int((r[1] + r[3]) / 2)}|w:2|k:c-A|t:picked|w:2")
        self.click("OK")
        from yc import excel

        ref = excel.ActiveWorkbook.Names("picked").RefersTo
        self.assertTrue(ref.startswith("=SUM(") and ref.endswith("!$C$3)"), ref)


class PythonTest(_Base):
    def test_range_insert_while_cut_inserts_the_cut_cells(self):
        # M5: Range.Insert during CutCopyMode inserted plain cells
        self.ws.Range("A1:C1").Value = [["a", "b", "c"]]
        self.keys("g:C:C|w:2|k:c-X|w:3")
        self.ws.Range("A:A").Insert()
        self.keys("w:3")
        self.assertEqual([self.ws.Range(f"{c}1").Value for c in "ABCD"], ["c", "a", "b", None])

    def test_linked_picture_paste(self):
        from yc import excel

        self.ws.Range("A1:B2").Value = [[1, 2], [3, 4]]
        self.keys("g:A1:B2|w:2|k:c-C|w:3|g:E5|w:2")
        n = self.ws.Shapes.Count
        self.ws.Pictures.Paste(Link=True)
        self.keys("w:4")
        self.assertEqual(self.ws.Shapes.Count, n + 1)
        del excel


class IconFilterTest(_Base):
    def _arrows(self):
        from yc import excel

        self.ws.Range("A1").Value = "n"
        self.ws.Range("A2:A10").Value = [[i * 10] for i in range(1, 10)]
        fc = self.ws.Range("A2:A10").FormatConditions.AddIconSetCondition()
        fc.IconSet = excel.ActiveWorkbook.IconSets(excel.xl3Arrows)
        self.keys("w:3")

    def test_python_filter_by_cell_icon(self):
        from yc import excel

        self._arrows()
        self.ws.Range("A1:A10").AutoFilter(1, excel.ActiveWorkbook.IconSets(excel.xl3Arrows).Item(3), excel.xlFilterIcon)
        self.assertTrue(self.ws.Range("A2").EntireRow.Hidden)
        self.assertFalse(self.ws.Range("A10").EntireRow.Hidden)
        f = self.ws.AutoFilter.Filters[0]
        self.assertEqual((f["kind"], f["icon_set"], f["icon"]), (7, excel.xl3Arrows, 2))

    def test_cell_menu_filter_by_selected_cells_icon(self):
        # the cell menu's Filter > Filter by Selected Cell's Icon
        self._arrows()
        self.keys("R:A2|w:4")
        self.click("Filter")
        self.keys("w:3")
        self.click("Filter by Selected Cell's Icon")
        self.keys("w:4")
        self.assertTrue(self.ws.AutoFilterMode)
        self.assertFalse(self.ws.Range("A2").EntireRow.Hidden)
        self.assertTrue(self.ws.Range("A10").EntireRow.Hidden)

    def test_filter_button_offers_the_columns_icons(self):
        # the drop-down's Filter by Color > Filter by Cell Icon lists the icons the column shows
        self._arrows()
        self.ws.Range("A1:A10").AutoFilter()
        self.keys("w:3")
        r = self.rect("A1")
        bx, by = int(r[2]) - 8, int((r[1] + r[3]) / 2)
        self.keys(f"m:{bx},{by}|w:3|c:{bx},{by}|w:5")
        self.click("Filter by Color")
        self.keys("w:4")
        self.assertTrue(self.items(ident="##filtericon1_2"), [i["id"] for i in self.items()][-20:])
        self.click(ident="##filtericon1_2")
        self.keys("w:4")
        self.assertTrue(self.ws.Range("A2").EntireRow.Hidden)
        self.assertFalse(self.ws.Range("A10").EntireRow.Hidden)


class GroupSavedTest(_Base):
    def test_grouped_sheets_come_back_when_the_file_opens(self):
        import tempfile

        from yc import excel

        other = excel.Worksheets.Add(After=self.ws)
        excel.Sheets([self.ws.Name, other.Name]).Select()
        self.assertEqual(excel.ActiveWindow.SelectedSheets.Count, 2)
        path = os.path.join(tempfile.mkdtemp(), "grouped.ycg")
        excel.ActiveWorkbook.SaveAs(path)
        names = sorted([self.ws.Name, other.Name])
        # the file opened in another YCGrid: the same sheets are grouped and the title says so
        p2 = yc.launch(path, hidden=True, exe=EXE, lang="en")
        try:
            time.sleep(0.5)
            sel = excel.ActiveWindow.SelectedSheets
            self.assertEqual(sorted(s.Name for s in sel), names)
            self.assertIn("[Group]", self.state().get("title", ""))
        finally:
            p2.terminate()
            p2.wait(10)
            type(self).conn = yc.connect(target=self.proc.pid)
        excel.ActiveSheet.Select()

if __name__ == "__main__":
    unittest.main()
