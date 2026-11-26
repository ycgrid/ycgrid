# SPDX-License-Identifier: Apache-2.0
"""Parts of the yc package that need no running YCGrid (python -m unittest discover -s python/tests)."""
import datetime as dt
import json
import os
import sys
import tempfile
import time
import unittest
from decimal import Decimal

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from yc import _msgpack, scaffold  # noqa: E402
from yc import excel  # noqa: E402
from yc.types import XlError, datetime_to_serial, serial_to_datetime  # noqa: E402


class MsgpackTest(unittest.TestCase):
    def test_roundtrip(self):
        for v in [None, True, False, 0, 127, -32, -33, 255, 65536, -70000, 2**40, 1.5, "中文", b"\x00\x01", [1, [2, 3]], {"a": 1, 2: "b"}]:
            self.assertEqual(_msgpack.unpackb(_msgpack.packb(v)), v)

    def test_decimal_exact(self):
        for s in ["1729658400.123456789", "-0.000000001", "0", "123456789012345678901234567"]:
            d = Decimal(s)
            self.assertEqual(_msgpack.unpackb(_msgpack.packb(d)), d)

    def test_error(self):
        self.assertEqual(_msgpack.unpackb(_msgpack.packb(XlError(7))).Text, "#N/A")
        self.assertEqual(XlError(2042), XlError(7))  # CVErr(xlErrNA)

    def test_error_numbers_are_excels(self):
        # CVErr numbers a script compares with: #SPILL! was 2009 and #CALC! 2010 (Excel 16: 2045 and 2050, measured)
        numbers = {"#NULL!": 2000, "#DIV/0!": 2007, "#VALUE!": 2015, "#REF!": 2023, "#NAME?": 2029, "#NUM!": 2036,
                   "#N/A": 2042, "#GETTING_DATA": 2043, "#SPILL!": 2045, "#CONNECT!": 2046, "#BLOCKED!": 2047,
                   "#CALC!": 2050}
        for code in range(1, 14):
            e = XlError(code)
            if e.Text in numbers:
                self.assertEqual(e.Number, numbers[e.Text], e.Text)
            self.assertEqual(XlError(e.Number), e, e.Text)  # CVErr(e.Number) is the same error
            self.assertEqual(excel.CVErr(e.Number).Text, e.Text)
        self.assertEqual(excel.CVErr(excel.xlErrSpill).Text, "#SPILL!")
        self.assertEqual(excel.CVErr(excel.xlErrCalc).Text, "#CALC!")
        self.assertEqual(excel.CVErr(excel.xlErrGettingData).Text, "#GETTING_DATA")
        self.assertEqual(excel.CVErr(excel.xlErrConnect).Text, "#CONNECT!")
        self.assertEqual(excel.CVErr(excel.xlErrBlocked).Text, "#BLOCKED!")


class MigrationDocTest(unittest.TestCase):
    def test_err_raise_example_runs(self):
        # skill/ycgrid-python/references/excel-migration.md writes Err.Raise as this: every name in it must exist
        ns = {}
        exec("from yc.excel import *\ne = YcError('message', Number=vbObjectError + 1)", ns)
        self.assertEqual(ns["e"].Number, -2147221503)


class DatesTest(unittest.TestCase):
    def test_serial(self):
        self.assertEqual(serial_to_datetime(45953), dt.datetime(2025, 10, 23))
        self.assertAlmostEqual(datetime_to_serial(dt.datetime(2026, 10, 23, 12)), 46318.5)
        self.assertEqual(serial_to_datetime(59), dt.datetime(1900, 2, 28))
        self.assertEqual(serial_to_datetime(61), dt.datetime(1900, 3, 1))
        self.assertEqual(serial_to_datetime(1), dt.datetime(1900, 1, 1))
        # a time alone (Ctrl+Shift+;) is on 1899-12-30, as Excel gives it, and goes back to the same serial
        self.assertEqual(serial_to_datetime(0.5), dt.datetime(1899, 12, 30, 12))
        self.assertAlmostEqual(datetime_to_serial(dt.datetime(1899, 12, 30, 12)), 0.5)
        self.assertAlmostEqual(datetime_to_serial(dt.datetime(1900, 1, 1)), 1)


