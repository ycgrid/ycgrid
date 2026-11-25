# SPDX-License-Identifier: Apache-2.0
r"""Every basic feature, done the way a person does it, and its result checked.

    <YCGrid>\runtime\python\python.exe -E -s scripts\feature_check.py [--exe path] [--only section,...] [--keep]
        [--shard K/N --covered-out file]      every Nth section from K (parallel runs), and the commands it checked
        [--coverage-from file,file...]        only the coverage check, from the shards' files

ui_sweep only proves that a command doesn't crash or hang, and chaos only that random input doesn't; neither looks at
what the command did, so a Record button that wrote an empty file passed both. This script clicks each command where
the user finds it (its ribbon tab, the drop-down menu it is in, its gallery; a shortcut or context menu otherwise) and
then checks the result: the cells, formats, files, windows, dialogs. Every registered command must be checked here or
listed in NOT_CLICKABLE with the reason; a new command without a check fails the run.
Exit code 1 when something failed.
"""
from __future__ import annotations

import argparse
import faulthandler
import os
import shutil
import sys
import tempfile
import time
import traceback

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "python"))

import yc  # noqa: E402

RESULTS: list[tuple[str, str, bool, str]] = []  # (section, name, ok, detail)
COVERED: set[str] = set()
SECTION = [""]
OUT = [""]
# YCGRID_DIALOG_SHOTS=<folder>: a screenshot of every dialog the first time it shows (App.shot_new_dialogs)
DIALOG_SHOTS = os.environ.get("YCGRID_DIALOG_SHOTS", "")
_SHOT_DIALOGS: set[str] = set()


def check(name, ok, detail=""):
    RESULTS.append((SECTION[0], name, bool(ok), "" if ok else str(detail)[:300]))
    print(("OK   " if ok else "FAIL ") + f"[{SECTION[0]}] {name}" + ("" if ok else f"   <- {str(detail)[:300]}"), flush=True)
    return bool(ok)


def wait(cond, secs=5.0, step=0.1):
    end = time.time() + secs
    while True:
        try:
            v = cond()
            if v:
                return v
        except Exception:  # noqa: BLE001
            pass
        if time.time() > end:
            try:
                return cond()
            except Exception:  # noqa: BLE001
                return None
        time.sleep(step)


