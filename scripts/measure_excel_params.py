# SPDX-License-Identifier: Apache-2.0
r"""The names and order of the arguments of Excel's methods (and of its properties that take arguments), for the
objects named in schema/excel_parity.toml, read from Excel's primary interop assembly by reflection and written to
tests/data/excel_method_params.json.

    <YCGrid>\runtime\python\python.exe -E -s scripts\measure_excel_params.py

Nothing is started: Windows PowerShell loads Microsoft.Office.Interop.Excel (installed with Office) and lists each
interface's methods and their parameters. scripts\check_excel_parity.py checks that YCGrid's Python members take the
same arguments in the same order, so a call written for Excel with its arguments by position (Range.Sort's MatchCase
and Orientation, Workbook.Close's Filename) works.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA = os.path.join(ROOT, "schema", "excel_parity.toml")
OUT = os.path.join(ROOT, "tests", "data", "excel_method_params.json")

SCRIPT = r"""
$asm = [System.Reflection.Assembly]::LoadWithPartialName("Microsoft.Office.Interop.Excel")
if ($null -eq $asm) { Write-Error "Microsoft.Office.Interop.Excel is not installed"; exit 1 }
$res = @{}
foreach ($n in (Get-Content -LiteralPath $args[0])) {
  $t = $asm.GetType("Microsoft.Office.Interop.Excel." + $n)
  if ($null -eq $t) { continue }
  $m = @{}
  foreach ($mi in $t.GetMethods()) {
    $nm = $mi.Name
    if ($nm.StartsWith("set_") -or $nm.StartsWith("let_") -or $nm.StartsWith("add_") -or $nm.StartsWith("remove_")) { continue }
    if ($nm.StartsWith("get_")) { $nm = $nm.Substring(4) }
    $ps = @($mi.GetParameters() | ForEach-Object { $_.Name })
    if ($ps.Count -eq 0) { continue }
    if (-not $m.ContainsKey($nm) -or $ps.Count -gt $m[$nm].Count) { $m[$nm] = $ps }
  }
  $res[$n] = $m
}
$res | ConvertTo-Json -Depth 5 -Compress | Set-Content -Encoding UTF8 -LiteralPath $args[1]
"""


def main():
    with open(SCHEMA, "rb") as f:
        schema = tomllib.load(f)
    names = sorted({spec["tlb"] for spec in schema["objects"].values()})
    with tempfile.TemporaryDirectory() as tmp:
        lst, ps1, raw = (os.path.join(tmp, n) for n in ("names.txt", "dump.ps1", "params.json"))
        with open(lst, "w", encoding="utf-8") as f:
            f.write("\n".join(names))
        with open(ps1, "w", encoding="utf-8-sig") as f:
            f.write(SCRIPT)
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1, lst, raw], check=True)
        with open(raw, encoding="utf-8-sig") as f:
            data = json.load(f)
    out = {obj: {m: list(p) if isinstance(p, list) else [p] for m, p in sorted(members.items())} for obj, members in sorted(data.items())}
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"source": "Microsoft.Office.Interop.Excel (reflection)", "objects": out}, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"wrote {os.path.relpath(OUT, ROOT)}: {sum(len(v) for v in out.values())} members of {len(out)} objects")
    return 0


if __name__ == "__main__":
    sys.exit(main())
