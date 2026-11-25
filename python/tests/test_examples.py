# SPDX-License-Identifier: Apache-2.0
"""The example projects in examples/ and the backtest statistics they share (no YCGrid needed)."""
import contextlib
import importlib.util
import io
import math
import os
import re
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

try:
    import numpy as np
    import pandas as pd
except ImportError:  # pragma: no cover
    np = pd = None

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXAMPLES = os.path.join(ROOT, "examples")
LIB = os.path.join(EXAMPLES, "lib")
# project folders that are not one of the research and trading screens (no run())
PROJECTS = {"demo", "txo_market_maker"}


def folders():
    return sorted(d for d in os.listdir(EXAMPLES) if os.path.isdir(os.path.join(EXAMPLES, d)) and d not in ("lib", "__pycache__"))


def screens():
    return [d for d in folders() if d not in PROJECTS]


def load(name):
    """examples/<name>/main.py as a module (the shared lib importable), without opening its workbook."""
    if LIB not in sys.path:
        sys.path.insert(0, LIB)
    spec = importlib.util.spec_from_file_location(f"example_test_{name}", os.path.join(EXAMPLES, name, "main.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class CompileTest(unittest.TestCase):
    def test_every_module_compiles(self):
        """A syntax error in any yc module (the host...) or example would break YCGrid's Python host at start."""
        bad = []
        for root in (os.path.join(ROOT, "python", "yc"), EXAMPLES):
            for d, _, files in os.walk(root):
                for f in files:
                    if f.endswith(".py"):
                        path = os.path.join(d, f)
                        try:
                            with open(path, encoding="utf-8") as fh:
                                compile(fh.read(), path, "exec")
                        except SyntaxError as e:
                            bad.append(f"{path}:{e.lineno}: {e.msg}")
        self.assertEqual(bad, [])

    def test_the_examples_live_in_one_folder(self):
        """examples/ is the only place for examples: no yc.examples package, no samples folder."""
        self.assertFalse(os.path.exists(os.path.join(ROOT, "python", "yc", "examples")))
        self.assertFalse(os.path.exists(os.path.join(ROOT, "samples")))


class ProjectFolderTest(unittest.TestCase):
    def test_every_folder_is_a_project_listed_in_the_readme(self):
        with open(os.path.join(EXAMPLES, "README.md"), encoding="utf-8") as f:
            readme = f.read()
        self.assertGreaterEqual(len(screens()), 24)
        for name in folders():
            d = os.path.join(EXAMPLES, name)
            self.assertTrue(os.path.isfile(os.path.join(d, "main.py")), name)
            self.assertTrue(os.path.isfile(os.path.join(d, "ycgrid.toml")), name)
            ycg = [f for f in os.listdir(d) if f.endswith(".ycg")]
            self.assertEqual(len(ycg), 1, name)
            if name not in PROJECTS:
                self.assertEqual(ycg, [name + ".ycg"], name)
            with open(os.path.join(d, ycg[0]), "rb") as f:
                self.assertIn(f.read(4), (b"YCG1", b"YCGE"), name)
            self.assertIn(f"[{name}]({name}/main.py)", readme, name)
        for name in re.findall(r"\| \[(\w+)\]\(\w+/main\.py\)", readme):
            self.assertIn(name, folders(), "the README lists a folder that isn't there")

    def test_each_screen_builds_on_open(self):
        """Each screen's main.py has run() and a Workbook_Open that calls it, and a docstring that says what it is."""
        import ast

        for name in screens():
            with open(os.path.join(EXAMPLES, name, "main.py"), encoding="utf-8") as f:
                tree = ast.parse(f.read())
            fns = {n.name: n for n in tree.body if isinstance(n, ast.FunctionDef)}
            self.assertIn("run", fns, name)
            self.assertIn("Workbook_Open", fns, name)
            calls = [n.func.id for n in ast.walk(fns["Workbook_Open"]) if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)]
            self.assertIn("run", calls, name)
            self.assertTrue(ast.get_docstring(tree), name)

    def test_the_shared_lib_is_a_project_path_not_a_package(self):
        """[python] paths = ["../lib"] puts examples/lib on the project's imports: its modules (ex_util...) are the
        project's own, never offered as missing packages, and the packages every example needs come with YCGrid."""
        from yc import env

        for name in folders():
            d = os.path.join(EXAMPLES, name)
            uses_lib = os.path.normcase(LIB) in [os.path.normcase(p) for p in env.project_paths(d)]
            if name != "demo":
                self.assertTrue(uses_lib, name)
            own = env._project_modules(d)
            third = {m for m in env.scan_imports(d) if m not in sys.stdlib_module_names and m not in own and not m.startswith("_")}
            self.assertLessEqual(third, {"numpy", "pandas"}, name)
            if np is not None:
                self.assertEqual(env.find_missing(d), [], name)

    @unittest.skipIf(np is None, "numpy / pandas not installed")
    def test_screens_import_with_the_shared_lib(self):
        for name in screens():
            mod = load(name)
            self.assertTrue(callable(getattr(mod, "run", None)), name)


class ProjectPathsTest(unittest.TestCase):
    """[python] paths in ycgrid.toml: folders of shared code a project imports as if they were its lib\\."""

    def project(self, toml):
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "shared"))
        os.makedirs(os.path.join(d, "app"))
        with open(os.path.join(d, "shared", "helper_mod.py"), "w", encoding="utf-8") as f:
            f.write("import json\nVALUE = 1\n")
        with open(os.path.join(d, "app", "main.py"), "w", encoding="utf-8") as f:
            f.write("import helper_mod\n")
        with open(os.path.join(d, "app", "ycgrid.toml"), "w", encoding="utf-8") as f:
            f.write(toml)
        return d

    def test_paths_resolve_relative_to_the_project(self):
        from yc import env

        d = self.project('[python]\npaths = ["../shared", "../missing", "../shared"]\n')
        app = os.path.join(d, "app")
        self.assertEqual(env.project_paths(app), [os.path.join(d, "shared")])  # missing folders and repeats left out
        self.assertIn("helper_mod", env._project_modules(app))
        self.assertIn("json", env.scan_imports(app))  # the shared code's imports count for package detection
        self.assertEqual(env.find_missing(app), [])
        d2 = self.project("[python]\n")
        self.assertEqual(env.project_paths(os.path.join(d2, "app")), [])
        self.assertEqual(env.find_missing(os.path.join(d2, "app")), ["helper-mod"])  # without paths it is unknown

    def test_only_the_shared_modules_the_project_imports_count(self):
        """A shared folder holds code for many projects: only the modules this project imports (and what they import)
        say which packages it needs, or every example was offered numpy because one module beside its own needs it."""
        from yc import env

        d = self.project('[python]\npaths = ["../shared"]\n')
        with open(os.path.join(d, "shared", "heavy_mod.py"), "w", encoding="utf-8") as f:
            f.write("import yc_test_heavy_pkg\n")
        with open(os.path.join(d, "shared", "helper_mod.py"), "a", encoding="utf-8") as f:
            f.write("import second_mod\n")
        with open(os.path.join(d, "shared", "second_mod.py"), "w", encoding="utf-8") as f:
            f.write("import yc_test_second_pkg\n")
        app = os.path.join(d, "app")
        found = env.scan_imports(app)
        self.assertIn("yc_test_second_pkg", found)  # imported through helper_mod, two steps away
        self.assertNotIn("yc_test_heavy_pkg", found)  # beside it, never imported
        self.assertEqual(env.find_missing(app), ["yc-test-second-pkg"])

    def test_pack_puts_the_shared_code_into_lib(self):
        from yc import cli

        d = self.project('[python]\npaths = ["../shared"]\n')
        app = os.path.join(d, "app")
        os.makedirs(os.path.join(app, "lib"))
        with open(os.path.join(app, "lib", "helper_mod.py"), "w", encoding="utf-8") as f:
            f.write("VALUE = 2\n")  # the project's own copy wins
        with open(os.path.join(d, "shared", "other_mod.py"), "w", encoding="utf-8") as f:
            f.write("X = 3\n")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cli.main(["pack", app, "--no-test"]), 0)
        z = os.path.join(app, "dist", os.listdir(os.path.join(app, "dist"))[0])
        with zipfile.ZipFile(z) as zf:
            names = zf.namelist()
            self.assertIn("app/lib/other_mod.py", names)
            self.assertEqual(names.count("app/lib/helper_mod.py"), 1)
            self.assertEqual(zf.read("app/lib/helper_mod.py").strip(), b"VALUE = 2")

    def test_devenv_tells_pylance_about_the_paths(self):
        import json

        from yc import scaffold

        d = self.project('[python]\npaths = ["../shared"]\n')
        app = os.path.join(d, "app")
        for p in ("pyrightconfig.json", os.path.join(".vscode", "settings.json")):
            os.makedirs(os.path.dirname(os.path.join(app, p)), exist_ok=True)
            with open(os.path.join(app, p), "w", encoding="utf-8") as f:
                f.write("{}")
        scaffold.ensure_dev_env(app)
        with open(os.path.join(app, "pyrightconfig.json"), encoding="utf-8") as f:
            self.assertIn("../shared", json.load(f)["extraPaths"])
        with open(os.path.join(app, ".vscode", "settings.json"), encoding="utf-8") as f:
            self.assertIn("${workspaceFolder}/../shared", json.load(f)["python.analysis.extraPaths"])


