# SPDX-License-Identifier: Apache-2.0
r"""Random edits against a hidden YCGrid, checked against what the answer must be (see
skill/ycgrid-python/references/testing.md, "Checked at random").

    <YCGrid>\runtime\python\python.exe -E -s scripts\calc_fuzz.py --mode recalc|undo|files [--seed N] [--steps S] [--exe path]

recalc: Python writes values and formulas (sums, lookups, whole columns and rows, OFFSET, ROW(), dynamic arrays,
        other sheets) and inserts / deletes rows, columns and cells, cuts, copies, clears, copies and deletes sheets.
        After each step the values the engine calculated step by step must equal CalculateFull's: a formula the
        dependency index missed shows an old value.
undo:   the same kinds of changes made in the UI (typing, Insert / Delete rows and columns, Ctrl+D, Delete, copy and
        cut with paste, sorting); Ctrl+Z must give back exactly the cells before each step, Ctrl+Y the cells after
        it, and the calculated values must equal CalculateFull's. Ends with save / reopen: the same cells.
files:  cells of every kind (numbers, text, TRUE, errors, dates, formulas of many functions, dynamic arrays, other
        sheets, number formats) saved as .ycg and as .xlsx and opened again: the same values, formulas and formats.

Circular references are taken out as soon as they appear (Worksheet.CircularReference). Exit code 1 when a step
went wrong; the log names the steps before it (a seed replays a run).
"""
from __future__ import annotations

import argparse
import os
import random
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
UNDO_LIMIT = 100  # Document::undo_limit
COLS = "ABCDEFGH"


