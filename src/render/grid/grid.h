// SPDX-License-Identifier: Apache-2.0
// grid.h - the grid (PLAN §8): draws from the snapshot, handles selection and edit input
//
// Only the visible area (visible rows x visible columns of each pane) is visited, independent of the sheet size (PLAN §8.0).
// This module doesn't know the engine: operations that change the model (commit input, resize columns, AutoFill...) are returned to the caller as GridActions.
#pragma once

#include "core/geometry.h"
#include "engine/snapshot.h"
#include "model/sheet.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <unordered_set>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace yc::engine {
struct ErrorCheckOptions;
}

namespace yc::render {

using core::Rect;

// Excel's mouse cursors that ImGui has no shape for: the grid passes them to ImGui::SetMouseCursor (ImGui accepts
// values past ImGuiMouseCursor_COUNT), the app shows them (main.cpp) and ui.state reports the last one by name
enum GridCursor : int {
  kCursorCell = ImGuiMouseCursor_COUNT + 1,  // the thick white cross over cells
  kCursorFill,                               // the thin black cross over the fill handle
  kCursorColHeader,                          // the black down arrow over column headers
  kCursorRowHeader,                          // the black right arrow over row headers
  kCursorColUnhide,                          // the split (two bars) just right of hidden columns
  kCursorRowUnhide,                          // the split just below hidden rows
};
// Worksheet.EnableFormatConditionsCalculation: sheets whose conditional formats aren't evaluated again (UI thread)
std::unordered_set<uint32_t>& CfFrozen();
const char* CursorName(int cursor);
int& LastFrameCursor();  // the cursor the last frame asked for (set by the app after drawing)
void* NativeCursor(int cursor);  // the HCURSOR of one of these (nullptr for ImGui's own cursors)
// its 32 x 32 design as Excel draws it ('#' shape, 'o' outline, '.' clear) and its hotspot (tests compare the sizes)
std::vector<std::string> CursorDesign(int cursor, int* hot_x, int* hot_y);

// Excel's column width units: characters of the default font (7 px each) plus 5 px of padding; 8.43 = 64 px.
// The Column Width dialog, the border-drag tip and Python's Range.ColumnWidth all convert with these.
inline double ColPxToChars(int32_t px) { return px <= 5 ? 0.0 : (px - 5) / 7.0; }
inline int32_t CharsToColPx(double chars) { return chars <= 0 ? 0 : static_cast<int32_t>(chars * 7.0 + 5.0 + 0.5); }

struct ScrollAxis {
  int64_t first = 0;   // first visible row / column of the scrollable pane
  int64_t extent = 0;  // scroll range (number of items); frozen while the scroll box is dragged
  bool thumb_drag = false;
  float drag_mouse0 = 0.0f;
  int64_t drag_px0 = 0;
  int repeat_part = 0;
  double repeat_next = 0.0;
  float wheel_accum = 0.0f;
};

enum class SelectMode : uint8_t { Cells, Rows, Cols, All };

struct Selection {
  std::vector<Rect> areas{Rect{0, 0, 0, 0}};  // multiple selection (Ctrl+click); the last one is active
  uint32_t act_r = 0, act_c = 0;               // active cell
  uint32_t anc_r = 0, anc_c = 0;               // anchor of the current area
  SelectMode mode = SelectMode::Cells;
  const Rect& Active() const { return areas.back(); }
  bool IsSingleCell() const { return areas.size() == 1 && areas[0].IsCell(); }
  // Excel: Tab across a row, then Enter goes to the next row in the column the Tabs started from. The chain holds
  // while the active cell is where the last Tab left it (a click or an arrow key elsewhere ends it).
  uint32_t tab_from = UINT32_MAX, tab_r = 0, tab_c = 0;
  uint32_t TabFrom() const { return tab_from != UINT32_MAX && tab_r == act_r && tab_c == act_c ? tab_from : UINT32_MAX; }
  // call before a Tab moves the active cell (from_c = the column it leaves), and NoteTabDone after. A Shift+Tab keeps
  // a chain going but never starts one (Excel 16, measured: Shift+Tab, then Enter goes straight down)
  void NoteTab(bool forward) { tab_from = TabFrom() != UINT32_MAX ? tab_from : forward ? act_c : UINT32_MAX; }
  void NoteTabDone() { tab_r = act_r, tab_c = act_c; }
};

// a reference in a formula's text: where it is ([begin, end) bytes), what it points at (sheet -1 = the formula's own)
// and its color (the same for the box on the sheet and the text in the edit box)
struct FormulaRefSpan {
  int begin = 0, end = 0;
  int32_t sheet = -1;
  Rect rect{};
  ImU32 color = 0;
};
std::vector<FormulaRefSpan> FormulaRefSpans(const std::string& text, const formula::SheetResolver* res, core::CellPos origin, bool r1c1);

// cell editing (PLAN §9.2): Enter mode (arrow keys commit and move) and Edit mode (F2, arrow keys move the caret)
struct EditState {
  bool active = false;
  uint32_t view_id = 0;  // the window it started in (GridOptions::view_id)
  bool enter_mode = true;
  uint32_t sheet = 0, r = 0, c = 0;
  std::string text;
  std::string original;
  bool in_formula_bar = false;
  bool focus_request = false;  // give keyboard focus to the edit box on the next frame
  bool focus_waiting = false;  // focus was asked for and the box hasn't the keyboard yet (keys of that frame go to the caret)
  int caret = 0;               // byte position (updated by the InputText callback)
  int sel_start = 0, sel_end = 0;
  // point mode (clicking a cell while typing a formula inserts a reference)
  int ref_begin = -1, ref_end = -1;  // position of the reference being inserted in text
  bool r1c1 = false;                 // the text uses R1C1 references (File -> Options)
  bool ref_dragging = false;
  int comma_at = -1;  // Ctrl+click while pointing: where its comma is, kept until the mouse is released
  uint32_t ref_r0 = 0, ref_c0 = 0;
  int ref_whole = 0;  // the reference being pointed is whole columns (1) or whole rows (2): a header click or drag
  // a colored box's handle or border is being dragged (Excel: resize or move the reference): 1 resize, 2 move
  int box_drag = 0;
  uint32_t box_grab_r = 0, box_grab_c = 0, box_h = 0, box_w = 0;  // move: the grabbed cell's offset, the box size
  std::string box_orig;  // the reference's text when the drag started (its $ signs are kept)
  std::vector<FormulaRefSpan> ref_spans;  // the references in text and their colors (set by the grid each frame)
  std::string ref_spans_text;             // the text they were found in
  // keyboard pointing (arrow keys pick references in Enter mode; Shift extends)
  bool kpoint = false;
  bool kp_end = false;  // End was pressed while pointing: the next arrow jumps as Ctrl+arrow (Excel's End mode)
  uint32_t kp_r = 0, kp_c = 0, kp_ar = 0, kp_ac = 0;
  ImRect field_rect;  // position of the in-cell edit box this frame (a click inside moves the caret instead of pointing)
  // formula autocomplete
  int ac_sel = 0;
  bool ac_scroll = false;
  std::string ac_hidden_token;  // prefix for which Esc closed the list (not shown again until the prefix changes)
  // pending changes for the InputText (applied in the next callback)
  bool pending_replace = false;
  std::string pending_text;
  int pending_caret = -1;
  int pending_sel_from = -1;  // with pending_caret: the text from here to the caret starts selected (AutoSum's range)
  bool cursor_to_end = false;
  // the entry began in a percent cell: Excel's automatic percent entry put a % after the caret; a % typed just before
  // it takes its place instead of making two (5, then %, is 5%)
  bool auto_pct = false;
  // AutoComplete (Excel): the text entries of the column around the cell; typing the start of one finishes it
  std::vector<std::string> column_words;
  uint64_t column_words_for = 0;  // which cell they were gathered for (sheet, row, column)
  int ac_prev_len = 0;            // the length typed (without a completion): shorter after an edit is a deletion, no completion
  bool ac_first = false;          // the entry began by typing: its first character completes too (one Chinese character)
  std::string ac_full;            // the entry AutoComplete suggests (Enter takes it as written there: APP is Apple)
  // Ctrl+Z / Ctrl+Y in the editor (ImGui's own undo is off): the text before each run of typing, as Excel takes a whole
  // run back at once (a caret move starts the next run); an entry typed over a cell takes back to its old content
  struct UndoStep {
    std::string text;
    int caret = 0;
    int sel_from = -1;  // the cell's own content comes back selected: typing replaces it (Excel 16: abc, Ctrl+Z, x is x)
  };
  std::vector<UndoStep> undo, redo;
  bool undo_run = false;   // typing goes on in the run the last step began
  std::string undo_last;   // the text at the last callback (what a new run takes back to)
  int undo_last_caret = 0;
  bool autocomplete = true;       // File > Options: Enable AutoComplete for cell values
  // File > Options > Formulas: Formula AutoComplete (the list of names), function ScreenTips (the argument hint)
  bool formula_list = true, formula_tips = true;
  float click_x = -1.0f;          // opened by a double-click: the caret goes to the character under this screen x
};

struct GridView {  // = Excel's Window (one per sheet)
  uint32_t sheet = 0;
  // the text the active cell showed when last drawn, fitted to its width as General fits it (Increase / Decrease
  // Decimal starts from the decimals General shows there, Excel 16, measured), and the frame it was drawn in
  std::string active_shown;
  int active_shown_frame = -1;
  // the table whose column names the column headings show in the last frame (0: the letters): the active cell in a
  // table whose header row is scrolled out of view, as Excel shows it
  uint32_t table_headings = 0;
  ScrollAxis v, h;
  Selection sel;
  float zoom = 1.0f;
  int page_view = 0;  // Window.View: 0 Normal, 1 Page Break Preview, 2 Page Layout (drawn by the app's overlay)
  uint32_t freeze_rows = 0, freeze_cols = 0;  // frozen rows / columns (0 = not frozen)
  uint32_t frozen_top = 0, frozen_left = 0;   // first row / column of the frozen area
  // View > Split: the same panes as frozen ones, but each scrolls on its own (frozen_top / frozen_left scroll the top / left panes)
  bool split = false;
  int split_drag = 0;  // dragging a split bar: 1 the horizontal one (rows), 2 the vertical one (columns)
  // [YC] T layout (an option chain: calls | strike | puts): the frozen columns stay in the middle of the window; the
  // columns before them (frozen_left > 0) show on their left, right-aligned against them, and scroll outward together
  // with the main pane (one scroll bar: scrolling right reveals the outer columns on both sides)
  bool mirror_left = false;
  float mirror_w = 0.0f;  // drawn width of that left pane (px, set every frame; 0 when off)
  bool TLayout() const { return mirror_left && freeze_cols > 0 && frozen_left > 0 && !split; }
  bool dragging = false;
  bool deselecting = false;  // a Ctrl+click (drag) that started on selected cells: they leave the selection on release
  // Excel's keyboard modes, shown on the status bar: F8 Extend Selection (arrows and clicks extend), Shift+F8 Add to
  // Selection (a click adds an area), End (the next arrow jumps like Ctrl+arrow); Escape ends them
  bool extend_mode = false, add_mode = false, end_mode = false;
  size_t add_keep = 0;  // Shift+F8: the areas selected before it, kept while the arrows move the cell added to them
  // the key that ended an entry in Enter mode (GridActions::nav_key), handled as a key on the sheet once the editor is
  // gone: Ctrl+Home, Shift+Right, End...
  struct NavReplay {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false, shift = false;
    int frame = 0;  // when it was noted: one not handled within a few frames (Page Layout view) is dropped
  } nav_replay;
  // fill handle drag
  bool fill_dragging = false;
  Rect fill_target{};
  // dragging the selection border: move (or copy with Ctrl) the cells
  bool move_dragging = false;
  Rect move_src{}, move_target{};
  uint32_t move_grab_r = 0, move_grab_c = 0;  // grabbed cell, relative to move_src
  // Shift held while dragging the border: the cells are inserted between rows (1) or columns (2) at (ins_r, ins_c)
  // instead of dropped over others (Excel's gray insertion bar)
  int move_insert = 0;
  uint32_t ins_r = 0, ins_c = 0;
  // the drag was made with the right button (the border or the fill handle): a menu of drop choices on release
  bool right_drag = false;
  bool drag_cancelled = false;  // Escape during a drag: nothing happens on release (Excel)
  // the fill handle's tip: the value the last cell gets (asked of the app when the target changes)
  Rect fill_tip_for{};
  bool fill_tip_ctrl = false;
  std::string fill_tip;
  // row height / column width drag
  int resize_kind = 0;  // 0 none, 1 column, 2 row
  uint32_t resize_index = 0;
  float resize_start_mouse = 0;
  int32_t resize_start_size = 0;
  int32_t resize_preview = -1;
  bool resize_hidden = false;  // the drag started just past hidden columns / rows: it unhides the last of them
  // per-frame output
  int64_t last_bottom_row = 0, last_right_col = 0;
  int64_t vis_rows = 0, vis_cols = 0;
  ImRect cells_rect;
  ImRect grid_rect;  // the whole grid (headers and scroll bars included)
  double px_scale = 1.0;  // DPI x zoom of this frame (sheet pixels -> screen pixels)
};

// coordinate conversion for overlay objects (controls, charts): sheet coordinates are pixels at 100% zoom (origin = upper-left of A1)
// a table's style on one of its cells (header, totals row, bands, first / last column); the cell's own fill and font color win
// (store: the workbook store holding its own table styles, New Table Style)
void ApplyTableLook(const model::Table& t, uint32_t r, uint32_t col, model::Style* s, const std::map<std::string, std::string>* store = nullptr);

struct OverlayGeom {
  ImRect cells;  // cell area (overlay objects are only drawn inside it)
  double sc = 1.0;
  std::function<ImVec2(double x, double y)> to_screen;  // sheet coordinates -> screen (scrolling and frozen panes included)
  std::function<ImVec2(ImVec2 p)> to_sheet;             // screen -> sheet coordinates
};

struct GridOptions {
  bool show_gridlines = true;
  bool show_headings = true;
  // File > Options > Advanced, Display options: this sheet's zeros and gridline color (0 automatic, else ARGB), the
  // workbook's scroll bars (the sheet tabs are hidden by passing no TabBarIO)
  bool show_zeros = true;
  uint32_t gridline_color = 0;
  bool show_vscroll = true, show_hscroll = true;
  bool keyboard = false;   // this window has focus and handles the keyboard
  bool show_formulas = false;  // Ctrl+`: show formulas
  const engine::ErrorCheckOptions* error_check = nullptr;  // background error checking: a green triangle (null: off)
  bool r1c1 = false;           // R1C1 reference style: numbered column headings, R1C1 formulas
  const Rect* copy_marquee = nullptr;  // copied range (marching dashed box)
  uint32_t copy_sheet = 0;
  bool marquee_no_paste = false;  // the marquee shows Format Painter's source: Enter doesn't paste
  bool design_mode = false;
  // what the mouse may change (off: the drag isn't offered, no cursor, no refusal afterwards):
  // Application.CellDragAndDrop and an unprotected sheet -> drag the selection's border, the fill handle;
  // an unprotected sheet or one allowing it -> drag column / row borders
  bool cell_drag_drop = true;
  bool resize_cols = true, resize_rows = true;
  bool chart_sheet = false;  // a chart sheet: no cells, only the overlay (its chart) and the sheet tabs
  uint32_t view_id = 0;  // which window of the sheet this is (View > New Window); the cell editor shows in the one it started in
  // the grid of the sheet the main window shows: it keeps an entry typed on another sheet live (a formula pointing
  // here), and only it, or every grid of another sheet took the same keys and typed them twice
  bool host_away_edit = false;
  // draws overlay objects over the cells; the objects' own ImGui items get the mouse first (the grid button allows overlap)
  std::function<void(const OverlayGeom&)> overlay;
  // live preview (hovering a Cell Styles tile): drawn as if preview_style were applied to preview_areas; the workbook
  // isn't changed
  std::function<void(model::Style&)> preview_style;
  const std::vector<Rect>* preview_areas = nullptr;
  // live preview of a conditional formatting rule (hovering Data Bars / Color Scales / Icon Sets, a Highlight Cells
  // Rules dialog open): drawn as if the rule were the sheet's first; the workbook doesn't change
  const model::CfRule* preview_rule = nullptr;
  // live preview of a table style (hovering a Format as Table / Table Styles tile): the table (its range, options and
  // the hovered style) drawn over its cells instead of their own table look; the workbook doesn't change
  const model::Table* preview_table = nullptr;
  // Page Layout > Background: the sheet's picture (tiled behind the cells) and its size in pixels; 0 = none
  ImTextureID background{};
  ImVec2 background_size{};
  // IMAGE() pictures: the texture of a URL and its pixel size (0 while it is still downloading)
  std::function<ImTextureID(const std::string& url, ImVec2* size)> image_for;
  // where Enter moves in Ready mode (File > Options > After pressing Enter, move selection): 0 doesn't move, 1 down,
  // 2 up, 3 right, 4 left (Shift+Enter goes the other way)
  int enter_dir = 1;
  // a protected sheet (Protect Sheet > "Select locked cells" / "Select unlocked cells"): which cells may be selected
  // (clicks and arrow keys skip the others); null = all
  std::function<bool(uint32_t r, uint32_t c)> selectable;
  // a protected sheet: Tab / Shift+Tab (and Enter) go to the next / previous unlocked cell in reading order (Excel)
  std::function<bool(uint32_t r, uint32_t c)> unlocked;
  // the value the fill handle's drag gives the last cell (Excel's tip while dragging); ctrl: Ctrl is held
  std::function<std::string(const Rect& src, const Rect& dest, bool ctrl)> fill_tip;
  // what pointing at one cell writes in a formula instead of its address ("": the address): a PivotTable's value
  // cell writes GETPIVOTDATA(...) (File > Options > Formulas > Use GetPivotData functions)
  std::function<std::string(uint32_t sheet, const Rect& r)> point_text;
  bool show_outline = true;  // Ctrl+8 / Window.DisplayOutline: the outline's bars and level buttons
  bool scroll_lock = false;  // Scroll Lock is on: the arrow keys scroll the window, the active cell stays (Excel)
};

// sheet tab bar (only the main window's grid has one)
struct SheetTab {
  uint32_t id;
  std::string name;
  uint32_t color = 0;
  bool floating = false;
};
struct TabBarIO {
  std::vector<SheetTab> tabs;
  uint32_t active = 0;
  float* split_ratio = nullptr;
  float* first_tab_scroll = nullptr;
  // outputs
  int64_t tear_off = -1;           // id of the sheet dragged out
  int64_t activate = -1;           // id of the clicked sheet
  int64_t activate_floating = -1;  // clicked a floating sheet
  int64_t redock = -1;
  int64_t rename = -1;             // double-click (rename)
  int64_t context = -1;            // context menu
  // grouped sheets (Ctrl / Shift + click on tabs): every selected tab is drawn white like the active one (Excel)
  std::vector<uint32_t> selected;
  int64_t group_toggle = -1;       // Ctrl+click: this tab joins or leaves the group
  int64_t group_extend = -1;       // Shift+click: the tabs from the active one to this one are grouped
  bool activate_list = false;      // right-click on the scroll arrows: Excel's Activate list of sheets
  int64_t move_from = -1, move_to = -1;  // drag to reorder (move_to == tabs.size(): after the last one)
  bool move_copy = false;                // Ctrl held at the drop: a copy goes there (Excel)
  bool add_sheet = false;
  // renaming on the tab itself (Excel): the caller's buffer; outputs: Enter / a click away commit, Escape cancels
  int64_t renaming = -1;
  char* rename_buf = nullptr;
  size_t rename_cap = 0;
  bool rename_focus = false;
  bool rename_drawn = false, rename_commit = false, rename_cancel = false;
  ImRect bar_rect;
};

// operations the grid asks the caller to perform
struct GridActions {
  bool clicked = false;              // got focus
  bool commit_edit = false;          // commit input (edit.text is written to edit's cell)
  bool commit_array = false;         // Ctrl+Enter: write the whole selection
  bool commit_cse = false;           // Ctrl+Shift+Enter: one array formula for the selection
  int commit_move = 0;               // move after committing: 0 stay, 1 Enter, 2 Shift+Enter, 3 Tab, 4 Shift+Tab, 5-8 arrow down / up / right / left
  bool cancel_edit = false;
  bool clear_contents = false;       // Delete
  int64_t table_add_row = -1;        // Tab in a table's last cell: the table (id) gets a row at its end (Excel)
  bool paste_enter = false;          // Enter while copied cells are outlined: paste them here and end copy mode (Excel)
  bool autofill = false;             // fill handle
  Rect autofill_src{}, autofill_dest{};
  bool move_cells = false;           // the selection border was dragged: move_src -> (move_r, move_c)
  bool move_copy = false;            // Ctrl was held: copy instead of move
  Rect move_src{};
  uint32_t move_r = 0, move_c = 0;
  int move_insert = 0;               // Shift was held: insert the cells there, shifting others down (1) or right (2)
  bool move_menu = false;            // a right-drag of the border: the drop menu (Move Here, Copy Here...)
  bool autofill_ctrl = false;        // Ctrl was held as the fill handle was let go (Excel: copy <-> series)
  bool fill_menu = false;            // a right-drag of the fill handle: the fill menu (Copy Cells, Fill Series...)
  int fill_shift = 0;                // Shift-drag of the fill handle: insert (1) or delete (2) the dragged cells
  Rect fill_shift_rect{};
  bool fill_shift_down = true;       // rows (true) or columns are shifted
  // outline: a level button (1..8) or a +/- button was clicked
  int outline_level = 0;             // > 0: show this level
  bool outline_level_rows = true;
  bool outline_toggle = false;       // collapse / expand the group of outline_index at outline_toggle_level
  bool outline_rows = true;
  uint32_t outline_index = 0;
  int outline_toggle_level = 0;
  bool outline_collapse = false;
  int resize_kind = 0;               // 1 column width, 2 row height
  uint32_t resize_index = 0;
  int32_t resize_px = 0;             // pixels at 100% zoom
  bool autofit = false;              // double-click on a header boundary
  bool resize_hidden = false;        // the boundary of hidden columns / rows: the drag or double-click unhides one (Excel)
  bool context_cells = false, context_col_header = false, context_row_header = false;
  bool double_click_cell = false;
  float click_x = -1.0f;  // the double-click's screen x (the caret goes there)
  bool selection_changed = false;
  bool start_edit_f2 = false;
  bool start_edit_typing = false;    // typing in Ready mode: start editing with the typed characters (replacing the contents)
  std::string start_edit_text;
  // Enter mode: a navigation key with Ctrl or Shift, or Home / End / Page Up / Page Down, ended the entry; once it is
  // written the key acts on the sheet as it does there (GridView::nav_replay), as Excel commits and then moves
  ImGuiKey nav_key = ImGuiKey_None;
  bool nav_ctrl = false, nav_shift = false;
  int point_move = 0;         // keyboard pointing: 1 down, 2 up, 3 right, 4 left, 5 PgDn, 6 PgUp, 7 Ctrl+Home,
                              // 8 Ctrl+Space (whole columns), 9 Shift+Space (whole rows), 10 Home, 11 Ctrl+End
  bool point_extend = false;  // Shift: extend the reference
  bool point_ctrl = false;    // Ctrl with an arrow: jump to the edge of the data, as the active cell does
  // F9 while editing a formula: replace the selected part (or the whole formula) with its value
  bool fn_args = false;  // Ctrl+A after a function's name: its Function Arguments (Excel)
  bool eval_part = false;
  int eval_begin = 0, eval_end = 0;
  // cross-sheet references in point mode: the caller needs to know the reference's sheet
};

struct GridStats {
  int64_t cells_processed = 0;
  int64_t texts_drawn = 0;
  int64_t merges_drawn = 0;
  int64_t formats = 0;
};

struct GridResult {
  GridActions act;
  GridStats stats;
};

// main function: snap is the snapshot for this frame; edit is the editing state shared by the whole application
GridResult DrawGrid(const engine::Snapshot& snap, const model::Sheet& sheet, GridView& view, EditState& edit, const ImRect& rect,
                    const GridOptions& opt, TabBarIO* tabs);

// edit field (the in-cell box and the formula bar share one EditState); returns whether the text changed
bool EditField(EditState& e, const char* id, const ImVec2& size, GridActions& act, bool in_formula_bar, bool word_wrap = false);
// set the edit text (when point mode inserts a reference, when F4 toggles absolute references)
void SetEditText(EditState& e, std::string text, int caret);
// whether a reference can be inserted at the caret (an operator, opening parenthesis, comma or = before it)
bool CanInsertRef(const EditState& e);

// extra formula autocomplete items (defined names, Python UDFs); set by the caller when the workbook or host changes
struct Completion {
  std::string name;
  std::string detail;  // names: refers to; UDFs: description
  int kind = 1;        // 1 defined name, 2 Python UDF, 3 table (ListObject)
  std::vector<std::string> columns;  // a table's column names (listed after "Table1[")
};
void SetFormulaCompletions(std::vector<Completion> items);

// make the active cell visible (also used by Python's ScrollIntoView and Goto)
void EnsureVisible(const model::Sheet& sheet, GridView& view, uint32_t r, uint32_t c);
void SelectCell(GridView& view, uint32_t r, uint32_t c, bool extend = false);
// where Tab / Shift+Tab from one active cell go by the sheet's own rules (Excel), on the sheet and after an entry
// alike: a protected sheet's next unlocked cell (unlocked set; selectable when locked cells can't be selected), in
// reading order and wrapping, or a table's next row from its last column (the previous row's last from its first).
// From a table's last cell *add_row is the table's id: it gets a row, the active cell going to that row's first.
// False when a plain move one cell over applies.
bool TabTarget(const model::Sheet& sheet, uint32_t r, uint32_t c, bool back, const std::function<bool(uint32_t, uint32_t)>& unlocked,
               const std::function<bool(uint32_t, uint32_t)>& selectable, uint32_t* nr, uint32_t* nc, int64_t* add_row);
void SelectRange(GridView& view, const Rect& r);
// a selection of whole columns (rows, the whole sheet) however it was made (Name Box, Go To, Python) is shown and
// handled as one (Excel: C:C typed in the Name Box, then Ctrl+- deletes the column)
void NormalizeWholeSelection(GridView& view, const model::Sheet& sheet);

// cell fill (solid, pattern or gradient) at a scale (pixels per point); also used by dialogs to preview fills
void DrawCellFill(ImDrawList* dl, const ImRect& r, const model::Style& st, float scale, ImU32 background);
// an icon of a conditional formatting icon set (model::IconSetStyle, icon 0 = the lowest), as cells show it: the
// Rules Manager and the Icon Sets menu draw them too
void DrawRuleIcon(ImDrawList* dl, const ImRect& b, int style, int index);

// number format cache: numfmt string id -> compiled format (UI thread)
std::string FormatCellText(const engine::Snapshot& snap, const model::Style& st, const core::Value& v, double max_chars, bool* numeric,
                           uint32_t* color, bool* hashes);

}  // namespace yc::render