@unittest.skipIf(np is None, "numpy / pandas not installed")
class BacktestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if LIB not in sys.path:
            sys.path.insert(0, LIB)

    def test_strategy_and_stats(self):
        import ex_math as quant
        from ex_backtest import crossover, positions_backtest, simulate_prices

        prices = simulate_prices(days=300, seed=1)
        self.assertTrue((prices["High"] >= prices[["Open", "Close"]].max(axis=1)).all())
        equity, trades = positions_backtest(prices, crossover(prices, 10, 30))
        self.assertEqual(len(equity), 300)
        self.assertTrue(set(trades["Side"]) <= {"Buy", "Sell"})
        st = quant.perf_stats(equity, trades=trades["PnL"])
        for k in ("Total Return", "CAGR", "Sharpe", "Max Drawdown", "Win Rate", "Trades"):
            self.assertIn(k, st)
        self.assertLessEqual(st["Max Drawdown"], 0)
        self.assertAlmostEqual(st["Total Return"], equity.iloc[-1] / equity.iloc[0] - 1)

    def test_drawdown_and_monthly(self):
        import ex_math as quant

        e = pd.Series([100.0, 110, 99, 121, 121], index=pd.date_range("2025-01-30", periods=5, freq="D"))
        dd = quant.drawdown(e)
        self.assertAlmostEqual(dd.iloc[2], 99 / 110 - 1)
        self.assertEqual(dd.iloc[3], 0)
        m = quant.monthly_returns(e)
        self.assertAlmostEqual(m.loc[2025, 1], 110 / 100 - 1)
        self.assertAlmostEqual(m.loc[2025, 2], 121 / 110 - 1)
        self.assertAlmostEqual(m.loc[2025, "Year"], 0.21)
        t = quant.trade_stats([10, -5, 20, None, -5])
        self.assertEqual(t["Trades"], 4)
        self.assertAlmostEqual(t["Win Rate"], 0.5)
        self.assertAlmostEqual(t["Profit Factor"], 3.0)
        self.assertTrue(math.isnan(quant.trade_stats([])["Win Rate"]))


