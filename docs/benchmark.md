# Benchmarks

Numbers anyone can reproduce with the scripts in `scripts\`. They measure what a trader or a researcher feels: how
many cells reach the screen, how long a price takes from Python to the screen, and how fast a chart of millions of
points redraws while you zoom.

Machine for the numbers below: Intel Core Ultra 9 285HX, 64 GB, NVIDIA RTX PRO 5000 Laptop GPU, Windows 11, YCGrid
release build, 120 Hz display, a 1600 x 977 window (704 visible cells), publish interval 16 ms. Run the scripts on your own machine and compare there.

## Live tables (`scripts\bench.py`)

A bound table of 100,000 rows x 10 columns (one million cells) updated by key through shared memory
(`ws.bind` / `BoundTable.update`). Latency is measured from `time.perf_counter_ns()` in Python when the data was sent
to the frame on screen that shows it (both on the same QueryPerformanceCounter clock).

```
runtime\python\python.exe -E -s scripts\bench.py --pid <YCGrid pid> --seconds 10 --discard --fraction 0.1 --rate 100
```

| Load | Cells written / s | Python -> screen p50 / p99 | Frames / s |
|---|---:|---:|---:|
| 1% of the rows, 500 updates / s | 5.5 million | 12 / 32 ms | 96 |
| 10% of the rows, 100 updates / s | 11 million | 21 / 40 ms | 94 |
| whole table, 20 updates / s | 22 million | 41 / 59 ms | 61 |
| whole table, as fast as possible | ~45 million | ~89 / 121 ms (queueing) | 70 |

The table above is a visible window. The same runs on a hidden window (`yc.launch(hidden=True)`, paced at 60 Hz) after
the pooled tile memory (2026-10-25):

| Load (hidden window) | Cells written / s | p50 / p99 | Frames / s |
|---|---:|---:|---:|
| 1% of the rows, 100 updates / s | 1.1 million | 13.6 / 39.7 ms | 95 |
| 10% of the rows, 20 updates / s | 2.2 million | 18.1 / 46.5 ms | 61 |
| 1% of the rows, 500 updates / s | 5.5 million | 12.6 / 42.9 ms | 94 |
| 50% of the rows, 10 updates / s | 5.5 million | 30.2 / 57.4 ms | 31 |
| 10% of the rows, 100 updates / s | 11 million | 20.1 / 47.3 ms | 94 |
| whole table, 20 updates / s | 22 million | 43.5 / 76.9 ms | 61 |
| whole table, as fast as possible | ~47 million | 81 / 102 ms (queueing) | 79 |

Soak, 8 hours at 5.5 M cells/s (1% of a million cells, 500 updates / s), hidden window paced at 120 Hz (2026-10-25,
07:44–15:44): median p99 16.7 ms, p50 8.6 ms, 120.6 fps, frame CPU 0.45 ms, memory growth 2.6% after warm-up
(0.7 MB / hour). 87% of the minutes kept p99 within 33 ms (publish interval + 2 frames); the other 64 minutes all fall
in times when builds and test runs used every core of the same machine, and every quiet stretch stayed at or below
18.2 ms.

Soak, 2 hours at 2.2 M cells/s (10% of the rows, 20 updates / s), hidden window: median p99 47.6 ms (94% of the minutes
under 60 ms; the slow minutes were while other builds and tests ran on the machine); private memory 268 MB at the
start, 337 MB after three steps during that contention, then flat for the last 68 minutes. Freed 64 KB tiles are kept
for reuse up to 96 MB.

`--hours N` turns it into a soak test (a sample every minute); `--rate 0` sends as fast as the bridge accepts.

## Charts of millions of points (`scripts\bench_plot.py`)

`yc.plot` sends the arrays through shared memory; YCGrid keeps a min / max pyramid per line and draws at most a few
points per pixel column, so the cost of a frame depends on the window, not on the data.

```
runtime\python\python.exe -E -s scripts\bench_plot.py --pid <YCGrid pid>
```

| Points | yc.plot: send | yc.plot: frame while zooming and panning (avg CPU) | matplotlib 3.11 (Agg): first draw | matplotlib: redraw after a zoom |
|---:|---:|---:|---:|---:|
| 100,000 | 0.014 s | 0.75 ms | 0.085 s | 14 ms |
| 1,000,000 | 0.047 s | 0.67 ms | 0.078 s | 24 ms |
| 10,000,000 | 0.47 s | 0.67 ms | 0.55 s | 61 ms |

matplotlib was measured with its fastest backend (Agg, off screen, path simplification on); an interactive window
(TkAgg, QtAgg) adds the time to show the image. Its redraw time grows with the data, so ten million points pan at
about 16 frames a second at best, while a YCGrid frame costs the same at any size, so the window keeps the monitor's
refresh rate. The script runs the matplotlib part when matplotlib is installed in the Python that runs it.

## Other paths

| Path | Script | What it shows |
|---|---|---|
| Market data topics (`Topics.Push`, `=QUOTE()`) | `scripts\stress_realtime.py` | over a million cell updates per second from a feed process |
| Record and replay | `examples/tick_replay` | live frames ignored during a replay, the same screen on recorded data |
| A million live cells in a workbook | `examples/million_cells` | throughput and latency printed on the sheet |

## How to compare with other tools

Use the same scenario (100,000 x 10 numbers, 10% of the rows changing 100 times a second) and the same measurement
(send time in the producer, display time on screen):

- **Excel with RTD or xlwings**: write the changed cells with `Range.Value` (xlwings) or serve them as RTD topics;
  Excel throttles RTD to `Application.RTD.ThrottleInterval` (2 s by default) and recalculates on the UI thread.
- **Web grids (Perspective, AG Grid)**: stream the same updates over a WebSocket; measure in the browser with
  `performance.now()` on the producer's timestamp.

We publish only numbers measured with the scripts above; results for other tools are welcome with their scripts.
