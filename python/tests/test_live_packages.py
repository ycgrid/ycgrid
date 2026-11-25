# SPDX-License-Identifier: Apache-2.0
"""The missing-packages bar, button by button, on a copy of a real example (examples/backtest_report with its
examples/lib) that also imports a package the runtime doesn't have: Install and Always install install it and the
example builds its screen; an install that fails brings the bar back with the reason, Try Again and Skip; Skip leaves
the example unloaded with the reason in the Error Log. Needs a built ycgrid.exe; the package installed is a local
wheel (find_links), uninstalled again afterwards."""
import base64
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.environ.get("YCGRID_EXE") or next(
    (p for p in (os.path.join(ROOT, d, "ycgrid.exe") for d in ("dist_release", "dist_debug")) if os.path.exists(p)), None)
PKG = "ycprobe_pkg"  # the package the test makes, installs and removes again
DIST_NAME = "ycprobe-pkg"


def make_wheel(folder):
    files = {f"{PKG}/__init__.py": b"VALUE = 42\n",
             f"{PKG}-1.0.dist-info/METADATA": f"Metadata-Version: 2.1\nName: {DIST_NAME}\nVersion: 1.0\n".encode(),
             f"{PKG}-1.0.dist-info/WHEEL": b"Wheel-Version: 1.0\nGenerator: yc-test\nRoot-Is-Purelib: true\nTag: py3-none-any\n"}
    record = ""
    for n, b in files.items():
        h = base64.urlsafe_b64encode(hashlib.sha256(b).digest()).rstrip(b"=").decode()
        record += f"{n},sha256={h},{len(b)}\n"
    record += f"{PKG}-1.0.dist-info/RECORD,,\n"
    with zipfile.ZipFile(os.path.join(folder, f"{PKG}-1.0-py3-none-any.whl"), "w") as z:
        for n, b in files.items():
            z.writestr(n, b)
        z.writestr(f"{PKG}-1.0.dist-info/RECORD", record)


def make_example(root, package, feed=False):
    """root/examples/{lib, probe} (backtest_report importing `package` too) and root/wheels; returns the .ycg."""
    ex = os.path.join(root, "examples")
    shutil.copytree(os.path.join(ROOT, "examples", "lib"), os.path.join(ex, "lib"), ignore=shutil.ignore_patterns("__pycache__"))
    d = os.path.join(ex, "probe")
    shutil.copytree(os.path.join(ROOT, "examples", "backtest_report"), d, ignore=shutil.ignore_patterns("__pycache__"))
    os.rename(os.path.join(d, "backtest_report.ycg"), os.path.join(d, "probe.ycg"))
    with open(os.path.join(d, "main.py"), encoding="utf-8") as f:
        src = f.read()
    with open(os.path.join(d, "main.py"), "w", encoding="utf-8") as f:
        f.write(src.replace("from __future__ import annotations\n", f"from __future__ import annotations\n\nimport {package}  # noqa: F401\n", 1))
    wheels = os.path.join(root, "wheels")
    os.makedirs(wheels)
    make_wheel(wheels)
    with open(os.path.join(d, "ycgrid.toml"), "a", encoding="utf-8") as f:
        f.write(f'find_links = "{wheels.replace(os.sep, "/")}"\n')
    if feed:  # a data feed, as txo_market_maker has: the restart after an install stops it first
        os.makedirs(os.path.join(d, "feeds"))
        with open(os.path.join(d, "feeds", "idle.py"), "w", encoding="utf-8") as f:
            f.write("import time\nwhile True:\n    time.sleep(1)\n")
        with open(os.path.join(d, "ycgrid.toml"), "a", encoding="utf-8") as f:
            f.write('\n[[python.feeds]]\nname = "idle"\nscript = "feeds/idle.py"\n')
    return os.path.join(d, "probe.ycg")


def runtime_python():
    return os.path.join(os.path.dirname(EXE), "runtime", "python", "python.exe")


