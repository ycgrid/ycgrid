# SPDX-License-Identifier: Apache-2.0
"""The per-directory summary of an OpenCppCoverage cobertura report (scripts\\coverage.cmd), and the least covered
files of src with 150+ lines: where unit tests are missing.

    python scripts\\coverage_summary.py build\\coverage\\debug\\cobertura.xml
"""
import os
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

path = sys.argv[1]
root = ET.parse(path).getroot()
files = {}
for cls in root.iter("class"):
    fn = cls.get("filename", "").replace("/", "\\")
    cov = tot = 0
    for ln in cls.iter("line"):
        tot += 1
        if int(ln.get("hits", "0")) > 0:
            cov += 1
    c0, t0 = files.get(fn, (0, 0))
    files[fn] = (c0 + cov, t0 + tot)

by_dir = defaultdict(lambda: [0, 0])
for fn, (c, t) in files.items():
    low = fn.lower()
    i = low.find("ycgrid\\")
    rel = fn[i + 7:] if i >= 0 else fn
    parts = rel.split("\\")
    d = "\\".join(parts[:2]) if parts[0] in ("src", "apps") else parts[0]
    by_dir[d][0] += c
    by_dir[d][1] += t

print(f"{'directory':<18}{'lines':>9}{'covered':>9}{'%':>7}")
tc = tt = 0
for d, (c, t) in sorted(by_dir.items()):
    tc += c
    tt += t
    print(f"{d:<18}{t:>9}{c:>9}{100 * c / t if t else 0:>6.0f}%")
print(f"{'TOTAL':<18}{tt:>9}{tc:>9}{100 * tc / tt if tt else 0:>6.0f}%")

print("\nleast covered files with 150+ lines (src only):")
rows = []
for fn, (c, t) in files.items():
    low = fn.lower()
    if "\\src\\" not in low or t < 150:
        continue
    i = low.find("ycgrid\\")
    rows.append((100 * c / t, t, c, fn[i + 7:]))
for pct, t, c, rel in sorted(rows)[:30]:
    print(f"  {pct:5.0f}%  {c:>5}/{t:<5}  {rel}")
