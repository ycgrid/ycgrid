// SPDX-License-Identifier: Apache-2.0
// document_protect.cpp - sheet and workbook protection (Review -> Protect Sheet / Protect Workbook / Allow Edit Ranges)
//
// The checks sit at the entry points of Document (cell contents, formats, rows and columns, objects, sheets), so the
// UI and Python are refused the same way. A refused operation does nothing and counts a "block"; the UI shows Excel's
// message when the count changes, and the Python bridge reports E_PROTECTED.
#include "engine/document.h"
#include "engine/tile_iter.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

namespace yc::engine {

using model::Sheet;

bool Document::ProtectSheet(uint32_t sheet, const model::SheetProtection& p, std::string_view password) {
  if (!S(sheet) || RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  model::SheetProtection pr = p;
  pr.on = true;
  pr.hash = model::ProtectionHash(password);
  pr.ranges = s->protection->ranges;  // the edit ranges stay as they are
  s->protection.mut() = std::move(pr);
  wb_.dirty = true;
  changes_.push_back(CellChange{sheet, Rect{0, 0, 0, 0}, kSrcUser});
  return true;
}

bool Document::UnprotectSheet(uint32_t sheet, std::string_view password) {
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s || !s->protection->on) return s != nullptr;
  if (s->protection->hash && s->protection->hash != model::ProtectionHash(password)) return false;
  s->protection.mut().on = false;
  s->protection.mut().hash = 0;
  s->protection.mut().ui_only = false;
  for (auto it = unlocked_ranges_.begin(); it != unlocked_ranges_.end();) it = it->first == sheet ? unlocked_ranges_.erase(it) : std::next(it);
  wb_.dirty = true;
  changes_.push_back(CellChange{sheet, Rect{0, 0, 0, 0}, kSrcUser});
  return true;
}

bool Document::ProtectWorkbook(std::string_view password, bool structure, bool windows) {
  if (RefuseReadOnly()) return false;
  wb_.protect_structure = structure;
  wb_.protect_windows = windows;
  wb_.protect_hash = model::ProtectionHash(password);
  wb_.dirty = true;
  return true;
}

bool Document::UnprotectWorkbook(std::string_view password) {
  if (RefuseReadOnly()) return false;
  if (wb_.protect_hash && wb_.protect_hash != model::ProtectionHash(password)) return false;
  wb_.protect_structure = wb_.protect_windows = false;
  wb_.protect_hash = 0;
  wb_.dirty = true;
  return true;
}

void Document::SetAllowEditRanges(uint32_t sheet, std::vector<model::AllowEditRange> ranges) {
  if (!S(sheet) || RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  s->protection.mut().ranges = std::move(ranges);
  for (auto it = unlocked_ranges_.begin(); it != unlocked_ranges_.end();) it = it->first == sheet ? unlocked_ranges_.erase(it) : std::next(it);
  wb_.dirty = true;
}

bool Document::UnlockEditRange(uint32_t sheet, size_t index, std::string_view password) {
  const Sheet* s = S(sheet);
  if (!s || index >= s->protection->ranges.size()) return false;
  if (s->protection->ranges[index].hash != model::ProtectionHash(password)) return false;
  unlocked_ranges_.insert({sheet, index});
  return true;
}

bool Document::ProtectionApplies(uint32_t sheet) const {
  const Sheet* s = S(sheet);
  if (!s || !s->protection->on) return false;
  return !(python_call && s->protection->ui_only);  // UserInterfaceOnly: code may change the sheet
}

int Document::EditRangeAt(uint32_t sheet, uint32_t r, uint32_t c, bool* open) const {
  const Sheet* s = S(sheet);
  if (!s) return -1;
  const auto& ranges = s->protection->ranges;
  int found = -1;
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (!ranges[i].rect.Contains(r, c)) continue;
    // Permissions: an account allowed edits the range without the password
    bool allowed = false;
    auto same = [](const std::string& a, const std::string& b) {
      return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
    };
    for (const auto& u : ranges[i].users) {
      if (u.allow && !current_user.empty() && same(u.name, current_user)) allowed = true;
    }
    if (ranges[i].hash == 0 || allowed || unlocked_ranges_.count({sheet, i})) {
      *open = true;
      return static_cast<int>(i);
    }
    if (found < 0) found = static_cast<int>(i);
  }
  *open = false;
  return found;
}

// whether any cell of a large range is locked, read from what styles there are rather than cell by cell: a cell shows its
// own style, else its row's, else its column's, else the default (locked). A range past 2,000,000 cells counted as
// locked without a look, and Delete over unlocked columns B:C was refused on a protected sheet.
bool Document::LockedByLines(const Sheet& s, Rect r) const {
  const auto& tbl = *wb_.styles;
  r.r1 = std::min(r.r1, s.row_limit - 1);
  r.c1 = std::min(r.c1, s.col_limit - 1);
  if (r.r0 > r.r1 || r.c0 > r.c1) return false;
  const uint64_t width = r.Cols();
  const auto& rs = *s.row_styles;
  const auto& cs = *s.col_styles;
  std::unordered_map<uint32_t, uint64_t> own_in_row;  // a styled row's cells with a style of their own
  std::unordered_map<uint32_t, uint64_t> own_in_col;  // a column's cells with a style of their own, in rows without one
  bool locked = false;
  ForEachCellInTiles(s.meta, r, [&](uint32_t rr, uint32_t cc, const model::MetaTile& t, uint32_t idx) {
    if (!t.style[idx]) return;
    locked = locked || tbl.Get(t.style[idx]).locked;
    if (rs.count(rr)) ++own_in_row[rr];
    else ++own_in_col[cc];
  });
  if (locked) return true;
  uint64_t styled_rows = 0;
  for (auto it = rs.lower_bound(r.r0); it != rs.end() && it->first <= r.r1; ++it) {
    ++styled_rows;
    if (tbl.Get(it->second).locked && own_in_row[it->first] < width) return true;
  }
  const uint64_t plain_rows = r.Rows() - styled_rows;  // their cells show the column's style
  if (plain_rows == 0) return false;
  for (uint32_t c = r.c0; c <= r.c1; ++c) {
    const auto it = cs.find(c);
    const bool col_locked = it == cs.end() || tbl.Get(it->second).locked;
    if (col_locked && own_in_col[c] < plain_rows) return true;
  }
  return false;
}

bool Document::IsLockedRange(uint32_t sheet, const Rect& r, int* range_needing_password) const {
  if (range_needing_password) *range_needing_password = -1;
  if (!ProtectionApplies(sheet)) return false;
  const Sheet* s = S(sheet);
  const auto& tbl = *wb_.styles;
  bool in_edit_range = false;
  for (const auto& er : s->protection->ranges) in_edit_range = in_edit_range || er.rect.Intersects(r);
  if (r.Count() > 4096 && !in_edit_range) return LockedByLines(*s, r);
  if (r.Count() > 2'000'000) return true;  // allowed ranges over a huge range: a cell at a time would take minutes
  for (uint32_t rr = r.r0; rr <= r.r1; ++rr) {
    for (uint32_t cc = r.c0; cc <= r.c1; ++cc) {
      bool open = false;
      const int er = EditRangeAt(sheet, rr, cc, &open);
      if (open) continue;
      if (!tbl.Get(s->GetStyle(rr, cc)).locked) continue;
      if (range_needing_password) *range_needing_password = er;
      return true;
    }
  }
  return false;
}

void Document::CountBlock(BlockReason why) {
  ++protect_blocks_;
  // what picks the message is this refusal's alone: each kind is cleared here, and the refusal sets its own after.
  // Refuse cleared some of them on every call, refusing or not, so a refusal of a table's header row followed by
  // the next area's allowed check (Delete Sheet Rows over areas) showed the protected-sheet message (basics seq270330)
  block_pivot_ = block_pivot_shift_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = false;
  block_merge_size_ = block_table_shift_ = block_filter_shift_ = block_too_large_ = block_table_merge_ = false;
  block_array_ = why == BlockReason::Array;  // every refusal says whether it was this one (the UI picks the message by it)
  if (block_counter) block_counter->fetch_add(1, std::memory_order_relaxed);
  if (block_reason) block_reason->store(static_cast<int>(why), std::memory_order_relaxed);
}

bool Document::RefuseOffSheet(uint32_t sheet, bool rows, uint32_t count, const Rect* band) {
  const Sheet* s = S(sheet);
  if (!s || count == 0) return false;
  // a table that reaches the sheet's last column can't be pushed past XFD (Excel 16 refuses an insertion that would
  // move cells in a table, measured). Rows grow on demand instead (PLAN 5.1, kept on purpose): a table on whole
  // columns grows the sheet as the cells do (ShiftSheet), where Excel refuses Rows.Insert (measured)
  // so can't a merged area: Excel 16 refuses an insertion that would push one off the sheet with its off-sheet message
  // (measured: merged cells count as nonblank there); shifted, A7:XFD7 became A7:XFE7 (doc_fuzz --wide seed 21200010)
  if (!rows) {
    const uint32_t last_from = count >= s->col_limit ? 0 : s->col_limit - count;
    for (const Rect& m : *s->merges) {
      if (m.c1 < last_from || (band && (m.r0 > band->r1 || m.r1 < band->r0))) continue;
      CountBlock(BlockReason::OffSheet);
      block_sheet_ = sheet;
      block_cells_ = m;
      block_range_ = -1;
      block_python_ = python_call;
      block_off_sheet_ = true;
      return true;
    }
  }
  if (!rows) {
    const uint32_t lim = s->col_limit;
    const uint32_t last_from = count >= lim ? 0 : lim - count;
    for (const auto& t : *s->tables) {
      const bool at_end = t.range.c1 >= last_from;
      const bool in_band = !band || (t.range.r0 <= band->r1 && t.range.r1 >= band->r0);
      if (!at_end || !in_band) continue;
      CountBlock(BlockReason::TableShift);
      block_sheet_ = sheet;
      block_cells_ = t.range;
      block_range_ = -1;
      block_python_ = python_call;
      block_table_shift_ = true;
      return true;
    }
  }
  if (!s->has_used) return false;
  // rows grow on demand (PLAN 5.1): only the hard limit pushes them off; columns stop at XFD
  const uint32_t limit = rows ? core::kMaxRows : core::kMaxCols;
  const uint32_t first = count >= limit ? 0 : limit - count;
  Rect r = rows ? Rect{first, band ? band->c0 : 0, limit - 1, band ? band->c1 : core::kMaxCols - 1}
                : Rect{band ? band->r0 : 0, first, band ? band->r1 : s->row_limit - 1, limit - 1};
  r.r1 = std::min(r.r1, s->used.r1);
  r.c1 = std::min(r.c1, s->used.c1);
  if (r.r0 > r.r1 || r.c0 > r.c1) return false;
  // the cells that hold something, tile by tile: whole rows inserted with a shift right over a sheet grown to two
  // million rows read 34 billion cells one by one (33 s, basics seq321952)
  bool found = false;
  uint32_t fr = 0, fc = 0;
  ForEachCellInTiles(s->values, r, [&](uint32_t rr, uint32_t cc, const model::ValueTile& t, uint32_t idx) {
    if (!found && t.Occupied(idx)) found = true, fr = rr, fc = cc;
  });
  if (!found) {
    ForEachCellInTiles(s->meta, r, [&](uint32_t rr, uint32_t cc, const model::MetaTile& t, uint32_t idx) {
      if (!found && t.formula[idx]) found = true, fr = rr, fc = cc;
    });
  }
  if (!found) return false;
  CountBlock(BlockReason::OffSheet);
  block_sheet_ = sheet;
  block_cells_ = Rect::Cell(fr, fc);
  block_range_ = -1;
  block_python_ = python_call;
  block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_merge_size_ = block_table_shift_ = false;
  block_off_sheet_ = true;
  return true;
}

bool Document::RefuseTableHeaderDelete(uint32_t sheet, uint32_t r0, uint32_t r1, uint32_t c0, uint32_t c1) {
  const Sheet* s = S(sheet);
  if (!s || s->tables->empty()) return false;
  for (const auto& t : *s->tables) {
    if (!t.header || t.range.r0 < r0 || t.range.r0 > r1 || t.range.c1 < c0 || t.range.c0 > c1) continue;
    const bool whole = t.range.r1 <= r1 && t.range.c0 >= c0 && t.range.c1 <= c1;  // the table goes with the rows
    if (whole) continue;
    CountBlock(BlockReason::TableShift);
    block_sheet_ = sheet;
    block_cells_ = t.range;
    block_range_ = -1;
    block_python_ = python_call;
    block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_merge_size_ = false;
    block_table_shift_ = true;
    block_filter_shift_ = false;
    return true;
  }
  return false;
}

bool Document::RefuseTablePartShift(uint32_t sheet, const Rect& rect, bool vertical, bool insert) {
  // Excel 16 (measured, table C7:D9): Delete E6 / C6 up and Insert C6 / C8 down, B8 right, Delete B8 left are refused;
  // C6:D6 or B6:E6 up and B7:B9 right move the whole table; cells below or right of it (C10, E8) leave it alone
  const Sheet* s = S(sheet);
  if (!s || s->tables->empty()) return false;
  for (const auto& t : *s->tables) {
    const Rect& g = t.range;
    // the band that moves: the columns (rows) of the cells, from the cells on
    const uint32_t b0 = vertical ? rect.c0 : rect.r0, b1 = vertical ? rect.c1 : rect.r1;
    const uint32_t t0 = vertical ? g.c0 : g.r0, t1 = vertical ? g.c1 : g.r1;
    if (b1 < t0 || b0 > t1 || (b0 <= t0 && b1 >= t1)) continue;  // beside it, or all of its width
    const uint32_t from = vertical ? rect.r0 : rect.c0, tend = vertical ? g.r1 : g.c1;
    if (tend < from) continue;  // above (left of) the cells: nothing of it moves
    const uint32_t cut_end = vertical ? rect.r1 : rect.c1, tstart = vertical ? g.r0 : g.c0;
    if (!insert && cut_end >= tstart) continue;  // cells deleted inside it: Delete Table Rows' own rules
    CountBlock(BlockReason::TableShift);
    block_sheet_ = sheet;
    block_cells_ = g;
    block_range_ = -1;
    block_python_ = python_call;
    block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_merge_size_ = false;
    block_table_shift_ = true;
    block_filter_shift_ = false;
    return true;
  }
  return false;
}

bool Document::RefuseTableOutsideList(uint32_t sheet, const Rect& rect) {
  // Sort and Remove Duplicates over cells that meet a table work only inside one table: a table inside the range, over
  // part of it or touching one corner is refused (Excel 16's Range.Sort and RemoveDuplicates raise, measured:
  // tblrange_ cases), while the table's whole range or part of its body is sorted. The message the UI shows isn't
  // measured; it is the table one
  const Sheet* s = S(sheet);
  if (!s || s->tables->empty()) return false;
  for (const auto& t : *s->tables) {
    if (!t.range.Intersects(rect) || t.range.Contains(rect)) continue;
    CountBlock(BlockReason::TableShift);
    block_sheet_ = sheet;
    block_cells_ = t.range;
    block_range_ = -1;
    block_python_ = python_call;
    block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_merge_size_ = false;
    block_table_shift_ = true;
    block_filter_shift_ = false;
    return true;
  }
  return false;
}

bool Document::KeepTableRow(uint32_t sheet, uint32_t r0, uint32_t r1, uint32_t c0, uint32_t c1, bool exact_cols) {
  const Sheet* s = S(sheet);
  if (!s || s->tables->empty()) return false;
  std::vector<Rect> emptied;
  for (const auto& t : *s->tables) {
    const Rect d = t.Data();
    if (!t.header || !t.HasData() || d.r0 != r0 || d.r1 > r1) continue;  // every data row, from the first
    if (t.totals && t.range.r1 <= r1) continue;                            // the totals row goes too
    if (exact_cols ? (t.range.c0 != c0 || t.range.c1 != c1) : (t.range.c0 < c0 || t.range.c1 > c1)) continue;
    emptied.push_back(Rect{r0, t.range.c0, r0, t.range.c1});
  }
  for (const Rect& r : emptied) ClearContents(sheet, r);
  return !emptied.empty();
}

bool Document::RefuseMergedSort(uint32_t sheet, const Rect& rect) {
  const Sheet* s = S(sheet);
  if (!s || s->merges->empty()) return false;
  // once a merged cell is in the list, every cell must be in a merge of one size, inside the list: B1:B2 merged
  // beside the single cells A1 and A2 is refused (Excel)
  bool any = false, bad = false;
  uint64_t rows = 0, cols = 0, covered = 0;
  for (const Rect& m : *s->merges) {
    if (!m.Intersects(rect)) continue;
    if (!any) {
      any = true;
      rows = m.Rows();
      cols = m.Cols();
    }
    bad = bad || m.Rows() != rows || m.Cols() != cols || m.r0 < rect.r0 || m.c0 < rect.c0 || m.r1 > rect.r1 || m.c1 > rect.c1;
    covered += m.Rows() * m.Cols();
  }
  if (any && (bad || covered != rect.Rows() * rect.Cols())) {
    CountBlock(BlockReason::MergeSize);
    block_sheet_ = sheet;
    block_cells_ = rect;
    block_range_ = -1;
    block_python_ = python_call;
    block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_table_shift_ = false;
    block_merge_size_ = true;
    return true;
  }
  return false;
}

namespace {
bool CutsArrays(const model::Sheet* s, const std::function<bool(const Rect&)>& cuts, Rect* hit) {
  if (!s || s->spills->empty()) return false;
  for (const auto& [k, sp] : *s->spills) {
    if (!sp.cse || !cuts(sp.rect)) continue;
    *hit = sp.rect;
    return true;
  }
  return false;
}
}  // namespace

bool Document::RefusePartOfArray(uint32_t sheet, const Rect& cells) {
  Rect hit;
  const bool cut = CutsArrays(S(sheet), [&](const Rect& a) {
    if (!a.Intersects(cells) || cells.Contains(a)) return false;
    // the anchor alone is part of the array too: Excel 16 changes nothing there (measured: Range("A4").Formula over
    // A4:B4's array leaves =D1*2). Let through "as a range written cell by cell", it put a plain formula over the
    // anchor and left the array's record and its other cells' values behind (doc_fuzz seed 17241693)
    return true;
  }, &hit);
  if (!cut) return false;
  CountBlock(BlockReason::Array);
  block_sheet_ = sheet;
  block_cells_ = hit;
  block_range_ = -1;
  block_python_ = python_call;
  block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_merge_size_ = block_table_shift_ = false;
  return true;
}

bool Document::RefuseArrayOperation(uint32_t sheet, const Rect& cells, bool multi_cell_only) {
  // Paste Special with an operation combines each cell with the copied one: an array's anchor would take a formula
  // of its own (refused) while its other cells took constants, and the array stayed over them (doc_fuzz --more seed
  // 80142: Redo then showed the array's values). Not measured in Excel (it needs Windows' clipboard): refused whole.
  // multi_cell_only: an array of one cell moves as a cell does (Sort, measured: sort_one_cell_array cases)
  Rect hit;
  if (!CutsArrays(S(sheet), [&](const Rect& a) { return a.Intersects(cells) && !(multi_cell_only && a.IsCell()); }, &hit)) return false;
  CountBlock(BlockReason::Array);
  block_sheet_ = sheet;
  block_cells_ = hit;
  block_range_ = -1;
  block_python_ = python_call;
  return true;
}

bool Document::RefuseArrayShift(uint32_t sheet, bool rows, uint32_t at, uint32_t count, bool insert, const Rect* band) {
  if (count == 0) return false;
  const uint64_t last = uint64_t{at} + count - 1;
  Rect hit;
  const bool cut = CutsArrays(S(sheet), [&](const Rect& a) {
    // along the shift (rows: a's rows) and across it (rows: a's columns, against the band's)
    const uint32_t lo = rows ? a.r0 : a.c0, hi = rows ? a.r1 : a.c1;
    const uint32_t x0 = rows ? a.c0 : a.r0, x1 = rows ? a.c1 : a.r1;
    if (band) {
      const uint32_t b0 = rows ? band->c0 : band->r0, b1 = rows ? band->c1 : band->r1;
      if (x1 < b0 || x0 > b1) return false;            // beside the cells that move
      if (hi >= at && (x0 < b0 || x1 > b1)) return true;  // part of it moves, part stays
    }
    if (insert) return lo < at && at <= hi;              // new lines inside it
    return uint64_t{hi} >= at && lo <= last && !(lo >= at && uint64_t{hi} <= last);  // deleted in part
  }, &hit);
  if (!cut) return false;
  CountBlock(BlockReason::Array);
  block_sheet_ = sheet;
  block_cells_ = hit;
  block_range_ = -1;
  block_python_ = python_call;
  block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_merge_size_ = block_table_shift_ = false;
  return true;
}

bool Document::RefuseFilterShift(uint32_t sheet, const Rect& rect, bool shift_down, bool sheet_filter_goes) {
  const Sheet* s = S(sheet);
  if (!s) return false;
  const model::AutoFilter& af = *s->autofilter;
  const bool lines = (rect.c0 == 0 && rect.c1 >= core::kMaxCols - 1) || (rect.r0 == 0 && rect.r1 >= s->row_limit - 1);
  const bool table_af = std::any_of(s->tables->begin(), s->tables->end(), [&](const model::Table& t) { return af.on && t.range.Intersects(af.range); });
  bool refuse = false;
  // the sheet's own AutoFilter (not a table's buttons): Excel 16 inserts no cells while it filters, rows hidden or
  // not, anywhere on the sheet, and with its buttons alone none inside its list (measured 2026-11-20 and 21: B3:C4
  // under a hidden row, A7:C7 below the list, G3:H4 beside it; A2:A3 in a list without criteria, where G3:G4 goes in).
  // Whole rows and columns go in. Only cells shifted across a hidden row were refused
  if (!lines && !sheet_filter_goes && af.on && !table_af) {
    const bool filtering = !af.columns.empty() ||
                           std::any_of(af.hidden.begin(), af.hidden.end(), [&](const auto& h) { return s->rows->IsHidden(h.first); });
    refuse = filtering || rect.Intersects(af.range);
  }
  if (!refuse) {
    // a table's filter: cells shifted across rows it hides
    const bool tables_hide = std::any_of(s->tables->begin(), s->tables->end(), [](const model::Table& t) { return !t.filter.hidden.empty(); });
    if (!tables_hide && (sheet_filter_goes || !table_af || af.hidden.empty())) return false;
    const Rect moved = shift_down ? Rect{rect.r0, rect.c0, s->row_limit - 1, rect.c1} : rect;
    const std::vector<Rect> runs = FilterShownRuns(*s, {moved});
    if (runs.size() == 1 && runs[0] == moved) return false;
  }
  CountBlock(BlockReason::TableShift);
  block_sheet_ = sheet;
  block_cells_ = rect;
  block_range_ = -1;
  block_python_ = python_call;
  block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_merge_size_ = false;
  block_table_shift_ = block_filter_shift_ = true;
  return true;
}

bool Document::RefusePivotShift(uint32_t sheet, bool rows, uint32_t at, uint32_t count, bool insert, const Rect* band) {
  const Sheet* s = S(sheet);
  if (count == 0 || !s || s->pivots->empty() || pivot_writing_) return false;
  const uint64_t last = uint64_t{at} + count - 1;
  for (const auto& p : *s->pivots) {
    const Rect a = PivotArea(p);
    const uint32_t lo = rows ? a.r0 : a.c0, hi = rows ? a.r1 : a.c1;
    const uint32_t x0 = rows ? a.c0 : a.r0, x1 = rows ? a.c1 : a.r1;
    bool cut;
    if (band) {
      // cells: only a shift across part of the report (Excel 16, measured: D4 shifted right beside E3:F6 is refused,
      // D3:D6 moves the report; cells inside it shifted with all its columns are taken as the report's items)
      const uint32_t b0 = rows ? band->c0 : band->r0, b1 = rows ? band->c1 : band->r1;
      cut = !(x1 < b0 || x0 > b1) && hi >= at && (x0 < b0 || x1 > b1);
    } else if (insert) {
      cut = lo < at && at <= hi;  // new lines inside it; before its first line they move it
    } else {
      cut = uint64_t{hi} >= at && lo <= last && !(lo >= at && uint64_t{hi} <= last);  // deleted in part
    }
    if (!cut) continue;
    CountBlock(BlockReason::Pivot);
    block_sheet_ = sheet;
    block_cells_ = a;
    block_range_ = -1;
    block_python_ = python_call;
    block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_merge_size_ = block_table_shift_ = false;
    block_pivot_ = block_pivot_shift_ = true;
    return true;
  }
  return false;
}

bool Document::Refuse(uint32_t sheet, const Rect* cells, Allow allow) {
  // read-only mode: nothing the user does changes the workbook (typing, paste, delete, fill, formatting, structure).
  // (the kinds of the last refusal are left as they are unless this call refuses: CountBlock clears them)
  if (RefuseReadOnly(sheet)) return true;
  // a PivotTable report can only be changed through the PivotTable (Excel: "Cannot change this part of a PivotTable report.")
  // the body of a data table is computed (Excel: Cannot change part of a data table)
  if (allow == Allow::Cells && cells && !table_writing_) {
    if (const Sheet* ts = S(sheet); ts && !ts->data_tables->empty()) {
      for (const auto& t : *ts->data_tables) {
        if (!t.Body().Intersects(*cells)) continue;
        CountBlock(BlockReason::DataTable);
        block_sheet_ = sheet;
        block_range_ = -1;
        block_python_ = python_call;
        block_pivot_ = false;
        block_table_ = true;
        return true;
      }
    }
  }
  if (allow == Allow::Cells && cells && !pivot_writing_) {
    if (const Sheet* ps = S(sheet); ps && !ps->pivots->empty()) {
      for (const auto& p : *ps->pivots) {
        if (!p.has_output || !p.output.Intersects(*cells)) continue;
        CountBlock(BlockReason::Pivot);
        block_sheet_ = sheet;
        block_range_ = -1;
        block_python_ = python_call;
        block_pivot_ = true;
        block_pivot_shift_ = false;
        block_table_ = false;
        return true;
      }
    }
  }
  // parameter cells: the user changes them by typing only (the UI confirms and sets param_commit); Python freely
  if (allow == Allow::Cells && cells && !python_call && !param_commit && !wb_.parameters->empty()) {
    for (const auto& pd : *wb_.parameters) {
      if (pd.sheet != sheet || !cells->Contains(pd.row, pd.col)) continue;
      CountBlock(BlockReason::Parameter);
      block_sheet_ = sheet;
      block_range_ = -1;
      block_python_ = false;
      block_pivot_ = block_table_ = false;
      block_param_ = true;
      return true;
    }
  }
  // part of an array formula can't be changed (Excel: "You can't change part of an array.")
  if (allow == Allow::Cells && cells && RefusePartOfArray(sheet, *cells)) return true;
  if (!ProtectionApplies(sheet)) return false;
  const auto& pr = *S(sheet)->protection;
  bool ok = true;
  switch (allow) {
    case Allow::Cells: break;
    case Allow::FormatCells: ok = pr.format_cells; break;
    case Allow::FormatColumns: ok = pr.format_columns || (outline_toggle_ && pr.ui_only && pr.enable_outlining); break;
    case Allow::FormatRows: ok = pr.format_rows || (outline_toggle_ && pr.ui_only && pr.enable_outlining); break;
    case Allow::InsertColumns: ok = pr.insert_columns; break;
    case Allow::InsertRows: ok = pr.insert_rows; break;
    case Allow::InsertHyperlinks: ok = pr.insert_hyperlinks; break;
    case Allow::DeleteColumns: ok = pr.delete_columns; break;
    case Allow::DeleteRows: ok = pr.delete_rows; break;
    case Allow::Sort: ok = pr.sort; break;
    case Allow::AutoFilter: ok = pr.autofilter || (pr.ui_only && pr.enable_autofilter); break;
    case Allow::Objects: ok = pr.objects; break;
    case Allow::Pivot: ok = pr.pivot || (pr.ui_only && pr.enable_pivot); break;
    case Allow::Scenarios: ok = pr.scenarios; break;
    case Allow::Never: ok = false; break;
  }
  int need = -1;
  if (ok && cells && IsLockedRange(sheet, *cells, &need)) ok = false;
  if (ok) return false;
  CountBlock(BlockReason::Protection);
  block_sheet_ = sheet;
  block_cells_ = cells ? *cells : Rect{0, 0, 0, 0};
  block_range_ = need;
  block_python_ = python_call;
  block_pivot_ = false;
  block_table_ = false;
  return true;
}

bool Document::RefuseReadOnly(uint32_t sheet) {
  if (!((ui_read_only && !python_call && !app_refresh) || protected_view)) return false;
  CountBlock(BlockReason::ReadOnly);
  block_sheet_ = sheet;
  block_cells_ = Rect{0, 0, 0, 0};
  block_range_ = -1;
  block_python_ = python_call;
  block_pivot_ = block_table_ = block_param_ = block_off_sheet_ = block_merge_size_ = block_table_shift_ = false;
  block_read_only_ = true;
  return true;
}

bool Document::RefuseStructure() {
  // read-only mode refuses the sheets' structure too (adding, deleting, renaming, moving, hiding, tab colors)
  if (RefuseReadOnly()) return true;
  if (!wb_.protect_structure) return false;
  CountBlock(BlockReason::Structure);
  block_sheet_ = 0;
  block_cells_ = Rect{0, 0, 0, 0};
  block_range_ = -1;
  block_python_ = python_call;
  // what an earlier refusal noted must not pick the message (a read-only refusal before it showed the read-only one)
  block_pivot_ = block_table_ = block_param_ = block_off_sheet_ = block_merge_size_ = block_read_only_ = block_table_shift_ = false;
  return true;
}

}  // namespace yc::engine
