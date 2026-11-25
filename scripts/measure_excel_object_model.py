# SPDX-License-Identifier: Apache-2.0
r"""Excel's object model as Excel itself lists it: the members of every object named in schema/excel_parity.toml, read
from the type library inside EXCEL.EXE, written to tests/data/excel_object_model.json.

    <YCGrid>\runtime\python\python.exe -E -s scripts\measure_excel_object_model.py --comtypes <dir> [--excel EXCEL.EXE]

No Excel is started: the type library is read from the file. It needs comtypes (pure Python): `pip install comtypes
--target <dir>` and give the folder with --comtypes, as for scripts\excel_diff.py. Members Excel hides (FUNCFLAG_FHIDDEN
or FRESTRICTED: the old names kept for old macros), COM's own plumbing (names starting with `_`: `_Default` is what
Python's call or Item gives, `_NewEnum` its iteration) and the code project of Excel's macro language (names starting
with `VB`: YCGrid's code is Python files) are left out. scripts\check_excel_parity.py compares the result with YCGrid.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA = os.path.join(ROOT, "schema", "excel_parity.toml")
OUT = os.path.join(ROOT, "tests", "data", "excel_object_model.json")
HIDDEN = 0x40 | 0x1  # FUNCFLAG_FHIDDEN | FUNCFLAG_FRESTRICTED
KINDS = {1: "method", 2: "get", 4: "put", 8: "put"}
IUNKNOWN = {"QueryInterface", "AddRef", "Release", "GetTypeInfoCount", "GetTypeInfo", "GetIDsOfNames", "Invoke"}


def excel_exe():
    import winreg

    with winreg.OpenKey(winreg.HKEY_CLASSES_ROOT, r"CLSID\{00024500-0000-0000-C000-000000000046}\LocalServer32") as k:
        v = winreg.QueryValueEx(k, "")[0]
    return v.split(" /")[0].strip('"')


def interfaces(path):
    from comtypes import typeinfo

    tlb = typeinfo.LoadTypeLibEx(path)
    out = {}
    for i in range(tlb.GetTypeInfoCount()):
        ti = tlb.GetTypeInfo(i)
        ta = ti.GetTypeAttr()
        if ta.typekind not in (3, 4):  # TKIND_INTERFACE, TKIND_DISPATCH
            continue
        members = {}
        for f in range(ta.cFuncs):
            fd = ti.GetFuncDesc(f)
            name = ti.GetDocumentation(fd.memid)[0]
            if name in IUNKNOWN or name.startswith("_") or name.startswith("VB") or fd.wFuncFlags & HIDDEN:
                continue
            kinds = members.setdefault(name, [])
            k = KINDS.get(fd.invkind, "method")
            if k not in kinds:
                kinds.append(k)
        name = tlb.GetDocumentation(i)[0]
        if len(members) > len(out.get(name, {})):  # the dispatch and dual interface of one object: the fuller one
            out[name] = members
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--comtypes", required=True, help="a folder holding comtypes (pip install comtypes --target DIR)")
    ap.add_argument("--excel", default=None, help="EXCEL.EXE (default: the one registered for Excel.Application)")
    a = ap.parse_args()
    sys.path.insert(0, a.comtypes)
    exe = a.excel or excel_exe()
    with open(SCHEMA, "rb") as f:
        objects = tomllib.load(f)["objects"]
    found = interfaces(exe)
    data = {}
    for name, o in objects.items():
        members = found.get(o["tlb"])
        if members is None:
            sys.exit(f"{o['tlb']} (for {name}) isn't in {exe}'s type library")
        data[name] = {k: " ".join(sorted(v)) for k, v in sorted(members.items())}
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"source": "the type library of " + os.path.basename(exe) + " (Excel 16)", "objects": data}, f,
                  indent=1, sort_keys=True)
        f.write("\n")
    print(f"{len(data)} objects, {sum(len(v) for v in data.values())} members -> {os.path.relpath(OUT, ROOT)}")


if __name__ == "__main__":
    main()
