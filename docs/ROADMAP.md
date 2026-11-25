# YCGrid roadmap: the trading and research platform

YCGrid is an Excel-grade spreadsheet that trading desks and quantitative researchers use as their main screen: data
processing, statistics, Python research and modelling, charts, live quote boards, strategy control and risk screens all
run in one place, driven by Python and agents. This document turns the product roadmap (2026-10-24) into work items
for this repository, records where it meets the existing design ([YCGRID_PLAN.md](YCGRID_PLAN.md)), and tracks status.

Status marks: **done** (shipped and tested), **partial** (some of it exists; the rest is listed), **new** (not started).

## Positioning and principles

- One platform from research to live trading: the same Python, the same screen, the same data path for replay and
  production.
- A spreadsheet close to Excel stays the base: formulas, tables, PivotTables, charts and Excel's object model are what
  people already know, and agents write them well. Trading features are added on top, never instead.
- Performance is decided by the data path between Python and the UI, so it comes first; then safety for live
  trading; then trading screens; then research and modelling; then the ecosystem (agents, packaging, open source).
- Every phase has an acceptance gate, measured with the benchmark tool (`scripts/bench.py`). A phase isn't finished
  until its gate passes.
- Agents are first-class users: every feature has a Python API, an MCP tool where it makes sense, a skill page and an
  example that runs as is.

## Where the roadmap meets the existing design

| Roadmap item | Existing design | Resolution |
|---|---|---|
| "Not doing: xlsx compatibility, copying Excel's formula engine" | `.xlsx` opens and a copy saves as `.xlsx` (ADR 0004); 513 Excel functions | Keep what exists. No further work on Excel file fidelity; formulas stay complete because they are part of the spreadsheet base. |
| "Not doing: general BI, report designer, drag-and-drop forms" | PivotTables, UserForms and form controls exist (the Data Model was removed, ADR 0012) | Keep them. New BI-style work only where trading research needs it (statistics, factor analysis, backtest reports). |
| "Python API aligned with Excel's macro naming" | The object model uses Excel's names (`Range.Value`, `Worksheet.Range`) | Unchanged: the documentation says "Excel's object model". |
| `sheet.bind(df, key=...)` (lower case) | Excel-style members are PascalCase; YCGrid additions are marked [YC] | Both: the Excel object model stays PascalCase; a pythonic layer for heavy users uses snake_case (`ws.bind`, `yc.data`, `yc.plot`). Both share one engine path (ADR 0007). |
| Shared memory + Arrow columnar transfer | PLAN §13.4 planned a shared-memory Topic table (not built); today all data goes through the YCP pipe as msgpack (≈1.2 M updates/s) | Build the shared-memory channel now, with Arrow's memory layout for columns and no Arrow library dependency (ADR 0007). The pipe stays the control channel. |
| Change flashes, live status | PLAN §8.12 (P2, not built) | Built in Phase 3 (flash and throttle per column). |
| Heartbeat, stale data | PLAN §13.9: bound cells keep their last values marked stale when the host disconnects (not built) | Built in Phase 2 (heartbeat sources, row freshness). |
| Multi-window, multi-monitor layouts saved in one file | Floating sheets, containers and panels are saved (PLAN §10.5) | Done; Phase 3 adds named layouts and templates. |
| Downsampling in C++ (LTTB) | Min/max per pixel column for long series (PLAN §9.3) | Keep min/max for lines (it preserves spikes, which traders need); add LTTB as an option and for scatter. |
| Replay through the same path as live | Topics and Python writes are the only live paths | Recording and replay write into the same bind / topic channel; a replay owns its tables and topics (ADR 0008). |
| `pip install ycgrid` | Portable folder with its own Python 3.12 (PLAN §13.15); never the system Python (§13 (d)) | Both: the portable folder stays the main distribution; a wheel carries `ycgrid.exe`, and its host runs on the Python that installed it, an explicit choice (ADR 0009). |
| Research plots | Excel charts saved in the workbook, fed by cells (PLAN §9) | Both: `yc.plot` figures are session windows fed by arrays, not saved; Excel charts stay the workbook's charts (ADR 0008). |
| Windows and Python only | Same | Same. |
| Notebook (research step by step) | Project code lives in project files (PLAN §13.2); people work in VS Code with an agent (§13.12) | Removed ([ADR 0011](adr/0011-the-notebook-is-removed.md)): the Immediate window, Debug in VS Code, hot reload and `yc.connect()` cover it; the former window's cells stay in `NBOK` ([ADR 0010](adr/0010-notebook-cells-in-the-workbook.md)) and `yc.notebook.export_ipynb()` writes them out. |
| Execution algorithms, research to live (the examples) | Every order goes through the OrderManager: risk limits, operator mode, refusal during replay, the change log (Phase 2) | They only submit through the OrderManager, so every safety check applies unchanged; nothing talks to a market directly. |

