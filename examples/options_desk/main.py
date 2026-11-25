# SPDX-License-Identifier: Apache-2.0
"""Options desk: a live option chain, an iron condor priced from it, its P&L curves, Greeks and a scenario grid.

Everything is built from the pieces: the T-style chain (calls | strike | puts) is a bound table keyed by strike with
change flashes and conditional formats for the at-the-money row and the in-the-money sides; an iron condor around the
at-the-money strike is opened at the chain's mid prices; its P&L at expiry and today (Black-Scholes, a few lines of
numpy) are drawn and redrawn as the market moves, with the spot marked; the legs, the Greeks and a spot x volatility
scenario grid are cells, refreshed every two seconds. The chain itself is in ../lib/ex_trading.py, shared with
txo_market_maker.

Open options_desk.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

import numpy as np
import pandas as pd

from yc import excel, plot, safety, sim
from yc.data import write_frame

import ex_math as qm
from ex_trading import CALLS, PUTS, chain_table
from ex_util import T, chart, fresh_sheet, header, thread, title, write

_state: dict = {}
MULT = 50  # index points to money
RATE = 0.015


def leg_values(legs, spots, vol_shift: float = 0.0, days_passed: float = 0.0):
    """The strategy's value (money) at each spot: Black-Scholes per leg, premium paid subtracted."""
    spots = np.asarray(spots, dtype=float)
    total = np.zeros_like(spots)
    for lg in legs:
        T_ = max(lg["days"] - days_passed, 1e-6) / 365
        total += lg["qty"] * (qm.bs_price(spots, lg["strike"], T_, RATE, max(lg["iv"] + vol_shift, 0.01), lg["cp"]) - lg["premium"])
    return total * MULT


def payoff(legs, spots):
    spots = np.asarray(spots, dtype=float)
    intrinsic = [np.maximum(spots - lg["strike"], 0) if lg["cp"] == "C" else np.maximum(lg["strike"] - spots, 0) for lg in legs]
    return sum(lg["qty"] * (v - lg["premium"]) for lg, v in zip(legs, intrinsic)) * MULT


