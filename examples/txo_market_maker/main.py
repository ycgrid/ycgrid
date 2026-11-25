# SPDX-License-Identifier: Apache-2.0
"""TXO market making screen, built from YCGrid's pieces.

Sheets: Chain (the T-style option chain: a bound table the feed fills), DOM (a TXF price ladder with click-to-trade),
Risk (positions and limits), Trade (the blotter), Strategies (start / stop by clicking), Params (parameter cells).
feeds/market.py streams a simulated market in its own process; orders go through an OrderManager with risk limits to
a paper gateway (yc.sim), never to a real market. Trading > Operator Mode unlocks the parameters and the start / stop
clicks; Ctrl+Alt+K flattens everything. The chain, the ladder and the blotter are the examples' shared code in
../lib (ex_trading and ex_util, added by ycgrid.toml's [python] paths): read them to change the screen.
"""
import threading
import time

import pandas as pd

from yc import alerts, safety, trading
from yc.data import bind
from yc.excel import *
from yc.interact import on_click

from ex_trading import Ladder, chain_table
from ex_util import add_rule, blotter, dark_table, positions

STRIKES = list(range(21500, 24501, 100))
gw = trading.PaperGateway(spot=23000, seed=7, step=0.1)
om = trading.OrderManager(gw, risk=trading.RiskLimits(max_qty=5, max_position=20, price_band=0.01))
ui = {}


def sheet(name):
    for ws in Worksheets:
        if ws.Name == name:
            ws.Cells.Clear()
            ws.Cells.FormatConditions.Delete()
            return ws
    ws = Worksheets.Add(After=Worksheets(Worksheets.Count))
    ws.Name = name
    return ws


def param(cell):
    return int(Worksheets("Params").Range(cell).Value)


def Workbook_Open():
    params = sheet("Params")
    for i, (name, value) in enumerate([("Spread (ticks)", 4), ("Size", 2), ("Max position", 20)]):
        params.Cells(i + 2, 1).Value, params.Cells(i + 2, 2).Value = name, value
    safety.parameter(params.Range("B2"), name="Spread", type=int, min=1, max=50, max_step=5)
    safety.parameter(params.Range("B3"), name="Size", type=int, min=1, max=10)
    safety.parameter(params.Range("B4"), name="Max position", type=int, min=0, max=100)
    # the chain: the feed fills the bound table "Chain" and writes the futures price above the strikes (Chain!F3)
    chain = sheet("Chain")
    ui["chain"], ui["spot"] = chain_table(chain, "A4", "Chain")
    ui["chain"].replace(pd.DataFrame({"Strike": STRIKES}))
    # the ladder: click Bid / Ask sizes to trade, My Bid / My Ask to cancel
    dom = sheet("DOM")
    ui["ladder"] = Ladder(dom, at="B4", name="TXF")
    on_click(ui["ladder"].column(1), lambda ctx: order("buy", ctx))
    on_click(ui["ladder"].column(3), lambda ctx: order("sell", ctx))
    on_click(ui["ladder"].column(0), lambda ctx: cancel("buy", ctx))
    on_click(ui["ladder"].column(4), lambda ctx: cancel("sell", ctx))
    # risk: positions, and limits whose usage is a formula with data bars
    risk = sheet("Risk")
    ui["pos"] = positions(risk, at="A3", name="Positions")
    ui["limits"] = bind(risk, pd.DataFrame({"Name": ["TXF position", "Daily loss"], "Value": [0.0, 0.0], "Limit": [20, 500000]}),
                        key="Name", name="Limits", at="A8", formats={"Value": "#,##0", "Limit": "#,##0"}, record=False)
    dark_table(ui["limits"])
    risk.Range("D8").Value = "Used"
    used = risk.Range("D9:D10")
    used.Formula = "=IFERROR(ABS(B9)/C9,0)"
    used.NumberFormat = "0%"
    used.FormatConditions.AddDatabar()
    add_rule(used, "=D9>1", fill="#3a0000", font="#ff3b3b", bold=True)
    alerts.add("Limit breach", "Risk!D9:D10", ">", 1.0)
    blotter(sheet("Trade"), om, at="A2", name="Blotter")
    # strategies: click Action to start / stop (in operator mode)
    strat = sheet("Strategies")
    ui["console"] = bind(strat, pd.DataFrame({"Strategy": ["mm_txf"], "Status": ["Stopped"], "PnL": [0.0], "Position": [0], "Orders": [0],
                                              "Action": ["Start"]}), key="Strategy", name="Strategies", at="A2", record=False,
                         formats={"PnL": "[Red]#,##0;[Green]-#,##0;0"})
    dark_table(ui["console"])
    on_click(strat.Range("F3:F10"), toggle)
    ui["running"] = False
    safety.on_kill("main.kill")
    Worksheets("Chain").Activate()
    threading.Thread(target=loop, daemon=True, name="paper-trading").start()


