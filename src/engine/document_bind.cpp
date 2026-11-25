// SPDX-License-Identifier: Apache-2.0
// document_bind.cpp - [YC] bound tables: tables fed by Python by key (model/bind.h, engine/bind.h, ADR 0007)
#include "core/i18n.h"
#include "engine/document.h"
#include "model/table.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace yc::engine {


using model::BoundTable;
using model::Table;

namespace {

bool IntKey(const Value& v, int64_t* k) {
  if (!v.IsNumeric()) return false;
  const double d = v.AsDouble();
  if (d != std::floor(d) || std::fabs(d) > 9.0e15) return false;
  *k = static_cast<int64_t>(d);
  return true;
}

bool SameVal(const Value& a, const Value& b) {
  if (a.IsEmpty() || b.IsEmpty()) return a.IsEmpty() && b.IsEmpty();
  if (a.IsNumeric() && b.IsNumeric()) return a.AsDouble() == b.AsDouble();
  if (a.IsString() && b.IsString()) return a.str() == b.str();
  if (a.IsBool() && b.IsBool()) return a.boolean() == b.boolean();
  return false;
}

// a key cell still holds this key (the user may have sorted or edited the table)
bool SameKey(const Value& cell, const Value& key) {
  if (cell.IsNumeric() && key.IsNumeric()) return cell.AsDouble() == key.AsDouble();
  if (cell.IsString() && key.IsString()) return cell.str() == key.str();
  return BindKeyText(cell) == BindKeyText(key) && !BindKeyText(key).empty();
}

}  // namespace

const uint32_t* Document::BindRt::Find(const Value& key) const {
  int64_t k = 0;
  if (IntKey(key, &k)) {
    auto it = int_row_of.find(k);
    return it == int_row_of.end() ? nullptr : &it->second;
  }
  const std::string t = BindKeyText(key);
  if (t.empty()) return nullptr;
  auto it = row_of.find(t);
  return it == row_of.end() ? nullptr : &it->second;
}

void Document::BindRt::Put(const Value& key, uint32_t row) {
  int64_t k = 0;
  if (IntKey(key, &k)) {
    int_row_of[k] = row;
  } else if (std::string t = BindKeyText(key); !t.empty()) {
    row_of[t] = row;
  }
}

const BoundTable* Document::FindBoundTable(std::string_view name) const {
  for (const auto& b : *wb_.bound_tables) {
    if (Lower(b.name) == Lower(name)) return &b;
  }
  return nullptr;
}

std::shared_ptr<const BindTimes> Document::BoundRowTimes(const std::string& name) const {
  auto it = bind_rt_.find(Lower(name));
  return it == bind_rt_.end() ? nullptr : it->second.times;
}

void Document::BindApplyFormats(const BoundTable& def, uint32_t sheet, const Rect& rows) {
  uint32_t sid = 0;
  const Table* t = model::FindTable(wb_, def.name, &sid);
  if (!t || sid != sheet || rows.r0 > rows.r1) return;
  for (const auto& c : def.columns) {
    if (c.number_format.empty()) continue;
    const int i = t->ColumnIndex(c.name);
    if (i < 0) continue;
    const uint32_t col = t->range.c0 + static_cast<uint32_t>(i);
    const uint32_t fmt = wb_.styles.mut().InternString(c.number_format);
    ModifyStyle(sheet, Rect{rows.r0, col, rows.r1, col}, [fmt](model::Style& st) { st.numfmt = fmt; });
  }
}

