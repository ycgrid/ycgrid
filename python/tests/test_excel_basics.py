# SPDX-License-Identifier: Apache-2.0
"""Basic operations replayed against Excel's measured results: tests/data/excel_basics_measured.json holds what Excel 16
gave for each case of scripts/excel_basics_cases.py (insert / delete of rows, columns and cells, Copy and Cut with a
Destination, PasteSpecial, the Fill commands, AutoFill, Clear, Merge, Sort and typed input). Each case runs on a fresh
sheet of a hidden YCGrid through yc.excel and every cell of its windows is compared: Formula, Value2, NumberFormat
(and MergeCells for the merge cases), and the RefersTo of the defined names.

KNOWN_DIFFERENCES lists the cases where YCGrid differs today, each with its reason: fix one, delete its entry.
KEPT_ON_PURPOSE lists the differences YCGrid keeps (its settings' defaults). The test also fails when a listed case
starts agreeing, so the lists stay honest. Measure again with
scripts/measure_excel_basics.py after changing a case (the fixture keeps each case's hash).

Run: python -m unittest tests.test_excel_basics (with the bundled Python). YC_BASICS_DUMP=<file> writes every
difference found, the known ones too; YC_BASICS_ONLY=<id>,<id> replays only those cases.
"""
from __future__ import annotations

import json
import math
import os
import re
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import yc  # noqa: E402
import excel_basics_cases as cases  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"
FIXTURE = os.path.join(ROOT, "tests", "data", "excel_basics_measured.json")

# Cases where YCGrid gives another result than Excel today: case id -> the reason. Fix the cause, then delete the line.
KNOWN_DIFFERENCES: dict[str, str] = {
    # Python's writes are calculated together, Excel calculates after each: a formula written just before the write
    # that closes a circular reference, with no read between, is calculated with it and shows 0 as one entered after
    # it does, where Excel keeps the value it had (docs/excel-parity-backlog.md, Open)
    "rand3_029": "=UNIQUE(B2:B6) written just before {=SUM(B2:B5)} closes a cycle: Excel keeps its values, YCGrid 0",
    "rand3_112": "Other!D1 read B2 before {=SUM(B2:B5)} over A1:B2 closed a cycle: Excel keeps 30, YCGrid 0",
}

# Cases where YCGrid differs from Excel on purpose (docs/settings.md): checked like KNOWN_DIFFERENCES, never fixed.
KEPT_ON_PURPOSE: dict[str, str] = {
    "type_formula_123456789012345678": "calculation.excel_15_digit_input is false by default: long numbers are kept exactly",
    "type_formula_12345678901234567890": "calculation.excel_15_digit_input is false by default: long numbers are kept exactly",
    # a circular reference built by a script: what reads the cycle keeps the value it showed when the cycle closed, and
    # YCGrid calculates a script's writes together, Excel after each one (docs/excel-parity-backlog.md, Kept on
    # purpose; with a read between the steps both give Excel's values, measured 2026-11-15)
    "rand_058": "a cycle closed by a script: G6 =COUNTA(A1:A8) shows 5, its value before the cut, where Excel recalculated it to 4 first",
    "rand_207": "a cycle closed by a script (B4 =$B4*#REF!): what reads it keeps the values it showed",
    "rand_218": "a cycle closed by a script (B2): what reads it keeps the values it showed",
    "rand_228": "a cycle closed by a script (B5 =$D$6 under D6 =SUM(D2:D5)): what reads it keeps the values it showed",
    "rand_290": "a cycle closed by a script (B8 =SUM(B:B) cut there): what reads it keeps the values it showed",
    "rand_380": "a cycle closed by a script (B8 =SUM(B:B) typed): it shows 0, where Excel gives the #REF! of B4 it reads",
    "rand2_058": "a cycle closed by a script (D6): what reads it keeps the values it showed",
    "rand2_082": "a cycle closed by a script (B8 =SUM(B:B) typed): what reads it keeps the values it showed",
    "rand2_094": "a cycle closed by a script (C3 =$B$2+C3 typed): Other!D2 =T!C3 keeps the value it showed",
    "rand2_109": "a cycle closed by a script (C3): what reads it keeps the values it showed",
    "rand2_123": "a cycle closed by a script (B2): what reads it keeps the values it showed",
    "rand2_158": "a cycle closed by a script (C3): what reads it keeps the values it showed",
    "rand2_162": "a cycle closed by a script (B2): what reads it keeps the values it showed",
    # (with a read before each step, measured 2026-11-21, these give Excel's values too)
    "rand3_026": "a cycle closed by a script (B5 =FILTER(B2:B5,...)): what reads it keeps the values it showed",
    "rand3_049": "a cycle closed by a script (C3 =SUM(B2:B5*C2:C5)): Other!D2 =T!C3, written with the case, shows 0",
    "rand3_126": "a cycle closed by a script (C2 =D6-D2): what reads it keeps the values it showed",
    "cyc_array_sum": "a cycle closed by a script ({=SUM(B2:B5)} in A1:B2): Other!D1, written with the case, shows 0",
    "cyc_array_self": "a cycle closed by a script ({=C2:D3+1}): Other!D2, written with the case, shows 0",
    "rand3_168": "a cycle closed by a script (B4 =FILTER(B2:B5,...)), then filled formulas reading it: D9 keeps its value",
    # a sheet grows past Excel's 1,048,576 rows when rows go in (docs/excel-parity-backlog.md, Kept on purpose): what
    # Excel cuts at its last row (I2:I1048576 stays, I1048576 is #REF!) moves down with the rows in YCGrid
    "edge_insert_rows_range": "rows inserted: =SUM(I2:I1048576) and =I1048576+1 move to row 1048577, past Excel's last row",
}
EXPECTED_DIFFERENCES = {**KNOWN_DIFFERENCES, **KEPT_ON_PURPOSE}


