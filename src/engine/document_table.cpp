// SPDX-License-Identifier: Apache-2.0
// document_table.cpp - tables (ListObjects): create, resize, rename, convert to range, totals row, auto-expansion,
// calculated columns, header names and the formulas that refer to them
#include "engine/document.h"
#include "core/i18n.h"

#include "engine/tile_iter.h"
#include "model/ref_texts.h"

#include <algorithm>
#include <unordered_set>

namespace yc::engine {

using model::Sheet;
using model::Table;

namespace {

std::string UpperAscii(std::string_view a) {  // IEq's key
  std::string u(a);
  for (char& x : u) {
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
  }
  return u;
}

bool IEq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
    if (x != y) return false;
  }
  return true;
}

// visit every structured reference of a formula
void ForEachStructRef(formula::Node& n, const std::function<void(formula::Node&)>& fn) {
  if (n.kind == formula::NK::StructRef) fn(n);
  for (auto& k : n.kids) ForEachStructRef(k, fn);
}
void ForEachStructRef(const formula::Node& n, const std::function<void(const formula::Node&)>& fn) {
  if (n.kind == formula::NK::StructRef) fn(n);
  for (const auto& k : n.kids) ForEachStructRef(k, fn);
}

// #REF!, what a reference to a deleted table or column becomes (=SUM(#REF!), as Excel writes it)
formula::Node RefError() {
  formula::Node n;
  n.kind = formula::NK::Err;
  n.err = core::Err::Ref;
  return n;
}

// the formula every data cell of a table column holds (a calculated column; skip: a row left out), or 0
uint32_t CalculatedFormula(const Sheet& s, uint32_t c, uint32_t r0, uint32_t r1, uint32_t skip = UINT32_MAX) {
  uint32_t id = 0;
  for (uint32_t r = r0; r <= r1; ++r) {
    if (r == skip) continue;
    const uint32_t f = s.GetFormula(r, c);
    if (!f || (id && f != id)) return 0;
    id = f;
  }
  return id;
}

// a formula that may name a table: a structured reference, or a name alone (=ROWS(Table1))
bool MayNameTable(const formula::CompiledFormula* f, bool bare_names) {
  return f && (f->key.find("T'") != std::string::npos || (bare_names && f->key.find("N'") != std::string::npos));
}

}  // namespace

const Table* Document::TableAt(uint32_t sheet, uint32_t r, uint32_t c) const { return model::TableAt(wb_, sheet, r, c); }

const Table* Document::TableById(uint32_t sheet, uint32_t id) const {
  const Sheet* s = S(sheet);
  if (!s) return nullptr;
  for (const Table& t : *s->tables) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

Table* Document::MutTable(uint32_t sheet, uint32_t id) {
  Sheet* s = M(sheet);
  if (!s) return nullptr;
  for (Table& t : s->tables.mut()) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

int Document::ResolveStructRef(uint32_t sheet, const formula::Node& n, core::CellPos origin, uint32_t* out_sheet, Rect* out) const {
  return model::ResolveTableRef(wb_, sheet, n.str, static_cast<uint8_t>(n.op), n.kids.size() > 0 ? n.kids[0].str : std::string(),
                                n.kids.size() > 1 ? n.kids[1].str : std::string(), origin.row, origin.col, out_sheet, out);
}

void Document::AddTableDeps(uint64_t key, uint32_t sheet, core::CellPos origin, const formula::Node& root) {
  ForEachStructRef(root, [&](const formula::Node& n) {
    uint32_t ts = sheet;
    Rect r;
    if (ResolveStructRef(sheet, n, origin, &ts, &r) == 0) deps_.Add(key, ts, r);
  });
}

bool Document::ValidTableName(std::string_view name, uint32_t except_id, std::string* error) const {
  std::string err;
  if (!ValidateName(name, &err)) {
    if (error) *error = err;
    return false;
  }
  for (const auto& sc : wb_.sheets) {
    for (const Table& t : *sc->tables) {
      if (t.id != except_id && IEq(t.name, name)) {
        if (error) *error = TR("A table with that name already exists.");
        return false;
      }
    }
  }
  for (const auto& n : *wb_.names) {
    if (IEq(n.name, name)) {
      if (error) *error = TR("A defined name with that name already exists.");
      return false;
    }
  }
  return true;
}

std::string Document::TableCantHold(uint32_t sheet, const Rect& rect, uint32_t table_id) const {
  const Sheet* s = S(sheet);
  if (!s) return {};
  for (const Table& o : *s->tables) {
    if (o.id != table_id && o.range.Intersects(rect)) return TR("A table can't overlap another table.");
  }
  for (const Rect& m : *s->merges) {
    if (m.Intersects(rect)) return TR("A table can't contain merged cells.");
  }
  // nor any of an array entered over several cells with Ctrl+Shift+Enter (Excel 16, measured: refused with this
  // message, part or all of it; a one-cell array is taken in): its header held the array's value as text, and the
  // array calculated it back to a number on open (doc_fuzz seed 10090004). SetArrayFormula refuses the other way round.
  for (const auto& [k, sp] : *s->spills) {
    if (sp.cse && !sp.rect.IsCell() && sp.rect.Intersects(rect)) return TR("Multi-cell array formulas aren't allowed in tables.");
  }
  return {};
}

std::string Document::NextTableName() const {
  for (int n = 1;; ++n) {
    const std::string name = TR("Table") + std::to_string(n);  // in the UI's language, as Excel's
    if (ValidTableName(name, 0, nullptr)) return name;
  }
}

void Document::SyncTableHeaders(uint32_t sheet) {
  if (syncing_tables_) return;
  Sheet* s = M(sheet);
  if (!s || s->tables->empty()) return;
  syncing_tables_ = true;
  const auto& tbl = *wb_.styles;
  std::vector<std::tuple<std::string, std::string, std::string>> renames;  // table, old column, new column
  std::vector<std::tuple<uint32_t, uint32_t, std::string>> writes;          // header cells to fill in
  bool renamed = false;
  for (Table& t : s->tables.mut()) {
    const uint32_t n = static_cast<uint32_t>(t.range.Cols());
    std::vector<model::TableColumn> cols(n);
    for (uint32_t j = 0; j < n && j < t.columns.size(); ++j) cols[j] = t.columns[j];
    std::vector<std::string> given(n);  // the names the headers hold
    std::vector<bool> not_text(n, false);  // a number, TRUE, an error or a formula: Excel makes the header that text
    for (uint32_t j = 0; j < n; ++j) {
      if (t.header) {
        const uint32_t hr = t.range.r0, hc = t.range.c0 + j;
        const Value& v = s->GetValue(hr, hc);
        const bool has_formula = s->GetFormula(hr, hc) != 0;
        // a formula written into a header becomes its value before it is calculated: =1+1 is "0" (Excel)
        given[j] = has_formula && IsPending(Key(sheet, hr, hc)) ? std::string("0") : DisplayTextOf(v, tbl.NumberFormat(tbl.Get(s->GetStyle(hr, hc))));
        not_text[j] = has_formula || (!v.IsEmpty() && !v.IsString());
      } else {
        given[j] = cols[j].name;
      }
    }
    std::vector<std::string> used;
    // Excel numbers blank headers (ColumnN) and repeated ones (a3) with one counter, each number tried once, skipping
    // the names any header of the table holds: a, a, , A, a2 are a, a3, Column4, A5, a2. The names are kept in sets
    // (a scan of every header for each try was quadratic over a table of 16,384 columns)
    int counter = 1;
    std::unordered_set<std::string> given_set, used_set;
    for (const std::string& g : given) given_set.insert(UpperAscii(g));
    auto taken = [&](const std::string& c) {
      const std::string u = UpperAscii(c);
      return given_set.count(u) != 0 || used_set.count(u) != 0;
    };
    for (uint32_t j = 0; j < n; ++j) {
      std::string name = given[j];
      bool generated = false;
      if (name.empty()) {
        // the first ColumnN no header has (a column added to Name, Qty, Price is Column1)
        do name = TR("Column") + std::to_string(counter++);  // in the UI's language, as Excel's
        while (taken(name));
        generated = true;
      } else if (used_set.count(UpperAscii(name))) {
        // unique within the table
        const std::string base = name;
        counter = std::max(counter, 2);
        do name = base + std::to_string(counter++);
        while (taken(name));
        generated = true;
      }
      used.push_back(name);
      used_set.insert(UpperAscii(name));
      if (t.header && (generated || not_text[j])) writes.emplace_back(t.range.r0, t.range.c0 + j, name);
      if (j < t.columns.size() && !t.columns[j].name.empty() && t.columns[j].name != name) renames.emplace_back(t.name, t.columns[j].name, name);
      cols[j].name = name;
    }
    renamed = renamed || !(t.columns == cols);
    t.columns = std::move(cols);
  }
  for (const auto& [r, c, name] : writes) {
    if (S(sheet)->GetFormula(r, c)) {
      UnregisterFormulaCell(sheet, r, c);
      M(sheet)->SetFormula(r, c, 0);
    }
    M(sheet)->SetValue(r, c, Value::String(name));
    changes_.push_back(CellChange{sheet, Rect::Cell(r, c), kSrcCalc});
    // what reads the header reads the text now: =B1:C5*2 kept 0 from the header's formula until a reopen gave 4
    // from its text "02" (doc_fuzz seed 11051768)
    MarkDependentsOf(sheet, Rect::Cell(r, c));
  }
  // the formats of headers a table grew over, now that they are text (AutoExpandTable)
  if (!grown_header_styles_.empty()) {
    std::vector<std::array<uint32_t, 4>> rest;
    for (const auto& g : grown_header_styles_) {
      if (g[0] != sheet) rest.push_back(g);
      else if (Sheet* ms = M(sheet)) ms->ShowStyle(g[1], g[2], g[3]);
    }
    grown_header_styles_.swap(rest);
  }
  syncing_tables_ = false;
  // the edit that made the header text (a paste, a cut, a fill) undoes this too, with the whole version: the
  // columns' names and the cells written here aren't the cells it noted (doc_fuzz seeds 10280369, 10290940)
  if (renamed || !writes.empty()) NoteTouched();
  RenameTableColumns(renames);
}

void Document::RenameTableColumns(const std::vector<std::tuple<std::string, std::string, std::string>>& renames) {
  if (renames.empty()) return;
  if (renames.size() == 1) {
    const auto& [table, from, to] = renames[0];
    RewriteTableRefs(table, table, from, to);
    return;
  }
  // the renames in order, at each reference, as one walk a rename each did (a column inserted into a table of 16,384
  // columns renamed thousands of headers, each walking every formula and rebuilding the engine's index: minutes)
  std::function<bool(formula::Node&, const Table*)> fix = [&](formula::Node& n, const Table* here) {
    bool changed = false;
    if (n.kind == formula::NK::StructRef) {
      for (const auto& [table, from, to] : renames) {
        const bool mine = n.str.empty() ? (here && IEq(here->name, table)) : IEq(n.str, table);
        if (!mine) continue;
        for (size_t k = 0; k < 2 && k < n.kids.size(); ++k) {
          if (!IEq(n.kids[k].str, from)) continue;
          n.kids[k].str = to;
          changed = true;
        }
      }
      return changed;
    }
    for (auto& k : n.kids) changed = fix(k, here) || changed;
    return changed;
  };
  struct Fix {
    uint32_t sheet, r, c, id;
  };
  std::vector<Fix> fixes;
  for (const auto& sc : wb_.sheets) {
    const Sheet& sh = sc.get();
    sh.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& tile) {
      if (tile.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        const uint32_t id = tile.formula[idx];
        if (!id) continue;
        const formula::CompiledFormula* f = Formula(id);
        if (!MayNameTable(f, false)) continue;
        const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
        formula::Node copy = f->root;
        if (fix(copy, model::TableAt(wb_, sh.id, r, c))) fixes.push_back(Fix{sh.id, r, c, InternFormula(std::move(copy))});
      }
    });
  }
  for (const Fix& fx : fixes) M(fx.sheet)->SetFormula(fx.r, fx.c, fx.id);
  if (!fixes.empty()) RebuildAll();
}

