# SPDX-License-Identifier: Apache-2.0
"""YCGrid's Python host process (started by ycgrid.exe; PLAN §13).

Load order (the core of the development rules):
  1. main.py          = ThisWorkbook: Workbook_* events, startup settings, OnChange registration, panels
  2. sheets\\*.py      sheet modules (file name = CodeName): Worksheet_* events, control events
  3. modules\\*.py     standard modules: macros, @udf, @stream
  4. call Workbook_Open() of main.py
The first entry of sys.path is the project folder; lib\\ can be imported, and so can the folders [python] paths in
ycgrid.toml names; files starting with an underscore are not loaded automatically.
"""
from __future__ import annotations

import argparse
import builtins
import datetime as _dt
import importlib.util
import inspect
import io
import os
import sys
import threading
import time
import traceback
import warnings
from typing import Any, Callable, Optional

from . import _conn
from ._conn import Connection, YcError

_conn_obj: Optional[Connection] = None
_app_dir = ""
_modules: dict[str, Any] = {}       # "main", "sheets.Sheet1", "modules.Module1"
_sheet_modules: dict[str, Any] = {}  # CodeName → module
_code_names: dict[str, int] = {}     # CodeName → sheet id
_udfs: dict[str, dict] = {}
_streams: dict[str, type] = {}
_stream_objs: dict[str, Any] = {}
_onchange: list[dict] = []           # {id, sheet, areas, fn, sources}
_onchange_id = 0
_lock = threading.RLock()
_timers: list = []


# ---------------------------------------------------------------------------
# output: print and exceptions go to YCGrid's Immediate window console
# ---------------------------------------------------------------------------
class _ConsoleStream(io.TextIOBase):
    def __init__(self, kind: str):
        self.kind = kind
        self._buf = ""
        self._lock = threading.Lock()

    def writable(self):
        return True

    def write(self, s):
        with self._lock:
            self._buf += s
            if "\n" in self._buf:
                lines, self._buf = self._buf.rsplit("\n", 1)
                self._send(lines)
        return len(s)

    def flush(self):
        with self._lock:
            if self._buf:
                self._send(self._buf)
                self._buf = ""

    def _send(self, text):
        try:
            if _conn_obj is not None and not _conn_obj.closed:
                _conn_obj.notify("log", {"text": text, "kind": self.kind})
        except YcError:
            pass


def _show_warning(message, category, filename, lineno, file=None, line=None):
    # a warning (numpy's RuntimeWarning in a PY cell fed a column of zeros) is a message, not an error: on stderr it
    # went into the Error Log as an error (chaos seed 5111)
    text = warnings.formatwarning(message, category, filename, lineno, line).rstrip()
    try:
        _conn_obj.notify("log", {"text": text, "kind": "warning"})
    except (YcError, AttributeError):
        print(text, file=sys.__stderr__)


def _report(where: str, exc: BaseException) -> None:
    tb = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
    # only keep the stack frames of project files (frames inside the yc package are hidden)
    lines = [ln for ln in tb.splitlines() if "\\yc\\" not in ln and "/yc/" not in ln or "Error" in ln]
    text = f"[{where}] {type(exc).__name__}: {exc}\n" + "\n".join(lines[-12:])
    try:
        _conn_obj.notify("log", {"text": text, "kind": "error"})
        _conn_obj.notify("python.error", {"where": where, "message": f"{type(exc).__name__}: {exc}"})
    except YcError:
        print(text, file=sys.__stderr__)


# ---------------------------------------------------------------------------
# registration (called by the decorators and properties of yc.excel)
# ---------------------------------------------------------------------------
def register_udf(name: str, fn: Callable, volatile=False, accept_errors=False) -> None:
    _udfs[name.upper()] = {"fn": fn, "volatile": volatile, "accept_errors": accept_errors}
    if _conn_obj is not None and _loaded:
        _conn_obj.notify("udf.register", {"names": [name.upper()], "help": _udf_help([name.upper()])})


def _udf_help(names=None) -> dict:
    """Insert Function's view of the @udf functions: {NAME: {"args": "price, [qty]", "desc": "the docstring's first
    line"}} (optional arguments in brackets, as Excel lists them)."""
    out = {}
    for name in names or list(_udfs):
        info = _udfs.get(name)
        if info is None or name == "PY":
            continue
        fn = info["fn"]
        args = []
        try:
            for prm in inspect.signature(fn).parameters.values():
                if prm.kind == prm.VAR_POSITIONAL:
                    args.append(f"[{prm.name}1], ...")
                elif prm.kind in (prm.POSITIONAL_ONLY, prm.POSITIONAL_OR_KEYWORD):
                    args.append(prm.name if prm.default is prm.empty else f"[{prm.name}]")
        except (TypeError, ValueError):
            pass
        doc = (inspect.getdoc(fn) or "").strip().splitlines()
        try:
            src = inspect.getsource(fn)
        except (OSError, TypeError):
            src = ""
        # a function that reads its calling cell is called for each cell (the others once for equal arguments)
        per_cell = "ThisCell" in src or "Application.Caller" in src
        out[name] = {"args": ", ".join(args), "desc": doc[0].strip() if doc else "", "per_cell": per_cell}
    return out


def register_stream(name: str, cls: type) -> None:
    _streams[name.upper()] = cls


# ---- =PY(code, ref1, ref2, ...): Python in a cell ----
def _py_arg(a):
    """A referenced range as Python sees it: a table with a header row -> DataFrame; numbers -> numpy array;
    one cell -> its value."""
    from .excel import VArray

    if not isinstance(a, (list, VArray)):
        return a
    rows = [list(r) for r in a]
    if not rows:
        return None
    header = rows[0]
    try:
        import numpy as np
        import pandas as pd
    except ImportError:
        return rows
    if len(rows) > 1 and all(isinstance(h, str) and h for h in header):
        return pd.DataFrame(rows[1:], columns=header)
    if all(isinstance(v, (int, float)) and not isinstance(v, bool) for r in rows for v in r):
        arr = np.array(rows, dtype=float)
        return arr.ravel() if 1 in arr.shape else arr
    return rows


def _py_cells(v):
    """A Python value as cells: DataFrame (with its header), Series, numpy, lists and dicts spill; scalars stay one cell."""
    if v is None:
        return ""
    mod = type(v).__module__
    if mod.startswith("pandas"):
        if hasattr(v, "columns"):  # DataFrame
            df = v if type(v.index).__name__ == "RangeIndex" else v.reset_index()  # row labels become a column
            return [[str(c) for c in df.columns]] + df.astype(object).where(df.notna(), None).values.tolist()
        if hasattr(v, "to_frame"):  # Series
            return [[x] for x in v.astype(object).where(v.notna(), None).tolist()]
    if hasattr(v, "tolist") and hasattr(v, "ndim"):
        v = v.tolist()
        if not isinstance(v, list):
            return v
    if isinstance(v, dict):
        return [[k, val] for k, val in v.items()]
    if isinstance(v, (list, tuple)):
        if v and all(isinstance(r, (list, tuple)) for r in v):
            width = max(len(r) for r in v)
            return [list(r) + [None] * (width - len(r)) for r in v]
        return [[x] for x in v]  # a flat list is a column
    return v


