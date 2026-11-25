# SPDX-License-Identifier: Apache-2.0
"""Technical chart: daily candles with moving averages and Bollinger bands, volume, MACD and KD in one window, and the
bars in a table beside it.

Two years of simulated daily bars; pandas computes the indicators in a line each, yc.plot draws them as four linked
plots (zoom one and the others follow) docked beside the sheet, and the bars are a bound table: click a row and the
charts mark that day. MACD crosses are marked on the candles. Colors follow Taiwanese terminals: red up, green down.

Open technical_chart.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from yc import plot
from yc.data import bind

import ex_math as qm
from ex_backtest import simulate_prices
from ex_util import DARK, T, add_rule, dark_table, fresh_sheet, header, rgb, title, write


def indicators(px: pd.DataFrame) -> pd.DataFrame:
    """Moving averages, Bollinger bands (20, 2), MACD (12, 26, 9) and KD (9, 3, 3) on daily bars."""
    c = px.Close
    out = px.copy()
    for n in (5, 20, 60):
        out[f"MA{n}"] = qm.sma(c, n)
    sd = c.rolling(20).std()
    out["BB up"], out["BB low"] = out.MA20 + 2 * sd, out.MA20 - 2 * sd
    out["DIF"] = qm.ema(c, 12) - qm.ema(c, 26)
    out["DEA"] = qm.ema(out.DIF, 9)
    out["MACD"] = 2 * (out.DIF - out.DEA)
    low9, high9 = px.Low.rolling(9).min(), px.High.rolling(9).max()
    rsv = (100 * (c - low9) / (high9 - low9)).fillna(50)
    out["K"] = rsv.ewm(alpha=1 / 3, adjust=False).mean()
    out["D"] = out.K.ewm(alpha=1 / 3, adjust=False).mean()
    out["Chg%"] = c.pct_change()
    return out


def volumes(px: pd.DataFrame, seed: int) -> np.ndarray:
    """Lots traded each day: more on big moves and wide ranges, as markets do."""
    rng = np.random.default_rng(seed)
    move = np.abs(px.Close.pct_change().fillna(0).to_numpy()) + (px.High - px.Low).to_numpy() / px.Close.to_numpy()
    return np.round(60_000 * (0.6 + 25 * move) * rng.lognormal(0, 0.25, len(px)), -2)


def run(days: int = 400, seed: int = 11, shown: int = 120):
    """days of bars; shown: how many of the latest the charts show at first (zoom out for the rest)."""
    px = simulate_prices(days=days, start=17000.0, seed=seed)
    px["Volume"] = volumes(px, seed)
    ind = indicators(px)
    up = (px.Close >= px.Open).to_numpy()

    ws = fresh_sheet("Technical", trading=True)
    title(ws, "Technical chart: candles, MA, Bollinger bands, volume, MACD and KD (simulated daily bars)",
          "Click a bar in the table to mark it on the charts; zoom or pan one chart and the others follow.")

    # the latest reading of each indicator, as a trader scans it
    last, prev = ind.iloc[-1], ind.iloc[-2]
    state = [["Close", float(last.Close)],
             ["Change", float(last["Chg%"])],
             ["vs MA20", "above" if last.Close > last.MA20 else "below"],
             ["vs MA60", "above" if last.Close > last.MA60 else "below"],
             ["MACD", "golden cross" if prev.DIF <= prev.DEA < last.DIF else
              "death cross" if prev.DIF >= prev.DEA > last.DIF else ("bullish" if last.DIF > last.DEA else "bearish")],
             ["K / D", f"{last.K:.1f} / {last.D:.1f}" + (" overbought" if last.K > 80 else " oversold" if last.K < 20 else "")],
             ["Bollinger", "above the band" if last.Close > last["BB up"] else
              "below the band" if last.Close < last["BB low"] else "inside the band"]]
    header(write(ws, "A4", [["Signal", "Now"]]))
    write(ws, "A5", state)
    ws.Range("B5").NumberFormat = "#,##0.00"
    ws.Range("B6").NumberFormat = "0.00%"
    ws.Range("B5:B11").HorizontalAlignment = -4131  # xlLeft
    add_rule(ws.Range("B5:B11"), '=OR(B5="above",B5="golden cross",B5="bullish",AND(ISNUMBER(B5),B5>0))',
             font=DARK["up"])
    add_rule(ws.Range("B5:B11"), '=OR(B5="below",B5="death cross",B5="bearish",AND(ISNUMBER(B5),B5<0))',
             font=DARK["down"])

    # the bars, newest first, as a bound table the charts follow
    cols = ["Close", "Chg%", "Volume", "MA20", "MACD", "K"]
    t = ind[cols].iloc[::-1].reset_index(names="Date")
    ws.Range("A13").Value = "Daily bars (newest first)"
    ws.Range("A13").Font.Bold = True
    ws.Range("A13").Font.Color = rgb(DARK["volume"])
    table = bind(ws, t, key="Date", name="Bars", at=ws.Range("A14"), record=False,
                 formats={"Date": "yyyy-mm-dd", "Close": "#,##0", "Chg%": "0.00%", "Volume": "#,##0",
                          "MA20": "#,##0", "MACD": "0.0", "K": "0.0"})
    dark_table(table)
    chg = table.list_object.ListColumns("Chg%").DataBodyRange
    add_rule(chg, f"={chg.Cells(1, 1).Address(False, False)}>0", font=DARK["up"])
    add_rule(chg, f"={chg.Cells(1, 1).Address(False, False)}<0", font=DARK["down"])
    macd = table.list_object.ListColumns("MACD").DataBodyRange
    add_rule(macd, f"={macd.Cells(1, 1).Address(False, False)}>0", font=DARK["up"])
    add_rule(macd, f"={macd.Cells(1, 1).Address(False, False)}<0", font=DARK["down"])
    table.list_object.ListColumns("Volume").DataBodyRange.Font.Color = rgb(DARK["volume"])
    ws.Columns("A").ColumnWidth = 12
    ws.Columns("B").ColumnWidth = 13
    ws.Range("C14:G14").ColumnWidth = 9

    # four linked plots: price, volume, MACD, KD
    fig = plot.figure("Technical chart (simulated TAIEX futures, daily)", rows=4, link_x=True, size=(980, 760),
                      theme="dark", height_ratios=[3, 1, 1.4, 1.7])
    x = ind.index
    p = fig[0]
    p.fill_between(x, ind["BB up"], ind["BB low"], label="Bollinger 20, 2", color="#2a3550")
    p.candles(x, px.Open, px.High, px.Low, px.Close, label="Price")
    p.line(x, ind.MA5, label="MA5", color=T["white"], width=1.2)
    p.line(x, ind.MA20, label="MA20", color=T["yellow"], width=1.6)
    p.line(x, ind.MA60, label="MA60", color=T["magenta"], width=1.6)
    cross = np.sign(ind.DIF - ind.DEA).diff().fillna(0).to_numpy()
    at = np.flatnonzero(cross != 0)
    gold = cross[at] > 0
    p.signals(x[at], np.where(gold, px.Low.to_numpy()[at], px.High.to_numpy()[at]), np.where(gold, 1, -1),
              label="MACD crosses", buy_color=T["up"], sell_color=T["down"], size=6)
    p.set(title="Price   MA5 white, MA20 yellow, MA60 magenta, Bollinger 20 / 2 band, MACD crosses", legend=False)
    fig[1].bar(x, px.Volume, label="Volume", colors=np.where(up, T["up"], T["down"]).tolist())
    fig[1].set(title="Volume", legend=False)
    hist = ind.MACD.fillna(0).to_numpy()
    fig[2].bar(x, hist, label="MACD histogram", colors=np.where(hist >= 0, T["up"], T["down"]).tolist())
    fig[2].line(x, ind.DIF, label="DIF 12-26", color=T["white"], width=1.4)
    fig[2].line(x, ind.DEA, label="DEA 9", color=T["yellow"], width=1.4)
    fig[2].set(title="MACD 12, 26, 9   DIF white, DEA yellow, histogram 2 x (DIF - DEA)", legend=False)
    fig[3].line(x, ind.K, label="K 9", color=T["cyan"], width=1.4)
    fig[3].line(x, ind.D, label="D 3", color=T["orange"], width=1.4)
    fig[3].hline([20, 80], color="#5a5a5a")
    fig[3].set(title="KD 9, 3, 3   K cyan, D orange", ylim=(0, 100), legend=False)
    # the latest bars at first, each plot's y range fitted to them (zoom out for the whole history)
    view = ind.iloc[-shown:]
    lo, hi = float(min(view.Low.min(), view["BB low"].min())), float(max(view.High.max(), view["BB up"].max()))
    p.set(xlim=(x[-shown], x[-1] + pd.Timedelta(days=2)), ylim=(lo - 0.03 * (hi - lo), hi + 0.03 * (hi - lo)))
    fig[1].set(ylim=(0, float(view.Volume.max()) * 1.1))
    m = float(np.nanmax(np.abs(view[["DIF", "DEA", "MACD"]].to_numpy())))
    fig[2].set(ylim=(-1.1 * m, 1.1 * m))
    fig.link_table("Bars", "Date")
    fig.Dock("right", Ratio=0.62)
    ws.Range("A14").Select()
    print(f"[example] technical chart: {len(px)} daily bars, {len(at)} MACD crosses")
    return table, fig


def Workbook_Open():
    run()
