# SPDX-License-Identifier: Apache-2.0
"""Rounds of random testing, and whether they still find things: the convergence curve.

    python scripts/random_rounds.py run [--rounds N] [--jobs J] [--tools doc_fuzz,chaos,basics,calc_fuzz,formula_fuzz,gaps_fuzz] [--snapshot]
    python scripts/random_rounds.py report

`run` starts N rounds spread over the random tools (fresh seeds, after the last ones logged), J at a time, each against
its own hidden YCGrid (doc_fuzz needs none), and appends one line per round to docs/random-runs.csv: when, tool, seed,
seconds, result. A failed round keeps its log (%TEMP%\\yc_rounds\\<round>.txt) for triage (docs/random-testing.md):
make it a test, fix the cause, then add a line to docs/random-findings.csv with what it was.

`report` reads both files and prints, per tool and over all of them, the CPU hours run, the findings (real ones and the
test tools' own), the findings per 10 hours over time, and whether the stopping rule of docs/random-testing.md holds:
no new real finding in the last 20 hours of rounds and none open.
"""
from __future__ import annotations

import argparse
import csv
import datetime
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(ROOT, "docs", "random-runs.csv")
FINDINGS = os.path.join(ROOT, "docs", "random-findings.csv")
RPY = os.path.join(ROOT, "dist_release", "runtime", "python", "python.exe")
REXE = os.path.join(ROOT, "dist_release", "ycgrid.exe")
DPY = os.path.join(ROOT, "dist_debug", "runtime", "python", "python.exe")
DEXE = os.path.join(ROOT, "dist_debug", "ycgrid.exe")
DOC_FUZZ = os.path.join(ROOT, "build", "msvc-release", "tests", "yc_doc_fuzz.exe")
RUN_FIELDS = ["date", "tool", "seed", "seconds", "result", "note"]
FINDING_FIELDS = ["date", "tool", "seed", "kind", "severity", "status", "summary", "test"]
STOP_HOURS = 20.0  # no new real finding in this many hours of rounds (and none open): converged


def use_snapshot() -> None:
    """Copy the builds the rounds run (dist_release, dist_debug, yc_doc_fuzz) aside and run those: a build started
    meanwhile (a fix) restages dist_* under the running rounds. The copy is refreshed when a build is newer."""
    global RPY, REXE, DPY, DEXE, DOC_FUZZ
    base = os.path.join(tempfile.gettempdir(), "yc_rounds_dist")
    for name in ("dist_release", "dist_debug"):
        src, dst = os.path.join(ROOT, name), os.path.join(base, name)
        stamp = os.path.join(src, "ycgrid.exe")
        if not os.path.exists(os.path.join(dst, "ycgrid.exe")) or os.path.getmtime(stamp) > os.path.getmtime(os.path.join(dst, "ycgrid.exe")):
            print(f"[rounds] copying {name} to {dst}", flush=True)
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(src, dst)
    os.makedirs(os.path.join(base, "fuzz"), exist_ok=True)
    shutil.copy2(DOC_FUZZ, os.path.join(base, "fuzz", "yc_doc_fuzz.exe"))
    RPY = os.path.join(base, "dist_release", "runtime", "python", "python.exe")
    REXE = os.path.join(base, "dist_release", "ycgrid.exe")
    DPY = os.path.join(base, "dist_debug", "runtime", "python", "python.exe")
    DEXE = os.path.join(base, "dist_debug", "ycgrid.exe")
    DOC_FUZZ = os.path.join(base, "fuzz", "yc_doc_fuzz.exe")


def S(name: str) -> str:
    return os.path.join(ROOT, "scripts", name)


def read_csv(path: str) -> list[dict]:
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", newline="") as f:
        return [row for row in csv.DictReader(f) if row.get("date")]


def next_seed(tool: str, start: int) -> int:
    used = [int(r["seed"]) for r in read_csv(RUNS) if r["tool"] == tool and r["seed"].isdigit()]
    return max(used + [start - 1]) + 1


