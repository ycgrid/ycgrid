# SPDX-License-Identifier: Apache-2.0
"""Implementation of the MCP tools (definitions in mcp/tools.json).

Every tool is a function whose parameter names match its inputSchema; it returns a string, a dict (turned into JSON
text) or a list of Content. Errors raise ToolError (the message is for the AI: say clearly what to do next).
"""
from __future__ import annotations

import base64
import contextlib
import fnmatch
import io
import json
import os
import re
import tempfile
import time
from decimal import Decimal
from typing import Any, Optional

from .. import _conn
from ..types import XlError

STATE: dict = {"target": None}


class ToolError(Exception):
    pass


class Image:
    def __init__(self, data: bytes, text: str = ""):
        self.data, self.text = data, text


def to_content(res) -> list:
    if isinstance(res, Image):
        out = [{"type": "image", "data": base64.b64encode(res.data).decode("ascii"), "mimeType": "image/png"}]
        if res.text:
            out.append({"type": "text", "text": res.text})
        return out
    if isinstance(res, str):
        return [{"type": "text", "text": res}]
    return [{"type": "text", "text": json.dumps(res, ensure_ascii=False, indent=1, default=_jd)}]


def _jd(o):
    if isinstance(o, Decimal):
        return str(o)
    if isinstance(o, XlError):
        return o.Text
    if hasattr(o, "isoformat"):
        return o.isoformat()
    return repr(o)


def describe_error(e: Exception) -> str:
    from .._conn import YcConnectionError, YcError

    if isinstance(e, YcConnectionError):
        STATE["connected"] = False
        return f"The connection to YCGrid was lost: {e}. Make sure YCGrid is still running, then call yc_connect."
    if isinstance(e, YcError):
        return f"YCGrid reported an error: {e}"
    return f"{type(e).__name__}: {e}"


# ---------------------------------------------------------------------------
# connection
# ---------------------------------------------------------------------------
def _alive_instances() -> list:
    from .. import _pid_alive

    return [i for i in _conn.find_instances() if i.get("pipe") and _pid_alive(i.get("pid", 0))]


def _ensure():
    c = _conn._current
    if c is not None and not c.closed:
        return c
    import yc

    try:
        tgt = STATE.get("target")
        if isinstance(tgt, str) and tgt.isdigit():
            tgt = int(tgt)
        if tgt is None:
            alive = _alive_instances()
            if len(alive) > 1:
                here = os.path.normcase(os.path.abspath(os.getcwd()))
                if not any(os.path.normcase(os.path.abspath(i.get("app_dir") or "")) == here for i in alive):
                    raise ToolError("Several YCGrid instances are running; call yc_connect with a target (pid or program folder) first:\n" + _instances_text(alive))
        return yc.connect(tgt)
    except yc.YcConnectionError as e:
        raise ToolError(f"{e}. Start YCGrid first (double-click a .ycg or ycgrid.exe), then call yc_connect.") from None


def rq(method: str, **params) -> Any:
    return _ensure().request(method, params)


def _instances_text(alive) -> str:
    return "\n".join(f"- pid {i.get('pid')}: {i.get('app_dir') or '(not saved)'} {i.get('workbook') or ''}" for i in alive)


# ---------------------------------------------------------------------------
# addresses and sheets
# ---------------------------------------------------------------------------
def _excel():
    _ensure()
    from .. import excel

    return excel


def _info() -> dict:
    return rq("wb.info")


def _sheet_rec(sheet: Optional[str]) -> dict:
    info = _info()
    sheets = info["sheets"]
    if sheet is None or sheet == "":
        v = rq("view.get")
        sid = v.get("focus_sheet") or v.get("sheet")
        for s in sheets:
            if s["id"] == sid:
                return s
        return sheets[0]
    key = str(sheet)
    if key.isdigit():
        i = int(key)
        if 1 <= i <= len(sheets):
            return sheets[i - 1]
    for s in sheets:
        if s["name"].lower() == key.lower() or s["code_name"].lower() == key.lower():
            return s
    raise ToolError(f"Sheet \"{sheet}\" not found. Existing sheets: {', '.join(s['name'] for s in sheets)}")


def _split(address: str, sheet: Optional[str]):
    """Address (may have a sheet prefix or be a defined name) -> (sheet record, (r0,c0,r1,c1))."""
    excel = _excel()
    if not address:
        raise ToolError("address is required")
    sname, a = excel._split_sheet(address)
    rec = _sheet_rec(sname if sname is not None else sheet)
    try:
        area = excel._parse_area(a.split(",")[0])
    except Exception:  # noqa: BLE001
        # defined names
        try:
            res = rq("names.resolve", name=a, sheet=rec["id"])
        except Exception:  # noqa: BLE001
            res = None
        if not res:
            raise ToolError(f"Invalid address: {address}") from None
        rec = next(s for s in _info()["sheets"] if s["id"] == res["sheet"])
        area = tuple(res["rect"])
    return rec, area


def _clip_used(rec: dict, area):
    r0, c0, r1, c1 = area
    used = rec.get("used")
    if r1 >= 0xFFFFFFF0 or r1 >= rec.get("row_limit", 1 << 31) - 1:
        r1 = max(r0, used[2]) if used else r0
    if c0 == 0 and c1 >= 16383:
        c1 = max(c0, used[3]) if used else c0
    return r0, c0, r1, c1


def _a1(r, c):
    from .. import excel

    return excel._col_name(c) + str(r + 1)


def _addr(area):
    r0, c0, r1, c1 = area
    return _a1(r0, c0) if (r0, c0) == (r1, c1) else f"{_a1(r0, c0)}:{_a1(r1, c1)}"


def _rng(address: str, sheet: Optional[str]):
    excel = _excel()
    rec, area = _split(address, sheet)
    return excel.Worksheets(rec["name"]).Range(_addr(area)), rec, area


def _color(hexstr: Optional[str]) -> Optional[int]:
    if not hexstr or hexstr == "none":
        return None
    h = hexstr.lstrip("#")
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    return r | (g << 8) | (b << 16)


def _fmt_value(v, date=False, date_mode="iso"):
    if v is None:
        return ""
    if isinstance(v, bool):
        return "TRUE" if v else "FALSE"
    if isinstance(v, XlError):
        return v.Text
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=False)
    if isinstance(v, float):
        if date and date_mode == "iso":
            from ..types import serial_to_datetime

            try:
                d = serial_to_datetime(v)
                return d.isoformat(sep=" ") if (d.hour or d.minute or d.second or d.microsecond) else d.date().isoformat()
            except Exception:  # noqa: BLE001
                pass
        return repr(int(v)) if v.is_integer() and abs(v) < 1e15 else repr(v)
    return str(v)


def _table(rows: list, r0: int, c0: int) -> str:
    from .. import excel

    ncols = max((len(r) for r in rows), default=0)
    head = ["", *[excel._col_name(c0 + j) for j in range(ncols)]]
    lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for i, row in enumerate(rows):
        cells = [str(r0 + i + 1), *[str(x).replace("|", "\\|").replace("\n", "⏎") for x in row]]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# 1. Connection and workbook
# ---------------------------------------------------------------------------
def yc_connect(target: Optional[str] = None):
    import yc

    alive = _alive_instances()
    if not alive:
        raise ToolError("No running YCGrid was found. Open YCGrid first (double-click a .ycg or ycgrid.exe).")
    if target is not None:
        STATE["target"] = target
    if _conn._current is not None:
        yc.disconnect()
    try:
        c = _ensure()
    except ToolError as e:
        return {"instances": alive, "selected": None, "message": str(e)}
    hello = c.request("hello", {"role": "controller", "pid": os.getpid()})
    return {"instances": [{"instance_id": i.get("pid"), "pid": i.get("pid"), "app_dir": i.get("app_dir"), "workbook": i.get("workbook"),
                           "pipe": i.get("pipe")} for i in alive],
            "selected": {"pid": hello.get("pid"), "app_dir": hello.get("app_dir"), "app_version": hello.get("app_version")},
            "python_host": _safe(lambda: rq("python.status"), {})}


def _safe(fn, default=None):
    try:
        return fn()
    except Exception:  # noqa: BLE001
        return default


