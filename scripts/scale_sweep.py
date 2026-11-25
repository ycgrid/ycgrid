# SPDX-License-Identifier: Apache-2.0
r"""Every UI command over the selections that reach a sheet's limits, timed, against a hidden YCGrid.

    <YCGrid>\runtime\python\python.exe -E -s scripts\scale_sweep.py [--exe path] [--only REGEX] [--limit S] [--kinds K,K]
        [--skip N]

Each command runs once with each kind of selection: the whole sheet, whole columns (B:D), whole rows (3:5) and a range
from B2 to XFD1048576, on a sheet with a small table, formats and a value in its last cell. A dialog the command opens
is accepted with Enter (its defaults applied to that selection), then the step is undone with Ctrl+Z. The command, its
dialog and its Undo must each answer within --limit seconds (default 10, a release build).

The basic operations met these limits one by one in random rounds: borders, Merge & Center, Merge Across, Fill Right,
AutoSum, Format Cells in Page Layout view and a paste past the last column each looped over billions of cells (a
freeze) or went past the sheet's edge. This sweep tries every command so, instead of waiting for a random sequence to
reach one. Exit code 1 when a step was too slow, YCGrid stopped answering, or a step went wrong otherwise.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# commands that leave the session, print, write files, wait for a person outside YCGrid or change machine settings
SKIP = re.compile(r"(Quit|Exit|FileClose|CloseAll|FileOpen|SaveAs|^Save$|FileSave|FileNew|NewWindow|Print|Install|Uninstall|"
                  r"Register|Update|Shutdown|Restart|Reload|Delete.?Sheet|Registry|Kill|OperatorMode|Record|Replay|Screenshot|"
                  r"VsCode|Python(Run|Debug)|Macro|Share|Export|Publish|Mail)", re.I)
KINDS = {
    "sheet": "g:A1|w:2|k:c-A|k:c-A|w:2",
    "cols": "g:B:D|w:2",
    "rows": "g:3:5|w:2",
    "far": "g:B2:XFD1048576|w:2",
}


def prepare(excel):
    from yc import safety

    safety.set_operator_mode(True)
    for f in (excel.ActiveWorkbook.Unprotect,):
        try:
            f()
        except Exception:  # noqa: BLE001 - not protected
            pass
    ws = excel.Worksheets(1)
    try:
        ws.Unprotect()
    except Exception:  # noqa: BLE001
        pass
    ws.Visible = True
    ws.Activate()
    ws.Range("A1:E1").Value = [["Name", "Qty", "Price", "Total", "When"]]
    ws.Range("A2:E6").Value = [["AAPL", 10, 1.5, None, 45000], ["MSFT", 20, 2.25, None, 45001], ["TSMC", 30, 3, None, 45002],
                               ["NVDA", 40, 4.75, None, 45003], ["AMZN", 50, 5, None, 45004]]
    ws.Range("D2:D6").Formula = "=B2*C2"
    ws.Range("C2:C6").NumberFormat = "0.00"
    ws.Range("A1:E1").Font.Bold = True
    ws.Range("XFD1048576").Value = "far"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    ap.add_argument("--only", default="", help="a regular expression of the command ids to try")
    ap.add_argument("--limit", type=float, default=10.0, help="seconds a step may take")
    ap.add_argument("--kinds", default=",".join(KINDS), help="the selections to try (" + ", ".join(KINDS) + ")")
    ap.add_argument("--skip", type=int, default=0, help="start after this many commands (to go on with a sweep cut short)")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(a.exe)), "python"))
    import yc
    from yc import excel, ui

    only = re.compile(a.only) if a.only else None
    kinds = [k for k in a.kinds.split(",") if k in KINDS]
    problems: list[str] = []

    class _Problems(list):
        def append(self, line):  # each problem shown at once: the sweep runs for hours
            print("PROBLEM", line, flush=True)
            super().append(line)

    problems = _Problems()
    state = {"p": None, "conn": None}

    def start():
        if state["p"] is not None and state["p"].poll() is None:
            state["p"].kill()
        state["p"] = yc.launch(hidden=True, exe=a.exe, lang="en")
        state["conn"] = yc.connect(target=state["p"].pid)
        prepare(excel)

    def step(script, timeout):
        """Runs keys and waits for YCGrid to answer; the seconds it took, or None when it stopped answering."""
        t0 = time.time()
        try:
            state["conn"].request("ui.input", {"script": script, "timeout_ms": int(timeout * 1000)}, timeout=timeout + 15)
            state["conn"].request("ui.state", {}, timeout=timeout + 15)
        except Exception:  # noqa: BLE001 - no answer in time, or YCGrid ended
            return None
        return time.time() - t0

    start()
    ids = [c["id"] for c in ui.commands() if not SKIP.search(c["id"]) and (not only or only.search(c["id"]))][a.skip:]
    print(f"scale_sweep: {len(ids)} commands x {len(kinds)} selections, limit {a.limit:.0f} s", flush=True)
    t_all = time.time()
    for i, cid in enumerate(ids):
        for kind in kinds:
            what = f"{cid} over {kind}"
            if step(KINDS[kind] + "|k:Escape|w:2", 60) is None:
                problems.append(f"{what}: selecting stopped answering")
                start()
                continue
            t0 = time.time()
            try:
                state["conn"].request("ui.command", {"id": cid}, timeout=a.limit * 6 + 30)
            except Exception as e:  # noqa: BLE001
                if state["p"].poll() is not None or "E_TIMEOUT" in str(e):
                    problems.append(f"{what}: the command {'ended YCGrid' if state['p'].poll() is not None else 'stopped answering'} ({e})")
                    start()
                continue
            took = step("w:3", a.limit * 6)
            if took is None:
                problems.append(f"{what}: YCGrid stopped answering after the command ({time.time() - t0:.0f} s)")
                start()
                continue
            took = time.time() - t0
            if took > a.limit:
                problems.append(f"{what}: the command took {took:.1f} s")
            st = state["conn"].request("ui.state", {}, timeout=30)
            if st.get("dialog"):  # its defaults, applied to the selection
                took = step("k:Enter|w:4", a.limit * 6)
                if took is None:
                    problems.append(f"{what}: its dialog's OK stopped answering")
                    start()
                    continue
                if took > a.limit:
                    problems.append(f"{what}: its dialog's OK took {took:.1f} s")
            took = step("k:Escape|w:2|k:Escape|w:2|k:c-Z|w:4", a.limit * 6)
            if took is None:
                problems.append(f"{what}: Undo after it stopped answering")
                start()
                continue
            if took > a.limit:
                problems.append(f"{what}: Undo after it took {took:.1f} s")
            for e in ui.errors():
                if e.get("crash") or "assert" in str(e.get("text", "")).lower():
                    problems.append(f"{what}: {e.get('text', '')[:160]}")
            ui.clear_errors()
        if (i + 1) % 40 == 0:
            print(f"  {i + 1}/{len(ids)} commands, {len(problems)} problems, {time.time() - t_all:.0f} s", flush=True)
            start()  # a fresh sheet: what earlier commands left (formats on a million cells) mustn't slow the rest
    if state["p"] is not None:
        state["p"].kill()
    print(f"scale_sweep: {len(ids)} commands in {time.time() - t_all:.0f} s: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
