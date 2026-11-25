# SPDX-License-Identifier: Apache-2.0
"""Excel's Analysis ToolPak (Data > Data Analysis) from Python, with the procedure names and arguments of Excel's
ATPVBAEN.XLAM, so `Application.Run("ATPVBAEN.XLAM!Descr", ...)` ported from Excel works unchanged:

    from yc.excel import *
    Application.Run("ATPVBAEN.XLAM!Descr", Range("A1:A101"), Range("C1"), "C", True, True, 95, 1, 1)
    Application.Run("ATPVBAEN.XLAM!Regress", Range("A1:A50"), Range("B1:D50"), False, True, 99, Range("F1"), True)

    from yc import analysis
    analysis.Descr(Range("A1:A101"), "Stats", "C", True, True)      # a new sheet named Stats
    out = analysis.Ttest2(Range("A1:A31"), Range("B1:B31"), Range("D1"), True, 0.05)   # the output Range

The output range is the top-left cell of the output (an existing Range or an address); a text that isn't a cell
address, or None, puts the output on a new sheet with that name. The output is returned as a Range. Charts that the
Excel dialogs can add (chart output, residual plots) aren't made; plot the output with yc.plot.
"""
from __future__ import annotations

from typing import Optional

__all__ = ["Anova1", "Anova2", "Anova3", "Mcorrel", "Mcovar", "Descr", "Expon", "Ftestv", "Fourier", "Histogram", "Moveavg",
           "Random", "RankPerc", "Regress", "Sample", "Ttestm", "Ttest1", "Ttest2", "Ztestm", "run"]


def _range(v):
    from . import excel

    if v is None or v is False:
        return None
    v = excel._resolve(v)  # Selection, ActiveCell
    if isinstance(v, excel.Range):
        return v
    if isinstance(v, str):
        try:
            return excel.Range(v)
        except Exception:  # noqa: BLE001 - not an address: a new sheet's name (outputs only)
            return v
    raise TypeError("expected a Range or an address")


def _ref(v) -> Optional[dict]:
    r = _range(v)
    if r is None or isinstance(r, str):
        return None
    return {"sheet": r._sheet, "rect": list(r._a)}


def run(tool: str, inprng=None, outrng=None, inprng2=None, binrng=None, **options):
    """Runs a tool by Excel's procedure name (Descr, Regress, Ttest2...) with yc option names; returns the output Range."""
    from . import excel

    p = {"tool": tool, **{k: v for k, v in options.items() if v is not None}}
    for key, v in (("in", inprng), ("in2", inprng2), ("bins", binrng)):
        ref = _ref(v)
        if ref:
            p[key] = ref
    out = _range(outrng)
    if out is None or isinstance(out, str):
        p["new_sheet"] = out or ""
        p["active_sheet"] = excel.Application.ActiveSheet._id
    else:
        p["out"] = {"sheet": out._sheet, "rect": list(out._a)}
    r = excel._rq("analysis.run", **p)
    return excel.Range._from(r["sheet"], [r["rect"]])


def _grouped(g) -> bool:
    return str(g or "C").upper().startswith("R")


def Anova1(inprng, outrng=None, grouped="C", labels=False, alpha=0.05):  # noqa: N802
    return run("Anova1", inprng, outrng, by_rows=_grouped(grouped), labels=bool(labels), alpha=float(alpha))


def Anova2(inprng, outrng=None, sample_rows=1, alpha=0.05):  # noqa: N802
    return run("Anova2", inprng, outrng, rows_per_sample=int(sample_rows), labels=True, alpha=float(alpha))


def Anova3(inprng, outrng=None, labels=False, alpha=0.05):  # noqa: N802
    return run("Anova3", inprng, outrng, labels=bool(labels), alpha=float(alpha))


def Mcorrel(inprng, outrng=None, grouped="C", labels=False):  # noqa: N802
    return run("Mcorrel", inprng, outrng, by_rows=_grouped(grouped), labels=bool(labels))


def Mcovar(inprng, outrng=None, grouped="C", labels=False):  # noqa: N802
    return run("Mcovar", inprng, outrng, by_rows=_grouped(grouped), labels=bool(labels))


def Descr(inprng, outrng=None, grouped="C", labels=False, summary=True, ds_conf=None, k1=None, k2=None):  # noqa: N802
    conf = 0.0 if ds_conf in (None, False) else float(95 if ds_conf is True else ds_conf)
    return run("Descr", inprng, outrng, by_rows=_grouped(grouped), labels=bool(labels), summary=bool(summary), conf=conf,
               k1=int(k1 or 0), k2=int(k2 or 0))


def Expon(inprng, outrng=None, damp=0.3, stderrs=False, chart=False, labels=False):  # noqa: N802
    return run("Expon", inprng, outrng, damping=float(0.3 if damp is None else damp), std_errors=bool(stderrs), labels=bool(labels))


