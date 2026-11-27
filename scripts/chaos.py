# SPDX-License-Identifier: Apache-2.0
r"""A hurried user at random, against a hidden YCGrid: seeded random actions the way a person works a spreadsheet, with a
health check after every step (see skill/ycgrid-python/references/testing.md, "A hurried user").

    <YCGrid>\runtime\python\python.exe -E -s scripts\chaos.py [--seed N] [--minutes M] [--exe path] [--lang en|zh-TW]

Actions: clicks, double-clicks and right-clicks on cells, typing values and formulas (good and bad ones), selections,
keyboard shortcuts, fill-handle and border drags, column and row resizing, random ribbon tabs and buttons, random
context-menu items, every command id at random, dialogs answered with Enter or Escape, undo / redo storms, sheets
added, renamed, switched and deleted, Python writing while the user works, figures made, dragged, docked, closed and
clicked, built-in panes shown and hidden, the Error Log docked and taken out again, example screens, save / reopen.
After every action: the process is alive, the UI answers within 2 s, nothing new in the Error Log (questions that
were answered automatically are only logged), no dialog that Escape can't close. At the end: errors in the console.

The log and a screenshot of each problem go to %TEMP%\ycgrid_chaos<SEED>\ (a seed replays a run). Exit code 1 when a
problem was found.
"""
from __future__ import annotations

import argparse
import faulthandler
import os
import random
import re
import shutil
import sys
import tempfile
import time
import traceback

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WINDOW = (1600, 900)  # the hidden instance's size: what lies past it is off the screen
# commands that leave the session, print, touch files or installs, or stop the user from typing: not chaos material
SKIP = re.compile(r"(Quit|Exit|FileClose|CloseAll|FileOpen|SaveAs|^Save$|FileSave|FileNew|NewWindow|Print|Install|Uninstall|"
                  r"Register|Update|Shutdown|Restart|Reload|Registry|Kill|OperatorMode|VSCode|Debug|ExportPdf|Pdf)", re.I)
# keys a user presses; the ones that open files, print or close windows are left out
KEYS = ["k:Enter", "k:Tab", "k:s-Tab", "k:Escape", "k:Delete", "k:BackSpace", "k:F2|w:2|k:Escape", "k:F4", "k:F9",
        "k:DownArrow", "k:UpArrow", "k:LeftArrow", "k:RightArrow", "k:s-DownArrow", "k:s-RightArrow", "k:c-DownArrow",
        "k:c-RightArrow", "k:c-s-DownArrow", "k:c-s-End", "k:PageDown", "k:PageUp", "k:c-Home", "k:c-End",
        "k:c-PageDown", "k:c-PageUp", "k:c-A", "k:c-C", "k:c-X", "k:c-V", "k:c-Z", "k:c-Y", "k:c-B", "k:c-I", "k:c-U",
        "k:c-D", "k:c-R", "k:c-Semicolon", "k:c-s-L", "k:c-T", "k:a-Equal", "k:c-1", "k:c-F", "k:c-H", "k:c-G",
        "k:c-Space", "k:s-Space", "k:c-Minus", "k:c-s-Equal", "k:c-K", "k:s-F2", "k:s-F3", "k:c-F1", "k:F7", "k:F5",
        "k:Alt|w:3|k:Escape", "k:Alt|w:3|t:H|w:3|k:Escape|k:Escape", "k:c-s-4", "k:c-s-5", "k:c-s-1"]
VALUES = ["1", "-1", "0", "3.14159", "1e308", "-1e-308", "abc", "", " ", "TRUE", "#N/A", "'0012", "2026-10-29",
          "12:30", "1/0", "50%", "$1,234.5", "台積電", "x" * 300, "=1/0", "=SUM(A:A)", "=A1+B1", "=A1",
          "=AVERAGE(B2:B30)", "=SEQUENCE(4,3)", "=RAND()", "=NOW()", "=XLOOKUP(2,A:A,B:B)", "=IFERROR(1/0,\"x\")",
          "=SUM(", "=)(", "=FOO(1)", "=INDIRECT(\"Z99\")", "=LET(x,5,x*x)", "=LAMBDA(a,a+1)(2)", "=TEXT(NOW(),\"yyyy\")",
          "=OFFSET(A1,2,2,3,3)", "=VLOOKUP(1,A1:C10,9,0)", "=SORT(A1:A10)", "=UNIQUE(B1:B20)", "=FILTER(A1:A9,A1:A9>0)",
          "=QUOTE(\"TXF.last\")", "=Sheet2!A1", "=REPT(\"a\",40000)", "=10^400", "=MAP(A1:A3,LAMBDA(v,v*2))"]
