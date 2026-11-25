# SPDX-License-Identifier: Apache-2.0
"""[YC] Alert rules: thresholds on cells or bound-table columns that highlight, beep, pop up and tell Python.

    from yc import alerts

    alerts.add("Delta limit", "Risk!C5", ">", 500, popup=True, sound=True)
    alerts.add("Spread wide", "Quotes[spread]", ">", 2.0, color="#ff9800", cooldown=10)
    alerts.add("IV out of band", "Chain!K3:K60", "outside", 0.10, 0.60)
    alerts.add("Fill", "Blotter!F2:F500", "changed", sound=True)

    @alerts.on_alert
    def handle(a):                         # a.name, a.message, a.address, a.value, a.record (a table row)...
        if a.name == "Delta limit":
            hedge()

Rules are saved with the workbook and evaluated by YCGrid whenever the data change, also without Python. The cells that
meet a condition are highlighted; once per cooldown a rule beeps, shows a popup, flashes the taskbar button (when
asked), writes the change log and calls the on_alert handlers (in the Python host).
"""
from __future__ import annotations

from typing import Callable, Optional

from ._conn import YcError, current as _conn

__all__ = ["add", "remove", "rules", "on_alert"]

_OPS = {">": 1, "<": 2, ">=": 3, "<=": 4, "=": 5, "==": 5, "<>": 6, "!=": 6, "between": 7, "outside": 8, "changed": 9, "change": 9}
_handlers: list[Callable] = []


def _color(v) -> int:
    if v in (None, False, 0, ""):
        return 0
    if isinstance(v, str) and v.startswith("#") and len(v) == 7:
        return 0xFF000000 | int(v[1:], 16)
    v = int(v)  # Excel RGB: R + G*256 + B*65536
    return 0xFF000000 | ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF)


def _target(target):
    from . import excel
    from .data import BoundTable

    if isinstance(target, BoundTable):
        rng = target.list_object.DataBodyRange
    elif isinstance(target, excel.Range):
        rng = target
    elif "[" in str(target) and "!" not in str(target):  # Table[column]
        name, col = str(target).rstrip("]").split("[", 1)
        lo = _find_table(name)
        rng = lo.ListColumns(col).DataBodyRange
    elif "!" in str(target):
        sheet, ref = str(target).rsplit("!", 1)
        rng = excel.Worksheets(sheet.strip("'")).Range(ref)
    else:
        rng = excel.ActiveSheet.Range(str(target))
    return rng._sheet, list(rng._a)


def _find_table(name: str):
    from . import excel

    for ws in excel.Worksheets:
        for lo in ws.ListObjects:
            if lo.Name.lower() == name.lower():
                return lo
    raise YcError(f"No table called {name!r}", 9, "E_NOT_FOUND")


def add(name: str, target, op: str = ">", value: float = 0, value2: Optional[float] = None, sound: bool = True, popup: bool = True,
        flash_window: bool = False, color="#ffc107", cooldown: float = 5, enabled: bool = True, notify: bool = False) -> None:
    """Add (or replace) a rule. target: a Range, an address ("Risk!C5:C9"), "Table[column]" or a BoundTable.
    op: ">", "<", ">=", "<=", "=", "<>", "between" / "outside" (value..value2), "changed" (any change of the value).
    color: the highlight ("#rrggbb", an Excel RGB number, or None). notify: also a Windows notification, seen when
    YCGrid is minimized or behind other windows."""
    if str(op).lower() not in _OPS:
        raise YcError(f"Unknown alert condition: {op!r}", 1004, "E_ARG")
    sheet, rect = _target(target)
    _conn().request("alerts.set", {"name": str(name), "sheet": sheet, "rect": rect, "op": _OPS[str(op).lower()], "a": float(value or 0),
                                   "b": float(value2 if value2 is not None else 0), "sound": bool(sound), "popup": bool(popup),
                                   "flash_window": bool(flash_window), "color": _color(color), "cooldown": float(cooldown),
                                   "enabled": bool(enabled), "notify": bool(notify)})


def remove(name: str) -> bool:
    return bool(_conn().request("alerts.remove", {"name": str(name)}))


def rules() -> list[dict]:
    return _conn().request("alerts.list", {})


def on_alert(fn: Callable) -> Callable:
    """Register a handler fn(alert) called when a rule fires (in the Python host). Usable as a decorator."""
    _handlers.append(fn)
    return fn


class Alert(dict):
    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError:
            raise AttributeError(name) from None


def _dispatch(params: dict) -> None:
    a = Alert(params)
    for fn in list(_handlers):
        fn(a)
