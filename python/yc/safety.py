# SPDX-License-Identifier: Apache-2.0
"""[YC] Safety for live trading: parameter cells, heartbeats, operator mode, the kill switch and the change log.

    from yc import safety
    safety.parameter("Params!B2", name="Spread", type=float, min=0, max=5, max_step=0.5)   # typed, checked, logged
    hb = safety.heartbeat("quotes", timeout=2)        # in a data feed: beats from a background thread
    safety.on_kill("risk.flatten_all")                # the KILL SWITCH button (Ctrl+Alt+K) runs this macro
    safety.settings(start_read_only=True, pin="1234", operator_idle_s=600)
    safety.log("order", "TXO 23000C", after="buy 1 @ 101.5")
    safety.change_log(limit=100)

What it guarantees on the screen: when a heartbeat is late, the Python host stops answering or a bound table with
`stale_after` stops updating, the whole window is tinted red and a banner names the source until it recovers; the
start and the end of every alarm are in the change log.
"""
from __future__ import annotations

import csv
import datetime as _dt
import threading
from typing import Callable, Iterable, Optional

from ._conn import YcError, current as _conn

__all__ = ["parameter", "remove_parameter", "parameters", "heartbeat", "Heartbeat", "beat", "log", "change_log", "export_change_log",
           "on_kill", "kill", "operator_mode", "set_operator_mode", "settings", "status", "alarm", "clear_alarm", "alarms"]

_TYPES = {None: 0, "any": 0, float: 1, "number": 1, "float": 1, int: 2, "int": 2, "integer": 2, str: 3, "text": 3, "str": 3,
          bool: 4, "bool": 4, "list": 5, "choice": 5}


def _rq(method: str, **params):
    return _conn().request(method, params)


def _cell(target):
    """(sheet id, row, col) of a Range or an address ("Params!B2", "B2" on the active sheet)."""
    from . import excel

    if isinstance(target, excel.Range):
        rng = target
    elif "!" in str(target):
        rng = _sheet_range(str(target))
    else:
        rng = excel.ActiveSheet.Range(str(target))
    a = rng._a
    return rng._sheet, a[0], a[1]


def _sheet_range(addr: str):
    from . import excel

    sheet, ref = addr.rsplit("!", 1)
    return excel.Worksheets(sheet.strip("'")).Range(ref)


# ---------------------------------------------------------------------------
# Parameter cells
# ---------------------------------------------------------------------------
def parameter(target, name: Optional[str] = None, type=float, min: Optional[float] = None, max: Optional[float] = None,  # noqa: A002
              choices: Optional[Iterable[str]] = None, confirm: bool = True, max_step: float = 0, on_change: Optional[Callable] = None):
    """Make a cell a parameter: the user changes it only by typing, in operator mode, with the type and the range
    checked and (confirm=True) a confirmation that shows the old and the new value; a change larger than `max_step`
    asks again with a warning. Python writes it freely. Every change, from anyone, goes to the change log.
    type: float / int / str / bool / "list" (with choices) / None (anything).
    on_change: a function called with the cell (Range) after each change (in the Python host)."""
    sheet, row, col = _cell(target)
    t = _TYPES.get(type if not isinstance(type, str) else type.lower())
    if t is None:
        raise YcError(f"Unknown parameter type: {type!r}", 1004, "E_ARG")
    if choices is not None:
        t = 5
    _rq("safety.param_set", sheet=sheet, row=row, col=col, name=name or "", type=t, min=min, max=max,
        choices=[str(c) for c in (choices or [])], confirm=bool(confirm), max_step=float(max_step or 0))
    if on_change is not None:
        from . import excel

        rng = excel.Range._from(sheet, [(row, col, row, col)])
        rng.OnChange = on_change
    return target


def remove_parameter(target) -> bool:
    sheet, row, col = _cell(target)
    return bool(_rq("safety.param_remove", sheet=sheet, row=row, col=col))


def parameters() -> list[dict]:
    """Every parameter cell: sheet id, row, col (0-based), name, type, min, max, choices, confirm, max_step."""
    return _rq("safety.params")


# ---------------------------------------------------------------------------
# Heartbeats
# ---------------------------------------------------------------------------
def beat(name: str, timeout: float = 2.0, detail: str = "") -> None:
    """One heartbeat: YCGrid raises an alarm when the next one is more than `timeout` seconds late."""
    _conn().notify("safety.beat", {"name": name, "timeout": float(timeout), "detail": str(detail)})


def alarm(name: str, text: str = "") -> None:
    """[YC] Raise an alarm (the red safety chip, Trading > Safety, status()["alarms"] as "name: text") until it is
    cleared: alarm(name) or clear_alarm(name). Raising it again replaces the text. OrderManager.reconcile uses it."""
    _conn().notify("safety.alarm", {"name": str(name), "text": str(text or "")})


