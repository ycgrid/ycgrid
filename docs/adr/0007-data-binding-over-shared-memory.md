# ADR 0007: Data binding over shared memory, and a pythonic layer next to the Excel object model

- Status: accepted
- Related: [ROADMAP.md](../ROADMAP.md) Phase 1, PLAN §4.4 (input channels), §13.3 (YCP), §13.4 (fast lane),
  `engine/bind.h`, `engine/document_bind.cpp`, `apps/ycgrid/python_bridge_data.cpp`, `python/yc/data.py`

## Context

Trading screens show tables keyed by an instrument or a strike (option chains, quote boards, positions) that change
many times a second. Until now Python reached cells in two ways: `Range.Value` writes (msgpack over the named pipe,
cell by cell) and topics (`Topics.Push`, also over the pipe, one string name per value). Both top out around 1–2
million cell updates per second, and both make Python find the cells itself.

The roadmap asks for `sheet.bind(df, key="strike")`, updates that only send the keys and columns that changed, column
formats declared once, and a shared-memory, columnar transfer. PLAN §13.4 had planned a shared-memory Topic table with
a native extension module, which was never built.

## Decision

1. **Bound tables.** `ws.bind(data, key=...)` creates an ordinary table (ListObject) and remembers the binding in the
   workbook (`BoundTable`, BIND chunk). The engine keeps key → row maps (integer keys in their own map) and writes
   only the cells a frame carries. New keys append rows; deleted keys remove them; when the user sorts the table the
   map is rebuilt from the key column. Because the data are cells of a table, formulas (`=SUM(Chain[bid])`),
   structured references, charts, filters, conditional formats and PivotTables work unchanged.
2. **Column frames in shared memory.** Each connection gets one named file mapping (64 MB) used as a ring. A frame
   is laid out like an Apache Arrow record batch: per column a values buffer (float64, int64, bit-packed booleans) or
   int32 offsets plus UTF-8 bytes, and an optional validity bitmap; everything little endian and 8-byte aligned. The
   Python side writes frames with `mmap` and sends `bind.frame {pos, len, end}` through the pipe; YCGrid decodes the
   frame (copying it out), stores `end` in the ring header so the space can be reused, and hands the rows to the
   engine. No native extension and no pyarrow are needed; numpy makes encoding fast.
3. **Merging and backpressure.** A frame that repeats the previous waiting frame's keys and columns replaces it (the
   latest values win, like the topic conflation table). When more than about a million cells wait, the bridge stops
   reading that connection's frames; the ring fills and the Python sender waits. Python also limits the bytes it has
   in flight (8 MB), so a fast sender is paced by YCGrid instead of queueing seconds of data. A long engine task stops
   when the publish interval is due, so the screen keeps updating while data arrive.
4. **Formats once.** Each declared column carries a number format (tick size, decimals, percent, basis points with
   the new `[mul=10000]` format token) and flash settings; the engine applies them to every row, including rows added
   later.
5. **Two API layers.** The Excel object model stays as it is (PascalCase, Excel's names). A pythonic layer for heavy
   users uses snake_case: `ws.bind`, `yc.data.bind`, `BoundTable.update / delete / replace`. Both write the same
   cells through the same engine.
6. **Latency is measured end to end.** A frame carries the sender's `perf_counter_ns` (QueryPerformanceCounter); the
   UI records, after `Present`, how old the newest data on screen are. `scripts/bench.py` reports it with throughput,
   frame rate and memory.

## Consequences

- Measured on the README's test machine (one million cells, 100,000 rows x 10 columns, publish interval 16 ms):
  - whole-table updates: about 45 million cells per second written;
  - tick-by-tick updates of 1% of the rows at 500 updates per second: p50 12 ms, p99 32 ms from Python to the screen;
  - 10% of the rows at 100 updates per second (11 million cells per second): p50 21 ms, p99 40 ms.
- The cost that remains is copy-on-write: after each publish the first write to a tile copies it (4,096 cells), so
  random updates over a large table cost about one table copy per publish. A longer publish interval trades latency
  for throughput (50 ms: 31–56 million cells per second).
- Bound data are cells: a saved workbook keeps the last values; the binding itself is saved, the stream is not.
- `Range.Value`, `Range.BindTopics`, `=QUOTE()` and `Topics.Push` keep working and remain the right tools for single
  values and Excel-style code.
- Topics still go through the pipe. Moving them to the shared ring is a later step if a workload needs it.
