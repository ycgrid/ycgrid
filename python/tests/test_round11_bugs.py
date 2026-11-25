# SPDX-License-Identifier: Apache-2.0
"""Round 11: bugs found by driving the object model the way recorded macros and scripts do, each with what Excel 16
does (measured).

Run: python -m unittest tests.test_round11_bugs (with the bundled Python).
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
class Round11Test(unittest.TestCase):
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

    def test_a_pivot_source_in_r1c1_as_recorded_macros_write_it(self):
        from yc import excel

        ws = self.ws
        ws.Name = "My Data R11"
        ws.Range("A1:B4").Value = [["k", "v"], ["a", 1], ["b", 2], ["a", 3]]
        rep = excel.Worksheets.Add(After=ws)
        rep.Name = "Rep R11"
        pc = excel.ActiveWorkbook.PivotCaches.Create(1, "'My Data R11'!R1C1:R4C2")  # raised "Invalid source"
        self.assertEqual(pc.SourceData, "'My Data R11'!R1C1:R4C2")
        pt = pc.CreatePivotTable("'Rep R11'!R3C1", "ptR11")
        self.assertEqual(pt.Location, "$A$3")
        self.assertEqual(pt.SourceData, "'My Data R11'!R1C1:R4C2")  # Excel 16 reads R1C1
        ws.Rows(3).Insert()
        self.assertEqual(pt.SourceData, "'My Data R11'!R1C1:R5C2")
        plain = excel.Worksheets.Add(After=rep)
        plain.Name = "PlainR11"
        plain.Range("A1:B2").Value = [["k", "v"], ["a", 1]]
        pt.SourceData = "PlainR11!R1C1:R2C2"
        self.assertEqual(pt.SourceData, "PlainR11!R1C1:R2C2")
        pt2 = excel.ActiveWorkbook.PivotCaches.Create(1, "PlainR11!A1:B2").CreatePivotTable(rep.Range("H3"), "pt2R11")
        self.assertEqual(pt2.SourceData, "PlainR11!R1C1:R2C2")

    def test_rows_inserted_through_a_pivot_report_are_refused(self):
        import time

        from yc import excel
        from yc.errors import YcError

        ws = self.ws
        ws.Range("A1:B4").Value = [["k", "v"], ["a", 1], ["b", 2], ["a", 3]]
        pt = excel.ActiveWorkbook.PivotCaches.Create(1, ws.Range("A1:B4")).CreatePivotTable(ws.Range("E3"), "ptRows")
        pt.AddFields(RowFields="k")
        pt.AddDataField(pt.PivotFields("v"))
        self.assertEqual(pt.TableRange1.Address, "$E$3:$F$6")
        with self.assertRaises(YcError):  # Excel 16: error 1004, "...it will affect a PivotTable"
            ws.Rows(4).Insert()
        with self.assertRaises(YcError):
            ws.Columns(6).Delete()
        self.assertEqual(pt.TableRange1.Address, "$E$3:$F$6")
        self.assertEqual(ws.Range("E5").Value2, "b")
        ws.Rows(3).Insert()  # at its first row the report moves
        self.assertEqual(pt.TableRange1.Address, "$E$4:$F$7")
        # the keys a person uses (Ctrl+Shift+= on a whole row) get Excel's message
        self.conn.request("ui.input", {"script": "m:2,2|w:2|g:A5|k:s-Space|k:c-s-Equal|w:4", "timeout_ms": 15000}, timeout=30)
        time.sleep(0.3)
        from yc import ui

        msgs = " ".join(str(e.get("text", "")) for e in ui.errors())
        self.assertIn("it will affect a PivotTable", msgs)
        self.assertEqual(pt.TableRange1.Address, "$E$4:$F$7")
        ws.Rows("4:7").Delete()  # every row of it: the report goes with them
        self.assertEqual(ws.PivotTables().Count, 0)

    def test_a_workbook_opens_with_the_calculation_settings_it_was_saved_with(self):
        import tempfile

        from yc import excel

        path = os.path.join(tempfile.mkdtemp(prefix="yc_r11_"), "cycle.ycg")
        app = excel.Application
        was = (app.Iteration, app.MaxIterations)
        app.Iteration = True
        app.MaxIterations = 37
        app.Calculation = excel.xlCalculationManual
        try:
            self.ws.Range("A1").Formula = "=A1+1"  # a model built on a cycle
            excel.ActiveWorkbook.SaveCopyAs(path)
        finally:
            app.Iteration, app.MaxIterations = was
            app.Calculation = excel.xlCalculationAutomatic
        other = yc.launch(path, hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=other.pid)
            # Excel 16 keeps the settings in the file and opening it sets them; the session here had iteration off
            self.assertEqual((excel.Application.Iteration, excel.Application.MaxIterations), (True, 37))
            self.assertEqual(excel.Application.Calculation, excel.xlCalculationManual)  # a file saved in Manual opens in Manual
        finally:
            other.terminate()
            other.wait(10)
            yc.connect(target=self.proc.pid)

    def test_a_copied_sheet_works_on_its_own_table_and_cells(self):
        from yc import excel

        ws = self.ws
        ws.Name = "Src R11"
        ws.Range("A1:B4").Value = [["k", "v"], ["a", 1], ["b", 2], ["c", 3]]
        lo = ws.ListObjects.Add(1, ws.Range("A1:B4"), XlListObjectHasHeaders=1)
        ws.Range("D1").Formula = f"=SUM({lo.Name}[v])"
        box = ws.Shapes.AddFormControl(excel.xlCheckBox, 300, 50, 80, 20)
        box.ControlFormat.LinkedCell = "'Src R11'!$F$2"
        ws.Range("G2").Validation.Add(3, 1, 1, "='Src R11'!$A$2:$A$4")
        ws.Range("H1:H3").FormatConditions.Add(2, Formula1="='Src R11'!$B2>1")
        ws.Copy(After=ws)
        cp = excel.Worksheets(ws.Index + 1)
        # Excel 16, measured: the copy's formula sums the copy's table, its control writes the copy's cell, and its
        # validation and rule read the copy's cells (naming no sheet)
        self.assertEqual(cp.Range("D1").Formula, f"=SUM({cp.ListObjects(1).Name}[v])")
        cp.Range("B2").Value = 100
        self.assertEqual((cp.Range("D1").Value2, ws.Range("D1").Value2), (105, 6))
        self.assertEqual(cp.Shapes(1).ControlFormat.LinkedCell, f"'{cp.Name}'!$F$2")
        cp.Shapes(1).ControlFormat.Value = 1
        self.assertEqual((cp.Range("F2").Value2, ws.Range("F2").Value2), (True, None))
        self.assertEqual(cp.Range("G2").Validation.Formula1, "=$A$2:$A$4")
        self.assertEqual(cp.Range("H1:H3").FormatConditions(1).Formula1, "=$B2>1")
        self.assertEqual(ws.Range("H1:H3").FormatConditions(1).Formula1, "='Src R11'!$B2>1")

    def test_a_deleted_table_column_leaves_ref_errors_however_it_went(self):
        ws = self.ws
        ws.Range("A1:C3").Value = [["k", "v", "w"], ["a", 1, 10], ["b", 2, 20]]
        lo = ws.ListObjects.Add(1, ws.Range("A1:C3"), XlListObjectHasHeaders=1)
        ws.Range("H1").Formula = f"=SUM({lo.Name}[v])"
        ws.Range("H2").Formula = f"=SUM({lo.Name}[w])"
        ws.Columns(2).Delete()  # Excel 16, measured: =SUM(#REF!), whatever deleted the column
        self.assertEqual(ws.Range("G1").Formula, "=SUM(#REF!)")
        self.assertEqual(ws.Range("G2").Formula, f"=SUM({lo.Name}[w])")
        ws.Range("C1").Value = "v"  # a column named v again doesn't take it over
        self.assertEqual(ws.Range("G1").Formula, "=SUM(#REF!)")
        ws.Range("B1:B3").Delete(-4159)  # cells shifted left across the table's rows (G2 goes to F2 with them)
        self.assertEqual(ws.Range("F2").Formula, "=SUM(#REF!)")

    def test_a_deleted_table_leaves_ref_errors_and_a_new_one_doesnt_take_them(self):
        ws = self.ws
        ws.Range("A3:B5").Value = [["k", "v"], ["a", 1], ["b", 2]]
        lo = ws.ListObjects.Add(1, ws.Range("A3:B5"), XlListObjectHasHeaders=1)
        lo.Name = "Gone11"
        ws.Range("H1").Formula = "=SUM(Gone11[v])"
        ws.Range("H2").Formula = "=ROWS(Gone11)"
        ws.Rows("3:5").Delete()  # Excel 16, measured: =SUM(#REF!), =ROWS(#REF!) (ROWS read #NAME?)
        self.assertEqual((ws.Range("H1").Formula, ws.Range("H2").Formula), ("=SUM(#REF!)", "=ROWS(#REF!)"))
        ws.Range("A3:B4").Value = [["k", "v"], ["a", 1]]
        ws.ListObjects.Add(1, ws.Range("A3:B4"), XlListObjectHasHeaders=1).Name = "Gone11"
        self.assertEqual(ws.Range("H1").Formula, "=SUM(#REF!)")

    def test_clearing_a_whole_table_deletes_it(self):
        import time

        ws = self.ws
        for how in ("clear", "contents", "delete key", "body"):
            ws.Range("A3:B5").Value = [["k", "v"], ["a", 1], ["b", 2]]
            lo = ws.ListObjects.Add(1, ws.Range("A3:B5"), XlListObjectHasHeaders=1)
            name = lo.Name
            ws.Range("H1").Formula = f"=SUM({name}[v])"
            if how == "clear":
                ws.Range("A1:D9").Clear()
            elif how == "contents":
                ws.Range("A3:B5").ClearContents()
            elif how == "delete key":
                self.conn.request("ui.input", {"script": "m:2,2|w:2|g:A3:B5|k:Delete|w:4", "timeout_ms": 15000}, timeout=30)
                time.sleep(0.2)
            else:
                ws.Range("A4:B5").ClearContents()
            if how == "body":  # its data cleared, the table stays (Excel 16, measured)
                self.assertEqual((ws.ListObjects.Count, ws.Range("H1").Formula, ws.Range("H1").Value2), (1, f"=SUM({name}[v])", 0))
                lo.Delete()
            else:  # Excel 16, measured: the table goes, and what named it is #REF! (its headers became Column1, Column2)
                self.assertEqual((ws.ListObjects.Count, ws.Range("H1").Formula), (0, "=SUM(#REF!)"), how)
            ws.Cells.Clear()

    def test_a_border_between_two_cells_reads_from_both(self):
        ws = self.ws
        b = ws.Range("B2").Borders(9)  # xlEdgeBottom
        b.LineStyle = 1
        b.Weight = -4138  # xlMedium
        # Excel 16, measured: the line between B2 and B3 is B3's top too
        self.assertEqual((ws.Range("B3").Borders(8).LineStyle, ws.Range("B3").Borders(8).Weight), (1, -4138))
        ws.Range("D2").Borders(10).LineStyle = 1  # xlEdgeRight
        self.assertEqual(ws.Range("E2").Borders(7).LineStyle, 1)
        self.assertEqual(ws.Range("E2:E3").Borders(7).LineStyle, None)  # E3 has none
        ws.Range("G5").Borders(8).LineStyle = -4115  # xlDash on top
        self.assertEqual(ws.Range("G4").Borders(9).LineStyle, -4115)
        self.assertEqual(ws.Range("G4:G5").Borders(12).LineStyle, -4142)  # the inside line is the cells' own
        ws.Range("B3").Borders(8).LineStyle = -4142  # clearing it from either cell clears the line
        self.assertEqual(ws.Range("B2").Borders(9).LineStyle, -4142)
        ws.Range("G4").Borders(9).LineStyle = -4142
        self.assertEqual(ws.Range("G5").Borders(8).LineStyle, -4142)


if __name__ == "__main__":
    unittest.main()