def Ftestv(inprng1, inprng2, outrng=None, labels=False, alpha=0.05):  # noqa: N802
    return run("Ftestv", inprng1, outrng, inprng2, labels=bool(labels), alpha=float(alpha))


def Fourier(inprng, outrng=None, inverse=False, labels=False):  # noqa: N802
    return run("Fourier", inprng, outrng, inverse=bool(inverse), labels=bool(labels))


def Histogram(inprng, outrng=None, binrng=None, pareto=False, chartc=False, chart=False, labels=False):  # noqa: N802
    return run("Histogram", inprng, outrng, binrng=binrng, pareto=bool(pareto), cumulative=bool(chartc), labels=bool(labels))


def Moveavg(inprng, outrng=None, interval=3, stderrs=False, chart=False, labels=False):  # noqa: N802
    return run("Moveavg", inprng, outrng, interval=int(interval or 3), std_errors=bool(stderrs), labels=bool(labels))


def Random(outrng=None, variables=1, points=None, distribution=1, seed=0, *params):  # noqa: N802
    """distribution: 1 uniform (from, to), 2 normal (mean, sd), 3 Bernoulli (p), 4 binomial (p, trials), 5 Poisson
    (lambda), 6 patterned (from, to, step, repeat each, repeat sequence), 7 discrete (a two-column value / probability
    Range)."""
    binrng = None
    nums = list(params)
    if int(distribution) == 7 and nums:
        binrng, nums = nums[0], nums[1:]
    opts = {f"p{i + 1}": float(v) for i, v in enumerate(nums[:5])}
    return run("Random", None, outrng, binrng=binrng, variables=int(variables or 1), points=int(points or 0),
               distribution=int(distribution), seed=int(seed or 0), **opts)


def RankPerc(inprng, outrng=None, grouped="C", labels=False):  # noqa: N802
    return run("RankPerc", inprng, outrng, by_rows=_grouped(grouped), labels=bool(labels))


def Regress(inpyrng, inpxrng, constant=False, labels=False, confid=None, soutrng=None, residuals=False, sresiduals=False,  # noqa: N802
            rplots=False, lplots=False, routrng=None, nplots=False, poutrng=None):
    """constant=True forces the intercept to zero (Excel's "Constant is Zero"); confid: a second confidence level (%)."""
    return run("Regress", inpyrng, soutrng, inpxrng, labels=bool(labels), zero_constant=bool(constant),
               confidence=0.0 if confid in (None, False) else float(confid), residuals=bool(residuals), std_residuals=bool(sresiduals),
               normal_probability=bool(nplots))


def Sample(inprng, outrng=None, method="P", rate=1, labels=False):  # noqa: N802
    periodic = str(method or "P").upper().startswith("P")
    return run("Sample", inprng, outrng, periodic=periodic, period=int(rate), count=int(rate), labels=bool(labels))


def Ttestm(inprng1, inprng2, outrng=None, labels=False, alpha=0.05, difference=0):  # noqa: N802
    """t-Test: Paired Two Sample for Means."""
    return run("Ttestm", inprng1, outrng, inprng2, labels=bool(labels), alpha=float(alpha), difference=float(difference or 0))


def Ttest1(inprng1, inprng2, outrng=None, labels=False, alpha=0.05, difference=0):  # noqa: N802
    """t-Test: Two-Sample Assuming Equal Variances."""
    return run("Ttest1", inprng1, outrng, inprng2, labels=bool(labels), alpha=float(alpha), difference=float(difference or 0))


def Ttest2(inprng1, inprng2, outrng=None, labels=False, alpha=0.05, difference=0):  # noqa: N802
    """t-Test: Two-Sample Assuming Unequal Variances."""
    return run("Ttest2", inprng1, outrng, inprng2, labels=bool(labels), alpha=float(alpha), difference=float(difference or 0))


def Ztestm(inprng1, inprng2, outrng=None, labels=False, alpha=0.05, difference=0, var1=None, var2=None):  # noqa: N802
    """z-Test: Two Sample for Means (var1 / var2: the known variances)."""
    return run("Ztestm", inprng1, outrng, inprng2, labels=bool(labels), alpha=float(alpha), difference=float(difference or 0),
               var1=float(var1 or 0), var2=float(var2 or 0))


_PROCS = {n.lower(): globals()[n] for n in __all__ if n != "run"}


def procedure(name: str):
    """The tool function for a macro name like "ATPVBAEN.XLAM!Descr" (None when it isn't a ToolPak procedure)."""
    n = str(name)
    if "!" in n:
        book, n = n.split("!", 1)
        if not book.strip("'").upper().startswith("ATPVBAEN"):
            return None
    return _PROCS.get(n.lower())
