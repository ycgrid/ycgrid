# Building a YCGrid app that works: structure, rules, pitfalls, testing

Read this before building anything bigger than a macro: a trading desk, a quote board, a research screen, a strategy.
Every rule here comes from a bug that shipped once. `examples/txo_market_maker` (an option market-making screen on
simulated quotes) shows the pieces put together (a feed process, bound tables, click-to-trade, risk limits, alerts),
but it is a single `main.py` that rebuilds its sheets on every open, not the structure below: lay a larger app out as
in §1.

YCGrid ships building blocks (object model, panels, `yc.plot`, topics, `yc.trading`, `yc.safety`); the app is yours to
compose. Don't wait for a ready-made feature: build it from the blocks, and if a block is missing or misbehaves (a
crash, a freeze, something only C++ can draw), report it instead of working around it in Python.

## 0. The workflow (every app, in this order)

Each stage ends with a check you can show, not with "it looks fine". Skipping one is how the bugs in §9 shipped.

| Stage | Do | Done when | Details |
|---|---|---|---|
| 1. Find out | Read `main.py` first. For a venue or data source, list what it **streams** and what only REST gives, by subscribing on the live endpoint (§5), before designing anything | a table: each piece of data -> stream name or REST call | §5 |
| 2. Structure | `main.py` / `modules` / `lib` / `feeds`; `UI_VERSION` checked in `Workbook_Open` **and** `Workbook_AfterReload`; objects by lifetime; threads find the current object | saving any module patches or rebuilds the running app without Developer > Reload; `Workbook_HostShutdown` stops every thread | §1, §2 |
| 3. Screen | Lay out by [screens.md](screens.md) "Screen layout rules"; freeze with the recipe in §3; lock what the user mustn't move ("Lock the screen") | a screenshot at the user's resolution passes the review list ([testing.md](testing.md) §6); a drag on the chain or a panel's tab changes nothing | screens.md, §3 |
| 4. Data | Push, not pull; REST seeds, fills gaps, and stands in only while a stream is down; a click wakes the fetch | measured: REST calls per minute with the streams up (only what no stream carries), click-to-display time under half a second | §5 |
| 5. Orders | `OrderManager` / your broker class with risk limits; queued and in-flight orders count; an unanswered order is UNKNOWN until checked | a paper session: every limit refuses with a reason; a dropped answer is resolved by the client order id | §6 |
| 6. Test like the user | A session script on a copy of the project, hidden YCGrid, paper config, the user's window size | the script passes, `ui.errors()` is empty, every screenshot read | §8, [testing.md](testing.md) |
| 7. Go live | The go-live checklist (§6), with the user | the user said yes to each item | §6 |
| 8. Report | What changed, how it was verified, what wasn't; which data on screen is live and which is simulated or delayed | | SKILL.md §4 |

Keep `lib/` testable without YCGrid (`tests/test_lib.py` with plain Python) and the session script in `tests/`: both
run after every change. The project's copy of this skill (`ycgrid-python` in `.claude/skills`, `.agents/skills`, `.github/skills`)
belongs to YCGrid and is replaced by the version of the YCGrid that opens the project: put project notes in
`AGENTS.md` / `CLAUDE.md`, never in the skill copy.

## 1. Structure

```
main.py              Workbook_Open -> RebuildUI() when UI_VERSION changed, then _setup(); Workbook_AfterReload (the same
                     check after a hot reload); Workbook_HostShutdown; Workbook_BeforeClose
modules/App.py       the app: building the sheets / panel (once per UI version), _setup (every open), macros, objects
lib/layout.py        cell addresses, topic names: ONE place, imported by the host modules and the feed processes
lib/*.py             logic without YCGrid (strategy, P&L): unit-testable with plain Python
feeds/quotes.py      market data in its own process ([[python.feeds]] in ycgrid.toml)
```

- **Objects by lifetime.** Decide what lives once per host (the quote subscription, the chart), once per session (the
  paper account, the strategy state) and per open (event registrations, parameter cells). Keep each in a module
  global that survives a hot reload: `_desk = globals().get("_desk")`.
