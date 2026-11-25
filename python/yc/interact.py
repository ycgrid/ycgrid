# SPDX-License-Identifier: Apache-2.0
"""[YC] Interaction for trading screens: click actions on cell ranges, hotkeys, and the context of a cell.

    from yc import interact

    @interact.on_click("Ladder!B3:B42", buttons=("left", "right"))
    def ladder_click(ctx):                 # ctx.record = the clicked table row by column name
        side = "buy" if ctx.button == "left" else "sell"
        send_order(side, price=ctx.record["Price"], qty=params.size)

    interact.on_key("^b", lambda ctx: send_order("buy", ctx.record["Strike"]))   # Ctrl+B on a chain row
    ctx = interact.cell_context()          # the active cell: value, table, row record...

Click actions fire on a mouse click only (never on arrow keys, so moving around can't send orders); a right click
on a range that handles it doesn't open the context menu. Handlers run in the Python host (main.py, modules\\).
"""
from __future__ import annotations

import itertools
import threading
from typing import Callable, Iterable, Optional, Union

from ._conn import YcError, current as _conn

__all__ = ["on_click", "off_click", "on_key", "off_key", "cell_context", "Context"]

_ids = itertools.count(1)
_clicks: dict[int, Callable] = {}
_keys: dict[str, Callable] = {}
_lock = threading.Lock()


class Context(dict):
    """The context of a cell: sheet, sheet_name, row, col (0-based), address, value, text; in a table also table,
    column, table_row and record (the row by column name). Click actions add button ("left" / "right"), double,
    ctrl, shift, alt; hotkeys add key. Items are also attributes: ctx.record["Strike"], ctx.value."""

    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError:
            raise AttributeError(name) from None

    @property
    def range(self):
        """The cell as a Range."""
        from . import excel

        return excel.Range._from(self["sheet"], [(self["row"], self["col"], self["row"], self["col"])])


def _rect_of(target):
    from . import excel

    if isinstance(target, excel.Range):
        rng = target
    elif "!" in str(target):
        sheet, ref = str(target).rsplit("!", 1)
        rng = excel.Worksheets(sheet.strip("'")).Range(ref)
    else:
        rng = excel.ActiveSheet.Range(str(target))
    return rng._sheet, list(rng._a)


def on_click(target, fn: Optional[Callable] = None, buttons: Union[str, Iterable[str]] = ("left",)):
    """Call fn(ctx) when a cell of `target` is clicked. Usable as a decorator. Returns the handler id (for off_click)."""
    btns = {buttons} if isinstance(buttons, str) else set(buttons)
    if not btns <= {"left", "right"}:
        raise YcError("buttons: 'left' and / or 'right'", 1004, "E_ARG")

    def register(f):
        sheet, rect = _rect_of(target)
        hid = next(_ids)
        with _lock:
            _clicks[hid] = f
        _conn().request("ui.click_range", {"id": hid, "sheet": sheet, "rect": rect, "left": "left" in btns, "right": "right" in btns})
        try:
            f.click_id = hid
        except AttributeError:  # a bound method: off_click takes the id
            pass
        return f

    return register(fn) if fn is not None else register


def off_click(handler) -> None:
    hid = getattr(handler, "click_id", handler)
    with _lock:
        _clicks.pop(hid, None)
    _conn().request("ui.click_range_remove", {"id": int(hid)})


def on_key(key: str, fn: Optional[Callable] = None):
    """Call fn(ctx) when the key is pressed (Excel's key text: "^b" Ctrl+B, "+{F5}" Shift+F5, "%s" Alt+S). The key
    no longer does its built-in action. Usable as a decorator."""

    def register(f):
        with _lock:
            _keys[key.lower()] = f
        _conn().request("ui.onkey", {"key": key, "procedure": f"yc.interact:{key}"})
        return f

    return register(fn) if fn is not None else register


def off_key(key: str) -> None:
    with _lock:
        _keys.pop(key.lower(), None)
    _conn().request("ui.onkey", {"key": key, "procedure": None})


def cell_context(target=None) -> Context:
    """The context of a cell (the active cell when target is None)."""
    if target is None:
        return Context(_conn().request("ui.cell_context", {}))
    sheet, rect = _rect_of(target)
    return Context(_conn().request("ui.cell_context", {"sheet": sheet, "row": rect[0], "col": rect[1]}))


# ---- called by the host ----
def _dispatch_click(params: dict) -> None:
    with _lock:
        fn = _clicks.get(int(params.get("id", 0)))
    if fn is not None:
        fn(Context(params))


def _dispatch_key(params: dict) -> bool:
    """True when the key belonged to on_key (else the host runs it as Application.OnKey's procedure)."""
    proc = str(params.get("procedure", ""))
    if not proc.startswith("yc.interact:"):
        return False
    with _lock:
        fn = _keys.get(proc.split(":", 1)[1].lower())
    if fn is not None:
        fn(Context(params))
    return True
