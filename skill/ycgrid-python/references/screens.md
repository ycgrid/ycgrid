# Trading screens and research from the pieces

YCGrid builds in the pieces only it has: live tables fed through shared memory, fast plots with events and handles,
click actions, alerts, the safety layer, an order manager with risk checks, a market simulator, record and replay.
Screens (option chains, ladders, blotters, reports) and research maths are made of those pieces plus numpy / pandas or
any package (statsmodels, scipy, pandas-ta, vectorbt...): there are no ready-made screens to configure. The examples
(project folders in YCGrid's `examples` folder) show each screen in full; read the one closest to what the user wants
before writing it.

## The pieces

| Piece | Where | Use |
|---|---|---|
| Bound table | `yc.data.bind(ws, df, key=..., formats=..., flash=..., stale_after=...)`, `.update` / `.sync` / `.replace` | any live grid: quotes, chains, ladders, blotters, positions ([data-binding.md](data-binding.md)) |
| Column renderers | `bind(render={"Size": "bar", "Chg": "bar2", "Px": {"type": "trend"}})` | size bars, two-sided bars, heat, progress, trend lines drawn every frame |
| Conditional formats | `Range.FormatConditions.Add(xlExpression, Formula1=...)`, `AddColorScale`, `AddDatabar` | the last price's row, ITM shading, profit red / loss green, limits beyond 100% |
| Formulas over tables | `=GROUPBY(Pos[[#All],[Expiry]], CHOOSECOLS(Pos[#All], ...), SUM, 3)` | totals by group, recalculated as the table changes |
| Click actions, hotkeys | `yc.interact.on_click(range, fn)`, `on_key("^b", fn)`, `Application.OnKey` | click-to-trade, cancel by clicking your order |
| Alerts | `yc.alerts.add(name, ref, ">", value, ...)` | thresholds that highlight, beep, pop up and call Python |
| Plot blocks | `candles`, `signals` (buy / sell marks), `rects` (footprint, profiles, zones), `barh`, `errorbar`, `polygon`, `arrows`, `annotate`, `line(tag=True)` | charts of any indicator or tool ([plot.md](plot.md)) |
| Plot interaction | `drag_line` / `drag_point` handles, `fig.on("click" / "hover" / "select" / "drag" / "release")`, `sync_cursor`, `link_table` | order lines dragged on the chart, region statistics, linked crosshairs |
| Big data | `yc.data.save_columns`, `yc.data.view` (Data View), `Axes.plot_files` | tens of millions of rows or points without passing through cells |
| Orders | `yc.trading.OrderManager(gateway, risk=RiskLimits(...))`, `on_order` / `on_fill` callbacks | every order risk-checked and logged; screens follow the callbacks |
| Live bars | `yc.bars.BarBuilder(seconds=5)`: `add(t, px, q)`, `frame()`, `draw(ax, volume_ax)` | ticks into candles |
| Safety | `yc.safety.heartbeat`, operator mode, kill switch, parameter cells, change log ([safety.md](safety.md)) | live trading guards |
| Simulator | `yc.sim.Market` (`step`, `option_chain`, `book`), `trading.PaperGateway` | demos and tests: never a real market |
| Record / replay | `yc.replay.record(everything=True)`, `mark`, `pin`, `compare` | the time machine |
| Python in cells | `=PY("x.describe()", A1:C200)` | pandas on ranges, spilled |

## Screen layout rules

Each rule comes from a trader looking at a screen that "worked". Check them on a screenshot at the user's window size
(often 1920x1080) before calling a screen done.

- **A header is aligned like its numbers.** Numeric columns (Bid, Ask, IV, Delta, P&L) are right-aligned, and so are
  their headers; text columns are left-aligned with their headers; the T layout's center column (Strike) is centered,
  header and values. A left header over right numbers makes neighbouring columns look shifted.
- **One alignment per form column.** In a ticket (labels in A, inputs in B), every input in B is aligned the same way
  (left), whether it holds a number, a symbol or a choice.
- **Decimals from the tick.** `#,##0.####` shows a whole number as `5.`: give prices a fixed number of decimals from
  the tick size (`#,##0.0` for 0.1, `#,##0.00` for 0.01), quantities from the lot step.
- **The key figure where the eye goes.** On an option chain that is the underlying's price: put it on the header rows
  right above the strike column (bold, larger), not in a corner. Don't hide the rows that hold it to make room.
- **Nothing important at the left edge of a T layout.** With `CenterColumns`, columns left of the center scroll away
  (only whole columns are shown; the outer ones that don't fit are left out); titles and status go in the center
  columns or in the panel.
- **Size the chain to fit.** Column widths from each field's widest value (a position column needs 5, Gamma 9), so
  the whole chain fits the user's screen beside the panel; uniform wide columns pushed the open interest off screen.
- **Choosers on top, around the center.** The underlying and expiry are cells with a list (`Validation`) on the frozen
  top row next to the center column (`標的 [BTCUSDT]  指數 | 80,743.20 | 到期 [261027]`): the trader switches
  without scrolling the panel, by typing or with Alt+Down.
- **Panels: what the user presses first comes first.** Order ticket and Buy / Sell at the top, settings and lists
  below; `FontSize` big enough to read at the target resolution (16-18 at 1920x1080), and a width that shows the
  longest label. Every primary action is visible without scrolling.
- **Messages show the values used**: "BUY 2 BTC-261030-70000-C @ 1,250.0 sent", with the price after rounding to the
  tick, not the text typed.
- **Say what is live**: label simulated or delayed data on screen ("PAPER", "delayed 15 min"), and a LIVE account in
  red.
- **Lock the screen** so a stray drag can't take it apart (a dragged strike column, an undocked ticket). Python
  decides what is locked; Python itself still lays it out:
  ```python
  def _lock_screen():                                   # end of RebuildUI and in Workbook_Open (see below)
      ThisWorkbook.Protect(Structure=True, Windows=True)          # no window moves, docks, resizes or closes
      ws = Worksheets("Chain")
      ws.Unprotect()
      ws.Cells.Locked = True
      ws.Range("I4,M4").Locked = False                              # the inputs the trader types into
      ws.Protect(UserInterfaceOnly=True)                            # no typing, moving or column widths; Python writes
  ```
  `UserInterfaceOnly` isn't saved (like Excel): call it in `Workbook_Open` and `Workbook_AfterReload`, and
  `Unprotect()` before rebuilding. Clicks on locked cells still select them (click-to-trade keeps working). Lock only
  one window with `Panel.Locked`, `Container.Locked`, `fig.Locked`. Turning drag and drop off everywhere is
  `Application.CellDragAndDrop = False` (the user's setting: put it back in `Workbook_BeforeClose`).
  Docked windows show their tab: give panels and figures short captions (the tab is their name on screen). The lock
  is for the workbook's windows only: a YCGrid window the user docks into the locked screen (Error Log...) still
  comes out by its tab.

## Recipes (each is an example to read)

### Option chain (calls | strike | puts) - `options_desk.py`

A bound table keyed by "Strike" with columns `C Delta, C IV, C Bid, C Ask, C Last, Strike, P Last, ...`; flash on
Bid / Ask / Last; `stale_after=3`. The underlying goes in a cell above the strikes; conditional formats on the call
and put blocks shade the in-the-money sides (`=strike < spot` / `> spot`) and mark the at-the-money row. `sim.Market.
option_chain(strikes, days)` returns columns with these names, so `table.update(pd.DataFrame(...))` feeds it.
`ActiveWindow.CenterColumns = <the strike column>` keeps the strike in the middle when the window is narrower than the
chain: the call side scrolls outward on the left as the put side does on the right (set `SplitRow` first to keep the
header rows on top).

### Price ladder with click-to-trade - `dom_ladder.py`

A bound table keyed by "Price" (`My Bid | Bid | Price | Ask | My Ask`), recentred with `.replace({"Price": prices})`
when the last trade comes near an edge; conditional formats for the last price's row and red / green prices above /
below it; `interact.on_click` on the Bid column calls `om.submit("TXF", "buy", 1, price)`, on My Bid cancels. Click
actions run in the YCGrid host only (`_util.in_host()`).

### Blotter and positions - `examples/_util.py`

`blotter(ws, om, at, name)`: a bound table keyed by order id, fed by `om.on_order.append(show)`, rows colored by status
with conditional formats. `positions(ws, at, name)`: a bound table keyed by contract (Contract, Qty, Avg, Last, PnL) that
it only creates: the caller keeps it up to date with `.update(...)` (from `om.position` / `om.avg_cost`, e.g. in an
`on_fill` callback).

### Backtest and its report - `backtest_report.py`, `strategy_lab.py`, `pairs_trading.py`

The backtest is pandas: a target position per bar, held from the next open, P&L from the opens, costs on changes
(`positions_backtest`). `report(ws, equity, trades, prices)` writes the statistics, monthly returns with a color scale
and the trade list (a bound table), and opens candles with `signals`, equity and drawdown, linked with
`fig.link_table(name, "Time")`. The sweep is a loop with a heatmap; walk-forward is a loop over windows.

### From research to live - `live_strategy.py`

On each completed bar (`bb.add(...)` returns it), the backtest's own rule decides the target; the difference goes to
`om.submit`. The decisions are a bound table; the chart shows the candles, the averages and `signals` per order.

### Execution algorithms - `execution_algos.py`

A TWAP and an iceberg are classes of ten lines with a `step()` called from the loop; children go through the
OrderManager; progress, average price and slippage are a bound table; fills are `signals` on the chart.

### Order flow and quote boards - `order_flow.py`, `quote_board.py`, `depth_heatmap.py`

`rects` with labels draw footprint cells (sell x buy per price per bar), `barh` the volume profile, a `drag_line`
handle the limit order; column renderers draw size bars and trends in a quote table; `heatmap` with `extent` shows
resting liquidity over time.

## Research maths

The examples carry the maths they need in `examples/_math.py` (Black-Scholes price / Greeks / implied volatility,
an SVI fit, SMA / EMA / RSI, performance statistics, drawdown, monthly returns): copy them or use a package. Other
examples compute in place: rank IC and quintile portfolios (`factor_research.py`), Monte Carlo VaR / ES with
Cornish-Fisher (`monte_carlo_var.py`), a long-only frontier and Excel's Solver for maximum Sharpe
(`portfolio_optimizer.py`), a hedge ratio by the Analysis ToolPak and a half-life (`pairs_trading.py`), a scenario
grid by full revaluation (`risk_dashboard.py`, `options_desk.py`), an implied volatility surface (`vol_surface.py`).

## Orders, risk checks and gateways (`yc.trading`)

```python
from yc import trading
gw = trading.PaperGateway(spot=23000)              # simulated; a broker subclasses trading.Gateway
om = trading.OrderManager(gw, risk=trading.RiskLimits(max_qty=5, max_position=10, price_band=0.01, max_orders_per_sec=5))
om.on_order.append(lambda o: ...)                  # every order change (New, Pending, Working, PartiallyFilled, Filled, Cancelled, Rejected)
om.on_fill.append(lambda f: ...)                   # every fill
oid = om.buy("TXF", 2, 23010)                      # trading.Rejected("...reason...") when a check fails
om.cancel(oid); om.cancel_all(); om.flatten()      # flatten: cancel all and close every position (the kill switch)
om.position("TXF"), om.avg_cost("TXF"), om.orders(open_only=True), om.fills
om.submit("TXF", "sell", 1, type="stop", stop=22928)   # when the gateway supports "stop" (stop_limit: a price too)
om.modify(oid, price=23005)                           # when the gateway supports "modify"
```

These are building blocks. Brackets, OCO groups, P&L with costs, order journals and reconciliation are a few lines of
your own on top of `on_order` / `on_fill`, `Gateway.positions()` / `open_orders()` / `account()` and
`safety.alarm(name, text)` (an alarm that shows until cleared) - YCGrid doesn't ship them.

- `RiskLimits`: `max_qty` per order, `max_position` per symbol (counting working orders), `price_band` around the
  gateway's reference price, `max_notional` (with `multiplier`), `max_orders_per_sec`, `symbols` allowed, and
  `duplicate_window` (seconds: the same order as the last one sent, again within it, is refused; a click action runs
  on every click, so a double-click on an order button sends two orders without it).
- `on_fill` runs after the order manager has counted the fill: `position()` and `avg_cost()` there are the position
  after it (no average once it is flat). A screen that works out each fill's realized profit keeps the position and
  average before the fill itself, as `examples/trading_desk` does.
- Orders to a non-paper gateway are refused during a replay and outside operator mode (`require_operator=True`).
- Every order, cancel, fill and rejection goes to the change log; a callback that raises doesn't stop the orders.
- Threads: `on_order` / `on_fill` run on the thread that reports the change - the caller of `submit` / `cancel` /
  `modify` for the immediate New / Working / Cancelled reports, and the gateway's own thread for the rest
  (`PaperGateway`: its matching thread; a broker: its SDK's callback thread). Keep them short, take your own lock
  around shared state, and never wait inside one for another order event. `Topics.Subscribe` callbacks run on the
  subscription's worker thread, macros and panel buttons on the host's event loop: a desk touched from all three
  needs one lock, taken in one order.
- A gateway implements `submit(order)`, `cancel(order_id)` and optionally `last_price(symbol)`, and reports with
  `emit_order(order)` / `emit_fill(fill)` from its own threads (the OrderManager listens through
  `gw.subscribe(on_order=None, on_fill=None)`; other listeners may subscribe too). `supports` names what it does itself (`"stop"`,
  `"stop_limit"`, `"modify"` with `modify(order, price, qty, stop)`); the OrderManager refuses the rest rather than
  emulating them. A real broker also answers `positions()`, `account()`, `open_orders()`, `connected()`.
  `yc/gateways/template.py` is a skeleton (no SDK, no credentials); `trading.check_gateway(gw, symbol, far_price)`
  tests one against a paper market before it is trusted.
- `PaperGateway(spot, symbol, latency, step, seed, market, spread_ticks=0, slippage_ticks=0, fill_ratio=1.0,
  queue=False)` matches orders itself: market orders at the bid / ask (`spread_ticks` around the last price) plus a
  random adverse slippage (`slippage_ticks`: a number or `(low, high)`); limits when the price reaches them (`queue=True`:
  only when it trades through) at their limit, but a limit that is marketable on arrival (a buy above the ask) fills
  at the market, as on an exchange; `fill_ratio < 1` fills in parts; stops, stop-limits and modify are native.
  Its simulated market is one contract (`symbol`, TXF by default). **Options and other contracts trade at the quotes
  you give it**: `gw.quote("TXO202611C23000", bid, ask)` from the loop that shows the chain (every strike, call and
  put); their market orders fill at the ask / bid and their limits when the ask / bid reaches them. An order for a
  contract without a quote is rejected ("no price for ..."), never matched against the futures price.
  `set_speed(2)` runs the market twice as fast, `pause()` / `resume()`, `close()`; `positions()` / `open_orders()`
  answer like a broker's.
- Feeds (`yc.feeds`): subclass `Feed` (conflation by key, heartbeat, reconnect); `CsvTailFeed` and `WebSocketFeed` are
  references.

## Click actions and hotkeys (`yc.interact`, `Application.OnKey`)

```python
from yc import interact

@interact.on_click("DOM!C4:C33", buttons=("left", "right"))
def hit(ctx):                       # ctx: sheet_name, address, row, col, value, text, table, column, record, button, ctrl...
    send("buy" if ctx.button == "left" else "sell", ctx.record["Price"])

interact.on_key("^b", lambda ctx: send("buy", ctx.record["Strike"]))     # Ctrl+B on a row of a chain
Application.OnKey("{F9}", "risk.hedge_all")                              # Excel's OnKey; "" disables, None restores
ctx = interact.cell_context()                                            # the active cell's context
```

- Click actions fire on a mouse click only, never on arrow keys: a press and a release on the same cell, as a button's,
  so a drag that starts on the range (selecting cells) fires nothing. A handled right click opens no context menu.
- `on_click(target, fn=None, buttons=("left",))` returns the function (its id in `fn.click_id`);
  `interact.off_click(fn or id)` removes it, `interact.off_key(key)` gives the key its built-in action back.
- `ctx` (a dict whose items are also attributes): `sheet`, `sheet_name`, `row`, `col` (0-based), `address`, `value`,
  `text`; in a table also `table`, `column`, `table_row`, `record` (the row by column name); clicks add `button`,
  `double`, `ctrl`, `shift`, `alt`, hotkeys `key`; `ctx.range` is the cell as a Range.
- Key text: `^` Ctrl, `+` Shift, `%` Alt, then a letter, a digit or `{F5}`, `{ENTER}`, `{ESC}`, `{DEL}`, `{UP}`...

## Alerts (`yc.alerts`)

```python
from yc import alerts
alerts.add("Delta limit", "Risk!C5", ">", 500)
alerts.add("Spread wide", "Quotes[spread]", ">", 2.0, color="#ff9800", cooldown=10)
alerts.add("IV band", "Chain!C5:C40", "outside", 0.10, 0.60, sound=False)
alerts.add("Fill", "Blotter[Status]", "changed")
alerts.add("Margin", "Risk!F2", ">", 0.8, notify=True, flash_window=True)   # also a Windows notification

@alerts.on_alert
def on_alert(a):                    # a.name, a.message, a.address, a.value, a.record...
    ...
```

Conditions: `>`, `<`, `>=`, `<=`, `=`, `<>`, `between`, `outside`, `changed`. Rules are saved with the workbook and
evaluated by YCGrid (also without Python): cells meeting the condition are highlighted; once per `cooldown` a rule
beeps (`sound`), pops up inside YCGrid (`popup`), optionally flashes the taskbar button (`flash_window`) and shows a
Windows notification (`notify`), writes the change log and calls the handlers. Trading > Alert Rules shows the same
rules in a window.

## The simulator (`yc.sim`) and live bars (`yc.bars`)

```python
from yc import sim, bars
mkt = sim.Market(spot=23000, vol=0.18, seed=1)         # GBM with jumps, a smile, a book
mkt.step(0.25); chain = mkt.option_chain(strikes, days=7); bids, asks = mkt.book(levels=10)
bb = bars.BarBuilder(seconds=5, keep=1000)              # add(time, price, size) -> the bars it completed
```

- `sim.Market` attributes: `spot`, `last` (the futures' last trade, on the tick grid), `rate`, `tick`; `iv(strikes,
  days)` is the smile it prices with. `option_chain(strikes, days=7.0, spread_ticks=1.0, opt_tick=0.1)` returns numpy
  columns "Strike", "C Bid", "C Ask", "C Last", "C IV", "C Delta", "C Gamma", "C Vega", "C Theta", "C Vol", "C OI" and
  the same for "P".
- `sim.PaperExchange(mkt, latency=0.02, on_fill=None, on_order=None, fee=0.0, multiplier=200.0)` matches orders
  against a `Market` without the OrderManager (benchmarks, simple demos): `submit(symbol, side, qty, price=None)` ->
  order id (None = market), `cancel(id)`, `cancel_all()`, `poll()` after each `mkt.step()` returns the fills,
  `flatten()`, `pnl(symbol, mark=None)`; `position`, `orders` dicts. Screens use `trading.PaperGateway`.
- `bb.frame()` is a DataFrame (index: start time; Open, High, Low, Close, Volume, Ticks; the forming bar last unless
  `include_forming=False`); `bb.draw(ax, volume_ax)` draws or redraws candles (and volume).

Use the simulator for demos and tests; never point a screen at a live account without the user's consent.

## Examples (`examples\<name>`)

Complete screens to start from, each a project folder in YCGrid's `examples` folder (next to ycgrid.exe; `examples/` in
the source tree): open `examples\<name>\<name>.ycg` and its `Workbook_Open` builds the screen with simulated
data. `examples\README.md` lists them: `market_watch` (multi-symbol quotes in the terminal look: a bound table
with its header drawn in cells, colors against a Prev column, tabs as click actions), `option_chain` (the T-style
chain), `technical_chart` (candles, volume, MACD and KD as linked plots), `trading_desk` (one contract's trading
screen on one sheet), `backtest_report`, `vol_surface`, `factor_research`, `strategy_lab`,
`live_strategy`, `pairs_trading`, `portfolio_optimizer`, `monte_carlo_var`, `risk_dashboard`, `options_desk`,
`option_payoff`, `dom_ladder`, `execution_algos`, `depth_heatmap`, `live_candles`, `history_playback`, `tick_replay`,
`quick_plot`, `million_cells`, `order_flow`, `quote_board`, `big_data`, `py_cells`, `time_machine`. Read the closest
one's `main.py` before writing a similar screen. The code several share is in `examples\lib` (`ex_util`: the trading
look, sheet layout, blotter and positions tables; `ex_math`: Black-Scholes, implied volatility, SVI, performance
statistics, indicators; `ex_backtest`: the backtest and its report; `ex_trading`: the ladder and the option chain),
which each example's `ycgrid.toml` adds with `[python] paths = ["../lib"]`; a project of the user's own copies what it
needs into its own `lib\` (or names a shared folder the same way).
