# SPDX-License-Identifier: Apache-2.0
"""Option chain: the T-style quote screen of Taiwanese trading terminals for TXO, calls on the left, the strike in the
middle, puts on the right, with the volatility smile and the open interest by strike docked beside it.

Prices are colored by their change (red up, green down) with arrows, volumes and open interest in yellow, the
in-the-money side of each strike shaded and the at-the-money strike lit; expiry tabs switch the series. The pieces:
one bound table keyed by strike (its header hidden and drawn in two rows of cells, so both sides read Bid / Ask /
Last), conditional formats against the underlying's cell, formatted cells with click actions for the tabs, and
yc.plot for the smile and the open interest. Quotes come from yc.sim's option chain (Black-Scholes on a skewed
smile) around a simulated TAIEX future: no real prices.

Open option_chain.ycg in YCGrid: Workbook_Open below builds the screen. The code the examples share is in ../lib
(ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

import numpy as np

from yc import excel, plot, safety, sim
from yc.data import bind
from yc.interact import on_click

from ex_util import DARK, T, add_rule, chart, col_letter, fresh_sheet, in_host, rgb, thread

_state: dict = {}
UP, DOWN = "\u25b2", "\u25bc"
FONT = "Arial"
STRIKES = np.arange(22300, 23801, 100)
EXPIRIES = [("W1  Dec 2", 6.0), ("Dec 16", 20.0), ("Jan 20", 55.0), ("Mar 17", 111.0)]
SIDE = ["Vol", "OI", "IV", "Delta", "Chg", "Last", "Ask", "Bid"]  # calls left to right; puts the other way round
CALLS = ["C " + c for c in SIDE]
PUTS = ["P " + c for c in reversed(SIDE)]
COLUMNS = CALLS + ["Strike"] + PUTS
PRICE_FMT = "[<50]#,##0.0;#,##0"
CHG_FMT = f"{UP}#,##0.0;{DOWN}#,##0.0;0.0"


def txo_tick(p: float) -> float:
    """TXO's price steps."""
    return 0.1 if p < 10 else 0.5 if p < 50 else 1.0 if p < 500 else 5.0 if p < 1000 else 10.0


def ticked(a) -> np.ndarray:
    return np.array([round(round(x / txo_tick(x)) * txo_tick(x), 1) for x in np.asarray(a, dtype=float)])


class Series:
    """One expiry: yesterday's settlement prices, the day's volume so far and the open interest, by strike."""

    def __init__(self, mkt: sim.Market, prev_spot: float, days: float, rng):
        self.days = days
        base = sim.Market(spot=prev_spot, vol=mkt.vol, seed=1)
        q = base.option_chain(STRIKES, days + 1)
        self.settle = {cp: ticked(q[f"{cp} Last"]) for cp in "CP"}
        dist = (STRIKES - prev_spot) / prev_spot
        scale = 6000 / max(days, 4) ** 0.5
        # open interest: calls pile up above the market, puts below it, round strikes more than others
        bump = np.where(STRIKES % 500 == 0, 1.35, 1.0)
        self.oi = {"C": (scale * 4 * np.exp(-((dist - 0.012) / 0.025) ** 2) * bump + rng.integers(50, 400, len(STRIKES))).astype(int),
                   "P": (scale * 4.4 * np.exp(-((dist + 0.015) / 0.028) ** 2) * bump + rng.integers(50, 400, len(STRIKES))).astype(int)}
        self.vol = {cp: (self.oi[cp] * rng.uniform(0.25, 0.6, len(STRIKES))).astype(int) for cp in "CP"}