def plan(rounds: int, tools: list[str]) -> list[tuple[str, str, int, list[str]]]:
    """(name, tool, seed, command) for each round, the tools taken in turn."""
    seeds = {"doc_fuzz": next_seed("doc_fuzz", 1000), "chaos": next_seed("chaos", 5000), "basics": next_seed("basics", 2000),
             "calc_fuzz": next_seed("calc_fuzz", 3000), "formula_fuzz": next_seed("formula_fuzz", 4000),
             "gaps_fuzz": next_seed("gaps_fuzz", 100)}
    out = []
    for i in range(rounds):
        tool = tools[i % len(tools)]
        seed = seeds[tool]
        seeds[tool] += 1
        lang = "zh-TW" if seed % 2 else "en"
        if tool == "doc_fuzz":
            # --keep-case: a case that ends the process (the job's memory limit) leaves its stream as
            # %TEMP%\doc_fuzz_current_<seed>.bin
            cmd = [DOC_FUZZ, "--keep-case", "--seed", str(seed * 10000), "--cases", "4000", "--out", tempfile.gettempdir()]  # about a minute
            if seed % 4 == 2:  # the Excel copy's round trip too
                cmd.insert(1, "--xlsx")
            if seed % 2:  # Paste Special, borders, fonts, hidden lines, Replace, Subtotal, outlines, notes
                cmd[1:1] = ["--more", "--xlsx"]
            if seed % 8 == 3:  # the --more operations and the calculation settings (1904, precision as displayed, a
                # sheet's calculation off, an interrupted calculation)
                cmd = [DOC_FUZZ, "--gaps", "--xlsx", "--keep-case", "--seed", str(seed * 10000), "--cases", "4000", "--out", tempfile.gettempdir()]
            if seed % 4 == 0:  # whole columns, rows and the sheet now and then: slower cases, so a minute of them
                cmd = [DOC_FUZZ, "--wide", "--keep-case", "--seed", str(seed * 10000), "--seconds", "90", "--out", tempfile.gettempdir()]
                if seed % 8 == 0:  # with the twelve more operations and ranges to the sheet's last cell
                    cmd.insert(2, "--more")
        elif tool == "chaos":
            cmd = [DPY, "-E", "-s", S("chaos.py"), "--seed", str(seed), "--lang", lang, "--minutes", "6", "--exe", DEXE]
        elif tool == "basics":
            # sequences seed*100 .. seed*100+59: a round's sequences are seeds of their own, and the next round's don't
            # overlap them (with --seed seed, round s ran s..s+59, so one round in sixty tried a new sequence)
            cmd = [RPY, "-E", "-s", S("basics.py"), "--exe", REXE, "--sequences", "60", "--seed", str(seed * 100), "--lang", lang]
        elif tool == "calc_fuzz":
            mode = ("recalc", "undo", "files")[seed % 3]
            cmd = [RPY, "-E", "-s", S("calc_fuzz.py"), "--mode", mode, "--seed", str(seed), "--steps", {"recalc": "400", "undo": "200", "files": "40"}[mode],
                   "--exe", REXE]
        elif tool == "gaps_fuzz":
            # rounds seed*1000 .. seed*1000+599: the next round's don't overlap them
            cmd = [RPY, "-E", "-s", S("gaps_fuzz.py"), "--exe", REXE, "--seed", str(seed * 1000), "--rounds", "600", "--lang", lang]
        else:
            cmd = [RPY, "-E", "-s", S("formula_fuzz.py"), "--seed", str(seed), "--n", "600", "--eval", "--exe", REXE]
        out.append((f"{tool}_{seed}", tool, seed, cmd))
    return out


ROUND_MEMORY_GB = 6  # each process of a round (the tool, and the YCGrid it starts) at most


