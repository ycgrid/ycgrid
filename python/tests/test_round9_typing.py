# SPDX-License-Identifier: Apache-2.0
"""Round 9: AutoSum over a range, F4 / Ctrl+Y, a percent cell, entries that start with a sign, Ctrl+Shift+arrows and
the keys that end an entry, each as Excel 16 does it: measured by posting the same keys to a hidden Excel of the
probe's own (Taiwan's Windows). Each test fails without its fix.

Run: python -m unittest tests.test_round9_typing (with the bundled Python).
"""
from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

EXE = os.environ.get("YCGRID_EXE") or os.path.join(os.path.dirname(__file__), "..", "..", "dist_release", "ycgrid.exe")
LIVE = os.path.exists(EXE) and os.environ.get("YC_NO_LIVE") != "1"

# the sheet every case starts from: a header row, a label column, two columns of numbers
BASE = {"A1": "Item", "B1": "Qty", "C1": "Price", "A2": "Apple", "B2": 10, "C2": 1.5, "A3": "Pear", "B3": 20, "C3": 2.25,
        "A4": "Fig", "B4": 30, "C4": 3}
# and more data around it, for the keys that jump to the edge of the data
NAV = dict(BASE, E2=1, E3=2, E5=3, G1=9, G7=8, D3="=B3*C3", D4="=B4*C4")


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class TypingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        from yc import excel

        self.keys("k:Escape|k:Escape|m:2,2|w:2")
        self.ws = excel.Worksheets.Add(After=excel.Worksheets(excel.Worksheets.Count))
        self.ws.Activate()
        self.fill(BASE)

    def tearDown(self):
        self.keys("k:Escape|k:Escape|m:2,2|w:2")

    def fill(self, cells):
        for a, v in cells.items():
            if isinstance(v, str) and v.startswith("="):
                self.ws.Range(a).Formula = v
            else:
                self.ws.Range(a).Value2 = v

    def keys(self, script):
        self.conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
        time.sleep(0.2)

    def run_keys(self, select, *keys):
        """select, then the keys three frames apart (as a person types; the frame race is tested apart)"""
        self.keys(f"g:{select}|w:3|" + "|w:3|".join(keys) + "|w:4")

    def f(self, a):
        return self.ws.Range(a).Formula

    def v(self, a):
        return self.ws.Range(a).Value2

    def selection(self):
        from yc import excel

        return excel.Selection.Address.replace("$", ""), excel.ActiveCell.Address.replace("$", "")

    def edit_text(self):
        return (yc.ui.state().get("edit") or {}).get("text")


