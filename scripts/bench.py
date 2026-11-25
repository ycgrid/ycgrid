# SPDX-License-Identifier: Apache-2.0
"""YCGrid benchmark: live tables through the shared-memory bind path (the roadmap's Phase 1 gate).

Builds a bound table of ROWS x COLS numbers (default 100,000 x 10 = one million cells) in a new workbook, then sends
updates by key as fast as possible (or at --rate updates per second) and reads YCGrid's own counters:

- cell updates per second sent and written,
- tick-to-screen latency: from the moment Python sent the data to the frame that showed it (p50 / p99 / max),
- frames per second and CPU time per frame, snapshots that reached the screen,
- memory (working set, private bytes) at the start and the end; with --hours, a sample every minute (soak test).

    runtime\\python\\python.exe -E -s scripts\\bench.py [--pid N] [--rows 100000] [--cols 10] [--fraction 1.0]
        [--seconds 10] [--rate 0] [--hours 0] [--publish 16] [--hz 0] [--out report.json] [--discard]

Start YCGrid first. The test replaces the open workbook: save your work, or pass --discard.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))

import numpy as np  # noqa: E402

import yc  # noqa: E402
from yc import excel, ui  # noqa: E402


def stats():
    return ui._rq("perf.stats", last=600)


def build(rows, cols):
    ws = excel.Application.ActiveSheet
    ws.Name = "Bench"
    data = {"key": np.arange(rows, dtype=np.int64)}
    rng = np.random.default_rng(1)
    for c in range(cols):
        data[f"c{c}"] = 100 + rng.standard_normal(rows)
    t0 = time.perf_counter()
    t = ws.bind(data, key="key", name="Bench", formats={f"c{c}": "0.00" for c in range(cols)})
    return t, time.perf_counter() - t0


def run(t, rows, cols, fraction, seconds, rate):
    rng = np.random.default_rng(2)
    n = max(1, int(rows * fraction))
    # pre-generated ticks, so the test measures YCGrid and not numpy
    ticks = []
    px = 100 + rng.standard_normal((cols, rows))
    for _ in range(16):
        px += rng.standard_normal((cols, rows)) * 0.01
        keys = np.sort(rng.choice(rows, n, replace=False)) if n < rows else np.arange(rows)
        upd = {"key": keys}
        for c in range(cols):
            upd[f"c{c}"] = px[c][keys].copy()
        ticks.append(upd)
    s0 = stats()
    ui._rq("perf.stats", last=1, reset_latency=True)
    sent = 0
    frames = 0
    t0 = time.perf_counter()
    nxt = t0
    while time.perf_counter() - t0 < seconds:
        t.update(ticks[frames % len(ticks)])
        frames += 1
        sent += n * cols
        if rate:
            nxt += 1.0 / rate
            d = nxt - time.perf_counter()
            if d > 0:
                time.sleep(d)
    t.update({"key": [0], "c0": [100.0]}, wait=True)  # everything sent has been written
    dt = time.perf_counter() - t0
    time.sleep(0.3)
    s1 = stats()
    return {
        "updates_sent": frames,
        "cells_sent_per_s": sent / dt,
        "cells_written_per_s": (s1["bind_cells"] - s0["bind_cells"]) / dt,
        "publishes_per_s": (s1["publishes"] - s0["publishes"]) / dt,
        "snapshots_on_screen_per_s": (s1["snapshots_shown"] - s0["snapshots_shown"]) / dt,
        "recalc_ms": s1["recalc_ms"],
        "engine_tasks_ms_per_s": (s1["engine_us_tasks"] - s0["engine_us_tasks"]) / 1000 / dt,
        "engine_recalc_ms_per_s": (s1["engine_us_recalc"] - s0["engine_us_recalc"]) / 1000 / dt,
        "engine_publish_ms_per_s": (s1["engine_us_publish"] - s0["engine_us_publish"]) / 1000 / dt,
        "engine_tasks_per_s": (s1["engine_tasks"] - s0["engine_tasks"]) / dt,
        "fps": (s1["frames"] - s0["frames"]) / dt,
        "frame_cpu_avg_ms": s1["frame_cpu_avg_ms"],
        "frame_cpu_p99_ms": s1["frame_cpu_p99_ms"],
        "latency_p50_ms": s1["latency_p50_ms"],
        "latency_p99_ms": s1["latency_p99_ms"],
        "latency_max_ms": s1["latency_max_ms"],
        "latency_samples": s1["latency_samples"],
        "working_set_mb": s1.get("working_set_mb"),
        "private_mb": s1.get("private_mb"),
        "seconds": dt,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--rows", type=int, default=100_000)
    ap.add_argument("--cols", type=int, default=10)
    ap.add_argument("--fraction", type=float, default=1.0, help="share of the rows each update changes")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--rate", type=float, default=0, help="updates per second (0 = as fast as possible)")
    ap.add_argument("--hours", type=float, default=0, help="soak test: run this long, sampling memory every minute")
    ap.add_argument("--publish", type=int, default=16, help="publish interval in ms during the test")
    ap.add_argument("--out", help="write the report as JSON")
    ap.add_argument("--discard", action="store_true", help="discard the open workbook without asking")
    ap.add_argument("--hz", type=float, default=0, help="draw frames at this fixed rate (e.g. 120: a 120 Hz display, also "
                    "for a hidden window, which Windows paces at 60 Hz); 0 = the display's vsync")
    a = ap.parse_args()
    yc.connect(a.pid)
    if not a.discard and excel.ThisWorkbook.Saved is False:
        print("The open workbook has changes: save it, or pass --discard.")
        return 2
    ui._rq("wb.new", discard_changes=True)
    old_interval = excel.Application.PublishInterval
    excel.Application.PublishInterval = a.publish
    report = {"rows": a.rows, "cols": a.cols, "cells": a.rows * a.cols, "fraction": a.fraction, "publish_interval_ms": a.publish,
              "frame_hz": a.hz or "vsync"}
    if a.hz:
        ui._rq("perf.set", hz=a.hz)
    try:
        t, build_s = build(a.rows, a.cols)
        report["bind_seconds"] = build_s
        s = stats()
        report["start_working_set_mb"] = s.get("working_set_mb")
        report["start_private_mb"] = s.get("private_mb")
        if a.hours > 0:
            samples = []
            end = time.monotonic() + a.hours * 3600
            while time.monotonic() < end:
                r = run(t, a.rows, a.cols, a.fraction, min(60, end - time.monotonic()), a.rate)
                r["t"] = time.time()
                samples.append(r)
                print(f"{time.strftime('%H:%M:%S')} {r['cells_written_per_s'] / 1e6:6.2f} M cells/s  p99 {r['latency_p99_ms']:6.1f} ms  "
                      f"{r['fps']:5.0f} fps  {r['private_mb']:7.1f} MB", flush=True)
            report["soak"] = samples
            first = samples[min(1, len(samples) - 1)]["private_mb"]
            report["memory_growth_pct"] = (samples[-1]["private_mb"] - first) / first * 100 if first else 0
        else:
            report.update(run(t, a.rows, a.cols, a.fraction, a.seconds, a.rate))
    finally:
        excel.Application.PublishInterval = old_interval
        if a.hz:
            ui._rq("perf.set", hz=0)
    for k, v in report.items():
        if k != "soak":
            print(f"{k:28} {v:,.2f}" if isinstance(v, float) else f"{k:28} {v}")
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
