# SPDX-License-Identifier: Apache-2.0
"""Round 9: big lists stay smooth. A copy writes its text only when something reads it, and the Clipboard pane
collects copies while it is shown, as Excel's Office Clipboard does. Each test fails without its fix.

Run: python -m unittest tests.test_round9_bigdata (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from test_round9_typing import LIVE, TypingTest  # noqa: E402


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class ClipboardPaneTest(TypingTest):
    def setUp(self):
        super().setUp()
        from yc import ui

        # the pane's items are the instance's: start from none (Clear All), whatever an earlier test copied
        ui.pane("clipboard", True)
        self.keys("w:3")
        self.click("Clear All")
        ui.pane("clipboard", False)
        self.ws.Range("A1:B3").Value = [["first", 1], ["second", 2], ["third", 3]]
        self.ws.Range("D1:E3").ClearContents()

    def tearDown(self):
        from yc import ui

        ui.pane("clipboard", False)
        super().tearDown()

    def click(self, label, prefix=False):
        from yc import ui

        hits = [i for i in ui.items("Clipboard") if (i["label"].startswith(label) if prefix else i["label"] == label)]
        self.assertTrue(hits, f"{label!r} isn't in the Clipboard pane: {self.pane_items()}")
        r = hits[0]["rect"]
        x, y = int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)
        self.keys(f"m:{x},{y}|w:3|c:{x},{y}|w:6|m:2,2|w:2")

    def pane_items(self):
        """the copies the pane lists, top to bottom"""
        from yc import ui

        items = [i for i in ui.items("Clipboard") if i["label"][:5] in ("first", "secon", "third")]
        return [i["label"] for i in sorted(items, key=lambda i: i["rect"][1])]

    def test_the_pane_collects_while_it_is_shown(self):
        # every Ctrl+C wrote out the copy's text for the pane (a million rows froze the window for over a second):
        # the pane collects while it is shown, the copy on the clipboard when it opens included, as in Excel
        from yc import ui

        self.keys("g:A1:B1|w:2|k:c-C|w:3|g:A2:B2|w:2|k:c-C|w:3")
        ui.pane("clipboard", True)
        self.keys("w:4")
        items = self.pane_items()
        self.assertEqual(len(items), 1, items)
        self.assertTrue(items[0].startswith("second"), items)
        self.keys("g:A3:B3|w:2|k:c-C|w:4")
        items = self.pane_items()
        self.assertEqual(len(items), 2, items)
        self.assertTrue(items[0].startswith("third"), items)  # the newest first

    def test_an_item_pastes_the_copied_text(self):
        # the text, written only when the pane read it, is the cells' text
        from yc import ui

        ui.pane("clipboard", True)
        self.keys("w:3|g:A2:B2|w:2|k:c-C|w:4|k:Escape|w:2|g:D1|w:2")
        self.click("second", prefix=True)
        self.assertEqual(self.ws.Range("D1").Value, "second")
        self.assertEqual(self.ws.Range("E1").Value, 2)


if __name__ == "__main__":
    unittest.main()