class AutoSumRangeTest(TypingTest):
    """Alt+= over a range: the totals go where they overwrite nothing (they overwrote B5's x, D4's formula, row 4)."""

    def test_totals_never_overwrite_what_is_below_or_beside(self):
        self.fill({"B5": "x"})
        self.run_keys("B2:C4", "k:a-Equal")
        self.assertEqual((self.v("B5"), self.f("B6"), self.f("C6")), ("x", "=SUM(B2:B4)", "=SUM(C2:C4)"))
        self.assertEqual(self.f("C5"), "")
        self.assertEqual(self.selection(), ("B2:C5", "B2"))  # one row more, though the totals are in row 6 (measured)

    def test_one_row_gets_its_total_in_the_first_free_cell_to_the_right(self):
        self.fill({"D4": "=B4*C4"})
        self.run_keys("B4:C4", "k:a-Equal")
        self.assertEqual((self.f("D4"), self.f("E4")), ("=B4*C4", "=SUM(B4:C4)"))
        self.assertEqual(self.selection(), ("B4:D4", "B4"))

    def test_the_row_below_must_be_free_in_every_column(self):
        self.fill({"D2": 5, "D3": 6})
        self.run_keys("B2:D3", "k:a-Equal")
        self.assertEqual((self.v("B4"), self.v("C4"), self.f("D4")), (30, 3, ""))
        self.assertEqual([self.f(a) for a in ("B5", "C5", "D5")], ["=SUM(B2:B3)", "=SUM(C2:C3)", "=SUM(D2:D3)"])
        self.assertEqual(self.selection(), ("B2:D4", "B2"))

    def test_an_earlier_total_below_is_redone_another_function_kept(self):
        self.fill({"B5": "=SUM(B2:B3)"})
        self.run_keys("B2:B4", "k:a-Equal")
        self.assertEqual((self.f("B5"), self.f("B6")), ("=SUM(B2:B4)", ""))
        self.fill({"B5": "=AVERAGE(B2:B4)"})
        self.run_keys("B2:B4", "k:a-Equal")
        self.assertEqual((self.f("B5"), self.f("B6")), ("=AVERAGE(B2:B4)", "=SUM(B2:B4)"))

    def test_an_empty_last_row_and_column_get_the_totals_and_the_corner(self):
        self.run_keys("B2:D5", "k:a-Equal")
        self.assertEqual([self.f(a) for a in ("D2", "D3", "D4")], ["=SUM(B2:C2)", "=SUM(B3:C3)", "=SUM(B4:C4)"])
        self.assertEqual([self.f(a) for a in ("B5", "C5", "D5")], ["=SUM(B2:B4)", "=SUM(C2:C4)", "=SUM(B5:C5)"])
        self.assertEqual(self.selection(), ("B2:D5", "B2"))

    def test_a_last_row_and_column_with_a_label_get_the_totals_beyond_them(self):
        self.fill({"D5": "x"})
        self.run_keys("B2:D5", "k:a-Equal")
        self.assertEqual([self.f(a) for a in ("E2", "E3", "E4")], ["=SUM(B2:D2)", "=SUM(B3:D3)", "=SUM(B4:D4)"])
        self.assertEqual([self.f(a) for a in ("B6", "C6", "E6")], ["=SUM(B2:B5)", "=SUM(C2:C5)", "=SUM(B6:D6)"])
        self.assertEqual((self.f("D6"), self.f("E5")), ("", ""))
        self.assertEqual(self.selection(), ("B2:E6", "B2"))

    def test_a_last_column_of_labels_gets_the_row_totals_beside_it(self):
        self.fill({"D2": "x", "D3": "y", "D4": "z"})
        self.run_keys("B2:D4", "k:a-Equal")
        self.assertEqual([self.f(a) for a in ("E2", "E3", "E4")], ["=SUM(B2:D2)", "=SUM(B3:D3)", "=SUM(B4:D4)"])
        self.assertEqual(self.f("B5"), "")
        self.assertEqual(self.selection(), ("B2:E4", "B2"))

    def test_the_header_row_and_the_label_column_are_left_out(self):
        self.run_keys("A1:D4", "k:a-Equal")
        self.assertEqual([self.f(a) for a in ("D1", "D2", "D3", "D4")], ["", "=SUM(B2:C2)", "=SUM(B3:C3)", "=SUM(B4:C4)"])
        self.assertEqual(self.selection(), ("B2:D4", "B2"))
        self.run_keys("A2:B4", "k:a-Equal")
        self.assertEqual((self.f("A5"), self.f("B5")), ("", "=SUM(B2:B4)"))
        self.assertEqual(self.selection(), ("B2:B5", "B2"))

    def test_empty_cells_get_the_first_cells_total_moved_along(self):
        self.run_keys("C5:C6", "k:a-Equal")
        self.assertEqual((self.f("C5"), self.f("C6")), ("=SUM(C2:C4)", "=SUM(C3:C5)"))
        self.run_keys("D2:D5", "k:a-Equal")
        self.assertEqual([self.f(a) for a in ("D2", "D5")], ["=SUM(B2:C2)", "=SUM(B5:C5)"])

    def test_empty_cells_under_a_label_sum_past_it(self):
        self.fill({"B5": "x"})
        self.run_keys("B6:C6", "k:a-Equal")
        self.assertEqual((self.f("B6"), self.f("C6")), ("=SUM(B2:B5)", "=SUM(C2:C5)"))

    def test_a_block_of_empty_cells_or_labels_only_opens_the_editor(self):
        self.run_keys("B5:C6", "k:a-Equal")
        self.assertEqual(self.edit_text(), "=SUM(B2:B4)")
        self.keys("k:Escape|w:3")
        self.assertEqual((self.f("B5"), self.f("C6")), ("", ""))
        self.fill({"A5": "Total"})
        self.run_keys("A5:C5", "k:a-Equal")
        self.assertEqual(self.edit_text(), "=SUM()")
        self.keys("k:Escape|w:3")
        self.assertEqual(self.v("A5"), "Total")

    def test_one_cell_range_is_written_as_one_reference(self):
        self.run_keys("C2:D4", "k:a-Equal")
        self.assertEqual([self.f(a) for a in ("D2", "D3", "D4")], ["=SUM(C2)", "=SUM(C3)", "=SUM(C4)"])


