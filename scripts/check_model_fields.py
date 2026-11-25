# SPDX-License-Identifier: Apache-2.0
"""Every member of the model is accounted for: schema/model_fields.yaml says, for each member of Sheet and Workbook, how
it is kept in a .ycg file, and for each field of a model struct that may name cells (a text, a rectangle or a cell),
what keeps it right when cells move. A new member or field fails this check until it is listed, so nobody adds one
without deciding; and each decision is checked against the code and the tests that prove it:

  saved members:   ycg        the round-trip test (tests/unit/test_model_roundtrip.cpp) compares it after a save and an open
                   elsewhere FILE  kept in the file another way (an encrypted file's password), checked in FILE
                   transient  not saved, with the reason
  reference fields: text      walked by the reference lists (src/model/ref_texts.h, the ForEach*RefText functions) and
                              a slot of tests/unit/test_ref_registry.cpp
                   rect FILE  moved by the structure operations (src/engine/document_struct.cpp) and checked in FILE
                   key        a position kept as a map key (notes, hyperlinks), moved with its cells
                   kept       names cells but stays where it is, as in Excel (the reason, measured)
                   none       not a reference (the reason)

    python scripts/check_model_fields.py            check
    python scripts/check_model_fields.py --missing  print lines to add for what isn't listed
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA = os.path.join(ROOT, "schema", "model_fields.yaml")
MODEL = os.path.join(ROOT, "src", "model")
ROUNDTRIP_TEST = os.path.join(ROOT, "tests", "unit", "test_model_roundtrip.cpp")
REF_TEST = os.path.join(ROOT, "tests", "unit", "test_ref_registry.cpp")
WALKERS = [os.path.join(MODEL, f) for f in ("ref_texts.h", "objects.h", "whatif.h")]
STRUCT_CODE = [os.path.join(ROOT, "src", "engine", f) for f in ("document_struct.cpp", "document_data.cpp")]

# the types of a field that may name cells
REF_TYPE = re.compile(r"^(?:const\s+)?(?:std::string|(?:core::)?Rect|(?:core::)?CellPos|std::vector<(?:core::)?Rect>|"
                      r"std::vector<(?:core::)?CellPos>|std::vector<std::string>)$")


def read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def split_top(text: str, sep: str) -> list[str]:
    """Split at sep outside <>, (), {} and []."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "<({[":
            depth += 1
        elif ch in ">)}]":
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def structs(src: str):
    """(name, members) for every struct / class: members are (type, name) of the data members at its own level."""
    src = strip_comments(src)
    for m in re.finditer(r"\b(?:struct|class)\s+(\w+)\s*(?:final\s*)?(?::[^{;]*)?\{", src):
        name, i, depth, body = m.group(1), m.end(), 1, []
        while i < len(src) and depth:
            ch = src[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            body.append(ch if depth == 1 or (depth == 2 and ch == "{") else ("\n" if ch == "\n" else " "))
            i += 1
        text = "".join(body)
        text = re.sub(r"\{\s*\}", "", text)  # nested bodies blanked out leave {}
        members = []
        for stmt in text.split(";"):
            stmt = " ".join(stmt.split())
            stmt = re.sub(r"^(?:public|private|protected)\s*:\s*", "", stmt)
            if not stmt or stmt.startswith(("using ", "friend ", "static ", "enum ", "typedef ", "return ", "template")):
                continue
            if stmt.startswith(("struct ", "class ")):
                continue
            # declarators split first (uint32_t a = 0, b = 0), then each without its initializer
            parts = [split_top(p, "=")[0].split("{")[0] for p in split_top(stmt, ",")]
            head = parts[0].strip()
            if "(" in head or "operator" in head:
                continue
            first = head
            mm = re.match(r"^(.*?)([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(\{.*\})?$", first)
            if not mm or not mm.group(1).strip():
                continue
            typ = mm.group(1).strip()
            members.append((typ, mm.group(2)))
            for p in parts[1:]:
                pm = re.match(r"^\s*\**([A-Za-z_]\w*)", p)
                if pm:
                    members.append((typ, pm.group(1)))
        yield name, members


def load_schema():
    entries = {}
    section = None
    for n, line in enumerate(read(SCHEMA).splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if not line.startswith(" "):
            section = line.rstrip(":").strip()
            continue
        key, _, value = line.strip().partition(":")
        value = value.split("#", 1)[0].strip()
        entries[(section, key.strip())] = (value, n)
    return entries


def mentions(text: str, field: str) -> bool:
    return re.search(r"[.>(]" + re.escape(field) + r"\b", text) is not None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--missing", action="store_true", help="print the lines to add for what isn't listed")
    a = ap.parse_args()
    schema = load_schema()
    problems, missing = [], []

    all_structs = {}
    for path in sorted(glob.glob(os.path.join(MODEL, "*.h"))):
        for name, members in structs(read(path)):
            all_structs.setdefault(name, members)

    # 1. Sheet and Workbook: how each member is kept
    roundtrip = read(ROUNDTRIP_TEST) if os.path.exists(ROUNDTRIP_TEST) else ""
    seen = set()
    for cls in ("Sheet", "Workbook"):
        for _typ, field in all_structs.get(cls, []):
            key = ("saved", f"{cls}.{field}")
            seen.add(key)
            if key not in schema:
                missing.append(("saved", f"{cls}.{field}", "ycg"))
                continue
            value, n = schema[key]
            kind = value.split()[0] if value else ""
            if kind == "ycg":
                if not mentions(roundtrip, field):
                    problems.append(f"model_fields.yaml:{n}: {cls}.{field} is saved but test_model_roundtrip.cpp doesn't compare it")
            elif kind == "elsewhere":
                test = os.path.join(ROOT, value.split()[1]) if len(value.split()) > 1 else ""
                if not test or not os.path.exists(test) or not mentions(read(test), field):
                    problems.append(f"model_fields.yaml:{n}: {cls}.{field} elsewhere: name the test file that checks it")
            elif kind == "transient":
                if len(value.split()) < 2:
                    problems.append(f"model_fields.yaml:{n}: {cls}.{field} transient: say why")
            else:
                problems.append(f"model_fields.yaml:{n}: {cls}.{field}: '{value}' is neither ycg nor transient")

    # 2. every field that may name cells: what keeps it right
    walkers = "\n".join(read(p) for p in WALKERS)
    ref_test = read(REF_TEST) if os.path.exists(REF_TEST) else ""
    struct_code = "\n".join(read(p) for p in STRUCT_CODE)
    for cls, members in sorted(all_structs.items()):
        for typ, field in members:
            if not REF_TYPE.match(typ):
                continue
            key = ("references", f"{cls}.{field}")
            seen.add(key)
            if key not in schema:
                missing.append(("references", f"{cls}.{field}", "none  # TODO: text / rect FILE / key / kept / none, and why"))
                continue
            value, n = schema[key]
            words = value.split()
            kind = words[0] if words else ""
            where = f"model_fields.yaml:{n}: {cls}.{field}"
            if kind == "text":
                if not mentions(walkers, field):
                    problems.append(f"{where} is a reference text, but no reference list walks it (src/model/ref_texts.h)")
                if not mentions(ref_test, field):
                    problems.append(f"{where} is a reference text without a slot in test_ref_registry.cpp")
            elif kind == "rect":
                test = os.path.join(ROOT, words[1]) if len(words) > 1 else ""
                if not test or not os.path.exists(test):
                    problems.append(f"{where}: rect needs the test file that checks it")
                elif not mentions(read(test), field):
                    problems.append(f"{where}: {words[1]} doesn't mention it")
                if not mentions(struct_code, field):
                    problems.append(f"{where} is a rectangle no structure operation moves (document_struct.cpp)")
            elif kind in ("key", "kept", "none"):
                if kind != "key" and len(words) < 2:
                    problems.append(f"{where} {kind}: say why")
            else:
                problems.append(f"{where}: unknown kind '{value}'")

    for key, (value, n) in schema.items():
        if key not in seen:
            problems.append(f"model_fields.yaml:{n}: {key[1]} is no longer in the model: remove it")

    if a.missing:
        last = None
        for section, key, value in missing:
            if section != last:
                print(f"{section}:")
                last = section
            print(f"  {key}: {value}")
        return 0
    for p in problems:
        print(p)
    for section, key, _ in missing:
        print(f"model_fields.yaml: {key} isn't listed under {section}: (python scripts/check_model_fields.py --missing)")
    n = len(problems) + len(missing)
    print(f"check_model_fields: {n} problem(s)" if n else "check_model_fields: every model field is accounted for")
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
