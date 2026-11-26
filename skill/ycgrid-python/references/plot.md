# Fast plots (`yc.plot`)

Plot from any Python script into the running YCGrid: matplotlib-like calls, drawn with ImPlot on the GPU. Data go to
YCGrid through shared memory as arrays, and long lines are downsampled when drawn (min / max per pixel column, or
LTTB), so ten million points pan and zoom at full frame rate. Figures are dockable windows: drag to pan, wheel to zoom,
double-click to fit, right-click for the plot menu.

```python
import numpy as np
from yc import plot

fig = plot.figure("Backtest", rows=2, link_x=True, size=(900, 600))   # plots share the x range when link_x
fig[0].line(dates, equity, label="Equity", color="#1565c0")
fig[0].candles(t, o, h, l, c, label="TXF")                            # merged per pixel when zoomed out
fig[1].fill_between(dates, drawdown, color="#c62828", label="Drawdown")
fig[0].set(title="TXF", ylabel="points", log_y=False)

plot.line(x, y)                                     # quick: a new window with one line
plot.hist(returns, bins="scott")                    # also plot.scatter / bar / candles / heatmap

ln = fig[0].line([], [], label="PnL", capacity=100_000)   # live: keep the newest 100k points
fig[0].set(follow=600)                                    # x shows the newest 600 units (seconds on a time axis)
ln.append([time.time()], [pnl])

fig.link_table("Trades", "Time")   # the active row of table Trades is marked; a click on a plot selects the nearest row
```

