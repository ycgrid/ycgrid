# SPDX-License-Identifier: Apache-2.0
"""[YC] A market simulator and a paper exchange, for examples, demos, tests and the benchmark.

    from yc import sim
    mkt = sim.Market(spot=23000, vol=0.18, seed=1)
    mkt.step(0.25)                                   # move 0.25 s of market time
    chain = mkt.option_chain(strikes, days=7)        # dict of numpy columns: strike, C Bid ... P OI (a smile, Greeks)
    book = mkt.book(levels=10)                       # (bids, asks) of (price, size)

    ex = sim.PaperExchange(mkt, latency=0.02, on_fill=lambda f: ...)
    oid = ex.submit("TXF", "buy", 2, price=23010)    # limit; price=None = market
    ex.cancel(oid); ex.poll()                        # poll() fills orders the market crossed

Nothing here connects to a real market: it is safe for demos and tests.
"""
from __future__ import annotations

import itertools
import math
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

import numpy as np


__all__ = ["Market", "PaperExchange", "Order", "Fill"]


class Market:
    """A price following geometric Brownian motion with occasional jumps, quoted with a tick and a spread."""

    def __init__(self, spot: float = 23000.0, vol: float = 0.18, tick: float = 1.0, rate: float = 0.015, seed: Optional[int] = None,
                 skew: float = -0.25, smile: float = 0.6):
        self.spot, self.vol, self.tick, self.rate = float(spot), float(vol), float(tick), float(rate)
        self.skew, self.smile = skew, smile
        self.rng = np.random.default_rng(seed)
        self.t = 0.0
        self.volume = 0
        self.last = self.spot

    def step(self, seconds: float = 0.25) -> float:
        dt = seconds / (252 * 6.5 * 3600)
        z = self.rng.standard_normal()
        jump = self.rng.standard_normal() * 0.002 if self.rng.random() < 0.002 else 0.0
        self.spot *= math.exp((-0.5 * self.vol ** 2) * dt + self.vol * math.sqrt(dt) * z + jump)
        self.t += seconds
        self.last = round(self.spot / self.tick) * self.tick
        self.volume += int(self.rng.integers(1, 30))
        return self.last

    def iv(self, strikes, days: float):
        """Implied volatility smile: skewed and curved in log-moneyness, flatter for longer maturities."""
        k = np.log(np.asarray(strikes, dtype=float) / self.spot)
        T = max(days, 0.5) / 365
        scale = 1.0 / math.sqrt(T * 12)
        return np.maximum(0.05, self.vol + self.skew * k * scale + self.smile * k * k * scale * scale)

    def option_chain(self, strikes, days: float = 7.0, spread_ticks: float = 1.0, opt_tick: float = 0.1) -> dict:
        """Quotes of calls and puts: C/P Bid, Ask, Last, IV, Delta, Gamma, Vega, Theta, Vol, OI (numpy arrays)."""
        K = np.asarray(list(strikes), dtype=float)
        T = max(days, 0.01) / 365
        iv = self.iv(K, days)
        out = {"Strike": K}
        for cp in ("C", "P"):
            px = _bs_price(self.spot, K, T, self.rate, iv, cp)
            g = _greeks(self.spot, K, T, self.rate, iv, cp)
            half = np.maximum(opt_tick, np.round(px * 0.004 / opt_tick) * opt_tick) * spread_ticks
            bid = np.maximum(0.0, np.round((px - half) / opt_tick) * opt_tick)
            ask = np.round((px + half) / opt_tick) * opt_tick
            last = np.round((px + self.rng.uniform(-1, 1, len(K)) * half) / opt_tick) * opt_tick
            out[f"{cp} Bid"], out[f"{cp} Ask"], out[f"{cp} Last"] = bid, ask, np.maximum(last, 0.0)
            out[f"{cp} IV"] = iv
            out[f"{cp} Delta"], out[f"{cp} Gamma"], out[f"{cp} Vega"], out[f"{cp} Theta"] = g["delta"], g["gamma"], g["vega"], g["theta"]
            out[f"{cp} Vol"] = self.rng.integers(0, 5000, len(K))
            out[f"{cp} OI"] = (20000 * np.exp(-((K - self.spot) / (self.spot * 0.03)) ** 2)).astype(np.int64) + self.rng.integers(0, 500, len(K))
        return out

    def book(self, levels: int = 10, size: int = 30):
        """(bids, asks): price / size pairs around the last price."""
        mid = self.last
        bids = [(mid - (i + 1) * self.tick, int(self.rng.integers(1, size) * (1 + i * 0.3))) for i in range(levels)]
        asks = [(mid + i * self.tick, int(self.rng.integers(1, size) * (1 + i * 0.3))) for i in range(levels)]
        return bids, asks



