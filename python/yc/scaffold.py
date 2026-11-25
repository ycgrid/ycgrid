# SPDX-License-Identifier: Apache-2.0
"""Project templates (yc init, Alt+F11; PLAN §13 (a)(f)). Existing files are never overwritten."""
from __future__ import annotations

import json
import os

MAIN_PY = '''\
"""ThisWorkbook: workbook events and startup code (the first file YCGrid loads).

Load order: main.py -> sheets/*.py -> modules/*.py -> Workbook_Open()
Sheet CodeNames (Sheet1...) are global objects; no import needed.
"""
from yc.excel import *


def Workbook_Open():
    pass
'''

SHEET_PY = '''\
"""Sheet module of {name} (CodeName {code}): Worksheet_* events and control events."""
from yc.excel import *

Me = SheetModule(__name__)
Range, Cells, Rows, Columns = Me.Range, Me.Cells, Me.Rows, Me.Columns
'''

MODULE_PY = '''\
"""Standard module: macros (functions without parameters, runnable with Alt+F8), @udf, @stream."""
from yc.excel import *
'''

AGENTS_MD = '''\
# YCGrid project (development rules for AI agents)

This folder is a YCGrid project: the workbook (`.ycg`) and its Python code live together; copy the whole folder to another machine and it works.

## Where to start

1. **`main.py`** = Excel's ThisWorkbook: workbook events such as `Workbook_Open` and `Workbook_SheetChange`; builds the UI and registers per-cell OnChange at startup.
2. **`sheets/<CodeName>.py`**: sheet modules (`Worksheet_Change`, control events such as `CommandButton1_Click`).
3. **`modules/*.py`**: standard modules (macros = functions without parameters, `@udf`, `@stream`).
4. `lib/`: shared code (only imported); files starting with an underscore are not loaded automatically.

Load order: main.py -> sheets -> modules -> `Workbook_Open()`. YCGrid reloads automatically when you save: functions and classes are swapped in place and the running state stays, so `Workbook_Open` doesn't run again; do what it decides once (rebuilding the screen when `UI_VERSION` changed) in `Workbook_AfterReload(Files)` too.

## How to write code

- Always `from yc.excel import *`; names, parameters and 1-based coordinates match Excel: `Range("A1").Value`, `Cells(2, 3)`, `Worksheets("Quotes")`.
- Sheet CodeNames (`Sheet1`) are global objects; don't import them.
- Add new packages to `requirements.txt`; YCGrid detects missing packages and installs them into the project's runtime (the system Python is not touched).
- Use `decimal.Decimal` or `Range.ValueNs` for exact numbers and nanosecond timestamps.
- Object model reference: the YCGrid skill, `ycgrid-python` in `.claude/skills/` (Claude Code), `.agents/skills/` (Codex) and `.github/skills/` (GitHub Copilot), the same files in each. Building an app (a desk, a quote board, a strategy): follow the workflow in its `references/app-development.md` §0.
- The skill copies belong to YCGrid and are replaced with the version of the YCGrid that opens this project: keep project notes in this file, not in them.

## Checking your work

- The YCGrid MCP tools (`yc_*`, server `ycgrid` in `.mcp.json` / `.vscode/mcp.json`) operate the running YCGrid: reload with `yc_reload_python`, read the console with `yc_read_console`, check results with `yc_read_range` and `yc_screenshot`.

## Don't

- Don't modify `runtime/` (the dedicated Python runtime).
- Don't modify the workbook inside a UDF.
- Write large amounts of data with one array: `Range("A1:C1000").Value = rows`, not a cell-by-cell loop.
'''

EXTENSIONS_JSON = {"recommendations": ["ms-python.python", "ms-python.vscode-pylance", "anthropic.claude-code", "github.copilot"]}

def _yc_parent() -> str:
    """Folder that contains the yc package (release: python\\ next to ycgrid.exe; development: python\\ in the repo)."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _paths_for(app_dir: str):
    """Returns (interpreter, yc folder); paths inside the project folder are relative, so the folder can be moved."""
    import sys

    py = os.path.join(app_dir, "runtime", "python", "python.exe")
    if not os.path.isfile(py):
        py = sys.executable

    def rel(p):
        try:
            r = os.path.relpath(p, app_dir)
        except ValueError:  # different drive
            return p
        return p if r.startswith("..") else r

    return rel(py), rel(_yc_parent())


def mcp_json(app_dir: str) -> dict:
    """.mcp.json: starts yc.mcp (stdio) with the project's portable Python."""
    py, yc_dir = _paths_for(app_dir)
    boot = f"import sys;sys.path.insert(0,r'{yc_dir}');from yc.mcp.server import main;sys.exit(main())"
    return {"mcpServers": {"ycgrid": {"command": py, "args": ["-E", "-s", "-c", boot]}}}

