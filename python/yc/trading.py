# SPDX-License-Identifier: Apache-2.0
"""[YC] Orders through a gateway, with pre-trade risk checks and positions; the gateway contract and a paper exchange.

    from yc import trading
    gw = trading.PaperGateway(spot=23000, spread_ticks=1, slippage_ticks=(0, 2))   # or your broker: subclass Gateway
    risk = trading.RiskLimits(max_qty=10, max_position=20, price_band=0.02, max_orders_per_sec=5)
    om = trading.OrderManager(gw, risk=risk)
    om.on_order.append(lambda o: ...)                              # every order change: update your blotter table
    om.on_fill.append(lambda f: ...)                               # every fill: update positions, marks on charts
    oid = om.buy("TXF", 2, 23010)                                  # raises trading.Rejected with the reason
    om.submit("TXF", "sell", 1, type="stop", stop=22928)           # when the gateway supports "stop"
    om.modify(oid, price=23005)                                    # when the gateway supports "modify"
    om.cancel(oid); om.cancel_all(); om.flatten()                  # flatten = the kill switch
    om.position("TXF"), om.orders(), om.fills

on_order / on_fill run on the thread that reports the change: the caller of submit / cancel / modify for the
immediate reports, the gateway's thread (PaperGateway's matching thread, a broker SDK's callback thread) for the rest.
Every order, cancel, fill and rejection is written to the change log (Trading > Change Log). Orders to a live
gateway are refused while a replay runs (yc.replay) or outside operator mode, unless the gateway is a paper one.
These are building blocks: brackets, OCO groups, P&L reports, journals and reconciliation are yours to compose from
on_order / on_fill, Gateway.positions / open_orders / account and safety.alarm. A gateway says which order features
it handles (Gateway.supports); the OrderManager refuses the others rather than emulating them. A real broker plugs in
by implementing Gateway.submit / cancel (and the queries) and calling self.emit_order / self.emit_fill from its
callbacks (yc/gateways/template.py); nothing in this module talks to a real market.
"""
from __future__ import annotations

import itertools
import math
import random
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Callable, Optional

__all__ = ["Order", "Fill", "Gateway", "PaperGateway", "RiskLimits", "Rejected", "OrderManager", "check_gateway"]

OPEN = ("New", "Working", "PartiallyFilled", "Pending")  # Pending: a stop waiting for its price (not at the gateway yet)


@dataclass
class Order:
    id: str
    symbol: str
    side: str          # "buy" / "sell"
    qty: int
    price: Optional[float]  # the limit price; None = market
    status: str = "New"     # New, Pending (a stop not triggered), Working, PartiallyFilled, Filled, Cancelled, Rejected
    filled: int = 0
    avg_px: float = 0.0
    tag: str = ""
    time: float = field(default_factory=time.time)
    reason: str = ""
    type: str = ""          # "market", "limit", "stop" (stop-market), "stop_limit"; "" = limit with a price, else market
    stop: Optional[float] = None   # stop orders: the trigger price
    parent: str = ""        # a bracket's exits: the entry order
    oco_group: str = ""     # one fills -> the others of the group are cancelled

    def __post_init__(self):
        if not self.type:
            self.type = "market" if self.price is None else "limit"


@dataclass
class Fill:
    order_id: str
    symbol: str
    side: str
    qty: int
    price: float
    time: float = field(default_factory=time.time)
    fee: float = 0.0  # fees of this fill, when the gateway reports them


class Rejected(Exception):
    """An order refused before it left (risk limits, replay, operator mode); str() is the reason."""