def _py_formula(code="", *args):
    """=PY("df.describe()", A1:C200): the code runs in the Immediate window's namespace (what it defined is there);
    x is the first range, _1, _2... each range, args all of them. =PY() with no code yet (just inserted from the
    function list) shows nothing."""
    import ast

    if code is None or str(code).strip() == "":
        return ""

    _ensure_console_ns()
    conv = [_py_arg(a) for a in args]
    local = {"x": conv[0] if conv else None, "args": conv}
    local.update({f"_{i + 1}": a for i, a in enumerate(conv)})
    ns = dict(_console_ns)
    ns.update(local)
    code = str(code)
    tree = ast.parse(code, "<PY>", "exec")
    last = None
    if tree.body and isinstance(tree.body[-1], ast.Expr):
        last = ast.Expression(tree.body.pop().value)
    exec(compile(tree, "<PY>", "exec"), ns)
    result = eval(compile(last, "<PY>", "eval"), ns) if last is not None else None
    return _py_cells(result)


def sheet_by_code_name(code: str):
    from .excel import Worksheet

    sid = _code_names.get(code)
    if sid is None:
        _refresh_code_names()
        sid = _code_names.get(code)
    return Worksheet(sid) if sid is not None else None


def sheet_module_attr(ws, name: str):
    try:
        code = ws.CodeName
    except YcError:
        return None
    mod = _sheet_modules.get(code)
    return getattr(mod, name, None) if mod is not None else None


def set_onchange(rng, handler, sources=None, append=False) -> None:
    """Range.OnChange = handler (None = remove every handler of that range)."""
    from .constants import ycPython, ycUser

    global _onchange_id
    with _lock:
        if handler is None:
            _onchange[:] = [h for h in _onchange if not (h["sheet"] == rng._sheet and h["areas"] == rng._areas)]
        else:
            if isinstance(handler, str):
                handler = _resolve_proc(handler)
            _onchange_id += 1
            _onchange.append({"id": _onchange_id, "sheet": rng._sheet, "areas": list(rng._areas), "fn": handler,
                              "sources": sources if sources is not None else (ycUser | ycPython)})
    _send_sources()


def _send_sources() -> None:
    """Tell YCGrid which changes to report: every source a per-cell OnChange asks for, and calculation / live data
    while a module defines Worksheet_LiveChange or main.py Workbook_SheetLiveChange."""
    from .constants import ycCalc, ycLive

    with _lock:
        mask = 0
        for h in _onchange:
            mask |= h["sources"]
    if any(hasattr(m, "Worksheet_LiveChange") for m in _sheet_modules.values()) or hasattr(_modules.get("main"), "Workbook_SheetLiveChange"):
        mask |= ycCalc | ycLive
    if _conn_obj is not None:
        _conn_obj.notify("events.sources", {"mask": mask})


def onchange_handlers(rng):
    with _lock:
        return [h["fn"] for h in _onchange if h["sheet"] == rng._sheet and h["areas"] == rng._areas]


def _resolve_proc(name: str) -> Callable:
    """"Module1.Foo", "Sheet1.Foo", "Foo" -> callable."""
    if callable(name):
        return name
    if "." in name:
        mod_name, fn_name = name.rsplit(".", 1)
        for key in (f"modules.{mod_name}", f"sheets.{mod_name}", mod_name):
            mod = _modules.get(key)
            if mod is not None and hasattr(mod, fn_name):
                return getattr(mod, fn_name)
        raise YcError(f"Macro not found: {name}", 1004, "E_NOT_FOUND")
    for mod in _modules.values():
        if hasattr(mod, name) and inspect.isfunction(getattr(mod, name)):
            return getattr(mod, name)
    raise YcError(f"Macro not found: {name}", 1004, "E_NOT_FOUND")


def run_macro(name, *args):
    return _resolve_proc(name)(*args)


def schedule(when, procedure, enabled=True) -> None:
    if isinstance(when, _dt.timedelta):
        when = _dt.datetime.now() + when
    with _lock:
        if not enabled:
            _timers[:] = [t for t in _timers if not (t[0] == when and t[1] == procedure)]
            return
        _timers.append((when, procedure))


def _timer_loop():
    while True:
        time.sleep(0.05)
        now = _dt.datetime.now()
        due = []
        with _lock:
            for t in list(_timers):
                if t[0] <= now:
                    due.append(t)
                    _timers.remove(t)
        for when, proc in due:
            _conn_obj.notifications.put(("__ontime__", {"proc": proc}))


# ---------------------------------------------------------------------------
# loading the project
# ---------------------------------------------------------------------------
_loaded = False


def _refresh_code_names():
    try:
        sheets = _conn_obj.request("wb.info")["sheets"]
    except YcError:
        return
    _code_names.clear()
    from .excel import _SheetMe

    for s in sheets:
        _code_names[s["code_name"]] = s["id"]
        setattr(builtins, s["code_name"], _SheetMe(s["id"], s["code_name"]))


_literals: dict = {}  # path -> {name: the source of its value} for `NAME = <immutable literal>` at the top level


def _immutable(v) -> bool:
    if isinstance(v, (tuple, frozenset)):
        return all(_immutable(x) for x in v)
    return v is None or isinstance(v, (bool, int, float, complex, str, bytes))


def _literal_constants(tree) -> dict:
    """{name: ast dump of the value} of the file's top-level assignments of an immutable literal (a number, text, a
    bool, None, a tuple of those): the constants a hot reload may change. Dicts, lists and calls are state, not
    constants, and are never re-run."""
    import ast

    out = {}
    for n in tree.body:
        names = _assigned_names(n)
        if len(names) != 1:
            continue
        try:
            v = ast.literal_eval(n.value)
        except (ValueError, TypeError, SyntaxError, MemoryError, RecursionError):
            continue
        if _immutable(v):
            out[names[0]] = ast.dump(n.value)
    return out


def _remember_literals(path: str) -> None:
    import ast

    try:
        with open(path, encoding="utf-8") as f:
            _literals[path] = _literal_constants(ast.parse(f.read(), path))
    except (OSError, SyntaxError, ValueError):
        _literals.pop(path, None)


