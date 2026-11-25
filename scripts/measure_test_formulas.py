# SPDX-License-Identifier: Apache-2.0
"""Check what the unit tests say a formula gives against Excel itself.

    dist_release\\runtime\\python\\python.exe -E -s scripts\\measure_test_formulas.py [--only FILE_PART]

Every `CHECK(t.Eval("formula") == "expected")` of tests/unit/*.cpp whose formula reads no cell (no reference, name,
table or volatile function) is calculated in a hidden Excel of this script's own (New-Object -ComObject never attaches
to one the user runs; it is quit and killed by its own process id), and the expected text is compared with Excel's
value. A test's expected value is a claim about Excel: written from a help page or a guess, it can be wrong while the
test passes (ADDRESS's help page quotes "[Book1]Sheet1", Excel doesn't). Prints each disagreement; exit code 1 when
there is one Excel can calculate (functions newer than the Excel at hand give #NAME? and are listed apart).

Differences that come from the region (DOLLAR's negative format, CHAR / CODE's code page) are listed under "Regional
settings, not differences" in docs/excel-parity-backlog.md, not fixed.
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECK = re.compile(r'CHECK\(\s*\w+\.Eval\("((?:[^"\\]|\\.)*)"\)\s*==\s*"((?:[^"\\]|\\.)*)"\s*\)')
CELLREF = re.compile(r"(?<![A-Za-z_.])\$?[A-Z]{1,3}\$?[0-9]+(?![0-9A-Za-z_(])|(?<![A-Za-z_])[A-Z]{1,3}:[A-Z]{1,3}(?![A-Za-z_(])")
NOT_ALONE = re.compile(r"\b(RAND|RANDBETWEEN|RANDARRAY|NOW|TODAY|INDIRECT|CELL|INFO|Rate|Band|Table\d|Data|Sheet)\b", re.I)
# disagreements with a reason: (text in the formula, why)
EXPLAINED = [
    ("IMAGE(", "IMAGE reads the web: #BUSY! in a probe"),
    ("DECIMAL", "YCGrid's exact decimals ([YC]); Excel has no such type"),
    ("#BLOCKED!", "the Trust Center's error value can't be typed into Excel"),
    ("DOLLAR(", "the region's negative currency (docs/excel-parity-backlog.md, Regional settings)"),
    ("CHAR(", "the region's code page (docs/excel-parity-backlog.md, Regional settings)"),
    ("CODE(", "the region's code page (docs/excel-parity-backlog.md, Regional settings)"),
    ("(U)", "U is a name the test defines"),
]
ERRORS = {-2146826281: "#DIV/0!", -2146826246: "#N/A", -2146826259: "#NAME?", -2146826288: "#NULL!", -2146826252: "#NUM!",
          -2146826265: "#REF!", -2146826273: "#VALUE!", -2146826256: "#SPILL!", -2146826238: "#CALC!", -2146826237: "#BUSY!"}

PS = r"""param([string]$In, [string]$Out)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W32 { [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p); }
"@
$items = Get-Content -Raw -Encoding UTF8 $In | ConvertFrom-Json
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
  foreach ($f in $items) {
    $kind = ''; $val = ''
    try {
      [void]$ws.Range('Z1000:Z1100').Clear()
      $c = $ws.Range('Z1000')
      $c.Formula2 = '=' + [string]$f
      $v = $c.Value2
      if ($v -is [object[,]]) { $v = $v[1, 1] }
      if ($null -eq $v) { $kind = 'empty' }
      elseif ($v -is [int]) { $kind = 'error'; $val = [string]$v }
      elseif ($v -is [double]) { $kind = 'number'; $val = $v.ToString('R', [Globalization.CultureInfo]::InvariantCulture) }
      elseif ($v -is [bool]) { $kind = 'bool'; $val = [string]$v }
      else { $kind = 'string'; $val = [string]$v }
    } catch { $kind = 'refused'; $val = $_.Exception.Message }
    $sw.WriteLine((ConvertTo-Json -Compress -InputObject ([ordered]@{ kind = $kind; value = $val })))
  }
  $wb.Close($false)
} finally {
  $xl.Quit()
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($xl)
  Start-Sleep 2
  if ($xlpid -ne 0) { Stop-Process -Id $xlpid -Force -ErrorAction SilentlyContinue }
}
"""


def cstr(t: str) -> str:
    """A C++ string literal's text as the string it holds: octal and hex escapes are UTF-8 bytes."""
    out, i, raw = bytearray(), 0, t.encode("utf-8")
    simple = {ord("n"): 10, ord("t"): 9, ord("r"): 13, ord("0"): 0}
    while i < len(raw):
        if raw[i] != 92:
            out.append(raw[i])
            i += 1
            continue
        nx = raw[i + 1]
        if 48 <= nx <= 55:
            j = i + 1
            while j < len(raw) and j < i + 4 and 48 <= raw[j] <= 55:
                j += 1
            out.append(int(raw[i + 1:j], 8) & 0xFF)
            i = j
        elif nx == ord("x"):
            j = i + 2
            while j < len(raw) and chr(raw[j]) in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(raw[i + 2:j], 16) & 0xFF)
            i = j
        else:
            out.append(simple.get(nx, nx))
            i += 2
    return out.decode("utf-8", errors="replace")


