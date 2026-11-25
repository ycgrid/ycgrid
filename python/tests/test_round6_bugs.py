# SPDX-License-Identifier: Apache-2.0
"""Round 6: bugs found in typing, filling, moving and pasting cells, each test failing without its fix.

Run: python -m unittest tests.test_round6_bugs (with the bundled Python).
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
class CellsRound6Test(unittest.TestCase):
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

        excel.Application.CutCopyMode = False
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)

    def test_text_cells_keep_what_is_typed(self):
        ws = self.ws
        ws.Range("A1:A4").NumberFormat = "@"
        self.keys("k:Escape|m:2,2|g:A1|w:2|t:00123|k:Enter|w:3|g:A2|w:2|t:=1+1|k:Enter|w:3")
        self.assertEqual(ws.Range("A1").Value, "00123")
        self.assertEqual(ws.Range("A2").Value, "=1+1")
        self.assertFalse(ws.Range("A2").HasFormula)
        ws.Range("A3").Value = "10"  # code writes as typing does
        self.assertEqual(ws.Range("A3").Value, "10")
        ws.Range("A3:A4").Formula = "=B1"
        self.assertEqual(ws.Range("A3").Value, "=B1")
        self.assertEqual(ws.Range("A4").Value, "=B1")
        ws.Range("B5").Value = "10"  # a General cell still reads the number
        self.assertEqual(ws.Range("B5").Value, 10)

    def test_paste_special_fills_the_whole_destination(self):
        from yc.excel import xlPasteAll, xlPasteSpecialOperationDivide, xlPasteValues

        ws = self.ws
        ws.Range("H1").Value = 10
        ws.Range("G1:G3").Value = [[2], [4], [6]]
        ws.Range("H1").Copy()
        ws.Range("G1:G3").PasteSpecial(Paste=xlPasteValues, Operation=xlPasteSpecialOperationDivide)
        self.assertEqual([r[0] for r in ws.Range("G1:G3").Value], [0.2, 0.4, 0.6])
        ws.Range("N1:O1").Value = [[1, 2]]
        ws.Range("N1:O1").Copy()
        ws.Range("N3:Q4").PasteSpecial(Paste=xlPasteAll)  # a whole multiple: the copy repeats
        self.assertEqual(ws.Range("N3:Q4").Value, [[1, 2, 1, 2], [1, 2, 1, 2]])
        ws.Range("H1").Copy()
        ws.Paste(Destination=ws.Range("J1:J2"))
        self.assertEqual(ws.Range("J1:J2").Value, [[10], [10]])

    def test_fill_handle_continues_two_months(self):
        ws = self.ws
        ws.Range("C1").Value = "Jan"
        ws.Range("C2").Value = "Feb"
        ws.Range("C1:C2").AutoFill(ws.Range("C1:C5"))
        self.assertEqual([r[0] for r in ws.Range("C3:C5").Value], ["Mar", "Apr", "May"])
        self.keys("k:Escape|m:2,2|g:D1|w:2|t:9:00 p|k:Enter|w:3")
        self.assertEqual(ws.Range("D1").Text, "9:00 PM")

    def test_cut_and_paste_takes_names_rules_and_validation_along(self):
        from yc import excel
        from yc.excel import xlBetween, xlCellValue, xlGreater, xlValidAlertStop, xlValidateWholeNumber

        ws = self.ws
        ws.Range("A1:A3").Value = [[1], [5], [9]]
        name = f"Blk_{ws.Index}"  # Blk5 would be a cell (column BLK)
        excel.ActiveWorkbook.Names.Add(name, RefersTo=f"='{ws.Name}'!$A$1:$A$3")
        ws.Range("C1").Formula = f"=SUM({name})"
        ws.Range("A1:A3").FormatConditions.Add(xlCellValue, xlGreater, "=4")
        ws.Range("A1:A3").Validation.Add(xlValidateWholeNumber, xlValidAlertStop, xlBetween, "0", "10")
        ws.Range("H1").Value = 1
        ws.Range("I1").Formula = "=H1+1"
        ws.Range("A1:A3").Cut(ws.Range("E1"))
        self.assertEqual(ws.Range("C1").Value, 15)
        self.assertEqual(ws.Range("E1:E3").FormatConditions.Count, 1)
        self.assertEqual(ws.Range("A1:A3").FormatConditions.Count, 0)
        self.assertEqual(ws.Range("E2").Validation.Type, xlValidateWholeNumber)
        ws.Range("E1").Cut(ws.Range("H1"))  # over a cell a formula reads
        self.assertEqual(ws.Range("I1").Formula, "=#REF!+1")


    def test_fill_handle_across_moves_table_columns(self):
        from yc.excel import xlSrcRange, xlYes

        ws = self.ws
        ws.Range("A1:D3").Value = [["Region", "Jan", "Feb", "Mar"], ["North", 1, 10, 100], ["South", 2, 20, 200]]
        lo = ws.ListObjects.Add(xlSrcRange, ws.Range("A1:D3"), None, xlYes)
        ws.Range("B6").Formula = f"=SUM({lo.Name}[Jan])"
        ws.Range("B6").AutoFill(ws.Range("B6:D6"))  # the fill handle: the month moves along
        self.assertEqual(ws.Range("D6").Formula, f"=SUM({lo.Name}[Mar])")
        self.assertEqual(ws.Range("D6").Value, 300)
        ws.Range("B6").Copy(ws.Range("B8"))  # a copy keeps it
        self.assertEqual(ws.Range("B8").Formula, f"=SUM({lo.Name}[Jan])")
        lo.ShowTotals = True
        self.assertEqual(ws.Range("D4").Formula, "=SUBTOTAL(109,[Mar])")

    def test_a_difference_that_cancels_is_zero_and_f4_toggles_whole_columns(self):
        ws = self.ws
        ws.Range("B1").Formula = "=0.1+0.2"
        ws.Range("B2").Value = 0.3
        ws.Range("B3").Formula = "=B1-B2"  # 0, as Excel's (it showed 5.55E-17)
        self.assertEqual(ws.Range("B3").Value, 0)
        ws.Range("B4").Formula = "=1*(B1-B2)"  # a product last keeps the residue, as in Excel
        self.assertNotEqual(ws.Range("B4").Value, 0)
        self.keys("k:Escape|m:2,2|g:D1|w:2|t:=SUM(A:A|k:F4|w:2|t:)|k:Enter|w:3")
        self.assertEqual(ws.Range("D1").Formula, "=SUM($A:$A)")

    def test_typed_numbers_get_excels_formats_and_marks(self):
        # measured against Excel 16: a decimal point typed gives two decimals, and Excel takes signs, parentheses,
        # spaces and NT$ around a number (each case was text or another format here)
        ws = self.ws
        ws.Columns("A").ColumnWidth = 20
        cases = [("12.5%", 0.125, "0.00%", "12.50%"), ("1,000.5", 1000.5, "#,##0.00", "1,000.50"),
                 ("$1,000", 1000, "$#,##0_);[Red]($#,##0)", "$1,000 "), ("$-5", -5, "$#,##0_);[Red]($#,##0)", "($5)"),
                 ("-NT$5", -5, '"NT$"#,##0_);("NT$"#,##0)', "(NT$5)"), ("%12.5", 0.125, "0.00%", "12.50%")]
        for i, (text, value, fmt, shown) in enumerate(cases, 1):
            with self.subTest(text=text):
                cell = ws.Range(f"A{i}")
                cell.Value = text
                self.assertAlmostEqual(cell.Value2, value)
                self.assertEqual(cell.NumberFormat, fmt)
                self.assertEqual(cell.Text, shown)
        ws.Range("B1").Value = "(-5)"
        self.assertEqual(ws.Range("B1").Value, "(-5)")  # two negative marks: text, as in Excel (it was 5)
        # typed in the cell the same way
        self.keys("k:Escape|m:2,2|g:C1|w:2|t:12.5%|k:Enter|w:3|g:C2|w:2|t:-NT$5|k:Enter|w:3")
        self.assertEqual(ws.Range("C1").NumberFormat, "0.00%")
        self.assertEqual(ws.Range("C2").Value2, -5)
        self.assertEqual(ws.Range("C2").Text, "(NT$5)")

    def test_text_to_columns_formats_a_text_column_as_text(self):
        from yc.excel import xlDelimited, xlGeneralFormat, xlTextFormat

        ws = self.ws
        ws.Range("A1").Value = "'001,002"
        ws.Range("A1").TextToColumns(Destination=ws.Range("A1"), DataType=xlDelimited, Comma=True,
                                     FieldInfo=[[1, xlTextFormat], [2, xlGeneralFormat]])
        self.assertEqual(ws.Range("A1").Value, "001")
        self.assertEqual(ws.Range("A1").NumberFormat, "@")  # as Excel's (it stayed General)
        self.assertEqual(ws.Range("B1").Value, 2)


if __name__ == "__main__":
    unittest.main()
