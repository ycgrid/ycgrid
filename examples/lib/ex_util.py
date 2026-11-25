# SPDX-License-Identifier: Apache-2.0
"""Helpers shared by the examples: the trading look, sheet layout, a blotter / positions table made of a bound
table, and load_example() for an example that builds another's screen. Example code: copy what you need."""
from __future__ import annotations

import contextlib
import datetime as _dt
import importlib.util
import os
import sys

from yc import excel
from yc import YcError
from yc.data import bind
from yc.plot import TRADING as T  # noqa: F401 - the trading line colors, for the examples


def rgb(hex_color: str) -> int:
    """'#rrggbb' -> Excel's RGB number (R + G*256 + B*65536)."""
    h = hex_color.lstrip("#")
    return int(h[0:2], 16) + int(h[2:4], 16) * 256 + int(h[4:6], 16) * 65536


# The trading look, after Taiwanese trading terminals: black background, light text, red for up and profit, green for
# down and loss, yellow sizes and volumes, cyan for your own orders.
DARK = {"bg": "#000000", "header": "#262626", "header_text": "#c8c8c8", "text": "#e6e6e6", "dim": "#8c8c8c",
      "up": "#ff3b3b", "down": "#00d25b", "flat": "#ffffff", "volume": "#ffd400", "mine": "#00c8ff",
      "last_bg": "#3a3000", "alert_bg": "#3a0000", "input_bg": "#1a1a1a", "call_bg": "#2a0000", "put_bg": "#002a12",
      "itm_bg": "#141414", "button": "#b00000"}
PNL_FORMAT = "[Red]#,##0;[Green]-#,##0;0"  # profit red, loss green (Taiwanese convention)


def dark_table(table):
    """A bound table (or ListObject) in the black trading style."""
    lo = getattr(table, "list_object", table)
    try:
        lo.TableStyle = "TableStyleTrading"
        lo.ShowAutoFilterDropDown = False  # a quote board, not a list to filter
        hdr = lo.HeaderRowRange  # on a black sheet the cells have their own fill, which wins over the style
        hdr.Interior.Color = rgb(DARK["header"])
        hdr.Font.Color = rgb(DARK["header_text"])
    except Exception:  # noqa: BLE001 - a table that can't be styled keeps its look
        pass
    return table


def col_letter(c: int) -> str:
    """0-based column -> letters."""
    s = ""
    c += 1
    while c:
        c, r = divmod(c - 1, 26)
        s = chr(65 + r) + s
    return s


def add_rule(rng, formula: str, fill=None, font=None, bold=None):
    """A conditional format with a formula (relative to the range's top-left cell)."""
    fc = rng.FormatConditions.Add(excel.xlExpression, Formula1=formula)
    if fill:
        fc.Interior.Color = rgb(fill)
    if font:
        fc.Font.Color = rgb(font)
    if bold is not None:
        fc.Font.Bold = bold
    return fc


def in_host() -> bool:
    """True inside the Python host (click actions and cell events only run there)."""
    return "yc.host" in sys.modules and getattr(sys.modules["yc.host"], "_conn_obj", None) is not None


def blotter(ws, om, at: str, name: str):
    """A blotter: a bound table keyed by order id, fed by the OrderManager's on_order; rows colored by status with
    conditional formats (working cyan, filled yellow, rejected / cancelled gray). Returns the table."""
    cols = ["Id", "Time", "Symbol", "Side", "Qty", "Price", "Filled", "AvgPx", "Status", "Note"]
    t = bind(ws, None, key="Id", name=name, at=at, columns=cols, record=False,
             formats={"Time": "hh:mm:ss.000", "Qty": "#,##0", "Price": "#,##0.00", "Filled": "#,##0", "AvgPx": "#,##0.00"},
             flash={"Status": {"mode": "change", "up": "#5c4d00"}})
    dark_table(t)
    a = ws.Range(at)._a
    sc = col_letter(a[1] + cols.index("Status"))
    row = excel.Range._from(ws._id, [(a[0] + 1, a[1], a[0] + 1, a[1] + len(cols) - 1)])
    add_rule(row, f'=${sc}{a[0] + 2}="Filled"', font=DARK["volume"])
    add_rule(row, f'=OR(${sc}{a[0] + 2}="Rejected",${sc}{a[0] + 2}="Cancelled")', font=DARK["dim"])
    add_rule(row, f'=OR(${sc}{a[0] + 2}="Working",${sc}{a[0] + 2}="New")', font=DARK["mine"])
    for col, need in (("Time", 12.5), ("Symbol", 16), ("Status", 10), ("Note", 14)):  # hh:mm:ss.000, option symbols
        c = ws.Columns(a[1] + cols.index(col) + 1)
        if (c.ColumnWidth or 0) < need:
            c.ColumnWidth = need

    def show(o):
        t.update({"Id": [o.id], "Time": [_dt.datetime.fromtimestamp(o.time)], "Symbol": [o.symbol], "Side": [o.side.title()],
                  "Qty": [o.qty], "Price": [o.price], "Filled": [o.filled], "AvgPx": [o.avg_px or None], "Status": [o.status],
                  "Note": [o.reason or o.tag]})

    om.on_order.append(show)
    return t


