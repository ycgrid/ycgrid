# SPDX-License-Identifier: Apache-2.0
"""The example screens used the way a trader uses them, checked by their results: each opens its workbook in a hidden
YCGrid (its Workbook_Open builds the screen and starts its live data), then clicks what a person clicks (tabs, rows,
buttons) while the data streams, and checks what the screen shows. Needs numpy (the examples' simulations)."""
from __future__ import annotations

import os
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..")))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe")

try:
    import numpy  # noqa: F401

    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False


@unittest.skipUnless(HAVE_NUMPY, "the example screens need numpy")
class ScreenBase(unittest.TestCase):
    EXAMPLE = ""
    FILE = ""  # the workbook's name when it isn't the folder's

    @classmethod
    def setUpClass(cls):
        path = os.path.join(ROOT, "examples", cls.EXAMPLE, (cls.FILE or cls.EXAMPLE) + ".ycg")
        cls.proc = yc.launch(path, hidden=True, exe=EXE, lang="en", size=(2000, 1221))
        cls.conn = yc.connect(target=cls.proc.pid)
        end = time.time() + 90
        while not cls.conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
            time.sleep(0.2)
        time.sleep(2)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def keys(self, script, wait=0.3):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 15000}, timeout=30)
        time.sleep(wait)

    def click(self, address, wait=0.8):
        from yc import ui

        x0, y0, x1, y1 = ui.cell_rect(address)
        x, y = (x0 + x1) / 2, (y0 + y1) / 2
        self.keys(f"m:{x:.0f},{y:.0f}|w:2|c:{x:.0f},{y:.0f}|w:3", wait)

    def figure(self):
        figs = self.conn.request("plot.figures", {})
        self.assertTrue(figs, "the screen's figure")
        return figs[0]

    def assertClean(self):
        from yc import ui

        self.assertEqual(ui.state().get("imgui_asserts", 0), 0)
        self.assertEqual([e for e in ui.errors() if e.get("error")], [], "nothing in the Error Log")


class MarketWatchTest(ScreenBase):
    EXAMPLE = "market_watch"
    TABS = ["Watch list", "Semis", "Electronics", "Financials", "Shipping", "ETFs"]

    def codes(self):
        from yc import excel

        return [str(r[0]) for r in excel.ActiveSheet.Range("A6:A60").Value if r[0] not in (None, "")]

    def test_each_tab_lists_its_symbols_and_a_row_charts_its_symbol(self):
        from yc import excel

        ws = excel.ActiveSheet
        seen = set()
        for i, name in enumerate(self.TABS):
            self.click(ws.Cells(4, 1 + 2 * i).Address.replace("$", ""), 1.2)
            codes = self.codes()
            self.assertTrue(codes, name)
            self.assertTrue(ws.Cells(4, 1 + 2 * i).Font.Bold, f"{name} is lit")
            if name != "Watch list":
                self.assertFalse(seen & set(codes), f"{name} repeats another category's symbols")
                seen |= set(codes)
            self.click("B7", 1.2)
            self.assertEqual(str(ws.Range("AA1").Value), codes[1], f"{name}: the row clicked is the one charted")
            top = self.figure()["axes"][0]
            self.assertTrue(top["title"].startswith(codes[1] + " "), top["title"])
        self.assertClean()

    def test_rows_clicked_fast_while_quotes_stream_chart_one_set_of_lines(self):
        """A click runs on the host's thread beside the live loop: both drew the chart without a lock, each saw the
        symbol change and added its lines, and every price tag showed twice."""
        from yc import excel

        ws = excel.ActiveSheet
        self.click(ws.Cells(4, 1).Address.replace("$", ""), 1.2)  # the watch list
        for k in range(12):
            self.click(f"B{6 + k % 6}", 0.12)
        time.sleep(1.5)
        for ax in self.figure()["axes"]:
            labels = [it["label"] for it in ax["items"]]
            self.assertEqual(len(labels), len(set(labels)), f"{ax['title']}: {labels}")
        self.assertClean()


