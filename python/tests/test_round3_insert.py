# SPDX-License-Identifier: Apache-2.0
"""Round 3 (insert): the Insert tab, shapes, controls, notes, sparklines, spelling and the Protect group working like
Excel's, each checked the way a person does it. Every test fails without its fix.

Run: python -m unittest tests.test_round3_insert (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


class Round3InsertBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        cls.proc.wait(30)

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

    def cell(self, addr):
        from yc import ui

        r = ui.cell_rect(addr)
        return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)

    def ppt(self):
        """Window pixels per point (the zoom and the screen's scale)."""
        from yc import ui

        a, b = ui.cell_rect("A1"), ui.cell_rect("B2")
        return (b[0] - a[0]) / self.ws.Range("A1").Width

    def mid(self, rect):
        return int((rect[0] + rect[2]) / 2), int((rect[1] + rect[3]) / 2)

    def click(self, x, y, extra=""):
        self.keys(f"m:{x},{y}|w:2|{extra}c:{x},{y}|w:5")

    def item(self, label, window="", by="label"):
        from yc import ui

        hits = [i for i in ui.items(window) if i[by] == label]
        self.assertTrue(hits, f"{label!r} not on the screen: {[i['label'] for i in ui.items(window)][:60]}")
        return self.mid(hits[0]["rect"])

    def press(self, label, window="", by="label"):
        x, y = self.item(label, window, by)
        self.click(x, y)

    def tab(self, label):
        from yc import ui

        tabs = ui.state()["ribbon"]["tabs"]
        self.assertIn(label, tabs)
        x, y = tabs[label]
        self.click(int(x), int(y))

    def ribbon(self, cmd, where=None):
        from yc import ui

        for it in ui.ribbon_items():
            if it["cmd"] == cmd and (where is None or it["where"] == where):
                return self.mid(it["rect"])
        self.fail(f"{cmd} is not on the ribbon now")

    def menu_pick(self, menu_cmd, entry_cmd, split=False):
        """Open a drop-down on the ribbon (a split button: its arrow) and click one of its entries / tiles."""
        x, y = self.ribbon(menu_cmd, "dropdown" if split else None)
        if split:  # a large split button: its arrow is the lower part
            from yc import ui

            r = next(i["rect"] for i in ui.ribbon_items() if i["cmd"] == menu_cmd)
            y = int(r[3] - 6)
        self.click(x, y)
        x, y = self.ribbon(entry_cmd)
        self.click(x, y)

    def command(self, cid):
        from yc import ui

        return next(c for c in ui.commands() if c["id"] == cid)

    def selected(self):
        return list(self.conn.request("shape.selection", {}, timeout=10) or [])


class ProtectTest(Round3InsertBase):
    def test_protect_sheet_remembers_its_options_and_the_button_says_unprotect(self):
        # the dialog started with Excel's defaults every time; the button kept one label; no Edit scenarios
        self.ws.Protect(AllowFormattingCells=True)
        self.assertTrue(self.ws.ProtectContents)
        self.keys("w:3")  # the ribbon reads the new state on its next frame (under load it read the old label)
        self.assertEqual(self.command("ProtectSheet")["label"].replace("\n", " "), "Unprotect Sheet")
        self.assertFalse(self.command("AllowEditRanges")["enabled"])
        self.ws.Unprotect()
        self.keys("w:3")
        self.assertEqual(self.command("ProtectSheet")["label"].replace("\n", " "), "Protect Sheet")
        self.tab("Review")
        x, y = self.ribbon("ProtectSheet")
        self.click(x, y)
        self.item("Protect worksheet and contents of locked cells")
        lx, ly = self.item("Select locked cells")  # the list scrolls, as Excel's: down to its last entries
        self.keys(f"m:{lx},{ly}|w:2|W:-10|w:4")
        self.item("Edit scenarios")
        self.item("Use PivotTable & PivotChart")
        self.keys("k:Enter|w:5")
        self.assertTrue(self.ws.ProtectContents)
        self.assertTrue(self.ws.Protection.AllowFormattingCells)
        self.assertTrue(self.ws.ProtectScenarios)

    def test_a_password_is_confirmed(self):
        self.tab("Review")
        x, y = self.ribbon("ProtectSheet")
        self.click(x, y)
        self.keys("t:abc|w:2|k:Enter|w:5")
        # Confirm Password, as Excel asks
        self.item("OK", "Confirm Password")
        self.keys("t:abc|w:2|k:Enter|w:5")
        self.assertTrue(self.ws.ProtectContents)
        with self.assertRaises(Exception):
            self.ws.Unprotect("wrong")
        self.ws.Unprotect("abc")
        self.assertFalse(self.ws.ProtectContents)

    def test_allow_edit_ranges_new_and_modify(self):
        self.keys("g:B2:C3|w:3")
        self.tab("Review")
        x, y = self.ribbon("AllowEditRanges")
        self.click(x, y)
        self.press("New...")
        self.item("OK", "New Range")
        self.keys("k:Enter|w:4")  # New Range: OK
        self.press("Modify...")
        self.item("OK", "Modify Range")
        self.keys("k:Escape|w:3")
        self.press("OK")
        ranges = self.conn.request("protect.get", {"sheet": self.ws._id}, timeout=10)["ranges"]
        self.assertEqual(len(ranges), 1)


class ObjectsTest(Round3InsertBase):
    def test_go_to_special_objects_selects_every_object(self):
        from yc import ui

        self.ws.Shapes.AddShape(1, 100, 40, 60, 30)
        self.ws.Shapes.AddShape(9, 200, 40, 60, 30)
        ui.command("GoToSpecial")
        self.keys("w:4")
        self.press("Objects")
        self.keys("k:Enter|w:5")
        self.assertEqual(len(self.selected()), 2)

    def test_a_shape_from_the_gallery_is_drawn_by_dragging(self):
        # shapes were dropped at a fixed size; the gallery had seven shapes
        self.tab("Insert")
        self.menu_pick("InsertShapesMenu", "InsertShape92")  # Stars and Banners > Star: 5 Points
        a = self.cell("C3")
        self.keys(f"m:{a[0]},{a[1]}|w:2|d:{a[0]},{a[1]},{a[0] + 120},{a[1] + 60}|w:6|m:2,2|w:2")
        sp = self.ws.Shapes(self.ws.Shapes.Count)
        self.assertEqual(sp.AutoShapeType, 92)
        self.assertTrue(sp.Name.startswith("Star: 5 Points"), sp.Name)
        self.assertAlmostEqual(sp.Width, 120 / self.ppt(), delta=3)
        self.assertAlmostEqual(sp.Height, 60 / self.ppt(), delta=3)
        # Shift keeps it square
        self.tab("Insert")
        self.menu_pick("InsertShapesMenu", "InsertShape63")  # Flowchart: Decision
        b = self.cell("C12")
        self.keys(f"m:{b[0]},{b[1]}|w:2|H:shift|d:{b[0]},{b[1]},{b[0] + 120},{b[1] + 40}|h:shift|w:6|m:2,2|w:2")
        sp = self.ws.Shapes(self.ws.Shapes.Count)
        self.assertEqual(sp.AutoShapeType, 63)
        self.assertAlmostEqual(sp.Width, sp.Height, delta=1)
        # a click: Excel's one inch
        self.tab("Insert")
        self.menu_pick("InsertShapesMenu", "InsertShapeOval")
        c = self.cell("H3")
        self.click(*c)
        sp = self.ws.Shapes(self.ws.Shapes.Count)
        self.assertAlmostEqual(sp.Width, 72, delta=1)
        self.assertAlmostEqual(sp.Height, 72, delta=1)

    def test_connectors_and_freeforms_from_the_lines_group(self):
        self.tab("Insert")
        self.menu_pick("InsertShapesMenu", "InsertShapeElbowArrow")
        a = self.cell("B3")
        self.keys(f"m:{a[0]},{a[1]}|w:2|d:{a[0]},{a[1]},{a[0] + 150},{a[1] + 80}|w:6|m:2,2|w:2")
        sp = self.ws.Shapes(self.ws.Shapes.Count)
        self.assertEqual(sp.ConnectorFormat.Type, 2)
        self.assertEqual(sp.Line.EndArrowheadStyle != 1, True)
        # Freeform: Scribble follows the mouse
        self.tab("Insert")
        self.menu_pick("InsertShapesMenu", "InsertShapeScribble")
        b = self.cell("B12")
        self.keys(f"m:{b[0]},{b[1]}|w:2|p:{b[0]},{b[1]}|m:{b[0] + 40},{b[1] + 30}|w:2|m:{b[0] + 90},{b[1] - 10}|w:2|m:{b[0] + 130},{b[1] + 40}|w:2|"
                  f"u:{b[0] + 130},{b[1] + 40}|w:6|m:2,2|w:2")
        sp = self.ws.Shapes(self.ws.Shapes.Count)
        self.assertEqual(sp.AutoShapeType, 138)
        self.assertGreater(sp.Width, 110 / self.ppt())

    def test_a_copied_picture_pastes_as_a_picture(self):
        # H2: a picture on the clipboard (a screenshot, a picture copied elsewhere) pasted nothing
        from yc import ui

        self.ws.Range("A1").Value = 5
        self.keys("g:A1|w:2")
        ui.command("ScreenshotSelection")
        self.keys("w:12|k:Escape|w:3|g:D5|w:3|k:c-V|w:8")
        self.assertEqual(self.ws.Shapes.Count, 1)
        self.assertEqual(self.ws.Shapes(1).Type, 13)  # msoPicture
        self.assertAlmostEqual(self.ws.Shapes(1).Left, self.ws.Range("D5").Left, delta=0.5)

    def test_a_screenshot_clicked_in_a_hidden_instance_opens_no_viewer(self):
        # chaos clicked the screenshot toast's picture, and each click opened the picture viewer on the user's desktop
        # (ShellExecute from a hidden instance); it says in the status bar what it would open instead
        from yc import ui

        self.ws.Range("A1").Value = 5
        self.keys("g:A1|w:2")
        ui.command("ScreenshotSelection")
        self.keys("w:12")
        # the picture lies under the toast's first line, whose right end is its x button
        cx, cy = self.item("x")
        x, y = int(cx - 20), int(cy + 25)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:4")
        self.assertIn("Open: ", ui.state().get("status", ""))
        self.keys("k:Escape|w:3|m:2,2")

    def test_python_add_shape_takes_every_preset(self):
        sp = self.ws.Shapes.AddShape(63, 10, 10, 80, 50)  # msoShapeFlowchartDecision
        self.assertEqual(sp.AutoShapeType, 63)
        cv = self.ws.Shapes.AddCurve([[10, 100], [60, 60], [120, 120]])
        self.assertEqual(cv.AutoShapeType, 138)
        cn = self.ws.Shapes.AddConnector(3, 10, 200, 150, 260)
        self.assertEqual(cn.ConnectorFormat.Type, 3)
        wa = self.ws.Shapes.AddTextEffect(1, "Hello", "", 36, False, False, 10, 300)
        self.assertEqual(wa.TextFrame.Characters().Text, "Hello")

    def test_icons_are_inserted_as_pictures(self):
        self.tab("Insert")
        x, y = self.ribbon("InsertIcons")
        self.click(x, y)
        self.press("icon:Star", "Insert Icons")
        self.press("icon:Heart", "Insert Icons")
        self.press("Insert (2)", "Insert Icons")
        self.assertEqual(self.ws.Shapes.Count, 2)
        self.assertEqual(self.ws.Shapes(1).Type, 13)  # msoPicture

    def test_icons_are_transparent_around_the_shape(self):
        # the PNG encoder wrote every pixel opaque: an icon was a dark gray square, in the dialog and on the sheet
        import tempfile

        from tests.test_parity_insert import read_png
        from yc import ui

        self.tab("Insert")
        x, y = self.ribbon("InsertIcons")
        self.click(x, y)
        self.press("icon:Star", "Insert Icons")
        self.press("Insert (1)", "Insert Icons")
        self.keys("k:Escape|w:2|g:Z40|w:2|g:A1|w:4")  # nothing selected over the picture
        star = self.ws.Shapes(1)
        a1 = ui.cell_rect("A1")
        px_per_pt = (a1[2] - a1[0]) / self.ws.Range("A1").Width
        # the star's box corner, outside the star: the sheet shows through
        cx = int(a1[0] + (star.Left + star.Width * 0.06) * px_per_pt)
        cy = int(a1[1] + (star.Top + star.Height * 0.06) * px_per_pt)
        path = os.path.join(tempfile.mkdtemp(), "icon.png")
        ui.screenshot(path, max_width=0)
        _, _, pixel = read_png(path)
        self.assertGreater(min(pixel(cx, cy)), 200, f"the corner of the icon is transparent, not {pixel(cx, cy)}")

    def test_wordart_takes_the_typing(self):
        self.tab("Insert")
        self.menu_pick("InsertWordArtMenu", "WordArt1")
        self.assertEqual(self.ws.Shapes.Count, 1)
        self.keys("t:Sales|w:2|k:Escape|w:5")
        tb = self.ws.Shapes(1)
        self.assertEqual(tb.TextFrame.Characters().Text, "Sales")
        self.assertEqual(tb.TextFrame.Characters().Font.Size, 54)

    def test_header_and_footer_opens_page_layout(self):
        self.tab("Insert")
        x, y = self.ribbon("InsertHeaderFooter")
        self.click(x, y)
        self.keys("w:6")
        self.assertTrue(self.command("ViewPageLayout")["pressed"])
        from yc import ui

        ui.command("ViewNormal")
        self.keys("w:4")

    def test_symbol_special_characters(self):
        from yc import ui

        ui.command("InsertSymbol")
        self.keys("w:5")
        self.press("Special Characters")
        self.press("©", "Symbol")
        self.press("Insert", "Symbol")
        self.assertEqual(self.ws.Range("A1").Value, "©")
        self.item("Close", "Symbol")  # Cancel reads Close once something was inserted
        self.press("Close", "Symbol")


class ControlsTest(Round3InsertBase):
    def test_activex_properties_show_excels_values(self):
        from yc import ui

        btn = self.ws.OLEObjects.Add("Forms.CommandButton.1", Left=100, Top=40, Width=90, Height=26)
        self.conn.request("shape.select", {"sheet": self.ws._id, "name": btn.Name}, timeout=10)
        ui.command("ControlProperties")
        self.keys("w:6")
        # scrolled to its end, as a person does to reach Placement and PrintObject
        win = [w for w in ui.windows() if w["label"].startswith("Properties")][0]["rect"]
        x, y = self.mid(win)
        self.keys(f"m:{x},{y}|w:2|W:-20|w:4")
        labels = [i["label"] for i in ui.items("Properties")]
        ids = [i["id"] for i in ui.items("Properties")]
        self.assertIn("1 - xlMoveAndSize", labels + ids, ids[:80])
        self.assertIn("print=True", labels + ids)

    def test_format_control_is_excels_dialog_with_print_object(self):
        from yc import ui

        cb = self.ws.Shapes.AddFormControl(1, 100, 40, 100, 20)  # xlCheckBox
        self.conn.request("shape.select", {"sheet": self.ws._id, "name": cb.Name}, timeout=10)
        ui.command("ControlProperties")
        self.keys("w:6")
        self.press("Properties", "Format Control")
        self.press("Print object", "Format Control")
        self.press("OK", "Format Control")
        ctl = [c for c in self.conn.request("obj.list", {"sheet": self.ws._id}, timeout=10)["controls"] if c["name"] == cb.Name][0]
        self.assertFalse(ctl["print_object"])


class NotesTest(Round3InsertBase):
    def test_note_box_is_sized_by_its_handle_and_formatted(self):
        self.ws.Range("B3").AddComment("hello")
        self.keys("g:B3|w:2|k:s-F2|w:6")
        x, y = self.item("note handle 5")  # bottom-right
        self.keys(f"m:{x},{y}|w:2|d:{x},{y},{x + 80},{y + 40}|w:6|m:2,2|w:2|k:Escape|w:4")
        shp = self.ws.Range("B3").Comment.Shape
        self.assertAlmostEqual(shp.Width, 150 + 80 / self.ppt(), delta=3)
        self.assertAlmostEqual(shp.Height, 90 + 40 / self.ppt(), delta=3)
        shp.Fill.ForeColor.RGB = 0xFFE0C0
        self.assertEqual(self.ws.Range("B3").Comment.Shape.Fill.ForeColor.RGB, 0xFFE0C0)
        # Format Comment
        self.keys("g:B3|w:2")
        from yc import ui

        ui.command("FormatComment")
        self.keys("w:5")
        self.press("Alignment", "Format Comment")
        self.press("OK", "Format Comment")


class SparklineTest(Round3InsertBase):
    def setUp(self):
        super().setUp()
        self.ws.Range("A1:C2").Value = [[1, None, 3], [4, 5, 6]]
        self.g = self.ws.Range("E1:E2").SparklineGroups.Add(1, "A1:C2")
        self.keys("g:E1|w:4")

    def test_sparkline_tab_edit_data_hidden_empty_axis_and_style(self):
        self.tab("Sparkline")
        # Hidden & Empty Cells: Zero
        self.menu_pick("SparkEditGroup", "SparkHiddenEmpty", split=True)
        self.press("Zero", "Hidden and Empty Cell Settings")
        self.press("OK", "Hidden and Empty Cell Settings")
        self.assertEqual(self.g.DisplayBlanksAs, 2)
        # Axis > Same for All Sparklines (minimum), Custom Value (maximum)
        self.menu_pick("SparkAxisMenu", "SparkMinSame")
        self.assertEqual(self.g.Axes.Vertical.MinScaleType, 1)
        self.menu_pick("SparkAxisMenu", "SparkMaxCustom")
        self.keys("k:c-A|t:10|k:Enter|w:5")
        self.assertEqual(self.g.Axes.Vertical.MaxScaleType, 3)
        self.assertEqual(self.g.Axes.Vertical.CustomMaxScaleValue, 10)
        # Style: a tile of the gallery colors the line
        before = self.g.SeriesColor.Color
        self.menu_pick("SparkStylesMenu", "SparkStyle1")
        self.assertNotEqual(self.g.SeriesColor.Color, before)

    def test_ungroup_and_group(self):
        self.keys("g:E1:E2|w:3")
        self.tab("Sparkline")
        x, y = self.ribbon("SparkUngroup")
        self.click(x, y)
        self.assertEqual(self.ws.Range("E1:E2").SparklineGroups.Count, 2)
        self.keys("g:E1:E2|w:3")
        self.tab("Sparkline")
        x, y = self.ribbon("SparkGroup")
        self.click(x, y)
        self.assertEqual(self.ws.Range("E1:E2").SparklineGroups.Count, 1)


class SpellingTest(Round3InsertBase):
    def test_undo_last_and_autocorrect(self):
        from yc import excel

        self.ws.Range("A1").Value = "speling"
        self.keys("g:A1|w:2|k:F7|w:8")
        self.press("Change")
        self.keys("w:5")
        self.assertEqual(self.ws.Range("A1").Value, "spelling")
        # the check ended: start again with a second word and take the change back with Undo Last
        self.keys("k:Enter|w:3")
        self.ws.Range("A2").Value = "wrnog"
        self.keys("g:A2|w:2|k:F7|w:8")
        self.press("AutoCorrect")
        self.keys("w:5|k:Enter|w:3")
        self.assertNotEqual(self.ws.Range("A2").Value, "wrnog")
        fixed = self.ws.Range("A2").Value
        # AutoCorrect corrects it when typed again
        self.keys("g:A4|w:2|t:wrnog|k:Enter|w:5")
        self.assertEqual(self.ws.Range("A4").Value, fixed)
        excel.Application.AutoCorrect.DeleteReplacement("wrnog")
        self.ws.Range("A5").Value = "teh speling"
        self.keys("g:A5|w:2|k:F7|w:8")
        self.press("Change")
        self.press("Undo Last")
        self.keys("w:5")
        self.assertEqual(self.ws.Range("A5").Value, "teh speling")
        self.keys("k:Escape|w:3")



class Round3MoreTest(Round3InsertBase):
    def test_symbol_shows_the_unicode_name_and_takes_ascii_codes(self):
        from yc import ui

        ui.command("InsertSymbol")
        self.keys("w:5")
        x, y = self.item("##code", "Symbol", by="id")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:2|k:c-A|t:25B2|w:3")
        self.item("unicode name:Black Up-Pointing Triangle", "Symbol")
        # from: ASCII (decimal): the code is typed in decimal
        x, y = self.item("##symfrom", "Symbol", by="id")
        self.click(x, y)
        self.press("ASCII (decimal)")
        x, y = self.item("##code", "Symbol", by="id")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:2|k:c-A|t:065|w:3")
        self.item("unicode name:Latin Capital Letter A", "Symbol")
        self.press("Insert", "Symbol")
        self.assertEqual(self.ws.Range("A1").Value, "A")
        self.press("Close", "Symbol")

    def test_permissions_let_this_account_edit_without_the_password(self):
        from yc import ui

        er = self.ws.Protection.AllowEditRanges.Add("R1", self.ws.Range("B2:C3"), "pw")
        me = os.environ.get("USERDOMAIN", "") + "\\" + os.environ.get("USERNAME", "")
        er.Users.Add(me, True)
        self.assertEqual(er.Users.Count, 1)
        self.assertTrue(er.Users.Item(1).AllowEdit)
        self.ws.Protect()
        self.keys("g:B2|w:2|t:5|k:Enter|w:6")
        self.assertEqual(self.ws.Range("B2").Value, 5)  # no password asked of an allowed account
        self.assertFalse(ui.state().get("dialog"))
        self.ws.Unprotect()
        self.keys("w:4")
        # Permissions... shows the account
        ui.command("AllowEditRanges")
        self.keys("w:5")
        self.press("Permissions...")
        self.item(self.ws.Protection.AllowEditRanges.Item(1).Users.Item(1).Name)
        self.keys("k:Escape|w:3|k:Escape|w:3")

    def test_form_button_alignment_and_3d_shading(self):
        from yc import ui

        b = self.ws.Shapes.AddFormControl(0, 100, 40, 90, 30)  # xlButtonControl
        self.conn.request("shape.select", {"sheet": self.ws._id, "name": b.Name}, timeout=10)
        ui.command("ControlProperties")
        self.keys("w:6")
        self.press("Alignment", "Format Control")
        self.press("Upward", "Format Control")
        self.press("Margins", "Format Control")
        self.item("Automatic##margins", "Format Control", by="id")
        self.press("OK", "Format Control")
        self.assertEqual(b.Orientation, -4171)  # xlUpward
        cb = self.ws.Shapes.AddFormControl(1, 100, 100, 100, 20)  # xlCheckBox
        self.conn.request("shape.select", {"sheet": self.ws._id, "name": cb.Name}, timeout=10)
        ui.command("ControlProperties")
        self.keys("w:6")
        self.press("Control", "Format Control")
        self.press("3-D shading", "Format Control")
        self.press("OK", "Format Control")
        self.assertTrue(cb.Display3DShading)

    def test_autocorrect_starts_with_excels_list(self):
        from yc import excel

        self.assertIn(("teh", "the"), excel.Application.AutoCorrect.ReplacementList())
        self.keys("g:A1|w:2|t:teh cat (c) 2026|k:Enter|w:5")
        self.assertEqual(self.ws.Range("A1").Value, "the cat © 2026")
        self.keys("g:A2|w:2|t:=1+1|k:Enter|w:5")  # formulas are left alone
        self.assertEqual(self.ws.Range("A2").Value, 2)

    def test_adjustment_handle_changes_the_shape(self):
        from yc import ui

        sp = self.ws.Shapes.AddShape(5, 100, 40, 160, 80)  # msoShapeRoundedRectangle
        self.assertAlmostEqual(sp.Adjustments.Item(1), 0.16667, delta=1e-4)
        a, k = ui.cell_rect("A1"), self.ppt()
        cx, cy = int(a[0] + (sp.Left + sp.Width / 2) * k), int(a[1] + (sp.Top + sp.Height / 2) * k)
        self.click(cx, cy)
        x, y = self.item("adjust handle 1")
        self.keys(f"m:{x},{y}|w:2|d:{x},{y},{x + int(20 * k)},{y}|w:6|m:2,2|w:2")
        self.assertAlmostEqual(sp.Adjustments.Item(1), 0.16667 + 20 / 80, delta=0.03)
        sp.Adjustments[1] = 0.4
        self.assertAlmostEqual(sp.Adjustments.Item(1), 0.4, delta=1e-9)



class Round3LastTest(Round3InsertBase):
    def test_paste_permissions_information_into_a_new_workbook(self):
        import json
        import subprocess

        from yc import ui

        er = self.ws.Protection.AllowEditRanges.Add("Budget", self.ws.Range("B2:C3"), "pw")
        er.Users.Add("PC\\ann", True)
        self.keys("w:3")
        ui.command("AllowEditRanges")
        self.keys("w:5")
        self.press("Paste permissions information into a new workbook")
        self.press("OK")
        # the new workbook: a hidden window of its own (this test's instance is hidden), started with --as-new
        child = None
        for _ in range(40):
            out = subprocess.run(["powershell", "-NoProfile", "-Command",
                                  "Get-CimInstance Win32_Process -Filter \"Name='ycgrid.exe'\" | Select-Object ProcessId,CommandLine | ConvertTo-Json"],
                                 capture_output=True, text=True).stdout
            procs = json.loads(out or "[]")
            procs = procs if isinstance(procs, list) else [procs]
            hits = [p for p in procs if "--as-new" in (p.get("CommandLine") or "") and f"--parent-pid {self.proc.pid}" in (p.get("CommandLine") or "")]
            if hits:
                child = hits[0]["ProcessId"]
                break
            time.sleep(0.5)
        self.assertIsNotNone(child)
        try:
            from yc import excel

            yc.connect(target=child)
            time.sleep(1)
            ws = excel.ActiveSheet
            self.assertTrue(str(ws.Range("A1").Value).startswith("Permissions for"))
            self.assertEqual(ws.Range("A4").Value, "Budget")
            self.assertEqual(ws.Range("C4").Value, "Yes")
            self.assertEqual(ws.Range("D4").Value, "PC\\ann")
            self.assertEqual(ws.Range("E4").Value, "Allow")
            self.assertEqual(excel.ActiveWorkbook.FullName.split("\\")[-1].split("/")[-1][:4], "Book")
        finally:
            yc.connect(target=self.proc.pid)
            subprocess.run(["taskkill", "/PID", str(child), "/F"], capture_output=True)

    def test_autocorrect_capital_i_and_replace_text_off(self):
        from yc import excel

        self.keys("g:A1|w:2|t:i|k:Enter|w:5")
        self.assertEqual(self.ws.Range("A1").Value, "I")
        excel.Application.AutoCorrect.ReplaceText = False
        try:
            self.keys("g:A2|w:2|t:i|k:Enter|w:5")
            self.assertEqual(self.ws.Range("A2").Value, "i")
        finally:
            excel.Application.AutoCorrect.ReplaceText = True

    def test_pie_and_curved_arrow_adjustments(self):
        pie = self.ws.Shapes.AddShape(142, 100, 40, 100, 100)  # msoShapePie
        self.assertEqual(pie.Adjustments.Count, 2)
        pie.Adjustments[1] = 90
        self.assertEqual(pie.Adjustments.Item(1), 90)
        arrow = self.ws.Shapes.AddShape(45, 250, 40, 80, 120)  # msoShapeCurvedRightArrow
        self.assertEqual(arrow.Adjustments.Count, 3)


if __name__ == "__main__":
    unittest.main()