def claims(only: str) -> list[dict]:
    out, seen = [], set()
    for path in sorted(glob.glob(os.path.join(ROOT, "tests", "unit", "*.cpp"))):
        if only and only not in os.path.basename(path):
            continue
        text = open(path, encoding="utf-8").read()
        for m in CHECK.finditer(text):
            formula, expected = cstr(m.group(1)), cstr(m.group(2))
            bare = re.sub(r'"[^"]*"', '""', formula)
            if CELLREF.search(bare) or NOT_ALONE.search(bare) or (formula, expected) in seen:
                continue
            seen.add((formula, expected))
            out.append({"where": f"{os.path.relpath(path, ROOT)}:{text.count(chr(10), 0, m.start()) + 1}", "formula": formula,
                        "expected": expected})
    return out


def agrees(expected: str, got: dict) -> bool:
    kind, v = got["kind"], got["value"]
    if kind == "error":
        return ERRORS.get(int(v), v) == expected
    if kind == "bool":
        return expected == ("TRUE" if v == "True" else "FALSE")
    if kind == "string":
        return expected == v
    if kind == "empty":
        return expected in ("", "0")
    if kind == "number":
        try:
            e = float(expected.replace(",", ""))
        except ValueError:
            return False
        x = float(v)
        digits = len(re.sub(r"[^0-9]", "", expected.split("E")[0].lstrip("-0.") or "0"))
        return x == e or math.isclose(x, e, rel_tol=10 ** -(max(digits, 1) - 1) / 2, abs_tol=1e-300)
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="test files whose name contains this")
    a = ap.parse_args()
    items = claims(a.only)
    tmp = tempfile.mkdtemp(prefix="yc_test_formulas_")
    script, inp, out = (os.path.join(tmp, n) for n in ("measure.ps1", "in.json", "out.jsonl"))
    with open(script, "w", encoding="utf-8-sig") as f:
        f.write(PS)
    with open(inp, "w", encoding="utf-8") as f:
        json.dump([i["formula"] for i in items], f, ensure_ascii=False)
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, "-In", inp, "-Out", out],
                   timeout=60 + 2 * len(items), check=False)
    got = [json.loads(line) for line in open(out, encoding="utf-8-sig") if line.strip()]
    sys.stdout.reconfigure(encoding="utf-8")
    wrong, newer, explained = [], [], []
    for it, g in zip(items, got):
        if agrees(it["expected"], g):
            continue
        why = next((w for part, w in EXPLAINED if part in it["formula"]), None)
        if why:
            explained.append((it, why))
        elif g["kind"] == "error" and ERRORS.get(int(g["value"])) == "#NAME?":
            newer.append((it, g))
        else:
            wrong.append((it, g))
    print(f"{len(got)} formulas of the unit tests measured in Excel: {len(wrong)} disagree, {len(explained)} explained, "
          f"{len(newer)} unknown to this Excel")
    for it, g in wrong:
        shown = ERRORS.get(int(g["value"]), g["value"]) if g["kind"] == "error" else g["value"]
        print(f"  {it['where']}  ={it['formula']}  the test says {it['expected']!r}, Excel {g['kind']} {shown!r}")
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
