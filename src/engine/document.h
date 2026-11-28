// SPDX-License-Identifier: Apache-2.0
// document.h - the engine's working state: workbook + formula registry + dependency index + recalculation + undo (PLAN §4-§7, §9)
//
// Used on the engine thread only (single writer). The UI and Python read immutable snapshots published by Engine.
//
// Recalculation (PLAN §7.3, §7.4, deferral):
//   1. changed cells -> DepIndex finds dependent formulas -> marked pending transitively.
//   2. parallel waves (many pending cells): every pending formula is evaluated at once on the worker threads, reading the
//      model only; a formula that reads a still-pending cell gives up for this wave. The finished results are written back
//      by the engine thread (single writer), and the next wave starts. Formulas with side effects (Python UDFs, streams)
//      never run in a wave; waves stop when they make little progress (long chains, circular references).
//   3. serial pass for whatever is left: cells are evaluated in (sheet, row, col) order; reading another pending cell throws
//      NeedCell, which is computed first and then retried (explicit stack, no recursion). Dynamic references
//      (OFFSET / INDIRECT) use the same path.
//   4. reading a cell already on the evaluation stack = circular reference: the stack isn't calculated (AbortStack), nor
//      what reads it. Without iteration the cells keep their values (a formula just entered shows 0, as Excel's) and
//      the cycle is reported; with iteration ResolveCycles calculates them in rounds, at every recalculation.
//
// Dynamic arrays: a formula whose result has several cells spills from its cell (the anchor) to the right and down.
// Member cells get plain values; Sheet::spills remembers each anchor's range. A non-empty cell in the way gives
// #SPILL!. Reading a member of a pending anchor waits for the anchor (like a pending cell), and when a range grows,
// shrinks or changes, the dependents of its cells are marked pending and evaluated in another pass.
//
// Undo (ADR 0001): every transaction saves a COW copy of the whole Workbook (only pointers are copied);
// undo = swap the old version back and rebuild the dependency registrations of the recorded regions. Formula and style tables only grow, so the current (superset) ones are kept.
#pragma once

#include "core/geometry.h"
#include "core/value.h"
#include "engine/analysis.h"
#include "engine/bind.h"
#include "engine/dep_index.h"
#include "engine/format_match.h"
#include "formula/ast.h"
#include "formula/eval.h"
#include "formula/parser.h"
#include "model/workbook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <cstdint>
#include <functional>
#include <set>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yc::engine {

using core::Rect;
using core::Value;

// kSrcRemote: a change from another copy of the workbook (co-authoring was removed, ADR 0012); kSrcRestore: undo / redo of a step whose structural changes were
// recorded (co-authoring sends those as operations instead of the restored cells); kSrcFile: a cell read from a file,
// kept as the file saved it
enum ChangeSource : uint8_t { kSrcUser = 1, kSrcPython = 2, kSrcCalc = 4, kSrcLive = 8, kSrcRemote = 16, kSrcRestore = 32, kSrcFile = 64, kSrcNoEvents = 0x80 };

// What a write does besides storing the cell, decided by where it comes from. Every rule that depends on the source
// reads this one table: a new source picks its row here instead of borrowing another source's bits and inheriting the
// rules meant for it (the .xlsx reader wrote as Python, and its formulas took their references' formats on open).
struct WritePolicy {
  bool input = false;         // new input, not a calculation's own result: spills under it give way, table headers
                              // are read again, iterative calculation goes on with the cycles
  bool table_rules = false;   // a value next to a table grows it; a formula in an empty table column fills it
  bool infer_format = false;  // a formula in a General cell shows in its references' format (typing and code, as Excel)
  bool foreign = false;       // outside Undo: written outside a step, it drops the user's undo history (ADR 0013)
  const char* name = "calc";  // as the audit log and change events name it
};
constexpr WritePolicy PolicyFor(uint8_t src) {
  WritePolicy p;
  const bool calc = (src & kSrcCalc) != 0, file = (src & kSrcFile) != 0;
  const uint8_t base = static_cast<uint8_t>(src & ~kSrcNoEvents);
  p.input = !calc;
  p.table_rules = !calc && !file && (src & (kSrcUser | kSrcPython)) != 0;
  p.infer_format = base == kSrcUser || base == kSrcPython;
  p.foreign = (src & (kSrcPython | kSrcLive | kSrcRemote | kSrcFile)) != 0;
  p.name = (src & kSrcUser) ? "user" : (src & kSrcPython) ? "python" : (src & kSrcLive) ? "live" : (src & kSrcRemote) ? "remote"
           : file ? "file" : "calc";
  return p;
}

// a structural change, recorded with each undo step (struct_log): sheets are named, not numbered
struct StructOp {
  enum Kind : uint8_t { InsertRows, DeleteRows, InsertCols, DeleteCols, InsertCells, DeleteCells, AddSheet, DeleteSheet, RenameSheet, MoveSheet, CopySheet };
  Kind kind = InsertRows;
  std::string sheet;    // the sheet (RenameSheet: its old name; CopySheet: the source)
  uint32_t at = 0, count = 0;  // rows / columns; MoveSheet: the old index
  Rect rect{};          // InsertCells / DeleteCells
  bool shift = false;   // InsertCells: down (else right); DeleteCells: up (else left)
  std::string name;     // AddSheet: (unused); RenameSheet: the new name; CopySheet: the copy's name
  int32_t index = -1;   // AddSheet / DeleteSheet / MoveSheet (new) / CopySheet: tab position
  // local, not sent: the sheet's id and how many changes were reported before it (cells changed earlier in the same
  // batch moved with it)
  uint32_t sheet_id = 0;
  size_t changes_at = 0;
  bool operator==(const StructOp&) const = default;
};
StructOp InverseOf(const StructOp& op);

struct CellChange {
  uint32_t sheet;  // sheet id
  Rect rect;
  uint8_t source;
};

struct CalcStats {
  uint32_t evaluated = 0;
  uint32_t retries = 0;
  uint32_t self_reads = 0;  // formulas found reading their own cell without a thrown NeedCell (CalcHost::soft_self)
  uint32_t circular = 0;
  uint32_t parallel = 0;  // evaluated in parallel waves
  uint32_t iterations = 0;  // iterative calculation rounds
  uint32_t waves = 0;
  double wave_eval_ms = 0;    // parallel evaluation time of the waves
  double wave_commit_ms = 0;  // writing the wave results back (engine thread)
  double ms = 0;
};

// provider of external evaluation (Python UDFs, streams); Engine connects it to the YCP host
class ExternalProvider {
 public:
  virtual ~ExternalProvider() = default;
  // asynchronous UDF request; the result comes back through Document::SetUdfResult
  // caller: the cell that asks (Application.Caller / ThisCell in the function); its key is part of `key`, so each cell
  // gets its own call, as Excel calls a worksheet function for each cell
  virtual void RequestUdf(const std::string& key, const std::string& name, const std::vector<std::shared_ptr<formula::Array>>& args,
                          uint32_t sheet, uint32_t row, uint32_t col) = 0;
  // subscribe to a stream topic (the first time a formula uses it)
  virtual void Subscribe(const std::string& topic, const std::vector<Value>& args) = 0;
};

// downloads for WEBSERVICE (implemented by the app; the result comes back through Document::SetUdfResult with the key)
class NetProvider {
 public:
  virtual ~NetProvider() = default;
  virtual void Fetch(const std::string& key, const std::string& url) = 0;
};

// clipboard contents (engine-internal format; serialized to TSV / HTML separately across processes)
struct ClipCell {
  Value value;
  std::shared_ptr<const model::FormulaDef> formula;  // relative reference semantics (R1C1)
  model::Style style;
  bool has_style = false;
};
struct PivotCache;  // pivot_engine.h
struct ClipData;
// the copy area pasted (or moved) with its upper-left cell at (r, c) would run past the sheet's last row or column:
// Excel refuses the paste ("We can't paste here because the Copy area and paste area aren't the same size", measured
// 2026-11-16 through Range.Copy Destination), where YCGrid pasted the used part and selected cells past XFD
bool PasteRunsOffSheet(const ClipData& clip, uint32_t r, uint32_t c, bool transpose = false);
bool AreaRunsOffSheet(const Rect& area, uint32_t r, uint32_t c, bool transpose = false);
// the copy area of a selection: whole rows / columns (to the sheet's row_limit) run to Excel's last row / column
Rect CopyAreaOf(const model::Sheet& s, const Rect& rect);

struct ClipData {
  uint32_t rows = 0, cols = 0;
  uint32_t src_sheet = 0;
  Rect src;
  // the copy area as selected, whole rows / columns to the sheet's last row / column (src is cut down to the used range):
  // what a paste must fit on the sheet (PasteRunsOffSheet); empty for a clip not copied from cells (rows x cols then)
  Rect area;
  std::vector<ClipCell> cells;  // row-major
  std::vector<Rect> merges;     // relative to the upper-left corner
  std::vector<int32_t> col_widths;
  // whole rows (columns) copied: each one's height (width) in px, -1 hidden, from the copy area's first one; a paste
  // with All of the clipboard's copy gives them to the rows (columns) it pastes onto (Excel 16, measured 2026-11-28)
  std::vector<int32_t> line_heights, line_widths;
  std::vector<std::pair<size_t, model::RichText>> rich;  // index into cells -> its fonts inside the text
  // index into cells -> its hyperlink: pasted with All and by fills, which take the destination's links away (Excel 16,
  // measured 2026-11-28: Copy Destination, FillDown, AutoFill and a cut bring them, a plain or empty cell removes one)
  std::vector<std::pair<size_t, model::Hyperlink>> links;
  // the conditional formatting of the copied cells: each rule clipped to the copy, its ranges relative to the upper-left
  // corner, its formulas relative to base (the rule's first cell on the source sheet). Pasted with All / Formats /
  // All except borders, as Excel's paste and Format Painter do.
  struct Rule {
    model::CfRule rule;
    core::CellPos base;
  };
  std::vector<Rule> rules;
  // notes and comment threads (key: row << 16 | col, relative to the upper-left corner) and the data validation rules
  // (ranges relative to the corner, formulas relative to base), pasted with All and Paste Special's own choices
  std::vector<std::pair<uint64_t, model::CellNote>> notes;
  struct Validation {
    model::DvRule rule;
    core::CellPos base;
  };
  std::vector<Validation> validations;
  bool carry_notes = false;  // the clipboard's copy: All pastes the notes and validation too (fills and sorts don't)
  bool cut = false;
  // the parts of arrays entered with Ctrl+Shift+Enter in the copy (relative to the upper-left corner) with their
  // array's formula: pasted with the formulas, each part is an array of its own, its formula moved from the array's
  // first cell to the part's (Excel 16, measured: A9:B9 of {=A1:B3*2} in A8:B10 pasted at D12 is {=D5:E7*2})
  struct ArrayPart {
    Rect rect;
    std::shared_ptr<const model::FormulaDef> formula;
  };
  std::vector<ArrayPart> arrays;
};

// FormulasAndFormats: Paste Special > Formulas and number formats (the formulas and the number formats, not fills or borders)
// Comments: Paste Special > Comments and Notes; Validation: only the data validation; AllSourceTheme: All with the
// copied theme colors kept as the colors they showed; AllMergingCf: All, the copied conditional formats added to the
// destination's instead of replacing them
enum class PasteMode : uint8_t { All = 0, Values, Formulas, Formats, ValuesAndFormats, NoBorders, ColumnWidths, FormulasAndFormats, Comments, Validation, AllSourceTheme, AllMergingCf };
// Paste Special > Operation: the copied numbers are added to (subtracted from...) the cells pasted onto
enum class PasteOp : uint8_t { None = 0, Add, Subtract, Multiply, Divide };

enum class BorderPreset : uint8_t {
  None = 0, All, Outside, ThickOutside, Inside, InsideH, InsideV, Top, Bottom, Left, Right, DoubleBottom, ThickBottom, TopBottom, TopThickBottom, TopDoubleBottom, DiagDown, DiagUp,
};

struct SortKey {
  uint32_t col;  // absolute column
  bool descending = false;
  int custom_list = -1;  // order by a custom list (index into AllFillLists); values not in it come after
  std::vector<std::string> custom_items;  // or by these items (CustomOrder:="High,Medium,Low")
  // Sort On (Excel's SortOn): 0 cell values, 1 cell color, 2 font color; the rows with color go on top (descending:
  // on bottom), the others keep their order. color is ARGB (no alpha = no fill / automatic)
  int sort_on = 0;
  uint32_t color = 0;
  // Sort On Conditional Formatting Icon (3): the rows showing this icon (icon set, 0-based icon) on top / on bottom
  int icon_set = 0, icon = -1;
};

class CalcHost;

// Data -> Text to Columns
struct TextToColumnsOptions {
  bool fixed = false;               // fixed width (breaks) instead of delimiters
  std::vector<uint32_t> breaks;     // fixed width: break positions in characters
  bool tab = true, semicolon = false, comma = false, space = false;
  std::string other;                // one more delimiter character
  bool consecutive = false;         // treat consecutive delimiters as one
  char qualifier = '"';             // text qualifier (0 = none)
  std::vector<int> types;           // per column: 1 General (default), 2 Text, 3..8 Date (MDY, DMY, YMD, MYD, DYM, YDM), 9 skip
  // Advanced: how General columns read numbers (0 = the usual '.' and ','); "123-" is -123 when trailing_minus
  char decimal = 0, thousands = 0;
  bool trailing_minus = true;
};
// a Text to Columns field read as a date in the order of `type` (3 MDY ... 8 YDM): "yyyy-mm-dd", or "" when it isn't one
std::string TextToColumnsDate(const std::string& field, int type);
std::vector<std::string> SplitColumns(const std::string& text, const TextToColumnsOptions& o);

// one step of Evaluate Formula: the formula with the part evaluated next at [hl_begin, hl_begin + hl_len)
// rows / columns inserted or deleted (with a band: cells shifted), in order: what the UI holds by position (the Watch
// Window's cells) follows them. delta > 0 inserts before `at`; < 0 deletes [at, at - delta)
struct StructShift {
  uint64_t seq = 0;
  uint32_t sheet = 0;
  bool rows = true;
  uint32_t at = 0;
  int64_t delta = 0;
  bool has_band = false;
  core::Rect band{};
};

struct EvalStep {
  std::string text;
  size_t hl_begin = 0, hl_len = 0;  // hl_len 0 = the final result
  // the underlined part is a reference to one cell: Evaluate Formula's Step In shows that cell's formula (or value)
  int32_t in_sheet = -1;
  uint32_t in_r = 0, in_c = 0;
};

