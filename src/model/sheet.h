// SPDX-License-Identifier: Apache-2.0
// sheet.h - worksheet (PLAN §5)
//
// Hot/cold split: ValueTile (values, may COW every cycle) and MetaTile (style, formula id; COW only on formatting or structural changes).
// Every member of Sheet is a shareable COW structure; publishing a snapshot = copying the Sheet object (pointers only).
#pragma once

#include "core/cow.h"
#include "core/geometry.h"
#include "core/value.h"
#include "model/dim_index.h"
#include "model/objects.h"
#include "model/page_setup.h"
#include "model/pivot.h"
#include "model/rich_text.h"
#include "model/rules.h"
#include "model/table.h"
#include "model/tile_dir.h"
#include "model/whatif.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace yc::model {

struct ValueTile {
  core::Value v[kTileCells];
  uint64_t occ[kTileCells / 64]{};  // non-empty bitmap
  uint32_t count = 0;
  uint32_t heap_strings = 0;  // values holding a shared string (a copy only needs to count them again when > 0)

  ValueTile() = default;
  // copy on write after a publish (live data writes hit hundreds of tiles a second): one block copy, then the strings
  // of the occupied cells get one more reference
  ValueTile(const ValueTile& o) : count(o.count), heap_strings(o.heap_strings) {
    std::memcpy(static_cast<void*>(v), static_cast<const void*>(o.v), sizeof(v));
    std::memcpy(occ, o.occ, sizeof(occ));
    if (!heap_strings) return;  // numbers only (live prices): nothing to count
    for (uint32_t w = 0; w < kTileCells / 64; ++w) {
      for (uint64_t m = occ[w]; m; m &= m - 1) v[w * 64 + static_cast<uint32_t>(std::countr_zero(m))].RetainAfterByteCopy();
    }
  }
  ValueTile& operator=(const ValueTile&) = delete;

  bool Occupied(uint32_t idx) const { return (occ[idx >> 6] >> (idx & 63)) & 1; }
  void Set(uint32_t idx, core::Value val) {
    const bool was = Occupied(idx);
    const bool now = !val.IsEmpty();
    heap_strings += (val.IsHeapString() ? 1u : 0u) - (v[idx].IsHeapString() ? 1u : 0u);
    v[idx] = std::move(val);
    if (was != now) {
      occ[idx >> 6] ^= uint64_t{1} << (idx & 63);
      count += now ? 1 : static_cast<uint32_t>(-1);
    }
  }
};

struct MetaTile {
  uint32_t style[kTileCells]{};
  uint32_t formula[kTileCells]{};
  uint32_t formula_count = 0;
  uint32_t style_count = 0;
};

// formula result spread over several cells: a dynamic array that spills from its anchor, or a legacy
// Ctrl+Shift+Enter array formula (fixed size, edited only as a whole). Member cells hold plain values.
struct SpillRange {
  core::Rect rect;   // includes the anchor (its upper-left cell)
  bool cse = false;
  bool operator==(const SpillRange&) const = default;
};

// Review -> Allow Edit Ranges: ranges users may change while the sheet is protected (with an optional password)
struct AllowEditRange {
  std::string title;
  core::Rect rect{};
  uint16_t hash = 0;  // password hash (ProtectionHash), 0 = no password
  // Permissions...: Windows accounts ("DOMAIN\\user") that edit the range without its password (allow) or not (deny)
  struct User {
    std::string name;
    bool allow = true;
    bool operator==(const User&) const = default;
  };
  std::vector<User> users;
  bool operator==(const AllowEditRange&) const = default;
};

// Review -> Protect Sheet
struct SheetProtection {
  bool on = false;
  uint16_t hash = 0;       // password hash, 0 = no password
  bool ui_only = false;    // UserInterfaceOnly: Python may still change the sheet (not saved, like Excel)
  // Worksheet.EnableAutoFilter / EnableOutlining / EnablePivotTable: with UserInterfaceOnly protection a person can
  // still use the AutoFilter arrows, the outline symbols and the PivotTables (not saved, as Excel 16's files don't
  // keep them: they read False in a workbook just opened)
  bool enable_autofilter = false, enable_outlining = false, enable_pivot = false;
  bool select_locked = true, select_unlocked = true;
  bool format_cells = false, format_columns = false, format_rows = false;
  bool insert_columns = false, insert_rows = false, insert_hyperlinks = false;
  bool delete_columns = false, delete_rows = false;
  bool sort = false, autofilter = false, pivot = false, objects = false;
  bool scenarios = false;  // Edit scenarios (Worksheet.ProtectScenarios is its opposite); saved as the 15th allow flag
  std::vector<AllowEditRange> ranges;
  bool operator==(const SheetProtection&) const = default;
};

