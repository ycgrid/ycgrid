# SPDX-License-Identifier: Apache-2.0
"""Round 8: conditional formatting and data validation from Python, as Excel 16 does them (measured), each test
failing without its fix.

Run: python -m unittest tests.test_round8_rules (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402
from yc.errors import ApplicationDefinedError  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class RulesRound8Test(unittest.TestCase):
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

        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()

    # ---- conditional formatting ----
    def test_format_conditions_add_appends_the_rule_last(self):
        # Excel's object model gives a new rule the last priority (the UI puts it on top)
        ws = self.ws
        ws.Range("A1:A3").Value2 = [[5], [5], [5]]
        f1 = ws.Range("A1:A3").FormatConditions.Add(1, 5, "1")
        f2 = ws.Range("A1:A3").FormatConditions.Add(1, 5, "2")
        f3 = ws.Range("C1").FormatConditions.AddTop10()
        self.assertEqual((f1.Priority, f2.Priority, f3.Priority), (1, 2, 3))
        self.assertEqual(ws.Range("A1:A3").FormatConditions(1).Formula1, "=1")

    def test_a_rule_added_with_its_equals_sign_edits_without_a_second_one(self):
        # FormatConditions.Add(..., "=10") kept the =, and the Rules Manager's Edit Rule showed ==10
        from yc import ui

        ws = self.ws
        ws.Range("A1:A3").FormatConditions.Add(1, 5, "=10")
        ws.Range("A1:A3").FormatConditions.Add(2, None, "=A1>5")
        self.assertEqual(ws.Range("A1").FormatConditions(1).Formula1, "=10")
        self.assertEqual(ws.Range("A1").FormatConditions(2).Formula1, "=A1>5")
        def keys(script):
            self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
            time.sleep(0.2)

        keys("g:A1:A3|w:2")
        ui.command("CfManageRules")
        keys("w:6")
        labels = [i["label"] for i in ui.items("")]
        self.assertIn("Cell value greater than 10", labels)
        self.assertIn("Formula: =A1>5", labels)
        keys("k:Escape|w:4")

    def test_formula1_of_rules_without_one_is_excels_formula(self):
        ws = self.ws
        fcs = ws.Range("C1:C4").FormatConditions
        t = fcs.Add(9, String="ab", TextOperator=0)
        self.assertEqual(t.Formula1, '=NOT(ISERROR(SEARCH("ab",C1)))')
        self.assertEqual(fcs.Add(9, String="a", TextOperator=1).Formula1, '=ISERROR(SEARCH("a",C1))')
        self.assertEqual(fcs.Add(9, String='x"y', TextOperator=2).Formula1, '=LEFT(C1,LEN("x""y"))="x""y"')
        self.assertEqual(fcs.Add(9, String="C", TextOperator=3).Formula1, '=RIGHT(C1,LEN("C"))="C"')
        b = ws.Range("B2:B5").FormatConditions
        self.assertEqual(b.Add(10).Formula1, "=LEN(TRIM(B2))=0")
        self.assertEqual(b.Add(13).Formula1, "=LEN(TRIM(B2))>0")
        self.assertEqual(b.Add(16).Formula1, "=ISERROR(B2)")
        self.assertEqual(b.Add(17).Formula1, "=NOT(ISERROR(B2))")
        self.assertEqual(b.Add(11, DateOperator=0).Formula1, "=FLOOR(B2,1)=TODAY()")
        self.assertEqual(b.Add(11, DateOperator=2).Formula1, "=AND(TODAY()-FLOOR(B2,1)<=6,FLOOR(B2,1)<=TODAY())")
        last = b.Add(11, DateOperator=9)
        self.assertEqual(last.Formula1, "=AND(MONTH(B2)=MONTH(TODAY()),YEAR(B2)=YEAR(TODAY()))")
        # the formula it reads, written back, changes nothing
        t.Formula1 = t.Formula1
        self.assertEqual(t.Type, 9)
        self.assertEqual(t.Formula1, '=NOT(ISERROR(SEARCH("ab",C1)))')

    # ---- data validation ----
    def test_validation_value_checks_the_cells_value(self):
        ws = self.ws
        ws.Range("E1").Formula = '="b"'
        ws.Range("E1").Validation.Add(3, 1, Formula1="a,b,c")
        self.assertTrue(ws.Range("E1").Validation.Value, '="b" is the text b')
        ws.Range("E2").Value2 = "'1"
        ws.Range("E2").Validation.Add(3, 1, Formula1="1,2,3")
        self.assertTrue(ws.Range("E2").Validation.Value, "the text 1 is in the typed list 1,2,3")
        ws.Range("E3").Value2 = "'TRUE"
        ws.Range("E3").Validation.Add(3, 1, Formula1="TRUE,FALSE")
        self.assertTrue(ws.Range("E3").Validation.Value)
        ws.Range("E4").Formula = "=0.1+0.2"
        ws.Range("E4").Validation.Add(6, 1, 8, "3")
        self.assertTrue(ws.Range("E4").Validation.Value, "0.1+0.2 is 0.3 as text: 3 characters")
        ws.Range("E5").Value2 = True
        ws.Range("E5").Validation.Add(1, 1, 5, "3")
        self.assertTrue(ws.Range("E5").Validation.Value, "TRUE is above every number")
        ws.Range("E6").Value2 = True
        ws.Range("E6").Validation.Add(1, 1, 1, "1", "10")
        self.assertFalse(ws.Range("E6").Validation.Value)
        # a list of cells: the text 3 isn't the number 3
        ws.Range("Z2:Z4").Value2 = [["x"], ["y"], [3]]
        ws.Range("E7").Value2 = "'3"
        ws.Range("E7").Validation.Add(3, 1, Formula1="=$Z$2:$Z$4")
        self.assertFalse(ws.Range("E7").Validation.Value)
        ws.Range("E8").Value2 = 3
        ws.Range("E8").Validation.Add(3, 1, Formula1="=$Z$2:$Z$4")
        self.assertTrue(ws.Range("E8").Validation.Value)

    def test_custom_validation_with_ignore_blank_passes_on_a_blank_reference(self):
        ws = self.ws
        ws.Range("A1:A5").Value2 = [[1], [3], [2], [5], [4]]
        ws.Range("A1:A5").Validation.Add(7, 1, Formula1="=A1<A2")
        self.assertTrue(ws.Range("A1").Validation.Value)
        self.assertFalse(ws.Range("A2").Validation.Value)
        self.assertTrue(ws.Range("A5").Validation.Value, "A6 is empty: Ignore blank lets A5 pass")

    def test_validation_of_cells_with_different_rules_raises(self):
        ws = self.ws
        ws.Range("F1").Validation.Add(1, 1, 1, "1", "10")
        ws.Range("F2").Validation.Add(3, 1, Formula1="a,b")
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("F1:F2").Validation.Type
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("F1:F2").Validation.Formula1
        ws.Range("G4:G5").Validation.Add(1, 1, 1, "1", "10")
        self.assertEqual(ws.Range("G4:G5").Validation.Type, 1)
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("G4:G6").Validation.Type  # G6 has none
        self.assertEqual(ws.Range("F1").Validation.Type, 1)

    def test_validation_add_refuses_what_excel_refuses(self):
        ws = self.ws
        ws.Range("H4:H5").Validation.Add(1, 1, 1, "1", "10")
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("H4").Validation.Add(2, 1, 1, "0", "1")  # a rule is there already
        self.assertEqual(ws.Range("H4").Validation.Type, 1)
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("H3:H4").Validation.Add(2, 1, 1, "0", "1")  # one cell of it has a rule
        self.assertEqual(ws.Range("H4:H5").Validation.Type, 1)
        refused = [((3, 1), {}), ((1, 1, 1, "1"), {}), ((1, 1, 1, "abc", "3"), {}), ((1, 1, 1, "10", "1"), {}), ((1,), {})]
        for row, (args, kw) in enumerate(refused, start=8):
            with self.assertRaises(ApplicationDefinedError, msg=repr(args)):
                ws.Range(f"H{row}").Validation.Add(*args, **kw)
            with self.assertRaises(ApplicationDefinedError):
                ws.Range(f"H{row}").Validation.Type  # nothing was added
        ws.Range("J1").Validation.Add(3, 1, Formula1="ab," * 100)
        ws.Range("J2").Validation.Add(4, 1, 5, "2026-01-01")
        ws.Range("J3").Validation.Add(5, 1, 5, "9:30")
        ws.Range("J4").Validation.Add(0)
        self.assertEqual([ws.Range(f"J{r}").Validation.Type for r in range(1, 5)], [3, 4, 5, 0])

    def test_validation_modify_with_formula1_leaves_no_formula2(self):
        ws = self.ws
        ws.Range("K4:K5").Validation.Add(1, 1, 1, "1", "10")
        ws.Range("K5").Validation.Modify(2, 1, 5, "0")
        v = ws.Range("K5").Validation
        self.assertEqual((v.Type, v.Operator, v.Formula1, v.Formula2), (2, 5, "0", ""))
        self.assertEqual(ws.Range("K4").Validation.Type, 1)
        with self.assertRaises(ApplicationDefinedError):
            ws.Range("K4").Validation.Modify(1, 1, 1, "5")  # Between without Formula2


if __name__ == "__main__":
    unittest.main()