def _load_file(mod_name: str, path: str, sheet_id: Optional[int] = None):
    from . import excel

    spec = importlib.util.spec_from_file_location(mod_name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[mod_name] = mod
    prev = getattr(excel._sheet_context, "sheet", None)
    excel._sheet_context.sheet = sheet_id
    try:
        spec.loader.exec_module(mod)
    finally:
        excel._sheet_context.sheet = prev
    _modules[mod_name] = mod
    _sources[path] = (mod_name, _mtime(path), sheet_id)
    _remember_literals(path)
    return mod


# ---- hot reload: saving a file swaps its functions and classes in place; state stays ----
_sources: dict = {}  # path -> (module name, mtime when loaded, sheet id)


def _mtime(path: str) -> float:
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0.0


def _patch_function(old, new) -> bool:
    """Give the old function object the new code, so every reference to it (callbacks, threads, registrations) runs
    the new code. False when it can't be patched (a different closure)."""
    if not (inspect.isfunction(old) and inspect.isfunction(new)):
        return False
    if old.__code__ is new.__code__:  # the same decorator wrapper: its inner function is what changed
        inner_old, inner_new = getattr(old, "__wrapped__", None), getattr(new, "__wrapped__", None)
        return inner_old is not None and inner_new is not None and _patch_function(inner_old, inner_new)
    try:
        old.__code__ = new.__code__
    except ValueError:  # free variables differ
        return False
    old.__defaults__, old.__kwdefaults__, old.__doc__ = new.__defaults__, new.__kwdefaults__, new.__doc__
    old.__dict__.update({k: v for k, v in new.__dict__.items() if k != "__wrapped__"})
    return True


class NeedsRestart(Exception):
    """A hot reload can't be done in place: restart the host instead (Workbook_HostShutdown, then Workbook_Open).
    error=False: an expected restart, reported as information rather than an error."""

    def __init__(self, msg: str, error: bool = True):
        super().__init__(msg)
        self.error = error


def _live_instances(cls) -> int:
    """How many objects of this class (or a subclass) exist now."""
    import gc

    n = 0
    for o in gc.get_objects():
        try:
            if isinstance(o, cls):
                n += 1
        except Exception:  # noqa: BLE001 - objects whose __class__ raises
            pass
    return n


def _assigned_names(node) -> list:
    """The plain names a module-level assignment binds (`A = ...`, `A, B = ...`, `A: int = ...`); [] for anything else
    (attribute or item targets, augmented assignments)."""
    import ast

    if isinstance(node, ast.Assign):
        targets = node.targets
    elif isinstance(node, ast.AnnAssign) and node.value is not None:
        targets = [node.target]
    else:
        return []
    names = []
    for t in targets:
        elts = t.elts if isinstance(t, (ast.Tuple, ast.List)) else [t]
        for e in elts:
            if not isinstance(e, ast.Name):
                return []
            names.append(e.id)
    return names


def hot_reload(path: str) -> dict:
    """Re-run the def / class / import statements of a loaded file inside its module and patch the existing functions
    and classes in place; module-level assignments whose names don't exist yet (new constants) run too, in file order,
    and so do assignments of an immutable literal whose value changed in the source (UI_VERSION = 4 -> 5). The file's
    other top-level code (connecting feeds, binding tables...) and assignments of existing names that didn't change in
    the source don't run again, so live tables, threads, figures and variables stay (a counter the code changed keeps
    its value). Returns {"patched", "added", "kept", "constants", "updated"}."""
    import ast

    from . import excel

    mod_name, _, sheet_id = _sources[path]
    mod = _modules[mod_name]
    with open(path, encoding="utf-8") as f:
        tree = ast.parse(f.read(), path)
    keep = (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Import, ast.ImportFrom)
    body, constants, updated = [], [], []
    lits, old_lits = _literal_constants(tree), _literals.get(path, {})
    for n in tree.body:
        if isinstance(n, keep):
            body.append(n)
        else:
            new = _assigned_names(n)
            if new and not any(x in mod.__dict__ for x in new):  # a new constant (existing names keep their value)
                body.append(n)
                constants += new
            elif len(new) == 1 and new[0] in lits and new[0] in old_lits and lits[new[0]] != old_lits[new[0]]:
                body.append(n)  # a constant edited in the source: the new value (state the code changed isn't touched)
                updated += new
    code = compile(ast.Module(body=body, type_ignores=[]), path, "exec")
    names = [n.name for n in body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))]
    old = {n: mod.__dict__[n] for n in names if n in mod.__dict__}
    prev = getattr(excel._sheet_context, "sheet", None)
    excel._sheet_context.sheet = sheet_id
    try:
        exec(code, mod.__dict__)
    except Exception as e:
        if constants:
            raise NeedsRestart(f"a new constant ({', '.join(constants)}) failed: {type(e).__name__}: {e}") from e
        raise
    finally:
        excel._sheet_context.sheet = prev
    _literals[path] = lits
    out = {"patched": [], "added": [], "kept": [], "constants": constants, "updated": updated, "new_attrs": {}}
    stale = []  # classes whose new __init__ sets attributes that existing objects don't have
    for n in names:
        new = mod.__dict__[n]
        o = old.get(n)
        if o is None:
            out["added"].append(n)
        elif inspect.isclass(o) and inspect.isclass(new):
            missing = sorted(_init_attrs(new) - _init_attrs(o))
            if missing:  # objects made before keep the attributes their old __init__ set
                out["new_attrs"][n] = missing
                live = _live_instances(o)
                if live:
                    stale.append(f"{n}.__init__ now sets {', '.join(missing)} and {live} {n} object{'s' if live > 1 else ''} made "
                                 f"before {'don' if live > 1 else 'doesn'}'t have {'them' if len(missing) > 1 else 'it'}")
            for attr, val in vars(new).items():  # existing instances get the new methods
                if attr in ("__dict__", "__weakref__"):
                    continue
                cur = o.__dict__.get(attr)
                if not (inspect.isfunction(cur) and _patch_function(cur, val)):
                    try:
                        setattr(o, attr, val)
                    except (AttributeError, TypeError):
                        pass
            mod.__dict__[n] = o
            out["patched"].append(n)
        elif _patch_function(o, new):
            mod.__dict__[n] = o
            out["patched"].append(n)
        else:
            out["kept"].append(n)  # the new object is used from now on; existing references keep the old one
    _sources[path] = (mod_name, _mtime(path), sheet_id)
    if stale:
        # their methods (already the new ones) would fail on the missing attributes: start over, as Developer > Reload
        # does, so the objects are made again by the new __init__
        raise NeedsRestart("; ".join(stale), error=False)
    return out


def _init_attrs(cls) -> set:
    """The attributes a class's __init__ sets on self (self.x = ...)."""
    import dis

    f = cls.__dict__.get("__init__")
    if not inspect.isfunction(f):
        return set()
    return {i.argval for i in dis.get_instructions(f.__code__) if i.opname == "STORE_ATTR"}


def _code_changed() -> bool:
    """YCGrid saw a code change: hot reload the changed files; new or deleted files restart the host (returns False then)."""
    current = {os.path.join(_app_dir, "main.py")} if os.path.isfile(os.path.join(_app_dir, "main.py")) else set()
    for sub in ("sheets", "modules"):
        current |= {os.path.join(_app_dir, sub, f) for f in _project_files(sub)}
    if current != set(_sources) or os.environ.get("YCGRID_RELOAD") == "restart":
        _conn_obj.notify("host.restart", {})
        return False
    changed = [p for p, (_, t, _) in _sources.items() if _mtime(p) != t]
    reloaded = []  # the files reloaded in place (project-relative), for Workbook_AfterReload
    for path in changed:
        rel = os.path.relpath(path, _app_dir)
        try:
            r = hot_reload(path)
        except SyntaxError as e:
            print(f"[reload] {rel}: syntax error on line {e.lineno}, the old code keeps running: {e.msg}", file=sys.stderr)
            continue
        except NeedsRestart as e:
            print(f"[reload] {rel}: {e}; restarting the host", file=sys.stderr if e.error else sys.stdout)
            _conn_obj.notify("host.restart", {})
            return False
        except Exception as e:  # noqa: BLE001
            _report(f"reloading {rel}", e)
            continue
        parts = [f"{len(r['patched'])} updated"]
        if r["added"]:
            parts.append(f"new: {', '.join(r['added'])}")
        if r["constants"]:
            parts.append(f"new constants: {', '.join(r['constants'])}")
        if r.get("updated"):
            parts.append(f"changed constants: {', '.join(r['updated'])}")
        if r["kept"]:
            parts.append(f"replaced (old references keep the old code): {', '.join(r['kept'])}")
        print(f"[reload] {rel}: {'; '.join(parts)} (state kept)")
        reloaded.append(rel.replace(os.sep, "/"))
    if reloaded:
        # the workbook's own follow-up: Workbook_Open runs once, so code that builds the screen from a version
        # number (UI_VERSION) checks it again here
        main_mod = _modules.get("main")
        if main_mod is not None and hasattr(main_mod, "Workbook_AfterReload"):
            _call("Workbook_AfterReload", main_mod.Workbook_AfterReload, reloaded)
    if changed:
        _send_sources()  # a module may have gained or lost Worksheet_LiveChange
        _conn_obj.notify("host.ready", {"macros": list_macros(), "udfs": sorted(_udfs), "udf_help": _udf_help(), "events": _wanted_events(), "sheet_events": _sheet_events(),
                                        "python": sys.version.split()[0], "modules": sorted(_modules), "load_errors": list(_load_errors)})
        _conn_obj.notify("host.opened", {})
    return True


_load_errors: list = []