bool Document::CreateBoundTable(BoundTable def, uint32_t r0, uint32_t c0, const std::vector<std::string>& columns, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (def.name.empty()) def.name = NextTableName();
  if (FindBoundTable(def.name)) return fail(TR("A bound table with this name already exists: ") + def.name);
  if (!ValidTableName(def.name, 0, error)) return false;
  if (!S(def.sheet)) return fail(TR("The sheet doesn't exist"));
  // the columns in the given order; the key column first when it isn't among them
  std::vector<std::string> names;
  const bool key_listed = std::any_of(columns.begin(), columns.end(), [&](const std::string& c) { return Lower(c) == Lower(def.key); });
  if (!def.key.empty() && !key_listed) names.push_back(def.key);
  for (const auto& c : columns) {
    if (std::none_of(names.begin(), names.end(), [&](const std::string& n) { return Lower(n) == Lower(c); })) names.push_back(c);
  }
  if (names.empty()) return fail(TR("A bound table needs at least one column"));
  if (c0 + names.size() > core::kMaxCols) return fail(TR("The table doesn't fit on the sheet"));
  const Rect range{r0, c0, r0 + 1, c0 + static_cast<uint32_t>(names.size()) - 1};
  for (const Table& o : *S(def.sheet)->tables) {
    if (o.range.Intersects(range))
      return fail(TR("A table can't overlap another table.") + std::string(" (") + o.name + ": " + core::CellA1(o.range.r0, o.range.c0) + ":" +
                  core::CellA1(o.range.r1, o.range.c1) + ")");
  }
  if (Refuse(def.sheet, &range)) return fail(TR("The sheet is protected"));
  ClearContents(def.sheet, range, kSrcPython);
  for (size_t i = 0; i < names.size(); ++i) SetValue(def.sheet, r0, c0 + static_cast<uint32_t>(i), Value::String(names[i]), kSrcLive);
  const uint32_t id = AddTable(def.sheet, range, true, "TableStyleMedium2", error);
  if (!id) return false;
  if (!RenameTable(def.sheet, id, def.name, error)) {
    DeleteTable(def.sheet, id, false);
    return false;
  }
  const std::string name = def.name;
  wb_.bound_tables.mut().push_back(std::move(def));
  BindRt& rt = bind_rt_[Lower(name)];
  rt = BindRt{};
  rt.times = std::make_shared<BindTimes>(1);
  wb_.dirty = true;
  return true;
}

bool Document::UpdateBoundTable(const std::string& name, const std::function<void(BoundTable&)>& fn, std::string* error) {
  auto& v = wb_.bound_tables.mut();
  for (auto& b : v) {
    if (Lower(b.name) != Lower(name)) continue;
    const std::string keep = b.name;
    const uint32_t sheet = b.sheet;
    fn(b);
    b.name = keep;
    b.sheet = sheet;
    uint32_t sid = 0;
    if (const Table* t = model::FindTable(wb_, keep, &sid); t && t->HasData()) BindApplyFormats(b, sid, t->Data());
    wb_.dirty = true;
    return true;
  }
  if (error) *error = TR("No bound table called ") + name;
  return false;
}

bool Document::RemoveBoundTable(const std::string& name, bool delete_table) {
  auto& v = wb_.bound_tables.mut();
  auto it = std::find_if(v.begin(), v.end(), [&](const BoundTable& b) { return Lower(b.name) == Lower(name); });
  if (it == v.end()) return false;
  const std::string tname = it->name;
  v.erase(it);
  bind_rt_.erase(Lower(tname));
  if (delete_table) {
    uint32_t sid = 0;
    if (const Table* t = model::FindTable(wb_, tname, &sid)) {
      const Rect r = t->range;
      const uint32_t id = t->id;
      DeleteTable(sid, id, false);
      ClearAll(sid, r, kSrcPython, false, /*to_lines=*/true);
    }
  }
  wb_.dirty = true;
  return true;
}

