# SPDX-License-Identifier: Apache-2.0
r"""A trader's and a researcher's day in a hidden YCGrid, timed (see skill/ycgrid-python/references/testing.md).

    <YCGrid>\runtime\python\python.exe -E -s scripts\trader_session.py [--exe path] [--lang en|zh-TW] [--only PHASE,...]

Phases: the quote board (clicks, keys), the click-to-trade ladder (an order per click, quick clicks, cancel by
clicking your own order), the order line dragged on the order-flow chart, live candles (wheel, Live, pan), the
backtest report, the research examples (strategy lab, factor research, portfolio optimizer, VaR, pairs, volatility
surface), a researcher's own sheet (formulas, fill-handle double-click, paste, percent, filter, sort, chart,
regression, PivotTable, undo / redo), several live screens at once with sheet switching and typing, the 20-million-row
Data View, and saving. A second process asks for ui.ping (the UI thread, not waiting for the engine) four times a
second: an answer slower than a second is a frozen screen (a thread in this process would wait for this process's GIL and report stalls that aren't YCGrid's).
After each phase: frame times (a frame over 500 ms is work on the UI thread), the Error Log, an open dialog.
Screenshots and the log go to --out. Exit code 1 when a problem was found.
"""
import argparse
import faulthandler
import os
import subprocess
import sys
import time
import traceback

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if len(sys.argv) > 1 and sys.argv[1] == "--monitor":  # the watcher process: exe, pid, seconds, start time
    exe, pid, secs, t00 = sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
    sys.path.insert(0, os.path.join(os.path.dirname(exe), "python"))
    import yc

    c = yc.connect(target=pid)
    end = time.time() + secs
    while time.time() < end:
        t0 = time.time()
        try:
            c.request("ui.ping", {}, timeout=60)  # the UI thread itself (ui.state waits for the engine's queue first)
        except Exception:  # noqa: BLE001 - the instance closed
            break
        if time.time() - t0 > 0.3:
            print(f"SLOW ui.ping at {t0 - t00:.2f} took {time.time() - t0:.2f}", flush=True)
        time.sleep(0.25)
    sys.exit(0)

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument("--exe", default=os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe"))
ap.add_argument("--lang", default="en")
ap.add_argument("--only", default="")
ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "yc_trader_session"))
a = ap.parse_args()
a.exe = os.path.abspath(a.exe)
sys.path.insert(0, os.path.join(os.path.dirname(a.exe), "python"))
import yc  # noqa: E402
from yc import excel, ui  # noqa: E402
from yc.mcp import tools  # noqa: E402

import example_build  # noqa: E402 - scripts/example_build.py: the example projects

os.makedirs(a.out, exist_ok=True)
faulthandler.dump_traceback_later(1800, exit=True)
P = yc.launch(hidden=True, exe=a.exe, lang=a.lang, size=(1920, 1080))
conn = yc.connect(target=P.pid)
end = time.time() + 60
while not conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
    time.sleep(0.2)

problems = []
phase = ["start"]
LOG = open(os.path.join(a.out, "trader.log"), "w", encoding="utf-8")


def log(*x):
    s = " ".join(str(v) for v in x)
    print(s, flush=True)
    LOG.write(s + "\n")
    LOG.flush()


def problem(kind, detail):
    problems.append((phase[0], kind, detail))
    log("!!!", phase[0], kind, detail)
    try:
        ui.screenshot(os.path.join(a.out, f"problem{len(problems):02d}.png"), max_width=1920)
    except Exception:
        pass


def check(name, ok, detail=""):
    log(("OK   " if ok else "FAIL ") + name + ("" if ok else f"   <- {detail}"))
    if not ok:
        problem("CHECK", f"{name}: {detail}")


def wait(cond, secs=4.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            if cond():
                return True
        except Exception:
            pass
        time.sleep(0.1)
    try:
        return cond()
    except Exception:
        return False


def inp(script, w=0.2):
    t0 = time.time()
    conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=30)
    dt = time.time() - t0
    time.sleep(w)
    return dt


