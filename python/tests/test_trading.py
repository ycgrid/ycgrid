# SPDX-License-Identifier: Apache-2.0
"""yc.trading: risk checks, positions and the kill switch with a fake gateway (no YCGrid needed)."""
import os
import sys
import time
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from yc import trading  # noqa: E402


class FakeGateway(trading.Gateway):
    paper = True

    def __init__(self, ref=100.0):
        super().__init__()
        self.ref = ref
        self.sent = []

    def submit(self, order):
        self.sent.append(order)
        order.status = "Working"
        self.emit_order(order)

    def cancel(self, order_id):
        for o in self.sent:
            if o.id == order_id and o.status == "Working":
                o.status = "Cancelled"
                self.emit_order(o)

    def last_price(self, symbol):
        return self.ref

    def fill(self, order, qty=None, price=None):
        q = qty or order.qty - order.filled
        order.filled += q
        order.status = "Filled" if order.filled >= order.qty else "PartiallyFilled"
        self.emit_fill(trading.Fill(order.id, order.symbol, order.side, q, price or order.price or self.ref))
        self.emit_order(order)


class TradingTest(unittest.TestCase):
    def setUp(self):
        self.gw = FakeGateway()
        self.om = trading.OrderManager(self.gw, risk=trading.RiskLimits(max_qty=5, max_position=8, price_band=0.05),
                                       log=False)

    def test_risk_checks(self):
        with self.assertRaisesRegex(trading.Rejected, "above the limit of 5 per order"):
            self.om.buy("X", 6, 100)
        with self.assertRaisesRegex(trading.Rejected, "away from the reference"):
            self.om.buy("X", 1, 120)
        with self.assertRaisesRegex(trading.Rejected, "positive"):
            self.om.buy("X", 0, 100)
        self.om.buy("X", 5, 100)
        with self.assertRaisesRegex(trading.Rejected, "position in X would reach"):
            self.om.buy("X", 4, 100)  # 5 working + 4 > 8
        self.assertEqual(len(self.gw.sent), 1)
        self.assertEqual([o.status for o in self.om.orders()], ["Working"])

    def test_rate_limit(self):
        om = trading.OrderManager(self.gw, risk=trading.RiskLimits(max_orders_per_sec=2), log=False)
        om.buy("X", 1, 100)
        om.buy("X", 1, 100)
        with self.assertRaisesRegex(trading.Rejected, "orders a second"):
            om.buy("X", 1, 100)

    def test_positions_and_flatten(self):
        a = self.om.buy("X", 3, 100)
        self.gw.fill(self.om._orders[a], price=100)
        b = self.om.buy("X", 2, 102)
        self.gw.fill(self.om._orders[b], price=102)
        self.assertEqual(self.om.position("X"), 5)
        self.assertAlmostEqual(self.om.avg_cost("X"), (300 + 204) / 5)
        c = self.om.sell("X", 2, 101)
        self.gw.fill(self.om._orders[c], price=101)
        self.assertEqual(self.om.position("X"), 3)
        self.assertAlmostEqual(self.om.avg_cost("X"), (300 + 204) / 5)  # selling doesn't change the average
        w = self.om.buy("X", 1, 99)  # stays working
        out = self.om.flatten()
        self.assertEqual(self.om._orders[w].status, "Cancelled")
        self.assertEqual(len(out), 1)
        flat = self.om._orders[out[0]]
        self.assertEqual((flat.side, flat.qty, flat.price, flat.tag), ("sell", 3, None, "flatten"))
        self.gw.fill(flat, price=103)
        self.assertEqual(self.om.position("X"), 0)
        self.assertEqual(self.om.positions(), {})
        self.assertEqual(len(self.om.fills), 4)

    def test_order_and_fill_callbacks(self):
        gw = FakeGateway()
        om = trading.OrderManager(gw, require_operator=False)
        seen, fills = [], []
        om.on_order.append(lambda o: seen.append((o.id, o.status)))
        om.on_fill.append(fills.append)
        om.on_order.append(lambda o: 1 / 0)  # a broken screen doesn't stop the orders
        oid = om.buy("TXF", 2, 100)
        gw.fill(gw.sent[-1])
        self.assertIn((oid, "New"), seen)
        self.assertEqual(seen[-1], (oid, "Filled"))
        self.assertEqual([f.qty for f in fills], [2])
        self.assertEqual(om.position("TXF"), 2)

    def test_paper_gateway(self):
        gw = trading.PaperGateway(spot=23000, step=0.02, latency=0.0)
        try:
            om = trading.OrderManager(gw, log=False)
            oid = om.buy("TXF", 1)  # market: fills at the touch
            for _ in range(100):
                if om.position("TXF") == 1:
                    break
                time.sleep(0.02)
            self.assertEqual(om.position("TXF"), 1)
            self.assertEqual(om._orders[oid].status, "Filled")
        finally:
            gw.close()



