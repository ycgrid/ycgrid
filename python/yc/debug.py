# SPDX-License-Identifier: Apache-2.0
"""[YC] Debug the running YCGrid's Python in VS Code (Developer > Debug in VS Code).

    import yc.debug
    yc.debug.start()          # in the YCGrid host: listen for VS Code, write .vscode/launch.json

Then in VS Code: Run and Debug > "Attach to YCGrid" (F5). Breakpoints stop strategies, event handlers, UDFs and cells
running in YCGrid while the YCGrid window keeps drawing live data (the UI never waits for Python).
"""
from __future__ import annotations

import json
import os
import sys

CONFIG_NAME = "Attach to YCGrid"
_listening: dict = {}


def _ensure_debugpy():
    # Python 3.12's frozen modules make pydevd print a "Debugger warning" on stderr, which the Error Log shows as an
    # error every time debugging starts; breakpoints in the user's modules work either way
    os.environ.setdefault("PYDEVD_DISABLE_FILE_VALIDATION", "1")
    try:
        import debugpy  # noqa: F401
    except ImportError:
        import tempfile

        from . import env

        with tempfile.TemporaryDirectory() as scratch:  # not a project: no requirements.txt is written
            if not env.install(scratch, ["debugpy"]):
                raise
    import debugpy

    return debugpy


def listen(port: int = 5678, host: str = "127.0.0.1") -> int:
    """Start the debug server in this process (once); returns the port VS Code attaches to."""
    if _listening:
        return _listening["port"]
    debugpy = _ensure_debugpy()
    # the adapter is a separate process: it must run on a real interpreter, not an embedding host
    debugpy.configure(python=sys.executable)
    real_port = _listen_on(debugpy, host, int(port))
    _listening.update(host=host, port=real_port)
    return real_port


def _listen_on(debugpy, host: str, port: int) -> int:
    """The port asked for; when another program has it (another YCGrid, a debug session), one the system picks:
    launch.json gets the port actually used."""
    try:
        _, real = debugpy.listen((host, port))
    except (RuntimeError, OSError) as e:
        if port == 0 or ("10048" not in str(e) and "in use" not in str(e).lower()):
            raise
        _, real = debugpy.listen((host, 0))
    return real


def launch_config(port: int, host: str = "127.0.0.1") -> dict:
    return {"name": CONFIG_NAME, "type": "debugpy", "request": "attach", "connect": {"host": host, "port": int(port)}, "justMyCode": True}


def write_launch_json(folder: str, port: int, host: str = "127.0.0.1") -> str:
    """Add (or update) the "Attach to YCGrid" configuration in folder/.vscode/launch.json; other configurations stay."""
    vs = os.path.join(folder, ".vscode")
    os.makedirs(vs, exist_ok=True)
    path = os.path.join(vs, "launch.json")
    data = {"version": "0.2.0", "configurations": []}
    if os.path.isfile(path):
        try:
            with open(path, encoding="utf-8") as f:
                text = "\n".join(ln for ln in f.read().splitlines() if not ln.lstrip().startswith("//"))
            data = json.loads(text) or data
        except (OSError, ValueError):
            pass  # a file we can't read is replaced
    cfgs = [c for c in data.get("configurations", []) if c.get("name") != CONFIG_NAME]
    data["configurations"] = [launch_config(port, host)] + cfgs
    data.setdefault("version", "0.2.0")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return path


def start(port: int = 5678, folder: str | None = None) -> dict:
    """Listen for VS Code and write the attach configuration into the project folder (the host's folder by default)."""
    real = listen(port)
    if folder is None:
        from . import host

        folder = getattr(host, "_app_dir", None) or os.getcwd()
    path = write_launch_json(folder, real)
    print(f"[debug] VS Code can attach on port {real}: Run and Debug > \"{CONFIG_NAME}\" ({path})")
    return {"port": real, "launch_json": path, "folder": folder}
