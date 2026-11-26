# SPDX-License-Identifier: Apache-2.0
"""Excel's habits where people work every day: keys, KeyTips, menus, the ribbon and File, each checked the way a person
does it (docs/excel-parity-backlog.md, 2026-11-03). Every test fails without its fix.

Run: python -m unittest tests.test_parity_habits (with the bundled Python).
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


class HabitsBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        cls.tmp = tempfile.mkdtemp(prefix="yc_parity_habits_")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("g:A1|w:3")

    def tearDown(self):
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def keytips(self, letters, before=""):
        """Alt, then each letter as a person types it (Alt, H, O, I)."""
        steps = "|".join(f"t:{ch}|w:4" for ch in letters)
        self.keys(f"{before}m:2,2|w:2|k:LeftAlt|w:3|{steps}|w:4")

    def dialog(self):
        from yc import ui

        return ui.state().get("dialog", 0)

    def visible(self, cmd):
        from yc import ui

        return any(i["cmd"] == cmd for i in ui.ribbon_items())


class HiddenInstanceTest(HabitsBase):
    def test_ribbon_shown_whatever_the_users_setting(self):
        # a hidden instance read the user's "Show tabs only": every test clicking a ribbon button failed
        from yc import ui

        self.assertFalse(ui.state()["ribbon"]["collapsed"])
        self.assertTrue([i for i in ui.ribbon_items() if i["cmd"] == "Bold"], "Home's buttons are shown")

    def test_vscode_commands_open_no_vscode_window(self):
        # every test that ran Developer > VS Code left a VS Code window open; one VS Code process holds all its windows,
        # so they couldn't be closed without the user's: a hidden instance makes the project and opens no window
        from yc import ui

        ui.command("OpenVSCode")
        self.keys("w:10")
        asked = ui.state()["vscode_request"]
        self.assertTrue(asked.endswith("main.py"), f"the project's main.py would open ({asked!r})")
        self.assertTrue(os.path.isfile(asked), "the project was made")

    def design_mode(self, on):
        from yc import ui

        if self.conn.request("ui.design_mode_get", {}, timeout=10)["on"] != on:
            ui.command("DesignMode")
            self.keys("w:3")
        self.assertEqual(self.conn.request("ui.design_mode_get", {}, timeout=10)["on"], on)

    def vscode_place(self):
        """What VS Code would open, split into the file, the line and the column."""
        from yc import ui

        asked = ui.state()["vscode_request"]
        path, line, col = asked.rsplit(":", 2)
        return path, int(line), int(col)

    def test_double_click_in_design_mode_opens_the_controls_event_procedure(self):
        # Excel's double-click on an ActiveX control in Design Mode opens the code window at CommandButton1_Click,
        # made empty when it isn't there yet; it opened Properties here
        from yc import ui

        btn = self.ws.OLEObjects.Add("Forms.CommandButton.1", Left=100, Top=40, Width=90, Height=26)
        combo = self.ws.OLEObjects.Add("Forms.ComboBox.1", Left=100, Top=90, Width=90, Height=20)
        self.design_mode(True)
        try:
            self.keys("w:4")
            objs = [i["rect"] for i in ui.items("Workbook") if "##obj" in i["id"]]
            self.assertEqual(len(objs), 2, "both controls are drawn")
            r = min(objs, key=lambda q: q[1])  # the button, above the combo box
            x, y = (r[0] + r[2]) // 2, (r[1] + r[3]) // 2
            self.keys(f"m:{x},{y}|w:3|D:{x},{y}|w:10|m:2,2|w:2")
            path, line, col = self.vscode_place()
            self.assertEqual(os.path.basename(path), f"{self.ws.CodeName}.py", "the sheet's module")
            with open(path, encoding="utf-8") as f:
                lines = f.read().split("\n")
            self.assertEqual(lines[line - 2], f"def {btn.Name}_Click():", "the button's default event, made")
            self.assertEqual((lines[line - 1], col), ("    pass", 5), "the caret in its body")
            self.assertFalse(any("Properties" in w["label"] for w in ui.windows()), "a double-click opens the code, not Properties")

            # once more: the same procedure, not a second one
            self.keys(f"m:{x},{y}|w:3|D:{x},{y}|w:10|m:2,2|w:2")
            self.assertEqual(self.vscode_place()[1], line)
            with open(path, encoding="utf-8") as f:
                self.assertEqual(f.read().count(f"def {btn.Name}_Click("), 1)

            # Developer > View Code with the combo box selected: its default event is Change
            self.conn.request("shape.select", {"sheet": self.ws._id, "name": combo.Name}, timeout=10)
            ui.command("ViewCode")
            self.keys("w:6")
            path2, line2, _ = self.vscode_place()
            self.assertEqual(path2, path)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            self.assertEqual(text.split("\n")[line2 - 2], f"def {combo.Name}_Change():")
        finally:
            self.design_mode(False)

        # what the user writes there runs when the button is clicked outside Design Mode
        body = '    import yc.excel as xl\n    xl.ActiveSheet.Range("C1").Value = "clicked"\n'
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace(f"def {btn.Name}_Click():\n    pass\n", f"def {btn.Name}_Click():\n{body}"))
        end = time.time() + 30
        while time.time() < end and self.ws.Range("C1").Value != "clicked":
            self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:10|m:2,2|w:2")
            time.sleep(0.5)  # the host loads the saved module again
        self.assertEqual(self.ws.Range("C1").Value, "clicked")


class KeyTipsTest(HabitsBase):
    def test_alt_h_l_opens_conditional_formatting(self):
        # Excel's fixed KeyTips named ids the ribbon doesn't use (CondFormatMenu): Alt, H, L got two other letters
        self.keytips("HL")
        self.assertTrue(self.visible("CfHighlightRules"), "Alt, H, L opens Conditional Formatting")

    def test_alt_n_c_opens_the_column_chart_menu(self):
        self.ws.Range("A1:B3").Value = [["x", "y"], [1, 2], [3, 4]]
        self.keytips("NC", "g:A1:B3|w:2|")
        self.assertTrue(self.visible("InsertChartColumn"), "Alt, N, C opens Insert Column or Bar Chart")

    def test_alt_h_o_i_autofits_the_column(self):
        # the Format menu's entries had their label's letters: AutoFit Column Width was U, not Excel's I
        self.ws.Range("B2").Value = "a long piece of text that needs a wide column"
        self.ws.Columns("B").ColumnWidth = 4
        self.keytips("HOI", "g:B2|w:2|")
        self.assertGreater(self.ws.Columns("B").ColumnWidth, 20)

    def test_alt_h_o_u_c_hides_the_column(self):
        # a submenu had no KeyTip: Hide & Unhide couldn't be reached from the keyboard
        self.keytips("HOUC", "g:C1|w:2|")
        self.assertTrue(self.ws.Columns("C").Hidden)
        self.keytips("HOUL", "g:B1:D1|w:2|")
        self.assertFalse(self.ws.Columns("C").Hidden)

    def test_alt_h_v_v_pastes_values(self):
        self.ws.Range("A1").Formula = "=1+1"
        self.keys("g:A1|w:2|k:c-C|w:3")
        self.keytips("HVV", "g:D1|w:2|")
        self.assertEqual(self.ws.Range("D1").Value, 2)
        self.assertFalse(self.ws.Range("D1").HasFormula)

    def test_excel_2003_menu_keys_still_work(self):
        """Alt, E, S, V (Paste Special, Values) and Alt, O, C, A did nothing: Excel still takes the 2003 menu keys after
        Alt when no tab has the letter, and people who learned them type them without looking."""
        from yc import ui

        self.ws.Range("A1").Formula = "=1+1"
        self.keys("g:A1|w:2|k:c-C|w:3")
        self.keytips("ES", "g:D1|w:2|")
        self.assertTrue(self.dialog(), "Alt, E, S opens Paste Special")
        self.keys("k:V|w:3|k:Enter|w:6")
        self.assertEqual(self.ws.Range("D1").Value, 2)
        self.assertFalse(self.ws.Range("D1").HasFormula)
        self.keys("k:Escape|w:3")
        self.ws.Range("B2").Value = "a long piece of text that needs a wide column"
        self.ws.Columns("B").ColumnWidth = 4
        self.keytips("OCA", "g:B2|w:2|")
        self.assertGreater(self.ws.Columns("B").ColumnWidth, 20)
        self.keys("m:2,2|w:2|k:LeftAlt|w:3|t:E|w:4")  # half a sequence: Excel's hint, then Esc leaves it
        self.assertEqual(ui.state()["ribbon"].get("legacy_keys"), "E")
        self.keys("k:Escape|w:4")
        self.assertFalse(ui.state()["ribbon"].get("legacy_keys"))
        self.assertEqual(ui.state()["ribbon"]["keytips"], 0)

    def test_alt_in_a_dialog_is_not_the_ribbons(self):
        """Alt in a modal dialog lit the ribbon's KeyTips behind it, and the letters typed next went to them: Alt, H
        then V in Paste Special pasted from Home's Paste menu under the dialog."""
        from yc import ui

        self.keys("g:A1|w:2|k:c-C|w:3|g:D1|w:2|k:c-a-V|w:8")
        d = self.dialog()
        self.assertTrue(d, "Paste Special")
        self.keys("k:LeftAlt|w:4|t:H|w:4")
        self.assertEqual(ui.state()["ribbon"]["keytips"], 0)
        self.assertEqual(self.dialog(), d)
        self.keys("k:Escape|w:4")

    def test_alt_h_b_a_draws_all_borders(self):
        from yc import excel

        self.keytips("HBA", "g:B2:C3|w:2|")
        none = excel.xlLineStyleNone
        self.assertNotEqual(self.ws.Range("B2").Borders(excel.xlEdgeRight).LineStyle, none, "a line inside too")
        self.assertNotEqual(self.ws.Range("C3").Borders(excel.xlEdgeBottom).LineStyle, none)

    def test_alt_a_s_a_sorts_and_alt_m_x_m_sets_manual(self):
        from yc import excel

        self.ws.Range("A1:A3").Value = [[3], [1], [2]]
        self.keytips("ASA", "g:A1:A3|w:2|")
        self.assertEqual([self.ws.Range(f"A{i}").Value for i in (1, 2, 3)], [1, 2, 3])
        try:
            self.keytips("MXM")
            self.assertEqual(excel.Application.Calculation, excel.xlCalculationManual)
        finally:
            excel.Application.Calculation = excel.xlCalculationAutomatic