def _settle_import_errors() -> None:
    """Errors "No module named X" logged before X was installed stop counting as new errors (the status bar's red chip):
    they become messages in the Error Log, marked resolved. Runs when the host starts, e.g. after installing packages."""
    import re

    try:
        entries = _conn_obj.request("errors.list", {"errors_only": True}) or []
        ids = []
        for e in entries:
            m = re.search(r"No module named '([\w.]+)'", f"{e.get('title', '')}\n{e.get('text', '')}")
            if m:
                try:
                    found = importlib.util.find_spec(m.group(1)) is not None
                except (ImportError, ValueError):
                    found = False
                if found:
                    ids.append(e["id"])
        if ids:
            _conn_obj.request("errors.resolve", {"ids": ids, "note": "Resolved: the module is installed now."})
    except Exception:  # noqa: BLE001 - bookkeeping only; never in the way of the host starting
        pass


def _sync_skill() -> None:
    """The project's copies of the YCGrid skill follow this YCGrid (background: file copies, no effect on the code)."""
    try:
        from . import scaffold

        n = scaffold.sync_skill(_app_dir)
        if n:
            print(f"[YCGrid] the YCGrid skill in this project was updated ({len(n)} files): AI agents read this version's rules")
    except Exception:  # noqa: BLE001 - a read-only folder, files in use: try again next time
        pass


def _note_load_error(path: str, e: BaseException) -> None:
    line = getattr(e, "lineno", None)
    if line is None:
        for fr in traceback.extract_tb(e.__traceback__):
            if os.path.normcase(fr.filename) == os.path.normcase(path):
                line = fr.lineno
    _load_errors.append({"file": os.path.relpath(path, _app_dir), "line": line or 0, "error": f"{type(e).__name__}: {e}"})


def _jsonable(v):
    """Job result: returned as is when msgpack can represent it, otherwise as repr."""
    from decimal import Decimal

    if v is None or isinstance(v, (bool, int, float, str, Decimal)):
        return v
    if isinstance(v, (list, tuple)):
        return [_jsonable(x) for x in v[:10000]]
    if isinstance(v, dict):
        return {str(k): _jsonable(x) for k, x in list(v.items())[:10000]}
    from .types import XlError

    if isinstance(v, XlError):
        return v
    return repr(v)


def _run_job(p: dict) -> None:
    """A job MCP / a standalone script asks the host to run (macro, code, control event); the result is reported when done."""
    from . import excel

    t0 = time.perf_counter()
    out = {"id": p.get("id"), "ok": True}
    kind = p.get("kind")
    prev = getattr(excel._sheet_context, "sheet", None)
    user = _UserCode().__enter__()
    try:
        if kind == "macro":
            fn = _resolve_proc(p["name"])
            out["result"] = _jsonable(fn(*p.get("args", [])))
        elif kind == "exec":
            out["result"] = _jsonable(_exec_in_console(p.get("code", "")))
        elif kind == "complete":  # Immediate window: names that complete the text before the caret
            out["result"] = complete(p.get("code", ""))
        elif kind == "control":
            sheet = p["sheet"]
            name, event = p["name"], p.get("event", "Click")
            code, mod = _sheet_module_for(sheet)
            fn = getattr(mod, f"{name}_{event}", None) if mod is not None else None
            if fn is None:
                out["handled"] = False
                out["message"] = f"sheets/{code}.py has no {name}_{event}()"
            else:
                excel._sheet_context.sheet = sheet
                fn()
                out["handled"] = True
                out["handler"] = f"{code}.{name}_{event}"
        else:
            raise YcError(f"Unknown job: {kind}", 1004, "E_ARG")
    except BaseException as e:  # noqa: BLE001 - also sys.exit() / KeyboardInterrupt in user code: the job must report back
        out["ok"] = False
        out["error"] = f"{type(e).__name__}: {e}"
        out["traceback"] = "".join(traceback.format_exception(type(e), e, e.__traceback__))[-6000:]
        _report(str(p.get("name") or kind), e)
    finally:
        user.__exit__(None, None, None)
        excel._sheet_context.sheet = prev
    out["ms"] = round((time.perf_counter() - t0) * 1000, 2)
    try:
        sys.stdout.flush()
        sys.stderr.flush()
    except Exception:  # noqa: BLE001
        pass
    _conn_obj.notify("job.done", out)


def _exec_in_console(code: str, filename: str = "<mcp>"):
    """Run in the Immediate window's namespace; the value of the last expression is the result."""
    import ast
    import linecache
    import warnings

    # matplotlib draws off screen in the host (no window may block it); plt.show() is then a no-op
    os.environ.setdefault("MPLBACKEND", "Agg")
    warnings.filterwarnings("ignore", message=".*non-interactive.*cannot be shown")

    _ensure_console_ns()
    # tracebacks show the source lines of the code that was run
    linecache.cache[filename] = (len(code), None, code.splitlines(True), filename)
    tree = ast.parse(code, filename, "exec")
    last = None
    if tree.body and isinstance(tree.body[-1], ast.Expr):
        last = ast.Expression(tree.body.pop().value)
    exec(compile(tree, filename, "exec"), _console_ns)
    return eval(compile(last, filename, "eval"), _console_ns) if last is not None else None


def _project_files(sub: str):
    d = os.path.join(_app_dir, sub)
    if not os.path.isdir(d):
        return []
    return sorted(f for f in os.listdir(d) if f.endswith(".py") and not f.startswith("_"))


def load_project() -> list[str]:
    """Names of the modules that failed to load because of missing packages."""
    global _loaded
    missing = []
    _refresh_code_names()

    _load_errors.clear()

    def try_load(mod_name, path, sheet_id=None):
        try:
            return _load_file(mod_name, path, sheet_id)
        except ModuleNotFoundError as e:
            missing.append(e.name or "")
            _report(f"loading {os.path.relpath(path, _app_dir)}", e)
            _note_load_error(path, e)
        except Exception as e:  # noqa: BLE001
            _report(f"loading {os.path.relpath(path, _app_dir)}", e)
            _note_load_error(path, e)
        return None

    main_py = os.path.join(_app_dir, "main.py")
    if os.path.isfile(main_py):
        try_load("main", main_py)
    known = {c.lower() for c in _code_names}   # empty when the workbook couldn't be asked: then nothing is skipped
    for f in _project_files("sheets"):
        code = f[:-3]
        if known and code.lower() not in known:
            # the module of a sheet that was deleted (or another workbook's in this folder): its Me has no sheet, so
            # it isn't loaded; the file stays, it is the user's code (a new sheet with this CodeName picks it up)
            try:
                _conn_obj.notify("log", {"text": f"[YCGrid] sheets\\{f}: no sheet has the CodeName {code}; the module isn't loaded"})
            except YcError:
                pass
            continue
        mod = try_load(f"sheets.{code}", os.path.join(_app_dir, "sheets", f), _code_names.get(code))
        if mod is not None:
            _sheet_modules[code] = mod
            mod.__yc_sheet_id__ = _code_names.get(code)
    for f in _project_files("modules"):
        mod = try_load(f"modules.{f[:-3]}", os.path.join(_app_dir, "modules", f))
        if mod is not None:
            sys.modules.setdefault(f[:-3], mod)
    _loaded = True
    return [m for m in missing if m]


def list_macros() -> list[str]:
    out = []
    for key, mod in _modules.items():
        for name, fn in vars(mod).items():
            if name.startswith("_") or not inspect.isfunction(fn) or fn.__module__ != mod.__name__:
                continue
            if any(name.startswith(p) for p in ("Workbook_", "Worksheet_")) or name in [u["fn"].__name__ for u in _udfs.values()]:
                continue
            try:
                sig = inspect.signature(fn)
            except (TypeError, ValueError):
                continue
            if any(p.default is inspect.Parameter.empty and p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD) for p in sig.parameters.values()):
                continue
            prefix = key.split(".", 1)[1] if "." in key else "ThisWorkbook"
            out.append(f"{prefix}.{name}")
    return sorted(out)


