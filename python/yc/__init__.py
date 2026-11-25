# SPDX-License-Identifier: Apache-2.0
"""Python interface of YCGrid.

In code loaded by YCGrid (main.py, sheets\\, modules\\) use it directly:

    from yc.excel import *

Standalone scripts (data feeds, Jupyter, tests) connect first:

    import yc
    yc.connect()
"""
from __future__ import annotations

import os
import subprocess as _subprocess
import sys

from ._conn import Connection, YcConnectionError, YcError, find_instances, set_current
from .types import XlError

__version__ = "0.10.3"
__all__ = ["connect", "disconnect", "launch", "find_exe", "XlError", "YcError", "YcConnectionError", "__version__"]


def _pid_alive(pid: int) -> bool:
    try:
        import ctypes

        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
        k32.CloseHandle(h)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    except Exception:  # noqa: BLE001
        return True


def connect(target=None, timeout: float = 5.0):
    """Connect to a running YCGrid. target: None (the instance of this folder or the only instance), a pid (int) or a program folder path."""
    import time

    if target is None and os.environ.get("YCGRID_PID", "").isdigit():
        target = int(os.environ["YCGRID_PID"])  # a data feed started by YCGrid connects to that YCGrid
    deadline = time.monotonic() + timeout

    def candidates():
        alive = [i for i in find_instances() if i.get("pipe") and _pid_alive(i.get("pid", 0))]
        if not alive:
            raise YcConnectionError("No running YCGrid was found")
        if isinstance(target, int):
            cands = [i for i in alive if i.get("pid") == target]
        elif isinstance(target, str):
            t = os.path.normcase(os.path.abspath(target))
            cands = [i for i in alive if os.path.normcase(os.path.abspath(i.get("app_dir") or "")) == t]
        else:
            here = os.path.normcase(os.path.abspath(os.getcwd()))
            cands = [i for i in alive if os.path.normcase(os.path.abspath(i.get("app_dir") or "")) == here]
            if not cands:
                if len(alive) > 1:
                    raise YcConnectionError("Several YCGrid instances are running; choose one by pid or program folder: yc.connect(target=...)")
                cands = alive
        if not cands:
            raise YcConnectionError("No matching YCGrid instance")
        return cands

    # an instance rewrites its file when it opens a workbook: a listing that misses it is tried again until timeout
    while True:
        try:
            cands = candidates()
            break
        except YcConnectionError:
            if not isinstance(target, int) or time.monotonic() > deadline:
                raise
            time.sleep(0.05)
    last = None
    while True:
        for inst in cands:
            try:
                conn = Connection.open_controller(inst["pipe"])
            except OSError as e:  # pipe busy (another script is connecting): retry shortly
                last = e
                continue
            hello = conn.request("hello", {"role": "controller", "pid": os.getpid()})
            from . import _conn

            if isinstance(hello, dict) and hello.get("hidden"):  # nobody can answer a dialog there: a lost reply fails fast
                conn.default_timeout = _conn.HIDDEN_REQUEST_TIMEOUT
                conn.hidden = True
            old = _conn._current
            set_current(conn)
            # the previous current connection, when nothing else holds it any more (yc.launch's own, a reconnect),
            # is closed: an open pipe nobody uses keeps a server thread of that YCGrid waiting. One the script still
            # holds (a second connection to watch the screen, one per instance) stays open; the host's never closes.
            if (old is not None and old is not conn and getattr(old, "_duplex", False) and not old.closed
                    and sys.getrefcount(old) <= 2):  # only `old` here and getrefcount's own argument
                old.close()
            from . import excel  # noqa: F401 (registers the extension types)

            return conn
        if time.monotonic() > deadline:
            raise YcConnectionError(f"Couldn't connect to YCGrid: {last}")
        time.sleep(0.05)


def disconnect():
    from . import _conn

    if _conn._current is not None:
        _conn._current.close()
    set_current(None)


def find_exe() -> str:
    """ycgrid.exe: $YCGRID_EXE, the wheel's copy (yc/_bin), or the YCGrid folder this package was installed with."""
    here = os.path.dirname(os.path.abspath(__file__))
    for p in (os.environ.get("YCGRID_EXE", ""), os.path.join(here, "_bin", "ycgrid.exe"), os.path.join(here, "..", "..", "ycgrid.exe")):
        if p and os.path.isfile(p):
            return os.path.abspath(p)
    raise YcConnectionError("ycgrid.exe wasn't found: set YCGRID_EXE to its path")


class _Process(_subprocess.Popen):
    """The YCGrid a launch started: terminate() ends it and waits until it has (up to a minute), so a caller's
    wait(10) after it doesn't time out on a machine busy with other work (TerminateProcess returns at once, and a
    process with a large workbook took over 10 s to be gone beside four random rounds)."""

    def terminate(self):
        super().terminate()
        try:
            super().wait(60)
        except Exception:  # noqa: BLE001
            pass


def launch(file: str = None, hidden: bool = False, exe: str = None, lang: str = None, timeout: float = 30.0, size=None,
           automation_security: int = 1):
    """Start a YCGrid and connect to it; returns its subprocess.Popen (terminate() closes it).

    hidden=True runs it without a window (CI, agents): drive it over YCP, check it with yc.ui.state() and
    yc.ui.screenshot(); it keeps no AutoRecover copies, and it ends when the process that launched it ends (nobody could
    see or close it otherwise). size=(width, height) in pixels.

    automation_security is its Application.AutomationSecurity, which the workbook given here (and those its scripts
    open) follows, as in Excel: msoAutomationSecurityLow (1, the default: the workbook's Python code runs),
    msoAutomationSecurityByUI (2: the Trust Center decides, as when a person opens it: untrusted code stays disabled
    and the Message Bar shows, also in a hidden YCGrid) or msoAutomationSecurityForceDisable (3)."""
    import subprocess
    import time

    from . import _conn

    args = [exe or find_exe(), "--new-window"]  # a process of its own, even when another YCGrid is running
    if file:
        args.append(os.path.abspath(file))
    if hidden:
        args += ["--hidden", "--parent-pid", str(os.getpid())]
    if lang:
        args += ["--lang", lang]
    args += ["--automation-security", str(int(automation_security))]
    if size:
        args += ["--width", str(int(size[0])), "--height", str(int(size[1]))]
    env = dict(os.environ)
    if os.path.normcase(os.path.dirname(os.path.abspath(args[0]))) == os.path.normcase(os.path.join(os.path.dirname(os.path.abspath(__file__)), "_bin")):
        env.setdefault("YCGRID_PYTHON", sys.executable)  # installed with pip: the host runs on this Python
    proc = _Process(args, env=env)
    deadline = time.monotonic() + timeout
    while True:
        # started for a file: ready once the file is read (or failed to open), so the caller finds the workbook
        # (another YCGrid's CompareSideBySideWith / Workbooks(name) look for it by its file name)
        if any(i.get("pid") == proc.pid and i.get("pipe") and not (file and i.get("opening")) for i in find_instances()):
            try:
                conn = connect(proc.pid)
            except BaseException:
                proc.terminate()  # never leave a YCGrid nobody holds (a hidden one would run until logoff)
                raise
            if hidden and conn is not None:  # (connect sets it too, from the instance's own answer)
                conn.default_timeout = _conn.HIDDEN_REQUEST_TIMEOUT
            return proc
        if proc.poll() is not None:
            raise YcConnectionError(f"YCGrid exited with code {proc.returncode}")
        if time.monotonic() > deadline:
            proc.terminate()
            raise YcConnectionError("YCGrid didn't start in time")
        time.sleep(0.1)