- **Workbook_Open does two things.** `RebuildUI()` only when `ThisWorkbook.Store["ui.version"] != UI_VERSION` (it
  resets what the user typed into the layout), and `_setup()` every time (OnChange registrations, `safety.parameter`,
  `BindTopics`, subscriptions, the session). Bump `UI_VERSION` whenever the building code changes.
- **Saving a module doesn't run Workbook_Open again** (a hot reload keeps the running state): put the same version
  check in `Workbook_AfterReload(Files)`, so bumping `UI_VERSION` rebuilds the screen the moment the file is saved. A
  constant edited in the source (`UI_VERSION = 5` -> `6`, a number, text, a tuple) takes its new value on the reload;
  module state (dicts, lists, objects, a counter the code changed) keeps its value.
```python
def Workbook_AfterReload(Files):                 # after every hot reload (Files: "modules/App.py", ...)
    if ThisWorkbook.Store.get("ui.version") != UI_VERSION:
        App._stop()                              # what Workbook_HostShutdown does
        App.RebuildUI()
        ThisWorkbook.Store["ui.version"] = UI_VERSION
        App._start()                             # what Workbook_Open does after the check
```
- **Workbook_HostShutdown** stops threads, unsubscribes, closes gateways (2 s limit). It runs on a host restart too.
  **Workbook_BeforeClose** runs only when the workbook closes: restore application settings there (§6).
- A host restart (a code reload, Developer > Reload) re-runs `main.py` and `_setup()`: everything must come back by
  itself (figures by key, panels from the workbook, subscriptions). Feed processes restart with the host.

### Trust: when your project's code runs (Excel's macro security)

YCGrid treats a project's Python like Excel treats macros (File > Options > Trust Center Settings):

- **Your own projects run without a prompt**: a project made with Alt+F11 or the VS Code / Debug buttons is trusted at
  once, and the projects folder (`Documents\YCGrid`, File > Options > Projects folder), YCGrid's `examples`
  are Trusted Locations (with subfolders). Keep your projects there, or add your work folder in Trust Center Settings >
  Trusted Locations > Add new location (tick "Subfolders of this location are also trusted").
- **Someone else's folder** (a zip from a colleague, a download) opens with its code disabled: no Workbook_Open, no
  events or buttons (Excel's "Cannot run the macro" message), `@udf` cells show `#NAME?`, `=PY` shows `#BLOCKED!`, no
  packages are installed, and the yellow Message Bar says "Python code has been disabled. [Enable Content]". Enable
  Content runs Workbook_Open at once and makes it a Trusted Document (moving the folder asks again); File > Info >
  Enable Content > Advanced Options enables it for this session only.
- **Scripts and tests**: `yc.launch(file)`, `Workbooks.Open` and the MCP tools follow `Application.AutomationSecurity`,
  `msoAutomationSecurityLow` by default: the code runs, as Excel's automation, and no Message Bar shows (in a hidden
  YCGrid too). To
  test the prompt as a person sees it: `yc.launch(book, hidden=True, automation_security=2)`
  (`msoAutomationSecurityByUI`), then `ui.state()["trust"]` (`code`, `bar`, `reason`) and click `Enable Content` from
  `ui.items()`.
- **Downloaded files** (a zip from a browser or a mail attachment keeps the Mark of the Web on the extracted `.ycg`)
  first open in **Protected View**: read-only, code off, "[Enable Editing]"; after Enable Editing the Security Warning
  above applies. Scripts' `Workbooks.Open` never opens in Protected View. Unblocking the file in Explorer
  (Properties > Unblock) removes the mark.
- `yc_python_status` says `code_disabled: true` (mode "disabled by the Trust Center") when a person's window kept the
  code from running; `yc_reload_python` refuses then. Ask the user to click Enable Content; never enable it yourself.

### Project tools: `python -m yc` (run with YCGrid's own Python, `<YCGrid>\runtime\python\python.exe`)

