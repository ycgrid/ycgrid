# SPDX-License-Identifier: Apache-2.0
"""Excel parity of objects (shapes, pictures, lines), notes, links, protection, views, page setup and bars, each checked
the way a person does it. Every test fails without its fix.

Run: python -m unittest tests.test_parity_insert (with the bundled Python).
"""
from __future__ import annotations

import os
import struct
import sys
import tempfile
import time
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


def write_png(path, w, h, rgb=(200, 40, 40)):
    """A plain w x h RGB PNG (standard library only)."""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) +
                chunk(b"IEND", b""))


def read_png(path):
    """(width, height, pixel(x, y) -> (r, g, b)) of an 8-bit RGB / RGBA PNG."""
    with open(path, "rb") as fh:
        data = fh.read()
    pos, idat, w, h, ctype = 8, b"", 0, 0, 2
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        c = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", c[:10])
        elif kind == b"IDAT":
            idat += c
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, cc = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - cc), abs(a - cc), abs(a + b - 2 * cc)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else cc)) & 255
        rows.append(line)
        prev = line
    return w, h, lambda x, y: tuple(rows[y][x * bpp:x * bpp + 3])


class InsertParityBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        cls.tmp = tempfile.mkdtemp(prefix="yc_parity_insert_")

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

    def screen(self, x_pt, y_pt):
        """Sheet points -> the window pixels ui.input takes (the sheet scrolled to A1)."""
        from yc import ui

        a, b = ui.cell_rect("A1"), ui.cell_rect("B2")
        kx = (b[0] - a[0]) / self.ws.Range("A1").Width
        ky = (b[1] - a[1]) / self.ws.Range("A1").Height
        return int(round(a[0] + x_pt * kx)), int(round(a[1] + y_pt * ky))

    def center(self, sp):
        return self.screen(sp.Left + sp.Width / 2, sp.Top + sp.Height / 2)

    def click_shape(self, sp, extra=""):
        x, y = self.center(sp)
        self.keys(f"m:{x},{y}|w:2|{extra}c:{x},{y}|w:4")

    def selected(self):
        return list(self.conn.request("shape.selection", {}, timeout=10) or [])

    def cell(self, addr):
        from yc import ui

        r = ui.cell_rect(addr)
        return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)

    def ribbon(self, cmd):
        from yc import ui

        for it in ui.ribbon_items():
            if it["cmd"] == cmd:
                r = it["rect"]
                return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        return None

    def click_tab(self, label):
        from yc import ui

        tabs = ui.state()["ribbon"]["tabs"]
        self.assertIn(label, tabs)
        x, y = tabs[label]
        self.keys(f"m:{int(x)},{int(y)}|w:2|c:{int(x)},{int(y)}|w:4")

    def active(self):
        from yc import excel

        return excel.ActiveCell.Address.replace("$", "")


