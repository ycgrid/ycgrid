# YCGrid examples

Every folder here is a YCGrid project: a workbook (`<name>.ycg`) with its Python code beside it (`main.py`, the
workbook's ThisWorkbook). Open the workbook in YCGrid (double-click it, File > Open, or `python -m yc open
examples\<name>\<name>.ycg`) and its `Workbook_Open` builds the screen. Every example uses simulated data and a
paper exchange, never a real market or account. They are meant to be read, copied and changed: copy a folder
together with `lib` and make it yours.

The examples are compositions of YCGrid's building blocks (the Excel object model, `yc.data`, `yc.plot`,
`yc.trading`, `yc.replay`, ...), not features of YCGrid: the screens themselves live here, not in the library.

## Projects

| Folder | What it is |
|---|---|
| [demo](demo/main.py) | A quote table with a button, a combo box, charts, a custom panel, conditional formatting, data validation, a filter and live updates; `sheets/Sheet1.py` holds the control events. |
| [txo_market_maker](txo_market_maker/main.py) | A complete TXO market-making screen: the option chain, a TXF ladder with click-to-trade, risk limits, the blotter and a strategy console, trading through an OrderManager with risk limits; the market streams from its own process (`feeds/market.py`). |

## Research and trading screens

| Folder | Title | What it builds |
|---|---|---|
| [market_watch](market_watch/main.py) | Market watch | Multi-symbol quotes in the style of Taiwanese trading terminals: prices colored against the previous close, change arrows, limit rows filled, category tabs, and the selected symbol's intraday line, average price and volume. |
| [option_chain](option_chain/main.py) | Option chain | The T-style TXO chain (calls, strike, puts) with two header rows drawn in cells, the money side shaded, the at-the-money strike lit and expiry tabs; the smile of every expiry and the open interest by strike. |
| [technical_chart](technical_chart/main.py) | Technical chart | Daily candles with moving averages and Bollinger bands, volume, MACD and KD as four linked plots docked beside the bars' table; a click on a day marks it. |
| [trading_desk](trading_desk/main.py) | Trading desk | One sheet for one contract: a watch list, order buttons, a click-to-trade ladder, the position and the fills, with live candles marking every fill and the average price. |
| [backtest_report](backtest_report/main.py) | Backtest report | A moving-average strategy on simulated futures: statistics, monthly returns, the trade list linked to price, equity and drawdown charts. |
| [vol_surface](vol_surface/main.py) | Volatility surface | Implied volatilities from option prices, an SVI fit per expiry, a 3-D surface and the smiles. |
| [factor_research](factor_research/main.py) | Factor research | A cross-sectional factor on 300 stocks: quintile portfolios, information coefficients and ToolPak statistics. |
| [strategy_lab](strategy_lab/main.py) | Strategy lab | A parameter sweep of a moving-average strategy with a Sharpe heatmap, checked out of sample against overfitting, with indicators. |
| [live_strategy](live_strategy/main.py) | Research to live trading | The strategy lab's moving-average rule, unchanged, trading on paper on live bars: each decision, the position, and a check that live matches the backtest. |
| [pairs_trading](pairs_trading/main.py) | Pairs trading | A hedge ratio by regression (Analysis ToolPak), the spread's z-score and a mean-reversion backtest report. |
| [portfolio_optimizer](portfolio_optimizer/main.py) | Portfolio optimizer | Mean-variance frontier of eight assets, the covariance matrix by the ToolPak and the maximum Sharpe portfolio found by Solver on the sheet. |
| [monte_carlo_var](monte_carlo_var/main.py) | Monte Carlo VaR | 100,000 correlated scenarios of a portfolio: the P&L distribution, VaR and expected shortfall. |
| [risk_dashboard](risk_dashboard/main.py) | Option risk dashboard | Option positions with Greeks, limits, and a spot x volatility scenario grid of P&L. |
| [options_desk](options_desk/main.py) | Options desk | A live option chain, an iron condor opened at its mids, the P&L at expiry and today redrawn as the market moves, the strategy's Greeks and a spot x volatility scenario grid. |
| [option_payoff](option_payoff/main.py) | Option payoff builder | A strategy's legs in cells and its payoff curve as formulas and an Excel chart: change a strike and the chart follows. |
| [dom_ladder](dom_ladder/main.py) | Click-to-trade ladder | A depth-of-market ladder on a simulated book: click the sizes to buy and sell against a paper exchange; positions and the blotter follow. |
| [execution_algos](execution_algos/main.py) | Execution algorithms | A TWAP and an iceberg, a few lines each in the example, working orders on a paper market: progress against the schedule, average price and slippage, every child order in the blotter. |
| [depth_heatmap](depth_heatmap/main.py) | Market depth heatmap | Resting liquidity by price over time as a live heatmap, with the last price and the trades drawn over it and the traded volume below. |
| [live_candles](live_candles/main.py) | Live candles | Simulated ticks aggregated into 5-second bars and drawn live with an EMA, volume and RSI. |
| [history_playback](history_playback/main.py) | History playback | A CSV of past ticks played into a live quote table and one-minute candles at 60x by a small player with pause, resume and seek. |
| [tick_replay](tick_replay/main.py) | Live quotes, record and replay | Twenty symbols streamed into a bound table with a live chart; eight seconds are recorded and replayed into the same screen. |
| [quick_plot](quick_plot/main.py) | Fast plots | Ten million points, a million-value histogram and a correlation heatmap, drawn in milliseconds. |
| [million_cells](million_cells/main.py) | A million live cells | A 100,000 x 10 bound table updated as fast as possible, with throughput and latency. |
| [order_flow](order_flow/main.py) | Order flow | A live footprint chart (sell x buy volume at each price of each bar), the volume profile with its point of control, VWAP and a limit order you drag on the chart. |
| [quote_board](quote_board/main.py) | Quote board | Forty symbols streaming into a table that draws its own size bars, change bars, volume heat and trend lines every frame. |
| [big_data](big_data/main.py) | Big data | Twenty million ticks drawn as one line and opened in a Data View that scrolls, sorts and filters them all, straight from column files. |
| [py_cells](py_cells/main.py) | Python in cells | =PY formulas run pandas and numpy on ranges and spill the results: statistics, correlations, a regression, rolling means. |
| [time_machine](time_machine/main.py) | Time machine | A trading screen recorded whole (market data, positions, orders, the chart, a marker per fill) and replayed: jump to any fill, compare two moments. |

## Shared code: `lib`

`lib` holds the code several examples use: `ex_util.py` (the trading look, sheet layout, a blotter and a positions
table, `load_example`), `ex_math.py` (Black-Scholes, implied volatility, SVI, performance statistics, indicators),
`ex_backtest.py` (simulated prices, a positions backtest and the backtest report) and `ex_trading.py` (the
click-to-trade ladder and the T-style option chain). Each example's `ycgrid.toml` adds it to the project's imports:

```toml
[python]
paths = ["../lib"]
```

`python -m yc pack <folder>` puts these files into the packed project's own `lib` folder, so a packed example runs
anywhere.
