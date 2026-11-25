# SPDX-License-Identifier: Apache-2.0
"""[YC] Data binding: DataFrames and arrays as live tables, through shared memory.

    t = ws.bind(df, key="strike", formats={"iv": "pct", "edge": "bp", "bid": {"tick": 0.05}}, flash=["bid", "ask"])
    t.update(changed_rows)                 # only the keys and columns that changed
    t.update({"strike": [23000], "bid": [101.5]})
    t.delete([22800])
    t.replace(df)                          # the rows become exactly these

The bound range is an ordinary table (ListObject) named after the binding, so formulas (`=SUM(Chain[bid])`),
charts, filters and PivotTables use it. Data go through a shared-memory ring as column frames (Arrow's memory
layout, no pyarrow needed); the pipe only says where a frame is. pandas, polars, pyarrow, numpy record arrays, dicts
of columns and lists of dicts are accepted; numpy makes encoding fast but isn't required.
"""
from __future__ import annotations

import datetime as _dt
import math
import mmap
import os
import re
import struct
import threading
import time
from typing import Any, Iterable, Optional

from ._conn import YcError, current as _conn

__all__ = ["bind", "table", "tables", "read", "read_range", "write_frame", "BoundTable", "number_format", "encode_frame", "decode_frame"]

UPSERT, REPLACE, DELETE, APPEND = 1, 2, 3, 4
_F64, _I64, _UTF8, _BOOL, _NULL = 1, 2, 3, 4, 5
_MAGIC = 0x46434359
_HDR = struct.Struct("<IHHIIIIIIqQ16x")   # 64 bytes
_DESC = struct.Struct("<IIB7x4Q")          # 48 bytes
_EPOCH = _dt.datetime(1899, 12, 30)


def _np():
    try:
        import numpy

        return numpy
    except ImportError:
        return None


# ---------------------------------------------------------------------------
# Columns of any table-like object
# ---------------------------------------------------------------------------
def _columns(data) -> tuple[list[str], list]:
    """(names, columns) of a DataFrame, a dict of columns, a record array or a list of dicts."""
    if data is None:
        return [], []
    mod = type(data).__module__.split(".")[0]
    if mod == "pandas":
        if hasattr(data, "to_frame") and not hasattr(data, "columns"):  # a Series
            data = data.to_frame()
        if data.index.name is not None or not _plain_index(data):
            data = data.reset_index()
        return [str(c) for c in data.columns], [data[c] for c in data.columns]
    if mod == "polars":
        return [str(c) for c in data.columns], [data.get_column(c) for c in data.columns]
    if mod == "pyarrow":
        return [str(n) for n in data.column_names], [data.column(i) for i in range(data.num_columns)]
    if isinstance(data, dict):
        return [str(k) for k in data], list(data.values())
    np = _np()
    if np is not None and isinstance(data, np.ndarray) and data.dtype.names:
        return list(data.dtype.names), [data[n] for n in data.dtype.names]
    if isinstance(data, (list, tuple)) and data and isinstance(data[0], dict):
        names: list[str] = []
        for row in data:
            for k in row:
                if k not in names:
                    names.append(k)
        return [str(n) for n in names], [[row.get(n) for row in data] for n in names]
    raise YcError(f"bind: can't read columns from {type(data).__name__}; pass a DataFrame, a dict of columns or a list of dicts",
                  1004, "E_ARG")


def _plain_index(df) -> bool:
    try:
        import pandas

        return isinstance(df.index, pandas.RangeIndex)
    except ImportError:
        return True


def _to_list_or_array(col):
    """A column as a numpy array when numpy is there (else a list)."""
    np = _np()
    mod = type(col).__module__.split(".")[0]
    if mod == "pandas":
        if str(col.dtype) == "category":
            col = col.astype(object)
        if np is not None:
            if str(col.dtype).startswith("datetime64"):
                return col.to_numpy(dtype="datetime64[ns]")
            if col.dtype.kind in "biuf":
                return col.to_numpy()
            if str(col.dtype) in ("Int64", "Int32", "Float64", "boolean"):
                return col.to_numpy(dtype=object, na_value=None)
            return col.to_numpy(dtype=object)
        return col.tolist()
    if mod == "polars":
        return col.to_numpy() if np is not None else col.to_list()
    if mod == "pyarrow":
        if hasattr(col, "combine_chunks"):
            col = col.combine_chunks()
        return col.to_numpy(zero_copy_only=False) if np is not None else col.to_pylist()
    if np is not None and isinstance(col, np.ndarray):
        return col
    if np is not None and isinstance(col, (list, tuple)):
        return list(col)
    return list(col) if not isinstance(col, list) else col


_offset_cache = [0.0, -1.0]  # (1462 or 0, when it was asked)


def date_offset(refresh=False) -> float:
    """1462 when the workbook uses the 1904 date system (Workbook.Date1904), else 0: serials written in bulk are the
    workbook's, so a datetime's 1900-system serial is shifted by it. Asked at most once a second."""
    import time

    now = time.monotonic()
    if refresh or now - _offset_cache[1] > 1.0:
        try:
            from .excel import _rq

            _offset_cache[0] = 1462.0 if _rq("wb.options").get("date1904") else 0.0
        except Exception:  # noqa: BLE001 - an older YCGrid, or not connected: the 1900 system
            _offset_cache[0] = 0.0
        _offset_cache[1] = now
    return _offset_cache[0]


def _excel_serial(v) -> float:
    if isinstance(v, _dt.datetime):
        if v.tzinfo is not None:
            v = v.astimezone().replace(tzinfo=None)
        d = v - _EPOCH
    else:
        d = _dt.datetime(v.year, v.month, v.day) - _EPOCH
    return d.days + d.seconds / 86400 + d.microseconds / 86400e6 - date_offset()


