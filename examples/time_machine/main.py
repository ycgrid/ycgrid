# SPDX-License-Identifier: Apache-2.0
"""Time machine: a click-to-trade ladder with live candles is recorded whole - the market data, the app's own
positions and orders, the chart and a marker for every order and fill - then replayed: step back to any fill on the
timeline, pin a moment and compare it with another.

Open time_machine.ycg in YCGrid: Workbook_Open below builds the screens, with simulated data. The code the examples
share is in ../lib (ex_util, ex_math, ex_backtest, ex_trading), which ycgrid.toml's [python] paths adds.
"""
from __future__ import annotations

import os
import tempfile
import time

from yc import excel, replay

from ex_util import load_example, thread

# the two examples this one records, built from their own folders (../dom_ladder/main.py, ../live_candles/main.py)
dom_ladder = load_example("dom_ladder")
live_candles = load_example("live_candles")


def run(seconds: float = 20.0):
    """Builds the ladder and the candles, records everything for `seconds` (paper orders are sent meanwhile), then
    starts the replay of the recording."""
    om = dom_ladder.run(seconds=seconds + 2)
    _, candles = live_candles.run(seconds=seconds + 2)
    # the ladder in front with the candles docked beside it: the candles' sheet, opened last, and its floating chart
    # hid the ladder whose fills the replay is about
    excel.Worksheets("Ladder").Activate()
    candles.Dock("right", Ratio=0.5)
    path = os.path.join(tempfile.gettempdir(), "ycgrid_time_machine.yctape")
    replay.record(path, everything=True)
    print(f"[example] recording everything for {seconds:g} s to {path} (orders are sent on the paper exchange)")

    def trade_then_replay():
        end = time.time() + seconds
        side = "buy"
        while time.time() < end:
            try:
                om.submit("TXF", side, 1, None)  # a market order: a fill and a marker on the timeline
            except Exception:  # noqa: BLE001 - a risk limit refused it
                pass
            side = "sell" if side == "buy" else "buy"
            time.sleep(seconds / 6)
        replay.stop_recording()
        replay.play(path, speed=2.0)
        print("[example] replaying at 2x: hover the markers on the replay bar, click one to go there, Pin This Moment / Compare")

    thread(trade_then_replay, "example-time-machine")
    return path


def Workbook_Open():
    run()
