# SPDX-License-Identifier: Apache-2.0
"""Market watch: a multi-symbol quote screen in the style of Taiwanese trading terminals, with the selected symbol's
intraday chart beside it.

Black background, prices colored against the previous close (red above, green below, white unchanged), changes with
up / down arrows, volumes in yellow, a row at its price limit (10% in Taiwan) filled red or green; category tabs above
the list switch what it shows, and a click on a row draws that symbol's intraday line, its average price and the
volume by minute in the docked chart. Everything is a building block composed here: the list is a bound table keyed
by code (its header hidden and drawn in cells, so both sides can read "Bid"), the colors are conditional formats
against the Prev column, the tabs are formatted cells with click actions (yc.interact.on_click), and the chart is
yc.plot. The market is simulated (a session running at 30 times real speed): no real prices.

Open market_watch.ycg in YCGrid: Workbook_Open below builds the screen. The code the examples share is in ../lib
(ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import datetime as dt
import threading
import time

import numpy as np

from yc import excel, plot, safety
from yc.data import bind
from yc.interact import on_click

from ex_util import DARK, T, add_rule, chart, fresh_sheet, in_host, rgb, thread

_state: dict = {}
UP, DOWN = "\u25b2", "\u25bc"  # the arrows terminals put before a change
FONT = "Arial"
SPEED = 30.0  # session seconds per real second
# code, name, previous close, category
SYMBOLS = [
    ("2330", "TSMC", 1085.0, "Semis"), ("2454", "MediaTek", 1340.0, "Semis"), ("2303", "UMC", 46.85, "Semis"),
    ("3711", "ASE Technology", 158.5, "Semis"), ("2379", "Realtek", 512.0, "Semis"), ("3034", "Novatek", 468.5, "Semis"),
    ("3661", "Alchip", 2815.0, "Semis"), ("3529", "eMemory", 2460.0, "Semis"), ("6669", "Wiwynn", 2275.0, "Electronics"),
    ("2317", "Hon Hai", 212.5, "Electronics"), ("2382", "Quanta", 286.0, "Electronics"), ("2308", "Delta", 418.0, "Electronics"),
    ("3008", "Largan", 2305.0, "Electronics"), ("2357", "ASUS", 598.0, "Electronics"), ("2353", "Acer", 35.6, "Electronics"),
    ("3231", "Wistron", 112.0, "Electronics"), ("2345", "Accton", 765.0, "Electronics"), ("2881", "Fubon FHC", 89.4, "Financials"),
    ("2882", "Cathay FHC", 66.2, "Financials"), ("2891", "CTBC FHC", 39.15, "Financials"), ("2886", "Mega FHC", 40.3, "Financials"),
    ("2884", "E.Sun FHC", 28.35, "Financials"), ("2603", "Evergreen", 214.5, "Shipping"), ("2609", "Yang Ming", 72.8, "Shipping"),
    ("2615", "Wan Hai", 88.6, "Shipping"), ("2618", "EVA Air", 41.25, "Shipping"), ("0050", "Taiwan 50 ETF", 196.35, "ETFs"),
    ("0056", "High Dividend ETF", 37.62, "ETFs"), ("00878", "ESG Dividend ETF", 21.88, "ETFs"),
    ("00919", "Value Dividend ETF", 22.97, "ETFs"),
]
WATCH = ["2330", "2317", "2454", "2382", "3661", "2308", "2881", "2603", "0050", "3008", "2303", "6669", "2609", "2891",
         "00878", "3231", "2345", "2357"]
TABS = ["Watch list", "Semis", "Electronics", "Financials", "Shipping", "ETFs"]
COLUMNS = ["Code", "Name", "Time", "Bid", "Ask", "Last", "Chg", "Chg%", "Tick", "Volume", "Open", "High", "Low", "Prev"]
WIDTHS = [6.5, 15, 8, 8.5, 8.5, 8.5, 8.5, 8, 5.5, 9, 8.5, 8.5, 8.5, 8.5]
PRICE_COLS = ["Bid", "Ask", "Last", "Open", "High", "Low"]
OPEN_MIN, CLOSE_MIN = 9 * 60, 13 * 60 + 30


def tick_size(p: float) -> float:
    """The Taiwan stock exchange's price steps."""
    return 0.01 if p < 10 else 0.05 if p < 50 else 0.1 if p < 100 else 0.5 if p < 500 else 1.0 if p < 1000 else 5.0


