# ADR 0009: A pip wheel next to the portable folder, with the host on the installing Python

- Status: accepted
- Related: [ROADMAP.md](../ROADMAP.md) Phase 5, PLAN §13 (d) (the Python interpreter), §13.15 (portable folder),
  `tools/make_wheel.py`, `scripts/build_wheel.cmd`, `PythonBridge::FindPython`

## Context

PLAN §13.15 ships YCGrid as one portable folder with its own Python 3.12, and §13 (d) says YCGrid never falls back to
the system Python: a project's packages must not depend on what happens to be installed on the machine.

Many researchers live in their own Python (a conda or venv with their libraries) and expect `pip install`. Asking them
to copy their environment into a portable runtime is the friction the roadmap wants to remove.

## Decision

1. **Both distributions.** The portable folder stays the main one. `scripts\build_wheel.cmd` also builds a
   `py3-none-win_amd64` wheel: the `yc` package with `ycgrid.exe` in `yc/_bin` (and the skill). The wheel has an
   optional `[data]` extra (numpy, pandas); `yc` itself needs only the standard library.
2. **The installing Python runs the host.** A YCGrid started from the wheel (`ycgrid`, `python -m yc open / run /
   examples`, `yc.launch()`) gets `YCGRID_PYTHON` set to the Python that started it; `FindPython` honours it, and an
   exe that lives in `site-packages\yc\_bin` falls back to the Python of that installation. These are explicit
   choices of the user, so §13 (d) still holds: YCGrid never picks some other Python it finds on the system.
3. **The package is found next to the exe.** `FindYcPackage` also accepts `site-packages\yc` (the exe in `yc\_bin`).
4. **Headless instances.** `yc.launch(hidden=True)` (`ycgrid.exe --hidden`) starts a YCGrid without a window for tests
   and agents; it keeps no AutoRecover copies, never receives files forwarded by another launch, and is checked with
   `yc.ui.state()` and `yc.ui.screenshot()`.

## Consequences

- Projects opened from the portable folder keep their bundled Python; a wheel user's projects use their environment,
  so `ycgrid.toml [python].interpreter` and requirements files still decide when a project needs something specific.
- The wheel is large (the exe), and is not uploaded to PyPI by this repository's scripts.
