# ADR 0010: Notebook cells are saved in the workbook; their code runs in the project's host

- Status: superseded by [ADR 0011](0011-the-notebook-is-removed.md) (the Notebook window was removed; the `NBOK`
  chunk stays)
- Related: [ROADMAP.md](../ROADMAP.md) (additions: Notebook), PLAN §13.2 (project folders and modules), §13.15
  (portable folders), hard rules 1, 11, 15 and 21, `apps/ycgrid/notebook_ui.cpp`, `python/yc/notebook.py`,
  the `NBOK` chunk in [ycg-format.md](../ycg-format.md)

## Context

PLAN §13.2 keeps a project's Python code in the project folder (`main.py`, `modules/*.py`, `sheets/*.py`): plain files
that version control, editors and code review handle, and that make the folder portable (hard rule 15).

Research works differently: short steps run one at a time, looked at, changed and run again, with the results next to
the code. Jupyter does this, but outside the spreadsheet, slowly, and with plots that can't keep up with market data.
The Notebook window brings that workflow into YCGrid.

## Decision

1. **The cells' code is saved in the workbook**, in a new `NBOK` chunk (`preserve_if_unknown`, `{1: [code of each
   cell]}`), not as project files. A notebook is part of the analysis in that workbook, like its formulas; a workbook
   without a project folder (most research) still keeps its notebook, and opening the file brings it back. Outputs are
   not saved: they are recomputed by running the cells.
2. **The cells run in the project's Python host**, in the Immediate window's namespace, as `exec` jobs. They never run
   inside ycgrid.exe (hard rule 11), and the UI thread never waits for them: each run is a host job on its own thread,
   and the window polls for the result (hard rule 1). A workbook without a project starts the host in a scratch folder.
3. **Code that becomes a screen moves to the project.** Templates, event handlers, UDFs and feeds stay in project files
   (§13.2); the notebook is for research. `yc_notebook` lets an agent write and run cells, and nothing stops a cell from
   importing the project's modules.

## Consequences

- Old readers keep the chunk unread and write it back (`preserve_if_unknown`); readers without notebook support open
  the workbook unchanged (hard rule 21). A round-trip test is in `tests/unit/test_bind.cpp`.
- Notebook code is inside a binary file: it doesn't diff like a `.py` file. Copy cells into `modules/` when they should
  be reviewed or shared as code.
- A cell can run for a long time; Interrupt raises KeyboardInterrupt in it. Replacing cells or opening another workbook
  while one runs doesn't wait for it.