class ShortcutsTest(HabitsBase):
    def test_f12_and_alt_f2_ask_where_to_save(self):
        # F12 was shown next to Save As but did nothing
        from yc import ui

        for key in ("F12", "a-F2"):
            ui.clear_errors()
            self.keys(f"k:Escape|w:3|k:{key}|w:6")
            self.assertIn("Save As", [e["title"] for e in ui.errors()], key)

    def test_ctrl_f12_opens_and_ctrl_shift_f12_prints(self):
        from yc import ui

        ui.clear_errors()
        self.keys("k:c-F12|w:6")
        self.assertIn("Open", [e["title"] for e in ui.errors()])
        self.keys("k:c-s-F12|w:6")
        self.assertTrue(ui.state()["ribbon"]["backstage"], "Ctrl+Shift+F12 shows File > Print")
        self.keys("k:Escape|w:4")

    def test_ctrl_f1_collapses_and_pins_the_ribbon(self):
        """Ctrl+F1 switches the ribbon between tabs only and always shown, as in Excel (two handlers of the key each
        toggled it in one frame, and it never changed)."""
        from yc import ui

        before = bool(ui.state()["ribbon"].get("collapsed"))
        try:
            self.keys("k:c-F1|w:6")
            self.assertEqual(bool(ui.state()["ribbon"].get("collapsed")), not before, "Ctrl+F1 once")
            self.keys("k:c-F1|w:6")
            self.assertEqual(bool(ui.state()["ribbon"].get("collapsed")), before, "Ctrl+F1 twice")
        finally:
            if bool(ui.state()["ribbon"].get("collapsed")) != before:
                self.keys("k:c-F1|w:6")

    def test_shift_f12_saves(self):
        from yc import excel, ui

        path = os.path.join(self.tmp, "saved.ycg")
        excel.ActiveWorkbook.SaveAs(path)
        self.ws.Range("A1").Value = 5
        self.keys("w:4")
        self.assertTrue(ui.state()["dirty"])
        for key in ("s-F12", "a-s-F2"):
            self.ws.Range("A1").Value = 6
            self.keys(f"w:4|k:{key}|w:8")
            self.assertFalse(ui.state()["dirty"], key)

    def test_alt_shift_f1_inserts_a_sheet(self):
        from yc import excel

        n = excel.Worksheets.Count
        self.keys("k:a-s-F1|w:6")
        self.assertEqual(excel.Worksheets.Count, n + 1)

    def test_ctrl_shift_t_toggles_the_total_row(self):
        self.ws.Range("A1:B3").Value = [["Name", "Qty"], ["a", 1], ["b", 2]]
        lo = self.ws.ListObjects.Add(1, self.ws.Range("A1:B3"))
        self.keys("g:A2|w:3|k:c-s-T|w:6")
        self.assertTrue(lo.ShowTotals)
        self.keys("g:A2|w:3|k:c-s-T|w:6")
        self.assertFalse(lo.ShowTotals)

    def test_alt_apostrophe_opens_the_style_dialog(self):
        self.keys("g:B2|w:2|k:a-Apostrophe|w:6")
        self.assertTrue(self.dialog(), "Alt+' opens Style")
        self.keys("k:Escape|w:4")
        self.assertFalse(self.dialog())


