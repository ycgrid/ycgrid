# SPDX-License-Identifier: Apache-2.0
"""YCGrid's MCP server (stdio, JSON-RPC 2.0, one message per line).

    runtime\\python\\python.exe -E -s -m yc.mcp

The single source of the tool definitions is mcp/tools.json in the repo (copied to yc/mcp/tools.json in releases);
the implementation is yc.mcp.tools. Tools work on YCGrid through their own controller connection, not through the Python host.
"""
from __future__ import annotations

import io
import json
import os
import sys
import threading
import traceback
from pathlib import Path
from typing import Any

PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")


def load_spec() -> dict:
    here = Path(__file__).resolve()
    for cand in (here.with_name("tools.json"), here.parents[3] / "mcp" / "tools.json"):
        if cand.is_file():
            with open(cand, encoding="utf-8") as f:
                return json.load(f)
    raise FileNotFoundError("tools.json not found (yc/mcp/tools.json or mcp/tools.json in the repo)")


class Server:
    def __init__(self, out=None):
        from . import tools

        self.spec = load_spec()
        self.tools = tools
        self._out = out or sys.stdout.buffer
        self._wlock = threading.Lock()

    # ---- Transport ----
    def send(self, msg: dict) -> None:
        data = json.dumps(msg, ensure_ascii=False, default=_json_default).encode("utf-8") + b"\n"
        with self._wlock:
            self._out.write(data)
            self._out.flush()

    def serve(self, inp=None) -> None:
        inp = inp or sys.stdin.buffer
        for raw in inp:
            line = raw.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                self.send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "JSON parse error"}})
                continue
            for m in msg if isinstance(msg, list) else [msg]:
                self.handle(m)

    # ---- Dispatch ----
    def handle(self, m: dict) -> None:
        mid = m.get("id")
        method = m.get("method")
        if method is None:  # replies (we never send requests)
            return
        try:
            result = self.dispatch(method, m.get("params") or {})
        except _RpcError as e:
            if mid is not None:
                self.send({"jsonrpc": "2.0", "id": mid, "error": {"code": e.code, "message": str(e)}})
            return
        except Exception as e:  # noqa: BLE001
            if mid is not None:
                self.send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32603, "message": f"{type(e).__name__}: {e}"}})
            return
        if mid is not None:
            self.send({"jsonrpc": "2.0", "id": mid, "result": result})

    def dispatch(self, method: str, params: dict) -> Any:
        if method == "initialize":
            req = params.get("protocolVersion")
            ver = req if req in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
            srv = self.spec.get("server", {})
            from .. import __version__

            return {"protocolVersion": ver, "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": srv.get("name", "YCGrid"), "title": srv.get("title", "YCGrid"), "version": __version__},
                    "instructions": srv.get("instructions", "")}
        if method == "ping":
            return {}
        if method.startswith("notifications/"):
            return None
        if method == "tools/list":
            out = []
            for t in self.spec["tools"]:
                d = {"name": t["name"], "description": t["description"], "inputSchema": t["inputSchema"]}
                for k in ("title", "annotations"):
                    if k in t:
                        d[k] = t[k]
                out.append(d)
            return {"tools": out}
        if method == "tools/call":
            return self.call_tool(params.get("name", ""), params.get("arguments") or {})
        if method in ("resources/list", "resources/templates/list"):
            return {"resources": []} if method == "resources/list" else {"resourceTemplates": []}
        if method == "prompts/list":
            return {"prompts": []}
        raise _RpcError(-32601, f"Unsupported method: {method}")

    def call_tool(self, name: str, args: dict) -> dict:
        fn = self.tools.HANDLERS.get(name)
        if fn is None:
            raise _RpcError(-32602, f"Unknown tool: {name}")
        # print output of user code (exec, macros) must never go to stdout (that is the MCP channel)
        real_stdout = sys.stdout
        sys.stdout = io.StringIO()
        try:
            res = fn(**args)
            content = self.tools.to_content(res)
            return {"content": content, "isError": False}
        except self.tools.ToolError as e:
            return {"content": [{"type": "text", "text": str(e)}], "isError": True}
        except TypeError as e:
            return {"content": [{"type": "text", "text": f"Invalid arguments: {e}"}], "isError": True}
        except Exception as e:  # noqa: BLE001
            text = self.tools.describe_error(e)
            if os.environ.get("YC_MCP_DEBUG"):
                text += "\n" + traceback.format_exc()
            return {"content": [{"type": "text", "text": text}], "isError": True}
        finally:
            sys.stdout = real_stdout


class _RpcError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def _json_default(o):
    from decimal import Decimal

    if isinstance(o, Decimal):
        return str(o)
    if hasattr(o, "isoformat"):
        return o.isoformat()
    return repr(o)


def main(argv=None) -> int:
    # stdout is the MCP channel: every other output goes to stderr
    out = sys.stdout.buffer
    sys.stdout = sys.stderr
    target = None
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--target" in argv:
        i = argv.index("--target")
        target = argv[i + 1] if i + 1 < len(argv) else None
    srv = Server(out)
    if target:
        srv.tools.STATE["target"] = target
    srv.serve()
    return 0
