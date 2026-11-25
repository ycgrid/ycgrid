# SPDX-License-Identifier: Apache-2.0
"""yc.plot: what a figure sends to YCGrid (no YCGrid needed; a fake connection records the requests)."""
import datetime as dt
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from yc import plot  # noqa: E402

from tests.test_data import decode  # noqa: E402

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None


class FakeConn:
    def __init__(self):
        self.sent = []

    def request(self, method, params=None):
        self.sent.append((method, dict(params or {}), None))
        return 1 if method == "plot.figure" else None

    def send_request(self, method, params, frame):
        self.sent.append((method, dict(params), decode(frame)))


@unittest.skipIf(np is None, "numpy not installed")
class PlotTest(unittest.TestCase):
    def setUp(self):
        self.fake = FakeConn()
        self._conn, self._channel = plot._conn, plot._channel
        plot._conn = lambda: self.fake
        plot._channel = lambda: self.fake
        plot._figs.clear()

    def tearDown(self):
        plot._conn, plot._channel = self._conn, self._channel
        plot._figs.clear()

    def items(self):
        return [s for s in self.fake.sent if s[0] == "plot.item"]

    def test_signals(self):
        fig = plot.figure("Signals")
        fig[0].signals([1, 2, 3], [100.0, 101.0, 99.0], ["B", "sell", -2], labels=["B 1", "S 1", None])
        method, p, (table, op, _, _, cols) = self.items()[-1]
        self.assertEqual(p["kind"], "signals")
        self.assertEqual(list(cols["side"][-1]), [1.0, -1.0, -1.0])
        self.assertEqual(list(cols["labels"][-1]), ["B 1", "S 1", ""])
        self.assertEqual(p["edge"], 0xFF20B050)  # sell green, buy red by default
        with self.assertRaises(Exception):
            fig[0].signals([1], [1.0], ["hold"])

    def test_synced_cursor(self):
        fig = plot.figure("A", sync_cursor="desk")
        self.assertEqual(self.fake.sent[-1][1]["sync_cursor"], "desk")
        plot.figure("B")
        self.assertNotIn("sync_cursor", self.fake.sent[-1][1])
        fig.cursor(12.5)
        self.assertEqual(self.fake.sent[-1][:2], ("plot.cursor", {"fig": 1, "x": 12.5}))
        fig.cursor(dt.datetime(2026, 1, 2, 9, 0))
        self.assertEqual(self.fake.sent[-1][1]["x"], dt.datetime(2026, 1, 2, 9, 0).timestamp())
        fig.cursor(None)
        self.assertTrue(math.isnan(self.fake.sent[-1][1]["x"]))

    def test_building_blocks_and_events(self):
        fig = plot.figure("Blocks")
        ax = fig[0]
        r = ax.rects([0, 1], [1, 2], [10, 10], [11, 12], colors=["red", "#00ff00"], labels=["a", "b"], edge="white", fmt="%.0f")
        method, p, (table, op, _, _, cols) = self.items()[-1]
        self.assertEqual(p["kind"], "rects")
        self.assertEqual(sorted(cols), ["c", "labels", "x0", "x1", "y0", "y1"])
        self.assertEqual(list(cols["labels"][-1]), ["a", "b"])  # a text column
        self.assertEqual(int(cols["c"][-1][1]), 0xFF00FF00)
        self.assertEqual(p["edge"], 0xFFFFFFFF)
        ax.barh([100, 101], [5, 7], colors=["red", "green"])
        self.assertEqual(self.items()[-1][1]["kind"], "barh")
        ax.errorbar([1, 2], [3, 4], err=[0.5, 0.5])
        self.assertEqual(sorted(self.items()[-1][2][4]), ["neg", "pos", "x", "y"])
        ax.polygon([0, 1, 1, float("nan"), 5, 6, 6], [0, 0, 1, float("nan"), 5, 5, 6])
        ax.arrows([0], [0], [1], [1])
        ax.annotate([1], [2], ["fill 3 @ 23001"])
        self.assertEqual(list(self.items()[-1][2][4]["labels"][-1]), ["fill 3 @ 23001"])
        ln = ax.line([1, 2], [3, 4], tag=True, fmt="%.1f")
        self.assertTrue(self.items()[-1][1]["tag"])
        seen = []
        fig.on("click", seen.append)
        sub = [x for x in self.fake.sent if x[0] == "plot.subscribe"][-1][1]
        self.assertEqual(sub["events"], ["click"])
        self.assertFalse(sub["push"])  # a script: polled
        moved = []
        h = ax.drag_line(23000, label="limit", on_release=moved.append)
        hp = [x for x in self.fake.sent if x[0] == "plot.handle"][-1][1]
        self.assertEqual((hp["kind"], hp["y"], hp["label"]), ("hline", 23000.0, "limit"))
        plot._dispatch({"type": "release", "fig": fig.id, "ax": 0, "handle": h.id, "x": 0.0, "y": 23005.0})
        self.assertEqual(moved[0].value, 23005.0)
        plot._dispatch({"type": "click", "fig": fig.id, "ax": 0, "x": 1.5, "y": 3.2, "item": ln.id, "index": 1})
        self.assertEqual((seen[0].type, seen[0].index), ("click", 1))
        h.set(23010)
        self.assertEqual([x for x in self.fake.sent if x[0] == "plot.handle"][-1][1]["y"], 23010.0)
        plot._poller["figs"].clear()  # stop the script poller of this test

    def test_figure_and_line(self):
        fig = plot.figure("Equity", rows=2, link_x=True, size=(800, 500))
        self.assertEqual(self.fake.sent[0][:2], ("plot.figure", {"title": "Equity", "rows": 2, "cols": 1, "size": [800.0, 500.0],
                                                                  "link_x": True, "theme": "light"}))
        ln = fig[1].line([1, 2, 3], [10.0, 11.5, 9.0], label="pnl", color="red", width=2)
        method, p, (table, op, _, _, cols) = self.items()[0]
        self.assertEqual((p["fig"], p["ax"], p["kind"], p["op"], p["label"], p["width"]), (1, 1, "line", "set", "pnl", 2.0))
        self.assertEqual(p["color"], 0xFFE5383B)  # YCGrid red
        self.assertEqual(cols["x"][1], [1.0, 2.0, 3.0])
        self.assertEqual(cols["y"][1], [10.0, 11.5, 9.0])
        ln.append([4], [12.0])
        self.assertEqual(self.items()[1][1]["op"], "append")
        self.assertEqual(self.items()[1][2][4]["y"][1], [12.0])
        ln.remove()
        self.assertEqual(self.fake.sent[-1][1]["op"], "remove")

    def test_dark_trading_theme(self):
        # the dark trading theme is "dark" (or "trading"); the theme name taken from a trading terminal's brand is gone
        plot.set_theme("dark")
        try:
            plot.figure("Depth")
            self.assertEqual(self.fake.sent[-1][1]["theme"], "dark")
            plot.set_theme("trading")
            with self.assertRaises(plot.YcError):
                plot.set_theme("x" + "q")
            with self.assertRaises(plot.YcError):
                plot.figure("Depth", theme="x" + "q")
        finally:
            plot.set_theme("light")

    def test_y_only_and_series(self):
        fig = plot.figure()
        fig[0].line(np.array([3.0, 4.0]))
        self.assertEqual(self.items()[0][2][4]["x"][1], [0.0, 1.0])
        try:
            import pandas as pd
        except ImportError:
            return
        s = pd.Series([1.0, 2.0], index=[10, 20])
        fig[0].line(s)
        self.assertEqual(self.items()[1][2][4]["x"][1], [10.0, 20.0])

    def test_dates_are_local_wall_clock(self):
        fig = plot.figure()
        when = dt.datetime(2026, 10, 20, 9, 0)
        fig[0].line(np.array([np.datetime64("2026-10-20T09:00")]), [1.0])
        fig[0].line([when], [1.0])
        # the axis becomes a time axis, and numpy and datetime give the same Unix seconds
        self.assertTrue(any(m == "plot.axes" and p.get("time") for m, p, _ in self.fake.sent))
        a = self.items()[0][2][4]["x"][1][0]
        b = self.items()[1][2][4]["x"][1][0]
        self.assertAlmostEqual(a, when.timestamp())
        self.assertAlmostEqual(b, when.timestamp())

    def test_date_scalars(self):
        when = dt.datetime(2026, 10, 20, 9, 0)
        self.assertAlmostEqual(plot._xval(np.datetime64("2026-10-20T09:00")), when.timestamp())
        self.assertAlmostEqual(plot._xval(when), when.timestamp())
        self.assertEqual(plot._xval(np.int64(5)), 5.0)
        try:
            import pandas as pd
        except ImportError:
            return
        self.assertAlmostEqual(plot._xval(pd.Timestamp(when)), when.timestamp())
        fig = plot.figure()
        fig[0].vline([pd.Timestamp(when)])
        self.assertAlmostEqual(self.items()[0][2][4]["values"][1][0], when.timestamp())

    def test_long_data_split_into_frames(self):
        fig = plot.figure()
        n = 2_500_000
        fig[0].line(np.arange(n, dtype="float64"), np.zeros(n))
        ops = [p["op"] for _, p, _ in self.items()]
        self.assertEqual(ops, ["set", "append", "append"])
        self.assertEqual(sum(len(f[4]["x"][1]) for _, _, f in self.items()), n)

    def test_candles_hist_heatmap(self):
        fig = plot.figure()
        fig[0].candles([1, 2], [10, 11], [12, 13], [9, 10], [11, 12])
        self.assertEqual(sorted(self.items()[0][2][4]), ["close", "high", "low", "open", "x"])
        fig[0].hist([1, 2, 2, 3], bins="sqrt")
        self.assertEqual(self.items()[1][1]["bins"], -1)
        fig[0].heatmap([[1, 0.5], [0.5, 1]], xlabels=["a", "b"], ylabels=["a", "b"], cmap="rdbu", vmin=-1, vmax=1)
        p, f = self.items()[2][1], self.items()[2][2]
        self.assertEqual((p["rows"], p["cols"], p["cmap"], p["vmin"]), (2, 2, 11, -1.0))
        self.assertEqual(f[4]["labels"][1], ["a", "b", "a", "b"])
        with self.assertRaises(plot.YcError):
            fig[0].heatmap([1, 2, 3])
        # placed in data coordinates; a datetime extent turns the x axis into a time axis
        hm = plot.figure(rows=1)[0].heatmap(np.zeros((3, 4)), extent=(dt.datetime(2026, 1, 2, 9), dt.datetime(2026, 1, 2, 10), 100, 103))
        p = self.items()[-1][1]
        self.assertEqual(p["extent"][2:], [100.0, 103.0])
        self.assertAlmostEqual(p["extent"][1] - p["extent"][0], 3600.0)
        hm.set_data(np.ones((3, 5)), extent=(0, 5, 100, 103))
        p = self.items()[-1][1]
        self.assertEqual((p["rows"], p["cols"], p["extent"]), (3, 5, [0.0, 5.0, 100.0, 103.0]))
        with self.assertRaises(plot.YcError):
            fig[0].heatmap(np.zeros((2, 2)), extent=(0, 1))
        fig[0].set(ylim="auto")
        self.assertTrue(self.fake.sent[-1][1].get("ylim_auto"))

    def test_clear_errors(self):
        fig = plot.figure()
        with self.assertRaisesRegex(plot.YcError, "x has 3, y has 2"):
            fig[0].line([1, 2, 3], [1, 2])
        with self.assertRaisesRegex(plot.YcError, "one-dimensional"):
            fig[0].line(np.zeros((3, 3)))
        with self.assertRaisesRegex(plot.YcError, "numbers or dates"):
            fig[0].line(["a", "b"], [1, 2])
        with self.assertRaisesRegex(plot.YcError, "Unknown color"):
            fig[0].line([1, 2], [1, 2], color="nocolor")
        with self.assertRaisesRegex(plot.YcError, "shape"):
            fig[0].surface([1, 2], [1, 2, 3], np.zeros((2, 2)))
        fig[0].surface([1, 2], [1, 2, 3], np.zeros((3, 2)))
        p, f = self.items()[-1][1], self.items()[-1][2]
        self.assertEqual((p["kind"], p["rows"], p["cols"]), ("surface", 3, 2))
        self.assertEqual(len(f[4]["z"][1]), 6)

    def test_axes_set_and_link(self):
        fig = plot.figure()
        fig[0].set(title="T", xlim=(0, 10), follow=60, log_y=True)
        m, p, _ = self.fake.sent[-1]
        self.assertEqual(m, "plot.axes")
        self.assertEqual((p["title"], p["xlim"], p["follow"], p["log_y"]), ("T", [0.0, 10.0], 60.0, True))
        fig.link_table("Trades", "Time")
        self.assertEqual(self.fake.sent[-1][:2], ("plot.link", {"fig": 1, "table": "Trades", "column": "Time"}))
        plot.close_all()
        self.assertEqual(self.fake.sent[-1][0], "plot.close")
        self.assertFalse(plot._figs)


if __name__ == "__main__":
    unittest.main()
