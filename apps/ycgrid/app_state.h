// SPDX-License-Identifier: Apache-2.0
// app_state.h - YCGrid application state (UI thread)
//
// Model state (sheet contents, formatting, structure) is owned by the engine; the UI reads a snapshot every frame.
// View state (scrolling, selection, zoom, frozen panes, window layout, text being edited) is owned by the UI (PLAN §10.1).
#pragma once

#include "engine/engine.h"
#include "engine/error_check.h"
#include "core/i18n.h"
#include "grid/grid.h"
#include "io/msgvalue.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace yc::engine {
struct ChartValues;
}

namespace yc::app {

using core::Rect;

struct Container {
  int id = 0;
  std::string name;
  bool open = true;
};

struct SheetWin {
  bool floating = false;
  render::GridView view;
  bool gridlines = true, headings = true;  // per sheet, like Excel (ActiveWindow.DisplayGridlines / DisplayHeadings)
  bool formulas = false;                   // Show Formulas (Ctrl+`): a setting of the sheet's window (Window.DisplayFormulas)
  bool outline = true;                     // outline symbols (Ctrl+8, Window.DisplayOutline)
  bool zeros = true;                       // "Show a zero in cells that have zero value" (Window.DisplayZeros)
  uint32_t gridline_color = 0;             // Options > Gridline color: 0 automatic, else 0xRRGGBB (Window.GridlineColor)
  // Options > "Show page breaks" (Worksheet.DisplayPageBreaks): the dashed lines where pages break in Normal view, on
  // after Print, Print Preview or a page setup change, as in Excel; not saved (Excel's files don't keep it either), so
  // a workbook opened or made anew starts without them (ResetViews clears every SheetWin)
  bool page_breaks = false;
};

enum class Dialog : int {
  None = 0,
  FormatCells,
  Find,
  Replace,
  GoTo,
  RowHeight,
  ColumnWidth,
  RenameSheet,
  InsertCells,
  DeleteCells,
  PasteSpecial,
  TabColor,
  Message,
  ZoomDialog,
  InsertFunction,
  NameManager,
  InputBox,
  Macros,
  InsertChart,
  CfQuick,
  CfNewRule,
  CfManage,
  DataValidation,
  NoteEdit,
  HyperlinkEdit,
  GoToSpecial,
  CustomSort,
  FillSeries,
  DefineName,
  CreateNames,
  PasteName,
  RemoveDuplicates,
  TextToColumns,
  GroupOutline,
  EvaluateFormula,
  CustomAutoFilter,
  Top10Filter,
  ProtectSheet,
  UnprotectSheet,
  ProtectWorkbook,
  UnprotectWorkbook,
  AllowEditRanges,
  UnlockRange,
  CreateTable,
  RenameTable,
  ResizeTable,
  CreateSparklines,
  PageSetup,
  CreatePivot,
  PivotSource,
  PivotMove,
  PivotRename,
  PivotValueField,
  PivotGroup,
  PivotCalcField,
  PivotCalcItem,
  PivotFieldSettings,  // Field Settings (subtotals, layout) of a row / column / filter field
  PivotOptions,        // PivotTable Options
  PivotSort,           // More Sort Options
  PivotFilterDlg,      // Label / Value / Top 10 filter of a field
  PivotShowDetail,     // Show Detail: the field to add below an item
  PivotNumberFormat,   // a data field's Number Format
  PivotChartType,      // the chart type of a new PivotChart
  InsertSlicer,
  GoalSeek,
  DataTableDlg,
  ScenarioManager,
  Consolidate,
  AdvancedFilter,
  Solver,
  PasswordOpen,
  EncryptWorkbook,
  CellStyle,
  CustomLists,
  CustomizeQat,
  EditLinks,
  MoveChart,
  DataAnalysis,  // the Analysis ToolPak's list of tools
  AnalysisTool,  // the chosen tool's options
  UnhideSheet,   // the hidden sheets to choose from
  MoveCopySheet, // Move or Copy: before which sheet, and whether to copy
  FunctionArgs,  // Function Arguments: a box per argument
  Ask,           // a question with OK / Cancel (AskYesNo)
  SortWarning,   // data next to the selection: expand it or not (data_ui.cpp)
  OutlineSettings,  // Data > Outline dialog launcher
  StandardWidth,    // Home > Format > Default Width: the sheet's standard column width
  SlicerSettings,   // Slicer tab > Slicer Settings (pivot_ui.cpp)
  CustomViews,      // View > Custom Views (print_ui.cpp)
  ArrangeWindows,   // View > Arrange All (windows_ui.cpp)
  UnhideWindow,     // View > Unhide: the hidden windows (windows_ui.cpp)
  DocProperties,    // File > Info > Properties > Advanced Properties (print_ui.cpp)
  ActivateSheetList,  // right-click on the sheet tab arrows: Activate (cells_ui.cpp)
  FillAcrossSheets,   // Home > Fill > Across Worksheets (cells_ui.cpp)
  AddWatch,         // Watch Window > Add Watch: the cells to watch (data_ui.cpp)
  ApplyNames,       // Formulas > Define Name > Apply Names (names_ui.cpp)
  TrustCenter,      // File > Options > Trust Center Settings (trust.cpp)
  SecurityOptions,  // File > Info > Enable Content > Advanced Options (trust.cpp)
  PivotReportConnections,  // a slicer's Report Connections (pivot_ui.cpp)
  PivotFilterConnections,  // PivotTable Analyze > Filter Connections: the slicers of the active PivotTable
  RecommendedPivots,       // Insert > Recommended PivotTables
  Subtotal,                // Data > Outline > Subtotal (data_ui.cpp)
  SortOptions,             // Sort > Options...: case sensitive, orientation (misc_ui.cpp)
  ForecastSheet,           // Data > Forecast Sheet (forecast_ui.cpp)
  ImportText,              // Data > From Text/CSV: the preview window (forecast_ui.cpp)
  ImportData,              // ... its Load To...: Import Data (forecast_ui.cpp)
  FormatControl,           // Format Control: Excel's dialog with tabs (objects_ui.cpp)
  FormatComment,           // a note's Format Comment (misc_ui.cpp)
};

class PythonBridge;

struct AppState {
  HWND hwnd = nullptr;
  std::wstring file_path;
  std::wstring opened_from;  // the file the workbook was opened from (also an Excel or CSV file, which opens untitled)
  bool headless = false;      // --hidden: no window on screen, no AutoRecover copies or recovery pane (CI, agents)
  // Application.Interactive: false blocks keyboard and mouse input (Python's dialogs still work); back to true when the
  // Python host restarts
  bool interactive = true;
  // Application.Caption / Window.Caption: the title bar's two parts, "<window> - <application>" (Excel 16, measured);
  // app_caption "" is YCGrid's own name, win_caption_set false the file's name
  std::string app_caption, win_caption;
  bool win_caption_set = false;
  bool show_status_bar = true;  // Application.DisplayStatusBar
  // Application.DisplayFullScreen: the window maximized without the ribbon, the formula bar and the status bar (they
  // read False meanwhile and come back after, Excel 16 measured)
  bool full_screen = false;
  int calc_interrupt_key = 2;  // Application.CalculationInterruptKey: xlNoKey (0), xlEscKey (1), xlAnyKey (2)
  int cursor = -4143;  // Application.Cursor: xlDefault (-4143), xlNorthwestArrow (1), xlWait (2), xlIBeam (3)
  int headless_window_state = -4137;  // a hidden instance's WindowState: its window is never shown
  std::string chart_tip;  // the tip the mouse over a chart showed last frame (ui.state, tests)
  ImRect work_rect;  // the last frame's workspace under the ribbon and formula bar (Application.UsableWidth)
  // Worksheet.ScrollArea (sheet id -> range): selection and scrolling stay inside it; not saved, like Excel
  std::map<uint32_t, Rect> scroll_areas;
  // [YC] safety for live trading (safety_ui.cpp): operator mode (off = read-only: parameter cells and guarded buttons
  // are locked), heartbeats of feeds and scripts, and the time of the user's last input (operator mode's idle timeout)
  bool operator_mode = true;
  double last_input = 0;
  struct Heartbeat {
    std::string name, detail;
    double timeout = 2, last = 0;  // seconds (steady clock)
  };
  std::vector<Heartbeat> heartbeats;
  // alarms raised by Python (safety.alarm): name -> text, shown until cleared (e.g. positions differ from the broker)
  std::map<std::string, std::string> alarms;
  // [YC] trading screens (trading_ui.cpp): click actions on cell ranges (click-to-trade), Application.OnKey hotkeys
  struct ClickRange {
    int64_t id = 0;
    uint32_t sheet = 0;
    Rect rect{};
    bool left = true, right = false;
  };
  std::vector<ClickRange> click_ranges;
  bool suppress_cell_menu = false;  // a right click went to a click action: no context menu this time
  uint32_t figure_keys = 0;         // the window id of the figure the user clicked last: it has the keyboard
  // Undo / Redo run on the engine thread (millions of formulas took the screen for seconds); the cells they changed
  // are shown when they are done (ApplyUndoMove, each frame)
  struct UndoMove {
    std::atomic<bool> done{false};
    bool ok = false;
    uint32_t sheet = 0;
    Rect rect{};
  };
  std::shared_ptr<UndoMove> undo_move;
  // an entry handed to a busy engine (CommitEdit) and its result (ApplyPendingCommit, each frame)
  struct PendingCommit {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string error;
    render::EditState edit;  // the editor as it was, given back when the entry is refused
    bool hint = true;        // the message adds "Not trying to type a formula?" (not for an array formula)
  };
  std::shared_ptr<PendingCommit> pending_commit;
  // a command that needs the engine's answer, asked while the engine works on what came before (a million formulas
  // filled a frame earlier): it runs on the frame after the engine has caught up (RunDeferred), never in a frame
  // that waits for it; the selection it was asked on comes back first when keep_selection
  struct Deferred {
    std::shared_ptr<std::atomic<bool>> ready;
    uint32_t sheet = 0;
    render::Selection sel;
    bool keep_selection = true;
    std::function<void(AppState&)> run;
  };
  std::vector<Deferred> deferred;
  bool running_deferred = false;
  bool in_request = false;  // a Python request runs (PythonBridge::OnUi): it waits for its answer, nothing is put off
  // Excel's circular reference warning: after an entry (or opening a file) the next settled snapshot is checked; the
  // message shows once per workbook (CheckCircularWarning, each frame)
  bool circ_check = false, circ_warned = false;
  uint64_t circ_after = 0;  // the snapshot version the entry made
  size_t circ_before = 0;   // circular cells before it
  struct OnKey {
    std::string key, procedure;  // procedure "" = the key does nothing
    int chord = 0;               // ImGuiKeyChord
  };
  std::vector<OnKey> on_keys;
  std::wstring app_dir;

