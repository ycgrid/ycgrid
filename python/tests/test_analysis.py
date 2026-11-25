# SPDX-License-Identifier: Apache-2.0
"""yc.analysis: Excel's ToolPak procedures map to analysis.run requests (no YCGrid needed)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from yc import analysis, excel  # noqa: E402


class AnalysisTest(unittest.TestCase):
    def setUp(self):
        self.sent = []
        self._rq = excel._rq

        def fake(method, **p):
            self.sent.append((method, p))
            return {"sheet": 7, "rect": [0, 4, 20, 8]}

        excel._rq = fake

    def tearDown(self):
        excel._rq = self._rq

    def R(self, r0, c0, r1, c1, sheet=1):
        return excel.Range._from(sheet, [(r0, c0, r1, c1)])

    def test_procedure_names(self):
        self.assertIs(analysis.procedure("ATPVBAEN.XLAM!Descr"), analysis.Descr)
        self.assertIs(analysis.procedure("'ATPVBAEN.XLAM'!regress"), analysis.Regress)
        self.assertIsNone(analysis.procedure("Book1.xlsm!Descr"))
        self.assertIsNone(analysis.procedure("ATPVBAEN.XLAM!Nope"))

    def test_descr(self):
        out = analysis.Descr(self.R(0, 0, 100, 2), self.R(0, 4, 0, 4), "R", True, True, 95, 2, 1)
        m, p = self.sent[-1]
        self.assertEqual(m, "analysis.run")
        self.assertEqual(p["tool"], "Descr")
        self.assertEqual(p["in"], {"sheet": 1, "rect": [0, 0, 100, 2]})
        self.assertEqual(p["out"], {"sheet": 1, "rect": [0, 4, 0, 4]})
        self.assertTrue(p["by_rows"])
        self.assertEqual((p["conf"], p["k1"], p["k2"]), (95.0, 2, 1))
        self.assertEqual((out._sheet, out._a), (7, (0, 4, 20, 8)))

    def test_regress_and_tests(self):
        analysis.Regress(self.R(0, 0, 50, 0), self.R(0, 1, 50, 3), False, True, 99, self.R(0, 5, 0, 5), True, True, nplots=True)
        p = self.sent[-1][1]
        self.assertEqual(p["in2"]["rect"], [0, 1, 50, 3])
        self.assertEqual((p["zero_constant"], p["confidence"], p["residuals"], p["std_residuals"], p["normal_probability"]),
                         (False, 99.0, True, True, True))
        analysis.Ztestm(self.R(0, 0, 9, 0), self.R(0, 1, 9, 1), self.R(0, 3, 0, 3), False, 0.01, 0.5, 2.0, 3.0)
        p = self.sent[-1][1]
        self.assertEqual((p["tool"], p["alpha"], p["difference"], p["var1"], p["var2"]), ("Ztestm", 0.01, 0.5, 2.0, 3.0))

    def test_random_and_sample(self):
        analysis.Random(self.R(0, 0, 0, 0), 2, 100, 2, 42, 10.0, 3.0)
        p = self.sent[-1][1]
        self.assertEqual((p["variables"], p["points"], p["distribution"], p["seed"], p["p1"], p["p2"]), (2, 100, 2, 42, 10.0, 3.0))
        self.assertNotIn("in", p)
        analysis.Sample(self.R(0, 0, 99, 0), self.R(0, 2, 0, 2), "R", 10)
        p = self.sent[-1][1]
        self.assertEqual((p["periodic"], p["count"]), (False, 10))


if __name__ == "__main__":
    unittest.main()
