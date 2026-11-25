# SPDX-License-Identifier: Apache-2.0
"""Round 3 (formulas): the Formulas tab, the error indicators and formula editing working as in Excel, each checked
the way a person does it: the ribbon's menus, the error options button, the dialogs, keys and clicks on the sheet.

Run: python -m unittest tests.test_round3_formulas (with the bundled Python).
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")


class Round3FormulasTest(unittest.TestCase):
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

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        excel.Application.Calculation = -4105  # automatic
        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("w:2")
        self.asserts0 = self.state().get("imgui_asserts", 0)

    def tearDown(self):
        from yc import excel, ui

        # no ImGui check failed while the menus, windows and dialogs were drawn (a debug build stops on those)
        self.assertEqual(self.state().get("imgui_asserts", 0), self.asserts0, "ImGui checks failed")

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        if ui.state().get("dialog", 0):
            self.keys("k:Escape|w:4")
        if ui.state()["ribbon"].get("backstage"):
            self.keys("k:Escape|w:4")
        self.conn.request("audit.clear", {"sheet": 0}, timeout=10)
        excel.Application.Calculation = -4105

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def state(self):
        from yc import ui

        return ui.state()

    def edit_text(self):
        return (self.state().get("edit") or {}).get("text")

    def click_at(self, x, y, double=False):
        self.keys(f"m:{int(x)},{int(y)}|w:3|{'D' if double else 'c'}:{int(x)},{int(y)}|w:6")

    def items(self, label=None, ident=None, window=""):
        from yc import ui

        hits = [i for i in ui.items(window) if (label is None or i["label"] == label) and (ident is None or i["id"] == ident)]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        return hits

    def labels(self, window=""):
        from yc import ui

        return [i["label"] for i in ui.items(window)]

    def click(self, label=None, ident=None, window="", nth=0, double=False):
        hits = self.items(label, ident, window)
        self.assertGreater(len(hits), nth, f"{label or ident!r} isn't on the screen")
        r = hits[nth]["rect"]
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2, double)

    def tab(self, name):
        tabs = self.state()["ribbon"]["tabs"]
        self.assertIn(name, tabs, f"the {name} tab isn't shown")
        x, y = tabs[name]
        time.sleep(0.55)
        self.keys(f"m:{int(x)},{int(y)}|w:3|c:{int(x)},{int(y)}|w:6")

    def ribbon(self, cmd, where=None):
        from yc import ui

        hits = [i for i in ui.ribbon_items() if i["cmd"] == cmd and (where is None or i["where"] == where)]
        self.assertTrue(hits, f"{cmd} isn't on the ribbon now")
        return hits[0]["rect"]

    def open_menu(self, cmd):
        """Open a drop-down or a split button's menu by its arrow, as a person does."""
        from yc import ui

        hits = [i for i in ui.ribbon_items() if i["cmd"] == cmd]
        self.assertTrue(hits, f"{cmd} isn't on the ribbon now")
        x0, y0, x1, y1 = hits[0]["rect"]
        if y1 - y0 > 50:  # a large button: its lower half (the arrow)
            x, y = (x0 + x1) / 2, y1 - 8
        else:  # a small one: the arrow at its right end
            x, y = x1 - 6, (y0 + y1) / 2
        self.click_at(x, y)
        self.assertTrue(self.state()["ribbon"].get("popup_open"), f"{cmd}'s menu opens")

    def menu_entry(self, cmd):
        r = self.ribbon(cmd, "menu")
        self.click_at((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)

    def menu_labels(self):
        """The commands of the open menu, top to bottom."""
        from yc import ui

        entries = [i for i in ui.ribbon_items() if i["where"] in ("menu", "submenu")]
        entries.sort(key=lambda i: i["rect"][1])
        seen = []
        for e in entries:
            if e["cmd"] not in seen:
                seen.append(e["cmd"])
        return seen

    def arrows(self):
        return self.conn.request("audit.arrows", {}, timeout=10)

    # ---- H5: background error checking ----
    def test_error_options_menu_is_excels(self):
        # Help on this Error and Error Checking Options... were missing
        ws = self.ws
        ws.Range("A1").Value = "'123"
        self.keys("w:3|g:A1|w:4")
        self.click("Error Options")
        got = self.labels()
        for entry in ("Number Stored as Text", "Convert to Number", "Help on this Error", "Ignore Error", "Edit in Formula Bar", "Error Checking Options..."):
            self.assertIn(entry, got)
        self.click("Error Checking Options...")
        self.keys("w:4")
        self.assertTrue(self.state()["ribbon"].get("backstage"), "File > Options opens")
        got = self.labels()
        self.assertIn("Enable background error checking", got)
        self.assertIn("Formulas which omit cells in a region", got)
        self.assertIn("Cells containing years represented as 2 digits", got)
        self.keys("k:Escape|w:4")

    def test_text_dates_and_omitted_cells_are_marked_and_fixed(self):
        from yc import constants as k
        from yc import excel

        ws = self.ws
        ws.Range("A1").Value = "'12/31/99"
        ws.Range("C1:C4").Value = [[1], [2], [3], [4]]
        ws.Range("C6").Formula = "=SUM(C1:C3)"
        self.keys("w:4")
        self.assertTrue(ws.Range("A1").Errors(k.xlTextDate).Value)
        self.assertTrue(ws.Range("C6").Errors(k.xlOmittedCells).Value)
        self.assertFalse(ws.Range("C6").Errors(k.xlEvaluateToError).Value)
        # the button's first entry fixes it, as in Excel
        self.keys("g:C6|w:4")
        self.click("Error Options")
        self.assertIn("Formula Omits Adjacent Cells", self.labels())
        self.click("Update Formula to Include Cells")
        self.keys("w:4")
        self.assertEqual(ws.Range("C6").Formula, "=SUM(C1:C4)")
        self.assertEqual(ws.Range("C6").Value, 10)
        self.keys("g:A1|w:4")
        self.click("Error Options")
        self.click("Convert XX to 20XX")
        self.keys("w:4")
        self.assertNotIsInstance(ws.Range("A1").Value, str)
        self.assertEqual(excel.Application.Evaluate(f"=YEAR('{ws.Name}'!A1)"), 2099)

    def test_misleading_number_format_is_marked_and_fixed(self):
        from yc import constants as k
        from yc import excel

        ws = self.ws
        self.keys("g:A1|w:2|t:$10|k:Enter|t:$20|k:Enter|t:=A1+A2|k:Enter|w:4")
        # the formula takes A1's currency format as it is entered (Excel); set back to General, it misleads
        self.assertIn("$", ws.Range("A3").NumberFormat)
        ws.Range("A3").NumberFormat = "General"
        self.keys("w:4")
        self.assertTrue(ws.Range("A3").Errors(k.xlMisleadingFormat).Value)
        self.assertTrue(excel.Application.ErrorCheckingOptions.MisleadingNumberFormats)
        self.keys("g:A3|w:4")
        self.click("Error Options")
        self.assertIn("Misleading Number Format", self.labels())
        self.click("Update Format to Currency")
        self.keys("w:4")
        self.assertIn("$", ws.Range("A3").NumberFormat)
        self.assertFalse(ws.Range("A3").Errors(k.xlMisleadingFormat).Value)

    def test_error_checking_options_from_python(self):
        from yc import excel

        o = excel.Application.ErrorCheckingOptions
        self.assertTrue(o.OmittedCells)
        self.assertTrue(o.TextDate)
        self.assertFalse(o.EmptyCellReferences)  # off by default, as in Excel
        o.OmittedCells = False
        try:
            self.assertFalse(excel.Application.ErrorCheckingOptions.OmittedCells)
        finally:
            o.OmittedCells = True

    # ---- Error Checking dialog and Trace Error ----
    def test_error_checking_dialog_walks_the_errors(self):
        ws = self.ws
        ws.Range("B2").Formula = "=1/0"
        ws.Range("A3").Value = "'5"
        self.keys("w:3|g:A1|w:2")
        self.tab("Formulas")
        self.click_at(*self._center(self.ribbon("ErrorChecking")))
        self.keys("w:4")
        self.assertEqual(self.state()["selection"], "B2")
        got = self.labels()
        self.assertIn("Error in cell B2", got)
        self.assertIn("Divide by Zero Error", got)
        self.assertIn("Show Calculation Steps...", got)
        self.click("Next")
        self.assertEqual(self.state()["selection"], "A3")
        self.assertIn("Convert to Number", self.labels())
        self.click("Ignore Error")
        self.keys("w:4")
        self.assertIn("The error check is complete for the entire sheet.", self.labels())
        self.click("OK")
        self.assertNotIn("Error in cell B2", self.labels())

    def test_trace_error_draws_arrows_back_to_the_error(self):
        ws = self.ws
        ws.Range("A1").Value = 0
        ws.Range("B1").Formula = "=1/A1"
        ws.Range("C1").Formula = "=B1+1"
        ws.Range("D1").Formula = "=C1*2"
        self.keys("w:3|g:D1|w:2")
        self.tab("Formulas")
        self.open_menu("ErrorChecking")
        self.assertIn("TraceError", self.menu_labels())
        self.menu_entry("TraceError")
        self.keys("w:4")
        arrows = self.arrows()
        # C1 -> D1, B1 -> C1 (red: errors) and A1 -> B1 (where the error starts)
        froms = sorted((a[1], a[2]) for a in arrows)
        self.assertEqual(froms, [(0, 0), (0, 1), (0, 2)])
        self.conn.request("audit.clear", {"sheet": 0}, timeout=10)
        ws.Range("D1").ShowErrors()
        self.assertEqual(len(self.arrows()), 3)

    # ---- Trace Precedents / Dependents across sheets ----
    def test_tracing_to_another_sheet_draws_a_sheet_icon_that_opens_go_to(self):
        from yc import excel

        other = excel.Worksheets.Add()
        other.Name = "Rates" + str(int(time.time() * 1000) % 100000)
        other.Range("A1").Value = 3
        self.ws.Activate()
        self.ws.Range("B2").Formula = f"={other.Name}!A1*2"
        self.keys("w:3|g:B2|w:2|x:TracePrecedents|w:4")
        arrows = [a for a in self.arrows() if a[7]]
        self.assertEqual(len(arrows), 1)
        self.assertEqual(arrows[0][7], [f"{other.Name}!$A$1"])
        self.click(ident="Trace to another sheet##yc_trace_sheet", double=True)
        self.keys("w:4")
        self.assertTrue(self.state().get("dialog"), "Go To opens")
        self.assertIn(f"{other.Name}!$A$1", self.labels())
        self.keys("k:Enter|w:5")
        self.assertEqual(excel.ActiveSheet.Name, other.Name)
        self.assertEqual(self.state()["selection"], "A1")
        # Trace Dependents from the other sheet: the arrow goes to the icon
        self.keys("x:RemoveArrows|w:2|x:TraceDependents|w:4")
        arrows = [a for a in self.arrows() if a[7]]
        self.assertEqual(len(arrows), 1)
        self.assertEqual(arrows[0][7], [f"{self.ws.Name}!$B$2"])

    # ---- Evaluate Formula: Step In / Step Out ----
    def test_evaluate_formula_steps_into_a_cell(self):
        ws = self.ws
        ws.Range("A1").Formula = "=3+4"
        ws.Range("B1").Formula = "=A1*2+1"
        self.keys("w:3|g:B1|w:2|x:EvaluateFormula|w:5")
        refs = [t for t in self.labels() if t.startswith("Reference: ")]
        self.assertEqual(len(refs), 1)
        self.click("Step In")
        refs = [t for t in self.labels() if t.startswith("Reference: ")]
        self.assertEqual(len(refs), 2, "a second box for A1")
        self.assertTrue(any("$A$1" in t for t in refs))
        self.click("Step Out")
        refs = [t for t in self.labels() if t.startswith("Reference: ")]
        self.assertEqual(len(refs), 1)
        self.click("Close")

    # ---- M3: Ctrl+click while pointing ----
    def test_ctrl_click_while_pointing_adds_a_reference(self):
        ws = self.ws
        ws.Range("A1").Value = 1
        ws.Range("B3").Value = 2
        self.keys("g:C2|w:2|t:=SUM(|A:A1|w:3|H:ctrl|A:B3|w:3|h:|w:2|t:)|k:Enter|w:4")
        self.assertEqual(ws.Range("C2").Formula, "=SUM(A1,B3)")
        self.assertEqual(ws.Range("C2").Value, 3)

    # ---- AutoSum: the proposal changes by dragging ----
    def test_autosum_proposal_is_replaced_by_dragging(self):
        from yc import ui

        ws = self.ws
        ws.Range("A1:A4").Value = [[1], [2], [3], [4]]
        self.keys("g:A5|w:2|k:a-Equal|w:4")
        self.assertEqual(self.edit_text(), "=SUM(A1:A4)")
        r2, r3 = ui.cell_rect("A2"), ui.cell_rect("A3")
        x, y2, y3 = int((r2[0] + r2[2]) / 2), int((r2[1] + r2[3]) / 2), int((r3[1] + r3[3]) / 2)
        self.keys(f"m:{x},{y2}|w:2|p:{x},{y2}|w:2|m:{x},{y3}|w:3|u:|w:3")
        self.assertEqual(self.edit_text(), "=SUM(A2:A3)")
        self.keys("k:Enter|w:4")
        self.assertEqual(ws.Range("A5").Value, 5)

    # ---- L2: Calculate Sheet ----
    def test_calculate_sheet_calculates_only_the_active_sheet(self):
        from yc import excel

        ws = self.ws
        other = excel.Worksheets.Add()
        ws.Activate()
        ws.Range("A1").Value = 1
        ws.Range("B1").Formula = "=A1"
        other.Range("B1").Formula = f"='{ws.Name}'!A1"
        excel.Application.Calculation = -4135  # manual
        self.keys("g:A1|w:2|t:2|k:Enter|w:4")
        self.assertEqual(ws.Range("B1").Value, 1)
        self.keys("k:s-F9|w:4")
        self.assertEqual(ws.Range("B1").Value, 2)
        self.assertEqual(other.Range("B1").Value, 1)
        other.Calculate()
        self.assertEqual(other.Range("B1").Value, 2)

    # ---- Calculation Options: Automatic except for data tables ----
    def test_automatic_except_for_data_tables(self):
        from yc import excel

        self.tab("Formulas")
        self.open_menu("CalculationOptions")
        self.assertEqual(self.menu_labels(), ["CalcAutomatic", "CalcSemiautomatic", "CalcManual"])
        self.menu_entry("CalcSemiautomatic")
        self.keys("w:3")
        self.assertEqual(excel.Application.Calculation, 2)  # xlCalculationSemiautomatic
        excel.Application.Calculation = -4105
        self.assertEqual(excel.Application.Calculation, -4105)
        excel.Application.Calculation = 2
        self.assertEqual(excel.Application.Calculation, 2)

    # ---- L8: Use in Formula ----
    def test_use_in_formula_is_a_drop_down_of_the_names(self):
        from yc import excel

        wb = excel.ActiveWorkbook
        wb.Names.Add("RateU", "=0.05")
        try:
            self.keys("g:C1|w:2|t:=1+|w:3")
            self.tab("Formulas")
            self.open_menu("UseInFormula")
            entries = self.menu_labels()
            self.assertEqual(entries[-1], "PasteName")  # Paste Names... at the end
            from yc import ui

            names = {c["id"]: c["label"] for c in ui.commands() if c["id"] in entries}
            hit = [e for e in entries if names.get(e) == "RateU"]
            self.assertTrue(hit, names)
            self.menu_entry(hit[0])
            self.keys("w:3")
            self.assertEqual(self.edit_text(), "=1+RateU")
            self.keys("k:Escape|w:3")
        finally:
            wb.Names("RateU").Delete()

    # ---- Define Name > Apply Names ----
    def test_apply_names_from_the_define_name_menu(self):
        from yc import excel

        ws = self.ws
        wb = excel.ActiveWorkbook
        wb.Names.Add("PriceAN", f"='{ws.Name}'!$A$1")
        try:
            ws.Range("A1").Value = 4
            ws.Range("B1").Formula = "=$A$1*2"
            self.keys("w:3|g:B1|w:2")
            self.tab("Formulas")
            self.open_menu("DefineName")
            self.menu_entry("ApplyNames")
            self.keys("w:4")
            self.assertTrue(self.state().get("dialog"))
            self.click("PriceAN")
            self.click("OK")
            self.assertEqual(ws.Range("B1").Formula, "=PriceAN*2")
            ws.Range("B2").Formula = "=$A$1+1"
            self.assertEqual(ws.Range("B2").ApplyNames("PriceAN"), 1)
            self.assertEqual(ws.Range("B2").Formula, "=PriceAN+1")
            # Use row and column names: where a row name crosses a column name
            wb.Names.Add("JanAN", f"='{ws.Name}'!$D$1:$D$3")
            wb.Names.Add("SalesAN", f"='{ws.Name}'!$C$2:$E$2")
            try:
                ws.Range("D2").Value = 7
                ws.Range("G5").Formula = "=D2*2"
                self.assertEqual(ws.Range("G5").ApplyNames(["JanAN", "SalesAN"]), 1)
                self.assertEqual(ws.Range("G5").Formula, "=SalesAN JanAN*2")
                self.assertEqual(ws.Range("G5").Value, 14)
                ws.Range("G6").Formula = "=D2+1"
                self.assertEqual(ws.Range("G6").ApplyNames(["JanAN", "SalesAN"], Order=2), 1)  # xlColumnThenRow
                self.assertEqual(ws.Range("G6").Formula, "=JanAN SalesAN+1")
                # the dialog has the same choices
                self.keys("g:G7|w:2")
                self.tab("Formulas")
                self.open_menu("DefineName")
                self.menu_entry("ApplyNames")
                self.keys("w:4")
                self.assertIn("Use row and column names", self.labels())
                self.click("Options >>")
                self.keys("w:4")  # the dialog grows to show the options
                got = self.labels()
                for t in ("Omit column name if same column", "Omit row name if same row", "Row Column", "Column Row"):
                    self.assertIn(t, got)
                self.keys("k:Escape|w:3")
            finally:
                wb.Names("JanAN").Delete()
                wb.Names("SalesAN").Delete()
        finally:
            wb.Names("PriceAN").Delete()

    # ---- M10: Show Formulas draws the columns twice as wide ----
    def test_show_formulas_draws_wider_columns(self):
        from yc import ui

        ws = self.ws
        ws.Range("A1").Formula = "=1+2"
        w0 = ui.cell_rect("B1")
        width0 = ws.Columns(2).ColumnWidth
        self.keys("g:A1|w:2|k:c-GraveAccent|w:4")
        try:
            w1 = ui.cell_rect("B1")
            self.assertAlmostEqual((w1[2] - w1[0]) / (w0[2] - w0[0]), 2.0, delta=0.1)
            self.assertEqual(ws.Columns(2).ColumnWidth, width0)  # the stored width doesn't change
        finally:
            self.keys("k:c-GraveAccent|w:4")
        w2 = ui.cell_rect("B1")
        self.assertAlmostEqual(w2[2] - w2[0], w0[2] - w0[0], delta=1)

    # ---- M11: tables in the Name Manager ----
    def test_name_manager_lists_tables(self):
        ws = self.ws
        ws.Range("A1:B3").Value = [["Qty", "Px"], [1, 2], [3, 4]]
        lo = ws.ListObjects.Add(1, ws.Range("A1:B3"), None, 1)
        lo.Name = "SalesNM"
        self.keys("w:3|x:NameManager|w:5")
        got = self.labels()
        self.assertIn("SalesNM", got)
        self.assertTrue(any(t.endswith("!$A$2:$B$3") for t in got), [t for t in got if "!" in t])
        # Edit... renames it, as in Excel
        self.click("SalesNM")
        self.click("Edit...")
        self.keys("k:c-A|t:SalesRenamed|w:2|k:Enter|w:5")
        self.assertEqual(ws.ListObjects(1).Name, "SalesRenamed")
        self.keys("k:Escape|w:3")

    # ---- L10: Watch Window ----
    def test_watch_window_names_cells_and_follows_inserted_rows(self):
        from yc import excel

        ws = self.ws
        ws.Range("B5").Value = 7
        excel.ActiveWorkbook.Names.Add("WatchedW", f"='{ws.Name}'!$B$5")
        try:
            excel.Application.Watches.Add(ws.Range("B5"))
            ws.Rows(1).Insert()
            self.keys("w:4")
            self.assertEqual(excel.Application.Watches(1).Source.Address(), "$B$6")
            got = self.labels()
            self.assertIn("Name", got)  # the column header
            self.assertIn("WatchedW", got)
            # Add Watch...: the selection, added with its dialog's Add
            self.keys("g:D1:D2|w:2")
            self.click("Add Watch...")
            self.assertTrue(self.state().get("dialog"))
            self.click("Add")
            self.assertEqual(excel.Application.Watches.Count, 3)
        finally:
            excel.Application.Watches.Delete()
            excel.ActiveWorkbook.Names("WatchedW").Delete()
            from yc import ui

            ui.pane("watch", False)


class RecentFunctionsTest(unittest.TestCase):
    """L11: Insert Function's Most Recently Used list is kept across restarts (a settings file of its own)."""

    def test_most_recently_used_survives_a_restart(self):
        settings = os.path.join(tempfile.mkdtemp(prefix="yc_r3f_"), "settings.json")
        old = os.environ.get("YCGRID_SETTINGS")
        os.environ["YCGRID_SETTINGS"] = settings
        try:
            proc = yc.launch(hidden=True, exe=EXE, lang="en")
            conn = yc.connect(target=proc.pid)
            try:
                conn.request("ui.input", {"script": "g:C1|w:2|k:s-F3|w:6|k:c-A|t:ROUNDUP|w:2|k:Enter|w:6|t:1.2|k:Tab|t:0|k:Enter|w:6", "timeout_ms": 8000}, timeout=20)
                time.sleep(0.3)
            finally:
                proc.terminate()
                proc.wait(10)
            with open(settings, encoding="utf-8") as fh:
                saved = json.load(fh)
            self.assertEqual(saved["formulas"]["recent_functions"][0], "ROUNDUP")
            proc = yc.launch(hidden=True, exe=EXE, lang="en")
            conn = yc.connect(target=proc.pid)
            try:
                conn.request("ui.input", {"script": "g:D1|w:2|k:s-F3|w:8", "timeout_ms": 8000}, timeout=20)
                time.sleep(0.3)
                from yc import ui

                got = [i["label"] for i in ui.items()]
                self.assertIn("ROUNDUP", got)  # Most Recently Used, the list it opens on
                conn.request("ui.input", {"script": "k:Escape|w:3|k:Escape|w:3", "timeout_ms": 8000}, timeout=20)
            finally:
                proc.terminate()
                proc.wait(10)
        finally:
            if old is None:
                os.environ.pop("YCGRID_SETTINGS", None)
            else:
                os.environ["YCGRID_SETTINGS"] = old


def _center(r):
    return (r[0] + r[2]) / 2, (r[1] + r[3]) / 2


Round3FormulasTest._center = staticmethod(_center)

if __name__ == "__main__":
    unittest.main()