class Gateway:
    """A connection to a market. Subclasses implement submit / cancel and report back with emit_order / emit_fill
    (from any thread); a real broker also answers the queries (positions, account, open_orders, connected) so the
    developer can reconcile after a restart. `paper` says whether orders are simulated; `supports` names the order
    features it handles itself ("stop", "stop_limit", "modify"); the OrderManager refuses the others."""

    paper = False
    supports: frozenset = frozenset()

    def __init__(self):
        self._on_order: list[Callable[[Order], None]] = []
        self._on_fill: list[Callable[[Fill], None]] = []

    def submit(self, order: Order) -> None:  # pragma: no cover - interface
        raise NotImplementedError

    def cancel(self, order_id: str) -> None:  # pragma: no cover - interface
        raise NotImplementedError

    def modify(self, order: Order, price: Optional[float] = None, qty: Optional[int] = None,
               stop: Optional[float] = None) -> None:  # pragma: no cover - interface
        """Change a working order in place (only when "modify" is in supports)."""
        raise NotImplementedError

    def last_price(self, symbol: str) -> Optional[float]:
        """A reference price: the price band, marks (None = unknown)."""
        return None

    # ---- queries a real broker answers (reconcile after a restart) ----
    def positions(self) -> dict:
        """{symbol: signed quantity} at the broker."""
        return {}

    def account(self) -> dict:
        """equity, margin, available, currency..."""
        return {}

    def open_orders(self) -> list:
        """The orders still working at the broker (Order objects; their id is the OrderManager's when known)."""
        return []

    def connected(self) -> bool:
        return True

    def subscribe(self, on_order: Optional[Callable[[Order], None]] = None, on_fill: Optional[Callable[[Fill], None]] = None):
        if on_order:
            self._on_order.append(on_order)
        if on_fill:
            self._on_fill.append(on_fill)

    def emit_order(self, order: Order):
        for f in list(self._on_order):
            f(order)

    def emit_fill(self, fill: Fill):
        for f in list(self._on_fill):
            f(fill)