void Document::BindRebuildMap(BindRt& rt, uint32_t sheet, const Table& t, int key_col) {
  rt.row_of.clear();
  rt.int_row_of.clear();
  const Rect d = t.Data();
  rt.rows = t.HasData() ? static_cast<uint32_t>(d.Rows()) : 0;
  const model::Sheet* s = S(sheet);
  if (key_col >= 0 && s) {
    rt.int_row_of.reserve(rt.rows);
    for (uint32_t i = 0; i < rt.rows; ++i) rt.Put(s->GetValue(d.r0 + i, d.c0 + static_cast<uint32_t>(key_col)), i);
  }
  // values held by a refresh cap follow their keys to the rows they are on now; when two land on the same cell (a
  // newer value was held after the rows moved), the newer one stays
  if (key_col >= 0 && !rt.pending.empty()) {
    std::map<uint64_t, BindRt::Held> moved;
    for (auto& [pk, h] : rt.pending) {
      uint32_t row = static_cast<uint32_t>(pk >> 20);
      if (!h.key.IsEmpty()) {
        const uint32_t* r2 = rt.Find(h.key);
        if (!r2) continue;  // the key is gone
        row = *r2;
      }
      const uint64_t npk = (uint64_t{row} << 20) | (pk & 0xFFFFF);
      auto it = moved.find(npk);
      if (it == moved.end()) moved.emplace(npk, std::move(h));
      else if (h.seq > it->second.seq) it->second = std::move(h);
    }
    rt.pending.swap(moved);
  }
  if (!rt.times || rt.times->t.size() < rt.rows) {
    if (const BoundTable* def = FindBoundTable(t.name)) {
      rt.spec.clear();  // rebuilt with the flash / refresh-cap columns below
      BindSyncTimes(rt, *def, t, rt.rows);
    }
  }
}