class Fuzz:
    def __init__(self, a):
        import yc

        self.a = a
        self.rnd = random.Random(a.seed)
        self.problems = 0
        self.log = []
        self.p = yc.launch(hidden=True, exe=a.exe, lang="en", size=(1600, 900))
        self.conn = yc.connect(target=self.p.pid)

    # ---- what to write ----
    def cell(self):
        return f"{self.rnd.choice(COLS)}{self.rnd.randint(1, 30)}"

    def rng(self):
        c1, c2 = sorted(self.rnd.sample(range(len(COLS)), 2))
        r1, r2 = sorted(self.rnd.sample(range(1, 31), 2))
        return f"{COLS[c1]}{r1}:{COLS[c2]}{r2}"

    def col(self):
        return f"{self.rnd.choice(COLS)}:{self.rnd.choice(COLS)}"

    def formula(self):
        from yc import excel as X

        r, n = self.rnd, self.rnd.randint(0, 17)
        s = f"'{r.choice([w.Name for w in X.Worksheets])}'!" if r.random() < 0.3 else ""
        return [f"={s}{self.cell()}+{self.cell()}", f"=SUM({s}{self.rng()})", f"=AVERAGE({s}{self.rng()})*2",
                f"=IFERROR(MATCH({r.randint(1, 50)},{s}{self.col()},1),-1)", f"=IFERROR(INDEX({s}{self.col()},{r.randint(1, 30)}),0)",
                "=ROW()*10+COLUMN()", f"=SUMIF({s}{self.rng()},\">10\")", f"=COUNTIF({s}{self.col()},\">5\")",
                f"=IFERROR(VLOOKUP({r.randint(1, 50)},{s}{self.rng()},1,TRUE),-1)", f"=OFFSET({s}{self.cell()},1,1)",
                f"=SUM({s}{r.randint(1, 30)}:{r.randint(1, 30)})", f"=SEQUENCE({r.randint(1, 3)},{r.randint(1, 3)},{r.randint(1, 9)})",
                f"=MAX({s}{self.rng()})-MIN({s}{self.rng()})",
                f"=SUMPRODUCT({s}{r.choice(COLS)}1:{r.choice(COLS)}5,{s}{r.choice(COLS)}6:{r.choice(COLS)}10)",
                f"=ROWS({s}{self.rng()})+COLUMNS({s}{self.rng()})",
                f"=IFERROR(XLOOKUP({r.randint(1, 50)},{s}{self.col()},{s}{self.col()}),0)", f"=COUNT({s}{self.col()})",
                "=SHEET()+SHEETS()"][n]

    # ---- checks ----
    def snap(self):
        from yc import excel as X

        out = {}
        for ws in X.Worksheets:
            r = ws.Range("A1:L45")
            out[ws.Name] = (r.Value2, r.Formula)
        return out

    @staticmethod
    def diff(a, b, what):
        lines = []
        for n in sorted(set(a) | set(b)):
            if n not in a or n not in b:
                lines.append(f"{what}: sheet {n} only on one side")
                continue
            for k in (0, 1):
                va, vb = a[n][k], b[n][k]
                for i in range(len(va)):
                    for j in range(len(va[i])):
                        x, y = va[i][j], vb[i][j]
                        if x != y and not (isinstance(x, float) and isinstance(y, float) and abs(x - y) < 1e-9 * max(1, abs(x))):
                            # the cell's formula too: a value that differs says little without what computes it
                            f = a[n][1][i][j] if k == 0 else ""
                            lines.append(f"{what}: {n}!{chr(65 + j)}{i + 1} {'value' if k == 0 else 'formula'} {x!r} vs {y!r}"
                                         + (f" ({f})" if isinstance(f, str) and f.startswith("=") else ""))
        return lines

    def problem(self, title, lines, last=6):
        self.problems += 1
        print(f"!!! {title}")
        for ln in self.log[-last:]:
            print("    step:", ln)
        for ln in lines[:12]:
            print("   ", ln)

    def no_cycles(self):
        from yc import excel as X

        for w in X.Worksheets:
            cr = w.CircularReference
            if cr is not None:
                self.log[-1] += f" (circular at {w.Name}!{cr.Address()}: cleared)"
                cr.ClearContents()

    def full_equals_incremental(self):
        from yc import excel as X

        a = self.snap()
        X.Application.CalculateFull()
        b = self.snap()
        d = self.diff(a, b, "step by step vs CalculateFull")
        if d:
            self.problem("RECALC: a value differs from a full recalculation", d)
        return b

    def setup(self):
        from yc import excel as X

        X.Application.DisplayAlerts = False
        X.Worksheets.Add()
        for ws in X.Worksheets:
            ws.Range("A1:H30").Value = [[self.rnd.randint(0, 50) for _ in range(8)] for _ in range(30)]

    # ---- recalc: Python changes ----
    def py_step(self):
        from yc import excel as X

        r = self.rnd
        ws = r.choice(list(X.Worksheets))
        k = r.random()
        if k < 0.25:
            c, v = self.cell(), r.randint(-5, 50)
            ws.Range(c).Value = v
            return f"{ws.Name}!{c} = {v}"
        if k < 0.50:
            c, f = self.cell(), self.formula()
            ws.Range(c).Formula = f
            return f"{ws.Name}!{c} := {f}"
        if k < 0.58:
            a, n = r.randint(1, 30), r.randint(1, 3)
            ws.Range(f"{a}:{a + n - 1}").EntireRow.Insert()
            return f"{ws.Name} insert rows {a} x{n}"
        if k < 0.66:
            a, n = r.randint(1, 30), r.randint(1, 3)
            ws.Range(f"{a}:{a + n - 1}").EntireRow.Delete()
            return f"{ws.Name} delete rows {a} x{n}"
        if k < 0.71:
            c = r.choice(COLS)
            ws.Range(f"{c}:{c}").EntireColumn.Insert()
            return f"{ws.Name} insert column {c}"
        if k < 0.76:
            c = r.choice(COLS)
            ws.Range(f"{c}:{c}").EntireColumn.Delete()
            return f"{ws.Name} delete column {c}"
        if k < 0.80:
            g = self.rng()
            ws.Range(g).Insert(Shift=X.xlShiftDown)
            return f"{ws.Name} insert cells {g}, shift down"
        if k < 0.84:
            g = self.rng()
            ws.Range(g).Delete(Shift=X.xlShiftToLeft)
            return f"{ws.Name} delete cells {g}, shift left"
        if k < 0.88:
            g = self.rng()
            ws.Range(g).ClearContents()
            return f"{ws.Name} clear {g}"
        if k < 0.92:
            g, d = self.rng(), self.cell()
            ws.Range(g).Cut(Destination=ws.Range(d))
            return f"{ws.Name} cut {g} to {d}"
        if k < 0.95:
            g, d = self.rng(), self.cell()
            ws.Range(g).Copy(Destination=ws.Range(d))
            return f"{ws.Name} copy {g} to {d}"
        if k < 0.97 and X.Worksheets.Count < 4:
            ws.Copy(After=ws)
            return f"copy sheet {ws.Name}"
        if X.Worksheets.Count > 2:
            n = ws.Name
            ws.Delete()
            return f"delete sheet {n}"
        X.Worksheets.Add()
        return "add sheet"

    def run_recalc(self):
        for i in range(self.a.steps):
            try:
                what = self.py_step()
            except Exception as e:  # noqa: BLE001 - a refused change (a part of an array...) is a step too
                what = f"(refused: {type(e).__name__}: {str(e)[:120]})"
            self.log.append(f"{i}: {what}")
            if self.p.poll() is not None:
                self.problem("CRASH", [])
                return
            self.no_cycles()
            self.full_equals_incremental()
            if self.problems > 3:
                return

    # ---- undo: UI changes ----
    def inp(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=30)

    def ui_step(self):
        from yc import excel as X

        r = self.rnd
        ws = r.choice(list(X.Worksheets))
        ws.Activate()
        k = r.random()
        if k < 0.30:
            c, v = self.cell(), r.randint(-5, 50) + 0.5   # never the value already there
            self.inp(f"g:{c}|t:{v}|k:Enter|w:2")
            return f"{ws.Name} type {c} {v}"
        if k < 0.50:
            c, f = self.cell(), self.formula()
            self.inp(f"g:{c}|t:{f}|k:Enter|w:2")
            return f"{ws.Name} type {c} {f}"
        if k < 0.58:
            a, n = r.randint(1, 30), r.randint(1, 3)
            self.inp(f"g:{a}:{a + n - 1}|w:2|x:InsertRows|w:3")
            return f"{ws.Name} insert rows {a} x{n}"
        if k < 0.66:
            a, n = r.randint(1, 30), r.randint(1, 3)
            self.inp(f"g:{a}:{a + n - 1}|w:2|x:DeleteRows|w:3")
            return f"{ws.Name} delete rows {a} x{n}"
        if k < 0.71:
            c = r.choice(COLS)
            self.inp(f"g:{c}:{c}|w:2|x:InsertColumns|w:3")
            return f"{ws.Name} insert column {c}"
        if k < 0.76:
            c = r.choice(COLS)
            self.inp(f"g:{c}:{c}|w:2|x:DeleteColumns|w:3")
            return f"{ws.Name} delete column {c}"
        if k < 0.81:
            g = self.rng()
            self.inp(f"g:{g}|w:2|k:c-D|w:3")
            return f"{ws.Name} fill down {g}"
        if k < 0.85:
            g = self.rng()
            self.inp(f"g:{g}|w:2|k:Delete|w:3")
            return f"{ws.Name} Delete {g}"
        if k < 0.90:
            g, d = self.rng(), self.cell()
            self.inp(f"g:{g}|w:2|k:c-C|w:2|g:{d}|w:2|k:c-V|w:3|k:Escape")
            return f"{ws.Name} copy {g}, paste at {d}"
        if k < 0.94:
            g, d = self.rng(), self.cell()
            self.inp(f"g:{g}|w:2|k:c-X|w:2|g:{d}|w:2|k:c-V|w:3")
            return f"{ws.Name} cut {g}, paste at {d}"
        g = f"A{r.randint(1, 10)}:{r.choice(COLS)}{r.randint(12, 30)}"
        self.inp(f"g:{g}|w:2|x:SortAscending|w:4|k:Escape")
        return f"{ws.Name} sort {g}"

    def run_undo(self):
        from yc import excel as X, ui

        first = self.snap()
        steps = []   # (what, cells before, cells after) of the steps on the undo list, oldest first

        def check(want, what):
            got = self.snap()
            d = self.diff(want, got, what)
            if d:
                self.problem(f"UNDO: {what} doesn't give back the cells", d, last=4)

        for i in range(self.a.steps):
            r = self.rnd.random()
            if steps and r < 0.25:
                n = 1 if r < 0.18 else self.rnd.randint(1, min(4, len(steps)))
                self.inp("|".join(["k:c-Z|w:3"] * n))
                self.log.append(f"{i}: Ctrl+Z x{n} (back over {[s[0] for s in steps[-n:]]})")
                check(steps[-n][1], f"Ctrl+Z x{n}")
                self.inp("|".join(["k:c-Y|w:3"] * n))
                self.log.append(f"{i}: Ctrl+Y x{n}")
                check(steps[-1][2], f"Ctrl+Y x{n}")
                if self.problems > 3:
                    return
                continue
            if i % 50 == 49:   # Python inserting rows drops the steps undone with their whole version (not all of them)
                ws0 = X.Worksheets(1)
                ws0.Rows(300).Insert()
                ws0.Rows(300).Delete()
                first, steps = self.snap(), []
                self.log.append(f"{i}: (the undo list cleared)")
            before = steps[-1][2] if steps else first
            u0 = ui.state()["undo"]
            depth0, made0 = u0["depth"], u0.get("made")
            try:
                what = self.ui_step()
            except Exception as e:  # noqa: BLE001
                what = f"(client: {type(e).__name__}: {str(e)[:120]})"
            if any(w.CircularReference is not None for w in X.Worksheets):   # typed or pasted: taken back
                if ui.state().get("dialog"):   # Excel's circular reference warning (the first time): OK
                    self.inp("k:Enter|w:3")
                self.inp("k:c-Z|w:3")
                what += " (circular: undone)"
            st = ui.state()
            if st.get("dialog"):
                what += f" (a dialog stayed open: {st.get('status')!r}; Escape)"
                self.inp("k:Escape|w:3")
            self.log.append(f"{i}: {what}")
            if self.p.poll() is not None:
                self.problem("CRASH", [])
                return
            after = self.full_equals_incremental()
            # at the undo list's limit (100) a new step drops the oldest and the depth stays: Python's writes leave the
            # steps undone by their cells, so the list fills up (seed 3028 reported a typed cell as no step). The count
            # of steps made tells: taking a step there for any action, a sort that made none was "undone" by taking
            # back the typing before it (seed 3130)
            u1 = ui.state()["undo"]
            made = u1["made"] > made0 if made0 is not None else (u1["depth"] > depth0 or depth0 >= UNDO_LIMIT)
            if "(circular: undone)" in what:   # its step was taken back here: the list is as it was
                made = False
            if made:   # clearing empty cells is a step too
                steps = (steps + [(what, before, after)])[-30:]
            elif self.diff(before, after, "same"):
                self.problem("UNDO: the cells changed without a step on the undo list",
                             self.diff(before, after, "changed") + [f"undo depth {depth0} -> {ui.state()['undo']['depth']}"])
            if self.problems > 3:
                return
        # save / reopen
        import yc

        path = os.path.join(tempfile.gettempdir(), f"calc_fuzz{self.a.seed}.ycg")
        saved = self.snap()
        X.ActiveWorkbook.SaveAs(path)
        self.p.terminate()
        self.p.wait(10)
        self.p = yc.launch(path, hidden=True, exe=self.a.exe, lang="en")
        yc.connect(target=self.p.pid)
        d = self.diff(saved, self.snap(), "save / reopen")
        if d:
            self.problem("SAVE: the reopened workbook has other cells", d)


