# SPDX-License-Identifier: Apache-2.0
"""Volatility surface: implied volatilities from option prices, an SVI fit per expiry, a 3-D surface and the smiles.

The "market" prices come from a smile with skew and a term structure plus half a tick of noise; _math (plain numpy) inverts them
to implied volatilities (vectorized Newton) and fits a raw SVI smile to each expiry's total variance. The sheet shows
the volatility grid with a color scale and the SVI parameters; the chart window shows the surface (drag to turn) and
the smiles with their fits.

Open vol_surface.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import numpy as np

from yc import plot

import ex_math as qm
from ex_util import color_scale, fresh_sheet, header, title, write

SPOT, RATE = 23000.0, 0.015
DAYS = [7, 14, 30, 60, 90, 180, 365]
MONEYNESS = np.round(np.arange(0.80, 1.2001, 0.025), 3)


def true_vol(k, T):
    """The smile used to make the prices: a decaying short-dated premium, skew and curvature that flatten with T."""
    atm = 0.15 + 0.06 * np.exp(-T / 0.15)
    return np.maximum(0.05, atm - 0.10 * k / np.sqrt(T + 0.05) * 0.35 + 0.9 * k * k / np.sqrt(T + 0.02) * 0.25)


def run(seed: int = 5):
    rng = np.random.default_rng(seed)
    K = SPOT * MONEYNESS
    T = np.array(DAYS) / 365.0
    iv = np.full((len(T), len(K)), np.nan)
    fits = []
    for i, t in enumerate(T):
        F = SPOT * np.exp(RATE * t)
        k = np.log(K / F)
        cp = np.where(K >= SPOT, "C", "P")  # out-of-the-money options
        px = qm.bs_price(SPOT, K, t, RATE, true_vol(k, t), cp)
        noise = rng.uniform(-0.25, 0.25, len(K)) * np.minimum(1.0, px / 20.0)  # half a tick, less on cheap options
        px = np.maximum(px + noise, 0.1)
        iv[i] = qm.implied_vol(px, SPOT, K, t, RATE, cp)
        iv[i][px < 0.5] = np.nan  # too cheap to trust: dropped, as on a real desk
        ok = np.isfinite(iv[i])
        params = qm.svi_fit(k[ok], iv[i][ok] ** 2 * t)
        fit_iv = np.sqrt(np.maximum(qm.svi(k, params), 1e-12) / t)
        rmse = float(np.sqrt(np.nanmean((fit_iv[ok] - iv[i][ok]) ** 2)) * 100)
        fits.append((DAYS[i], *params, rmse, k, fit_iv))
    ws = fresh_sheet("Vol Surface")
    title(ws, f"Implied volatility surface (spot {SPOT:,.0f}, rate {RATE:.1%})",
          "Rows: days to expiry; columns: strike / spot. Out-of-the-money options; SVI fitted per expiry.")
    head = write(ws, "A4", [["Days \\ K/S"] + [float(m) for m in MONEYNESS]])
    header(head)
    grid = write(ws, "A5", [[d] + [None if not np.isfinite(v) else float(v) for v in row] for d, row in zip(DAYS, iv)])
    body = ws.Range(ws.Cells(5, 2), ws.Cells(4 + len(DAYS), 1 + len(K)))
    body.NumberFormat = "0.0%"
    ws.Range(ws.Cells(4, 2), ws.Cells(4, 1 + len(K))).NumberFormat = "0.0%"
    color_scale(body, low=0x7BBE63, mid=0x84EBFF, high=0x6B69F8)
    r0 = 6 + len(DAYS)
    ws.Cells(r0, 1).Value = "SVI fit (total variance w(k) = a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2)))"
    ws.Cells(r0, 1).Font.Bold = True
    header(write(ws, f"A{r0 + 1}", [["Days", "a", "b", "rho", "m", "sigma", "RMSE (vol pts)"]]))
    write(ws, f"A{r0 + 2}", [[f[0], *[float(x) for x in f[1:7]]] for f in fits])
    ws.Range(ws.Cells(r0 + 2, 2), ws.Cells(r0 + 1 + len(fits), 7)).NumberFormat = "0.0000"
    ws.Columns(1).ColumnWidth = 12
    del grid
    # the charts: the surface and the smiles
    fig = plot.figure("Volatility surface", rows=1, cols=2, size=(1200, 520))
    fig[0].surface(MONEYNESS, T, iv * 100, cmap="jet", label="IV")
    fig[0].set(title="Implied volatility (%)", xlabel="K/S", ylabel="T (years)", zlabel="IV %", view=(-50, 28))
    colors = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b", "#e377c2"]
    for (d, *_rest), row, col in zip(fits, iv, colors):
        fit_iv = _rest[-1]
        fig[1].scatter(K, row * 100, label=f"{d}d", color=col, size=4)
        fig[1].line(K, fit_iv * 100, color=col, width=1.5, label=f"##{d}fit")
    fig[1].vline([SPOT], color="#7f7f7f", label="Spot")
    fig[1].set(title="Smiles: market (dots) and SVI (lines)", xlabel="Strike", ylabel="IV %")
    print(f"[example] SVI RMSE by expiry (vol points): " + ", ".join(f"{f[0]}d {f[6]:.2f}" for f in fits))
    return fig


def Workbook_Open():
    run()
