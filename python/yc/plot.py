# SPDX-License-Identifier: Apache-2.0
"""[YC] Fast plots from any Python script: matplotlib-like calls, drawn by YCGrid with ImPlot.

    from yc import plot
    fig = plot.figure("Backtest", rows=2, link_x=True)
    fig[0].line(dates, equity, label="Equity")
    fig[1].fill_between(dates, drawdown, color="#c62828", label="Drawdown")
    fig[0].candles(t, o, h, l, c)
    plot.line(x, y)                             # quick: a new window with one line
    ln = fig[0].line([], [], label="PnL"); fig[0].set(follow=600)
    ln.append(t_new, pnl_new)                   # live: the newest 10 minutes in view

Figures are dockable windows of the running YCGrid (pan with the mouse, wheel to zoom, double-click to fit). Data go
through shared memory as arrays, and long lines are downsampled when drawn, so ten million points zoom smoothly.
Dates (datetime, numpy datetime64, pandas timestamps) make a time axis in local time. Figures belong to the session.
"""
from __future__ import annotations

import datetime as _dt
import itertools
import math
from typing import Iterable, Optional, Sequence

from ._conn import YcError, current as _conn
from .data import _channel, encode_frame

__all__ = ["figure", "Figure", "Axes", "Item", "Handle", "line", "scatter", "hist", "candles", "heatmap", "bar", "surface", "close_all"]

_CMAPS = {"deep": 0, "dark": 1, "pastel": 2, "paired": 3, "viridis": 4, "plasma": 5, "hot": 6, "cool": 7, "pink": 8, "jet": 9,
          "twilight": 10, "rdbu": 11, "brbg": 12, "piyg": 13, "spectral": 14, "greys": 15, "grays": 15}
_MARKERS = {None: 0, "": 0, "o": 1, "s": 2, "d": 3, "D": 3, "^": 4, "v": 5, "x": 6, "+": 7}
# YCGrid's own named colors (clear on white); on the dark trading theme use the bright ones in TRADING
_NAMED = {"red": "#e5383b", "green": "#16a34a", "blue": "#2f6fed", "orange": "#f08c00", "purple": "#7c3aed", "gray": "#6b7280",
          "grey": "#6b7280", "black": "#111111", "brown": "#92400e", "pink": "#db2777", "olive": "#65a30d", "cyan": "#0ea5e9",
          "yellow": "#ffd400", "magenta": "#ff4fd8", "white": "#ffffff"}
# the trading theme's palette (black background, Taiwanese trading terminals): lines, then up / down
TRADING = {"yellow": "#ffd400", "magenta": "#ff4fd8", "cyan": "#00d8ff", "green": "#39e07a", "orange": "#ff8a00",
           "white": "#ffffff", "violet": "#9d7bff", "up": "#ff3b3b", "down": "#00d25b", "text": "#e6e6e6", "grid": "#2a2a2a"}
_theme = "light"


def set_theme(name: str = "light"):
    """The theme of the figures made after this call: "light" (default) or "dark" (the dark trading theme:
    bright lines, red up / green down candles)."""
    global _theme
    if name not in ("light", "dark", "trading"):
        raise YcError(f"plot theme is light or dark, not {name!r}", 1004, "E_ARG")
    _theme = name
_figs: dict[int, "Figure"] = {}
_menus: dict = {}  # fig id -> [(text, callback) or None]
_items = itertools.count(1)


def _np():
    import numpy

    return numpy


def _argb(color) -> int:
    if color is None:
        return 0
    if isinstance(color, str):
        c = _NAMED.get(color.lower(), color)
        if c.startswith("#") and len(c) == 7:
            return 0xFF000000 | int(c[1:], 16)
        if c.startswith("#") and len(c) == 9:  # #rrggbbaa
            return (int(c[7:9], 16) << 24) | int(c[1:7], 16)
        raise YcError(f"Unknown color: {color!r} (use '#rrggbb', '#aarrggbb', a name like 'red' or 'gray', or an Excel RGB number)",
                      1004, "E_ARG")
    if isinstance(color, tuple):
        r, g, b = (int(v * 255) if isinstance(v, float) and v <= 1 else int(v) for v in color[:3])
        return 0xFF000000 | (r << 16) | (g << 8) | b
    v = int(color)  # Excel RGB number
    return 0xFF000000 | ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF)


def _array(v):
    """A column as float64 numpy; dates -> Unix seconds. Returns (array, is_time)."""
    np = _np()
    if v is None:
        return None, False
    mod = type(v).__module__.split(".")[0]
    if mod == "pandas":
        if hasattr(v, "to_numpy"):
            v = v.to_numpy()
    elif mod == "polars":
        v = v.to_numpy()
    a = np.asarray(v)
    if a.dtype.kind == "M":
        if mod == "pandas" and getattr(getattr(v, "dtype", None), "tz", None) is not None:
            return a.astype("datetime64[ns]").astype("int64") / 1e9, True  # time zone aware: UTC instants
        ns = a.astype("datetime64[ns]")
        secs = ns.astype("int64").astype("float64") / 1e9
        secs[np.isnat(ns)] = np.nan
        return _local_to_epoch(secs), True
    first = next((x for x in a.flat if x is not None), None) if a.dtype.kind == "O" and a.size else None
    if first is not None and (isinstance(first, (_dt.datetime, _dt.date)) or getattr(first, "dtype", None) is not None and first.dtype.kind == "M"):
        out = np.array([_epoch(x) for x in a.flat], dtype="float64").reshape(a.shape)
        return out, True
    try:
        return a.astype("float64"), False
    except (TypeError, ValueError):
        sample = next((x for x in a.flat if isinstance(x, str)), a.flat[0] if a.size else None)
        raise YcError(f"plot: values must be numbers or dates, not text such as {str(sample)!r} (for categories use numbers and "
                      "text labels, e.g. bar(range(n), values) and text())", 1004, "E_ARG") from None


def _local_to_epoch(wall):
    """Naive times are local wall-clock times, like the sheet's: shift them to Unix seconds."""
    np = _np()
    ok = wall[np.isfinite(wall)]
    if not ok.size:
        return wall

    def offset(w):
        t = _dt.datetime(1970, 1, 1) + _dt.timedelta(seconds=float(w))
        return t.astimezone().utcoffset().total_seconds()

    lo, hi = offset(ok.min()), offset(ok.max())
    if lo == hi:
        return wall - lo
    hours = np.floor(wall / 3600.0)  # time zones with daylight saving: one offset per hour
    uniq, inv = np.unique(np.nan_to_num(hours), return_inverse=True)
    offs = np.array([offset(h * 3600.0) for h in uniq])
    return wall - offs[inv].reshape(wall.shape)