class AddressTest(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(excel._parse_area("$B$2:A1"), (0, 0, 1, 1))
        self.assertEqual(excel._parse_area("C:A")[1:], (0, excel.ROW_END, 2))
        self.assertEqual(excel._parse_area("3:5"), (2, 0, 4, excel.MAX_COLS - 1))
        self.assertEqual(excel._col_name(16383), "XFD")
        self.assertEqual(excel._split_sheet("'My Sheet'!A1"), ("My Sheet", "A1"))
        with self.assertRaises(excel.ApplicationDefinedError):
            excel._parse_area("XFE1")
        for bad in ("A0", "A0:B2", "0:0"):   # row 0 used to become row 1
            with self.assertRaises(excel.ApplicationDefinedError):
                excel._parse_area(bad)

    def test_values_to_wire(self):
        w = excel._to_wire_2d
        self.assertEqual(w((x for x in (1, 2)), 1, 2), [[1, 2]])            # a generator is a row
        self.assertEqual(w(([r, r * 2] for r in (1, 2)), 2, 2), [[1, 2], [2, 4]])
        self.assertEqual(w(range(3), 1, 3), [[0, 1, 2]])
        self.assertEqual(w("abc", 1, 1), "abc")                               # text is one value
        with self.assertRaises(excel.ApplicationDefinedError):
            w([[1, 2], 3], 2, 2)                                              # a mix of rows and scalars
        with self.assertRaises(excel.ApplicationDefinedError):
            w([1, [2, 3]], 1, 2)

    def test_excel_helpers(self):
        self.assertEqual(excel.Mid("YCGrid", 2, 3), "CGr")
        self.assertEqual(excel.InStr("abcabc", "c"), 3)
        self.assertEqual(excel.InStr(4, "abcabc", "c"), 6)
        self.assertTrue(excel.Like("A123", "A###"))
        self.assertTrue(excel.Like("bat", "[a-c]?t"))
        self.assertEqual(excel.Mod(-7, 3), -1)  # Excel: the sign follows the divisor
        self.assertEqual(excel.RGB(255, 0, 0), 255)
        self.assertEqual(excel.DateDiff("m", "2026/1/31", "2026/3/1"), 2)
        self.assertEqual(excel.DateAdd("m", 1, "2026/1/31"), dt.datetime(2026, 2, 28))


class PyFormulaTest(unittest.TestCase):
    def test_py_with_no_code_yet_shows_nothing(self):
        # Formulas > Function Library > PY inserts =PY() before the code is typed: no error in the Error Log
        from yc import host

        self.assertEqual(host._py_formula(), "")
        self.assertEqual(host._py_formula("   "), "")


class DebugPortTest(unittest.TestCase):
    def test_a_busy_port_falls_back_to_a_free_one(self):
        # Debug in VS Code failed when port 5678 was taken (another YCGrid's debugger): WinError 10048
        from yc import debug

        class FakeDebugpy:
            def __init__(self):
                self.asked = []

            def listen(self, address):
                self.asked.append(address[1])
                if address[1] == 5678:
                    raise RuntimeError("Can't listen for client connections: [WinError 10048] address in use")
                return address[0], 61234

        d = FakeDebugpy()
        self.assertEqual(debug._listen_on(d, "127.0.0.1", 5678), 61234)
        self.assertEqual(d.asked, [5678, 0])
        with self.assertRaises(RuntimeError):  # another failure is reported, not hidden
            class Broken(FakeDebugpy):
                def listen(self, address):
                    raise RuntimeError("debugpy is broken")
            debug._listen_on(Broken(), "127.0.0.1", 5678)


class CollectionCallTest(unittest.TestCase):
    def test_call_without_index_returns_the_collection(self):
        # like Excel: ws.ChartObjects() / ws.OLEObjects() is the collection, ws.ChartObjects(1) an item
        from yc import ui

        for cls in (ui.ChartObjects, ui.OLEObjects, ui.Shapes):
            c = cls(1)
            self.assertIs(c(), c)

    def test_columns_and_rows_count_lines(self):
        # Excel: Columns("B:D").Count is 3 and For Each goes column by column; Range("B:D").Count counts cells
        ws = excel.Worksheet(1)
        cols = ws.Columns("B:D")
        self.assertEqual(cols.Count, 3)
        self.assertEqual([c.Column for c in cols], [2, 3, 4])
        rows = ws.Rows("2:4")
        self.assertEqual(rows.Count, 3)
        self.assertEqual([r.Row for r in rows], [2, 3, 4])
        self.assertEqual(ws.Columns(2).Count, 1)


class ScaffoldTest(unittest.TestCase):
    def test_init(self):
        with tempfile.TemporaryDirectory() as d:
            created = scaffold.init_project(d, [("Sheet1", "工作表1"), ("Sheet2", "報價")])
            self.assertTrue(os.path.isfile(os.path.join(d, "main.py")))
            self.assertTrue(os.path.isfile(os.path.join(d, "sheets", "Sheet2.py")))
            self.assertTrue(any(p.endswith("AGENTS.md") for p in created))
            # ready for an agent, without sample code
            self.assertFalse(os.path.exists(os.path.join(d, "examples")))
            self.assertTrue(os.path.isfile(os.path.join(d, ".claude", "settings.json")))
            self.assertTrue(os.path.isfile(os.path.join(d, ".mcp.json")))
            # the skill where each agent reads it: Claude Code, Codex (only .agents/skills), GitHub Copilot
            for agent_dir in (".claude", ".agents", ".github"):
                self.assertTrue(os.path.isfile(os.path.join(d, agent_dir, "skills", "ycgrid-python", "SKILL.md")), agent_dir)
            # VS Code opens without prompts
            with open(os.path.join(d, ".vscode", "settings.json"), encoding="utf-8") as f:
                st = json.load(f)
            self.assertTrue(st["extensions.ignoreRecommendations"])
            self.assertEqual(st["git.openRepositoryInParentFolders"], "never")
            # a second run does not overwrite
            self.assertEqual(scaffold.init_project(d, [("Sheet1", "工作表1")]), [])

    def test_project_skill_copies_follow_ycgrid(self):
        src = scaffold.ycgrid_skill_dir()
        self.assertTrue(os.path.isfile(os.path.join(src, "SKILL.md")), src)
        with tempfile.TemporaryDirectory() as d:
            old = os.path.join(d, ".claude", "skills", "ycgrid-python")
            os.makedirs(os.path.join(old, "references"))
            with open(os.path.join(old, "SKILL.md"), "w", encoding="utf-8") as f:
                f.write("an old version")
            with open(os.path.join(old, "references", "gone.md"), "w", encoding="utf-8") as f:
                f.write("a file the skill no longer has")
            changed = scaffold.sync_skill(d)
            with open(os.path.join(old, "SKILL.md"), encoding="utf-8") as a, open(os.path.join(src, "SKILL.md"), encoding="utf-8") as b:
                self.assertEqual(a.read(), b.read())
            self.assertFalse(os.path.exists(os.path.join(old, "references", "gone.md")))
            self.assertTrue(os.path.isfile(os.path.join(old, "references", "testing.md")))
            self.assertTrue(changed)
            self.assertEqual(scaffold.sync_skill(d), [])                       # up to date: nothing written
            self.assertFalse(os.path.exists(os.path.join(d, ".github", "skills")))   # copies it never had aren't made
            self.assertFalse(os.path.exists(os.path.join(d, ".agents", "skills")))

    def test_vscode_button_adds_codex_skill_to_older_projects(self):
        # a project made before the Codex copy: .claude and .github only; Codex reads .agents/skills alone
        with tempfile.TemporaryDirectory() as d:
            src = scaffold.ycgrid_skill_dir()
            import shutil

            for agent_dir in (".claude", ".github"):
                shutil.copytree(src, os.path.join(d, agent_dir, "skills", "ycgrid-python"))
            scaffold.ensure_dev_env(d)
            codex = os.path.join(d, ".agents", "skills", "ycgrid-python")
            with open(os.path.join(codex, "SKILL.md"), encoding="utf-8") as a, open(os.path.join(src, "SKILL.md"), encoding="utf-8") as b:
                self.assertEqual(a.read(), b.read())
            self.assertTrue(os.path.isfile(os.path.join(codex, "references", "object-model.md")))
        # a folder without any skill copy gets none
        with tempfile.TemporaryDirectory() as d:
            scaffold.ensure_dev_env(d)
            self.assertFalse(os.path.exists(os.path.join(d, ".agents")))


class R1C1Test(unittest.TestCase):
    def test_a1_to_r1c1(self):
        from yc._r1c1 import a1_to_r1c1

        self.assertEqual(a1_to_r1c1("=SUM(A1:B3)+$C$2", 4, 4), "=SUM(R[-4]C[-4]:R[-2]C[-3])+R2C3")
        self.assertEqual(a1_to_r1c1('="A1"&RATE(1,2)&單價', 0, 0), '="A1"&RATE(1,2)&單價')
        self.assertEqual(a1_to_r1c1("=A:B+3:3+'My Sheet'!B$1", 4, 4), "=C[-4]:C[-3]+R[-2]+'My Sheet'!R1C[-3]")

    def test_r1c1_to_a1(self):
        from yc._r1c1 import r1c1_to_a1

        self.assertEqual(r1c1_to_a1("=R[-1]C+RC[-1]+R1C1+SUM(R[-3]C:R[-1]C)+ROUND(R2C,0)", 9, 2), "=C9+B10+$A$1+SUM(C7:C9)+ROUND(C$2,0)")
        self.assertEqual(r1c1_to_a1("=C[1]+R", 9, 2), "=D:D+10:10")
        self.assertEqual(r1c1_to_a1("=R[-5]C", 2, 0), "=A1048574")  # wraps around the sheet, as Excel (measured)

    def test_roundtrip(self):
        from yc._r1c1 import a1_to_r1c1, r1c1_to_a1

        for f in ["=A1*$B$2+Sheet2!C$3", "=SUM($A:$A)/COUNT(2:2)", "=IF(A1>0,\"A1\",B2)"]:
            self.assertEqual(r1c1_to_a1(a1_to_r1c1(f, 5, 7), 5, 7), f)


class RequestTimeoutTest(unittest.TestCase):
    """A reply that never comes raises E_TIMEOUT instead of waiting for ever (the controller pipe too)."""

    def test_duplex_request_times_out(self):
        from yc import _conn

        r, w_unused = os.pipe()
        r_unused, w = os.pipe()
        conn = _conn.Connection(open(r, "rb", buffering=0), open(w, "wb", buffering=0), duplex=True)
        try:
            t0 = time.monotonic()
            with self.assertRaises(_conn.YcError) as cm:
                conn.request("hello", {}, timeout=0.3)
            self.assertEqual(cm.exception.Code, "E_TIMEOUT")
            self.assertLess(time.monotonic() - t0, 3)
            conn.default_timeout = 0.2                       # the connection's own default
            with self.assertRaises(_conn.YcError):
                conn.request("hello", {})
        finally:
            conn.close()
            for fd in (w_unused, r_unused):
                os.close(fd)

if __name__ == "__main__":
    unittest.main()


class RequestShapeTest(unittest.TestCase):
    """The requests new Range / Worksheet / Application members send (no YCGrid needed: _rq is replaced)."""

    def setUp(self):
        self.sent = []
        self.reply = None
        self.replies = {}  # method -> its own reply (the rest get self.reply)
        self._orig = excel._rq

        def fake(method, **params):
            self.sent.append((method, params))
            return self.replies.get(method, self.reply)

        from yc import rules

        self._orig_rules = rules._rq
        excel._rq = fake
        rules._rq = fake

    def tearDown(self):
        from yc import rules

        excel._rq = self._orig
        rules._rq = self._orig_rules

    def test_text_to_columns(self):
        rng = excel.Range._from(1, [(0, 0, 9, 0)])
        rng.TextToColumns(DataType=excel.xlDelimited, Comma=True, FieldInfo=[[1, excel.xlGeneralFormat], [3, excel.xlSkipColumn]])
        m, p = self.sent[-1]
        self.assertEqual(m, "range.text_to_columns")
        self.assertTrue(p["comma"])
        self.assertEqual(p["types"], [1, 1, 9])
        self.assertEqual(p["qualifier"], '"')
        rng.TextToColumns(DataType=excel.xlFixedWidth, FieldInfo=[[0, 1], [5, 2], [9, 1]])
        self.assertEqual(self.sent[-1][1]["breaks"], [5, 9])
        self.assertTrue(self.sent[-1][1]["fixed"])

    def test_remove_duplicates_and_outline(self):
        excel.Range._from(1, [(0, 0, 9, 2)]).RemoveDuplicates(Columns=[1, 3], Header=excel.xlYes)
        self.assertEqual(self.sent[-1], ("range.remove_duplicates", {"sheet": 1, "rect": [0, 0, 9, 2], "cols": [1, 3], "header": True}))
        excel.Range._from(1, [(3, 0, 5, excel.MAX_COLS - 1)]).Group()
        self.assertEqual(self.sent[-1][1], {"sheet": 1, "rows": True, "a": 3, "b": 5, "group": True})
        excel.Range._from(1, [(0, 2, excel.ROW_END, 3)]).Ungroup()
        self.assertEqual(self.sent[-1][1], {"sheet": 1, "rows": False, "a": 2, "b": 3, "group": False})
        excel.Worksheet(1).Outline.ShowLevels(RowLevels=2)
        self.assertEqual(self.sent[-1], ("outline.show_levels", {"sheet": 1, "rows": 2, "cols": 0}))

    def test_watches_and_reference_style(self):
        self.reply = [[1, 4, 2]]
        w = excel.Application.Watches.Add(excel.Range._from(1, [(4, 2, 4, 2)]))
        self.assertEqual(self.sent[-1], ("watch.add", {"sheet": 1, "row": 4, "col": 2}))
        self.assertEqual(w.Source._a, (4, 2, 4, 2))
        self.reply = True
        self.assertEqual(excel.Application.ReferenceStyle, excel.xlR1C1)
        excel.Application.ReferenceStyle = excel.xlA1
        self.assertEqual(self.sent[-1], ("app.reference_style", {"r1c1": False}))


class UiCoverageRequestTest(RequestShapeTest):
    """The members added so every UI command has a Python entry point (PLAN 13.5a)."""

    def test_series_indent_outline_picture(self):
        rng = excel.Range._from(1, [(0, 0, 9, 0)])
        rng.DataSeries(Type=excel.xlGrowth, Step=2, Stop=1000)
        self.assertEqual(self.sent[-1], ("range.series", {"sheet": 1, "rect": [0, 0, 9, 0], "by_cols": True, "type": 1, "unit": 0,
                                                          "step": 2.0, "stop": 1000.0, "trend": False}))
        excel.Range._from(1, [(0, 0, 0, 5)]).DataSeries(Type=excel.xlChronological, Date=excel.xlMonth)
        self.assertEqual((self.sent[-1][1]["by_cols"], self.sent[-1][1]["type"], self.sent[-1][1]["unit"]), (False, 2, 2))
        self.assertNotIn("stop", self.sent[-1][1])
        self.reply = {"indent": 2}
        rng.InsertIndent(3)
        self.assertEqual(self.sent[-1][0], "range.style")
        excel.Range._from(1, [(2, 1, 4, 3)]).ClearOutline()
        self.assertEqual(self.sent[-2], ("outline.level", {"sheet": 1, "rows": True, "index": 2, "b": 4, "set": 1}))
        self.assertEqual(self.sent[-1], ("outline.level", {"sheet": 1, "rows": False, "index": 1, "b": 3, "set": 1}))

    def test_application_ui_members(self):
        excel.Application.Undo()
        self.assertEqual(self.sent[-1], ("ui.command", {"id": "Undo"}))
        excel.Application.Redo()
        self.assertEqual(self.sent[-1], ("ui.command", {"id": "Redo"}))
        excel.Application.DisplayFormulaBar = False
        self.assertEqual(self.sent[-1], ("cmdbar.pane", {"name": "formula_bar", "visible": False}))
        excel.Application.RibbonDisplay = excel.ycRibbonTabsOnly
        self.assertEqual(self.sent[-1], ("cmdbar.ribbon_mode", {"mode": 1}))
        self.reply = None  # cancelled
        self.assertIs(excel.Application.GetOpenFilename("CSV (*.csv),*.csv", MultiSelect=True), False)
        self.assertEqual(self.sent[-1][1]["filter"], "CSV (*.csv),*.csv")
        self.assertTrue(self.sent[-1][1]["multi"])
        self.reply = r"C:\data\out.csv"
        self.assertEqual(excel.Application.GetSaveAsFilename("out.csv"), r"C:\data\out.csv")
        self.assertEqual(self.sent[-1][1]["initial"], "out.csv")
        excel.Windows.Arrange()
        self.assertEqual(self.sent[-1], ("layout.window", {"arrange": 0, "active_workbook": False}))
        excel.Windows.Arrange(excel.xlArrangeStyleVertical, ActiveWorkbook=True)
        self.assertEqual(self.sent[-1][1]["active_workbook"], True)

    def test_export_image(self):
        self.reply = {"width": 10, "height": 5, "scaled_down": False, "note": ""}
        r = excel.Range._from(2, [(0, 0, 9, 3)]).ExportImage(Scale=2, Headings=True)
        self.assertEqual(self.sent[-1], ("image.export", {"what": "range", "path": "", "headings": True, "scale": 2.0, "sheet": 2,
                                                          "rect": [0, 0, 9, 3]}))
        self.assertIsNone(r["path"])
        self.reply = {"width": 10, "height": 5}
        excel.Worksheet(3).ExportImage("out.png", Gridlines=False)
        m, p = self.sent[-1]
        self.assertEqual((p["what"], p["sheet"], p["gridlines"]), ("sheet", 3, False))
        self.assertTrue(os.path.isabs(p["path"]) and p["path"].endswith("out.png"))
        self.reply = {"width": 10, "height": 5}
        excel.Workbook.ExportImage(excel.ThisWorkbook, None)
        self.assertEqual(self.sent[-1][1]["what"], "workbook")

    def test_commands_schema_covers_every_command(self):
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
        import check_commands

        excel._rq = self._orig  # the check only imports yc
        self.assertEqual(check_commands.check(), [])


class CodeChecksTest(unittest.TestCase):
    """The checks that keep a kind of bug from coming back: every model field accounted for (saved, follows its
    cells), and the lessons of AGENTS.md that a pattern can catch."""

    def _run(self, name):
        import subprocess
        script = os.path.join(os.path.dirname(__file__), "..", "..", "scripts", name)
        r = subprocess.run([sys.executable, "-E", "-s", script], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_every_model_field_is_accounted_for(self):
        self._run("check_model_fields.py")

    def test_every_member_of_excels_object_model_is_yours_not_planned_or_a_gap(self):
        self._run("check_excel_parity.py")

    def test_lessons_that_a_pattern_catches(self):
        self._run("check_lessons.py")


class RandomRoundsRuleTest(unittest.TestCase):
    """random_rounds.py's stopping rule: the hours of rounds since the last real finding."""

    @staticmethod
    def rule():
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
        import random_rounds

        return random_rounds.hours_since_last_finding

    @staticmethod
    def a_round(date, tool, seed, hours, result="ok"):
        return {"date": date, "tool": tool, "seed": str(seed), "seconds": str(hours * 3600), "result": result}

    @staticmethod
    def found(date, tool, seed="-", status="fixed"):
        return {"date": date, "tool": tool, "seed": str(seed), "kind": "real", "status": status, "summary": ""}

    def test_a_finding_by_hand_days_after_the_rounds_counts_none_of_them(self):
        """Placed at the round of the finding before it, a finding by hand three days after the last round counted
        every round since as clean, and the report read converged on the day four real bugs were found."""
        runs = [self.a_round("2026-11-21", "chaos", 1, 10, "fail"), self.a_round("2026-11-22", "basics", 2, 15),
                self.a_round("2026-11-23", "basics", 3, 6)]
        findings = [self.found("2026-11-21", "chaos", 1), self.found("2026-11-26", "screen_walk")]
        since, open_ = self.rule()(runs, findings)
        self.assertEqual(since, 0.0)
        self.assertEqual(open_, [])

    def test_a_finding_by_hand_beside_a_round_that_day_counts_from_that_round(self):
        runs = [self.a_round("2026-11-21", "chaos", 1, 10, "fail"), self.a_round("2026-11-21", "basics", 2, 4),
                self.a_round("2026-11-22", "basics", 3, 6)]
        findings = [self.found("2026-11-21", "chaos", 1), self.found("2026-11-21", "review")]
        since, _ = self.rule()(runs, findings)
        self.assertAlmostEqual(since, 10.0)  # the two rounds after chaos's

    def test_an_open_finding_is_reported(self):
        since, open_ = self.rule()([self.a_round("2026-11-21", "basics", 1, 30)],
                                   [self.found("2026-11-20", "review", status="open")])
        self.assertAlmostEqual(since, 30.0)
        self.assertEqual(len(open_), 1)


class BoundaryHistoryTest(unittest.TestCase):
    """check_boundary --history: a secret a later commit removed is still found in the history."""

    def test_a_control_character_is_found(self):
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
        import check_boundary

        # a heredoc turned the backslash and b of scripts\\basics.py into a backspace; tabs and line ends are text
        self.assertTrue(check_boundary.CONTROL.search("scripts" + chr(8) + "asics.py"))
        self.assertTrue(check_boundary.CONTROL.search("a" + chr(0) + "b"))
        self.assertFalse(check_boundary.CONTROL.search("a\tb\r\n"))

    def test_removed_secret_is_found_in_history(self):
        import subprocess

        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
        import check_boundary

        with tempfile.TemporaryDirectory() as root:
            def git(*args):
                subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", *args], cwd=root,
                               check=True, capture_output=True)

            def write(name, text):
                with open(os.path.join(root, name), "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)

            git("init", "-q")
            write("feed.py", "URL = 'wss://example.com'\napi_key = \"abcd1234efgh5678\"\nlocal = '127.0.0.1'\n")
            write("deploy.pem", "x\n")
            write("note.md", "token = \"a fine example 1\"  # boundary: ok\n")
            git("add", "-A")
            git("commit", "-q", "-m", "first")
            write("feed.py", "URL = 'wss://example.com'\napi_key = os.environ['FEED_KEY']\n")
            os.remove(os.path.join(root, "deploy.pem"))
            git("add", "-A")
            git("commit", "-q", "-m", "remove the key")
            rules = check_boundary.RULES
            now = []
            for rel in check_boundary.tracked_files(root):
                with open(os.path.join(root, rel), encoding="utf-8") as f:
                    now += [hit for line in f for hit in check_boundary.scan_line(rules, line)]
            self.assertEqual(now, [])
            found = check_boundary.scan_history(rules, ["HEAD", "no-such-branch"], root=root)
            where = sorted((f.split(":", 1)[1], no, name) for f, no, name, _ in found)
            self.assertEqual(where, [("deploy.pem", 0, "a file type that holds secrets"),
                                     ("feed.py", 2, "API key / token in code")])
            self.assertEqual(check_boundary.scan_history(rules, ["no-such-branch"], root=root), [])

    def test_source_files_need_the_license_line(self):
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
        import check_boundary

        lacks = check_boundary.lacks_license_line
        spdx = "// SPDX-License-Identifier: Apache-2.0\n"
        self.assertTrue(lacks("src/core/new.cpp", ["#pragma once\n"]))
        self.assertTrue(lacks("python/yc/new.py", ['"""Doc."""\n']))
        self.assertTrue(lacks("apps/ycgrid/CMakeLists.txt", ["add_executable(x)\n"]))
        self.assertTrue(lacks("scripts/new.cmd", ["@echo off\n", "chcp 65001 >nul\n", "rem Usage\n"]))
        self.assertFalse(lacks("src/core/new.cpp", [spdx, "#pragma once\n"]))
        self.assertFalse(lacks("scripts/new.cmd", ["@echo off\n", "chcp 65001 >nul\n",
                                                   "rem SPDX-License-Identifier: Apache-2.0\n"]))
        self.assertFalse(lacks("python/yc/x.py", ["#!/usr/bin/env python\n", "# SPDX-License-Identifier: Apache-2.0\n"]))
        # generated files, other people's code and files that aren't source are left alone
        self.assertFalse(lacks("src/core/i18n_zh.inc", ["// Generated by: python scripts/i18n_build.py build\n"]))
        self.assertFalse(lacks("third_party/imgui/imgui.cpp", ["// dear imgui\n"]))
        self.assertFalse(lacks("docs/notes.md", ["# Notes\n"]))
        self.assertFalse(lacks("schema/commands.yaml", ["commands:\n"]))


class VersionTest(unittest.TestCase):
    """A release names its version in five files (docs/releasing.md): they agree, or the zip, the wheel, the MCP
    server and Windows' file properties each report another one."""

    def test_every_file_names_the_same_version(self):
        import re

        root = os.path.join(os.path.dirname(__file__), "..", "..")

        def read(rel):
            with open(os.path.join(root, rel), encoding="utf-8") as f:
                return f.read()

        version = read("VERSION").strip()
        self.assertRegex(version, r"^\d+\.\d+\.\d+$")
        import yc
        found = {
            "python/yc/__init__.py": yc.__version__,
            "vcpkg.json": json.loads(read("vcpkg.json"))["version-string"],
            "mcp/tools.json": json.loads(read("mcp/tools.json"))["server"]["version"],
            "apps/ycgrid/ycgrid.manifest": re.search(r'name="YCGrid.App" version="([^"]+)\.0"',
                                                     read("apps/ycgrid/ycgrid.manifest")).group(1),
        }
        self.assertEqual(found, dict.fromkeys(found, version))


class WheelMetadataTest(unittest.TestCase):
    """tools/make_wheel.py: the wheel carries what PyPI shows (license, links, a README whose links work there)."""

    def setUp(self):
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
        import make_wheel
        self.mw = make_wheel

    def test_readme_links_become_absolute(self):
        text = ("[Contributing](CONTRIBUTING.md) [Bugs](.github/ISSUE_TEMPLATE/bug_report.yml) "
                "[Up](#license) [Site](https://example.com/x) ![Shot](docs/images/en/live.gif) [Here](./docs/a.md)")
        out = self.mw.pypi_readme(text)
        self.assertIn("[Contributing](https://github.com/ycgrid/ycgrid/blob/main/CONTRIBUTING.md)", out)
        self.assertIn("(https://github.com/ycgrid/ycgrid/blob/main/.github/ISSUE_TEMPLATE/bug_report.yml)", out)
        self.assertIn("[Up](#license)", out)
        self.assertIn("[Site](https://example.com/x)", out)
        self.assertIn("![Shot](https://raw.githubusercontent.com/ycgrid/ycgrid/main/docs/images/en/live.gif)", out)
        self.assertIn("[Here](https://github.com/ycgrid/ycgrid/blob/main/docs/a.md)", out)

    def test_metadata_names_the_license_and_its_files(self):
        import email.parser
        import zipfile

        with tempfile.TemporaryDirectory() as tmp:
            dist = os.path.join(tmp, "dist")
            os.makedirs(os.path.join(dist, "licenses"))
            with open(os.path.join(dist, "ycgrid.exe"), "wb") as f:
                f.write(b"MZ")
            for name in ("ycgrid-LICENSE.txt", "imgui-LICENSE.txt"):
                with open(os.path.join(dist, "licenses", name), "w", encoding="utf-8") as f:
                    f.write("license text\n")
            import pathlib
            whl = self.mw.build(pathlib.Path(dist), pathlib.Path(tmp, "out"))
            with zipfile.ZipFile(whl) as z:
                names = set(z.namelist())
                info = next(n.split("/")[0] for n in names if n.endswith(".dist-info/METADATA"))
                meta = email.parser.Parser().parsestr(z.read(f"{info}/METADATA").decode("utf-8"))
            self.assertEqual(meta["Metadata-Version"], "2.4")
            self.assertEqual(meta["License-Expression"], "Apache-2.0")
            self.assertEqual(sorted(meta.get_all("License-File")), ["imgui-LICENSE.txt", "ycgrid-LICENSE.txt"])
            for lic in meta.get_all("License-File"):
                self.assertIn(f"{info}/licenses/{lic}", names)
            urls = dict(u.split(", ", 1) for u in meta.get_all("Project-URL"))
            self.assertEqual(urls["Source"], "https://github.com/ycgrid/ycgrid")
            self.assertNotIn("](CONTRIBUTING.md)", meta.get_payload())

    def test_a_dist_without_licenses_is_refused(self):
        import pathlib

        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "ycgrid.exe"), "wb") as f:
                f.write(b"MZ")
            with self.assertRaises(SystemExit):
                self.mw.build(pathlib.Path(tmp), pathlib.Path(tmp, "out"))

    def test_a_license_the_notices_dont_list_is_refused(self):
        # an older build left the license of ImGui's embedded fonts, which YCGrid no longer links, in dist_release
        import pathlib

        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "licenses"))
            with open(os.path.join(tmp, "ycgrid.exe"), "wb") as f:
                f.write(b"MZ")
            for name in ("ycgrid-LICENSE.txt", "NOTICE.txt", "zlib-LICENSE.txt", "proggy-fonts-LICENSE.txt"):
                with open(os.path.join(tmp, "licenses", name), "w", encoding="utf-8") as f:
                    f.write("license text\n")
            with self.assertRaises(SystemExit) as e:
                self.mw.build(pathlib.Path(tmp), pathlib.Path(tmp, "out"))
            self.assertIn("proggy-fonts-LICENSE.txt", str(e.exception))
            self.assertNotIn("zlib", str(e.exception))


