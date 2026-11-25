# SPDX-License-Identifier: Apache-2.0
"""YCP connection: frames of "4-byte length (little-endian) + msgpack" over Windows named pipes.

Message format (matches the C++ side, apps/ycgrid/python_bridge.cpp):
  request       [0, id, method, params]          Python -> YCGrid
  reply         [1, id, error, result]           YCGrid -> Python; error = None or [code, message]
  notification  [2, method, params]              YCGrid -> Python (only the host connection receives them)

The host connection uses two one-way pipes (writing still works while the reader thread blocks); standalone
scripts (controller) use one duplex pipe, request / reply, without notifications.
"""
from __future__ import annotations

import itertools
import json
import os
import queue
import struct
import sys
import threading
import time
from typing import Any, Callable, Optional

from . import _msgpack
from . import types as _types  # noqa: F401 (registers the extension types)

__all__ = ["Connection", "YcError", "YcConnectionError", "current", "set_current", "find_instances", "main_connection"]


class YcError(Exception):
    """Base class of every YCGrid error."""

    def __init__(self, Description: str = "", Number: int = 1004, Code: str = "E_FAIL", Source: str = "YCGrid"):  # noqa: N803
        super().__init__(Description)
        self.Number = Number
        self.Description = Description
        self.Code = Code
        self.Source = Source


class YcConnectionError(YcError, ConnectionError):
    def __init__(self, msg="The connection to ycgrid.exe was lost"):
        super().__init__(msg, 0, "E_CONNECTION")


# how long request() waits for YCGrid's reply when the caller gives no timeout (seconds; <= 0 = for ever). A reply
# that never comes (a message box nobody can click, a dropped frame) then raises E_TIMEOUT instead of hanging the
# script; yc.launch(hidden=True) lowers it, since nobody can answer a dialog there
# (YCGRID_REQUEST_TIMEOUT; the old name YC_REQUEST_TIMEOUT is still read)
REQUEST_TIMEOUT = float(os.environ.get("YCGRID_REQUEST_TIMEOUT") or os.environ.get("YC_REQUEST_TIMEOUT") or "600")
HIDDEN_REQUEST_TIMEOUT = 120.0