class PaperGateway(Gateway):
    """A simulated exchange on a yc.sim Market that moves in a background thread. Market orders fill at the ask / bid
    (spread_ticks around the last price) plus a random adverse slippage of slippage_ticks (a number, or (low, high));
    limit orders fill when the price reaches them (queue=True: only when it trades through them) at their limit, or, when
    already marketable on arrival, at the market (a buy limit above the ask fills at the ask); fill_ratio < 1
    fills each order in parts (at least one lot per tick). Stops, stop-limits and modify are native. set_speed,
    pause and resume control the market."""

    paper = True
    supports = frozenset({"stop", "stop_limit", "modify"})

    def __init__(self, spot: float = 23000.0, symbol: str = "TXF", latency: float = 0.05, step: float = 0.2, seed: int = 1,
                 market=None, spread_ticks: float = 0.0, slippage_ticks=0, fill_ratio: float = 1.0, queue: bool = False):
        super().__init__()
        from . import sim

        self.symbol = symbol
        self.market = market or sim.Market(spot=spot, seed=seed)
        self.latency, self.base_step = float(latency), float(step)
        self.spread_ticks, self.fill_ratio, self.queue = float(spread_ticks), max(0.01, min(1.0, float(fill_ratio))), bool(queue)
        lo_hi = slippage_ticks if isinstance(slippage_ticks, (tuple, list)) else (0, slippage_ticks)
        self.slippage_ticks = (float(lo_hi[0]), float(lo_hi[1]))
        self.speed = 1.0
        self._rng = random.Random(seed)
        self._orders: dict[str, Order] = {}   # id -> Order (working or pending stops)
        self._pos: dict[str, int] = {}
        self._quotes: dict[str, tuple] = {}   # other symbols (options, spreads...): (bid, ask, last) from quote()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._paused = threading.Event()
        threading.Thread(target=self._run, daemon=True, name="paper-gateway").start()

    # ---- control ----
    @property
    def step(self) -> float:
        """Seconds between market ticks (base step / speed)."""
        return self.base_step / self.speed

    def set_speed(self, speed: float):
        """The market runs `speed` times as fast (2 = twice as many ticks a second)."""
        self.speed = max(0.01, float(speed))

    def pause(self):
        self._paused.set()

    def resume(self):
        self._paused.clear()

    def close(self):
        self._stop.set()

    def quote(self, symbol: str, bid: float, ask: float, last: Optional[float] = None) -> None:
        """The prices another symbol trades at (an option of the chain, a second contract): its market orders fill at
        the ask / bid, its limits when the ask / bid reaches them. Call it from the loop that shows the quotes; the
        gateway's own symbol follows its simulated market."""
        bid, ask = float(bid), float(ask)
        with self._lock:
            self._quotes[symbol] = (bid, ask, float(last) if last is not None else (bid + ask) / 2)

    # ---- orders ----
    def submit(self, order: Order) -> None:
        if order.symbol != self.symbol and order.symbol not in self._quotes:
            # never match an option against the futures price: the broker would refuse an unknown contract too
            order.status, order.reason = "Rejected", f"no price for {order.symbol}: give the paper gateway its quotes with quote(symbol, bid, ask)"
            self.emit_order(order)
            return
        with self._lock:
            order._sent_at = time.time()  # type: ignore[attr-defined]
            self._orders[order.id] = order
            order.status = "Pending" if order.type in ("stop", "stop_limit") and not getattr(order, "_triggered", False) else "Working"
        self.emit_order(order)

    def cancel(self, order_id: str) -> None:
        with self._lock:
            o = self._orders.pop(order_id, None)
            if o is None or o.status not in OPEN:
                return
            o.status = "Cancelled"
        self.emit_order(o)

    def modify(self, order: Order, price: Optional[float] = None, qty: Optional[int] = None, stop: Optional[float] = None) -> None:
        with self._lock:
            o = self._orders.get(order.id)
            if o is None or o.status not in OPEN:
                raise Rejected(f"{order.id} isn't working")
            if price is not None:
                o.price = float(price)
            if stop is not None:
                o.stop = float(stop)
            if qty is not None:
                if int(qty) < o.filled:
                    raise Rejected(f"{order.id} has {o.filled} filled already")
                o.qty = int(qty)
        self.emit_order(o)

    def last_price(self, symbol: str) -> Optional[float]:
        if symbol != self.symbol and symbol in self._quotes:
            return self._quotes[symbol][2]
        return float(self.market.last) if self.market.last else None

    # ---- queries ----
    def positions(self) -> dict:
        with self._lock:
            return {k: v for k, v in self._pos.items() if v}

    def account(self) -> dict:
        return {"equity": None, "margin": None, "available": None, "currency": "TWD", "paper": True}

    def open_orders(self) -> list:
        with self._lock:
            return [o for o in self._orders.values() if o.status in OPEN]

    # ---- matching ----
    def _tick(self) -> float:
        return float(getattr(self.market, "tick", 1.0) or 1.0)

    def _run(self):
        while not self._stop.is_set():
            if not self._paused.is_set():
                fills = []
                with self._lock:
                    self.market.step(self.base_step)
                    fills = self._match()
                for o, f in fills:
                    if f is not None:
                        self.emit_fill(f)
                    self.emit_order(o)
            time.sleep(self.step)

    def _match(self) -> list:
        """Under the lock: trigger stops, fill what the market reached. Returns [(order, fill)]."""
        last, tick, now = float(self.market.last), self._tick(), time.time()
        half = self.spread_ticks * tick
        out = []
        for o in list(self._orders.values()):
            if o.status not in OPEN or now - getattr(o, "_sent_at", 0) < self.latency:
                continue
            if o.symbol != self.symbol and o.symbol in self._quotes:
                bid, ask, px = self._quotes[o.symbol]   # a quoted symbol trades at its own bid / ask
            else:
                bid, ask, px = last - half, last + half, last
            if o.status == "Pending":  # a stop: triggered when the price reaches it
                if (o.side == "buy" and px >= o.stop) or (o.side == "sell" and px <= o.stop):
                    o._triggered = True  # type: ignore[attr-defined]
                    o.status = "Working"
                    if o.type == "stop":
                        o.price = None  # now a market order
                else:
                    continue
            if o.price is None:  # market: the ask / bid, slipped
                slip = round(self._rng.uniform(*self.slippage_ticks)) * tick
                fpx = ask + slip if o.side == "buy" else bid - slip
            else:
                if o.symbol == self.symbol:  # the simulated market: limits fill when the last price reaches them
                    reached = (px < o.price if o.side == "buy" else px > o.price) if self.queue else (px <= o.price if o.side == "buy" else px >= o.price)
                else:  # a quoted symbol: when the ask (buy) or the bid (sell) reaches the limit
                    reached = (ask < o.price if o.side == "buy" else bid > o.price) if self.queue else (ask <= o.price if o.side == "buy" else bid >= o.price)
                if not reached:
                    o._rested = True  # type: ignore[attr-defined]
                    continue
                fpx = o.price
                if not getattr(o, "_rested", False):  # marketable on arrival: it takes the market, not its own limit
                    fpx = min(o.price, ask) if o.side == "buy" else max(o.price, bid)
            rest = o.qty - o.filled
            if rest <= 0:  # modify() lowered the quantity to what is filled: complete, nothing more to trade
                o.status = "Filled"
                self._orders.pop(o.id, None)
                out.append((o, None))
                continue
            q = rest if self.fill_ratio >= 1 else max(1, min(rest, int(math.floor(o.qty * self.fill_ratio))))
            o.avg_px = (o.avg_px * o.filled + fpx * q) / (o.filled + q)
            o.filled += q
            o.status = "Filled" if o.filled >= o.qty else "PartiallyFilled"
            if o.status == "Filled":
                self._orders.pop(o.id, None)
            self._pos[o.symbol] = self._pos.get(o.symbol, 0) + (q if o.side == "buy" else -q)
            out.append((o, Fill(o.id, o.symbol, o.side, q, fpx, now)))
        return out