// Excel's legacy 16-bit password hash (what Protect / Unprotect compare)
inline uint16_t ProtectionHash(std::string_view password) {
  if (password.empty()) return 0;
  uint32_t h = 0;
  for (size_t i = password.size(); i-- > 0;) {
    h = ((h >> 14) & 0x01) | ((h << 1) & 0x7FFF);
    h ^= static_cast<unsigned char>(password[i]);
  }
  h = ((h >> 14) & 0x01) | ((h << 1) & 0x7FFF);
  h ^= static_cast<uint32_t>(password.size());
  h ^= 0xCE4B;
  return static_cast<uint16_t>(h);
}

// view state of this sheet in the main window (for saving; owned by the UI at run time, synced back before saving)
struct SheetView {
  uint32_t top_row = 0, left_col = 0;
  uint32_t act_row = 0, act_col = 0, anc_row = 0, anc_col = 0;
  float zoom = 1.0f;
  uint32_t freeze_row = 0, freeze_col = 0;  // 0 = not frozen; otherwise the number of frozen rows / columns
  bool show_gridlines = true, show_headings = true;
  bool operator==(const SheetView&) const = default;
};

class Sheet {
 public:
  uint32_t id = 0;
  std::string name;
  std::string code_name;
  uint32_t row_limit = core::kDefaultRows;
  uint32_t col_limit = core::kMaxCols;
  bool visible = true;
  bool very_hidden = false;  // xlSheetVeryHidden: hidden and left out of Unhide; only code shows it again
  uint32_t tab_color = 0;
  bool chart_sheet = false;  // a chart sheet: no cells, its one chart fills the window
  // Data > Outline settings (Outline.SummaryRow / SummaryColumn / AutomaticStyles): where a group's +/- button sits
  bool outline_below = true, outline_right = true, outline_styles = false;

  TileDir<ValueTile> values;
  TileDir<MetaTile> meta;
  core::Cow<DimIndex> rows{std::make_shared<DimIndex>(20)};
  core::Cow<DimIndex> cols{std::make_shared<DimIndex>(64)};
  core::Cow<std::vector<core::Rect>> merges;
  core::Cow<std::map<uint32_t, uint32_t>> row_styles;  // entire-row formats
  core::Cow<std::map<uint32_t, uint32_t>> col_styles;  // entire-column formats
  core::Cow<std::vector<Control>> controls;
  core::Cow<std::vector<ChartObj>> charts;
  core::Cow<std::map<uint64_t, CellNote>> notes;  // key = row << 16 | col
  core::Cow<std::vector<core::Rect>> ignored_errors;  // Ignore Error: cells error checking leaves alone (.ycg sheet key 47)
  core::Cow<std::map<uint64_t, Hyperlink>> links;  // key = row << 16 | col
  core::Cow<std::map<uint64_t, RichText>> rich;    // key = row << 16 | col: fonts inside the text (stale once the text changes)
  core::Cow<std::map<std::string, std::string>> store;  // sheet KV (values are msgpack bytes)
  core::Cow<std::vector<CfRule>> cond_formats;   // conditional formats (by priority)
  core::Cow<std::vector<DvRule>> validations;    // data validation
  core::Cow<AutoFilter> autofilter;              // AutoFilter
  core::Cow<std::map<uint64_t, SpillRange>> spills;  // key = anchor row << 16 | col
  core::Cow<SheetProtection> protection;
  core::Cow<std::vector<Table>> tables;  // ListObjects
  core::Cow<std::vector<ShapeObj>> shapes;  // pictures, shapes, text boxes
  // Paste Options > Linked Picture: a picture shape (id) that shows cells (source sheet, rect) as they are now; the app
  // draws the picture again when they change (.ycg sheet key 70)
  struct LinkedPicture {
    uint32_t shape = 0, sheet = 0;
    core::Rect rect{};
    bool operator==(const LinkedPicture& o) const { return shape == o.shape && sheet == o.sheet && rect == o.rect; }
  };
  core::Cow<std::vector<LinkedPicture>> linked_pictures;
  // the sheet's tab is selected (grouped sheets, Excel's [Group]); set when the workbook is saved (.ycg sheet key 71)
  bool tab_selected = false;
  core::Cow<std::vector<SparklineGroup>> sparklines;
  core::Cow<PageSetup> page_setup;  // Page Layout -> Page Setup
  core::Cow<std::string> background;  // Page Layout > Background: a picture's bytes tiled behind the cells (screen only; .ycg sheet key 100)
  core::Cow<std::vector<PivotTable>> pivots;
  core::Cow<std::vector<Slicer>> slicers;
  core::Cow<std::vector<DataTable>> data_tables;  // What-If -> Data Table
  core::Cow<std::vector<Scenario>> scenarios;     // What-If -> Scenario Manager
  core::Cow<SolverModel> solver;                  // Data -> Solver
  core::Cow<ConsolidateSettings> consolidate;     // Data -> Consolidate: the dialog's settings and references
  SheetView view;

  // used range (a grow-only estimate; ComputeUsedRange computes the exact value)
  bool has_used = false;
  core::Rect used{0, 0, 0, 0};