| Command | Does |
|---|---|
| `init <folder> [--sheet Sheet1=Quotes ...] [--skill <dir>]` | a project template (`main.py`, `sheets\<CodeName>.py`, `modules\Module1.py`, `ycgrid.toml`, `requirements.txt`, `AGENTS.md` / `CLAUDE.md`, the MCP server in `.mcp.json` / `.vscode\mcp.json` and `.claude\settings.json` (Claude Code uses it without asking), `.vscode\extensions.json`, `pack.cmd`, `.gitignore`, the VS Code settings `devenv` writes, the skill copies in `.claude\skills` (Claude Code), `.agents\skills` (Codex) and `.github\skills` (GitHub Copilot)); existing files are kept; prints the files made |
| `devenv <folder> [--sheet ...]` | the VS Code settings (Pylance, the `yc` package path, CodeName types); the VS Code button runs it every time |
| `pack <folder> [--with-wheels] [--with-dev] [--no-test]` | `dist\<folder>-YYYYMMDD.zip` of the project without `.ycgrid`, `.venv`, `_work`, `_uvcache`, caches (`__pycache__`, `*.pyc`, `*.tmp~`), `.git`, `dist` (and without `.claude`, `.agents`, `.github`, `examples` unless `--with-dev`; `runtime\wheels` only with `--with-wheels`); when the folder holds `ycgrid.exe` the zip is unpacked and started once as a smoke test |
| `open [file]` | start YCGrid (with a workbook) and return |
| `run <script> [args] [--workbook f] [--hidden]` | run a script connected to the running YCGrid, starting one when none runs |
| `python info` / `python upgrade [3.13]` | the project Python's version and path / download another Python next to `runtime\python` (then set `[python] version` in `ycgrid.toml` and swap the folders) |

## 2. Threads: who calls you, and the one-lock rule

| Code | Runs on |
|---|---|
| macros, panel buttons (`OnAction`), cell events, `fig.on` / handle / menu callbacks, `Workbook_*` | the host's event loop (one thread) |
| `Application.Topics.Subscribe` callbacks | the subscription's own worker thread |
| `OrderManager.on_order` / `on_fill` | the gateway's thread (`PaperGateway`'s matching thread, a broker SDK's thread), or the caller of `submit` / `cancel` / `modify` |
| your refresh loop | your thread |

- Shared state (positions, strategy, lines) under **one** lock (`threading.RLock`), always taken in the same order.
- **Never call YCGrid or wait for an order event while holding the lock**: take what you need, release, then write
  cells or update the chart.
- Callbacks stay short. No `time.sleep`, no network, no `ui.state()` polling in a callback or in `Workbook_Open`.
- One refresh thread (4 times a second is plenty) writes the status cells and chart decorations, and **only when
  something changed** (keep the last written value; compare before writing). Writing the same cells every tick
  floods the engine and makes the screen flicker.
- **A thread finds the current object; it doesn't keep the one it started with.** A rebuild (a `UI_VERSION` bump, a
  host restart) makes a new desk object while a thread that survived it still holds the old one and keeps drawing for
  it. Start each thread once per host (a module global `_thread = globals().get("_thread")`), and let `setup` hand it
  the new object (`_bind(desk)`) or read a module global on every loop.
- A thread that waits (`Event.wait(timeout)`) is woken by the event that changes its work (`_wake.set()` from the
  click handler), so the user doesn't wait for the timeout.

## 3. Sheets, cells and inputs

- **Rebuilding a sheet**: delete its tables first (`for lo in ws.ListObjects: lo.Delete()`), then `ws.Cells.Clear()`
  (contents, formats and data validation), `ws.Cells.FormatConditions.Delete()`. Turn `Application.EnableEvents`
  off while building and back on in `finally`.
- **Data validation lists only on cells that take a choice** (`"Support,Resistance"`, `"TRUE,FALSE"`). A list left on
  a number cell refuses every number ("the value isn't in the list"); when a layout moves, the old rule must go
  (`Range.Validation.Delete()`).
- **Number settings the user changes go through `yc.safety.parameter`** (type, min, max, confirm, change log). Panel
  controls (spin, combo, check box) linked to a parameter cell change it through the same checks; give a spin the same
  `Min` / `Max` as the parameter, so the user can't pick a value that will be refused.
- **Your own writes must not trigger your handlers**: register with `Range.SetOnChange(handler, Sources=ycUser)`
  (user edits only) or `Sources=ycUser | ycPython` when code writes must count too.