### Hard rules checked against the roadmap work (2026-10-25)

Every addition was reviewed against PLAN §0.2. Conflicts found and how they were resolved:

| Hard rule | Finding | Resolution |
|---|---|---|
| 1. The UI thread never waits or does O(size) work | The Switch Windows list read every instance file and enumerated windows on the UI thread twice a second (30 ms frames); the snapshot a frame dropped freed its replaced tiles on the UI thread (5-7 ms); the Notebook saved its cells with a synchronous engine call; big scatter plots scanned all points on the UI thread | The window list runs on a worker; old snapshots are released on a worker (bounded queue); the Notebook posts to the engine and runs cells on detached threads; scatter decimation runs on a worker from a copy of the data. Frame CPU p99 went from 33 ms to under 1 ms |
| 2 / 3. Single writer, readers read snapshots | Bind refresh caps, the notebook and alerts all write through the engine thread | No conflict |
| 4. Hot/cold split | The tile pool and block copies only touch value tiles | No conflict |
| 6. Every queue is bounded | New queues: snapshot release (64), live decision log (`keep_log`, 10,000), algo fills held during a submit (only while a submit is in flight), Run All (the cells) | Bounded as listed |
| 8. Core modules without Win32 | The tile pool (`model/`) uses only the standard library | No conflict |
| 11. Python outside the exe; bounded waits | Notebook runs are host jobs with a timeout; Interrupt is a notification handled on the host's reader thread | No conflict |
| 12. Excel's names; [YC] marked | `yc.notebook`, `yc.trading`, `yc.plot` additions are the snake_case research layer of ADR 0007, documented as [YC] modules | No conflict |
| 13. Single source for interfaces | `mcp/tools.json` updated (42 tools, docs generated); the `BIND`, `SAFE` and `NBOK` chunks were missing from ycg-format.md | Chunks documented |
| 15. Portable folders | Notebook code inside the `.ycg` instead of project files | ADR 0010 |
| 19 / 20. ImPlot on the UI thread; Python never draws | Heatmap extents, scatter workers and inline matplotlib pictures: workers only compute arrays; drawing stays on the UI thread; Python sends data | No conflict |
| 21. Backward-compatible files | `NBOK` (new chunk, preserve_if_unknown) and alert flag 16 (a new bit in an existing field) | Round-trip tests in `test_bind.cpp` and `test_safety.cpp` |

## Phase 1: the data binding foundation

The data path decides the platform's ceiling: a million live cells, tick to screen within a frame or two.

| Item | Status | Notes |
|---|---|---|
| Shared-memory transfer, columnar | done | A named file mapping per connection holds a ring of column frames (Arrow layout: values, validity, UTF-8 offsets); the pipe only carries "frame at offset N". Python writes it with `mmap`, no extension module. |
| DataFrame binding `ws.bind(df, key="strike")` | done | pandas, polars, pyarrow, numpy record arrays, dicts of arrays. The bound range is a table (ListObject), so formulas, filters, charts and PivotTables use it like any table. |
| Updates by key | done | Python sends only the changed keys and columns; the engine finds the rows (hash map, re-checked against the key column) and writes the cells; new keys append rows, deleted keys remove them. |
| Column formats declared once | done | Tick size, decimals, percent, basis points (`[mul=10000]`) and thousands separators are declared per column and applied by C++ to every row, including rows added later; flash settings and refresh caps per column (drawn since Phase 3); conditional formats use `FormatConditions` on the table's columns. |
| Excel-style API kept | done | `Range.Value`, `Range.BindTopics`, `=QUOTE()` share the engine with `bind`. |
| Benchmark tool | done | `scripts/bench.py`: bind updates on a million cells, tick-to-screen latency (send time to the frame that shows it), frame rate, engine time, memory over hours (`--hours`) and a JSON report; `scripts/stress_realtime.py` covers topics and block writes. Results in ADR 0007 and the README. |