class AutoSumCellTest(TypingTest):
    """Alt+= in one cell: the proposed range is selected (typing replaces it) and pointed at from its first cell."""

    def test_typing_replaces_the_proposed_range(self):
        self.run_keys("B6", "k:a-Equal", "t:,C2:C4", "k:Enter")
        self.assertEqual(self.f("B6"), "=SUM(,C2:C4)")
        self.run_keys("B7", "k:a-Equal", "k:Backspace", "t:B2:B3", "k:Enter")
        self.assertEqual(self.f("B7"), "=SUM(B2:B3)")

    def test_labels_just_above_are_passed_over(self):
        self.fill({"B5": "x"})
        self.run_keys("B6", "k:a-Equal")
        self.assertEqual(self.edit_text(), "=SUM(B2:B5)")  # it proposed =SUM() under a label
        self.keys("k:Escape|w:3")
        self.fill({"B5": True})
        self.run_keys("B6", "k:a-Equal")
        self.assertEqual(self.edit_text(), "=SUM()")  # TRUE stops it, as in Excel
        self.keys("k:Escape|w:3")

    def test_the_arrows_point_from_the_ranges_first_cell(self):
        self.run_keys("B6", "k:a-Equal", "k:DownArrow", "k:Enter")
        self.assertEqual(self.f("B6"), "=SUM(B3)")  # it was =SUM(B6), a circular reference
        self.run_keys("B6", "k:a-Equal", "k:s-UpArrow", "k:Enter")
        self.assertEqual(self.f("B6"), "=SUM(B2:B4)")
        self.run_keys("D4", "k:a-Equal", "k:LeftArrow", "k:Enter")
        self.assertEqual(self.f("D4"), "=SUM(A4)")


class RepeatTest(TypingTest):
    """F4 and Ctrl+Y repeat the last command, never typing; after an Undo they redo."""

    def test_typing_is_never_repeated_and_never_ends_the_repeat(self):
        self.run_keys("E1", "k:c-s-4", "t:abc", "k:Enter", "k:RightArrow", "k:F4")
        self.assertEqual((self.v("F2"), self.ws.Range("F2").NumberFormat), (None, "$#,##0.00_);[Red]($#,##0.00)"))
        self.run_keys("B2", "k:c-s-4", "k:F2", "k:Enter", "k:F4")
        self.assertEqual(self.v("B3"), 20)  # F2, Enter on B2 then F4 typed 10 over it
        self.assertEqual(self.ws.Range("B3").NumberFormat, "$#,##0.00_);[Red]($#,##0.00)")

    def test_the_delete_key_is_repeated(self):
        self.run_keys("B2", "k:Delete", "k:DownArrow", "k:F4")
        self.assertEqual((self.v("B2"), self.v("B3")), (None, None))
        self.run_keys("C2", "k:Delete", "t:5", "k:Enter", "k:c-Y")
        self.assertEqual((self.v("C2"), self.v("C3")), (5, None))

    def test_f4_after_an_undo_redoes(self):
        self.run_keys("B2", "k:c-s-4", "k:c-Z", "k:DownArrow", "k:F4")
        self.assertEqual(self.ws.Range("B2").NumberFormat, "$#,##0.00_);[Red]($#,##0.00)")
        self.assertEqual(self.ws.Range("B3").NumberFormat, "General")
        self.assertEqual(self.selection()[1], "B2")

    def test_after_a_redo_ctrl_y_repeats(self):
        self.run_keys("B2", "k:c-s-4", "k:c-Z", "k:c-Y", "k:DownArrow", "k:c-Y")
        self.assertEqual(self.ws.Range("B3").NumberFormat, "$#,##0.00_);[Red]($#,##0.00)")


