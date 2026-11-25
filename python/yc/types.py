# SPDX-License-Identifier: Apache-2.0
"""Value types: error values, pending results, the protocol encoding of Decimal, Excel date serial conversion."""
from __future__ import annotations

import datetime as _dt
import decimal
import struct

from . import _msgpack

__all__ = ["XlError", "Pending", "PENDING", "serial_to_datetime", "datetime_to_serial", "ERROR_TEXT"]

# same numbers as C++ core::Err (shared by .ycg and the protocol, never changed)
ERROR_TEXT = {
    1: "#NULL!", 2: "#DIV/0!", 3: "#VALUE!", 4: "#REF!", 5: "#NAME?", 6: "#NUM!", 7: "#N/A",
    8: "#GETTING_DATA", 9: "#SPILL!", 10: "#CALC!", 11: "#PYTHON!", 12: "#CONNECT!", 13: "#BLOCKED!",
}
# Excel CVErr numbers (xlErrDiv0 = 2007...) <-> internal numbers: Excel's XlCVError values (#GETTING_DATA 2043,
# #SPILL! 2045 and #CALC! 2050 measured in Excel 16). #PYTHON! has no number there: 2000 + its internal number
_Excel_TO_INTERNAL = {2000: 1, 2007: 2, 2015: 3, 2023: 4, 2029: 5, 2036: 6, 2042: 7, 2043: 8, 2045: 9, 2050: 10,
                      2011: 11, 2046: 12, 2047: 13}
_INTERNAL_TO_Excel = {v: k for k, v in _Excel_TO_INTERNAL.items()}


class XlError:
    """A cell error value (Excel's CVErr). Check with IsError(v)."""

    __slots__ = ("code",)

    def __init__(self, code: int):
        self.code = _Excel_TO_INTERNAL.get(code, code)

    @property
    def Number(self) -> int:  # noqa: N802 (Excel name)
        return _INTERNAL_TO_Excel.get(self.code, 2000 + self.code)

    @property
    def Text(self) -> str:  # noqa: N802
        return ERROR_TEXT.get(self.code, "#ERROR!")

    def __eq__(self, other):
        return isinstance(other, XlError) and other.code == self.code

    def __hash__(self):
        return hash(("XlError", self.code))

    def __repr__(self):
        return f"XlError({self.Text})"

    __str__ = lambda self: self.Text  # noqa: E731


class Pending:
    """An asynchronous result hasn't arrived yet (#GETTING_DATA)."""

    __slots__ = ()

    def __repr__(self):
        return "Pending"


PENDING = Pending()


def _enc_decimal(d: decimal.Decimal):
    sign, digits, exp = d.as_tuple()
    if not isinstance(exp, int):
        raise ValueError("Decimal can't be NaN or infinite")
    scale = -exp if exp < 0 else 0
    mant = int("".join(map(str, digits)) or "0") * (10 ** exp if exp > 0 else 1)
    if scale > 28:
        q = decimal.Decimal(1).scaleb(-28)
        return _enc_decimal(d.quantize(q, rounding=decimal.ROUND_HALF_UP))
    if sign:
        mant = -mant
    if not (-(1 << 111) <= mant < (1 << 111)):
        raise OverflowError("Decimal is outside the range of a 112-bit mantissa")
    return 1, struct.pack("<B", scale) + (mant & ((1 << 128) - 1)).to_bytes(16, "little")


def _dec_decimal(data: bytes) -> decimal.Decimal:
    scale = data[0]
    mant = int.from_bytes(data[1:17], "little", signed=True)
    return decimal.Decimal(mant).scaleb(-scale)


_msgpack.ext_encoders.append((decimal.Decimal, _enc_decimal))
_msgpack.ext_encoders.append((XlError, lambda e: (2, bytes([e.code]))))
_msgpack.ext_encoders.append((Pending, lambda e: (3, b"\x00")))
_msgpack.ext_decoders[1] = _dec_decimal
_msgpack.ext_decoders[2] = lambda data: XlError(data[0])
_msgpack.ext_decoders[3] = lambda data: PENDING

# ---- Excel date serials (1900 date system, including the nonexistent 1900/2/29) ----
_EPOCH = _dt.datetime(1899, 12, 30)


def serial_to_datetime(serial: float) -> _dt.datetime:
    if 1 <= serial < 61:
        serial += 1  # before 1900/3/1 (Excel's 1900 leap year bug); a time alone (< 1) stays on 1899-12-30, as in Excel
    return _EPOCH + _dt.timedelta(days=serial)


def datetime_to_serial(value) -> float:
    if isinstance(value, _dt.datetime):
        if value.tzinfo is not None:
            value = value.astimezone().replace(tzinfo=None)
        delta = value - _EPOCH
    elif isinstance(value, _dt.date):
        delta = _dt.datetime(value.year, value.month, value.day) - _EPOCH
    elif isinstance(value, _dt.timedelta):
        return value.total_seconds() / 86400.0
    else:
        raise TypeError(value)
    s = delta.days + delta.seconds / 86400.0 + delta.microseconds / 86400e6
    return s - 1 if 1 <= s < 61 else s