def _side(s) -> float:
    """+1 for a buy, -1 for a sell: numbers by sign, text by its first letter (B / L buy, S sell)."""
    if isinstance(s, str):
        c = s.strip().upper()[:1]
        if c in ("B", "L"):
            return 1.0
        if c == "S":
            return -1.0
        raise YcError(f"signals: side {s!r} is neither a buy nor a sell", 1004, "E_ARG")
    return 1.0 if float(s) > 0 else -1.0


def _xval(x) -> float:
    """A number, or a date / time as Unix seconds."""
    try:
        return float(x)
    except (TypeError, ValueError):
        return _epoch(x)


def _epoch(x) -> float:
    if x is None:
        return math.nan
    if type(x).__module__.split(".")[0] == "numpy" and hasattr(x, "dtype") and x.dtype.kind == "M":
        return float(_local_to_epoch(_np().array([x.astype("datetime64[ns]").astype("int64") / 1e9]))[0])
    if hasattr(x, "to_pydatetime"):  # pandas Timestamp: its timestamp() reads naive times as UTC
        x = x.to_pydatetime()
    if isinstance(x, _dt.datetime):
        return x.timestamp()
    return _dt.datetime(x.year, x.month, x.day).timestamp()


class Item:
    """A plotted item: set_data / append replace or extend its data; style(...) changes its look."""

    def __init__(self, ax: "Axes", kind: str, item_id: str):
        self.ax, self.kind, self.id = ax, kind, item_id

    def __repr__(self):
        return f"<plot {self.kind} {self.id!r} in figure {self.ax.fig.id}, plot {self.ax.index}>"

    def data(self) -> dict:
        """[YC] The columns YCGrid holds for this item (plot.item_data)."""
        return item_data(self)

    def _send(self, op: str, cols: dict, style: Optional[dict] = None):
        p = {"fig": self.ax.fig.id, "ax": self.ax.index, "item": self.id, "kind": self.kind, "op": op}
        if style:
            p.update(style)
        names = list(cols)
        arrays = [cols[n] for n in names]
        if not names:
            return _conn().request("plot.item", p)
        np = _np()
        n = max(len(a) for a in arrays) if arrays else 0
        step = 1_000_000
        if all(isinstance(a, np.ndarray) for a in arrays) and n > step:
            # very long data: several frames (the first replaces, the rest append)
            for i in range(0, n, step):
                part = [a[i:i + step] if len(a) == n else a for a in arrays]
                self._frame(p if i == 0 else {**p, "op": "append"}, names, part)
            return None
        return self._frame(p, names, arrays)

    def _frame(self, p, names, arrays):
        frame, _ = encode_frame(f"@plot{self.ax.fig.id}", 2, names, arrays)
        ch = _channel()
        return ch.send_request("plot.item", p, frame)

    def set_data(self, *args, extent=None, **cols):
        """Replace the data: same arguments as the call that made the item (x, y / values / t, o, h, l, c / z). A heatmap
        may change its shape, and its extent=(x0, x1, y0, y1)."""
        style = None
        if self.kind == "heatmap" and args:
            np = _np()
            shape = np.shape(args[0])
            if len(shape) == 2:
                style = {"rows": int(shape[0]), "cols": int(shape[1])}
            if extent is not None:
                style = {**(style or {}), "extent": self.ax._extent(extent)}
        cols = self.ax._columns(self.kind, args, cols)
        self._send("set", cols, style)
        return self

    def append(self, *args, **cols):
        """Add points at the end (live data); `capacity` given when the item was made keeps the newest points."""
        cols = self.ax._columns(self.kind, args, cols)
        self._send("append", cols)
        return self

    def style(self, **style):
        self._send("set", {}, _style(style))
        return self

    def remove(self):
        _conn().request("plot.item", {"fig": self.ax.fig.id, "ax": self.ax.index, "item": self.id, "op": "remove"})


def _style(kw: dict) -> dict:
    out = {}
    for k, v in kw.items():
        if v is None and k not in ("vmin", "vmax"):
            continue
        if k == "color":
            out["color"] = _argb(v)
        elif k == "marker":
            out["marker"] = _MARKERS.get(v, 1) if not isinstance(v, int) else v
        elif k == "cmap":
            # ImPlot's maps by number; YCGrid's own ("pnl": loss green, zero black, profit red; "pnl_light") by name
            name = str(v).lower()
            out["cmap"] = v if isinstance(v, int) else (name if name in ("pnl", "pnl_light") else _CMAPS.get(name, 4))
        elif k in ("label", "text", "downsample", "fmt"):
            out["text" if k == "fmt" else k] = str(v)
        elif k in ("bins", "capacity", "rows", "cols"):
            out[k] = int(v)
        else:
            out[k] = v
    return out


_handles = itertools.count(1)
_callbacks: dict = {}   # (figure id, event) -> [callbacks]
_handle_objs: dict = {}  # (figure id, handle id) -> Handle
_poller = {"thread": None, "figs": set()}


class Handle:
    """A draggable line, point or band (Axes.drag_line / drag_point / drag_band)."""

    def __init__(self, ax, kind, y, x, label, color, width, on_move, on_release, y1=None, resize=True, on_close=None,
                 closable=False, fit=False):
        self.ax, self.kind, self.id = ax, kind, f"h{next(_handles)}"
        self.x, self.y = x, y
        self.y1 = y1
        self.label = label
        self.on_move, self.on_release, self.on_close = on_move, on_release, on_close
        p = {"fig": ax.fig.id, "ax": ax.index, "id": self.id, "kind": kind, "x": float(x), "y": float(y), "width": float(width)}
        if y1 is not None:
            p["y1"], p["resize"] = float(y1), bool(resize)
        if closable or on_close is not None:
            p["closable"] = True
        if fit:
            p["fit"] = True  # counts in the plot's fit (handles never move the view by default)
        if label is not None:
            p["label"] = str(label)
        if color is not None:
            p["color"] = _argb(color)
        _conn().request("plot.handle", p)
        _handle_objs[(ax.fig.id, self.id)] = self
        ax.fig._listen()

    @property
    def value(self) -> float:
        """The y of a horizontal line (a band: its middle), the x of a vertical one."""
        if self.kind == "band":
            return (self.y + self.y1) / 2
        return self.y if self.kind == "hline" else self.x

    @property
    def y0(self) -> float:
        """A band's bottom."""
        return self.y

    def set(self, value=None, x=None, y=None, label=None, y0=None, y1=None, color=None):
        """Move it from Python (an order's new price, a time) or change its label or color; a band: y0 / y1, or value =
        its new middle."""
        if value is not None:
            if self.kind == "band":
                half = (self.y1 - self.y) / 2
                y0, y1 = float(value) - half, float(value) + half
            elif self.kind == "hline":
                y = value
            else:
                x = value
        if y0 is not None:
            y = y0
        p = {"fig": self.ax.fig.id, "ax": self.ax.index, "id": self.id}
        if x is not None:
            self.x = p["x"] = float(x)
        if y is not None:
            self.y = p["y"] = float(y)
        if y1 is not None:
            self.y1 = p["y1"] = float(y1)
        if label is not None:
            self.label = p["label"] = str(label)
        if color is not None:
            p["color"] = _argb(color)
        _conn().request("plot.handle", p)
        return self

    def remove(self):
        _conn().request("plot.handle", {"fig": self.ax.fig.id, "ax": self.ax.index, "id": self.id, "op": "remove"})
        _handle_objs.pop((self.ax.fig.id, self.id), None)

    def __repr__(self):
        return f"<Handle {self.id} {self.kind} {self.label or ''} at {self.value:g}>"


