# SPDX-License-Identifier: Apache-2.0
"""Conditional formatting (FormatConditions), data validation (Validation), AutoFilter; Excel compatible.

Difference from Excel: relative references in FormatConditions and Validation are relative to the upper-left cell
of the applied range (Excel uses the active cell, which often confuses people). E.g. "=B2>100" applied to B2:B100 is
B3>100 for B3.
"""
from __future__ import annotations

from typing import Any

from .errors import ApplicationDefinedError, SubscriptOutOfRangeError


def _rq(method: str, **params) -> Any:
    from ._conn import current

    return current().request(method, params)


def _areas(rng) -> list:
    return [list(a) for a in rng._areas]


def _f(v) -> str:
    """Formula1 / Formula2: a number, a string, "=A1"."""
    if v is None:
        return ""
    if isinstance(v, bool):
        return "TRUE" if v else "FALSE"
    if isinstance(v, (int, float)):
        return repr(v)
    s = str(v)
    if hasattr(v, "Address"):
        return "=" + v.Address(External=True)
    return s


# the formulas Excel makes for the rules that have no Formula1 of their own ({x}: the rule's first cell, relative; {t}:
# its text as a formula string), as FormatCondition.Formula1 reads them
_TEXT_FORMULAS = {0: "=NOT(ISERROR(SEARCH({t},{x})))", 1: "=ISERROR(SEARCH({t},{x}))", 2: "=LEFT({x},LEN({t}))={t}", 3: "=RIGHT({x},LEN({t}))={t}"}
_DATE_FORMULAS = {
    0: "=FLOOR({x},1)=TODAY()",
    1: "=FLOOR({x},1)=TODAY()-1",
    2: "=AND(TODAY()-FLOOR({x},1)<=6,FLOOR({x},1)<=TODAY())",
    3: "=AND(TODAY()-ROUNDDOWN({x},0)<=WEEKDAY(TODAY())-1,ROUNDDOWN({x},0)-TODAY()<=7-WEEKDAY(TODAY()))",
    4: "=AND(TODAY()-ROUNDDOWN({x},0)>=(WEEKDAY(TODAY())),TODAY()-ROUNDDOWN({x},0)<(WEEKDAY(TODAY())+7))",
    5: "=AND(MONTH({x})=MONTH(EDATE(TODAY(),0-1)),YEAR({x})=YEAR(EDATE(TODAY(),0-1)))",
    6: "=FLOOR({x},1)=TODAY()+1",
    7: "=AND(ROUNDDOWN({x},0)-TODAY()>(7-WEEKDAY(TODAY())),ROUNDDOWN({x},0)-TODAY()<(15-WEEKDAY(TODAY())))",
    8: "=AND(MONTH({x})=MONTH(EDATE(TODAY(),0+1)),YEAR({x})=YEAR(EDATE(TODAY(),0+1)))",
    9: "=AND(MONTH({x})=MONTH(TODAY()),YEAR({x})=YEAR(TODAY()))",
}
_KIND_FORMULAS = {10: "=LEN(TRIM({x}))=0", 13: "=LEN(TRIM({x}))>0", 16: "=ISERROR({x})", 17: "=NOT(ISERROR({x}))"}


def _generated_formula(r: dict) -> str:
    """Formula1 of a rule kind that has none of its own (an empty string for the others)."""
    from ._r1c1 import _col_name

    ranges = r.get("ranges") or []
    if not ranges:
        return ""
    x = _col_name(int(ranges[0][1])) + str(int(ranges[0][0]) + 1)
    t = int(r.get("type", 0))
    if t == 9:
        f = _TEXT_FORMULAS.get(int(r.get("text_op", 0)), _TEXT_FORMULAS[0])
        quoted = '"' + str(r.get("text", "")).replace('"', '""') + '"'
        return f.replace("{x}", x).replace("{t}", quoted)
    if t == 11:
        return _DATE_FORMULAS.get(int(r.get("time_period", 0)), _DATE_FORMULAS[0]).replace("{x}", x)
    if t in _KIND_FORMULAS:
        return _KIND_FORMULAS[t].replace("{x}", x)
    return ""


# ---------------------------------------------------------------------------
# conditional formatting
# ---------------------------------------------------------------------------
class _CfInterior:
    def __init__(self, fc: "FormatCondition"):
        self._fc = fc

    @property
    def Color(self):  # noqa: N802
        return self._fc._get().get("fill")

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._fc._set(fill=None if v is None else int(v))

    @property
    def ColorIndex(self):  # noqa: N802
        return -4142 if self._fc._get().get("fill") is None else 1

    @ColorIndex.setter
    def ColorIndex(self, v):  # noqa: N802
        if int(v) == -4142:
            self._fc._set(fill=None)

    # Excel's XlPattern <-> model::FillPattern (Format... > Fill > Pattern Style)
    _PATTERNS = {1: 0, -4126: 1, -4125: 2, -4124: 3, 17: 4, 18: 5, -4128: 6, -4166: 7, -4121: 8, -4162: 9, 9: 10, 10: 11, 11: 12, 12: 13, 13: 14,
                 14: 15, 15: 16, 16: 17}

    @property
    def Pattern(self):  # noqa: N802
        p = self._fc._get().get("pattern")
        if p is None:
            return 1 if self._fc._get().get("fill") is not None else -4142
        return {v: k for k, v in _CfInterior._PATTERNS.items()}.get(p, 1)

    @Pattern.setter
    def Pattern(self, v):  # noqa: N802
        v = int(v)
        self._fc._set(pattern=None if v in (-4142, 1) else _CfInterior._PATTERNS.get(v, 0))

    @property
    def PatternColor(self):  # noqa: N802
        return self._fc._get().get("pattern_color", 0)

    @PatternColor.setter
    def PatternColor(self, v):  # noqa: N802
        self._fc._set(pattern_color=int(v))