class HostShutdownTest(unittest.TestCase):
    """shutdown: Workbook_BeforeClose only when the workbook closes; Workbook_HostShutdown always, cut off after its limit."""

    def _run(self, reason, main):
        from yc import host

        saved = dict(host._modules), host._conn_obj, host.HOST_SHUTDOWN_LIMIT

        class Conn:
            def __init__(self):
                self.sent = []

            def notify(self, method, params):
                self.sent.append((method, params))

        host._modules.clear()
        host._modules["main"] = main
        host._conn_obj = Conn()
        host.HOST_SHUTDOWN_LIMIT = 0.3
        try:
            self.assertFalse(host._dispatch("shutdown", {"reason": reason}))
            return host._conn_obj.sent
        finally:
            host._modules.clear()
            host._modules.update(saved[0])
            host._conn_obj, host.HOST_SHUTDOWN_LIMIT = saved[1], saved[2]

    def test_restart_close_and_limit(self):
        import time
        import types

        calls = []
        main = types.SimpleNamespace(Workbook_BeforeClose=lambda cancel: calls.append("close"),
                                     Workbook_HostShutdown=lambda: calls.append("shutdown"))
        self._run("restart", main)
        self.assertEqual(calls, ["shutdown"])  # a restart doesn't close the workbook
        calls.clear()
        self._run("close", main)
        self.assertEqual(calls, ["close", "shutdown"])  # the PLAN's order
        slow = types.SimpleNamespace(Workbook_HostShutdown=lambda: time.sleep(5))
        t0 = time.perf_counter()
        sent = self._run("reload", slow)
        self.assertLess(time.perf_counter() - t0, 2.0)  # cut off after the limit
        self.assertTrue(any("didn't return" in p.get("text", "") for m, p in sent))


