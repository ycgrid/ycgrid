# SPDX-License-Identifier: Apache-2.0
"""Python runtime: detecting and installing missing packages (PLAN §13 (d)(e)(g)).

- Packages only go into YCGrid's own runtime\\python (sys.prefix), never the system Python.
- Detection: requirements.txt compared with the installed packages; ast scans the imports of every .py in the project.
- Installation: uv.exe next to the runtime (uv pip install --python <this interpreter>); python -m pip without uv.
- On success the packages are added to requirements.txt and requirements.lock is updated with uv pip freeze.
"""
from __future__ import annotations

import ast
import importlib.metadata as md
import importlib.util
import os
import re
import subprocess
import sys

try:
    import tomllib
except ImportError:  # pragma: no cover
    tomllib = None

# module name -> PyPI package name (common mismatches)
MODULE_TO_PACKAGE = {
    "cv2": "opencv-python", "PIL": "Pillow", "sklearn": "scikit-learn", "yaml": "PyYAML", "bs4": "beautifulsoup4",
    "win32api": "pywin32", "win32com": "pywin32", "win32con": "pywin32", "pythoncom": "pywin32", "dateutil": "python-dateutil",
    "dotenv": "python-dotenv", "jwt": "PyJWT", "Crypto": "pycryptodome", "serial": "pyserial", "usb": "pyusb",
    "OpenSSL": "pyOpenSSL", "google.protobuf": "protobuf", "zmq": "pyzmq", "talib": "TA-Lib", "fitz": "PyMuPDF",
    "docx": "python-docx", "pptx": "python-pptx", "magic": "python-magic", "attr": "attrs", "skimage": "scikit-image",
    "mpl_toolkits": "matplotlib", "lxml": "lxml", "shioaji": "shioaji", "ib_insync": "ib-insync",
}


def _toml(app_dir: str) -> dict:
    path = os.path.join(app_dir, "ycgrid.toml")
    if tomllib is None or not os.path.isfile(path):
        return {}
    try:
        with open(path, "rb") as f:
            return tomllib.load(f)
    except (OSError, ValueError):
        return {}


def policy(app_dir: str) -> str:
    v = _toml(app_dir).get("python", {}).get("auto_install", "ask")
    return v if v in ("ask", "always", "never") else "ask"


def package_for_module(module: str) -> str:
    top = module.split(".")[0]
    return MODULE_TO_PACKAGE.get(module, MODULE_TO_PACKAGE.get(top, top.replace("_", "-")))


def _requirement_names(app_dir: str) -> list[str]:
    path = os.path.join(app_dir, "requirements.txt")
    out = []
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line or line.startswith("-"):
                continue
            m = re.match(r"^([A-Za-z0-9_.\-]+)", line)
            if m:
                out.append(m.group(1))
    return out


def _installed() -> set[str]:
    names = set()
    for d in md.distributions():
        n = d.metadata.get("Name")
        if n:
            names.add(re.sub(r"[-_.]+", "-", n).lower())
    return names


