# SPDX-License-Identifier: Apache-2.0
"""The Trust Center for a workbook's Python code, as Excel's macro security, and the event differences round 2 left.

A hidden YCGrid launched with msoAutomationSecurityByUI behaves like a person's window: a project that isn't trusted
doesn't run, the Message Bar offers Enable Content, and the Trust Center Settings dialog is used with clicks.
Every test fails without its fix. Run: python -m unittest tests.test_round3_trust (with the bundled Python).
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

MAIN = '''
def Workbook_Open():
    with open(LOG, "a", encoding="utf-8") as fh:
        fh.write("Open\\n")
'''

FUNCS = '''from yc.excel import *

@udf
def MYDOUBLE(x: float) -> float:
    return x * 2
'''


def make_project(folder: str, log: str) -> str:
    """A project made with yc init, whose Workbook_Open writes to log, a Python function and a workbook using it."""
    py = os.path.join(os.path.dirname(EXE), "runtime", "python", "python.exe")
    subprocess.run([py, "-E", "-s", "-c", f"import sys;sys.path.insert(0,r'{os.path.join(ROOT, 'python')}');from yc.cli import main;sys.exit(main())",
                    "init", folder, "--sheet", "Sheet1=Sheet1"], check=True, capture_output=True)
    with open(os.path.join(folder, "main.py"), "a", encoding="utf-8") as f:
        f.write(f"\nLOG = r'{log}'\n" + MAIN)
    with open(os.path.join(folder, "modules", "Funcs.py"), "w", encoding="utf-8") as f:
        f.write(FUNCS)
    book = os.path.join(folder, "Book.ycg")
    p0 = yc.launch(hidden=True, exe=EXE, lang="en")
    try:
        from yc import excel

        excel.ThisWorkbook.SaveAs(book)
        excel.Range("A1").Formula = "=MYDOUBLE(21)"
        excel.Range("A2").Formula = '=PY("6*7")'
        excel.ThisWorkbook.Save()
    finally:
        p0.terminate()
        p0.wait(10)
    return book


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class TrustCenterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="yctrust_")
        cls.log = os.path.join(cls.tmp, "log.txt")
        cls.book = make_project(os.path.join(cls.tmp, "projA"), cls.log)
        cls.locs = os.path.join(cls.tmp, "locs")
        cls.book_b = make_project(os.path.join(cls.locs, "projB"), cls.log)
        open(cls.log, "w").close()
        # a person's window: the Trust Center decides (yc.launch's default, msoAutomationSecurityLow, runs the code)
        cls.proc = yc.launch(cls.book, hidden=True, exe=EXE, lang="en", automation_security=2)
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        try:
            cls.proc.terminate()
            cls.proc.wait(10)
        finally:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)

    def lines(self):
        with open(self.log, encoding="utf-8") as fh:
            return [ln for ln in fh.read().splitlines() if ln]

    def trust(self):
        from yc import ui

        return ui.state()["trust"]

    def wait(self, cond, what, timeout=20.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if cond():
                return
            time.sleep(0.2)
        self.fail(f"{what}: {self.trust()} {self.lines()}")

    def click_item(self, label, window=""):
        from yc import ui

        hits = [i for i in ui.items(window) if i["label"] == label and not i["disabled"]]
        self.assertTrue(hits, f"{label!r} isn't on the screen")
        x0, y0, x1, y1 = hits[-1]["rect"]
        self.keys(f"m:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:3|c:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:6")

    def open_again(self, book):
        """Close the workbook and open it again from Python (Application.AutomationSecurity applies)."""
        from yc import excel

        excel.ThisWorkbook.Close(SaveChanges=False)
        self.keys("w:4")
        excel.Workbooks.Open(book)
        self.keys("w:6")

    def open_backstage(self, page):
        """File, then one of its pages (BackstageInfo, BackstageOptions...), as a person clicks them."""
        from yc import ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        for _ in range(3):
            if ui.state()["ribbon"].get("backstage"):
                break
            fx, fy = ui.state()["ribbon"]["tabs"]["File"]
            self.keys(f"m:{int(fx)},{int(fy)}|w:2|c:{int(fx)},{int(fy)}|w:6")
        hits = [i for i in ui.ribbon_items() if i["where"] == "backstage" and i["cmd"] == page]
        self.assertTrue(hits, ui.ribbon_items())
        x0, y0, x1, y1 = hits[0]["rect"]
        self.keys(f"m:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:2|c:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:6")

    def open_trust_center(self):
        """File > Options > Trust Center Settings..."""
        from yc import ui

        self.open_backstage("BackstageOptions")
        # the Trust Center section is near the end of the Options page: scroll down to it, as a person does
        for _ in range(8):
            if any(i["label"] == "Trust Center Settings..." for i in ui.items()):
                break
            self.keys("m:900,600|w:2|W:-10|w:3")
        self.click_item("Trust Center Settings...")
        self.assertGreater(ui.state()["dialog"], 0)

    # ---- the tests run in order: the first opens the untrusted project ----
    def test_1_untrusted_project_waits_for_enable_content(self):
        from yc import excel

        self.wait(lambda: self.trust()["code"] == "disabled", "the project's code wasn't disabled")
        st = self.trust()
        self.assertTrue(st["bar"])  # the Message Bar under the ribbon
        self.assertEqual(st["reason"], "disabled")
        status = self.conn.request("python.status", {})
        self.assertTrue(status["code_disabled"])
        self.assertTrue(status["workbook_open_done"])  # nothing will run: scripts waiting for Workbook_Open go on
        self.assertFalse(status["running"])
        time.sleep(1.0)
        self.assertEqual(self.lines(), [])  # no Workbook_Open
        self.assertEqual(excel.Range("A1").Text, "#NAME?")  # a disabled Python function, as Excel's disabled macros
        self.assertEqual(excel.Range("A2").Text, "#BLOCKED!")  # =PY isn't run either
        from yc import ui

        labels = [i["label"] for i in ui.items()]
        self.assertIn("Enable Content", labels)
        self.click_item("Enable Content")
        self.wait(lambda: "Open" in self.lines(), "Enable Content didn't run Workbook_Open")
        self.wait(lambda: excel.Range("A1").Value == 42, "the Python function wasn't calculated")
        self.wait(lambda: excel.Range("A2").Value == 42, "=PY wasn't calculated")
        st = self.trust()
        self.assertFalse(st["bar"])
        self.assertEqual(st["code"], "enabled")
        self.assertEqual(ui.state()["imgui_asserts"], 0)

    def test_2_trusted_document_runs_at_once_until_cleared(self):
        from yc import excel, ui

        self.assertEqual(excel.Application.AutomationSecurity, excel.msoAutomationSecurityByUI)
        self.assertTrue(any("proja" in d for d in excel.Application.TrustCenter.TrustedDocuments))
        open(self.log, "w").close()
        self.open_again(self.book)
        self.wait(lambda: "Open" in self.lines(), "a Trusted Document didn't run at once")
        self.assertEqual(self.trust()["reason"], "trusted_document")
        self.assertFalse(self.trust()["bar"])
        # Trust Center > Trusted Documents > Clear > Yes > OK
        self.open_trust_center()
        self.click_item("Trusted Documents")
        self.click_item("Clear")
        self.click_item("Yes")
        self.click_item("OK", "Trust Center")
        self.assertEqual(ui.state()["dialog"], 0)
        self.keys("k:Escape|w:3")  # back from File > Options to the sheet
        self.assertFalse(ui.state()["ribbon"]["backstage"])
        self.assertEqual(excel.Application.TrustCenter.TrustedDocuments, [])
        open(self.log, "w").close()
        self.open_again(self.book)
        self.wait(lambda: self.trust()["code"] == "disabled", "a cleared document still ran")
        self.assertTrue(self.trust()["bar"])
        time.sleep(0.8)
        self.assertEqual(self.lines(), [])
        # the close X hides the bar; the code stays disabled and File > Info still offers Enable Content
        hits = [i for i in ui.items() if i["label"] == "Close Message Bar"]
        self.assertTrue(hits)
        x0, y0, x1, y1 = hits[0]["rect"]
        self.keys(f"m:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:3|c:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:4|m:2,2")
        self.assertFalse(self.trust()["bar"])
        self.assertTrue(self.trust()["disabled"])

    def test_3_info_advanced_options_enables_for_this_session(self):
        from yc import excel, ui

        if self.trust()["code"] != "disabled":
            excel.Application.TrustCenter.ClearTrustedDocuments()
            self.open_again(self.book)
        open(self.log, "w").close()
        self.open_backstage("BackstageInfo")
        self.click_item("Enable Content")         # its drop-down: Enable All Content / Advanced Options
        self.click_item("Advanced Options")
        self.click_item("Enable content for this session")
        self.click_item("OK", "Security Options")
        self.wait(lambda: "Open" in self.lines(), "Advanced Options > Enable content for this session didn't run it")
        self.assertEqual(self.trust()["reason"], "session")
        self.assertEqual(excel.Application.TrustCenter.TrustedDocuments, [])  # not made a Trusted Document

    def test_4_trusted_location(self):
        from yc import excel, ui

        open(self.log, "w").close()
        self.open_again(self.book_b)
        self.wait(lambda: self.trust()["code"] == "disabled", "projB isn't disabled before its folder is trusted")
        # Trust Center > Trusted Locations > Add new location... > path, subfolders > OK > OK
        self.open_trust_center()
        self.click_item("Trusted Locations")
        self.click_item("Add new location...")
        boxes = [i for i in ui.items("Trusted Location") if i["id"] == "##tl_path"]
        self.assertTrue(boxes, ui.items("Trusted Location"))
        x0, y0, x1, y1 = boxes[0]["rect"]
        self.keys(f"c:{int((x0 + x1) / 2)},{int((y0 + y1) / 2)}|w:3")
        self.conn.request("ui.input", {"script": "t:" + self.locs.replace("|", "") + "|w:2", "timeout_ms": 8000}, timeout=20)
        self.click_item("Subfolders of this location are also trusted")
        self.click_item("OK", "Trusted Location")
        self.click_item("OK", "Trust Center")
        self.assertEqual(ui.state()["dialog"], 0)
        locs = excel.Application.TrustCenter.TrustedLocations
        self.assertTrue(any(os.path.normcase(x["Path"]) == os.path.normcase(self.locs) and x["Subfolders"] for x in locs), locs)
        open(self.log, "w").close()
        self.open_again(self.book_b)
        self.wait(lambda: "Open" in self.lines(), "a workbook in a trusted location didn't run at once")
        self.assertEqual(self.trust()["reason"], "trusted_location")
        self.assertFalse(self.trust()["bar"])
        excel.Application.TrustCenter.RemoveTrustedLocation(self.locs)
        self.assertFalse(any(os.path.normcase(x["Path"]) == os.path.normcase(self.locs) for x in excel.Application.TrustCenter.TrustedLocations))

    def test_5_force_disable_and_macro_settings(self):
        from yc import excel

        excel.Application.AutomationSecurity = excel.msoAutomationSecurityForceDisable
        try:
            open(self.log, "w").close()
            self.open_again(self.book)  # trusted for this session, but ForceDisable never runs a script's workbook
            self.wait(lambda: self.trust()["code"] == "disabled", "ForceDisable ran the code")
            self.assertFalse(self.trust()["bar"])  # without a message
        finally:
            excel.Application.AutomationSecurity = excel.msoAutomationSecurityByUI
        tc = excel.Application.TrustCenter
        tc.ClearTrustedDocuments()
        tc.MacroSettings = excel.ycMacroEnableAll
        try:
            self.assertEqual(tc.MacroSettings, excel.ycMacroEnableAll)
            open(self.log, "w").close()
            self.open_again(self.book_b)  # never trusted (its location was removed): Enable all runs it
            self.wait(lambda: "Open" in self.lines(), "Enable all didn't run the code")
            self.assertEqual(self.trust()["reason"], "enable_all")
        finally:
            tc.MacroSettings = excel.ycMacroDisableWithNotification
        with self.assertRaises(Exception):
            excel.Application.AutomationSecurity = 7


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class AutomationDefaultTest(unittest.TestCase):
    def test_scripts_run_the_code_without_the_bar(self):
        tmp = tempfile.mkdtemp(prefix="yctrust_auto_")
        try:
            log = os.path.join(tmp, "log.txt")
            book = make_project(os.path.join(tmp, "proj"), log)
            open(log, "w").close()
            proc = yc.launch(book, hidden=True, exe=EXE, lang="en")  # msoAutomationSecurityLow, as Excel's automation
            try:
                from yc import excel, ui

                t0 = time.time()
                while time.time() - t0 < 20 and "Open" not in open(log, encoding="utf-8").read():
                    time.sleep(0.2)
                self.assertIn("Open", open(log, encoding="utf-8").read())
                st = ui.state()["trust"]
                self.assertEqual((st["code"], st["bar"], st["reason"]), ("enabled", False, "automation"))
                self.assertEqual(excel.Application.AutomationSecurity, excel.msoAutomationSecurityLow)
                # a workbook opened by a script into a hidden YCGrid keeps the window hidden (its saved window placement
                # showed it on the user's screen, and the next file dialog waited there for nobody)
                excel.ThisWorkbook.Close(SaveChanges=False)
                excel.Workbooks.Open(book)
                self.assertFalse(ui.state()["visible"])
                # a write refused by sheet protection is reported once, by itself: the next Close doesn't report it
                excel.ActiveSheet.Protect()
                with self.assertRaises(Exception):
                    excel.Range("C9").Value = 1
                excel.ThisWorkbook.Close(SaveChanges=False)
            finally:
                proc.terminate()
                proc.wait(10)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


def mark_from_internet(path: str, zone: int = 3) -> None:
    """The Mark of the Web a browser adds: the Zone.Identifier alternate data stream."""
    with open(path + ":Zone.Identifier", "w", encoding="ascii") as f:
        f.write(f"[ZoneTransfer]\r\nZoneId={zone}\r\n")


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class ProtectedViewTest(unittest.TestCase):
    """A workbook downloaded from the Internet opens in Protected View, as in Excel."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="ycpv_")
        cls.log = os.path.join(cls.tmp, "log.txt")
        cls.book = make_project(os.path.join(cls.tmp, "projPV"), cls.log)
        mark_from_internet(cls.book)
        open(cls.log, "w").close()
        cls.proc = yc.launch(cls.book, hidden=True, exe=EXE, lang="en", automation_security=2)
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        try:
            cls.proc.terminate()
            cls.proc.wait(10)
        finally:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    keys = TrustCenterTest.keys
    lines = TrustCenterTest.lines
    trust = TrustCenterTest.trust
    wait = TrustCenterTest.wait
    click_item = TrustCenterTest.click_item
    open_backstage = TrustCenterTest.open_backstage
    open_trust_center = TrustCenterTest.open_trust_center

    def person_opens(self, path):
        """Close the workbook, then File > Open (Ctrl+O) as a person: the Trust Center and Protected View apply."""
        from yc import excel, ui

        excel.ThisWorkbook.Close(SaveChanges=False)
        self.keys("w:4|m:2,2|w:2")
        ui.answer_file_dialogs(path)
        self.keys("k:c-O|w:8")
        self.wait(lambda: os.path.normcase(ui.state()["file"]) == os.path.normcase(path), "the file didn't open")
        self.settle()

    def settle(self):
        """Wait for the project's start (Workbook_Open) to finish before the next open or close."""
        self.wait(lambda: self.conn.request("python.status", {}).get("workbook_open_done"), "Workbook_Open didn't finish", 30)

    def copy_marked(self, name):
        dst = os.path.join(os.path.dirname(self.book), name)
        shutil.copyfile(self.book, dst)  # (a plain copy has no Mark of the Web: it is added like a browser does)
        mark_from_internet(dst)
        return dst

    def test_1_opens_read_only_until_enable_editing(self):
        from yc import excel, ui

        self.wait(lambda: self.trust()["protected_view"], "the downloaded workbook isn't in Protected View")
        st = self.trust()
        self.assertEqual(st["protected_view_reason"], "internet")
        self.assertTrue(st["protected_view_bar"])
        self.assertIn("Enable Editing", [i["label"] for i in ui.items()])
        time.sleep(0.8)
        self.assertEqual(self.lines(), [])  # no Python
        self.assertTrue(self.conn.request("python.status", {})["protected_view"])
        # typing changes nothing
        self.keys("g:B5|w:2|t:typed|k:Enter|w:4")
        self.assertIn(excel.Range("B5").Value, (None, ""))
        pv = excel.Application.ProtectedViewWindows
        self.assertEqual(pv.Count, 1)
        self.assertTrue(pv.Item(1).Caption.endswith("[Protected View]"))
        # Enable Editing: editable; then the Python code is decided as usual (this folder isn't trusted: the bar)
        self.click_item("Enable Editing")
        self.wait(lambda: not self.trust()["protected_view"], "Enable Editing didn't leave Protected View")
        self.wait(lambda: self.trust()["bar"], "the Security Warning didn't follow")
        self.assertEqual(excel.Application.ProtectedViewWindows.Count, 0)
        self.keys("g:B5|w:2|t:typed|k:Enter|w:4")
        self.assertEqual(excel.Range("B5").Value, "typed")
        self.click_item("Enable Content")
        self.wait(lambda: "Open" in self.lines(), "Enable Content didn't run Workbook_Open")
        self.assertEqual(ui.state()["imgui_asserts"], 0)

    def test_2_editing_enabled_once_and_scripts(self):
        from yc import excel

        self.person_opens(self.book)  # its editing was enabled: it opens normally
        self.assertFalse(self.trust()["protected_view"])
        other = self.copy_marked("Other.ycg")
        excel.ThisWorkbook.Close(SaveChanges=False)
        self.keys("w:4")
        excel.Workbooks.Open(other)  # a script's Workbooks.Open never opens in Protected View (Excel)
        self.keys("w:6")
        self.settle()
        self.assertFalse(self.trust()["protected_view"])
        self.person_opens(other)
        self.assertTrue(self.trust()["protected_view"])
        wb = excel.Application.ProtectedViewWindows.Item(1).Edit()  # Excel's ProtectedViewWindow.Edit
        self.assertEqual(os.path.normcase(wb.FullName), os.path.normcase(other))
        self.assertFalse(self.trust()["protected_view"])

    def test_3_trust_center_protected_view_page(self):
        from yc import excel, ui

        third = self.copy_marked("Third.ycg")
        self.open_trust_center()
        self.click_item("Protected View")
        self.click_item("Enable Protected View for files originating from the Internet")  # off
        self.click_item("OK", "Trust Center")
        self.keys("k:Escape|w:3")
        self.assertFalse(excel.Application.TrustCenter.ProtectedViewInternet)
        self.person_opens(third)
        self.assertFalse(self.trust()["protected_view"])
        excel.Application.TrustCenter.ProtectedViewInternet = True
        fourth = self.copy_marked("Fourth.ycg")
        self.person_opens(fourth)
        self.assertTrue(self.trust()["protected_view"])
        # File > Info shows the Protected View section with Enable Editing
        self.open_backstage("BackstageInfo")
        self.click_item("Enable Editing")
        self.wait(lambda: not self.trust()["protected_view"], "File > Info > Enable Editing didn't work")
        self.assertFalse(ui.state()["ribbon"]["backstage"])
        # a script can't change a workbook in Protected View either (Excel's read-only ProtectedViewWindow.Workbook)
        fifth = self.copy_marked("Fifth.ycg")
        self.person_opens(fifth)
        self.assertTrue(self.trust()["protected_view"])
        with self.assertRaises(Exception):
            excel.Range("B6").Value = 1
        excel.ThisWorkbook.Close(SaveChanges=False)  # reports nothing: the refusal belonged to the write (it did before)