class HotReloadTest(unittest.TestCase):
    """Saving a module swaps its functions and classes in place; its other top-level code doesn't run again."""

    def test_new_constants_run_existing_names_keep_their_value(self):
        from yc import host

        d = tempfile.mkdtemp()
        path = os.path.join(d, "consts.py")
        with open(path, "w", encoding="utf-8") as f:
            f.write("counter = 0\nROW = 14\n\ndef bump():\n    global counter\n    counter += 1\n")
        mod = host._load_file("modules.consts_test", path)
        mod.bump()
        mod.bump()
        with open(path, "w", encoding="utf-8") as f:
            f.write("counter = 0\nROW = 14\nZONE = f'B{ROW}:F25'\nNEW_CONST: int = 5\nA, B = 1, 2\n\n"
                    "def bump():\n    global counter\n    counter += 1\n\ndef f():\n    return NEW_CONST, ZONE, A + B\n")
        r = host.hot_reload(path)
        self.assertEqual(r["constants"], ["ZONE", "NEW_CONST", "A", "B"])
        self.assertEqual(mod.f(), (5, "B14:F25", 3))
        self.assertEqual(mod.counter, 2)  # state kept: the existing assignment didn't run again
        with open(path, "w", encoding="utf-8") as f:
            f.write("counter = 0\nBROKEN = 1 / 0\n\ndef f():\n    return BROKEN\n")
        with self.assertRaises(host.NeedsRestart):
            host.hot_reload(path)

    def test_a_constant_edited_in_the_source_takes_its_new_value(self):
        from yc import host

        d = tempfile.mkdtemp()
        path = os.path.join(d, "ver.py")
        src = "UI_VERSION = 4\ncounter = 0\n_cache = {}\nLABELS = ('a', 'b')\n\ndef bump():\n    global counter\n    counter += 1\n"
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        mod = host._load_file("modules.ver_test", path)
        mod.bump()
        mod._cache["k"] = 1
        with open(path, "w", encoding="utf-8") as f:
            f.write(src.replace("UI_VERSION = 4", "UI_VERSION = 5").replace("('a', 'b')", "('a', 'b', 'c')"))
        r = host.hot_reload(path)
        self.assertEqual(r["updated"], ["UI_VERSION", "LABELS"])
        self.assertEqual((mod.UI_VERSION, mod.LABELS), (5, ("a", "b", "c")))
        self.assertEqual(mod.counter, 1)              # unchanged in the source: the code's value stays
        self.assertEqual(mod._cache, {"k": 1})        # a dict is state, never re-run
        r = host.hot_reload(path)                     # saved again without a change: nothing re-runs
        self.assertEqual(r["updated"], [])

    def test_workbook_after_reload_gets_the_reloaded_files(self):
        from yc import host

        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "modules"))
        main_py, mod_py = os.path.join(d, "main.py"), os.path.join(d, "modules", "Desk.py")
        with open(main_py, "w", encoding="utf-8") as f:
            f.write("CALLS = []\n\ndef Workbook_AfterReload(Files):\n    CALLS.append(list(Files))\n")
        with open(mod_py, "w", encoding="utf-8") as f:
            f.write("def f():\n    return 1\n")

        class Conn:
            def __init__(self):
                self.sent = []

            def notify(self, m, p=None):
                self.sent.append(m)

        saved = (host._app_dir, dict(host._sources), dict(host._modules), host._conn_obj)
        try:
            host._app_dir, host._conn_obj = d, Conn()
            host._sources.clear()
            main = host._load_file("main", main_py)
            host._load_file("modules.Desk", mod_py)
            time.sleep(0.05)
            with open(mod_py, "w", encoding="utf-8") as f:
                f.write("def f():\n    return 2\n")
            os.utime(mod_py, None)
            host._sources[mod_py] = (host._sources[mod_py][0], 0.0, None)   # seen as changed
            self.assertTrue(host._code_changed())
            self.assertEqual(main.CALLS, [["modules/Desk.py"]])
        finally:
            host._app_dir, host._conn_obj = saved[0], saved[3]
            host._sources.clear()
            host._sources.update(saved[1])
            host._modules.clear()
            host._modules.update(saved[2])

    def test_functions_and_classes_are_patched_in_place(self):
        from yc import host

        d = tempfile.mkdtemp()
        path = os.path.join(d, "strat.py")
        old = "STARTS = []\nSTARTS.append(1)\n\ndef signal(x):\n    return x + 1\n\nclass Bot:\n    def act(self):\n        return 'old'\n"
        new = old.replace("x + 1", "x * 10").replace("'old'", "'new'") + "\ndef extra():\n    return signal(2)\n"
        with open(path, "w", encoding="utf-8") as f:
            f.write(old)
        mod = host._load_file("modules.strat_test", path)
        callback = mod.signal          # e.g. registered with on_click or a feed thread
        bot = mod.Bot()                # a live object
        mod.STARTS.append("state")
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)
        r = host.hot_reload(path)
        self.assertEqual(sorted(r["patched"]), ["Bot", "signal"])
        self.assertEqual(r["added"], ["extra"])
        self.assertEqual(callback(3), 30)        # the old reference runs the new code
        self.assertEqual(bot.act(), "new")       # the existing instance gets the new method
        self.assertEqual(mod.extra(), 20)
        self.assertEqual(mod.STARTS, [1, "state"])  # top-level code didn't run again: state kept
        with open(path, "w", encoding="utf-8") as f:
            f.write("def signal(x)\n    return 0\n")
        with self.assertRaises(SyntaxError):
            host.hot_reload(path)
        self.assertEqual(callback(3), 30)        # a syntax error leaves the old code running

    def test_new_init_attributes_are_reported(self):
        from yc import host

        d = tempfile.mkdtemp()
        path = os.path.join(d, "desk.py")
        with open(path, "w", encoding="utf-8") as f:
            f.write("class Desk:\n    def __init__(self):\n        self.a = 1\n")
        mod = host._load_file("modules.desk_attr_test", path)
        with open(path, "w", encoding="utf-8") as f:
            f.write("class Desk:\n    def __init__(self):\n        self.a = 1\n        self.stop_event = None\n        self.lock = 2\n")
        r = host.hot_reload(path)
        self.assertEqual(r["new_attrs"], {"Desk": ["lock", "stop_event"]})   # no Desk existed: reloaded in place
        desk = mod.Desk()
        self.assertEqual(desk.lock, 2)
        # a Desk made before would break on the attributes its old __init__ never set: the host restarts instead
        with open(path, "w", encoding="utf-8") as f:
            f.write("class Desk:\n    def __init__(self):\n        self.a = 1\n        self.stop_event = None\n"
                    "        self.lock = 2\n        self.clicks = []\n")
        with self.assertRaises(host.NeedsRestart) as cm:
            host.hot_reload(path)
        self.assertFalse(cm.exception.error)                                    # information, not an error
        self.assertIn("clicks", str(cm.exception))
        self.assertIn("1 Desk object", str(cm.exception))



class ProtectionRequestTest(RequestShapeTest):
    def test_protect(self):
        excel.Worksheet(2).Protect("pw", AllowSorting=True, UserInterfaceOnly=True)
        m, p = self.sent[-1]
        self.assertEqual(m, "protect.sheet")
        self.assertEqual(p["password"], "pw")
        self.assertTrue(p["ui_only"])
        self.assertTrue(p["flags"]["sort"])
        self.assertFalse(p["flags"]["objects"])
        excel.Worksheet(2).Protect()  # no password: keeps the old one
        self.assertNotIn("password", self.sent[-1][1])
        self.reply = {"ranges": []}
        excel.Worksheet(2).Protection.AllowEditRanges.Add("A", excel.Range._from(2, [(0, 0, 1, 1)]), "k")
        self.assertEqual(self.sent[-1], ("protect.ranges", {"sheet": 2, "ranges": [["A", 0, 0, 1, 1, "k"]]}))

    def test_filters_and_icons(self):
        from yc import rules

        self.reply = {"on": True}
        rules.range_autofilter(excel.Range._from(1, [(0, 0, 5, 2)]), 2, 0x00FFFF, excel.xlFilterCellColor)
        self.assertEqual(self.sent[-1][1]["kind"], 4)
        self.assertEqual(self.sent[-1][1]["color"], 0xFFFFFF00)
        rules.range_autofilter(excel.Range._from(1, [(0, 0, 5, 2)]), 1, excel.xlFilterThisMonth, excel.xlFilterDynamic)
        self.assertEqual(self.sent[-1][1]["dynamic"], 7)
        self.reply = {"id": 3}
        fc = rules.FormatConditions(excel.Range._from(1, [(0, 0, 9, 0)])).AddIconSetCondition()
        self.assertEqual(self.sent[-1][1]["rule"]["type"], 6)
        fc.IconSet = excel.ThisWorkbook.IconSets(excel.xl5Arrows)
        self.assertEqual(self.sent[-1][1]["props"], {"icon_style": 14})


class TableRequestTest(RequestShapeTest):
    def test_list_objects(self):
        self.reply = {"id": 7, "name": "Table1"}
        lo = excel.Worksheet(1).ListObjects.Add(Source=excel.Range._from(1, [(0, 0, 4, 2)]), XlListObjectHasHeaders=excel.xlYes)
        # no TableStyle given: YCGrid uses the workbook's default table style (Table Design > Set As Default)
        self.assertEqual(self.sent[-1], ("table.add", {"sheet": 1, "rect": [0, 0, 4, 2], "headers": True}))
        lo.ShowTotals = True
        self.assertEqual(self.sent[-1], ("table.update", {"sheet": 1, "id": 7, "props": {"totals": True}}))
        self.reply = [{"id": 7, "name": "T", "range": [0, 0, 5, 2], "header": True, "totals": True, "columns": [["A", 0, ""], ["B", 0, ""], ["C", 109, ""]]}]
        self.assertEqual(lo.DataBodyRange._a, (1, 0, 4, 2))
        self.assertEqual(lo.ListColumns("c").TotalsCalculation, excel.xlTotalsCalculationSum)
        lo.ListColumns(2).TotalsCalculation = excel.xlTotalsCalculationAverage
        self.assertEqual(self.sent[-1][1]["func"], 101)
        lo.ListRows.Add()
        self.assertEqual(self.sent[-2], ("table.row", {"sheet": 1, "id": 7, "add": True, "pos": -1}))


class ShapeRequestTest(RequestShapeTest):
    def setUp(self):
        super().setUp()
        from yc import ui

        self._orig_ui = ui._rq
        ui._rq = excel._rq

    def tearDown(self):
        from yc import ui

        ui._rq = self._orig_ui
        super().tearDown()

    def test_shapes(self):
        from yc import ui

        self.reply = {"name": "Oval 3"}
        sh = ui.Shapes(1).AddShape(excel.msoShapeOval, 10, 20, 100, 50)
        self.assertEqual(self.sent[-1], ("shape.add", {"sheet": 1, "kind": 3, "auto_type": 9, "left": 10.0, "top": 20.0, "width": 100.0, "height": 50.0}))
        sh.Fill.ForeColor.RGB = 0x0000FF  # red in Excel's R + G*256 + B*65536
        self.assertEqual(self.sent[-1], ("shape.set", {"sheet": 1, "name": "Oval 3", "props": {"fill_color": 0xFFFF0000}}))
        sh.TextFrame2.TextRange.Text = "Hi"
        self.assertEqual(self.sent[-1][1]["props"], {"text": "Hi"})
        sh.TextFrame.HorizontalAlignment = excel.xlRight
        self.assertEqual(self.sent[-1][1]["props"], {"halign": 2})
        self.reply = {"kind": 3, "fill_color": 0xFF00FF00, "line_weight": 2.0}
        self.assertEqual(sh.AutoShapeType, excel.msoShapeOval)
        self.assertEqual(sh.Type, excel.msoAutoShape)
        self.assertEqual(sh.Fill.ForeColor.RGB, 0x00FF00)
        self.assertEqual(sh.Line.Weight, 2.0)
        self.reply = {"name": "Straight Connector 4"}
        ui.Shapes(1).AddLine(100, 50, 20, 80)
        self.assertEqual(self.sent[-1][1], {"sheet": 1, "kind": 5, "left": 20.0, "top": 50.0, "width": 80.0, "height": 30.0, "flip_h": True, "flip_v": False})
        self.reply = {"name": "TextBox 5"}
        ui.Shapes(1).AddTextbox(excel.msoTextOrientationHorizontal, 0, 0, 80, 20)
        self.assertEqual(self.sent[-1][1]["kind"], 8)

    def test_sparklines(self):
        self.reply = {"id": 9}
        rng = excel.Range._from(1, [(0, 4, 2, 4)])
        g = rng.SparklineGroups.Add(excel.xlSparkColumn, "A1:D3")
        self.assertEqual(self.sent[-1], ("spark.add", {"sheet": 1, "location": "E1:E3", "source": "A1:D3", "type": 1}))
        g.Points.Highpoint.Visible = True
        self.assertEqual(self.sent[-1], ("spark.set", {"sheet": 1, "id": 9, "props": {"high": True}}))
        g.SeriesColor.Color = 0xFF0000  # blue
        self.assertEqual(self.sent[-1][1]["props"], {"color": 0xFF0000FF})
        self.reply = [{"id": 9, "type": 1, "items": [{"location": "E1", "source": "'S'!A1:D1"}, {"location": "E2", "source": "'S'!A2:D2"}]}]
        self.assertEqual(g.Type, excel.xlSparkColumn)
        self.assertEqual(g.Location._a, (0, 4, 1, 4))
        self.assertEqual(rng.SparklineGroups.Count, 1)
        rng.SparklineGroups.Clear()
        self.assertEqual(self.sent[-1], ("spark.clear", {"sheet": 1, "range": "E1:E3"}))