def perf(last=240):
    return conn.request("perf.stats", {"last": last})


def shot(name):
    try:
        ui.screenshot(os.path.join(a.out, f"{name}.png"), max_width=1920)
    except Exception as e:
        log("screenshot failed", e)


def build(name, timeout_ms=120000):
    t0 = time.time()
    r = example_build.build_in_host(name, timeout_ms=timeout_ms)
    dt = time.time() - t0
    log(f"   built {name} in {dt:.1f}s ok={r.get('ok')} {str(r.get('error') or '')[:300]}")
    if not r.get("ok", True) or r.get("error"):
        problem("BUILD", f"{name}: {r.get('error')} {str(r.get('traceback'))[-800:]}")
    return r, dt


# ---- the watcher: another process asks for ui.state 4x a second; a slow answer is a frozen screen ----
stalls = []
T00 = time.time()
phase_times = []
MON = subprocess.Popen([sys.executable, "-E", "-s", os.path.abspath(__file__), "--monitor", a.exe, str(P.pid), "3000", str(T00)],
                       stdout=subprocess.PIPE, text=True)


def phase_at(t):
    name = "start"
    for t0, n in phase_times:
        if t0 <= t:
            name = n
    return name


def frame_check(tag, limit_ms=100.0):
    s = perf(240)
    log(f"   perf {tag}: avg {s['frame_cpu_avg_ms']:.1f} p99 {s['frame_cpu_p99_ms']:.1f} max {s['frame_cpu_max_ms']:.1f} ms "
        f"ws {s.get('working_set_mb', 0):.0f} MB recalc {s['recalc_ms']:.1f} ms")
    if s["frame_cpu_p99_ms"] > limit_ms or s["frame_cpu_max_ms"] > 500:
        problem("JANK", f"{tag}: frame p99 {s['frame_cpu_p99_ms']:.0f} ms, max {s['frame_cpu_max_ms']:.0f} ms")
    return s


def errs_since(n):
    return [e for e in ui.errors(after=n) if e.get("error") and "answered automatically" not in str(e.get("text"))]


def last_err_id():
    e = ui.errors()
    return max([int(x.get("id", 0)) for x in e] or [0])


def act(name):
    def deco(f):
        PHASES.append((name, f))
        return f
    return deco


PHASES = []


# ===================================================================================================== phases
@act("quote_board")
def quote_board():
    build("quote_board")
    time.sleep(3)
    frame_check("quote board streaming")
    # a trader scans the board: clicks rows, arrows, page down, back up, a rapid burst
    for r in range(6, 30, 3):
        inp(f"A:C{r}|w:2", 0.05)
    lat = [inp("k:DownArrow|w:1", 0) for _ in range(20)]
    log(f"   arrow key round trip max {max(lat) * 1000:.0f} ms")
    if max(lat) > 1.0:
        problem("SLOW_KEYS", f"arrow keys on a streaming board took up to {max(lat):.2f}s")
    inp("k:PageDown|w:2|k:PageDown|w:2|k:c-Home|w:2")
    inp("|".join(f"A:{c}{r}" for c in "BCDEFG" for r in (8, 12, 16)) + "|w:2")
    inp("Q:B10|w:3|k:Escape|w:2")  # a double-click on a live cell then Escape
    inp("R:D12|w:5|k:Escape|w:3")
    shot("quote_board")
    frame_check("quote board after clicks")


