# SPDX-License-Identifier: Apache-2.0
"""Pure-Python parts of the package, without a running YCGrid: the paper market and exchange (yc.sim), the project
environment scan (yc.env), the column frame codec (yc.data), the MCP tools' helpers and the command line's parsers."""
import io
import math
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from yc import data, env, sim  # noqa: E402
from yc._conn import YcError  # noqa: E402
from yc.mcp import tools  # noqa: E402


class SimMarketTest(unittest.TestCase):
    def test_market_steps_and_stays_on_the_tick(self):
        m = sim.Market(spot=100.0, vol=0.2, tick=0.5, seed=3)
        last = [m.step(0.25) for _ in range(200)]
        self.assertTrue(all(abs(x / 0.5 - round(x / 0.5)) < 1e-9 for x in last), "prices land on the tick")
        self.assertTrue(all(x > 0 for x in last))
        self.assertEqual(m.last, last[-1])
        m2 = sim.Market(spot=100.0, vol=0.2, tick=0.5, seed=3)
        self.assertEqual([m2.step(0.25) for _ in range(200)], last, "the same seed gives the same path")

    def test_option_chain_prices_are_consistent(self):
        m = sim.Market(spot=23000.0, vol=0.18, seed=1)
        strikes = [22800, 23000, 23200]
        chain = m.option_chain(strikes, days=7.0)
        self.assertEqual(list(chain["Strike"]), strikes)
        for cp in ("C", "P"):
            self.assertTrue(all(chain[f"{cp} Bid"] <= chain[f"{cp} Ask"]), cp)
            self.assertTrue(all(chain[f"{cp} IV"] > 0))
            self.assertEqual(len(chain[f"{cp} OI"]), 3)
        self.assertTrue(all(chain["C Delta"] > 0) and all(chain["P Delta"] < 0))
        self.assertGreater(chain["C Last"][0], chain["C Last"][2], "a lower strike call is worth more")
        ivs = m.iv(strikes, 7.0)
        self.assertEqual(len(list(ivs)), 3)
        self.assertTrue(all(0 < v < 2 for v in ivs))
        book = m.book(levels=5, size=10)
        self.assertEqual(len(book[0]), 5)
        self.assertLess(book[0][0][0], book[1][0][0], "best bid under best ask")

    def test_black_scholes_put_call_parity_and_greeks(self):
        S, K, T, r, vol = 100.0, 100.0, 0.5, 0.01, 0.2
        c = sim._bs_price(S, K, T, r, vol, "C")
        p = sim._bs_price(S, K, T, r, vol, "P")
        self.assertAlmostEqual(c - p, S - K * math.exp(-r * T), places=6)
        g = sim._greeks(S, K, T, r, vol, "C")
        self.assertTrue(0.4 < g["delta"] < 0.7)
        self.assertGreater(g["gamma"], 0)
        self.assertGreater(g["vega"], 0)
        self.assertLess(g["theta"], 0)
        gp = sim._greeks(S, K, T, r, vol, "P")
        self.assertAlmostEqual(g["delta"] - gp["delta"], 1.0, places=6)
        self.assertAlmostEqual(sim._black76_price(S, K, T, r, vol, "C"), sim._bs_price(S, K, T, r, vol, "C", q=r), places=6)
        self.assertAlmostEqual(sim._norm_cdf(0.0), 0.5, places=9)
        self.assertAlmostEqual(sim._norm_cdf(1.96), 0.975, places=3)

    def test_paper_exchange_fills_tracks_positions_and_flattens(self):
        m = sim.Market(spot=100.0, vol=0.0, tick=1.0, seed=1)
        fills, orders = [], []
        x = sim.PaperExchange(m, latency=0.0, on_fill=fills.append, on_order=orders.append, multiplier=1.0)
        oid = x.submit("TXF", "buy", 2)
        self.assertTrue(oid.startswith("P"))
        self.assertEqual(x.poll()[0].qty, 2)
        self.assertEqual(x.position["TXF"], 2)
        self.assertEqual(len(fills), 1)
        lim = x.submit("TXF", "sell", 1, price=m.last + 50)
        self.assertEqual(x.poll(), [], "a limit above the market waits")
        self.assertTrue(x.cancel(lim))
        self.assertFalse(x.cancel(lim), "already cancelled")
        self.assertEqual(x.orders[lim].status, "Cancelled")
        self.assertEqual(x.pnl("TXF", mark=m.last + 3), 6.0)
        closing = x.flatten()
        self.assertEqual(len(closing), 1)
        x.poll()
        self.assertEqual(x.position["TXF"], 0)
        with self.assertRaises(ValueError):
            x.submit("TXF", "hold", 1)
        with self.assertRaises(ValueError):
            x.submit("TXF", "buy", 0)
        self.assertEqual(x.cancel_all(), 0)
        self.assertTrue(all(o.status in ("Filled", "Cancelled") for o in orders))


class EnvScanTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="yc_env_")

    def write(self, rel, text):
        path = os.path.join(self.dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def test_requirements_and_imports_are_read(self):
        self.write("requirements.txt", "numpy>=1.0  # arrays\n-e .\n\npandas==2.0\nsome_pkg[extra]\n")
        self.assertEqual(env._requirement_names(self.dir), ["numpy", "pandas", "some_pkg"])
        self.assertEqual(env._requirement_names(os.path.join(self.dir, "nowhere")), [])
        self.write("main.py", "import os, json\nfrom yc.excel import *\nimport definitely_missing_pkg_xyz\nfrom . import local\n")
        self.write("sheets/Sheet1.py", "import numpy as np\n")
        self.write("broken.py", "def (:\n")
        self.write("runtime/skipped.py", "import never_seen_pkg\n")
        found = env.scan_imports(self.dir)
        self.assertIn("os", found)
        self.assertIn("yc", found)
        self.assertIn("definitely_missing_pkg_xyz", found)
        self.assertIn("numpy", found)
        self.assertNotIn("never_seen_pkg", found, "the runtime folder is skipped")
        missing = env.find_missing(self.dir)
        self.assertIn("definitely-missing-pkg-xyz", missing)
        self.assertNotIn("os", missing)
        self.assertNotIn("yc", missing)

    def test_needed_modules_and_missing_for_error(self):
        self.write("a.py", "import pandas\ntry:\n    import optional_thing\nexcept ImportError:\n    optional_thing = None\n")
        need = env.needed_modules([os.path.join(self.dir, "a.py")], code="import requests\n", roots=[self.dir])
        self.assertIn("pandas", need)
        self.assertIn("requests", need)
        self.assertNotIn("optional_thing", need, "an import guarded by try / except ImportError is optional")
        try:
            import no_such_module_abc  # noqa: F401
        except ImportError as e:
            self.assertIn("no-such-module-abc", env.missing_for_error(e, code="import no_such_module_abc"))
        self.assertEqual(env.package_for_module("sklearn"), "scikit-learn")
        self.assertEqual(env.package_for_module("cv2"), "opencv-python")
        self.assertEqual(env.package_for_module("google.protobuf"), "protobuf")
        self.assertEqual(env.package_for_module("plain_name.sub"), "plain-name")
        self.assertTrue(env._available("json"))
        self.assertFalse(env._available("no_such_module_abc"))

    def test_policy_is_read_from_the_project_toml(self):
        self.assertEqual(env.policy(self.dir), "ask")
        env._set_policy(self.dir, "never")
        self.assertEqual(env.policy(self.dir), "never")
        env._set_policy(self.dir, "always")
        self.assertEqual(env.policy(self.dir), "always")
        with open(os.path.join(self.dir, "ycgrid.toml"), encoding="utf-8") as f:
            self.assertEqual(f.read().count("auto_install"), 1)
        self.write("ycgrid.toml", '[python]\nauto_install = "whatever"\n')
        self.assertEqual(env.policy(self.dir), "ask", "an unknown value falls back to ask")


class FrameCodecTest(unittest.TestCase):
    def test_columns_of_every_type_survive_a_round_trip(self):
        import datetime as dt

        names = ["px", "qty", "ok", "sym", "when"]
        cols = [[1.5, None, 3.25], [1, 2, 3], [True, False, True], ["a", None, "台積電"], [dt.datetime(2026, 1, 2, 12, 30), None, dt.datetime(2026, 1, 3)]]
        frame, dates = data.encode_frame("quotes", 1, names, cols, key_col=3)
        self.assertEqual(dates, [False, False, False, False, True])
        table, out = data.decode_frame(frame, dates)
        self.assertEqual(table, "quotes")
        self.assertEqual(list(out), names)
        px = list(out["px"])
        self.assertEqual(px[0], 1.5)
        self.assertTrue(px[1] is None or px[1] != px[1], "a missing number is None or NaN")
        self.assertEqual(list(out["qty"]), [1, 2, 3])
        self.assertEqual([bool(x) for x in out["ok"]], [True, False, True])
        self.assertEqual(out["sym"], ["a", None, "台積電"])
        self.assertEqual(len(out["when"]), 3)
        with self.assertRaises(YcError):
            data.encode_frame("t", 1, ["a", "b"], [[1, 2], [1]])
        with self.assertRaises(YcError):
            data.decode_frame(b"not a frame at all" * 8)

    def test_an_empty_frame_and_a_frame_without_numpy(self):
        frame, _ = data.encode_frame("empty", 2, [], [])
        self.assertEqual(data.decode_frame(frame)[1], {})
        saved = data._np
        try:
            data._np = lambda: None  # the codec without numpy: plain lists
            frame, _ = data.encode_frame("t", 1, ["n", "s", "b"], [[1.0, None], ["x", "y"], [True, None]])
            _, out = data.decode_frame(frame)
            self.assertEqual(out["n"], [1.0, None])
            self.assertEqual(out["s"], ["x", "y"])
            self.assertEqual(out["b"], [True, None])
        finally:
            data._np = saved


class McpHelpersTest(unittest.TestCase):
    def test_values_patterns_colors_and_keys(self):
        self.assertEqual(tools._fmt_value(None), "")
        self.assertEqual(tools._fmt_value(False), "FALSE")
        self.assertEqual(tools._fmt_value(2.0), "2")
        self.assertEqual(tools._fmt_value(2.5), "2.5")
        self.assertEqual(tools._fmt_value(1e16), "1e+16")
        self.assertEqual(tools._fmt_value("x"), '"x"')
        self.assertEqual(tools._fmt_value(46000.5, date=True), "2025-12-09 12:00:00")
        self.assertEqual(tools._fmt_value(46000.0, date=True), "2025-12-09")
        self.assertEqual(tools._fmt_value(46000.0, date=True, date_mode="serial"), "46000")
        pat = tools._excel_pattern("a*b?c~*", "part", False)
        self.assertTrue(pat.search("AxxByc*"))
        self.assertIsNone(pat.search("abc"))
        whole = tools._excel_pattern("tx?", "whole", True)
        self.assertTrue(whole.search("TX1") is None and whole.search("tx1"))
        self.assertEqual(tools._color("#0A0B0C"), 0x0C0B0A)
        self.assertIsNone(tools._color(None))
        self.assertEqual(tools._key_token("Ctrl+Alt+Delete"), "k:c-a-Delete")
        self.assertEqual(tools._key_token("pgdn"), "k:PageDown")
        self.assertEqual(tools._key_token("F2"), "k:F2")
        self.assertEqual(tools._key_token("a"), "k:A")
        self.assertEqual(tools._addr((2, 1, 2, 1)), "B3")
        self.assertEqual(tools._table([[1, "a|b"]], 0, 0), "|  | A | B |\n|---|---|---|\n| 1 | 1 | a\\|b |")
        self.assertEqual(tools._plain(float("nan")), None)
        import datetime as dt

        self.assertEqual(tools._plain(dt.date(2026, 1, 2)), "2026-01-02")
        self.assertEqual(tools._clip_used({"used": [0, 0, 9, 2]}, (0, 0, 0xFFFFFFFF, 1)), (0, 0, 9, 1))
        self.assertEqual(tools._clip_used({"used": [0, 0, 9, 2]}, (3, 0, 5, 16383)), (3, 0, 5, 2))
        self.assertEqual(tools._clip_used({}, (3, 0, 0xFFFFFFFF, 16383)), (3, 0, 3, 0))

    def test_control_props_are_translated(self):
        p = tools._ctl_props(caption="Go", placement="move", font={"name": "Arial", "size": 9, "bold": True}, back_color="#FF0000",
                             fore_color="#00FF00", word_wrap=True, list_fill_range="A1:A3", items=["a"], linked_cell="B1", style="dropdown_list",
                             enabled=False, visible=True, value=3)
        self.assertEqual(p["placement"], 2)
        self.assertEqual(p["font_name"], "Arial")
        self.assertEqual(p["back_color"], 0xFF)
        self.assertEqual(p["fore_color"], 0xFF00)
        self.assertEqual(p["style"], 1)
        self.assertFalse(p["enabled"])
        self.assertEqual(p["value"], 3)
        self.assertEqual(tools._extra_ctl_props(min=0, max=9, multi_select="extended")["multi_select"], 2)
        self.assertEqual(tools._ctl_props(), {})
        with self.assertRaises(ToolErrorType):
            tools._chart_type("xlHologram")
        self.assertIsInstance(tools._chart_type("xlLine"), int)
        self.assertEqual(tools._handler_exists("", "Sheet1", "btn_Click"), False)
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "sheets"))
        with open(os.path.join(d, "sheets", "Sheet1.py"), "w", encoding="utf-8") as f:
            f.write("def btn_Click():\n    pass\n")
        self.assertTrue(tools._handler_exists(d, "Sheet1", "btn_Click"))
        self.assertFalse(tools._handler_exists(d, "Sheet1", "other_Click"))
        errs = tools._compile_check(d)
        self.assertEqual(errs, [])
        with open(os.path.join(d, "bad.py"), "w", encoding="utf-8") as f:
            f.write("def (:\n")
        self.assertEqual(tools._compile_check(d)[0]["file"], "bad.py")