PANES = ["immediate", "watch", "clipboard", "comments", "pivot_fields", "latency", "alerts", "heartbeats", "change_log",
         "perf", "formula_bar", "errors"]
EXAMPLES = ["quote_board", "quick_plot", "tick_replay", "live_candles", "py_cells", "order_flow", "depth_heatmap",
            "backtest_report", "option_payoff"]


class Chaos:
    def __init__(self, a):
        import yc

        self.a = a
        self.rnd = random.Random(a.seed)
        self.out = os.path.join(tempfile.gettempdir(), f"ycgrid_chaos{a.seed}")
        shutil.rmtree(self.out, ignore_errors=True)  # a seed replays only from a clean folder (saved workbooks, modules)
        os.makedirs(self.out, exist_ok=True)
        self.logf = open(os.path.join(self.out, "actions.log"), "w", encoding="utf-8")
        self.problems: list[tuple[str, str]] = []
        self.seen_err = 0
        self.figs: list = []
        self.yc = yc
        # a debug build calculates and draws up to a hundred times slower than the release one (160,000 volatile formulas
        # under a live feed: a step of 3.6 s released took 412 s in debug, seed 5043). Its time limits are wider; it is
        # run for its asserts, release builds are timed (trader_session), and a freeze still ends in a request's timeout
        self.slow_scale = 50 if "debug" in os.path.abspath(a.exe).lower() else 1
        self.start()

    # ---- the instance ----
    def start(self):
        self.p = self.yc.launch(hidden=True, exe=self.a.exe, lang=self.a.lang, size=WINDOW)
        self.conn = self.yc.connect(target=self.p.pid)
        end = time.time() + 60
        while not self.conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
            time.sleep(0.2)
        self.figs = []
        self.seen_err = 0
        self.sample()

    def sample(self):
        from yc import excel, safety

        safety.set_operator_mode(True)
        for f in (excel.ActiveWorkbook.Unprotect,):
            try:
                f()
            except Exception:  # noqa: BLE001 - not protected
                pass
        ws = excel.Worksheets(1)
        try:
            ws.Unprotect()
        except Exception:  # noqa: BLE001
            pass
        ws.Range("A1:E1").Value = [["Date", "Open", "High", "Low", "Close"]]
        ws.Range("A2:E31").Value = [[46000 + i, 100 + i, 103 + i, 98 + i, 101 + i] for i in range(30)]

    def log(self, *a):
        s = " ".join(str(x) for x in a)
        print(s, flush=True)
        self.logf.write(s + "\n")
        self.logf.flush()

    def problem(self, kind, detail):
        self.problems.append((kind, detail))
        self.log("!!!", kind, detail)
        try:
            from yc import ui

            ui.screenshot(os.path.join(self.out, f"problem{len(self.problems):02d}.png"))
        except Exception:  # noqa: BLE001
            pass

    def native_windows(self) -> str:
        """The top-level windows of the YCGrid process: a stuck UI is often a native dialog (#32770) nobody can see."""
        import ctypes
        from ctypes import wintypes

        user32 = ctypes.windll.user32
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def cb(hwnd, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == self.p.pid:
                cls, title = ctypes.create_unicode_buffer(128), ctypes.create_unicode_buffer(256)
                user32.GetClassNameW(hwnd, cls, 128)
                user32.GetWindowTextW(hwnd, title, 256)
                text = ""
                if cls.value == "#32770":  # a dialog: its message (an assertion names the file and the expression)
                    parts = []

                    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
                    def child(h, _):
                        b = ctypes.create_unicode_buffer(2048)
                        user32.GetWindowTextW(h, b, 2048)
                        if b.value.strip():
                            parts.append(b.value.strip())
                        return True

                    user32.EnumChildWindows(hwnd, child, 0)
                    text = " [" + " / ".join(parts).replace("\n", " ") + "]"
                found.append(f"{cls.value}:{title.value!r}{' visible' if user32.IsWindowVisible(hwnd) else ''}{text}")
            return True

        user32.EnumWindows(cb, 0)
        return "; ".join(found) or "none"

    def inp(self, script, wait=0.15):
        self.last_script = script
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=25)
        time.sleep(wait)

    def cell(self):
        return f"{self.rnd.choice('ABCDEFGHIJ')}{self.rnd.randint(1, 40)}"

    # ---- health ----
    @staticmethod
    def cpu_times():
        """(idle, total) CPU time of the whole machine so far (Windows' GetSystemTimes)."""
        import ctypes
        from ctypes import wintypes

        idle, kernel, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
        ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
        val = lambda ft: (ft.dwHighDateTime << 32) | ft.dwLowDateTime  # noqa: E731
        return val(idle), val(kernel) + val(user)  # kernel time includes idle time

    def busy_since(self, mark):
        """The share of the machine's CPU in use since mark (cpu_times()): a slow step on a machine with no CPU left
        (a build or other rounds beside this one) says nothing about YCGrid. Seeds 5006, 5007 and 5018 took 12.7 s to
        reopen with a build beside them, 0.3 s alone."""
        idle0, total0 = mark
        idle1, total1 = self.cpu_times()
        return 1.0 - (idle1 - idle0) / max(1, total1 - total0)

    def slow(self, kind, detail, mark):
        busy = self.busy_since(mark)
        if busy > 0.9:
            self.log("   (busy machine)", f"{kind} {detail} with the CPU {busy:.0%} in use: not counted")
            return
        self.problem(kind, f"{detail} (CPU {busy:.0%} in use)")

    def health(self, tag):
        from yc import ui

        if self.p.poll() is not None:
            self.problem("CRASH", f"YCGrid ended with exit code {self.p.poll() & 0xFFFFFFFF:#x} after: {tag} "
                                  f"(last input: {getattr(self, 'last_script', '')!r})")
            self.start()
            return
        t0, mark = time.time(), self.cpu_times()
        try:
            st = ui.state()
        except Exception as e:  # noqa: BLE001
            try:
                self.p.wait(5)   # a lost connection: did the process end, and how?
            except Exception:  # noqa: BLE001
                pass
            if self.p.poll() is not None:
                kind = "EXIT" if self.p.poll() == 0 else "CRASH"
                self.problem(kind, f"YCGrid ended with exit code {self.p.poll() & 0xFFFFFFFF:#x} after: {tag} "
                                   f"(last input: {getattr(self, 'last_script', '')!r})")
            else:
                self.problem("UNRESPONSIVE", f"ui.state failed after {tag}: {e!r}; windows: {self.native_windows()}")
                self.p.terminate()
            self.start()
            return
        if time.time() - t0 > 2.0 * self.slow_scale:
            self.slow("SLOW", f"ui.state took {time.time() - t0:.1f}s after {tag}", mark)
        for _ in range(3):  # a dialog, a menu or Backstage left open: the user presses Escape
            rb = st.get("ribbon") or {}
            if not (st.get("dialog") or rb.get("popup_open") or rb.get("backstage")):
                break
            self.inp(self.rnd.choice(["k:Escape|w:4", "k:Enter|w:4|k:Escape|w:3"]))
            st = ui.state()
        else:
            if st.get("dialog"):
                self.problem("STUCK_DIALOG", f"dialog {st.get('dialog')} doesn't close with Escape after {tag}")
        # by id: a command may clear the Error Log, so a count of the entries seen would point at the wrong ones
        for e in ui.errors(after=self.seen_err):
            self.seen_err = max(self.seen_err, int(e.get("id", 0)))
            txt = str(e.get("text", ""))
            # a question nobody could see was answered (the answer a user clicking OK gives); a Python error in a user
            # formula (PY cells, UDFs) fed with the junk typed here shows in its cell, as it should
            # and a script (an example from the ribbon) refused by the protection chaos turned on, as Excel's object
            # model refuses it (chaos seed 5130: backtest_report adding a sheet to a workbook whose structure is locked)
            expected = any(m in txt for m in ("answered automatically", "已自動回答", "ProtectedError:"))
            # a workbook saved to another folder switches to the Python project there, stopping this one's host: a
            # script still running in it (an example from the ribbon) is cut short, as a person saving then would see
            # (chaos seed 7145: backtest_report and a Save As right after it)
            expected = expected or (tag == "save as" and "The Python host is stopping" in txt)
            # what a protected sheet or workbook refused (the protection chaos turned on), noted in the Error Log since
            # 2026-11-25 where Excel shows a message box (chaos seed 7718 counted 21 of them as problems)
            expected = expected or bool(e.get("refusal"))
            # the hang watchdog's 10 s on a debug build is a fraction of a second released (seed 5562: Ctrl+D over
            # 8 million cells, a 0.65 s frame released, 11 s on debug); a real freeze still shows as UNRESPONSIVE
            title = str(e.get("title", ""))
            if self.slow_scale > 1 and ("stopped responding" in title or "停止回應" in title):
                expected = True
            if not e.get("error") or expected or str(e.get("title", "")).startswith("[UDF "):
                self.log("   (message)", (e.get("title") or ""), "|", txt[:140].replace("\n", " | "))
                continue
            self.problem("ERRORLOG", f"after {tag}: {e.get('title')} | {txt[:500]}")
        if not st.get("operator_mode"):
            from yc import safety

            safety.set_operator_mode(True)

    # ---- actions ----
    def a_click(self):
        c = self.cell()
        self.inp(f"A:{c}|w:2")
        return f"click {c}"

    def a_rapid_clicks(self):
        self.inp("|".join(f"A:{self.cell()}" for _ in range(self.rnd.randint(4, 12))) + "|w:2")
        return "rapid clicks"

    def a_type(self):
        c, v = self.cell(), self.rnd.choice(VALUES)
        how = self.rnd.random()
        if how < 0.6:
            self.inp(f"A:{c}|w:2|t:{v}|k:Enter|w:3" if v else f"A:{c}|w:2|k:Delete|w:2")
        elif how < 0.85:
            self.inp(f"Q:{c}|w:3|t:{v}|k:{self.rnd.choice(['Enter', 'Tab', 'Escape'])}|w:3")
        else:
            # a click that doesn't reach the cell (it closed a ribbon menu) leaves the selection, and Ctrl+Enter fills all
            # of it: four whole columns are four million formulas, 7.8 s on an idle machine where Excel 16 took over five
            # minutes (measured), and 17.5 s beside other rounds (seed 5724). Such a step gets a wider time limit
            from yc import ui

            self.inp(f"A:{c}|w:2")
            sel = (ui.state().get("selection") or "").split(":")
            cells = 1
            if len(sel) == 2:
                a, b = (self.rc(x) for x in sel)
                if a and b:
                    cells = (abs(b[0] - a[0]) + 1) * (abs(b[1] - a[1]) + 1)
            self.big_step = cells > 1_000_000
            self.inp(f"t:{v}|k:c-Enter|w:3")
        return f"type {c} {v[:40]!r}"

    @staticmethod
    def rc(a1):
        import re

        m = re.fullmatch(r"\$?([A-Z]{1,3})\$?(\d+)", a1.strip())
        if not m:
            return None
        col = 0
        for ch in m.group(1):
            col = col * 26 + ord(ch) - 64
        return int(m.group(2)), col

    def a_select(self):
        c1, c2 = self.cell(), self.cell()
        self.inp(f"g:{c1}:{c2}|w:2")
        return f"select {c1}:{c2}"

    def a_keys(self):
        k = self.rnd.choice(KEYS)
        self.inp(k + "|w:3")
        return f"keys {k}"

    def a_mouse_drag(self):
        from yc import ui

        c = self.cell()
        self.inp(f"A:{c}|w:2")
        r = ui.cell_rect(c)
        if not r:
            return "no cell rect"
        kind = self.rnd.choice(["fill", "border", "free"])
        dx, dy = self.rnd.randint(-200, 300), self.rnd.randint(-150, 300)
        if kind == "fill":
            x, y = r[2] - 1, r[3] - 1
        elif kind == "border":
            x, y = r[2] - 1, (r[1] + r[3]) / 2
        else:
            x, y = (r[0] + r[2]) / 2, (r[1] + r[3]) / 2
        self.inp(f"m:{x:.0f},{y:.0f}|w:3|d:{x:.0f},{y:.0f},{x + dx:.0f},{y + dy:.0f}|w:6")
        return f"drag {kind} from {c} by {dx},{dy}"

    def a_resize_header(self):
        from yc import ui

        c = f"{self.rnd.choice('ABCDEFG')}{self.rnd.randint(1, 20)}"
        r = ui.cell_rect(c)
        if not r:
            return "no cell rect"
        if self.rnd.random() < 0.5:   # a column edge in the column header row
            x, y = r[2], r[1] - 12
            d = f"d:{x:.0f},{y:.0f},{x + self.rnd.randint(-120, 200):.0f},{y:.0f}"
        else:                         # a row edge in the row header column
            x, y = 20, r[3]
            d = f"d:{x:.0f},{y:.0f},{x:.0f},{y + self.rnd.randint(-40, 120):.0f}"
        if self.rnd.random() < 0.2:
            d = d.replace("d:", "D:").rsplit(",", 2)[0]  # a double-click on the edge: AutoFit
        self.inp(f"m:{x:.0f},{y:.0f}|w:3|{d}|w:6")
        return f"resize header at {c}"

    def a_right_click_menu(self):
        c = self.cell()
        n = self.rnd.randint(0, 14)
        steps = "|".join(["k:DownArrow"] * n)
        end = self.rnd.choice(["k:Enter", "k:Escape", "k:Enter"])
        self.inp(f"R:{c}|w:5|" + (steps + "|" if n else "") + end + "|w:6")
        return f"context menu {c} item {n} {end}"

    def a_ribbon_click(self):
        from yc import ui

        rb = ui.state().get("ribbon") or {}
        if rb.get("collapsed") and rb.get("arrow"):
            x, y = rb["arrow"]
            self.inp(f"c:{x:.0f},{y:.0f}|w:6")
        tabs = rb.get("tabs") or {}
        names = list(tabs)[1:]  # not File (the first tab in every language): Backstage's Close ends the session
        if not names:
            return "no ribbon tabs"
        t = self.rnd.choice(names)
        tx, ty = tabs[t]
        x, y = self.rnd.randint(10, 1580), self.rnd.randint(int(ty) + 18, int(ty) + 150)
        self.inp(f"c:{tx:.0f},{ty:.0f}|w:5|m:{x},{y}|w:3|c:{x},{y}|w:8")
        return f"ribbon {t} click at {x},{y}"

    def a_command(self):
        from yc import ui

        if not hasattr(self, "_cmds"):
            self._cmds = [c["id"] for c in ui.commands() if not SKIP.search(c["id"])]
        cid = self.rnd.choice(self._cmds)
        if self.rnd.random() < 0.5:
            self.inp(f"g:{self.cell()}:{self.cell()}|w:2")
        self.last_script = f"command {cid}"
        try:
            self.conn.request("ui.command", {"id": cid}, timeout=20)
        except Exception as e:  # noqa: BLE001 - refused (a protected sheet, nothing selected...)
            return f"command {cid} -> {str(e)[:100]}"
        time.sleep(0.25)
        if ui.state().get("dialog"):
            self.inp(self.rnd.choice(["k:Enter|w:6", "k:Escape|w:4", "k:Tab|w:2|k:Tab|w:2|k:Enter|w:6"]))
        return f"command {cid}"

    def a_undo_storm(self):
        k = self.rnd.choice(["k:c-Z", "k:c-Y"])
        n = self.rnd.randint(1, 12)
        self.inp("|".join([k] * n) + "|w:4")
        return f"{k} x{n}"

    def a_sheet(self):
        from yc import excel, ui

        how = self.rnd.random()
        sheets = [s["name"] for s in ui.state()["sheets"]]
        if how < 0.3 and len(sheets) < 8:
            excel.Worksheets.Add()
            return "add sheet"
        if how < 0.55:
            excel.Worksheets(self.rnd.choice(sheets)).Activate()
            return "switch sheet"
        if how < 0.7:
            name = self.rnd.choice(["Data", "P&L", "報價", "x" * 31, "Sheet 2", "a'b"])
            try:
                excel.ActiveSheet.Name = name
            except Exception as e:  # noqa: BLE001 - a duplicate or invalid name: refused like Excel
                return f"rename -> {str(e)[:80]}"
            return f"rename {name!r}"
        if how < 0.8 and len(sheets) > 2:
            ws = excel.Worksheets(self.rnd.choice(sheets[1:]))
            try:
                ws.Delete()
            except Exception as e:  # noqa: BLE001
                return f"delete sheet -> {str(e)[:80]}"
            return "delete sheet"
        self.inp("k:c-PageDown|w:3" if self.rnd.random() < 0.5 else "k:c-PageUp|w:3")
        return "next / previous sheet"

    def a_python_writes(self):
        from yc import excel

        ws = excel.ActiveSheet
        r0, c0 = self.rnd.randint(1, 30), self.rnd.randint(1, 8)
        h, w = self.rnd.randint(1, 20), self.rnd.randint(1, 6)
        vals = [[self.rnd.choice([self.rnd.random() * 1000, -self.rnd.random(), "txt", None, True])
                 for _ in range(w)] for _ in range(h)]
        try:
            ws.Range(ws.Cells(r0, c0), ws.Cells(r0 + h - 1, c0 + w - 1)).Value = vals
        except Exception as e:  # noqa: BLE001 - a protected sheet, a table, a PivotTable: refused
            return f"python write -> {str(e)[:80]}"
        return f"python writes {h}x{w} at R{r0}C{c0}"

    def a_figure(self):
        from yc import plot, ui
        from yc.constants import ycDockBottom, ycDockLeft, ycDockRight

        self.figs = [f for f in self.figs if f is not None]
        how = self.rnd.random()
        if how < 0.3 or not self.figs:
            n = self.rnd.choice([3, 50, 5000, 200000])
            f = plot.figure(f"Chaos {self.rnd.randint(1, 99)}")
            xs = list(range(n))
            ys = [self.rnd.gauss(0, 1) for _ in range(min(n, 5000))] * (n // min(n, 5000))
            f[0].line(xs[:len(ys)], ys)
            self.figs.append(f)
            return f"figure with {n} points"
        f = self.rnd.choice(self.figs)
        if how < 0.45:
            try:
                f.close()
            except Exception:  # noqa: BLE001
                pass
            self.figs.remove(f)
            return "close figure"
        if how < 0.6:
            f.Dock(self.rnd.choice([ycDockRight, ycDockBottom, ycDockLeft]), Ratio=self.rnd.choice([0.2, 0.35, 0.5]))
            return "dock figure"
        wins = [w for w in ui.windows() if w["label"].startswith("Chaos")]
        if not wins:
            return "no figure window"
        w = self.rnd.choice(wins)
        r = w["rect"]
        if how < 0.8:
            x, y = r[0] + 60, r[1] + 10
            self.hand_drag(x, y, self.rnd.randint(50, 1500), self.rnd.randint(150, 850))
            return f"drag figure {w['label']}"
        x, y = (r[0] + r[2]) / 2, (r[1] + r[3]) / 2
        act = self.rnd.choice([f"Q:0,0", f"c:{x:.0f},{y:.0f}", f"m:{x:.0f},{y:.0f}|r:|w:4|k:Escape",
                               f"d:{x:.0f},{y:.0f},{x + 80:.0f},{y + 40:.0f}"])
        if act.startswith("Q"):
            act = f"D:{x:.0f},{y:.0f}"
        self.inp(act + "|w:4")
        return f"mouse on figure: {act}"

    def a_pane(self):
        from yc import ui

        name = self.rnd.choice(PANES)
        on = self.rnd.random() < 0.6
        try:
            ui.pane(name, on)
        except Exception as e:  # noqa: BLE001
            return f"pane {name} -> {str(e)[:80]}"
        return f"pane {name} {'on' if on else 'off'}"

    def hand_drag(self, x, y, tx, ty):
        x, y, tx, ty = int(x), int(y), int(tx), int(ty)
        steps = "|".join(f"m:{x + (tx - x) * k // 10},{y + (ty - y) * k // 10}|w:2" for k in range(1, 11))
        self.inp(f"m:{x},{y}|w:4|p:{x},{y}|w:3|{steps}|w:6|u:|w:8")

    def a_dock_window(self):
        """Dock the Error Log at an edge or take it out again a random way: a window the user docks must come out."""
        from yc import ui

        def win(part):
            return next((w for w in ui.windows() if part in w["name"]), None)

        if not win("yc_errors"):
            ui.command("ErrorLog")
            self.inp("w:8")
        w, grid = win("yc_errors"), win("yc_gridhost")
        if not w or not grid:
            return "no error log / grid window"
        if not w["docked"]:
            g = grid["rect"]
            side = self.rnd.choice(["bottom", "left", "right", "top"])
            tx, ty = {"bottom": ((g[0] + g[2]) / 2, g[3] - 22), "left": (g[0] + 22, (g[1] + g[3]) / 2),
                      "right": (g[2] - 22, (g[1] + g[3]) / 2), "top": ((g[0] + g[2]) / 2, g[1] + 22)}[side]
            self.hand_drag(w["rect"][0] + 60, w["rect"][1] + 12, tx, ty)
            return f"dock Error Log {side} -> docked={bool((win('yc_errors') or {}).get('docked'))}"
        if w.get("floating"):
            # in a group that floats over the sheets: alone there, it is out of the dock already, and a drag of its tab
            # moves the group, as ImGui moves a node of one window (seed 7639 reported it stuck)
            wr = w["rect"]
            mates = [x for x in ui.windows() if x.get("docked") and x.get("floating") and "yc_errors" not in x["name"] and x.get("rect")
                     and x["rect"][0] < wr[2] and wr[0] < x["rect"][2] and x["rect"][1] < wr[3] and wr[1] < x["rect"][3]]
            if not mates:
                return "Error Log floats alone in its group"
        tab = w.get("tab") or [w["rect"][0], w["rect"][1], w["rect"][0] + 100, w["rect"][1] + 26]
        # what floats over the sheets: a floating window, or a group of docked windows floating as one
        over = [x for x in ui.windows() if x.get("visible", True) and (not x.get("docked") or x.get("floating"))
                and x.get("rect") and "yc_errors" not in x["name"]]
        # a floating window takes the mouse a few pixels outside its edge too (ImGui's resize border): a press 5 px
        # above one resized it instead of grabbing the tab (seed 5001). Grab the tab where it shows clear of them.
        pad = 8

        def covered(px, py):
            return next((x for x in over if x["rect"][0] - pad <= px <= x["rect"][2] + pad
                         and x["rect"][1] - pad <= py <= x["rect"][3] + pad), None)

        # its own tab (in a group the first tab is another's): the middle first, then other points along it
        points = [((tab[0] + tab[2]) / 2, (tab[1] + tab[3]) / 2)] + [
            (tab[0] + (tab[2] - tab[0]) * fx, tab[1] + (tab[3] - tab[1]) * fy) for fy in (0.5, 0.3, 0.7) for fx in (0.3, 0.7)]
        # and on the screen: a floating group dragged past the window's edge shows its tab nowhere (seed 5031 grabbed
        # x 2053 in a 1600 wide window); a person drags the group back first
        on_screen = [p for p in points if 0 <= p[0] < WINDOW[0] and 0 <= p[1] < WINDOW[1]]
        # and clear of its own node's edges: the splitter between two docked nodes takes a press a few pixels inside
        # them, and a grab there resized the node instead (seed 5338)
        wr = w["rect"]
        on_screen = [p for p in on_screen if wr[0] + 6 <= p[0] <= wr[2] - 6]
        if not on_screen:
            return "Error Log tab off the screen"
        free = [p for p in on_screen if not covered(*p)]
        if not free:   # a floating window lies over the tab: the user sees no tab there to grab
            return f"Error Log tab under {covered(*on_screen[0])['label']}"
        tx, ty = free[0]
        how = self.rnd.choice(["drag", "double", "menu"])
        drop = None
        if how == "drag":
            # over the sheets, away from the center: a window dropped on the docking targets (drawn in the middle of
            # the window under the mouse) docks there again, as a user who aims at them wants
            # well below the tab bar: a short drag along the bar only reorders the tabs (a group over the sheets puts
            # the sheets' corner right under its tabs)
            # and not over a floating window: dropped there, it docks into that window (a user aiming at the sheets
            # sees the figure in the way)
            # and well inside the sheets, 60 px from their top and bottom: the windows docked around them move when this
            # one comes out, and a point near the sheets' edge then lay on the tab bar of a figure docked under them,
            # where it docks as a tab (seed 5679: the sheets were 50 px tall); sheets too short to aim at: double-click
            g = grid["rect"]
            # and not over itself, docked beside the sheets or in a group floating over them: dropped back on its own place
            # it docks there again (seed 7828 inside its floating group, 7839 on its node left of the sheets)
            floating = [x["rect"] for x in over] + ([w["rect"]] if w.get("rect") else [])
            dy = min(max(g[1] + 60, ty + 150), g[3] - 60, 860)
            for px, py in [(g[0] + min(60, (g[2] - g[0]) / 5), dy), (g[2] - min(60, (g[2] - g[0]) / 5), dy),
                           (g[0] + min(60, (g[2] - g[0]) / 5), g[3] - 60), (g[2] - min(60, (g[2] - g[0]) / 5), g[3] - 60)]:
                if g[3] - g[1] < 160 or g[2] - g[0] < 400:  # too narrow too: the node's own docking targets cover it (seed 7839)
                    break
                # (and 100 px clear of the window's sides: ImGui docks a window dropped there at that side of the whole
                # dock space, seed 7839 dropped it 50 px from the left and it docked on the left again)
                if g[0] <= px <= g[2] and 100 <= px <= WINDOW[0] - 100 and g[1] + 60 <= py <= g[3] - 60 and not any(r[0] <= px <= r[2] and r[1] <= py <= r[3] for r in floating):
                    drop = (px, py)
                    break
            if drop is None:
                how = "double"   # the sheets are covered: nowhere a user could drop it
        if how == "drag":
            self.hand_drag(tx, ty, *drop)
        elif how == "double":
            self.inp(f"m:{tx:.0f},{ty:.0f}|w:4|D:{tx:.0f},{ty:.0f}|w:10")
        else:
            self.inp(f"m:{tx:.0f},{ty:.0f}|w:4|r:|w:6|k:DownArrow|w:2|k:UpArrow|w:2|k:Enter|w:10")
        w = win("yc_errors")
        if w and w["docked"] and how != "menu":
            # what the drag saw, for triage: the tab grabbed, where it was dropped, and every window around
            self.log("   ", f"tab {tx:.0f},{ty:.0f} drop {drop} over before " + "; ".join(
                f"{x['name']} {[round(v) for v in x['rect']]}" for x in over) + " | windows after " + "; ".join(
                f"{x['name']} {[round(v) for v in x.get('rect') or []]} docked={x.get('docked')} floating={x.get('floating')}"
                for x in ui.windows() if x.get("visible", True)))
            self.problem("STUCK_DOCKED", f"the Error Log didn't come out of its dock ({how})")
        return f"undock Error Log by {how}"

    def a_example(self):
        import example_build

        name = self.rnd.choice(EXAMPLES)
        try:
            example_build.run_here(name)
        except Exception as e:  # noqa: BLE001
            return f"example {name} -> {str(e)[:120]}"
        time.sleep(1.5)
        return f"example {name}"

    def a_save_reopen(self):
        from yc import excel

        path = os.path.join(self.out, f"chaos_{self.rnd.randint(1, 3)}.ycg")
        try:
            excel.ActiveWorkbook.SaveAs(path)
        except Exception as e:  # noqa: BLE001
            return f"save as -> {str(e)[:120]}"
        if self.rnd.random() < 0.5:
            return "save as"
        self.p.terminate()
        # a debug build's large working set takes a while to be freed on a busy machine: 15 s ran out twice beside a
        # build (chaos seeds 5648, 5650), and the TerminateProcess exit code 1 then read as a crash
        self.p.wait(120)
        p = self.yc.launch(path, hidden=True, exe=self.a.exe, lang=self.a.lang, size=WINDOW)
        self.p, self.conn = p, self.yc.connect(target=p.pid)
        end = time.time() + 60
        while not self.conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
            time.sleep(0.2)
        self.figs, self.seen_err = [], 0
        return "save as, quit, reopen"

    def a_wait(self):
        s = self.rnd.choice([0.2, 0.5, 1, 2])
        time.sleep(s)
        return f"wait {s}s"

    def run(self):
        acts = [(self.a_click, 12), (self.a_rapid_clicks, 3), (self.a_type, 14), (self.a_select, 5), (self.a_keys, 14),
                (self.a_mouse_drag, 5), (self.a_resize_header, 3), (self.a_right_click_menu, 6), (self.a_ribbon_click, 8),
                (self.a_command, 10), (self.a_undo_storm, 4), (self.a_sheet, 4), (self.a_python_writes, 5),
                (self.a_figure, 5), (self.a_pane, 3), (self.a_dock_window, 3), (self.a_example, 1),
                (self.a_save_reopen, 1), (self.a_wait, 2)]
        funcs = [f for f, w in acts for _ in range(w)]
        self.log("seed", self.a.seed, "minutes", self.a.minutes, "exe", self.a.exe)
        self.health("start")
        stop = time.time() + self.a.minutes * 60
        n = 0
        while time.time() < stop and (not self.a.actions or n < self.a.actions):
            f = self.rnd.choice(funcs)
            n += 1
            t0, mark = time.time(), self.cpu_times()
            try:
                what = f()
            except Exception as e:  # noqa: BLE001
                what = f"{f.__name__} raised {type(e).__name__}: {str(e)[:200]}"
                if self.p.poll() is None and ("timed out" in str(e).lower() or "E_TIMEOUT" in str(e)):
                    self.problem("TIMEOUT", f"{what}; input: {getattr(self, 'last_script', '')!r}; windows: {self.native_windows()}")
                else:
                    self.log("   (client)", what)
            dt = time.time() - t0
            self.log(f"{n:5d} {dt:5.2f}s {what}")
            big, self.big_step = getattr(self, "big_step", False), False
            if dt > 10 * self.slow_scale * (5 if big else 1):
                self.slow("SLOW_ACTION", f"{what} took {dt:.1f}s", mark)
            self.health(what)
        self.log("done:", n, "actions")
        try:
            from yc.mcp import tools

            con = str(tools.yc_read_console(max_lines=3000, min_level="error"))
            lines = [ln for ln in con.splitlines() if ln.strip() and not ln.startswith("next_cursor")]
            self.log("console errors:", len(lines), "lines")
            # one problem per error: its first line ("[where] Type: message"); the traceback lines under it and the
            # errors of user formulas (PY cells, UDFs) fed with junk are only logged
            heads = [ln for ln in lines if re.match(r"^\s*\d*\s*!?\s*\[(?!UDF )", ln)]
            for ln in lines[:60]:
                self.log("   ", ln[:300])
            for ln in heads[:40]:
                self.problem("CONSOLE", ln[:300])
        except Exception as e:  # noqa: BLE001
            self.log("console read failed:", repr(e))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--minutes", type=float, default=5)
    ap.add_argument("--exe", default=os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    ap.add_argument("--lang", default="en")
    ap.add_argument("--actions", type=int, default=0, help="stop after this many actions (replaying a seed)")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(a.exe)), "python"))
    faulthandler.dump_traceback_later(a.minutes * 60 + 900, exit=True)  # never a run that hangs forever
    c = Chaos(a)
    try:
        c.run()
    except Exception:  # noqa: BLE001
        c.problem("SCRIPT", traceback.format_exc()[-1500:])
    finally:
        c.log("PROBLEMS:", len(c.problems))
        for k, d in c.problems:
            c.log("  ", k, d[:600])
        try:
            c.p.terminate()
        except Exception:  # noqa: BLE001
            pass
    return 1 if c.problems else 0


if __name__ == "__main__":
    sys.exit(main())
