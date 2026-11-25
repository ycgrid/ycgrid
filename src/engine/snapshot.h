// SPDX-License-Identifier: Apache-2.0
// snapshot.h - immutable snapshot the engine publishes to the UI / Python (PLAN §4.5, ADR 0001)
#pragma once

#include "engine/external.h"
#include "engine/document.h"
#include "formula/parser.h"
#include "model/workbook.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace yc::engine {

struct Snapshot {
  model::Workbook wb;
  uint64_t version = 0;
  CalcStats stats;
  bool can_undo = false, can_redo = false;
  std::string undo_label, redo_label;
  std::vector<std::string> undo_labels, redo_labels;  // newest first, up to 100 (the Undo / Redo drop-down lists)
  uint32_t undo_depth = 0, redo_depth = 0;  // steps on the undo / redo lists (ui.state: tests count them)
  uint64_t steps_made = 0;  // Document::StepsMade: a step added at the list's limit changes this, not the depth
  uint64_t user_done = 0;  // the user actions (PostUser) done when it was published (Engine::UserActionsPending)
  size_t circular = 0;
  uint64_t first_circular = 0;
  std::vector<std::pair<uint64_t, core::Rect>> spill_blocked;  // #SPILL! anchors (cell keys) -> the range each wants
  std::vector<uint64_t> circular_cells;  // the first ones found (up to 4096): the status bar looks for one on the sheet shown
  bool calculating = false;  // still pending (when a large recalculation runs in slices)
  bool manual_calc = false;  // calculation is manual: pending cells wait for F9
  bool semiauto_calc = false;  // Automatic except for data tables
  // protection: operations refused so far (the UI shows Excel's message when this grows), where the last one was
  uint32_t protect_blocks = 0, block_sheet = 0;
  core::Rect block_cells{0, 0, 0, 0};  // what the last refused change tried to change
  int block_range = -1;
  bool block_pivot = false;
  bool block_read_only = false;
  bool block_table = false;  // the last refused edit was inside a data table
  bool block_too_large = false;
  bool block_table_merge = false;  // the last refused merge was over a table  // the last refused fill would write past 20,000,000 cells
  bool block_off_sheet = false;  // the last refused insertion would push nonblank cells off the sheet
  bool block_merge_size = false;  // the last refused sort met merged cells of different sizes
  bool block_table_shift = false;  // the last refused deletion took a table's header row
  bool block_pivot_shift = false;
  bool block_filter_shift = false;  // the last refused insertion would shift a filtered list's cells  // the last refused insertion or deletion went through a PivotTable report
  bool block_array = false;  // the last refused change would cut an array formula
  bool block_python = false;
  bool block_param = false;  // the last refused edit touched a parameter cell
  // [YC] bound tables: when the newest / oldest data shown by this snapshot were sent (PerfCounterNs; 0 = none), and
  // each table's row update times (by lowercase name) for freshness
  int64_t input_newest_ns = 0, input_oldest_ns = 0;
  uint64_t bind_cells = 0;  // cells written by bound-table frames so far
  std::map<std::string, std::shared_ptr<const BindTimes>> bind_times;
  // since the previous snapshot: sheets whose formulas were calculated and sheets added (Worksheet_Calculate,
  // Workbook_NewSheet; Engine::on_changes sees every snapshot's, the UI may skip snapshots)
  std::vector<uint32_t> calc_sheets, added_sheets;
  // the last rows / columns inserted or deleted (Document::shift_log), shared between snapshots
  std::shared_ptr<const std::vector<StructShift>> shift_log;

  const model::Sheet* Sheet(uint32_t id) const { return wb.SheetById(id); }
};

// sheet name resolution from a Workbook (to turn formulas back into A1 text in a snapshot)
class WorkbookResolver final : public formula::SheetResolver {
 public:
  // register_external: another workbook's sheet that isn't linked yet is added to the workbook's links (loading a file)
  explicit WorkbookResolver(const model::Workbook& wb, bool register_external = false) : wb_(wb), register_(register_external) {}
  int32_t FindSheet(std::string_view name) const override {
    const int32_t i = wb_.FindSheet(name);
    if (i < 0 && name.find('[') != std::string_view::npos)
      return register_ ? FindExternalSheet(const_cast<model::Workbook&>(wb_), name, true) : FindExternalSheet(wb_, name);
    return i < 0 ? -1 : static_cast<int32_t>(wb_.sheets[static_cast<size_t>(i)]->id);
  }
  std::string SheetName(int32_t id) const override {
    if (IsExternalSheet(id)) return ExternalSheetName(wb_, id);
    const model::Sheet* s = id >= 0 ? wb_.SheetById(static_cast<uint32_t>(id)) : nullptr;
    return s ? s->name : std::string();
  }

 private:
  bool LegacyDecimal() const override { return wb_.legacy_decimal; }
  const model::Workbook& wb_;
  bool register_ = false;
};

// A1 text of the formula (with '='); empty string when there is no formula
std::string FormulaTextOf(const model::Workbook& wb, uint32_t sheet, uint32_t r, uint32_t c, bool r1c1 = false);
// contents shown in the formula bar
// {=TABLE(row_input,col_input)} for a cell in a data table's body, else empty
std::string DataTableText(const model::Sheet& s, uint32_t r, uint32_t c);
std::string InputTextOf(const model::Workbook& wb, uint32_t sheet, uint32_t r, uint32_t c, bool r1c1 = false);
// effective style of a cell
const model::Style& StyleOf(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c);

}  // namespace yc::engine