ToolErrorType = tools.ToolError


class CliParserTest(unittest.TestCase):
    def test_help_lists_every_command(self):
        from yc import cli

        buf = io.StringIO()
        with redirect_stdout(buf), self.assertRaises(SystemExit):
            cli.main(["--help"])
        text = buf.getvalue()
        for cmd in ("open", "run", "init", "devenv", "pack", "python"):
            self.assertIn(cmd, text)

    def test_init_makes_a_project_and_pack_zips_it(self):
        from yc import cli

        d = os.path.join(tempfile.mkdtemp(), "Proj")
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(cli.main(["init", d]), 0)
        self.assertTrue(os.path.isdir(d))
        files = os.listdir(d)
        self.assertTrue(any(f.endswith(".py") for f in files), files)
        with redirect_stdout(buf):
            self.assertEqual(cli.main(["pack", d, "--no-test"]), 0)
        zips = [f for f in os.listdir(os.path.join(d, "dist")) if f.endswith(".zip")]
        self.assertEqual(len(zips), 1, zips)
        import zipfile

        with zipfile.ZipFile(os.path.join(d, "dist", zips[0])) as z:
            names = z.namelist()
        self.assertTrue(any(n.endswith("main.py") for n in names), names)
        self.assertFalse(any("dist/" in n for n in names), "the zip doesn't pack itself")


if __name__ == "__main__":
    unittest.main()
