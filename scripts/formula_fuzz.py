# SPDX-License-Identifier: Apache-2.0
r"""Random formulas against answers worked out in Python the way Excel defines them (see
skill/ycgrid-python/references/testing.md, "Checked at random").

    <YCGrid>\runtime\python\python.exe -E -s scripts\formula_fuzz.py [--seed N] [--n COUNT] [--exe path] [--eval]

A hidden YCGrid gets a grid of known numbers, texts, logicals and blanks (A1:H20) and N formulas picked at random from
templates: arithmetic trees with precedence (-2^2, 2^3^2, 50%), rounding (ROUND half away from zero, INT, TRUNC,
MROUND, CEILING.MATH, EVEN / ODD), MOD and QUOTIENT signs, aggregates over mixed ranges (what SUM, COUNT, AVERAGE,
MIN, PRODUCT, MEDIAN, STDEV ignore and when they error), SUMIF / COUNTIFS criteria, text functions and coercion
("3"+"4", TRUE+1, text > any number, case-insensitive comparison), logic (IF, IFS, SWITCH, CHOOSE, IS*), lookups
(INDEX, MATCH, VLOOKUP, XLOOKUP, OFFSET, INDIRECT, ADDRESS), dates (DATE overflow, EDATE / EOMONTH clamping, WEEKDAY,
DATEDIF, WEEKNUM), dynamic arrays and lambdas (SEQUENCE, FILTER, UNIQUE, TAKE / DROP, MAP, REDUCE, BYROW, SORTBY,
LET, LAMBDA), RANK.EQ and MODE ties. Every answer is compared with the engine's; --eval also checks
Application.Evaluate on the first 100. Exit code 1 when a formula gave another answer (the seed replays the run).
It found MODE returning the value whose count reached the maximum first instead of the first of the tied values.
"""
from __future__ import annotations

import argparse
import datetime as dt
import math
import os
import random
import sys
from decimal import ROUND_HALF_UP, Decimal
from fractions import Fraction

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

TEXTS = ["apple", "Banana", "cherry", "APPLE", "date", "fig", "Grape", "kiwi", "a b", " pad "]
EPOCH = dt.date(1899, 12, 30)


def serial(d: dt.date) -> int:
    return (d - EPOCH).days


def xround(x, n=0):  # half away from zero on the decimal repr
    q = Decimal(1).scaleb(-n)
    if x >= 0:
        return float(Decimal(repr(x)).quantize(q, rounding=ROUND_HALF_UP))
    return -float(Decimal(repr(-x)).quantize(q, rounding=ROUND_HALF_UP))


