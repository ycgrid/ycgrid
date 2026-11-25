# SPDX-License-Identifier: Apache-2.0
"""Round 8: the Range object model measured against Excel 16 (Rows / Columns of a part of the sheet, several areas,
Delete / Insert without Shift, Find, Union, Cells, Formula of constants), each test failing without its fix.

Run: python -m unittest tests.test_round8_om (with the bundled Python).
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


class FormulaNumberTextTest(unittest.TestCase):
    """Range.Formula of a number constant: the text Excel 16 reads (measured through COM)."""

    def test_numbers_read_as_excel_writes_them(self):
        from yc.excel import _formula_number_text as t

        cases = {5: "5", 0: "0", -0.5: "-0.5", 0.1 + 0.2: "0.3", 1 / 3: "0.333333333333333", 2 / 3: "0.666666666666667",
                 45322: "45322", 12345.6789: "12345.6789", 1e15: "1000000000000000", 1234567890123456: "1234567890123460",
                 123456789012345678: "123456789012346000", 1e20: "100000000000000000000", -1e20: "-100000000000000000000",
                 9.99e20: "999000000000000000000", 1e21: "1E+21", 1.5e21: "1.5E+21", 1e300: "1E+300", 1e-5: "0.00001",
                 2.5e-7: "0.00000025", 1.23456789e-10: "0.000000000123456789", 1e-19: "0.0000000000000000001",
                 1.5e-19: "1.5E-19", 1e-20: "1E-20", 1.2345678901234e-8: "1.2345678901234E-08", -1e-9: "-0.000000001",
                 0.000123456789012345678: "0.000123456789012346", 5e-324: "0"}
        for x, want in cases.items():
            self.assertEqual(t(x), want, repr(x))


class UnionTest(unittest.TestCase):
    """Application.Union tidies its areas as Excel 16 does (measured)."""

    def addr(self, *refs):
        from yc import excel

        return excel.Union(*[excel.Range._from(1, [excel._parse_area(r)]) for r in refs]).Address

    def test_union_joins_contains_and_keeps_order(self):
        cases = {("A1:A3", "A2"): "$A$1:$A$3", ("A2", "A1:A3"): "$A$1:$A$3", ("A1:B2", "B1:C2"): "$A$1:$C$2",
                 ("A1:A2", "A2:A3"): "$A$1:$A$3", ("A1:B2", "B2:C3"): "$A$1:$B$2,$B$2:$C$3", ("A1", "C3", "A1"): "$C$3,$A$1",
                 ("A1", "B1", "A1"): "$A$1:$B$1", ("A1", "C3", "E5", "C3"): "$A$1,$E$5,$C$3", ("A1:A2", "C1", "A3"): "$C$1,$A$1:$A$3",
                 ("A1", "B2", "A2", "B1"): "$A$1:$B$2", ("A1:B1", "A1:A2"): "$A$1:$B$1,$A$1:$A$2", ("A1:B2", "C1:C2", "A3:C3"): "$A$1:$C$3",
                 ("A1", "C1"): "$A$1,$C$1", ("A1:A2", "B1:B3"): "$A$1:$A$2,$B$1:$B$3",
                 # the partner is looked for from the last area back (measured, 2026-11-09)
                 ("C4:J7", "A3:F9", "D4:G4"): "$A$3:$F$9,$C$4:$J$7", ("B6:J9", "B4:I8", "E7:F8"): "$B$6:$J$9,$B$4:$I$8",
                 ("D2:I9", "F6:J10", "F2:J9"): "$D$2:$I$9,$F$2:$J$10", ("C4:J10", "A3:E7", "B2", "C7:F9"): "$A$3:$E$7,$B$2,$C$4:$J$10"}
        for refs, want in cases.items():
            self.assertEqual(self.addr(*refs), want, refs)


class WholeColumnOffsetTest(unittest.TestCase):
    def test_whole_columns_moved_down_are_off_the_sheet(self):
        from yc import excel

        cols = excel.Range._from(1, [excel._parse_area("C:D")])
        self.assertEqual(cols.Offset(0, 2).Address, "$E:$F")
        with self.assertRaises(excel.ApplicationDefinedError):  # Excel 16: an error (measured)
            cols.Offset(4, 2)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class RangeObjectModelRound8Test(unittest.TestCase):
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

    def grid(self, rows=4, cols=4):
        for r in range(1, rows + 1):
            for c in range(1, cols + 1):
                self.ws.Cells(r, c).Value = "ABCDEFGH"[c - 1] + str(r)

    def v(self, ref):
        return self.ws.Range(ref).Value2

    def test_rows_and_columns_of_a_part_act_on_that_part(self):
        ws = self.ws
        self.grid()
        self.assertEqual(ws.Range("B2:C3").Rows.Address, "$B$2:$C$3")  # it read $2:$3
        self.assertEqual(ws.Range("B2:C3").Columns.Address, "$B$2:$C$3")  # $B:$C
        self.assertEqual((ws.Range("B2:C3").Rows.Count, ws.Range("B2:C3").Columns.Count), (2, 2))
        self.assertEqual(ws.Range("B2:C3").Rows(1).Address, "$B$2:$C$2")
        self.assertEqual(ws.Range("B2:C3").Columns.Item(2).Address, "$C$2:$C$3")
        self.assertEqual(ws.Range("B2:C3").Rows.Value2, [["B2", "C2"], ["B3", "C3"]])
        ws.Range("B2:C3").Rows.Delete()  # Excel: B2:C3 deleted, the cells below shift up (rows 2:3 stay)
        self.assertEqual([self.v(x) for x in ("A2", "B2", "C2", "D2", "A3", "B3", "B4")], ["A2", "B4", "C4", "D2", "A3", None, None])

        self.grid()
        ws.Range("B2:C3").Rows.ClearContents()
        self.assertEqual([self.v(x) for x in ("A2", "B2", "C3", "D3")], ["A2", None, None, "D3"])

        self.grid()
        ws.Range("B2:C3").Columns.Insert()  # cells inserted, shifting down (a square range), as Excel
        self.assertEqual([self.v(x) for x in ("A2", "B2", "B4", "C5", "D2")], ["A2", None, "B2", "C3", "D2"])

    def test_rows_and_columns_of_a_part_hide_and_group_whole_lines(self):
        ws = self.ws
        ws.Range("B2:C3").Columns.Hidden = True  # Excel hides columns B:C, not rows 2:3
        self.assertTrue(ws.Columns(2).Hidden)
        self.assertFalse(ws.Rows(2).Hidden)
        ws.Columns("B:C").Hidden = False
        ws.Range("B2:C3").Rows.Hidden = True
        self.assertTrue(ws.Rows(2).Hidden)
        self.assertFalse(ws.Columns(2).Hidden)
        ws.Rows("2:3").Hidden = False
        ws.Range("B2:C3").Columns.Group()
        self.assertEqual((ws.Columns(2).OutlineLevel, ws.Rows(2).OutlineLevel), (2, 1))
        ws.Columns("B:C").Ungroup()

    def test_methods_excel_refuses_over_several_areas(self):
        # Excel 16 (measured 2026-11-20): RemoveDuplicates, TextToColumns, ListObjects.Add and ListObject.Resize refuse
        # a range of several areas (YCGrid did the first area alone); FormulaArray fills the first area only
        from yc.excel import ApplicationDefinedError

        ws = self.ws
        ws.Range("A1:E5").Value = [[f"v{r % 2},{c}" for c in range(1, 6)] for r in range(1, 6)]
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("A1:B5,D1:E5").RemoveDuplicates(1, 1)
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("A1:A5,D1:D5").TextToColumns(ws.Range("A1"), Comma=True)
        with self.assertRaises(ApplicationDefinedError):
            ws.ListObjects.Add(1, ws.Range("A1:B5,D1:E5"), None, 1)
        self.assertEqual(ws.Range("A3").Value, "v1,1")  # nothing was done
        lo = ws.ListObjects.Add(1, ws.Range("A1:B3"), None, 1)
        with self.assertRaises(ApplicationDefinedError):
            lo.Resize(ws.Range("A1:B5,D1:E5"))
        self.assertEqual(lo.Range.Address(False, False), "A1:B3")
        lo.Unlist()
        ws.Range("G1:H2,J1:K2").FormulaArray = "=1"
        self.assertEqual((ws.Range("G1").Value, ws.Range("J1").Value, ws.Range("J1").HasArray), (1, None, False))

    def test_fill_and_refusals_over_several_areas_as_excel(self):
        # Excel 16 (measured 2026-11-20, the same calls in a hidden Excel): a fill over several areas fills each column
        # (row) from the cell of all the areas' that comes first in its direction; Sort, AutoFilter, Group, AddComment,
        # Subtotal and Resize refuse several areas, Merge refuses areas that overlap; every object has Application
        from yc.excel import ApplicationDefinedError

        ws = self.ws
        data = [["Name", "Qty", "Price", "Total", "When"], ["AAPL", 10, 1.5, None, 45000], ["MSFT", 20, 2.25, None, 45001],
                ["TSMC", 30, 3, None, 45002], ["NVDA", 40, 4.75, None, 45003], ["AMZN", 50, 5, None, 45004]]

        def fresh():
            ws.Cells.Clear()
            ws.Range("A1:E6").Value = data
            ws.Range("D2:D6").Formula = "=B2*C2"

        def f(a):
            return ws.Range(a).Formula

        fresh()
        ws.Range("B2:C3,B5:C6").FillDown()
        self.assertEqual([f(a) for a in ("B5", "C5", "B6", "C6", "B4")], ["10", "1.5", "10", "1.5", "30"])
        fresh()
        ws.Range("A1:B2,D4:E5").FillDown()
        self.assertEqual(f("E5"), "45002")
        fresh()
        ws.Range("B2:C3,C3:D4").FillDown()
        self.assertEqual(f("C4"), "1.5")
        fresh()
        ws.Range("B2:B5,D2:D5").FillRight()
        self.assertEqual([f(a) for a in ("D2", "D3", "D5", "C2")], ["10", "20", "40", "1.5"])
        fresh()
        ws.Range("B2:C3,C3:D4").FillRight()
        self.assertEqual([f(a) for a in ("D3", "D4", "C2")], ["20", "3", "10"])
        fresh()
        ws.Range("B2:C3,B5:C6").FillUp()
        self.assertEqual([f(a) for a in ("B2", "C2", "B3", "B5")], ["50", "5", "50", "50"])
        fresh()
        ws.Range("A1:B2,D4:E5").FillUp()
        self.assertEqual(f("E4"), "45003")
        fresh()
        ws.Range("B2:B5,D2:D5").FillLeft()
        self.assertEqual([f(a) for a in ("B2", "B5", "C2")], ["=#REF!*A2", "=#REF!*A5", "1.5"])
        fresh()
        ws.Range("B2:C3,C3:D4").FillLeft()
        self.assertEqual([f(a) for a in ("C3", "C4")], ["=A3*B3", "=A4*B4"])

        fresh()
        for call in (lambda r: r.Sort(r.Cells(1, 1)), lambda r: r.AutoFilter(), lambda r: r.Rows.Group(), lambda r: r.Columns.Group(),
                     lambda r: r.AddComment("n"), lambda r: r.Subtotal(1, -4157, [2]), lambda r: r.Resize(2, 2)):
            with self.assertRaises(ApplicationDefinedError):
                call(ws.Range("A1:B2,D4:E5"))
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("B2:C3,C3:D4").Merge()
        self.assertEqual(f("B2"), "10")
        # HasFormula reads every area: Null when they differ (only the first area was read)
        self.assertIsNone(ws.Range("A1:B2,D4:E5").HasFormula)
        self.assertFalse(ws.Range("A1:B2,B4:C5").HasFormula)
        self.assertTrue(ws.Range("D2:D3,D5:D6").HasFormula)
        self.assertIs(ws.Application, ws.Range("A1").Application)
        self.assertIs(ws.Parent.Application, ws.Range("A1").Application)
        ws.Cells.Clear()

    def test_one_cell_and_one_line_as_excel(self):
        # Excel 16 (measured 2026-11-20, probe_single): one cell's Find and Replace work on the whole sheet; a fill one
        # line deep takes the line beside the range, and raises at the sheet's edge; AddComment refuses several cells;
        # DataSeries without Rowcol goes down the columns only when the range has more rows than columns
        from yc.excel import ApplicationDefinedError

        ws = self.ws
        data = [["Name", "Qty", "Price"], ["AAPL", 10, 1.5], ["MSFT", 20, 2.25], ["TSMC", 30, 3]]

        def fresh():
            ws.Cells.Clear()
            ws.Range("A1:C4").Value = data

        def row(*cells):
            return [ws.Range(a).Formula for a in cells]

        fresh()
        self.assertEqual(ws.Range("A1").Find("MSFT").Address(False, False), "A3")
        self.assertEqual(ws.Range("C1").Find("MSFT").Address(False, False), "A3")
        self.assertIsNone(ws.Range("B2:B3").Find("MSFT"))
        ws.Range("C3").Replace("A", "#", 2)
        self.assertEqual(row("A2", "B1", "A4"), ["##PL", "Qty", "TSMC"])
        cases = [("FillDown", "B3:C3", ["10", "1.5"], ("B3", "C3")), ("FillRight", "B2:B4", ["AAPL", "MSFT", "TSMC"], ("B2", "B3", "B4")),
                 ("FillUp", "A3:C3", ["TSMC", "30", "3"], ("A3", "B3", "C3")), ("FillLeft", "B2:B3", ["1.5", "2.25"], ("B2", "B3")),
                 ("FillDown", "B3", ["10"], ("B3",))]
        for op, addr, want, cells in cases:
            fresh()
            getattr(ws.Range(addr), op)()
            self.assertEqual(row(*cells), want, (op, addr))
        for op, addr in (("FillDown", "A1"), ("FillRight", "A2"), ("FillUp", "A1048576"), ("FillLeft", "XFD1")):
            with self.assertRaises(ApplicationDefinedError):
                getattr(ws.Range(addr), op)()
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("B2:C3").AddComment("n")
        for addr, cells, want in (("B2:C3", ("C2", "B3", "C3"), ["11", "20", "21"]), ("B2:B4", ("B3", "B4", "C3"), ["11", "12", "2.25"]),
                                  ("B2:C4", ("B3", "C3", "C4"), ["11", "2.5", "3.5"]), ("B2:D3", ("C2", "D2", "C3"), ["11", "12", "21"])):
            fresh()
            ws.Range(addr).DataSeries()
            self.assertEqual(row(*cells), want, addr)
        # the start of a series a script just wrote as a formula has its value
        fresh()
        ws.Range("D2:D4").Formula = "=B2*C2"
        ws.Range("D2:D4").DataSeries()
        self.assertEqual(row("D3", "D4"), ["16", "17"])
        ws.Cells.Clear()

    def test_a_filtered_list_as_excel(self):
        # Excel 16 (measured 2026-11-20, probe_filter: A1:E6 filtered to Qty > 15, row 2 hidden): a whole column's
        # value skips the hidden row; FillDown over the whole column and FillUp fill the rows shown (FillDown wrote a row
        # at a time, a million pastes, and failed); a hidden row filled left is filled; cells inserted anywhere are
        # refused; Delete without Shift takes the rows shown whole; every row deleted keeps the hidden one; Find passes
        # the hidden row over
        from yc.excel import YcError

        ws = self.ws
        data = [["Name", "Qty", "Price", "Total", "When"], ["AAPL", 10, 1.5, None, 45000], ["MSFT", 20, 2.25, None, 45001],
                ["TSMC", 30, 3, None, 45002], ["NVDA", 40, 4.75, None, 45003], ["AMZN", 50, 5, None, 45004]]

        def fresh():
            if ws.AutoFilterMode:
                ws.AutoFilterMode = False
            ws.Cells.Clear()
            ws.Range("A1:E6").Value = data
            ws.Range("D2:D6").Formula = "=B2*C2"
            ws.Range("A1:E6").AutoFilter(2, ">15")

        def col(c, n=7):
            return [ws.Range(f"{c}{r}").Formula for r in range(1, n + 1)]

        fresh()
        ws.Range("B:B").Value = 5
        self.assertEqual(col("B"), ["5", "10", "5", "5", "5", "5", "5"])
        fresh()
        t0 = time.time()
        ws.Range("B:B").FillDown()
        self.assertLess(time.time() - t0, 10)
        self.assertEqual(col("B"), ["Qty", "10", "Qty", "Qty", "Qty", "Qty", "Qty"])
        fresh()
        ws.Range("B2:B6").FillUp()
        self.assertEqual(col("B"), ["Qty", "10", "50", "50", "50", "50", ""])
        fresh()
        ws.Range("A2:C2").FillLeft()
        self.assertEqual([ws.Range(a).Formula for a in ("A2", "B2", "C2")], ["1.5", "1.5", "1.5"])
        for addr, shift in (("B3:C4", -4121), ("B3:C4", -4161), ("A7:C7", -4121), ("G3:H4", -4121)):
            fresh()
            with self.assertRaises(YcError):
                ws.Range(addr).Insert(shift)
        fresh()
        ws.Range("3:4").Insert()
        self.assertEqual(ws.Range("A5").Value, "MSFT")
        fresh()
        ws.Range("B2:B6").Delete()
        self.assertEqual(col("A"), ["Name", "AAPL", "", "", "", "", ""])
        fresh()
        ws.Range("B:B").EntireRow.Delete()
        self.assertEqual([ws.Range(a).Formula for a in ("A1", "B1", "D1", "A2")], ["AAPL", "10", "=B1*C1", ""])
        fresh()
        self.assertEqual(ws.Range("A1:E6").Find("4500", LookIn=-4163).Address(False, False), "E3")
        self.assertIsNone(ws.Range("A1:E6").Find("AAPL", LookIn=-4123))
        ws.AutoFilterMode = False
        ws.Cells.Clear()

    def test_a_table_takes_formulas_inserted_rows_and_fills_as_excel(self):
        # Excel 16 (measured 2026-11-20): one formula down a whole column through a table goes into its data rows (every
        # cell got the header's "0"); rows inserted inside the data take the calculated column; a fill down past the
        # table grows it, its calculated column too
        ws = self.ws
        data = [["Name", "Qty", "Price", "Total"], ["a", 10, 1, None], ["b", 20, 2, None], ["c", 30, 3, None], ["d", 40, 4, None], ["e", 50, 5, None]]

        def fresh():
            for lo in list(ws.ListObjects()):
                lo.Unlist()
            ws.Cells.Clear()
            ws.Range("A1:D6").Value = data
            ws.Range("D2:D6").Formula = "=B2*C2"
            return ws.ListObjects.Add(1, ws.Range("A1:D6"), None, 1)

        fresh()
        ws.Range("B:B").Formula = "=ROW()"
        self.assertEqual([ws.Range(f"B{r}").Formula for r in (1, 2, 3, 9)], ["0", "=ROW()", "=ROW()", "=ROW()"])
        lo = fresh()
        ws.Range("3:4").Insert()
        self.assertEqual([ws.Range(f"D{r}").Formula for r in (3, 4, 5)], ["=B3*C3", "=B4*C4", "=B5*C5"])
        self.assertEqual(lo.Range.Address(False, False), "A1:D8")
        lo = fresh()
        ws.Range("B6:B10").FillDown()
        self.assertEqual((lo.Range.Address(False, False), ws.Range("D8").Formula, ws.Range("B10").Value), ("A1:D10", "=B8*C8", 50))
        lo = fresh()
        ws.Range("A7:C7").FillDown()
        self.assertEqual((lo.Range.Address(False, False), ws.Range("D7").Formula), ("A1:D7", "=B7*C7"))
        # Insert without Shift inside the data: table rows (B3:C4) or a table column (B2:B6), as Excel 16's; with a Shift
        # it is refused (cells shifted across part of a table)
        from yc.excel import YcError

        lo = fresh()
        ws.Range("B3:C4").Insert()
        self.assertEqual((lo.Range.Address(False, False), ws.Range("A5").Value, ws.Range("A3").Value, ws.Range("D3").Formula),
                         ("A1:D8", "b", None, "=B3*C3"))
        lo = fresh()
        ws.Range("B2:B6").Insert()
        self.assertEqual((lo.Range.Address(False, False), ws.Range("B1").Value, ws.Range("C2").Value, ws.Range("E2").Formula),
                         ("A1:E6", "Column1", 10, "=C2*D2"))
        lo = fresh()
        with self.assertRaises(YcError):
            ws.Range("B3:C4").Insert(-4121)
        # a table's buttons aren't the sheet's AutoFilter, and Unlist leaves none (Excel 16, measured): a table made
        # there later over part of the old one was refused as a change to a filtered range
        self.assertFalse(ws.AutoFilterMode)
        ws.AutoFilterMode = False
        self.assertTrue(lo.ShowAutoFilter)
        ws.Range("B2:B6").Insert()
        lo.Unlist()
        self.assertFalse(ws.AutoFilterMode)
        lo = fresh()
        lo.Unlist()
        ws.Cells.Clear()

    def test_merged_cells_as_excel(self):
        # Excel 16 (measured 2026-11-20, B3:C4 merged in A1:E6): a fill over a merged cell changes nothing (FillUp over
        # B3:C4 emptied B3); Clear and Cut of part of it are refused; the merged cell whole is one cell to Find and Replace
        from yc.excel import YcError

        ws = self.ws
        data = [["Name", "Qty", "Price", "Total", "When"], ["AAPL", 10, 1.5, None, 45000], ["MSFT", 20, 2.25, None, 45001],
                ["TSMC", 30, 3, None, 45002], ["NVDA", 40, 4.75, None, 45003], ["AMZN", 50, 5, None, 45004]]

        def fresh():
            ws.Cells.UnMerge()
            ws.Cells.Clear()
            ws.Range("A1:E6").Value = data
            ws.Range("B3:C4").Merge()

        for op, addr in (("FillUp", "B3:C4"), ("FillLeft", "B2:B6"), ("FillDown", "3:4"), ("FillRight", "B3:C4"), ("FillUp", "A3:E3")):
            fresh()
            before = ws.Range("A1:E6").Formula
            getattr(ws.Range(addr), op)()
            self.assertEqual(ws.Range("A1:E6").Formula, before, (op, addr))
        for call in (lambda: ws.Range("B3").Clear(), lambda: ws.Range("B2:B6").Clear(), lambda: ws.Range("B3").Cut(ws.Range("H1")),
                     lambda: ws.Range("A3:E3").Cut(ws.Range("H1"))):
            fresh()
            with self.assertRaises(YcError):
                call()
            self.assertEqual(ws.Range("B3").Value, 20)
        fresh()
        self.assertEqual(ws.Range("B3:C4").Find("AMZN").Address(False, False), "A6")
        ws.Range("B3:C4").Replace("A", "#", 2)
        self.assertEqual((ws.Range("A1").Value, ws.Range("A2").Value), ("N#me", "##PL"))
        ws.Cells.UnMerge()
        ws.Cells.Clear()

    def test_a_protected_sheet_as_excel(self):
        # Excel 16 (measured 2026-11-20, B:D unlocked, then Protect): AutoFit, Validation.Add, FormatConditions.Add and
        # AddComment raise (they went through); a fill with a locked cell in its target writes nothing (the unlocked
        # cells were filled); Clear of unlocked cells empties them and keeps their formats (it raised); Replace over
        # locked cells changes nothing and raises nothing (the unlocked matches were changed, or it raised)
        from yc.excel import YcError

        ws = self.ws
        data = [["Name", "Qty", "Price", "Total", "When"], ["AAPL", 10, 1.5, None, 45000], ["MSFT", 20, 2.25, None, 45001]]

        def fresh():
            ws.Unprotect()
            ws.Cells.Clear()
            ws.Range("A1:E3").Value = data
            ws.Range("D2:D3").Formula = "=B2*C2"
            ws.Range("B3").Font.Bold = True
            ws.Range("B:D").Locked = False
            ws.Protect()

        fresh()
        for call in (lambda: ws.Range("B:B").Columns.AutoFit(), lambda: ws.Range("B3").Validation.Add(1, 1, 1, "0", "10"),
                     lambda: ws.Range("B3").FormatConditions.Add(1, 5, "=2"), lambda: ws.Range("B3").AddComment("n")):
            with self.assertRaises(YcError):
                call()
        self.assertIsNone(ws.Range("B3").Comment)
        before = ws.Range("A1:E3").Formula
        with self.assertRaises(YcError):
            ws.Range("A1:E1").FillRight()
        self.assertEqual(ws.Range("A1:E3").Formula, before)
        ws.Range("B3").Clear()
        self.assertEqual((ws.Range("B3").Value, ws.Range("B3").Font.Bold, ws.Range("B3").Locked), (None, True, False))
        fresh()
        self.assertTrue(ws.Range("A1:E3").Replace("A", "#", 2))
        self.assertEqual(ws.Range("A1:E3").Formula, before)
        ws.Unprotect()
        ws.Cells.Clear()

    def test_arrays_entered_with_ctrl_shift_enter_as_excel(self):
        # Excel 16 (measured 2026-11-20, G2:G4 = {=B2:B4*2}): a formula written over a range that cuts the array changes
        # nothing and raises nothing (the cells beside it were written, G3 alone raised); an array inside the range is
        # replaced (it stayed); a sort whose list holds any of the array raises and changes nothing (the array's cells
        # were sorted as values and it moved to G1 as =B1:B3*2)
        from yc.excel import YcError

        ws = self.ws

        def fresh():
            ws.Cells.Clear()
            ws.Range("A1:B4").Value = [["Name", "Qty"], ["AAPL", 30], ["MSFT", 10], ["TSMC", 20]]
            ws.Range("G2:G4").FormulaArray = "=B2:B4*2"

        def g():
            return [ws.Range(f"G{r}").Formula for r in (1, 2, 3, 4)]

        for addr in ("G3", "2:3"):
            fresh()
            ws.Range(addr).Formula = "=ROW()"
            self.assertEqual(g(), ["", "=B2:B4*2", "=B2:B4*2", "=B2:B4*2"], addr)
            self.assertEqual(ws.Range("A2").Value, "AAPL")
        for addr in ("G2:G4", "G:G", "F2:H5"):
            fresh()
            ws.Range(addr).Formula = "=ROW()"
            self.assertEqual(g()[1:], ["=ROW()", "=ROW()", "=ROW()"], addr)
            self.assertFalse(ws.Range("G3").HasArray)
        for addr, key in (("G:G", "G1"), ("G2:G4", "G2"), ("A1:G4", "B1")):
            fresh()
            with self.assertRaises(YcError):
                ws.Range(addr).Sort(ws.Range(key))
            self.assertEqual(g(), ["", "=B2:B4*2", "=B2:B4*2", "=B2:B4*2"], addr)
            self.assertEqual(ws.Range("A2").Value, "AAPL")
        fresh()
        ws.Range("A2:B4").Sort(ws.Range("B2"))
        self.assertEqual([ws.Range(f"A{r}").Value for r in (2, 3, 4)], ["MSFT", "TSMC", "AAPL"])
        self.assertTrue(ws.Range("G3").HasArray)
        ws.Cells.Clear()

    def test_next_previous_and_prefix_character_as_excel(self):
        # Excel 16 (measured 2026-11-20): Next and Previous are the cells beside the range's first cell, as Tab goes
        # (B2:C4's Next is C2; rows 2:3's is B2, where it raised); PrefixCharacter is ' for text typed after one
        ws = self.ws
        self.assertEqual(ws.Range("B2:C4").Next.Address(), "$C$2")
        self.assertEqual(ws.Range("B2:C4").Previous.Address(), "$A$2")
        self.assertEqual(ws.Range("2:3").Next.Address(), "$B$2")
        self.assertEqual(ws.Range("A1:B2,D4:E5").Next.Address(), "$B$1")
        ws.Range("A1").Value = "'00123"
        ws.Range("A2").Value = "abc"
        self.assertEqual((ws.Range("A1").PrefixCharacter, ws.Range("A2").PrefixCharacter, ws.Range("A3").PrefixCharacter), ("'", "", ""))
        ws.Range("A1:A2").ClearContents()

    def test_find_searches_every_area_in_order(self):
        # Excel 16 (measured 2026-11-20): a range of several areas is searched as one sequence, each area by rows in
        # the areas' order, from the cell after the first one round to it; only the first area was searched
        ws = self.ws
        for a in ("B2", "F1", "F5", "A1", "XFD1048576", "H3"):
            ws.Range(a).Value = "x"

        def walk(addr):
            rng = ws.Range(addr)
            f = rng.Find("x", LookIn=-4163, LookAt=1, SearchOrder=1, MatchCase=False)
            seq = [f.Address(False, False)] if f is not None else ["None"]
            first = f
            for _ in range(6):
                if f is None:
                    break
                f = rng.FindNext(f)
                seq.append(f.Address(False, False))
                if f.Address() == first.Address():
                    break
            return " ".join(seq)

        self.assertEqual(walk("A1:B2,F1:F5"), "B2 F1 F5 A1 B2")
        self.assertEqual(walk("F1:F5,A1:B2"), "F5 A1 B2 F1 F5")
        self.assertEqual(walk("A1:B2,XFD1048576"), "B2 XFD1048576 A1 B2")
        self.assertEqual(walk("C1:D4,H3"), "H3 H3")
        self.assertEqual(walk("B2,F5"), "F5 B2 F5")
        self.assertEqual(ws.Range("A1:B2,F1:F5").Find("x", SearchOrder=2).Address(False, False), "B2")
        self.assertEqual(ws.Range("A1:B2,F1:F5").Find("x", SearchDirection=2).Address(False, False), "F5")
        self.assertEqual(ws.Range("F1:F5,A1:B2").Find("x", SearchDirection=2).Address(False, False), "B2")
        self.assertEqual(ws.Range("A1:B2,XFD1048576").Find("x", SearchDirection=2).Address(False, False), "XFD1048576")
        self.assertEqual(ws.Range("B2,F5").Find("x", SearchDirection=2).Address(False, False), "F5")

    def test_formula_array_over_whole_columns(self):
        # Excel 16 (measured 2026-11-20): Range("B:B").FormulaArray = "=1" enters one array over the column; the
        # column's end marker read as past the sheet and YCGrid answered "Invalid array formula"
        ws = self.ws
        ws.Range("B:B").FormulaArray = "=1"
        self.assertEqual((ws.Range("B2").Value, ws.Range("B1048576").Value), (1, 1))
        self.assertTrue(ws.Range("B5").HasArray)
        self.assertEqual(ws.Range("B5").FormulaArray, "=1")
        ws.Range("B:B").ClearContents()
        self.assertIsNone(ws.Range("B2").Value)

    def test_group_takes_whole_lines_as_they_are_whichever_lines_are_asked_for(self):
        # Excel 16 (measured 2026-11-20): whole rows group their rows and whole columns their columns whichever lines
        # are asked for; the whole sheet groups the lines asked for, its columns when none were. Range("3:5").Columns
        # .Group sent rows 0 to 0xFFFFFFFF, and YCGrid set four billion levels until it ran out of memory
        ws = self.ws
        t0 = time.time()
        ws.Range("3:5").Columns.Group()
        self.assertEqual((ws.Rows(3).OutlineLevel, ws.Rows(6).OutlineLevel, ws.Columns(1).OutlineLevel), (2, 1, 1))
        ws.Rows("3:5").Ungroup()
        ws.Range("B:B").Rows.Group()
        self.assertEqual((ws.Columns(2).OutlineLevel, ws.Columns(3).OutlineLevel, ws.Rows(1).OutlineLevel), (2, 1, 1))
        ws.Columns("B").Ungroup()
        ws.Cells.Rows.Group()
        self.assertEqual((ws.Rows(1).OutlineLevel, ws.Rows(1048576).OutlineLevel, ws.Columns(1).OutlineLevel), (2, 2, 1))
        ws.Cells.Rows.Ungroup()
        self.assertEqual(ws.Rows(1048576).OutlineLevel, 1)
        ws.Cells.Group()
        self.assertEqual((ws.Rows(1).OutlineLevel, ws.Columns(1).OutlineLevel, ws.Columns(16384).OutlineLevel), (1, 2, 2))
        ws.Cells.Ungroup()
        ws.Rows("1:1048576").OutlineLevel = 3  # a level for every row, given as runs
        self.assertEqual((ws.Rows(7).OutlineLevel, ws.Rows(1048576).OutlineLevel), (3, 3))
        ws.Rows("1:1048576").OutlineLevel = 1
        self.assertEqual(ws.Rows(7).OutlineLevel, 1)
        self.assertLess(time.time() - t0, 20)

    def test_addresses_past_the_last_row_and_of_whole_columns(self):
        # Excel 16 (measured): a row past the sheet is error 1004; rows 1 to the last are whole columns in Address
        from yc.excel import ApplicationDefinedError

        ws = self.ws
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("A1048577")
        self.assertEqual(ws.Range("A1:A1048576").Address, "$A:$A")
        self.assertEqual(ws.Range("B1:C1048576").Address(False, False), "B:C")
        self.assertEqual(ws.Range("A1:XFD1048576").Address, "$1:$1048576")
        self.assertEqual(ws.Range("A2:A1048576").Address, "$A$2:$A$1048576")

    def test_a_spilled_value_has_no_formula(self):
        # Excel 16 (measured): Formula and Formula2 of a cell a dynamic array spilled into are "" (it read the value)
        ws = self.ws
        ws.Range("K1").Formula = "=SEQUENCE(3)"
        self.assertEqual(ws.Range("K1").Formula, "=SEQUENCE(3)")
        self.assertEqual(ws.Range("K2").Formula, "")
        self.assertEqual(ws.Range("K2").Value, 2)
        ws.Range("K1").ClearContents()

    def test_every_row_and_column_shown_again_as_macros_write_it(self):
        # Rows.Hidden = False, Cells.EntireRow.Hidden = False: every row; Columns / Cells.EntireColumn: every column.
        # Rows.Hidden was kept on the Python object, and the whole sheet's EntireRow was taken as columns
        ws = self.ws
        for unhide in (lambda: setattr(ws.Rows, "Hidden", False), lambda: setattr(ws.Cells.EntireRow, "Hidden", False),
                       lambda: setattr(ws.Range("A:A").EntireRow, "Hidden", False)):
            ws.Rows(7).Hidden = True
            ws.Columns(3).Hidden = True
            self.assertIsNone(ws.Rows.Hidden)  # some hidden, some not: Excel's Null
            unhide()
            self.assertFalse(ws.Rows(7).Hidden)
            self.assertTrue(ws.Columns(3).Hidden)  # rows only
            ws.Columns(3).Hidden = False
        for unhide in (lambda: setattr(ws.Columns, "Hidden", False), lambda: setattr(ws.Cells.EntireColumn, "Hidden", False)):
            ws.Columns(3).Hidden = True
            ws.Rows(7).Hidden = True
            unhide()
            self.assertFalse(ws.Columns(3).Hidden)
            self.assertTrue(ws.Rows(7).Hidden)  # columns only
            ws.Rows(7).Hidden = False

    def test_delete_and_insert_act_on_every_area(self):
        from yc import excel

        ws = self.ws
        for i in range(1, 9):
            ws.Range(f"A{i}").Value = i
        ws.Range("A2,A4,A6:A7").EntireRow.Delete()  # it deleted row 2 only
        self.assertEqual([self.v(f"A{i}") for i in range(1, 6)], [1, 3, 5, 8, None])
        for i in range(1, 9):
            ws.Range(f"A{i}").Value = i
        excel.Union(ws.Range("2:2"), ws.Range("5:5")).Delete()
        self.assertEqual([self.v(f"A{i}") for i in range(1, 8)], [1, 3, 4, 6, 7, 8, None])

        ws.Range("A1:G1").Value = ["A", "B", "C", "D", "E", None, None]
        ws.Range("B:B,D:D").Insert()  # a new column before B and before D
        self.assertEqual(ws.Range("A1:G1").Value2, [["A", None, "B", "C", None, "D", "E"]])

        self.grid(6)
        ws.Range("A2:A3,C2:C3").Delete()  # each area has more rows than columns: shifts left
        self.assertEqual(ws.Range("A2:D2").Value2, [["B2", "D2", None, None]])
        self.assertEqual(self.v("A4"), "A4")

        self.grid(6)
        ws.Range("A2,A4").Insert()  # single cells shift down, each before its own cell
        self.assertEqual([self.v(f"A{i}") for i in range(1, 8)], ["A1", None, "A2", "A3", None, "A4", "A5"])

        with self.assertRaises(yc.YcError):
            ws.Range("2:3,3:4").Delete()  # Excel: can't be used on overlapping selections
        with self.assertRaises(yc.YcError):
            ws.Range("A2,4:4").Delete()  # whole rows with other cells

    def test_insert_and_delete_without_shift_follow_the_shape(self):
        ws = self.ws
        ws.Range("A1:B2").Value = [["a", "b"], ["c", "d"]]
        ws.Range("A1:A2").Insert()  # more rows than columns: shift right (it shifted down)
        self.assertEqual(ws.Range("A1:C2").Value2, [[None, "a", "b"], [None, "c", "d"]])
        self.assertIsNone(self.v("A3"))
        ws.Range("A1:C3").ClearContents()
        ws.Range("A1:B2").Value = [["a", "b"], ["c", "d"]]
        ws.Range("A1:A2").Delete()  # shift left
        self.assertEqual(ws.Range("A1:B2").Value2, [["b", None], ["d", None]])
        ws.Range("A1:C4").ClearContents()
        ws.Range("A1:A3").Value = [[1], [2], [3]]
        ws.Range("A1:C1").Delete()  # wider than tall: shift up
        self.assertEqual(ws.Range("A1:A3").Value2, [[2], [3], [None]])

    def test_copy_of_several_areas_closes_them_up(self):
        ws = self.ws
        ws.Range("A1:A4").Value = [[1], [2], [3], [4]]
        ws.Range("A1,A3").Copy(Destination=ws.Range("C1"))  # it pasted A1 only
        self.assertEqual(ws.Range("C1:C3").Value2, [[1], [3], [None]])
        ws.Range("A1,C1").Copy(Destination=ws.Range("A8"))
        self.assertEqual(ws.Range("A8:C8").Value2, [[1, 1, None]])
        # formulas paste as their values; formats come along
        ws.Range("E1").Formula = "=A1+1"
        ws.Range("E3").Formula = "=A3*2"
        ws.Range("E1").NumberFormat = "0.00"
        ws.Range("E3").Font.Bold = True
        ws.Range("E1,E3").Copy(ws.Range("G1"))
        self.assertEqual((ws.Range("G1").Formula, ws.Range("G2").Formula), ("2", "6"))
        self.assertEqual(ws.Range("G1").NumberFormat, "0.00")
        self.assertTrue(ws.Range("G2").Font.Bold)
        ws.Range("A1,A3").Copy(ws.Range("H1:H4"))  # a destination twice its size gets it twice
        self.assertEqual(ws.Range("H1:H4").Value2, [[1], [3], [1], [3]])
        with self.assertRaises(yc.YcError):
            ws.Range("A1,B3").Copy(ws.Range("J1"))  # Excel: won't work on multiple selections
        with self.assertRaises(yc.YcError):
            ws.Range("A1,A3").Cut(ws.Range("J1"))

    def test_find_remembers_order_and_look_in_and_finds_empty_cells(self):
        ws = self.ws
        ws.Range("A1").Value = "x"
        ws.Range("A2").Value = "q"
        ws.Range("B1").Value = "q"
        ws.Range("B2").Formula = "=B9"
        ws.Range("C3").Formula = "=1+1"
        r = ws.Range("A1:C5")
        self.assertEqual(r.Find("q", SearchOrder=2, LookAt=2, LookIn=-4123).Address, "$A$2")
        self.assertEqual(r.Find("q").Address, "$A$2")  # by columns, remembered (it searched by rows: $B$1)
        self.assertEqual(r.Find("B9").Address, "$B$2")  # in formulas, remembered (it found nothing)
        self.assertEqual(r.Find("q", SearchOrder=1).Address, "$B$1")
        self.assertEqual(r.Find("2", LookIn=-4163, LookAt=1).Address, "$C$3")
        self.assertIsNone(r.Find("1+1"))  # values and whole cells, remembered
        ws.Range("C1").Value = "Q"
        self.assertEqual(r.Find("Q", MatchCase=True, LookAt=2).Address, "$C$1")
        self.assertEqual(r.Find("Q").Address, "$B$1")  # MatchCase isn't remembered

        ws.Cells.Clear()
        ws.Range("A1").Value = "x"
        ws.Range("B1").Value = "y"
        self.assertEqual(ws.Range("A1:C2").Find("", LookIn=-4163, SearchOrder=1).Address, "$C$1")  # it found nothing
        self.assertEqual(ws.Range("A1:B2").Find("", SearchOrder=2).Address, "$A$2")
        self.assertIsNone(ws.Range("A1:B1").Find("", SearchOrder=1))
        self.assertEqual(ws.Range("A1:C2").Find("", After=ws.Range("C1")).Address, "$A$2")
        self.assertEqual(ws.Columns("A").Find("").Address, "$A$2")
        ws.Range("C1").Formula = '=""'
        self.assertEqual(ws.Range("A1:C2").Find("", LookIn=-4163).Address, "$C$1")  # shows nothing
        self.assertEqual(ws.Range("A1:C2").Find("", LookIn=-4123).Address, "$A$2")  # holds a formula

    def test_union_of_overlapping_ranges(self):
        from yc import excel

        u = excel.Union(self.ws.Range("A1:A3"), self.ws.Range("A2"))
        self.assertEqual((u.Address, u.Count), ("$A$1:$A$3", 3))  # it was $A$1:$A$3,$A$2 counting 4
        self.assertEqual(excel.Application.Union(self.ws.Range("A1:B2"), self.ws.Range("B1:C2")).Address, "$A$1:$C$2")

    def test_cells_is_a_collection(self):
        ws = self.ws
        self.assertEqual(ws.Range("A1:B2").Cells.Count, 4)  # AttributeError: Cells was a method
        self.assertEqual(ws.Range("A1:B2").Cells.Item(4).Address, "$B$2")
        self.assertEqual(ws.Range("B2:C3").Cells(3).Address, "$B$3")
        self.assertEqual(ws.Range("B2:D4").Cells(2, 2).Address, "$C$3")
        self.assertEqual(ws.Range("B2:C3").Rows.Cells.Count, 4)
        self.assertEqual(ws.Range("B2:C3").Cells().Address, "$B$2:$C$3")
        self.assertEqual([c.Address for c in ws.Range("A1:B2").Cells], ["$A$1", "$B$1", "$A$2", "$B$2"])
        ws.Range("A1:B2").Cells.Value = 7
        ws.Range("A1:B2").Cells.Font.Bold = True
        self.assertEqual(ws.Range("B2").Value2, 7)
        self.assertTrue(ws.Range("B2").Font.Bold)
        self.assertEqual(len(ws.Range("A1:C1").Cells), 3)

    def test_one_formula_for_several_areas_is_relative_to_the_first(self):
        ws = self.ws
        ws.Range("A1,C1").Formula = "=B1"
        self.assertEqual((ws.Range("A1").Formula, ws.Range("C1").Formula), ("=B1", "=D1"))  # C1 kept =B1
        ws.Range("A3:A4,C3:C4").Formula = "=B3"
        self.assertEqual((ws.Range("C3").Formula, ws.Range("C4").Formula, ws.Range("A4").Formula), ("=D3", "=D4", "=B4"))
        ws.Range("E1,G3").Formula = "=F1"
        self.assertEqual(ws.Range("G3").Formula, "=H3")
        ws.Range("E5,G7").Formula = "=$B$1+F5"
        self.assertEqual(ws.Range("G7").Formula, "=$B$1+H7")
        ws.Range("I1,K1").Value2 = "=J1"
        self.assertEqual(ws.Range("K1").Formula, "=L1")

    def test_replace_returns_true(self):
        ws = self.ws
        ws.Range("A1").Value = "abc"
        self.assertIs(ws.Range("A1:A2").Replace("zzz", "y", LookAt=2), True)  # it returned False
        self.assertIs(ws.Range("A1").Replace("b", "y"), True)
        self.assertEqual(ws.Range("A1").Value2, "ayc")

    def test_formula_of_a_constant_is_its_text(self):
        ws = self.ws
        ws.Range("A1").Value = 5
        ws.Range("A2").Value = "'10"
        ws.Range("A3").Value = "''abc"
        ws.Range("A4").Value = True
        ws.Range("A5").Value = 1e20
        ws.Range("A6").Value = "2024-01-31"
        ws.Range("A7").Value = 0.1
        ws.Range("A8").Value = "'=A1"
        self.assertEqual(ws.Range("A1").Formula, "5")  # it read 5.0
        self.assertEqual(ws.Range("A2").Formula, "10")  # it read '10
        self.assertEqual(ws.Range("A2").Value2, "10")
        self.assertEqual(ws.Range("A3").Formula, "'abc")  # the text itself starts with an apostrophe
        self.assertEqual(ws.Range("A4").Formula, "TRUE")
        self.assertEqual(ws.Range("A5").Formula, "100000000000000000000")
        self.assertEqual(ws.Range("A6").Formula, "45322")
        self.assertEqual(ws.Range("A7").FormulaR1C1, "0.1")
        self.assertEqual(ws.Range("A8").Formula, "=A1")
        self.assertEqual(ws.Range("A8").FormulaR1C1, "=A1")  # text, not a formula to convert
        self.assertEqual(ws.Range("A1:A3").Formula, [["5"], ["10"], ["'abc"]])
        self.assertEqual(ws.Range("A1").FormulaArray, "5")
        self.assertEqual(ws.Range("A1").Value2, 5)


if __name__ == "__main__":
    unittest.main()