class OptionChainTest(ScreenBase):
    EXAMPLE = "option_chain"
    DAYS = [6.0, 20.0, 55.0, 111.0]  # the expiry tabs' days to expiry

    def test_each_expiry_tab_shows_its_series(self):
        from yc import excel

        ws = excel.ActiveSheet
        for i, days in enumerate(self.DAYS):
            self.click(ws.Cells(3, 1 + 2 * i).Address.replace("$", ""), 1.2)
            self.assertTrue(ws.Cells(3, 1 + 2 * i).Font.Bold, f"tab {i} is lit")
            self.assertEqual(ws.Cells(2, 7).Value, days, f"tab {i}: days to expiry")
            strikes = [r[0] for r in ws.Range("I7:I30").Value if r[0] not in (None, "")]  # the Strike column
            self.assertEqual(strikes, [float(k) for k in range(22300, 23801, 100)], f"tab {i}: the strikes")
        self.assertClean()

    def test_tabs_clicked_fast_while_quotes_stream_chart_one_smile_each(self):
        from yc import excel

        ws = excel.ActiveSheet
        for k in range(10):
            self.click(ws.Cells(3, 1 + 2 * (k % 4)).Address.replace("$", ""), 0.1)
        time.sleep(1.5)
        for ax in self.figure()["axes"]:
            labels = [it["label"] for it in ax["items"] if it["label"]]
            self.assertEqual(len(labels), len(set(labels)), f"{ax['title']}: {labels}")
        self.assertClean()


class DomLadderTest(ScreenBase):
    EXAMPLE = "dom_ladder"

    def ladder(self):
        return self.ws.Range("B6:F35").Value  # My Bid, Bid, Price, Ask, My Ask

    def working(self):
        rows = self.ws.Range("H9:P60").Value
        return [r for r in rows if r[0] not in (None, "") and r[8] == "Working"]

    def setUp(self):
        from yc import excel

        self.ws = excel.ActiveSheet

    def bid_row(self):
        lad = self.ladder()
        last = self.ws.Range("D4").Value  # the last price, above the Price column
        return next(k for k, r in enumerate(lad) if r[1] not in (None, "") and r[2] < last - 3)

    def test_orders_at_one_price_show_their_total(self):
        """The ladder showed the size of the last order at a price, not the sum: 1 where four lots worked."""
        k = self.bid_row()
        px = self.ladder()[k][2]
        for _ in range(4):
            row = next(i for i, r in enumerate(self.ladder()) if r[2] == px)
            self.click(f"C{6 + row}", 0.4)
        time.sleep(1.0)
        mine = sum(r[4] - (r[6] or 0) for r in self.working() if r[5] == px)
        shown = next(r[0] for r in self.ladder() if r[2] == px)
        self.assertEqual(shown, mine)
        self.assertGreaterEqual(mine, 4)
        self.assertClean()

    def test_a_drag_over_the_sizes_sends_nothing(self):
        """Selecting sizes by a drag that began on one sent an order: a click action ran on the press."""
        from yc import ui

        n0 = len(self.working())
        k = self.bid_row()
        x0, y0, x1, y1 = ui.cell_rect(f"C{6 + k}")
        x, y = int((x0 + x1) / 2), int((y0 + y1) / 2)
        self.keys(f"m:{x},{y}|w:2|d:{x},{y},{x},{y + 100}|w:6", 1.5)
        self.assertEqual(len(self.working()), n0)
        self.assertClean()


class TechnicalChartTest(ScreenBase):
    EXAMPLE = "technical_chart"

    def test_the_marked_day_follows_the_row_by_mouse_and_keys(self):
        """The mark of a daily bar read "7/2 00:00", a time nobody traded at: a day is tagged with its date."""
        from yc import excel, ui

        ws = excel.ActiveSheet
        self.click("A20", 1.0)
        tags = {ax.get("cursor_tag") for ax in self.figure()["axes"]}
        self.assertEqual(tags, {ws.Range("A20").Value.strftime("%Y-%m-%d")})
        self.keys("k:Down|w:3|k:Down|w:3|k:Down|w:3", 1.0)  # the keys move the mark too
        self.assertEqual(ui.state()["active_cell"], "A23")
        tags = {ax.get("cursor_tag") for ax in self.figure()["axes"]}
        self.assertEqual(tags, {ws.Range("A23").Value.strftime("%Y-%m-%d")})
        self.assertClean()


class OrderFlowTest(ScreenBase):
    EXAMPLE = "order_flow"

    def footprint(self):
        return self.figure()["axes"][0]

    def order_line(self):
        return next(h for h in self.footprint()["handles"] if h["kind"] == "hline")

    def test_the_working_order_is_in_view_and_a_fill_shows_on_its_line(self):
        """The order line sat below the prices the footprint fitted, out of view, so the trader couldn't see or drag
        it; and once the order filled the line still read BUY LMT, a working order that was gone."""
        time.sleep(2)
        ax, line = self.footprint(), self.order_line()
        self.assertTrue(line["label"].startswith("BUY LMT 1 @"), line["label"])
        self.assertTrue(ax["view"][2] <= line["y"] <= ax["view"][3], (line["y"], ax["view"]))
        # a double-click near the top of the plot: a buy limit above the market, which fills
        x0, y0, x1, y1 = ax["rect"]
        x, y = int((x0 + x1) / 2), int(y0 + (y1 - y0) * 0.03)
        self.keys(f"m:{x},{y}|w:3|D:{x},{y}|w:6", 0.5)
        end = time.time() + 10
        while time.time() < end and not self.order_line()["label"].startswith("filled"):
            time.sleep(0.3)
        self.assertTrue(self.order_line()["label"].startswith("filled"), self.order_line()["label"])
        self.assertClean()


