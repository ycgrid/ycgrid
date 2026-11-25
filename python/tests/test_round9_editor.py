# SPDX-License-Identifier: Apache-2.0
"""Round 9: the cell editor's own keys as Excel 16 has them (measured by posting the same keys to a hidden Excel of
the probe's own): Ctrl+arrows by Excel's words, Ctrl+Delete, Up and Down at the ends of a line, Ctrl+Z / Ctrl+Y for
a run of typing, F4 on the reference at the caret, AutoComplete's candidates, and keys sent one frame after another
(the editor took the keyboard a frame or two late and lost them). Each test fails without its fix.

Run: python -m unittest tests.test_round9_editor (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from test_round9_typing import LIVE, TypingTest  # noqa: E402

W1 = "SUM(B2:B4)*2+C2,D3 x"
W2 = 'ab cd.ef-gh_ij/kl 1.5% "q" a&b'


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class EditorKeysTest(TypingTest):
    """Edit mode (F2): Excel's words, Ctrl+Delete, Up and Down."""

    def caret_after(self, text, *keys):
        self.ws.Range("E1").Value2 = text
        self.run_keys("E1", "k:F2", *keys, "t:^", "k:Enter")
        return self.v("E1")

    def test_ctrl_left_and_right_stop_at_excels_words(self):
        for n, want in ((1, "SUM(B2:B4)*2+C2,D3 ^x"), (2, "SUM(B2:B4)*2+C2,^D3 x"), (3, "SUM(B2:B4)*2+C2^,D3 x"),
                        (7, "SUM(B2:B4^)*2+C2,D3 x"), (10, "SUM(^B2:B4)*2+C2,D3 x")):
            with self.subTest(left=n):
                self.assertEqual(self.caret_after(W1, *["k:c-LeftArrow"] * n), want)
        for n, want in ((1, "SUM^(B2:B4)*2+C2,D3 x"), (6, "SUM(B2:B4)*^2+C2,D3 x")):
            with self.subTest(right=n):
                self.assertEqual(self.caret_after(W1, "k:Home", *["k:c-RightArrow"] * n), want)
        self.assertEqual(self.caret_after(W2, *["k:c-LeftArrow"] * 4), 'ab cd.ef-gh_ij/kl 1.5% "q^" a&b')
        self.assertEqual(self.caret_after(W2, "k:Home", *["k:c-RightArrow"] * 7), 'ab cd.ef-gh_^ij/kl 1.5% "q" a&b')

    def test_ctrl_shift_left_selects_by_words(self):
        for n, want in ((1, "=B2+C2*9"), (2, "=B2+C29"), (3, "=B2+9"), (5, "=9")):
            with self.subTest(n=n):
                self.ws.Range("E1").Formula = "=B2+C2*D3"
                self.run_keys("E1", "k:F2", *["k:c-s-LeftArrow"] * n, "t:9", "k:Enter")
                self.assertEqual(self.f("E1"), want)  # it replaced the whole entry

    def test_ctrl_delete_deletes_to_the_end_of_the_line(self):
        self.ws.Range("E1").Value2 = W1
        self.run_keys("E1", "k:F2", "k:c-LeftArrow", "k:c-LeftArrow", "k:c-Delete", "k:Enter")
        self.assertEqual(self.v("E1"), "SUM(B2:B4)*2+C2,")
        self.ws.Range("E1").Value2 = W1
        self.run_keys("E1", "k:F2", "k:Home", "k:c-Delete", "k:Enter")
        self.assertIsNone(self.v("E1"))

    def test_up_and_down_go_to_the_ends_of_a_line(self):
        for keys, want in ((("k:UpArrow",), 110), (("k:s-UpArrow",), 1), (("k:Home", "k:DownArrow"), 101),
                           (("k:Home", "k:s-DownArrow"), 1), (("k:LeftArrow", "k:UpArrow"), 110)):
            with self.subTest(keys=keys):
                self.ws.Range("B2").Value2 = 10
                self.run_keys("B2", "k:F2", *keys, "t:1", "k:Enter")
                self.assertEqual(self.v("B2"), want)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class EditorUndoTest(TypingTest):
    """Ctrl+Z in the editor takes back the last run of typing; the cell's own content comes back first."""

    def test_ctrl_z_takes_back_a_run_of_typing(self):
        for select, keys, want in (("B2", ("t:abc", "k:c-Z", "t:x"), "x"), ("B2", ("t:abc", "k:c-Z"), 10),
                                   ("B2", ("k:F2", "t:5", "t:6", "k:c-Z"), 10), ("B2", ("k:F2", "t:5", "k:LeftArrow", "t:6", "k:c-Z"), 105),
                                   ("B2", ("t:abc", "k:c-Z", "k:c-Y"), "abc"), ("B2", ("k:F2", "t:5", "k:c-Z", "t:7"), 107),
                                   ("E1", ("t:ab", "t:c", "k:c-Z", "t:d"), "d"), ("E1", ("t:abc", "k:c-Z"), None)):
            with self.subTest(keys=keys):
                self.ws.Range("B2").Value2 = 10
                self.ws.Range("E1").ClearContents()
                self.run_keys(select, *keys, "k:Enter")
                self.assertEqual(self.v(select), want)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class EditorF4Test(TypingTest):
    """F4 changes the one cell reference the caret is at (an end of a range on its own), every one selected, or all of
    one just pointed at; nothing when the caret is at none."""

    def test_f4_changes_the_reference_at_the_caret(self):
        for keys, want in ((("t:=B2:C3", "k:F2", "k:LeftArrow", "k:F4"), "=B2:$C$3"),
                           (("t:=B2:C3", "k:F2", "k:LeftArrow", "k:LeftArrow", "k:LeftArrow", "k:F4"), "=$B$2:C3"),
                           (("t:=SUM(B2:B4)", "k:F4"), "=SUM(B2:$B$4)"), (("t:=SUM(B2:B4", "k:F4", "k:F4"), "=SUM(B2:B$4)"),
                           (("t:=B2+C2", "k:F2", "k:Home", "k:F4"), "=B2+C2"), (("t:=B2+5", "k:F4"), "=B2+5"),
                           (("t:=B2 ", "k:F4"), "=B2"), (("t:=B2+C2", "k:F2", "k:s-Home", "k:F4"), "=$B$2+$C$2"),
                           (("t:=B2:C3", "k:F2", "k:s-LeftArrow", "k:s-LeftArrow", "k:F4"), "=B2:$C$3"),
                           (("t:=", "k:LeftArrow", "k:s-DownArrow", "k:F4"), "=$D$1:$D$2")):
            with self.subTest(keys=keys):
                self.ws.Range("E1").ClearContents()
                self.run_keys("E1", *keys, "k:Enter")
                self.assertEqual(self.f("E1"), want)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class AutoCompleteTest(TypingTest):
    """AutoComplete: the header row isn't a candidate, the letters typed keep their case, an entry as long as the text
    typed counts, and formulas' text and TRUE / FALSE are candidates."""

    def typed(self, select, *keys):
        self.run_keys(select, *keys, "k:Enter")
        return self.v(select)

    def test_the_header_row_is_left_out(self):
        # (it, not i: AutoCorrect makes a lone i I, as US Excel does; Taiwan's Excel keeps it)
        self.assertEqual(self.typed("A5", "t:it"), "it")  # Item over Apple and 10: a header (it completed Item)
        self.assertEqual(self.typed("A5", "t:ite"), "ite")
        self.fill({"B1": None, "C1": None, "B2": None, "C2": None, "B3": None, "C3": None, "B4": None, "C4": None})
        self.assertEqual(self.typed("A5", "t:it"), "Item")  # one column of text: no header

    def test_the_letters_typed_keep_their_case(self):
        self.assertEqual(self.typed("A5", "t:f", "k:Delete"), "f")
        self.assertEqual(self.typed("A5", "t:app", "k:Backspace"), "app")
        self.assertEqual(self.typed("A5", "t:a", "k:F2", "t:x"), "ax")
        self.assertEqual(self.typed("A5", "t:APP"), "Apple")  # Enter takes the entry as it is written
        self.assertEqual(self.typed("A5", "t:a", "k:a-Enter", "t:b"), "a\nb")

    def test_an_entry_as_long_as_the_text_typed_counts(self):
        self.fill({"A5": "Apple pie", "A6": "Figs"})
        self.assertEqual(self.typed("A7", "t:apple"), "apple")
        self.assertEqual(self.typed("A7", "t:fig"), "fig")
        self.assertEqual(self.typed("A7", "t:apple "), "Apple pie")

    def test_formula_text_and_logicals_are_candidates(self):
        self.ws.Range("A5").Formula = '="Kiwi"'
        self.assertEqual(self.typed("A6", "t:k"), "Kiwi")
        self.fill({"A7": "Trade", "A8": True})
        self.assertEqual(self.typed("A9", "t:tr"), "tr")


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class FrameRaceTest(TypingTest):
    """Keys one frame apart, as a fast typist or a busy screen sends them: the editor has the keyboard at once."""

    def fast(self, select, *keys):
        self.keys(f"g:{select}|w:3|" + "|".join(keys) + "|w:4")

    def test_keys_right_after_the_editor_opens(self):
        self.fast("A5", "t:a", "k:Enter")
        self.assertEqual(self.v("A5"), "Apple")  # the entry was written before AutoComplete saw it
        self.fast("A2", "k:F2", "k:s-Home", "t:x", "k:Enter")
        self.assertEqual(self.v("A2"), "x")
        self.ws.Range("E1").Value2 = W1
        self.fast("E1", "k:F2", "k:c-LeftArrow", "t:^", "k:Enter")
        self.assertEqual(self.v("E1"), "SUM(B2:B4)*2+C2,D3 ^x")
        self.fast("B3", "k:F2", "t:5", "k:c-Z", "k:Enter")
        self.assertEqual(self.v("B3"), 20)


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class SelectionCommandsTest(TypingTest):
    """Ctrl+Shift+8 and Ctrl+[ select as Excel's do."""

    def test_ctrl_shift_8_makes_the_regions_first_cell_active(self):
        self.run_keys("B2", "k:c-s-8")
        self.assertEqual(self.selection(), ("A1:C4", "A1"))

    def test_ctrl_bracket_joins_the_cells_it_selects(self):
        self.ws.Range("D3").Formula = "=B3*C3"
        self.run_keys("D3", "k:c-LeftBracket")
        self.assertEqual(self.selection(), ("B3:C3", "B3"))

    def test_a_fraction_typed_into_a_scientific_cell_shows_as_a_fraction(self):
        self.ws.Range("E1").NumberFormat = "0.00E+00"
        self.run_keys("E1", "t:1/2", "k:Enter")
        self.assertEqual((self.v("E1"), self.ws.Range("E1").NumberFormat), (0.5, "# ?/?"))


if __name__ == "__main__":
    unittest.main()