def uninstall():
    py = runtime_python()
    uv = os.path.join(os.path.dirname(EXE), "runtime", "uv.exe")
    cmd = [uv, "pip", "uninstall", "--python", py, DIST_NAME] if os.path.exists(uv) else [py, "-m", "pip", "uninstall", "-y", DIST_NAME]
    subprocess.run(cmd, capture_output=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


@unittest.skipIf(EXE is None or os.environ.get("YC_NO_LIVE") == "1" or not os.path.exists(runtime_python() if EXE else ""),
                 "no built ycgrid.exe with its runtime")
class PackageBarTest(unittest.TestCase):
    def setUp(self):
        uninstall()
        self.dir = tempfile.mkdtemp(prefix="yc_pkgbar_")
        self.proc = None

    def tearDown(self):
        if self.proc is not None:
            self.proc.terminate()
            self.proc.wait(10)
        uninstall()
        shutil.rmtree(self.dir, ignore_errors=True)

    def open(self, package=PKG, feed=False):
        """Opens the probe example as a person does (the Trust Center asks: Enable Content first)."""
        ycg = make_example(self.dir, package, feed)
        self.proc = yc.launch(ycg, hidden=True, exe=EXE, lang="en", automation_security=2)
        self.conn = yc.connect(target=self.proc.pid)
        self.assertTrue(self.wait(lambda: "Enable Content" in self.bar(), 30), self.bar())
        self.click("Enable Content")
        self.assertTrue(self.wait(lambda: self.status()["missing_packages"] and "Install" in self.bar(), 60), self.status())
        self.assertEqual(self.status()["missing_packages"], [package.replace("_", "-")])  # never ex_util & co.
        self.assertFalse(self.built())

    def status(self):
        return self.conn.request("python.status", {})

    def bar(self):
        from yc import ui

        return {i["label"]: i for i in ui.items() if i.get("label") in ("Install", "Always install", "Skip", "Try Again", "Enable Content")}

    def click(self, label):
        it = self.bar()[label]
        x, y = (it["rect"][0] + it["rect"][2]) / 2, (it["rect"][1] + it["rect"][3]) / 2
        self.conn.request("ui.input", {"script": f"m:{x:.0f},{y:.0f}|w:3|c:{x:.0f},{y:.0f}|w:5|m:2,2", "timeout_ms": 5000}, timeout=15)

    def built(self):
        from yc import excel

        try:
            return [s.Name for s in excel.Worksheets] != ["Sheet1"]
        except Exception:  # noqa: BLE001 - the host restarting
            return False

    @staticmethod
    def wait(check, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if check():
                return True
            time.sleep(0.5)
        return bool(check())

    def test_install_installs_and_the_example_builds(self):
        from yc import ui

        self.open(feed=True)
        self.click("Install")
        self.assertTrue(self.wait(self.built, 180), self.status())
        # the host's exit to restart with the packages is no crash, also while the feeds are being stopped
        self.assertFalse([e.get("title") for e in ui.errors() if "host exited" in e.get("title", "")])
        st = self.status()
        self.assertEqual(st["missing_packages"], [])
        self.assertEqual(st["install_error"], "")
        self.assertFalse(st["installing"])
        self.assertEqual(self.bar(), {})
        with open(os.path.join(self.dir, "examples", "probe", "ycgrid.toml"), encoding="utf-8") as f:
            self.assertIn('auto_install = "ask"', f.read())

    def test_always_install_installs_and_remembers(self):
        self.open()
        self.click("Always install")
        self.assertTrue(self.wait(self.built, 180), self.status())
        with open(os.path.join(self.dir, "examples", "probe", "ycgrid.toml"), encoding="utf-8") as f:
            self.assertIn('auto_install = "always"', f.read())

    def test_a_failed_install_says_why_and_offers_try_again_and_skip(self):
        from yc import ui

        self.open("ycprobe_absent_pkg_zz")  # on no index and in no wheel folder
        self.click("Install")
        self.assertTrue(self.wait(lambda: self.status()["install_error"], 180), self.status())
        st = self.status()
        self.assertFalse(st["installing"])
        self.assertEqual(st["missing_packages"], ["ycprobe-absent-pkg-zz"])
        self.assertTrue(self.wait(lambda: {"Try Again", "Skip"} <= set(self.bar()), 10), self.bar())
        self.assertNotIn("Install", self.bar())
        self.assertTrue(any("Couldn't install the Python packages" in e.get("title", "") and e.get("error") for e in ui.errors()),
                        [e.get("title") for e in ui.errors()])
        self.click("Try Again")  # fails the same way, and says so again
        self.assertTrue(self.wait(lambda: "Try Again" in self.bar() and not self.status()["installing"], 180), self.status())
        self.click("Skip")
        self.assertTrue(self.wait(lambda: not self.bar(), 10), self.bar())
        self.assertFalse(self.built())

    def test_skip_leaves_the_example_unloaded_and_says_why(self):
        from yc import ui

        self.open()
        self.click("Skip")
        self.assertTrue(self.wait(lambda: not self.bar(), 10), self.bar())
        self.assertFalse(self.built())
        self.assertTrue(any("No module named 'ycprobe_pkg'" in e.get("title", "") for e in ui.errors()),
                        [e.get("title") for e in ui.errors()])


if __name__ == "__main__":
    unittest.main()