class App:
    """A hidden YCGrid, and how a person reaches each command in it."""

    def __init__(self, exe, size=(1600, 900), path=None):
        self.exe = exe
        self.p = yc.launch(path, hidden=True, exe=exe, lang="en", size=size) if path else yc.launch(hidden=True, exe=exe, lang="en", size=size)
        self.conn = yc.connect(target=self.p.pid)
        from yc import excel, ui

        self.excel, self.ui = excel, ui
        end = time.time() + 60
        while not self.conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
            time.sleep(0.2)
        self.inp("w:3")
        self.where: dict[str, tuple] = {}  # cmd -> (tab, parent menu cmd or None, where)
        self.err_mark = 0

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(10)
        except Exception:  # noqa: BLE001
            pass

    # ---- input ----
    def inp(self, script, wait_s=0.25):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 10000}, timeout=30)
        if wait_s:
            time.sleep(wait_s)
        if DIALOG_SHOTS:
            self.shot_new_dialogs()

    def shot_new_dialogs(self):
        """YCGRID_DIALOG_SHOTS=<folder>: a screenshot and the size of every dialog the first time it shows, to review
        that each opens at a fitting size (menus and drop-downs, named "##...", are left out)."""
        import json

        for w in self.conn.request("ui.state", {}, timeout=10).get("windows", []):
            title = w["name"].split("###")[0]
            if not w["popup"] or not title or title.startswith("##") or title in _SHOT_DIALOGS:
                continue
            _SHOT_DIALOGS.add(title)
            name = "".join(ch if ch.isalnum() else "_" for ch in title)[:40]
            path = os.path.join(DIALOG_SHOTS, f"{SECTION[0]}__{name}.png")
            self.ui.screenshot(path, max_width=1600)
            with open(os.path.join(DIALOG_SHOTS, "dialogs.jsonl"), "a", encoding="utf-8") as f:
                f.write(json.dumps({"section": SECTION[0], "title": title, "rect": w["rect"], "auto": w["auto_size"],
                                    "shot": path}) + "\n")

    def state(self):
        return self.ui.state()

    def escape_all(self):
        for _ in range(4):
            st = self.state()
            rb = st.get("ribbon") or {}
            if not (st.get("dialog") or rb.get("popup_open") or rb.get("backstage")):
                return
            self.inp("k:Escape|w:3")

    def items(self, where=None):
        return [i for i in self.ui.ribbon_items() if where is None or i["where"] == where]

    def click_rect(self, r, part="center"):
        x0, y0, x1, y1 = r
        if part == "arrow":  # the drop-down part of a split button: its right edge (small) or bottom (large)
            if (y1 - y0) > 50:
                x, y = (x0 + x1) / 2, y1 - 8
            else:
                x, y = x1 - 6, (y0 + y1) / 2
        else:
            x, y = (x0 + x1) / 2, (y0 + y1) / 2
            if (y1 - y0) > 50:  # a large split button: its icon half runs the command
                y = y0 + 20
        x, y = int(x), int(y)
        self.inp(f"m:{x},{y}|w:3|c:{x},{y}|w:4")

    def hover(self, r):
        x, y = int(r[0]) + 40, int((r[1] + r[3]) / 2)
        self.inp(f"m:{x},{y}|w:8")

    def tab(self, name):
        """Click a ribbon tab (waits between clicks on the same tab so they aren't a double-click)."""
        rb = self.state()["ribbon"]
        if name not in rb["tabs"]:
            return False
        names = list(rb["tabs"])
        if rb.get("tab", -1) == names.index(name):
            return True
        x, y = rb["tabs"][name]
        time.sleep(0.55)
        self.inp(f"c:{int(x)},{int(y)}|w:4")
        return True

    def discover(self, tabs=None):
        """Map every command reachable from the ribbon: tab, the menu holding it, buttons inside galleries."""
        rb = self.state()["ribbon"]
        for t in tabs or [t for t in rb["tabs"] if t != "File"]:
            if not self.tab(t):
                continue
            top = [i for i in self.items() if i["where"] in ("ribbon", "dropdown", "combo")]
            for it in top:
                self.where.setdefault(it["cmd"], (t, None, it["where"]))
            for it in [i for i in top if i["where"] == "dropdown"]:
                before = {i["cmd"] for i in self.items() if i["where"] in ("menu", "tile")}
                self.click_rect(it["rect"], "arrow")
                st = self.state()
                if st["ribbon"].get("popup_open"):
                    shown = self.items()
                    for sub in shown:
                        if sub["where"] in ("menu", "tile") and sub["cmd"] not in before:
                            self.where.setdefault(sub["cmd"], (t, it["cmd"], sub["where"]))
                    for head in [x for x in shown if x["where"] == "submenu"]:  # Conditional Formatting > Icon Sets
                        self.where.setdefault(head["cmd"], (t, it["cmd"], "submenu"))
                        self.hover(head["rect"])
                        for sub in self.items("menu"):
                            if sub["cmd"] not in self.where:
                                self.where[sub["cmd"]] = (t, it["cmd"], "menu", head["cmd"])
                self.escape_all()
        return self.where

    def run(self, cid, how="auto", arrow=False):
        """Run a command as a person does: click it on its ribbon tab (opening the menu it is in), else its id."""
        COVERED.add(cid)
        w = self.where.get(cid) if how in ("auto", "ribbon") else None
        if w:
            t, parent = w[0], w[1]
            self.escape_all()
            self.tab(t)
            if parent:
                r = next((i["rect"] for i in self.items() if i["cmd"] == parent), None)
                if r:
                    self.click_rect(r, "arrow")
                if len(w) > 3:  # in a submenu: hover its heading first
                    h = next((i["rect"] for i in self.items("submenu") if i["cmd"] == w[3]), None)
                    if h:
                        self.hover(h)
            r = next((i["rect"] for i in self.items() if i["cmd"] == cid), None)
            if r:
                self.click_rect(r, "arrow" if arrow else "center")
                # a drop-down whose face only opens its menu (View > Freeze Panes): the entry of the same name in it
                if not arrow and w[2] == "dropdown" and self.state()["ribbon"].get("popup_open"):
                    e = next((i["rect"] for i in self.items("menu") if i["cmd"] == cid), None)
                    if e:
                        self.click_rect(e)
                return "ribbon"
        self.inp(f"x:{cid}|w:4")
        return "id"

    # ---- checks ----
    def sheet(self, name=None):
        """A fresh sheet for a test (activated), so tests don't see each other's cells."""
        self.escape_all()
        ws = self.excel.Worksheets.Add()
        if name:
            ws.Name = name
        self.inp("w:3")
        return ws

    def new_errors(self):
        es = [e for e in self.ui.errors(errors_only=True) if e["id"] > self.err_mark]
        if es:
            self.err_mark = max(e["id"] for e in es)
        return es

    def dialog(self):
        return self.state().get("dialog", 0)

    def controls(self, window=""):
        return self.ui.items(window)

    def press(self, label, window="", nth=0, by="label"):
        """Click a control (button, check box, swatch...) by its text, as a person does; False when it isn't shown."""
        hits = [i for i in self.controls(window) if (i[by] == label if by != "contains" else label in i["id"])]
        hits.sort(key=lambda i: (i["rect"][1], i["rect"][0]))
        if len(hits) <= nth:
            return False
        self.click_rect(hits[nth]["rect"])
        return True


