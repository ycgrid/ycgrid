# SPDX-License-Identifier: Apache-2.0
"""A1 <-> R1C1 reference style conversion (Range.FormulaR1C1, Address(ReferenceStyle=xlR1C1), ConvertFormula).

Only references in formulas are converted; string constants, sheet names, function names and defined names are
left alone. Coordinates are always 0-based.
"""
from __future__ import annotations

import re

MAX_COLS = 16384
MAX_ROWS = 1048576

_IDENT = re.compile(r"[A-Za-z0-9_.\\\u0080-￿]")
# A1: entire columns A:C, entire rows 2:5, cells / ranges A1, $A$1:B2
_A1_COLS = re.compile(r"(\$?)([A-Za-z]{1,3}):(\$?)([A-Za-z]{1,3})")
_A1_ROWS = re.compile(r"(\$?)(\d+):(\$?)(\d+)")
_A1_CELL = re.compile(r"(\$?)([A-Za-z]{1,3})(\$?)(\d+)")
# R1C1: R[-1]C, RC[2], R2C3, R, C, R[1]:R[3], C2:C4
_RC_PART = re.compile(r"(?P<R>[Rr](?P<r>\[-?\d+\]|\d+)?)?(?P<C>[Cc](?P<c>\[-?\d+\]|\d+)?)?")


def _col_index(name: str) -> int:
    v = 0
    for ch in name.upper():
        v = v * 26 + (ord(ch) - 64)
    return v - 1


def _col_name(c: int) -> str:
    s = ""
    c += 1
    while c > 0:
        c, rem = divmod(c - 1, 26)
        s = chr(65 + rem) + s
    return s


def _is_ident(ch: str) -> bool:
    return bool(ch) and bool(_IDENT.match(ch))


def _scan(formula: str, convert):
    """Walk formula text: strings and quoted sheet names are kept as is; other positions go to convert(text, i) -> (output, new position) or None."""
    out = []
    i, n = 0, len(formula)
    while i < n:
        ch = formula[i]
        if ch in "\"'":
            j = i + 1
            while j < n:
                if formula[j] == ch:
                    if j + 1 < n and formula[j + 1] == ch:
                        j += 2
                        continue
                    break
                j += 1
            out.append(formula[i:j + 1])
            i = j + 1
            continue
        prev = formula[i - 1] if i > 0 else ""
        if not _is_ident(prev) or prev == "!":
            r = convert(formula, i)
            if r is not None:
                text, i = r
                out.append(text)
                continue
        # ordinary characters or identifiers: copy the whole identifier (never start matching in the middle of a word)
        if _is_ident(ch):
            j = i
            while j < n and _is_ident(formula[j]):
                j += 1
            out.append(formula[i:j])
            i = j
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _end_ok(formula: str, j: int) -> bool:
    """A reference can't be followed by an identifier character or an opening parenthesis (that is a function or a name)."""
    if j >= len(formula):
        return True
    return not _is_ident(formula[j]) and formula[j] not in "(!"


def _rel(n: int, absolute: bool, letter: str, base: int) -> str:
    if absolute:
        return f"{letter}{n + 1}"
    d = n - base
    return letter if d == 0 else f"{letter}[{d}]"


# ---------------------------------------------------------------------------
# A1 → R1C1
# ---------------------------------------------------------------------------
def a1_to_r1c1(formula: str, row: int, col: int) -> str:
    """Convert an A1-style formula (with =) to R1C1; row, col is the formula's cell (0-based)."""

    def conv(f, i):
        for rx, kind in ((_A1_COLS, "cols"), (_A1_ROWS, "rows"), (_A1_CELL, "cell")):
            m = rx.match(f, i)
            if not m or not _end_ok(f, m.end()):
                continue
            if kind == "cols":
                c0, c1 = _col_index(m.group(2)), _col_index(m.group(4))
                if c0 >= MAX_COLS or c1 >= MAX_COLS:
                    continue
                a = _rel(c0, bool(m.group(1)), "C", col)
                b = _rel(c1, bool(m.group(3)), "C", col)
                return (a if a == b else a + ":" + b), m.end()
            if kind == "rows":
                r0, r1 = int(m.group(2)) - 1, int(m.group(4)) - 1
                if r0 < 0 or r1 < 0:
                    continue
                a = _rel(r0, bool(m.group(1)), "R", row)
                b = _rel(r1, bool(m.group(3)), "R", row)
                return (a if a == b else a + ":" + b), m.end()
            c, r = _col_index(m.group(2)), int(m.group(4)) - 1
            if c >= MAX_COLS or r < 0:
                continue
            return _rel(r, bool(m.group(3)), "R", row) + _rel(c, bool(m.group(1)), "C", col), m.end()
        return None

    return _scan(formula, conv)