class ChartRequestTest(RequestShapeTest):
    def setUp(self):
        super().setUp()
        from yc import ui

        self._orig_ui = ui._rq
        ui._rq = excel._rq

    def tearDown(self):
        from yc import ui

        ui._rq = self._orig_ui
        super().tearDown()

    def test_series_formatting(self):
        from yc import ui

        ch = ui.Chart(1, "Chart 1")
        s = ui.Series(ch, 0)
        self.reply = {"type": 2, "series": [{"type": -1, "trend": 0, "err": 0, "totals": []}]}
        s.ChartType = excel.xlLine
        self.assertEqual(self.sent[-1], ("chart.series_set", {"sheet": 1, "name": "Chart 1", "index": 0, "props": {"type": 0}}))
        s.ApplyDataLabels()
        self.assertEqual(self.sent[-1][1]["props"], {"data_labels": True})
        s.Trendlines().Add(Type=excel.xlPolynomial, Order=3, Forward=2, DisplayEquation=True)
        self.assertEqual(self.sent[-1][1]["props"], {"trend": 4, "trend_forward": 2.0, "trend_backward": 0.0, "trend_eq": True, "trend_r2": False,
                                                     "trend_order": 3})
        s.ErrorBar(excel.xlY, excel.xlErrorBarIncludeBoth, excel.xlErrorBarTypeStDev, 2)
        self.assertEqual(self.sent[-1][1]["props"], {"err": 3, "err_amount": 2.0, "err_include": 1})
        s.Points(4).IsTotal = True
        self.assertEqual(self.sent[-1][1]["props"], {"totals": [3]})
        ch.ChartGroups(1).BinWidthValue = 5
        self.assertEqual(self.sent[-1], ("chart.set", {"sheet": 1, "name": "Chart 1", "props": {"bin_width": 5.0, "bin_count": 0}}))
        ch.ChartType = excel.xlTreemap
        self.assertEqual(self.sent[-1][1]["props"], {"type": 17})
        self.reply = {"type": 17, "series": [{"type": -1, "trend": 1, "trend_order": 2, "err": 0}]}
        with self.assertRaises(excel.UnsupportedError):
            s.ChartType = excel.xlLine  # treemaps don't combine
        self.assertEqual(ch.ChartType, excel.xlTreemap)
        self.assertEqual(s.Trendlines(1).Type, excel.xlLinear)
        s.ErrorBar(excel.xlY, excel.xlErrorBarIncludePlusValues, excel.xlErrorBarTypeCustom, [1, 2], 0.5)
        self.assertEqual(self.sent[-1][1]["props"], {"err": 5, "err_plus": "{1.0,2.0}", "err_minus": "0.5", "err_include": 2})


class CommentRequestTest(RequestShapeTest):
    def test_threaded_comments(self):
        excel.Application.UserName = "Ann"
        rng = excel.Range._from(1, [(1, 1, 1, 1)])
        t = rng.AddCommentThreaded("Check this")
        self.assertEqual(self.sent[-1], ("comment.set", {"sheet": 1, "row": 1, "col": 1, "text": "Check this", "author": "Ann"}))
        self.reply = {"threaded": True, "author": "Ann", "text": "Check this", "date": 0, "resolved": False,
                      "replies": [{"author": "Bob", "text": "ok", "date": 0}]}
        t.AddReply("Done")
        self.assertEqual(self.sent[-2], ("comment.reply", {"sheet": 1, "row": 1, "col": 1, "text": "Done", "author": "Ann"}))
        self.assertEqual(t.Replies.Count, 1)
        self.assertEqual(t.Replies(1).Author.Name, "Bob")
        t.Replies(1).Delete()
        self.assertEqual(self.sent[-1], ("comment.reply_delete", {"sheet": 1, "row": 1, "col": 1, "index": 0}))
        t.Resolved = True
        self.assertEqual(self.sent[-1], ("comment.resolve", {"sheet": 1, "row": 1, "col": 1, "resolved": True}))
        self.assertIsNone(rng.Comment)  # a thread is not a note
        self.assertIsNotNone(rng.CommentThreaded)
        t.Delete()
        self.assertEqual(self.sent[-1], ("comment.set", {"sheet": 1, "row": 1, "col": 1, "text": ""}))

    def test_notes_collection(self):
        self.reply = [{"row": 0, "col": 0, "text": "a", "author": "Ann"}, {"row": 2, "col": 1, "text": "b", "author": "Bob"},
                      {"row": 3, "col": 3, "text": "t", "threaded": True}]
        notes = excel.Worksheet(1).Comments   # Excel's collection: Count, 1-based Item / call, For Each
        self.assertEqual(notes.Count, 2)
        self.assertEqual(notes(2).Text(), "b")
        self.assertEqual([n.Text() for n in notes], ["a", "b"])
        with self.assertRaises(excel.SubscriptOutOfRangeError):
            notes(3)


class SpellingRequestTest(RequestShapeTest):
    def test_check_spelling(self):
        self.reply = {"ok": False, "suggestions": ["receive"]}
        self.assertFalse(excel.Application.CheckSpelling("recieve"))
        self.assertEqual(self.sent[-1], ("spell.check", {"word": "recieve", "lang": "en-US"}))
        self.assertEqual(excel.Application.GetSpellingSuggestions("recieve"), ["receive"])
        self.assertTrue(excel.Application.CheckSpelling("NASA", IgnoreUppercase=True))


class WorkbookPasswordRequestTest(RequestShapeTest):
    def test_password(self):
        excel.ThisWorkbook.SaveAs("C:/x/book.ycg", Password="pw")
        self.assertEqual(self.sent[-1], ("wb.save", {"path": "C:/x/book.ycg", "password": "pw"}))
        excel.ThisWorkbook.Password = ""
        self.assertEqual(self.sent[-1], ("wb.password", {"password": ""}))
        orig = excel._other_instances
        excel._other_instances = lambda: []  # no other YCGrid windows
        try:
            self.reply = {"path": "", "here": True}
            excel.Workbooks.Open("C:/x/book.ycg", Password="pw")
        finally:
            excel._other_instances = orig
        import os

        self.assertEqual(self.sent[-1], ("wb.open_window", {"path": os.path.abspath("C:/x/book.ycg"), "password": "pw"}))
        self.reply = True
        self.assertEqual(excel.ThisWorkbook.Password, "********")
        with self.assertRaises(excel.UnsupportedError):
            excel.ThisWorkbook.SaveAs("C:/x/b.ycg", WriteResPassword="w")


class WindowViewRequestTest(RequestShapeTest):
    def test_view(self):
        w = excel.Window(3)
        w.View = excel.xlPageBreakPreview
        self.assertEqual(self.sent[-1], ("view.set", {"sheet": 3, "page_view": 1}))
        self.reply = {"page_view": 2}
        self.assertEqual(w.View, excel.xlPageLayoutView)
        with self.assertRaises(ValueError):
            w.View = 7


class FormControlRequestTest(ShapeRequestTest):
    def test_form_controls(self):
        from yc import ui

        sheet = excel.Worksheet(1)
        orig = ui._list
        ui._list = lambda sid: {"controls": [], "charts": [], "shapes": []}
        try:
            self.reply = {"name": "Drop Down 1"}
            dd = sheet.Shapes.AddFormControl(excel.xlDropDown, 10, 20, 100, 18)
            m, p = self.sent[-1]
            self.assertEqual(m, "ctl.add")
            self.assertEqual((p["kind"], p["form"], p["name"]), ("combobox", True, "Drop Down 1"))
            dd.ControlFormat.ListIndex = 2
            self.assertEqual(self.sent[-1], ("ctl.set", {"sheet": 1, "name": "Drop Down 1", "props": {"list_index": 1}}))
            dd.OnAction = "Module1.Pick"
            self.assertEqual(self.sent[-1][1]["props"], {"on_action": "Module1.Pick"})
            self.reply = {"kind": "checkbox", "value": "True", "list_index": -1, "min": 0}
            self.assertEqual(dd.Value, excel.xlOn)
            with self.assertRaises(excel.UnsupportedError):
                sheet.Shapes.AddFormControl(excel.xlEditBox, 0, 0, 10, 10)
        finally:
            ui._list = orig


class UserFormRequestTest(RequestShapeTest):
    def test_userform(self):
        from yc import forms

        forms._rq = excel._rq

        class F(excel.UserForm):
            def UserForm_Initialize(self):
                self.Caption = "Order"
                t = self.Controls.Add("Forms.TextBox.1", "txtQty")
                t.Left = 80
                self.Controls.Add("Forms.CheckBox.1")
                self.Controls.Add("Forms.OptionButton.1", "a").Value = True
                self.Controls.Add("Forms.OptionButton.1", "b")

        f = F()
        self.assertEqual([c.Name for c in f.Controls], ["txtQty", "CheckBox1", "a", "b"])
        self.assertEqual(f.txtQty.Left, 80.0)
        self.assertTrue(f.a.Value)
        f.b.Value = True
        self.assertFalse(f.a.Value)
        self.sent.clear()
        f.Show(excel.vbModeless)
        m, p = self.sent[0]
        self.assertEqual(m, "panel.set")
        self.assertTrue(p["panel"]["form"])
        self.assertFalse(p["panel"]["modal"])
        self.assertEqual([c["type"] for c in p["panel"]["controls"]], ["textbox", "checkbox", "optionbutton", "optionbutton"])
        with self.assertRaises(excel.UnsupportedError):
            f.Controls.Add("Forms.Image.1")
        self.reply = []
        excel.Unload(f)
        self.assertEqual(self.sent[-1][0], "panel.delete")
        self.assertFalse(f.Visible)


class TimelineRequestTest(RequestShapeTest):
    def test_timeline_and_calc_items(self):
        import datetime as dt

        sc = excel.SlicerCache("PT1", 0, True)
        self.assertEqual(sc.SlicerCacheType, excel.xlTimeline)
        orig_ws = excel.Worksheets
        excel.Worksheets = [excel.Worksheet(1)]
        self.replies = {"slicer.list": []}  # no timeline on the sheet: the cache filters the PivotTable's field itself
        self.addCleanup(setattr, excel, "Worksheets", orig_ws)
        sc.TimelineState.SetFilterDateRange(dt.date(2024, 3, 1), dt.date(2024, 4, 30))
        m, p = self.sent[-1]
        self.assertEqual(m, "timeline.filter")
        self.assertEqual((p["pivot"], p["field"], p["from"], p["to"]), ("PT1", 0, 45352.0, 45412.0))
        sc.ClearDateFilter()
        self.assertEqual(self.sent[-1][1]["from"], 1)
        self.assertEqual(self.sent[-1][1]["to"], 0)


class SaveAsFormatTest(RequestShapeTest):
    def test_saveas_xlsx(self):
        excel.ThisWorkbook.SaveAs("C:/x/report", FileFormat=excel.xlOpenXMLWorkbook)
        self.assertEqual(self.sent[-1], ("wb.save", {"path": "C:/x/report.xlsx"}))
        excel.ThisWorkbook.SaveAs("C:/x/data.csv", FileFormat=excel.xlCSV)
        self.assertEqual(self.sent[-1], ("wb.save", {"path": "C:/x/data.csv"}))


class PrintRequestTest(RequestShapeTest):
    def test_page_setup(self):
        ps = excel.Worksheet(2).PageSetup
        ps.Orientation = excel.xlLandscape
        self.assertEqual(self.sent[-1], ("page.set", {"sheet": 2, "props": {"landscape": True}}))
        ps.Zoom = False
        self.assertEqual(self.sent[-1][1]["props"], {"zoom": 0})
        ps.FitToPagesTall = False
        self.assertEqual(self.sent[-1][1]["props"], {"fit_tall": 0})
        ps.PrintArea = "$A$1:$D$20"
        self.assertEqual(self.sent[-1][1]["props"], {"print_area": "$A$1:$D$20"})
        ps.PrintTitleRows = ""
        self.assertEqual(self.sent[-1][1]["props"], {"title_rows": ""})
        ps.LeftMargin = excel.Application.InchesToPoints(0.5)
        self.assertEqual(self.sent[-1][1]["props"], {"left": 36.0})
        ps.FirstPageNumber = excel.xlAutomatic
        self.assertEqual(self.sent[-1][1]["props"], {"first_page_number": 0})
        with self.assertRaises(AttributeError):
            ps.NoSuchProperty = 1
        self.reply = {"landscape": False, "zoom": 0, "fit_wide": 1, "fit_tall": 0, "over_then_down": True, "first_page_number": 0}
        self.assertEqual(ps.Orientation, excel.xlPortrait)
        self.assertIs(ps.Zoom, False)
        self.assertIs(ps.FitToPagesTall, False)
        self.assertEqual(ps.FitToPagesWide, 1)
        self.assertEqual(ps.Order, excel.xlOverThenDown)
        self.assertEqual(ps.FirstPageNumber, excel.xlAutomatic)

    def test_breaks_and_pdf(self):
        ws = excel.Worksheet(2)
        self.reply = [[48, False], [60, True]]
        self.assertEqual(ws.HPageBreaks.Count, 2)
        self.assertEqual(ws.HPageBreaks(2).Type, excel.xlPageBreakManual)
        self.assertEqual(ws.HPageBreaks(1).Location._a[0], 48)
        ws.VPageBreaks.Add(excel.Range._from(2, [(0, 5, 0, 5)]))
        self.assertEqual(self.sent[-1], ("page.breaks", {"sheet": 2, "rows": False, "add": 5}))
        ws.ResetAllPageBreaks()
        self.assertEqual(self.sent[-1], ("page.breaks", {"sheet": 2, "rows": True, "reset": True}))
        self.reply = True
        path = ws.ExportAsFixedFormat(excel.xlTypePDF, "out")
        m, p = self.sent[-1]
        self.assertEqual(m, "print.pdf")
        self.assertTrue(p["path"].endswith("out.pdf"))
        self.assertEqual(p["sheets"], [2])
        self.assertEqual(path, p["path"])
        excel.Range._from(2, [(0, 0, 9, 3)]).ExportAsFixedFormat(excel.xlTypePDF, "r.pdf")
        self.assertEqual(self.sent[-1][1]["area"], [0, 0, 9, 3])
        with self.assertRaises(excel.UnsupportedError):
            ws.ExportAsFixedFormat(excel.xlTypeXPS, "x")

    def test_workbook_properties(self):
        self.assertTrue(hasattr(excel.Workbook(), "CustomDocumentProperties"))