  std::unique_ptr<engine::Engine> engine;
  std::shared_ptr<const engine::Snapshot> snap;  // snapshot used for this frame

  std::map<uint32_t, SheetWin> wins;  // by sheet id
  uint32_t active_sheet = 0;          // sheet shown in the main window
  uint32_t focus_sheet = 0;           // sheet that last had focus (target of the formula bar and the ribbon)
  std::vector<uint32_t> just_torn;
  std::string last_moving_window;
  float tab_scroll = 0.0f;

  render::EditState edit;
  model::RichText edit_rich;  // fonts inside the text being edited (runs refer to edit_rich.text)

  // clipboard (engine-internal format + system clipboard sequence number, to tell whether we wrote it)
  bool has_clip = false;
  // what was copied, shared and never changed once set (CopySelection makes a new one): a paste, the Paste Options
  // button and the engine's step hold the same cells instead of a copy each (six million cells were copied three times)
  std::shared_ptr<const engine::ClipData> clip = std::make_shared<const engine::ClipData>();
  DWORD clip_seq = 0;
  // a copy handed to a busy engine (CopySelection): `clip` holds only where it came from until the cells are ready
  // (ApplyPendingCopy, each frame); a paste asked for meanwhile waits in `after_copy` instead of the screen waiting
  struct PendingCopy {
    std::atomic<bool> done{false};
    engine::ClipData data;
  };
  std::shared_ptr<PendingCopy> pending_copy;
  std::function<void(AppState&)> after_copy;
  // the last fill-handle drag (the Auto Fill Options button)
  // grouped sheets (Ctrl / Shift + click on the tabs, Select All Sheets, Worksheets(Array(...)).Select): every one of
  // them gets what is typed, cleared, formatted, filled or inserted on the active sheet (Excel's [Group] mode). Empty or
  // one sheet: no group. The active sheet is always one of them.
  std::vector<uint32_t> group_sheets;
  bool focus_cell = false;  // View > Focus Cell: the active cell's row and column tinted (Excel)
  bool hide_objects = false;  // Ctrl+6 / Workbook.DisplayDrawingObjects = xlHide: shapes, charts and controls aren't shown
  // the command F4 / Ctrl+Y repeat (Repeat): the last repeatable command run, kept over Undo, Redo and typing
  std::string repeat_id;
  struct LastFill {
    bool active = false, date = false;
    uint32_t sheet = 0;
    Rect src{}, dest{};
    int type = 0;  // the option chosen (0 = the default fill)
    bool series = false;  // the default fill made a series (Item1 -> Item2, a time, a list entry), not copies
  } last_fill;
  // the last paste of copied cells (the Paste Options button beside it)
  struct LastPaste {
    bool active = false;
    uint32_t sheet = 0;
    Rect dest{};             // the cells pasted
    std::shared_ptr<const engine::ClipData> clip = std::make_shared<const engine::ClipData>();  // what was pasted (pasted again another way)
    int option = 0;          // the option chosen (0 = Paste)
  } last_paste;
  // the last insert of rows / columns / cells (the Insert Options button beside them: Format Same As Above...)
  std::string repeat_command;  // InsertOrDelete on whole rows / columns: the command F4 repeats (InsertRows...)
  struct LastInsert {
    bool active = false;
    uint32_t sheet = 0;
    int what = 0;     // as InsertOrDelete: 1 rows, 2 columns, 3 cells shifted down, 4 cells shifted right
    Rect rect{};      // the inserted cells
    Rect sel{};       // the selection then (the button stays while it is unchanged)
    int origin = 0;   // the formats they took: 0 above / left, 1 below / right, 2 none
  } last_insert;
  bool new_workbook_later = false;    // Workbook.Close() of changed work: the next frame asks and starts a new workbook
  bool close_without_asking = false;  // Workbook.Close(SaveChanges=False): the window closes without the save prompt
  bool ribbon_revealed = false;  // Auto-hide Ribbon: shown until the next click in the sheet
  bool marquee = false;
  Rect marquee_rect{};
  uint32_t marquee_sheet = 0;
  // format painter
  bool format_painter = false, format_painter_sticky = false, painter_armed = false;
  Rect painter_rect{};  // Format Painter's source cells (outlined while it is on)
  int format_category = -1;  // Format Cells opens its Number tab on this category once (More Accounting Formats)
  uint32_t painter_sheet = 0;
  engine::ClipData painter;
  // Clipboard task pane: the text of the last 24 copies made while it is shown (Excel's Office Clipboard collects only
  // then), and the copy it took last
  std::vector<std::string> clip_history;
  DWORD clip_history_seq = 0;
  // formula auditing: tracer arrows (from a precedent cell / range to a formula cell) and the cells of the last level
  // traced, so another click goes one level further
  struct AuditArrow {
    uint32_t sheet;
    Rect from;
    uint32_t to_r, to_c;
    // Excel removes the arrows when the formula they point to changes or rows / columns are inserted or deleted:
    // the formula and the sheet's rows / columns as they were when traced
    uint32_t to_formula = 0;
    std::string to_text;  // the formula's text (an edit can keep its id)
    const void* rows = nullptr;
    const void* cols = nullptr;
    bool precedent = true;  // drawn by Trace Precedents (else Trace Dependents)
    // the cells are on other sheets: a dashed arrow from a sheet icon to the formula (precedents) or from the cell to
    // the icon (dependents); a double-click on the icon lists these references in Go To, as in Excel
    std::vector<std::string> other_refs;
  };
  std::vector<AuditArrow> audit_arrows;
  std::vector<Rect> audit_prec_frontier, audit_dep_frontier;
  std::pair<uint32_t, Rect> audit_prec_origin{}, audit_dep_origin{};
  // Watch Window
  struct Watch {
    uint32_t sheet, r, c;
  };
  std::vector<Watch> watches;
  uint64_t struct_seq_seen = 0;  // the last row / column shift applied to the watches (FollowStructureChanges)
  bool show_watch = false;

  std::vector<Container> containers;
  int next_container_id = 1;
  // [YC] windows the user can't move, undock, dock into, resize or close ("panel:<name>", "container:<id>",
  // "figure:<key>"): Panel.Locked and the like from Python (saved in the layout). Review > Protect Workbook > Windows
  // locks every window. Python still docks, floats and sizes them.
  std::set<std::string> locked_windows;
  bool WindowsProtected() const;
  bool WindowLocked(const std::string& key) const { return WindowsProtected() || locked_windows.count(key) != 0; }
  // the user tried to move a locked window or the layout: the status bar says why (once per gesture)
  void SayWindowsLocked();

  // the focused sheet's gridlines / headings (the View ribbon's check boxes edit these; SyncSheetView copies them to
  // and from that sheet's SheetWin); default_*: what a sheet seen for the first time gets (Options)
  bool show_gridlines = true;
  bool show_headings = true;
  bool default_gridlines = true, default_headings = true;
  uint32_t view_synced_sheet = UINT32_MAX;
  bool show_formula_bar = true;
  // Options > Advanced > Display options for this workbook (saved with it): the sheet tabs and the scroll bars
  bool show_sheet_tabs = true, show_hscroll = true, show_vscroll = true;
  bool show_formulas = false;
  bool show_chart = false;
  bool show_imgui_demo = false;
  bool show_implot_demo = false;
  bool show_perf = false;
  bool design_mode = false;  // Design Mode (PLAN §12.9)
  int chart_type = 1;
  bool chart_decimate = true;
  float tab_split = 0.55f;

  bool req_layout_save = false;
  bool req_layout_load = false;
  bool req_layout_reset = false;
  bool layout_restored = false;
  bool first_frame = true;

  // dialogs
  Dialog dialog = Dialog::None;
  bool dialog_open_request = false;
  int format_tab = 0;
  std::string message_title, message_text;
  // a Python request is running that raises its own errors (a save): ShowError puts the text here instead of opening a
  // message box, as Excel's object model raises without a dialog
  std::string* quiet_error = nullptr;
  std::wstring password_path;  // the encrypted workbook waiting for its password
  bool password_wrong = false;

  std::string status;
  std::string status_stats;  // the selection statistics shown at the status bar's right (Average: 2    Count: 3...)
  double status_until = 0.0;
  // a refusal said in red beside the status bar's Error Log entry instead of a message box (a protected sheet), its
  // whole text in the Error Log: a box over the sheet stopped the work at every touch of a locked cell (NoteRefusal)
  std::string refusal;
  double refusal_until = 0.0;

  // Python (PLAN §13)
  std::unique_ptr<PythonBridge> python;
  std::string python_status;      // Application.StatusBar
  bool show_console = false;      // Immediate window
  int64_t project_stamp = 0;      // last modification time of the .py files (reload on save)
  double project_poll_at = 0.0;
  std::string inputbox_title, inputbox_prompt, inputbox_text;
  std::function<void(bool, const std::string&)> inputbox_done;

  // MCP: screenshots (handled by main.cpp after a frame is drawn)
  struct ShotRequest {
    std::wstring path;
    std::string target = "grid", address;
    uint32_t sheet = 0;
    int max_width = 1400;
    bool headers = true;
    int wait_frames = 2;  // capture after scrolling and selection are applied
    bool to_clipboard = false;  // Copy as Picture: put it on the clipboard (no file)
    // target "window": the whole main window; "imgui_window": the floating window whose ImGui name is address
    std::function<void(std::vector<uint8_t>&& rgba, int w, int h)> on_pixels;  // Screenshot: the pixels (RGBA), no file
    std::function<void(bool ok, int w, int h, const std::string& info)> done;
  };
  std::vector<ShotRequest> shots;

  // measurements
  int64_t visible_cells = 0;
  double ribbon_ms = 0.0, grid_ms = 0.0;
  std::vector<double> frame_ms, ribbon_ms_hist;  // recent frames only (trimmed, so a session running for days doesn't grow)
  uint64_t frames_presented = 0;
  bool vsync = true;  // perf.set: off measures how fast frames can be drawn
  bool perf_continuous = false;  // perf.set: draw frames nonstop (like a game loop) instead of only on changes
  double perf_hz = 0;  // perf.set: draw frames at this fixed rate without vsync (a display of that rate, also when hidden)
  uint64_t snapshots_shown = 0, last_shown_version = 0;  // distinct engine snapshots that reached the screen
  // tick-to-screen latency of bound-table data: the send time of the newest data in the snapshot being drawn, and the
  // recent latencies (ms, measured after Present)
  int64_t pending_input_ns = 0;
  std::vector<double> input_latency_ms;

