# SPDX-License-Identifier: Apache-2.0
"""YCGrid object model (Excel compatible; spec in skill/ycgrid-python/references/object-model.md).

    from yc.excel import *
    Range("A1").Value = "Hello"
    Worksheets("Quotes").Range("B2:D10").Font.Bold = True

All coordinates and collection indexes are 1-based; names and parameters match Excel. Members not implemented yet
raise UnsupportedError (438).
"""
from __future__ import annotations

import datetime as _dt
import decimal as _decimal
import math as _math
import os as _os
import re as _re
import struct as _struct
import threading as _threading
import time as _time
from typing import Any, Iterable, Optional

from . import _msgpack
from ._conn import YcConnectionError, YcError, current as _conn
from ._conn import _tls as _conn_tls
from .constants import *  # noqa: F401,F403
from .constants import (xlDelimited, xlFixedWidth, xlGeneralFormat, xlNo, xlSkipColumn, xlTextFormat,  # noqa: F401
                        xlTextQualifierDoubleQuote, xlTextQualifierNone, xlTextQualifierSingleQuote)
from .constants import (xlBottom, xlCenter, xlCenterAcrossSelection, xlDistributed, xlDown, xlFill, xlGeneral, xlJustify, xlLeft,
                        xlRight, xlToLeft, xlToRight, xlTop, xlUp, xlNone, xlLineStyleNone, xlContinuous, xlDash, xlDashDot,
                        xlDashDotDot, xlDot, xlDouble, xlSlantDashDot, xlHairline, xlThin, xlMedium, xlThick, xlEdgeLeft, xlEdgeTop,
                        xlEdgeBottom, xlEdgeRight, xlInsideVertical, xlInsideHorizontal, xlDiagonalDown, xlDiagonalUp,
                        xlUnderlineStyleNone, xlUnderlineStyleSingle, xlUnderlineStyleDouble, xlUnderlineStyleSingleAccounting,
                        xlUnderlineStyleDoubleAccounting, xlShiftDown, xlShiftToRight, xlShiftUp, xlShiftToLeft, xlAscending,
                        xlDescending, xlYes, xlGuess, xlSheetVisible, xlSheetHidden, xlSheetVeryHidden, xlCalculationAutomatic,
                        xlCalculationManual, xlCalculationSemiautomatic, xlValues, xlFormulas, xlWhole, xlByRows, xlNext, vbOKOnly, ycUser, ycPython)
from .errors import (ApplicationDefinedError, CodeInterruptedError, FormulaError, ProtectedError, SubscriptOutOfRangeError, TypeMismatchError,  # noqa: F401
                     UnsupportedError)
from . import ui as _ui_mod
from .types import PENDING, Pending, XlError, datetime_to_serial, serial_to_datetime


def _open_on_desktop(target) -> bool:
    """Open a file, folder or web page on the user's desktop (os.startfile), as Excel does. A script driving a hidden
    YCGrid (a test, an agent) opens nothing there and answers False: tests opened picture viewers over the user's
    screen."""
    try:
        if _conn().hidden:
            return False
    except YcConnectionError:
        pass
    _os.startfile(str(target))  # noqa: S606  # lesson-ok: py-startfile the one place that opens files on the desktop
    return True

_MAX_DATE_SERIAL = 2958466  # 10000-01-01: Excel's dates end on 9999-12-31

# datetime -> ext 4 (f64 serial): YCGrid writes the serial and applies a date format to cells in General format
_msgpack.ext_encoders.insert(0, (_dt.datetime, lambda d: (4, _struct.pack("<d", datetime_to_serial(d)))))
_msgpack.ext_encoders.insert(1, (_dt.date, lambda d: (4, _struct.pack("<d", datetime_to_serial(d)))))
_msgpack.ext_encoders.insert(2, (_dt.timedelta, lambda d: (5, _struct.pack("<d", datetime_to_serial(d)))))


def _rq(method: str, **params) -> Any:
    return _conn().request(method, params)


# ---------------------------------------------------------------------------
# A1 addresses
# ---------------------------------------------------------------------------
MAX_COLS = 16384


def _col_index(name: str) -> int:
    v = 0
    for ch in name.upper():
        if not ("A" <= ch <= "Z"):
            raise ApplicationDefinedError(f"Invalid column name: {name}")
        v = v * 26 + (ord(ch) - 64)
    if not 1 <= v <= MAX_COLS:
        raise ApplicationDefinedError(f"Column name out of range: {name}")
    return v - 1


def _col_name(c: int) -> str:
    s = ""
    c += 1
    while c > 0:
        c, rem = divmod(c - 1, 26)
        s = chr(65 + rem) + s
    return s


_CELL = _re.compile(r"^\$?([A-Za-z]{1,3})\$?(\d+)$")
_COLS = _re.compile(r"^\$?([A-Za-z]{1,3})$")
_ROWS = _re.compile(r"^\$?(\d+)$")
ROW_END = 0xFFFFFFFF


def _split_sheet(addr: str):
    addr = addr.strip()
    if "!" not in addr:
        return None, addr
    sheet, _, rest = addr.rpartition("!")
    if sheet.startswith("'") and sheet.endswith("'"):
        sheet = sheet[1:-1].replace("''", "'")
    return sheet, rest


def _parse_area(a: str):
    """'A1', 'A1:B2', 'A:C', '2:5' -> (r0, c0, r1, c1) (0-based; r1 = ROW_END for entire columns)."""
    parts = a.replace(" ", "").split(":")
    if len(parts) == 1:
        m = _CELL.match(parts[0])
        if not m:
            raise ApplicationDefinedError(f"Invalid address: {a}")
        r, c = int(m.group(2)) - 1, _col_index(m.group(1))
        if r < 0:
            raise ApplicationDefinedError(f"Invalid address: {a} (rows start at 1)")
        return r, c, r, c
    if len(parts) != 2:
        raise ApplicationDefinedError(f"Invalid address: {a}")
    p, q = parts
    m1, m2 = _CELL.match(p), _CELL.match(q)
    if m1 and m2:
        r0, c0 = int(m1.group(2)) - 1, _col_index(m1.group(1))
        r1, c1 = int(m2.group(2)) - 1, _col_index(m2.group(1))
        if r0 < 0 or r1 < 0:
            raise ApplicationDefinedError(f"Invalid address: {a} (rows start at 1)")
        return min(r0, r1), min(c0, c1), max(r0, r1), max(c0, c1)
    m1, m2 = _COLS.match(p), _COLS.match(q)
    if m1 and m2:
        c0, c1 = _col_index(m1.group(1)), _col_index(m2.group(1))
        return 0, min(c0, c1), ROW_END, max(c0, c1)
    m1, m2 = _ROWS.match(p), _ROWS.match(q)
    if m1 and m2:
        r0, r1 = int(m1.group(1)) - 1, int(m2.group(1)) - 1
        if r0 < 0 or r1 < 0:
            raise ApplicationDefinedError(f"Invalid address: {a} (rows start at 1)")
        return min(r0, r1), 0, max(r0, r1), MAX_COLS - 1
    raise ApplicationDefinedError(f"Invalid address: {a}")


def _a1(r: int, c: int, abs_r=True, abs_c=True) -> str:
    return ("$" if abs_c else "") + _col_name(c) + ("$" if abs_r else "") + str(r + 1)


# a whole formula that is one range (A1:A3, Sheet2!$A$1:$B$2, 1:2, C:D): FormulaR1C1 writes it the legacy way, with
# implicit intersection (Excel 16, measured: =R1C1:R3C1 reads back =@$A$1:$A$3, one value, not a spill)
_BARE_RANGE = _re.compile(r"^=((?:'[^']*(?:''[^']*)*'|[^'!(),+*/^&=<>\s-]+)!)?(?:\$?[A-Za-z]{1,3}\$?\d+:\$?[A-Za-z]{1,3}\$?\d+"
                          r"|\$?\d+:\$?\d+|\$?[A-Za-z]{1,3}:\$?[A-Za-z]{1,3})$")


def _legacy_at(formula: str) -> str:
    return "=@" + formula[1:] if _BARE_RANGE.match(formula) else formula


def _quote_sheet(name: str) -> str:
    if _re.match(r"^[A-Za-z_\u0080-￿][\w.\u0080-￿]*$", name) and not _CELL.match(name):
        return name
    return "'" + name.replace("'", "''") + "'"


# ---------------------------------------------------------------------------
# value conversion
# ---------------------------------------------------------------------------
class VArray(list):
    """2D array read from a Range: arr(i, j) is 1-based (like Excel), arr[i][j] is 0-based (like Python)."""

    def __call__(self, i: int, j: int = 1):
        return self[i - 1][j - 1]

    @property
    def Rows(self) -> int:  # noqa: N802
        return len(self)

    @property
    def Columns(self) -> int:  # noqa: N802
        return len(self[0]) if self else 0

    def ToNumpy(self, dtype="float64", Empty=float("nan")):  # noqa: N802,N803
        import numpy as np

        def conv(v):
            if v is None or isinstance(v, (str, XlError, Pending)):
                return Empty
            if isinstance(v, _dt.datetime):
                return datetime_to_serial(v)
            return float(v) if not isinstance(v, bool) else float(v)

        return np.array([[conv(v) for v in row] for row in self], dtype=dtype)


def _to_wire(v: Any) -> Any:
    """Python value -> protocol value."""
    if v is None or isinstance(v, (bool, int, float, str, _decimal.Decimal, XlError, _dt.datetime, _dt.date, _dt.timedelta)):
        if isinstance(v, float) and (_math.isnan(v) or _math.isinf(v)):
            return XlError(6)
        return v
    if isinstance(v, Range):
        return v.Value
    if hasattr(v, "item") and not hasattr(v, "__len__"):
        return v.item()  # numpy scalars
    return str(v)


def _formula_number_text(x) -> str:
    """A number as Excel's Range.Formula reads a constant (measured in Excel 16): 15 significant digits written out in
    full while that takes at most 21 characters (1E+20 is "100000000000000000000", 2.5E-07 "0.00000025"), else in
    E notation ("1E+21", "1.5E-19")."""
    x = float(x)
    if x != x or x in (float("inf"), float("-inf")):
        return str(x)
    if abs(x) < 2.2250738585072014e-308:  # zero, and the subnormal numbers Excel keeps as 0
        return "0"
    mant, exp_text = f"{x:.14e}".split("e")
    exp = int(exp_text)
    neg = mant.startswith("-")
    digits = mant.lstrip("-").replace(".", "").rstrip("0") or "0"
    if exp >= 0:
        dec = digits + "0" * (exp + 1 - len(digits)) if len(digits) <= exp + 1 else digits[:exp + 1] + "." + digits[exp + 1:]
    else:
        dec = "0." + "0" * (-exp - 1) + digits
    if len(dec) > 21:
        dec = digits[0] + ("." + digits[1:] if len(digits) > 1 else "") + ("E+" if exp >= 0 else "E-") + f"{abs(exp):02d}"
    return ("-" if neg else "") + dec


def _to_wire_2d(value: Any, rows: int, cols: int):
    """Value to write -> 2D list (scalars fill the range; 1D is one row, like Excel; any other iterable except text,
    bytes and dicts is read like a list, so generators and ranges work)."""
    if hasattr(value, "tolist") and hasattr(value, "ndim"):
        value = value.tolist()
    if not isinstance(value, (list, tuple, str, bytes, dict)) and hasattr(value, "__iter__"):
        value = [list(x) if (not isinstance(x, (list, tuple, str, bytes, dict)) and hasattr(x, "__iter__")) else x for x in value]
    if isinstance(value, (list, tuple)):
        nested = [isinstance(x, (list, tuple)) for x in value]
        if value and nested[0]:
            if not all(nested):
                raise ApplicationDefinedError("Value: give a list of rows (each a list), or a flat list, not a mix of both")
            return [[_to_wire(x) for x in row] for row in value]
        if any(nested):
            raise ApplicationDefinedError("Value: give a list of rows (each a list), or a flat list, not a mix of both")
        return [[_to_wire(x) for x in value]]
    return _to_wire(value)


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
class _Debug:
    @staticmethod
    def Print(*items):  # noqa: N802
        text = " ".join("" if i is None else str(i) for i in items)
        try:
            _conn().notify("log", {"text": text})
        except YcError:
            print(text)


Debug = _Debug()


class CommandBarControl:
    """A command in a context menu added from Python (CommandBars("Cell").Controls.Add)."""

    def __init__(self, bar, cid):
        self._bar, self._id = bar, cid

    def __repr__(self):
        return f"<CommandBarControl {self.Caption!r}>"

    def __eq__(self, other):
        return isinstance(other, CommandBarControl) and (other._bar, other._id) == (self._bar, self._id)

    def _get(self):
        def find(v):
            for c in v:
                if c["id"] == self._id:
                    return c
                hit = find(c["controls"])
                if hit:
                    return hit
            return None

        c = find(_rq("cmdbar.get", bar=self._bar))
        if c is None:
            raise ApplicationDefinedError("The control was deleted")
        return c

    def _set(self, **kw):
        _rq("cmdbar.set", bar=self._bar, id=self._id, **kw)


def _cbc_prop(key, conv=str):
    return property(lambda self: self._get()[key], lambda self, v: self._set(**{key: conv(v)}))


CommandBarControl.Caption = _cbc_prop("caption")
CommandBarControl.OnAction = _cbc_prop("on_action")
CommandBarControl.Tag = _cbc_prop("tag")
CommandBarControl.Parameter = _cbc_prop("parameter")
CommandBarControl.BeginGroup = _cbc_prop("begin_group", bool)
CommandBarControl.Enabled = _cbc_prop("enabled", bool)
CommandBarControl.Visible = _cbc_prop("visible", bool)
CommandBarControl.Type = property(lambda self: self._get()["type"])
CommandBarControl.Id = property(lambda self: self._id)
CommandBarControl.FaceId = property(lambda self: 0, lambda self, v: None)  # no icons in context menus
CommandBarControl.Controls = property(lambda self: CommandBarControls(self._bar, self._id))
CommandBarControl.Delete = lambda self, Temporary=None: _rq("cmdbar.delete", bar=self._bar, id=self._id)  # noqa: N802,N803


class CommandBarControls:
    def __init__(self, bar, parent=0):
        self._bar, self._parent = bar, parent

    def _list(self):
        v = _rq("cmdbar.get", bar=self._bar)
        if self._parent:
            def find(items):
                for c in items:
                    if c["id"] == self._parent:
                        return c["controls"]
                    hit = find(c["controls"])
                    if hit is not None:
                        return hit
                return None

            v = find(v) or []
        return v

    def Add(self, Type=1, Id=None, Parameter=None, Before=None, Temporary=None):  # noqa: N802,N803
        """Type: msoControlButton (1) or msoControlPopup (10, a submenu with its own Controls); Before: 1-based position."""
        cid = _rq("cmdbar.add", bar=self._bar, parent=self._parent, type=int(Type), parameter=str(Parameter or ""),
                  before=int(Before or 0))
        return CommandBarControl(self._bar, cid)

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def __len__(self):
        return len(self._list())

    def __iter__(self):
        return iter([CommandBarControl(self._bar, c["id"]) for c in self._list()])

    def __call__(self, Index):  # noqa: N803
        v = self._list()
        if isinstance(Index, int) and not isinstance(Index, bool):
            if not 1 <= Index <= len(v):
                raise SubscriptOutOfRangeError(f"Controls({Index})")
            return CommandBarControl(self._bar, v[Index - 1]["id"])
        for c in v:
            if c["caption"].replace("&", "") == str(Index).replace("&", ""):
                return CommandBarControl(self._bar, c["id"])
        raise SubscriptOutOfRangeError(f"No control {Index!r}")

    Item = __call__


class CommandBar:
    """A context menu Python can add commands to: "Cell", "Row", "Column" or "Ply" (sheet tabs); the added commands stay
    until Reset() or the end of the session."""

    def __init__(self, name):
        self.Name = name

    @property
    def Controls(self):  # noqa: N802
        return CommandBarControls(self.Name)

    def Reset(self):  # noqa: N802
        _rq("cmdbar.reset", bar=self.Name)

    def FindControl(self, Type=None, Id=None, Tag=None, Visible=None, Recursive=True):  # noqa: N802,N803
        def walk(items):
            for c in items:
                if (Tag is None or c["tag"] == Tag) and (Type is None or c["type"] == Type) and (Id is None or c["id"] == Id) \
                        and (Visible is None or c["visible"] == bool(Visible)):
                    return c
                if Recursive:
                    hit = walk(c["controls"])
                    if hit:
                        return hit
            return None

        c = walk(_rq("cmdbar.get", bar=self.Name))
        return CommandBarControl(self.Name, c["id"]) if c else None


class _CommandBars:
    def __call__(self, Index):  # noqa: N803
        """CommandBars("Cell"): the cell context menu ("Row", "Column", "Ply" for the others)."""
        return CommandBar(str(Index))

    Item = __call__

    @property
    def ActionControl(self):  # noqa: N802
        """The context menu command whose OnAction macro is running."""
        a = _rq("cmdbar.action", bar="")
        return CommandBarControl(a["bar"], a["id"]) if a["id"] else None

    def ExecuteMso(self, idMso: str):  # noqa: N802,N803
        _rq("ui.command", id=idMso)

    def GetEnabledMso(self, idMso: str) -> bool:  # noqa: N802,N803
        return bool(_rq("ui.command_state", id=idMso).get("enabled", False))

    def GetPressedMso(self, idMso: str) -> bool:  # noqa: N802,N803
        return bool(_rq("ui.command_state", id=idMso).get("pressed", False))

    def GetLabelMso(self, idMso: str) -> str:  # noqa: N802,N803
        return _rq("ui.command_state", id=idMso).get("label", "")


import collections as _collections

_TickBase = _collections.namedtuple("Tick", "topic value time_ns seq")
_topic_names: list = []  # Topics.Register's names by id (Tick.name), shared by every Application object
_topic_ids: dict = {}


class Tick(_TickBase):
    """One topic update for Topics.Subscribe: (topic, value, time_ns, seq) - topic is the id of
    Application.Topics.Register(name) in this process, time_ns the source's time (Push TimeNs=) or when YCGrid got it
    (ns since the epoch), seq the subscription's update number (a gap = updates dropped). .name is the topic's name."""

    __slots__ = ()

    @property
    def name(self) -> str:
        t = self.topic
        return _topic_names[t] if isinstance(t, int) and 0 <= t < len(_topic_names) else str(t)


class TopicSubscription:
    """[YC] Application.Topics.Subscribe: the callback gets lists of Tick in arrival order on its own thread (a slow
    callback delays only this subscription). Dropped counts updates lost when a queue was full (YCGrid's ring or the
    one here); Delivered, Errors and LastError tell how it goes; Unsubscribe() ends it."""

    def __init__(self, topics, names, on_tick, conflate, max_batch, capacity):
        self._topics, self.Names, self.OnTick = topics, list(names), on_tick
        self.Conflate, self.MaxBatch, self.Capacity = bool(conflate), max(1, int(max_batch)), max(1, int(capacity))
        self._ids = {n: topics.Register(n) for n in self.Names}
        self._q = _collections.deque()
        self._cv = _threading.Condition()
        self._remote_dropped = self._local_dropped = 0
        self.Delivered = self.Errors = 0
        self.LastError = ""
        self.Id = None
        self._active = True
        self._worker = _threading.Thread(target=self._run, daemon=True, name="yc-topics")
        self._worker.start()

    @property
    def Dropped(self):  # noqa: N802
        return self._remote_dropped + self._local_dropped

    @property
    def Active(self):  # noqa: N802
        return self._active

    def _put(self, ticks, dropped):
        ids = self._ids
        batch = [Tick(ids.get(t[0], t[0]), _from_wire(t[1]), t[2], t[3]) for t in ticks]
        with self._cv:
            self._remote_dropped = max(self._remote_dropped, int(dropped or 0))
            self._q.extend(batch)
            over = len(self._q) - self.Capacity
            if over > 0:
                for _ in range(over):
                    self._q.popleft()
                self._local_dropped += over
            self._cv.notify()

    def _run(self):
        while True:
            with self._cv:
                while self._active and not self._q:
                    self._cv.wait()
                if not self._active and not self._q:
                    return
                n = min(len(self._q), self.MaxBatch)
                batch = [self._q.popleft() for _ in range(n)]
            try:
                self.OnTick(batch)
            except Exception as e:  # noqa: BLE001 - a broken callback mustn't stop the subscription
                self.Errors += 1
                self.LastError = f"{type(e).__name__}: {e}"
                try:
                    from . import host

                    host._report(f"Topics.Subscribe {getattr(self.OnTick, '__name__', '')}", e)
                except Exception:  # noqa: BLE001
                    pass
            self.Delivered += len(batch)

    def Unsubscribe(self):  # noqa: N802
        if self.Id is not None:
            try:
                _rq("topic.unsubscribe", id=self.Id)
            except Exception:  # noqa: BLE001 - YCGrid already gone
                pass
            self._topics._subs.pop(self.Id, None)
        with self._cv:
            self._active = False
            self._cv.notify()

    def __repr__(self):
        return f"<TopicSubscription {self.Id} {self.Names} delivered={self.Delivered} dropped={self.Dropped}>"


class _Topics:
    """Real-time quote hub (simplified): name <-> id; Push updates the value of =QUOTE("name")."""

    def __init__(self):
        self._ids: dict[str, int] = _topic_ids
        self._names: list[str] = _topic_names
        self._lock = _threading.Lock()
        self._subs: dict = {}  # subscription id -> TopicSubscription

    def Subscribe(self, Names, OnTick, Conflate=False, MaxBatch=1000, Capacity=1 << 20):  # noqa: N802,N803
        """[YC] Every update of these topics (pushed by feeds, scripts or a replay), in order, to OnTick(batch) on a
        worker thread of the host: batch is a list of Tick (topic_id, value, time_ns, seq; .name). Conflate=True:
        only the latest value per topic per cycle (about 20 ms). MaxBatch: ticks per call; Capacity: the queue
        kept while the callback catches up (the oldest are dropped beyond it and counted in Dropped). Only in the
        YCGrid Python host (modules of a project). Returns a TopicSubscription (Unsubscribe())."""
        from . import host

        conn = host._conn_obj
        if conn is None:
            raise ApplicationDefinedError("Topics.Subscribe runs in the YCGrid Python host (a module of the project)")
        names = [Names] if isinstance(Names, str) else [str(n) for n in Names]
        if not names:
            raise ApplicationDefinedError("Topics.Subscribe needs at least one topic name")
        conn.urgent["topic.ticks"] = self._ticks
        sub = TopicSubscription(self, names, OnTick, Conflate, MaxBatch, Capacity)
        sub.Id = int(_rq("topic.subscribe", topics=names, conflate=bool(Conflate), max_batch=int(MaxBatch), capacity=int(Capacity)))
        self._subs[sub.Id] = sub
        return sub

    @property
    def Subscriptions(self):  # noqa: N802
        """[YC] The live TopicSubscription objects of this host."""
        return list(self._subs.values())

    def _ticks(self, p):
        """topic.ticks from YCGrid (on the connection's reader thread): queue the batch for its subscription."""
        sub = self._subs.get((p or {}).get("sub"))
        if sub is not None:
            sub._put(p.get("ticks") or [], p.get("dropped", 0))

    def Register(self, Names, Type=None):  # noqa: N802,N803
        single = isinstance(Names, str)
        names = [Names] if single else list(Names)
        out = []
        with self._lock:
            for n in names:
                if n not in self._ids:
                    self._ids[n] = len(self._names)
                    self._names.append(n)
                out.append(self._ids[n])
        return out[0] if single else out

    def Lookup(self, Name):  # noqa: N802,N803
        return self._ids.get(Name)

    @property
    def Count(self):  # noqa: N802
        return len(self._names)

    @staticmethod
    def _flat(x):
        """ids / values as a flat list (scalars, lists, nested lists, numpy arrays of any shape)."""
        if hasattr(x, "ravel"):
            return x.ravel().tolist()
        if isinstance(x, (list, tuple)):
            out = []
            for v in x:
                if isinstance(v, (list, tuple)) or hasattr(v, "ravel"):
                    out.extend(_Topics._flat(v))
                else:
                    out.append(v)
            return out
        return [x]

    def _name(self, i):
        return self._names[i] if isinstance(i, int) and 0 <= i < len(self._names) else str(i)

    @property
    def Stats(self):  # noqa: N802
        """[YC] dict: UpdatesPerSecond (since the previous Stats call, or over half a second the first time), TotalUpdates,
        Topics (topics YCGrid knows), BoundCells (cells bound with Range.BindTopics)."""
        import time as _time

        now, s = _time.perf_counter(), _rq("topic.stats")
        prev = getattr(self, "_stats_prev", None)
        if prev is None:
            _time.sleep(0.5)
            prev, (now, s) = (now, s["updates"]), (_time.perf_counter(), _rq("topic.stats"))
        rate = (s["updates"] - prev[1]) / max(1e-9, now - prev[0])
        self._stats_prev = (now, s["updates"])
        subs = [{"Id": x["id"], "Topics": x["topics"], "Conflate": x["conflate"], "Queued": x["queued"],
                 "Delivered": x["delivered"], "Dropped": x["dropped"], "Capacity": x["capacity"]} for x in s.get("subscriptions", [])]
        return {"UpdatesPerSecond": rate, "TotalUpdates": s["updates"], "Topics": s["topics"], "BoundCells": s["bound_cells"],
                "Subscriptions": subs}

    def Push(self, Ids, Values, TimeNs=None):  # noqa: N802,N803
        """Update topics (=QUOTE cells, bound cells, Topics.Subscribe callbacks). [YC] TimeNs: the source's time of
        the updates in ns since the epoch (one number, or one per update), passed to subscribers as time_ns."""
        if isinstance(Ids, (list, tuple)) or (hasattr(Ids, "tolist") and hasattr(Ids, "ndim")):
            ids = self._flat(Ids)
            vals = self._flat(Values)
        else:
            ids, vals = [Ids], [Values]
        times = None
        if TimeNs is not None:
            times = self._flat(TimeNs) if hasattr(TimeNs, "__len__") or hasattr(TimeNs, "ravel") else [TimeNs] * len(ids)
        items = []
        for k, (i, v) in enumerate(zip(ids, vals)):
            if times is not None and k < len(times):
                items.append(["QUOTE|" + self._name(i), _to_wire(v), int(times[k])])
            else:
                items.append(["QUOTE|" + self._name(i), _to_wire(v)])
        _conn().notify("stream.push", {"items": items})

    def SetError(self, Id, ErrorText="#CONNECT!"):  # noqa: N802,N803
        from .types import ERROR_TEXT

        code = next((k for k, t in ERROR_TEXT.items() if t == ErrorText), 12)
        self.Push(Id, XlError(code))


class _Dialog:
    def __init__(self, number):
        self._n = int(number)

    def Show(self, *args):  # noqa: N802
        """Open the built-in dialog and wait until it is closed (arguments are ignored)."""
        _rq("ui.dialog_show", dialog=self._n)
        import time

        while _rq("ui.dialog_open"):
            time.sleep(0.05)
        return True


class _Dialogs:
    def __call__(self, Index):  # noqa: N803
        return _Dialog(Index)

    Item = __call__


class _AutoRecover:
    def _g(self):
        return _rq("autorecover.get")

    @property
    def Enabled(self):  # noqa: N802
        return bool(self._g()["enabled"])

    @Enabled.setter
    def Enabled(self, v):  # noqa: N802
        _rq("autorecover.set", enabled=bool(v))

    @property
    def Time(self):  # noqa: N802
        return int(self._g()["minutes"])

    @Time.setter
    def Time(self, v):  # noqa: N802
        _rq("autorecover.set", minutes=int(v))

    @property
    def Path(self):  # noqa: N802
        return self._g()["path"]

    @Path.setter
    def Path(self, v):  # noqa: N802
        _rq("autorecover.set", path=str(v))


class _CellError:
    """Range.Errors(index): one error checking rule of a cell (Excel's Error object)."""

    def __init__(self, rng, index):
        self._r, self._i = rng, int(index)

    @property
    def Value(self):  # noqa: N802
        a = self._r._a
        return self._i in _rq("cell.error_check", sheet=self._r._sheet, row=a[0], col=a[1]).get("rules", [])

    @property
    def Ignore(self):  # noqa: N802
        a = self._r._a
        return bool(_rq("cell.error_check", sheet=self._r._sheet, row=a[0], col=a[1])["ignored"])

    @Ignore.setter
    def Ignore(self, v):  # noqa: N802
        for a in self._r._areas:
            _rq("cell.ignore_errors", sheet=self._r._sheet, rect=list(a), on=bool(v))


class _CellErrors:
    """Range.Errors: Errors(xlNumberAsText), Errors.Item(xlEvaluateToError)."""

    def __init__(self, rng):
        self._r = rng

    def __call__(self, index):
        return _CellError(self._r, index)

    def Item(self, index):  # noqa: N802
        return _CellError(self._r, index)


class _ErrorCheckingOptions:
    """Application.ErrorCheckingOptions (File -> Options -> Formulas -> Error checking; saved for this computer)."""

    _keys = {"BackgroundChecking": "background", "EvaluateToError": "evaluate_to_error", "NumberAsText": "number_as_text",
             "InconsistentFormula": "inconsistent_formula", "TextDate": "text_date", "OmittedCells": "omitted_cells",
             "UnlockedFormulaCells": "unlocked_formula", "EmptyCellReferences": "empty_cell_reference",
             "ListDataValidation": "list_data_validation", "InconsistentTableFormula": "inconsistent_list_formula",
             "MisleadingNumberFormats": "misleading_format"}

    def __getattr__(self, name):
        if name in _ErrorCheckingOptions._keys:
            return bool(_rq("app.error_checking")[_ErrorCheckingOptions._keys[name]])
        if name == "IndicatorColorIndex":
            return -4105  # xlColorIndexAutomatic: Excel's green
        raise AttributeError(name)

    def __setattr__(self, name, v):
        if name in _ErrorCheckingOptions._keys:
            _rq("app.error_checking", **{_ErrorCheckingOptions._keys[name]: bool(v)})
        elif name == "IndicatorColorIndex":
            pass
        else:
            raise AttributeError(name)


class ByRef:
    """[YC] An event argument a handler sets, as Excel's ByRef Cancel: `Cancel.Value = True` cancels the action
    (Workbook_BeforeSave, Worksheet_BeforeDoubleClick...), and so does `return True` from the handler. Python can't
    rebind the caller's variable, so `Cancel = True` alone changes nothing; `bool(Cancel)` reads it."""

    __slots__ = ("Value",)

    def __init__(self, Value=False):  # noqa: N803
        self.Value = Value

    def __bool__(self):
        return bool(self.Value)

    def __repr__(self):
        return f"ByRef({self.Value!r})"


class ProtectedViewWindow:
    """A window showing a workbook in Protected View (a file from the Internet, an unsafe location or an Outlook
    attachment): read-only, its Python code off, until Edit (Enable Editing)."""

    def __init__(self, info):
        self._info = info

    @property
    def Caption(self):  # noqa: N802
        import os

        return os.path.basename(self._info.get("path", "")) + " [Protected View]"

    @property
    def SourceName(self):  # noqa: N802
        import os

        return os.path.basename(self._info.get("path", ""))

    @property
    def SourcePath(self):  # noqa: N802
        import os

        return os.path.dirname(self._info.get("path", ""))

    @property
    def Workbook(self):  # noqa: N802
        """The workbook, read-only while in Protected View (writes raise)."""
        return ThisWorkbook

    def Edit(self, WriteResPassword=None, UpdateLinks=None):  # noqa: N802,N803
        """Enable Editing: the workbook becomes editable (its Python code is then decided by the Trust Center, as for any
        workbook) and is returned, as Excel's ProtectedViewWindow.Edit."""
        _rq("app.protected_view", edit=True)
        return ThisWorkbook

    def Close(self):  # noqa: N802
        ThisWorkbook.Close(SaveChanges=False)


class _ProtectedViewWindows:
    """Application.ProtectedViewWindows: the open Protected View windows (this YCGrid's, as each window holds one
    workbook). Workbooks.Open never opens in Protected View, as in Excel."""

    def _list(self):
        info = _rq("app.protected_view")
        return [ProtectedViewWindow(info)] if info.get("active") else []

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def Item(self, Index):  # noqa: N802,N803
        items = self._list()
        if not isinstance(Index, int) or not 1 <= Index <= len(items):
            raise SubscriptOutOfRangeError(f"No Protected View window {Index}")
        return items[Index - 1]

    __call__ = Item

    def __iter__(self):
        return iter(self._list())

    def __len__(self):
        return self.Count


class _TrustCenter:
    """[YC] Application.TrustCenter: the Trust Center Settings dialog's choices (File -> Options -> Trust Center)."""

    _MACRO_TO_UI = {2: 0, 4: 1, 1: 2}  # Excel's numbers -> the dialog's order in YCGrid

    def _get(self, **kw):
        return _rq("app.trust_center", **kw)

    @property
    def MacroSettings(self):  # noqa: N802
        """ycMacroDisableWithNotification (the default: untrusted code is disabled and the Message Bar says so),
        ycMacroDisableWithoutNotification or ycMacroEnableAll."""
        ui = self._get()["macro_setting"]
        return {v: k for k, v in self._MACRO_TO_UI.items()}[ui]

    @MacroSettings.setter
    def MacroSettings(self, v):  # noqa: N802
        if v not in self._MACRO_TO_UI:
            raise ValueError("MacroSettings: ycMacroDisableWithNotification, ycMacroDisableWithoutNotification or ycMacroEnableAll")
        self._get(macro_setting=self._MACRO_TO_UI[v])

    def _flag(name, doc):  # noqa: N805
        def fget(self):
            return bool(self._get()[name])

        def fset(self, v):
            self._get(**{name: bool(v)})

        return property(fget, fset, doc=doc)

    MessageBar = _flag("message_bar", "Show the Message Bar when Python code has been disabled (False: never show it).")
    ProtectedViewInternet = _flag("pv_internet", "Enable Protected View for files originating from the Internet.")
    ProtectedViewUnsafeLocations = _flag("pv_unsafe_locations", "Enable Protected View for files located in potentially unsafe locations.")
    ProtectedViewOutlook = _flag("pv_outlook", "Enable Protected View for Outlook attachments.")
    AllowNetworkLocations = _flag("allow_network_locations", "Allow Trusted Locations on my network (not recommended).")
    DisableTrustedLocations = _flag("disable_trusted_locations", "Disable all Trusted Locations.")
    AllowNetworkDocuments = _flag("allow_network_documents", "Allow documents on a network to be trusted.")
    DisableTrustedDocuments = _flag("disable_trusted_documents", "Disable Trusted Documents (Enable Content then enables for this session only).")
    del _flag

    @property
    def TrustedLocations(self):  # noqa: N802
        """The trusted folders, as dicts {Path, Subfolders, Description, DateModified}."""
        return [{"Path": x["path"], "Subfolders": x["subfolders"], "Description": x["description"], "DateModified": x["modified"]}
                for x in self._get()["locations"]]

    def AddTrustedLocation(self, Path, Subfolders=False, Description=""):  # noqa: N802,N803
        """Add new location: workbooks in this folder (and its subfolders with Subfolders=True) run their code without
        asking."""
        import os

        self._get(add_location=os.path.abspath(str(Path)), subfolders=bool(Subfolders), description=str(Description))

    def RemoveTrustedLocation(self, Path):  # noqa: N802,N803
        import os

        p = str(Path)
        self._get(remove_location=p if "%" in p else os.path.abspath(p))

    @property
    def TrustedDocuments(self):  # noqa: N802
        """The project folders made Trusted Documents (Enable Content), lower case."""
        return list(self._get()["documents"])

    def ClearTrustedDocuments(self):  # noqa: N802
        """Trusted Documents > Clear: none of them is trusted any more (workbooks open now keep running)."""
        self._get(clear_documents=True)


class _Application:
    Name = "YCGrid"
    _enable_events = True
    _caller = _threading.local()

    def __init__(self):
        self.CommandBars = _CommandBars()
        self.Topics = _Topics()
        self.DisplayAlerts = True
        self.ScreenUpdating = True

    # ---- Objects ----
    @property
    def ThisWorkbook(self):  # noqa: N802
        return ThisWorkbook

    # ---- custom lists (File > Options > Edit Custom Lists; saved in settings.json for every workbook) ----
    def _lists(self):
        r = _rq("lists.get")
        return r["lists"], r["builtin"]

    @property
    def CustomListCount(self):  # noqa: N802
        return len(self._lists()[0])

    def GetCustomListContents(self, ListNum):  # noqa: N802,N803
        lists, _ = self._lists()
        if not 1 <= int(ListNum) <= len(lists):
            raise SubscriptOutOfRangeError(f"GetCustomListContents({ListNum})")
        return tuple(lists[int(ListNum) - 1])

    def GetCustomListNum(self, ListArray):  # noqa: N802,N803
        """The number of the list with exactly these entries (0 = none)."""
        want = [str(x) for x in (ListArray.Value if isinstance(ListArray, Range) else ListArray)]
        if want and isinstance(want[0], (list, tuple)):
            want = [str(x) for row in want for x in row]
        for i, lst in enumerate(self._lists()[0]):
            if [x.lower() for x in lst] == [x.lower() for x in want]:
                return i + 1
        return 0

    def AddCustomList(self, ListArray, ByRow=False):  # noqa: N802,N803
        """A list from a sequence of texts or from a Range (ByRow: read the range row by row)."""
        if isinstance(ListArray, Range):
            vals = ListArray.Value
            vals = vals if isinstance(vals, list) else [[vals]]
            if ByRow:
                items = [v for row in vals for v in row]
            else:
                items = [vals[r][c] for c in range(len(vals[0])) for r in range(len(vals))]
        else:
            items = list(ListArray)
        items = [str(v) for v in items if v is not None and str(v) != ""]
        if len(items) < 2:
            raise ApplicationDefinedError("A custom list needs at least two entries")
        if self.GetCustomListNum(items):
            return
        lists, builtin = self._lists()
        _rq("lists.set", user=lists[builtin:] + [items])

    def DeleteCustomList(self, ListNum):  # noqa: N802,N803
        lists, builtin = self._lists()
        n = int(ListNum)
        if n <= builtin:
            raise ApplicationDefinedError("Built-in lists can't be deleted")
        if n > len(lists):
            raise SubscriptOutOfRangeError(f"DeleteCustomList({ListNum})")
        user = lists[builtin:]
        del user[n - builtin - 1]
        _rq("lists.set", user=user)

    @property
    def Dialogs(self):  # noqa: N802
        """Dialogs(xlDialogFormatNumber).Show() and the other built-in dialogs."""
        return _Dialogs()

    @property
    def AutoRecover(self):  # noqa: N802
        """Enabled, Time (minutes), Path: File > Options > Save."""
        return _AutoRecover()

    def CheckSpelling(self, Word, CustomDictionary=None, IgnoreUppercase=False):  # noqa: N802,N803
        """True when the word (or text) is spelled correctly (Windows spell checker, English by default)."""
        w = str(Word)
        if IgnoreUppercase and w.isupper():
            return True
        return bool(_rq("spell.check", word=w, lang=_spell_lang)["ok"])

    def GetSpellingSuggestions(self, Word):  # noqa: N802,N803
        """[YC] Suggested spellings of a word (empty when it is spelled correctly)."""
        return list(_rq("spell.check", word=str(Word), lang=_spell_lang)["suggestions"])

    @property
    def UserName(self):  # noqa: N802
        """The author name written on new comments (defaults to the Windows user name)."""
        global _user_name
        if _user_name is None:
            import os

            _user_name = os.environ.get("USERNAME") or os.environ.get("USER") or "Python"
        return _user_name

    @UserName.setter
    def UserName(self, v):  # noqa: N802
        global _user_name
        _user_name = str(v)

    ActiveWorkbook = ThisWorkbook

    @property
    def Workbooks(self):  # noqa: N802
        return _Workbooks()

    @property
    def Worksheets(self):  # noqa: N802
        return Worksheets

    @property
    def Sheets(self):  # noqa: N802
        return Sheets

    @property
    def Charts(self):  # noqa: N802
        return Charts

    @property
    def ActiveSheet(self):  # noqa: N802
        # view.focus is answered without waiting for a UI frame (view.get takes one frame, ~8 ms)
        sid = _rq("view.focus")
        for s in _sheets_info():
            if s["id"] == sid:
                return _sheet_object(s)
        return Worksheet(sid)

    @property
    def ActiveChart(self):  # noqa: N802
        """The active chart sheet's chart (selected embedded charts: ChartObjects(...).Chart)."""
        sh = self.ActiveSheet
        return sh if isinstance(sh, ChartSheet) else None

    @property
    def ActiveWindow(self):  # noqa: N802
        return Window(None)

    @property
    def ActiveCell(self):  # noqa: N802
        v = _rq("view.get")
        r, c = v["active"]
        return Range._from(v["focus_sheet"], [(r, c, r, c)])

    @property
    def Selection(self):  # noqa: N802
        v = _rq("view.get")
        return Range._from(v["focus_sheet"], [tuple(a) for a in v["areas"]])

    @property
    def Names(self):  # noqa: N802
        return Names

    @property
    def WorksheetFunction(self):  # noqa: N802
        return WorksheetFunction

    @property
    def Version(self):  # noqa: N802
        return _rq("hello", role="query").get("app_version", "")

    @property
    def Path(self):  # noqa: N802
        return _rq("hello", role="query").get("exe_dir", "")

    @property
    def Caller(self):  # noqa: N802
        return getattr(self._caller, "range", None)

    # ---- Settings ----
    @property
    def EnableEvents(self):  # noqa: N802
        return _Application._enable_events

    @EnableEvents.setter
    def EnableEvents(self, v):  # noqa: N802
        _Application._enable_events = bool(v)

    @property
    def Watches(self):  # noqa: N802
        return _Watches()

    @property
    def Calculation(self):  # noqa: N802
        c = _rq("calc.get")
        if not c["auto"]:
            return xlCalculationManual
        return xlCalculationAutomatic if c.get("data_tables", True) else xlCalculationSemiautomatic

    @Calculation.setter
    def Calculation(self, v):  # noqa: N802
        _rq("calc.set", auto=v != xlCalculationManual, data_tables=v != xlCalculationSemiautomatic)

    @property
    def CalculationState(self):  # noqa: N802
        """xlDone (0) when nothing waits to be calculated, xlPending (2) when formulas wait (manual calculation); a
        recalculation runs on the engine thread and is done by the time a request returns, so never xlCalculating."""
        return 2 if _rq("calc.get").get("pending") else 0

    _active_printer = ""

    @property
    def ActivePrinter(self):  # noqa: N802
        """The printer PrintOut uses (the Windows default printer until set)."""
        return type(self)._active_printer or _rq("print.printers")["default"]

    @ActivePrinter.setter
    def ActivePrinter(self, v):  # noqa: N802
        name = str(v or "")
        if " on " in name and name not in _rq("print.printers")["printers"]:
            name = name.rsplit(" on ", 1)[0]  # Excel's "Printer on Ne01:"
        type(self)._active_printer = name

    @property
    def Printers(self):  # noqa: N802
        """[YC] The names of the installed printers."""
        return list(_rq("print.printers")["printers"])

    def InchesToPoints(self, Inches):  # noqa: N802,N803
        return float(Inches) * 72.0

    def CentimetersToPoints(self, Centimeters):  # noqa: N802,N803
        return float(Centimeters) * 72.0 / 2.54

    @property
    def AutoCorrect(self):  # noqa: N802
        return _AutoCorrect()

    @property
    def ReferenceStyle(self):  # noqa: N802
        """xlA1 or xlR1C1 (File -> Options; saved for this computer)."""
        return xlR1C1 if _rq("app.reference_style") else xlA1

    @ReferenceStyle.setter
    def ReferenceStyle(self, v):  # noqa: N802
        _rq("app.reference_style", r1c1=v == xlR1C1)

    @property
    def FindFormat(self):  # noqa: N802
        """The format Range.Find / Replace look for with SearchFormat=True (Find and Replace > Format...): set its
        Font, Interior, Borders, NumberFormat, alignment and protection; Clear() empties it."""
        return _find_format

    @property
    def ReplaceFormat(self):  # noqa: N802
        """The format Range.Replace writes with ReplaceFormat=True (Find and Replace > Replace with > Format...)."""
        return _replace_format

    @property
    def CutCopyMode(self):  # noqa: N802
        """xlCopy or xlCut while copied cells are outlined (Paste or Enter pastes them), else False. Setting False ends
        it, as Esc does."""
        return {1: xlCopy, 2: xlCut}.get(_rq("app.cut_copy_mode"), False)

    @CutCopyMode.setter
    def CutCopyMode(self, v):  # noqa: N802
        if not v:
            _rq("app.cut_copy_mode", off=True)

    def _edit_options(self, **kw):
        return _rq("app.edit_options", **kw)

    @property
    def MoveAfterReturn(self):  # noqa: N802
        """Enter moves the selection after an entry (File -> Options; saved for this computer)."""
        return bool(self._edit_options()["move_after_return"])

    @MoveAfterReturn.setter
    def MoveAfterReturn(self, v):  # noqa: N802
        self._edit_options(move_after_return=bool(v))

    @property
    def MoveAfterReturnDirection(self):  # noqa: N802
        """xlDown, xlToRight, xlUp or xlToLeft: where Enter moves (Shift+Enter: the other way)."""
        return (xlDown, xlToRight, xlUp, xlToLeft)[self._edit_options()["direction"]]

    @MoveAfterReturnDirection.setter
    def MoveAfterReturnDirection(self, v):  # noqa: N802
        self._edit_options(direction={xlDown: 0, xlToRight: 1, xlUp: 2, xlToLeft: 3}[v])

    @property
    def ShowQuickAnalysis(self):  # noqa: N802
        """The Quick Analysis button at a selection's corner (File > Options > General; Ctrl+Q works either way)."""
        return bool(self._edit_options()["show_quick_analysis"])

    @ShowQuickAnalysis.setter
    def ShowQuickAnalysis(self, v):  # noqa: N802
        self._edit_options(show_quick_analysis=bool(v))

    @property
    def ShowChartTipNames(self):  # noqa: N802
        """File > Options > Advanced > Chart: the tip over a chart names the element under the mouse."""
        return bool(self._edit_options()["show_chart_tip_names"])

    @ShowChartTipNames.setter
    def ShowChartTipNames(self, v):  # noqa: N802
        self._edit_options(show_chart_tip_names=bool(v))

    @property
    def ShowChartTipValues(self):  # noqa: N802
        """File > Options > Advanced > Chart: the tip over a chart's point shows its value."""
        return bool(self._edit_options()["show_chart_tip_values"])

    @ShowChartTipValues.setter
    def ShowChartTipValues(self, v):  # noqa: N802
        self._edit_options(show_chart_tip_values=bool(v))

    @property
    def CalculateBeforeSave(self):  # noqa: N802
        """File > Options > Formulas: in manual calculation a save calculates first, so the file has calculated values."""
        return bool(self._edit_options()["calculate_before_save"])

    @CalculateBeforeSave.setter
    def CalculateBeforeSave(self, v):  # noqa: N802
        self._edit_options(calculate_before_save=bool(v))

    @property
    def FormatStaleValues(self):  # noqa: N802
        """File > Options > Formulas: in manual calculation the cells waiting to be calculated show struck through."""
        return bool(self._edit_options()["format_stale_values"])

    @FormatStaleValues.setter
    def FormatStaleValues(self, v):  # noqa: N802
        self._edit_options(format_stale_values=bool(v))

    @property
    def EnableLargeOperationAlert(self):  # noqa: N802
        """File > Options > Advanced: ask before a paste or a Fill Down / Right over more than LargeOperationCellThousandCount thousand
        cells (a person's; scripts are never asked)."""
        return bool(self._edit_options()["large_operation_alert"])

    @EnableLargeOperationAlert.setter
    def EnableLargeOperationAlert(self, v):  # noqa: N802
        self._edit_options(large_operation_alert=bool(v))

    @property
    def LargeOperationCellThousandCount(self):  # noqa: N802
        """How many thousand cells a paste or a Fill Down / Right reaches before EnableLargeOperationAlert asks (Excel's 33554)."""
        return int(self._edit_options()["large_operation_thousands"])

    @LargeOperationCellThousandCount.setter
    def LargeOperationCellThousandCount(self, v):  # noqa: N802
        self._edit_options(large_operation_thousands=int(v))

    @property
    def GenerateGetPivotData(self):  # noqa: N802
        """File > Options > Formulas: pointing at a PivotTable's value cell while typing a formula writes GETPIVOTDATA(...)
        instead of the cell's address."""
        return bool(self._edit_options()["generate_getpivotdata"])

    @GenerateGetPivotData.setter
    def GenerateGetPivotData(self, v):  # noqa: N802
        self._edit_options(generate_getpivotdata=bool(v))

    @property
    def GenerateTableRefs(self):  # noqa: N802
        """File > Options > Formulas > Use table names in formulas: pointing at a table's column, row or header while
        typing a formula writes Table1[Px], Table1[@Qty]... instead of the cells' address. xlGenerateTableRefsStruct (1)
        when on, xlGenerateTableRefsA1 (0) when off, as Excel's."""
        return 1 if self._edit_options()["generate_table_refs"] else 0

    @GenerateTableRefs.setter
    def GenerateTableRefs(self, v):  # noqa: N802
        self._edit_options(generate_table_refs=bool(v))

    @property
    def AutoFormatAsYouTypeReplaceHyperlinks(self):  # noqa: N802
        """AutoCorrect > AutoFormat As You Type > Internet and network paths with hyperlinks: a web address
        (http://, https://, ftp://, www.) or an e-mail address typed into a cell becomes a link, in an Undo step of its
        own (Excel's default: on). Only typing: Range.Value from Python never makes one."""
        return bool(self._edit_options()["autoformat_hyperlinks"])

    @AutoFormatAsYouTypeReplaceHyperlinks.setter
    def AutoFormatAsYouTypeReplaceHyperlinks(self, v):  # noqa: N802
        self._edit_options(autoformat_hyperlinks=bool(v))

    @property
    def AlertBeforeOverwriting(self):  # noqa: N802
        """Ask before cells dragged onto others replace what they hold (File > Options > Advanced)."""
        return bool(self._edit_options()["alert_before_overwriting"])

    @AlertBeforeOverwriting.setter
    def AlertBeforeOverwriting(self, v):  # noqa: N802
        self._edit_options(alert_before_overwriting=bool(v))

    @property
    def StandardFont(self):  # noqa: N802
        """The font of new workbooks (File > Options > General)."""
        return self._edit_options()["standard_font"]

    @StandardFont.setter
    def StandardFont(self, v):  # noqa: N802
        self._edit_options(standard_font=str(v))

    @property
    def StandardFontSize(self):  # noqa: N802
        return float(self._edit_options()["standard_font_size"])

    @StandardFontSize.setter
    def StandardFontSize(self, v):  # noqa: N802
        self._edit_options(standard_font_size=float(v))

    @property
    def SheetsInNewWorkbook(self):  # noqa: N802
        """How many sheets a new workbook has (File > Options > General)."""
        return int(self._edit_options()["sheets_in_new_workbook"])

    @SheetsInNewWorkbook.setter
    def SheetsInNewWorkbook(self, v):  # noqa: N802
        self._edit_options(sheets_in_new_workbook=int(v))

    @property
    def EditDirectlyInCell(self):  # noqa: N802
        """F2 and a double-click edit in the cell (False: in the formula bar)."""
        return bool(self._edit_options()["edit_directly_in_cell"])

    @EditDirectlyInCell.setter
    def EditDirectlyInCell(self, v):  # noqa: N802
        self._edit_options(edit_directly_in_cell=bool(v))

    @property
    def EnableAutoComplete(self):  # noqa: N802
        """Typing the start of a text already in the column finishes it."""
        return bool(self._edit_options()["enable_autocomplete"])

    @EnableAutoComplete.setter
    def EnableAutoComplete(self, v):  # noqa: N802
        self._edit_options(enable_autocomplete=bool(v))

    @property
    def ExtendList(self):  # noqa: N802
        """File > Options > Advanced > Extend data range formats and formulas: a value typed in the row under a list
        takes the format three or more rows above share, and the row's empty cells take the formula four of the five
        rows above hold, once every cell of the row it reads holds something (as Excel's). Typing only."""
        return bool(self._edit_options()["extend_list"])

    @ExtendList.setter
    def ExtendList(self, v):  # noqa: N802
        self._edit_options(extend_list=bool(v))

    @property
    def FlashFill(self):  # noqa: N802
        """File > Options > Advanced > Automatically Flash Fill: grey suggestions down the column while typing."""
        return bool(self._edit_options()["auto_flash_fill"])

    @FlashFill.setter
    def FlashFill(self, v):  # noqa: N802
        self._edit_options(auto_flash_fill=bool(v))

    @property
    def AutomationSecurity(self):  # noqa: N802
        """How a workbook opened by a script (Workbooks.Open, yc.launch, the MCP tools) treats its Python code:
        msoAutomationSecurityLow (the default, as in Excel: it runs), msoAutomationSecurityByUI (the Trust Center
        decides, as for a workbook a person opens: untrusted code stays disabled and the Message Bar offers Enable
        Content) or msoAutomationSecurityForceDisable (it never runs). Not saved: each YCGrid starts with Low (or
        yc.launch's automation_security)."""
        return int(_rq("app.automation_security"))

    @AutomationSecurity.setter
    def AutomationSecurity(self, v):  # noqa: N802
        _rq("app.automation_security", value=int(v))

    @property
    def ProtectedViewWindows(self):  # noqa: N802
        """The Protected View windows: a workbook a person opened from the Internet (the Mark of the Web), a browser
        cache or an Outlook attachment folder shows read-only with its code off. `.Item(1).Edit()` enables editing and
        returns the Workbook. Scripts' Workbooks.Open never opens in Protected View."""
        return _ProtectedViewWindows()

    @property
    def TrustCenter(self):  # noqa: N802
        """[YC] File -> Options -> Trust Center Settings: Trusted Locations, Trusted Documents, Macro Settings and the
        Message Bar (Excel keeps these in the registry, with no object model). Saved for this computer; a hidden
        YCGrid starts from the defaults and keeps its changes in memory."""
        return _TrustCenter()

    @property
    def CellDragAndDrop(self):  # noqa: N802
        """Dragging the selection's border moves the cells and the fill handle fills (File -> Options; saved for this
        computer, so an app that turns it off sets it back in Workbook_BeforeClose). A protected sheet offers neither."""
        return bool(_rq("app.cell_drag_drop"))

    @CellDragAndDrop.setter
    def CellDragAndDrop(self, v):  # noqa: N802
        _rq("app.cell_drag_drop", on=bool(v))

    @property
    def TruncateTo15Digits(self):  # noqa: N802
        """[YC] File -> Options -> Calculation: "Truncate numbers longer than 15 significant digits like Excel". True: a
        typed number's digits past the 15th become 0, as in Excel; False (the default): it is kept exactly as a Decimal.
        Saved for this computer."""
        return bool(_rq("app.truncate_15_digits"))

    @TruncateTo15Digits.setter
    def TruncateTo15Digits(self, v):  # noqa: N802
        _rq("app.truncate_15_digits", on=bool(v))

    @property
    def Iteration(self):  # noqa: N802
        """Iterative calculation of circular references (File -> Options)."""
        return bool(_rq("calc.get")["iterate"])

    @Iteration.setter
    def Iteration(self, v):  # noqa: N802
        _rq("calc.set", iterate=bool(v))

    @property
    def MaxIterations(self):  # noqa: N802
        return int(_rq("calc.get")["max_iterations"])

    @MaxIterations.setter
    def MaxIterations(self, v):  # noqa: N802
        _rq("calc.set", max_iterations=int(v))

    @property
    def MaxChange(self):  # noqa: N802
        return float(_rq("calc.get")["max_change"])

    @MaxChange.setter
    def MaxChange(self, v):  # noqa: N802
        _rq("calc.set", max_change=float(v))

    @property
    def PublishInterval(self):  # noqa: N802
        """[YC] Minimum ms between screen updates caused by Python / live data (0 = every change); this session only."""
        return int(_rq("engine.publish_interval"))

    @PublishInterval.setter
    def PublishInterval(self, v):  # noqa: N802
        _rq("engine.publish_interval", ms=int(v))

    @property
    def StatusBar(self):  # noqa: N802
        return _rq("ui.status_get").get("text", False) or False

    @StatusBar.setter
    def StatusBar(self, v):  # noqa: N802
        _rq("ui.status", text="" if v is False or v is None else str(v))

    @property
    def Interactive(self):  # noqa: N802
        """False blocks the user's keyboard and mouse input (dialogs from Python still work); it turns back on when the
        Python host restarts."""
        return bool(_rq("ui.interactive"))

    @Interactive.setter
    def Interactive(self, v):  # noqa: N802
        _rq("ui.interactive", on=bool(v))

    @property
    def DesignMode(self):  # noqa: N802
        return bool(_rq("ui.design_mode_get")["on"])

    @DesignMode.setter
    def DesignMode(self, v):  # noqa: N802
        _rq("ui.design_mode", on=bool(v))

    # ---- The window, the cursor and interrupting (Excel 16's values, measured) ----
    def _win(self, **kw):
        return _rq("app.window", **kw)

    @property
    def Caption(self):  # noqa: N802
        """The title bar: "<the workbook's window caption> - YCGrid". Setting it replaces the application's part
        ("Desk" makes "Book1 - Desk"); None or "" puts YCGrid back."""
        return self._win()["title"]

    @Caption.setter
    def Caption(self, v):  # noqa: N802
        self._win(app_caption="" if v is None else str(v))

    @property
    def WindowState(self):  # noqa: N802
        """xlMaximized (-4137), xlMinimized (-4140) or xlNormal (-4143): the YCGrid window's."""
        return int(self._win()["state"])

    @WindowState.setter
    def WindowState(self, v):  # noqa: N802
        if int(v) not in (xlMaximized, xlMinimized, xlNormal):
            raise ApplicationDefinedError("Unable to set the WindowState property")
        self._win(state=int(v))

    @property
    def DisplayFullScreen(self):  # noqa: N802
        """The window maximized without the ribbon, the formula bar and the status bar (DisplayFormulaBar and
        DisplayStatusBar read False meanwhile; False brings them back as they were)."""
        return bool(self._win()["full_screen"])

    @DisplayFullScreen.setter
    def DisplayFullScreen(self, v):  # noqa: N802
        self._win(full_screen=bool(v))

    @property
    def DisplayStatusBar(self):  # noqa: N802
        """The status bar at the bottom of the window (False in full screen)."""
        return bool(self._win()["status_bar"])

    @DisplayStatusBar.setter
    def DisplayStatusBar(self, v):  # noqa: N802
        self._win(status_bar=bool(v))

    @property
    def Cursor(self):  # noqa: N802
        """xlDefault (-4143), xlWait (2), xlNorthwestArrow (1) or xlIBeam (3): the mouse pointer over YCGrid, as a
        macro sets it while it works. It stays until set back (or Python restarts), as in Excel."""
        return int(self._win()["cursor"])

    @Cursor.setter
    def Cursor(self, v):  # noqa: N802
        self._win(cursor=int(v))

    @property
    def Ready(self):  # noqa: N802
        """False while a cell is being edited or a dialog is open (what YCGrid can't take from code then)."""
        return bool(self._win()["ready"])

    @property
    def Left(self):  # noqa: N802
        """The YCGrid window's position and size on the screen, in points."""
        return float(self._win()["left"])

    @Left.setter
    def Left(self, v):  # noqa: N802
        self._win(left=float(v))

    @property
    def Top(self):  # noqa: N802
        return float(self._win()["top"])

    @Top.setter
    def Top(self, v):  # noqa: N802
        self._win(top=float(v))

    @property
    def Width(self):  # noqa: N802
        return float(self._win()["width"])

    @Width.setter
    def Width(self, v):  # noqa: N802
        self._win(width=float(v))

    @property
    def Height(self):  # noqa: N802
        return float(self._win()["height"])

    @Height.setter
    def Height(self, v):  # noqa: N802
        self._win(height=float(v))

    @property
    def UsableWidth(self):  # noqa: N802
        """The width (points) of the workspace under the ribbon and formula bar, where the sheets are."""
        return float(self._win()["usable_width"])

    @property
    def UsableHeight(self):  # noqa: N802
        return float(self._win()["usable_height"])

    @property
    def EnableCancelKey(self):  # noqa: N802
        """What Ctrl+Break does to the code running: xlInterrupt (1, the default: it stops, and the Error Log says
        so), xlErrorHandler (2: CodeInterruptedError is raised where the code was, for an `except` to clean up) or
        xlDisabled (0: nothing). Back to xlInterrupt when the code returns, as Excel resets it at idle. Esc keeps its
        meaning in the UI (YCGrid stays usable while code runs)."""
        from . import host

        return host.cancel_key

    @EnableCancelKey.setter
    def EnableCancelKey(self, v):  # noqa: N802
        from . import host

        if int(v) not in (0, 1, 2):
            raise ApplicationDefinedError("EnableCancelKey must be xlDisabled, xlInterrupt or xlErrorHandler")
        host.cancel_key = int(v)

    @property
    def CalculationInterruptKey(self):  # noqa: N802
        """xlAnyKey (2, the default), xlEscKey (1) or xlNoKey (0): the key that stops a calculation running for a
        while. YCGrid's UI never waits for a calculation; the cells it didn't reach stay pending (CalculationState
        xlPending) and the next calculation, or F9, goes on."""
        return int(_rq("app.calc_interrupt")["key"])

    @CalculationInterruptKey.setter
    def CalculationInterruptKey(self, v):  # noqa: N802
        if int(v) not in (0, 1, 2):
            raise ApplicationDefinedError("CalculationInterruptKey must be xlNoKey, xlEscKey or xlAnyKey")
        _rq("app.calc_interrupt", key=int(v))

    def CheckAbort(self, KeepAbort=None):  # noqa: N802,N803
        """Stops the calculation in progress (what it didn't reach stays pending); KeepAbort, a Range, is calculated
        all the same."""
        _rq("app.calc_interrupt", abort=True)
        if KeepAbort is not None:
            (KeepAbort if isinstance(KeepAbort, Range) else Range(KeepAbort)).Calculate()

    @property
    def MultiThreadedCalculation(self):  # noqa: N802
        """File > Options > Advanced > Formulas: Enabled, ThreadMode (xlThreadModeAutomatic: every processor;
        xlThreadModeManual: ThreadCount threads) and ThreadCount (saved for this computer)."""
        return _MultiThreadedCalculation()

    @property
    def ThisCell(self):  # noqa: N802
        """Inside a worksheet function written in Python (a UDF): the cell that called it; None anywhere else."""
        c = self.Caller
        return c if isinstance(c, Range) else None

    def International(self, Index):  # noqa: N802,N803
        """The separators and orders YCGrid reads and writes (US Excel's; see Regional settings in
        docs/excel-parity-backlog.md): xlDecimalSeparator ".", xlListSeparator ",", xlDateOrder 0 (month, day, year),
        xlCountryCode 1, ... (Excel's xlApplicationInternational indexes 1 to 45)."""
        i = int(Index)
        if i not in _INTERNATIONAL:
            raise ApplicationDefinedError(f"International({i}): the index must be one of Excel's 1 to 45")
        return _INTERNATIONAL[i]

    def FileDialog(self, fileDialogType):  # noqa: N802,N803
        """A file dialog object: msoFileDialogOpen (1), msoFileDialogSaveAs (2), msoFileDialogFilePicker (3) or
        msoFileDialogFolderPicker (4). Set Title, InitialFileName, AllowMultiSelect, Filters, ButtonName; Show()
        returns -1 when the user chose (SelectedItems holds the paths) and 0 when cancelled."""
        return _FileDialog(int(fileDialogType))

    def MacroOptions(self, Macro=None, Description=None, HasMenu=None, MenuText=None, HasShortcutKey=None,  # noqa: N802,N803
                     ShortcutKey=None, Category=None, StatusBar=None, HelpContextID=None, HelpFile=None,
                     ArgumentDescriptions=None):
        """A worksheet function's description, category and argument descriptions, shown by Insert Function (Shift+F3)
        and its argument help; Macro is the function's name. ShortcutKey (with HasShortcutKey) runs a macro on
        Ctrl+key (Ctrl+Shift+key for a capital), as Application.OnKey does."""
        if not Macro:
            raise ApplicationDefinedError("MacroOptions needs the Macro's name")
        name = str(Macro).split("!")[-1]
        _rq("udf.options", name=name, description=None if Description is None else str(Description),
            category=None if Category is None else (Category if isinstance(Category, str) else _UDF_CATEGORIES.get(int(Category), str(Category))),
            args=None if ArgumentDescriptions is None else [str(a) for a in ArgumentDescriptions])
        if HasShortcutKey and ShortcutKey:
            key = str(ShortcutKey)
            self.OnKey(("^+" + key.lower()) if key.isupper() else ("^" + key), name)

    # ---- Methods ----
    def Calculate(self):  # noqa: N802
        _rq("calc.run", full=False)

    def CalculateFull(self):  # noqa: N802
        _rq("calc.run", full=True)

    CalculateFullRebuild = CalculateFull

    def Evaluate(self, Name: str, _sheet: Optional[int] = None):  # noqa: N802,N803
        text = Name if Name.startswith("=") else "=" + Name
        return _from_wire(_rq("eval", formula=text, sheet=_sheet))

    def Goto(self, Reference, Scroll=False):  # noqa: N802,N803
        rng = Reference if isinstance(Reference, Range) else Range(Reference)
        rng.Select()
        if Scroll:
            ActiveWindow.ScrollRow = rng.Row
            ActiveWindow.ScrollColumn = rng.Column

    def Run(self, Macro, *Args):  # noqa: N802,N803
        from . import analysis, host

        # the Analysis ToolPak: Application.Run("ATPVBAEN.XLAM!Descr", ...)
        if isinstance(Macro, str) and (tool := analysis.procedure(Macro)) is not None and "!" in Macro:
            return tool(*Args)
        return host.run_macro(Macro, *Args)

    def OnChange(self, Address, Handler=None, Sources=ycUser | ycPython):  # noqa: N802,N803
        """[YC] A cell-change handler for a range ("Trade!B14:F25"; the active sheet without a sheet name) from any
        module, with its sources (ycUser = the user's edits only). As a decorator: @Application.OnChange("B2:B9",
        Sources=ycUser)."""
        rng = Range(Address) if isinstance(Address, str) else Address

        def register(fn):
            rng.SetOnChange(fn, Sources)
            return fn

        return register(Handler) if Handler is not None else register

    def OnKey(self, Key, Procedure=None):  # noqa: N802,N803
        """Run a macro when a key is pressed: Key in Excel's text ("^b" Ctrl+B, "+{F5}" Shift+F5, "%s" Alt+S, "{F9}");
        Procedure "module.function" (or a function); "" makes the key do nothing; None restores it. The macro can read
        [YC] yc.interact.cell_context() (the active cell's table row)."""
        if callable(Procedure):
            from . import interact

            interact.on_key(Key, lambda ctx, f=Procedure: f())
            return
        _rq("ui.onkey", key=str(Key), procedure=None if Procedure is None else str(Procedure))

    def OnTime(self, EarliestTime, Procedure, LatestTime=None, Schedule=True):  # noqa: N802,N803
        from . import host

        host.schedule(EarliestTime, Procedure, Schedule)

    def Wait(self, Time):  # noqa: N802,N803
        target = Time if isinstance(Time, _dt.datetime) else _dt.datetime.now() + (Time if isinstance(Time, _dt.timedelta) else _dt.timedelta(seconds=Time))
        while _dt.datetime.now() < target:
            _time.sleep(0.01)
        return True

    def Volatile(self, Volatile=True):  # noqa: N802,N803
        pass  # volatile UDFs are declared with @udf(volatile=True)

    def Quit(self):  # noqa: N802
        _rq("ui.command", id="FileClose")

    def Intersect(self, *Args):  # noqa: N802,N803
        return Intersect(*Args)

    def Union(self, *Args):  # noqa: N802,N803
        return Union(*Args)

    def InputBox(self, Prompt, Title=None, Default="", Left=None, Top=None, HelpFile=None, HelpContextID=None, Type=None):  # noqa: N802,N803
        r = _rq("ui.inputbox", prompt=str(Prompt), title=Title or "YCGrid", default=str(Default), type=Type or 2)
        if r is None:
            return False
        if Type == 8:
            return Range(r)
        if Type == 1:
            try:
                return float(r)
            except ValueError:
                return False
        return r

    def Sync(self, Wait="ui"):  # noqa: N802,N803
        _rq("sync", wait=Wait)

    # ---- the UI's own actions ----
    def Undo(self):  # noqa: N802
        """Undo the user's last action (Ctrl+Z). Changes made from Python clear the undo list, as in Excel."""
        _rq("ui.command", id="Undo")

    def Redo(self):  # noqa: N802
        """[YC] Redo what Undo took back (Ctrl+Y)."""
        _rq("ui.command", id="Redo")

    def Repeat(self):  # noqa: N802
        """Repeat the user's last formatting, insert / delete, fill or clear command on the selection (F4)."""
        _rq("ui.command", id="Repeat")

    @property
    def ErrorCheckingOptions(self):  # noqa: N802
        """Background error checking: BackgroundChecking, NumberAsText, EvaluateToError, InconsistentFormula."""
        return _ErrorCheckingOptions()

    @property
    def DisplayFormulaAutoComplete(self):  # noqa: N802
        """The list of functions and names shown while a formula is typed (File -> Options -> Formulas; saved)."""
        return bool(_rq("app.formula_options")["autocomplete"])

    @DisplayFormulaAutoComplete.setter
    def DisplayFormulaAutoComplete(self, v):  # noqa: N802
        _rq("app.formula_options", autocomplete=bool(v))

    @property
    def DisplayFunctionToolTips(self):  # noqa: N802
        """The argument hint (SUM(number1, [number2], ...)) shown while a function's arguments are typed (saved)."""
        return bool(_rq("app.formula_options")["tooltips"])

    @DisplayFunctionToolTips.setter
    def DisplayFunctionToolTips(self, v):  # noqa: N802
        _rq("app.formula_options", tooltips=bool(v))

    @property
    def FormulaBarHeight(self):  # noqa: N802
        """The formula bar's height in lines (1 = collapsed; Ctrl+Shift+U expands it to 3; saved)."""
        return int(_rq("app.formula_options")["bar_lines"])

    @FormulaBarHeight.setter
    def FormulaBarHeight(self, v):  # noqa: N802
        _rq("app.formula_options", bar_lines=int(v))

    @property
    def DisplayFormulaBar(self):  # noqa: N802
        # False in full screen, and as it was after (Excel 16, measured)
        return bool(_rq("cmdbar.pane", name="formula_bar")) and not self.DisplayFullScreen

    @DisplayFormulaBar.setter
    def DisplayFormulaBar(self, v):  # noqa: N802
        _rq("cmdbar.pane", name="formula_bar", visible=bool(v))

    @property
    def RibbonDisplay(self):  # noqa: N802
        """[YC] ycRibbonAlwaysShow (0), ycRibbonTabsOnly (1) or ycRibbonAutoHide (2): Ribbon Display Options."""
        return int(_rq("cmdbar.ribbon_mode"))

    @RibbonDisplay.setter
    def RibbonDisplay(self, v):  # noqa: N802
        _rq("cmdbar.ribbon_mode", mode=int(v))

    def GetOpenFilename(self, FileFilter=None, FilterIndex=1, Title=None, ButtonText=None, MultiSelect=False):  # noqa: N802,N803
        """The Open dialog without opening anything: the path, a list of paths (MultiSelect), or False when cancelled.
        FileFilter as in Excel: "Text Files (*.txt),*.txt,All Files (*.*),*.*"."""
        r = _rq("app.file_dialog", save=False, multi=bool(MultiSelect), filter=FileFilter or "All Files (*.*),*.*",
                index=int(FilterIndex or 1), title=Title or "")
        if r is None or r == [] or r == "":
            return False
        return list(r) if MultiSelect else r

    def GetSaveAsFilename(self, InitialFilename=None, FileFilter=None, FilterIndex=1, Title=None, ButtonText=None):  # noqa: N802,N803
        """The Save As dialog without saving: the chosen path, or False when cancelled."""
        r = _rq("app.file_dialog", save=True, filter=FileFilter or "All Files (*.*),*.*", index=int(FilterIndex or 1),
                title=Title or "", initial=str(InitialFilename or ""))
        return r or False

    # attributes kept in the object itself (the rest are properties or class members)
    _plain = frozenset({"CommandBars", "Topics", "DisplayAlerts", "ScreenUpdating"})

    def __getattr__(self, name):
        if name in ("Panels", "Containers", "Layout"):
            from . import ui

            return getattr(ui, name)
        # Application.VLookup(...) etc.: a worksheet function; errors are returned as XlError instead of raising
        if name[:1].isupper() and _is_worksheet_function(name):
            def fn(*args):
                return _call_function(name, args, raise_errors=False)

            fn.__name__ = name
            return fn
        # like Excel's "Object doesn't support this property or method": a typo or a member YCGrid doesn't have fails here
        raise AttributeError(f"Application has no member {name!r} (and it isn't a worksheet function)")

    def __setattr__(self, name, value):
        # a typo or a member YCGrid doesn't have raises instead of being kept silently (Application.StrictErrors = True
        # did nothing); properties without a setter raise as read-only
        if name.startswith("_") or name in _Application._plain or isinstance(getattr(type(self), name, None), property):
            object.__setattr__(self, name, value)
            return
        raise AttributeError(f"Application has no member {name!r} that can be set")


def _from_wire(v):
    # an array result (rows of values) reads like Range.Value: arr(i, j) 1-based, arr[i][j] 0-based
    if isinstance(v, list) and v and isinstance(v[0], list):
        return VArray(v)
    return v


# ---------------------------------------------------------------------------
# Workbook, KV
# ---------------------------------------------------------------------------
class Store:
    """Workbook / worksheet KV store (values saved as msgpack, PLAN §15.8)."""

    def __init__(self, sheet: Optional[int] = None):
        self._sheet = sheet
        self._batch: Optional[dict] = None

    def _get_raw(self, key):
        return _rq("store.get", sheet=self._sheet, key=str(key))

    def __getitem__(self, key):
        if self._batch is not None and key in self._batch:
            v = self._batch[key]
            if v is _DELETED:
                raise KeyError(key)
            return v
        raw = self._get_raw(key)
        if raw is None:
            raise KeyError(key)
        return _decode_store(raw)

    def get(self, key, default=None):
        try:
            return self[key]
        except KeyError:
            return default

    def __setitem__(self, key, value):
        if self._batch is not None:
            self._batch[key] = value
            return
        _rq("store.set", sheet=self._sheet, key=str(key), value=_encode_store(value))

    def __delitem__(self, key):
        if self._batch is not None:
            self._batch[key] = _DELETED
            return
        if not _rq("store.delete", sheet=self._sheet, key=str(key)):
            raise KeyError(key)

    def __contains__(self, key):
        return self.get(key, _DELETED) is not _DELETED

    def keys(self):
        return list(_rq("store.keys", sheet=self._sheet))

    def items(self):
        return [(k, self[k]) for k in self.keys()]

    def __iter__(self):
        return iter(self.keys())

    def __len__(self):
        return len(self.keys())

    def Batch(self):  # noqa: N802
        store = self

        class _B:
            def __enter__(self_inner):
                store._batch = {}
                return store

            def __exit__(self_inner, *exc):
                pending, store._batch = store._batch, None
                if exc[0] is None:
                    for k, v in pending.items():
                        if v is _DELETED:
                            _rq("store.delete", sheet=store._sheet, key=str(k))
                        else:
                            _rq("store.set", sheet=store._sheet, key=str(k), value=_encode_store(v))
                return False

        return _B()


_DELETED = object()


def _encode_store(value) -> bytes:
    if hasattr(value, "tolist") and hasattr(value, "dtype"):
        value = {"__ndarray__": value.tolist(), "dtype": str(value.dtype)}
    if isinstance(value, set):
        value = list(value)
    return _msgpack.packb(value)


def _decode_store(raw: bytes):
    v = _msgpack.unpackb(raw)
    if isinstance(v, dict) and "__ndarray__" in v:
        try:
            import numpy as np

            return np.array(v["__ndarray__"], dtype=v.get("dtype"))
        except ImportError:
            return v["__ndarray__"]
    if isinstance(v, _decimal.Decimal) or v is None:
        return v
    return v


class _DocProp:
    def __init__(self, name):
        self.Name = name

    @property
    def Value(self):  # noqa: N802
        r = _rq("props.get", name=self.Name)
        if r is None:
            raise SubscriptOutOfRangeError(f"No such property: {self.Name}")
        return r

    @Value.setter
    def Value(self, v):  # noqa: N802
        _rq("props.set", name=self.Name, value=_to_wire(v))

    def Delete(self):  # noqa: N802
        _rq("props.delete", name=self.Name)


class _BuiltinDocProp:
    """Workbook.BuiltinDocumentProperties("Title"): Value (Title, Subject, Author, Manager, Company, Category, Keywords,
    Comments, Status; Creation Date, Last Save Time and Last Author are set when the workbook is saved)."""

    _DATES = {"Creation Date": "Created", "Last Save Time": "LastSaved"}
    _KEYS = {"Last Author": "LastModifiedBy"}

    def __init__(self, name):
        self.Name = name

    def _key(self):
        return self._DATES.get(self.Name) or self._KEYS.get(self.Name) or self.Name

    @property
    def Value(self):  # noqa: N802
        v = _rq("layout.docprops").get(self._key())
        if self.Name in self._DATES:
            import datetime

            return datetime.datetime.fromtimestamp(v) if v else None
        return v if v is not None else ""

    @Value.setter
    def Value(self, v):  # noqa: N802
        if self.Name in self._DATES or self.Name in self._KEYS:
            raise AttributeError(f"{self.Name} is set when the workbook is saved")
        _rq("layout.docprops", name=self._key(), value=str(v))


class _BuiltinDocProps:
    _NAMES = ("Title", "Subject", "Author", "Keywords", "Comments", "Template", "Last Author", "Revision Number", "Application Name",
              "Last Print Date", "Creation Date", "Last Save Time", "Total Editing Time", "Number of Pages", "Number of Words",
              "Number of Characters", "Security", "Category", "Format", "Manager", "Company", "Number of Bytes", "Number of Lines",
              "Number of Paragraphs", "Number of Slides", "Number of Notes", "Number of Hidden Slides", "Number of Multimedia Clips",
              "Hyperlink base", "Number of characters (with spaces)", "Content type", "Content status", "Language", "Document version")

    def __call__(self, Index):  # noqa: N803
        if isinstance(Index, int):
            if not 1 <= Index <= len(self._NAMES):
                raise SubscriptOutOfRangeError()
            Index = self._NAMES[Index - 1]
        if Index == "Content status":
            Index = "Status"
        return _BuiltinDocProp(Index)

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return len(self._NAMES)

    def __iter__(self):
        return (_BuiltinDocProp(n) for n in self._NAMES)


class _CustomView:
    """Workbook.CustomViews(name): Show / Delete; PrintSettings and RowColSettings say what it keeps."""

    def __init__(self, name):
        self.Name = name

    def _get(self):
        return _rq("layout.custom_views", op="get", name=self.Name)

    @property
    def PrintSettings(self):  # noqa: N802
        return bool(self._get()["print"])

    @property
    def RowColSettings(self):  # noqa: N802
        return bool(self._get()["hidden"])

    def Show(self):  # noqa: N802
        _rq("layout.custom_views", op="show", name=self.Name)

    def Delete(self):  # noqa: N802
        _rq("layout.custom_views", op="delete", name=self.Name)


class _CustomViews:
    """View > Custom Views: the display and print settings saved by name in the workbook."""

    def _names(self):
        return _rq("layout.custom_views", op="list")

    def Add(self, ViewName, PrintSettings=True, RowColSettings=True):  # noqa: N802,N803
        _rq("layout.custom_views", op="add", name=str(ViewName), print=bool(PrintSettings), hidden=bool(RowColSettings))
        return _CustomView(str(ViewName))

    def __call__(self, Index):  # noqa: N803
        names = self._names()
        if isinstance(Index, int):
            if not 1 <= Index <= len(names):
                raise SubscriptOutOfRangeError()
            return _CustomView(names[Index - 1])
        if Index not in names:
            raise SubscriptOutOfRangeError(f"No such view: {Index}")
        return _CustomView(Index)

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def __iter__(self):
        return (_CustomView(n) for n in self._names())


class _DocProps:
    def __call__(self, Index=None):  # noqa: N803
        if Index is None:
            return self
        names = self._names()
        if isinstance(Index, int):
            if not 1 <= Index <= len(names):
                raise SubscriptOutOfRangeError()
            return _DocProp(names[Index - 1])
        if Index not in names:
            raise SubscriptOutOfRangeError(f"No such property: {Index}")
        return _DocProp(Index)

    Item = __call__

    def _names(self):
        return _rq("props.list")

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def Add(self, Name, LinkToContent=False, Type=None, Value=None, LinkSource=None):  # noqa: N802,N803
        _rq("props.set", name=Name, value=_to_wire(Value))
        return _DocProp(Name)

    def __iter__(self):
        return (_DocProp(n) for n in self._names())


class _IconSetRef:
    """Workbook.IconSets(xl3Arrows): an icon set to assign to FormatCondition.IconSet."""

    __slots__ = ("ID",)

    def __init__(self, n):
        self.ID = int(n)

    @property
    def Count(self):  # noqa: N802
        return 5 if self.ID in (14, 15, 16, 17, 20) else 4 if self.ID in (9, 10, 11, 12, 13) else 3

    def Item(self, Index):  # noqa: N802,N803
        """IconSets(xl3Arrows).Item(1): one icon of the set (1 = the icon of the lowest values), for SortField.SetIcon."""
        if not 1 <= int(Index) <= self.Count:
            raise SubscriptOutOfRangeError()
        return _Icon(self, int(Index))

    __call__ = Item


class _Icon:
    """An icon of an icon set (Icon object): Parent is its IconSet, Index its place (1 = the lowest values' icon)."""

    def __init__(self, icon_set, index):
        self.Parent, self.Index = icon_set, index


class _ThemeColor:
    def __init__(self, index):
        self._i = int(index)

    @property
    def RGB(self):  # noqa: N802
        c = _rq("theme.get")["colors"][self._i - 1]
        return _argb_to_bgr(c)

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        colors = list(_rq("theme.get")["colors"])
        colors[self._i - 1] = _color_to_argb(v) & 0xFFFFFF
        _rq("theme.set", colors=colors)


class _ThemeColorScheme:
    def Colors(self, Index):  # noqa: N802,N803
        """msoThemeDark1 ... msoThemeFollowedHyperlink (1..12)."""
        if not 1 <= int(Index) <= 12:
            raise SubscriptOutOfRangeError(f"Colors({Index})")
        return _ThemeColor(Index)

    @property
    def Name(self):  # noqa: N802
        """The color set's name; [YC] set it to apply a built-in set (Page Layout > Colors: "Office", "Blue Warm"...)."""
        return _rq("theme.get")["colors_name"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        _rq("theme.set", colors_name=str(v))


class _ThemeFont:
    def __init__(self, major):
        self._major = major

    @property
    def Name(self):  # noqa: N802
        return _rq("theme.get")["major" if self._major else "minor"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        _rq("theme.set", **{"major" if self._major else "minor": str(v)})


class _ThemeFonts:
    def __init__(self, major):
        self._major = major

    def __call__(self, Index=1):  # noqa: N803
        return _ThemeFont(self._major)

    Item = __call__

    @property
    def Name(self):  # noqa: N802
        return _ThemeFont(self._major).Name


class _ThemeFontScheme:
    @property
    def MajorFont(self):  # noqa: N802
        return _ThemeFonts(True)

    @property
    def MinorFont(self):  # noqa: N802
        return _ThemeFonts(False)

    @property
    def Name(self):  # noqa: N802
        """The font pair's name; [YC] set it to apply a built-in pair (Page Layout > Fonts)."""
        return _rq("theme.get")["fonts_name"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        _rq("theme.set", fonts_name=str(v))


class _ThemeEffectScheme:
    """Workbook.Theme.ThemeEffectScheme (Page Layout > Themes > Effects): Load(name) applies a built-in scheme
    ("Office", "Subtle Solids", "Banded Edges", ... or an .eftx file named after one); Name reads it."""

    def Load(self, FileName):  # noqa: N802,N803
        import os

        name = os.path.splitext(os.path.basename(str(FileName)))[0]
        _rq("layout.theme_effects", name=name)

    @property
    def Name(self):  # noqa: N802
        return _rq("layout.theme_effects")["name"]

    @property
    def BuiltinSchemes(self):  # noqa: N802
        """[YC] The names Load accepts."""
        return list(_rq("layout.theme_effects")["builtin"])


class _Theme:
    """Workbook.Theme: ThemeColorScheme.Colors(msoThemeAccent1).RGB, ThemeFontScheme.MajorFont(msoThemeLatin).Name..."""

    @property
    def ThemeEffectScheme(self):  # noqa: N802
        return _ThemeEffectScheme()

    @property
    def ThemeColorScheme(self):  # noqa: N802
        return _ThemeColorScheme()

    @property
    def ThemeFontScheme(self):  # noqa: N802
        return _ThemeFontScheme()

    @property
    def Name(self):  # noqa: N802
        """[YC] The theme's name ("Office", "Facet"...)."""
        return _rq("theme.get")["name"]

    @property
    def BuiltinThemes(self):  # noqa: N802
        """[YC] The names ApplyTheme accepts."""
        return list(_rq("theme.get")["builtin"])


class _StyleTarget:
    """Lets Font / Interior / Borders change a named style instead of cells."""

    __slots__ = ("_name",)

    def __init__(self, name):
        self._name = name

    def _style_get(self):
        for x in _rq("cellstyle.list"):
            if x["name"].lower() == self._name.lower():
                return x["style"]
        raise SubscriptOutOfRangeError(f"No style named {self._name!r}")

    def _style(self, **props):
        _rq("cellstyle.save", name=self._name, props=props)

    def _border_set(self, edges, style, color):
        _rq("cellstyle.save", name=self._name, props={"borders": {"edges": edges, "style": style, "color": color}})


_INCLUDES = {"IncludeNumber": 1, "IncludeAlignment": 2, "IncludeFont": 4, "IncludeBorder": 8, "IncludePatterns": 16, "IncludeProtection": 32}


class Style:
    """A named cell style (Workbook.Styles(name), Range.Style): changing it changes every cell that has it."""

    def __init__(self, name):
        self._name = str(name)

    def __repr__(self):
        return f"<Style {self._name!r}>"

    def __eq__(self, other):
        return isinstance(other, Style) and other._name.lower() == self._name.lower()

    def __str__(self):
        return self._name

    def _info(self):
        for x in _rq("cellstyle.list"):
            if x["name"].lower() == self._name.lower():
                return x
        raise SubscriptOutOfRangeError(f"No style named {self._name!r}")

    def _set(self, **props):
        _rq("cellstyle.save", name=self._name, props=props)

    @property
    def Name(self):  # noqa: N802
        return self._info()["name"]

    NameLocal = Name

    @property
    def BuiltIn(self):  # noqa: N802
        return bool(self._info()["builtin"])

    @property
    def Font(self):  # noqa: N802
        return Font(_StyleTarget(self._name))

    @property
    def Interior(self):  # noqa: N802
        return Interior(_StyleTarget(self._name))

    @property
    def Borders(self):  # noqa: N802
        return Borders(_StyleTarget(self._name))

    @property
    def NumberFormat(self):  # noqa: N802
        return self._info()["style"]["numfmt"]

    @NumberFormat.setter
    def NumberFormat(self, v):  # noqa: N802
        self._set(numfmt=str(v))

    @property
    def HorizontalAlignment(self):  # noqa: N802
        return _HALIGN_OUT[self._info()["style"]["halign"]]

    @HorizontalAlignment.setter
    def HorizontalAlignment(self, v):  # noqa: N802
        self._set(halign=_HALIGN_IN[v])

    @property
    def VerticalAlignment(self):  # noqa: N802
        return _VALIGN_OUT[self._info()["style"]["valign"]]

    @VerticalAlignment.setter
    def VerticalAlignment(self, v):  # noqa: N802
        self._set(valign=_VALIGN_IN[v])

    @property
    def WrapText(self):  # noqa: N802
        return bool(self._info()["style"]["wrap"])

    @WrapText.setter
    def WrapText(self, v):  # noqa: N802
        self._set(wrap=bool(v))

    @property
    def IndentLevel(self):  # noqa: N802
        return self._info()["style"]["indent"]

    @IndentLevel.setter
    def IndentLevel(self, v):  # noqa: N802
        self._set(indent=int(v))

    @property
    def Locked(self):  # noqa: N802
        return bool(self._info()["style"]["locked"])

    @Locked.setter
    def Locked(self, v):  # noqa: N802
        self._set(locked=bool(v))

    @property
    def FormulaHidden(self):  # noqa: N802
        return bool(self._info()["style"]["hidden"])

    @FormulaHidden.setter
    def FormulaHidden(self, v):  # noqa: N802
        self._set(hidden=bool(v))

    def Delete(self):  # noqa: N802
        """Delete a custom style (its cells get the Normal formats); a changed built-in style goes back to its original."""
        _rq("cellstyle.delete", name=self._name)


def _include_prop(flag):
    def get(self):
        return bool(self._info()["includes"] & flag)

    def set_(self, v):
        inc = self._info()["includes"]
        _rq("cellstyle.save", name=self._name, includes=(inc | flag) if v else (inc & ~flag))

    return property(get, set_)


for _n, _f in _INCLUDES.items():
    setattr(Style, _n, _include_prop(_f))


class Styles:
    """Workbook.Styles: the built-in and custom cell styles."""

    def _names(self):
        return [x["name"] for x in _rq("cellstyle.list")]

    def __call__(self, Index):  # noqa: N803
        names = self._names()
        if isinstance(Index, int) and not isinstance(Index, bool):
            if not 1 <= Index <= len(names):
                raise SubscriptOutOfRangeError(f"Styles({Index})")
            return Style(names[Index - 1])
        for n in names:
            if n.lower() == str(Index).lower():
                return Style(n)
        raise SubscriptOutOfRangeError(f"No style named {Index!r}")

    Item = __call__

    def __iter__(self):
        return iter([Style(n) for n in self._names()])

    def __len__(self):
        return len(self._names())

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def Merge(self, Workbook):  # noqa: N802,N803
        """Home > Cell Styles > Merge Styles: the cell styles of another workbook (a .ycg path, or an object with
        FullName) come into this one; styles with the same names are replaced."""
        path = getattr(Workbook, "FullName", Workbook)
        _rq("cellstyle.merge", path=str(path))

    def Add(self, Name, BasedOn=None):  # noqa: N802,N803
        """A new style with the formats of BasedOn (a Range: its first cell) or of Normal."""
        kw = dict(name=str(Name), create=True)
        if isinstance(BasedOn, Range):
            kw.update(based_sheet=BasedOn._sheet, based_row=BasedOn._a[0], based_col=BasedOn._a[1])
        return Style(_rq("cellstyle.save", **kw))


class _SortOnValue:
    """SortField.SortOnValue: the color of a Sort On color level (Color: an RGB number, as Range.Interior.Color)."""

    def __init__(self):
        self.Color = None


class SortField:
    def __init__(self, col, order, custom, sort_on=0, row=None):
        self._col, self.Order, self.CustomOrder = col, order, custom
        self._row = row  # the key's row (Orientation = xlLeftToRight sorts columns by a row)
        self.SortOn = sort_on  # xlSortOnValues, xlSortOnCellColor (1), xlSortOnFontColor (2), xlSortOnIcon (3)
        self.SortOnValue = _SortOnValue()
        self._icon = None

    def SetIcon(self, Icon):  # noqa: N802,N803
        """Sort On Conditional Formatting Icon: the rows showing this icon (Workbook.IconSets(set).Item(n)) on top
        (xlDescending: on bottom)."""
        self._icon = Icon
        self.SortOn = 3

    def _key(self, left_to_right=False):
        k = [self._row if left_to_right and self._row is not None else self._col, self.Order == xlDescending]
        c = self.CustomOrder
        if isinstance(c, int) and not isinstance(c, bool) and c > 0:
            k.append(c - 1)
        elif c:
            k += [-1, [x.strip() for x in (c.split(",") if isinstance(c, str) else c)]]
        if self.SortOn in (xlSortOnCellColor, xlSortOnFontColor):
            # the color's rows on top (xlDescending: on bottom); no color given = no fill / automatic
            bgr = self.SortOnValue.Color
            argb = 0 if bgr is None else 0xFF000000 | ((int(bgr) & 0xFF) << 16) | (int(bgr) & 0xFF00) | ((int(bgr) >> 16) & 0xFF)
            k = k[:2] + (k[2:4] if len(k) >= 4 else [k[2], []] if len(k) == 3 else [-1, []]) + [int(self.SortOn), argb]
        elif self.SortOn == 3 and self._icon is not None:
            k = k[:2] + [-1, []] + [3, 0, int(self._icon.Parent.ID), int(self._icon.Index) - 1]
        return k


class SortFields:
    def __init__(self):
        self._f = []

    def Add(self, Key, SortOn=0, Order=xlAscending, CustomOrder=None, DataOption=0):  # noqa: N802,N803
        """Key: a Range (its column); CustomOrder: a custom list number or "High,Medium,Low"."""
        col = Key._a[1] if isinstance(Key, Range) else int(Key)
        f = SortField(col, Order, CustomOrder, SortOn or 0, Key._a[0] if isinstance(Key, Range) else None)
        self._f.append(f)
        return f

    Add2 = Add

    def Clear(self):  # noqa: N802
        self._f = []

    @property
    def Count(self):  # noqa: N802
        return len(self._f)

    def __call__(self, Index):  # noqa: N803
        return self._f[int(Index) - 1]

    Item = __call__


class SortObj:
    """Worksheet.Sort: SortFields, SetRange, Header, Apply (the state stays with the sheet, like Excel)."""

    def __init__(self, sheet_id):
        self._sheet = sheet_id
        self.SortFields = SortFields()
        self.Header = xlGuess
        self.MatchCase = False
        self.Orientation = xlTopToBottom
        self.SortMethod = 1
        self._rng = None

    def SetRange(self, Rng):  # noqa: N802,N803
        self._rng = Rng

    @property
    def Rng(self):  # noqa: N802
        return self._rng

    def Apply(self):  # noqa: N802
        if self._rng is None:
            raise ApplicationDefinedError("Sort.SetRange first")
        ltr = self.Orientation == xlLeftToRight
        a = self._rng._a
        header = self.Header == xlYes or (self.Header == xlGuess and not ltr and _guess_header(self._rng._sheet, a))
        keys = [f._key(ltr) for f in self.SortFields._f] or [[a[0] if ltr else a[1], False]]
        _rq("range.sort", sheet=self._rng._sheet, rect=list(a), keys=keys, header=header, match_case=bool(self.MatchCase), left_to_right=ltr)


class _TableSort(SortObj):
    """ListObject.Sort, as a recorded macro writes it: SortFields.Clear / Add(Key, SortOn, Order, CustomOrder,
    DataOption), Header, MatchCase, Orientation, Apply. Its range is always the table (Rng), Apply sorts the data rows
    (never the header or totals row), and the sort fields stay after Apply, as in Excel."""

    def __init__(self, table):
        self._t = table
        super().__init__(table._sheet)

    @property
    def Header(self):  # noqa: N802
        return xlYes if self._t._get()["header"] else xlNo

    @Header.setter
    def Header(self, v):  # noqa: N802
        # Excel: xlGuess changes nothing, xlNo on a table with its header row is refused
        if v == xlNo and self._t._get()["header"]:
            raise ApplicationDefinedError("The parameter is incorrect: a table with a header row sorts with Header = xlYes")

    def SetRange(self, Rng):  # noqa: N802,N803
        pass  # a table's sort always covers the table

    @property
    def Rng(self):  # noqa: N802
        return self._t.Range

    def Apply(self):  # noqa: N802
        body = self._t.DataBodyRange
        if body is None or not self.SortFields._f:
            return  # Excel: nothing to sort by, nothing changes
        a = body._a
        keys = [f._key(False) for f in self.SortFields._f]
        _rq("range.sort", sheet=self._t._sheet, rect=list(a), keys=keys, header=False, match_case=bool(self.MatchCase), left_to_right=False)


_TABLE_SORTS = {}


def _guess_header(sheet, a):
    """Excel's xlGuess, the one the Sort and Create Table dialogs make (measured on 13 layouts): no blank in the first
    two rows, and some column whose first cell is text over a number below it, or a number over text, or a first row
    formatted apart from the second."""
    if a[2] <= a[0]:
        return False
    return bool(_rq("range.guess_header", sheet=sheet, rect=list(a)))


_SORTS = {}


class Workbook:
    CodeName = "ThisWorkbook"

    def __setattr__(self, name, value):
        # a typo or a member YCGrid doesn't have raises, as Excel's "Object doesn't support this property": the
        # Workbook kept any name silently (wb.PrecisionAsDisplayed = True did nothing); read-only properties raise too
        if name.startswith("_") or name in ("Store", "CustomDocumentProperties") or isinstance(getattr(type(self), name, None), property):
            object.__setattr__(self, name, value)
            return
        raise AttributeError(f"Workbook has no member {name!r} that can be set")

    def __init__(self, _cx=None, _pid=0):
        self._cx = _cx  # another YCGrid window's workbook: its connection (None = this workbook)
        self._pid = _pid
        self.Store = Store(None)
        self.CustomDocumentProperties = _DocProps()

    @property
    def BuiltinDocumentProperties(self):  # noqa: N802
        """File > Info's properties: BuiltinDocumentProperties("Title").Value."""
        return _BuiltinDocProps()

    @property
    def CustomViews(self):  # noqa: N802
        """View > Custom Views."""
        return _CustomViews()

    def IconSets(self, Index):  # noqa: N802,N803
        return _IconSetRef(Index)

    def _sheet_ids(self):
        return [ws._id for ws in Worksheets if ws.Visible]

    @property
    def PivotCaches(self):  # noqa: N802
        """PivotCaches.Create(...) as a recorded macro writes it; PivotCaches().Create(...) too."""
        return _PivotCaches()

    @property
    def SlicerCaches(self):  # noqa: N802
        return _SlicerCaches()



    # ---- links to other workbooks (=[Book2.ycg]Sheet1!A1) ----
    def LinkSources(self, Type=1):  # noqa: N802,N803
        """The source files of the workbook's links (a tuple), or None when it has none."""
        paths = [x["path"] for x in _rq("links.list")]
        return tuple(paths) if paths else None

    def UpdateLink(self, Name=None, Type=None):  # noqa: N802,N803
        """Read the values again from one source (its path or file name) or from every source."""
        if Name is None:
            _rq("links.update")
        else:
            for n in ([Name] if isinstance(Name, str) else list(Name)):
                _rq("links.update", name=str(n))

    def ChangeLink(self, Name, NewName, Type=1):  # noqa: N802,N803
        """Point the formulas that use one source at another file."""
        import os

        _rq("links.change", name=str(Name), new_name=os.path.abspath(str(NewName)))

    def BreakLink(self, Name, Type=1):  # noqa: N802,N803
        """The formulas that use the source become their values."""
        _rq("links.break", name=str(Name))

    def LinkInfo(self, Name, LinkInfo=1, Type=None, EditionRef=None):  # noqa: N802,N803
        """[YC] dict: path, updated (datetime or None), error ("" = fine)."""
        for x in _rq("links.list"):
            import os

            if x["path"].lower() == str(Name).lower() or os.path.basename(x["path"]).lower() == str(Name).lower():
                return {"path": x["path"], "updated": _dt.datetime.fromtimestamp(x["updated"]) if x["updated"] else None, "error": x["error"]}
        raise SubscriptOutOfRangeError(f"No link to {Name!r}")

    @property
    def Styles(self):  # noqa: N802
        return Styles()

    @property
    def TableStyles(self):  # noqa: N802
        """Excel's built-in table styles and the workbook's own (Home > Format as Table > New Table Style...)."""
        return TableStyles()

    @property
    def DefaultTableStyle(self):  # noqa: N802
        """The style Ctrl+T, Format as Table and ListObjects.Add start with (New Table Style's "Set as default table
        style for this document"); a TableStyle. Set it with a name or a TableStyle; saved with the workbook."""
        return TableStyle(_rq("tablestyle.default_get"))

    @DefaultTableStyle.setter
    def DefaultTableStyle(self, v):  # noqa: N802
        _rq("tablestyle.default_set", name=str(getattr(v, "Name", v)))

    @property
    def Theme(self):  # noqa: N802
        return _Theme()


    # ---- sharing: YCGrid workbooks aren't co-authored (ADR 0012); Excel's members answer for one local user ----
    @property
    def CoAuthoring(self):  # noqa: N802
        return False

    MultiUserEditing = CoAuthoring

    @property
    def UserStatus(self):  # noqa: N802
        """Who has the workbook open: (name, time, 1 = exclusive)."""
        return ((Application.UserName, _dt.datetime.now(), 1),)

    def ExclusiveAccess(self):  # noqa: N802
        """Nothing to do: the workbook is never shared."""

    def ApplyTheme(self, Filename):  # noqa: N802,N803
        """[YC] A built-in theme by name ("Office", "Office 2007 - 2010", "Facet"...; see Theme.BuiltinThemes)."""
        import os

        name = os.path.splitext(os.path.basename(str(Filename)))[0]
        _rq("theme.set", theme=name)

    def ExportImage(self, Filename=None, Headings=False, Gridlines=None, Scale=1.0):  # noqa: N802,N803
        """[YC] Home > Screenshot > Whole Workbook: every visible sheet, one under another with its name, as one PNG
        (or to the Clipboard when Filename is None); see Range.ExportImage."""
        return _export_image("workbook", Filename, Headings, Gridlines, Scale)

    def FollowHyperlink(self, Address, SubAddress=None, NewWindow=False, AddHistory=None, ExtraInfo=None, Method=None,  # noqa: N802,N803
                        HeaderInfo=None):
        """Open a web page or file (Address), or go to a place in this workbook (SubAddress, e.g. "Sheet2!A1")."""
        import os as _os

        if Address:
            _open_on_desktop(Address)
        elif SubAddress:
            Application.Goto(str(SubAddress))

    def RefreshAll(self):  # noqa: N802
        """Refresh every PivotTable."""
        _rq("pivot.refresh_all")

    def PrintOut(self, From=None, To=None, Copies=1, Preview=False, ActivePrinter=None, PrintToFile=False, Collate=True, PrToFileName=None,  # noqa: N802,N803
                 IgnorePrintAreas=False):
        """Print every visible sheet with its page setup."""
        _print_out(self._sheet_ids(), From, To, Copies, Preview, ActivePrinter, PrintToFile, Collate, PrToFileName, IgnorePrintAreas)

    def PrintPreview(self, EnableChanges=True):  # noqa: N802,N803
        _rq("print.preview")

    def ExportAsFixedFormat(self, Type=0, Filename=None, Quality=0, IncludeDocProperties=True, IgnorePrintAreas=False, From=None, To=None,  # noqa: N802,N803
                            OpenAfterPublish=False):
        """Save every visible sheet as one PDF (Type=xlTypePDF)."""
        return _export_pdf(self._sheet_ids(), Type, Filename, Quality, IncludeDocProperties, IgnorePrintAreas, From, To, OpenAfterPublish)

    def Protect(self, Password=None, Structure=True, Windows=False):  # noqa: N802,N803
        """Protect the workbook's structure: sheets can't be added, deleted, renamed, moved or hidden."""
        _rq("protect.workbook", password=str(Password or ""), structure=bool(Structure), windows=bool(Windows))

    def Unprotect(self, Password=None):  # noqa: N802,N803
        _rq("protect.unworkbook", password=str(Password or ""))

    @property
    def ProtectStructure(self):  # noqa: N802
        return bool(_rq("protect.wbget")["structure"])

    @property
    def ProtectWindows(self):  # noqa: N802
        return bool(_rq("protect.wbget")["windows"])

    def _info(self):
        return _rq("wb.info")

    @property
    def Name(self):  # noqa: N802
        import os

        p = self._info().get("path") or ""
        return os.path.basename(p) if p else "Book1"

    @property
    def FullName(self):  # noqa: N802
        return self._info().get("path") or self.Name

    @property
    def Path(self):  # noqa: N802
        import os

        p = self._info().get("path") or ""
        return os.path.dirname(p)

    @property
    def Saved(self):  # noqa: N802
        return not self._info().get("dirty", False)

    @Saved.setter
    def Saved(self, v):  # noqa: N802
        if v:
            _rq("wb.mark_saved")

    def _options(self, **kw):
        return _rq("wb.options", **kw)

    @property
    def Date1904(self):  # noqa: N802
        """File > Options > Advanced > Use 1904 date system (saved with the workbook). The cells keep their numbers and
        show dates four years and a day later; DATE, TODAY and the other date functions give this system's numbers.
        Python's datetimes are the same either way."""
        return bool(self._options()["date1904"])

    @Date1904.setter
    def Date1904(self, v):  # noqa: N802
        from . import data

        self._options(date1904=bool(v))
        data.date_offset(refresh=True)

    @property
    def PrecisionAsDisplayed(self):  # noqa: N802
        """File > Options > Advanced > Set precision as displayed (saved with the workbook). True rounds every number,
        for good, to what its number format shows (0.00: two decimals; a percent: two more; a trailing comma: three
        fewer), and so every number written or calculated after; General, dates, times and fractions keep theirs."""
        return bool(self._options()["precision_as_displayed"])

    @PrecisionAsDisplayed.setter
    def PrecisionAsDisplayed(self, v):  # noqa: N802
        self._options(precision_as_displayed=bool(v))

    @property
    def ForceFullCalculation(self):  # noqa: N802
        """Every calculation calculates every formula of the workbook, not only those that wait (saved with it)."""
        return bool(self._options()["force_full_calc"])

    @ForceFullCalculation.setter
    def ForceFullCalculation(self, v):  # noqa: N802
        self._options(force_full_calc=bool(v))

    @property
    def ReadOnly(self):  # noqa: N802
        return bool(self._info().get("read_only"))

    @property
    def DisplayDrawingObjects(self):  # noqa: N802
        """xlDisplayShapes, or xlHide while shapes, pictures, charts and controls are hidden (Ctrl+6)."""
        return xlDisplayShapes if _rq("cells.display_objects") else xlHide

    @DisplayDrawingObjects.setter
    def DisplayDrawingObjects(self, v):  # noqa: N802
        _rq("cells.display_objects", hide=int(v) == xlHide)

    @property
    def Worksheets(self):  # noqa: N802
        return Worksheets if self._cx is None else _Worksheets(self._cx)

    @property
    def Sheets(self):  # noqa: N802
        return Sheets if self._cx is None else _Worksheets(self._cx, "all")

    @property
    def Charts(self):  # noqa: N802
        """The workbook's chart sheets (Charts.Add makes one from the selected data)."""
        return Charts if self._cx is None else _Worksheets(self._cx, "chart")

    @property
    def Names(self):  # noqa: N802
        return Names

    @property
    def ActiveSheet(self):  # noqa: N802
        return Application.ActiveSheet

    def _before_save(self, save_as_ui):
        """The host's own save of its workbook runs Workbook_BeforeSave here (YCGrid asks the host for every other
        save, but can't while the host waits for this one). False: a handler set Cancel."""
        if self._cx is not None:
            return True
        from . import host

        if host._conn_obj is None or _conn() is not host._conn_obj:
            return True
        return host.run_before_save(save_as_ui)

    def Save(self):  # noqa: N802
        if not self._before_save(False):
            return
        _rq("wb.save")

    def SaveAs(self, Filename, FileFormat=None, Password=None, WriteResPassword=None, ReadOnlyRecommended=None,  # noqa: N802,N803
               CreateBackup=None, AccessMode=None, ConflictResolution=None, AddToMru=None):
        """Save under a new name; Password encrypts the file (an empty string removes the encryption)."""
        if WriteResPassword:
            raise UnsupportedError("A password to modify (WriteResPassword) is not supported; use Password to encrypt the file")
        path = str(Filename)
        # a macro-enabled workbook (52, .xlsm) isn't written: YCGrid's .xlsx has no macros, and Excel refused a file
        # with .xlsm's name and .xlsx's content ("the file format or extension isn't valid")
        if (FileFormat is not None and int(FileFormat) == 52) or path.lower().endswith(".xlsm"):
            raise UnsupportedError("YCGrid doesn't save .xlsm (macro-enabled) workbooks: save as .xlsx (FileFormat=51) or .ycg")
        if FileFormat is not None and int(FileFormat) == 51 and not path.lower().endswith(".xlsx"):
            path += ".xlsx"  # xlOpenXMLWorkbook
        elif FileFormat is not None and int(FileFormat) in (6, 62) and not path.lower().endswith(".csv"):
            path += ".csv"  # xlCSV / xlCSVUTF8
        p = {"path": path}
        if Password is not None:
            p["password"] = str(Password)
        if not self._before_save(False):
            return
        _rq("wb.save", **p)

    @property
    def Password(self):  # noqa: N802
        """Write a password to encrypt the workbook on its next save ("" removes it); reading gives "********" like Excel."""
        return "********" if self.HasPassword else ""

    @Password.setter
    def Password(self, v):  # noqa: N802
        _rq("wb.password", password=str(v or ""))

    @property
    def HasPassword(self):  # noqa: N802
        return bool(_rq("wb.has_password"))

    def SaveCopyAs(self, Filename):  # noqa: N802,N803
        _rq("wb.save", path=str(Filename), copy=True)

    def Close(self, SaveChanges=None):  # noqa: N802,N803
        """Close the workbook (SaveChanges True saves first, False discards changes). This window's workbook is replaced
        by a new blank one and YCGrid stays open, like Excel after its last workbook closes (Application.Quit closes
        YCGrid); another window's workbook closes that window."""
        _rq("wb.close", save=None if SaveChanges is None else bool(SaveChanges), keep=self._cx is None)
        if self._cx is not None:
            _REMOTE.pop(self._pid, None)

    def Activate(self):  # noqa: N802
        """Bring the workbook's window to the front."""
        _rq("ui.activate")

    def __eq__(self, other):
        return isinstance(other, Workbook) and other._cx is self._cx

    def __hash__(self):
        return hash(id(self._cx))

    def __repr__(self):
        return "<Workbook ThisWorkbook>" if self._cx is None else f"<Workbook {self.Name!r}>"


# other YCGrid windows' workbooks: pid -> Workbook (connected on first use)
_REMOTE = {}


def _remote_workbook(info):
    from ._conn import Connection

    pid = info.get("pid", 0)
    wb = _REMOTE.get(pid)
    if wb is not None and not wb._cx.closed:
        return wb
    conn = Connection.open_controller(info["pipe"])
    conn.request("hello", {"role": "controller", "pid": _os_getpid()})
    wb = Workbook(conn, pid)
    _REMOTE[pid] = wb
    return wb


def _os_getpid():
    import os

    return os.getpid()


def _workbook_names(wb):
    """(Name, FullName) of an open workbook, or None for another window that closed while we asked (its connection is
    gone): a window closed by its user mustn't make Workbooks.Open or Workbooks(name) fail."""
    try:
        return wb.Name, wb.FullName
    except YcConnectionError:
        if getattr(wb, "_cx", None) is None:
            raise  # this window's own connection: a real error
        return None


def _other_instances():
    """The other running YCGrid windows (each holds one workbook)."""
    from ._conn import find_instances
    from . import _pid_alive

    own = _own_pid()
    out = []
    for i in find_instances():
        pid = i.get("pid", 0)
        if i.get("pipe") and pid != own and _pid_alive(pid):
            out.append(i)
    out.sort(key=lambda i: i.get("pid", 0))
    return out


def _own_pid():
    """The pid of the YCGrid this script talks to (its main connection, whatever object is running)."""
    from ._conn import main_connection

    c = main_connection()
    if c is None:
        return 0
    pid = getattr(c, "_yc_pid", None)
    if pid is None:
        pid = int(c.request("app.pid", {}))
        c._yc_pid = pid
    return pid


class _Workbooks:
    """The open workbooks: this one first, then the other YCGrid windows' (each YCGrid window holds one workbook)."""

    def _all(self):
        out = [ThisWorkbook]
        for i in _other_instances():
            try:
                out.append(_remote_workbook(i))
            except (OSError, YcError):
                continue
        return out

    def __call__(self, Index=1):  # noqa: N803
        wbs = self._all()
        if isinstance(Index, int) and not isinstance(Index, bool):
            if not 1 <= Index <= len(wbs):
                raise SubscriptOutOfRangeError(f"Workbooks({Index})")
            return wbs[Index - 1]
        import os

        want = str(Index).lower()
        for wb in wbs:
            names = _workbook_names(wb)
            if names is None:
                continue
            name, full = names[0].lower(), names[1].lower()
            if name == want or os.path.splitext(name)[0] == want or full == want:
                return wb
        raise SubscriptOutOfRangeError(f"No open workbook named {Index!r}")

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return len(self._all())

    def __len__(self):
        return len(self._all())

    def __iter__(self):
        return iter(self._all())

    def _wait_for(self, match, timeout=30.0):
        import time

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for i in _other_instances():
                if match(i):
                    try:
                        return _remote_workbook(i)
                    except (OSError, YcError):
                        pass
            time.sleep(0.1)
        raise ApplicationDefinedError("The new YCGrid window didn't start in time")

    def Open(self, Filename, UpdateLinks=None, ReadOnly=False, Format=None, Password=None, WriteResPassword=None,  # noqa: N802,N803
             IgnoreReadOnlyRecommended=None, Origin=None, Delimiter=None, Editable=None, Notify=None, Converter=None, AddToMru=None,
             Local=None, CorruptLoad=None):
        """Open a workbook (.ycg, .xlsx, .csv) like Excel: an empty Book1 is replaced, otherwise it opens in a new YCGrid window
        and the returned Workbook works with it (Worksheets, Range values and formulas, Save, Close). Password opens an
        encrypted workbook."""
        import os

        path = os.path.abspath(str(Filename))
        for wb in self._all():
            names = _workbook_names(wb)
            if names and names[1] and os.path.normcase(names[1]) == os.path.normcase(path):
                return wb  # already open
        before = {i.get("pid") for i in _other_instances()}
        r = _rq("wb.open_window", path=path, **({"password": str(Password)} if Password is not None else {}))
        if r.get("here"):
            return ThisWorkbook
        want = os.path.normcase(path)

        def match(i):
            # the new window couldn't read the file: its error, not its blank workbook (Excel raises error 1004)
            if i.get("pid") not in before and os.path.normcase(i.get("open_failed") or "") == want:
                try:  # no window is left behind, as in Excel
                    _remote_workbook(i).Close(SaveChanges=False)
                except (OSError, YcError):
                    pass
                raise ApplicationDefinedError(i.get("open_error") or f"The workbook couldn't be opened: {path}")
            # an Excel or CSV file opens untitled: its window names it as the source
            return want in (os.path.normcase(i.get("file") or ""), os.path.normcase(i.get("source") or ""))

        return self._wait_for(match)

    def OpenText(self, Filename, Origin=None, StartRow=1, DataType=None, TextQualifier=None,  # noqa: N802,N803
                 ConsecutiveDelimiter=False, Tab=False, Semicolon=False, Comma=False, Space=False, Other=False, OtherChar=None,
                 FieldInfo=None, TextVisualLayout=None, DecimalSeparator=None, ThousandsSeparator=None,
                 TrailingMinusNumbers=None, Local=None):
        """Opens a text file as a new workbook, split into columns as Data > Text to Columns does with the same
        arguments (FieldInfo [[column, xlTextFormat], ...] keeps codes such as 0050 as text; xlSkipColumn leaves a
        column out). Origin is the file's code page (65001 UTF-8, 950 Big5, xlWindows the system's; by default a byte
        order mark or UTF-8, else the system's); StartRow skips the lines before it. The options apply whatever the
        file's extension. Returns the new workbook (untitled, as YCGrid opens a CSV), its sheet named after the file."""
        import os

        path = os.path.abspath(str(Filename))
        with open(path, "rb") as f:
            raw = f.read()
        lines = _decode_text(raw, Origin).splitlines()[max(0, int(StartRow or 1) - 1):]
        wb = self.Add()
        ws = wb.Worksheets(1)
        try:
            ws.Name = os.path.splitext(os.path.basename(path))[0][:31] or ws.Name
        except YcError:
            pass  # a name Excel refuses ([, ], :, ...): the sheet keeps its own
        if lines:
            # the lines as they are (a Text column takes them without reading numbers), then column A back to General
            # before Text to Columns gives each column its type
            col = ws.Range(f"A1:A{len(lines)}")
            col.NumberFormat = "@"
            col.Value = [[ln] for ln in lines]
            col.NumberFormat = "General"
            ws.Range(f"A1:A{len(lines)}").TextToColumns(
                Destination=ws.Range("A1"), DataType=xlDelimited if DataType is None else DataType,
                TextQualifier=xlTextQualifierDoubleQuote if TextQualifier is None else TextQualifier,
                ConsecutiveDelimiter=ConsecutiveDelimiter, Tab=Tab, Semicolon=Semicolon, Comma=Comma, Space=Space,
                Other=Other, OtherChar=OtherChar, FieldInfo=FieldInfo, DecimalSeparator=DecimalSeparator,
                ThousandsSeparator=ThousandsSeparator, TrailingMinusNumbers=TrailingMinusNumbers)
            ws.Range("A1").Select()
        return wb

    def Add(self, Template=None):  # noqa: N802,N803
        """A new workbook: in this window when it is empty, else in a new YCGrid window (returned)."""
        before = {i.get("pid") for i in _other_instances()}
        r = _rq("wb.new_window")
        if r.get("here"):
            return ThisWorkbook
        pid = r.get("pid")
        if pid:
            # the window this call started: any YCGrid started meanwhile (another script, a person) was taken for it,
            # and Workbooks.OpenText wrote its file into that window (gaps_fuzz, two runs side by side)
            return self._wait_for(lambda i: i.get("pid") == pid)
        return self._wait_for(lambda i: i.get("pid") not in before)


# ---------------------------------------------------------------------------
# Worksheets, Worksheet
# ---------------------------------------------------------------------------
def _date_offset() -> float:
    """1462 in a workbook with the 1904 date system, else 0 (data.date_offset)."""
    from .data import date_offset

    return date_offset()


def _decode_text(raw: bytes, origin=None) -> str:
    """A text file's bytes as text: Origin as Workbooks.OpenText takes it (a code page number, xlWindows, xlMSDOS,
    xlMacintosh), or a byte order mark, UTF-8, else the system's code page."""
    codecs = {xlWindows: "mbcs", xlMSDOS: "oem", xlMacintosh: "mac-roman", 65001: "utf-8", 1200: "utf-16-le",
              1201: "utf-16-be", 65000: "utf-7"}
    if origin is not None:
        o = int(origin)
        name = codecs.get(o, f"cp{o}")
        return raw.decode("utf-8-sig" if name == "utf-8" else name, errors="replace")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", errors="replace")
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", errors="replace")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("mbcs", errors="replace")


def _sheets_info():
    return _rq("wb.info")["sheets"]


def _sheet_object(info):
    """A Worksheet, or a ChartSheet for a chart sheet."""
    return ChartSheet(info["id"]) if info.get("chart") else Worksheet(info["id"])


class _Worksheets:
    """Worksheets (kind "work"), Sheets (every sheet, chart sheets too) or Charts (chart sheets)."""

    def __init__(self, cx=None, kind="work"):
        self._cx = cx
        self._kind = kind

    def _infos(self):
        sheets = _sheets_info()
        if self._kind == "work":
            return [s for s in sheets if not s.get("chart")]
        if self._kind == "chart":
            return [s for s in sheets if s.get("chart")]
        return sheets

    def __call__(self, Index=None):  # noqa: N803
        if Index is None:
            return self
        if isinstance(Index, (list, tuple)):  # Sheets(Array("Sheet1", "Sheet2")): several sheets, to group or fill across
            return _SheetGroup([self(i) for i in Index])
        sheets = self._infos()
        if isinstance(Index, bool) or not isinstance(Index, (int, str)):
            raise TypeMismatchError()
        if isinstance(Index, int):
            if not 1 <= Index <= len(sheets):
                raise SubscriptOutOfRangeError(f"Sheet index out of range: {Index}")
            return _sheet_object(sheets[Index - 1])
        for s in sheets:
            if s["name"].lower() == Index.lower():
                return _sheet_object(s)
        raise SubscriptOutOfRangeError(f"Sheet not found: {Index}")

    Item = __call__

    def __getitem__(self, Index):  # noqa: N803
        return self(Index + 1 if isinstance(Index, int) else Index)

    def __iter__(self):
        return iter([_sheet_object(s) for s in self._infos()])

    def __len__(self):
        return len(self._infos())

    @property
    def Count(self):  # noqa: N802
        return len(self._infos())

    def Add(self, Before=None, After=None, Count=1, Type=None, Name=None):  # noqa: N802,N803
        """A new worksheet; on Charts (or with Type=xlChart) a chart sheet of the selected data."""
        chart = self._kind == "chart" or (Type is not None and int(Type) == xlChart)
        last = None
        for _ in range(int(Count)):
            index = None
            if Before is not None:
                index = Before.Index - 1
            elif After is not None:
                index = After.Index
            else:
                # neither: before the active sheet, as in Excel (Sheets.Add(After:=Sheets(Sheets.Count)) adds at the end)
                ids = [i["id"] for i in _sheets_info()]
                focus = int(_rq("view.focus"))
                if focus in ids:
                    index = ids.index(focus)
            if chart:
                p = {"name": Name or ""}
                if index is not None:
                    p["index"] = index
                last = ChartSheet(_rq("chartsheet.add", **p))
            else:
                r = _rq("sheet.add", name=Name or "", index=index)
                last = Worksheet(r["id"])
                last.Activate()
        return last

    def Add2(self, Before=None, After=None, Count=1, NewLayout=None):  # noqa: N802,N803
        return self.Add(Before=Before, After=After, Count=Count, Type=xlChart if self._kind == "chart" else None)

    def __contains__(self, name):
        return any(s["name"].lower() == str(name).lower() for s in self._infos())

    def Exists(self, Name):  # noqa: N802,N803
        return Name in self

    def Select(self, Replace=True):  # noqa: N802,N803
        """Group every sheet of the collection (Select All Sheets): what is typed or formatted on one goes to all."""
        _SheetGroup(list(self)).Select(Replace)

    def FillAcrossSheets(self, Range, Type=xlFillWithAll):  # noqa: N802,N803
        """Home > Fill > Across Worksheets: copy Range (all, contents or formats) to the same cells of every sheet here."""
        _SheetGroup(list(self)).FillAcrossSheets(Range, Type)


class _SheetGroup:
    """Several sheets: Sheets(Array(...)), Window.SelectedSheets. Select groups them (Excel's [Group])."""

    def __init__(self, sheets):
        self._sheets = list(sheets)

    def __iter__(self):
        return iter(self._sheets)

    def __len__(self):
        return len(self._sheets)

    def __getitem__(self, i):
        return self._sheets[i]

    def __call__(self, Index):  # noqa: N803
        if isinstance(Index, int):
            return self._sheets[Index - 1]
        for s in self._sheets:
            if s.Name.lower() == str(Index).lower():
                return s
        raise SubscriptOutOfRangeError(f"Sheet not found: {Index}")

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return len(self._sheets)

    def Select(self, Replace=True):  # noqa: N802,N803
        ids = [s._id for s in self._sheets]
        if Replace:
            _rq("cells.group_set", ids=ids)
        else:
            _rq("cells.group_set", ids=ids, add=True)

    def FillAcrossSheets(self, Range, Type=xlFillWithAll):  # noqa: N802,N803
        kind = {xlFillWithAll: 0, xlFillWithContents: 1, xlFillWithFormats: 2}.get(Type)
        if kind is None:
            raise ValueError("Type must be xlFillWithAll, xlFillWithContents or xlFillWithFormats")
        _rq("cells.fill_across", ids=[s._id for s in self._sheets], sheet=Range._sheet, areas=[list(a) for a in Range._areas], kind=kind)


class _Pictures:
    def __init__(self, sheet_id):
        self._sheet = sheet_id

    def Paste(self, Destination=None, Link=False):  # noqa: N802,N803
        """Paste Options > Picture (Link=True: Linked Picture) of the copied cells (Range.Copy first) at the active cell."""
        _rq("view.set", activate_sheet=self._sheet)
        _rq("cells.paste_picture", link=bool(Link))


Worksheets = _Worksheets()
Sheets = _Worksheets(kind="all")
Charts = _Worksheets(kind="chart")


class _Tab:
    def __init__(self, ws):
        self._ws = ws

    @property
    def Color(self):  # noqa: N802
        argb = self._ws._info()["tab_color"]
        return _argb_to_bgr(argb) if argb else xlNone

    @Color.setter
    def Color(self, v):  # noqa: N802
        _rq("sheet.tab_color", id=self._ws._id, color=0 if v in (xlNone, None, False) else _color_to_argb(v))


class _AllowEditRange:
    def __init__(self, sheet, index):
        self._sheet, self._index = sheet, index

    def _all(self):
        return [list(r) for r in _rq("protect.get", sheet=self._sheet)["ranges"]]

    @property
    def Title(self):  # noqa: N802
        return self._all()[self._index][0]

    @property
    def Range(self):  # noqa: N802
        r = self._all()[self._index]
        return Range._from(self._sheet, [tuple(r[1:5])])

    def ChangePassword(self, Password):  # noqa: N802,N803
        rs = self._all()
        rs[self._index][5] = str(Password or "")
        _rq("protect.ranges", sheet=self._sheet, ranges=rs)

    @property
    def Users(self):  # noqa: N802
        """Permissions...: the Windows accounts ("DOMAIN\\user") that edit the range without its password."""
        return _UserAccessList(self)

    def Delete(self):  # noqa: N802
        rs = self._all()
        del rs[self._index]
        _rq("protect.ranges", sheet=self._sheet, ranges=rs)


class _UserAccess:
    def __init__(self, lst, i):
        self._l, self._i = lst, i

    @property
    def Name(self):  # noqa: N802
        return self._l._users()[self._i][0]

    @property
    def AllowEdit(self):  # noqa: N802
        return bool(self._l._users()[self._i][1])

    @AllowEdit.setter
    def AllowEdit(self, v):  # noqa: N802
        us = self._l._users()
        us[self._i][1] = bool(v)
        self._l._store(us)

    def Delete(self):  # noqa: N802
        us = self._l._users()
        del us[self._i]
        self._l._store(us)


class _UserAccessList:
    """AllowEditRange.Users (Excel's UserAccessList): Count, Item(Index or Name), Add(Name, AllowEdit), DeleteAll()."""

    def __init__(self, rng):
        self._r = rng

    def _users(self):
        r = self._r._all()[self._r._index]
        return [list(u) for u in (r[6] if len(r) > 6 else [])]

    def _store(self, us):
        rs = self._r._all()
        r = rs[self._r._index]
        rs[self._r._index] = r[:6] + [us]
        _rq("protect.ranges", sheet=self._r._sheet, ranges=rs)

    @property
    def Count(self):  # noqa: N802
        return len(self._users())

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        us = self._users()
        if isinstance(Index, str):
            for i, u in enumerate(us):
                if u[0].lower() == Index.lower():
                    return _UserAccess(self, i)
            raise SubscriptOutOfRangeError(f"No user named {Index!r}")
        if not 1 <= int(Index) <= len(us):
            raise SubscriptOutOfRangeError("Users index out of range")
        return _UserAccess(self, int(Index) - 1)

    def __call__(self, Index):  # noqa: N803
        return self.Item(Index)

    def Add(self, Name, AllowEdit=True):  # noqa: N802,N803
        us = [u for u in self._users() if u[0].lower() != str(Name).lower()]
        us.append([str(Name), bool(AllowEdit)])
        self._store(us)
        return _UserAccess(self, len(us) - 1)

    def DeleteAll(self):  # noqa: N802
        self._store([])


class _AllowEditRanges:
    def __init__(self, sheet):
        self._sheet = sheet

    def _all(self):
        return [list(r) for r in _rq("protect.get", sheet=self._sheet)["ranges"]]

    @property
    def Count(self):  # noqa: N802
        return len(self._all())

    def __len__(self):
        return self.Count

    def Add(self, Title, Range, Password=None):  # noqa: N802,N803
        """A range users may edit while the sheet is protected (Password: asked for before the first change)."""
        rng = Range if not isinstance(Range, str) else _make_range(self._sheet, Range, None)
        a = rng._a
        rs = self._all()
        rs.append([str(Title), a[0], a[1], a[2], a[3], str(Password) if Password else 0])
        _rq("protect.ranges", sheet=self._sheet, ranges=rs)
        return _AllowEditRange(self._sheet, len(rs) - 1)

    def Item(self, Index):  # noqa: N802,N803
        rs = self._all()
        if isinstance(Index, str):
            for i, r in enumerate(rs):
                if r[0].lower() == Index.lower():
                    return _AllowEditRange(self._sheet, i)
            raise SubscriptOutOfRangeError(f"No edit range named {Index!r}")
        return _AllowEditRange(self._sheet, int(Index) - 1)

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        for i in range(self.Count):
            yield _AllowEditRange(self._sheet, i)


class _Protection:
    """Worksheet.Protection: what users may still do on the protected sheet (read-only, set with Protect)."""

    _MAP = {"AllowFormattingCells": "format_cells", "AllowFormattingColumns": "format_columns", "AllowFormattingRows": "format_rows",
            "AllowInsertingColumns": "insert_columns", "AllowInsertingRows": "insert_rows", "AllowInsertingHyperlinks": "insert_hyperlinks",
            "AllowDeletingColumns": "delete_columns", "AllowDeletingRows": "delete_rows", "AllowSorting": "sort", "AllowFiltering": "autofilter",
            "AllowUsingPivotTables": "pivot"}

    def __init__(self, sheet):
        self._sheet = sheet

    def __getattr__(self, name):
        key = _Protection._MAP.get(name)
        if key is None:
            raise AttributeError(name)
        return bool(_rq("protect.get", sheet=self._sheet)["flags"][key])

    @property
    def AllowEditRanges(self):  # noqa: N802
        return _AllowEditRanges(self._sheet)


# ---------------------------------------------------------------------------
# Tables (ListObjects)
# ---------------------------------------------------------------------------
_TOTALS_TO_SUBTOTAL = {0: 0, 1: 109, 2: 101, 3: 103, 4: 102, 5: 105, 6: 104, 7: 107, 8: 110}
_SUBTOTAL_TO_TOTALS = {v: k for k, v in _TOTALS_TO_SUBTOTAL.items()}


class ListColumn:
    def __init__(self, table, index):
        self._t, self._i = table, index  # 0-based

    @property
    def Name(self):  # noqa: N802
        return self._t._get()["columns"][self._i][0]

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._t.HeaderRowRange.Cells(1, self._i + 1).Value = str(v)

    @property
    def Index(self):  # noqa: N802
        return self._i + 1

    @property
    def Total(self):  # noqa: N802
        """The column's cell in the total row (None while the table shows no total row)."""
        t = self._t._get()
        if not t["totals"]:
            return None
        r = t["range"]
        return Range._from(self._t._sheet, [(r[2], r[1] + self._i, r[2], r[1] + self._i)])

    @property
    def Range(self):  # noqa: N802
        r = self._t._get()["range"]
        return Range._from(self._t._sheet, [(r[0], r[1] + self._i, r[2], r[1] + self._i)])

    @property
    def DataBodyRange(self):  # noqa: N802
        d = self._t.DataBodyRange
        return None if d is None else d.Columns(self._i + 1)

    @property
    def TotalsCalculation(self):  # noqa: N802
        return _SUBTOTAL_TO_TOTALS.get(self._t._get()["columns"][self._i][1], 0)

    @TotalsCalculation.setter
    def TotalsCalculation(self, v):  # noqa: N802
        _rq("table.total", sheet=self._t._sheet, id=self._t._id, col=self._i, func=_TOTALS_TO_SUBTOTAL.get(int(v), 0), label="")

    def Delete(self):  # noqa: N802
        _rq("table.column", sheet=self._t._sheet, id=self._t._id, add=False, pos=self._i)


class ListRow:
    def __init__(self, table, index):
        self._t, self._i = table, index  # 0-based data row

    @property
    def Index(self):  # noqa: N802
        return self._i + 1

    @property
    def Range(self):  # noqa: N802
        return self._t.DataBodyRange.Rows(self._i + 1)

    def Delete(self):  # noqa: N802
        _rq("table.row", sheet=self._t._sheet, id=self._t._id, add=False, pos=self._i)


class _ListColumns:
    def __init__(self, table):
        self._t = table

    @property
    def Count(self):  # noqa: N802
        return len(self._t._get()["columns"])

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        cols = self._t._get()["columns"]
        if isinstance(Index, str):
            for i, c in enumerate(cols):
                if c[0].lower() == Index.lower():
                    return ListColumn(self._t, i)
            raise SubscriptOutOfRangeError(f"No column named {Index!r}")
        i = int(Index) - 1
        if not 0 <= i < len(cols):
            raise SubscriptOutOfRangeError("ListColumns index out of range")
        return ListColumn(self._t, i)

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        for i in range(self.Count):
            yield ListColumn(self._t, i)

    def Add(self, Position=None):  # noqa: N802,N803
        _rq("table.column", sheet=self._t._sheet, id=self._t._id, add=True, pos=-1 if Position is None else int(Position) - 1)
        n = self.Count
        return ListColumn(self._t, n - 1 if Position is None else int(Position) - 1)


class _ListRows:
    def __init__(self, table):
        self._t = table

    @property
    def Count(self):  # noqa: N802
        d = self._t.DataBodyRange
        return 0 if d is None else d.Rows.Count

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        i = int(Index) - 1
        if not 0 <= i < self.Count:
            raise SubscriptOutOfRangeError("ListRows index out of range")
        return ListRow(self._t, i)

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        for i in range(self.Count):
            yield ListRow(self._t, i)

    def Add(self, Position=None, AlwaysInsert=True):  # noqa: N802,N803
        """A new data row (at Position, 1-based; at the end by default). Calculated columns continue into it.
        Position goes from 1 to Count + 1 (Excel's DISP_E_BADINDEX past that)."""
        if Position is not None and not 1 <= int(Position) <= self.Count + 1:
            raise SubscriptOutOfRangeError(f"Invalid index: ListRows.Add takes a Position from 1 to {self.Count + 1}")
        _rq("table.row", sheet=self._t._sheet, id=self._t._id, add=True, pos=-1 if Position is None else int(Position) - 1)
        return ListRow(self._t, self.Count - 1 if Position is None else int(Position) - 1)


class ListObject:
    """A table (Insert -> Table). Formulas can use structured references such as Table1[Qty] and [@Qty]."""

    def __init__(self, sheet_id, table_id):
        self._sheet, self._id = sheet_id, table_id

    def __repr__(self):
        return f"<ListObject {self.Name!r}>"

    def __eq__(self, other):
        return isinstance(other, ListObject) and other._id == self._id

    def _get(self):
        for t in _rq("table.list", sheet=self._sheet):
            if t["id"] == self._id:
                return t
        raise SubscriptOutOfRangeError("The table no longer exists")

    def _set(self, **props):
        _rq("table.update", sheet=self._sheet, id=self._id, props=props)

    @property
    def Name(self):  # noqa: N802
        return self._get()["name"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._set(name=str(v))

    DisplayName = Name

    @property
    def Parent(self):  # noqa: N802
        return Worksheet(self._sheet)

    @property
    def Range(self):  # noqa: N802
        t = self._get()
        r0, c0, r1, c1 = t["range"]
        # a table without data rows keeps its empty insert row, as Excel reports it (A1:C2 with headers on row 1)
        if t["header"] and not t["totals"] and r0 == r1:
            r1 += 1
        return Range._from(self._sheet, [(r0, c0, r1, c1)])

    @property
    def HeaderRowRange(self):  # noqa: N802
        t = self._get()
        r = t["range"]
        return Range._from(self._sheet, [(r[0], r[1], r[0], r[3])]) if t["header"] else None

    @property
    def TotalsRowRange(self):  # noqa: N802
        t = self._get()
        r = t["range"]
        return Range._from(self._sheet, [(r[2], r[1], r[2], r[3])]) if t["totals"] else None

    @property
    def Slicers(self):  # noqa: N802
        """The slicers that filter this table."""
        out = []
        for sc in ActiveWorkbook.SlicerCaches:
            lo = getattr(sc, "ListObject", None)
            if lo is not None and getattr(lo, "Name", None) == self.Name and getattr(lo, "_sheet", None) == self._sheet:
                out.extend(sc.Slicers)
        return out

    @property
    def DataBodyRange(self):  # noqa: N802
        t = self._get()
        r0, c0, r1, c1 = t["range"]
        if t["header"]:
            r0 += 1
        if t["totals"]:
            r1 -= 1
        return Range._from(self._sheet, [(r0, c0, r1, c1)]) if r0 <= r1 else None

    @property
    def ListColumns(self):  # noqa: N802
        return _ListColumns(self)

    @property
    def ListRows(self):  # noqa: N802
        return _ListRows(self)

    def _flag(name, key):  # noqa: N805
        def get(self):
            return bool(self._get()[key])

        def put(self, v):
            self._set(**{key: bool(v)})

        return property(get, put, doc=name)

    ShowTotals = _flag("ShowTotals", "totals")
    ShowHeaders = _flag("ShowHeaders", "header")
    ShowTableStyleRowStripes = _flag("ShowTableStyleRowStripes", "banded_rows")
    ShowTableStyleColumnStripes = _flag("ShowTableStyleColumnStripes", "banded_cols")
    ShowTableStyleFirstColumn = _flag("ShowTableStyleFirstColumn", "first_col")
    ShowTableStyleLastColumn = _flag("ShowTableStyleLastColumn", "last_col")
    ShowAutoFilterDropDown = _flag("ShowAutoFilterDropDown", "filter_button")
    del _flag

    @property
    def ShowAutoFilter(self):  # noqa: N802
        return bool(self._get()["filter_button"])

    @ShowAutoFilter.setter
    def ShowAutoFilter(self, v):  # noqa: N802
        self._set(filter_button=bool(v))

    @property
    def AutoFilter(self):  # noqa: N802
        """The table's AutoFilter (Filters, ShowAllData, ApplyFilter, Sort): the sheet's when it is on the table, or the
        table's own buttons (every table has its own, as in Excel); None when the Filter Button is off."""
        from . import rules

        af = _rq("af.get", sheet=self._sheet, table=self._id)
        return rules.AutoFilter(self._sheet, self._id) if af and af.get("on") else None

    @property
    def TableStyle(self):  # noqa: N802
        return self._get()["style"]

    @TableStyle.setter
    def TableStyle(self, v):  # noqa: N802
        self._set(style=str(getattr(v, "Name", v)))

    @property
    def Sort(self):  # noqa: N802
        """The table's Sort object (SortFields.Clear / Add, Header, Apply), kept with the table as in Excel: a
        recorded macro's ListObjects("Table1").Sort.SortFields.Add ... .Sort.Apply sorts the data rows."""
        key = (self._sheet, self._id)
        s = _TABLE_SORTS.get(key)
        if s is None:
            s = _TABLE_SORTS[key] = _TableSort(self)
        return s

    def Resize(self, Range):  # noqa: N802,N803
        """Change the table's range (the header row stays where it is)."""
        _one_area(Range)
        _rq("table.resize", sheet=self._sheet, id=self._id, rect=list(Range._a))

    def Unlist(self):  # noqa: N802
        """Convert the table to a normal range (its look stays as cell formats)."""
        _rq("table.delete", sheet=self._sheet, id=self._id, keep_look=True)

    def Delete(self):  # noqa: N802
        """Delete the table and its cells' contents and formats."""
        _rq("table.delete", sheet=self._sheet, id=self._id, keep_look=False, clear=True)


# ---- PivotTables (Insert -> PivotTable) ----
_PIVOT_FUNCS = [xlSum, xlCount, xlAverage, xlMax, xlMin, xlProduct, xlCountNums, xlStDev, xlStDevP, xlVar, xlVarP, xlDistinctCount]
_PIVOT_SHOW = {xlNoAdditionalCalculation: 0, xlNormal: 0, xlPercentOfTotal: 1, xlPercentOfColumn: 2, xlPercentOfRow: 3, xlRunningTotal: 4,
               xlPercentOf: 5, xlPercentOfParentRow: 6, xlPercentOfParentColumn: 7, xlPercentOfParent: 8, xlDifferenceFrom: 9,
               xlPercentDifferenceFrom: 10, xlPercentRunningTotal: 11, xlRankAscending: 12, xlRankDecending: 13, xlIndex: 14}
_PIVOT_SHOW_BACK = {0: xlNoAdditionalCalculation, 1: xlPercentOfTotal, 2: xlPercentOfColumn, 3: xlPercentOfRow, 4: xlRunningTotal, 5: xlPercentOf,
                    6: xlPercentOfParentRow, 7: xlPercentOfParentColumn, 8: xlPercentOfParent, 9: xlDifferenceFrom, 10: xlPercentDifferenceFrom,
                    11: xlPercentRunningTotal, 12: xlRankAscending, 13: xlRankDecending, 14: xlIndex}
# PivotField.Subtotals: Excel's 12 booleans (Automatic, Sum, Count, Average, Max, Min, Product, Count Nums, StdDev, StdDevp, Var, Varp)
_SUBTOTAL_FUNCS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]


def _alerts():
    """Application.DisplayAlerts: a report growing over data raises (as Excel would ask) unless it is False."""
    return bool(getattr(Application, "DisplayAlerts", True))
_AXES = {xlRowField: "rows", xlColumnField: "cols", xlPageField: "pages"}


def _pivot_func(v) -> int:
    try:
        return _PIVOT_FUNCS.index(int(v))
    except ValueError:
        raise ApplicationDefinedError(f"Unsupported PivotTable function: {v}") from None


class PivotItem:
    def __init__(self, field, name, shown=None):
        self._f, self._name, self._shown = field, name, shown

    def __repr__(self):
        return f"<PivotItem {self.Name}>"

    @property
    def Name(self):  # noqa: N802
        """The item as the report shows it (a date in the source's format, a renamed item's new name)."""
        if self._shown is None:
            for idt, shown in _rq("pivot.items", sheet=self._f._pt._sheet, name=self._f._pt._name, field=self._f._index(), display=True):
                if idt == self._name:
                    self._shown = shown
            if self._shown is None:
                self._shown = self._name
        return self._shown

    @Name.setter
    def Name(self, v):  # noqa: N802
        self.Caption = v

    Value = Name

    def _item_cells(self, key):
        pt = self._f._pt
        r = _rq("pivot.item_ranges", sheet=pt._sheet, name=pt._name, field=self._f._index(), item=self._name)
        if not r or r.get(key) is None:
            raise ApplicationDefinedError(f"The item {self._name} isn't on the PivotTable report")
        return Range._from(pt._sheet, [tuple(r[key])])

    @property
    def LabelRange(self):  # noqa: N802
        """The cell of the item's label on the report."""
        return self._item_cells("label")

    @property
    def DataRange(self):  # noqa: N802
        """The values of the rows (or columns) the item holds, its subtotal included, without the grand totals."""
        return self._item_cells("data")

    @property
    def SourceName(self):  # noqa: N802
        """[YC] The item's identity: its value in General format (what filters, slicers and files name)."""
        return self._name

    @property
    def Caption(self):  # noqa: N802
        return self.Name

    @Caption.setter
    def Caption(self, v):  # noqa: N802
        d = self._f._pt._get()
        found = self._f._find(d)
        if found is None:
            raise ApplicationDefinedError("Only items of fields on Rows, Columns or Filters can be renamed")
        caps = found[1].get("item_captions", [])
        pairs = [(caps[i], caps[i + 1]) for i in range(0, len(caps) - 1, 2) if caps[i] != self._name]
        if str(v) != self._name:
            pairs.append((self._name, str(v)))
        found[1]["item_captions"] = [x for pr in pairs for x in pr]
        self._f._pt._put(d)
        self._shown = None

    @property
    def ShowDetail(self):  # noqa: N802
        """False when the item is collapsed (expand / collapse)."""
        axis = self._f._axis()
        if axis is None:
            return True
        e = axis[1]
        listed = self._name in e.get("collapsed", [])
        return not (not listed if e.get("collapse_all") else listed)

    @ShowDetail.setter
    def ShowDetail(self, v):  # noqa: N802
        d = self._f._pt._get()
        found = self._f._find(d)
        if found is None:
            raise ApplicationDefinedError("Only items of fields on Rows or Columns can be expanded or collapsed")
        e = found[1]
        lst = [x for x in e.get("collapsed", []) if x != self._name]
        if (not bool(v)) != bool(e.get("collapse_all")):
            lst.append(self._name)
        e["collapsed"] = lst
        self._f._pt._put(d)

    @property
    def Position(self):  # noqa: N802
        """The item's place among its field's items as the report shows them (manual order, sorting)."""
        names = _rq("pivot.items", sheet=self._f._pt._sheet, name=self._f._pt._name, field=self._f._index(), order=True)
        return names.index(self._name) + 1 if self._name in names else 0

    @Position.setter
    def Position(self, v):  # noqa: N802
        """Move the item among its field's items (the field's sort becomes manual)."""
        d = self._f._pt._get()
        found = self._f._find(d)
        if found is None:
            raise ApplicationDefinedError("Only items of fields on Rows or Columns can be moved")
        order = _rq("pivot.items", sheet=self._f._pt._sheet, name=self._f._pt._name, field=self._f._index(), order=True)
        order = [x for x in order if x != self._name]
        order.insert(max(0, min(len(order), int(v) - 1)), self._name)
        e = found[1]
        e["manual_order"], e["descending"], e["sort_by"] = order, False, -1
        self._f._pt._put(d)

    @property
    def Visible(self):  # noqa: N802
        axis = self._f._axis()
        return axis is None or self._name not in axis[1]["hidden"]

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        d = self._f._pt._get()
        found = self._f._find(d)
        if found is None:
            raise ApplicationDefinedError("Only fields on Rows, Columns or Filters can be filtered")
        hidden = found[1]["hidden"]
        if v and self._name in hidden:
            hidden.remove(self._name)
        elif not v and self._name not in hidden:
            hidden.append(self._name)
        self._f._pt._put(d)

    def _calc(self, d=None):
        d = d or self._f._pt._get()
        f = self._f._index(d)
        for c in d.get("calc_items", []):
            if c["field"] == f and c["name"].lower() == self._name.lower():
                return c
        return None

    @property
    def IsCalculated(self):  # noqa: N802
        return self._calc() is not None

    @property
    def Formula(self):  # noqa: N802
        c = self._calc()
        return c["formula"] if c else ""

    @Formula.setter
    def Formula(self, v):  # noqa: N802
        d = self._f._pt._get()
        c = self._calc(d)
        if c is None:
            raise ApplicationDefinedError(f"{self._name} is not a calculated item")
        c["formula"] = str(v)
        self._f._pt._put(d)

    def Delete(self):  # noqa: N802
        """Delete a calculated item."""
        d = self._f._pt._get()
        f = self._f._index(d)
        d["calc_items"] = [c for c in d.get("calc_items", []) if not (c["field"] == f and c["name"].lower() == self._name.lower())]
        self._f._pt._put(d)


class _CalculatedItems:
    """PivotField.CalculatedItems(): items made of the field's other items (=East+West, =North*1.1)."""

    def __init__(self, field):
        self._f = field

    def _list(self, d=None):
        d = d or self._f._pt._get()
        f = self._f._index(d)
        return [c for c in d.get("calc_items", []) if c["field"] == f]

    def Add(self, Name, Formula, UseStandardFormula=False):  # noqa: N802,N803
        d = self._f._pt._get()
        f = self._f._index(d)
        d["calc_items"] = [c for c in d.get("calc_items", []) if not (c["field"] == f and c["name"].lower() == str(Name).lower())]
        d["calc_items"].append({"field": f, "name": str(Name), "formula": str(Formula)})
        self._f._pt._put(d)
        return PivotItem(self._f, str(Name))

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        cs = self._list()
        if isinstance(Index, int):
            if not 1 <= Index <= len(cs):
                raise SubscriptOutOfRangeError("CalculatedItems index out of range")
            return PivotItem(self._f, cs[Index - 1]["name"])
        for c in cs:
            if c["name"].lower() == str(Index).lower():
                return PivotItem(self._f, c["name"])
        raise SubscriptOutOfRangeError(f"Calculated item not found: {Index}")

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([PivotItem(self._f, c["name"]) for c in self._list()])


class _PivotFilter:
    def __init__(self, field, kind):
        self._f, self._kind = field, kind

    def _e(self):
        found = self._f._find(self._f._pt._get())
        return found[1][self._kind] if found else {"type": 0, "data": 0, "v1": "", "v2": ""}

    FilterType = property(lambda self: self._e()["type"])
    Value1 = property(lambda self: self._e()["v1"])
    Value2 = property(lambda self: self._e()["v2"])

    @property
    def DataField(self):  # noqa: N802
        return self._f._pt.DataFields(self._e()["data"] + 1) if self._kind == "value_filter" else None

    def Delete(self):  # noqa: N802
        d = self._f._pt._get()
        found = self._f._find(d)
        if found:
            found[1][self._kind] = {"type": 0, "data": 0, "v1": "", "v2": ""}
            self._f._pt._put(d)


class _DataPivotField:
    """PivotTable.DataPivotField: the Values field; its PivotItems are the data fields."""

    def __init__(self, pt):
        self._pt = pt

    @property
    def Name(self):  # noqa: N802
        return "Values"

    Caption = Value = Name

    @property
    def Orientation(self):  # noqa: N802
        d = self._pt._get()
        if len(d["data"]) < 2:
            return xlHidden
        return xlRowField if d.get("values_on_rows") else xlColumnField

    def PivotItems(self, Index=None):  # noqa: N802,N803
        caps = list(self._pt._get().get("captions", []))
        items = [_DataPivotItem(self, c) for c in caps]
        if Index is None:
            return items
        if isinstance(Index, int):
            if not 1 <= Index <= len(items):
                raise SubscriptOutOfRangeError("PivotItems index out of range")
            return items[Index - 1]
        for it in items:
            if it.Name.lower() == str(Index).lower():
                return it
        raise SubscriptOutOfRangeError(f"PivotItem not found: {Index}")


class _DataPivotItem:
    def __init__(self, field, name):
        self._f, self._name = field, name

    @property
    def Name(self):  # noqa: N802
        return self._name

    Caption = Value = SourceName = Name

    @property
    def Parent(self):  # noqa: N802
        return self._f


class _PivotCell:
    """Range.PivotCell: what a cell of a PivotTable report is (PivotCellType), with its PivotTable, PivotField,
    PivotItem, the RowItems and ColumnItems that pick it, and its DataField."""

    _TYPES = {"value": 0, "item": 1, "subtotal": 2, "grand": 3, "data_caption": 4, "field_caption": 5, "page_name": 5,
              "page_value": 6, "header": 9, "blank": 9}

    def __init__(self, rng, info):
        self._r, self._d = rng, info

    @property
    def PivotCellType(self):  # noqa: N802
        """xlPivotCellValue (0), xlPivotCellPivotItem (1), xlPivotCellSubtotal (2), xlPivotCellGrandTotal (3),
        xlPivotCellDataField (4: a caption), xlPivotCellPivotField (5: a filter's name), xlPivotCellPageFieldItem (6),
        xlPivotCellBlankCell (9)."""
        return self._TYPES.get(self._d["kind"], 9)

    @property
    def PivotTable(self):  # noqa: N802
        return PivotTable(self._r._sheet, self._d["name"])

    @property
    def PivotField(self):  # noqa: N802
        f = self._d.get("field")
        if f:
            return PivotField(self.PivotTable, f)
        if self._d.get("data_field"):
            return self.DataField
        raise ApplicationDefinedError("The cell has no PivotField")

    @property
    def PivotItem(self):  # noqa: N802
        f, it = self._d.get("field"), self._d.get("item")
        if not f:
            raise ApplicationDefinedError("The cell isn't an item's label")
        return PivotField(self.PivotTable, f).PivotItems(it)

    @property
    def DataField(self):  # noqa: N802
        name = self._d.get("data_field")
        if not name:
            raise ApplicationDefinedError("The cell isn't a value of the report")
        return self.PivotTable.DataFields(name)

    def _items(self, key):
        pt = self.PivotTable
        return [PivotField(pt, f).PivotItems(i) for f, i in self._d.get(key, [])]

    @property
    def RowItems(self):  # noqa: N802
        return self._items("row_items")

    @property
    def ColumnItems(self):  # noqa: N802
        return self._items("col_items")

    @property
    def Range(self):  # noqa: N802
        return self._r


class _PivotFilters:
    """PivotField.PivotFilters: a label filter and a value filter (Top 10 is a value filter), as in Excel."""

    def __init__(self, field):
        self._f = field

    def _list(self):
        found = self._f._find(self._f._pt._get())
        if not found:
            return []
        return [_PivotFilter(self._f, k) for k in ("label_filter", "value_filter") if found[1].get(k, {}).get("type", 0)]

    def Add2(self, Type, DataField=None, Value1=None, Value2=None, Order=None, Name=None, Description=None, MemberPropertyField=None,  # noqa: N802,N803
             WholeDayFilter=None):
        d = self._f._pt._get()
        found = self._f._find(d)
        if found is None:
            raise ApplicationDefinedError("Only fields on Rows or Columns can be filtered")
        t = int(Type)
        data = 0
        if DataField is not None:
            caps = [c.lower() for c in d["captions"]]
            name = DataField.Name if isinstance(DataField, PivotField) else str(DataField)
            if name.lower() not in caps:
                raise ApplicationDefinedError(f"Not a data field of the PivotTable: {name}")
            data = caps.index(name.lower())
        f = {"type": t, "data": data, "v1": "" if Value1 is None else str(Value1), "v2": "" if Value2 is None else str(Value2)}
        empty = {"type": 0, "data": 0, "v1": "", "v2": ""}
        if t >= 15:
            found[1]["label_filter"], found[1]["value_filter"] = f, empty
        else:
            found[1]["value_filter"], found[1]["label_filter"] = f, empty
        self._f._pt._put(d)
        return _PivotFilter(self._f, "label_filter" if t >= 15 else "value_filter")

    Add = Add2

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def Item(self, Index):  # noqa: N802,N803
        lst = self._list()
        if not 1 <= int(Index) <= len(lst):
            raise SubscriptOutOfRangeError("PivotFilters index out of range")
        return lst[int(Index) - 1]

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter(self._list())

    def __len__(self):
        return self.Count


class PivotField:
    """A field of a PivotTable: a source field (Orientation, Position, PivotItems, CurrentPage) or a data field (Function, Caption,
    NumberFormat, Calculation) when it comes from DataFields / AddDataField."""

    def __init__(self, pt, name, data_index=None):
        self._pt, self._name, self._data = pt, name, data_index

    def __repr__(self):
        return f"<PivotField {self._name}>"

    def _index(self, d=None) -> int:
        d = d or self._pt._get()
        for i, n in enumerate(d["fields"]):
            if n.lower() == self._name.lower():
                return i
        raise SubscriptOutOfRangeError(f"PivotField not found: {self._name}")

    def _find(self, d):
        """(axis key, axis entry, position) of this source field, or None."""
        f = self._index(d)
        for key in ("rows", "cols", "pages"):
            for pos, e in enumerate(d[key]):
                if e["field"] == f:
                    return key, e, pos
        return None

    def _axis(self):
        found = self._find(self._pt._get())
        return None if found is None else (found[0], found[1])

    def _data_entry(self, d):
        if self._data is None or self._data >= len(d["data"]):
            raise ApplicationDefinedError(f"{self._name} is not a data field")
        return d["data"][self._data]

    @property
    def Name(self):  # noqa: N802
        if self._data is not None:
            return self._pt._get()["captions"][self._data]
        return self._name

    @Name.setter
    def Name(self, v):  # noqa: N802
        self.Caption = v

    @property
    def SourceName(self):  # noqa: N802
        return self._name

    @property
    def Orientation(self):  # noqa: N802
        if self._data is not None:
            return xlDataField
        found = self._find(self._pt._get())
        if found is None:
            return xlHidden
        return {"rows": xlRowField, "cols": xlColumnField, "pages": xlPageField}[found[0]]

    @Orientation.setter
    def Orientation(self, v):  # noqa: N802
        d = self._pt._get()
        f = self._index(d)
        v = int(v)
        if self._data is not None:
            if v == xlHidden:
                d["data"].pop(self._data)
                self._pt._put(d)
            return
        for key in ("rows", "cols", "pages"):
            d[key] = [e for e in d[key] if e["field"] != f]
        if v == xlDataField:
            d["data"].append({"field": f, "func": 0, "show_as": 0, "caption": "", "number_format": ""})
        elif v in _AXES:
            d[_AXES[v]].append({"field": f, "hidden": [], "subtotal": True, "descending": False, "page_item": ""})
        elif v != xlHidden:
            raise ApplicationDefinedError(f"Unknown orientation: {v}")
        self._pt._put(d)

    @property
    def Position(self):  # noqa: N802
        if self._data is not None:
            return self._data + 1
        found = self._find(self._pt._get())
        return 0 if found is None else found[2] + 1

    @Position.setter
    def Position(self, v):  # noqa: N802
        d = self._pt._get()
        if self._data is not None:
            e = d["data"].pop(self._data)
            d["data"].insert(max(0, int(v) - 1), e)
        else:
            found = self._find(d)
            if found is None:
                raise ApplicationDefinedError("The field is not on the PivotTable")
            key, e, pos = found
            d[key].pop(pos)
            d[key].insert(max(0, int(v) - 1), e)
        self._pt._put(d)

    def CalculatedItems(self, Index=None):  # noqa: N802,N803
        c = _CalculatedItems(self)
        return c if Index is None else c.Item(Index)

    def PivotItems(self, Index=None):  # noqa: N802,N803
        pairs = _rq("pivot.items", sheet=self._pt._sheet, name=self._pt._name, field=self._index(), display=True)
        items = [PivotItem(self, n, shown) for n, shown in pairs]
        if Index is None:
            return items
        if isinstance(Index, int):
            if not 1 <= Index <= len(items):
                raise SubscriptOutOfRangeError("PivotItems index out of range")
            return items[Index - 1]
        for it in items:
            if it.Name.lower() == str(Index).lower() or it.SourceName.lower() == str(Index).lower():
                return it
        raise SubscriptOutOfRangeError(f"PivotItem not found: {Index}")

    @property
    def Value(self):  # noqa: N802
        """The field's name on the report (its caption)."""
        return self.Caption

    @Value.setter
    def Value(self, v):  # noqa: N802
        self.Caption = v

    @property
    def CurrentPageName(self):  # noqa: N802
        """None: the page name of an OLAP field (a PivotTable on a Data Model), which YCGrid's fields are not (Excel 16
        reads Null for them too, measured); setting it sets CurrentPage."""
        return None

    @CurrentPageName.setter
    def CurrentPageName(self, v):  # noqa: N802
        self.CurrentPage = v

    def AddPageItem(self, Item, ClearList=False):  # noqa: N802,N803
        """A filter field's multiple selection: shows Item too (ClearList: only it)."""
        d = self._pt._get()
        found = self._find(d)
        if found is None or found[0] != "pages":
            raise ApplicationDefinedError(f"{self._name} isn't a filter field of the PivotTable")
        names = [it.SourceName for it in self.PivotItems()]
        want = str(Item.SourceName if isinstance(Item, PivotItem) else Item)
        match = next((n for n in names if n.lower() == want.lower()), None)
        if match is None:
            raise ApplicationDefinedError(f"PivotItem not found: {want}")
        e = found[1]
        if ClearList or e.get("page_item"):
            shown = set() if ClearList else {e["page_item"]}
        else:
            shown = set(names) - set(e.get("hidden", [])) if e.get("hidden") else set()
        shown.add(match)
        e["page_item"], e["hidden"] = "", [n for n in names if n not in shown]
        e["multi"] = True
        self._pt._put(d)

    @property
    def VisibleItems(self):  # noqa: N802
        return [it for it in self.PivotItems() if it.Visible]

    @property
    def HiddenItems(self):  # noqa: N802
        return [it for it in self.PivotItems() if not it.Visible]

    # ---- AutoShow: the top or bottom items by a data field (the field's value filter) ----
    def AutoShow(self, Type, Range=None, Count=None, Field=None):  # noqa: N802,N803
        """xlAutomatic shows the Count top (Range xlTop) or bottom (xlBottom) items by the data field Field, as a
        Top 10 filter does; xlManual shows every item again."""
        if int(Type) == xlManual:
            self.ClearValueFilters()
            return
        bottom = Range in (xlBottom, 2)
        self.PivotFilters.Add2(xlBottomCount if bottom else xlTopCount, DataField=Field, Value1=int(Count or 10))

    def _value_filter(self):
        d = self._pt._get()
        found = self._find(d)
        return d, (found[1].get("value_filter") if found else None)

    @property
    def AutoShowType(self):  # noqa: N802
        _, f = self._value_filter()
        return xlAutomatic if f and f.get("type") in (xlTopCount, xlBottomCount) else xlManual

    @property
    def AutoShowRange(self):  # noqa: N802
        _, f = self._value_filter()
        return xlBottom if f and f.get("type") == xlBottomCount else xlTop

    @property
    def AutoShowCount(self):  # noqa: N802
        _, f = self._value_filter()
        try:
            return int(float(f["v1"])) if f and f.get("type") in (xlTopCount, xlBottomCount) else 10
        except (TypeError, ValueError):
            return 10

    @property
    def AutoShowField(self):  # noqa: N802
        d, f = self._value_filter()
        if not f or f.get("type") not in (xlTopCount, xlBottomCount):
            return ""
        caps = d.get("captions", [])
        return caps[f["data"]] if 0 <= f.get("data", 0) < len(caps) else ""

    @property
    def DataType(self):  # noqa: N802
        """xlDate (2), xlNumber (-4145) or xlText (-4158): what the field's items are."""
        items = [it.SourceName for it in self.PivotItems() if it.SourceName not in ("(blank)", "")]
        if not items:
            return xlText
        shown = [it.Name for it in self.PivotItems()]
        nums = 0
        for n in items:
            try:
                float(n)
                nums += 1
            except ValueError:
                pass
        if nums != len(items):
            return xlText
        dates = sum(1 for t in shown if ("/" in t or "-" in t[1:]) and any(ch.isdigit() for ch in t))
        return xlDate if dates == len(shown) and shown else xlNumber

    def AutoGroup(self):  # noqa: N802
        """Groups a date field by Excel's automatic periods (years, quarters, months) as a drag of a date field
        does; other fields are left as they are."""
        if self.DataType == xlDate:
            self.Group(Start=True, End=True, Periods=[False, False, False, False, True, True, True])

    def _report_cells(self, key):
        kw = dict(data=self._data) if self._data is not None else dict(field=self._index())
        r = _rq("pivot.field_ranges", sheet=self._pt._sheet, name=self._pt._name, **kw)
        if not r or r.get(key) is None:
            raise ApplicationDefinedError(f"The field {self._name} isn't on the PivotTable report")
        return Range._from(self._pt._sheet, [tuple(r[key])])

    @property
    def DataRange(self):  # noqa: N802
        """The cells of the field's items on the report (a row field's labels, subtotal rows included; a column field's
        along its header row; a filter's value; a data field's values without grand totals), as in Excel."""
        return self._report_cells("data")

    @property
    def LabelRange(self):  # noqa: N802
        """The cell holding the field's caption on the report ("Row Labels" in compact form), as in Excel."""
        return self._report_cells("label")

    def Group(self, Start=True, End=True, By=None, Periods=None):  # noqa: N802,N803
        """Group the field's items. Dates: Periods = Excel's array of 7 booleans (seconds, minutes, hours, days, months, quarters,
        years), or [YC] By=ycGroupYears / ycGroupQuarters / ycGroupMonths / ycGroupYearMonths / ycGroupDays.
        Numbers: By = the size of each range, Start / End = the first / last value (True = automatic)."""
        d = self._pt._get()
        f = self._index(d)
        g = {"field": f, "by": 0, "start": 0.0, "end": 0.0, "interval": 10.0}
        if Periods is not None:
            p = [bool(x) for x in list(Periods)[:7]] + [False] * 7
            months, quarters, years, days = p[4], p[5], p[6], p[3]
            g["by"] = 4 if years and months else 1 if years else 2 if quarters else 3 if months else 5
            g["periods"] = sum(1 << i for i in range(7) if p[i])
            if not g["periods"]:
                raise ApplicationDefinedError("Periods: choose at least one period")
            if days and isinstance(By, (int, float)) and not isinstance(By, bool) and By > 1:
                g["days"] = int(By)
            g["auto_start"], g["auto_end"] = Start is True, End is True
            if Start is not True:
                g["start"] = float(Start if isinstance(Start, (int, float)) else datetime_to_serial(Start))
            if End is not True:
                g["end"] = float(End if isinstance(End, (int, float)) else datetime_to_serial(End))
        elif isinstance(By, int) and 1 <= By <= 5 and self._is_date_field():
            g["by"] = int(By)
        else:
            # numbers: ranges of By, from Start to End (True = the smallest / largest value)
            values = self._numbers()
            g["by"] = 6
            g["interval"] = float(By or 10)
            g["start"] = float(Start) if Start is not True else (min(values) if values else 0.0)
            g["end"] = float(End) if End is not True else (max(values) if values else 0.0)
        d["groups"] = [x for x in d["groups"] if x["field"] != f] + [g]
        self._pt._put(d)

    def Ungroup(self):  # noqa: N802
        d = self._pt._get()
        f = self._index(d)
        d["groups"] = [x for x in d["groups"] if x["field"] != f]
        self._pt._put(d)

    def _source_values(self):
        d = self._pt._get()
        f = self._index(d)
        src = Range(d["source"]) if "!" in d["source"] or ":" in d["source"] else None
        if src is None:
            return []
        col = src.Columns(f + 1).Value
        return [r[0] if isinstance(r, (list, tuple)) else r for r in (col[1:] if isinstance(col, (list, tuple)) else [])]

    def _numbers(self):
        return [float(v) for v in self._source_values() if isinstance(v, (int, float)) and not isinstance(v, bool)]

    def _is_date_field(self):
        import datetime as dt

        return any(isinstance(v, (dt.date, dt.datetime)) for v in self._source_values())

    def ClearAllFilters(self):  # noqa: N802
        d = self._pt._get()
        found = self._find(d)
        if found is not None:
            empty = {"type": 0, "data": 0, "v1": "", "v2": ""}
            found[1]["hidden"] = []
            found[1]["page_item"] = ""
            found[1]["label_filter"], found[1]["value_filter"] = dict(empty), dict(empty)
            self._pt._put(d)

    def _clear(self, key):
        d = self._pt._get()
        found = self._find(d)
        if found is not None:
            found[1][key] = {"type": 0, "data": 0, "v1": "", "v2": ""}
            self._pt._put(d)

    def ClearLabelFilters(self):  # noqa: N802
        self._clear("label_filter")

    def ClearValueFilters(self):  # noqa: N802
        self._clear("value_filter")

    def ClearManualFilter(self):  # noqa: N802
        d = self._pt._get()
        found = self._find(d)
        if found is not None:
            found[1]["hidden"] = []
            self._pt._put(d)

    @property
    def PivotFilters(self):  # noqa: N802
        return _PivotFilters(self)

    def _axis_prop(key, default=False, conv=bool):  # noqa: N805
        def g(self):
            found = self._find(self._pt._get())
            return conv(found[1].get(key, default)) if found else default

        def s(self, v):
            d = self._pt._get()
            found = self._find(d)
            if found is None:
                raise ApplicationDefinedError("The field is not on Rows, Columns or Filters")
            found[1][key] = conv(v)
            self._pt._put(d)

        return property(g, s)

    RepeatLabels = _axis_prop("repeat_labels")
    LayoutBlankLine = _axis_prop("blank_line")
    ShowAllItems = _axis_prop("show_all_items")
    EnableMultiplePageItems = _axis_prop("multi_page")
    IncludeNewItemsInFilter = _axis_prop("include_new")
    LayoutPageBreak = _axis_prop("page_break")
    del _axis_prop

    def _form(self):
        d = self._pt._get()
        found = self._find(d)
        f = found[1].get("form", -1) if found else -1
        return d["layout"] if f is None or f < 0 else f

    def _set_layout(self, **kw):
        d = self._pt._get()
        found = self._find(d)
        if found is None or found[0] != "rows":
            raise ApplicationDefinedError("The field is not on Rows")
        found[1].update(kw)
        self._pt._put(d)

    @property
    def LayoutForm(self):  # noqa: N802
        """xlTabular (0) or xlOutline (1): Field Settings > Layout & Print."""
        return 0 if self._form() == 2 else 1

    @LayoutForm.setter
    def LayoutForm(self, v):  # noqa: N802
        compact = self._form() == 0
        self._set_layout(form=2 if int(v) == 0 else (0 if compact else 1))

    @property
    def LayoutCompactRow(self):  # noqa: N802
        """Display labels from the next field in the same column (compact form)."""
        return self._form() == 0

    @LayoutCompactRow.setter
    def LayoutCompactRow(self, v):  # noqa: N802
        self._set_layout(form=0 if v else 1)

    @property
    def LayoutSubtotalLocation(self):  # noqa: N802
        """xlAtTop (1) or xlAtBottom (2)."""
        d = self._pt._get()
        found = self._find(d)
        t = found[1].get("sub_top", -1) if found else -1
        top = d["subtotals"] == 1 if t is None or t < 0 else t == 1
        return 1 if top else 2

    @LayoutSubtotalLocation.setter
    def LayoutSubtotalLocation(self, v):  # noqa: N802
        self._set_layout(sub_top=1 if int(v) == 1 else 0)

    @property
    def ShowDetail(self):  # noqa: N802
        """[YC] True when no item of the field is collapsed."""
        found = self._find(self._pt._get())
        return not found or (not found[1].get("collapse_all") and not found[1].get("collapsed"))

    @ShowDetail.setter
    def ShowDetail(self, v):  # noqa: N802
        """Expand (True) or collapse (False) every item of the field."""
        d = self._pt._get()
        found = self._find(d)
        if found is None:
            raise ApplicationDefinedError("The field is not on Rows or Columns")
        found[1]["collapse_all"], found[1]["collapsed"] = not bool(v), []
        self._pt._put(d)

    @property
    def DrilledDown(self):  # noqa: N802
        return self.ShowDetail

    @DrilledDown.setter
    def DrilledDown(self, v):  # noqa: N802
        self.ShowDetail = v

    @property
    def AutoSortOrder(self):  # noqa: N802
        found = self._find(self._pt._get())
        if not found:
            return xlAscending
        if found[1].get("manual_order"):
            return xlManual
        return xlDescending if found[1]["descending"] else xlAscending

    @property
    def AutoSortField(self):  # noqa: N802
        d = self._pt._get()
        found = self._find(d)
        if found and found[1].get("sort_by", -1) >= 0 and found[1]["sort_by"] < len(d["captions"]):
            return d["captions"][found[1]["sort_by"]]
        return self.Name

    @property
    def CurrentPage(self):  # noqa: N802
        found = self._find(self._pt._get())
        if found is None or found[0] != "pages":
            raise ApplicationDefinedError("CurrentPage is only for fields on the Filters area")
        return found[1]["page_item"] or "(All)"

    @CurrentPage.setter
    def CurrentPage(self, v):  # noqa: N802
        d = self._pt._get()
        found = self._find(d)
        if found is None or found[0] != "pages":
            raise ApplicationDefinedError("CurrentPage is only for fields on the Filters area")
        v = "" if v is None else str(getattr(v, "Name", v))
        found[1]["page_item"] = "" if v in ("", "(All)") else v
        self._pt._put(d)

    def AutoSort(self, Order, Field=None, PivotLine=None, CustomSubtotal=None):  # noqa: N802,N803
        """Sort the field's items: Field = this field's name (by labels) or a data field's caption (by its values)."""
        d = self._pt._get()
        found = self._find(d)
        if found is None:
            return
        e = found[1]
        if int(Order) == xlManual:
            e["sort_by"], e["descending"] = -1, False
            e["manual_order"] = _rq("pivot.items", sheet=self._pt._sheet, name=self._pt._name, field=self._index(), order=True)
            self._pt._put(d)
            return
        e["descending"] = int(Order) == xlDescending
        e["manual_order"] = []
        e["sort_by"], e["sort_col"] = -1, []
        name = Field.Name if isinstance(Field, PivotField) else (None if Field is None else str(Field))
        caps = [c.lower() for c in d["captions"]]
        if name is not None and name.lower() in caps and name.lower() != self._name.lower():
            e["sort_by"] = caps.index(name.lower())
        self._pt._put(d)

    @property
    def Subtotals(self):  # noqa: N802
        """Excel's 12 booleans: Automatic, then Sum, Count, Average, Max, Min, Product, Count Nums, StdDev, StdDevp, Var, Varp."""
        found = self._find(self._pt._get())
        if not found or not found[1]["subtotal"]:
            return tuple([False] * 12)
        mask = found[1].get("subtotal_funcs", 0)
        if not mask:
            return tuple([True] + [False] * 11)
        return tuple([False] + [bool(mask >> f & 1) for f in _SUBTOTAL_FUNCS])

    @Subtotals.setter
    def Subtotals(self, v):  # noqa: N802
        d = self._pt._get()
        found = self._find(d)
        if found is not None:
            if isinstance(v, (list, tuple)):
                flags = [bool(x) for x in list(v)[:12]] + [False] * 12
                mask = sum(1 << f for i, f in enumerate(_SUBTOTAL_FUNCS) if flags[i + 1])
                found[1]["subtotal"] = flags[0] or mask != 0
                found[1]["subtotal_funcs"] = 0 if flags[0] else mask
            else:
                found[1]["subtotal"] = bool(v)
                found[1]["subtotal_funcs"] = 0
            self._pt._put(d)

    def Subtotals_(self, Index):  # noqa: N802,N803
        return self.Subtotals[int(Index) - 1]

    @property
    def Formula(self):  # noqa: N802
        """A calculated field's formula ("" for a source field)."""
        for c in self._pt._get()["calc_fields"]:
            if c["name"].lower() == self._name.lower():
                return c["formula"]
        return ""

    @Formula.setter
    def Formula(self, v):  # noqa: N802
        d = self._pt._get()
        for c in d["calc_fields"]:
            if c["name"].lower() == self._name.lower():
                c["formula"] = str(v)
                self._pt._put(d)
                return
        raise ApplicationDefinedError(f"{self._name} is not a calculated field")

    @property
    def IsCalculated(self):  # noqa: N802
        return any(c["name"].lower() == self._name.lower() for c in self._pt._get()["calc_fields"])

    def Delete(self):  # noqa: N802
        """Delete a calculated field (and its data fields)."""
        d = self._pt._get()
        f = self._index(d)
        d["calc_fields"] = [c for c in d["calc_fields"] if c["name"].lower() != self._name.lower()]
        d["data"] = [e for e in d["data"] if e["field"] != f]
        self._pt._put(d)

    # ---- data fields ----
    @property
    def Function(self):  # noqa: N802
        return _PIVOT_FUNCS[self._data_entry(self._pt._get())["func"]]

    @Function.setter
    def Function(self, v):  # noqa: N802
        d = self._pt._get()
        self._data_entry(d)["func"] = _pivot_func(v)
        self._pt._put(d)

    @property
    def Caption(self):  # noqa: N802
        if self._data is None:
            found = self._find(self._pt._get())
            return found[1].get("caption") or self._name if found else self._name
        return self._pt._get()["captions"][self._data]

    @Caption.setter
    def Caption(self, v):  # noqa: N802
        d = self._pt._get()
        v = str(v)
        others = [n.lower() for n in d["fields"] if n.lower() != self._name.lower()]
        others += [c.lower() for i, c in enumerate(d["captions"]) if i != self._data]
        if v.lower() in others:
            raise ApplicationDefinedError("PivotTable field name already exists.")
        if self._data is None:
            found = self._find(d)
            if found is None:
                raise ApplicationDefinedError("Only fields on Rows, Columns or Filters (or data fields) take a caption")
            found[1]["caption"] = "" if v == self._name else v
        else:
            self._data_entry(d)["caption"] = v
        self._pt._put(d)

    @property
    def NumberFormat(self):  # noqa: N802
        return self._data_entry(self._pt._get())["number_format"] or "General"

    @NumberFormat.setter
    def NumberFormat(self, v):  # noqa: N802
        d = self._pt._get()
        self._data_entry(d)["number_format"] = "" if v in (None, "General") else str(v)
        self._pt._put(d)

    @property
    def Calculation(self):  # noqa: N802
        return _PIVOT_SHOW_BACK[self._data_entry(self._pt._get())["show_as"]]

    @Calculation.setter
    def Calculation(self, v):  # noqa: N802
        d = self._pt._get()
        if int(v) not in _PIVOT_SHOW:
            raise UnsupportedError(f"Unsupported Calculation: {v}")
        self._data_entry(d)["show_as"] = _PIVOT_SHOW[int(v)]
        self._pt._put(d)

    @property
    def BaseField(self):  # noqa: N802
        """Show Values As: the field the calculation runs along (a name)."""
        d = self._pt._get()
        f = self._data_entry(d).get("base_field", -1)
        return d["fields"][f] if 0 <= f < len(d["fields"]) else None

    @BaseField.setter
    def BaseField(self, v):  # noqa: N802
        d = self._pt._get()
        name = v.SourceName if isinstance(v, PivotField) else str(v)
        self._data_entry(d)["base_field"] = PivotField(self._pt, name)._index(d)
        self._pt._put(d)

    @property
    def BaseItem(self):  # noqa: N802
        """Show Values As: the item compared with: an item's name, "(previous)" or "(next)" (xlPrevious / xlNext)."""
        return self._data_entry(self._pt._get()).get("base_item", "")

    @BaseItem.setter
    def BaseItem(self, v):  # noqa: N802
        d = self._pt._get()
        v = {xlPrevious: "(previous)", xlNext: "(next)"}.get(v, v) if isinstance(v, int) and not isinstance(v, bool) else v
        self._data_entry(d)["base_item"] = getattr(v, "SourceName", None) or str(v)
        self._pt._put(d)


class _PivotFieldList:
    def __init__(self, items):
        self._items = items

    @property
    def Count(self):  # noqa: N802
        return len(self._items)

    def Item(self, Index):  # noqa: N802,N803
        if isinstance(Index, int):
            if not 1 <= Index <= len(self._items):
                raise SubscriptOutOfRangeError("PivotFields index out of range")
            return self._items[Index - 1]
        for f in self._items:
            if f.Name.lower() == str(Index).lower() or f.SourceName.lower() == str(Index).lower():
                return f
        raise SubscriptOutOfRangeError(f"PivotField not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter(self._items)

    def __len__(self):
        return len(self._items)


class _CalculatedFields:
    def __init__(self, pt):
        self._pt = pt

    def _list(self):
        return self._pt._get()["calc_fields"]

    def Add(self, Name, Formula, UseStandardFormula=False):  # noqa: N802,N803
        d = self._pt._get()
        d["calc_fields"] = [c for c in d["calc_fields"] if c["name"].lower() != str(Name).lower()] + [{"name": str(Name), "formula": str(Formula)}]
        self._pt._put(d)
        return PivotField(self._pt, str(Name))

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def Item(self, Index):  # noqa: N802,N803
        cs = self._list()
        if isinstance(Index, int):
            if not 1 <= Index <= len(cs):
                raise SubscriptOutOfRangeError("CalculatedFields index out of range")
            return PivotField(self._pt, cs[Index - 1]["name"])
        for c in cs:
            if c["name"].lower() == str(Index).lower():
                return PivotField(self._pt, c["name"])
        raise SubscriptOutOfRangeError(f"Calculated field not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([PivotField(self._pt, c["name"]) for c in self._list()])

    def __len__(self):
        return self.Count


class _SlicerItem:
    def __init__(self, cache, name):
        self._c, self._name = cache, name

    def __repr__(self):
        return f"<SlicerItem {self._name}>"

    Name = property(lambda self: self._name)
    Value = Name
    Caption = Name

    @property
    def Selected(self):  # noqa: N802
        return self._name not in self._c._hidden()

    @Selected.setter
    def Selected(self, v):  # noqa: N802
        hidden = list(self._c._hidden())
        if v and self._name in hidden:
            hidden.remove(self._name)
        elif not v and self._name not in hidden:
            hidden.append(self._name)
        self._c._filter(hidden)


class _TimelineViewState:
    def __init__(self, slicer):
        self._s = slicer

    @property
    def Level(self):  # noqa: N802
        """xlTimelineLevelYears (0), Quarters (1), Months (2) or Days (3)."""
        return self._s._get()["level"]

    @Level.setter
    def Level(self, v):  # noqa: N802
        self._s._set(level=int(v))

    def _bit(bit):  # noqa: N805
        def g(self):
            return not (self._s._get().get("tl_hide", 0) & bit)

        def s(self, v):
            h = self._s._get().get("tl_hide", 0)
            self._s._set(tl_hide=(h & ~bit) if v else (h | bit))

        return property(g, s)

    ShowSelectionLabel = _bit(1)
    ShowHorizontalScrollbar = _bit(2)
    ShowTimeLevel = _bit(4)
    del _bit

    @property
    def ShowHeader(self):  # noqa: N802
        return self._s.DisplayHeader

    @ShowHeader.setter
    def ShowHeader(self, v):  # noqa: N802
        self._s.DisplayHeader = bool(v)


class _TimelineState:
    def __init__(self, cache):
        self._c = cache

    def _filter(self):
        for f in self._c._pt().get("date_filters", []):
            if f["field"] == self._c._field:
                return f
        return None

    @property
    def StartDate(self):  # noqa: N802
        f = self._filter()
        return serial_to_datetime(f["from"]) if f else None

    @property
    def EndDate(self):  # noqa: N802
        f = self._filter()
        return serial_to_datetime(f["to"]) if f else None

    @property
    def FilterType(self):  # noqa: N802
        return 0 if self._filter() is None else 1  # [YC] 0 none, 1 a date range

    def SetFilterDateRange(self, StartDate, EndDate):  # noqa: N802,N803
        def serial(v):
            if isinstance(v, (int, float)):
                return float(v)
            if isinstance(v, str):
                v = _dt.datetime.fromisoformat(v)
            if isinstance(v, _dt.date) and not isinstance(v, _dt.datetime):
                v = _dt.datetime(v.year, v.month, v.day)
            return datetime_to_serial(v)

        first = self._c._first_slicer()
        if first:
            _rq("slicer.dates", sheet=first[0], name=first[1], **{"from": serial(StartDate), "to": serial(EndDate)})
        else:
            _rq("timeline.filter", pivot=self._c._pivot, field=self._c._field, **{"from": serial(StartDate), "to": serial(EndDate)})


class Slicer:
    """A slicer on a sheet: Name, Caption, Top, Left, Width, Height, NumberOfColumns, SlicerCache, Delete()."""

    def __init__(self, sheet_id, name):
        self._sheet, self._name = sheet_id, name

    def __repr__(self):
        return f"<Slicer {self._name}>"

    def _get(self):
        for s in _rq("slicer.list", sheet=self._sheet):
            if s["name"].lower() == self._name.lower():
                return s
        raise SubscriptOutOfRangeError(f"Slicer not found: {self._name}")

    def _set(self, **props):
        _rq("slicer.set", sheet=self._sheet, name=self._name, props=props)
        if "name" in props:
            self._name = props["name"]

    def _prop(key, conv=float):  # noqa: N805
        def g(self):
            return self._get()[key]

        def s(self, v):
            self._set(**{key: conv(v)})

        return property(g, s)

    Name = _prop("name", str)
    Caption = _prop("caption", str)
    Left = _prop("left")
    Top = _prop("top")
    Width = _prop("width")
    Height = _prop("height")
    NumberOfColumns = _prop("columns", int)
    DisplayHeader = _prop("show_header", bool)
    del _prop

    @property
    def SlicerCache(self):  # noqa: N802
        d = self._get()
        return SlicerCache(d["pivot"], d["field"], d.get("timeline", False), d.get("table", ""))

    @property
    def TimelineViewState(self):  # noqa: N802
        if not self._get().get("timeline"):
            raise ApplicationDefinedError("This slicer is not a timeline")
        return _TimelineViewState(self)

    def Delete(self):  # noqa: N802
        _rq("slicer.delete", sheet=self._sheet, name=self._name)

    def Copy(self):  # noqa: N802
        """Copy the slicer: Worksheet.Paste (or Ctrl+V) pastes a copy at the active cell."""
        _rq("slicer.copy", sheet=self._sheet, name=self._name)

    def Cut(self):  # noqa: N802
        _rq("slicer.copy", sheet=self._sheet, name=self._name, cut=True)


class _Slicers:
    def __init__(self, cache):
        self._c = cache

    def _all(self):
        out = []
        for ws in Worksheets:
            for s in _rq("slicer.list", sheet=ws._id):
                if self._c._mine(s):
                    out.append(Slicer(ws._id, s["name"]))
        return out

    def Add(self, SlicerDestination, Level=None, Name=None, Caption=None, Top=None, Left=None, Width=None, Height=None):  # noqa: N802,N803
        ws = SlicerDestination if isinstance(SlicerDestination, Worksheet) else Worksheets(str(SlicerDestination))
        if self._c._table:
            p = {"table": self._c._table, "field": self._c._field}
        else:
            p = {"pivot": self._c._pivot, "field": self._c._field, "timeline": self._c._timeline}
        if Level is not None and self._c._timeline:
            p["level"] = int(Level)
        if Name:
            p["name"] = str(Name)
        if Caption is not None:
            p["caption"] = str(Caption)
        for k, v in (("top", Top), ("left", Left), ("width", Width), ("height", Height)):
            if v is not None:
                p[k] = float(v)
        name = _rq("slicer.add", sheet=ws._id, **p)
        return Slicer(ws._id, name)

    @property
    def Count(self):  # noqa: N802
        return len(self._all())

    def Item(self, Index):  # noqa: N802,N803
        items = self._all()
        if isinstance(Index, int):
            return items[Index - 1]
        for s in items:
            if s.Name.lower() == str(Index).lower():
                return s
        raise SubscriptOutOfRangeError(f"Slicer not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter(self._all())


class _SlicerPivotTables:
    """SlicerCache.PivotTables: the PivotTables the slicers filter (Report Connections)."""

    def __init__(self, cache):
        self._c = cache

    def _names(self):
        first = self._c._first_slicer()
        if not first:
            return [self._c._pivot] if self._c._pivot else []
        for s in _rq("slicer.list", sheet=first[0]):
            if s["name"] == first[1]:
                return [s["pivot"]] + list(s.get("more_pivots", []))
        return []

    def _all(self):
        out = []
        for n in self._names():
            for ws in Worksheets:
                for p in _rq("pivot.list", sheet=ws._id):
                    if p["name"] == n:
                        out.append(PivotTable(p["sheet"], p["name"]))
        return out

    def _connect(self, names):
        first = self._c._first_slicer()
        if not first:
            raise ApplicationDefinedError("The slicer cache has no slicer")
        _rq("slicer.connect", sheet=first[0], name=first[1], pivots=list(names), alerts=_alerts())
        # the cache follows its first PivotTable (the slicers name it)
        for s in _rq("slicer.list", sheet=first[0]):
            if s["name"] == first[1]:
                self._c._pivot, self._c._field = s["pivot"], s["field"]

    def AddPivotTable(self, PivotTable):  # noqa: N802,N803
        names = self._names()
        if PivotTable.Name not in names:
            self._connect(names + [PivotTable.Name])

    def RemovePivotTable(self, PivotTable):  # noqa: N802,N803
        names = [n for n in self._names() if n.lower() != PivotTable.Name.lower()]
        self._connect(names)

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def Item(self, Index):  # noqa: N802,N803
        items = self._all()
        if isinstance(Index, int):
            return items[Index - 1]
        for p in items:
            if p.Name.lower() == str(Index).lower():
                return p
        raise SubscriptOutOfRangeError(f"PivotTable not found: {Index}")

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter(self._all())

    def __len__(self):
        return self.Count

    def __getitem__(self, i):
        return self._all()[i]


class SlicerCache:
    """The filter state of one field of a PivotTable (or one column of a table), shared by its slicers."""

    def __init__(self, pivot, field, timeline=False, table=""):
        self._pivot, self._field, self._timeline, self._table = pivot, int(field), bool(timeline), table or ""

    def __repr__(self):
        return f"<SlicerCache {self.Name}>"

    def _mine(self, s):
        """A slicer of this cache (a slicer.list entry)."""
        if self._table:
            return s.get("table", "").lower() == self._table.lower() and s["field"] == self._field
        return not s.get("table") and s["pivot"] == self._pivot and s["field"] == self._field

    def _first_slicer(self):
        """(sheet id, name) of the cache's first slicer, or None."""
        for ws in Worksheets:
            for s in _rq("slicer.list", sheet=ws._id):
                if self._mine(s):
                    return ws._id, s["name"]
        return None

    @property
    def ListObject(self):  # noqa: N802
        """The table a table's slicers filter (None for a PivotTable's)."""
        if not self._table:
            return None
        for ws in Worksheets:
            for lo in ws.ListObjects:
                if lo.Name.lower() == self._table.lower():
                    return lo
        return None

    def _pt(self):
        for ws in Worksheets:
            for p in _rq("pivot.list", sheet=ws._id):
                if p["name"] == self._pivot:
                    return p
        raise SubscriptOutOfRangeError(f"PivotTable not found: {self._pivot}")

    def _hidden(self):
        if self._table:
            first = self._first_slicer()
            return _rq("slicer.items", sheet=first[0], name=first[1])["hidden"] if first else []
        p = self._pt()
        for key in ("rows", "cols", "pages"):
            for e in p[key]:
                if e["field"] == self._field:
                    return e["hidden"]  # the field on the report: one filter, its own
        for f in p["slicer_filters"]:
            if f["field"] == self._field:
                return f["hidden"]
        return []

    def _filter(self, hidden):
        first = self._first_slicer()
        if first:
            # through a slicer: every PivotTable it is connected to, or its table's column
            _rq("slicer.select", sheet=first[0], name=first[1], hidden=list(hidden), alerts=_alerts())
        else:
            _rq("slicer.filter", pivot=self._pivot, field=self._field, hidden=list(hidden), alerts=_alerts())

    @property
    def SourceName(self):  # noqa: N802
        if self._table:
            lo = self.ListObject
            return lo.HeaderRowRange.Cells(1, self._field + 1).Value if lo else ""
        return self._pt()["fields"][self._field]

    @property
    def Name(self):  # noqa: N802
        return "Slicer_" + _re.sub(r"\W", "_", self.SourceName)

    @property
    def PivotTables(self):  # noqa: N802
        """The PivotTables this cache filters: AddPivotTable / RemovePivotTable (Report Connections), Count, Item."""
        return _SlicerPivotTables(self)

    def SlicerItems(self, Index=None):  # noqa: N802,N803
        if self._table:
            first = self._first_slicer()
            names = _rq("slicer.items", sheet=first[0], name=first[1])["items"] if first else []
        else:
            p = self._pt()
            names = _rq("pivot.items", sheet=p["sheet"], name=p["name"], field=self._field)
        items = [_SlicerItem(self, n) for n in names]
        if Index is None:
            return items
        if isinstance(Index, int):
            return items[Index - 1]
        for it in items:
            if it.Name.lower() == str(Index).lower():
                return it
        raise SubscriptOutOfRangeError(f"SlicerItem not found: {Index}")

    @property
    def VisibleSlicerItems(self):  # noqa: N802
        """The slicer's items that are selected (shown)."""
        hidden = self._hidden()
        return [it for it in self.SlicerItems() if it.Name not in hidden]

    @property
    def VisibleSlicerItemsList(self):  # noqa: N802
        hidden = self._hidden()
        return [it.Name for it in self.SlicerItems() if it.Name not in hidden]

    def ClearManualFilter(self):  # noqa: N802
        self._filter([])

    @property
    def SlicerCacheType(self):  # noqa: N802
        return 2 if self._timeline else 1  # xlTimeline / xlSlicer

    @property
    def TimelineState(self):  # noqa: N802
        return _TimelineState(self)

    def ClearDateFilter(self):  # noqa: N802
        first = self._first_slicer()
        if first:
            _rq("slicer.dates", sheet=first[0], name=first[1], **{"from": 1, "to": 0})
        else:
            _rq("timeline.filter", pivot=self._pivot, field=self._field, **{"from": 1, "to": 0})

    ClearAllFilters = ClearManualFilter

    # Slicer Settings > Item Sorting and Filtering, set on every slicer of this field
    def _settings(self):
        for ws in Worksheets:
            for sl in _rq("slicer.list", sheet=ws._id):
                if self._mine(sl):
                    return sl
        return {}

    def _set_all(self, **props):
        for ws in Worksheets:
            for sl in _rq("slicer.list", sheet=ws._id):
                if self._mine(sl):
                    _rq("slicer.set", sheet=ws._id, name=sl["name"], props=props)

    @property
    def SortItems(self):  # noqa: N802
        """xlSlicerSortAscending (2, the default) or xlSlicerSortDescending (3)."""
        return 3 if self._settings().get("descending") else 2

    @SortItems.setter
    def SortItems(self, v):  # noqa: N802
        if int(v) not in (2, 3):
            raise UnsupportedError(f"Unsupported SortItems: {v}")
        self._set_all(descending=int(v) == 3)

    @property
    def CrossFilterType(self):  # noqa: N802
        """Items with no data under the other filters: xlSlicerCrossFilterShowItemsWithDataAtTop (2, the default: dimmed
        and listed last), xlSlicerCrossFilterShowItemsWithNoData (3: dimmed in their place), xlSlicerNoCrossFilter (1),
        xlSlicerCrossFilterHideButtonsWithNoData (4)."""
        s = self._settings()
        if s.get("hide_no_data"):
            return 4
        if not s.get("indicate_no_data", True):
            return 1
        return 2 if s.get("no_data_last", True) else 3

    @CrossFilterType.setter
    def CrossFilterType(self, v):  # noqa: N802
        if int(v) not in (1, 2, 3, 4):
            raise UnsupportedError(f"Unsupported CrossFilterType: {v}")
        self._set_all(indicate_no_data=int(v) != 1, no_data_last=int(v) in (2, 4), hide_no_data=int(v) == 4)

    @property
    def ShowAllItems(self):  # noqa: N802
        """False: Hide items with no data (Slicer Settings)."""
        return not self._settings().get("hide_no_data", False)

    @ShowAllItems.setter
    def ShowAllItems(self, v):  # noqa: N802
        self._set_all(hide_no_data=not v)

    @property
    def Slicers(self):  # noqa: N802
        return _Slicers(self)

    def Delete(self):  # noqa: N802
        for s in self.Slicers:
            s.Delete()
        self._filter([])


class _SlicerCaches:
    def Add2(self, Source, SourceField, Name=None, SlicerCacheType=None):  # noqa: N802,N803
        """A slicer cache for a field of a PivotTable or a column of a table (Source = the PivotTable or the ListObject,
        SourceField = the field's / column's name)."""
        if isinstance(Source, PivotTable):
            f = PivotField(Source, getattr(SourceField, "SourceName", str(SourceField)))._index()
            return SlicerCache(Source.Name, f, SlicerCacheType is not None and int(SlicerCacheType) == 2)
        if type(Source).__name__ == "ListObject":
            if SlicerCacheType is not None and int(SlicerCacheType) == 2:
                raise UnsupportedError("Timelines filter PivotTables")
            want = str(getattr(SourceField, "Name", SourceField)).lower()
            heads = Source.HeaderRowRange.Value
            heads = heads[0] if heads and isinstance(heads[0], (list, tuple)) else heads
            for i, h in enumerate(heads if isinstance(heads, (list, tuple)) else [heads]):
                if str(h).lower() == want:
                    return SlicerCache("", i, False, Source.Name)
            raise ApplicationDefinedError(f"No such column: {SourceField}")
        raise UnsupportedError("Slicers are supported for PivotTables and tables")

    Add = Add2

    def _all(self):
        seen, out = set(), []
        for ws in Worksheets:
            for s in _rq("slicer.list", sheet=ws._id):
                key = (s["pivot"], s["field"], s.get("table", ""))
                if key not in seen:
                    seen.add(key)
                    out.append(SlicerCache(s["pivot"], s["field"], s.get("timeline", False), s.get("table", "")))
        return out

    @property
    def Count(self):  # noqa: N802
        return len(self._all())

    def Item(self, Index):  # noqa: N802,N803
        items = self._all()
        if isinstance(Index, int):
            return items[Index - 1]
        for c in items:
            if c.Name.lower() == str(Index).lower():
                return c
        raise SubscriptOutOfRangeError(f"SlicerCache not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter(self._all())


_PIVOT_PENDING = {}  # PivotTable.ManualUpdate: (sheet, name) -> the definition waiting


def _copy_dict(d):
    import copy as _copy

    return _copy.deepcopy(d)


class _PivotCacheObj:
    def __init__(self, source, pt=None):
        self._source, self._pt = source, pt

    @property
    def RefreshOnFileOpen(self):  # noqa: N802
        return bool(self._pt and self._pt._get()["options"]["refresh_on_open"])

    @RefreshOnFileOpen.setter
    def RefreshOnFileOpen(self, v):  # noqa: N802
        if self._pt is None:
            raise ApplicationDefinedError("The cache isn't used by a PivotTable yet")
        d = self._pt._get()
        d["options"]["refresh_on_open"] = bool(v)
        self._pt._put(d)

    @property
    def SourceData(self):  # noqa: N802
        return _source_r1c1(self._pt._get()["source"] if self._pt else self._source)

    @property
    def MissingItemsLimit(self):  # noqa: N802
        """xlMissingItemsNone (0): YCGrid lists only the items the source has now (a filter's list drops an item the
        source lost at the refresh). Setting xlMissingItemsNone or xlMissingItemsDefault (-1) keeps that; keeping
        missing items isn't supported."""
        return 0

    @MissingItemsLimit.setter
    def MissingItemsLimit(self, v):  # noqa: N802
        if int(v) not in (0, -1):
            raise UnsupportedError("MissingItemsLimit: YCGrid keeps no items its source lost (xlMissingItemsNone)")

    def CreatePivotChart(self, ChartDestination, XlChartType=None, Left=None, Top=None, Width=None, Height=None):  # noqa: N802,N803
        """A chart of this cache's first PivotTable, on the sheet ChartDestination (a Worksheet or its name)."""
        ws = ChartDestination if isinstance(ChartDestination, Worksheet) else Worksheets(str(ChartDestination))
        pts = [self._pt] if self._pt is not None else [pt for sh in Worksheets for pt in sh.PivotTables() if pt.PivotCache()._source == self._source]
        if not pts:
            raise ApplicationDefinedError("CreatePivotChart: no PivotTable uses this cache yet (CreatePivotTable first)")
        pt = pts[0]
        co = ws.Shapes.AddChart2(-1, XlChartType if XlChartType is not None else 51, Left, Top, Width, Height)
        co.Chart.SetSourceData(pt.TableRange1)
        return co

    def CreatePivotTable(self, TableDestination, TableName=None, ReadData=True, DefaultVersion=None):  # noqa: N802,N803
        """A new PivotTable at TableDestination (a Range or an address such as "Sheet2!R3C1" / "Sheet2!A3")."""
        if isinstance(TableDestination, Range):
            sheet = TableDestination._sheet
            dest = TableDestination.Address(External=True)
        else:
            dest = _source_a1(TableDestination)
            if "!" in dest:
                sname = dest.rsplit("!", 1)[0]
                if sname.startswith("'") and sname.endswith("'"):
                    sname = sname[1:-1].replace("''", "'")
                sheet = Worksheets(sname)._id
            else:
                sheet = _active_sheet_id()
        p = {"source": self._source, "dest": dest}
        if TableName:
            p["name"] = str(TableName)
        d = _rq("pivot.add", sheet=sheet, alerts=_alerts(), **p)
        return PivotTable(d["sheet"], d["name"])

    def Refresh(self):  # noqa: N802
        if self._pt:
            self._pt.RefreshTable()
        else:
            _rq("pivot.refresh_all")


def _source_a1(text) -> str:
    """A PivotTable source or destination as text: R1C1 ("Sheet1!R1C1:R5C2", what recorded macros write) becomes A1."""
    text = str(text)
    if text.startswith("="):
        text = text[1:]
    sheet, bang, ref = text.rpartition("!")
    if _re.fullmatch(r"R\d+C\d+(:R\d+C\d+)?", ref.strip(), _re.I):
        from . import _r1c1

        ref = _r1c1.r1c1_to_a1("=" + ref.strip().upper(), 0, 0)[1:]
    return sheet + bang + ref


def _source_r1c1(text) -> str:
    """A PivotTable's SourceData as Excel reads it: "Sheet1!R1C1:R5C2" (the sheet quoted only when it must be); a table
    name stays as it is."""
    text = str(text)
    sheet, bang, ref = text.rpartition("!")
    if not _re.fullmatch(r"\$?[A-Za-z]{1,3}\$?\d+(:\$?[A-Za-z]{1,3}\$?\d+)?", ref):
        return text
    from . import _r1c1

    ref = _r1c1.a1_to_r1c1("=" + _re.sub(r"\$?([A-Za-z]{1,3})\$?(\d+)", r"$\1$\2", ref), 0, 0)[1:]
    if not bang:
        return ref
    if sheet.startswith("'") and sheet.endswith("'"):
        sheet = sheet[1:-1].replace("''", "'")
    return _quote_sheet(_re.sub(r"^\[[^\]]*\]", "", sheet)) + "!" + ref


class _PivotCaches:
    def __call__(self):
        return self

    def Create(self, SourceType=1, SourceData=None, Version=None):  # noqa: N802,N803
        """SourceData: a Range, an address ("Data!A1:D100") or a table name."""
        if SourceData is None:
            raise ApplicationDefinedError("PivotCaches.Create needs SourceData")
        src = SourceData.Address(External=True) if isinstance(SourceData, Range) else _source_a1(SourceData)
        return _PivotCacheObj(src)

    def Refresh(self):  # noqa: N802
        _rq("pivot.refresh_all")


def _opt_put(pt, key, value):
    """Write one PivotTable Options value."""
    d = pt._get()
    d["options"][key] = value
    pt._put(d)


def _body(d, o):
    """A report's rectangle without the column only its filter area needs: one value under a filter is one column
    wide (TableRange1 A3:A4, TableRange2 A1:B4, Excel 16 measured)."""
    n = d.get("body_cols") or 0
    if n <= 0:
        return tuple(o)
    return o[0], o[1], o[2], min(o[3], o[1] + n - 1)


class PivotTable:
    def __init__(self, sheet_id, name):
        self._sheet, self._name = sheet_id, name

    def __repr__(self):
        return f"<PivotTable {self._name}>"

    def _get(self) -> dict:
        pending = _PIVOT_PENDING.get((self._sheet, self._name.lower()))
        if pending is not None:
            return _copy_dict(pending)
        return _rq("pivot.get", sheet=self._sheet, name=self._name)

    def _put(self, d: dict) -> dict:
        key = (self._sheet, self._name.lower())
        if key in _PIVOT_PENDING:
            _PIVOT_PENDING[key] = _copy_dict(d)  # ManualUpdate: applied when it is set back to False
            return d
        out = _rq("pivot.set", sheet=self._sheet, name=self._name, alerts=_alerts(), **{"def": d})
        if out:
            self._name = out["name"]
        return out

    @property
    def ManualUpdate(self):  # noqa: N802
        """True: changes wait (Defer Layout Update) until it is set back to False."""
        return (self._sheet, self._name.lower()) in _PIVOT_PENDING

    @ManualUpdate.setter
    def ManualUpdate(self, v):  # noqa: N802
        key = (self._sheet, self._name.lower())
        if v and key not in _PIVOT_PENDING:
            _PIVOT_PENDING[key] = _rq("pivot.get", sheet=self._sheet, name=self._name)
        elif not v and key in _PIVOT_PENDING:
            d = _PIVOT_PENDING.pop(key)
            self._put(d)

    def _opt(key, conv=bool):  # noqa: N805
        def g(self):
            return conv(self._get()["options"][key])

        def s(self, v):
            d = self._get()
            d["options"][key] = conv(v)
            self._put(d)

        return property(g, s)

    MergeLabels = _opt("merge_labels")
    DisplayErrorString = _opt("show_error")
    ErrorString = _opt("error_string", str)
    DisplayNullString = _opt("show_null")
    NullString = _opt("null_string", str)
    HasAutoFormat = _opt("autofit")  # Autofit column widths on update
    PreserveFormatting = _opt("preserve_formatting")
    ShowDrillIndicators = _opt("show_buttons")
    DisplayFieldCaptions = _opt("field_captions")
    EnableDrilldown = _opt("enable_drilldown")
    SortUsingCustomLists = _opt("custom_lists")
    CompactRowIndent = _opt("indent", int)
    ShowTableStyleRowHeaders = _opt("style_row_headers")
    ShowTableStyleColumnHeaders = _opt("style_col_headers")
    ShowTableStyleRowStripes = _opt("style_banded_rows")
    ShowTableStyleColumnStripes = _opt("style_banded_cols")
    ShowAllFieldButtons = _opt("chart_buttons")  # [YC] the field buttons of this PivotTable's PivotCharts (Chart.ShowAllFieldButtons)
    EnableSelection = _opt("enable_selection")  # [YC] Analyze > Select > Enable Selection
    # PivotTable Options > Display > Field List: Sort A to Z (True) or in data source order (False)
    FieldListSortAscending = property(lambda self: self._get()["options"].get("field_list_order", 0) == 1,
                                      lambda self, v: _opt_put(self, "field_list_order", 1 if v else 0))
    del _opt

    # ---- The report's areas (Excel 16's) ----
    def _areas(self):
        d = self._get()
        o = d["output"]
        if not o:
            raise ApplicationDefinedError(f"The PivotTable {self._name} has no report")
        return d, _body(d, o)

    @property
    def RowRange(self):  # noqa: N802
        """The row area: the row labels' column(s) from their caption down to the grand total."""
        d, o = self._areas()
        if not d["label_cols"]:
            # no row labels (one value, or values over column fields): Excel 16 raises (measured)
            raise ApplicationDefinedError("The PivotTable has no row area")
        top = o[0] + d["page_rows"] + max(0, d["header_rows"] - 1)
        return self._range(top, o[1], o[2], o[1] + d["label_cols"] - 1)

    @property
    def ColumnRange(self):  # noqa: N802
        """The column area: the column fields' caption and items over the values (only with column fields)."""
        d, o = self._areas()
        if not d["cols"]:
            raise ApplicationDefinedError("The PivotTable has no column fields")
        top = o[0] + d["page_rows"]
        return self._range(top, o[1] + d["label_cols"], top + max(1, d["header_rows"]) - 1, o[3])

    @property
    def DataLabelRange(self):  # noqa: N802
        """The cell with the values' caption (Sum of Sales, or Values for several data fields): the report's corner."""
        d, o = self._areas()
        if not d["data"] or (not d["cols"] and len(d["data"]) == 1):
            # one data field without column fields has no caption cell of its own (Excel 16 raises too, measured)
            raise ApplicationDefinedError("The PivotTable has no data label cell")
        r = o[0] + d["page_rows"]
        return self._range(r, o[1], r, o[1])

    @property
    def PageRange(self):  # noqa: N802
        """The filter area above the report: each filter's name and value (None without filters)."""
        d, o = self._areas()
        n = len(d["pages"])
        if not n:
            return None
        return self._range(o[0], o[1], o[0] + n - 1, o[1] + 1)

    PageRangeCells = PageRange

    @property
    def DataPivotField(self):  # noqa: N802
        """The Values field: its PivotItems are the data fields; on rows or columns when there are several."""
        return _DataPivotField(self)

    def _shown_fields(self, d=None):
        d = d or self._get()
        used = {e["field"] for key in ("rows", "cols", "pages") for e in d[key]} | {e["field"] for e in d["data"]}
        return used

    @property
    def VisibleFields(self):  # noqa: N802
        """The fields on the report (rows, columns, filters, values)."""
        d = self._get()
        used = self._shown_fields(d)
        return [PivotField(self, n) for i, n in enumerate(d["fields"]) if i in used]

    @property
    def HiddenFields(self):  # noqa: N802
        """The fields that are on no area of the report."""
        d = self._get()
        used = self._shown_fields(d)
        return [PivotField(self, n) for i, n in enumerate(d["fields"]) if i not in used]

    @property
    def Slicers(self):  # noqa: N802
        """The slicers connected to this PivotTable."""
        out = []
        for sc in ActiveWorkbook.SlicerCaches:
            if any(getattr(pt, "_name", None) == self._name for pt in sc.PivotTables):
                out.extend(sc.Slicers)
        return out

    @property
    def GrandTotalName(self):  # noqa: N802
        """The grand totals' label ("Grand Total" until set; "" puts it back)."""
        return self._get()["options"].get("grand_name") or "Grand Total"

    @GrandTotalName.setter
    def GrandTotalName(self, v):  # noqa: N802
        _opt_put(self, "grand_name", "" if v is None or str(v) == "Grand Total" else str(v))

    @property
    def RefreshDate(self):  # noqa: N802
        """When the report was last refreshed (a datetime; None before the first refresh)."""
        t = self._get()["options"].get("refreshed_at") or 0
        return serial_to_datetime(t + _date_offset()) if t else None

    @property
    def RefreshName(self):  # noqa: N802
        """Who refreshed it last (Application.UserName, else the Windows user)."""
        return self._get()["options"].get("refreshed_by") or ""

    def GetData(self, Name):  # noqa: N802,N803
        """The value for "Sum of Sales East 2024": a data field's caption (quoted when it has spaces) and the items
        that pick the cell, as Excel's GetData reads them."""
        import shlex

        words = shlex.split(str(Name).replace("'", '"'))
        if not words:
            raise ApplicationDefinedError("GetData needs a data field and items")
        d = self._get()
        caps = [c.lower() for c in d.get("captions", [])]
        data = words[0] if words[0].lower() in caps else (d.get("captions") or [""])[0]
        items = words[1:] if words[0].lower() in caps else words
        pairs = []
        for it in items:
            for key in ("rows", "cols", "pages"):
                hit = None
                for e in d[key]:
                    f = PivotField(self, d["fields"][e["field"]])
                    if any(pi.Name.lower() == it.lower() or pi.SourceName.lower() == it.lower() for pi in f.PivotItems()):
                        hit = f._name
                        break
                if hit:
                    pairs += [hit, it]
                    break
            else:
                raise ApplicationDefinedError(f"GetData: no field of the PivotTable has the item {it}")
        return self.GetPivotData(data, *pairs)

    def ShowPages(self, PageField=None):  # noqa: N802,N803
        """PivotTable Analyze > Options > Show Report Filter Pages: a new sheet for each item of a filter field, named
        after it, with a copy of this report filtered to that item, at the same place."""
        d = self._get()
        if not d["pages"]:
            raise ApplicationDefinedError("The PivotTable has no filter fields")
        name = PageField.Name if isinstance(PageField, PivotField) else (str(PageField) if PageField else d["fields"][d["pages"][0]["field"]])
        field = PivotField(self, name)
        idx = field._index(d)
        if not any(e["field"] == idx for e in d["pages"]):
            raise ApplicationDefinedError(f"{name} isn't a filter field of the PivotTable")
        o = d["output"]
        here = Worksheet(self._sheet)
        made = []
        for it in field.PivotItems():
            ws = Worksheets.Add(Before=here)
            try:
                ws.Name = str(it.Name)[:31]
            except YcError:
                pass
            pt = self.PivotCache().CreatePivotTable(ws.Cells(o[0] + 1, o[1] + 1))
            nd = pt._get()
            for key in ("rows", "cols", "pages", "data", "values_on_rows", "row_grand", "col_grand", "layout", "subtotals",
                        "style", "options", "groups", "calc_fields"):
                if key in d:
                    nd[key] = _copy_dict({"v": d[key]})["v"]
            for e in nd["pages"]:
                if e["field"] == idx:
                    e["page_item"], e["hidden"] = it.SourceName, []
            pt._put(nd)
            made.append(ws)
        return made

    def RepeatAllLabels(self, Repeat):  # noqa: N802,N803
        """xlRepeatLabels (2) or xlDoNotRepeatLabels (1) for every row field."""
        d = self._get()
        for e in d["rows"]:
            e["repeat_labels"] = int(Repeat) == 2
        self._put(d)

    def ClearAllFilters(self):  # noqa: N802
        """Every field's filters, slicers and timelines."""
        d = self._get()
        empty = {"type": 0, "data": 0, "v1": "", "v2": ""}
        for key in ("rows", "cols", "pages"):
            for e in d[key]:
                e["hidden"], e["page_item"] = [], ""
                e["label_filter"], e["value_filter"] = dict(empty), dict(empty)
        d["slicer_filters"], d["date_filters"] = [], []
        self._put(d)

    def PivotSelect(self, Name="", Mode=xlDataAndLabel, UseStandardName=None):  # noqa: N802,N803
        """[YC] Name "" = the whole report; Mode xlDataAndLabel / xlLabelOnly / xlDataOnly. Selects the cells and returns them."""
        d = self._get()
        o = d["output"]
        if not o:
            return None
        top = o[0] + d["page_rows"]
        if int(Mode) == xlDataOnly:
            r = self._range(top + d["header_rows"], o[1] + d["label_cols"], o[2], o[3])
        elif int(Mode) == xlLabelOnly:
            r = self._range(top, o[1], o[2], o[1] + max(1, d["label_cols"]) - 1)
        else:
            r = self._range(top, o[1], o[2], o[3])
        r.Select()
        return r

    @property
    def Name(self):  # noqa: N802
        return self._name

    @Name.setter
    def Name(self, v):  # noqa: N802
        d = self._get()
        d["name"] = str(v)
        self._put(d)

    @property
    def Parent(self):  # noqa: N802
        return Worksheet(self._sheet)

    def PivotFields(self, Index=None):  # noqa: N802,N803
        d = self._get()
        lst = _PivotFieldList([PivotField(self, n) for n in d["fields"]])
        if Index is None:
            return lst
        try:
            return lst.Item(Index)
        except SubscriptOutOfRangeError:
            # a data field by its caption, as recorded macros write PivotFields("Sum of Sales").NumberFormat (Excel 16
            # finds it, measured; gaps_fuzz found YCGrid raising)
            if isinstance(Index, str):
                for i, e in enumerate(d["data"]):
                    f = PivotField(self, d["fields"][e["field"]], i)
                    if f.Name.lower() == Index.lower():
                        return f
            raise

    def _axis_fields(self, key):
        d = self._get()
        return _PivotFieldList([PivotField(self, d["fields"][e["field"]]) for e in d[key]])

    @property
    def RowFields(self):  # noqa: N802
        return self._axis_fields("rows")

    @property
    def ColumnFields(self):  # noqa: N802
        return self._axis_fields("cols")

    @property
    def PageFields(self):  # noqa: N802
        return self._axis_fields("pages")

    @property
    def DataFields(self):  # noqa: N802
        d = self._get()
        return _PivotFieldList([PivotField(self, d["fields"][e["field"]], i) for i, e in enumerate(d["data"])])

    def AddDataField(self, Field, Caption=None, Function=None):  # noqa: N802,N803
        """Add a data field (Values area): Function xlSum / xlCount / xlAverage / xlMax / xlMin..."""
        d = self._get()
        name = Field.SourceName if isinstance(Field, PivotField) else str(Field)
        f = PivotField(self, name)._index(d)
        func = 0 if Function is None else _pivot_func(Function)
        d["data"].append({"field": f, "func": func, "show_as": 0, "caption": str(Caption or ""), "number_format": ""})
        self._put(d)
        return PivotField(self, name, len(d["data"]) - 1)

    def AddFields(self, RowFields=None, ColumnFields=None, PageFields=None, AddToTable=False):  # noqa: N802,N803
        d = self._get()

        def norm(v):
            if v is None:
                return []
            return [v] if isinstance(v, (str, PivotField)) else list(v)

        for key, names in (("rows", norm(RowFields)), ("cols", norm(ColumnFields)), ("pages", norm(PageFields))):
            if not AddToTable:
                d[key] = []
            for n in names:
                f = PivotField(self, n.SourceName if isinstance(n, PivotField) else str(n))._index(d)
                for k2 in ("rows", "cols", "pages"):
                    d[k2] = [e for e in d[k2] if e["field"] != f]
                d[key].append({"field": f, "hidden": [], "subtotal": True, "descending": False, "page_item": ""})
        self._put(d)

    def RefreshTable(self):  # noqa: N802
        """Read the source again. A report that would grow over data raises (Excel asks) unless Application.DisplayAlerts
        is False; it never covers a table."""
        _rq("pivot.refresh", sheet=self._sheet, name=self._name, alerts=_alerts())
        return True

    def CalculatedFields(self):  # noqa: N802
        """Fields computed from other fields' sums: CalculatedFields().Add("Unit", "=Sales/Qty")."""
        return _CalculatedFields(self)

    def PivotCache(self):  # noqa: N802
        return _PivotCacheObj(self._get()["source"], self)

    @property
    def SourceData(self):  # noqa: N802
        """The source as Excel reads it: R1C1 text ("Sheet1!R1C1:R5C2"), or the table's name."""
        return _source_r1c1(self._get()["source"])

    def ChangePivotCache(self, PivotCache):  # noqa: N802,N803
        """Use another source: a PivotCache (Workbook.PivotCaches().Create(...) or another report's), a Range or a table name."""
        src = getattr(PivotCache, "SourceData", PivotCache)
        self.SourceData = src

    @SourceData.setter
    def SourceData(self, v):  # noqa: N802
        src = v.Address(External=True) if isinstance(v, Range) else _source_a1(v)
        d = self._get()
        try:
            r = _make_range(self._sheet, src)
            d["src_sheet"], d["src"], d["src_table"] = r._sheet, list(r._areas[0]), ""
        except YcError:
            d["src_table"] = src  # a table name
        self._put(d)

    def _range(self, r0, c0, r1, c1):
        return Range._from(self._sheet, [(r0, c0, r1, c1)])

    @property
    def TableRange2(self):  # noqa: N802
        """The whole report, filters included."""
        o = self._get()["output"]
        if not o:
            return None
        return self._range(*o)

    @property
    def TableRange1(self):  # noqa: N802
        """The report without the filter area."""
        d = self._get()
        o = d["output"]
        if not o:
            return None
        return self._range(*_body(d, (o[0] + d["page_rows"], o[1], o[2], o[3])))

    @property
    def DataBodyRange(self):  # noqa: N802
        d = self._get()
        o = d["output"]
        if not o or not d["data"]:
            return None
        o = _body(d, o)
        return self._range(o[0] + d["page_rows"] + d["header_rows"], o[1] + d["label_cols"], o[2], o[3])

    @property
    def Location(self):  # noqa: N802
        d = self._get()
        return Range._from(self._sheet, [(d["r0"], d["c0"], d["r0"], d["c0"])]).Address()

    @Location.setter
    def Location(self, v):  # noqa: N802
        r = v if isinstance(v, Range) else _make_range(self._sheet, str(v))
        d = self._get()
        # the report's body (TableRange1) goes to the cell and its filters above it, as in Excel (measured: Location
        # F10 with a filter gives TableRange1 F10, TableRange2 F8); with too few rows above, the body moves down
        pages = len(d["pages"]) + 1 if d["pages"] else 0
        d["r0"], d["c0"] = max(r._a[0], pages) - pages, r._a[1]
        self._put(d)

    def _flag(key):  # noqa: N805
        def g(self):
            return bool(self._get()[key])

        def s(self, v):
            d = self._get()
            d[key] = bool(v)
            self._put(d)

        return property(g, s)

    RowGrand = _flag("col_grand")     # Excel: RowGrand = the grand total column at the end of every row
    ColumnGrand = _flag("row_grand")  # ColumnGrand = the grand total row below every column
    del _flag

    def RowAxisLayout(self, RowLayout):  # noqa: N802,N803
        d = self._get()
        d["layout"] = {xlCompactRow: 0, xlOutlineRow: 1, xlTabularRow: 2}[int(RowLayout)]
        if d["layout"] == 2 and d["subtotals"] == 1:
            d["subtotals"] = 2
        self._put(d)

    def SubtotalLocation(self, Location):  # noqa: N802,N803
        d = self._get()
        d["subtotals"] = 1 if int(Location) == xlAtTop else 2
        self._put(d)

    @property
    def TableStyle2(self):  # noqa: N802
        return self._get()["style"]

    @TableStyle2.setter
    def TableStyle2(self, v):  # noqa: N802
        d = self._get()
        d["style"] = str(v)
        self._put(d)

    def ClearTable(self):  # noqa: N802
        d = self._get()
        d["rows"], d["cols"], d["pages"], d["data"] = [], [], [], []
        self._put(d)

    def GetPivotData(self, DataField=None, *pairs):  # noqa: N802,N803
        """[YC] Returns the value (Excel returns the Range holding it); pairs are field, item, field, item..."""
        d = self._get()
        if DataField is None:
            DataField = d["captions"][0] if d["captions"] else ""
        args = [f'"{DataField}"', Range._from(self._sheet, [(d["r0"], d["c0"], d["r0"], d["c0"])]).Address(External=True)]
        args += [f'"{p}"' for p in pairs]
        return Application.Evaluate("GETPIVOTDATA(" + ",".join(args) + ")")

    def Delete(self):  # noqa: N802
        """[YC] Delete the PivotTable and clear its report."""
        _rq("pivot.delete", sheet=self._sheet, name=self._name, clear=True)


class _PivotTables:
    def __init__(self, sheet_id):
        self._sheet = sheet_id

    def _names(self):
        return [p["name"] for p in _rq("pivot.list", sheet=self._sheet)]

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def Item(self, Index):  # noqa: N802,N803
        names = self._names()
        if isinstance(Index, int):
            if not 1 <= Index <= len(names):
                raise SubscriptOutOfRangeError("PivotTables index out of range")
            return PivotTable(self._sheet, names[Index - 1])
        for n in names:
            if n.lower() == str(Index).lower():
                return PivotTable(self._sheet, n)
        raise SubscriptOutOfRangeError(f"PivotTable not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([PivotTable(self._sheet, n) for n in self._names()])

    def __len__(self):
        return self.Count

    def Add(self, PivotCache, TableDestination, TableName=None):  # noqa: N802,N803
        return PivotCache.CreatePivotTable(TableDestination, TableName)


# ---- Page setup and printing (Page Layout tab, File -> Print, Export -> PDF) ----
class _HFText:
    """PageSetup.FirstPage.CenterHeader: its Text."""

    def __init__(self, page, key, index):
        self._p, self._k, self._i = page, key, index

    @property
    def Text(self):  # noqa: N802
        return self._p._get()[self._k][self._i]

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._p._set(**{self._k: [self._i, str(v)]})


class _HFPage:
    """PageSetup.FirstPage / EvenPage: LeftHeader, CenterHeader, RightHeader, LeftFooter, CenterFooter, RightFooter."""

    def __init__(self, page_setup, key):
        self._ps, self._k = page_setup, key

    def _get(self):
        return self._ps._get()

    def _set(self, **props):
        return self._ps._set(**props)

    LeftHeader = property(lambda self: _HFText(self, self._k, 0))
    CenterHeader = property(lambda self: _HFText(self, self._k, 1))
    RightHeader = property(lambda self: _HFText(self, self._k, 2))
    LeftFooter = property(lambda self: _HFText(self, self._k, 3))
    CenterFooter = property(lambda self: _HFText(self, self._k, 4))
    RightFooter = property(lambda self: _HFText(self, self._k, 5))


class _HFGraphic:
    """PageSetup.CenterHeaderPicture: Filename (setting it loads the picture), Width and Height in points."""

    _files = {}  # (sheet, section) -> the file last loaded (Filename reads it back)

    def __init__(self, page_setup, section):
        object.__setattr__(self, "_ps", page_setup)
        object.__setattr__(self, "_k", section)

    def _info(self):
        return self._ps._get()["hf_pictures"][self._k]

    @property
    def Filename(self):  # noqa: N802
        return self._files.get((self._ps._sheet, self._k), "") if self._info()["has"] else ""

    @Filename.setter
    def Filename(self, v):  # noqa: N802
        import os

        path = os.path.abspath(str(v)) if v else ""
        self._ps._set(hf_picture=[self._k, path if path else None, 0, 0])
        self._files[(self._ps._sheet, self._k)] = path

    @property
    def Width(self):  # noqa: N802
        return float(self._info()["width"])

    @Width.setter
    def Width(self, v):  # noqa: N802
        self._ps._set(hf_picture=[self._k, "", float(v), 0])

    @property
    def Height(self):  # noqa: N802
        return float(self._info()["height"])

    @Height.setter
    def Height(self, v):  # noqa: N802
        self._ps._set(hf_picture=[self._k, "", 0, float(v)])

    def __setattr__(self, name, value):
        if not hasattr(type(self), name):
            raise AttributeError(f"Graphic has no property {name}")
        object.__setattr__(self, name, value)


class _PageSetup:
    """Worksheet.PageSetup: margins and header / footer heights are in points; Zoom is a percent or False (then FitToPagesWide x
    FitToPagesTall pages)."""

    def __init__(self, sheet_id):
        object.__setattr__(self, "_sheet", sheet_id)

    def _get(self):
        return _rq("page.get", sheet=self._sheet)

    def _set(self, **props):
        return _rq("page.set", sheet=self._sheet, props=props)

    def _prop(key, to_xl=lambda v: v, to_yc=lambda v: v):  # noqa: N805
        def g(self):
            return to_xl(self._get()[key])

        def s(self, v):
            self._set(**{key: to_yc(v)})

        return property(g, s)

    Orientation = _prop("landscape", lambda v: xlLandscape if v else xlPortrait, lambda v: int(v) == xlLandscape)
    PaperSize = _prop("paper", int, int)
    LeftMargin = _prop("left", float, float)
    RightMargin = _prop("right", float, float)
    TopMargin = _prop("top", float, float)
    BottomMargin = _prop("bottom", float, float)
    HeaderMargin = _prop("header", float, float)
    FooterMargin = _prop("footer", float, float)
    PrintGridlines = _prop("gridlines", bool, bool)
    PrintHeadings = _prop("headings", bool, bool)
    CenterHorizontally = _prop("center_h", bool, bool)
    CenterVertically = _prop("center_v", bool, bool)
    BlackAndWhite = _prop("black_and_white", bool, bool)
    Order = _prop("over_then_down", lambda v: xlOverThenDown if v else xlDownThenOver, lambda v: int(v) == xlOverThenDown)
    FirstPageNumber = _prop("first_page_number", lambda v: v if v > 0 else xlAutomatic, lambda v: 0 if int(v) == xlAutomatic else int(v))
    LeftHeader = _prop("header_left", str, str)
    CenterHeader = _prop("header_center", str, str)
    RightHeader = _prop("header_right", str, str)
    LeftFooter = _prop("footer_left", str, str)
    CenterFooter = _prop("footer_center", str, str)
    RightFooter = _prop("footer_right", str, str)
    PrintArea = _prop("print_area", lambda v: v or "", lambda v: _print_ref(v))
    PrintComments = _prop("print_comments", lambda v: {0: xlPrintNoComments, 1: xlPrintSheetEnd, 2: xlPrintInPlace}[int(v)],
                          lambda v: {xlPrintNoComments: 0, xlPrintSheetEnd: 1, xlPrintInPlace: 2}[int(v)])
    PrintErrors = _prop("print_errors", int, int)
    Draft = _prop("draft", bool, bool)
    PrintTitleRows = _prop("title_rows", lambda v: v or "", lambda v: _print_ref(v))
    PrintTitleColumns = _prop("title_cols", lambda v: v or "", lambda v: _print_ref(v))
    DifferentFirstPageHeaderFooter = _prop("different_first", bool, bool)
    OddAndEvenPagesHeaderFooter = _prop("different_odd_even", bool, bool)
    ScaleWithDocHeaderFooter = _prop("scale_with_doc", bool, bool)
    AlignMarginsHeaderFooter = _prop("align_margins", bool, bool)
    del _prop

    # Header / Footer > Insert Picture: the picture a section shows where its text has &G (Graphic: Filename, Width, Height)
    LeftHeaderPicture = property(lambda self: _HFGraphic(self, 0))
    CenterHeaderPicture = property(lambda self: _HFGraphic(self, 1))
    RightHeaderPicture = property(lambda self: _HFGraphic(self, 2))
    LeftFooterPicture = property(lambda self: _HFGraphic(self, 3))
    CenterFooterPicture = property(lambda self: _HFGraphic(self, 4))
    RightFooterPicture = property(lambda self: _HFGraphic(self, 5))

    @property
    def FirstPage(self):  # noqa: N802
        """The first page's header and footer (used with DifferentFirstPageHeaderFooter): FirstPage.CenterHeader.Text."""
        return _HFPage(self, "first_hf")

    @property
    def EvenPage(self):  # noqa: N802
        """The even pages' header and footer (used with OddAndEvenPagesHeaderFooter)."""
        return _HFPage(self, "even_hf")

    @property
    def Zoom(self):  # noqa: N802
        z = self._get()["zoom"]
        return z if z > 0 else False

    @Zoom.setter
    def Zoom(self, v):  # noqa: N802
        self._set(zoom=0 if v is False or v is None else int(v))

    @property
    def FitToPagesWide(self):  # noqa: N802
        v = self._get()["fit_wide"]
        return v if v > 0 else False

    @FitToPagesWide.setter
    def FitToPagesWide(self, v):  # noqa: N802
        self._set(fit_wide=0 if v is False or v is None else int(v))

    @property
    def FitToPagesTall(self):  # noqa: N802
        v = self._get()["fit_tall"]
        return v if v > 0 else False

    @FitToPagesTall.setter
    def FitToPagesTall(self, v):  # noqa: N802
        self._set(fit_tall=0 if v is False or v is None else int(v))

    @property
    def Pages(self):  # noqa: N802
        """Pages.Count: how many pages the sheet prints on."""
        n = _rq("page.count", sheet=self._sheet)

        class _Pages:
            Count = n

            def __len__(self):
                return n

        return _Pages()

    def __setattr__(self, name, value):
        if not hasattr(type(self), name):
            raise AttributeError(f"PageSetup has no property {name}")
        object.__setattr__(self, name, value)


def _print_ref(v):
    """PrintArea / PrintTitleRows: an address, a Range, or "" / False / None to clear."""
    if v is None or v is False or v == "":
        return ""
    if isinstance(v, str):
        return v
    return v.Address()


class _PageBreak:
    def __init__(self, sheet_id, rows, index, manual):
        self._sheet, self._rows, self._index, self._manual = sheet_id, rows, index, manual

    @property
    def Location(self):  # noqa: N802
        """The first row (HPageBreak) or column (VPageBreak) of the new page."""
        if self._rows:
            return Range._from(self._sheet, [(self._index, 0, self._index, MAX_COLS - 1)])
        return Range._from(self._sheet, [(0, self._index, ROW_END, self._index)])

    @property
    def Type(self):  # noqa: N802
        return xlPageBreakManual if self._manual else xlPageBreakAutomatic

    @property
    def Extent(self):  # noqa: N802
        return 1  # xlPageBreakFull

    def Delete(self):  # noqa: N802
        _rq("page.breaks", sheet=self._sheet, rows=self._rows, remove=self._index)


class _PageBreaks:
    """Worksheet.HPageBreaks / VPageBreaks: automatic and manual breaks of the current page setup."""

    def __init__(self, sheet_id, rows):
        self._sheet, self._rows = sheet_id, rows

    def _all(self):
        return [_PageBreak(self._sheet, self._rows, i, m) for i, m in _rq("page.breaks", sheet=self._sheet, rows=self._rows)]

    def Add(self, Before):  # noqa: N802,N803
        a = Before._a
        idx = a[0] if self._rows else a[1]
        _rq("page.breaks", sheet=self._sheet, rows=self._rows, add=int(idx))
        return _PageBreak(self._sheet, self._rows, idx, True)

    @property
    def Count(self):  # noqa: N802
        return len(self._all())

    def Item(self, Index):  # noqa: N802,N803
        items = self._all()
        if not 1 <= int(Index) <= len(items):
            raise SubscriptOutOfRangeError("Page break index out of range")
        return items[int(Index) - 1]

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter(self._all())

    def __len__(self):
        return self.Count


def _print_out(sheets, From=None, To=None, Copies=1, Preview=False, ActivePrinter=None, PrintToFile=False, Collate=True, PrToFileName=None,
               IgnorePrintAreas=False, area=None):
    if Preview:
        _rq("print.preview")
        return
    p = {"sheets": list(sheets), "from": int(From or 1), "to": int(To or 0), "copies": int(Copies or 1), "collate": bool(Collate),
         "printer": str(ActivePrinter or Application.ActivePrinter or ""), "ignore_area": bool(IgnorePrintAreas)}
    if PrintToFile and PrToFileName:
        p["path"] = str(PrToFileName)
    if area is not None:
        p["area"] = list(area)
    _rq("print.out", sheet=p["sheets"][0], **p)


def _export_pdf(sheets, Type=0, Filename=None, Quality=0, IncludeDocProperties=True, IgnorePrintAreas=False, From=None, To=None, OpenAfterPublish=False,
                area=None):
    import os

    if int(Type) != xlTypePDF:
        raise UnsupportedError("Only xlTypePDF is supported")
    if not Filename:
        raise ApplicationDefinedError("ExportAsFixedFormat needs a Filename")
    path = os.path.abspath(str(Filename))
    if not path.lower().endswith(".pdf"):
        path += ".pdf"
    p = {"sheets": list(sheets), "path": path, "from": int(From or 1), "to": int(To or 0), "ignore_area": bool(IgnorePrintAreas)}
    if area is not None:
        p["area"] = list(area)
    _rq("print.pdf", sheet=p["sheets"][0], **p)
    if OpenAfterPublish:
        _open_on_desktop(path)
    return path


# ---- Sparklines (Insert -> Sparklines) ----
_SPARK_TYPE = {1: 0, 2: 1, 3: 2}  # xlSparkLine / xlSparkColumn / xlSparkColumnStacked100 (win / loss)


def _spark_argb(v) -> int:
    v = int(v) & 0xFFFFFF
    return 0xFF000000 | ((v & 0xFF) << 16) | (v & 0xFF00) | (v >> 16)


def _spark_bgr(v) -> int:
    v = int(v)
    return ((v >> 16) & 0xFF) | (v & 0xFF00) | ((v & 0xFF) << 16)


class _SparkColor:
    """FormatColor: .Color is an Excel RGB value."""

    def __init__(self, g, key):
        self._g, self._k = g, key

    @property
    def Color(self):  # noqa: N802
        return _spark_bgr(self._g._get()[self._k])

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._g._set(**{self._k: _spark_argb(v)})


class _SparkPoint:
    def __init__(self, g, flag, color_key):
        self._g, self._f, self._c = g, flag, color_key

    @property
    def Visible(self):  # noqa: N802
        return self._g._get()[self._f]

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._g._set(**{self._f: bool(v)})

    @property
    def Color(self):  # noqa: N802
        return _SparkColor(self._g, self._c)


class _SparkPoints:
    def __init__(self, g):
        self._g = g

    Highpoint = property(lambda self: _SparkPoint(self._g, "high", "high_color"))
    Lowpoint = property(lambda self: _SparkPoint(self._g, "low", "low_color"))
    Firstpoint = property(lambda self: _SparkPoint(self._g, "first", "first_color"))
    Lastpoint = property(lambda self: _SparkPoint(self._g, "last", "last_color"))
    Negative = property(lambda self: _SparkPoint(self._g, "negative", "negative_color"))
    Markers = property(lambda self: _SparkPoint(self._g, "markers", "marker_color"))


class _SparkAxis:
    def __init__(self, g):
        self._g = g

    @property
    def Visible(self):  # noqa: N802
        return self._g._get()["axis"]

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._g._set(axis=bool(v))

    @property
    def Color(self):  # noqa: N802
        return _SparkColor(self._g, "axis_color")


class _SparkHorizontal:
    def __init__(self, g):
        self._g = g
        self.Axis = _SparkAxis(g)

    @property
    def RightToLeftPlotOrder(self):  # noqa: N802
        """Axis > Plot Data Right-to-Left."""
        return self._g._get()["right_to_left"]

    @RightToLeftPlotOrder.setter
    def RightToLeftPlotOrder(self, v):  # noqa: N802
        self._g._set(right_to_left=bool(v))


class _SparkVertical:
    """Axis > Vertical Axis Minimum / Maximum Value Options: xlSparkScaleGroup 1 (same for all), xlSparkScaleSingle 2
    (each sparkline's own), xlSparkScaleCustom 3 (CustomMinScaleValue / CustomMaxScaleValue)."""

    def __init__(self, g):
        self._g = g

    @property
    def MinScaleType(self):  # noqa: N802
        return self._g._get()["min_scale"]

    @MinScaleType.setter
    def MinScaleType(self, v):  # noqa: N802
        self._g._set(min_scale=int(v))

    @property
    def MaxScaleType(self):  # noqa: N802
        return self._g._get()["max_scale"]

    @MaxScaleType.setter
    def MaxScaleType(self, v):  # noqa: N802
        self._g._set(max_scale=int(v))

    @property
    def CustomMinScaleValue(self):  # noqa: N802
        return self._g._get()["min_value"]

    @CustomMinScaleValue.setter
    def CustomMinScaleValue(self, v):  # noqa: N802
        self._g._set(min_value=float(v), min_scale=3)

    @property
    def CustomMaxScaleValue(self):  # noqa: N802
        return self._g._get()["max_value"]

    @CustomMaxScaleValue.setter
    def CustomMaxScaleValue(self, v):  # noqa: N802
        self._g._set(max_value=float(v), max_scale=3)


class _SparkAxes:
    def __init__(self, g):
        self._g = g

    @property
    def Horizontal(self):  # noqa: N802
        return _SparkHorizontal(self._g)

    @property
    def Vertical(self):  # noqa: N802
        return _SparkVertical(self._g)


class SparklineGroup:
    """Sparklines that share a type and look (Range.SparklineGroups.Item)."""

    def __init__(self, sheet_id, gid):
        self._sheet, self._id = sheet_id, gid

    def __repr__(self):
        return f"<SparklineGroup {self._id}>"

    def _get(self) -> dict:
        for g in _rq("spark.list", sheet=self._sheet):
            if g["id"] == self._id:
                return g
        raise SubscriptOutOfRangeError("The sparkline group was deleted")

    def _set(self, **props):
        return _rq("spark.set", sheet=self._sheet, id=self._id, props=props)

    @property
    def Type(self):  # noqa: N802
        return {0: 1, 1: 2, 2: 3}[self._get()["type"]]

    @Type.setter
    def Type(self, v):  # noqa: N802
        self._set(type=_SPARK_TYPE.get(int(v), 0))

    @property
    def Count(self):  # noqa: N802
        return len(self._get()["items"])

    @property
    def Location(self):  # noqa: N802
        items = self._get()["items"]
        cells = [_make_range(self._sheet, it["location"])._areas[0] for it in items]
        r0, c0 = min(a[0] for a in cells), min(a[1] for a in cells)
        r1, c1 = max(a[2] for a in cells), max(a[3] for a in cells)
        return Range._from(self._sheet, [(r0, c0, r1, c1)])

    @property
    def SourceData(self):  # noqa: N802
        items = self._get()["items"]
        return ",".join(it["source"] for it in items)

    @property
    def SeriesColor(self):  # noqa: N802
        return _SparkColor(self, "color")

    @property
    def Points(self):  # noqa: N802
        return _SparkPoints(self)

    @property
    def Axes(self):  # noqa: N802
        return _SparkAxes(self)

    @property
    def LineWeight(self):  # noqa: N802
        return self._get()["line_weight"]

    @LineWeight.setter
    def LineWeight(self, v):  # noqa: N802
        self._set(line_weight=float(v))

    @property
    def DisplayBlanksAs(self):  # noqa: N802
        """Hidden & Empty Cells: xlNotPlotted 1 (gaps), xlZero 2, xlInterpolated 3 (connect data points with line)."""
        return self._get()["display_blanks"]

    @DisplayBlanksAs.setter
    def DisplayBlanksAs(self, v):  # noqa: N802
        self._set(display_blanks=int(v))

    @property
    def DisplayHidden(self):  # noqa: N802
        """Show data in hidden rows and columns."""
        return self._get()["show_hidden"]

    @DisplayHidden.setter
    def DisplayHidden(self, v):  # noqa: N802
        self._set(show_hidden=bool(v))

    @property
    def DateRange(self):  # noqa: N802
        """Axis > Date Axis Type: the dates that place the points ("" = General Axis Type)."""
        return self._get()["date_range"]

    @DateRange.setter
    def DateRange(self, v):  # noqa: N802
        self._set(date_range=v if isinstance(v, str) else v.Address(External=True))

    def ModifyDateRange(self, DateRange):  # noqa: N802,N803
        self.DateRange = DateRange

    def Modify(self, Location, SourceData):  # noqa: N802,N803
        """Edit Data > Edit Group Location & Data."""
        loc = Location if isinstance(Location, str) else Location.Address(False, False)
        src = SourceData if isinstance(SourceData, str) else SourceData.Address(External=True)
        _rq("spark.modify", sheet=self._sheet, id=self._id, location=loc, source=src)

    def ModifyLocation(self, Location):  # noqa: N802,N803
        loc = Location if isinstance(Location, str) else Location.Address(False, False)
        _rq("spark.modify", sheet=self._sheet, id=self._id, location=loc)

    def ModifySourceData(self, SourceData):  # noqa: N802,N803
        src = SourceData if isinstance(SourceData, str) else SourceData.Address(External=True)
        _rq("spark.modify", sheet=self._sheet, id=self._id, source=src)

    def Delete(self):  # noqa: N802
        _rq("spark.clear", sheet=self._sheet, id=self._id)


class SparklineGroups:
    """The sparkline groups in a range (Range.SparklineGroups)."""

    def __init__(self, rng):
        self._r = rng

    def _addr(self):
        return self._r.Address(False, False)  # the location is always on the range's own sheet

    def _groups(self):
        return _rq("spark.list", sheet=self._r._sheet, range=self._addr())

    def Add(self, Type, SourceData):  # noqa: N802,N803
        """Draw sparklines of SourceData (an address) in this range: one per row (or column) of the data."""
        src = SourceData if isinstance(SourceData, str) else SourceData.Address(External=True)
        d = _rq("spark.add", sheet=self._r._sheet, location=self._addr(), source=src, type=_SPARK_TYPE.get(int(Type), 0))
        return SparklineGroup(self._r._sheet, d["id"])

    @property
    def Count(self):  # noqa: N802
        return len(self._groups())

    def Item(self, Index):  # noqa: N802,N803
        gs = self._groups()
        if not 1 <= int(Index) <= len(gs):
            raise SubscriptOutOfRangeError("SparklineGroups index out of range")
        return SparklineGroup(self._r._sheet, gs[int(Index) - 1]["id"])

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([SparklineGroup(self._r._sheet, g["id"]) for g in self._groups()])

    def __len__(self):
        return self.Count

    def Clear(self):  # noqa: N802
        """Clear the sparklines in this range."""
        _rq("spark.clear", sheet=self._r._sheet, range=self._addr())

    def ClearGroups(self):  # noqa: N802
        """Delete every group that has a sparkline in this range."""
        for g in self._groups():
            _rq("spark.clear", sheet=self._r._sheet, id=g["id"])

    def Group(self, Location):  # noqa: N802,N803
        """Sparkline > Group: the sparklines in this range join the group at Location (it keeps its look)."""
        lead = Location.Address(False, False) if not isinstance(Location, str) else Location
        _rq("spark.group", sheet=self._r._sheet, range=self._addr(), lead=lead)

    def Ungroup(self):  # noqa: N802
        """Sparkline > Ungroup: each sparkline in this range becomes a group of its own."""
        _rq("spark.ungroup", sheet=self._r._sheet, range=self._addr())


class _ListObjects:
    def __init__(self, sheet_id):
        self._sheet = sheet_id

    def _list(self):
        return _rq("table.list", sheet=self._sheet)

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        lst = self._list()
        if isinstance(Index, str):
            for t in lst:
                if t["name"].lower() == Index.lower():
                    return ListObject(self._sheet, t["id"])
            raise SubscriptOutOfRangeError(f"No table named {Index!r}")
        i = int(Index) - 1
        if not 0 <= i < len(lst):
            raise SubscriptOutOfRangeError("ListObjects index out of range")
        return ListObject(self._sheet, lst[i]["id"])

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        for t in self._list():
            yield ListObject(self._sheet, t["id"])

    def Add(self, SourceType=1, Source=None, LinkSource=None, XlListObjectHasHeaders=xlGuess, Destination=None,  # noqa: N802,N803
            TableStyleName=None):
        """Create a table over Source (the current region of the active cell when omitted)."""
        if Source is None:
            rng = Application.ActiveCell.CurrentRegion
        elif isinstance(Source, str):
            rng = _make_range(self._sheet, Source, None)
        else:
            rng = Source
        _one_area(rng)
        a = rng._a
        if a[0] == a[2] and a[1] == a[3]:
            a = rng.CurrentRegion._a
        headers = XlListObjectHasHeaders != xlNo
        # xlGuess: YCGrid guesses as Excel does (the Create Table dialog's guess: a blank in the first two rows says no
        # header; text over a number below, or a first row formatted apart, says one)
        guess = {"guess": True} if XlListObjectHasHeaders == xlGuess else {}
        t = _rq("table.add", sheet=self._sheet, rect=list(a), headers=headers, **guess, **({} if TableStyleName is None else {"style": str(getattr(TableStyleName, "Name", TableStyleName))}))
        if not t:
            raise ApplicationDefinedError("A table can't be created here")
        return ListObject(self._sheet, t["id"])


class _Outline:
    __slots__ = ("_sheet",)

    def __init__(self, sheet_id):
        self._sheet = sheet_id

    def ShowLevels(self, RowLevels=0, ColumnLevels=0):  # noqa: N802,N803
        """Show outline levels 1..RowLevels of the rows (and of the columns); 0 leaves that direction unchanged."""
        _rq("outline.show_levels", sheet=self._sheet, rows=int(RowLevels or 0), cols=int(ColumnLevels or 0))

    def _settings(self, **change):
        return _rq("outline.settings", sheet=self._sheet, **change)

    @property
    def SummaryRow(self):  # noqa: N802
        """xlSummaryBelow (1, the default) or xlSummaryAbove (0): where a row group's summary row and its +/- button are."""
        return xlSummaryBelow if self._settings()["below"] else xlSummaryAbove

    @SummaryRow.setter
    def SummaryRow(self, v):  # noqa: N802
        self._settings(below=int(v) == xlSummaryBelow)

    @property
    def SummaryColumn(self):  # noqa: N802
        """xlSummaryOnRight (-4152, the default) or xlSummaryOnLeft (-4131)."""
        return xlSummaryOnRight if self._settings()["right"] else xlSummaryOnLeft

    @SummaryColumn.setter
    def SummaryColumn(self, v):  # noqa: N802
        self._settings(right=int(v) == xlSummaryOnRight)

    @property
    def AutomaticStyles(self):  # noqa: N802
        return bool(self._settings()["styles"])

    @AutomaticStyles.setter
    def AutomaticStyles(self, v):  # noqa: N802
        self._settings(styles=bool(v))


class _Watch:
    __slots__ = ("_index", "_sheet", "_r", "_c")

    def __init__(self, index, sheet, r, c):
        self._index, self._sheet, self._r, self._c = index, sheet, r, c

    @property
    def Source(self):  # noqa: N802
        return Range._from(self._sheet, [(self._r, self._c, self._r, self._c)])

    def Delete(self):  # noqa: N802
        _rq("watch.delete", index=self._index)


class _Watches:
    """Application.Watches: the cells of the Watch Window."""

    def _list(self):
        return _rq("watch.list")

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def __len__(self):
        return self.Count

    def Add(self, Source):  # noqa: N802,N803
        rng = Source if isinstance(Source, Range) else Range(Source)
        a = rng._a
        lst = _rq("watch.add", sheet=rng._sheet, row=a[0], col=a[1])
        return _Watch(len(lst) - 1, rng._sheet, a[0], a[1])

    def Delete(self):  # noqa: N802
        _rq("watch.delete", index=-1)

    def Item(self, Index):  # noqa: N802,N803
        lst = self._list()
        s, r, c = lst[int(Index) - 1]
        return _Watch(int(Index) - 1, s, r, c)

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        for i, (s, r, c) in enumerate(self._list()):
            yield _Watch(i, s, r, c)


class Worksheet:
    """A worksheet, identified by id (stays valid after renaming)."""

    __slots__ = ("_id", "_cx")

    def __init__(self, sheet_id: int):
        object.__setattr__(self, "_id", sheet_id)
        object.__setattr__(self, "_cx", getattr(_conn_tls, "conn", None))

    def _info(self):
        for s in _sheets_info():
            if s["id"] == self._id:
                return s
        raise SubscriptOutOfRangeError("The sheet has been deleted")

    def __eq__(self, other):
        return isinstance(other, Worksheet) and other._id == self._id and getattr(other, "_cx", None) is getattr(self, "_cx", None)

    def __hash__(self):
        return hash(("ws", self._id, id(getattr(self, "_cx", None))))

    def __repr__(self):
        try:
            return f"<Worksheet {self.Name!r}>"
        except YcError:
            return f"<Worksheet id={self._id}>"

    # ---- Properties ----
    def bind(self, data=None, key=None, **kw):
        """[YC] Bind a DataFrame / dict of columns to a live table on this sheet, updated by key through shared memory:
        t = ws.bind(df, key="strike", formats={"iv": "pct"}); t.update(changes). See yc.data.bind."""
        from .data import bind

        return bind(self, data, key=key, **kw)

    @property
    def CircularReference(self):  # noqa: N802
        """A cell of a circular reference on this sheet (the one the status bar names first), or None."""
        c = self._info().get("circular")
        if not c:
            return None
        return Range._from(self._id, [(c[0], c[1], c[0], c[1])])

    @property
    def ScrollArea(self):  # noqa: N802
        """The range the user can select and scroll to ("$A$1:$H$40"); "" means the whole sheet. Not saved, like Excel:
        set it again in Workbook_Open."""
        return _rq("sheet.scroll_area", sheet=self._id)

    @ScrollArea.setter
    def ScrollArea(self, v):  # noqa: N802
        _rq("sheet.scroll_area", sheet=self._id, area=v.Address() if isinstance(v, Range) else str(v or ""))

    @property
    def Sort(self):  # noqa: N802
        """The sheet's Sort object (SortFields, SetRange, Header, Apply)."""
        return _SORTS.setdefault(self._id, SortObj(self._id))

    @property
    def Name(self):  # noqa: N802
        return self._info()["name"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        _rq("sheet.rename", id=self._id, name=str(v))

    @property
    def CodeName(self):  # noqa: N802
        return self._info()["code_name"]

    @property
    def Names(self):  # noqa: N802
        """Sheet-level names of this sheet (Names.Add creates sheet-level names)."""
        return _Names(self._id)

    @property
    def Index(self):  # noqa: N802
        return self._info()["index"]

    @property
    def Visible(self):  # noqa: N802
        i = self._info()
        return xlSheetVisible if i["visible"] else xlSheetVeryHidden if i.get("very_hidden") else xlSheetHidden

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        """xlSheetVisible (True), xlSheetHidden (False) or xlSheetVeryHidden (not offered by Unhide); the last visible
        sheet can't be hidden."""
        if v is None:
            return
        _rq("sheet.visible", id=self._id, visible=v in (True, xlSheetVisible), very=v == xlSheetVeryHidden)

    def SaveAs(self, Filename, FileFormat=None, Password=None, WriteResPassword=None, ReadOnlyRecommended=None,  # noqa: N802,N803
               CreateBackup=None, AddToMru=None, TextCodepage=None, TextVisualLayout=None, Local=None):
        """Saves this sheet's workbook under a new name: a text format (xlCSV, xlCSVUTF8, a .csv name) writes this
        sheet, as Excel does; any other writes the whole workbook (Workbook.SaveAs)."""
        path = str(Filename)
        text = (FileFormat is not None and int(FileFormat) in (6, 62)) or path.lower().endswith(".csv")
        if not text:
            return Workbook.SaveAs(ActiveWorkbook, Filename, FileFormat, Password, WriteResPassword, ReadOnlyRecommended,
                                   CreateBackup, None, None, AddToMru)
        # this sheet's window (another YCGrid's sheet saves there): the sheet a CSV holds is the active one
        self.Activate()
        if FileFormat is not None and int(FileFormat) in (6, 62) and not path.lower().endswith(".csv"):
            path += ".csv"
        _rq("wb.save", path=path)

    # ---- Data > Consolidate: the last one onto this sheet (Excel keeps it with the sheet; not saved) ----
    _CONSOLIDATE_FUNCS = (xlSum, xlCount, xlAverage, xlMax, xlMin, xlProduct, xlCountNums, xlStDev, xlStDevP, xlVar, xlVarP)

    @property
    def ConsolidationFunction(self):  # noqa: N802
        """The function of the last consolidation onto the sheet (xlSum until there is one)."""
        c = _rq("sheet.consolidation", sheet=self._id)
        return self._CONSOLIDATE_FUNCS[c["func"]] if c and 0 <= c["func"] < len(self._CONSOLIDATE_FUNCS) else xlSum

    @property
    def ConsolidationOptions(self):  # noqa: N802
        """[Use labels in top row, Use labels in left column, Create links to source data] of the last consolidation."""
        c = _rq("sheet.consolidation", sheet=self._id)
        return [bool(c["top"]), bool(c["left"]), bool(c["links"])] if c else [False, False, False]

    @property
    def ConsolidationSources(self):  # noqa: N802
        """The references of the last consolidation, as R1C1 text ("Sheet1!R1C1:R5C3"); None when there was none."""
        c = _rq("sheet.consolidation", sheet=self._id)
        return list(c["sources"]) if c else None

    @property
    def EnableCalculation(self):  # noqa: N802
        """False: none of the sheet's formulas is calculated, whatever asks (an edit, F9, CalculateFull,
        Range.Calculate); a formula entered shows 0, other sheets read its cells as they show. True again calculates
        the whole sheet. Not saved (Excel 16, measured)."""
        return bool(_rq("sheet.calc", sheet=self._id))

    @EnableCalculation.setter
    def EnableCalculation(self, v):  # noqa: N802
        if v is None:
            return
        _rq("sheet.calc", sheet=self._id, on=bool(v))

    @property
    def EnableFormatConditionsCalculation(self):  # noqa: N802
        """False: the sheet's conditional formats keep what they show (they aren't evaluated again as values change);
        True evaluates them again. Not saved."""
        return bool(_rq("sheet.cf_calc", sheet=self._id))

    @EnableFormatConditionsCalculation.setter
    def EnableFormatConditionsCalculation(self, v):  # noqa: N802
        if v is None:
            return
        _rq("sheet.cf_calc", sheet=self._id, on=bool(v))

    def _ui_enable(self, which, v=None):
        if v is None:
            return bool(_rq("sheet.ui_enable", sheet=self._id, which=which))
        return bool(_rq("sheet.ui_enable", sheet=self._id, which=which, on=bool(v)))

    @property
    def EnableAutoFilter(self):  # noqa: N802
        """With Protect(UserInterfaceOnly=True): a person can still use the AutoFilter arrows. Not saved (False in a
        workbook just opened, as in Excel)."""
        return self._ui_enable(0)

    @EnableAutoFilter.setter
    def EnableAutoFilter(self, v):  # noqa: N802
        if v is not None:
            self._ui_enable(0, v)

    @property
    def EnableOutlining(self):  # noqa: N802
        """With Protect(UserInterfaceOnly=True): a person can still expand and collapse the outline (its symbols and
        levels). Not saved."""
        return self._ui_enable(1)

    @EnableOutlining.setter
    def EnableOutlining(self, v):  # noqa: N802
        if v is not None:
            self._ui_enable(1, v)

    @property
    def EnablePivotTable(self):  # noqa: N802
        """With Protect(UserInterfaceOnly=True): a person can still use the sheet's PivotTables. Not saved."""
        return self._ui_enable(2)

    @EnablePivotTable.setter
    def EnablePivotTable(self, v):  # noqa: N802
        if v is not None:
            self._ui_enable(2, v)

    @property
    def DisplayPageBreaks(self):  # noqa: N802
        """File > Options > Advanced: "Show page breaks" for this sheet: the dashed lines where pages break in Normal
        view. Printing, Print Preview and a page setup change turn them on, as in Excel; not saved with the workbook."""
        return bool(_rq("view.get", sheet=self._id)["page_breaks"])

    @DisplayPageBreaks.setter
    def DisplayPageBreaks(self, v):  # noqa: N802
        if v is None:
            return
        _rq("view.set", sheet=self._id, page_breaks=bool(v))

    @property
    def Tab(self):  # noqa: N802
        return _Tab(self)

    @property
    def StandardWidth(self):  # noqa: N802
        """The standard column width in characters (Home > Format > Default Width): every column not sized by hand."""
        return round(float(self._info().get("standard_width", 8.43)), 2)

    @StandardWidth.setter
    def StandardWidth(self, v):  # noqa: N802
        _rq("sheet.standard_width", id=self._id, chars=float(v))

    @property
    def RowLimit(self):  # noqa: N802
        return self._info()["row_limit"]

    @RowLimit.setter
    def RowLimit(self, v):  # noqa: N802
        _rq("sheet.row_limit", id=self._id, rows=int(v))

    @property
    def Store(self):  # noqa: N802
        return Store(self._id)

    @property
    def Parent(self):  # noqa: N802
        return ThisWorkbook

    @property
    def Next(self):  # noqa: N802
        sheets = _sheets_info()
        i = self.Index
        return Worksheet(sheets[i]["id"]) if i < len(sheets) else None

    @property
    def Previous(self):  # noqa: N802
        sheets = _sheets_info()
        i = self.Index
        return Worksheet(sheets[i - 2]["id"]) if i > 1 else None

    @property
    def UsedRange(self):  # noqa: N802
        u = self._info().get("used")
        if not u:
            return Range._from(self._id, [(0, 0, 0, 0)])
        return Range._from(self._id, [tuple(u)])

    @property
    def Cells(self):  # noqa: N802
        return _CellsAccessor(self._id, None)

    @property
    def Rows(self):  # noqa: N802
        return _RowsAccessor(self._id)

    @property
    def Columns(self):  # noqa: N802
        return _ColsAccessor(self._id)

    def write_frame(self, data, at="A1", header: bool = True, index: bool = False):
        """[YC] Writes a DataFrame (or dict of columns, numpy record array...) as plain cells from `at`, through shared
        memory; dates get a date format. Returns the Range written. (bind() makes a live table instead.)"""
        from . import data as _data

        return _data.write_frame(self, data, at=at, header=header, index=index)

    def Range(self, Cell1, Cell2=None):  # noqa: N802,N803
        return _make_range(self._id, Cell1, Cell2)

    @property
    def Outline(self):  # noqa: N802
        """The sheet's outline (Data -> Group): Outline.ShowLevels(RowLevels, ColumnLevels)."""
        return _Outline(self._id)

    @property
    def ListObjects(self):  # noqa: N802
        """The sheet's tables (Insert -> Table)."""
        return _ListObjects(self._id)

    @property
    def PivotTables(self):  # noqa: N802
        """PivotTables.Count, PivotTables() and PivotTables("PT1"), as Excel's macros write them."""
        return _PivotTables(self._id)

    # ---- page setup and printing ----
    @property
    def PageSetup(self):  # noqa: N802
        return _PageSetup(self._id)

    @property
    def HPageBreaks(self):  # noqa: N802
        return _PageBreaks(self._id, True)

    @property
    def VPageBreaks(self):  # noqa: N802
        return _PageBreaks(self._id, False)

    def ResetAllPageBreaks(self):  # noqa: N802
        _rq("page.breaks", sheet=self._id, rows=True, reset=True)

    def PrintOut(self, From=None, To=None, Copies=1, Preview=False, ActivePrinter=None, PrintToFile=False, Collate=True, PrToFileName=None,  # noqa: N802,N803
                 IgnorePrintAreas=False):
        _print_out([self._id], From, To, Copies, Preview, ActivePrinter, PrintToFile, Collate, PrToFileName, IgnorePrintAreas)

    def SetBackgroundPicture(self, Filename):  # noqa: N802,N803
        """Page Layout > Background: a picture tiled behind the cells (on screen only); "" or None deletes it."""
        import os

        _rq("layout.background", sheet=self._id, path=os.path.abspath(str(Filename)) if Filename else "")

    def PrintPreview(self, EnableChanges=True):  # noqa: N802,N803
        self.Activate()
        _rq("print.preview")

    def ExportAsFixedFormat(self, Type=0, Filename=None, Quality=0, IncludeDocProperties=True, IgnorePrintAreas=False, From=None, To=None,  # noqa: N802,N803
                            OpenAfterPublish=False):
        """Save the sheet as PDF with its page setup (Type=xlTypePDF); returns the file's full path."""
        return _export_pdf([self._id], Type, Filename, Quality, IncludeDocProperties, IgnorePrintAreas, From, To, OpenAfterPublish)

    def ClearArrows(self):  # noqa: N802
        """Remove the tracer arrows of formula auditing."""
        _rq("audit.clear", sheet=self._id)

    # ---- protection ----
    def Protect(self, Password=None, DrawingObjects=True, Contents=True, Scenarios=True, UserInterfaceOnly=False,  # noqa: N802,N803
                AllowFormattingCells=False, AllowFormattingColumns=False, AllowFormattingRows=False, AllowInsertingColumns=False,
                AllowInsertingRows=False, AllowInsertingHyperlinks=False, AllowDeletingColumns=False, AllowDeletingRows=False,
                AllowSorting=False, AllowFiltering=False, AllowUsingPivotTables=False):
        """Protect the sheet: locked cells (Range.Locked) can't be changed. UserInterfaceOnly=True still lets Python change it."""
        flags = {"format_cells": AllowFormattingCells, "format_columns": AllowFormattingColumns, "format_rows": AllowFormattingRows,
                 "insert_columns": AllowInsertingColumns, "insert_rows": AllowInsertingRows, "insert_hyperlinks": AllowInsertingHyperlinks,
                 "delete_columns": AllowDeletingColumns, "delete_rows": AllowDeletingRows, "sort": AllowSorting, "autofilter": AllowFiltering,
                 "pivot": AllowUsingPivotTables, "objects": not DrawingObjects, "scenarios": not Scenarios}
        p = {"sheet": self._id, "flags": {k: bool(v) for k, v in flags.items()}, "ui_only": bool(UserInterfaceOnly)}
        if Password is not None:
            p["password"] = str(Password)
        _rq("protect.sheet", **p)

    def Unprotect(self, Password=None):  # noqa: N802,N803
        _rq("protect.unsheet", sheet=self._id, password=str(Password or ""))

    @property
    def ProtectContents(self):  # noqa: N802
        return bool(_rq("protect.get", sheet=self._id)["on"])

    @property
    def ProtectDrawingObjects(self):  # noqa: N802
        p = _rq("protect.get", sheet=self._id)
        return bool(p["on"] and not p["flags"]["objects"])

    @property
    def ProtectScenarios(self):  # noqa: N802
        """True when the sheet is protected without "Edit scenarios": scenarios can't be added, changed or deleted."""
        p = _rq("protect.get", sheet=self._id)
        return bool(p["on"] and not p["flags"].get("scenarios", False))

    @property
    def EnableSelection(self):  # noqa: N802
        """xlNoRestrictions 0, xlUnlockedCells 1, xlNoSelection -4142: which cells can be selected while protected."""
        f = _rq("protect.get", sheet=self._id)["flags"]
        if not f.get("select_unlocked", True):
            return -4142
        return 0 if f.get("select_locked", True) else 1

    @EnableSelection.setter
    def EnableSelection(self, v):  # noqa: N802
        _rq("protect.selection", sheet=self._id, value=int(v))

    @property
    def ProtectionMode(self):  # noqa: N802
        """True when protected with UserInterfaceOnly=True."""
        p = _rq("protect.get", sheet=self._id)
        return bool(p["on"] and p["ui_only"])

    @property
    def Protection(self):  # noqa: N802
        return _Protection(self._id)

    # ---- Methods ----
    def _shown(self, what):
        if not self._info()["visible"]:
            raise ApplicationDefinedError(f"{what} method of Worksheet class failed: the sheet is hidden")

    def Activate(self):  # noqa: N802
        """Make the sheet the active one. A hidden (or very hidden) sheet can't be active: Excel's Activate then does
        nothing and raises nothing (Select raises)."""
        if not self._info()["visible"]:
            return
        _rq("view.set", activate_sheet=self._id)

    def Select(self, Replace=True):  # noqa: N802,N803
        """Select (show) the sheet; Replace=False adds it to the grouped sheets instead, as Ctrl+click on its tab."""
        self._shown("Select")
        if Replace:
            _rq("cells.group_set", ids=[self._id])
        else:
            _rq("cells.group_set", ids=[self._id], add=True)

    @property
    def Pictures(self):  # noqa: N802
        """Pictures.Paste(Link): a picture of the copied cells at the active cell; Link=True keeps it showing them as they change."""
        return _Pictures(self._id)

    def Delete(self):  # noqa: N802
        _rq("sheet.delete", id=self._id)

    def CircleInvalid(self):  # noqa: N802
        """Data > Data Validation > Circle Invalid Data: red circles around the cells whose values break their
        validation rule (up to 255, as Excel); a cell corrected loses its circle. Returns how many were circled [YC]."""
        return _rq("sheet.circles", id=self._id, circle=True)

    def ClearCircles(self):  # noqa: N802
        """Data > Data Validation > Clear Validation Circles."""
        _rq("sheet.circles", id=self._id, circle=False)

    def Copy(self, Before=None, After=None):  # noqa: N802,N803
        """A copy of the sheet before or after another; the copy becomes the active sheet, as in Excel (code that
        copies a sheet then works on ActiveSheet). A hidden sheet's copy is hidden too and the active sheet stays;
        a very hidden sheet can't be copied, as in Excel."""
        if self._info().get("very_hidden"):
            raise ApplicationDefinedError("Copy method of Worksheet class failed: the sheet is very hidden")
        index = Before.Index - 1 if Before is not None else (After.Index if After is not None else None)
        ws = Worksheet(_rq("sheet.copy", id=self._id, index=index)["id"])
        ws.Activate()  # nothing for a hidden copy
        return ws

    def Move(self, Before=None, After=None):  # noqa: N802,N803
        if Before is not None:
            index = Before.Index - 1
            if self.Index < Before.Index:
                index -= 1
        elif After is not None:
            index = After.Index - 1 if self.Index < After.Index else After.Index
        else:
            return
        _rq("sheet.move", id=self._id, index=index)

    def Calculate(self):  # noqa: N802
        """Calculates this sheet only (Shift+F9): with manual calculation the other sheets keep waiting."""
        _rq("calc.run", full=False, sheet=self._id)

    def Evaluate(self, Name):  # noqa: N802,N803
        return Application.Evaluate(Name, _sheet=self._id)

    def Float(self, Left=None, Top=None, Width=None, Height=None):  # noqa: N802,N803
        _rq("view.float_sheet", id=self._id, floating=True, rect=[Left, Top, Width, Height])

    def Dock(self):  # noqa: N802
        _rq("view.float_sheet", id=self._id, floating=False)

    def __getattr__(self, name):
        # controls: Me.CommandButton1
        if name.startswith("_"):
            raise AttributeError(name)
        from . import ui

        ctrl = ui.find_control(self._id, name)
        if ctrl is not None:
            return ctrl
        # functions defined in the sheet module: Sheet1.MyFunc()
        from . import host

        fn = host.sheet_module_attr(self, name)
        if fn is not None:
            return fn
        raise AttributeError(f"The sheet has no member {name}")

    @property
    def QueryTables(self):  # noqa: N802
        """Data > From Text (Legacy): QueryTables.Add("TEXT;path", Destination) imports a text file at a cell."""
        return _QueryTables(self._id)

    @property
    def Scenarios(self):  # noqa: N802
        """The sheet's scenarios (Scenario Manager): Scenarios.Add(...), Scenarios() and Scenarios(Index), as Excel's
        macros write them (Scenarios.Add failed: it was only a method)."""
        return _Scenarios(self._id)

    @property
    def OLEObjects(self):  # noqa: N802
        from . import ui

        return ui.OLEObjects(self._id)

    @property
    def Shapes(self):  # noqa: N802
        from . import ui

        return ui.Shapes(self._id)

    def _forms(kind):  # noqa: N805
        def f(self, Index=None):  # noqa: N803
            from . import ui

            c = ui.FormControls(self._id, kind)
            return c if Index is None else c.Item(Index)

        return f

    Buttons = _forms("button")
    CheckBoxes = _forms("checkbox")
    DropDowns = _forms("combobox")
    ListBoxes = _forms("listbox")
    OptionButtons = _forms("optionbutton")
    Spinners = _forms("spinbutton")
    ScrollBars = _forms("scrollbar")
    GroupBoxes = _forms("groupbox")
    Labels = _forms("label")
    del _forms

    @property
    def ChartObjects(self):  # noqa: N802
        from . import ui

        return ui.ChartObjects(self._id)

    @property
    def Hyperlinks(self):  # noqa: N802
        return _Hyperlinks(self._id)

    @property
    def Comments(self):  # noqa: N802
        """All notes on the sheet (a list of Comment objects; threaded comments are in CommentsThreaded)."""
        return _Comments(_Note(self._id, x["row"], x["col"], x) for x in _rq("note.list", sheet=self._id) if not x.get("threaded"))

    @property
    def CommentsThreaded(self):  # noqa: N802
        """The threaded comments on the sheet."""
        return _CommentsThreaded(self._id)

    def ExportImage(self, Filename=None, Headings=False, Gridlines=None, Scale=1.0):  # noqa: N802,N803
        """[YC] Home > Screenshot > Whole Sheet: every cell, chart and picture of the sheet as a PNG (or to the
        Clipboard when Filename is None); see Range.ExportImage."""
        return _export_image("sheet", Filename, Headings, Gridlines, Scale, sheet=self._id)

    def Paste(self, Destination=None, Link=False):  # noqa: N802,N803
        """Paste the Clipboard at Destination (a Range of this sheet; default: the selection, which a copied cell or
        block fills as in Excel). Link=True pastes formulas that refer to the copied cells (Paste Link). Copied shapes
        (Shape.Copy) are pasted as new shapes."""
        if Destination is None and not Link and _rq("shape.paste", sheet=self._id):
            return  # the copied shapes were pasted
        if Destination is None:
            v = _rq("view.get")
            rect = [0, 0, 0, 0]
            if v["focus_sheet"] == self._id:
                ar, ac = v["active"]
                rect = [ar, ac, ar, ac]
                for a in v["areas"]:  # the area of the active cell
                    if a[0] <= ar <= a[2] and a[1] <= ac <= a[3]:
                        rect = list(a)
                        break
            r, c = rect[0], rect[1]
        else:
            r, c = Destination._a[0], Destination._a[1]
            rect = list(Destination._a)
        _rq("ui.paste", sheet=self._id, row=r, col=c, rect=rect, mode="all", transpose=False, skip_blanks=False, link=bool(Link))

    # ---- AutoFilter ----
    @property
    def AutoFilterMode(self):  # noqa: N802
        # a table's filter buttons aren't the sheet's AutoFilter (Excel 16, measured 2026-11-20: AutoFilterMode is False
        # with only a table's buttons on the sheet); setting False leaves them
        af = _rq("af.get", sheet=self._id)
        return bool(af and af["on"] and not af.get("table_owned"))

    @AutoFilterMode.setter
    def AutoFilterMode(self, v):  # noqa: N802
        if not v and self.AutoFilterMode:
            _rq("af.set", sheet=self._id, rect=None)

    @property
    def AutoFilter(self):  # noqa: N802
        from . import rules

        return rules.AutoFilter(self._id) if self.AutoFilterMode else None

    @property
    def FilterMode(self):  # noqa: N802
        af = _rq("af.get", sheet=self._id)
        return bool(af and af["on"] and af["hidden_rows"] > 0)

    def ShowAllData(self):  # noqa: N802
        _rq("af.show_all", sheet=self._id)


# ---------------------------------------------------------------------------
# Range
# ---------------------------------------------------------------------------
def _split_top(text: str, sep: str):
    """Split at sep (not inside single-quoted sheet names or parentheses). sep=None: split at whitespace."""
    parts, cur, quote, depth = [], "", False, 0
    for ch in text:
        if ch == "'":
            quote = not quote
        elif not quote and ch == "(":
            depth += 1
        elif not quote and ch == ")":
            depth -= 1
        hit = (ch.isspace() if sep is None else ch == sep) and not quote and depth == 0
        if hit:
            if cur or sep is not None:
                parts.append(cur)
            cur = ""
            continue
        cur += ch
    if cur or sep is not None:
        parts.append(cur)
    return parts


def _resolve_ref(sheet_id, part: str):
    """One address or name -> (sheet id, (r0, c0, r1, c1)). Names may be sheet-level, "Sheet!Name" or dynamic."""
    sheet_name, rest = _split_sheet(part)
    if sheet_name is not None:
        sheet_id = Worksheets(sheet_name)._id
    rest = rest.strip()
    if rest.endswith("#") and len(rest) > 1:
        # A1#: the spill range of the dynamic array anchored there (Excel: Range("C1#") is $C$1:$C$3)
        r0, c0, r1, c1 = _parse_area(rest[:-1])
        s = _rq("range.spill", sheet=sheet_id, row=r0, col=c0)
        if not s or s.get("cse") or tuple(s["anchor"]) != (r0, c0) or (r0, c0) != (r1, c1):
            raise ApplicationDefinedError(f"Invalid address or name: {part.strip()} (no dynamic array spills from there)")
        return sheet_id, tuple(s["rect"])
    try:
        return sheet_id, _parse_area(rest)
    except ApplicationDefinedError:
        n = _rq("names.resolve", name=rest, sheet=sheet_id)
        if not n:
            raise ApplicationDefinedError(f"Invalid address or name: {part.strip()}") from None
        return n["sheet"], tuple(n["rect"])


def _intersect_areas(a, b):
    r0, c0, r1, c1 = max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])
    return (r0, c0, r1, c1) if r0 <= r1 and c0 <= c1 else None


def _rows_fit(sheet_id, last_row: int) -> bool:
    """Whether a 0-based row is on the sheet: Excel's 1,048,576 rows, or more on a sheet whose row limit was raised
    (Offset, Resize and Cells past the last row raise error 1004, as in Excel, instead of making a range off the sheet)."""
    return last_row < 1048576 or last_row < Worksheet(sheet_id).RowLimit


def _make_range(sheet_id, Cell1, Cell2=None, base=None):
    """Create a Range by Excel's rules. base: relative to the upper-left corner of another Range (rng.Range("A1")).

    Address text supports: A1, A1:B2, A:C, 2:5, Sheet!A1, unions "A1,C3:D4", intersections "A1:C5 B2:D3",
    defined names (workbook or sheet level, "Sheet!Name", dynamic names such as OFFSET).
    """
    if isinstance(Cell1, Range):
        if Cell2 is None:
            return Cell1
        a, b = Cell1._areas[0], Cell2._areas[-1]
        return Range._from(Cell1._sheet, [(min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))])
    if not isinstance(Cell1, str):
        raise TypeMismatchError("Range arguments must be address strings or Ranges")
    text = _re.sub(r"\s*:\s*", ":", Cell1.strip())
    if text.startswith("="):
        text = text[1:]
    areas = []
    sheet = None
    for part in _split_top(text, ","):
        pieces = [x for x in _split_top(part, None) if x]
        if not pieces:
            raise ApplicationDefinedError(f"Invalid address: {Cell1}")
        sid, area = _resolve_ref(sheet_id, pieces[0])
        for extra in pieces[1:]:  # intersection operator (space)
            sid2, area2 = _resolve_ref(sid, extra)
            area = _intersect_areas(area, area2) if sid2 == sid else None
            if area is None:
                raise ApplicationDefinedError(f"The ranges don't intersect: {part.strip()}")
        if sheet is None:
            sheet = sid
        elif sid != sheet:
            raise ApplicationDefinedError("Multiple ranges must be on the same sheet")
        areas.append(area)
    sheet_id = sheet
    for a in areas:  # a row past the sheet's last one isn't an address (Excel: Range("A1048577") is error 1004)
        if a[2] != ROW_END and not _rows_fit(sheet_id, a[2]):
            raise ApplicationDefinedError(f"Invalid address: {Cell1} (past the sheet's last row)")
    if Cell2 is not None:
        b = _make_range(sheet_id, Cell2)._areas[0]
        a = areas[0]
        areas = [(min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))]
    if base is not None:
        r0, c0 = base[0], base[1]
        areas = [(a[0] + r0, a[1] + c0, a[2] + r0 if a[2] != ROW_END else ROW_END, a[3] + c0) for a in areas]
    return Range._from(sheet_id, areas)


_last_find = None  # search settings of Range.Find (reused by FindNext / FindPrevious)


class Range:
    """A range of cells (may have several areas). Coordinates are 0-based internally; public properties are 1-based like Excel."""

    # _by: "cols" / "rows" for Worksheet.Columns(...) / Rows(...); _whole: "row" / "col" for EntireRow / EntireColumn,
    # which tell whole rows from whole columns when they cover the whole sheet (Cells.EntireRow.Hidden = False)
    __slots__ = ("_sheet", "_areas", "_cx", "_by", "_whole")

    def __init__(self, Cell1, Cell2=None):  # noqa: N803
        # global Range("A1"): the active sheet (Me.Range replaces it in sheet modules)
        sheet = _active_sheet_id()
        r = _make_range(sheet, Cell1, Cell2)
        object.__setattr__(self, "_sheet", r._sheet)
        object.__setattr__(self, "_areas", r._areas)
        object.__setattr__(self, "_cx", getattr(_conn_tls, "conn", None))

    @classmethod
    def _from(cls, sheet_id: int, areas) -> "Range":
        r = object.__new__(cls)
        object.__setattr__(r, "_sheet", sheet_id)
        object.__setattr__(r, "_areas", [tuple(a) for a in areas])
        # made while a method of another workbook's object runs: it belongs to that workbook too
        object.__setattr__(r, "_cx", getattr(_conn_tls, "conn", None))
        return r

    def _p(self, **kw):
        return dict(sheet=self._sheet, areas=[list(a) for a in self._areas], **kw)

    @property
    def _a(self):
        return self._areas[0]

    def __repr__(self):
        try:
            return f"<Range {self.Worksheet.Name}!{self.Address(False, False)}>"
        except YcError:
            return f"<Range {self._areas}>"

    def __eq__(self, other):
        return isinstance(other, Range) and other._sheet == self._sheet and other._areas == self._areas

    def __hash__(self):
        return hash((self._sheet, tuple(self._areas)))

    def __setattr__(self, name, value):
        # only defined properties can be set (so a typo never silently succeeds, like a compile error in Excel)
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"Range has no settable property {name}")

    # ---- Getting ranges ----
    def Range(self, Cell1, Cell2=None):  # noqa: N802,N803
        return _make_range(self._sheet, Cell1, Cell2, base=self._a)

    def Item(self, RowIndex=None, ColumnIndex=None):  # noqa: N802,N803
        return _CellsAccessor(self._sheet, self._a)(RowIndex, ColumnIndex) if RowIndex is not None else self

    __call__ = Item  # Range("B2:D4")(2, 2): the default member, as in Excel

    @property
    def Cells(self):  # noqa: N802
        """The range's cells, as Excel's Range.Cells: Cells(2, 3) and Cells(5) count from the upper-left cell,
        Cells.Count is the number of cells (Rows(2).Cells.Count too), Cells.Item(3), For Each over the cells, and
        every other Range member (Cells.Value, Cells.Font.Bold = True)."""
        return _RangeCells(self)

    def Offset(self, RowOffset=0, ColumnOffset=0):  # noqa: N802,N803
        areas = [(a[0] + RowOffset, a[1] + ColumnOffset, (a[2] + RowOffset) if a[2] != ROW_END else ROW_END, a[3] + ColumnOffset)
                 for a in self._areas]
        # whole columns moved down end past the sheet (Excel: Range("C:D").Offset(4, 2) is an error, measured)
        if RowOffset and any(a[2] == ROW_END for a in self._areas):
            raise ApplicationDefinedError("Offset moves the range outside the sheet")
        if any(x[0] < 0 or x[1] < 0 or x[3] >= MAX_COLS or (x[2] != ROW_END and not _rows_fit(self._sheet, x[2])) for x in areas):
            raise ApplicationDefinedError("Offset moves the range outside the sheet")
        return Range._from(self._sheet, areas)

    def Resize(self, RowSize=None, ColumnSize=None):  # noqa: N802,N803
        _one_area(self)
        a = self._a
        rows = int(RowSize) if RowSize is not None else self._nrows(a)
        cols = int(ColumnSize) if ColumnSize is not None else a[3] - a[1] + 1
        if rows < 1 or cols < 1:
            raise ApplicationDefinedError("Resize needs at least one row and one column")
        if a[1] + cols > MAX_COLS or not _rows_fit(self._sheet, a[0] + rows - 1):
            raise ApplicationDefinedError("Resize makes the range larger than the sheet")
        return Range._from(self._sheet, [(a[0], a[1], a[0] + rows - 1, a[1] + cols - 1)])

    def End(self, Direction):  # noqa: N802,N803
        a = self._a
        r, c = _rq("range.end", sheet=self._sheet, row=a[0], col=a[1], dir=int(Direction))
        return Range._from(self._sheet, [(r, c, r, c)])

    # ---- Conditional formatting, data validation, AutoFilter ----
    @property
    def FormatConditions(self):  # noqa: N802
        from . import rules

        return rules.FormatConditions(self)

    @property
    def Validation(self):  # noqa: N802
        from . import rules

        return rules.Validation(self)

    def AutoFilter(self, Field=None, Criteria1=None, Operator=1, Criteria2=None, VisibleDropDown=True):  # noqa: N802,N803
        from . import rules

        _one_area(self)
        return rules.range_autofilter(self, Field, Criteria1, Operator, Criteria2, VisibleDropDown)

    @property
    def CurrentRegion(self):  # noqa: N802
        return Range._from(self._sheet, [tuple(_rq("range.current_region", sheet=self._sheet, row=self._a[0], col=self._a[1]))])

    @property
    def EntireRow(self):  # noqa: N802
        rng = Range._from(self._sheet, [(a[0], 0, a[2], MAX_COLS - 1) for a in self._areas])
        object.__setattr__(rng, "_whole", "row")
        return rng

    @property
    def EntireColumn(self):  # noqa: N802
        rng = Range._from(self._sheet, [(0, a[1], ROW_END, a[3]) for a in self._areas])
        object.__setattr__(rng, "_whole", "col")
        return rng

    @property
    def Rows(self):  # noqa: N802
        return _RangeRows(self)

    @property
    def Columns(self):  # noqa: N802
        return _RangeCols(self)

    @property
    def Areas(self):  # noqa: N802
        return _Areas(self)

    @property
    def MergeArea(self):  # noqa: N802
        a = self._a
        m = _rq("range.merge_area", sheet=self._sheet, row=a[0], col=a[1])
        return Range._from(self._sheet, [tuple(m)])

    def _spill(self):
        a = self._a
        return _rq("range.spill", sheet=self._sheet, row=a[0], col=a[1])

    # ---- Data tools ----
    def to_frame(self, header: bool = True, pandas: bool = True):
        """[YC] The range as a pandas DataFrame read through shared memory (first row = column names when header);
        dates as datetime64, whole-number columns as int64. pandas=False returns a dict of columns."""
        from . import data

        return data.read_range(self, header=header, pandas=pandas)

    def RemoveDuplicates(self, Columns=None, Header=xlNo):  # noqa: N802,N803
        """Delete the rows that repeat earlier rows in the given columns (1-based within the range; default all).
        Header: xlYes (the first row is a header), xlNo or xlGuess (a first row of text is a header)."""
        _one_area(self)
        a = self._a
        if Columns is None:
            cols = []
        elif isinstance(Columns, (list, tuple)):
            cols = [int(x) for x in Columns]
        else:
            cols = [int(Columns)]
        header = Header == xlYes or (Header == xlGuess and _guess_header(self._sheet, a))
        return _rq("range.remove_duplicates", sheet=self._sheet, rect=list(a), cols=cols, header=header)

    def TextToColumns(self, Destination=None, DataType=xlDelimited, TextQualifier=xlTextQualifierDoubleQuote,  # noqa: N802,N803
                      ConsecutiveDelimiter=False, Tab=False, Semicolon=False, Comma=False, Space=False, Other=False, OtherChar=None,
                      FieldInfo=None, DecimalSeparator=None, ThousandsSeparator=None, TrailingMinusNumbers=None):
        """Split the text of the first column into columns (like Data -> Text to Columns).
        FieldInfo: delimited [[column, xlGeneralFormat / xlTextFormat / xlMDYFormat ... xlYDMFormat / xlSkipColumn], ...];
        fixed width [[start position, format], ...] (positions in characters, the first is 0).
        DecimalSeparator / ThousandsSeparator: how General columns read numbers ("." and "," by default);
        TrailingMinusNumbers (default True): "123-" is -123."""
        _one_area(self)
        a = self._a
        dest = Destination if isinstance(Destination, Range) else (Range(Destination) if isinstance(Destination, str) else None)
        dr, dc = (dest._a[0], dest._a[1]) if dest is not None else (a[0], a[1])
        qual = {xlTextQualifierDoubleQuote: '"', xlTextQualifierSingleQuote: "'"}.get(TextQualifier, "")
        breaks, types = [], []
        if FieldInfo:
            items = sorted([list(x) for x in FieldInfo], key=lambda x: x[0])
            if DataType == xlFixedWidth:
                breaks = [int(x[0]) for x in items if int(x[0]) > 0]
                types = [int(x[1]) for x in items]
            else:
                width = max(int(x[0]) for x in items)
                types = [xlGeneralFormat] * width
                for col, fmt in items:
                    types[int(col) - 1] = int(fmt)
        return _rq("range.text_to_columns", sheet=self._sheet, rect=list(a), dest_row=dr, dest_col=dc, fixed=DataType == xlFixedWidth,
                   tab=bool(Tab), semicolon=bool(Semicolon), comma=bool(Comma), space=bool(Space),
                   other=(OtherChar or "")[:1] if Other else "", consecutive=bool(ConsecutiveDelimiter), qualifier=qual,
                   breaks=breaks, types=types, decimal=(DecimalSeparator or "")[:1], thousands=(ThousandsSeparator or "")[:1],
                   trailing_minus=True if TrailingMinusNumbers is None else bool(TrailingMinusNumbers))

    @property
    def DisplayFormat(self):  # noqa: N802
        """The range's first cell as the sheet shows it, its conditional formats applied (read-only): Font (Color,
        Bold, Italic, Underline, Strikethrough...), Interior (Color, Pattern), NumberFormat and the alignment. A macro
        reads which cells a rule colored with it: Range("B2").DisplayFormat.Interior.Color."""
        return _DisplayFormat(self)

    # ---- PivotTable reports ----
    def _pivot_info(self):
        a = self._a
        return _rq("pivot.cell_info", sheet=self._sheet, row=a[0], col=a[1])

    @property
    def PivotCell(self):  # noqa: N802
        """What the range's first cell is in a PivotTable report (PivotCellType, PivotField, PivotItem, RowItems,
        ColumnItems, DataField); an error outside every report, as in Excel."""
        info = self._pivot_info()
        if not info:
            raise ApplicationDefinedError("The cell isn't in a PivotTable report")
        return _PivotCell(self, info)

    @property
    def PivotField(self):  # noqa: N802
        """The PivotField of the range's first cell (an item's field, a value's data field)."""
        return self.PivotCell.PivotField

    @property
    def PivotItem(self):  # noqa: N802
        """The PivotItem whose label is the range's first cell."""
        return self.PivotCell.PivotItem

    # ---- Outline ----
    def _outline_dim(self):
        # Excel 16 (measured 2026-11-20): whole rows group their rows and whole columns their columns, whichever lines
        # were asked for (Range("3:5").Columns.Group groups rows 3:5, Range("B:B").Rows.Group column B); the whole
        # sheet groups the lines asked for, its columns when none were; other ranges their rows unless columns were
        a = self._a
        mark = getattr(self, "_by", None) or getattr(self, "_whole", None)
        wide = a[1] == 0 and a[3] == MAX_COLS - 1
        tall = a[0] == 0 and a[2] >= 1048575  # ROW_END too (YCGrid cuts the lines to the sheet)
        if wide and tall:
            return (True, 0, a[2]) if mark in ("rows", "row") else (False, 0, MAX_COLS - 1)
        if wide:
            return True, a[0], a[2]
        if tall or mark in ("cols", "col"):
            return False, a[1], a[3]
        return True, a[0], a[2]

    def Group(self, Start=None, End=None, By=None, Periods=None):  # noqa: N802,N803
        """Group entire rows (or entire columns) so they can be collapsed; a range of cells groups its rows.
        Labels of a PivotTable report: the selected items become a group of a new field ("Region2"), as Group Selection;
        with Start, End, By or Periods, the label's field is grouped by them (PivotField.Group), as recorded macros
        write pf.DataRange.Cells(1).Group(Start:=True, End:=True, Periods:=...)."""
        _one_area(self)
        a = self._a
        if Start is not None or End is not None or By is not None or Periods is not None:
            f = _rq("pivot.field_at", sheet=self._sheet, row=a[0], col=a[1])
            if f:
                PivotTable(self._sheet, f["name"]).PivotFields(f["field"]).Group(True if Start is None else Start, True if End is None else End,
                                                                                By, Periods)
                return True
        if _rq("pivot.group", sheet=self._sheet, rect=list(a)) == 1:
            return True
        rows, lo, hi = self._outline_dim()
        _rq("outline.group", sheet=self._sheet, rows=rows, a=lo, b=hi, group=True)
        return True

    def Ungroup(self):  # noqa: N802
        _one_area(self)
        rows, lo, hi = self._outline_dim()
        _rq("outline.group", sheet=self._sheet, rows=rows, a=lo, b=hi, group=False)
        return True

    @property
    def ShowDetail(self):  # noqa: N802
        """Whether the group this summary row (column) sums up is expanded. The summary row is the one below its group
        (above with Outline.SummaryRow = xlSummaryAbove), the summary column right of it (left with xlSummaryOnLeft);
        any other row or column reads True, as in Excel."""
        rows, lo, hi = self._outline_dim()
        st = _rq("outline.settings", sheet=self._sheet) or {}
        nb = lo - 1 if st.get("below" if rows else "right", True) else lo + 1
        if nb < 0:
            return True
        if _rq("outline.level", sheet=self._sheet, rows=rows, index=nb) <= _rq("outline.level", sheet=self._sheet, rows=rows, index=lo):
            return True
        detail = Range._from(self._sheet, [(nb, 0, nb, 0) if rows else (0, nb, 0, nb)])
        return not (detail.EntireRow.Hidden if rows else detail.EntireColumn.Hidden)

    @ShowDetail.setter
    def ShowDetail(self, v):  # noqa: N802
        """Collapse (False) or expand (True) the group this summary row (column) sums up, or the group a row inside it
        belongs to; a row in no group raises, as in Excel."""
        a = self._a
        if v and a[0] == a[2] and a[1] == a[3]:
            # a value of a PivotTable report: its records on a new sheet (Show Details)
            if _rq("pivot.details", sheet=self._sheet, row=a[0], col=a[1]) != -1:
                return
        rows, lo, hi = self._outline_dim()
        if not _rq("outline.detail", sheet=self._sheet, rows=rows, index=lo, show=bool(v)):
            raise ApplicationDefinedError("Unable to set the ShowDetail property of the Range class: the row or column is in no group")

    @property
    def OutlineLevel(self):  # noqa: N802
        """Outline level of the first row (1 = not grouped)."""
        rows, lo, hi = self._outline_dim()
        return _rq("outline.level", sheet=self._sheet, rows=rows, index=lo)

    @OutlineLevel.setter
    def OutlineLevel(self, v):  # noqa: N802
        """1 (no group) to 8, Excel's deepest outline level; other levels raise."""
        if v is None:
            return
        if not 1 <= int(v) <= 8:
            raise ApplicationDefinedError("Unable to set the OutlineLevel property of the Range class: the level goes from 1 to 8")
        rows, lo, hi = self._outline_dim()
        _rq("outline.level", sheet=self._sheet, rows=rows, index=lo, b=hi, set=int(v))

    def AutoOutline(self):  # noqa: N802
        """Data > Group > Auto Outline: group the rows (columns) that SUM / SUBTOTAL formulas in the range add up.
        A single cell works on the whole sheet's data. Returns the number of groups made; with nothing to group it
        raises "Cannot create an outline.", as Excel's does (measured)."""
        a = self._a
        n = _rq("outline.auto", sheet=self._sheet, rect=list(a))
        if not n:
            raise ApplicationDefinedError("Cannot create an outline.")
        return n

    def ClearOutline(self):  # noqa: N802
        """Remove the row and column groups of the range's rows and columns; one cell clears the whole sheet's outline,
        as Data > Ungroup > Clear Outline does in Excel. Collapsed rows and columns are shown again (Excel 16, measured;
        Ungroup and OutlineLevel = 1 keep them hidden)."""
        a = self._a
        if a[0] == a[2] and a[1] == a[3]:
            a = (0, 0, ROW_END, MAX_COLS - 1)
        r1 = a[2] if a[2] != ROW_END else self.Worksheet.RowLimit - 1
        _rq("outline.level", sheet=self._sheet, rows=True, index=a[0], b=r1, set=1, clear=True)
        _rq("outline.level", sheet=self._sheet, rows=False, index=a[1], b=a[3], set=1, clear=True)

    @property
    def ListObject(self):  # noqa: N802
        """The table (ListObject) the range's first cell is in, or None."""
        r, c = self._a[0], self._a[1]
        for t in _rq("table.list", sheet=self._sheet):
            g = t["range"]
            if g[0] <= r <= g[2] and g[1] <= c <= g[3]:
                return ListObject(self._sheet, t["id"])
        return None

    @property
    def PivotTable(self):  # noqa: N802
        """The PivotTable whose report covers the range's first cell (error when none)."""
        a = self._a
        for p in _rq("pivot.list", sheet=self._sheet):
            o = p["output"] or [p["r0"], p["c0"], p["r0"] + 17, p["c0"] + 2]
            if o[0] <= a[0] <= o[2] and o[1] <= a[1] <= o[3]:
                return PivotTable(self._sheet, p["name"])
        raise ApplicationDefinedError("The cell is not in a PivotTable")

    def PrintOut(self, From=None, To=None, Copies=1, Preview=False, ActivePrinter=None, PrintToFile=False, Collate=True, PrToFileName=None):  # noqa: N802,N803
        """Print only this range, with its sheet's page setup."""
        _print_out([self._sheet], From, To, Copies, Preview, ActivePrinter, PrintToFile, Collate, PrToFileName, area=self._a)

    def ExportAsFixedFormat(self, Type=0, Filename=None, Quality=0, IncludeDocProperties=True, IgnorePrintAreas=False, From=None, To=None,  # noqa: N802,N803
                            OpenAfterPublish=False):
        """Save only this range as PDF."""
        return _export_pdf([self._sheet], Type, Filename, Quality, IncludeDocProperties, IgnorePrintAreas, From, To, OpenAfterPublish, area=self._a)

    @property
    def SparklineGroups(self):  # noqa: N802
        """The sparklines in this range (Insert -> Sparklines)."""
        return SparklineGroups(self)

    # ---- Formula auditing ----
    @property
    def Errors(self):  # noqa: N802
        """Background error checking of the (first) cell: Errors(xlNumberAsText).Value is True when the cell breaks
        that rule (the green triangle), .Ignore = True is the error button's Ignore Error (saved with the workbook)."""
        return _CellErrors(self)

    def ShowPrecedents(self, Remove=False):  # noqa: N802,N803
        """Draw tracer arrows from the cells this formula uses (each call one level further; Remove=True removes them)."""
        a = self._a
        _rq("audit.trace", sheet=self._sheet, row=a[0], col=a[1], precedents=True, remove=bool(Remove))

    def ApplyNames(self, Names=None, IgnoreRelativeAbsolute=True, UseRowColumnNames=True, OmitColumn=True, OmitRow=True,  # noqa: N802,N803
                   Order=1, AppendLast=False):
        """Formulas -> Define Name -> Apply Names: the references in this range's formulas that are exactly one of the
        names become that name (Names: a name or a list of names; None: every name). A single cell applies them to the
        whole sheet, as in Excel. IgnoreRelativeAbsolute=False replaces only absolute references. UseRowColumnNames: a
        cell where a row-range name crosses a column-range name becomes "RowName ColumnName" (Order xlRowThenColumn /
        xlColumnThenRow); OmitColumn / OmitRow leave out the name of the formula's own column / row. AppendLast is
        accepted as in Excel. Returns the number of formulas changed."""
        if Names is None:
            names = [n.Name.split("!")[-1] for n in Application.ActiveWorkbook.Names]
        elif isinstance(Names, str):
            names = [Names]
        else:
            names = [str(n) for n in Names]
        rects = [list(a) for a in self._areas]
        return int(_rq("names.apply", sheet=self._sheet, rects=rects, names=names, ignore_relative=bool(IgnoreRelativeAbsolute),
                       use_row_column=bool(UseRowColumnNames), omit_column=bool(OmitColumn), omit_row=bool(OmitRow), order=int(Order)))

    def ShowErrors(self):  # noqa: N802
        """Formulas -> Error Checking -> Trace Error: arrows back from this cell's error value to the cell where it starts
        (red), and from that cell's precedents (blue)."""
        a = self._a
        _rq("audit.trace", sheet=self._sheet, row=a[0], col=a[1], errors=True)

    def ShowDependents(self, Remove=False):  # noqa: N802,N803
        """Draw tracer arrows to the formulas that use this cell (each call one level further; Remove=True removes them)."""
        a = self._a
        _rq("audit.trace", sheet=self._sheet, row=a[0], col=a[1], precedents=False, remove=bool(Remove))

    @property
    def HasArray(self):  # noqa: N802
        """True when the (first) cell is part of an array formula (Ctrl+Shift+Enter)."""
        s = self._spill()
        return bool(s) and bool(s["cse"])

    @property
    def CurrentArray(self):  # noqa: N802
        """The whole range of the array formula the (first) cell belongs to, otherwise None."""
        s = self._spill()
        if not s or not s["cse"]:
            return None
        return Range._from(self._sheet, [tuple(s["rect"])])

    @property
    def FormulaArray(self):  # noqa: N802
        """The array formula of the range (Ctrl+Shift+Enter); a single cell's formula or value otherwise; None (Excel's
        Null) for several cells that aren't one array formula."""
        s = self._spill()
        if s and s["cse"]:
            r, c = s["anchor"]
            return Range._from(self._sheet, [(r, c, r, c)]).Formula
        r0, c0, r1, c1 = self._a
        if (r0, c0) != (r1, c1):
            return None
        f = self.Formula
        return f[0][0] if isinstance(f, VArray) else f

    @FormulaArray.setter
    def FormulaArray(self, v):  # noqa: N802
        if v is None:  # Null: nothing to enter
            return
        if not (isinstance(v, str) and v.startswith("=")):
            self.Formula = v  # a constant goes into every cell, as Excel enters it (no array formula)
            return
        _rq("range.set_array", sheet=self._sheet, rect=list(self._a), formula=str(v))

    @property
    def HasSpill(self):  # noqa: N802
        """True when the (first) cell is part of a dynamic array's spill range."""
        s = self._spill()
        return bool(s) and not s["cse"]

    @property
    def SpillingToRange(self):  # noqa: N802
        """The whole spill range when this is the anchor cell of a dynamic array, otherwise None."""
        s = self._spill()
        if not s or s["cse"] or tuple(s["anchor"]) != (self._a[0], self._a[1]):
            return None
        return Range._from(self._sheet, [tuple(s["rect"])])

    @property
    def SpillParent(self):  # noqa: N802
        """The anchor cell of the dynamic array this cell belongs to, otherwise None."""
        s = self._spill()
        if not s or s["cse"]:
            return None
        r, c = s["anchor"]
        return Range._from(self._sheet, [(r, c, r, c)])

    @property
    def Worksheet(self):  # noqa: N802
        return Worksheet(self._sheet)

    Parent = Worksheet

    def Find(self, What, After=None, LookIn=None, LookAt=None, SearchOrder=None, SearchDirection=xlNext, MatchCase=False,  # noqa: N802,N803
             MatchByte=None, SearchFormat=None):
        """Like Excel: searches from the cell after After (by default after the range's upper-left cell, which is
        then searched last), wrapping around; returns None when nothing is found.
        What="" finds the next empty cell (LookIn=xlFormulas: no formula or constant; xlValues: shows nothing).
        SearchFormat=True: only cells with Application.FindFormat (What="" finds any cell with it).
        LookIn, LookAt and SearchOrder left out take the last Find's (first xlValues, xlPart, xlByRows), as Excel
        saves them; MatchCase is not saved. The settings are remembered for FindNext / FindPrevious."""
        global _last_find
        prev = _last_find or {}
        order = SearchOrder if SearchOrder is not None else prev.get("order", xlByRows)
        look_at = LookAt if LookAt is not None else prev.get("look_at", 2)
        look_in = LookIn if LookIn is not None else prev.get("look_in", xlValues)
        opts = dict(what=str(What), fmt=_find_format._spec() if SearchFormat else None, by_rows=order == xlByRows, match_case=bool(MatchCase),
                    whole=look_at == xlWhole, formulas=look_in == xlFormulas)
        _last_find = dict(what=str(What), order=order, match_case=opts["match_case"], look_at=look_at, look_in=look_in, range=self, fmt=opts["fmt"])
        return self._find(opts, After, SearchDirection == xlPrevious)

    def _one_cell(self):
        """One cell, or one merged cell whole (Excel 16 takes B3:C4 merged as one cell: its Find, Replace and
        SpecialCells work on the whole sheet, measured 2026-11-20)."""
        a = self._areas
        if len(a) != 1:
            return False
        a = a[0]
        if a[0] == a[2] and a[1] == a[3]:
            return True
        if a[2] == ROW_END or (a[2] - a[0] + 1) * (a[3] - a[1] + 1) > 1000000:
            return False
        return tuple(_rq("range.merge_area", sheet=self._sheet, row=a[0], col=a[1])) == tuple(a)

    def _whole_if_cell(self):
        """One cell searches the whole sheet, as Excel 16 does (measured 2026-11-20: Range("A1").Find("MSFT") finds A3,
        Range("C3").Replace changes every cell); only the cell itself was searched."""
        if self._one_cell():
            return Range._from(self._sheet, [(0, 0, ROW_END, MAX_COLS - 1)])
        return self

    def _find(self, opts, After, previous):
        # Excel starts after (or, searching backwards, before) the upper-left cell when After is left out
        start = After._a[:2] if After is not None else self._a[:2]
        where = self._whole_if_cell()
        if opts["what"] == "" and not opts.get("fmt"):
            return where._find_empty(start, previous, opts["by_rows"], opts["formulas"])
        r = _rq("range.find", **where._p(start=[start[0], start[1]], previous=previous, **opts))
        if r is None:
            return None
        return Range._from(self._sheet, [(r[0], r[1], r[0], r[1])])

    def _find_empty(self, start, previous, by_rows, formulas):
        """Find(""): the next empty cell of the (first) area in search order after start, wrapping around. Only the
        part inside the used range is read: every cell past it is empty."""
        a = self._a
        r1 = a[2] if a[2] != ROW_END else self.Worksheet.RowLimit - 1
        u = self.Worksheet.UsedRange._a
        ir0, ic0, ir1, ic1 = max(a[0], u[0]), max(a[1], u[1]), min(r1, u[2]), min(a[3], u[3])
        grid = None
        if ir0 <= ir1 and ic0 <= ic1:
            grid = _rq("range.get", sheet=self._sheet, rect=[ir0, ic0, ir1, ic1], what="formula" if formulas else "value2")["values"]

        def empty(r, c):
            if grid is None or not (ir0 <= r <= ir1 and ic0 <= c <= ic1):
                return True
            x = grid[r - ir0][c - ic0]
            return x is None or x == ""

        # lines (rows by rows, columns by columns) and the cells along them: the rest of the start's line, the lines after
        # it, then from the first line round to the start (backwards for xlPrevious). A line or cell outside the used
        # range is empty, so the walk stops there: it reads at most the used range, never the million rows of a column.
        lo, hi = ((a[0], r1), (a[1], a[3])) if by_rows else ((a[1], a[3]), (a[0], r1))
        s_line, s_cell = (start[0], start[1]) if by_rows else (start[1], start[0])
        s_line, s_cell = min(max(s_line, lo[0]), lo[1]), min(max(s_cell, hi[0]), hi[1])  # an After outside the range
        step = -1 if previous else 1
        first, last = (hi[1], hi[0]) if previous else (hi[0], hi[1])

        def lines():
            yield s_line, range(s_cell + step, last + step, step)
            line = s_line + step
            while lo[0] <= line <= lo[1]:
                yield line, range(first, last + step, step)
                line += step
            line = lo[1] if previous else lo[0]
            while line != s_line:
                yield line, range(first, last + step, step)
                line += step
            yield s_line, range(first, s_cell + step, step)

        for line, cells in lines():
            for k in cells:
                r, c = (line, k) if by_rows else (k, line)
                if empty(r, c):
                    return Range._from(self._sheet, [(r, c, r, c)])
        return None

    def _find_again(self, After, previous):
        p = _last_find
        if not p:
            return None
        opts = dict(what=p["what"], by_rows=p["order"] in (None, xlByRows), match_case=p["match_case"], whole=p["look_at"] == xlWhole,
                    formulas=p["look_in"] == xlFormulas, fmt=p.get("fmt"))
        return self._find(opts, After, previous)

    def FindNext(self, After=None):  # noqa: N802,N803
        """Continue searching forwards with the settings of the last Find (After: start after this cell)."""
        return self._find_again(After, False)

    def FindPrevious(self, After=None):  # noqa: N802,N803
        """Search backwards with the settings of the last Find."""
        return self._find_again(After, True)

    @property
    def Next(self):  # noqa: N802
        """The cell right of the range's first cell, as Tab goes (Excel 16, measured 2026-11-20: B2:C4's is C2, rows 2:3's B2);
        the whole range moved was given."""
        a = self._a
        return Range._from(self._sheet, [(a[0], a[1], a[0], a[1])]).Offset(0, 1)

    @property
    def Previous(self):  # noqa: N802
        """The cell left of the range's first cell, as Shift+Tab goes."""
        a = self._a
        return Range._from(self._sheet, [(a[0], a[1], a[0], a[1])]).Offset(0, -1)

    @property
    def PrefixCharacter(self):  # noqa: N802
        """' when the first cell's text was typed after an apostrophe (it keeps 00123 text), else "" (Excel's)."""
        a = self._a
        return _rq("range.get", sheet=self._sheet, rect=[a[0], a[1], a[0], a[1]], what="prefix")["values"][0][0]

    # ---- Position and size ----
    # without parentheses it is "$A$1" (object model §7.2): implemented as a callable string
    @property
    def Address(self):  # noqa: N802
        rng = self

        class _Addr(str):
            def __call__(self_inner, RowAbsolute=True, ColumnAbsolute=True, ReferenceStyle=1, External=False, RelativeTo=None):  # noqa: N803
                if ReferenceStyle == xlR1C1:
                    return Range._address_r1c1(rng, RowAbsolute, ColumnAbsolute, External, RelativeTo)
                return Range._address(rng, RowAbsolute, ColumnAbsolute, External)

        return _Addr(Range._address(self, True, True, False))

    def _address(self, ra, ca, external):
        parts = []
        for a in self._areas:
            # from row 1 to the sheet's last row is whole columns, as Excel writes it (A1:A1048576 is $A:$A)
            down = a[2] == ROW_END or (a[0] == 0 and a[2] >= 1048575 and a[2] == self.Worksheet.RowLimit - 1)
            if down and not (a[1] == 0 and a[3] == MAX_COLS - 1):
                s = ("$" if ca else "") + _col_name(a[1]) + ":" + ("$" if ca else "") + _col_name(a[3])
            elif a[1] == 0 and a[3] == MAX_COLS - 1:
                # whole rows; the whole sheet too, as Excel writes it (Cells.Address is "$1:$1048576")
                last = self.Worksheet.RowLimit if a[2] == ROW_END else a[2] + 1
                s = ("$" if ra else "") + str(a[0] + 1) + ":" + ("$" if ra else "") + str(last)
            elif a[0] == a[2] and a[1] == a[3]:
                s = _a1(a[0], a[1], ra, ca)
            else:
                s = _a1(a[0], a[1], ra, ca) + ":" + _a1(a[2], a[3], ra, ca)
            parts.append(s)
        out = ",".join(parts)
        if external:
            out = f"{_quote_sheet(self.Worksheet.Name)}!" + out
        return out

    def _address_r1c1(self, ra, ca, external, relative_to):
        from . import _r1c1

        base = relative_to._a if relative_to is not None else (0, 0)
        out = ",".join(_r1c1.area_r1c1(a, ROW_END, ra, ca, base[0], base[1]) for a in self._areas)
        if external:
            out = f"{_quote_sheet(self.Worksheet.Name)}!" + out
        return out

    def AddressLocal(self, RowAbsolute=True, ColumnAbsolute=True, ReferenceStyle=1, External=False, RelativeTo=None):  # noqa: N802,N803
        return self.Address(RowAbsolute, ColumnAbsolute, ReferenceStyle, External, RelativeTo)

    @property
    def Row(self):  # noqa: N802
        return self._a[0] + 1

    @property
    def Column(self):  # noqa: N802
        return self._a[1] + 1

    def _nrows(self, a):
        return (self.Worksheet.RowLimit if a[2] == ROW_END else a[2] + 1) - a[0]

    @property
    def Count(self):  # noqa: N802
        by = getattr(self, "_by", None)
        if by == "cols":  # Columns("B:C").Count is 2 in Excel, Rows("1:3").Count 3
            return sum(a[3] - a[1] + 1 for a in self._areas)
        if by == "rows":
            return sum(self._nrows(a) for a in self._areas)
        return self._cell_count()

    def _cell_count(self):
        return sum(self._nrows(a) * (a[3] - a[1] + 1) for a in self._areas)

    CountLarge = Count

    @property
    def Left(self):  # noqa: N802
        return _rq("range.geometry", sheet=self._sheet, rect=list(self._a))["left"]

    @property
    def Top(self):  # noqa: N802
        return _rq("range.geometry", sheet=self._sheet, rect=list(self._a))["top"]

    @property
    def Width(self):  # noqa: N802
        return _rq("range.geometry", sheet=self._sheet, rect=list(self._a))["width"]

    @property
    def Height(self):  # noqa: N802
        return _rq("range.geometry", sheet=self._sheet, rect=list(self._a))["height"]

    @property
    def ColumnWidth(self):  # noqa: N802
        a = self._a
        d = _rq("dims.get", sheet=self._sheet, kind="col", index=a[1], **({"last": a[3]} if a[3] > a[1] else {}))
        return None if d.get("mixed") else d["chars"]  # columns of different widths: None (Excel's Null)

    @ColumnWidth.setter
    def ColumnWidth(self, v):  # noqa: N802
        if v is None:  # Excel's Null (columns that differ) read back: nothing changes
            return
        if not 0 <= float(v) <= 255:  # Excel refuses it rather than clamp it
            raise ApplicationDefinedError("Unable to set the ColumnWidth property: the width must be from 0 to 255")
        for a in self._areas:
            _rq("dims.set", sheet=self._sheet, kind="col", first=a[1], last=a[3], chars=float(v))

    @property
    def RowHeight(self):  # noqa: N802
        a = self._a
        d = _rq("dims.get", sheet=self._sheet, kind="row", index=a[0], **({"last": a[2]} if a[2] > a[0] else {}))
        return None if d.get("mixed") else d["points"]  # rows of different heights: None (Excel's Null)

    @RowHeight.setter
    def RowHeight(self, v):  # noqa: N802
        if v is None:  # Excel's Null (rows that differ) read back: nothing changes
            return
        if not 0 <= float(v) <= 409:  # Excel refuses it rather than clamp it
            raise ApplicationDefinedError("Unable to set the RowHeight property: the height must be from 0 to 409 points")
        for a in self._areas:
            _rq("dims.set", sheet=self._sheet, kind="row", first=a[0], last=min(a[2], 0x7FFFFFFE), points=float(v))

    def _line_kind(self, a):
        """Whole rows ("row") or whole columns ("col"): what Rows / Columns / EntireRow / EntireColumn made the range,
        else by its shape (a range down to the sheet's end is columns)."""
        mark = getattr(self, "_by", None) or getattr(self, "_whole", None)
        if mark in ("rows", "row"):
            return "row"
        if mark in ("cols", "col"):
            return "col"
        return "col" if a[2] == ROW_END else "row"

    @property
    def Hidden(self):  # noqa: N802
        a = self._a
        kind = self._line_kind(a)
        first, last = (a[1], a[3]) if kind == "col" else (a[0], min(a[2], 0x7FFFFFFE))
        d = _rq("dims.get", sheet=self._sheet, kind=kind, index=first, **({"last": last} if last > first else {}))
        return None if d.get("hidden_mixed") else d["hidden"]  # some hidden, some not: None (Excel's Null)

    @Hidden.setter
    def Hidden(self, v):  # noqa: N802
        if v is None:  # Excel's Null read back: nothing changes
            return
        for a in self._areas:
            if self._line_kind(a) == "col":
                _rq("dims.set", sheet=self._sheet, kind="col", first=a[1], last=a[3], hidden=bool(v))
            else:
                _rq("dims.set", sheet=self._sheet, kind="row", first=a[0], last=min(a[2], 0x7FFFFFFE), hidden=bool(v))

    # ---- Values and formulas ----
    def _get(self, what):
        data = _rq("range.get", sheet=self._sheet, rect=list(self._a), what=what)
        values = data["values"]
        if what == "value":
            off = data.get("date_offset", 0.0)  # the 1904 date system's serials are 1462 days fewer
            for r, c in data.get("dates", []):
                v = values[r][c]
                # a number no date shows (negative, past 9999-12-31) stays a number, as Excel's Value gives it
                if isinstance(v, (int, float)) and not isinstance(v, bool) and 0 <= v and v + off < _MAX_DATE_SERIAL:
                    values[r][c] = serial_to_datetime(v + off)
        if len(values) == 1 and len(values[0]) == 1:
            return values[0][0]
        return VArray(values)

    def _set(self, kind, value):
        a = self._a
        if kind in ("input", "formula") and len(self._areas) > 1 and isinstance(value, str) and value.startswith("=") and len(value) > 1:
            # one formula for several areas is relative to the first area's upper-left cell, as in Excel:
            # Range("A1,C1").Formula = "=B1" puts =D1 in C1 (a reference moved off the sheet is #REF!)
            from . import _r1c1

            rel = _r1c1.a1_to_r1c1(value, a[0], a[1])
            for area in self._areas:
                Range._from(self._sheet, [area])._set(kind, _r1c1.r1c1_to_a1(rel, area[0], area[1]))
            return
        rows, cols = self._nrows(a), a[3] - a[1] + 1
        payload = _to_wire_2d(value, rows, cols)
        if isinstance(payload, list) and payload and rows * cols <= 4_000_000:
            # Excel repeats a one-row array down the range and a one-column array across it (Range("A1:A3").Value =
            # [1, 2, 3] is 1, 1, 1); the cells past a larger array get #N/A
            if len(payload) == 1 and rows > 1:
                payload = payload * rows
            if cols > 1 and all(len(r) == 1 for r in payload):
                payload = [r * cols for r in payload]
        for area in self._areas:
            _rq("range.set", sheet=self._sheet, rect=list(area), kind=kind, values=payload, events=Application.EnableEvents)

    @property
    def Value(self):  # noqa: N802
        return self._get("value")

    @Value.setter
    def Value(self, v):  # noqa: N802
        self._set("input", v)

    @property
    def Value2(self):  # noqa: N802
        return self._get("value2")

    @Value2.setter
    def Value2(self, v):  # noqa: N802
        self._set("input", v)

    def _formula_texts(self, f, r1c1=False, constants=True):
        """The bridge's formulas as Excel's Formula reads them: a constant is the text of the formula bar without its
        apostrophe (5 is "5", 1E+20 "100000000000000000000", text typed as '10 is "10"); r1c1 writes formulas in R1C1.
        constants=False keeps constants as the bridge gives them (a copy writes them back as they were)."""
        from . import _r1c1

        a = self._a
        grid = f if isinstance(f, VArray) else [[f]]
        plain = None  # the cells' text values, read only when a constant comes with an apostrophe
        real = None  # which cells hold formulas: the bridge gives text typed as '=A1 as =A1
        arrays = []  # array formulas (Ctrl+Shift+Enter) met: (rect, anchor row, anchor col, formula)
        asked = [0]
        # a cell of an array formula reads the array's formula, as in Excel (the bridge gives its value); asked cell by
        # cell, so a few for a small range and one for a large one
        ask_limit = 8 if len(grid) * (len(grid[0]) if grid else 0) <= 64 else 1

        def array_formula(i, j):
            r, c = a[0] + i, a[1] + j
            for rect, ar, ac, text in arrays:
                if rect[0] <= r <= rect[2] and rect[1] <= c <= rect[3]:
                    return text, ar, ac
            if asked[0] >= ask_limit:
                return None
            asked[0] += 1
            s = _rq("range.spill", sheet=self._sheet, row=r, col=c)
            if not s or not s.get("cse"):
                return None
            ar, ac = s["anchor"]
            if (ar, ac) == (r, c):
                return None  # the anchor gives the formula itself
            text = Range._from(self._sheet, [(ar, ac, ar, ac)])._get("formula")
            if not (isinstance(text, str) and text.startswith("=")):
                return None
            arrays.append((tuple(s["rect"]), ar, ac, text))
            return text, ar, ac

        def one(x, i, j):
            nonlocal plain, real
            if constants and not (isinstance(x, str) and x.startswith("=")):
                af = array_formula(i, j)
                if af is not None:
                    text, ar, ac = af
                    return _r1c1.a1_to_r1c1(text, ar, ac) if r1c1 else text
            if isinstance(x, str) and x.startswith("="):
                if not r1c1:
                    return x
                if real is None:
                    h = self._get("has_formula")
                    real = h if isinstance(h, VArray) else [[h]]
                if real[i][j]:
                    return _r1c1.a1_to_r1c1(x, a[0] + i, a[1] + j)
                return x if constants else "'" + x  # text: a copy writes it back as text
            if not constants:
                return x
            if isinstance(x, (int, float)) and not isinstance(x, bool):
                return _formula_number_text(x)
            if isinstance(x, str) and x.startswith("'"):
                if plain is None:
                    v = self._get("value2")
                    plain = v if isinstance(v, VArray) else [[v]]
                try:
                    if plain[i][j] == x[1:]:  # the apostrophe only marks the text (PrefixCharacter), as in Excel
                        return x[1:]
                except IndexError:
                    pass
            return x

        out = [[one(x, i, j) for j, x in enumerate(row)] for i, row in enumerate(grid)]
        return VArray(out) if isinstance(f, VArray) else out[0][0]

    def _formula_r1c1_raw(self):
        return self._formula_texts(self._get("formula"), r1c1=True, constants=False)

    @property
    def Formula(self):  # noqa: N802
        """The formula, or for a constant the text of the formula bar (always text, as in Excel: 5 reads "5")."""
        return self._formula_texts(self._get("formula"))

    @Formula.setter
    def Formula(self, v):  # noqa: N802
        self._set("formula", v)

    @property
    def FormulaR1C1(self):  # noqa: N802
        """R1C1-style formula (relative references are expressed from each cell's own position, e.g. =R[-1]C+1)."""
        return self._formula_texts(self._get("formula"), r1c1=True)

    @FormulaR1C1.setter
    def FormulaR1C1(self, v):  # noqa: N802
        from . import _r1c1

        if hasattr(v, "tolist") and hasattr(v, "ndim"):
            v = v.tolist()
        if isinstance(v, (list, tuple)):
            rows = v if v and isinstance(v[0], (list, tuple)) else [v]
            a = self._a
            conv = [[(_legacy_at(_r1c1.r1c1_to_a1(x, a[0] + i, a[1] + j)) if isinstance(x, str) and x.startswith("=") else x)
                     for j, x in enumerate(row)] for i, row in enumerate(rows)]
            self._set("formula", conv)
            return
        if isinstance(v, str) and v.startswith("="):
            # the same R1C1 formula for every area: converted to A1 at each area's upper-left cell; YCGrid offsets the other cells
            for area in self._areas:
                Range._from(self._sheet, [area]).Formula = _legacy_at(_r1c1.r1c1_to_a1(v, area[0], area[1]))
            return
        self._set("formula", v)

    FormulaR1C1Local = FormulaR1C1
    FormulaLocal = Formula

    @property
    def Formula2(self):  # noqa: N802
        """Formula with dynamic array semantics (YCGrid doesn't spill yet, so it is the same as Formula)."""
        return self.Formula

    @Formula2.setter
    def Formula2(self, v):  # noqa: N802
        self.Formula = v

    @property
    def Formula2R1C1(self):  # noqa: N802
        return self.FormulaR1C1

    @Formula2R1C1.setter
    def Formula2R1C1(self, v):  # noqa: N802
        self.FormulaR1C1 = v

    @property
    def Text(self):  # noqa: N802
        # what the cells show, as Excel reads it for a range: their text when they all show the same, else None (Null)
        v = self._get("text")
        if isinstance(v, list):
            texts = {x for row in v for x in (row if isinstance(row, list) else [row])}
            return texts.pop() if len(texts) == 1 else None
        return v

    @property
    def HasFormula(self):  # noqa: N802
        if len(self._areas) > 1:  # every area's cells, as Excel reads them (the first area alone was read)
            each = [Range._from(self._sheet, [a]).HasFormula for a in self._areas]
            return True if all(x is True for x in each) else (False if all(x is False for x in each) else None)
        f = self._get("has_formula")
        if isinstance(f, VArray):
            flat = [x for row in f for x in row]
            return True if all(flat) else (False if not any(flat) else None)
        return bool(f)

    @property
    def NumberFormat(self):  # noqa: N802
        # one format for the whole range, as Excel reads it: None (Null) when the cells differ (it gave a list per cell)
        v = self._get("numfmt")
        if isinstance(v, list):
            codes = {x for row in v for x in (row if isinstance(row, list) else [row])}
            return codes.pop() if len(codes) == 1 else None
        return v

    @NumberFormat.setter
    def NumberFormat(self, v):  # noqa: N802
        if v is None:  # Excel's Null read back: nothing changes (it wrote the format "None")
            return
        self._style(numfmt=str(v))

    NumberFormatLocal = NumberFormat

    @property
    def ValueNs(self):  # noqa: N802
        v = self.Value
        if isinstance(v, VArray):
            return VArray([[_to_ns(x) for x in row] for row in v])
        return _to_ns(v)

    @ValueNs.setter
    def ValueNs(self, v):  # noqa: N802
        def conv(x):
            if x is None or isinstance(x, (str, XlError)):  # empty, text or an error value: written as they read
                return x
            return _decimal.Decimal(int(x)).scaleb(-9)

        if isinstance(v, (list, tuple)):
            v = [[conv(x) for x in row] if isinstance(row, (list, tuple)) else conv(row) for row in v]
        else:
            v = conv(v)
        self._set("value", v)

    def ToNumpy(self, dtype="float64", Empty=float("nan")):  # noqa: N802,N803
        v = self.Value
        if not isinstance(v, VArray):
            v = VArray([[v]])
        return v.ToNumpy(dtype, Empty)

    # ---- Formatting ----
    def _style(self, **props):
        _rq("range.style", **self._p(props=props))

    def _style_get(self, raw=False):
        """The first cell's formats; several cells, or characters with fonts of their own: a property they don't share
        is None (Excel's Null). raw: the top-left cell's own formats, which its characters' fonts start from."""
        a = self._a
        if raw:
            st = _rq("range.style_get", sheet=self._sheet, row=a[0], col=a[1], raw=True)
            st.pop("mixed", None)
            return st
        if a[0] == a[2] and a[1] == a[3]:
            st = _rq("range.style_get", sheet=self._sheet, row=a[0], col=a[1])
        else:
            st = _rq("range.style_get", sheet=self._sheet, row=a[0], col=a[1], rect=list(a))
        for k in st.pop("mixed", ()):
            st[k] = None
        return st

    def _border_set(self, edges, style, color):
        _rq("range.border", **self._p(edges=edges, style=style, color=color))

    @property
    def Style(self):  # noqa: N802
        """The cell's named style (a Style); set it with a name ("Good", "Heading 1", a custom style) or a Style."""
        return Style(self._style_get().get("cell_style", "Normal"))

    @Style.setter
    def Style(self, v):  # noqa: N802
        name = v.Name if isinstance(v, Style) else str(v)
        _rq("range.cellstyle", **self._p(name=name))

    @property
    def Font(self):  # noqa: N802
        return Font(self)

    def Characters(self, Start=1, Length=None):  # noqa: N802,N803
        """Part of the (top-left) cell's text: .Text, .Font (Bold, Italic, Color, Size, Name, Superscript, Subscript...), .Insert, .Delete."""
        return Characters(self, Start, Length)

    @property
    def Interior(self):  # noqa: N802
        return Interior(self)

    @property
    def Borders(self):  # noqa: N802
        return Borders(self)

    @property
    def HorizontalAlignment(self):  # noqa: N802
        h = self._style_get()["halign"]
        return None if h is None else _HALIGN_OUT[h]

    @HorizontalAlignment.setter
    def HorizontalAlignment(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._style(halign=_HALIGN_IN.get(v, 0))

    @property
    def VerticalAlignment(self):  # noqa: N802
        v = self._style_get()["valign"]
        return None if v is None else _VALIGN_OUT[v]

    @VerticalAlignment.setter
    def VerticalAlignment(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._style(valign=_VALIGN_IN.get(v, 0))

    @property
    def WrapText(self):  # noqa: N802
        return self._style_get()["wrap"]

    @WrapText.setter
    def WrapText(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._style(wrap=bool(v))

    @property
    def ShrinkToFit(self):  # noqa: N802
        return self._style_get()["shrink"]

    @ShrinkToFit.setter
    def ShrinkToFit(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._style(shrink=bool(v))

    @property
    def IndentLevel(self):  # noqa: N802
        return self._style_get()["indent"]

    @IndentLevel.setter
    def IndentLevel(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        # several cells (a merged cell is one) are indented as a block: every one becomes Left-aligned, Right ones too
        block = self.Count > 1
        if block and len(self._areas) == 1:
            try:  # one merged cell is one cell
                block = self.Cells(1, 1).MergeArea.Address != self.Address
            except Exception:  # noqa: BLE001 (a reply without the area: indent as several cells)
                pass
        self._style(indent=int(v), **({"indent_block": True} if block else {}))

    def InsertIndent(self, InsertAmount):  # noqa: N802,N803
        """Add InsertAmount (may be negative) to the indent level (0..15)."""
        self.IndentLevel = max(0, min(15, int(self.IndentLevel or 0) + int(InsertAmount)))

    @property
    def ReadingOrder(self):  # noqa: N802
        """Format Cells > Alignment > Text direction: xlContext, xlLTR or xlRTL (Right-to-Left mirrors General alignment)."""
        r = self._style_get().get("reading_order", 0)
        return None if r is None else {0: xlContext, 1: xlLTR, 2: xlRTL}.get(r, xlContext)

    @ReadingOrder.setter
    def ReadingOrder(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._style(reading_order={xlContext: 0, xlLTR: 1, xlRTL: 2}.get(int(v), 0))

    @property
    def JustifyLastLine(self):  # noqa: N802
        """[YC] Format Cells > Alignment > Justify distributed (with HorizontalAlignment = xlDistributed); the file
        formats' justifyLastLine."""
        return bool(self._style_get().get("justify_last_line", False))

    @JustifyLastLine.setter
    def JustifyLastLine(self, v):  # noqa: N802
        self._style(justify_last_line=bool(v))

    @property
    def Orientation(self):  # noqa: N802
        """Degrees (-90..90, counterclockwise) or xlHorizontal / xlUpward / xlDownward / xlVertical (stacked), like Excel."""
        r = self._style_get()["rotation"]
        return {0: xlHorizontal, 90: xlUpward, -90: xlDownward, 255: xlVertical}.get(r, r)

    @Orientation.setter
    def Orientation(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        v = int(v)
        v = {xlHorizontal: 0, xlUpward: 90, xlDownward: -90, xlVertical: 255}.get(v, v)
        if v != 255 and not -90 <= v <= 90:
            raise ApplicationDefinedError("Orientation must be -90..90 or an xlOrientation constant")
        self._style(rotation=v)

    @property
    def Locked(self):  # noqa: N802
        return self._style_get()["locked"]

    @Locked.setter
    def Locked(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._style(locked=bool(v))

    @property
    def FormulaHidden(self):  # noqa: N802
        return self._style_get()["hidden"]

    @FormulaHidden.setter
    def FormulaHidden(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._style(hidden=bool(v))

    @property
    def MergeCells(self):  # noqa: N802
        # True when every cell is in a merged area, None (Excel's Null) when only some are
        states = {_rq("range.merge_state", sheet=self._sheet, rect=list(a)) for a in self._areas}
        if states == {0}:
            return False
        if states == {1}:
            return True
        return None

    @MergeCells.setter
    def MergeCells(self, v):  # noqa: N802
        if v is None:  # Excel's Null read back: nothing changes
            return
        if v:  # Excel 16, measured: unlike Merge(), the upper-left cell keeps its formats and the others go back to Normal
            for a in self._areas:
                _rq("range.merge", sheet=self._sheet, rect=list(a), across=False, property=True)
        else:
            self.UnMerge()

    def BorderAround(self, LineStyle=xlContinuous, Weight=xlThin, ColorIndex=None, Color=None):  # noqa: N802,N803
        for edge in (xlEdgeLeft, xlEdgeTop, xlEdgeBottom, xlEdgeRight):
            b = self.Borders(edge)
            b.LineStyle = LineStyle
            b.Weight = Weight
            if Color is not None:
                b.Color = Color

    # ---- Editing ----
    def ClearContents(self):  # noqa: N802
        _rq("range.clear", **self._p(what=0, events=Application.EnableEvents))

    def ClearFormats(self):  # noqa: N802
        _rq("range.clear", **self._p(what=1))

    def Clear(self):  # noqa: N802
        _rq("range.clear", **self._p(what=2, events=Application.EnableEvents))

    def _shift_mode(self, a, Shift, insert):  # noqa: N803
        """How Delete / Insert moves the cells of area a: whole columns or rows, else Shift; without Shift, Excel's
        rule: an area with more rows than columns shifts sideways (Range("A1:A2").Delete shifts left), others up / down."""
        # the whole sheet is its rows when it was made of rows (Range("B:B").EntireRow, Rows): its columns were deleted
        mark = getattr(self, "_by", None) or getattr(self, "_whole", None)
        wide = a[1] == 0 and a[3] == MAX_COLS - 1
        if a[2] == ROW_END and not (wide and mark in ("row", "rows")):
            return "cols"
        if wide:
            return "rows"
        if Shift is None:
            sideways = self._nrows(a) > a[3] - a[1] + 1
        else:
            sideways = Shift == (xlShiftToRight if insert else xlShiftToLeft)
        if insert:
            # *_auto: inside a table's data YCGrid inserts table rows (columns), as Excel 16's Insert without Shift does
            # (measured 2026-11-20: B3:C4 inserts rows 3:4 of A1:E6, B2:B6 a table column); an explicit Shift there is
            # refused
            return ("right" if sideways else "down") + ("_auto" if Shift is None else "")
        # left_auto: YCGrid shifts up where a filter hides rows in the area, as Excel 16 does (measured 2026-11-20:
        # B2:B6.Delete in a filtered list takes the rows it shows whole, and shifts left otherwise)
        return ("left_auto" if Shift is None else "left") if sideways else "up"

    def _areas_last_first(self):
        """The areas for Delete / Insert, the lowest (then rightmost) first, so that each one acts on the cells it named
        (Range("A2,A4").EntireRow.Delete deletes rows 2 and 4, as in Excel). Overlapping areas, or whole rows / columns
        with other cells, are refused with Excel's messages."""
        areas = list(self._areas)
        if len(areas) > 1:
            whole = [a[2] == ROW_END or (a[1] == 0 and a[3] == MAX_COLS - 1) for a in areas]
            for i, a in enumerate(areas):
                for b in areas[i + 1:]:
                    if a[0] <= b[2] and b[0] <= a[2] and a[1] <= b[3] and b[1] <= a[3]:
                        raise ApplicationDefinedError("That command cannot be used on overlapping selections.")
            if any(whole) and not all(whole):
                raise ApplicationDefinedError("This command cannot be used on selections that contain entire rows or columns and also other "
                                              "cells. Select only entire rows, entire columns or groups of cells.")
        return sorted(areas, key=lambda a: (a[0], a[1]), reverse=True)

    def Delete(self, Shift=None):  # noqa: N802,N803
        for a in self._areas_last_first():
            _rq("range.delete", sheet=self._sheet, rect=list(a), mode=self._shift_mode(a, Shift, False))

    def Insert(self, Shift=None, CopyOrigin=None):  # noqa: N802,N803
        areas = self._areas_last_first()
        if len(areas) == 1:
            a = areas[0]
            # cells copied or cut (CutCopyMode): Insert Copied Cells / Insert Cut Cells there, as in Excel
            if _rq("app.cut_copy_mode") and _rq("cells.insert_copied", sheet=self._sheet, rect=list(a),
                                                  mode=self._shift_mode(a, Shift, True).replace("_auto", "")):
                return
        for a in areas:
            # CopyOrigin: xlFormatFromLeftOrAbove (default) or xlFormatFromRightOrBelow, the formats the new cells take
            _rq("range.insert", sheet=self._sheet, rect=list(a), mode=self._shift_mode(a, Shift, True), origin=1 if CopyOrigin == 1 else 0)

    def Justify(self):  # noqa: N802
        """Home > Fill > Justify: the text of the first column spread over lines as wide as the range, one per row."""
        _rq("cells.justify", sheet=self._sheet, rect=list(self._a))

    # ---- What-If Analysis and data tools ----
    def GoalSeek(self, Goal, ChangingCell):  # noqa: N802,N803
        """Change ChangingCell until this formula cell returns Goal; True when a solution was found."""
        a, c = self._a, ChangingCell._a
        r = _rq("whatif.goalseek", sheet=self._sheet, row=a[0], col=a[1], goal=float(Goal), ch_sheet=ChangingCell._sheet, ch_row=c[0], ch_col=c[1])
        return bool(r["found"])

    def Table(self, RowInput=None, ColumnInput=None):  # noqa: N802,N803
        """Make this range a data table ({=TABLE(RowInput, ColumnInput)}); give one or both input cells."""
        if RowInput is None and ColumnInput is None:
            raise ApplicationDefinedError("Table needs RowInput, ColumnInput or both")
        _rq("whatif.table", sheet=self._sheet, rect=list(self._a), row=None if RowInput is None else [RowInput._a[0], RowInput._a[1]],
            col=None if ColumnInput is None else [ColumnInput._a[0], ColumnInput._a[1]])

    def Consolidate(self, Sources, Function=xlSum, TopRow=False, LeftColumn=False, CreateLinks=False):  # noqa: N802,N803
        """Consolidate Sources (references as R1C1 or A1 text, e.g. "Sheet1!R1C1:R5C3", or Range objects) at this cell."""
        funcs = {xlSum: 0, xlCount: 1, xlAverage: 2, xlMax: 3, xlMin: 4, xlProduct: 5, xlCountNums: 6, xlStDev: 7, xlStDevP: 8, xlVar: 9, xlVarP: 10}
        if Function not in funcs:
            raise ApplicationDefinedError(f"Unknown consolidation function: {Function}")
        if isinstance(Sources, (str, Range)):
            Sources = [Sources]
        from . import _r1c1

        refs = []
        for src in Sources:
            if isinstance(src, Range):
                refs.append(f"'{Worksheet(src._sheet).Name}'!{src.Address}")
                continue
            text = str(src)
            sheet, _, ref = text.rpartition("!")
            if _re.fullmatch(r"R\d*C\d*(:R\d*C\d*)?", ref.upper()):
                ref = _r1c1.r1c1_to_a1("=" + ref, 0, 0)[1:]
            refs.append(f"{sheet}!{ref}" if sheet else ref)
        a = self._a
        _rq("range.consolidate", sheet=self._sheet, row=a[0], col=a[1], sources=refs, func=funcs[Function], top=bool(TopRow), left=bool(LeftColumn),
            links=bool(CreateLinks))

    def AdvancedFilter(self, Action, CriteriaRange=None, CopyToRange=None, Unique=False):  # noqa: N802,N803
        """Filter this list with a criteria range: Action xlFilterInPlace (hide the other rows; ShowAllData shows them) or xlFilterCopy."""
        copy = Action == xlFilterCopy
        if copy and CopyToRange is None:
            raise ApplicationDefinedError("xlFilterCopy needs CopyToRange")
        p = {"rect": list(self._a), "copy": copy, "unique": bool(Unique)}
        if CriteriaRange is not None:
            p["crit_sheet"], p["crit"] = CriteriaRange._sheet, list(CriteriaRange._a)
        if copy:
            p["dest_sheet"], p["dest"] = CopyToRange._sheet, list(CopyToRange._a)
        _rq("range.advfilter", sheet=self._sheet, **p)

    def FlashFill(self):  # noqa: N802
        """Fill this column from the examples typed above it (like Ctrl+E); returns True."""
        a = self._a
        _rq("range.flashfill", sheet=self._sheet, row=a[0], col=a[1])
        return True

    # ---- [YC] market data bound to cells ----
    def BindTopics(self, TopicIds):  # noqa: N802,N803
        """[YC] Bind every cell to a market data topic (ids from Application.Topics.Register, same shape as the range):
        each Topics.Push updates the cell directly, with no formula. Bindings are runtime state: bind again in Workbook_Open."""
        a = self._a
        rows, cols = self._nrows(a), a[3] - a[1] + 1
        flat = _Topics._flat(TopicIds)
        if len(flat) != rows * cols:
            raise ApplicationDefinedError(f"BindTopics needs {rows * cols} topic ids for this range (got {len(flat)})")
        names = ["" if i is None else "QUOTE|" + Application.Topics._name(i) for i in flat]
        _rq("topic.bind", sheet=self._sheet, rect=list(a), topics=names)

    def UnbindTopics(self):  # noqa: N802
        """[YC] Remove the topic bindings of the range (the cells keep their last values)."""
        _rq("topic.unbind", sheet=self._sheet, rect=list(self._a))

    @property
    def TopicIds(self):  # noqa: N802
        """[YC] The topic ids bound to the cells (rows of ids; None where a cell isn't bound)."""
        a = self._a
        cols = a[3] - a[1] + 1
        names = _rq("topic.bound", sheet=self._sheet, rect=list(a))
        ids = [Application.Topics.Register(n[6:]) if n.startswith("QUOTE|") else None for n in names]
        rows = [ids[i:i + cols] for i in range(0, len(ids), cols)]
        return rows[0][0] if len(ids) == 1 else rows

    def Copy(self, Destination=None):  # noqa: N802,N803
        if Destination is None:
            _rq("ui.copy", sheet=self._sheet, rect=list(self._a), cut=False)
            return
        if len(self._areas) > 1:
            self._copy_areas(Destination)
            return
        if getattr(Destination, "_cx", None) is not getattr(self, "_cx", None):
            # another workbook: values and formulas go across (relative references follow, like Excel); formats stay behind
            data = self._formula_r1c1_raw()
            a = self._a
            Destination.Resize(a[2] - a[0] + 1, a[3] - a[1] + 1).FormulaR1C1 = data
            return
        d = Destination._a
        _rq("range.copy", sheet=self._sheet, rect=list(self._a), dest_sheet=Destination._sheet, row=d[0], col=d[1], dest=list(d), mode="all", cut=False)

    def _copy_areas(self, Destination):  # noqa: N803
        """Copy of several areas, as Excel's: areas that share their columns (rows) are pasted one under (beside) the
        other in sheet order, closed up (Range("A1,A3").Copy Range("C1") fills C1:C2), with their formats; formulas
        paste as their values. Areas that don't line up raise Excel's "This action won't work on multiple selections"."""
        first = self._areas[0]
        if all((a[1], a[3]) == (first[1], first[3]) for a in self._areas):
            areas, down = sorted(self._areas, key=lambda a: a[0]), True
        elif all((a[0], a[2]) == (first[0], first[2]) for a in self._areas):
            areas, down = sorted(self._areas, key=lambda a: a[1]), False
        else:
            raise ApplicationDefinedError("This action won't work on multiple selections.")
        places, at = [], 0
        for a in areas:
            places.append((at, 0) if down else (0, at))
            at += self._nrows(a) if down else a[3] - a[1] + 1
        h = at if down else self._nrows(first)
        w = first[3] - first[1] + 1 if down else at
        d = Destination._a
        dh, dw = Destination._nrows(d), d[3] - d[1] + 1
        # a destination that is a whole multiple of the copy gets it repeated (as one area's copy does)
        tiles_r, tiles_c = (dh // h, dw // w) if dh % h == 0 and dw % w == 0 and dh * dw < 1_000_000 else (1, 1)
        other = getattr(Destination, "_cx", None) is not getattr(self, "_cx", None)
        for a, (oy, ox) in zip(areas, places):
            rows, cols = self._nrows(a), a[3] - a[1] + 1
            values = _rq("range.get", sheet=self._sheet, rect=list(a), what="value2")["values"]
            formulas = other or any(x for row in _rq("range.get", sheet=self._sheet, rect=list(a), what="has_formula")["values"] for x in row)
            for ti in range(tiles_r):
                for tj in range(tiles_c):
                    r0, c0 = d[0] + ti * h + oy, d[1] + tj * w + ox
                    if not other:
                        _rq("range.copy", sheet=self._sheet, rect=list(a), dest_sheet=Destination._sheet, row=r0, col=c0, dest=[r0, c0, r0, c0],
                            mode="all", cut=False)
                    if formulas:  # the values, written as they are (no typing rules: text that looks like a date stays text)
                        target = Range._from(Destination._sheet, [(r0, c0, r0 + rows - 1, c0 + cols - 1)])
                        object.__setattr__(target, "_cx", getattr(Destination, "_cx", None))
                        target._set("value", values)

    def Cut(self, Destination=None):  # noqa: N802,N803
        if len(self._areas) > 1:  # Excel refuses it (it would cut the first area only)
            raise ApplicationDefinedError("This can't be done on a multiple range selection. Select a single range and try again.")
        if Destination is None:
            _rq("ui.copy", sheet=self._sheet, rect=list(self._a), cut=True)
            return
        d = Destination._a
        _rq("range.copy", sheet=self._sheet, rect=list(self._a), dest_sheet=Destination._sheet, row=d[0], col=d[1], dest=list(d), mode="all", cut=True)

    def PasteSpecial(self, Paste=None, Operation=None, SkipBlanks=False, Transpose=False):  # noqa: N802,N803
        """Paste: xlPasteAll, xlPasteValues, xlPasteFormulas, xlPasteFormats, xlPasteComments (comments and notes),
        xlPasteValidation, xlPasteAllUsingSourceTheme, xlPasteAllExceptBorders, xlPasteColumnWidths,
        xlPasteFormulasAndNumberFormats, xlPasteValuesAndNumberFormats, xlPasteAllMergingConditionalFormats.
        Operation: xlPasteSpecialOperationAdd / Subtract / Multiply / Divide combine the copied numbers
        with the cells' numbers (a formula becomes =(formula)+number; a copied formula =5+(formula)); text, TRUE /
        FALSE and errors, copied or in the cells, leave the cells as they are; text that reads as a number counts.
        Transpose turns references to the copied cells with them, absolute ones too, as in Excel."""
        from .constants import (xlPasteValues, xlPasteFormats, xlPasteFormulas, xlPasteAllExceptBorders, xlPasteValuesAndNumberFormats,
                                xlPasteColumnWidths, xlPasteFormulasAndNumberFormats, xlPasteComments, xlPasteValidation,
                                xlPasteAllUsingSourceTheme, xlPasteAllMergingConditionalFormats)

        mode = {xlPasteValues: "values", xlPasteFormats: "formats", xlPasteFormulas: "formulas", xlPasteAllExceptBorders: "noborders",
                xlPasteValuesAndNumberFormats: "valuesformats", xlPasteColumnWidths: "widths",
                xlPasteFormulasAndNumberFormats: "formulasformats", xlPasteComments: "comments", xlPasteValidation: "validation",
                xlPasteAllUsingSourceTheme: "sourcetheme", xlPasteAllMergingConditionalFormats: "mergecf"}.get(Paste, "all")
        op = {2: 1, 3: 2, 4: 3, 5: 4}.get(Operation, 0)
        # the whole destination: a copy that fits it a whole number of times fills it (one copied cell, every cell), as in Excel
        _rq("ui.paste", sheet=self._sheet, row=self._a[0], col=self._a[1], rect=list(self._a), mode=mode, transpose=bool(Transpose),
            skip_blanks=bool(SkipBlanks), op=op)

    def Merge(self, Across=False):  # noqa: N802,N803
        _no_overlap(self)
        for a in self._areas:
            _rq("range.merge", sheet=self._sheet, rect=list(a), across=bool(Across))

    def UnMerge(self):  # noqa: N802
        for a in self._areas:
            _rq("range.unmerge", sheet=self._sheet, rect=list(a))

    def AutoFit(self):  # noqa: N802
        for a in self._areas:
            kind = "col" if a[2] == ROW_END else "row"
            _rq("dims.autofit", sheet=self._sheet, kind=kind, first=a[1] if kind == "col" else a[0], last=a[3] if kind == "col" else a[2])

    def AutoFill(self, Destination, Type=0):  # noqa: N802,N803
        """Type: xlFillDefault, xlFillCopy, xlFillSeries, xlFillFormats, xlFillValues, xlFillDays, xlFillWeekdays, xlFillMonths,
        xlFillYears, xlLinearTrend, xlGrowthTrend."""
        _rq("range.autofill", sheet=self._sheet, src=list(self._a), dest=list(Destination._a), type=int(Type or 0))

    def FillDown(self):  # noqa: N802
        self._fill(0)

    def FillRight(self):  # noqa: N802
        self._fill(1)

    def FillUp(self):  # noqa: N802
        self._fill(2)

    def FillLeft(self):  # noqa: N802
        self._fill(3)

    def _fill(self, d):
        """Fill Down / Right / Up / Left. Several areas, as Excel 16 fills them (measured 2026-11-20): each column (row)
        takes the cell of all the areas' that comes first in the fill's direction (FillDown: the topmost), and every
        other cell of the areas in that column (row) gets it; only the first area was filled."""
        if len(self._areas) == 1:
            # one line deep in the fill's direction: Excel 16 fills it from the line beside it (measured 2026-11-20:
            # FillDown on B3 takes B2, FillUp on A3:C3 takes row 4, FillRight on B2:B4 column A); at the sheet's edge it
            # raises. Nothing was filled
            a = list(self._a)
            last = a[2] if a[2] != ROW_END else 1048575
            edge = False
            if d in (0, 2) and a[0] == last:
                edge = a[0] == 0 if d == 0 else last >= 1048575
                if d == 0:
                    a[0] -= 1
                else:
                    a[2] = last + 1
            elif d in (1, 3) and a[1] == a[3]:
                edge = a[1] == 0 if d == 1 else a[3] >= MAX_COLS - 1
                if d == 1:
                    a[1] -= 1
                else:
                    a[3] += 1
            if edge:
                raise ApplicationDefinedError(f"{('FillDown', 'FillRight', 'FillUp', 'FillLeft')[d]} method of Range class failed: "
                                              "there is no row or column beside the range to fill it from")
            _rq("range.fill", sheet=self._sheet, rect=a, dir=d)
            return
        vertical = d in (0, 2)
        lo_end = d in (0, 1)  # the source is at the low end (top, left)
        ar = [tuple(a) for a in self._areas]
        # the source line (row for a vertical fill, column otherwise) of each column (row) the areas cover
        src = {}
        for a in ar:
            across = range(a[1], a[3] + 1) if vertical else range(a[0], (a[2] if a[2] != ROW_END else 1048575) + 1)
            line = (a[0] if lo_end else a[2]) if vertical else (a[1] if lo_end else a[3])
            for k in across:
                cur = src.get(k)
                src[k] = line if cur is None else (min(cur, line) if lo_end else max(cur, line))
        for a in ar:
            across = list(range(a[1], a[3] + 1) if vertical else range(a[0], (a[2] if a[2] != ROW_END else 1048575) + 1))
            own = (a[0] if lo_end else a[2]) if vertical else (a[1] if lo_end else a[3])
            # runs of columns (rows) with one source line: the area's own edge fills, another line is copied in
            i = 0
            while i < len(across):
                j = i
                while j + 1 < len(across) and src[across[j + 1]] == src[across[i]]:
                    j += 1
                s, k0, k1 = src[across[i]], across[i], across[j]
                part = (a[0], k0, a[2], k1) if vertical else (k0, a[1], k1, a[3])
                if s == own:
                    _rq("range.fill", sheet=self._sheet, rect=list(part), dir=d)
                else:
                    cells = (s, k0, s, k1) if vertical else (k0, s, k1, s)
                    Range._from(self._sheet, [cells]).Copy(Range._from(self._sheet, [part]))
                i = j + 1

    def Replace(self, What, Replacement, LookAt=None, SearchOrder=None, MatchCase=False, MatchByte=None, SearchFormat=False,  # noqa: N802,N803
                ReplaceFormat=False):
        """SearchFormat=True: only cells with Application.FindFormat; ReplaceFormat=True: Application.ReplaceFormat is
        written into each replaced cell (What="" with SearchFormat: every cell with the format, its contents kept).
        Returns True, as Excel's Replace does, whether or not anything was replaced."""
        try:
            _rq("range.replace", **self._whole_if_cell()._p(what=str(What), repl=str(Replacement), whole=LookAt == xlWhole,
                                                           match_case=bool(MatchCase), fmt=_find_format._spec() if SearchFormat else None,
                                                           repl_fmt=_replace_format._spec() if ReplaceFormat else None))
        except ProtectedError:
            pass  # locked cells of a protected sheet: Excel 16 replaces nothing and returns True (measured 2026-11-20)
        return True

    def Sort(self, Key1=None, Order1=xlAscending, Key2=None, Type=None, Order2=xlAscending, Key3=None, Order3=xlAscending, Header=2,  # noqa: N802,N803
             OrderCustom=None, **kw):
        """OrderCustom: 1 + the number of a custom list (Application.GetCustomListContents) that orders Key1, as in
        Excel (1 is the normal order; recorded macros write OrderCustom:=1).
        Header: xlNo (the default), xlYes or xlGuess (a first row of text over other values is a header). One cell
        sorts its current region, as in Excel."""
        _one_area(self)
        if self.Count == 1 and self.CurrentRegion.Count > 1:
            return self.CurrentRegion.Sort(Key1, Order1, Key2, Type, Order2, Key3, Order3, Header, OrderCustom, **kw)
        ltr = kw.get("Orientation", xlTopToBottom) == xlLeftToRight
        keys = []
        for k, o in ((Key1, Order1), (Key2, Order2), (Key3, Order3)):
            if k is None:
                continue
            kr = k if isinstance(k, Range) else _make_range(self._sheet, k)
            keys.append([kr._a[0] if ltr else kr._a[1], o == xlDescending])
        if not keys:
            keys = [[self._a[0] if ltr else self._a[1], Order1 == xlDescending]]
        if OrderCustom and int(OrderCustom) > 1:  # Excel 16, measured: OrderCustom n + 1 orders by list n
            keys[0].append(int(OrderCustom) - 2)
        header = Header == xlYes or (Header == xlGuess and not ltr and _guess_header(self._sheet, self._a))
        _rq("range.sort", sheet=self._sheet, rect=list(self._a), keys=keys, header=header, match_case=bool(kw.get("MatchCase", False)),
            left_to_right=ltr)

    def Subtotal(self, GroupBy, Function, TotalList, Replace=True, PageBreaks=False, SummaryBelowData=1):  # noqa: N802,N803
        """Data > Subtotal: GroupBy and TotalList are field numbers (1 = the list's first column); Function xlSum,
        xlCount, xlAverage, xlMax, xlMin, xlProduct, xlCountNums, xlStDev, xlStDevP, xlVar, xlVarP. One cell: its
        current region is the list (header row first)."""
        _one_area(self)
        funcs = {xlAverage: 1, xlCountNums: 2, xlCount: 3, xlMax: 4, xlMin: 5, xlProduct: 6, xlStDev: 7, xlStDevP: 8, xlSum: 9, xlVar: 10, xlVarP: 11}
        if Function not in funcs:
            raise ApplicationDefinedError(f"Unknown subtotal function: {Function}")
        rng = self.CurrentRegion if self.Count == 1 else self
        a = rng._a
        totals = TotalList if isinstance(TotalList, (list, tuple)) else [TotalList]
        _rq("range.subtotal", sheet=self._sheet, rect=list(a), group=a[1] + int(GroupBy) - 1, func=funcs[Function],
            totals=[a[1] + int(t) - 1 for t in totals], replace=bool(Replace), page_breaks=bool(PageBreaks), below=SummaryBelowData != 0)
        return True

    def RemoveSubtotal(self):  # noqa: N802
        """Subtotal > Remove All: the subtotal rows, the list's outline and its page breaks go."""
        rng = self.CurrentRegion if self.Count == 1 else self
        _rq("range.removesubtotal", sheet=self._sheet, rect=list(rng._a))
        return True

    def Calculate(self):  # noqa: N802
        """Calculate the formulas of the range only, from the values the other cells show (Excel's Range.Calculate)."""
        for a in self._areas:
            _rq("calc.run", full=False, sheet=self._sheet, rect=list(a))

    def Dirty(self):  # noqa: N802
        """Mark the range's formulas to be calculated again (Excel's Range.Dirty): with automatic calculation they are
        calculated at once, with manual calculation at the next Calculate."""
        if Application.Calculation == xlCalculationManual:
            for a in self._areas:
                _rq("calc.dirty", sheet=self._sheet, rect=list(a))
        else:
            self.Calculate()

    def Select(self):  # noqa: N802
        _rq("view.set", activate_sheet=self._sheet, select=[list(a) for a in self._areas])

    def Activate(self):  # noqa: N802
        a = self._a
        _rq("view.set", activate_sheet=self._sheet, active=[a[0], a[1]])

    def Show(self):  # noqa: N802
        a = self._a
        _rq("view.set", activate_sheet=self._sheet, ensure_visible=[a[0], a[1]])

    def AddComment(self, Text=""):  # noqa: N802,N803
        _one_area(self)
        a = self._a
        if a[0] != a[2] or a[1] != a[3]:  # Excel 16 refuses several cells (measured 2026-11-20)
            raise ApplicationDefinedError("AddComment method of Range class failed: a note goes on one cell")
        if _rq("note.get", sheet=self._sheet, row=self._a[0], col=self._a[1]):
            # Excel refuses a second note (error 1004): change it with Comment.Text, or ClearComments first
            raise ApplicationDefinedError("The cell already has a comment (use Comment.Text to change it)")
        _rq("note.set", sheet=self._sheet, row=self._a[0], col=self._a[1], text=str(Text))
        return self.Comment

    @property
    def Comment(self):  # noqa: N802
        """The note (None when there is none or the cell has a threaded comment): .Text(), .Author, .Delete()."""
        n = _rq("note.get", sheet=self._sheet, row=self._a[0], col=self._a[1])
        return _Note(self._sheet, self._a[0], self._a[1], n) if n and not n.get("threaded") else None

    def AddCommentThreaded(self, Text):  # noqa: N802,N803
        """Start a comment thread on the top-left cell (replaces a note there)."""
        _rq("comment.set", sheet=self._sheet, row=self._a[0], col=self._a[1], text=str(Text), author=Application.UserName)
        return CommentThreaded(self._sheet, self._a[0], self._a[1])

    @property
    def CommentThreaded(self):  # noqa: N802
        """The comment thread of the top-left cell (None when there is none)."""
        n = _rq("note.get", sheet=self._sheet, row=self._a[0], col=self._a[1])
        return CommentThreaded(self._sheet, self._a[0], self._a[1]) if n and n.get("threaded") else None

    def CheckSpelling(self, CustomDictionary=None, IgnoreUppercase=None, AlwaysSuggest=None, SpellLang=None):  # noqa: N802,N803
        """Review > Spelling over this range (the Spelling dialog). Application.CheckSpelling(word) checks one word
        without a dialog."""
        self.Select()
        _rq("ui.command", id="Spelling")

    def ConvertNotesToComments(self):  # noqa: N802
        """[YC] Turn the notes in the range into comment threads (Review > Convert to Comments); returns how many."""
        return sum(_rq("comment.convert", sheet=self._sheet, rect=list(a)) for a in self._areas)

    def NoteText(self, Text=None, Start=None, Length=None):  # noqa: N802,N803
        """The note's text, as Excel's (measured): without Text, Length characters from Start (1-based; both left out:
        all of it); with Text, Text replaces Length characters from Start (Length left out: to the end; Start left
        out: the whole note)."""
        n = _rq("note.get", sheet=self._sheet, row=self._a[0], col=self._a[1])
        cur = n["text"] if n else ""
        s = max(int(Start), 1) - 1 if Start is not None else 0
        if Text is None:
            return cur[s:s + int(Length)] if Length is not None else cur[s:]
        new = str(Text) if Start is None else cur[:s] + str(Text) + (cur[s + int(Length):] if Length is not None else "")
        _rq("note.set", sheet=self._sheet, row=self._a[0], col=self._a[1], text=new)
        return new

    def ClearComments(self):  # noqa: N802
        for a in self._areas:
            _rq("note.clear", sheet=self._sheet, rect=list(a))

    ClearNotes = ClearComments

    def DataSeries(self, Rowcol=None, Type=-4132, Date=1, Step=1, Stop=None, Trend=False):  # noqa: N802,N803
        """Home > Fill > Series: fill the range from its first cell. Rowcol xlRows / xlColumns (default: along the
        longer side); Type xlDataSeriesLinear, xlGrowth, xlChronological (Date: xlDay, xlWeekday, xlMonth, xlYear) or
        xlAutoFill; Step; Stop (optional). Trend (linear or growth): the best fit through the values at the start of
        each row / column, written over the whole range (the starting values too), as Excel's Series > Trend."""
        a = self._a
        if Rowcol is None:
            # down the columns when the range has more rows than columns, else along the rows (Excel 16, measured: a
            # square range goes along its rows)
            by_cols = (a[2] - a[0]) > (a[3] - a[1])
        else:
            by_cols = int(Rowcol) == 2
        t = int(Type)
        if t == 4:
            # xlAutoFill: Auto Fill from the rows (columns) up to the last one with anything in it, as Excel's
            # (measured: 1, 4 over five cells gives 1, 4, 7, 10, 13; 1, blank, 5 gives 1, blank, 5, 9)
            f = self._get("formula")
            grid = [list(row) for row in f] if isinstance(f, VArray) else [[f]]
            if by_cols:
                used = [any(x not in (None, "") for x in row) for row in grid]
            else:
                used = [any(row[k] not in (None, "") for row in grid) for k in range(len(grid[0]) if grid else 0)]
            lines = len(used)
            last = max((k for k, u in enumerate(used) if u), default=-1)
            if last < 0 or last >= lines - 1:
                return  # nothing to fill from, or nothing left to fill
            src = (a[0], a[1], a[0] + last, a[3]) if by_cols else (a[0], a[1], a[2], a[1] + last)
            Range._from(self._sheet, [src]).AutoFill(self, 0)
            return
        kind = {-4132: 0, 2: 1, 3: 2}.get(t)
        if kind is None:
            raise ValueError("Type is xlDataSeriesLinear, xlGrowth, xlChronological or xlAutoFill")
        p = dict(sheet=self._sheet, rect=list(a), by_cols=by_cols, type=kind, unit=max(0, int(Date or 1) - 1), step=float(Step),
                 trend=bool(Trend))
        if Stop is not None:
            p["stop"] = float(Stop)
        _rq("range.series", **p)

    def ExportImage(self, Filename=None, Headings=False, Gridlines=None, Scale=1.0):  # noqa: N802,N803
        """[YC] Home > Screenshot > Selection: the range as it looks (formats, conditional formats, charts and pictures
        on it; no selection marks), however large, as a PNG file, or on the Clipboard when Filename is None.
        Gridlines: None = as the sheet shows them; Scale 2 = twice the pixels. Returns width, height, path, note
        (e.g. "the first 3,000 rows") and scaled_down (a very large picture is drawn at a lower resolution)."""
        return _export_image("range", Filename, Headings, Gridlines, Scale, sheet=self._sheet, rect=list(self._a))

    def CopyPicture(self, Appearance=1, Format=-4147):  # noqa: N802,N803
        """Copy the range as it looks on the screen to the Clipboard, as a picture (to paste into other programs)."""
        _rq("ui.screenshot", path="", target="range", address=self.Address(External=True), sheet=self._sheet,
            include_headers=False, max_width=0, to_clipboard=True)

    @property
    def Hyperlinks(self):  # noqa: N802
        return _Hyperlinks(self._sheet, self)

    def ClearHyperlinks(self):  # noqa: N802
        for a in self._areas:
            _rq("link.clear", sheet=self._sheet, rect=list(a))

    # ---- Defined names ----
    @property
    def Name(self):  # noqa: N802
        """The defined name (Name object) that refers to this range; raises an error when there is none (like Excel)."""
        for n in Names._all():
            if not n["visible"]:
                continue
            if n["scope"] >= 0 and n["scope"] != self._sheet:
                continue
            body = n["refers_to"][1:]
            if "(" in body:
                continue  # dynamic names don't count
            r = _rq("names.resolve", name=n["name"] if n["scope"] < 0 else f"{_quote_sheet(n['scope_name'])}!{n['name']}", sheet=self._sheet)
            if r and r["sheet"] == self._sheet and len(self._areas) == 1 and tuple(r["rect"]) == self._clamped_area():
                return Name(n)
        raise ApplicationDefinedError("This range has no defined name")

    @Name.setter
    def Name(self, v):  # noqa: N802
        """rng.Name = "Price" (workbook level) or "Sheet1!Price" (sheet level)."""
        v = str(v)
        sheet_name, local = _split_sheet(v)
        if sheet_name is not None:
            Worksheets(sheet_name).Names.Add(Name=local, RefersTo=self)
        else:
            Names.Add(Name=v, RefersTo=self)

    def _clamped_area(self):
        a = self._a
        if a[2] == ROW_END:
            return (a[0], a[1], self.Worksheet.RowLimit - 1, a[3])
        return a

    def ListNames(self):  # noqa: N802
        """Paste a "name | refers to" list at the upper-left cell of this range (like Excel's Range.ListNames)."""
        rows = [[n["name"], "'" + n["refers_to"]] for n in Names._all() if n["visible"]]
        if rows:
            self.Cells(1, 1).Resize(len(rows), 2).Value = rows

    def CreateNames(self, Top=None, Left=None, Bottom=None, Right=None):  # noqa: N802,N803
        """Create names from the text in the top row, left column etc. (like Create from Selection). Detected from the contents when nothing is given (like Excel)."""
        if Top is None and Left is None and Bottom is None and Right is None:
            a = self._a
            first_row = self.Cells(1, 2 if a[3] > a[1] else 1).Value
            first_col = self.Cells(2 if a[2] > a[0] else 1, 1).Value
            Top = isinstance(first_row, str)
            Left = a[3] > a[1] and isinstance(first_col, str)
            if not Top and not Left:
                Top = True
        return _rq("names.create_from_selection", sheet=self._sheet, rect=list(self._a), top=bool(Top), left=bool(Left),
                   bottom=bool(Bottom), right=bool(Right))

    # ---- Special cells, formula auditing ----
    def SpecialCells(self, Type, Value=None):  # noqa: N802,N803
        """Like Excel: xlCellTypeBlanks, xlCellTypeConstants, xlCellTypeFormulas, xlCellTypeComments, xlCellTypeLastCell,
        xlCellTypeVisible, xlCellTypeAllFormatConditions, xlCellTypeSameFormatConditions, xlCellTypeAllValidation,
        xlCellTypeSameValidation (the cells sharing a rule with this single cell, or with the active cell when it is inside
        the range, else the range's first cell).
        Value (constants and formulas only): a combination of xlNumbers + xlTextValues + xlLogical + xlErrors.
        A single cell searches the used range of the whole sheet (notes, rules and validation: the whole sheet, as Go To
        Special does; visible cells: that cell, or the whole sheet's once rows or columns are hidden); raises "No cells were found." when nothing matches."""
        values = 1 | 2 | 4 | 16 if Value is None else int(Value)
        anchor = {}
        if int(Type) in (-4173, -4175):  # xlCellTypeSameFormatConditions, xlCellTypeSameValidation
            a0 = self._areas[0]
            r, c = a0[0], a0[1]
            if len(self._areas) > 1 or a0[0] != a0[2] or a0[1] != a0[3]:
                try:
                    ac = Application.ActiveCell
                    if ac is not None and ac._sheet == self._sheet and any(
                            x[0] <= ac._a[0] <= x[2] and x[1] <= ac._a[1] <= x[3] for x in self._areas):
                        r, c = ac._a[0], ac._a[1]
                except YcError:
                    pass
            anchor = {"anchor_row": r, "anchor_col": c}
        areas = []
        # one merged cell whole is one cell: the whole sheet, as a single cell (Excel 16, measured)
        mine = [(self._a[0], self._a[1], self._a[0], self._a[1])] if len(self._areas) == 1 and self._one_cell() else self._areas
        for a in mine:
            areas += [tuple(x) for x in _rq("range.special", sheet=self._sheet, rect=list(a), kind=int(Type), values=values, **anchor)]
            if len(self._areas) == 1 or Type == xlCellTypeLastCell:
                break
        if not areas:
            raise ApplicationDefinedError("No cells were found.")
        return Range._from(self._sheet, areas)

    def _differences(self, Comparison, rows):  # noqa: N803
        cmp = Comparison if isinstance(Comparison, Range) else Range(Comparison)
        areas = [tuple(x) for x in _rq("range.differences", sheet=self._sheet, rect=list(self._a), row=cmp._a[0], col=cmp._a[1], rows=rows)]
        if not areas:
            raise ApplicationDefinedError("No cells were found.")
        return Range._from(self._sheet, areas)

    def RowDifferences(self, Comparison):  # noqa: N802,N803
        """Go To Special > Row differences: in each row, the cells that differ from that row's cell in Comparison's
        column (formulas compare as formulas, the rest by value). A single cell searches its current region."""
        return self._differences(Comparison, True)

    def ColumnDifferences(self, Comparison):  # noqa: N802,N803
        """Go To Special > Column differences: in each column, the cells that differ from its cell in Comparison's row."""
        return self._differences(Comparison, False)

    def _trace(self, method, direct):
        areas = []
        for a in self._areas:
            areas += [tuple(x) for x in _rq(method, sheet=self._sheet, rect=list(a), direct=direct) if tuple(x) not in areas]
        if not areas:
            raise ApplicationDefinedError("No cells were found.")
        # a set of cells, as Excel gives it: no area inside another, touching ones joined (=A1+B1 traces $A$1:$B$1)
        inside = lambda a, b: b[0] <= a[0] and b[1] <= a[1] and a[2] <= b[2] and a[3] <= b[3]  # noqa: E731
        areas = [a for i, a in enumerate(areas) if not any(j != i and inside(a, b) and (a != b or j < i) for j, b in enumerate(areas))]
        return Range._from(self._sheet, _join_adjacent(areas))

    @property
    def Precedents(self):  # noqa: N802
        """All precedents (same sheet; indirect references included)."""
        return self._trace("range.precedents", False)

    @property
    def DirectPrecedents(self):  # noqa: N802
        """Direct precedents: the ranges these formulas reference directly."""
        return self._trace("range.precedents", True)

    @property
    def Dependents(self):  # noqa: N802
        """All dependents: formulas that use these cells directly or indirectly."""
        return self._trace("range.dependents", False)

    @property
    def DirectDependents(self):  # noqa: N802
        return self._trace("range.dependents", True)

    # ---- [YC] per-cell OnChange ----
    @property
    def OnChange(self):  # noqa: N802
        from . import host

        return host.onchange_handlers(self)

    @OnChange.setter
    def OnChange(self, handler):  # noqa: N802
        from . import host

        host.set_onchange(self, handler)

    def SetOnChange(self, handler, Sources=ycUser | ycPython):  # noqa: N802,N803
        """[YC] Range.OnChange = handler with a choice of sources, in any module: Sources=ycUser runs it for the user's
        edits only (code writes don't), ycPython for writes from Python, ycLive / ycCalc for live data and
        recalculation (or several with |). handler: a function or "Module.Function"; None removes the range's handlers."""
        from . import host

        host.set_onchange(self, handler, None if handler is None else int(Sources))
        return handler

    # ---- Iteration ----
    def __iter__(self):
        cx = getattr(self, "_cx", None)
        by = getattr(self, "_by", None)
        if by:  # For Each col In Columns("B:C"): whole columns (rows), as in Excel
            for a in self._areas:
                lines = range(a[1], a[3] + 1) if by == "cols" else range(a[0], (a[2] if a[2] != ROW_END else self.Worksheet.RowLimit - 1) + 1)
                for k in lines:
                    line = Range._from(self._sheet, [(a[0], k, a[2], k)] if by == "cols" else [(k, a[1], k, a[3])])
                    object.__setattr__(line, "_cx", cx)
                    yield line
            return
        for a in self._areas:
            r1 = a[2] if a[2] != ROW_END else self.Worksheet.RowLimit - 1
            for r in range(a[0], r1 + 1):
                for c in range(a[1], a[3] + 1):
                    cell = Range._from(self._sheet, [(r, c, r, c)])
                    object.__setattr__(cell, "_cx", cx)
                    yield cell

    def __len__(self):
        return self.Count


def _to_ns(v):
    if isinstance(v, _decimal.Decimal):
        return int((v * 1_000_000_000).to_integral_value(rounding=_decimal.ROUND_HALF_UP))
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return int(round(v * 1_000_000_000))
    return v


class _CellsAccessor:
    """Cells, Cells(r, c), Cells(Rows.Count, 1)."""

    def __init__(self, sheet_id, base):
        self._sheet = sheet_id
        self._base = base

    def __call__(self, RowIndex=None, ColumnIndex=None):  # noqa: N803
        if RowIndex is None:
            return Range._from(self._sheet, [(0, 0, ROW_END, MAX_COLS - 1)])
        if ColumnIndex is None:
            # Cells(n): the n-th cell in row-major order
            n = int(RowIndex) - 1
            ncols = (self._base[3] - self._base[1] + 1) if self._base else MAX_COLS
            RowIndex, ColumnIndex = n // ncols + 1, n % ncols + 1
        c = _col_index(ColumnIndex) if isinstance(ColumnIndex, str) else int(ColumnIndex) - 1
        r = int(RowIndex) - 1
        if self._base:
            r += self._base[0]
            c += self._base[1]
        if r < 0 or c < 0 or c >= MAX_COLS or not _rows_fit(self._sheet, r):
            raise ApplicationDefinedError("Cell position out of range")
        return Range._from(self._sheet, [(r, c, r, c)])

    Item = __call__

    def __getattr__(self, name):
        if name.startswith("_"):  # private names (the workbook binding looks up _cx) aren't the range's
            raise AttributeError(name)
        return getattr(self(), name)

    def __setattr__(self, name, value):
        if name.startswith("_"):
            object.__setattr__(self, name, value)
        else:
            setattr(self(), name, value)

    @property
    def Count(self):  # noqa: N802
        return Worksheet(self._sheet).RowLimit * MAX_COLS


class _RangeCells(_CellsAccessor):
    """Range.Cells: Cells() is the range (by cells, whatever Rows / Columns it came from), Cells(r, c) / Cells(n)
    count from its upper-left cell, Count is its number of cells."""

    def __init__(self, rng):
        super().__init__(rng._sheet, rng._a)
        self._r = rng

    def __call__(self, RowIndex=None, ColumnIndex=None):  # noqa: N803
        if RowIndex is None:
            r = Range._from(self._r._sheet, self._r._areas)
            object.__setattr__(r, "_cx", getattr(self._r, "_cx", None))
            return r
        return _CellsAccessor.__call__(self, RowIndex, ColumnIndex)

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return self()._cell_count()

    def __iter__(self):
        return iter(self())

    def __len__(self):
        return self.Count


class _RowsAccessor:
    def __init__(self, sheet_id):
        self._sheet = sheet_id

    def __call__(self, Index):  # noqa: N803
        if isinstance(Index, str):
            a = _parse_area(Index if ":" in Index else f"{Index}:{Index}")
            rng = Range._from(self._sheet, [(a[0], 0, a[2], MAX_COLS - 1)])
        else:
            r = int(Index) - 1
            if r < 0:
                raise ApplicationDefinedError(f"Rows({Index}): rows start at 1")
            rng = Range._from(self._sheet, [(r, 0, r, MAX_COLS - 1)])
        object.__setattr__(rng, "_by", "rows")
        return rng

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return Worksheet(self._sheet).RowLimit

    # Rows without an index is every row of the sheet: Rows.Hidden = False shows them all, as in Excel (the member
    # was kept on this object and nothing changed)
    def _all(self):
        rng = Range._from(self._sheet, [(0, 0, ROW_END, MAX_COLS - 1)])
        object.__setattr__(rng, "_by", "rows")
        return rng

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return getattr(self._all(), name)

    def __setattr__(self, name, value):
        if name.startswith("_"):
            object.__setattr__(self, name, value)
        else:
            setattr(self._all(), name, value)


class _ColsAccessor:
    def __init__(self, sheet_id):
        self._sheet = sheet_id

    def __call__(self, Index):  # noqa: N803
        if isinstance(Index, str):
            a = _parse_area(Index if ":" in Index else f"{Index}:{Index}")
            rng = Range._from(self._sheet, [(0, a[1], ROW_END, a[3])])
        else:
            c = int(Index) - 1
            if c < 0 or c >= MAX_COLS:
                raise ApplicationDefinedError(f"Columns({Index}): columns are 1 to {MAX_COLS}")
            rng = Range._from(self._sheet, [(0, c, ROW_END, c)])
        object.__setattr__(rng, "_by", "cols")
        return rng

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return MAX_COLS

    # Columns without an index is every column (Columns.Hidden = False, Columns.ColumnWidth = 12)
    def _all(self):
        rng = Range._from(self._sheet, [(0, 0, ROW_END, MAX_COLS - 1)])
        object.__setattr__(rng, "_by", "cols")
        return rng

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return getattr(self._all(), name)

    def __setattr__(self, name, value):
        if name.startswith("_"):
            object.__setattr__(self, name, value)
        else:
            setattr(self._all(), name, value)


class _RangeLines:
    """Range.Rows / Range.Columns: the range itself, counted, indexed and walked by rows (columns). Excel's
    Range("B2:C3").Rows.Delete deletes B2:C3 (the cells shift as for Range.Delete), its Address is $B$2:$C$3 and
    Rows.Value2 reads B2:C3; only the members that act on whole rows (columns) take them: Hidden, Group, Ungroup,
    OutlineLevel and ShowDetail (Range("B2:C3").Columns.Hidden hides columns B:C)."""

    _WHOLE = frozenset(("Hidden", "Group", "Ungroup", "OutlineLevel", "ShowDetail"))
    _entire = ""

    def __init__(self, rng):
        object.__setattr__(self, "_r", rng)

    def _target(self, name):
        if name in ("Group", "Ungroup"):  # the range's own lines decide first (Range._outline_dim)
            rng = Range._from(self._r._sheet, list(self._r._areas))
            object.__setattr__(rng, "_by", "rows" if self._entire == "EntireRow" else "cols")
            return rng
        return getattr(self._r, self._entire) if name in self._WHOLE else self._r

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        if name.startswith("_"):  # the range's own (_cx for the workbook binding, _a, _sheet)
            return getattr(object.__getattribute__(self, "_r"), name)
        return getattr(self._target(name), name)

    def __setattr__(self, name, value):
        setattr(self._target(name), name, value)


class _RangeRows(_RangeLines):
    _entire = "EntireRow"

    def __call__(self, i):
        a = self._r._a
        return Range._from(self._r._sheet, [(a[0] + i - 1, a[1], a[0] + i - 1, a[3])])

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return self._r._nrows(self._r._a)

    def __iter__(self):
        return (self(i) for i in range(1, self.Count + 1))

    def AutoFit(self):  # noqa: N802
        a = self._r._a
        _rq("dims.autofit", sheet=self._r._sheet, kind="row", first=a[0], last=a[2])


class _RangeCols(_RangeLines):
    _entire = "EntireColumn"

    def __call__(self, i):
        a = self._r._a
        return Range._from(self._r._sheet, [(a[0], a[1] + i - 1, a[2], a[1] + i - 1)])

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        a = self._r._a
        return a[3] - a[1] + 1

    def __iter__(self):
        return (self(i) for i in range(1, self.Count + 1))

    def AutoFit(self):  # noqa: N802
        a = self._r._a
        if a[0] == 0 and a[2] >= ROW_END:
            _rq("dims.autofit", sheet=self._r._sheet, kind="col", first=a[1], last=a[3])
        else:  # Excel: only the range's cells are measured
            _rq("dims.autofit", sheet=self._r._sheet, kind="col", first=a[1], last=a[3], row0=a[0], row1=a[2])


class _Areas:
    def __init__(self, rng):
        self._r = rng

    def __call__(self, i):
        return Range._from(self._r._sheet, [self._r._areas[i - 1]])

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return len(self._r._areas)

    def __iter__(self):
        return (self(i) for i in range(1, self.Count + 1))


def Intersect(*Ranges):  # noqa: N802,N803
    rs = [r for r in Ranges if r is not None]
    if not rs:
        return None
    sheet = rs[0]._sheet
    cur = list(rs[0]._areas)
    for r in rs[1:]:
        if r._sheet != sheet:
            raise ApplicationDefinedError("Intersect ranges must be on the same sheet")
        nxt = []
        for a in cur:
            for b in r._areas:
                r0, c0, r1, c1 = max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])
                if r0 <= r1 and c0 <= c1:
                    nxt.append((r0, c0, r1, c1))
        cur = nxt
        if not cur:
            return None
    return Range._from(sheet, cur)


def Union(*Ranges):  # noqa: N802,N803
    rs = [r for r in Ranges if r is not None]
    if not rs:
        return None
    sheet = rs[0]._sheet
    areas = []
    for r in rs:
        if r._sheet != sheet:
            raise ApplicationDefinedError("Union ranges must be on the same sheet")
        for a in r._areas:
            _union_add(areas, tuple(a))
    return Range._from(sheet, areas)


def _union_add(areas, b):
    """Add area b to a Union's areas as Excel 16 does (309 unions measured, 307 the same): the areas are searched from
    the last one back for a partner of b, one that holds b, lies inside it or makes one rectangle with it (the same
    columns and rows that touch or overlap, or the same rows); the partner is taken out, b becomes their union and
    goes on looking, and b comes last. So Union(A1, C3, A1) is $C$3,$A$1, Union(A1:A3, A2) $A$1:$A$3,
    Union(A1:B2, B1:C2) $A$1:$C$2, Union(C4:J7, A3:F9, D4:G4) $A$3:$F$9,$C$4:$J$7, while Union(A1:B2, B2:C3) keeps
    both (B2 counted twice)."""

    def inside(x, y):
        return y[0] <= x[0] and y[1] <= x[1] and x[2] <= y[2] and x[3] <= y[3]

    def joined(a, b):
        if a[1] == b[1] and a[3] == b[3] and a[0] <= b[2] + 1 and b[0] <= a[2] + 1:
            return (min(a[0], b[0]), a[1], max(a[2], b[2]), a[3])
        if a[0] == b[0] and a[2] == b[2] and a[1] <= b[3] + 1 and b[1] <= a[3] + 1:
            return (a[0], min(a[1], b[1]), a[2], max(a[3], b[3]))
        return None

    while True:
        for i in range(len(areas) - 1, -1, -1):
            a = areas[i]
            u = a if inside(b, a) else b if inside(a, b) else joined(a, b)
            if u:
                b = u
                del areas[i]
                break
        else:
            areas.append(b)
            return


def _join_adjacent(areas):
    """Excel's Union joins areas that touch into one block (A1 and A2 are $A$1:$A$2, cells added in a loop make one
    range) and keeps overlapping ones as they are (Union(A1:A2, A2:A3) counts A2 twice)."""
    areas = list(areas)
    joined = True
    while joined:
        joined = False
        for i in range(len(areas)):
            for j in range(len(areas)):
                if i == j:
                    continue
                a, b = areas[i], areas[j]
                if a[1] == b[1] and a[3] == b[3] and a[2] + 1 == b[0]:  # b right below a, as wide
                    areas[i] = (a[0], a[1], b[2], a[3])
                elif a[0] == b[0] and a[2] == b[2] and a[3] + 1 == b[1]:  # b right of a, as tall
                    areas[i] = (a[0], a[1], a[2], b[3])
                else:
                    continue
                del areas[j]
                joined = True
                break
            if joined:
                break
    return areas


# ---------------------------------------------------------------------------
# Font, Interior, Borders
# ---------------------------------------------------------------------------
def _theme_tag(index, tint=0.0) -> int:
    """A style color that refers to the theme (index: xlThemeColorDark1 ... xlThemeColorFollowedHyperlink)."""
    index = int(index)
    if not 1 <= index <= 12:
        raise ApplicationDefinedError(f"ThemeColor must be 1..12: {index}")
    t = int(round(max(-1.0, min(1.0, float(tint))) * 10000)) & 0xFFFF
    return 0xFE000000 | ((index - 1) << 16) | t


def _apply_tint(rgb: int, tint: float) -> int:
    """Excel's TintAndShade on a fixed color: lighter (> 0) or darker (< 0) in HSL luminance."""
    import colorsys

    r, g, b = ((rgb >> 16) & 0xFF) / 255, ((rgb >> 8) & 0xFF) / 255, (rgb & 0xFF) / 255
    h, lum, sat = colorsys.rgb_to_hls(r, g, b)
    lum = lum * (1 + tint) if tint < 0 else lum * (1 - tint) + tint
    r, g, b = colorsys.hls_to_rgb(h, max(0.0, min(1.0, lum)), sat)
    return (round(r * 255) << 16) | (round(g * 255) << 8) | round(b * 255)


def _theme_props(cls, color_key, theme_key):
    """ThemeColor and TintAndShade for Font / Interior (cls) over the color property color_key."""

    def get_theme(self):
        t = self._g().get(theme_key)
        return None if t is None else t[0] + 1

    def set_theme(self, v):
        if v is None:  # read from a color that isn't a theme color: nothing to change
            return
        t = self._g().get(theme_key)
        self._s(**{color_key: _theme_tag(v, t[1] if t else 0.0)})

    def get_tint(self):
        t = self._g().get(theme_key)
        return t[1] if t else 0.0

    def set_tint(self, v):
        g = self._g()
        t = g.get(theme_key)
        if t:
            self._s(**{color_key: _theme_tag(t[0] + 1, v)})
        elif g[color_key]:
            self._s(**{color_key: 0xFF000000 | _apply_tint(g[color_key] & 0xFFFFFF, float(v))})

    cls.ThemeColor = property(get_theme, set_theme, doc="xlThemeColorDark1 ... xlThemeColorFollowedHyperlink (None = a fixed color); it follows the theme")
    cls.TintAndShade = property(get_tint, set_tint, doc="-1 (darker) .. 1 (lighter)")


def _argb_to_bgr(argb: int) -> int:
    r, g, b = (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF
    return r | (g << 8) | (b << 16)


def _color_to_argb(v) -> int:
    if isinstance(v, str):
        s = v.lstrip("#")
        if len(s) != 6:
            raise TypeMismatchError(f"Colors must be #RRGGBB: {v}")
        return 0xFF000000 | int(s, 16)
    v = int(v)
    r, g, b = v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF
    return 0xFF000000 | (r << 16) | (g << 8) | b


def RGB(Red, Green, Blue):  # noqa: N802,N803
    return int(Red) | (int(Green) << 8) | (int(Blue) << 16)


# Excel's 56-color palette (ColorIndex 1-56, BGR)
_PALETTE = [0x000000, 0xFFFFFF, 0x0000FF, 0x00FF00, 0xFF0000, 0x00FFFF, 0xFF00FF, 0xFFFF00, 0x000080, 0x008000, 0x800000, 0x008080,
            0x800080, 0x808000, 0xC0C0C0, 0x808080, 0xFF9999, 0x663399, 0xCCFFFF, 0xFFFFCC, 0x660066, 0x8080FF, 0xCC6600, 0xFFCCCC,
            0x800000, 0xFF00FF, 0x00FFFF, 0xFFFF00, 0x800080, 0x000080, 0x808000, 0xFF0000, 0xFFCC00, 0xFFFFCC, 0xCCFFCC, 0x99FFFF,
            0xFFCC99, 0xCC99FF, 0xFF99CC, 0x99CCFF, 0xFF6633, 0xCCCC33, 0x00CC99, 0x00CCFF, 0x0099FF, 0x0066FF, 0x996666, 0x969696,
            0x663300, 0x669933, 0x003300, 0x003333, 0x003399, 0x663399, 0x993333, 0x333333]

_HALIGN_IN = {xlGeneral: 0, xlLeft: 1, xlCenter: 2, xlRight: 3, xlFill: 4, xlJustify: 5, xlCenterAcrossSelection: 6, xlDistributed: 7}
_HALIGN_OUT = {v: k for k, v in _HALIGN_IN.items()}
_VALIGN_IN = {xlBottom: 0, xlCenter: 1, xlTop: 2, xlJustify: 3, xlDistributed: 4}
_VALIGN_OUT = {v: k for k, v in _VALIGN_IN.items()}
_UNDERLINE_IN = {xlUnderlineStyleNone: 0, False: 0, None: 0, xlUnderlineStyleSingle: 1, True: 1, xlUnderlineStyleDouble: 2,
                 xlUnderlineStyleSingleAccounting: 3, xlUnderlineStyleDoubleAccounting: 4}
_UNDERLINE_OUT = {0: xlUnderlineStyleNone, 1: xlUnderlineStyleSingle, 2: xlUnderlineStyleDouble, 3: xlUnderlineStyleSingleAccounting,
                  4: xlUnderlineStyleDoubleAccounting}


class _StyleObj:
    __slots__ = ("_r",)

    def __init__(self, rng):
        object.__setattr__(self, "_r", rng)

    def __setattr__(self, name, value):
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"{type(self).__name__} has no settable property {name}")

    def _g(self):
        return self._r._style_get()

    def _s(self, **kw):
        self._r._style(**kw)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _DisplayRange:
    """What Font and Interior read for Range.DisplayFormat: the shown format, never written."""

    def __init__(self, rng):
        self._rng = rng

    def _style_get(self):
        a = self._rng._a
        return _rq("range.display_format", sheet=self._rng._sheet, row=a[0], col=a[1])

    def _style(self, **kw):
        raise ApplicationDefinedError("DisplayFormat is read-only: format the range itself")

    def __getattr__(self, name):
        return getattr(self._rng, name)


class _DisplayFormat:
    """Range.DisplayFormat (read-only): the format the sheet shows, conditional formats included."""

    def __init__(self, rng):
        self._dr = _DisplayRange(rng)

    def _d(self):
        return self._dr._style_get()

    @property
    def Font(self):  # noqa: N802
        return Font(self._dr)

    @property
    def Interior(self):  # noqa: N802
        return Interior(self._dr)

    @property
    def NumberFormat(self):  # noqa: N802
        return self._d()["numfmt"]

    NumberFormatLocal = NumberFormat

    @property
    def HorizontalAlignment(self):  # noqa: N802
        return _HALIGN_OUT.get(self._d()["halign"], xlGeneral)

    @property
    def VerticalAlignment(self):  # noqa: N802
        return _VALIGN_OUT.get(self._d()["valign"], xlBottom)

    @property
    def WrapText(self):  # noqa: N802
        return bool(self._d()["wrap"])

    @property
    def ShrinkToFit(self):  # noqa: N802
        return bool(self._d()["shrink"])

    @property
    def IndentLevel(self):  # noqa: N802
        return int(self._d()["indent"])

    @property
    def Orientation(self):  # noqa: N802
        return int(self._d()["rotation"])

    @property
    def Locked(self):  # noqa: N802
        return bool(self._d()["locked"])

    @property
    def FormulaHidden(self):  # noqa: N802
        return bool(self._d()["hidden"])

    @property
    def Style(self):  # noqa: N802
        return self._d()["cell_style"]


class Font(_StyleObj):
    @property
    def Name(self):  # noqa: N802
        return self._g()["font_name"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(font_name=str(v))

    @property
    def Size(self):  # noqa: N802
        return self._g()["font_size"]

    @Size.setter
    def Size(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(font_size=float(v))

    @property
    def Bold(self):  # noqa: N802
        return self._g()["bold"]

    @Bold.setter
    def Bold(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(bold=bool(v))

    @property
    def Italic(self):  # noqa: N802
        return self._g()["italic"]

    @Italic.setter
    def Italic(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(italic=bool(v))

    @property
    def Underline(self):  # noqa: N802
        u = self._g()["underline"]
        return None if u is None else _UNDERLINE_OUT[u]

    @Underline.setter
    def Underline(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(underline=_UNDERLINE_IN.get(v, 1))

    @property
    def Strikethrough(self):  # noqa: N802
        return self._g()["strike"]

    @Strikethrough.setter
    def Strikethrough(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(strike=bool(v))

    @property
    def Color(self):  # noqa: N802
        c = self._g()["font_color"]
        if c is None:
            return None
        return _argb_to_bgr(c) if c else 0

    @Color.setter
    def Color(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(font_color=_color_to_argb(v))

    @property
    def ColorIndex(self):  # noqa: N802
        c = self.Color
        if c is None:
            return None
        return _PALETTE.index(c) + 1 if c in _PALETTE else xlAutomatic

    @ColorIndex.setter
    def ColorIndex(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(font_color=0 if v in (xlAutomatic, xlNone) else _color_to_argb(_PALETTE[int(v) - 1]))

    @property
    def Superscript(self):  # noqa: N802
        s = self._g().get("script", 0)
        return None if s is None else s == 1  # None: some cells are, some aren't (Excel's Null; it read False)

    @Superscript.setter
    def Superscript(self, v):  # noqa: N802
        if v is None:  # Excel's Null read back: nothing changes
            return
        self._s(script=1 if v else 0)

    @property
    def Subscript(self):  # noqa: N802
        s = self._g().get("script", 0)
        return None if s is None else s == 2

    @Subscript.setter
    def Subscript(self, v):  # noqa: N802
        if v is None:
            return
        self._s(script=2 if v else 0)

    @property
    def FontStyle(self):  # noqa: N802
        g = self._g()
        if g["bold"] is None or g["italic"] is None:
            return None  # cells that differ: Excel's Null (it raised KeyError)
        return {(False, False): "Regular", (True, False): "Bold", (False, True): "Italic", (True, True): "Bold Italic"}[(bool(g["bold"]), bool(g["italic"]))]

    @FontStyle.setter
    def FontStyle(self, v):  # noqa: N802
        if v is None:  # Excel's Null read back: nothing changes
            return
        s = str(v).lower()
        self._s(bold="bold" in s or "\u7c97" in s, italic="italic" in s or "\u659c" in s)


class Characters:
    """Range.Characters(Start, Length): part of a cell's text, with its own Font (bold words, superscripts...)."""

    __slots__ = ("_r", "_start", "_len")

    def __init__(self, rng, Start=1, Length=None):  # noqa: N803
        object.__setattr__(self, "_r", rng)
        object.__setattr__(self, "_start", max(1, int(Start)))
        object.__setattr__(self, "_len", None if Length is None else max(0, int(Length)))

    def __setattr__(self, name, value):
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"Characters has no settable property {name}")

    def _cell(self):
        r = self._r._a
        return dict(sheet=self._r._sheet, row=r[0], col=r[1])

    def _span(self, text):
        n = len(text)
        a = min(self._start - 1, n)
        b = n if self._len is None else min(n, a + self._len)
        return a, b

    def _kw(self):
        kw = dict(self._cell(), start=self._start - 1)
        if self._len is not None:
            kw["length"] = self._len
        return kw

    @property
    def Text(self):  # noqa: N802
        t = _rq("rich.get", **self._cell())["text"]
        a, b = self._span(t)
        return t[a:b]

    @Text.setter
    def Text(self, v):  # noqa: N802
        _rq("rich.replace", text=str(v), **self._kw())

    Caption = Text

    @property
    def Count(self):  # noqa: N802
        t = _rq("rich.get", **self._cell())["text"]
        a, b = self._span(t)
        return b - a

    def Insert(self, String):  # noqa: N802,N803
        """Replace these characters with String (Length=0 inserts)."""
        _rq("rich.replace", text=str(String), **self._kw())

    def Delete(self):  # noqa: N802
        _rq("rich.replace", text="", **self._kw())

    @property
    def Font(self):  # noqa: N802
        return _CharFont(self)


class _CharFont:
    """The Font of Characters: a property reads None when the characters mix values (like Excel)."""

    __slots__ = ("_c",)

    def __init__(self, chars):
        object.__setattr__(self, "_c", chars)

    def __setattr__(self, name, value):
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"Font has no settable property {name}")

    def _apply(self, **props):
        _rq("rich.apply", props=props, **self._c._kw())

    def _get(self, key):
        c = self._c
        d = _rq("rich.get", **c._cell())
        cell = c._r._style_get(raw=True)
        base = {"font": cell["font_name"], "size": cell["font_size"], "color": cell["font_color"], "bold": cell["bold"], "italic": cell["italic"],
                "strike": cell["strike"], "underline": cell["underline"], "script": cell.get("script", 0)}
        runs = d["runs"]
        a, b = c._span(d["text"])
        seen = set()
        for i in range(a, max(b, a + 1)):
            run = None
            for x in runs:
                if x["start"] <= i:
                    run = x
            v = base[key]
            if run is not None:
                rv = run[key]
                if key in ("bold", "italic", "strike"):
                    v = bool(rv) if rv >= 0 else v
                elif key in ("underline", "script"):
                    v = rv if rv >= 0 else v
                elif key == "font":
                    v = rv or v
                elif rv:
                    v = rv
            seen.add(v)
            if len(seen) > 1:
                return None
        return seen.pop() if seen else base[key]

    @property
    def Name(self):  # noqa: N802
        return self._get("font")

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._apply(font=str(v))

    @property
    def Size(self):  # noqa: N802
        return self._get("size")

    @Size.setter
    def Size(self, v):  # noqa: N802
        self._apply(size=float(v))

    @property
    def Bold(self):  # noqa: N802
        return self._get("bold")

    @Bold.setter
    def Bold(self, v):  # noqa: N802
        self._apply(bold=bool(v))

    @property
    def Italic(self):  # noqa: N802
        return self._get("italic")

    @Italic.setter
    def Italic(self, v):  # noqa: N802
        self._apply(italic=bool(v))

    @property
    def Strikethrough(self):  # noqa: N802
        return self._get("strike")

    @Strikethrough.setter
    def Strikethrough(self, v):  # noqa: N802
        self._apply(strike=bool(v))

    @property
    def Underline(self):  # noqa: N802
        u = self._get("underline")
        return None if u is None else _UNDERLINE_OUT[u]

    @Underline.setter
    def Underline(self, v):  # noqa: N802
        self._apply(underline=_UNDERLINE_IN.get(v, 1))

    @property
    def Color(self):  # noqa: N802
        c = self._get("color")
        return None if c is None else (_argb_to_bgr(c) if c else 0)

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._apply(color=_color_to_argb(v))

    @property
    def Superscript(self):  # noqa: N802
        s = self._get("script")
        return None if s is None else s == 1

    @Superscript.setter
    def Superscript(self, v):  # noqa: N802
        self._apply(script=1 if v else 0)

    @property
    def Subscript(self):  # noqa: N802
        s = self._get("script")
        return None if s is None else s == 2

    @Subscript.setter
    def Subscript(self, v):  # noqa: N802
        self._apply(script=2 if v else 0)

    @property
    def FontStyle(self):  # noqa: N802
        b, i = self.Bold, self.Italic
        if b is None or i is None:
            return None
        return {(False, False): "Regular", (True, False): "Bold", (False, True): "Italic", (True, True): "Bold Italic"}[(b, i)]

    @FontStyle.setter
    def FontStyle(self, v):  # noqa: N802
        s = str(v).lower()
        self._apply(bold="bold" in s, italic="italic" in s)


class Interior(_StyleObj):
    @property
    def Color(self):  # noqa: N802
        c = self._g()["fill_color"]
        if c is None:
            return None
        return _argb_to_bgr(c) if c else 16777215

    @Color.setter
    def Color(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(fill_color=0 if v == xlNone else _color_to_argb(v))

    @property
    def ColorIndex(self):  # noqa: N802
        c = self._g()["fill_color"]
        if c is None:
            return None
        if not c:
            return xlNone
        bgr = _argb_to_bgr(c)
        return _PALETTE.index(bgr) + 1 if bgr in _PALETTE else xlNone

    @ColorIndex.setter
    def ColorIndex(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        self._s(fill_color=0 if v in (xlNone, xlAutomatic) else _color_to_argb(_PALETTE[int(v) - 1]))

    @property
    def Pattern(self):  # noqa: N802
        g = self._g()
        if any(k in g and g[k] is None for k in ("gradient", "pattern", "fill_color")):
            return None  # cells that differ
        if g.get("gradient"):
            return xlPatternLinearGradient if g["gradient"] == 1 else xlPatternRectangularGradient  # from the center or a corner
        if g.get("pattern"):
            return _PATTERN_OUT[g["pattern"]]
        return xlSolid if g["fill_color"] else xlNone

    @Pattern.setter
    def Pattern(self, v):  # noqa: N802
        if v is None:  # Excel's Null (cells that differ) read back: nothing changes
            return
        if v == xlNone:
            self._s(fill_color=0, pattern=0, gradient=0)
        elif v == xlPatternLinearGradient:
            self._s(gradient=1)
        elif v == xlPatternRectangularGradient:
            self._s(gradient=2)
        elif v in (xlSolid, xlPatternAutomatic):
            self._s(pattern=0, gradient=0, fill_solid=True)  # a cell without a fill turns white, as in Excel
        elif v in _PATTERN_IN:
            self._s(pattern=_PATTERN_IN[v], gradient=0)
        else:
            raise ApplicationDefinedError(f"Unknown pattern: {v}")

    @property
    def PatternColor(self):  # noqa: N802
        c = self._g().get("pattern_color", 0)
        if c is None:  # cells that differ: Excel's Null (it read 0)
            return None
        return _argb_to_bgr(c) if c else 0

    @PatternColor.setter
    def PatternColor(self, v):  # noqa: N802
        if v is None:  # Excel's Null read back: nothing changes (it cleared every cell's pattern color)
            return
        self._s(pattern_color=0 if v == xlAutomatic else _color_to_argb(v))

    @property
    def PatternColorIndex(self):  # noqa: N802
        c = self.PatternColor
        if c is None:
            return None
        return _PALETTE.index(c) + 1 if c in _PALETTE else xlAutomatic

    @PatternColorIndex.setter
    def PatternColorIndex(self, v):  # noqa: N802
        if v is None:
            return
        self._s(pattern_color=0 if v in (xlAutomatic, xlNone) else _color_to_argb(_PALETTE[int(v) - 1]))

    @property
    def Gradient(self):  # noqa: N802
        """The gradient of a fill whose Pattern is xlPatternLinearGradient or xlPatternRectangularGradient."""
        return _Gradient(self)


class _ColorStop:
    def __init__(self, grad, index):
        self._grad, self._i = grad, index

    @property
    def Color(self):  # noqa: N802
        g = self._grad._i._g()
        c = g["fill_color"] if self._i == 0 else g.get("gradient_color2", 0)
        return _argb_to_bgr(c) if c else 16777215

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._grad._i._s(**{"fill_color" if self._i == 0 else "gradient_color2": _color_to_argb(v)})

    @property
    def Position(self):  # noqa: N802
        return float(self._i)


class _ColorStops:
    """Two stops are kept, like Excel's two-color gradients: position 0 (fill color) and 1 (second color)."""

    def __init__(self, grad):
        self._grad = grad

    def Add(self, Position):  # noqa: N802,N803
        return _ColorStop(self._grad, 0 if float(Position) < 0.5 else 1)

    def Clear(self):  # noqa: N802
        pass

    def Item(self, Index):  # noqa: N802,N803
        if Index not in (1, 2):
            raise SubscriptOutOfRangeError()
        return _ColorStop(self._grad, int(Index) - 1)

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    @property
    def Count(self):  # noqa: N802
        return 2


class _Gradient:
    def __init__(self, interior):
        self._i = interior

    @property
    def Degree(self):  # noqa: N802
        return float(self._i._g().get("gradient_angle", 0))

    @Degree.setter
    def Degree(self, v):  # noqa: N802
        self._i._s(gradient_angle=int(round(float(v) / 45.0)) * 45 % 360)

    @property
    def ColorStops(self):  # noqa: N802
        return _ColorStops(self)

    # a rectangular gradient's focus (Fill Effects > From corner / From center): left = right and top = bottom, each
    # 0 or 1 for a corner, all 0.5 for the center
    def _rect(self):
        g = self._i._g()
        if g.get("gradient") == 3:
            k = int(g.get("gradient_angle", 0)) % 4
            x, y = (1.0 if k in (1, 3) else 0.0), (1.0 if k >= 2 else 0.0)
            return [x, y, x, y]
        return [0.5, 0.5, 0.5, 0.5]

    def _set_rect(self, i, v):
        r = self._rect()
        r[i] = float(v)
        if i in (0, 2):
            r[0] = r[2] = float(v)
        else:
            r[1] = r[3] = float(v)
        if r[0] in (0.0, 1.0) and r[1] in (0.0, 1.0):
            self._i._s(gradient=3, gradient_angle=(1 if r[0] == 1.0 else 0) + (2 if r[1] == 1.0 else 0))
        else:
            self._i._s(gradient=2, gradient_angle=0)

    @property
    def RectangleLeft(self):  # noqa: N802
        return self._rect()[0]

    @RectangleLeft.setter
    def RectangleLeft(self, v):  # noqa: N802
        self._set_rect(0, v)

    @property
    def RectangleTop(self):  # noqa: N802
        return self._rect()[1]

    @RectangleTop.setter
    def RectangleTop(self, v):  # noqa: N802
        self._set_rect(1, v)

    @property
    def RectangleRight(self):  # noqa: N802
        return self._rect()[2]

    @RectangleRight.setter
    def RectangleRight(self, v):  # noqa: N802
        self._set_rect(2, v)

    @property
    def RectangleBottom(self):  # noqa: N802
        return self._rect()[3]

    @RectangleBottom.setter
    def RectangleBottom(self, v):  # noqa: N802
        self._set_rect(3, v)


# Excel's XlPattern <-> internal pattern ids (model::FillPattern)
_PATTERN_IN = {xlPatternGray75: 1, xlPatternGray50: 2, xlPatternGray25: 3, xlPatternGray16: 4, xlPatternGray8: 5, xlPatternHorizontal: 6,
               xlPatternVertical: 7, xlPatternDown: 8, xlPatternUp: 9, xlPatternChecker: 10, xlPatternSemiGray75: 11, xlPatternLightHorizontal: 12,
               xlPatternLightVertical: 13, xlPatternLightDown: 14, xlPatternLightUp: 15, xlPatternGrid: 16, xlPatternCrissCross: 17}
_PATTERN_OUT = {v: k for k, v in _PATTERN_IN.items()}


# borders: Excel's LineStyle + Weight <-> the 13 internal line styles
_EDGE_IN = {xlEdgeLeft: "left", xlEdgeTop: "top", xlEdgeRight: "right", xlEdgeBottom: "bottom", xlDiagonalDown: "diag_down",
            xlDiagonalUp: "diag_up", xlInsideVertical: "inside_v", xlInsideHorizontal: "inside_h"}


def _line_to_internal(style, weight):
    # internal: 0 none, 1 thin, 2 medium, 3 dashed, 4 dotted, 5 thick, 6 double, 7 hairline, 8 medium dashed, 9 dash-dot, 10 medium dash-dot, 11 dash-dot-dot, 12 medium dash-dot-dot, 13 slanted dash-dot
    if style in (xlLineStyleNone, xlNone, None):
        return 0
    if style == xlDouble:
        return 6
    if style == xlDash:
        return 8 if weight == xlMedium else 3
    if style == xlDot:
        return 4
    if style == xlDashDot:
        return 10 if weight == xlMedium else 9
    if style == xlDashDotDot:
        return 12 if weight == xlMedium else 11
    if style == xlSlantDashDot:
        return 13
    return {xlHairline: 7, xlThin: 1, xlMedium: 2, xlThick: 5}.get(weight, 1)


# the weights each broken line style comes in (Format Cells' line styles); Weight set to another one makes the line
# continuous, as Excel does (measured: xlDot then xlHairline -> xlContinuous xlHairline, xlDouble then xlThin ->
# xlContinuous xlThin; xlDash then xlMedium stays dashed)
_LINE_WEIGHTS = {xlDash: (xlThin, xlMedium), xlDashDot: (xlThin, xlMedium), xlDashDotDot: (xlThin, xlMedium), xlDot: (xlThin,),
                 xlDouble: (xlThick,), xlSlantDashDot: (xlMedium,)}


_INTERNAL_TO_LINE = {0: (xlLineStyleNone, xlThin), 1: (xlContinuous, xlThin), 2: (xlContinuous, xlMedium), 3: (xlDash, xlThin),
                     4: (xlDot, xlThin), 5: (xlContinuous, xlThick), 6: (xlDouble, xlThick), 7: (xlContinuous, xlHairline),
                     8: (xlDash, xlMedium), 9: (xlDashDot, xlThin), 10: (xlDashDot, xlMedium), 11: (xlDashDotDot, xlThin),
                     12: (xlDashDotDot, xlMedium), 13: (xlSlantDashDot, xlMedium)}


_theme_props(Font, "font_color", "font_theme")
_theme_props(Interior, "fill_color", "fill_theme")


class Border:
    def __init__(self, rng, edges):
        object.__setattr__(self, "_r", rng)
        object.__setattr__(self, "_edges", edges)

    def __setattr__(self, name, value):
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"Border has no settable property {name}")

    def _get(self):
        """The edge of the range (as in Excel: Borders(xlEdgeBottom) of B2:C3 is the bottom of row 3, not of B2); None
        as the style when its cells differ."""
        edge = self._edges[0]
        idx = {"left": 0, "top": 1, "right": 2, "bottom": 3, "diag_down": 4, "diag_up": 5, "inside_v": 2, "inside_h": 3}[edge]
        r = self._r
        try:
            nr, nc = r.Rows.Count, r.Columns.Count
        except Exception:  # noqa: BLE001
            nr = nc = 1
        # an outer edge is the line the cell shows: its own, or the facing edge of the cell beside it (Excel 16,
        # measured: B3's top is B2's bottom); an inside edge is the cells' own (G4:G5's inside line stays None there)
        key = "shown_borders" if edge in ("left", "top", "right", "bottom") else "borders"

        def side(st):
            return (st.get(key) or st["borders"])[idx]

        if (nr == 1 and nc == 1) or edge in ("diag_down", "diag_up"):
            return side(r._style_get())
        cap = 64  # a whole column or row: its first cells say it
        rows, cols = range(1, min(nr, cap) + 1), range(1, min(nc, cap) + 1)
        cells = {"left": [(i, 1) for i in rows], "right": [(i, nc) for i in rows], "top": [(1, j) for j in cols],
                 "bottom": [(nr, j) for j in cols],
                 "inside_v": [(i, j) for i in rows for j in cols if j < nc][:cap],
                 "inside_h": [(i, j) for i in rows for j in cols if i < nr][:cap]}[edge]
        if not cells:  # one row has no inside horizontal border
            return [0, 0]
        got = [side(r.Cells(i, j)._style_get()) for i, j in cells]
        first = got[0]
        return first if all(list(g) == list(first) for g in got) else [None, 0]

    def _set(self, style=None, weight=None, color=None):
        cur_style, cur_weight = _INTERNAL_TO_LINE.get(self._get()[0], (xlContinuous, xlThin))
        if style is None and weight is not None and weight not in _LINE_WEIGHTS.get(cur_style, (weight,)):
            cur_style = xlContinuous  # Excel: a weight the style has no line for makes it continuous (xlDot, then Weight = xlHairline)
        st = _line_to_internal(cur_style if style is None else style, cur_weight if weight is None else weight)
        if style is None and weight is not None and self._get()[0] == 0:
            st = _line_to_internal(xlContinuous, weight)
        self._r._border_set(list(self._edges), st, 0 if color is None else color)

    @property
    def LineStyle(self):  # noqa: N802
        st = self._get()[0]
        return None if st is None else _INTERNAL_TO_LINE.get(st, (xlContinuous, xlThin))[0]  # None: the cells differ

    @LineStyle.setter
    def LineStyle(self, v):  # noqa: N802
        self._set(style=v)

    @property
    def Weight(self):  # noqa: N802
        st = self._get()[0]
        return None if st is None else _INTERNAL_TO_LINE.get(st, (xlContinuous, xlThin))[1]

    @Weight.setter
    def Weight(self, v):  # noqa: N802
        self._set(weight=v)

    @property
    def Color(self):  # noqa: N802
        c = self._get()[1]
        return _argb_to_bgr(c) if c else 0

    @Color.setter
    def Color(self, v):  # noqa: N802
        st = self._get()[0] or 1
        self._r._border_set(list(self._edges), st, _color_to_argb(v))

    @property
    def ThemeColor(self):  # noqa: N802
        t = self._get()[2] if len(self._get()) > 2 else None
        return None if t is None else t[0] + 1

    @ThemeColor.setter
    def ThemeColor(self, v):  # noqa: N802
        if v is None:
            return
        g = self._get()
        t = g[2] if len(g) > 2 else None
        self._r._border_set(list(self._edges), g[0] or 1, _theme_tag(v, t[1] if t else 0.0))

    @property
    def TintAndShade(self):  # noqa: N802
        g = self._get()
        return g[2][1] if len(g) > 2 and g[2] else 0.0

    @TintAndShade.setter
    def TintAndShade(self, v):  # noqa: N802
        g = self._get()
        t = g[2] if len(g) > 2 else None
        if t:
            self._r._border_set(list(self._edges), g[0] or 1, _theme_tag(t[0] + 1, v))
        elif g[1]:
            self._r._border_set(list(self._edges), g[0] or 1, 0xFF000000 | _apply_tint(g[1] & 0xFFFFFF, float(v)))

    @property
    def ColorIndex(self):  # noqa: N802
        return xlAutomatic

    @ColorIndex.setter
    def ColorIndex(self, v):  # noqa: N802
        if v not in (xlAutomatic, xlNone):
            self.Color = _PALETTE[int(v) - 1]


class Borders:
    def __init__(self, rng):
        self._r = rng

    def __call__(self, Index):  # noqa: N803
        if Index not in _EDGE_IN:
            raise SubscriptOutOfRangeError(f"Invalid border index: {Index}")
        return Border(self._r, [_EDGE_IN[Index]])

    Item = __call__
    Count = 6

    def __iter__(self):
        return (self(i) for i in (xlEdgeLeft, xlEdgeTop, xlEdgeBottom, xlEdgeRight, xlDiagonalDown, xlDiagonalUp))

    def _all(self):
        return Border(self._r, ["left", "top", "right", "bottom", "inside_v", "inside_h"])

    @property
    def LineStyle(self):  # noqa: N802
        return self(xlEdgeLeft).LineStyle

    @LineStyle.setter
    def LineStyle(self, v):  # noqa: N802
        self._all().LineStyle = v

    @property
    def Weight(self):  # noqa: N802
        return self(xlEdgeLeft).Weight

    @Weight.setter
    def Weight(self, v):  # noqa: N802
        self._all().Weight = v

    @property
    def Color(self):  # noqa: N802
        return self(xlEdgeLeft).Color

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._all().Color = v


# ---------------------------------------------------------------------------
# Window (view state, through the UI command ring)
# ---------------------------------------------------------------------------
class Window:
    def __init__(self, sheet_id):
        object.__setattr__(self, "_sheet", sheet_id)

    def __setattr__(self, name, value):
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"Window has no settable property {name}")

    def _v(self):
        return _rq("view.get", sheet=self._sheet)

    def _set(self, **kw):
        _rq("view.set", sheet=self._sheet, **kw)

    @property
    def ScrollRow(self):  # noqa: N802
        return self._v()["scroll_row"] + 1

    @ScrollRow.setter
    def ScrollRow(self, v):  # noqa: N802
        self._set(scroll_row=int(v) - 1)

    @property
    def ScrollColumn(self):  # noqa: N802
        return self._v()["scroll_col"] + 1

    @ScrollColumn.setter
    def ScrollColumn(self, v):  # noqa: N802
        self._set(scroll_col=int(v) - 1)

    @property
    def Zoom(self):  # noqa: N802
        return round(self._v()["zoom"] * 100)

    @Zoom.setter
    def Zoom(self, v):  # noqa: N802
        if v is True:  # Excel: the selection fills the window (View > Zoom to Selection)
            _rq("ui.command", id="ZoomToSelection")
            return
        if not 10 <= float(v) <= 400:  # Excel refuses it (error 1004) rather than clamp it
            raise ApplicationDefinedError("Unable to set the Zoom property: the zoom must be from 10 to 400")
        self._set(zoom=int(v) / 100.0)

    @property
    def View(self):  # noqa: N802
        """xlNormalView (1), xlPageBreakPreview (2) or xlPageLayoutView (3)."""
        return (1, 2, 3)[self._v().get("page_view", 0)]

    @View.setter
    def View(self, v):  # noqa: N802
        m = {1: 0, 2: 1, 3: 2}.get(int(v))
        if m is None:
            raise ValueError("View must be xlNormalView, xlPageBreakPreview or xlPageLayoutView")
        self._set(page_view=m)

    @property
    def Split(self):  # noqa: N802
        """View -> Split: panes that each scroll on their own (SplitRow / SplitColumn give their size)."""
        return bool(self._v().get("split"))

    @Split.setter
    def Split(self, v):  # noqa: N802
        self._set(split=bool(v))

    @property
    def FreezePanes(self):  # noqa: N802
        g = self._v()
        f = g["freeze"]
        return bool(f[0] or f[1]) and not g.get("split")

    @FreezePanes.setter
    def FreezePanes(self, v):  # noqa: N802
        self._set(freeze="active" if v else "none")

    @property
    def CenterColumns(self):  # noqa: N802
        """[YC] T layout (an option chain: calls | strike | puts): these columns stay in the middle of the window, the
        columns on their left are right-aligned against them, and one horizontal scroll moves both sides outward.
        "K", "K:L" or a Range sets it (not column A: something has to be on the left); None ends it. Rows frozen with
        SplitRow / FreezePanes stay frozen on top. Returns "K:K" style text, or None."""
        c = self._v().get("center")
        return None if not c else f"{_col_name(c[0])}:{_col_name(c[0] + c[1] - 1)}"

    @CenterColumns.setter
    def CenterColumns(self, v):  # noqa: N802
        if v is None or v == "" or v is False:
            self._set(center=None)
            return
        if isinstance(v, Range):
            c0, c1 = v._a[1], v._a[3]
        else:
            text = str(v).replace("$", "").strip()
            a = _parse_area(text if ":" in text else f"{text}:{text}")
            c0, c1 = a[1], a[3]
        if c0 <= 0:
            raise ApplicationDefinedError("CenterColumns: column A can't be in the middle (nothing would be on its left)")
        self._set(center=[c0, c1 - c0 + 1])

    @property
    def SplitRow(self):  # noqa: N802
        return self._v()["freeze"][0]

    @SplitRow.setter
    def SplitRow(self, v):  # noqa: N802
        self._split_at(int(v), None)

    def _split_at(self, rows, cols):
        """SplitRow / SplitColumn, as Excel 16 does (measured): a frozen window stays frozen at the new place, any
        other window is split there (FreezePanes False, Split True)."""
        g = self._v()
        f = g["freeze"]
        frozen = bool(f[0] or f[1]) and not g.get("split")
        new = [f[0] if rows is None else rows, f[1] if cols is None else cols]
        self._set(freeze=new)
        if not frozen and (new[0] or new[1]):
            self._set(split=True)

    @property
    def SplitColumn(self):  # noqa: N802
        return self._v()["freeze"][1]

    @SplitColumn.setter
    def SplitColumn(self, v):  # noqa: N802
        self._split_at(None, int(v))

    @property
    def DisplayRuler(self):  # noqa: N802
        """View > Ruler: the rulers of Page Layout view."""
        return bool(_rq("layout.window")["ruler"])

    @DisplayRuler.setter
    def DisplayRuler(self, v):  # noqa: N802
        _rq("layout.window", ruler=bool(v))

    @property
    def Visible(self):  # noqa: N802
        """View > Hide / Unhide of the workbook's window."""
        return bool(_rq("layout.window")["main_visible"])

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        _rq("layout.window", window=0, visible=bool(v))

    @property
    def DisplayGridlines(self):  # noqa: N802
        return self._v()["gridlines"]

    @DisplayGridlines.setter
    def DisplayGridlines(self, v):  # noqa: N802
        self._set(gridlines=bool(v))

    @property
    def DisplayHeadings(self):  # noqa: N802
        return self._v()["headings"]

    @DisplayHeadings.setter
    def DisplayHeadings(self, v):  # noqa: N802
        self._set(headings=bool(v))

    @property
    def DisplayZeros(self):  # noqa: N802
        """File > Options > Advanced: "Show a zero in cells that have zero value" for this sheet (saved with the workbook)."""
        return self._v()["zeros"]

    @DisplayZeros.setter
    def DisplayZeros(self, v):  # noqa: N802
        self._set(zeros=bool(v))

    @property
    def GridlineColor(self):  # noqa: N802
        """The sheet's gridline color as an RGB value (RGB(r, g, b)); 0 = Automatic (the theme's gray)."""
        c = int(self._v()["gridline_color"])
        return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF)

    @GridlineColor.setter
    def GridlineColor(self, v):  # noqa: N802
        v = int(v)
        self._set(gridline_color=((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF))

    @property
    def DisplayWorkbookTabs(self):  # noqa: N802
        """The sheet tabs (Options > Advanced > Display options for this workbook)."""
        return self._v()["sheet_tabs"]

    @DisplayWorkbookTabs.setter
    def DisplayWorkbookTabs(self, v):  # noqa: N802
        self._set(sheet_tabs=bool(v))

    @property
    def DisplayHorizontalScrollBar(self):  # noqa: N802
        return self._v()["hscroll"]

    @DisplayHorizontalScrollBar.setter
    def DisplayHorizontalScrollBar(self, v):  # noqa: N802
        self._set(hscroll=bool(v))

    @property
    def DisplayVerticalScrollBar(self):  # noqa: N802
        return self._v()["vscroll"]

    @DisplayVerticalScrollBar.setter
    def DisplayVerticalScrollBar(self, v):  # noqa: N802
        self._set(vscroll=bool(v))

    @property
    def DisplayFormulas(self):  # noqa: N802
        return self._v()["formulas"]

    @DisplayFormulas.setter
    def DisplayFormulas(self, v):  # noqa: N802
        self._set(formulas=bool(v))

    @property
    def DisplayOutline(self):  # noqa: N802
        """The outline's bars and level buttons (Ctrl+8)."""
        return bool(_rq("cells.display_outline", sheet=self._sheet))

    @DisplayOutline.setter
    def DisplayOutline(self, v):  # noqa: N802
        _rq("cells.display_outline", sheet=self._sheet, value=bool(v))

    @property
    def SelectedSheets(self):  # noqa: N802
        """The selected (grouped) sheets; one sheet when none are grouped."""
        ids = _rq("cells.group_get")
        return _SheetGroup([_sheet_object(s) for i in ids for s in _sheets_info() if s["id"] == i])

    @property
    def VisibleRange(self):  # noqa: N802
        v = self._v()
        return Range._from(v["sheet"], [tuple(v["visible"])])

    @property
    def ActiveCell(self):  # noqa: N802
        return Application.ActiveCell

    @property
    def Selection(self):  # noqa: N802
        return Application.Selection

    RangeSelection = Selection

    @property
    def ActiveSheet(self):  # noqa: N802
        return Application.ActiveSheet

    @property
    def Floating(self):  # noqa: N802
        return self._v()["floating"]

    @Floating.setter
    def Floating(self, v):  # noqa: N802
        _rq("view.float_sheet", id=self._v()["sheet"], floating=bool(v))

    def SmallScroll(self, Down=0, Up=0, ToRight=0, ToLeft=0):  # noqa: N802,N803
        v = self._v()
        self._set(scroll_row=max(0, v["scroll_row"] + Down - Up), scroll_col=max(0, v["scroll_col"] + ToRight - ToLeft))

    def LargeScroll(self, Down=0, Up=0, ToRight=0, ToLeft=0):  # noqa: N802,N803
        v = self._v()
        vr = v["visible"]
        page_r, page_c = max(1, vr[2] - vr[0]), max(1, vr[3] - vr[1])
        self._set(scroll_row=max(0, v["scroll_row"] + (Down - Up) * page_r), scroll_col=max(0, v["scroll_col"] + (ToRight - ToLeft) * page_c))

    def Activate(self):  # noqa: N802
        if self._sheet is not None:
            Worksheet(self._sheet).Activate()

    def NewWindow(self):  # noqa: N802
        """View > New Window: a second window on this workbook."""
        _rq("ui.command", id="NewWindow")
        return self

    def ActivateNext(self):  # noqa: N802
        """The next window of View > Switch Windows (Ctrl+F6, Ctrl+Tab)."""
        _rq("ui.command", id="NextWindow")

    def ActivatePrevious(self):  # noqa: N802
        """The previous window of View > Switch Windows (Ctrl+Shift+F6, Ctrl+Shift+Tab)."""
        _rq("ui.command", id="PrevWindow")

    # ---- The window: its caption, state, place and panes (Excel 16's rules, measured) ----
    @property
    def Caption(self):  # noqa: N802
        """The workbook's part of the title bar ("Book1"); setting it replaces that part ("" leaves only the
        application's name), None puts the file's name back."""
        return _rq("app.window")["win_caption"]

    @Caption.setter
    def Caption(self, v):  # noqa: N802
        if v is None:
            _rq("app.window", win_caption_reset=True)  # (a None argument isn't sent)
        else:
            _rq("app.window", win_caption=str(v))

    @property
    def WindowState(self):  # noqa: N802
        """xlMaximized, xlMinimized or xlNormal: the YCGrid window's (it holds the workbook's windows)."""
        return Application.WindowState

    @WindowState.setter
    def WindowState(self, v):  # noqa: N802
        Application.WindowState = v

    @property
    def Left(self):  # noqa: N802
        """The window's position and size on the screen, in points (the YCGrid window's, as Excel 16 reads its
        workbook windows)."""
        return Application.Left

    @Left.setter
    def Left(self, v):  # noqa: N802
        Application.Left = v

    @property
    def Top(self):  # noqa: N802
        return Application.Top

    @Top.setter
    def Top(self, v):  # noqa: N802
        Application.Top = v

    @property
    def Width(self):  # noqa: N802
        return Application.Width

    @Width.setter
    def Width(self, v):  # noqa: N802
        Application.Width = v

    @property
    def Height(self):  # noqa: N802
        return Application.Height

    @Height.setter
    def Height(self, v):  # noqa: N802
        Application.Height = v

    @property
    def UsableWidth(self):  # noqa: N802
        """The width (points) the cells have in the window, without the headings and scroll bars."""
        return float(_rq("app.window")["cells_width"])

    @property
    def UsableHeight(self):  # noqa: N802
        return float(_rq("app.window")["cells_height"])

    def Close(self, SaveChanges=None, Filename=None, RouteWorkbook=None):  # noqa: N802,N803
        """Closing the workbook's window closes the workbook (SaveChanges and Filename as Workbook.Close)."""
        return ActiveWorkbook.Close(SaveChanges, Filename)

    @property
    def Panes(self):  # noqa: N802
        """The window's panes: one, or two or four when it is split or frozen (Panes(1) is the top left one; in four,
        2 is the bottom left, 3 the top right, 4 the bottom right, as Excel numbers them)."""
        return Panes(self)

    @property
    def ActivePane(self):  # noqa: N802
        """The pane that holds the active cell."""
        g = self._v()
        r, c = g["active"]
        for p in Panes(self):
            a = p._area(g)
            if a[0] <= r <= a[2] and a[1] <= c <= a[3]:
                return p
        return Pane(self, 1)

    def ScrollIntoView(self, Left, Top, Width, Height, Start=True):  # noqa: N802,N803
        """Scrolls so the rectangle (points from the sheet's top left) is in view; Start keeps its top-left corner in
        view, False its bottom-right."""
        self._set(into_view=[float(Left), float(Top), float(Width), float(Height), bool(Start)])


class Panes:
    """Window.Panes: one pane, or two or four of a split or frozen window."""

    def __init__(self, window):
        object.__setattr__(self, "_w", window)

    def _layout(self, g=None):
        g = g or self._w._v()
        rows, cols = g["freeze"]
        return bool(rows), bool(cols)

    @property
    def Count(self):  # noqa: N802
        rows, cols = self._layout()
        return (2 if rows else 1) * (2 if cols else 1)

    def Item(self, Index):  # noqa: N802,N803
        i = int(Index)
        if not 1 <= i <= self.Count:
            raise SubscriptOutOfRangeError()
        return Pane(self._w, i)

    __call__ = Item

    def __iter__(self):
        return (Pane(self._w, i) for i in range(1, self.Count + 1))

    def __len__(self):
        return self.Count


class Pane:
    """One pane of a window (Window.Panes(i), ActivePane): what it shows and its own scrolling. A split or frozen
    window's top panes share their rows, its left panes their columns."""

    def __init__(self, window, index):
        object.__setattr__(self, "_w", window)
        object.__setattr__(self, "_i", int(index))

    def __setattr__(self, name, value):
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"Pane has no settable property {name}")

    def __eq__(self, other):
        return isinstance(other, Pane) and other._i == self._i and other._w._sheet == self._w._sheet

    def __hash__(self):
        return hash(("pane", self._i, self._w._sheet))

    @property
    def Index(self):  # noqa: N802
        return self._i

    def _place(self, g, scroll=False):
        """(bottom, right): whether this pane is in the lower row of panes and in the right column. In four panes of
        a split window Excel numbers them down then across (1 top left, 2 bottom left, 3 top right, 4 bottom right)
        for what they show, while ScrollRow / ScrollColumn of panes 2 and 3 read and set each other's; in four frozen
        panes across then down (2 is the top right), each pane's ScrollRow / ScrollColumn its own (Excel 16, measured
        both ways; gaps_fuzz seed 1151 found the frozen ones numbered as a split window's)."""
        rows, cols = bool(g["freeze"][0]), bool(g["freeze"][1])
        i = self._i
        if rows and cols:
            if not g.get("split"):
                return i in (3, 4), i in (2, 4)
            if scroll and i in (2, 3):
                i = 5 - i
            return i in (2, 4), i in (3, 4)
        if rows:
            return i == 2, False
        if cols:
            return False, i == 2
        return True, True  # one pane: the window's scrolling part

    def _area(self, g=None):
        g = g or self._w._v()
        top, left, first_row, first_col = g["panes"]
        rows, cols = g["freeze"]
        vis = g["visible"]
        bottom, right = self._place(g)
        if rows and not bottom:
            r0, r1 = top, top + rows - 1
        else:
            r0, r1 = (first_row if rows else vis[0]), vis[2]
        if cols and not right:
            c0, c1 = left, left + cols - 1
        else:
            c0, c1 = (first_col if cols else vis[1]), vis[3]
        return r0, c0, max(r0, r1), max(c0, c1)

    @property
    def VisibleRange(self):  # noqa: N802
        g = self._w._v()
        return Range._from(g["sheet"], [self._area(g)])

    def _origin(self, g):
        top, left, first_row, first_col = g["panes"]
        rows, cols = g["freeze"]
        bottom, right = self._place(g, scroll=True)
        row = (first_row if bottom else top) if rows else g["scroll_row"]
        col = (first_col if right else left) if cols else g["scroll_col"]
        return row, col, bottom, right

    @property
    def ScrollRow(self):  # noqa: N802
        return self._origin(self._w._v())[0] + 1

    @ScrollRow.setter
    def ScrollRow(self, v):  # noqa: N802
        g = self._w._v()
        _, _, bottom, _ = self._origin(g)
        if not g["freeze"][0]:
            self._w._set(scroll_row=int(v) - 1)
        elif not g.get("split"):
            # frozen: the frozen rows stay, and the panes below scroll whichever pane is asked (Excel 16, measured)
            self._w._set(lower_row=int(v) - 1)
        else:
            self._w._set(**{("lower_row" if bottom else "top_row"): int(v) - 1})

    @property
    def ScrollColumn(self):  # noqa: N802
        return self._origin(self._w._v())[1] + 1

    @ScrollColumn.setter
    def ScrollColumn(self, v):  # noqa: N802
        g = self._w._v()
        _, _, _, right = self._origin(g)
        if not g["freeze"][1]:
            self._w._set(scroll_col=int(v) - 1)
        elif not g.get("split"):
            # frozen: Panes(1).ScrollColumn = 17 scrolls the panes on the right to column Q (Excel 16, measured)
            self._w._set(right_col=int(v) - 1)
        else:
            self._w._set(**{("right_col" if right else "left_col"): int(v) - 1})

    def SmallScroll(self, Down=0, Up=0, ToRight=0, ToLeft=0):  # noqa: N802,N803
        self.ScrollRow = max(1, self.ScrollRow + int(Down) - int(Up))
        self.ScrollColumn = max(1, self.ScrollColumn + int(ToRight) - int(ToLeft))

    def LargeScroll(self, Down=0, Up=0, ToRight=0, ToLeft=0):  # noqa: N802,N803
        a = self._area()
        page_r, page_c = max(1, a[2] - a[0] + 1), max(1, a[3] - a[1] + 1)
        self.ScrollRow = max(1, self.ScrollRow + (int(Down) - int(Up)) * page_r)
        self.ScrollColumn = max(1, self.ScrollColumn + (int(ToRight) - int(ToLeft)) * page_c)

    def ScrollIntoView(self, Left, Top, Width, Height, Start=True):  # noqa: N802,N803
        self._w.ScrollIntoView(Left, Top, Width, Height, Start)

    def Activate(self):  # noqa: N802
        """Makes this pane the active one: the active cell moves to its top-left visible cell unless it is already
        in the pane."""
        g = self._w._v()
        a = self._area(g)
        r, c = g["active"]
        if not (a[0] <= r <= a[2] and a[1] <= c <= a[3]):
            self._w._set(select=[[a[0], a[1], a[0], a[1]]])
        return True


class _MultiThreadedCalculation:
    """Application.MultiThreadedCalculation: Enabled, ThreadMode and ThreadCount (File > Options > Advanced >
    Formulas; saved for this computer). ThreadCount is the threads used: every processor but two (the UI's and the
    Python host's) in xlThreadModeAutomatic."""

    def __setattr__(self, name, value):
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"MultiThreadedCalculation has no settable property {name}")

    @staticmethod
    def _o(**kw):
        return _rq("app.edit_options", **kw)

    @property
    def Enabled(self):  # noqa: N802
        return bool(self._o()["multi_threaded_calc"])

    @Enabled.setter
    def Enabled(self, v):  # noqa: N802
        self._o(multi_threaded_calc=bool(v))

    @property
    def ThreadMode(self):  # noqa: N802
        return xlThreadModeManual if self._o()["calc_thread_count"] else xlThreadModeAutomatic

    @ThreadMode.setter
    def ThreadMode(self, v):  # noqa: N802
        if int(v) == xlThreadModeAutomatic:
            self._o(calc_thread_count=0)
        elif not self._o()["calc_thread_count"]:
            self._o(calc_thread_count=int(self._o()["pool_threads"]))

    @property
    def ThreadCount(self):  # noqa: N802
        o = self._o()
        return int(o["calc_thread_count"] or o["pool_threads"])

    @ThreadCount.setter
    def ThreadCount(self, v):  # noqa: N802
        n = int(v)
        if not 1 <= n <= 1024:
            raise ApplicationDefinedError("ThreadCount must be from 1 to 1024")
        self._o(calc_thread_count=n)


class _FileDialogFilters:
    def __init__(self):
        self._f = [("All Files", "*.*")]

    def Add(self, Description, Extensions, Position=None):  # noqa: N802,N803
        item = (str(Description), str(Extensions))
        if Position is None:
            self._f.append(item)
        else:
            self._f.insert(max(0, int(Position) - 1), item)
        return item

    def Clear(self):  # noqa: N802
        self._f = []

    def Delete(self, filter=None):  # noqa: N802,A002
        if filter is None:
            self._f = []
        else:
            del self._f[int(filter) - 1]

    @property
    def Count(self):  # noqa: N802
        return len(self._f)

    def Item(self, Index):  # noqa: N802,N803
        return self._f[int(Index) - 1]

    __call__ = Item

    def __iter__(self):
        return iter(self._f)

    def _text(self):
        return ",".join(f"{d} ({e}),{e}" for d, e in self._f) or "All Files (*.*),*.*"


class _FileDialog:
    """Application.FileDialog(type): Title, InitialFileName, AllowMultiSelect, Filters, FilterIndex, ButtonName;
    Show() returns -1 when the user chose (SelectedItems holds the paths), 0 when cancelled; Execute() opens the
    chosen workbook (msoFileDialogOpen) or saves under the chosen name (msoFileDialogSaveAs)."""

    def __init__(self, kind):
        if kind not in (msoFileDialogOpen, msoFileDialogSaveAs, msoFileDialogFilePicker, msoFileDialogFolderPicker):
            raise ApplicationDefinedError("FileDialog: the type must be msoFileDialogOpen, SaveAs, FilePicker or FolderPicker")
        self.DialogType = kind
        self.Title = {1: "Open", 2: "Save As", 3: "Browse", 4: "Browse"}[kind]
        self.InitialFileName = ""
        self.AllowMultiSelect = False
        self.ButtonName = ""
        self.FilterIndex = 1
        self.Filters = _FileDialogFilters()
        self.SelectedItems = _SelectedItems([])

    def Show(self):  # noqa: N802
        save, folder = self.DialogType == msoFileDialogSaveAs, self.DialogType == msoFileDialogFolderPicker
        multi = bool(self.AllowMultiSelect) and not save and not folder
        r = _rq("app.file_dialog", save=save, folder=folder, multi=multi, filter=self.Filters._text(),
                index=int(self.FilterIndex or 1), title=str(self.Title or ""), initial=str(self.InitialFileName or ""),
                button=str(self.ButtonName or ""))
        if r is None or r == [] or r == "":
            self.SelectedItems = _SelectedItems([])
            return 0
        self.SelectedItems = _SelectedItems(list(r) if isinstance(r, list) else [r])
        return -1

    def Execute(self):  # noqa: N802
        if not self.SelectedItems.Count:
            return
        path = self.SelectedItems(1)
        if self.DialogType == msoFileDialogOpen:
            for p in self.SelectedItems:
                Workbooks.Open(p)
        elif self.DialogType == msoFileDialogSaveAs:
            ActiveWorkbook.SaveAs(path)


class _SelectedItems:
    def __init__(self, items):
        self._items = list(items)

    @property
    def Count(self):  # noqa: N802
        return len(self._items)

    def Item(self, Index):  # noqa: N802,N803
        return self._items[int(Index) - 1]

    __call__ = Item

    def __iter__(self):
        return iter(self._items)

    def __len__(self):
        return len(self._items)


# Application.International: what YCGrid reads and writes, which is US Excel's (the separators, the order of dates,
# the currency); Excel itself answers with the machine's regional settings
_INTERNATIONAL = {
    1: 1, 2: 1, 3: ".", 4: ",", 5: ",", 6: "R", 7: "C", 8: "r", 9: "c", 10: "[", 11: "]", 12: "{", 13: "}", 14: ",",
    15: ";", 16: "@", 17: "/", 18: ":", 19: "y", 20: "m", 21: "d", 22: "h", 23: "m", 24: "s", 25: "$", 26: "General",
    27: 2, 28: 0, 29: 2, 30: 3, 31: 3, 32: 0, 33: False, 34: False, 35: False, 36: False, 37: True, 38: False,
    39: True, 40: True, 41: False, 42: False, 43: True, 44: True, 45: False,
}

# Application.MacroOptions' Category numbers (Excel's): the Insert Function categories
_UDF_CATEGORIES = {1: "Financial", 2: "Date & Time", 3: "Math & Trig", 4: "Statistical", 5: "Lookup & Reference",
                   6: "Database", 7: "Text", 8: "Logical", 9: "Information", 14: "User Defined", 15: "Engineering"}


class _Windows:
    _STYLES = {xlArrangeStyleTiled: 0, xlArrangeStyleHorizontal: 1, xlArrangeStyleVertical: 2, xlArrangeStyleCascade: 3}

    def Arrange(self, ArrangeStyle=xlArrangeStyleTiled, ActiveWorkbook=False, SyncHorizontal=None, SyncVertical=None):  # noqa: N802,N803
        """View > Arrange All: the workbook's windows (and, unless ActiveWorkbook, the other YCGrid windows) tiled,
        horizontal, vertical or cascaded."""
        _rq("layout.window", arrange=self._STYLES.get(int(ArrangeStyle), 0), active_workbook=bool(ActiveWorkbook))
        return True

    def CompareSideBySideWith(self, WindowName=None):  # noqa: N802,N803
        """View > View Side by Side: with WindowName, another YCGrid's workbook (its file name; Synchronous Scrolling
        then scrolls both YCGrids together); without it, the workbook's second window."""
        if WindowName:
            _rq("layout.window", side_by_side=True, **{"with": str(WindowName)})
        else:
            _rq("layout.window", side_by_side=True)
        return True

    def BreakSideBySide(self):  # noqa: N802
        _rq("layout.window", side_by_side=False)
        return True

    def ResetPositionsSideBySide(self):  # noqa: N802
        """View > Reset Window Position."""
        _rq("layout.window", reset_positions=True)

    @property
    def SyncScrollingSideBySide(self):  # noqa: N802
        """View > Synchronous Scrolling."""
        return bool(_rq("layout.window")["sync_scrolling"])

    @SyncScrollingSideBySide.setter
    def SyncScrollingSideBySide(self, v):  # noqa: N802
        _rq("layout.window", sync_scrolling=bool(v))

    def __call__(self, Index=1):  # noqa: N803
        if isinstance(Index, int):
            sheets = _sheets_info()
            return Window(sheets[Index - 1]["id"])
        return Window(Worksheets(Index)._id)

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return len(_sheets_info())

    def __iter__(self):
        return (Window(s["id"]) for s in _sheets_info())


Windows = _Windows()


# ---------------------------------------------------------------------------
# Names
# ---------------------------------------------------------------------------
class Name:
    """A defined name (like Excel's Name object)."""

    def __init__(self, info):
        object.__setattr__(self, "_n", info["name"])
        object.__setattr__(self, "_scope", info["scope"])

    def __repr__(self):
        return f"<Name {self.Name}={self.RefersTo}>"

    def __eq__(self, other):
        return isinstance(other, Name) and other._n.lower() == self._n.lower() and other._scope == self._scope

    def __hash__(self):
        return hash((self._n.lower(), self._scope))

    def __setattr__(self, name, value):
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset is not None:
            prop.fset(self, value)
        else:
            raise AttributeError(f"Name has no settable property {name}")

    def _info(self):
        for n in Names._all():
            if n["name"].lower() == self._n.lower() and n["scope"] == self._scope:
                return n
        raise ApplicationDefinedError(f"The name no longer exists: {self._n}")

    def _update(self, **kw):
        _rq("names.update", old_name=self._n, old_scope=self._scope, **kw)

    @property
    def Name(self):  # noqa: N802
        """Sheet-level names carry the sheet prefix, e.g. "Sheet1!Price" (like Excel)."""
        n = self._info()
        return f"{_quote_sheet(n['scope_name'])}!{n['name']}" if n["scope"] >= 0 else n["name"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        """Rename: formulas that use the name are updated too (like Excel)."""
        _, local = _split_sheet(str(v))
        self._update(name=local)
        object.__setattr__(self, "_n", local)

    NameLocal = Name

    @property
    def RefersTo(self):  # noqa: N802
        return self._info()["refers_to"]

    @RefersTo.setter
    def RefersTo(self, v):  # noqa: N802
        self._update(refers_to=_refers_text(v))

    RefersToLocal = RefersTo

    @property
    def RefersToR1C1(self):  # noqa: N802
        info = self._info()
        if "refers_to_r1c1" in info:
            return info["refers_to_r1c1"]
        from . import _r1c1

        return _r1c1.a1_to_r1c1(info["refers_to"], 0, 0)

    @RefersToR1C1.setter
    def RefersToR1C1(self, v):  # noqa: N802
        self._update(refers_to=str(v) if str(v).startswith("=") else "=" + str(v), r1c1=True)

    RefersToR1C1Local = RefersToR1C1

    @property
    def RefersToRange(self):  # noqa: N802
        """The range the name refers to (dynamic names work too); raises an error when it isn't a range."""
        r = _rq("names.resolve", name=self._n, sheet=self._scope if self._scope >= 0 else _active_sheet_id())
        if not r:
            raise ApplicationDefinedError(f"The name \"{self._n}\" doesn't refer to a range")
        # a union (=Data!$A$1,Data!$A$3) is a range of several areas, as in Excel
        return Range._from(r["sheet"], [tuple(a) for a in r.get("areas") or [r["rect"]]])

    @property
    def Value(self):  # noqa: N802
        """Like Excel: Name.Value is the refers-to formula text (="=Sheet1!$A$1")."""
        return self.RefersTo

    @Value.setter
    def Value(self, v):  # noqa: N802
        self.RefersTo = v

    @property
    def Visible(self):  # noqa: N802
        return self._info()["visible"]

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._update(visible=bool(v))

    @property
    def Comment(self):  # noqa: N802
        return self._info()["comment"]

    @Comment.setter
    def Comment(self, v):  # noqa: N802
        self._update(comment=str(v))

    @property
    def Index(self):  # noqa: N802
        lst = Names._sorted(Names._all())
        for i, n in enumerate(lst, 1):
            if n["name"].lower() == self._n.lower() and n["scope"] == self._scope:
                return i
        return 0

    @property
    def Parent(self):  # noqa: N802
        return Worksheet(self._scope) if self._scope >= 0 else ThisWorkbook

    @property
    def WorkbookParameter(self):  # noqa: N802
        return False

    @property
    def MacroType(self):  # noqa: N802
        return xlNone

    def Delete(self):  # noqa: N802
        _rq("names.delete", name=self._n, scope=self._scope)


def _refers_text(v) -> str:
    """Value for Names.Add / RefersTo -> formula text (a Range becomes an absolute address with the sheet; constants get =)."""
    if isinstance(v, Range):
        return "=" + v.Address(True, True, xlA1, True)
    if isinstance(v, bool):
        return "=" + ("TRUE" if v else "FALSE")
    if isinstance(v, (int, float)):
        return "=" + repr(v)
    t = str(v)
    if t.startswith("="):
        return t
    # text without =: a string constant, like Excel
    return '="' + t.replace('"', '""') + '"'


class _Names:
    """Collection of names. Workbook.Names / Application.Names: every name; Worksheet.Names: the sheet-level names of that sheet."""

    def __init__(self, sheet_id=None):
        self._sheet = sheet_id

    @staticmethod
    def _all():
        return _rq("names.list") or []

    @staticmethod
    def _sorted(lst):
        return sorted(lst, key=lambda n: (n["name"].lower(), n["scope"]))

    def _list(self):
        lst = self._all()
        if self._sheet is not None:
            lst = [n for n in lst if n["scope"] == self._sheet]
        return self._sorted(lst)

    def _find(self, key):
        key = str(key)
        sheet_name, local = _split_sheet(key)
        lst = self._list()
        if sheet_name is not None:
            sid = Worksheets(sheet_name)._id
            hit = [n for n in lst if n["scope"] == sid and n["name"].lower() == local.lower()]
        else:
            hit = [n for n in lst if n["name"].lower() == key.lower()]
            # same name: a sheet collection -> that sheet; the workbook collection -> workbook level first, then the active sheet
            hit.sort(key=lambda n: (n["scope"] != (self._sheet if self._sheet is not None else -1), n["scope"] != _active_sheet_id()))
        return hit[0] if hit else None

    def __call__(self, Index=None, IndexLocal=None, RefersTo=None):  # noqa: N803
        if RefersTo is not None:
            target = _refers_text(RefersTo).replace("'", "").lower()
            for n in self._list():
                if n["refers_to"].replace("'", "").lower() == target:
                    return Name(n)
            raise SubscriptOutOfRangeError(f"No name refers to {RefersTo}")
        key = Index if Index is not None else IndexLocal
        if isinstance(key, int) and not isinstance(key, bool):
            lst = self._list()
            if not 1 <= key <= len(lst):
                raise SubscriptOutOfRangeError()
            return Name(lst[key - 1])
        n = self._find(key)
        if n is None:
            raise SubscriptOutOfRangeError(f"Name not found: {key}")
        return Name(n)

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def __len__(self):
        return self.Count

    def __iter__(self):
        return (Name(n) for n in self._list())

    def __contains__(self, key):
        return self._find(key) is not None

    def Add(self, Name=None, RefersTo=None, Visible=True, MacroType=None, ShortcutKey=None, Category=None, NameLocal=None,  # noqa: N802,N803
            RefersToLocal=None, CategoryLocal=None, RefersToR1C1=None, RefersToR1C1Local=None, Comment=None):
        """Add a name (replaces a name with the same name and scope, like Excel). Name may be "Sheet1!Price" to create a sheet-level name.
        RefersTo may be a Range, an address formula ("=Sheet1!$A$1:$A$10"), a constant or a formula ("=0.05", "=OFFSET(...)")."""
        name = Name if Name is not None else NameLocal
        if not name:
            raise ApplicationDefinedError("Names.Add needs Name")
        scope = self._sheet if self._sheet is not None else -1
        sheet_name, local = _split_sheet(str(name))
        if sheet_name is not None:
            scope = Worksheets(sheet_name)._id
            name = local
        if RefersTo is None:
            RefersTo = RefersToLocal
        r1c1 = False
        if RefersTo is None and (RefersToR1C1 is not None or RefersToR1C1Local is not None):
            t = str(RefersToR1C1 if RefersToR1C1 is not None else RefersToR1C1Local)
            RefersTo = t if t.startswith("=") else "=" + t
            r1c1 = True
        if RefersTo is None:
            raise ApplicationDefinedError("Names.Add needs RefersTo")
        # relative references are relative to the active cell, as in Excel (=Sheet1!A1 with B1 active: the cell left)
        params = dict(name=str(name), refers_to=str(RefersTo) if r1c1 else _refers_text(RefersTo), scope=scope,
                      visible=bool(Visible))
        if r1c1:
            params["r1c1"] = True
        if Comment is not None:
            params["comment"] = str(Comment)
        _rq("names.add", **params)
        return self(f"{_quote_sheet(Worksheet(scope).Name)}!{name}" if scope >= 0 else str(name))


Names = _Names()


# ---------------------------------------------------------------------------
# WorksheetFunction
# ---------------------------------------------------------------------------
def _literal(v) -> str:
    if isinstance(v, Range):
        return v.Address(External=True)
    if v is None:
        return ""
    if isinstance(v, bool):
        return "TRUE" if v else "FALSE"
    if isinstance(v, (int, float)):
        return repr(float(v)) if isinstance(v, float) else str(v)
    if isinstance(v, _decimal.Decimal):
        return f'DECIMAL("{v}")'
    if isinstance(v, _dt.datetime):
        return repr(datetime_to_serial(v))
    if isinstance(v, XlError):
        return v.Text
    if isinstance(v, (list, tuple)) or hasattr(v, "tolist"):
        rows = v.tolist() if hasattr(v, "tolist") else v
        if rows and not isinstance(rows[0], (list, tuple)):
            rows = [rows]
        return "{" + ";".join(",".join(_literal(x) for x in row) for row in rows) + "}"
    return '"' + str(v).replace('"', '""') + '"'


_worksheet_functions: Optional[frozenset] = None


def _is_worksheet_function(name: str) -> bool:
    """Whether Application.<name> / WorksheetFunction.<name> names a worksheet function (asked once from YCGrid)."""
    global _worksheet_functions
    if _worksheet_functions is None:
        try:
            _worksheet_functions = frozenset(str(n).upper() for n in _rq("app.functions"))
        except YcError:
            return True  # not connected yet, or an older YCGrid: the call itself reports an unknown name (#NAME?)
    fn = name.upper().replace("_", ".")
    return fn in _worksheet_functions or fn.replace(".", "") in _worksheet_functions or fn.replace(".", "_") in _worksheet_functions


def _call_function(name: str, args, raise_errors: bool):
    fn = name.upper().replace("_", ".")
    if fn in ("NORM.S.DIST", "NORM.DIST", "NORM.INV", "NORM.S.INV", "STDEV.S", "STDEV.P", "VAR.S", "VAR.P", "PERCENTILE.INC",
              "PERCENTILE.EXC", "RANK.EQ", "RANK.AVG", "MODE.SNGL", "CEILING.MATH", "FLOOR.MATH", "COVARIANCE.S", "QUARTILE.INC"):
        pass
    elif "." in fn and fn.replace(".", "") in ("VLOOKUP", "HLOOKUP"):
        fn = fn.replace(".", "")
    text = "=" + fn + "(" + ",".join(_literal(a) for a in args) + ")"
    v = _from_wire(_rq("eval", formula=text, sheet=None))
    if raise_errors and isinstance(v, XlError):
        raise ApplicationDefinedError(f"WorksheetFunction.{name} returned the error value {v.Text}")
    return v


class _WorksheetFunction:
    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)

        def fn(*args):
            return _call_function(name, args, raise_errors=True)

        return fn


WorksheetFunction = _WorksheetFunction()

# ---------------------------------------------------------------------------
# global objects
# ---------------------------------------------------------------------------
class _CellFormatFont:
    def __init__(self, cf):
        object.__setattr__(self, "_cf", cf)

    _KEYS = {"Bold": "bold", "Italic": "italic", "Strikethrough": "strike", "Name": "font_name", "Size": "font_size"}

    def __getattr__(self, name):
        cf = object.__getattribute__(self, "_cf")
        if name in _CellFormatFont._KEYS:
            return cf._d.get(_CellFormatFont._KEYS[name])
        if name == "Underline":
            u = cf._d.get("underline")
            return None if u is None else _UNDERLINE_OUT.get(u, xlUnderlineStyleNone)
        if name == "Color":
            c = cf._d.get("font_color")
            return None if c is None else (_argb_to_bgr(c) if c else 0)
        if name in ("Superscript", "Subscript"):
            sc = cf._d.get("script")
            return None if sc is None else sc == (1 if name == "Superscript" else 2)
        raise AttributeError(name)

    def __setattr__(self, name, v):
        cf = object.__getattribute__(self, "_cf")
        if name in ("Bold", "Italic", "Strikethrough"):
            cf._d[_CellFormatFont._KEYS[name]] = bool(v)
        elif name == "Name":
            cf._d["font_name"] = str(v)
        elif name == "Size":
            cf._d["font_size"] = float(v)
        elif name == "Underline":
            cf._d["underline"] = _UNDERLINE_IN.get(v, 1 if v else 0)
        elif name == "Color":
            cf._d["font_color"] = _color_to_argb(v)
        elif name in ("Superscript", "Subscript"):
            cf._d["script"] = (1 if name == "Superscript" else 2) if v else 0
        else:
            raise AttributeError(name)


class _CellFormatInterior:
    def __init__(self, cf):
        object.__setattr__(self, "_cf", cf)

    def __getattr__(self, name):
        cf = object.__getattribute__(self, "_cf")
        if name == "Color":
            c = cf._d.get("fill_color")
            return None if c is None else (_argb_to_bgr(c) if c else 16777215)
        if name == "Pattern":
            p = cf._d.get("pattern")
            return None if p is None else (_PATTERN_OUT[p] if p else xlSolid)
        if name == "PatternColor":
            c = cf._d.get("pattern_color")
            return None if c is None else _argb_to_bgr(c)
        raise AttributeError(name)

    def __setattr__(self, name, v):
        cf = object.__getattribute__(self, "_cf")
        if name == "Color":
            cf._d["fill_color"] = _color_to_argb(v)
            cf._d.setdefault("pattern", 0)
        elif name == "ColorIndex":
            cf._d["fill_color"] = 0 if v in (xlNone, xlAutomatic) else _color_to_argb(_PALETTE[int(v) - 1])
        elif name == "Pattern":
            cf._d["pattern"] = 0 if v in (xlSolid, xlPatternAutomatic) else _PATTERN_IN.get(v, 0)
            if v in (xlNone, None):
                cf._d["fill_color"] = 0
        elif name == "PatternColor":
            cf._d["pattern_color"] = _color_to_argb(v)
        else:
            raise AttributeError(name)


class _CellFormatBorder:
    def __init__(self, cf, edge):
        object.__setattr__(self, "_cf", cf)
        object.__setattr__(self, "_e", edge)

    def _side(self):
        cf, e = object.__getattribute__(self, "_cf"), object.__getattribute__(self, "_e")
        return cf._d.setdefault("borders", {}).setdefault(e, [0, 0])

    def __getattr__(self, name):
        cf, e = object.__getattribute__(self, "_cf"), object.__getattribute__(self, "_e")
        side = cf._d.get("borders", {}).get(e)
        if name == "LineStyle":
            return None if side is None else _INTERNAL_TO_LINE.get(side[0], (xlContinuous, xlThin))[0]
        if name == "Weight":
            return None if side is None else _INTERNAL_TO_LINE.get(side[0], (xlContinuous, xlThin))[1]
        if name == "Color":
            return None if side is None else _argb_to_bgr(side[1])
        raise AttributeError(name)

    def __setattr__(self, name, v):
        side = self._side()
        if name == "LineStyle":
            side[0] = _line_to_internal(v, xlThin)
        elif name == "Weight":
            cur = _INTERNAL_TO_LINE.get(side[0], (xlContinuous, xlThin))[0] if side[0] else xlContinuous
            side[0] = _line_to_internal(cur, v)
        elif name == "Color":
            if not side[0]:
                side[0] = 1
            side[1] = _color_to_argb(v)
        else:
            raise AttributeError(name)


class _CellFormatBorders:
    def __init__(self, cf):
        self._cf = cf

    def __call__(self, Index):  # noqa: N803
        if Index not in _EDGE_IN or _EDGE_IN[Index].startswith("inside"):
            raise SubscriptOutOfRangeError()
        return _CellFormatBorder(self._cf, _EDGE_IN[Index])

    Item = __call__

    def __setattr__(self, name, v):
        if name == "_cf":
            object.__setattr__(self, name, v)
            return
        for e in ("left", "top", "right", "bottom"):
            setattr(_CellFormatBorder(self._cf, e), name, v)


# ---- Workbook.TableStyles (Home > Format as Table, New Table Style) ----
# Excel's XlTableStyleElementType -> YCGrid's element order (whole table, column stripes, row stripes, last column,
# first column, header row, total row)
_TS_ELEMENT = {0: 0, 1: 7, 2: 8, 3: 6, 4: 5, 5: 3, 6: 4, 7: 1, 8: 2}


class _TableStyleElementPart:
    def __init__(self, el, kind):
        object.__setattr__(self, "_el", el)
        object.__setattr__(self, "_k", kind)

    def __getattr__(self, name):
        el, k = object.__getattribute__(self, "_el"), object.__getattribute__(self, "_k")
        d = el._get()
        if k == "interior" and name == "Color":
            return _argb_to_bgr(d["fill"]) if d["fill"] else 16777215
        if k == "font" and name == "Color":
            return _argb_to_bgr(d["font_color"]) if d["font_color"] else 0
        if k == "font" and name in ("Bold", "Italic"):
            v = d[name.lower()]
            return None if v < 0 else bool(v)
        raise AttributeError(name)

    def __setattr__(self, name, v):
        el, k = object.__getattribute__(self, "_el"), object.__getattribute__(self, "_k")
        if k == "interior" and name == "Color":
            el._set(fill=_color_to_argb(v))
        elif k == "font" and name == "Color":
            el._set(font_color=_color_to_argb(v))
        elif k == "font" and name in ("Bold", "Italic"):
            el._set(**{name.lower(): 1 if v else 0})
        else:
            raise AttributeError(name)


class _TableStyleBorder:
    _EDGES = {xlEdgeTop: "top", xlEdgeBottom: "bottom", xlEdgeLeft: "left", xlEdgeRight: "right", xlInsideHorizontal: "inner_h",
              xlInsideVertical: "inner_v"}

    def __init__(self, el, edge):
        object.__setattr__(self, "_el", el)
        object.__setattr__(self, "_e", _TableStyleBorder._EDGES[edge])

    def __getattr__(self, name):
        side = object.__getattribute__(self, "_el")._get()[object.__getattribute__(self, "_e")]
        if name == "LineStyle":
            return _INTERNAL_TO_LINE.get(side[0], (xlContinuous, xlThin))[0]
        if name == "Weight":
            return _INTERNAL_TO_LINE.get(side[0], (xlContinuous, xlThin))[1]
        if name == "Color":
            return _argb_to_bgr(side[1]) if side[1] else 0
        raise AttributeError(name)

    def __setattr__(self, name, v):
        el, e = object.__getattribute__(self, "_el"), object.__getattribute__(self, "_e")
        side = list(el._get()[e])
        if name == "LineStyle":
            side[0] = _line_to_internal(v, xlThin)
        elif name == "Weight":
            side[0] = _line_to_internal(_INTERNAL_TO_LINE.get(side[0], (xlContinuous, xlThin))[0] if side[0] else xlContinuous, v)
        elif name == "Color":
            side[0] = side[0] or 1
            side[1] = _color_to_argb(v)
        else:
            raise AttributeError(name)
        el._set(**{e: side})


class TableStyleElement:
    """One element of a table style (TableStyle.TableStyleElements(xlHeaderRow)): Interior, Font, Borders, StripeSize."""

    def __init__(self, style, index):
        self._style, self._i = style, _TS_ELEMENT[int(index)]

    def _get(self):
        return self._style._def()["elements"][self._i]

    def _set(self, **kw):
        d = self._style._def()
        d["elements"][self._i].update(kw)
        self._style._put(d)

    @property
    def Interior(self):  # noqa: N802
        return _TableStyleElementPart(self, "interior")

    @property
    def Font(self):  # noqa: N802
        return _TableStyleElementPart(self, "font")

    def Borders(self, Index):  # noqa: N802,N803
        return _TableStyleBorder(self, Index)

    @property
    def HasFormat(self):  # noqa: N802
        e = self._get()
        return bool(e["fill"] or e["font_color"] or e["bold"] >= 0 or e["italic"] >= 0 or any(e[k][0] for k in ("top", "bottom", "left", "right", "inner_h", "inner_v")))

    @property
    def StripeSize(self):  # noqa: N802
        d = self._style._def()
        return d["col_stripe"] if self._i in (1, 2) else d["row_stripe"]

    @StripeSize.setter
    def StripeSize(self, v):  # noqa: N802
        d = self._style._def()
        d["col_stripe" if self._i in (1, 2) else "row_stripe"] = max(1, min(9, int(v)))
        self._style._put(d)

    def Clear(self):  # noqa: N802
        self._set(fill=0, font_color=0, bold=-1, italic=-1, top=[0, 0], bottom=[0, 0], left=[0, 0], right=[0, 0], inner_h=[0, 0], inner_v=[0, 0])


class _TableStyleElements:
    def __init__(self, style):
        self._style = style

    def __call__(self, Index):  # noqa: N803
        if int(Index) not in _TS_ELEMENT:
            raise SubscriptOutOfRangeError()
        return TableStyleElement(self._style, Index)

    Item = __call__

    @property
    def Count(self):  # noqa: N802
        return len(_TS_ELEMENT)


class TableStyle:
    """A table style: Excel's built-in ones (read-only) or the workbook's own (New Table Style)."""

    def __init__(self, name):
        self._name = name

    def _def(self):
        d = _rq("tablestyle.get", name=self._name)
        if d is None:
            raise SubscriptOutOfRangeError(f"No table style {self._name!r}")
        return d

    def _put(self, d):
        if d.get("builtin"):
            raise ApplicationDefinedError("A built-in table style can't be changed; Duplicate it first")
        _rq("tablestyle.put", name=self._name, row_stripe=d["row_stripe"], col_stripe=d["col_stripe"], elements=d["elements"])

    @property
    def Name(self):  # noqa: N802
        return self._name

    NameLocal = Name

    @property
    def BuiltIn(self):  # noqa: N802
        return bool(self._def()["builtin"])

    @property
    def ShowAsAvailableTableStyle(self):  # noqa: N802
        return True

    @property
    def TableStyleElements(self):  # noqa: N802
        return _TableStyleElements(self)

    def Duplicate(self, NewTableStyleName=None):  # noqa: N802,N803
        d = self._def()
        names = {s.Name for s in TableStyles()}
        name = NewTableStyleName or next(f"{self._name} {k}" for k in range(2, 10000) if f"{self._name} {k}" not in names)
        _rq("tablestyle.put", name=str(name), row_stripe=d["row_stripe"], col_stripe=d["col_stripe"], elements=d["elements"])
        return TableStyle(str(name))

    def Delete(self):  # noqa: N802
        if self.BuiltIn:
            raise ApplicationDefinedError("A built-in table style can't be deleted")
        _rq("tablestyle.delete", name=self._name)

    def __repr__(self):
        return f"<TableStyle {self._name}>"


class TableStyles:
    """Workbook.TableStyles: Excel's 60 built-in styles (and TableStyleTrading), then the workbook's own."""

    def _names(self):
        return [e["name"] for e in _rq("tablestyle.list")]

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def Item(self, Index):  # noqa: N802,N803
        names = self._names()
        if isinstance(Index, str):
            if Index not in names:
                raise SubscriptOutOfRangeError(f"No table style {Index!r}")
            return TableStyle(Index)
        if not 1 <= int(Index) <= len(names):
            raise SubscriptOutOfRangeError()
        return TableStyle(names[int(Index) - 1])

    def __call__(self, Index):  # noqa: N803
        return self.Item(Index)

    def __iter__(self):
        return (TableStyle(n) for n in self._names())

    def __len__(self):
        return self.Count

    @property
    def DefaultTableStyle(self):  # noqa: N802
        """Workbook.DefaultTableStyle, from the collection."""
        return TableStyle(_rq("tablestyle.default_get"))

    @DefaultTableStyle.setter
    def DefaultTableStyle(self, v):  # noqa: N802
        _rq("tablestyle.default_set", name=str(getattr(v, "Name", v)))

    def Add(self, TableStyleName):  # noqa: N802,N803
        """A new empty style of the workbook (Home > Format as Table > New Table Style...)."""
        empty = {"fill": 0, "font_color": 0, "bold": -1, "italic": -1, "top": [0, 0], "bottom": [0, 0], "left": [0, 0], "right": [0, 0],
                 "inner_h": [0, 0], "inner_v": [0, 0]}
        _rq("tablestyle.put", name=str(TableStyleName), row_stripe=1, col_stripe=1, elements=[dict(empty) for _ in range(9)])
        return TableStyle(str(TableStyleName))


class CellFormat:
    """Application.FindFormat / ReplaceFormat (Excel's CellFormat): only the properties set are compared or written."""

    _PLAIN = {"NumberFormat": "numfmt", "WrapText": "wrap", "ShrinkToFit": "shrink", "IndentLevel": "indent",
              "Locked": "locked", "FormulaHidden": "hidden"}

    def __init__(self):
        object.__setattr__(self, "_d", {})

    def Clear(self):  # noqa: N802
        self._d.clear()

    @property
    def Font(self):  # noqa: N802
        return _CellFormatFont(self)

    @property
    def Interior(self):  # noqa: N802
        return _CellFormatInterior(self)

    @property
    def Borders(self):  # noqa: N802
        return _CellFormatBorders(self)

    def __getattr__(self, name):
        d = object.__getattribute__(self, "_d")
        if name in CellFormat._PLAIN:
            return d.get(CellFormat._PLAIN[name])
        if name == "HorizontalAlignment":
            return None if "halign" not in d else _HALIGN_OUT.get(d["halign"], xlGeneral)
        if name == "VerticalAlignment":
            return None if "valign" not in d else _VALIGN_OUT.get(d["valign"], xlBottom)
        if name == "Orientation":
            return d.get("rotation")
        raise AttributeError(name)

    def __setattr__(self, name, v):
        d = object.__getattribute__(self, "_d")
        if name in CellFormat._PLAIN:
            key = CellFormat._PLAIN[name]
            d[key] = str(v) if key == "numfmt" else int(v) if key == "indent" else bool(v)
        elif name == "HorizontalAlignment":
            d["halign"] = _HALIGN_IN.get(v, 0)
        elif name == "VerticalAlignment":
            d["valign"] = _VALIGN_IN.get(v, 0)
        elif name == "Orientation":
            d["rotation"] = {xlHorizontal: 0, xlUpward: 90, xlDownward: -90, xlVertical: 255}.get(int(v), int(v))
        else:
            raise AttributeError(name)

    def _spec(self):
        return dict(self._d) or None


_find_format = CellFormat()
_replace_format = CellFormat()

Application = _Application()
ThisWorkbook = Workbook()
ActiveWorkbook = ThisWorkbook
Workbooks = _Workbooks()
_Application.ActiveWorkbook = property(lambda self: ThisWorkbook)

# Range / Cells in a sheet module point at that sheet: the host marks "the current sheet" with a contextvar
_sheet_context = _threading.local()


def _active_sheet_id() -> int:
    sid = getattr(_sheet_context, "sheet", None)
    if sid is not None:
        return sid
    return int(_rq("view.focus"))  # answered without waiting for a UI frame


# ---------------------------------------------------------------------------
# Scenarios (What-If Analysis -> Scenario Manager)
# ---------------------------------------------------------------------------
def _one_area(rng):
    """Excel 16 refuses several areas for RemoveDuplicates, TextToColumns, ListObjects.Add, ListObject.Resize, Sort,
    AutoFilter, Group, Ungroup, Subtotal, AddComment and Resize (measured 2026-11-20); the first area alone was done."""
    if len(rng._areas) > 1:
        raise ApplicationDefinedError("This can't be done on a multiple range selection. Select a single range and try again.")


def _no_overlap(rng):
    """Merge over areas that overlap: Excel 16 refuses it (measured 2026-11-20)."""
    ar = rng._areas
    for i in range(len(ar)):
        for j in range(i + 1, len(ar)):
            a, b = ar[i], ar[j]
            if a[0] <= b[2] and b[0] <= a[2] and a[1] <= b[3] and b[1] <= a[3]:
                raise ApplicationDefinedError("That command cannot be used on overlapping selections.")


def _cells_of(rng, limit=32):
    """The cells of rng in reading order, at most limit + 1 of them: YCGrid refuses a list past the limit, and the
    whole sheet would be 17 billion cells. A whole column's cells are all its rows."""
    out = []
    for a in rng._areas:
        r1 = a[2] if a[2] != ROW_END else 1048575
        for r in range(a[0], r1 + 1):
            for c in range(a[1], a[3] + 1):
                if len(out) > limit:
                    return out
                out.append([r, c])
    return out


class QueryTable:
    """A text file imported into a sheet (Excel's legacy From Text): set the TextFile* properties, then Refresh."""

    def __init__(self, sheet, connection, destination):
        self._sheet = sheet
        self.Connection = connection
        self.Destination = destination
        self.Name = _os.path.splitext(_os.path.basename(connection.split(";", 1)[-1]))[0]
        self.TextFileParseType = xlDelimited
        self.TextFileCommaDelimiter = False
        self.TextFileTabDelimiter = True
        self.TextFileSemicolonDelimiter = False
        self.TextFileSpaceDelimiter = False
        self.TextFileOtherDelimiter = None
        self.TextFileConsecutiveDelimiter = False
        self.TextFileTextQualifier = 1  # xlTextQualifierDoubleQuote
        self.TextFileColumnDataTypes = None  # xlGeneralFormat (1) or xlTextFormat (2) per column; xlSkipColumn (9)
        self.TextFilePlatform = 65001  # the file's code page (File Origin)
        self.TextFileStartRow = 1
        self.TextFileFixedColumnWidths = None
        self.ResultRange = None

    def Refresh(self, BackgroundQuery=False):  # noqa: N802,N803
        """Read the file and write its fields from the Destination cell (numbers and dates recognized in General
        columns, Text columns kept as typed)."""
        import csv as _csv
        import io as _io

        path = self.Connection.split(";", 1)[-1]
        with open(path, "rb") as fh:
            raw = fh.read()
        cp = int(self.TextFilePlatform or 65001)
        text = raw.decode("utf-8-sig" if cp == 65001 else f"cp{cp}", errors="replace")
        lines = text.splitlines()[max(0, int(self.TextFileStartRow) - 1):]
        if self.TextFileParseType == xlFixedWidth:
            widths = list(self.TextFileFixedColumnWidths or [])
            rows = []
            for line in lines:
                parts, at = [], 0
                for w in widths:
                    parts.append(line[at:at + int(w)].strip())
                    at += int(w)
                parts.append(line[at:].strip())
                rows.append(parts)
        else:
            delims = ("," if self.TextFileCommaDelimiter else "") + ("\t" if self.TextFileTabDelimiter else "") + \
                (";" if self.TextFileSemicolonDelimiter else "") + (" " if self.TextFileSpaceDelimiter else "") + (self.TextFileOtherDelimiter or "")[:1]
            delim = delims[0] if delims else ","
            body = "\n".join(lines)
            for d in delims[1:]:
                body = body.replace(d, delim)
            quote = '"' if self.TextFileTextQualifier == 1 else "'" if self.TextFileTextQualifier == 2 else None
            reader = _csv.reader(_io.StringIO(body), delimiter=delim, quotechar=quote or '"', quoting=_csv.QUOTE_MINIMAL if quote else _csv.QUOTE_NONE,
                                 skipinitialspace=False)
            rows = [r for r in reader]
            if self.TextFileConsecutiveDelimiter:
                rows = [[f for f in r if f != ""] for r in rows]
        kinds = list(self.TextFileColumnDataTypes or [])
        keep = [i for i in range(max((len(r) for r in rows), default=0)) if not (i < len(kinds) and kinds[i] == 9)]
        rows = [[r[i] if i < len(r) else "" for i in keep] for r in rows]
        types = [1 if (i < len(kinds) and kinds[i] == 2) else 0 for i in keep]
        d = self.Destination._a
        a = _rq("range.importrows", sheet=self.Destination._sheet, row=d[0], col=d[1], rows=rows, types=types)
        self.ResultRange = Range._from(self.Destination._sheet, [tuple(a)])
        return True


class _QueryTables:
    def __init__(self, sheet):
        self._sheet = sheet
        self._items = []

    def Add(self, Connection, Destination, Sql=None):  # noqa: N802,N803
        """Connection "TEXT;C:\\path\\file.csv"; Destination: the top-left cell."""
        if not str(Connection).upper().startswith("TEXT;"):
            raise UnsupportedError("Only text files (Connection=\"TEXT;path\") can be imported")
        q = QueryTable(self._sheet, str(Connection), Destination)
        self._items.append(q)
        return q

    @property
    def Count(self):  # noqa: N802
        return len(self._items)

    def __call__(self, Index):  # noqa: N803
        return self._items[int(Index) - 1]


class Scenario:
    def __init__(self, sheet, name):
        self._sheet, self._name = sheet, name

    def _get(self):
        for x in _rq("scenario.list", sheet=self._sheet):
            if x["name"].lower() == self._name.lower():
                return x
        raise SubscriptOutOfRangeError(f"No such scenario: {self._name}")

    @property
    def Name(self):  # noqa: N802
        return self._get()["name"]

    @property
    def Comment(self):  # noqa: N802
        return self._get()["comment"]

    @property
    def Locked(self):  # noqa: N802
        """Protection > Prevent changes."""
        return self._get()["locked"]

    @Locked.setter
    def Locked(self, v):  # noqa: N802
        self._set(locked=bool(v))

    @property
    def Hidden(self):  # noqa: N802
        """Protection > Hide."""
        return self._get()["hidden"]

    @Hidden.setter
    def Hidden(self, v):  # noqa: N802
        self._set(hidden=bool(v))

    def _set(self, **kw):
        x = self._get()
        x.update(kw)
        _rq("scenario.add", sheet=self._sheet, name=x["name"], comment=x["comment"], cells=x["cells"], values=x["values"], locked=x["locked"],
            hidden=x["hidden"])

    @property
    def ChangingCells(self):  # noqa: N802
        cells = self._get()["cells"]
        return Range._from(self._sheet, [(r, c, r, c) for r, c in cells])

    def Values(self, Index=None):  # noqa: N802,N803
        v = self._get()["values"]
        return v if Index is None else v[int(Index) - 1]

    def Show(self):  # noqa: N802
        _rq("scenario.show", sheet=self._sheet, name=self._name)

    def Delete(self):  # noqa: N802
        _rq("scenario.delete", sheet=self._sheet, name=self._name)

    def ChangeScenario(self, ChangingCells, Values=None):  # noqa: N802,N803
        x = self._get()
        _rq("scenario.add", sheet=self._sheet, name=x["name"], comment=x["comment"], cells=_cells_of(ChangingCells),
            values=[_to_wire(v) for v in (Values or [])], locked=x["locked"], hidden=x["hidden"])


class _Scenarios:
    def __init__(self, sheet):
        self._sheet = sheet

    def Add(self, Name, ChangingCells, Values=None, Comment=None, Locked=True, Hidden=False):  # noqa: N802,N803
        """Add a scenario; Values default to the cells' current values."""
        if Values is not None and not isinstance(Values, (list, tuple)):
            Values = [Values]
        _rq("scenario.add", sheet=self._sheet, name=str(Name), comment=str(Comment or ""), cells=_cells_of(ChangingCells),
            values=[_to_wire(v) for v in (Values or [])], locked=bool(Locked), hidden=bool(Hidden))
        return Scenario(self._sheet, str(Name))

    def Item(self, Index):  # noqa: N802,N803
        items = _rq("scenario.list", sheet=self._sheet)
        if isinstance(Index, int):
            if not 1 <= Index <= len(items):
                raise SubscriptOutOfRangeError()
            return Scenario(self._sheet, items[Index - 1]["name"])
        for x in items:
            if x["name"].lower() == str(Index).lower():
                return Scenario(self._sheet, x["name"])
        raise SubscriptOutOfRangeError(f"No such scenario: {Index}")

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    @property
    def Count(self):  # noqa: N802
        return len(_rq("scenario.list", sheet=self._sheet))

    def __iter__(self):
        return iter([Scenario(self._sheet, x["name"]) for x in _rq("scenario.list", sheet=self._sheet)])

    def __len__(self):
        return self.Count

    def CreateSummary(self, ReportType=xlStandardSummary, ResultCells=None):  # noqa: N802,N803
        """A Scenario Summary sheet (before this sheet), or with ReportType=xlPivotTable a Scenario PivotTable report;
        ResultCells are the formulas to report."""
        from .constants import xlPivotTable

        if ReportType not in (xlStandardSummary, xlPivotTable):
            raise ApplicationDefinedError(f"Unknown report type: {ReportType}")
        sid = _rq("scenario.summary", sheet=self._sheet, results=_cells_of(ResultCells) if ResultCells is not None else [],
                  type=2 if ReportType == xlPivotTable else 1)
        return Worksheet(sid)

    def Merge(self, Source):  # noqa: N802,N803
        """Scenario Manager > Merge: copy the scenarios of Source (a Worksheet or a sheet name); a name in use gets a number."""
        src = Source if isinstance(Source, Worksheet) else Worksheets(str(Source))
        return _rq("scenario.merge", sheet=self._sheet, source=src._id)


# ---------------------------------------------------------------------------
# Solver: the functions of Excel's Solver add-in, on the active sheet
# ---------------------------------------------------------------------------
_SOLVER_LAST = {}


def _solver_sheet():
    return Application.ActiveSheet._id


def _ref_text(x):
    if isinstance(x, Range):
        return f"'{Worksheet(x._sheet).Name}'!" + x.Address
    s = str(x)
    return s[1:] if s.startswith("=") and "!" in s[:40] and not any(ch in s for ch in "+-*/^(") else s


def _solver_model():
    m = _rq("solver.get", sheet=_solver_sheet())
    m.setdefault("constraints", [])
    return m


def _solver_save(m):
    _rq("solver.set", sheet=_solver_sheet(), model=m)


def SolverReset():  # noqa: N802
    """Clear the Solver model of the active sheet."""
    _solver_save({"objective": "", "goal": 1, "value": 0, "changing": "", "constraints": [], "non_negative": True, "method": 1})


def SolverOk(SetCell=None, MaxMinVal=1, ValueOf=0, ByChange=None, Engine=None, EngineDesc=None):  # noqa: N802,N803
    """Define the model: SetCell (objective), MaxMinVal 1 max / 2 min / 3 value of, ValueOf, ByChange (variable cells),
    Engine 1 Simplex LP / 2 GRG Nonlinear / 3 Evolutionary (or EngineDesc)."""
    m = _solver_model()
    if SetCell is not None:
        m["objective"] = _ref_text(SetCell)
    m["goal"] = int(MaxMinVal)
    m["value"] = float(ValueOf or 0)
    if ByChange is not None:
        m["changing"] = _ref_text(ByChange)
    if EngineDesc:
        Engine = {"simplex lp": 1, "grg nonlinear": 2, "evolutionary": 3}.get(str(EngineDesc).lower(), Engine)
    if Engine is not None:
        m["method"] = {1: 2, 2: 1, 3: 3}[int(Engine)]
    _solver_save(m)


def SolverAdd(CellRef, Relation, FormulaText=None):  # noqa: N802,N803
    """Add a constraint: Relation 1 <=, 2 =, 3 >=, 4 int, 5 bin, 6 dif; FormulaText is a number, a reference or a formula."""
    m = _solver_model()
    rhs = "" if FormulaText is None else (_ref_text(FormulaText) if isinstance(FormulaText, Range) else str(FormulaText))
    m["constraints"].append([_ref_text(CellRef), int(Relation), rhs])
    _solver_save(m)


def SolverChange(CellRef, Relation, FormulaText=None):  # noqa: N802,N803
    """Change the right side of an existing constraint."""
    m = _solver_model()
    ref = _ref_text(CellRef)
    for c in m["constraints"]:
        if c[0].replace("$", "").lower() == ref.replace("$", "").lower() and c[1] == int(Relation):
            c[2] = "" if FormulaText is None else (_ref_text(FormulaText) if isinstance(FormulaText, Range) else str(FormulaText))
    _solver_save(m)


def SolverDelete(CellRef, Relation, FormulaText=None):  # noqa: N802,N803
    m = _solver_model()
    ref = _ref_text(CellRef)
    m["constraints"] = [c for c in m["constraints"] if not (c[0].replace("$", "").lower() == ref.replace("$", "").lower() and c[1] == int(Relation))]
    _solver_save(m)


def SolverOptions(MaxTime=None, Iterations=None, Precision=None, AssumeLinear=None, StepThru=None, Estimates=None, Derivatives=None,  # noqa: N802,N803
                  SearchOption=None, IntTolerance=None, Scaling=None, Convergence=None, AssumeNonNeg=None, PopulationSize=None, RandomSeed=None,
                  MutationRate=None, Multistart=None, RequireBounds=None, MaxSubproblems=None, MaxIntegerSols=None, MaxTimeNoImp=None,
                  SolveWithout=None, **_):
    """The Solver Options dialog's settings (All Methods, GRG Nonlinear, Evolutionary), with Excel's argument names."""
    m = _solver_model()
    for arg, key, conv in ((StepThru, "show_iterations", bool), (Derivatives, "derivatives", int), (IntTolerance, "integer_optimality", float),
                           (Scaling, "auto_scaling", bool), (Convergence, "convergence", float), (PopulationSize, "population", int),
                           (RandomSeed, "random_seed", int), (MutationRate, "mutation_rate", float), (Multistart, "multistart", bool),
                           (RequireBounds, "require_bounds", bool), (MaxSubproblems, "max_subproblems", int), (MaxIntegerSols, "max_feasible", int),
                           (MaxTimeNoImp, "max_time_no_improve", float), (SolveWithout, "ignore_integer", bool)):
        if arg is not None:
            m[key] = conv(arg)
    if MaxTime is not None:
        m["max_time"] = float(MaxTime)
    if Iterations is not None:
        m["max_iterations"] = int(Iterations)
    if Precision is not None:
        m["precision"] = float(Precision)
    if AssumeNonNeg is not None:
        m["non_negative"] = bool(AssumeNonNeg)
    if AssumeLinear:
        m["method"] = 2
    _solver_save(m)


def SolverSolve(UserFinish=False, ShowRef=None):  # noqa: N802,N803
    """Solve the model of the active sheet and keep the solution; returns Excel's result code
    (0 or 1 solution found, 3 limit reached, 4 unbounded, 5 no feasible solution, 7 not linear, 13 model error, 14 integer solution)."""
    r = _rq("solver.solve", sheet=_solver_sheet())
    _SOLVER_LAST.clear()
    _SOLVER_LAST.update(r, sheet=_solver_sheet())
    return int(r["status"])


def SolverFinish(KeepFinal=1, ReportArray=None):  # noqa: N802,N803
    """KeepFinal 1 keeps the solution, 2 restores the original values of the variable cells; ReportArray: the reports
    to make, 1 Answer, 2 Sensitivity, 3 Limits (new sheets before the model's sheet)."""
    if ReportArray and _SOLVER_LAST:
        want = set(int(x) for x in (ReportArray if isinstance(ReportArray, (list, tuple)) else [ReportArray]))
        _rq("solver.reports", sheet=_SOLVER_LAST["sheet"], status=int(_SOLVER_LAST.get("status", 0)), message=_SOLVER_LAST.get("message", ""),
            original=list(_SOLVER_LAST.get("original", [])), answer=1 in want, sensitivity=2 in want, limits=3 in want)
    if int(KeepFinal) != 2 or not _SOLVER_LAST:
        return
    m = _rq("solver.get", sheet=_SOLVER_LAST["sheet"])
    rng = Worksheet(_SOLVER_LAST["sheet"]).Range(m["changing"])
    cells = _cells_of(rng, len(_SOLVER_LAST.get("original", [])))
    for (r, c), v in zip(cells, _SOLVER_LAST.get("original", [])):
        Worksheet(_SOLVER_LAST["sheet"]).Cells(r + 1, c + 1).Value = v


def SolverSave(SaveArea):  # noqa: N802,N803
    """Save Model: write the model of the active sheet into cells from SaveArea's first cell (the objective, the
    variable count, one cell per constraint, then the options)."""
    a = SaveArea._a
    r = _rq("solver.save", sheet=SaveArea._sheet, model_sheet=_solver_sheet(), row=a[0], col=a[1])
    return Range._from(SaveArea._sheet, [tuple(r)])


def SolverLoad(LoadArea, Merge=False):  # noqa: N802,N803
    """Load Model: read a model saved with SolverSave (LoadArea: its cells) into the active sheet's model."""
    _rq("solver.load", sheet=LoadArea._sheet, model_sheet=_solver_sheet(), rect=list(LoadArea._a))
    return 0


def SolverGet(TypeNum, SheetName=None):  # noqa: N802,N803
    """Model settings: 1 objective, 2 MaxMinVal, 3 ValueOf, 4 variable cells, 5 constraint count, 6 constraint cells, 7 relations, 8 right sides."""
    m = _rq("solver.get", sheet=Worksheet(SheetName)._id if SheetName else _solver_sheet())
    cons = m.get("constraints", [])
    return {1: m.get("objective"), 2: m.get("goal"), 3: m.get("value"), 4: m.get("changing"), 5: len(cons),
            6: [c[0] for c in cons], 7: [c[1] for c in cons], 8: [c[2] for c in cons]}.get(int(TypeNum))


def _resolve(obj):
    """The object behind ActiveSheet / ActiveCell / Selection (proxies), else obj itself."""
    return obj._getter() if isinstance(obj, _ActiveProxy) else obj


class _ActiveProxy:
    """ActiveSheet, ActiveCell, Selection, ActiveWindow: the current value on every access."""

    def __init__(self, getter):
        object.__setattr__(self, "_getter", getter)

    def __getattr__(self, name):
        return getattr(self._getter(), name)

    def __setattr__(self, name, value):
        setattr(self._getter(), name, value)

    def __call__(self, *a, **kw):
        return self._getter()(*a, **kw)

    def __iter__(self):
        return iter(self._getter())

    def __repr__(self):
        return repr(self._getter())

    def __eq__(self, other):
        return self._getter() == other

    def __hash__(self):
        return hash(self._getter())


ActiveSheet = _ActiveProxy(lambda: Application.ActiveSheet)
ActiveCell = _ActiveProxy(lambda: Application.ActiveCell)
Selection = _ActiveProxy(lambda: Application.Selection)
ActiveWindow = _ActiveProxy(lambda: Window(_rq("view.get")["focus_sheet"]))


def Cells(RowIndex=None, ColumnIndex=None):  # noqa: N802,N803
    return _CellsAccessor(_active_sheet_id(), None)(RowIndex, ColumnIndex)


class _GlobalRows:
    def __call__(self, Index):  # noqa: N803
        return _RowsAccessor(_active_sheet_id())(Index)

    @property
    def Count(self):  # noqa: N802
        return Worksheet(_active_sheet_id()).RowLimit


class _GlobalCols:
    def __call__(self, Index):  # noqa: N803
        return _ColsAccessor(_active_sheet_id())(Index)

    Count = MAX_COLS


Rows = _GlobalRows()
Columns = _GlobalCols()


def Evaluate(Name):  # noqa: N802,N803
    return Application.Evaluate(Name)


class SheetModule:
    """Me in a sheet module: Me = SheetModule(__name__) in sheets/Sheet1.py."""

    def __new__(cls, module_name: str):
        from . import host

        code = module_name.rsplit(".", 1)[-1]
        ws = host.sheet_by_code_name(code)
        if ws is None:
            raise SubscriptOutOfRangeError(f"No sheet has the CodeName {code}")
        return _SheetMe(ws._id, code)


class _SheetMe(Worksheet):
    __slots__ = ("_code",)

    def __init__(self, sheet_id, code):
        super().__init__(sheet_id)
        object.__setattr__(self, "_code", code)

    def Range(self, Cell1, Cell2=None):  # noqa: N802,N803
        return _make_range(self._id, Cell1, Cell2)

    # Cells, Rows, Columns and every other member are the Worksheet's: Sheet1.Cells(2, 3), Sheet1.Cells.Clear(),
    # Sheet1.Rows(5).Insert(), Sheet1.Name = "X" work like on Worksheets("...")

    def OnChange(self, Address, Sources=ycUser | ycPython):  # noqa: N802,N803
        """Decorator: @Me.OnChange("B2:B100")."""
        rng = self.Range(Address)

        def deco(fn):
            from . import host

            host.set_onchange(rng, fn, Sources, append=True)
            return fn

        return deco



# ---------------------------------------------------------------------------
# notes (Comment) and hyperlinks (Hyperlinks)
# ---------------------------------------------------------------------------
class _Note:
    def __init__(self, sheet, r, c, data):
        self._sheet, self._r, self._c, self._d = sheet, r, c, data

    def Text(self, Text=None, Start=None, Overwrite=False):  # noqa: N802,N803
        """The note's text; Text(t) replaces it, Text(t, Start) puts t at character Start (1-based): inserted, or written
        over the characters there with Overwrite=True, as Excel's Comment.Text."""
        if Text is None:
            return self._d["text"]
        new = str(Text)
        if Start is not None:
            n = _rq("note.get", sheet=self._sheet, row=self._r, col=self._c)
            cur = (n or {}).get("text", self._d["text"]) or ""
            i = min(max(int(Start) - 1, 0), len(cur))
            new = cur[:i] + new + (cur[i + len(new):] if Overwrite else cur[i:])
        _rq("note.set", sheet=self._sheet, row=self._r, col=self._c, text=new)
        self._d["text"] = new
        return new

    @property
    def Author(self):  # noqa: N802
        return self._d["author"]

    @property
    def Visible(self):  # noqa: N802
        """True: the note stays on the sheet (Review > Show/Hide Note); False: it shows when the mouse is over its cell."""
        n = _rq("note.get", sheet=self._sheet, row=self._r, col=self._c)
        return bool(n and n.get("visible"))

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        _rq("note.visible", sheet=self._sheet, row=self._r, col=self._c, visible=bool(v))

    @property
    def Parent(self):  # noqa: N802
        return Range._from(self._sheet, [(self._r, self._c, self._r, self._c)])

    @property
    def Shape(self):  # noqa: N802
        """The note's box: Width, Height, Left, Top (points), Fill / Line .ForeColor.RGB, TextFrame alignment and font
        (its handles and Format Comment)."""
        return _NoteShape(self)

    def Delete(self):  # noqa: N802
        _rq("note.set", sheet=self._sheet, row=self._r, col=self._c, text="")

    def _step(self, d):
        notes = sorted((x for x in _rq("note.list", sheet=self._sheet) if not x.get("threaded")), key=lambda x: (x["row"], x["col"]))
        keys = [(x["row"], x["col"]) for x in notes]
        if (self._r, self._c) not in keys:
            return None
        i = keys.index((self._r, self._c)) + d
        return _Note(self._sheet, notes[i]["row"], notes[i]["col"], notes[i]) if 0 <= i < len(notes) else None

    def Next(self):  # noqa: N802
        """The next note on the sheet (rows, then columns), None after the last (Review > Next Note)."""
        return self._step(1)

    def Previous(self):  # noqa: N802
        return self._step(-1)


def _bgr_of_argb(v):
    v = int(v) & 0xFFFFFF
    return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF)


class _NoteColor:
    def __init__(self, note, key, default):
        self._n, self._k, self._def = note, key, default

    @property
    def RGB(self):  # noqa: N802
        v = self._n._look().get(self._k, 0)
        return _bgr_of_argb(v) if v else self._def

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        self._n._set_look(**{self._k: 0xFF000000 | _bgr_of_argb(v)})


class _NoteFill:
    def __init__(self, note, key, default):
        self.ForeColor = _NoteColor(note, key, default)


class _NoteFont:
    def __init__(self, note):
        self._n = note

    def _p(name, key, conv, default):  # noqa: N805
        def g(self):
            v = self._n._look().get(key)
            return v if v else default

        def s(self, v):
            self._n._set_look(**{key: conv(v)})

        return property(g, s)

    Name = _p("Name", "font_name", str, "Tahoma")
    Size = _p("Size", "font_size", float, 9.0)
    Bold = _p("Bold", "bold", bool, False)
    Italic = _p("Italic", "italic", bool, False)

    @property
    def Color(self):  # noqa: N802
        v = self._n._look().get("font_color", 0)
        return _bgr_of_argb(v) if v else 0

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._n._set_look(font_color=0xFF000000 | _bgr_of_argb(v))


class _NoteTextFrame:
    """xlHAlignLeft -4131, xlHAlignCenter -4108, xlHAlignRight -4152; xlVAlignTop -4160, xlVAlignCenter -4108, xlVAlignBottom -4107."""

    _H = {0: -4131, 1: -4108, 2: -4152}
    _V = {0: -4160, 1: -4108, 2: -4107}

    def __init__(self, note):
        self._n = note

    def Characters(self, Start=None, Length=None):  # noqa: N802,N803
        n = self._n

        class _Chars:
            Font = _NoteFont(n)

            @property
            def Text(self):  # noqa: N802
                return n.Text()

        return _Chars()

    @property
    def HorizontalAlignment(self):  # noqa: N802
        return self._H.get(self._n._look().get("halign", 0), -4131)

    @HorizontalAlignment.setter
    def HorizontalAlignment(self, v):  # noqa: N802
        self._n._set_look(halign={v: k for k, v in self._H.items()}.get(int(v), 0))

    @property
    def VerticalAlignment(self):  # noqa: N802
        return self._V.get(self._n._look().get("valign", 0), -4160)

    @VerticalAlignment.setter
    def VerticalAlignment(self, v):  # noqa: N802
        self._n._set_look(valign={v: k for k, v in self._V.items()}.get(int(v), 0))


class _NoteShape:
    """Comment.Shape: the note's box. Left / Top are on the sheet (points); by default it sits beside its cell."""

    def __init__(self, note):
        self._n = note
        self.Fill = _NoteFill(self, "fill_color", 0xE1FFFF)
        self.Line = _NoteFill(self, "line_color", 0x606060)
        self.TextFrame = _NoteTextFrame(self)

    def _look(self):
        d = _rq("note.get", sheet=self._n._sheet, row=self._n._r, col=self._n._c) or {}
        return d.get("look", {})

    def _set_look(self, **props):
        _rq("note.look", sheet=self._n._sheet, row=self._n._r, col=self._n._c, props=props)

    def Text(self):  # noqa: N802
        return self._n.Text()

    def _cell(self):
        return self._n.Parent

    @property
    def Width(self):  # noqa: N802
        return self._look().get("w") or 150.0

    @Width.setter
    def Width(self, v):  # noqa: N802
        self._set_look(w=float(v))

    @property
    def Height(self):  # noqa: N802
        return self._look().get("h") or 90.0

    @Height.setter
    def Height(self, v):  # noqa: N802
        self._set_look(h=float(v))

    @property
    def Left(self):  # noqa: N802
        c = self._cell()
        return c.Left + c.Width + 10.5 + (self._look().get("dx") or 0.0)

    @Left.setter
    def Left(self, v):  # noqa: N802
        c = self._cell()
        self._set_look(dx=float(v) - (c.Left + c.Width + 10.5))

    @property
    def Top(self):  # noqa: N802
        return self._cell().Top - 6.0 + (self._look().get("dy") or 0.0)

    @Top.setter
    def Top(self, v):  # noqa: N802
        self._set_look(dy=float(v) - (self._cell().Top - 6.0))


class _AutoCorrect:
    """Application.AutoCorrect: words replaced as they are typed into a cell (Spelling > AutoCorrect)."""

    def AddReplacement(self, What, Replacement):  # noqa: N802,N803
        _rq("app.autocorrect", add=[str(What), str(Replacement)])

    def DeleteReplacement(self, What):  # noqa: N802,N803
        _rq("app.autocorrect", delete=str(What))

    @property
    def ReplaceText(self):  # noqa: N802
        """Replace text as you type: False turns AutoCorrect's replacements off (saved for this computer)."""
        return bool(_rq("app.autocorrect", get_replace_text=True))

    @ReplaceText.setter
    def ReplaceText(self, v):  # noqa: N802
        _rq("app.autocorrect", replace_text=bool(v))

    def ReplacementList(self, Index=None):  # noqa: N802,N803
        lst = [tuple(x) for x in _rq("app.autocorrect")]
        return lst if Index is None else lst[int(Index) - 1]


_user_name = None
_spell_lang = "en-US"


class _CommentAuthor:
    def __init__(self, name):
        self.Name = name

    def __repr__(self):
        return f"<Author {self.Name!r}>"


class CommentThreaded:
    """A threaded comment: the first post of a cell's conversation; Replies holds the answers."""

    def __init__(self, sheet, r, c, reply=-1):
        self._sheet, self._r, self._c, self._i = sheet, r, c, reply  # reply -1 = the first post

    def __repr__(self):
        return f"<CommentThreaded {self.Parent.Address(False, False)}{'' if self._i < 0 else f' reply {self._i + 1}'}>"

    def _thread(self):
        n = _rq("note.get", sheet=self._sheet, row=self._r, col=self._c)
        if not n or not n.get("threaded"):
            raise SubscriptOutOfRangeError("The comment no longer exists")
        return n

    def _post(self):
        n = self._thread()
        if self._i < 0:
            return n
        if self._i >= len(n["replies"]):
            raise SubscriptOutOfRangeError("The reply no longer exists")
        return n["replies"][self._i]

    def Text(self, Text=None, Start=None, Overwrite=False):  # noqa: N802,N803
        """The post's text; Text(t, Start) puts t at character Start (inserted, or over the text there with
        Overwrite=True), as Excel's CommentThreaded.Text."""
        if Text is None:
            return self._post()["text"]
        new = str(Text)
        if Start is not None:
            cur = self._post()["text"] or ""
            i = min(max(int(Start) - 1, 0), len(cur))
            new = cur[:i] + new + (cur[i + len(new):] if Overwrite else cur[i:])
        _rq("comment.edit", sheet=self._sheet, row=self._r, col=self._c, index=self._i, text=new)
        return new

    @property
    def Author(self):  # noqa: N802
        return _CommentAuthor(self._post()["author"])

    @property
    def Date(self):  # noqa: N802
        return _dt.datetime.fromtimestamp(self._post()["date"])

    @property
    def Parent(self):  # noqa: N802
        return Range._from(self._sheet, [(self._r, self._c, self._r, self._c)])

    @property
    def Replies(self):  # noqa: N802
        return _CommentReplies(self)

    def AddReply(self, Text):  # noqa: N802,N803
        _rq("comment.reply", sheet=self._sheet, row=self._r, col=self._c, text=str(Text), author=Application.UserName)
        return CommentThreaded(self._sheet, self._r, self._c, len(self._thread()["replies"]) - 1)

    @property
    def Resolved(self):  # noqa: N802
        return bool(self._thread().get("resolved"))

    @Resolved.setter
    def Resolved(self, v):  # noqa: N802
        _rq("comment.resolve", sheet=self._sheet, row=self._r, col=self._c, resolved=bool(v))

    def _step(self, d):
        if self._i >= 0:
            return None
        a = [(x._r, x._c) for x in _CommentsThreaded(self._sheet)._all()]
        k = a.index((self._r, self._c)) + d if (self._r, self._c) in a else -1
        return CommentThreaded(self._sheet, *a[k]) if 0 <= k < len(a) else None

    def Next(self):  # noqa: N802
        """The next threaded comment on the sheet (Review > Next Comment); None after the last."""
        return self._step(1)

    def Previous(self):  # noqa: N802
        """The previous threaded comment on the sheet; None before the first."""
        return self._step(-1)

    def Delete(self):  # noqa: N802
        """The first post: delete the whole thread; a reply: delete that reply."""
        if self._i < 0:
            _rq("comment.set", sheet=self._sheet, row=self._r, col=self._c, text="")
        else:
            _rq("comment.reply_delete", sheet=self._sheet, row=self._r, col=self._c, index=self._i)


class _CommentReplies:
    def __init__(self, thread: CommentThreaded):
        self._t = thread

    @property
    def Count(self):  # noqa: N802
        return len(self._t._thread()["replies"])

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        if not 1 <= int(Index) <= self.Count:
            raise SubscriptOutOfRangeError("Reply index out of range")
        return CommentThreaded(self._t._sheet, self._t._r, self._t._c, int(Index) - 1)

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([self.Item(i + 1) for i in range(self.Count)])


class _Comments(list):
    """Worksheet.Comments: Excel's collection (Count, Item / call with a 1-based index, For Each) that is also a list."""

    @property
    def Count(self):  # noqa: N802
        return len(self)

    def Item(self, Index):  # noqa: N802,N803
        if not 1 <= int(Index) <= len(self):
            raise SubscriptOutOfRangeError("Comment index out of range")
        return self[int(Index) - 1]

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self


class _CommentsThreaded:
    def __init__(self, sheet):
        self._sheet = sheet

    def _all(self):
        return [CommentThreaded(self._sheet, x["row"], x["col"]) for x in _rq("note.list", sheet=self._sheet) if x.get("threaded")]

    @property
    def Count(self):  # noqa: N802
        return len(self._all())

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        a = self._all()
        if not 1 <= int(Index) <= len(a):
            raise SubscriptOutOfRangeError("Comment index out of range")
        return a[int(Index) - 1]

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter(self._all())


class Hyperlink:
    def __init__(self, sheet, r, c):
        self._sheet, self._r, self._c = sheet, r, c

    def _get(self):
        return _rq("link.get", sheet=self._sheet, row=self._r, col=self._c) or {"address": "", "sub_address": "", "tip": ""}

    def _set(self, **kw):
        d = self._get()
        d.update(kw)
        _rq("link.set", sheet=self._sheet, row=self._r, col=self._c, address=d["address"], sub_address=d["sub_address"], tip=d["tip"], style=False)

    @property
    def Address(self):  # noqa: N802
        return self._get()["address"]

    @Address.setter
    def Address(self, v):  # noqa: N802
        self._set(address=str(v))

    @property
    def SubAddress(self):  # noqa: N802
        return self._get()["sub_address"]

    @SubAddress.setter
    def SubAddress(self, v):  # noqa: N802
        self._set(sub_address=str(v))

    @property
    def ScreenTip(self):  # noqa: N802
        return self._get()["tip"]

    @ScreenTip.setter
    def ScreenTip(self, v):  # noqa: N802
        self._set(tip=str(v))

    @property
    def Range(self):  # noqa: N802
        return Range._from(self._sheet, [(self._r, self._c, self._r, self._c)])

    @property
    def TextToDisplay(self):  # noqa: N802
        return self.Range.Text

    @TextToDisplay.setter
    def TextToDisplay(self, v):  # noqa: N802
        self.Range.Value = str(v)

    def Delete(self):  # noqa: N802
        # Remove Hyperlinks: the cell's formats go too (Excel 16, measured); Range.ClearHyperlinks keeps them
        _rq("link.clear", sheet=self._sheet, rect=[self._r, self._c, self._r, self._c], formats=True)

    def Follow(self, NewWindow=False):  # noqa: N802,N803
        import os as _os

        d = self._get()
        if d["address"]:
            _open_on_desktop(d["address"])
        elif d["sub_address"]:
            Application.Goto(d["sub_address"])


class _Hyperlinks:
    """Worksheet.Hyperlinks or Range.Hyperlinks."""

    def __init__(self, sheet, rng=None):
        self._sheet, self._rng = sheet, rng

    def _list(self):
        items = []
        areas = self._rng._areas if self._rng is not None else [None]
        for a in areas:
            items += _rq("link.list", sheet=self._sheet, **({"rect": list(a)} if a else {}))
        return items

    def Add(self, Anchor, Address="", SubAddress="", ScreenTip="", TextToDisplay=None):  # noqa: N802,N803
        a = Anchor._areas[0]
        p = dict(sheet=Anchor._sheet, row=a[0], col=a[1], address=str(Address or ""), sub_address=str(SubAddress or ""), tip=str(ScreenTip or ""))
        if TextToDisplay is not None:
            p["text"] = str(TextToDisplay)
        elif Anchor.Value in (None, ""):
            p["text"] = str(Address or SubAddress)
        _rq("link.set", **p)
        return Hyperlink(Anchor._sheet, a[0], a[1])

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def Item(self, Index):  # noqa: N802,N803
        items = self._list()
        i = int(Index)
        if not 1 <= i <= len(items):
            raise SubscriptOutOfRangeError("Hyperlinks index out of range")
        x = items[i - 1]
        return Hyperlink(self._sheet, x["row"], x["col"])

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([Hyperlink(self._sheet, x["row"], x["col"]) for x in self._list()])

    def __len__(self):
        return self.Count

    def Delete(self):  # noqa: N802
        for x in self._list():
            _rq("link.clear", sheet=self._sheet, rect=[x["row"], x["col"], x["row"], x["col"]], formats=True)

# ---------------------------------------------------------------------------
# MsgBox, InputBox
# ---------------------------------------------------------------------------
def LoadPicture(Filename):  # noqa: N802,N803
    """A picture file for Image.Picture (MSForms)."""
    from . import ui

    return ui.LoadPicture(Filename)


def MsgBox(Prompt="", Buttons=vbOKOnly, Title=None):  # noqa: N802,N803
    return _rq("ui.msgbox", prompt=str(Prompt), buttons=int(Buttons), title=Title or "YCGrid")


def _export_image(what, Filename, Headings, Gridlines, Scale, **where):
    """Range / Worksheet / Workbook.ExportImage: draw the cells as a PNG (or to the Clipboard when Filename is None)."""
    import os as _os

    p = dict(what=what, path=_os.path.abspath(str(Filename)) if Filename else "", headings=bool(Headings), scale=float(Scale), **where)
    if Gridlines is not None:
        p["gridlines"] = bool(Gridlines)
    r = _rq("image.export", **p)
    r["path"] = p["path"] or None
    return r


def InputBox(Prompt="", Title=None, Default=""):  # noqa: N802,N803
    r = _rq("ui.inputbox", prompt=str(Prompt), title=Title or "YCGrid", default=str(Default), type=2)
    return "" if r is None else r


def DoEvents():  # noqa: N802
    _rq("sync", wait="engine")


# ---------------------------------------------------------------------------
# helpers for porting Excel macros (object model §3)
# ---------------------------------------------------------------------------
Empty = None
Nothing = None


def CVErr(Number):  # noqa: N802,N803
    return XlError(Number)


def IsError(v):  # noqa: N802
    return isinstance(v, XlError)


def IsEmpty(v):  # noqa: N802
    return v is None or v == ""


def IsNumeric(v):  # noqa: N802
    if v is None or isinstance(v, bool):
        return True
    if isinstance(v, (int, float, _decimal.Decimal)):
        return True
    try:
        float(str(v).replace(",", ""))
        return True
    except ValueError:
        return False


def IsDate(v):  # noqa: N802
    if isinstance(v, (_dt.datetime, _dt.date)):
        return True
    try:
        DateValue(v)
        return True
    except Exception:  # noqa: BLE001
        return False


def IsArray(v):  # noqa: N802
    return isinstance(v, (list, tuple, VArray))


def Left(s, n):  # noqa: N802
    return str(s)[: int(n)]


def Right(s, n):  # noqa: N802
    s = str(s)
    return s[len(s) - int(n):] if n > 0 else ""


def Mid(s, start, length=None):  # noqa: N802
    s = str(s)
    return s[start - 1:] if length is None else s[start - 1:start - 1 + length]


def Len(s):  # noqa: N802
    return len("" if s is None else str(s))


def InStr(*args):  # noqa: N802
    if len(args) >= 3 and isinstance(args[0], int):
        start, s1, s2 = args[0], args[1], args[2]
    else:
        start, s1, s2 = 1, args[0], args[1]
    return str(s1).find(str(s2), start - 1) + 1


def InStrRev(s1, s2, start=-1):  # noqa: N802
    s1 = str(s1)
    end = len(s1) if start == -1 else start
    return s1.rfind(str(s2), 0, end) + 1


def Replace(expression, find, replace, start=1, count=-1):  # noqa: N802
    s = str(expression)[start - 1:]
    return s.replace(find, replace) if count < 0 else s.replace(find, replace, count)


def Trim(s):  # noqa: N802
    return str(s).strip(" ")


def LTrim(s):  # noqa: N802
    return str(s).lstrip(" ")


def RTrim(s):  # noqa: N802
    return str(s).rstrip(" ")


def UCase(s):  # noqa: N802
    return str(s).upper()


def LCase(s):  # noqa: N802
    return str(s).lower()


def StrComp(a, b, compare=0):  # noqa: N802
    a, b = (str(a), str(b)) if compare == 0 else (str(a).lower(), str(b).lower())
    return (a > b) - (a < b)


def StrReverse(s):  # noqa: N802
    return str(s)[::-1]


def Space(n):  # noqa: N802
    return " " * int(n)


def String(n, ch):  # noqa: N802
    return (ch if isinstance(ch, str) else chr(ch))[0] * int(n)


def Chr(n):  # noqa: N802
    return chr(int(n))


ChrW = Chr


def Asc(s):  # noqa: N802
    return ord(str(s)[0])


AscW = Asc


def Split(expression, delimiter=" ", limit=-1):  # noqa: N802
    return str(expression).split(delimiter, limit if limit >= 0 else -1) if limit != -1 else str(expression).split(delimiter)


def Join(items, delimiter=" "):  # noqa: N802
    return delimiter.join("" if x is None else str(x) for x in items)


def Like(s, pattern):  # noqa: N802
    regex = ""
    i = 0
    while i < len(pattern):
        ch = pattern[i]
        if ch == "*":
            regex += ".*"
        elif ch == "?":
            regex += "."
        elif ch == "#":
            regex += r"\d"
        elif ch == "[":
            j = pattern.find("]", i)
            body = pattern[i + 1:j]
            regex += "[" + ("^" + body[1:] if body.startswith("!") else body) + "]"
            i = j
        else:
            regex += _re.escape(ch)
        i += 1
    return _re.fullmatch(regex, str(s), _re.S) is not None


def CStr(v):  # noqa: N802
    if v is None:
        return ""
    if isinstance(v, bool):
        return "True" if v else "False"
    if isinstance(v, float) and v.is_integer():
        return str(int(v))
    return str(v)


def CDbl(v):  # noqa: N802
    if v is None:
        return 0.0
    try:
        return float(str(v).replace(",", "")) if isinstance(v, str) else float(v)
    except ValueError as e:
        raise TypeMismatchError(f"Can't convert to a number: {v!r}") from e


CSng = CDbl


def CLng(v):  # noqa: N802
    return int(round(CDbl(v)))


CInt = CLng


def CBool(v):  # noqa: N802
    if isinstance(v, str):
        if v.lower() in ("true", "\u662f"):
            return True
        if v.lower() in ("false", "\u5426"):
            return False
        return CDbl(v) != 0
    return bool(v)


def CDate(v):  # noqa: N802
    if isinstance(v, _dt.datetime):
        return v
    if isinstance(v, (int, float)):
        return serial_to_datetime(float(v))
    return DateValue(v)


def Val(s):  # noqa: N802
    m = _re.match(r"\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)", str(s).replace(" ", ""))
    return float(m.group(1)) if m else 0.0


def Int(v):  # noqa: N802
    return _math.floor(v)


def Fix(v):  # noqa: N802
    return int(v)


def Round(v, n=0):  # noqa: N802
    return round(v, n)


Abs = abs


def Sgn(v):  # noqa: N802
    return (v > 0) - (v < 0)


def Sqr(v):  # noqa: N802
    return _math.sqrt(v)


def Log(v):  # noqa: N802
    return _math.log(v)


def Exp(v):  # noqa: N802
    return _math.exp(v)


def Rnd():  # noqa: N802
    import random

    return random.random()


def Mod(a, b):  # noqa: N802
    return _math.fmod(a, b) if isinstance(a, float) or isinstance(b, float) else int(_math.fmod(a, b))


def DateSerial(year, month, day):  # noqa: N802
    y, m = year + (month - 1) // 12, (month - 1) % 12 + 1
    return _dt.datetime(y, m, 1) + _dt.timedelta(days=day - 1)


def TimeSerial(hour, minute, second):  # noqa: N802
    return _dt.timedelta(hours=hour, minutes=minute, seconds=second)


def DateValue(s):  # noqa: N802
    s = str(s).strip()
    for fmt in ("%Y/%m/%d", "%Y-%m-%d", "%m/%d/%Y", "%Y/%m/%d %H:%M:%S", "%Y-%m-%d %H:%M:%S", "%Y/%m/%d %H:%M", "%Y\u5e74%m\u6708%d\u65e5"):
        try:
            d = _dt.datetime.strptime(s, fmt)
            return _dt.datetime(d.year, d.month, d.day)
        except ValueError:
            continue
    raise TypeMismatchError(f"Can't convert to a date: {s!r}")


def TimeValue(s):  # noqa: N802
    parts = [float(p) for p in str(s).strip().split(":")]
    while len(parts) < 3:
        parts.append(0)
    return _dt.timedelta(hours=parts[0], minutes=parts[1], seconds=parts[2])


def Now():  # noqa: N802
    return _dt.datetime.now()


def Date():  # noqa: N802
    t = _dt.datetime.now()
    return _dt.datetime(t.year, t.month, t.day)


def Time():  # noqa: N802
    t = _dt.datetime.now()
    return _dt.timedelta(hours=t.hour, minutes=t.minute, seconds=t.second, microseconds=t.microsecond)


def Timer():  # noqa: N802
    return Time().total_seconds()


def Year(d):  # noqa: N802
    return CDate(d).year


def Month(d):  # noqa: N802
    return CDate(d).month


def Day(d):  # noqa: N802
    return CDate(d).day


def Hour(d):  # noqa: N802
    return (d.seconds // 3600) if isinstance(d, _dt.timedelta) else CDate(d).hour


def Minute(d):  # noqa: N802
    return (d.seconds // 60 % 60) if isinstance(d, _dt.timedelta) else CDate(d).minute


def Second(d):  # noqa: N802
    return (d.seconds % 60) if isinstance(d, _dt.timedelta) else CDate(d).second


def Weekday(d, firstdayofweek=1):  # noqa: N802
    return (CDate(d).isoweekday() % 7 - (firstdayofweek - 1)) % 7 + 1


def DateAdd(interval, number, date):  # noqa: N802
    d = CDate(date)
    n = int(number)
    if interval == "yyyy":
        return DateSerial(d.year + n, d.month, d.day).replace(hour=d.hour, minute=d.minute, second=d.second)
    if interval == "q":
        return DateAdd("m", 3 * n, d)
    if interval == "m":
        y, m = d.year + (d.month - 1 + n) // 12, (d.month - 1 + n) % 12 + 1
        import calendar

        return d.replace(year=y, month=m, day=min(d.day, calendar.monthrange(y, m)[1]))
    delta = {"d": _dt.timedelta(days=1), "y": _dt.timedelta(days=1), "w": _dt.timedelta(days=1), "ww": _dt.timedelta(weeks=1),
             "h": _dt.timedelta(hours=1), "n": _dt.timedelta(minutes=1), "s": _dt.timedelta(seconds=1)}[interval]
    return d + delta * n


def DateDiff(interval, date1, date2):  # noqa: N802
    a, b = CDate(date1), CDate(date2)
    if interval == "yyyy":
        return b.year - a.year
    if interval == "m":
        return (b.year - a.year) * 12 + b.month - a.month
    if interval == "q":
        return ((b.year - a.year) * 12 + b.month - a.month) // 3
    secs = (b - a).total_seconds()
    div = {"d": 86400, "y": 86400, "w": 604800, "ww": 604800, "h": 3600, "n": 60, "s": 1}[interval]
    return int(secs // div)


def Format(expression, fmt=""):  # noqa: N802
    if not fmt:
        return CStr(expression)
    v = expression
    if isinstance(v, _dt.datetime):
        v = datetime_to_serial(v)
    return _rq("format", value=v, format=str(fmt))


def Array(*items):  # noqa: N802
    return list(items)


def UBound(arr, dimension=1):  # noqa: N802
    if isinstance(arr, VArray):
        return arr.Rows if dimension == 1 else arr.Columns
    return (len(arr) if dimension == 1 else len(arr[0])) - 1


def LBound(arr, dimension=1):  # noqa: N802
    return 1 if isinstance(arr, VArray) else 0


def IIf(expression, truepart, falsepart):  # noqa: N802
    return truepart if expression else falsepart


def Environ(name):  # noqa: N802
    import os

    return os.environ.get(name, "")


def Beep():  # noqa: N802
    try:
        import winsound

        winsound.MessageBeep()
    except ImportError:
        pass


# udf and stream decorators (collected by the host)
def udf(fn=None, *, name=None, volatile=False, vectorize=False, thread_safe=False, async_=False, accept_errors=False, nan_as=None):
    from . import host

    def deco(f):
        host.register_udf(name or f.__name__, f, volatile=volatile, accept_errors=accept_errors)
        return f

    return deco(fn) if fn is not None else deco


def stream(name):
    from . import host

    def deco(cls):
        host.register_stream(name, cls)
        return cls

    return deco




# ---------------------------------------------------------------------------
# Objects of another workbook (another YCGrid window) remember its connection; while one of their methods runs, every request
# (and every object made meanwhile) goes to that window
# ---------------------------------------------------------------------------
_NO_BINDING = object()


def _binding_of(obj):
    cx = getattr(obj, "_cx", _NO_BINDING)
    if cx is _NO_BINDING:
        r = getattr(obj, "_r", None)
        if r is not None:
            cx = getattr(r, "_cx", _NO_BINDING)
    return cx


def _bound(fn):
    import functools

    @functools.wraps(fn)
    def run(self, *a, **k):
        cx = _binding_of(self)
        if cx is _NO_BINDING or getattr(_conn_tls, "conn", None) is cx:
            return fn(self, *a, **k)
        prev = getattr(_conn_tls, "conn", None)
        _conn_tls.conn = cx
        try:
            out = fn(self, *a, **k)
        finally:
            _conn_tls.conn = prev
        if cx is not None:
            _tag_binding(out, cx)
        return out

    return run


def _tag_binding(out, cx):
    """Objects made while another window's request ran (Cells, Rows, PivotTables...) keep talking to that window."""
    items = out if isinstance(out, (list, tuple)) else (out,)
    for o in items:
        if type(o).__module__ != __name__ or isinstance(o, type):
            continue
        try:
            if "_cx" in getattr(o, "__dict__", {}) or (hasattr(type(o), "__slots__") and "_cx" in type(o).__slots__):
                continue
            object.__setattr__(o, "_cx", cx)
        except (AttributeError, TypeError):
            pass


def _bind_classes():
    import inspect

    skip = {"__init__", "__new__", "__setattr__", "__getattr__", "__getattribute__", "__eq__", "__hash__", "__init_subclass__", "__class_getitem__",
            "__slots__", "__del__", "__enter__", "__exit__"}
    for obj in list(globals().values()):
        if not inspect.isclass(obj) or obj.__module__ != __name__ or issubclass(obj, BaseException):
            continue
        for name, attr in list(vars(obj).items()):
            if name in skip:
                continue
            if isinstance(attr, property):
                setattr(obj, name, property(_bound(attr.fget) if attr.fget else None, _bound(attr.fset) if attr.fset else None,
                                            _bound(attr.fdel) if attr.fdel else None, attr.__doc__))
            elif inspect.isfunction(attr) and (not name.startswith("__") or name in ("__call__", "__iter__", "__len__", "__getitem__", "__repr__",
                                                                                      "__str__", "__contains__", "__bool__")):
                setattr(obj, name, _bound(attr))


class ChartSheet(_ui_mod.Chart):
    """A chart sheet (Sheets("Chart1"), Charts(1)): a sheet that is one chart; it has the Chart members too."""

    def __init__(self, sheet_id: int):
        self._id = sheet_id
        self._cx = getattr(_conn_tls, "conn", None)
        charts = _ui_mod._list(sheet_id)["charts"]
        super().__init__(sheet_id, charts[0]["name"] if charts else "")

    def _info(self):
        for s in _sheets_info():
            if s["id"] == self._id:
                return s
        raise SubscriptOutOfRangeError("The sheet has been deleted")

    def __eq__(self, other):
        return isinstance(other, ChartSheet) and other._id == self._id

    def __hash__(self):
        return hash(("cs", self._id))

    def __repr__(self):
        try:
            return f"<ChartSheet {self.Name!r}>"
        except YcError:
            return f"<ChartSheet id={self._id}>"

    @property
    def Name(self):  # noqa: N802
        return self._info()["name"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        _rq("sheet.rename", id=self._id, name=str(v))

    @property
    def Index(self):  # noqa: N802
        return self._info()["index"]

    @property
    def Type(self):  # noqa: N802
        return xlChart

    @property
    def Visible(self):  # noqa: N802
        i = self._info()
        return xlSheetVisible if i["visible"] else xlSheetVeryHidden if i.get("very_hidden") else xlSheetHidden

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        """xlSheetVisible (True), xlSheetHidden (False) or xlSheetVeryHidden (not offered by Unhide); the last visible
        sheet can't be hidden."""
        if v is None:
            return
        _rq("sheet.visible", id=self._id, visible=v in (True, xlSheetVisible), very=v == xlSheetVeryHidden)

    @property
    def Tab(self):  # noqa: N802
        return _Tab(self)

    @property
    def CodeName(self):  # noqa: N802
        return self._info()["code_name"]

    def Activate(self):  # noqa: N802
        _rq("view.set", activate_sheet=self._id)

    def Select(self, Replace=True):  # noqa: N802,N803
        self.Activate()

    def Delete(self):  # noqa: N802
        _rq("sheet.delete", id=self._id)

    def Move(self, Before=None, After=None):  # noqa: N802,N803
        Worksheet.Move(self, Before, After)

    def Copy(self, Before=None, After=None):  # noqa: N802,N803
        index = Before.Index - 1 if Before is not None else (After.Index if After is not None else None)
        cs = ChartSheet(_rq("sheet.copy", id=self._id, index=index)["id"])
        cs.Activate()
        return cs



def _add_application():
    """Every object of Excel's model has Application (ws.Application.WorksheetFunction, rng.Application.Selection):
    Worksheet, Range and Workbook raised AttributeError."""
    import inspect

    prop = property(lambda self: Application, doc="The Application object, as every object of Excel's model has.")
    for obj in list(globals().values()):
        if inspect.isclass(obj) and obj.__module__ == __name__ and not issubclass(obj, BaseException) and "Application" not in dir(obj):
            obj.Application = prop


_add_application()
_bind_classes()

from .forms import Unload, UserForm, vbModal, vbModeless  # noqa: E402

__all__ = [n for n in dir() if not n.startswith("_") and n not in ("annotations", "Any", "Iterable", "Optional")]