class Event(dict):
    """An event of a figure: type ("click", "dblclick", "hover", "select", "drag", "release", "menu"), fig, ax (the
    plot), x / y in data coordinates (time axes: Unix seconds), item and index (the nearest data point of the plot's
    first series), ctrl / shift / alt (the modifier keys); click: button ("left", "middle", "right" - right clicks
    with fig.on("click", cb, right=True) or a context menu; Shift + right click opens the plot's own menu then);
    select: x0, x1, y0, y1; drag / release: handle (the Handle object; a band also y0, y1); close: handle (its "x"
    button was clicked); menu: item (its index) and text."""

    __getattr__ = dict.get


def _dispatch(ev: dict) -> None:
    """An event from YCGrid (pushed to the host, or polled): run its callbacks."""
    e = Event(ev)
    fig = e.get("fig")
    if e.get("type") == "menu":  # fig.context_menu: the chosen item's callback
        items = _menus.get(fig) or []
        i = e.get("item", -1)
        if 0 <= i < len(items) and items[i] is not None and items[i][1] is not None:
            _safe(items[i][1], e)
    if e.get("type") in ("drag", "release"):
        h = _handle_objs.get((fig, e.get("handle")))
        if h is not None:
            h.x, h.y = e.get("x", h.x), e.get("y", h.y)
            if "y1" in e:
                h.y, h.y1 = e.get("y0", h.y), e["y1"]
            e["handle"] = h
            cb = h.on_release if e["type"] == "release" else h.on_move
            if cb is not None:
                _safe(cb, h)
    if e.get("type") == "close":
        h = _handle_objs.get((fig, e.get("handle")))
        if h is not None:
            e["handle"] = h
            if h.on_close is not None:
                _safe(h.on_close, h)
    for cb in list(_callbacks.get((fig, e.get("type")), [])):
        _safe(cb, e)


def _safe(cb, arg):
    try:
        cb(arg)
    except Exception:  # noqa: BLE001 - one handler's error doesn't stop the others
        import traceback

        traceback.print_exc()


def _in_host() -> bool:
    try:
        from . import host

        return host._conn_obj is not None
    except Exception:  # noqa: BLE001
        return False


def _poll_loop():
    """Scripts (not the host): fetch the events of the figures with callbacks and dispatch them."""
    import time as _time

    while _poller["figs"]:
        for fid in list(_poller["figs"]):
            try:
                for ev in _conn().request("plot.events", {"fig": fid}) or []:
                    _dispatch(ev)
            except Exception:  # noqa: BLE001 - the figure was closed or YCGrid exited
                _poller["figs"].discard(fid)
        _time.sleep(0.03)
    _poller["thread"] = None