class ControlRequestTest(RequestShapeTest):
    def setUp(self):
        super().setUp()
        from yc import ui

        self._orig_ui = ui._rq
        ui._rq = excel._rq

    def tearDown(self):
        from yc import ui

        ui._rq = self._orig_ui
        super().tearDown()

    def test_new_controls(self):
        from yc import ui

        self.reply = {"name": "CheckBox1"}
        cb = ui.OLEObjects(1).Add("Forms.CheckBox.1", Left=10, Top=20, LinkedCell="Sheet1!$A$1", Value=True)
        m, p = self.sent[-1]
        self.assertEqual(m, "ctl.add")
        self.assertEqual((p["kind"], p["width"], p["height"], p["value"], p["linked_cell"]), ("checkbox", 108.0, 18.0, "True", "Sheet1!$A$1"))
        self.reply = {"kind": "checkbox", "linked_cell": "", "value": "True"}
        self.assertIs(cb.Object.Value, True)
        cb.Value = False
        self.assertEqual(self.sent[-1][1]["props"], {"value": "False"})
        self.reply = {"name": "SpinButton1"}
        sp = ui.OLEObjects(1).Add("Forms.SpinButton.1", Min=1, Max=10, SmallChange=2)
        p = self.sent[-1][1]
        self.assertEqual((p["kind"], p["min"], p["max"], p["small_change"]), ("spinbutton", 1, 10, 2))
        self.reply = {"kind": "spinbutton", "linked_cell": "", "value": "7", "min": 1}
        self.assertEqual(sp.Value, 7)
        self.reply = {"name": "OptionButton1"}
        ui.OLEObjects(1).Add("OptionButton", GroupName="side")
        self.assertEqual(self.sent[-1][1]["group_name"], "side")
        self.reply = {"name": "ListBox1"}
        lb = ui.OLEObjects(1).Add("Forms.ListBox.1", List=["a", "b"], MultiSelect=excel.fmMultiSelectMulti)
        self.assertEqual(self.sent[-1][1]["multi_select"], 1)
        self.reply = {"selected": [1]}
        self.assertTrue(lb.Selected(1))
        lb.Selected[0] = True
        self.assertEqual(self.sent[-1][1]["props"], {"selected": [0, 1]})
        self.reply = {"name": "Image1"}
        ui.OLEObjects(1).Add("Forms.Image.1", Picture=b"\x89PNG")
        self.assertEqual(self.sent[-1][1]["picture"], b"\x89PNG")
        with self.assertRaises(excel.UnsupportedError):
            ui.OLEObjects(1).Add("Forms.Frame.1")


class RibbonRequestTest(RequestShapeTest):
    def setUp(self):
        super().setUp()
        from yc import ui

        self._orig_ui = ui._rq
        ui._rq = excel._rq

    def tearDown(self):
        from yc import ui

        ui._rq = self._orig_ui
        super().tearDown()

    def test_ribbon_ui(self):
        from yc import ui

        r = ui.IRibbonUI()
        r.Invalidate()
        self.assertEqual(self.sent[-1], ("ribbon.invalidate", {"control": ""}))
        r.InvalidateControl("btnStart")
        self.assertEqual(self.sent[-1], ("ribbon.invalidate", {"control": "btnStart"}))
        self.reply = True
        self.assertTrue(r.ActivateTabMso("TabHome"))
        self.assertEqual(self.sent[-1], ("ribbon.activate", {"id": "TabHome"}))
        c = ui.IRibbonControl("btnStart", "t1")
        self.assertEqual((c.Id, c.Tag), ("btnStart", "t1"))

    def test_host_ribbon_values(self):
        from yc import host

        self.assertEqual(host._ribbon_text(True), "1")
        self.assertEqual(host._ribbon_text(False), "0")
        self.assertEqual(host._ribbon_text(None), "")
        self.assertEqual(host._ribbon_text(3), "3")


class PivotRequestTest(RequestShapeTest):
    def _def(self):
        return {"sheet": 1, "name": "PT", "fields": ["Region", "Product", "Sales"], "rows": [], "cols": [], "pages": [], "data": [],
                "captions": [], "output": [0, 5, 4, 8], "page_rows": 0, "header_rows": 2, "label_cols": 1, "r0": 0, "c0": 5,
                "source": "'S'!$A$1:$C$6", "layout": 0, "subtotals": 1, "col_grand": True, "row_grand": True, "style": "PivotStyleLight16"}

    def test_fields_and_layout(self):
        import copy

        base = self._def()
        self.reply = copy.deepcopy(base)
        pt = excel.PivotTable(1, "PT")
        pt.PivotFields("Region").Orientation = excel.xlRowField
        m, p = self.sent[-1]
        self.assertEqual(m, "pivot.set")
        self.assertEqual(p["def"]["rows"][0]["field"], 0)
        pt.PivotFields("Product").Orientation = excel.xlColumnField
        self.assertEqual(self.sent[-1][1]["def"]["cols"][0]["field"], 1)
        df = pt.AddDataField(pt.PivotFields("Sales"), "Total", excel.xlAverage)
        data = self.sent[-1][1]["def"]["data"]
        self.assertEqual(data[-1], {"field": 2, "func": 2, "show_as": 0, "caption": "Total", "number_format": ""})
        self.reply["captions"] = ["Total"]
        self.assertEqual(df.Function, excel.xlAverage)
        df.Calculation = excel.xlPercentOfTotal
        self.assertEqual(self.sent[-1][1]["def"]["data"][-1]["show_as"], 1)
        self.assertEqual(pt.RowFields.Count, 1)
        self.assertEqual(pt.PivotFields("Product").Orientation, excel.xlColumnField)
        self.assertEqual(pt.DataBodyRange._a, (2, 6, 4, 8))
        self.assertEqual(pt.TableRange1._a, (0, 5, 4, 8))
        pt.RowAxisLayout(excel.xlTabularRow)
        self.assertEqual(self.sent[-1][1]["def"]["layout"], 2)
        self.assertEqual(self.sent[-1][1]["def"]["subtotals"], 2)
        pt.RowGrand = False
        self.assertFalse(self.sent[-1][1]["def"]["col_grand"])
        pt.PivotFields("Region").Orientation = excel.xlPageField
        d = self.sent[-1][1]["def"]
        self.assertEqual((len(d["rows"]), d["pages"][0]["field"]), (0, 0))
        pt.PivotFields("Region").CurrentPage = "East"
        self.assertEqual(self.sent[-1][1]["def"]["pages"][0]["page_item"], "East")
        pt.RefreshTable()
        self.assertEqual(self.sent[-1], ("pivot.refresh", {"sheet": 1, "name": "PT", "alerts": True}))

    def test_caches(self):
        self.reply = {"sheet": 1, "name": "PivotTable1"}
        pc = excel.Workbook().PivotCaches().Create(excel.xlDatabase, "Data!A1:C6")
        orig = excel._active_sheet_id
        excel._active_sheet_id = lambda: 1
        try:
            pt = pc.CreatePivotTable("F3", "PT1")
        finally:
            excel._active_sheet_id = orig
        self.assertEqual(self.sent[-1][0], "pivot.add")
        self.assertEqual(self.sent[-1][1]["source"], "Data!A1:C6")
        self.assertEqual(self.sent[-1][1]["dest"], "F3")
        self.assertEqual(self.sent[-1][1]["name"], "PT1")
        self.assertEqual(pt.Name, "PivotTable1")


class PivotExtrasRequestTest(RequestShapeTest):
    def test_calc_group_slicer(self):
        import copy

        base = {"sheet": 1, "name": "PT", "fields": ["Date", "Region", "Sales", "Qty"], "rows": [], "cols": [], "pages": [], "data": [],
                "captions": [], "output": None, "page_rows": 0, "header_rows": 1, "label_cols": 1, "r0": 0, "c0": 5, "source": "S!A1:D9",
                "layout": 0, "subtotals": 1, "col_grand": True, "row_grand": True, "style": "x", "groups": [], "calc_fields": [],
                "slicer_filters": [{"field": 1, "hidden": ["East"]}]}
        self.reply = copy.deepcopy(base)
        pt = excel.PivotTable(1, "PT")
        pt.CalculatedFields().Add("Unit", "=Sales/Qty")
        self.assertEqual(self.sent[-1][1]["def"]["calc_fields"], [{"name": "Unit", "formula": "=Sales/Qty"}])
        orig = excel.PivotField._is_date_field
        excel.PivotField._is_date_field = lambda self: True
        try:
            pt.PivotFields("Date").Group(By=excel.ycGroupYearMonths)
        finally:
            excel.PivotField._is_date_field = orig
        self.assertEqual(self.sent[-1][1]["def"]["groups"][-1]["by"], 4)
        pt.PivotFields("Date").Group(Periods=[False, False, False, False, False, False, True])
        self.assertEqual(self.sent[-1][1]["def"]["groups"][-1]["by"], 1)
        pt.PivotFields("Date").Ungroup()
        self.assertEqual(self.sent[-1][1]["def"]["groups"], [])
        # a slicer cache: selection through the hidden items of the field
        sc = excel.SlicerCache("PT", 1)
        orig_ws = excel.Worksheets
        excel.Worksheets = [excel.Worksheet(1)]
        self.replies = {"slicer.list": []}  # no slicer on the sheet: the cache filters the PivotTable's field itself
        try:
            self.reply = [copy.deepcopy(base)]
            self.assertEqual(sc._hidden(), ["East"])
            sc.ClearManualFilter()
            self.assertEqual(self.sent[-1], ("slicer.filter", {"pivot": "PT", "field": 1, "hidden": [], "alerts": True}))
        finally:
            excel.Worksheets = orig_ws


class StylesRequestTest(RequestShapeTest):
    def test_styles_and_theme(self):
        rng = excel.Range._from(1, [(0, 0, 1, 0)])
        rng.Style = "Good"
        self.assertEqual(self.sent[-1][0], "range.cellstyle")
        self.assertEqual(self.sent[-1][1]["name"], "Good")
        self.reply = "Mine"
        st = excel.Workbook().Styles.Add("Mine", BasedOn=excel.Range._from(2, [(3, 4, 3, 4)]))
        self.assertEqual(self.sent[-1], ("cellstyle.save", {"name": "Mine", "create": True, "based_sheet": 2, "based_row": 3, "based_col": 4}))
        st.NumberFormat = "0.0"
        self.assertEqual(self.sent[-1], ("cellstyle.save", {"name": "Mine", "props": {"numfmt": "0.0"}}))
        self.assertEqual(excel._theme_tag(excel.xlThemeColorAccent1, 0.5), 0xFE000000 | (4 << 16) | 5000)
        self.assertEqual(excel._theme_tag(excel.xlThemeColorDark1, -0.25) & 0xFFFF, (-2500) & 0xFFFF)
        self.reply = None
        excel.Workbook().ApplyTheme("C:/themes/Facet.thmx")
        self.assertEqual(self.sent[-1], ("theme.set", {"theme": "Facet"}))


class EditingRequestTest(RequestShapeTest):
    def test_fill_sort_lists(self):
        rng = excel.Range._from(1, [(0, 0, 0, 0)])
        rng.AutoFill(excel.Range._from(1, [(0, 0, 4, 0)]), excel.xlFillWeekdays)
        self.assertEqual(self.sent[-1][1]["type"], 6)
        excel.Range._from(1, [(0, 0, 9, 2)]).Sort(Key1=excel.Range._from(1, [(0, 1, 0, 1)]), Header=excel.xlYes, OrderCustom=3)
        self.assertEqual(self.sent[-1][1]["keys"], [[1, False, 1]])  # OrderCustom 3 is list 2 (1 is the normal order)
        f = excel.SortField(2, excel.xlDescending, "High, Low")
        self.assertEqual(f._key(), [2, True, -1, ["High", "Low"]])
        self.reply = {"lists": [["Sun", "Mon"], ["A", "B"]], "builtin": 1}
        self.assertEqual(excel.Application.GetCustomListNum(["a", "b"]), 2)
        excel.Application.AddCustomList(["x", "y"])
        self.assertEqual(self.sent[-1], ("lists.set", {"user": [["A", "B"], ["x", "y"]]}))
        with self.assertRaises(excel.ApplicationDefinedError):
            excel.Application.DeleteCustomList(1)