**Gate**: a million cells under tick-by-tick updates; report updates per second, tick-to-screen latency (p50 / p99),
frame rate and memory growth over an 8-hour run. Targets: ≥ 5 M cell updates/s through bind, p99 tick-to-screen
≤ publish interval + 2 frames, 120 fps on a 120 Hz display, memory growth < 5% after warm-up.

**Gate status: met** (measured 2026-10-25 on the machine in [benchmark.md](benchmark.md), a hidden YCGrid window;
the 8-hour run is paced at 120 Hz, the earlier rows at 60 Hz; publish interval 16 ms; see benchmark.md for every row):

| Target | Result | Met |
|---|---|---|
| ≥ 5 M cell updates/s through bind | 5.5 M/s (1% of the rows, 500 updates/s) with p99 43 ms; up to ~43 M/s when saturated | yes |
| p99 ≤ publish interval + 2 frames (49 ms at 60 Hz, 33 ms at 120 Hz) | 39.7 ms at 1 M/s, 46.5 ms at 2 M/s, 47.3 ms at 11 M/s; 57 ms when each update is half the table (500,000 cells), 77 ms for the whole table | at 60 Hz up to ~11 M/s in small updates; at 120 Hz see the 8-hour row |
| p99 at 120 Hz, 8 hours (5.5 M cells/s, 1% of a million cells, 500 updates/s) | median p99 16.7 ms (p50 8.6 ms) over 476 minutes; 412 minutes (87%) within 33 ms. The 64 minutes above it all fall in the times builds and test runs used every core of the same machine; every quiet stretch (07:44–08:04, 11:36–12:30, 12:46–13:45, 14:52–15:28) stayed at or below 18.2 ms | yes, on a quiet machine |
| 120 fps on a 120 Hz display | 8 hours on a hidden window paced at 120 Hz: median 120.6 fps, never below 120 in quiet stretches (96 at worst during a build); frame CPU 0.45 ms on average | yes |
| memory growth < 5% after warm-up, 8 hours | 8 hours at 5.5 M cells/s: 2.6% growth after warm-up (0.7 MB per hour; private memory 291 -> 300 MB, 339 MB at most during a build) | yes |

The engine time per update was halved by pooled tile memory (an update of 10% of a million cells: 18 ms -> 10 ms),
which brought p99 at 2 M cells/s from 53 to 46 ms. What is left for the 120 Hz target is mostly the copy of the
touched 64 KB tiles after each publish (about 6 ms for 400 tiles) and frame pacing.

## Phase 2: safety for live trading

The biggest risk on a live screen isn't a slow frame but an old price shown as current, or a parameter changed by
mistake.

| Item | Status | Notes |
|---|---|---|
| Process heartbeat | done | Feeds, the Python host and any script register a heartbeat; when one stops (crash, hang, network), the whole window is tinted and a banner names the source. It can never look like normal updating. |
| Data freshness | done | Bound tables keep each row's last update time; an age column can be shown; rows older than the threshold turn gray. Topics carry their own time. |
| Parameter cells | done | Typed, range-checked cells (`Range.Parameter`); committing calls Python; dangerous parameters ask for confirmation (old → new shown). |
| Change log | done | Every parameter change, operator action and order-panel action is recorded (time, user, old, new, source) in the workbook and can be exported. |
| Read-only and operator modes | done | A workbook can open read-only; operator mode (optionally password- or PIN-gated, with an idle timeout) is needed to change parameters or press guarded buttons. |
| Kill switch | done | A guarded button and shortcut that calls the registered emergency handler (cancel all, flatten) and logs it. |

**Gate**: simulate Python crashing, Python hanging and the market data stopping; the UI warns within the configured
time in all three; every parameter change is in the log. Passed: a killed feed raised its heartbeat and stale-table
alarms within 2 s (timeouts 1 s), a hung host within 4 s (timeout 3 s), a crashed host at once; typed and Python
parameter changes, mode changes, the kill switch and every alarm's start and end were logged.

## Phase 3: trading screens

Built-in templates so an agent doesn't start from nothing each time. (Phase 6 replaced the templates with examples built
from the pieces, ADR 0012; `examples/txo_market_maker` was rebuilt the same way.)