# ---- Black-Scholes for the simulated option chain ----
_SQRT2PI = math.sqrt(2 * math.pi)


def _norm_pdf(x):
    x = np.asarray(x, dtype=float)
    return np.exp(-0.5 * x * x) / _SQRT2PI


def _norm_cdf(x):
    """Standard normal CDF (W. J. Cody's rational approximation through erfc; |error| < 1e-14)."""
    x = np.asarray(x, dtype=float)
    return 0.5 * _erfc(-x / math.sqrt(2.0))


def _erfc(x):
    # erfc for arrays: math.erfc per element is exact; vectorize for small inputs, numpy polynomial for large ones
    x = np.asarray(x, dtype=float)
    if x.size <= 4096:
        return np.vectorize(math.erfc, otypes=[float])(x) if x.ndim else np.float64(math.erfc(float(x)))
    z = np.abs(x)
    t = 1.0 / (1.0 + 0.5 * z)
    # Numerical Recipes erfcc: fractional error < 1.2e-7
    r = t * np.exp(-z * z - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 + t * (-0.18628806 + t * (
        0.27886807 + t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))))
    return np.where(x >= 0, r, 2.0 - r)


def _cp_sign(cp):
    cp = np.asarray(cp)
    if cp.dtype.kind in "US":
        return np.where(np.char.upper(cp.astype(str)) == "C", 1.0, -1.0)
    return np.where(cp.astype(float) > 0, 1.0, -1.0)


def _d1d2(S, K, T, r, vol, q):
    S, K, T, vol = (np.asarray(a, dtype=float) for a in (S, K, T, vol))
    sT = vol * np.sqrt(np.maximum(T, 1e-12))
    d1 = (np.log(S / K) + (r - q + 0.5 * vol * vol) * T) / sT
    return d1, d1 - sT


def _bs_price(S, K, T, r, vol, cp="C", q=0.0):
    """Black-Scholes price. cp: "C" / "P" (or +1 / -1), arrays broadcast; T in years."""
    S, K, T = (np.asarray(a, dtype=float) for a in (S, K, T))
    w = _cp_sign(cp)
    d1, d2 = _d1d2(S, K, T, r, vol, q)
    px = w * (S * np.exp(-q * T) * _norm_cdf(w * d1) - K * np.exp(-r * T) * _norm_cdf(w * d2))
    intrinsic = np.maximum(w * (S - K), 0.0)
    return np.where(T <= 0, intrinsic, px)


def _black76_price(F, K, T, r, vol, cp="C"):
    return _bs_price(F, K, T, r, vol, cp, q=r)


def _greeks(S, K, T, r, vol, cp="C", q=0.0) -> dict:
    """delta, gamma, vega (per 1 volatility point), theta (per calendar day), rho (per 1%)."""
    S, K, T, vol = (np.asarray(a, dtype=float) for a in (S, K, T, vol))
    w = _cp_sign(cp)
    d1, d2 = _d1d2(S, K, T, r, vol, q)
    eq, er = np.exp(-q * T), np.exp(-r * T)
    pdf = _norm_pdf(d1)
    sq = np.sqrt(np.maximum(T, 1e-12))
    delta = w * eq * _norm_cdf(w * d1)
    gamma = eq * pdf / (S * vol * sq)
    vega = S * eq * pdf * sq / 100.0
    theta = (-S * eq * pdf * vol / (2 * sq) - w * r * K * er * _norm_cdf(w * d2) + w * q * S * eq * _norm_cdf(w * d1)) / 365.0
    rho = w * K * T * er * _norm_cdf(w * d2) / 100.0
    return {"delta": delta, "gamma": gamma, "vega": vega, "theta": theta, "rho": rho}

