# Random testing: how it runs, what it found, and when it is done

Random testing finds what nobody thought to try. Left alone it also never says when to stop: a round that finds
nothing proves little, and one that finds something may only have met code written last week. This page is the
process that keeps it honest: every round is logged, every finding is classified, a finding is closed by closing its
class, and a stopping rule says when the curve has flattened.

## The tools

| Tool | What it does | Speed | What it checks |
|---|---|---|---|
| `tests/fuzz/doc_fuzz.cpp` (`yc_doc_fuzz`) | the Document in-process, driven by a byte stream | thousands of steps a second | Undo gives back what was there, Redo what the step made, a step that changed something left an Undo step, a `.ycg` save and open (with a full recalculation) gives back the same content, nothing throws; with `--xlsx` (three rounds in four) an Excel copy saved and opened gives back every cell's formula, value, format, merged area and size while the workbook holds nothing the cells depend on that the copy leaves out, and three damaged copies of it open as an error or a workbook (no crash, hang or out of memory); with `--more` (every other round) twelve more operations: moving and hiding sheets, Paste Special, borders, fonts, hidden lines, Replace, Remove Duplicates, Fill Right, Clear, Subtotal, outlines and notes; with both `--wide` and `--more` (one round in eight) a range may also go from a cell to the sheet's last cell, as Shift+click there or Ctrl+Shift+End selects |
| `scripts/chaos.py` | a hurried person at a hidden YCGrid: clicks, keys, ribbon, dialogs, docking | ~500 actions in 6 minutes | no crash, no freeze, no ImGui assert, no stuck dialog or dock |
| `scripts/basics.py --sequences` | random sequences of basic operations in every state | 60 sequences in 3 minutes | Undo / Redo exact, read-only changes nothing, only Excel's messages |
| `scripts/calc_fuzz.py` | random edits through Python | seconds | recalculation against `CalculateFull`, Undo, `.ycg` and `.xlsx` round trips |
| `scripts/formula_fuzz.py` | random formulas | 600 in a second | answers worked out in Python with Excel's rules |
| `scripts/gaps_fuzz.py` | random scenarios over the object-model members of the Excel gap list (PivotTable areas and cells, Top N, Show Pages, panes, windows, OpenText and Text to Columns' delimiters, DisplayFormat, the 1904 date system, precision as displayed, a sheet's calculation off, threads, Chart.Shapes, ListColumn.Total, Consolidate), in either UI language | 600 rounds in 90 seconds | every value worked out in Python from the scenario (a report cell is the aggregate of the records its items name, a Top N shows the N largest totals, a text field is the file's field), and the rules measured in Excel 16 (frozen panes' numbering, a one-value report's width, consecutive delimiters) |
| `yc_doc_fuzz --gaps` | the `--more` operations and the calculation settings: the 1904 date system, precision as displayed and its formats, a full calculation every time, a sheet's calculation off and on, dates, and a calculation interrupted part way then finished | as doc_fuzz | as doc_fuzz; a step whose calculation was interrupted is saved, opened and compared at once (`tests/fuzz/corpus/gaps`) |
| `rand_*` cases of `scripts/excel_basics_cases.py` | random sequences of object-model steps, measured in Excel itself | 150 in a minute (Excel), replayed by `tests.test_excel_basics` | every cell's formula, value and format against Excel's |

The first five check that YCGrid agrees with itself; only the last checks that it agrees with Excel. A difference
from Excel is found by measuring, never by a fuzzer that has no Excel to ask.

## Running rounds

```
python scripts/random_rounds.py run --rounds 30 --jobs 6     # fresh seeds, after the ones already logged
python scripts/random_rounds.py run --snapshot ...           # runs copies of the builds, so a fix can be built meanwhile
python scripts/random_rounds.py report                      # hours, findings, the curve, the stopping rule
```

Every round adds a line to [random-runs.csv](random-runs.csv). `check_all` runs fixed seeds of each tool (the same
ones every time, so a failure there is a regression, not luck); `random_rounds.py` runs new ones (exploration).
For the Excel comparison, raise `RANDOM_CASES` in `excel_basics_cases.py` and measure the new cases
(`scripts/measure_excel_basics.py --only rand_`): they join the fixture and the replay.

## A finding, from failure to closed

1. **Cut it down.** `yc_doc_fuzz --replay FILE --minimize` drops bytes while it still fails the same way (a 40-step
   stream became 5 steps); `--every-step` checks the save and open after every step, so the stream stops at the step
   that broke it, and `--verbose` prints both dumps. For the other tools, `basics.py --sequence state:op,op` and the seed of `chaos.py`.
2. **Decide what it is**, and add a line to [random-findings.csv](random-findings.csv):
   - `real`: YCGrid is wrong (severity `crash`, `data-loss`, `file`, `wrong-result`, `ui`);
   - `tool`: the test tool was wrong (a recorder that missed a window, a dump that hashed internal numbers, a slow step
     counted while a build used every CPU). Fix the tool the same day: a tool that cries wolf trains everyone to ignore
     it.
   - status `kept`: a real difference decided to stay, with its reason under "Kept on purpose" in
     [excel-parity-backlog.md](excel-parity-backlog.md); it doesn't hold convergence back. Use it only with a measured
     reason, never to make the report say "converged".
3. **Make it a test that fails** (`tests/unit/test_fuzz_found.cpp` for doc_fuzz, a basics case, a chaos seed in
   `chaos_seeds.txt`), then fix the cause where every path meets.
4. **Close the class, not the case.** Ask what else has the same shape. The answers so far became checks that fail
   for the whole class: `model/ref_texts.h` with `test_ref_registry.cpp` (every reference kind against every
   operation), `schema/model_fields.yaml` with `check_model_fields.py` and `test_model_roundtrip.cpp` (every field
   saved or followed), `engine::PolicyFor` (one table for what a write's source means), `check_lessons.py` (the
   lessons a pattern can catch).
5. **Keep the stream.** A doc_fuzz stream that failed goes into `tests/fuzz/corpus/` once fixed; check_all replays
   the whole corpus. One that can't be fixed yet goes into `tests/fuzz/open/` with its line in the findings
   (`status` open).

## When it is done: the stopping rule

Random testing has converged when **20 hours of rounds have passed since the last real finding, and no finding is
open** (`random_rounds.py report` says so). Not "nothing found today": the hours count, across tools. They are counted
from the round that found the last real finding (its tool and seed in `random-runs.csv`), not from the next day: by
day alone the 8.4 clean hours of a day's last 200 rounds counted 0. A finding no round names (found by hand) is placed
at the round of the finding written before it.

While it has not converged, **no new features**: the time goes to fixing findings and, above all, to closing their
classes (step 4). A feature added meanwhile restarts the count for the code it touches and makes the curve say more
about the new code than about the old. Measured Excel differences (`KNOWN_DIFFERENCES` of `test_excel_basics.py`)
are findings too and count against convergence.

## Checking what was written down

A lesson or a rule written as Excel's without a measurement is a finding waiting to happen: on 2026-11-15 the lessons
were checked against Excel 16 one by one where a probe could reach them. Seven held (the 15-digit correction, a
formula's format, a font on every character, the sheet activated after a delete, union names, the ImGui signatures,
the circular reference values); one was wrong ("list validation ignores case": a typed list matches with case) and the
explanation of three known differences was wrong (calculation timing, not "Excel calculates beside the cycle"). Write
"measured" beside a rule only after measuring it.

## What the curve says so far

`report` prints the real findings per 10 hours of rounds by day. On 2026-11-14 the UI tools found 1 real problem in
2.2 hours; on 2026-11-15 the new in-process fuzzer found 4 in 0.4 hours and the first 150 random sequences measured in
Excel 5 more (40 of the 150 differed through one sort rule). A new, faster tool always starts high: what matters is
that its line falls as classes close, and that each tool's falls, not only the total.

The classes closed while these tools were built, before any random round found them, came from asking what else has
the shape of a bug just fixed: the reference matrix found a scatter chart's X error bars, an icon set's formula
thresholds, a copied sheet's Solver model, panel controls' linked cells, and six kinds of rectangles (allowed ranges,
ignored errors, linked pictures, alerts, parameters, the print area's other areas) that never followed their cells;
the cut matrix found the AutoFilter, an advanced filter's list, sparklines' data, PivotTable sources and a
Ctrl+Shift+Enter array left behind by a cut.