class DemoTest(ScreenBase):
    EXAMPLE = "demo"
    OPENS = {"2330": 1050.0, "2317": 205.0, "2454": 1380.0, "2308": 395.0}

    def setUp(self):
        from yc import excel

        self.ws = excel.Worksheets("Quotes")

    def press(self, label, window="Demo Console"):
        from yc import ui

        it = next(i for i in ui.items(window) if i.get("label") == label)
        r = it["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:4", 0.3)

    def updates_a_second(self, sec=5):
        n, last, end = 0, self.ws.Range("C2:C11").Value, time.time() + sec
        while time.time() < end:
            v = self.ws.Range("C2:C11").Value
            n += v != last
            last = v
            time.sleep(0.05)
        return n / sec

    def test_pause_and_resume_keep_one_update_a_second(self):
        """Resuming within a second of a pause left the old tick waiting and started a second chain: every quick
        pause and resume added an update a second (three a second after three)."""
        for _ in range(3):
            self.press("Pause / Resume Live Updates")
            self.press("Pause / Resume Live Updates")
        self.assertLess(self.updates_a_second(), 1.6)
        self.assertClean()

    def test_a_sorted_table_keeps_each_change_against_its_own_open(self):
        """The prices were written back by position against the opens in their first order: after a sort every Change
        was measured against another stock's open (TSMC +408%)."""
        from yc import ui

        self.click("C2", 0.2)
        ui.command("SortDescending")
        time.sleep(3)
        for code, _name, price, change, *_ in self.ws.Range("A2:G11").Value:
            if str(code) in self.OPENS:
                self.assertAlmostEqual(change, price / self.OPENS[str(code)] - 1, delta=0.002, msg=code)
        self.assertClean()


class QuickPlotTest(ScreenBase):
    EXAMPLE = "quick_plot"

    def figures(self):
        return {f["title"]: f for f in self.conn.request("plot.figures", {})}

    def test_the_chart_is_in_view_and_zooms_and_the_heatmap_labels_dont_run_together(self):
        """The three windows opened cascaded and the heatmap, opened last, covered the ten million points the example
        is about (a wheel over the chart zoomed nothing); its column labels ran together (HONHAIFUBON)."""
        figs = self.figures()
        self.assertTrue(all(f["docked"] for f in figs.values()), figs.keys())
        self.assertTrue(figs["Correlation"]["axes"][0].get("labels_staggered"))
        ax = figs["10,000,000 points"]["axes"][0]
        x0, y0, x1, y1 = ax["rect"]
        cx, cy = int((x0 + x1) / 2), int((y0 + y1) / 2)
        self.keys(f"m:{cx},{cy}|w:4|W:3|w:8", 0.4)
        v = self.figures()["10,000,000 points"]["axes"][0]["view"]
        self.assertLess(v[1] - v[0], ax["view"][1] - ax["view"][0], "the wheel zooms the chart")
        self.keys(f"m:{cx},{cy}|w:4|D:{cx},{cy}|w:10", 0.5)
        self.assertClean()


