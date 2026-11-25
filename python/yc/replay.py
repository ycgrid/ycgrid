# SPDX-License-Identifier: Apache-2.0
"""[YC] Record the data reaching bound tables, and replay it into the same tables.

    from yc import replay
    path = replay.record()                    # Trading > Record: a .yctape next to the workbook (recordings folder)
    ...
    replay.stop_recording()
    replay.play(path, speed=5)                # the same screen, on recorded data: live frames are ignored meanwhile
    replay.seek(120); replay.pause(); replay.resume(); replay.set_speed(0)   # 0 = as fast as possible
    replay.live()                             # back to live data; the tables are restored

    if replay.is_replaying():                 # e.g. route orders to a paper account
        ...

A recording starts with a snapshot of the bound tables, then has every frame with its arrival time. The screen's
Python code doesn't change: feeds keep sending (their frames are dropped while the replay owns a table), and code that
reads the tables sees the recorded values.
"""
from __future__ import annotations

import os
import time
from typing import Iterable, Optional

from ._conn import current as _conn

__all__ = ["record", "stop_recording", "play", "pause", "resume", "seek", "set_speed", "set_loop", "live", "status",
           "is_replaying", "wait", "mark", "pin", "compare"]


def record(path: Optional[str] = None, tables: Optional[Iterable[str]] = None, everything: bool = False) -> str:
    """Start recording (the market-data tables, or the named ones); returns the file's path. With no table marked as
    market data (bind(record=False) everywhere) it records the whole screen, as everything=True does; with nothing
    live to record it raises. everything=True is the
    time machine (Trading > Record > Record Everything): every bound table - the app's own positions, orders and
    strategy tables too - the yc.plot figures, the alerts and the marks, to replay the whole screen later."""
    p = {"path": os.path.abspath(path) if path else ""}
    if everything:
        p["tables"] = ["*"]
    elif tables is not None:
        p["tables"] = [str(t) for t in tables]
    return _conn().request("replay.record", p)


def mark(text: str, kind: str = "note"):
    """A marker on the recording's timeline (kind "note", "fill", "order", "alert" or your own): the replay bar shows
    it and a click goes there. Nothing happens when nothing is being recorded."""
    _conn().request("replay.mark", {"text": str(text), "kind": str(kind)})


def pin() -> bool:
    """Remember the recorded tables as they are now (in a replay: at its position) to compare() with another moment."""
    return bool(_conn().request("replay.pin", {}))


def compare(limit: int = 500) -> list:
    """What changed since pin(): [{table, key, column, then, now, change}] (change for numbers)."""
    return list(_conn().request("replay.compare", {"limit": int(limit)}))


def stop_recording():
    _conn().request("replay.stop_record", {})


def play(path: str, speed: float = 1.0, start: float = 0.0, loop: bool = False):
    """Replay a recording into its bound tables, from `start` seconds, at `speed` times the recorded pace."""
    _conn().request("replay.play", {"path": os.path.abspath(path), "speed": float(speed), "start": float(start), "loop": bool(loop)})


def pause():
    _conn().request("replay.pause", {"pause": True})


def resume():
    _conn().request("replay.pause", {"pause": False})


def seek(seconds: float):
    """Jump to a time of the recording (seconds from its start); the tables show their state at that time."""
    _conn().request("replay.seek", {"seconds": float(seconds)})


def set_speed(speed: float):
    _conn().request("replay.speed", {"speed": float(speed)})


def set_loop(loop: bool = True):
    _conn().request("replay.loop", {"loop": bool(loop)})


def live():
    """Stop the replay: the bound tables show live data again (restored to what they were before the replay)."""
    _conn().request("replay.live", {})


def status() -> dict:
    """mode ("live", "replay", "paused", "ended"), position / length (seconds), speed, frame / frames, tables, marks
    ([{t, kind, text}] on the timeline), pinned, and the recording: recording, record_path, record_seconds,
    record_frames."""
    return _conn().request("replay.status", {})


def is_replaying() -> bool:
    return status()["mode"] != "live"


def wait(timeout: Optional[float] = None, poll: float = 0.05) -> bool:
    """Wait until the replay reaches the end of the recording (or is stopped); False on timeout."""
    end = None if timeout is None else time.monotonic() + timeout
    while True:
        if status()["mode"] in ("ended", "live"):
            return True
        if end is not None and time.monotonic() > end:
            return False
        time.sleep(poll)
