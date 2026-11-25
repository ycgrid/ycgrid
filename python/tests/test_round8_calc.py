# SPDX-License-Identifier: Apache-2.0
"""Round 8: calculation through the object model, measured against Excel 16 (Range.Calculate calculates the range
only, Worksheet.Calculate leaves what read a waiting cell waiting, turning Iteration off keeps the cycle's values, the
cells of an array formula read its formula and part of it can't be changed).

Run: python -m unittest tests.test_round8_calc (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class CalculationRound8Test(unittest.TestCase):
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
        excel.Application.Iteration = False
        excel.Application.MaxIterations = 100
        excel.Application.MaxChange = 0.001
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()

    def tearDown(self):
        from yc import excel

        excel.Application.Calculation = excel.xlCalculationAutomatic
        excel.Application.Iteration = False

    def v(self, ref):
        return self.ws.Range(ref).Value2

    def test_range_calculate_calculates_the_range_only(self):
        from yc import excel

        ws = self.ws
        ws.Range("A1").Value = 1
        ws.Range("B1").Formula = "=A1*2"
        ws.Range("C1").Formula = "=A1*3"
        ws.Range("D1").Formula = "=B1+1"
        excel.Application.Calculation = excel.xlCalculationManual
        ws.Range("A1").Value = 5
        ws.Range("B1").Calculate()
        self.assertEqual([self.v(x) for x in ("B1", "C1", "D1")], [10, 3, 3])  # C1 and D1 became 15 and 11
        excel.Application.Calculate()
        self.assertEqual([self.v(x) for x in ("C1", "D1")], [15, 11])

    def test_a_formula_a_sheet_calculation_read_from_a_waiting_cell_is_calculated_by_f9(self):
        from yc import excel

        ws = self.ws
        other = excel.Worksheets.Add(After=ws)
        ws.Range("A1").Value = 1
        ws.Range("B1").Formula = "=A1*2"
        other.Range("A1").Formula = "='" + ws.Name + "'!B1+1"
        excel.Application.Calculation = excel.xlCalculationManual
        ws.Range("A1").Value = 5
        other.Calculate()
        self.assertEqual(other.Range("A1").Value2, 3)
        excel.Application.Calculate()
        self.assertEqual(other.Range("A1").Value2, 11)  # it stayed 3

    def test_turning_iteration_off_keeps_the_values(self):
        from yc import excel

        ws = self.ws
        excel.Application.Iteration = True
        ws.Range("A1").Value = 1000
        ws.Range("B1").Formula = "=A1+C1"
        ws.Range("C1").Formula = "=B1*0.05"
        b, c = self.v("B1"), self.v("C1")
        self.assertGreater(b, 1052)
        excel.Application.Iteration = False  # B1 became 0 and C1 50
        self.assertEqual((self.v("B1"), self.v("C1")), (b, c))

    def test_iteration_goes_on_at_every_calculation(self):
        from yc import excel

        ws = self.ws
        excel.Application.Iteration = True
        excel.Application.MaxIterations = 1
        ws.Range("A1").Formula = "=A1+1"
        first = self.v("A1")
        excel.Application.Calculate()
        self.assertEqual(self.v("A1"), first + 1)  # it stayed
        ws.Range("Z1").Value = 1
        self.assertEqual(self.v("A1"), first + 2)

    def test_array_formula_cells_read_its_formula_and_cant_be_changed_in_part(self):
        ws = self.ws
        ws.Range("A1:A3").Value = [[1], [2], [3]]
        ws.Range("C1:C3").FormulaArray = "=A1:A3*10"
        self.assertEqual(ws.Range("C2").Formula, "=A1:A3*10")  # it read 20
        self.assertEqual(ws.Range("C2").Formula2, "=A1:A3*10")
        self.assertEqual(ws.Range("C1:C3").Formula, [["=A1:A3*10"], ["=A1:A3*10"], ["=A1:A3*10"]])
        with self.assertRaises(yc.YcError):
            ws.Range("C2").ClearContents()
        with self.assertRaises(yc.YcError):
            ws.Range("2:2").Delete()
        with self.assertRaises(yc.YcError):
            ws.Range("2:2").Insert()
        try:
            ws.Range("C2").Value2 = 7  # Excel raises nothing here but changes nothing
        except yc.YcError:
            pass
        self.assertEqual([self.v(x) for x in ("C1", "C2", "C3")], [10, 20, 30])
        self.assertEqual(ws.Range("C1").FormulaArray, "=A1:A3*10")
        ws.Range("C1:C3").ClearContents()  # the whole array
        self.assertIsNone(self.v("C2"))


if __name__ == "__main__":
    unittest.main()