# ---------------------------------------------------------------------------
# event dispatch
# ---------------------------------------------------------------------------
_offered: set = set()  # packages the bar has offered for a failing handler in this host (offered once: Skip holds)


def _offer_missing(e: BaseException) -> None:
    """A handler failed on a package that isn't installed (Workbook_Open of a screen whose imports looked complete,
    an event, a macro): the package bar offers it, with every other one the failing code needs, as for the project's
    own imports. After the install the host restarts and Workbook_Open runs again."""
    if not (isinstance(e, ModuleNotFoundError) and e.name) or _conn_obj is None:
        return
    from . import env

    try:
        pkgs = env.missing_for_error(e, "", _app_dir)
    except Exception:  # noqa: BLE001 - the scan is a convenience; the failed import alone still works
        pkgs = [env.package_for_module(e.name)]
    new = [p for p in pkgs if p not in _offered]
    if new:
        _offered.update(pkgs)
        _conn_obj.notify("env.missing", {"packages": pkgs, "policy": env.policy(_app_dir)})


# ---- Application.EnableCancelKey: Ctrl+Break interrupts the code running ----
cancel_key = 1  # xlDisabled 0, xlInterrupt 1, xlErrorHandler 2; back to xlInterrupt when the code returns (Excel at idle)
_user_threads: dict = {}  # thread id -> depth of the user code it is running (handlers, macros, jobs)
_user_lock = threading.Lock()


class _UserCode:
    """Marks the current thread as running user code for Ctrl+Break, and resets EnableCancelKey when the outermost
    call returns."""

    def __enter__(self):
        tid = threading.get_ident()
        with _user_lock:
            _user_threads[tid] = _user_threads.get(tid, 0) + 1
        return self

    def __exit__(self, *exc):
        global cancel_key
        tid = threading.get_ident()
        with _user_lock:
            n = _user_threads.get(tid, 1) - 1
            if n <= 0:
                _user_threads.pop(tid, None)
                if not _user_threads:
                    cancel_key = 1
            else:
                _user_threads[tid] = n
        return False


def _on_interrupt(_p=None) -> None:
    """Ctrl+Break (urgent: handled on the reader thread while the code runs): KeyboardInterrupt in the code running
    (xlInterrupt: it stops, and the Error Log says so), CodeInterruptedError (xlErrorHandler: the code can catch it),
    or nothing (xlDisabled)."""
    import ctypes

    from .errors import CodeInterruptedError

    if cancel_key == 0:
        return
    with _user_lock:
        tids = list(_user_threads)
    exc = KeyboardInterrupt if cancel_key == 1 else CodeInterruptedError
    for tid in tids:
        ctypes.pythonapi.PyThreadState_SetAsyncExc(ctypes.c_ulong(tid), ctypes.py_object(exc))


def _call(where: str, fn: Callable, *args, sheet_id: Optional[int] = None):
    from . import excel

    prev = getattr(excel._sheet_context, "sheet", None)
    excel._sheet_context.sheet = sheet_id
    try:
        with _UserCode():
            return fn(*args)
    except KeyboardInterrupt as e:  # Ctrl+Break (EnableCancelKey xlInterrupt)
        _report(where, YcError("Code execution has been interrupted (Ctrl+Break)", 18, "E_INTERRUPTED") if not str(e) else e)
    except Exception as e:  # noqa: BLE001
        _report(where, e)
        try:
            _offer_missing(e)
        except Exception:  # noqa: BLE001 - offering packages never hides the handler's own error
            pass
    finally:
        excel._sheet_context.sheet = prev
    return None


HOST_SHUTDOWN_LIMIT = 2.0  # seconds Workbook_HostShutdown may take (PLAN §13)


def _call_limited(where: str, fn: Callable, limit: float):
    """fn() on a thread, waited for at most `limit` seconds: a handler that hangs can't keep the host from exiting."""
    t = threading.Thread(target=_call, args=(where, fn), daemon=True, name=where)
    t.start()
    t.join(limit)
    if t.is_alive():
        msg = f"[{where}] didn't return within {limit:g} s; the host goes on without it"
        try:
            _conn_obj.notify("log", {"text": msg, "kind": "error"})
        except Exception:  # noqa: BLE001 - YCGrid already gone
            print(msg, file=sys.__stderr__)


def _sheet_module_for(sheet_id: int):
    for code, sid in _code_names.items():
        if sid == sheet_id:
            return code, _sheet_modules.get(code)
    return None, None


def _on_sheet_change(p: dict) -> None:
    from . import excel
    from .constants import ycCalc, ycLive, ycPython, ycUser

    if not excel.Application.EnableEvents:
        return
    sheet = p["sheet"]
    areas = [tuple(a) for a in p["areas"]]
    src = {"user": ycUser, "python": ycPython, "calc": ycCalc, "live": ycLive}.get(p.get("source", "user"), ycUser)
    target = excel.Range._from(sheet, areas)
    # 1) per-cell OnChange
    with _lock:
        handlers = [h for h in _onchange if h["sheet"] == sheet and h["sources"] & src]
    for h in handlers:
        hit = excel.Intersect(target, excel.Range._from(sheet, h["areas"]))
        if hit is not None:
            _call("OnChange", h["fn"], hit, sheet_id=sheet)
    if src not in (ycUser, ycPython):
        # calculation / live data: only Worksheet_LiveChange, then main.py's Workbook_SheetLiveChange
        code, mod = _sheet_module_for(sheet)
        if mod is not None and hasattr(mod, "Worksheet_LiveChange"):
            _call(f"{code}.Worksheet_LiveChange", mod.Worksheet_LiveChange, target, sheet_id=sheet)
        main = _modules.get("main")
        if main is not None and hasattr(main, "Workbook_SheetLiveChange"):
            _call("Workbook_SheetLiveChange", main.Workbook_SheetLiveChange, excel.Worksheet(sheet), target)
        return
    # 2) Worksheet_Change
    code, mod = _sheet_module_for(sheet)
    if mod is not None and hasattr(mod, "Worksheet_Change"):
        _call(f"{code}.Worksheet_Change", mod.Worksheet_Change, target, sheet_id=sheet)
    # 3) Workbook_SheetChange
    main = _modules.get("main")
    if main is not None and hasattr(main, "Workbook_SheetChange"):
        _call("Workbook_SheetChange", main.Workbook_SheetChange, excel.Worksheet(sheet), target)


def _on_udf_call(p: dict) -> None:
    from . import excel
    from .types import XlError

    key, name, args = p["key"], p["name"].upper(), p.get("args", [])
    info = _udfs.get(name)
    if info is None:
        _conn_obj.notify("udf.result", {"key": key, "value": XlError(5)})
        return
    # 1x1 arrays become scalars, everything else stays a 2D list
    conv = []
    for a in args:
        if isinstance(a, list) and len(a) == 1 and isinstance(a[0], list) and len(a[0]) == 1:
            conv.append(a[0][0])
        elif isinstance(a, list):
            conv.append(excel.VArray(a))
        else:
            conv.append(a)
    if not info["accept_errors"]:
        for a in conv:
            if isinstance(a, XlError):
                _conn_obj.notify("udf.result", {"key": key, "value": a})
                return
    caller = p.get("caller")
    excel._Application._caller.range = excel.Range._from(caller[0], [(caller[1], caller[2], caller[1], caller[2])]) if caller else None
    try:
        result = info["fn"](*conv)
    except Exception as e:  # noqa: BLE001
        _report(f"UDF {name}", e)
        result = XlError(11)  # #PYTHON!
    finally:
        excel._Application._caller.range = None
    if hasattr(result, "tolist") and hasattr(result, "ndim"):
        result = result.tolist()
    if isinstance(result, (list, tuple)):
        rows = [list(r) for r in result] if result and isinstance(result[0], (list, tuple)) else [list(result)]
        if len(rows) == 1 and len(rows[0]) == 1:
            result = rows[0][0]
        elif rows and rows[0]:  # an array: the formula spills it
            width = max(len(r) for r in rows)
            _conn_obj.notify("udf.result", {"key": key, "array": [[excel._to_wire(v) for v in r] + [None] * (width - len(r)) for r in rows]})
            return
        else:
            result = None
    _conn_obj.notify("udf.result", {"key": key, "value": excel._to_wire(result)})