class _CfBorder:
    """FormatCondition.Borders(xlTop / xlBottom / xlLeft / xlRight): one line of the rule's format."""

    _SIDE = {-4131: 0, 7: 0, -4160: 1, 8: 1, -4152: 2, 10: 2, -4107: 3, 9: 3}  # xlLeft, xlTop, xlRight, xlBottom (and xlEdgeLeft...)

    def __init__(self, fc, index):
        self._fc, self._k = fc, _CfBorder._SIDE.get(int(index), 0)

    def _sides(self):
        return [list(s) for s in self._fc._get().get("sides", [[0, 0]] * 4)]

    @property
    def LineStyle(self):  # noqa: N802
        return 1 if self._sides()[self._k][0] else -4142

    @LineStyle.setter
    def LineStyle(self, v):  # noqa: N802
        s = self._sides()
        s[self._k][0] = 0 if int(v) == -4142 else (s[self._k][0] or 1)
        self._fc._set(sides=s)

    @property
    def Weight(self):  # noqa: N802
        st = self._sides()[self._k][0]
        return {2: -4138, 5: 4, 7: 1}.get(st, 2)

    @Weight.setter
    def Weight(self, v):  # noqa: N802
        s = self._sides()
        s[self._k][0] = {1: 7, 2: 1, -4138: 2, 4: 5}.get(int(v), 1)
        self._fc._set(sides=s)

    @property
    def Color(self):  # noqa: N802
        return self._sides()[self._k][1]

    @Color.setter
    def Color(self, v):  # noqa: N802
        s = self._sides()
        s[self._k][1] = int(v)
        s[self._k][0] = s[self._k][0] or 1
        self._fc._set(sides=s)


class _CfBorders:
    """FormatCondition.Borders: the outline the rule draws around a cell (Excel's Red Border); Borders(xlTop) and
    the others are the same outline."""

    def __init__(self, fc: "FormatCondition"):
        self._fc = fc

    def __call__(self, Index=None):  # noqa: N803
        return self if Index is None else _CfBorder(self._fc, Index)

    def Item(self, Index):  # noqa: N802,N803
        return _CfBorder(self._fc, Index)

    @property
    def Color(self):  # noqa: N802
        return self._fc._get().get("border")

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._fc._set(border=None if v is None else int(v))

    @property
    def LineStyle(self):  # noqa: N802
        return -4142 if self._fc._get().get("border") is None else 1

    @LineStyle.setter
    def LineStyle(self, v):  # noqa: N802
        if int(v) == -4142:
            self._fc._set(border=None)
        elif self._fc._get().get("border") is None:
            self._fc._set(border=0)


class _CfFont:
    def __init__(self, fc: "FormatCondition"):
        self._fc = fc

    def _prop(key):  # noqa: N805
        def g(self):
            return self._fc._get().get(key)

        def s(self, v):
            self._fc._set(**{key: v if key == "font_color" or v is None else bool(v)})

        return property(g, s)

    Color = _prop("font_color")
    Bold = _prop("bold")
    Italic = _prop("italic")
    Strikethrough = _prop("strike")
    del _prop

    @property
    def Underline(self):  # noqa: N802
        """xlUnderlineStyleNone / Single / Double / SingleAccounting / DoubleAccounting; None when the rule leaves it."""
        u = self._fc._get().get("underline")
        return None if u is None else {0: -4142, 1: 2, 2: -4119, 3: 4, 4: 5}.get(u, -4142)

    @Underline.setter
    def Underline(self, v):  # noqa: N802
        self._fc._set(underline=None if v is None else {-4142: 0, False: 0, 2: 1, True: 1, -4119: 2, 4: 3, 5: 4}.get(v, 1))


class _FormatColor:
    def __init__(self, fc: "FormatCondition", index: int):
        self._fc, self._i = fc, index

    @property
    def Color(self):  # noqa: N802
        return self._fc._get()["colors"][self._i]

    @Color.setter
    def Color(self, v):  # noqa: N802
        cols = list(self._fc._get()["colors"])
        cols[self._i] = int(v)
        self._fc._set(colors=cols)


class _ScaleCriterion:
    """ColorScaleCriteria(i): its color, and where it sits (Type: xlConditionValueLowestValue, ...Number, ...Percent,
    ...Formula, ...Percentile, ...HighestValue; Value)."""

    _DEFAULT = {0: 1, 1: 5, 2: 2}  # lowest, 50th percentile, highest

    def __init__(self, fc, index):
        self._fc, self._i = fc, index
        self.FormatColor = _FormatColor(fc, index)

    def _points(self):
        return [list(p) for p in self._fc._get()["scale_points"]]

    @property
    def Type(self):  # noqa: N802
        t = self._points()[self._i][0]
        return self._DEFAULT[self._i] if t < 0 else t

    @Type.setter
    def Type(self, v):  # noqa: N802
        pts = self._points()
        if pts[self._i][0] < 0 and self._i == 1:
            pts[self._i][1] = 50.0
        pts[self._i][0] = int(v)
        self._fc._set(scale_points=pts)

    @property
    def Value(self):  # noqa: N802
        p = self._points()[self._i]
        if p[0] == 4:
            return "=" + p[2]
        return 50.0 if p[0] < 0 and self._i == 1 else p[1]

    @Value.setter
    def Value(self, v):  # noqa: N802
        pts = self._points()
        if isinstance(v, str) and v.startswith("="):
            pts[self._i][2] = v[1:]
            pts[self._i][0] = 4
        else:
            pts[self._i][1] = float(v)
            if pts[self._i][0] < 0:
                pts[self._i][0] = 0
        self._fc._set(scale_points=pts)