class ProtectedSheetRefusalTest(HabitsBase):
    """A change refused by a protected sheet opens no message box over the work (kept on purpose, unlike Excel): the
    whole message goes to the Error Log as an error and a red line beside its status bar entry names the cells."""

    def refused(self, keys):
        from yc import ui

        ui.clear_errors()
        self.keys(keys)
        self.assertFalse(self.dialog(), f"{keys}: no message box")
        errs = [e for e in ui.errors() if "protected sheet" in e.get("text", "")]
        self.assertTrue(errs, f"{keys}: the refusal is in the Error Log")
        self.assertTrue(errs[-1]["error"], "an error: the entry's badge turns red")
        self.assertNotIn("answered automatically", errs[-1]["text"], "no box was answered for the user")
        return ui.state()["error_log"]

    def test_typing_into_a_locked_cell_is_said_on_the_status_bar(self):
        sheet = self.ws.Name
        self.ws.Range("B2").Value = 5
        self.ws.Protect()
        try:
            log = self.refused("g:B2|w:3|t:9|w:6")
            self.assertEqual(log["refusal"], f"Protected sheet, not changed: {sheet}!B2")
            self.assertGreaterEqual(log["errors"], 1)
            self.assertEqual(self.ws.Range("B2").Value, 5)
            log = self.refused("k:Escape|w:2|g:B2|w:3|k:Delete|w:8")  # refused by the engine, said by the UI
            self.assertTrue(log["refusal"].startswith("Protected sheet, not changed:"), log["refusal"])
            self.assertEqual(self.ws.Range("B2").Value, 5)
        finally:
            self.ws.Unprotect()


class WindowKeysTest(HabitsBase):
    def test_ctrl_f6_and_ctrl_tab_switch_windows(self):
        # Ctrl+Tab was ImGui's pane switcher and moved the active cell; Ctrl+F6 did nothing
        from yc import ui

        self.keys("g:C3|w:2")
        ui.command("NewWindow")
        self.keys("w:4")
        time.sleep(0.8)  # Switch Windows' list follows the windows twice a second
        start = ui.state()["focus_window"]
        self.keys("k:c-F6|w:6")
        after = ui.state()["focus_window"]
        self.assertNotEqual(after, start, "Ctrl+F6 goes to the other window")
        self.keys("k:c-s-F6|w:6")
        self.assertEqual(ui.state()["focus_window"], start, "Ctrl+Shift+F6 comes back")
        cell = ui.state()["active_cell"]
        self.keys("k:c-Tab|w:6")
        self.assertEqual(ui.state()["focus_window"], after, "Ctrl+Tab goes to the other window")
        self.keys("k:c-s-Tab|w:6")
        self.assertEqual(ui.state()["focus_window"], start)
        self.assertEqual(ui.state()["active_cell"], cell, "the active cell stays")


def read_png(path):
    """(width, height, pixel(x, y) -> (r, g, b)) of an 8-bit RGB / RGBA PNG (the screenshots), standard library only."""
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