void Document::RewriteTableRefs(const std::string& old_table, const std::string& new_table, const std::string& old_col, const std::string& new_col) {
  // every formula that names the table (or, without a name, sits inside it) gets the new table / column name. An
  // empty new name means the table (old_col empty) or the column is gone: what points at it becomes #REF!, as in
  // Excel (=SUM(#REF!)). A change of the table itself also reaches its name alone (=ROWS(Table1)) and the reference
  // texts of defined names, rules and charts.
  const bool whole = old_col.empty();
  const bool gone = whole ? new_table.empty() : new_col.empty();
  // a name alone is the table's only while no defined name has that name
  const bool bare = whole && std::none_of(wb_.names->begin(), wb_.names->end(), [&](const model::NameDef& d) { return IEq(d.name, old_table); });
  std::function<bool(formula::Node&, const Table*)> fix = [&](formula::Node& n, const Table* here) {
    if (n.kind == formula::NK::StructRef) {
      const bool mine = n.str.empty() ? (here && IEq(here->name, old_table)) : IEq(n.str, old_table);
      if (!mine) return false;
      if (whole) {
        if (gone) {
          n = RefError();
          return true;
        }
        if (n.str.empty() || n.str == new_table) return false;
        n.str = new_table;
        return true;
      }
      bool changed = false;
      for (size_t k = 0; k < 2 && k < n.kids.size(); ++k) {
        if (!IEq(n.kids[k].str, old_col)) continue;
        if (gone) {
          n = RefError();
          return true;
        }
        n.kids[k].str = new_col;
        changed = true;
      }
      return changed;
    }
    if (bare && n.kind == formula::NK::Name && IEq(n.str, old_table)) {
      if (gone) n = RefError();
      else n.str = new_table;
      return true;
    }
    bool changed = false;
    for (auto& k : n.kids) changed = fix(k, here) || changed;
    return changed;
  };
  struct Fix {
    uint32_t sheet, r, c, id;
  };
  std::vector<Fix> fixes;
  for (const auto& sc : wb_.sheets) {
    const Sheet& sh = sc.get();
    sh.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& tile) {
      if (tile.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        const uint32_t id = tile.formula[idx];
        if (!id) continue;
        const formula::CompiledFormula* f = Formula(id);
        if (!MayNameTable(f, bare)) continue;
        const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
        formula::Node copy = f->root;
        if (fix(copy, model::TableAt(wb_, sh.id, r, c))) fixes.push_back(Fix{sh.id, r, c, InternFormula(std::move(copy))});
      }
    });
  }
  for (const Fix& fx : fixes) M(fx.sheet)->SetFormula(fx.r, fx.c, fx.id);
  // names, rules and charts name the table in their texts (read at A1 and written back there). A column renamed by
  // its header is left to the cells: inserting or deleting columns renames them for a moment while they shift.
  bool texts = false;
  if (whole || gone) {
    const std::string low = Lower(old_table);
    ForEachReferenceText([&](std::string& text) {
      if (text.empty() || Lower(text).find(low) == std::string::npos) return;
      std::string_view t = text;
      const bool eq = t[0] == '=';
      if (eq) t.remove_prefix(1);
      formula::ParseResult pr = formula::Parse(t, core::CellPos{0, 0}, this);
      if (!pr.ok || !fix(pr.root, nullptr)) return;
      text = (eq ? "=" : "") + formula::ToA1(pr.root, core::CellPos{0, 0}, this);
      texts = true;
    });
  }
  if (!fixes.empty() || texts) RebuildAll();
}

