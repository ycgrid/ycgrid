# SPDX-License-Identifier: Apache-2.0
"""Research maths the examples use, in plain numpy and pandas: example code to copy and change, not a YCGrid API.

YCGrid builds in the pieces only YCGrid has (live tables, fast plots, events, replay); the maths are a few lines of numpy
or a package of your choice (statsmodels, scipy, pandas-ta, vectorbt...). These are the ones the examples need:

    bs_price(S, K, T, r, vol, "C"), greeks(...), implied_vol(price, S, K, T, r, "C")   # Black-Scholes, vectorized
    svi(k, params), svi_fit(k, total_variance)                                        # a raw SVI smile
    sma(close, n), ema(close, n), rsi(close, n)                                       # pandas in, pandas out
    perf_stats(equity), drawdown(equity), monthly_returns(equity), trade_stats(pnl)   # backtest statistics
"""
from __future__ import annotations

import math

import numpy as np

_SQRT2PI = math.sqrt(2 * math.pi)


def norm_pdf(x):
    x = np.asarray(x, dtype=float)
    return np.exp(-0.5 * x * x) / _SQRT2PI


def norm_cdf(x):
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


def bs_price(S, K, T, r, vol, cp="C", q=0.0):
    """Black-Scholes price. cp: "C" / "P" (or +1 / -1), arrays broadcast; T in years."""
    S, K, T = (np.asarray(a, dtype=float) for a in (S, K, T))
    w = _cp_sign(cp)
    d1, d2 = _d1d2(S, K, T, r, vol, q)
    px = w * (S * np.exp(-q * T) * norm_cdf(w * d1) - K * np.exp(-r * T) * norm_cdf(w * d2))
    intrinsic = np.maximum(w * (S - K), 0.0)
    return np.where(T <= 0, intrinsic, px)


def black76_price(F, K, T, r, vol, cp="C"):
    return bs_price(F, K, T, r, vol, cp, q=r)


def greeks(S, K, T, r, vol, cp="C", q=0.0) -> dict:
    """delta, gamma, vega (per 1 volatility point), theta (per calendar day), rho (per 1%)."""
    S, K, T, vol = (np.asarray(a, dtype=float) for a in (S, K, T, vol))
    w = _cp_sign(cp)
    d1, d2 = _d1d2(S, K, T, r, vol, q)
    eq, er = np.exp(-q * T), np.exp(-r * T)
    pdf = norm_pdf(d1)
    sq = np.sqrt(np.maximum(T, 1e-12))
    delta = w * eq * norm_cdf(w * d1)
    gamma = eq * pdf / (S * vol * sq)
    vega = S * eq * pdf * sq / 100.0
    theta = (-S * eq * pdf * vol / (2 * sq) - w * r * K * er * norm_cdf(w * d2) + w * q * S * eq * norm_cdf(w * d1)) / 365.0
    rho = w * K * T * er * norm_cdf(w * d2) / 100.0
    return {"delta": delta, "gamma": gamma, "vega": vega, "theta": theta, "rho": rho}


def implied_vol(price, S, K, T, r, cp="C", q=0.0, lo=1e-4, hi=5.0, tol=1e-8, iters=60):
    """Implied volatility (vectorized): Newton steps kept inside a bisection bracket; NaN when the price is outside
    the no-arbitrage bounds."""
    price, S, K, T = (np.asarray(a, dtype=float) for a in (price, S, K, T))
    shape = np.broadcast(price, S, K, T).shape
    price, S, K, T = (np.broadcast_to(a, shape).astype(float) for a in (price, S, K, T))
    w = np.broadcast_to(_cp_sign(cp), shape)
    lower = np.maximum(w * (S * np.exp(-q * T) - K * np.exp(-r * T)), 0.0)
    upper = np.where(w > 0, S * np.exp(-q * T), K * np.exp(-r * T))
    ok = (price > lower) & (price < upper) & (T > 0)
    a = np.full(shape, lo)
    b = np.full(shape, hi)
    x = np.full(shape, 0.3)
    for _ in range(iters):
        p = bs_price(S, K, T, r, x, w, q)
        diff = p - price
        a = np.where(diff < 0, x, a)
        b = np.where(diff > 0, x, b)
        v = greeks(S, K, T, r, x, w, q)["vega"] * 100.0
        step = np.where(v > 1e-12, diff / np.where(v > 1e-12, v, 1.0), 0.0)
        nx = x - step
        nx = np.where((nx <= a) | (nx >= b) | (v <= 1e-12), 0.5 * (a + b), nx)
        if np.all(np.abs(nx - x)[ok] < tol) if ok.any() else True:
            x = nx
            break
        x = nx
    return np.where(ok, x, np.nan)