def clear_alarm(name: str) -> None:
    alarm(name, "")


def alarms() -> list[str]:
    """Every alarm showing now: heartbeats, the host, stale tables and the ones raised with alarm()."""
    return list(status().get("alarms", []))


class Heartbeat:
    """Beats from a background thread every timeout/4 seconds while `alive()` (when given) is true.
    Use it in data feeds and strategies: if the process dies or `alive()` turns false, the screen shows it."""

    def __init__(self, name: str, timeout: float = 2.0, alive: Optional[Callable[[], bool]] = None, detail: Optional[Callable[[], str]] = None):
        self.name, self.timeout, self._alive, self._detail = name, float(timeout), alive, detail
        self._stop = threading.Event()
        self._conn = _conn()
        self._thread = threading.Thread(target=self._run, name=f"yc-heartbeat-{name}", daemon=True)
        self._thread.start()

    def _run(self):
        from . import _conn as conn_mod

        conn_mod._tls.conn = self._conn  # this thread talks to the connection that created the heartbeat
        while not self._stop.is_set():
            try:
                if self._alive is None or self._alive():
                    d = self._detail() if self._detail else ""
                    self._conn.notify("safety.beat", {"name": self.name, "timeout": self.timeout, "detail": str(d)})
            except Exception:  # noqa: BLE001
                pass
            self._stop.wait(max(0.05, self.timeout / 4))

    def beat(self, detail: str = ""):
        """An extra beat now (e.g. after each processed message)."""
        self._conn.notify("safety.beat", {"name": self.name, "timeout": self.timeout, "detail": detail})

    def stop(self, remove: bool = True):
        """Stop beating; remove=True also takes the source off the screen (no alarm)."""
        self._stop.set()
        if remove:
            try:
                self._conn.notify("safety.unbeat", {"name": self.name})
            except Exception:  # noqa: BLE001
                pass


def heartbeat(name: str, timeout: float = 2.0, alive: Optional[Callable[[], bool]] = None, detail: Optional[Callable[[], str]] = None) -> Heartbeat:
    return Heartbeat(name, timeout, alive, detail)


# ---------------------------------------------------------------------------
# The change log
# ---------------------------------------------------------------------------
def log(kind: str, target: str, before: str = "", after: str = "", user: Optional[str] = None) -> None:
    """Add an entry (orders, actions, model changes...): kind, target, before / after texts. Time is added by YCGrid."""
    p = {"kind": str(kind), "target": str(target), "before": str(before), "after": str(after), "source": "python"}
    if user:
        p["user"] = str(user)
    _conn().notify("safety.log", p)


def change_log(limit: int = 0) -> list[dict]:
    """The entries, oldest first (limit: the newest N): time (datetime), user, kind, target, before, after, source."""
    out = _rq("safety.log_get", limit=int(limit or 0))
    for e in out:
        e["time"] = _dt.datetime.fromtimestamp(e["time"])
    return out


def export_change_log(path: str) -> str:
    rows = change_log()
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["time", "user", "kind", "target", "before", "after", "source"])
        for e in rows:
            w.writerow([e["time"].isoformat(sep=" ", timespec="milliseconds"), e["user"], e["kind"], e["target"], e["before"], e["after"], e["source"]])
    return path


# ---------------------------------------------------------------------------
# Modes, kill switch, settings
# ---------------------------------------------------------------------------
def settings(**changes) -> dict:
    """Read, or change: start_read_only (bool), pin (str; "" removes it), operator_idle_s (int), kill_macro (str),
    host_timeout_s (float)."""
    allowed = {"start_read_only", "pin", "operator_idle_s", "kill_macro", "host_timeout_s"}
    bad = set(changes) - allowed
    if bad:
        raise YcError(f"Unknown safety settings: {', '.join(sorted(bad))}", 1004, "E_ARG")
    return _rq("safety.settings", **changes)


def on_kill(macro: str) -> None:
    """What the kill switch runs: a macro of the Python host ("module.function"); it gets no arguments."""
    if callable(macro):
        macro = f"{macro.__module__}.{macro.__name__}"
    settings(kill_macro=str(macro))


def kill(ask: bool = False) -> bool:
    """Run the kill switch from code (logged with source "python")."""
    return bool(_rq("safety.kill", ask=bool(ask)))


def operator_mode() -> bool:
    return bool(_rq("safety.mode")["operator"])


def set_operator_mode(on: bool, pin: str = "") -> bool:
    """Switch operator mode (a PIN is needed to switch it on when one is set)."""
    return bool(_rq("safety.mode", operator=bool(on), pin=str(pin))["operator"])


def status() -> dict:
    """{operator, alarms: [text], heartbeats: [{name, age, timeout, detail}]}."""
    return _rq("safety.mode")