uint32_t Document::AddTable(uint32_t sheet, const Rect& rect_in, bool has_headers, const std::string& style, std::string* error) {
  if (Refuse(sheet, nullptr, Allow::Never)) return 0;
  const Sheet* s = S(sheet);
  if (!s) return 0;
  Rect rect = rect_in;
  // the sheet's own rows and columns: whole columns given to the hard row limit (or the end marker) made a table of two
  // billion rows, whose calculated column a typed formula filled a row at a time (doc_fuzz --wide: 49 GB)
  rect.r1 = std::min(rect.r1, s->row_limit - 1);
  rect.c1 = std::min(rect.c1, s->col_limit - 1);
  if (rect.r0 > rect.r1 || rect.c0 > rect.c1) return 0;
  if (const std::string why = TableCantHold(sheet, Rect{rect.r0, rect.c0, std::max(rect.r1, rect.r0 + 1), rect.c1}, 0); !why.empty()) {
    if (error) *error = why;
    return 0;
  }
  // the sheet's AutoFilter (when no table holds it): a table over part of its list is refused, one over all of its
  // header row and columns takes it over, its criteria kept only when the table is exactly the filtered list, otherwise
  // dropped with the rows they hid shown (Excel 16, measured 2026-11-16 through ListObjects.Add)
  const model::AutoFilter& af = *s->autofilter;
  const bool af_free = af.on && std::none_of(s->tables->begin(), s->tables->end(), [&](const Table& t) { return TableOwnsSheetFilter(*s, t); });
  bool drop_filter = false;
  if (af_free && af.range.Intersects(rect)) {
    if (rect.r0 > af.range.r0 || rect.r1 < af.range.r0 || rect.c0 > af.range.c0 || rect.c1 < af.range.c1) {
      if (error) *error = TR("This will change a filtered range on your worksheet. To complete this task, please remove AutoFilters.");
      return 0;
    }
    drop_filter = !has_headers || !(rect == af.range);
  }
  const Rect header_row{rect.r0, rect.c0, rect.r0, rect.c1};
  // the header row's insertion is asked before anything changes: refused after the rest, it left half a table
  if (!has_headers && RefuseInsertCells(sheet, header_row, true, drop_filter)) return 0;
  if (drop_filter) ClearAutoFilter(sheet);
  if (!has_headers) {
    // a header row is inserted above the data (Column1, Column2...), shifting the cells down
    if (!InsertCells(sheet, header_row, true)) return 0;
    rect.r1 += 1;
  }
  if (rect.r1 == rect.r0) rect.r1 += 1;  // a header only: one empty data row
  TakeSpillsUnder(sheet, rect);
  Table t;
  t.id = wb_.next_object_id++;
  t.name = NextTableName();
  t.range = rect;
  t.style = style.empty() ? "TableStyleMedium2" : style;
  t.columns.resize(rect.Cols());
  M(sheet)->tables.mut().push_back(t);
  NoteStructural();
  SyncTableHeaders(sheet);
  // the table gets the sheet's AutoFilter when the sheet has none
  if (!S(sheet)->autofilter->on) SetAutoFilter(sheet, rect);
  RebuildAll();
  changes_.push_back(CellChange{sheet, rect, kSrcUser});
  wb_.dirty = true;
  return t.id;
}

bool Document::ResizeTable(uint32_t sheet, uint32_t id, const Rect& rect, std::string* error) {
  if (Refuse(sheet, nullptr, Allow::Never)) return false;
  const Table* t = TableById(sheet, id);
  if (!t) return false;
  const Rect old = t->range;
  if (rect.r0 != old.r0 || rect.r1 <= rect.r0) {
    if (error) *error = TR("The headers must remain in the same row, and the table needs at least one data row.");
    return false;
  }
  if (const std::string why = TableCantHold(sheet, rect, id); !why.empty()) {
    if (error) *error = why;
    return false;
  }
  const Table before = *t;
  NoteStructural();
  TakeSpillsUnder(sheet, rect);
  // columns left out of the table: what points at them is #REF! (Excel)
  for (uint32_t j = 0; j < before.columns.size(); ++j) {
    const uint32_t c = old.c0 + j;
    if ((c < rect.c0 || c > rect.c1) && !before.columns[j].name.empty()) RewriteTableRefs(before.name, before.name, before.columns[j].name, std::string());
  }
  // the totals row moves to the new last row with what it holds; the row it leaves is a data row now
  const uint32_t keep_c0 = std::max(old.c0, rect.c0), keep_c1 = std::min(old.c1, rect.c1);
  const bool move_totals = before.totals && rect.r1 != old.r1 && keep_c0 <= keep_c1;
  ClipData totals;
  if (move_totals) {
    totals = Copy(sheet, Rect{old.r1, keep_c0, old.r1, keep_c1});
    ClearContents(sheet, Rect{old.r1, keep_c0, old.r1, keep_c1}, kSrcCalc);
  }
  Table* m = MutTable(sheet, id);
  m->range = rect;
  // each column keeps its name and totals where it stays (the left edge may move)
  std::vector<model::TableColumn> cols(rect.Cols());
  for (uint32_t j = 0; j < cols.size(); ++j) {
    const int64_t from = int64_t{rect.c0} + j - old.c0;
    if (from >= 0 && from < static_cast<int64_t>(before.columns.size())) cols[j] = before.columns[static_cast<size_t>(from)];
  }
  m->columns = std::move(cols);
  SyncTableHeaders(sheet);
  if (S(sheet)->autofilter->on && S(sheet)->autofilter->range == old) M(sheet)->autofilter.mut().range = rect;
  if (move_totals) Paste(sheet, rect.r1, keep_c0, totals);
  // calculated columns continue into the new data rows (Excel's Resize fills them)
  if (const Table* nt = TableById(sheet, id)) {
    const Rect od = before.Data(), nd = nt->Data();
    if (od.r0 <= od.r1 && nd.r1 > od.r1) {
      for (uint32_t c = std::max(nd.c0, od.c0); c <= std::min(nd.c1, od.c1); ++c) {
        if (!CalculatedFormula(*S(sheet), c, od.r0, od.r1)) continue;
        const ClipData one = Copy(sheet, Rect::Cell(od.r1, c));
        for (uint32_t r = od.r1 + 1; r <= nd.r1; ++r) {
          if (S(sheet)->IsEmptyCell(r, c)) Paste(sheet, r, c, one, PasteMode::Formulas);
        }
      }
    }
  }
  RebuildAll();
  changes_.push_back(CellChange{sheet, old.Union(rect), kSrcUser});
  wb_.dirty = true;
  return true;
}

bool Document::RenameTable(uint32_t sheet, uint32_t id, const std::string& name, std::string* error) {
  if (RefuseReadOnly(sheet)) return false;  // read-only mode: nothing of the table changes (it changed half)
  const Table* t = TableById(sheet, id);
  if (!t) return false;
  if (IEq(t->name, name) && t->name == name) return true;
  if (!ValidTableName(name, id, error)) return false;
  const std::string old = t->name;
  MutTable(sheet, id)->name = name;
  NoteStructural();
  RewriteTableRefs(old, name, std::string(), std::string());
  // its slicers and the PivotTables made from it follow the new name
  for (auto& sc : wb_.sheets) {
    if (std::any_of(sc->slicers->begin(), sc->slicers->end(), [&](const model::Slicer& x) { return x.table == old; })) {
      for (auto& x : sc.mut().slicers.mut()) {
        if (x.table == old) x.table = name;
      }
    }
    if (std::any_of(sc->pivots->begin(), sc->pivots->end(), [&](const auto& p) { return IEq(p.src_table, old); })) {
      for (auto& p : sc.mut().pivots.mut()) {
        if (IEq(p.src_table, old)) p.src_table = name;
      }
    }
  }
  wb_.dirty = true;
  return true;
}

