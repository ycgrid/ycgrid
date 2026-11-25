# Live tables: binding DataFrames by key (`ws.bind`)

For tables that change many times a second (option chains, quote boards, positions, order books, research results
that update), bind the data instead of writing `Range.Value` in loops. A bound table is an ordinary table
(ListObject) whose rows Python updates by key through shared memory: only the keys and columns that changed travel,
and YCGrid finds the rows, writes the cells and recalculates what depends on them.

This is the pythonic layer (snake_case). The Excel object model (`Range.Value`, `Range.BindTopics`, `=QUOTE()`)
works on the same cells; use it for single values and for code ported from Excel.

## Binding

```python
import pandas as pd
from yc.excel import *

chain = pd.DataFrame({"strike": strikes, "bid": bids, "ask": asks, "iv": ivs, "delta": deltas})
t = Sheets("Chain").bind(
    chain, key="strike", name="TXO", at="B3",
    formats={"bid": {"tick": 0.1}, "ask": {"tick": 0.1}, "iv": "pct1", "delta": "0.000", "edge": "bp"},
    flash=["bid", "ask"],          # up / down colors when the values change
    stale_after=3,                 # rows not updated for 3 s turn gray
)
```

| Parameter | Meaning |
|---|---|
| `data` | pandas or polars DataFrame, pyarrow Table, dict of columns (lists or numpy arrays), list of dicts, numpy record array; `None` with `columns=[...]` for an empty table |
| `key` | the column rows are found by (int, float or text); without it `update()` appends |
| `name` | the table's name (structured references: `=SUM(TXO[bid])`); a binding with the same name is replaced |
| `at` | the header row's top-left cell (`"B3"` or a Range) |
| `formats` | per column: `{"tick": 0.05}`, `{"decimals": 3}`, `"pct"` / `"pct1"`, `"bp"` / `"bp2"` (basis points), `"int"` / `"thousands"`, `"price2"`, `"date"`, `"datetime"`, `"time"`, or an Excel format code |
| `render` | `{column: "bar" \| "bar2" \| "heat" \| "progress" \| "trend"}` or `{"type", "color", "neg", "max", "length"}`: drawn by YCGrid over the cells every frame (size bars, two-sided bars around zero, heat colors, progress, each row's recent values as a line; hide a trend column's number with the format `";;;"`); `BoundTable.set_render(...)` changes them; saved with the workbook |
| `flash` | `True` (every column), a list of columns, or `{column: {"mode": "updown" or "change", "up": "#e5383b", "down": "#16a34a", "ms": 600}}` (default red up, green down) |
| `stale_after` | seconds without an update after which a row is drawn gray (0 = never) |
| `record` | market data, recorded and replayed (Trading > Record, `yc.replay`); `False` for tables the app keeps itself (positions, orders, strategy state), which stay live during a replay |
| `show_age` | write each row's age (seconds since its last update) at the right of the table ([safety.md](safety.md)) |
| `columns` | the column names when `data` is `None` (an empty table filled later: `ws.bind(None, key="Symbol", columns=["Symbol", "Bid", "Ask"])`) |
| `replace` | `True` (default): a binding with the same name is replaced, its table and cells removed |

Dates (`datetime`, `datetime64`) become Excel dates with a date format; NaN and `None` are empty cells.

## Updating

```python
t.update(changed)                          # DataFrame of changed rows: only these keys and columns change
t.update(strike=[23000, 23100], bid=[101.5, 80.2])
t.update({"strike": keys, "bid": bid_np, "ask": ask_np})   # numpy arrays: fastest
t.append(rows)                             # add rows without key lookup
t.delete([22800, 22900])                   # remove rows by key
t.replace(df)                              # the rows become exactly these, in this order
r = t.update(df, wait=True)                # wait for the write: {"rows", "added", "removed", "cells"}
Player(ticks_df, t, speed=20).start()     # history rows (a "time" column) into the table: a class of the example
                                           # examples/history_playback/main.py (YCGrid's examples folder)
t.sync(positions_df)                       # a full snapshot every time: only new keys, changed cells and removed
                                           # keys are sent ({"added", "changed", "removed", "cells"});
                                           # delete_missing=False keeps keys the snapshot leaves out
t.last_error()                             # errors of updates sent without waiting ("" = none)
```