def check_gateway(gw: Gateway, symbol: str, far_price: float, timeout: float = 10.0, allow_live: bool = False) -> list:
    """The checks a gateway plugin should pass (run it on a paper or test account): a resting limit order is reported
    Working, cancelling it is reported Cancelled, a market order of 1 is filled with consistent Fill records and the
    order ends Filled; the position is closed again at the end. far_price: a limit price that won't trade (far below
    the market). Returns [(check, ok, detail)]."""
    if not gw.paper and not allow_live:
        raise Rejected("check_gateway sends real orders: use a paper / test account, or pass allow_live=True")
    events: list = []
    fills: list = []
    lock = threading.Lock()

    def on_order(o):
        with lock:
            events.append((o.id, o.status))

    def on_fill(f):
        with lock:
            fills.append(f)

    gw.subscribe(on_order=on_order, on_fill=on_fill)

    def wait(pred) -> bool:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with lock:
                if pred():
                    return True
            time.sleep(0.02)
        return False

    report = []

    def note(name, ok, detail=""):
        report.append((name, bool(ok), detail))

    rest = Order(f"CHK-{int(time.time() * 1000)}-1", symbol, "buy", 1, float(far_price))
    gw.submit(rest)
    note("resting limit order reported Working", wait(lambda: (rest.id, "Working") in events), f"events: {events[-3:]}")
    gw.cancel(rest.id)
    note("cancel reported Cancelled", wait(lambda: (rest.id, "Cancelled") in events), f"events: {events[-3:]}")
    mkt = Order(f"CHK-{int(time.time() * 1000)}-2", symbol, "buy", 1, None)
    gw.submit(mkt)
    got = wait(lambda: sum(f.qty for f in fills if f.order_id == mkt.id) >= 1)
    note("market order filled", got, f"fills: {[(f.qty, f.price) for f in fills if f.order_id == mkt.id]}")
    mine = [f for f in fills if f.order_id == mkt.id]
    note("fills are consistent (symbol, side, qty > 0, price > 0)",
         bool(mine) and all(f.symbol == symbol and f.side == "buy" and f.qty > 0 and f.price > 0 for f in mine))
    note("order ends Filled", wait(lambda: (mkt.id, "Filled") in events), f"events: {[e for e in events if e[0] == mkt.id]}")
    if mine:  # close the position again
        gw.submit(Order(f"CHK-{int(time.time() * 1000)}-3", symbol, "sell", sum(f.qty for f in mine), None))
    return report


@dataclass
class RiskLimits:
    """Pre-trade checks; 0 / None = no limit."""
    max_qty: int = 0                  # per order
    max_position: int = 0             # absolute position per symbol after the order fills
    price_band: float = 0.0           # limit price within this fraction of the reference price
    max_orders_per_sec: float = 0.0   # orders sent per second, all symbols
    max_notional: float = 0.0         # qty x price x multiplier per order
    multiplier: float = 1.0
    symbols: Optional[set] = None     # the symbols allowed (None = any)


def _mark(kind: str, text: str) -> None:
    """A marker on the recording's timeline (the time machine); nothing without a YCGrid connection."""
    try:
        from . import replay

        replay.mark(text, kind)
    except Exception:  # noqa: BLE001 - not connected (tests, offline research)
        pass


def _price_text(o: Order) -> str:
    if o.type == "stop":
        return f"STP {o.stop}"
    if o.type == "stop_limit":
        return f"STP {o.stop} LMT {o.price}"
    return "MKT" if o.price is None else str(o.price)