class ShapeKeysTest(InsertParityBase):
    def test_arrows_move_and_size_a_shape(self):
        # H1: the arrows did nothing to a shape (only charts and controls were looked up)
        sp = self.ws.Shapes.AddShape(1, 100, 40, 120, 60)
        self.click_shape(sp)
        self.assertEqual(self.selected(), [sp.Name])
        # a screen pixel a press, Ctrl too; Shift+Down 10% lower about the center (Excel 16, measured 2026-11-28)
        from yc import ui

        x0, _, x1, _ = ui.cell_rect("A1")
        px = 1.0 / ((x1 - x0) / self.ws.Range("A1").Width)
        self.keys("k:RightArrow|w:2|k:RightArrow|w:4")
        self.assertAlmostEqual(sp.Left, 100 + 2 * px, delta=0.02)
        self.keys("k:s-DownArrow|w:4")
        self.assertAlmostEqual(sp.Height, 60 / 1.1, delta=0.02)
        self.keys("k:c-LeftArrow|w:4")
        self.assertAlmostEqual(sp.Left, 100 + px, delta=0.02)

    def test_ctrl_keys_act_on_the_shape_not_the_cells(self):
        # H2: Ctrl+D filled the hidden cell down, Ctrl+B bolded it, Ctrl+1 opened Format Cells; objects couldn't be copied
        from yc import ui

        self.ws.Range("A1").Value = 5
        self.ws.Range("A2").Value = 7
        sp = self.ws.Shapes.AddShape(1, 100, 40, 120, 60)
        self.keys("g:A1:A2|w:2")
        self.click_shape(sp)
        self.keys("k:c-D|w:5")
        self.assertEqual(self.ws.Shapes.Count, 2)
        self.assertEqual(self.ws.Range("A2").Value, 7)
        dup = self.ws.Shapes(2)
        self.assertAlmostEqual(dup.Left, 107.5, delta=0.01)
        # the duplicate is selected; Ctrl+C, Ctrl+V: a third one, a step further
        self.keys("k:c-C|w:3|k:c-V|w:5")
        self.assertEqual(self.ws.Shapes.Count, 3)
        self.assertAlmostEqual(self.ws.Shapes(3).Left, 115, delta=0.01)
        self.assertAlmostEqual(self.ws.Shapes(3).Top, 55, delta=0.01)
        self.keys("k:c-B|w:4")
        self.assertFalse(self.ws.Range("A1").Font.Bold)
        self.assertTrue(self.ws.Shapes(3).TextFrame.Characters().Font.Bold)
        self.keys("k:c-1|w:6")
        self.assertTrue(any(w["label"].startswith("Format Shape") for w in ui.windows()))
        # the copy is pasted at the active cell once a cell is clicked
        x, y = self.cell("H10")
        self.keys(f"m:2,2|w:2|c:{x},{y}|w:3|k:c-V|w:5")
        self.assertEqual(self.ws.Shapes.Count, 4)
        self.assertAlmostEqual(self.ws.Shapes(4).Left, self.ws.Range("H10").Left, delta=0.1)
        # Cut: the shape goes at once
        self.click_shape(self.ws.Shapes(4))
        self.keys("k:c-X|w:5")
        self.assertEqual(self.ws.Shapes.Count, 3)

    def test_python_copy_paste_and_duplicate(self):
        sp = self.ws.Shapes.AddShape(1, 50, 50, 80, 40)
        d = sp.Duplicate()
        self.assertEqual(self.ws.Shapes.Count, 2)
        self.assertAlmostEqual(d.Left, 57.5, delta=0.01)
        sp.Copy()
        from yc import excel

        other = excel.Worksheets.Add()
        other.Paste()
        self.assertEqual(other.Shapes.Count, 1)

    def test_typing_on_a_shape_edits_its_text_and_escape_keeps_it(self):
        # M2: Escape threw the typed text away; typing on a selected shape went nowhere
        tb = self.ws.Shapes.AddTextbox(1, 100, 40, 160, 60)
        tb.TextFrame.Characters().Text = "abc"
        x, y = self.center(tb)
        self.keys(f"m:{x},{y}|w:2|D:{x},{y}|w:5|k:c-End|t:def|w:2|k:Escape|w:5")
        self.assertEqual(tb.TextFrame.Characters().Text, "abcdef")
        self.assertEqual(self.selected(), [tb.Name])
        self.keys("t:X|w:4|k:Escape|w:5")
        self.assertTrue(tb.TextFrame.Characters().Text.endswith("X"), tb.TextFrame.Characters().Text)

    def test_corner_drag_keeps_a_pictures_proportions(self):
        # M3: any corner drag distorted a picture; Shift was ignored
        png = os.path.join(self.tmp, "p.png")
        write_png(png, 80, 40)
        pic = self.ws.Shapes.AddPicture(png, False, True, 60, 30, 200, 100)
        self.assertEqual(pic.LockAspectRatio, -1)
        self.click_shape(pic)
        x, y = self.screen(pic.Left + pic.Width, pic.Top + pic.Height)
        x, y = x + 1, y + 1  # on the bottom-right handle
        self.keys(f"m:{x},{y}|w:2|d:{x},{y},{x + 100},{y + 10}|w:6|m:2,2|w:2")
        self.assertGreater(pic.Width, 220)
        self.assertAlmostEqual(pic.Height / pic.Width, 0.5, delta=0.01)
        # a rectangle: free without Shift, proportional with Shift
        sp = self.ws.Shapes.AddShape(1, 60, 200, 100, 50)
        self.click_shape(sp)
        x, y = self.screen(sp.Left + sp.Width, sp.Top + sp.Height)
        x, y = x + 1, y + 1
        self.keys(f"m:{x},{y}|w:2|H:shift|d:{x},{y},{x + 80},{y + 5}|h:shift|w:6|m:2,2|w:2")
        self.assertAlmostEqual(sp.Height / sp.Width, 0.5, delta=0.02)

    def test_a_line_takes_clicks_only_near_it_and_turns_by_its_end(self):
        # M4: the line's whole box caught clicks; its ends couldn't be dragged past each other
        ln = self.ws.Shapes.AddLine(100, 100, 300, 200)
        # the box's top-right corner, far from the line: the cell gets the click
        x, y = self.screen(290, 108)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        self.assertEqual(self.selected(), [])
        # on the line: selected; its end dragged to the far left turns it
        x, y = self.screen(200, 150)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        self.assertEqual(self.selected(), [ln.Name])
        ex, ey = self.screen(300, 200)
        tx, ty = self.screen(50, 250)
        self.keys(f"m:{ex},{ey}|w:2|d:{ex},{ey},{tx},{ty}|w:6|m:2,2|w:2")
        self.assertEqual(ln.HorizontalFlip, -1)
        self.assertAlmostEqual(ln.Left, 50, delta=2)
        self.assertAlmostEqual(ln.Width, 50, delta=2)


