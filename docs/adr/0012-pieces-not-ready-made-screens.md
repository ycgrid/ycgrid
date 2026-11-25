# ADR 0012: YCGrid builds in the pieces, not ready-made screens or research libraries

- Status: accepted
- Related: [ROADMAP.md](../ROADMAP.md) phases 3, 5 and 6, [ADR 0011](0011-the-notebook-is-removed.md), PLAN §13.12 (the
  AI development tool chain), `examples/` (then `python/yc/examples`), `skill/ycgrid-python/references/screens.md`

## Context

Earlier phases added ready-made trading screens (`yc.templates`: option chain, price ladder, positions, order ticket,
blotter, risk limits, strategy console, backtest report) and research libraries (`yc.ta`, `yc.quant`, `yc.factor`,
`yc.portfolio`, `yc.risk`, `yc.options`, `yc.backtest`, `yc.live`, `yc.algo`, `yc.session`), plus `yc.playback`,
`yc.plugins` and `.ycapp` packages. YCGrid's users work in VS Code with an agent. For them these are second-tier
features: an agent writes them in minutes on top of YCGrid's pieces, numpy and pandas, or the user takes a package
(statsmodels, scipy, pandas-ta, vectorbt...) they already trust. Built in, they add nothing only YCGrid has, they fix
one way of laying out a screen, and they are code to keep correct and documented. What only YCGrid has are the pieces:
live tables through shared memory, fast plots with events and handles, click actions, alerts, the safety layer, the
order manager with risk checks, the simulator, record and replay.

## Decision

1. **Removed**: `yc.templates`, `yc.ta`, `yc.quant`, `yc.factor`, `yc.portfolio`, `yc.risk`, `yc.options`,
   `yc.backtest`, `yc.live`, `yc.algo`, `yc.session`, `yc.playback`, `yc.plugins`, `yc.apps` and `.ycapp` (with
   Developer > Install App and `python -m yc app`), and the test group on the View tab (its commands stay for tests
   and Python).
2. **The examples show how**: every example that used them was rewritten with the pieces and numpy / pandas; the
   maths several examples share are in `examples/_math.py`, the blotter and positions tables in `examples/_util.py`,
   the backtest report in `examples/backtest_report.py`. They are example code to copy, not an API.
   `examples/txo_market_maker` (then `samples/`) was rebuilt the same way and now trades through an OrderManager.
3. **The order manager stays and opens up**: `yc.trading.OrderManager` keeps the risk checks, replay refusal, operator
   mode and the change log; screens follow it through `on_order` / `on_fill` instead of template parameters.
   `yc.bars`, `yc.sim`, `yc.feeds` and `trading.check_gateway` stay: they are pieces.
4. **`skill/ycgrid-python/references/screens.md`** replaces `templates.md`: the pieces and a recipe per screen, each
   pointing at the example to read.
5. **Four Excel features go too**, for the same reason: Get & Transform (Power Query, its editor and the M
   interpreter; pandas and database drivers do this in Python), the Data Model with its DAX measures and the seven
   `CUBE*` functions, co-authoring through a shared folder, and reading Excel 97-2003 `.xls` files (save them as
   `.xlsx` in Excel). Their `.ycg` chunks (`QRYS`, `DMOD`, `COAU`) are no longer known: they load as preserved
   unknown chunks and are written back unchanged (hard rule 21; a Catch2 round-trip test). A PivotTable on the Data
   Model opens with its last values and says it can't be refreshed. `Workbook.CoAuthoring` / `UserStatus` answer for
   one local user; `Workbook.Queries`, `Connections`, `Model` and `PivotTable.CubeFields` are gone; values `CUBE*`
   formulas left in cells still show their captions.

## Consequences

- Code calling the removed modules must be changed; nothing in a `.ycg` refers to them (they wrote ordinary cells,
  tables and formats), so files open unchanged. `CUBE*` formulas become unknown names (#NAME?) when recalculated.
- The simulator prices its option chain with its own Black-Scholes functions (private to `yc.sim`).
- Agents are pointed at the pieces and the examples instead of a fixed set of screens.

## Addendum (2026-11-02)

Reconfirmed after the Excel parity rounds removed every other difference from Excel's UI: the features of point 5 stay
out. Their places in the UI (Data > Get Data's Transform Data, Create PivotTable's "Add this data to the Data Model")
are shown disabled with the reason, as Excel shows a feature that isn't available. They are listed under "Not planned"
in [excel-parity-backlog.md](../excel-parity-backlog.md).
