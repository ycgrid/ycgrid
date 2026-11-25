# SPDX-License-Identifier: Apache-2.0
"""UI translation table tool (English source text -> Traditional Chinese).

  python scripts/i18n_build.py extract   add every TR("...") / TRW(L"...") text found in the source to
                                         i18n/zh-TW.json (new entries get an empty value)
  python scripts/i18n_build.py build     generate src/core/i18n_zh.inc from i18n/zh-TW.json (compiled into ycgrid.exe)
  python scripts/i18n_build.py check     list texts without a translation and printf-format mismatches
                                         (exit code 1 when something is wrong)

Keys and values in zh-TW.json are written the way they appear inside a C string literal (\\n and \\" stay
escaped). For "Label##id" only the label is a key; for "Label##ctx_name" the whole "Label##ctx_name" is the key.
The generated .inc stores the Chinese text as octal escapes so all C++ sources stay ASCII.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JSON_PATH = os.path.join(ROOT, "i18n", "zh-TW.json")
INC_PATH = os.path.join(ROOT, "src", "core", "i18n_zh.inc")
SCAN_DIRS = ["apps", "src"]
TR_CALL = re.compile(r'\bTRW?\(\s*((?:L?"(?:[^"\\]|\\.)*"\s*)+)\)')
PIECE = re.compile(r'L?"((?:[^"\\]|\\.)*)"')
# no space flag: UI text like "% of Grand Total" isn't a printf "% o" (the code has no space-flag formats)
PRINTF = re.compile(r"%[-+#0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcsp%]")
C_TOKEN = re.compile(r"\\(.|$)|.", re.S)


def c_escapes(s: str) -> list:
    """The character after each backslash of a C string literal's body, in order ("a\\\\b\\n" gives ["\\", "n"]; a
    backslash at the end gives "<end>")."""
    return [m.group(1) or "<end>" for m in C_TOKEN.finditer(s) if m.group(0).startswith("\\")]


def bad_escape(s: str) -> bool:
    """A backslash C doesn't read as a whole escape: one before a non-ASCII character (its first byte's octal
    escape took it), an unknown letter, or the end."""
    return any(ch not in "\\\"'?abfnrtv01234567x" for ch in c_escapes(s))


def strip_comments(src: str) -> str:
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        c = src[i]
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def key_of(body: str) -> str:
    """The lookup key: the label, plus its ##ctx_ marker if it has one."""
    first = body.find("##")
    if first < 0:
        return body
    if body.startswith("##ctx_", first):
        end = body.find("##", first + 2)
        return body if end < 0 else body[:end]
    return body[:first]


def keys_in_source() -> dict:
    found = {}
    for d in SCAN_DIRS:
        for dp, _, fs in os.walk(os.path.join(ROOT, d)):
            for f in fs:
                if not f.endswith((".cpp", ".h")):
                    continue
                p = os.path.join(dp, f)
                src = strip_comments(open(p, encoding="utf-8").read())
                for m in TR_CALL.finditer(src):
                    key = key_of("".join(PIECE.findall(m.group(1))))
                    if key.split("##")[0].strip():
                        found.setdefault(key, os.path.relpath(p, ROOT).replace("\\", "/"))
    return found


def load() -> dict:
    if not os.path.exists(JSON_PATH):
        return {}
    with open(JSON_PATH, encoding="utf-8") as f:
        return json.load(f)


def save(table: dict) -> None:
    os.makedirs(os.path.dirname(JSON_PATH), exist_ok=True)
    with open(JSON_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(table, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")


def c_ascii(s: str) -> str:
    """Escapes non-ASCII characters as octal byte escapes (already-escaped sequences are kept)."""
    out = []
    after_escape = False
    for ch in s:
        if ord(ch) < 128:
            if after_escape and ch.isdigit():
                out.append('" "')  # a digit right after an octal escape: split the literal (MSVC C4125)
            out.append(ch)
            after_escape = False
        else:
            out.extend(f"\\{b:03o}" for b in ch.encode("utf-8"))
            after_escape = True
    return "".join(out)


def main(argv) -> int:
    cmd = argv[1] if len(argv) > 1 else "build"
    table = load()
    if cmd == "extract":
        src = keys_in_source()
        added = [k for k in src if k not in table]
        for k in added:
            table[k] = ""
        save(table)
        todo = sum(1 for k in src if not table.get(k))
        print(f"{len(src)} texts, {len(added)} new, {todo} without a translation")
        return 0
    if cmd == "check":
        src = keys_in_source()
        missing = [k for k in src if not table.get(k)]
        for k in missing:
            print(f"missing: {src[k]}: {k}")
        bad = [k for k, v in table.items() if v and PRINTF.findall(k) != PRINTF.findall(v)]
        for k in bad:
            print(f"format mismatch: {k!r} -> {table[k]!r}")
        # a value is a C string literal: one backslash before a Chinese comma escaped the comma's first byte instead
        escapes = [k for k, v in table.items() if v and bad_escape(v)]
        for k in escapes:
            print(f"bad escape (write a backslash as two): {k!r} -> {table[k]!r}")
        unused = [k for k in table if k not in src]
        print(f"{len(missing)} missing / {len(src)}; {len(bad)} format mismatches; {len(escapes)} bad escapes; "
              f"{len(unused)} unused entries")
        return 1 if missing or bad or escapes else 0
    if cmd == "build":
        lines = ["// Generated by: python scripts/i18n_build.py build (edit i18n/zh-TW.json, not this file)"]
        for k in sorted(table):
            if table[k]:
                lines.append(f'    {{"{k}", "{c_ascii(table[k])}"}},')
        text = "\n".join(lines) + "\n"
        old = open(INC_PATH, encoding="utf-8").read() if os.path.exists(INC_PATH) else ""
        if old != text:
            with open(INC_PATH, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        print(f"{sum(1 for v in table.values() if v)} translations -> {os.path.relpath(INC_PATH, ROOT)}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