def _norm(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def project_paths(app_dir: str) -> list[str]:
    """[python] paths in ycgrid.toml: folders of shared code the project imports as if it were in its lib\\ (relative
    to the project, e.g. "../lib"); the folders that exist, as absolute paths, in their order."""
    paths = _toml(app_dir).get("python", {}).get("paths", [])
    if isinstance(paths, str):
        paths = [paths]
    out = []
    for p in paths if isinstance(paths, list) else []:
        if not isinstance(p, str) or not p:
            continue
        full = os.path.normpath(p if os.path.isabs(p) else os.path.join(app_dir, p))
        if os.path.isdir(full) and full not in out:
            out.append(full)
    return out


def _project_modules(app_dir: str) -> set[str]:
    mods = set()
    for d in [os.path.join(app_dir, sub) for sub in ("", "lib", "modules", "sheets")] + project_paths(app_dir):
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if f.endswith(".py"):
                mods.add(f[:-3])
            elif os.path.isdir(os.path.join(d, f)) and os.path.isfile(os.path.join(d, f, "__init__.py")):
                mods.add(f)
    mods |= {"main", "sheets", "modules", "lib", "yc"}
    return mods


_SKIP_DIRS = ("runtime", ".ycgrid", "__pycache__", ".git", ".venv", "examples", "dist", "typings")


def _file_imports(path: str) -> set[str]:
    try:
        with open(path, encoding="utf-8") as fh:
            tree = ast.parse(fh.read())
    except (OSError, SyntaxError, UnicodeDecodeError):
        return set()
    found = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                found.add(a.name.split(".")[0])
        elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
            found.add(node.module.split(".")[0])
    return found


def _tree_files(top: str) -> list[str]:
    out = []
    for root, dirs, files in os.walk(top):
        dirs[:] = [d for d in dirs if d not in _SKIP_DIRS]
        out += [os.path.join(root, f) for f in files if f.endswith(".py")]
    return out


def scan_imports(app_dir: str) -> set[str]:
    """Top-level modules the project's code imports: every file of the project folder, and of its [python] paths the
    modules it imports (and those they import), not the whole shared folder: an example that uses only ex_util isn't
    offered numpy because ex_math beside it needs it."""
    found = set()
    for path in _tree_files(app_dir):
        found |= _file_imports(path)
    shared: dict[str, list[str]] = {}  # module name in a [python] paths folder -> its files
    for top in project_paths(app_dir):
        for f in os.listdir(top):
            full = os.path.join(top, f)
            if f.endswith(".py"):
                shared.setdefault(f[:-3], [full])
            elif os.path.isfile(os.path.join(full, "__init__.py")):
                shared.setdefault(f, _tree_files(full))
    todo, seen = [m for m in found if m in shared], set()
    while todo:
        m = todo.pop()
        if m in seen:
            continue
        seen.add(m)
        for path in shared[m]:
            more = _file_imports(path)
            found |= more
            todo += [x for x in more if x in shared and x not in seen]
    return found


def find_missing(app_dir: str) -> list[str]:
    installed = _installed()
    missing = {r for r in _requirement_names(app_dir) if _norm(r) not in installed}
    stdlib = set(getattr(sys, "stdlib_module_names", ())) | set(sys.builtin_module_names)
    project = _project_modules(app_dir)
    for mod in scan_imports(app_dir):
        if mod in stdlib or mod in project or mod.startswith("_"):
            continue
        if importlib.util.find_spec(mod) is not None:
            continue
        missing.add(package_for_module(mod))
    return sorted(missing)


_OPTIONAL_GUARDS = {"ImportError", "ModuleNotFoundError", "Exception", "BaseException"}


def _imports(tree: ast.AST) -> tuple[set[str], list[tuple[int, str, list[str]]]]:
    """Top-level names of the absolute imports in a tree, and its module-level relative imports (level, module, names).
    Imports guarded by try/except ImportError are optional and left out."""
    names: set[str] = set()

    def visit(node, guarded):
        for child in ast.iter_child_nodes(node):
            g = guarded
            if isinstance(child, ast.Try):
                for h in child.handlers:
                    caught = h.type
                    types = caught.elts if isinstance(caught, ast.Tuple) else [caught]
                    if caught is None or any(isinstance(t, ast.Name) and t.id in _OPTIONAL_GUARDS for t in types):
                        g = True
                for part in child.body:
                    visit_one(part, g)
                for part in child.handlers + child.orelse + child.finalbody:
                    visit_one(part, guarded)
                continue
            visit_one(child, g)

    def visit_one(child, guarded):
        if not guarded:
            if isinstance(child, ast.Import):
                names.update(a.name.split(".")[0] for a in child.names)
            elif isinstance(child, ast.ImportFrom) and child.module and child.level == 0:
                names.add(child.module.split(".")[0])
        visit(child, guarded)

    visit(tree, False)
    rel = [(n.level, n.module or "", [a.name for a in n.names]) for n in getattr(tree, "body", [])
           if isinstance(n, ast.ImportFrom) and n.level > 0]
    return names, rel


def needed_modules(files: list[str], code: str = "", roots: list[str] | None = None) -> set[str]:
    """Top-level modules imported by the code and the files (with the modules they import beside them, relatively, at the
    top), limited to files under roots. Used to offer every missing package of a failure at once."""
    roots = [os.path.normcase(os.path.abspath(r)) for r in (roots or [])]

    def local(path: str) -> bool:
        p = os.path.normcase(os.path.abspath(path))
        return os.path.isfile(p) and p.endswith(".py") and any(p.startswith(r + os.sep) for r in roots)

    mods: set[str] = set()
    if code:
        try:
            mods |= _imports(ast.parse(code))[0]
        except SyntaxError:
            pass
    seen: set[str] = set()
    todo = [f for f in files if local(f)]
    while todo:
        path = os.path.normcase(os.path.abspath(todo.pop()))
        if path in seen or len(seen) > 200:
            continue
        seen.add(path)
        try:
            with open(path, encoding="utf-8") as fh:
                names, rel = _imports(ast.parse(fh.read()))
        except (OSError, SyntaxError, UnicodeDecodeError):
            continue
        mods |= names
        for level, module, imported in rel:
            base = os.path.dirname(path)
            for _ in range(level - 1):
                base = os.path.dirname(base)
            parts = [p for p in module.split(".") if p]
            target = os.path.join(base, *parts)
            cands = [target + ".py", os.path.join(target, "__init__.py")] if parts else []
            if not parts or os.path.isdir(target):  # from . import a, b / from .pkg import a: a may be a module
                cands += [os.path.join(target, n + ".py") for n in imported]
            todo += [c for c in cands if local(c)]
    return mods


def _available(module: str) -> bool:
    if module in getattr(sys, "stdlib_module_names", ()) or module in sys.builtin_module_names:
        return True
    try:
        return importlib.util.find_spec(module) is not None
    except (ImportError, ValueError):
        return False


def missing_for_error(exc: BaseException, code: str = "", app_dir: str = "") -> list[str]:
    """The packages to offer for a ModuleNotFoundError: the one that failed and every other missing package the failing
    code imports (the files in its traceback, under the yc package or the project), so one install is enough."""
    import traceback

    name = getattr(exc, "name", None) or ""
    top = name.split(".")[0]
    if not top or _available(top):  # a missing submodule of something installed isn't a package to install
        return []
    files = [f.f_code.co_filename for f, _ in traceback.walk_tb(exc.__traceback__)]
    roots = [os.path.dirname(os.path.abspath(__file__))] + ([app_dir] + project_paths(app_dir) if app_dir else [])
    project = _project_modules(app_dir) if app_dir else set()
    mods = {m for m in needed_modules(files, code, roots) if not m.startswith("_") and m not in project}
    mods = {m for m in mods if not _available(m)} | {top}
    return sorted({package_for_module(m) for m in mods})


def _uv() -> str | None:
    # runtime\python\python.exe → runtime\uv.exe
    cand = os.path.join(os.path.dirname(sys.prefix), "uv.exe")
    return cand if os.path.isfile(cand) else None


last_error = ""  # why the last install() failed, in a line or two for the package bar ("" after a success)


def failure_reason(output: str, code: int) -> str:
    """The lines of an installer's output that say why it failed (uv's "error:" / "cause:" / "hint:", pip's "ERROR:"),
    else its last lines; never empty."""
    lines = [ln.strip() for ln in output.splitlines() if ln.strip()]
    keep = [ln for ln in lines if ln.lower().startswith(("error", "cause", "hint", "\u00d7"))]
    text = " ".join(keep or lines[-3:])
    return text or f"the installer returned {code}"


def install(app_dir: str, packages: list[str], always: bool = False) -> bool:
    """Installs the packages into the project's Python (uv, else pip) and notes them in requirements.txt; on a
    failure returns False with the reason in last_error."""
    global last_error
    last_error = ""
    if not packages:
        return True
    cfg = _toml(app_dir).get("python", {})
    print(f"Installing packages: {', '.join(packages)}")
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PYTHON", "VIRTUAL_ENV", "CONDA"))}
    # [python] find_links: folder of wheels for offline installation (relative to the project), default runtime/wheels
    wheels = cfg.get("find_links") or os.path.join("runtime", "wheels")
    if not os.path.isabs(wheels):
        wheels = os.path.join(app_dir, wheels)
    uv = _uv()
    if uv:
        cmd = [uv, "pip", "install", "--python", sys.executable]
        env["UV_CACHE_DIR"] = os.path.join(os.path.dirname(sys.prefix), "_uvcache")
    else:
        cmd = [sys.executable, "-E", "-s", "-m", "pip", "install", "--disable-pip-version-check"]
    if cfg.get("index_url"):
        cmd += ["--index-url", cfg["index_url"]]
    if os.path.isdir(wheels):
        cmd += ["--find-links", wheels]
    cmd += packages
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except OSError as e:
        last_error = f"Couldn't run the installer: {e}"
        print(last_error, file=sys.stderr)
        return False
    for line in (p.stdout + p.stderr).splitlines()[-30:]:
        print(line)
    if p.returncode != 0:
        last_error = failure_reason(p.stdout + "\n" + p.stderr, p.returncode)
        print(f"Installation failed (code {p.returncode}): {last_error}", file=sys.stderr)
        return False
    # add to requirements.txt
    req = os.path.join(app_dir, "requirements.txt")
    existing = {_norm(n) for n in _requirement_names(app_dir)}
    with open(req, "a", encoding="utf-8") as f:
        for pkg in packages:
            if _norm(pkg) not in existing:
                f.write(pkg + "\n")
    # requirements.lock: exact versions, to rebuild on another machine
    try:
        freeze_cmd = [uv, "pip", "freeze", "--python", sys.executable] if uv else [sys.executable, "-E", "-s", "-m", "pip", "freeze"]
        out = subprocess.run(freeze_cmd, capture_output=True, text=True, encoding="utf-8", env=env,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
        # packages built into the runtime (pip etc.) are recorded as local paths; not written to the lock
        lines = [ln for ln in out.splitlines() if " @ file:" not in ln and ln.strip()]
        with open(os.path.join(app_dir, "requirements.lock"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    except OSError:
        pass
    if always:
        _set_policy(app_dir, "always")
    print("Installation finished, reloading the Python code.")
    return True


def _set_policy(app_dir: str, value: str) -> None:
    path = os.path.join(app_dir, "ycgrid.toml")
    text = ""
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            text = f.read()
    if re.search(r"(?m)^auto_install\s*=", text):
        text = re.sub(r'(?m)^auto_install\s*=.*$', f'auto_install = "{value}"', text)
    elif "[python]" in text:
        text = text.replace("[python]", f'[python]\nauto_install = "{value}"', 1)
    else:
        text += f'\n[python]\nauto_install = "{value}"\n'
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