def yc_get_workbook_info(include: Optional[list] = None):
    inc = set(include or ["file", "sheets", "names", "selection", "calc", "options", "python", "charts", "layout"])
    info = _info()
    st = _safe(lambda: rq("python.status"), {}) or {}
    app_dir = st.get("app_dir") or ""
    out: dict = {}
    if "file" in inc:
        out["file"] = {"path": info.get("path"), "dirty": info.get("dirty"), "read_only": info.get("read_only"), "app_dir": app_dir}
    if "sheets" in inc or "charts" in inc:
        sheets = []
        for i, s in enumerate(info["sheets"]):
            objs = _safe(lambda s=s: rq("obj.list", sheet=s["id"]), {"controls": [], "charts": []})
            d = {"index": i + 1, "name": s["name"], "code_name": s["code_name"], "visible": s["visible"],
                 "used_range": _addr(s["used"]) if s.get("used") else None, "controls": len(objs["controls"]), "charts": len(objs["charts"]),
                 "module": f"sheets/{s['code_name']}.py", "module_exists": bool(app_dir) and os.path.isfile(os.path.join(app_dir, "sheets", s["code_name"] + ".py"))}
            if "charts" in inc and objs["charts"]:
                d["chart_names"] = [c["name"] for c in objs["charts"]]
            sheets.append(d)
        out["sheets"] = sheets
    if "names" in inc:
        out["names"] = _safe(lambda: rq("names.list"), [])
    if "selection" in inc:
        v = _safe(lambda: rq("view.get"), {}) or {}
        sid = v.get("focus_sheet")
        out["selection"] = {"sheet": next((s["name"] for s in info["sheets"] if s["id"] == sid), None),
                            "areas": [_addr(a) for a in v.get("areas", [])], "active_cell": _a1(*v["active"]) if v.get("active") else None,
                            "visible_range": _addr(v["visible"]) if v.get("visible") else None, "zoom": v.get("zoom")}
    if "calc" in inc:
        out["calc"] = _safe(lambda: rq("calc.get"), {})
    if "options" in inc:
        out["options"] = {"design_mode": _safe(lambda: rq("ui.design_mode_get")["on"], False)}
    if "python" in inc:
        out["python"] = {k: st.get(k) for k in ("running", "ready", "python_version", "macros", "missing_packages", "recent_errors") if k in st}
    if "layout" in inc:
        out["layout"] = {"containers": _safe(lambda: rq("container.list"), []), "panels": [p["name"] for p in _safe(lambda: rq("panel.list"), [])]}
    return out