bool Document::DeleteTable(uint32_t sheet, uint32_t id, bool keep_look) {
  if (Refuse(sheet, nullptr, Allow::Never)) return false;
  if (!TableById(sheet, id)) return false;
  ClearTableFilter(sheet, id);  // the rows its own filter hid are shown
  const Table* t = TableById(sheet, id);
  if (!t) return false;
  const Table copy = *t;
  NoteStructural();
  if (!keep_look) {
    // ListObject.Delete (the cells go too): what pointed at the table is #REF!, as in Excel (=SUM(#REF!))
    RewriteTableRefs(copy.name, std::string(), std::string(), std::string());
  } else {
    // Convert to Range / Unlist: formulas that use the table's names become ordinary references
    const bool bare = std::none_of(wb_.names->begin(), wb_.names->end(), [&](const model::NameDef& d) { return IEq(d.name, copy.name); });
    struct Fix {
      uint32_t sheet, r, c, id;
    };
    std::vector<Fix> fixes;
    for (const auto& sc : wb_.sheets) {
      const Sheet& sh = sc.get();
      sh.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& tile) {
        if (tile.formula_count == 0) return;
        for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
          const uint32_t fid = tile.formula[idx];
          const formula::CompiledFormula* f = fid ? Formula(fid) : nullptr;
          if (!MayNameTable(f, bare)) continue;
          const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
          formula::Node root = f->root;
          bool changed = false;
          auto absolute = [&](uint32_t ts, const Rect& rr, bool this_row) {
            formula::Node ref;
            ref.kind = formula::NK::Ref;
            // named with its sheet, its own too, as Excel 16 writes it (measured: [@Qty] becomes Sheet1!$C3)
            ref.ref.sheet = static_cast<int32_t>(ts);
            ref.ref.r0 = this_row ? formula::RefCoord{0, false} : formula::RefCoord{rr.r0, true};
            ref.ref.r1 = this_row ? formula::RefCoord{0, false} : formula::RefCoord{rr.r1, true};
            ref.ref.c0 = formula::RefCoord{rr.c0, true};
            ref.ref.c1 = formula::RefCoord{rr.c1, true};
            ref.ref.is_range = !rr.IsCell();
            return ref;
          };
          std::function<void(formula::Node&)> walk = [&](formula::Node& n) {
            if (n.kind == formula::NK::StructRef) {
              uint32_t ts = sh.id;
              Rect rr;
              const bool mine = n.str.empty() ? copy.range.Contains(r, c) && sh.id == sheet : IEq(n.str, copy.name);
              if (mine && ResolveStructRef(sh.id, n, core::CellPos{r, c}, &ts, &rr) == 0) {
                n = absolute(ts, rr, (static_cast<uint8_t>(n.op) & model::kItemThisRow) != 0);
                changed = true;
              }
              return;
            }
            if (bare && n.kind == formula::NK::Name && IEq(n.str, copy.name)) {
              // =ROWS(Table1): the data body
              Rect rr = copy.Data();
              if (rr.r0 > rr.r1) rr.r1 = rr.r0;
              n = absolute(sheet, rr, false);
              changed = true;
              return;
            }
            for (auto& k : n.kids) walk(k);
          };
          walk(root);
          if (changed) fixes.push_back(Fix{sh.id, r, c, InternFormula(std::move(root))});
        }
      });
    }
    for (const Fix& fx : fixes) M(fx.sheet)->SetFormula(fx.r, fx.c, fx.id);
  }
  // the look stays as cell formats
  if (keep_look) {
    // every cell takes the style's fill, font and lines (Excel keeps the whole look)
    if (const model::TableStyleDef* def = model::FindTableStyle(*wb_.store, copy.style)) {
      const model::TableStyleDef d = *def;
      for (uint32_t r = copy.range.r0; r <= copy.range.r1; ++r) {
        for (uint32_t c = copy.range.c0; c <= copy.range.c1; ++c) {
          ModifyStyle(sheet, Rect::Cell(r, c), [&](model::Style& x) { model::ApplyTableStyle(copy, d, r, c, &x); });
        }
      }
    } else if (copy.header) {
      ModifyStyle(sheet, Rect{copy.range.r0, copy.range.c0, copy.range.r0, copy.range.c1}, [](model::Style& x) { x.bold = true; });
    }
  }
  // its filter buttons go with it, the sheet's AutoFilter when the table had it (Excel 16, measured 2026-11-20:
  // AutoFilterMode is False after Unlist); left on, a table made there later over part of it was refused
  if (const Sheet* s = S(sheet); s && TableOwnsSheetFilter(*s, copy)) ClearAutoFilter(sheet);
  auto& tables = M(sheet)->tables.mut();
  tables.erase(std::remove_if(tables.begin(), tables.end(), [&](const Table& x) { return x.id == id; }), tables.end());
  RebuildAll();
  changes_.push_back(CellChange{sheet, copy.range, kSrcUser});
  wb_.dirty = true;
  return true;
}

void Document::ClearTablesIn(uint32_t sheet, const Rect& rect) {
  const Sheet* s = S(sheet);
  if (!s || s->tables->empty()) return;
  std::vector<uint32_t> whole;
  for (const Table& t : *s->tables) {
    if (rect.Contains(t.range)) whole.push_back(t.id);
  }
  for (const uint32_t id : whole) DeleteTable(sheet, id, false);
}

bool Document::UpdateTable(uint32_t sheet, uint32_t id, const std::function<void(Table&)>& fn) {
  if (RefuseReadOnly(sheet)) return false;  // read-only mode: nothing of the table changes (it changed half)
  const Table* t = TableById(sheet, id);
  if (!t) return false;
  const Table before = *t;
  Table after = before;
  fn(after);
  after.id = before.id;
  after.name = before.name;
  after.range = before.range;
  // the row above taken by the header row is free, or cells are inserted; the row under the data taken by the totals
  // row, the same: both insertions are asked before anything changes, or a refused one let the headers or totals be
  // written over the cells in the way
  const auto room_above = [&](const Rect& r) {
    bool room = r.r0 > 0;
    if (room) {
      const Rect above{r.r0 - 1, r.c0, r.r0 - 1, r.c1};
      for (uint32_t c = r.c0; c <= r.c1 && room; ++c) room = S(sheet)->IsEmptyCell(above.r0, c);
      for (const Table& o : *S(sheet)->tables) room = room && (o.id == id || !o.range.Intersects(above));
    }
    return room;
  };
  const auto busy_below = [&](const Rect& r) {
    bool busy = false;
    for (uint32_t c = r.c0; c <= r.c1; ++c) busy = busy || !S(sheet)->IsEmptyCell(r.r1 + 1, c);
    return busy;
  };
  {
    const Rect& r = before.range;
    if (after.header && !before.header && !room_above(r) && RefuseInsertCells(sheet, Rect{r.r0, r.c0, r.r0, r.c1}, true)) return false;
    if (after.totals && !before.totals && busy_below(r) && RefuseInsertCells(sheet, Rect{r.r1 + 1, r.c0, r.r1 + 1, r.c1}, true)) return false;
  }
  NoteStructural();
  bool write_headers = false;
  // the header row (Excel's ShowHeaders): hidden, the table starts at its first data row and the header cells are
  // cleared (the column names stay); shown again, it takes the row above, made free when it isn't, and the names
  // come back
  if (after.header != before.header) {
    Rect r = before.range;
    if (!after.header) {
      ClearTableFilter(sheet, id);
      if (const Table* now = TableById(sheet, id)) after.filter = now->filter;
      if (S(sheet)->autofilter->on && S(sheet)->autofilter->range == before.range) ClearAutoFilter(sheet);
      ClearContents(sheet, Rect{r.r0, r.c0, r.r0, r.c1}, kSrcCalc);
      r.r0 += 1;
      if (r.r1 < r.r0) r.r1 = r.r0;
    } else {
      if (!room_above(r)) {
        if (!InsertCells(sheet, Rect{r.r0, r.c0, r.r0, r.c1}, true)) return false;  // the table moves down a row
        if (const Table* moved = TableById(sheet, id)) r = moved->range;
      }
      r.r0 -= 1;
      write_headers = true;
    }
    after.range = r;
  }
  // the totals row comes and goes with the row below the data
  if (after.totals != before.totals) {
    Rect r = after.range;
    if (after.totals) {
      // Excel's default: Total under the first column, and the last column summed when it holds numbers, counted
      // when it holds only text
      bool numbers = false, text = false;
      if (const Sheet* s = S(sheet)) {
        for (uint32_t rr = r.r0 + (after.header ? 1 : 0); rr <= r.r1 && !numbers; ++rr) {
          const Value& v = s->GetValue(rr, r.c1);
          numbers = v.IsNumeric();
          text = text || !v.IsEmpty();
        }
      }
      // make room when the row below isn't empty
      if (busy_below(r) && !InsertCells(sheet, Rect{r.r1 + 1, r.c0, r.r1 + 1, r.c1}, true)) return false;
      r.r1 += 1;
      after.range = r;
      if (after.columns.size() == r.Cols() && !after.columns.empty()) {
        bool any = false;
        for (const auto& c : after.columns) any = any || c.totals_func || !c.totals_label.empty();
        if (!any) {
          after.columns.front().totals_label = "Total";
          after.columns.back().totals_func = text && !numbers ? 103 : 109;  // COUNTA or SUM
        }
      }
    } else {
      ClearContents(sheet, Rect{r.r1, r.c0, r.r1, r.c1});
      r.r1 -= 1;
      after.range = r;
    }
  }
  *MutTable(sheet, id) = after;
  if (write_headers) {
    for (uint32_t j = 0; j < after.columns.size() && j < after.range.Cols(); ++j) {
      SetValue(sheet, after.range.r0, after.range.c0 + j, Value::String(after.columns[j].name), kSrcCalc);
    }
    SyncTableHeaders(sheet);
    if (after.filter_button && !S(sheet)->autofilter->on) SetAutoFilter(sheet, after.range);
  }
  // the totals row's cells are written when it comes, not at every change of the table: a formula typed into it
  // (=MAX(C3:C6)) was cleared by Banded Rows
  if (after.totals && !before.totals) WriteTotalsRow(sheet, id);
  RebuildAll();
  changes_.push_back(CellChange{sheet, before.range.Union(after.range), kSrcUser});
  wb_.dirty = true;
  return true;
}