  // ---- methods ----
  void SetStatus(std::string s, double seconds = 4.0);
  SheetWin& Win(uint32_t sheet_id) {
    auto [it, fresh] = wins.try_emplace(sheet_id);
    if (fresh) it->second.gridlines = default_gridlines, it->second.headings = default_headings;
    return it->second;
  }
  // each frame: the ribbon's gridlines / headings follow the focused sheet, and their changes go to it
  void SyncSheetView() {
    SheetWin& w = Win(focus_sheet);
    if (view_synced_sheet != focus_sheet) {
      view_synced_sheet = focus_sheet;
      show_gridlines = w.gridlines, show_headings = w.headings;
    } else {
      w.gridlines = show_gridlines, w.headings = show_headings;
    }
  }
  // View > New Window: more windows on the workbook, each with its own sheet, selection and scrolling
  struct ExtraWin {
    uint32_t id = 0;
    render::GridView view;
    float tab_split = 0.6f, tab_scroll = 0.0f;
    bool focus_request = true;
    bool hidden = false;  // View > Hide (View > Unhide shows it again)
  };
  std::vector<ExtraWin> extra_wins;
  uint32_t next_extra_win = 2;
  bool arrange_windows = false;  // View > Arrange All: tile this workbook's windows over the sheets next frame
  int arrange_mode = 2;          // Arrange Windows: 0 Tiled, 1 Horizontal, 2 Vertical, 3 Cascade
  bool main_hidden = false;      // View > Hide on the workbook's first window (Unhide shows it)
  bool side_by_side = false;     // View > View Side by Side (two windows, one above the other)
  bool sync_scrolling = true;    // View > Synchronous Scrolling: the windows side by side scroll together
  uint32_t focus_win = 0;  // the focused window: 0 = the sheet's own window, else an ExtraWin id
  render::GridView* ExtraView(uint32_t id) {
    for (auto& w : extra_wins) {
      if (w.id == id) return &w.view;
    }
    return nullptr;
  }
  // the view the Name Box, Go To and Python's Select / Selection act on for `sheet`: the focused window's when it shows
  // that sheet (a second window from View > New Window or Side by Side), else the sheet's own. The Name Box selected
  // in the first window while the keys typed into the second one's active cell.
  render::GridView& ViewFor(uint32_t sheet) {
    if (focus_win && sheet == focus_sheet) {
      if (render::GridView* v = ExtraView(focus_win); v && v->sheet == sheet) return *v;
    }
    return Win(sheet).view;
  }
  render::GridView& FocusView() {
    if (focus_win) {
      if (render::GridView* v = ExtraView(focus_win); v && v->sheet == focus_sheet) return *v;
    }
    return wins[focus_sheet].view;
  }
  const model::Sheet* FocusSheet() const { return snap ? snap->Sheet(focus_sheet) : nullptr; }
  std::string SheetWindowName(uint32_t id) const;
  std::string ContainerWindowName(const Container& c) const;
  std::wstring LayoutPath() const;
  void ShowMessage(std::string title, std::string text);  // also recorded in the Errors window (as a message)
  void ShowError(std::string title, std::string text);    // a message box that is an error (Errors window, log file)
  // no message box: the text goes to the Error Log as an error and `brief` shows in red on the status bar for a while
  void NoteRefusal(std::string text, std::string brief);
  void OpenDialog(Dialog d) {
    dialog = d;
    dialog_open_request = true;
  }
  // windows that fit their contents and got narrower frame after frame (an item as wide as the window): a bug the
  // user sees as a dialog shrinking; ui.state lists them, ui_sweep fails on them
  std::set<std::string> shrinking_windows;
  // dialogs whose contents are wider than the dialog (a fixed width smaller than a table or box in it): the right
  // part is cut off; ui.state lists them, ui_sweep fails on them
  std::set<std::string> clipped_windows;
  // the top-level windows drawn in the last frame (dialogs, panes, popups): ui.state's "windows", so a test can watch a
  // window's size over time
  struct WindowRect {
    std::string name;
    float x = 0, y = 0, w = 0, h = 0;
    bool auto_size = false, popup = false;
  };
  std::vector<WindowRect> window_rects;
};

inline const char* const kGridHostWindow = TR("Workbook###yc_gridhost");
inline const char* const kChartWindow = TR("Chart###yc_chart");

// commands_setup.cpp
void RegisterCommands(AppState& s);
void ApplyUndoMove(AppState& s);  // each frame: the cells the last Undo / Redo changed, once done
void RedoOrRepeat(AppState& s);   // Ctrl+Y and F4: Redo after an Undo, otherwise Repeat (Excel)
void ApplyPendingCommit(AppState& s);  // each frame: an entry a busy engine refused comes back to the editor
// background error checking: the error options button beside the active cell when it breaks a rule (data_ui.cpp)
void DrawErrorCheckButton(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
engine::ErrorCheckOptions ErrorCheckingOptions();  // File > Options > Formulas, as the grid draws the triangles
void OpenErrorChecking(AppState& s);  // Formulas > Error Checking: the dialog walking the sheet's errors (data_ui.cpp)
void TraceError(AppState& s);         // Formulas > Error Checking > Trace Error
void SyncCircularMenu(AppState& s);   // each frame: Error Checking > Circular References lists the cells
void SyncUseInFormulaMenu(AppState& s);  // each frame: Formulas > Use in Formula lists the names (names_ui.cpp)
void OpenFormulaOptions(AppState& s);
void OpenGoToWith(AppState& s, std::vector<std::string> refs);  // Go To listing only these references (dialogs.cpp)
void FollowStructureChanges(AppState& s, const engine::Snapshot& snap);  // the watches follow inserted / deleted rows  // File > Options at its Formulas section (Error Checking Options..., file_ops.cpp)
void ArmCircularWarning(AppState& s);  // the user entered something / opened a file: warn when it makes a circular reference
void CheckCircularWarning(AppState& s);  // each frame
std::string CircularStatusText(const AppState& s);  // the status bar's text ("" when there is no circular reference)
void SyncCommandState(AppState& s);  // ribbon button states are derived from the active cell's style (PLAN §11.6)

// sheet_actions.cpp: engine operations on the current selection
std::vector<Rect> SelAreas(AppState& s);  // expanded according to the selection mode (entire rows / columns)
Rect SelRect(AppState& s);                // active area
// while editing a cell's text: change the font of the selected characters; false = no selection, a formula, or not editing
bool EditSelectionFormat(AppState& s, const std::function<void(model::RichRun&, const model::RunFont& at_start)>& fn);
bool EditHasTextSelection(const AppState& s);
void ApplyStyle(AppState& s, const std::string& label, std::function<void(model::Style&)> fn);
// the rows of areas that size themselves and need another height once restyle is applied (Wrap Text, a bigger font,
// a cell style): row -> height in px, to set in the same undo step as the change
std::vector<std::pair<uint32_t, int32_t>> FitRowsForStyle(AppState& s, uint32_t sheet, const std::vector<Rect>& areas,
                                                          const std::function<void(model::Style&)>& restyle, const std::string* font = nullptr);
void StartEdit(AppState& s, bool replace, std::string initial, bool in_formula_bar = false);
void CommitEdit(AppState& s, bool array, int move, bool cse = false);  // cse: Ctrl+Shift+Enter array formula
// the array formula (Ctrl+Shift+Enter) that (r,c) belongs to, or nullptr
const model::SpillRange* ArrayFormulaAt(const AppState& s, uint32_t sheet, uint32_t r, uint32_t c, uint32_t* anchor_r = nullptr, uint32_t* anchor_c = nullptr);
// whether the areas cut through an array formula; shows Excel's message when they do
bool RefusePartOfArray(AppState& s, uint32_t sheet, const std::vector<Rect>& areas);
void CancelEdit(AppState& s);
// wait = false: a busy engine gets the copy and the screen goes on (ApplyPendingCopy); Python's requests wait for it
void CopySelection(AppState& s, bool cut, bool wait = false);
void ApplyPendingCopy(AppState& s);    // each frame: a copy handed to the engine becomes the clipboard's once ready
void WaitForPendingCopy(AppState& s);  // a request that reads the copied cells now (Python)
// a paste asked for before the copy is ready: true when it was put off until then (it runs on the cells selected now)
bool PasteAfterCopy(AppState& s, std::function<void(AppState&)> paste);
// the engine still works on what came before: a command that needs its answer now would make the frame wait
bool EngineBehind(const AppState& s);
// true when `run` was put off until the engine has caught up (a Python request and a command already put off run now)
bool DeferWhileEngineBusy(AppState& s, std::function<void(AppState&)> run, bool keep_selection = true);
void RunDeferred(AppState& s);              // each frame
bool UiWaitsForEngine(const AppState& s);   // a scripted input waits for what was put off, as typing ahead in Excel
// text on the clipboard; a hidden instance (tests, agents) uses a clipboard of its own and never touches the user's
void SetClipboardTextUtf8(HWND hwnd, const std::string& text);
std::string ClipboardTextUtf8(HWND hwnd);
bool ClipboardHasText(HWND hwnd);  // without writing out the text of copied cells
// the window's clipboard messages: copied cells promise their text and write it when a program asks for it
// (WM_RENDERFORMAT of CF_UNICODETEXT; all_formats: WM_RENDERALLFORMATS, the window closes), as Excel does
void RenderClipboardText(AppState& s, bool all_formats);
void ClipboardReleased();  // WM_DESTROYCLIPBOARD: another copy replaced this window's
void PasteClipboard(AppState& s, engine::PasteMode mode = engine::PasteMode::All, bool transpose = false, bool skip_blanks = false,
                    engine::PasteOp op = engine::PasteOp::None);
void PasteLink(AppState& s);  // Paste Special > Paste Link: formulas that refer to the copied cells
// Excel's cut / copy mode: copied cells outlined; Esc, typing, insert / delete and sort end it (Paste then has nothing
// of those cells to paste)
bool CopyModeOn(const AppState& s);
void EndCopyMode(AppState& s);
// AutoFit Row Height / Column Width of every row (column) in spans, measured once, one undo step
// rows (columns only): measure the cells of these areas, as Home > Format > AutoFit Column Width and
// Range.Columns.AutoFit do; nullptr: the whole column (a double-click on the border)
void AutoFitDims(AppState& s, uint32_t sheet, bool cols, const std::vector<std::pair<uint32_t, uint32_t>>& spans, const std::vector<Rect>* rows = nullptr);
// today's date / the current time as typed by Ctrl+; and Ctrl+Shift+;
std::string TodayText();
void EditInsertText(AppState& s, const std::string& text);  // typed at the caret of the cell being edited
std::string NowTimeText();
void ClearSelection(AppState& s, int what);  // 0 contents, 1 formats, 2 all
void InsertOrDelete(AppState& s, bool insert, int what);  // what: 0 by selection, 1 entire row, 2 entire column, 3 shift down/up, 4 shift right/left
void AutoSum(AppState& s, const char* fn);
// Application.EnableLargeOperationAlert: true when it asked (the operation runs again on Yes)
bool AskBeforeLargeOperation(AppState& s, uint64_t cells, std::function<void()> again);
void FillSelection(AppState& s, int dir);  // 0 down, 1 right, 2 up, 3 left
void SortSelection(AppState& s, bool descending);
// Data > Sort, Custom Sort, Remove Duplicates: the range they work on, found as Excel does (data_ui.cpp): a table's
// data body (header and totals rows never move), the AutoFilter range, the current region around one cell, or the
// selection. warn: a multi-cell selection has data next to it (Excel's Sort Warning offers the expanded list).
struct DataRange {
  Rect rect{};
  bool header = false;  // the first row of rect is the header
  uint32_t table = 0;   // the table whose data body rect is (0 = none)
  bool warn = false;
  Rect expanded{};      // the list around the selection (warn)
  bool expanded_header = false;
};
DataRange FindDataRange(AppState& s);
bool GuessHeader(AppState& s, const model::Sheet& sh, const Rect& r);
// action: 0 Sort A to Z, 1 Sort Z to A, 2 Custom Sort, 3 Remove Duplicates; asks the Sort Warning first when r.warn
void RunDataAction(AppState& s, int action, const DataRange& r);
void SortDataRange(AppState& s, bool descending, const DataRange& r);     // sheet_actions.cpp
void OpenCustomSort(AppState& s, const DataRange& r);                     // misc_ui.cpp
void OpenRemoveDuplicates(AppState& s, const DataRange& r);               // data_ui.cpp
void SplitWindow(AppState& s);  // View > Split (toggle)
void FreezePanes(AppState& s, int kind);   // 0 toggle at the active cell, 1 top row, 2 first column
// panes unfrozen: the window scrolls back to where the frozen part began, so the rows and columns that were frozen stay
// in view, as Excel's (measured: frozen at C4 from the top, then unfrozen, ScrollRow is 1, not 4)
inline void UnfreezeView(render::GridView& v) {
  if (v.freeze_rows) v.v.first = v.frozen_top;
  if (v.freeze_cols) v.h.first = v.frozen_left;
  v.freeze_rows = v.freeze_cols = 0;
}
void Goto(AppState& s, const std::string& ref);
void ActivateSheet(AppState& s, uint32_t id);
// ---- cells_ui.cpp: cells and sheets by mouse and keyboard as in Excel ----
// grouped sheets: the sheets an edit made on `sheet` goes to (the group when sheet is its active sheet; else sheet)
std::vector<uint32_t> GroupTargets(AppState& s, uint32_t sheet);
void SetSheetGroup(AppState& s, std::vector<uint32_t> ids, uint32_t active);  // ids empty or one sheet: no group
void SheetTabClicked(AppState& s, uint32_t id, int how);  // 0 a click, 1 Ctrl+click, 2 Shift+click on a tab
// a Shift-drop of the selection's border (or the drop menu's Shift Down / Right and Copy / Move): the cells go in
// between rows (dir 1) or columns (dir 2) at (r, c), the others make room; a move closes up their old place
void InsertDrop(AppState& s, uint32_t sheet, const Rect& src, int dir, uint32_t r, uint32_t c, bool copy);
bool InsertCopiedAt(AppState& s, const Rect& at, int what);
// Paste Options > Picture / Linked Picture: a picture of cells src (of src_sheet) placed at `at`; a linked one is
// drawn again whenever those cells change
void PasteCellsPicture(AppState& s, uint32_t sheet, const Rect& at, uint32_t src_sheet, const Rect& src, bool linked);  // Insert Copied / Cut Cells at `at` (false: nothing copied)
void OpenMoveDropMenu(AppState& s, uint32_t sheet, const Rect& src, uint32_t r, uint32_t c);  // right-drag of the border
void OpenFillDropMenu(AppState& s, uint32_t sheet, const Rect& src, const Rect& dest);        // right-drag of the fill handle
std::string FillTipFor(AppState& s, uint32_t sheet, const Rect& src, const Rect& dest, bool ctrl);
void RegisterCellCommands(AppState& s);
// paste_ui.cpp: Excel's ways to paste (ids as AppState::LastPaste::option), as pictures in Home > Paste, the Paste
// Options button and the cell menu
void RegisterPasteCommands(AppState& s);
// File > Info > Manage Workbook > Recover Unsaved Workbooks: the Document Recovery pane (false: nothing to recover)
bool OpenRecoveryPane();
// quick_analysis.cpp: the Quick Analysis button at the selection's corner and Ctrl+Q
void RegisterQuickAnalysisCommands(AppState& s);
void DrawQuickAnalysis(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void DrawPasteGlyph(ImDrawList* dl, int id, ImVec2 min, float size, bool enabled);
void ApplyPasteWay(engine::Document& d, uint32_t sheet, const Rect& dest, const engine::ClipData& clip, int id);
void PasteWayNow(AppState& s, int id);
int PasteIconRow(const int* ids, size_t n, int current);  // the way clicked, or -1
int PasteGallery(int current);                             // Paste / Paste Values / Other Paste Options rows
void DrawCellsUi(AppState& s);  // every frame: the drop menus, the Activate list, Fill Across Worksheets, Pick From List
float DrawStatusViewButtons(AppState& s, const ImRect& bar, float right);  // Normal / Page Layout / Page Break Preview
bool ScrollLockOn();
void DrawSelectObjectsOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);  // Find & Select > Select Objects
void DrawCellMenuFilter(AppState& s);  // the cell menu's Filter submenu
io::MV CellsRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err);  // Python: cells.*
void ChangeDecimals(AppState& s, int delta);
void HandleGridActions(AppState& s, uint32_t sheet_id, const render::GridActions& act);
std::string SelectionAddress(AppState& s);
void ApplyFormatPainter(AppState& s);
// Home > Borders > Draw Border / Draw Border Grid / Erase Border (border_draw.cpp)
void RegisterBorderDrawCommands(AppState& s);
void UpdateBorderDrawCommands();
bool BorderDrawOn();
void BorderDrawOff();
const char* BorderDrawStatus();     // the status bar's hint while a mode is on (nullptr = off)
void BorderDrawArm(AppState& s);    // the grid saw a click on the cells
void BorderDrawFrame(AppState& s);  // every frame: draws when the mouse goes up
uint32_t BorderPenColor();          // Line Color (0 = automatic): the menu's borders use it too
void ApplyNumberFormat(AppState& s, const std::string& code, const std::string& label = TR("Number Format"));

// dialogs.cpp
void DrawDialogs(AppState& s);
// Excel's questions ("There's already data here. Do you want to replace it?"): OK runs yes, Cancel does nothing
void AskYesNo(AppState& s, std::string title, std::string text, std::function<void()> yes);
// the same question with buttons of its own: `ok` (Enter) runs `yes`, `other` runs `alt`, Cancel (Escape) neither
void AskChoice(AppState& s, std::string title, std::string text, std::string ok, std::function<void()> yes,
               std::string other, std::function<void()> alt);
// the open message or question's title and text ("" when neither is open): ui.state()["dialog_text"], for tests
std::string DialogText(const AppState& s);
// Function Arguments for fn (initial: the arguments already written); replace_cell: the call is the cell's formula
void OpenFunctionArgs(AppState& s, const std::string& fn, const std::vector<std::string>& initial, bool replace_cell);
bool OpenFunctionArgsForCell(AppState& s);
// Formulas > Function Library: the category menus' commands (InsertFn_SUM...); the functions used last
void RegisterFunctionLibrary(AppState& s);
void BeginFunctionEdit(AppState& s);  // fx on a cell not being edited: it shows "=" while the function is chosen
void UpdateFunctionLibraryMenu();
const std::vector<std::string>& RecentFunctions();
// the cell menu's PivotTable part when the active cell is in a report (Refresh, Sort, Summarize Values By...);
// false: not in a report
bool DrawPivotCellMenu(AppState& s);
// a number format chosen as Excel's Format Cells shows it: a category, its decimal places, a sample (true = changed)
bool NumberFormatPicker(std::string& code);
// a color swatch opening Office's palette (theme colors and shades, standard colors, More Colors); allow_auto adds
// auto_label (Automatic / No Color) as the first choice, stored as 0
bool PaletteColor(const char* label, uint32_t* argb, bool allow_auto, const char* auto_label);  // fx on a cell whose formula is one call (=SUM(A1:A9)): its arguments
void DrawContextMenus(AppState& s);
void OpenCellContextMenu();
// Shift+F10 / the Menu key: at the active cell; whole rows or columns selected open the row or column menu, as Excel's
// (kind 1 rows, 2 columns)
void OpenCellContextMenuAt(ImVec2 pos, int kind = 0);
bool ContextMenuOpen();                   // a cell, header or sheet tab menu is open
void OpenColHeaderContextMenu();
void OpenRowHeaderContextMenu();
void OpenSheetTabContextMenu(uint32_t sheet_id);
void BeginRenameSheet(AppState& s, uint32_t sheet_id);

// layout_io.cpp
bool SaveLayout(AppState& s);
bool LoadLayout(AppState& s, bool at_startup);
std::string CaptureLayoutJson(AppState& s);
bool ApplyLayoutJson(AppState& s, const std::string& json, bool restore_main_window);

// file_ops.cpp
bool NewWorkbook(AppState& s);
model::Workbook NewUserWorkbook();  // a blank workbook with Options > General's font, size and number of sheets
// .ycg or .csv; an encrypted workbook without the right password opens the password prompt (prompt) or fails with *error
bool OpenWorkbook(AppState& s, const std::wstring& path, const std::string& password = {}, bool prompt = true, std::string* error = nullptr);
bool OpenWorkbookDialog(AppState& s);
bool SaveWorkbook(AppState& s, bool save_as, bool background = false);  // background: Save / Save As (the screen goes on)
// events_ui.cpp: Excel's workbook and sheet events for the Python project
void TrackSheetEvents(AppState& s);  // end of every frame: SelectionChange, Activate / Deactivate, Before* timeouts
// a Before* event (Workbook_BeforeSave, Worksheet_BeforeDoubleClick...): act(s) runs now when nobody handles it, else
// once the handlers ran without setting Cancel (or didn't answer in time)
void AskThenAct(AppState& s, const std::string& event, uint32_t sheet, const std::vector<Rect>* target, io::MV extra, std::function<void(AppState&)> act);
void SaveWithEvents(AppState& s, bool save_as);  // Save / Save As as the user starts them: BeforeSave, then the save
void FireAfterSave(AppState& s, bool success);
void FireFollowHyperlink(AppState& s, uint32_t sheet, uint32_t r, uint32_t c);
bool AskBeforeClose(AppState& s);  // WM_CLOSE: true = Workbook_BeforeClose was asked (the answer closes again)
void BeforeCloseUndone(AppState& s);  // the close was cancelled later (the save prompt)
// what a Cell Styles command applies (for the live preview while its ribbon tile is hovered); empty when cmd isn't one
std::function<void(model::Style&)> CellStylePreview(const std::string& cmd);
// links_ui.cpp: links to other workbooks (=[Book2.ycg]Sheet1!A1): reading their values, Data > Edit Links
bool UpdateLink(AppState& s, size_t link, std::string* err);
bool UpdateAllLinks(AppState& s, std::string* first_err);
void UpdateLinksUi(AppState& s);  // every frame: read the sources when the workbook opens and when a source is saved
void RegisterLinkCommands(AppState& s);
void DrawLinksDialog(AppState& s);
// windows_ui.cpp: View > Window (New Window, Switch Windows, Arrange All); further workbooks open in their own windows
bool WorkbookInUse(const AppState& s);  // has a file, changes or data (an empty Book1 is replaced instead)
// the other YCGrid windows' workbooks (each its own process): name, file ("" = not saved yet), control pipe
struct OtherWorkbook {
  std::string name;
  std::wstring file;
  std::string pipe;
};
std::vector<OtherWorkbook> OtherWorkbooks(bool hidden_too = false);  // hidden_too: also YCGrids without a window (automation)
// Merge Styles: the styles of another workbook, asked of its running YCGrid over its pipe (unsaved changes included),
// else read from its file (ask: Excel's "Merge styles that have same names?"; otherwise they are replaced)
void MergeStylesFrom(AppState& s, const std::wstring& file, bool ask = true, const std::string& pipe = {});
bool OpenPasswordOk(const std::wstring& path, const std::string& password);  // an encrypted .ycg opens with this password (also: not encrypted)
// as_new: the file opens as a new, unsaved workbook ("Book2") and is deleted once read (a report made for it)
bool OpenInNewWindow(const std::wstring& path, const std::string& password = {}, bool as_new = false, uint32_t* pid = nullptr);  // a new YCGrid window ("" = a blank workbook); an open file's window is activated
bool ActivateWindowWithFile(const std::wstring& path);
void NewWindow(AppState& s);
void RegisterWindowCommands(AppState& s);
void UpdateWindowsUi(AppState& s);
void DrawWindowDialogs(AppState& s);  // Arrange Windows, Unhide (View > Window)
// View Side by Side with another YCGrid: the workbook to compare with (CompareSideBySideWith), the partner's pid, and
// what it tells this one over YCP (it started / ended, it scrolled)
void SetSideBySideWith(const std::string& name);
unsigned long SideBySidePartner();
void PartnerSideBySide(AppState& s, unsigned long pid, bool on);
void PartnerScrolled(AppState& s, int64_t dv, int64_t dh);
// ui_custom.cpp: Quick Access Toolbar, ribbon display options, command search, context menus from Python, Application.Dialogs
std::vector<std::string> QatCommands();
// the built-in panes by name, for Python's yc.ui.pane(): flag = whether it shows; command = the command that toggles it
// ("" = set the flag directly)
void RegisterPane(const std::string& name, const std::string& command, std::function<bool&()> flag);
std::map<std::string, bool> PaneStates();                 // which panes are shown (saved with a layout)
void SetPaneState(const std::string& name, bool shown);  // a pane of a loaded layout
// every modal dialog: ImGui::BeginPopupModal, and Ctrl+C (outside a text box) copies all the dialog's text
bool BeginModal(const char* title, bool* open, int flags = 0);
// a window fitting its contents (AlwaysAutoResize) with wrapped text keeps the width it opened at (it narrowed by itself
// every frame): KeepOpenedWidth(name) before ImGui::Begin(name, ...), NoteOpenedWidth() once Begin returned true
void KeepOpenedWidth(const char* name);
void NoteOpenedWidth();
// A file dialog (Open, Save As, Insert Picture...) for a window nobody can see (--hidden): it would wait on the desktop
// for nobody, so it isn't shown; logged like a message box answered automatically. true = don't open the dialog.
// true when nobody can see the window (a hidden instance): the dialog isn't shown. *answer is then the next path a
// test queued with ui.answer_file_dialogs ("" = cancelled); without answer the dialog counts as cancelled
bool NoFileDialog(HWND owner, const std::string& what, std::wstring* answer = nullptr);
void QueueFileDialogAnswers(const std::vector<std::wstring>& paths);  // "" in the list = Cancel
// a dialog's default button: clicked, or Enter pressed in the dialog (not in a multi-line text box, not while a list or
// menu of it is open, not on the frame the dialog appears)
bool DlgOkButton(const char* label, const ImVec2& size);
// where a floating tool window (Immediate, Error Log, Data View...) first opens, so it doesn't cover the ribbon, the
// Name Box and the formula bar (ImGui's default corner): centered, along the bottom, or at a side below the ribbon
enum class WindowPlace { Center, Bottom, Left, Right };
void PlaceNewWindow(WindowPlace where);
// a task pane (Clipboard, Format, Selection) docked beside the sheets the first time it shows in a session, as Excel's
// panes sit beside the grid: floating, the Clipboard pane lay over columns A:D and the cells it was for. Call before
// its Begin; left of the sheets or right, about `width` pixels wide. Moved or floated later, it stays where it is put
void DockAsTaskPane(const char* window_title, bool left, float width);
// a dialog's list (Data Analysis tools, chart types) by keyboard, as in Excel: Up / Down, Home / End, and a letter
// jumps to the next item starting with it. Call before drawing the list; returns true when the choice moved (scroll
// it into view: ImGui::SetScrollHereY() when drawing the chosen item)
bool DlgListKeys(int* sel, int count, const std::function<const char*(int)>& label);
// a dialog's tab bar: ImGui::BeginTabBar, and Ctrl+Tab / Ctrl+PgDn go to the next tab, Ctrl+Shift+Tab / Ctrl+PgUp to the
// one before (Excel's dialogs); every dialog's tabs use it
bool BeginDlgTabBar(const char* id);
// a check box over several others ((Select All)): ticked when all are, empty when none, mixed otherwise (Excel's filled
// square); a click on a mixed one ticks it
bool CheckboxMixed(const char* label, bool* all, bool mixed);
// an option's letter in a dialog (Excel's underlined one), while the dialog has the keyboard: the letter alone when no
// text box takes it, or with Alt (Ctrl+-, R, Enter deletes a row; Alt+C ticks Text to Columns' Comma)
bool DlgKey(ImGuiKey key);
// a dialog that isn't modal (Find and Replace, Select Data Source, Symbol...) calls this after its Begin: while it has
// the keyboard, Alt + a letter is the dialog's, never the ribbon's KeyTips behind it (Excel)
void NoteDialogKeyboard();
void FinishModalCopy(AppState& s);  // once a frame, after the dialogs
void RegisterUiCustomCommands(AppState& s);
void UpdateUiCustom(AppState& s);
bool RibbonHidden(AppState& s);  // Auto-hide Ribbon and not revealed
void DrawUiCustom(AppState& s);
void DrawCustomMenuItems(AppState& s, const char* bar);  // "Cell", "Row", "Column", "Ply"
io::MV CommandBarRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err);
bool ShowBuiltinDialog(AppState& s, int number, std::string* err);
bool DialogOpen(const AppState& s);
// editing_ui.cpp: custom lists, the Auto Fill Options button, AutoRecover, in-place IME composition
void ApplyCustomListSettings();  // settings.json -> the engine's AutoFill / Sort lists
void DrawCustomListsDialog(AppState& s);
void NoteAutoFill(AppState& s, uint32_t sheet, const Rect& src, const Rect& dest);
// whether the fill handle's default fill of src makes a series (else copies); *date: the first cell holds a date
bool FillMakesSeries(AppState& s, uint32_t sheet, const Rect& src, bool* date = nullptr);
// the cells' rectangle on the screen in a window (frozen panes and the T layout included)
ImRect CellsScreenRect(const model::Sheet& sh, const render::GridView& v, const Rect& r);
void DrawFillOptions(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void DrawPasteOptions(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);  // the (Ctrl) button after a paste
void DrawInsertOptions(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);  // the button after an insert
void OpenFind(AppState& s, bool replace);  // Find and Replace (modeless, as Excel's): the Find or the Replace tab
void FindNextKey(AppState& s);             // Shift+F4: the next match of the last search, without the dialog
// Go To Special's kinds as commands (Alt+; visible cells, Ctrl+Shift+O notes, Find & Select > Formulas...)
void SelectSpecial(AppState& s, int kind, int values = 1 | 2 | 4 | 16);
bool FindOpen();
bool NoteEditing();
bool EscClosedPopup();  // this frame's Escape closed a menu or flyout: it closes nothing else (main.cpp)
// renaming a sheet on its tab: the tab bar draws the box (main.cpp), then reports what happened
bool SheetRenaming(uint32_t* sheet, char** buf, size_t* cap, bool* focus);
void SheetRenameResult(AppState& s, bool drawn, bool commit, bool cancel);  // a note is being typed beside its cell (the sheet leaves the keyboard to it)
void UpdateAutoRecover(AppState& s);  // every frame
void AutoRecoverOnExit();             // a clean exit removes this session's AutoRecover copy
void DrawRecoveryPane(AppState& s);
bool HandleImeMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result);
void UpdateIme(AppState& s);
void DrawImeComposition(AppState& s);
bool ImeComposing();
void SimulateIme(const std::string& text, bool commit);  // UI scripts: the messages an IME sends
// styles_ui.cpp: Home > Cell Styles (built-in and custom named styles), Page Layout > Themes
void RegisterStyleCommands(AppState& s);
void UpdateStylesUi(AppState& s);  // every frame: the theme used for drawing, the gallery
void DrawStyleDialogs(AppState& s);
model::Style* StyleFormatTarget();  // Style > Format...: the Format Cells dialog edits this style instead of cells
void StyleFormatDone(AppState& s, bool ok);
std::string StyleFormatCode();  // the number style's own code (Comma: #,##0.00...) while its Format... is open
std::vector<std::string> ThemeMenuIds(int which);  // 0 themes, 1 colors, 2 fonts
bool SaveWorkbookTo(AppState& s, std::wstring path, int filter = 1, bool background = false);  // no dialog (filter 2 = CSV)
bool IsYcgSavePath(const std::wstring& path);  // not .xlsx / .csv / .txt / .tsv: saved as a .ycg
void FinishBackgroundSave(AppState& s, bool wait);  // every frame (wait=false); before exiting or replacing the workbook (true)
// a .ycg save in three steps: prepare and finish on the UI thread, the write (the slow part) on any thread, so a
// save asked for by Python doesn't stop the screen
struct YcgSave {
  std::wstring path;
  std::string layout_json;
  std::shared_ptr<const engine::Snapshot> snap;
};
YcgSave PrepareYcgSave(AppState& s, std::wstring path);
bool WriteYcgSave(const YcgSave& plan, std::string* err);
bool FinishYcgSave(AppState& s, const YcgSave& plan, bool ok, const std::string& err);
std::wstring NewProjectWorkbookPath();  // <projects folder>\BookN\BookN.ycg in a folder that doesn't exist yet
bool ExportCsvDialog(AppState& s);
bool ConfirmDiscard(AppState& s);  // asks when there are unsaved changes; returns false = cancel
void DrawBackstageContent(AppState& s, int page, const ImRect& area);
std::string AboutText();               // "YCGrid <version>" and the program's path, one per line
void DrawAboutMenu(AppState& s);       // the About menu on the tab row
void OpenProgramLocation(AppState& s);  // Explorer with ycgrid.exe selected (a hidden instance only says where)
void DrawFileDialogs(AppState& s);
// print_ui.cpp: Page Break Preview and Page Layout view (GridView::page_view)
void DrawPageViews(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void OpenPageSetupTab(AppState& s, int tab);  // the password prompt and Encrypt with Password
void UpdateWindowTitle(AppState& s);
std::vector<std::wstring>& RecentFiles();

// python_ui.cpp
void RegisterPythonCommands(AppState& s);
void StartPythonForWorkbook(AppState& s);  // starts the host when the workbook's folder has a main.py
void OpenVSCode(AppState& s);
// View Code on a control: the sheet's module in VS Code at <control>_<event>(), added empty when it isn't there
void ViewControlCode(AppState& s, uint32_t sheet, const std::string& control, const std::string& event);
std::string LastVsCodeRequest();  // what a hidden instance would have opened in VS Code (it opens none)
void DrawPythonUi(AppState& s);            // Immediate window, missing-packages bar
void PollProjectChanges(AppState& s);      // .py saved -> reload
float DrawPackageBar(AppState& s, const ImRect& r, bool measure_only);
void DrawPythonDialogs(AppState& s);

// chart_window.cpp
void DrawChartWindow(AppState& s);

// objects_ui.cpp: controls, charts, Design Mode, custom panels (PLAN §9, §10.3, §12.9)
void RegisterObjectCommands(AppState& s);
void DrawSheetObjects(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
bool ObjectsOwnKeyboard(AppState& s, uint32_t sheet_id);
// [insert] objects selected on the active sheet (not typing in one): Excel's keys act on them, not on the cells
bool ObjectSelected(const AppState& s);
bool ObjectShortcuts(AppState& s);  // Ctrl+C/X/V/D/1/B/I... on the selected objects; true when an object is selected
void RegisterShapeCommands(AppState& s);  // Shape Format / Picture Format, Arrange, the object clipboard
void UpdateShapeContext(AppState& s);     // every frame: the contextual tabs follow the selection; the Selection Pane
void SelectObject(AppState& s, uint32_t sheet, int kind, uint32_t id, bool replace);  // kind 1 control, 2 chart, 3 shape
std::vector<std::pair<int, uint32_t>> SelectedObjectIds();  // (kind, id), the primary first
void PasteAsPicture(AppState& s);  // a copied chart (or the copied cells) as a picture at the active cell
bool ObjectClipboardPending();  // objects were copied and nothing else since: Paste pastes them
bool PickPictureFile(AppState& s, std::string* bytes);       // illustrations_ui.cpp: Change Picture
void ShowProtectedMessage(AppState& s);                      // protect_ui.cpp: Excel's "...on a protected sheet" message
uint32_t ClipboardSequence();                                // sheet_actions.cpp: changes with every copy (any program)
uint32_t ClaimClipboard(AppState& s);  // objects were copied: the cells' copy ends; returns the new sequence number
void ClearObjectSelection();
bool HasSelectedChart();
// Developer > View Code with an ActiveX control selected: its default event's procedure (false: none selected)
bool ViewSelectedControlCode(AppState& s);
// a chart's picture (Chart.CopyPicture / Export, Paste as Picture): the cell at its upper-left corner, and where it was
// drawn on the screen last (false: no such chart, or not drawn)
bool ChartTopLeftCell(const AppState& s, uint32_t sheet, const std::string& name, uint32_t* row, uint32_t* col);
bool ChartScreenRect(const AppState& s, uint32_t sheet, const std::string& name, ImRect* rect);
void DrawObjectWindows(AppState& s);  // charts in their own windows, custom panels, Properties window
void DrawInsertChartDialog(AppState& s);
void RegisterChartCommands(AppState& s);  // Chart Design / Format tabs, chart sheets, Move Chart
void UpdateChartContext(AppState& s);
void DrawMoveChartDialog(AppState& s);
// select a chart and one of its elements (a model::ChartElem); format: also open the Format pane
void SelectChartElement(AppState& s, uint32_t sheet, uint32_t chart_id, int elem, int series, bool format, int point = -1);
void InsertChartFromSelection(AppState& s, model::ChartType type, bool combo = false);
// the chart the selection makes (false: no data there; *ch is an empty chart of that type) and the cells it comes from
bool BuildChartFromSelection(AppState& s, model::ChartType type, model::ChartObj* ch, Rect* anchor);
void InsertBuiltChart(AppState& s, model::ChartObj ch, Rect anchor);  // placed beside anchor, selected, undoable
const char* ChartTypeName(model::ChartType t);
bool SelectedChartObj(AppState& s, uint32_t* sheet, uint32_t* id);  // the selected chart or the chart sheet's
// chart_type_ui.cpp: Insert Chart (change = false) or Change Chart Type for the selected chart; combo_series: Change
// Series Chart Type (Combo, that series first); insert_type: Insert Chart on that type's kind (More ... Charts);
// recommended: on Recommended Charts
void OpenChartTypeDialog(AppState& s, bool change, int combo_series = -1, int insert_type = -1, bool recommended = false);
// Insert Chart for a new PivotChart (base: the PivotTable's chart with its series, where it goes)
void OpenPivotChartDialog(AppState& s, uint32_t sheet, const model::ChartObj& base);
// a shape or picture just inserted is selected, as in Excel: Delete removes it, the handles resize it
void SelectInsertedShape(AppState& s, uint32_t sheet, uint32_t id);
int SelectAllObjects(AppState& s, uint32_t sheet);  // Go To Special > Objects: selects every object shown; returns how many
// Insert > Shapes: the next drag (or click) on the sheet draws gallery entry code (an msoAutoShapeType, 0 a text box,
// engine::kLine... for lines, curves and freeforms)
void ArmShapeDrawing(AppState& s, int code);
bool ShapeDrawingArmed();
void MarkPlaceholderText(uint32_t shape_id);  // the first typing on the shape replaces its text (a new WordArt)
std::string ApplyAutoCorrect(const std::string& text);  // spelling.cpp: AutoCorrect's replacements in typed text (not formulas)
std::vector<std::pair<std::string, std::string>> AutoCorrectList();  // built-in (less the deleted) and the user's own
void DrawAutoCorrectList();  // the replacements with Delete buttons (inside the current window)
void AutoCorrectAdd(const std::string& what, const std::string& with);
void AutoCorrectDelete(const std::string& what);
// the clipboard's picture (sheet_actions.cpp): "PNG" or CF_DIB as PNG bytes; a hidden instance keeps its own
bool ClipboardImage(HWND hwnd, std::string* png);
void SetPrivateClipboardImage(const std::string& png);
// a picture (file bytes) inserted at the active cell at its own size, selected (illustrations_ui.cpp)
void InsertPictureBytes(AppState& s, std::string bytes);
bool BuildSeriesFromRange(const model::Workbook& wb, const model::Sheet& sh, Rect r, model::ChartType type, model::ChartObj* ch, bool by_rows);
// the same from several areas (Ctrl+selected columns side by side, rows one above the other), as Excel lays them out
bool BuildSeriesFromAreas(const model::Workbook& wb, const model::Sheet& sh, std::vector<Rect> areas, model::ChartType type, model::ChartObj* ch, bool by_rows);
bool PlotByRows(const std::vector<Rect>& areas);  // Excel's choice: series in rows when the block is wider than tall
// a rebuilt chart keeps each series' formatting (colors, labels, trendlines...) by position
void KeepSeriesFormat(const std::vector<model::ChartSeries>& old, std::vector<model::ChartSeries>* fresh);
// "Sheet1!$A$1:$B$5,Sheet1!$D$1:$D$5" (without "=") and back; every area on one sheet
std::string AreasAddress(const model::Workbook& wb, uint32_t sheet, const std::vector<Rect>& areas);
bool ResolveAreasText(const model::Workbook& wb, uint32_t default_sheet, std::string_view text, uint32_t* sheet, std::vector<Rect>* areas);
// a new chart type; treemaps / sunbursts lay out the source range again, waterfall / funnel charts show their values
void ChangeChartType(const model::Workbook& wb, model::ChartObj& c, model::ChartType t);
Rect ExpandToRegion(const model::Sheet& sh, Rect r);
void DrawChartBody(AppState& s, uint32_t sheet, const model::ChartObj& ch, const ImVec2& pos, const ImVec2& size, bool allow_input, float font_scale = 1.0f);
bool ResolveRangeText(const model::Workbook& wb, uint32_t default_sheet, std::string_view text, uint32_t* sheet, Rect* rect, int depth = 0);
// the selected chart and its selected element (a model::ChartElem; series: its index); false when none is selected
bool SelectedChartElem(uint32_t* sheet, uint32_t* id, int* elem, int* series);
int SelectedChartPoint();  // the selected chart element's point (a data point or one data label), -1: none

// chart_data_ui.cpp: Select Data Source, range boxes, the chart's data outlined on the sheet
// a range box (Excel's RefEdit): typed, or filled by selecting cells on the sheet after clicking it or its button.
// key names the box (unique among the open dialogs); returns true when the text changed. It lets go after one selection
// (or when it isn't drawn), so later clicks on the sheet don't change it
bool RefEdit(AppState& s, const char* key, std::string* text, float width);
// range_box_ui.cpp: a cell / range box in a dialog (modal or not), like Excel's RefEdit: typed, or its button shrinks
// the dialog to a bar while the user selects cells on the sheet; Enter brings the dialog back with the address.
// key names the box; label is shown on the bar. Returns true when the text was typed (picked text arrives later).
enum RangeBoxFlags : int {
  RangeBox_Formula = 1,   // a formula box: a pick after an operator ("=SUM(") adds the reference instead of replacing
  RangeBox_Sheet = 2,     // always write the sheet name (otherwise only when cells of another sheet are picked)
  RangeBox_Equals = 4,    // the text starts with "=" (Refers to, rule formulas)
  RangeBox_Relative = 8,  // A1 instead of $A$1
  RangeBox_Cell = 16,     // one cell (the selection's first)
};
bool RangeBox(AppState& s, const char* key, char* buf, size_t size, float width = 0, int flags = 0, const char* label = nullptr);
bool RangeBox(AppState& s, const char* key, std::string* text, float width = 0, int flags = 0, const char* label = nullptr);
bool RangePicking();          // a dialog is shrunk to a range box while cells are selected
void DrawRangePick(AppState& s);  // every frame, after the dialogs: the shrunk dialog's bar
void OpenSelectData(AppState& s, uint32_t sheet, uint32_t id);
bool SelectDataOpen();
void DrawSelectDataWindow(AppState& s);
// the chart as the open Select Data dialog changes it (drawn instead of the saved one), or nullptr
const model::ChartObj* ChartDataPreview(uint32_t sheet, uint32_t id);
// the selected chart's data outlined on the sheet (names red, categories purple, values blue) with draggable corners
void DrawChartDataHighlights(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
// an empty chart's Select Data button (r: the chart on the screen); true when it was pressed
bool DrawEmptyChartPrompt(AppState& s, uint32_t sheet, uint32_t id, const ImRect& r);
void NoteChartRect(uint32_t sheet, uint32_t id, const ImRect& r);
// =SERIES(name, X values, values, order): series i of c as Excel writes it, and back (false: not one)
std::string SeriesFormula(const model::ChartObj& c, size_t i);
bool ParseSeriesFormula(std::string_view text, std::string* name, std::string* x, std::string* values, int* order);
bool ApplySeriesFormula(AppState& s, uint32_t sheet, uint32_t id, int series, const std::string& text);  // undoable
// a selected series in the formula bar (its =SERIES formula, edited there) and the Name Box; false: none selected
bool DrawSeriesFormulaBar(AppState& s, const ImRect& fb);
bool SeriesNameBoxText(AppState& s, std::string* out);  // where a chart was drawn (dialogs open over it)
// a chart's series values from its cells or Python data (the screen and the printed page)
void ResolveChartValues(const engine::Snapshot& snap, uint32_t sheet, const model::ChartObj& ch, engine::ChartValues* out);
// a chart title as shown: "=Sheet1!$B$1" is that cell's text; an empty title shows placeholder ("Chart Title")
std::string ResolveChartText(const engine::Snapshot& snap, uint32_t sheet, const std::string& text, const char* placeholder);
void ForgetPanelDocking();
void DrawUserForm(AppState& s, const model::Panel& p);  // a UserForm shown from Python (a form panel)

// rules_ui.cpp: AutoFilter, data validation, conditional formatting
enum class ValidationResult { Accept, Retry, Cancel };
void RegisterRuleCommands(AppState& s);
// the conditional formatting rule to preview on the sheet: a Data Bars / Color Scales / Icon Sets entry under the
// mouse, or the open Highlight Cells Rules / Top Bottom dialog's rule; nullptr = none (rules_ui.cpp)
const model::CfRule* CfPreviewRule(AppState& s, const std::string& hovered);
void DrawRuleOverlays(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void DrawInputMessage(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void DrawRulePopups(AppState& s);
void DrawRuleDialogs(AppState& s);
ValidationResult CheckValidation(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::string& text);

// misc_ui.cpp: notes, hyperlinks, cell styles, table styles, Go To Special, custom sort, series, copy as picture, clipboard pane
void RegisterMiscCommands(AppState& s);
void DrawStaleOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);  // Format Stale Values
// Use GetPivotData functions: GETPIVOTDATA("Sum of Sales",$A$3,"Region","East") for a PivotTable's value cell
// pointed at while typing a formula ("" for any other cell, or with the option off)
std::string GetPivotDataFor(AppState& s, uint32_t sheet, uint32_t r, uint32_t c);
void DrawCellTips(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void DrawMiscWindows(AppState& s);
void DrawMiscDialogs(AppState& s);

// data_ui.cpp: Remove Duplicates, Text to Columns, outline, tracer arrows, Evaluate Formula, Watch Window
void RegisterDataCommands(AppState& s);
void DrawDataDialogs(AppState& s);
void RegisterSpellingCommands(AppState& s);  // Review > Spelling (spelling.cpp)
void DrawSpellingWindow(AppState& s);
// a word against the Windows spell checker (UI thread); false and suggestions when misspelled
bool SpellCheckWord(const std::string& lang, const std::string& word, std::vector<std::string>* suggestions);
void RegisterWhatIfCommands(AppState& s);
// [YC] safety for live trading (safety_ui.cpp)
void RegisterSafetyCommands(AppState& s);
// CommitEdit asks first: false = don't commit (the reason was shown; *cancel: end editing); *param = a parameter cell
// that was checked and confirmed (the Document lets this one edit through)
bool SafetyBeforeCommit(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::string& text, bool* param, bool* cancel);
void DrawFocusCell(AppState& s, uint32_t sheet, const render::OverlayGeom& g);  // View > Focus Cell (windows_ui.cpp)
void DrawSafetyOverlay(AppState& s, uint32_t sheet, const render::OverlayGeom& g);  // parameter cells, stale rows, ages
// [YC] Trading > Record / Replay (replay_ui.cpp): commands, the replay bar, status bar chips
void RegisterReplayCommands(AppState& s);
void DrawReplayFrame(AppState& s);
float DrawReplayStatus(AppState& s, ImDrawList* dl, const ImRect& bar, float left, float ty);
std::wstring DefaultRecordingPath(const AppState& s);  // a new file in <workbook folder>/recordings (Documents/YCGrid/recordings when unsaved)
float DrawSafetyStatus(AppState& s, ImDrawList* dl, const ImRect& bar, float left, float ty);  // mode chip, kill switch
void DrawSafetyFrame(AppState& s);  // alarms (banner and tint), idle timeout, the safety windows
void SafetyOnOpen(AppState& s, bool start_read_only);  // a workbook opened (or a new one)
void SafetyBeat(AppState& s, const std::string& name, double timeout, const std::string& detail);
void SafetyRemoveBeat(AppState& s, const std::string& name);
void SafetySetAlarm(AppState& s, const std::string& name, const std::string& text);  // "" text clears it
bool SetOperatorMode(AppState& s, bool on, const std::string& pin, std::string* error);
bool RunKillSwitch(AppState& s, bool ask, const std::string& source, std::string* error);
std::vector<std::string> SafetyAlarms(AppState& s);
// [YC] trading screens (trading_ui.cpp)
void DrawTradingOverlay(AppState& s, uint32_t sheet, const render::OverlayGeom& g);  // flashes, alert highlights, click actions
void DrawTradingFrame(AppState& s);  // alert rules, popups
bool HandleOnKeys(AppState& s);      // Application.OnKey: true = the key was taken
bool ParseOnKey(const std::string& text, int* chord);
io::MV CellContextMV(AppState& s, uint32_t sheet, uint32_t r, uint32_t c);
// [YC] yc.plot figures (figures_ui.cpp)
io::MV FigureRequest(AppState& s, const std::string& m, const io::MV& p, const std::shared_ptr<engine::RawFrame>& data, std::string* err);
io::MV PanelInput(AppState& s, const io::MV& p, std::string* err);  // panel.input: {panel, control, action click / set_value, value}
void SetFigureOwner(int64_t owner, const std::string& name, bool host);  // around FigureRequest: who sends it
void FiguresOwnerGone(int64_t owner);  // a connection closed: its figures close or show "(stopped)"
// [YC] plot items fed by topics (Axes.candles_live / line_live / step_live): any thread hands the updates over, the
// UI thread applies them before drawing
struct PlotLiveUpdate {
  std::string topic;  // without the "QUOTE|" prefix
  double value = 0;
  int64_t time_ns = 0;  // ns since the epoch
};
// [YC] figures as dockable windows: a keyed figure's window is "###yc_figk_<key>" (its place survives in layouts)
std::string FigureWindowName(const std::string& key_or_id);  // "" = no such figure
std::vector<std::string> FigureWindowNames();                 // the open keyed figures (layout capture)
bool PlotLiveWants();  // some item follows a topic
void PlotLiveFeed(const std::vector<PlotLiveUpdate>& updates);
// [YC] Data View (dataview_ui.cpp): tens of millions of rows of .npy columns in a window (yc.data.view)
io::MV DataViewRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err);
void DrawDataViews(AppState& s);
// the time machine: a figure request from a tape (not recorded again, not dropped); the figures as requests that rebuild
// them; backing up / restoring the figures a replay takes over
io::MV ReplayFigureRequest(AppState& s, const std::string& m, const io::MV& p, const std::shared_ptr<engine::RawFrame>& data, std::string* err);
std::vector<std::tuple<std::string, io::MV, std::shared_ptr<engine::RawFrame>>> FigureSnapshot();
void FigureBackup(const std::unordered_set<int64_t>& figs, bool restore);
void DrawFigures(AppState& s);
io::MV FiguresState();  // the open figures and their items (ui.state)
// What-If Analysis, Solver, Consolidate, Advanced Filter, Flash Fill (whatif_ui.cpp)
void DrawWhatIfDialogs(AppState& s);
// Data tab tools (datatools_ui.cpp): Subtotal, Forecast Sheet, From Text/CSV, the Flash Fill preview and options button,
// the tables' resize handle
void RegisterDataToolCommands(AppState& s);
void DrawDataToolDialogs(AppState& s);
void DrawDataToolOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
bool FlashFillAccept(AppState& s);  // Enter on a live Flash Fill preview fills the column (CommitEdit)
void FlashFillDone(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::vector<uint32_t>& rows, size_t undo_depth,
                   const std::vector<std::string>& texts);
// Data > Data Analysis (analysis_ui.cpp)
void RegisterAnalysisCommands(AppState& s);
void DrawAnalysisDialogs(AppState& s);
// runs Python code in the host like the Immediate window (starts it in a scratch folder when needed; python_ui.cpp)
// shown=false: run for a dialog, where the Immediate window may be closed (errors go to the Error Log)
void RunPythonInHost(AppState& s, const std::string& code, bool shown = true);
// the cells kept from the former Notebook window (notebook_ui.cpp, ADR 0011)
io::MV NotebookRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err);  // notebook.cells / export
// Trading > Latency Monitor (latency_ui.cpp)
void RegisterLatencyCommands(AppState& s);
void RegisterAlertCommands(AppState& s);  // alerts_ui.cpp: Trading > Alert Rules
void DrawAlertRules(AppState& s);
void DrawLatencyMonitor(AppState& s);
void DrawAuditOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
// Data Validation > Circle Invalid Data (Worksheet.CircleInvalid): circles the cells whose values break their rule, up
// to 255 as Excel; returns how many. ClearValidationCircles takes them away (Worksheet.ClearCircles)
int CircleInvalidData(AppState& s, uint32_t sheet_id);
void ClearValidationCircles(AppState& s, uint32_t sheet_id = 0);
int ValidationCircleCount(uint32_t sheet_id);
void DrawDataWindows(AppState& s);

// protect_ui.cpp: Review -> Protect Sheet / Workbook, Allow Edit Ranges
void RegisterProtectCommands(AppState& s);
void SyncProtectCommands(AppState& s);  // the Protect group's labels follow the sheet (Unprotect Sheet)
void DrawProtectDialogs(AppState& s);
void CheckProtectBlocks(AppState& s);  // shows Excel's message when the engine refused a change from the UI
bool SheetProtected(const AppState& s, uint32_t sheet);
bool RefuseLockedEdit(AppState& s, uint32_t sheet, uint32_t r, uint32_t c);  // true (and the message) for a locked cell
// read-only mode (Operator Mode off) or Protected View: true, with the status bar's message, for an action that would
// change the workbook. Called before running it, so a dialog never reads the engine's refusal (0, false, "") as
// "nothing found" and shows a wrong or blank message box
bool RefuseReadOnlyUi(AppState& s);
// a protected sheet's rules for the active cell: which cells are unlocked (Tab goes between them) and, when locked cells
// can't be selected, which can be; both left empty on a sheet without protection
void ProtectionCellRules(const model::Sheet& sh, const model::StyleTable* styles, std::function<bool(uint32_t, uint32_t)>* unlocked,
                         std::function<bool(uint32_t, uint32_t)>* selectable);
// commands that change the workbook don't run (nor open their dialogs) in read-only mode (safety_ui.cpp)
bool ReadOnlyRefusesCommand(const std::string& id);

// tables_ui.cpp: Insert -> Table, Format as Table, Table Design tab, totals row menu
void RegisterTableCommands(AppState& s);
void DrawTableDialogs(AppState& s);
// the table (or the range Format as Table would make one of) in the table style whose gallery tile is under the
// mouse, for the sheet's live preview; nullptr = none (tables_ui.cpp)
const model::Table* TableStylePreview(AppState& s, const std::string& hovered);
// Format Cells for something other than the cells (a table style's element): starts from start, shows the tabs in
// tabs (bit per tab: 1 Number, 2 Alignment, 4 Font, 8 Border, 16 Fill, 32 Protection) with inside lines, and hands
// back the fields chosen and the inside lines on OK (dialogs.cpp)
void OpenFormatCellsFor(AppState& s, const char* title, const engine::FormatSpec& start, unsigned tabs, model::BorderSide inner_h, model::BorderSide inner_v,
                        std::function<void(const engine::FormatSpec&, model::BorderSide, model::BorderSide)> done);
void DrawTableOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void UpdateTableContext(AppState& s);  // the Table Design tab and its check boxes follow the active cell
std::wstring PickImageFile(HWND owner);  // Open dialog for PNG / JPEG / BMP / GIF files ("" = cancelled)
std::wstring PickAnyFile(HWND owner, const char* title);  // Open dialog for any file ("" = cancelled)
void RegisterIllustrationCommands(AppState& s);  // illustrations_ui.cpp: pictures, shapes, text boxes, sparklines
void DrawIllustrationDialogs(AppState& s);
void UpdateSparklineContext(AppState& s);  // the Sparkline tab follows the active cell
// insert_gallery.cpp: Insert > Icons, Insert > WordArt
void RegisterInsertGallery(AppState& s);
void DrawInsertGallery(AppState& s);
void UpdateWordArtMenu();
uint32_t InsertWordArt(AppState& s, int style, const std::string& text);  // a WordArt text box at the active cell
std::vector<std::string> IconNames();
// print_ui.cpp: Insert > Header & Footer (Page Layout view, typing in the center header)
void StartHeaderFooterEditing(AppState& s);
// print_ui.cpp: File -> Print, PDF export, the Page Layout tab, Page Setup, page-break lines
void RegisterPrintCommands(AppState& s);
void DrawPrintBackstage(AppState& s, const ImRect& area);
void DrawExportPdfButton(AppState& s);
void DrawPageSetupDialog(AppState& s);
void DrawPageBreakOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void UpdatePrintContext(AppState& s);
// shape_effects.cpp: Format Shape > Effects on screen, the Shape Effects / Shape Styles menus, Themes > Effects
void DrawShapeOnScreen(ImDrawList* dl, const model::ShapeObj& sh, const ImRect& r, float scale);  // objects_ui.cpp: the shape alone
void DrawShapeFx(ImDrawList* dl, const model::ShapeObj& sh, const ImRect& r, float scale);        // with its effects
bool DrawShapeEffectsSection(model::ShapeObj& m);  // the Format Shape pane's Effects; true when changed
void RegisterShapeEffectCommands(AppState& s);
void UpdateShapeEffects(AppState& s);  // every frame
std::string ThemeEffectsName(AppState& s);
void ApplyShapeQuickStyle(AppState& s, uint32_t sheet, uint32_t id, int k);  // Shape.ShapeStyle (0-41)
bool SetThemeEffects(AppState& s, const std::string& name);
// File > Info's Properties (Title, Tags, Categories, dates, Author) and, on save, the dates and Last Modified By
void DrawInfoProperties(AppState& s);
void StampDocProperties(engine::Document& d);
// Page Layout > Background: the texture of the sheet's background picture and its size in pixels (0 = none)
ImTextureID SheetBackgroundTexture(AppState& s, uint32_t sheet, ImVec2* size);
// Python's layout.* requests (Page Layout, View tab and window members): print_ui.cpp
io::MV LayoutRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err);
// pivot_ui.cpp: Insert -> PivotTable, the PivotTable Fields pane, PivotTable Analyze / Design
void RegisterPivotCommands(AppState& s);
void UpdatePivotContext(AppState& s);  // the active cell's PivotTable (contextual tabs, the pane)
void DrawPivotFieldList(AppState& s);
bool PivotDoubleClick(AppState& s);  // true: the active cell is in a PivotTable report (handled: Show Details, expand / collapse...)
// text typed over a report's caption or label renames it (true: handled, the cell isn't written)
bool PivotCommitText(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::string& text);
// Group Selection: the selected labels of one field become a group (a new field "Region2"); err: why not
bool PivotGroupCells(engine::Document& d, uint32_t sheet, const Rect& sel, std::string* err);
void DrawPivotDialogs(AppState& s);
void DrawPivotOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
void DrawSlicers(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);
// a slicer or timeline selected by a click (its Slicer tab, Delete, Escape); cleared by a click on the cells
bool SlicerSelected(uint32_t sheet_id);
void ClearSlicerSelection();
// a slicer copied (Ctrl+C, Slicer.Copy) and nothing copied since: Paste pastes it at the active cell
bool SlicerClipboardPending();
bool CopySlicerNamed(AppState& s, uint32_t sheet, const std::string& name, bool cut);
// a label of a PivotTable report dragged to another label: its item moves there (true: the drop was in a report)
bool PivotMoveCells(AppState& s, uint32_t sheet, const Rect& src, uint32_t r, uint32_t c);
// Data > Group / Ungroup (Alt+Shift+Right / Left) in a PivotTable: Group Selection / Ungroup (true: handled)
bool PivotOutline(AppState& s, bool group);
// custom_ribbon.cpp: RibbonX (customUI.xml in the project folder)
void UpdateCustomRibbon(AppState& s);  // every frame: reload on change, ask Python for get* values
void SetRibbonValue(AppState& s, const std::string& key, const std::string& value);
void InvalidateRibbon(AppState& s, const std::string& control);  // "" = every control
void ShowPageBreaks(AppState& s, uint32_t sheet);  // turns the sheet's page-break lines on (after printing or a page setup change)
bool ExportPdf(AppState& s, const std::wstring& path, const std::vector<uint32_t>& sheets, bool ignore_area, int from, int to, std::string* err,
               const Rect* area = nullptr);
bool PrintSheets(AppState& s, const std::vector<uint32_t>& sheets, const std::wstring& printer, int from, int to, int copies, bool collate,
                 const std::wstring& to_file, std::string* err);
int PageCount(AppState& s, uint32_t sheet);

// names_ui.cpp: defined names (Name Box, Name Manager, Define Name, Create from Selection, Paste Name)
void RegisterNameCommands(AppState& s);
void DrawNameDialogs(AppState& s);
std::string NameForSelection(AppState& s);  // returns the name when the selection is exactly a defined name (shown in the Name Box)
void NameBoxEnter(AppState& s, const std::string& text);
void DrawNameBoxList(AppState& s);

// main.cpp: pictures (decoded with WIC) as textures
bool ImageSize(const std::string& bytes, int* w, int* h);
ImTextureID TextureFromImage(const std::string& bytes, int* w = nullptr, int* h = nullptr);  // 0 when it can't be decoded
void ReleaseTexture(ImTextureID t);
// engine thread: a control's value set by the user (or a simulated click): its linked cell follows (TRUE / FALSE for check boxes,
// option and toggle buttons), and an option button that turns on turns the others in its group off
void ApplyControlValue(engine::Document& d, uint32_t sheet, uint32_t id, const std::string& value, int list_index);
bool DecodeImageRgba(const std::string& bytes, std::vector<uint8_t>* px, int* w, int* h);  // any thread
ImTextureID TextureFromRgba(const uint8_t* px, int w, int h);

// main.cpp: simulated input (shared by --keys and MCP yc_ui_input)
std::string QueueUiScript(const std::string& script);  // "" or why the script was refused (an unknown key)
bool UiScriptBusy();
// [YC] ui.cell_rect: a range of the active sheet in the window pixels ui.input takes (scrolled into view first)
bool RangeWindowRect(const std::string& addr, float out[4]);
// where Page Layout view drew the range last frame (its first cell's page); false: not on a page shown
bool PageLayoutRangeRect(uint32_t sheet, const Rect& a, ImRect* out);
// a Windows message box on the UI thread - except in a hidden instance or while an agent's input / macro runs with
// automatic dialogs, where nobody could answer it: then the text goes to the Immediate window and the Error Log and
// auto_answer (IDOK, IDYES, IDCANCEL...) is returned at once
int AppMessageBox(AppState& s, const std::string& text, const std::string& title, unsigned flags, int auto_answer);

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

// [YC] before ImGui::Begin of a dockable window: a locked one (AppState::WindowLocked) gets a class without undocking,
// docking, splitters, a close button or the window menu; returns the window flags to add (no moving or resizing)
int LockWindow(bool locked);
// docked windows (dock_tabs.cpp): every docked window shows its tab; a double-click, the tab's right-click menu or the
// tab bar's menu floats it, unless the workbook locks it
void InitDockTabs();
void DrawDockTabs(AppState& s);
void FloatDockedWindow(struct ImGuiWindow* w);
bool DockTabMenuOpen();  // a tab's menu is open: the arrows and Enter choose in it

}  // namespace yc::app
