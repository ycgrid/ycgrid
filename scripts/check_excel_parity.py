# SPDX-License-Identifier: Apache-2.0
"""Excel's object model against YCGrid's: every member Excel lists is YCGrid's, not planned, or an open gap.

    python scripts/check_excel_parity.py           check; exit 1 on a problem
    python scripts/check_excel_parity.py --write   also write docs/excel-object-model.md (the gap list)

Excel's members come from tests/data/excel_object_model.json (scripts/measure_excel_object_model.py reads them from
EXCEL.EXE's type library); the objects, the YCGrid classes that stand for them and the members classified as not
planned or as gaps from schema/excel_parity.toml. YCGrid's members are what the classes define: their properties and
methods, names their `__getattr__` answers from a table (`_plain`, `_PLAIN`, `_keys`), and the attributes `__init__`
sets. A problem is a member Excel has that is in none of the three, one listed that YCGrid now has (take it off the
list: the gaps only shrink), one listed that Excel doesn't have, or one listed twice. A method YCGrid has takes
Excel's arguments in Excel's order (tests/data/excel_method_params.json, from scripts/measure_excel_params.py): an
argument of another name at a place, or fewer places than Excel's without *args, is a problem too, since a call
written for Excel passes them by position (Range.Sort's tenth and eleventh, Workbook.Close's Filename). Run it with
the bundled Python (runtime\\python\\python.exe -E -s).
"""
from __future__ import annotations

import ast
import importlib
import inspect
import json
import os
import sys
import textwrap
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA = os.path.join(ROOT, "schema", "excel_parity.toml")
DATA = os.path.join(ROOT, "tests", "data", "excel_object_model.json")
PARAMS = os.path.join(ROOT, "tests", "data", "excel_method_params.json")
DOC = os.path.join(ROOT, "docs", "excel-object-model.md")
PRIORITIES = ("high", "medium", "low")


def _load():
    with open(SCHEMA, "rb") as f:
        schema = tomllib.load(f)
    with open(DATA, encoding="utf-8") as f:
        excel = json.load(f)["objects"]
    return schema, excel


def _init_attrs(cls) -> set:
    """The capitalized attributes `__init__` sets: self.SortFields = ..., object.__setattr__(self, "Name", ...)."""
    out = set()
    for klass in cls.__mro__:
        init = klass.__dict__.get("__init__")
        if init is None:
            continue
        try:
            tree = ast.parse(textwrap.dedent(inspect.getsource(init)))
        except (OSError, TypeError, SyntaxError):
            continue
        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and isinstance(node.ctx, ast.Store) and isinstance(node.value, ast.Name) \
                    and node.value.id == "self":
                out.add(node.attr)
            elif isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "__setattr__" \
                    and len(node.args) >= 2 and isinstance(node.args[1], ast.Constant) and isinstance(node.args[1].value, str):
                out.add(node.args[1].value)
    return {n for n in out if n[:1].isupper()}


def yc_members(names) -> set:
    sys.path.insert(0, os.path.join(ROOT, "python"))
    have = set()
    for qual in names:
        mod, cname = qual.split(".")
        cls = getattr(importlib.import_module("yc." + mod), cname, None)
        if cls is None:
            raise SystemExit(f"schema/excel_parity.toml names yc.{qual}, which doesn't exist")
        have |= {n for n in dir(cls) if n[:1].isupper()}
        for table in ("_plain", "_PLAIN", "_keys"):
            have |= {n for n in getattr(cls, table, ()) if isinstance(n, str)}
        have |= _init_attrs(cls)
    return have