// helpers
inline uint64_t Key(uint32_t sheet, uint32_t r, uint32_t c) { return core::CellKey(sheet, r, c); }
inline uint32_t KeySheet(uint64_t k) { return static_cast<uint32_t>(k >> 48); }
inline uint32_t KeyRow(uint64_t k) { return static_cast<uint32_t>((k >> 16) & 0xFFFFFFFFu); }
inline uint32_t KeyCol(uint64_t k) { return static_cast<uint32_t>(k & 0xFFFF); }

// text shown in the formula bar (the number format decides how dates / percentages appear)
std::string InputTextOf(const Value& v, std::string_view numfmt);
std::string Lower(std::string_view s);  // ASCII lowercase
// Find and Replace's wildcards: * any run, ? one character, ~ escapes the next one (~*, ~?, ~~). Finds the leftmost
// (longest) match of pattern in text at or after from; whole: the match must be the whole text. Both already lowered
// for a match that ignores case.
bool WildFind(std::string_view pattern, std::string_view text, size_t from, bool whole, size_t* start, size_t* len);
// rows an AutoFilter (or Advanced Filter) hides inside the areas are left out: each area becomes its runs of shown
// rows (Excel's Copy, Delete, Fill Down and Ctrl+Enter act on the cells a filtered list shows)
std::vector<Rect> FilterShownRuns(const model::Sheet& s, const std::vector<Rect>& areas);
// does an area take part of a merged cell without all of it (Excel's object model refuses or skips such a write)
bool CutsMerge(const model::Sheet& s, const Rect& area);
// a number format with a time but no date part (h:mm, [h]:mm:ss): AutoFill steps it by hours
bool IsTimeOnlyFormat(std::string_view code);
// text that AutoFill continues as a series on its own (Item1, Q1, 1st, Mon, January), not copies
bool FillTextSeries(std::string_view text);
// displayed cell text (number format applied, no column-width adjustment)
std::string DisplayTextOf(const Value& v, std::string_view numfmt);
// filter and COUNTIF-style criteria: ">100", "<>T*", "=" (blank); text is the displayed text
bool MatchCriterion(const std::string& crit, const Value& v, const std::string& text);
// AutoFilter: tests data rows against a filter's criteria (Top 10 thresholds and averages are worked out once, when it
// is built). except_field skips one column's criterion: the filter menu lists the values the other columns leave.
class FilterTester {
 public:
  FilterTester(const model::Workbook& wb, const model::Sheet& s, const model::AutoFilter& af, const Rect& range);
  bool Passes(uint32_t row, int except_field = -1) const;

 private:
  const model::Workbook& wb_;
  const model::Sheet& s_;
  const model::AutoFilter& af_;
  Rect rg_;
  std::vector<double> thresholds_;
  // kind 1 (by value): each column's allowed texts, lowercased (Excel's list is case-insensitive)
  std::vector<std::unordered_set<std::string>> values_;
  // Filter by Cell Icon (kind 7): each data row's icon in that column (icon set << 8 | icon, -1 none), by column
  std::vector<std::vector<int>> icons_;
  double today_ = 0;
  int ty_ = 0, tm_ = 0, td_ = 0;
};
// a filter's range grown down over the rows typed right below it (Excel takes them into the list)
Rect AutoFilterExtent(const model::Sheet& s, const Rect& range);
// tables and AutoFilters: the sheet's AutoFilter is the table's (its header row and columns); the table has buttons of
// its own (Filter Button on, the sheet's filter elsewhere); the range its own filter works on (header + data rows)
bool TableOwnsSheetFilter(const model::Sheet& s, const model::Table& t);
bool TableHasOwnFilter(const model::Sheet& s, const model::Table& t);
Rect TableFilterRange(const model::Table& t);
bool FilterHidRow(const model::Sheet& s, uint32_t r);
// a table's slicer: the column's items (as shown, "(blank)" for empty cells), the ones its filter leaves out, and the ones
// that still have rows under the other columns' filters; false when the table or column is gone
// a PivotChart's series and categories from its PivotTable's report (false: the source can't be read; the chart keeps them)
bool PivotChartSeries(const model::Workbook& wb, const model::PivotTable& pt, model::ChartObj* ch);
bool TableSlicerState(const model::Workbook& wb, const model::Slicer& sl, uint32_t* sheet, std::vector<std::string>* items, std::vector<std::string>* hidden,
                      std::vector<char>* has_data);  // hidden by the sheet's AutoFilter (or Advanced Filter) or a table's
// whether a name is valid (Excel rules); on failure error holds the reason
bool ValidateName(std::string_view name, std::string* error);
std::vector<Rect> SubtractRect(const Rect& a, const Rect& b);

// a PivotTable's report area, or its placeholder (3 x 18 cells) while it has no fields
Rect PivotArea(const model::PivotTable& p);

// Insert -> Sparklines: data and location ranges -> the sparklines (false when the sizes don't match)
bool BuildSparklines(uint32_t data_sheet, const Rect& data, const Rect& loc, std::vector<model::Sparkline>* out);

// why an operation was refused (Python gets an E_PROTECTED error saying which)
// OffSheet: an insertion would push nonblank cells off the sheet (appended: the numbers are read by other threads)
// Array: a change would cut an array formula (Ctrl+Shift+Enter): "You can't change part of an array."
enum class BlockReason { None, ReadOnly, Pivot, DataTable, Parameter, Protection, Structure, OffSheet, MergeSize, TableShift, Array, TooLarge, TableMerge };

class Document : public formula::SheetResolver {
 public:
  Document();
  explicit Document(model::Workbook wb);  // loading: rebuild formula registrations and dependencies, full recalculation
  ~Document() override;
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  const model::Sheet* S(uint32_t id) const;  // sheet by id
  model::Sheet* M(uint32_t id);              // COW: get a writable version
  void Reindex();                            // call after the sheet list changes

  const model::Workbook& wb() const { return wb_; }
  model::Workbook& wb_mut() { return wb_; }

  // ---- SheetResolver ----
  int32_t FindSheet(std::string_view name) const override;
  std::string SheetName(int32_t id) const override;

  // ---- Options ----
  bool excel_15_digit_input = false;
  bool auto_calc = true;
  // Calculation Options > Automatic except for data tables (xlCalculationSemiautomatic): with auto_calc on, data tables
  // are calculated only by F9 (CalculateNow); false = that mode
  bool auto_data_tables = true;
  int calc_threads = 0;  // 0 = parallel waves when there is enough work; 1 = single-threaded (tests, comparison)
  // iterative calculation (File -> Options): circular references are recalculated until the largest change of a
  // round is below max_change, at most max_iterations rounds (Excel's defaults); off = report them and keep 0
  bool iterate = false;
  int max_iterations = 100;
  double max_change = 0.001;
  static constexpr size_t kParallelMin = 512;  // fewer pending formulas than this: the serial pass is faster
  ExternalProvider* provider = nullptr;
  // =PY with no Python host running (no provider yet): the app starts one; called on the engine thread, once until the
  // UDF cache is cleared (the host's start clears it and recalculates)
  static inline std::function<void()> on_python_needed;
  // the open workbook's Python code is disabled (Trust Center, trust.cpp): =PY formulas give #BLOCKED! instead of running
  // (Excel's error for Python in Excel blocked by security); calculated again once it is enabled (RecalcUdfCells)
  static inline std::atomic<bool> python_blocked{false};
  NetProvider* net = nullptr;
  uint8_t extra_src_bits = 0;  // added to the source of every change during this period (e.g. Python writes with EnableEvents = False)
  // co-authoring: structural changes (also their inverses on undo) are appended here while it is set
  std::vector<StructOp>* struct_log = nullptr;

  // ---- Cell contents ----
  // user input rules: a leading = is a formula; a leading ' forces text; numbers, dates and percentages are recognized and get a suggested format
  // r1c1: formulas are typed in R1C1 reference style (File -> Options)
  bool SetInput(uint32_t sheet, uint32_t r, uint32_t c, std::string_view text, std::string* error = nullptr, uint8_t src = kSrcUser, bool r1c1 = false);
  // the cell's number format is Text (@): what is typed there stays text, formulas too (Excel)
  bool IsTextFormatted(uint32_t sheet, uint32_t r, uint32_t c) const;
  void SetValue(uint32_t sheet, uint32_t r, uint32_t c, Value v, uint8_t src = kSrcUser);
  // bulk write: values are row-major, size = rect.Count()
  void SetValues(uint32_t sheet, const Rect& rect, const std::vector<Value>& values, uint8_t src = kSrcPython);
  // the number format a formula typed at (r, c) gives its result from its references or function (=A1+1 as A1, =TODAY()
  // as a date), whatever format the cell has; "" when it gives none
  std::string InferredFormat(uint32_t sheet, uint32_t r, uint32_t c, std::string_view text);
  bool SetFormula(uint32_t sheet, uint32_t r, uint32_t c, std::string_view text, std::string* error = nullptr, uint8_t src = kSrcUser,
                  bool r1c1 = false);  // text without the '='
  // Ctrl+Shift+Enter: one array formula for the whole rect (text without the '='); the result is fitted to the rect.
  // Fails when rect would cut through another array formula.
  bool SetArrayFormula(uint32_t sheet, const Rect& rect, std::string_view text, std::string* error = nullptr, uint8_t src = kSrcUser, bool r1c1 = false);
  void ClearContents(uint32_t sheet, const Rect& rect, uint8_t src = kSrcUser);
  // Excel's Clear Formats: styles, rich text, merges and (cond_formats) the conditional formatting of the cells
  // to_lines: the cells take their row's or column's formats again (what a cut leaves, cells emptied to be rewritten);
  // otherwise they are Normal, as the person's Clear Formats makes them, and whole lines lose their own formats
  void ClearFormats(uint32_t sheet, const Rect& rect, bool cond_formats = true, bool to_lines = false);
  // true (refused as too large) when Clear Formats over rect would give more than 20,000,000 cells Normal one by one
  bool RefuseClearFormatsSize(uint32_t sheet, const Rect& rect);
  // contents, formats and validation; everything (Home > Clear > Clear All, Range.Clear): also the conditional
  // formatting, notes and hyperlinks, as Excel's
  void ClearAll(uint32_t sheet, const Rect& rect, uint8_t src = kSrcUser, bool everything = false, bool to_lines = false);

  const Value& GetValue(uint32_t sheet, uint32_t r, uint32_t c) const;
  // evaluate a formula (without writing a cell): Application.Evaluate, WorksheetFunction
  // array: set to the whole result when it is an array or a range of more than one cell (its top-left is returned)
  Value EvaluateFormula(uint32_t sheet, std::string_view text, std::string* error, std::shared_ptr<formula::Array>* array = nullptr);
  // a parsed formula worked out as if it stood in (sheet, at): its relative references count from there (the
  // computed criteria of Advanced Filter, one record at a time); a cell not calculated yet gives #CALC!
  Value EvaluateAt(uint32_t sheet, const formula::Node& root, core::CellPos at);
  std::string GetFormulaA1(uint32_t sheet, uint32_t r, uint32_t c) const;  // "=SUM(A1:A3)"; "" when there is no formula
  std::string GetInputText(uint32_t sheet, uint32_t r, uint32_t c) const;  // text shown in the formula bar

  // ---- Protection (document_protect.cpp) ----
  bool ProtectSheet(uint32_t sheet, const model::SheetProtection& p, std::string_view password);
  bool UnprotectSheet(uint32_t sheet, std::string_view password);  // false: wrong password
  bool ProtectWorkbook(std::string_view password, bool structure, bool windows);
  bool UnprotectWorkbook(std::string_view password);
  void SetAllowEditRanges(uint32_t sheet, std::vector<model::AllowEditRange> ranges);
  bool UnlockEditRange(uint32_t sheet, size_t index, std::string_view password);  // for this session
  // a cell of r is locked by the sheet's protection; *range_needing_password: an edit range whose password would open it
  bool IsLockedRange(uint32_t sheet, const Rect& r, int* range_needing_password = nullptr) const;
  bool LockedByLines(const model::Sheet& s, Rect r) const;  // a large range without allowed ranges: by its styles
  uint32_t protect_blocks() const { return protect_blocks_; }  // operations refused so far
  uint32_t block_sheet() const { return block_sheet_; }
  const Rect& block_cells() const { return block_cells_; }  // the cells of the last refused change (0:0 when none)
  int block_range() const { return block_range_; }  // the edit range the last refused edit was in (password), or -1
  bool block_pivot() const { return block_pivot_; }  // the last refused edit was inside a PivotTable report
  bool block_pivot_shift() const { return block_pivot_ && block_pivot_shift_; }  // ... an insertion or deletion through it
  bool block_table_merge() const { return block_table_merge_; }  // the last refused merge was over a table
  bool block_too_large() const { return block_too_large_; }  // the last refused fill would write past 20,000,000 cells
  bool block_off_sheet() const { return block_off_sheet_; }  // the last refused insertion would push cells off the sheet
  bool block_merge_size() const { return block_merge_size_; }  // the last refused sort met merged cells of other sizes
  bool block_table_shift() const { return block_table_shift_; }  // the last refused deletion took a table's header row
  bool block_filter_shift() const { return block_table_shift_ && block_filter_shift_; }  // ... cells of a filtered list
  bool block_array() const { return block_array_; }  // the last refused change would cut an array formula
  bool block_read_only() const { return block_read_only_; }  // the last refused edit came while the UI is read-only
  // [YC] read-only mode (operator mode off): every change from the UI is refused; Python, live data and formulas write
  bool ui_read_only = false;
  // Protected View (the app's Trust Center): the workbook came from the Internet; nothing changes it, Python neither,
  // until Enable Editing
  bool protected_view = false;
  bool block_python() const { return block_python_; }  // the last refused operation came from Python
  bool python_call = false;  // the current command comes from Python (UserInterfaceOnly lets it through)
  // the app itself brings derived state up to date (a linked picture's image of its cells), as a recalculation
  // does: read-only mode lets it through
  bool app_refresh = false;
  std::atomic<uint32_t>* block_counter = nullptr;  // also counted here (read by other threads)
  // Application.CalculationInterruptKey / CheckAbort: set by another thread to stop the calculation in progress at
  // the next cell; what isn't calculated stays pending (CalculationState xlPending) and the next calculation goes on
  // from there. Recalc clears it when it returns.
  std::atomic<bool>* interrupt = nullptr;
  // tests (doc_fuzz --gaps): the calculation stops as if interrupted at the n-th check between cells from now (-1: never)
  int64_t interrupt_after = -1;
  std::string user_name;  // Application.UserName, for what records who did it (a PivotTable's RefreshName)
  // the @udf functions (upper case) that read their calling cell (Application.Caller / ThisCell): called per cell
  std::unordered_set<std::string> per_cell_udfs;
  bool CalcInterrupted() const { return interrupted_; }  // the last Recalc stopped early
  std::atomic<int>* block_reason = nullptr;        // why the last operation was refused (a BlockReason, read by other threads)
  // undo entries dropped (the redo list a new action ends, the oldest past the limit) are often the last owners of
  // millions of cells, which take a second to free: an engine hands them to a thread of its own here, so the action
  // (and a UI waiting on it) doesn't wait; without it they are freed in place
  std::function<void(std::shared_ptr<void>)> release_later;