  // ---- Reading ----
  const core::Value& GetValue(uint32_t r, uint32_t c) const {
    static const core::Value kEmpty;
    const ValueTile* t = values.GetCell(r, c);
    return t ? t->v[TileIndex(r, c)] : kEmpty;
  }
  uint32_t GetFormula(uint32_t r, uint32_t c) const {
    const MetaTile* t = meta.GetCell(r, c);
    return t ? t->formula[TileIndex(r, c)] : 0;
  }
  uint32_t GetOwnStyle(uint32_t r, uint32_t c) const {
    const MetaTile* t = meta.GetCell(r, c);
    return t ? t->style[TileIndex(r, c)] : 0;
  }
  // effective style: cell > entire row > entire column > default (PLAN §5.4)
  uint32_t GetStyle(uint32_t r, uint32_t c) const {
    const uint32_t own = GetOwnStyle(r, c);
    if (own) return own;
    if (!row_styles->empty()) {
      auto it = row_styles->find(r);
      if (it != row_styles->end()) return it->second;
    }
    if (!col_styles->empty()) {
      auto it = col_styles->find(c);
      if (it != col_styles->end()) return it->second;
    }
    return 0;
  }
  bool IsEmptyCell(uint32_t r, uint32_t c) const { return GetValue(r, c).IsEmpty() && GetFormula(r, c) == 0; }

  // merging: the merged area containing (r,c)
  const core::Rect* MergeAt(uint32_t r, uint32_t c) const {
    for (const auto& m : *merges) {
      if (m.Contains(r, c)) return &m;
    }
    return nullptr;
  }

  // spill / array formula that covers (r,c) (anchor included); nullptr when none
  const SpillRange* SpillAt(uint32_t r, uint32_t c, uint32_t* anchor_r = nullptr, uint32_t* anchor_c = nullptr) const {
    for (const auto& [k, sp] : *spills) {
      if (!sp.rect.Contains(r, c)) continue;
      if (anchor_r) *anchor_r = static_cast<uint32_t>(k >> 16);
      if (anchor_c) *anchor_c = static_cast<uint32_t>(k & 0xFFFF);
      return &sp;
    }
    return nullptr;
  }
  const SpillRange* SpillOf(uint32_t anchor_r, uint32_t anchor_c) const {
    auto it = spills->find((uint64_t{anchor_r} << 16) | anchor_c);
    return it == spills->end() ? nullptr : &it->second;
  }

  // ---- Writing (engine thread) ----
  void SetValue(uint32_t r, uint32_t c, core::Value v) {
    const bool empty = v.IsEmpty();
    if (empty && !values.GetCell(r, c)) return;
    values.MutableCell(r, c).Set(TileIndex(r, c), std::move(v));
    if (!empty) Touch(r, c);
  }
  void SetFormula(uint32_t r, uint32_t c, uint32_t def) {
    if (def == 0 && !meta.GetCell(r, c)) return;
    MetaTile& t = meta.MutableCell(r, c);
    uint32_t& slot = t.formula[TileIndex(r, c)];
    if ((slot != 0) != (def != 0)) t.formula_count += def ? 1 : static_cast<uint32_t>(-1);
    slot = def;
    if (def) Touch(r, c);
  }
  void SetStyle(uint32_t r, uint32_t c, uint32_t style) {
    if (style == 0 && !meta.GetCell(r, c)) return;
    MetaTile& t = meta.MutableCell(r, c);
    uint32_t& slot = t.style[TileIndex(r, c)];
    if ((slot != 0) != (style != 0)) t.style_count += style ? 1 : static_cast<uint32_t>(-1);
    slot = style;
    if (style) Touch(r, c);
  }
  // the style a cell takes from its row or column (0: neither has formats of its own)
  uint32_t LineStyleAt(uint32_t r, uint32_t c) const {
    if (!row_styles->empty()) {
      auto it = row_styles->find(r);
      if (it != row_styles->end()) return it->second;
    }
    if (!col_styles->empty()) {
      auto it = col_styles->find(c);
      if (it != col_styles->end()) return it->second;
    }
    return 0;
  }
  // gives the cell the formats of `style` (an interned id, 0 the default) whatever its row or column has: the
  // default under a row's or column's formats is the cell's own Normal, as in Excel. SetStyle(0) instead takes the
  // cell's own style off, and the cell shows its row's or column's again.
  void ShowStyle(uint32_t r, uint32_t c, uint32_t style) {
    if (style == kStyleOwnNormal) style = 0;
    if (style == 0 && LineStyleAt(r, c)) style = kStyleOwnNormal;
    if (style != GetOwnStyle(r, c)) SetStyle(r, c, style);
  }
  void Touch(uint32_t r, uint32_t c) {
    if (!has_used) {
      used = core::Rect::Cell(r, c);
      has_used = true;
    } else {
      used = used.Union(core::Rect::Cell(r, c));
    }
  }
  void ComputeUsedRange();

  // call before publishing a snapshot (finishes deferred computations)
  void Finalize() const {
    rows->Finalize();
    cols->Finalize();
  }
};

}  // namespace yc::model