@dataclass
class Order:
    id: str
    symbol: str
    side: str
    qty: int
    price: Optional[float]
    status: str = "Working"
    filled: int = 0
    avg_px: float = 0.0
    time: float = field(default_factory=time.time)


@dataclass
class Fill:
    order_id: str
    symbol: str
    side: str
    qty: int
    price: float
    time: float


class PaperExchange:
    """Simulated matching against a Market: market orders fill at the touch after `latency`, limit orders when the
    price trades through them. Tracks positions and realized + unrealized P&L per symbol."""

    def __init__(self, market: Market, latency: float = 0.02, on_fill: Optional[Callable[[Fill], None]] = None,
                 on_order: Optional[Callable[[Order], None]] = None, fee: float = 0.0, multiplier: float = 200.0):
        self.mkt, self.latency, self.fee, self.multiplier = market, latency, fee, multiplier
        self.on_fill, self.on_order = on_fill, on_order
        self.orders: dict[str, Order] = {}
        self.position: dict[str, int] = {}
        self.cost: dict[str, float] = {}
        self.realized: dict[str, float] = {}
        self._ids = itertools.count(1)

    def submit(self, symbol: str, side: str, qty: int, price: Optional[float] = None) -> str:
        side = side.lower()
        if side not in ("buy", "sell") or qty <= 0:
            raise ValueError("side must be buy / sell and qty > 0")
        o = Order(f"P{next(self._ids)}", symbol, side, int(qty), price)
        self.orders[o.id] = o
        if self.on_order:
            self.on_order(o)
        return o.id

    def cancel(self, order_id: str) -> bool:
        o = self.orders.get(order_id)
        if not o or o.status != "Working":
            return False
        o.status = "Cancelled"
        if self.on_order:
            self.on_order(o)
        return True

    def cancel_all(self) -> int:
        n = 0
        for oid, o in list(self.orders.items()):
            n += self.cancel(oid)
        return n

    def poll(self) -> list[Fill]:
        """Fill what the market crossed; call it after Market.step()."""
        fills = []
        now = time.time()
        px = self.mkt.last
        for o in self.orders.values():
            if o.status != "Working" or now - o.time < self.latency:
                continue
            crossed = o.price is None or (o.side == "buy" and px <= o.price) or (o.side == "sell" and px >= o.price)
            if not crossed:
                continue
            fpx = px if o.price is None else o.price
            f = Fill(o.id, o.symbol, o.side, o.qty - o.filled, fpx, now)
            o.avg_px, o.filled, o.status = fpx, o.qty, "Filled"
            self._book(f)
            fills.append(f)
            if self.on_fill:
                self.on_fill(f)
            if self.on_order:
                self.on_order(o)
        return fills

    def flatten(self) -> list[str]:
        """Cancel everything and send market orders that close every position (a kill switch)."""
        self.cancel_all()
        out = []
        for sym, q in self.position.items():
            if q:
                out.append(self.submit(sym, "sell" if q > 0 else "buy", abs(q)))
        return out

    def _book(self, f: Fill):
        s = 1 if f.side == "buy" else -1
        q0 = self.position.get(f.symbol, 0)
        q1 = q0 + s * f.qty
        c = self.cost.get(f.symbol, 0.0)
        if q0 == 0 or (q0 > 0) == (s > 0):
            c += s * f.qty * f.price
        else:  # closing: realize against the average cost
            avg = c / q0
            closed = min(abs(q0), f.qty)
            self.realized[f.symbol] = self.realized.get(f.symbol, 0.0) + closed * (f.price - avg) * (1 if q0 > 0 else -1) * self.multiplier
            c = avg * q1 if abs(q1) <= abs(q0) and (q1 == 0 or (q1 > 0) == (q0 > 0)) else f.price * q1
        self.position[f.symbol] = q1
        self.cost[f.symbol] = c if q1 else 0.0

    def pnl(self, symbol: str, mark: Optional[float] = None) -> float:
        q = self.position.get(symbol, 0)
        mark = self.mkt.last if mark is None else mark
        unreal = (mark - self.cost.get(symbol, 0.0) / q) * q * self.multiplier if q else 0.0
        return self.realized.get(symbol, 0.0) + unreal