class PercentEntryTest(TypingTest):
    """A number typed into a percent cell is a percentage (Excel's automatic percent entry): 7 was 700%."""

    def typed(self, fmt, *keys, value=None):
        self.ws.Range("E1").Clear()
        if value is not None:
            self.ws.Range("E1").Value2 = value
        self.ws.Range("E1").NumberFormat = fmt
        self.run_keys("E1", *keys)
        return self.ws.Range("E1").Formula, self.v("E1")

    def test_a_number_typed_is_a_percentage(self):
        self.assertEqual(self.typed("0%", "t:7", "k:Enter")[1], 0.07)
        self.assertEqual(self.typed("0%", "t:1", "t:.", "t:5", "k:Enter")[1], 0.015)
        self.assertEqual(self.typed("0%", "t:.", "t:5", "k:Enter")[1], 0.005)
        self.assertEqual(self.typed("0%", "t:1", "t:0", "t:0", "k:Enter")[1], 1)
        self.assertEqual(self.typed("0%", "t:1", "t:2", "t:%", "k:Enter")[1], 0.12)  # the % typed takes the place of Excel's
        self.assertEqual(self.typed("0%", "t:5", "k:Tab")[1], 0.05)
        self.assertEqual(self.typed("0.00%", "t:7", "k:Enter", value=0.5)[1], 0.07)
        self.assertEqual(self.ws.Range("E1").NumberFormat, "0.00%")

    def test_what_is_not_a_number_keeps_its_percent_sign_or_stays_as_typed(self):
        self.assertEqual(self.typed("0%", "t:5", "t:a", "k:Enter"), ("5a%", "5a%"))
        self.assertEqual(self.typed("0%", "t:=", "t:5", "k:Enter"), ("=5", 5))
        self.assertEqual(self.typed("0%", "t:a", "t:5", "k:Enter")[1], "a5")

    def test_editing_a_percentage_keeps_it_one(self):
        self.assertEqual(self.typed("0%", "k:F2", "k:Backspace", "k:Enter", value=0.1)[1], 0.1)
        self.assertEqual(self.typed("0%", "k:F2", "k:Home", "t:2", "k:Enter", value=0.1)[1], 2.1)
        self.assertEqual(self.typed("0%", "t:5", "k:F2", "k:F2", "t:1", "k:Enter")[1], 0.51)

    def test_ctrl_shift_5_then_typing(self):
        self.run_keys("E2", "k:c-s-5", "t:7", "k:Enter")
        self.assertEqual(self.v("E2"), 0.07)

    def test_python_writes_the_value_as_given(self):
        self.ws.Range("E1").NumberFormat = "0%"
        self.ws.Range("E1").Value = 7
        self.assertEqual(self.v("E1"), 7)


class SignedEntryTest(TypingTest):
    """An entry that starts with + or - is a formula unless it is an amount: +1+2 was the text +1+2."""

    def test_signed_entries(self):
        for typed, formula, value in (("+1+2", "=1+2", 3), ("-1-2", "=-1-2", -3), ("+1/2", "=1/2", 0.5), ("-2^2", "=-2^2", 4),
                                      ("-5", "-5", -5), ("+5", "5", 5), ("-$5", "-5", -5), ("+B2+C2", "=+B2+C2", 11.5)):
            with self.subTest(typed=typed):
                self.run_keys("E1", f"t:{typed}", "k:Enter")
                self.assertEqual((self.f("E1"), self.v("E1")), (formula, value))


