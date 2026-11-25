# SPDX-License-Identifier: Apache-2.0
"""Backtest report: a moving-average crossover on simulated daily futures prices, and a report built from the pieces.

The backtest is a few lines of pandas (positions_backtest() in ../lib/ex_backtest.py). The report is made of YCGrid's building blocks, no ready-made
screen: statistics written to cells, monthly returns with a color scale, the trade list as a bound table, and a chart
window with candles, buy / sell marks (Axes.signals), equity and drawdown, linked to the table (Figure.link_table):
click a trade to mark it on the charts, click a chart to find the trade. report() is in ex_backtest too: the
strategy lab, pairs trading and live strategy examples use it.

Open backtest_report.ycg in YCGrid: Workbook_Open below builds the screen, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

from ex_backtest import crossover, positions_backtest, report, simulate_prices
from ex_util import fresh_sheet


def run(fast: int = 20, slow: int = 60):
    prices = simulate_prices()
    equity, trades = positions_backtest(prices, crossover(prices, fast, slow))
    bench = 4_000_000.0 + (prices["Close"] - prices["Close"].iloc[0]) * 200.0
    ws = fresh_sheet("Backtest")
    stats, table, fig = report(ws, equity, trades, prices, bench.rename("Buy and hold"),
                               title=f"MA {fast}/{slow} crossover, simulated TXF (1 contract)", name="Trades")
    print(f"[example] CAGR {stats.get('CAGR', float('nan')):.2%}, Sharpe {stats.get('Sharpe', float('nan')):.2f}, "
          f"max drawdown {stats.get('Max Drawdown', float('nan')):.2%}, {len(trades)} trades")
    return stats, table, fig


def Workbook_Open():
    run()