class _Topic:
    def __init__(self, key: str, args):
        self.Id = key
        self.Args = tuple(args)

    def Push(self, value):  # noqa: N802
        from . import excel

        _conn_obj.notify("stream.push", {"items": [[self.Id, excel._to_wire(value)]]})


def _on_stream_subscribe(p: dict) -> None:
    from . import excel
    from .types import XlError

    name = p["name"].upper()
    args = list(p.get("args", []))
    if name == "STREAM" and args:  # =STREAM(name, topic...): the @stream of that name, with the topics
        name, args = str(args[0]).upper(), args[1:]
    cls = _streams.get(name)
    if cls is None:
        if name == "QUOTE":
            return  # QUOTE is provided by Application.Topics.Push
        _conn_obj.notify("stream.push", {"items": [[p["topic"], XlError(5)]]})
        return
    obj = _stream_objs.get(name)
    if obj is None:
        obj = _stream_objs[name] = cls()
    _call(f"@stream {name}", obj.subscribe, _Topic(p["topic"], args))


def _on_control(p: dict) -> None:
    from . import ui

    if p.get("panel"):
        ui.dispatch_panel_event(p)
        return
    sheet = p["sheet"]
    code, mod = _sheet_module_for(sheet)
    fn_name = f"{p['name']}_{p['event']}"
    if mod is not None and hasattr(mod, fn_name):
        _call(f"{code}.{fn_name}", getattr(mod, fn_name), sheet_id=sheet)
        return
    on_action = p.get("on_action")
    if on_action:
        try:
            _call(on_action, _resolve_proc(on_action), sheet_id=sheet)
        except YcError as e:
            _report(on_action, e)


def _ribbon_text(v) -> str:
    if isinstance(v, bool):
        return "1" if v else "0"
    return "" if v is None else str(v)


def _on_ribbon(p: dict) -> None:
    """RibbonX callbacks from customUI.xml: onLoad(ribbon), onAction / onChange(control, ...), get*(control, ...) -> value."""
    from . import ui

    name = p.get("callback", "")
    try:
        fn = _resolve_proc(name)
    except YcError as e:
        _report(name, e)
        return
    op = p.get("op")
    if op == "load":
        _call(name, fn, ui.IRibbonUI())
        return
    control = ui.IRibbonControl(p.get("control", ""), p.get("tag", ""))
    args = list(p.get("args") or [])
    if op == "action":
        _call(name, fn, control, *args)
        return
    if op == "get":
        value = _call(name, fn, control, *args)
        _conn_obj.request("ribbon.value", {"key": p.get("key", ""), "value": _ribbon_text(value)})


# sheet events and the workbook-level event main.py gets for any sheet (Excel's order: the sheet's, then the workbook's)
_SHEET_EVENTS = {
    "Worksheet_SelectionChange": "Workbook_SheetSelectionChange",
    "Worksheet_Activate": "Workbook_SheetActivate",
    "Worksheet_Deactivate": "Workbook_SheetDeactivate",
    "Worksheet_Calculate": "Workbook_SheetCalculate",
    "Worksheet_BeforeDoubleClick": "Workbook_SheetBeforeDoubleClick",
    "Worksheet_BeforeRightClick": "Workbook_SheetBeforeRightClick",
    "Worksheet_FollowHyperlink": "Workbook_SheetFollowHyperlink",
    "Worksheet_LiveChange": "Workbook_SheetLiveChange",
}
# every event name YCGrid fires (yc_python_status reports the others as never fired)
FIRED_EVENTS = frozenset(set(_SHEET_EVENTS) | set(_SHEET_EVENTS.values()) | {
    "Workbook_Open", "Workbook_BeforeClose", "Workbook_BeforeSave", "Workbook_AfterSave", "Workbook_NewSheet",
    "Workbook_SheetChange", "Workbook_AfterReload", "Workbook_HostShutdown", "Worksheet_Change"})


def _on_event(p: dict) -> None:
    """An event from YCGrid (events_ui.cpp): the sheet module's handler, then main.py's Workbook_Sheet* one. A Before*
    event (p["ask"]) is answered with whether a handler set Cancel.Value = True, also when a handler raised."""
    from . import excel

    name = p.get("name", "")
    sheet = p.get("sheet")
    ask = p.get("ask")
    cancel = excel.ByRef(False)
    try:
        if not excel.Application.EnableEvents:
            return
        main = _modules.get("main")
        if name.startswith("Workbook_"):
            fn = getattr(main, name, None) if main is not None else None
            if fn is None:
                return
            if name == "Workbook_BeforeSave":
                args = [bool(p.get("save_as_ui", False)), cancel]
            elif name == "Workbook_AfterSave":
                args = [bool(p.get("success", True))]
            elif name == "Workbook_NewSheet":
                args = [excel.Worksheet(sheet)]
            elif name == "Workbook_BeforeClose":
                args = [cancel]
            else:
                args = []
            _cancelled(_call(name, fn, *args), cancel)
            return
        if name not in _SHEET_EVENTS or sheet is None:
            return
        if name == "Worksheet_FollowHyperlink":
            target = excel.Hyperlink(sheet, p.get("row", 0), p.get("col", 0))
        else:
            target = excel.Range._from(sheet, [tuple(a) for a in p.get("areas", [])]) if p.get("areas") else None
        if name in ("Worksheet_BeforeDoubleClick", "Worksheet_BeforeRightClick"):
            args = [target, cancel]
        elif name in ("Worksheet_SelectionChange", "Worksheet_FollowHyperlink"):
            args = [target]
        else:
            args = []
        code, mod = _sheet_module_for(sheet)
        fn = getattr(mod, name, None) if mod is not None else None
        if fn is not None:
            _cancelled(_call(f"{code}.{name}", fn, *args, sheet_id=sheet), cancel)
        wb_name = _SHEET_EVENTS[name]
        fn = getattr(main, wb_name, None) if main is not None else None
        if fn is not None:
            _cancelled(_call(wb_name, fn, excel.Worksheet(sheet), *args), cancel)
    finally:
        if ask:
            _conn_obj.notify("event.answer", {"id": ask, "cancel": bool(cancel)})


def run_before_save(save_as_ui: bool) -> bool:
    """The host's own Workbook.Save / SaveAs: Workbook_BeforeSave runs here first (YCGrid can't ask the host while the
    host waits for the save). False when a handler cancelled."""
    from . import excel

    main = _modules.get("main")
    fn = getattr(main, "Workbook_BeforeSave", None) if main is not None else None
    if fn is None or not excel.Application.EnableEvents:
        return True
    cancel = excel.ByRef(False)
    _cancelled(_call("Workbook_BeforeSave", fn, bool(save_as_ui), cancel), cancel)
    return not cancel