class _BarPoint:
    """Databar.MinPoint / MaxPoint: the value the shortest / longest bar stands for."""

    def __init__(self, fc, i):
        self._fc, self._i = fc, i

    def _b(self):
        return [list(x) for x in self._fc._get()["bar_bounds"]]

    @property
    def Type(self):  # noqa: N802
        t = self._b()[self._i][0]
        return (7 if self._i == 0 else 6) if t < 0 else t  # xlConditionValueAutomaticMin / Max

    @property
    def Value(self):  # noqa: N802
        b = self._b()[self._i]
        return ("=" + b[2]) if b[0] == 4 else b[1]

    def Modify(self, newtype, newvalue=None):  # noqa: N802
        b = self._b()
        t = int(newtype)
        if t in (6, 7):
            t = -1
        f = ""
        val = 0.0
        if isinstance(newvalue, str):
            f = newvalue[1:] if newvalue.startswith("=") else newvalue
            if t != 4:
                val = float(f)
                f = ""
        elif newvalue is not None:
            val = float(newvalue)
        b[self._i] = [t, val, f]
        self._fc._set(bar_bounds=b)


class _BarBorder:
    def __init__(self, fc):
        self._fc = fc

    @property
    def Type(self):  # noqa: N802
        return 1 if self._fc._get().get("bar_border") is not None else 0  # xlDataBarBorderSolid / None

    @Type.setter
    def Type(self, v):  # noqa: N802
        if int(v) == 0:
            self._fc._set(bar_border=None)
        elif self._fc._get().get("bar_border") is None:
            self._fc._set(bar_border=self._fc._get()["bar_color"])

    @property
    def Color(self):  # noqa: N802
        return _BarColor(self._fc, "bar_border")


class _BarColor:
    def __init__(self, fc, key="bar_color"):
        self._key = key
        self._fc = fc

    @property
    def Color(self):  # noqa: N802
        return self._fc._get()[self._key]

    @Color.setter
    def Color(self, v):  # noqa: N802
        if v is None and self._key == "bar_color":  # the bar always has a color: nothing to change
            return
        self._fc._set(**{self._key: None if v is None else int(v)})  # the border: None is no border, as it reads