// the row times grow with the table; the flash / refresh-cap columns follow the definition
void Document::BindSyncTimes(BindRt& rt, const BoundTable& def, const Table& t, size_t rows) {
  std::vector<model::BoundColumn> spec;
  for (const auto& c : def.columns) {
    if (c.flash || c.max_hz) spec.push_back(c);
  }
  if (rt.times && rt.times->t.size() >= rows && spec == rt.spec) {
    // columns may have moved: keep the indexes current
    for (auto& c : rt.times->cols) {
      if (const int i = t.ColumnIndex(c->name); i >= 0) c->col = static_cast<uint32_t>(i);
    }
    return;
  }
  const size_t cap = std::max<size_t>(rows + rows / 2, 16);
  auto nt = std::make_shared<BindTimes>(std::max(cap, rt.times ? rt.times->t.size() : size_t{0}));
  if (rt.times) {
    for (size_t i = 0; i < rt.times->t.size(); ++i) nt->t[i].store(rt.times->t[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
  for (const auto& sc : spec) {
    const int i = t.ColumnIndex(sc.name);
    if (i < 0) continue;
    auto col = std::make_unique<BindTimes::Col>(nt->t.size());
    col->col = static_cast<uint32_t>(i);
    col->name = sc.name;
    col->flash = sc.flash;
    col->flash_ms = sc.flash_ms;
    col->max_hz = sc.max_hz;
    col->up = sc.up_color;
    col->down = sc.down_color;
    if (rt.times) {
      if (const BindTimes::Col* old = rt.times->ColAt(col->col)) {
        for (size_t r = 0; r < old->t.size() && r < col->t.size(); ++r) {
          col->t[r].store(old->t[r].load(std::memory_order_relaxed), std::memory_order_relaxed);
          col->dir[r].store(old->dir[r].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
      }
    }
    nt->cols.push_back(std::move(col));
  }
  rt.spec = std::move(spec);
  rt.times = std::move(nt);
}

int64_t Document::BindNextFlush() const {
  int64_t next = 0;
  for (const auto& [k, rt] : bind_rt_) {
    if (!rt.pending.empty() && (next == 0 || rt.next_flush < next)) next = rt.next_flush;
  }
  return next;
}

void Document::BindFlush(int64_t now) {
  for (auto& [k, rt] : bind_rt_) {
    if (rt.pending.empty() || rt.next_flush > now || !rt.times) continue;
    BindFlushTable(k, rt, now, false);
  }
}

// writes the held values whose turn has come (all of them with force, before rows are deleted)
void Document::BindFlushTable(const std::string& name, BindRt& rt, int64_t now, bool force) {
  if (rt.pending.empty() || !rt.times) return;
  const BoundTable* def = FindBoundTable(name);
  uint32_t sid = 0;
  const Table* t = def ? model::FindTable(wb_, def->name, &sid) : nullptr;
  if (!t || !t->HasData()) {
    rt.pending.clear();
    return;
  }
  const Rect d = t->Data();
  const int key_col = rt.key_col >= 0 && static_cast<size_t>(rt.key_col) < t->columns.size() ? rt.key_col : -1;
  // the rows may have moved since the values were held (the user sorted or deleted rows): the rebuild moves them
  if (key_col >= 0) {
    for (const auto& [pk, h] : rt.pending) {
      const uint32_t row = static_cast<uint32_t>(pk >> 20);
      if (!h.key.IsEmpty() &&
          (row >= static_cast<uint32_t>(d.Rows()) || !SameKey(S(sid)->GetValue(d.r0 + row, d.c0 + static_cast<uint32_t>(key_col)), h.key))) {
        BindRebuildMap(rt, sid, *t, key_col);
        break;
      }
    }
  }
  rt.next_flush = 0;
  for (auto it = rt.pending.begin(); it != rt.pending.end();) {
    const uint32_t row = static_cast<uint32_t>(it->first >> 20);
    const uint32_t col = static_cast<uint32_t>(it->first & 0xFFFFF);
    const BindRt::Held& held = it->second;
    BindTimes::Col* cs = nullptr;
    for (auto& c : rt.times->cols) {
      if (c->col == col) cs = c.get();
    }
    const int64_t period = cs && cs->max_hz > 0 ? 1000000000LL / cs->max_hz : 0;
    const int64_t last = cs && row < cs->t.size() ? cs->t[row].load(std::memory_order_relaxed) : 0;
    if (!force && last + period > now) {
      rt.next_flush = rt.next_flush == 0 ? last + period : std::min(rt.next_flush, last + period);
      ++it;
      continue;
    }
    if (row < static_cast<uint32_t>(d.Rows()) && col < t->columns.size()) {
      const uint32_t r = d.r0 + row, c = t->range.c0 + col;
      const Value old = S(sid)->GetValue(r, c);
      if (cs && row < cs->t.size() && !SameVal(old, held.v)) {
        cs->t[row].store(now, std::memory_order_relaxed);
        cs->dir[row].store(old.IsNumeric() && held.v.IsNumeric() ? (held.v.AsDouble() > old.AsDouble() ? 1 : held.v.AsDouble() < old.AsDouble() ? -1 : 0) : 0,
                           std::memory_order_relaxed);
      }
      M(sid)->SetValue(r, c, held.v);
      MarkChanged(sid, Rect::Cell(r, c), kSrcLive);
    }
    it = rt.pending.erase(it);
  }
}

// adds rows at the end of the table body; cells below move down when they aren't empty
bool Document::BindGrow(uint32_t sheet, uint32_t table_id, uint32_t rows, std::string* error) {
  const Table* t = TableById(sheet, table_id);
  if (!t || rows == 0) return t != nullptr;
  if (t->range.r1 + rows >= S(sheet)->row_limit) EnsureRowLimit(sheet, t->range.r1 + rows);
  const Rect band{t->range.r1 + 1, t->range.c0, t->range.r1 + rows, t->range.c1};
  bool empty = true;
  const model::Sheet* s = S(sheet);
  for (uint32_t r = band.r0; r <= band.r1 && empty; ++r) {
    for (uint32_t c = band.c0; c <= band.c1 && empty; ++c) empty = s->GetValue(r, c).IsEmpty() && !s->GetFormula(r, c);
  }
  // the first data row of a new table is the blank row AddTable made: fill it before adding more
  if (!empty && !InsertCells(sheet, band, true)) return false;  // refused: the table would take the cells in the way
  t = TableById(sheet, table_id);
  if (!t) return false;
  const Rect old_body = t->Data();
  Rect nr = t->range;
  nr.r1 += rows;
  if (!ResizeTable(sheet, table_id, nr, error)) return false;
  // conditional formats over the table's last row grow with it (like Excel's tables)
  if (const model::Sheet* cs = S(sheet); cs && !cs->cond_formats->empty() && old_body.r0 <= old_body.r1) {
    bool any = false;
    for (const auto& rule : *cs->cond_formats) {
      for (const auto& r : rule.ranges) any = any || (r.r1 == old_body.r1 && r.r0 >= nr.r0 && r.c0 >= nr.c0 && r.c1 <= nr.c1);
    }
    if (any) {
      for (auto& rule : M(sheet)->cond_formats.mut()) {
        for (auto& r : rule.ranges) {
          if (r.r1 == old_body.r1 && r.r0 >= nr.r0 && r.c0 >= nr.c0 && r.c1 <= nr.c1) r.r1 += rows;
        }
      }
    }
  }
  return true;
}

bool Document::TableFrame(std::string_view name, bool skip_formulas, ColumnFrame* out, std::string* error) const {
  uint32_t sid = 0;
  const Table* t = model::FindTable(wb_, name, &sid);
  if (!t) {
    if (error) *error = TR("No table called ") + std::string(name);
    return false;
  }
  const model::Sheet* s = S(sid);
  const BoundTable* def = FindBoundTable(t->name);
  *out = ColumnFrame{};
  out->op = FrameOp::Replace;
  out->table = def ? def->name : t->name;
  const Rect d = t->Data();
  uint32_t rows = t->HasData() ? static_cast<uint32_t>(d.Rows()) : 0;
  if (rows == 1) {  // a fresh table's one blank row counts as no rows
    bool blank = true;
    for (uint32_t c = d.c0; c <= d.c1 && blank; ++c) blank = s->GetValue(d.r0, c).IsEmpty() && !s->GetFormula(d.r0, c);
    if (blank) rows = 0;
  }
  out->nrows = rows;
  for (uint32_t c = t->range.c0; c <= t->range.c1; ++c) {
    const size_t i = c - t->range.c0;
    const std::string col = i < t->columns.size() ? t->columns[i].name : std::string();
    if (skip_formulas && rows && s->GetFormula(d.r0, c)) continue;
    if (def && Lower(col) == Lower(def->key)) out->key_col = static_cast<int>(out->names.size());
    out->names.push_back(col);
    std::vector<Value> v(rows);
    for (uint32_t r = 0; r < rows; ++r) v[r] = s->GetValue(d.r0 + r, c);
    out->cols.push_back(std::move(v));
  }
  return true;
}

bool Document::ApplyColumnFrame(const ColumnFrame& f, std::string* error, BindResult* result) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  const BoundTable* def0 = FindBoundTable(f.table);
  if (!def0) return fail(TR("No bound table called ") + f.table);
  const BoundTable def = *def0;
  uint32_t sid = 0;
  const Table* t = model::FindTable(wb_, def.name, &sid);
  if (!t) return fail(TR("The bound table's table was deleted: ") + def.name);
  if (Refuse(sid, &t->range)) return fail(TR("The sheet is protected"));
  const uint32_t tid = t->id;
  BindRt& rt = bind_rt_[Lower(def.name)];
  bool structural = false;
  // columns: unknown ones are added at the right of the table
  std::vector<int> col_of(f.names.size(), -1);
  for (size_t i = 0; i < f.names.size(); ++i) {
    int idx = t->ColumnIndex(f.names[i]);
    if (idx < 0) {
      // a new column at the right: cells there move right only when they aren't empty
      const Rect band{t->range.r0, t->range.c1 + 1, t->range.r1, t->range.c1 + 1};
      bool empty = band.c0 < core::kMaxCols;
      for (uint32_t r = band.r0; r <= band.r1 && empty; ++r) empty = S(sid)->GetValue(r, band.c0).IsEmpty() && !S(sid)->GetFormula(r, band.c0);
      if (empty) {
        Rect nr = t->range;
        nr.c1 += 1;
        if (!ResizeTable(sid, tid, nr, error)) return false;
      } else if (!TableInsertColumn(sid, tid, -1)) {
        return fail(TR("The column couldn't be added: ") + f.names[i]);
      }
      t = TableById(sid, tid);
      const uint32_t c = t->range.c1;
      SetValue(sid, t->range.r0, c, Value::String(f.names[i]), kSrcLive);
      SyncTableHeaders(sid);
      t = TableById(sid, tid);
      idx = t->ColumnIndex(f.names[i]);
      structural = true;
    }
    col_of[i] = idx;
  }
  const int key_tcol = def.key.empty() ? -1 : t->ColumnIndex(def.key);
  if ((f.op == FrameOp::Upsert || f.op == FrameOp::Delete) && (f.key_col < 0 || key_tcol < 0))
    return fail(TR("Updates by key need the key column: ") + def.key);
  // a fresh table has one blank row (AddTable needs it): it counts as no rows
  auto body_rows = [&](const Table* tt) -> uint32_t {
    if (!tt->HasData()) return 0;
    const Rect d = tt->Data();
    if (static_cast<uint32_t>(d.Rows()) == 1) {
      const model::Sheet* s = S(sid);
      bool blank = true;
      for (uint32_t c = d.c0; c <= d.c1 && blank; ++c) blank = s->GetValue(d.r0, c).IsEmpty() && !s->GetFormula(d.r0, c);
      if (blank) return 0;
    }
    return static_cast<uint32_t>(d.Rows());
  };
  uint32_t rows = body_rows(t);
  rt.key_col = key_tcol;
  if (rt.rows != rows || !rt.times) BindRebuildMap(rt, sid, *t, key_tcol);
  rt.rows = rows;
  BindSyncTimes(rt, def, *t, std::max<size_t>(rows, 1) + f.nrows);
  const int64_t now = PerfCounterNs();
  BindResult res;
  std::vector<uint32_t> target(f.nrows, UINT32_MAX);  // body row of each frame row
  if (f.op == FrameOp::Delete) {
    // values held by a refresh cap go to their rows first (rows below the deleted ones move up)
    if (!rt.pending.empty()) {
      BindFlushTable(Lower(def.name), rt, now, true);
      t = TableById(sid, tid);
    }
    std::vector<uint32_t> del;
    for (uint32_t i = 0; i < f.nrows; ++i) {
      if (const uint32_t* row = rt.Find(f.cols[static_cast<size_t>(f.key_col)][i])) del.push_back(*row);
    }
    std::sort(del.begin(), del.end());
    del.erase(std::unique(del.begin(), del.end()), del.end());
    for (auto it = del.rbegin(); it != del.rend(); ++it) {
      const Table* tt = TableById(sid, tid);
      if (!tt) break;
      if (tt->Data().Rows() <= 1) {
        ClearContents(sid, Rect{tt->Data().r0, tt->range.c0, tt->Data().r0, tt->range.c1}, kSrcLive);
      } else {
        TableDeleteRow(sid, tid, *it);
      }
      ++res.removed;
    }
    structural = !del.empty();
    t = TableById(sid, tid);
    if (t) BindRebuildMap(rt, sid, *t, key_tcol);
    rt.rows = t ? body_rows(t) : 0;
  } else {
    if (f.op == FrameOp::Replace) {
      // the rows become exactly these (values held by a refresh cap are dropped with the old rows)
      rt.pending.clear();
      const Rect d = t->Data();
      // the data columns are cleared; calculated columns (formulas) stay, like Excel's table columns
      if (rows > 0) {
        for (uint32_t c = t->range.c0; c <= t->range.c1; ++c) {
          if (!S(sid)->GetFormula(d.r0, c)) ClearContents(sid, Rect{d.r0, c, d.r0 + rows - 1, c}, kSrcLive);
        }
      }
      const uint32_t have = t->HasData() ? static_cast<uint32_t>(d.Rows()) : 0;
      if (f.nrows > have) {
        if (!BindGrow(sid, tid, f.nrows - have, error)) return false;
        structural = true;
      } else if (f.nrows < have && f.nrows > 0) {
        Rect nr = t->range;
        nr.r1 = d.r0 + f.nrows - 1;
        ResizeTable(sid, tid, nr, error);
        structural = true;
      }
      for (uint32_t i = 0; i < f.nrows; ++i) target[i] = i;
      res.added = f.nrows;
      rows = f.nrows;
    } else {
      // upsert / append: existing keys keep their rows, new ones go to the end
      std::vector<uint32_t> fresh;
      BindRt new_keys;  // keys first seen in this frame -> their new rows
      const Rect d0 = TableById(sid, tid)->Data();
      const model::Sheet* s0 = S(sid);
      bool rebuilt = false;
      for (uint32_t i = 0; i < f.nrows; ++i) {
        if (f.op == FrameOp::Append || f.key_col < 0) {
          fresh.push_back(i);
          continue;
        }
        const Value& key = f.cols[static_cast<size_t>(f.key_col)][i];
        if (key.IsEmpty()) continue;
        const uint32_t* row = rt.Find(key);
        if (row && !rebuilt && !SameKey(s0->GetValue(d0.r0 + *row, d0.c0 + static_cast<uint32_t>(key_tcol)), key)) {
          // the map is stale (the user sorted or edited the table): read the key column again
          BindRebuildMap(rt, sid, *TableById(sid, tid), key_tcol);
          rebuilt = true;
          row = rt.Find(key);
        }
        if (row) {
          target[i] = *row;
        } else if (const uint32_t* nk = new_keys.Find(key)) {
          target[i] = *nk;  // the same new key twice in one frame: the last value wins
        } else {
          new_keys.Put(key, rows + static_cast<uint32_t>(fresh.size()));
          fresh.push_back(i);
        }
      }
      if (!fresh.empty()) {
        const Table* tt = TableById(sid, tid);
        const uint32_t have = tt->HasData() ? static_cast<uint32_t>(tt->Data().Rows()) : 0;
        const uint32_t need = rows + static_cast<uint32_t>(fresh.size());
        if (need > have) {
          if (!BindGrow(sid, tid, need - have, error)) return false;
          structural = true;
        }
        for (size_t j = 0; j < fresh.size(); ++j) {
          target[fresh[j]] = rows + static_cast<uint32_t>(j);
          if (f.key_col >= 0) rt.Put(f.cols[static_cast<size_t>(f.key_col)][fresh[j]], rows + static_cast<uint32_t>(j));
        }
        res.added = static_cast<uint32_t>(fresh.size());
        rows = need;
      }
    }
    // write the cells, column by column
    t = TableById(sid, tid);
    const Rect d = t->Data();
    model::Sheet* s = M(sid);
    uint32_t lo = UINT32_MAX, hi = 0, cmin = UINT32_MAX, cmax = 0;
    // frame rows in sheet row order, so each tile is looked up (and copied once after a publish) only once per column
    std::vector<uint32_t> order;
    order.reserve(f.nrows);
    for (uint32_t i = 0; i < f.nrows; ++i) {
      if (target[i] != UINT32_MAX) order.push_back(i);
    }
    bool sorted = true;
    for (size_t j = 1; j < order.size() && sorted; ++j) sorted = target[order[j - 1]] <= target[order[j]];
    if (!sorted) std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return target[a] < target[b]; });
    BindSyncTimes(rt, def, *t, rows);
    for (size_t c = 0; c < f.cols.size(); ++c) {
      if (col_of[c] < 0) continue;
      const uint32_t col = t->range.c0 + static_cast<uint32_t>(col_of[c]);
      cmin = std::min(cmin, col);
      cmax = std::max(cmax, col);
      const auto& vals = f.cols[c];
      // change flash / refresh cap of this column
      BindTimes::Col* cs = nullptr;
      for (auto& x : rt.times->cols) {
        if (x->col == static_cast<uint32_t>(col_of[c])) cs = x.get();
      }
      const int64_t period = cs && cs->max_hz > 0 ? 1000000000LL / cs->max_hz : 0;
      model::ValueTile* vt = nullptr;
      const model::MetaTile* mt = nullptr;
      uint32_t tile_row = UINT32_MAX;
      for (uint32_t i : order) {
        const uint32_t r = d.r0 + target[i];
        if (r / model::kTileRows != tile_row) {
          tile_row = r / model::kTileRows;
          mt = s->meta.GetCell(r, col);
          vt = nullptr;
        }
        const uint32_t idx = model::TileIndex(r, col);
        if (mt && mt->formula_count && mt->formula[idx]) {
          UnregisterFormulaCell(sid, r, col);
          s->SetFormula(r, col, 0);
          mt = s->meta.GetCell(r, col);
        }
        if (cs && target[i] < cs->t.size()) {
          const uint32_t row = target[i];
          const uint64_t pk = (uint64_t{row} << 20) | static_cast<uint32_t>(col_of[c]);
          const int64_t last = cs->t[row].load(std::memory_order_relaxed);
          // (a replace isn't held back: its rows were just cleared)
          if (period && last && now - last < period && f.op != FrameOp::Replace) {
            // capped: the newest value waits for its turn (BindFlush)
            rt.pending[pk] = BindRt::Held{f.key_col >= 0 ? f.cols[static_cast<size_t>(f.key_col)][i] : Value(), vals[i], ++rt.held_seq};
            rt.next_flush = rt.next_flush == 0 ? last + period : std::min(rt.next_flush, last + period);
            continue;
          }
          if (!rt.pending.empty()) rt.pending.erase(pk);
          const Value& old = vt ? vt->v[idx] : s->GetValue(r, col);
          if (!SameVal(old, vals[i])) {
            cs->t[row].store(now, std::memory_order_relaxed);
            const int8_t dir = old.IsNumeric() && vals[i].IsNumeric() ? (vals[i].AsDouble() > old.AsDouble() ? 1 : vals[i].AsDouble() < old.AsDouble() ? -1 : 0) : 0;
            cs->dir[row].store(dir, std::memory_order_relaxed);
          }
        }
        if (vals[i].IsEmpty() && !vt && !s->values.GetCell(r, col)) continue;
        if (!vt) vt = &s->values.MutableCell(r, col);
        vt->Set(idx, vals[i]);
        ++res.cells;
      }
      if (!order.empty()) {
        s->Touch(d.r0 + target[order.front()], col);
        s->Touch(d.r0 + target[order.back()], col);
      }
    }
    for (uint32_t i = 0; i < f.nrows; ++i) {
      if (target[i] == UINT32_MAX) continue;
      lo = std::min(lo, target[i]);
      hi = std::max(hi, target[i]);
    }
    if (lo <= hi && cmin <= cmax) {
      // one changed region per run of touched rows (frames are usually in row order)
      std::vector<uint32_t> rs;
      rs.reserve(f.nrows);
      for (uint32_t i = 0; i < f.nrows; ++i) {
        if (target[i] != UINT32_MAX) rs.push_back(target[i]);
      }
      std::sort(rs.begin(), rs.end());
      rs.erase(std::unique(rs.begin(), rs.end()), rs.end());
      size_t a = 0;
      while (a < rs.size()) {
        size_t b = a;
        while (b + 1 < rs.size() && rs[b + 1] <= rs[b] + 32) ++b;  // close runs merge: fewer, larger regions
        MarkChanged(sid, Rect{d.r0 + rs[a], cmin, d.r0 + rs[b], cmax}, kSrcLive);
        a = b + 1;
      }
    }
    if (res.added) BindApplyFormats(def, sid, Rect{d.r0 + rows - res.added, t->range.c0, d.r0 + rows - 1, t->range.c1});
    // update times
    if (!rt.times || rt.times->t.size() < rows) BindSyncTimes(rt, def, *t, rows);
    for (uint32_t i = 0; i < f.nrows; ++i) {
      if (target[i] != UINT32_MAX && target[i] < rt.times->t.size()) rt.times->t[target[i]].store(now, std::memory_order_relaxed);
    }
    if (f.op == FrameOp::Replace) BindRebuildMap(rt, sid, *t, key_tcol);
    rt.rows = rows;
  }
  if (f.sent_ns > 0) {
    input_newest_ns_ = std::max(input_newest_ns_, f.sent_ns);
    input_oldest_ns_ = input_oldest_ns_ == 0 ? f.sent_ns : std::min(input_oldest_ns_, f.sent_ns);
  }
  bind_cells_written_ += res.cells;
  if (structural) ClearUndo();
  res.rows = rt.rows;
  if (result) *result = res;
  wb_.dirty = true;
  return true;
}

}  // namespace yc::engine
