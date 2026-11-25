# SPDX-License-Identifier: Apache-2.0
"""Excel's workbook and sheet events, fired where Excel fires them, and a few API edges.

A hidden YCGrid opens a project whose handlers write what fired to a log file; each test does what a person does
(clicks, keys, double / right clicks, Ctrl+S) and reads the log. Every test fails without its fix.
Run: python -m unittest tests.test_round2_events (with the bundled Python).
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.environ.get("YCGRID_EXE") or next(
    (p for p in (os.path.join(ROOT, d, "ycgrid.exe") for d in ("dist_release", "dist_debug")) if os.path.exists(p)), None)

LOG_HELPER = '''
import os as _os

def _log(*parts):
    with open(LOG, "a", encoding="utf-8") as fh:
        fh.write("|".join(str(p) for p in parts) + "\\n")

def _flag(name):
    return _os.path.exists(_os.path.join(_os.path.dirname(LOG), name))
'''

SHEET = '''
def Worksheet_SelectionChange(Target):
    _log("SelectionChange", Target.Address)

def Worksheet_Activate():
    _log("Activate")

def Worksheet_Deactivate():
    _log("Deactivate")

def Worksheet_Calculate():
    _log("Calculate")

def Worksheet_BeforeDoubleClick(Target, Cancel):
    _log("BeforeDoubleClick", Target.Address)
    if _flag("cancel_dbl"):
        Cancel.Value = True

def Worksheet_BeforeRightClick(Target, Cancel):
    _log("BeforeRightClick", Target.Address)
    if _flag("cancel_right"):
        Cancel.Value = True

def Worksheet_FollowHyperlink(Target):
    _log("FollowHyperlink", Target.Range.Address, Target.SubAddress)

def Worksheet_LiveChange(Target):
    _log("LiveChange", Target.Address)
'''

MAIN = '''
def Workbook_BeforeSave(SaveAsUI, Cancel):
    _log("BeforeSave", SaveAsUI)
    if _flag("cancel_save"):
        Cancel.Value = True

def Workbook_AfterSave(Success):
    _log("AfterSave", Success)

def Workbook_BeforeClose(Cancel):
    _log("BeforeClose")
    if _flag("cancel_close"):
        Cancel.Value = True

def Workbook_NewSheet(Sh):
    _log("NewSheet", Sh.Name)

def Workbook_SheetActivate(Sh):
    _log("SheetActivate", Sh.Name)

def Workbook_SheetDeactivate(Sh):
    _log("SheetDeactivate", Sh.Name)

def Workbook_SheetSelectionChange(Sh, Target):
    _log("SheetSelectionChange", Sh.Name, Target.Address)

def Workbook_SheetCalculate(Sh):
    _log("SheetCalculate", Sh.Name)

def Workbook_SheetBeforeDoubleClick(Sh, Target, Cancel):
    _log("SheetBeforeDoubleClick", Sh.Name, Target.Address)

def Workbook_SheetBeforeRightClick(Sh, Target, Cancel):
    _log("SheetBeforeRightClick", Sh.Name, Target.Address)

def Workbook_SheetFollowHyperlink(Sh, Target):
    _log("SheetFollowHyperlink", Sh.Name, Target.Range.Address)

def Workbook_SheetLiveChange(Sh, Target):
    _log("SheetLiveChange", Sh.Name, Target.Address)
'''

# a sheet event in a standard module never runs: yc_python_status lists it apart
MODULE = '''from yc.excel import *

def Worksheet_Change(Target):
    pass
'''


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class EventsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="ycevents_")
        cls.proj = os.path.join(cls.tmp, "proj")
        py = os.path.join(os.path.dirname(EXE), "runtime", "python", "python.exe")
        subprocess.run([py, "-E", "-s", "-c", f"import sys;sys.path.insert(0,r'{os.path.join(ROOT, 'python')}');from yc.cli import main;sys.exit(main())",
                        "init", cls.proj, "--sheet", "Sheet1=Sheet1"], check=True, capture_output=True)
        cls.log = os.path.join(cls.proj, "events.txt")
        helper = f"\nLOG = r'{cls.log}'\n" + LOG_HELPER
        with open(os.path.join(cls.proj, "sheets", "Sheet1.py"), "a", encoding="utf-8") as f:
            f.write("\n" + helper + SHEET)
        with open(os.path.join(cls.proj, "main.py"), "a", encoding="utf-8") as f:
            f.write("\n" + helper + MAIN)
        with open(os.path.join(cls.proj, "modules", "Misplaced.py"), "w", encoding="utf-8") as f:
            f.write(MODULE)
        cls.book = os.path.join(cls.proj, "Book.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            from yc import excel

            excel.ThisWorkbook.SaveAs(cls.book)
        finally:
            p0.terminate()
            p0.wait(10)
        cls.proc = yc.launch(cls.book, hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        t0 = time.time()
        while time.time() - t0 < 30:
            st = cls.conn.request("python.status", {})
            if st.get("ready") and st.get("workbook_open_done"):
                break
            time.sleep(0.2)
        else:
            raise AssertionError("the Python host didn't start")

    @classmethod
    def tearDownClass(cls):
        try:
            cls.proc.terminate()
            cls.proc.wait(10)
        finally:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        from yc import excel

        for flag in ("cancel_dbl", "cancel_right", "cancel_save", "cancel_close"):
            self.set_flag(flag, False)
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        excel.Worksheets("Sheet1").Activate()
        excel.Worksheets("Sheet1").Range("A1").Select()
        self.keys("w:4")
        time.sleep(0.4)
        self.clear()

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.1)

    def set_flag(self, name, on):
        path = os.path.join(self.proj, name)
        if on:
            open(path, "w").close()
        elif os.path.exists(path):
            os.remove(path)

    def clear(self):
        open(self.log, "w").close()

    def lines(self, settle=0.6):
        time.sleep(settle)
        with open(self.log, encoding="utf-8") as fh:
            return [ln for ln in fh.read().splitlines() if ln]

    def wait_for(self, line, timeout=5.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            got = self.lines(0.1)
            if line in got:
                return got
        self.fail(f"{line!r} wasn't logged: {self.lines(0)}")

    def ui_state(self):
        from yc import ui

        return ui.state()

    # ---- SelectionChange: every change, once ----
    def test_selection_change_click(self):
        # a left click changed the selection while the button was down and nothing was sent on release
        self.keys("A:C3|w:4")
        got = self.lines()
        self.assertEqual(got.count("SelectionChange|$C$3"), 1, got)
        self.assertIn("SheetSelectionChange|Sheet1|$C$3", got)
        self.assertLess(got.index("SelectionChange|$C$3"), got.index("SheetSelectionChange|Sheet1|$C$3"))

    def test_selection_change_keys_name_box_drag_and_python(self):
        from yc import excel, ui

        self.keys("k:Down|w:3")
        self.assertEqual(self.lines().count("SelectionChange|$A$2"), 1)
        self.clear()
        self.keys("g:B2:D4|w:4")  # the Name Box
        self.assertEqual(self.lines().count("SelectionChange|$B$2:$D$4"), 1)
        self.clear()
        x0, y0 = self.center(ui.cell_rect("B6"))
        x1, y1 = self.center(ui.cell_rect("D8"))
        self.keys(f"m:{x0},{y0}|w:3|d:{x0},{y0},{x1},{y1}|w:4|m:2,2")  # (the mouse hovers before it presses)
        got = [ln for ln in self.lines() if ln.startswith("SelectionChange|")]
        self.assertEqual(got, ["SelectionChange|$B$6:$D$8"])  # once, with the final range (not every frame of the drag)
        self.clear()
        excel.Worksheets("Sheet1").Range("F6").Select()
        self.keys("w:3")
        self.assertEqual(self.lines().count("SelectionChange|$F$6"), 1)
        self.clear()
        self.keys("A:F6|w:4")  # the same cell again: nothing changed, nothing fired
        self.assertEqual([ln for ln in self.lines() if "SelectionChange" in ln], [])

    @staticmethod
    def center(r):
        return (r[0] + r[2]) // 2, (r[1] + r[3]) // 2

    # ---- Activate / Deactivate, NewSheet ----
    def test_activate_deactivate_and_new_sheet(self):
        from yc import excel

        self.keys("k:s-F11|w:6")  # Insert Worksheet: a new sheet, which becomes the active one
        got = self.lines()
        new = self.ui_state()["active_sheet"]
        self.assertNotEqual(new, "Sheet1")
        self.assertIn(f"NewSheet|{new}", got)
        self.assertIn("Deactivate", got)
        self.assertIn("SheetDeactivate|Sheet1", got)
        self.assertIn(f"SheetActivate|{new}", got)
        self.assertLess(got.index("Deactivate"), got.index("SheetDeactivate|Sheet1"))
        self.assertLess(got.index("SheetDeactivate|Sheet1"), got.index(f"SheetActivate|{new}"))
        self.assertNotIn("SelectionChange|$A$1", got)  # switching sheets isn't a selection change
        self.clear()
        excel.Worksheets("Sheet1").Activate()
        self.keys("w:4")
        got = self.lines()
        self.assertEqual(got, [f"SheetDeactivate|{new}", "Activate", "SheetActivate|Sheet1"])
        self.clear()
        excel.Worksheets(new).Copy(After=excel.Worksheets(new))  # a copy isn't a new sheet (Excel)
        excel.Worksheets("Sheet1").Activate()
        self.keys("w:4")
        self.assertEqual([ln for ln in self.lines() if ln.startswith("NewSheet")], [])
        self.clear()
        added = excel.Worksheets.Add()  # Python's Worksheets.Add fires it too
        self.wait_for(f"NewSheet|{added.Name}")

    # ---- Calculate, LiveChange without a per-cell OnChange ----
    def test_calculate_and_live_change(self):
        from yc import excel

        ws = excel.Worksheets("Sheet1")
        ws.Range("J1").Formula = "=J2*2"
        self.clear()
        ws.Range("J2").Value = 21
        # the workbook's event comes after the sheet's: waiting for the sheet's alone, a busy machine read the lines
        # before the workbook's arrived
        got = self.wait_for("SheetCalculate|Sheet1")
        self.assertIn("Calculate", got)
        # Worksheet_LiveChange needed a per-cell OnChange asking for ycCalc / ycLive before it was ever sent
        got = self.wait_for("SheetLiveChange|Sheet1|$J$1")
        self.assertIn("LiveChange|$J$1", got)
        self.assertEqual(ws.Range("J1").Value, 42)

    # ---- BeforeDoubleClick, BeforeRightClick: Cancel ----
    def test_before_double_click_cancel(self):
        self.set_flag("cancel_dbl", True)
        self.keys("Q:D4|w:8")
        got = self.lines()
        self.assertIn("BeforeDoubleClick|$D$4", got)
        self.assertIn("SheetBeforeDoubleClick|Sheet1|$D$4", got)
        self.assertNotIn("edit", self.ui_state())  # cancelled: the cell isn't in edit mode
        self.set_flag("cancel_dbl", False)
        self.clear()
        self.keys("Q:E4|w:8")
        self.assertIn("BeforeDoubleClick|$E$4", self.lines())
        st = self.ui_state()
        self.assertIn("edit", st)  # not cancelled: editing starts, as before
        self.assertEqual(st["edit"]["cell"], "E4")
        self.keys("k:Escape|w:2")

    def test_before_right_click_cancel(self):
        self.set_flag("cancel_right", True)
        self.keys("R:E5|w:8")
        got = self.lines()
        self.assertIn("BeforeRightClick|$E$5", got)
        self.assertIn("SheetBeforeRightClick|Sheet1|$E$5", got)
        self.assertFalse(self.ui_state()["ribbon"]["popup_open"])  # cancelled: no context menu
        self.set_flag("cancel_right", False)
        self.clear()
        self.keys("R:E6|w:8")
        self.assertIn("BeforeRightClick|$E$6", self.lines())
        self.assertTrue(self.ui_state()["ribbon"]["popup_open"])
        self.keys("k:Escape|w:2|m:2,2")

    # ---- FollowHyperlink ----
    def test_follow_hyperlink(self):
        from yc import excel

        ws = excel.Worksheets("Sheet1")
        ws.Hyperlinks.Add(ws.Range("B10"), "", SubAddress="Sheet1!H20", TextToDisplay="go")
        self.keys("w:2")
        self.clear()
        self.keys("A:B10|w:6")  # one click follows a link (Excel)
        got = self.wait_for("FollowHyperlink|$B$10|Sheet1!H20")
        self.assertIn("SheetFollowHyperlink|Sheet1|$B$10", got)
        self.assertEqual(self.ui_state()["active_cell"], "H20")

    # ---- BeforeSave / AfterSave ----
    def test_before_and_after_save(self):
        from yc import excel

        excel.Worksheets("Sheet1").Range("K1").Value = "x"
        self.keys("k:c-S|w:10")
        got = self.wait_for("AfterSave|True")
        self.assertIn("BeforeSave|False", got)
        self.assertLess(got.index("BeforeSave|False"), got.index("AfterSave|True"))
        # Cancel.Value = True: no save
        self.set_flag("cancel_save", True)
        excel.Worksheets("Sheet1").Range("K1").Value = "y"
        stamp = os.path.getmtime(self.book)
        self.clear()
        time.sleep(0.05)
        self.keys("k:c-S|w:10")
        got = self.lines(1.0)
        self.assertIn("BeforeSave|False", got)
        self.assertNotIn("AfterSave|True", got)
        self.assertEqual(os.path.getmtime(self.book), stamp)
        self.assertTrue(self.ui_state()["dirty"])
        # a script's save asks too, and so does the host's own (run in Python before the save)
        self.clear()
        self.assertFalse(self.conn.request("wb.save", {}))
        self.assertIn("BeforeSave|False", self.lines())
        self.set_flag("cancel_save", False)
        self.clear()
        r = self.conn.request("host.job", {"kind": "exec", "code": "ThisWorkbook.Save()", "timeout_ms": 20000}, timeout=40)
        self.assertTrue(r.get("ok"), r)
        got = self.wait_for("AfterSave|True")
        self.assertIn("BeforeSave|False", got)
        self.assertFalse(self.ui_state()["dirty"])

    # ---- BeforeClose: Cancel keeps the window ----
    def test_before_close_cancel(self):
        self.set_flag("cancel_close", True)
        self.keys("x:FileClose|w:10")
        self.wait_for("BeforeClose")
        time.sleep(0.5)
        self.assertIsNone(self.proc.poll())  # still running
        self.assertTrue(self.ui_state()["python"]["ready"])

    # ---- yc_python_status tells which handlers run ----
    def test_python_status_is_truthful(self):
        from yc.mcp import tools

        st = tools.yc_python_status(include=["events"])
        for name in ("Workbook_BeforeSave", "Worksheet_SelectionChange", "Workbook_SheetLiveChange", "Worksheet_Calculate"):
            self.assertIn(name, st["events"])
        self.assertNotIn("Worksheet_Change", st["events"])  # only defined in modules/Misplaced.py: it never runs
        self.assertIn("modules.Misplaced.Worksheet_Change", st.get("events_never_fired", []))

    # ---- MCP tools that refused what works ----
    def test_mcp_group_csv_and_schema(self):
        import json

        from yc import excel
        from yc.mcp import tools

        ws = excel.Worksheets("Sheet1")
        tools.yc_edit_structure(action="group", target="30:32", sheet="Sheet1")
        self.assertEqual(ws.Range("A31").EntireRow.OutlineLevel, 2)
        tools.yc_edit_structure(action="ungroup", target="30:32", sheet="Sheet1")
        self.assertEqual(ws.Range("A31").EntireRow.OutlineLevel, 1)
        ws.Range("A1").Value = "csv-cell"
        out = os.path.join(self.tmp, "sheet.csv")
        tools.yc_workbook_file(action="save_as", path=out[:-4], format="csv")
        with open(out, encoding="utf-8-sig") as fh:
            self.assertIn("csv-cell", fh.read())
        self.assertEqual(os.path.normcase(self.ui_state()["file"]), os.path.normcase(self.book))  # still the .ycg
        with open(os.path.join(ROOT, "mcp", "tools.json"), encoding="utf-8") as fh:
            spec = {t["name"]: t for t in json.load(fh)["tools"]}
        actions = spec["yc_manage_sheets"]["inputSchema"]["properties"]["action"]["enum"]
        self.assertNotIn("set_code_name", actions)  # offered and then refused

    # ---- connect / timeouts ----
    def test_connect_to_hidden_instance(self):
        from yc import _conn

        main = _conn.current()
        closed = []
        orig = _conn.Connection.close

        def close(c):
            closed.append(id(c))
            orig(c)

        _conn.Connection.close = close
        try:
            yc.connect(target=self.proc.pid)  # nothing holds it once the next connect replaces it
            dropped = id(_conn._current)
            conn = yc.connect(target=self.proc.pid)
            self.assertEqual(conn.default_timeout, _conn.HIDDEN_REQUEST_TIMEOUT)  # was the 600 s default
            self.assertIn(dropped, closed)  # the replaced connection doesn't stay open
            self.assertFalse(main.closed)   # one the script still holds (the class's) stays usable
            self.assertEqual(main.request("hello", {"role": "controller"}).get("pid"), self.proc.pid)
        finally:
            _conn.Connection.close = orig
            yc.set_current(main)

    def test_request_timeout_environment_variable(self):
        py = sys.executable
        code = f"import sys;sys.path.insert(0,r'{os.path.join(ROOT, 'python')}');from yc import _conn;print(_conn.REQUEST_TIMEOUT)"
        for env, want in (({"YCGRID_REQUEST_TIMEOUT": "33"}, "33.0"), ({"YC_REQUEST_TIMEOUT": "44"}, "44.0")):
            e = {k: v for k, v in os.environ.items() if k not in ("YCGRID_REQUEST_TIMEOUT", "YC_REQUEST_TIMEOUT")}
            e.update(env)
            out = subprocess.run([py, "-E", "-s", "-c", code], env=e, capture_output=True, text=True, check=True).stdout.strip()
            self.assertEqual(out, want)

    # ---- Application: worksheet functions, but no silent unknown names ----
    def test_application_unknown_names_raise(self):
        from yc import excel

        self.assertEqual(excel.Application.Sum(1, 2, 3), 6)
        self.assertEqual(excel.Application.VLookup(2, [[1, "a"], [2, "b"]], 2, False), "b")
        with self.assertRaises(AttributeError):
            excel.Application.StrictErrors = True  # stored silently before, and did nothing
        with self.assertRaises(AttributeError):
            excel.Application.NoSuchMember  # noqa: B018 - was a function that returned #NAME?
        self.assertFalse(hasattr(excel.Application, "CalculateUntilAsyncQueriesDone"))  # planned, not there yet
        excel.Application.DisplayAlerts = False  # members still work
        excel.Application.DisplayAlerts = True
        excel.Application.EnableEvents = True


if __name__ == "__main__":
    unittest.main()