class FormatCondition:
    def __init__(self, sheet_id: int, rule_id: int):
        self._sheet, self._id = sheet_id, rule_id

    def __repr__(self):
        return f"<FormatCondition {self._id}>"

    def _get(self) -> dict:
        for r in _rq("cf.list", sheet=self._sheet):
            if r["id"] == self._id:
                return r
        raise ApplicationDefinedError("The conditional formatting rule no longer exists")

    def _set(self, **props):
        _rq("cf.set", sheet=self._sheet, id=self._id, props=props)

    @property
    def Type(self):  # noqa: N802
        return self._get()["type"]

    @property
    def Operator(self):  # noqa: N802
        return self._get()["op"]

    @property
    def Formula1(self):  # noqa: N802
        """The rule's formula; for Text that Contains, A Date Occurring, Blanks, No Blanks, Errors and No Errors the
        formula Excel makes for the rule (=NOT(ISERROR(SEARCH("ab",C1)))), relative to its first cell."""
        r = self._get()
        f = r["formula1"]
        if not f:
            return _generated_formula(r)
        return f if f.startswith("=") else "=" + f

    @Formula1.setter
    def Formula1(self, v):  # noqa: N802
        r = self._get()
        gen = "" if r["formula1"] else _generated_formula(r)
        if gen and _f(v) == gen:
            return  # the formula it reads, written back: nothing changes
        self._set(formula1=_f(v))

    @property
    def Formula2(self):  # noqa: N802
        f = self._get()["formula2"]
        return f if not f or f.startswith("=") else "=" + f

    @Formula2.setter
    def Formula2(self, v):  # noqa: N802
        self._set(formula2=_f(v))

    @property
    def Interior(self):  # noqa: N802
        return _CfInterior(self)

    @property
    def Font(self):  # noqa: N802
        return _CfFont(self)

    @property
    def Borders(self):  # noqa: N802
        return _CfBorders(self)

    @property
    def StopIfTrue(self):  # noqa: N802
        return self._get()["stop_if_true"]

    @StopIfTrue.setter
    def StopIfTrue(self, v):  # noqa: N802
        self._set(stop_if_true=bool(v))

    @property
    def Priority(self):  # noqa: N802
        return self._get()["priority"]

    @Priority.setter
    def Priority(self, v):  # noqa: N802
        _rq("cf.set", sheet=self._sheet, id=self._id, priority=int(v))

    def SetFirstPriority(self):  # noqa: N802
        self.Priority = 1

    def SetLastPriority(self):  # noqa: N802
        self.Priority = 1_000_000

    @property
    def AppliesTo(self):  # noqa: N802
        from . import excel

        return excel.Range._from(self._sheet, [tuple(r) for r in self._get()["ranges"]])

    def ModifyAppliesToRange(self, Range):  # noqa: N802,N803
        self._set(ranges=_areas(Range))

    def Delete(self):  # noqa: N802
        _rq("cf.delete", sheet=self._sheet, id=self._id)

    # top N, average, duplicates
    @property
    def Rank(self):  # noqa: N802
        return self._get()["rank"]

    @Rank.setter
    def Rank(self, v):  # noqa: N802
        self._set(rank=int(v))

    @property
    def Percent(self):  # noqa: N802
        return self._get()["percent"]

    @Percent.setter
    def Percent(self, v):  # noqa: N802
        self._set(percent=bool(v))

    @property
    def TopBottom(self):  # noqa: N802
        return 0 if self._get()["bottom"] else 1

    @TopBottom.setter
    def TopBottom(self, v):  # noqa: N802
        self._set(bottom=int(v) == 0)

    @property
    def AboveBelow(self):  # noqa: N802
        """xlAboveAverage 0, xlBelowAverage 1, xlEqualAboveAverage 2, xlEqualBelowAverage 3, xlAboveStdDev 4, xlBelowStdDev 5."""
        r = self._get()
        if r.get("std_dev"):
            return 5 if r["below"] else 4
        if r.get("equal_average"):
            return 3 if r["below"] else 2
        return 1 if r["below"] else 0

    @AboveBelow.setter
    def AboveBelow(self, v):  # noqa: N802
        v = int(v)
        std = self._get().get("std_dev") or 1
        self._set(below=v in (1, 3, 5), equal_average=v in (2, 3), std_dev=std if v in (4, 5) else 0)

    @property
    def NumStdDev(self):  # noqa: N802
        return self._get().get("std_dev", 0)

    @NumStdDev.setter
    def NumStdDev(self, v):  # noqa: N802
        self._set(std_dev=int(v))

    @property
    def TextOperator(self):  # noqa: N802
        """xlContains 0, xlDoesNotContain 1, xlBeginsWith 2, xlEndsWith 3."""
        return self._get().get("text_op", 0)

    @TextOperator.setter
    def TextOperator(self, v):  # noqa: N802
        self._set(text_op=int(v))

    @property
    def Text(self):  # noqa: N802
        return self._get()["text"]

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._set(text=str(v))

    @property
    def DateOperator(self):  # noqa: N802
        """Excel's XlTimePeriods: xlToday 0, xlYesterday 1, xlLast7Days 2, xlThisWeek 3, xlLastWeek 4, xlLastMonth 5,
        xlTomorrow 6, xlNextWeek 7, xlNextMonth 8, xlThisMonth 9."""
        return self._get().get("time_period", 0)

    @DateOperator.setter
    def DateOperator(self, v):  # noqa: N802
        self._set(time_period=int(v))

    @property
    def DupeUnique(self):  # noqa: N802
        return 1 if self._get()["duplicate"] else 0

    @DupeUnique.setter
    def DupeUnique(self, v):  # noqa: N802
        self._set(duplicate=int(v) == 1)

    # color scales, data bars
    def ColorScaleCriteria(self, Index):  # noqa: N802,N803
        n = self._get()["points"]
        i = int(Index) - 1
        if not 0 <= i < n:
            raise SubscriptOutOfRangeError("ColorScaleCriteria index out of range")
        return _ScaleCriterion(self, 2 if (n == 2 and i == 1) else i)

    @property
    def BarColor(self):  # noqa: N802
        return _BarColor(self)

    # data bars (Excel's Databar)
    @property
    def MinPoint(self):  # noqa: N802
        return _BarPoint(self, 0)

    @property
    def MaxPoint(self):  # noqa: N802
        return _BarPoint(self, 1)

    @property
    def BarFillType(self):  # noqa: N802
        """xlDataBarFillGradient (1) or xlDataBarFillSolid (0)."""
        return 1 if self._get()["bar_gradient"] else 0

    @BarFillType.setter
    def BarFillType(self, v):  # noqa: N802
        self._set(bar_gradient=int(v) == 1)

    @property
    def BarBorder(self):  # noqa: N802
        return _BarBorder(self)

    @property
    def Direction(self):  # noqa: N802
        """xlContext, xlLTR or xlRTL."""
        return {0: -5002, 1: -5003, 2: -5004}[self._get()["bar_direction"]]

    @Direction.setter
    def Direction(self, v):  # noqa: N802
        self._set(bar_direction={-5002: 0, -5003: 1, -5004: 2}.get(int(v), 0))

    @property
    def ShowValue(self):  # noqa: N802
        """False = Show Bar Only (data bars)."""
        return self._get()["bar_show_value"]

    @ShowValue.setter
    def ShowValue(self, v):  # noqa: N802
        self._set(bar_show_value=bool(v))

    @property
    def NumberFormat(self):  # noqa: N802
        """The number format the rule shows its cells in (Format... > Number); None when it leaves the cell's."""
        return self._get().get("numfmt")

    @NumberFormat.setter
    def NumberFormat(self, v):  # noqa: N802
        self._set(numfmt=None if v is None else str(v))

    # icon sets
    @property
    def IconSet(self):  # noqa: N802
        """The icon set (an XlIconSet value such as xl3Arrows); also accepts Workbook.IconSets(n)."""
        return self._get()["icon_style"]

    @IconSet.setter
    def IconSet(self, v):  # noqa: N802
        self._set(icon_style=int(getattr(v, "ID", v)))

    @property
    def ReverseOrder(self):  # noqa: N802
        return self._get()["icon_reverse"]

    @ReverseOrder.setter
    def ReverseOrder(self, v):  # noqa: N802
        self._set(icon_reverse=bool(v))

    @property
    def ShowIconOnly(self):  # noqa: N802
        return self._get()["icon_only"]

    @ShowIconOnly.setter
    def ShowIconOnly(self, v):  # noqa: N802
        self._set(icon_only=bool(v))

    def IconCriteria(self, Index):  # noqa: N802,N803
        """IconCriteria(1) is the lowest icon (no threshold); 2..n set where each next icon starts."""
        n = len(self._get()["icon_thresholds"]) + 1
        i = int(Index)
        if not 1 <= i <= n:
            raise SubscriptOutOfRangeError("IconCriteria index out of range")
        return _IconCriterion(self, i - 2)


# icon thresholds: engine type <-> XlConditionValueTypes
_ICON_TYPE_TO_XL = {0: 3, 1: 0, 2: 5, 3: 4}  # percent, number, percentile, formula
_XL_TO_ICON_TYPE = {3: 0, 0: 1, 5: 2, 4: 3, 7: 2}