| Call | Data | Options |
|---|---|---|
| `line(x, y)` / `line(y)` | numbers or dates; a pandas Series uses its index as x | `label`, `color`, `width`, `marker`, `capacity`, `downsample` (`"minmax"`, `"lttb"`, `"none"`) |
| `step(x, y)` | | `label`, `color`, `width`, `capacity` |
| `scatter(x, y)` | | `label`, `color`, `size`, `marker` (`o s d ^ v x +`), `capacity`, `downsample` (`"auto"`: over 100,000 points, one per occupied 2 x 2 pixel cell of the view, so outliers stay visible; `"none"`) |
| `bar(x, height)` | | `width`, `label`, `color` |
| `hist(values, bins)` | | bins: a count or `"sqrt"`, `"sturges"`, `"rice"`, `"scott"` |
| `fill_between(x, y1, y2=None, ref=0)` | | `label`, `color` |
| `candles(t, open, high, low, close)` | | `label`, `up_color` |
| `heatmap(z, xlabels, ylabels)` | a 2-D array, rows top to bottom | `cmap` (`viridis`, `rdbu`, `plasma`, `hot`, `jet`..., and YCGrid's `pnl`: loss green, zero black, profit red, or `pnl_light` through white; give `vmin=-m, vmax=m` so zero sits in the middle), `vmin`, `vmax`, `fmt`; values written in cells up to 400 cells; `extent=(x0, x1, y0, y1)` places it in data coordinates (datetimes on a time axis) so lines and points can be drawn over it (a market depth heatmap); `set_data(z, extent=...)` may change the shape |
| `text(x, y, s)` | | `color` |
| `hline(values)` / `vline(values)` | | `label`, `color`, `width`, `text`; `tag=True` puts each value on its axis after `text` ("STOP 23,074", `fmt` e.g. `"%.0f"`), `style` `"solid"` / `"dash"` / `"dot"` (also on `line` and `step`) |
| `hband(y0, y1)` / `vband(x0, x1)` | numbers or lists (several bands) | across the whole plot at any zoom: zones, value areas, sessions; `color`, `edge`, `text` / `texts` (written inside), `colors`, `fit` (count in that axis's auto-fit; default False) |
| `surface(x, y, z)` | `z[j][i]` over `x[i]`, `y[j]` (e.g. volatility by strike and expiry) | `cmap`, `vmin`, `vmax`, `wireframe`; the plot becomes a 3-D view (drag to turn, wheel to zoom, double-click to reset, hover for values); `set(zlabel=..., view=(azimuth, elevation))` |

- `plot.figure(title="", rows=None, cols=None, size=None, link_x=False, pos=None, theme=None, sync_cursor=None,
  key=None, reuse=False, persist=None, height_ratios=None, width_ratios=None)`: 1 x 1 plots and
  760 x 480 by default; size and pos (from the main window's top-left) in points; without pos new figures cascade. `height_ratios=[4, 1, 1]` / `width_ratios` size the plots (a candle chart
  with small strips under it in one figure).
- Keys and owners: `key="desk.main"` names the window across processes and restarts - a new figure with that key takes
  the window over (place and size stay, items are cleared; `reuse=True` also keeps its title and layout).
  `plot.figures()` lists every figure of the session (`id`, `key`, `title`, `rows`, `cols`, `owner` "host" / "script
  <pid>", `alive`, `persist`, `docked`, and `axes`: each plot's `title` and `items`, each with its `kind` and `label`,
  so a test checks what a figure shows: a line added twice shows twice; `cursor_tag`, the date a linked
  table's row marks on a daily time axis; `view` [x0, x1, y0, y1], `rect` (the plot area in window pixels, to
  point at a price) and `handles`, each drag line's `kind`, `label`, `x` and `y`); `plot.close(key=...)` closes one from any
  process. A figure belongs to the connection
  that made it: when the YCGrid host restarts or exits its figures close, unless made with `persist=True` (then the
  title says "(stopped)"); scripts' figures stay by default (`persist=True`).
- `signals(x, price, side, labels=None, buy_color=None, sell_color=None, size=8)`: buy / sell marks on candles - an
  up triangle just under price for a buy, a down triangle just above it for a sell, each with an optional label ("B 2").
  side: +1 / -1, "B" / "S", "buy" / "sell", "long" / "short"; pass the bar's low for buys and its high for sells to keep
  the marks off the candles. Buy red, sell green by default.
- Synced crosshairs: figures made with the same `sync_cursor="name"` show the x under the mouse in one of them as a
  crosshair (line and axis tag) in all the others; the plots of one figure with `link_x=True` do it among themselves.
  With `link_x=True` stacked plots (a candle chart over its volume) show the x labels once, under the bottom row.
- Time axes show local time on a 24-hour clock; spans up to two days get ticks on whole seconds, minutes or hours
  ("22:45", "22:45:30").
  `fig.cursor(x)` sets it from Python (a number or a date / time, e.g. the replay time; `None` removes it); hovering a
  plot takes over.
- Themes: `theme="light"` (default) or `"dark"` (also `"trading"`): the dark trading theme, black, with bright lines
  and red up / green down candles and volume (Taiwanese convention). `plot.set_theme("dark")` sets the theme of the figures
  made after it; `fig.theme` reads it. `plot.TRADING` holds the theme's colors: `yellow`, `magenta`, `cyan`, `green`,
  `orange`, `white`, `violet`, `up`, `down`, `text`, `grid`. Lines without a color take YCGrid's own palette (blue,
  orange, green, red, purple... on white; yellow, magenta, cyan, green, orange, white... on black).
- `Axes.set(title, xlabel, ylabel, time, log_y, legend, xlim, ylim, follow, equal, yfit, ywidth)`: limits apply once per call;
  the user pans and zooms freely afterwards. With `follow`, `yfit="stable"` moves y only when the visible data come
  near its edges or fill less than a third of it (a live chart that doesn't jitter; data that are never negative
  keep 0 as the floor); the default `"auto"` fits y to the visible data every frame. While the user drags a handle,
  the plot's limits stay put. `ywidth`: the y axis's width - points (fixed; wider labels or tags still widen it),
  `"stable"` (only grows, so tags that come and go - order lines, the average cost - don't move the plot sideways;
  the default with `yfit="stable"`) or `"labels"` (as wide as its labels and tags at each moment). `yfit="stable"`
  counts lines, steps, scatter, bars, candles and the rects overlapping the window (footprint cells, zones).
- The view follows the data until the user takes it: every `set_data` / `append` fits the plot again (a payoff
  recomputed after a fill, a smile refitted, an item made empty and filled later), until the user zooms (wheel, box)
  or pans it, or Python sets `xlim` / `ylim`. A double-click (or `fig.fit()`) fits it and lets it follow again.
  Live plots that should scroll use `follow=` instead (they set their own window).
- Axis numbers from 1,000 up (y) and 1,000,000 up (x) are printed with thousands separators (`8,000,000`, not
  `8e+06`).
- `follow=N` shows the newest N x units ending a little after the newest point (half a bar more for candles and bars,
  so the forming bar is whole and its high and low count in the y fit).
- Following and looking around: a plot with `follow` stops following when the user zooms (wheel), pans (drag) or box
  zooms (right-drag), so they can look closely at older bars or set a level precisely; a "Paused" badge in its corner
  goes back live when clicked, and so does `Axes.set(follow=...)` from code. `fig.following` reads the state,
  `fig.on("follow", cb)` gets an Event with `paused` True / False, `fig.input("wheel")` / `fig.input("live")` do it
  in tests. `fig.fit()` fits every plot to its data. `ylim` stays when data are added later (only x
  fits); a plot with `follow` fits its y axis to the visible data each frame unless `ylim` was given;
  `set(ylim="auto")` lets y fit the data again.
- An item: `set_data(...)` (same arguments as the call that made it), `append(...)`, `style(color=..., label=...)`,
  `remove()`. `ax.clear()` raises (remove each item, or make a new figure); `fig.close()`, `plot.close_all()`.
- Colors: `"#rrggbb"`, `"#aarrggbb"`, names (`red`, `green`, `blue`, `orange`, `gray`, `yellow`, `magenta`, `white`...)
  or an Excel RGB number.
- Building blocks (draw your own indicators and trading tools from them):
  - `rects(x0, x1, y0, y1, colors=None, labels=None, values=None, fmt=None, color=None, edge=None)`: rectangles in data
    coordinates by the hundred thousand, each with its color and its text (drawn when it fits): footprint cells, volume
    profiles, TPO, zones, order lifetimes.
  - `barh(y, length, height=None, colors=None)`, `errorbar(x, y, err=None, neg=None, pos=None, horizontal=False)`,
    `polygon(x, y, color, edge)` (NaN separates polygons), `arrows(x0, y0, x1, y1)`, `annotate(x, y, texts, color)`;
    `bar(..., colors=...)` and `scatter(..., colors=...)` take one color per bar / point; `line(..., tag=True, fmt=)` puts
    the last value on the y axis.
  - Events: `fig.on("click" | "dblclick" | "hover" | "select" | "drag" | "release", callback, right=False)` (also a
    decorator) gets an `Event` with `type`, `ax`, `x`, `y` (data coordinates; time axes: Unix seconds), `item` /
    `index` (the nearest point of the plot's first series), `ctrl`, `shift`, `alt`; clicks have `button` (`"left"`,
    `"middle"`, `"right"`: right clicks come only with `right=True` or a context menu, and the plot's own menu then opens
    with Shift + right click); `select` (Ctrl + drag) has `x0`, `x1`, `y0`, `y1`. In the host (modules) events are
    pushed; scripts get them from a background thread, or call `fig.poll()`.
  - Context menu: `fig.context_menu([("Add support here", add_support), None, ("Buy 1 limit here", buy)])`: a right
    click opens it; the chosen item's callback gets the Event (`x`, `y` where it was opened, `ax`, `text`).
  - Handles: `ax.drag_line(value, horizontal=True, label, color, on_move=, on_release=)`, `ax.drag_point(x, y, ...)` and
    `ax.drag_band(y0, y1, label, color, resize=True, on_move=, on_release=)` return a `Handle` (`value`, `x`, `y`; a band
    `y0`, `y1`, moved by its body, resized by its edges; `set(value | x= | y= | y0= | y1= | label=)`, `remove()`): an
    order line you drag to change its price, a stop, a support zone. `drag_line(..., on_close=fn)` (or `closable=True`)
    adds an "x" button at the line's end: a click calls `fn(handle)` and sends a `"close"` event (cancel the order).
    Y-axis tags (line and hline tags, handle labels) at close prices are stacked apart so they stay readable (the lines
    stay at their prices); keep their texts short.
    Handles don't change the view: `fit=True` counts one in the plot's fit. `handle.set(color=...)` recolors it (a
    zone the price is inside).
    The `order_flow` example builds a footprint with a draggable, cancellable order from these.
  - Testing interactions: `fig.input(action, ax=0, **kw)` does what the mouse would, through the same events -
    `"click"` (x, y, button, ctrl, shift, alt), `"double_click"`, `"select"` (x0, x1, y0, y1), `"drag_handle"` (handle,
    x / y), `"drag_band_edge"` (handle, edge, y), `"close_handle"` (handle), `"context_menu"` (text, x, y); MCP:
    `yc_plot_input`. A right `"click"` opens the context menu at that point, as the mouse would (screenshots show it);
    `"context_menu"` chooses an item and closes it. `yc.ui.state()` lists each figure's `menu`, `docked`, and each
    plot's `handles` (`closable`), `items` (`kind`, `label`, `points`) and `view` (the limits drawn last frame:
    `[x0, x1, y0, y1]`, x in Unix seconds on a time axis): check that what the trader should see is in view.
  - Live from topics (no Python in the loop): `ax.candles_live(topic, seconds=60, volume_topic=None, keep=3000,
    volume_ax=None, cumulative_volume=False)` builds candles in YCGrid from a topic's updates (`Topics.Push` from a
    feed, a script, the host, or a replay) by the rules of `yc.bars.BarBuilder`, the forming bar included; the volume
    topic's sizes go to bars on `volume_ax` in the candle colors (`item.volume`). `ax.line_live(topic, capacity)` /
    `ax.step_live(topic, capacity)` add a point per update. Times are the updates' times (`Topics.Push(...,
    TimeNs=)`, else when YCGrid got them). The chart keeps moving while Python is busy, reloading or paused; the first
    data fit the view, `ax.set(follow=seconds)` keeps the newest in view. `set_data` still loads history first;
    `item.data()` / `plot.item_data(item)` read back what YCGrid holds (x, open, high, low, close, volume).
- `Axes.plot_files(kind, label=None, color=None, **columns)`: the columns are `.npy` paths (`yc.data.save_columns`) read
  by C++ directly: hundreds of millions of points without passing through Python.
- Updating a figure the user has closed raises `YcError` ("... it was closed"); a live loop that should go on without
  its chart catches it (the examples use `with chart():` from `examples/lib/ex_util.py`).
- Dates (datetime, numpy datetime64, pandas timestamps) make the x axis a time axis. Times without a time zone are
  local wall-clock times, like the sheet's; time-zone-aware pandas columns are converted. Raw numbers on a time axis
  are Unix seconds (`time.time()`).
- `link_table(table, column)`: for a time axis the table column holds sheet dates (they are converted).
- Figures belong to the YCGrid session (they aren't saved with the workbook); a script that ends leaves them open, the
  host's close when it restarts (see Keys and owners).
- Docking: `fig.Dock(Position=ycDockRight, Ratio=0.3)` docks a figure beside the sheets in the main window
  (`ycDockLeft` / `Right` / `Top` / `Bottom` / `Tab`), `fig.Dock(ycDockRight, Container=c)` or `c.Dock(fig, ...)`
  into a container (`yc.ui.Containers.Add`), `fig.Dock(ycDockTab, Anchor=other_fig)` makes it a tab in another
  figure's place (two pages in one spot: a chart that needs its own room; `Anchor` with Left / Right / Top / Bottom
  docks beside that figure), `fig.Float(Left, Top, Width, Height)` takes it out; `fig.Docked` says
  whether it sits in a dock, `fig.Locked = True` keeps the user from moving, undocking, resizing or closing its window
  (zoom, pan, handles and the context menu still work). Docking a figure that is docked already moves it. A figure with a
  `key` keeps its place by key: `yc.ui.Layout.Save(name)` / `Restore(name)` (saved in the workbook) put it back, also
  when the figure is made again later with that key.
- For charts saved with the workbook and fed by cells, use Excel charts (`ChartObjects.Add`, `Shapes.AddChart2`).