def order(side, ctx, qty=None):
    price = (ctx.get("record") or {}).get("Price") if isinstance(ctx, dict) else ctx
    if price is None:
        return None
    try:
        return om.submit("TXF", side, qty or param("B3"), float(price), tag="DOM" if isinstance(ctx, dict) else "MM")
    except trading.Rejected as e:
        Application.StatusBar = f"Order refused: {e}"
        return None


def cancel(side, ctx):
    price = (ctx.get("record") or {}).get("Price")
    for o in om.orders(open_only=True):
        if o.side == side and o.price == price:
            om.cancel(o.id)


def toggle(ctx):
    if not safety.operator_mode():
        MsgBox("Strategies can be started and stopped in operator mode only.", vbExclamation, "Strategies")
        return
    ui["running"] = not ui["running"]
    if not ui["running"]:  # a stopped strategy leaves nothing in the market: its quotes would still fill
        for o in om.orders(open_only=True):
            if o.tag == "MM":
                om.cancel(o.id)
    safety.log("action", "strategy mm_txf", after="start" if ui["running"] else "stop")
    ui["console"].update({"Strategy": ["mm_txf"], "Status": ["Running" if ui["running"] else "Stopped"],
                          "Action": ["Stop" if ui["running"] else "Start"]})


def kill():
    """The kill switch: cancel every order and flatten every position at the market."""
    ui["running"] = False
    om.flatten()
    ui["console"].update({"Strategy": ["mm_txf"], "Status": ["Stopped"], "Action": ["Start"]})


def loop():
    hb = safety.heartbeat("paper trading", timeout=2)
    quotes = {}
    while True:
        spot = ui["spot"].Value
        if isinstance(spot, (int, float)):
            gw.market.spot = gw.market.last = round(spot)  # the paper market follows the feed (or a replay of it)
        last = float(gw.market.last)
        mine = [o for o in om.orders(open_only=True) if o.price is not None]
        ui["ladder"].update(last, *gw.market.book(levels=15), {o.price: o.qty - o.filled for o in mine if o.side == "buy"},
                            {o.price: o.qty - o.filled for o in mine if o.side == "sell"})
        q = om.position("TXF")
        if ui["running"]:
            spread, size, max_pos = param("B2"), param("B3"), param("B4")
            for side, px in (("buy", last - spread), ("sell", last + spread)):
                working = next((o for o in mine if o.id == quotes.get(side)), None)
                if working and working.price == px:
                    continue
                if working:
                    om.cancel(working.id)
                if (side == "buy" and q < max_pos) or (side == "sell" and q > -max_pos):
                    quotes[side] = order(side, px, size)
        else:  # stopped: a quote sent while it was stopping goes too
            for o in mine:
                if o.tag == "MM":
                    om.cancel(o.id)
        avg = om.avg_cost("TXF")
        pnl = (last - avg) * q * 200 if q and avg else 0.0
        ui["pos"].update({"Contract": ["TXF"], "Qty": [q], "Avg": [avg], "Last": [last], "PnL": [pnl]})
        ui["limits"].update({"Name": ["TXF position", "Daily loss"], "Value": [abs(q), max(0.0, -pnl)]})
        ui["console"].update({"Strategy": ["mm_txf"], "PnL": [pnl], "Position": [q], "Orders": [len(mine)]})
        hb.beat()
        time.sleep(0.25)
