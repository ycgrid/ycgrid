# SPDX-License-Identifier: Apache-2.0
"""The command coverage check (PLAN §13.5a): every UI command has a Python entry point.

    python scripts/check_commands.py           check; exit 1 on a gap
    python scripts/check_commands.py --write   also write skill/ycgrid-python/references/ui-coverage.md
    python scripts/check_commands.py --live    also start dist_release\\ycgrid.exe hidden and check the commands it has
                                               (the ones made in loops too)

Commands are read from the C++ sources (the CommandRegistry registrations), entries from schema/commands.yaml; each
entry is resolved against the yc package. Run it with the bundled Python (runtime\\python\\python.exe -E -s).
"""
from __future__ import annotations

import fnmatch
import glob
import importlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA = os.path.join(ROOT, "schema", "commands.yaml")
DOC = os.path.join(ROOT, "skill", "ycgrid-python", "references", "ui-coverage.md")

# the functions that register a command: Make("Bold", ...), make(...), add(...), the alignment helpers, c.id = "..."
_REG = re.compile(r'(?:\b(?:make|Make|add|sel|legend|axis|orient|valign|halign)\("([A-Z][A-Za-z0-9]+)"|\.id = "([A-Za-z0-9]+)")')
_PANE = re.compile(r'RegisterPane\("([a-z_]+)"')


def _read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def source_files():
    for d in ("apps/ycgrid", "src/ui"):
        yield from glob.glob(os.path.join(ROOT, d, "**", "*.cpp"), recursive=True)


def registered_commands() -> set:
    ids = set()
    for f in source_files():
        for m in _REG.finditer(_read(f)):
            ids.add(m.group(1) or m.group(2))
    return ids


def registered_panes() -> set:
    panes = set()
    for f in source_files():
        panes.update(_PANE.findall(_read(f)))
    return panes


def load_schema(path: str = SCHEMA) -> dict:
    """{id: [entries]} in file order, plus the section of each id (from the "# ---- Section ----" comments)."""
    out, sections, section = {}, {}, ""
    for n, line in enumerate(_read(path).splitlines(), 1):
        s = line.strip()
        if s.startswith("# ----"):
            section = s.strip("# -")
            continue
        if not s or s.startswith("#"):
            continue
        if ":" not in s:
            raise ValueError(f"{path}:{n}: expected 'Id: Entry, ...'")
        key, val = s.split(":", 1)
        key = key.strip()
        if key in out:
            raise ValueError(f"{path}:{n}: {key} is listed twice")
        out[key] = [e.strip() for e in re.split(r",\s*(?![^(]*\))", val) if e.strip()]
        sections[key] = section
    return out, sections


def _yc():
    sys.path.insert(0, os.path.join(ROOT, "python"))
    import yc  # noqa: F401
    from yc import excel, ui

    mods = [excel, ui]
    for name in ("rules", "data", "forms", "interact", "alerts", "safety", "replay", "analysis", "notebook"):
        try:
            mods.append(importlib.import_module("yc." + name))
        except Exception:  # noqa: BLE001 - an optional module that can't load here
            pass
    src = ""
    for f in glob.glob(os.path.join(ROOT, "python", "yc", "**", "*.py"), recursive=True):
        src += open(f, encoding="utf-8").read() + "\n"
    return mods, src


def _root_class(mods, name):
    for m in mods:
        for n in (name, "_" + name):
            obj = getattr(m, n, None)
            if obj is not None:
                return obj if isinstance(obj, type) else type(obj)
    return None


def check_entry(entry: str, mods, src: str, panes: set) -> str:
    """'' when the entry exists, else why not."""
    if entry == "ExecuteMso":
        return ""
    m = re.fullmatch(r'ui\.pane\("([a-z_]+)"\)', entry)
    if m:
        return "" if m.group(1) in panes else f"no pane {m.group(1)!r} (RegisterPane)"
    if entry.startswith("yc."):
        mod, _, attr = entry.rpartition(".")
        try:
            return "" if hasattr(importlib.import_module(mod), attr) else f"{mod} has no {attr}"
        except Exception as e:  # noqa: BLE001
            return f"{mod} doesn't import: {e}"
    parts = entry.split(".")
    if parts[0] == "ui":
        from yc import ui

        return "" if hasattr(ui, parts[1]) else f"yc.ui has no {parts[1]}"
    cls = _root_class(mods, parts[0])
    if cls is not None and len(parts) > 1 and not hasattr(cls, parts[1]):
        return f"{cls.__name__} has no {parts[1]}"
    # the deeper parts (and roots that are only reached through other objects): defined somewhere in yc
    for p in parts[1 if cls is not None else 0:]:
        if not re.search(rf"(def {p}\b|\b{p} = |class _?{p}\b)", src):
            return f"{p} isn't defined in yc"
    return ""


