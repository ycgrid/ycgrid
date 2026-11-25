# SPDX-License-Identifier: Apache-2.0
"""[YC] Data feeds: the interface market data plugins implement, and two reference feeds.

A feed turns an outside source (a broker API, a WebSocket, a file another program writes) into rows for a bound table.
`Feed` does the parts every feed needs, so a plugin only parses messages:

- conflation by key (the newest row per symbol wins between two publishes) and batched publishing at `interval`;
- a heartbeat (`yc.safety`) and the table's staleness (`stale_after` on the binding);
- reconnecting with back-off when the source drops; `stop()` from any thread.

    from yc.feeds import Feed

    class MyFeed(Feed):
        def run_source(self):                    # runs on the feed's thread; return or raise to reconnect
            for msg in my_broker.stream():       # blocking reads
                if self.stopping: return
                self.emit({"Symbol": msg.code, "Bid": msg.bid, "Ask": msg.ask})

    feed = MyFeed(ws.bind(None, key="Symbol", columns=["Symbol", "Bid", "Ask"]), key="Symbol").start()

Reference feeds: `CsvTailFeed` (rows appended to a CSV by another program) and `WebSocketFeed` (JSON messages from a
WebSocket URL, needs the `websockets` package). A feed is a Python class: import it from your project or a package.
"""
from __future__ import annotations

import csv
import json
import threading
import time
from typing import Callable, Iterable, Optional


class Feed:
    """Base class of data feeds. Subclasses implement run_source() (blocking; call emit() with each row)."""

    name = "feed"

    def __init__(self, table=None, key: Optional[str] = None, interval: float = 0.1, heartbeat: float = 3.0,
                 reconnect: Iterable[float] = (0.5, 1, 2, 5, 10), on_rows: Optional[Callable[[list], None]] = None):
        """table: a BoundTable (ws.bind / yc.data.table) the rows go to; key: the column rows are conflated by;
        interval: seconds between publishes; on_rows: also called with each published batch (strategies, bars)."""
        self.table, self.key, self.interval = table, key, float(interval)
        self.heartbeat_timeout = heartbeat
        self.reconnect_delays = list(reconnect) or [1.0]
        self.on_rows = on_rows
        self.stopping = False
        self.connected = False
        self.rows_in = self.rows_out = self.reconnects = 0
        self.last_error: Optional[str] = None
        self._pending: dict = {}
        self._order: list = []
        self._lock = threading.Lock()
        self._threads: list = []

    # ---- for subclasses ----
    def run_source(self) -> None:  # pragma: no cover - interface
        """Connect and read until the source ends or stopping is set; return or raise to reconnect."""
        raise NotImplementedError

    def emit(self, row: dict) -> None:
        """A row from the source; with a key, a newer row for the same key replaces one not yet published."""
        with self._lock:
            self.rows_in += 1
            k = row.get(self.key) if self.key else len(self._order)
            if k not in self._pending:
                self._order.append(k)
            self._pending[k] = row if self.key is None or k not in self._pending else {**self._pending[k], **row}

    # ---- running ----
    def start(self) -> "Feed":
        self.stopping = False
        for target, name in ((self._source_loop, "source"), (self._publish_loop, "publish")):
            t = threading.Thread(target=target, name=f"yc-feed-{self.name}-{name}", daemon=True)
            t.start()
            self._threads.append(t)
        return self

    def stop(self, timeout: float = 2.0) -> None:
        self.stopping = True
        for t in self._threads:
            if t is not threading.current_thread():
                t.join(timeout)
        self._threads.clear()
        self.flush()

    def flush(self) -> list:
        """Publish what is pending now; returns the rows."""
        with self._lock:
            rows = [self._pending[k] for k in self._order]
            self._pending, self._order = {}, []
        if not rows:
            return rows
        self.rows_out += len(rows)
        if self.table is not None:
            cols = {}
            for r in rows:
                for c in r:
                    cols.setdefault(c, None)
            self.table.update({c: [r.get(c) for r in rows] for c in cols})
        if self.on_rows is not None:
            self.on_rows(rows)
        return rows

    def status(self) -> dict:
        return {"name": self.name, "connected": self.connected, "rows_in": self.rows_in, "rows_out": self.rows_out,
                "reconnects": self.reconnects, "last_error": self.last_error}

    def _source_loop(self):
        attempt = 0
        while not self.stopping:
            try:
                self.connected = True
                self.run_source()
                attempt = 0
            except Exception as e:  # noqa: BLE001 - the source failed: reconnect after a pause
                self.last_error = f"{type(e).__name__}: {e}"
            finally:
                self.connected = False
            if self.stopping:
                break
            self.reconnects += 1
            delay = self.reconnect_delays[min(attempt, len(self.reconnect_delays) - 1)]
            attempt += 1
            end = time.monotonic() + delay
            while not self.stopping and time.monotonic() < end:
                time.sleep(0.05)

    def _publish_loop(self):
        hb = None
        try:
            from . import safety

            hb = safety.heartbeat(f"feed {self.name}", timeout=self.heartbeat_timeout, alive=lambda: self.connected)
        except Exception:  # noqa: BLE001 - outside YCGrid (tests, scripts without a connection)
            hb = None
        while not self.stopping:
            time.sleep(self.interval)
            try:
                self.flush()
            except Exception as e:  # noqa: BLE001 - YCGrid closed or the table was removed
                self.last_error = f"{type(e).__name__}: {e}"
            if hb is not None:
                hb.beat()
        if hb is not None:
            hb.stop()