class MenusTest(HabitsBase):
    def click_label(self, label, nth=0):
        from yc import ui

        hits = sorted((i for i in ui.items() if i["label"] == label), key=lambda i: (i["rect"][1], i["rect"][0]))
        self.assertGreater(len(hits), nth, f"{label!r} isn't on the screen")
        r = hits[nth]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def hover_label(self, label):
        from yc import ui

        r = next(i["rect"] for i in ui.items() if i["label"] == label)
        self.keys(f"m:{int(r[0]) + 20},{int((r[1] + r[3]) / 2)}|w:8")

    def test_right_click_shows_the_mini_toolbar(self):
        # Excel's mini toolbar over the cell menu: its Bold makes the cell bold and the menu goes away
        from yc import ui

        self.ws.Range("B2").Value = "x"
        self.keys("R:B2|w:6")
        bold = [i for i in ui.ribbon_items() if i["where"] == "minitoolbar" and i["cmd"] == "Bold"]
        self.assertTrue(bold, "the mini toolbar shows Bold")
        r = bold[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        self.assertTrue(self.ws.Range("B2").Font.Bold)
        self.assertFalse(ui.state()["ribbon"]["popup_open"], "the menu closed")
        labels = [i["label"] for i in ui.items()]
        self.assertNotIn("Merge Cells", labels, "Excel's cell menu has no merge entries")

    def test_keyboard_menu_has_no_mini_toolbar(self):
        from yc import ui

        self.keys("g:B2|w:2|k:s-F10|w:6")
        self.assertTrue(ui.state()["ribbon"]["popup_open"])
        self.assertFalse([i for i in ui.ribbon_items() if i["where"] == "minitoolbar"], "Shift+F10: the menu only (Excel)")

    def test_cell_menu_paste_options_and_paste_special(self):
        self.ws.Range("A1").Formula = "=1+1"
        self.ws.Range("A1").NumberFormat = "0.00"
        self.keys("g:A1|w:2|k:c-C|w:3|R:D1|w:6")
        self.click_label("Values")  # the picture row under "Paste Options:"
        self.assertEqual(self.ws.Range("D1").Value, 2)
        self.assertFalse(self.ws.Range("D1").HasFormula)
        self.keys("g:A1|w:2|k:c-C|w:3|R:D3|w:6")
        self.hover_label("Paste Special")
        self.click_label("Values & Number Formatting")
        self.assertEqual(self.ws.Range("D3").Value, 2)
        self.assertEqual(self.ws.Range("D3").NumberFormat, "0.00")

    def test_home_paste_gallery_keeps_the_column_width(self):
        # Alt, H, V, W: Keep Source Column Widths, a tile of Home > Paste's pictures
        self.ws.Range("A1").Value = 1
        self.ws.Columns("A").ColumnWidth = 30
        self.keys("g:A1|w:2|k:c-C|w:3")
        self.keytips("HVW", "g:C1|w:2|")
        self.assertEqual(self.ws.Range("C1").Value, 1)
        self.assertAlmostEqual(self.ws.Columns("C").ColumnWidth, 30, delta=0.6)

    def test_put_selected_cell_icon_on_top(self):
        from yc import excel

        r = self.ws.Range("A1:A4")
        r.Value = [[1], [5], [9], [3]]
        r.FormatConditions.AddIconSetCondition()
        self.keys("R:A3|w:6")
        self.hover_label("Sort")
        self.click_label("Put Selected Cell Icon On Top")
        self.assertEqual(self.ws.Range("A1").Value, 9)
        del excel

    def test_sheet_tab_menu_tab_color_and_view_code(self):
        from yc import ui

        tab = next(i for i in ui.items() if i["label"] == self.ws.Name)
        x, y = int((tab["rect"][0] + tab["rect"][2]) / 2), int((tab["rect"][1] + tab["rect"][3]) / 2)
        self.keys(f"m:{x},{y}|w:3|r:|w:6")
        self.assertIn("View Code", [i["label"] for i in ui.items()])
        self.hover_label("Tab Color")
        self.click_label("Red")
        self.assertEqual(self.ws.Tab.Color & 0xFFFFFF, 0x0000FF, "Excel's Red (BGR)")


class QuickAnalysisTest(HabitsBase):
    def test_button_at_the_corner_inserts_a_chart(self):
        from yc import ui

        self.ws.Range("A1:B4").Value = [["n", "v"], ["a", 1], ["b", 3], ["c", 2]]
        self.keys("g:A1:B4|w:6")
        button = [i for i in ui.items() if i["label"] == "Quick Analysis"]
        self.assertTrue(button, "the Quick Analysis button at the selection's corner")
        r = button[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        hits = [i for i in ui.items() if i["label"] == "Charts"]
        self.assertTrue(hits)
        c = hits[0]["rect"]
        self.keys(f"m:{int((c[0] + c[2]) / 2)},{int((c[1] + c[3]) / 2)}|w:3|c:{int((c[0] + c[2]) / 2)},{int((c[1] + c[3]) / 2)}|w:6")
        n = self.ws.ChartObjects().Count
        t = next(i["rect"] for i in ui.items() if i["label"] == "Clustered Column")
        self.keys(f"m:{int((t[0] + t[2]) / 2)},{int((t[1] + t[3]) / 2)}|w:3|c:{int((t[0] + t[2]) / 2)},{int((t[1] + t[3]) / 2)}|w:8")
        self.assertEqual(self.ws.ChartObjects().Count, n + 1)

    def test_ctrl_q_adds_data_bars(self):
        from yc import ui

        self.ws.Range("C1:C4").Value = [[1], [2], [3], [4]]
        self.keys("g:C1:C4|w:4|k:c-Q|w:6")
        bars = [i for i in ui.items() if i["label"] == "Data Bars"]
        self.assertTrue(bars, "Ctrl+Q opens Quick Analysis on Formatting")
        r = bars[0]["rect"]
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:3|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:6")
        self.assertEqual(self.ws.Range("C1:C4").FormatConditions.Count, 1)

    def test_no_button_without_data(self):
        from yc import ui

        self.keys("g:H1:I4|w:6")
        self.assertFalse([i for i in ui.items() if i["label"] == "Quick Analysis"], "blank cells: no Quick Analysis")


class ViewAndStatusTest(HabitsBase):
    def test_focus_cell_tints_the_row(self):
        from yc import ui

        shot = os.path.join(self.tmp, "focus.png")
        self.keys("g:B3|w:4")
        r = ui.cell_rect("F3")
        px, py = int(r[0]) + 6, int((r[1] + r[3]) / 2)
        ui.screenshot(shot, max_width=8000)  # full size: the pixels are the window's
        before = read_png(shot)[2](px, py)
        ui.command("FocusCell")
        try:
            self.keys("w:4")
            ui.screenshot(shot, max_width=8000)
            after = read_png(shot)[2](px, py)
            self.assertNotEqual(before, after, "the active cell's row is tinted")
            far = read_png(shot)[2](int(ui.cell_rect("F6")[0]) + 6, int((ui.cell_rect("F6")[1] + ui.cell_rect("F6")[3]) / 2))
            self.assertEqual(far, before, "other rows aren't")
        finally:
            ui.command("FocusCell")

    def test_status_bar_hides_the_zoom_slider(self):
        from yc import ui

        def slider():
            return [i for i in ui.items("") if i["id"] == "##zoom_slider"]

        self.assertTrue(slider())
        pct = [i for i in ui.items("") if i["id"] == "##zoom_pct"][0]["rect"]
        x, y = int(pct[0] / 2), int((pct[1] + pct[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|r:|w:4")
        MenusTest.click_label(self, "Zoom Slider")
        try:
            self.assertFalse(slider(), "Zoom Slider unchecked: no slider")
        finally:
            self.keys(f"m:{x},{y}|w:2|r:|w:4")
            MenusTest.click_label(self, "Zoom Slider")
        self.assertTrue(slider())


class RibbonTest(HabitsBase):
    def tab(self, name):
        from yc import ui

        x, y = ui.state()["ribbon"]["tabs"][name]
        self.keys(f"m:{int(x)},{int(y)}|w:2|c:{int(x)},{int(y)}|w:6")

    def test_commands_of_a_tab_have_their_own_icons(self):
        from yc import ui

        glyph = {c["id"]: c.get("glyph", "") for c in ui.commands()}
        allowed = {frozenset({"ShapeFill", "ShapeNoFill"})}
        for name in ("Home", "Insert", "Page Layout", "Formulas", "Data", "Review", "View", "Developer"):
            self.tab(name)
            seen = {}
            for it in ui.ribbon_items():
                r = it["rect"]
                if it["where"] not in ("ribbon", "dropdown") or not glyph.get(it["cmd"]) or r[2] - r[0] < 20:
                    continue  # (a group's dialog launcher is the same small arrow everywhere, as in Excel)
                seen.setdefault(glyph[it["cmd"]], set()).add(it["cmd"])
            shared = [c for c in seen.values() if len(c) > 1 and frozenset(c) not in allowed]
            self.assertFalse(shared, f"{name}: commands sharing an icon")
        self.tab("Home")

    def test_column_chart_gallery_has_the_3d_charts(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "A", "B"], ["x", 1, 2], ["y", 3, 1], ["z", 2, 4]]
        self.keys("g:A1:C4|w:3")
        self.keytips("NC")
        tiles = {i["cmd"] for i in ui.ribbon_items() if i["where"] == "menu"}
        for cid in ("InsertChartColumnStacked", "InsertChart3DClusteredColumn", "InsertChartBarStacked100", "InsertChart3DBar"):
            self.assertIn(cid, tiles, "Excel's Column or Bar gallery")
        r = next(i["rect"] for i in ui.ribbon_items() if i["cmd"] == "InsertChartColumnStacked")
        n = self.ws.ChartObjects().Count
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:3|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:8")
        self.assertEqual(self.ws.ChartObjects().Count, n + 1)
        self.assertEqual(self.ws.ChartObjects(n + 1).Chart.ChartType, 52)  # xlColumnStacked

    def test_combo_is_a_drop_down(self):
        from yc import ui

        self.ws.Range("A1:C4").Value = [["M", "A", "B"], ["x", 1, 20], ["y", 3, 10], ["z", 2, 40]]
        self.keys("g:A1:C4|w:3")
        self.tab("Insert")
        r = next(i["rect"] for i in ui.ribbon_items() if i["cmd"] == "InsertChartComboMenu")
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:3|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:6")
        t = next(i["rect"] for i in ui.ribbon_items() if i["cmd"] == "InsertChartComboSecondary")
        n = self.ws.ChartObjects().Count
        self.keys(f"m:{int((t[0] + t[2]) / 2)},{int((t[1] + t[3]) / 2)}|w:3|c:{int((t[0] + t[2]) / 2)},{int((t[1] + t[3]) / 2)}|w:8")
        self.assertEqual(self.ws.ChartObjects().Count, n + 1)
        self.assertEqual(self.ws.ChartObjects(n + 1).Chart.SeriesCollection(2).AxisGroup, 2, "the line on the secondary axis")
        self.tab("Home")

    def test_review_groups_in_excels_order_and_notes_drop_down(self):
        from yc import ui

        self.tab("Review")
        items = {i["cmd"]: i["rect"] for i in ui.ribbon_items()}
        self.assertLess(items["NewComment"][0], items["NotesMenu"][0])
        self.assertLess(items["NotesMenu"][0], items["ProtectSheet"][0], "Protect comes after Notes (Excel)")
        self.assertNotIn("InsertComment", items, "New Note is in the Notes drop-down")
        self.tab("Home")

    def test_pivot_name_box_renames(self):
        from yc import excel, ui

        self.ws.Range("A1:B4").Value = [["Region", "Sales"], ["East", 1], ["West", 2], ["East", 3]]
        pc = excel.ActiveWorkbook.PivotCaches().Create(1, self.ws.Range("A1:B4"))
        pt = pc.CreatePivotTable(self.ws.Range("D3"), "PT1")
        pt.PivotFields("Region").Orientation = 1
        pt.AddDataField(pt.PivotFields("Sales"))
        self.keys("g:D4|w:6")
        self.tab("PivotTable Analyze")
        box = [i for i in ui.items() if i["label"] == "PivotTable Name"]
        self.assertTrue(box, "the PivotTable Name box")
        r = box[0]["rect"]
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:3|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:3|k:c-A|t:Sales by Region|k:Enter|w:6")
        self.assertEqual(self.ws.PivotTables(1).Name, "Sales by Region")
        self.tab("Home")


class BackstageTest(HabitsBase):
    def file(self):
        from yc import ui

        x, y = ui.state()["ribbon"]["tabs"]["File"]
        self.keys(f"m:{int(x)},{int(y)}|w:2|c:{int(x)},{int(y)}|w:6")
        self.assertTrue(ui.state()["ribbon"]["backstage"])

    def page(self, cid):
        from yc import ui

        r = next(i["rect"] for i in ui.ribbon_items() if i["where"] == "backstage" and i["cmd"] == cid)
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:2|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:6")

    def test_file_opens_on_home_with_the_recent_workbooks_pinned_and_removed(self):
        from yc import excel, ui

        path = os.path.join(self.tmp, "habits_recent.ycg")
        excel.ActiveWorkbook.SaveAs(path)
        self.file()
        try:
            labels = [i["label"] for i in ui.items()]
            self.assertTrue(any(lb.startswith("habits_recent.ycg") for lb in labels), "Home lists the recent workbooks")
            self.page("BackstageOpen")
            MenusTest.click_label(self, "Pin habits_recent.ycg")
            self.assertIn("Unpin habits_recent.ycg", [i["label"] for i in ui.items()], "pinned")
            row = next(i["rect"] for i in ui.items() if i["label"].startswith("habits_recent.ycg"))
            x, y = int(row[0]) + 40, int((row[1] + row[3]) / 2)
            self.keys(f"m:{x},{y}|w:2|r:|w:5")
            MenusTest.click_label(self, "Remove from list")
            self.assertFalse(any(i["label"].startswith("habits_recent.ycg") for i in ui.items()), "removed from the list")
        finally:
            self.keys("k:Escape|w:4")

    def test_options_categories_go_to_the_trust_center(self):
        from yc import ui

        self.file()
        try:
            self.page("BackstageOptions")
            self.assertFalse(any(i["label"] == "Trust Center Settings..." for i in ui.items()), "below the fold at first")
            nav = [i for i in ui.items() if i["label"] == "Trust Center"]
            self.assertTrue(nav, "Excel's categories on the left")
            r = nav[0]["rect"]
            self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:2|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:6")
            self.assertTrue(any(i["label"] == "Trust Center Settings..." for i in ui.items()), "the category is scrolled into view")
        finally:
            self.keys("k:Escape|w:4")

    def test_export_changes_the_file_type_to_excel(self):
        from yc import ui

        out = os.path.join(self.tmp, "habits_export.xlsx")
        ui.answer_file_dialogs(out)
        self.file()
        try:
            self.page("BackstageExport")
            MenusTest.click_label(self, "Excel Workbook (*.xlsx)")
            self.keys("w:10")
            self.assertTrue(os.path.exists(out), "saved as an Excel workbook")
        finally:
            self.keys("k:Escape|w:4")


class CursorTest(HabitsBase):
    def design(self, name):
        d = self.conn.request("ui.cursor_design", {"name": name}, timeout=10)
        rows = d["rows"]
        pts = [(x, y) for y, r in enumerate(rows) for x, ch in enumerate(r) if ch != "."]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        return rows, (max(xs) - min(xs) + 1, max(ys) - min(ys) + 1), (d["hot_x"], d["hot_y"])

    def test_cursors_are_excels_size(self):
        # the cell cross was 23 px with 9 px arms on Excel's 32 px grid, much fatter than Excel's: sizes measured from
        # Excel's own cursors (the fill cross, the header arrows, the resize arrows)
        rows, size, hot = self.design("cell")
        self.assertEqual(size, (17, 17), "the white cross: 15 px and its outline")
        arm = sum(1 for ch in rows[hot[1] - 5] if ch != ".")
        self.assertEqual(arm, 5, "3 px arms and their outline")
        rows, size, hot = self.design("fill")
        self.assertEqual(size, (13, 13))
        self.assertEqual(sum(1 for ch in rows[hot[1] - 4] if ch != "."), 3, "1 px arms")
        _, size, hot = self.design("column_header")
        self.assertEqual(size, (9, 12))
        _, size, _ = self.design("row_header")
        self.assertEqual(size, (12, 9))
        _, size, _ = self.design("column_unhide")
        self.assertLessEqual(size[0], 19)
        self.assertLessEqual(size[1], 17)


class DialogSizeTest(HabitsBase):
    def test_password_dialog_keeps_its_width(self):
        # a dialog fitting its contents with a full-width box and wrapped text got narrower every frame
        from yc import ui

        x, y = ui.state()["ribbon"]["tabs"]["File"]
        self.keys(f"m:{int(x)},{int(y)}|w:2|c:{int(x)},{int(y)}|w:6")
        r = next(i["rect"] for i in ui.ribbon_items() if i["where"] == "backstage" and i["cmd"] == "BackstageInfo")
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:2|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:6")
        MenusTest.click_label(self, "Encrypt with Password...")
        self.assertTrue(self.dialog(), "Encrypt Document opens")

        def width():  # the full-width password box follows the dialog's width
            return next(i["rect"][2] - i["rect"][0] for i in ui.items() if i["id"] == "##pw")

        self.keys("w:6")
        first = width()
        self.keys("w:60")
        self.assertEqual(width(), first, "the dialog keeps its width")
        self.assertEqual(ui.state()["shrinking_windows"], [], "no window narrows by itself")
        self.keys("k:Escape|w:4")

    # ---- the size a dialog opens at, measured in OK buttons (80 wide in every dialog, at any screen scale) ----
    def dialog_box(self):
        """(width, height, OK button width) of the dialog on the screen."""
        from yc import ui

        wins = [w for w in ui.state()["windows"] if w["popup"]]
        self.assertTrue(wins, "a dialog is open")
        w = max(wins, key=lambda w: w["rect"][2] * w["rect"][3])
        x, y, ww, hh = w["rect"]
        ok = next(i["rect"] for i in ui.items() if i["label"] == "OK" and x <= i["rect"][0] <= x + ww and y <= i["rect"][1] <= y + hh)
        self.ok_gap = y + hh - ok[3]  # from the OK button to the dialog's bottom edge
        self.ok_height = ok[3] - ok[1]
        return ww, hh, ok[2] - ok[0]

    def test_password_dialogs_open_narrow(self):
        # Encrypt Document took its longest sentence's width and stretched the password boxes with it (4.75 OK buttons);
        # Protect Sheet and Protect Structure were half again as wide as Excel's
        from yc import ui

        x, y = ui.state()["ribbon"]["tabs"]["File"]
        self.keys(f"m:{int(x)},{int(y)}|w:2|c:{int(x)},{int(y)}|w:6")
        r = next(i["rect"] for i in ui.ribbon_items() if i["where"] == "backstage" and i["cmd"] == "BackstageInfo")
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:2|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:6")
        MenusTest.click_label(self, "Encrypt with Password...")
        w, h, ok = self.dialog_box()
        self.assertLessEqual(w / ok, 4.0, "Encrypt Document is as narrow as Excel's")
        self.keys("k:Escape|w:4")
        for cmd, most in (("ProtectSheet", 4.2), ("ProtectWorkbook", 3.5)):
            ui.command(cmd)
            self.keys("w:6")
            w, h, ok = self.dialog_box()
            self.assertLessEqual(w / ok, most, f"{cmd} is as narrow as Excel's")
            self.keys("k:Escape|w:4")

    def test_sort_dialog_shows_its_whole_table(self):
        # the Sort dialog's width was fixed below its table's: the Order column and the table's border were cut off
        from yc import ui

        self.ws.Range("A1:C4").Value = [["Name", "Qty", "Price"], ["b", 2, 3], ["a", 1, 2], ["c", 3, 1]]
        self.keys("g:A1:C4|w:3")
        ui.command("CustomSort")
        self.keys("w:8")
        self.assertEqual(ui.state()["clipped_windows"], [], "no dialog cuts off its contents")
        self.keys("k:Escape|w:4")

    def test_data_validation_keeps_one_size(self):
        # with Allow: Any value the dialog was one row high and jumped when another Allow was chosen
        from yc import ui

        ui.command("DataValidation")
        self.keys("w:6")
        w, h, ok = self.dialog_box()
        self.assertGreaterEqual(h / ok, 3.5, "Data Validation opens at its full height")
        self.assertLess(self.ok_gap, self.ok_height * 1.5, "its buttons are at the bottom, as in Excel")
        self.keys("k:Escape|w:4")

    def test_screenshot_and_spelling_windows_keep_their_width(self):
        # the screenshot window placed its x from its own width; Spelling wraps the sentence: both narrowed every frame
        from yc import ui

        ui.command("ScreenshotWorkbook")
        self.keys("w:60")
        self.assertNotIn("##yc_screenshot", ui.state()["shrinking_windows"])
        self.ws.Range("A1").Value = "teh wrnog words in a long sentence that wraps in the window"
        self.keys("g:A1|w:3")
        ui.command("Spelling")
        self.keys("w:60")
        self.assertEqual(ui.state()["shrinking_windows"], [], "no window narrows by itself")
        MenusTest.click_label(self, "Cancel")


class AboutMenuTest(HabitsBase):
    def open_about(self):
        from yc import ui

        x, y = ui.state()["ribbon"]["tabs"]["About"]  # after the tabs, where a person finds it
        self.keys(f"m:{int(x)},{int(y)}|w:2|c:{int(x)},{int(y)}|w:6")

    def test_about_lists_the_version_and_the_program(self):
        from yc import excel, ui

        self.open_about()
        texts = [i["label"] for i in ui.items()]
        exe = os.path.normcase(os.path.abspath(EXE))
        self.assertIn(f"YCGrid {excel.Application.Version}", texts, "the version")
        self.assertTrue(any(os.path.normcase(t) == exe for t in texts), f"the program's path ({exe}) in {texts}")
        MenusTest.click_label(self, "Copy Version and Location")
        self.keys("g:A1|w:3|k:c-V|w:6")  # pasted where a person pastes it: one line per cell
        self.assertEqual(self.ws.Range("A1").Value, f"YCGrid {excel.Application.Version}")
        self.assertEqual(os.path.normcase(self.ws.Range("A2").Value), exe)
        self.open_about()
        MenusTest.click_label(self, "Open File Location")  # a hidden instance only says where (no Explorer window)
        self.assertEqual(os.path.normcase(ui.state()["status"].split(": ", 1)[1]), exe)


class NewWorkbookDefaultsTest(unittest.TestCase):
    """File > Options > General: a new workbook's font, size and number of sheets (a YCGrid started with such settings)."""

    @classmethod
    def setUpClass(cls):
        import json

        cls.tmp = tempfile.mkdtemp(prefix="yc_parity_habits_nw_")
        settings = os.path.join(cls.tmp, "settings.json")
        with open(settings, "w", encoding="utf-8") as f:
            json.dump({"general": {"standard_font": "Arial", "standard_font_size": 14, "sheets_in_new_workbook": 3}}, f)
        old = os.environ.get("YCGRID_SETTINGS")
        os.environ["YCGRID_SETTINGS"] = settings
        try:
            cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        finally:
            if old is None:
                os.environ.pop("YCGRID_SETTINGS", None)
            else:
                os.environ["YCGRID_SETTINGS"] = old
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def test_the_first_workbook_follows_the_options(self):
        from yc import excel

        time.sleep(1.0)
        app = excel.Application
        self.assertEqual((app.StandardFont, app.StandardFontSize, app.SheetsInNewWorkbook), ("Arial", 14.0, 3))
        self.assertEqual(excel.Worksheets.Count, 3)
        f = excel.Worksheets(1).Range("A1").Font
        self.assertEqual((f.Name, f.Size), ("Arial", 14))


class VsCodeNotFoundTest(unittest.TestCase):
    """Alt+F11 on a machine without VS Code where it is usually installed: it asks for VS Code's program and keeps it
    in the settings (a YCGrid started with YCGRID_NO_VSCODE=1 looks nowhere but the settings)."""

    @classmethod
    def setUpClass(cls):
        import json

        cls.tmp = tempfile.mkdtemp(prefix="yc_parity_habits_vs_")
        cls.settings = os.path.join(cls.tmp, "settings.json")
        with open(cls.settings, "w", encoding="utf-8") as f:
            json.dump({"projects": {"folder": os.path.join(cls.tmp, "projects")}}, f)
        saved = {k: os.environ.get(k) for k in ("YCGRID_SETTINGS", "YCGRID_NO_VSCODE")}
        os.environ["YCGRID_SETTINGS"] = cls.settings
        os.environ["YCGRID_NO_VSCODE"] = "1"
        try:
            cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def saved_vscode(self):
        import json

        with open(self.settings, encoding="utf-8") as f:
            return json.load(f).get("editor", {}).get("vscode", "")

    def test_alt_f11_asks_for_vscode_and_keeps_the_answer(self):
        from yc import ui

        # not found: a question, and nothing opens; Escape leaves it so
        self.keys("m:2,2|w:2|k:a-F11|w:10")
        self.assertIn("VS Code not found", ui.state().get("dialog_text", ""))
        self.assertEqual(ui.state()["vscode_request"], "", "nothing opens before VS Code is found")
        self.keys("k:Escape|w:4")
        self.assertEqual(ui.state().get("dialog", 0), 0)
        self.assertEqual(ui.state()["vscode_request"], "")
        self.assertEqual(self.saved_vscode(), "")

        # Download VS Code: the download page (a hidden instance opens no browser, it says so)
        self.keys("k:a-F11|w:10")
        MenusTest.click_label(self, "Download VS Code")
        self.assertIn("download page", ui.state()["status"])
        self.assertEqual(ui.state().get("dialog", 0), 0)

        # a file that isn't a program is refused and not kept
        readme = os.path.join(self.tmp, "README.txt")
        open(readme, "w").close()
        ui.answer_file_dialogs(readme)
        self.keys("k:a-F11|w:10|k:Enter|w:8")
        self.assertIn("Choose VS Code's program", "\n".join(e.get("text", "") for e in ui.errors()[-3:]) + ui.state().get("dialog_text", ""))
        self.keys("k:Escape|w:3|k:Escape|w:3")
        self.assertEqual(self.saved_vscode(), "")
        self.assertEqual(ui.state()["vscode_request"], "")

        # Browse (Enter) picks the program: it opens and is kept in the settings
        code = os.path.join(self.tmp, "VS Code", "Code.exe")
        os.makedirs(os.path.dirname(code), exist_ok=True)
        open(code, "wb").close()
        ui.answer_file_dialogs(code)
        self.keys("k:a-F11|w:10")
        self.assertIn("VS Code not found", ui.state().get("dialog_text", ""))
        self.keys("k:Enter|w:10")
        asked = ui.state()["vscode_request"]
        self.assertTrue(asked.endswith("main.py"), f"the project's main.py opens ({asked!r})")
        self.assertEqual(os.path.normcase(self.saved_vscode()), os.path.normcase(code))
        self.assertIn("kept in the settings", ui.state()["status"])

        # the next time it opens without a question
        self.keys("k:Escape|w:3|k:a-F11|w:10")
        self.assertEqual(ui.state().get("dialog", 0), 0, ui.state().get("dialog_text", ""))


class OptionsTest(HabitsBase):
    def options_page(self):
        from yc import ui

        x, y = ui.state()["ribbon"]["tabs"]["File"]
        self.keys(f"m:{int(x)},{int(y)}|w:2|c:{int(x)},{int(y)}|w:6")
        r = next(i["rect"] for i in ui.ribbon_items() if i["where"] == "backstage" and i["cmd"] == "BackstageOptions")
        self.keys(f"m:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:2|c:{int((r[0] + r[2]) / 2)},{int((r[1] + r[3]) / 2)}|w:6")

    def test_zeros_hidden_from_the_options_page_and_saved(self):
        from yc import excel, ui

        self.ws.Range("A1:B1").Value = [[0, 5]]
        self.options_page()
        MenusTest.click_label(self, "Advanced")
        MenusTest.click_label(self, "Show a zero in cells that have zero value")
        MenusTest.click_label(self, "OK")
        self.assertFalse(excel.ActiveWindow.DisplayZeros)
        self.keys("g:D6|w:4")  # the active cell's border away from the cells looked at
        shot = os.path.join(self.tmp, "zeros.png")
        ui.screenshot(shot, max_width=8000)
        _, _, px = read_png(shot)

        def ink(addr):
            r = ui.cell_rect(addr)
            return sum(1 for x in range(int(r[0]) + 3, int(r[2]) - 3) for y in range(int(r[1]) + 3, int(r[3]) - 3) if sum(px(x, y)) < 300)

        self.assertEqual(ink("A1"), 0, "no 0 shown")
        self.assertGreater(ink("B1"), 0, "5 is shown")
        excel.ActiveWindow.DisplayWorkbookTabs = False
        excel.ActiveWindow.GridlineColor = 0x0000FF  # RGB(255, 0, 0)
        path = os.path.join(self.tmp, "display.ycg")
        excel.ActiveWorkbook.SaveAs(path)
        excel.ActiveWindow.DisplayWorkbookTabs = True
        excel.ActiveWindow.DisplayZeros = True
        # the file opened by another YCGrid shows them as saved
        other = yc.launch(path, hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=other.pid)
            time.sleep(1.0)
            w = excel.ActiveWindow
            self.assertEqual((w.DisplayZeros, w.DisplayWorkbookTabs, w.GridlineColor), (False, False, 0x0000FF), "saved with the workbook")
            self.assertFalse([i for i in ui.items() if i["label"] == excel.ActiveSheet.Name], "no sheet tabs")
        finally:
            other.terminate()
            other.wait(10)
            yc.connect(target=self.proc.pid)

    def test_page_breaks_turned_off_from_the_options_page_and_not_saved(self):
        """Excel's Normal view shows the page breaks after Print or a page setup change, until "Show page breaks" is
        cleared (File > Options > Advanced) or DisplayPageBreaks is False; the setting isn't kept in the file."""
        from yc import excel, ui

        self.ws.Range("A1:F12").Value = 1
        self.keys("g:H20|w:4")  # the active cell away from the line looked at
        self.assertFalse(self.ws.DisplayPageBreaks, "no lines before anything printed")
        self.ws.HPageBreaks.Add(self.ws.Range("A6"))  # a manual break: Excel's blue line above row 6
        self.assertTrue(self.ws.DisplayPageBreaks)

        def line():  # the manual break's blue dashes along the top of row 6
            shot = os.path.join(self.tmp, "breaks.png")
            self.keys("w:6")
            ui.screenshot(shot, max_width=8000)
            _, _, px = read_png(shot)
            top = int(ui.cell_rect("B6")[1])
            x0, x1 = int(ui.cell_rect("B6")[0]), int(ui.cell_rect("E6")[2])
            return sum(1 for x in range(x0, x1) for y in range(top - 2, top + 3) if px(x, y)[2] > 140 and px(x, y)[0] < 90)

        self.assertGreater(line(), 10, "the break is drawn")
        self.options_page()
        MenusTest.click_label(self, "Advanced")
        MenusTest.click_label(self, "Show page breaks")
        MenusTest.click_label(self, "OK")
        self.assertFalse(self.ws.DisplayPageBreaks)
        self.assertEqual(line(), 0, "no line once Show page breaks is cleared")
        self.ws.DisplayPageBreaks = True
        self.assertGreater(line(), 10)
        self.ws.DisplayPageBreaks = False
        self.assertEqual(line(), 0)
        self.ws.PageSetup.Orientation = 2  # a page setup change shows them again, as in Excel
        self.assertTrue(self.ws.DisplayPageBreaks)
        self.ws.DisplayPageBreaks = None  # Null written back changes nothing
        self.assertTrue(self.ws.DisplayPageBreaks)

        name = self.ws.Name
        path = os.path.join(self.tmp, "breaks.ycg")
        excel.ActiveWorkbook.SaveCopyAs(path)
        other = yc.launch(path, hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=other.pid)
            time.sleep(1.0)
            self.assertFalse(excel.Worksheets(name).DisplayPageBreaks, "not saved with the workbook")
        finally:
            other.terminate()
            other.wait(10)
            yc.connect(target=self.proc.pid)
        self.ws.DisplayPageBreaks = False

    def test_scroll_bars_hidden(self):
        from yc import excel, ui

        self.assertTrue([i for i in ui.items("") if i["id"] == "##vscroll"])
        excel.ActiveWindow.DisplayVerticalScrollBar = False
        excel.ActiveWindow.DisplayHorizontalScrollBar = False
        try:
            self.keys("w:4")
            ids = {i["id"] for i in ui.items("")}
            self.assertNotIn("##vscroll", ids)
            self.assertNotIn("##hscroll", ids)
        finally:
            excel.ActiveWindow.DisplayVerticalScrollBar = True
            excel.ActiveWindow.DisplayHorizontalScrollBar = True

    def test_quick_analysis_option_hides_the_button(self):
        from yc import excel, ui

        self.ws.Range("A1:B3").Value = [["n", "v"], ["a", 1], ["b", 2]]
        self.options_page()
        MenusTest.click_label(self, "Show Quick Analysis options on selection")
        MenusTest.click_label(self, "OK")
        try:
            self.assertFalse(excel.Application.ShowQuickAnalysis)
            self.keys("g:A1:B3|w:6")
            self.assertFalse([i for i in ui.items() if i["label"] == "Quick Analysis"], "no button")
            self.keys("k:c-Q|w:6")
            self.assertTrue([i for i in ui.items() if i["label"] == "Data Bars"], "Ctrl+Q still opens it")
        finally:
            excel.Application.ShowQuickAnalysis = True

    def test_paste_options_button_and_overwrite_alert_can_be_off(self):
        from yc import excel, ui

        app = excel.Application
        self.ws.Range("A1:A2").Value = [[1], [2]]
        self.ws.Range("C1").Value = "keep"
        app.AlertBeforeOverwriting = False
        try:
            self.keys("g:A1:A2|w:2")
            r, c = ui.cell_rect("A1"), ui.cell_rect("C1")
            x0, y0 = int(r[2]) - 1, int((r[1] + r[3]) / 2)
            x1, y1 = int((c[0] + c[2]) / 2), int((c[1] + c[3]) / 2)
            self.keys(f"m:{x0},{y0}|w:6|d:{x0},{y0},{x1},{y1}|w:8")
            self.assertFalse(self.dialog(), "no question")
            self.assertEqual((self.ws.Range("C1").Value, self.ws.Range("C2").Value), (1, 2))
        finally:
            app.AlertBeforeOverwriting = True
        # Show Paste Options button off (the Options page): a paste shows no button
        self.options_page()
        MenusTest.click_label(self, "Advanced")
        MenusTest.click_label(self, "Show Paste Options button when content is pasted")
        MenusTest.click_label(self, "OK")
        try:
            self.keys("g:C1|w:2|k:c-C|w:3|g:E1|w:2|k:c-V|w:6")
            self.assertEqual(self.ws.Range("E1").Value, 1)
            shot = os.path.join(self.tmp, "pasteopts.png")
            ui.screenshot(shot, max_width=8000)
            _, _, px = read_png(shot)
            e = ui.cell_rect("E1")
            corner = [px(int(e[2]) + 6 + k, int(e[3]) + 8) for k in range(10)]
            self.assertTrue(all(sum(p) > 700 for p in corner), "no Paste Options button at the corner")
        finally:
            self.options_page()
            MenusTest.click_label(self, "Advanced")
            MenusTest.click_label(self, "Show Paste Options button when content is pasted")
            MenusTest.click_label(self, "OK")


if __name__ == "__main__":
    unittest.main()
