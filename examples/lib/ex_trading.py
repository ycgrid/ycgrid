# SPDX-License-Identifier: Apache-2.0
"""The examples' trading screen pieces: the click-to-trade ladder (dom_ladder, time_machine, txo_market_maker) and
the T-style option chain (options_desk, txo_market_maker), each a bound table with conditional formats.
"""
from __future__ import annotations

from yc import excel
from yc.data import bind

from ex_util import DARK, add_rule, col_letter, dark_table, rgb

LEVELS, TICK = 30, 1.0
CALLS = ["C Delta", "C IV", "C Bid", "C Ask", "C Last"]
PUTS = ["P Last", "P Bid", "P Ask", "P IV", "P Delta"]


class Ladder:
    """My Bid | Bid | Price | Ask | My Ask, prices high to low around the market."""

    COLUMNS = ["My Bid", "Bid", "Price", "Ask", "My Ask"]

    def __init__(self, ws, at: str, name: str):
        self.ws = ws
        a = ws.Range(at)._a
        self.r0, self.c0 = a[0], a[1]
        self.table = bind(ws, None, key="Price", name=name, at=at, columns=self.COLUMNS, record=False,
                          formats={"Price": "#,##0", "Bid": "int", "Ask": "int", "My Bid": "int", "My Ask": "int"},
                          flash={"Bid": {"mode": "change", "up": "#5c4d00"}, "Ask": {"mode": "change", "up": "#5c4d00"}})
        dark_table(self.table)
        self.last_cell = ws.Cells(self.r0, self.c0 + 3)  # above the Price column
        ws.Cells(self.r0, self.c0 + 2).Value = "Last"
        self.last_cell.Font.Color = rgb(DARK["volume"])
        self.prices: list[float] = []
        self._style()

    def _style(self):
        ws, r, c0 = self.ws, self.r0, self.c0
        pc = col_letter(c0 + 2)
        last = f"${pc}${r}"
        first = r + 2  # the first data row (1-based)
        rows = ws.Range(ws.Cells(first, c0 + 1), ws.Cells(first + LEVELS - 1, c0 + 5))
        price = ws.Range(ws.Cells(first, c0 + 3), ws.Cells(first + LEVELS - 1, c0 + 3))
        add_rule(rows, f"=${pc}{first}={last}", fill=DARK["last_bg"], font=DARK["flat"], bold=True)  # the last price's row
        add_rule(price, f"=AND(ISNUMBER({last}),${pc}{first}>{last})", font=DARK["up"])  # above: red
        add_rule(price, f"=AND(ISNUMBER({last}),${pc}{first}<{last})", font=DARK["down"])  # below: green
        for k, color in ((1, "volume"), (3, "volume"), (0, "mine"), (4, "mine")):
            ws.Range(ws.Cells(first, c0 + 1 + k), ws.Cells(first + LEVELS - 1, c0 + 1 + k)).Font.Color = rgb(DARK[color])
        price.Font.Bold = True
        price.HorizontalAlignment = excel.xlCenter
        for i, w in enumerate((8, 9, 11, 9, 8)):
            ws.Columns(c0 + i + 1).ColumnWidth = w

    def column(self, i: int):
        first = self.r0 + 2
        return self.ws.Range(self.ws.Cells(first, self.c0 + 1 + i), self.ws.Cells(first + LEVELS - 1, self.c0 + 1 + i))

    def recenter(self, price: float):
        c = round(price / TICK) * TICK
        self.prices = [c + (LEVELS // 2 - i) * TICK for i in range(LEVELS)]
        self.table.replace({"Price": self.prices}, wait=True)

    def update(self, last: float, bids, asks, my_bids: dict, my_asks: dict):
        self.last_cell.Value = last
        if not self.prices or not (self.prices[-1] + 4 * TICK < last < self.prices[0] - 4 * TICK):
            self.recenter(last)  # near an edge: the ladder follows the market
        b, a = dict(bids), dict(asks)
        self.table.update({"Price": self.prices, "Bid": [b.get(p) for p in self.prices], "Ask": [a.get(p) for p in self.prices],
                           "My Bid": [my_bids.get(p) for p in self.prices], "My Ask": [my_asks.get(p) for p in self.prices]})


def chain_table(ws, at: str, name: str):
    """The T-style chain: a bound table keyed by Strike; calls on the left, puts on the right. The underlying goes in
    the cell above the strikes; conditional formats shade the in-the-money sides and mark the at-the-money row."""
    cols = CALLS + ["Strike"] + PUTS
    fmt = {c: ("0.00" if "Delta" in c else "0.0%" if "IV" in c else "#,##0.0") for c in cols if c != "Strike"}
    t = bind(ws, None, key="Strike", name=name, at=at, columns=cols, formats={**fmt, "Strike": "#,##0"},
             flash=[c for c in cols if c.split()[-1] in ("Bid", "Ask", "Last")], stale_after=3, record=False)
    dark_table(t)
    a = ws.Range(at)._a
    r0, c0 = a[0], a[1]
    k = c0 + len(CALLS)  # the strike column
    spot = ws.Cells(r0, k + 1)  # the cell above the strike header (Cells is 1-based)
    ws.Cells(r0, k).Value = "Underlying"
    ws.Cells(r0, k).HorizontalAlignment = excel.xlRight  # spills left into the empty cells, not cut by the price
    first = f"${col_letter(k)}{r0 + 2}"
    s = f"${col_letter(k)}${r0}"
    calls = ws.Range(ws.Cells(r0 + 2, c0 + 1), ws.Cells(r0 + 200, k))
    puts = ws.Range(ws.Cells(r0 + 2, k + 2), ws.Cells(r0 + 200, k + 1 + len(PUTS)))
    add_rule(calls, f"=AND(ISNUMBER({first}),{first}<{s})", fill=DARK["call_bg"])  # calls in the money below the spot
    add_rule(puts, f"=AND(ISNUMBER({first}),{first}>{s})", fill=DARK["put_bg"])
    row = ws.Range(ws.Cells(r0 + 2, c0 + 1), ws.Cells(r0 + 200, k + 1 + len(PUTS)))
    add_rule(row, f"=AND(ISNUMBER({first}),ABS({first}-{s})<50)", fill=DARK["last_bg"], bold=True)  # at the money (100 apart)
    return t, spot