def to_tick(p: float) -> float:
    t = tick_size(p)
    return round(round(p / t) * t, 2)


class Stock:
    """One symbol's simulated session: minute closes and volumes so far, the last trade, the day's range. The price
    follows a return that moves a little each minute (drift: a trend per minute), trades printing a tick around it."""

    def __init__(self, code, name, prev, cat, rng, drift):
        self.code, self.name, self.prev, self.cat, self.rng, self.drift = code, name, prev, cat, rng, drift
        self.ret = rng.normal(0.002, 0.006)  # the gap at the open
        self.up_limit, self.down_limit = to_tick(prev * 1.1 - tick_size(prev) / 2), to_tick(prev * 0.9 + tick_size(prev) / 2)
        self.open = to_tick(prev * (1 + self.ret))
        self.last, self.high, self.low, self.volume, self.tick = self.open, self.open, self.open, 0, 0
        self.closes, self.vols, self.turnover = [], [], 0.0
        self.lot = int(rng.choice([1, 2, 3, 5, 8, 10, 20]))

    def trade(self):
        """A trade at most a tick from where the price is going, sized in lots."""
        r = self.rng
        p = to_tick(self.prev * (1 + self.ret) + r.choice([-1, 0, 0, 0, 0, 1]) * tick_size(self.last))
        p = min(max(p, self.down_limit), self.up_limit)
        self.last, self.tick = p, int(max(1, r.lognormal(np.log(self.lot), 0.8)))
        self.high, self.low = max(self.high, p), min(self.low, p)
        self.volume += self.tick
        self.turnover += p * self.tick
        if self.vols:
            self.vols[-1] += self.tick
            self.closes[-1] = min(max(to_tick(self.prev * (1 + self.ret)), self.down_limit), self.up_limit)

    def new_minute(self):
        self.ret = min(max(self.ret + self.rng.normal(self.drift, 0.0011), -0.1), 0.1)
        self.closes.append(self.last)
        self.vols.append(0)

    def row(self, clock: str) -> list:
        spread = tick_size(self.last)
        at_up, at_down = self.last >= self.up_limit, self.last <= self.down_limit
        bid = None if at_down else (self.last if at_up else to_tick(self.last - spread * (self.rng.random() < 0.5)))
        ask = None if at_up else (self.last if at_down else to_tick((bid or self.last) + spread))
        return [self.code, self.name, clock, bid, ask, self.last, self.last - self.prev, self.last / self.prev - 1,
                self.tick, self.volume, self.open, self.high, self.low, self.prev]


def session_clock(minute: float) -> str:
    m = int(minute)
    return f"{m // 60:02d}:{m % 60:02d}:{int((minute - m) * 60):02d}"