def positions(ws, at: str, name: str):
    """A positions table (Contract, Qty, Avg, Last, PnL) in the trading look; feed it with .update({...})."""
    t = bind(ws, None, key="Contract", name=name, at=at, columns=["Contract", "Qty", "Avg", "Last", "PnL"], record=False,
             formats={"Qty": "[Red]#,##0;[Green]-#,##0;0", "Avg": "#,##0.00", "Last": "#,##0.00", "PnL": PNL_FORMAT},
             flash=["Last", "PnL"])
    return dark_table(t)


_trading = set()  # the sheets (ids) in the black trading look


def _blank(ws) -> bool:
    """Nothing on the sheet: no cell holds anything and there are no charts."""
    return ws.UsedRange.Address == "$A$1" and ws.Range("A1").Formula in (None, "") and not list(ws.ChartObjects())


def fresh_sheet(name: str, trading: bool = False):
    """The example's sheet, emptied (made at the end when it doesn't exist); it becomes the active sheet. trading: the
    black look of trading terminals (black cells, light text, no gridlines)."""
    ws = None
    for w in excel.Worksheets:
        if w.Name == name:
            ws = w
            # what a person left on it goes too: an AutoFilter outlives Cells.Clear (as in Excel), and the example's
            # table then refused to go in ("This will change a filtered range", chaos seed 6156)
            if ws.AutoFilterMode:
                ws.AutoFilterMode = False
            if ws.FilterMode:
                ws.ShowAllData()
            ws.Cells.Clear()
            ws.Cells.FormatConditions.Delete()
            for co in list(ws.ChartObjects()):
                co.Delete()
            break
    if ws is None and excel.Worksheets.Count == 1 and _blank(excel.Worksheets(1)):
        ws = excel.Worksheets(1)  # the example's own workbook as it ships: its one empty sheet becomes the first
        ws.Name = name
    if ws is None:
        ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        ws.Name = name
    ws.Activate()
    if trading:
        _trading.add(ws._id)
        ws.Cells.Interior.Color = rgb(DARK["bg"])
        ws.Cells.Font.Color = rgb(DARK["text"])
    else:
        _trading.discard(ws._id)
    excel.ActiveWindow.DisplayGridlines = not trading
    return ws


@contextlib.contextmanager
def chart():
    """Figure updates inside a live loop: after the user closes the figure they're skipped, and the loop (the sheet,
    the orders, the heartbeat) goes on."""
    try:
        yield
    except YcError as e:
        if "was closed" not in str(e):
            raise


def is_trading(obj) -> bool:
    """obj (a Worksheet or a Range) is on a sheet in the trading look."""
    return getattr(obj, "_id", getattr(obj, "_sheet", None)) in _trading


def title(ws, text: str, note: str = ""):
    dark = is_trading(ws)
    ws.Range("A1").Value = text
    ws.Range("A1").Font.Bold = True
    ws.Range("A1").Font.Size = 14
    if dark:
        ws.Range("A1").Font.Color = rgb(DARK["volume"])
    if note:
        ws.Range("A2").Value = note
        ws.Range("A2").Font.Italic = True
        ws.Range("A2").Font.Color = rgb(DARK["dim"]) if dark else 0x595959


def header(rng, fill=None):
    """Header cells: white bold text on dark blue (dark gray on a trading sheet)."""
    dark = is_trading(rng)
    rng.Font.Bold = True
    rng.Font.Color = rgb(DARK["header_text"]) if dark else 0xFFFFFF
    rng.Interior.Color = fill if fill is not None else (rgb(DARK["header"]) if dark else 0x784E1F)


def write(ws, top_left: str, rows):
    """A block of rows from a cell; returns its Range."""
    rows = [list(r) for r in rows]
    if not rows:
        return None
    a = ws.Range(top_left)._a
    rng = excel.Range._from(ws._id, [(a[0], a[1], a[0] + len(rows) - 1, a[1] + max(len(r) for r in rows) - 1)])
    rng.Value = rows
    return rng


def color_scale(rng, low=None, mid=None, high=None):
    """Low red, high green; on a trading sheet low green, black, high red (losses green, profits red)."""
    dark = is_trading(rng)
    low = low if low is not None else (rgb("#00602a") if dark else 0x6B69F8)
    mid = mid if mid is not None else (rgb(DARK["bg"]) if dark else 0xFFFFFF)
    high = high if high is not None else (rgb("#8a0000") if dark else 0x7BBE63)
    cs = rng.FormatConditions.AddColorScale(3)
    cs.ColorScaleCriteria(1).FormatColor.Color = low
    cs.ColorScaleCriteria(2).FormatColor.Color = mid
    cs.ColorScaleCriteria(3).FormatColor.Color = high
    return cs


def thread(target, name: str):
    """Start an example's loop on a daemon thread, as the examples do for their live data. The loop ends quietly when
    YCGrid closes or the connection is lost (YcConnectionError): there is nothing left to draw to. Any other error
    is the example's own and is printed as usual."""
    import threading

    from yc import YcConnectionError

    def run():
        try:
            target()
        except YcConnectionError:
            return

    t = threading.Thread(target=run, daemon=True, name=name)
    t.start()
    return t


def load_example(name: str):
    """Another example's main.py (examples/<name>/main.py) as a module, to build its screen in this workbook as well:
    time_machine records the ladder and the live candles this way. Loaded once; its Workbook_Open isn't called."""
    key = "example_" + name
    if key in sys.modules:
        return sys.modules[key]
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), name, "main.py")
    spec = importlib.util.spec_from_file_location(key, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[key] = mod
    try:
        spec.loader.exec_module(mod)
    except BaseException:
        del sys.modules[key]
        raise
    return mod