def num_text(x):
    """General text of a number (as & gives it)."""
    if isinstance(x, bool):
        return "TRUE" if x else "FALSE"
    if x is None:
        return ""
    if isinstance(x, str):
        return x
    if float(x).is_integer() and abs(x) < 1e15:
        return str(int(x))
    return repr(float(x))


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        r = self.r
        self.grid = {}
        for row in range(1, 21):
            for c in "ABCDE":
                self.grid[f"{c}{row}"] = r.randint(-20, 60)
            self.grid[f"F{row}"] = r.randint(-40, 120) / 4
            self.grid[f"G{row}"] = r.choice(TEXTS)
            k = r.random()
            self.grid[f"H{row}"] = (None if k < 0.3 else r.choice([True, False]) if k < 0.5
                                    else r.choice(TEXTS) if k < 0.7 else r.randint(0, 9))

    # ---- helpers ----
    def v(self, ref):
        return self.grid[ref]

    def ncell(self):
        return f"{self.r.choice('ABCDE')}{self.r.randint(1, 20)}"

    def fcell(self):
        return f"F{self.r.randint(1, 20)}"

    def tcell(self):
        return f"G{self.r.randint(1, 20)}"

    def hcell(self):
        return f"H{self.r.randint(1, 20)}"

    def col_vals(self, c, r1=1, r2=20):
        return [self.grid[f"{c}{i}"] for i in range(r1, r2 + 1)]

    def rng(self, cols="ABCDE"):
        r = self.r
        c1, c2 = sorted(r.sample(range(len(cols)), 2)) if len(cols) > 1 else (0, 0)
        r1, r2 = sorted(r.sample(range(1, 21), 2))
        ref = f"{cols[c1]}{r1}:{cols[c2]}{r2}"
        vals = [self.grid[f"{cols[c]}{i}"] for c in range(c1, c2 + 1) for i in range(r1, r2 + 1)]
        return ref, vals

    @staticmethod
    def nums(vals):
        return [x for x in vals if isinstance(x, (int, float)) and not isinstance(x, bool)]

    # ---- templates: each returns (formula, expected); None as expected skips the case ----
    def t_arith(self):
        r = self.r
        depth = r.randint(1, 3)

        def node(d):
            if d == 0 or r.random() < 0.3:
                k = r.random()
                if k < 0.5:
                    c = self.ncell()
                    return c, Fraction(self.v(c))
                if k < 0.8:
                    n = r.randint(-9, 9)
                    return (f"({n})" if n < 0 else str(n)), Fraction(n)
                c = self.fcell()
                return c, Fraction(self.v(c))
            op = r.choice("+-*/")
            a, va = node(d - 1)
            b, vb = node(d - 1)
            if isinstance(va, str) or isinstance(vb, str):
                return f"({a}{op}{b})", va if isinstance(va, str) else vb
            if op == "/":
                if vb == 0:
                    return f"({a}/{b})", "#DIV/0!"
                return f"({a}/{b})", va / vb
            return f"({a}{op}{b})", {"+": va + vb, "-": va - vb, "*": va * vb}[op]

        f, val = node(depth)
        return "=" + f, (val if isinstance(val, str) else float(val))

    def t_power(self):
        r = self.r
        base, e = r.randint(-5, 5), r.randint(0, 4)
        k = r.random()
        if k < 0.3:
            return f"=-{abs(base)}^2", float(abs(base) ** 2)   # unary minus binds tighter: (-b)^2
        if k < 0.5:
            return "=2^3^2", 64.0
        if k < 0.7:
            c = self.ncell()
            x = self.v(c)
            if x == 0 and e == 0:
                return f"={c}^{e}", "#NUM!"
            return f"={c}^{e}", float(x ** e)
        if k < 0.85:
            return f"=POWER({base},{e})", ("#NUM!" if base == 0 and e == 0 else float(base ** e))
        c = self.ncell()
        x = self.v(c)
        if x < 0:
            return f"=SQRT({c})", "#NUM!"
        return f"=SQRT({c})", math.sqrt(x)

    def t_percent(self):
        n = self.r.randint(0, 200)
        return f"={n}%", n / 100

    def t_round(self):
        r = self.r
        c = self.fcell() if r.random() < 0.5 else self.ncell()
        x = self.v(c)
        fn = r.choice(["ROUND", "ROUNDUP", "ROUNDDOWN", "INT", "TRUNC", "EVEN", "ODD", "MROUND", "CEILING.MATH",
                       "FLOOR.MATH", "SIGN", "ABS"])
        if fn == "ROUND":
            n = r.choice([0, 1, -1])
            return f"=ROUND({c},{n})", xround(x, n)
        if fn == "ROUNDUP":
            n = r.choice([0, 1])
            m = 10 ** n
            return f"=ROUNDUP({c},{n})", math.copysign(math.ceil(abs(x) * m - 1e-12) / m, x) if x else 0.0
        if fn == "ROUNDDOWN":
            n = r.choice([0, 1])
            m = 10 ** n
            return f"=ROUNDDOWN({c},{n})", math.copysign(math.floor(abs(x) * m + 1e-12) / m, x) if x else 0.0
        if fn == "INT":
            return f"=INT({c})", float(math.floor(x))
        if fn == "TRUNC":
            return f"=TRUNC({c})", float(math.trunc(x))
        if fn == "EVEN":
            a = math.ceil(abs(x))
            a = a if a % 2 == 0 else a + 1
            return f"=EVEN({c})", float(math.copysign(a, x)) if x else 0.0
        if fn == "ODD":
            a = math.ceil(abs(x))
            a = a if a % 2 == 1 else a + 1
            return f"=ODD({c})", float(math.copysign(a, x)) if x else 1.0
        if fn == "MROUND":
            m = r.choice([2, 5, 0.5])
            if x < 0:
                return f"=MROUND({c},{m})", "#NUM!"
            q = Fraction(x) / Fraction(m)
            return f"=MROUND({c},{m})", float(Fraction(math.floor(q + Fraction(1, 2))) * Fraction(m))
        if fn == "CEILING.MATH":
            m = r.choice([2, 5, 0.5])
            return f"=CEILING.MATH({c},{m})", float(math.ceil(Fraction(x) / Fraction(m)) * Fraction(m))
        if fn == "FLOOR.MATH":
            m = r.choice([2, 5, 0.5])
            return f"=FLOOR.MATH({c},{m})", float(math.floor(Fraction(x) / Fraction(m)) * Fraction(m))
        if fn == "SIGN":
            return f"=SIGN({c})", float((x > 0) - (x < 0))
        return f"=ABS({c})", float(abs(x))

    def t_mod(self):
        r = self.r
        a, b = self.ncell(), r.choice([-7, -3, 3, 4, 7, 0])
        x = self.v(a)
        fn = r.choice(["MOD", "QUOTIENT"])
        if b == 0:
            return f"={fn}({a},{b})", "#DIV/0!"
        if fn == "MOD":
            return f"=MOD({a},{b})", float(x - b * math.floor(x / b))
        return f"=QUOTIENT({a},{b})", float(math.trunc(x / b))

    def t_intmath(self):
        r = self.r
        a, b = r.randint(0, 40), r.randint(1, 40)
        fn = r.choice(["GCD", "LCM", "FACT", "COMBIN", "LOG", "EXP", "LN", "LOG10", "PI"])
        if fn == "GCD":
            return f"=GCD({a},{b})", float(math.gcd(a, b))
        if fn == "LCM":
            return f"=LCM({a},{b})", float(a * b // math.gcd(a, b)) if a else 0.0
        if fn == "FACT":
            n = r.randint(-1, 12)
            return f"=FACT({n})", ("#NUM!" if n < 0 else float(math.factorial(n)))
        if fn == "COMBIN":
            n, k = r.randint(0, 12), r.randint(0, 6)
            return f"=COMBIN({n},{k})", ("#NUM!" if k > n else float(math.comb(n, k)))
        if fn == "LOG":
            return f"=LOG({2 ** a},2)", float(a)
        if fn == "EXP":
            return f"=EXP({b / 10})", math.exp(b / 10)
        if fn == "LN":
            return f"=LN({b})", math.log(b)
        if fn == "LOG10":
            return f"=LOG10({10 ** (a % 6)})", float(a % 6)
        return "=PI()", math.pi

    def t_agg(self):
        r = self.r
        cols = r.choice(["ABCDE", "F", "H", "ABCDEFGH"])
        ref, vals = self.rng(cols)
        nums = self.nums(vals)
        fn = r.choice(["SUM", "MIN", "MAX", "AVERAGE", "COUNT", "COUNTA", "COUNTBLANK", "PRODUCT", "SUMSQ", "MEDIAN",
                       "LARGE", "SMALL", "STDEV.P", "VAR.P", "STDEV.S"])
        if fn == "SUM":
            return f"=SUM({ref})", float(sum(nums))
        if fn == "MIN":
            return f"=MIN({ref})", float(min(nums)) if nums else 0.0
        if fn == "MAX":
            return f"=MAX({ref})", float(max(nums)) if nums else 0.0
        if fn == "AVERAGE":
            return f"=AVERAGE({ref})", (sum(nums) / len(nums) if nums else "#DIV/0!")
        if fn == "COUNT":
            return f"=COUNT({ref})", float(len(nums))
        if fn == "COUNTA":
            return f"=COUNTA({ref})", float(sum(1 for x in vals if x is not None))
        if fn == "COUNTBLANK":
            return f"=COUNTBLANK({ref})", float(sum(1 for x in vals if x is None))
        if fn == "PRODUCT":
            p = 1.0
            for x in nums:
                p *= x
            return f"=PRODUCT({ref})", (p if nums else 0.0)
        if fn == "SUMSQ":
            return f"=SUMSQ({ref})", float(sum(x * x for x in nums))
        if fn == "MEDIAN":
            if not nums:
                return f"=MEDIAN({ref})", "#NUM!"
            s = sorted(nums)
            n = len(s)
            return f"=MEDIAN({ref})", float(s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2)
        if fn in ("LARGE", "SMALL"):
            k = r.randint(1, 4)
            s = sorted(nums, reverse=(fn == "LARGE"))
            return f"={fn}({ref},{k})", (float(s[k - 1]) if k <= len(s) else "#NUM!")
        if fn in ("STDEV.P", "VAR.P"):
            if not nums:
                return f"={fn}({ref})", "#DIV/0!"
            m = sum(nums) / len(nums)
            var = sum((x - m) ** 2 for x in nums) / len(nums)
            return f"={fn}({ref})", (math.sqrt(var) if fn == "STDEV.P" else var)
        if len(nums) < 2:
            return f"=STDEV.S({ref})", "#DIV/0!"
        m = sum(nums) / len(nums)
        return f"=STDEV.S({ref})", math.sqrt(sum((x - m) ** 2 for x in nums) / (len(nums) - 1))

    def t_cond_agg(self):
        r = self.r
        c = r.choice("ABCDE")
        r1, r2 = sorted(r.sample(range(1, 21), 2))
        vals = self.col_vals(c, r1, r2)
        op = r.choice([">", "<", ">=", "<=", "<>", "="])
        n = r.randint(-5, 40)
        test = {">": lambda x: x > n, "<": lambda x: x < n, ">=": lambda x: x >= n, "<=": lambda x: x <= n,
                "<>": lambda x: x != n, "=": lambda x: x == n}[op]
        crit = f'"{op}{n}"' if (op != "=" or r.random() < 0.5) else str(n)
        hit = [x for x in vals if test(x)]
        fn = r.choice(["SUMIF", "COUNTIF", "AVERAGEIF", "MAXIFS", "MINIFS", "SUMIFS", "COUNTIFS", "SUMIF2"])
        ref = f"{c}{r1}:{c}{r2}"
        if fn == "SUMIF":
            return f"=SUMIF({ref},{crit})", float(sum(hit))
        if fn == "COUNTIF":
            return f"=COUNTIF({ref},{crit})", float(len(hit))
        if fn == "AVERAGEIF":
            return f"=AVERAGEIF({ref},{crit})", (sum(hit) / len(hit) if hit else "#DIV/0!")
        if fn == "MAXIFS":
            return f"=MAXIFS({ref},{ref},{crit})", float(max(hit)) if hit else 0.0
        if fn == "MINIFS":
            return f"=MINIFS({ref},{ref},{crit})", float(min(hit)) if hit else 0.0
        c2 = r.choice("ABCDE")
        ref2 = f"{c2}{r1}:{c2}{r2}"
        vals2 = self.col_vals(c2, r1, r2)
        if fn == "SUMIF2":
            return f"=SUMIF({ref},{crit},{ref2})", float(sum(y for x, y in zip(vals, vals2) if test(x)))
        m = r.randint(0, 30)
        if fn == "SUMIFS":
            return (f'=SUMIFS({ref2},{ref},{crit},{ref2},">{m}")',
                    float(sum(y for x, y in zip(vals, vals2) if test(x) and y > m)))
        return (f'=COUNTIFS({ref},{crit},{ref2},"<{m}")',
                float(sum(1 for x, y in zip(vals, vals2) if test(x) and y < m)))

    def t_text_crit(self):
        r = self.r
        r1, r2 = sorted(r.sample(range(1, 21), 2))
        vals = self.col_vals("G", r1, r2)
        w = r.choice(TEXTS)
        ref = f"G{r1}:G{r2}"
        k = r.random()
        if k < 0.4:
            return f'=COUNTIF({ref},"{w}")', float(sum(1 for x in vals if x.lower() == w.lower()))
        if k < 0.7:
            return f'=COUNTIF({ref},"<>{w}")', float(sum(1 for x in vals if x.lower() != w.lower()))
        c2 = r.choice("ABCDE")
        vals2 = self.col_vals(c2, r1, r2)
        return (f'=SUMIF({ref},"{w}",{c2}{r1}:{c2}{r2})',
                float(sum(y for x, y in zip(vals, vals2) if x.lower() == w.lower())))

    def t_text(self):
        r = self.r
        c = self.tcell()
        s = self.v(c)
        fn = r.choice(["LEN", "LEFT", "RIGHT", "MID", "UPPER", "LOWER", "TRIM", "REPT", "CONCAT", "SUBSTITUTE", "REPLACE",
                       "FIND", "SEARCH", "EXACT", "VALUE", "CHAR", "CODE", "TEXTJOIN", "PROPER", "AMP", "T", "N",
                       "CONCATENATE"])
        if fn == "LEN":
            return f"=LEN({c})", float(len(s))
        if fn == "LEFT":
            n = r.randint(0, 6)
            return f"=LEFT({c},{n})", s[:n]
        if fn == "RIGHT":
            n = r.randint(0, 6)
            return f"=RIGHT({c},{n})", s[max(0, len(s) - n):] if n else ""
        if fn == "MID":
            a, n = r.randint(1, 5), r.randint(0, 4)
            return f"=MID({c},{a},{n})", s[a - 1:a - 1 + n]
        if fn == "UPPER":
            return f"=UPPER({c})", s.upper()
        if fn == "LOWER":
            return f"=LOWER({c})", s.lower()
        if fn == "PROPER":
            if " " in s.strip():
                return None, None
            return f"=PROPER({c})", s.strip().capitalize() if s == s.strip() else None
        if fn == "TRIM":
            return f"=TRIM({c})", " ".join(s.split())
        if fn == "REPT":
            n = r.randint(0, 4)
            return f"=REPT({c},{n})", s * n
        if fn == "CONCAT":
            d, e = self.tcell(), self.ncell()
            return f"=CONCAT({c},{d},{e})", s + self.v(d) + num_text(self.v(e))
        if fn == "CONCATENATE":
            d = self.ncell()
            return f'=CONCATENATE({c},"-",{d})', s + "-" + num_text(self.v(d))
        if fn == "AMP":
            d, e = self.ncell(), self.hcell()
            return f'={c}&{d}&{e}&""', s + num_text(self.v(d)) + num_text(self.v(e))
        if fn == "SUBSTITUTE":
            old, new = r.choice("aep "), r.choice(["X", "", "yy"])
            return f'=SUBSTITUTE({c},"{old}","{new}")', s.replace(old, new)
        if fn == "REPLACE":
            a, n = r.randint(1, 4), r.randint(0, 3)
            return f'=REPLACE({c},{a},{n},"Z")', s[:a - 1] + "Z" + s[a - 1 + n:]
        if fn == "FIND":
            w = r.choice("aepXB")
            i = s.find(w)
            return f'=FIND("{w}",{c})', (float(i + 1) if i >= 0 else "#VALUE!")
        if fn == "SEARCH":
            w = r.choice("aepXB")
            i = s.lower().find(w.lower())
            return f'=SEARCH("{w}",{c})', (float(i + 1) if i >= 0 else "#VALUE!")
        if fn == "EXACT":
            d = self.tcell()
            return f"=EXACT({c},{d})", s == self.v(d)
        if fn == "VALUE":
            n = r.randint(-99, 999)
            return f'=VALUE("{n}")', float(n)
        if fn == "CHAR":
            n = r.randint(33, 126)
            if chr(n) == '"':
                n = 65
            return f"=CHAR({n})", chr(n)
        if fn == "CODE":
            return f"=CODE({c})", float(ord(s[0]))
        if fn == "T":
            d = self.ncell()
            return f"=T({d})", ""
        if fn == "N":
            return f"=N({c})", 0.0
        r1 = r.randint(1, 17)
        return f'=TEXTJOIN("|",TRUE,G{r1}:G{r1 + 2})', "|".join(self.col_vals("G", r1, r1 + 2))

    def t_compare(self):
        r = self.r
        k = r.random()
        if k < 0.4:
            a, b = self.ncell(), self.ncell()
            op = r.choice(["=", "<>", "<", ">", "<=", ">="])
            x, y = self.v(a), self.v(b)
            res = {"=": x == y, "<>": x != y, "<": x < y, ">": x > y, "<=": x <= y, ">=": x >= y}[op]
            return f"={a}{op}{b}", res
        if k < 0.7:
            a, b = self.tcell(), self.tcell()
            x, y = self.v(a).lower(), self.v(b).lower()
            op = r.choice(["=", "<", ">"])
            return f"={a}{op}{b}", {"=": x == y, "<": x < y, ">": x > y}[op]
        if k < 0.85:
            a, b = self.tcell(), self.ncell()
            return f"={a}>{b}", True   # text is greater than any number
        a = self.hcell()
        x = self.v(a)
        if x is None:
            return f"={a}=0", True
        if isinstance(x, bool):
            return f"={a}=TRUE", x
        return f"=ISBLANK({a})", False

    def t_coerce(self):
        r = self.r
        k = r.random()
        if k < 0.2:
            return "=TRUE+1", 2.0
        if k < 0.35:
            return '="3"+"4"', 7.0
        if k < 0.5:
            return '="3"&4', "34"
        if k < 0.6:
            return '=-"3"', -3.0
        if k < 0.7:
            return '="abc"*2', "#VALUE!"
        if k < 0.8:
            a = self.hcell()
            x = self.v(a)
            if x is None:
                return f"={a}+1", 1.0
            if isinstance(x, bool):
                return f"={a}+1", float(x) + 1
            if isinstance(x, str):
                return f"={a}+1", "#VALUE!"
            return f"={a}+1", float(x) + 1
        if k < 0.9:
            a = self.hcell()
            x = self.v(a)
            return f'={a}&"!"', num_text(x) + "!"
        c = self.ncell()
        return f'=SUM({c},TRUE,"2")', float(self.v(c)) + 3   # direct arguments: coerced

    def t_logic(self):
        r = self.r
        a, b = self.ncell(), self.ncell()
        x, y = self.v(a), self.v(b)
        fn = r.choice(["IF", "AND", "OR", "NOT", "XOR", "IFS", "SWITCH", "IFERROR", "CHOOSE", "ISNUMBER", "ISTEXT",
                       "ISLOGICAL", "ISERROR", "ISNA", "ISEVEN", "ISODD", "TYPE", "IFNA"])
        if fn == "IF":
            return f'=IF({a}>{b},"big",{a}-{b})', ("big" if x > y else float(x - y))
        if fn == "AND":
            return f"=AND({a}>0,{b}>0)", bool(x > 0 and y > 0)
        if fn == "OR":
            return f"=OR({a}>50,{b}<0)", bool(x > 50 or y < 0)
        if fn == "NOT":
            return f"=NOT({a})", x == 0
        if fn == "XOR":
            return f"=XOR({a}>0,{b}>0)", bool((x > 0) != (y > 0))
        if fn == "IFS":
            return f'=IFS({a}<0,"neg",{a}=0,"zero",TRUE,"pos")', ("neg" if x < 0 else "zero" if x == 0 else "pos")
        if fn == "SWITCH":
            n = r.randint(1, 4)
            return f'=SWITCH({n},1,"one",2,"two","many")', {1: "one", 2: "two"}.get(n, "many")
        if fn == "IFERROR":
            return f'=IFERROR(1/{a},"div")', ("div" if x == 0 else 1 / x)
        if fn == "IFNA":
            return "=IFNA(NA(),7)", 7.0
        if fn == "CHOOSE":
            n = r.randint(0, 4)
            return f'=CHOOSE({n},"a","b","c")', ({1: "a", 2: "b", 3: "c"}.get(n, "#VALUE!"))
        if fn == "ISNUMBER":
            h = self.hcell()
            v = self.v(h)
            return f"=ISNUMBER({h})", isinstance(v, (int, float)) and not isinstance(v, bool)
        if fn == "ISTEXT":
            h = self.hcell()
            return f"=ISTEXT({h})", isinstance(self.v(h), str)
        if fn == "ISLOGICAL":
            h = self.hcell()
            return f"=ISLOGICAL({h})", isinstance(self.v(h), bool)
        if fn == "ISERROR":
            return f"=ISERROR(1/{a})", x == 0
        if fn == "ISNA":
            return "=ISNA(NA())", True
        if fn == "ISEVEN":
            return f"=ISEVEN({a})", x % 2 == 0
        if fn == "ISODD":
            return f"=ISODD({a})", x % 2 == 1
        h = self.hcell()
        v = self.v(h)
        return f"=TYPE({h})", (1.0 if v is None else 4.0 if isinstance(v, bool) else 2.0 if isinstance(v, str) else 1.0)

    def t_lookup(self):
        r = self.r
        fn = r.choice(["INDEX", "MATCH", "VLOOKUP", "HLOOKUP", "XLOOKUP", "INDIRECT", "OFFSET", "ROW", "COLUMN", "ROWS",
                       "COLUMNS", "ADDRESS", "INDEX2", "XMATCH"])
        if fn == "INDEX":
            i, j = r.randint(1, 21), r.randint(1, 8)
            if i > 20:
                return f"=INDEX(A1:H20,{i},{j})", "#REF!"
            v = self.v(f"{'ABCDEFGH'[j - 1]}{i}")
            if v is None:
                return f"=INDEX(A1:H20,{i},{j})", 0.0
            if isinstance(v, bool) or isinstance(v, str):
                return f"=INDEX(A1:H20,{i},{j})", v
            return f"=INDEX(A1:H20,{i},{j})", float(v)
        if fn == "INDEX2":
            i = r.randint(1, 20)
            return f"=INDEX(A:A,{i})", float(self.v(f"A{i}"))
        if fn in ("MATCH", "XMATCH"):
            c = r.choice("ABCDE")
            vals = self.col_vals(c)
            n = r.choice(vals + [r.randint(-20, 60)])
            pos = float(vals.index(n) + 1) if n in vals else "#N/A"
            return (f"=MATCH({n},{c}:{c},0)" if fn == "MATCH" else f"=XMATCH({n},{c}1:{c}20)"), pos
        if fn == "VLOOKUP":
            vals = self.col_vals("A")
            n = r.choice(vals + [r.randint(-20, 60)])
            j = r.randint(1, 7)
            if n not in vals:
                return f"=VLOOKUP({n},A1:G20,{j},FALSE)", "#N/A"
            i = vals.index(n) + 1
            v = self.v(f"{'ABCDEFG'[j - 1]}{i}")
            return f"=VLOOKUP({n},A1:G20,{j},FALSE)", (float(v) if isinstance(v, (int, float)) else v)
        if fn == "HLOOKUP":
            row1 = [self.v(f"{c}1") for c in "ABCDE"]
            n = r.choice(row1 + [999])
            if n not in row1:
                return f"=HLOOKUP({n},A1:E5,3,FALSE)", "#N/A"
            j = row1.index(n)
            return f"=HLOOKUP({n},A1:E5,3,FALSE)", float(self.v(f"{'ABCDE'[j]}3"))
        if fn == "XLOOKUP":
            vals = self.col_vals("B")
            n = r.choice(vals + [r.randint(-20, 60)])
            if n not in vals:
                return f'=XLOOKUP({n},B1:B20,G1:G20,"none")', "none"
            i = vals.index(n) + 1
            return f'=XLOOKUP({n},B1:B20,G1:G20,"none")', self.v(f"G{i}")
        if fn == "INDIRECT":
            c = self.ncell()
            return f'=INDIRECT("{c}")', float(self.v(c))
        if fn == "OFFSET":
            i, j = r.randint(0, 19), r.randint(0, 4)
            return f"=OFFSET(A1,{i},{j})", float(self.v(f"{'ABCDE'[j]}{i + 1}"))
        if fn == "ROW":
            c = self.ncell()
            return f"=ROW({c})", float(int(c[1:]))
        if fn == "COLUMN":
            c = self.ncell()
            return f"=COLUMN({c})", float("ABCDE".index(c[0]) + 1)
        if fn == "ROWS":
            ref, vals = self.rng("ABC")
            a, b = ref.split(":")
            return f"=ROWS({ref})", float(int(b[1:]) - int(a[1:]) + 1)
        if fn == "COLUMNS":
            ref, vals = self.rng("ABCDE")
            a, b = ref.split(":")
            return f"=COLUMNS({ref})", float(ord(b[0]) - ord(a[0]) + 1)
        i, j = r.randint(1, 30), r.randint(1, 26)
        return f"=ADDRESS({i},{j})", f"${chr(64 + j)}${i}"

    def t_date(self):
        r = self.r
        y, m, d = r.randint(1950, 2080), r.randint(-3, 15), r.randint(-3, 35)
        mm = m - 1
        yy = y + mm // 12
        mm = mm % 12 + 1
        first = dt.date(yy, mm, 1)
        date = first + dt.timedelta(days=d - 1)
        s = serial(date)
        fn = r.choice(["DATE", "YEAR", "MONTH", "DAY", "WEEKDAY", "WEEKDAY2", "EDATE", "EOMONTH", "DAYS", "TEXT",
                       "DATEDIF", "DATEVALUE", "WEEKNUM"])
        if fn == "DATE":
            return f"=DATE({y},{m},{d})", float(s)
        if fn == "YEAR":
            return f"=YEAR({s})", float(date.year)
        if fn == "MONTH":
            return f"=MONTH({s})", float(date.month)
        if fn == "DAY":
            return f"=DAY({s})", float(date.day)
        if fn == "WEEKDAY":
            return f"=WEEKDAY({s})", float((date.weekday() + 1) % 7 + 1)
        if fn == "WEEKDAY2":
            return f"=WEEKDAY({s},2)", float(date.weekday() + 1)
        if fn in ("EDATE", "EOMONTH"):
            k = r.randint(-30, 30)
            mm = date.month - 1 + k
            yy = date.year + mm // 12
            mm = mm % 12 + 1
            nxt = dt.date(yy + (mm == 12), mm % 12 + 1, 1)
            last = nxt - dt.timedelta(days=1)
            if fn == "EDATE":
                return f"=EDATE({s},{k})", float(serial(dt.date(yy, mm, min(date.day, last.day))))
            return f"=EOMONTH({s},{k})", float(serial(last))
        if fn == "DAYS":
            k = r.randint(-500, 500)
            return f"=DAYS({s + k},{s})", float(k)
        if fn == "TEXT":
            return f'=TEXT({s},"yyyy-mm-dd")', date.isoformat()
        if fn == "DATEVALUE":
            return f'=DATEVALUE("{date.isoformat()}")', float(s)
        if fn == "WEEKNUM":
            jan1 = dt.date(date.year, 1, 1)
            wk = (date.timetuple().tm_yday + (jan1.weekday() + 1) % 7 - 1) // 7 + 1
            return f"=WEEKNUM({s})", float(wk)
        k = r.randint(0, 3000)
        d2 = date + dt.timedelta(days=k)
        unit = r.choice(["d", "m", "y"])
        if unit == "d":
            exp = float(k)
        elif unit == "y":
            exp = float(d2.year - date.year - ((d2.month, d2.day) < (date.month, date.day)))
        else:
            months = (d2.year - date.year) * 12 + d2.month - date.month
            if d2.day < date.day:
                months -= 1
            exp = float(months)
        return f'=DATEDIF({s},{s + k},"{unit}")', exp

    def t_array(self):
        r = self.r
        c = r.choice("ABCDE")
        r1, r2 = sorted(r.sample(range(1, 21), 2))
        vals = self.col_vals(c, r1, r2)
        ref = f"{c}{r1}:{c}{r2}"
        n = r.randint(-5, 40)
        fn = r.choice(["SUMSEQ", "SUMMUL", "SUMPRODUCTCMP", "SUMIFARR", "MAXABS", "INDEXSORT", "UNIQUE", "FILTER", "TAKE",
                       "DROP", "LET", "LAMBDA", "MAP", "SUMPRODUCT", "SORTBY", "BYROW", "REDUCE", "SEQUENCE", "TRANSPOSE",
                       "COUNTUNIQUE", "CHOOSECOLS"])
        if fn == "SUMSEQ":
            k = r.randint(1, 30)
            return f"=SUM(SEQUENCE({k}))", float(k * (k + 1) / 2)
        if fn == "SUMMUL":
            return f"=SUM({ref}*2)", float(2 * sum(vals))
        if fn == "SUMPRODUCTCMP":
            return f"=SUMPRODUCT(({ref}>{n})*1)", float(sum(1 for x in vals if x > n))
        if fn == "SUMIFARR":
            return f"=SUM(IF({ref}>{n},1,0))", float(sum(1 for x in vals if x > n))
        if fn == "MAXABS":
            return f"=MAX(ABS({ref}-{n}))", float(max(abs(x - n) for x in vals))
        if fn == "INDEXSORT":
            return f"=INDEX(SORT({ref}),1)", float(min(vals))
        if fn == "UNIQUE":
            return f"=ROWS(UNIQUE({ref}))", float(len(set(vals)))
        if fn == "COUNTUNIQUE":
            g = self.col_vals("G", r1, r2)
            return f"=ROWS(UNIQUE(G{r1}:G{r2}))", float(len({x.lower() for x in g}))
        if fn == "FILTER":
            hit = [x for x in vals if x > n]
            return f"=SUM(FILTER({ref},{ref}>{n},0))", float(sum(hit))
        if fn == "TAKE":
            k = r.randint(1, 3)
            return f"=SUM(TAKE({ref},{k}))", float(sum(vals[:k]))
        if fn == "DROP":
            k = r.randint(1, 3)
            return f"=SUM(DROP({ref},{k}))", (float(sum(vals[k:])) if k < len(vals) else "#CALC!")
        if fn == "LET":
            return f"=LET(x,{n},y,x*2,x+y)", float(n * 3)
        if fn == "LAMBDA":
            return f"=LAMBDA(a,b,a*b+1)({n},3)", float(n * 3 + 1)
        if fn == "MAP":
            return f"=SUM(MAP({ref},LAMBDA(v,v*v)))", float(sum(x * x for x in vals))
        if fn == "SUMPRODUCT":
            c2 = r.choice("ABCDE")
            v2 = self.col_vals(c2, r1, r2)
            return f"=SUMPRODUCT({ref},{c2}{r1}:{c2}{r2})", float(sum(x * y for x, y in zip(vals, v2)))
        if fn == "SORTBY":
            c2 = r.choice("ABCDE")
            v2 = self.col_vals(c2, r1, r2)
            pairs = sorted(zip(v2, range(len(vals))))
            return f"=INDEX(SORTBY({ref},{c2}{r1}:{c2}{r2}),1)", float(vals[pairs[0][1]])
        if fn == "BYROW":
            return f"=SUM(BYROW({ref},LAMBDA(rw,SUM(rw)*2)))", float(sum(vals) * 2)
        if fn == "REDUCE":
            return f"=REDUCE(0,{ref},LAMBDA(acc,v,acc+v))", float(sum(vals))
        if fn == "SEQUENCE":
            a, b = r.randint(1, 4), r.randint(1, 4)
            return f"=SUM(SEQUENCE({a},{b},{n},2))", float(sum(n + 2 * i for i in range(a * b)))
        if fn == "TRANSPOSE":
            return f"=COLUMNS(TRANSPOSE({ref}))", float(len(vals))
        return (f"=SUM(CHOOSECOLS(A{r1}:E{r2},1,2))",
                float(sum(self.col_vals("A", r1, r2)) + sum(self.col_vals("B", r1, r2))))

    def t_rank(self):
        r = self.r
        c = r.choice("ABCDE")
        vals = self.col_vals(c)
        i = r.randint(1, 20)
        x = vals[i - 1]
        k = r.random()
        if k < 0.5:
            return f"=RANK.EQ({c}{i},{c}1:{c}20)", float(sum(1 for v in vals if v > x) + 1)
        if k < 0.8:
            return f"=RANK.EQ({c}{i},{c}1:{c}20,1)", float(sum(1 for v in vals if v < x) + 1)
        from collections import Counter
        cnt = Counter(vals)
        best = max(cnt.values())
        if best == 1:
            return f"=MODE.SNGL({c}1:{c}20)", "#N/A"
        return f"=MODE.SNGL({c}1:{c}20)", float(next(v for v in vals if cnt[v] == best))

    def one(self):
        fns = [self.t_arith, self.t_power, self.t_percent, self.t_round, self.t_mod, self.t_intmath, self.t_agg,
               self.t_cond_agg, self.t_text_crit, self.t_text, self.t_compare, self.t_coerce, self.t_logic,
               self.t_lookup, self.t_date, self.t_array, self.t_rank]
        while True:
            f, e = self.r.choice(fns)()
            if e is not None:
                return f, e


def same(exp, got):
    from yc.types import XlError

    if isinstance(got, XlError):
        got = got.Text
    if isinstance(exp, str):
        return isinstance(got, str) and exp == got
    if isinstance(exp, bool):
        return isinstance(got, bool) and exp == got
    if isinstance(got, bool) or not isinstance(got, (int, float)):
        return False
    return abs(exp - got) <= 1e-9 * max(1.0, abs(exp))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--n", type=int, default=400, help="formulas per run")
    ap.add_argument("--exe", default=os.environ.get("YCGRID_EXE") or os.path.join(ROOT, "dist_release", "ycgrid.exe"))
    ap.add_argument("--eval", action="store_true", help="also check Application.Evaluate on the first 100")
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(a.exe)), "python"))
    import yc
    from yc import excel as X

    p = yc.launch(hidden=True, exe=a.exe, lang="en", size=(1200, 800))
    try:
        g = Gen(a.seed)
        ws = X.ActiveSheet
        ws.Range("A1:H20").Value = [[g.grid[f"{c}{i}"] for c in "ABCDEFGH"] for i in range(1, 21)]
        cases = [g.one() for _ in range(a.n)]
        ws.Range(f"K1:K{a.n}").Formula = [[f] for f, _ in cases]
        X.Application.CalculateFull()
        got = ws.Range(f"K1:K{a.n}").Value2
        bad = 0
        for (f, e), (gv,) in zip(cases, got):
            if not same(e, gv):
                bad += 1
                print(f"MISMATCH {f}  expected {e!r}  got {gv!r}")
        if a.eval:
            for f, e in cases[:100]:
                gv = X.Application.Evaluate(f)
                if not same(e, gv):
                    bad += 1
                    print(f"EVAL MISMATCH {f}  expected {e!r}  got {gv!r}")
        print(f"seed {a.seed}: {len(cases)} formulas, {bad} mismatches")
        return 1 if bad else 0
    finally:
        p.terminate()


if __name__ == "__main__":
    sys.exit(main())