def run(seconds: float = 0, days: float = 14.0, width: int = 200, wing: int = 100):
    """seconds: how long it runs (0 = until YCGrid closes or run() is called again); days: to expiry."""
    if "stop" in _state:
        _state["stop"].set()
    mkt = sim.Market(spot=23000, vol=0.18, seed=12)
    strikes = np.arange(22000, 24050, 100)
    ws = fresh_sheet("Options Desk", trading=True)
    title(ws, f"Options desk: iron condor {width} / {wing} on a live chain ({days:g} days to expiry, x{MULT})",
          "The chain streams; the condor is opened at the mids; P&L curves, Greeks and the scenario grid follow the market.")
    chain, spot_cell = chain_table(ws, "A6", "DeskChain")
    # T layout: the strike column stays in the middle of a narrow window, calls and puts scroll outward together
    # (a wide window shows everything as it is)
    ws.Activate()
    excel.ActiveWindow.CenterColumns = ws.Range("A6").Offset(0, len(CALLS))
    q = mkt.option_chain(strikes, days=days)
    cols = CALLS + ["Strike"] + PUTS
    chain.replace(pd.DataFrame({c: q[c] for c in cols}))
    spot_cell.Value = float(mkt.spot)
    atm = float(strikes[np.argmin(np.abs(strikes - mkt.spot))])

    def mid(cp, k):
        i = int(np.where(strikes == k)[0][0])
        return float((q[f"{cp} Bid"][i] + q[f"{cp} Ask"][i]) / 2), float(q[f"{cp} IV"][i])

    legs = []
    for cp, k, qty in (("P", atm - width - wing, 1), ("P", atm - width, -1), ("C", atm + width, -1), ("C", atm + width + wing, 1)):
        px, iv = mid(cp, k)
        legs.append({"cp": cp, "strike": k, "qty": qty, "premium": px, "iv": iv, "days": days})
    header(write(ws, "M4", [["Leg", "Strike", "Qty", "Premium", "IV"]]))
    write(ws, "M5", [[lg["cp"], lg["strike"], lg["qty"], lg["premium"], lg["iv"]] for lg in legs])
    ws.Range("P5:P8").NumberFormat = "0.0"
    ws.Range("Q5:Q8").NumberFormat = "0.0%"
    grid_x = np.linspace(atm * 0.9, atm * 1.1, 4001)
    at_exp = payoff(legs, grid_x)
    sign = np.sign(at_exp)
    be = [float(grid_x[i]) for i in np.where(np.diff(sign) != 0)[0]]
    write(ws, "M10", [["Credit (points)", -sum(lg["qty"] * lg["premium"] for lg in legs)],
                      ["Max profit", float(at_exp.max())], ["Max loss", float(at_exp.min())],
                      ["Breakevens", " / ".join(f"{b:,.0f}" for b in be)]])
    ws.Range("N11:N12").NumberFormat = "#,##0"
    header(write(ws, "M15", [[f"Greeks (x{MULT})", "Now"]]))
    ws.Columns(14).ColumnWidth = 16
    spots = np.linspace(atm * 0.93, atm * 1.07, 300)
    fig = plot.figure("Iron condor P&L", size=(820, 460), pos=(120, 180), theme="dark")
    fig[0].set(title="P&L (TWD) at expiry and today", xlabel="Underlying price")
    fig[0].line(spots, payoff(legs, spots), label="at expiry", color=T["white"], width=2)
    now_item = fig[0].line(spots, leg_values(legs, spots), label="today", color=T["yellow"], width=2)
    spot_item = fig[0].vline([float(mkt.spot)], label="spot", color=T["cyan"], width=2)
    fig[0].hline([0], color="#5a5a5a")
    stop = threading.Event()
    _state["stop"] = stop

    def loop():
        hb = safety.heartbeat("example options desk", timeout=3)
        t_end = time.time() + seconds if seconds else None
        last_grid = 0.0
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            mkt.step(0.5)
            qq = mkt.option_chain(strikes, days=days)
            chain.update(pd.DataFrame({c: qq[c] for c in cols}))
            s = float(mkt.spot)
            spot_cell.Value = s
            with chart():
                now_item.set_data(spots, leg_values(legs, spots))
                spot_item.set_data([s])
            if time.time() - last_grid >= 2.0:
                last_grid = time.time()
                g = {k: 0.0 for k in ("delta", "gamma", "vega", "theta")}
                for lg in legs:
                    gl = qm.greeks(s, lg["strike"], lg["days"] / 365, RATE, lg["iv"], lg["cp"])
                    for k in g:
                        g[k] += float(gl[k]) * lg["qty"] * MULT
                write(ws, "M16", [["Delta", g["delta"]], ["Gamma", g["gamma"]], ["Vega", g["vega"]], ["Theta", g["theta"]],
                                  ["P&L now", float(leg_values(legs, [s])[0])]])
                ws.Range("N16:N20").NumberFormat = "#,##0.00"
                now = float(leg_values(legs, [s])[0])
                moves, shifts = [-0.05, -0.03, -0.01, 0, 0.01, 0.03, 0.05], [-0.05, 0.0, 0.05]
                grid = pd.DataFrame({f"{m:+.0%}": [float(leg_values(legs, [s * (1 + m)], dv)[0]) - now for dv in shifts]
                                     for m in moves}, index=[f"vol {dv:+.0%}" for dv in shifts]).T
                ws.Range("M23").Value = "Scenario P&L vs now (spot x volatility)"
                write_frame(ws, grid, at="M24", index=True)
                ws.Range("N25:P31").NumberFormat = "[Red]#,##0;[Green]-#,##0;0"
            hb.beat()
            time.sleep(0.5)
        hb.stop()

    thread(loop, "example-options-desk")
    print(f"[example] iron condor {atm - width - wing:.0f}/{atm - width:.0f}/{atm + width:.0f}/{atm + width + wing:.0f}, "
          f"breakevens {', '.join(f'{b:,.0f}' for b in be)}")
    return legs


def Workbook_Open():
    run()