# ======================================================================================================================
# sections: each takes the App and checks a group of features
# ======================================================================================================================
SECTIONS = []


def section(fn):
    SECTIONS.append(fn)
    return fn


def run_sections(exe, only, shard=None):
    for i, fn in enumerate(SECTIONS):
        name = fn.__name__[2:] if fn.__name__.startswith("s_") else fn.__name__  # only the prefix: "s_chart_types_b" is "chart_types_b"
        if only and name not in only:
            continue
        if shard and i % shard[1] != shard[0]:
            continue
        SECTION[0] = name
        print(f"== {name}", flush=True)
        app = None
        try:
            app = App(exe, **getattr(fn, "app_args", {}))
            app.discover(getattr(fn, "tabs", None))
            fn(app)
            if app.p.poll() is None:  # a section may close its window on purpose (File > Close)
                errs = app.new_errors()
                check("no unexpected errors in the Error Log", not errs, [(e["title"], (e.get("text") or "")[:120]) for e in errs])
                # every dialog the section opened kept its size and showed all of its contents
                st = app.conn.request("ui.state", {}, timeout=10)
                check("no dialog shrank by itself or cut off its contents",
                      not st.get("shrinking_windows") and not st.get("clipped_windows"),
                      {"shrinking": st.get("shrinking_windows"), "clipped": st.get("clipped_windows")})
        except Exception as e:  # noqa: BLE001
            check("section ran to the end", False, f"{type(e).__name__}: {e}\n{traceback.format_exc()[-800:]}")
        finally:
            if app:
                app.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    ap.add_argument("--only", default="")
    ap.add_argument("--keep", action="store_true", help="keep the output folder")
    ap.add_argument("--shard", default="", help="K/N: run the sections whose index %% N == K (parallel runs)")
    ap.add_argument("--covered-out", default="", help="write the commands this run checked, one per line")
    ap.add_argument("--coverage-from", default="", help="comma-separated --covered-out files: only the coverage check")
    a = ap.parse_args()
    shard = tuple(int(x) for x in a.shard.split("/")) if a.shard else None
    faulthandler.dump_traceback_later(3600, exit=True)
    OUT[0] = tempfile.mkdtemp(prefix="yc_features_")
    only = {s for s in a.only.split(",") if s}
    import feature_sections  # noqa: F401 - registers the sections

    if a.coverage_from:
        for f in a.coverage_from.split(","):
            with open(f, encoding="utf-8") as fh:
                COVERED.update(line.strip() for line in fh if line.strip())
    else:
        run_sections(a.exe, only, shard)
    if a.covered_out:
        with open(a.covered_out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(sorted(COVERED)) + "\n")
    if not only and not shard:
        SECTION[0] = "coverage"
        app = App(a.exe)
        try:
            ids = [c["id"] for c in app.ui.commands()]
        finally:
            app.close()
        from feature_sections import NOT_CLICKABLE

        missing = [i for i in ids if i not in COVERED and i not in NOT_CLICKABLE]
        check(f"every command is checked ({len(ids)} commands)", not missing, f"{len(missing)} without a check: {' '.join(missing)}")
    bad = [r for r in RESULTS if not r[2]]
    print(f"\n{len(RESULTS) - len(bad)} passed, {len(bad)} failed")
    for s, n, _, d in bad:
        print(f"  FAIL [{s}] {n}: {d}")
    if not a.keep:
        shutil.rmtree(OUT[0], ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import feature_check  # the sections register with the module, not with __main__

    sys.exit(feature_check.main())
