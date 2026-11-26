# SPDX-License-Identifier: Apache-2.0
"""Against a running YCGrid (hidden, with a temporary project): the host's events and shutdown, figures across
connections, plot interaction through plot.input, topics, orders. Skipped without a built ycgrid.exe
(dist_release or dist_debug) or with YC_NO_LIVE=1."""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# YCGRID_EXE runs the tests on another build (dist_debug: its asserts catch out-of-range reads that release passes)
EXE = os.environ.get("YCGRID_EXE") or next(
    (p for p in (os.path.join(ROOT, d, "ycgrid.exe") for d in ("dist_release", "dist_debug")) if os.path.exists(p)), None)

MODULE = '''from yc.excel import *
import time
calls = []
sheet_changes = []

def Setup():
    Range("A1:A10").OnChange = "T.OnEdit"

def OnEdit(target):
    calls.append(target.Address)

def Writes():
    t0 = time.perf_counter()
    for i in range(50):
        Range("A1:A10").Value = [[i]] * 10
    Worksheets(1).Activate()
    used = ActiveSheet.UsedRange.Address
    return [time.perf_counter() - t0, used, len(calls), len(sheet_changes)]

def Counts():
    return [len(calls), len(sheet_changes)]

panel_clicks = []

def OnPanel():
    panel_clicks.append(1)

def PanelClicks():
    return len(panel_clicks)

user_edits = []

def SetupUserOnly():
    Application.OnChange("C1:C5", lambda t: user_edits.append(t.Address), Sources=ycUser)

def UserEdits():
    return list(user_edits)

ticks = []
slow_calls = []
subs = {}

def SubscribeTicks():
    subs["fast"] = Application.Topics.Subscribe(["LT.px"], lambda b: ticks.extend((t.seq, t.value, t.name) for t in b))
    subs["slow"] = Application.Topics.Subscribe("LT.px", _slow)
    return [subs["fast"].Id, subs["slow"].Id]

def _slow(batch):
    slow_calls.append(time.perf_counter())
    if len(slow_calls) == 1:
        time.sleep(1.0)

def TickReport():
    s = subs["fast"]
    seqs = [t[0] for t in ticks]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b - a != 1)
    vals = [t[1] for t in ticks]
    in_order = all(b > a for a, b in zip(vals, vals[1:]))
    return [len(ticks), gaps, in_order, seqs[0] if seqs else None, s.Dropped, len(slow_calls), ticks[0][2] if ticks else None]

def Unsubscribe():
    for s in subs.values():
        s.Unsubscribe()
    return [s.Active for s in subs.values()]
'''

SHEET = '''from yc.excel import *

def Worksheet_Change(Target):
    import modules.T as T
    T.sheet_changes.append(Target.Address)
'''