@act("ladder")
def ladder():
    build("dom_ladder")
    time.sleep(2)
    ws = excel.Application.ActiveSheet
    check("ladder sheet active", ws.Name == "Ladder", ws.Name)

    def blotter_rows():
        return sum(1 for v in ws.Range("H9:H80").Value if v[0] not in (None, ""))

    # find a Bid row with size below the last price: click it to buy
    last = ws.Range("E5").Value
    log("   last", last)
    rows = [r for r in range(7, 37) if isinstance(ws.Range(f"C{r}").Value, (int, float))]
    check("ladder shows bid sizes", bool(rows), [ws.Range(f'C{r}').Value for r in range(7, 12)])
    n0 = blotter_rows()
    if rows:
        r = rows[len(rows) // 2]
        px = ws.Range(f"D{r}").Value
        inp(f"A:C{r}|w:4", 0.6)
        ok = wait(lambda: blotter_rows() > n0, 3)
        check(f"click on Bid at {px} sends an order", ok, f"blotter rows {blotter_rows()} (was {n0})")
    # order spam: 15 clicks on Ask sizes; the risk limits must refuse past the position limit, with a reason
    arows = [r for r in range(7, 37) if isinstance(ws.Range(f"E{r}").Value, (int, float))]
    if arows:
        n1 = blotter_rows()
        inp("|".join([f"A:E{arows[0]}|w:1"] * 15) + "|w:4", 1.0)
        st = ui.state()
        log("   status after spam:", st["status"], "orders", n1, "->", blotter_rows())
        check("quick clicks on the ladder don't open the cell editor", "edit" not in st, st.get("edit"))
        check("every quick click is an order", blotter_rows() - n1 >= 10, (n1, blotter_rows()))
    time.sleep(1.5)
    shot("ladder")
    # cancel own orders: click My Bid / My Ask cells that hold a size; the blotter's working orders go down
    def working():
        return sum(1 for v in ws.Range("P9:P80").Value if v[0] == "Working")
    w0 = working()
    mine = [(c, r) for r in range(7, 37) for c in "BF" if isinstance(ws.Range(f"{c}{r}").Value, (int, float))]
    log("   my working orders on the ladder:", mine[:6], "working in the blotter:", w0)
    check("own orders shown on the ladder", bool(mine), mine)
    for c, r in mine[:4]:
        inp(f"A:{c}{r}|w:3", 0.4)
    ok = wait(lambda: working() < w0, 4)
    log("   working after cancel clicks:", working())
    check("a click on My Bid / My Ask cancels", ok, (w0, working()))
    frame_check("ladder")


@act("order_flow")
def order_flow():
    build("order_flow")
    time.sleep(3)
    figs = ui.state()["figures"]
    f = next((x for x in figs if "order flow" in x["title"].lower()), None)
    check("order flow figure", f is not None, [x["title"] for x in figs])
    if not f:
        return
    hs = [h for p in f["plots"] for h in (p.get("handles") or [])]
    log("   handles:", [(h.get("label"), h.get("value", h.get("y"))) for h in hs][:6])
    buy = next((h for h in hs if "BUY" in str(h.get("label"))), None)
    check("order line on the chart", buy is not None, hs[:3])
    if buy:
        y0 = float(buy.get("value", buy.get("y", 0)))
        conn.request("plot.input", {"fig": f["id"], "ax": 0, "action": "drag_handle", "handle": buy["id"], "y": y0 - 5})
        time.sleep(1.5)
        f2 = next(x for x in ui.state()["figures"] if x["id"] == f["id"])
        b2 = next((h for p in f2["plots"] for h in (p.get("handles") or []) if h["id"] == buy["id"]), None)
        log("   after drag:", b2)
        conn.request("plot.input", {"fig": f["id"], "ax": 0, "action": "wheel", "x": 5, "y": y0, "delta": 1})
        conn.request("plot.input", {"fig": f["id"], "ax": 0, "action": "double_click", "x": 5, "y": y0})
        time.sleep(1)
    shot("order_flow")
    frame_check("order flow")


@act("candles")
def candles():
    build("live_candles")
    time.sleep(6)
    figs = ui.state()["figures"]
    f = next((x for x in figs if "live candles" in x["title"].lower()), None)
    check("live candles figure", f is not None, [x["title"] for x in figs])
    if not f:
        return
    v = f["plots"][0]["view"]
    log("   view", v)
    conn.request("plot.input", {"fig": f["id"], "ax": 0, "action": "wheel"})
    time.sleep(0.5)
    f2 = next(x for x in ui.state()["figures"] if x["id"] == f["id"])
    check("wheel pauses following", f2.get("follow_paused"), f2.get("follow_paused"))
    conn.request("plot.input", {"fig": f["id"], "ax": 0, "action": "live"})
    time.sleep(6)
    f3 = next(x for x in ui.state()["figures"] if x["id"] == f["id"])
    check("Live resumes following", not f3.get("follow_paused"), f3.get("follow_paused"))
    check("the view moves with new bars", f3["plots"][0]["view"][1] >= v[1], (v, f3["plots"][0]["view"]))
    # mouse on the window: drag to pan, wheel, right-click menu
    w = next((x for x in ui.windows() if "live candles" in x["label"].lower()), None)
    if w:
        r = w["rect"]
        cx, cy = (r[0] + r[2]) // 2, (r[1] + r[3]) // 3
        inp(f"m:{cx},{cy}|w:4|d:{cx},{cy},{cx - 150},{cy + 20}|w:6|m:{cx},{cy}|r:|w:6|k:Escape|w:4|D:{cx},{cy}|w:6")
    shot("candles")
    frame_check("candles")


@act("backtest")
def backtest():
    _, dt = build("backtest_report")
    ws = excel.Application.ActiveSheet
    log("   sheet", ws.Name, "used", ui.state()["sheets"][-1]["used"] if ui.state()["sheets"] else "")
    # walk the trade list like a user: find the trades table and click a few rows
    tables = [t for t in ui.state()["sheets"] if t["name"] == ws.Name][0].get("tables")
    log("   tables", tables)
    inp("k:c-Home|w:2")
    for k in range(6):
        inp("k:PageDown|w:2", 0.05)
    inp("k:c-Home|w:2")
    shot("backtest")
    frame_check("backtest report", 150)


@act("research")
def research():
    for name in ("strategy_lab", "factor_research", "portfolio_optimizer", "monte_carlo_var", "pairs_trading", "vol_surface"):
        phase[0] = f"research:{name}"
        n0 = last_err_id()
        _, dt = build(name, timeout_ms=240000)
        if dt > 30:
            problem("SLOW_BUILD", f"{name} took {dt:.0f}s")
        e = errs_since(n0)
        if e:
            problem("ERRORLOG", f"{name}: {[x.get('title') for x in e][:3]} {str(e[0].get('text'))[:300]}")
        shot(f"research_{name}")
        frame_check(name, 150)
    # rotate the surface by dragging inside it
    w = next((x for x in ui.windows() if "volatility surface" in x["label"].lower()), None)
    if w:
        r = w["rect"]
        cx, cy = r[0] + (r[2] - r[0]) // 4, (r[1] + r[3]) // 2
        for _ in range(3):
            inp(f"m:{cx},{cy}|w:3|d:{cx},{cy},{cx + 120},{cy + 40}|w:4", 0.05)
        frame_check("vol surface rotate", 150)


@act("manual")
def manual():
    """A researcher's own sheet: paste a series, formulas, fill down, filter, sort, chart, regression, pivot."""
    for f in ui.state()["figures"]:  # the user closes the example charts before working on a sheet of their own
        conn.request("plot.close", {"fig": f["id"]})
    time.sleep(0.5)
    excel.Worksheets.Add()
    ws = excel.Application.ActiveSheet
    import random

    rnd = random.Random(5)
    hdr = [["Date", "Close", "Volume", "Sector"]]
    rows = []
    px = 100.0
    for i in range(250):
        px *= 1 + rnd.gauss(0, 0.01)
        rows.append([45000 + i, round(px, 2), rnd.randint(1000, 90000), rnd.choice(["Tech", "Fin", "Energy"])])
    ws.Range("A1:D1").Value = hdr
    ws.Range("A2:D251").Value = rows
    ws.Range("A2:A251").NumberFormat = "yyyy-mm-dd"
    # returns column typed by the user, then filled down by double-clicking the fill handle
    inp("A:E1|w:2|t:Return|k:Enter|w:2|A:E3|w:2|t:=B3/B2-1|k:Enter|w:3")
    r = ui.cell_rect("E3")
    inp("A:E3|w:3")
    inp(f"m:{r[2] - 1},{r[3] - 1}|w:4|D:{r[2] - 1},{r[3] - 1}|w:8", 0.5)
    check("fill handle double-click fills to the end of the data", ws.Range("E251").Formula == "=B251/B250-1",
          ws.Range("E251").Formula)
    inp("A:F1|w:2|t:Vol20|k:Enter|w:2|A:F22|w:2|t:=STDEV.S(E3:E22)*SQRT(252)|k:Enter|w:3")
    inp("A:F22|w:2|k:c-C|w:2|g:F23:F251|w:2|k:c-V|w:4|k:Escape|w:2", 0.4)
    check("paste of a formula down 229 rows", ws.Range("F251").Formula == "=STDEV.S(E232:E251)*SQRT(252)", ws.Range("F251").Formula)
    inp("g:E2:F251|w:2|k:c-s-5|w:3")  # percent
    check("percent format shown", "%" in str(ws.Range("E3").Text), ws.Range("E3").Text)
    # autofilter on Sector, choose one value with the keyboard
    inp("A:A1|w:2|k:c-s-L|w:4|A:D1|w:2|k:a-DownArrow|w:6", 0.3)
    st = ui.state()
    log("   filter menu: popup", st.get("ribbon", {}).get("popup_open"), "dialog", st.get("dialog"))
    inp("k:Escape|w:3")
    # sort by Volume descending via the Data tab command
    inp("A:C2|w:2")
    for cid in ("SortDescending", "SortZA", "SortDesc"):
        try:
            ui.command(cid)
            log("   sorted with", cid)
            break
        except Exception:
            continue
    time.sleep(0.5)
    v = [ws.Range(f"C{r}").Value for r in range(2, 6)]
    check("sorted by volume, descending", v == sorted(v, reverse=True), v)
    # a line chart of Close
    inp("g:B1:B251|w:2")
    for cid in ("InsertLineChart", "InsertLine", "ChartLine"):
        try:
            ui.command(cid)
            log("   chart with", cid)
            break
        except Exception:
            continue
    time.sleep(0.5)
    inp("k:Escape|w:3")
    # Data Analysis: regression of Close on Volume with the keyboard
    inp("A:H2|w:2")
    ui.command("DataAnalysis")
    time.sleep(0.3)
    inp("k:R|w:2|k:R|w:2|k:R|w:2|k:Enter|w:8")
    inp("t:$B$1:$B$251|k:Tab|t:$C$1:$C$251|k:Tab|k:Space|k:Enter|w:15", 1.0)
    log("   sheets after regression:", [s["name"] for s in ui.state()["sheets"]][-3:], "dialog", ui.state()["dialog"])
    if ui.state()["dialog"]:
        problem("DIALOG", f"regression dialog still open: {ui.state()['dialog']}")
        inp("k:Escape|w:4")
    excel.Worksheets(ws.Name).Activate()
    # PivotTable of volume by sector
    inp("g:A1:D251|w:2")
    try:
        ui.command("InsertPivotTable")
        time.sleep(0.4)
        inp("k:Enter|w:10", 0.6)
        log("   pivot: dialog", ui.state()["dialog"], "sheet", ui.state()["active_sheet"])
        inp("k:Escape|w:3")
    except Exception as e:
        log("   pivot ->", e)
    # undo a lot, redo a lot
    inp("|".join(["k:c-Z"] * 15) + "|w:4", 0.4)
    inp("|".join(["k:c-Y"] * 15) + "|w:4", 0.4)
    shot("manual")
    frame_check("manual work", 150)


@act("heavy")
def heavy():
    """Several live screens at once and switching between them like a trader with many tabs."""
    for name in ("options_desk", "execution_algos", "depth_heatmap", "million_cells"):
        build(name, timeout_ms=180000)
    time.sleep(4)
    frame_check("four live screens + earlier ones", 100)
    lat = []
    for _ in range(30):
        lat.append(inp("k:c-PageDown|w:2", 0.05))
    for _ in range(10):
        lat.append(inp("k:c-PageUp|w:2", 0.05))
    log(f"   sheet switch round trips: max {max(lat) * 1000:.0f} ms avg {sum(lat) / len(lat) * 1000:.0f} ms")
    if max(lat) > 1.5:
        problem("SLOW_SWITCH", f"sheet switching took up to {max(lat):.2f}s with live screens")
    # typing into a sheet while a million cells stream
    excel.Worksheets("Million").Activate()
    inp("w:3")

    def free(addr):  # a cell no floating window covers (the user clicks what they see)
        r = ui.cell_rect(addr)
        return r and not any(w.get("visible", True) and not w["docked"] and w["rect"][0] <= (r[0] + r[2]) / 2 <= w["rect"][2]
                             and w["rect"][1] <= (r[1] + r[3]) / 2 <= w["rect"][3] for w in ui.windows())
    cell = next((c for c in ("L3", "N3", "P3", "R3", "T3", "L30", "P30") if free(c)), None)
    if cell is None:  # every spot covered: the user closes the charts first
        for f in ui.state()["figures"]:
            conn.request("plot.close", {"fig": f["id"]})
        inp("w:4")
        cell = "L3"
    below = cell[0] + str(int(cell[1:]) + 1)
    t0 = time.time()
    inp(f"A:{cell}|w:2|t:hello|k:Enter|w:2|A:{below}|w:2|t:=1+2|k:Enter|w:3")
    log(f"   typing into {cell} during million-cell updates: {time.time() - t0:.2f}s")
    check("typed cell kept", excel.ActiveSheet.Range(below).Value == 3 and excel.ActiveSheet.Range(cell).Value == "hello",
          (excel.ActiveSheet.Range(cell).Value, excel.ActiveSheet.Range(below).Value))
    frame_check("million cells", 120)
    shot("heavy")


@act("big_workbook")
def big_workbook():
    """A model with a million formulas: filling, copying the sheet, inserting rows, deleting, undo and redo storms.
    The engine may take seconds; the screen never waits for it."""
    for f in ui.state()["figures"]:
        conn.request("plot.close", {"fig": f["id"]})
    excel.Worksheets.Add()
    ws = excel.Application.ActiveSheet
    ws.Range("A1:A20").Value = [[i] for i in range(20)]
    steps = [("fill a million formulas", "g:B1:C500000|w:2|t:=A1*2|k:c-Enter|w:4"), ("copy the sheet", None),
             ("undo", "k:c-Z|w:4"), ("insert a row", "A:A5|w:2|k:s-Space|w:2|k:c-s-Equal|w:4"),
             ("undo x3", "k:c-Z|w:2|k:c-Z|w:2|k:c-Z|w:4"), ("redo x2", "k:c-Y|w:2|k:c-Y|w:4"),
             ("delete a column", "A:D1|w:2|k:c-Space|w:2|k:c-Minus|w:4"), ("undo", "k:c-Z|w:4"),
             # a million cells copied and pasted: the paste held three copies of them on the UI thread (a 1.6 s frame
             # for six million)
             ("copy and paste a million cells", "g:B1:C500000|w:2|k:c-C|w:3|g:H1|w:2|k:c-V|w:4|k:Escape|w:2"), ("undo", "k:c-Z|w:4"),
             # a color scale over half a million cells while the sheet changes: its statistics were gathered and sorted
             # again for every new snapshot, a frame each
             ("color scale, then scrolling", "g:B1:B500000|w:2|x:CfScaleGYR|w:4|g:B1|w:2|" + "k:PageDown|" * 20 + "w:3"),
             ("undo", "k:c-Z|w:4")]
    for name, script in steps:
        f0 = perf(1)["frames"]
        t0 = time.time()
        if script:
            inp(script, 0.1)
        else:
            # Move or Copy, as Excel: tick Create a copy, then OK (Enter)
            ui.command("CopySheet")
            inp("w:4", 0.1)
            box = [i for i in ui.items("") if i["label"] == "Create a copy"]
            if box:
                r = box[0]["rect"]
                x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
                inp(f"m:{x},{y}|w:2|c:{x},{y}|w:3|k:Enter|w:4", 0.1)
            else:
                problem("DIALOG", "Move or Copy has no Create a copy box")
            ws.Activate()  # Excel shows the copy: the storm goes on in the model sheet
        ui.state()
        s = perf(max(1, perf(1)["frames"] - f0))
        log(f"   {name}: {time.time() - t0:.2f}s, frame max {s['frame_cpu_max_ms']:.0f} ms")
        if s["frame_cpu_max_ms"] > 500:
            problem("FROZEN", f"{name}: a frame of {s['frame_cpu_max_ms']:.0f} ms")
    check("the filled formulas are there", ws.Range("C500000").Formula == "=B500000*2", ws.Range("C500000").Formula)


@act("bigdata")
def bigdata():
    _, dt = build("big_data", timeout_ms=300000)
    time.sleep(2)
    frame_check("big data", 150)
    w = next((x for x in ui.windows() if "ticks" in x["label"].lower()), None)
    if w:
        r = w["rect"]
        cx, cy = (r[0] + r[2]) // 2, (r[1] + r[3]) // 2
        inp(f"m:{cx},{cy}|w:3|d:{cx},{cy},{cx - 300},{cy}|w:4|D:{cx},{cy}|w:4")
        frame_check("big data pan", 150)
    shot("bigdata")


@act("save")
def save():
    path = os.path.join(a.out, "trader_book.ycg")
    if os.path.exists(path):
        os.remove(path)
    t0 = time.time()
    excel.ActiveWorkbook.SaveAs(path)
    log(f"   save took {time.time() - t0:.1f}s, {os.path.getsize(path) / 1e6:.1f} MB")
    if time.time() - t0 > 10:
        problem("SLOW_SAVE", f"save took {time.time() - t0:.1f}s")


# ===================================================================================================== run
only = set(x for x in a.only.split(",") if x)
try:
    for name, f in PHASES:
        if only and name not in only:
            continue
        phase[0] = name
        phase_times.append((time.time() - T00, name))
        log(f"== {name}")
        n0 = last_err_id()
        t0 = time.time()
        try:
            f()
        except Exception:
            if P.poll() is not None:
                problem("CRASH", f"YCGrid ended {P.poll() & 0xFFFFFFFF:#x}")
                break
            problem("SCRIPT", traceback.format_exc()[-1500:])
        if P.poll() is not None:
            problem("CRASH", f"YCGrid ended {P.poll() & 0xFFFFFFFF:#x}")
            break
        phase[0] = name
        for e in errs_since(n0):
            problem("ERRORLOG", f"{e.get('title')} | {str(e.get('text'))[:400]}")
        st = ui.state()
        if st.get("dialog"):
            problem("DIALOG_LEFT", f"dialog {st['dialog']} open after {name}")
            inp("k:Escape|w:4")
        log(f"   {name} done in {time.time() - t0:.1f}s")
finally:
    MON.kill()
    for ln in MON.stdout.read().splitlines():
        if ln.startswith("SLOW"):
            t = float(ln.split(" at ")[1].split()[0])
            stalls.append((phase_at(t), float(ln.split("took ")[1]), ln))
    if P.poll() is None:
        try:
            con = str(tools.yc_read_console(max_lines=3000, min_level="error"))
            lines = [ln for ln in con.splitlines() if ln.strip() and not ln.startswith("next_cursor")]
            log("console errors:", len(lines))
            for ln in lines[:40]:
                log("   ", ln[:300])
        except Exception as e:
            log("console read failed", e)
    for s in stalls:
        log("STALL", s)
        if s[1] > 1.0:  # shorter waits while a step itself works (a save, a sort) are logged only
            problems.append((s[0], "STALL", f"the UI thread took {s[1]:.2f}s to answer {s[2]}"))
    log("PROBLEMS:", len(problems))
    for p in problems:
        log("  ", p[0], p[1], str(p[2])[:600])
    P.terminate()
    sys.exit(1 if problems else 0)