def run(seconds: float = 0, seed: int = 6):
    """seconds: how long the quotes stream (0 = until YCGrid closes or run() is called again)."""
    if "stop" in _state:
        _state["stop"].set()
    rng = np.random.default_rng(seed)
    prev_spot = 22950.0
    mkt = sim.Market(spot=23062.0, vol=0.17, seed=seed)
    series = [Series(mkt, prev_spot, d, np.random.default_rng(seed + i)) for i, (_, d) in enumerate(EXPIRIES)]
    current = {"i": 0}

    ws = fresh_sheet("Chain", trading=True)
    ws.Cells.Font.Name = FONT
    n = len(COLUMNS)
    mid = len(CALLS) + 1  # the strike's column (1-based)
    ws.Range("A1").Value = "TXO option chain"
    ws.Range("A1").Font.Bold = True
    ws.Range("A1").Font.Size = 14
    ws.Range("A1").Font.Color = rgb(DARK["volume"])
    ws.Range("D1").Value = "Simulated quotes: click an expiry to switch the series."
    ws.Range("D1").Font.Italic = True
    ws.Range("D1").Font.Color = rgb(DARK["dim"])

    # the underlying and the series' numbers, one strip
    strip = ws.Range(ws.Cells(2, 1), ws.Cells(2, n))
    strip.Interior.Color = rgb("#0c0c0c")
    labels = {1: "TXF", 6: "Days", 8: "ATM IV", 11: "P/C OI", 13: "Max pain", 16: "Volume"}
    for c, text in labels.items():
        ws.Cells(2, c).Value = text
        ws.Cells(2, c).Font.Color = rgb("#b4b4b4")
    spot_cell = ws.Cells(2, 2)
    spot_cell.NumberFormat = "#,##0"
    ws.Cells(2, 3).NumberFormat = CHG_FMT.replace(".0", "")
    ws.Cells(2, 5).NumberFormat = "+0.00%;-0.00%;0.00%"
    add_rule(ws.Range("B2:E2"), "=$C$2>0", font=DARK["up"], bold=True)
    add_rule(ws.Range("B2:E2"), "=$C$2<0", font=DARK["down"], bold=True)
    ws.Cells(2, 9).NumberFormat = "0.0%"
    ws.Cells(2, 12).NumberFormat = "0.00"
    ws.Cells(2, 15).NumberFormat = "#,##0"
    ws.Cells(2, 17).NumberFormat = "#,##0"
    for c in (7, 9, 12, 15):
        ws.Cells(2, c).Font.Color = rgb("#f0f0f0")
    ws.Cells(2, 17).Font.Color = rgb(DARK["volume"])

    # the expiry tabs
    tabs = []
    for i, (name, _) in enumerate(EXPIRIES):
        cell = ws.Range(ws.Cells(3, 1 + 2 * i), ws.Cells(3, 2 + 2 * i))
        cell.Merge()
        cell.NumberFormat = "@"  # "Dec 16" is a name here, not a date
        cell.Value = name
        cell.HorizontalAlignment = excel.xlCenter
        tabs.append(cell)

    # two header rows drawn in cells: CALLS | Strike | PUTS over the column names
    r_band, r_head = 5, 6
    band_c = ws.Range(ws.Cells(r_band, 1), ws.Cells(r_band, mid - 1))
    band_p = ws.Range(ws.Cells(r_band, mid + 1), ws.Cells(r_band, n))
    for rg, text, fill, font in ((band_c, "CALLS", "#3d0a0a", DARK["up"]), (band_p, "PUTS", "#0a3319", DARK["down"])):
        rg.Merge()
        rg.Value = text
        rg.HorizontalAlignment = excel.xlCenter
        rg.Interior.Color = rgb(fill)
        rg.Font.Color = rgb(font)
        rg.Font.Bold = True
    ws.Cells(r_band, mid).Interior.Color = rgb("#262626")

    fmt = {}
    for side in (CALLS, PUTS):
        for c in side:
            k = c[2:]
            fmt[c] = {"Vol": "#,##0", "OI": "#,##0", "IV": "0.0%", "Delta": "0.00", "Chg": CHG_FMT}.get(k, PRICE_FMT)
    fmt["Strike"] = "0"
    table = bind(ws, None, key="Strike", name="Chain", at=ws.Cells(r_head, 1), columns=COLUMNS, record=True, formats=fmt,
                 flash={c: {"mode": "updown", "up": "#6b0000", "down": "#00451d", "ms": 500} for c in ("C Last", "P Last")})
    table.list_object.ShowHeaders = False  # the header row is drawn in cells: both sides read Bid / Ask / Last
    table.list_object.TableStyle = ""
    names = SIDE + ["Strike"] + list(reversed(SIDE))
    head = ws.Range(ws.Cells(r_head, 1), ws.Cells(r_head, n))
    head.Value = [names]
    head.Interior.Color = rgb("#1b1b1b")
    head.Font.Color = rgb("#b4b4b4")
    head.HorizontalAlignment = excel.xlCenter
    head.Borders(excel.xlEdgeBottom).Color = rgb("#5a5a5a")
    rows = len(STRIKES)
    first, last = r_head + 1, r_head + rows
    body = ws.Range(ws.Cells(first, 1), ws.Cells(last, n))
    body.Font.Name = FONT
    body.Borders(excel.xlInsideHorizontal).Color = rgb("#161616")
    widths = {"Vol": 6.6, "OI": 6.8, "IV": 6.0, "Delta": 6.0, "Chg": 7.8, "Last": 6.8, "Ask": 6.4, "Bid": 6.4}
    for c, name in enumerate(COLUMNS, start=1):
        ws.Columns(c).ColumnWidth = 7.6 if name == "Strike" else widths[name[2:]]

    def column(name):
        i = COLUMNS.index(name) + 1
        return ws.Range(ws.Cells(first, i), ws.Cells(last, i)), i

    spot_ref = "$B$2"
    strike_l = f"${col_letter(mid - 1)}{first}"
    calls_rng = ws.Range(ws.Cells(first, 1), ws.Cells(last, mid - 1))
    puts_rng = ws.Range(ws.Cells(first, mid + 1), ws.Cells(last, n))
    add_rule(calls_rng, f"={strike_l}<{spot_ref}", fill="#3a2f10")  # in the money: calls below the market
    add_rule(puts_rng, f"={strike_l}>{spot_ref}", fill="#3a2f10")  # puts above it
    strike_rng, _ = column("Strike")
    add_rule(strike_rng, f"=ABS({strike_l}-{spot_ref})<=50", fill=DARK["volume"], font="#000000", bold=True)
    strike_rng.Interior.Color = rgb("#1e1e1e")
    strike_rng.Font.Color = rgb("#f0f0f0")
    strike_rng.Font.Bold = True
    strike_rng.HorizontalAlignment = excel.xlCenter
    for cp in "CP":
        chg_rng, ci = column(f"{cp} Chg")
        chg = f"${col_letter(ci - 1)}{first}"
        for k in ("Chg", "Last", "Bid", "Ask"):
            rng_k, _ = column(f"{cp} {k}")
            add_rule(rng_k, f"={chg}>0", font=DARK["up"])
            add_rule(rng_k, f"={chg}<0", font=DARK["down"])
        for k in ("Vol", "OI"):
            column(f"{cp} {k}")[0].Font.Color = rgb(DARK["volume"])
        for k in ("IV", "Delta"):
            column(f"{cp} {k}")[0].Font.Color = rgb("#9a9a9a")
        column(f"{cp} Last")[0].Font.Bold = True
    excel.ActiveWindow.FreezePanes = False
    ws.Cells(first, 1).Select()
    excel.ActiveWindow.FreezePanes = True

    # the smile and the open interest, docked beside the chain
    fig = plot.figure("TXO smile and open interest", rows=2, theme="dark", size=(700, 640), height_ratios=[1, 1])
    items: dict = {}

    def paint_tabs():
        for i, cell in enumerate(tabs):
            on = i == current["i"]
            cell.Interior.Color = rgb("#3a3a3a" if on else "#141414")
            cell.Font.Color = rgb(DARK["volume"] if on else "#9a9a9a")
            cell.Font.Bold = on

    lock = threading.Lock()

    def quotes():
        s = series[current["i"]]
        q = mkt.option_chain(STRIKES, s.days)
        out = {"Strike": STRIKES.astype(int)}
        for cp in "CP":
            lastp = ticked(q[f"{cp} Last"])
            for k, v in (("Bid", ticked(q[f"{cp} Bid"])), ("Ask", ticked(q[f"{cp} Ask"])), ("Last", lastp),
                         ("Chg", np.round(lastp - s.settle[cp], 1)), ("IV", q[f"{cp} IV"]), ("Delta", q[f"{cp} Delta"]),
                         ("Vol", s.vol[cp]), ("OI", s.oi[cp])):
                out[f"{cp} {k}"] = v
        return q, out

    def draw(q):
        s = series[current["i"]]
        if items.get("i") != current["i"]:
            for it in items.get("all", []):
                it.remove()
            a0, a1 = fig[0], fig[1]
            # the smile of every expiry: the term structure, the series shown drawn bold
            palette = [T["yellow"], T["magenta"], T["cyan"], T["orange"]]
            items["smiles"] = [a0.line(STRIKES, mkt.iv(STRIKES, d) * 100, label=name.strip(), color=palette[j],
                                       width=2.6 if j == current["i"] else 1.2, marker="o" if j == current["i"] else None)
                               for j, (name, d) in enumerate(EXPIRIES)]
            items["c_iv"] = items["smiles"][current["i"]]
            items["spot"] = a0.vline([float(mkt.last)], color=T["cyan"], style="dash", tag=True, fmt="%.0f")
            items["c_oi"] = a1.bar(STRIKES - 22, s.oi["C"], width=40, label="call OI", color=T["up"])
            items["p_oi"] = a1.bar(STRIKES + 22, s.oi["P"], width=40, label="put OI", color=T["down"])
            items["spot2"] = a1.vline([float(mkt.last)], color=T["cyan"], style="dash")
            items["all"] = items["smiles"] + [items[k] for k in ("spot", "c_oi", "p_oi", "spot2")]
            a0.set(title="Smile by expiry (bold: shown)", ylabel="IV %", legend=True)
            a1.set(title="Open interest by strike (calls red, puts green)", legend=False)
            items["i"] = current["i"]
        else:
            items["c_iv"].set_data(STRIKES, q["C IV"] * 100)
            items["spot"].set_data([float(mkt.last)])
            items["spot2"].set_data([float(mkt.last)])

    def summary():
        s = series[current["i"]]
        k_atm = int(STRIKES[np.argmin(np.abs(STRIKES - mkt.last))])
        q = mkt.option_chain([k_atm], s.days)
        # max pain: the settlement that pays option holders least
        pay = [float(np.sum(s.oi["C"] * np.maximum(k - STRIKES, 0)) + np.sum(s.oi["P"] * np.maximum(STRIKES - k, 0)))
               for k in STRIKES]
        ws.Range(ws.Cells(2, 2), ws.Cells(2, 5)).Value = [[float(mkt.last), float(mkt.last) - prev_spot, None,
                                                            float(mkt.last) / prev_spot - 1]]
        ws.Cells(2, 7).Value = s.days
        ws.Cells(2, 9).Value = float(q["C IV"][0])
        ws.Cells(2, 12).Value = float(s.oi["P"].sum() / s.oi["C"].sum())
        ws.Cells(2, 15).Value = int(STRIKES[int(np.argmin(pay))])
        ws.Cells(2, 17).Value = int(s.vol["C"].sum() + s.vol["P"].sum())

    def show(i: int):
        with lock:
            current["i"] = i
            paint_tabs()
            q, out = quotes()
            table.replace(out, wait=True)
            summary()
            with chart():
                draw(q)

    if in_host():  # click actions run in the YCGrid host
        for i, cell in enumerate(tabs):
            on_click(cell, lambda ctx, i=i: show(i))
    show(0)
    fig.Dock("right", Ratio=0.29)

    stop = threading.Event()
    _state["stop"] = stop

    def loop():
        hb = safety.heartbeat("example option chain", timeout=3)
        t_end = time.time() + seconds if seconds else None
        k = 0
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            mkt.step(2.0)
            s = series[current["i"]]
            for cp in "CP":  # trades print near the money most
                near = np.exp(-((STRIKES - mkt.last) / 250.0) ** 2)
                s.vol[cp] += (rng.poisson(near * 6)).astype(int)
            with lock:
                q, out = quotes()
                table.update(out)
                summary()
                if k % 4 == 0:
                    with chart():
                        draw(q)
            k += 1
            hb.beat()
            time.sleep(0.5)
        hb.stop()

    thread(loop, "example-option-chain")
    print("[example] the option chain is live: click an expiry to switch the series")
    return table, fig


def Workbook_Open():
    run()