class CodeNameSheetTest(RequestShapeTest):
    """Sheet1 (a CodeName global) works like the Worksheet: Cells / Rows / Columns are its accessors."""

    def test_cells_rows_name(self):
        me = excel._SheetMe(3, "Sheet1")
        me.Cells.Clear()
        self.assertEqual(self.sent[-1][0], "range.clear")
        self.assertEqual(self.sent[-1][1]["sheet"], 3)
        self.assertEqual(me.Cells(2, 3).Address(), "$C$2")
        me.Rows(5).Insert()
        self.assertEqual(self.sent[-1][1]["sheet"], 3)
        me.Name = "Trade"
        self.assertIn("Trade", json.dumps(self.sent[-1]))
        Range, Cells, Rows, Columns = me.Range, me.Cells, me.Rows, me.Columns  # the sheet-module header
        self.assertEqual(Cells(1, 1).Address(), "$A$1")
        self.assertEqual(Range("B2").Address(), "$B$2")


class ErrorLogRequestTest(unittest.TestCase):
    def test_errors_requests(self):
        from yc import ui

        sent = []
        orig = ui._rq
        ui._rq = lambda method, **p: sent.append((method, p)) or ([{"id": 3, "error": True}] if method == "errors.list" else "C:/logs")
        try:
            self.assertEqual(ui.errors(errors_only=True, after=2), [{"id": 3, "error": True}])
            self.assertEqual(sent[-1], ("errors.list", {"errors_only": True, "after": 2}))
            ui.clear_errors()
            self.assertEqual(sent[-1], ("errors.clear", {}))
            self.assertEqual(ui.errors_folder(), "C:/logs")
            ui._rq = lambda method, **p: sent.append((method, p)) or 2
            self.assertEqual(ui.resolve_errors([4, "5"], note="fixed"), 2)
            self.assertEqual(sent[-1], ("errors.resolve", {"ids": [4, 5], "note": "fixed"}))
        finally:
            ui._rq = orig


class TestToolRequestTest(unittest.TestCase):
    """ui.items, ui.ribbon_items and ui.answer_file_dialogs: what tests use to click what a person clicks."""

    def test_requests(self):
        from yc import ui

        sent, answers = [], {"ui.items": [None, [{"label": "OK"}]], "ui.ribbon_items": [[{"cmd": "Bold"}]]}
        orig = ui._rq

        def fake(method, **p):
            sent.append((method, p))
            q = answers.get(method)
            return q.pop(0) if q else None

        ui._rq = fake
        try:
            self.assertEqual(ui.items("Find"), [{"label": "OK"}], "the first call turns the recording on, then lets a frame be drawn")
            self.assertEqual([m for m, _ in sent], ["ui.items", "ui.input", "ui.items"])
            self.assertEqual(sent[0][1], {"window": "Find"})
            self.assertEqual(ui.ribbon_items(), [{"cmd": "Bold"}])
            ui.answer_file_dialogs("a.ycg", "")
            m, p = sent[-1]
            self.assertEqual(m, "ui.answer_file_dialogs")
            self.assertEqual(p["paths"], [os.path.abspath("a.ycg"), ""], "paths made absolute; \"\" is Cancel")
        finally:
            ui._rq = orig


class MissingPackagesTest(unittest.TestCase):
    """A ModuleNotFoundError offers every missing package the failing code needs, so one install is enough."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        pkg = os.path.join(self.tmp.name, "ycmp_pkg")
        os.makedirs(pkg)
        files = {
            "__init__.py": "",
            "a.py": "import os\nimport yc_absent_one\nfrom . import b\ntry:\n    import yc_optional_x\nexcept ImportError:\n    pass\n",
            "b.py": "def f():\n    import yc_absent_two.sub\n",
        }
        for name, text in files.items():
            with open(os.path.join(pkg, name), "w", encoding="utf-8") as f:
                f.write(text)
        sys.path.insert(0, self.tmp.name)
        self.addCleanup(sys.path.remove, self.tmp.name)

    def failure(self, code):
        try:
            exec(code, {})
        except ModuleNotFoundError as e:
            return e
        self.fail("no ModuleNotFoundError")

    def test_every_missing_package_of_the_failure_at_once(self):
        from yc import env

        e = self.failure("import ycmp_pkg.a")
        self.assertEqual(env.missing_for_error(e, "import ycmp_pkg.a; import yc_absent_three", self.tmp.name),
                         ["yc-absent-one", "yc-absent-three", "yc-absent-two"],
                         "the failed import, the code's own imports and those of the module imported beside it; "
                         "not the optional (try/except ImportError) one, nor the project's own package")

    def test_a_missing_submodule_of_an_installed_package_is_not_a_package(self):
        from yc import env

        self.assertEqual(env.missing_for_error(self.failure("import yc.no_such_module"), "import yc.no_such_module"), [])

    def test_examples_need_numpy_and_pandas_only(self):
        """The example projects (examples/<name>, their shared code in examples/lib through [python] paths) need no
        package YCGrid doesn't bring: ex_util and the other shared modules are the project's own."""
        from yc import env

        root = os.path.join(os.path.dirname(os.path.abspath(env.__file__)), "..", "..", "examples")
        # what each needs before it runs: its own imports and those of the shared modules it imports (ex_math's numpy
        # for vol_surface), never the whole shared folder (option_payoff runs without numpy and isn't offered it)
        expect = {"vol_surface": {"numpy", "pandas"}, "backtest_report": {"numpy", "pandas"}, "quick_plot": {"numpy"},
                  "option_payoff": set(), "time_machine": set(), "txo_market_maker": {"pandas"},
                  "market_watch": {"numpy"}, "option_chain": {"numpy"}, "technical_chart": {"numpy", "pandas"},
                  "trading_desk": {"numpy", "pandas"}}
        for name, needs in expect.items():
            d = os.path.join(root, name)
            own = env._project_modules(d)
            self.assertIn("ex_util", own, name)
            mods = env.scan_imports(d)
            third = {m for m in mods if m not in sys.stdlib_module_names and not m.startswith("_") and m not in own and m != "yc"}
            self.assertEqual(third, needs, name)

    def test_a_handler_failing_on_a_missing_package_offers_it(self):
        """Workbook_Open (or any handler) that fails on a package nobody installed brings the package bar, once: the
        examples that reach numpy only through yc.trading / yc.sim showed the error and no way to install it."""
        from yc import host

        sent = []

        class Conn:
            def notify(self, method, params):
                sent.append((method, params))

        saved = host._conn_obj, host._app_dir, set(host._offered)
        host._conn_obj, host._app_dir = Conn(), tempfile.mkdtemp()
        host._offered.clear()
        try:
            def handler():
                import yc_test_absent_module_qq  # noqa: F401

            from unittest import mock

            with mock.patch.object(host, "_report"):
                host._call("Workbook_Open", handler)
                host._call("Worksheet_Change", handler)  # the next failure doesn't bring back a bar the person skipped
                host._call("Other", lambda: 1 / 0)  # other errors offer nothing
            offers = [p for m, p in sent if m == "env.missing"]
            self.assertEqual(len(offers), 1, sent)
            self.assertEqual(offers[0]["packages"], ["yc-test-absent-module-qq"])
            self.assertEqual(offers[0]["policy"], "ask")
        finally:
            host._conn_obj, host._app_dir = saved[0], saved[1]
            host._offered.clear()
            host._offered.update(saved[2])

    def test_a_failed_install_says_why(self):
        """env.install keeps the installer's reason for the package bar (it said only "Installation failed (code 1)",
        and the bar went away as if the packages were coming)."""
        import subprocess
        from unittest import mock

        from yc import env

        out = ("Using Python 3.12.15 environment at: runtime/python\n"
               "error: No solution found when resolving dependencies\n"
               "  cause: Because no-such-pkg was not found in the package registry, we can conclude ...\n")
        done = subprocess.CompletedProcess([], 1, stdout=out, stderr="")
        with tempfile.TemporaryDirectory() as d, mock.patch("subprocess.run", return_value=done), \
                mock.patch("sys.stdout"), mock.patch("sys.stderr"):
            self.assertFalse(env.install(d, ["no-such-pkg"]))
            self.assertFalse(os.path.exists(os.path.join(d, "requirements.txt")))
        self.assertTrue(env.last_error.startswith("error: No solution found"), env.last_error)
        self.assertIn("not found in the package registry", env.last_error)
        self.assertEqual(env.failure_reason("", 3), "the installer returned 3")
        self.assertEqual(env.failure_reason("a\nb\nc\nd", 1), "b c d")  # no error line: the last ones
        with tempfile.TemporaryDirectory() as d, mock.patch("subprocess.run", side_effect=OSError("no uv")), \
                mock.patch("sys.stdout"), mock.patch("sys.stderr"):
            self.assertFalse(env.install(d, ["x"]))
        self.assertIn("no uv", env.last_error)

    def test_install_messages_are_english(self):
        """ycgrid.exe --install / --uninstall (install.cmd, uninstall.cmd) and the moved-association question speak
        English on every Windows, as the scripts do: no text there goes through the UI translation."""
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "app", "shell_integration.cpp")
        with open(path, encoding="utf-8") as f:
            src = f.read()
        code = "\n".join(line.split("//")[0] for line in src.splitlines())
        self.assertNotRegex(code, r"\bTRW?\(")
        self.assertIn("Done: double-clicking a .ycg file opens it in YCGrid.", src)
        self.assertIn("Removed YCGrid's file association", src)


class UiCustomRequestTest(RequestShapeTest):
    def test_commandbars_and_dialogs(self):
        self.reply = 7
        c = excel.Application.CommandBars("Cell").Controls.Add(Type=excel.msoControlButton, Before=2)
        self.assertEqual(self.sent[-1], ("cmdbar.add", {"bar": "Cell", "parent": 0, "type": 1, "parameter": "", "before": 2}))
        self.reply = None
        c.OnAction = "Module1.go"
        self.assertEqual(self.sent[-1], ("cmdbar.set", {"bar": "Cell", "id": 7, "on_action": "Module1.go"}))
        c.BeginGroup = 1
        self.assertEqual(self.sent[-1][1]["begin_group"], True)
        self.reply = False  # ui.dialog_open -> closed at once
        self.assertTrue(excel.Application.Dialogs(excel.xlDialogFormatNumber).Show())
        self.assertIn(("ui.dialog_show", {"dialog": 42}), self.sent)


class LinksRequestTest(RequestShapeTest):
    def test_link_methods(self):
        self.reply = []
        self.assertIsNone(excel.ThisWorkbook.LinkSources())
        self.reply = [{"path": r"C:\data\Book2.ycg", "updated": 0, "error": "not found"}]
        self.assertEqual(excel.ThisWorkbook.LinkSources(excel.xlExcelLinks), (r"C:\data\Book2.ycg",))
        info = excel.ThisWorkbook.LinkInfo("book2.ycg")
        self.assertEqual((info["updated"], info["error"]), (None, "not found"))
        with self.assertRaises(excel.SubscriptOutOfRangeError):
            excel.ThisWorkbook.LinkInfo("Other.ycg")
        self.reply = None
        excel.ThisWorkbook.UpdateLink()
        self.assertEqual(self.sent[-1], ("links.update", {}))
        excel.ThisWorkbook.UpdateLink(["a.ycg", "b.ycg"])
        self.assertEqual(self.sent[-2:], [("links.update", {"name": "a.ycg"}), ("links.update", {"name": "b.ycg"})])
        excel.ThisWorkbook.BreakLink("a.ycg")
        self.assertEqual(self.sent[-1], ("links.break", {"name": "a.ycg"}))


class CoauthorRequestTest(RequestShapeTest):
    def test_coauthoring_members(self):
        wb = excel.ThisWorkbook
        self.assertFalse(wb.CoAuthoring)
        self.assertFalse(wb.MultiUserEditing)
        self.assertEqual(wb.UserStatus[0][2], 1)  # one local user, without asking YCGrid
        wb.ExclusiveAccess()
        self.assertFalse(any(m.startswith("coauthor") for m, _ in self.sent))

class CellsAccessorTest(RequestShapeTest):
    def test_private_names_are_not_forwarded(self):
        # the workbook binding looks up _cx on every object: Cells must not turn that into a range (it recursed)
        cells = excel.Worksheet(1).Cells
        self.assertFalse(hasattr(cells, "_cx_missing"))
        r = excel.Worksheet(1).Cells(2, 3)
        self.assertEqual(r._a, (1, 2, 1, 2))


class ChartElementsTest(RequestShapeTest):
    def setUp(self):
        super().setUp()
        from yc import ui

        self._orig_ui = ui._rq
        self.chart = {"name": "Chart 1", "legend_pos": 0, "title_size": 0.0, "title_color": 0, "title_bold": False, "chart_fill": 0,
                      "plot_fill": 0, "chart_line": 0, "chart_line_none": False, "val_has_title": False, "val_title": "", "val_format": "",
                      "val_grid": True, "val_visible": True, "cat_grid": False, "cat_visible": True,
                      "series": [{"marker": -1, "marker_size": 0.0, "line_weight": 0.0, "color": 0}]}

        def fake(method, **params):
            self.sent.append((method, params))
            if method == "chart.get":
                return self.chart
            if method == "chart.move":
                return {"sheet": 9, "name": "Chart 1"}
            if method == "obj.list":
                return {"charts": [{"name": "Chart 1"}], "controls": []}
            return None

        ui._rq = fake

    def tearDown(self):
        from yc import ui

        ui._rq = self._orig_ui
        super().tearDown()

    def test_elements(self):
        from yc import ui

        c = ui.Chart(1, "Chart 1")
        self.assertEqual(c.Legend.Position, excel.xlLegendPositionBottom)
        c.Legend.Position = excel.xlLegendPositionCorner
        self.assertEqual(self.sent[-1][1]["props"], {"legend_pos": 4, "has_legend": True})
        self.assertEqual(c.ChartTitle.Font.Size, 14)  # automatic: Excel's default
        c.ChartTitle.Font.Color = excel.RGB(255, 0, 0)
        self.assertEqual(self.sent[-1][1]["props"], {"title_color": 0xFFFF0000})
        self.assertEqual(c.ChartArea.Format.Fill.ForeColor.RGB, 0xFFFFFF)
        c.PlotArea.Interior.Color = excel.RGB(0, 0, 255)
        self.assertEqual(self.sent[-1][1]["props"], {"plot_fill": 0xFF0000FF})
        c.Axes(excel.xlValue).TickLabels.NumberFormat = "General"
        self.assertEqual(self.sent[-1][1]["props"], {"val_format": ""})
        c.Axes(excel.xlCategory).HasMajorGridlines = True
        self.assertEqual(self.sent[-1][1]["props"], {"cat_grid": True})
        c.Axes(excel.xlValue).HasTitle = True
        self.assertEqual(self.sent[-1][1]["props"], {"val_has_title": True, "val_title": "Axis Title"})
        s = c.SeriesCollection(1)
        self.assertEqual(s.MarkerStyle, excel.xlMarkerStyleAutomatic)
        s.MarkerStyle = excel.xlMarkerStyleDiamond
        self.assertEqual(self.sent[-1], ("chart.series_set", {"sheet": 1, "name": "Chart 1", "index": 0, "props": {"marker": 3}}))
        c.Legend.Select()
        self.assertEqual(self.sent[-1], ("chart.select", {"sheet": 1, "name": "Chart 1", "elem": 4, "series": 0, "format": False}))

    def test_3d_charts(self):
        from yc import ui

        self.chart.update({"type": 27, "rotation": 20.0, "elevation": 15.0, "perspective": 15.0, "depth_percent": 100.0})
        c = ui.Chart(1, "Chart 1")
        self.assertEqual(c.ChartType, excel.xlSurface)
        c.ChartType = excel.xl3DPie
        self.assertEqual(self.sent[-1][1]["props"], {"type": 26})
        c.ChartType = excel.xl3DColumnClustered
        self.assertEqual(self.sent[-1][1]["props"], {"type": 21})
        self.assertEqual(c.Elevation, 15.0)
        c.Rotation = 40
        self.assertEqual(self.sent[-1][1]["props"], {"rotation": 40.0})
        c.RightAngleAxes = True
        self.assertEqual(self.sent[-1][1]["props"], {"perspective": 0.0})
        c.ChartArea.ClearContents()
        self.assertIn(("chart.series_delete", {"sheet": 1, "name": "Chart 1", "index": 0}), self.sent)

    def test_location_and_collections(self):
        from yc import ui

        sheets = [{"id": 1, "name": "Sheet1", "chart": False, "index": 1, "visible": True},
                  {"id": 9, "name": "Chart1", "chart": True, "index": 2, "visible": True}]
        excel._rq = lambda method, **params: {"sheets": sheets} if method == "wb.info" else None
        try:
            self.assertEqual(excel.Worksheets.Count, 1)
            self.assertEqual(excel.Sheets.Count, 2)
            self.assertEqual(excel.Charts.Count, 1)
            cs = ui.Chart(1, "Chart 1").Location(excel.xlLocationAsNewSheet, "Sales")
            self.assertEqual(self.sent[-2], ("chart.move", {"sheet": 1, "name": "Chart 1", "to": 0, "new_name": "Sales"}))
            self.assertIsInstance(cs, excel.ChartSheet)
            self.assertEqual(cs.Name, "Chart1")
            self.assertEqual(cs.Type, excel.xlChart)
        finally:
            excel._rq = self._orig


class MultiWorkbookTest(RequestShapeTest):
    def test_binding(self):
        # objects of another window's workbook send their requests over that connection
        class FakeConn:
            closed = False

            def __init__(self):
                self.sent = []

            def request(self, method, params):
                self.sent.append((method, params))
                return {"values": [[42]]} if method == "range.get" else None

        cx = FakeConn()
        from yc import _conn

        r = excel.Range._from(1, [(0, 0, 0, 0)])
        object.__setattr__(r, "_cx", cx)
        orig = excel._rq
        excel._rq = lambda method, **params: _conn.current().request(method, params)
        try:
            self.assertEqual(r.Value, 42)
            self.assertEqual(cx.sent[-1][0], "range.get")
            off = r.Offset(1, 0)  # made while bound: belongs to the same workbook
            self.assertIs(off._cx, cx)
            self.assertIsNone(getattr(_conn._tls, "conn", None))
        finally:
            excel._rq = orig


class RemoteBindingTest(unittest.TestCase):
    """Objects of another window's workbook send their requests to that window (Cells, Rows and other helpers too)."""

    def test_cells_follow_the_sheet(self):
        from yc import _conn as conn_mod

        class Fake:
            closed = False

            def __init__(self, name):
                self.name, self.sent = name, []

            def request(self, method, params):
                self.sent.append(method)
                return {"values": [[1]]}

        main, other = Fake("main"), Fake("other")
        orig = excel._conn
        excel._conn = lambda: getattr(conn_mod._tls, "conn", None) or main
        try:
            conn_mod._tls.conn = other
            ws = excel.Worksheet(1)
            conn_mod._tls.conn = None
            ws.Cells(2, 3).Value
            ws.Cells.Item(1, 1).Value
            ws.Rows(2).Value
        finally:
            excel._conn = orig
            conn_mod._tls.conn = None
        self.assertEqual(main.sent, [])
        self.assertEqual(other.sent, ["range.get", "range.get", "range.get"])


