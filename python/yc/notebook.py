# SPDX-License-Identifier: Apache-2.0
"""[YC] The code cells of workbooks saved with the former Notebook window.

The notebook was removed (ADR 0011). Old workbooks keep their cells: read them, or write them to a plain .ipynb:

    from yc import notebook
    notebook.cells()                  # [{"code": ...}, ...]
    notebook.export_ipynb("old.ipynb")
"""
from __future__ import annotations

import os

from ._conn import current as _conn

__all__ = ["cells", "export_ipynb"]


def cells() -> list:
    """[{code}] for every cell the workbook kept."""
    return _conn().request("notebook.cells", {})


def export_ipynb(path) -> str:
    """Write the kept cells to a Jupyter notebook (nbformat 4, kernel python3); returns the path."""
    path = os.path.abspath(str(path))
    _conn().request("notebook.export", {"path": path})
    return path