# ---- files: save and open again ----
FILE_FORMULAS = ["=SUM(A1:A5)", "=AVERAGE(B1:B9)", "=IF(A1>5,\"big\",\"small\")", "=VLOOKUP(3,A1:C9,2,FALSE)",
                 "=TEXT(A1,\"0.00\")", "=A1&\" \"&B1", "=ROUND(PI()*A1,3)", "=IFERROR(1/0,\"div\")",
                 "=COUNTIFS(A1:A9,\">2\",B1:B9,\"<50\")", "=SUMPRODUCT(A1:A3,B1:B3)", "=INDEX(A1:C9,2,3)", "=MATCH(4,A1:A9,0)",
                 "=DATE(2026,10,30)", "=EDATE(DATE(2026,1,31),1)", "=LEFT(\"hello\",2)", "=SUBSTITUTE(\"a-b-c\",\"-\",\"+\")",
                 "=MAX(A:A)", "=SUM(Sheet2!A1:B3)", "=XLOOKUP(5,A1:A9,B1:B9,\"none\")", "=LET(x,A1,x*x+1)", "=SEQUENCE(2,2)",
                 "=FILTER(A1:A9,A1:A9>3)", "=SORT(B1:B5)", "=UNIQUE(C1:C9)", "=TEXTJOIN(\",\",TRUE,A1:A4)",
                 "=IFS(A1>3,1,TRUE,2)", "=SWITCH(A2,1,\"one\",\"other\")", "=$A$1+A$2+$B3", "=SUM(A1:A3 B2:B4)", "=-2^2",
                 "=10%", "=\"a\"\"b\"", "=TRUE", "=NA()", "=ROWS(A:A)", "=SUM(1:1)", "=AND(A1>0,OR(B1<0,C1=\"x\"))"]