class CsvTailFeed(Feed):
    """Rows another program appends to a CSV file (a header line first), published as they arrive."""

    name = "csv"

    def __init__(self, path: str, table=None, key: Optional[str] = None, convert: Optional[Callable[[dict], dict]] = None,
                 from_start: bool = False, poll: float = 0.05, **kw):
        super().__init__(table, key, **kw)
        self.path, self.convert, self.from_start, self.poll = path, convert, from_start, poll
        self._pos = None

    def run_source(self):
        with open(self.path, newline="", encoding="utf-8") as f:
            header = next(csv.reader([f.readline()]))
            if self._pos is not None:
                f.seek(self._pos)
            elif not self.from_start:
                f.seek(0, 2)
            buf = ""
            while not self.stopping:
                chunk = f.readline()
                if not chunk:
                    self._pos = f.tell() - len(buf.encode("utf-8"))
                    time.sleep(self.poll)
                    continue
                buf += chunk
                if not buf.endswith("\n"):
                    continue  # the writer is in the middle of a line
                for values in csv.reader([buf]):
                    row = dict(zip(header, values))
                    self.emit(self.convert(row) if self.convert else row)
                buf = ""
                self._pos = f.tell()


class WebSocketFeed(Feed):
    """JSON messages from a WebSocket URL. parse(message) returns a row, a list of rows or None (ignore);
    subscribe: messages sent after connecting (each is JSON-encoded unless it is a str)."""

    name = "websocket"

    def __init__(self, url: str, table=None, key: Optional[str] = None, parse: Optional[Callable] = None,
                 subscribe: Iterable = (), headers: Optional[dict] = None, **kw):
        super().__init__(table, key, **kw)
        self.url, self.parse, self.subscribe_msgs, self.headers = url, parse, list(subscribe), headers or {}

    def run_source(self):
        try:
            from websockets.sync.client import connect
        except ImportError as e:
            raise RuntimeError("WebSocketFeed needs the websockets package (pip install websockets)") from e
        with connect(self.url, additional_headers=self.headers, open_timeout=10) as ws:
            for m in self.subscribe_msgs:
                ws.send(m if isinstance(m, str) else json.dumps(m))
            while not self.stopping:
                try:
                    msg = ws.recv(timeout=0.5)
                except TimeoutError:
                    continue
                data = json.loads(msg) if isinstance(msg, (str, bytes)) else msg
                out = self.parse(data) if self.parse else data
                if out is None:
                    continue
                for row in out if isinstance(out, list) else [out]:
                    self.emit(row)