def series_ref(series, part):
    """The range of a chart series' X values (part 1) or values (part 2), without the sheet, from its =SERIES formula."""
    args = series.Formula[len("=SERIES("):-1].split(",")
    return args[part].split("!")[-1]


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class LiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="yclive_")
        cls.proj = os.path.join(cls.tmp, "proj")
        py = os.path.join(os.path.dirname(EXE), "runtime", "python", "python.exe")
        subprocess.run([py, "-E", "-s", "-c", f"import sys;sys.path.insert(0,r'{os.path.join(ROOT, 'python')}');from yc.cli import main;sys.exit(main())",
                        "init", cls.proj, "--sheet", "Sheet1=Sheet1"], check=True, capture_output=True)
        cls.marker = os.path.join(cls.proj, "events.txt").replace("\\", "/")
        with open(os.path.join(cls.proj, "modules", "T.py"), "w", encoding="utf-8") as f:
            f.write(MODULE)
        with open(os.path.join(cls.proj, "sheets", "Sheet1.py"), "a", encoding="utf-8") as f:
            f.write("\n\n" + SHEET.split("\n", 1)[1])
        with open(os.path.join(cls.proj, "main.py"), "a", encoding="utf-8") as f:
            f.write(f'''

def Workbook_HostShutdown():
    with open("{cls.marker}", "a") as fh:
        fh.write("shutdown\\n")

def Workbook_BeforeClose(cancel):
    with open("{cls.marker}", "a") as fh:
        fh.write("beforeclose\\n")
''')
        book = os.path.join(cls.proj, "Book.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=p0.pid)
            from yc import excel

            excel.ThisWorkbook.SaveAs(book)
        finally:
            p0.terminate()
            p0.wait(10)
        cls.proc = yc.launch(book, hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        cls.wait_host()

    @classmethod
    def wait_host(cls, timeout=30):
        t0 = time.time()
        while time.time() - t0 < timeout:
            st = cls.conn.request("python.status", {})
            if st.get("ready") and st.get("workbook_open_done"):
                return st
            time.sleep(0.2)
        raise AssertionError("the Python host didn't start")

    @classmethod
    def tearDownClass(cls):
        try:
            cls.proc.terminate()
            cls.proc.wait(10)
        finally:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def job(self, name=None, code=None, timeout_ms=30000):
        p = {"kind": "macro", "name": name} if name else {"kind": "exec", "code": code}
        p["timeout_ms"] = timeout_ms
        r = self.conn.request("host.job", p, timeout=timeout_ms / 1000 + 20)
        self.assertTrue(r.get("ok"), r.get("error"))
        return r.get("result")

    # ---- B2: writes from a host macro don't wait for their own events ----
    def test_b2_macro_writes_with_onchange(self):
        self.job("T.Setup")
        took, used, during, sheet_during = self.job("T.Writes")
        self.assertLess(took, 1.0)
        self.assertEqual(used, "$A$1:$A$10")
        self.assertEqual((during, sheet_during), (0, 0))  # the handlers run after the macro, not inside it
        for _ in range(50):
            calls, sheet = self.job("T.Counts")
            if calls and sheet:
                break
            time.sleep(0.1)
        self.assertGreater(calls, 0)
        self.assertGreater(sheet, 0)

    # ---- B4 / F6: figures belong to their connection; keys find them again ----
    def test_b4_f6_figures(self):
        from yc import plot

        self.job(code="from yc import plot\nplot.figure('Host temp', key='live.host')\nplot.figure('Host kept', key='live.kept', persist=True)\nNone")
        keys = {f["key"]: f for f in plot.figures()}
        self.assertEqual(keys["live.host"]["owner"], "host")
        self.assertTrue(keys["live.kept"]["persist"])
        # a script takes the host's window over by key (reuse keeps its title and layout)
        fig = plot.figure(key="live.kept", reuse=True)
        self.assertEqual(fig.id, keys["live.kept"]["id"])
        info = {f["key"]: f for f in plot.figures()}["live.kept"]
        self.assertEqual(info["title"], "Host kept")
        self.assertTrue(info["owner"].startswith("script"))
        self.job(code="from yc import plot\nplot.figure('Host 2', key='live.host2')\nNone")
        open(self.marker, "w").close()
        self.conn.request("host.restart", {})
        time.sleep(1.0)
        self.wait_host()
        keys = {f["key"]: f for f in plot.figures()}
        self.assertNotIn("live.host", keys)    # not persistent: closed with the host
        self.assertNotIn("live.host2", keys)
        self.assertIn("live.kept", keys)       # taken over by this script: still alive
        self.assertTrue(keys["live.kept"]["alive"])
        self.assertTrue(plot.close(key="live.kept"))
        self.assertNotIn("live.kept", {f["key"] for f in plot.figures()})
        # B1: a restart runs Workbook_HostShutdown, not Workbook_BeforeClose
        with open(self.marker) as fh:
            self.assertEqual(fh.read().split(), ["shutdown"])

    # ---- F5 / F3 / F4 / B7 / F8: layout, bands, tags, right clicks and the context menu ----
    def test_plot_interaction(self):
        from yc import plot, ui

        fig = plot.figure("Live plot", rows=2, height_ratios=[4, 1], key="live.plot")
        ax = fig[0]
        ax.line([0, 1, 2, 3], [10, 12, 11, 13])
        ax.hband([10.5], [11.0], text="S1", color="#3300c8ff", edge="#00c8ff")
        ax.hline([12.5], tag=True, text="STOP", fmt="%.1f", style="dash", color="red")
        moved = []
        band = ax.drag_band(11.2, 11.6, label="Z1", on_release=lambda h: moved.append((h.y0, h.y1)))
        got = []
        fig.on("click", got.append, right=True)
        fig.on("select", got.append)
        chosen = []
        fig.context_menu([("Add support here", lambda e: chosen.append(e.y)), None, ("Delete", None)])
        time.sleep(0.3)
        fig.input("click", x=1.0, y=11.5, button="right")
        fig.input("click", x=2.0, y=12.0, ctrl=True)
        fig.input("select", x0=0.5, x1=2.5, y0=10.0, y1=12.0)
        fig.input("drag_band_edge", handle=band, edge="top", y=12.2)
        fig.input("context_menu", text="Add support here", x=1.5, y=10.7)
        for _ in range(60):
            if len(got) >= 3 and moved and chosen:
                break
            time.sleep(0.05)
        clicks = [e for e in got if e.type == "click"]
        self.assertEqual(clicks[0].button, "right")
        self.assertTrue(clicks[1].ctrl)
        self.assertEqual([e.type for e in got if e.type == "select"], ["select"])
        self.assertEqual(moved[-1], (11.2, 12.2))
        self.assertEqual(chosen, [10.7])
        st = [f for f in ui.state()["figures"] if f["key"] == "live.plot"][0]
        self.assertEqual(st["menu"], ["Add support here", "", "Delete"])
        self.assertEqual(st["plots"][0]["handles"][0]["label"], "Z1")
        self.assertEqual({i["kind"] for i in st["plots"][0]["items"]}, {"line", "hband", "hline"})
        plot.close(key="live.plot")


    # ---- F12 / F14: panel styling, panel and plot input through the MCP tools ----
    def test_panels_and_mcp_input(self):
        from yc import plot, ui
        from yc import excel
        from yc.constants import ycDockRight
        from yc.mcp import tools

        if excel.Application.Panels.Exists("LiveDesk"):
            excel.Application.Panels("LiveDesk").Delete()
        self.assertFalse(excel.Application.Panels.Exists("LiveDesk"))
        p = excel.Application.Panels.Add("LiveDesk", Caption="Live desk", Dock=ycDockRight, Width=220)
        self.assertTrue(excel.Application.Panels.Exists("LiveDesk"))
        b = p.Controls.Add("button", "Buy", Caption="BUY", OnAction="T.OnPanel")
        b.BackColor, b.ForeColor, b.FontSize, b.Bold, b.Height = 192, 0xFFFFFF, 14, True, 30
        lab = p.Controls.Add("label", "PnL", Caption="P&L", LinkedCell="Z1")
        lab.NumberFormat = "[Red]#,##0;[Green]-#,##0"
        self.assertEqual((b.BackColor, b.ForeColor, b.FontSize, b.Bold, b.Height), (192, 0xFFFFFF, 14, True, 30))
        self.assertEqual(lab.NumberFormat, "[Red]#,##0;[Green]-#,##0")
        before = self.job("T.PanelClicks")
        tools.yc_panel_input("LiveDesk", "Buy")
        for _ in range(50):
            if self.job("T.PanelClicks") > before:
                break
            time.sleep(0.1)
        self.assertEqual(self.job("T.PanelClicks"), before + 1)
        # a figure's select callback, from yc_plot_input
        fig = plot.figure("MCP plot", key="live.mcp")
        fig[0].line([0, 1, 2], [1, 2, 3])
        got = []
        fig.on("select", got.append)
        h = fig[0].drag_line(2.0, label="LMT", on_release=lambda hh: got.append(("release", hh.value)))
        time.sleep(0.2)
        r = tools.yc_plot_input("live.mcp", [{"action": "select", "x0": 0, "x1": 1, "y0": 0, "y1": 2},
                                             {"action": "drag_handle", "handle": "LMT", "y": 2.5}])
        self.assertEqual(r["done"], ["select", "drag_handle"])
        for _ in range(60):
            if len(got) >= 2:
                break
            time.sleep(0.05)
        self.assertEqual(got[0].type, "select")
        self.assertIn(("release", 2.5), got)
        self.assertEqual(h.value, 2.5)
        plot.close(key="live.mcp")
        p.Delete()


    # ---- F13: OnChange for the user's edits only ----
    def test_f13_user_only_onchange(self):
        from yc import excel
        from yc.mcp import tools

        self.job("T.SetupUserOnly")
        excel.Worksheets(1).Range("C2").Value = 5          # a code write: not the user's
        self.job(code="Range('C3').Value = 7; None")       # nor the host's
        time.sleep(0.5)
        self.assertEqual(self.job("T.UserEdits"), [])
        tools.yc_ui_input([{"type": "select", "address": "C4"}, {"type": "type", "text": "9"}])
        for _ in range(40):
            if self.job("T.UserEdits"):
                break
            time.sleep(0.1)
        self.assertEqual(self.job("T.UserEdits"), ["$C$4"])

    # ---- F1: every topic update reaches the host's subscription, in order ----
    def test_f1_topic_subscribe(self):
        from yc import excel, ui
        from yc.errors import ApplicationDefinedError

        with self.assertRaises(ApplicationDefinedError):  # scripts read topics through cells
            excel.Application.Topics.Subscribe("LT.px", print)
        self.job("T.SubscribeTicks")
        n, batch = 100_000, 2000
        topics = excel.Application.Topics
        t0 = time.perf_counter()
        for i in range(0, n, batch):
            topics.Push(["LT.px"] * batch, [float(v) for v in range(i, i + batch)])
        push_s = time.perf_counter() - t0
        t_ui = time.perf_counter()
        ui.state()  # the UI answers while the slow subscription sleeps
        ui_s = time.perf_counter() - t_ui
        for _ in range(300):
            got, gaps, in_order, first, dropped, slow, name = self.job("T.TickReport")
            if got >= n and slow > 1:
                break
            time.sleep(0.1)
        self.assertEqual((got, gaps, in_order, first, dropped, name), (n, 0, True, 1, 0, "LT.px"), f"push took {push_s:.2f} s")
        self.assertGreater(slow, 1)
        self.assertLess(ui_s, 1.0)
        stats = topics.Stats["Subscriptions"]
        self.assertEqual(len(stats), 2)
        self.assertEqual(sorted(x["Delivered"] for x in stats), [n, n])
        self.assertEqual(self.job("T.Unsubscribe"), [False, False])
        self.assertEqual(topics.Stats["Subscriptions"], [])

    # ---- F2: candles built by YCGrid from topic updates, the same bars as yc.bars.BarBuilder ----
    def test_f2_candles_live(self):
        import random

        from yc import bars, excel, plot

        fig = plot.figure("Live candles", rows=2, height_ratios=[3, 1], key="live.candles")
        candles = fig[0].candles_live("LT.c", seconds=1, volume_topic="LT.v", volume_ax=fig[1], keep=100)
        line = fig[0].line_live("LT.c", capacity=1000)
        time.sleep(0.2)
        rng = random.Random(7)
        base = (int(time.time()) // 10) * 10.0
        bb = bars.BarBuilder(seconds=1)
        px = 23000.0
        for i in range(40):
            t = base + i * 0.27
            px += rng.choice([-2, -1, 0, 1, 2])
            qty = rng.randint(1, 5)
            bb.add(t, px, qty)
            excel.Application.Topics.Push(["LT.c", "LT.v"], [px, qty], TimeNs=int(round(t * 1e9)))
        want = bb.all()
        for _ in range(100):
            got = candles.data()
            if len(got.get("x", [])) == len(want) and got["close"][-1] == want[-1].close and len(line.data().get("x", [])) == 40:
                break
            time.sleep(0.05)
        self.assertEqual(got["x"], [b.start for b in want])
        for col in ("open", "high", "low", "close", "volume"):
            self.assertEqual(got[col], [getattr(b, col) for b in want], col)
        self.assertEqual(candles.volume.data()["y"], [b.volume for b in want])
        self.assertEqual(len(line.data()["y"]), 40)
        plot.close(key="live.candles")

    # ---- F8: a closable drag line (the block an order line is built from: drag = new price, x = cancel) ----
    def test_f8_closable_line(self):
        from yc import plot, ui

        fig = plot.figure("Closable", key="live.close")
        fig[0].line([0, 1, 2], [95, 100, 105])
        events = []
        h = fig[0].drag_line(96, label="BUY LMT 1 @ 96", on_release=lambda hh: events.append(("move", hh.value)),
                             on_close=lambda hh: events.append(("close", hh.id)))
        plain = fig[0].drag_line(99, label="plain")
        time.sleep(0.2)
        st = [f for f in ui.state()["figures"] if f["key"] == "live.close"][0]
        hs = {x["label"]: x for x in st["plots"][0]["handles"]}
        self.assertTrue(hs["BUY LMT 1 @ 96"].get("closable"))
        self.assertFalse(hs["plain"].get("closable", False))
        fig.input("drag_handle", handle=h, y=97)
        fig.input("close_handle", handle=h)
        with self.assertRaises(Exception):
            fig.input("close_handle", handle=plain)  # no x button
        for _ in range(60):
            if len(events) >= 2:
                break
            time.sleep(0.05)
        self.assertEqual(events, [("move", 97.0), ("close", h.id)])
        plot.close(key="live.close")

    # ---- F7: figures dock like sheets and panels; keyed ones come back by key in layouts ----
    def test_f7_dock_figures(self):
        import json

        from yc import excel, plot, ui
        from yc.constants import ycDockRight

        def state(key):
            for _ in range(40):
                st = [f for f in ui.state()["figures"] if f["key"] == key]
                if st and "docked" in st[0]:
                    return st[0]
                time.sleep(0.05)
            return st[0] if st else {}

        def wait_docked(key, want):
            for _ in range(60):
                st = state(key)
                if st.get("docked") == want:
                    return st
                time.sleep(0.05)
            return st

        fig = plot.figure("Docked", key="live.dock")
        fig[0].line([0, 1, 2], [1, 3, 2])
        self.assertFalse(state("live.dock").get("docked"))
        fig.Dock(ycDockRight, Ratio=0.4)
        main_root = wait_docked("live.dock", True).get("dock_root")
        self.assertTrue(state("live.dock")["docked"])
        ui.Layout.Save("LiveDesk")
        js = json.loads(excel.ThisWorkbook.Store["yc.layout.LiveDesk"])
        self.assertIn("###yc_figk_live.dock", json.dumps(js["dock"]))
        fig.Float(Left=40, Top=40, Width=400, Height=300)
        self.assertFalse(wait_docked("live.dock", False)["docked"])
        ui.Layout.Restore("LiveDesk")
        self.assertTrue(wait_docked("live.dock", True)["docked"])
        # closed and made again with the same key (another run of the script): back in its place
        plot.close(key="live.dock")
        time.sleep(0.2)
        fig = plot.figure("Docked again", key="live.dock")
        self.assertTrue(wait_docked("live.dock", True)["docked"])
        # into a container
        c = ui.Containers.Add("LiveDeskBox")
        fig2 = plot.figure("In a box", key="live.box")
        fig2.Dock(ycDockRight, Container=c)
        st = wait_docked("live.box", True)
        self.assertTrue(st["docked"])
        self.assertNotEqual(st.get("dock_root"), main_root)
        ui.Layout.Delete("LiveDesk")
        plot.close(key="live.dock")
        plot.close(key="live.box")
        c.Delete()

    # ---- R1: docking a keyed figure again (it came back docked) must not crash ----
    def test_redock_same_key(self):
        from yc import plot
        from yc.constants import ycDockBottom, ycDockLeft, ycDockRight

        for i in range(3):
            fig = plot.figure(f"Redock {i}", key="live.redock")
            fig[0].line([0, 1], [0, 1])
            for pos in (ycDockRight, ycDockRight, ycDockBottom, ycDockLeft):
                fig.Dock(pos, Ratio=0.4)
                for _ in range(40):
                    if fig.Docked:
                        break
                    time.sleep(0.05)
                self.assertTrue(fig.Docked)
            plot.close(key="live.redock")
            time.sleep(0.2)
        self.assertTrue(self.conn.request("python.status", {}).get("ready"))  # still alive

    # ---- the hang watchdog writes a dump when the UI thread stops answering ----
    def test_hang_dump(self):
        import glob

        from yc import ui

        folder = self.conn.request("debug.ui_stall", {"seconds": 0}, timeout=10)
        before = set(glob.glob(os.path.join(folder, "ycgrid-hang-*.dmp")))
        self.conn.request("debug.ui_stall", {"seconds": 13}, timeout=40)
        new = set(glob.glob(os.path.join(folder, "ycgrid-hang-*.dmp"))) - before
        self.assertEqual(len(new), 1, folder)
        dump = new.pop()
        self.assertGreater(os.path.getsize(dump), 10_000)
        os.remove(dump)
        self.assertTrue(any("stopped responding" in str(e) for e in ui.errors()))

    # ---- panel colors, handle colors, a figure's dock state ----
    def test_panel_and_handle_colors(self):
        from yc import excel, plot, ui

        if excel.Application.Panels.Exists("LiveDark"):
            excel.Application.Panels("LiveDark").Delete()
        p = excel.Application.Panels.Add("LiveDark", Caption="Dark")
        self.assertIsNone(p.BackColor)
        p.BackColor = 0x120E0D
        p.ForeColor = 0xE0E0E0
        self.assertEqual((p.BackColor, p.ForeColor), (0x120E0D, 0xE0E0E0))
        p.BackColor = None
        self.assertIsNone(p.BackColor)
        p.Delete()
        fig = plot.figure("Colors", key="live.colors")
        fig[0].line([0, 1, 2], [1, 2, 3])
        fig[0].set(follow=10, yfit="stable")
        band = fig[0].drag_band(1.2, 1.4, label="Z", color="#38bdf8")
        band.set(color="#ffffff", label="Z inside")
        self.assertFalse(fig.Docked)
        st = [f for f in ui.state()["figures"] if f["key"] == "live.colors"][0]
        self.assertEqual(st["plots"][0]["handles"][0]["label"], "Z inside")
        plot.close(key="live.colors")


    # ---- a following plot the user zooms stops following; the badge (or set(follow=)) goes back live ----
    def test_follow_pause(self):
        from yc import plot, ui

        fig = plot.figure("Follow", key="live.follow")
        now = time.time()
        fig[0].line([now - 100 + i for i in range(100)], list(range(100)))
        fig[0].set(follow=60, yfit="stable", ywidth="stable")
        fig[0].set(ywidth=70)
        got = []
        fig.on("follow", got.append)
        time.sleep(0.2)
        self.assertTrue(fig.following)
        fig.input("wheel", x=now - 20, y=50)
        for _ in range(40):
            if got:
                break
            time.sleep(0.05)
        self.assertFalse(fig.following)
        self.assertEqual(got[-1].paused, True)
        st = [f for f in ui.state()["figures"] if f["key"] == "live.follow"][0]
        self.assertTrue(st["follow_paused"])
        fig.input("live")
        self.assertTrue(fig.following)
        fig.input("wheel")
        fig[0].set(follow=60)   # set from code: follows again
        self.assertTrue(fig.following)
        plot.close(key="live.follow")

    # ---- gridlines and headings belong to each sheet, like Excel ----
    def test_gridlines_per_sheet(self):
        from yc import excel

        ws1 = excel.Worksheets(1)
        ws1.Activate()
        excel.ActiveWindow.DisplayGridlines = False
        excel.ActiveWindow.DisplayHeadings = False
        ws2 = excel.Worksheets.Add()
        ws2.Activate()
        try:
            self.assertTrue(excel.ActiveWindow.DisplayGridlines)   # a new sheet has its own (Excel's default)
            self.assertTrue(excel.ActiveWindow.DisplayHeadings)
            ws1.Activate()
            self.assertFalse(excel.ActiveWindow.DisplayGridlines)
            self.assertFalse(excel.ActiveWindow.DisplayHeadings)
        finally:
            excel.ActiveWindow.DisplayGridlines = True
            excel.ActiveWindow.DisplayHeadings = True
            excel.Application.DisplayAlerts = False
            ws2.Delete()


    # ---- the ribbon in tabs-only mode: the arrow, the same tab, a click elsewhere or Esc put it away ----
    def test_ribbon_peek(self):
        from yc import excel, ui

        def rb():
            return ui.state()["ribbon"]

        def click(x, y):
            self.conn.request("ui.input", {"script": f"m:{x:.0f},{y:.0f}|w:3|c:{x:.0f},{y:.0f}|w:8"}, timeout=20)
            time.sleep(0.6)   # past the double-click time (a double-click on a tab pins the ribbon)

        def tab(name):
            click(*rb()["tabs"][name])

        def arrow():
            click(*rb()["arrow"])

        excel.Application.RibbonDisplay = 1
        try:
            time.sleep(0.3)
            k = rb()["tabs"]["Home"][0] / 70.0       # the 1600-wide layout -> this window's pixels
            menu = (1012 * k, 87 * k)                 # Conditional Formatting's drop-down arrow (opens a menu)
            tab("Home")
            self.assertTrue(rb()["peek"])
            arrow()
            self.assertFalse(rb()["peek"])
            tab("Insert")
            tab("Data")
            self.assertEqual((rb()["peek"], list(rb()["tabs"])[rb()["tab"]]), (True, "Data"))
            tab("Data")
            self.assertFalse(rb()["peek"])
            for close in ("arrow", "tab", "sheet", "esc"):
                tab("Home")
                click(*menu)
                self.assertTrue(rb()["popup_open"], close)
                if close == "arrow":
                    arrow()
                elif close == "tab":
                    tab("Home")
                elif close == "sheet":
                    click(1000 * k, 700 * k)
                else:
                    self.conn.request("ui.input", {"script": "k:Escape|w:6|k:Escape|w:6"}, timeout=20)
                    time.sleep(0.3)
                self.assertFalse(rb()["peek"], close)
            self.assertEqual(excel.Application.RibbonDisplay, 1)   # still tabs-only (nothing pinned it)
        finally:
            excel.Application.RibbonDisplay = 0

    # ---- a refused entry in a hidden instance answers itself instead of waiting for nobody ----
    def test_refused_entry_does_not_block(self):
        from yc import excel, safety, ui
        from yc.mcp import tools

        ws = excel.Worksheets(1)
        ws.Range("E2").Value = 2
        safety.parameter(ws.Range("E2"), name="Max lots", type=int, min=1, max=10, confirm=False)
        try:
            t0 = time.time()
            tools.yc_ui_input([{"type": "select", "address": "E2"}, {"type": "type", "text": "11"}])
            self.assertLess(time.time() - t0, 10)
            self.assertEqual(ws.Range("E2").Value, 2)                  # refused
            tools.yc_ui_input([{"type": "select", "address": "E2"}, {"type": "type", "text": "4"}])
            self.assertEqual(ws.Range("E2").Value, 4)
            self.assertTrue(any("answered automatically" in str(e) for e in ui.errors()))
        finally:
            safety.remove_parameter(ws.Range("E2"))
            ws.Range("E2").ClearContents()

    # ---- a panel control linked to a parameter cell changes it through the parameter's checks ----
    def test_panel_control_on_parameter_cell(self):
        from yc import excel, safety
        from yc.mcp import tools

        ws = excel.Worksheets(1)
        ws.Range("F2").Value = 1
        safety.parameter(ws.Range("F2"), name="Qty", type=int, min=1, max=5, confirm=False)
        if excel.Application.Panels.Exists("LiveParam"):
            excel.Application.Panels("LiveParam").Delete()
        pnl = excel.Application.Panels.Add("LiveParam", Caption="Param")
        # the spin reaches 10, the parameter allows 5: the parameter's check refuses what the spin allows
        pnl.Controls.Add("spin", "spQty", Caption="Qty", Min=1, Max=10, LinkedCell=f"{ws.Name}!F2")
        try:
            tools.yc_panel_input("LiveParam", "spQty", "set_value", "3")
            for _ in range(30):
                if ws.Range("F2").Value == 3:
                    break
                time.sleep(0.1)
            self.assertEqual(ws.Range("F2").Value, 3)   # was refused as "a parameter cell" before
            tools.yc_panel_input("LiveParam", "spQty", "set_value", "9")
            time.sleep(0.5)
            self.assertEqual(ws.Range("F2").Value, 3)   # above the parameter's max: refused
            tools.yc_panel_input("LiveParam", "spQty", "set_value", "25")
            time.sleep(0.5)
            self.assertEqual(ws.Range("F2").Value, 3)   # past the spin's Max: it stops at 10, which is refused too
            self.assertTrue(any("Change" in e["kind"] or e["kind"] == "parameter" for e in safety.change_log(limit=20)))
        finally:
            pnl.Delete()
            safety.remove_parameter(ws.Range("F2"))
            ws.Range("F2").ClearContents()

@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class StaleSheetModuleTest(unittest.TestCase):
    """The module of a deleted sheet (sheets\\Sheet9.py) stays in the folder: the workbook opens without an error and
    the module isn't loaded; the console says why."""

    def test_reopen_after_the_sheet_was_deleted(self):
        tmp = tempfile.mkdtemp(prefix="ycstale_")
        book = os.path.join(tmp, "Stale.ycg")
        os.makedirs(os.path.join(tmp, "sheets"))
        with open(os.path.join(tmp, "main.py"), "w", encoding="utf-8") as f:
            f.write("from yc.excel import *\n")
        with open(os.path.join(tmp, "sheets", "Sheet9.py"), "w", encoding="utf-8") as f:
            f.write("from yc.excel import *\nMe = SheetModule(__name__)\n")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=p0.pid)
            from yc import excel

            excel.ThisWorkbook.SaveAs(book)
        finally:
            p0.terminate()
            p0.wait(10)
        p = yc.launch(book, hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=p.pid)
            from yc import ui

            end = time.time() + 60
            while not conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
                time.sleep(0.2)
            self.assertEqual([e for e in ui.errors() if e.get("error")], [])
            lines = [ln["text"] for ln in conn.request("console.read", {"max": 500})["lines"]]
            self.assertTrue(any("Sheet9.py" in ln and "isn't loaded" in ln for ln in lines), lines[-10:])
        finally:
            p.terminate()
            p.wait(10)
            shutil.rmtree(tmp, ignore_errors=True)


    def test_immediate_window_after_the_folder_was_deleted(self):
        tmp = tempfile.mkdtemp(prefix="ycgone_")
        p = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=p.pid)
            from yc import excel, ui

            excel.ThisWorkbook.SaveAs(os.path.join(tmp, "Gone.ycg"))
            shutil.rmtree(tmp, ignore_errors=True)   # moved or deleted while the workbook is open
            ui.pane("immediate", True)
            time.sleep(0.5)
            conn.request("ui.input", {"script": "t:?1+1|k:Enter|w:6", "timeout_ms": 8000}, timeout=20)
            end, texts = time.time() + 30, []
            while time.time() < end:
                texts = [ln["text"] for ln in conn.request("console.read", {"max": 200})["lines"]]
                if "2" in texts:
                    break
                time.sleep(0.3)
            self.assertIn("2", texts, texts[-8:])
            self.assertTrue(any("no longer exists" in t for t in texts), texts[-8:])
        finally:
            p.terminate()
            p.wait(10)
            shutil.rmtree(tmp, ignore_errors=True)


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class SavedLayoutTest(unittest.TestCase):
    """R8: a workbook saved with a docked panel since deleted, a docked keyed figure and per-sheet view settings opens
    again and again without a crash, with its settings."""

    def test_reopen(self):
        from yc.constants import ycDockRight

        tmp = tempfile.mkdtemp(prefix="yclayout_")
        book = os.path.join(tmp, "Layout.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=p0.pid)
            from yc import excel, plot

            old = excel.Application.Panels.Add("Old", Caption="Old", Dock=ycDockRight, Width=200)
            excel.Application.Panels.Add("Ticket", Caption="Ticket", Dock=ycDockRight, Width=200)
            fig = plot.figure("Chart", key="layout.chart")
            fig[0].line([0, 1], [0, 1])
            fig.Dock(ycDockRight, Ratio=0.6)
            time.sleep(0.5)
            excel.ActiveWindow.DisplayGridlines = False
            excel.ThisWorkbook.SaveAs(book)
            old.Delete()
            time.sleep(0.3)
            excel.ThisWorkbook.Save()
        finally:
            p0.terminate()
            p0.wait(10)
        try:
            for i in range(8):
                p = yc.launch(book, hidden=True, exe=EXE, lang="en")
                try:
                    conn = yc.connect(target=p.pid)
                    from yc import excel

                    time.sleep(0.8)
                    conn.request("ui.state", {}, timeout=10)
                    self.assertIsNone(p.poll(), f"launch {i} ended")
                    self.assertFalse(excel.ActiveWindow.DisplayGridlines)
                finally:
                    p.terminate()
                    p.wait(10)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class FeedRestartTest(unittest.TestCase):
    """R2: reloading the Python host while a data feed process is starting must never freeze the window."""

    def test_rapid_reloads_with_a_feed(self):
        tmp = tempfile.mkdtemp(prefix="ycfeed_")
        proj = os.path.join(tmp, "proj")
        py = os.path.join(os.path.dirname(EXE), "runtime", "python", "python.exe")
        subprocess.run([py, "-E", "-s", "-c", f"import sys;sys.path.insert(0,r'{os.path.join(ROOT, 'python')}');from yc.cli import main;sys.exit(main())",
                        "init", proj], check=True, capture_output=True)
        os.makedirs(os.path.join(proj, "feeds"), exist_ok=True)
        with open(os.path.join(proj, "feeds", "forever.py"), "w", encoding="utf-8") as f:
            f.write("import time\nwhile True:\n    time.sleep(0.05)\n")   # a feed that never ends on its own
        with open(os.path.join(proj, "ycgrid.toml"), "a", encoding="utf-8") as f:
            f.write('\n[[python.feeds]]\nname = "forever"\nscript = "feeds/forever.py"\nrestart = "always"\n')
        book = os.path.join(proj, "Book.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=p0.pid)
            from yc import excel

            excel.ThisWorkbook.SaveAs(book)
        finally:
            p0.terminate()
            p0.wait(10)
        proc = yc.launch(book, hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            for i in range(12):
                t0 = time.time()
                conn.request("ui.command", {"id": "ReloadPython"}, timeout=25)   # on the UI thread
                if i % 3 == 0:
                    time.sleep(0.03 * i)   # different points of the feed's start
                took = time.time() - t0
                self.assertLess(took, 20, f"reload {i} took {took:.1f} s")
                conn.request("ui.state", {}, timeout=10)   # the window answers
        finally:
            proc.terminate()
            proc.wait(10)
            shutil.rmtree(tmp, ignore_errors=True)


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class StreamReloadTest(unittest.TestCase):
    """An @stream behind =STREAM(name, topic): it was never found (the host looked for the function's own name),
    and after a reload of Python the formula kept the old host's subscription and stopped updating."""

    def test_stream_keeps_updating_after_reload(self):
        tmp = tempfile.mkdtemp(prefix="ycstream_")
        proj = os.path.join(tmp, "proj")
        os.makedirs(proj)
        with open(os.path.join(proj, "main.py"), "w", encoding="utf-8") as f:
            f.write("import threading, time\nfrom yc.excel import stream\n\n\n@stream(\"TICK\")\nclass Tick:\n"
                    "    def subscribe(self, topic):\n        def run():\n            n = 0\n            while True:\n"
                    "                n += 1\n                topic.Push([n, topic.Args[0]][0] if topic.Args == ('a',) else -1)\n"
                    "                time.sleep(0.1)\n        threading.Thread(target=run, daemon=True).start()\n\n"
                    "    def unsubscribe(self, topic):\n        pass\n")
        book = os.path.join(proj, "Book.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=p0.pid)
            from yc import excel

            excel.ThisWorkbook.SaveAs(book)
        finally:
            p0.terminate()
            p0.wait(10)
        proc = yc.launch(book, hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            from yc import excel, ui

            end = time.time() + 60
            while not conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
                time.sleep(0.2)
            ws = excel.Application.ActiveSheet
            ws.Range("A1").Formula = '=STREAM("TICK","a")'

            def moving():
                end = time.time() + 15
                first = None
                while time.time() < end:
                    v = ws.Range("A1").Value
                    if isinstance(v, float) and v > 0:
                        if first is None:
                            first = v
                        elif v > first:
                            return True
                    time.sleep(0.2)
                return ws.Range("A1").Value

            self.assertIs(moving(), True, "the @stream is found by its name, with the topics as Args")
            conn.request("ui.command", {"id": "ReloadPython"}, timeout=25)
            end = time.time() + 60
            time.sleep(1)
            while time.time() < end and not ui.state()["python"]["ready"]:
                time.sleep(0.2)
            self.assertIs(moving(), True, "it keeps updating after Python was reloaded")
        finally:
            proc.terminate()
            proc.wait(10)
            shutil.rmtree(tmp, ignore_errors=True)


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class UserFlowTest(unittest.TestCase):
    """What a trader / researcher does with the mouse and keyboard on a blank workbook (found by clicking through)."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.ws = excel.Worksheets.Add()
        self.ws.Activate()

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.3)

    def ohlc(self):
        self.ws.Range("A1:E1").Value = [["Date", "Open", "High", "Low", "Close"]]
        self.ws.Range("A2:E11").Value = [[46000 + i, 100 + i, 103 + i, 98 + i, 101 + i] for i in range(10)]
        self.ws.Range("A2:A11").NumberFormat = "yyyy-mm-dd"

    def test_immediate_window_errors_stay_in_the_window(self):
        from yc import ui

        ui.pane("immediate", True)
        try:
            time.sleep(0.5)
            self.keys("t:1+|k:Enter|w:6")   # its input has the keyboard; Python starts for it
            end = time.time() + 30
            lines = []
            while time.time() < end:
                lines = self.conn.request("console.read", {"max": 200})["lines"]
                if any("SyntaxError" in ln["text"] for ln in lines):
                    break
                time.sleep(0.3)
            err = [ln for ln in lines if "SyntaxError" in ln["text"]]
            self.assertTrue(err, [ln["text"] for ln in lines[-8:]])
            self.assertEqual(err[-1]["kind"], "error", "red in the Immediate window")
            self.assertFalse([e for e in ui.errors(errors_only=True) if "SyntaxError" in e.get("title", "") + e.get("text", "")],
                             "a typo in the Immediate window isn't an entry in the Error Log")
        finally:
            ui.pane("immediate", False)

    def test_formula_typed_after_double_clicking_an_empty_cell(self):
        # the first characters arrive before the edit box has focus; the formula hints read the text before the caret
        for i, typed in enumerate(["=", "=SUM(1", "=OFFSET(A1,2,2,3,3)"]):
            self.keys(f"Q:C{i + 2}|w:3|t:{typed}|w:3|k:Escape|w:2")
            self.assertIsNone(self.proc.poll(), f"YCGrid ended typing {typed!r} into a new edit")
        self.keys("Q:C9|w:3|t:=SUM(1,2|k:Enter|w:3")
        self.assertEqual(self.ws.Range("C9").Value, 3)

    def test_circular_reference_and_the_undo_list(self):
        from yc import excel, ui

        ws = self.ws
        self.assertIsNone(ws.CircularReference)
        ws.Range("A1").Formula = "=B1+1"
        ws.Range("B1").Formula = "=A1+1"
        self.assertIn(ws.CircularReference.Address(), ("$A$1", "$B$1"))
        ws.Range("D5").Value = 7   # an edit elsewhere: the cycle is still there (the status bar keeps naming it)
        self.assertIsNotNone(ws.CircularReference)
        ws.Range("B1").Value = 5
        self.assertIsNone(ws.CircularReference)
        self.assertIsNone(excel.Worksheets(1).CircularReference)
        depth = ui.state()["undo"]["depth"]
        self.keys("g:C3|t:12|k:Enter|w:2")
        st = ui.state()
        self.assertEqual(st["undo"]["depth"], depth + 1)
        self.assertTrue(st["undo"]["label"])
        self.keys("k:c-Z|w:3")
        self.assertEqual(ui.state()["redo"]["depth"], 1)

    def test_quick_undo_and_redo_while_the_engine_works(self):
        # the frame's version lags behind the engine while a big fill runs: Undo / Redo were grayed out by it and
        # Ctrl+Z / Ctrl+Y pressed right away were lost
        ws = self.ws
        ws.Range("A1:A20").Value = [[i] for i in range(20)]
        self.keys("g:E1|t:1|k:Enter|w:3")   # a typed cell: nothing left to redo
        self.keys("g:B1:C300000|w:2|t:=A1*2|k:c-Enter|k:c-Z|w:1|k:c-Y|w:1|k:c-Z|w:1|k:c-Y|w:2")
        self.assertEqual(ws.Range("C300000").Formula, "=B300000*2")
        self.keys("k:c-Z|w:1|k:c-Y|w:1|k:c-Z|w:2")
        self.assertEqual(ws.Range("C300000").Formula, "")
        ws.Range("B1:C300000").ClearContents()

    def test_insert_pie_without_labels(self):
        from yc import ui

        self.ws.Range("A1:B5").Value = [["x", "y"], [1, 2], [2, 3], [3, 5], [4, 4]]
        self.keys("g:A1:B5|w:2")
        self.assertTrue(ui.command("InsertChartPie"))
        time.sleep(0.8)   # a few frames: the pie is drawn
        self.assertIsNone(self.proc.poll(), "YCGrid ended drawing a pie chart")
        ui.state()

    def test_dates_in_the_first_column_are_categories(self):
        from yc import excel

        self.ohlc()
        co = self.ws.ChartObjects.Add(300, 10, 360, 216)
        co.Chart.SetSourceData(self.ws.Range("A1:E11"))
        co.Chart.ChartType = excel.xlStockOHLC
        self.assertEqual(co.Chart.SeriesCollection.Count, 4)   # Open, High, Low, Close; the dates label the axis
        self.assertEqual(co.Chart.SeriesCollection(1).Name, "Open")

    def _chart_rect(self):
        from yc import ui

        objs = [i for i in ui.items("Workbook") if "##obj" in i["id"]]
        self.assertTrue(objs, "the chart is drawn on the sheet")
        return objs[-1]["rect"]

    def _cell_center(self, addr):
        from yc import ui

        r = ui.cell_rect(addr)
        return (r[0] + r[2]) // 2, (r[1] + r[3]) // 2

    def test_an_empty_chart_gets_its_data_by_dragging_over_the_cells(self):
        # Insert > Scatter with nothing selected, Select Data in the chart, drag over the cells, OK (Excel's way)
        from yc import ui

        self.ws.Range("A1:B6").Value = [["X", "Sales"]] + [[i, i * 3] for i in range(1, 6)]
        self.keys("g:H2|w:2")
        self.assertTrue(ui.command("InsertChartScatter"))
        self.keys("w:4")
        co = self.ws.ChartObjects(self.ws.ChartObjects().Count)
        self.assertEqual(co.Chart.SeriesCollection().Count, 0, "an empty chart")
        btn = [i for i in ui.items() if i["label"].startswith("Select Data")]
        self.assertTrue(btn, "the empty chart shows Select Data")
        x, y = (btn[0]["rect"][0] + btn[0]["rect"][2]) // 2, (btn[0]["rect"][1] + btn[0]["rect"][3]) // 2
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        self.assertTrue(any("yc_select_data" in w["name"] for w in ui.windows()), "Select Data Source opens")
        x0, y0 = self._cell_center("A1")
        x1, y1 = self._cell_center("B6")
        self.keys(f"m:{x0},{y0}|w:4|d:{x0},{y0},{x1},{y1}|w:8")
        self.assertEqual(co.Chart.SeriesCollection().Count, 0, "nothing changes before OK")
        ok = [i for i in ui.items() if i["label"] == "OK" and "Select Data" in i.get("window", "")]
        self.assertTrue(ok)
        x, y = (ok[0]["rect"][0] + ok[0]["rect"][2]) // 2, (ok[0]["rect"][1] + ok[0]["rect"][3]) // 2
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")
        sc = co.Chart.SeriesCollection(1)
        self.assertEqual((sc.Name, series_ref(sc, 1), series_ref(sc, 2)), ("Sales", "$A$2:$A$6", "$B$2:$B$6"))
        self.assertTrue(co.Chart.HasTitle, "one series: its name is the title, as in Excel")
        co.Delete()

    def test_dragging_the_outlined_data_corner_changes_the_chart(self):
        from yc import ui

        self.ws.Range("A1:B9").Value = [["M", "Sales"]] + [[f"m{i}", i * 2] for i in range(1, 9)]
        self.keys("g:A1|w:2")
        self.assertTrue(ui.command("InsertChartColumn"))
        self.keys("w:4")
        co = self.ws.ChartObjects(self.ws.ChartObjects().Count)
        r = self._chart_rect()
        cx, cy = int(r[2] - 6), int(r[1] + 6)
        self.keys(f"m:{cx},{cy}|w:3|c:{cx},{cy}|w:5")         # select the chart: its data is outlined
        b9 = ui.cell_rect("B9")
        tx, ty = self._cell_center("B5")
        self.keys(f"m:{int(b9[2])},{int(b9[3])}|w:6|d:{int(b9[2])},{int(b9[3])},{tx},{ty}|w:8")
        s = co.Chart.SeriesCollection(1)
        self.assertEqual((series_ref(s, 2), series_ref(s, 1)), ("$B$2:$B$5", "$A$2:$A$5"))
        self.keys("m:2,2|w:2")                                 # park the mouse: later tests open menus by keyboard
        co.Delete()

    def test_ctrl_selected_columns_make_one_scatter_series(self):
        from yc import ui

        self.ws.Range("A1:C5").Value = [["X", "skip", "Y"], [1, "a", 2], [2, "b", 4], [3, "c", 5], [4, "d", 9]]
        self.keys("g:A1:A5,C1:C5|w:2")                       # X, then Ctrl+select Y (not next to it)
        self.assertTrue(ui.command("InsertChartScatter"))
        self.keys("w:4")
        c = self.ws.ChartObjects(self.ws.ChartObjects().Count).Chart
        self.assertEqual(c.SeriesCollection().Count, 1)
        s = c.SeriesCollection(1)
        self.assertEqual((s.Name, series_ref(s, 1), series_ref(s, 2)), ("Y", "$A$2:$A$5", "$C$2:$C$5"))
        self.assertEqual((s.XValues, s.Values), ((1, 2, 3, 4), (2, 4, 5, 9)))  # the values, as Excel gives them
        self.ws.ChartObjects(self.ws.ChartObjects().Count).Delete()

    def test_double_click_the_chart_title_to_type_a_new_one(self):
        from yc import ui

        self.ws.Range("A1:B4").Value = [["M", "Sales"], ["Jan", 1], ["Feb", 3], ["Mar", 2]]
        self.keys("g:A1|w:2")
        self.assertTrue(ui.command("InsertChartColumn"))
        self.keys("w:4")
        co = self.ws.ChartObjects(self.ws.ChartObjects().Count)
        self.assertEqual(co.Chart.ChartTitle.Text, "Sales")
        r = self._chart_rect()
        x, y = int((r[0] + r[2]) / 2), int(r[1] + 18)
        self.keys(f"m:{x},{y}|w:3|D:{x},{y}|w:6")
        self.keys("k:c-A|t:Monthly Sales|k:Enter|w:5")
        self.assertEqual(co.Chart.ChartTitle.Text, "Monthly Sales")
        co.Delete()

    def test_series_formula_reads_and_writes_the_ranges(self):
        self.ws.Range("A1:C4").Value = [["M", "S", "C"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 5, 6]]
        co = self.ws.ChartObjects.Add(300, 10, 360, 216)
        co.Chart.SetSourceData(self.ws.Range("A1:C4"))
        sh = self.ws.Name
        s1 = co.Chart.SeriesCollection(1)
        self.assertEqual(s1.Formula, f"=SERIES({sh}!$B$1,{sh}!$A$2:$A$4,{sh}!$B$2:$B$4,1)")
        s1.Formula = f'=SERIES("Total",{sh}!$A$2:$A$3,{sh}!$C$2:$C$3,1)'
        s1 = co.Chart.SeriesCollection(1)
        self.assertEqual((s1.Name, s1.Values, s1.XValues), ("Total", (2, 4), ("Jan", "Feb")))
        with self.assertRaises(ValueError):
            s1.Formula = "=SUM(A1)"
        co.Delete()

    def test_chart_filters_hide_series_and_categories(self):
        self.ws.Range("A1:C4").Value = [["M", "S", "C"], ["Jan", 1, 2], ["Feb", 3, 4], ["Mar", 5, 6]]
        co = self.ws.ChartObjects.Add(300, 10, 360, 216)
        co.Chart.SetSourceData(self.ws.Range("A1:C4"))
        s2 = co.Chart.SeriesCollection(2)
        self.assertFalse(s2.IsFiltered)
        s2.IsFiltered = True
        self.assertTrue(co.Chart.SeriesCollection(2).IsFiltered)
        cats = co.Chart.ChartGroups(1).FullCategoryCollection()
        self.assertEqual([c.Name for c in cats], ["Jan", "Feb", "Mar"])
        cats("Feb").IsFiltered = True
        self.assertEqual([c.Name for c in co.Chart.ChartGroups(1).CategoryCollection()], ["Jan", "Mar"])
        co.Delete()

    def test_tab_across_a_row_then_enter_starts_the_next_row(self):
        from yc import ui

        self.keys("g:B2|t:Name|k:Tab|t:Qty|k:Tab|t:Price|k:Enter|w:2")
        self.assertEqual(ui.state()["active_cell"], "B3")   # Excel: back to the column the Tabs started from
        self.keys("k:Tab|k:Tab|k:Enter|w:2")                 # Tab without typing too
        self.assertEqual(ui.state()["active_cell"], "B4")
        self.keys("g:C5|k:Enter|w:2")                        # a plain Enter goes down
        self.assertEqual(ui.state()["active_cell"], "C6")

    def test_autosum_adds_up_a_column_of_formulas(self):
        r = self.ws.Range
        r("A1:B3").Value = [[1, "=A1*2"], [2, "=A2*2"], [3, "=A3*2"]]
        self.keys("g:B4|k:a-Equal|w:3|k:Enter|w:3")
        self.assertEqual(r("B4").Formula, "=SUM(B1:B3)")
        self.assertEqual(r("B4").Value, 12)
        r("B5:B6").Value = [[5], [6]]                        # an earlier total ends the run above
        self.keys("g:B7|k:a-Equal|w:3|k:Enter|w:3")
        self.assertEqual(r("B7").Formula, "=SUM(B5:B6)")

    def test_enter_pastes_while_copied_cells_are_outlined(self):
        from yc import ui

        r = self.ws.Range
        r("A1").Value = 7
        self.keys("g:A1|k:c-C|w:2")
        self.assertTrue(ui.state()["copy_mode"])
        self.keys("k:Escape|w:2")
        self.assertFalse(ui.state()["copy_mode"])
        self.keys("g:A1|k:c-X|g:C3|k:Enter|w:3")
        self.assertEqual((r("A1").Value, r("C3").Value), (None, 7))
        self.assertFalse(ui.state()["copy_mode"])
        self.keys("g:C3|k:c-C|g:D3|k:Enter|w:3")
        self.assertEqual(r("D3").Value, 7)
        self.keys("k:Enter|w:2")                             # copy mode ended: Enter moves again
        self.assertEqual(ui.state()["active_cell"], "D4")

    def test_paste_special_letters(self):
        from yc import ui

        r = self.ws.Range
        r("A1").Formula = "=1+2"
        self.keys("g:A1|k:c-C|g:A3|k:c-a-V|w:8")
        self.assertNotEqual(ui.state()["dialog"], 0)
        self.keys("k:V|w:2|k:Enter|w:6")                     # V: Values
        self.assertEqual((r("A3").Value, r("A3").HasFormula), (3, False))
        self.keys("k:Escape|g:A1|k:c-C|g:A5|k:c-a-V|w:8|k:Enter|w:6")   # opens on All again
        self.assertEqual(r("A5").Formula, "=1+2")
        self.keys("k:Escape|w:2")

    def test_a_python_save_keeps_the_screen_live(self):
        import threading

        from yc import excel, ui

        ws = self.ws
        ws.Range("A1:H300000").Formula = "=ROW()*1.37+COLUMN()/7"     # about 8 MB: the write takes most of a second
        ws.Range("J1:J300000").Formula = '="t"&MOD(ROW()*7919,100003)'
        tmp = tempfile.mkdtemp(prefix="ycsave_")
        pings, done = [], threading.Event()
        from yc import _conn

        main = _conn._current
        c = yc.connect(target=self.proc.pid)                 # a second connection, like a monitor's
        yc.set_current(main)                                 # the test's own requests stay on the first

        def ping():
            while not done.is_set():
                t0 = time.perf_counter()
                c.request("ui.ping", {}, timeout=30)
                pings.append(time.perf_counter() - t0)
                time.sleep(0.02)
            c.close()

        t = threading.Thread(target=ping)
        book = os.path.join(tmp, "Big.ycg")
        try:
            for _ in range(2):                               # the first save also builds the caches it uses
                t0 = time.perf_counter()
                excel.ThisWorkbook.SaveAs(book)
                took = time.perf_counter() - t0
                if not t.is_alive():
                    t.start()
            self.assertTrue(os.path.getsize(book) > 1_000_000)
            # the file is written off the UI thread: the screen answers within a frame or two while it saves
            self.assertLess(max(pings), max(0.25, took / 3), (took, max(pings)))

            # Ctrl+S by hand: written on a worker too (PLAN 15.1), then the workbook is saved
            ws.Range("L1").Value = 42
            self.assertFalse(excel.ThisWorkbook.Saved)
            before = os.path.getmtime(book)
            pings.clear()
            t0 = time.perf_counter()
            self.keys("g:L2|k:c-S")
            while time.perf_counter() - t0 < 30 and (os.path.getmtime(book) == before or "Saved " not in ui.state().get("status", "")):
                time.sleep(0.05)
            took = time.perf_counter() - t0
            self.assertNotEqual(os.path.getmtime(book), before)
            self.assertTrue(excel.ThisWorkbook.Saved)
            self.assertLess(max(pings), max(0.25, took / 3), (took, max(pings)))

            for name, save in (("Save", lambda: excel.ThisWorkbook.Save()),
                               ("SaveCopyAs", lambda: excel.ThisWorkbook.SaveCopyAs(os.path.join(tmp, "Copy.ycg")))):
                ws.Range("L1").Value = 43
                pings.clear()
                t0 = time.perf_counter()
                save()
                took = time.perf_counter() - t0
                self.assertLess(max(pings), max(0.25, took / 3), (name, took, max(pings)))
            self.assertTrue(os.path.getsize(os.path.join(tmp, "Copy.ycg")) > 1_000_000)
            done.set()
            t.join(30)
        finally:
            done.set()
            ws.Delete()
            self.setUp()
            shutil.rmtree(tmp, ignore_errors=True)

    def test_wrapped_text_and_line_breaks_grow_the_row(self):
        from yc import ui

        ws, r = self.ws, self.ws.Range
        r("B2").Value = "a long text that should wrap within the cell nicely"
        self.keys("g:B2|w:2")
        ui.command("WrapText")
        self.keys("w:3")
        self.assertTrue(r("B2").WrapText)
        self.assertGreater(ws.Rows(2).RowHeight, 30)
        self.keys("k:c-Z|w:3")                               # one undo: the wrap and the height
        self.assertEqual((r("B2").WrapText, ws.Rows(2).RowHeight), (False, 15))
        self.keys("g:B4|t:first|k:a-Enter|t:second|k:Enter|w:4")
        self.assertEqual(r("B4").Value, "first\nsecond")     # the break where it was typed, even typed fast
        self.assertTrue(r("B4").WrapText)                    # Alt+Enter turns Wrap Text on, as in Excel
        self.assertGreaterEqual(ws.Rows(4).RowHeight, 22)    # two lines of 11 pt text (one line: 15)
        self.keys("k:c-Z|w:3")
        self.assertEqual(ws.Rows(4).RowHeight, 15)
        ws.Rows(6).RowHeight = 45                            # a height set by hand stays
        r("B6").Value = "a long text that should wrap within the cell nicely and more"
        self.keys("g:B6|w:2")
        ui.command("WrapText")
        self.keys("w:3")
        self.assertAlmostEqual(ws.Rows(6).RowHeight, 45, delta=0.8)

    def test_undo_shows_the_cells_it_changes(self):
        from yc import excel, ui

        other = excel.Worksheets.Add()
        other.Activate()
        self.keys("w:3|g:C5|t:hello|k:Enter|w:3")
        self.ws.Activate()
        self.keys("w:3|g:A1|w:2|k:c-Z|w:4")
        st = ui.state()
        self.assertEqual(other.Range("C5").Value, None)
        self.assertEqual((st["active_sheet"], st["active_cell"]), (other.Name, "C5"))
        self.ws.Activate()
        self.keys("w:3|k:c-Y|w:4")
        self.assertEqual((ui.state()["active_sheet"], other.Range("C5").Value), (other.Name, "hello"))
        self.ws.Activate()
        self.keys("w:3")
        other.Delete()

    def test_a_new_chart_lands_on_the_screen(self):
        from yc import excel, ui

        self.ws.Range("AD1:AE6").Value = [["x", "y"]] + [[i, i * i] for i in range(1, 6)]
        self.keys("g:AD1:AE6|w:3")                           # data at the right edge of what the window shows
        self.assertTrue(ui.command("InsertChartLine"))
        self.keys("w:4")
        co = self.ws.ChartObjects(self.ws.ChartObjects().Count)
        vis = excel.ActiveWindow.VisibleRange
        x0, x1 = vis.Left, vis.Left + vis.Width
        self.assertGreaterEqual(co.Left, x0 - 1)
        self.assertLessEqual(co.Left + co.Width, x1 + 1)
        co.Delete()

    def test_every_ribbon_button_has_an_icon(self):
        from yc import ui

        cmds = ui.commands()
        self.assertGreater(sum(1 for c in cmds if c["ribbon"]), 100, "the ribbon's buttons are listed")
        bare = sorted(c["id"] for c in cmds if c["ribbon"] and not c["icon"])
        self.assertEqual(bare, [], "ribbon buttons drawn without an icon (blank)")

    def test_a_protected_sheet_grays_what_it_does_not_allow(self):
        from yc import ui

        def on(cid):
            return {c["id"]: c["enabled"] for c in ui.commands()}[cid]

        self.ws.Protect()
        try:
            self.keys("w:3")
            self.assertEqual((on("InsertRows"), on("FormatCellsDialog"), on("Bold"), on("Copy")), (False, False, False, True))
            self.keys("g:C5|k:c-1|w:6")                      # Ctrl+1 does nothing (no dialog, no refusal)
            self.assertEqual(ui.state()["dialog"], 0)
            self.ws.Unprotect()
            self.ws.Protect(AllowFormattingCells=True, AllowInsertingRows=True)
            self.keys("w:3")
            self.assertEqual((on("Bold"), on("InsertRows"), on("DeleteRows")), (True, True, False))
        finally:
            self.ws.Unprotect()
        self.keys("w:3")
        self.assertTrue(on("InsertRows") and on("Bold"))

    def test_a_workbook_without_python_counts_as_opened(self):
        # nothing to wait for: the test skeleton's "while not workbook_open_done" loop must end
        self.assertTrue(self.conn.request("python.status", {}).get("workbook_open_done"))

    def test_enter_and_escape_in_dialogs(self):
        from yc import ui

        self.ohlc()
        self.keys("g:A1|w:2|k:c-T|w:8")
        self.assertTrue(ui.state()["dialog"])
        self.keys("k:Enter|w:8")   # Enter is OK, as in Excel
        self.assertEqual(ui.state()["dialog"], 0)
        self.assertEqual(self.ws.ListObjects.Count, 1)
        for cmd in ("CfGreater", "DataValidation"):
            self.keys("g:B2:B11|w:2")
            ui.command(cmd)
            time.sleep(0.4)
            self.assertTrue(ui.state()["dialog"], cmd)
            self.keys("k:Escape|w:8")
            self.assertEqual(ui.state()["dialog"], 0, f"Escape didn't close {cmd}")

    def _click(self, item):
        r = item["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:5")

    def test_a_dialogs_range_box_takes_cells_picked_on_the_sheet(self):
        # Excel's RefEdit: the box's button shrinks the dialog, the user selects cells, Enter brings the dialog back
        from yc import ui

        self.ws.Range("A1:B2").Value = [[1, "=A1*2"], [3, 4]]
        self.keys("m:2,2|g:D5|w:2")
        ui.command("InsertTable")
        self.keys("w:6")
        dlg = ui.state()["dialog"]
        self.assertTrue(dlg)
        pick = [i for i in ui.items() if i["label"] == "\U000f0244"]
        self.assertTrue(pick, "the range box has its button")
        self._click(pick[0])
        self.assertTrue(any("yc_range_pick" in w["name"] for w in ui.windows()), "the dialog shrinks to the box")
        x0, y0 = self._cell_center("A1")
        x1, y1 = self._cell_center("B2")
        self.keys(f"m:{x0},{y0}|w:3|d:{x0},{y0},{x1},{y1}|w:6|k:Enter|w:8")
        self.assertFalse(any("yc_range_pick" in w["name"] for w in ui.windows() if w.get("visible", True)))
        self.assertEqual(ui.state()["dialog"], dlg, "the same dialog is back")
        self.keys("k:Enter|w:8")                              # OK: the table covers the cells picked
        self.assertEqual(self.ws.ListObjects.Count, 1)
        self.assertEqual(self.ws.ListObjects(1).Range.Address.replace("$", ""), "A1:B2")

    def test_file_dialogs_in_a_hidden_window(self):
        from yc import ui

        ui.clear_errors()
        t0 = time.time()
        for cmd in ("FileSaveAs", "InsertPicture"):
            ui.command(cmd)   # nobody can see a hidden window: cancelled at once, not left on the desktop
        self.assertLess(time.time() - t0, 5)
        titles = [e["title"] for e in ui.errors()]
        self.assertIn("Save As", titles)
        self.assertIn("Insert Picture", titles)

    def test_hidden_instance_leaves_the_users_state_alone(self):
        from yc import excel, ui

        user = os.path.join(os.environ.get("LOCALAPPDATA", ""), "ycgrid")
        watched = [os.path.join(user, "recent.json"), os.path.join(user, "default_layout.json")]
        mtime = lambda f: os.path.getmtime(f) if os.path.exists(f) else None  # noqa: E731
        before = [mtime(f) for f in watched]
        tmp = tempfile.mkdtemp(prefix="ycstate_")
        try:
            # a container and a saved layout (the UI sweep runs every command) stay in the hidden instance: they used
            # to open as "Container 1..6" in the user's next YCGrid
            before_ids = {c.Name for c in excel.Application.Containers}
            ui.command("NewContainer")
            ui.command("LayoutSave")
            time.sleep(0.3)
            for c in list(excel.Application.Containers):   # a floating container over the grid would take later clicks
                if c.Name not in before_ids:
                    c.Delete()
            excel.ThisWorkbook.SaveAs(os.path.join(tmp, "State.ycg"))
            time.sleep(0.3)
            self.assertEqual(before, [mtime(f) for f in watched])   # nor do a test's files go into the Recent list
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_escape_closes_a_dialog_opened_while_searching(self):
        from yc import ui

        self.keys("k:a-Q|w:5|t:paste|w:3")   # the command search has the keyboard
        ui.command("PasteSpecial")
        time.sleep(0.4)
        self.assertTrue(ui.state()["dialog"])
        self.keys("k:Escape|w:8")
        self.assertEqual(ui.state()["dialog"], 0)

    def test_double_click_on_the_fill_handle_fills_down_beside_the_data(self):
        from yc import ui

        self.ws.Range("A1:D1").Value = [["Date", "Close", "Volume", "Sector"]]
        self.ws.Range("A2:D41").Value = [[46000 + i, 100 + i, 1000 + i, "Tech"] for i in range(40)]
        self.ws.Range("E30").Value = "note"          # the fill stops before a cell that holds something
        self.keys("A:E3|w:2|t:=B3/B2-1|k:Enter|w:3|A:E3|w:3")
        r = ui.cell_rect("E3")
        x, y = int(r[2] - 1), int(r[3] - 1)
        self.keys(f"m:{x},{y}|w:4|D:{x},{y}|w:8")
        self.assertEqual(self.ws.Range("E29").Formula, "=B29/B28-1")
        self.assertEqual(self.ws.Range("E30").Value, "note")
        self.assertNotIn("edit", ui.state(), "no cell editor after the double-click")
        self.assertEqual(ui.state()["selection"], "E3:E29")
        self.keys("k:c-Z|w:4")
        self.assertIsNone(self.ws.Range("E4").Value, "one undo takes the fill back")

    def test_ping_answers_while_the_engine_works(self):
        # ui.state waits for the engine's queue (it shows this connection's writes); ui.ping is the UI thread alone
        from yc import ui

        self.assertTrue(ui.ping())
        self.keys("g:B1:B1048576|w:2|t:=ROW()*2|k:c-Enter|w:2")   # a million formulas on the engine thread
        t0 = time.time()
        self.assertTrue(ui.ping())
        self.assertLess(time.time() - t0, 1.0)
        end = time.time() + 60
        while time.time() < end and self.ws.Range("B1048576").Value != 2097152:
            time.sleep(0.2)
        self.assertEqual(self.ws.Range("B1048576").Value, 2097152)

    def test_insert_pivottable_from_a_large_range_keeps_the_screen_live(self):
        # the source is read on the engine thread: 100,000 rows took the screen for a second
        from yc import ui

        n = 100_000
        self.ws.Range("A1:C1").Value = [["Symbol", "Side", "Qty"]]
        self.ws.Range(f"A2:C{n + 1}").Value = [["TXF" if i % 3 else "MXF", "B" if i % 2 else "S", i % 50] for i in range(n)]
        sheets = len(ui.state()["sheets"])
        self.keys(f"g:A1:C{n + 1}|w:2")
        ui.command("InsertPivotTable")
        self.keys("w:4")
        self.conn.request("perf.stats", {"last": 1, "reset_latency": True})
        self.keys("k:Enter|w:10")
        end = time.time() + 15
        while time.time() < end and len(ui.state()["sheets"]) == sheets:
            time.sleep(0.1)
        self.keys("w:6")
        st = ui.state()
        self.assertEqual(len(st["sheets"]), sheets + 1)
        self.assertEqual(st["active_cell"], "A3", "the new report's corner, on the new sheet")
        self.assertEqual(st["dialog"], 0)
        self.assertLess(self.conn.request("perf.stats", {"last": 60})["frame_cpu_max_ms"], 400)

    def test_keys_typed_on_a_clicked_figure_stay_out_of_the_cells(self):
        # a trader clicks a chart over the sheet and types: the keys went into the active cell behind the chart
        from yc import plot, ui

        self.keys("A:B2|w:2")
        f = plot.figure("Keys on a chart", size=(500, 300), pos=(100, 150))
        f[0].line([0, 1, 2], [1, 3, 2])
        try:
            self.keys("w:6|t:4|k:Enter|w:3")              # a chart a script pops up doesn't take the user's keys
            self.assertEqual(self.ws.Range("B2").Value, 4)
            self.keys("A:C2|w:3")                         # the active cell behind the chart
            w = next(x for x in ui.windows() if x["label"].startswith("Keys on a chart"))
            x, y = int((w["rect"][0] + w["rect"][2]) / 2), int((w["rect"][1] + w["rect"][3]) / 2)
            self.keys(f"c:{x},{y}|w:4|t:hello|k:Enter|w:4")
            self.assertIsNone(self.ws.Range("C2").Value)
            self.assertNotIn("edit", ui.state())
            r = ui.cell_rect("L4")                     # a cell beside the chart: a click there gives the keys back
            self.assertGreater(r[0], w["rect"][2])
            self.keys("A:L4|w:3|t:7|k:Enter|w:3")
            self.assertEqual(self.ws.Range("L4").Value, 7)
        finally:
            f.close()

    def test_copy_and_paste_while_the_engine_works_keep_the_screen_live(self):
        # Ctrl+C asked the engine for the copied cells and the screen waited behind the work before it: a frame of
        # 2.4 s in trader_session, Ctrl+C a frame after a million formulas were filled
        from yc import ui

        self.ws.Range("A1:A20").Value = [[i] for i in range(20)]
        self.keys("g:B1:C500000|w:2|t:=A1*2|k:c-Enter|w:4")
        end = time.time() + 60
        while time.time() < end and self.ws.Range("C500000").Formula != "=B500000*2":
            time.sleep(0.2)
        f0 = self.conn.request("perf.stats", {"last": 1})["frames"]
        # filled again, copied a frame later and pasted at once: the copy takes the new formulas, the paste the copy
        self.keys("g:B1:C500000|w:2|t:=A1*3|k:c-Enter|w:1|k:c-C|w:1|g:H1|k:c-V|w:4")
        frames = self.conn.request("perf.stats", {"last": 1})["frames"] - f0
        self.assertLess(self.conn.request("perf.stats", {"last": max(1, frames)})["frame_cpu_max_ms"], 400)
        self.assertEqual(self.ws.Range("H1").Formula, "=G1*3")
        self.assertEqual(self.ws.Range("I500000").Formula, "=H500000*3")
        self.assertEqual(self.conn.request("app.cut_copy_mode", {}), 1)  # still outlined after the paste, as in Excel
        self.keys("k:Escape|w:2")

    def test_keys_that_ask_the_engine_keep_the_screen_live_while_it_works(self):
        # each of these asked the engine in the frame and waited behind a million formulas filled a frame before
        # (1.4 s to 1.7 s): an entry under a cell with text (the Flash Fill preview), an array formula, a name in the
        # Name Box, F9 in the editor, Format Painter, Trace Precedents. Put off, they act once the engine caught up.
        from yc import excel, ui

        self.ws.Range("A1:A20").Value = [[i] for i in range(20)]
        self.ws.Range("K1").Value = 5
        self.ws.Range("P1").Value = "x"
        self.ws.Range("P1").Font.Bold = True
        self.ws.Range("Q1").Value = "AAPL US"
        excel.ActiveWorkbook.Names.Add("BusyRate", f"='{self.ws.Name}'!$K$1")
        self.keys("g:B1:C500000|w:2|t:=A1*2|k:c-Enter|w:4")
        end = time.time() + 60
        while time.time() < end and self.ws.Range("C500000").Formula != "=B500000*2":
            time.sleep(0.2)
        n = [3]

        def busy(script):
            n[0] += 1
            f0 = self.conn.request("perf.stats", {"last": 1})["frames"]
            self.keys(f"g:B1:C500000|w:2|t:=A1*{n[0]}|k:c-Enter|w:1|" + script)
            frames = self.conn.request("perf.stats", {"last": 1})["frames"] - f0
            return self.conn.request("perf.stats", {"last": max(1, frames)})["frame_cpu_max_ms"]

        self.assertLess(busy("g:Q2|t:7|k:Enter|w:4"), 400, "typing under a cell with text")
        self.assertEqual(self.ws.Range("Q2").Value, 7)
        self.assertLess(busy("g:R3:R5|t:=K1*2|k:c-s-Enter|w:4"), 400, "an array formula")
        self.assertTrue(self.ws.Range("R3").HasArray)
        self.assertEqual(self.ws.Range("R5").Value, 10)
        self.assertLess(busy("g:BusyRate|w:4"), 400, "a name in the Name Box")
        self.assertEqual(excel.Selection.Address, "$K$1")
        self.assertLess(busy("g:S2|t:=K1+1|k:F9|w:3"), 400, "F9 in the editor")
        self.assertEqual((ui.state().get("edit") or {}).get("text"), "=6")
        self.keys("k:Escape|w:2")
        self.assertLess(busy("g:P1|x:FormatPainter|w:4"), 400, "Format Painter")
        x, y = [int(v) for v in ui.cell_rect("S8")[:2]]
        self.keys(f"m:{x + 4},{y + 4}|w:2|c:{x + 4},{y + 4}|w:4")
        self.assertTrue(self.ws.Range("S8").Font.Bold)
        self.assertLess(busy("g:C5|x:TracePrecedents|w:4"), 400, "Trace Precedents")
        self.keys("x:RemoveArrows|w:2|m:2,2")

    def test_ctrl_enter_fills_a_whole_column_and_the_screen_stays_live(self):
        # Excel fills all of the selection; YCGrid stopped at row 100,000 and held the screen while it filled
        from yc import ui

        self.ws.Range("A1:A3").Value = [[1], [2], [3]]
        self.keys("A:C1|w:2|k:c-Space|w:3|t:=A1*2|w:2")
        self.conn.request("perf.stats", {"last": 1})
        self.keys("k:c-Enter|w:4")
        end = time.time() + 60
        while time.time() < end and self.ws.Range("C1048576").Formula != "=A1048576*2":
            time.sleep(0.2)
        self.assertEqual(self.ws.Range("C1048576").Formula, "=A1048576*2")
        self.assertEqual(self.ws.Range("C3").Value, 6)
        self.assertLess(self.conn.request("perf.stats", {"last": 30})["frame_cpu_max_ms"], 400)
        self.keys("A:E1|w:2|k:c-Z|w:6")                        # one step takes all of it back
        end = time.time() + 30
        while time.time() < end and self.ws.Range("C1048576").Formula:
            time.sleep(0.2)
        self.assertEqual(self.ws.Range("C1048576").Formula, "")
        self.assertIsNone(self.ws.Range("C1").Value)
        self.assertNotIn("edit", ui.state())
        # the whole sheet (17 billion cells) is refused with a reason, not filled for ever
        n = len(ui.errors())
        self.keys("A:E2|w:2|k:c-A|k:c-A|w:2|t:=1|w:2|k:c-Enter|w:6|k:Escape|w:3")
        self.assertIsNone(self.proc.poll())
        self.assertTrue(any("too large to fill" in str(e.get("text")) for e in ui.errors()[n:]), ui.errors()[n:])
        self.assertIsNone(self.ws.Range("E5").Value)

    def test_quick_clicks_on_a_click_action_are_each_a_click(self):
        # click-to-trade: a scalper clicks a price several times fast; the second click of a pair is a double-click,
        # which must not open the cell editor (it took every click after it)
        from yc import ui
        from yc.mcp import tools

        name = self.ws.Name
        self.ws.Range("B2:B6").Value = [[100], [99], [98], [97], [96]]
        tools._host_ready()  # a workbook without main.py: Python starts when asked
        r = tools.yc_exec_python(
            "from yc import excel, interact\n"
            f"ws = excel.Worksheets({name!r})\n"
            "def h(ctx):\n"
            "    c = ws.Range('D1')\n"
            "    c.Value = (c.Value or 0) + 1\n"
            "interact.on_click(ws.Range('B2:B6'), h)\n", target="host", timeout_ms=60000)
        self.assertTrue(r.get("ok"), r)
        self.keys("|".join(["A:B3|w:1"] * 10) + "|w:6")
        end = time.time() + 10
        while time.time() < end and self.ws.Range("D1").Value != 10:
            time.sleep(0.2)
        self.assertEqual(self.ws.Range("D1").Value, 10)
        self.assertNotIn("edit", ui.state())
        self.assertEqual(self.ws.Range("B3").Value, 99)

    def test_a_drag_from_a_click_action_cell_is_no_click(self):
        """A click action ran on the press: selecting cells by a drag that began on a ladder's size sent an order. It
        runs on a press and a release on the same cell, as a button does."""
        from yc import ui
        from yc.mcp import tools

        name = self.ws.Name
        self.ws.Range("B2:B6").Value = [[100], [99], [98], [97], [96]]
        tools._host_ready()
        r = tools.yc_exec_python(
            "from yc import excel, interact\n"
            f"ws = excel.Worksheets({name!r})\n"
            "ws.Range('E1').Value = 0\n"
            "def h(ctx):\n"
            "    c = ws.Range('E1')\n"
            "    c.Value = (c.Value or 0) + 1\n"
            "interact.on_click(ws.Range('B2:B6'), h)\n", target="host", timeout_ms=60000)
        self.assertTrue(r.get("ok"), r)
        x0, y0, x1, y1 = ui.cell_rect("B3")
        x, y = int((x0 + x1) / 2), int((y0 + y1) / 2)
        y2 = int(sum(ui.cell_rect("B5")[1::2]) / 2)
        self.keys(f"m:{x},{y}|w:3|d:{x},{y},{x},{y2}|w:6")  # a drag over the sizes: a selection, no click
        time.sleep(1.0)
        self.assertEqual(self.ws.Range("E1").Value, 0)
        self.assertEqual(ui.state()["selection"], "B3:B5")
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6")  # a click still is one
        end = time.time() + 10
        while time.time() < end and self.ws.Range("E1").Value != 1:
            time.sleep(0.2)
        self.assertEqual(self.ws.Range("E1").Value, 1)

    def test_regression_by_keyboard(self):
        from yc import excel, ui

        self.ws.Range("A1:B1").Value = [["Index", "Stock"]]
        self.ws.Range("A2:B21").Value = [[i * 0.001, 0.002 + 1.5 * i * 0.001 + (0.0001 if i % 2 else -0.0001)] for i in range(20)]
        self.keys("g:A1|w:2")
        ui.command("DataAnalysis")
        time.sleep(0.4)
        self.keys("k:R|w:2|k:R|w:2|k:R|w:2|k:Enter|w:8")   # a letter jumps through the list: Random, Rank, Regression
        self.keys(f"t:$B$1:$B$21|w:2|k:Tab|w:2|t:$A$1:$A$21|w:2|k:Tab|w:2|k:Space|w:3|k:Enter|w:15")   # Y, X, Labels, OK
        self.assertEqual(ui.state()["dialog"], 0)
        out = excel.Application.ActiveSheet   # the results' new sheet is shown
        self.assertNotEqual(out.Name, self.ws.Name)
        rows = {r[0]: r for r in out.Range("A1:B20").Value if r[0]}
        self.assertAlmostEqual(rows["Index"][1], 1.5, delta=0.05)   # Labels stayed ticked: the header names the row

    def test_find_next_with_enter(self):
        from yc import ui

        self.ws.Range("A1:A5").Value = [["MXF"], ["TXF"], ["TXO"], ["TXF"], ["MXF"]]
        self.keys("g:A1|w:2|k:c-F|w:8|t:TXF|w:2|k:Enter|w:6")
        first = ui.state()["active_cell"]
        self.keys("k:Enter|w:6")   # Enter again: the next one, the box keeps the keyboard
        self.assertEqual((first, ui.state()["active_cell"]), ("A2", "A4"))
        self.keys("k:Escape|w:4")

    def test_advertised_shortcuts_and_typed_dates(self):
        from yc import ui

        self.ws.Range("A1:B3").Value = [["Sym", "PnL"], ["TXF", 1], ["MXF", 2]]
        self.keys("g:A1|w:2|k:c-s-L|w:6")          # Ctrl+Shift+L: AutoFilter, as the ribbon says
        self.assertTrue(self.ws.AutoFilterMode)
        self.keys("g:A1|w:2|k:a-DownArrow|w:6")    # Alt+Down on a header opens its filter menu
        self.assertTrue(ui.state()["ribbon"]["popup_open"])
        self.keys("k:Escape|w:4")
        for keys in ("k:c-K", "k:c-s-F"):  # hyperlink, font: dialogs, not F2's edit mode
            self.keys(f"g:A1|w:2|{keys}|w:8")
            self.assertTrue(ui.state()["dialog"], keys)
            self.keys("k:Escape|w:4")
        # Shift+F2: the note's box beside the cell (Excel), and Escape gives the keyboard back to the sheet
        self.keys("g:A1|w:2|k:s-F2|w:6")
        self.assertTrue([i for i in ui.items("") if i["id"].endswith("##note_inplace")], "the note box")
        self.keys("k:Escape|w:4")
        self.assertFalse([i for i in ui.items("") if i["id"].endswith("##note_inplace")], "Escape closes it")
        w = self.ws.Columns("D").ColumnWidth
        self.keys("g:D1|w:2|t:2026/12/31 13:45|w:2|k:Enter|w:4")   # a typed date widens a standard column
        self.assertGreater(self.ws.Columns("D").ColumnWidth, w)
        self.assertNotIn("#", self.ws.Range("D1").Text)

    def test_manual_calculation(self):
        from yc import excel

        self.keys("g:A1|w:2|t:5|k:Enter|w:2|g:B1|w:2|t:=A1*2|k:Enter|w:4")
        excel.Application.Calculation = excel.xlCalculationManual
        try:
            self.keys("g:C1|w:2|t:=A1*3|k:Enter|w:6")   # entered: computed at once
            self.assertEqual(self.ws.Range("C1").Value, 15)
            self.keys("g:A1|w:2|t:7|k:Enter|w:6")       # dependents wait for F9, reads too
            self.assertEqual(self.ws.Range("B1").Value, 10)
            self.keys("k:F9|w:6")
            self.assertEqual(self.ws.Range("B1").Value, 14)
        finally:
            excel.Application.Calculation = excel.xlCalculationAutomatic

    def test_validation_list_by_keyboard(self):
        v = self.ws.Range("B2").Validation
        v.Add(3, 1, 1, "Buy,Sell")   # xlValidateList
        self.keys("g:B2|w:2|k:a-DownArrow|w:8|k:DownArrow|w:3|k:DownArrow|w:3|k:Enter|w:6")   # Alt+Down opens the list
        self.assertEqual(self.ws.Range("B2").Value, "Sell")
        self.keys("g:C5|w:2|k:a-DownArrow|w:4|k:s-a-RightArrow|w:4")   # no list: Alt+arrows don't move the cell
        from yc import ui
        self.assertEqual(ui.state()["active_cell"], "C5")
        self.keys("k:Escape|w:4")   # Alt+Shift+Right is Group: its question closes (the next test's keys reach the grid)
        self.assertFalse(ui.state()["ribbon"]["popup_open"])
        self.assertEqual(ui.state()["dialog"], 0)

    def test_formula_pointing_and_f4(self):
        from yc import ui

        self.ws.Range("A1:A3").Value = [[10], [20], [30]]
        # right after "=" the arrows point (the full function list is showing), F4 makes the reference absolute
        self.keys("g:C4|w:2|t:=|w:2|k:LeftArrow|w:2|k:LeftArrow|w:2|k:UpArrow|w:3")
        self.assertEqual(ui.state()["edit"]["text"], "=A3")
        self.keys("k:F4|w:2|t:*2|k:Enter|w:4")
        self.assertEqual(self.ws.Range("C4").Formula, "=$A$3*2")
        self.assertNotIn("edit", ui.state())
        # Shift+arrows extend the pointed range; keys typed without waiting still point from the caret
        self.keys("g:C5|t:=SUM(|k:LeftArrow|k:LeftArrow|k:UpArrow|k:UpArrow|k:UpArrow|k:UpArrow"
                  "|k:s-DownArrow|k:s-DownArrow|t:)|k:Enter|w:4")
        self.assertEqual(self.ws.Range("C5").Formula, "=SUM(A1:A3)")
        self.keys("g:C6|w:2|t:=A1+B2|w:2|k:F4|w:2|k:F4|w:2|k:Enter|w:4")   # the reference before the caret
        self.assertEqual(self.ws.Range("C6").Formula, "=A1+B$2")

    def test_excel_keys_for_selection_formats_and_repeat(self):
        from yc import excel, ui

        ws = self.ws
        ws.Range("A1:C4").Value = [["Sym", "Qty", "Px"], ["TXF", 1, 23000], ["MXF", 2, 23010], ["TXO", 3, 300]]
        ws.Range("D2").Formula = "=B2*C2"
        self.keys("g:B3|w:2|k:c-A|w:2")                # Ctrl+A: the data first, then the whole sheet
        self.assertEqual(ui.state()["selection"], "A1:D4")
        self.keys("k:c-A|w:2")
        self.assertEqual(ui.state()["selection"], "A1:XFD1048576")
        self.keys("g:C2|w:2|k:c-s-1|w:3")              # Ctrl+Shift+! ... ^
        self.assertEqual(ws.Range("C2").NumberFormat, "#,##0.00")
        self.keys("g:C3|w:2|k:c-s-6|w:3")
        self.assertEqual(ws.Range("C3").NumberFormat, "0.00E+00")
        self.keys("g:A2|w:2|k:c-B|w:2|g:A4|w:2|k:F4|w:3")   # F4 repeats the last formatting command
        self.assertTrue(ws.Range("A4").Font.Bold)
        self.keys("g:A3|w:2")
        excel.Application.Repeat()                      # the same from Python
        time.sleep(0.2)
        self.assertTrue(ws.Range("A3").Font.Bold)
        self.keys("g:D3|w:2|k:c-Apostrophe|w:2|k:Enter|w:4")   # Ctrl+' : the formula above, unchanged
        self.assertEqual(ws.Range("D3").Formula, "=B2*C2")
        self.keys("g:D2|w:2|k:c-LeftBracket|w:3")      # Ctrl+[ : the cells the formula uses (B2 and C2)
        self.assertEqual(ui.state()["active_cell"], "B2")
        # menu by keyboard: Cut is lit, Down -> Copy (the mouse is parked first: a menu item under it would be lit instead)
        self.keys("m:2,2|w:2|g:A1|w:2|k:s-F10|w:4|k:DownArrow|w:2|k:Enter|w:4")
        self.keys("g:F1|w:2|k:c-V|w:4")
        self.assertEqual(ws.Range("F1").Value, "Sym")
        with self.assertRaises(Exception):              # a misspelt key is refused, not skipped
            self.keys("k:c-Contrl")

    def test_t_layout_keeps_the_strike_in_the_middle(self):
        from yc import excel, ui

        ws = self.ws
        calls = [f"C{i}" for i in range(10)]
        puts = [f"P{i}" for i in range(10)]
        ws.Range("A5:U5").Value = [calls + ["Strike"] + puts]
        ws.Range("A6:U45").Value = [[f"c{r}.{c}" for c in range(10)] + [2000 + 25 * r] + [f"p{r}.{c}" for c in range(10)]
                                    for r in range(40)]
        for i in range(1, 22):
            ws.Columns(i).ColumnWidth = 22   # wider than the window: both sides have to scroll
        self.keys("g:K6|w:2")
        ui.command("FreezeCenterColumns")
        time.sleep(0.3)
        w = excel.ActiveWindow
        self.assertEqual((w.CenterColumns, w.SplitRow), ("K:K", 5))
        self.keys("g:U10|w:4")                         # the outer put column: the right pane scrolls...
        self.assertGreaterEqual(w._v()["visible"][3], 20)
        self.keys("A:K12|w:3")                         # ...and the strike stays clickable where it is drawn
        self.assertEqual(ui.state()["active_cell"], "K12")
        self.keys("g:A10|w:4|A:A12|w:3")               # the outer call column, on the mirrored left pane
        self.assertEqual(ui.state()["active_cell"], "A12")
        self.keys("A:J12|w:3")
        self.assertEqual(ui.state()["active_cell"], "J12")
        saved = excel._rq("layout.capture")
        self.assertIn('"mirror_left":true', saved)     # kept with the workbook's layout
        w.FreezePanes = False
        self.assertIsNone(w.CenterColumns)
        excel._rq("layout.apply", json=saved, main_window=False)
        self.assertEqual(w.CenterColumns, "K:K")
        w.CenterColumns = None

    def test_a_right_click_menu_keeps_a_live_plot_following(self):
        from yc import plot, ui

        fig = plot.figure("Live menu", key="live.menu")
        now = time.time()
        fig[0].line([now - 60 + i for i in range(60)], [i % 7 for i in range(60)])
        fig[0].set(time=True, follow=30)
        fig.context_menu([("Five minutes", lambda e: None)])
        try:
            self.keys("w:6")

            def paused():
                return next(f for f in ui.state()["figures"] if f["key"] == "live.menu")["follow_paused"]

            w = next(x for x in ui.windows() if "live.menu" in x["name"])
            x, y = int((w["rect"][0] + w["rect"][2]) / 2), int((w["rect"][1] + w["rect"][3]) / 2)
            self.keys(f"m:{x},{y}|w:4|r:|w:6|k:Escape|w:4")   # the menu opens and closes: still live
            self.assertFalse(paused())
            fig.input("wheel", x=now - 10, y=3)                # a zoom by the wheel pauses it (the check works)
            self.keys("w:4")
            self.assertTrue(paused())
        finally:
            fig.close()

    def test_a_plot_filled_later_fits_its_data(self):
        from yc import plot, ui

        fig = plot.figure("filled later", key="test.filled_later")
        line = fig[0].line([], [], label="px")
        time.sleep(0.5)   # drawn empty: the first fit has nothing to fit
        line.set_data([1.0, 2.0, 3.0], [23000.0, 23010.0, 22990.0])
        time.sleep(0.6)
        view = next(f for f in ui.state()["figures"] if f.get("key") == "test.filled_later")["plots"][0]["view"]
        self.assertLessEqual(view[2], 22990.0)
        self.assertGreaterEqual(view[3], 23010.0)
        fig.close()


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1", "no built ycgrid.exe")
class DialogCodeTest(unittest.TestCase):
    """Code run for a dialog (host.eval, shown=False) while the Immediate window is closed: a failure must not be silent."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def run_code(self, code, shown):
        self.conn.request("host.eval", {"code": code, "shown": shown}, timeout=10)

    def wait_for(self, check, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            got = check()
            if got:
                return got
            time.sleep(0.3)
        return check()

    def test_a_missing_package_is_offered_and_the_error_logged(self):
        from yc import ui

        before = {e["id"] for e in ui.errors()}
        self.run_code("import yc_test_absent_module; import yc_test_absent_other", shown=False)
        wanted = {"yc-test-absent-module", "yc-test-absent-other"}
        missing = self.wait_for(lambda: wanted <= set(self.conn.request("python.status", {}, timeout=5).get("missing_packages", [])))
        self.assertTrue(missing, "every package the code needs is offered at once (the Missing Python packages bar: one "
                                 "Install), as for the project's own imports")
        logged = self.wait_for(lambda: [e for e in ui.errors() if e["id"] not in before and "yc_test_absent_module" in e["title"] + (e.get("text") or "")])
        self.assertTrue(logged, "the error reaches the Error Log, since the Immediate window may be closed")
        self.assertFalse([e for e in ui.errors(errors_only=True) if e["id"] not in before and "Missing packages" in e["title"]],
                         "the bar's note isn't a second error in the status bar's count")

    def test_errors_of_a_module_installed_since_stop_counting(self):
        from yc import ui

        mod = "yc_test_later_module"
        self.run_code(f"import {mod}", shown=False)
        entry = self.wait_for(lambda: [e for e in ui.errors(errors_only=True) if f"'{mod}'" in e["title"] + (e.get("text") or "")])
        self.assertTrue(entry)
        # "installed": importable from the project folder once the host restarts (as after Install)
        path = os.path.join(self.conn.request("python.status", {}, timeout=5)["app_dir"], mod + ".py")
        with open(path, "w", encoding="utf-8") as f:
            f.write("")
        self.addCleanup(os.remove, path)
        self.conn.request("python.reload", {}, timeout=60)
        eid = entry[-1]["id"]
        settled = self.wait_for(lambda: [e for e in ui.errors() if e["id"] == eid and not e["error"] and "Resolved" in e["text"]])
        self.assertTrue(settled, "the stale error becomes a message marked resolved")
        self.assertFalse([e for e in ui.errors(errors_only=True) if e["id"] == eid], "and no longer counts as an error")

    def test_other_errors_of_dialog_code_reach_the_error_log(self):
        from yc import ui

        before = {e["id"] for e in ui.errors()}
        self.run_code("raise ValueError('dialog code failed')", shown=False)
        logged = self.wait_for(lambda: [e for e in ui.errors() if e["id"] not in before and "dialog code failed" in e["title"] + (e.get("text") or "")])
        self.assertTrue(logged)
        self.run_code("raise ValueError('typed in the Immediate window')", shown=True)
        time.sleep(2)
        self.assertFalse([e for e in ui.errors() if "typed in the Immediate window" in e["title"] + (e.get("text") or "")],
                         "an error typed in the Immediate window shows there only")


class HiddenInstanceEndsTest(unittest.TestCase):
    def test_a_hidden_instance_ends_with_the_script_that_launched_it(self):
        import ctypes

        code = ("import sys; sys.path.insert(0, %r); import yc; p = yc.launch(hidden=True, exe=%r, lang='en'); "
                "print(p.pid, flush=True)") % (os.path.join(ROOT, "python"), EXE)
        out = subprocess.run([sys.executable, "-E", "-s", "-c", code], capture_output=True, text=True, timeout=60)
        pid = int(out.stdout.split()[-1])                   # the script ended without terminate()
        k32 = ctypes.WinDLL("kernel32")
        k32.OpenProcess.restype = ctypes.c_void_p
        h = k32.OpenProcess(0x00100001, False, pid)          # SYNCHRONIZE | PROCESS_TERMINATE
        if not h:
            return                                           # already gone
        try:
            ended = k32.WaitForSingleObject(ctypes.c_void_p(h), 15000) == 0
            if not ended:
                k32.TerminateProcess(ctypes.c_void_p(h), 1)    # the test's own instance: don't leave it behind
            self.assertTrue(ended, "the hidden instance kept running after its script ended")
        finally:
            k32.CloseHandle(ctypes.c_void_p(h))


class CrashDumpTest(unittest.TestCase):
    """A crash leaves a dump with the crashing thread's stack (written by a thread of its own: written from the
    crashing thread, chaos round 5093 left an empty file)."""

    def test_a_crash_writes_a_dump(self):
        import glob

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            folder = conn.request("debug.crash", {}, timeout=10)
            before = set(glob.glob(os.path.join(folder, "ycgrid-crash-*.dmp")))
            proc.wait(60)
            self.assertEqual(proc.returncode & 0xFFFFFFFF, 0xC0000005)
            new = [f for f in glob.glob(os.path.join(folder, "ycgrid-crash-*-%d.dmp" % proc.pid)) if f not in before]
            if not new:  # written before the listing above
                new = glob.glob(os.path.join(folder, "ycgrid-crash-*-%d.dmp" % proc.pid))
            self.assertEqual(len(new), 1, folder)
            self.assertGreater(os.path.getsize(new[0]), 10_000)
            os.remove(new[0])
        finally:
            if proc.poll() is None:
                proc.kill()


class PythonWarningTest(unittest.TestCase):
    """A Python warning (numpy's RuntimeWarning in a PY cell) is a message in the Error Log, not an error (on stderr it
    was one: chaos seed 5111)."""

    def test_a_warning_is_a_message_not_an_error(self):
        from yc import excel, ui

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=proc.pid)
            excel.Range("A1").Formula = "=PY(\"__import__('warnings').warn('careful here')\")"
            found = []
            deadline = time.time() + 40
            while not found and time.time() < deadline:
                found = [e for e in ui.errors() if "careful here" in str(e.get("text", ""))]
                time.sleep(0.5)
            self.assertTrue(found, [e.get("title") for e in ui.errors()][-5:])
            self.assertFalse(found[0].get("error"), found[0])
            self.assertFalse([e for e in ui.errors(errors_only=True) if "careful here" in str(e.get("text", ""))])
        finally:
            proc.kill()


class CopyModeObjectModelTest(unittest.TestCase):
    """Excel 16's object model, measured 2026-11-15: Rows.Insert while a copy is held inserts the copied cells and keeps
    copy mode; Rows / Columns / Range.Delete end it."""

    def test_insert_keeps_copy_mode_and_delete_ends_it(self):
        from yc import excel

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=proc.pid)
            ws = excel.ActiveSheet
            ws.Range("C10").Value = 9
            ws.Range("C10").Copy()
            ws.Rows(20).Insert()
            self.assertEqual(excel.Application.CutCopyMode, 1)
            self.assertEqual(ws.Range("C20").Value, 9)
            for delete in (lambda: ws.Rows(30).Delete(), lambda: ws.Columns(8).Delete(), lambda: ws.Range("F50:F52").Delete(-4162)):
                ws.Range("C10").Copy()
                delete()
                self.assertEqual(excel.Application.CutCopyMode, 0)
        finally:
            proc.kill()


class FormatPainterWholeSheetTest(unittest.TestCase):
    """Format Painter over the whole sheet: basics seq338133 merged the whole sheet (a value in its last cell), and a
    click with the painter, which selects the merged cell, pasted the copied formats 17 billion times (a freeze)."""

    def test_whole_sheet_painted_onto_itself_and_a_large_copy_refused(self):
        from yc import excel, ui

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            inp = lambda s: conn.request("ui.input", {"script": s, "timeout_ms": 30000}, timeout=60)
            ws = excel.ActiveSheet
            ws.Range("XFD1048576").Value2 = "far"
            inp("g:A1|w:2|k:c-A|w:3|x:MergeCenter|w:4|k:Escape|w:2|m:2,2|w:2")
            r = ui.cell_rect("H8")
            x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
            n = len(ui.errors())
            t0 = time.time()
            inp(f"x:FormatPainter|w:2|m:{x},{y}|w:2|c:{x},{y}|w:3")
            self.assertLess(time.time() - t0, 10)
            # the whole sheet is past the 4,000,000 cells the painter copies (basics seq396008: a copy of millions
            # of cells took the memory): it says so at once, and nothing is painted
            texts = [e.get("text", "") for e in ui.errors()[n:]]
            self.assertTrue(any("too large to copy the formats" in t for t in texts), texts)
            self.assertFalse(ui.state().get("format_painter"))
        finally:
            proc.terminate()


class AutoSumWholeSelectionTest(unittest.TestCase):
    """AutoSum (Alt+=) over the whole sheet, whole columns and whole rows, as Excel 16 does (measured 2026-11-15): the
    whole sheet starts editing the active cell and writes nothing (it wrote a million formulas: chaos seed 5324), whole
    columns get their totals in the last row, whole rows in the last column."""

    def test_whole_sheet_columns_and_rows(self):
        from yc import excel

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            inp = lambda s: conn.request("ui.input", {"script": s, "timeout_ms": 60000}, timeout=90)
            ws = excel.ActiveSheet
            ws.Range("F21:G32").Value = [[i, i + 1] for i in range(12)]
            ws.Range("G4:H4").Value = [[1, 2]]
            t0 = time.time()
            inp("g:A1|w:2|k:c-A|w:3|k:a-Equal|w:3")
            self.assertLess(time.time() - t0, 10)
            from yc import ui
            self.assertTrue(ui.state().get("edit"), "the whole sheet: the active cell's editor")
            inp("k:Escape|w:3")
            self.assertEqual(ws.UsedRange.Address, "$F$4:$H$32")
            inp("g:F:H|w:2|k:a-Equal|w:3")
            self.assertEqual(ws.Range("F1048576").Formula, "=SUM(F4:F1048575)")
            self.assertEqual(ws.Range("XFD21").Formula, "")
            inp("k:c-Z|w:3|g:21:32|w:2|k:a-Equal|w:3")
            self.assertEqual(ws.Range("XFD21").Formula, "=SUM(F21:XFC21)")
            self.assertEqual(ws.Range("F1048576").Formula, "")
        finally:
            proc.kill()

    def test_an_empty_formatted_column_to_the_sheet_end_writes_totals_as_far_as_the_data(self):
        # basics seq417545: a number format over XFD4:XFD1048576 made the used range reach the sheet's end, and AutoSum
        # over that empty column wrote a total in each of its million rows (18 s); formats aren't values
        from yc import excel

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            inp = lambda s: conn.request("ui.input", {"script": s, "timeout_ms": 60000}, timeout=90)
            ws = excel.ActiveSheet
            ws.Range("A1:C6").Value = [[r * 10 + c for c in range(1, 4)] for r in range(1, 7)]
            ws.Range("XFD4:XFD1048576").NumberFormat = "#,##0.00"
            t0 = time.time()
            inp("g:XFD4:XFD1048576|w:2|k:a-Equal|w:3")
            self.assertLess(time.time() - t0, 8)
            # as without the format: the totals stop past the data, not at the sheet's end
            self.assertEqual(ws.Range("XFD100").Formula, "")
        finally:
            proc.kill()

    def test_totals_beside_whole_columns_go_as_far_as_the_data(self):
        # Excel 16, measured 2026-11-16: E:I with numbers in E5:H9 and column I empty writes I5:I9 and E1048576:I1048576
        # (ten formulas); 5:9 with row 9 empty writes E9:I9 and XFD5:XFD9. Every row of E:I got a total, a million
        # formulas (chaos seed 5643: 80 s on a debug build)
        from yc import excel

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            inp = lambda s: conn.request("ui.input", {"script": s, "timeout_ms": 60000}, timeout=90)
            ws = excel.ActiveSheet
            ws.Range("E5:H9").Value = [[r * 10 + c for c in range(5, 9)] for r in range(5, 10)]
            t0 = time.time()
            inp("g:E:I|w:2|k:a-Equal|w:3")
            self.assertLess(time.time() - t0, 10)
            self.assertEqual(ws.Range("I5").Formula, "=SUM(E5:H5)")
            self.assertEqual(ws.Range("I9").Formula, "=SUM(E9:H9)")
            self.assertEqual(ws.Range("I10").Formula, "")
            self.assertEqual(ws.Range("H1048576").Formula, "=SUM(H5:H1048575)")
            self.assertEqual(ws.Range("I1048576").Formula, "=SUM(E1048576:H1048576)")
            self.assertEqual(ws.UsedRange.Address, "$E$5:$I$1048576")
            inp("k:c-Z|w:3")
            ws.Range("E9:H9").ClearContents()
            ws.Range("I5:I8").Value = [[1]] * 4
            t0 = time.time()
            inp("g:5:9|w:2|k:a-Equal|w:3")
            self.assertLess(time.time() - t0, 10)
            self.assertEqual(ws.Range("E9").Formula, "=SUM(E5:E8)")
            self.assertEqual(ws.Range("I9").Formula, "=SUM(I5:I8)")
            self.assertEqual(ws.Range("J9").Formula, "")
            self.assertEqual(ws.Range("XFD8").Formula, "=SUM(E8:XFC8)")
            self.assertEqual(ws.Range("XFD9").Formula, "=SUM(E9:XFC9)")
        finally:
            proc.kill()

    def test_a_range_down_to_the_last_row_gets_totals_as_far_as_the_data(self):
        # chaos seed 6089: AutoSum over B10:J1048576 wrote a row total in every row to the sheet's end (a million
        # formulas). Excel 16 (measured with CommandBars.ExecuteMso("AutoSum")): B and C, the columns with numbers, get
        # their totals in row 1048576, the empty columns none, and J1048576 the grand total
        from yc import excel

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            inp = lambda s: conn.request("ui.input", {"script": s, "timeout_ms": 60000}, timeout=90)
            ws = excel.ActiveSheet
            ws.Range("B10:C20").Value = [[r, r * 2] for r in range(10, 21)]
            t0 = time.time()
            inp("g:B10:J1048576|w:2|k:a-Equal|w:3")
            self.assertLess(time.time() - t0, 10)
            self.assertEqual(ws.Range("B1048576").Formula, "=SUM(B10:B1048575)")
            self.assertEqual(ws.Range("C1048576").Formula, "=SUM(C10:C1048575)")
            self.assertEqual(ws.Range("D1048576").Formula, "")
            self.assertEqual(ws.Range("J1048576").Formula, "=SUM(B1048576:I1048576)")
            self.assertEqual(ws.Range("J21").Formula, "")
            # again over the same selection: the totals are redone, still as far as the data (the totals in row
            # 1048576 had made the used range reach the end, and every row got a total)
            t0 = time.time()
            inp("g:B10:J1048576|w:2|k:a-Equal|w:3")
            self.assertLess(time.time() - t0, 10)
            self.assertEqual(ws.Range("J20").Formula, "=SUM(B20:I20)")
            self.assertEqual(ws.Range("J500").Formula, "")
            self.assertEqual(ws.Range("B1048576").Formula, "=SUM(B10:B1048575)")
        finally:
            proc.kill()


class DeleteAreasTest(unittest.TestCase):
    """Delete Sheet Rows over several areas, one of them a table's header row: Excel refuses the whole command with
    the table's message. YCGrid deleted the other rows and then said the sheet was protected (basics seq270330)."""

    def test_one_refused_area_refuses_them_all(self):
        from yc import excel, ui

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            inp = lambda s: conn.request("ui.input", {"script": s, "timeout_ms": 60000}, timeout=90)
            ws = excel.ActiveSheet
            ws.Range("B2").Value = "keep"
            ws.Range("D5:E7").Value = [["a", "b"], [1, 2], [3, 4]]
            ws.ListObjects.Add(1, ws.Range("D5:E7"), None, 1)
            seen = len(ui.errors())
            inp("g:B2|H:ctrl|A:D5|h:|w:3|x:DeleteRows|w:4")
            msgs = [e.get("text", str(e)) for e in ui.errors()[seen:]]
            self.assertTrue(msgs and "table" in msgs[-1] and "protected" not in msgs[-1], msgs)
            self.assertEqual(ws.Range("B2").Value, "keep")
            self.assertEqual(ws.ListObjects(1).Range.Address, "$D$5:$E$7")
        finally:
            proc.kill()


class PasteOffSheetTest(unittest.TestCase):
    """A paste whose copy area runs past the sheet's last column from the destination is refused with Excel's message
    and changes nothing (Excel 16, measured): YCGrid pasted the used part and selected J2:XFL3 (basics seq 268656)."""

    def test_copy_area_past_the_edge(self):
        from yc import excel, ui

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            inp = lambda s: conn.request("ui.input", {"script": s, "timeout_ms": 60000}, timeout=90)
            ws = excel.ActiveSheet
            ws.Range("B5").Value = 1
            ws.Range("C6").Value = 2
            for how in ("k:c-V", "k:c-a-V|w:6|k:Enter"):
                seen = len(ui.errors())
                inp(f"g:B5:XFD6|w:2|k:c-C|g:J2|w:2|{how}|w:4")
                msgs = [e.get("text", str(e)) for e in ui.errors()[seen:]]
                self.assertTrue(any("can't paste here" in m for m in msgs), (how, msgs))
                self.assertEqual(ws.UsedRange.Address, "$B$5:$C$6")
                self.assertEqual(ui.state().get("selection"), "J2")
                inp("k:Escape|k:Escape|w:2")
            with self.assertRaises(Exception):
                ws.Range("B5:XFD6").Copy(ws.Range("J2"))
            self.assertEqual(ws.UsedRange.Address, "$B$5:$C$6")
            ws.Range("B5:XFD6").Copy(ws.Range("A8"))  # fits from column A
            self.assertEqual(ws.Range("B9").Value, 2)
        finally:
            proc.kill()


class ContainerKeyboardTest(unittest.TestCase):
    """A click on a container's free part keeps the keyboard there: typing then went into the active cell behind it,
    and Ctrl+Enter filled ten million selected cells (chaos seed 5501)."""

    def test_typing_after_a_click_on_a_container(self):
        from yc import excel, ui

        proc = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            conn = yc.connect(target=proc.pid)
            inp = lambda s: conn.request("ui.input", {"script": s, "timeout_ms": 60000}, timeout=90)
            inp("g:A1|w:2")
            ui.Containers.Add(Name="Box", Left=300, Top=300, Width=500, Height=300)
            inp("w:5")
            box = next(w for w in ui.windows() if "yc_container_" in w["name"])
            r = box["rect"]
            x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
            inp(f"m:{x},{y}|w:2|c:{x},{y}|w:3|t:7|k:Enter|w:3")
            self.assertEqual(excel.ActiveSheet.Range("A1").Formula, "")
        finally:
            proc.kill()


class RecordButtonTest(unittest.TestCase):
    """Trading > Record from the ribbon, stopped from the ribbon or the REC chip, then replayed: the file it leaves
    always replays, and a screen with nothing to record says so instead of leaving an empty file."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def inp(self, script, wait=0.5):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(wait)

    def status(self):
        return self.conn.request("replay.status", {})

    def test_a_nothing_to_record_says_so(self):
        from yc import ui

        before = {e["id"] for e in ui.errors()}
        self.inp("A:A1|t:1|k:Enter|x:RecordData|w:5")
        self.assertFalse(self.status()["recording"], "typed cells only: there is nothing to record")
        said = [e for e in ui.errors() if e["id"] not in before and "Nothing here can be recorded" in e["title"] + (e.get("text") or "")]
        self.assertTrue(said, "the user is told why, instead of getting an empty file that won't replay")

    def test_b_record_without_market_data_tables_replays(self):
        from yc import data, excel

        # an app's own table (record=False), like most examples' tables: Record used to write an empty file
        t = data.bind(excel.ActiveSheet, {"Sym": ["A", "B"], "Px": [1.0, 2.0]}, key="Sym", name="RecButtonTable", at="D1", record=False)
        self.inp("x:RecordData|w:5")
        st = self.status()
        self.assertTrue(st["recording"])
        for i in range(10):
            t.update({"Sym": ["A", "B"], "Px": [1.0 + i, 2.0 + i]}, wait=True)
            time.sleep(0.05)
        path = st["record_path"]
        self.inp("x:RecordData|w:5")  # stop (the REC chip runs the same)
        self.assertFalse(self.status()["recording"])
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
        self.conn.request("replay.play", {"path": path, "speed": 0.0, "start": 0.0, "loop": False})
        try:
            st = self.status()
            self.assertIn("recbuttontable", [x.lower() for x in st["tables"]])
            self.assertGreater(st["frames"], 5, "the snapshot and the updates are on the tape")
        finally:
            self.conn.request("replay.live", {})


class DockTest(unittest.TestCase):
    """A window the user docks comes out again, easily: its tab is shown; dragging the tab out, double-clicking it or
    its right-click menu floats it. Workbook.Protect(Windows) locks the workbook's windows, not YCGrid's own."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en", size=(1600, 900))
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def inp(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.3)

    def hand_drag(self, x, y, tx, ty):
        """Press, move in steps, hold over the target, release: docking previews need the hold."""
        x, y, tx, ty = int(x), int(y), int(tx), int(ty)
        steps = "|".join(f"m:{x + (tx - x) * k // 10},{y + (ty - y) * k // 10}|w:2" for k in range(1, 11))
        self.inp(f"m:{x},{y}|w:4|p:{x},{y}|w:3|{steps}|w:6|u:|w:8")

    def win(self, part):
        from yc import ui

        return next((w for w in ui.windows() if part in w["name"]), None)

    def test_window_list_has_every_window(self):
        # the Screenshot menu has 16 window slots; ui.windows listed only those, so the 17th window on the screen was
        # missing (or a shown tab was reported hidden)
        from yc import plot, ui

        figs = [plot.figure(f"Many {i}") for i in range(20)]
        try:
            for f in figs:
                f[0].line([0, 1, 2], [1, 3, 2])
            self.inp("w:6")
            shown = [w for w in ui.windows() if w["label"].startswith("Many ")]
            self.assertEqual(sorted(w["label"] for w in shown), sorted(f"Many {i}" for i in range(20)))
            self.assertTrue(all(w["visible"] for w in shown), [w["label"] for w in shown if not w["visible"]])
        finally:
            for f in figs:
                f.close()

    def dock_under_the_sheet(self, part):
        """Drop it on the bottom target of the dock space (the lowest edge of the sheets and the docked windows)."""
        from yc import ui

        w, grid = self.win(part), self.win("yc_gridhost")
        bottom = max(x["rect"][3] for x in ui.windows() if x["docked"] or x is grid or "yc_gridhost" in x["name"])
        self.hand_drag(w["rect"][0] + 60, w["rect"][1] + 12, (grid["rect"][0] + grid["rect"][2]) / 2, bottom - 22)
        w = self.win(part)
        self.assertTrue(w["docked"], w)
        return w

    def open_error_log(self):
        from yc import ui

        if not self.win("yc_errors"):   # the command shows / hides it
            ui.command("ErrorLog")
            self.inp("w:8")

    def take_out_three_ways(self, part):
        w = self.dock_under_the_sheet(part)
        tx, ty = w["rect"][0] + 60, w["rect"][1] + 13             # its tab
        self.hand_drag(tx, ty, 700, 300)                           # 1. drag the tab out
        self.assertFalse(self.win(part)["docked"])
        w = self.dock_under_the_sheet(part)
        self.inp(f"m:{tx:.0f},{ty:.0f}|w:4|D:{tx:.0f},{ty:.0f}|w:10")   # 2. double-click the tab
        w = self.win(part)
        self.assertFalse(w["docked"])
        self.assertLess(w["rect"][2] - w["rect"][0], 1600 * 0.7)  # a window over the sheet, not the docked strip
        self.dock_under_the_sheet(part)
        self.inp(f"m:{tx:.0f},{ty:.0f}|w:4|r:|w:6|k:DownArrow|w:2|k:UpArrow|w:2|k:Enter|w:10")   # 3. right-click > Float
        self.assertFalse(self.win(part)["docked"])

    def test_a_docked_tool_window_comes_out(self):
        self.open_error_log()
        self.take_out_three_ways("yc_errors")

    def test_each_tab_of_a_group_is_its_own(self):
        from yc import ui

        self.open_error_log()
        log = self.dock_under_the_sheet("yc_errors")
        ui.pane("watch", True)
        self.inp("w:8")
        watch = next(w for w in ui.windows() if "Watch" in w["label"])
        lr = log["rect"]
        self.hand_drag(watch["rect"][0] + 60, watch["rect"][1] + 12, (lr[0] + lr[2]) / 2, (lr[1] + lr[3]) / 2)  # a tab
        try:
            log, watch = self.win("yc_errors"), next(w for w in ui.windows() if "Watch" in w["label"])
            self.assertTrue(log["docked"] and watch["docked"], (log, watch))
            self.assertEqual(log["rect"], watch["rect"], "one group of tabs")
            a, b = log["tab"], watch["tab"]
            self.assertTrue(a[2] <= b[0] or b[2] <= a[0], f"two tabs side by side: {a} {b}")
            tx, ty = (a[0] + a[2]) / 2, (a[1] + a[3]) / 2
            self.inp(f"m:{tx:.0f},{ty:.0f}|w:4|D:{tx:.0f},{ty:.0f}|w:10")   # the Error Log's own tab
            self.assertFalse(self.win("yc_errors")["docked"], "the window whose tab was double-clicked comes out")
            self.assertTrue(next(w for w in ui.windows() if "Watch" in w["label"])["docked"], "the other tab stays")
        finally:
            ui.pane("watch", False)

    def test_protected_windows_lock_the_workbook_not_ycgrid(self):
        from yc import excel
        from yc.constants import ycDockBottom

        panels = excel.Application.Panels
        panels.Add("DeskPanel", Caption="DeskPanel", Dock=ycDockBottom, Height=160)
        excel.ThisWorkbook.Protect(Structure=True, Windows=True)
        try:
            self.inp("w:8")
            w = self.win("DeskPanel")
            tx, ty = w["rect"][0] + 40, w["rect"][1] + 13
            self.inp(f"m:{tx:.0f},{ty:.0f}|w:4|D:{tx:.0f},{ty:.0f}|w:10")
            self.assertTrue(self.win("DeskPanel")["docked"])           # the workbook's window stays
            self.open_error_log()
            self.take_out_three_ways("yc_errors")                     # YCGrid's own comes out
        finally:
            excel.ThisWorkbook.Unprotect()
            panels("DeskPanel").Delete()

    def test_docking_docked_figures_again_and_again(self):
        # a docked figure docked again leaves its node (ImGui deletes it at once) and splits another in the same
        # frame: the root's central node pointed at the deleted node and YCGrid crashed (0xC0000005)
        import random

        from yc import plot
        from yc.constants import ycDockBottom, ycDockLeft, ycDockRight

        rnd = random.Random(1)
        figs = []
        try:
            for i in range(4):
                f = plot.figure(f"Redock {i}", key=f"dock.redock{i}")
                f[0].line([0, 1, 2], [i, 3, 2])
                figs.append(f)
            for k in range(60):
                rnd.choice(figs).Dock(rnd.choice([ycDockRight, ycDockBottom, ycDockLeft]), Ratio=rnd.choice([0.2, 0.35, 0.5]))
                if k % 3 == 0:
                    self.inp("w:1")
                self.assertIsNone(self.proc.poll(), f"YCGrid ended after {k + 1} docks")
            self.inp("w:4")
            self.assertTrue(all(f.Docked for f in figs))
        finally:
            for f in figs:
                f.close()

    def test_a_figure_as_a_tab_in_another_figures_place(self):
        from yc import plot
        from yc.constants import ycDockBottom, ycDockTab

        a = plot.figure("Page A", key="dock.page_a")
        a[0].line([0, 1, 2], [1, 3, 2])
        b = plot.figure("Page B", key="dock.page_b")
        b[0].line([0, 1, 2], [2, 1, 3])
        try:
            a.Dock(ycDockBottom, Ratio=0.35)
            self.inp("w:6")
            wa = self.win("dock.page_a")
            b.Dock(ycDockTab, Anchor=a)
            self.inp("w:8")
            wb = self.win("dock.page_b")                   # the new tab shows; A waits behind its tab
            self.assertTrue(wb["docked"] and a.Docked, (wb, a.Docked))
            self.assertEqual(wa["rect"], wb["rect"])     # one place, two tabs
            with self.assertRaises(yc.YcError):
                b.Dock(ycDockTab, Anchor="Page A")         # an Anchor is a Figure
        finally:
            b.close()
            a.close()

    def test_a_locked_panel_stays_an_unlocked_one_floats(self):
        from yc import excel
        from yc.constants import ycDockBottom

        panels = excel.Application.Panels
        panels.Add("DockTicket", Caption="DockTicket", Dock=ycDockBottom, Height=160)
        panels("DockTicket").Controls.Add(1, "lbl", Caption="Buy")
        try:
            self.inp("w:8")
            w = self.win("DockTicket")
            self.assertTrue(w["docked"], w)
            tx, ty = w["rect"][0] + 40, w["rect"][1] + 13
            panels("DockTicket").Locked = True
            self.inp(f"w:4|m:{tx:.0f},{ty:.0f}|w:4|D:{tx:.0f},{ty:.0f}|w:10")
            self.assertTrue(self.win("DockTicket")["docked"])          # locked by the workbook: stays
            panels("DockTicket").Locked = False
            self.inp(f"w:4|m:{tx:.0f},{ty:.0f}|w:4|D:{tx:.0f},{ty:.0f}|w:10")
            self.assertFalse(self.win("DockTicket")["docked"])         # the user's to move again
        finally:
            panels("DockTicket").Delete()


class LockTest(unittest.TestCase):
    """A screen the user can't pull apart by accident, decided by Python (Workbook.Protect(Windows), Locked,
    Worksheet.Protect, Application.CellDragAndDrop), and undo of the user's changes while Python keeps writing."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en", size=(1600, 900))
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.ws = excel.Worksheets.Add()
        self.ws.Activate()

    def inp(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.4)

    def drag(self, x, y, x2, y2):
        self.inp(f"m:{x:.0f},{y:.0f}|w:6|d:{x:.0f},{y:.0f},{x2:.0f},{y2:.0f}|w:10")

    def win(self, label):
        from yc import ui

        return next((w for w in ui.windows() if w["label"].startswith(label)), None)

    def test_window_locks(self):
        from yc import excel, plot, ui
        from yc.constants import ycDockRight

        panels = excel.Application.Panels
        panels.Add("LockTicket", Caption="LockTicket", Dock=ycDockRight, Width=260)
        panels("LockTicket").Controls.Add(1, "lbl", Caption="Buy")
        # placed on the left, away from the panel's splitter at any DPI (the default place can cover it)
        fig = plot.figure("Lock chart", key="lock.chart", pos=(20, 260), size=(360, 240))
        fig[0].line([0, 1, 2], [1, 3, 2])
        try:
            time.sleep(0.8)
            # unlocked: the splitter and the title bar move
            x0 = self.win("LockTicket")["rect"][0]
            y = self.win("LockTicket")["rect"][1] + 30   # the figure is on the left, so the splitter takes the drag
            self.drag(x0 - 1, y, x0 - 81, y)
            x1 = self.win("LockTicket")["rect"][0]
            self.assertLess(x1, x0 - 40, "an unlocked splitter moves")
            r = self.win("Lock chart")["rect"]
            self.drag(r[0] + 60, r[1] + 8, r[0] + 160, r[1] + 58)
            r1 = self.win("Lock chart")["rect"]
            self.assertGreater(r1[0], r[0] + 50, "an unlocked figure moves")
            # one window
            fig.Locked = True
            self.assertTrue(fig.Locked)
            self.drag(r1[0] + 60, r1[1] + 8, r1[0] + 160, r1[1] + 58)
            self.assertEqual(self.win("Lock chart")["rect"][:2], r1[:2])
            fig.Locked = False
            # every window
            excel.ThisWorkbook.Protect(Structure=False, Windows=True)
            self.assertTrue(excel.ThisWorkbook.ProtectWindows)
            self.drag(x1 - 1, y, x1 - 81, y)
            self.assertAlmostEqual(self.win("LockTicket")["rect"][0], x1, delta=1.5)
            self.drag(r1[0] + 60, r1[1] + 8, r1[0] - 40, r1[1] + 58)
            self.assertEqual(self.win("Lock chart")["rect"][:2], r1[:2])
            ui.command("LayoutReset")   # the user's Reset Layout says why it can't
            time.sleep(0.3)
            self.assertIn("protected", ui.state().get("status") or "")
            fig.Dock(ycDockRight, Ratio=0.3)   # Python still lays the screen out
            time.sleep(0.8)
            self.assertTrue(fig.Docked)
            excel.ThisWorkbook.Unprotect()
            self.assertFalse(excel.ThisWorkbook.ProtectWindows)
            panels("LockTicket").Locked = True
            self.assertTrue(panels("LockTicket").Locked)
            self.assertFalse([e for e in ui.errors() if e.get("error")])
        finally:
            fig.close()
            panels("LockTicket").Delete()

    def test_new_figure_fits_the_main_window(self):
        from yc import plot

        figs = [plot.figure(f"Fit {i}") for i in range(3)]   # the default size and the cascade
        try:
            time.sleep(0.8)
            for i in range(3):
                r = self.win(f"Fit {i}")["rect"]
                self.assertGreaterEqual(min(r[0], r[1]), 0, r)
                self.assertLessEqual(r[2], 1600 + 0.5, f"Fit {i} hangs off the right edge: {r}")
                self.assertLessEqual(r[3], 900 + 0.5, f"Fit {i} hangs off the bottom: {r}")
        finally:
            for f in figs:
                f.close()

    def test_cell_drag_and_drop_and_undo_while_python_writes(self):
        from yc import excel, ui

        ws = self.ws
        ws.Range("B2").Value = 5
        self.inp("A:B2|w:4")
        r = ui.cell_rect("B2")
        h = r[3] - r[1]
        self.drag(r[2] - 1, (r[1] + r[3]) / 2, r[2] - 1, (r[1] + r[3]) / 2 + 2 * h)   # the selection's right border
        self.assertIsNone(ws.Range("B2").Value)
        self.assertEqual(ws.Range("B4").Value, 5)
        for i in range(5):   # live data keeps coming
            ws.Range("H1").Value = i
        self.inp("k:c-Z|w:6")
        self.assertEqual(ws.Range("B2").Value, 5, "the user's move is undone although Python wrote since")
        self.assertIsNone(ws.Range("B4").Value)
        self.assertEqual(ws.Range("H1").Value, 4, "Python's writes aren't undone")
        excel.Application.CellDragAndDrop = False
        try:
            self.assertFalse(excel.Application.CellDragAndDrop)
            self.inp("A:B2|w:4")
            r = ui.cell_rect("B2")
            self.drag(r[2] - 1, (r[1] + r[3]) / 2, r[2] - 1, (r[1] + r[3]) / 2 + 2 * h)
            self.assertEqual(ws.Range("B2").Value, 5)
            self.assertIsNone(ws.Range("B4").Value)
        finally:
            excel.Application.CellDragAndDrop = True

    def test_protected_sheet_offers_no_drags(self):
        from yc import ui

        ws = self.ws
        ws.Range("C3").Value = 7
        ws.Range("E5").Locked = False
        ws.Protect(UserInterfaceOnly=True)
        try:
            self.inp("A:C3|w:4")
            r = ui.cell_rect("C3")
            h = r[3] - r[1]
            self.drag(r[2] - 1, (r[1] + r[3]) / 2, r[2] - 1, (r[1] + r[3]) / 2 + 2 * h)
            self.assertEqual(ws.Range("C3").Value, 7)
            w0 = ws.Columns("C").ColumnWidth
            self.drag(r[2], r[1] - h * 1.5, r[2] + 80, r[1] - h * 1.5)   # C's border in the header row
            self.assertEqual(ws.Columns("C").ColumnWidth, w0)
            ws.Range("C3").Value = 8     # UserInterfaceOnly: Python still writes
            self.assertEqual(ws.Range("C3").Value, 8)
            self.inp("A:E5|w:2|t:12|k:Enter|w:4")   # an unlocked input cell
            self.assertEqual(ws.Range("E5").Value, 12)
            self.assertFalse([e for e in ui.errors() if e.get("error") and "protected" in str(e.get("text", "")).lower()])
        finally:
            ws.Unprotect()

    def test_locked_windows_are_saved_with_the_workbook(self):
        from yc.constants import ycDockRight

        tmp = tempfile.mkdtemp(prefix="yclock_")
        book = os.path.join(tmp, "Locked.ycg")
        p0 = yc.launch(hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=p0.pid)
            from yc import excel

            excel.Application.Panels.Add("Saved", Caption="Saved", Dock=ycDockRight, Width=200)
            excel.Application.Panels("Saved").Locked = True
            excel.ThisWorkbook.SaveAs(book)
        finally:
            p0.terminate()
            p0.wait(10)
        p1 = yc.launch(book, hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=p1.pid)
            from yc import excel

            time.sleep(0.5)
            self.assertTrue(excel.Application.Panels("Saved").Locked)
        finally:
            p1.terminate()
            p1.wait(10)
            yc.connect(target=self.proc.pid)   # the class's instance again
            shutil.rmtree(tmp, ignore_errors=True)

    def test_the_users_refusal_isnt_pythons_error(self):
        """Typing into a locked cell is refused for the user; Python's requests running at that moment succeed (they
        used to get ProtectedError: the refusal was counted for whichever request was running)."""
        import threading

        from yc import excel

        ws = self.ws
        ws.Range("A1").Value = 1
        ws.Protect(UserInterfaceOnly=True)
        errors, stop = [], threading.Event()

        def reads():
            while not stop.is_set():
                try:
                    excel.Worksheets(1).Name
                    ws.Range("B1").Value = 2
                except Exception as e:  # noqa: BLE001
                    errors.append(repr(e))

        t = threading.Thread(target=reads, daemon=True)
        t.start()
        try:
            for _ in range(6):
                self.inp("A:A1|w:2|t:9|w:2|k:Escape|w:2")
        finally:
            stop.set()
            t.join(10)
            ws.Unprotect()
        self.assertEqual(ws.Range("A1").Value, 1)
        self.assertEqual(errors, [])

    def test_typing_is_undone_while_live_values_stream(self):
        ws = self.ws
        ws.Range("C3").Value = 1
        self.inp("A:C3|w:2|t:2|k:Enter|w:4")
        self.assertEqual(ws.Range("C3").Value, 2)
        for i in range(20):
            ws.Range("D3").Value = i * 1.5
        self.inp("k:c-Z|w:6")
        self.assertEqual(ws.Range("C3").Value, 1)
        self.assertEqual(ws.Range("D3").Value, 28.5)


if __name__ == "__main__":
    unittest.main()


class OddInputTest(unittest.TestCase):
    """Values a person types that Excel refuses or keeps as text, and long Chinese texts in the places YCGrid shortens
    them (undo labels, the Clipboard pane, sheet names from file names): what the user sees stays right, and every
    later request still decodes."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        from yc import safety

        safety.set_operator_mode(True)
        cls.err_mark = 0

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.ws = excel.Worksheets.Add()
        self.ws.Activate()
        self.keys("k:Escape|w:2|m:2,2|w:1")
        self.messages()

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.3)

    def messages(self):
        """The messages shown since the last call (a hidden instance answers them and logs them)."""
        from yc import ui

        es = [e for e in ui.errors(errors_only=False) if e["id"] > type(self).err_mark]
        if es:
            type(self).err_mark = max(e["id"] for e in es)
        return [e.get("text", "") for e in es]

    def test_row_height_and_column_width_outside_the_range_are_refused(self):
        from yc import ui

        self.ws.Rows(2).RowHeight = 15
        self.keys("A:B2|w:2")
        ui.command("RowHeight")
        time.sleep(0.4)
        self.assertTrue(ui.state()["dialog"], "Row Height opened")
        self.keys("k:c-A|t:-5|k:Enter|w:5")
        self.assertTrue(any("between 0 and 409" in m for m in self.messages()), "Excel's message for a height of -5")
        self.assertFalse(self.ws.Rows(2).Hidden, "a refused height never hides the row")
        self.assertEqual(self.ws.Rows(2).RowHeight, 15)
        self.assertTrue(ui.state()["dialog"], "the dialog stays, with the text to fix")
        self.keys("k:c-A|t:500|k:Enter|w:5")
        self.assertTrue(any("between 0 and 409" in m for m in self.messages()))
        self.assertEqual(self.ws.Rows(2).RowHeight, 15)
        self.keys("k:c-A|t:1.2.3|k:Enter|w:5")  # the box takes digits only; two points are not a number
        self.assertTrue(any("not valid" in m for m in self.messages()), "1.2.3 is not a height")
        self.assertTrue(ui.state()["dialog"])
        self.keys("k:c-A|t:30|k:Enter|w:5")
        self.assertFalse(ui.state()["dialog"], "a valid height closes the dialog")
        self.assertEqual(self.ws.Rows(2).RowHeight, 30)
        width = self.ws.Columns("B").ColumnWidth
        ui.command("ColumnWidth")
        time.sleep(0.4)
        self.keys("k:c-A|t:300|k:Enter|w:5")
        self.assertTrue(any("between 0 and 255" in m for m in self.messages()), "Excel's message for a width of 300")
        self.assertEqual(self.ws.Columns("B").ColumnWidth, width)
        self.keys("k:Escape|w:3")
        self.assertFalse(ui.state()["dialog"])

    def test_undo_label_of_long_chinese_typing_is_whole_characters(self):
        from yc import ui

        text = "x" + "\u53f0\u7a4d\u96fb" * 4
        self.keys("A:A1|t:" + text + "|k:Enter|w:3")
        self.assertEqual(self.ws.Range("A1").Value, text)
        label = ui.state()["undo"]["label"]  # a cut inside a character made this request fail to decode
        self.assertTrue('..."' in label and "\u53f0\u7a4d\u96fb" in label, label)  # Typing "x台積電台積電..." in A1
        self.assertTrue(ui.items(), "later requests still decode")

    def test_clipboard_pane_preview_of_long_chinese_text(self):
        from yc import ui

        text = "x" + "\u53f0\u7a4d\u96fb" * 50
        self.ws.Range("A3").Value = text
        self.keys("A:A3|w:2|k:c-C|w:3")
        ui.pane("clipboard", True)
        try:
            time.sleep(0.5)
            previews = [i["label"] for i in ui.items() if i["label"].startswith("x\u53f0")]  # decodes: cut between characters
            self.assertTrue(previews, "the copied text is listed")
            self.assertTrue(all(len(p.encode("utf-8")) <= 120 for p in previews), previews)
        finally:
            ui.pane("clipboard", False)
            self.keys("k:Escape|w:2")

    def test_number_beyond_the_double_range_stays_text(self):
        self.keys("A:A5|t:1e309|k:Enter|w:3")
        self.assertEqual(self.ws.Range("A5").Value, "1e309")
        self.keys("A:A6|t:1e308|k:Enter|w:3")  # past Excel's largest number, 9.99999999999999E+307: text (measured)
        self.assertEqual(self.ws.Range("A6").Value, "1e308")
        self.keys("A:A7|t:9.99999999999999e307|k:Enter|w:3")
        self.assertEqual(self.ws.Range("A7").Value, 9.99999999999999e307)

    def test_sheet_name_limit_counts_characters(self):
        from yc import excel

        with self.assertRaises(Exception):
            self.ws.Name = "A" * 32
        self.ws.Name = "\u53f0" * 31
        self.assertEqual(len(excel.Application.ActiveSheet.Name), 31)
        with self.assertRaises(Exception):
            self.ws.Name = "\u53f0" * 32

    def test_every_write_to_a_protected_cell_is_refused(self):
        # the bridge read the request's refusal count before the engine had added it: some refused writes came back
        # as done, and the script went on as if the cell held the new value
        from yc.errors import ProtectedError

        self.ws.Range("A1").Value = "locked"
        self.ws.Protect()
        try:
            done = 0
            for i in range(200):
                try:
                    self.ws.Range("A1").Value = i
                    done += 1
                except ProtectedError:
                    pass
            self.assertEqual(done, 0, "every write to the locked cell raises ProtectedError")
            self.assertEqual(self.ws.Range("A1").Value, "locked")
        finally:
            self.ws.Unprotect()

    def test_unhide_columns_shows_its_shortcut(self):
        from yc import ui

        by_id = {c["id"]: c for c in ui.commands()}
        self.assertEqual(by_id["UnhideColumns"]["shortcut"], "Ctrl+Shift+)")  # bound, but the menu showed none
        self.assertEqual(by_id["UnhideRows"]["shortcut"], "Ctrl+Shift+(")
        self.ws.Columns("B:C").Hidden = True
        self.keys("g:A:D|w:2|k:c-s-0|w:4")
        self.assertFalse(self.ws.Columns("B").Hidden, "Ctrl+Shift+) unhides the columns")

    def test_missing_recent_file_is_offered_for_removal(self):
        from yc import excel, ui

        folder = tempfile.mkdtemp()
        path = os.path.join(folder, "gone_soon.ycg")
        excel.ActiveWorkbook.SaveAs(path)
        end = time.time() + 10
        while not os.path.exists(path) and time.time() < end:
            time.sleep(0.2)
        excel.ActiveWorkbook.SaveAs(os.path.join(folder, "stays.ycg"))  # the window holds another file now
        time.sleep(1)
        os.remove(path)
        x, y = ui.state()["ribbon"]["tabs"]["File"]
        self.keys(f"c:{int(x)},{int(y)}|w:6")
        self.assertTrue(ui.state()["ribbon"]["backstage"])
        try:
            page = next(i for i in ui.ribbon_items() if i["cmd"] == "BackstageOpen")["rect"]
            self.keys(f"c:{(page[0] + page[2]) / 2},{(page[1] + page[3]) / 2}|w:6")
            row = next(i for i in ui.items() if i["label"].startswith("gone_soon.ycg\n"))["rect"]
            self.keys(f"c:{(row[0] + row[2]) / 2},{(row[1] + row[3]) / 2}|w:8")
            st = ui.state()
            self.assertTrue(st["dialog"], "the question (Excel: couldn't find the file, remove it from the list?)")
            self.assertTrue([w for w in st["windows"] if w.get("popup") and "yc_ask" in w["name"]], [w["name"] for w in st["windows"]])
            self.assertIn("stays.ycg", st["title"], "the open workbook is untouched")
            self.keys("k:Enter|w:6")  # Yes: the entry leaves the list
            self.assertFalse(ui.state()["dialog"])
            labels = [i["label"] for i in ui.items()]
            self.assertFalse([l for l in labels if l.startswith("gone_soon.ycg\n")], labels[:20])
            self.assertTrue([l for l in labels if l.startswith("stays.ycg\n")], "the other entry stays")
        finally:
            self.keys("k:Escape|w:3")
            self.assertFalse(ui.state()["ribbon"]["backstage"])


@unittest.skipUnless(EXE and not os.environ.get("YC_NO_LIVE"), "needs a built ycgrid.exe")
class ColumnFilesLiveTest(unittest.TestCase):
    """yc.data.view and plot_files read .npy files in C++: every dtype save_columns writes, and files that are cut
    short, lie about their size or hold Python objects are an error message, never the end of YCGrid."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        cls.dir = tempfile.mkdtemp(prefix="yc_npy_")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)
        shutil.rmtree(cls.dir, ignore_errors=True)

    def raw_npy(self, name, header, data=b""):
        h = header.encode("latin-1")
        h += b" " * ((64 - (10 + len(h) + 1) % 64) % 64) + b"\n"
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write(b"\x93NUMPY\x01\x00" + len(h).to_bytes(2, "little") + h + data)
        return path

    def test_small_integers_and_half_floats_are_read(self):
        # int8 / int16 / uint8 / uint16 / float16 columns of a DataFrame failed with "unsupported dtype"
        import numpy as np

        from yc import data

        cols = {"i1": np.array([-3, 1, 2], dtype=np.int8), "i2": np.array([-300, 1, 2], dtype=np.int16),
                "u1": np.array([200, 1, 2], dtype=np.uint8), "u2": np.array([60000, 1, 2], dtype=np.uint16),
                "f2": np.array([0.5, 1.5, -2.25], dtype=np.float16), "s": np.array([b"ab", b"c", b""], dtype="S2")}
        files = data.save_columns(cols, folder=self.dir)  # bytes ('S') failed there, turned into float64
        v = data.view(files, title="small")
        try:
            self.assertEqual(v.state()["rows"], 3)
            self.assertEqual(v.filter("i1", "<0"), 1)
            self.assertEqual(v.filter("i1", ""), 3)
            self.assertEqual(v.filter("u1", ">100"), 1)
            self.assertEqual(v.filter("u1", ""), 3)
            self.assertEqual(v.filter("u2", ">50000"), 1)
            self.assertEqual(v.filter("u2", ""), 3)
            self.assertEqual(v.filter("f2", "<-2"), 1)
            self.assertEqual(v.filter("f2", ""), 3)
            self.assertEqual(v.filter("s", "ab"), 1)
        finally:
            v.close()

    def test_bad_files_are_an_error_message(self):
        from yc import data, ui
        from yc.errors import YcError

        bad = {
            "object": self.raw_npy("obj.npy", "{'descr': '|O', 'fortran_order': False, 'shape': (3,), }", b"\0" * 24),
            "short": self.raw_npy("short.npy", "{'descr': '<f8', 'fortran_order': False, 'shape': (1000,), }", b"\0" * 16),
            "overflow": self.raw_npy("ovf.npy", "{'descr': '<f8', 'fortran_order': False, 'shape': (2305843009213693952, 8), }", b"\0" * 64),
            "text": self.raw_npy("txt.npy", "{'descr': '<U4611686018427387904', 'fortran_order': False, 'shape': (1, 4), }", b"\0" * 64),
        }
        for what, path in bad.items():
            with self.subTest(what):
                with self.assertRaises(YcError):
                    data.view({"x": path}, title=what)
                self.assertIn("title", ui.state(), "YCGrid still answers")


@unittest.skipUnless(EXE and not os.environ.get("YC_NO_LIVE"), "needs a built ycgrid.exe")
class ObjectModelParityTest(unittest.TestCase):
    """What code written for Excel expects of Find, Sort, SpecialCells, sheet copies and renames and formats that
    differ across a range."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.ws = excel.Worksheets.Add()
        self.ws.Activate()

    def test_find_starts_after_the_upper_left_cell(self):
        # Excel searches the first cell last when After is left out; YCGrid found it first
        ws = self.ws
        ws.Range("A1:A4").Value = [["apple"], ["Apple pie"], ["banana"], ["pineapple"]]
        self.assertEqual(ws.Range("A1:A4").Find("apple").Address, "$A$2")
        self.assertEqual(ws.Range("A1:A4").Find("apple", MatchCase=True).Address, "$A$4")
        self.assertEqual(ws.Range("A1:A4").Find("apple", LookAt=1).Address, "$A$1")  # xlWhole: only A1, found last
        self.assertEqual(ws.Range("A1:A4").Find("apple", SearchDirection=2, MatchCase=False, LookAt=2).Address, "$A$4")

    def test_sort_guesses_the_header(self):
        from yc import excel

        ws = self.ws
        ws.Range("A1:B4").Value = [["Name", "N"], ["b", 2], ["c", 1], ["a", 3]]
        ws.Range("A1:B4").Sort(Key1=ws.Range("A1"), Header=excel.xlGuess)
        self.assertEqual([ws.Range(f"A{i}").Value for i in range(1, 5)], ["Name", "a", "b", "c"])
        # one cell sorts its current region, as in Excel
        ws.Range("B2").Sort(Key1=ws.Range("B1"), Header=excel.xlYes)
        self.assertEqual([ws.Range(f"B{i}").Value for i in range(2, 5)], [1, 2, 3])
        self.assertEqual(ws.Range("A2").Value, "c", "the whole region moved")
        # the Sort object's xlGuess raised KeyError
        srt = ws.Sort
        srt.SortFields.Clear()
        srt.SortFields.Add(Key=ws.Range("A2:A4"))
        srt.SetRange(ws.Range("A1:B4"))
        srt.Header = excel.xlGuess
        srt.Apply()
        self.assertEqual([ws.Range(f"A{i}").Value for i in range(1, 5)], ["Name", "a", "b", "c"])

    def test_formulas_just_written_are_found_and_sorted_by_value(self):
        # Go To Special read the cells before their new formulas had values: =1+1 wasn't a formula yet
        from yc import excel

        ws = self.ws
        ws.Range("E1").Formula = "=1+1"
        self.assertEqual(ws.Cells.SpecialCells(excel.xlCellTypeFormulas).Address, "$E$1")
        ws.Range("G1").Formula = "=2+2"
        self.assertEqual(ws.Cells.SpecialCells(excel.xlCellTypeFormulas, excel.xlNumbers).Address, "$E$1,$G$1")
        ws.Range("A1:A3").Formula = [["=30"], ["=10"], ["=20"]]
        ws.Range("A1:A3").Sort(Key1=ws.Range("A1"), Header=excel.xlNo)
        self.assertEqual([ws.Range(f"A{i}").Value for i in range(1, 4)], [10, 20, 30])

    def test_a_copied_sheet_becomes_the_active_one(self):
        from yc import excel

        self.ws.Name = "Quotes"
        copy = self.ws.Copy(After=self.ws)
        self.assertEqual(excel.ActiveSheet.Name, "Quotes (2)")
        self.assertEqual(copy.Name, "Quotes (2)")

    def test_renaming_a_sheet_rewrites_the_names_that_refer_to_it(self):
        # a defined name kept "=Old!$A$1:$A$3" and stopped referring to anything
        from yc import excel

        ws = self.ws
        ws.Name = "OldName"
        ws.Range("A1:A3").Value = [[1], [2], [3]]
        wb = excel.ActiveWorkbook
        wb.Names.Add("RenNums", RefersTo="=OldName!$A$1:$A$3")
        ws.Range("B1").Formula = "=SUM(RenNums)"
        ws.Range("C1").Validation.Add(3, 1, 1, "=OldName!$A$1:$A$3")
        ws.Name = "New Name"
        self.assertEqual(wb.Names("RenNums").RefersTo, "='New Name'!$A$1:$A$3")
        self.assertEqual(wb.Names("RenNums").RefersToRange.Address, "$A$1:$A$3")
        self.assertEqual(ws.Range("C1").Validation.Formula1, "='New Name'!$A$1:$A$3")
        ws.Range("A1").Value = 10
        self.assertEqual(ws.Range("B1").Value, 15)
        wb.Names("RenNums").Delete()

    def test_formats_that_differ_read_as_none_and_write_back_as_nothing(self):
        ws = self.ws
        r = ws.Range("A1:A2")
        r.Value = [["x"], ["y"]]
        ws.Range("A1").Font.Bold = True
        ws.Range("A1").Font.Color = 0x0000FF
        ws.Range("A2").Interior.Color = 0x00FF00
        ws.Range("A2").HorizontalAlignment = -4108  # xlCenter
        self.assertIsNone(r.Font.Bold)
        self.assertIsNone(r.Font.Color)
        self.assertIsNone(r.Interior.Color)
        self.assertIsNone(r.HorizontalAlignment)
        self.assertEqual(r.Font.Italic, False, "the same in both cells")
        for name, obj in (("Bold", r.Font), ("Color", r.Font)):
            setattr(obj, name, None)
        r.Interior.Color = None
        r.HorizontalAlignment = None
        self.assertEqual(ws.Range("A1").Font.Bold, True)
        self.assertEqual(ws.Range("A2").Font.Bold, False)
        self.assertEqual(ws.Range("A1").Font.Color, 0x0000FF)
        self.assertEqual(ws.Range("A2").Interior.Color, 0x00FF00)
        self.assertEqual(ws.Range("A1").Interior.Color, 16777215)
        self.assertEqual(ws.Range("A2").HorizontalAlignment, -4108)
        # a whole column with one bold cell is mixed too; a uniform range is not
        self.assertIsNone(ws.Range("A:A").Font.Bold)
        self.assertEqual(ws.Range("C1:D5").Font.Bold, False)

    def test_relative_names_follow_the_cell_that_uses_them(self):
        # Names.Add with B1 active and "=Sheet!A1": the cell to the left, as in Excel (it worked as an absolute A1)
        from yc import excel

        ws = self.ws
        ws.Range("A1:A3").Value = [[1], [2], [3]]
        wb = excel.ActiveWorkbook
        ws.Range("B1").Select()
        wb.Names.Add("RelLeft", RefersTo=f"={ws.Name}!A1")
        ws.Range("B2").Formula = "=RelLeft"
        ws.Range("B3").Formula = "=RelLeft*10"
        self.assertEqual(ws.Range("B2").Value, 2)
        self.assertEqual(ws.Range("B3").Value, 30)
        ws.Range("C5").Select()
        self.assertEqual(wb.Names("RelLeft").RefersTo, f"={ws.Name}!B5")  # shown from the active cell
        self.assertEqual(wb.Names("RelLeft").RefersToR1C1, f"={ws.Name}!RC[-1]")
        self.assertEqual(wb.Names("RelLeft").RefersToRange.Address, "$B$5")
        wb.Names.Add("RelUp", RefersToR1C1=f"={ws.Name}!R[-1]C")
        ws.Range("A4").Formula = "=RelUp+100"
        self.assertEqual(ws.Range("A4").Value, 103)
        wb.Names("RelLeft").Delete()
        wb.Names("RelUp").Delete()

    def test_a_sheet_name_before_a_name_picks_that_sheets_name(self):
        from yc import excel

        ws = self.ws
        ws.Name = "PickSrc"
        ws.Range("A1").Value = 7
        ws.Names.Add("Pick", RefersTo="=PickSrc!$A$1")
        other = excel.Worksheets.Add()
        other.Range("A1").Formula = "=PickSrc!Pick*2"
        self.assertEqual(other.Range("A1").Value, 14)
        self.assertEqual(other.Range("A1").Formula, "=PickSrc!Pick*2")

    def test_typed_and_written_dates_and_date_functions(self):
        ws = self.ws
        ws.Range("A1").Value = "15-Jan-2024"
        self.assertEqual(ws.Range("A1").Value2, 45306)
        self.assertEqual(ws.Range("A1").NumberFormat, "d-mmm-yy")
        ws.Range("A2").Value = "Jan 2024"
        self.assertEqual(ws.Range("A2").Value2, 45292)
        ws.Range("A3").Formula = '=DATEVALUE("22-MAY-2011")'
        self.assertEqual(ws.Range("A3").Value, 40685)  # a serial number, as in Excel
        # a date function written by a macro shows a date, in every cell of the range; a format already set stays
        ws.Range("B1:B3").Formula = "=TODAY()"
        self.assertEqual([ws.Range(f"B{i}").NumberFormat for i in range(1, 4)], ["yyyy/m/d"] * 3)
        ws.Range("C2").NumberFormat = "0.00"
        ws.Range("C1:C2").Formula = "=NOW()"
        self.assertEqual(ws.Range("C1").NumberFormat, "yyyy/m/d h:mm")
        self.assertEqual(ws.Range("C2").NumberFormat, "0.00")
        # a number no date can show stays a number (it raised OverflowError)
        ws.Range("B1").Value = 2 ** 60
        self.assertEqual(ws.Range("B1").Value, float(2 ** 60))
        ws.Range("B2").Value = -1
        self.assertEqual(ws.Range("B2").Value, -1)

    def test_widths_and_heights_that_differ_and_out_of_range(self):
        from yc import errors

        ws = self.ws
        ws.Columns("A").ColumnWidth = 12
        self.assertIsNone(ws.Columns("A:B").ColumnWidth)
        ws.Columns("A:B").ColumnWidth = None  # read back: nothing changes
        self.assertEqual(ws.Columns("A").ColumnWidth, 12)
        self.assertNotEqual(ws.Columns("B").ColumnWidth, 12)
        ws.Columns("C:D").ColumnWidth = 15
        self.assertEqual(ws.Columns("C:D").ColumnWidth, 15)
        ws.Rows(2).RowHeight = 30
        self.assertIsNone(ws.Rows("1:3").RowHeight)
        ws.Rows(5).Hidden = True
        self.assertIsNone(ws.Rows("4:6").Hidden)
        ws.Rows("4:6").Hidden = None
        self.assertTrue(ws.Rows(5).Hidden)
        self.assertFalse(ws.Rows(4).Hidden)
        with self.assertRaises(errors.ApplicationDefinedError):
            ws.Columns("E").ColumnWidth = 300
        with self.assertRaises(errors.ApplicationDefinedError):
            ws.Rows(7).RowHeight = -5
        with self.assertRaises(errors.ApplicationDefinedError):
            ws.Name = "History"