void Document::WriteTotalsRow(uint32_t sheet, uint32_t id) {
  const Table* t = TableById(sheet, id);
  if (!t || !t->totals) return;
  const Table copy = *t;
  const uint32_t r = copy.range.r1;
  for (uint32_t j = 0; j < copy.columns.size(); ++j) {
    const auto& col = copy.columns[j];
    const uint32_t c = copy.range.c0 + j;
    if (col.totals_func) {
      std::string name = col.name;
      std::string esc;
      for (char ch : name) {
        if (ch == '[' || ch == ']' || ch == '#' || ch == '\'') esc += '\'';
        esc += ch;
      }
      // the table's own column, without its name, as Excel writes it: =SUBTOTAL(109,[Price])
      SetFormula(sheet, r, c, "SUBTOTAL(" + std::to_string(col.totals_func) + ",[" + esc + "])", nullptr, kSrcCalc);
      // shown in the column's number format, the one its data rows share (Excel 16, measured: under 0.00, $#,##0.00 or
      // 0% the totals cell takes it; a SUBTOTAL typed there infers none, so it is the totals row's own)
      const Rect d = copy.Data();
      const Sheet* ss = S(sheet);
      if (d.r0 <= d.r1 && ss && !wb_.styles->Get(ss->GetStyle(r, c)).numfmt) {
        const uint32_t fmt = wb_.styles->Get(ss->GetStyle(d.r0, c)).numfmt;
        bool shared = fmt != 0;
        for (uint32_t rr = d.r0 + 1; rr <= d.r1 && shared; ++rr) shared = wb_.styles->Get(ss->GetStyle(rr, c)).numfmt == fmt;
        if (shared) ModifyStyle(sheet, Rect::Cell(r, c), [fmt](model::Style& st) { st.numfmt = fmt; });
      }
    } else if (!col.totals_label.empty()) {
      SetValue(sheet, r, c, Value::String(col.totals_label), kSrcCalc);
    } else {
      ClearContents(sheet, Rect::Cell(r, c), kSrcCalc);
    }
  }
}

bool Document::SetTableTotal(uint32_t sheet, uint32_t id, uint32_t column, int func, const std::string& label) {
  if (RefuseReadOnly(sheet)) return false;  // read-only mode: nothing of the table changes (it changed half)
  Table* t = MutTable(sheet, id);
  if (!t || column >= t->columns.size()) return false;
  t->columns[column].totals_func = func;
  t->columns[column].totals_label = label;
  WriteTotalsRow(sheet, id);
  return true;
}

// typing next to a table makes it bigger (Excel's AutoCorrect "include new rows and columns in table"); a block
// written below or right of it (a paste, Python's Range.Value) grows it once to take the whole block
void Document::ExtendList(uint32_t sheet, uint32_t r, uint32_t c) {
  const Sheet* s = S(sheet);
  if (!s || r < 2 || s->IsEmptyCell(r, c) || TableAt(sheet, r, c)) return;  // a table has rules of its own
  // the list: the columns the row above and the new row fill around the typed cell, and the rows above up to a blank
  // one, five at most. Only these rows are read: a CurrentRegion under a million-row list walked all of it per Enter
  auto filled = [&](uint32_t rr, uint32_t cc) { return !s->IsEmptyCell(rr, cc); };
  uint32_t c0 = c, c1 = c;
  while (c0 > 0 && c - c0 < 255 && (filled(r - 1, c0 - 1) || filled(r, c0 - 1))) --c0;
  while (c1 + 1 < core::kMaxCols && c1 - c < 255 && (filled(r - 1, c1 + 1) || filled(r, c1 + 1))) ++c1;
  auto row_empty = [&](uint32_t rr) {
    for (uint32_t cc = c0; cc <= c1; ++cc) {
      if (filled(rr, cc)) return false;
    }
    return true;
  };
  if (row_empty(r - 1)) return;  // nothing right above: not the row under a list
  if (r + 1 < s->row_limit && !row_empty(r + 1)) return;  // typed inside a list: nothing extends
  uint32_t top = r - 1;
  while (top > 0 && r - top < 5 && !row_empty(top - 1)) --top;  // the five rows above, up to a blank one
  const Rect reg{top, c0, r, c1};
  // the typed cell's format: a run of three or more rows above that share one (A2:A4 bold over A5, A6: bold), the
  // nearest one; rows that differ in between break a run (A2, A4, A6 bold: none)
  if (s->GetOwnStyle(r, c) == 0) {
    int64_t run = 0, run_end = -1;
    uint32_t style = 0;
    for (int64_t rr = int64_t{r} - 1; rr >= int64_t{top}; --rr) {
      const uint32_t st = s->GetStyle(static_cast<uint32_t>(rr), c);
      if (run > 0 && st == style) {
        ++run;
      } else {
        if (run >= 3) break;
        run = 1, style = st, run_end = rr;
      }
    }
    if (run >= 3 && style != 0) Paste(sheet, r, c, Copy(sheet, Rect::Cell(static_cast<uint32_t>(run_end), c)), PasteMode::Formats);
  }
  // the formulas of the row above, into the row's empty cells
  for (uint32_t k = reg.c0; k <= reg.c1; ++k) {
    s = S(sheet);
    if (k == c || !s->IsEmptyCell(r, k)) continue;
    const uint32_t fid = s->GetFormula(r - 1, k);
    if (!fid) continue;
    uint32_t same = 0;
    for (uint32_t rr = top; rr < r; ++rr) same += s->GetFormula(rr, k) == fid ? 1 : 0;
    if (same < 4) continue;
    const formula::CompiledFormula* f = Formula(fid);
    if (!f) continue;
    // every cell of the new row it reads holds something (=B6*C6 waits for C6; $F$1 and other rows don't count)
    bool ready = true;
    formula::Node probe = f->root;
    formula::ForEachRef(probe, [&](formula::RefSpec& ref) {
      if (!ready || !ref.HasCells() || ref.sheet >= 0 || ref.whole_cols || ref.whole_rows || ref.r0.abs || ref.r0.v != 0) return;
      if (ref.is_range && (ref.r1.abs || ref.r1.v != 0)) return;
      const int64_t a = ref.c0.Resolve(k), b = ref.is_range ? ref.c1.Resolve(k) : a;
      for (int64_t cc = std::min(a, b); cc <= std::max(a, b) && ready; ++cc) {
        if (cc < 0 || cc >= static_cast<int64_t>(core::kMaxCols) || s->IsEmptyCell(r, static_cast<uint32_t>(cc))) ready = false;
      }
    });
    if (ready) Paste(sheet, r, k, Copy(sheet, Rect::Cell(r - 1, k)), PasteMode::Formulas);
  }
}

