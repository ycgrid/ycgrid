# SPDX-License-Identifier: Apache-2.0
"""Measure in Excel what a cut and paste does to a range a formula reads, and write tests/unit/excel_measured_moves.inc.

    dist_release\\runtime\\python\\python.exe -E -s scripts\\measure_excel_moves.py [--check]

Each case puts =SUM(range) in Z40, cuts a block that holds part of the range or lies next to it (Range.Cut
Destination) and reads the formula back. The cases: every cut of a column's cells over C2:C5, C3 and C2:C3 (to the
same column and the columns beside it), every cut meeting B2:D5 moved up to three rows and two columns, every cut
holding an edge of C2:C11, and two sets of random ranges, cuts and shifts. The hidden Excel is this script's own
(New-Object -ComObject never attaches to one the user runs), quit and killed by its own process id. It takes about 40
minutes. --check measures only the random sets and compares them with the table instead of writing it.

The table decides how Document::MoveCells moves a range's edges (MoveEdges in src/engine/document_data.cpp): a rule
written from a few cases fitted them and broke others, until 13,617 measured cuts settled it.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "unit", "excel_measured_moves.inc")

PS = r"""param([string]$In, [string]$Out)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W32 { [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p); }
"@
$sw = New-Object IO.StreamWriter($Out, $false, (New-Object Text.UTF8Encoding($false)))
$sw.AutoFlush = $true
$xl = New-Object -ComObject Excel.Application
$xlpid = [uint32]0
[void][W32]::GetWindowThreadProcessId([IntPtr]$xl.Hwnd, [ref]$xlpid)
try {
  $xl.DisplayAlerts = $false
  $xl.ScreenUpdating = $false
  $wb = $xl.Workbooks.Add()
  $ws = $wb.Worksheets.Item(1)
  $xl.Calculation = -4135
  foreach ($line in [IO.File]::ReadAllLines($In)) {
    $p = $line.Split(',') | ForEach-Object { [int]$_ }
    $a = $ws.Cells.Item($p[0], $p[1]).Address($false, $false)
    $b = $ws.Cells.Item($p[2], $p[3]).Address($false, $false)
    $ws.Range("Z40").Formula = "=SUM(${a}:${b})"
    $src = $ws.Range($ws.Cells.Item($p[4], $p[5]), $ws.Cells.Item($p[6], $p[7]))
    [void]$src.Cut($ws.Cells.Item($p[4] + $p[8], $p[5] + $p[9]))
    $sw.WriteLine($line + "," + $ws.Range("Z40").Formula)
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($src)
  }
  $wb.Close($false)
} finally {
  $sw.Close()
  $xl.Quit()
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($xl)
  Start-Sleep -Milliseconds 500
  if ($xlpid) { Stop-Process -Id $xlpid -ErrorAction SilentlyContinue }
}
"""


def column_cuts():
    # a column's cells cut over C2:C5, C3 and C2:C3: to the same column (C) and to B and D
    out = []
    for a, b in ((2, 5), (3, 3), (2, 3)):
        for s in range(1, 8):
            for e in range(s, 8):
                for d in range(1, 10):
                    for col in (3, 2, 4):
                        if d == s and col == 3:
                            continue
                        out.append((a, 3, b, 3, s, 3, e, 3, d - s, col - 3))
    return out


def block_cuts():
    # every block meeting B2:D5 (not holding all of it) moved up to three rows and two columns
    out = []
    for r0 in range(1, 7):
        for r1 in range(r0, 7):
            for c0 in range(1, 6):
                for c1 in range(c0, 6):
                    if r1 < 2 or r0 > 5 or c1 < 2 or c0 > 4:
                        continue
                    if r0 <= 2 and r1 >= 5 and c0 <= 2 and c1 >= 4:
                        continue
                    for dr in range(-3, 4):
                        for dc in range(-2, 3):
                            if (dr, dc) == (0, 0) or r0 + dr < 1 or c0 + dc < 1:
                                continue
                            out.append((2, 2, 5, 4, r0, c0, r1, c1, dr, dc))
    return out


def random_cuts(seed, n, rows, cols, reach, src_rows, src_cols, max_dr, max_dc, straight):
    rng = random.Random(seed)
    out = []
    while len(out) < n:
        r0 = rng.randint(2, 1 + rows[0]); r1 = r0 + rng.randint(0, rows[1])
        c0 = rng.randint(2, 1 + cols[0]); c1 = c0 + rng.randint(0, cols[1])
        s0 = rng.randint(max(1, r0 - reach), r1 + 1); s1 = s0 + rng.randint(0, src_rows)
        t0 = rng.randint(max(1, c0 - reach), c1 + 1); t1 = t0 + rng.randint(0, src_cols)
        if not (s1 >= r0 and s0 <= r1 and t1 >= c0 and t0 <= c1):
            continue
        if s0 <= r0 and s1 >= r1 and t0 <= c0 and t1 >= c1:
            continue
        k = rng.random() if straight else 1.0
        dr = rng.randint(-max_dr, max_dr); dc = rng.randint(-max_dc, max_dc)
        if k < 0.3:
            dc = 0
        elif k < 0.5:
            dr = 0
        if (dr, dc) == (0, 0) or s0 + dr < 1 or t0 + dc < 1:
            continue
        if straight and (s1 + dr >= 40 or t1 + dc >= 26):
            continue
        out.append((r0, c0, r1, c1, s0, t0, s1, t1, dr, dc))
    return out


def long_cuts():
    # every cut holding an edge of C2:C11 (one column wide, or B:D), to every row 1..14
    out = []
    for wide in (False, True):
        for s in range(1, 15):
            for e in range(s, 15):
                if e < 2 or s > 11 or (s <= 2 and e >= 11) or not (s <= 2 or e >= 11):
                    continue
                for d in range(1, 15):
                    if d != s:
                        c0, c1 = (2, 4) if wide else (3, 3)
                        out.append((2, 3, 11, 3, s, c0, e, c1, d - s, 0))
    return out


SETS = [
    ("a column's cuts over C2:C5, C3 and C2:C3", column_cuts),
    ("every block meeting B2:D5", block_cuts),
    ("random ranges and cuts (seed 20261123)", lambda: random_cuts(20261123, 2500, (5, 5), (5, 4), 3, 6, 5, 4, 4, False)),
    ("every cut holding an edge of C2:C11", long_cuts),
    ("random ranges, cuts and longer shifts (seed 77123)", lambda: random_cuts(77123, 2500, (8, 8), (6, 6), 4, 8, 6, 8, 6, True)),
]


def col_num(letters):
    n = 0
    for ch in letters:
        n = n * 26 + ord(ch) - 64
    return n


def parse(formula):
    if "#REF!" in formula:
        return (0, 0, 0, 0)
    m = re.fullmatch(r"=SUM\(([A-Z]+)(\d+)(?::([A-Z]+)(\d+))?\)", formula)
    if not m:
        raise ValueError("unexpected formula " + formula)
    c0, r0 = col_num(m.group(1)), int(m.group(2))
    c1, r1 = (col_num(m.group(3)), int(m.group(4))) if m.group(3) else (c0, r0)
    return (r0, c0, r1, c1)


def measure(cases):
    tmp = tempfile.mkdtemp(prefix="yc_moves_")
    script, inp, out = (os.path.join(tmp, n) for n in ("measure.ps1", "in.csv", "out.csv"))
    with open(script, "w", encoding="utf-8-sig") as f:
        f.write(PS)
    with open(inp, "w", encoding="utf-8") as f:
        f.write("\n".join(",".join(map(str, c)) for c in cases) + "\n")
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, "-In", inp, "-Out", out],
                   timeout=120 + len(cases), check=False)
    got = {}
    for line in open(out, encoding="utf-8-sig"):
        p = line.strip().split(",", 10)
        if len(p) == 11:
            got[tuple(map(int, p[:10]))] = parse(p[10])
    return got


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="measure the random sets and compare them with the table")
    a = ap.parse_args()
    sets = [(name, make()) for name, make in SETS]
    if a.check:
        table = {}
        for line in open(OUT, encoding="utf-8"):
            if line.startswith("{"):
                v = tuple(int(x) for x in re.findall(r"-?\d+", line))
                table[v[:10]] = v[10:]
        cases = [c for name, cs in sets if name.startswith("random") for c in cs]
        got = measure(cases)
        wrong = [c for c in cases if got.get(c) != table.get(c)]
        print(f"{len(got)} cuts measured, {len(wrong)} differ from the table")
        for c in wrong[:20]:
            print("  ", c, "Excel", got.get(c), "table", table.get(c))
        return 1 if wrong or len(got) != len(cases) else 0
    cases = [c for _, cs in sets for c in cs]
    got = measure(cases)
    if len(got) != len(cases):
        print(f"only {len(got)} of {len(cases)} cuts were measured")
        return 1
    lines = [
        "// Generated by scripts/measure_excel_moves.py from Excel 16: do not edit, measure again.",
        "// Each line: a range (first row, first column, last row, last column; 1-based) read by =SUM(range) in Z40, the",
        "// cells cut (the same four numbers), the rows and columns they move by, and the range Excel 16's formula reads",
        "// after Range.Cut Destination (0,0,0,0: =SUM(#REF!)).",
        f"// {len(cases)} cuts.",
    ]
    for name, cs in sets:
        lines.append(f"// {name}: {len(cs)} cuts")
        for c in cs:
            w = got[c]
            lines.append("{%d,%d,%d,%d, %d,%d,%d,%d, %d,%d, %d,%d,%d,%d}," % (*c, *w))
    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{len(cases)} cuts written to {os.path.relpath(OUT, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