def signature_problems(schema) -> list:
    """YCGrid's methods against Excel's arguments: the same names in the same order, as far as YCGrid's go, and as
    many places as Excel's (or *args)."""
    if not os.path.exists(PARAMS):
        return [f"{os.path.relpath(PARAMS, ROOT)} is missing (run scripts/measure_excel_params.py)"]
    with open(PARAMS, encoding="utf-8") as f:
        params = json.load(f)["objects"]
    sys.path.insert(0, os.path.join(ROOT, "python"))
    out = []
    for obj, spec in schema["objects"].items():
        xl = params.get(spec["tlb"], {})
        for qual in spec["yc"]:
            mod, cname = qual.split(".")
            cls = getattr(importlib.import_module("yc." + mod), cname, None)
            for name, want in xl.items():
                if name.startswith("_"):
                    continue
                f = inspect.getattr_static(cls, name, None)
                if isinstance(f, (staticmethod, classmethod)):
                    f = f.__func__
                if f is None or isinstance(f, property) or not inspect.isfunction(f):
                    continue
                sig = inspect.signature(f)
                pos = [q for q in sig.parameters.values() if q.kind in (q.POSITIONAL_ONLY, q.POSITIONAL_OR_KEYWORD)]
                if pos and pos[0].name == "self":
                    pos = pos[1:]
                for i, q in enumerate(pos[:len(want)]):
                    if q.name.lower() != want[i].lower():
                        out.append(f"{obj}.{name}: its argument {i + 1} is {q.name}, Excel's is {want[i]} "
                                   f"(Excel: {', '.join(want)})")
                        break
                else:
                    if len(pos) < len(want) and not any(q.kind == q.VAR_POSITIONAL for q in sig.parameters.values()):
                        out.append(f"{obj}.{name}: takes {len(pos)} of Excel's {len(want)} arguments by position "
                                   f"(add {', '.join(want[len(pos):])}, taken and ignored when YCGrid has no use for them)")
    return out


def classify():
    """{object: {member: ("yc" | "not_planned" | "high" | "medium" | "low" | None, group)}}, problems."""
    schema, excel = _load()
    problems = []
    listed = {}  # "Object.Member" or "*.Member" -> (kind, group text)
    for kind, groups in (("not_planned", schema.get("not_planned", [])), ("gap", schema.get("gap", []))):
        for g in groups:
            k = kind if kind == "not_planned" else g["priority"]
            if k not in ("not_planned",) + PRIORITIES:
                problems.append(f"a gap group with the priority {k!r} (high, medium or low)")
            text = g.get("reason") or g.get("what") or ""
            for m in g["members"]:
                if m in listed:
                    problems.append(f"{m}: listed twice")
                listed[m] = (k, text)
    result = {}
    used = set()
    for obj, spec in schema["objects"].items():
        if obj not in excel:
            problems.append(f"{obj}: not in {os.path.relpath(DATA, ROOT)} (run scripts/measure_excel_object_model.py)")
            continue
        have = yc_members(spec["yc"])
        result[obj] = {}
        for m in excel[obj]:
            key, star = f"{obj}.{m}", f"*.{m}"
            entry = listed.get(key) or listed.get(star)
            if m in have:
                if key in listed:
                    problems.append(f"{key}: YCGrid has it now; take it off schema/excel_parity.toml")
                    used.add(key)
                result[obj][m] = ("yc", "")
            elif entry:
                used.add(key if key in listed else star)
                result[obj][m] = entry
            else:
                problems.append(f"{key}: Excel has it and YCGrid doesn't; add it to YCGrid, or to [[not_planned]] "
                                f"with the reason, or to [[gap]] in schema/excel_parity.toml")
                result[obj][m] = (None, "")
    for m in listed:
        obj = m.split(".")[0]
        if m not in used and obj != "*":
            problems.append(f"{m}: listed in schema/excel_parity.toml, but Excel's {obj} has no such member")
    problems += signature_problems(schema)
    return result, problems, schema


def check() -> list:
    return classify()[1]


