# SPDX-License-Identifier: Apache-2.0
"""[YC] A skeleton for a broker gateway: copy it into your project (modules/my_broker.py), fill in the calls to your
broker's SDK, and test it with trading.check_gateway on a paper / test account before trading.

    from yc import trading
    from my_broker import MyBrokerGateway
    gw = MyBrokerGateway(account="...")            # read credentials from the environment or a keyring, never a cell
    print(trading.check_gateway(gw, "TXF", far_price=1000))
    om = trading.OrderManager(gw, risk=trading.RiskLimits(max_qty=2, max_position=4, price_band=0.02))
    print(gw.positions(), gw.open_orders())         # compare with your own records after a restart

Rules the OrderManager relies on:
- submit / cancel / modify return quickly; the broker's answers come later through emit_order / emit_fill, from any
  thread. Mutate the Order you were given (status, filled, avg_px, reason) and emit it; emit one Fill per execution.
- Status values: New, Pending (a stop at the broker not yet triggered), Working, PartiallyFilled, Filled, Cancelled,
  Rejected (set order.reason).
- Keep a map between the OrderManager's id (order.id) and the broker's own id.
- `supports` lists what the broker does itself ("stop", "stop_limit", "modify"); the OrderManager refuses the rest.
- positions / open_orders / account / connected answer from the broker, so you can reconcile after a restart.
Nothing here imports an SDK or holds credentials; this module only shows the shape.
"""
from __future__ import annotations

import threading
from typing import Optional

from ..trading import Fill, Gateway, Order


class TemplateGateway(Gateway):
    paper = False                       # True only for a simulator or a paper account
    supports = frozenset({"modify"})    # add "stop" / "stop_limit" when the broker holds stops itself

    def __init__(self, **connect_args):
        super().__init__()
        self._lock = threading.Lock()
        self._by_broker_id: dict[str, Order] = {}
        self._broker_id: dict[str, str] = {}
        self._connected = False
        # self.api = broker_sdk.Client(**connect_args)
        # self.api.on_order_update = self._on_broker_order
        # self.api.on_execution = self._on_broker_fill
        # self.api.connect(); self._connected = True

    # ---- orders ----
    def submit(self, order: Order) -> None:
        # broker_id = self.api.place(symbol=order.symbol, side=order.side, qty=order.qty, price=order.price,
        #                            type=order.type, stop=order.stop, client_id=order.id)
        broker_id = order.id  # replace with the broker's id
        with self._lock:
            self._by_broker_id[broker_id] = order
            self._broker_id[order.id] = broker_id
        order.status = "New"
        self.emit_order(order)

    def cancel(self, order_id: str) -> None:
        with self._lock:
            broker_id = self._broker_id.get(order_id)
        if broker_id is None:
            return
        # self.api.cancel(broker_id)        # the Cancelled status arrives in _on_broker_order

    def modify(self, order: Order, price: Optional[float] = None, qty: Optional[int] = None, stop: Optional[float] = None) -> None:
        with self._lock:
            broker_id = self._broker_id.get(order.id)
        if broker_id is None:
            return
        # self.api.amend(broker_id, price=price, qty=qty, stop=stop)

    def last_price(self, symbol: str) -> Optional[float]:
        # return self.api.quote(symbol).last
        return None

    # ---- the broker's callbacks (any thread) ----
    def _on_broker_order(self, broker_id: str, status: str, filled: int = 0, avg_px: float = 0.0, reason: str = ""):
        with self._lock:
            o = self._by_broker_id.get(broker_id)
        if o is None:
            return
        o.status, o.filled, o.avg_px, o.reason = status, int(filled), float(avg_px), reason
        self.emit_order(o)

    def _on_broker_fill(self, broker_id: str, qty: int, price: float, time: Optional[float] = None):
        with self._lock:
            o = self._by_broker_id.get(broker_id)
        if o is None:
            return
        f = Fill(o.id, o.symbol, o.side, int(qty), float(price))
        if time is not None:
            f.time = float(time)
        self.emit_fill(f)

    # ---- queries (reconcile) ----
    def positions(self) -> dict:
        # return {p.symbol: p.signed_qty for p in self.api.positions()}
        return {}

    def account(self) -> dict:
        # a = self.api.account(); return {"equity": a.equity, "margin": a.margin, "available": a.available, "currency": a.ccy}
        return {}

    def open_orders(self) -> list:
        with self._lock:
            return [o for o in self._by_broker_id.values() if o.status in ("New", "Pending", "Working", "PartiallyFilled")]

    def connected(self) -> bool:
        return self._connected