void Document::GrowDeferredTables() {
  defer_table_growth = false;
  const std::vector<std::pair<uint32_t, core::CellPos>> calc = std::move(deferred_calc_);
  deferred_calc_.clear();
  const std::vector<std::pair<uint32_t, Rect>> list = std::move(deferred_growth_);
  deferred_growth_.clear();
  if (!list.empty()) GrowTables(list);
  if (calc.empty()) return;
  Begin(TR("Calculated Column"));
  for (const auto& [sheet, p] : calc) FillCalculatedColumn(sheet, p.row, p.col);
  Commit();
}

void Document::GrowTables(const std::vector<std::pair<uint32_t, Rect>>& list) {
  Begin(TR("Table AutoExpansion"));
  for (const auto& [sheet, w] : list) {
    // the cells the write wrote are its own, whichever of its parts noted them (Ctrl+Enter: the active cell, then the
    // fill over the others)
    std::vector<Rect> written;
    for (const auto& [sh2, w2] : list) {
      if (sh2 == sheet) written.push_back(w2);
    }
    growth_written_ = &written;
    AutoExpandTable(sheet, w);
    growth_written_ = nullptr;
  }
  Commit();
}

void Document::AutoExpandTable(uint32_t sheet, const Rect& w, bool formula) {
  const Sheet* s = S(sheet);
  if (!s || s->tables->empty() || ProtectionApplies(sheet)) return;
  // a formula in the row under a table (typed, Alt+= or Range.Formula) makes that row the table's totals row, with
  // nothing else written in it; a value grows the data (Excel 16, measured: =SUM(C3:C6) in C7 under B2:D6 is a
  // totals row, 5 a data row). Grown as data, the SUM's range followed to C3:C7, a circular reference
  if (formula && w.r0 == w.r1) {
    for (const Table& t : *s->tables) {
      if (t.totals || w.r0 != t.range.r1 + 1 || w.c0 < t.range.c0 || w.c1 > t.range.c1) continue;
      if (!t.header && t.range.r0 == t.range.r1) continue;  // a table of one row keeps it as data
      const Rect old = t.range;
      NoteStructural();
      Table* mt = MutTable(sheet, t.id);
      mt->range.r1 = w.r0;
      mt->totals = true;
      RebuildAll();
      changes_.push_back(CellChange{sheet, Rect{old.r0, old.c0, w.r0, old.c1}, kSrcUser});
      wb_.dirty = true;
      return;
    }
  }
  if (defer_table_growth) {
    if (deferred_growth_.size() < 4096) deferred_growth_.emplace_back(sheet, w);
    return;
  }
  for (const Table& t : *s->tables) {
    const bool below = !t.totals && w.r0 == t.range.r1 + 1 && w.c0 >= t.range.c0 && w.c0 <= t.range.c1;
    const bool right = w.c0 == t.range.c1 + 1 && w.r0 >= t.range.r0 && w.r0 <= t.range.r1;
    if (!below && !right) continue;
    Rect nr = t.range;
    if (below) nr.r1 = w.r1;
    else nr.c1 = std::min(w.c1, core::kMaxCols - 1);
    // only over a line that holds nothing besides what is written (Excel 16, measured): 9 typed in C7 under a table
    // doesn't grow it while B7 holds =SUM(B2:B6), 5 or any text, nor in D2 beside it while D4 does. Grown, the table
    // took the SUM in and its range followed to =SUM(B2:B7), a circular reference (basics seq470822). A spill's
    // values are TakeSpillsUnder's
    {
      const Rect band = below ? Rect{t.range.r1 + 1, t.range.c0, nr.r1, t.range.c1} : Rect{t.range.r0, t.range.c1 + 1, t.range.r1, nr.c1};
      bool other = false;
      auto counts = [&](uint32_t rr, uint32_t cc) {
        if (other || w.Contains(rr, cc)) return;
        if (growth_written_ && std::any_of(growth_written_->begin(), growth_written_->end(), [&](const Rect& x) { return x.Contains(rr, cc); })) return;
        uint32_t ar = 0, ac = 0;
        if (!s->spills->empty() && s->SpillAt(rr, cc, &ar, &ac) && (ar != rr || ac != cc)) return;
        other = true;
      };
      ForEachCellInTiles(s->values, band, [&](uint32_t rr, uint32_t cc, const model::ValueTile& vt, uint32_t idx) {
        if (vt.Occupied(idx)) counts(rr, cc);
      });
      ForEachCellInTiles(s->meta, band, [&](uint32_t rr, uint32_t cc, const model::MetaTile& mt, uint32_t idx) {
        if (mt.formula[idx]) counts(rr, cc);
      });
      if (other) continue;
    }
    // it doesn't grow over what a table can't hold: a typed formula beside it took in part of an array, whose value
    // became a header (doc_fuzz seed 10312766)
    if (!TableCantHold(sheet, nr, t.id).empty()) return;
    const uint32_t id = t.id;
    const Rect old = t.range;
    const bool had_header = t.header;  // read before the table is copied for writing
    const uint32_t body0 = old.r0 + (had_header ? 1 : 0);
    NoteStructural();
    TakeSpillsUnder(sheet, nr);
    Table* m = MutTable(sheet, id);
    m->range = nr;
    m->columns.resize(nr.Cols());
    if (S(sheet)->autofilter->on && S(sheet)->autofilter->range == old) M(sheet)->autofilter.mut().range = nr;
    SyncTableHeaders(sheet);
    // the cells written that it grows over take the formats of its cells beside them (the last row's, the last
    // column's), over a format of their own, once the header row is text; the other cells it takes in keep theirs
    // (Excel 16, measured: 12.5 typed beside a table's 0.00 header in a 0% cell is the header "1250%" in 0.00; 7 under
    // it in a 0% cell shows 0.00; Fill Right of one row leaves the 0.00% of the column's other cells). A growth of more
    // than a million cells is a fill's, which writes its own formats
    const Rect grown = below ? Rect{old.r1 + 1, nr.c0, nr.r1, nr.c1} : Rect{nr.r0, old.c1 + 1, nr.r1, nr.c1};
    const Rect add{std::max(grown.r0, w.r0), std::max(grown.c0, w.c0), std::min(grown.r1, w.r1), std::min(grown.c1, w.c1)};
    if (grown.Intersects(w) && add.Count() <= 1'000'000) {
      NoteFormat(sheet, add);
      Sheet* ms = M(sheet);
      for (uint32_t r = add.r0; r <= add.r1; ++r) {
        for (uint32_t c = add.c0; c <= add.c1; ++c) {
          // grown to the right only the header takes the format beside it: 7 typed in D2 beside C2's 0.00 stays General
          // (Excel 16, measured: tblgrow_refs_right), where a row grown under the table takes the row above's
          if (!below && !(had_header && r == nr.r0)) continue;
          const uint32_t st = below ? ms->GetStyle(old.r1, c) : ms->GetStyle(r, old.c1);
          if (ms->GetStyle(r, c) == st) continue;
          // under it, only a format every data row of the column shares: 0.00 in C2:C4 makes the new C5 0.00, in C3:C4
          // only it leaves C5 as it was, its own 0% or General (Excel 16, measured through Range.Formula and by keys)
          if (below) {
            bool shared = true;
            for (uint32_t rr = body0; rr < old.r1 && shared; ++rr) shared = ms->GetStyle(rr, c) == st;
            if (!shared) continue;
          }
          // a header's format waits until its text is made from what the write put there (SyncTableHeaders, at the end
          // of the write: the grow comes before the write)
          if (!below && had_header && r == nr.r0) grown_header_styles_.push_back({sheet, r, c, st});
          else ms->ShowStyle(r, c, st);
        }
      }
    }
    if (below) GrowRangesWithTable(sheet, old.r0, old.r1, nr.r1, old.c0, old.c1);
    if (below) {
      // calculated columns: their formula continues in the new rows the write leaves empty. A formula in the last row
      // of a column that holds others (values, other formulas) isn't one: Excel 16 leaves the new row empty there
      // (measured, rand3_054: =A2&B2 in F9 only of E6:G9 was copied into F10 under a write in E10)
      for (uint32_t cc = nr.c0; cc <= nr.c1; ++cc) {
        if ((cc >= w.c0 && cc <= w.c1) || !S(sheet)->GetFormula(old.r1, cc)) continue;
        if (body0 <= old.r1 && !CalculatedFormula(*S(sheet), cc, body0, old.r1)) continue;
        const ClipData one = Copy(sheet, Rect::Cell(old.r1, cc));
        // a run of empty cells at a time: a fill down a whole column grew the table by a million rows
        for (uint32_t rr = w.r0; rr <= w.r1;) {
          if (!S(sheet)->IsEmptyCell(rr, cc)) {
            ++rr;
            continue;
          }
          uint32_t end = rr;
          while (end < w.r1 && S(sheet)->IsEmptyCell(end + 1, cc)) ++end;
          PasteRepeated(sheet, rr, cc, end - rr + 1, 1, one, PasteMode::Formulas);
          rr = end + 1;
        }
      }
    }
    RebuildAll();
    changes_.push_back(CellChange{sheet, nr, kSrcUser});
    return;
  }
}

