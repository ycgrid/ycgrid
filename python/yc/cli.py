# SPDX-License-Identifier: Apache-2.0
"""yc command line (python -m yc ...): open, run, init, devenv, pack, python."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile


def cmd_init(ns) -> int:
    from . import scaffold

    sheets = []
    for item in ns.sheet or []:
        code, _, name = item.partition("=")
        sheets.append((code, name or code))
    created = scaffold.init_project(os.path.abspath(ns.dir), sheets or None, ns.skill)
    print(json.dumps({"created": created}, ensure_ascii=False))
    return 0


def cmd_devenv(ns) -> int:
    """VS Code development environment (Pylance settings, yc package path, CodeName types). Run by the VS Code button every time."""
    from . import scaffold

    sheets = []
    for item in ns.sheet or []:
        code, _, name = item.partition("=")
        sheets.append((code, name or code))
    written = scaffold.ensure_dev_env(os.path.abspath(ns.dir), sheets or None)
    print(json.dumps({"written": written}, ensure_ascii=False))
    return 0


EXCLUDE_DIRS = {".ycgrid", "__pycache__", ".git", "dist", ".venv", "_work", "_uvcache"}


def cmd_pack(ns) -> int:
    root = os.path.abspath(ns.dir)
    name = os.path.basename(root.rstrip("\\/")) or "project"
    out_dir = os.path.join(root, "dist")
    os.makedirs(out_dir, exist_ok=True)
    zip_path = os.path.join(out_dir, f"{name}-{dt.date.today():%Y%m%d}.zip")
    count = 0
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for base, dirs, files in os.walk(root):
            rel = os.path.relpath(base, root)
            parts = set(rel.split(os.sep))
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS and not (d == "wheels" and not ns.with_wheels)]
            if parts & EXCLUDE_DIRS:
                continue
            if not ns.with_dev and rel.split(os.sep)[0] in (".claude", ".github", ".agents", "examples"):
                continue
            for f in files:
                if f.endswith((".tmp~", ".pyc")):
                    continue
                full = os.path.join(base, f)
                z.write(full, os.path.join(name, os.path.relpath(full, root)))
                count += 1
        # [python] paths outside the folder (an example's ..\lib) go into the copy's lib\, which is always imported
        from . import env

        packed = {n.lower() for n in z.namelist()}  # the archive's names use "/"
        for extra in env.project_paths(root):
            if os.path.normcase(extra + os.sep).startswith(os.path.normcase(root + os.sep)):
                continue  # inside the project: packed above
            for base, dirs, files in os.walk(extra):
                dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
                for f in files:
                    if f.endswith((".tmp~", ".pyc")):
                        continue
                    full = os.path.join(base, f)
                    arc = os.path.join(name, "lib", os.path.relpath(full, extra))
                    key = arc.replace(os.sep, "/").lower()
                    if key not in packed:  # the project's own lib\ file wins
                        z.write(full, arc)
                        packed.add(key)
                        count += 1
    print(f"Packed {count} files: {zip_path}")
    # smoke test: extract to a temporary folder and run once with --frames
    exe = os.path.join(root, "ycgrid.exe")
    if os.path.isfile(exe) and not ns.no_test:
        tmp = tempfile.mkdtemp(prefix="yc_pack_")
        try:
            with zipfile.ZipFile(zip_path) as z:
                z.extractall(tmp)
            test_exe = os.path.join(tmp, name, "ycgrid.exe")
            r = subprocess.run([test_exe, "--frames", "30", "--warp", "--no-handoff"], timeout=120)
            print("Smoke test: " + ("passed" if r.returncode == 0 else f"failed ({r.returncode})"))
        except (OSError, subprocess.TimeoutExpired) as e:
            print(f"The smoke test couldn't run: {e}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    return 0


def cmd_python(ns) -> int:
    info = {"version": sys.version.split()[0], "executable": sys.executable, "prefix": sys.prefix}
    if ns.action == "info":
        print(json.dumps(info, ensure_ascii=False))
        return 0
    if ns.action == "upgrade":
        uv = os.path.join(os.path.dirname(sys.prefix), "uv.exe")
        if not os.path.isfile(uv):
            print("runtime\\uv.exe not found", file=sys.stderr)
            return 1
        runtime = os.path.dirname(sys.prefix)
        target = os.path.join(runtime, f"python-{ns.version}")
        work = os.path.join(runtime, "_work")
        env = dict(os.environ, UV_PYTHON_INSTALL_DIR=os.path.join(work, "python"), UV_CACHE_DIR=os.path.join(work, "cache"))
        r = subprocess.run([uv, "python", "install", ns.version, "--install-dir", os.path.join(work, "python"), "--no-bin", "--no-registry"], env=env)
        if r.returncode != 0:
            return r.returncode
        print(f"Downloaded Python {ns.version}; set [python].version = \"{ns.version}\" in ycgrid.toml and rename {target} to runtime\\python (keep the old one to roll back).")
        return 0
    return 1


def cmd_open(ns) -> int:
    """Start YCGrid (with a workbook) and return."""
    import subprocess

    import yc

    args = [yc.find_exe(), "--no-handoff"] + ([os.path.abspath(ns.file)] if ns.file else [])
    env = dict(os.environ)
    env.setdefault("YCGRID_PYTHON", sys.executable)
    subprocess.Popen(args, env=env)
    return 0


def cmd_run(ns) -> int:
    """Start YCGrid (or use the running one) and run a script connected to it, like a data feed or a research notebook."""
    import runpy

    import yc

    try:
        yc.connect()
    except yc.YcConnectionError:
        yc.launch(ns.workbook, hidden=ns.hidden)
    sys.argv = [ns.script] + list(ns.args)
    runpy.run_path(ns.script, run_name="__main__")
    return 0


def open_main() -> int:
    """The `ycgrid` command of the wheel: ycgrid [workbook]."""
    return cmd_open(argparse.Namespace(file=sys.argv[1] if len(sys.argv) > 1 else None))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="yc")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("open", help="Start YCGrid, optionally with a workbook")
    p.add_argument("file", nargs="?")
    p.set_defaults(fn=cmd_open)
    p = sub.add_parser("run", help="Run a Python script connected to YCGrid (started when none is running)")
    p.add_argument("script")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.add_argument("--workbook", help="the workbook to open when YCGrid is started")
    p.add_argument("--hidden", action="store_true", help="start YCGrid without a window (tests)")
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("init", help="Create a YCGrid project template in a folder")
    p.add_argument("dir")
    p.add_argument("--sheet", action="append", help="CodeName=sheet name")
    p.add_argument("--skill", help="skill folder to copy into the project")
    p.set_defaults(fn=cmd_init)
    p = sub.add_parser("devenv", help="Update the VS Code development settings (Pylance, yc package path, CodeName types)")
    p.add_argument("dir")
    p.add_argument("--sheet", action="append", help="CodeName=sheet name")
    p.set_defaults(fn=cmd_devenv)
    p = sub.add_parser("pack", help="Pack the project into a zip")
    p.add_argument("dir")
    p.add_argument("--with-wheels", action="store_true")
    p.add_argument("--with-dev", action="store_true")
    p.add_argument("--no-test", action="store_true")
    p.set_defaults(fn=cmd_pack)
    p = sub.add_parser("python", help="Python runtime")
    p.add_argument("action", choices=["info", "upgrade"])
    p.add_argument("version", nargs="?", default="3.13")
    p.set_defaults(fn=cmd_python)
    ns = ap.parse_args(argv)
    return ns.fn(ns)


if __name__ == "__main__":
    sys.exit(main())
