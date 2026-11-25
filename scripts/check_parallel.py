# SPDX-License-Identifier: Apache-2.0
r"""Runs the slow checks side by side: each job is its own process (and its own hidden YCGrid), so they don't wait for
one another.

    <YCGrid>\runtime\python\python.exe -E -s scripts\check_parallel.py [--quick | --chaos] [--jobs N] [--only name,...] [--list]

Jobs:
- python:<module or module.Class>  the Python tests, one job per module (test_live: one per test class)
- basics:K/N                       scripts\basics.py's cases K, K+N...: every basic operation in every state (also
                                   with --quick: the basic operations come first)
- basics:seqN                      scripts\basics.py --sequences: 120 random sequences of eight operations each
- basics_zh:K/3, basics_zh:seq7     the same in the Traditional Chinese UI (--lang zh-TW)
- basics_cmd:K/4                    every command of the Home tab in the basic states (basics.py --commands)
- basics_tabs:K/4                   every command of the other tabs in the basic states (--commands --tab others)
- feature:K/N                      feature_check sections K, K+N, K+2N... then one coverage check over all of them
- ui_sweep:commands, :dialogs      ui_sweep's commands, then its dialogs and examples, side by side
- fuzz:*, formula_fuzz             as in check_all.cmd
- doc_fuzz                         the Document in-process: tests/fuzz/corpus, then 120 fixed seeds
- doc_fuzz_wide, _more, _wide_more the streams of tests/fuzz/corpus/wide, /more and /wide_more, with their flags
- doc_fuzz_gaps                    the calculation settings (1904, precision as displayed, a sheet's calculation, an
                                   interrupted calculation): tests/fuzz/corpus/gaps, then 120 fixed seeds with --gaps
- gaps_fuzz                        the object model's members of the Excel gap list, 400 fixed rounds, each checked by
                                   its result (pivot areas and values, panes, OpenText, DisplayFormat...)
- trader_session                   timed (frozen screens, lost clicks): runs alone at the end, after the others
- chaos:<seed>                     --chaos: every seed of scripts\chaos_seeds.txt on the debug build (dist_debug)
Each job gets its own TEMP folder (hidden instances put their unsaved projects there), and its own log:
%TEMP%\yc_check_<job>.txt. Exit code 1 when a job failed; the summary names the failed jobs and their logs.
"""
from __future__ import annotations

import argparse
import ast
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PY = os.path.join(ROOT, "dist_release", "runtime", "python", "python.exe")
EXE = os.path.join(ROOT, "dist_release", "ycgrid.exe")
# every python/tests/test_*.py module, picked up by itself (a new module or class needs no change here)
MODULES = sorted(f[:-3] for f in os.listdir(os.path.join(ROOT, "python", "tests")) if f.startswith("test_") and f.endswith(".py"))
FEATURE_SHARDS = 6
BASICS_SHARDS = 6


def test_classes(module: str) -> list[str]:
    """The TestCase classes of a test module (read from the source: importing would start nothing, but is slower)."""
    path = os.path.join(ROOT, "python", "tests", module + ".py")
    tree = ast.parse(open(path, encoding="utf-8").read())
    return [n.name for n in tree.body if isinstance(n, ast.ClassDef) and any(
        (isinstance(b, ast.Attribute) and b.attr == "TestCase") or (isinstance(b, ast.Name) and b.id.endswith("TestCase")) for b in n.bases)]