void Document::GrowRangesWithTable(uint32_t sheet, uint32_t first, uint32_t old_last, uint32_t new_last, uint32_t c0, uint32_t c1) {
  // Excel 16 (measured, tblgrow_refs_ cases): a range of two rows or more inside the table's columns, from its first
  // row on, ending at its last row grows to the new last row, $ or not, from any sheet and through a name (=SUM(B2:C3)
  // under B1:C3 is =SUM(B2:C4) once C4 is typed); one row (C3:C3), one cell (=C3), a range wider than the table
  // (B2:D3) stay, and so does everything when the table grows to the right
  if (new_last <= old_last) return;
  auto grow = [&](formula::RefSpec& ref, core::CellPos origin, int32_t own) {
    if (!ref.HasCells() || ref.bad || !ref.is_range || ref.whole_rows || ref.whole_cols || ref.spill || ref.sheet2 >= 0) return false;
    if (ref.TargetSheet(own) != static_cast<int32_t>(sheet)) return false;
    int64_t ra = ref.r0.Resolve(origin.row), rb = ref.r1.Resolve(origin.row);
    formula::RefCoord* hi = &ref.r1;
    if (ra > rb) std::swap(ra, rb), hi = &ref.r0;
    const int64_t ca = std::min(ref.c0.Resolve(origin.col), ref.c1.Resolve(origin.col));
    const int64_t cb = std::max(ref.c0.Resolve(origin.col), ref.c1.Resolve(origin.col));
    if (rb != int64_t{old_last} || ra >= rb || ra < int64_t{first} || ca < int64_t{c0} || cb > int64_t{c1}) return false;
    hi->v = hi->abs ? int64_t{new_last} : int64_t{new_last} - origin.row;
    return true;
  };
  bool any = false;
  // cell formulas: those reading the table's last row (the dependency index knows them, on every sheet)
  std::vector<uint64_t> readers;
  deps_.QueryRect(sheet, Rect{old_last, c0, old_last, c1}, [&](uint64_t k) { readers.push_back(k); });
  std::sort(readers.begin(), readers.end());
  readers.erase(std::unique(readers.begin(), readers.end()), readers.end());
  for (uint64_t k : readers) {
    const uint32_t ks = KeySheet(k), r = KeyRow(k), c = KeyCol(k);
    const Sheet* rs = S(ks);
    const formula::CompiledFormula* f = rs ? Formula(rs->GetFormula(r, c)) : nullptr;
    if (!f) continue;
    formula::Node root = f->root;
    bool changed = false;
    formula::ForEachRef(root, [&](formula::RefSpec& ref) { changed = grow(ref, core::CellPos{r, c}, static_cast<int32_t>(ks)) || changed; });
    if (!changed) continue;
    const uint32_t nid = InternFormula(std::move(root));
    UnregisterFormulaCell(ks, r, c);
    M(ks)->SetFormula(r, c, nid);
    RegisterFormulaCell(ks, r, c, nid);
    AddPending(k);
    changes_.push_back(CellChange{ks, Rect::Cell(r, c), kSrcUser});
    any = true;
  }
  // texts: names (relative to A1, as kept), rules (at their first cell, on their own sheet), objects
  auto text = [&](std::string& t, core::CellPos origin, int32_t own) {
    if (t.empty()) return;
    std::string_view v = t;
    const bool eq = v[0] == '=';
    if (eq) v.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(v, origin, this);
    if (!pr.ok) return;
    bool changed = false;
    formula::ForEachRef(pr.root, [&](formula::RefSpec& ref) { changed = grow(ref, origin, own) || changed; });
    if (changed) t = (eq ? "=" : "") + formula::ToA1(pr.root, origin, this), any = true;
  };
  {
    std::vector<model::NameDef> names = *wb_.names;
    bool named = false;
    for (auto& n : names) {
      const std::string before = n.refers_to;
      text(n.refers_to, core::CellPos{0, 0}, n.sheet_scope >= 0 ? n.sheet_scope : -5);
      named = named || n.refers_to != before;
    }
    if (named) wb_.names.mut() = std::move(names);
  }
  for (auto& sc : wb_.sheets) {
    const int32_t own = static_cast<int32_t>(sc->id);
    model::EditRuleRefTexts(sc, [&](std::string& t, core::CellPos origin, bool constants_ok) {
      if (!(constants_ok && !t.empty() && t[0] != '=')) text(t, origin, own);
    });
    model::EditObjectRefTexts(sc, [&](std::string& t) { text(t, core::CellPos{0, 0}, own); });
  }
  // the ranges conditional formats and validation apply to grow too when they end at its last row inside its columns
  // (Excel 16, measured by keys: a rule on C3:C6 under a growth to row 7 applies to C3:C7, one on C3:C5 stays)
  if (Sheet* ms = M(sheet)) {
    auto grow_rects = [&](std::vector<Rect>& v) {
      bool ch = false;
      for (Rect& x : v) {
        if (x.r1 == old_last && x.r0 >= first && x.c0 >= c0 && x.c1 <= c1) x.r1 = new_last, ch = true;
      }
      return ch;
    };
    if (!ms->cond_formats->empty()) {
      auto rules = *ms->cond_formats;
      bool ch = false;
      for (auto& rule : rules) ch = grow_rects(rule.ranges) || ch;
      if (ch) ms->cond_formats.mut() = std::move(rules), any = true;
    }
    if (!ms->validations->empty()) {
      auto rules = *ms->validations;
      bool ch = false;
      for (auto& rule : rules) ch = grow_rects(rule.ranges) || ch;
      if (ch) ms->validations.mut() = std::move(rules), any = true;
    }
  }
  // the step's Undo takes back what read the old range too, with the whole version (AutoExpandTable rebuilds the index
  // and calculates again after it)
  if (any) NoteTouched();
}

