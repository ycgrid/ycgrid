# Safety for live trading (`yc.safety`)

A trading screen must never show an old price as a live one, and a parameter must never change by accident. YCGrid
gives every workbook these tools; use them in any screen that drives real orders or real risk.

## What the user sees

- **Alarms**: when a heartbeat is late, the Python host crashes or stops answering, or a bound table with
  `stale_after` stops updating, the whole window is tinted red with a red border and a banner names the source and
  how long ago it stopped. It stays until the source recovers. Every alarm's start and end go to the change log.
- **Stale rows**: rows of a bound table not updated within `stale_after` seconds are grayed; `show_age=True` writes
  each row's age at the right of the table.
- **Parameter cells**: an orange corner marks them. The user changes them only by typing, only in operator mode;
  the type and range are checked and (by default) a confirmation shows the old and the new value. Pasting, filling
  and clearing don't change them.
- **Operator mode**: the status bar shows `OPERATOR` or `READ-ONLY` (click to switch; Trading > Operator Mode). A
  workbook can open read-only, ask for a PIN and drop back to read-only after an idle time. Read-only refuses every
  change from the screen (cells, formats, sheets, protection, rules, notes, names, objects, Undo) with a status bar
  message, and the commands that would change the workbook open no dialog; Python's writes go on.
- **Kill switch**: a red `KILL SWITCH` chip in the status bar, Trading > Kill Switch, or Ctrl+Alt+K; it asks once
  and runs the handler Python registered.
- **Change log**: Trading > Change Log lists every entry (filter, export to CSV); it is saved in the workbook.

## Python

```python
from yc import safety

# parameter cells: typed, range-checked, confirmed, logged
safety.parameter("Params!B2", name="Spread", type=float, min=0, max=5, max_step=0.5)
safety.parameter("Params!B3", name="Max lots", type=int, min=1, max=50)
safety.parameter("Params!B4", name="Side", choices=["Buy", "Sell", "Both"], confirm=False)
safety.parameter("Params!B5", name="Enabled", type=bool, on_change=lambda cell: restart_quoting())

# heartbeats: in a data feed or a strategy loop
hb = safety.heartbeat("TXO feed", timeout=1.0)                     # beats from a background thread
hb = safety.heartbeat("pricer", timeout=2.0, alive=lambda: pricer.ok, detail=lambda: f"{pricer.n} quotes")
hb.beat()                                                            # an extra beat now
hb.stop()                                                            # stop and take it off the screen

# bound tables: gray rows and an alarm when the data stop
t = ws.bind(df, key="strike", stale_after=2.0, show_age=True)

# the kill switch runs a macro of the Python host (no arguments)
safety.on_kill("risk.flatten_all")
safety.kill()                                                        # from code (logged as "python")

# modes and settings
safety.settings(start_read_only=True, pin="2468", operator_idle_s=900, host_timeout_s=3)
safety.operator_mode()                                               # True / False
safety.set_operator_mode(True, pin="2468")

# the change log: parameters (any source), modes, kill switch, alarms, and what you log
safety.log("order", "TXO 202611 23000C", after="buy 2 @ 101.5", user="mm-bot")
for e in safety.change_log(limit=50):
    print(e["time"], e["kind"], e["target"], e["before"], "->", e["after"], e["source"])
safety.export_change_log(r"C:\logs\changes.csv")

safety.status()   # {"operator": True, "alarms": [...], "heartbeats": [{"name", "age", "timeout", "detail"}]}
```

| Function | Notes |
|---|---|
| `parameter(target, name, type, min, max, choices, confirm=True, max_step=0, on_change=None)` | `type`: `float`, `int`, `str`, `bool`, `"list"` (with `choices`), `None` (anything) |
| `remove_parameter(target)`, `parameters()` | |
| `heartbeat(name, timeout=2.0, alive=None, detail=None)` -> `Heartbeat` | `beat()`, `stop(remove=True)`; one-off: `beat(name, timeout)` |
| `log(kind, target, before="", after="", user=None)` | kinds by convention: `order`, `fill`, `cancel`, `action`, `model` |
| `alarm(name, text)`, `clear_alarm(name)`, `alarms()` | your own alarm (positions differ from the broker, a feed's sequence gap...): shown with the others as "name: text" until cleared; raising it again replaces the text; `alarms()` lists every alarm showing |
| `change_log(limit=0)`, `export_change_log(path)` | entries: `time` (datetime), `user`, `kind`, `target`, `before`, `after`, `source` |
| `on_kill(macro)`, `kill(ask=False)` | the macro runs in the Python host |
| `settings(start_read_only, pin, operator_idle_s, kill_macro, host_timeout_s)` | returns the current settings; `pin=""` removes the PIN |
| `operator_mode()`, `set_operator_mode(on, pin="")`, `status()` | |

## Rules for agents

- Every value a strategy reads from the sheet (spreads, sizes, limits, switches) is a parameter cell.
- Every process that produces data the trader relies on beats a heartbeat with a timeout shorter than the time a
  stale value could do harm.
- Every bound table of market data has `stale_after`.
- Every screen that can send orders registers a kill switch and logs its orders with `safety.log`.
- Test with simulated orders; don't press the kill switch or send orders against a live account without the user's
  consent.
