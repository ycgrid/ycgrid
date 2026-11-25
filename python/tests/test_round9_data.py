# SPDX-License-Identifier: Apache-2.0
"""Round 9: data operations the way a trader does them, through the keys: a filtered list keeps its hidden rows when
the rows it shows are deleted, as in Excel. Each test fails without its fix.

Run: python -m unittest tests.test_round9_data (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from test_round9_typing import LIVE, TypingTest  # noqa: E402

DATA = [["Sym", "Side", "Qty", "Px"], ["TXF", "Buy", 2, 23000], ["MXF", "Sell", 1, 23010], ["txf", "Buy", 2, 23000],
        ["TXO", "Buy", 5, 300], ["MXF", "Sell", 1, 23010], ["TXF", "Sell", 3, 22990], [None, "Buy", 4, 23005], ["TE", "Buy", 10, 1000.5]]


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class FilteredListTest(TypingTest):
    def setUp(self):
        super().setUp()
        self.ws.Range("A1:D12").ClearContents()
        self.ws.Range("A1:D9").Value = DATA
        self.ws.Range("A1:D9").AutoFilter(2, "Buy")
        self.keys("w:4")

    def tearDown(self):
        self.ws.AutoFilterMode = False
        super().tearDown()

    def column(self, col="A", n=9):
        return [self.ws.Range(f"{col}{r}").Value for r in range(1, n + 1)]

    def test_deleting_rows_keeps_the_rows_the_filter_hides(self):
        # rows 4 and 5 are shown, row 6 (MXF Sell) hidden: Ctrl+- deletes 4 and 5 only (it deleted row 6 too)
        self.keys("g:A4:A6|w:2|k:s-Space|w:2|k:c-Minus|w:5")
        self.assertEqual(self.column("A"), ["Sym", "TXF", "MXF", "MXF", "TXF", None, "TE", None, None])
        self.assertEqual(self.column("B", 7), ["Side", "Buy", "Sell", "Sell", "Sell", "Buy", "Buy"])
        self.keys("k:c-Z|w:5")
        self.assertEqual(self.column("A"), [r[0] for r in DATA])  # Undo gives every row back

    def test_the_other_keys_act_on_the_rows_shown(self):
        # (as before: copied, filled and Ctrl+Enter only where the filter shows rows, as Excel)
        self.keys("g:F2:F9|w:2|t:x|k:c-Enter|w:4")
        self.assertEqual(self.column("F", 9)[1:], ["x", None, "x", "x", None, None, "x", "x"])
        self.keys("g:C2:C9|w:2|k:c-C|w:3|g:H1|w:2|k:c-V|w:4|k:Escape|w:2")
        self.assertEqual(self.column("H", 6), [2, 2, 5, 4, 10, None])


if __name__ == "__main__":
    unittest.main()
