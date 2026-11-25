# From research to trading in YCGrid

A walk through the whole workflow on one screen: data, research, backtests, live paper trading, execution, risk and
replay. YCGrid builds in the pieces only it has (live tables, fast plots with events, click actions, alerts, safety,
the order manager, record and replay); the screens and the maths are yours, written with numpy, pandas, your
packages or your agent ([ADR 0012](adr/0012-pieces-not-ready-made-screens.md)). Every step names the pieces and an
example that puts them together (a project in `examples/<name>`: open its workbook; read its `main.py`). All
examples use simulated data; nothing here connects to a real market or account.

## 1. Data in and out

| You have | Do this |
|---|---|
| A DataFrame to look at | `ws.write_frame(df)` writes it as cells (about 0.1 s for a million cells); `Range("A1").CurrentRegion.to_frame()` reads cells back |
| A live feed | `ws.bind(df, key="symbol", formats={"bid": {"tick": 0.05, "max_hz": 4}}, flash=["bid", "ask"])` makes a live table; `t.update(changed)` sends only changed rows, `t.sync(full_snapshot)` works out the difference for you |
| Topics from another process | `Range.BindTopics` / `=QUOTE("name")` with a feed process under `[[python.feeds]]` in `ycgrid.toml`; `yc.feeds.Feed` for conflation, heartbeat and reconnects |
| Ticks to bars | `yc.bars.BarBuilder(seconds=5)`; `bb.add(t, price, size)` returns the bars it completed |
| A history file (CSV, Parquet) | read it with pandas and send the rows whose time has come to a bound table from a thread (`history_playback` has a 40-line player with pause and seek) |
| Tens of millions of rows | `yc.data.view(df)` (Data View: scroll, sort, filter in C++) and `Axes.plot_files` |

Examples: `tick_replay`, `history_playback`, `million_cells`, `live_candles`, `big_data`.

## 2. Research

- **Research from VS Code**: scripts and `# %%` cells connect to the open YCGrid with `yc.connect()`; `yc.plot` opens
  fast windows in YCGrid, the Immediate window runs code in the running host, and Developer > Debug in VS Code stops
  in it. `=PY()` runs pandas in cells and spills the result.
- **Charts that keep up**: `yc.plot` draws lines, candles, buy / sell marks, rectangles, scatter, histograms, heatmaps
  (also in price / time coordinates for depth maps) and 3-D surfaces; ten million points zoom smoothly, live series
  append and follow, figures share crosshairs (`sync_cursor`). See [benchmark.md](benchmark.md) for the comparison
  with matplotlib.
- **Statistics**: the Analysis ToolPak (Data > Data Analysis, or `yc.analysis.Regress(...)` with Excel's arguments),
  Solver, PivotTables, formulas and dynamic arrays; in Python, numpy / pandas / statsmodels / scipy. The examples carry
  the maths they use in `examples/_math.py` (Black-Scholes, implied volatility, SVI, indicators, performance
  statistics) and in place (rank IC and quintiles, VaR / ES, a frontier, a half-life).

Examples: `factor_research`, `vol_surface`, `pairs_trading`, `portfolio_optimizer`, `monte_carlo_var`, `quick_plot`,
`py_cells`.

## 3. Backtests

A backtest is a few lines of pandas: a target position per bar, held from the next open.

```python
def rule(d, fast, slow):                       # the position to hold after each bar's close
    return (d.Close.rolling(fast).mean() > d.Close.rolling(slow).mean()).astype(int)

pos = rule(prices, 10, 30).shift(1).fillna(0)  # decided at the close, held from the next bar
pnl = pos * (prices.Open.shift(-1) - prices.Open).fillna(0) * 200 - pos.diff().abs().fillna(0) * 2 * 200
equity = 4_000_000 + pnl.cumsum()
```

The report is made of pieces: statistics in cells, monthly returns with a color scale, the trades as a bound table,
and candles with `signals` (buy / sell marks), equity and drawdown linked to the table (`fig.link_table`). The sweep is
a loop with a heatmap; walk-forward chooses on each window and trades the next one.

Examples: `backtest_report` (its `report()` is reused by the others), `strategy_lab` (sweep, out-of-sample check and
walk-forward), `pairs_trading`.

## 4. From the backtest to live paper trading

```python
from yc import bars, trading

om = trading.OrderManager(trading.PaperGateway(), risk=trading.RiskLimits(max_qty=5, max_position=10))
bb = bars.BarBuilder(seconds=60)
for t, px, qty in feed:
    if bb.add(t, px, qty):                      # a bar completed: the same rule, unchanged
        target = int(rule(bb.frame(include_forming=False), 10, 30).iloc[-1])
        if target != om.position("TXF"):
            om.submit("TXF", "buy" if target > om.position("TXF") else "sell", abs(target - om.position("TXF")))
```

`om.on_order` / `om.on_fill` keep a blotter and marks on the chart up to date. A real broker plugs in by subclassing
`trading.Gateway` (outside this repository); `trading.check_gateway` tests it against a paper market first.

Example: `live_strategy`.

## 5. Execution

Execution algorithms are small classes with a `step()` called from your loop, sending children through the same
OrderManager: the `execution_algos` example has a TWAP and an iceberg with a progress table (average price, slippage
against the arrival price) and the fills marked on the chart.

Examples: `execution_algos`, `dom_ladder` (click to trade), `order_flow` (a limit order dragged on the chart),
`depth_heatmap`.

## 6. Trading screens and risk

- Screens from the pieces ([screens.md](../skill/ycgrid-python/references/screens.md)): an option chain is a bound table
  keyed by strike with conditional formats for the in-the-money sides; a ladder is a bound table keyed by price with
  click actions; positions and limits are bound tables with `GROUPBY` totals, data bars and a red rule beyond 100%.
- Safety: parameter cells that need confirmation and are logged, heartbeats with stale-data alarms, operator mode,
  a kill switch (`om.flatten()`), and the change log (Trading > Change Log).
- Alerts: `yc.alerts.add("Delta limit", "Risk!C5", ">", 500, notify=True, flash_window=True)` highlights, beeps,
  pops up, flashes the taskbar and shows a Windows notification; `@alerts.on_alert` runs your code. Trading > Alert
  Rules manages the same rules without code.

Examples: `risk_dashboard`, `options_desk`, `monte_carlo_var`, `option_payoff`, and the `examples/txo_market_maker`
project.

## 7. Record, replay and reproduce

Trading > Record writes everything that reaches the bound tables (and topic pushes) to a `.yctape`; Record Everything
adds the app's own tables, the figures and a marker per order and fill. Replay plays it back into the same screen at
any speed, with the strategy code unchanged; pin a moment and compare it with another (`yc.replay`).

Example: `time_machine`.

## 8. Agents

The MCP server (`python -m yc.mcp`) gives an agent 43 tools: read and write cells and tables, run Python in the host,
build examples, record and replay, take screenshots and read the whole UI state. The `skill/ycgrid-python` folder is
the reference an agent needs to write working screens from the pieces.

## 9. Measure

`scripts/bench.py` (live tables: cells per second, tick-to-screen latency, frames, memory over hours) and
`scripts/bench_plot.py` (charts against matplotlib); Trading > Latency Monitor shows the same numbers live. Results are
in [benchmark.md](benchmark.md).
