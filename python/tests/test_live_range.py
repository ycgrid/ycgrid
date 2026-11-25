# SPDX-License-Identifier: Apache-2.0
"""Range and defined-name integration tests: need a running YCGrid (blank workbook).

    set YCGRID_LIVE_PID=<pid of ycgrid.exe>
    runtime\\python\\python.exe -m unittest tests.test_live_range

Skipped when YCGRID_LIVE_PID is not set. Changes the active workbook (adds a test sheet).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

PID = int(os.environ.get("YCGRID_LIVE_PID", "0") or 0)


@unittest.skipUnless(PID, "needs a running YCGrid (set YCGRID_LIVE_PID)")
class LiveRange(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import yc

        yc.connect(PID)
        from yc import excel

        cls.v = excel
        cls.ws = excel.Worksheets.Add()
        cls.ws.Name = "範圍測試"
        ws = cls.ws
        ws.Range("A1:C1").Value = [["代碼", "價格", "數量"]]
        ws.Range("A2:C5").Value = [["'2330", 600, 1], ["'2317", 100, 2], ["'2330", 600, 1], ["'2454", 900, 3]]
        ws.Range("E1").Value = 0.05

    @classmethod
    def tearDownClass(cls):
        import yc

        for n in list(cls.v.ThisWorkbook.Names):
            n.Delete()
        cls.v.Application.DisplayAlerts = False
        cls.ws.Delete()
        yc.disconnect()

    def test_names_add_and_formula(self):
        v, ws = self.v, self.ws
        n = v.ThisWorkbook.Names.Add(Name="稅率", RefersTo=ws.Range("E1"), Comment="營業稅")
        self.assertEqual(n.RefersTo, "=範圍測試!$E$1")
        self.assertEqual(n.Comment, "營業稅")
        self.assertEqual(n.RefersToRange.Address, "$E$1")
        ws.Range("F1").Formula = "=稅率*100"
        self.assertEqual(ws.Range("F1").Value, 5)
        # renaming: formulas follow
        n.Name = "營業稅率"
        self.assertEqual(ws.Range("F1").Formula, "=營業稅率*100")
        self.assertIn("營業稅率", v.Names)
        # constant name
        v.Names.Add("倍數", "=3")
        ws.Range("F2").Formula = "=倍數*2"
        self.assertEqual(ws.Range("F2").Value, 6)
        v.Names("倍數").Delete()
        self.assertNotIn("倍數", v.Names)

    def test_range_name_and_lookup(self):
        v, ws = self.v, self.ws
        ws.Range("B2:B5").Name = "價格"
        self.assertEqual(ws.Range("價格").Address, "$B$2:$B$5")
        self.assertEqual(ws.Range("B2:B5").Name.Name, "價格")
        self.assertEqual(v.WorksheetFunction.Sum(ws.Range("價格")), 2200)
        # sheet-level name
        ws.Names.Add(Name="本地", RefersTo="=$C$2:$C$5")
        self.assertEqual(ws.Range("本地").Address, "$C$2:$C$5")
        self.assertEqual(ws.Names.Count, 1)
        self.assertTrue(v.Names("範圍測試!本地").Name.endswith("!本地"))
        # dynamic name
        v.Names.Add("動態", "=OFFSET(範圍測試!$A$1,0,0,COUNTA(範圍測試!$A:$A),1)")
        self.assertEqual(ws.Range("動態").Address, "$A$1:$A$5")
        ws.Range("A6").Value = "1101"
        self.assertEqual(ws.Range("動態").Address, "$A$1:$A$6")
        ws.Range("A6").ClearContents()

    def test_union_intersection(self):
        ws = self.ws
        self.assertEqual(ws.Range("A1,C3:D4").Areas.Count, 2)
        self.assertEqual(ws.Range("A1:C5 B2:D3").Address, "$B$2:$C$3")

    def test_r1c1(self):
        ws = self.ws
        ws.Range("D2").Formula = "=B2*C2"
        self.assertEqual(ws.Range("D2").FormulaR1C1, "=RC[-2]*RC[-1]")
        ws.Range("D2:D5").FormulaR1C1 = "=RC[-2]*RC[-1]"
        self.assertEqual(ws.Range("D5").Formula, "=B5*C5")
        self.assertEqual(ws.Range("D5").Value, 2700)
        self.assertEqual(ws.Range("B2:C3").Address(ReferenceStyle=self.v.xlR1C1), "R2C2:R3C3")
        self.assertEqual(ws.Range("B2").Address(False, False, self.v.xlR1C1, RelativeTo=ws.Range("A1")), "R[1]C[1]")

    def test_special_cells_and_trace(self):
        v, ws = self.v, self.ws
        ws.Range("D2:D5").Formula = "=B2*C2"
        f = ws.Range("A1:D5").SpecialCells(v.xlCellTypeFormulas)
        self.assertEqual(f.Address, "$D$2:$D$5")
        nums = ws.Range("A1:D5").SpecialCells(v.xlCellTypeConstants, v.xlNumbers)
        self.assertEqual(nums.Address, "$B$2:$C$5")
        self.assertEqual(ws.Range("D2").DirectPrecedents.Address, "$B$2:$C$2")  # one block, as Excel joins touching cells
        self.assertEqual(ws.Range("B3").Dependents.Address, "$D$3")
        with self.assertRaises(Exception):
            ws.Range("H1:H3").SpecialCells(v.xlCellTypeFormulas)

    def test_find_next_previous(self):
        ws = self.ws
        first = ws.Range("A1:A5").Find("2330")
        self.assertEqual(first.Address, "$A$2")
        nxt = ws.Range("A1:A5").FindNext(first)
        self.assertEqual(nxt.Address, "$A$4")
        self.assertEqual(ws.Range("A1:A5").FindNext(nxt).Address, "$A$2")
        self.assertEqual(ws.Range("A1:A5").FindPrevious(first).Address, "$A$4")

    def test_remove_duplicates(self):
        ws = self.ws
        ws.Range("H1:I5").Value = [["代碼", "價格"], ["2330", 600], ["2317", 100], ["2330", 600], ["2454", 900]]
        removed = ws.Range("H1:I5").RemoveDuplicates(Columns=[1, 2], Header=self.v.xlYes)
        self.assertEqual(removed, 1)
        self.assertEqual(ws.Range("H4").Text, "2454")
        self.assertIsNone(ws.Range("H5").Value)


if __name__ == "__main__":
    unittest.main()