FILE_VALUES = [0, 1, -1, 3.5, 1e-10, 123456789012, "text", "台積電", True, False, "'0012", 0.1 + 0.2]
FILE_FORMATS = ["0.00", "#,##0", "yyyy-mm-dd", "0%", "@", "General", "[Red]0.0;[Blue]-0.0"]


def run_files(f):
    import yc
    from yc import excel as X

    r = f.rnd
    ws1 = X.Worksheets(1)
    X.Worksheets.Add(After=ws1)
    for ws in X.Worksheets:
        ws.Range("A1:C9").Value = [[r.randint(0, 9) for _ in range(3)] for _ in range(9)]
        for _ in range(f.a.steps):
            c = f"{r.choice('ABCDEF')}{r.randint(1, 15)}"
            v = r.choice(FILE_FORMULAS) if r.random() < 0.6 else r.choice(FILE_VALUES)
            try:
                if isinstance(v, str) and v.startswith("="):
                    ws.Range(c).Formula = v
                else:
                    ws.Range(c).Value = v
            except Exception:  # noqa: BLE001 - a refused formula is a step too
                pass
            f.log.append(f"{ws.Name}!{c} {v!r}")
            if r.random() < 0.3:
                ws.Range(c).NumberFormat = r.choice(FILE_FORMATS)
    for _ in range(50):   # circular references give other values at every calculation
        cr = [x for x in (w.CircularReference for w in X.Worksheets) if x is not None]
        if not cr:
            break
        for x in cr:
            x.ClearContents()

    def cells():
        # each cell's format: a range's NumberFormat is one value (None when its cells differ), as in Excel
        def formats(ws):
            return [[ws.Cells(i, j).NumberFormat for j in range(1, 7)] for i in range(1, 16)]

        return {ws.Name: (ws.Range("A1:F15").Value2, ws.Range("A1:F15").Formula, formats(ws)) for ws in X.Worksheets}

    for ext in ("ycg", "xlsx"):
        before = cells()
        path = os.path.join(tempfile.gettempdir(), f"calc_fuzz_files{f.a.seed}.{ext}")
        if os.path.exists(path):
            os.remove(path)
        X.ActiveWorkbook.SaveAs(path)
        q = yc.launch(path, hidden=True, exe=f.a.exe, lang="en")
        try:
            yc.connect(target=q.pid)
            after = cells()
        finally:
            q.terminate()
            q.wait(10)
            yc.connect(target=f.p.pid)
        lines = []
        for n, (va, fa, na) in before.items():
            if n not in after:
                lines.append(f"{ext}: sheet {n} missing")
                continue
            for k, what in enumerate(("value", "formula", "format")):
                a, b = before[n][k], after[n][k]
                for i in range(len(a)):
                    for j in range(len(a[i])):
                        x, y = a[i][j], b[i][j]
                        if x != y and not (isinstance(x, float) and isinstance(y, float) and abs(x - y) <= 1e-12 * max(1, abs(x))):
                            lines.append(f"{ext}: {n}!{'ABCDEF'[j]}{i + 1} {what}: {x!r} -> {y!r} (formula {fa[i][j]!r})")
        if lines:
            f.problem(f"FILES: the reopened .{ext} has other cells", lines, last=0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mode", choices=["recalc", "undo", "files"], default="recalc")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--exe", default=os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(a.exe)), "python"))
    f = Fuzz(a)
    t0 = time.time()
    try:
        f.setup()
        if a.mode == "recalc":
            f.run_recalc()
        elif a.mode == "undo":
            f.run_undo()
        else:
            run_files(f)
    finally:
        try:
            f.p.terminate()
        except Exception:  # noqa: BLE001
            pass
    print(f"{a.mode} seed {a.seed}: {len(f.log)} steps in {time.time() - t0:.0f}s, problems: {f.problems}")
    return 1 if f.problems else 0


if __name__ == "__main__":
    sys.exit(main())
