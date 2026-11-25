# SPDX-License-Identifier: Apache-2.0
"""The everyday work of a trader or researcher, checked by its results against Excel's: 300 common formulas
(aggregation, lookup, conditional sums, logic, text, dates, statistics, finance, LET / LAMBDA), 30 dynamic
arrays, what typing enters (dates, currency, percent, fractions, exponents), the editor (AutoComplete, pointing
with the arrows, AutoSum, F2, Escape, Undo / Redo), the common charts (column, line, bar, area, pie, scatter,
combo: series, titles, labels, legend, a changed value, the picture), a P&L sheet made with the keys (fill handle,
copy, Ctrl+D, format shortcuts, insert row, sort, filter, freeze, merge, conditional format, names, validation,
number formats), and a save / reopen round trip. The release check: every check here is one a person does daily.

Run: python -m unittest tests.test_release_common (with the bundled Python).
"""
from __future__ import annotations

import datetime
import math
import os
import statistics
import sys
import tempfile
import time
import unittest
from datetime import date

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402
from yc.types import XlError  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"

PRICES = [150.25, 300.5, 125, 200.75, 130]
QTY = [10, 5, 8, 12, 7]
TICKERS = ["AAPL", "MSFT", "GOOG", "TSLA", "AMZN"]
ACCENT1, ACCENT2 = (0x44, 0x72, 0xC4), (0xED, 0x7D, 0x31)  # Office theme: the first two series


def serial(y, m, d):
    return (date(y, m, d) - date(1899, 12, 30)).days


DATES = [serial(2024, 1, 15), serial(2024, 2, 20), serial(2024, 3, 5), serial(2024, 4, 10), serial(2024, 5, 25)]


def same(actual, expected, tol=1e-9):
    """actual (as Range.Value gives it) is expected: an error text "#N/A", a bool, a number (tol), text, or rows."""
    if isinstance(actual, datetime.datetime):  # dates come back as datetime, as from Excel's COM
        actual = (actual - datetime.datetime(1899, 12, 30)).total_seconds() / 86400
    if isinstance(expected, str) and expected.startswith("#"):
        return isinstance(actual, XlError) and actual.Text == expected
    if isinstance(expected, bool):
        return isinstance(actual, bool) and actual == expected
    if isinstance(expected, (int, float)):
        if isinstance(actual, bool) or not isinstance(actual, (int, float)):
            return False
        return math.isclose(actual, expected, rel_tol=tol, abs_tol=tol)
    if isinstance(expected, list):
        if not isinstance(actual, list):
            actual = [[actual]]
        if actual and not isinstance(actual[0], list):
            actual = [actual]
        return len(actual) == len(expected) and all(
            len(a) == len(e) and all(same(x, y, tol) for x, y in zip(a, e)) for a, e in zip(actual, expected))
    return actual == expected


def show(v):
    return v.Text if isinstance(v, XlError) else repr(v)


def read_png(path):
    """(width, height, pixel(x, y) -> (r, g, b)) of an 8-bit RGB / RGBA PNG, with the standard library."""
    import struct
    import zlib

    with open(path, "rb") as fh:
        data = fh.read()
    pos, idat, w, h, ctype = 8, b"", 0, 0, 2
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, lambda x, y: tuple(rows[y][x * bpp:x * bpp + 3])


def color_count(path, ref, tol=60):
    w, h, px = read_png(path)
    return sum(1 for y in range(0, h, 2) for x in range(0, w, 2) if sum(abs(a - b) for a, b in zip(px(x, y), ref)) <= tol)


def expected_values():
    """Excel's results worked out in Python where a closed form exists (statistics, finance)."""
    n = 5
    mean_q = statistics.mean(QTY)
    sd_q = statistics.stdev(QTY)
    skew = n / ((n - 1) * (n - 2)) * sum(((x - mean_q) / sd_q) ** 3 for x in QTY)
    kurt = (n * (n + 1) / ((n - 1) * (n - 2) * (n - 3))) * sum(((x - mean_q) / sd_q) ** 4 for x in QTY) - 3 * (n - 1) ** 2 / ((n - 2) * (n - 3))
    lr = statistics.linear_regression(PRICES, QTY)
    corr = statistics.correlation(PRICES, QTY)
    nd = statistics.NormalDist()

    def pmt(r, nper, pv, fv=0.0):
        f = (1 + r) ** nper
        return -(pv * f + fv) * r / (f - 1)

    def fv_(r, nper, p):
        f = (1 + r) ** nper
        return -(p * (f - 1) / r)

    def pv_(r, nper, p):
        f = (1 + r) ** nper
        return -(p * (f - 1) / r) / f

    def solve(f, lo, hi):
        for _ in range(300):
            mid = (lo + hi) / 2
            if f(lo) * f(mid) <= 0:
                hi = mid
            else:
                lo = mid
        return (lo + hi) / 2

    xd = [serial(2024, 1, 1), serial(2024, 7, 1), serial(2025, 1, 1)]
    xv = [-1000, 500, 600]
    r = 0.05 / 12
    p = pmt(r, 360, 200000)
    return dict(
        sd=sd_q, skew=skew, kurt=kurt, slope=lr.slope, intercept=lr.intercept, corr=corr, mean_q=mean_q, nd=nd,
        pmt=p, fv=fv_(r, 120, -100), pv=pv_(0.08, 10, -1000), npv=100 / 1.1 + 200 / 1.21 + 300 / 1.331,
        irr=solve(lambda x: -100 + 30 / (1 + x) + 40 / (1 + x) ** 2 + 50 / (1 + x) ** 3, 0.0001, 1.0),
        rate=solve(lambda x: 1000 * (1 + x) ** 12 - 100 * ((1 + x) ** 12 - 1) / x, 0.0001, 1.0),
        xd=xd, xirr=solve(lambda x: sum(v / (1 + x) ** ((d - xd[0]) / 365) for v, d in zip(xv, xd)), 0.0001, 1.0),
        xnpv=sum(v / 1.1 ** ((d - xd[0]) / 365) for v, d in zip(xv, xd)),
        cumipmt=sum(-(200000 * (1 + r) ** (k - 1) + p * ((1 + r) ** (k - 1) - 1) / r) * r for k in range(1, 13)),
        pmt2=pmt(0.06 / 12, 36, -20000), mirr=((30 * 1.12 ** 2 + 40 * 1.12 + 50) / 100) ** (1 / 3) - 1,
    )


