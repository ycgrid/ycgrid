# ADR 0011: The notebook is removed

- Status: accepted (supersedes [ADR 0010](0010-notebook-cells-in-the-workbook.md); its `NBOK` chunk stays)
- Related: [ROADMAP.md](../ROADMAP.md) phase 6, PLAN §13.2 and §13.12 (the AI development tool chain), hard rule 21,
  `apps/ycgrid/notebook_ui.cpp`, `python/yc/notebook.py`, `python/yc/debug.py`

## Context

The Notebook window (ADR 0010) gave YCGrid code cells beside the sheet. It sat between better tools: VS Code has
notebooks, an editor, debugging and the agents YCGrid's users work with. A VS Code kernel that ran cells inside YCGrid's
host replaced the window for a while, but what it added over the tools YCGrid already has was small, and it cost a
dependency download (ipykernel), a kernel spec in the user's Jupyter folder that points at one YCGrid installation,
kernel selection, an extension and a split screen (text in VS Code, figures in YCGrid). YCGrid builds in what is fast,
practical and missing elsewhere; a notebook is none of those.

What the notebook was used for is covered without it:

| Need | Without a notebook |
|---|---|
| Look at or change the running state | the Immediate window (IntelliSense, the host's namespace) |
| Stop in a running strategy and inspect it | Developer > Debug in VS Code (debugpy attached to the host) |
| Change code without losing state | saving a project file swaps its functions in the running host |
| Research: load data, analyze, plot, write to sheets | any Python with `yc.connect()` (scripts, VS Code's `# %%` cells, any Jupyter) |
| An agent operating YCGrid | the MCP tools and the skill |

## Decision

1. **No notebook in YCGrid**: no window, no command, no kernel (`yc.kernel` and `python -m yc kernel` are gone), and
   the host's exec job has no notebook options (value previews, pictures, cell tracebacks, interrupt).
2. **The `NBOK` chunk stays** (hard rule 21): workbooks keep their old cells unchanged and write them back;
   `yc.notebook.cells()` reads them and `yc.notebook.export_ipynb(path)` writes them to a plain `.ipynb`.
3. **Debugging and reloading stay**: Developer > Debug in VS Code and hot reload serve the VS Code workflow directly.

## Consequences

- Nothing is installed into the user's Jupyter folder; YCGrid's runtime doesn't need ipykernel.
- People who like notebooks use their own Jupyter with `import yc; yc.connect()`: a client of the running YCGrid like
  any script.
