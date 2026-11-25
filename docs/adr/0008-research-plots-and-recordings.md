# ADR 0008: Research plots outside the workbook, and recordings that own their tables

- Status: accepted
- Related: [ROADMAP.md](../ROADMAP.md) Phase 4, [ADR 0007](0007-data-binding-over-shared-memory.md),
  `apps/ycgrid/figures_ui.cpp`, `apps/ycgrid/replay.cpp`, `engine/tape.h`, `python/yc/plot.py`, `python/yc/replay.py`

## Context

Researchers plot from scripts (matplotlib's `plt.plot`) and want the result in front of them at once, with millions of
points and no workbook ceremony. Excel charts already exist in YCGrid (`ChartObjects`, saved with the workbook, fed by
cells) but they are the wrong tool for ten million points from a numpy array.

Traders want to run yesterday's session through today's screen: the same tables, formulas, alerts and strategy code,
fed by recorded data. The roadmap's gate: the market-making screen switches between live and replay without code
changes.

## Decision

1. **Figures are session objects, not workbook objects.** `yc.plot` figures are dockable ImPlot windows owned by the
   running YCGrid, not saved in the `.ycg` and not restored from saved window settings. Their data travel through the
   shared-memory ring of ADR 0007 as column frames (decoded into plain arrays, no `Value` per point). Lines keep a
   min / max pyramid so a frame costs the same for a thousand or ten million points; LTTB is available on request.
   Excel charts remain the way to put a chart in a workbook.
2. **Times are local wall-clock times.** Dates without a time zone (numpy, pandas, `datetime`) mean local time, like
   the sheet's date serials; plots convert them to Unix seconds and show local time, and linking a plot to a table
   converts the table's date serials.
3. **A recording is a tape of frames.** A `.yctape` holds, after a snapshot of the recorded bound tables, every frame
   that reached them and every batch of topic updates, each with its arrival time (a new record kind can be added
   without breaking older readers, which skip unknown kinds). Tables are recorded when marked `record` (the default);
   tables the application keeps itself (positions, orders, strategy state) are bound with `record=False`.
4. **A replay owns its tables and topics.** While it runs, live frames for the tables on the tape and live pushes for
   its topics are dropped; replayed frames go through the same inbox as live ones with their send time set to now, so
   freshness checks and latency stay meaningful. Seeking replays from the snapshot without waiting. Going back to live
   restores the tables as they were before the replay. Orders from `yc.trading` to a non-paper gateway are refused
   while a replay runs.

## Consequences

- A screen built on bound tables and topics runs on recorded data unchanged; screens that write cells with
  `Range.Value` are not recorded (they are the application's own state, like `record=False` tables).
- Figures don't survive a restart; a script that needs a lasting chart in the workbook uses Excel charts.
- A recording only contains what reached YCGrid; it is not a market data archive (raw feeds stay with the feed
  process).
