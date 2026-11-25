# SPDX-License-Identifier: Apache-2.0
"""Build the ycgrid wheel: the yc package with ycgrid.exe inside (yc/_bin), for people who use their own Python.

Usage (scripts\\build_wheel.cmd runs it with the bundled Python):
    python tools/make_wheel.py <dist folder> <output folder>

The wheel is win_amd64 for any Python 3 (yc is pure Python; the exe is a separate program). After
`pip install ycgrid-*.whl`: `ycgrid` (or python -m yc open) starts YCGrid, `python -m yc run script.py` runs a script
connected to it, and the Python host of YCGrid runs on that same Python.
"""
from __future__ import annotations

import base64
import hashlib
import os
import pathlib
import re
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
NAME = "ycgrid"
REPO = "https://github.com/ycgrid/ycgrid"
# PyPI shows the README without the repository around it: relative links and images point at the released branch
BLOB = REPO + "/blob/main/"
RAW = "https://raw.githubusercontent.com/ycgrid/ycgrid/main/"
LINK = re.compile(r"(!?)\[([^\]]*)\]\(([^)\s]+)\)")


def pypi_readme(text: str) -> str:
    """README.md with its relative links and images made absolute, for the package's long description."""
    def absolute(m: re.Match) -> str:
        bang, label, url = m.groups()
        if re.match(r"[a-z][a-z0-9+.-]*:|#", url, re.I):
            return m.group(0)
        return f"{bang}[{label}]({(RAW if bang else BLOB) + url.removeprefix('./')})"
    return LINK.sub(absolute, text)


def version() -> str:
    text = (ROOT / "python" / "yc" / "__init__.py").read_text(encoding="utf-8")
    return re.search(r'__version__ = "([^"]+)"', text).group(1)


def record_line(arc: str, data: bytes) -> str:
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return f"{arc},sha256={digest},{len(data)}"


def build(dist: pathlib.Path, out: pathlib.Path) -> pathlib.Path:
    ver = version()
    tag = "py3-none-win_amd64"
    out.mkdir(parents=True, exist_ok=True)
    whl = out / f"{NAME}-{ver}-{tag}.whl"
    info = f"{NAME}-{ver}.dist-info"
    files: list[tuple[str, bytes]] = []
    pkg = ROOT / "python" / "yc"
    for p in sorted(pkg.rglob("*")):
        if p.is_dir() or "__pycache__" in p.parts or p.suffix in (".pyc", ".pyo"):
            continue
        files.append(("yc/" + p.relative_to(pkg).as_posix(), p.read_bytes()))
    exe = dist / "ycgrid.exe"
    if not exe.is_file():
        raise SystemExit(f"{exe} doesn't exist: build first (scripts\\build.cmd release)")
    files.append(("yc/_bin/ycgrid.exe", exe.read_bytes()))
    for extra in ("skill",):  # the agent skill ships with the package too
        src = dist / extra
        if src.is_dir():
            for p in sorted(src.rglob("*")):
                if p.is_file():
                    files.append((f"yc/_bin/{extra}/" + p.relative_to(src).as_posix(), p.read_bytes()))
    licenses = sorted(p for p in (dist / "licenses").glob("*") if p.is_file()) if (dist / "licenses").is_dir() else []
    if not any(p.name == "ycgrid-LICENSE.txt" for p in licenses):
        raise SystemExit(f"{dist / 'licenses'} has no ycgrid-LICENSE.txt: build first (scripts\\build.cmd release)")
    # every other license file is one THIRD-PARTY-NOTICES.md lists: a file an older build left behind is refused
    notices = (ROOT / "THIRD-PARTY-NOTICES.md").read_text(encoding="utf-8")
    stray = [p.name for p in licenses
             if p.name not in ("ycgrid-LICENSE.txt", "NOTICE.txt", "THIRD-PARTY-NOTICES.md") and f"`{p.name}`" not in notices]
    if stray:
        raise SystemExit(f"THIRD-PARTY-NOTICES.md doesn't list {', '.join(stray)} (in {dist / 'licenses'}): "
                         "add the component there, or rebuild if it is left from an older build")
    readme = pypi_readme((ROOT / "README.md").read_text(encoding="utf-8"))
    meta = "\n".join([
        "Metadata-Version: 2.4",
        f"Name: {NAME}",
        f"Version: {ver}",
        "Summary: An Excel-compatible spreadsheet for trading and research, driven by Python (Windows)",
        "License-Expression: Apache-2.0",
        *[f"License-File: {p.name}" for p in licenses],
        "Keywords: spreadsheet,excel,trading,quant,real-time,dear-imgui,mcp,ai-agents",
        "Requires-Python: >=3.10",
        f"Project-URL: Homepage, {REPO}",
        f"Project-URL: Documentation, {REPO}#documentation",
        f"Project-URL: Source, {REPO}",
        f"Project-URL: Issues, {REPO}/issues",
        f"Project-URL: Changelog, {REPO}/blob/main/CHANGELOG.md",
        "Classifier: Environment :: Win32 (MS Windows)",
        "Classifier: Intended Audience :: Financial and Insurance Industry",
        "Classifier: Intended Audience :: Science/Research",
        "Classifier: Operating System :: Microsoft :: Windows",
        "Classifier: Programming Language :: C++",
        "Classifier: Programming Language :: Python :: 3",
        "Classifier: Programming Language :: Python :: 3 :: Only",
        "Classifier: Topic :: Office/Business :: Financial :: Spreadsheet",
        "Provides-Extra: data",
        "Requires-Dist: numpy>=1.24; extra == \"data\"",
        "Requires-Dist: pandas>=2.0; extra == \"data\"",
        "Description-Content-Type: text/markdown",
        "",
        readme,
    ])
    files.append((f"{info}/METADATA", meta.encode("utf-8")))
    files.append((f"{info}/WHEEL", "\n".join(["Wheel-Version: 1.0", "Generator: ycgrid make_wheel", "Root-Is-Purelib: false",
                                               f"Tag: {tag}", ""]).encode()))
    files.append((f"{info}/entry_points.txt", b"[console_scripts]\nycgrid = yc.cli:open_main\n"))
    files.append((f"{info}/top_level.txt", b"yc\n"))
    for lic in licenses:
        files.append((f"{info}/licenses/{lic.name}", lic.read_bytes()))
    lines = [record_line(a, d) for a, d in files] + [f"{info}/RECORD,,"]
    files.append((f"{info}/RECORD", ("\n".join(lines) + "\n").encode()))
    with zipfile.ZipFile(whl, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, data in files:
            z.writestr(arc, data)
    return whl


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    w = build(pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve())
    print(f"{w} ({os.path.getsize(w) / 1e6:.1f} MB)")
