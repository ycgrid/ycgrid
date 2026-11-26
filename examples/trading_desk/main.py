# SPDX-License-Identifier: Apache-2.0
"""Trading desk: the screen a trader builds for one contract, on one sheet: a watch list, order buttons, a
click-to-trade ladder, the position and the fills, with a live candle chart docked beside them that marks every fill
and the position's average price.

Every piece is a building block composed here: the watch list and the fills are bound tables (flash on change), the
buttons are formatted cells with click actions (yc.interact.on_click), the ladder is the examples' shared Ladder
(../lib/ex_trading.py), orders go through an OrderManager with risk limits to a paper gateway (yc.sim: nothing
reaches a real market), yc.bars makes 5-second candles from the gateway's market and yc.plot draws them, docked.
Click Bid sizes in the ladder to buy at that price and Ask sizes to sell; BUY MKT / SELL MKT trade the quantity in the
Qty cell; FLATTEN closes the position and cancels the working orders.

Open trading_desk.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import threading
import time

import numpy as np

from yc import bars, excel, plot, safety, sim, trading
from yc.data import bind
from yc.interact import on_click

import ex_math as qm
from ex_trading import Ladder
from ex_util import DARK, T, add_rule, chart, dark_table, fresh_sheet, header, in_host, rgb, thread, title, write

_state: dict = {}
POINT = 200  # TXF: money per index point
# the watch list: the traded contract first, then what a TXF trader keeps an eye on (simulated)
WATCH = [("TXF", None), ("MXF", None), ("TXO 23000 C", "C"), ("TXO 23000 P", "P"),
         ("2330 TSMC", 1085.0), ("2317 Hon Hai", 212.5), ("2454 MediaTek", 1340.0), ("USD/TWD", 31.95)]


def history(gw_market, bb: bars.BarBuilder, count: int, seed: int) -> None:
    """count bars before now (the chart starts full): a random walk of ticks every quarter second at 22% a year,
    ending at the gateway's price."""
    rng = np.random.default_rng(seed)
    secs = count * bb.seconds
    n = int(secs / 0.25)
    step = 0.22 * (0.25 / (252 * 6.5 * 3600)) ** 0.5
    path = np.exp(np.cumsum(rng.standard_normal(n) * step))
    prices = np.round(float(gw_market.last) * path / path[-1])
    now = time.time()
    for k in range(n):
        bb.add(now - secs + k * 0.25, float(prices[k]), float(rng.integers(1, 12)))