def jobs(quick: bool) -> tuple[list[tuple[str, list[str], str]], list[tuple[str, list[str], str]]]:
    """(parallel jobs, jobs run alone afterwards): (name, command, working directory)."""
    py = [PY, "-E", "-s"]
    par = []
    for m in MODULES:
        if m == "test_live":
            for c in test_classes(m):
                par.append((f"python:{m}.{c}", py + ["-m", "unittest", f"tests.{m}.{c}"], os.path.join(ROOT, "python")))
        else:
            par.append((f"python:{m}", py + ["-m", "unittest", f"tests.{m}"], os.path.join(ROOT, "python")))
    for k in range(BASICS_SHARDS):
        par.append((f"basics:{k}/{BASICS_SHARDS}", py + [os.path.join(ROOT, "scripts", "basics.py"), "--exe", EXE, "--shard", f"{k}/{BASICS_SHARDS}"], ROOT))
    # and random sequences of eight operations, undone and redone (two seeds side by side)
    for seed in (1, 1001):
        par.append((f"basics:seq{seed}", py + [os.path.join(ROOT, "scripts", "basics.py"), "--exe", EXE, "--sequences", "120", "--seed", str(seed)], ROOT))
    # every command of the Home tab, and of the other tabs, in the basic states (four jobs each)
    for k in range(4):
        par.append((f"basics_cmd:{k}/4", py + [os.path.join(ROOT, "scripts", "basics.py"), "--exe", EXE, "--commands", "--shard", f"{k}/4"], ROOT))
        par.append((f"basics_tabs:{k}/4", py + [os.path.join(ROOT, "scripts", "basics.py"), "--exe", EXE, "--commands", "--tab", "others",
                                               "--shard", f"{k}/4"], ROOT))
    # the same in the Traditional Chinese UI people use: half the matrix's cases each (K, K+2...) and one seed
    for k in range(3):
        par.append((f"basics_zh:{k}/3", py + [os.path.join(ROOT, "scripts", "basics.py"), "--exe", EXE, "--lang", "zh-TW", "--shard", f"{k}/3"], ROOT))
    par.append(("basics_zh:seq7", py + [os.path.join(ROOT, "scripts", "basics.py"), "--exe", EXE, "--lang", "zh-TW", "--sequences", "120", "--seed", "7"], ROOT))
    if quick:
        return par, []
    cov = os.path.join(tempfile.gettempdir(), "yc_check_covered")
    for k in range(FEATURE_SHARDS):
        par.append((f"feature:{k}/{FEATURE_SHARDS}", py + [os.path.join(ROOT, "scripts", "feature_check.py"), "--exe", EXE,
                                                           "--shard", f"{k}/{FEATURE_SHARDS}", "--covered-out", f"{cov}{k}.txt"], ROOT))
    # the sweep in two jobs side by side: every command, then every dialog and the examples (one job took close to
    # the 60 minutes a job may run)
    par.append(("ui_sweep:commands", py + [os.path.join(ROOT, "scripts", "ui_sweep.py"), "commands", "--exe", EXE], ROOT))
    par.append(("ui_sweep:dialogs", py + [os.path.join(ROOT, "scripts", "ui_sweep.py"), "dialogs", "examples", "--exe", EXE], ROOT))
    fz = os.path.join(ROOT, "scripts", "calc_fuzz.py")
    par.append(("fuzz:recalc", py + [fz, "--mode", "recalc", "--seed", "1", "--steps", "400", "--exe", EXE], ROOT))
    par.append(("fuzz:undo", py + [fz, "--mode", "undo", "--seed", "7", "--steps", "200", "--exe", EXE], ROOT))
    par.append(("fuzz:files", py + [fz, "--mode", "files", "--seed", "2", "--steps", "40", "--exe", EXE], ROOT))
    par.append(("formula_fuzz", py + [os.path.join(ROOT, "scripts", "formula_fuzz.py"), "--seed", "1", "--n", "500", "--exe", EXE], ROOT))
    # the object-model members of the Excel gap list, every round checked by its result, in both UI languages (its
    # findings are unit and live tests of their own: test_excel_gaps.cpp, test_live_gaps.py)
    gz = os.path.join(ROOT, "scripts", "gaps_fuzz.py")
    par.append(("gaps_fuzz", py + [gz, "--seed", "1", "--rounds", "400", "--exe", EXE], ROOT))
    par.append(("gaps_fuzz:zh", py + [gz, "--seed", "100001", "--rounds", "200", "--lang", "zh-TW", "--exe", EXE], ROOT))
    # the Document in-process (tests/fuzz/doc_fuzz.cpp): every stream that failed once, then fixed seeds (Undo / Redo,
    # .ycg round trips and a full recalculation checked after every step)
    doc_fuzz = os.path.join(ROOT, "build", "msvc-release", "tests", "yc_doc_fuzz.exe")
    if os.path.exists(doc_fuzz):
        par.append(("doc_fuzz", [doc_fuzz, "--xlsx", "--corpus", os.path.join(ROOT, "tests", "fuzz", "corpus"), "--seed", "1", "--cases", "120",
                                 "--out", tempfile.gettempdir()], ROOT))
        # the streams found with whole columns, rows and the sheet as ranges read the same way only with --wide
        par.append(("doc_fuzz_wide", [doc_fuzz, "--wide", "--corpus", os.path.join(ROOT, "tests", "fuzz", "corpus", "wide"), "--seed", "1",
                                      "--cases", "1", "--out", tempfile.gettempdir()], ROOT))
        # ... and those found with both (ranges from a cell to the sheet's last cell come with them)
        par.append(("doc_fuzz_wide_more", [doc_fuzz, "--wide", "--more", "--corpus", os.path.join(ROOT, "tests", "fuzz", "corpus", "wide_more"),
                                           "--seed", "1", "--cases", "1", "--out", tempfile.gettempdir()], ROOT))
        # ... and those found with the operations of --more (Paste Special, borders, Replace, Subtotal...) with it
        par.append(("doc_fuzz_more", [doc_fuzz, "--more", "--xlsx", "--corpus", os.path.join(ROOT, "tests", "fuzz", "corpus", "more"),
                                      "--seed", "1", "--cases", "120", "--out", tempfile.gettempdir()], ROOT))
        # ... and those found with the calculation settings of --gaps
        par.append(("doc_fuzz_gaps", [doc_fuzz, "--gaps", "--xlsx", "--corpus", os.path.join(ROOT, "tests", "fuzz", "corpus", "gaps"),
                                      "--seed", "1", "--cases", "120", "--out", tempfile.gettempdir()], ROOT))
    after = [("feature:coverage", py + [os.path.join(ROOT, "scripts", "feature_check.py"), "--exe", EXE, "--coverage-from",
                                        ",".join(f"{cov}{k}.txt" for k in range(FEATURE_SHARDS))], ROOT),
             ("trader_session", py + [os.path.join(ROOT, "scripts", "trader_session.py"), "--exe", EXE], ROOT)]
    return par, after