def style_sheet(ws, r0: int, rows: int):
    """The terminal look: the header drawn in cells, prices colored against Prev, limit rows filled."""
    hdr = ws.Range(ws.Cells(r0, 1), ws.Cells(r0, len(COLUMNS)))
    hdr.Value = [COLUMNS]
    hdr.Interior.Color = rgb("#1b1b1b")
    hdr.Font.Color = rgb("#b4b4b4")
    hdr.HorizontalAlignment = excel.xlCenter
    hdr.Borders(excel.xlEdgeBottom).Color = rgb("#5a5a5a")
    first, last = r0 + 1, r0 + rows
    body = ws.Range(ws.Cells(first, 1), ws.Cells(last, len(COLUMNS)))
    body.Borders(excel.xlInsideHorizontal).Color = rgb("#161616")
    body.Borders(excel.xlEdgeBottom).Color = rgb("#161616")
    for c, w in enumerate(WIDTHS, start=1):
        ws.Columns(c).ColumnWidth = w
    col = {n: excel.Range._from(ws._id, [(first - 1, i, last - 1, i)]) for i, n in enumerate(COLUMNS)}
    prev = f"$N{first}"
    for n in PRICE_COLS:
        rng, a = col[n], col[n].Cells(1, 1).Address(False, False)
        add_rule(rng, f"=AND(ISNUMBER({a}),{a}>{prev})", font=DARK["up"])
        add_rule(rng, f"=AND(ISNUMBER({a}),{a}<{prev})", font=DARK["down"])
    for n in ("Chg", "Chg%"):
        a = col[n].Cells(1, 1).Address(False, False)
        add_rule(col[n], f"={a}>0", font=DARK["up"])
        add_rule(col[n], f"={a}<0", font=DARK["down"])
    # at the price limit: the last price filled, as terminals mark it
    add_rule(col["Last"], f"=AND(ISNUMBER($H{first}),$H{first}>=0.098)", fill="#d00000", font="#ffffff", bold=True)
    add_rule(col["Last"], f"=AND(ISNUMBER($H{first}),$H{first}<=-0.098)", fill="#008a3a", font="#ffffff", bold=True)
    col["Code"].Errors(excel.xlNumberAsText).Ignore = True  # codes are text (0050 keeps its zeros)
    col["Code"].Font.Color = rgb("#f0f0f0")
    col["Name"].Font.Color = rgb("#f0f0f0")
    col["Time"].Font.Color = rgb("#8c8c8c")
    col["Prev"].Font.Color = rgb("#d0d0d0")
    for n in ("Tick", "Volume"):
        col[n].Font.Color = rgb(DARK["volume"])
    ws.Range(ws.Cells(r0, 1), ws.Cells(last, len(COLUMNS))).Font.Name = FONT
    # the selected row, as terminals light it: the code chosen is kept in AA1
    add_rule(body, f"=$A{first}=$AA$1", fill="#10294d")