class Axes:
    """One plot of a figure."""

    def __init__(self, fig: "Figure", index: int):
        self.fig, self.index = fig, index
        self._time = None

    def __repr__(self):
        return f"<plot {self.index} of figure {self.fig.id}>"

    def set(self, title=None, xlabel=None, ylabel=None, time=None, log_y=None, legend=None, xlim=None, ylim=None, follow=None,
            equal=None, zlabel=None, view=None, yfit=None, ywidth=None) -> "Axes":
        """Titles, limits, a time x axis, a log y axis; follow=N keeps the newest N x units (seconds on a time axis) in view.
        yfit (with follow): "auto" fits y to the visible data every frame; "stable" moves y only when the data come
        near its edges or fill less than a third of it (a live chart that doesn't jitter). While the user drags a
        handle the plot's limits stay put. ywidth: the y axis's width - points (fixed, wider labels or tags still widen
        it), "stable" (only grows: tags that come and go don't move the plot; the default with yfit="stable") or
        "labels" (as wide as its labels and tags this moment). 3-D surfaces: zlabel, and view=(azimuth, elevation) in
        degrees."""
        p = {"fig": self.fig.id, "ax": self.index}
        for k, v in (("title", title), ("xlabel", xlabel), ("ylabel", ylabel), ("time", time), ("log_y", log_y), ("legend", legend),
                     ("follow", follow), ("equal", equal), ("zlabel", zlabel), ("yfit", yfit), ("ywidth", ywidth)):
            if v is not None:
                p[k] = v
        if view is not None:
            p["view"] = [float(view[0]), float(view[1])]
        if xlim is not None:
            p["xlim"] = [_xval(x) for x in xlim]
        if isinstance(ylim, str) and ylim == "auto":
            p["ylim_auto"] = True  # y fits the data again
        elif ylim is not None:
            p["ylim"] = [float(y) for y in ylim]
        _conn().request("plot.axes", p)
        return self

    def _columns(self, kind, args, kw):
        np = _np()
        cols = {}

        def put(name, v):
            a, is_time = _array(v)
            if a is not None:
                if a.ndim > 1 and kind != "heatmap" and max(a.shape) != a.size:
                    raise YcError(f"{kind}: {name} must be one-dimensional, not of shape {a.shape}; call {kind}() once per "
                                  "column for several series", 1004, "E_ARG")
                cols[name] = a.ravel()
                if is_time and name == "x" and not self._time:
                    self._time = True
                    self.set(time=True)

        if kind in ("line", "scatter", "step", "bar", "fill"):
            if len(args) == 1:  # y only (a pandas Series: its index is x)
                y = args[0]
                if hasattr(y, "index") and type(y).__module__.split(".")[0] == "pandas":
                    put("x", y.index)
                    put("y", y)
                else:
                    a, _ = _array(y)
                    put("x", np.arange(len(a), dtype="float64"))
                    put("y", a)
            elif len(args) >= 2:
                put("x", args[0])
                put("y", args[1])
                if kind == "fill" and len(args) >= 3 and args[2] is not None:
                    put("y2", args[2])
        elif kind in ("hist", "hline", "vline"):
            if args:
                put("values", args[0])
        elif kind == "candles":
            for name, v in zip(("x", "open", "high", "low", "close"), args):
                put(name, v)
        elif kind == "heatmap":
            if args:
                z, _ = _array(args[0])
                cols["z"] = z.ravel()
        elif kind == "barh":
            if len(args) >= 2:
                put("x", args[0])  # where each bar is on the y axis
                put("y", args[1])  # its length
        elif kind == "errorbar":
            for name, v in zip(("x", "y", "neg", "pos"), args):
                put(name, v)
        elif kind in ("rects", "arrows"):
            names = ("x0", "x1", "y0", "y1") if kind == "rects" else ("x0", "y0", "x1", "y1")
            for name, v in zip(names, args):
                put(name, v)
        elif kind in ("hband", "vband"):
            for name, v in zip(("y0", "y1") if kind == "hband" else ("x0", "x1"), args):
                put(name, v)
        elif kind in ("polygon", "annotations"):
            for name, v in zip(("x", "y"), args):
                put(name, v)
        elif kind == "signals":
            for name, v in zip(("x", "y"), args):
                put(name, v)
            if len(args) >= 3:
                cols["side"] = np.array([_side(s) for s in args[2]], dtype="float64")
        labels = kw.pop("labels", None)
        colors = kw.pop("colors", None)
        for k, v in kw.items():
            put(k, v)
        if colors is not None:  # one color per point / bar / rect
            cols["c"] = np.array([float(_argb(c)) for c in colors], dtype="float64")
        if labels is not None:
            cols["labels"] = ["" if t is None else str(t) for t in labels]
        lens = {k: len(v) for k, v in cols.items()}
        if kind not in ("heatmap", "hist", "hline", "vline", "polygon") and len(set(lens.values())) > 1:
            raise YcError(f"{kind}: the data must have the same length, but " + ", ".join(f"{k} has {n}" for k, n in lens.items()),
                          1004, "E_ARG")
        return cols

    def _add(self, kind, args, cols_kw, style) -> Item:
        it = Item(self, kind, f"i{next(_items)}")
        cols = self._columns(kind, args, cols_kw)
        it._send("set", cols, _style(style))
        return it

    # ---- [YC] items fed by topics: YCGrid builds them from the updates, with no Python in the loop ----
    def _live(self, kind, live, style) -> Item:
        it = Item(self, kind, f"i{next(_items)}")
        if not self._time:
            self._time = True
            self.set(time=True)
        p = {"fig": self.fig.id, "ax": self.index, "item": it.id, "kind": kind, "op": "set", "live": live}
        p.update(_style(style))
        _conn().request("plot.item", p)
        return it

    def candles_live(self, topic: str, seconds: float = 60, volume_topic: Optional[str] = None, keep: int = 3000,
                     volume_ax: Optional["Axes"] = None, label=None, cumulative_volume: bool = False) -> Item:
        """Candles YCGrid builds itself from a topic's updates (Topics.Push from a feed, a script, the host or a
        replay): bars of `seconds` by the rules of yc.bars.BarBuilder (the forming bar included), the newest `keep`.
        volume_topic: the traded size of each update (cumulative_volume=True: a running total), drawn as bars on
        volume_ax in the candle colors. The chart follows live data and replays while Python is busy or paused.
        set_data(...) still loads history first; plot.item_data(item) reads the bars back (x, open, high, low,
        close, volume)."""
        if seconds <= 0:
            raise ValueError("seconds must be positive")
        live = {"topic": str(topic), "seconds": float(seconds), "keep": int(keep or 0)}
        if volume_topic:
            live.update(volume_topic=str(volume_topic), cumulative=bool(cumulative_volume))
            if volume_ax is not None:
                if volume_ax.fig is not self.fig:
                    raise ValueError("volume_ax must be a plot of the same figure")
                if not volume_ax._time:
                    volume_ax._time = True
                    volume_ax.set(time=True)
                live.update(volume_ax=volume_ax.index, volume_item=f"i{next(_items)}")
        it = self._live("candles", live, dict(label=label))
        if "volume_item" in live:
            it.volume = Item(volume_ax, "bar", live["volume_item"])
        return it

    def line_live(self, topic: str, capacity: int = 100_000, label=None, color=None, width=1.5, tag=False, fmt=None,
                  style=None) -> Item:
        """A line with a point for every update of a topic (x = its time), the newest `capacity` kept; drawn by YCGrid
        without Python."""
        return self._live("line", {"topic": str(topic), "keep": int(capacity or 0)},
                          dict(label=label, color=color, width=width, tag=bool(tag), fmt=fmt, style=style))

    def step_live(self, topic: str, capacity: int = 100_000, label=None, color=None, width=1.5, style=None) -> Item:
        """A step line of a topic's updates (a position, a state the strategy pushes)."""
        return self._live("step", {"topic": str(topic), "keep": int(capacity or 0)},
                          dict(label=label, color=color, width=width, style=style))

    def line(self, x, y=None, label=None, color=None, width=1.5, marker=None, capacity=0, downsample="minmax", tag=False,
             fmt=None, style=None) -> Item:
        """A line (x sorted ascending for downsampling; downsample "minmax", "lttb" or "none"). tag: the last value as a
        label on the y axis (fmt, e.g. "%.1f"); style "solid", "dash" or "dot"."""
        return self._add("line", (x,) if y is None else (x, y), {}, dict(label=label, color=color, width=width, marker=marker,
                                                                          capacity=capacity, downsample=downsample, tag=bool(tag),
                                                                          fmt=fmt, style=style))

    def step(self, x, y=None, label=None, color=None, width=1.5, capacity=0, style=None) -> Item:
        return self._add("step", (x,) if y is None else (x, y), {}, dict(label=label, color=color, width=width, capacity=capacity,
                                                                          style=style))

    def scatter(self, x, y, label=None, color=None, size=3, marker="o", capacity=0, downsample="auto", colors=None) -> Item:
        """Points. Over 100,000 points are drawn one per occupied 2 x 2 pixel cell of the view (outliers stay visible);
        downsample="none" draws every point. colors: one color per point."""
        return self._add("scatter", (x, y), {"colors": colors}, dict(label=label, color=color, size=size, marker=marker,
                                                                      capacity=capacity, downsample=downsample))

    def bar(self, x, height, width=None, label=None, color=None, colors=None) -> Item:
        """Vertical bars; colors: one color per bar (e.g. volume red on up bars, green on down bars)."""
        return self._add("bar", (x, height), {"colors": colors}, dict(label=label, color=color, bar_width=width))

    def barh(self, y, length, height=None, label=None, color=None, colors=None) -> Item:
        """Horizontal bars at y (a volume profile by price, a ranking): length along x, height their thickness."""
        return self._add("barh", (y, length), {"colors": colors}, dict(label=label, color=color, bar_width=height))

    def errorbar(self, x, y, err=None, neg=None, pos=None, horizontal=False, label=None, color=None) -> Item:
        """Error bars around y (or x with horizontal=True): err both ways, or neg / pos separately."""
        np = _np()
        if err is not None:
            neg = pos = err
        n = len(_array(y)[0])
        neg = np.zeros(n) if neg is None else neg
        pos = np.zeros(n) if pos is None else pos
        return self._add("errorbar", (x, y, neg, pos), {}, dict(label=label, color=color, horizontal=bool(horizontal)))

    def rects(self, x0, x1, y0, y1, colors=None, labels=None, values=None, fmt=None, color=None, edge=None, label=None) -> Item:
        """Rectangles in data coordinates, drawn by the hundred thousand: footprint cells (bid x ask volume per price per
        bar), volume profiles, TPO, zones, order lifetimes. colors: one per rect; labels (text) or values (numbers with
        fmt, e.g. "%.0f") are written inside when they fit; edge: an outline color. set_data(x0, x1, y0, y1, colors=...,
        labels=...) replaces them."""
        cols = {"colors": colors, "labels": labels}
        if values is not None:
            cols["v"] = values
        return self._add("rects", (x0, x1, y0, y1), cols, dict(label=label, color=color, edge=_argb(edge) if edge is not None else None,
                                                                fmt=fmt))

    def polygon(self, x, y, color=None, edge=None, width=1.0, label=None) -> Item:
        """Filled polygons (NaN in x / y separates several): channels, value areas, regions."""
        return self._add("polygon", (x, y), {}, dict(label=label, color=color, edge=_argb(edge) if edge is not None else None, width=width))

    def arrows(self, x0, y0, x1, y1, color=None, width=1.5, label=None) -> Item:
        """Arrows from (x0, y0) to (x1, y1): signals, order moves, annotations."""
        return self._add("arrows", (x0, y0, x1, y1), {}, dict(label=label, color=color, width=width))

    def signals(self, x, price, side, labels=None, buy_color=None, sell_color=None, size=8, label=None) -> Item:
        """Buy / sell marks on a candlestick chart: an up triangle just under price for a buy, a down triangle just
        above it for a sell, with an optional label each (e.g. "B 2"). side: +1 / -1, "B" / "S", "buy" / "sell",
        "long" / "short". Pass each bar's low as price for buys and its high for sells to keep the marks off the
        candles. Colors default to buy red, sell green (red up / green down). set_data(x, price, side, labels=...)."""
        return self._add("signals", (x, price, side), {"labels": labels},
                         dict(label=label, color=buy_color if buy_color is not None else "#e04040",
                              edge=_argb(sell_color if sell_color is not None else "#20b050"), size=size))

    def annotate(self, x, y, texts, color=None, label=None) -> Item:
        """Text labels with a colored background, pointing at (x, y): fills, events, notes."""
        return self._add("annotations", (x, y), {"labels": list(texts)}, dict(label=label, color=color))

    def plot_files(self, kind: str = "line", label=None, color=None, **columns) -> Item:
        """An item whose columns YCGrid reads from .npy files (yc.data.save_columns): hundreds of millions of points
        without passing through Python. kind: line, step, scatter, bar, candles (x, open, high, low, close), rects...;
        columns: the item's column names -> .npy paths, e.g. plot_files("line", x=files["time"], y=files["price"])."""
        import os

        it = Item(self, kind, f"i{next(_items)}")
        p = {"fig": self.fig.id, "ax": self.index, "item": it.id, "kind": kind, "op": "set",
             "files": {k: os.path.abspath(v) for k, v in columns.items()}}
        p.update(_style(dict(label=label, color=color)))
        if "x" in columns and str(columns["x"]).lower().endswith(".npy"):
            try:
                import numpy as np

                if np.load(columns["x"], mmap_mode="r").dtype.kind == "M" and not self._time:
                    self._time = True
                    self.set(time=True)
            except Exception:  # noqa: BLE001 - YCGrid reports a file it can't read
                pass
        _conn().request("plot.item", p)
        return it

    # ---- interaction: draggable handles and events ----
    def drag_line(self, value, horizontal=True, label=None, color=None, width=2.0, on_move=None, on_release=None,
                  on_close=None, closable=False, fit=False) -> "Handle":
        """A line the user drags (horizontal: a price level; else a time): on_move(handle) while dragging (at most 30
        times a second), on_release(handle) when let go. Its position is handle.value. Build "drag the order line to
        change its price" or "drag the stop" from it. on_close (or closable=True): an "x" button at the line's end;
        clicking it calls on_close(handle) (cancel the order) and sends a "close" event. fit=True: the line counts in
        the plot's fit (by default handles never change the view)."""
        return Handle(self, "hline" if horizontal else "vline", float(value) if horizontal else 0.0,
                      0.0 if horizontal else float(value), label, color, width, on_move, on_release, on_close=on_close,
                      closable=closable, fit=fit)

    def drag_point(self, x, y, label=None, color=None, size=2.0, on_move=None, on_release=None) -> "Handle":
        """A point the user drags; handle.x and handle.y follow it."""
        return Handle(self, "point", float(y), float(x), label, color, size, on_move, on_release)

    def drag_band(self, y0, y1, label=None, color=None, resize=True, on_move=None, on_release=None, fit=False) -> "Handle":
        """A price band across the whole plot that the user moves by its body and (resize=True) resizes by its top and
        bottom edges: a support / resistance zone. on_move(handle) while dragging, on_release(handle) when let go;
        handle.y0 / handle.y1 are its edges, handle.set(y0=..., y1=...) moves it from Python."""
        return Handle(self, "band", float(min(y0, y1)), 0.0, label, color, 1.5, on_move, on_release, y1=float(max(y0, y1)),
                      resize=resize, fit=fit)

    def hband(self, y0, y1, label=None, color=None, text=None, edge=None, colors=None, texts=None, fit=False) -> Item:
        """Horizontal bands across the whole plot at any zoom (y0..y1; lists for several): zones, value areas, limits.
        text / texts: written inside at the left; edge: an outline color; fit: count in the y auto-fit (the x auto-fit
        never sees them). set_data(y0, y1, colors=..., labels=...) replaces them."""
        y0s = y0 if hasattr(y0, "__len__") else [y0]
        y1s = y1 if hasattr(y1, "__len__") else [y1]
        labels = texts if texts is not None else ([text] * len(y0s) if text is not None else None)
        return self._add("hband", (y0s, y1s), {"colors": colors, "labels": labels},
                         dict(label=label, color=color, edge=_argb(edge) if edge is not None else None, fit=bool(fit)))

    def vband(self, x0, x1, label=None, color=None, text=None, edge=None, colors=None, texts=None, fit=False) -> Item:
        """Vertical bands over the whole height (x0..x1, times as dates): sessions, news windows, the replay range."""
        x0s = x0 if hasattr(x0, "__len__") and not isinstance(x0, str) else [x0]
        x1s = x1 if hasattr(x1, "__len__") and not isinstance(x1, str) else [x1]
        labels = texts if texts is not None else ([text] * len(x0s) if text is not None else None)
        return self._add("vband", (x0s, x1s), {"colors": colors, "labels": labels},
                         dict(label=label, color=color, edge=_argb(edge) if edge is not None else None, fit=bool(fit)))

    def hist(self, values, bins=None, label=None, color=None) -> Item:
        """A histogram; bins: a count, or "sturges" / "sqrt" / "rice" / "scott" (default Sturges)."""
        named = {"sqrt": -1, "sturges": -2, "rice": -3, "scott": -4}
        b = named.get(bins, -2) if not isinstance(bins, int) else bins
        return self._add("hist", (values,), {}, dict(label=label, color=color, bins=b))

    def fill_between(self, x, y1, y2=None, ref=0.0, label=None, color=None) -> Item:
        return self._add("fill", (x, y1, y2), {}, dict(label=label, color=color, ref=float(ref)))

    def candles(self, t, open, high, low, close, label=None, up_color=None) -> Item:  # noqa: A002
        """Candlesticks (OHLC). Several candles per pixel are merged when zoomed out."""
        return self._add("candles", (t, open, high, low, close), {}, dict(label=label, color=up_color))

    def heatmap(self, z, xlabels: Optional[Sequence[str]] = None, ylabels: Optional[Sequence[str]] = None, cmap="viridis", vmin=None,
                vmax=None, fmt="%.2f", label=None, extent=None) -> Item:
        """A heatmap of a 2-D array (rows top to bottom). Labels are written along the axes; values are written in the
        cells when there are 400 or fewer. extent=(x0, x1, y0, y1) places it in data coordinates (times as datetimes
        or epoch seconds on a time axis, prices...), so lines and points can be drawn over it; the first row is at y1."""
        np = _np()
        a = np.asarray(z, dtype="float64")
        if a.ndim != 2:
            raise YcError("heatmap: z must be a 2-D array", 1004, "E_ARG")
        it = Item(self, "heatmap", f"i{next(_items)}")
        cols = {"z": a.ravel()}
        style = _style(dict(cmap=cmap, vmin=vmin, vmax=vmax, fmt=fmt, label=label, rows=a.shape[0], cols=a.shape[1],
                            extent=self._extent(extent)))
        if xlabels is not None or ylabels is not None:
            labels = [str(x) for x in (xlabels or [""] * a.shape[1])] + [str(y) for y in (ylabels or [""] * a.shape[0])]
            p = {"fig": self.fig.id, "ax": self.index, "item": it.id, "kind": "heatmap", "op": "set", **style}
            n = max(len(labels), a.size)
            names = ["z", "labels"]
            zz = np.full(n, np.nan)
            zz[:a.size] = a.ravel()
            lab = labels + [""] * (n - len(labels))
            frame, _ = encode_frame(f"@plot{self.fig.id}", 2, names, [zz, lab])
            _channel().send_request("plot.item", p, frame)
            return it
        it._send("set", cols, style)
        return it

    def _extent(self, extent):
        if extent is None:
            return None
        if len(extent) != 4:
            raise YcError("heatmap: extent is (x0, x1, y0, y1)", 1004, "E_ARG")
        out = []
        for i, v in enumerate(extent):
            a, is_time = _array([v])
            if is_time and i < 2 and not self._time:
                self._time = True
                self.set(time=True)
            out.append(float(a[0]))
        return out

    def surface(self, x, y, z, cmap="viridis", vmin=None, vmax=None, wireframe=False, label=None) -> Item:
        """A 3-D surface: z[j][i] over x[i] (columns) and y[j] (rows), e.g. implied volatility by strike and expiry. The
        plot becomes a 3-D view: drag to turn, wheel to zoom, double-click to reset; hover shows a point's values."""
        np = _np()
        xs, _ = _array(x)
        ys, _ = _array(y)
        zs = np.asarray(z, dtype="float64")
        if zs.shape != (len(ys), len(xs)):
            raise YcError(f"surface: z must have the shape (len(y), len(x)) = ({len(ys)}, {len(xs)}), not {zs.shape}", 1004, "E_ARG")
        it = Item(self, "surface", f"i{next(_items)}")
        n = zs.size  # one frame: x and y padded to the length of z
        pad = lambda a: np.concatenate([a.ravel(), np.full(n - a.size, np.nan)]) if a.size < n else a.ravel()  # noqa: E731
        it._send("set", {"x": pad(xs), "y": pad(ys), "z": zs.ravel()},
                 _style(dict(cmap=cmap, vmin=vmin, vmax=vmax, label=label, text="wireframe" if wireframe else "",
                             rows=len(ys), cols=len(xs))))
        return it

    def hline(self, values, label=None, color=None, width=1.0, tag=False, fmt=None, text=None, style=None) -> Item:
        """Horizontal lines across the plot. tag: each value on the y axis in the line's color, after text ("STOP 23,074";
        fmt e.g. "%.0f"); style "solid", "dash" or "dot"."""
        v = values if isinstance(values, (list, tuple)) or hasattr(values, "__len__") else [values]
        return self._add("hline", (v,), {}, dict(label=label, color=color, width=width, tag=bool(tag), fmt=fmt, tag_text=text, style=style))

    def vline(self, values, label=None, color=None, width=1.0, tag=False, fmt=None, text=None, style=None) -> Item:
        """Vertical lines over the plot; tag: each value on the x axis; style "solid", "dash" or "dot"."""
        v = values if isinstance(values, (list, tuple)) or hasattr(values, "__len__") else [values]
        return self._add("vline", (v,), {}, dict(label=label, color=color, width=width, tag=bool(tag), fmt=fmt, tag_text=text, style=style))

    def text(self, x, y, s: str, color=None) -> Item:
        it = Item(self, "text", f"i{next(_items)}")
        x = _epoch(x) if isinstance(x, (_dt.date, _dt.datetime)) else float(x)
        it._send("set", {}, _style(dict(text=s, ref=x, bar_width=float(y), color=color)))
        return it

    def clear(self):
        """Remove every item (the figure is drawn again empty)."""
        raise YcError("Axes.clear: remove items with Item.remove(), or make a new figure", 1004, "E_ARG")