def run(seconds: float = 0, bar_seconds: float = 5.0, bars_shown: int = 90):
    """seconds: how long the screen updates (0 = until YCGrid closes or run() is called again)."""
    if "stop" in _state:
        _state["stop"].set()
        _state["gw"].close()
    gw = trading.PaperGateway(spot=23000, seed=21, step=0.15)
    # duplicate_window: a double-click on BUY MKT sends one order, not two
    om = trading.OrderManager(gw, risk=trading.RiskLimits(max_qty=10, max_position=20, price_band=0.01,
                                                          duplicate_window=0.5),
                              require_operator=False)
    ws = fresh_sheet("Desk", trading=True)
    ws.Cells.Font.Name = "Arial"
    title(ws, "Trading desk: TXF on one screen (paper gateway, risk-checked)",
          "Click Bid / Ask sizes to trade at that price, the buttons for market orders. Limits: 10 lots an order, position 20.")

    # order entry: a quantity cell and four buttons (formatted cells with click actions)
    write(ws, "A4", [["Qty", 2]])
    ws.Range("A4").Font.Color = rgb(DARK["dim"])
    qty_cell = ws.Range("B4")
    qty_cell.Interior.Color = rgb(DARK["input_bg"])
    qty_cell.Font.Color = rgb(DARK["mine"])
    qty_cell.Font.Bold = True
    buttons = {"C4": ("BUY MKT", "#b00000"), "D4": ("SELL MKT", "#007a33"), "E4": ("FLATTEN", "#5a5a5a"),
               "F4": ("CANCEL", "#3a3a3a")}
    for cell, (text, fill) in buttons.items():
        b = ws.Range(cell)
        b.Value = text
        b.Interior.Color = rgb(fill)
        b.Font.Color = 0xFFFFFF
        b.Font.Bold = True
        b.HorizontalAlignment = excel.xlCenter

    lad = Ladder(ws, at="A6", name="DeskLadder")  # My Bid | Bid | Price | Ask | My Ask in A:E

    # the watch list, the position and the fills, right of the ladder
    ws.Range("G3").Value = "Watch list"
    ws.Range("G3").Font.Bold = True
    ws.Range("G3").Font.Color = rgb(DARK["volume"])
    watch = bind(ws, None, key="Symbol", name="DeskWatch", at=ws.Range("G4"),
                 columns=["Symbol", "Last", "Chg", "Chg%"], record=False,
                 formats={"Last": "#,##0.00", "Chg": "\u25b2#,##0.00;\u25bc#,##0.00;0.00", "Chg%": "+0.00%;-0.00%;0.00%"},
                 flash={"Last": {"mode": "updown", "up": "#5c0000", "down": "#003d1a"}})
    dark_table(watch)
    first = 5
    chg = ws.Range(f"H{first}:J{first + len(WATCH) - 1}")  # Last, Chg and Chg% colored by the change
    add_rule(chg, f"=$I{first}>0", font=DARK["up"])
    add_rule(chg, f"=$I{first}<0", font=DARK["down"])
    ws.Range(f"G{first}:G{first + len(WATCH) - 1}").Font.Color = rgb(DARK["text"])

    r = first + len(WATCH) + 1
    ws.Range(f"G{r}").Value = "Position"
    ws.Range(f"G{r}").Font.Bold = True
    ws.Range(f"G{r}").Font.Color = rgb(DARK["volume"])
    header(write(ws, f"G{r + 1}", [["Contract", "Qty", "Avg", "P&L", "Realized"]]))
    pos_row = ws.Range(f"G{r + 2}:K{r + 2}")
    pos_row.NumberFormat = "#,##0.0"
    ws.Range(f"H{r + 2}").NumberFormat = "+0;-0;0"
    ws.Range(f"J{r + 2}:K{r + 2}").NumberFormat = "[Red]+#,##0;[Green]-#,##0;0"
    ws.Range(f"H{r + 2}").Font.Color = rgb(DARK["mine"])

    r += 4
    ws.Range(f"G{r}").Value = "Fills"
    ws.Range(f"G{r}").Font.Bold = True
    ws.Range(f"G{r}").Font.Color = rgb(DARK["volume"])
    fills = bind(ws, None, key="#", name="DeskFills", at=ws.Range(f"G{r + 1}"),
                 columns=["Time", "Side", "Qty", "Price", "P&L", "#"], record=False,
                 formats={"Time": "hh:mm:ss", "Price": "#,##0", "P&L": "[Red]+#,##0;[Green]-#,##0;0"})
    dark_table(fills)
    side_col = ws.Range(f"H{r + 2}:H{r + 40}")
    add_rule(side_col, f'=$H{r + 2}="BUY"', font=DARK["up"], bold=True)
    add_rule(side_col, f'=$H{r + 2}="SELL"', font=DARK["down"], bold=True)
    for col, w in (("G", 13), ("H", 9.5), ("I", 8), ("J", 8), ("K", 8), ("L", 4)):
        ws.Columns(col).ColumnWidth = w

    # the chart: 5-second candles with volume, the fills marked, the average price as a line
    bb = bars.BarBuilder(seconds=bar_seconds, keep=3000)
    history(gw.market, bb, bars_shown, seed=5)
    fig = plot.figure("TXF 5-second candles", rows=2, link_x=True, theme="dark",
                      size=(900, 700), height_ratios=[3, 1])
    fig[0].set(follow=bars_shown * bar_seconds, yfit="stable", legend=False,
               title="TXF 5-second candles: your fills (cyan buy, yellow sell) and average price")
    fig[1].set(follow=bars_shown * bar_seconds, legend=False)
    with chart():
        bb.draw(fig[0], fig[1])
    marks = fig[0].signals([], [], [], label="fills", buy_color=T["cyan"], sell_color=T["yellow"], size=7)
    avg_line = {"item": None, "at": None}
    fig.Dock("right", Ratio=0.44)

    lock = threading.Lock()
    fill_rows: list = []
    realized = {"money": 0.0}
    # the position and its average price before each fill, kept here: on_fill runs after the order manager has
    # counted the fill, and a fill that closes the position leaves no average to read there (its profit was lost)
    book = {"q": 0, "avg": 0.0}

    def on_fill(f):
        with lock:
            fill_rows.append(f)
            pnl = 0.0
            signed = f.qty if f.side == "buy" else -f.qty
            q0, avg0 = book["q"], book["avg"]
            if q0 and (q0 > 0) != (signed > 0):  # a fill against the position: its realized profit
                closed = min(abs(q0), abs(signed))
                pnl = (f.price - avg0) * closed * POINT * (1 if q0 > 0 else -1)
            q1 = q0 + signed
            if q1 == 0:
                book["avg"] = 0.0
            elif q0 == 0 or (q0 > 0) == (signed > 0):  # opened or added to
                book["avg"] = (avg0 * abs(q0) + f.price * abs(signed)) / abs(q1)
            elif (q1 > 0) != (q0 > 0):  # through flat to the other side
                book["avg"] = f.price
            book["q"] = q1
            realized["money"] += pnl
            fills.update({"#": [len(fill_rows)], "Time": [f.time / 86400 + 25569 - time.timezone / 86400],
                          "Side": [f.side.upper()], "Qty": [f.qty], "Price": [f.price], "P&L": [pnl or None]})

    om.on_fill.append(on_fill)

    def price_of(ctx):
        px = (ctx.get("record") or {}).get("Price")
        return None if px is None else float(px)

    def qty():
        """The quantity typed in B4, or None when it isn't a whole number of lots: a typo sends no order (it sent one
        lot)."""
        v = qty_cell.Value
        if isinstance(v, bool) or not isinstance(v, (int, float)) or v != int(v) or v < 1:
            return None
        return int(v)

    def send(side, price=None):
        n = qty()
        if n is None:
            excel.Application.StatusBar = f"Order refused: the quantity in B4 ({qty_cell.Value!r}) isn't a whole number of lots"
            return
        try:
            om.submit("TXF", side, n, price)
            excel.Application.StatusBar = False
        except trading.Rejected as e:
            excel.Application.StatusBar = f"Order refused: {e}"

    def at_price(side):
        def h(ctx):
            px = price_of(ctx)
            if px is not None:
                send(side, px)
        return h

    def cancel(side):
        def h(ctx):
            px = price_of(ctx)
            for o in om.orders(open_only=True):
                if o.side == side and o.price == px:
                    om.cancel(o.id)
        return h

    if in_host():  # click actions run in the YCGrid host
        on_click(lad.column(1), at_price("buy"))
        on_click(lad.column(3), at_price("sell"))
        on_click(lad.column(0), cancel("buy"))
        on_click(lad.column(4), cancel("sell"))
        on_click(ws.Range("C4"), lambda ctx: send("buy"))
        on_click(ws.Range("D4"), lambda ctx: send("sell"))
        on_click(ws.Range("E4"), lambda ctx: om.flatten())
        on_click(ws.Range("F4"), lambda ctx: om.cancel_all())

    # the other contracts on the watch list move on their own markets
    others = {name: sim.Market(spot=base, vol=0.25, tick=0.05 if base < 100 else 0.5, seed=31 + i)
              for i, (name, base) in enumerate(WATCH) if isinstance(base, float)}
    opens = {name: m.last for name, m in others.items()}
    tx_open = float(gw.market.last)
    stop = threading.Event()
    _state.update(stop=stop, gw=gw, om=om)
    rng = np.random.default_rng(3)

    def watch_rows(tx: float) -> dict:
        rows = {"Symbol": [], "Bid": [], "Ask": [], "Last": [], "Chg": [], "Chg%": []}

        def put(sym, last, base, spread):
            for k, v in (("Symbol", sym), ("Bid", last - spread), ("Ask", last + spread), ("Last", last),
                         ("Chg", last - base), ("Chg%", last / base - 1 if base else 0.0)):
                rows[k].append(v)

        put("TXF", tx, tx_open, 1)
        put("MXF", tx + 2, tx_open + 2, 1)
        for name, cp in (("TXO 23000 C", "C"), ("TXO 23000 P", "P")):
            px = float(qm.bs_price(tx, 23000, 10 / 365, 0.015, 0.18, cp))
            base = float(qm.bs_price(tx_open, 23000, 10 / 365, 0.015, 0.18, cp))
            put(name, round(px, 1), round(base, 1), 0.5)
        for name, m in others.items():
            put(name, float(m.last), opens[name], m.tick)
        return rows

    def loop():
        hb = safety.heartbeat("example desk", timeout=3)
        t_end = time.time() + seconds if seconds else None
        mkt = gw.market
        last_draw = 0.0
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            now = time.time()
            tx = float(mkt.last)
            bb.add(now, tx, float(rng.integers(1, 12)))
            for m in others.values():
                m.step(0.1)
            if now - last_draw >= 0.25:
                last_draw = now
                mine = [o for o in om.orders(open_only=True) if o.price is not None]
                lad.update(tx, *mkt.book(levels=12),
                           {o.price: o.qty - o.filled for o in mine if o.side == "buy"},
                           {o.price: o.qty - o.filled for o in mine if o.side == "sell"})
                watch.update(watch_rows(tx))
                q, avg = om.position("TXF"), om.avg_cost("TXF")
                open_pnl = (tx - avg) * q * POINT if q and avg else 0.0
                pos_row.Value = [["TXF", q, avg if q else None, open_pnl, realized["money"]]]
                with chart():
                    bb.draw(fig[0], fig[1])
                    with lock:
                        fs = list(fill_rows)
                    if fs:
                        marks.set_data([f.time for f in fs], [f.price for f in fs], [1 if f.side == "buy" else -1 for f in fs])
                    want = round(avg, 1) if q and avg else None
                    if want != avg_line["at"]:
                        if avg_line["item"] is not None:
                            avg_line["item"].remove()
                            avg_line["item"] = None
                        if want is not None:
                            avg_line["item"] = fig[0].hline([want], label=f"avg {want:,.1f} ({q:+d})", color=T["cyan"],
                                                            tag=True, style="dash")
                        avg_line["at"] = want
                hb.beat()
            time.sleep(0.1)
        hb.stop()

    thread(loop, "example-desk")
    ws.Range("B4").Select()
    print("[example] the trading desk is live: click the ladder or the buttons to trade (paper gateway)")
    return om


def Workbook_Open():
    run()
