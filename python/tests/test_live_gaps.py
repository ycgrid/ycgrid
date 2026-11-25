# SPDX-License-Identifier: Apache-2.0
"""The members of Excel's object model YCGrid didn't have and traders, model training and statistics use
(docs/excel-object-model.md), each against a hidden YCGrid, as a person or a ported macro uses it. The expected values
are Excel 16's where Excel was measured (the probes are with the commit that added them)."""
import datetime as dt
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.environ.get("YCGRID_EXE") or next(
    (p for p in (os.path.join(ROOT, d, "ycgrid.exe") for d in ("dist_release", "dist_debug")) if os.path.exists(p)), None)

MAIN = '''from yc.excel import *
import time


@udf
def PriceBS(spot, vol=0.2):
    """Black-Scholes price of a call."""
    return spot * vol


@udf
def WhoCalls():
    c = Application.ThisCell
    return c.Address if c is not None else "none"


def Workbook_Open():
    Application.MacroOptions(Macro="PriceBS", Description="Prices a call.", Category="Financial",
                             ArgumentDescriptions=["the underlying's price", "the volatility"])


def LongLoop():
    Range("Z1").Value = "started"
    end = time.time() + 20
    while time.time() < end:
        time.sleep(0.01)
    Range("Z1").Value = "finished"


def LongLoopTrapped():
    Application.EnableCancelKey = xlErrorHandler
    Range("Z1").Value = "started"
    try:
        end = time.time() + 20
        while time.time() < end:
            time.sleep(0.01)
        Range("Z1").Value = "finished"
    except CodeInterruptedError:
        Range("Z1").Value = "trapped"


def LongLoopDisabled():
    Application.EnableCancelKey = xlDisabled
    Range("Z1").Value = "started"
    end = time.time() + 3
    while time.time() < end:
        time.sleep(0.01)
    Range("Z1").Value = "finished"


def CancelKeyAfter():
    Application.EnableCancelKey = xlDisabled
    return Application.EnableCancelKey
'''


def _skip():
    return EXE is None or os.environ.get("YC_NO_LIVE") == "1"