class Figure:
    """A plot window: fig[i] / fig.axes(i) are its plots (row by row)."""

    def __init__(self, title: str = "", rows: Optional[int] = None, cols: Optional[int] = None, size=None, link_x: bool = False,
                 pos=None, theme: Optional[str] = None, sync_cursor: Optional[str] = None, key: Optional[str] = None,
                 replace: bool = True, reuse: bool = False, persist: Optional[bool] = None, height_ratios=None, width_ratios=None):
        self.theme = theme or _theme
        if self.theme not in ("light", "dark", "trading"):  # a misspelled theme would silently draw the light one
            raise YcError(f"plot theme is light or dark, not {self.theme!r}", 1004, "E_ARG")
        p = {"title": title or "", "link_x": bool(link_x), "theme": self.theme}
        if rows is not None or not (key and reuse):
            p["rows"] = int(rows or 1)
        if cols is not None or not (key and reuse):
            p["cols"] = int(cols or 1)
        if size is not None or pos is not None or not (key and reuse):
            size = size or (760, 480)
            p["size"] = [float(size[0]), float(size[1])]
        if sync_cursor:
            p["sync_cursor"] = str(sync_cursor)
        if pos is not None:
            p["place"] = [float(pos[0]), float(pos[1])]  # "pos" names a frame in the data channel
        if key:
            p["key"] = str(key)
            p["reuse"] = bool(reuse)  # replace (the default) sets the title and layout again
        if persist is not None:
            p["persist"] = bool(persist)
        if height_ratios is not None:
            p["height_ratios"] = [float(x) for x in height_ratios]
        if width_ratios is not None:
            p["width_ratios"] = [float(x) for x in width_ratios]
        self.id = int(_conn().request("plot.figure", p))
        self.key = key
        if key and reuse and (rows is None or cols is None):  # the existing window's layout
            info = next((f for f in figures() if f["id"] == self.id), {})
            rows, cols = rows or info.get("rows", 1), cols or info.get("cols", 1)
        self.rows, self.cols = int(rows or 1), int(cols or 1)
        self._axes = [Axes(self, i) for i in range(self.rows * self.cols)]
        _figs[self.id] = self

    def __repr__(self):
        return f"<figure {self.id}: {self.rows} x {self.cols} plots>"

    def cursor(self, x=None):
        """Show x (a number or a date / time) as the crosshair in every plot of this figure's cursor group (figures made
        with the same sync_cursor, or this figure's plots with link_x); None removes it. Hovering a plot takes over."""
        _conn().request("plot.cursor", {"fig": self.id, "x": math.nan if x is None else _xval(x)})

    def __getitem__(self, i) -> Axes:
        if isinstance(i, tuple):
            i = i[0] * self.cols + i[1]
        return self._axes[i]

    def axes(self, i=0) -> Axes:
        return self[i]

    def on(self, event: str, callback=None, right: bool = False):
        """Call callback(event) on "click", "dblclick", "hover", "select" (Ctrl + drag a rectangle) or "drag" /
        "release" (handles), or "follow" (a following plot paused by the user's zoom / pan, or back live: Event.paused);
        see Event for the fields (button, ctrl / shift / alt...). right=True: right clicks come as
        "click" with button "right" (the plot's own menu then opens with Shift + right click). In the YCGrid host
        (modules) events are pushed; in a script a background thread fetches them. Usable as a decorator."""
        if event not in ("click", "dblclick", "hover", "select", "drag", "release", "follow"):
            raise YcError(f"event is click, dblclick, hover, select, drag, release or follow, not {event!r}", 1004, "E_ARG")
        if right:
            self._right = True

        def register(cb):
            _callbacks.setdefault((self.id, event), []).append(cb)
            self._listen()
            return cb

        return register(callback) if callback is not None else register

    def off(self, event: Optional[str] = None):
        """Remove the callbacks of an event (all events when None)."""
        for key in [k for k in _callbacks if k[0] == self.id and (event is None or k[1] == event)]:
            _callbacks.pop(key)
        self._listen()

    # ---- [YC] docking: a figure is a window like a sheet or a panel ----
    _DOCK = {1: "left", 2: "right", 3: "top", 4: "bottom", 5: "tab"}

    def Dock(self, Position=2, Ratio=0.3, Container=None, Anchor=None):  # noqa: N802,N803
        """Dock it into the main window beside the sheets (Position ycDockLeft / ycDockRight / ycDockTop /
        ycDockBottom / ycDockTab, Ratio of the space), or into Container (ui.Containers.Add(...), or its name).
        [YC] Anchor=another docked Figure: beside that figure instead of the sheets, or with ycDockTab a tab in its
        place (two pages in one spot, the user switches by the tabs: a chart that needs the room of its own).
        A figure made with key= keeps its place by key: Layout.Save / Restore bring it back where it was, also for
        a figure made later with the same key."""
        pos = Position.lower() if isinstance(Position, str) else self._DOCK.get(int(Position), "right")
        if Container is not None:
            from . import ui

            c = Container if isinstance(Container, ui.Container) else ui.Container(str(Container))
            c.Dock(self, Position=pos, Ratio=Ratio)
            return self
        p = {"fig": self.id, "position": pos, "ratio": float(Ratio)}
        if Anchor is not None:
            if not isinstance(Anchor, Figure):
                raise YcError("Anchor is a Figure (the one to dock beside or to share a place with as a tab)", 1004, "E_ARG")
            p["anchor"] = Anchor.id
        _conn().request("plot.dock", p)
        return self

    @property
    def Locked(self) -> bool:  # noqa: N802
        """[YC] True: the user can't move, undock, resize or close the figure's window (zoom, pan, handles and the
        context menu still work). A figure with a key keeps it in the workbook's layout."""
        return bool(_conn().request("ui.lock", {"kind": "figure", "name": self._lock_name()}))

    @Locked.setter
    def Locked(self, v) -> None:  # noqa: N802
        _conn().request("ui.lock", {"kind": "figure", "name": self._lock_name(), "locked": bool(v)})

    def _lock_name(self) -> str:
        return self.key if self.key else f"#{self.id}"

    @property
    def Docked(self) -> bool:  # noqa: N802
        """[YC] True when the figure sits in a dock (the main window or a container), False when it floats."""
        for f in _conn().request("plot.figures", {}):
            if f.get("id") == self.id:
                return bool(f.get("docked", False))
        return False

    @property
    def following(self) -> bool:
        """[YC] False while the user has paused a following plot (zoom, pan); a click on its "Paused" badge,
        fig.input("live") or Axes.set(follow=...) goes back live."""
        for f in _conn().request("plot.figures", {}):
            if f.get("id") == self.id:
                return not f.get("follow_paused", False)
        return True

    def Float(self, Left=None, Top=None, Width=None, Height=None):  # noqa: N802,N803
        """Take it out of the dock as a floating window (points from the main window's top-left)."""
        from . import ui

        ui.Float(self, Left=Left, Top=Top, Width=Width, Height=Height)
        return self

    def context_menu(self, items):
        """A right-click menu on the plots: [(text, callback), None (a separator), ...]; callback(event) gets the
        Event with x / y where the menu was opened (data coordinates) and ax. [] removes it. Shift + right click
        opens the plot's own menu."""
        items = [None if it is None else (str(it[0]), it[1]) for it in (items or [])]
        _menus[self.id] = items
        _conn().request("plot.menu", {"fig": self.id, "items": ["" if it is None else it[0] for it in items]})
        self._listen()
        return self

    def input(self, action: str, ax: int = 0, **kw):
        """[YC] What a mouse would do, in data coordinates, through the same events (tests, agents): "click" (x, y,
        button, ctrl, shift, alt), "double_click" (x, y), "select" (x0, x1, y0, y1), "drag_handle" (handle: a Handle,
        its id or label; x / y), "drag_band_edge" (handle, edge "top" / "bottom", y), "close_handle" (handle: its x
        button), "context_menu" (text, x, y), "wheel" (a following plot stops following), "live" (its Paused badge). The callbacks run as for the real mouse (in a script: when the events
        are polled)."""
        p = {"fig": self.id, "ax": int(ax), "action": str(action)}
        for k, v in kw.items():
            if k == "handle" and isinstance(v, Handle):
                v = v.id
            elif k in ("x", "x0", "x1") and not isinstance(v, (int, float)):
                v = _xval(v)
            p[k] = v
        return _conn().request("plot.input", p)

    def _listen(self):
        wanted = sorted({e for (fid, e) in _callbacks if fid == self.id} | ({"drag"} if any(k[0] == self.id for k in _handle_objs) else set()))
        wanted = [{"release": "drag"}.get(e, e) for e in wanted]
        if _menus.get(self.id):
            wanted.append("click")  # the menu's events come through the same channel
        push = _in_host()
        p = {"fig": self.id, "events": sorted(set(wanted)), "push": push}
        if getattr(self, "_right", False):
            p["right"] = True
        _conn().request("plot.subscribe", p)
        if not push and wanted:
            _poller["figs"].add(self.id)
            if _poller["thread"] is None:
                import threading

                _poller["thread"] = threading.Thread(target=_poll_loop, name="yc-plot-events", daemon=True)
                _poller["thread"].start()

    def poll(self) -> list:
        """The events since the last call (scripts that run their own loop instead of callbacks)."""
        return [Event(e) for e in (_conn().request("plot.events", {"fig": self.id}) or [])]

    def link_table(self, table, column: str) -> "Figure":
        """Follow a table: its active row's `column` value is marked on the plots, and a click on a plot selects the
        nearest row (e.g. a trade list next to the price chart)."""
        name = getattr(table, "name", None) or getattr(table, "Name", None) or str(table)
        _conn().request("plot.link", {"fig": self.id, "table": name, "column": column})
        return self

    def fit(self):
        _conn().request("plot.fit", {"fig": self.id})

    def close(self):
        try:
            _conn().request("plot.close", {"fig": self.id})
        finally:
            _figs.pop(self.id, None)