def wait_for(pred, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.01)
    return pred()


class FakeMarket:
    """A market that only moves when the test says so (PaperGateway(market=...))."""
    tick = 1.0

    def __init__(self, last=100.0):
        self.last = last
        self.steps = 0

    def step(self, seconds):
        self.steps += 1
        return self.last


class OrderTypesTest(unittest.TestCase):
    """Stops and modify go to gateways that support them; the others refuse them (no emulation)."""

    def om(self, gw=None, **kw):
        gw = gw or FakeGateway()
        kw.setdefault("log", False)
        return gw, trading.OrderManager(gw, **kw)

    def test_unsupported_features_are_refused(self):
        gw, om = self.om()  # FakeGateway: supports nothing
        with self.assertRaisesRegex(trading.Rejected, "doesn't take stop orders"):
            om.submit("X", "sell", 1, type="stop", stop=95)
        oid = om.buy("X", 1, 99)
        with self.assertRaisesRegex(trading.Rejected, "doesn't modify"):
            om.modify(oid, price=98)
        with self.assertRaisesRegex(trading.Rejected, "type is market"):
            om.submit("X", "buy", 1, type="iceberg")
        self.assertEqual(len(gw.sent), 1)

    def test_native_stop_on_paper_gateway(self):
        m = FakeMarket(100)
        gw = trading.PaperGateway(market=m, step=0.01, latency=0.0)
        try:
            _, om = self.om(gw)
            with self.assertRaisesRegex(trading.Rejected, "needs stop="):
                om.submit("TXF", "buy", 1, type="stop")
            with self.assertRaisesRegex(trading.Rejected, "needs a price"):
                om.submit("TXF", "buy", 1, type="stop_limit", stop=105)
            sid = om.submit("TXF", "buy", 1, type="stop", stop=105)
            o = om.order(sid)
            self.assertTrue(wait_for(lambda: o.status == "Pending"))
            self.assertEqual([x.id for x in gw.open_orders()], [sid])
            time.sleep(0.05)
            self.assertEqual(om.position("TXF"), 0)
            m.last = 106
            self.assertTrue(wait_for(lambda: om.position("TXF") == 1))
            self.assertEqual((o.status, len(om.fills)), ("Filled", 1))
            c = om.submit("TXF", "sell", 1, type="stop", stop=90)
            self.assertTrue(wait_for(lambda: om.order(c).status == "Pending"))
            om.cancel(c)
            self.assertTrue(wait_for(lambda: om.order(c).status == "Cancelled"))
        finally:
            gw.close()

    def test_modify_on_paper_gateway(self):
        m = FakeMarket(100)
        gw = trading.PaperGateway(market=m, step=0.01, latency=0.0)
        try:
            _, om = self.om(gw, risk=trading.RiskLimits(price_band=0.1))
            oid = om.buy("TXF", 1, 95)
            with self.assertRaisesRegex(trading.Rejected, "away from the reference"):
                om.modify(oid, price=80)  # the risk checks apply to the new price
            om.modify(oid, price=96)
            self.assertEqual(om.order(oid).price, 96)
            m.last = 96
            self.assertTrue(wait_for(lambda: om.position("TXF") == 1))
            self.assertEqual(om.fills[0].price, 96)
            with self.assertRaisesRegex(trading.Rejected, "isn't working"):
                om.modify(oid, price=97)
        finally:
            gw.close()