# ---------------------------------------------------------------------------
# 2. Reading
# ---------------------------------------------------------------------------
def yc_read_range(address: str, sheet: Optional[str] = None, fields: Optional[list] = None, max_cells: int = 2000, row_offset: int = 0,
                  date_mode: str = "iso", summary_only: bool = False, output: str = "table"):
    rec, area = _split(address, sheet)
    r0, c0, r1, c1 = _clip_used(rec, area)
    nrows, ncols = r1 - r0 + 1, c1 - c0 + 1
    if summary_only:
        vals = rq("range.get", sheet=rec["id"], rect=[r0, c0, min(r1, r0 + 199), c1], what="value")["values"]
        kinds = []
        for j in range(ncols):
            col = [row[j] for row in vals[1:] if j < len(row) and row[j] is not None]
            kinds.append("number" if col and all(isinstance(x, (int, float, Decimal)) and not isinstance(x, bool) for x in col)
                         else "text" if col else "empty")
        return {"sheet": rec["name"], "range": _addr((r0, c0, r1, c1)), "rows": nrows, "columns": ncols,
                "first_row": [_fmt_value(x) for x in (vals[0] if vals else [])], "column_kinds": dict(zip([_a1(r0, c0 + j)[:-len(str(r0 + 1))] for j in range(ncols)], kinds))}
    fields = fields or ["value"]
    per_row = max(1, ncols)
    take = max(1, min(nrows - row_offset, max(1, max_cells // per_row)))
    if row_offset >= nrows:
        raise ToolError(f"row_offset out of range ({nrows} rows in total)")
    a0 = r0 + row_offset
    a1 = a0 + take - 1
    data = {}
    what = {"value": "value", "formula": "formula", "text": "text", "number_format": "numfmt"}
    dates = set()
    for f in fields:
        res = rq("range.get", sheet=rec["id"], rect=[a0, c0, a1, c1], what=what[f])
        data[f] = res["values"]
        if f == "value":
            dates = {(d[0], d[1]) for d in res.get("dates", [])}
    rows = []
    for i in range(take):
        row = []
        for j in range(ncols):
            parts = []
            for f in fields:
                v = data[f][i][j] if i < len(data[f]) and j < len(data[f][i]) else None
                if f == "value":
                    parts.append(_fmt_value(v, (i, j) in dates, date_mode))
                elif f == "formula":
                    parts.append(v if isinstance(v, str) and v.startswith("=") else ("" if len(fields) > 1 else _fmt_value(v)))
                else:
                    parts.append(str(v or ""))
            row.append(" ¦ ".join(p for p in parts if p != "") if len(fields) > 1 else parts[0])
        rows.append(row)
    more = a1 < r1
    note = f"\n(truncated: {r1 - a1} more rows, use row_offset={row_offset + take} for the next page)" if more else ""
    head = f"Sheet \"{rec['name']}\" {_addr((a0, c0, a1, c1))}" + (f" (fields: {' ¦ '.join(fields)})" if len(fields) > 1 else "")
    if output == "json":
        return {"sheet": rec["name"], "range": _addr((a0, c0, a1, c1)), "fields": fields, "rows": rows, "truncated": more,
                "next_row_offset": row_offset + take if more else None}
    return head + "\n" + _table(rows, a0, c0) + note


def yc_inspect_cells(address: str, sheet: Optional[str] = None, include: Optional[list] = None, dependency_depth: int = 1, max_cells: int = 20):
    rec, area = _split(address, sheet)
    r0, c0, r1, c1 = _clip_used(rec, area)
    inc = set(include or ["value", "formula", "style", "merge", "comment"])
    out = []
    n = 0
    for r in range(r0, r1 + 1):
        for c in range(c0, c1 + 1):
            if n >= max_cells:
                break
            n += 1
            d: dict = {"address": _a1(r, c)}
            one = [r, c, r, c]
            if "value" in inc:
                v = rq("range.get", sheet=rec["id"], rect=one, what="value")
                val = v["values"][0][0]
                d["value"] = val
                d["type"] = type(val).__name__ if val is not None else "empty"
                d["text"] = rq("range.get", sheet=rec["id"], rect=one, what="text")["values"][0][0]
            if "formula" in inc:
                f = rq("range.get", sheet=rec["id"], rect=one, what="formula")["values"][0][0]
                if isinstance(f, str) and f.startswith("="):
                    d["formula"] = f
                    try:
                        excel = _excel()
                        d["formula_r1c1"] = excel.Worksheets(rec["name"]).Range(_a1(r, c)).FormulaR1C1
                    except Exception:  # noqa: BLE001
                        pass
            if "style" in inc:
                d["style"] = rq("range.style_get", sheet=rec["id"], row=r, col=c)
                d["number_format"] = rq("range.get", sheet=rec["id"], rect=one, what="numfmt")["values"][0][0]
            if "merge" in inc:
                m = _safe(lambda: rq("range.merge_area", sheet=rec["id"], row=r, col=c))
                if m and tuple(m) != (r, c, r, c):
                    d["merge_area"] = _addr(m)
            if "validation" in inc:
                dv = _safe(lambda: rq("dv.get", sheet=rec["id"], row=r, col=c))
                if dv:
                    d["validation"] = {k: dv[k] for k in ("type", "op", "formula1", "formula2", "alert", "input_message", "error_message")}
            if "conditional_format" in inc:
                rules = _safe(lambda: rq("cf.list", sheet=rec["id"], rect=[r, c, r, c]), []) or []
                if rules:
                    d["conditional_formats"] = [{k: x[k] for k in ("id", "type", "op", "formula1", "formula2", "text", "fill", "font_color", "priority")}
                                                for x in rules]
            if inc & {"precedents", "dependents", "hyperlink", "binding"}:
                d["unavailable"] = "Views of the dependency tree, hyperlinks and topic bindings are not available yet"
            out.append(d)
    return {"sheet": rec["name"], "cells": out, "truncated": (r1 - r0 + 1) * (c1 - c0 + 1) > max_cells}


def _excel_pattern(what: str, look_at: str, match_case: bool):
    # Excel's * ? ~ wildcards -> regular expression
    out = ""
    i = 0
    while i < len(what):
        ch = what[i]
        if ch == "~" and i + 1 < len(what):
            out += re.escape(what[i + 1])
            i += 2
            continue
        out += ".*" if ch == "*" else "." if ch == "?" else re.escape(ch)
        i += 1
    if look_at == "whole":
        out = "^" + out + "$"
    return re.compile(out, 0 if match_case else re.IGNORECASE)


def yc_find(what: str, scope: str = "sheet", sheet: Optional[str] = None, look_in: str = "values", look_at: str = "part",
            match_case: bool = False, regex: bool = False, max_results: int = 100):
    if look_in == "comments":
        raise ToolError("Searching notes is not available yet; use look_in=values or formulas")
    pat = re.compile(what if look_at == "part" else f"^(?:{what})$", 0 if match_case else re.IGNORECASE) if regex else _excel_pattern(what, look_at, match_case)
    recs = _info()["sheets"] if scope == "workbook" else [_sheet_rec(sheet)]
    hits, total = [], 0
    for rec in recs:
        if not rec.get("used"):
            continue
        r0, c0, r1, c1 = rec["used"]
        step = max(1, 200000 // max(1, c1 - c0 + 1))
        for a in range(r0, r1 + 1, step):
            b = min(r1, a + step - 1)
            vals = rq("range.get", sheet=rec["id"], rect=[a, c0, b, c1], what="formula" if look_in == "formulas" else "text")["values"]
            for i, row in enumerate(vals):
                for j, v in enumerate(row):
                    if v is None or v == "":
                        continue
                    s = v if isinstance(v, str) else _fmt_value(v)
                    if pat.search(s):
                        total += 1
                        if len(hits) < max_results:
                            hits.append({"sheet": rec["name"], "address": _a1(a + i, c0 + j), "content": s[:200]})
    return {"matches": hits, "total": total, "truncated": total > len(hits)}


# ---------------------------------------------------------------------------
# 3. Writing and formatting
# ---------------------------------------------------------------------------
def yc_write_range(address: str, sheet: Optional[str] = None, values: Optional[list] = None, fill_value: Any = None, mode: str = "auto",
                   fire_events: bool = True, undoable: bool = True, wait_recalc: bool = True):
    rng, rec, area = _rng(address, sheet)
    r0, c0, r1, c1 = area
    if values is None and fill_value is None:
        raise ToolError("values (a 2D array) or fill_value is required")
    if values is not None:
        if len(values) != r1 - r0 + 1 or any(len(row) != c1 - c0 + 1 for row in values):
            raise ToolError(f"values must be {r1 - r0 + 1} rows x {c1 - c0 + 1} columns to match {address}")
        data = values
    else:
        data = fill_value
    if mode == "values":
        conv = (lambda x: "'" + x if isinstance(x, str) and x.startswith("=") else x)
        data = [[conv(x) for x in row] for row in data] if values is not None else conv(data)
    excel = _excel()
    prev = None
    if not fire_events:
        prev = excel.Application.EnableEvents
    if mode == "formulas":
        rng.Formula = data
    else:
        rng.Value = data
    if wait_recalc:
        rq("sync", wait="engine")
    preview = yc_read_range(_addr((r0, c0, min(r1, r0 + 4), c1)), rec["name"], fields=["text"], max_cells=50)
    notes = []
    if not fire_events:
        notes.append("fire_events=false: YCGrid decides whether writes from a controller connection fire events; to fire no events at all, use Application.EnableEvents=False in the host (yc_exec_python target=host)")
    if not undoable:
        notes.append("undoable=false was ignored (Python's writes are never steps of the user's undo list and leave it as it was; ADR 0013)")
    del prev
    return f"Wrote \"{rec['name']}\" {_addr(area)} ({(r1 - r0 + 1) * (c1 - c0 + 1)} cells).\n" + preview + ("\n" + "\n".join(notes) if notes else "")


_LINE = {"none": -4142, "thin": 1, "medium": 1, "thick": 1, "double": -4119, "hair": 1, "dotted": -4118, "dashed": -4115, "dash_dot": 4,
         "dash_dot_dot": 5, "medium_dashed": -4115, "medium_dash_dot": 4, "medium_dash_dot_dot": 5, "slant_dash_dot": 13}
_WEIGHT = {"thin": 2, "medium": -4138, "thick": 4, "hair": 1, "medium_dashed": -4138, "medium_dash_dot": -4138, "medium_dash_dot_dot": -4138,
           "slant_dash_dot": -4138}
_EDGES = {"left": [7], "top": [8], "bottom": [9], "right": [10], "inside_vertical": [11], "inside_horizontal": [12], "inside": [11, 12],
          "outline": [7, 8, 9, 10], "all": [7, 8, 9, 10, 11, 12], "diagonal_down": [5], "diagonal_up": [6]}
_HALIGN = {"general": 1, "left": -4131, "center": -4108, "right": -4152, "fill": 5, "justify": -4130, "center_across_selection": 7, "distributed": -4117}
_VALIGN = {"top": -4160, "center": -4108, "bottom": -4107, "justify": -4130, "distributed": -4117}
_ULINE = {"none": -4142, "single": 2, "double": -4119, "single_accounting": 4, "double_accounting": 5}


def yc_format_range(address: str, sheet: Optional[str] = None, number_format: Optional[str] = None, font: Optional[dict] = None,
                    fill: Optional[dict] = None, borders: Optional[list] = None, alignment: Optional[dict] = None, merge: Optional[str] = None,
                    column_width: Optional[float] = None, row_height: Optional[float] = None, autofit: Optional[str] = None, clear: Optional[str] = None):
    rng, rec, area = _rng(address, sheet)
    done = []
    if clear:
        {"formats": rng.ClearFormats, "contents": rng.ClearContents, "all": rng.Clear}.get(clear, lambda: None)()
        done.append(f"cleared {clear}" + (" (clearing notes and hyperlinks is not available yet)" if clear in ("comments", "hyperlinks") else ""))
    if number_format is not None:
        rng.NumberFormat = number_format
        done.append("number format")
    if font:
        f = rng.Font
        for k, attr in (("name", "Name"), ("size", "Size"), ("bold", "Bold"), ("italic", "Italic"), ("strikethrough", "Strikethrough")):
            if k in font:
                setattr(f, attr, font[k])
        if "underline" in font:
            f.Underline = _ULINE[font["underline"]]
        if "color" in font:
            f.Color = _color(font["color"])
        if font.get("superscript") or font.get("subscript"):
            done.append(" (superscript / subscript not available yet)")
        done.append("font")
    if fill:
        c = fill.get("color")
        if c == "none":
            rng.Interior.ColorIndex = -4142
        elif c:
            rng.Interior.Color = _color(c)
        done.append("fill")
    for b in borders or []:
        for e in b["edges"]:
            for idx in _EDGES[e]:
                bd = rng.Borders(idx)
                bd.LineStyle = _LINE[b["style"]]
                if b["style"] != "none":
                    if b["style"] in _WEIGHT:
                        bd.Weight = _WEIGHT[b["style"]]
                    if b.get("color"):
                        bd.Color = _color(b["color"])
        done.append("borders")
    if alignment:
        # the indent first: Excel makes an indented General or centered cell left-aligned, and an alignment asked for
        # explicitly wins over that
        if "indent" in alignment:
            rng.IndentLevel = alignment["indent"]
        if "horizontal" in alignment:
            rng.HorizontalAlignment = _HALIGN[alignment["horizontal"]]
        if "vertical" in alignment:
            rng.VerticalAlignment = _VALIGN[alignment["vertical"]]
        if "wrap_text" in alignment:
            rng.WrapText = alignment["wrap_text"]
        if "shrink_to_fit" in alignment:
            rng.ShrinkToFit = alignment["shrink_to_fit"]
        if "orientation" in alignment:
            rng.Orientation = alignment["orientation"]
        done.append("alignment")
    if merge:
        if merge == "unmerge":
            rng.UnMerge()
        else:
            rng.Merge(Across=(merge == "merge_across"))
            if merge == "merge_center":
                rng.HorizontalAlignment = -4108
        done.append(merge)
    if column_width is not None:
        rng.ColumnWidth = column_width
        done.append("column width")
    if row_height is not None:
        rng.RowHeight = row_height
        done.append("row height")
    if autofit:
        if autofit in ("columns", "both"):
            rng.Columns.AutoFit()
        if autofit in ("rows", "both"):
            rng.Rows.AutoFit()
        done.append("autofit")
    return f"Applied to \"{rec['name']}\" {_addr(area)}: {', '.join(done) or '(no changes)'}"


def yc_edit_structure(action: str, target: Optional[str] = None, sheet: Optional[str] = None, shift: Optional[str] = None):
    excel = _excel()
    rec = _sheet_rec(sheet)
    ws = excel.Worksheets(rec["name"])
    if action == "unfreeze_panes":
        rq("view.set", sheet=rec["id"], freeze=[0, 0])
        return f"Unfroze the panes of \"{rec['name']}\""
    if not target:
        raise ToolError("target is required (e.g. \"5:7\", \"C:D\", \"B2:C3\")")
    rng = ws.Range(target)
    if action in ("insert_rows", "insert_columns", "insert_cells"):
        if action == "insert_rows":
            rng.EntireRow.Insert()
        elif action == "insert_columns":
            rng.EntireColumn.Insert()
        else:
            rng.Insert(Shift=-4121 if shift in (None, "down") else -4161)
    elif action in ("delete_rows", "delete_columns", "delete_cells"):
        if action == "delete_rows":
            rng.EntireRow.Delete()
        elif action == "delete_columns":
            rng.EntireColumn.Delete()
        else:
            rng.Delete(Shift=-4162 if shift in (None, "up") else -4159)
    elif action in ("hide", "unhide"):
        is_col = bool(re.fullmatch(r"\$?[A-Za-z]+(:\$?[A-Za-z]+)?", target))
        (rng.EntireColumn if is_col else rng.EntireRow).Hidden = action == "hide"
    elif action == "freeze_panes":
        r = rng.Row - 1
        c = rng.Column - 1
        rq("view.set", sheet=rec["id"], scroll_row=0, scroll_col=0, freeze=[r, c])
    elif action in ("group", "ungroup"):
        # Data > Group / Ungroup: whole columns ("C:D") group columns, anything else groups its rows
        is_col = bool(re.fullmatch(r"\$?[A-Za-z]+(:\$?[A-Za-z]+)?", target))
        whole = rng.EntireColumn if is_col else rng.EntireRow
        whole.Group() if action == "group" else whole.Ungroup()
    else:
        raise ToolError(f"Unknown action: {action}")
    return f"\"{rec['name']}\" {action} {target} done"


def yc_manage_sheets(action: str, sheet: Optional[str] = None, new_name: Optional[str] = None,
                     before: Optional[str] = None, after: Optional[str] = None, tab_color: Optional[str] = None, confirm: Optional[bool] = None):
    excel = _excel()
    if action == "add":
        kw = {}
        if before:
            kw["Before"] = excel.Worksheets(_sheet_rec(before)["name"])
        if after:
            kw["After"] = excel.Worksheets(_sheet_rec(after)["name"])
        ws = excel.Worksheets.Add(**kw)
        if new_name:
            ws.Name = new_name
        return f"Added sheet \"{ws.Name}\" (CodeName {ws.CodeName}; the module file sheets/{ws.CodeName}.py can be created when needed)"
    rec = _sheet_rec(sheet)
    ws = excel.Worksheets(rec["name"])
    if action == "delete":
        if not confirm:
            raise ToolError("Deleting a sheet can't be undone: pass confirm=true to delete it")
        ws.Delete()
        return f"Deleted \"{rec['name']}\""
    if action == "rename":
        ws.Name = new_name
        return f"Renamed to \"{new_name}\""
    if action in ("move", "copy"):
        kw = {}
        if before:
            kw["Before"] = excel.Worksheets(_sheet_rec(before)["name"])
        if after:
            kw["After"] = excel.Worksheets(_sheet_rec(after)["name"])
        (ws.Move if action == "move" else ws.Copy)(**kw)
        return f"{action} done"
    if action in ("hide", "very_hide", "unhide"):
        ws.Visible = {"hide": 0, "very_hide": 2, "unhide": -1}[action]
        return f"{action} done"
    if action == "activate":
        if ws.Visible != -1:  # Worksheet.Activate of a hidden sheet does nothing, as in Excel
            raise ToolError(f"\"{rec['name']}\" is hidden: unhide it first (action=\"unhide\")")
        ws.Activate()
        return f"Switched to \"{rec['name']}\""
    if action == "set_tab_color":
        c = _color(tab_color)
        ws.Tab.Color = c if c is not None else False
        return "Tab color set"
    raise ToolError(f"Unknown action: {action}")


# ---------------------------------------------------------------------------
# 4. Controls and charts
# ---------------------------------------------------------------------------
_PLACEMENT = {"move_and_size": 1, "move": 2, "free_floating": 3, "float_window": 100}
_PLACEMENT_NAME = {v: k for k, v in _PLACEMENT.items()}


def _geom_from(rec, cover_range, left, top, width, height):
    if cover_range:
        g = rq("range.geometry", sheet=rec["id"], rect=list(_split(cover_range, rec["name"])[1]))
        return {"left": g["left"], "top": g["top"], "width": g["width"], "height": g["height"]}
    out = {}
    for k, v in (("left", left), ("top", top), ("width", width), ("height", height)):
        if v is not None:
            out[k] = float(v)
    return out


def _handler_exists(app_dir, code, fn) -> bool:
    path = os.path.join(app_dir or "", "sheets", code + ".py")
    if not os.path.isfile(path):
        return False
    with open(path, encoding="utf-8") as f:
        return re.search(rf"^\s*def\s+{re.escape(fn)}\s*\(", f.read(), re.M) is not None


def yc_list_controls(sheet: Optional[str] = None, check_handlers: bool = True, kind: str = "any"):
    recs = [_sheet_rec(sheet)] if sheet else _info()["sheets"]
    app_dir = (_safe(lambda: rq("python.status"), {}) or {}).get("app_dir", "")
    out = []
    for rec in recs:
        for c in rq("obj.list", sheet=rec["id"])["controls"]:
            if kind != "any" and c["kind"] != kind:
                continue
            ev = _default_event(c["kind"])
            d = {"sheet": rec["name"], "code_name": rec["code_name"], "name": c["name"], "kind": c["kind"],
                 "left": c["left"], "top": c["top"], "width": c["width"], "height": c["height"], "placement": _PLACEMENT_NAME.get(c["placement"]),
                 "enabled": c["enabled"], "visible": c["visible"]}
            if c["kind"] == "button":
                d["caption"] = c["caption"]
            else:
                d.update(value=c["value"], list_index=c["list_index"], list_fill_range=c["list_fill_range"], items=c["items"], linked_cell=c["linked_cell"])
            if check_handlers:
                d["handler"] = f"sheets/{rec['code_name']}.py: {c['name']}_{ev}()"
                d["handler_exists"] = _handler_exists(app_dir, rec["code_name"], f"{c['name']}_{ev}")
            out.append(d)
    return out


def _ctl_props(caption=None, placement=None, font=None, back_color=None, fore_color=None, word_wrap=None, list_fill_range=None, items=None,
               linked_cell=None, style=None, enabled=None, visible=None, value=None):
    p = {}
    if caption is not None:
        p["caption"] = caption
    if placement:
        p["placement"] = _PLACEMENT[placement]
    if font:
        for k, kk in (("name", "font_name"), ("size", "font_size"), ("bold", "bold"), ("italic", "italic")):
            if k in font:
                p[kk] = font[k]
    if back_color:
        p["back_color"] = _color(back_color)
    if fore_color:
        p["fore_color"] = _color(fore_color)
    if word_wrap is not None:
        p["word_wrap"] = word_wrap
    if list_fill_range is not None:
        p["list_fill_range"] = list_fill_range
    if items is not None:
        p["items"] = items
    if linked_cell is not None:
        p["linked_cell"] = linked_cell
    if style:
        p["style"] = 1 if style == "dropdown_list" else 0
    if enabled is not None:
        p["enabled"] = enabled
    if visible is not None:
        p["visible"] = visible
    if value is not None:
        p["value"] = value
    return p


# the event a control's handler is written for: the default event Excel's View Code opens, as YCGrid fires it
# (DefaultControlEvent in apps/ycgrid/objects_ui.cpp)
_CHANGE_KINDS = ("combobox", "textbox", "spinbutton", "scrollbar")


def _default_event(kind: str) -> str:
    return "Change" if kind in _CHANGE_KINDS else "Click"


def _extra_ctl_props(value=None, group_name=None, min=None, max=None, small_change=None, large_change=None,  # noqa: A002
                     multi_select=None, multi_line=None):
    p = {}
    for k, v in (("group_name", group_name), ("min", min), ("max", max), ("small_change", small_change), ("large_change", large_change),
                 ("multi_line", multi_line)):
        if v is not None:
            p[k] = v
    if multi_select is not None:
        p["multi_select"] = {"single": 0, "multi": 1, "extended": 2}[multi_select]
    if value is not None:
        p["value"] = value
    return p


def yc_add_control(sheet: Optional[str] = None, name: Optional[str] = None, caption: Optional[str] = None, cover_range: Optional[str] = None,
                   left=None, top=None, width=None, height=None, placement=None, font=None, back_color=None, fore_color=None, word_wrap=None,
                   kind: str = "button", list_fill_range=None, items=None, linked_cell=None, style=None, value=None, group_name=None,
                   min=None, max=None, small_change=None, large_change=None, multi_select=None, multi_line=None):  # noqa: A002
    from ..ui import _SIZES

    rec = _sheet_rec(sheet)
    if kind not in _SIZES:
        raise ToolError(f"Unknown control kind: {kind}; available: {', '.join(_SIZES)}")
    p = {"kind": kind, **_geom_from(rec, cover_range, left, top, width, height),
         **_ctl_props(caption, placement, font, back_color, fore_color, word_wrap, list_fill_range, items, linked_cell, style),
         **_extra_ctl_props(value, group_name, min, max, small_change, large_change, multi_select, multi_line)}
    p.setdefault("width", float(_SIZES[kind][0]))
    p.setdefault("height", float(_SIZES[kind][1]))
    if name:
        p["name"] = name
    c = rq("ctl.add", sheet=rec["id"], **p)
    ev = _default_event(kind)
    return {"name": c["name"], "sheet": rec["name"], "left": c["left"], "top": c["top"], "width": c["width"], "height": c["height"],
            "handler_file": f"sheets/{rec['code_name']}.py",
            "handler_template": f"def {c['name']}_{ev}():\n    " + ("Range(\"A1\").Value = \"clicked\"" if kind == "button" else f"v = Me.{c['name']}.Value")}


def yc_modify_control(name: str, sheet: Optional[str] = None, new_name=None, caption=None, cover_range=None, left=None, top=None, width=None,
                      height=None, placement=None, enabled=None, visible=None, font=None, back_color=None, fore_color=None, word_wrap=None,
                      z_order=None, delete: bool = False, list_fill_range=None, items=None, linked_cell=None, style=None, value=None):
    rec = _sheet_rec(sheet)
    if delete:
        rq("ctl.delete", sheet=rec["id"], name=name)
        return f"Deleted control {name} (the event functions {name}_Click / _Change are still in sheets/{rec['code_name']}.py)"
    p = {**_geom_from(rec, cover_range, left, top, width, height),
         **_ctl_props(caption, placement, font, back_color, fore_color, word_wrap, list_fill_range, items, linked_cell, style, enabled, visible, value)}
    if new_name:
        p["name"] = new_name
    if z_order:
        lst = rq("obj.list", sheet=rec["id"])
        zs = [c["z"] for c in lst["controls"]] + [c["z"] for c in lst["charts"]] or [0]
        cur = next((c["z"] for c in lst["controls"] if c["name"].lower() == name.lower()), 0)
        p["z"] = {"bring_to_front": max(zs) + 1, "send_to_back": min(zs) - 1, "bring_forward": cur + 1, "send_backward": cur - 1}[z_order]
    c = rq("ctl.set", sheet=rec["id"], name=name, props=p)
    msg = {"control": c}
    if new_name:
        msg["reminder"] = f"Rename the event functions too: {name}_Click -> {new_name}_Click (sheets/{rec['code_name']}.py)"
    return msg


def yc_trigger_control(name: str, sheet: Optional[str] = None, dialogs: str = "auto", timeout_ms: int = 10000, event: str = "click",
                       value: Optional[str] = None, list_index: Optional[int] = None):
    rec = _sheet_rec(sheet)
    p = {"sheet": rec["id"], "name": name, "event": "Change" if event == "change" else "Click", "dialogs": dialogs, "timeout_ms": timeout_ms}
    if value is not None:
        p["value"] = value
    if list_index is not None:
        p["list_index"] = list_index
    return rq("control.trigger", **p)


_XL_CHART = {"xlLine": 4, "xlLineMarkers": 65, "xlColumnClustered": 51, "xlColumnStacked": 52, "xlBarClustered": 57, "xlXYScatter": -4169,
             "xlXYScatterLines": 74, "xlArea": 1, "xlPie": 5, "xlDoughnut": -4120, "xlStockOHLC": 89, "xlStockHLC": 88,
             "xlRadar": -4151, "xlRadarMarkers": 81, "xlRadarFilled": 82, "xlWaterfall": 119, "xlHistogram": 118, "xlPareto": 122,
             "xlBoxwhisker": 121, "xlTreemap": 117, "xlSunburst": 120, "xlFunnel": 123, "xlPieOfPie": 68, "xlBarOfPie": 71,
             "xlStockVHLC": 90, "xlStockVOHLC": 91}
_TYPE_NAME = {0: "xlLine", 1: "xlLineMarkers", 2: "xlColumnClustered", 3: "xlColumnStacked", 4: "xlBarClustered", 5: "xlXYScatter", 6: "xlArea",
              7: "xlPie", 8: "xlDoughnut", 9: "xlStockOHLC", 10: "xlRadar", 11: "xlRadarMarkers", 12: "xlRadarFilled", 13: "xlWaterfall",
              14: "xlHistogram", 15: "xlPareto", 16: "xlBoxwhisker", 17: "xlTreemap", 18: "xlSunburst", 19: "xlFunnel",
              45: "xlStockHLC", 46: "xlPieOfPie", 47: "xlBarOfPie", 48: "xlStockVHLC", 49: "xlStockVOHLC"}


def _chart_type(name: str) -> int:
    from ..ui import _XL_TO_TYPE

    if name not in _XL_CHART:
        raise ToolError(f"Unsupported chart type {name}; available: {', '.join(_XL_CHART)}")
    return _XL_TO_TYPE[_XL_CHART[name]]


def _chart_desc(rec, c):
    return {"sheet": rec["name"], "name": c["name"], "chart_type": _TYPE_NAME.get(c["type"]), "left": c["left"], "top": c["top"],
            "width": c["width"], "height": c["height"], "title": c["title"] if c["has_title"] else None, "legend": c["has_legend"],
            "interactive": c["interactive"], "chart_window": c["floating"],
            "y_axis": None if c["y_auto"] else [c["y_min"], c["y_max"]],
            "series": [{"name": s["name"], "source": "python" if s["python_data"] else f"=SERIES({s['name']},{s['x_values']},{s['values']})",
                        "points": s["count"] if s["python_data"] else None, "axis": "secondary" if s["axis_group"] == 1 else "primary"}
                       for s in c["series"]]}


def yc_list_charts(sheet: Optional[str] = None):
    recs = [_sheet_rec(sheet)] if sheet else _info()["sheets"]
    return [_chart_desc(rec, c) for rec in recs for c in rq("obj.list", sheet=rec["id"])["charts"]]


def yc_add_chart(chart_type: str, sheet: Optional[str] = None, name=None, source=None, plot_by: str = "columns", title=None, cover_range=None,
                 left=None, top=None, width=None, height=None, interactive: bool = False):
    rec = _sheet_rec(sheet)
    p = {"type": _chart_type(chart_type), **_geom_from(rec, cover_range, left, top, width, height), "interactive": interactive}
    p.setdefault("left", 0.0)
    p.setdefault("top", 0.0)
    p.setdefault("width", 360.0)
    p.setdefault("height", 216.0)
    if name:
        p["name"] = name
    if title is not None:
        p["title"] = title
    if source:
        p["source"] = source
        p["plot_by"] = 1 if plot_by == "rows" else 2
    c = rq("chart.add", sheet=rec["id"], **p)
    return _chart_desc(rec, c)


def yc_modify_chart(name: str, sheet: Optional[str] = None, delete: bool = False, set: Optional[dict] = None, series: Optional[list] = None,  # noqa: A002
                    float: Optional[bool] = None):  # noqa: A002
    rec = _sheet_rec(sheet)
    if delete:
        rq("chart.delete", sheet=rec["id"], name=name)
        return f"Deleted chart {name}"
    props = {}
    for k, v in (set or {}).items():
        kl = k.lower()
        if kl in ("chart_type", "type"):
            props["type"] = _chart_type(v)
        elif kl in ("title",):
            props["title"] = v
        elif kl in ("has_title", "has_legend", "interactive", "left", "top", "width", "height", "name", "y_min", "y_max", "x_min", "x_max", "y_auto", "x_auto"):
            props[kl] = v
        elif kl == "legend":
            props["has_legend"] = bool(v)
        elif kl == "source":
            rq("chart.set_source", sheet=rec["id"], name=name, source=v)
        else:
            raise ToolError(f"Unknown chart property: {k}")
    if float is not None:
        props["floating"] = float
    c = rq("chart.set", sheet=rec["id"], name=name, props=props) if props else rq("chart.get", sheet=rec["id"], name=name)
    cname = c["name"]
    for s in series or []:
        op = s.get("op", "set")
        idx = s.get("index")
        idx = idx - 1 if isinstance(idx, int) and idx >= 1 else -1
        if op == "delete":
            rq("chart.series_delete", sheet=rec["id"], name=cname, index=idx)
            continue
        if "y" in s or "data" in s:
            rq("chart.series_data", sheet=rec["id"], name=cname, index=idx, y=s.get("y", s.get("data")), x=s.get("x"),
               append=bool(s.get("append")), **({"series_name": s["name"]} if "name" in s else {}))
            continue
        if s.get("topic"):
            raise ToolError("Series.BindTopic is not available yet: use Series.AppendData in the stream callback")
        sp = {k: s[k] for k in ("name", "values", "x_values") if k in s}
        if "axis" in s:
            sp["axis_group"] = 1 if s["axis"] in ("secondary", 2) else 0
        if "color" in s:
            sp["color"] = _color(s["color"])
        rq("chart.series_set", sheet=rec["id"], name=cname, index=idx, props=sp)
    return _chart_desc(rec, rq("chart.get", sheet=rec["id"], name=cname))


def yc_manage_panels(action: str, name: Optional[str] = None, caption=None, dock=None, controls: Optional[list] = None, keep_values: bool = True):
    if action == "list":
        return rq("panel.list")
    if not name:
        raise ToolError("name is required")
    if action == "delete":
        return {"deleted": rq("panel.delete", name=name)}
    p = {"name": name, "keep_values": keep_values}
    if caption is not None:
        p["caption"] = caption
    if dock:
        p["dock"] = dock
    if action == "add" and caption is None:
        p["caption"] = name
    if controls is not None:
        p["controls"] = [{**c, "type": str(c.get("type", "label")).lower().removeprefix("yc")} for c in controls]
    elif action == "rebuild":
        p["controls"] = []
    return rq("panel.set", panel=p)


def yc_manage_layout(action: str, sheet: Optional[str] = None, container: Optional[str] = None, item: Optional[str] = None,
                     position: str = "tab", ratio: float = 0.3, rect: Optional[dict] = None, layout_name: Optional[str] = None):
    rect = rect or {}
    if action == "describe":
        info = _info()
        views = []
        for s in info["sheets"]:
            v = _safe(lambda s=s: rq("view.get", sheet=s["id"]), {}) or {}
            views.append({"sheet": s["name"], "floating": v.get("floating"), "scroll": [v.get("scroll_row"), v.get("scroll_col")], "zoom": v.get("zoom")})
        return {"sheets": views, "containers": rq("container.list"), "panels": [p["name"] for p in rq("panel.list")],
                "saved_layouts": _excel().Application.Layout.Names}
    if action in ("float_sheet", "dock_sheet"):
        rec = _sheet_rec(sheet)
        rq("view.float_sheet", id=rec["id"], floating=action == "float_sheet")
        if action == "float_sheet" and rect:
            rq("window.float", item={"kind": "sheet", "name": rec["name"]}, **{k: float(v) for k, v in rect.items()})
        return f"{action}: {rec['name']}"
    if action == "add_container":
        return rq("container.add", name=container or "", **{k: float(v) for k, v in rect.items()})
    if action == "remove_container":
        return {"removed": rq("container.delete", name=container)}
    if action == "dock_into":
        kind, _, nm = (item or "").partition(":")
        if not nm:
            kind, nm = "sheet", item
        return {"docked": rq("container.dock", name=container, item={"kind": kind, "name": nm}, position=position, ratio=ratio)}
    if action == "save_layout":
        _excel().Application.Layout.Save(layout_name or "default")
        return f"Saved layout \"{layout_name or 'default'}\" (stored in the workbook, kept when saved)"
    if action == "restore_layout":
        _excel().Application.Layout.Restore(layout_name or "default")
        return f"Restored layout \"{layout_name or 'default'}\""
    raise ToolError(f"Unknown action: {action}")


def yc_scroll_view(sheet: Optional[str] = None, scroll_row: Optional[int] = None, scroll_column: Optional[int] = None, goto: Optional[str] = None,
                   select: Optional[str] = None, zoom: Optional[int] = None, freeze: Optional[str] = None):
    rec = _sheet_rec(sheet)
    p: dict = {"sheet": rec["id"], "activate_sheet": rec["id"]}
    if scroll_row:
        p["scroll_row"] = scroll_row - 1
    if scroll_column:
        p["scroll_col"] = scroll_column - 1
    if zoom:
        p["zoom"] = zoom / 100.0
    if freeze is not None:
        if freeze in ("", "none"):
            p["freeze"] = [0, 0]
        else:
            r0, c0, _, _ = _split(freeze, rec["name"])[1]
            p["freeze"] = [r0, c0]
            p.setdefault("scroll_row", 0)
            p.setdefault("scroll_col", 0)
    sel = select or goto
    if sel:
        _, area = _split(sel, rec["name"])
        p["select"] = [list(area)]
        p["active"] = [area[0], area[1]]
        if goto:
            p["scroll_row"], p["scroll_col"] = area[0], area[1]
    if len(p) > 2:
        rq("view.set", **p)
    rq("sync", wait="ui")
    v = rq("view.get", sheet=rec["id"])
    return {"sheet": rec["name"], "visible_range": _addr(v["visible"]) if v.get("visible") else None, "selection": [_addr(a) for a in v.get("areas", [])],
            "active_cell": _a1(*v["active"]) if v.get("active") else None, "zoom": round(v.get("zoom", 1) * 100), "freeze": v.get("freeze")}


def yc_execute_command(command: str, query: bool = False):
    if query:
        return rq("ui.command_state", id=command)
    ok = rq("ui.command", id=command)
    if not ok:
        raise ToolError(f"Command {command} not found (command ids: see references/ui-coverage.md)")
    return f"Ran {command}"


# ---------------------------------------------------------------------------
# 5. Python
# ---------------------------------------------------------------------------
def yc_python_status(include: Optional[list] = None):
    st = rq("python.status")
    inc = set(include or ["host", "modules", "macros", "udfs", "events", "controls", "errors"])
    out = {}
    if "host" in inc:
        out["host"] = {k: st.get(k) for k in ("running", "ready", "workbook_open_done", "python_version", "python_exe", "app_dir", "last_error")}
        out["host"]["mode"] = "managed" if st.get("running") else "not connected"
        if st.get("code_disabled"):
            # the Trust Center kept the project's code from running (the Message Bar): a person clicks Enable Content
            out["host"]["mode"] = "disabled by the Trust Center"
        out["host"]["code_disabled"] = bool(st.get("code_disabled"))
    for k in ("modules", "macros", "udfs"):
        if k in inc:
            out[k] = st.get(k, [])
    if "events" in inc:
        # the handlers YCGrid runs; a Workbook_* / Worksheet_* name it never fires (a typo, or an Excel event YCGrid
        # doesn't have) is listed apart, so a handler shown here is one that runs
        from ..host import FIRED_EVENTS

        names = st.get("events", [])
        out["events"] = [n for n in names if n in FIRED_EVENTS]
        never = [n for n in names if n not in FIRED_EVENTS]
        if never:
            out["events_never_fired"] = never
    if "errors" in inc:
        out["load_errors"] = st.get("load_errors", [])
        out["recent_errors"] = st.get("recent_errors", [])
        out["missing_packages"] = st.get("missing_packages", [])
    if "controls" in inc:
        out["controls"] = [{"control": f"{c['sheet']}.{c['name']}", "handler": c.get("handler"), "exists": c.get("handler_exists")}
                           for c in yc_list_controls(check_handlers=True)]
    if "feeds" in inc or "streams" in inc:
        out["feeds"] = "The status of data feed processes ([[python.feeds]]) is not available yet"
    return out


def _compile_check(app_dir: str) -> list:
    errs = []
    for root, dirs, files in os.walk(app_dir):
        dirs[:] = [d for d in dirs if d not in ("runtime", ".ycgrid", "__pycache__", ".git", ".venv", "dist", "typings", "examples")]
        for f in files:
            if f.endswith(".py"):
                path = os.path.join(root, f)
                try:
                    with open(path, encoding="utf-8") as fh:
                        compile(fh.read(), path, "exec")
                except SyntaxError as e:
                    errs.append({"file": os.path.relpath(path, app_dir), "line": e.lineno, "error": f"SyntaxError: {e.msg}"})
                except (OSError, UnicodeDecodeError) as e:
                    errs.append({"file": os.path.relpath(path, app_dir), "line": 0, "error": str(e)})
    return errs


def yc_reload_python(mode: str = "auto", restart_feeds: bool = False, force: bool = False, timeout_ms: int = 30000):
    st = rq("python.status")
    app_dir = st.get("app_dir") or ""
    if app_dir:
        errs = _compile_check(app_dir)
        if errs and not force:
            return {"reloaded": False, "reason": "Syntax error, not reloaded (the old code keeps running)", "errors": errs}
    r = _ensure().request("python.reload", {"timeout_ms": timeout_ms}, timeout=timeout_ms / 1000 + 10)
    st2 = _safe(lambda: rq("python.status"), {}) or {}
    return {"reloaded": r.get("started", False), "ready": r.get("ready"), "workbook_open_done": r.get("workbook_open_done"),
            "errors": r.get("errors", []), "load_errors": st2.get("load_errors", []), "macros": r.get("macros", []),
            "console": r.get("console", [])[-40:]}


def _job(timeout_ms: int, **params):
    return _ensure().request("host.job", {**params, "timeout_ms": timeout_ms}, timeout=timeout_ms / 1000 + 10)


def yc_run_macro(name: str, args: Optional[list] = None, dialogs: str = "auto", timeout_ms: int = 30000):
    r = _job(timeout_ms, kind="macro", name=name, args=args or [], dialogs=dialogs)
    return {k: r.get(k) for k in ("ok", "result", "error", "traceback", "ms", "console", "timeout") if r.get(k) not in (None, "")}


def yc_exec_python(code: str, target: str = "controller", dialogs: str = "auto", timeout_ms: int = 30000):
    if target == "host":
        r = _job(timeout_ms, kind="exec", code=code, dialogs=dialogs)
        return {k: r.get(k) for k in ("ok", "result", "error", "traceback", "ms", "console", "timeout") if r.get(k) not in (None, "")}
    import ast
    import traceback as tb

    excel = _excel()
    ns = {k: getattr(excel, k) for k in excel.__all__}
    buf = io.StringIO()
    t0 = time.perf_counter()
    result = None
    try:
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            tree = ast.parse(code, "<yc_exec_python>", "exec")
            last = None
            if tree.body and isinstance(tree.body[-1], ast.Expr):
                last = ast.Expression(tree.body.pop().value)
            exec(compile(tree, "<yc_exec_python>", "exec"), ns)
            if last is not None:
                result = eval(compile(last, "<yc_exec_python>", "eval"), ns)
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "error": f"{type(e).__name__}: {e}", "traceback": tb.format_exc()[-4000:], "output": buf.getvalue()[-8000:]}
    out = {"ok": True, "ms": round((time.perf_counter() - t0) * 1000, 2), "output": buf.getvalue()[-8000:]}
    if result is not None:
        out["result"] = result if isinstance(result, (int, float, str, bool, list, dict, Decimal)) else repr(result)
    return out


def yc_evaluate_formula(formula: str, sheet: Optional[str] = None, as_cell: Optional[str] = None, max_cells: int = 200):
    rec = _sheet_rec(sheet)
    p = {"formula": formula if formula.startswith("=") else "=" + formula, "sheet": rec["id"]}
    v = rq("eval", **p)
    if isinstance(v, list):
        flat = sum(len(r) if isinstance(r, list) else 1 for r in v)
        if flat > max_cells:
            return {"type": "array", "truncated": True, "rows": v[: max(1, max_cells // max(1, len(v[0]) if v and isinstance(v[0], list) else 1))]}
        return {"type": "array", "value": v}
    note = {"note": "as_cell is not available yet: formulas are evaluated relative to A1"} if as_cell else {}
    if isinstance(v, XlError):
        return {"type": "error", "value": v.Text, **note}
    return {"type": type(v).__name__ if v is not None else "empty", "value": v, **note}


def yc_read_console(since_cursor: Optional[int] = None, min_level: str = "info", sources: Optional[list] = None, max_lines: int = 200):
    r = rq("console.read", since=since_cursor if since_cursor is not None else -1, max=max_lines, min_kind=1 if min_level == "error" else 0)
    lines = r["lines"]
    if sources:
        want = set(sources)
        lines = [ln for ln in lines if ("system" in want and ln["kind"] == "system") or ("python" in want and ln["kind"] in ("output", "error", "input"))]
    text = "\n".join(f"{ln['n']:>6} {'!' if ln['kind'] == 'error' else ' '} {ln['text']}" for ln in lines)
    return f"{text}\n\nnext_cursor={r['next_cursor']}" + ("(older lines were discarded)" if r.get("truncated") else "")


def _runtime_app_dir() -> str:
    try:
        return rq("python.status").get("app_dir") or os.getcwd()
    except Exception:  # noqa: BLE001
        return os.getcwd()


def yc_python_env(action: str = "status", version: Optional[str] = None):
    import importlib.metadata as md
    import sys

    from .. import env

    app_dir = _runtime_app_dir()
    if action != "status":
        raise ToolError("Upgrading and rolling back the Python version is not available yet (PLAN §13 (e)); it is fixed at 3.12 for now")
    pkgs = sorted({f"{d.metadata['Name']}=={d.version}" for d in md.distributions() if d.metadata.get("Name")})
    req = env._requirement_names(app_dir)
    lock = []
    lp = os.path.join(app_dir, "requirements.lock")
    if os.path.isfile(lp):
        with open(lp, encoding="utf-8") as f:
            lock = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
    return {"python": sys.version.split()[0], "executable": sys.executable, "prefix": sys.prefix, "app_dir": app_dir,
            "requirements": req, "lock": lock, "installed": pkgs, "missing": env.find_missing(app_dir)}


def yc_install_packages(packages: Optional[list] = None, offline: bool = False, reload: bool = True):
    from .. import env

    app_dir = _runtime_app_dir()
    pkgs = packages or env.find_missing(app_dir)
    if not pkgs:
        return "No missing packages"
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        if offline:
            os.environ["UV_OFFLINE"] = "1"
        ok = env.install(app_dir, pkgs)
    out = {"installed": ok, "packages": pkgs, "log": buf.getvalue()[-6000:]}
    if ok and reload:
        out["reload"] = _safe(lambda: yc_reload_python(), {})
    return out


# ---------------------------------------------------------------------------
# 6. UI
# ---------------------------------------------------------------------------
_KEYMAP = {"down": "DownArrow", "up": "UpArrow", "left": "LeftArrow", "right": "RightArrow", "esc": "Escape", "escape": "Escape", "del": "Delete",
           "delete": "Delete", "enter": "Enter", "return": "Enter", "tab": "Tab", "space": "Space", "backspace": "Backspace", "home": "Home",
           "end": "End", "pgup": "PageUp", "pageup": "PageUp", "pgdn": "PageDown", "pagedown": "PageDown", "insert": "Insert", "`": "GraveAccent"}


def _key_token(combo: str) -> str:
    parts = [p.strip() for p in combo.split("+") if p.strip()]
    mods = ""
    key = ""
    for p in parts:
        pl = p.lower()
        if pl in ("ctrl", "control"):
            mods += "c-"
        elif pl == "shift":
            mods += "s-"
        elif pl == "alt":
            mods += "a-"
        else:
            key = _KEYMAP.get(pl, p.upper() if len(p) == 1 else p[0].upper() + p[1:])
    return f"k:{mods}{key}"


def yc_ui_input(actions: list, sheet: Optional[str] = None, dialogs: str = "auto", timeout_ms: int = 15000):
    if sheet:
        rec = _sheet_rec(sheet)
        rq("view.set", activate_sheet=rec["id"])
    toks = []
    for a in actions:
        t = a["type"]
        if t == "select":
            toks.append("g:" + a["address"])
        elif t == "type":
            toks.append("t:" + a.get("text", "").replace("|", "¦"))
            commit = a.get("commit", "enter")
            if commit != "none":
                toks.append("k:" + ("Enter" if commit == "enter" else "Tab"))
        elif t == "keys":
            toks.append(_key_token(a["keys"]))
        elif t == "click":
            toks.append("A:" + a["address"])
        elif t == "double_click":
            toks.append("Q:" + a["address"])
        elif t == "right_click":
            toks.append("R:" + a["address"])
        elif t == "scroll_to":
            toks.append("g:" + a["address"])
        elif t == "design_mode":
            rq("ui.design_mode_set", on=bool(a.get("on", True)))
            continue
        elif t == "wait":
            toks.append(f"s:{max(1, int(a.get('ms', 100)))}")
        toks.append("w:2")
    r = _ensure().request("ui.input", {"script": "|".join(toks), "timeout_ms": timeout_ms, "dialogs": dialogs}, timeout=timeout_ms / 1000 + 10)
    time.sleep(0.05)
    st = yc_scroll_view(sheet)
    return {"done": r.get("done"), "view": st, "console_tail": yc_read_console(max_lines=10)}


def yc_screenshot(target: str = "grid", address: Optional[str] = None, sheet: Optional[str] = None, include_headers: bool = True, max_width: int = 1400):
    p = {"target": target, "max_width": max_width, "include_headers": include_headers}
    if sheet or address:
        rec, area = _split(address, sheet) if address else (_sheet_rec(sheet), None)
        p["sheet"] = rec["id"]
        if area:
            p["address"] = _addr(area)
    elif target == "range":
        raise ToolError("target=range needs address")
    fd, path = tempfile.mkstemp(suffix=".png", prefix="yc_shot_")
    os.close(fd)
    try:
        p["path"] = path
        r = _ensure().request("ui.screenshot", p, timeout=20)
        with open(path, "rb") as f:
            data = f.read()
    finally:
        with contextlib.suppress(OSError):
            os.remove(path)
    return Image(data, f"{r.get('info', '')} ({r.get('width')}x{r.get('height')}; floating windows are not in the main window screenshot)")


def yc_calculate(scope: str = "dirty", sheet: Optional[str] = None):
    if scope != "stats_only":
        rq("calc.run", full=scope in ("full", "rebuild"))
        rq("sync", wait="engine")
    return rq("calc.get")


def yc_topics(action: str, filter: Optional[str] = None, address: Optional[str] = None, topics: Optional[list] = None,  # noqa: A002
              values: Optional[list] = None, max_results: int = 200):
    if action == "push":
        if not topics or values is None or len(topics) != len(values):
            raise ToolError("push needs topics and values of the same length")
        rq("stream.push", items=[[t, v] for t, v in zip(topics, values)])
        return f"Pushed {len(topics)} test values"
    if action == "lookup":
        if not address:
            raise ToolError("lookup needs address")
        f = yc_read_range(address, fields=["formula"], output="json")
        return {"formulas": f["rows"], "note": "topic bindings are expressed as formulas (e.g. =QUOTE(\"2330\",\"Bid\"))"}
    raise ToolError("The topic list and throughput statistics are not available yet; use action=push to test formulas and events")


def yc_ui_state():
    return rq("ui.state")


def _host_ready(timeout: float = 30.0):
    st = rq("ui.state")
    if st["python"]["ready"]:
        return
    rq("host.eval", code="pass")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if rq("ui.state")["python"]["ready"]:
            return
        time.sleep(0.2)
    raise ToolError("The Python host didn't start; read yc_read_console for the reason")


def yc_bound_tables(action: str = "list", table: Optional[str] = None, max_rows: int = 200):
    from .. import data

    if action == "list":
        return rq("bind.list")
    if action != "read":
        raise ToolError("action: list or read")
    if not table:
        raise ToolError("read needs table")
    cols = data.read(table, pandas=False)
    names = list(cols)
    n = len(cols[names[0]]) if names else 0
    rows = [[_plain(cols[c][i]) for c in names] for i in range(min(n, max_rows))]
    return {"columns": names, "rows": rows, "total_rows": n, "truncated": n > max_rows}


def _plain(v):
    """A JSON-friendly cell value: numpy scalars to Python, NaN / NaT to None, dates to ISO text."""
    if hasattr(v, "dtype") and getattr(v.dtype, "kind", "") == "M":
        s = str(v)
        return None if s == "NaT" else s
    if hasattr(v, "item"):
        try:
            v = v.item()
        except (ValueError, TypeError):
            return str(v)
    if isinstance(v, float) and v != v:
        return None
    if hasattr(v, "isoformat"):
        return v.isoformat()
    return v


def yc_replay(action: str = "status", path: Optional[str] = None, tables: Optional[list] = None, speed: float = 1.0,
              start: float = 0.0, seconds: Optional[float] = None, loop: bool = False):
    if action == "status":
        return rq("replay.status")
    if action == "record":
        p = {"path": os.path.abspath(path) if path else ""}
        if tables:
            p["tables"] = tables
        return {"recording_to": rq("replay.record", **p)}
    if action == "stop_recording":
        rq("replay.stop_record")
    elif action == "play":
        if not path:
            raise ToolError("play needs path (a .yctape)")
        rq("replay.play", path=os.path.abspath(path), speed=float(speed), start=float(start), loop=bool(loop))
    elif action == "pause":
        rq("replay.pause", pause=True)
    elif action == "resume":
        rq("replay.pause", pause=False)
    elif action == "seek":
        if seconds is None:
            raise ToolError("seek needs seconds")
        rq("replay.seek", seconds=float(seconds))
    elif action == "speed":
        rq("replay.speed", speed=float(speed))
    elif action == "live":
        rq("replay.live")
    else:
        raise ToolError("action: status, record, stop_recording, play, pause, resume, seek, speed or live")
    return rq("replay.status")


def yc_workbook_file(action: str, path: Optional[str] = None, format: Optional[str] = None, discard_changes: bool = False,  # noqa: A002
                     sheet: Optional[str] = None, all_sheets: bool = False):
    if action == "export_pdf":
        if not path:
            raise ToolError("export_pdf needs path")
        excel = _excel()
        if all_sheets:
            out = excel.ActiveWorkbook.ExportAsFixedFormat(excel.xlTypePDF, path)
        else:
            ws = excel.Worksheets(sheet) if sheet else excel.ActiveSheet
            out = ws.ExportAsFixedFormat(excel.xlTypePDF, path)
        return f"Exported PDF: {out}"
    if action == "save":
        ok = rq("wb.save")
        return "Saved" if ok else "Saving failed or was cancelled"
    if action == "save_as":
        if not path:
            raise ToolError("save_as needs path")
        if format == "csv" and not path.lower().endswith(".csv"):
            path += ".csv"
        if path.lower().endswith(".csv"):
            # like Excel's Save As CSV: the active sheet's values; the workbook itself stays as it was (no file name change)
            ok = rq("wb.save", path=path)
            return f"Saved the active sheet as CSV: {path}" if ok else "Saving failed"
        ok = rq("wb.save", path=path)
        return f"Saved as {path}" if ok else "Saving failed"
    if action == "open":
        if not path:
            raise ToolError("open needs path")
        rq("wb.open", path=path, discard_changes=discard_changes)
        return f"Opened {path}"
    if action == "new":
        rq("wb.new", discard_changes=discard_changes)
        return "Created a new workbook"
    raise ToolError(f"Unknown action: {action}")


def yc_app_options(set: Optional[dict] = None):  # noqa: A002
    excel = _excel()
    if set:
        for k, v in set.items():
            if k == "calculation":
                rq("calc.set", auto=v == "automatic")
            elif k == "design_mode":
                rq("ui.design_mode_set", on=bool(v))
            elif k == "status_bar":
                excel.Application.StatusBar = v if v is not None else False
            elif k == "zoom":
                rq("view.set", zoom=v / 100.0)
            elif k == "display_gridlines":
                excel.ActiveWindow.DisplayGridlines = bool(v)
            elif k == "display_formulas":
                excel.ActiveWindow.DisplayFormulas = bool(v)
            elif k == "enable_events":
                _job(5000, kind="exec", code=f"Application.EnableEvents = {bool(v)}", dialogs="auto")
            else:
                raise ToolError(f"Option {k} is not available yet")
    v = _safe(lambda: rq("view.get"), {}) or {}
    return {"calculation": "automatic" if (_safe(lambda: rq("calc.get"), {}) or {}).get("auto", True) else "manual",
            "design_mode": _safe(lambda: rq("ui.design_mode_get")["on"], False), "zoom": round(v.get("zoom", 1) * 100),
            "display_gridlines": v.get("gridlines"), "display_formulas": v.get("formulas"), "status_bar": _safe(lambda: rq("ui.status_get"))}


def yc_workbook_store(action: str, key: Optional[str] = None, value: Any = None, scope: str = "workbook", sheet: Optional[str] = None):
    excel = _excel()
    if scope == "document_properties":
        props = excel.ThisWorkbook.CustomDocumentProperties
        if action == "list":
            return {p.Name: p.Value for p in props}
        if action == "get":
            return props(key).Value
        if action == "set":
            try:
                props(key).Value = value
            except Exception:  # noqa: BLE001
                props.Add(Name=key, LinkToContent=False, Type=4 if isinstance(value, str) else 1, Value=value)
            return "OK"
        props(key).Delete()
        return "OK"
    store = excel.Worksheets(_sheet_rec(sheet)["name"]).Store if scope == "sheet" else excel.ThisWorkbook.Store
    if action == "list":
        return {k: store.get(k) for k in store.keys()}
    if not key:
        raise ToolError("key is required")
    if action == "get":
        return {"key": key, "value": store.get(key), "exists": key in store}
    if action == "set":
        store[key] = value
        return f"Wrote {key} (the workbook now has unsaved changes)"
    if action == "delete":
        del store[key]
        return f"Deleted {key}"
    raise ToolError(f"Unknown action: {action}")


def yc_plot_input(figure: str, actions: list, plot: int = 0):
    figs = rq("plot.figures")
    f = next((x for x in figs if x.get("key") == str(figure) or str(x.get("id")) == str(figure)), None)
    if f is None:
        from .._conn import YcError

        raise YcError(f"No figure {figure!r} (figures: {', '.join(x.get('key') or str(x.get('id')) for x in figs)})", 1004, "E_NOT_FOUND")
    done = []
    for a in actions:
        p = {"fig": f["id"], "ax": int(a.get("plot", plot))}
        p.update({k: v for k, v in a.items() if k != "plot"})
        rq("plot.input", **p)
        done.append(a.get("action"))
    return {"figure": f["id"], "key": f.get("key", ""), "done": done}


def yc_panel_input(panel: str, control: str, action: str = "click", value=None):
    p = {"panel": panel, "control": control, "action": action}
    if value is not None:
        p["value"] = value
    rq("panel.input", **p)
    return {"panel": panel, "control": control, "action": action}


def yc_api_lookup(query: str, kind: str = "any", max_results: int = 10):
    from . import apiref

    return apiref.lookup(query, kind, max_results)


def yc_extract_macros(path: str, include_controls: bool = True, modules: Optional[list] = None, max_chars_per_module: int = 50000):
    from . import macro_extract

    return macro_extract.extract(path, include_controls, modules, max_chars_per_module)


HANDLERS = {n: f for n, f in globals().items() if n.startswith("yc_") and callable(f)}
