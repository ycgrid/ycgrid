# SPDX-License-Identifier: Apache-2.0
"""The example projects (examples/<name>) for the check scripts: their names, and three ways to build one.

    open_example(exe, name)        a hidden YCGrid that opens examples/<name>/<name>.ycg, as a person does: its
                                   Workbook_Open builds the screen (ui_sweep)
    build_in_host(name)            the example's run() in the connected YCGrid's Python host, in the open workbook
                                   (trader_session: one session builds several screens, click actions work)
    run_here(name)                 the example's run() in this process, connected as a script (chaos)

The examples' shared code is examples/lib (each example's ycgrid.toml adds it with [python] paths).
"""
from __future__ import annotations

import importlib.util
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXAMPLES = os.path.join(ROOT, "examples")
LIB = os.path.join(EXAMPLES, "lib")
PROJECTS = ("demo", "txo_market_maker")  # projects of their own, not research or trading screens with run()


def names() -> list[str]:
    """The research and trading screens, in folder order."""
    return sorted(d for d in os.listdir(EXAMPLES) if d not in PROJECTS and d != "lib"
                  and os.path.isfile(os.path.join(EXAMPLES, d, "main.py")))


def workbook(name: str) -> str:
    d = os.path.join(EXAMPLES, name)
    return os.path.join(d, next(f for f in os.listdir(d) if f.endswith(".ycg")))


def _load(name: str):
    if LIB not in sys.path:
        sys.path.insert(0, LIB)
    import ex_util

    return ex_util.load_example(name)


def run_here(name: str, **options):
    return _load(name).run(**options)


def build_in_host(name: str, timeout_ms: int = 120000) -> dict:
    """{"ok", "error", "traceback", "console", "sheets", "figures"} after the example's run() in the host."""
    from yc.mcp import tools

    tools._host_ready()
    code = (f"import sys\nif {LIB!r} not in sys.path:\n    sys.path.insert(0, {LIB!r})\n"
            f"import ex_util\nex_util.load_example({name!r}).run()\n")
    r = tools._job(timeout_ms, kind="exec", code=code, dialogs="auto")
    out = {k: r.get(k) for k in ("ok", "error", "traceback", "console") if r.get(k) not in (None, "")}
    st = tools.rq("ui.state")
    out["sheets"] = [x["name"] for x in st.get("sheets", [])]
    out["figures"] = [f["title"] for f in st.get("figures", [])]
    return out


def open_example(exe: str, name: str, timeout: float = 120.0):
    """(process, connection) of a hidden YCGrid that opened the example's workbook, once its Workbook_Open has built
    the screen (built()) or `timeout` passed."""
    import yc

    p = yc.launch(workbook(name), hidden=True, exe=os.path.abspath(exe), lang="en")  # started in the workbook's folder
    conn = yc.connect(target=p.pid)
    deadline = time.time() + timeout
    while time.time() < deadline and p.poll() is None:
        try:
            if built():
                break
        except Exception:  # noqa: BLE001 - busy building
            pass
        time.sleep(0.5)
    return p, conn


def built() -> bool:
    """The connected YCGrid shows an example's screen: its sheets (the blank Sheet1 it ships with renamed) or its chart
    windows (quick_plot draws only charts)."""
    from yc import excel, ui

    return [s.Name for s in excel.Worksheets] != ["Sheet1"] or bool(ui.state().get("figures"))
