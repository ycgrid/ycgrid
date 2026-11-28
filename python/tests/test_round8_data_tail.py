# SPDX-License-Identifier: Apache-2.0
"""Round 8, the last data differences, measured against Excel 16: DataSeries with xlAutoFill, PasteSpecial
(Transpose and operations), FormulaR1C1 wrapping around the sheet, Text to Columns' quotes and formulas, a union as a
name's RefersTo, Auto Outline's outer levels, PivotTable filters above the body, PivotField.DataRange / LabelRange
and Range.Group on a label, and a typed list's Validation.Value. Each test fails without its fix.

Run: python -m unittest tests.test_round8_data_tail (with the bundled Python).
"""
from __future__ import annotations

import itertools
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"
_N = itertools.count(1)


class _Live(unittest.TestCase):
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

        excel.Application.DisplayAlerts = False
        excel.Application.CutCopyMode = False
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Name = f"T{next(_N)}"
        self.ws.Activate()
        self.ws.Range("A1").Select()

    def put(self, cells):
        for addr, v in cells.items():
            if isinstance(v, str) and v.startswith(("=", "'")):
                self.ws.Range(addr).Formula = v
            else:
                self.ws.Range(addr).Value2 = v

    def col(self, rng):
        v = self.ws.Range(rng).Value2
        return [x for row in v for x in row]


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class DataSeriesAndR1C1Test(_Live):
    """Python only: DataSeries(Type:=xlAutoFill) and FormulaR1C1."""

    def test_dataseries_autofill_reads_every_filled_line(self):
        from yc import excel

        self.put({"A1": 1, "A2": 4})
        self.ws.Range("A1:A5").DataSeries(excel.xlColumns, excel.xlAutoFill)
        self.assertEqual(self.col("A1:A5"), [1, 4, 7, 10, 13])  # Excel; one cell copied 1 before
        self.put({"C1": 1, "D1": 4})
        self.ws.Range("C1:G1").DataSeries(excel.xlRows, excel.xlAutoFill)
        self.assertEqual(self.col("C1:G1"), [1, 4, 7, 10, 13])
        self.put({"I1": "Jan"})
        self.ws.Range("I1:I4").DataSeries(excel.xlColumns, excel.xlAutoFill)
        self.assertEqual(self.col("I1:I4"), ["Jan", "Feb", "Mar", "Apr"])
        self.put({"K1": 1, "K2": 4, "K3": 7})
        self.ws.Range("K1:K3").DataSeries(excel.xlColumns, excel.xlAutoFill)  # nothing left to fill
        self.assertEqual(self.col("K1:K4"), [1, 4, 7, None])

    def test_formula_r1c1_wraps_around_the_sheet_and_orders_ranges(self):
        cases = [("A1", "=R[-1]C", "=A1048576"), ("B1", "=RC[-2]", "=XFD1"), ("C1", "=SUM(C[-1]:C[-2])", "=SUM(A:B)"),
                 ("D1", "=SUM(C[-1]:C[-2])", "=SUM(B:C)"), ("C2", "=SUM(R[1]:R[-1])", "=SUM(1:3)"),
                 # a whole formula that is one range is written the legacy way (Excel 16, measured: ii_r1c1_range)
                 ("C3", "=R[2]C[1]:R[-1]C[-1]", "=@B2:D5"), ("D5", "=SUM(R[-10]C:R[1]C)", "=SUM(D6:D1048571)"),
                 ("B2", "=SUM(R1C:R[-1]C[5])", "=SUM(B$1:G1)")]
        for addr, r1c1, a1 in cases:
            self.ws.Range(addr).FormulaR1C1 = r1c1
            self.assertEqual(self.ws.Range(addr).Formula, a1, f"{r1c1} in {addr}")


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class PasteAndTextToColumnsTest(_Live):
    def paste(self, src, dst, **kw):
        self.ws.Range(src).Copy()
        self.ws.Range(dst).PasteSpecial(**kw)

    def test_transpose_turns_absolute_references_to_the_copied_cells(self):
        from yc import excel

        self.put({"A1": 1, "A2": 2, "B1": "=$A$1", "B2": "=$A$2+$B$1+A1+$C$9", "A3": "=$A1+A$1"})
        self.paste("A1:B3", "E1", Paste=excel.xlPasteAll, Transpose=True)
        self.assertEqual(self.ws.Range("E2").Formula, "=$E$1")
        self.assertEqual(self.ws.Range("F2").Formula, "=$F$1+$E$2+E1+$C$9")
        self.assertEqual(self.ws.Range("G1").Formula, "=E$1+$E1")

    def test_operations_on_formulas_text_and_logical_values(self):
        from yc import excel

        self.put({"A1": "=1+1", "D1": 5, "D2": 5, "A3": "x", "D3": 5, "A4": 2, "D4": "'5", "D5": True})
        self.paste("A1", "D1", Paste=excel.xlPasteAll, Operation=excel.xlPasteSpecialOperationAdd)
        self.assertEqual(self.ws.Range("D1").Formula, "=5+(1+1)")
        self.assertEqual(self.ws.Range("D1").Value2, 7)
        self.paste("A3", "D3", Paste=excel.xlPasteAll, Operation=excel.xlPasteSpecialOperationAdd)
        self.assertEqual(self.ws.Range("D3").Value2, 5)  # copied text: the cell stays
        self.paste("A4", "D4:D5", Paste=excel.xlPasteAll, Operation=excel.xlPasteSpecialOperationMultiply)
        self.assertEqual(self.ws.Range("D4").Value2, 10)  # text that reads as a number
        self.assertIs(self.ws.Range("D5").Value2, True)  # TRUE stays

    def test_text_to_columns_quotes_and_formulas(self):
        from yc import excel

        self.put({"A1": "'ab\"cd,e", "A2": "'\"a,b\"c,d", "A3": "'=1+1,2", "A4": "'=SUM(1,2)"})
        self.ws.Range("A1:A4").TextToColumns(DataType=excel.xlDelimited, TextQualifier=excel.xlTextQualifierDoubleQuote, Comma=True)
        self.assertEqual(self.ws.Range("A1").Value2, 'ab"cd')
        self.assertEqual(self.ws.Range("B1").Value2, "e")
        self.assertEqual(self.ws.Range("A2").Value2, "a,bc")
        self.assertEqual(self.ws.Range("A3").Formula, "=1+1")
        self.assertEqual(self.ws.Range("A3").Value2, 2)
        self.assertEqual(self.ws.Range("A4").Value2, "=SUM(1")


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class NamesAndOutlineTest(_Live):
    def test_a_union_is_a_name_and_a_range_of_areas(self):
        from yc import excel

        self.put({"A1": 1, "A2": 2, "A3": 3})
        self.ws.Name = "Q9"  # reads as a cell: quoted in A1 formulas, bare in R1C1 ones (Excel 16, measured)
        sh = self.ws.Name
        n = excel.ActiveWorkbook.Names.Add(f"Un_{sh}", f"={sh}!$A$1,{sh}!$A$3")
        self.assertEqual(n.RefersTo, "='Q9'!$A$1,'Q9'!$A$3")
        self.assertEqual(n.RefersToR1C1, "=Q9!R1C1,Q9!R3C1")
        rng = n.RefersToRange
        self.assertEqual(rng.Address(), "$A$1,$A$3")
        self.assertEqual(rng.Areas.Count, 2)
        with self.assertRaises(yc.YcError):
            excel.ActiveWorkbook.Names.Add(f"Kn_{sh}", f"={sh}!$A$1,5")  # a constant can't be joined (Excel refuses too)

    def test_auto_outline_nests_a_total_of_totals(self):
        self.put({"A1": 1, "A2": 2, "A3": "=SUM(A1:A2)", "A4": 3, "A5": 4, "A6": "=SUM(A4:A5)", "A7": "=SUM(A3,A6)"})
        self.ws.Range("A1").AutoOutline()
        self.assertEqual([self.ws.Rows(r).OutlineLevel for r in range(1, 9)], [3, 3, 2, 3, 3, 2, 1, 1])

    def test_a_subtotal_closed_by_its_button_stays_closed_through_the_grand_total(self):
        # Excel 16, measured 2026-11-28: the level 2 button closes the subtotals' groups, and they stay closed when the
        # grand total's group is closed and opened; with every group open, closing and opening it brings all back
        ws = self.ws
        ws.Cells.ClearOutline()
        for r in range(1, 10):
            ws.Cells(r, 1).Value = r
        ws.Rows("2:7").Group()
        ws.Rows("2:3").Group()
        ws.Rows("5:6").Group()

        def hidden():
            return "".join("h" if ws.Rows(r).Hidden else "." for r in range(1, 10))

        ws.Rows(8).ShowDetail = False
        ws.Rows(8).ShowDetail = True
        self.assertEqual(hidden(), ".........")
        ws.Outline.ShowLevels(2)
        ws.Rows(8).ShowDetail = False
        ws.Rows(8).ShowDetail = True
        self.assertEqual(hidden(), ".hh.hh...")
        ws.Outline.ShowLevels(1)
        ws.Rows(8).ShowDetail = True
        self.assertEqual(hidden(), ".........")
        ws.Cells.ClearOutline()

    def test_auto_outline_takes_the_side_of_the_first_total(self):
        # Excel 16, measured 2026-11-28: totals above their details group the rows after them and set Summary rows
        # above; the first total in reading order picks the side and a total on the other side is left out; totals
        # left of their columns set Summary columns to the left; nothing to group raises
        from yc import excel

        def levels(rows=True, n=8):
            return "".join(str((self.ws.Rows(i) if rows else self.ws.Columns(i)).OutlineLevel) for i in range(1, n + 1))

        def fresh(cells):
            self.ws.Cells.ClearOutline()
            self.ws.Cells.Clear()
            self.ws.Outline.SummaryRow = excel.xlSummaryBelow
            self.ws.Outline.SummaryColumn = excel.xlSummaryOnRight
            self.put(cells)
            self.ws.Range("A1").AutoOutline()

        fresh({"B1": "=SUBTOTAL(9,B2:B4)", "B2": 1, "B3": 2, "B4": 3, "B5": "=SUBTOTAL(9,B6:B7)", "B6": 4, "B7": 5})
        self.assertEqual((levels(), self.ws.Outline.SummaryRow), ("12221221", excel.xlSummaryAbove))
        fresh({"B1": "=SUM(B2,B5)", "B2": "=SUM(B3:B4)", "B3": 1, "B4": 2, "B5": "=SUM(B6:B7)", "B6": 3, "B7": 4})
        self.assertEqual(levels(), "12332331")
        fresh({"B1": "=SUM(B3:B4)", "B3": 1, "B4": 2})
        self.assertEqual(levels(), "12221111")
        fresh({"B1": "=SUM(B2:B3)", "B2": 1, "B3": 2, "B4": 4, "B5": 5, "B6": "=SUM(B4:B5)"})
        self.assertEqual((levels(), self.ws.Outline.SummaryRow), ("12211111", excel.xlSummaryAbove))
        fresh({"B1": 1, "B2": 2, "B3": "=SUM(B1:B2)", "B4": "=SUM(B5:B6)", "B5": 4, "B6": 5})
        self.assertEqual((levels(), self.ws.Outline.SummaryRow), ("22111111", excel.xlSummaryBelow))
        fresh({"A1": "=SUM(B1:C1)", "B1": 1, "C1": 2, "D1": "=SUM(E1:F1)", "E1": 3, "F1": 4})
        self.assertEqual((levels(False), self.ws.Outline.SummaryColumn), ("12212211", excel.xlSummaryOnLeft))
        self.ws.Cells.ClearOutline()
        self.ws.Cells.Clear()
        self.put({"B1": "=B2*2", "B2": 1})
        with self.assertRaises(yc.YcError):
            self.ws.Range("A1").AutoOutline()


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class SortOrderTailTest(_Live):
    def test_a_custom_order_descending_and_case_sensitive(self):
        # Excel 16, measured 2026-11-28 (Sort.SortFields.Add with CustomOrder): descending puts the values not in the
        # list first; Case sensitive takes only the items as written; blanks stay last
        ws = self.ws

        def sort(order, match_case=False):
            ws.Range("A1:B9").Value = [["Risk", 0], ["Mid", 1], ["high", 2], ["Low", 3], [None, 4], ["Mid", 5], ["Other", 6],
                                       ["low", 7], ["High", 8]]
            ws.Sort.SortFields.Clear()
            ws.Sort.SortFields.Add(ws.Range("A2:A9"), 0, order, "Low,Mid,High")
            ws.Sort.MatchCase = match_case
            ws.Sort.SetRange(ws.Range("A1:B9"))
            ws.Sort.Header = 1
            ws.Sort.Apply()
            return [int(x) for x in self.col("B2:B9")]

        self.assertEqual(sort(1), [3, 7, 1, 5, 2, 8, 6, 4])
        self.assertEqual(sort(2), [6, 2, 8, 1, 5, 3, 7, 4])
        self.assertEqual(sort(1, True), [3, 1, 5, 8, 2, 7, 6, 4])

    def test_sort_takes_excels_arguments_by_position(self):
        # Range.Sort(Key1, Order1, Key2, Type, Order2, Key3, Order3, Header, OrderCustom, MatchCase, Orientation), as
        # a macro written for Excel passes them: left to right by row 2 descending, the first column a header
        # (Excel 16 measured 2026-11-28 through COM)
        ws = self.ws
        ws.Range("A1:F3").Value = [["Sym", "C", "A", "B", None, "A"], ["Px", 3, 1, 2, 9, 0], ["Qty", 30, 10, 20, 90, 5]]
        ws.Range("A1:F3").Sort(ws.Range("A2"), 2, None, None, 1, None, 1, 1, 1, False, 2)
        self.assertEqual([ws.Cells(3, c).Value for c in range(1, 7)], ["Qty", 90, 30, 20, 10, 5])
        ws.Range("A1:F3").Value = [["Sym", "C", "A", "B", None, "A"], ["Px", 3, 1, 2, 9, 0], ["Qty", 30, 10, 20, 90, 5]]
        ws.Range("B1:F3").Sort(ws.Range("B1"), 1, ws.Range("B3"), None, 2, None, 1, 2, 1, False, 2)
        self.assertEqual([ws.Cells(3, c).Value for c in range(2, 7)], [10, 5, 20, 30, 90])


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ExcelArgumentsTailTest(_Live):
    def test_calls_with_excels_arguments_by_position(self):
        # every method takes Excel's arguments in Excel's order (scripts/check_excel_parity.py checks the names):
        # Workbook.Close(SaveChanges, Filename), Shapes.AddChart(Type, Left, Top, Width, Height), Areas.Item(Index)
        import tempfile

        from yc import excel

        ws = self.ws
        ws.Range("A1:B3").Value = [["x", 1], ["y", 2], ["z", 3]]
        sh = ws.Shapes.AddChart(4, 10, 20, 300, 200)  # xlLine
        self.assertEqual(sh.Chart.ChartType, 4)
        self.assertEqual((sh.Left, sh.Top), (10, 20))
        self.assertEqual(ws.Range("A1:A2,B3").Areas.Item(Index=2).Address(False, False), "B3")
        path = os.path.join(tempfile.mkdtemp(), "closed.ycg")
        excel.ActiveWorkbook.Close(True, path)
        self.assertTrue(os.path.exists(path))


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ExcelDiffTailTest(_Live):
    def test_merges_protection_and_arrays_as_excel(self):
        # scripts/excel_diff.py against Excel 16 (2026-11-28): a write over part of a merged cell leaves it alone, a
        # fill over several areas on a protected sheet changes nothing when one target is locked, and HasArray over
        # a range an array covers in part is Null
        ws = self.ws
        ws.Range("A1:E6").Value = [["Name", "Qty", "Price", "Total", "When"], ["AAPL", 10, 1.5, None, 45000],
                                   ["MSFT", 20, 2.25, None, 45001], ["TSMC", 30, 3, None, 45002],
                                   ["NVDA", 40, 4.75, None, 45003], ["AMZN", 50, 5, None, 45004]]
        ws.Range("B3:C4").Merge()
        ws.Range("B:B").Value2 = 5
        self.assertEqual((ws.Range("B2").Value2, ws.Range("B3").Value2), (5, 20))
        ws.Range("B3").Value2 = 7  # the merged cell's own cell alone is written
        self.assertEqual(ws.Range("B3").Value2, 7)
        ws.Range("B2:C3").Value2 = 8  # and by a range that isn't whole rows or columns
        self.assertEqual(ws.Range("B3").Value2, 8)
        ws.Range("B:B").Select()  # Select takes in the merged cells it cuts
        from yc import excel

        self.assertEqual(excel.Selection.Address(), "$B:$C")
        ws.Range("B3:C4").UnMerge()
        ws.Range("G2:G4").FormulaArray = "=B2:B4*2"
        self.assertIsNone(ws.Range("2:3").HasArray)
        self.assertTrue(ws.Range("G2:G3").HasArray)
        self.assertFalse(ws.Range("A2:B3").HasArray)
        ws.Range("B:D").Locked = False
        ws.Protect()
        try:
            with self.assertRaises(yc.YcError):
                ws.Range("A1:B2,D4:E5").FillRight()
            self.assertEqual(ws.Range("B1").Value2, 5)  # nothing filled, the first area neither
        finally:
            ws.Unprotect()

    def test_a_table_header_text_and_notes_over_areas_as_excel(self):
        # Excel 16 (2026-11-28): one formula over whole rows through a table's header row is relative to the first
        # cell it writes (the header keeps its text); Text over areas that show different text is Null; a note over
        # several one-cell areas goes on the first area's cell
        ws = self.ws
        ws.Range("A1:E6").Value = [["Name", "Qty", "Price", "Total", "When"], ["AAPL", 10, 1.5, None, 45000],
                                   ["MSFT", 20, 2.25, None, 45001], ["TSMC", 30, 3, None, 45002],
                                   ["NVDA", 40, 4.75, None, 45003], ["AMZN", 50, 5, None, 45004]]
        ws.ListObjects.Add(1, ws.Range("A1:E6"), None, 1)
        ws.Range("1:1").Formula = "=ROW()*10+COLUMN()"
        # the header row holds text, as Excel writes it there: "0", "02", "03", "04", "05" (measured)
        self.assertEqual([ws.Cells(1, c).Value2 for c in range(1, 6)], ["0", "02", "03", "04", "05"])
        self.assertEqual(ws.Range("F1").Formula, "=ROW()*10+COLUMN()")
        self.assertEqual(ws.Range("G1").Value2, 17)
        ws.Range("H2").Value2 = 10
        ws.Range("J4").Formula = "=1+1"
        self.assertIsNone(ws.Range("H2,J4").Text)
        ws.Range("J4").Value2 = 10
        self.assertEqual(ws.Range("H2,J4").Text, "10")
        ws.Range("J4,H2").AddComment("x")
        self.assertEqual(ws.Range("J4").Comment.Text(), "x")
        self.assertIsNone(ws.Range("H2").Comment)
        with self.assertRaises(yc.YcError):
            ws.Range("K2:L2,K4").AddComment("y")


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class FreshReadsTailTest(_Live):
    def test_reads_right_after_a_formula_is_written_see_its_value(self):
        # Excel's object model calculates after each write: DisplayFormat and a DataFrame read right after a formula is
        # written see its value (they read it uncalculated: empty, and the rule not applied)
        import yc.data as data

        ws = self.ws
        ws.Range("A1:B1").Value = [["x", "y"]]
        for i in range(5):
            ws.Range("A2").Value = i
            ws.Range("B2").Formula = "=A2*10"
            self.assertEqual(data.read_range(ws.Range("A1:B2"), pandas=False)["y"][0], i * 10)
            ws.Range("C1").Formula = f"=A2+{i}"
            ws.Range("C1").FormatConditions.Delete()
            ws.Range("C1").FormatConditions.Add(1, 3, f"={2 * i}").Interior.Color = 255
            self.assertEqual(ws.Range("C1").DisplayFormat.Interior.Color, 255)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class WritablePropertiesTailTest(_Live):
    def test_properties_excel_sets_are_set(self):
        # each property Excel's type library lists with put has a setter (scripts/check_excel_parity.py checks); these
        # had none and a macro setting them raised
        from yc import excel

        ws = self.ws
        ws.Range("A1:C2").Value = [[1, 2, 3], [4, 5, 6]]
        ws.Range("E1:F2").Value = [[3, 2], [1, 9]]
        g = ws.Range("D1:D2").SparklineGroups.Add(1, f"{ws.Name}!A1:C2")
        g.SourceData = f"{ws.Name}!E1:F2"
        self.assertIn("E1:F1", g.SourceData)
        g.Location = ws.Range("H1:H2")
        self.assertEqual(g.Location.Address(False, False), "H1:H2")
        ws.Scenarios().Add("Base", ws.Range("A1"), [1], "first")
        sc = ws.Scenarios("Base")
        sc.Comment = "edited"
        sc.Name = "Bull"
        self.assertEqual((ws.Scenarios(1).Name, ws.Scenarios(1).Comment, ws.Scenarios().Count), ("Bull", "edited", 1))
        ws.HPageBreaks.Add(ws.Range("A10"))
        pb = ws.HPageBreaks(1)
        pb.Location = ws.Range("A15")
        self.assertEqual([b.Location.Row for b in ws.HPageBreaks if b.Type == excel.xlPageBreakManual], [15])
        ws.Range("J1:K4").Value = [["Side", "Qty"], ["Buy", 1], ["Sell", 2], ["Buy", 3]]
        pc = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, f"{ws.Name}!J1:K3")
        pt = pc.CreatePivotTable(f"{ws.Name}!M1", f"PT{ws.Name}")
        pt.PivotCache().SourceData = f"{ws.Name}!J1:K4"
        self.assertIn("R4C11", pt.SourceData)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class GoToSpecialTailTest(_Live):
    def test_a_multi_area_select_makes_the_first_area_active(self):
        # Excel 16, measured 2026-11-28: Range("B2,D5").Select is "$B$2,$D$5" with B2 active, D5:E6,A1:B2 keeps its
        # order with D5 active, E6.Activate in A1:B2,D5:E6 keeps the order, and SpecialCells(...).Select is as Select
        from yc import excel

        ws = self.ws
        ws.Range("B2,D5").Select()
        self.assertEqual((excel.Selection.Address(False, False), excel.ActiveCell.Address(False, False)), ("B2,D5", "B2"))
        ws.Range("D5:E6,A1:B2").Select()
        self.assertEqual((excel.Selection.Address(False, False), excel.ActiveCell.Address(False, False)), ("D5:E6,A1:B2", "D5"))
        ws.Range("A1:B2,D5:E6").Select()
        ws.Range("E6").Activate()
        self.assertEqual((excel.Selection.Address(False, False), excel.ActiveCell.Address(False, False)), ("A1:B2,D5:E6", "E6"))
        ws.Range("B2").AddComment("n")
        ws.Range("D5").AddComment("m")
        ws.Range("A1").SpecialCells(-4144).Select()
        self.assertEqual((excel.Selection.Address(False, False), excel.ActiveCell.Address(False, False)), ("B2,D5", "B2"))

    def test_same_validation_takes_every_rule_with_the_same_settings(self):
        # Excel 16, measured 2026-11-28: the same list added to G1:G3 and G8 is one rule; another input message or
        # "a, b" isn't; =A6>0 in J6 is J1's =A1>0
        ws = self.ws
        ws.Range("G1:G3").Validation.Add(3, 1, 1, "a,b")
        ws.Range("G6").Validation.Add(3, 1, 1, "a,b")
        ws.Range("G6").Validation.InputMessage = "pick"
        ws.Range("G8").Validation.Add(3, 1, 1, "a,b")
        ws.Range("G10").Validation.Add(3, 1, 1, "a, b")
        ws.Range("J1:J3").Validation.Add(7, 1, 1, "=A1>0")
        ws.Range("J6").Validation.Add(7, 1, 1, "=A6>0")
        ws.Range("J8").Validation.Add(7, 1, 1, "=A1>0")
        self.assertEqual(ws.Range("G2").SpecialCells(-4175).Address(False, False), "G1:G3,G8")
        self.assertEqual(ws.Range("J2").SpecialCells(-4175).Address(False, False), "J1:J3,J6")


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class PivotTailTest(_Live):
    ROWS = [["Region", "Product", "Sales", "Num"], ["East", "P1", 10, 5], ["West", "P2", 20, 15], ["East", "P2", 30, 25],
            ["North", "P1", 40, 35], ["West", "P1", 50, 45], ["North", "P2", 60, 55], ["East", "P1", 70, 65], ["West", "P2", 80, 75]]

    def pivot(self, dest, rows=("Region",), cols=(), pages=()):
        from yc import excel

        self.ws.Range("M1:P9").Value = self.ROWS
        pc = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, f"{self.ws.Name}!M1:P9")
        pt = pc.CreatePivotTable(f"{self.ws.Name}!{dest}", f"PT{self.ws.Name}")
        for f in rows:
            pt.PivotFields(f).Orientation = excel.xlRowField
        for f in cols:
            pt.PivotFields(f).Orientation = excel.xlColumnField
        for f in pages:
            pt.PivotFields(f).Orientation = excel.xlPageField
        pt.AddDataField(pt.PivotFields("Sales"), "Sum of Sales", excel.xlSum)
        return pt

    @staticmethod
    def tr(pt):
        return pt.TableRange1.Address(False, False) + " | " + pt.TableRange2.Address(False, False)

    def test_filters_go_above_the_body(self):
        from yc import excel

        pt = self.pivot("A3")
        self.assertEqual(self.tr(pt), "A3:B7 | A3:B7")
        pt.PivotFields("Product").Orientation = excel.xlPageField
        self.assertEqual(self.tr(pt), "A3:B7 | A1:B7")  # Excel; the body moved to A5 before
        pt.PivotFields("Num").Orientation = excel.xlPageField
        self.assertEqual(self.tr(pt), "A4:B8 | A1:B8")
        pt.PivotFields("Product").Orientation = excel.xlHidden
        self.assertEqual(self.tr(pt), "A4:B8 | A2:B8")
        pt.PivotFields("Num").Orientation = excel.xlHidden
        self.assertEqual(self.tr(pt), "A4:B8 | A4:B8")
        pt.PivotFields("Product").Orientation = excel.xlPageField
        pt.Location = f"{self.ws.Name}!$F$10"
        self.assertEqual(self.tr(pt), "F10:G14 | F8:G14")

    def test_field_data_and_label_ranges(self):
        pt = self.pivot("A3", cols=("Product",), pages=("Num",))
        self.assertEqual(self.tr(pt), "A3:D8 | A1:D8")

        def both(f):
            return f.DataRange.Address(False, False), f.LabelRange.Address(False, False)

        self.assertEqual(both(pt.PivotFields("Region")), ("A5:A7", "A4"))
        self.assertEqual(both(pt.PivotFields("Product")), ("B4:C4", "B3"))
        self.assertEqual(both(pt.PivotFields("Num")), ("B1", "A1"))
        self.assertEqual(both(pt.DataFields(1)), ("B5:C7", "A3"))

    def test_field_ranges_with_two_row_fields(self):
        from yc import excel

        pt = self.pivot("A3", rows=("Region", "Product"))
        self.assertEqual(pt.PivotFields("Product").DataRange.Address(False, False), "A4:A12")
        self.assertEqual(pt.PivotFields("Product").LabelRange.Address(False, False), "A3")
        self.assertEqual(pt.DataFields(1).DataRange.Address(False, False), "B4:B12")
        self.assertEqual(pt.DataFields(1).LabelRange.Address(False, False), "B3")
        pt.RowAxisLayout(excel.xlTabularRow)
        self.assertEqual(pt.PivotFields("Product").DataRange.Address(False, False), "B4:B12")
        self.assertEqual(pt.PivotFields("Product").LabelRange.Address(False, False), "B3")

    def test_range_group_on_a_label_groups_its_field(self):
        pt = self.pivot("A3", rows=("Num",))
        pt.PivotFields("Num").DataRange.Cells(2).Group(0, 100, 20)  # as a recorded macro writes it
        names = [i.Name for i in pt.PivotFields("Num").PivotItems()]
        for n in ("0-19", "20-39", "40-59", "60-79"):
            self.assertIn(n, names)
        self.assertEqual(self.ws.Range("A4").Value2, "0-19")


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ValidationValueTest(_Live):
    def test_a_typed_list_counts_case_and_drops_the_spaces_around(self):
        from yc import excel

        self.ws.Range("A1").Validation.Add(excel.xlValidateList, excel.xlValidAlertStop, excel.xlBetween, "a,b,c")
        got = {}
        for v in ("a", "A", " a", "b ", "B", "d"):
            self.ws.Range("A1").Value2 = v
            got[v] = self.ws.Range("A1").Validation.Value
        self.assertEqual(got, {"a": True, "A": False, " a": True, "b ": True, "B": False, "d": False})
        # a list of cells: without case, spaces count (Excel)
        self.put({"C1": "a", "C2": "b"})
        self.ws.Range("A2").Validation.Add(excel.xlValidateList, excel.xlValidAlertStop, excel.xlBetween, "=$C$1:$C$2")
        for v, ok in (("A", True), (" a", False)):
            self.ws.Range("A2").Value2 = v
            self.assertEqual(self.ws.Range("A2").Validation.Value, ok, v)


if __name__ == "__main__":
    unittest.main()
