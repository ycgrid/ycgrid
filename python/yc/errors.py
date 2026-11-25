# SPDX-License-Identifier: Apache-2.0
"""Exception classes (mapped to Excel error numbers; PLAN object model §3A)."""
from __future__ import annotations

from ._conn import YcConnectionError, YcError

__all__ = [
    "YcError", "YcConnectionError", "SubscriptOutOfRangeError", "TypeMismatchError",
    "ApplicationDefinedError", "FormulaError", "UnsupportedError", "ProtectedError", "CodeInterruptedError", "BY_CODE",
]


class SubscriptOutOfRangeError(YcError, KeyError, IndexError):
    def __init__(self, msg="Subscript out of range"):
        YcError.__init__(self, msg, 9, "E_NOT_FOUND")

    def __str__(self):
        return self.Description


class TypeMismatchError(YcError, TypeError):
    def __init__(self, msg="Type mismatch"):
        YcError.__init__(self, msg, 13, "E_TYPE")


class ApplicationDefinedError(YcError):
    def __init__(self, msg="Application-defined or object-defined error"):
        super().__init__(msg, 1004, "E_APP")


class FormulaError(ApplicationDefinedError):
    def __init__(self, msg="The formula has an error", Position: int = 0):  # noqa: N803
        super().__init__(msg)
        self.Code = "E_FORMULA"
        self.Position = Position


class ProtectedError(ApplicationDefinedError):
    """A change refused by sheet / workbook protection, or a wrong password (Excel error 1004)."""

    def __init__(self, msg="The cell or chart you're trying to change is on a protected sheet"):
        super().__init__(msg)
        self.Code = "E_PROTECTED"


class CodeInterruptedError(YcError):
    """Ctrl+Break while Application.EnableCancelKey is xlErrorHandler: Excel's trappable error 18, raised where the
    code was running so a handler can clean up (`except CodeInterruptedError:`)."""

    def __init__(self, msg="Code execution has been interrupted"):
        YcError.__init__(self, msg, 18, "E_INTERRUPTED")


class UnsupportedError(YcError, NotImplementedError):
    def __init__(self, msg="This member is not implemented yet"):
        YcError.__init__(self, msg, 438, "E_UNSUPPORTED")


BY_CODE = {
    "E_NOT_FOUND": SubscriptOutOfRangeError,
    "E_TYPE": TypeMismatchError,
    "E_APP": ApplicationDefinedError,
    "E_FORMULA": FormulaError,
    "E_UNSUPPORTED": UnsupportedError,
    "E_CONNECTION": YcConnectionError,
    "E_PROTECTED": ProtectedError,
    "E_PASSWORD": ProtectedError,
}
