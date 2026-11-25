# ADR 0002: parallel recalculation in waves, batched dependency propagation

- Status: accepted (M4)
- Related: PLAN §7.1 (index structures), §7.3 (recalculation algorithm), §7.7 (correctness)

## Context

Recalculation ran on the engine thread only: pending formulas were evaluated one by one in (sheet, row, col) order, and
a formula reading another pending cell threw `NeedCell`, which was computed first (explicit stack). Dependency
propagation queried the index once per changed cell, scanning the whole tile bucket each time.

PLAN §7.3 describes rank buckets: maintain a topological level per formula and evaluate each level with `parallel_for`.
Keeping ranks exact under edits, inserts, dynamic references and names is a large change to the document model.

## Decision

**Waves instead of ranks.** When at least `Document::kParallelMin` (512) formulas are pending:

1. Every pending formula without side effects is evaluated on the worker pool (`core::ParallelFor`, one thread per
   core minus two). Workers only read the model. A formula that reads a still-pending cell is flagged *blocked* and its
   result discarded; it doesn't throw, because C++ exceptions serialize across threads on Windows (measured: the
   throwing version was slower than the serial pass).
2. The engine thread writes the finished results back (single writer, hard rule 2) and the blocked cells form the
   next wave.
3. Waves stop when one finishes less than 1/8 of its cells (long chains such as running totals, circular references).
   The existing serial pass then handles whatever is left, in linear time, with circular-reference detection.

Formulas calling Python UDFs or streams (`kUdf`, `kStream`) never run in a wave: they have side effects (requests to
the host, subscriptions). RAND uses a per-thread generator.

**Batched propagation.** Changed cells of a round are grouped per tile into bitmaps (`ChangedCells`); every bucket of
the dependency index is scanned once per round and each entry is tested against the bitmaps (PLAN §7.1).

## Consequences

- 200,000 formulas over 80,000 inputs (bench `yc_unit_tests "[bench]"`, 24 threads): full recalculation
  324 ms -> 150 ms single-threaded (batched propagation) -> 74 ms with waves.
- Values are identical to the serial pass: `test_parallel_calc.cpp` compares every cell of a workbook with independent
  formulas, a 3,000-step chain, growing SUM ranges, lookups, errors and a circular reference, before and after edits.
- `Document::calc_threads = 1` forces the serial pass (tests, comparisons).
- Chains don't speed up (a chain is sequential by nature); wide sheets of independent formulas, the common shape of
  quote tables, do.