def run_capped(cmd, cwd, env, stdout, timeout):
    """Run cmd inside a Windows job object that limits every process in it to ROUND_MEMORY_GB: past it an allocation
    fails in that process, which ends the round as a failure instead of taking the machine's memory. Two --wide
    doc_fuzz rounds held 80 GB each, Windows ran out of memory, and a YCGrid beside them died of an uncaught bad_alloc.
    The return code, and the peak memory one process of the job reached."""
    import ctypes
    from ctypes import wintypes as w

    class Basic(ctypes.Structure):
        _fields_ = [("a", ctypes.c_longlong), ("b", ctypes.c_longlong), ("LimitFlags", w.DWORD), ("c", ctypes.c_size_t),
                    ("d", ctypes.c_size_t), ("e", w.DWORD), ("f", ctypes.c_size_t), ("g", w.DWORD), ("h", w.DWORD)]

    class Extended(ctypes.Structure):
        _fields_ = [("Basic", Basic), ("Io", ctypes.c_ulonglong * 6), ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateJobObjectW.restype = w.HANDLE
    k.OpenProcess.restype = w.HANDLE
    job = k.CreateJobObjectW(None, None)
    info = Extended()
    info.Basic.LimitFlags = 0x100 | 0x2000  # JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    info.ProcessMemoryLimit = int(ROUND_MEMORY_GB * (1 << 30))
    k.SetInformationJobObject(w.HANDLE(job), 9, ctypes.byref(info), ctypes.sizeof(info))
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=stdout, stderr=subprocess.STDOUT)
    h = k.OpenProcess(0x1F0FFF, False, p.pid)
    k.AssignProcessToJobObject(w.HANDLE(job), w.HANDLE(h))
    k.CloseHandle(w.HANDLE(h))
    try:
        rc = p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        raise
    finally:
        k.QueryInformationJobObject(w.HANDLE(job), 9, ctypes.byref(info), ctypes.sizeof(info), None)
        peak = info.PeakProcessMemoryUsed / (1 << 30)
        k.CloseHandle(w.HANDLE(job))  # ends whatever of the round is still running
    return rc, peak


def run_round(name, tool, seed, cmd, lock, logdir):
    tmp = tempfile.mkdtemp(prefix="yc_round_")
    env = dict(os.environ, TEMP=tmp, TMP=tmp, PYTHONIOENCODING="utf-8")
    t0 = time.time()
    log = os.path.join(logdir, name + ".txt")
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        try:
            rc, peak = run_capped(cmd, ROOT, env, f, 3000)
            if peak > ROUND_MEMORY_GB * 0.95:
                f.write(f"\n[rounds] a process reached the {ROUND_MEMORY_GB} GB limit ({peak:.2f} GB)\n")
        except subprocess.TimeoutExpired:
            rc = -9
            f.write("\n[rounds] timed out\n")
    shutil.rmtree(tmp, ignore_errors=True)
    seconds = time.time() - t0
    with lock:
        print(f"{'ok  ' if rc == 0 else 'FAIL'} {name} ({seconds:.0f} s){'' if rc == 0 else '  log: ' + log}", flush=True)
        new = not os.path.exists(RUNS)
        with open(RUNS, "a", encoding="utf-8", newline="") as out:
            w = csv.DictWriter(out, RUN_FIELDS, lineterminator="\n")
            if new:
                w.writeheader()
            w.writerow({"date": datetime.date.today().isoformat(), "tool": tool, "seed": seed, "seconds": f"{seconds:.0f}",
                        "result": "ok" if rc == 0 else "fail", "note": "" if rc == 0 else os.path.basename(log)})


def cmd_run(a):
    tools = [t for t in a.tools.split(",") if t]
    missing = [p for p, needed in ((DOC_FUZZ, "doc_fuzz"), (REXE, "basics"), (DEXE, "chaos")) if needed in tools and not os.path.exists(p)]
    if missing:
        sys.exit(f"build first (scripts\\build.cmd release /silent, and debug for chaos): {missing}")
    if a.snapshot:
        use_snapshot()
    logdir = os.path.join(tempfile.gettempdir(), "yc_rounds")
    os.makedirs(logdir, exist_ok=True)
    items = plan(a.rounds, tools)
    lock = threading.Lock()
    running: list[threading.Thread] = []
    while items or running:
        running = [t for t in running if t.is_alive()]
        while items and len(running) < a.jobs:
            t = threading.Thread(target=run_round, args=(*items.pop(0), lock, logdir), daemon=True)
            t.start()
            running.append(t)
        time.sleep(0.5)
    print("done; python scripts/random_rounds.py report")


