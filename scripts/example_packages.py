# SPDX-License-Identifier: Apache-2.0
r"""Every example's missing-packages flow, button by button, in a YCGrid that has never installed numpy or pandas.

    <YCGrid>\runtime\python\python.exe -E -s scripts\example_packages.py [--dist dist_release] [name ...]

It copies --dist to a temporary folder without numpy and pandas (a first run's runtime) and, for every example in its
examples folder, opened as a double-click opens it (the Trust Center deciding; the examples folder is trusted), with
the packages removed again before each step:
  skip    the bar lists what the example needs; Skip: the bar goes, the example isn't built, the Error Log names it
  retry   Install while the package index can't be reached: the bar comes back with the reason and Try Again; the
          index back, Try Again installs them, the host restarts and the example builds
  always  Always install: installed, built, its ycgrid.toml says always
  auto    removed again and opened: installed and built with no click
An example that needs no package (the demo) must build with no bar. Installs come from PyPI (the network is needed;
uv's cache makes the later ones fast). One line a step; exit code 1 when a step failed.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("names", nargs="*")
ap.add_argument("--dist", default=os.path.join(ROOT, "dist_release"))
a = ap.parse_args()
dist = os.path.abspath(a.dist)
only = a.names
fresh = os.path.join(tempfile.mkdtemp(prefix="yc_expkg_"), "YCGrid")
NOT_YET = {"numpy", "pandas", "numpy.libs", "pandas.libs"}


def _skip(folder, names):
    return [n for n in names if n in ("__pycache__", "_uvcache", "ycgrid.pdb") or n.split("-")[0] in NOT_YET]


shutil.copytree(dist, fresh, ignore=_skip)
src = os.path.join(dist, "examples")
EXE = os.path.join(fresh, "ycgrid.exe")
PY = os.path.join(fresh, "runtime", "python", "python.exe")
UV = os.path.join(fresh, "runtime", "uv.exe")
EX = os.path.join(fresh, "examples")
sys.path.insert(0, os.path.join(fresh, "python"))
import yc  # noqa: E402
from yc import excel, ui  # noqa: E402

BAR = ("Install", "Always install", "Skip", "Try Again", "Enable Content")
BAD_INDEX = 'index_url = "http://127.0.0.1:9/simple"\n'
results = []


def runtime_busy():
    """A Python of the fresh runtime still running (a closed YCGrid's host exiting holds numpy's .pyd files)."""
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        f"(Get-Process python, ycgrid -ErrorAction SilentlyContinue | Where-Object {{ $_.Path -like '{fresh}*' }}).Count"],
                       capture_output=True, text=True)
    return (r.stdout.strip() or "0") != "0"


def remove_packages():
    end = time.time() + 30
    while runtime_busy() and time.time() < end:
        time.sleep(0.5)
    subprocess.run([UV, "pip", "uninstall", "--python", PY, "numpy", "pandas"], capture_output=True)
    sp = os.path.join(fresh, "runtime", "python", "Lib", "site-packages")
    for f in os.listdir(sp):
        if f.split("-")[0] in ("numpy", "pandas", "numpy.libs", "pandas.libs"):
            shutil.rmtree(os.path.join(sp, f), ignore_errors=True)
    r = subprocess.run([PY, "-E", "-s", "-c", "import importlib.util as u; print(bool(u.find_spec('numpy') or u.find_spec('pandas')))"],
                       capture_output=True, text=True)
    assert r.stdout.strip() == "False", r.stdout + r.stderr


def reset(name):
    """The example folder as it ships (its toml and requirements put back) and lib."""
    shutil.rmtree(os.path.join(EX, name), ignore_errors=True)
    shutil.copytree(os.path.join(src, name), os.path.join(EX, name), ignore=shutil.ignore_patterns("__pycache__", ".ycgrid"))
    shutil.rmtree(os.path.join(EX, "lib"), ignore_errors=True)
    shutil.copytree(os.path.join(src, "lib"), os.path.join(EX, "lib"), ignore=shutil.ignore_patterns("__pycache__"))


def toml(name):
    return os.path.join(EX, name, "ycgrid.toml")


class Session:
    def __init__(self, name):
        d = os.path.join(EX, name)
        ycg = next(f for f in os.listdir(d) if f.endswith(".ycg"))
        self.p = yc.launch(os.path.join(d, ycg), hidden=True, exe=EXE, lang="en", automation_security=2)
        self.conn = yc.connect(target=self.p.pid)

    def close(self):
        self.p.terminate()
        self.p.wait(10)
        time.sleep(0.5)

    def status(self):
        return self.conn.request("python.status", {})

    def bar(self):
        return {i["label"]: i for i in ui.items() if i.get("label") in BAR}

    def click(self, label):
        it = self.bar()[label]
        x, y = (it["rect"][0] + it["rect"][2]) / 2, (it["rect"][1] + it["rect"][3]) / 2
        self.conn.request("ui.input", {"script": f"m:{x:.0f},{y:.0f}|w:3|c:{x:.0f},{y:.0f}|w:5|m:2,2", "timeout_ms": 5000}, timeout=15)

    def built(self):
        try:
            return [s.Name for s in excel.Worksheets] != ["Sheet1"] or bool(ui.state().get("figures"))
        except Exception:  # noqa: BLE001 - the host restarting
            return False

    def errors(self):
        return [e.get("title", "") for e in ui.errors() if e.get("error")]

    @staticmethod
    def wait(check, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if check():
                    return True
            except Exception:  # noqa: BLE001
                pass
            time.sleep(0.5)
        return False

    def wait_bar(self, label, timeout=60):
        return self.wait(lambda: label in self.bar(), timeout)


def step(name, what, fn):
    t = time.time()
    s = None
    try:
        s = Session(name)
        ok, note = fn(s)
    except Exception as e:  # noqa: BLE001
        ok, note = False, f"{type(e).__name__}: {e}"
    finally:
        if s is not None:
            s.close()
    results.append((name, what, ok))
    print(f"{'OK  ' if ok else 'FAIL'} {name:20} {what:7} {time.time() - t:5.1f}s {note}", flush=True)


def missing_shown(s):
    if not s.wait_bar("Install", 60):
        return None
    return s.status()["missing_packages"]


def do_skip(s):
    miss = missing_shown(s)
    if not miss or "numpy" not in miss and "pandas" not in miss:
        return False, f"bar: {miss}"
    s.click("Skip")
    gone = s.wait(lambda: not s.bar(), 10)
    time.sleep(1)
    logged = any("No module named" in e.get("title", "") for e in ui.errors())
    return gone and not s.built() and logged, f"missing={miss} bar_gone={gone} built={s.built()} logged={logged}"


def do_retry(s):
    miss = missing_shown(s)
    if not miss:
        return False, "no bar"
    s.click("Install")
    failed = s.wait(lambda: s.status()["install_error"] and "Try Again" in s.bar(), 240)
    reason = s.status()["install_error"][:90]
    if not failed:
        return False, f"no failure shown: {s.status()}"
    with open(toml(s_name[0]), encoding="utf-8") as f:
        t = f.read()
    with open(toml(s_name[0]), "w", encoding="utf-8") as f:
        f.write(t.replace(BAD_INDEX, ""))  # the network is back
    s.click("Try Again")
    built = s.wait(lambda: s.built() and not s.status()["missing_packages"] and not s.status()["installing"], 240)
    st = s.status()
    return built and not st["missing_packages"] and not st["install_error"], f"reason='{reason}' built={built} missing={st['missing_packages']}"


def do_always(s):
    miss = missing_shown(s)
    if not miss:
        return False, "no bar"
    s.click("Always install")
    built = s.wait(lambda: s.built() and not s.status()["missing_packages"] and not s.status()["installing"], 240)
    with open(toml(s_name[0]), encoding="utf-8") as f:
        always = 'auto_install = "always"' in f.read()
    return built and always, f"built={built} toml_always={always}"


def do_auto(s):
    built = s.wait(lambda: s.built() and not s.status()["missing_packages"] and not s.status()["installing"], 240)  # no click
    return built and not s.status()["missing_packages"], f"built={built} clicked=nothing"


def do_none(s):
    built = s.wait(s.built, 60)
    return built and not s.bar() and not s.errors(), f"built={built} bar={list(s.bar())} errors={s.errors()[:2]}"


s_name = [""]
names = only or sorted(d for d in os.listdir(src) if d != "lib" and os.path.isdir(os.path.join(src, d)))
for name in names:
    s_name[0] = name
    reset(name)
    remove_packages()
    needs = None
    # does it need a package at all? (the demo needs none)
    probe = Session(name)
    try:
        Session.wait(lambda: probe.status()["workbook_open_done"] and (probe.built() or probe.status()["missing_packages"]), 60)
        needs = probe.status()["missing_packages"]
    finally:
        probe.close()
    if not needs:
        step(name, "nopkg", do_none)
        continue
    reset(name)
    step(name, "skip", do_skip)
    reset(name)
    with open(toml(name), encoding="utf-8") as f:
        t = f.read()
    assert "[python]\n" in t, toml(name)
    with open(toml(name), "w", encoding="utf-8") as f:  # in [python], not after a [[python.feeds]] table
        f.write(t.replace("[python]\n", "[python]\n" + BAD_INDEX, 1))
    remove_packages()
    step(name, "retry", do_retry)
    reset(name)
    remove_packages()
    step(name, "always", do_always)
    remove_packages()
    step(name, "auto", do_auto)
    reset(name)
fails = [r for r in results if not r[2]]
print(f"{len(results)} steps, {len(fails)} failed: {fails}")
shutil.rmtree(os.path.dirname(fresh), ignore_errors=True)
sys.exit(1 if fails else 0)