def formula_cases():
    """(formula, Excel's result[, tolerance]) on a sheet holding the data table in A1:D6."""
    e = expected_values()
    nd = e["nd"]
    xd = e["xd"]
    return [
        # math and aggregation
        ("=SUM(B2:B6)", 906.5), ("=AVERAGE(B2:B6)", 181.3), ("=MAX(B2:B6)", 300.5), ("=MIN(B2:B6)", 125),
        ("=COUNT(B2:B6)", 5), ("=COUNTA(A1:A6)", 6), ("=COUNTBLANK(A1:A8)", 2), ("=SUMPRODUCT(B2:B6,C2:C6)", 7324),
        ("=SUMPRODUCT((C2:C6>7)*B2:B6)", 476), ("=PRODUCT(2,3,4)", 24), ("=SUMSQ(3,4)", 25),
        ("=ROUND(B2*1.1,1)", 165.3), ("=ROUND(2.5,0)", 3), ("=ROUND(-2.5,0)", -3), ("=ROUND(1.005,2)", 1.01),
        ("=ROUNDUP(1.231,1)", 1.3), ("=ROUNDDOWN(1.299,1)", 1.2), ("=INT(-3.5)", -4), ("=TRUNC(-3.5)", -3),
        ("=MOD(-7,3)", 2), ("=MOD(7,-3)", -2), ("=ABS(-5)", 5), ("=SQRT(144)", 12), ("=POWER(2,10)", 1024),
        ("=2^0.5", math.sqrt(2)), ("=EXP(0)", 1), ("=LN(EXP(2))", 2), ("=LOG10(1000)", 3), ("=LOG(8,2)", 3),
        ("=CEILING.MATH(4.3)", 5), ("=FLOOR.MATH(-4.3)", -5), ("=MROUND(17,5)", 15), ("=SIGN(-3)", -1),
        ("=PI()", math.pi), ("=FACT(5)", 120), ("=COMBIN(5,2)", 10), ("=GCD(12,18)", 6), ("=LCM(4,6)", 12),
        ("=ISEVEN(4)", True), ("=ISODD(4)", False), ("=QUOTIENT(17,5)", 3), ("=RANDBETWEEN(1,1)", 1),
        ("=RAND()<1", True), ("=EVEN(3)", 4), ("=ODD(4)", 5), ("=-2^2", 4), ("=2^3^2", 64), ("=1+2*3", 7),
        ("=(1+2)*3", 9), ("=10%", 0.1), ("=50%*200", 100), ("=1E3", 1000), ("=TRUE+TRUE", 2),
        ("=SUM(\"3\",4)", 7), ("=\"5\"+5", 10), ("=\"a\"+1", "#VALUE!"), ("=1/0", "#DIV/0!"), ("=FOO()", "#NAME?"),
        ("=A2+1", "#VALUE!"), ("=SUM(A2:A6)", 0), ("=1=1", True), ("=\"A\"=\"a\"", True),
        ("=SUM(B2:B6)/COUNT(B2:B6)=AVERAGE(B2:B6)", True), ("=AGGREGATE(9,6,B2:B6)", 906.5),
        ("=SUBTOTAL(9,B2:B6)", 906.5), ("=SUBTOTAL(1,B2:B6)", 181.3), ("=SUBTOTAL(109,B2:B6)", 906.5),
        # lookup
        ("=VLOOKUP(\"GOOG\",A2:C6,2,FALSE)", 125), ("=VLOOKUP(\"ZZZ\",A2:C6,2,FALSE)", "#N/A"),
        ("=IFERROR(VLOOKUP(\"ZZZ\",A2:C6,2,FALSE),\"none\")", "none"), ("=XLOOKUP(\"TSLA\",A2:A6,C2:C6)", 12),
        ("=XLOOKUP(\"X\",A2:A6,C2:C6,\"nf\")", "nf"), ("=INDEX(B2:B6,MATCH(\"MSFT\",A2:A6,0))", 300.5),
        ("=MATCH(200.75,B2:B6,0)", 4), ("=HLOOKUP(\"Qty\",A1:D6,3,FALSE)", 5), ("=XMATCH(\"GOOG\",A2:A6)", 3),
        ("=OFFSET(A1,1,1)", 150.25), ("=INDIRECT(\"B3\")", 300.5), ("=ADDRESS(2,3)", "$C$2"), ("=ADDRESS(2,3,4)", "C2"),
        ("=ROWS(A2:A6)", 5), ("=COLUMNS(A1:C1)", 3), ("=ROW(B5)", 5), ("=COLUMN(C1)", 3), ("=CHOOSE(3,\"a\",\"b\",\"c\")", "c"),
        ("=SUM(OFFSET(B2,0,0,3,1))", 575.75), ("=LOOKUP(2,1/(A2:A6=\"TSLA\"),B2:B6)", 200.75),
        ("=INDEX(A1:D6,3,1)", "MSFT"), ("=VLOOKUP(7.5,{5,\"a\";7,\"b\";9,\"c\"},2,TRUE)", "b"), ("=MATCH(\"T*\",A2:A6,0)", 4),
        ("=MATCH(7.5,{5,7,9},1)", 2), ("=XLOOKUP(7.5,{5,7,9},{\"a\",\"b\",\"c\"},,-1)", "b"), ("=XLOOKUP(7.5,{5,7,9},{\"a\",\"b\",\"c\"},,1)", "c"),
        # conditional aggregation
        ("=SUMIF(C2:C6,\">7\",B2:B6)", 476), ("=SUMIFS(B2:B6,C2:C6,\">=7\",A2:A6,\"A*\")", 280.25),
        ("=COUNTIF(A2:A6,\"A*\")", 2), ("=COUNTIFS(C2:C6,\">5\",B2:B6,\"<200\")", 3),
        ("=AVERAGEIF(C2:C6,\">=8\",B2:B6)", 476 / 3), ("=MAXIFS(B2:B6,C2:C6,\"<10\")", 300.5),
        ("=MINIFS(B2:B6,C2:C6,\">=10\")", 150.25), ("=COUNTIF(B2:B6,\">\"&AVERAGE(B2:B6))", 2),
        ("=SUMIF(A2:A6,\"aapl\",B2:B6)", 150.25), ("=COUNTIF(A2:A6,\"?????\")", 0), ("=COUNTIF(A2:A6,\"????\")", 5),
        # logic
        ("=IF(B2>100,\"High\",\"Low\")", "High"), ("=IFS(B4>200,\"A\",B4>100,\"B\",TRUE,\"C\")", "B"),
        ("=AND(B2>100,C2>5)", True), ("=OR(B2>1000,C2>5)", True), ("=NOT(TRUE)", False), ("=XOR(TRUE,TRUE)", False),
        ("=SWITCH(2,1,\"one\",2,\"two\",\"other\")", "two"), ("=IFNA(NA(),\"x\")", "x"), ("=IF(A9=\"\",\"empty\",\"full\")", "empty"),
        ("=ISBLANK(A9)", True), ("=ISNUMBER(B2)", True), ("=ISTEXT(A2)", True), ("=ISERROR(1/0)", True),
        ("=ISNA(NA())", True), ("=ERROR.TYPE(1/0)", 2), ("=TYPE(\"a\")", 2), ("=N(\"x\")", 0), ("=T(\"x\")", "x"),
        ("=ISLOGICAL(TRUE)", True), ("=ISFORMULA(B2)", False), ("=IF(1,2)", 2), ("=IF(0,2)", False),
        # text
        ("=LEFT(\"YCGrid\",2)", "YC"), ("=RIGHT(\"YCGrid\",4)", "Grid"), ("=MID(\"YCGrid\",3,2)", "Gr"), ("=LEN(\"YCGrid\")", 6),
        ("=FIND(\"G\",\"YCGrid\")", 3), ("=SEARCH(\"grid\",\"YCGrid\")", 3), ("=FIND(\"g\",\"YCGrid\")", "#VALUE!"),
        ("=SUBSTITUTE(\"a-b-c\",\"-\",\"+\")", "a+b+c"), ("=SUBSTITUTE(\"a-b-c\",\"-\",\"+\",2)", "a-b+c"),
        ("=REPLACE(\"abcdef\",2,3,\"X\")", "aXef"), ("=TRIM(\"  a   b  \")", "a b"), ("=UPPER(\"abc\")", "ABC"),
        ("=LOWER(\"ABC\")", "abc"), ("=PROPER(\"hello world\")", "Hello World"), ("=CONCAT(\"a\",\"b\",1)", "ab1"),
        ("=CONCATENATE(\"x\",\"y\")", "xy"), ("=\"a\"&\"b\"", "ab"), ("=TEXTJOIN(\",\",TRUE,A2:A4)", "AAPL,MSFT,GOOG"),
        ("=TEXTJOIN(\"-\",TRUE,\"a\",\"\",\"b\")", "a-b"), ("=TEXTJOIN(\"-\",FALSE,\"a\",\"\",\"b\")", "a--b"),
        ("=VALUE(\"12.5\")", 12.5), ("=TEXT(1234.567,\"#,##0.00\")", "1,234.57"), ("=TEXT(0.256,\"0.0%\")", "25.6%"),
        ("=TEXT(DATE(2024,3,5),\"yyyy-mm-dd\")", "2024-03-05"), ("=TEXT(DATE(2024,3,5),\"mmm d, yyyy\")", "Mar 5, 2024"),
        ("=TEXT(-5,\"$#,##0;($#,##0)\")", "($5)"), ("=TEXT(D2,\"dddd\")", "Monday"), ("=TEXT(0.5,\"h:mm AM/PM\")", "12:00 PM"),
        ("=TEXT(1234567,\"0.00E+00\")", "1.23E+06"), ("=TEXT(12,\"000\")", "012"), ("=TEXT(0.125,\"# ?/?\")", " 1/8"),
        ("=REPT(\"ab\",3)", "ababab"), ("=CHAR(65)", "A"), ("=CODE(\"A\")", 65), ("=EXACT(\"a\",\"A\")", False),
        ("=NUMBERVALUE(\"1.234,56\",\",\",\".\")", 1234.56), ("=DOLLAR(1234.5)", "$1,234.50"), ("=FIXED(1234.567,1)", "1,234.6"),
        ("=CLEAN(\"a\"&CHAR(10)&\"b\")", "ab"), ("=LEN(TRIM(\" x \"))", 1), ("=TEXTBEFORE(\"a@b.com\",\"@\")", "a"),
        ("=TEXTAFTER(\"a@b.com\",\"@\")", "b.com"), ("=UNICODE(\"A\")", 65), ("=A2&\" \"&B2", "AAPL 150.25"),
        ("=\"Total: \"&TEXT(SUM(B2:B6),\"#,##0.0\")", "Total: 906.5"), ("=LEN(A2)", 4), ("=VALUE(\"$1,234\")", 1234),
        ("=\"1\"=1", False), ("=UPPER(A2)=\"AAPL\"", True), ("=MID(\"abc\",5,2)", ""), ("=LEFT(123,2)", "12"),
        # dates and times
        ("=DATE(2024,2,29)", serial(2024, 2, 29)), ("=YEAR(D2)", 2024), ("=MONTH(D3)", 2), ("=DAY(D4)", 5),
        ("=EOMONTH(D2,0)", serial(2024, 1, 31)), ("=EOMONTH(D2,1)", serial(2024, 2, 29)), ("=EDATE(D2,1)", serial(2024, 2, 15)),
        ("=DATEDIF(D2,D6,\"d\")", 131), ("=DATEDIF(D2,D6,\"m\")", 4), ("=DATEDIF(D2,D6,\"y\")", 0), ("=WEEKDAY(D2)", 2),
        ("=WEEKDAY(D2,2)", 1), ("=NETWORKDAYS(D2,D3)", 27), ("=WORKDAY(D2,5)", serial(2024, 1, 22)), ("=DAYS(D3,D2)", 36),
        ("=DATEVALUE(\"2024-01-15\")", serial(2024, 1, 15)), ("=DATEVALUE(\"1/15/2024\")", serial(2024, 1, 15)),
        ("=TODAY()>45000", True), ("=NOW()>TODAY()", True), ("=HOUR(TIME(13,45,10))", 13), ("=MINUTE(TIME(13,45,10))", 45),
        ("=SECOND(TIME(13,45,10))", 10), ("=TIME(13,45,10)", 49510 / 86400), ("=YEARFRAC(D2,D6)", 130 / 360),
        ("=ISOWEEKNUM(D2)", 3), ("=WEEKNUM(D2)", 3), ("=DAYS360(D2,D6)", 130), ("=D2+1", serial(2024, 1, 16)),
        ("=MONTH(EDATE(DATE(2024,1,31),1))", 2), ("=D3-D2", 36), ("=DATE(2024,13,1)", serial(2025, 1, 1)),
        ("=DATE(2024,3,0)", serial(2024, 2, 29)), ("=TIMEVALUE(\"6:00 PM\")", 0.75), ("=D2>D3", False),
        ("=WORKDAY(DATE(2024,1,5),1)", serial(2024, 1, 8)), ("=NETWORKDAYS(DATE(2024,1,1),DATE(2024,1,31))", 23),
        ("=EOMONTH(DATE(2024,1,31),1)", serial(2024, 2, 29)), ("=EDATE(DATE(2024,1,31),1)", serial(2024, 2, 29)),
        # statistics
        ("=MEDIAN(B2:B6)", 150.25), ("=LARGE(B2:B6,2)", 200.75), ("=SMALL(B2:B6,2)", 130), ("=RANK(B3,B2:B6)", 1),
        ("=RANK.EQ(B4,B2:B6)", 5), ("=RANK(B4,B2:B6,1)", 1), ("=STDEV.S(C2:C6)", e["sd"]), ("=STDEV(C2:C6)", e["sd"]),
        ("=STDEV.P(C2:C6)", statistics.pstdev(QTY)), ("=VAR.S(C2:C6)", statistics.variance(QTY)), ("=VAR.P(C2:C6)", statistics.pvariance(QTY)),
        ("=CORREL(B2:B6,C2:C6)", e["corr"]), ("=PEARSON(B2:B6,C2:C6)", e["corr"]), ("=RSQ(C2:C6,B2:B6)", e["corr"] ** 2),
        ("=SLOPE(C2:C6,B2:B6)", e["slope"]), ("=INTERCEPT(C2:C6,B2:B6)", e["intercept"]),
        ("=FORECAST.LINEAR(150,C2:C6,B2:B6)", e["intercept"] + e["slope"] * 150), ("=FORECAST(150,C2:C6,B2:B6)", e["intercept"] + e["slope"] * 150),
        ("=COVARIANCE.S(B2:B6,C2:C6)", statistics.covariance(PRICES, QTY)), ("=COVARIANCE.P(B2:B6,C2:C6)", statistics.covariance(PRICES, QTY) * 4 / 5),
        ("=GEOMEAN(C2:C6)", statistics.geometric_mean(QTY)), ("=HARMEAN(C2:C6)", statistics.harmonic_mean(QTY)),
        ("=AVEDEV(C2:C6)", sum(abs(x - e["mean_q"]) for x in QTY) / 5), ("=DEVSQ(C2:C6)", 29.2), ("=SKEW(C2:C6)", e["skew"]), ("=KURT(C2:C6)", e["kurt"]),
        ("=PERCENTILE.INC(C2:C6,0.4)", 7.6), ("=PERCENTILE.EXC(C2:C6,0.5)", 8), ("=QUARTILE.INC(C2:C6,3)", 10), ("=QUARTILE(C2:C6,1)", 7),
        ("=PERCENTRANK.INC(C2:C6,8)", 0.5), ("=MODE(1,2,2,3)", 2), ("=MODE.SNGL(1,2,2,3)", 2), ("=TRIMMEAN(C2:C6,0.4)", 25 / 3),
        ("=NORM.S.DIST(1.96,TRUE)", nd.cdf(1.96)), ("=NORM.INV(0.975,0,1)", nd.inv_cdf(0.975)), ("=NORM.DIST(0,0,1,FALSE)", nd.pdf(0)),
        ("=NORMSDIST(1.96)", nd.cdf(1.96)), ("=NORM.S.INV(0.05)", nd.inv_cdf(0.05)), ("=STANDARDIZE(12,8.4,2)", 1.8),
        ("=MAX(C2:C6)-MIN(C2:C6)", 7), ("=COUNT(A2:D6)", 15), ("=AVERAGEA(1,TRUE,3)", 5 / 3), ("=COUNTA(A2:D6)", 20),
        # finance
        ("=PMT(0.05/12,360,200000)", e["pmt"]), ("=PMT(0,12,1200)", -100), ("=FV(0.05/12,120,-100)", e["fv"]), ("=PV(0.08,10,-1000)", e["pv"]),
        ("=NPV(0.1,100,200,300)", e["npv"]), ("=IRR({-100,30,40,50})", e["irr"], 1e-6), ("=RATE(12,-100,1000)", e["rate"], 1e-6),
        ("=XIRR({-1000,500,600},{%d,%d,%d})" % tuple(xd), e["xirr"], 1e-6), ("=XNPV(0.1,{-1000,500,600},{%d,%d,%d})" % tuple(xd), e["xnpv"], 1e-7),
        ("=NPER(0.05,-100,1000)", math.log(2) / math.log(1.05)), ("=SLN(10000,1000,5)", 1800), ("=EFFECT(0.05,12)", (1 + 0.05 / 12) ** 12 - 1),
        ("=NOMINAL(EFFECT(0.05,12),12)", 0.05), ("=IPMT(0.05/12,1,360,200000)", -200000 * 0.05 / 12),
        ("=PPMT(0.05/12,1,360,200000)", e["pmt"] + 200000 * 0.05 / 12), ("=CUMIPMT(0.05/12,360,200000,1,12,0)", e["cumipmt"]),
        ("=DB(10000,1000,5,1)", 10000 * round(1 - (1000 / 10000) ** (1 / 5), 3)), ("=SYD(10000,1000,5,1)", 3000), ("=DDB(10000,1000,5,1)", 4000),
        ("=PMT(0.06/12,36,-20000)", e["pmt2"]), ("=FV(0.07,10,0,-1000)", 1000 * 1.07 ** 10), ("=PV(0.05,1,0,-105)", 100),
        ("=MIRR({-100,30,40,50},0.1,0.12)", e["mirr"]),
        # LET, LAMBDA and arrays reduced to one value
        ("=LET(x,2,y,3,x*y)", 6), ("=LAMBDA(x,x*2)(4)", 8), ("=REDUCE(0,C2:C6,LAMBDA(a,b,a+b))", 42),
        ("=SUM(MAP(C2:C6,LAMBDA(v,v*2)))", 84), ("=SUM(SEQUENCE(10))", 55), ("=SUMPRODUCT(--(A2:A6=\"AAPL\"),B2:B6)", 150.25),
        ("=MAX(IF(C2:C6>7,B2:B6))", 200.75), ("=COUNT(UNIQUE(C2:C6))", 5), ("=INDEX(SORT(B2:B6),1)", 125),
        ("=SUM(FILTER(B2:B6,C2:C6>7))", 476), ("=ROWS(FILTER(A2:A6,C2:C6>100,\"none\"))", 1), ("=FILTER(A2:A6,C2:C6>100,\"none\")", "none"),
        ("=TEXTJOIN(\",\",TRUE,SORT(A2:A6))", "AAPL,AMZN,GOOG,MSFT,TSLA"), ("=INDEX(SORTBY(A2:A6,B2:B6),1)", "GOOG"),
        ("=SUM(TAKE(C2:C6,2))", 15), ("=SUM(DROP(C2:C6,4))", 7), ("=SUM(MMULT({1,2;3,4},{1;1}))", 10),
        ("=SUM(TRANSPOSE(C2:C6))", 42), ("=SUM(IF(ISNUMBER(B2:B6),B2:B6,0))", 906.5), ("=SUMPRODUCT(LEN(A2:A6))", 20),
        ("=AVERAGE(IF(A2:A6<>\"MSFT\",B2:B6))", 606 / 4), ("=SUM(B2:B6*(C2:C6>=8))", 476),
        ("=LET(r,B2:B6,SUM(r)/COUNT(r))", 181.3), ("=SUM(SCAN(0,C2:C6,LAMBDA(a,b,a+b)))", 125),
        ("=BYROW(A2:C3,LAMBDA(r,SUM(r)))", [[160.25], [305.5]]),
    ]