def cmd_report(_a) -> int:
    runs, findings = read_csv(RUNS), read_csv(FINDINGS)
    hours: dict[str, float] = {}
    for r in runs:
        hours[r["tool"]] = hours.get(r["tool"], 0.0) + float(r["seconds"] or 0) / 3600
    total = sum(hours.values())
    real = [f for f in findings if f["kind"] == "real"]
    print(f"{'tool':14} {'hours':>7} {'rounds':>7} {'failed':>7} {'real':>5} {'tool':>5}")
    for tool in sorted(set(hours) | {f["tool"] for f in findings}):
        rs = [r for r in runs if r["tool"] == tool]
        fs = [f for f in findings if f["tool"] == tool]
        print(f"{tool:14} {hours.get(tool, 0):7.1f} {len(rs):7d} {sum(r['result'] != 'ok' for r in rs):7d} "
              f"{sum(f['kind'] == 'real' for f in fs):5d} {sum(f['kind'] == 'tool' for f in fs):5d}")
    print(f"{'all':14} {total:7.1f} {len(runs):7d} {sum(r['result'] != 'ok' for r in runs):7d} {len(real):5d} "
          f"{sum(f['kind'] == 'tool' for f in findings):5d}")
    # the curve: real findings per 10 hours, by day
    by_day: dict[str, list[float]] = {}
    for r in runs:
        by_day.setdefault(r["date"], [0.0, 0])[0] += float(r["seconds"] or 0) / 3600
    for f in real:
        by_day.setdefault(f["date"], [0.0, 0])[1] += 1
    print("\nreal findings per 10 hours of rounds, by day:")
    for day in sorted(by_day):
        h, n = by_day[day]
        rate = f"{10 * n / h:6.1f}" if h > 0 else "     -"
        print(f"  {day}  {h:6.1f} h  {n:3d} found  {rate}")
    # the stopping rule: STOP_HOURS of rounds since the last real finding, and none open
    # kept: decided to stay, with its reason under "Kept on purpose" in docs/excel-parity-backlog.md
    open_ = [f for f in findings if f["status"] not in ("fixed", "kept")]
    # the hours after the round that found the last real finding. A finding names its round by tool and seed (a
    # doc_fuzz stream's seed is its round's times 10000 plus the case); one no round names (found by hand) counts from
    # the day after it. By day alone, every round of the day a finding was made counted 0 hours, however clean
    def round_of(f) -> int:
        try:
            seed = int(f["seed"])
        except ValueError:
            return -1
        for i in range(len(runs) - 1, -1, -1):
            r = runs[i]
            if r["tool"] != f["tool"] or r["result"] == "ok":
                continue
            try:
                rs = int(r["seed"])
            except ValueError:
                continue
            if rs == seed or (f["tool"] == "doc_fuzz" and rs == seed // 10000):
                return i
        return -1
    # findings are written in the order they were found: one no round names (found by hand, beside the rounds) is
    # placed at the round of the finding before it, or at the end of its day's rounds when none before it is placed
    start, anchor = 0, -1
    for f in findings:
        i = round_of(f)
        if i >= 0:
            anchor = max(anchor, i)
        if f["kind"] != "real":
            continue
        if i < 0:
            i = anchor if anchor >= 0 else next((k for k, r in enumerate(runs) if r["date"] > f["date"]), len(runs)) - 1
        start = max(start, i + 1)
    since = sum(float(r["seconds"] or 0) / 3600 for r in runs[start:]) if real else total
    print(f"\nopen findings: {len(open_)}" + "".join(f"\n  {f['date']} {f['tool']} {f['seed']}: {f['summary']}" for f in open_))
    done = since >= STOP_HOURS and not open_
    print(f"hours of rounds since the last real finding: {since:.1f} (the rule asks {STOP_HOURS:.0f}) -> "
          f"{'converged: random testing stops finding things' if done else 'not converged: keep fixing classes, no new features (AGENTS.md)'}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--rounds", type=int, default=20)
    r.add_argument("--jobs", type=int, default=6)
    r.add_argument("--tools", default="doc_fuzz,chaos,basics,calc_fuzz,formula_fuzz,gaps_fuzz")
    r.add_argument("--snapshot", action="store_true", help="run copies of the builds, so a build can run meanwhile")
    sub.add_parser("report")
    a = ap.parse_args()
    if a.cmd == "run":
        cmd_run(a)
        return 0
    return cmd_report(a)


if __name__ == "__main__":
    sys.exit(main())