class Connection:
    def __init__(self, read_pipe, write_pipe, *, duplex: bool = False):
        self._r = read_pipe
        self._w = write_pipe
        self._duplex = duplex
        self.default_timeout: Optional[float] = None  # None: REQUEST_TIMEOUT
        self.hidden = False  # the YCGrid at the other end is a hidden instance (a test, an agent): nobody sees its desktop
        self._wlock = threading.Lock()
        self._ids = itertools.count(1)
        self._pending: dict[int, list] = {}
        self._plock = threading.Lock()
        self.notifications: "queue.Queue[tuple[str, Any]]" = queue.Queue()
        self.urgent: dict = {}  # notifications handled at once on the reader thread (method -> fn(params))
        self.closed = False
        self._reader: Optional[threading.Thread] = None
        if not duplex:
            self._reader = threading.Thread(target=self._read_loop, name="yc-reader", daemon=True)
            self._reader.start()

    # ---- Opening ----
    @classmethod
    def open_host(cls, pipe_in: str, pipe_out: str) -> "Connection":
        """host: pipe_in is YCGrid -> Python, pipe_out is Python -> YCGrid."""
        r = open(pipe_in, "rb", buffering=0)
        w = open(pipe_out, "wb", buffering=0)
        return cls(r, w)

    @classmethod
    def open_controller(cls, pipe: str) -> "Connection":
        f = open(pipe, "r+b", buffering=0)
        return cls(f, f, duplex=True)

    # ---- Frames ----
    def _read_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self._r.read(n - len(buf))
            if not chunk:
                raise YcConnectionError()
            buf += chunk
        return bytes(buf)

    def _read_frame(self) -> Any:
        (n,) = struct.unpack("<I", self._read_exact(4))
        return _msgpack.unpackb(self._read_exact(n))

    def _wait_frame(self, deadline: Optional[float], method: str) -> None:
        """Duplex pipe (one handle, read on the calling thread): wait until a frame's first bytes are there, or the
        deadline passes. Only the wait for a frame's start is bounded, so a frame is never left half read."""
        if deadline is None or sys.platform != "win32":
            return
        import ctypes
        import msvcrt

        k32 = ctypes.windll.kernel32
        handle = ctypes.c_void_p(msvcrt.get_osfhandle(self._r.fileno()))
        avail = ctypes.c_ulong(0)
        pause = 0.0
        while True:
            if not k32.PeekNamedPipe(handle, None, 0, None, ctypes.byref(avail), None):
                self.closed = True
                raise YcConnectionError()
            if avail.value:
                return
            if time.monotonic() > deadline:
                raise YcError(f"YCGrid didn't answer {method} in time (E_TIMEOUT)", 0, "E_TIMEOUT")
            time.sleep(pause)
            pause = min(0.004, pause + 0.0005)

    def _deadline(self, timeout: Optional[float]) -> Optional[float]:
        t = REQUEST_TIMEOUT if self.default_timeout is None else self.default_timeout
        if timeout is not None:
            t = timeout
        return None if t <= 0 else time.monotonic() + t

    def _write_frame(self, msg: Any) -> None:
        data = _msgpack.packb(msg)
        try:
            self._w.write(struct.pack("<I", len(data)) + data)
        except OSError as e:
            self.closed = True
            raise YcConnectionError() from e

    def _read_loop(self) -> None:
        try:
            while True:
                msg = self._read_frame()
                if not isinstance(msg, list) or not msg:
                    continue
                if msg[0] == 1:
                    _, rid, err, result = msg
                    with self._plock:
                        slot = self._pending.get(rid)
                    if slot is not None:
                        slot[1] = (err, result)
                        slot[0].set()
                elif msg[0] == 2:
                    h = self.urgent.get(msg[1])
                    if h is not None:
                        try:
                            h(msg[2] if len(msg) > 2 else None)
                        except Exception:  # noqa: BLE001 - an urgent handler mustn't stop the reader
                            pass
                        continue
                    self.notifications.put((msg[1], msg[2] if len(msg) > 2 else None))
        except (YcConnectionError, OSError, ValueError):
            pass
        finally:
            self.closed = True
            with self._plock:
                for slot in self._pending.values():
                    slot[1] = (["E_CONNECTION", "The connection to ycgrid.exe was lost"], None)
                    slot[0].set()
            self.notifications.put(("__closed__", None))

    # ---- Requests ----
    def request(self, method: str, params: Optional[dict] = None, timeout: Optional[float] = None) -> Any:
        """Send a request and return its result. timeout: seconds to wait for the reply (None: this connection's
        default_timeout, else REQUEST_TIMEOUT; <= 0: for ever); E_TIMEOUT is raised when it passes."""
        if self.closed:
            raise YcConnectionError()
        rid = next(self._ids)
        msg = [0, rid, method, params or {}]
        deadline = self._deadline(timeout)
        if self._duplex:
            with self._wlock:
                self._write_frame(msg)
                while True:
                    self._wait_frame(deadline, method)
                    reply = self._read_frame()  # a late reply to an earlier request that timed out is skipped here
                    if isinstance(reply, list) and reply and reply[0] == 1 and reply[1] == rid:
                        break
            err, result = reply[2], reply[3]
        else:
            ev = threading.Event()
            slot = [ev, None]
            with self._plock:
                self._pending[rid] = slot
            try:
                with self._wlock:
                    self._write_frame(msg)
                if not ev.wait(None if deadline is None else max(0.0, deadline - time.monotonic())):
                    raise YcError(f"YCGrid didn't answer {method} in time (E_TIMEOUT)", 0, "E_TIMEOUT")
                err, result = slot[1]
            finally:
                with self._plock:
                    self._pending.pop(rid, None)
        if err:
            code, message = (err + [""])[:2] if isinstance(err, list) else ("E_FAIL", str(err))
            raise _make_error(code, message)
        return result

    def notify(self, method: str, params: Optional[dict] = None) -> None:
        """A request without waiting for the reply (id = 0)."""
        with self._wlock:
            self._write_frame([0, 0, method, params or {}])

    def close(self) -> None:
        self.closed = True
        for f in {self._r, self._w}:
            try:
                f.close()
            except OSError:
                pass


def _make_error(code: str, message: str) -> Exception:
    from . import errors  # deferred import

    cls = errors.BY_CODE.get(code, YcError)
    if cls is YcError:
        return YcError(message, 1004, code)
    return cls(message)


# ---- The current connection (one per process) ----
_current: Optional[Connection] = None
# objects of another workbook (another YCGrid window) send their requests to that window: set while their methods run
_tls = threading.local()


def current() -> Connection:
    bound = getattr(_tls, "conn", None)
    if bound is not None:
        if bound.closed:
            raise YcConnectionError("That workbook has been closed")
        return bound
    if _current is None:
        raise YcConnectionError("Not connected to YCGrid: standalone scripts must call yc.connect() first")
    return _current


def main_connection() -> Optional[Connection]:
    return _current


def set_current(conn: Optional[Connection]) -> None:
    global _current
    _current = conn


def find_instances() -> list[dict]:
    """List running YCGrid instances (%LOCALAPPDATA%\\ycgrid\\instances\\*.json)."""
    base = os.path.join(os.environ.get("LOCALAPPDATA", ""), "ycgrid", "instances")
    out = []
    if not os.path.isdir(base):
        return out
    for name in os.listdir(base):
        if not name.endswith(".json"):
            continue
        try:
            with open(os.path.join(base, name), encoding="utf-8") as f:
                info = json.load(f)
        except (OSError, ValueError):
            continue
        out.append(info)
    return out
