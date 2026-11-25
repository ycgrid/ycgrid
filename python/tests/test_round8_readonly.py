# SPDX-License-Identifier: Apache-2.0
"""Round 8: read-only mode (Trading > Operator Mode off) refuses every change the user makes, quietly, with the status
bar's message: sheets, protection, rules, notes, Undo; the dialogs of such commands don't open, and one left open
when read-only mode came on closes without a wrong or blank message box. Python's writes are not affected.

Run: python -m unittest tests.test_round8_readonly (with the bundled Python).
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


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ReadOnlyRefusesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel, safety

        safety.set_operator_mode(True)
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        try:
            excel.Application.ActiveWorkbook.Unprotect()
        except Exception:  # noqa: BLE001 - not protected
            pass
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()
        self.ws.Range("A1:C6").Value = [["Name", "Qty", "Px"], ["a", 1, 10.5], ["b", 2, 11], ["a", 1, 10.5], ["c", 3, 12], ["d", 4, 13]]
        self.keys("w:3")
        self.errors0 = len(yc.ui.errors())

    def tearDown(self):
        from yc import safety

        safety.set_operator_mode(True)
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def read_only(self):
        from yc import safety

        safety.set_operator_mode(False)
        self.keys("w:3")

    def messages(self):
        """Message boxes and errors shown since setUp (a hidden instance answers them and logs them)."""
        return [e.get("text", "") for e in yc.ui.errors()[self.errors0:]]

    def assert_quiet(self):
        self.assertEqual(self.messages(), [], "read-only mode showed a message box instead of the status bar")
        self.assertEqual(yc.ui.state()["dialog"], 0, "a dialog stayed open")
        self.assertIn("Read-only", yc.ui.state()["status"])

    def test_sheets_stay_as_they_are(self):
        from yc import excel

        wb = excel.Application.ActiveWorkbook
        names = [s.Name for s in wb.Worksheets]
        name = self.ws.Name
        self.read_only()
        self.keys("k:s-F11|w:4")  # Insert Sheet added one
        self.keys("x:DeleteSheet|w:4")  # the tab menu's Delete deleted it without a message
        self.keys("x:RenameSheet|w:4|t:ZZ|k:Enter|w:4")  # the tab's name box opened and renamed it
        self.keys("x:HideSheet|w:4")
        self.keys("x:TabColor|w:6")  # opened its color dialog
        self.assert_quiet()
        self.assertEqual([s.Name for s in wb.Worksheets], names)
        self.assertTrue(wb.Worksheets(name).Visible)
        # Python's writes are not affected by read-only mode
        added = excel.Worksheets.Add()
        self.assertEqual(wb.Worksheets.Count, len(names) + 1)
        added.Delete()

    def test_protection_rules_and_formats_open_no_dialog(self):
        from yc import excel

        wb = excel.Application.ActiveWorkbook
        self.read_only()
        self.keys("g:A1|x:ProtectSheet|w:8|k:Enter|w:6")
        self.keys("x:ProtectWorkbook|w:8|k:Enter|w:6")
        self.keys("g:B2:B6|x:CfGreater|w:8|k:Enter|w:6")
        self.keys("g:B2:B6|x:CfBarBlue|w:6")
        self.keys("g:A1|k:c-1|w:6")  # Format Cells opened and refused only at OK
        self.keys("x:DataValidation|w:6")
        self.assert_quiet()
        self.assertFalse(self.ws.ProtectContents)
        self.assertFalse(wb.ProtectStructure)
        self.assertEqual(self.ws.Range("B2:B6").FormatConditions.Count, 0)
        excel.Worksheets.Add().Delete()  # Python's Worksheets.Add raised ProtectedError after the workbook was protected

    def test_notes_dont_open_and_one_being_typed_closes_unsaved(self):
        from yc import safety

        self.read_only()
        self.keys("g:B2|k:s-F2|w:8|t:hello|w:3|A:H12|w:6")
        self.assertIsNone(self.ws.Range("B2").Comment, "Shift+F2 made a note in read-only mode")
        self.assert_quiet()
        safety.set_operator_mode(True)
        self.keys("w:3|g:B3|k:s-F2|w:8|t:abc|w:3")
        self.read_only()
        self.keys("t:zz|w:3|A:H12|w:6")
        self.assertIsNone(self.ws.Range("B3").Comment, "the note box open when read-only mode came on took the typing and saved it")
        self.assertIsNone(self.ws.Range("H12").Value2)

    def test_undo_puts_back_no_earlier_edit(self):
        self.keys("A:E5|w:3|t:42|k:Enter|w:4")
        self.assertEqual(self.ws.Range("E5").Value2, 42)
        self.read_only()
        self.keys("k:c-Z|w:4|k:c-Y|w:4")
        self.assertEqual(self.ws.Range("E5").Value2, 42)
        self.assert_quiet()

    def test_a_dialog_open_when_read_only_mode_comes_on_closes_without_a_wrong_message(self):
        # Remove Duplicates said "No duplicate values found.", Subtotal a blank message and stayed open, Replace All
        # "We couldn't find anything to replace.", Advanced Filter "The filter couldn't be applied."
        from yc import safety

        for cmd in ("RemoveDuplicates", "Subtotal", "AdvancedFilter", "InsertTable"):
            safety.set_operator_mode(True)
            self.keys(f"k:Escape|w:2|g:A1:C6|w:2|x:{cmd}|w:8")
            self.assertNotEqual(yc.ui.state()["dialog"], 0, f"{cmd} opened no dialog in operator mode")
            self.read_only()
            self.keys("k:Enter|w:8")
            self.assertEqual(self.messages(), [], cmd)
            self.assertEqual(yc.ui.state()["dialog"], 0, f"{cmd}'s dialog stayed open")
        self.assertEqual([self.ws.Range(f"A{i}").Value2 for i in range(1, 8)], ["Name", "a", "b", "a", "c", "d", None])
        self.assertEqual(self.ws.ListObjects.Count, 0)

    def test_replace_all_says_read_only_not_nothing_found(self):
        from yc import ui

        self.keys("g:A1|k:c-H|w:8|t:a|k:Tab|w:2|t:zz|w:2")
        self.read_only()
        items = [i for i in ui.items() if i["label"] == "Replace All"]
        self.assertTrue(items, "the Replace tab is open")
        r = items[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        self.assertEqual(self.messages(), [])
        self.assertIn("Read-only", ui.state()["status"])
        self.assertEqual(self.ws.Range("A2").Value2, "a")


if __name__ == "__main__":
    unittest.main()