- A hidden status column (e.g. `K`) that the panel's labels show through `LinkedCell` + `NumberFormat`
  (`'[Red]"LONG "0;[Green]"SHORT "0;"FLAT"'`, `"[Red]+#,##0;[Green]-#,##0"`) is the cheapest live display: no Python
  per update, colors by sign. Quotes go straight in with `Range.BindTopics`; a formula
  (`=TEXT(K2,"#,##0")&" / "&TEXT(K3,"#,##0")`) builds composite texts.
- Bound tables (`yc.data.bind`) for logs and blotters: `replace` for a newest-first log, `update` keyed by id for
  orders.
- **Freezing a rebuilt sheet, in this order**: a workbook saved frozen opens frozen, and `ScrollRow` /
  `ScrollColumn` don't move a frozen window, so a rebuild that only sets `SplitRow` again freezes wherever the old
  view was (the header row vanished that way).
```python
w = ActiveWindow                     # the sheet must be active
w.FreezePanes = False                # 1. unfreeze what the file saved
w.ScrollRow, w.ScrollColumn = 1, 1   # 2. scroll home
w.SplitRow = 5                       # 3. split under row 5 (an unfrozen window splits, as Excel's)
w.FreezePanes = True                 # 4. freeze where the split is: rows 1-5 stay on top
w.CenterColumns = "K"                # 5. a T layout: K stays in the middle (after the freeze)
ws.Rows("1:3").Hidden = True         # 6. hide helper rows last
```

## 4. Charts (`yc.plot`)

- **Every figure gets a `key`.** A figure made again with the same key takes the window over; the dock place and
  layouts follow the key. Never hunt figures by title.
- **One chart per host, not per session.** Rebuilding the figure clears its items (history). Seed live candles once:
  `ax.candles_live(topic, seconds, volume_topic=..., volume_ax=...)` then `candles.set_data(t, o, h, l, c, volume=v)`
  from the ticks you kept (`yc.bars.BarBuilder`).
- **A live chart that doesn't move under the trader**: `ax.set(follow=bars * seconds, yfit="stable")` (y changes only
  when the price nears the edges; the y axis keeps its width, so tags don't shift the plot). Zoom / pan pauses
  following and shows a "Paused" badge; the badge or `ax.set(follow=...)` goes back live: don't fight it with code
  that resets limits.
- **Create items and handles once, update them when they change.** `item.set_data(...)`, `handle.set(value, label=,
  color=)`; never remove and re-create on every refresh (flicker, lost drags). Re-create only when the set changes
  (a line added / removed).
- Handles (`drag_line`, `drag_band`) never change the view unless `fit=True`; while the user drags, limits stay put.
  Order lines: `on_release` -> modify, `on_close` (the x) -> cancel; when the modify is refused, `set` the line back.
- Y-axis tags (line / hline tags, handle labels) are stacked apart by YCGrid when prices are close; keep them short
  (`"STP1 22991"`, `"AVG 23003"`).
- The context menu items are fixed texts; the callback gets `e.y` (where it was opened). Decide one gesture per action
  and document it in the app's help text (e.g. double-click adds a line, right-click opens orders and lines).
- **Docking**: dock the figure once (`if not fig.Docked: fig.Dock(...)`, and a `Store` flag for "lay out again");
  docking a docked figure again moves it. To migrate a workbook saved with an older layout, `ui.Layout.Reset()`
  first, wait a moment, then dock.
- `theme="dark"` for trading (the dark trading theme: black, red up / green down); the context menu and popups follow it.

## 5. Market data and strategies

- Quotes come from a **feed process** pushing topics (`Application.Topics.Push(ids, values, TimeNs=...)`), never
  from the host. The host gets every tick with `Application.Topics.Subscribe([...], cb)` (host only), charts take them
  in C++ (`candles_live`, `line_live`), cells with `BindTopics`.
- **Startup handshake**: the feed waits until the host says it listens (a cell like `"ready <time>"` written by
  `_setup` after subscribing, cleared at shutdown; the feed accepts only a mark newer than its own start), then sends
  history (back-dated `TimeNs`) and streams.