class OrderManager:
    """Sends orders through a gateway after the risk checks and keeps orders, fills and positions. Screens follow
    through on_order (called with the Order on every change: new, pending, working, filled, cancelled, rejected) and on_fill
    (called with each Fill); the examples bind a blotter and a positions table this way."""

    def __init__(self, gateway: Gateway, risk: Optional[RiskLimits] = None, require_operator: bool = True, log: bool = True):
        self.gw, self.risk = gateway, risk or RiskLimits()
        self.require_operator, self.log_changes = require_operator, log
        self._orders: dict[str, Order] = {}
        self._pos: dict[str, int] = {}
        self._cost: dict[str, float] = {}
        self.fills: list[Fill] = []
        self._sent = deque()
        self._ids = itertools.count(1)
        self._lock = threading.RLock()
        self.on_fill: list[Callable[[Fill], None]] = []
        self.on_order: list[Callable[[Order], None]] = []
        gateway.subscribe(self._order_event, self._fill_event)

    # ---- orders ----
    def buy(self, symbol: str, qty: int, price: Optional[float] = None, tag: str = "", **kw) -> str:
        return self.submit(symbol, "buy", qty, price, tag, **kw)

    def sell(self, symbol: str, qty: int, price: Optional[float] = None, tag: str = "", **kw) -> str:
        return self.submit(symbol, "sell", qty, price, tag, **kw)

    def submit(self, symbol: str, side: str, qty: int, price: Optional[float] = None, tag: str = "", type: Optional[str] = None,  # noqa: A002
               stop: Optional[float] = None) -> str:
        """An order: type "market" (price None), "limit" (price), "stop" (stop=) or "stop_limit" (stop= and price);
        from price when omitted. Stops need a gateway that supports them. Returns its id; raises Rejected."""
        side = side.lower()
        typ = (type or ("limit" if price is not None else "market")).lower()
        if typ not in ("market", "limit", "stop", "stop_limit"):
            raise Rejected(f"type is market, limit, stop or stop_limit, not {typ!r}")
        if typ in ("stop", "stop_limit") and typ not in self.gw.supports:
            raise Rejected(f"the gateway doesn't take {typ} orders (Gateway.supports)")
        if typ in ("stop", "stop_limit") and stop is None:
            raise Rejected(f"a {typ} order needs stop=")
        if typ in ("limit", "stop_limit") and price is None:
            raise Rejected(f"a {typ} order needs a price")
        o = Order(f"O{next(self._ids)}", symbol, side, int(qty), None if typ in ("market", "stop") else float(price), tag=tag,
                  type=typ, stop=None if stop is None else float(stop))
        try:
            self._check(o)
        except Rejected as e:
            o.status, o.reason = "Rejected", str(e)
            self._record(o)
            self._log("order rejected", o, str(e))
            raise
        with self._lock:
            self._orders[o.id] = o
            self._sent.append(time.monotonic())
        self._record(o)
        self._log("order", o, f"{side} {qty} {symbol} @ {_price_text(o)}")
        _mark("order", f"{o.id} {side} {qty} {symbol} @ {_price_text(o)}")
        self.gw.submit(o)
        return o.id

    def modify(self, order_id: str, price: Optional[float] = None, qty: Optional[int] = None, stop: Optional[float] = None) -> None:
        """Change a working order's price, qty or stop at the gateway (it must support "modify"); the risk checks apply
        to the new values."""
        o = self._orders.get(order_id)
        if o is None or o.status not in OPEN:
            raise Rejected(f"{order_id} isn't working")
        if "modify" not in self.gw.supports:
            raise Rejected("the gateway doesn't modify orders (Gateway.supports): cancel and send a new one")
        trial = Order(o.id, o.symbol, o.side, int(qty) if qty is not None else o.qty, o.price if price is None else float(price),
                      type=o.type, stop=o.stop if stop is None else float(stop))
        self._check(trial, replacing=o)
        self._log("modify", o, f"{_price_text(trial)} x {trial.qty}")
        self.gw.modify(o, None if price is None else float(price), None if qty is None else int(qty), None if stop is None else float(stop))

    def cancel(self, order_id: str) -> None:
        o = self._orders.get(order_id)
        if o and o.status in OPEN:
            self._log("cancel", o, "")
            self.gw.cancel(order_id)

    def cancel_all(self, symbol: Optional[str] = None) -> int:
        n = 0
        for o in list(self._orders.values()):
            if o.status in OPEN and (symbol is None or o.symbol == symbol):
                self.cancel(o.id)
                n += 1
        return n

    def flatten(self) -> list[str]:
        """The kill switch: cancel every order and close every position at the market. No risk check applies: it
        must work in every state (operator mode off, a replay running, a symbol no longer allowed)."""
        self.cancel_all()
        out = []
        for sym, q in list(self._pos.items()):
            if q:
                o = Order(f"O{next(self._ids)}", sym, "sell" if q > 0 else "buy", abs(q), None, tag="flatten")
                with self._lock:
                    self._orders[o.id] = o
                self._record(o)
                self._log("flatten", o, f"{o.side} {o.qty} {sym} @ MKT")
                self.gw.submit(o)
                out.append(o.id)
        return out

    kill = flatten

    # ---- state ----
    def order(self, order_id: str) -> Optional[Order]:
        return self._orders.get(order_id)

    def orders(self, open_only: bool = False) -> list[Order]:
        return [o for o in self._orders.values() if not open_only or o.status in OPEN]

    def position(self, symbol: str) -> int:
        return self._pos.get(symbol, 0)

    def positions(self) -> dict[str, int]:
        return {k: v for k, v in self._pos.items() if v}

    def avg_cost(self, symbol: str) -> Optional[float]:
        q = self._pos.get(symbol, 0)
        return self._cost.get(symbol, 0.0) / q if q else None

    # ---- internals ----
    def _check(self, o: Order, replacing: Optional[Order] = None):
        r = self.risk
        if o.side not in ("buy", "sell"):
            raise Rejected(f"side must be buy or sell, not {o.side!r}")
        if o.qty <= 0:
            raise Rejected("the quantity must be positive")
        if r.symbols is not None and o.symbol not in r.symbols:
            raise Rejected(f"{o.symbol} isn't an allowed symbol")
        if r.max_qty and o.qty > r.max_qty:
            raise Rejected(f"quantity {o.qty} is above the limit of {r.max_qty} per order")
        if r.max_position:
            # the worst case on each side: every resting buy fills and no sell does, or the other way round (a resting
            # stop on the other side must not make room for a new order: it may never trigger)
            pos = self._pos.get(o.symbol, 0)
            buys = sells = 0
            for x in self.orders(True):
                if x.symbol == o.symbol and x is not replacing:
                    if x.side == "buy":
                        buys += x.qty - x.filled
                    else:
                        sells += x.qty - x.filled
            if o.side == "buy":
                buys += o.qty
            else:
                sells += o.qty
            worst = pos + buys if abs(pos + buys) >= abs(pos - sells) else pos - sells
            if abs(worst) > r.max_position:
                raise Rejected(f"the position in {o.symbol} would reach {worst}, above the limit of {r.max_position}")
        ref = self.gw.last_price(o.symbol)
        if r.price_band and ref:
            for label, px in (("price", o.price), ("stop", o.stop)):
                if px is not None and abs(px / ref - 1) > r.price_band:
                    raise Rejected(f"{label} {px} is more than {r.price_band:.1%} away from the reference {ref}")
        if r.max_notional and ref:
            notional = o.qty * (o.price or ref) * r.multiplier
            if notional > r.max_notional:
                raise Rejected(f"notional {notional:,.0f} is above the limit of {r.max_notional:,.0f}")
        if r.max_orders_per_sec and replacing is None:
            now = time.monotonic()
            while self._sent and now - self._sent[0] > 1.0:
                self._sent.popleft()
            if len(self._sent) >= r.max_orders_per_sec:
                raise Rejected(f"more than {r.max_orders_per_sec:g} orders a second")
        if not self.gw.paper:
            from . import replay, safety

            if replay.is_replaying():
                raise Rejected("a replay is running: orders to a live gateway are refused")
            if self.require_operator and not safety.operator_mode():
                raise Rejected("operator mode is off (Trading > Operator Mode)")

    def _order_event(self, o: Order):
        self._record(o)

    def _fill_event(self, f: Fill):
        with self._lock:
            s = 1 if f.side == "buy" else -1
            q0 = self._pos.get(f.symbol, 0)
            q1 = q0 + s * f.qty
            c = self._cost.get(f.symbol, 0.0)
            if q0 == 0 or (q0 > 0) == (s > 0):
                c += s * f.qty * f.price
            else:
                avg = c / q0
                c = avg * q1 if q1 and (q1 > 0) == (q0 > 0) else f.price * q1
            self._pos[f.symbol] = q1
            self._cost[f.symbol] = c if q1 else 0.0
            self.fills.append(f)
        self._log("fill", self._orders.get(f.order_id), f"{f.side} {f.qty} {f.symbol} @ {f.price}")
        _mark("fill", f"{f.side} {f.qty} {f.symbol} @ {f.price}")
        for cb in list(self.on_fill):
            try:
                cb(f)
            except Exception:  # noqa: BLE001 - a screen problem must not stop the fills
                pass

    def _record(self, o: Order):
        if o is None:
            return
        for cb in list(self.on_order):
            try:
                cb(o)
            except Exception:  # noqa: BLE001 - a screen problem must not stop the orders
                pass

    def _log(self, kind: str, o: Optional[Order], text: str):
        if not self.log_changes or o is None:
            return
        try:
            from . import safety

            safety.log(kind, o.id, after=text)
        except Exception:  # noqa: BLE001 - not connected (tests)
            pass