  // ---- Tables (document_table.cpp) ----
  // creates a table (ListObject) over rect; without headers a header row is inserted above. Returns its id (0 = failed)
  uint32_t AddTable(uint32_t sheet, const Rect& rect, bool has_headers, const std::string& style, std::string* error = nullptr);
  bool ResizeTable(uint32_t sheet, uint32_t id, const Rect& rect, std::string* error = nullptr);
  bool RenameTable(uint32_t sheet, uint32_t id, const std::string& name, std::string* error = nullptr);  // formulas follow
  // Convert to Range (keep_look: the table's look stays as cell formats); formulas get ordinary references
  bool DeleteTable(uint32_t sheet, uint32_t id, bool keep_look);
  // a person's Clear Contents or Clear All over a whole table (its header row too) deletes the table, as Excel 16 does
  // (measured): what named it is #REF!. Called by the UI's Delete / Clear and Python's Range.Clear / ClearContents
  // before they clear, not by ClearContents itself (a cut table moved elsewhere stays a table)
  void ClearTablesIn(uint32_t sheet, const Rect& rect);
  // options (style, banding, header / totals row...); turning the totals row on or off adds or removes that row
  bool UpdateTable(uint32_t sheet, uint32_t id, const std::function<void(model::Table&)>& fn);
  bool SetTableTotal(uint32_t sheet, uint32_t id, uint32_t column, int func, const std::string& label);  // func: SUBTOTAL number
  bool TableInsertRow(uint32_t sheet, uint32_t id, int64_t pos);       // pos: 0-based data row; < 0 = at the end
  bool TableDeleteRow(uint32_t sheet, uint32_t id, uint32_t index);    // index: 0-based data row
  bool TableInsertColumn(uint32_t sheet, uint32_t id, int64_t pos);    // < 0 = at the end
  bool TableDeleteColumn(uint32_t sheet, uint32_t id, uint32_t index);
  const model::Table* TableAt(uint32_t sheet, uint32_t r, uint32_t c) const;
  const model::Table* TableById(uint32_t sheet, uint32_t id) const;
  std::string NextTableName() const;
  int ResolveStructRef(uint32_t sheet, const formula::Node& n, core::CellPos origin, uint32_t* out_sheet, Rect* out) const;

  // ---- Dynamic arrays ----
  // the spill range or array formula covering (r,c): its anchor and range; false when none
  bool SpillAt(uint32_t sheet, uint32_t r, uint32_t c, uint32_t* anchor_r, uint32_t* anchor_c, Rect* rect) const;

  // ---- Formatting ----
  void ModifyStyle(uint32_t sheet, const Rect& rect, const std::function<void(model::Style&)>& fn);
  // Page Layout > Themes: colors that refer to the theme change everywhere; new theme fonts change the default font
  void SetTheme(const model::Theme& t);
  // Page Layout > Effects: the theme's effect scheme (msgpack of its name, the store's view.theme_effects), one Undo
  // step as a theme's colors and fonts
  void SetThemeEffects(const std::string& value);

  // named cell styles (Home > Cell Styles, Workbook.Styles): a custom style, else the built-in one (names are case-insensitive)
  std::optional<model::NamedStyle> CellStyleDef(std::string_view name) const;
  bool ApplyCellStyle(uint32_t sheet, const Rect& rect, std::string_view name);
  // add or change a style (a changed built-in style is stored as well); cells with it take the new formats
  void SaveCellStyle(model::NamedStyle ns);
  // Data > Edit Links: new values read from a source (values: one map per linked sheet, row << 16 | column -> value);
  // error != "" keeps the old values; the formulas that read them recalculate
  // read_from: the link's source when it was read (a source changed meanwhile ignores the stale values); path: where it was found
  void SetExternalValues(size_t link, std::vector<std::map<uint64_t, Value>> values, double updated, const std::string& error,
                         const std::string& path = {}, const std::string* read_from = nullptr);
  // Break Link: the formulas that read the source become their values
  void BreakLink(size_t link);
  void ChangeLinkSource(size_t link, const std::string& path);
  // delete a custom style (a changed built-in style goes back to its original); its cells get the Normal formats
  bool DeleteCellStyle(std::string_view name);
  // every column, every row but the few (at most kFewRowsLeft) outside rect: through the columns' styles
  void ModifyAllButRows(uint32_t sheet, const Rect& rect, const std::function<void(model::Style&)>& fn);
  static constexpr uint32_t kFewRowsLeft = 16;
  void ModifyRowStyle(uint32_t sheet, uint32_t r0, uint32_t r1, const std::function<void(model::Style&)>& fn);
  void ModifyColStyle(uint32_t sheet, uint32_t c0, uint32_t c1, const std::function<void(model::Style&)>& fn);
  void SetBorders(uint32_t sheet, const Rect& rect, BorderPreset preset, model::LineStyle line = model::LineStyle::Thin, uint32_t color = 0);
  // property: Range.MergeCells = True (Excel 16, measured): the first value still goes to the upper-left cell, which
  // keeps its own formats; the other cells go back to Normal (Merge gives them all the value cell's formats)
  bool Merge(uint32_t sheet, const Rect& rect, bool across = false, bool center = false, bool property = false);
  // a merged area as a file saved it (importers): the cells keep their values and formats, unlike Merge
  void AddMergedArea(uint32_t sheet, const Rect& rect);
  void Unmerge(uint32_t sheet, const Rect& rect);
  void SetRowHeight(uint32_t sheet, uint32_t r0, uint32_t r1, int32_t px);
  void SetColWidth(uint32_t sheet, uint32_t c0, uint32_t c1, int32_t px);
  // a change of widths (heights) the sheet's protection refuses (counted as a refusal, as SetColWidth's)
  bool RefuseFormatDims(uint32_t sheet, bool cols) { return Refuse(sheet, nullptr, cols ? Allow::FormatColumns : Allow::FormatRows); }
  // Excel's Standard Width (Home > Format > Default Width, Worksheet.StandardWidth): every column not sized by hand
  void SetStandardWidth(uint32_t sheet, int32_t px);
  void HideRows(uint32_t sheet, uint32_t r0, uint32_t r1, bool hidden);
  void HideCols(uint32_t sheet, uint32_t c0, uint32_t c1, bool hidden);
  // AutoFit column width: measure(text, style) returns the width in pixels (font measurement provided by the UI)
  void AutoFitCols(uint32_t sheet, uint32_t c0, uint32_t c1, const std::function<int32_t(const std::string&, const model::Style&)>& measure);

  // ---- Structure ----
  // origin: the inserted cells take the formats of the row above / column to the left (0, Excel's
  // xlFormatFromLeftOrAbove), of the row below / column to the right (1), or stay plain (2); row heights / column
  // widths set by hand come along
  void InsertRows(uint32_t sheet, uint32_t at, uint32_t count, int origin = 0);
  void FillCalculatedColumns(uint32_t sheet, uint32_t at, uint32_t count);  // rows inserted inside a table's data
  void DeleteRows(uint32_t sheet, uint32_t at, uint32_t count);
  void InsertCols(uint32_t sheet, uint32_t at, uint32_t count, int origin = 0);
  void DeleteCols(uint32_t sheet, uint32_t at, uint32_t count);
  // insert / delete cells and shift (shift_down/shift_right or shift_up/shift_left)
  // InsertCells answers false when it refused (and changed nothing); RefuseInsertCells asks the same questions first,
  // for a command that inserts cells after other changes (a table's header row): sheet_filter_goes leaves out the rows
  // the sheet's AutoFilter hides, for a command that removes it before the insertion
  bool InsertCells(uint32_t sheet, const Rect& rect, bool shift_down, int origin = 0);
  bool RefuseInsertCells(uint32_t sheet, const Rect& rect, bool shift_down, bool sheet_filter_goes = false);
  // the questions DeleteCells and Insert / DeleteRows / Cols ask before changing anything (grown: the cells a deletion
  // inside a table takes): a command over several areas asks them for all of its areas first, as Excel refuses the
  // whole command (Delete Sheet Rows over areas deleted the first and refused the next, a change and a message)
  bool RefuseDeleteCells(uint32_t sheet, const Rect& rect, bool shift_up, Rect* grown = nullptr);
  bool RefuseRowsCols(uint32_t sheet, bool rows, bool insert, uint32_t at, uint32_t count);
  void DeleteCells(uint32_t sheet, const Rect& rect, bool shift_up);
  void EnsureRowLimit(uint32_t sheet, uint32_t row);  // grows when a write goes past the end (PLAN §5.1)
  void SetRowLimit(uint32_t sheet, uint32_t rows);

  uint32_t AddSheet(std::string name = {}, int32_t index = -1);  // returns the id
  bool DeleteSheet(uint32_t id);
  // reserved_ok: the name History is taken (a script's Worksheet.Name, as Excel's object model takes it)
  bool RenameSheet(uint32_t id, std::string_view name, std::string* error = nullptr, bool reserved_ok = false);
  bool MoveSheet(uint32_t id, int32_t new_index);
  uint32_t CopySheet(uint32_t id, int32_t new_index = -1);
  // false when refused: the last visible sheet can't be hidden. very: xlSheetVeryHidden (not offered by Unhide)
  bool SetSheetVisible(uint32_t id, bool visible, bool very = false);
  void SetTabColor(uint32_t id, uint32_t color);

  // ---- Controls and charts (PLAN §9, §12.9) ----
  uint32_t AddControl(uint32_t sheet, model::Control c);  // assigns an id and a name automatically (CommandButton1, ComboBox1...)
  bool UpdateControl(uint32_t sheet, uint32_t id, const std::function<void(model::Control&)>& fn);
  bool DeleteControl(uint32_t sheet, uint32_t id);
  uint32_t AddChart(uint32_t sheet, model::ChartObj c);   // named "Chart N"
  bool UpdateChart(uint32_t sheet, uint32_t id, const std::function<void(model::ChartObj&)>& fn);
  bool DeleteChart(uint32_t sheet, uint32_t id);
  // a chart sheet holding chart c (name "" = Chart1, Chart2...); returns the sheet id
  uint32_t AddChartSheet(std::string name, int32_t index, model::ChartObj c);
  // Move Chart: to a new chart sheet (to == 0, named new_name) or onto worksheet `to` at left / top (points).
  // A chart sheet left without its chart is deleted. Returns the destination sheet (0 = failed); *new_id: the chart's id there
  uint32_t MoveChart(uint32_t from, uint32_t id, uint32_t to, const std::string& new_name, double left, double top, uint32_t* new_id = nullptr);
  // ---- Pictures, shapes, text boxes and sparklines (document_shapes.cpp) ----
  uint32_t AddShape(uint32_t sheet, model::ShapeObj s);  // named "Picture 1", "Rectangle 2", "TextBox 3"...
  bool UpdateShape(uint32_t sheet, uint32_t id, const std::function<void(model::ShapeObj&)>& fn);
  bool DeleteShape(uint32_t sheet, uint32_t id);
  uint32_t AddSparklineGroup(uint32_t sheet, model::SparklineGroup g);  // the cells' old sparklines are replaced
  bool UpdateSparklineGroup(uint32_t sheet, uint32_t id, const std::function<void(model::SparklineGroup&)>& fn);
  // Sparkline > Group: the sparklines of the groups touching areas become one group with the look of the group at
  // (r, c) (else the first); returns its id (0: fewer than two groups). Ungroup: each of their sparklines on its own.
  uint32_t GroupSparklines(uint32_t sheet, const std::vector<Rect>& areas, uint32_t r, uint32_t c);
  void UngroupSparklines(uint32_t sheet, const std::vector<Rect>& areas);
  void ClearSparklines(uint32_t sheet, const Rect& area);
  const model::SparklineGroup* SparklineGroupAt(uint32_t sheet, uint32_t r, uint32_t c) const;

