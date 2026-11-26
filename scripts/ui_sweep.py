# SPDX-License-Identifier: Apache-2.0
r"""UI smoke sweeps against a hidden YCGrid, the way a user would hit them (see skill/ycgrid-python/references/testing.md).

    <YCGrid>\runtime\python\python.exe -E -s scripts\ui_sweep.py [commands] [dialogs] [examples] [--exe path] [--out dir]

commands  run every UI command once on a sample OHLC sheet: the process must stay alive, the UI must answer, nothing
          unexpected may reach the Error Log (dialogs are closed with Escape afterwards)
dialogs   open every dialog, press Enter (the default button), reopen, press Escape: both must close it (or show its
          result page); a dialog that keeps the keyboard is a failure
examples  open every example's workbook (examples/<name>/<name>.ycg) in a fresh instance, let its Workbook_Open
          build the screen and run, screenshot it (look at the pictures)
Without arguments all three run. Exit code 1 when something failed. Screenshots go to --out (default: a temp folder).
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# commands that leave the session, print, or touch files / installs: not part of a sweep
SKIP = re.compile(r"(Quit|Exit|FileClose|CloseAll|FileOpen|SaveAs|^Save$|FileSave|FileNew|NewWindow|Print|Install|Uninstall|"
                  r"Register|Update|Shutdown|Restart|Reload|Delete.?Sheet|Registry|Kill|OperatorMode)", re.I)
# commands whose message on the sample sheet is the expected answer (as in Excel)
EXPECTED_MESSAGES = {"FlashFill", "EvaluateFormula", "DataTableDialog", "TextToColumns", "Spelling"}


def sample(excel):
    from yc import safety

    safety.set_operator_mode(True)  # a command may have switched to read-only mode
    ws = excel.Worksheets(1)  # or activated a chart sheet
    for f in (ws.Unprotect, excel.ActiveWorkbook.Unprotect):
        try:
            f()
        except Exception:  # noqa: BLE001 - not protected
            pass
    ws.Visible = True  # Hide Sheet may have hidden it, and a hidden sheet can't be activated (Excel)
    ws.Activate()
    # the shapes and charts commands insert pile up on the sheet and every frame draws them all: keep the newest few
    # (commands that format a shape still find one), or the sweep slows to seconds a command
    try:
        while ws.Shapes.Count > 8:
            ws.Shapes(1).Delete()
    except Exception:  # noqa: BLE001 - a shape that can't be deleted: leave the rest
        pass
    ws.Range("A1:E1").Value = [["Date", "Open", "High", "Low", "Close"]]
    ws.Range("A2:E41").Value = [[46000 + i, 100 + i, 103 + i, 98 + i, 101 + i] for i in range(40)]
    ws.Range("A2:A41").NumberFormat = "yyyy-mm-dd"


def launch(exe):
    import yc

    p = yc.launch(hidden=True, exe=exe, lang="en")
    return p, yc.connect(target=p.pid)


def sweep_commands(exe, failures):
    from yc import excel, ui

    p, conn = launch(exe)
    try:
        sample(excel)
        n = 0
        seen_asserts = 0
        shrunk = set()
        for c in ui.commands():
            cid = c["id"]
            if SKIP.search(cid):
                continue
            conn.request("ui.input", {"script": "g:B2:E41|w:2", "timeout_ms": 5000}, timeout=15)
            try:
                conn.request("ui.command", {"id": cid}, timeout=20)
                time.sleep(0.2)
                conn.request("ui.input", {"script": "w:12", "timeout_ms": 5000}, timeout=15)  # long enough to see a window shrink
                st = conn.request("ui.state", {}, timeout=10)
            except Exception as e:  # noqa: BLE001
                failures.append(f"command {cid}: {e}" + (" (YCGrid ended)" if p.poll() is not None else ""))
                if p.poll() is not None:
                    p, conn = launch(exe)
                    sample(excel)
                    seen_asserts = 0
                continue
            n += 1
            # a window that fits its contents and narrows by itself frame after frame (the Password dialog did)
            for name in st.get("shrinking_windows", []):
                if name not in shrunk:
                    shrunk.add(name)
                    failures.append(f"command {cid}: the window {name!r} shrinks by itself")
            # a dialog narrower than its contents: the right part is cut off (a fixed width too small)
            for name in st.get("clipped_windows", []):
                if name not in shrunk:
                    shrunk.add(name)
                    failures.append(f"command {cid}: the dialog {name!r} cuts off its contents on the right")
            # an ImGui rule broken while the command ran: a release build counts it, a debug build stops on it
            asserts = st.get("imgui_asserts", 0)
            if asserts > seen_asserts:
                failures.append(f"command {cid}: {asserts - seen_asserts} ImGui check(s) failed (a debug build stops here)")
                seen_asserts = asserts
            for e in ui.errors():
                if cid not in EXPECTED_MESSAGES and "answered automatically" not in e.get("text", "") and e.get("error"):
                    failures.append(f"command {cid}: {e.get('title')}: {e.get('text', '')[:160]}")
            ui.clear_errors()
            if st.get("dialog") or st["ribbon"].get("popup_open") or st["ribbon"].get("backstage"):
                conn.request("ui.input", {"script": "k:Escape|w:3|k:Escape|w:3", "timeout_ms": 5000}, timeout=15)
        print(f"commands: {n} run")
    finally:
        p.terminate()


def sweep_dialogs(exe, failures):
    from yc import excel, ui

    p, conn = launch(exe)
    try:
        sample(excel)
        seen = {}
        windows = {}  # dialogs that don't block the sheet (Symbol, Find and Replace): a window with Cancel or Close
        for c in ui.commands():  # find the commands that open a dialog
            cid = c["id"]
            if SKIP.search(cid):
                continue
            conn.request("ui.input", {"script": "g:B2:E41|w:2", "timeout_ms": 5000}, timeout=15)
            before = {x["name"] for x in ui.windows()}
            try:
                conn.request("ui.command", {"id": cid}, timeout=20)
            except Exception as e:  # noqa: BLE001 - e.g. refused on a protected sheet: no dialog to check
                print(f"  {cid}: {e}")
                sample(excel)
                continue
            time.sleep(0.15)
            d = conn.request("ui.state", {}, timeout=10).get("dialog")
            if d:
                seen.setdefault(d, cid)
            for x in ui.windows():
                if x["name"] not in before and x["name"] not in windows and not x.get("docked"):
                    labels = {i.get("label", "").split("##")[0] for i in ui.items(x["label"])}
                    if labels & {"Cancel", "Close"}:
                        windows[x["name"]] = (cid, x["label"])
            conn.request("ui.input", {"script": "k:Escape|w:3|k:Escape|w:3", "timeout_ms": 5000}, timeout=15)
            for x in ui.windows():  # what Escape left open: closed here so the next command starts clean
                if x["name"] in windows and x["name"] not in before:
                    conn.request("ui.input", {"script": "k:Escape|w:3", "timeout_ms": 5000}, timeout=15)
            sample(excel)
        for name, (cid, label) in sorted(windows.items()):  # Escape right after it opens closes it, as Excel's dialogs
            conn.request("ui.input", {"script": "g:B2:E41|w:2", "timeout_ms": 5000}, timeout=15)
            conn.request("ui.command", {"id": cid}, timeout=20)
            time.sleep(0.3)
            if name not in {x["name"] for x in ui.windows()}:
                continue
            conn.request("ui.input", {"script": "k:Escape|w:6", "timeout_ms": 5000}, timeout=15)
            time.sleep(0.3)
            if name in {x["name"] for x in ui.windows()}:
                failures.append(f"window {name} ({cid}): Escape doesn't close it")
                p_items = [i for i in ui.items(label) if i.get("label", "").split("##")[0] in ("Cancel", "Close")]
                if p_items:  # close it with its own button for the next checks
                    r = p_items[0]["rect"]
                    x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
                    conn.request("ui.input", {"script": f"m:{x},{y}|w:2|c:{x},{y}|w:4", "timeout_ms": 5000}, timeout=15)
            sample(excel)
        print(f"windows: {len(windows)} checked with Escape")
        seen_asserts = conn.request("ui.state", {}).get("imgui_asserts", 0)  # the first pass reports none: commands do
        for d, cid in sorted(seen.items()):
            for key in ("Escape", "Enter"):
                conn.request("ui.input", {"script": "g:B2:E41|w:2", "timeout_ms": 5000}, timeout=15)
                try:
                    conn.request("ui.command", {"id": cid}, timeout=20)
                except Exception:  # noqa: BLE001
                    break
                time.sleep(0.3)
                if conn.request("ui.state", {}).get("dialog") != d:
                    break
                conn.request("ui.input", {"script": f"k:{key}|w:6", "timeout_ms": 5000}, timeout=15)
                time.sleep(0.3)
                if key == "Escape" and conn.request("ui.state", {}).get("dialog") == d:
                    failures.append(f"dialog {d} ({cid}): Escape doesn't close it")
                conn.request("ui.input", {"script": "k:Escape|w:3|k:Escape|w:3", "timeout_ms": 5000}, timeout=15)
                ui.clear_errors()
                sample(excel)
                asserts = conn.request("ui.state", {}).get("imgui_asserts", 0)
                if asserts > seen_asserts:
                    failures.append(f"dialog {d} ({cid}): {asserts - seen_asserts} ImGui check(s) failed (a debug build stops here)")
                    seen_asserts = asserts
        print(f"dialogs: {len(seen)} checked with Enter and Escape")
    finally:
        p.terminate()


def sweep_examples(exe, out, failures):
    import example_build
    from yc import ui

    for name in example_build.names():
        p = None
        try:
            p, conn = example_build.open_example(exe, name)
            time.sleep(8)  # live screens: let them draw
            ui.screenshot(os.path.join(out, f"example_{name}.png"))
            errs = [e for e in ui.errors() if e.get("error")]
            built = example_build.built()
            if not built or errs or p.poll() is not None:
                failures.append(f"example {name}: {'not built' if not built else [e.get('text', '')[:160] for e in errs]}")
        except Exception as e:  # noqa: BLE001
            failures.append(f"example {name}: {e}")
        finally:
            if p is not None:
                p.terminate()
            time.sleep(0.5)
    print(f"examples: screenshots in {out}")


def main():
    ap = argparse.ArgumentParser()
    # no choices= here: argparse checks the empty default against them and refuses a run without arguments
    ap.add_argument("what", nargs="*", help="commands, dialogs, examples (default: all three)")
    ap.add_argument("--exe", default=os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    ap.add_argument("--out", default=tempfile.mkdtemp(prefix="yc_sweep_"))
    a = ap.parse_args()
    unknown = [w for w in a.what if w not in ("commands", "dialogs", "examples")]
    if unknown:
        ap.error(f"unknown part(s): {', '.join(unknown)} (choose from commands, dialogs, examples)")
    os.makedirs(a.out, exist_ok=True)  # the screenshots go here; --out may name a new folder
    sys.path.insert(0, os.path.join(os.path.dirname(a.exe), "python"))
    what = a.what or ["commands", "dialogs", "examples"]
    failures: list[str] = []
    if "commands" in what:
        sweep_commands(a.exe, failures)
    if "dialogs" in what:
        sweep_dialogs(a.exe, failures)
    if "examples" in what:
        sweep_examples(a.exe, a.out, failures)
    for f in failures:
        print("FAIL", f)
    print(f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