def run(seconds: float = 0, start_minute: float = 11 * 60 + 5, seed: int = 8):
    """seconds: how long the quotes stream (0 = until YCGrid closes or run() is called again); start_minute: the
    session time the screen opens at (minutes after midnight; the session before it is simulated at once)."""
    if "stop" in _state:
        _state["stop"].set()
    rng = np.random.default_rng(seed)
    # a few names trend through the morning (return per minute): two reach the 10% limit, as some always do
    drifts = {"3661": 0.0011, "2609": 0.0009, "2603": 0.0005, "2382": 0.0003, "3008": -0.0004, "2357": -0.00025,
              "00878": -0.0001}
    stocks = {c: Stock(c, n, p, cat, np.random.default_rng(seed * 1000 + i), drifts.get(c, 0.0))
              for i, (c, n, p, cat) in enumerate(SYMBOLS)}
    for m in range(int(start_minute - OPEN_MIN)):  # the session so far
        for s in stocks.values():
            s.new_minute()
            for _ in range(int(rng.integers(3, 12))):
                s.trade()
    clock = {"minute": float(start_minute)}

    ws = fresh_sheet("Quotes", trading=True)
    ws.Cells.Font.Name = FONT
    ws.Range("A1").Value = "Market watch"
    ws.Range("A1").Font.Bold = True
    ws.Range("A1").Font.Size = 14
    ws.Range("A1").Font.Color = rgb(DARK["volume"])
    ws.Range("C1").Value = "Simulated session at 30x: click a tab to switch the list, a row to chart it."
    ws.Range("C1").Font.Color = rgb(DARK["dim"])
    ws.Range("C1").Font.Italic = True
    # the index strip: an equal-weight index of the list, as the headline number
    idx = ws.Range("A2:N2")
    idx.Interior.Color = rgb("#0c0c0c")
    ws.Range("A2").Value = "Index"
    ws.Range("A2").Font.Color = rgb("#b4b4b4")
    ws.Range("E2").Value = "Volume"
    ws.Range("E2").Font.Color = rgb("#b4b4b4")
    ws.Range("H2").Value = "Adv / Dec"
    ws.Range("H2").Font.Color = rgb("#b4b4b4")
    ws.Range("B2").NumberFormat = "#,##0.00"
    ws.Range("C2").NumberFormat = f"{UP}#,##0.00;{DOWN}#,##0.00;0.00"
    ws.Range("D2").NumberFormat = "+0.00%;-0.00%;0.00%"
    ws.Range("F2").NumberFormat = "#,##0"
    ws.Range("F2").Font.Color = rgb(DARK["volume"])
    add_rule(ws.Range("B2:D2"), "=$C$2>0", font=DARK["up"], bold=True)
    add_rule(ws.Range("B2:D2"), "=$C$2<0", font=DARK["down"], bold=True)
    ws.Range("I2").Font.Color = rgb(DARK["up"])
    ws.Range("J2").Font.Color = rgb(DARK["down"])

    # the tabs: two cells each, the active one lit
    tab_cells = {}
    for i, name in enumerate(TABS):
        rng_tab = ws.Range(ws.Cells(4, 1 + 2 * i), ws.Cells(4, 2 + 2 * i))
        rng_tab.Merge()
        rng_tab.Value = name
        rng_tab.HorizontalAlignment = excel.xlCenter
        tab_cells[name] = rng_tab

    r0, rows = 5, max(len(WATCH), max(sum(1 for s in SYMBOLS if s[3] == t) for t in TABS[1:]))
    table = bind(ws, None, key="Code", name="Quotes", at=ws.Cells(r0, 1), columns=COLUMNS, record=True,
                 formats={"Bid": "#,##0.00", "Ask": "#,##0.00", "Last": "#,##0.00", "Open": "#,##0.00",
                          "High": "#,##0.00", "Low": "#,##0.00", "Prev": "#,##0.00", "Chg": f"{UP}#,##0.00;{DOWN}#,##0.00;0.00",
                          "Chg%": "+0.00%;-0.00%;0.00%", "Tick": "#,##0", "Volume": "#,##0", "Time": "@"},
                 flash={"Last": {"mode": "updown", "up": "#6b0000", "down": "#00451d", "ms": 500},
                        "Tick": {"mode": "change", "up": "#3a3000", "ms": 400}})
    table.list_object.ShowHeaders = False  # the header is drawn in cells: both price sides read as terminals write them
    table.list_object.ShowTableStyleRowStripes = False
    table.list_object.TableStyle = ""
    style_sheet(ws, r0, rows)
    ws.Range("A1:N4").Font.Name = FONT
    excel.ActiveWindow.FreezePanes = False
    ws.Range("A6").Select()
    excel.ActiveWindow.FreezePanes = True

    current = {"tab": "Watch list", "code": WATCH[0]}

    def codes(tab: str) -> list:
        return WATCH if tab == "Watch list" else [c for c, _, _, cat in SYMBOLS if cat == tab]

    def paint_tabs():
        for name, cell in tab_cells.items():
            on = name == current["tab"]
            cell.Interior.Color = rgb("#3a3a3a" if on else "#141414")
            cell.Font.Color = rgb(DARK["volume"] if on else "#9a9a9a")
            cell.Font.Bold = on

    def frame(tab: str) -> dict:
        rows_ = [stocks[c].row(session_clock(clock["minute"])) for c in codes(tab)]
        return {n: [r[i] for r in rows_] for i, n in enumerate(COLUMNS)}

    lock = threading.Lock()

    def show(tab: str):
        with lock:
            current["tab"] = tab
            paint_tabs()
            table.replace(frame(tab), wait=True)

    # the intraday chart: price and its average against the previous close, volume by minute
    fig = plot.figure("Intraday", rows=2, link_x=True, theme="dark", size=(760, 640), height_ratios=[3, 1])
    day = dt.datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
    t_open = day + dt.timedelta(minutes=OPEN_MIN)
    t_close = day + dt.timedelta(minutes=CLOSE_MIN)
    items = {}

    def draw(code: str):
        s = stocks[code]
        n = len(s.closes)
        t = [t_open + dt.timedelta(minutes=k) for k in range(n)]
        closes = np.array(s.closes, dtype=float)
        vols = np.array(s.vols, dtype=float)
        cum = np.cumsum(vols)
        avg = np.cumsum(closes * vols) / np.where(cum > 0, cum, 1)
        avg[cum == 0] = closes[cum == 0]
        dirs = np.sign(np.diff(np.r_[s.prev, closes]))
        colors = [T["up"] if d > 0 else T["down"] if d < 0 else "#bdbdbd" for d in dirs]
        if items.get("code") != code:
            for it in list(items.get("all", [])):
                it.remove()
            reach = max(abs(s.high / s.prev - 1), abs(s.low / s.prev - 1), 0.01) * 1.15
            a0 = fig[0]
            items["prev"] = a0.hline([s.prev], label="previous close", color="#7a7a7a", style="dash", tag=True, fmt="%.2f")
            items["avg"] = a0.line(t, avg, label="average", color=T["yellow"], width=1.4)
            items["px"] = a0.line(t, closes, label="price", color=T["white"], width=1.8, tag=True, fmt="%.2f")
            items["vol"] = fig[1].bar(t, vols, label="volume", colors=colors, width=50)
            items["all"] = [items["prev"], items["avg"], items["px"], items["vol"]]
            a0.set(time=True, xlim=(t_open, t_close), ylim=(s.prev * (1 - reach), s.prev * (1 + reach)), legend=False,
                   title=f"{s.code} {s.name}  (yellow: average price)")
            fig[1].set(time=True, legend=False, ylim=(0, float(max(vols.max(), 1)) * 1.15), title="Volume by minute")
            items["code"] = code
        else:
            items["avg"].set_data(t, avg)
            items["px"].set_data(t, closes)
            items["vol"].set_data(t, vols, colors=colors)

    def select(code: str):
        current["code"] = code
        ws.Range("AA1").Value = code

    def pick(ctx):
        code = (ctx.get("record") or {}).get("Code")
        if code in stocks:
            select(str(code))
            with chart():
                draw(current["code"])

    if in_host():  # click actions run in the YCGrid host
        for name, cell in tab_cells.items():
            on_click(cell, lambda ctx, name=name: show(name))
        on_click(ws.Range(ws.Cells(r0 + 1, 1), ws.Cells(r0 + 40, len(COLUMNS))), pick)
    ws.Range("AA1").NumberFormat = "@"
    ws.Range("AA1").Font.Color = rgb(DARK["bg"])
    select("3661")  # the name at its limit: the morning's story
    show("Watch list")
    with chart():
        draw(current["code"])
    fig.Dock("right", Ratio=0.29)

    stop = threading.Event()
    _state["stop"] = stop
    trng = np.random.default_rng(seed + 1)

    def loop():
        hb = safety.heartbeat("example market watch", timeout=3)
        t_end = time.time() + seconds if seconds else None
        last = time.time()
        while not stop.is_set() and (t_end is None or time.time() < t_end):
            now = time.time()
            m0 = clock["minute"]
            clock["minute"] = min(CLOSE_MIN - 1 / 60, m0 + (now - last) * SPEED / 60)
            last = now
            if int(clock["minute"]) > int(m0):
                for s in stocks.values():
                    s.new_minute()
            for s in stocks.values():
                if trng.random() < 0.35:
                    s.trade()
            with lock:
                table.update(frame(current["tab"]))
            vals = np.array([s.last / s.prev - 1 for s in stocks.values()])
            level = 20000 * (1 + vals.mean())
            ws.Range("B2:D2").Value = [[level, level - 20000, vals.mean()]]
            ws.Range("F2").Value = sum(s.volume for s in stocks.values())
            ws.Range("I2:J2").Value = [[f"{UP} {int((vals > 0).sum())}", f"{DOWN} {int((vals < 0).sum())}"]]
            with chart():
                draw(current["code"])
            hb.beat()
            time.sleep(0.25)
        hb.stop()

    thread(loop, "example-market-watch")
    print("[example] market watch is live: click a tab to switch, a row to chart it")
    return table, fig


def Workbook_Open():
    run()