def chaos_jobs() -> list[tuple[str, list[str], str]]:
    dpy = os.path.join(ROOT, "dist_debug", "runtime", "python", "python.exe")
    dexe = os.path.join(ROOT, "dist_debug", "ycgrid.exe")
    out = []
    with open(os.path.join(ROOT, "scripts", "chaos_seeds.txt"), encoding="utf-8") as f:
        for line in f:
            parts = line.split("#")[0].split()
            if len(parts) >= 3:
                seed, lang, actions = parts[:3]
                out.append((f"chaos:{seed}", [dpy, "-E", "-s", os.path.join(ROOT, "scripts", "chaos.py"), "--seed", seed, "--lang", lang,
                                              "--actions", actions, "--minutes", "60", "--exe", dexe], ROOT))
    return out


def log_path(name: str) -> str:
    safe = "".join(ch if ch.isalnum() else "_" for ch in name)
    return os.path.join(tempfile.gettempdir(), f"yc_check_{safe}.txt")


def run_job(name: str, cmd: list[str], cwd: str, results: dict, lock: threading.Lock) -> None:
    tmp = tempfile.mkdtemp(prefix="yc_job_")
    env = dict(os.environ, TEMP=tmp, TMP=tmp, YCGRID_EXE=EXE, PYTHONIOENCODING="utf-8")
    t0 = time.time()
    with open(log_path(name), "w", encoding="utf-8", errors="replace") as log:
        try:
            rc = subprocess.run(cmd, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=3600).returncode
        except subprocess.TimeoutExpired:
            rc = -9
            log.write("\n[check_parallel] timed out after 60 minutes\n")
    shutil.rmtree(tmp, ignore_errors=True)
    with lock:
        results[name] = (rc, time.time() - t0)
        print(f"[check] {'ok  ' if rc == 0 else 'FAIL'} {name} ({time.time() - t0:.0f} s)" + ("" if rc == 0 else f"  -> {log_path(name)}"), flush=True)


def run_all(items, n: int, results: dict) -> None:
    lock = threading.Lock()
    pending = list(items)
    running: list[threading.Thread] = []
    while pending or running:
        running = [t for t in running if t.is_alive()]
        while pending and len(running) < n:
            name, cmd, cwd = pending.pop(0)
            t = threading.Thread(target=run_job, args=(name, cmd, cwd, results, lock), daemon=True)
            t.start()
            running.append(t)
        time.sleep(0.2)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="only the Python tests")
    ap.add_argument("--chaos", action="store_true", help="only the chaos seeds, on the debug build")
    ap.add_argument("--jobs", type=int, default=max(2, min(8, (os.cpu_count() or 4) // 3)))
    ap.add_argument("--only", default="", help="comma-separated job names (prefixes) to run")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    par, after = (chaos_jobs(), []) if a.chaos else jobs(a.quick)
    if a.only:
        keep = [x for x in a.only.split(",") if x]
        par = [j for j in par if any(j[0].startswith(k) for k in keep)]
        after = [j for j in after if any(j[0].startswith(k) for k in keep)]
    if a.list:
        for j in par + after:
            print(j[0])
        return 0
    t0 = time.time()
    results: dict = {}
    print(f"[check] {len(par)} jobs, {a.jobs} at a time", flush=True)
    run_all(par, a.jobs, results)
    run_all(after, 1, results)  # coverage needs every shard; trader_session is timed and runs alone
    bad = sorted(n for n, (rc, _) in results.items() if rc != 0)
    print(f"[check] {len(results) - len(bad)} passed, {len(bad)} failed in {time.time() - t0:.0f} s", flush=True)
    for n in bad:
        print(f"  FAIL {n}: {log_path(n)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