class _IconCriterion:
    def __init__(self, fc, i):
        self._fc, self._i = fc, i

    def _all(self):
        return [list(t) for t in self._fc._get()["icon_thresholds"]]

    def _put(self, **kw):
        if self._i < 0:
            return  # the lowest icon has no threshold
        th = self._all()
        t = th[self._i]
        if "type" in kw:
            t[0] = kw["type"]
        if "value" in kw:
            v = kw["value"]
            if isinstance(v, str) and v.startswith("="):
                t[0], t[2] = 3, v[1:]
            else:
                t[1] = float(v)
        if "gte" in kw:
            t[3] = kw["gte"]
        self._fc._set(icon_thresholds=th)

    @property
    def Type(self):  # noqa: N802
        return 0 if self._i < 0 else _ICON_TYPE_TO_XL.get(self._all()[self._i][0], 3)

    @Type.setter
    def Type(self, v):  # noqa: N802
        self._put(type=_XL_TO_ICON_TYPE.get(int(v), 0))

    @property
    def Value(self):  # noqa: N802
        if self._i < 0:
            return None
        t = self._all()[self._i]
        return "=" + t[2] if t[0] == 3 else t[1]

    @Value.setter
    def Value(self, v):  # noqa: N802
        self._put(value=v)

    @property
    def Operator(self):  # noqa: N802
        return 7 if self._i < 0 or self._all()[self._i][3] else 5  # xlGreaterEqual / xlGreater

    @Operator.setter
    def Operator(self, v):  # noqa: N802
        self._put(gte=int(v) != 5)


class FormatConditions:
    def __init__(self, rng):
        self._rng = rng
        self._sheet = rng._sheet

    def _rules(self):
        return _rq("cf.list", sheet=self._sheet, rect=list(self._rng._areas[0]))

    def _add(self, **rule) -> FormatCondition:
        rule.setdefault("ranges", _areas(self._rng))
        # last: Excel's object model appends a rule after the sheet's others (Priority = their count); the UI's go on top
        r = _rq("cf.add", sheet=self._sheet, rule=rule, last=True)
        return FormatCondition(self._sheet, r["id"])

    def Add(self, Type, Operator=None, Formula1=None, Formula2=None, String=None, TextOperator=None, DateOperator=None,  # noqa: N802,N803
            ScopeType=None):
        t = int(Type)
        rule = {"type": t}
        if Operator is not None:
            rule["op"] = int(Operator)
        elif t == 1:
            rule["op"] = 1  # xlBetween
        if Formula1 is not None:
            rule["formula1"] = _f(Formula1)
        if Formula2 is not None:
            rule["formula2"] = _f(Formula2)
        if t == 9:
            rule["text"] = str(String or "")
            if TextOperator is not None:
                rule["text_op"] = int(TextOperator)
        if t == 11:  # xlTimePeriod: A Date Occurring
            rule["time_period"] = int(DateOperator or 0)
        if t in (1, 2) and Formula1 is None:
            raise ApplicationDefinedError("FormatConditions.Add needs Formula1")
        return self._add(**rule)

    def AddColorScale(self, ColorScaleType=3):  # noqa: N802,N803
        n = int(ColorScaleType)
        cols = [0x6B69F8, 0x84EBFF, 0x7BBE63] if n == 3 else [0x6B69F8, 0x7BBE63, 0x7BBE63]
        return self._add(type=3, points=n, colors=cols)

    def AddDatabar(self):  # noqa: N802
        return self._add(type=4)

    def AddIconSetCondition(self):  # noqa: N802
        """An icon set (3 traffic lights by default); change IconSet, IconCriteria, ReverseOrder, ShowIconOnly."""
        return self._add(type=6, icon_style=4)

    def AddTop10(self):  # noqa: N802
        return self._add(type=5, rank=10, bottom=False)

    def AddAboveAverage(self):  # noqa: N802
        return self._add(type=12, below=False)

    def AddUniqueValues(self):  # noqa: N802
        return self._add(type=8, duplicate=True)

    @property
    def Count(self):  # noqa: N802
        return len(self._rules())

    def Item(self, Index):  # noqa: N802,N803
        rs = self._rules()
        i = int(Index)
        if not 1 <= i <= len(rs):
            raise SubscriptOutOfRangeError("FormatConditions index out of range")
        return FormatCondition(self._sheet, rs[i - 1]["id"])

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([FormatCondition(self._sheet, r["id"]) for r in self._rules()])

    def __len__(self):
        return self.Count

    def Delete(self):  # noqa: N802
        for a in self._rng._areas:
            _rq("cf.clear", sheet=self._sheet, rect=list(a))