// a formula typed into a table names one of that table's columns without its name, as Excel writes it
// (=Table1[@Qty] there is =[@Qty], Table1[Qty] is [Qty]); the other forms keep it: Table1[@[Sym]:[Qty]],
// Table1[[Qty]:[Px]], Table1[[#Headers],[Qty]] (Excel 16, measured by keys); typed into an empty table column it fills the whole column (a calculated column), and typed over a
// calculated column it replaces the column's formula in every row (Excel's default)
void Document::FillCalculatedColumn(uint32_t sheet, uint32_t r, uint32_t c, bool fill) {
  const Table* t = TableAt(sheet, r, c);
  if (!t) return;
  const uint32_t fid = S(sheet)->GetFormula(r, c);
  if (const formula::CompiledFormula* f = fid ? Formula(fid) : nullptr; f && f->key.find("T'") != std::string::npos) {
    const std::string own = t->name;
    formula::Node root = f->root;
    bool changed = false;
    ForEachStructRef(root, [&](formula::Node& n) {
      const uint8_t items = static_cast<uint8_t>(n.op);
      const bool one_column = n.kids.size() >= 2 && !n.kids[0].str.empty() && n.kids[1].str.empty() && (items == 0 || items == 16);
      if (one_column && !n.str.empty() && IEq(n.str, own)) {
        n.str.clear();
        changed = true;
      }
    });
    if (changed) {
      const uint32_t nid = InternFormula(std::move(root));
      M(sheet)->SetFormula(r, c, nid);
      RegisterFormulaCell(sheet, r, c, nid);
    }
    t = TableAt(sheet, r, c);
    if (!t) return;
  }
  if (!fill) return;
  const Sheet* s = S(sheet);
  Rect d = t->Data();
  d.r1 = std::min(d.r1, s->row_limit - 1);  // a table kept to the end marker: the sheet's rows
  if (r < d.r0 || r > d.r1 || d.Rows() < 2) return;
  bool empty = true;
  for (uint32_t rr = d.r0; rr <= d.r1 && empty; ++rr) empty = rr == r || s->IsEmptyCell(rr, c);
  if (!empty && !CalculatedFormula(*s, c, d.r0, d.r1, r)) return;
  // one paste of the column, the cell's formula in every row (a formula is kept relative to its cell, so the copies are
  // the same formula moved): a paste a row over a table on whole columns made a change record each, and the fuzzer's
  // table of a million rows reached 49 GB (doc_fuzz --wide)
  const ClipData one = Copy(sheet, Rect::Cell(r, c));
  if (one.cells.size() != 1) return;
  ClipData col;
  col.rows = static_cast<uint32_t>(d.Rows());
  col.cols = 1;
  col.src_sheet = sheet;
  col.src = Rect{d.r0, c, d.r1, c};
  col.cells.assign(col.rows, one.cells[0]);
  Paste(sheet, d.r0, c, col, PasteMode::Formulas);
}

}  // namespace yc::engine

namespace yc::engine {

// ListRows.Add / ListRow.Delete / ListColumns.Add / ListColumn.Delete: cells inside the table move, the rest of the sheet doesn't
bool Document::TableInsertRow(uint32_t sheet, uint32_t id, int64_t pos) {
  if (RefuseReadOnly(sheet)) return false;  // read-only mode: nothing of the table changes (it changed half)
  const Table* t = TableById(sheet, id);
  if (!t) return false;
  const Rect d = t->Data();
  const uint32_t rows = d.r0 <= d.r1 ? d.r1 - d.r0 + 1 : 0;
  const uint32_t at = pos < 0 || pos >= static_cast<int64_t>(rows) ? (d.r0 <= d.r1 ? d.r1 + 1 : d.r0) : d.r0 + static_cast<uint32_t>(pos);
  const Rect band{at, t->range.c0, at, t->range.c1};
  const bool inside = at <= t->range.r1;
  if (!InsertCells(sheet, band, true)) return false;  // refused: the formulas below went into the row it didn't make
  if (!inside) {
    const Table* t2 = TableById(sheet, id);
    if (t2) {
      Rect nr = t2->range;
      nr.r1 = at;
      ResizeTable(sheet, id, nr);
    }
  }
  // calculated columns continue into the new row
  const Table* t3 = TableById(sheet, id);
  if (t3 && at > t3->Data().r0) {
    for (uint32_t c = t3->range.c0; c <= t3->range.c1; ++c) {
      if (S(sheet)->GetFormula(at - 1, c)) Paste(sheet, at, c, Copy(sheet, Rect::Cell(at - 1, c)), PasteMode::Formulas);
    }
  }
  return true;
}

bool Document::TableDeleteRow(uint32_t sheet, uint32_t id, uint32_t index) {
  if (RefuseReadOnly(sheet)) return false;  // read-only mode: nothing of the table changes (it changed half)
  const Table* t = TableById(sheet, id);
  if (!t) return false;
  const Rect d = t->Data();
  if (d.r0 > d.r1 || index >= d.Rows()) return false;
  const uint32_t r = d.r0 + index;
  if (d.Rows() == 1) {
    if (!t->header) return false;  // a table without headers keeps a row
    if (!t->totals) {
      // the last row: Excel keeps the table with no data rows, and its empty row stays on the sheet (nothing below
      // moves up). The table is its header row then; typing below it adds a row again.
      if (Refuse(sheet, nullptr, Allow::Never)) return false;
      const Rect row{r, t->range.c0, r, t->range.c1}, old = t->range;
      ClearContents(sheet, row);
      NoteStructural();
      Rect nr = old;
      nr.r1 = nr.r0;
      MutTable(sheet, id)->range = nr;
      if (S(sheet)->autofilter->on && S(sheet)->autofilter->range == old) M(sheet)->autofilter.mut().range = nr;
      RebuildAll();
      changes_.push_back(CellChange{sheet, old, kSrcUser});
      wb_.dirty = true;
      return true;
    }
  }
  DeleteCells(sheet, Rect{r, t->range.c0, r, t->range.c1}, true);
  return true;
}

bool Document::TableInsertColumn(uint32_t sheet, uint32_t id, int64_t pos) {
  if (RefuseReadOnly(sheet)) return false;  // read-only mode: nothing of the table changes (it changed half)
  const Table* t = TableById(sheet, id);
  if (!t) return false;
  const Rect old = t->range;
  const uint32_t cols = static_cast<uint32_t>(old.Cols());
  const uint32_t at = pos < 0 || pos >= static_cast<int64_t>(cols) ? old.c1 + 1 : old.c0 + static_cast<uint32_t>(pos);
  const bool inside = at <= old.c1;
  const uint32_t index = at - old.c0;
  if (inside && index > 0) {
    // the new column's entry goes in first, so names and totals stay with their columns while the cells shift (the
    // shift sizes the list to the table's new width from its end)
    NoteStructural();
    auto& v = MutTable(sheet, id)->columns;
    if (v.size() < cols) v.resize(cols);
    v.insert(v.begin() + index, model::TableColumn{});
  }
  InsertCells(sheet, Rect{old.r0, at, old.r1, at}, false);
  const Table* t2 = TableById(sheet, id);
  if (!t2) return false;
  if (inside && index > 0) {
    if (t2->range == old) {  // refused (protection, data pushed off the sheet): the list as it was
      auto& v = MutTable(sheet, id)->columns;
      v.erase(v.begin() + index);
      return false;
    }
  } else if (inside) {
    // at the left edge the cells' shift moved the whole table right: the column made at its left is its first one,
    // as Excel's ListColumns.Add(1) makes it
    Rect moved = old;
    moved.c0 += 1;
    moved.c1 += 1;
    if (!(t2->range == moved)) return false;
    NoteStructural();
    Table* m = MutTable(sheet, id);
    Rect nr = moved;
    nr.c0 = at;
    m->range = nr;
    m->columns.resize(moved.Cols());
    m->columns.insert(m->columns.begin(), model::TableColumn{});
    if (S(sheet)->autofilter->on && S(sheet)->autofilter->range == moved) M(sheet)->autofilter.mut().range = nr;
    RebuildAll();
    changes_.push_back(CellChange{sheet, nr, kSrcUser});
    wb_.dirty = true;
  } else {
    Rect nr = t2->range;
    nr.c1 = at;
    ResizeTable(sheet, id, nr);
  }
  SyncTableHeaders(sheet);
  return true;
}

bool Document::TableDeleteColumn(uint32_t sheet, uint32_t id, uint32_t index) {
  const Table* t = TableById(sheet, id);
  if (!t || index >= t->range.Cols() || t->range.Cols() <= 1) return false;
  if (Refuse(sheet, nullptr, Allow::Never)) return false;
  const Table before = *t;
  const uint32_t c = before.range.c0 + index;
  // the shift takes its entry out, so the other columns keep their names and totals, and makes what points at it
  // #REF!, as every deletion of a table column does (ShiftSheet)
  NoteStructural();
  {
    auto& v = MutTable(sheet, id)->columns;
    if (v.size() < before.range.Cols()) v.resize(before.range.Cols());
  }
  DeleteCells(sheet, Rect{before.range.r0, c, before.range.r1, c}, false);
  SyncTableHeaders(sheet);
  return true;
}

}  // namespace yc::engine