SPILL_CASES = [
    ("=SORT(C2:C6)", [[5], [7], [8], [10], [12]]), ("=SORT(B2:B6,,-1)", [[300.5], [200.75], [150.25], [130], [125]]),
    ("=UNIQUE({1;2;2;3})", [[1], [2], [3]]), ("=FILTER(A2:A6,C2:C6>7)", [["AAPL"], ["GOOG"], ["TSLA"]]),
    ("=SEQUENCE(3)", [[1], [2], [3]]), ("=SEQUENCE(1,3,10,5)", [[10, 15, 20]]), ("=SORTBY(A2:A6,B2:B6)", [["GOOG"], ["AMZN"], ["AAPL"], ["TSLA"], ["MSFT"]]),
    ("=TRANSPOSE(A2:A3)", [["AAPL", "MSFT"]]), ("=MAP(C2:C6,LAMBDA(v,v*2))", [[20], [10], [16], [24], [14]]),
    ("=VSTACK(A2,A3)", [["AAPL"], ["MSFT"]]), ("=HSTACK(A2,B2)", [["AAPL", 150.25]]), ("=TAKE(A2:A6,2)", [["AAPL"], ["MSFT"]]),
    ("=DROP(A2:A6,4)", [["AMZN"]]), ("=CHOOSECOLS(A1:C2,2)", [["Price"], [150.25]]), ("=TOCOL({1,2;3,4})", [[1], [2], [3], [4]]),
    ("=INDEX(A2:C6,0,1)", [[t] for t in TICKERS]), ("=MMULT({1,2;3,4},{1;1})", [[3], [7]]), ("=TEXTSPLIT(\"a,b,c\",\",\")", [["a", "b", "c"]]),
    ("=B2:B3", [[150.25], [300.5]]), ("=C2:C6*2", [[20], [10], [16], [24], [14]]), ("=A2:A3&\"-\"&C2:C3", [["AAPL-10"], ["MSFT-5"]]),
    ("=SEQUENCE(2,2)", [[1, 2], [3, 4]]), ("=XLOOKUP(\"MSFT\",A2:A6,B2:D6)", [[300.5, 5, DATES[1]]]),
    ("=WRAPROWS(SEQUENCE(6),3)", [[1, 2, 3], [4, 5, 6]]), ("=EXPAND({1},1,2,0)", [[1, 0]]),
    ("=SORT(A1:C3,2,-1,FALSE)", [["Ticker", "Price", "Qty"], ["MSFT", 300.5, 5], ["AAPL", 150.25, 10]]),
    ("=UNIQUE(A2:A6&\"\")", [[t] for t in TICKERS]), ("=LARGE(B2:B6,{1,2})", [[300.5, 200.75]]), ("=ROW(A1:A3)", [[1], [2], [3]]),
    ("=IF(C2:C6>7,\"big\",\"small\")", [["big"], ["small"], ["big"], ["big"], ["small"]]),
]