def _sheet_events() -> dict:
    """Worksheet_* handler -> the ids of the sheets whose module defines it: YCGrid asks the host about a Before*
    event only on those sheets (or every sheet when main.py has the Workbook_Sheet* one)."""
    out: dict = {}
    for code, mod in _sheet_modules.items():
        sid = getattr(mod, "__yc_sheet_id__", None)
        if sid is None:
            continue
        for n, v in vars(mod).items():
            if n.startswith("Worksheet_") and callable(v):
                out.setdefault(n, []).append(int(sid))
    return out


def _cancelled(result, cancel) -> None:
    """A Before* handler cancels with Excel's Cancel = True written the Python ways: Cancel.Value = True, or return
    True from the handler (Python can't rebind the caller's Cancel)."""
    if result is True:
        cancel.Value = True


def _wanted_events() -> list[str]:
    """The event handlers the project defines: Workbook_* in main.py, Worksheet_* in sheets/. One in another module
    never runs: it is listed as "module.name" (yc_python_status shows it apart)."""
    names = set()
    for key, mod in _modules.items():
        for n, v in vars(mod).items():
            if not n.startswith(("Workbook_", "Worksheet_")) or not callable(v):
                continue
            right = (key == "main" and n.startswith("Workbook_")) or (key.startswith("sheets.") and n.startswith("Worksheet_"))
            names.add(n if right else f"{key}.{n}")
    return sorted(names)


# ---------------------------------------------------------------------------
# main program
# ---------------------------------------------------------------------------
def _dispatch(method: str, params) -> bool:
    """Handle one notification from YCGrid; False = the host should stop."""
    try:
        if method == "__closed__" or method == "shutdown":
            # closing: Workbook_BeforeClose, then Workbook_HostShutdown; a restart or reload (or YCGrid gone): only
            # Workbook_HostShutdown, so the code can stop its threads and close its connections (PLAN §13)
            main_mod = _modules.get("main")
            reason = (params or {}).get("reason", "close") if method == "shutdown" else "closed"
            if main_mod is not None:
                # (the window asks Workbook_BeforeClose itself first, where Cancel works: then it isn't run again)
                if reason == "close" and hasattr(main_mod, "Workbook_BeforeClose") and not (params or {}).get("before_close_done"):
                    from . import excel

                    if excel.Application.EnableEvents:
                        _call("Workbook_BeforeClose", main_mod.Workbook_BeforeClose, excel.ByRef(False))
                if hasattr(main_mod, "Workbook_HostShutdown"):
                    _call_limited("Workbook_HostShutdown", main_mod.Workbook_HostShutdown, HOST_SHUTDOWN_LIMIT)
            return False
        if method == "ping":  # liveness check (safety): answered from the event loop, so a busy host doesn't answer
            _conn_obj.notify("host.pong", {})
        elif method == "click":  # yc.interact.on_click
            from . import interact

            interact._dispatch_click(params)
        elif method == "onkey":  # Application.OnKey / yc.interact.on_key
            from . import interact

            if not interact._dispatch_key(params):
                proc = params.get("procedure", "")
                _call(proc, _resolve_proc(proc))
        elif method == "alert":  # yc.alerts.on_alert
            from . import alerts

            alerts._dispatch(params)
        elif method == "sheet.change":
            _on_sheet_change(params)
        elif method == "udf.call":
            _on_udf_call(params)
        elif method == "stream.subscribe":
            _on_stream_subscribe(params)
        elif method == "control":
            _on_control(params)
        elif method == "ribbon":
            _on_ribbon(params)
        elif method == "event":
            _on_event(params)
        elif method == "macro.run":
            try:
                _call(params["name"], _resolve_proc(params["name"]))
            except YcError as e:
                _report(params["name"], e)
        elif method == "env.install":
            from . import env

            pkgs = params.get("packages", [])
            try:
                ok = env.install(_app_dir, pkgs, always=params.get("always", False))
            except Exception as e:  # noqa: BLE001 - writing requirements.txt, the lock...: still say so
                ok, env.last_error = False, f"{type(e).__name__}: {e}"
            if ok:
                _conn_obj.notify("host.restart", {})
                return False
            # the package bar comes back with the reason, so the person can try again or skip (it said nothing before)
            _conn_obj.notify("env.install_failed", {"packages": pkgs, "reason": env.last_error or "the installation failed"})
        elif method == "__ontime__":
            proc = params["proc"]
            _call(str(proc), _resolve_proc(proc) if isinstance(proc, str) else proc)
        elif method == "job":
            _run_job(params)
        elif method == "code_changed":  # a file of the project was saved
            return _code_changed()
        elif method == "plot.event":  # a click / drag / selection on a figure with callbacks (yc.plot)
            from . import plot

            plot._dispatch(params)
        elif method == "eval":
            # Immediate window console: ? expression or a statement; shown=False: run for a dialog or a script (host.eval),
            # where the Immediate window may be closed
            _run_console(params.get("code", ""), shown=params.get("shown", True))
    except Exception as e:  # noqa: BLE001
        _report(method, e)
    finally:
        try:
            sys.stdout.flush()
            sys.stderr.flush()
        except Exception:  # noqa: BLE001
            pass
    return True


_stopping = False


def pump_while(cond: Callable[[], bool], poll: float = 0.05) -> bool:
    """A nested event loop (a modal UserForm.Show): handle YCGrid's notifications while cond() is true.
    Returns False when the host is stopping (the notification is put back for the main loop)."""
    global _stopping
    if _conn_obj is None:
        return False
    import queue

    while cond() and not _stopping:
        try:
            method, params = _conn_obj.notifications.get(timeout=poll)
        except queue.Empty:
            continue
        if method in ("__closed__", "shutdown", "env.install"):
            _stopping = True
            _conn_obj.notifications.put((method, params))
            return False
        _dispatch(method, params)
    return not _stopping


def main(argv=None) -> int:
    global _conn_obj, _app_dir
    ap = argparse.ArgumentParser(prog="yc.host")
    ap.add_argument("--pipe-in", required=True)
    ap.add_argument("--pipe-out", required=True)
    ap.add_argument("--app-dir", required=True)
    ns = ap.parse_args(argv)
    _app_dir = os.path.abspath(ns.app_dir)
    _conn_obj = Connection.open_host(ns.pipe_in, ns.pipe_out)
    _conn_obj.urgent["host.interrupt"] = _on_interrupt  # Ctrl+Break reaches the code while it runs
    _conn.set_current(_conn_obj)
    sys.stdout = _ConsoleStream("out")
    sys.stderr = _ConsoleStream("error")
    warnings.showwarning = _show_warning
    for p in (os.path.join(_app_dir, "lib"), _app_dir):
        if p not in sys.path:
            sys.path.insert(0, p)
    os.chdir(_app_dir)
    _conn_obj.request("hello", {"role": "host", "pid": os.getpid(), "python": sys.version.split()[0], "executable": sys.executable})

    from . import env, excel  # noqa: F401 (registers the extension types on import)

    # [python] paths in ycgrid.toml: shared code folders (examples\lib for YCGrid's examples), after the project's lib\
    at = sys.path.index(os.path.join(_app_dir, "lib")) + 1 if os.path.join(_app_dir, "lib") in sys.path else 1
    for i, p in enumerate(p for p in env.project_paths(_app_dir) if p not in sys.path):
        sys.path.insert(at + i, p)

    # package detection (PLAN §13 (g)): requirements.txt and import scanning
    try:
        missing_pkgs = env.find_missing(_app_dir)
    except Exception as e:  # noqa: BLE001
        _report("package detection", e)
        missing_pkgs = []
    _udfs.setdefault("PY", {"fn": _py_formula, "volatile": False, "accept_errors": False})
    failed_imports = load_project()
    if failed_imports:
        missing_pkgs = sorted(set(missing_pkgs) | {env.package_for_module(m) for m in failed_imports})
    if missing_pkgs:
        _offered.update(missing_pkgs)  # Workbook_Open failing on the same ones doesn't offer them again
        _conn_obj.notify("env.missing", {"packages": missing_pkgs, "policy": env.policy(_app_dir)})
    threading.Thread(target=_timer_loop, name="yc-ontime", daemon=True).start()
    threading.Thread(target=_sync_skill, name="yc-skill", daemon=True).start()
    threading.Thread(target=_settle_import_errors, name="yc-settle", daemon=True).start()
    _send_sources()  # Worksheet_LiveChange needs calculation / live changes reported
    _conn_obj.notify("host.ready", {"macros": list_macros(), "udfs": sorted(_udfs), "udf_help": _udf_help(), "events": _wanted_events(), "sheet_events": _sheet_events(),
                                    "python": sys.version.split()[0], "modules": sorted(_modules), "load_errors": list(_load_errors)})
    main_mod = _modules.get("main")
    if main_mod is not None and hasattr(main_mod, "Workbook_Open"):
        _call("Workbook_Open", main_mod.Workbook_Open)
    sys.stdout.flush()
    _conn_obj.notify("host.opened", {})

    while True:
        try:
            method, params = _conn_obj.notifications.get()
            if not _dispatch(method, params):
                break
        except KeyboardInterrupt:  # an interrupt that came just as the job finished
            continue
    return 0


