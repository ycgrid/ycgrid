# SPDX-License-Identifier: Apache-2.0
"""Round 9: files. A save that fails raises with its reason, as Excel's object model raises error 1004: Save, SaveAs,
SaveCopyAs and Close(SaveChanges=True) returned False, which no script read, so a script never knew its report
wasn't written. Each test fails without its fix.

Run: python -m unittest tests.test_round9_files (with the bundled Python).
"""
from __future__ import annotations

import os
import shutil
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from test_round9_typing import EXE, LIVE  # noqa: E402

import yc  # noqa: E402


@unittest.skipUnless(LIVE, "no built ycgrid.exe")
class SaveFailsTest(unittest.TestCase):
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

        self.dir = tempfile.mkdtemp(prefix="yc_round9_files_")
        excel.Application.DisplayAlerts = False
        excel.ActiveSheet.Range("A1:B2").Value = [["Sym", "Px"], ["TXF", 23000]]

    def tearDown(self):
        for root, _dirs, files in os.walk(self.dir):
            for f in files:
                os.chmod(os.path.join(root, f), stat.S_IWRITE | stat.S_IREAD)
        shutil.rmtree(self.dir, ignore_errors=True)

    def read_only_file(self, name, data=b"old"):
        p = os.path.join(self.dir, name)
        with open(p, "wb") as f:
            f.write(data)
        os.chmod(p, stat.S_IREAD)
        return p

    def test_save_copy_as_to_a_name_windows_refuses_raises(self):
        from yc import excel

        with self.assertRaises(yc.YcError) as cm:
            excel.ActiveWorkbook.SaveCopyAs(os.path.join(self.dir, "a?b.ycg"))
        self.assertIn("doesn't allow this file name", str(cm.exception))
        self.assertEqual(os.listdir(self.dir), [])  # nothing written, no temporary file left

    def test_save_copy_as_over_a_read_only_file_raises_and_keeps_it(self):
        from yc import excel

        for name in ("copy.ycg", "copy.csv"):
            p = self.read_only_file(name)
            with self.assertRaises(yc.YcError) as cm:
                excel.ActiveWorkbook.SaveCopyAs(p)
            self.assertIn("read-only", str(cm.exception), name)
            with open(p, "rb") as f:
                self.assertEqual(f.read(), b"old", name)
        self.assertEqual(sorted(os.listdir(self.dir)), ["copy.csv", "copy.ycg"])

    def test_save_copy_as_into_a_new_folder_makes_it(self):
        # as SaveAs does (it reported success and wrote nothing)
        from yc import excel

        p = os.path.join(self.dir, "reports", "today", "fills.ycg")
        excel.ActiveWorkbook.SaveCopyAs(p)
        self.assertGreater(os.path.getsize(p), 0)

    def test_save_as_save_and_close_raise_when_the_file_is_read_only(self):
        from yc import excel

        wb = excel.ActiveWorkbook
        book = os.path.join(self.dir, "book.ycg")
        wb.SaveAs(book)
        os.chmod(book, stat.S_IREAD)
        excel.ActiveSheet.Range("C1").Value = 1
        with self.assertRaises(yc.YcError) as cm:
            wb.Save()
        self.assertIn("read-only", str(cm.exception))
        with self.assertRaises(yc.YcError):
            wb.SaveAs(self.read_only_file("other.ycg"))
        with self.assertRaises(yc.YcError) as cm:
            wb.Close(SaveChanges=True)
        self.assertIn("read-only", str(cm.exception))
        self.assertEqual(excel.ActiveSheet.Range("C1").Value, 1)  # still open, nothing lost
        os.chmod(book, stat.S_IWRITE | stat.S_IREAD)
        wb.Save()
        self.assertTrue(wb.Saved)


if __name__ == "__main__":
    unittest.main()