  // ---- PivotTables (document_pivot.cpp) ----
  // creates the PivotTable and writes its report (empty until fields are added); returns its id (0 = failed: err says why)
  uint32_t AddPivot(uint32_t sheet, model::PivotTable pt, std::string* err = nullptr);
  bool UpdatePivot(uint32_t sheet, uint32_t id, const std::function<void(model::PivotTable&)>& fn, std::string* err = nullptr);  // then refreshes
  bool RefreshPivot(uint32_t sheet, uint32_t id, std::string* err = nullptr);  // reads the source again and rewrites the report
  void RefreshAllPivots();
  // a report never covers a table, and covers data only when allowed: 1 = refuse (pivot_overwrite_hit is set, the
  // PivotTable stays as it was) so the UI can ask "There's already data in ... Do you want to replace it?"; 0 = replace
  int pivot_overwrite = 0;
  bool pivot_overwrite_hit = false;
  // delete a PivotTable: detach_charts turns its PivotCharts into static charts (their series stay)
  bool DeletePivot(uint32_t sheet, uint32_t id, bool clear, bool detach_charts = true);
  const model::PivotTable* PivotById(uint32_t sheet, uint32_t id) const;
  const model::PivotTable* PivotAt(uint32_t sheet, uint32_t r, uint32_t c) const;  // the report (or its placeholder) covering the cell
  const model::PivotTable* FindPivot(std::string_view name, uint32_t* sheet = nullptr) const;
  std::string NextPivotName() const;
  // ---- Slicers ----
  uint32_t AddSlicer(uint32_t sheet, model::Slicer sl);  // named after its field ("Region", "Region 1"...)
  bool UpdateSlicer(uint32_t sheet, uint32_t id, const std::function<void(model::Slicer&)>& fn);
  bool DeleteSlicer(uint32_t sheet, uint32_t id);
  // a slicer's selection: the hidden items of the field in its PivotTable (then the report refreshes)
  bool SetSlicerFilter(const std::string& pivot, int field, std::vector<std::string> hidden, std::string* err = nullptr);
  // a timeline's date range (whole days, inclusive); from > to clears it
  bool SetTimelineFilter(const std::string& pivot, int field, double from, double to, std::string* err = nullptr);
  // a slicer's selection (items hidden) for every PivotTable it is connected to (Report Connections), or its table's
  // column filter; a timeline's dates for every PivotTable it is connected to
  bool SetSlicerSelection(uint32_t sheet, uint32_t id, std::vector<std::string> hidden, std::string* err = nullptr);
  bool SetSlicerDates(uint32_t sheet, uint32_t id, double from, double to, std::string* err = nullptr);
  // Report Connections: the PivotTables (by name, each having a field of the slicer's field's name) the slicer filters
  bool ConnectSlicer(uint32_t sheet, uint32_t id, const std::vector<std::string>& pivots, std::string* err = nullptr);
  // PivotCharts: rebuild the series of every chart linked to a PivotTable (after refreshes, and when a file opens)
  void UpdatePivotCharts();
  // Show Details (double-click a value): the records behind it on a new sheet as a table; returns the sheet's id (0 = not a value)
  uint32_t PivotShowDetails(uint32_t sheet, uint32_t r, uint32_t c, std::string* err = nullptr);
  // GETPIVOTDATA: the PivotTable covering (r,c), its data field, and field / item pairs
  bool PivotLookupAt(uint32_t sheet, uint32_t r, uint32_t c, const std::string& data_field, const std::vector<std::pair<std::string, std::string>>& pairs,
                     Value* out) const;

  // ---- Page setup (document_shapes.cpp) ----
  bool UpdatePageSetup(uint32_t sheet, const std::function<void(model::PageSetup&)>& fn);
  // Page Layout > Background / Delete Background: the picture's bytes ("" = none)
  bool SetSheetBackground(uint32_t sheet, std::string image);

  // ---- Notes and hyperlinks ----
  // empty text deletes the note, unless keep_empty: a note emptied in its box, or made and left empty, stays (Excel 16,
  // measured by keys: Shift+F2, Backspace over all of it, Escape keeps the note)
  void SetNote(uint32_t sheet, uint32_t r, uint32_t c, const std::string& author, const std::string& text, bool keep_empty = false);
  // a note's box: its size, place and Format Comment look (false: no note there)
  bool SetNoteLook(uint32_t sheet, uint32_t r, uint32_t c, const model::CellNote::Look& look);
  // fonts inside a cell's text (runs that change nothing remove the entry); the cell keeps its value
  void SetRichText(uint32_t sheet, uint32_t r, uint32_t c, model::RichText rt);
  // the runs of a cell, while the cell still holds their text (nullptr otherwise)
  const model::RichText* RichTextAt(uint32_t sheet, uint32_t r, uint32_t c) const;
  void ClearNotes(uint32_t sheet, const Rect& area);
  // File -> Info -> Encrypt with Password (empty = no encryption); applies from the next save
  void SetOpenPassword(const std::string& password) {
    if (RefuseReadOnly()) return;
    wb_.open_password = password;
    wb_.dirty = true;
  }
  // threaded comments (a cell has either a note or a comment thread); date: seconds since 1970
  void SetComment(uint32_t sheet, uint32_t r, uint32_t c, const std::string& author, const std::string& text, double date);  // empty text = delete the thread
  bool AddCommentReply(uint32_t sheet, uint32_t r, uint32_t c, const std::string& author, const std::string& text, double date);
  bool EditCommentPost(uint32_t sheet, uint32_t r, uint32_t c, int reply, const std::string& text);  // reply -1 = the first post
  bool DeleteCommentReply(uint32_t sheet, uint32_t r, uint32_t c, int reply);
  bool ResolveComment(uint32_t sheet, uint32_t r, uint32_t c, bool resolved);
  int ConvertNotesToComments(uint32_t sheet, const Rect& area, double date);  // returns how many notes became threads
  // Review > Notes > Show/Hide Note and Show All Notes: the notes in area stay on the sheet (or go back to showing on
  // hover); threaded comments are left alone. Returns how many changed.
  int SetNotesVisible(uint32_t sheet, const Rect& area, bool visible);
  void SetHyperlink(uint32_t sheet, uint32_t r, uint32_t c, const model::Hyperlink& link);
  void ClearHyperlinks(uint32_t sheet, const Rect& area);

  // ---- Conditional formatting, data validation, AutoFilter ----
  // a new rule gets the highest priority (the UI), or the lowest when rule.priority < 0 (FormatConditions.Add, as Excel)
  // a protected sheet takes no new conditional format, locked cells or not (Excel 16, measured: FormatConditions.Add
  // raises); asked by what adds one for a person or a script, not by a paste that carries the copied cells' rules
  bool RefuseProtectedSheet(uint32_t sheet) { return Refuse(sheet, nullptr, Allow::Never); }
  uint32_t AddCondFormat(uint32_t sheet, model::CfRule rule);
  bool UpdateCondFormat(uint32_t sheet, uint32_t id, const std::function<void(model::CfRule&)>& fn);
  bool DeleteCondFormat(uint32_t sheet, uint32_t id);
  void ClearCondFormats(uint32_t sheet, const Rect& area);  // subtract area from the rules' ranges; rules that become empty are deleted
  // every rule of the sheet at once, in priority order (the Rules Manager's OK / Apply); id 0 gets a new id
  void SetCondFormats(uint32_t sheet, std::vector<model::CfRule> rules);
  void SetValidation(uint32_t sheet, const Rect& area, const model::DvRule& rule);  // replaces the old rules inside area
  void ClearValidation(uint32_t sheet, const Rect& area);
  bool SetAutoFilter(uint32_t sheet, const Rect& range);  // range includes the header row
  void ClearAutoFilter(uint32_t sheet);
  void SetFilterColumn(uint32_t sheet, const model::FilterColumn& col);  // kind 0 = clear the column's criteria; filters again afterwards
  void ShowAllData(uint32_t sheet);
  void ApplyAutoFilter(uint32_t sheet);
  void ApplyAutoFilter(uint32_t sheet, bool resort);  // Reapply: filters, then sorts by the recorded levels
  void SetFilterSort(uint32_t sheet, const std::vector<model::SortLevel>& levels);  // records the list's sort (AutoFilter.Sort)
  // a table's own AutoFilter (the sheet's is on another range): criteria, Clear, sort state, the Filter Button
  void SetTableFilterColumn(uint32_t sheet, uint32_t table_id, const model::FilterColumn& col);
  void ClearTableFilter(uint32_t sheet, uint32_t table_id);
  void SetTableFilterSort(uint32_t sheet, uint32_t table_id, const std::vector<model::SortLevel>& levels);
  void SetTableFilterButton(uint32_t sheet, uint32_t table_id, bool on);

  // custom panels (PLAN §10.3): by name
  void UpsertPanel(model::Panel p);
  bool UpdatePanel(const std::string& name, const std::function<void(model::Panel&)>& fn);
  bool DeletePanel(const std::string& name);

  bool AddName(const std::string& name, const std::string& refers_to, int32_t scope = -1, std::string* error = nullptr);
  // ---- Defined names (named ranges) ----
  bool DefineName(model::NameDef def, std::string* error = nullptr);  // adds or replaces by (name, scope)
  // modify a name: renaming also updates the formulas that use it (like Excel)
  bool UpdateName(const std::string& old_name, int32_t old_scope, model::NameDef def, std::string* error = nullptr);
  const model::NameDef* FindName(std::string_view name, uint32_t current_sheet) const;  // sheet-level names first
  // name -> range (supports dynamic names such as OFFSET, INDEX; also accepts "Sheet!Name")
  // anchor: the cell the name is read from (the active cell, the formula's cell); a relative name's references are
  // offsets from it (Excel's relative names)
  bool ResolveNameRef(std::string_view name, uint32_t current_sheet, uint32_t* sheet, Rect* rect, std::string* error = nullptr,
                      core::CellPos anchor = core::CellPos{});
  // evaluate a reference formula to a range (e.g. "OFFSET(A1,0,0,COUNTA(A:A),1)")
  bool EvaluateRef(uint32_t sheet, std::string_view text, uint32_t* out_sheet, Rect* out_rect, std::string* error = nullptr);
  // create names from the selection (text in the top row, left column, bottom row, right column becomes the names)
  std::vector<model::NameDef> NamesFromSelection(uint32_t sheet, const Rect& r, bool top, bool left, bool bottom, bool right) const;
  // Apply Names (Range.ApplyNames): the references of the area's formulas that are exactly one of the names become the
  // name; ignore_relative (Excel's "Ignore Relative/Absolute", on by default) also replaces relative references.
  // Returns the number of formulas changed
  struct ApplyNamesOptions {
    bool ignore_relative = true;  // Ignore Relative/Absolute
    bool use_row_column = true;   // Use row and column names (=Jan Sales where a row name crosses a column name)
    bool omit_column = true, omit_row = true;  // Options: omit the name of the formula's own column / row
    bool row_first = true;        // Name order: Row Column (xlRowThenColumn) or Column Row
  };
  int ApplyNames(uint32_t sheet, const Rect& area, const std::vector<std::string>& names, const ApplyNamesOptions& o);
  int ApplyNames(uint32_t sheet, const Rect& area, const std::vector<std::string>& names, bool ignore_relative = true) {
    ApplyNamesOptions o;
    o.ignore_relative = ignore_relative;
    o.use_row_column = false;
    return ApplyNames(sheet, area, names, o);
  }
  // formula auditing: precedents / dependents (like Range.Precedents, DirectPrecedents, Dependents, DirectDependents; same sheet only)
  std::vector<Rect> Precedents(uint32_t sheet, const Rect& range, bool direct);
  std::vector<Rect> Dependents(uint32_t sheet, const Rect& range, bool direct);
  // the cells on other sheets (or other workbooks) one cell's formula reads, and the formulas on other sheets that read
  // the cell: Trace Precedents / Dependents draw a dashed arrow to a sheet icon for them, as Excel does
  std::vector<std::pair<int32_t, Rect>> OtherSheetPrecedents(uint32_t sheet, uint32_t r, uint32_t c);
  std::vector<std::pair<int32_t, Rect>> OtherSheetDependents(uint32_t sheet, uint32_t r, uint32_t c);
  bool DeleteName(const std::string& name, int32_t scope = -1);

  // ---- Data tools (document_data.cpp) ----
  // removes the rows whose values in cols (absolute column indexes; empty = all) repeat an earlier row; returns how many
  uint32_t RemoveDuplicates(uint32_t sheet, const Rect& rect, const std::vector<uint32_t>& cols, bool header);
  // splits the text of the first column of src into columns starting at (dest_r, dest_c); returns the number of columns
  uint32_t TextToColumns(uint32_t sheet, const Rect& src, uint32_t dest_r, uint32_t dest_c, const TextToColumnsOptions& o);
  // outline: group (level + 1) or ungroup rows / columns a..b; show levels; collapse / expand the group around index
  void GroupOutline(uint32_t sheet, bool rows, uint32_t a, uint32_t b, bool group);
  // Clear Outline: the lines of [a, b] (all of them by default) leave every group and are shown (Excel 16, measured:
  // Range.ClearOutline and Data > Ungroup > Clear Outline both show collapsed rows; Ungroup keeps them hidden)
  void ClearOutline(uint32_t sheet, bool rows, uint32_t a = 0, uint32_t b = 0xFFFFFFFFu);
  // Data > Outline settings: summary rows below detail, summary columns right of detail, automatic styles
  void SetOutlineSettings(uint32_t sheet, bool summary_below, bool summary_right, bool automatic_styles);
  int AutoOutline(uint32_t sheet, const Rect& area);  // groups the rows / columns SUM and SUBTOTAL formulas add up; returns the groups made
  void ShowOutlineLevel(uint32_t sheet, bool rows, int level);  // hides everything deeper than level
  // hides (true) or shows the lines given, a run of consecutive lines with one choice at a time
  void HideLineRuns(uint32_t sheet, bool rows, const std::vector<std::pair<int64_t, bool>>& lines);
  bool OutlineBlock(uint32_t sheet, bool rows, uint32_t index, int level, uint32_t* a, uint32_t* b) const;
  void SetGroupCollapsed(uint32_t sheet, bool rows, uint32_t index, int level, bool collapse);
  // the summary line of the group over lines [a, b] (the outline settings' side); -1 off the sheet
  int64_t SummaryLine(uint32_t sheet, bool rows, int64_t a, int64_t b) const;
  // moves cells like Excel's drag and drop / cut and paste: formulas that point at the moved cells follow them
  bool MoveCells(uint32_t src_sheet, const Rect& src, uint32_t dst_sheet, uint32_t dst_r, uint32_t dst_c);
  // Evaluate Formula: every step of the formula in (r,c), innermost part first, the result last
  // step_in_constant: a cell without a formula gives one step, its value (Step In on a constant)
  std::vector<EvalStep> EvaluateSteps(uint32_t sheet, uint32_t r, uint32_t c, bool step_in_constant = false);