class PaperGatewayTest(unittest.TestCase):
    """F10: spread, slippage, partial fills, queue position, speed and pause."""

    def gw(self, last=100.0, **kw):
        m = FakeMarket(last)
        kw.setdefault("step", 0.01)
        kw.setdefault("latency", 0.0)
        gw = trading.PaperGateway(market=m, **kw)
        self.addCleanup(gw.close)
        return m, gw, trading.OrderManager(gw, log=False)

    def test_spread_and_slippage(self):
        _, gw, om = self.gw(spread_ticks=1)
        om.buy("TXF", 1)
        om.sell("TXF", 1)
        self.assertTrue(wait_for(lambda: len(om.fills) == 2))
        self.assertEqual(sorted((f.side, f.price) for f in om.fills), [("buy", 101.0), ("sell", 99.0)])
        _, gw, om = self.gw(slippage_ticks=(2, 2))
        om.buy("TXF", 1)
        self.assertTrue(wait_for(lambda: len(om.fills) == 1))
        self.assertEqual(om.fills[0].price, 102.0)

    def test_marketable_limit_takes_the_market(self):
        m, gw, om = self.gw(last=100.0, spread_ticks=1)
        om.buy("TXF", 1, 105)   # above the ask (101): price improvement
        om.sell("TXF", 1, 95)   # below the bid (99)
        self.assertTrue(wait_for(lambda: len(om.fills) == 2))
        self.assertEqual(sorted((f.side, f.price) for f in om.fills), [("buy", 101.0), ("sell", 99.0)])
        # a limit that rested first fills at its own price, even when the market gaps through it
        oid = om.buy("TXF", 1, 98)
        time.sleep(0.1)
        m.last = 90
        self.assertTrue(wait_for(lambda: om.order(oid).status == "Filled"))
        self.assertEqual(om.fills[-1].price, 98)

    def test_quoted_symbols_trade_at_their_own_prices(self):
        m, gw, om = self.gw(last=23000.0)
        opt = "TXO202611C23000"
        oid = om.buy(opt, 1)   # no quote yet: refused, never matched against the futures price
        self.assertTrue(wait_for(lambda: om.order(oid).status == "Rejected"))
        self.assertIn("no price", om.order(oid).reason)
        gw.quote(opt, 300.0, 302.0)
        self.assertEqual(gw.last_price(opt), 301.0)
        om.buy(opt, 2)          # market: the ask
        om.sell(opt, 1, 290)    # marketable limit: the bid
        rest = om.buy(opt, 1, 295)   # below the ask: rests
        self.assertTrue(wait_for(lambda: len(om.fills) == 2))
        self.assertEqual(sorted((f.side, f.price) for f in om.fills), [("buy", 302.0), ("sell", 300.0)])
        self.assertEqual(om.order(rest).status, "Working")
        gw.quote(opt, 293.0, 295.0)  # the ask comes down to the limit
        self.assertTrue(wait_for(lambda: om.order(rest).status == "Filled"))
        self.assertEqual(om.fills[-1].price, 295.0)
        self.assertEqual(om.position(opt), 2)
        self.assertEqual(om.position("TXF"), 0)

    def test_partial_fills(self):
        _, gw, om = self.gw(fill_ratio=0.5)
        oid = om.buy("TXF", 2)
        self.assertTrue(wait_for(lambda: om.order(oid).status == "Filled"))
        self.assertEqual([f.qty for f in om.fills], [1, 1])

    def test_queue_needs_a_trade_through(self):
        m, gw, om = self.gw(last=101, queue=True)
        oid = om.buy("TXF", 1, 100)
        m.last = 100
        time.sleep(0.1)
        self.assertEqual(om.order(oid).status, "Working")  # touched, not traded through
        m.last = 99
        self.assertTrue(wait_for(lambda: om.order(oid).status == "Filled"))
        self.assertEqual(om.fills[0].price, 100)

    def test_speed_and_pause(self):
        m, gw, om = self.gw(step=0.2)
        self.assertAlmostEqual(gw.step, 0.2)
        gw.set_speed(2)
        self.assertAlmostEqual(gw.step, 0.1)
        gw.set_speed(1)
        gw.base_step = 0.01
        gw.pause()
        time.sleep(0.3)  # the step in progress may finish
        n = m.steps
        time.sleep(0.2)
        self.assertEqual(m.steps, n)  # no ticks while paused
        gw.resume()
        self.assertTrue(wait_for(lambda: m.steps > n))
        self.assertEqual(gw.positions(), {})
        oid = om.buy("TXF", 2)
        self.assertTrue(wait_for(lambda: om.order(oid).status == "Filled"))
        self.assertEqual(gw.positions(), {"TXF": 2})
        self.assertEqual(gw.positions(), om.positions())  # the broker's view and the local one agree