SHEET = '''
def Worksheet_BeforeDoubleClick(Target, Cancel):
    _log("BeforeDoubleClick", Target.Address)
    return _flag("cancel_dbl")   # Excel's Cancel = True, the Python way

def Worksheet_SelectionChange(Target):
    _log("SelectionChange", Target.Address)

def Worksheet_Calculate():
    _log("Calculate")
    if _flag("write_quietly"):
        Application.EnableEvents = False
        Range("K2").Value = Range("K2").Value + 1   # K3 depends on it
        Application.EnableEvents = True
'''

HELPER = '''
import os as _os
from yc.excel import *

def _log(*parts):
    with open(LOG, "a", encoding="utf-8") as fh:
        fh.write("|".join(str(p) for p in parts) + "\\n")

def _flag(name):
    return _os.path.exists(_os.path.join(_os.path.dirname(LOG), name))
'''


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class EventsLeftTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="ycevents3_")
        cls.proj = os.path.join(cls.tmp, "proj")
        cls.log = os.path.join(cls.tmp, "events.txt")
        py = os.path.join(os.path.dirname(EXE), "runtime", "python", "python.exe")
        subprocess.run([py, "-E", "-s", "-c", f"import sys;sys.path.insert(0,r'{os.path.join(ROOT, 'python')}');from yc.cli import main;sys.exit(main())",
                        "init", cls.proj, "--sheet", "Sheet1=Sheet1"], check=True, capture_output=True)
        with open(os.path.join(cls.proj, "sheets", "Sheet1.py"), "a", encoding="utf-8") as f:
            f.write(f"\nLOG = r'{cls.log}'\n" + HELPER + SHEET)
        cls.book = os.path.join(cls.proj, "Book.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            from yc import excel

            excel.ThisWorkbook.SaveAs(cls.book)
        finally:
            p0.terminate()
            p0.wait(10)
        open(cls.log, "w").close()
        cls.proc = yc.launch(cls.book, hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        t0 = time.time()
        while time.time() - t0 < 30:
            st = cls.conn.request("python.status", {})
            if st.get("ready") and st.get("workbook_open_done"):
                break
            time.sleep(0.2)

    @classmethod
    def tearDownClass(cls):
        try:
            cls.proc.terminate()
            cls.proc.wait(10)
        finally:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        for f in ("cancel_dbl", "write_quietly"):
            self.flag(f, False)
        self.keys("k:Escape|w:2|m:2,2|w:2")
        from yc import excel

        excel.Range("A1").Select()
        self.keys("w:4")
        time.sleep(0.4)
        open(self.log, "w").close()

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.1)

    def flag(self, name, on):
        path = os.path.join(self.tmp, name)
        if on:
            open(path, "w").close()
        elif os.path.exists(path):
            os.remove(path)

    def lines(self, settle=0.8):
        time.sleep(settle)
        with open(self.log, encoding="utf-8") as fh:
            return [ln for ln in fh.read().splitlines() if ln]

    def test_return_true_cancels(self):
        from yc import ui

        self.flag("cancel_dbl", True)
        self.keys("Q:D4|w:8")
        self.assertIn("BeforeDoubleClick|$D$4", self.lines())
        self.assertNotIn("edit", ui.state())  # `return True` cancelled it: no editing (it was ignored before)
        self.flag("cancel_dbl", False)
        self.keys("Q:E4|w:8")
        self.assertEqual(ui.state().get("edit", {}).get("cell"), "E4")
        self.keys("k:Escape|w:2")

    def test_calculate_not_fired_again_by_a_write_with_events_off(self):
        from yc import excel

        excel.Range("K2").Value = 0
        excel.Range("K3").Formula = "=K2*2"
        excel.Range("K4").Formula = "=K1+1"
        self.lines(0.6)
        open(self.log, "w").close()
        self.flag("write_quietly", True)
        excel.Range("K1").Value = 5  # K4 is calculated: Calculate runs once and writes K2 with events off
        got = self.lines(1.5)
        self.flag("write_quietly", False)
        self.assertEqual(got.count("Calculate"), 1, got)  # (twice before: K3's recalculation fired it again)
        self.assertEqual(excel.Range("K3").Value, excel.Range("K2").Value * 2)

    def test_enter_inside_a_selection_fires_no_selection_change(self):
        # Excel: Enter / Tab move the active cell inside the selection, which stays the same: no SelectionChange
        self.keys("g:B2:D4|w:4")
        open(self.log, "w").close()
        self.keys("k:Enter|w:3|k:Tab|w:3|k:Enter|w:4")
        self.assertEqual([ln for ln in self.lines() if ln.startswith("SelectionChange")], [])
        self.keys("k:Down|w:4")  # leaving the selection's shape is a change
        self.assertTrue([ln for ln in self.lines() if ln.startswith("SelectionChange")])


if __name__ == "__main__":
    unittest.main()