- **History is drawn, never traded**: the strategy acts only on ticks younger than a few seconds (`tick.time_ns`).
- Watch `subscription.Dropped` and raise `safety.alarm` when it grows.
- Keep the strategy in `lib/` as plain Python (prices in, signals out) and test it without YCGrid.
- **Push, not pull.** Everything the venue streams comes by its stream: quotes, the book, trades, candles, 24 h statistics, and on a live account orders, fills, balances and positions (the user data stream). REST is for what no stream carries (history once on a symbol change, open interest, the contract list), for seeding at start, and as a fallback only while a stream is down. A user-stream event triggers the REST refresh it implies at once; periodic REST reconciliation runs slowly while the stream is up and fast only while it is down (this also keeps the API weight far from the venue's limit).
- **Check stream names against the live endpoint**, not the docs: subscribe each candidate on its own connection and see which push within a minute (an unknown stream may close the connection or stay silent; docs and endpoints disagree on paths and case). Streams that push on trades only (candles, 24 h tickers) can be silent for minutes on thin contracts: that is not an error.
- A user action that changes what is shown (a click selects another symbol) wakes the thread that fetches it and draws as soon as the data arrives; don't leave it to a timer (a 1 s poll plus a 2 s redraw made the chart follow a click seconds late).

## 6. Orders and risk (`yc.trading`)

- Orders go through `OrderManager` with `RiskLimits`; by default only to `PaperGateway`. Never connect a real broker
  without the user's explicit consent.
- **Every position has a protective stop**, also positions from the ticket or a filled limit (the strategy's level,
  else average cost -/+ the stop distance). The user can drag it (keep it as an override) or cancel it with its x
  (keep it off until the position changes).
- **Max position holds for every source**: the strategy, the ticket buttons and chart limits. Refuse with a reason in
  the status bar and the activity log ("would make the position +5, beyond Max pos 4").
- An automatic strategy trades against manual positions: make the Auto switch visible next to the ticket, and say in
  the log which lines traded.
- Flatten = cancel everything (stops too) and close at the market; reset the stop override.
- **Stopping a strategy cancels its working orders** (tag them, e.g. `tag="MM"`, and cancel by tag); its loop also
  cancels any it finds while stopped. A stopped market maker whose quotes stay in the book still trades.
- Reconcile: compare your position, the gateway's (`gw.positions()`) and your fills' every refresh; alarm when they
  disagree for more than a second.
- P&L from your own fills (fees and tax per lot and side), not from gateway internals.
- **Count every order that may still trade**: working orders, orders queued in your app but not yet sent, and orders
  sent without an answer yet. Two clicks in a row each passed the position limit when only acknowledged orders
  counted. `OrderManager` with `RiskLimits(max_position=...)` counts every order it sent that is still open (an answer
  pending or working) and checks the worst case per side (`max(|pos + buys + new buy|, |pos - sells - new sell|)`): a
  resting stop on the other side doesn't make room. It checks one order at a time and knows nothing of what your app
  holds back: a broker class of your own adds the orders it queued, and checks the legs of a batch together before
  sending the first.
- **No answer is not a rejection.** A timeout or a dropped connection while sending (no error code, no HTTP status)
  may have placed the order. Mark it UNKNOWN, keep counting it against the limits, show it as such, and look it up by
  its client order id a few seconds later; never send it again blindly. Only a rejection from the venue (an error
  code) means it isn't there.
- The status message shows the values actually sent (the price after rounding to the tick, the quantity after the
  lot step), not what was typed.

**Going live checklist** (go through it with the user; never switch on your own):

1. Paper first: the same code path with a paper gateway (or the venue's testnet) passes the session script.
2. Keys live outside the project folder (environment variables or a file in the user profile): the folder gets
   copied and packed. Read-only or trade-only keys, no withdrawal permission, an IP allow-list where the venue has one.
3. The mode is one setting (`config.ini`: `mode = paper | live`) shown on screen at all times (a red LIVE badge).
4. Small limits for the first live day: max quantity, max position, max open orders, max loss.
5. The user data stream is connected (orders, fills, balances by push), and the reconciliation against REST agrees.
6. The kill switch / Flatten was tested on paper, and UNKNOWN orders resolve (pull the network cable on paper).
7. The user confirms each item, then switches the mode and restarts.

## 7. Application settings vs workbook settings

- Workbook / sheet settings travel with the file: gridlines and headings (`ActiveWindow.DisplayGridlines`,
  `DisplayHeadings`) belong to the active sheet; panels, figures by key, layouts in `ThisWorkbook.Store`.
- **Application settings are the user's, for every workbook**, and saved: `Application.RibbonDisplay`,
  `Application.DisplayFormulaBar`. If the app changes them, store the user's values first
  (`ThisWorkbook.Store`) and restore them in `Workbook_BeforeClose`.
- Where settings live: File -> Options writes `%LOCALAPPDATA%\ycgrid\settings.json` (this computer: language, view,
  calculation, `live_data.publish_interval_ms`, `editor.vscode` / `editor.restart_on_save`, editing, AutoRecover, the
  ribbon, `errors.log_to_file` / `log_folder`); a project's own settings are in `ycgrid.toml` next to `main.py` and travel
  with the folder:

| `ycgrid.toml` key | Meaning |
|---|---|
| `[python] interpreter` | the Python to use (relative to the project); empty = `YCGRID_PYTHON` when set, then the project's `runtime\python`, then the `runtime\python` next to `ycgrid.exe` or in a folder above it, then (a pip install) the Python YCGrid was installed into |
| `[python] auto_install` | `ask`, `always` or `never`: what to do about missing packages |
| `[python] index_url`, `find_links` | a package index (a company mirror) / a folder of wheels for offline installs (default `runtime\wheels`) |
| `[python] paths` | folders of shared code the project imports as if they were in its `lib\` (relative to the project, e.g. `["../lib"]`, as YCGrid's examples share `examples\lib`): added to `sys.path` after `lib\`, their modules are the project's own (never offered as missing packages) and their imports count for package detection; `python -m yc pack` copies them into the packed project's `lib\`, and the VS Code settings name them for Pylance |
| `[python] version` | a note of the Python the project was made for (YCGrid doesn't read it) |
| `[[python.feeds]]` | a data feed process: `name`, `script`, `restart` (`"always"`: restarted 3 s after it exits, at most 5 times a minute; `"never"`), `enabled` |

| Environment variable | Meaning |
|---|---|
| `YCGRID_PID`, `YCGRID_FEED` | set for feed processes: the YCGrid that started them (`yc.connect()` uses it) and the feed's name |
| `YCGRID_RELOAD=restart` | saving a project file restarts the host instead of swapping its functions |
| `YCGRID_EXE`, `YCGRID_PYTHON` | the `ycgrid.exe` that `yc.launch()` / `python -m yc` start; the Python the host runs on (pip installs) |
| `YCGRID_LANG` | the UI language (`en`, `zh-TW`) |
| `YCGRID_ERROR_LOG` | a folder for the Error Log's files, hidden instances included |
| `YCGRID_REQUEST_TIMEOUT` | seconds a request waits for YCGrid's reply (600; 120 on a connection to a hidden instance). The old name `YC_REQUEST_TIMEOUT` is still read |

## 8. Testing like the user (do this before saying it works)

The full guide - every input, pixels and drags, dialogs, what to check and the screenshot review list - is
[testing.md](testing.md). In short:

Test on a **copy** of the project folder, in a **hidden** YCGrid, through the **same inputs a person uses**:

```python
import shutil, time, yc
shutil.copytree(project, tmp, ignore=shutil.ignore_patterns("__pycache__"))
p = yc.launch(tmp + r"\Book.ycg", hidden=True)
conn = yc.connect(target=p.pid)
while not (s := conn.request("python.status", {})).get("workbook_open_done"):
    time.sleep(0.2)
from yc.mcp import tools
tools.yc_panel_input("Ticket", "btnBuy")                         # a panel button
tools.yc_panel_input("Ticket", "spQty", "set_value", "3")        # a spin / combo / check box
tools.yc_ui_input([{"type": "select", "address": "B18"}, {"type": "type", "text": "4"}], sheet="Desk")  # typing
# the chart the host made, by its key (don't make a figure with that key from the test: it would clear it)
tools.yc_plot_input("desk.chart", [{"action": "click", "x": t, "y": price, "button": "right"},
                                   {"action": "context_menu", "text": "Buy limit here", "x": t, "y": price}])
tools.yc_plot_input("desk.chart", [{"action": "drag_handle", "handle": "BUY1 22950", "y": 22945},   # a handle by label or id
                                   {"action": "close_handle", "handle": "BUY1 22945"},
                                   {"action": "double_click", "x": t, "y": 22900},
                                   {"action": "wheel"}, {"action": "live"}])
```

- Check what the trader sees: the status cells, `ui.state()["figures"]` (handles with labels and prices, `docked`,
  `follow_paused`), the Orders table, `Application.StatusBar`, the activity log, `ui.screenshot(path)`.
- Dialogs as a person answers them: open one with `yc.ui.command(id)` or its shortcut, then `k:Enter` (the default
  button: OK, Solve...) or `k:Escape` (cancel); `ui.state()["dialog"]` is 0 again when it closed. A hidden YCGrid
  never shows a file dialog (Open, Save As on a new workbook, Insert Picture, Export PDF...): it is cancelled at once
  and listed in `ui.errors()` - save tests' workbooks with `ThisWorkbook.SaveAs(path)`.
- Clicks on YCGrid's own chrome: `conn.request("ui.input", {"script": "m:x,y|w:3|c:x,y|w:8"})` in window pixels; the
  ribbon's tab and arrow positions are in `ui.state()["ribbon"]`. Wait more than half a second between two clicks on
  the same tab (a double-click pins the ribbon).
- At the end: `ui.errors()` must be empty (entries marked "answered automatically" are message boxes a hidden instance
  answered for nobody - a refused entry you caused on purpose is expected), `safety.status()["alarms"]` empty, and the
  position / gateway / fills agree.
- Write the session as a script and keep it: run it after every change (it found most of the bugs listed above).
- **The copy runs on paper**: after copying, overwrite its settings (`config.ini` -> `mode = paper`) so a test never
  reaches a live account, whatever the user's folder says. Leave out `state/`, `.ycgrid/` and `tests/` when copying.
- **The user's window size**: `yc.launch(..., hidden=True, size=(1920, 1080))` (or what the user has); a layout that
  fits at the default size can cut the order buttons off at theirs.
- **Packages for a test YCGrid**: a test run with another `ycgrid.exe` (a fresh build) uses that YCGrid's runtime; install
  the project's packages into it with `<YCGrid>\runtime\uv.exe pip install --python <YCGrid>\runtime\python\python.exe
  -r requirements.txt`, never with the system `pip`.
- **Measure what the user feels**: time from the click (`ui.input` / `yc_panel_input`) to the new data on screen
  (poll the cell or `ui.state()["figures"]`), and count the REST calls per minute with the streams up (wrap your
  HTTP function with a counter in the test). Report the numbers.
- **Edit the copy while it runs**: change a module in the copy, bump `UI_VERSION`, save, and check that the screen
  rebuilt by itself and the data streams again (that is what the developer does all day).
- Never test on the user's running YCGrid or capture their screen; never press order buttons in their session.

## 9. Mistakes we made (don't repeat them)

| Symptom | Cause | Rule |
|---|---|---|
| "The value isn't in the list" on a number setting | an old layout's list validation stayed on the cell | §3: clear tables, validation and formats when rebuilding |
| A panel spin can't change a setting ("This is a parameter cell") | older YCGrid refused control writes to parameter cells | update YCGrid; spin Min / Max = parameter range |
| BUY 3 then position 0 | the auto strategy sold at a resistance with the same Qty | §6: a visible Auto switch; log the line that traded |
| A manual position without a stop | stops only for strategy entries | §6: every position has a stop |
| The chart jumps / shifts sideways while trading | y fitted every frame; tags changing the axis width | §4: `yfit="stable"` |
| The chart can't be zoomed | code reset the limits every frame | §4: zoom pauses following; don't reset limits |
| Tags "LA A R1 23040" on top of each other | several tags at one price | §4: short tags (YCGrid stacks them) |
| A new session empties the chart | the figure was rebuilt per session | §4: one chart per host, seeded with history |
| History ticks traded at startup | the strategy took back-dated ticks | §5: trade only fresh ticks |
| the host restarts after saving a module (`[reload] ... __init__ now sets x and 1 Desk object made before doesn't have it; restarting the host`) | a hot reload doesn't re-run `__init__` of live objects, so their new methods would fail on the new attribute: YCGrid restarts the host (Workbook_HostShutdown, then Workbook_Open), as Developer > Reload does | expected; keep state that must survive in the workbook, `ThisWorkbook.Store` or files (stop threads in Workbook_HostShutdown). To keep the host running, read new attributes with `getattr(self, "x", default)` instead of setting them in `__init__` |
| Every workbook opened with a collapsed ribbon | the app changed an application setting and never restored it | §7 |
| UI frozen for 30 s in a test | a message box in a hidden instance (fixed in YCGrid: answered automatically) | §8: read `ui.errors()` |
| A stopped market maker still filled | Stop only stopped the loop; its quotes stayed in the book | §6: stopping cancels the strategy's orders |
| A live chart stayed empty on 0..1 | items made empty, filled later, the view never fitted (fixed in YCGrid: fits on the first data) | §4: `follow=` on live plots |
| A chart over the table it explains, `#####` in cells | fixed positions and widths that didn't fit the data | place charts beside / under the data; widen columns for the largest value |
| Break-evens off by up to a table step | read from the table's grid of prices | interpolate where the P&L changes sign |
| Option orders filled at ~23,000 on paper | the paper gateway priced every symbol from its futures market | `gw.quote(symbol, bid, ask)` for every option you show (YCGrid now rejects unquoted symbols) |
| A payoff chart flat at -0.5..0.5 | the first data were all zero and the view never refitted (fixed in YCGrid: the view follows the data until the user zooms) | let the plot follow; set `xlim` / `ylim` only for a fixed frame |
| The chart followed a strike click seconds late | the candle thread polled every second and redrew every two | §2: the click wakes the thread; §5 |
| Quotes a second old, the venue's request weight high | bid / ask / mark and candles pulled by REST on a timer while the venue streams them | §5: push, not pull; REST only for what no stream carries |
| A stream subscribed by the documented name stayed silent | the live endpoint uses another path or case | §5: check each stream on the live endpoint |
| Bumping `UI_VERSION` changed nothing until Developer > Reload | a hot reload doesn't run `Workbook_Open` | §1: the same check in `Workbook_AfterReload` |
| After a rebuild, the chart drew for the old desk | the thread kept the object it started with | §2: threads find the current object |
| The header row vanished after a rebuild | the sheet was saved frozen; `ScrollRow` doesn't move a frozen window | §3: the freeze recipe |
| The underlying price disappeared | info rows hidden to make room for the T layout | [screens.md](screens.md): the key figure above the center column |
| "BTCUSDT" cut at the top left | text in the first column of a T layout, which scrolls under the frozen edge | [screens.md](screens.md): nothing important at the left edge |
| Bid / ask columns look out of line | headers left, numbers right; a form column mixing alignments | [screens.md](screens.md): header alignment = content alignment |
| Order buttons below the bottom of the panel, text too small | default font and order at 1920x1080 | [screens.md](screens.md): primary actions first, `FontSize` |
| Prices shown as `5.` | `#,##0.####` on a whole number | [screens.md](screens.md): decimals from the tick |
| The message said 0.0123456 but the order went at 0.012 | the message showed the typed value | §6: show the values sent |
| Two quick clicks passed the position limit | queued / in-flight orders didn't count | §6: count every order that may still trade |
| An order sent during a network blip was forgotten (and filled) | a send error without an answer was treated as rejected | §6: UNKNOWN, then look it up by client order id |
| A drag on the strike column moved the cells; Ctrl+Z did nothing | the sheet wasn't protected, and Python's writes cleared the undo list (fixed in YCGrid: undo covers the user's changes only) | [screens.md](screens.md) "Lock the screen" |
| `ProtectedError` from a read (`Worksheets(...)`) in the desk's loop; "protected sheet" for the order book | a held-back bound-table write counted as the user's, and its refusal reported to whichever request ran (fixed in YCGrid) | update YCGrid; the Error Log names the refused cells |
| Agents kept old rules after a YCGrid update | the project's skill copy was made once and never updated (fixed in YCGrid: the host syncs it) | §0: project notes go in AGENTS.md, not the skill copy |
