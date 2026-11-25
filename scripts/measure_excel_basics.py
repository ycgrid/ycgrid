# SPDX-License-Identifier: Apache-2.0
"""Measure the basic operations of scripts/excel_basics_cases.py in Excel and write tests/data/excel_basics_measured.json.

Run with the bundled Python (no pywin32 needed):
    dist_release\\runtime\\python\\python.exe -E -s scripts\\measure_excel_basics.py [--only ID_PREFIX] [--chunk N]

Each chunk of cases runs in a PowerShell script written to %TEMP%: it starts a hidden Excel of its own
(New-Object -ComObject Excel.Application never attaches to one the user runs), notes that Excel's process id, and quits
and kills only that process at the end. A chunk that runs past its time limit is stopped and its Excel killed by that
process id; the cases after the one that hung run again in a new chunk. Formula and NumberFormat are read and written
with the en-US culture (InvokeMember), so the codes are US Excel's whatever the region.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import excel_basics_cases as cases_mod  # noqa: E402
from excel_basics_cases import col_name, plan, plan_hash, window_cells  # noqa: E402

FIXTURE = os.path.join(ROOT, "tests", "data", "excel_basics_measured.json")
ERR_TEXT = {2000: "#NULL!", 2007: "#DIV/0!", 2015: "#VALUE!", 2023: "#REF!", 2029: "#NAME?", 2036: "#NUM!", 2042: "#N/A",
            2043: "#GETTING_DATA", 2045: "#SPILL!", 2047: "#BLOCKED!", 2050: "#CALC!"}

PS_SCRIPT = r'''
param([string]$In, [string]$Out, [string]$PidFile)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W32 { [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid); }
"@
$en = [Globalization.CultureInfo]::GetCultureInfo('en-US')
$inv = [Globalization.CultureInfo]::InvariantCulture
$BF = [Reflection.BindingFlags]
$CT = [System.__ComObject]
function SetP($o, [string]$n, $v) { [void]$CT.InvokeMember($n, $BF::SetProperty, $null, $o, [object[]]@(, $v), $en) }
function GetP($o, [string]$n) { $x = $CT.InvokeMember($n, $BF::GetProperty, $null, $o, $null, $en); return , $x }
function Rel($o) { if ($null -ne $o) { [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($o) } }
function CallM($o, [string]$n, [object[]]$a, [string[]]$names) {
  if ($a.Count -eq 0) { return $CT.InvokeMember($n, $BF::InvokeMethod, $null, $o, $null, $en) }
  return $CT.InvokeMember($n, $BF::InvokeMethod, $null, $o, $a, $null, $en, $names)
}
function Tag($x) {
  if ($null -eq $x -or $x -is [DBNull]) { return @('', '') }
  if ($x -is [double]) { return @('n', $x.ToString('R', $inv)) }
  if ($x -is [string]) { return @('s', $x) }
  if ($x -is [bool]) { if ($x) { return @('b', '1') } else { return @('b', '0') } }
  if ($x -is [int]) { return @('e', [string]([int64]$x + 2146828288)) }
  return @('?', [string]$x)
}
$data = Get-Content -Raw -Encoding UTF8 $In | ConvertFrom-Json
$sw = New-Object IO.StreamWriter($Out, $true, (New-Object Text.UTF8Encoding($false)))
$sw.AutoFlush = $true
$xl = New-Object -ComObject Excel.Application
$xlpid = [uint32]0
[void][W32]::GetWindowThreadProcessId([IntPtr]$xl.Hwnd, [ref]$xlpid)
Set-Content -Path $PidFile -Value ([string]$xlpid) -Encoding ASCII
try {
  $xl.DisplayAlerts = $false
  $xl.ScreenUpdating = $false
  $sw.WriteLine((ConvertTo-Json -Compress -InputObject ([ordered]@{ meta = [ordered]@{ version = [string]$xl.Version; build = [string]$xl.Build } })))
  $wbs = $xl.Workbooks; $wb = $wbs.Add(); $sheets = $wb.Worksheets; $names = $wb.Names
  $other = $sheets.Item(1); $other.Name = 'Other'
  foreach ($p in $data.other_static) { $c = $other.Range([string]$p[0]); SetP $c 'Formula' ([string]$p[1]); Rel $c }
  $haveT = $false
  $ws = $null
  function Rng([string]$addr) {
    if ($addr.Contains('!')) {
      $parts = $addr.Split('!'); $sh = $sheets.Item($parts[0]); $r = $sh.Range($parts[1]); Rel $sh; return , $r
    }
    $r = $ws.Range($addr); return , $r
  }
  foreach ($case in $data.cases) {
    $id = [string]$case.id
    $err = ''
    try {
      if ($haveT) { $t = $sheets.Item('T'); [void]$t.Delete(); Rel $t; $haveT = $false }
      $last = $sheets.Item($sheets.Count); $ws = $sheets.Add([Reflection.Missing]::Value, $last); Rel $last
      $ws.Name = 'T'; $haveT = $true
      $c = $other.Range([string]$data.other_clear); [void]$c.Clear(); Rel $c
      foreach ($p in $case.cells) {
        $c = $ws.Range([string]$p[0])
        if ($null -ne $p[2]) { SetP $c 'NumberFormat' ([string]$p[2]) }
        SetP $c 'Formula2' ([string]$p[1]); Rel $c
      }
      foreach ($p in $case.names) { $n = CallM $names 'Add' @([string]$p[0], [string]$p[1]) @('Name', 'RefersTo'); Rel $n }
      foreach ($p in $case.other) { $c = $other.Range([string]$p[0]); SetP $c 'Formula2' ([string]$p[1]); Rel $c }
    } catch { $err = 'setup: ' + $_.Exception.Message }
    $refused = ''
    if ($err -eq '') {
      $step = 0
      foreach ($s in $case.steps) {
        $step++
        $keep = @()
        $target = $null; $own = $true
        try {
          if ([string]$s[1] -eq '#sheet') { $target = $ws; $own = $false } else { $target = Rng ([string]$s[1]) }
          $mname = [string]$s[2]
          while ($s[0] -ne 'set' -and $mname.Contains('.')) {
            $dot = $mname.IndexOf('.'); $next = GetP $target $mname.Substring(0, $dot)
            if ($own) { Rel $target }
            $target = $next; $own = $true; $mname = $mname.Substring($dot + 1)
          }
          if ($s[0] -eq 'set') {
            $v = $s[3]; $prop = [string]$s[2]
            if ($prop -eq 'Formula') { $prop = 'Formula2' }
            SetP $target $prop $v
          } else {
            $a = @(); $nm = @()
            foreach ($prop in $s[3].PSObject.Properties) {
              $v = $prop.Value
              if ($v -is [pscustomobject]) { $v = Rng ([string]$v.range); $keep += , $v }
              $a += , $v; $nm += [string]$prop.Name
            }
            $res = CallM $target $mname ([object[]]$a) ([string[]]$nm)
            if ($res -is [System.__ComObject]) { Rel $res }
          }
        } catch {
          $m = $_.Exception.Message
          if ($_.Exception.InnerException) { $m = $_.Exception.InnerException.Message }
          $refused = "step ${step}: " + $m
        }
        foreach ($k in $keep) { Rel $k }
        if ($own) { Rel $target }
        if ($refused -ne '') { break }
      }
      try { SetP $xl 'CutCopyMode' $false } catch { }
    }
    $cells = New-Object System.Collections.Generic.List[object]
    $nameOut = [ordered]@{}
    if ($err -eq '') {
      try {
        foreach ($rd in $case.read) {
          $r = Rng ([string]$rd.range)
          $f = GetP $r 'Formula2'
          $v = GetP $r 'Value2'
          $nfAll = GetP $r 'NumberFormat'
          $nr = [int]$rd.rows; $nc = [int]$rd.cols
          for ($i = 0; $i -lt $nr; $i++) {
            $rowFmt = $null
            if (-not ($nfAll -is [string])) {
              $rr = Rng ([string]$rd.row_ranges[$i]); $rowFmt = GetP $rr 'NumberFormat'; Rel $rr
            }
            for ($j = 0; $j -lt $nc; $j++) {
              $addr = [string]$rd.cells[$i][$j]
              if ($nr -eq 1 -and $nc -eq 1) { $fx = $f; $vx = $v } else { $fx = $f[($i + 1), ($j + 1)]; $vx = $v[($i + 1), ($j + 1)] }
              if ($nfAll -is [string]) { $fmt = $nfAll } elseif ($rowFmt -is [string]) { $fmt = $rowFmt } else {
                $cc = Rng $addr; $fmt = [string](GetP $cc 'NumberFormat'); Rel $cc
              }
              $mg = ''
              if ($case.merge) { $cc = Rng $addr; $mg = [string](GetP $cc 'MergeCells'); Rel $cc }
              $tv = Tag $vx
              $cells.Add([object[]]@($addr, [string]$fx, $tv[0], $tv[1], [string]$fmt, $mg))
            }
          }
          Rel $r
        }
        foreach ($p in $case.names) {
          $n = CallM $names 'Item' @([string]$p[0]) @('Index'); $nameOut[[string]$p[0]] = [string](GetP $n 'RefersTo'); Rel $n
        }
      } catch { $err = 'read: ' + $_.Exception.Message }
    }
    $sw.WriteLine((ConvertTo-Json -Compress -Depth 6 -InputObject ([ordered]@{ id = $id; error = $err; refused = $refused; cells = $cells; names = $nameOut })))
    Rel $ws; $ws = $null
  }
  $wb.Close($false)
  Rel $other; Rel $names; Rel $sheets; Rel $wb; Rel $wbs
} finally {
  $sw.Close()
  try { $xl.Quit() } catch { }
  Rel $xl; $xl = $null
  [GC]::Collect(); [GC]::WaitForPendingFinalizers(); [GC]::Collect()
  for ($k = 0; $k -lt 20; $k++) { if (-not (Get-Process -Id $xlpid -ErrorAction SilentlyContinue)) { break }; Start-Sleep -Milliseconds 500 }
  if (Get-Process -Id $xlpid -ErrorAction SilentlyContinue) { Stop-Process -Id $xlpid -Force }
}
'''


def ps_input(plans):
    out = []
    for p in plans:
        reads = []
        for rng in p["read"]:
            sheet, r0, c0, nr, nc, grid = window_cells(rng)
            prefix = sheet + "!" if sheet != "T" else ""
            rows = [f"{prefix}{col_name(c0)}{r0 + i + 1}:{col_name(c0 + nc - 1)}{r0 + i + 1}" for i in range(nr)]
            reads.append({"range": rng, "rows": nr, "cols": nc, "cells": grid, "row_ranges": rows})
        out.append({"id": p["id"], "cells": p["cells"], "names": p["names"], "other": p["other"], "steps": p["steps"],
                    "read": reads, "merge": p["merge"]})
    return {"cases": out, "other_static": [[a, t] for a, t in cases_mod.OTHER_STATIC.items()], "other_clear": cases_mod.OTHER_CLEAR}


def decode(line):
    """A result line -> the fixture's form: only cells that aren't empty General (or are merged)."""
    cells = {}
    for addr, formula, tag, val, fmt, merge in line["cells"]:
        if tag == "n":
            v = float(val)
            if v.is_integer() and abs(v) < 2 ** 53:
                v = int(v)
        elif tag == "s":
            v = val
        elif tag == "b":
            v = val == "1"
        elif tag == "e":
            v = {"err": ERR_TEXT.get(int(val), "#ERR" + val)}
        else:
            v = None
        merged = merge == "True"
        if formula == "" and v is None and fmt == "General" and not merged:
            continue
        entry = [formula, v, fmt]
        if merge:
            entry.append(merged)
        cells[addr] = entry
    return {"cells": cells, "names": line.get("names") or {}, "refused": line.get("refused") or "", "error": line.get("error") or ""}


def kill_pid(pid):
    if pid:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)


def run_chunk(plans, timeout, log):
    tmp = tempfile.mkdtemp(prefix="yc_measure_")
    script = os.path.join(tmp, "measure.ps1")
    with open(script, "w", encoding="utf-8-sig") as f:
        f.write(PS_SCRIPT)
    inp, out, pidf = (os.path.join(tmp, n) for n in ("in.json", "out.jsonl", "pid.txt"))
    with open(inp, "w", encoding="utf-8") as f:
        json.dump(ps_input(plans), f)
    proc = subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, "-In", inp, "-Out", out,
                             "-PidFile", pidf], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        so, _ = proc.communicate(timeout=timeout)
        if proc.returncode:
            log(f"  powershell exit {proc.returncode}: {so.decode('utf-8', 'replace')[-2000:]}")
    except subprocess.TimeoutExpired:
        log(f"  chunk ran past {timeout}s: stopping it")
        proc.kill()
        proc.communicate()
    finally:
        pid = None
        try:
            pid = int(open(pidf).read().strip())
        except (OSError, ValueError):
            pass
        # the script quits its Excel itself; one still running (a hang or a kill) is stopped by its own process id
        time.sleep(1)
        if pid and subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True).stdout.count(str(pid)):
            log(f"  killing our Excel {pid}")
            kill_pid(pid)
    meta, results = {}, {}
    if os.path.exists(out):
        for raw in open(out, encoding="utf-8"):
            raw = raw.strip()
            if not raw:
                continue
            line = json.loads(raw)
            if "meta" in line:
                meta = line["meta"]
            else:
                results[line["id"]] = decode(line)
    log(f"  {len(results)}/{len(plans)} cases in {time.time() - t0:.0f}s")
    return meta, results


def dump_fixture(meta, plans, results):
    head = {
        "comment": "Generated by scripts/measure_excel_basics.py from scripts/excel_basics_cases.py: basic operations measured "
                   "in Excel. Do not edit by hand; change the cases and measure again.",
        "date": datetime.date.today().isoformat(),
        "excel_version": meta.get("version", ""),
        "excel_build": meta.get("build", ""),
        "regional_settings": "Taiwan (zh-TW), Traditional Chinese UI; Formula and NumberFormat read in en-US",
        "cell_form": "[Formula (Excel's Formula2: YCGrid's Formula is Formula2), Value2, NumberFormat(, MergeCells)]; cells of the windows not listed are empty General",
    }
    lines = ["{"]
    for k, v in head.items():
        lines.append(f"  {json.dumps(k)}: {json.dumps(v)},")
    lines.append('  "cases": [')
    rows = []
    for p in plans:
        if p["id"] not in results:
            continue
        r = results[p["id"]]
        rec = {"id": p["id"], "hash": plan_hash(p), "read": p["read"], "skip": p["skip"], "refused": r["refused"], "error": r["error"],
               "names": r["names"], "cells": r["cells"]}
        rows.append("    " + json.dumps(rec, ensure_ascii=True))
    lines.append(",\n".join(rows))
    lines.append("  ]")
    lines.append("}")
    os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
    with open(FIXTURE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="measure only the cases whose id starts with this (kept with the others measured before)")
    ap.add_argument("--chunk", type=int, default=50)
    ap.add_argument("--timeout", type=int, default=420, help="seconds a chunk may run")
    args = ap.parse_args()

    def log(m):
        print(m, flush=True)

    plans = [plan(c) for c in cases_mod.CASES]
    todo = [p for p in plans if p["id"].startswith(tuple(args.only.split(",")))]
    results, meta = {}, {}
    if args.only and os.path.exists(FIXTURE):  # keep the cases measured before
        old = json.load(open(FIXTURE, encoding="utf-8"))
        meta = {"version": old.get("excel_version", ""), "build": old.get("excel_build", "")}
        hashes = {p["id"]: plan_hash(p) for p in plans}
        for rec in old["cases"]:
            if hashes.get(rec["id"]) == rec["hash"]:
                results[rec["id"]] = rec
    hung = []
    queue = list(todo)
    while queue:
        chunk, queue = queue[:args.chunk], queue[args.chunk:]
        log(f"chunk of {len(chunk)} from {chunk[0]['id']} ({len(queue)} left after it)")
        m, got = run_chunk(chunk, args.timeout, log)
        meta = m or meta
        results.update(got)
        missing = [p for p in chunk if p["id"] not in got]
        if missing:
            hung.append(missing[0]["id"])  # the case that was running when the chunk stopped
            log(f"  no result for {missing[0]['id']}: left out")
            queue = missing[1:] + queue
    dump_fixture(meta, plans, results)
    log(f"wrote {FIXTURE}: {sum(1 for p in plans if p['id'] in results)} cases; hung: {hung}")


if __name__ == "__main__":
    main()
