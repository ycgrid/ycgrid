# SPDX-License-Identifier: Apache-2.0
"""Every property of the object model, on a hidden YCGrid with a workbook that has one of everything (data, a
formula, a note, a hyperlink, validation, a rule, a name, a table, a PivotTable, a chart, a shape): each getter runs
and gives a value or Excel's own run-time error, and each read-write property takes back the value it read without
changing it or anything else. This found FormulaArray rewriting a whole range as an array formula, a rule's
Underline = None underlining, and a chart title's text turned into a formula."""
import collections
import inspect
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402
from yc.errors import ApplicationDefinedError, SubscriptOutOfRangeError, UnsupportedError  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.environ.get("YCGRID_EXE") or next(
    (p for p in (os.path.join(ROOT, d, "ycgrid.exe") for d in ("dist_release", "dist_debug")) if os.path.exists(p)), None)

MODULES = ("yc.excel", "yc.ui", "yc.rules", "yc.forms")
# Excel's run-time errors for a property that doesn't apply to this object (1004, 9, 438): expected, not a failure
EXPECTED = (ApplicationDefinedError, SubscriptOutOfRangeError, UnsupportedError)
# writing these back has effects beyond the object (the window, events, the session) or isn't a plain value
NO_WRITE = {"Visible", "Interactive", "WindowState", "ScreenUpdating", "DisplayAlerts", "EnableEvents", "Calculation",
            "Caption", "Left", "Top", "Width", "Height", "StatusBar", "Cursor", "AutomationSecurity", "UserName",
            "DefaultFilePath", "Name", "CodeName", "ActivePrinter", "Zoom", "ScrollRow", "ScrollColumn", "Selection",
            "ActiveCell", "ActiveSheet", "OperatorMode", "EnableCancelKey", "FreezePanes", "SplitRow", "SplitColumn",
            "Split", "DisplayFullScreen", "OnChange"}
# a cell's value or formula written over its formula: the formula is replaced, and a cell beside a table expands
# the table (AutoExpandListRange), in Excel too
VALUE_PROPS = {"Value", "Value2", "ValueNs", "Formula", "Formula2", "FormulaR1C1", "Formula2R1C1", "FormulaLocal", "FormulaR1C1Local"}


def _is_model(o):
    return type(o).__module__ in MODULES and not inspect.isclass(o)


def _props(o):
    out = {}
    for klass in reversed(type(o).__mro__):
        for k, v in vars(klass).items():
            if isinstance(v, property) and v.fget and not k.startswith("_"):
                out[k] = v
    return out