def figure(title: str = "", rows: Optional[int] = None, cols: Optional[int] = None, size=None, link_x: bool = False, pos=None,
           theme: Optional[str] = None, sync_cursor: Optional[str] = None, key: Optional[str] = None, replace: bool = True,
           reuse: bool = False, persist: Optional[bool] = None, height_ratios=None, width_ratios=None) -> Figure:
    """A plot window: rows x cols plots (1 x 1 by default), size (760 x 480) and pos (from the main window's top-left)
    in points; link_x shares the x range of the plots (and their crosshair); theme "light" or "dark" (the dark trading
    look; default: set_theme()); sync_cursor: a group name - the x under the mouse in one figure shows as a crosshair
    in every figure of the group. height_ratios / width_ratios: relative plot heights / widths ([4, 1, 1]).

    key names the window across connections and restarts: a figure with an existing key takes that window over - its
    place and size stay, its items are cleared; reuse=True also keeps its title and layout. persist: whether the
    figure stays open when the process that made it ends (default: False for the YCGrid host, True for scripts); a
    persistent figure of a stopped process shows "(stopped)" in its title."""
    return Figure(title, rows, cols, size, link_x, pos, theme, sync_cursor, key, replace, reuse, persist, height_ratios, width_ratios)


def figures() -> list:
    """Every figure of the session (any process): dicts with id, key, title, rows, cols, owner ("host", "script <pid>",
    "" for YCGrid's own), alive (its owner is still connected), persist, docked and axes (each plot's title and its
    items' kind and label)."""
    return list(_conn().request("plot.figures", {}))


