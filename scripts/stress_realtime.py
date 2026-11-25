# SPDX-License-Identifier: Apache-2.0
"""Real-time stress test for a running YCGrid.

Builds a quote board in a new workbook (=QUOTE cells plus dependent formulas, conditional formatting and a
total), pushes ticks as fast as possible and reads YCGrid's own counters: ticks and cell updates per second,
how many engine snapshots reached the screen, frames per second and the CPU time of a frame.

    runtime\\python\\python.exe -E -s scripts\\stress_realtime.py [--pid N] [--symbols 200] [--seconds 8] [--discard]

Start YCGrid first. The test replaces the open workbook with a new one: save your work, or pass --discard.
"""
import argparse
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))

import yc  # noqa: E402
from yc import excel, ui  # noqa: E402

FIELDS = ["Bid", "Ask", "Last", "Volume", "Change"]


def build_board(n):
    ws = excel.Application.ActiveSheet
    ws.Name = "Quotes"
    hdr = ["Symbol"] + FIELDS + ["Spread", "Mid", "Qty", "P&L", "Chg%"]
    ws.Range("A1").Resize(1, len(hdr)).Value = [hdr]
    syms = [f"S{i:04d}" for i in range(n)]
    ids = excel.Application.Topics.Register([f"{s}.{f}" for s in syms for f in FIELDS])
    rows = []
    for i, s in enumerate(syms):
        r = i + 2
        rows.append([s] + [f'=QUOTE("{s}.{f}")' for f in FIELDS]
                    + [f"=C{r}-B{r}", f"=(B{r}+C{r})/2", (i % 7 + 1) * 1000, f"=(D{r}-100)*I{r}", f"=IFERROR(F{r}/(D{r}-F{r}),0)"])
    ws.Range("A2").Resize(n, len(hdr)).Formula = rows
    ws.Range(f"A{n + 3}").Value = "Total P&L"
    ws.Range(f"J{n + 3}").Formula = f"=SUM(J2:J{n + 1})"
    ws.Range(f"B2:D{n + 1}").NumberFormat = "#,##0.00"
    ws.Range(f"K2:K{n + 1}").NumberFormat = "0.00%"
    ws.Range(f"K2:K{n + 1}").FormatConditions.AddColorScale(3)
    ws.Range(f"J2:J{n + 1}").FormatConditions.AddDatabar()
    return ws, ids


def make_ticks(n, count=40):
    """Pre-generated ticks, so the test measures YCGrid and not Python's random()."""
    px = [100.0 + random.random() * 50 for _ in range(n)]
    vol = [0] * n
    out = []
    for _ in range(count):
        vals = []
        for i in range(n):
            px[i] = max(1.0, px[i] + random.uniform(-0.5, 0.5))
            vol[i] += random.randint(1, 20)
            vals += [round(px[i] - 0.05, 2), round(px[i] + 0.05, 2), round(px[i], 2), vol[i], round(px[i] - 100, 2)]
        out.append(vals)
    return out


def run_case(label, ws, ids, ticks, n, seconds, publish_ms, vsync=True, continuous=False, mode="topics"):
    app = excel.Application
    app.PublishInterval = publish_ms
    ui._rq("perf.set", vsync=vsync, continuous=continuous)
    time.sleep(0.5)
    s0 = ui._rq("perf.stats", last=600)
    t0 = time.perf_counter()
    k = 0
    while time.perf_counter() - t0 < seconds:
        v = ticks[k % len(ticks)]
        k += 1
        if mode == "topics":
            app.Topics.Push(ids, v)
        else:
            ws.Range("B2").Resize(n, 5).Value = [v[i * 5:(i + 1) * 5] for i in range(n)]
    ui._rq("sync")
    el = time.perf_counter() - t0
    time.sleep(0.3)
    s1 = ui._rq("perf.stats", last=600)
    return {
        "case": label, "ticks_per_s": round(k / el, 1), "cell_updates_per_s": round(k * n * 5 / el),
        "screen_updates_per_s": round((s1["snapshots_shown"] - s0["snapshots_shown"]) / el, 1),
        "frames_per_s": round((s1["frames"] - s0["frames"]) / el, 1),
        "frame_cpu_avg_ms": round(s1["frame_cpu_avg_ms"], 2), "frame_cpu_p99_ms": round(s1["frame_cpu_p99_ms"], 2),
        "recalc_ms": round(s1["recalc_ms"], 2), "visible_cells": s1["visible_cells"], "refresh_hz": s1["refresh_hz"],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, help="YCGrid process id (default: the only running YCGrid)")
    ap.add_argument("--symbols", type=int, default=200, help="symbols on the board; each has 5 live fields and 5 formulas")
    ap.add_argument("--seconds", type=float, default=8.0, help="seconds per case")
    ap.add_argument("--discard", action="store_true", help="discard unsaved changes in the open workbook")
    ap.add_argument("--json", help="also write the results to this JSON file")
    a = ap.parse_args()

    yc.connect(a.pid)
    if ui._rq("wb.info").get("dirty") and not a.discard:
        print("The open workbook has unsaved changes: save it, or run again with --discard.")
        return 1
    ui._rq("wb.new", discard_changes=True)
    n = a.symbols
    ws, ids = build_board(n)
    ticks = make_ticks(n)
    prev = excel.Application.PublishInterval
    results = []
    try:
        results.append(run_case("Topics.Push, publish interval 100 ms (default)", ws, ids, ticks, n, a.seconds, 100))
        results.append(run_case("Topics.Push, publish interval 16 ms", ws, ids, ticks, n, a.seconds, 16))
        results.append(run_case("Topics.Push, publish interval 0 (every change)", ws, ids, ticks, n, a.seconds, 0))
        results.append(run_case("Range.Value block writes, 16 ms", ws, ids, ticks, n, a.seconds, 16, mode="value"))
        results.append(run_case("Topics.Push, 0 ms, vsync off, continuous drawing", ws, ids, ticks, n, a.seconds, 0, vsync=False, continuous=True))
    finally:
        ui._rq("perf.set", vsync=True, continuous=False)
        excel.Application.PublishInterval = prev
    print(f"{n} symbols: {n * 5:,} live =QUOTE cells per tick, {n * 9 + 1:,} formulas in all\n")
    print(f"{'case':<50}{'ticks/s':>9}{'cells/s':>12}{'screen/s':>10}{'fps':>8}{'frame ms':>10}{'p99 ms':>8}{'recalc ms':>10}")
    for r in results:
        print(f"{r['case']:<50}{r['ticks_per_s']:>9,.0f}{r['cell_updates_per_s']:>12,}{r['screen_updates_per_s']:>10,.0f}"
              f"{r['frames_per_s']:>8,.0f}{r['frame_cpu_avg_ms']:>10.2f}{r['frame_cpu_p99_ms']:>8.2f}{r['recalc_ms']:>10.2f}")
    print(f"\nscreen refresh {results[0]['refresh_hz']} Hz, {results[0]['visible_cells']:,} visible cells")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
