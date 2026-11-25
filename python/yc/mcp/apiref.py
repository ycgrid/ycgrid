# SPDX-License-Identifier: Apache-2.0
"""yc_api_lookup: look up the YCGrid Python object model (YCGrid doesn't need to be running).

Two sources merged:
1. reference docs skill/ycgrid-python/references/*.md (table rows and paragraphs, with section titles)
2. the actual yc.excel / yc.ui objects: whether members exist (implementation status), signatures, docstrings
"""
from __future__ import annotations

import inspect
import re
from functools import lru_cache
from pathlib import Path


def _ref_dirs():
    here = Path(__file__).resolve()
    root = here.parents[3]
    return [root / "skill" / "ycgrid-python" / "references", here.parent / "references"]


@lru_cache(maxsize=1)
def _entries() -> list:
    out = []
    for d in _ref_dirs():
        if not d.is_dir():
            continue
        for f in sorted(d.glob("*.md")):
            if f.name == "mcp-tools.md":
                continue
            section = ""
            para: list = []
            for line in f.read_text(encoding="utf-8").splitlines():
                if line.startswith("#"):
                    section = line.lstrip("#").strip()
                    para = []
                    continue
                s = line.strip()
                if not s or s.startswith("|---") or s == "---":
                    if para:
                        out.append({"file": f.name, "section": section, "text": " ".join(para)})
                        para = []
                    continue
                if s.startswith("|"):
                    out.append({"file": f.name, "section": section, "text": s})
                else:
                    para.append(s)
            if para:
                out.append({"file": f.name, "section": section, "text": " ".join(para)})
        break
    return out


def _tokens(q: str) -> list:
    return [t for t in re.split(r"[\s.()\[\],]+", q.lower()) if t]


def _introspect(query: str):
    from .. import constants, ui, excel

    parts = [p for p in re.split(r"[.\s]+", query.strip()) if p]
    if not parts:
        return None
    roots = {"Range": excel.Range, "Worksheet": excel.Worksheet, "Workbook": excel.Workbook, "Application": type(excel.Application),
             "Font": excel.Font, "Interior": excel.Interior, "Borders": excel.Borders, "Border": excel.Border, "Window": excel.Window,
             "OLEObject": ui.OLEObject, "CommandButton": ui.OLEObject, "ComboBox": ui.OLEObject, "OLEObjects": ui.OLEObjects,
             "ChartObjects": ui.ChartObjects, "ChartObject": ui.ChartObject, "Chart": ui.Chart, "Series": ui.Series,
             "SeriesCollection": ui.SeriesCollection, "Panel": ui.Panel, "Panels": type(ui.Panels), "PanelControl": ui.PanelControl,
             "Containers": type(ui.Containers), "Container": ui.Container, "Layout": type(ui.Layout), "Shapes": ui.Shapes,
             "Store": excel.Store}
    head = parts[0]
    obj = None
    kind = None
    if head in roots:
        obj, kind = roots[head], "class"
    elif hasattr(constants, head):
        return {"name": head, "kind": "constant", "value": getattr(constants, head), "implemented": True}
    elif hasattr(excel, head):
        obj, kind = getattr(excel, head), "function"
    if obj is None:
        return None
    name = head
    for i, p in enumerate(parts[1:]):
        if isinstance(obj, property):  # a property: go on with the class of what it returns
            owner, prev = cls, parts[i]
            obj = _property_class(owner, prev, obj)
            if obj is None:
                return {"name": name + "." + ".".join(parts[i + 1:]), "kind": "member", "implemented": None,
                        "note": f"{name} is a property whose value's class can't be found statically; check it at run time (dir())"}
        cls = obj if inspect.isclass(obj) else type(obj)
        attr = inspect.getattr_static(cls, p, None)
        name += "." + p
        if attr is None:
            return {"name": name, "kind": "member", "implemented": False,
                    "note": f"{head} has no member {p} (it may not be implemented yet, or have another name; see the documentation results)"}
        obj, kind = attr, "member"
    out = {"name": name, "kind": kind, "implemented": True}
    target = obj.fget if isinstance(obj, property) else obj
    if isinstance(obj, property):
        out["member_type"] = "property" + (" (writable)" if obj.fset else " (read-only)")
    try:
        if callable(target) and not isinstance(obj, property):
            out["signature"] = name.split(".")[-1] + str(inspect.signature(target)).replace("self, ", "").replace("(self)", "()")
    except (TypeError, ValueError):
        pass
    doc = inspect.getdoc(target)
    if doc and not doc.startswith(("str(object", "int(", "Return")):
        out["doc"] = doc[:600]
    return out


def _property_class(owner, pname: str, prop):
    """The class a property returns: its return annotation, else a class named <Owner><Property> (Panel.Controls ->
    PanelControls), <Property> (Range.Font -> Font) or <Property> without its plural "s"; None when none is found."""
    import sys

    from .. import excel, ui

    fget = prop.fget
    ann = getattr(fget, "__annotations__", {}).get("return")
    if isinstance(ann, str):
        mod = sys.modules.get(getattr(fget, "__module__", ""), None)
        ann = getattr(mod, ann.strip("'\""), None) if mod else None
    if inspect.isclass(ann):
        return ann
    for cand in (owner.__name__.lstrip("_") + pname, pname, pname[:-1] if pname.endswith("s") else None):
        for m in (excel, ui):
            c = getattr(m, cand, None) if cand else None
            if inspect.isclass(c):
                return c
    return None


def lookup(query: str, kind: str = "any", max_results: int = 10) -> dict:
    res: dict = {"query": query}
    try:
        intro = _introspect(query)
    except Exception:  # noqa: BLE001
        intro = None
    if intro and (kind in ("any", intro["kind"]) or (kind == "member" and intro["kind"] in ("member", "function"))):
        res["object_model"] = intro
    toks = _tokens(query)
    scored = []
    for e in _entries():
        t = e["text"].lower()
        sec = e["section"].lower()
        if kind == "event" and "_" not in t and "event" not in sec:
            continue
        if kind == "constant" and not re.search(r"\b(xl|vb|yc|mso|fm)[a-z]", t):
            continue
        score = 0
        for tok in toks:
            if tok in t:
                score += 3 + t.count(tok)
            if tok in sec:
                score += 2
        if query.lower() in t:
            score += 10
        if score:
            scored.append((score, e))
    scored.sort(key=lambda x: -x[0])
    res["docs"] = [{"file": e["file"], "section": e["section"], "text": e["text"][:800]} for _, e in scored[:max_results]]
    if not res["docs"] and not intro:
        res["message"] = "No matching entries: it may not be supported yet. Try other keywords (class names, member names, the Excel spelling) and don't invent APIs."
    return res