def uncovered(ids) -> list:
    """The command ids (e.g. from a running YCGrid: yc.ui.commands()) that schema/commands.yaml doesn't cover."""
    schema, _ = load_schema()
    patterns = [k for k in schema if k.endswith("*")]
    return [c for c in ids if c not in schema and not any(fnmatch.fnmatchcase(c, p) for p in patterns)]


def check_live(exe: str) -> list:
    """Start YCGrid hidden and check every command it has, including the ones made in loops (galleries, menus)."""
    sys.path.insert(0, os.path.join(ROOT, "python"))
    import yc
    from yc import ui

    proc = yc.launch(hidden=True, exe=exe, lang="en")
    try:
        yc.connect(target=proc.pid)
        return [f"{c}: a UI command (in the running app) without a Python entry point" for c in uncovered(x["id"] for x in ui.commands())]
    finally:
        proc.terminate()
        proc.wait(timeout=10)


def check(write: bool = False) -> list:
    """The problems (empty when every command is covered)."""
    schema, sections = load_schema()
    cmds = registered_commands()
    panes = registered_panes()
    mods, src = _yc()
    problems = []
    patterns = [k for k in schema if k.endswith("*")]
    for c in sorted(cmds):
        if c not in schema and not any(fnmatch.fnmatchcase(c, p) for p in patterns):
            problems.append(f"{c}: a UI command without a Python entry point in schema/commands.yaml")
    for c, entries in schema.items():
        if not c.endswith("*") and c not in cmds:
            problems.append(f"{c}: listed in schema/commands.yaml but not registered in the UI")
        if not entries:
            problems.append(f"{c}: no entry")
        for e in entries:
            why = check_entry(e, mods, src, panes)
            if why:
                problems.append(f"{c}: {e}: {why}")
    if write:
        write_doc(schema, sections)
    return problems


def write_doc(schema: dict, sections: dict):
    lines = ["# UI commands and their Python entry points", "",
             "Generated from `schema/commands.yaml` by `scripts/check_commands.py --write`; do not edit by hand.", "",
             "Every ribbon button, menu item and shortcut is a command. Each one is reachable from Python through the",
             "members listed here, and any of them can also be run by id: `yc.ui.command(\"Bold\")` (the same as",
             "`Application.CommandBars.ExecuteMso(\"Bold\")`); `yc.ui.commands()` lists them with their labels and state.",
             "Built-in panes: `yc.ui.pane(name, visible)` / `yc.ui.panes()`. `ExecuteMso` marks commands that only drive",
             "the UI itself (run them by id).", ""]
    current = None
    for c, entries in schema.items():
        if sections[c] != current:
            current = sections[c]
            lines += ["", f"## {current}", "", "| Command | Python |", "|---|---|"]
        lines.append(f"| `{c}` | " + ", ".join(f"`{e}`" for e in entries) + " |")
    text = "\n".join(lines).replace("\n\n\n", "\n\n") + "\n"
    with open(DOC, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    probs = check(write="--write" in sys.argv)
    if "--live" in sys.argv:  # --live [path\to\ycgrid.exe]: also the commands of the running app
        i = sys.argv.index("--live")
        exe = sys.argv[i + 1] if i + 1 < len(sys.argv) and sys.argv[i + 1].endswith(".exe") else os.path.join(ROOT, "dist_release", "ycgrid.exe")
        probs += check_live(exe)
    for p in probs:
        print(p)
    schema, _ = load_schema()
    print(f"{len(registered_commands())} commands, {len(schema)} entries, {len(probs)} problems")
    sys.exit(1 if probs else 0)