TOML = '''\
# YCGrid project settings
[python]
version = "3.12"
auto_install = "ask"   # ask: ask first; always: install automatically; never: don't install
# index_url = "https://pypi.example.com/simple"   # a private package index
# find_links = "runtime/wheels"   # wheels for offline installation
'''

PACK_CMD = '''\
@echo off
chcp 65001 >nul
rem Pack this project in one step: dist\\<project>-<date>.zip (ycgrid.exe, runtime, code and workbook)
setlocal
set "HERE=%~dp0"
set "PY=%HERE%runtime\\python\\python.exe"
if not exist "%PY%" set "PY={py}"
if not exist "%PY%" (
  echo [YCGrid] runtime\\python\\python.exe not found
  pause
  exit /b 1
)
"%PY%" -E -s -c "import sys;sys.path[0:0]=[r'%HERE%python',r'{yc_dir}'];from yc.cli import main;sys.exit(main())" pack "%HERE%." %*
if errorlevel 1 pause
endlocal
'''


def _write(path: str, content, created: list[str]) -> None:
    if os.path.exists(path):
        return
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    if isinstance(content, (dict, list)):
        content = json.dumps(content, ensure_ascii=False, indent=2) + "\n"
    newline = "\r\n" if path.endswith(".cmd") else "\n"
    with open(path, "w", encoding="utf-8", newline=newline) as f:
        f.write(content)
    created.append(path)


# Where each coding agent reads project skills: Claude Code .claude/skills, Codex .agents/skills (it reads no other
# folder), GitHub Copilot .github/skills (it also reads the other two).
SKILL_TARGETS = (os.path.join(".claude", "skills", "ycgrid-python"), os.path.join(".agents", "skills", "ycgrid-python"),
                 os.path.join(".github", "skills", "ycgrid-python"))
CODEX_SKILL = SKILL_TARGETS[1]


