# SPDX-License-Identifier: Apache-2.0
"""Open-source boundary check: run before a release (and in CI) on the files git tracks.

    python scripts/check_boundary.py [--extra patterns.txt] [--max-mb 20] [--history [REF ...]]

Flags what must not leave the private side: credentials (keys, tokens, passwords in code), private network
addresses and internal host names, personal paths, files that hold secrets (.env, .pem, .pfx, keys), large binaries,
and anything matching the extra patterns (one regular expression per line, e.g. broker account formats or internal
project names; keep that file outside the repository and pass it with --extra or YCGRID_BOUNDARY_PATTERNS).
A line that is a deliberate example says so with "boundary: ok" in a comment. Exit code 1 when something is found.
It also checks that every source file of YCGrid's own (C++, Python, .cmd, CMake; not third_party, not generated
files) carries the license line "SPDX-License-Identifier: Apache-2.0" in its first lines, and that no text file holds a
control character: a Bash heredoc turns a backslash and a letter into one (`scripts\\basics.py` became `scripts`, a
backspace, `asics.py`), and the file still reads fine almost everywhere.

--history also scans every line and file name the commits of the published branches ever added (main and dev by
default, or the refs given): a secret removed in a later commit is still in a public repository's history.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

RULES = [
    ("private key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----")),
    ("AWS access key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{36,}\b")),
    ("Slack token", re.compile(r"\bxox[abprs]-[A-Za-z0-9-]{10,}")),
    ("API key / token in code", re.compile(r"""(?i)\b(?:api[_-]?key|secret|token|passwd|password)\b\s*[:=]\s*["'][^"'\s]{8,}["']""")),
    ("bearer token", re.compile(r"(?i)\bbearer\s+[A-Za-z0-9._\-]{20,}")),
    ("private network address", re.compile(r"\b(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b")),
    ("internal host name", re.compile(r"(?i)\b[a-z0-9-]+\.(?:corp|internal|intranet|lan)\b")),
    ("personal path", re.compile(r"(?i)\b[A-Z]:\\Users\\(?!Public\\|Default\\|<|%|\{)[^\\\s\"']+\\")),
]
SECRET_FILES = re.compile(r"(?i)(^|/)(\.env(\..*)?|.*\.(pem|pfx|p12|key|keystore|jks)|id_rsa.*|credentials(\.json)?)$")
TEXT_EXT = {".py", ".cpp", ".h", ".hpp", ".c", ".cmd", ".bat", ".md", ".txt", ".json", ".toml", ".yaml", ".yml", ".ini", ".cfg",
            ".xml", ".html", ".js", ".ts", ".cmake", ".inc", ".rc", ".csv", ""}
SKIP_DIRS = ("third_party/",)
LICENSE_LINE = "SPDX-License-Identifier: Apache-2.0"
LICENSED_EXT = {".cpp", ".h", ".hpp", ".c", ".inc", ".py", ".cmd", ".bat", ".cmake"}
CONTROL = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1a\x1c-\x1f]")  # not tab, line ends or ESC


def lacks_license_line(rel: str, lines: list[str]) -> bool:
    """True for a source file of YCGrid's own whose first lines don't carry LICENSE_LINE (generated files are left out)."""
    name = rel.rsplit("/", 1)[-1]
    if rel.startswith(SKIP_DIRS) or (os.path.splitext(name)[1].lower() not in LICENSED_EXT and name != "CMakeLists.txt"):
        return False
    head = "".join(lines[:3]).lower()
    if "generated" in head or "do not edit" in head or "don't edit" in head:
        return False
    return not any(LICENSE_LINE in line for line in lines[:5])


def tracked_files(root: str = ROOT) -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True).stdout
    return [f for f in out.decode("utf-8").split("\0") if f]


def scan_line(rules, line: str):
    """(rule name, matched text) for each rule the line breaks."""
    if "boundary: ok" in line:
        return []
    return [(name, m.group(0)[:60]) for name, rx in rules for m in [rx.search(line)] if m]


def scan_history(rules, refs, root: str = ROOT):
    """Findings in what the commits reachable from refs added: (commit:path, line in that commit's file, rule, text)."""
    refs = [r for r in refs if subprocess.run(["git", "rev-parse", "--verify", "--quiet", r + "^{commit}"], cwd=root,
                                              capture_output=True).returncode == 0]
    if not refs:
        return []
    out = subprocess.run(["git", "log", "-p", "--unified=0", "--no-color", "--no-ext-diff", "--no-renames",
                          "--format=commit %H", *refs, "--", ".", *[":!" + d for d in SKIP_DIRS]],
                         cwd=root, capture_output=True, check=True).stdout.decode("utf-8", "replace")
    findings, seen = [], set()
    commit, path, line_no = "", "", 0
    for raw in out.split("\n"):
        raw = raw.rstrip("\r")
        if raw.startswith("commit "):
            commit = raw[7:19]
        elif raw.startswith("+++ "):
            path = raw[6:] if raw.startswith("+++ b/") else ""
            if path and SECRET_FILES.search(path) and path not in seen:
                seen.add(path)
                findings.append((f"{commit}:{path}", 0, "a file type that holds secrets", ""))
        elif raw.startswith("@@ "):
            m = re.search(r"\+(\d+)", raw)
            line_no = int(m.group(1)) if m else 0
        elif raw.startswith("+") and path:
            if os.path.splitext(path)[1].lower() in TEXT_EXT:
                for name, text in scan_line(rules, raw[1:]):
                    key = (path, name, text)
                    if key not in seen:  # a line kept through many commits is reported once, at its first commit
                        seen.add(key)
                        findings.append((f"{commit}:{path}", line_no, name, text))
            line_no += 1
    return findings


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--extra", default=os.environ.get("YCGRID_BOUNDARY_PATTERNS"), help="a file of extra regular expressions")
    ap.add_argument("--max-mb", type=float, default=20.0, help="larger tracked files are reported")
    ap.add_argument("--history", nargs="*", metavar="REF",
                    help="also scan what the commits of these refs ever added (default: main dev)")
    ns = ap.parse_args(argv)
    rules = list(RULES)
    if ns.extra:
        with open(ns.extra, encoding="utf-8") as f:
            for i, line in enumerate(f, 1):
                line = line.strip()
                if line and not line.startswith("#"):
                    rules.append((f"extra pattern {i}", re.compile(line)))
    findings = []
    for rel in tracked_files():
        if rel.startswith(SKIP_DIRS):
            continue
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue
        if SECRET_FILES.search(rel):
            findings.append((rel, 0, "a file type that holds secrets", ""))
        size = os.path.getsize(path)
        if size > ns.max_mb * 1e6:
            findings.append((rel, 0, f"a large file ({size / 1e6:.1f} MB)", ""))
        if os.path.splitext(rel)[1].lower() not in TEXT_EXT:
            continue
        try:
            with open(path, encoding="utf-8") as f:
                lines = f.readlines()
        except (UnicodeDecodeError, OSError):
            continue
        if lacks_license_line(rel, lines):
            findings.append((rel, 1, f"no license line ({LICENSE_LINE})", ""))
        for no, line in enumerate(lines, 1):
            for name, text in scan_line(rules, line):
                findings.append((rel, no, name, text))
            if CONTROL.search(line):
                findings.append((rel, no, "a control character (a heredoc that ate a backslash?)", repr(line.strip())[:80]))
    if ns.history is not None:
        findings += scan_history(rules, ns.history or ["main", "dev"])
    for rel, no, name, text in findings:
        where = f"{rel}:{no}" if no else rel
        print(f"{where}: {name}" + (f": {text}" if text else ""))
    print(f"[boundary] {len(findings)} finding(s) in {len(tracked_files())} tracked files"
          + (" and their history" if ns.history is not None else ""))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