_console_ns: dict = {}


_SAFE_CALL = None


def _completion_base(expr: str):
    """Evaluates the object before the last dot for completion, without side effects: names, attributes, subscripts with
    literals and calls of the Excel object model's accessors (capitalized names such as Range("A1"), Worksheets(1)) with
    literal arguments. None when the expression is anything else."""
    import ast

    try:
        tree = ast.parse(expr, mode="eval")
    except SyntaxError:
        return None

    def ok(n) -> bool:
        if isinstance(n, ast.Name):
            return True
        if isinstance(n, ast.Constant):
            return True
        if isinstance(n, ast.Attribute):
            return ok(n.value)
        if isinstance(n, ast.Subscript):
            return ok(n.value) and isinstance(n.slice, ast.Constant)
        if isinstance(n, ast.Call):
            f = n.func
            name = f.attr if isinstance(f, ast.Attribute) else f.id if isinstance(f, ast.Name) else ""
            return (bool(name) and name[0].isupper() and ok(f) and all(isinstance(a, ast.Constant) for a in n.args)
                    and all(isinstance(k.value, ast.Constant) for k in n.keywords))
        return False

    if not ok(tree.body):
        return None
    try:
        return eval(compile(tree, "<complete>", "eval"), _console_ns)
    except Exception:  # noqa: BLE001
        return None


def _kind(v) -> str:
    import inspect

    if inspect.isclass(v):
        return "class"
    if inspect.ismodule(v):
        return "module"
    if callable(v):
        return "function"
    return "value"


def _doc(v) -> str:
    import inspect

    try:
        d = inspect.getdoc(v) if callable(v) or inspect.ismodule(v) else None
    except Exception:  # noqa: BLE001
        d = None
    if not d:
        return ""
    line = d.strip().splitlines()[0]
    return line[:160]


def complete(code: str, limit: int = 200) -> dict:
    """Completions for the end of `code`: {"start": index where the word being typed starts, "items": [{"name", "kind",
    "doc"}]} from the Immediate window's namespace (variables, the Excel object model, builtins, keywords) or the
    members of the object before a dot."""
    import builtins
    import keyword
    import re

    _ensure_console_ns()
    word = re.search(r"(\w*)$", code).group(1)
    start = len(code) - len(word)
    base_expr = None
    if start > 0 and code[start - 1] == ".":
        mb = re.search(r"([A-Za-z_]\w*(?:\([^()]*\)|\[[^\[\]]*\])*(?:\.[A-Za-z_]\w*(?:\([^()]*\)|\[[^\[\]]*\])*)*)$", code[:start - 1])
        base_expr = mb.group(1) if mb else ""
        if not base_expr:
            return {"start": start, "items": []}
    items = []
    if base_expr is not None:
        obj = _completion_base(base_expr)
        if obj is None:
            return {"start": start, "items": []}
        names = [n for n in dir(obj) if (word.startswith("_") or not n.startswith("_"))]
        for n in sorted(names, key=lambda x: (x.lower() != word.lower(), x.lower())):
            if not n.lower().startswith(word.lower()):
                continue
            try:
                v = getattr(type(obj), n, None)
                if isinstance(v, property):
                    items.append({"name": n, "kind": "property", "doc": _doc(v.fget)})
                    continue
                v = getattr(obj, n) if v is None or not callable(v) else v
            except Exception:  # noqa: BLE001 - a property that fails: still listed
                items.append({"name": n, "kind": "value", "doc": ""})
                continue
            items.append({"name": n, "kind": _kind(v), "doc": _doc(v)})
            if len(items) >= limit:
                break
        return {"start": start, "items": items}
    if not word:
        return {"start": start, "items": []}
    pool = {}
    for n in list(_console_ns):
        pool.setdefault(n, _console_ns[n])
    for n in dir(builtins):
        if not n.startswith("_"):
            pool.setdefault(n, getattr(builtins, n))
    low = word.lower()
    cands = [n for n in pool if n.lower().startswith(low) and not n.startswith("__")]
    cands += [k for k in keyword.kwlist if k.startswith(word) and k not in pool]
    for n in sorted(cands, key=lambda x: (not x.startswith(word), x.lower()))[:limit]:
        if n in pool:
            items.append({"name": n, "kind": _kind(pool[n]), "doc": _doc(pool[n])})
        else:
            items.append({"name": n, "kind": "keyword", "doc": ""})
    return {"start": start, "items": items}


def _ensure_console_ns() -> None:
    from . import excel

    if not _console_ns:
        _console_ns.update({k: getattr(excel, k) for k in excel.__all__})
        for name, mod in _modules.items():
            _console_ns[name.split(".")[-1]] = mod


def _run_console(code: str, shown: bool = True) -> None:
    _ensure_console_ns()
    code = code.strip()
    if not code:
        return
    print(">>> " + code)
    try:
        if code.startswith("?"):
            print(repr(eval(code[1:], _console_ns)))
            return
        try:
            v = eval(compile(code, "<immediate>", "eval"), _console_ns)
            if v is not None:
                print(repr(v))
        except SyntaxError:
            exec(compile(code, "<immediate>", "exec"), _console_ns)
    except Exception as e:  # noqa: BLE001
        sys.stdout.flush()
        try:
            if isinstance(e, ModuleNotFoundError) and e.name:
                # offer the package as for the project's own imports, with every other one the code needs (one install,
                # not one per package); once they are installed, the code runs again
                from . import env

                try:
                    pkgs = env.missing_for_error(e, code, _app_dir)
                except Exception:  # noqa: BLE001 - the scan is a convenience; the failed import alone still works
                    pkgs = [env.package_for_module(e.name)]
                if pkgs:
                    _conn_obj.notify("env.missing", {"packages": pkgs, "policy": env.policy(_app_dir), "retry": code,
                                                     "shown": bool(shown)})
            if shown:  # typed in the Immediate window: the error shows there and isn't repeated in the Error Log
                _conn_obj.notify("log", {"text": f"{type(e).__name__}: {e}", "kind": "error_shown"})
            else:  # run for a dialog: the Immediate window may not be open, so the Error Log and the status bar say it
                _report(code.splitlines()[0][:60], e)
        except (YcError, AttributeError):
            print(f"{type(e).__name__}: {e}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