# ---------------------------------------------------------------------------
# data validation
# ---------------------------------------------------------------------------
class Validation:
    def __init__(self, rng):
        self._rng = rng
        self._sheet = rng._sheet

    def _at(self, row, col):
        return _rq("dv.get", sheet=self._sheet, row=row, col=col)

    def _get(self) -> dict:
        """The rule of the range: every cell must have the same one, or Excel's properties raise 1004."""
        areas = [tuple(a) for a in self._rng._areas]
        a = areas[0]
        r = self._at(a[0], a[1])
        if r is None:
            raise ApplicationDefinedError("This range has no data validation (use Validation.Add first)")
        if len(areas) == 1 and a[0] == a[2] and a[1] == a[3]:
            return r
        look = {k: v for k, v in r.items() if k != "ranges"}
        todo = _subtract(areas, r.get("ranges") or [])
        for _ in range(4096):  # each step takes at least the cell it asks about
            if not todo:
                return r
            b = todo[0]
            o = self._at(b[0], b[1])
            if o is None or {k: v for k, v in o.items() if k != "ranges"} != look:
                raise ApplicationDefinedError("The cells of the range have different data validation (or some have none)")
            todo = _subtract(todo, o.get("ranges") or [(b[0], b[1], b[0], b[1])])
        return r

    def _has_any(self) -> bool:
        """Whether any cell of the range has data validation."""
        for a in self._rng._areas:
            if a[0] == a[2] and a[1] == a[3]:
                if self._at(a[0], a[1]) is not None:
                    return True
            elif _rq("range.special", sheet=self._sheet, rect=list(a), kind=-4174, values=1 | 2 | 4 | 16):  # xlCellTypeAllValidation
                return True
        return False

    def _modify(self, **rule):
        for a in self._rng._areas:
            _rq("dv.modify", sheet=self._sheet, rect=list(a), rule=rule)

    def Add(self, Type, AlertStyle=1, Operator=1, Formula1=None, Formula2=None):  # noqa: N802,N803
        """Like Excel, raises 1004 when a cell of the range already has data validation (Delete it first) or the rule
        is incomplete (a list without Formula1, Between without Formula2, a number that isn't one)."""
        rule = {"type": int(Type), "alert": int(1 if AlertStyle is None else AlertStyle), "op": int(1 if Operator is None else Operator),
                "formula1": _f(Formula1), "formula2": _f(Formula2)}
        _check_dv(rule)
        self._check_formulas(rule)
        if self._has_any():
            raise ApplicationDefinedError("The range already has data validation (use Validation.Delete or Modify)")
        for a in self._rng._areas:
            _rq("dv.set", sheet=self._sheet, rect=list(a), rule=rule)

    def Modify(self, Type=None, AlertStyle=None, Operator=None, Formula1=None, Formula2=None):  # noqa: N802,N803
        rule = {}
        for k, v in (("type", Type), ("alert", AlertStyle), ("op", Operator)):
            if v is not None:
                rule[k] = int(v)
        if Formula1 is not None:
            rule["formula1"] = _f(Formula1)
            rule["formula2"] = _f(Formula2)  # as Excel: a new Formula1 without a Formula2 leaves none
        elif Formula2 is not None:
            rule["formula2"] = _f(Formula2)
        if rule.keys() & {"type", "op", "formula1", "formula2"}:
            a = self._rng._areas[0]
            cur = self._at(a[0], a[1]) or {}
            merged = {"type": cur.get("type", 0), "op": cur.get("op", 1), "formula1": cur.get("formula1", ""), "formula2": cur.get("formula2", "")}
            merged.update({k: v for k, v in rule.items() if k in merged})
            _check_dv(merged)
            self._check_formulas(merged)
        self._modify(**rule)

    def _check_formulas(self, rule: dict):
        # a formula that gives an error at the range's first cell: Excel 16 refuses it (1004, measured 2026-11-28),
        # where its dialog asks "The formula currently evaluates to an error"
        t, op = int(rule.get("type", 0)), int(rule.get("op", 1))
        if t == 0:
            return
        fs = [rule.get("formula1") or ""]
        if t in (1, 2, 4, 5, 6) and op in (1, 2):
            fs.append(rule.get("formula2") or "")
        a = self._rng._areas[0]
        for f in fs:
            if f.startswith("=") and _rq("dv.formula_fails", sheet=self._sheet, row=a[0], col=a[1], formula=f, list=t == 3):
                raise ApplicationDefinedError("The formula currently evaluates to an error: " + f)

    def Delete(self):  # noqa: N802
        for a in self._rng._areas:
            _rq("dv.clear", sheet=self._sheet, rect=list(a))

    def _prop(key, conv=lambda v: v):  # noqa: N805
        def g(self):
            return self._get()[key]

        def s(self, v):
            self._modify(**{key: conv(v)})

        return property(g, s)

    Type = _prop("type", int)
    Operator = _prop("op", int)
    AlertStyle = _prop("alert", int)
    IgnoreBlank = _prop("ignore_blank", bool)
    InCellDropdown = _prop("in_cell_dropdown", bool)
    ShowInput = _prop("show_input", bool)
    ShowError = _prop("show_error", bool)
    InputTitle = _prop("input_title", str)
    InputMessage = _prop("input_message", str)
    ErrorTitle = _prop("error_title", str)
    ErrorMessage = _prop("error_message", str)
    del _prop

    @property
    def Formula1(self):  # noqa: N802
        return self._get()["formula1"]

    @property
    def Formula2(self):  # noqa: N802
        return self._get()["formula2"]

    @property
    def Value(self):  # noqa: N802
        """Whether the value the range's first cell holds passes validation (the value a formula gives, not its text:
        ="b" is in the list a,b,c; the text '1 is in 1,2,3)."""
        a = self._rng._areas[0]
        cell = self._rng.Cells(1, 1)
        v = cell.Value2
        if v is None:
            text = ""
        elif isinstance(v, bool):
            text = "=TRUE" if v else "=FALSE"
        elif isinstance(v, (int, float)):
            text = "=" + repr(float(v))  # the stored number exactly (=0.1+0.2 is 0.30000000000000004)
        elif isinstance(v, str):
            text = "'" + v  # the text as it is, never read as a number
        else:
            text = cell.Formula  # an error: the cell's own formula or constant gives it again
            text = "" if text is None else str(text)
        r = _rq("dv.check", sheet=self._sheet, row=a[0], col=a[1], text=text)
        return r["valid"]


def _subtract(rects, cuts) -> list:
    """The parts of rects (r0, c0, r1, c1) outside every cut."""
    out = [tuple(int(x) for x in r) for r in rects]
    for c in cuts:
        c0r, c0c, c1r, c1c = (int(x) for x in c)
        nxt = []
        for r0, q0, r1, q1 in out:
            if r1 < c0r or r0 > c1r or q1 < c0c or q0 > c1c:
                nxt.append((r0, q0, r1, q1))
                continue
            if r0 < c0r:
                nxt.append((r0, q0, c0r - 1, q1))
            if r1 > c1r:
                nxt.append((c1r + 1, q0, r1, q1))
            mr0, mr1 = max(r0, c0r), min(r1, c1r)
            if q0 < c0c:
                nxt.append((mr0, q0, mr1, c0c - 1))
            if q1 > c1c:
                nxt.append((mr0, c1c + 1, mr1, q1))
        out = nxt
    return out


def _is_number_text(s: str) -> bool:
    """A Formula1 / Formula2 Excel takes for a number, date or time rule: a formula, or a number, date or time."""
    s = s.strip()
    if s.startswith("="):
        return len(s) > 1
    try:
        float(s.replace(",", ""))
        return True
    except ValueError:
        return any(ch.isdigit() for ch in s)  # a date or time (2026-01-01, 9:30); "abc" is refused, as in Excel