# Texts the measuring Excel wrote in its own UI language (Traditional Chinese): a table's default headers and an error
# value made header text. YCGrid runs these cases in English, so they are read as US Excel writes them.
_LOCALIZED = [(re.compile("^\u6b04(\\d+)$"), r"Column\1"), (re.compile("^#\u6ea2\u51fa!(\\d*)$"), r"#SPILL!\1")]


def _unlocalize(x):
    if isinstance(x, str):
        for pat, repl in _LOCALIZED:
            x = pat.sub(repl, x)
    return x


def _load_fixture():
    with open(FIXTURE, encoding="utf-8") as f:
        data = json.load(f)
    for rec in data["cases"]:
        for entry in rec.get("cells", {}).values():
            entry[0], entry[1] = _unlocalize(entry[0]), _unlocalize(entry[1])
    return data


def _norm_value(v):
    from yc.types import XlError

    if isinstance(v, XlError):
        return {"err": v.Text}
    if isinstance(v, float) and v.is_integer() and abs(v) < 2 ** 53:
        return int(v)
    return v


def _same_value(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b or (isinstance(a, bool) and isinstance(b, bool) and a == b)
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b or math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-300)
    return a == b


def _same_formula(a, b):
    if a == b:
        return True
    try:  # a constant: the same number written with other digits (15 significant digits on both sides)
        return math.isclose(float(a), float(b), rel_tol=1e-12)
    except (TypeError, ValueError):
        return False