class _Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="yc_gaps_")
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        from yc import excel

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    @staticmethod
    def wait(check, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if check():
                return True
            time.sleep(0.1)
        return bool(check())


@unittest.skipIf(_skip(), "no built ycgrid.exe")
class ApplicationWindowTest(_Base):
    def test_caption_is_window_then_application_as_excel(self):
        from yc import excel

        app, win = excel.Application, excel.ActiveWindow
        self.assertTrue(app.Caption.endswith(" - YCGrid"), app.Caption)
        app.Caption = "Desk"
        self.assertTrue(app.Caption.endswith(" - Desk"))
        win.Caption = "Quotes"
        self.assertEqual(app.Caption, "Quotes - Desk")
        self.assertEqual(win.Caption, "Quotes")
        app.Caption = None
        self.assertEqual(app.Caption, "Quotes - YCGrid")
        win.Caption = None
        self.assertTrue(app.Caption.endswith(" - YCGrid") and not app.Caption.startswith("Quotes"))

    def test_status_bar_and_full_screen(self):
        from yc import excel

        app = excel.Application
        h0 = app.UsableHeight
        app.DisplayStatusBar = False
        self.keys("w:4")
        self.assertFalse(app.DisplayStatusBar)
        self.assertGreater(app.UsableHeight, h0)  # the sheets get the status bar's room
        app.DisplayStatusBar = True
        app.DisplayFullScreen = True
        self.keys("w:4")
        try:
            self.assertTrue(app.DisplayFullScreen)
            self.assertFalse(app.DisplayFormulaBar)
            self.assertFalse(app.DisplayStatusBar)
            self.assertEqual(app.WindowState, excel.xlMaximized)
            self.assertGreater(app.UsableHeight, h0)
        finally:
            app.DisplayFullScreen = False
        self.keys("w:4")
        self.assertTrue(app.DisplayFormulaBar)
        self.assertTrue(app.DisplayStatusBar)
        self.assertAlmostEqual(app.UsableHeight, h0, delta=1)

    def test_window_state_place_cursor_and_ready(self):
        from yc import excel

        app = excel.Application
        app.WindowState = excel.xlNormal
        self.assertEqual(app.WindowState, excel.xlNormal)
        self.assertEqual(excel.ActiveWindow.WindowState, excel.xlNormal)
        app.Width = 700
        app.Left = 40
        self.assertAlmostEqual(app.Width, 700, delta=2)
        self.assertAlmostEqual(excel.ActiveWindow.Left, 40, delta=2)
        self.assertGreater(excel.ActiveWindow.UsableWidth, 100)
        app.Cursor = excel.xlWait
        self.assertEqual(app.Cursor, excel.xlWait)
        app.Cursor = excel.xlDefault
        self.assertTrue(app.Ready)
        self.keys("g:B2|w:2|t:abc|w:3")
        self.assertFalse(app.Ready)  # a cell being edited
        self.keys("k:Escape|w:3")
        self.assertTrue(app.Ready)
        with self.assertRaises(excel.ApplicationDefinedError):
            app.WindowState = 5

    def test_panes_of_a_split_window(self):
        from yc import excel

        win = excel.ActiveWindow
        self.assertEqual(win.Panes.Count, 1)
        win.SplitRow = 5
        win.SplitColumn = 2
        try:
            self.keys("w:4")
            self.assertEqual(win.Panes.Count, 4)
            self.assertTrue(win.Panes(1).VisibleRange.Address.startswith("$A$1:$B$5"))
            self.assertTrue(win.Panes(2).VisibleRange.Address.startswith("$A$6"))  # bottom left
            self.assertTrue(win.Panes(3).VisibleRange.Address.startswith("$C$1"))  # top right
            win.Panes(4).ScrollRow = 30
            self.keys("w:4")
            self.assertTrue(win.Panes(2).VisibleRange.Address.startswith("$A$30"), win.Panes(2).VisibleRange.Address)
            self.assertEqual(win.Panes(3).ScrollRow, 30)  # Excel 16 reads panes 2 and 3 crossed (measured)
            self.assertEqual(win.ActivePane.Index, 1)
        finally:
            win.SplitRow = 0
            win.SplitColumn = 0
        self.assertEqual(win.Panes.Count, 1)

    def test_frozen_panes_are_numbered_across_and_scroll_what_scrolls(self):
        # Excel 16, measured: frozen at C13, 2 is the top right; a frozen pane asked to scroll scrolls the others
        from yc import excel

        win = excel.ActiveWindow
        self.ws.Range("C13").Select()
        win.FreezePanes = True
        try:
            win.Panes(4).ScrollColumn = 12
            self.assertTrue(win.Panes(1).VisibleRange.Address.startswith("$A$1:$B$12"), win.Panes(1).VisibleRange.Address)
            self.assertTrue(win.Panes(2).VisibleRange.Address.startswith("$L$1:"), win.Panes(2).VisibleRange.Address)
            self.assertTrue(win.Panes(3).VisibleRange.Address.startswith("$A$13:$B$"), win.Panes(3).VisibleRange.Address)
            self.assertTrue(win.Panes(4).VisibleRange.Address.startswith("$L$13:"), win.Panes(4).VisibleRange.Address)
            self.assertEqual((win.Panes(2).ScrollRow, win.Panes(2).ScrollColumn), (1, 12))
            self.assertEqual((win.Panes(3).ScrollRow, win.Panes(3).ScrollColumn), (13, 1))
            win.Panes(1).ScrollColumn = 17
            self.assertTrue(win.Panes(1).VisibleRange.Address.startswith("$A$1:$B$12"), win.Panes(1).VisibleRange.Address)
            self.assertEqual(win.Panes(1).ScrollColumn, 1)
            self.assertEqual(win.Panes(4).ScrollColumn, 17)
            win.Panes(3).ScrollRow = 93
            self.assertTrue(win.Panes(4).VisibleRange.Address.startswith("$Q$93:"), win.Panes(4).VisibleRange.Address)
        finally:
            win.FreezePanes = False

    def test_international_threads_interrupt_key_and_file_dialog(self):
        from yc import excel, ui

        app = excel.Application
        self.assertEqual(app.International(excel.xlDecimalSeparator), ".")
        self.assertEqual(app.International(excel.xlListSeparator), ",")
        self.assertEqual(app.International(excel.xlDateOrder), 0)
        mtc = app.MultiThreadedCalculation
        try:
            mtc.ThreadCount = 2
            self.assertEqual(mtc.ThreadCount, 2)
            self.assertEqual(mtc.ThreadMode, excel.xlThreadModeManual)
            mtc.Enabled = False
            self.assertFalse(mtc.Enabled)
        finally:
            mtc.Enabled = True
            mtc.ThreadMode = excel.xlThreadModeAutomatic
        self.assertEqual(mtc.ThreadMode, excel.xlThreadModeAutomatic)
        self.assertEqual(app.CalculationInterruptKey, excel.xlAnyKey)
        app.CalculationInterruptKey = excel.xlEscKey
        self.assertEqual(app.CalculationInterruptKey, excel.xlEscKey)
        app.CalculationInterruptKey = excel.xlAnyKey
        app.CheckAbort()  # nothing calculates: nothing happens
        fd = app.FileDialog(excel.msoFileDialogFolderPicker)
        self.assertEqual(fd.Show(), 0)  # a hidden instance: nobody picks
        ui.answer_file_dialogs(self.tmp)
        fd = app.FileDialog(excel.msoFileDialogFolderPicker)
        self.assertEqual(fd.Show(), -1)
        self.assertEqual(os.path.normcase(fd.SelectedItems(1)), os.path.normcase(self.tmp))

    def test_options_of_the_options_page_from_python(self):
        from yc import excel

        app = excel.Application
        for name in ("ShowChartTipNames", "ShowChartTipValues", "CalculateBeforeSave", "FormatStaleValues", "GenerateGetPivotData"):
            was = getattr(app, name)
            setattr(app, name, not was)
            self.assertEqual(getattr(app, name), not was, name)
            setattr(app, name, was)
        self.assertEqual(app.LargeOperationCellThousandCount, 33554)

    def test_large_operation_alert_asks_before_a_fill_of_many_cells(self):
        from yc import excel, ui

        app = excel.Application
        app.EnableLargeOperationAlert = True
        app.LargeOperationCellThousandCount = 1
        try:
            self.ws.Range("A1").Value = 7
            self.keys("g:A1:A3000|w:3|k:c-D|w:5")
            self.assertIn("large number of cells", ui.state().get("dialog_text", ""))
            self.assertIsNone(self.ws.Range("A3000").Value)  # nothing yet
            self.keys("k:Enter|w:8")
            self.assertTrue(self.wait(lambda: self.ws.Range("A3000").Value == 7))
            self.ws.Range("B1").Value = 1
            self.keys("g:B1:B10|w:3|k:c-D|w:5")  # under the count: no question
            self.assertEqual(ui.state().get("dialog", 0), 0)
            self.assertEqual(self.ws.Range("B10").Value, 1)
        finally:
            app.LargeOperationCellThousandCount = 33554
            app.EnableLargeOperationAlert = False


@unittest.skipIf(_skip(), "no built ycgrid.exe")
class WorkbookCalculationTest(_Base):
    def test_date1904_keeps_numbers_and_python_dates(self):
        from yc import excel

        wb = excel.ActiveWorkbook
        self.ws.Range("A1").Value = dt.datetime(2024, 1, 2)
        self.assertEqual(self.ws.Range("A1").Value2, 45293)
        wb.Date1904 = True
        try:
            self.assertTrue(wb.Date1904)
            self.assertEqual(self.ws.Range("A1").Value2, 45293)  # the number stays (Excel 16)
            self.assertEqual(self.ws.Range("A1").Value, dt.datetime(2028, 1, 3))  # four years and a day later
            self.ws.Range("A2").Value = dt.datetime(2024, 1, 2)
            self.assertEqual(self.ws.Range("A2").Value2, 45293 - 1462)
            self.assertEqual(self.ws.Range("A2").Value, dt.datetime(2024, 1, 2))
            self.ws.Range("A3").Formula = "=DATE(2020,1,1)"
            self.assertEqual(self.ws.Range("A3").Value2, 42369)
        finally:
            wb.Date1904 = False
        self.assertEqual(self.ws.Range("A1").Value, dt.datetime(2024, 1, 2))

    def test_precision_as_displayed_and_force_full_calculation(self):
        from yc import excel

        wb = excel.ActiveWorkbook
        self.ws.Range("A1").Value = 1.23456
        self.ws.Range("A1").NumberFormat = "0.00"
        wb.PrecisionAsDisplayed = True
        try:
            self.assertEqual(self.ws.Range("A1").Value, 1.23)
            self.ws.Range("A2").NumberFormat = "0.0"
            self.ws.Range("A2").Value = 9.87
            self.assertEqual(self.ws.Range("A2").Value, 9.9)
        finally:
            wb.PrecisionAsDisplayed = False
        self.assertFalse(wb.PrecisionAsDisplayed)
        self.assertEqual(self.ws.Range("A1").Value, 1.23)  # for good
        wb.ForceFullCalculation = True
        self.assertTrue(wb.ForceFullCalculation)
        wb.ForceFullCalculation = False

    def test_sheet_calculation_off_and_on(self):
        self.ws.Range("B1").Value = 5
        self.ws.Range("C1").Formula = "=B1*2"
        self.ws.EnableCalculation = False
        try:
            self.assertFalse(self.ws.EnableCalculation)
            self.ws.Range("B1").Value = 7
            self.assertEqual(self.ws.Range("C1").Value, 10)
            self.ws.Range("D1").Formula = "=1+1"
            self.assertEqual(self.ws.Range("D1").Value, 0)
        finally:
            self.ws.EnableCalculation = True
        self.assertEqual(self.ws.Range("C1").Value, 14)
        self.assertEqual(self.ws.Range("D1").Value, 2)

    def test_display_format_reads_the_conditional_format(self):
        from yc import excel

        self.ws.Range("A1:A2").Value = [[3], [9]]
        fc = self.ws.Range("A1:A2").FormatConditions.Add(excel.xlCellValue, excel.xlGreater, "5")
        fc.Interior.Color = 255  # red
        fc.Font.Bold = True
        self.assertEqual(self.ws.Range("A2").DisplayFormat.Interior.Color, 255)
        self.assertTrue(self.ws.Range("A2").DisplayFormat.Font.Bold)
        self.assertNotEqual(self.ws.Range("A1").DisplayFormat.Interior.Color, 255)
        self.assertNotEqual(self.ws.Range("A2").Interior.Color, 255)  # the cell's own format is unchanged
        with self.assertRaises(excel.ApplicationDefinedError):
            self.ws.Range("A2").DisplayFormat.Font.Bold = False
        self.ws.EnableFormatConditionsCalculation = False
        self.assertFalse(self.ws.EnableFormatConditionsCalculation)
        self.ws.EnableFormatConditionsCalculation = True

    def test_protection_enables_and_consolidation(self):
        from yc import excel

        for name in ("EnableAutoFilter", "EnableOutlining", "EnablePivotTable"):
            self.assertFalse(getattr(self.ws, name))
            setattr(self.ws, name, True)
            self.assertTrue(getattr(self.ws, name), name)
            setattr(self.ws, name, False)
        self.assertEqual(self.ws.ConsolidationFunction, excel.xlSum)
        self.assertIsNone(self.ws.ConsolidationSources)
        self.ws.Range("A1:A2").Value = [[1], [2]]
        self.ws.Range("C1").Consolidate([f"{self.ws.Name}!R1C1:R2C1"], excel.xlAverage, TopRow=False, LeftColumn=False)
        self.assertEqual(self.ws.ConsolidationFunction, excel.xlAverage)
        self.assertEqual(self.ws.ConsolidationOptions, [False, False, False])
        self.assertEqual(self.ws.ConsolidationSources, [f"{self.ws.Name}!R1C1:R2C1"])

    def test_outlining_on_a_protected_sheet_with_enable_outlining(self):
        from yc import ui

        self.ws.Range("A2:A4").Rows.Group()
        self.ws.Protect(UserInterfaceOnly=True)
        try:
            self.keys("g:A3|w:2")
            try:
                ui.command("HideDetail")  # a person's Hide Detail on a protected sheet: refused
            except yc.errors.ProtectedError:
                pass  # the refusal reaches the request when the engine answers before it returns
            self.keys("w:6")
            self.assertFalse(self.ws.Rows(3).Hidden)
            self.ws.EnableOutlining = True
            ui.command("HideDetail")  # with EnableOutlining the outline works
            self.keys("w:6")
            self.assertTrue(self.wait(lambda: self.ws.Rows(3).Hidden))
        finally:
            self.ws.EnableOutlining = False
            self.ws.Unprotect()
            self.keys("k:Escape|w:2")


@unittest.skipIf(_skip(), "no built ycgrid.exe")
class FilesTest(_Base):
    def test_a_new_workbook_is_the_window_it_started_not_one_started_meanwhile(self):
        # another YCGrid starting while Workbooks.Add waits for its window (another script, a person) was taken for
        # it, and Workbooks.OpenText wrote its file into that window (gaps_fuzz, two runs side by side)
        from unittest import mock

        from yc import excel

        self.ws.Range("A1").Value2 = "in use"  # a workbook in use: Add opens a new window
        real = excel._rq
        intruder = []

        def rq(method, **params):
            if method == "wb.new_window" and not intruder:
                intruder.append(yc.launch(hidden=True, exe=EXE, lang="en"))
                yc.connect(target=self.proc.pid)  # launch talks to the new one: this script stays with its own
                deadline = time.time() + 20
                while time.time() < deadline and not any(i.get("pid") == intruder[0].pid for i in excel._other_instances()):
                    time.sleep(0.1)
            return real(method, **params)

        try:
            with mock.patch.object(excel, "_rq", rq):
                wb = excel.Workbooks.Add()
            self.assertTrue(intruder)
            wb.Worksheets(1).Range("A1").Value2 = "mine"
            other = excel._remote_workbook(next(i for i in excel._other_instances() if i.get("pid") == intruder[0].pid))
            self.assertIsNone(other.Worksheets(1).Range("A1").Value2)  # nothing was written into the intruder
            wb.Close(False)
        finally:
            yc.connect(target=self.proc.pid)
            for p in intruder:
                p.terminate()
                p.wait(10)

    def test_open_text_keeps_codes_as_text_and_saves_a_sheet_as_csv(self):
        from yc import excel

        path = os.path.join(self.tmp, "quotes.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("skip this line\ncode,name,price\n0050,ETF,150.5\n2330,TSMC,1085\n")
        me = self.proc.pid
        wb = excel.Workbooks.OpenText(path, StartRow=2, Comma=True, FieldInfo=[[1, excel.xlTextFormat], [2, excel.xlGeneralFormat]])
        try:
            ws = wb.Worksheets(1)
            self.assertEqual(ws.Name, "quotes")
            self.assertEqual(ws.Range("A1").Value, "code")
            self.assertEqual(ws.Range("A2").Value, "0050")  # a Text column keeps its zeros
            self.assertEqual(ws.Range("C3").Value, 1085)
            out = os.path.join(self.tmp, "out.csv")
            ws.SaveAs(out)
            with open(out, encoding="utf-8-sig") as f:
                self.assertIn("0050", f.read())
        finally:
            try:
                wb.Close(SaveChanges=False)
            except Exception:  # noqa: BLE001 - the window may be gone already
                pass
            yc.connect(target=me)


@unittest.skipIf(_skip(), "no built ycgrid.exe")
class PivotTest(_Base):
    def make(self):
        from yc import excel

        data = excel.Worksheets.Add()
        data.Name = f"D{int(time.time() * 1000) % 100000}"
        data.Range("A1:D6").Value = [["Region", "Year", "Product", "Sales"], ["East", 2023, "A", 10], ["East", 2024, "B", 20],
                                     ["West", 2023, "A", 30], ["West", 2024, "B", 40], ["North", 2024, "A", 5]]
        dst = excel.Worksheets.Add()
        PivotTest.n = getattr(PivotTest, "n", 0) + 1
        pc = excel.ActiveWorkbook.PivotCaches.Create(excel.xlDatabase, data.Range("A1:D6"))
        pt = pc.CreatePivotTable(dst.Range("A3"), f"Gaps{PivotTest.n}")
        pt.PivotFields("Region").Orientation = excel.xlRowField
        pt.PivotFields("Year").Orientation = excel.xlColumnField
        pt.PivotFields("Product").Orientation = excel.xlPageField
        pt.AddDataField(pt.PivotFields("Sales"), "Sum of Sales", excel.xlSum)
        return dst, pt

    def test_report_areas_and_cells_as_excel_measures_them(self):
        from yc import excel

        dst, pt = self.make()
        # Excel 16, measured on the same report
        self.assertEqual(pt.TableRange1.Address, "$A$3:$D$8")
        self.assertEqual(pt.RowRange.Address, "$A$4:$A$8")
        self.assertEqual(pt.ColumnRange.Address, "$B$3:$D$4")
        self.assertEqual(pt.DataLabelRange.Address, "$A$3")
        self.assertEqual(pt.DataBodyRange.Address, "$B$5:$D$8")
        self.assertEqual(pt.PageRange.Address, "$A$1:$B$1")
        east = pt.PivotFields("Region").PivotItems("East")
        self.assertEqual(east.DataRange.Address, "$B$5:$C$5")
        self.assertEqual(east.LabelRange.Address, "$A$5")
        y = pt.PivotFields("Year").PivotItems("2024")
        self.assertEqual(y.DataRange.Address, "$C$5:$C$7")
        self.assertEqual(y.LabelRange.Address, "$C$4")
        types = {a: dst.Range(a).PivotCell.PivotCellType for a in ("A1", "B1", "A3", "B3", "A4", "B4", "A5", "B5", "A8", "B8")}
        self.assertEqual(types, {"A1": 5, "B1": 6, "A3": 4, "B3": 9, "A4": 9, "B4": 1, "A5": 1, "B5": 0, "A8": 3, "B8": 0})
        with self.assertRaises(excel.ApplicationDefinedError):
            dst.Range("A9").PivotCell
        cell = dst.Range("B5").PivotCell
        self.assertEqual([i.Name for i in cell.RowItems], ["East"])
        self.assertEqual([i.Name for i in cell.ColumnItems], ["2023"])
        self.assertEqual(dst.Range("B5").PivotField.Name, "Sum of Sales")
        self.assertEqual(dst.Range("A5").PivotItem.Name, "East")
        self.assertEqual(pt.GetData("'Sum of Sales' East 2024"), 20)
        self.assertEqual(pt.DataPivotField.Orientation, excel.xlHidden)
        self.assertEqual(pt.PivotFields("Region").DataType, excel.xlText)
        self.assertEqual(pt.PivotFields("Year").DataType, excel.xlNumber)
        self.assertEqual(pt.PivotFields("Region").AutoShowType, excel.xlManual)
        self.assertEqual(pt.PivotFields("Region").AutoShowRange, excel.xlTop)
        self.assertEqual(pt.PivotFields("Region").AutoShowCount, 10)
        self.assertIsNone(pt.PivotFields("Product").CurrentPageName)
        self.assertEqual(sorted(f.Name for f in pt.VisibleFields), ["Product", "Region", "Sales", "Year"])
        self.assertEqual(pt.HiddenFields, [])
        self.assertTrue(pt.RefreshName)
        self.assertIsInstance(pt.RefreshDate, dt.datetime)

    def test_areas_and_cell_types_of_two_more_layouts_as_excel_measures_them(self):
        from yc import excel

        data = excel.Worksheets.Add()
        data.Name = f"L{int(time.time() * 1000) % 100000}"
        data.Range("A1:D6").Value = [["Region", "Year", "Product", "Sales"], ["East", 2022, "A", 10], ["West", 2023, "B", 20],
                                     ["East", 2023, "A", 5], ["North", 2022, "B", 7], ["West", 2022, "A", 3]]
        PivotTest.n = getattr(PivotTest, "n", 0) + 1
        # one value under a filter: a report one column wide (Excel 16, measured)
        d1 = excel.Worksheets.Add()
        pt = excel.ActiveWorkbook.PivotCaches.Create(excel.xlDatabase, data.Range("A1:D6")).CreatePivotTable(d1.Range("A3"), f"One{PivotTest.n}")
        pt.PivotFields("Year").Orientation = excel.xlPageField
        pt.AddDataField(pt.PivotFields("Sales"), "Sum of Sales", excel.xlSum)
        self.assertEqual(pt.TableRange1.Address, "$A$3:$A$4")
        self.assertEqual(pt.TableRange2.Address, "$A$1:$B$4")
        self.assertEqual(pt.DataBodyRange.Address, "$A$4")
        with self.assertRaises(excel.ApplicationDefinedError):
            pt.RowRange
        self.assertEqual((d1.Range("A3").PivotCell.PivotCellType, d1.Range("A4").PivotCell.PivotCellType), (4, 0))
        # two row fields over a column field: the outer items' rows hold their subtotals, an empty value cell is a
        # value cell and the column fields' Grand Total heading a grand total
        d2 = excel.Worksheets.Add()
        pt2 = excel.ActiveWorkbook.PivotCaches.Create(excel.xlDatabase, data.Range("A1:D6")).CreatePivotTable(d2.Range("A1"), f"Two{PivotTest.n}")
        pt2.PivotFields("Year").Orientation = excel.xlRowField
        pt2.PivotFields("Product").Orientation = excel.xlRowField
        pt2.PivotFields("Region").Orientation = excel.xlColumnField
        pt2.AddDataField(pt2.PivotFields("Sales"), "Sum of Sales", excel.xlSum)
        self.assertEqual(pt2.TableRange1.Address, "$A$1:$E$9")
        self.assertEqual(pt2.DataBodyRange.Address, "$B$3:$E$9")
        self.assertEqual(d2.Range("B3:E3").Value, [[10, 7, 3, 20]])
        types = {a: d2.Range(a).PivotCell.PivotCellType for a in ("E2", "C4", "B5", "A9", "E9", "A3")}
        self.assertEqual(types, {"E2": 3, "C4": 0, "B5": 0, "A9": 3, "E9": 0, "A3": 1})

    def test_grand_total_name_auto_show_and_show_pages(self):
        from yc import excel

        dst, pt = self.make()
        self.assertEqual(pt.GrandTotalName, "Grand Total")
        pt.GrandTotalName = "All"
        self.assertEqual(pt.GrandTotalName, "All")
        self.assertEqual(dst.Range("A8").Value, "All")
        region = pt.PivotFields("Region")
        region.AutoShow(excel.xlAutomatic, excel.xlTop, 2, "Sum of Sales")
        self.assertEqual(region.AutoShowType, excel.xlAutomatic)
        self.assertEqual(region.AutoShowCount, 2)
        self.assertEqual(region.AutoShowField, "Sum of Sales")
        self.assertEqual(pt.RowRange.Address, "$A$4:$A$7")  # the top two regions and the grand total
        region.AutoShow(excel.xlManual)
        self.assertEqual(pt.RowRange.Address, "$A$4:$A$8")
        before = excel.Worksheets.Count
        made = pt.ShowPages("Product")
        self.assertEqual(sorted(w.Name for w in made), ["A", "B"])
        self.assertEqual(excel.Worksheets.Count, before + 2)
        self.assertEqual(made[0].PivotTables(1).PivotFields("Product").CurrentPage, made[0].Name)

    def test_pointing_at_a_value_writes_getpivotdata(self):
        from yc import excel, ui

        dst, pt = self.make()
        excel.Application.GenerateGetPivotData = True
        r = ui.cell_rect("B5")
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"g:F2|w:2|t:=|w:3|m:{x},{y}|w:2|c:{x},{y}|w:4|k:Enter|w:6")
        f = dst.Range("F2").Formula
        self.assertTrue(f.upper().startswith("=GETPIVOTDATA("), f)
        self.assertIn('"Region","East"', f.replace(" ", ""))
        self.assertEqual(dst.Range("F2").Value, 10)
        excel.Application.GenerateGetPivotData = False
        self.keys(f"g:F3|w:2|t:=|w:3|m:{x},{y}|w:2|c:{x},{y}|w:4|k:Enter|w:6")
        self.assertEqual(dst.Range("F3").Formula, "=B5")
        excel.Application.GenerateGetPivotData = True


@unittest.skipIf(_skip(), "no built ycgrid.exe")
class ChartsTest(_Base):
    def chart(self):
        self.ws.Range("A1:B4").Value = [["Day", "Price"], ["Mon", 100], ["Tue", 250000], ["Wed", 120]]
        co = self.ws.ChartObjects().Add(150, 60, 300, 200)
        co.Chart.SetSourceData(self.ws.Range("A1:B4"))
        co.Chart.ChartType = 51  # xlColumnClustered
        return co

    def test_axes_units_spacing_and_names(self):
        from yc import excel

        co = self.chart()
        ch = co.Chart
        val, cat = ch.Axes(excel.xlValue), ch.Axes(excel.xlCategory)
        self.assertEqual(val.AxisGroup, excel.xlPrimary)
        self.assertFalse(val.HasDisplayUnitLabel)
        val.DisplayUnit = excel.xlThousands
        self.assertTrue(val.HasDisplayUnitLabel)
        self.assertEqual(val.DisplayUnitLabel.Text, "Thousands")
        val.DisplayUnitCustom = 250
        self.assertEqual(val.DisplayUnit, -4114)  # xlCustom (Excel 16, measured)
        self.assertEqual(val.DisplayUnitLabel.Text, "x 250")
        val.HasDisplayUnitLabel = False
        self.assertFalse(val.HasDisplayUnitLabel)
        cat.TickMarkSpacing = 2
        self.assertEqual(cat.TickMarkSpacing, 2)
        self.assertEqual(cat.CategoryNames, ["Mon", "Tue", "Wed"])
        self.assertTrue(ch.DisplayValueNotAvailableAsBlank)
        ch.DisplayValueNotAvailableAsBlank = False
        self.assertFalse(ch.DisplayValueNotAvailableAsBlank)
        tl, br = co.TopLeftCell, co.BottomRightCell
        self.assertLessEqual(tl.Left, co.Left)
        self.assertGreater(tl.Left + tl.Width, co.Left)
        self.assertLessEqual(br.Top, co.Top + co.Height)
        self.assertGreater(br.Top + br.Height, co.Top + co.Height)

    def test_shapes_on_a_chart_move_with_it(self):
        co = self.chart()
        ch = co.Chart
        n = self.ws.Shapes.Count  # the chart (Shapes lists charts too, as Excel's)
        line = ch.Shapes.AddLine(10, 20, 200, 20)
        self.assertEqual(ch.Shapes.Count, 1)
        # Excel 16, measured: a chart's shape is placed from the chart's corner and isn't one of the sheet's shapes
        self.assertAlmostEqual(line.Left, 10, delta=0.5)
        self.assertAlmostEqual(line.Top, 20, delta=0.5)
        self.assertEqual(self.ws.Shapes.Count, n)
        co.Left = co.Left + 50
        self.assertAlmostEqual(ch.Shapes(1).Left, 10, delta=0.5)  # it moved with the chart
        ch.Shapes(1).Left = 30
        self.assertAlmostEqual(ch.Shapes(1).Left, 30, delta=0.5)
        co.Delete()
        self.assertEqual(self.ws.Shapes.Count, n - 1)

    def test_export_as_pdf_copy_picture_and_tips(self):
        from yc import excel, ui

        co = self.chart()
        self.keys("w:6")
        pdf = os.path.join(self.tmp, "chart.pdf")
        co.Chart.ExportAsFixedFormat(0, pdf)
        with open(pdf, "rb") as f:
            self.assertEqual(f.read(5), b"%PDF-")
        co.CopyPicture()  # the instance's own clipboard
        # the tip over a column names its point and value
        excel.Application.ShowChartTipNames = True
        excel.Application.ShowChartTipValues = True
        box = None
        for _ in range(10):
            st = ui.state()
            if st.get("chart_tip"):
                break
            r = [i for i in ui.items("") if i.get("id", "").endswith("##obj")]
            if r:
                box = r[-1]["rect"]
            if box:
                x, y = int(box[0] + (box[2] - box[0]) * 0.5), int(box[1] + (box[3] - box[1]) * 0.55)
                self.keys(f"m:{x},{y}|w:20")
        tip = ui.state().get("chart_tip", "")
        self.assertIn("Value", tip + " ")  # a column under the mouse


@unittest.skipIf(_skip(), "no built ycgrid.exe")
class ProjectTest(unittest.TestCase):
    """What runs in Python: Ctrl+Break by EnableCancelKey, ThisCell in a UDF, MacroOptions in Insert Function."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="yc_gaps_proj_")
        proj = os.path.join(cls.tmp, "proj")
        py = os.path.join(os.path.dirname(EXE), "runtime", "python", "python.exe")
        subprocess.run([py, "-E", "-s", "-c", f"import sys;sys.path.insert(0,r'{os.path.join(ROOT, 'python')}');from yc.cli import main;sys.exit(main())",
                        "init", proj, "--sheet", "Sheet1=Sheet1"], check=True, capture_output=True)
        with open(os.path.join(proj, "main.py"), "w", encoding="utf-8") as f:
            f.write(MAIN)
        book = os.path.join(proj, "Book.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=p0.pid)
            from yc import excel

            excel.ThisWorkbook.SaveAs(book)
        finally:
            p0.terminate()
            p0.wait(10)
        cls.proc = yc.launch(book, hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        end = time.time() + 60
        while time.time() < end and not cls.conn.request("python.status", {}).get("workbook_open_done"):
            time.sleep(0.2)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)

    def z1(self):
        from yc import excel

        return excel.Worksheets(1).Range("Z1").Value

    def run_macro(self, name):
        out = {}
        # a connection of its own: one waits for its answer, and the checks meanwhile go through another
        jc = yc.connect(target=self.proc.pid)
        self.conn = yc.connect(target=self.proc.pid)

        def go():
            out["r"] = jc.request("host.job", {"kind": "macro", "name": name, "timeout_ms": 30000}, timeout=40)

        t = threading.Thread(target=go)
        t.start()
        return t, out

    def wait_z1(self, value, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if self.z1() == value:
                return True
            time.sleep(0.1)
        return self.z1() == value

    def test_ctrl_break_stops_the_macro_as_enable_cancel_key_says(self):
        from yc import ui

        t, out = self.run_macro("main.LongLoop")
        self.assertTrue(self.wait_z1("started"))
        self.keys("k:c-Pause|w:3")
        t.join(15)
        self.assertFalse(t.is_alive(), "the macro stopped")
        self.assertFalse(out["r"].get("ok"))
        self.assertEqual(self.z1(), "started")
        self.assertTrue(any("interrupt" in (e.get("title", "") + e.get("text", "")).lower() for e in ui.errors()), ui.errors()[-3:])
        # xlErrorHandler: the code catches it
        t, out = self.run_macro("main.LongLoopTrapped")
        self.assertTrue(self.wait_z1("started"))
        self.keys("k:c-Pause|w:3")
        t.join(15)
        self.assertEqual(self.z1(), "trapped")
        # xlDisabled: nothing stops it
        t, out = self.run_macro("main.LongLoopDisabled")
        self.assertTrue(self.wait_z1("started"))
        self.keys("k:c-Pause|w:3")
        t.join(15)
        self.assertEqual(self.z1(), "finished")
        # back to xlInterrupt once the code returns, as Excel at idle
        r = self.conn.request("host.job", {"kind": "macro", "name": "main.CancelKeyAfter", "timeout_ms": 10000}, timeout=20)
        self.assertEqual(r.get("result"), 0)
        r = self.conn.request("host.job", {"kind": "exec", "code": "Application.EnableCancelKey", "timeout_ms": 10000}, timeout=20)
        self.assertEqual(r.get("result"), 1)

    def test_this_cell_in_a_udf(self):
        from yc import excel

        ws = excel.Worksheets(1)
        ws.Range("C7").Formula = "=WhoCalls()"
        end = time.time() + 10
        while time.time() < end and ws.Range("C7").Value != "$C$7":
            time.sleep(0.2)
        self.assertEqual(ws.Range("C7").Value, "$C$7")

    def test_macro_options_show_in_insert_function(self):
        from yc import ui

        h = self.conn.request("debug.func_help", {"name": "PRICEBS", "arg_names": ["spot", "vol"]})
        self.assertEqual(h["args"], "spot, [vol]")  # from the function's signature
        self.assertEqual(h["desc"], "Prices a call.")  # MacroOptions' Description wins over the docstring
        self.assertEqual(h["category"], "Financial")
        self.assertEqual(h["arg_help"], {"spot": "the underlying's price", "vol": "the volatility"})
        h = self.conn.request("debug.func_help", {"name": "WHOCALLS"})
        self.assertEqual(h["category"], "User Defined")
        self.assertEqual(h["desc"], "No help available.")

        self.keys("g:E5|w:2|k:s-F3|w:8|t:pricebs|w:8")
        try:
            items = [i["label"] for i in ui.items()]
            self.assertIn("PRICEBS", items)  # found by its name
        finally:
            self.keys("k:Escape|w:3|k:Escape|w:3")


if __name__ == "__main__":
    unittest.main()