class ShapeFormatTest(InsertParityBase):
    def test_shape_format_tab_follows_the_selection(self):
        # H3: no Shape Format / Picture Format tab
        from yc import ui

        sp = self.ws.Shapes.AddShape(1, 100, 40, 120, 60)
        self.assertNotIn("Shape Format", ui.state()["ribbon"]["tabs"])
        self.click_shape(sp)
        self.assertIn("Shape Format", ui.state()["ribbon"]["tabs"])
        self.click_tab("Shape Format")
        # Shape Outline's palette (its arrow): the red standard color
        items = [i for i in ui.ribbon_items() if i["cmd"] == "ShapeOutline"]
        self.assertTrue(items)
        r = items[0]["rect"]
        ax, ay = int(r[2] - 4), int((r[1] + r[3]) / 2)
        self.keys(f"m:{ax},{ay}|w:2|c:{ax},{ay}|w:5")
        sw = sorted([i for i in ui.items("") if i["id"] == "##sw"], key=lambda i: (round(i["rect"][1]), i["rect"][0]))
        rows = sorted({round(i["rect"][1]) for i in sw})
        self.assertGreaterEqual(len(rows), 7, rows)
        standard = [i for i in sw if round(i["rect"][1]) == rows[6]]
        r = standard[1]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:5")
        self.assertEqual(sp.Line.ForeColor.RGB, 255)
        # Height box: 2 inches
        boxes = [i for i in ui.items("") if i["label"] == "Shape Height"]
        self.assertTrue(boxes, "the Height box isn't on the ribbon")
        r = boxes[0]["rect"]
        bx, by = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{bx},{by}|w:2|c:{bx},{by}|w:3|k:c-A|t:2|k:Enter|w:6")
        self.assertAlmostEqual(sp.Height, 144, delta=0.01)
        # a cell clicked: the tab goes away
        x, y = self.cell("J20")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        self.assertNotIn("Shape Format", ui.state()["ribbon"]["tabs"])
        # a picture: Picture Format, not Shape Format
        png = os.path.join(self.tmp, "q.png")
        write_png(png, 40, 40)
        pic = self.ws.Shapes.AddPicture(png, False, True, 300, 40, 60, 60)
        self.click_shape(pic)
        tabs = ui.state()["ribbon"]["tabs"]
        self.assertIn("Picture Format", tabs)
        self.assertNotIn("Shape Format", tabs)

    def test_format_fields_from_python(self):
        sp = self.ws.Shapes.AddShape(1, 10, 10, 100, 50)
        sp.Line.DashStyle = 4
        sp.Line.EndArrowheadStyle = 2
        sp.Fill.Transparency = 0.5
        sp.TextFrame2.MarginLeft = 2
        sp.TextFrame2.WordWrap = 0
        sp.AlternativeText = "logo"
        sp.Rotation = 30
        sp.IncrementRotation(15)
        sp.Flip(0)
        self.assertEqual(sp.Line.DashStyle, 4)
        self.assertEqual(sp.Line.EndArrowheadStyle, 2)
        self.assertEqual(sp.Fill.Transparency, 0.5)
        self.assertEqual(sp.TextFrame2.MarginLeft, 2)
        self.assertEqual(sp.TextFrame2.WordWrap, 0)
        self.assertEqual(sp.AlternativeText, "logo")
        self.assertEqual(sp.Rotation, 45)
        self.assertEqual(sp.HorizontalFlip, -1)
        sp.LockAspectRatio = -1
        sp.ScaleHeight(2, False)
        self.assertAlmostEqual(sp.Height, 100, delta=0.01)

    def test_rotate_and_align_and_group_from_the_tab(self):
        # M5 / M6: no rotation, one object at a time, no Align or Group
        a = self.ws.Shapes.AddShape(1, 60, 30, 60, 30)
        b = self.ws.Shapes.AddShape(1, 150, 100, 60, 30)
        c = self.ws.Shapes.AddShape(1, 240, 170, 60, 30)
        self.click_shape(a)
        self.click_shape(b, "H:shift|")
        self.click_shape(c)
        self.keys("h:shift|w:2")
        self.assertEqual(sorted(self.selected()), sorted([a.Name, b.Name, c.Name]))
        self.click_tab("Shape Format")
        from yc import ui

        ui.command("ObjAlignLeft")
        time.sleep(0.3)
        self.assertEqual({round(s.Left, 2) for s in (a, b, c)}, {60.0})
        ui.command("GroupObjects")
        time.sleep(0.3)
        self.assertNotEqual(a.GroupId, 0)
        self.assertEqual(a.GroupId, b.GroupId)
        # click one member: the group is selected; dragging it moves all three
        x, y = self.center(b)
        self.keys(f"m:2,2|w:2|c:{self.cell('M30')[0]},{self.cell('M30')[1]}|w:3|m:{x},{y}|w:2|c:{x},{y}|w:3")
        self.assertEqual(len(self.selected()), 3)
        time.sleep(0.6)  # the next press is not a double-click (which edits the shape's text)
        self.keys(f"d:{x},{y},{x + 40},{y}|w:6|m:2,2|w:2")
        self.assertTrue(all(s.Left > 70 for s in (a, b, c)), [s.Left for s in (a, b, c)])
        ui.command("RotateRight90")
        time.sleep(0.3)
        self.assertEqual(b.Rotation, 90)

    def test_selection_pane_hides_an_object(self):
        from yc import ui

        a = self.ws.Shapes.AddShape(1, 60, 30, 60, 30)
        self.ws.Shapes.AddShape(1, 150, 100, 60, 30)
        self.click_shape(a)
        ui.command("SelectionPane")
        self.keys("w:4")
        rows = [i for i in ui.items("Selection") if i["label"] == "Hide"]
        self.assertEqual(len(rows), 2)
        # the bottom row is the first shape (top of the z order first)
        r = max(rows, key=lambda i: i["rect"][1])["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        self.assertEqual(a.Visible, 0)
        ui.command("SelectionPane")

    def test_shape_text_is_drawn_bold(self):
        # M1: Bold / Italic / the font were ignored on screen (the printout used them)
        from yc import ui

        tb = self.ws.Shapes.AddTextbox(1, 40, 20, 220, 60)
        tb.TextFrame.Characters().Text = "WWWWWWWW"
        tb.TextFrame.Characters().Font.Size = 20
        x0, y0 = self.screen(40, 20)
        x1, y1 = self.screen(260, 80)

        def dark():
            p = os.path.join(self.tmp, "t.png")
            self.keys("w:3")
            ui.screenshot(p, max_width=10000)
            _, _, px = read_png(p)
            return sum(1 for yy in range(y0 + 3, y1 - 3) for xx in range(x0 + 3, x1 - 3) if sum(px(xx, yy)) < 200)

        plain = dark()
        tb.TextFrame.Characters().Font.Bold = True
        self.keys("w:4")
        bold = dark()
        self.assertGreater(bold, plain * 1.15)


class SymbolTest(InsertParityBase):
    def test_insert_symbol_by_its_code(self):
        # L1: no Insert > Symbol
        from yc import ui

        self.keys("g:A1|w:2")
        self.click_tab("Insert")
        hit = self.ribbon("InsertSymbol")
        self.assertIsNotNone(hit)
        self.keys(f"m:{hit[0]},{hit[1]}|w:2|c:{hit[0]},{hit[1]}|w:6")
        box = [i for i in ui.items("Symbol") if i["id"] == "##code"]
        self.assertTrue(box)
        r = box[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:2|k:c-A|t:25B2|w:3")
        ins = [i for i in ui.items("Symbol") if i["label"] == "Insert"][0]["rect"]
        x, y = int((ins[0] + ins[2]) / 2), int((ins[1] + ins[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:5")
        self.assertEqual(self.ws.Range("A1").Value, "▲")
        close = [i for i in ui.items("Symbol") if i["label"] == "Close"][0]["rect"]
        x, y = int((close[0] + close[2]) / 2), int((close[1] + close[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3")


class SpellingTest(InsertParityBase):
    def test_f7_checks_text_boxes(self):
        # L6: F7 checked cell text only
        from yc import ui

        tb = self.ws.Shapes.AddTextbox(1, 100, 40, 160, 60)
        tb.TextFrame.Characters().Text = "speling"
        self.keys("g:A1|w:2|k:F7|w:8")
        hits = [i for i in ui.items("") if i["label"] == "Change"]
        self.assertTrue(hits, [i["label"] for i in ui.items("")][:30])
        r = hits[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:6|k:Enter|w:4")
        self.assertEqual(tb.TextFrame.Characters().Text, "spelling")

    def test_f7_with_a_value_in_the_last_cell_is_quick(self):
        # chaos seed 6125: F7 walked every cell of the used range, and a value in XFD1048576 made that the whole
        # sheet (25 s released, the debug build stood still)
        from yc import ui

        self.ws.Range("C3").Value = "speling"
        self.ws.Range("XFD1048576").Value = 1
        t = time.time()
        self.keys("g:B2|w:2|k:F7|w:6")
        self.assertLess(time.time() - t, 5)
        hits = [i for i in ui.items("") if i["label"] == "Change"]
        self.assertTrue(hits, [i["label"] for i in ui.items("")][:30])
        self.keys("k:Escape|w:3")
        self.ws.Range("XFD1048576").ClearContents()


class ProtectionTest(InsertParityBase):
    def test_locked_shape_on_a_protected_sheet_stays_put(self):
        # H4: a protected sheet's shapes could still be dragged, sized and formatted
        from yc import ui

        sp = self.ws.Shapes.AddShape(1, 100, 40, 120, 60)
        self.ws.Protect()
        x, y = self.center(sp)
        last = max([e["id"] for e in ui.errors()] or [0])
        self.keys(f"m:{x},{y}|w:2|d:{x},{y},{x + 50},{y}|w:6|m:2,2|w:2")
        self.assertAlmostEqual(sp.Left, 100, delta=0.01)
        # the protected-sheet message (shown, and listed in the Error Log)
        self.assertTrue(any("protected sheet" in e["text"] for e in ui.errors(after=last)), ui.errors(after=last))
        self.keys("k:Escape|w:3")
        time.sleep(0.5)
        self.ws.Unprotect()
        sp.Locked = False
        self.ws.Protect()
        self.keys(f"m:{x},{y}|w:2|d:{x},{y},{x + 50},{y}|w:6|m:2,2|w:2")
        self.assertGreater(sp.Left, 120)
        self.ws.Unprotect()

    def test_tab_moves_between_unlocked_cells(self):
        # H5: Select locked cells / unlocked cells did nothing; Tab moved one column on a protected sheet
        self.ws.Range("B2").Locked = False
        self.ws.Range("D5").Locked = False
        self.ws.Protect()
        self.ws.EnableSelection = 1  # xlUnlockedCells
        self.assertEqual(self.ws.EnableSelection, 1)
        self.keys("g:B2|w:3|k:Tab|w:3")
        self.assertEqual(self.active(), "D5")
        x, y = self.cell("C3")
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3")
        self.assertEqual(self.active(), "D5")
        self.keys("k:s-Tab|w:3")
        self.assertEqual(self.active(), "B2")
        # Right goes to the next unlocked cell in reading order, all the way round (Excel 16, measured 2026-11-28)
        self.keys("k:RightArrow|w:3")
        self.assertEqual(self.active(), "D5")
        self.ws.Unprotect()


class NotesLinksTest(InsertParityBase):
    def test_one_click_follows_a_link_and_a_hold_selects(self):
        # M7: links followed only with Ctrl+click
        from yc import excel

        target = excel.Worksheets.Add()
        target_name = target.Name
        self.ws.Activate()
        self.ws.Hyperlinks.Add(self.ws.Range("B2"), "", f"{target_name}!C3", "", "go")
        self.keys("w:3")
        x, y = self.cell("B2")
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        self.assertEqual(excel.ActiveSheet.Name, target_name)
        self.assertEqual(self.active(), "C3")
        self.ws.Activate()
        self.keys("w:3")
        x, y = self.cell("B2")
        self.keys(f"m:{x},{y}|w:2|p:{x},{y}|w:3")
        time.sleep(0.9)  # click and hold: past Excel's half second
        self.keys(f"u:{x},{y}|w:4")
        self.assertEqual(excel.ActiveSheet.Name, self.ws.Name)
        self.assertEqual(self.active(), "B2")
        # a web link: a hidden instance opens nothing on the desktop and says what it would have opened
        from yc import ui

        self.ws.Hyperlinks.Add(self.ws.Range("D4"), "https://example.com/2330", TextToDisplay="TSMC")
        self.keys("w:3")
        before = ui.state()["desktop_opens"]
        x, y = self.cell("D4")
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        self.assertEqual((ui.state()["desktop_opens"], ui.state()["desktop_open"]), (before + 1, "https://example.com/2330"))
        self.keys(f"m:{x},{y}|w:2|p:{x},{y}|w:3")
        time.sleep(0.9)
        self.keys(f"u:{x},{y}|w:4")
        self.assertEqual(ui.state()["desktop_opens"], before + 1, "a hold selects the cell and follows nothing")

    def test_remove_link_takes_its_look(self):
        # M8: Remove Link left blue underlined text
        r = self.ws.Range("B2")
        r.Value = "site"
        self.keys("g:B2|w:2|k:c-K|w:6|t:https://example.com|w:2|k:Enter|w:6")
        self.assertEqual(r.Hyperlinks.Count, 1)
        self.keys("g:B2|w:2|k:c-K|w:6")
        from yc import ui

        hits = [i for i in ui.items("") if i["label"] == "Remove Link"]
        self.assertTrue(hits)
        rr = hits[0]["rect"]
        x, y = int((rr[0] + rr[2]) / 2), int((rr[1] + rr[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:6")
        self.assertEqual(r.Hyperlinks.Count, 0)
        self.assertNotEqual(r.Font.Underline, 2)  # xlUnderlineStyleSingle
        self.assertEqual(r.Font.Color, 0)

    def test_link_dialog_lists_the_workbooks_folder(self):
        # L10: the page was an address box with Browse... only
        from yc import excel, ui

        folder = tempfile.mkdtemp(prefix="yc_links_")
        with open(os.path.join(folder, "notes.txt"), "w") as f:
            f.write("x")
        excel.ActiveWorkbook.SaveAs(os.path.join(folder, "book.ycg"))
        self.keys("g:B2|w:2|k:c-K|w:6")
        hits = [i for i in ui.items("") if i["label"] == "notes.txt"]
        self.assertTrue(hits, [i["label"] for i in ui.items("")][:40])
        r = hits[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:3|k:Enter|w:6")
        self.assertEqual(self.ws.Range("B2").Hyperlinks.Count, 1)
        self.assertTrue(self.ws.Range("B2").Hyperlinks(1).Address.endswith("notes.txt"))

    def test_cell_menu_follows_the_note_and_the_link(self):
        # M9: the menu always offered New Note; no Edit / Delete Note, no hyperlink items
        from yc import ui

        self.ws.Range("B3").AddComment("hello")
        x, y = self.cell("B3")
        self.keys(f"m:{x},{y}|w:2|r:{x},{y}|w:5")
        labels = [i["label"] for i in ui.items("")]
        self.assertIn("Edit Note", labels)
        self.assertIn("Delete Note", labels)
        self.assertNotIn("New Note", labels)
        hit = [i for i in ui.items("") if i["label"] == "Delete Note"][0]["rect"]
        hx, hy = int((hit[0] + hit[2]) / 2), int((hit[1] + hit[3]) / 2)
        self.keys(f"m:{hx},{hy}|w:3|c:{hx},{hy}|w:5")
        self.assertIsNone(self.ws.Range("B3").Comment)
        self.ws.Hyperlinks.Add(self.ws.Range("C3"), "https://example.com")
        x, y = self.cell("C3")
        self.keys(f"m:{x},{y}|w:2|r:{x},{y}|w:5")
        labels = [i["label"] for i in ui.items("")]
        self.assertIn("Open Hyperlink", labels)
        self.assertIn("Remove Hyperlink", labels)

    def test_comment_card_opens_beside_its_cell(self):
        # M11: the comment window opened at a fixed place
        from yc import ui

        self.keys("g:H20|w:3|k:c-a-M|w:8")
        wins = [w for w in ui.windows() if w["label"].startswith("Comment")]
        self.assertTrue(wins, [w["label"] for w in ui.windows()])
        r = ui.cell_rect("H20")
        wl, wt = wins[0]["rect"][0], wins[0]["rect"][1]
        self.assertGreaterEqual(wl, r[2] - 2)
        self.assertLess(wl - r[2], 40)
        self.assertLess(abs(wt - r[1]), 40)
        self.keys("k:Escape|w:3")

    def test_sparklines_from_blank_cells_fill_the_location(self):
        # M10: the selection always went to Data Range and the location was guessed
        from yc import excel

        for row in range(2, 5):
            for col in "BCDE":
                self.ws.Range(f"{col}{row}").Value = row * 3 + ord(col) % 5
        self.keys("g:F2:F4|w:3")
        from yc import ui

        ui.command("InsertSparkLine")
        self.keys("w:6|t:B2:E4|w:2|k:Enter|w:6")
        self.assertEqual(self.ws.Range("F2:F4").SparklineGroups.Count, 1)
        self.assertEqual(excel.ActiveSheet.Name, self.ws.Name)


class ViewBarsTest(InsertParityBase):
    def test_keytips_reach_the_drop_down_entries(self):
        # M14: a KeyTip sequence ended when the drop-down opened (Alt, W, F, F = Freeze Panes)
        from yc import excel

        self.keys("g:B3|w:3|k:LeftAlt|w:3|t:W|w:3|t:F|w:4|t:F|w:6")
        self.assertTrue(excel.ActiveWindow.FreezePanes)
        excel.ActiveWindow.FreezePanes = False
        # Alt, P, R, S: Set Print Area
        self.keys("g:B2:D9|w:3|k:LeftAlt|w:3|t:P|w:3|t:R|w:4|t:S|w:6")
        self.assertEqual(self.ws.PageSetup.PrintArea, "$B$2:$D$9")

    def test_status_bar_sums_in_the_cells_format(self):
        # M15: the statistics ignored the number format (Sum: 0.520833333333333)
        from yc import ui

        self.ws.Range("A1").Value = "12:00"
        self.ws.Range("A2").Value = "0:30"
        self.keys("g:A1:A2|w:4")
        self.assertIn("Sum: 12:30", ui.state()["status_stats"])
        self.ws.Range("B1").Value = 1000
        self.ws.Range("B2").Value = 234.5
        self.ws.Range("B1:B2").NumberFormat = "$#,##0.00"
        self.keys("g:B1:B2|w:4")
        self.assertIn("Sum: $1,234.50", ui.state()["status_stats"])

    def test_view_buttons_show_the_current_view(self):
        # L2: the view buttons weren't toggles; Zoom to Selection wasn't on the ribbon
        from yc import ui

        self.click_tab("View")
        self.assertIsNotNone(self.ribbon("ZoomToSelection"))
        ui.command("ViewPageLayout")
        self.keys("w:4")
        cmds = {c["id"]: c for c in ui.commands()}
        self.assertTrue(cmds["ViewPageLayout"]["pressed"])
        self.assertFalse(cmds["ViewNormal"]["pressed"])
        ui.command("ViewNormal")
        self.keys("w:4")
        cmds = {c["id"]: c for c in ui.commands()}
        self.assertTrue(cmds["ViewNormal"]["pressed"])

    def test_an_inactive_colored_tab_is_filled(self):
        # L4: an inactive colored tab showed a 4 px strip only
        from yc import excel, ui

        self.ws.Tab.Color = 0x0000FF  # red (Excel's BGR)
        other = excel.Worksheets.Add()
        other.Activate()
        self.keys("w:4")
        tab = [i for i in ui.items("") if i["id"] == self.ws.Name + "##sheettab"]
        self.assertTrue(tab)
        r = tab[0]["rect"]
        p = os.path.join(self.tmp, "tabs.png")
        ui.screenshot(p, max_width=10000)
        _, _, px = read_png(p)
        x, y = int(r[2] - 4), int(r[1] + 4)  # the tab's top-right, away from its text and the old strip
        red, green, blue = px(x, y)
        self.assertGreater(red, 180)
        self.assertLess(green, 80)

    def test_shift_f11_inserts_before_the_active_sheet(self):
        # L3: the new sheet went after the active one
        from yc import excel

        names = [s.Name for s in excel.Worksheets]
        i = names.index(self.ws.Name)
        self.keys("k:s-F11|w:6")
        after = [s.Name for s in excel.Worksheets]
        self.assertEqual(len(after), len(names) + 1)
        self.assertEqual(after[i + 1], self.ws.Name)
        self.assertEqual(excel.ActiveSheet.Name, after[i])


class PageSetupTest(InsertParityBase):
    def test_first_page_header(self):
        # M13: no different first page / odd and even headers
        ps = self.ws.PageSetup
        ps.CenterHeader = "Every page"
        ps.DifferentFirstPageHeaderFooter = True
        ps.FirstPage.CenterHeader.Text = "Cover"
        self.assertTrue(ps.DifferentFirstPageHeaderFooter)
        self.assertEqual(ps.FirstPage.CenterHeader.Text, "Cover")
        self.assertEqual(ps.CenterHeader, "Every page")
        ps.OddAndEvenPagesHeaderFooter = True
        ps.EvenPage.CenterFooter.Text = "even"
        self.assertEqual(ps.EvenPage.CenterFooter.Text, "even")
        self.assertTrue(ps.ScaleWithDocHeaderFooter)
        ps.AlignMarginsHeaderFooter = False
        self.assertFalse(ps.AlignMarginsHeaderFooter)

    def test_header_footer_tab_check_boxes(self):
        from yc import ui

        ui.command("PageSetupDialog")
        self.keys("w:6")
        tabs = [i for i in ui.items("") if i["label"] == "Header/Footer"]
        self.assertTrue(tabs)
        r = tabs[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        boxes = [i for i in ui.items("") if i["label"] == "Different first page"]
        self.assertTrue(boxes)
        r = boxes[0]["rect"]
        x, y = int(r[0] + 8), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4")
        ok = [i for i in ui.items("") if i["label"] == "OK"]
        r = ok[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:5")
        self.assertTrue(self.ws.PageSetup.DifferentFirstPageHeaderFooter)


if __name__ == "__main__":
    unittest.main()