def svi(k, params):
    """Raw SVI total variance w(k) = a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2)); k = log(K / F)."""
    a, b, rho, m, s = params
    k = np.asarray(k, dtype=float)
    return a + b * (rho * (k - m) + np.sqrt((k - m) ** 2 + s * s))


def svi_fit(k, w, weights=None, iters=200):
    """Fit raw SVI to total variances w at log-moneyness k (Levenberg-Marquardt). Returns (a, b, rho, m, sigma)."""
    k = np.asarray(k, dtype=float)
    w = np.asarray(w, dtype=float)
    mask = np.isfinite(k) & np.isfinite(w)
    k, w = k[mask], w[mask]
    wt = np.ones_like(w) if weights is None else np.asarray(weights, dtype=float)[mask]
    p = np.array([max(1e-6, w.min() * 0.9), 0.1, -0.3, 0.0, 0.1])
    lam = 1e-3

    def resid(q):
        return (svi(k, q) - w) * wt

    def clamp(q):
        q = q.copy()
        q[1] = max(q[1], 1e-6)
        q[2] = min(max(q[2], -0.999), 0.999)
        q[4] = max(q[4], 1e-4)
        return q

    r = resid(p)
    for _ in range(iters):
        J = np.empty((len(k), 5))
        for j in range(5):
            h = 1e-6 * max(1.0, abs(p[j]))
            dp = p.copy()
            dp[j] += h
            J[:, j] = (resid(clamp(dp)) - r) / h
        A = J.T @ J
        g = J.T @ r
        step = np.linalg.solve(A + lam * np.diag(np.diag(A) + 1e-12), -g)
        q = clamp(p + step)
        rq = resid(q)
        if rq @ rq < r @ r:
            p, r, lam = q, rq, lam * 0.3
            if np.linalg.norm(step) < 1e-10:
                break
        else:
            lam *= 10
            if lam > 1e10:
                break
    return tuple(float(x) for x in p)


# ---------------------------------------------------------------------------
# Backtest statistics
# ---------------------------------------------------------------------------
def _values(equity):
    v = getattr(equity, "to_numpy", None)
    return np.asarray(v() if v else equity, dtype="float64")


def drawdown(equity):
    """Drawdown from the running peak (0 at a new high, -0.25 = 25% below it); a pandas Series stays a Series."""
    e = _values(equity)
    peak = np.maximum.accumulate(np.where(np.isfinite(e), e, -np.inf))
    dd = np.where(peak > 0, e / peak - 1, 0.0)
    if hasattr(equity, "index") and type(equity).__module__.split(".")[0] == "pandas":
        import pandas as pd

        return pd.Series(dd, index=equity.index, name="drawdown")
    return dd


def trade_stats(pnl) -> dict:
    """Statistics of closed trades' profits: count, win rate, profit factor, average trade / win / loss, best, worst."""
    p = np.asarray([x for x in _values(pnl) if np.isfinite(x)], dtype="float64")
    wins, losses = p[p > 0], p[p < 0]
    return {
        "Trades": int(p.size),
        "Win Rate": float(wins.size / p.size) if p.size else math.nan,
        "Profit Factor": float(wins.sum() / -losses.sum()) if losses.size else (math.inf if wins.size else math.nan),
        "Avg Trade": float(p.mean()) if p.size else math.nan,
        "Avg Win": float(wins.mean()) if wins.size else math.nan,
        "Avg Loss": float(losses.mean()) if losses.size else math.nan,
        "Best Trade": float(p.max()) if p.size else math.nan,
        "Worst Trade": float(p.min()) if p.size else math.nan,
    }


