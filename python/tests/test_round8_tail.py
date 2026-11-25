# SPDX-License-Identifier: Apache-2.0
"""Round 8, the last differences: Range("C1#"), Workbook members that don't exist, CalculationState, Range.Dirty,
a value written to part of an array formula, an arrow pressed right after F2; each test failing without its fix.

Run: python -m unittest tests.test_round8_tail (with the bundled Python).
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
class TailRound8Test(unittest.TestCase):
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

        excel.Application.Calculation = excel.xlCalculationAutomatic
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()

    def tearDown(self):
        from yc import excel

        excel.Application.Calculation = excel.xlCalculationAutomatic
        self.keys("k:Escape|w:2|m:2,2|w:2")

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def test_a_spill_reference_names_the_spill_range(self):
        from yc.errors import ApplicationDefinedError

        self.ws.Range("C1").Formula2 = "=SEQUENCE(3)"
        self.assertEqual(self.ws.Range("C1#").Address, "$C$1:$C$3")  # Excel; it raised "Invalid address or name"
        self.assertEqual(sum(r[0] for r in self.ws.Range("C1#").Value2), 6)
        # a dynamic array whose result is one value: its spill range is the cell (Excel 16, measured); it raised
        self.ws.Range("E1").Formula2 = "=DROP({1;2;3},2)"
        self.assertEqual(self.ws.Range("E1#").Address, "$E$1")
        self.assertEqual(self.ws.Range("E1#").Value2, 3)
        self.ws.Range("F1").Formula2 = "=E1#"
        self.assertEqual(self.ws.Range("F1").Value2, 3)
        self.ws.Range("E2").Value2 = 7  # a constant has none, as in Excel (=E2# is #REF!)
        with self.assertRaises(ApplicationDefinedError):
            self.ws.Range("E2#")
        with self.assertRaises(ApplicationDefinedError):
            self.ws.Range("D1#")

    def test_save_copy_as_follows_the_extension(self):
        # SaveCopyAs("x.xlsx") wrote a .ycg (header YCG1) under the .xlsx name; Save As to .xlsx exports a copy
        import tempfile

        from yc import excel

        self.ws.Range("A1").Value2 = 1
        folder = tempfile.mkdtemp(prefix="yc_r8_")
        for name, head in (("copy.ycg", b"YCG1"), ("copy.xlsx", b"PK"), ("copy.csv", b"\xef\xbb\xbf1")):  # CSV UTF-8 with its BOM
            path = os.path.join(folder, name)
            excel.ActiveWorkbook.SaveCopyAs(path)
            with open(path, "rb") as fh:
                self.assertEqual(fh.read(len(head)), head, name)
        self.assertNotEqual(os.path.basename(excel.ActiveWorkbook.FullName), "copy.xlsx")  # a copy: the workbook keeps its file

    def test_a_workbook_member_that_doesnt_exist_raises(self):
        from yc import excel

        with self.assertRaises(AttributeError):
            excel.ActiveWorkbook.PrecisionAsDisplay = True  # a misspelled member was kept silently and did nothing
        excel.ActiveWorkbook.Saved = excel.ActiveWorkbook.Saved  # a real member still works

    def test_calculation_state_and_dirty(self):
        from yc import excel

        self.ws.Range("A1").Value2 = 1
        self.ws.Range("B1").Formula = "=A1*2"
        self.assertEqual(excel.Application.CalculationState, 0)  # xlDone
        excel.Application.Calculation = excel.xlCalculationManual
        self.ws.Range("A1").Value2 = 5
        self.assertEqual(excel.Application.CalculationState, 2)  # xlPending: B1 waits
        excel.Application.Calculate()
        self.assertEqual(excel.Application.CalculationState, 0)
        self.assertEqual(self.ws.Range("B1").Value2, 10)
        self.ws.Range("B1").Dirty()
        self.assertEqual(excel.Application.CalculationState, 2)
        excel.Application.Calculate()
        self.assertEqual(excel.Application.CalculationState, 0)

    def test_a_value_written_to_part_of_an_array_is_ignored_without_an_error(self):
        from yc.errors import ProtectedError

        self.ws.Range("A1:A3").Value = [[1], [2], [3]]
        self.ws.Range("C1:C3").FormulaArray = "=A1:A3*10"
        self.ws.Range("C2").Value2 = 7  # Excel: no error, nothing changes
        self.assertEqual(self.ws.Range("C2").Value2, 20)
        with self.assertRaises(ProtectedError):
            self.ws.Range("C2").ClearContents()  # Excel: "You can't change part of an array"
        self.assertEqual(self.ws.Range("C2").Value2, 20)

    def test_an_arrow_right_after_f2_moves_the_caret(self):
        self.ws.Range("A1").Value2 = "hello"
        self.keys("g:A1|w:2|k:F2|k:Left|t:X|k:Enter|w:4")  # Left in the frame the box takes the keyboard
        self.assertEqual(self.ws.Range("A1").Value2, "hellXo")


    def test_paste_list_shows_a_union_name_as_typed(self):
        from yc import excel, ui

        self.ws.Name = "Un_" + str(int(time.time() * 1000) % 100000)  # not a cell address: written bare
        excel.ActiveWorkbook.Names.Add("UnionTail", "=%s!$A$1,%s!$A$3" % (self.ws.Name, self.ws.Name))
        try:
            self.keys("g:E1|w:2|k:F3|w:5")
            b = [i for i in ui.items("Paste Name") if i["label"] == "Paste List"][0]["rect"]
            x, y = int((b[0] + b[2]) / 2), int((b[1] + b[3]) / 2)
            self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
            got = {self.ws.Cells(r, 5).Value2: self.ws.Cells(r, 6).Value2 for r in range(1, 20) if self.ws.Cells(r, 5).Value2}
            # the kept ((...)) pairs were pasted: Excel lists the name as it was typed
            self.assertEqual(got.get("UnionTail"), "=%s!$A$1,%s!$A$3" % (self.ws.Name, self.ws.Name))
        finally:
            excel.ActiveWorkbook.Names("UnionTail").Delete()


if __name__ == "__main__":
    unittest.main()