# what typing enters: (typed, Value, Text or the texts a region may show)
TYPED_CASES = [
    ("=SUM(B2:B6)", 906.5, "906.5"), ("=B2*C2", 1502.5, "1502.5"), ("=b2+b3", 450.75, "450.75"),
    ("1/15/2024", serial(2024, 1, 15), ("1/15/2024", "2024/1/15")), ("15:30", 15.5 / 24, "15:30"), ("$1,234.50", 1234.5, "$1,234.50 "),
    ("12.5%", 0.125, "12.50%"), ("1,234", 1234, "1,234"), ("007", 7, "7"), ("'007", "007", "007"), ("true", True, "TRUE"),
    ("3 1/2", 3.5, "3 1/2"), ("(5)", -5, "-5"), ("1e3", 1000, "1.00E+03"), ("-$5", -5, "($5)"), ("hello", "hello", "hello"),
    ("=1/0", "#DIV/0!", "#DIV/0!"), ("=AVERAGE(B2:B6)", 181.3, "181.3"), ("=TEXT(B2,\"$0.00\")", "$150.25", "$150.25"),
    ("=D2", serial(2024, 1, 15), "1/15/2024"), ("=B2+B3", 450.75, "450.75"), ("1.5e-3", 0.0015, "1.50E-03"), ("-0.5", -0.5, "-0.5"),
    ("=IF(B2>100,\"yes\",\"no\")", "yes", "yes"), ("=sum(b2:b6)", 906.5, "906.5"),
]

MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun"]
REV = [120, 135, 150, 142, 160, 175]
COST = [80, 85, 95, 90, 100, 105]


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ReleaseCommonTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en", size=(1600, 1000))
        cls.conn = yc.connect(target=cls.proc.pid)
        cls.n = 0

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel, ui

        ui.clear_errors()
        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        type(self).n += 1
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Name = f"T{self.n}"
        self.ws.Activate()
        self.keys("w:2")

    def tearDown(self):
        from yc import ui

        self.keys("k:Escape|w:2|k:Escape|w:2|m:2,2|w:2")
        errs = [e["text"] for e in ui.errors(errors_only=True)]
        self.assertEqual(errs, [], "the Error Log stays empty")
        st = ui.state()
        self.assertFalse(st.get("imgui_asserts") or st.get("shrinking_windows") or st.get("clipped_windows"), st)

    # ---- helpers ----
    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 20000}, timeout=40)
        time.sleep(0.15)

    def data_table(self, ws):
        ws.Range("A1:D1").Value = [["Ticker", "Price", "Qty", "Date"]]
        ws.Range("A2:C6").Value = [[t, p, q] for t, p, q in zip(TICKERS, PRICES, QTY)]
        ws.Range("D2:D6").Value = [[d] for d in DATES]
        ws.Range("D2:D6").NumberFormat = "m/d/yyyy"

    def chart_data(self, ws):
        ws.Range("A1:C1").Value = [["Month", "Revenue", "Cost"]]
        ws.Range("A2:C7").Value = [[m, r, c] for m, r, c in zip(MONTHS, REV, COST)]

    def shot_dir(self):
        d = os.path.join(tempfile.gettempdir(), "yc_release_common")
        os.makedirs(d, exist_ok=True)
        return d

    # ---- formulas ----
    def test_common_formulas_as_excel(self):
        ws = self.ws
        self.data_table(ws)
        bad = []
        for i, case in enumerate(formula_cases()):
            f, expected = case[0], case[1]
            tol = case[2] if len(case) > 2 else 1e-9
            addr = f"P{i + 1}"
            ws.Range(addr).Formula = f
            v = ws.Range(addr + "#").Value if isinstance(expected, list) else ws.Range(addr).Value
            if not same(v, expected, tol):
                bad.append(f"{f}: got {show(v)}, Excel {expected!r}")
        self.assertEqual(bad, [], "\n".join(bad))

    def test_dynamic_arrays_spill_as_excel(self):
        ws = self.ws
        self.data_table(ws)
        bad = []
        for i, (f, expected) in enumerate(SPILL_CASES):
            addr = f"T{i * 8 + 1}"
            ws.Range(addr).Formula = f
            v = ws.Range(addr + "#").Value
            if not same(v, expected):
                bad.append(f"{f}: got {show(v)}, Excel {expected!r}")
        self.assertEqual(bad, [], "\n".join(bad))
        # across sheets
        other = self.ws.Parent.Worksheets.Add(After=self.ws)
        other.Range("A1").Formula = f"=SORT({ws.Name}!C2:C6)"
        other.Range("B1").Formula = f"=INDEX({ws.Name}!B2:B6,MATCH(\"TSLA\",{ws.Name}!A2:A6,0))"
        self.assertEqual(other.Range("A1#").Value, [[5], [7], [8], [10], [12]])
        self.assertEqual(other.Range("B1").Value, 200.75)
        ws.Activate()

    # ---- the editor ----
    def test_typing_and_the_editor_as_excel(self):
        ws = self.ws
        self.data_table(ws)
        bad = []
        for i, (text, ev, et) in enumerate(TYPED_CASES):
            addr = f"J{i + 2}"
            self.keys(f"g:{addr}|w:2|t:{text}|k:Enter|w:4")
            v, t = ws.Range(addr).Value, ws.Range(addr).Text
            if not (same(v, ev) and (t == et if isinstance(et, str) else t in et)):
                bad.append(f"{text!r}: got {show(v)} / {t!r}, Excel {ev!r} / {et!r}")
        self.assertEqual(bad, [], "\n".join(bad))
        # AutoComplete: =sum, Tab, the arguments
        self.keys("g:J40|w:2|t:=sum|w:4|k:Tab|w:3|t:B2:B3)|k:Enter|w:4")
        self.assertEqual(ws.Range("J40").Formula.upper(), "=SUM(B2:B3)")
        self.assertEqual(ws.Range("J40").Value, 450.75)
        # pointing with the arrows while typing
        self.keys("g:J41|w:2|t:=|" + "|".join(["k:Left"] * 9) + "|w:2|t:*2|k:Enter|w:4")
        self.assertEqual(ws.Range("J41").Formula, "=A41*2")
        # AutoSum under the Price column
        self.keys("g:B7|w:2|k:a-=|w:4|k:Enter|w:4")
        self.assertEqual(ws.Range("B7").Formula, "=SUM(B2:B6)")
        self.assertEqual(ws.Range("B7").Value, 906.5)
        self.keys("g:B7|w:2|k:Delete|w:3")
        # F2 appends, Escape drops
        self.keys("g:J2|w:2|k:F2|w:2|t:/2|k:Enter|w:4")
        self.assertEqual(ws.Range("J2").Formula, "=SUM(B2:B6)/2")
        self.assertEqual(ws.Range("J2").Value, 453.25)
        self.keys("g:J2|w:2|k:F2|w:2|t:*999|k:Escape|w:4")
        self.assertEqual(ws.Range("J2").Formula, "=SUM(B2:B6)/2")
        # Undo / Redo of typing
        self.keys("g:J50|w:2|t:123|k:Enter|w:3|k:c-Z|w:4")
        self.assertIsNone(ws.Range("J50").Value)
        self.keys("k:c-Y|w:4")
        self.assertEqual(ws.Range("J50").Value, 123)
        # a typed change recalculates, and Undo of it too
        self.keys("g:B2|w:2|t:100|k:Enter|w:4")
        self.assertEqual(ws.Range("J2").Value, (906.5 - 50.25) / 2)
        self.assertEqual(ws.Range("J3").Value, 1000)
        self.keys("k:c-Z|w:4")
        self.assertEqual(ws.Range("B2").Value, 150.25)
        self.assertEqual(ws.Range("J2").Value, 453.25)
        # General shows up to 11 digits, as Excel's
        ws.Range("F1").Formula = "=1/3"
        ws.Range("F2").Formula = "=123456789012"
        ws.Range("F3").Formula = "=1234567890123456"
        ws.Range("F4").Value = 0.1 + 0.2
        ws.Range("F5").Formula = "=1E+20"
        ws.Range("F6").Formula = "=-1234.5678"
        self.assertEqual([ws.Range(f"F{i}").Text for i in range(1, 7)],
                         ["0.333333333", "1.23457E+11", "1.23457E+15", "0.3", "1E+20", "-1234.5678"])

    # ---- charts ----
    def test_common_charts(self):
        from yc import ui

        ws = self.ws
        self.chart_data(ws)
        self.keys("k:Escape|w:2|m:2,2|w:2")
        folder = self.shot_dir()
        for cmd, nser in (("InsertChartColumn", 2), ("InsertChartLine", 2), ("InsertChartBar", 2), ("InsertChartArea", 2),
                          ("InsertChartPie", 1), ("InsertChartScatter", 2), ("InsertChartCombo", 2)):
            with self.subTest(cmd):
                before = ws.ChartObjects().Count
                self.keys("g:A1:C7|w:2")
                self.assertTrue(ui.command(cmd))
                self.keys("w:8")
                self.assertEqual(ws.ChartObjects().Count, before + 1, "one chart inserted")
                ch = ws.ChartObjects(before + 1).Chart
                s1 = ch.SeriesCollection(1)
                self.assertEqual(ch.SeriesCollection().Count, nser)
                self.assertEqual(list(s1.Values), REV)
                self.assertEqual(s1.Name, "Revenue")
                self.assertIn(list(s1.XValues), [MONTHS, [1, 2, 3, 4, 5, 6]] if cmd == "InsertChartScatter" else [MONTHS])
                self.assertTrue(ch.HasTitle)
                self.assertEqual(ch.ChartTitle.Text, "Revenue" if nser == 1 else "Chart Title")  # Excel's default titles
                self.assertTrue(ch.HasLegend)
                ch.ChartTitle.Text = f"{cmd[11:]} 2024"
                if cmd != "InsertChartPie":
                    ax = ch.Axes(2)
                    ax.HasTitle = True
                    ax.AxisTitle.Text = "USD"
                    self.assertEqual(ax.AxisTitle.Text, "USD")
                s1.ApplyDataLabels()
                self.assertTrue(s1.HasDataLabels)
                ch.Legend.Position = -4107  # xlLegendPositionBottom
                ws.Range("B2").Value = 200  # a changed value follows into the chart
                self.assertEqual(list(ch.SeriesCollection(1).Values)[0], 200)
                ws.Range("B2").Value = 120
                png = os.path.join(folder, f"chart_{cmd[11:]}.png")
                self.assertTrue(ch.Export(png))
                self.assertGreater(color_count(png, ACCENT1), 30, "the first series is drawn")
                if nser == 2:
                    self.assertGreater(color_count(png, ACCENT2), 30, "the second series is drawn")
        # combo: the second series on the secondary axis
        ch = ws.ChartObjects(ws.ChartObjects().Count).Chart
        ch.SeriesCollection(2).AxisGroup = 2
        self.assertEqual(ch.SeriesCollection(2).AxisGroup, 2)
        self.assertTrue(ch.HasAxis(2, 2))
        # rows inserted or deleted inside the data follow into the series
        c1 = ws.ChartObjects(1).Chart
        f0 = c1.SeriesCollection(1).Formula
        self.assertIn("$B$7", f0)
        ws.Rows(4).Insert()
        self.assertIn("$B$8", c1.SeriesCollection(1).Formula)
        ws.Rows(4).Delete()
        self.assertEqual(c1.SeriesCollection(1).Formula, f0)
        ws.Rows(3).Delete()
        self.assertEqual(list(c1.SeriesCollection(1).Values), [120, 150, 142, 160, 175])
        ws.Rows(3).Insert()
        ws.Range("A3:C3").Value = [["Feb", 135, 85]]
        self.assertEqual(list(c1.SeriesCollection(1).Values), REV)
        # the type switched, a copy with the keys, Delete and Undo
        c1.ChartType = 4  # xlLine
        self.assertEqual(c1.ChartType, 4)
        c1.ChartType = 51
        nn = ws.ChartObjects().Count
        ws.ChartObjects(1).Select()
        self.keys("w:2|k:c-C|w:3|g:A30|w:2|k:c-V|w:6")
        self.assertEqual(ws.ChartObjects().Count, nn + 1, "Ctrl+C / Ctrl+V copies the chart")
        self.keys("k:Delete|w:4")
        self.assertEqual(ws.ChartObjects().Count, nn, "Delete removes the pasted chart")
        self.keys("k:c-Z|w:4")
        self.assertEqual(ws.ChartObjects().Count, nn + 1, "Undo brings it back")
        ws.ChartObjects(nn + 1).Delete()
        # Python's Excel names, and a trendline on the scatter
        sh = ws.Shapes.AddChart2(-1, 4, 400, 20, 360, 216)
        sh.Chart.SetSourceData(ws.Range("A1:B7"))
        self.assertEqual(sh.Chart.SeriesCollection().Count, 1)
        self.assertEqual(list(sh.Chart.SeriesCollection(1).Values), REV)
        png = os.path.join(folder, "chart_AddChart2.png")
        self.assertTrue(sh.Chart.Export(png))
        self.assertGreater(color_count(png, ACCENT1), 30)
        scatter = [ws.ChartObjects(i).Chart for i in range(1, ws.ChartObjects().Count + 1)
                   if ws.ChartObjects(i).Chart.ChartType in (-4169, 74, 75, 72, 73)]
        self.assertTrue(scatter)
        scatter[0].SeriesCollection(1).Trendlines().Add()
        self.assertEqual(scatter[0].SeriesCollection(1).Trendlines().Count, 1)

    # ---- a P&L sheet made with the keys ----
    def test_everyday_operations_on_a_sheet(self):
        from yc import excel, ui

        ws = self.ws
        self.keys("g:A1|w:2|t:Ticker|k:Tab|t:Qty|k:Tab|t:Entry|k:Tab|t:Last|k:Tab|t:P&L|k:Enter|w:2")
        self.keys("g:A2|t:AAPL|k:Tab|t:10|k:Tab|t:150|k:Tab|t:155.5|k:Tab|t:=(D2-C2)*B2|k:Enter|w:2")
        self.keys("g:A3|t:MSFT|k:Tab|t:5|k:Tab|t:300|k:Tab|t:295|k:Tab|t:=(D3-C3)*B3|k:Enter|w:2")
        self.keys("g:A4|t:GOOG|k:Tab|t:8|k:Tab|t:125|k:Tab|t:131.25|k:Tab|t:=(D4-C4)*B4|k:Enter|w:2")
        self.keys("g:A6|t:Total|k:Tab|t:=SUM(B2:B4)|k:Enter|w:2|g:E6|t:=SUM(E2:E4)|k:Enter|w:3")
        self.assertEqual(ws.Range("E1").Value, "P&L")
        self.assertEqual(ws.Range("E2:E4").Value, [[55], [-25], [50]])
        self.assertEqual(ws.Range("E6").Value, 80)
        # the fill handle: 1, 2 -> 3, 4, 5
        self.keys("g:G2|t:1|k:Enter|t:2|k:Enter|w:2|g:G2:G3|w:3")
        r = ui.cell_rect("G3")
        hx, hy = r[2] - 1, r[3] - 1
        self.keys(f"m:{hx},{hy}|w:4|d:{hx},{hy},{hx},{hy + 3 * (r[3] - r[1])}|w:10")
        self.assertEqual(ws.Range("G4:G6").Value, [[3], [4], [5]])
        # copy / paste adjusts relative references; Ctrl+D fills down
        self.keys("g:E2|w:2|k:c-C|w:3|g:F2|w:2|k:c-V|w:4|k:Escape|w:2")
        self.assertEqual(ws.Range("F2").Formula, "=(E2-D2)*C2")
        self.keys("g:F2|k:Delete|w:2|g:H2|t:=B2*2|k:Enter|w:2|g:H2:H4|w:2|k:c-D|w:4")
        self.assertEqual(ws.Range("H4").Formula, "=B4*2")
        self.assertEqual(ws.Range("H4").Value, 16)
        self.keys("g:H2:H6|k:Delete|w:2|g:G2:G6|k:Delete|w:2")
        # format shortcuts (Excel's codes: the currency one pads with _) )
        self.keys("g:C2:D4|w:2|k:c-s-4|w:4")
        self.assertEqual((ws.Range("C2").Text, ws.Range("D2").Text), ("$150.00 ", "$155.50 "))
        self.keys("g:E2|w:2|k:c-s-5|w:4")
        self.assertEqual(ws.Range("E2").Text, "5500%")
        self.keys("k:c-s-GraveAccent|w:4")
        self.assertEqual(ws.Range("E2").Text, "55")
        self.keys("g:A1:E1|w:2|k:c-B|w:3")
        self.assertTrue(ws.Range("A1").Font.Bold and ws.Range("E1").Font.Bold)
        self.keys("g:A6:E6|w:2|k:c-s-7|w:4")
        self.assertEqual((ws.Range("A6").Borders(7).LineStyle, ws.Range("E6").Borders(10).LineStyle), (1, 1))
        self.keys("g:D2|w:2")
        ui.command("DecimalIncrease")
        self.keys("w:3")
        self.assertEqual(ws.Range("D2").Text, "$155.500 ")
        ui.command("DecimalDecrease")
        self.keys("w:3")
        self.assertEqual(ws.Range("D2").Text, "$155.50 ")
        # a row inserted: formulas follow; Undo
        self.keys("g:A3|w:2")
        ui.command("InsertRows")
        self.keys("w:4")
        self.assertEqual(ws.Range("E4").Formula, "=(D4-C4)*B4")
        self.assertEqual(ws.Range("E7").Formula, "=SUM(E2:E5)")
        self.assertEqual(ws.Range("E7").Value, 80)
        self.keys("k:c-Z|w:4")
        self.assertEqual(ws.Range("E3").Formula, "=(D3-C3)*B3")
        self.assertEqual(ws.Range("E6").Formula, "=SUM(E2:E4)")
        # Sort Descending from one cell sorts the region; Undo
        self.keys("g:E2|w:2")
        ui.command("SortDescending")
        self.keys("w:5")
        self.assertEqual(ws.Range("A2:A4").Value, [["AAPL"], ["GOOG"], ["MSFT"]])
        self.assertEqual(ws.Range("E2").Formula, "=(D2-C2)*B2")
        self.assertEqual(ws.Range("E6").Value, 80)
        self.keys("k:c-Z|w:4")
        self.assertEqual(ws.Range("A2:A4").Value, [["AAPL"], ["MSFT"], ["GOOG"]])
        # the filter
        self.keys("g:A1|w:2|k:c-s-L|w:5")
        self.assertTrue(ws.AutoFilterMode)
        ws.Range("A1:E4").AutoFilter(1, "A*")
        self.keys("w:4")
        self.assertEqual([ws.Rows(i).Hidden for i in (2, 3, 4)], [False, True, True])
        self.assertEqual(ws.Evaluate("SUBTOTAL(109,E2:E4)"), 55)
        ws.ShowAllData()
        self.keys("k:c-s-L|w:4")
        self.assertFalse(ws.AutoFilterMode)
        self.assertFalse(any(ws.Rows(i).Hidden for i in (2, 3, 4)))
        # freeze panes, merge, AutoFit
        self.keys("g:B2|w:2")
        ui.command("FreezePanes")
        self.keys("w:4")
        self.assertTrue(excel.ActiveWindow.FreezePanes)
        ui.command("FreezePanes")
        self.keys("w:4")
        self.assertFalse(excel.ActiveWindow.FreezePanes)
        self.keys("g:A8:E8|w:2|t:Portfolio|k:Enter|w:2|g:A8:E8|w:2")
        ui.command("MergeCenter")
        self.keys("w:4")
        self.assertTrue(ws.Range("A8").MergeCells)
        self.assertEqual(ws.Range("A8").HorizontalAlignment, -4108)
        w0 = ws.Columns("A").ColumnWidth
        ws.Columns("A").AutoFit()
        self.assertNotEqual(ws.Columns("A").ColumnWidth, w0)
        # a conditional format, a name, cross-sheet formulas, Replace, validation
        fc = ws.Range("E2:E4").FormatConditions.Add(1, 6, "=0")  # xlCellValue, xlLess
        fc.Interior.Color = 0xC7CEFF
        self.assertEqual(ws.Range("E2:E4").FormatConditions.Count, 1)
        excel.ActiveWorkbook.Names.Add("PnLQty", f"={ws.Name}!$B$2:$B$4")
        ws.Range("G1").Formula = "=SUM(PnLQty)"
        self.assertEqual(ws.Range("G1").Value, 23)
        ws.Range("A2:A4").Replace("GOOG", "GOOGL")
        self.assertEqual(ws.Range("A4").Value, "GOOGL")
        ws.Range("A2:A4").Replace("GOOGL", "GOOG")
        ws.Range("I2").Validation.Add(3, 1, 1, "Buy,Sell,Hold")
        self.keys("g:I2|w:2|t:Hold|k:Enter|w:3")
        self.assertEqual(ws.Range("I2").Value, "Hold")
        n_err = len(ui.errors())
        self.keys("g:I2|w:2|t:Short|k:Enter|w:4|k:Escape|w:3|k:Escape|w:3")
        msgs = [e.get("text", "") for e in ui.errors()[n_err:]]  # a hidden instance answers the message box by itself
        self.assertTrue(any("isn't in the list" in m for m in msgs), msgs)
        self.assertEqual(ws.Range("I2").Value, "Hold")
        ui.clear_errors()
        # number formats people set
        for i, (v, fmt) in enumerate(((serial(2024, 1, 15), "yyyy-mm-dd"), (0.1234, "0.00%"), (1234567.891, "#,##0.00"),
                                      (-1234.5, "#,##0.00;[Red](#,##0.00)"), (1234.5, "$#,##0.00_);($#,##0.00)"), (0.5, "h:mm AM/PM"), (42, "@"))):
            ws.Range(f"K{i + 1}").Value = v
            ws.Range(f"K{i + 1}").NumberFormat = fmt
        self.assertEqual([ws.Range(f"K{i}").Text for i in range(1, 8)],
                         ["2024-01-15", "12.34%", "1,234,567.89", "(1,234.50)", "$1,234.50 ", "12:00 PM", "42"])
        excel.ActiveWindow.Zoom = 120
        self.assertEqual(excel.ActiveWindow.Zoom, 120)
        excel.ActiveWindow.Zoom = 100

    # ---- save and reopen ----
    def test_save_reopen_round_trip(self):
        from yc import excel, ui

        ws = self.ws
        self.data_table(ws)
        ws.Range("F1").Formula = "=SUM(B2:B6)"
        ws.Range("F2").Formula = "=SORT(C2:C6)"
        ws.Range("B2:B6").NumberFormat = "$#,##0.00"
        ws.Range("A1:D1").Font.Bold = True
        ws.Range("H1:I1").Merge()
        ws.Range("H1").Value = "Merged"
        ws.Range("B2:B6").FormatConditions.Add(1, 5, "=200").Interior.Color = 0xC7CEFF
        ws.Range("J2").Validation.Add(3, 1, 1, "Buy,Sell,Hold")
        excel.ActiveWorkbook.Names.Add("RtPrices", f"={ws.Name}!$B$2:$B$6")
        ws.Range("F10").Formula = "=SUM(RtPrices)"
        self.keys("g:A1:C6|w:2")
        self.assertTrue(ui.command("InsertChartColumn"))
        self.keys("w:8")
        ch = ws.ChartObjects(1).Chart
        ch.ChartTitle.Text = "Prices 2024"
        ch.SeriesCollection(1).ApplyDataLabels()
        name = ws.Name
        path = os.path.join(tempfile.mkdtemp(prefix="yc_rc_"), "round_trip.ycg")
        excel.ActiveWorkbook.SaveAs(path)
        self.assertGreater(os.path.getsize(path), 1000)
        other = yc.launch(path, hidden=True, exe=EXE, lang="en")
        try:
            yc.connect(target=other.pid)
            from yc import excel as ex2

            w2 = ex2.ActiveWorkbook.Worksheets(name)
            self.assertEqual(w2.Range("B3").Value, 300.5)
            self.assertEqual(w2.Range("B3").Text, "$300.50")
            self.assertEqual(w2.Range("D2").Text, "1/15/2024")
            self.assertEqual(w2.Range("F1").Formula, "=SUM(B2:B6)")
            self.assertEqual(w2.Range("F1").Value, 906.5)
            self.assertEqual(w2.Range("F2#").Value, [[5], [7], [8], [10], [12]])
            self.assertEqual(w2.Range("F10").Value, 906.5)
            self.assertTrue(w2.Range("A1").Font.Bold)
            self.assertTrue(w2.Range("H1").MergeCells)
            self.assertEqual(w2.Range("B2:B6").FormatConditions.Count, 1)
            self.assertEqual(w2.Range("J2").Validation.Formula1, "Buy,Sell,Hold")
            self.assertEqual(w2.ChartObjects().Count, 1)
            c2 = w2.ChartObjects(1).Chart
            self.assertEqual(c2.ChartTitle.Text, "Prices 2024")
            self.assertEqual(c2.SeriesCollection().Count, 2)
            self.assertEqual(list(c2.SeriesCollection(1).Values), PRICES)
            self.assertTrue(c2.SeriesCollection(1).HasDataLabels)
        finally:
            other.terminate()
            other.wait(10)
            yc.connect(target=self.proc.pid)
        # a copy as .xlsx is a real xlsx (a zip)
        xpath = os.path.splitext(path)[0] + ".xlsx"
        excel.ActiveWorkbook.SaveCopyAs(xpath)
        with open(xpath, "rb") as fh:
            self.assertEqual(fh.read(2), b"PK")


if __name__ == "__main__":
    unittest.main()