class TimeMachineTest(ScreenBase):
    EXAMPLE = "time_machine"

    def test_the_ladder_shows_beside_its_candles_and_the_replay_bar_seeks(self):
        """The candles' sheet, opened last, and its floating chart hid the ladder whose fills the replay is about."""
        from yc import excel, replay, ui

        self.assertEqual(excel.ActiveSheet.Name, "Ladder")
        self.assertTrue(all(f["docked"] for f in self.conn.request("plot.figures", {})))
        end = time.time() + 40
        while time.time() < end and ui.state().get("replay", {}).get("mode") != "replay":
            time.sleep(0.5)
        label = next(w["label"] for w in ui.windows() if w["label"].startswith("Replay"))
        play = next(i for i in ui.items(label) if i.get("id", "").endswith("##play"))["rect"]
        bar = next(i for i in ui.items(label) if i.get("id") == "##pos")["rect"]
        x, y = int((play[0] + play[2]) / 2), int((play[1] + play[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6", 0.5)  # pause
        tx, ty = int(bar[0] + (bar[2] - bar[0]) * 0.25), int((bar[1] + bar[3]) / 2)
        self.keys(f"m:{tx},{ty}|w:3|c:{tx},{ty}|w:6", 0.5)
        st = replay.status()
        self.assertAlmostEqual(st["position"] / st["length"], 0.25, delta=0.08)
        self.assertClean()


class TxoMarketMakerTest(ScreenBase):
    EXAMPLE, FILE = "txo_market_maker", "txo_mm"

    def test_a_double_click_on_start_starts_the_strategy(self):
        """The second click of a double-click on Start ran the toggle again: the strategy started and stopped at once,
        and the trader saw Start where they had just clicked it."""
        from yc import excel, safety, ui

        safety.set_operator_mode(True)
        ws = excel.Worksheets("Strategies")
        ws.Activate()
        time.sleep(0.5)
        self.assertEqual(ws.Range("B3").Value, "Stopped")
        x0, y0, x1, y1 = ui.cell_rect("F3")
        x, y = int((x0 + x1) / 2), int((y0 + y1) / 2)
        self.keys(f"m:{x},{y}|w:2|c:{x},{y}|w:1|c:{x},{y}|w:4", 1.5)
        self.assertEqual(ws.Range("B3").Value, "Running")
        self.click("F3", 1.5)  # one click stops it
        self.assertEqual(ws.Range("B3").Value, "Stopped")
        self.assertClean()


class TradingDeskTest(ScreenBase):
    EXAMPLE = "trading_desk"
    POINT = 200  # TXF: money per index point

    def setUp(self):
        from yc import excel

        self.ws = excel.ActiveSheet
        col = [r[0] for r in self.ws.Range("G1:G80").Value]
        self.pos_r, self.fills_r = col.index("Position") + 3, col.index("Fills") + 2
        self.click("E4", 1.5)  # flat
        self.click("F4", 0.8)  # nothing working
        self.set_qty(2)

    def set_qty(self, text):
        self.click("B4", 0.3)
        self.keys(f"t:{text}|k:Enter|w:4", 0.5)

    def position(self):
        return self.ws.Range(f"G{self.pos_r}:K{self.pos_r}").Value[0]  # Contract, Qty, Avg, P&L, Realized

    def fills(self):
        rows = self.ws.Range(f"G{self.fills_r + 1}:L{self.fills_r + 60}").Value
        return [r for r in rows if r[1] not in (None, "")]  # Time, Side, Qty, Price, P&L, #

    def test_a_round_trip_books_its_profit(self):
        """A fill that closed the position read the order manager's average after the fill, which is gone once flat:
        the realized profit stayed 0 and the fill's P&L blank."""
        realized0, n0 = self.position()[4] or 0.0, len(self.fills())
        self.click("C4", 1.5)
        self.click("D4", 1.5)
        rows = self.fills()[n0:]
        self.assertEqual([r[1] for r in rows], ["BUY", "SELL"])
        expected = (rows[1][3] - rows[0][3]) * 2 * self.POINT
        self.assertEqual(self.position()[1], 0)
        self.assertAlmostEqual((self.position()[4] or 0.0) - realized0, expected)
        self.assertAlmostEqual(rows[1][4] or 0.0, expected)
        self.assertClean()

    def test_a_double_click_on_buy_sends_one_order(self):
        n0 = len(self.fills())
        from yc import ui

        x0, y0, x1, y1 = ui.cell_rect("C4")
        x, y = (x0 + x1) / 2, (y0 + y1) / 2
        self.keys(f"m:{x:.0f},{y:.0f}|w:2|c:{x:.0f},{y:.0f}|w:1|c:{x:.0f},{y:.0f}|w:4", 2.0)
        self.assertEqual(len(self.fills()) - n0, 1, "one fill for a double-click")
        self.assertEqual(self.position()[1], 2)
        self.assertClean()

    def test_a_quantity_typed_wrong_sends_nothing(self):
        """A typo in the quantity sent one lot."""
        from yc import excel

        for text in ("abc", "0", "1.5"):
            n0 = len(self.fills())
            self.set_qty(text)
            self.click("C4", 1.5)
            self.assertEqual(len(self.fills()), n0, f"qty {text!r}")
            self.assertIn("Order refused", str(excel.Application.StatusBar), text)
        self.set_qty(15)  # above the 10 lots an order
        n0 = len(self.fills())
        self.click("C4", 1.5)
        self.assertEqual(len(self.fills()), n0)
        self.assertIn("above the limit", str(excel.Application.StatusBar))
        self.assertClean()


if __name__ == "__main__":
    unittest.main()