| Item | Status | Notes |
|---|---|---|
| T-style option chain | done | Calls, strike, puts; contract-month switching (TXO / TXW and others); ATM highlight. Was `yc.templates.OptionChain`. Removed in phase 6 ([ADR 0012](adr/0012-pieces-not-ready-made-screens.md)): the examples build it from the pieces. |
| Price ladder (DOM) | done | Depth around the last price, own orders, click-to-trade hooks (simulated by default). Removed in phase 6 ([ADR 0012](adr/0012-pieces-not-ready-made-screens.md)): the `dom_ladder` example builds it from the pieces. |
| Positions with Greeks totals | done | Grouped by expiry and product with totals; uses table subtotals. Removed in phase 6 ([ADR 0012](adr/0012-pieces-not-ready-made-screens.md)): the positions table is in `examples/_util.py`. |
| Change flash and throttling | done | Up / down colors and fade per column; a per-cell refresh cap enforced in C++. |
| Hotkeys and context menus to Python | done | Context menus can be customized (`CommandBars("Cell")`); `Application.OnKey` runs a Python handler, which reads the cell context (the active cell's table row, e.g. the strike) with `yc.interact.cell_context()`. |
| Alert rules | done | Thresholds on cells or bound columns: color, sound, popup, Windows notification; with a log. |
| Multi-window, multi-monitor layouts | done | Floating sheets, containers and panels are saved in the `.ycg`; Phase 3 adds named layouts that can be switched. |
| Order ticket, blotter, strategy console, risk dashboard | done | Additional templates (see below). Removed in phase 6 ([ADR 0012](adr/0012-pieces-not-ready-made-screens.md)): the examples build it from the pieces. |

**Gate**: a complete TXO market-making screen built from the templates in about 100 lines of Python, passing the
Phase 1 benchmark at the opening peak. Built: `examples/txo_market_maker` (main.py 109 lines then, 160 since it was
rebuilt from the pieces in phase 6; the feed 32 lines) with the chain, a ladder with click-to-trade, positions with totals, limits, a strategy console, a ticket and a blotter, paper
fills; with the screen running, Python-to-screen latency p50 4 ms / p99 30 ms at 58 fps; the bind path's benchmark
is in Phase 1.

## Phase 4: research and modelling

Research and live trading on the same screen, with the speed Jupyter and matplotlib don't have.

| Item | Status | Notes |
|---|---|---|
| Charts bound to DataFrame columns | done | `yc.plot` draws columns directly: lines, steps, scatter, bars, histograms, fills, candlesticks, heatmaps with labels, lines and text; figures of several linked plots. |
| Linked tables and charts | done | `fig.link_table(table, column)`: the active row is marked on the plots, a click on a plot selects the nearest row (sheet dates and time axes are converted). |
| Downsampling in C++ | done | A min/max pyramid per line (ten million points zoom at ~2 ms a frame), LTTB on request, candles merged per pixel, scatter plots over 100,000 points drawn as one point per occupied pixel cell (outliers kept); lines fit all their data. |
| Backtest report template | done | Statistics, monthly returns with a color scale, the trade list linked to price / equity / drawdown charts; now `report()` in the `backtest_report` example (ADR 0012). |
| Record and replay | done | `.yctape` recordings of bind frames (a snapshot, then every frame with its time); replay at 0.25x to 50x or as fast as possible through the same inbox as live frames, with seek, pause and loop; `yc.replay`, Trading > Record / Replay. Topic pushes (`Topics.Push`, `=QUOTE()`) are recorded and replayed too. |
| `yc.plot`: plotting from any script | done | matplotlib-like calls open dockable, live-updatable figures; data through shared memory; `append` with a capacity and `follow` for live series. |
| Analysis ToolPak | done | Data > Data Analysis with all 19 tools and Excel's output layouts; `Application.Run("ATPVBAEN.XLAM!Regress", ...)` and `yc.analysis`. Chart outputs are left to `yc.plot`. |
| DataFrame round trips | done | `Range.to_frame()`, `yc.data.read(table)` / `BoundTable.to_frame()` and `ws.write_frame(df)` go through shared memory by column (dates as datetime64 and back with date formats, whole numbers as int64): about 0.1 s for a million cells each way; `ws.bind` for live tables. |

**Gate**: the Phase 3 market-making screen switches between live and replay without code changes. Passed: the
option chain is recorded and replayed (Trading > Record / Replay) while the strategy, positions and orders keep running
on the replayed prices; tables the app keeps itself are bound with `record=False`.

## Phase 5: agents, packaging and open source

| Item | Status | Notes |
|---|---|---|
| The API as a skill with complete examples | done | `skill/ycgrid-python`: the Excel object model plus data binding (with replay), plot, screens built from the pieces (which replaced the templates in phase 6), app development, testing, safety and the MCP tools; the project folders in `examples/` are the complete examples. |
| Headless verification | done | `yc.launch(hidden=True)` / `ycgrid.exe --hidden`, `yc.ui.state()` (the whole UI state in one call), `yc.ui.screenshot()`. |
| Clear error messages | done | bind / plot / replay / analysis errors name the column, lengths, shapes, files and tables involved and say how to fix them. |
| `pip install ycgrid` | done | `scripts\build_wheel.cmd` packs `yc` with `ycgrid.exe` (`yc/_bin`); the `ycgrid` command, `python -m yc open / run / examples`, `yc.launch()`; YCGrid's Python host runs on the installing Python (`YCGRID_PYTHON`). The wheel is attached to each GitHub release (installed from the downloaded file); it is not on PyPI ([releasing.md](releasing.md) step 4). |
| Public benchmark | done | `docs/benchmark.md`: live tables (`scripts/bench.py`) and charts (`scripts/bench_plot.py`, measured against matplotlib), the machine, and how to measure Excel RTD / xlwings and web grids in the same scenario. |
| README | done | Research and trading section with screenshots (backtest, volatility surface), "Why not Excel + xlwings, or Perspective", pip install, benchmarks; English and Traditional Chinese; an animated capture at the top (`tools/make_readme_gif.py` records it from a hidden YCGrid). |
| Open-source boundary check | done | `scripts/check_boundary.py`: credentials, tokens, private addresses and host names, personal paths, secret files, large binaries and private patterns (`--extra`) in the tracked files and, with `--history`, in every commit; exit code 1 on findings. |

**Gate**: an agent that has never seen YCGrid writes a working option chain screen from the skill alone; a clean Windows
machine installs with one command.

## Additions beyond the roadmap

Still in YCGrid:

- **Paper trading and an order gateway interface**: `yc.trading` defines orders, fills, positions, a gateway interface
  and pre-trade risk limits; `PaperGateway` on `yc.sim` drives the examples and tests. Real brokers plug in outside
  this repository.
- **Market data simulator**: `yc.sim` generates ticks, option chains (Black-Scholes with a smile) and order books for
  the examples, demos and the benchmark.
- **Live bars**: `yc.bars` turns ticks into live candles.
- **Latency monitor**: Trading > Latency Monitor plots Python-to-screen latency, frame CPU, engine load and cells
  written per second, live.
- **Alert rules window**: Trading > Alert Rules adds rules for the selected cells, and turns rules and their actions
  (sound, popup, taskbar, Windows notification) on and off without code.

Built, then removed in phase 6 ([ADR 0012](adr/0012-pieces-not-ready-made-screens.md)); the examples now do the same
with numpy / pandas and the pieces:

- **Quant helpers** (`yc.quant`): Black-Scholes / Black-76 prices and Greeks, implied volatility, SVI smile fitting,
  bootstrapped statistics, least squares, ADF and Engle-Granger cointegration tests, half-life, rolling beta, Hurst
  exponent, as Python functions and vectorized UDFs.
- **Backtest engine** (`yc.backtest`, `yc.ta`): target positions or Strategy classes, next-open fills, costs, sweeps
  with heatmaps, walk-forward optimization with its efficiency; technical indicators.
- **Option strategies and portfolio construction** (`yc.options`, `yc.portfolio`): builders, exact breakevens and
  limits, payoff and value curves, Greeks; minimum variance, maximum Sharpe, risk parity and the frontier, long only,
  with a built-in quadratic-program solver.
- **History playback** (`yc.playback.Player`): your own tick files played into bound tables at any speed with pause
  and seek, conflated by key.
- **Factor research** (`yc.factor`): rank IC, quantile portfolios, IC decay, turnover, a summary.
- **Portfolio risk** (`yc.risk`): historical, normal and Cornish-Fisher VaR / ES, contributions, scenario grids of
  option books by full revaluation, Kelly and volatility-target sizing.
- **Research to live trading** (`yc.live.LiveStrategy`): the backtest's function or Strategy class run on live bars,
  trading the difference through the OrderManager.
- **Execution algorithms** (`yc.algo`): TWAP, VWAP along a volume profile, POV and iceberg orders worked through the
  OrderManager, with a live monitor table.
- **Session tools** (`yc.session`): a dated snapshot of the workbook and its bound tables, and a pre-open checklist.
- **Notebook** (removed by [ADR 0011](adr/0011-the-notebook-is-removed.md)): the window's cells stay in the `.ycg`
  (`NBOK` chunk), and `yc.notebook.export_ipynb()` writes them to a `.ipynb`.

## Phase 6: building blocks, not canned features (2026-10-25)

YCGrid's users build their own screens in VS Code with an agent. So YCGrid builds in what is practical, fast, and what
other systems can't do or integrate poorly (the pieces), and leaves the features made of them to the user.

**Not built in** (an agent writes them in minutes on the pieces below; built in, they add nothing only YCGrid has): an AI
assistant pane, a trade journal, margin calculators, SQL (Python queries do it), a statistics pane, notifications and
daily reports, one-click order buttons, a scanner, an ML panel, parameter-sweep overfitting reports.

**Removed** ([ADR 0012](adr/0012-pieces-not-ready-made-screens.md)): the research and trading libraries (`yc.ta`,
`yc.quant`, `yc.factor`, `yc.portfolio`, `yc.risk`, `yc.options`, `yc.backtest`, `yc.live`, `yc.algo`, `yc.session`),
the ready-made screens (`yc.templates`), `yc.playback`, `yc.plugins`, `.ycapp` packages and the test group on the
View tab; and four Excel features better done in Python or not wanted: Get & Transform (Power Query and its M
interpreter), the Data Model with the CUBE functions, co-authoring through a shared folder, and reading `.xls`. Old
files keep their chunks (QRYS, DMOD, COAU) as they are. The examples were rewritten to build the same screens and
research from the pieces and numpy / pandas.

| Item | Status | Notes |
|---|---|---|
| A1. VS Code kernel on the live YCGrid | dropped | Built, then removed with the notebook (ADR 0011): small gain over the Immediate window, debugging, hot reload and `yc.connect()`, for an ipykernel download, a kernel spec per installation and a split screen. |
| A2. The notebook removed | done | ADR 0011; the `NBOK` cells are kept (hard rule 21); `yc.notebook.export_ipynb()` writes them out. |
| A3. Debugging the running strategies | done | Developer > Debug in VS Code: debugpy in the host, an "Attach to YCGrid" configuration; the window keeps drawing at a breakpoint. |
| A4. Hot reload that keeps state | done | Saving a project file patches its functions and classes in place (callbacks and live objects get the new code); new / deleted files restart; `editor.restart_on_save`. |
| B1. Rect cells | done | `ax.rects(...)`: hundreds of thousands of rectangles with colors and text, culled and merged in C++ (footprint, profiles, TPO, zones). |
| B2. Basic shapes | done | `barh`, `errorbar`, `polygon`, `arrows`, `annotate`, per-point colors on bars / scatter, the last value as an axis tag, buy / sell marks on candles (`signals`: triangles under / over the price with labels; the live strategy example uses them). |
| B3. Chart events to Python | done | `fig.on("click" / "dblclick" / "hover" / "select" / "drag" / "release")`, draggable handles (`drag_line`, `drag_point`); pushed to the host, polled by scripts. Synced crosshairs: `plot.figure(sync_cursor="group")` shows the hovered x in every figure of the group (the plots of a `link_x` figure among themselves); `fig.cursor(x)` sets it from Python. |
| B4. Column renderers of bound tables | done | `render=` / `set_render`: bars, two-sided bars, heat, progress and per-row trend lines drawn every frame; saved in `BIND`. |
| C1. `=PY()` | done | Python in cells, sharing the kernel's namespace; DataFrames spill with their header; UDF arrays spill too. |
| C2. Data off the cells | done | `yc.data.save_columns` / `view` (Data View: tens of millions of rows, parallel sort and filter, To Sheet) and `Axes.plot_files` (C++ reads `.npy` columns). Arrow data come as a pyarrow Table or an Arrow / Feather / Parquet file and are written to `.npy` columns once (no Arrow reader in C++); the virtual rows are the Data View, a window of its own rather than a bound table on a sheet (a sheet stops at 1,048,576 rows; To Sheet copies what is shown). |
| C3. Time machine | done | `record(everything=True)`: every bound table, the figures and markers (orders, fills, alerts, `mark()`); markers on the replay bar, Pin / Compare two moments. |
| D1. `.ycapp` apps | dropped | Built, then removed (ADR 0012): copying the project folder or cloning it does the same without an install flow. |
| D2. Feed and gateway interfaces | done | `yc.feeds.Feed` (conflation, heartbeat, reconnect; `CsvTailFeed`, `WebSocketFeed`), `trading.check_gateway`, `OrderManager.on_order` / `on_fill`; the plugin registry was removed (a feed is a class you import). |

Examples: `order_flow` (B1-B3), `quote_board` (B4), `big_data` (C2), `py_cells` (C1), `time_machine` (C3).

## Examples

The trading screen is a project folder (`examples/txo_market_maker`: feeds, parameters, operator mode, kill
switch, replay), and so is every research and trading example (`examples/<name>`: its workbook, `main.py` and
`ycgrid.toml`, the code they share in `examples/lib`): they ship with YCGrid in its `examples` folder, and opening
one's workbook builds its screen. (They were a package, `yc.examples`, with a Trading > Examples gallery, until the
examples and samples became one folder.)