# ---------------------------------------------------------------------------
# R1C1 → A1
# ---------------------------------------------------------------------------
def _part(tok, base, size):
    """'[3]' / '5' / None -> (index, absolute?). A relative offset wraps around the sheet, as Excel's: R[-1]C in row 1
    is the last row; one of the sheet's size or more is out of range (index -1)."""
    if tok is None:
        return base, False
    if tok.startswith("["):
        d = int(tok[1:-1])
        return ((base + d) % size if abs(d) < size else -1), False
    return int(tok) - 1, True


def r1c1_to_a1(formula: str, row: int, col: int) -> str:
    """Convert an R1C1-style formula to A1; row, col is the formula's cell (0-based)."""

    def one(f, i):
        m = _RC_PART.match(f, i)
        if not m or m.end() == i:
            return None
        if m.group("R") and m.group("C"):
            r, ra = _part(m.group("r"), row, MAX_ROWS)
            c, ca = _part(m.group("c"), col, MAX_COLS)
            return ("cell", r, ra, c, ca), m.end()
        if m.group("R"):
            r, ra = _part(m.group("r"), row, MAX_ROWS)
            return ("row", r, ra), m.end()
        c, ca = _part(m.group("c"), col, MAX_COLS)
        return ("col", c, ca), m.end()

    def a1_row(r, ra):
        return ("$" if ra else "") + str(r + 1)

    def a1_col(c, ca):
        return ("$" if ca else "") + _col_name(c)

    def conv(f, i):
        first = one(f, i)
        if first is None:
            return None
        p, j = first
        q = None
        if j < len(f) and f[j] == ":":
            second = one(f, j + 1)
            if second is not None and second[0][0] == p[0]:
                q, j = second
        if not _end_ok(f, j):
            return None
        # a range can't go past the sheet
        def bad(x):
            if x[0] == "cell":
                return x[1] < 0 or x[1] >= MAX_ROWS or x[3] < 0 or x[3] >= MAX_COLS
            return x[1] < 0 or x[1] >= (MAX_COLS if x[0] == "col" else MAX_ROWS)

        if bad(p) or (q is not None and bad(q)):
            return "#REF!", j
        if q is not None:
            # a range is written from its top-left to its bottom-right cell, each row and column keeping its $ (Excel:
            # C[-1]:C[-2] in C1 is A:B, R[2]C[1]:R[-1]C[-1] in C3 is B2:D5)
            if p[0] == "cell":
                (pr, pra), (qr, qra) = sorted([(p[1], p[2]), (q[1], q[2])], key=lambda t: t[0])
                (pc, pca), (qc, qca) = sorted([(p[3], p[4]), (q[3], q[4])], key=lambda t: t[0])
                p, q = ("cell", pr, pra, pc, pca), ("cell", qr, qra, qc, qca)
            elif q[1] < p[1]:
                p, q = q, p
        if p[0] == "cell":
            s = a1_col(p[3], p[4]) + a1_row(p[1], p[2])
            if q is not None:
                s += ":" + a1_col(q[3], q[4]) + a1_row(q[1], q[2])
            return s, j
        if p[0] == "row":
            q = q or p
            return a1_row(p[1], p[2]) + ":" + a1_row(q[1], q[2]), j
        q = q or p
        return a1_col(p[1], p[2]) + ":" + a1_col(q[1], q[2]), j

    return _scan(formula, conv)


def area_r1c1(area, row_end, abs_r=True, abs_c=True, base_row=0, base_col=0) -> str:
    """Range (r0, c0, r1, c1) -> R1C1 address text (Address(ReferenceStyle=xlR1C1))."""
    r0, c0, r1, c1 = area

    def R(r):
        return _rel(r, abs_r, "R", base_row)

    def C(c):
        return _rel(c, abs_c, "C", base_col)

    if r1 == row_end:  # entire columns
        return C(c0) if c0 == c1 else C(c0) + ":" + C(c1)
    if c0 == 0 and c1 == MAX_COLS - 1:  # entire rows
        return R(r0) if r0 == r1 else R(r0) + ":" + R(r1)
    if r0 == r1 and c0 == c1:
        return R(r0) + C(c0)
    return R(r0) + C(c0) + ":" + R(r1) + C(c1)