def perf_stats(equity, periods_per_year: float = 252, rf: float = 0.0, trades=None) -> dict:
    """Performance of an equity curve (account values, one per period): total return, CAGR, annual volatility,
    Sharpe and Sortino (rf = annual risk-free rate), max drawdown and its length in periods, Calmar, best / worst
    period; with trades (closed trades' profits) also trade_stats()."""
    e = _values(equity)
    e = e[np.isfinite(e)]
    out: dict = {}
    if e.size < 2 or e[0] <= 0:
        return out
    r = e[1:] / e[:-1] - 1
    years = (e.size - 1) / periods_per_year
    if hasattr(equity, "index") and getattr(equity.index, "dtype", None) is not None and equity.index.dtype.kind == "M":
        days = (equity.index[-1] - equity.index[0]).days
        out["Start"], out["End"] = equity.index[0].to_pydatetime(), equity.index[-1].to_pydatetime()
        years = days / 365.25 if days > 0 else years
    total = e[-1] / e[0] - 1
    out["Total Return"] = float(total)
    out["CAGR"] = float((e[-1] / e[0]) ** (1 / years) - 1) if years > 0 and e[-1] > 0 else (-1.0 if years > 0 else math.nan)
    vol = float(r.std(ddof=1) * math.sqrt(periods_per_year)) if r.size > 1 else math.nan
    out["Annual Volatility"] = vol
    ex = r - rf / periods_per_year
    out["Sharpe"] = float(ex.mean() / r.std(ddof=1) * math.sqrt(periods_per_year)) if r.size > 1 and r.std(ddof=1) > 0 else math.nan
    down = np.minimum(ex, 0)
    dd_dev = math.sqrt((down ** 2).mean()) if r.size else 0
    out["Sortino"] = float(ex.mean() / dd_dev * math.sqrt(periods_per_year)) if dd_dev > 0 else math.nan
    dd = drawdown(e)
    out["Max Drawdown"] = float(dd.min())
    longest, run = 0, 0
    for x in dd:
        run = run + 1 if x < 0 else 0
        longest = max(longest, run)
    out["Max Drawdown Length"] = int(longest)
    out["Calmar"] = float(out["CAGR"] / -out["Max Drawdown"]) if out["Max Drawdown"] < 0 and math.isfinite(out["CAGR"]) else math.nan
    out["Best Period"] = float(r.max())
    out["Worst Period"] = float(r.min())
    if trades is not None:
        out.update(trade_stats(trades))
    return out


def monthly_returns(equity):
    """Returns by year (rows) and month (columns 1-12) plus "Year", from a pandas Series of equity with a date index."""
    import pandas as pd

    e = equity.dropna()
    month_end = e.groupby([e.index.year, e.index.month]).last()
    prev = month_end.shift(1)
    prev.iloc[0] = e.iloc[0]
    rets = (month_end / prev - 1).unstack()
    rets = rets.reindex(columns=range(1, 13))
    year_end = e.groupby(e.index.year).last()
    year_prev = year_end.shift(1)
    year_prev.iloc[0] = e.iloc[0]
    rets["Year"] = (year_end / year_prev - 1).values
    rets.index.name = "Year"
    return pd.DataFrame(rets)


# ---------------------------------------------------------------------------
# Indicators: pandas does them in a line each
# ---------------------------------------------------------------------------
def sma(close, n: int):
    return close.rolling(n).mean()


def ema(close, n: int):
    return close.ewm(span=n, adjust=False).mean()


def rsi(close, n: int = 14):
    """Wilder's RSI, 0..100."""
    d = close.diff()
    up = d.clip(lower=0).ewm(alpha=1 / n, adjust=False).mean()
    down = (-d.clip(upper=0)).ewm(alpha=1 / n, adjust=False).mean()
    return 100 - 100 / (1 + up / down)