def _check_dv(rule: dict):
    """Refuses with 1004 what Excel's Validation.Add / Modify refuse."""
    t, op = int(rule.get("type", 0)), int(rule.get("op", 1))
    f1, f2 = rule.get("formula1", "") or "", rule.get("formula2", "") or ""
    if t == 0:  # xlValidateInputOnly
        return
    if not f1:
        raise ApplicationDefinedError("This kind of data validation needs Formula1")
    if t in (1, 2, 4, 5, 6):  # whole number, decimal, date, time, text length
        two = op in (1, 2)  # xlBetween, xlNotBetween
        if two and not f2:
            raise ApplicationDefinedError("Between and Not Between need Formula2")
        if not _is_number_text(f1) or (two and not _is_number_text(f2)):
            raise ApplicationDefinedError("Formula1 and Formula2 must be numbers or formulas")
        if two:
            try:
                lo, hi = float(f1.replace(",", "")), float(f2.replace(",", ""))
            except ValueError:
                return
            if lo > hi:
                raise ApplicationDefinedError("The minimum must be less than or equal to the maximum")


# ---------------------------------------------------------------------------
# AutoFilter
# ---------------------------------------------------------------------------
_TOP = {3: (True, False), 4: (False, False), 5: (True, True), 6: (False, True)}


def range_autofilter(rng, Field=None, Criteria1=None, Operator=1, Criteria2=None, VisibleDropDown=True):
    sheet = rng._sheet
    a0 = rng._areas[0]
    # a table with buttons of its own (the sheet's AutoFilter is elsewhere): its filter, as in Excel
    table = _rq("af.owner", sheet=sheet, row=a0[0], col=a0[1]) or 0
    af = _rq("af.get", sheet=sheet, table=table) if table else _rq("af.get", sheet=sheet)
    if table and not (Field is None and Criteria1 is None):
        pass
    elif Field is None and Criteria1 is None:
        # no arguments: toggle
        if af and af["on"]:
            _rq("af.set", sheet=sheet, rect=None)
        else:
            _rq("af.set", sheet=sheet, rect=_filter_rect(rng))
        return True
    if not table and (not af or not af["on"]):
        _rq("af.set", sheet=sheet, rect=_filter_rect(rng))
    p = {"sheet": sheet, "field": int(Field or 1)}
    if table:
        p["table"] = table
    op = int(Operator or 1)
    if Criteria1 is None and op != 14:
        p["kind"] = 0
    elif op == 7:  # xlFilterValues
        vals = Criteria1 if isinstance(Criteria1, (list, tuple)) else [Criteria1]
        p.update(kind=1, values=[str(v).lstrip("=") for v in vals if v not in ("", "=")], blanks=any(v in ("", "=") for v in vals))
    elif op in _TOP:
        top, pct = _TOP[op]
        p.update(kind=3, top=top, percent=pct, count=int(Criteria1))
    elif op in (8, 9):  # xlFilterCellColor, xlFilterFontColor: Criteria1 is an RGB color
        v = int(Criteria1)
        argb = 0xFF000000 | ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF)
        p.update(kind=4 if op == 8 else 5, color=argb)
    elif op in (12, 13):  # xlFilterNoFill, xlFilterAutomaticFontColor
        p.update(kind=4 if op == 12 else 5, color=0)
    elif op == 10:  # xlFilterIcon: Criteria1 is ActiveWorkbook.IconSets(xl3Arrows).Item(n) (1 = the lowest values' icon)
        if Criteria1 is None or not hasattr(Criteria1, "Parent"):
            p.update(kind=7, icon_set=0, icon=-1)
        else:
            p.update(kind=7, icon_set=int(Criteria1.Parent.ID), icon=int(Criteria1.Index) - 1)
    elif op == 14:  # xlFilterNoIcon: the cells without a conditional formatting icon
        p.update(kind=7, icon_set=0, icon=-1)
    elif op == 11:  # xlFilterDynamic: Criteria1 is an XlDynamicFilterCriteria (xlFilterToday, xlFilterAboveAverage...)
        p.update(kind=6, dynamic=int(Criteria1))
    else:
        c1 = str(Criteria1)
        if c1[:1] not in "<>=":
            c1 = "=" + c1
        p.update(kind=2, crit1=c1)
        if Criteria2 is not None:
            c2 = str(Criteria2)
            if c2[:1] not in "<>=":
                c2 = "=" + c2
            p.update(crit2=c2, **{"or": op == 2})
    _rq("af.column", **p)
    return True


def _filter_rect(rng):
    a = rng._areas[0]
    if a[0] == a[2] and a[1] == a[3]:
        return list(rng.CurrentRegion._areas[0])
    return list(a)


class AutoFilter:
    """Worksheet.AutoFilter (None when the sheet has no filter); ListObject.AutoFilter (a table's own buttons)."""

    def __init__(self, sheet_id: int, table: int = 0):
        self._sheet = sheet_id
        self._table = table

    def _get(self):
        return _rq("af.get", sheet=self._sheet, table=self._table)

    @property
    def Range(self):  # noqa: N802
        from . import excel

        return excel.Range._from(self._sheet, [tuple(self._get()["range"])])

    @property
    def FilterMode(self):  # noqa: N802
        return self._get()["hidden_rows"] > 0

    def ShowAllData(self):  # noqa: N802
        _rq("af.show_all", sheet=self._sheet, table=self._table)

    def ApplyFilter(self):  # noqa: N802
        _rq("af.reapply", sheet=self._sheet, table=self._table)

    @property
    def Filters(self):  # noqa: N802
        """AutoFilter.Filters: Filters(i) / Filters.Item(i) is column i of the list (a Filter: On, Criteria1, Criteria2,
        Operator), Filters.Count the list's columns, as in Excel; Filters[k] and len(Filters) still give the filtered
        columns' settings (dicts)."""
        return _Filters(self)

    @property
    def Sort(self):  # noqa: N802
        """AutoFilter.Sort: the list's sort (SortFields, Apply); the filter buttons show it and Reapply sorts again."""
        return AutoFilterSort(self._sheet, self._table)