@unittest.skipIf(not EXE or os.environ.get("YC_NO_LIVE"), "needs a built ycgrid.exe")
class ObjectSweepTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        end = time.time() + 60
        while not cls.conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
            time.sleep(0.2)
        from yc import excel, safety

        safety.set_operator_mode(True)
        ws = excel.Application.ActiveSheet
        cls.ws = ws
        ws.Range("A1:D1").Value = [["Sym", "Side", "Qty", "Px"]]
        ws.Range("A2:D7").Value = [["TXF", "B", 1, 100.5], ["TXF", "S", 2, 101], ["MXF", "B", 3, 50], ["MXF", "S", 4, 51],
                                   ["TXF", "B", 5, 99], ["MXF", "B", 6, 52]]
        ws.Range("E2").Formula = "=C2*D2"
        ws.Range("A1").AddComment("a note")
        ws.Hyperlinks.Add(ws.Range("F1"), "https://example.com", TextToDisplay="site")
        ws.Range("C2:C7").Validation.Add(Type=excel.xlValidateWholeNumber, Operator=excel.xlBetween, Formula1="1", Formula2="9")
        ws.Range("D2:D7").FormatConditions.Add(Type=excel.xlCellValue, Operator=excel.xlGreater, Formula1="=60")
        excel.ActiveWorkbook.Names.Add("Qty", "=Sheet1!$C$2:$C$7")
        cls.table = ws.ListObjects.Add(excel.xlSrcRange, ws.Range("A1:D7"), XlListObjectHasHeaders=excel.xlYes)
        pws = excel.Worksheets.Add()
        pws.Name = "Pivot"
        cls.pt = excel.ActiveWorkbook.PivotCaches().Create(excel.xlDatabase, "Sheet1!A1:D7").CreatePivotTable(pws.Range("A3"), "PT1")
        cls.pt.PivotFields("Sym").Orientation = excel.xlRowField
        cls.pt.AddDataField(cls.pt.PivotFields("Qty"))
        ws.Activate()
        cls.chart = ws.ChartObjects().Add(300, 50, 300, 200)
        cls.chart.Chart.SetSourceData(ws.Range("C1:D7"))
        cls.shape = ws.Shapes.AddShape(1, 10, 300, 80, 40)
        cls.roots = {
            "Application": excel.Application, "Workbook": excel.ActiveWorkbook, "Worksheet": ws, "Range": ws.Range("A1:D7"),
            "Cell": ws.Range("E2"), "Window": excel.ActiveWindow, "ListObject": cls.table, "Name": excel.ActiveWorkbook.Names("Qty"),
            "Style": excel.ActiveWorkbook.Styles("Normal"), "Hyperlink": ws.Hyperlinks(1), "Validation": ws.Range("C2").Validation,
            "FormatCondition": ws.Range("D2").FormatConditions(1), "Comment": ws.Range("A1").Comment,
            "Characters": ws.Range("A2").Characters(1, 2), "PageSetup": ws.PageSetup, "ChartObject": cls.chart, "Shape": cls.shape,
            "PivotTable": cls.pt, "PivotField": cls.pt.PivotFields("Sym"), "PivotItem": cls.pt.PivotFields("Sym").PivotItems(1),
        }

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def state(self):
        """What a write-back must never change: the table's data, the PivotTable's fields, the objects on the sheet."""
        from yc import excel

        ws = excel.Worksheets("Sheet1")
        return (ws.Range("A1:E7").Formula, tuple(f.Name for f in self.pt.PivotFields()), ws.ListObjects.Count,
                ws.ChartObjects().Count, ws.Shapes.Count)

    def walk(self, visit):
        """visit(obj, class name, property name, property, value) for every property reached from the roots, three
        objects deep (Range.Font.ThemeFont...), each class once at each depth."""
        seen = set()

        def go(o, depth):
            for name, prop in _props(o).items():
                try:
                    v = getattr(o, name)
                except EXPECTED:
                    continue
                visit(o, type(o).__name__, name, prop, v)
                if depth < 3 and _is_model(v) and (type(v).__name__, depth) not in seen:
                    seen.add((type(v).__name__, depth))
                    go(v, depth + 1)

        for o in self.roots.values():
            go(o, 0)

    def test_every_getter_gives_a_value_or_excels_error(self):
        failures = []
        count = collections.Counter()
        seen = set()

        def go(o, depth):
            for name in _props(o):
                try:
                    v = getattr(o, name)
                    count["read"] += 1
                except EXPECTED:
                    count["expected"] += 1
                    continue
                except Exception as e:  # noqa: BLE001
                    failures.append(f"{type(o).__name__}.{name}: {type(e).__name__}: {e}")
                    continue
                if depth < 3 and _is_model(v) and (type(v).__name__, depth) not in seen:
                    seen.add((type(v).__name__, depth))
                    go(v, depth + 1)

        for o in self.roots.values():
            go(o, 0)
        self.assertGreater(count["read"], 900, "the sweep reached the object model")
        self.assertEqual(failures, [])

    def test_every_property_takes_back_what_it_read(self):
        failures = []
        written = [0]

        def visit(o, cls, name, prop, v):
            if prop.fset is None or name in NO_WRITE or _is_model(v):
                return
            if name in VALUE_PROPS and getattr(o, "HasFormula", False) is not False:
                return
            before = self.state()
            try:
                setattr(o, name, v)
            except EXPECTED:
                return
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cls}.{name} = {v!r}: {type(e).__name__}: {e}")
                return
            written[0] += 1
            after = self.state()
            if after != before:
                where = o.Address(External=True) if hasattr(o, "Address") and callable(o.Address) else ""
                failures.append(f"{cls}.{name} = {v!r} changed the workbook {where}")
            back = getattr(o, name)
            same = back == v or (isinstance(v, float) and isinstance(back, float) and abs(v - back) < 1e-9)
            if not same:
                failures.append(f"{cls}.{name}: wrote {v!r}, read {back!r}")

        self.walk(visit)
        self.assertGreater(written[0], 400, "the sweep wrote properties back")
        self.assertEqual(failures, [])

    def test_formula_array_of_several_plain_cells_is_null_and_text_is_a_constant(self):
        from yc import excel

        ws = excel.Worksheets.Add()
        try:
            ws.Range("A1:B2").Value = [[1, 2], [3, 4]]
            self.assertIsNone(ws.Range("A1:B2").FormulaArray)  # Excel's Null: not one array formula
            self.assertEqual(ws.Range("A1").FormulaArray, "1")  # a constant's formula is its text, as Excel's
            ws.Range("A1:B2").FormulaArray = None  # nothing to enter
            self.assertEqual(ws.Range("B2").Value, 4)
            ws.Range("C1:C2").FormulaArray = "7"  # a constant, not =7 as an array
            self.assertEqual(ws.Range("C2").Value, 7)
            self.assertFalse(ws.Range("C1").HasArray)
            ws.Range("D1:D2").FormulaArray = "=ROW(D1:D2)"
            self.assertTrue(ws.Range("D1").HasArray)
            self.assertEqual(ws.Range("D1:D2").FormulaArray, "=ROW(D1:D2)")
        finally:
            ws.Delete()