- Updates are asynchronous by default: `update()` returns as soon as the frame is in shared memory. A fast sender is
  paced by YCGrid (it waits when YCGrid is behind), and a frame that repeats the waiting frame's keys and columns
  replaces it, so the screen always shows the newest data.
- New keys add rows at the end; columns that the table doesn't have yet are added at the right.
- Formulas next to the bound columns, inside the table (`=[@bid]-[@ask]`), recalculate when their row changes.
- Sorting or filtering the table in the UI is fine: keys are found again.

## Changing the look later

```python
t.set_format(iv="pct2", edge="bp")
t.set_flash(["last"], up="#b71c1c", down="#1b5e20", ms=400)
t.stale_after = 5
```

## Reading back and other members

```python
df = t.to_frame()          # pandas DataFrame through shared memory (a dict of columns without pandas)
df = yc.data.read("Trades") # any table (ListObject): dates as datetime64, whole numbers as int64
df = Range("A1:F5000").to_frame()          # any range (first row = names; A:F stops at the used range)
ws.write_frame(df, at="H1", index=False)   # plain cells, not a live table; dates get a date format
# both go through shared memory: a million cells in well under a second
t.list_object               # the Excel ListObject: Range, DataBodyRange, ListColumns...
t.range                     # the whole table as a Range
t.columns                   # column names
t.age                       # seconds since the newest row update (None before any data): freshness checks
t.record                    # market data (recorded and replayed); settable, like t.stale_after
t.definition()              # the binding as YCGrid holds it: name, sheet, key, columns, stale_after, show_age, record, age
yc.data.table("TXO")        # an existing binding (after reopening the workbook)
yc.data.tables()
t.unbind(delete=False)      # stop binding (delete=True also removes the table)
```

## Recording and replay (`yc.replay`)

Trading > Record writes every frame reaching the bound tables marked `record` (the default) to a `.yctape` file in
the workbook's `recordings` folder, after a snapshot of those tables. Trading > Replay feeds a recording into the same
tables at its recorded pace (0.25x to 50x, or as fast as possible), with a replay bar to pause, seek and loop. While
a replay runs, live frames for its tables are ignored, so the screen and the Python code reading it run on recorded
data unchanged; Back to Live restores the tables as they were. The status bar shows `REC` and `REPLAY`.

```python
from yc import replay
path = replay.record()                    # or record("day1.yctape", tables=["Chain"])
replay.stop_recording()
replay.play(path, speed=5, start=60, loop=False)
replay.pause(); replay.resume(); replay.seek(120); replay.set_speed(0); replay.set_loop(True)
replay.status()                           # mode ("live", "replay", "paused", "ended"), position, length, speed, frame, frames...
replay.wait(timeout=30)                   # until the end of the recording
replay.live()
if replay.is_replaying():                 # e.g. send orders to a paper account
    ...
```

Market data topics (`Application.Topics.Push`, `=QUOTE()` cells) are recorded with all tables (or with
`tables=["@topics", ...]`) and replayed the same way: live pushes for the recorded topics are ignored meanwhile.

Seeking replays from the snapshot without waiting, so the tables show their state at that time. Replayed frames get
the time they are replayed as their send time: freshness (`stale_after`) and latency work as with live data.

## Performance

Measured with `scripts\bench.py` (one million cells, 100,000 rows x 10 columns, publish interval 16 ms):

| Load | Cells written / s | Python -> screen p50 / p99 |
|---|---:|---:|
| whole table, as fast as possible | ~45 million | ~90 / 120 ms (queueing) |
| whole table, 20 updates / s | 22 million | 41 / 59 ms |
| 10% of the rows, 100 updates / s | 11 million | 21 / 40 ms |
| 1% of the rows, 500 updates / s | 5.5 million | 12 / 32 ms |

- Send numpy arrays (or pandas / polars columns); plain lists are encoded in Python and are several times slower.
- Send only what changed: rows of changed keys, and only the changed columns.
- Batch: one `update()` with 1,000 rows is far cheaper than 1,000 updates of one row.
- `Application.PublishInterval` trades latency for throughput: 16 ms for the lowest latency, 50 ms when a large
  table changes everywhere at once.
- Data feed processes (`[[python.feeds]]`) are the right place for streams; the host stays free for events.