@unittest.skipUnless(LIVE and os.path.exists(FIXTURE), "no built ycgrid.exe or no measured fixture")
class ExcelBasicsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        from yc import excel

        excel.Application.Calculation = excel.xlCalculationAutomatic
        other = excel.Worksheets(1)
        other.Name = "Other"
        for addr, text in cases.OTHER_STATIC.items():
            other.Range(addr).Formula = text
        cls.other = other

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    # ---- running one case ----
    def _rng(self, ws, addr):
        from yc import excel

        if "!" in addr:
            sheet, _, ref = addr.rpartition("!")
            return excel.Worksheets(sheet).Range(ref)
        return ws.Range(addr)

    def _fresh_sheet(self):
        from yc import excel

        try:
            excel.Worksheets("T").Delete()
        except Exception:  # noqa: BLE001 (the first case has none)
            pass
        ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        ws.Name = "T"
        ws.Activate()
        return ws

    def _run(self, p):
        """Run a resolved case; (refused message, {addr: [formula, value, numfmt(, merged)]}, {name: RefersTo})."""
        from yc import excel

        ws = self._fresh_sheet()
        self.other.Range(cases.OTHER_CLEAR).Clear()
        for addr, text, fmt in p["cells"]:
            r = ws.Range(addr)
            if fmt is not None:
                r.NumberFormat = fmt
            r.Formula = text
        for name, ref in p["names"]:
            excel.ActiveWorkbook.Names.Add(Name=name, RefersTo=ref)
        for addr, text in p["other"]:
            self.other.Range(addr).Formula = text
        refused = ""
        for i, step in enumerate(p["steps"], 1):
            kind, target, what, arg = step
            try:
                # "#sheet" is the case's sheet itself, and a dotted method walks its properties (ListObjects.Add)
                t = ws if target == "#sheet" else self._rng(ws, target)
                if kind == "set":
                    setattr(t, what, arg)
                else:
                    kw = {k: (self._rng(ws, v["range"]) if isinstance(v, dict) else v) for k, v in arg.items()}
                    *path, method = what.split(".")
                    for part in path:
                        t = getattr(t, part)
                    getattr(t, method)(**kw)
            except Exception as e:  # noqa: BLE001 (Excel refused some steps too: compared below)
                refused = f"step {i}: {type(e).__name__}: {e}"
                break
        excel.Application.CutCopyMode = False
        got = {}
        for rng in p["read"]:
            sheet, r0, c0, nr, nc, grid = cases.window_cells(rng)
            r = self._rng(ws, rng)
            f, v, nf = r.Formula, r.Value2, r._get("numfmt")
            if nr == 1 and nc == 1:
                f, v, nf = [[f]], [[v]], [[nf]]
            for i in range(nr):
                for j in range(nc):
                    entry = [f[i][j], _norm_value(v[i][j]), nf[i][j]]
                    if p["merge"]:
                        entry.append(bool(self._rng(ws, grid[i][j]).MergeCells))
                    got[grid[i][j]] = entry
        names = {}
        for name, _ in p["names"]:
            try:
                names[name] = excel.ActiveWorkbook.Names(name).RefersTo
            except Exception as e:  # noqa: BLE001
                names[name] = f"<{type(e).__name__}: {e}>"
        return refused, got, names

    def _compare(self, p, rec, refused, got, names):
        out = []
        if bool(rec["refused"]) != bool(refused):
            out.append(f"refused: Excel {rec['refused'] or 'no'!r} | YCGrid {refused or 'no'!r}")
        skip_fmt = "numfmt" in p["skip"]
        for rng in p["read"]:
            grid = cases.window_cells(rng)[5]
            for row in grid:
                for addr in row:
                    want = rec["cells"].get(addr) or (["", None, "General", False] if p["merge"] else ["", None, "General"])
                    have = got[addr]
                    bad = []
                    if not _same_formula(want[0], have[0]):
                        bad.append("formula")
                    if not _same_value(want[1], have[1]):
                        bad.append("value")
                    if not skip_fmt and want[2] != have[2]:
                        bad.append("numfmt")
                    if p["merge"] and want[3] != have[3]:
                        bad.append("merged")
                    if bad:
                        out.append(f"{addr} ({', '.join(bad)}): Excel {want!r} | YCGrid {have!r}")
        for name, ref in rec["names"].items():
            if names.get(name) != ref:
                out.append(f"name {name}: Excel {ref!r} | YCGrid {names.get(name)!r}")
        return out

    # ---- the tests ----
    def test_fixture_matches_the_cases(self):
        fx = _load_fixture()
        measured = {r["id"]: r["hash"] for r in fx["cases"]}
        stale = [c["id"] for c in cases.CASES if measured.get(c["id"]) != cases.plan_hash(cases.plan(c))]
        self.assertEqual(stale, [], "cases changed or added since they were measured: run scripts/measure_excel_basics.py --only ...")
        unknown = sorted(set(EXPECTED_DIFFERENCES) - set(measured))
        self.assertEqual(unknown, [], "KNOWN_DIFFERENCES / KEPT_ON_PURPOSE name cases that don't exist")

    def test_basic_operations_agree_with_excel(self):
        fx = _load_fixture()
        recs = {r["id"]: r for r in fx["cases"]}
        from yc import excel

        expected = dict(EXPECTED_DIFFERENCES)
        if excel.Application.TruncateTo15Digits:  # this computer's choice: long numbers are cut to 15 digits as in Excel
            for cid in KEPT_ON_PURPOSE:
                expected.pop(cid, None)
        diffs, agreeing_known = {}, []
        t0 = time.time()
        ran = 0
        only = set(filter(None, os.environ.get("YC_BASICS_ONLY", "").split(",")))  # replay these cases only
        for c in cases.CASES:
            if only and c["id"] not in only:
                continue
            rec = recs.get(c["id"])
            if rec is None or rec["error"]:
                continue  # not measured (test_fixture_matches_the_cases reports it) or Excel's run failed
            p = cases.plan(c)
            try:
                refused, got, names = self._run(p)
                d = self._compare(p, rec, refused, got, names)
            except Exception as e:  # noqa: BLE001 (one broken case doesn't hide the others)
                d = [f"replay failed: {type(e).__name__}: {e}"]
            ran += 1
            if d:
                diffs[c["id"]] = d
            elif c["id"] in expected:
                agreeing_known.append(c["id"])
        elapsed = time.time() - t0
        dump = os.environ.get("YC_BASICS_DUMP")
        if dump:
            with open(dump, "w", encoding="utf-8") as f:
                f.write(f"{ran} cases in {elapsed:.0f}s, {ran - len(diffs)} agree\n")
                for cid, d in diffs.items():
                    f.write(f"\n{cid}{'  [expected: ' + expected[cid] + ']' if cid in expected else ''}\n")
                    for line in d:
                        f.write(f"  {line}\n")
        new = {cid: d for cid, d in diffs.items() if cid not in expected}
        msg = []
        if new:
            msg.append(f"{len(new)} case(s) differ from Excel (of {ran}):")
            for cid, d in new.items():
                msg.append(f"  {cid}:")
                msg.extend(f"    {line}" for line in d[:12])
                if len(d) > 12:
                    msg.append(f"    ... {len(d) - 12} more")
        if agreeing_known:
            msg.append("these KNOWN_DIFFERENCES / KEPT_ON_PURPOSE cases agree with Excel now; delete their entries: " + ", ".join(agreeing_known))
        if msg:
            self.fail("\n".join(msg))


if __name__ == "__main__":
    unittest.main()