  // ---- Clipboard, fill, sort ----
  ClipData Copy(uint32_t sheet, const Rect& rect) const;
  // the cells Copy would hold for rect (cut down to the used range as it cuts it), asked before copying
  uint64_t CopyCellCount(uint32_t sheet, const Rect& rect) const;
  // a copy of the rows a filter shows (FilterShownRuns), one under the other: pasted as one block, as in Excel
  ClipData CopyShown(uint32_t sheet, const Rect& rect) const;
  // Fill Down over a filtered list: the first row it shows goes into the other rows it shows, the hidden ones keep
  // theirs (Excel 16: Ctrl+D and Range.FillDown, measured); false when the filter hides none of rect's rows
  // Fill Down (up: Fill Up) over a filtered list: the top (bottom) row it shows goes into the other rows it shows
  bool FillDownShown(uint32_t sheet, const Rect& rect, bool up = false);
  // Delete cells (shifting up or left) over a filtered list: the cells it shows, each run of rows bottom up (Excel 16's
  // object model, measured: C3:C4 under a filter hiding row 3 deletes C4 only)
  void DeleteCellsShown(uint32_t sheet, const Rect& rect, bool shift_up);
  // returns the range actually pasted
  Rect Paste(uint32_t sheet, uint32_t r, uint32_t c, const ClipData& clip, PasteMode mode = PasteMode::All, bool transpose = false, bool skip_blanks = false,
             PasteOp op = PasteOp::None);
  // the copied cells pasted tile_r x tile_c times from (r, c) (a destination that is a whole multiple of them): every
  // way a paste fills a selection goes through here. One copied cell over a filtered list goes into the rows the
  // filter shows only, a block of cells into every row (Excel 16, measured: Ctrl+V, Worksheet.Paste, Copy Destination)
  void PasteRepeated(uint32_t sheet, uint32_t r, uint32_t c, uint32_t tile_r, uint32_t tile_c, const ClipData& clip,
                     PasteMode mode = PasteMode::All, bool transpose = false, bool skip_blanks = false, PasteOp op = PasteOp::None);
  // whole_target: the first row / column counts as changed too, as Excel's Change event Target is the range filled
  // (Ctrl+D on one row copies the row above, which isn't selected: false)
  void FillDown(uint32_t sheet, const Rect& rect, bool whole_target = true);
  void FillRight(uint32_t sheet, const Rect& rect, bool whole_target = true);
  // Fill Down (dir 0), Right (1), Up (2), Left (3): the first row (column; the last going up / left) over the others.
  // A rect past the sheet's end stops at it; a large one (whole columns, the whole sheet) is filled a column (row) at
  // a time, never a paste per row, when the copied line has no merged cells
  void FillEdge(uint32_t sheet, const Rect& rect, int dir, bool whole_target = true);
  // Ctrl+Enter: the content of (r, c) into every cell of the areas, adjusted like a paste (relative references
  // move). Millions of cells: one formula shared where no reference leaves the sheet, one change per area.
  void FillFrom(uint32_t sheet, uint32_t r, uint32_t c, const std::vector<Rect>& areas);
  // AutoFill: src is the source, dest contains src and extends in one direction; numbers step linearly, dates, trailing numbers in text increment.
  // type is Excel's XlAutoFillType: 0 default, 1 copy, 2 series, 3 formats only, 4 values only (no formats), 5 days, 6 weekdays, 7 months,
  // 8 years, 9 linear trend, 10 growth trend
  void AutoFill(uint32_t sheet, const Rect& src, const Rect& dest, int type = 0);
  // a fill's data validation: each cell of target the rule of the source cell it repeats (document_rules.cpp)
  void FillValidations(uint32_t sheet, const Rect& src, const Rect& target, bool vertical, bool reverse);
  // the fill handle across: the formula at (r, c), filled from (src_r, src_c), names table columns delta further on
  void ShiftTableColumns(uint32_t sheet, uint32_t r, uint32_t c, uint32_t src_r, uint32_t src_c, int64_t delta);
  // the fill handle's tip while it is dragged (Excel): the text the last cell of dest gets, in its number format. The
  // first line of the source (its first column, or first row going sideways) is filled on a scratch document; "" when
  // that line holds a formula (Excel shows no value then) or the drag is too long to work out
  static std::string AutoFillTip(const model::Workbook& wb, const model::Sheet& s, const Rect& src, const Rect& dest, int type = 0);
  // Paste Options > Linked Picture: picture shape `shape` of `sheet` shows cells src of src_sheet (the app draws it
  // again when they change); an empty src (r0 > r1) unlinks it
  void SetLinkedPicture(uint32_t sheet, uint32_t shape, uint32_t src_sheet, const Rect& src);
  // the selected (grouped) sheets' tabs, saved with the workbook as Excel does; doesn't count as a change
  void SetTabsSelected(const std::vector<uint32_t>& ids);
  // Series (Home -> Fill -> Series): every column (by_cols) or row starts from its first cell
  // type: 0 linear, 1 growth, 2 date (unit: 0 day, 1 weekday, 2 month, 3 year); with has_stop it stops past stop
  void FillSeries(uint32_t sheet, const Rect& rect, bool by_cols, int type, int unit, double step, bool has_stop, double stop);
  // Series > Trend: each line's best-fit line (growth: exponential curve) through the values at its start, written over
  // the whole line (the starting values too, as in Excel); false when no line has two values to fit
  bool FillTrend(uint32_t sheet, const Rect& rect, bool by_cols, bool growth);
  // hidden rows stay; Options: match_case (lowercase before uppercase, as Excel), left_to_right (the columns are sorted
  // by the rows the keys name in SortKey::col)
  void Sort(uint32_t sheet, const Rect& rect, const std::vector<SortKey>& keys, bool has_header, bool visible_only = true, bool match_case = false,
            bool left_to_right = false);
  // find: returns the next match (by rows or by columns)
  // fmt: Find Format (the cell's format must have its fields; with an empty what, any cell with that format)
  bool Find(uint32_t sheet, const Rect& within, std::string_view what, uint32_t start_r, uint32_t start_c, bool by_rows, bool match_case, bool whole_cell,
            bool look_in_formulas, uint32_t* out_r, uint32_t* out_c, const FormatSpec* fmt = nullptr) const;
  // find all: every matching cell in search order (up to max)
  std::vector<core::CellPos> FindAll(uint32_t sheet, const Rect& within, std::string_view what, bool by_rows, bool match_case, bool whole_cell,
                                     bool look_in_formulas, size_t max = 5000, const FormatSpec* fmt = nullptr) const;
  // find_fmt: only cells with that format; repl_fmt: written into each replaced cell (with an empty what, every cell
  // of find_fmt gets it and keeps its contents)
  uint32_t Replace(uint32_t sheet, const Rect& within, std::string_view what, std::string_view with, bool match_case, bool whole_cell,
                   const FormatSpec* find_fmt = nullptr, const FormatSpec* repl_fmt = nullptr);

  // ---- Transactions and undo ----
  void Begin(std::string label);  // when nested only the outermost counts
  void Commit();
  bool Undo();
  bool Redo();
  bool CanUndo() const { return !undo_.empty(); }
  // where the next Undo (redo: Redo) changes cells: the sheet of its first change and the range around its changes
  // there (Excel shows and selects it); false when it changes no cells (sheets, names, themes...)
  bool UndoTarget(bool redo, uint32_t* sheet, Rect* rect) const;
  bool CanRedo() const { return !redo_.empty(); }
  size_t UndoDepth() const { return undo_.size(); }
  // every step an action has put on the undo list, ever: at the list's limit the depth stays when a step is added
  uint64_t StepsMade() const { return steps_made_; }
  size_t RedoDepth() const { return redo_.size(); }
  std::string UndoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
  std::string RedoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
  std::vector<std::string> UndoLabels(bool redo, size_t max) const {  // newest first
    const auto& v = redo ? redo_ : undo_;
    std::vector<std::string> out;
    for (auto it = v.rbegin(); it != v.rend() && out.size() < max; ++it) out.push_back(it->label);
    return out;
  }
  void ClearUndo();  // after Python changes the structure (rows, columns or sheets move): old positions no longer match
  // Python wrote cells, formats or objects (not undoable, ADR 0013): the user's changes stay undoable, except the ones
  // undone by putting a whole version back (they would undo Python's writes too)
  void ForeignWrite();
  size_t undo_limit = 100;

  // ---- Recalculation ----
  void Recalc();     // only pending + volatile
  // a write with Application.EnableEvents = False: its recalculation happens now and reports no Worksheet_Calculate, as
  // in Excel (which calculates during the write). Only when nothing else was waiting to be calculated.
  void RecalcWithoutEvents();
  void CalcEntered();  // manual calculation: just the formulas entered since the last call (Excel computes those)
  // before a request reads what the cells show (values, the formats rules give them): the formulas just written have
  // their values, as Excel's object model calculates after each write (manual calculation: the ones entered)
  void SettleForRead() {
    if (auto_calc) Recalc();
    else CalcEntered();
  }
  void RecalcAll();  // every formula (Ctrl+Alt+F9)
  // F9 (Calculate Now): what waits and the volatile formulas, and the data tables also when they are left out of
  // automatic calculation (Automatic except for data tables)
  void CalculateNow();
  // Shift+F9 (Calculate Sheet, Worksheet.Calculate): only the formulas of this sheet that wait, and its volatile ones;
  // other sheets' cells are read as they show (with manual calculation they keep waiting, as in Excel)
  void RecalcSheet(uint32_t sheet);
  // Range.Calculate: every formula of rect, from the values the other cells show (Excel); one that read a cell still
  // waiting outside rect waits too
  void RecalcRange(uint32_t sheet, const Rect& rect);
  // Range.Dirty: the formulas of rect wait to be calculated again (calculated at the next Recalc)
  void MarkDirty(uint32_t sheet, const Rect& rect);
  // the Python host is ready or registered its functions: every distinct call of a Python function (or WEBSERVICE)
  // is asked again by one of its formulas; the cells show their values until the answer comes, and the other
  // formulas of the call are calculated again only when it differs. Formulas calculated without a host are all
  // calculated again. (A full recalculation took a minute at every reload of a workbook with millions of them.)
  // host_udfs: the names the host registered (upper case); formulas calculated without a host that call none of
  // them keep their #NAME? (an unknown function stays unknown). nullptr: all of them are calculated again.
  void RecalcUdfCells(const std::vector<std::string>* host_udfs = nullptr);
  // the Python host stopped or started: the stream formulas (=STREAM, =QUOTE) subscribe again with the new host (they
  // kept the old host's subscription and stopped updating); the cells keep their values meanwhile
  void ProviderChanged();
  void MarkAllVolatileDirty();
  const CalcStats& last_stats() const { return stats_; }
  bool has_pending() const { return !pending_.empty(); }
  bool IsPendingCell(uint32_t sheet, uint32_t r, uint32_t c) const { return IsPending(Key(sheet, r, c)); }  // for tests
  // the cells of a rectangle that wait to be calculated (Format Stale Values draws the ones in view), at most `cap`
  void PendingIn(uint32_t sheet, const Rect& rect, size_t cap, std::vector<uint64_t>* out) const;
  // Worksheet.EnableCalculation (Excel 16, measured; not saved, as Excel's files don't keep it): a sheet whose
  // calculation is off calculates none of its formulas, whatever asks (an edit, F9, CalculateFull, Range.Calculate),
  // and a formula entered there shows 0; other sheets read its cells as they show. Turned on again, the whole sheet
  // is calculated.
  void SetSheetCalculation(uint32_t sheet, bool on);
  // Worksheet.EnableAutoFilter (0), EnableOutlining (1), EnablePivotTable (2): not saved, no Undo step (Excel's)
  void SetProtectionEnable(uint32_t sheet, int which, bool on);
  bool ProtectionEnable(uint32_t sheet, int which) const;
  // File > Options > Advanced > When calculating this workbook (saved with it; an Undo step each):
  // the 1904 date system (the cells keep their numbers and show other dates; every formula is calculated again),
  // Set precision as displayed (turned on, every number is rounded for good to what its format shows, and so is every
  // number written or calculated after, Excel 16 measured), and a full calculation every time
  void SetDate1904(bool on);
  void SetPrecisionAsDisplayed(bool on);
  void SetForceFullCalculation(bool on);
  bool SheetCalculation(uint32_t sheet) const { return !calc_off_.count(sheet); }
  // Ignore Error (the error checking button): the cells' rule is no longer flagged (false: flagged again)
  void SetIgnoreErrors(uint32_t sheet, const Rect& rect, bool ignore);
  // #SPILL! anchors and the range each wants (the grid outlines it, Select Obstructing Cells selects what is in it)
  const std::unordered_map<uint64_t, Rect>& spill_blocked() const { return spill_blocked_; }
  std::vector<uint64_t> circular_cells;  // circular-reference cells found by the last recalculation
  // the sizes of the indexes beside the workbook (dependencies live and stale, pending, volatile, spills): a test or
  // a slow step's triage compares them with a fresh open of the same workbook (debug.engine)
  std::vector<std::pair<std::string, int64_t>> DebugCounts() const;
  // the last structural shifts (StructShift; up to 256, numbered from 1 on)
  std::vector<StructShift> shift_log;
  uint64_t shift_seq = 0;