# ---------------------------------------------------------------------------
# Frames
# ---------------------------------------------------------------------------
class _Buf:
    def __init__(self):
        self.b = bytearray()

    def put(self, data) -> int:
        pad = (-len(self.b)) % 8
        if pad:
            self.b += b"\0" * pad
        off = len(self.b)
        self.b += data
        return off


def _pack_bits(flags) -> bytes:
    np = _np()
    if np is not None:
        return np.packbits(np.asarray(flags, dtype=bool), bitorder="little").tobytes()
    out = bytearray((len(flags) + 7) // 8)
    for i, f in enumerate(flags):
        if f:
            out[i >> 3] |= 1 << (i & 7)
    return bytes(out)


def _encode_column(buf: _Buf, col, n: int):
    """-> (type, values_off, values_len, offsets_off, validity_off, is_date)."""
    np = _np()
    col = _to_list_or_array(col)
    if np is not None and isinstance(col, list) and col and isinstance(col[0], (int, float)) and not isinstance(col[0], bool):
        try:
            arr = np.asarray(col)
            if arr.dtype.kind in "fiu":
                col = arr
        except (ValueError, TypeError, OverflowError):
            pass
    if np is not None and isinstance(col, np.ndarray):
        k = col.dtype.kind
        if k == "f":
            a = col.astype("<f8", copy=False)
            return _F64, buf.put(a.tobytes()), n * 8, 0, 0, False
        if k in "iu":
            a = col.astype("<i8", copy=False)
            return _I64, buf.put(a.tobytes()), n * 8, 0, 0, False
        if k == "b":
            return _BOOL, buf.put(np.packbits(col, bitorder="little").tobytes()), (n + 7) // 8, 0, 0, False
        if k == "M":
            ns = col.astype("datetime64[ns]").astype("<i8")
            nat = ns == np.iinfo("<i8").min
            serial = ns / 86400e9 + 25569.0 - date_offset()
            serial[nat] = np.nan
            return _F64, buf.put(serial.astype("<f8").tobytes()), n * 8, 0, 0, True
        if k == "m":
            days = col.astype("timedelta64[ns]").astype("<i8") / 86400e9
            return _F64, buf.put(days.astype("<f8").tobytes()), n * 8, 0, 0, False
        col = col.tolist()
    # a list: numbers, booleans, dates, text, None
    kinds = set()
    for v in col:
        if v is None or (isinstance(v, float) and math.isnan(v)):
            continue
        if isinstance(v, bool):
            kinds.add("b")
        elif isinstance(v, (int, float)):
            kinds.add("n")
        elif isinstance(v, (_dt.datetime, _dt.date)):
            kinds.add("d")
        elif hasattr(v, "__float__") and not isinstance(v, str):
            kinds.add("n")
        else:
            kinds.add("s")
    if not kinds:
        return _NULL, 0, 0, 0, 0, False
    valid = [not (v is None or (isinstance(v, float) and math.isnan(v))) for v in col]
    vbits = 0 if all(valid) else buf.put(_pack_bits(valid))
    if kinds == {"b"}:
        return _BOOL, buf.put(_pack_bits([bool(v) for v in col])), (n + 7) // 8, 0, vbits, False
    if kinds <= {"n", "d", "b"} and "n" in kinds or kinds == {"d"}:
        vals = [(_excel_serial(v) if isinstance(v, (_dt.datetime, _dt.date)) else float(v)) if ok else math.nan for v, ok in zip(col, valid)]
        return _F64, buf.put(struct.pack(f"<{n}d", *vals)), n * 8, 0, vbits, kinds == {"d"}
    parts = [str(v).encode("utf-8") if ok else b"" for v, ok in zip(col, valid)]
    offs = [0]
    total = 0
    for p in parts:
        total += len(p)
        offs.append(total)
    if total > 0x7FFFFFFF:
        raise YcError("bind: a text column is larger than 2 GB", 1004, "E_ARG")
    o_off = buf.put(struct.pack(f"<{n + 1}i", *offs))
    v_off = buf.put(b"".join(parts))
    return _UTF8, v_off, total, o_off, vbits, False


def encode_frame(table: str, op: int, names: list[str], cols: list, key_col: int = -1, sent_ns: Optional[int] = None) -> tuple[bytes, list[bool]]:
    """One column frame (engine/bind.h). Returns the bytes and, per column, whether it held dates."""
    n = len(cols[0]) if cols else 0
    for c in cols:
        if len(c) != n:
            lens = ", ".join(f"{nm} has {len(cc)}" for nm, cc in zip(names, cols))
            raise YcError(f"bind: the columns don't have the same length ({lens})", 1004, "E_ARG")
    buf = _Buf()
    buf.b += b"\0" * (_HDR.size + _DESC.size * len(cols))
    tname = table.encode("utf-8")
    name_off = buf.put(tname)
    descs = []
    dates = []
    for name, col in zip(names, cols):
        nb = str(name).encode("utf-8")
        n_off = buf.put(nb)
        t, v_off, v_len, o_off, valid, is_date = _encode_column(buf, col, n)
        descs.append(_DESC.pack(n_off, len(nb), t, v_off, v_len, o_off, valid))
        dates.append(is_date)
    pad = (-len(buf.b)) % 8
    buf.b += b"\0" * pad
    hdr = _HDR.pack(_MAGIC, 1, op, len(cols), n, 0xFFFFFFFF if key_col < 0 else key_col, name_off, len(tname), 0,
                    time.perf_counter_ns() if sent_ns is None else sent_ns, len(buf.b))
    buf.b[0:_HDR.size] = hdr
    for i, d in enumerate(descs):
        o = _HDR.size + i * _DESC.size
        buf.b[o:o + _DESC.size] = d
    return bytes(buf.b), dates


# ---------------------------------------------------------------------------
# The shared-memory ring of a connection
# ---------------------------------------------------------------------------
def decode_frame(frame, dates=None, date_offset=0.0) -> tuple[str, dict]:
    """A column frame -> (table name, {column: values}). With numpy, numbers come back as float64 / int64 arrays (a
    missing number is NaN), booleans as bool arrays, text as lists (a missing value is None); dates[i] turns column i
    from Excel serial numbers into datetime64. Without numpy, every column is a list."""
    np = _np()
    mv = memoryview(frame)
    magic, ver, _op, ncols, nrows, _key, name_off, name_len, _, _sent, total = _HDR.unpack_from(mv, 0)
    if magic != _MAGIC or total > len(mv):
        raise YcError("Not a column frame", 1004, "E_ARG")
    table = bytes(mv[name_off:name_off + name_len]).decode()
    out = {}
    for i in range(ncols):
        n_off, n_len, t, v_off, v_len, o_off, valid = _DESC.unpack_from(mv, _HDR.size + i * _DESC.size)
        name = bytes(mv[n_off:n_off + n_len]).decode()
        if valid:
            bits = bytes(mv[valid:valid + (nrows + 7) // 8])
            ok = (np.unpackbits(np.frombuffer(bits, np.uint8), bitorder="little")[:nrows].astype(bool) if np is not None
                  else [(bits[j >> 3] >> (j & 7)) & 1 == 1 for j in range(nrows)])
        else:
            ok = None
        if t in (_F64, _I64) and np is not None:
            a = np.frombuffer(mv, np.float64 if t == _F64 else np.int64, nrows, v_off).copy()
            if ok is not None and not ok.all():
                a = a.astype(np.float64)
                a[~ok] = np.nan
        elif t in (_F64, _I64):
            a = list(struct.unpack_from(f"<{nrows}{'d' if t == _F64 else 'q'}", mv, v_off))
            if ok is not None:
                a = [x if k else None for x, k in zip(a, ok)]
        elif t == _BOOL:
            bits = bytes(mv[v_off:v_off + (nrows + 7) // 8])
            if np is not None:
                a = np.unpackbits(np.frombuffer(bits, np.uint8), bitorder="little")[:nrows].astype(bool)
                if ok is not None and not ok.all():
                    a = np.where(ok, a, None)
            else:
                a = [bool((bits[j >> 3] >> (j & 7)) & 1) for j in range(nrows)]
                if ok is not None:
                    a = [x if k else None for x, k in zip(a, ok)]
        elif t == _UTF8:
            offs = struct.unpack_from(f"<{nrows + 1}i", mv, o_off)
            raw = bytes(mv[v_off:v_off + offs[-1]])
            a = [raw[offs[j]:offs[j + 1]].decode() for j in range(nrows)]
            if ok is not None:
                a = [x if k else None for x, k in zip(a, ok)]
        else:
            a = [None] * nrows
        if dates is not None and i < len(dates) and dates[i] and t in (_F64, _I64):
            if np is not None:
                ms = np.round((np.asarray(a, dtype=np.float64) + date_offset - 25569.0) * 86400000.0)
                d = np.full(nrows, np.datetime64("NaT", "ms"))
                fin = np.isfinite(ms)
                d[fin] = ms[fin].astype(np.int64).astype("datetime64[ms]")
                a = d
            else:
                a = [None if x is None else _EPOCH + _dt.timedelta(days=x + date_offset) for x in a]
        out[name] = a
    return table, out


_read_lock = threading.Lock()
_read_maps: dict = {}


def read(name: str, pandas: bool = True):
    """Any table (ListObject) as a pandas DataFrame, read through shared memory: a million cells in milliseconds.
    Dates come back as datetime64 columns. pandas=False (or without pandas) returns a dict of columns."""
    return _read_frame("table.read", {"name": name}, pandas)


def read_range(rng, header: bool = True, pandas: bool = True):
    """A range as a DataFrame through shared memory; its first row names the columns when header is True. Whole
    columns (A:D) stop at the used range. Same types as read()."""
    from . import excel

    rng = excel._resolve(rng)
    return _read_frame("range.read", {"sheet": rng._sheet, "rect": list(rng._a), "header": bool(header)}, pandas)


def write_frame(sheet, data, at="A1", header: bool = True, index: bool = False):
    """Writes table-like data (a DataFrame, dict of columns, numpy record array...) as plain cells starting at `at`,
    through shared memory; dates get a date format. index=True writes a pandas index as the first column. Returns
    the Range written. (ws.bind makes a live table instead.)"""
    from . import excel

    sheet = excel._resolve(sheet)
    ws = sheet if isinstance(sheet, excel.Worksheet) else excel.Worksheets(sheet)
    if index and hasattr(data, "reset_index"):
        data = data.reset_index()
    names, cols = _columns(data)
    frame, dates = encode_frame("@write", REPLACE, names, cols)
    formats = []
    np = _np()
    for c, is_date in zip(cols, dates):
        if not is_date:
            formats.append("")
            continue
        arr = _to_list_or_array(c)
        timed = False
        if np is not None and isinstance(arr, np.ndarray) and arr.dtype.kind == "M":
            ok = ~np.isnat(arr)
            ns = arr[ok].astype("datetime64[ns]").astype("int64")
            timed = bool(((ns % (86400 * 10**9)) != 0).any())
        else:
            timed = any(isinstance(v, _dt.datetime) and (v.hour or v.minute or v.second or v.microsecond) for v in arr)
        formats.append("yyyy-mm-dd hh:mm:ss" if timed else "yyyy-mm-dd")
    anchor = ws.Range(at) if isinstance(at, str) else excel._resolve(at)
    r = _channel().send_request("range.write_frame", {"sheet": ws._id, "row": anchor._a[0], "col": anchor._a[1], "header": bool(header),
                                                       "formats": formats}, frame)
    return excel.Range._from(ws._id, [tuple(r)]) if r else None


def _read_frame(method: str, params: dict, pandas: bool):
    with _read_lock:
        r = _conn().request(method, params)
        m = _read_maps.get(r["name"])
        if m is None:
            m = mmap.mmap(-1, r["size"], tagname=r["name"])
            for k in list(_read_maps):
                _read_maps.pop(k).close()
            _read_maps[r["name"]] = m
        frame = m[:r["len"]]
    _, cols = decode_frame(frame, r.get("dates"), r.get("date_offset", 0.0))
    np = _np()
    if np is not None:
        for k, a in cols.items():  # sheet numbers are doubles: whole-number columns become int64 (keys, quantities)
            if isinstance(a, np.ndarray) and a.dtype == np.float64 and a.size and np.isfinite(a).all() and (np.abs(a) < 2.0 ** 53).all() and (a == np.floor(a)).all():
                cols[k] = a.astype(np.int64)
    if pandas:
        try:
            import pandas as pd

            return pd.DataFrame(cols)
        except ImportError:
            pass
    return cols


class _Channel:
    def __init__(self, conn):
        info = conn.request("data.channel", {"size_mb": 64})
        self.conn = conn
        self.size = int(info["size"])
        self.hdr = int(info["header"])
        self.cap = self.size - self.hdr
        self.mm = mmap.mmap(-1, self.size, tagname=info["name"])
        self.head = 0
        self.inflight = 8 << 20
        self.lock = threading.RLock()

    def _tail(self) -> int:
        return struct.unpack_from("<Q", self.mm, 16)[0]

    def send_request(self, method: str, params: dict, frame: bytes):
        """A frame in the ring plus a request that names it (yc.plot); waits for the reply."""
        with self.lock:  # in ring order: a later frame's request can't free this one's space first
            pos, end = self._put(frame)
            return self.conn.request(method, {**params, "chan": 0, "pos": pos, "len": len(frame), "end": end})

    def _put(self, frame: bytes):
        n = len(frame)
        n8 = (n + 7) & ~7
        if n8 > self.cap:
            raise YcError("a frame is larger than the data channel", 1004, "E_ARG")
        with self.lock:
            pos = self.head % self.cap
            if pos + n8 > self.cap:
                self.head += self.cap - pos
                pos = 0
            deadline = time.monotonic() + 30
            limit = min(self.cap, max(self.inflight, n8 * 2))
            while self.head + n8 - self._tail() > limit:
                if self.conn.closed or time.monotonic() > deadline:
                    raise YcError("YCGrid is not reading the data channel", 0, "E_TIMEOUT")
                time.sleep(0.0002)
            self.mm[self.hdr + pos:self.hdr + pos + n] = frame
            self.head += n8
            return pos, self.head

    def send(self, frame: bytes, sync: bool):
        n = len(frame)
        n8 = (n + 7) & ~7
        if n8 > self.cap:
            raise YcError("bind: a frame is larger than the data channel", 1004, "E_ARG")
        with self.lock:
            pos = self.head % self.cap
            if pos + n8 > self.cap:  # frames don't wrap: skip to the start
                self.head += self.cap - pos
                pos = 0
            deadline = time.monotonic() + 30
            # wait while the ring is full, or while more than `inflight` bytes wait for YCGrid (keeps latency low:
            # a fast sender is paced by YCGrid instead of queueing seconds of data)
            limit = min(self.cap, max(self.inflight, n8 * 2))
            while self.head + n8 - self._tail() > limit:
                if self.conn.closed or time.monotonic() > deadline:
                    raise YcError("YCGrid is not reading the data channel", 0, "E_TIMEOUT")
                time.sleep(0.0002)
            self.mm[self.hdr + pos:self.hdr + pos + n] = frame
            self.head += n8
            params = {"chan": 0, "pos": pos, "len": n, "end": self.head}
            if sync:
                params["sync"] = True
                return self.conn.request("bind.frame", params)
            self.conn.notify("bind.frame", params)
            return None


_channels: dict[int, _Channel] = {}
_chan_lock = threading.Lock()


def _channel() -> _Channel:
    conn = _conn()
    with _chan_lock:
        ch = _channels.get(id(conn))
        if ch is None or ch.conn is not conn or conn.closed:
            ch = _Channel(conn)
            _channels[id(conn)] = ch
        return ch


# ---------------------------------------------------------------------------
# Formats declared once per column
# ---------------------------------------------------------------------------
def number_format(spec) -> str:
    """A column format spec -> an Excel number format code.

    "pct" / "pct1" (percent, 2 or 1 decimals), "bp" (basis points), "int" / "thousands" (#,##0),
    "price2" (2 decimals with thousands separators), {"tick": 0.05} (the tick's decimals), {"decimals": 3},
    {"format": "0.00"}, "date", "datetime", "time", or an Excel format code ("0.000", "#,##0.00"). A dict may also hold
    "max_hz" (the column's refresh cap, used by bind)."""
    if spec is None:
        return ""
    if isinstance(spec, dict):
        if "tick" in spec:
            from decimal import Decimal

            # repr gives the shortest form ("0.05", "1e-05"); its exponent is the number of decimals
            d = max(0, -Decimal(repr(float(spec["tick"]))).normalize().as_tuple().exponent)
            return "#,##0" + ("." + "0" * d if d else "")
        if "decimals" in spec:
            d = int(spec["decimals"])
            return ("#,##0" if spec.get("thousands", True) else "0") + ("." + "0" * d if d else "")
        if "format" in spec:
            return number_format(spec["format"])
        if set(spec) <= {"max_hz"}:  # only a refresh cap: no number format
            return ""
        raise YcError(f"Unknown format spec: {spec!r} (use tick, decimals, format and max_hz)", 1004, "E_ARG")
    s = str(spec)
    low = s.lower()
    if low.startswith("pct") or low in ("percent", "%"):
        d = int(low[3:]) if low[3:].isdigit() else 2
        return "0" + ("." + "0" * d if d else "") + "%"
    if low.startswith("bp"):
        d = int(low[2:]) if low[2:].isdigit() else 1
        return "[mul=10000]0" + ("." + "0" * d if d else "") + '" bp"'
    if low in ("int", "thousands"):
        return "#,##0"
    if low.startswith("price") and low[5:].isdigit():
        return "#,##0." + "0" * int(low[5:])
    if low == "date":
        return "yyyy-mm-dd"
    if low == "datetime":
        return "yyyy-mm-dd hh:mm:ss"
    if low == "time":
        return "hh:mm:ss.000"
    return s


def _color(v) -> int:
    """An RGB value (Excel's R + G*256 + B*65536) or "#rrggbb" -> ARGB."""
    if isinstance(v, str) and v.startswith("#") and len(v) == 7:
        return 0xFF000000 | int(v[1:], 16)
    v = int(v)
    r, g, b = v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF
    return 0xFF000000 | (r << 16) | (g << 8) | b


_RENDERS = {None: 0, "none": 0, "bar": 1, "bar2": 2, "diverging": 2, "heat": 3, "progress": 4, "trend": 5}


def _render_def(spec) -> dict:
    """A column renderer: "bar", "bar2" (two-sided), "heat", "progress", "trend", or {"type": ..., "color", "neg", "max",
    "length"} -> the definition's fields."""
    sp = spec if isinstance(spec, dict) else {"type": spec}
    t = sp.get("type")
    if t not in _RENDERS:
        raise YcError(f"render type is bar, bar2, heat, progress or trend, not {t!r}", 1004, "E_ARG")
    d = {"render": _RENDERS[t]}
    if sp.get("color") is not None:
        d["render_color"] = _color(sp["color"])
    if sp.get("neg") is not None:
        d["render_neg"] = _color(sp["neg"])
    if sp.get("max") is not None:
        d["render_max"] = float(sp["max"])
    if sp.get("length") is not None:
        d["trend_len"] = int(sp["length"])
    return d


def _column_defs(names, formats, flash, date_cols, render=None) -> list[dict]:
    formats = dict(formats or {})
    if flash is True:
        flash = {n: "updown" for n in names}
    elif isinstance(flash, (list, tuple, set)):
        flash = {n: "updown" for n in flash}
    flash = dict(flash or {})
    out = []
    for n in names:
        fmt = number_format(formats[n]) if n in formats else ("yyyy-mm-dd hh:mm:ss" if n in date_cols else "")
        d = {"name": n, "number_format": fmt}
        f = flash.get(n)
        if f:
            spec = f if isinstance(f, dict) else {"mode": f}
            mode = str(spec.get("mode", "updown")).lower()
            d["flash"] = 2 if mode in ("change", "any") else 1
            if "up" in spec:
                d["up_color"] = _color(spec["up"])
            if "down" in spec:
                d["down_color"] = _color(spec["down"])
            if "ms" in spec:
                d["flash_ms"] = int(spec["ms"])
        if isinstance(formats.get(n), dict) and "max_hz" in formats[n]:
            d["max_hz"] = int(formats[n]["max_hz"])
        if render and n in render:
            d.update(_render_def(render[n]))
        if d["number_format"] or d.get("flash") or d.get("max_hz") or d.get("render"):
            out.append(d)
    return out


# ---------------------------------------------------------------------------
# Bound tables
# ---------------------------------------------------------------------------
def _rows_per_frame(cols, n) -> int:
    """Rows per frame so a frame stays well inside the channel (16 MB)."""
    if n == 0:
        return 1
    width = 0
    for c in cols:
        width += 8 if not isinstance(c, list) else 16
    return max(1, min(n, (16 << 20) // max(width, 8)))


def _slice(col, a, b):
    return col[a:b] if not hasattr(col, "iloc") else col.iloc[a:b]


class BoundTable:
    """A table fed from Python by key. Rows are found by the key column; update() sends only what changed."""

    def __init__(self, name: str, key: Optional[str] = None):
        self._name, self._key = name, key

    def __repr__(self):
        return f"<BoundTable {self._name}>"

    @property
    def name(self) -> str:
        return self._name

    @property
    def key(self) -> Optional[str]:
        if self._key is None:
            d = self.definition()
            self._key = d.get("key") or None
        return self._key

    def definition(self) -> dict:
        for d in _conn().request("bind.list", {}):
            if d["name"].lower() == self._name.lower():
                return d
        raise YcError(f"No bound table called {self._name!r}", 9, "E_NOT_FOUND")

    # ---- data ----
    def _send(self, op: int, data, wait: bool):
        names, cols = _columns(data)
        if not names:
            return None
        cols = [_to_list_or_array(c) for c in cols]
        key = self.key
        key_col = names.index(key) if key in names else -1
        if op in (UPSERT, DELETE) and key_col < 0:
            raise YcError(f"bind: the data need the key column {key!r}", 1004, "E_ARG")
        n = len(cols[0])
        step = _rows_per_frame(cols, n)
        ch = _channel()
        result = None
        for a in range(0, max(n, 1), step):
            b = min(n, a + step)
            part = [_slice(c, a, b) for c in cols]
            this_op = op if a == 0 or op != REPLACE else (UPSERT if key_col >= 0 else APPEND)
            frame, _ = encode_frame(self._name, this_op, names, part, key_col)
            last = b >= n
            result = ch.send(frame, sync=wait and last)
        return result

    def update(self, data=None, wait: bool = False, **columns):
        """Upsert by key: the rows of known keys change (only the columns sent), new keys are added at the end.
        `data` is a DataFrame / dict of columns / list of dicts; or pass columns as keywords: update(strike=[...], bid=[...]).
        wait=True waits until the rows are written and returns {rows, added, removed, cells}."""
        if data is None:
            data = columns
        return self._send(UPSERT, data, wait)

    upsert = update

    def append(self, data=None, wait: bool = False, **columns):
        """Add rows at the end (no key lookup)."""
        return self._send(APPEND, data if data is not None else columns, wait)

    def replace(self, data, wait: bool = False):
        """The table's rows become exactly these."""
        return self._send(REPLACE, data, wait)

    def delete(self, keys: Iterable, wait: bool = False):
        """Remove the rows of these keys."""
        keys = list(keys) if not hasattr(keys, "tolist") else keys.tolist()
        return self._send(DELETE, {self.key: keys}, wait)

    def sync(self, data, delete_missing: bool = True, wait: bool = False) -> dict:
        """Makes the table match `data` (a full snapshot, e.g. the whole positions DataFrame every second) and sends only
        the difference against the previous sync: new keys, the changed cells of known keys, and (delete_missing) the
        removal of keys that are gone. The first sync replaces the table's rows. Returns
        {"added", "changed", "removed", "cells"} (cells = values sent). Unchanged rows cost nothing in YCGrid, and change
        flashes only fire for values that moved."""
        names, cols = _columns(data)
        key = self.key
        if not names:
            return {"added": 0, "changed": 0, "removed": 0, "cells": 0}
        if key not in names:
            raise YcError(f"sync: the data need the key column {key!r}", 1004, "E_ARG")
        np = _np()
        cols = [_to_list_or_array(c) for c in cols]
        if np is not None:
            cols = [c if isinstance(c, np.ndarray) else np.asarray(c, dtype=object) for c in cols]
        k = names.index(key)
        keys = cols[k].tolist() if hasattr(cols[k], "tolist") else list(cols[k])
        n = len(keys)
        prev = getattr(self, "_synced", None)
        if prev is None or prev[0] != names:
            self._synced = (names, {kk: i for i, kk in enumerate(keys)}, cols)
            self._send(REPLACE if delete_missing else UPSERT, dict(zip(names, cols)), wait)
            return {"added": n, "changed": 0, "removed": 0, "cells": n * len(names)}
        _, pidx, pcols = prev

        def same(a, b):
            if np is not None and isinstance(a, np.ndarray):
                eq = np.asarray(a == b, dtype=bool) if a.shape == b.shape else np.zeros(len(a), dtype=bool)
                if a.dtype.kind == "f" and b.dtype.kind == "f":
                    eq |= np.isnan(a) & np.isnan(b)
                elif a.dtype.kind == "M" and b.dtype.kind == "M":
                    eq |= np.isnat(a) & np.isnat(b)
                elif a.dtype.kind == "O" or b.dtype.kind == "O":
                    eq |= np.fromiter((x != x and y != y for x, y in zip(a.tolist(), b.tolist())), dtype=bool, count=len(a))
                return eq
            return [x == y or (x != x and y != y) for x, y in zip(a, b)]

        pos = [pidx.get(kk, -1) for kk in keys]
        new_rows = [i for i, q in enumerate(pos) if q < 0]
        old_rows = [i for i, q in enumerate(pos) if q >= 0]
        old_pos = [pos[i] for i in old_rows]
        changed_cols, row_changed = [], [False] * len(old_rows)
        for j, c in enumerate(cols):
            if j == k or not old_rows:
                continue
            if np is not None:
                eq = same(c[old_rows], pcols[j][old_pos])
            else:
                eq = same([c[i] for i in old_rows], [pcols[j][q] for q in old_pos])
            diff = [not e for e in (eq.tolist() if hasattr(eq, "tolist") else eq)]
            if any(diff):
                changed_cols.append(j)
                row_changed = [r or d for r, d in zip(row_changed, diff)]
        upd = [old_rows[i] for i, r in enumerate(row_changed) if r]
        cells = 0

        def take(c, rows):
            return c[rows] if np is not None else [c[i] for i in rows]

        gone = []
        if delete_missing:
            now_keys = set(keys)
            gone = [kk for kk in pidx if kk not in now_keys]
        last = None
        if upd:
            sel = [k] + changed_cols
            last = self._send(UPSERT, {names[j]: take(cols[j], upd) for j in sel}, wait and not new_rows and not gone)
            cells += len(upd) * len(sel)
        if new_rows:
            last = self._send(UPSERT, {nm: take(c, new_rows) for nm, c in zip(names, cols)}, wait and not gone)
            cells += len(new_rows) * len(names)
        if gone:
            last = self.delete(gone, wait=wait)
            cells += len(gone)
        self._synced = (names, {kk: i for i, kk in enumerate(keys)}, cols)
        out = {"added": len(new_rows), "changed": len(upd), "removed": len(gone), "cells": cells}
        if wait and isinstance(last, dict):
            out["result"] = last
        return out

    def last_error(self) -> str:
        """The last error of updates sent without waiting ("" = none); reading it clears it."""
        return _conn().request("bind.error", {"name": self._name})

    # ---- look ----
    def set_format(self, column: Optional[str] = None, fmt=None, **formats):
        """set_format("iv", "pct") or set_format(iv="pct", edge="bp"); applied to every row, also rows added later."""
        if column is not None:
            formats[column] = fmt
        d = self.definition()
        cols = {c["name"]: c for c in d["columns"]}
        for n, spec in formats.items():
            c = cols.setdefault(n, {"name": n, "number_format": ""})
            c["number_format"] = number_format(spec)
        _conn().request("bind.set", {"name": self._name, "columns": list(cols.values())})

    def set_render(self, spec: Optional[dict] = None, **columns):
        """Column renderers, drawn by YCGrid over the cells every frame at any update rate:
        "bar" (value / column max: sizes, volumes), "bar2" (two-sided around zero: P&L, delta), "heat" (color from the
        column's low to high), "progress" (0..1, or of "max"), "trend" (the row's last "length" values as a line; hide
        the number with formats={column: ";;;"} to show only the line). A dict per column sets "color" (positive),
        "neg", "max" (the bar scale) and "length". None removes a column's renderer.

            t.set_render(Bid="bar", Ask={"type": "bar", "color": "#00c8ff"}, PnL="bar2", Last={"type": "trend", "length": 120})
        """
        spec = dict(spec or {}, **columns)
        d = self.definition()
        cols = {c["name"]: c for c in d["columns"]}
        for n, sp in spec.items():
            c = cols.setdefault(n, {"name": n, "number_format": ""})
            for k in ("render", "render_color", "render_neg", "render_max", "trend_len"):
                c.pop(k, None)
            if sp is not None:
                c.update(_render_def(sp))
        _conn().request("bind.set", {"name": self._name, "columns": list(cols.values())})

    def set_flash(self, columns=True, up=None, down=None, ms=None):
        """Flash cells when they change: up / down colors (RGB or "#rrggbb"), fade time in ms; columns=None turns it off."""
        d = self.definition()
        cols = {c["name"]: c for c in d["columns"]}
        names = self.columns if columns is True else ([] if columns is None else list(columns) if not isinstance(columns, str) else [columns])
        if columns is None:
            for c in cols.values():
                c["flash"] = 0
        for n in names:
            c = cols.setdefault(n, {"name": n, "number_format": ""})
            c["flash"] = 1
            if up is not None:
                c["up_color"] = _color(up)
            if down is not None:
                c["down_color"] = _color(down)
            if ms is not None:
                c["flash_ms"] = int(ms)
        _conn().request("bind.set", {"name": self._name, "columns": list(cols.values())})

    @property
    def stale_after(self) -> float:
        return float(self.definition()["stale_after"])

    @stale_after.setter
    def stale_after(self, seconds: float):
        _conn().request("bind.set", {"name": self._name, "stale_after": float(seconds or 0)})

    @property
    def age(self) -> Optional[float]:
        """Seconds since the newest row update (None before any data): freshness checks."""
        return self.definition().get("age")

    @property
    def record(self) -> bool:
        """Market data: recorded and replayed (yc.replay). False for the app's own tables."""
        return bool(self.definition().get("record", True))

    @record.setter
    def record(self, on: bool):
        _conn().request("bind.set", {"name": self._name, "record": bool(on)})

    # ---- reading ----
    @property
    def list_object(self):
        """The table as an Excel ListObject (Range, DataBodyRange, ListColumns...)."""
        from . import excel

        for ws in excel.Worksheets:
            for lo in ws.ListObjects:
                if lo.Name.lower() == self._name.lower():
                    return lo
        raise YcError(f"The table of {self._name!r} was deleted", 9, "E_NOT_FOUND")

    @property
    def range(self):
        return self.list_object.Range

    @property
    def columns(self) -> list[str]:
        return [c.Name for c in self.list_object.ListColumns]

    def to_frame(self):
        """The rows as a pandas DataFrame (a dict of columns without pandas), read through shared memory."""
        try:
            return read(self.name)
        except YcError as e:
            if e.Code != "E_UNSUPPORTED":  # an older YCGrid: read the cells
                raise
        lo = self.list_object
        names = self.columns
        body = lo.DataBodyRange
        vals = body.Value if body is not None else []
        if not isinstance(vals, (list, tuple)) or (vals and not isinstance(vals[0], (list, tuple))):
            vals = [[vals]] if body is not None and len(names) == 1 else [list(vals)] if vals else []
        cols = {n: [row[i] for row in vals] for i, n in enumerate(names)}
        try:
            import pandas

            return pandas.DataFrame(cols)
        except ImportError:
            return cols

    def unbind(self, delete: bool = False):
        """Stop the binding; delete=True also removes the table and its cells."""
        _conn().request("bind.remove", {"name": self._name, "delete": bool(delete)})


def bind(sheet, data=None, key: Optional[str] = None, name: Optional[str] = None, at="A1", formats: Optional[dict] = None,
         flash=None, stale_after: float = 0, show_age: bool = False, columns: Optional[list] = None, replace: bool = True,
         record: bool = True, render: Optional[dict] = None) -> BoundTable:
    """Bind table-like data to a new table on `sheet` at `at` (top-left of the header row) and return the BoundTable.

    key: the column rows are found by (updates by key); without it, update() appends.
    formats: per column, a format spec (see number_format) or an Excel format code.
    flash: True / a list of columns / {column: {"mode": "updown" | "change", "up": "#e5383b", "down": "#16a34a", "ms": 600}} (red up, green down).
    render: {column: "bar" | "bar2" | "heat" | "progress" | "trend" or {"type", "color", "neg", "max", "length"}}: drawn over
    the cells by YCGrid every frame (bid / ask size bars, two-sided P&L bars, heat colors, progress, each row's recent
    values as a mini line); see BoundTable.set_render.
    stale_after: seconds without an update after which a row turns gray (0 = never).
    columns: the column names when `data` is None (an empty table filled later).
    replace: an existing binding with this name is replaced (its table and cells are removed).
    record: market data, recorded by Trading > Record and replayed (yc.replay); False for tables the app keeps itself
    (positions, orders, strategy state), which stay live during a replay."""
    from . import excel

    names, cols = _columns(data) if data is not None else (list(columns or []), [])
    if key is not None and key not in names:
        if cols:  # before the table is made
            raise YcError(f"bind: the data have no key column {key!r} (columns: {', '.join(map(str, names))})", 1004, "E_ARG")
        names = [key] + names
    sheet = excel._resolve(sheet)
    ws = sheet if isinstance(sheet, excel.Worksheet) else excel.Worksheets(sheet)
    anchor = ws.Range(at) if isinstance(at, str) else excel._resolve(at)
    r0, c0 = anchor._a[0], anchor._a[1]
    date_cols = set()
    if cols:
        np = _np()
        for n, c in zip(names, cols):
            arr = _to_list_or_array(c)
            if np is not None and isinstance(arr, np.ndarray) and arr.dtype.kind == "M":
                date_cols.add(n)
            elif isinstance(arr, list) and arr and isinstance(next((v for v in arr if v is not None), None), (_dt.date, _dt.datetime)):
                date_cols.add(n)
    params = {"sheet": ws._id, "row": r0, "col": c0, "name": name or "", "key": key or "", "names": names,
              "columns": _column_defs(names, formats, flash, date_cols, render), "stale_after": float(stale_after or 0),
              "show_age": bool(show_age), "replace": bool(replace), "record": bool(record)}
    d = _conn().request("bind.create", params)
    t = BoundTable(d["name"], key)
    if cols:
        t._send(REPLACE if key is None else UPSERT, data, wait=True)
    return t


def table(name: str) -> BoundTable:
    """An existing binding (after reopening the workbook, or from another script)."""
    t = BoundTable(name)
    t.definition()
    return t


def tables() -> list[BoundTable]:
    return [BoundTable(d["name"], d.get("key") or None) for d in _conn().request("bind.list", {})]


# ---------------------------------------------------------------------------
# Columns on disk: Data View and plots that read files directly
# ---------------------------------------------------------------------------
def save_columns(data, folder: Optional[str] = None) -> dict:
    """Write each column as a .npy file YCGrid reads straight from disk (no data channel, no Python objects): numbers,
    datetimes (kept as datetime64), booleans, text. data: a pandas / polars DataFrame, a pyarrow Table, a dict of
    columns, or a Parquet / Feather / Arrow / CSV path (read with pandas). Returns {column: path}; folder defaults to a new temporary folder."""
    import tempfile

    import numpy as np

    if isinstance(data, str):
        import pandas as pd

        low = data.lower()
        data = pd.read_parquet(data) if low.endswith(".parquet") else pd.read_feather(data) if low.endswith((".feather", ".arrow")) else pd.read_csv(data)
    if isinstance(data, dict):
        cols = data
    elif hasattr(data, "column_names") and hasattr(data, "column"):  # a pyarrow Table (Arrow / Feather / Parquet in memory)
        cols = {str(n): data.column(n).to_numpy() for n in data.column_names}
    else:  # pandas or polars
        cols = {str(c): data[c] for c in data.columns}
    folder = folder or tempfile.mkdtemp(prefix="yccols_")
    os.makedirs(folder, exist_ok=True)
    out = {}
    for i, (name, col) in enumerate(cols.items()):
        a = np.asarray(col)
        if a.dtype.kind == "M":
            a = a.astype("datetime64[ns]")
        elif a.dtype.kind == "O":
            a = a.astype(str)  # text: fixed-width unicode
        elif a.dtype.kind not in "fiubUS":
            a = a.astype("float64")
        path = os.path.join(folder, f"{i:03d}_{re.sub(r'[^A-Za-z0-9_.-]', '_', str(name))[:40]}.npy")
        np.save(path, np.ascontiguousarray(a))
        out[str(name)] = path
    return out


class DataView:
    """A Data View window (yc.data.view): read-only, any number of rows."""

    def __init__(self, vid: int, title: str):
        self.id, self.title = vid, title

    def __repr__(self):
        return f"<DataView {self.title!r}>"

    def filter(self, column: str, expr: str = "") -> int:
        """Filter a column like the filter row: ">100", "<=5", "=7", "!=0", "a..b" (dates: Unix seconds), or text
        (contains, case-insensitive); "" clears it. Returns the rows shown."""
        return int(_conn().request("dataview.filter", {"id": self.id, "column": str(column), "filter": str(expr)}))

    def sort(self, column: Optional[str] = None, descending: bool = False) -> int:
        """Sort by a column (None = the data's order). Returns the rows shown."""
        return int(_conn().request("dataview.sort", {"id": self.id, "column": column or "", "descending": bool(descending)}))

    def state(self) -> dict:
        """rows, shown, first (the data row numbers of the first shown rows)."""
        return _conn().request("dataview.state", {"id": self.id})

    def close(self):
        _conn().request("dataview.close", {"id": self.id})


def view(data, title: Optional[str] = None) -> DataView:
    """Open a Data View: tens of millions of rows in a window that scrolls, sorts and filters in C++ (on all cores),
    with column statistics and To Sheet. data: a DataFrame, a pyarrow Table, a dict of columns, a Parquet / Feather / CSV path, or the
    {column: path} of save_columns(). Nothing goes through cells or the data channel: the columns are .npy files."""
    files = data if isinstance(data, dict) and all(isinstance(v, str) and v.endswith(".npy") for v in data.values()) else save_columns(data)
    name = title or (os.path.basename(data) if isinstance(data, str) else "Data")
    cols = [{"name": n, "path": os.path.abspath(pth)} for n, pth in files.items()]
    return DataView(int(_conn().request("dataview.open", {"title": name, "columns": cols})), name)