class _Filters:
    def __init__(self, af):
        self._af = af

    def _columns(self):
        return self._af._get()["columns"]

    @property
    def Count(self):  # noqa: N802
        r = self._af._get()["range"]
        return int(r[3]) - int(r[1]) + 1

    def Item(self, Index):  # noqa: N802,N803
        i = int(Index)
        if not 1 <= i <= self.Count:
            raise IndexError(f"AutoFilter.Filters({i}): the list has {self.Count} columns")
        return Filter(self._af, i)

    def __call__(self, Index):  # noqa: N803
        return self.Item(Index)

    def __getitem__(self, k):
        return self._columns()[k]

    def __len__(self):
        return len(self._columns())

    def __iter__(self):
        return iter(self._columns())


class Filter:
    """One column of an AutoFilter (AutoFilter.Filters(i)): On, Criteria1, Criteria2, Operator, as Excel reads them."""

    def __init__(self, af, field):
        self._af, self._field = af, field

    def _col(self):
        for c in self._af._get()["columns"]:
            if int(c["field"]) == self._field and int(c["kind"]) != 0:
                return c
        return None

    @property
    def On(self):  # noqa: N802
        return self._col() is not None

    def _read(self):
        """(Operator, Criteria1, Criteria2) as Excel gives them."""
        c = self._col()
        if c is None:
            return 0, None, None
        k = int(c["kind"])
        if k == 1:
            vals = ["=" + v for v in c["values"]] + (["="] if c.get("blanks") else [])
            if len(vals) == 1:
                return 0, vals[0], None
            if len(vals) == 2:
                return 2, vals[0], vals[1]  # xlOr
            return 7, vals, None  # xlFilterValues
        if k == 2:
            if c.get("crit2"):
                return (2 if c.get("or") else 1), c["crit1"], c["crit2"]
            return 0, c["crit1"], None
        if k == 3:
            op = {(True, False): 3, (False, False): 4, (True, True): 5, (False, True): 6}[(bool(c["top"]), bool(c["percent"]))]
            return op, int(c["count"]), None
        if k in (4, 5):
            argb = int(c.get("color", 0))
            if not argb:
                return (12 if k == 4 else 13), None, None  # xlFilterNoFill / xlFilterAutomaticFontColor
            return (8 if k == 4 else 9), ((argb & 0xFF) << 16) | (argb & 0xFF00) | ((argb >> 16) & 0xFF), None
        if k == 6:
            return 11, int(c.get("dynamic", 0)), None  # xlFilterDynamic
        if k == 7:
            return (10 if int(c.get("icon_set", 0)) else 14), None, None  # xlFilterIcon / xlFilterNoIcon
        return 0, None, None

    @property
    def Operator(self):  # noqa: N802
        return self._read()[0]

    @property
    def Criteria1(self):  # noqa: N802
        return self._read()[1]

    @property
    def Criteria2(self):  # noqa: N802
        return self._read()[2]

    @property
    def Count(self):  # noqa: N802
        op, c1, c2 = self._read()
        return 0 if c1 is None else (len(c1) if isinstance(c1, list) else 1 + (c2 is not None))


class _AfSortField:
    def __init__(self, sheet_id, col, desc, table=0):
        self._sheet, self._col, self._table = sheet_id, col, table
        self.Order = 2 if desc else 1  # xlDescending / xlAscending

    @property
    def Key(self):  # noqa: N802
        from . import excel

        r = _rq("af.get", sheet=self._sheet, table=self._table)["range"]
        return excel.Range._from(self._sheet, [(r[0], r[1] + self._col, r[2], r[1] + self._col)])


class _AfSortFields:
    def __init__(self, owner):
        self._o = owner

    def _list(self):
        if self._o._pending is not None:
            return self._o._pending
        af = _rq("af.get", sheet=self._o._sheet, table=self._o._table) or {}
        return [_AfSortField(self._o._sheet, c, d, self._o._table) for c, d in af.get("sort", [])]

    @property
    def Count(self):  # noqa: N802
        return len(self._list())

    def __call__(self, Index):  # noqa: N803
        return self._list()[int(Index) - 1]

    Item = __call__

    def Clear(self):  # noqa: N802
        self._o._pending = []

    def Add(self, Key, SortOn=0, Order=1, CustomOrder=None, DataOption=0):  # noqa: N802,N803
        """Key: a cell or column of the filter range."""
        r = _rq("af.get", sheet=self._o._sheet, table=self._o._table)["range"]
        col = (Key._a[1] if hasattr(Key, "_a") else int(Key)) - r[1]
        if col < 0 or col > r[3] - r[1]:
            raise ApplicationDefinedError("The sort key is outside the filter range")
        if self._o._pending is None:
            self._o._pending = []
        f = _AfSortField(self._o._sheet, col, Order == 2, self._o._table)
        self._o._pending.append(f)
        return f

    Add2 = Add


class AutoFilterSort:
    """Worksheet.AutoFilter.Sort: SortFields.Add(Key, Order=xlDescending), then Apply sorts the list's visible rows."""

    def __init__(self, sheet_id, table=0):
        self._sheet = sheet_id
        self._table = table
        self._pending = None
        self.SortFields = _AfSortFields(self)
        self.Header = 1  # xlYes: the filter range has its header row

    def Apply(self):  # noqa: N802
        levels = [[f._col, f.Order == 2] for f in self.SortFields._list()]
        _rq("af.sort", sheet=self._sheet, levels=levels, table=self._table)
        self._pending = None