  // ---- External results ----
  void SetUdfResult(const std::string& key, Value v);
  // an array result (=PY, UDFs that return arrays): the formula spills it
  void SetUdfArrayResult(const std::string& key, std::shared_ptr<formula::Array> a);
  // ---- What-If Analysis and data tools (document_whatif.cpp) ----
  struct GoalSeekResult {
    bool found = false;
    double value = 0, result = 0;
    int iterations = 0;
    std::string error;
  };
  GoalSeekResult GoalSeek(uint32_t sheet, uint32_t set_r, uint32_t set_c, double target, uint32_t ch_sheet, uint32_t ch_r, uint32_t ch_c);
  bool AddDataTable(uint32_t sheet, const Rect& rect, const core::CellPos* row_input, const core::CellPos* col_input, std::string* error);
  bool DataTableAt(uint32_t sheet, uint32_t r, uint32_t c, model::DataTable* out) const;  // r, c in a table's body
  void RecalcDataTables();
  bool AddScenario(uint32_t sheet, model::Scenario sc, std::string* error);  // replaces one with the same name
  bool DeleteScenario(uint32_t sheet, const std::string& name);
  bool ShowScenario(uint32_t sheet, const std::string& name);
  uint32_t ScenarioSummary(uint32_t sheet, const std::vector<core::CellPos>& result_cells, std::string* error);  // the new sheet's id
  // func: 0 sum, 1 count, 2 average, 3 max, 4 min, 5 product, 6 count numbers, 7 stdev, 8 stdevp, 9 var, 10 varp
  // Data > Data Analysis (Analysis ToolPak, document_analysis.cpp): reads the input ranges, runs the tool and writes
  // its output at (out_sheet, out_r, out_c) or on a new sheet; one undo step
  struct AnalysisRequest {
    analysis::Params p;  // the options (its blocks are filled from the ranges below)
    uint32_t in_sheet = 0, in2_sheet = 0, bins_sheet = 0;
    Rect in{}, in2{}, bins{};
    bool has_in = false, has_in2 = false, has_bins = false;
    bool new_sheet = false;  // a new worksheet (named sheet_name, or the next "SheetN")
    std::string sheet_name;
    uint32_t out_sheet = 0, out_r = 0, out_c = 0;
    bool dry_run = false;  // only work out where the results go (out_sheet, out_rect): the UI asks before writing over data
    // charts beside the output, as Excel's: Histogram's Chart Output; Regression's Residual Plots, Line Fit Plots and
    // Normal Probability Plots
    bool chart_output = false, residual_plots = false, line_fit_plots = false, normal_plots = false;
  };
  bool RunAnalysis(AnalysisRequest req, uint32_t* out_sheet, Rect* out_rect, std::string* error);
  void AnalysisCharts(const AnalysisRequest& req, const analysis::Result& res, uint32_t osheet, const Rect& dest);
  // create_links: detail rows of links to each source under a summary formula, grouped (Create links to source data)
  bool Consolidate(uint32_t dest_sheet, uint32_t r, uint32_t c, const std::vector<std::pair<uint32_t, Rect>>& sources, int func, bool top_row,
                   bool left_col, std::string* error, bool create_links = false);
  // the last consolidation onto each sheet (Worksheet.ConsolidationFunction / Options / Sources; not saved)
  struct LastConsolidation {
    int func = 0;
    bool top = false, left = false, links = false;
    std::vector<std::pair<uint32_t, Rect>> sources;
  };
  std::map<uint32_t, LastConsolidation> last_consolidation;
  bool AdvancedFilter(uint32_t sheet, const Rect& list, uint32_t crit_sheet, const Rect& criteria, bool copy, uint32_t dest_sheet, const Rect& dest,
                      bool unique, std::string* error);
  // the cells filled; filled_cells gets (row, text) of each; dry_run only works them out (the live preview)
  uint32_t FlashFill(uint32_t sheet, uint32_t r, uint32_t c, std::string* error, std::vector<std::pair<uint32_t, std::string>>* filled_cells = nullptr,
                     bool dry_run = false);
  struct SolverResult {
    int status = 0;  // Excel's SolverSolve codes: 0 solution, 1 converged, 3 limit, 4 unbounded, 5 infeasible, 7 not linear, 13 model error, 14 integer
    std::string message;
    double objective = 0;
    int iterations = 0;
    std::vector<double> original;  // the variable cells' values before solving
  };
  SolverResult Solve(uint32_t sheet, const model::SolverModel& m);
  // ---- Data tab tools (document_datatools.cpp) ----
  // Data > Subtotal (Range.Subtotal): list includes its header row; group_col and total_cols are sheet columns; func is
  // SUBTOTAL's function number (1 average, 2 count numbers, 3 count, 4 max, 5 min, 6 product, 7 stdev, 8 stdevp, 9 sum,
  // 10 var, 11 varp). Inserts a summary row per group and a grand row, outlines them; replace removes the old ones first
  bool Subtotal(uint32_t sheet, const Rect& list, uint32_t group_col, int func, const std::vector<uint32_t>& total_cols, bool replace,
                bool page_breaks, bool summary_below, std::string* error);
  // Subtotal > Remove All (Range.RemoveSubtotal): deletes the rows holding SUBTOTAL formulas, the list's outline and
  // page breaks; returns the rows deleted
  uint32_t RemoveSubtotal(uint32_t sheet, const Rect& list);
  bool IsSubtotalRow(uint32_t sheet, uint32_t r, uint32_t c0, uint32_t c1) const;
  void SetConsolidateSettings(uint32_t sheet, model::ConsolidateSettings co);  // Consolidate's dialog settings (.ycg SHET 81)
  // Scenario Manager > Merge: copies the scenarios of src_sheet (a name in use gets a number); returns how many
  int MergeScenarios(uint32_t sheet, uint32_t src_sheet);
  // Scenario Summary > Scenario PivotTable report: a new sheet with the result cells per scenario and a PivotTable
  uint32_t ScenarioPivotReport(uint32_t sheet, const std::vector<core::CellPos>& results, std::string* error);
  // Solver Results > Reports: Answer / Sensitivity / Limits sheets for the model just solved (before the model's sheet)
  std::vector<uint32_t> SolverReports(uint32_t sheet, const model::SolverModel& m, const SolverResult& res, bool answer, bool sensitivity, bool limits,
                                      std::string* error);
  // Solver > Save Model / Load Model: the model as cells (objective, variable count, constraints, options) and back
  Rect SaveSolverModel(uint32_t sheet, const model::SolverModel& m, uint32_t r0, uint32_t c0, std::string* error);
  bool LoadSolverModel(uint32_t sheet, const Rect& area, model::SolverModel* out, std::string* error);
  // Get Data > From Text/CSV: fields written from (r0, c0); types per column: 0 numbers and dates recognized, 1 text
  Rect ImportRows(uint32_t sheet, uint32_t r0, uint32_t c0, const std::vector<std::vector<std::string>>& rows, const std::vector<int>& types);
  // Data > Forecast Sheet: a new sheet (before the data's) with a table of FORECAST.ETS formulas and a chart
  struct ForecastRequest {
    uint32_t sheet = 0;
    Rect timeline{}, values{};  // one column each (the header in the first row when has_header)
    bool has_header = true;
    double end = 0;             // Forecast End (on the timeline)
    double start = 0;           // Forecast Start (0 = the last point; earlier: the forecast uses the data up to it)
    double confidence = 0.95;   // Confidence Interval
    bool show_ci = true;
    int seasonality = 0;        // 0 detect automatically, else the period
    bool fill_zeros = false;    // Fill Missing Points Using: Interpolation (false) or Zeros
    int aggregate = 1;          // Aggregate Duplicates Using: 1 AVERAGE, 2 COUNT, 3 COUNTA, 4 MAX, 5 MEDIAN, 6 MIN, 7 SUM
    bool statistics = false;    // Include forecast statistics
    bool column_chart = false;  // the line chart or the column chart
  };
  uint32_t ForecastSheet(const ForecastRequest& rq, std::string* error);  // the new sheet's id (0: error says why)
  void SetSolverModel(uint32_t sheet, model::SolverModel m);
  // "Sheet1!$A$1:$B$2,$C$3" or names -> (sheet, rect) parts
  bool ParseRefList(std::string_view text, uint32_t default_sheet, std::vector<std::pair<uint32_t, Rect>>* out) const;
  bool ResolveNameRect(const std::string& name, uint32_t sheet, uint32_t* out_sheet, Rect* out) const;
  bool block_table() const { return block_table_; }  // the last refused edit was inside a data table
  bool block_param() const { return block_param_; }  // the last refused edit touched a parameter cell

  // ---- [YC] safety for live trading (document_safety.cpp, model/safety.h) ----
  // parameter cells: the user changes them only by typing (the UI checks and confirms, then sets param_commit for
  // that one edit); Python changes them freely. Every change of their values goes to the change log.
  bool param_commit = false;
  // What a table does by itself after a write is a step of its own, for the UI's typing, Ctrl+Enter and Fill Right:
  // its growth (Table AutoExpansion), then a formula filling its empty column (Calculated Column); Excel 16's first
  // Ctrl+Z takes back the column's fill, the next the growth, the next the typing (measured by keys; Ctrl+D takes
  // growth and fill back at once). Set around the write; GrowDeferredTables makes what was noted meanwhile
  bool defer_table_growth = false;
  void GrowDeferredTables();
  void GrowTables(const std::vector<std::pair<uint32_t, Rect>>& writes);  // one step: each write's growth
  std::string audit_user;  // who the user changes of this window are logged as
  const model::ParameterDef* ParameterAt(uint32_t sheet, uint32_t r, uint32_t c) const;
  bool SetParameter(model::ParameterDef def, std::string* error);  // adds, or replaces the one of the same cell
  bool RemoveParameter(uint32_t sheet, uint32_t r, uint32_t c);
  // checks a typed value against a parameter's type and range; *error says what is wrong
  static bool CheckParameterValue(const model::ParameterDef& def, const Value& v, std::string* error);
  void AddAudit(model::AuditEntry e);
  void SetSafetySettings(const model::SafetySettings& s);
  // [YC] the former Notebook window's code cells, kept with the workbook (ADR 0010 / 0011)
  void SetNotebook(std::vector<std::string> cells) {
    if (*wb_.notebook == cells) return;
    wb_.notebook.mut() = std::move(cells);
    wb_.dirty = true;
  }
  void SetAlert(model::AlertRule rule);  // adds, or replaces the rule of the same name
  bool RemoveAlert(const std::string& name);

  void UpdateTopic(const std::string& topic, Value v);  // market tick
  // [YC] Range.BindTopics: cells that show a topic directly (no formula); topics are row-major, "" unbinds a cell.
  // Bindings are runtime state (bound again by Workbook_Open), like a stream formula's subscriptions.
  void BindTopics(uint32_t sheet, const Rect& rect, const std::vector<std::string>& topics);
  void UnbindTopics(uint32_t sheet, const Rect& rect);
  std::vector<std::string> BoundTopics(uint32_t sheet, const Rect& rect) const;  // row-major, "" = not bound
  struct TopicStats {
    uint64_t updates = 0;
    size_t topics = 0, bound_cells = 0;
  };
  TopicStats GetTopicStats() const { return TopicStats{topic_updates_, topics_.size(), bound_topic_.size()}; }

  // ---- [YC] bound tables: tables fed by Python by key (document_bind.cpp, model/bind.h, ADR 0007) ----
  // creates a table at (r0, c0) with the key column first, then the other columns; the definition is saved
  bool CreateBoundTable(model::BoundTable def, uint32_t r0, uint32_t c0, const std::vector<std::string>& columns, std::string* error);
  // changes the definition (formats, flash, freshness); the formats are applied to every row again
  bool UpdateBoundTable(const std::string& name, const std::function<void(model::BoundTable&)>& fn, std::string* error);
  // forgets the binding; delete_table also removes the table and its cells
  bool RemoveBoundTable(const std::string& name, bool delete_table);
  const model::BoundTable* FindBoundTable(std::string_view name) const;
  struct BindResult {
    uint32_t rows = 0, added = 0, removed = 0, cells = 0;
  };
  // applies a column frame (upsert by key, replace, delete, append); the table grows or shrinks as needed
  bool ApplyColumnFrame(const ColumnFrame& f, std::string* error, BindResult* result = nullptr);
  // any table's body as a Replace frame (recording snapshots, columnar reads into Python); the key column is the bound
  // table's key; skip_formulas leaves out columns computed by formulas (a replay must not overwrite them)
  bool TableFrame(std::string_view name, bool skip_formulas, ColumnFrame* out, std::string* error) const;
  // per body row: when it was last updated (PerfCounterNs); shared with the snapshot for the UI
  std::shared_ptr<const BindTimes> BoundRowTimes(const std::string& name) const;
  std::map<std::string, std::shared_ptr<const BindTimes>> AllBoundRowTimes() const {  // by lowercase name
    std::map<std::string, std::shared_ptr<const BindTimes>> m;
    for (const auto& [k, rt] : bind_rt_) m[k] = rt.times;
    return m;
  }
  // the newest and oldest send times of the frames applied since the last publish (tick-to-screen latency)
  void TakeInputTimes(int64_t* newest, int64_t* oldest) {
    *newest = input_newest_ns_;
    *oldest = input_oldest_ns_;
    input_newest_ns_ = input_oldest_ns_ = 0;
  }
  uint64_t bind_cells_written() const { return bind_cells_written_; }
  // refresh caps (BoundColumn::max_hz): when held-back values are due (PerfCounterNs; 0 = none), and writing them
  int64_t BindNextFlush() const;
  void BindFlush(int64_t now);
  void InvalidateUdfCache();

  // ---- Change tracking (for OnChange, snapshots, charts) ----
  std::vector<CellChange> TakeChanges();
  // sheets whose formulas were calculated, and sheets added (AddSheet), since the last call: Worksheet_Calculate and
  // Workbook_NewSheet. Copies and loaded workbooks don't count as added, as in Excel.
  void TakeSheetEvents(std::vector<uint32_t>* calculated, std::vector<uint32_t>* added) {
    calculated->swap(calc_sheets_);
    added->swap(added_sheets_);
    calc_sheets_.clear();
    added_sheets_.clear();
  }
  // cells whose value actually changed during recalculation (up to kMaxCalcChanges, beyond that only the bounding box)
  static constexpr size_t kMaxCalcChanges = 200000;

  // ---- Formula registration (loading, tests) ----
  uint32_t InternFormula(formula::Node root);
  const formula::CompiledFormula* Formula(uint32_t id) const;
  // rebuild the formula dedup table and the dependency index, and mark every formula pending (mark_pending false:
  // the caller marks what changed itself; two million set insertions it then threw away)
  void RebuildAll(bool mark_pending = true);

 private:
  friend class CalcHost;

  // rows or columns whose size, visibility or entire-row / column format a change set
  struct DimSpan {
    uint32_t sheet = 0;
    bool rows = true;
    uint32_t a = 0, b = 0;
  };
  struct UndoEntry {
    std::string label;
    model::Workbook wb;
    std::vector<CellChange> regions;  // the cells it changed (formulas, values, formats, notes...)
    std::vector<DimSpan> dims;
    bool structural = false;
    bool touched = false;  // a change without cells to name (themes, names, rules...): undone with the whole version
    std::vector<StructOp> ops;  // its structural changes (co-authoring sends their inverses on undo)
    bool settled = false;  // nothing waited for calculation when wb was taken: its values are complete
    // the #SPILL! anchors of wb and the ranges they want (spill_blocked_ when wb was taken): the version's values come
    // back without a calculation, so its anchors must still wake when the cells in their way change. Shared by the
    // entries that took the same set (SharedBlocked): a copy an entry of a million #SPILL! anchors (a whole sheet of
    // =SEQUENCE(3) filled down) was 64 MB a step and 6 GB over the undo list (doc_fuzz --wide --more seed 26000048)
    std::shared_ptr<const std::unordered_map<uint64_t, Rect>> blocked;
    // the circular references reported when wb was taken: its values come back without a calculation, and the
    // formulas that read a cycle stay uncalculated only while the cycle is known (shared the same way)
    std::shared_ptr<const std::vector<uint64_t>> circular;
    // undone by putting its cells and rows / columns back, leaving everything else (live data) as it is
    bool Selective() const { return !structural && !touched; }
  };
  void RecordStruct(StructOp op);
  void DropUndo(std::vector<UndoEntry>&& entries);  // through release_later
  bool Settled() const { return pending_.empty() && new_pending_.empty(); }

