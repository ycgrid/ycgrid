# SPDX-License-Identifier: Apache-2.0
"""The simulated market of the TXO sample, in its own process (ycgrid.toml [[python.feeds]]).

It streams the futures price into Chain!F3 (above the strikes) and the option chain (bid, ask, last, IV, delta for
calls and puts) into the bound table "Chain" four times a second, and beats a heartbeat: if this process
stops, YCGrid turns red and names it. Replace the simulator with your market data API to go live.
"""
import time

import yc
from yc import data, excel, safety, sim

yc.connect()
mkt = sim.Market(spot=23000, vol=0.18, seed=11)
while True:  # the host builds the screen when the workbook opens
    try:
        chain = data.table("Chain")
        break
    except yc.YcError:
        time.sleep(0.5)
hb = safety.heartbeat("market feed", timeout=1.5)
spot = excel.Worksheets("Chain").Range("F3")
strikes = list(range(21500, 24501, 100))
fields = ["C Delta", "C IV", "C Bid", "C Ask", "C Last", "P Last", "P Bid", "P Ask", "P IV", "P Delta"]
n = 0
while True:
    mkt.step(0.25)
    q = mkt.option_chain(strikes, days=12)
    chain.update({"Strike": q["Strike"], **{f: q[f] for f in fields}})
    spot.Value = mkt.last
    hb.beat(f"{mkt.last:,.0f}")
    n += 1
    time.sleep(0.25)