class EcosystemTest(unittest.TestCase):
    """yc.feeds (conflation, publishing, reconnecting) and trading.check_gateway."""

    def test_feed_conflates_by_key_and_reconnects(self):
        import time as _time

        from yc.feeds import CsvTailFeed, Feed

        class Table:
            def __init__(self):
                self.updates = []

            def update(self, cols):
                self.updates.append(cols)

        class Flaky(Feed):
            name = "flaky"
            runs = 0

            def run_source(self):
                Flaky.runs += 1
                if Flaky.runs == 1:
                    raise ConnectionError("dropped")
                self.emit({"Symbol": "TXF", "Bid": 1})
                self.emit({"Symbol": "TXF", "Bid": 2})   # replaces the first before the publish
                self.emit({"Symbol": "MXF", "Bid": 9})
                while not self.stopping:
                    _time.sleep(0.01)

        t = Table()
        f = Flaky(t, key="Symbol", interval=0.05, reconnect=[0.05]).start()
        _time.sleep(0.6)
        f.stop()
        self.assertEqual(f.reconnects, 1)
        self.assertIn("dropped", f.last_error)
        merged = {}
        for u in t.updates:
            for i, sym in enumerate(u["Symbol"]):
                merged[sym] = u["Bid"][i]
        self.assertEqual(merged, {"TXF": 2, "MXF": 9})
        self.assertEqual(sum(len(u["Symbol"]) for u in t.updates), 2)

        # CsvTailFeed: rows appended by another program, a half-written line waits for its end
        d = tempfile.mkdtemp()
        path = os.path.join(d, "q.csv")
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("Symbol,Last\nOLD,1\n")
        got = []
        tail = CsvTailFeed(path, key="Symbol", interval=0.02, on_rows=got.extend, convert=lambda r: {**r, "Last": float(r["Last"])}).start()
        _time.sleep(0.3)
        with open(path, "a", encoding="utf-8", newline="") as fh:
            fh.write("TXF,230")
            fh.flush()
            _time.sleep(0.2)
            fh.write("01\n")
        _time.sleep(0.4)
        tail.stop()
        self.assertEqual(got, [{"Symbol": "TXF", "Last": 23001.0}])  # the old row is skipped (tail from the end)

    def test_gateway_checks(self):
        gw = trading.PaperGateway(step=0.02)
        try:
            report = trading.check_gateway(gw, "TXF", far_price=1000, timeout=10)
        finally:
            gw.close()
        self.assertTrue(all(ok for _, ok, _ in report), report)

        class Live(trading.Gateway):
            paper = False

        with self.assertRaises(trading.Rejected):
            trading.check_gateway(Live(), "TXF", far_price=1)

class RiskWorstCaseTest(unittest.TestCase):
    """The position limit counts each side's resting orders separately: a resting stop on the other side (it may
    never trigger) makes no room for a new order."""

    def test_resting_sell_does_not_make_room_for_a_buy(self):
        gw = FakeGateway()
        om = trading.OrderManager(gw, risk=trading.RiskLimits(max_position=3), log=False)
        gw.fill(gw.sent[-1]) if False else None
        oid = om.buy("TXF", 3)
        gw.fill(om.order(oid))                      # long 3
        self.assertEqual(om.position("TXF"), 3)
        om.sell("TXF", 3, 90)                       # a resting protective sell (the FakeGateway rests every order)
        with self.assertRaisesRegex(trading.Rejected, "would reach 6"):
            om.buy("TXF", 3)                        # 3 + 3 if the sell never fills
        om.sell("TXF", 1, 91)                       # the sell side's worst case is 3 - 3 - 1 = -1: fine
        with self.assertRaisesRegex(trading.Rejected, "would reach -4"):
            om.sell("TXF", 3, 92)                   # 3 - 3 - 1 - 3

    def test_modify_down_to_the_filled_quantity_completes_without_overfilling(self):
        m = FakeMarket(100)
        gw = trading.PaperGateway(market=m, step=0.01, latency=0.0, fill_ratio=0.5)
        try:
            gw.pause()                              # the matching runs only when the test calls it
            om = trading.OrderManager(gw, log=False)
            oid = om.buy("TXF", 4)
            with gw._lock:
                fills = gw._match()
            for o, f in fills:
                gw.emit_fill(f)
                gw.emit_order(o)
            self.assertEqual(om.order(oid).filled, 2)
            om.modify(oid, qty=2)                   # what is filled: nothing more to trade
            with gw._lock:
                fills = gw._match()
            self.assertEqual([f for _, f in fills], [None])
            self.assertEqual(om.order(oid).status, "Filled")
            self.assertEqual(om.order(oid).filled, 2)
            self.assertEqual(om.position("TXF"), 2)
            self.assertEqual(gw.open_orders(), [])
        finally:
            gw.close()