  void RegisterFormulaCell(uint32_t sheet, uint32_t r, uint32_t c, uint32_t id, bool fresh = false);  // fresh: nothing registered yet
  void UnregisterFormulaCell(uint32_t sheet, uint32_t r, uint32_t c);
  void MarkChanged(uint32_t sheet, const Rect& rect, uint8_t src);
  // whole rows / columns resized, hidden or shown with no cell written: redrawn, and what depends on them is
  // calculated again (SUBTOTAL skips hidden rows), but the arrays spilled across them stay
  void MarkRowsColsChanged(uint32_t sheet, const Rect& band);
  void MarkDependentsOf(uint32_t sheet, const Rect& rect);
  void AddPending(uint64_t key);
  // empties the pending set with every index of it (its tiles, the new ones, the pending spill anchors): never one
  // without the others. Undo cleared the set and left an anchor in pending_spills_, and every range over its
  // spill waited for a cell nobody calculated (doc_fuzz seed 10040032: SUM read 0 on a copied sheet)
  void ClearPending();
  void ErasePending(uint64_t key);
  bool IsPending(uint64_t key) const { return pending_.count(key) != 0; }
  bool interrupted_ = false;
  bool outline_toggle_ = false;  // the outline symbols hide or show lines (EnableOutlining lets them through)
  std::unordered_set<uint32_t> calc_off_;  // SetSheetCalculation(false)
  // Set precision as displayed: the numbers of these cells rounded to what their format shows
  void RoundShownIn(const std::vector<CellChange>& regions);
  bool RoundShown(uint32_t sheet, uint32_t r, uint32_t c, Value* v) const;
  void ShowEnteredOnCalcOff(uint32_t sheet, uint32_t r, uint32_t c);
  bool StopRequested() {
    if (!interrupted_ && interrupt && interrupt->load(std::memory_order_relaxed)) interrupted_ = true;
    if (!interrupted_ && interrupt_after >= 0 && interrupt_after-- == 0) interrupted_ = true;
    return interrupted_;
  }
  bool PendingInRect(uint32_t sheet, const Rect& rect, uint64_t* first) const;
  void RebuildDepsInRegion(uint32_t sheet, const Rect& rect, const model::Workbook& before);
  void NoteRegion(uint32_t sheet, const Rect& rect);
  void NoteFormat(uint32_t sheet, const Rect& rect);  // only formats changed there (undo leaves the values)
  static constexpr uint8_t kUndoFormatOnly = 0x40;     // in an undo region's source
  void NoteDims(uint32_t sheet, bool rows, uint32_t a, uint32_t b);
  void NoteStructural();
  // FillEdge of a large rect: line (the edge's copy) over dest a source cell at a time
  void FillLinesInBulk(uint32_t sheet, const Rect& edge, const Rect& dest, bool vertical, const ClipData& line);
  // a conditional format's formulas written for first cell `from`, written for `to` (relative references move)
  model::CfRule RebasedRule(model::CfRule x, core::CellPos from, core::CellPos to) const;
  void MarkWholeIfOtherStateChanged(UndoEntry& e) const;
  static bool WritesOverSpill(const UndoEntry& e);  // its cells meet an array spilled in its version  // at commit: state outside cells and rows / columns changed
  void CopyRegionFrom(const model::Workbook& from, uint32_t sheet, const Rect& rect, bool formats_only);
  void CopyDimsFrom(const model::Workbook& from, const DimSpan& d);
  void RestoreSelective(const UndoEntry& e);
  void EraseRich(uint32_t sheet, const Rect& rect);
  void EraseLinksIn(uint32_t sheet, const Rect& rect);  // a cell emptied, pasted or filled over loses its hyperlink (Excel)
  // the links a paste or a fill brings (key: row << 16 | col), up to Excel's 65,530 a worksheet
  void PutLinks(uint32_t sheet, const std::vector<std::pair<uint64_t, model::Hyperlink>>& links);
  // rows (columns) from `at` take the sizes of a whole-row (whole-column) copy: px, -1 hidden
  void PutLineSizes(uint32_t sheet, bool rows, uint32_t at, const std::vector<int32_t>& sizes);
  static constexpr size_t kMaxSheetLinks = 65530;
  // the style ids of the cells in rect whose text has fonts of its own (before a format change, for RunsFollowFont)
  std::vector<std::pair<uint64_t, uint32_t>> RichStyleIds(uint32_t sheet, const Rect& rect) const;
  // Excel: a font set on a cell is set for every character of its text: the runs drop their own value of each font
  // property fn assigns or that changed for the cell (before: RichStyleIds from before the change)
  void RunsFollowFont(uint32_t sheet, const std::vector<std::pair<uint64_t, uint32_t>>& before,
                      const std::function<void(model::Style&)>& fn);
  void RestyleNamed(uint32_t name_id, const std::function<void(model::Style&)>& fn);  // every cell with that named style
  void NoteTouched() {
    if (txn_depth_ > 0) pending_undo_.touched = true;
  }
  void CheckpointIfNeeded();
  void RestoreFrom(model::Workbook&& wb, const std::vector<CellChange>& regions, bool structural, bool settled = false);

  // formula offsets for structural changes: f maps old coordinates to new ones (-1 = deleted)
  using MapFn = std::function<int64_t(int64_t)>;
  void ShiftSheet(uint32_t sheet, bool rows, uint32_t at, int64_t delta, const Rect* band);
  void AdjustFormulas(uint32_t sheet, bool rows, uint32_t at, int64_t delta, const Rect* band);

  Value EvaluateFormulaCell(uint64_t key, std::shared_ptr<formula::Array>* spill = nullptr);
  // evaluate part of a formula at origin (Evaluate Formula): ranges up to 100 cells come back as arrays, one cell as its value
  formula::EV EvaluateNodeAt(uint32_t sheet, core::CellPos origin, const formula::Node& n);
  void Propagate();
  void RecalcWaves();                      // step 2 above
  void EvaluatePendingSerial(int64_t only_sheet = -1);  // step 3 above (only_sheet: just that sheet's cells)
  // write a recalculated value and record the change; arr = the whole result of a formula that spills
  void StoreResult(uint64_t key, Value v, std::shared_ptr<formula::Array> arr = nullptr);

  // tables
  model::Table* MutTable(uint32_t sheet, uint32_t id);
  bool ValidTableName(std::string_view name, uint32_t except_id, std::string* error) const;
  void SyncTableHeaders(uint32_t sheet);  // header texts -> column names (blank or repeated headers get ColumnN / Name2)
  void RewriteTableRefs(const std::string& old_table, const std::string& new_table, const std::string& old_col, const std::string& new_col);
  // (table, old column, new column) applied in order at each reference, in one walk
  void RenameTableColumns(const std::vector<std::tuple<std::string, std::string, std::string>>& renames);
  void AddTableDeps(uint64_t key, uint32_t sheet, core::CellPos origin, const formula::Node& root);
  void WriteTotalsRow(uint32_t sheet, uint32_t id);
  void RenameInReferenceTexts(uint32_t id, const std::string& old_name, const std::string& new_name);
  void BreakReferenceTexts(uint32_t id);  // a sheet being deleted: references to it in those texts become #REF!
  // cell formulas with a 3-D reference that starts or ends at sheet `id`: fix rewrites such a reference (true when it
  // changed it); the formulas changed are put back on their cells (`skip`: a sheet whose cells are left alone)
  void RewriteThreeDCells(uint32_t id, const std::function<bool(formula::RefSpec&)>& fix, uint32_t skip);
  void ForEachReferenceText(const std::function<void(std::string&)>& fn);  // names, charts, rules and validation
  void AutoExpandTable(uint32_t sheet, uint32_t r, uint32_t c, bool formula = false) { AutoExpandTable(sheet, Rect::Cell(r, c), formula); }
  // w: the cells being written; formula: a formula is (one under a table makes its row the totals row)
  void AutoExpandTable(uint32_t sheet, const Rect& w, bool formula = false);
  // a table on `sheet` (columns c0..c1, first row first) grew down from its last row old_last to new_last: the ranges
  // that end at old_last inside it grow with it (cell formulas, names, rules, objects)
  void GrowRangesWithTable(uint32_t sheet, uint32_t first, uint32_t old_last, uint32_t new_last, uint32_t c0, uint32_t c1);
  // fill: also fill the column (a calculated column); without it only the formula's own table loses its name
  void FillCalculatedColumn(uint32_t sheet, uint32_t r, uint32_t c, bool fill = true);

 public:
  // Extend data range formats and formulas (Application.ExtendList), after a value typed at (r, c) in the row under a
  // list (Excel 16, measured by keys on 30 layouts): the typed cell takes the format of a run of three or more rows
  // that share it among the five above; an empty cell of the row takes the formula of the row above when four of the
  // five rows above (the row above among them) hold it and every cell of the new row it reads holds something
  void ExtendList(uint32_t sheet, uint32_t r, uint32_t c);

 private:
  bool syncing_tables_ = false;
  std::vector<std::pair<uint32_t, Rect>> deferred_growth_;  // defer_table_growth's writes beside a table
  std::vector<std::pair<uint32_t, core::CellPos>> deferred_calc_;  // and its formulas that may fill a table column
  const std::vector<Rect>* growth_written_ = nullptr;  // GrowDeferredTables: every cell its write wrote, on that sheet
  // header cells a table grew over and the format each takes once SyncTableHeaders made it text: {sheet, r, c, style}
  std::vector<std::array<uint32_t, 4>> grown_header_styles_;

  // protection checks: true = refused (and counted)
  enum class Allow : uint8_t {
    Cells, FormatCells, FormatColumns, FormatRows, InsertColumns, InsertRows, InsertHyperlinks, DeleteColumns, DeleteRows, Sort, AutoFilter, Objects, Pivot, Scenarios, Never
  };
  bool Refuse(uint32_t sheet, const Rect* cells, Allow allow = Allow::Cells);
  // read-only mode (or Protected View) only: every UI change that sheet or workbook protection doesn't govern (rules,
  // notes, names, protection itself, undo...) is refused here, counted like Refuse's so the status bar says why
  bool RefuseReadOnly(uint32_t sheet = 0);
  bool RefuseStructure();
  bool ProtectionApplies(uint32_t sheet) const;
  int EditRangeAt(uint32_t sheet, uint32_t r, uint32_t c, bool* open) const;
  void CountBlock(BlockReason why);
  // Excel refuses an insertion that would push nonblank cells off the sheet ("To prevent possible loss of data...")
  bool RefuseOffSheet(uint32_t sheet, bool rows, uint32_t count, const Rect* band);
  bool block_off_sheet_ = false;
  bool block_too_large_ = false;
  bool block_table_merge_ = false;
  // Excel sorts merged cells only when they all have one size ("To do this, all the merged cells need to be the same size")
  bool RefuseMergedSort(uint32_t sheet, const Rect& rect);
  bool block_merge_size_ = false;
  // Excel refuses deleting rows that hold a table's header row but not the whole table ("...attempting to shift
  // cells in a table"): the first data row became the header
  bool RefuseTableHeaderDelete(uint32_t sheet, uint32_t r0, uint32_t r1, uint32_t c0, uint32_t c1);
  // cells inserted or deleted with a shift that would move part of a table's columns (rows): Excel's "...attempting
  // to shift cells in a table" (measured). Deleting cells inside a table isn't asked here.
  bool RefuseTablePartShift(uint32_t sheet, const Rect& rect, bool vertical, bool insert);
  bool RefuseTableOutsideList(uint32_t sheet, const Rect& rect);  // Sort / Remove Duplicates meeting a table they aren't inside
  // what a table over rect can't hold, as its message ("" when it can): another table, merged cells, any of a
  // multi-cell array. Asked by every way a table gets its cells: AddTable, ResizeTable and AutoExpandTable.
  std::string TableCantHold(uint32_t sheet, const Rect& rect, uint32_t table_id) const;
  // a table keeps a data row: rows r0..r1 that hold every data row of a table (not its header or totals row) leave the
  // first one, its table cells emptied (Excel 16, measured); true when row r0 stays. exact_cols: cells deleted with a
  // shift up, which must span the table's columns exactly
  bool KeepTableRow(uint32_t sheet, uint32_t r0, uint32_t r1, uint32_t c0, uint32_t c1, bool exact_cols);
  bool block_table_shift_ = false;
  // Excel refuses changing part of an array formula (Ctrl+Shift+Enter): writing or clearing cells that cut it (a
  // single cell that is its anchor excepted: a range written cell by cell starts there and replaces the whole array)
  bool RefusePartOfArray(uint32_t sheet, const Rect& cells);
  // ... and Paste Special with an operation over any of one (it can't combine an array cell by cell)
  bool RefuseArrayOperation(uint32_t sheet, const Rect& cells, bool multi_cell_only = false);
  // ... and inserting or deleting rows (rows) or columns through it: count lines at `at`, within band's columns
  // (rows) or rows (columns) when band is given (Insert / Delete with a shift)
  bool RefuseArrayShift(uint32_t sheet, bool rows, uint32_t at, uint32_t count, bool insert, const Rect* band);
  // ... and through a PivotTable report (its placeholder too), as Excel refuses ("...it will affect a PivotTable"):
  // whole lines inserted inside it or deleting part of it, cells shifted across part of it. Lines that hold all of it
  // move it, or delete it with them.
  bool RefusePivotShift(uint32_t sheet, bool rows, uint32_t at, uint32_t count, bool insert, const Rect* band);
  bool block_pivot_shift_ = false;  // with block_pivot_: refused by RefusePivotShift
  // Excel refuses inserting cells that would shift a filtered list's cells (Excel 16, measured: C4:C5 or C3:C4 shifted
  // down or right under a filter hiding rows 3, 6 and 7): its rows would no longer line up with what it hid
  bool RefuseFilterShift(uint32_t sheet, const Rect& rect, bool shift_down, bool sheet_filter_goes = false);
  bool block_filter_shift_ = false;  // with block_table_shift_: refused by RefuseFilterShift
  // (table, column) deleted by the last ShiftSheet, the column empty for a whole table: AdjustFormulas makes what
  // points at them #REF! after the shift
  std::vector<std::pair<std::string, std::string>> gone_table_cols_;
  bool CutHoldsTable(uint32_t sheet, const Rect& cells) const;  // MoveCells: a whole table goes with the cells
  bool block_array_ = false;
  uint32_t protect_blocks_ = 0, block_sheet_ = 0;
  Rect block_cells_{0, 0, 0, 0};
  int block_range_ = -1;
  bool block_pivot_ = false;
  bool block_read_only_ = false;
  bool block_table_ = false;
  bool block_param_ = false;
  void SeedParameterLog();
  std::unordered_map<uint64_t, std::string> param_seen_;  // value text of each parameter cell when last logged
  void AuditParameters(const std::vector<CellChange>& changes);
  bool table_writing_ = false, in_tables_ = false, tables_dirty_ = false;
  bool pivot_writing_ = false;  // RefreshPivot writes its own report
  bool pivot_blocked_ = false;  // the last RefreshPivot was refused by what the report would cover (nothing changed)
  // each PivotTable's cache as of its last refresh, by (sheet, id): GETPIVOTDATA reads the report as it was refreshed,
  // not the source edited since (Excel 16, measured). Undo and Redo drop them (the source is read again)
  std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<const PivotCache>> pivot_caches_;
  bool block_python_ = false;
  std::set<std::pair<uint32_t, size_t>> unlocked_ranges_;