class InteractiveScrollAreaTest(RequestShapeTest):
    def test_requests(self):
        self.reply = True
        excel.Application.Interactive = False
        self.assertEqual(self.sent[-1], ("ui.interactive", {"on": False}))
        self.assertTrue(excel.Application.Interactive)
        self.reply = "$A$1:$D$9"
        ws = excel.Worksheet(3)
        ws.ScrollArea = "A1:D9"
        self.assertEqual(self.sent[-1], ("sheet.scroll_area", {"sheet": 3, "area": "A1:D9"}))
        self.assertEqual(ws.ScrollArea, "$A$1:$D$9")
        ws.ScrollArea = ""
        self.assertEqual(self.sent[-1][1]["area"], "")


class SafetyRequestTest(RequestShapeTest):
    def test_requests(self):
        from yc import safety

        orig = safety._rq
        safety._rq = lambda method, **params: excel._rq(method, **params)
        try:
            self.reply = True
            safety.parameter(excel.Range._from(2, [(1, 1, 1, 1)]), name="Spread", type=float, min=0, max=5, max_step=0.5)
            self.assertEqual(self.sent[-1], ("safety.param_set", {"sheet": 2, "row": 1, "col": 1, "name": "Spread", "type": 1, "min": 0, "max": 5,
                                                                  "choices": [], "confirm": True, "max_step": 0.5}))
            safety.parameter(excel.Range._from(2, [(2, 1, 2, 1)]), choices=["Buy", "Sell"], confirm=False)
            self.assertEqual(self.sent[-1][1]["type"], 5)
            self.assertEqual(self.sent[-1][1]["choices"], ["Buy", "Sell"])
            self.reply = {"kill_macro": "risk.flatten"}
            safety.on_kill("risk.flatten")
            self.assertEqual(self.sent[-1], ("safety.settings", {"kill_macro": "risk.flatten"}))
            with self.assertRaises(excel.YcError):
                safety.settings(nope=1)
            self.reply = {"operator": False, "alarms": [], "heartbeats": []}
            self.assertFalse(safety.set_operator_mode(False))
            self.assertEqual(self.sent[-1], ("safety.mode", {"operator": False, "pin": ""}))
            self.reply = [{"time": 1.7e9, "user": "u", "kind": "parameter", "target": "Spread", "before": "1", "after": "2", "source": "user"}]
            log = safety.change_log(10)
            self.assertEqual(log[0]["time"].year, 2023)
            self.assertEqual(self.sent[-1], ("safety.log_get", {"limit": 10}))
        finally:
            safety._rq = orig


class InteractAlertsRequestTest(RequestShapeTest):
    def test_requests(self):
        from yc import alerts, interact
        from yc import _conn as conn_mod

        class Fake:
            def __init__(self, outer):
                self.outer = outer

            def request(self, method, params):
                return excel._rq(method, **params)

        orig_i, orig_a = interact._conn, alerts._conn
        interact._conn = alerts._conn = lambda: Fake(self)
        try:
            self.reply = None
            rng = excel.Range._from(2, [(2, 1, 41, 1)])
            got = []

            @interact.on_click(rng, buttons=("left", "right"))
            def h(ctx):
                got.append((ctx.button, ctx.record["Price"]))

            m, p = self.sent[-1]
            self.assertEqual(m, "ui.click_range")
            self.assertEqual((p["sheet"], p["rect"], p["left"], p["right"]), (2, [2, 1, 41, 1], True, True))
            interact._dispatch_click({"id": p["id"], "button": "right", "record": {"Price": 101.5}})
            self.assertEqual(got, [("right", 101.5)])
            interact.on_key("^b", lambda ctx: got.append(ctx.key))
            self.assertEqual(self.sent[-1], ("ui.onkey", {"key": "^b", "procedure": "yc.interact:^b"}))
            self.assertTrue(interact._dispatch_key({"procedure": "yc.interact:^b", "key": "^b"}))
            self.assertEqual(got[-1], "^b")
            self.assertFalse(interact._dispatch_key({"procedure": "module.macro"}))
            excel.Application.OnKey("^q", "risk.flatten")
            self.assertEqual(self.sent[-1], ("ui.onkey", {"key": "^q", "procedure": "risk.flatten"}))
            excel.Application.OnKey("^q")
            self.assertEqual(self.sent[-1], ("ui.onkey", {"key": "^q", "procedure": None}))
            self.reply = True
            alerts.add("Delta", excel.Range._from(3, [(4, 2, 4, 2)]), ">", 500, color="#ff0000", cooldown=10)
            m, p = self.sent[-1]
            self.assertEqual((m, p["op"], p["a"], p["color"], p["cooldown"], p["rect"]), ("alerts.set", 1, 500.0, 0xFFFF0000, 10.0, [4, 2, 4, 2]))
            alerts.add("Band", excel.Range._from(3, [(4, 2, 9, 2)]), "outside", 0.1, 0.6, color=None)
            self.assertEqual((self.sent[-1][1]["op"], self.sent[-1][1]["b"], self.sent[-1][1]["color"]), (8, 0.6, 0))
            with self.assertRaises(excel.YcError):
                alerts.add("Bad", excel.Range._from(3, [(4, 2, 4, 2)]), "~", 1)
            seen = []
            alerts.on_alert(lambda a: seen.append(a.name))
            alerts._dispatch({"name": "Delta", "message": "x"})
            self.assertEqual(seen, ["Delta"])
        finally:
            interact._conn, alerts._conn = orig_i, orig_a
            alerts._handlers.clear()
