// SPDX-License-Identifier: Apache-2.0
// rule_eval.h - evaluation on a read-only snapshot: conditional formatting, data validation (used on the UI thread, not through the engine)
//
// All snapshot values are already computed, so this EvalHost never throws NeedCell; UDFs and streams return #N/A (not supported in conditional formats).
// Only visible cells are evaluated (PLAN §8.0); rules that need statistics over the whole range (top N, average, duplicates, color scales, data bars)
// compute them on first use and cache them in the evaluator (one evaluator per snapshot version).
#pragma once

#include "formula/eval.h"
#include "formula/parser.h"
#include "model/workbook.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yc::engine {

class SnapshotEvalHost final : public formula::EvalHost, public formula::SheetResolver {
 public:
  explicit SnapshotEvalHost(const model::Workbook& wb) : wb_(wb) {}
  const core::Value& Cell(int32_t sheet, uint32_t r, uint32_t c) override;
  void ForEachCell(int32_t sheet, const core::Rect& rect, const std::function<bool(uint32_t, uint32_t, const core::Value&)>& fn) override;
  uint32_t RowLimit(int32_t sheet) override;
  core::Rect UsedRect(int32_t sheet) override;
  int32_t FindSheet(std::string_view name) const override;
  int32_t FindSheet(std::string_view name) override { return static_cast<const SnapshotEvalHost*>(this)->FindSheet(name); }
  std::string SheetName(int32_t sheet) const override;
  std::string SheetName(int32_t sheet) override { return static_cast<const SnapshotEvalHost*>(this)->SheetName(sheet); }
  int32_t SheetIndex(int32_t sheet) override { return wb_.IndexOfId(static_cast<uint32_t>(sheet)) + 1; }
  int32_t SheetCount() override { return static_cast<int32_t>(wb_.sheets.size()); }
  bool ResolveName(std::string_view name, int32_t current_sheet, core::CellPos origin, formula::EV* out) override;
  formula::EV CallUdf(const std::string&, const std::vector<formula::EV>&, int32_t, core::CellPos) override { return formula::EV::Err(core::Err::NA); }
  formula::EV Stream(const std::string&, const std::vector<formula::EV>&, int32_t, core::CellPos) override { return formula::EV::Err(core::Err::NA); }
  bool RowHidden(int32_t sheet, uint32_t r) override;
  int TableRef(int32_t sheet, const formula::Node& n, core::CellPos origin, formula::RangeRef* out) override;

  // formula text (without '=') parsed relative to base and evaluated at (r,c)
  core::Value Evaluate(uint32_t sheet, const formula::Node& root, uint32_t r, uint32_t c);
  // one cell read as if it held v (data validation judges the value being entered, as Excel does)
  void SetOverride(uint32_t sheet, uint32_t r, uint32_t c, const core::Value& v) {
    has_ov_ = true;
    ov_sheet_ = static_cast<int32_t>(sheet);
    ov_r_ = r;
    ov_c_ = c;
    ov_v_ = v;
  }

 private:
  void ForEachRaw(int32_t sheet, const core::Rect& rect, const std::function<bool(uint32_t, uint32_t, const core::Value&)>& fn);
  const model::Workbook& wb_;
  int depth_ = 0;
  bool has_ov_ = false;
  int32_t ov_sheet_ = -1;
  uint32_t ov_r_ = 0, ov_c_ = 0;
  core::Value ov_v_;
};

// result of the conditional formats applied to one cell
struct CfVisual {
  bool has_fill = false, has_font = false;
  uint32_t fill = 0, font = 0;
  int8_t bold = -1, italic = -1, strike = -1;
  uint32_t border = 0;  // an outline around the cell (ARGB; 0 = none)
  float bar = -1.0f;  // data bar length ratio (0-1); < 0 means none
  float bar_axis = 0.0f;      // where the bar starts (0-1): the axis at 0 when the range has negative values
  bool bar_negative = false;  // the bar goes left from the axis, in red (Excel 2010+ default)
  uint32_t bar_color = 0;
  int8_t icon_style = 0;  // icon set (XlIconSet), 0 = none
  int8_t icon = -1;       // icon index, 0 = the lowest
  bool icon_only = false;
  // data bar look: gradient or solid, its border (ARGB, 0 = none), drawn from the right, the value hidden
  bool bar_gradient = true, bar_rtl = false, bar_only = false;
  uint32_t bar_border = 0;
  // the rule's Format...: number format code, underline, pattern, lines per side (left, top, right, bottom)
  std::string numfmt;
  int8_t underline = -1, pattern = -1;
  uint32_t pattern_color = 0;
  uint8_t side_style[4] = {0, 0, 0, 0};
  uint32_t side_color[4] = {0, 0, 0, 0};
  bool Any() const {
    return has_fill || has_font || bold >= 0 || italic >= 0 || strike >= 0 || border || bar >= 0 || icon >= 0 || !numfmt.empty() || underline >= 0 ||
           pattern >= 0 || side_style[0] || side_style[1] || side_style[2] || side_style[3];
  }
};

class CondFormatEval {
 public:
  // wb is held by value (Cow members, cheap to copy), so the evaluator doesn't depend on the snapshot's lifetime
  CondFormatEval(model::Workbook wb, uint32_t sheet);
  bool Empty() const { return rules_.empty(); }
  // returns whether any rule applied
  bool Eval(uint32_t r, uint32_t c, CfVisual* out);
  // a new snapshot (live data, a recalculation in slices) made every rule gather its range again: a color scale over
  // a million cells sorted a million numbers a frame. A rule unchanged since prev (the same rules: the Cow is shared)
  // keeps prev's statistics for half a second when its range holds 100,000 cells or more, so its colors follow the
  // data twice a second there and the screen keeps its frame rate
  void KeepStatsFrom(const CondFormatEval& prev);

 private:
  struct Stats {
    bool ready = false;
    std::vector<double> nums;  // sorted
    double avg = 0, lo = 0, hi = 0, mid = 0, threshold = 0;
    std::unordered_map<std::string, int> counts;
    size_t cells = 0;      // the cells gathered
    size_t errors = 0;     // the error values among them
    double built_at = 0;   // when (steady clock, seconds)
  };
  struct Prepared {
    const model::CfRule* rule = nullptr;
    core::Rect bounds{};
    std::optional<formula::ParseResult> f1, f2;
    std::shared_ptr<Stats> stats = std::make_shared<Stats>();
  };
  bool Match(Prepared& p, uint32_t r, uint32_t c, const core::Value& v, CfVisual* out);
  void BuildStats(Prepared& p);
  std::string KeyOf(const core::Value& v) const;

  model::Workbook wb_;
  uint32_t sheet_;
  const model::Sheet* sh_ = nullptr;
  SnapshotEvalHost host_;
  std::vector<Prepared> rules_;
};

// ---- Data validation ----
const model::DvRule* FindValidation(const model::Sheet& sh, uint32_t r, uint32_t c);
// items of a list validation (displayed text)
// values (optional): each item's value beside its text (a source cell's value, a constant as typed), so an entry
// matches by value as in Excel (1 is in a list showing 1.00)
std::vector<std::string> ValidationList(const model::Workbook& wb, uint32_t sheet, const model::DvRule& rule, uint32_t r, uint32_t c,
                                        std::vector<core::Value>* values = nullptr);
// whether the text typed by the user passes validation; on failure message explains why (a default when the rule has no error message)
bool ValidateInput(const model::Workbook& wb, uint32_t sheet, uint32_t r, uint32_t c, const std::string& text, const model::DvRule& rule, std::string* message);

}  // namespace yc::engine