def item_data(item: "Item") -> dict:
    """[YC] An item's columns as YCGrid has them ({"x": [...], "y": [...]} / x, open, high, low, close, volume for
    candles): what a live item built, or what was sent."""
    return _conn().request("plot.item_data", {"fig": item.ax.fig.id, "ax": item.ax.index, "item": item.id})


def close(key: Optional[str] = None, fig: Optional[int] = None) -> bool:
    """Close a figure by key (from any process) or by id; True when one was closed."""
    if key is not None:
        ok = bool(_conn().request("plot.close", {"key": str(key)}))
        for i, f in list(_figs.items()):
            if f.key == key:
                _figs.pop(i, None)
        return ok
    if fig is not None:
        try:
            _conn().request("plot.close", {"fig": int(fig)})
            return True
        except Exception:  # noqa: BLE001 - already closed
            return False
        finally:
            _figs.pop(int(fig), None)
    raise ValueError("plot.close needs key= or fig=")


def _quick(kind, title, *args, **kw):
    fig = figure(title or kind.title())
    return getattr(fig[0], kind)(*args, **kw), fig


def line(x, y=None, title=None, **kw) -> Item:
    """A new window with one line (like matplotlib's plt.plot)."""
    return _quick("line", title, x, y, **kw)[0]


def scatter(x, y, title=None, **kw) -> Item:
    return _quick("scatter", title, x, y, **kw)[0]


def hist(values, bins=None, title=None, **kw) -> Item:
    return _quick("hist", title, values, bins, **kw)[0]


def bar(x, height, title=None, **kw) -> Item:
    return _quick("bar", title, x, height, **kw)[0]


def candles(t, o, h, l, c, title=None, **kw) -> Item:  # noqa: E741
    return _quick("candles", title, t, o, h, l, c, **kw)[0]


def heatmap(z, title=None, **kw) -> Item:
    return _quick("heatmap", title, z, **kw)[0]


def surface(x, y, z, title=None, **kw) -> Item:
    return _quick("surface", title, x, y, z, **kw)[0]


def close_all():
    for f in list(_figs.values()):
        f.close()