class SelectionKeysTest(TypingTest):
    """Ctrl+Shift+arrows from a range jump along the active cell's row or column; Ctrl+Shift+Home / End grow."""

    def setUp(self):
        super().setUp()
        self.fill(NAV)

    def test_ctrl_shift_arrows_from_a_range(self):
        for select, keys, want in (("A1", ("k:c-s-DownArrow", "k:c-s-RightArrow"), "A1:C4"),
                                   ("B2", ("k:s-DownArrow", "k:c-s-RightArrow"), "B2:C3"),
                                   ("C1", ("k:s-RightArrow", "k:c-s-DownArrow"), "C1:D4"),
                                   ("C1", ("k:s-DownArrow", "k:s-DownArrow", "k:s-DownArrow", "k:c-s-RightArrow"), "C1:G4"),
                                   ("B2", ("k:c-s-DownArrow", "k:c-s-RightArrow", "k:c-s-UpArrow"), "B1:C2")):
            with self.subTest(select=select, keys=keys):
                self.run_keys(select, *keys)
                self.assertEqual(self.selection()[0], want)

    def test_ctrl_shift_home_and_end_grow_the_selection(self):
        self.run_keys("C3", "k:s-RightArrow", "k:c-s-Home")
        self.assertEqual(self.selection(), ("A1:D3", "C3"))
        self.run_keys("C3", "k:s-LeftArrow", "k:c-s-End")
        self.assertEqual(self.selection(), ("B3:G7", "C3"))

    def test_shift_tab_never_starts_the_column_enter_goes_back_to(self):
        self.run_keys("E3", "k:s-Tab", "k:Enter")
        self.assertEqual(self.selection()[1], "D4")
        self.run_keys("E3", "t:a", "k:s-Tab", "t:b", "k:Tab", "t:c", "k:Enter")
        self.assertEqual(self.selection()[1], "D4")
        self.run_keys("E3", "t:a", "k:Tab", "k:Tab", "k:s-Tab", "k:s-Tab", "k:s-Tab", "t:b", "k:Enter")
        self.assertEqual(self.selection()[1], "E4")  # a chain the Tabs began goes on


class EnterModeKeysTest(TypingTest):
    """In Enter mode the keys that move on the sheet end the entry, then move there; pointing, they point."""

    def test_keys_that_end_an_entry_then_act_on_the_sheet(self):
        self.run_keys("E1", "t:abc", "k:c-Home", "t:x", "k:Enter")
        self.assertEqual((self.v("A1"), self.v("E1"), self.selection()[1]), ("x", "abc", "A2"))
        self.run_keys("E1", "t:abc", "k:s-RightArrow", "t:x", "k:Enter")
        self.assertEqual((self.v("E1"), self.selection()), ("x", ("E1:F1", "F1")))
        self.run_keys("E1", "t:abc", "k:c-LeftArrow", "t:x", "k:Enter")
        self.assertEqual((self.v("C1"), self.v("E1"), self.selection()[1]), ("x", "abc", "C2"))
        self.run_keys("E1", "t:abc", "k:Home", "t:y", "k:Enter")
        self.assertEqual((self.v("A1"), self.v("E1")), ("y", "abc"))
        self.run_keys("E3", "t:abc", "k:End", "k:DownArrow", "t:x", "k:Enter")
        self.assertEqual((self.v("E3"), self.v("E1048576")), ("abc", "x"))

    def test_pointing_keys(self):
        # (Ctrl+End's cell first: a cell written and cleared stays in the used area, in Excel too, until a save)
        for keys, want in ((("t:=SUM(", "k:c-Home", "k:c-s-End"), "=SUM(A1:C4)"), (("t:=B2+", "k:Home"), "=B2+A1"),
                           (("t:=SUM(", "k:LeftArrow", "k:s-Home"), "=SUM(A1:D1)"), (("t:=B2+", "k:End", "k:LeftArrow"), "=B2+C1")):
            with self.subTest(keys=keys):
                self.ws.Range("E1").ClearContents()
                self.run_keys("E1", *keys, "k:Enter")
                self.assertEqual(self.f("E1"), want)


if __name__ == "__main__":
    unittest.main()