def ycgrid_skill_dir() -> str:
    """The skill that ships with this YCGrid: <YCGrid>/skill/ycgrid-python, beside the python folder this package is in."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "skill", "ycgrid-python"))


def sync_skill(app_dir: str, skill_dir: str | None = None) -> list[str]:
    """Bring the project's copies of the YCGrid skill (SKILL_TARGETS) up to this YCGrid's version:
    changed and new files are copied, files the skill no longer has are removed. AI agents in the project then read the
    rules of the YCGrid they work with (the copies used to stay as they were when the project was made). The copies
    belong to YCGrid: keep project notes in AGENTS.md / CLAUDE.md. A copy the project doesn't have isn't created.
    Returns the paths written or removed."""
    src = skill_dir or ycgrid_skill_dir()
    if not os.path.isfile(os.path.join(src, "SKILL.md")):
        return []
    want = {}
    for base, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        for f in files:
            p = os.path.join(base, f)
            want[os.path.normcase(os.path.relpath(p, src))] = (os.path.relpath(p, src), p)
    changed: list[str] = []
    for rel in SKILL_TARGETS:
        dst = os.path.join(app_dir, rel)
        if not os.path.isdir(dst) or os.path.normcase(os.path.abspath(dst)) == os.path.normcase(os.path.abspath(src)):
            continue
        for r, p in want.values():
            t = os.path.join(dst, r)
            with open(p, "rb") as f:
                data = f.read()
            try:
                with open(t, "rb") as f:
                    same = f.read() == data
            except OSError:
                same = False
            if not same:
                os.makedirs(os.path.dirname(t), exist_ok=True)
                with open(t, "wb") as f:
                    f.write(data)
                changed.append(t)
        for base, dirs, files in os.walk(dst):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for f in files:
                t = os.path.join(base, f)
                if os.path.normcase(os.path.relpath(t, dst)) not in want:
                    os.remove(t)
                    changed.append(t)
    return changed


def init_project(app_dir: str, sheets: list[tuple[str, str]] | None = None, skill_dir: str | None = None) -> list[str]:
    """sheets: [(CodeName, sheet name)]. Returns the files created."""
    created: list[str] = []
    sheets = sheets or [("Sheet1", "Sheet1")]
    _write(os.path.join(app_dir, "main.py"), MAIN_PY, created)
    for code, name in sheets:
        _write(os.path.join(app_dir, "sheets", f"{code}.py"), SHEET_PY.format(code=code, name=name), created)
    _write(os.path.join(app_dir, "modules", "Module1.py"), MODULE_PY, created)
    _write(os.path.join(app_dir, "requirements.txt"), "# Python packages this project needs (one per line), for example:\n# numpy\n# pandas\n", created)
    _write(os.path.join(app_dir, "ycgrid.toml"), TOML, created)
    _write(os.path.join(app_dir, "AGENTS.md"), AGENTS_MD, created)
    _write(os.path.join(app_dir, "CLAUDE.md"), AGENTS_MD, created)
    _write(os.path.join(app_dir, ".vscode", "extensions.json"), EXTENSIONS_JSON, created)
    mj = mcp_json(app_dir)
    _write(os.path.join(app_dir, ".mcp.json"), mj, created)
    _write(os.path.join(app_dir, ".vscode", "mcp.json"), {"servers": mj["mcpServers"]}, created)
    # Claude Code: use the project's YCGrid MCP server without asking
    _write(os.path.join(app_dir, ".claude", "settings.json"), {"enableAllProjectMcpServers": True}, created)
    import sys

    _write(os.path.join(app_dir, "pack.cmd"), PACK_CMD.replace("{yc_dir}", _yc_parent()).replace("{py}", sys.executable), created)
    _write(os.path.join(app_dir, ".gitignore"), "runtime/\n__pycache__/\n.ycgrid/\ndist/\n*.tmp~\n", created)
    created += ensure_dev_env(app_dir, sheets)
    skill_dir = skill_dir or ycgrid_skill_dir()
    if skill_dir and os.path.isdir(skill_dir):
        import shutil

        for rel in SKILL_TARGETS:
            target = os.path.join(app_dir, rel)
            if not os.path.exists(target):
                shutil.copytree(skill_dir, target)
                created.append(target)
    return created


def write_typings(app_dir: str, sheets: list[tuple[str, str]]) -> None:
    """__builtins__.pyi (project root; Pylance only reads it there): lets the editor know the CodeName globals (Sheet1...)."""
    lines = ['"""Generated by YCGrid: sheet CodeNames are global objects. Updated by the VS Code button after you add sheets."""',
             "from yc.excel import Worksheet", ""]
    for code, name in sheets:
        if code.isidentifier():
            lines.append(f"{code}: Worksheet  # {name}")
    text = "\n".join(lines) + "\n"
    path = os.path.join(app_dir, "__builtins__.pyi")
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    # the old location under typings\ (Pylance never reads it)
    old = os.path.join(app_dir, "typings", "__builtins__.pyi")
    if os.path.isfile(old):
        os.remove(old)


def _load_json(path: str):
    """Read VS Code JSON (// comments and trailing commas allowed); returns None when it can't be read (the user's file is not overwritten)."""
    import re

    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return {}
    out, i, n, in_str = [], 0, len(text), False
    while i < n:  # strip comments (not inside strings)
        ch = text[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            out.append(ch)
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
            continue
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        else:
            out.append(ch)
        i += 1
    cleaned = re.sub(r",(\s*[}\]])", r"\1", "".join(out))
    try:
        data = json.loads(cleaned) if cleaned.strip() else {}
    except ValueError:
        return None
    return data if isinstance(data, dict) else None


def _dump_json(path: str, data: dict) -> bool:
    text = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return True


# checks that don't produce squiggles: only keep what really fails at run time (undefined names, missing modules, syntax errors)
PYRIGHT_RULES = {
    "typeCheckingMode": "basic",
    # the YCGrid object model is dynamic (properties live in YCGrid), so don't report what type inference can't know
    "reportAttributeAccessIssue": "none",
    "reportOptionalMemberAccess": "none",
    "reportOptionalSubscript": "none",
    "reportOptionalIterable": "none",
    "reportOptionalCall": "none",
    "reportOptionalOperand": "none",
    "reportCallIssue": "none",
    "reportArgumentType": "none",
    "reportIndexIssue": "none",
    "reportOperatorIssue": "none",
    "reportReturnType": "none",
    "reportAssignmentType": "none",
    "reportGeneralTypeIssues": "none",
    "reportRedeclaration": "none",
    "reportSelfClsParameterName": "none",
    "reportFunctionMemberAccess": "none",
    "reportUnsupportedDunderAll": "none",
    "reportWildcardImportFromLibrary": "none",
    "reportMissingModuleSource": "none",
    "reportTypedDictNotRequiredAccess": "none",
    "reportPrivateImportUsage": "none",
    # real errors: keep
    "reportUndefinedVariable": "error",
    "reportMissingImports": "error",
}


def ensure_dev_env(app_dir: str, sheets: list[tuple[str, str]] | None = None) -> list[str]:
    """VS Code / Pylance development environment: runs every time the VS Code button is pressed (existing settings are merged, your other settings are kept).

    - pyrightconfig.json: where the yc package is, which checks to keep (Pylance uses this file)
    - .vscode\\settings.json: the interpreter points at YCGrid's portable Python
    - __builtins__.pyi: CodeName globals
    """
    written: list[str] = []
    py, yc_dir = _paths_for(app_dir)

    def ws(p: str) -> str:
        return p if os.path.isabs(p) else "${workspaceFolder}/" + p.replace("\\", "/")

    from . import env

    # [python] paths in ycgrid.toml (an example's ../lib), relative to the project as the host imports them
    shared = []
    for p in env.project_paths(app_dir):
        try:
            shared.append(os.path.relpath(p, app_dir).replace("\\", "/"))
        except ValueError:  # another drive
            shared.append(p.replace("\\", "/"))
    cfg_path = os.path.join(app_dir, "pyrightconfig.json")
    cfg = _load_json(cfg_path)
    if cfg is not None:
        cfg.update({
            "pythonVersion": "3.12",
            "pythonPlatform": "Windows",
            "extraPaths": [yc_dir.replace("\\", "/"), "lib"] + shared,
            "exclude": ["runtime", "**/__pycache__", ".ycgrid", "dist", ".claude", ".agents", ".github"],
        })
        for k, v in PYRIGHT_RULES.items():
            cfg.setdefault(k, v)
        if _dump_json(cfg_path, cfg):
            written.append(cfg_path)

    st_path = os.path.join(app_dir, ".vscode", "settings.json")
    st = _load_json(st_path)
    if st is not None:
        st["python.defaultInterpreterPath"] = ws(py)
        st["python.analysis.extraPaths"] = [ws(yc_dir), "${workspaceFolder}/lib"] + [ws(p) for p in shared]
        st.setdefault("python.analysis.diagnosticMode", "openFilesOnly")
        st.setdefault("python.languageServer", "Default")
        # open without prompts: no "install the recommended extensions?" or "open the Git repository in a parent folder?"
        st.setdefault("extensions.ignoreRecommendations", True)
        st.setdefault("git.openRepositoryInParentFolders", "never")
        ex = st.setdefault("files.exclude", {})
        if isinstance(ex, dict):
            for k in ("runtime", "**/__pycache__", ".ycgrid", "__builtins__.pyi", "pyrightconfig.json"):
                ex.setdefault(k, True)
        if _dump_json(st_path, st):
            written.append(st_path)
    ext_path = os.path.join(app_dir, ".vscode", "extensions.json")
    if not os.path.exists(ext_path) and _dump_json(ext_path, EXTENSIONS_JSON):
        written.append(ext_path)
    if sheets:
        write_typings(app_dir, sheets)
    # projects made before Codex's copy existed: the VS Code button adds it beside the copies they have
    codex = os.path.join(app_dir, CODEX_SKILL)
    if not os.path.exists(codex) and any(os.path.isdir(os.path.join(app_dir, t)) for t in SKILL_TARGETS):
        os.makedirs(codex, exist_ok=True)
        written.append(codex)
    written += sync_skill(app_dir)   # the rules of the YCGrid that opens the editor
    return written


def add_sheet_module(app_dir: str, code: str, name: str) -> str:
    path = os.path.join(app_dir, "sheets", f"{code}.py")
    created: list[str] = []
    _write(path, SHEET_PY.format(code=code, name=name), created)
    return path