| Example | Shows | Status |
|---|---|---|
| `examples/txo_market_maker` | TXO chain, DOM, positions, limits, blotter, strategy console, built from the pieces through an OrderManager | done |
| `backtest_report` | statistics, monthly returns, trade list linked to price / equity / drawdown charts | done |
| `vol_surface` | implied volatility from prices, SVI fit per expiry, 3-D surface, smiles | done |
| `factor_research` | quintile portfolios, rank IC, ToolPak statistics, four linked charts | done |
| `strategy_lab` | a parameter sweep with a Sharpe heatmap, the out-of-sample check, walk-forward, indicators (loops over a pandas backtest) | done |
| `live_candles` | ticks into live 5-second candles with EMA, volume and RSI (`yc.bars`) | done |
| `live_strategy` | the backtest's rule, unchanged, trading on paper on each completed bar, with `signals` per order | done |
| `pairs_trading` | hedge ratio by `Application.Run("ATPVBAEN.XLAM!Regress")`, half-life, z-score, backtest report | done |
| `portfolio_optimizer` | ToolPak covariance, MMULT formulas, maximum Sharpe by Solver, frontier chart | done |
| `monte_carlo_var` | 100,000 fat-tailed scenarios, VaR / ES, tail contributions | done |
| `risk_dashboard` | positions with Greeks, limits, spot x volatility scenario grid | done |
| `options_desk` | a live chain (a bound table with ITM / ATM formats), an iron condor opened at its mids, P&L curves, Greeks and a scenario grid | done |
| `option_payoff` | legs in cells, payoff as formulas and an Excel chart | done |
| `dom_ladder` | price ladder with click-to-trade against the paper exchange | done |
| `depth_heatmap` | resting liquidity by price over time as a live heatmap, the last price and trades drawn over it | done |
| `execution_algos` | a TWAP and an iceberg written in the example: progress against the schedule, slippage, child orders, fills as `signals` | done |
| `history_playback` | a CSV of past ticks played into a live quote table and candles at 60x by a 40-line player | done |
| `tick_replay` | live quotes, a live chart, recording and replay into the same screen | done |
| `quick_plot` | ten million points, a million-value histogram, a correlation heatmap | done |
| `million_cells` | the benchmark scenario with throughput and latency | done |
| `order_flow` | a live footprint (rects), the volume profile (barh) with its POC, VWAP with an axis tag, a limit order dragged on the chart | done |
| `quote_board` | forty symbols with size bars, change bars, volume heat and per-row trend lines drawn by the table | done |
| `big_data` | twenty million ticks as one line and in a Data View, read from column files | done |
| `py_cells` | `=PY` statistics, correlations, a regression and rolling means spilled from ranges | done |
| `time_machine` | a ladder and candles recorded whole, replayed with markers for every fill, Pin / Compare | done |

## Risks

- **Diluted focus**: Excel parity work must not crowd out the trading path; every phase gate is about trading or
  research.
- **Cross-process latency**: if Phase 1 misses its gate, later work is limited; it comes first.
- **Maintenance**: open source brings issues and compatibility work; the supported surface is the documented API.
- **Leaks**: trading logic or internal connections in the public tree; the boundary check runs before every release.