class NotebookTest(unittest.TestCase):
    """yc.notebook: the cells workbooks kept from the former Notebook window."""

    def test_kept_cells_and_export(self):
        from yc import notebook

        class Fake:
            def __init__(self):
                self.sent = []

            def request(self, method, params=None):
                self.sent.append((method, params))
                return [{"code": "1 + 1"}] if method == "notebook.cells" else 1

        fake = Fake()
        saved = notebook._conn
        notebook._conn = lambda: fake
        try:
            self.assertEqual(notebook.cells(), [{"code": "1 + 1"}])
            path = notebook.export_ipynb("x.ipynb")
            self.assertEqual(fake.sent[-1], ("notebook.export", {"path": path}))
        finally:
            notebook._conn = saved


class CliTest(unittest.TestCase):
    """python -m yc ...: the commands YCGrid itself runs (Alt+F11 creates the project with `init`)."""

    def test_init_and_devenv(self):
        from yc import cli

        d = os.path.join(tempfile.mkdtemp(), "proj")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cli.main(["init", d, "--sheet", "Sheet1=Sheet1"]), 0)
            self.assertTrue(os.path.exists(os.path.join(d, "main.py")))
            self.assertEqual(cli.main(["devenv", d, "--sheet", "Sheet1=Sheet1"]), 0)
        with self.assertRaises(SystemExit):  # every command parses (a broken parser stops all of them)
            with contextlib.redirect_stdout(io.StringIO()):
                cli.main(["--help"])