def write_doc(result, schema) -> str:
    lines = [
        "# Excel's object model in YCGrid",
        "",
        "Generated by `scripts/check_excel_parity.py --write` from `schema/excel_parity.toml` and",
        "`tests/data/excel_object_model.json` (Excel 16's members, read from its type library). Edit the schema, not",
        "this file.",
        "",
        "Every member of the objects below is YCGrid's, not planned (with the reason), or an open gap (with its",
        "priority). File > Options settings are members of Application, Workbook, Worksheet and Window, so they are",
        "counted here too. Worksheet functions are covered by `tests/unit/excel_measured_formulas.inc`, the UI commands",
        "by `skill/ycgrid-python/references/ui-coverage.md`.",
        "",
        "Priorities: **high**: Python lacks what YCGrid's own UI does, or a ported macro sets it in its first lines and",
        "stops there (Application raises on a member it doesn't know); **medium**: a File > Options item, or a member",
        "macros commonly read or set; **low**: the rest.",
        "",
    ]
    total = {k: 0 for k in ("all", "yc", "not_planned") + PRIORITIES}
    rows = []
    for obj, members in result.items():
        c = {k: 0 for k in total}
        for kind, _ in members.values():
            c["all"] += 1
            if kind in c:
                c[kind] += 1
        for k in total:
            total[k] += c[k]
        rows.append(f"| {obj} | {c['all']} | {c['yc']} | {c['not_planned']} | {c['high']} | {c['medium']} | {c['low']} |")
    lines += ["## Summary", "",
              f"{total['yc']} of Excel's {total['all']} members are YCGrid's, {total['not_planned']} are not planned and "
              f"{total['high'] + total['medium'] + total['low']} are open gaps ({total['high']} high, {total['medium']} "
              f"medium, {total['low']} low).", "",
              "| Object | Excel's members | YCGrid has | Not planned | Gaps: high | medium | low |",
              "|---|---|---|---|---|---|---|", *rows,
              f"| **All** | {total['all']} | {total['yc']} | {total['not_planned']} | {total['high']} | {total['medium']} | {total['low']} |",
              ""]

    def members_of(kind, text):
        by_obj = {}
        for obj, members in result.items():
            for m, (k, t) in members.items():
                if k == kind and t == text:
                    by_obj.setdefault(obj, []).append(m)
        return by_obj

    lines += ["## Open gaps", ""]
    for p in PRIORITIES:
        for g in (g for g in schema.get("gap", []) if g["priority"] == p):
            by_obj = members_of(p, g["what"])
            n = sum(len(v) for v in by_obj.values())
            if not n:
                continue
            lines += [f"### {p.capitalize()}: {g['what']} ({n})", ""]
            for obj, ms in by_obj.items():
                lines.append(f"- **{obj}**: " + ", ".join(f"`{m}`" for m in sorted(ms)))
            lines.append("")
    lines += ["## Not planned", ""]
    for g in schema.get("not_planned", []):
        by_obj = members_of("not_planned", g["reason"])
        n = sum(len(v) for v in by_obj.values())
        lines += [f"### {g['reason']} ({n})", ""]
        for obj, ms in by_obj.items():
            lines.append(f"- **{obj}**: " + ", ".join(f"`{m}`" for m in sorted(ms)))
        lines.append("")
    return "\n".join(lines)


def main():
    result, problems, schema = classify()
    if "--write" in sys.argv:
        with open(DOC, "w", encoding="utf-8", newline="\n") as f:
            f.write(write_doc(result, schema))
        print(f"wrote {os.path.relpath(DOC, ROOT)}")
    counts = {}
    for members in result.values():
        for kind, _ in members.values():
            counts[kind] = counts.get(kind, 0) + 1
    for p in problems:
        print(p)
    print(f"check_excel_parity: {sum(counts.values())} Excel members: {counts.get('yc', 0)} YCGrid's, "
          f"{counts.get('not_planned', 0)} not planned, gaps {counts.get('high', 0)} high / {counts.get('medium', 0)} "
          f"medium / {counts.get('low', 0)} low; {len(problems)} problem(s)")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