 public:
  // the Windows account working on the workbook ("DOMAIN\\user", set by the app): an Allow Edit Range whose
  // Permissions allow it needs no password
  std::string current_user;

 private:

  // spills (see the top of this file)
  void RebuildSpillIndex();
  void IndexSpill(uint64_t anchor, const Rect& rect, bool add);
  bool PendingSpillAt(uint32_t sheet, uint32_t r, uint32_t c, uint64_t* anchor) const;  // (r,c) is a member of a pending anchor
  bool PendingSpillIn(uint32_t sheet, const Rect& rect, uint64_t* anchor) const;
  void UpdateSpill(uint64_t key, Value* v, const std::shared_ptr<formula::Array>& arr);
  void ClearSpill(uint64_t anchor, const Rect* keep);
  void FormatStockDates(uint32_t sheet, const Rect& t, const formula::Array& a, const formula::Node& root);  // forget the range and clear its members (except anchor and keep)
  void UnspillTouched(uint32_t sheet, const Rect& rect);  // something was written into members: spill again (or #SPILL!)
  void ClearSheetSpills(uint32_t sheet);  // before rows / columns move: dynamic ranges are dropped and computed again
  void DropOrphanSpills();  // anchors that lost their formula
  void RestoreBlockedSpills(const std::unordered_map<uint64_t, Rect>& blocked);
  void RestoreCircular(const std::vector<uint64_t>& circular);  // after Undo / Redo put a version back
  // spill_blocked_ and circular_cells as an undo entry keeps them: the last copy again while they are the same
  std::shared_ptr<const std::unordered_map<uint64_t, Rect>> SharedBlocked();
  std::shared_ptr<const std::vector<uint64_t>> SharedCircular();
  std::shared_ptr<const std::unordered_map<uint64_t, Rect>> blocked_shared_;
  std::shared_ptr<const std::vector<uint64_t>> circular_shared_;
  // the one rule of what keeps an array from spilling over t: a value or formula of another cell, a merged area, another
  // array's range (own: the anchor's current spill, whose values aren't in the way)
  bool SpillBlocked(uint64_t key, const Rect& t, const Rect* own) const;
  void RecheckBlockedSpills();  // before a calculation: #SPILL! anchors whose range is free now are calculated again
  void MeetSpillCycle(uint64_t anchor, const Rect& spill);  // a spill that reaches back to its anchor: a cycle
  bool ReadsThroughNames(uint64_t key) const;  // its formula reads cells no index holds (a name, OFFSET, INDIRECT)
  void SetDynamicDeps(uint64_t key, std::vector<std::pair<int32_t, Rect>>& reads);  // its index: its references + reads
  void TakeSpillsUnder(uint32_t sheet, const Rect& rect);  // a table gets these cells: the spills over them give them up
  void WakeBlockedSpills(uint32_t sheet, const Rect& rect);  // something changed where a #SPILL! anchor wanted to spill
  static uint64_t TileKeyOf(uint64_t k) {
    return (uint64_t{KeySheet(k)} << 48) | (uint64_t{KeyRow(k) / model::kTileRows} << 16) | (KeyCol(k) / model::kTileCols);
  }

  std::unordered_map<uint64_t, Rect> spill_anchor_;                 // anchor key -> spill range (all sheets)
  std::unordered_map<uint64_t, std::vector<uint64_t>> spill_tiles_;  // tile key -> anchors whose range touches the tile
  std::unordered_set<uint64_t> pending_spills_;                      // anchors that are pending
  std::unordered_map<uint64_t, Rect> spill_blocked_;                 // #SPILL! anchors -> the range they want
  // a formula's references, unresolved, and whether it reads a table: by formula id (the formula table only grows and
  // an id's formula never changes), so registering a million copies of one formula walks its tree once
  struct RefInfo {
    std::vector<formula::RefSpec> refs;
    bool table = false;
  };
  std::unordered_map<uint32_t, RefInfo> ref_info_;
  const RefInfo& RefInfoOf(uint32_t id, const formula::CompiledFormula* f);
  uint32_t last_ref_id_ = 0;
  const RefInfo* last_ref_info_ = nullptr;
  bool register_pending_ = true;  // RegisterFormulaCell marks the cell pending (off in RebuildAll(false))
  uint64_t storing_ = ~0ull;           // the cell StoreResult is storing (an array that spills over what it reads)
  bool storing_read_itself_ = false;  // its spill marked it pending again
  bool spill_moved_ = false;          // its spill appeared or changed size
  bool spills_changed_ = false;       // some spill's cells changed in this calculation (Recalc's pass for dynamic readers)
  std::unordered_set<uint64_t> respill_checked_;  // anchors calculated once more this recalculation (Recalc)
  bool spill_hook_ = true;  // off while undo puts a version back (its spill ranges are already consistent)

  std::unique_ptr<CalcHost> host_;
  std::vector<int32_t> id_index_;
  std::vector<uint64_t> new_pending_;
  uint64_t current_cell_ = 0;
  std::unordered_map<std::string, std::unordered_set<uint64_t>> udf_cells_;

  model::Workbook wb_;
  std::unordered_map<std::string, uint32_t> formula_ids_;  // SerializeKey -> id
  DepIndex deps_;
  std::unordered_set<uint64_t> pending_;
  std::vector<uint64_t> entered_;  // formulas entered while calculation is manual (CalcEntered)
  bool stale_reads_ = false;       // CalcEntered: pending cells are read with the value they show
  // RecalcSheet / RecalcRange: pending cells of other sheets (outside only_rect_) are read with the value they show
  int64_t only_sheet_ = -1;
  Rect only_rect_{0, 0, core::kMaxRows - 1, core::kMaxCols - 1};
  bool StaleAt(int32_t sheet, uint32_t r, uint32_t c) const {
    return stale_reads_ || (only_sheet_ >= 0 && (sheet != only_sheet_ || !only_rect_.Contains(r, c)));
  }
  // the part of rect this calculation calculates (*rect is narrowed to it); false: all of it is read as it shows
  bool LivePart(int32_t sheet, Rect* rect) const;
  // (r, c) is a cell of an array (a spill) whose anchor is in cycle_block_ (*anchor: it)
  bool CycleSpillAt(uint32_t sheet, uint32_t r, uint32_t c, uint64_t* anchor) const;
  // a cell of rect is in cycle_block_ (*first: it)
  bool CycleBlockedIn(uint32_t sheet, const Rect& rect, uint64_t* first) const;
  bool stale_hit_ = false;               // the formula being calculated read a pending cell as it shows (only_sheet_)
  std::vector<uint64_t> stale_kept_;     // RecalcSheet / RecalcRange: calculated from waiting values, so still pending
  // Circular references. A pass that meets one doesn't calculate the cells of the cycle, nor those that read them
  // (Excel): they are taken out of the pending set (cycle_aborted_; the cycle's own cells in cycle_members_) and their
  // readers meet them in cycle_block_. Without iteration they keep the values they show (a formula just entered shows
  // 0) and circular_cells reports the cycle; with iteration ResolveCycles calculates them in rounds.
  std::unordered_set<uint64_t> cycle_block_;
  std::vector<uint64_t> cycle_aborted_, cycle_members_;
  std::vector<uint64_t> fresh_aborted_;  // formulas just entered that AbortStack set aside (ResolveCycles)
  // iterative calculation: the cells of the cycles found, calculated again at every recalculation that follows an
  // edit, F9 or a full calculation (Excel keeps them dirty), MaxIterations rounds each time
  std::vector<uint64_t> iter_cells_;
  uint64_t input_seq_ = 0, iter_seq_ = 0;  // edits so far (MarkChanged), and at the last recalculation
  uint64_t cycles_checked_seq_ = ~0ull;     // input_seq_ when BeginCycles last walked circular_cells
  bool force_iteration_ = false;           // CalculateNow: the cycles are calculated again without an edit
  // formulas entered (written, pasted, filled) and not calculated since: one that can't be calculated because of a
  // circular reference shows 0, the others keep their values (Excel)
  std::unordered_set<uint64_t> fresh_formulas_;
  std::unordered_set<uint64_t> restored_formulas_;
  uint64_t steps_made_ = 0;  // put back by Undo / Redo since the last calculation
  void AbortStack(std::vector<uint64_t>& stack, uint64_t needed);  // the stack waits for a cycle: not calculated
  void BeginCycles(int64_t sheet, const Rect& rect);  // before a pass: iteration cells pending again, cycles blocked
  void ResolveCycles();  // after a pass: the cycles it met (iterated, or reported)
  // an array entered with Ctrl+Shift+Enter that met itself as a cycle by reading its own cells: calculated when no
  // value of it reads itself (true), as in Excel; else its cells are as they were (false)
  // *waits_for: the cell not calculated yet that it read, when that is why it didn't settle
  bool SettleArrayReadingItself(uint64_t anchor, uint64_t* waits_for = nullptr);
  // several arrays entered with Ctrl+Shift+Enter that read each other's cells (and nothing else of the cycle): their
  // values settled together, each read of the others' cells waiting until that value is set (doc_fuzz --xlsx seed
  // 24002581569: B3 {=A1:B3*2} and A2:A4 {=A1:B3*2} met as a cycle in one order of calculation, not in another)
  bool SettleArraysTogether(const std::vector<uint64_t>& anchors);
  void RecalcOnly(uint32_t sheet, const Rect& rect, bool every_formula);  // RecalcSheet / RecalcRange
  std::unordered_map<uint64_t, uint32_t> pending_tiles_;  // tile key -> number of pending cells
  // (sheet << 32 | column) -> number of pending cells: a range of a column beside pending ones in the same tiles
  // (A:A next to a calculated column of a million formulas) is known to wait for nothing without a look at each cell
  std::unordered_map<uint64_t, uint32_t> pending_cols_;
  std::unordered_set<uint64_t> volatile_;
  std::unordered_set<uint64_t> in_progress_;
  std::unordered_set<uint64_t> cycle_break_;

  // UDFs and streams
  std::unordered_map<std::string, Value> udf_cache_;
  std::unordered_map<std::string, std::shared_ptr<formula::Array>> udf_arrays_;
  std::unordered_set<std::string> udf_requested_;
  // the answers from before the Python host started again: shown until the new host answers (RecalcUdfCells)
  std::unordered_map<std::string, Value> udf_stale_;
  std::unordered_map<std::string, std::shared_ptr<formula::Array>> udf_stale_arrays_;
  std::vector<uint64_t> udf_no_host_cells_;  // formulas that called a Python function while no host was there
  bool udf_without_host_ = false;            // more than a million of them: scan for #NAME? instead
  struct Topic {
    Value value;
    bool has_value = false;
    std::unordered_set<uint64_t> cells;  // formulas (=QUOTE(...))
    std::unordered_set<uint64_t> bound;  // cells bound with BindTopics
    bool subscribed = false;
  };
  std::unordered_map<std::string, Topic> topics_;
  std::unordered_map<uint64_t, std::string> bound_topic_;  // cell -> topic
  uint64_t topic_updates_ = 0;

  // bound tables at run time: key -> body row, and the rows' update times (by lowercase table name)
  struct BindRt {
    std::unordered_map<std::string, uint32_t> row_of;   // text keys
    std::unordered_map<int64_t, uint32_t> int_row_of;   // whole-number keys (the common case: strikes, ids)
    const uint32_t* Find(const Value& key) const;
    void Put(const Value& key, uint32_t row);
    uint32_t rows = 0;  // body rows when the map was built
    std::shared_ptr<BindTimes> times;
    std::vector<model::BoundColumn> spec;  // the flash / refresh-cap columns the times were made for
    // values held back by a refresh cap: (body row << 20 | table column) -> the row's key and the newest value; written
    // by BindFlush. The key finds the row again when rows moved meanwhile (sort, delete).
    struct Held {
      Value key, v;
      uint64_t seq = 0;  // newer values have larger numbers
    };
    std::map<uint64_t, Held> pending;
    int64_t next_flush = 0;
    int key_col = -1;  // table column of the key
    uint64_t held_seq = 0;
  };
  std::unordered_map<std::string, BindRt> bind_rt_;
  int64_t input_newest_ns_ = 0, input_oldest_ns_ = 0;
  uint64_t bind_cells_written_ = 0;
  bool BindGrow(uint32_t sheet, uint32_t table_id, uint32_t rows, std::string* error);
  void BindSyncTimes(BindRt& rt, const model::BoundTable& def, const model::Table& t, size_t rows);
  void BindApplyFormats(const model::BoundTable& def, uint32_t sheet, const Rect& rows);
  void BindRebuildMap(BindRt& rt, uint32_t sheet, const model::Table& t, int key_col);
  void BindFlushTable(const std::string& name, BindRt& rt, int64_t now, bool force);

  std::vector<CellChange> changes_;
  std::vector<uint32_t> calc_sheets_, added_sheets_;  // TakeSheetEvents
  size_t calc_change_cells_ = 0;
  double round_change_ = 0;  // largest change written by StoreResult since it was reset (iterative calculation)
  CalcStats stats_;

  // transactions
  int txn_depth_ = 0;
  bool txn_has_checkpoint_ = false;
  std::vector<UndoEntry> undo_, redo_;
  UndoEntry pending_undo_;
};

// a name's Refers To as Excel shows it: a union kept as ((A,B)) (Document::DefineName) loses the two pairs again
// (=A,B); any other text, with or without its leading =, comes back as it is
std::string ShownNameUnion(const std::string& refers_to, const formula::SheetResolver* sheets);
// Go To Special > Data validation > Same: the ranges of every rule with the settings of the one at (r, c), its formulas
// read through sheets
std::vector<Rect> SameValidationAreas(const model::Sheet& s, uint32_t r, uint32_t c, const formula::SheetResolver* sheets);

}  // namespace yc::engine
