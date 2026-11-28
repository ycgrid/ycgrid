// SPDX-License-Identifier: Apache-2.0
// document.cpp - cell contents, formula registration, recalculation, transactions and undo
#include "engine/document.h"
#include "engine/external.h"
#include "core/i18n.h"

#include "core/parallel.h"
#include "engine/snapshot.h"
#include "engine/tile_iter.h"
#include "formula/datetime.h"
#include "formula/numfmt.h"

#include <algorithm>
#include <tuple>
#include <set>
#include <chrono>
#include <cctype>
#include <thread>
#include <cmath>
#include <exception>

namespace yc::engine {

using model::Sheet;

// ---------------------------------------------------------------------------
// evaluation host: connects Document to formula::Evaluator
// ---------------------------------------------------------------------------


// the cached values of another workbook's sheet inside a range, row by row
void ForEachExternal(const model::Workbook& wb, int32_t sheet, const Rect& rect, const std::function<bool(uint32_t, uint32_t, const Value&)>& fn) {
  const size_t li = ExternalLinkOf(sheet), si = ExternalSheetOf(sheet);
  const auto& links = *wb.external_links;
  if (li >= links.size() || si >= links[li].values.size()) return;
  for (const auto& [k, v] : links[li].values[si]) {  // ordered by row, then column
    const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
    if (r < rect.r0 || r > rect.r1 || c < rect.c0 || c > rect.c1) continue;
    if (!fn(r, c, v)) return;
  }
}

class CalcHost final : public formula::EvalHost {
 public:
  // parallel: used by a worker thread of a recalculation wave. It only reads the model, and instead of throwing
  // (exceptions serialize across threads on Windows) it flags the formula: blocked() = it read a still-pending cell,
  // serial() = it called a function with side effects (Python UDF, stream). Flagged results are thrown away.
  explicit CalcHost(Document& d, bool parallel = false) : d_(d), parallel_(parallel) {}
  void ResetFlags() { blocked_ = serial_ = false; }
  // while set: the cells the formula being calculated reads (a name, OFFSET, INDIRECT: references no index holds)
  std::vector<std::pair<int32_t, core::Rect>>* record = nullptr;
  bool blocked() const { return blocked_; }
  bool serial() const { return serial_; }
  // the serial pass: a formula that reads its own cell is a circular reference, found without an exception (one a cell
  // made a million formulas reading themselves take 15 s: =G1+1 in G1 filled down, basics seq444742). From the read
  // on, nothing else is read or called, as the throw it replaces stopped the formula there (self_hit()).
  bool soft_self = false;
  // an array entered with Ctrl+Shift+Enter that reads a circular reference: the cells the cycle blocks read as a
  // value nothing calculates to (#BLOCKED!), so each of its values that reads one shows 0 and the others are calculated
  bool cycle_as_unset = false;
  static const Value& Unset() {
    static const Value v = Value::Error(core::Err::Blocked);
    return v;
  }
  bool self_hit() const { return self_hit_; }
  void ResetSelf() { self_hit_ = false; }

  const Value& Cell(int32_t sheet, uint32_t r, uint32_t c) override {
    static const Value kEmpty;
    if (IsExternalSheet(sheet)) return ExternalValue(d_.wb_, sheet, r, c);
    const uint64_t k = Key(static_cast<uint32_t>(sheet), r, c);
    if (self_hit_) return kEmpty;
    const bool stale = d_.StaleAt(sheet, r, c);
    if (d_.IsPending(k) && !d_.cycle_break_.count(k)) {
      if (!stale) {
        if (soft_self && !parallel_ && k == d_.current_cell_) {
          self_hit_ = true;
          return kEmpty;
        }
        if (!parallel_) throw formula::NeedCell{sheet, r, c};
        blocked_ = true;
        return kEmpty;
      }
      if (d_.only_sheet_ >= 0) d_.stale_hit_ = true;  // read as it shows: the reader waits for the next calculation too
    }
    // a cell of a circular reference (or one that reads it) that this calculation doesn't calculate: its readers aren't
    // calculated either (Excel)
    uint64_t ca = k;
    if (!stale && !d_.cycle_block_.empty() && (d_.cycle_block_.count(k) || d_.CycleSpillAt(static_cast<uint32_t>(sheet), r, c, &ca))) {
      if (cycle_as_unset) return Unset();
      // (a cell of a circular array waits for its anchor)
      if (!parallel_) throw formula::NeedCell{static_cast<int32_t>(KeySheet(ca)), KeyRow(ca), KeyCol(ca)};
      blocked_ = true;
      return kEmpty;
    }
    uint64_t a;
    if (!stale && !d_.pending_spills_.empty() && d_.PendingSpillAt(static_cast<uint32_t>(sheet), r, c, &a)) {
      if (!parallel_) throw formula::NeedCell{static_cast<int32_t>(KeySheet(a)), KeyRow(a), KeyCol(a)};
      blocked_ = true;
      return kEmpty;
    }
    if (record) record->emplace_back(sheet, Rect::Cell(r, c));
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    return s ? s->GetValue(r, c) : kEmpty;
  }

  int TableRef(int32_t sheet, const formula::Node& n, core::CellPos origin, formula::RangeRef* out) override {
    uint32_t ts = static_cast<uint32_t>(sheet);
    Rect r;
    const int e = d_.ResolveStructRef(static_cast<uint32_t>(sheet), n, origin, &ts, &r);
    if (e) return e;
    out->sheet = static_cast<int32_t>(ts);
    out->rect = r;
    return 0;
  }

  bool PivotData(int32_t sheet, uint32_t r, uint32_t c, const std::string& data_field, const std::vector<std::pair<std::string, std::string>>& pairs,
                 Value* out) override {
    // reads the whole source: never on a worker thread
    if (parallel_) {
      serial_ = true;
      return false;
    }
    return d_.PivotLookupAt(static_cast<uint32_t>(sheet), r, c, data_field, pairs, out);
  }


  bool SpillRange(int32_t sheet, uint32_t r, uint32_t c, Rect* out) override {
    if (IsExternalSheet(sheet) || self_hit_) return false;
    if (record) record->emplace_back(sheet, Rect::Cell(r, c));
    const uint64_t k = Key(static_cast<uint32_t>(sheet), r, c);
    if (!d_.StaleAt(sheet, r, c) && ((d_.IsPending(k) && !d_.cycle_break_.count(k)) || (!d_.cycle_block_.empty() && d_.cycle_block_.count(k)))) {
      if (!parallel_) throw formula::NeedCell{sheet, r, c};
      blocked_ = true;
      return false;
    }
    auto it = d_.spill_anchor_.find(k);
    if (it != d_.spill_anchor_.end()) {
      *out = it->second;
      return true;
    }
    // a formula whose result is a single value (e.g. a range that shrank to one cell): the cell itself
    if (!HasFormula(sheet, r, c)) return false;
    *out = Rect::Cell(r, c);
    return true;
  }

  void ForEachCell(int32_t sheet, const Rect& rect_in, const std::function<bool(uint32_t, uint32_t, const Value&)>& fn) override {
    if (IsExternalSheet(sheet)) {
      ForEachExternal(d_.wb_, sheet, rect_in, fn);
      return;
    }
    if (self_hit_) return;
    if (record) record->emplace_back(sheet, rect_in);
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    if (!s || !s->has_used) return;
    Rect rect = rect_in;
    // clamp to the used range (entire-column references)
    rect.r1 = std::min(rect.r1, s->used.r1);
    rect.c1 = std::min(rect.c1, s->used.c1);
    if (rect.r0 > rect.r1 || rect.c0 > rect.c1) return;
    uint64_t first = 0;
    // the part of the range this calculation calculates (Worksheet.Calculate / Range.Calculate read the rest as it shows)
    Rect live = rect;
    const bool any_live = d_.LivePart(sheet, &live);
    const bool waits = any_live && (d_.PendingInRect(static_cast<uint32_t>(sheet), live, &first) ||
                                    (!d_.pending_spills_.empty() && d_.PendingSpillIn(static_cast<uint32_t>(sheet), live, &first)));
    const bool cycle = any_live && !waits && d_.CycleBlockedIn(static_cast<uint32_t>(sheet), live, &first);
    if (cycle && cycle_as_unset && !parallel_) {
      const uint32_t sid = static_cast<uint32_t>(sheet);
      ForEachValueRowMajor(*s, rect, [&](uint32_t rr, uint32_t cc, const Value& v) {
        uint64_t a;
        const bool blocked = d_.cycle_block_.count(Key(sid, rr, cc)) || d_.CycleSpillAt(sid, rr, cc, &a);
        return fn(rr, cc, blocked ? Unset() : v);
      });
      return;
    }
    if (waits || cycle) {
      if (soft_self && !parallel_ && first == d_.current_cell_) {
        self_hit_ = true;  // the cell it waits for is its own
        return;
      }
      if (!parallel_) throw formula::NeedCell{static_cast<int32_t>(KeySheet(first)), KeyRow(first), KeyCol(first)};
      blocked_ = true;
      return;
    }
    if (d_.only_sheet_ >= 0 && d_.PendingInRect(static_cast<uint32_t>(sheet), rect, &first)) d_.stale_hit_ = true;
    ForEachValueRowMajor(*s, rect, fn);
  }

  uint32_t RowLimit(int32_t sheet) override {
    if (IsExternalSheet(sheet)) return 1048576;
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    return s ? s->row_limit : 0;
  }
  Rect UsedRect(int32_t sheet) override {
    if (IsExternalSheet(sheet)) {
      bool any = false;
      return ExternalUsed(d_.wb_, sheet, &any);
    }
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    return s && s->has_used ? s->used : Rect{0, 0, 0, 0};
  }
  uint32_t LastRowIn(int32_t sheet, uint32_t c0, uint32_t c1) override {
    if (IsExternalSheet(sheet)) return UsedRect(sheet).r1;
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    if (!s || !s->has_used) return 0;
    if (c0 <= s->used.c0 && c1 >= s->used.c1) return s->used.r1;  // the columns of the whole used range
    // the highest row band with a value or a formula in the columns, then its last such row
    const uint32_t ct0 = c0 / model::kTileCols, ct1 = std::min<uint32_t>(c1, core::kMaxCols - 1) / model::kTileCols;
    uint32_t best = 0;
    bool found = false;
    const uint32_t top = std::min<uint32_t>(s->used.r1 / model::kTileRows, std::max(s->values.RowTileCapacity(), s->meta.RowTileCapacity()));
    for (uint32_t rt = top + 1; rt-- > 0 && !found;) {
      auto scan = [&](uint32_t ct, auto occupied) {
        const uint32_t cc0 = std::max(c0, ct * model::kTileCols), cc1 = std::min(c1, ct * model::kTileCols + model::kTileCols - 1);
        for (uint32_t c = cc0; c <= cc1; ++c) {
          for (uint32_t i = model::kTileRows; i-- > 0;) {
            const uint32_t row = rt * model::kTileRows + i;
            if (occupied(model::TileIndex(row, c))) {
              best = std::max(best, row);
              found = true;
              break;
            }
          }
        }
      };
      if (s->values.HasRowBand(rt)) {
        s->values.ForEachInBand(rt, ct0, ct1, [&](uint32_t ct, const model::ValueTile& t) {
          if (t.count) scan(ct, [&](uint32_t idx) { return t.Occupied(idx); });
        });
      }
      if (s->meta.HasRowBand(rt)) {
        s->meta.ForEachInBand(rt, ct0, ct1, [&](uint32_t ct, const model::MetaTile& t) {
          if (t.formula_count) scan(ct, [&](uint32_t idx) { return t.formula[idx] != 0; });
        });
      }
    }
    return found ? best : 0;
  }
  int32_t FindSheet(std::string_view name) override { return d_.FindSheet(name); }
  std::string SheetName(int32_t sheet) override { return d_.SheetName(sheet); }
  int32_t SheetIndex(int32_t sheet) override { return d_.wb_.IndexOfId(static_cast<uint32_t>(sheet)) + 1; }
  int32_t SheetCount() override { return static_cast<int32_t>(d_.wb_.sheets.size()); }
  int32_t SheetIdAt(int32_t index) override {
    if (index < 1 || index > static_cast<int32_t>(d_.wb_.sheets.size())) return -1;
    return static_cast<int32_t>(d_.wb_.sheets[static_cast<size_t>(index - 1)]->id);
  }

  bool ResolveName(std::string_view name, int32_t current_sheet, core::CellPos origin, formula::EV* out) override {
    const model::NameDef* found = nullptr;
    for (const auto& n : *d_.wb_.names) {
      if (!IEq(n.name, name)) continue;
      if (n.sheet_scope == current_sheet) {
        found = &n;
        break;
      }
      if (n.sheet_scope < 0 && !found) found = &n;
    }
    if (!found) {
      // a table name alone is its data body (=SUM(Table1))
      uint32_t ts = 0;
      if (const model::Table* t = model::FindTable(d_.wb_, name, &ts)) {
        Rect r = t->Data();
        if (r.r0 > r.r1) r.r1 = r.r0;
        *out = formula::EV::R(formula::RangeRef{static_cast<int32_t>(ts), r});
        return true;
      }
      return false;
    }
    if (++name_depth_ > 32) {
      --name_depth_;
      *out = formula::EV::Err(core::Err::Ref);
      return true;
    }
    std::string_view text = found->refers_to;
    if (!text.empty() && text[0] == '=') text.remove_prefix(1);
    // relative references are kept relative to A1 (Excel's relative names): they reach the same offsets from the cell
    // that uses the name
    auto pr = std::make_shared<formula::ParseResult>(formula::Parse(text, core::CellPos{0, 0}, &d_));
    if (!pr->ok) {
      --name_depth_;
      *out = formula::EV::Err(core::Err::Name);
      return true;
    }
    if (formula::HasRelativeRef(pr->root)) formula::RelativeTo(pr->root, origin);
    formula::Evaluator ev(*this, current_sheet, origin);
    ev.owner = pr;  // a LAMBDA defined by the name points into this tree
    *out = ev.Eval(pr->root);
    --name_depth_;
    return true;
  }

  bool NameUnion(std::string_view name, int32_t current_sheet, core::CellPos origin, formula::Node* out) override {
    const model::NameDef* found = nullptr;
    for (const auto& n : *d_.wb_.names) {
      if (!IEq(n.name, name)) continue;
      if (n.sheet_scope == current_sheet) {
        found = &n;
        break;
      }
      if (n.sheet_scope < 0 && !found) found = &n;
    }
    if (!found) return false;
    std::string_view text = found->refers_to;
    if (!text.empty() && text[0] == '=') text.remove_prefix(1);
    if (text.size() < 5 || text.compare(0, 2, "((") != 0) return false;  // only a kept union (Document::DefineName)
    formula::ParseResult pr = formula::Parse(text, core::CellPos{0, 0}, &d_);
    if (!pr.ok) return false;
    const formula::Node* u = &pr.root;
    while (u->kind == formula::NK::Paren && u->kids.size() == 1) u = &u->kids[0];
    if (u->kind != formula::NK::Binary || u->op != ',') return false;
    if (formula::HasRelativeRef(pr.root)) formula::RelativeTo(pr.root, origin);
    *out = std::move(pr.root);
    return true;
  }

  formula::EV CallUdf(const std::string& name, const std::vector<formula::EV>& args, int32_t sheet, core::CellPos origin) override {
    if (self_hit_) return formula::EV::Err(core::Err::Calc);
    if (parallel_) {
      serial_ = true;
      return formula::EV::Err(core::Err::Calc);
    }
    if (name == "PY" && Document::python_blocked && d_.provider) {
      // a host runs without the project (the Immediate window): the workbook's own =PY code still doesn't run
      d_.udf_no_host_cells_.push_back(d_.current_cell_);
      return formula::EV::Err(core::Err::Blocked);
    }
    if (!d_.provider) {
      // calculated again when the host is ready (RecalcUdfCells): a list, or past a million (a workbook of Python
      // formulas and no host) a scan for #NAME? then
      if (!d_.udf_without_host_) {
        if (d_.udf_no_host_cells_.size() < 1'000'000) {
          d_.udf_no_host_cells_.push_back(d_.current_cell_);
        } else {
          d_.udf_without_host_ = true;
          std::vector<uint64_t>().swap(d_.udf_no_host_cells_);
        }
      }
      if (name == "PY" && Document::python_blocked) return formula::EV::Err(core::Err::Blocked);
      if (name != "PY" || !Document::on_python_needed) return formula::EV::Err(core::Err::Name);
      if (d_.udf_requested_.insert("PY:host").second) Document::on_python_needed();
      return formula::EV::S(Value::Pending());  // computed when the host is ready
    }
    std::vector<std::shared_ptr<formula::Array>> arrays;
    std::string key = name + "(";
    formula::Evaluator ev(*this, sheet, origin);
    for (const auto& a : args) {
      auto arr = ev.ToArray(a);
      if (!arr) arr = std::make_shared<formula::Array>();
      key += SerializeArray(*arr);
      key += ';';
      arrays.push_back(std::move(arr));
    }
    key += ")";
    // a function that reads its calling cell (Application.Caller / ThisCell) is called for each cell, as Excel calls it;
    // the others once for every cell of the same arguments (each gets the first cell as its caller)
    if (!d_.per_cell_udfs.empty() && d_.per_cell_udfs.count(name)) {
      key += "@" + std::to_string(KeySheet(d_.current_cell_)) + ":" + std::to_string(KeyRow(d_.current_cell_)) + ":" + std::to_string(KeyCol(d_.current_cell_));
    }
    d_.udf_cells_[key].insert(d_.current_cell_);
    if (auto ai = d_.udf_arrays_.find(key); ai != d_.udf_arrays_.end()) return formula::EV::A(ai->second);
    auto it = d_.udf_cache_.find(key);
    if (it != d_.udf_cache_.end()) return formula::EV::S(it->second);
    if (d_.udf_requested_.insert(key).second) {
      d_.provider->RequestUdf(key, name, arrays, KeySheet(d_.current_cell_), KeyRow(d_.current_cell_), KeyCol(d_.current_cell_));
    }
    if (auto sa = d_.udf_stale_arrays_.find(key); sa != d_.udf_stale_arrays_.end()) return formula::EV::A(sa->second);
    if (auto sv = d_.udf_stale_.find(key); sv != d_.udf_stale_.end()) return formula::EV::S(sv->second);  // until it answers
    return formula::EV::S(Value::Pending());
  }

  formula::EV WebFetch(const std::string& url) override {
    if (self_hit_) return formula::EV::Err(core::Err::Calc);
    if (parallel_) {
      serial_ = true;
      return formula::EV::Err(core::Err::Calc);
    }
    if (!d_.net) return formula::EV::Err(core::Err::Value);
    const std::string key = "WEB|" + url;
    d_.udf_cells_[key].insert(d_.current_cell_);
    auto it = d_.udf_cache_.find(key);
    if (it != d_.udf_cache_.end()) return formula::EV::S(it->second);
    if (d_.udf_requested_.insert(key).second) d_.net->Fetch(key, url);
    if (auto sv = d_.udf_stale_.find(key); sv != d_.udf_stale_.end()) return formula::EV::S(sv->second);
    return formula::EV::S(Value::Pending());
  }

  formula::EV Stream(const std::string& name, const std::vector<formula::EV>& args, int32_t, core::CellPos) override {
    if (self_hit_) return formula::EV::Err(core::Err::Calc);
    if (parallel_) {
      serial_ = true;
      return formula::EV::Err(core::Err::Calc);
    }
    std::string topic = name;
    std::vector<Value> vals;
    for (const auto& a : args) {
      topic += '|';
      topic += formula::Evaluator::ToText(a.s);
      vals.push_back(a.s);
    }
    auto& t = d_.topics_[topic];
    t.cells.insert(d_.current_cell_);
    if (!t.subscribed && d_.provider) {
      t.subscribed = true;
      d_.provider->Subscribe(topic, vals);
    }
    if (t.value.IsEmpty()) return formula::EV::S(d_.provider ? Value::Pending() : Value::Error(core::Err::Connect));
    return formula::EV::S(t.value);
  }

  bool RowHidden(int32_t sheet, uint32_t r) override {
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    return s && s->rows->IsHidden(r);
  }
  bool RowFiltered(int32_t sheet, uint32_t r) override {
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    return s && FilterHidRow(*s, r) && s->rows->IsHidden(r);  // AutoFilter (the sheet's or a table's) or Advanced Filter
  }
  std::string FormulaText(int32_t sheet, uint32_t r, uint32_t c) override { return d_.GetFormulaA1(static_cast<uint32_t>(sheet), r, c); }
  CellLook LookOf(int32_t sheet, uint32_t r, uint32_t c) override {
    CellLook look;
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    if (!s) return look;
    const model::Style& st = d_.wb_.styles->Get(s->GetStyle(r, c));
    if (st.numfmt) look.numfmt = std::string(d_.wb_.styles->Str(st.numfmt));
    look.halign = static_cast<uint8_t>(st.halign);
    look.locked = st.locked;
    const int32_t px = s->cols->Size(c);
    look.width = px <= 5 ? 0.0 : (px - 5) / 7.0;  // pixels to characters, as the column header's tip
    return look;
  }
  bool HasFormula(int32_t sheet, uint32_t r, uint32_t c) override {
    const Sheet* s = d_.S(static_cast<uint32_t>(sheet));
    return s && s->GetFormula(r, c) != 0;
  }

 private:
  static bool IEq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      char x = a[i], y = b[i];
      if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
      if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
      if (x != y) return false;
    }
    return true;
  }
  static std::string SerializeArray(const formula::Array& a) {
    std::string s = std::to_string(a.rows) + "x" + std::to_string(a.cols) + ":";
    for (const Value& v : a.v) {
      s += static_cast<char>('0' + static_cast<int>(v.type()));
      if (v.IsString()) s += v.str();
      else if (v.IsNumber()) s += std::to_string(v.number());
      else if (v.IsBool()) s += v.boolean() ? '1' : '0';
      else if (v.IsDecimal()) s += v.dec_mantissa().ToString() + "e" + std::to_string(v.dec_scale());
      else if (v.IsError()) s += std::to_string(static_cast<int>(v.error()));
      s += '\x1f';
    }
    return s;
  }

  Document& d_;
  bool parallel_ = false;
  bool blocked_ = false, serial_ = false;
  bool self_hit_ = false;  // the serial pass: the formula read its own cell (soft_self)
  int name_depth_ = 0;
};

// ---------------------------------------------------------------------------

Document::Document() : Document(model::Workbook::NewDefault()) {}

Document::Document(model::Workbook wb) : wb_(std::move(wb)) {
  host_ = std::make_unique<CalcHost>(*this);
  Reindex();
  RebuildAll();
  Recalc();
  // PivotTable Options > Refresh data when opening the file
  std::vector<std::pair<uint32_t, uint32_t>> refresh;
  for (const auto& sc : wb_.sheets) {
    for (const auto& p : *sc->pivots) {
      if (p.refresh_on_open) refresh.emplace_back(sc->id, p.id);
    }
  }
  for (const auto& [sheet, id] : refresh) RefreshPivot(sheet, id, nullptr);
  if (!refresh.empty()) wb_.dirty = false;
  UpdatePivotCharts();
  SeedParameterLog();
}

Document::~Document() = default;

void Document::Reindex() {
  id_index_.clear();
  for (size_t i = 0; i < wb_.sheets.size(); ++i) {
    const uint32_t id = wb_.sheets[i]->id;
    if (id >= id_index_.size()) id_index_.resize(id + 1, -1);
    id_index_[id] = static_cast<int32_t>(i);
  }
  RebuildSpillIndex();
}

const Sheet* Document::S(uint32_t id) const {
  if (id >= id_index_.size() || id_index_[id] < 0) return nullptr;
  return &wb_.sheets[static_cast<size_t>(id_index_[id])].get();
}
Sheet* Document::M(uint32_t id) {
  if (id >= id_index_.size() || id_index_[id] < 0) return nullptr;
  return &wb_.sheets[static_cast<size_t>(id_index_[id])].mut();
}

int32_t Document::FindSheet(std::string_view name) const {
  const int32_t i = wb_.FindSheet(name);
  if (i >= 0) return static_cast<int32_t>(wb_.sheets[static_cast<size_t>(i)]->id);
  // another workbook's sheet ([Book2.ycg]Sheet1): typing a reference to it creates the link (the link table isn't undo state)
  if (name.find('[') != std::string_view::npos) return FindExternalSheet(const_cast<model::Workbook&>(wb_), name, true);
  return -1;
}
std::string Document::SheetName(int32_t id) const {
  if (IsExternalSheet(id)) return ExternalSheetName(wb_, id);
  const Sheet* s = id >= 0 ? S(static_cast<uint32_t>(id)) : nullptr;
  return s ? s->name : std::string();
}

// ---- Formula registration ----

uint32_t Document::InternFormula(formula::Node root) {
  std::string key = formula::SerializeKey(root);
  auto it = formula_ids_.find(key);
  if (it != formula_ids_.end()) return it->second;
  auto def = std::make_shared<formula::CompiledFormula>();
  def->root = std::move(root);
  def->flags = formula::ComputeFlags(def->root);
  def->key = key;
  const uint32_t id = wb_.formulas.mut().Append(def);
  formula_ids_.emplace(std::move(key), id);
  return id;
}

const formula::CompiledFormula* Document::Formula(uint32_t id) const {
  return static_cast<const formula::CompiledFormula*>(wb_.formulas->Get(id));
}

const Document::RefInfo& Document::RefInfoOf(uint32_t id, const formula::CompiledFormula* f) {
  if (id == last_ref_id_ && last_ref_info_) return *last_ref_info_;  // a column of one formula: the same id in a row
  auto info = ref_info_.find(id);
  if (info == ref_info_.end()) {
    RefInfo ri;
    formula::CollectRefSpecs(f->root, &ri.refs);
    ri.table = f->key.find("T'") != std::string::npos;
    info = ref_info_.emplace(id, std::move(ri)).first;
  }
  last_ref_id_ = id;
  last_ref_info_ = &info->second;  // a node's address survives rehashing, and entries are never erased
  return info->second;
}

void Document::RegisterFormulaCell(uint32_t sheet, uint32_t r, uint32_t c, uint32_t id, bool fresh) {
  const uint64_t k = Key(sheet, r, c);
  if (!fresh) {
    deps_.Remove(k);
    volatile_.erase(k);
  }
  const formula::CompiledFormula* f = Formula(id);
  if (!f) return;
  const RefInfo& info = RefInfoOf(id, f);
  const core::CellPos at{r, c};
  for (const auto& spec : info.refs) {
    if (spec.lost_sheet) continue;  // a deleted sheet's cells: nothing to wait for (it is #REF!)
    formula::ResolvedRef ref;
    if (formula::ResolveRef(spec, at, &ref)) deps_.Add(k, ref.sheet < 0 ? sheet : static_cast<uint32_t>(ref.sheet), ref.rect);
  }
  if (info.table) AddTableDeps(k, sheet, at, f->root);
  if (f->flags & (model::FormulaDef::kVolatile | model::FormulaDef::kDynamicRef)) volatile_.insert(k);
  if (register_pending_) {
    AddPending(k);
    if (!fresh) fresh_formulas_.insert(k);  // entered, not loaded: shows 0 if a circular reference keeps it uncalculated
  }
}

void Document::ShowEnteredOnCalcOff(uint32_t sheet, uint32_t r, uint32_t c) {
  // a formula entered on a sheet whose calculation is off shows 0 until the sheet calculates again (Excel 16). Only
  // an entered one: a formula Undo puts back, or one moved, shifted or copied, keeps the value it had (it was done in
  // RegisterFormulaCell, where every path meets, and Undo of cells inserted there showed 0; doc_fuzz --gaps 700022)
  if (calc_off_.empty() || !calc_off_.count(sheet)) return;
  if (Sheet* s = M(sheet)) s->SetValue(r, c, Value::Number(0));
  MarkChanged(sheet, Rect::Cell(r, c), kSrcCalc);
}

void Document::SetDate1904(bool on) {
  if (wb_.date1904 == on) return;
  Begin(TR("Use 1904 date system"));
  wb_.date1904 = on;
  formula::SetDate1904(on);
  NoteTouched();
  RecalcAll();  // DATE, TODAY and the rest give the other system's numbers
  Commit();
}

void Document::SetForceFullCalculation(bool on) {
  if (wb_.force_full_calc == on) return;
  Begin(TR("Force full calculation"));
  wb_.force_full_calc = on;
  NoteTouched();
  Commit();
}

bool Document::RoundShown(uint32_t sheet, uint32_t r, uint32_t c, Value* v) const {
  if (!v->IsNumeric() || v->IsDecimal()) return false;
  const Sheet* s = S(sheet);
  if (!s) return false;
  double out = 0;
  if (!formula::RoundAsShown(v->AsDouble(), wb_.styles->NumberFormat(wb_.styles->Get(s->GetStyle(r, c))), &out) || out == v->AsDouble()) return false;
  *v = Value::Number(out);
  return true;
}

void Document::RoundShownIn(const std::vector<CellChange>& regions) {
  for (const CellChange& rg : regions) {
    Sheet* s = M(rg.sheet);
    if (!s || !s->has_used) continue;
    Rect rect = rg.rect;
    rect.r1 = std::min(rect.r1, s->used.r1);
    rect.c1 = std::min(rect.c1, s->used.c1);
    if (rect.r0 > rect.r1 || rect.c0 > rect.c1) continue;
    std::vector<std::tuple<uint32_t, uint32_t, Value>> rounded;
    ForEachValueRowMajor(*s, rect, [&](uint32_t r, uint32_t c, const Value& v) {
      Value x = v;
      if (RoundShown(rg.sheet, r, c, &x)) rounded.emplace_back(r, c, x);
      return true;
    });
    for (auto& [r, c, x] : rounded) {
      s->SetValue(r, c, x);
      MarkChanged(rg.sheet, Rect::Cell(r, c), kSrcCalc);
    }
  }
}

void Document::SetPrecisionAsDisplayed(bool on) {
  if (wb_.precision_as_displayed == on) return;
  Begin(TR("Set precision as displayed"));
  wb_.precision_as_displayed = on;
  NoteTouched();
  if (on) {
    // every number now, constants and calculated ones (what reads them is calculated again)
    std::vector<CellChange> all;
    for (const auto& sc : wb_.sheets) {
      if (sc->has_used) all.push_back(CellChange{sc->id, sc->used, kSrcCalc});
    }
    for (const CellChange& rg : all) NoteRegion(rg.sheet, rg.rect);
    RoundShownIn(all);
  } else {
    // off: the constants keep the digits they were rounded to, and every formula is calculated again at full precision
    // (Excel 16, measured: =1/3 in 0.00 reads 0.333... again, =DATE(2024,1,31) in 0.00E+00 45322; doc_fuzz --gaps
    // 737276). Left alone they kept their rounded values until something calculated them
    RecalcAll();
  }
  Commit();
}

void Document::SetProtectionEnable(uint32_t sheet, int which, bool on) {
  Sheet* s = M(sheet);
  if (!s || ProtectionEnable(sheet, which) == on) return;
  auto& p = s->protection.mut();
  (which == 0 ? p.enable_autofilter : which == 1 ? p.enable_outlining : p.enable_pivot) = on;
}

bool Document::ProtectionEnable(uint32_t sheet, int which) const {
  const Sheet* s = S(sheet);
  if (!s) return false;
  const auto& p = *s->protection;
  return which == 0 ? p.enable_autofilter : which == 1 ? p.enable_outlining : p.enable_pivot;
}

void Document::SetSheetCalculation(uint32_t sheet, bool on) {
  if (!S(sheet) || on == SheetCalculation(sheet)) return;
  if (!on) {
    // in automatic calculation what waits is calculated first, as Excel had calculated it already
    if (auto_calc) Recalc();
    calc_off_.insert(sheet);
    std::vector<uint64_t> waiting;
    PendingIn(sheet, Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1}, pending_.size(), &waiting);
    for (const uint64_t k : waiting) ErasePending(k);  // they keep the values they show
    return;
  }
  calc_off_.erase(sheet);
  MarkDirty(sheet, Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1});
  RecalcSheet(sheet);  // now, in manual calculation too (what reads it elsewhere follows as an edit's readers do)
}

std::vector<std::pair<std::string, int64_t>> Document::DebugCounts() const {
  size_t formulas = 0, row_styles = 0, col_styles = 0;
  for (const auto& sc : wb_.sheets) {
    sc->meta.ForEach([&](uint32_t, uint32_t, const model::MetaTile& t) { formulas += t.formula_count; });
    row_styles += sc->row_styles->size();
    col_styles += sc->col_styles->size();
  }
  return {{"formula_cells", static_cast<int64_t>(formulas)},
          {"row_styles", static_cast<int64_t>(row_styles)},
          {"col_styles", static_cast<int64_t>(col_styles)},
          {"deps_live", static_cast<int64_t>(deps_.live())},
          {"deps_stale", static_cast<int64_t>(deps_.stale())},
          {"pending", static_cast<int64_t>(pending_.size())},
          {"volatile", static_cast<int64_t>(volatile_.size())},
          {"spill_anchors", static_cast<int64_t>(spill_anchor_.size())},
          {"spill_blocked", static_cast<int64_t>(spill_blocked_.size())},
          {"circular", static_cast<int64_t>(circular_cells.size())},
          {"formula_ids", static_cast<int64_t>(formula_ids_.size())},
          {"undo_depth", static_cast<int64_t>(UndoDepth())}};
}

void Document::UnregisterFormulaCell(uint32_t sheet, uint32_t r, uint32_t c) {
  const uint64_t k = Key(sheet, r, c);
  deps_.Remove(k);
  volatile_.erase(k);
  ErasePending(k);
  if (spill_hook_ && spill_anchor_.count(k)) ClearSpill(k, nullptr);  // the formula is gone: so are its spilled values
}

void Document::RebuildAll(bool mark_pending) {
  register_pending_ = mark_pending;
  formula_ids_.clear();
  const auto& ft = *wb_.formulas;
  for (uint32_t id = 1; id <= ft.size(); ++id) {
    const auto* f = static_cast<const formula::CompiledFormula*>(ft.Get(id));
    if (f && !f->key.empty()) formula_ids_.emplace(f->key, id);
  }
  deps_.Clear();
  ClearPending();
  volatile_.clear();
  RebuildSpillIndex();
  size_t n = 0;  // room for every formula first: the tables grew step by step through millions of them
  for (const auto& sc : wb_.sheets) {
    sc.get().meta.ForEach([&](uint32_t, uint32_t, const model::MetaTile& t) { n += t.formula_count; });
  }
  deps_.Reserve(n);
  if (mark_pending) pending_.reserve(n);
  for (const auto& sc : wb_.sheets) {
    const Sheet& s = sc.get();
    s.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        if (!t.formula[idx]) continue;
        RegisterFormulaCell(s.id, rt * model::kTileRows + idx % model::kTileRows, ct * model::kTileCols + idx / model::kTileRows, t.formula[idx], true);
      }
    });
  }
  register_pending_ = true;
}

// ---- pending ----

void Document::AddPending(uint64_t key) {
  if (!calc_off_.empty() && calc_off_.count(KeySheet(key))) return;  // Worksheet.EnableCalculation = False
  if (key == storing_) storing_read_itself_ = true;
  if (pending_.insert(key).second) {
    ++pending_tiles_[TileKeyOf(key)];
    ++pending_cols_[(uint64_t{KeySheet(key)} << 32) | KeyCol(key)];
    new_pending_.push_back(key);
    if (!spill_anchor_.empty() && spill_anchor_.count(key)) pending_spills_.insert(key);
  }
}

void Document::ClearPending() {
  if (pending_.size() > 100000) {  // freed on a thread of its own (half a second here with millions)
    std::thread([old = std::move(pending_)]() mutable { old.clear(); }).detach();
    pending_ = {};  // lesson-ok: pending-clear moved to the thread above
  }
  pending_.clear();  // lesson-ok: pending-clear the one place the set is emptied, with its indexes
  pending_tiles_.clear();
  pending_cols_.clear();
  new_pending_.clear();
  pending_spills_.clear();
}

void Document::ErasePending(uint64_t key) {
  if (pending_.erase(key)) {
    auto it = pending_tiles_.find(TileKeyOf(key));
    if (it != pending_tiles_.end() && --it->second == 0) pending_tiles_.erase(it);
    auto ic = pending_cols_.find((uint64_t{KeySheet(key)} << 32) | KeyCol(key));
    if (ic != pending_cols_.end() && --ic->second == 0) pending_cols_.erase(ic);
    if (!pending_spills_.empty()) pending_spills_.erase(key);
  }
}

bool Document::PendingInRect(uint32_t sheet, const Rect& rect, uint64_t* first) const {
  if (pending_.empty()) return false;
  const uint32_t rt0 = rect.r0 / model::kTileRows, rt1 = rect.r1 / model::kTileRows;
  const uint32_t ct0 = rect.c0 / model::kTileCols, ct1 = rect.c1 / model::kTileCols;
  auto scan_tile = [&](uint32_t rt, uint32_t ct) {
    const uint32_t r0 = std::max(rect.r0, rt * model::kTileRows), r1 = std::min(rect.r1, rt * model::kTileRows + model::kTileRows - 1);
    const uint32_t c0 = std::max(rect.c0, ct * model::kTileCols), c1 = std::min(rect.c1, ct * model::kTileCols + model::kTileCols - 1);
    for (uint32_t c = c0; c <= c1; ++c) {
      if (!pending_cols_.count((uint64_t{sheet} << 32) | c)) continue;  // nothing in this column waits
      for (uint32_t r = r0; r <= r1; ++r) {
        const uint64_t k = Key(sheet, r, c);
        if (pending_.count(k) && !cycle_break_.count(k)) {
          *first = k;
          return true;
        }
      }
    }
    return false;
  };
  const uint64_t ntiles = (uint64_t{rt1} - rt0 + 1) * (uint64_t{ct1} - ct0 + 1);
  if (ntiles > pending_tiles_.size()) {
    for (const auto& [tk, n] : pending_tiles_) {
      if ((tk >> 48) != sheet) continue;
      const uint32_t rt = static_cast<uint32_t>((tk >> 16) & 0xFFFFFFFFu), ct = static_cast<uint32_t>(tk & 0xFFFF);
      if (rt < rt0 || rt > rt1 || ct < ct0 || ct > ct1) continue;
      if (scan_tile(rt, ct)) return true;
    }
    return false;
  }
  for (uint32_t rt = rt0; rt <= rt1; ++rt) {
    for (uint32_t ct = ct0; ct <= ct1; ++ct) {
      if (!pending_tiles_.count((uint64_t{sheet} << 48) | (uint64_t{rt} << 16) | ct)) continue;
      if (scan_tile(rt, ct)) return true;
    }
  }
  return false;
}

// ---- Changes ----

void Document::MarkChanged(uint32_t sheet, const Rect& rect, uint8_t src) {
  src = static_cast<uint8_t>(src | extra_src_bits);
  const WritePolicy policy = PolicyFor(src);
  if (policy.input) ++input_seq_;  // iterative calculation: the next recalculation goes on with the cycles
  if (policy.input && !in_tables_) tables_dirty_ = true;
  // Python, live tables, topics and co-authors aren't undone; the user's changes undone with the whole version are
  // dropped so undo can't put old live values back (ADR 0013)
  if (txn_depth_ == 0 && policy.foreign && !(undo_.empty() && redo_.empty())) ForeignWrite();
  // the same cells changed twice before a publish are noted once, but every write still does what a write does: a
  // fill through a spill, a calculation that spilled it again, then a formula typed into the same cell left the
  // array spilled over it (doc_fuzz seed 10592817)
  const bool repeat = !changes_.empty() && changes_.back().sheet == sheet && changes_.back().source == src &&
                      changes_.back().rect == rect;
  if (!repeat) changes_.push_back(CellChange{sheet, rect, src});
  NoteRegion(sheet, rect);
  MarkDependentsOf(sheet, rect);
  if (!spill_anchor_.empty() && policy.input) UnspillTouched(sheet, rect);
  if (policy.input && !syncing_tables_) {
    if (const Sheet* ts = S(sheet); ts && !ts->tables->empty()) {
      for (const auto& t : *ts->tables) {
        if (t.header && rect.Intersects(Rect{t.range.r0, t.range.c0, t.range.r0, t.range.c1})) {
          SyncTableHeaders(sheet);
          break;
        }
      }
    }
  }
  if (!spill_blocked_.empty()) WakeBlockedSpills(sheet, rect);
  wb_.dirty = true;
}

void Document::MarkRowsColsChanged(uint32_t sheet, const Rect& band) {
  // not MarkChanged: it takes the rows / columns for cells written, so every array spilled across them lost its spill
  // record and kept its values as typed values, which then blocked it (Undo of a column width or of hidden rows over
  // a spill showed #SPILL!), and a hide noted the whole rows' cells as changed by the step
  ++input_seq_;
  if (!in_tables_) tables_dirty_ = true;
  changes_.push_back(CellChange{sheet, band, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
  MarkDependentsOf(sheet, band);
  wb_.dirty = true;
}

void Document::MarkDependentsOf(uint32_t sheet, const Rect& rect) {
  if (rect.IsCell()) {
    deps_.Query(sheet, rect.r0, rect.c0, [&](uint64_t k) { AddPending(k); });
  } else {
    deps_.QueryRect(sheet, rect, [&](uint64_t k) { AddPending(k); });
  }
}

std::vector<CellChange> Document::TakeChanges() {
  if (!wb_.parameters->empty() && !changes_.empty()) AuditParameters(changes_);
  std::vector<CellChange> out;
  out.swap(changes_);
  calc_change_cells_ = 0;
  return out;
}

void Document::EnsureRowLimit(uint32_t sheet, uint32_t row) {
  Sheet* s = M(sheet);
  if (!s || row < s->row_limit) return;
  uint64_t want = (uint64_t{row} / core::kDefaultRows + 1) * core::kDefaultRows;
  if (want > core::kMaxRows) want = core::kMaxRows;
  NoteTouched();
  s->row_limit = static_cast<uint32_t>(want);
}

void Document::SetRowLimit(uint32_t sheet, uint32_t rows) {
  Sheet* s = M(sheet);
  if (!s) return;
  rows = std::clamp<uint32_t>(rows, 1, core::kMaxRows);
  if (s->has_used && rows <= s->used.r1) rows = s->used.r1 + 1;
  NoteTouched();
  s->row_limit = rows;
}

// ---- Cell contents ----

bool Document::SetInput(uint32_t sheet, uint32_t r, uint32_t c, std::string_view text, std::string* error, uint8_t src, bool r1c1) {
  {
    const Rect cell = Rect::Cell(r, c);
    if (Refuse(sheet, &cell)) return false;
  }
  if (text.empty()) {
    ClearContents(sheet, Rect::Cell(r, c), src);
    return true;
  }
  if (text[0] == '\'') {
    SetValue(sheet, r, c, Value::String(text.substr(1)), src);
    return true;
  }
  // a cell formatted as Text keeps what is typed as text, as in Excel: 00123 keeps its zeros and =A1 shows as =A1
  if (IsTextFormatted(sheet, r, c)) {
    SetValue(sheet, r, c, Value::String(text), src);
    return true;
  }
  // a formula typed with a full-width = (an IME in full-width mode) is the formula in half-width (Excel 16, measured)
  std::string folded;
  if (text.size() > 3 && static_cast<unsigned char>(text[0]) == 0xEF) {
    folded = formula::FoldFullWidthFormula(text);
    text = folded;
  }
  // typing (or code writing) next to a table grows it; a formula in an empty table column fills the column
  const bool table_rules = PolicyFor(src).table_rules;
  // ...but a formula that is refused changes nothing, as in Excel: F2, 9, Enter on =SUM(B2:B6) under a table grew
  // the table over it, its range followed to =SUM(B2:B7), a circular reference, and left an Undo step (basics
  // seq470822). One with a structured reference ([@Qty]) may need the table grown first to read
  if (table_rules && text[0] == '=' && text.size() > 1 && text.find('[') == std::string_view::npos) {
    const formula::ParseResult pre = formula::Parse(text.substr(1), core::CellPos{r, c}, this, r1c1);
    if (!pre.ok) {
      if (error) *error = pre.error;
      return false;
    }
  }
  if (table_rules) {
    // a formula with a structured reference grows the table at once ([@Qty] beside it reads the grown table); the
    // rest may wait (defer_table_growth)
    const bool keep = defer_table_growth;
    if (text[0] == '=' && text.find('[') != std::string_view::npos) defer_table_growth = false;
    AutoExpandTable(sheet, r, c, text[0] == '=' && text.size() > 1);
    defer_table_growth = keep;
  }
  if (text[0] == '=' && text.size() > 1) {
    if (!SetFormula(sheet, r, c, text.substr(1), error, src, r1c1)) return false;
    if (table_rules) {
      // the formula's own table loses its name at once; the column's fill may wait as a step of its own
      FillCalculatedColumn(sheet, r, c, !defer_table_growth);
      if (defer_table_growth && deferred_calc_.size() < 4096) deferred_calc_.emplace_back(sheet, core::CellPos{r, c});
    }
    return true;
  }
  if (formula::LooksLikeFormula(text)) {
    std::string err;
    // a + that starts a formula before a number goes (Excel 16, measured: +1+2 is =1+2; -1-2 stays =-1-2, +B2 =+B2)
    std::string_view body = text;
    if (body[0] == '+' && (std::isdigit(static_cast<unsigned char>(body[1])) || body[1] == '.')) body.remove_prefix(1);
    if (SetFormula(sheet, r, c, body, &err, src, r1c1)) return true;
  }
  std::string cell_code;
  if (const Sheet* s = S(sheet)) {
    const model::Style& st = wb_.styles->Get(s->GetStyle(r, c));
    if (st.numfmt) cell_code = std::string(wb_.styles->NumberFormat(st));
  }
  // a/b in a cell with a number format is a fraction, not a date (Excel: 3/32 in a # ??/32 cell is 0.09375)
  if (double frac = 0; formula::FractionForNumberFormat(text, cell_code, &frac)) {
    SetValue(sheet, r, c, Value::Number(frac), src);
    // over a built-in format of another kind it shows as a fraction (Excel 16, measured: 1/2 typed into a 0.00E+00 cell
    // is # ?/?); a plain number format stays
    const size_t slash = text.find('/');
    const bool two = slash != std::string_view::npos && text.size() - slash > 2 && std::isdigit(static_cast<unsigned char>(text[slash + 2]));
    const std::string fmt = two ? "# ??/??" : "# ?/?";
    if (formula::TypedFormatReplaces(cell_code, fmt))
      ModifyStyle(sheet, Rect::Cell(r, c), [&](model::Style& x) { x.numfmt = wb_.styles.mut().InternString(fmt); });
    return true;
  }
  formula::InputParse p = formula::ParseInput(text, excel_15_digit_input);
  SetValue(sheet, r, c, p.value, src);
  // the entry's own format: on a General cell, and over a built-in format of another kind (Excel: 5% typed into a
  // m/d/yyyy cell shows 5%, not 1/0/1900; a custom format stays)
  if (!p.format.empty() && formula::TypedFormatReplaces(cell_code, p.format)) {
    const std::string fmt = p.format;
    ModifyStyle(sheet, Rect::Cell(r, c), [&](model::Style& x) { x.numfmt = wb_.styles.mut().InternString(fmt); });
  }
  return true;
}

bool Document::IsTextFormatted(uint32_t sheet, uint32_t r, uint32_t c) const {
  const Sheet* s = S(sheet);
  return s && wb_.styles->NumberFormat(wb_.styles->Get(s->GetStyle(r, c))) == "@";
}

void Document::SetValue(uint32_t sheet, uint32_t r, uint32_t c, Value v, uint8_t src) {
  if (PolicyFor(src).table_rules && !v.IsEmpty()) AutoExpandTable(sheet, r, c);
  {
    const Rect cell = Rect::Cell(r, c);
    if (Refuse(sheet, &cell)) return;
  }
  if (c >= core::kMaxCols) return;
  EnsureRowLimit(sheet, r);
  Sheet* s = M(sheet);
  if (!s) return;
  if (s->GetFormula(r, c)) {
    UnregisterFormulaCell(sheet, r, c);
    s->SetFormula(r, c, 0);
  }
  if (v.IsEmpty()) {
    EraseLinksIn(sheet, Rect::Cell(r, c));
    s = M(sheet);
  }
  if (wb_.precision_as_displayed) RoundShown(sheet, r, c, &v);  // Set precision as displayed: what is written too
  s->SetValue(r, c, std::move(v));
  MarkChanged(sheet, Rect::Cell(r, c), src);
}

void Document::SetValues(uint32_t sheet, const Rect& rect, const std::vector<Value>& values, uint8_t src) {
  if (Refuse(sheet, &rect)) return;
  if (values.size() < rect.Count()) return;
  // values written next to a table grow it, as one value does (SetValue); text and numbers alike
  if (PolicyFor(src).table_rules) {
    const Sheet* t = S(sheet);
    if (t && !t->tables->empty()) {
      const size_t n = static_cast<size_t>(rect.Count());
      for (size_t i = 0; i < n; ++i) {
        if (!values[i].IsEmpty()) {
          AutoExpandTable(sheet, rect);
          break;
        }
      }
    }
  }
  EnsureRowLimit(sheet, rect.r1);
  Sheet* s = M(sheet);
  if (!s) return;
  size_t i = 0;
  for (uint32_t r = rect.r0; r <= rect.r1; ++r) {
    for (uint32_t c = rect.c0; c <= rect.c1; ++c, ++i) {
      if (s->GetFormula(r, c)) {
        UnregisterFormulaCell(sheet, r, c);
        s->SetFormula(r, c, 0);
      }
      if (wb_.precision_as_displayed && values[i].IsNumber()) {
        Value x = values[i];
        RoundShown(sheet, r, c, &x);
        s->SetValue(r, c, std::move(x));
        continue;
      }
      s->SetValue(r, c, values[i]);
    }
  }
  if (!S(sheet)->links->empty()) {  // the cells emptied lose their hyperlinks (Excel): a walk of the links, not the cells
    std::vector<uint32_t> rows, cols;
    for (const auto& [k, l] : *S(sheet)->links) {
      const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
      if (!rect.Contains(r, c)) continue;
      if (values[static_cast<size_t>(r - rect.r0) * rect.Cols() + (c - rect.c0)].IsEmpty()) rows.push_back(r), cols.push_back(c);
    }
    for (size_t k = 0; k < rows.size(); ++k) EraseLinksIn(sheet, Rect::Cell(rows[k], cols[k]));
  }
  MarkChanged(sheet, rect, src);
}

namespace {

const formula::Node& RootCall(const formula::Node& n) { return n.kind == formula::NK::Paren && !n.kids.empty() ? RootCall(n.kids[0]) : n; }

// the date format Excel gives a typed date (and a date function's result in a General cell)
std::string DateFormatCode(bool with_time) {
  static const std::string date = formula::ParseInput("2024/1/5").format;
  const std::string d = date.empty() ? std::string("yyyy/m/d") : date;
  return with_time ? d + " h:mm" : d;
}

// The number format Excel gives a formula's result when it is entered into a General cell (measured against Excel
// 16): a reference passes its cell's format on (a range its first cell's), and so do + and -, unary signs and these
// functions' first formatted argument; constants and General cells pass nothing on; anything else (* / ^ & %, the
// other functions) gives General, and so do two dates added or subtracted (=A2-A1 counts days). Date and time
// functions, the depreciation and payment functions and RATE / IRR / MIRR give their own formats.
struct ResultFormat {
  enum Kind : uint8_t { None, General, Code } kind = None;
  std::string code;
};

ResultFormat CombineFormats(ResultFormat a, ResultFormat b) {
  if (a.kind == ResultFormat::General || b.kind == ResultFormat::General) return {ResultFormat::General, {}};
  if (a.kind == ResultFormat::None) return b;
  if (b.kind == ResultFormat::None) return a;
  if (formula::FormatShowsDate(a.code) && formula::FormatShowsDate(b.code)) return {ResultFormat::General, {}};
  return a;
}

ResultFormat InferResultFormat(const formula::Node& n, core::CellPos at,
                               const std::function<std::string(int32_t, uint32_t, uint32_t)>& format_at) {
  using formula::NK;
  switch (n.kind) {
    case NK::Paren:
      return n.kids.empty() ? ResultFormat{} : InferResultFormat(n.kids[0], at, format_at);
    case NK::Num:
    case NK::Str:
    case NK::Bool:
    case NK::Err:
    case NK::Missing:
    case NK::Name:
      return {};
    case NK::Ref: {
      formula::ResolvedRef ref;
      if (n.ref.bad || n.ref.sheet2 >= 0 || !formula::ResolveRef(n.ref, at, &ref)) return {};
      std::string code = format_at(ref.sheet, ref.rect.r0, ref.rect.c0);
      if (code.empty() || code == "General") return {};
      return {ResultFormat::Code, std::move(code)};
    }
    case NK::Unary:
      return n.kids.empty() ? ResultFormat{} : InferResultFormat(n.kids[0], at, format_at);
    case NK::Binary:
      if ((n.op == '+' || n.op == '-') && n.kids.size() == 2)
        return CombineFormats(InferResultFormat(n.kids[0], at, format_at), InferResultFormat(n.kids[1], at, format_at));
      return {ResultFormat::General, {}};
    case NK::Func: {
      static const std::set<std::string> kPass = {"SUM",   "AVERAGE",   "MIN", "MAX",   "MEDIAN", "ROUND",
                                                  "ROUNDUP", "ROUNDDOWN", "INT", "TRUNC", "MOD"};
      static const std::set<std::string> kMoney = {"PMT", "PV", "FV", "NPV", "IPMT", "PPMT", "SLN", "DDB", "SYD", "DB"};
      if (kPass.count(n.str)) {
        ResultFormat r;
        for (const formula::Node& k : n.kids) r = CombineFormats(r, InferResultFormat(k, at, format_at));
        return r;
      }
      if (kMoney.count(n.str)) return {ResultFormat::Code, "$#,##0.00_);[Red]($#,##0.00)"};
      if (n.str == "RATE" || n.str == "IRR" || n.str == "MIRR") return {ResultFormat::Code, "0%"};
      if (n.str == "DATE" || n.str == "TODAY") return {ResultFormat::Code, DateFormatCode(false)};
      if (n.str == "NOW") return {ResultFormat::Code, DateFormatCode(true)};
      if (n.str == "TIME") return {ResultFormat::Code, "h:mm AM/PM"};
      return {ResultFormat::General, {}};
    }
    default:
      return {ResultFormat::General, {}};
  }
}

}  // namespace

bool Document::SetFormula(uint32_t sheet, uint32_t r, uint32_t c, std::string_view text, std::string* error, uint8_t src, bool r1c1) {
  if (r >= core::kMaxRows || c >= core::kMaxCols) {  // past XFD: the tile directory has no slot for it
    if (error) *error = TR("The cell is outside the sheet.");
    return false;
  }
  {
    const Rect cell = Rect::Cell(r, c);
    if (Refuse(sheet, &cell)) return false;
  }
  formula::ParseResult pr = formula::Parse(text, core::CellPos{r, c}, this, r1c1);
  if (!pr.ok) {
    if (error) *error = pr.error;
    return false;
  }
  EnsureRowLimit(sheet, r);
  Sheet* s = M(sheet);
  if (!s) return false;
  // like Excel: a formula typed into a cell without a number format, or written there by a macro, shows its result
  // in the format its references or its function give it (=A1+1 as A1, =SUM(B:B) as B1, =TODAY() as a date); a
  // file's cell keeps the format the file gave it (a General formula over formatted cells stays General)
  std::string auto_format;
  if (PolicyFor(src).infer_format && !wb_.styles->Get(s->GetStyle(r, c)).numfmt) {
    const ResultFormat rf = InferResultFormat(pr.root, core::CellPos{r, c}, [&](int32_t ref_sheet, uint32_t rr, uint32_t cc) {
      const Sheet* rs = S(ref_sheet < 0 ? sheet : static_cast<uint32_t>(ref_sheet));
      if (!rs) return std::string();
      const uint32_t id = wb_.styles->Get(rs->GetStyle(rr, cc)).numfmt;
      return id ? std::string(wb_.styles->Str(id)) : std::string();
    });
    if (rf.kind == ResultFormat::Code) auto_format = rf.code;
  }
  const uint32_t id = InternFormula(std::move(pr.root));
  s = M(sheet);
  s->SetFormula(r, c, id);
  if (!auto_format.empty() && !wb_.styles->Get(s->GetStyle(r, c)).numfmt) {
    auto& styles = wb_.styles.mut();
    model::Style st = styles.Get(s->GetStyle(r, c));
    st.numfmt = styles.InternString(auto_format);
    s->SetStyle(r, c, styles.Intern(st));
  }
  RegisterFormulaCell(sheet, r, c, id);
  MarkChanged(sheet, Rect::Cell(r, c), src);
  ShowEnteredOnCalcOff(sheet, r, c);
  if (!auto_calc) entered_.push_back(Key(sheet, r, c));  // manual calculation still computes a formula as it's entered
  return true;
}

std::string Document::InferredFormat(uint32_t sheet, uint32_t r, uint32_t c, std::string_view text) {
  formula::ParseResult pr = formula::Parse(text, core::CellPos{r, c}, this);
  if (!pr.ok) return std::string();
  const ResultFormat rf = InferResultFormat(pr.root, core::CellPos{r, c}, [&](int32_t ref_sheet, uint32_t rr, uint32_t cc) {
    const Sheet* rs = S(ref_sheet < 0 ? sheet : static_cast<uint32_t>(ref_sheet));
    if (!rs) return std::string();
    const uint32_t id = wb_.styles->Get(rs->GetStyle(rr, cc)).numfmt;
    return id ? std::string(wb_.styles->Str(id)) : std::string();
  });
  return rf.kind == ResultFormat::Code ? rf.code : std::string();
}

bool Document::SetArrayFormula(uint32_t sheet, const Rect& rect, std::string_view text, std::string* error, uint8_t src, bool r1c1) {
  if (Refuse(sheet, &rect)) return false;
  if (rect.r1 >= core::kMaxRows || rect.c1 >= core::kMaxCols) return false;
  // an array is calculated whole, a value a cell: over the whole sheet (17 billion cells) it took all the machine's
  // memory (doc_fuzz --wide, 80 GB before the process died). Past 20,000,000 cells it is refused, as the UI's
  // Ctrl+Shift+Enter is
  if (rect.Count() > 20'000'000) {
    if (error) *error = TR("The selection is too large for an array formula (at most 20,000,000 cells).");
    return false;
  }
  formula::ParseResult pr = formula::Parse(text, core::CellPos{rect.r0, rect.c0}, this, r1c1);
  if (!pr.ok) {
    if (error) *error = pr.error;
    return false;
  }
  const Sheet* cs = S(sheet);
  if (!cs) return false;
  for (const auto& [k, sp] : *cs->spills) {
    if (sp.cse && sp.rect.Intersects(rect) && !rect.Contains(sp.rect)) {
      if (error) *error = TR("You can't change part of an array.");
      return false;
    }
  }
  // Excel refuses an array over several cells of a table: its header would hold a result (text there, a number once
  // calculated again on open: doc_fuzz seed 112)
  if (!rect.IsCell()) {
    for (const auto& t : *cs->tables) {
      if (!t.range.Intersects(rect)) continue;
      if (error) *error = TR("Multi-cell array formulas aren't allowed in tables.");
      return false;
    }
  }
  EnsureRowLimit(sheet, rect.r1);
  ClearContents(sheet, rect, src);  // the range is replaced as a whole (older arrays and spills in it go away)
  {
    // a dynamic array that spilled into the range gives those cells up: calculated again, it finds them taken
    // (#SPILL!). Its spill record stayed over the array's cells, it spilled back over them as its own, and a later
    // insertion cleared them with its spill (doc_fuzz seed 10130034)
    std::vector<uint64_t> spilled;
    for (const auto& [k, sp] : *S(sheet)->spills) {
      if (!sp.cse && sp.rect.Intersects(rect)) spilled.push_back(Key(sheet, static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF)));
    }
    for (const uint64_t a : spilled) {
      ClearSpill(a, nullptr);
      AddPending(a);
    }
  }
  const uint32_t id = InternFormula(std::move(pr.root));
  Sheet* s = M(sheet);
  s->SetFormula(rect.r0, rect.c0, id);
  s->spills.mut()[(uint64_t{rect.r0} << 16) | rect.c0] = model::SpillRange{rect, true};
  s->Touch(rect.r1, rect.c1);
  const uint64_t a = Key(sheet, rect.r0, rect.c0);
  spill_anchor_[a] = rect;
  IndexSpill(a, rect, true);
  RegisterFormulaCell(sheet, rect.r0, rect.c0, id);
  MarkChanged(sheet, rect, src);
  ShowEnteredOnCalcOff(sheet, rect.r0, rect.c0);
  if (!auto_calc) entered_.push_back(Key(sheet, rect.r0, rect.c0));  // manual calculation: computed as it's entered
  return true;
}

void Document::ClearContents(uint32_t sheet, const Rect& rect, uint8_t src) {
  // like Excel: clearing a range that holds a whole PivotTable report deletes the PivotTable
  // (pt.TableRange2.Clear() is how macros delete one); clearing only part of a report is still refused
  if (!pivot_writing_) {
    if (const Sheet* ps = S(sheet); ps && !ps->pivots->empty()) {
      std::vector<uint32_t> whole;
      for (const auto& p : *ps->pivots) {
        if (p.has_output && rect.Contains(p.output)) whole.push_back(p.id);
      }
      for (const uint32_t id : whole) DeletePivot(sheet, id, true);
    }
  }
  // clearing the whole body of a data table removes the table (like Excel)
  if (!table_writing_) {
    if (const Sheet* ts = S(sheet); ts && !ts->data_tables->empty()) {
      const auto& tv = *ts->data_tables;
      if (std::any_of(tv.begin(), tv.end(), [&](const model::DataTable& t) { return rect.Contains(t.Body()); })) {
        auto& v = M(sheet)->data_tables.mut();
        v.erase(std::remove_if(v.begin(), v.end(), [&](const model::DataTable& t) { return rect.Contains(t.Body()); }), v.end());
        NoteTouched();
        table_writing_ = true;
        ClearContents(sheet, rect, src);
        table_writing_ = false;
        return;
      }
    }
  }
  if (Refuse(sheet, &rect)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  std::vector<std::pair<uint32_t, uint32_t>> cells;
  ForEachCellInTiles(s->meta, rect, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.formula[idx]) cells.emplace_back(r, c);
  });
  for (auto [r, c] : cells) {
    UnregisterFormulaCell(sheet, r, c);
    s->SetFormula(r, c, 0);
  }
  cells.clear();
  ForEachCellInTiles(s->values, rect, [&](uint32_t r, uint32_t c, const model::ValueTile& t, uint32_t idx) {
    if (t.Occupied(idx)) cells.emplace_back(r, c);
  });
  for (auto [r, c] : cells) s->SetValue(r, c, Value());
  EraseRich(sheet, rect);
  EraseLinksIn(sheet, rect);
  MarkChanged(sheet, rect, src);
}

// Clear Formats gives the lines' formats crossing the cells cleared Normal cell by cell: past 20,000,000 cells that
// can't be done another way (ClearFormats), it is refused, before anything changes (Clear asks before it clears
// the contents)
bool Document::RefuseClearFormatsSize(uint32_t sheet, const Rect& rect) {
  const Sheet* s = S(sheet);
  if (!s) return false;
  Rect rc = rect;
  rc.r1 = std::min(rc.r1, s->row_limit - 1);
  rc.c1 = std::min(rc.c1, s->col_limit - 1);
  if (rc.r0 > rc.r1 || rc.c0 > rc.c1) return false;
  const bool whole_cols = rc.r0 == 0 && rc.r1 == s->row_limit - 1;
  const bool whole_rows = rc.c0 == 0 && rc.c1 == s->col_limit - 1;
  {
    uint64_t cost = 0;
    if (!whole_rows) {
      const uint32_t r_last = whole_cols ? s->row_limit - 1 : rc.r1;
      uint64_t styled_rows = 0;  // the rows' own Normal is skipped there
      for (auto it = s->row_styles->lower_bound(rc.r0); it != s->row_styles->end() && it->first <= r_last; ++it) {
        styled_rows += it->second != model::kStyleOwnNormal;
      }
      cost += styled_rows * rc.Cols();
    }
    if (!whole_cols && !whole_rows) {
      const uint64_t styled_cols = static_cast<uint64_t>(std::distance(s->col_styles->lower_bound(rc.c0), s->col_styles->upper_bound(rc.c1)));
      const bool by_rows_above = rc.r1 == s->row_limit - 1 && styled_cols * rc.r0 <= 20'000'000;
      if (!by_rows_above) cost += styled_cols * (uint64_t{rc.r1} - rc.r0 + 1);
    }
    if (cost > 20'000'000) {
      CountBlock(BlockReason::TooLarge);
      block_sheet_ = sheet;
      block_cells_ = rc;
      block_range_ = -1;
      block_python_ = python_call;
      block_too_large_ = true;
      return true;
    }
  }
  return false;
}

void Document::ClearFormats(uint32_t sheet, const Rect& rect, bool cond_formats, bool to_lines) {
  if (Refuse(sheet, nullptr, Allow::FormatCells)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  if (to_lines) {
    // what a cut leaves (Excel 16, measured: H3 cut out of a formatted column H shows H's format again)
    std::vector<std::pair<uint32_t, uint32_t>> own;
    ForEachCellInTiles(s->meta, rect, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
      if (t.style[idx]) own.emplace_back(r, c);
    });
    for (auto [r, c] : own) s->SetStyle(r, c, 0);
    EraseRich(sheet, rect);
    s = M(sheet);
    auto& m = s->merges.mut();
    m.erase(std::remove_if(m.begin(), m.end(), [&](const Rect& x) { return rect.Contains(x); }), m.end());
    NoteFormat(sheet, rect);
    changes_.push_back(CellChange{sheet, rect, kSrcUser});
    wb_.dirty = true;
    if (cond_formats) ClearCondFormats(sheet, rect);
    return;
  }
  // whole columns and rows lose their own formats too (Excel: Clear Formats over column C leaves C9 unfilled); left in
  // place, the column's fill or the whole sheet's borders came back on every cell after Clear. Whole rows crossed by
  // a column's formats keep a Normal style of their own, which wins over the column's, as Excel's row records do.
  Rect rc = rect;
  rc.r1 = std::min(rc.r1, s->row_limit - 1);
  rc.c1 = std::min(rc.c1, s->col_limit - 1);
  if (rc.r0 > rc.r1 || rc.c0 > rc.c1) return;
  const bool whole_cols = rc.r0 == 0 && rc.r1 == s->row_limit - 1;
  const bool whole_rows = rc.c0 == 0 && rc.c1 == s->col_limit - 1;
  auto any_in = [](const std::map<uint32_t, uint32_t>& m, uint32_t a, uint32_t b) { return m.lower_bound(a) != m.upper_bound(b); };
  if (RefuseClearFormatsSize(sheet, rect)) return;
  if (whole_cols && any_in(*s->col_styles, rc.c0, rc.c1)) {
    auto& m = s->col_styles.mut();
    m.erase(m.lower_bound(rc.c0), m.upper_bound(rc.c1));
    NoteDims(sheet, false, rc.c0, rc.c1);
  }
  if (whole_rows && (any_in(*s->row_styles, rc.r0, rc.r1) || !s->col_styles->empty())) {
    auto& m = s->row_styles.mut();
    m.erase(m.lower_bound(rc.r0), m.upper_bound(rc.r1));
    if (!s->col_styles->empty()) {
      for (uint32_t r = rc.r0; r <= rc.r1; ++r) m.emplace_hint(m.end(), r, model::kStyleOwnNormal);
    }
    NoteDims(sheet, true, rc.r0, rc.r1);
  }
  std::vector<std::pair<uint32_t, uint32_t>> cells;
  ForEachCellInTiles(s->meta, rc, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.style[idx]) cells.emplace_back(r, c);
  });
  for (auto [r, c] : cells) s->ShowStyle(r, c, 0);
  // a row's formats crossing the columns cleared: those cells take the Normal style as their own
  if (!whole_rows) {
    const uint32_t r_last = whole_cols ? s->row_limit - 1 : rc.r1;
    for (auto it = s->row_styles->lower_bound(rc.r0); it != s->row_styles->end() && it->first <= r_last; ++it) {
      if (it->second == model::kStyleOwnNormal) continue;
      for (uint32_t c = rc.c0; c <= rc.c1; ++c) s->ShowStyle(it->first, c, 0);
    }
  }
  if (!whole_cols && !whole_rows) {
    // a column's formats crossing the rows cleared (rows with a style of their own are done above)
    const auto lo = s->col_styles->lower_bound(rc.c0), hi = s->col_styles->upper_bound(rc.c1);
    const uint64_t styled_cols = static_cast<uint64_t>(std::distance(lo, hi));
    const uint64_t rows_cleared = uint64_t{rc.r1} - rc.r0 + 1;
    if (styled_cols * rows_cleared > 4'000'000 && rc.r1 == s->row_limit - 1 && styled_cols * rc.r0 <= 20'000'000) {
      // down to the sheet's end (C11 to the last cell, columns C:XFD formatted: 17 billion cells cell by cell, out of
      // memory, doc_fuzz --wide --more seed 89031): the columns lose their formats, and the rows above the cleared
      // cells keep showing them, cell by cell
      std::vector<std::pair<uint32_t, uint32_t>> cols(lo, hi);
      for (const auto& [c, id] : cols) {
        for (uint32_t r = 0; r < rc.r0; ++r) {
          if (!s->GetOwnStyle(r, c) && s->row_styles->find(r) == s->row_styles->end()) s->SetStyle(r, c, id);
        }
      }
      auto& cm = s->col_styles.mut();
      cm.erase(cm.lower_bound(rc.c0), cm.upper_bound(rc.c1));
      NoteDims(sheet, false, rc.c0, rc.c1);
    } else {
      for (auto it = lo; it != hi; ++it) {
        for (uint32_t r = rc.r0; r <= rc.r1; ++r) s->ShowStyle(r, it->first, 0);
      }
    }
  }
  EraseRich(sheet, rc);
  s = M(sheet);
  // merged areas are removed too (like Excel's Clear Formats)
  auto& m = s->merges.mut();
  m.erase(std::remove_if(m.begin(), m.end(), [&](const Rect& x) { return rect.Contains(x); }), m.end());
  NoteFormat(sheet, rect);
  changes_.push_back(CellChange{sheet, rect, kSrcUser});
  wb_.dirty = true;
  if (cond_formats) ClearCondFormats(sheet, rect);  // Excel's Clear Formats takes the cells' rules off too
}

void Document::ClearAll(uint32_t sheet, const Rect& rect, uint8_t src, bool everything, bool to_lines) {
  if (!to_lines && RefuseClearFormatsSize(sheet, rect)) return;
  const uint32_t refused = protect_blocks_;
  ClearContents(sheet, rect, src);
  // refused (locked cells, part of a PivotTable report): like Excel, nothing is cleared, not even the formats
  if (protect_blocks_ != refused) return;
  // unlocked cells of a sheet protected without Format Cells: their contents go and their formats stay (Excel 16,
  // measured 2026-11-20: Clear of an unlocked bold B3 empties it, still bold and unlocked); the formats' refusal
  // made the whole Clear raise
  if (ProtectionApplies(sheet) && !S(sheet)->protection->format_cells) return;
  ClearFormats(sheet, rect, everything, to_lines);
  ClearValidation(sheet, rect);  // like Excel's Clear / Clear All: the drop-down lists and input rules go too
  if (everything) {
    ClearNotes(sheet, rect);
    ClearHyperlinks(sheet, rect);
  }
}

const Value& Document::GetValue(uint32_t sheet, uint32_t r, uint32_t c) const {
  static const Value kEmpty;
  const Sheet* s = S(sheet);
  return s ? s->GetValue(r, c) : kEmpty;
}

Value Document::EvaluateFormula(uint32_t sheet, std::string_view text, std::string* error, std::shared_ptr<formula::Array>* array) {
  if (!text.empty() && text[0] == '=') text.remove_prefix(1);
  formula::ParseResult pr = formula::Parse(text, core::CellPos{0, 0}, this);
  if (!pr.ok) {
    if (error) *error = pr.error;
    return Value::Error(core::Err::Name);
  }
  Recalc();
  const uint64_t saved = current_cell_;
  current_cell_ = Key(sheet, 0, 0);
  Value v;
  try {
    v = formula::EvaluateCell(pr.root, *host_, static_cast<int32_t>(sheet), core::CellPos{0, 0}, array);
  } catch (const formula::NeedCell&) {
    v = Value::Error(core::Err::Calc);
    if (array) array->reset();
  }
  current_cell_ = saved;
  return v;
}

Value Document::EvaluateAt(uint32_t sheet, const formula::Node& root, core::CellPos at) {
  const uint64_t saved = current_cell_;
  current_cell_ = Key(sheet, at.row, at.col);
  Value v;
  try {
    v = formula::EvaluateCell(root, *host_, static_cast<int32_t>(sheet), at);
  } catch (const formula::NeedCell&) {
    v = Value::Error(core::Err::Calc);
  }
  current_cell_ = saved;
  return v;
}

bool Document::EvaluateRef(uint32_t sheet, std::string_view text, uint32_t* out_sheet, Rect* out_rect, std::string* error) {
  if (!text.empty() && text[0] == '=') text.remove_prefix(1);
  formula::ParseResult pr = formula::Parse(text, core::CellPos{0, 0}, this);
  if (!pr.ok) {
    if (error) *error = pr.error;
    return false;
  }
  Recalc();
  const uint64_t saved = current_cell_;
  current_cell_ = Key(sheet, 0, 0);
  bool ok = false;
  try {
    formula::Evaluator ev(*host_, static_cast<int32_t>(sheet), core::CellPos{0, 0});
    const formula::EV e = ev.Eval(pr.root);
    if (e.k == formula::EV::K::Range) {
      *out_sheet = e.r.sheet < 0 ? sheet : static_cast<uint32_t>(e.r.sheet);
      *out_rect = e.r.rect;
      ok = true;
    } else if (error) {
      *error = TR("The reference isn't a range");
    }
  } catch (const formula::NeedCell&) {
    if (error) *error = TR("Calculating, please try again");
  }
  current_cell_ = saved;
  return ok;
}

std::string Document::GetFormulaA1(uint32_t sheet, uint32_t r, uint32_t c) const {
  const Sheet* s = S(sheet);
  if (!s) return {};
  const formula::CompiledFormula* f = Formula(s->GetFormula(r, c));
  if (!f) return {};
  return "=" + formula::ToA1(f->root, core::CellPos{r, c}, this);
}

std::string Document::GetInputText(uint32_t sheet, uint32_t r, uint32_t c) const {
  std::string f = GetFormulaA1(sheet, r, c);
  if (!f.empty()) return f;
  const Sheet* s = S(sheet);
  if (!s) return {};
  if (!s->data_tables->empty()) {
    if (std::string t = DataTableText(*s, r, c); !t.empty()) return t;
  }
  const Value& v = s->GetValue(r, c);
  const model::Style& st = wb_.styles->Get(s->GetStyle(r, c));
  const std::string& code = wb_.styles->NumberFormat(st);
  return InputTextOf(v, code);
}

std::string DisplayTextOf(const Value& v, std::string_view numfmt) { return formula::FormatValue(v, numfmt).text; }

std::string InputTextOf(const Value& v, std::string_view numfmt) {
  switch (v.type()) {
    case core::VType::Empty: return {};
    case core::VType::Bool: return v.boolean() ? "TRUE" : "FALSE";
    case core::VType::Error: return core::ErrorText(v.error());
    case core::VType::StrInline:
    case core::VType::StrHeap: {
      std::string s(v.str());
      // text that looks like a number: needs a leading ' to round-trip as text
      const formula::InputParse p = formula::ParseInput(s);
      if (!p.value.IsString() && !s.empty()) return "'" + s;
      return s;
    }
    case core::VType::Decimal: return core::DecToString(core::DecFromValue(v));
    case core::VType::Pending: return {};
    case core::VType::Number: {
      const double d = v.number();
      if (formula::IsDateFormat(numfmt) && d >= 0) {
        const bool has_time = d != std::floor(d);
        const bool has_date = d >= 1;
        std::string code = has_date ? "yyyy/m/d" : "";
        if (has_time) code += has_date ? " h:mm:ss" : "h:mm:ss";
        if (code.empty()) code = "yyyy/m/d";
        return formula::FormatValue(v, code).text;
      }
      // 15 significant digits, as Excel shows a constant to edit: General's 11 (1.23457E+11) turned F2 then Enter into
      // a rounding of the number (an order id, a volume, a market value)
      if (numfmt.find('%') != std::string_view::npos && numfmt.find('"') == std::string_view::npos) {
        return formula::EditNumberText(d * 100) + "%";
      }
      return formula::EditNumberText(d);
    }
  }
  return {};
}

// ---- Recalculation ----

Value Document::EvaluateFormulaCell(uint64_t key, std::shared_ptr<formula::Array>* spill) {
  const uint32_t sheet = KeySheet(key), r = KeyRow(key), c = KeyCol(key);
  const Sheet* s = S(sheet);
  if (!s) return Value();
  const formula::CompiledFormula* f = Formula(s->GetFormula(r, c));
  if (!f) return s->GetValue(r, c);
  current_cell_ = key;
  return formula::EvaluateCell(f->root, *host_, static_cast<int32_t>(sheet), core::CellPos{r, c}, spill);
}

formula::EV Document::EvaluateNodeAt(uint32_t sheet, core::CellPos origin, const formula::Node& n) {
  formula::EV e;
  try {
    formula::Evaluator ev(*host_, static_cast<int32_t>(sheet), origin);
    e = ev.Eval(n);
    if (e.k == formula::EV::K::Range && e.r.rect.IsCell() && e.r.sheet2 < 0) e = formula::EV::S(ev.ToScalar(e));
    if (e.k == formula::EV::K::Range && e.r.rect.Count() <= 100 && e.r.sheet2 < 0) e = formula::EV::A(ev.ToArray(e));
  } catch (const formula::NeedCell&) {
    e = formula::EV::Err(core::Err::Calc);
  }
  return e;
}

void Document::Propagate() {
  // newly pending cells: mark their (transitive) dependents pending too, one round per "generation" of new cells,
  // each round querying the dependency index once per changed tile (PLAN §7.1). A cell found pending in one of the
  // round's tiles is marked changed at once, so the entries after it in the same scan see it: a chain of formulas
  // down a column (=G2+1 filled a million rows) took a round per cell, each scanning its tile's entries (7.5 s for
  // Ctrl+' over its first cell, basics seq430649); it now takes about a round per tile
  std::vector<uint64_t> round;
  while (!new_pending_.empty()) {
    round.clear();
    round.swap(new_pending_);
    ChangedCells changed;
    for (uint64_t k : round) changed.Add(KeySheet(k), KeyRow(k), KeyCol(k));
    changed.Finish();
    deps_.QueryChanged(changed, [&](uint64_t d) {  // new cells go to new_pending_: the next round
      const size_t before = new_pending_.size();
      AddPending(d);
      return new_pending_.size() > before && changed.Mark(KeySheet(d), KeyRow(d), KeyCol(d));
    });
  }
}

void Document::CalcEntered() {
  // manual calculation (Excel): a formula is computed when it is entered, from the values the cells show now; what
  // depends on it, and on cells changed meanwhile, waits for F9
  if (entered_.empty()) return;
  stale_reads_ = true;
  for (uint64_t k : entered_) {
    if (!IsPending(k)) continue;
    Value v;
    std::shared_ptr<formula::Array> arr;
    try {
      v = EvaluateFormulaCell(k, &arr);
    } catch (const std::exception&) {
      v = Value::Error(core::Err::Calc);
    }
    StoreResult(k, std::move(v), std::move(arr));
  }
  stale_reads_ = false;
  entered_.clear();
}

void Document::PendingIn(uint32_t sheet, const Rect& rect, size_t cap, std::vector<uint64_t>* out) const {
  out->clear();
  for (const uint64_t k : pending_) {
    if (out->size() >= cap) break;
    if (KeySheet(k) == sheet && rect.Contains(KeyRow(k), KeyCol(k))) out->push_back(k);
  }
}

void Document::MarkAllVolatileDirty() {
  for (uint64_t k : volatile_) AddPending(k);
}

void Document::RecalcWithoutEvents() {
  if (!auto_calc) return;  // manual calculation: F9 calculates later, and fires Calculate then (Excel)
  const size_t before = calc_sheets_.size();
  Recalc();
  calc_sheets_.resize(before);
}

void Document::Recalc() {
  const auto t0 = std::chrono::steady_clock::now();
  stats_ = CalcStats{};
  interrupted_ = false;
  // Workbook.ForceFullCalculation: every calculation calculates every formula
  if (wb_.force_full_calc) {
    for (const auto& sc : wb_.sheets) MarkDirty(sc->id, Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1});
  }
  struct ClearInterrupt {
    std::atomic<bool>* f;
    ~ClearInterrupt() {
      if (f) f->store(false, std::memory_order_relaxed);
    }
  } clear_interrupt{interrupt};
  if (!spill_anchor_.empty()) DropOrphanSpills();
  RecheckBlockedSpills();
  MarkAllVolatileDirty();
  BeginCycles(-1, only_rect_);
  stats_.circular = static_cast<uint32_t>(circular_cells.size());
  if (pending_.empty()) {
    cycle_block_.clear();
    fresh_formulas_.clear();
    restored_formulas_.clear();
    respill_checked_.clear();
    if (tables_dirty_ && !in_tables_ && auto_data_tables) RecalcDataTables();
    return;
  }
  spills_changed_ = false;
  if (calc_threads != 1 && pending_.size() >= kParallelMin && core::WorkerCount() > 1) RecalcWaves();
  EvaluatePendingSerial();
  if (StopRequested()) {
    // interrupted (CalculationInterruptKey, CheckAbort): the cells not calculated stay pending, nothing half done is
    // kept; circular references and the second rounds wait for the calculation that finishes. The cells a cycle the
    // pass met set aside (its members and what read them, taken out of the pending set by AbortStack) wait again, so
    // the calculation that goes on meets the cycle as one never interrupted does: with the block cleared it found no
    // cycle, or calculated what reads one (doc_fuzz --gaps 763382)
    for (const uint64_t k : cycle_aborted_) {
      const Sheet* cs = S(KeySheet(k));
      if (cs && cs->GetFormula(KeyRow(k), KeyCol(k))) AddPending(k);
    }
    cycle_aborted_.clear();
    cycle_members_.clear();
    fresh_aborted_.clear();
    in_progress_.clear();
    cycle_break_.clear();
    cycle_block_.clear();
    new_pending_.clear();
    stats_.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return;
  }
  // spill ranges that grew, shrank or changed marked more cells pending: evaluate those too (bounded, in case two
  // spills keep resizing each other). A cell marked again during the pass may already have been calculated again in
  // it (it came later in the order), leaving nothing pending but its dependents not yet propagated: a reader of a
  // spilled value calculated before the anchor spilled again, read by a formula on another sheet (calc_fuzz 3102).
  for (int pass = 0; pass < 64 && !Settled(); ++pass) {
    Propagate();
    EvaluatePendingSerial();
  }
  // the circular references met: iterated, or left uncalculated and reported; what their rounds spilled is
  // calculated after them
  ResolveCycles();
  for (int pass = 0; pass < 64 && !Settled(); ++pass) {
    Propagate();
    EvaluatePendingSerial();
    ResolveCycles();
  }
  // what reads cells through a name, OFFSET or INDIRECT isn't in the dependency index, so a spill that appeared,
  // changed or went in this calculation may have filled cells such a formula read before: it is calculated once more
  // (doc_fuzz seed 10431002: =Rate*2, Rate a relative name, read F11:F12 before =SEQUENCE(3) in F10 spilled there).
  // RAND and NOW aren't calculated twice; OFFSET and INDIRECT are (Excel 16, measured: =Rate*2 spilling into B2, read
  // by =OFFSET(A1,1,1) in E8 that the name reads, is a circular reference: doc_fuzz seed 10503625).
  for (int round = 0; round < 4 && spills_changed_ && circular_cells.empty(); ++round) {
    spills_changed_ = false;
    bool any = false;
    for (const uint64_t k : volatile_) {
      const Sheet* vs = S(KeySheet(k));
      const formula::CompiledFormula* f = vs ? Formula(vs->GetFormula(KeyRow(k), KeyCol(k))) : nullptr;
      if (!f || !(f->flags & model::FormulaDef::kDynamicRef) || (f->flags & model::FormulaDef::kChanging)) continue;
      AddPending(k);
      any = true;
    }
    if (!any) break;
    for (int pass = 0; pass < 64 && !Settled(); ++pass) {
      Propagate();
      EvaluatePendingSerial();
      ResolveCycles();  // a cycle the spill closed is reported as any other
    }
  }
  stats_.circular = iterate ? 0 : static_cast<uint32_t>(circular_cells.size());
  in_progress_.clear();
  cycle_break_.clear();
  cycle_block_.clear();
  new_pending_.clear();
  fresh_formulas_.clear();
  restored_formulas_.clear();
  respill_checked_.clear();
  stats_.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  // data tables follow the cells they depend on (like Excel's "Automatic" calculation; not with "Automatic except
  // for data tables", where F9 calculates them)
  if (tables_dirty_ && !in_tables_ && auto_data_tables) RecalcDataTables();
}

namespace {
// appends the keys of `more` that `list` doesn't hold yet, in their order
void AppendNew(std::vector<uint64_t>& list, const std::vector<uint64_t>& more) {
  std::unordered_set<uint64_t> have(list.begin(), list.end());
  for (uint64_t k : more) {
    if (have.insert(k).second) list.push_back(k);
  }
}
}  // namespace

void Document::BeginCycles(int64_t sheet, const Rect& rect) {
  auto here = [&](uint64_t k) { return sheet < 0 || (KeySheet(k) == static_cast<uint64_t>(sheet) && rect.Contains(KeyRow(k), KeyCol(k))); };
  auto has_formula = [&](uint64_t k) {
    const Sheet* cs = S(KeySheet(k));
    return cs && cs->GetFormula(KeyRow(k), KeyCol(k)) != 0;
  };
  if (!iterate && !iter_cells_.empty()) {
    // iteration was turned off: the cycles keep the values it gave them and are reported (Excel doesn't calculate
    // anything when the option changes)
    AppendNew(circular_cells, iter_cells_);
    iter_cells_.clear();
  }
  if (iterate) {
    // the cycles are calculated again at every recalculation that follows an edit (or F9, a full calculation, a
    // sheet's): Excel keeps their cells dirty, so =A1+1 with one iteration counts 1, 2, 3...
    const bool again = force_iteration_ || input_seq_ != iter_seq_ || sheet >= 0;
    if (again && !iter_cells_.empty()) {
      std::vector<uint64_t> keep;
      for (uint64_t k : iter_cells_) {
        if (!here(k)) keep.push_back(k);
        else if (has_formula(k)) AddPending(k);  // found again by the pass (or dropped when no longer circular)
      }
      iter_cells_.swap(keep);
    }
    // a circular reference found while iteration was off is iterated now that it is on
    for (uint64_t k : circular_cells) {
      if (has_formula(k)) AddPending(k);
    }
    circular_cells.clear();
  }
  if (sheet < 0) iter_seq_ = input_seq_;
  Propagate();
  // a circular reference stays reported until it is calculated again (Excel keeps it in the status bar while the
  // cycle exists): the pending cells are found again by the pass if they still form one; cleared cells drop out.
  // Those not calculated again stay uncalculated, and so do the formulas that read them.
  // (asked again only after an edit or with cells to calculate: a recalculation with nothing new, at every request of
  // a script, walked a million circular cells each time)
  if (!pending_.empty() || input_seq_ != cycles_checked_seq_) {
    const bool none_pending = pending_.empty();
    std::erase_if(circular_cells, [&](uint64_t k) { return (!none_pending && IsPending(k)) || !has_formula(k); });
    cycles_checked_seq_ = input_seq_;
  }
  cycle_block_.clear();
  // nothing to calculate: nothing reads the block (Recalc returns at once and clears it). Rebuilt anyway, a million
  // circular cells cost a million inserts at every recalculation, edits elsewhere and Python's requests alike
  // (basics seq444742: Ctrl+D of =G1+1 down the column took 10 s where Excel 16 takes 2)
  if (pending_.empty()) return;
  cycle_block_.reserve(circular_cells.size());
  for (uint64_t k : circular_cells) cycle_block_.insert(k);
}

void Document::AbortStack(std::vector<uint64_t>& stack, uint64_t needed) {
  // the stack's cells wait for each other in turn, the top one for `needed`: when that is on the stack the cells from
  // it up form the cycle; below it, and all of them when `needed` is a cycle's cell met before, they read the cycle
  size_t from = stack.size();
  if (in_progress_.count(needed)) {
    for (size_t i = 0; i < stack.size(); ++i) {
      if (stack[i] == needed) {
        from = i;
        break;
      }
    }
  }
  for (size_t i = 0; i < stack.size(); ++i) {
    const uint64_t k = stack[i];
    if (i >= from) cycle_members_.push_back(k);
    cycle_aborted_.push_back(k);
    cycle_block_.insert(k);
    in_progress_.erase(k);
    // not calculated: it keeps the value it shows, except a formula just entered, which shows 0 (Excel)
    // a dynamic array put back by Undo / Redo comes without its spilled values (CopyRegionFrom): it is set as the
    // step that entered it set it, or the cycle left its spill empty (doc_fuzz seed 23 after Redo)
    const model::SpillRange* sp = restored_formulas_.count(k) ? S(KeySheet(k))->SpillOf(KeyRow(k), KeyCol(k)) : nullptr;
    if (i < from && fresh_formulas_.count(k)) fresh_aborted_.push_back(k);  // a reader of the cycle just entered
    if (fresh_formulas_.count(k) || (sp && !sp->cse)) StoreResult(k, Value::Number(0), nullptr);
    else ErasePending(k);
  }
  stack.clear();
}

void Document::ResolveCycles() {
  if (cycle_aborted_.empty()) return;
  std::vector<uint64_t> cells, members;
  cells.swap(cycle_aborted_);
  members.swap(cycle_members_);
  std::unordered_set<uint64_t> fresh(fresh_aborted_.begin(), fresh_aborted_.end());
  fresh_aborted_.clear();
  std::sort(cells.begin(), cells.end());
  cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
  if (!iterate) {
    // an array entered with Ctrl+Shift+Enter that met itself only by reading its own cells is calculated when no value
    // of it reads itself, as in Excel 16 ({=B2:C4*2} over C2:C4 shows B2:B4 times 2; {=C2:D3*1} is a circular
    // reference): what read it is calculated after it
    // (a cycle through other cells too isn't settled: its reads of them wait)
    std::unordered_set<uint64_t> tried, settled_keys;
    for (const uint64_t k : members) {
      const Sheet* ks = S(KeySheet(k));
      const model::SpillRange* ksp = ks ? ks->SpillOf(KeyRow(k), KeyCol(k)) : nullptr;
      if (!ksp || !ksp->cse || !tried.insert(k).second) continue;
      if (SettleArrayReadingItself(k)) settled_keys.insert(k);
    }
    // arrays that read each other's cells, every member of the cycle one: settled together
    if (settled_keys.empty() && members.size() >= 2 && std::all_of(members.begin(), members.end(), [&](uint64_t k) {
          const Sheet* ks = S(KeySheet(k));
          const model::SpillRange* ksp = ks ? ks->SpillOf(KeyRow(k), KeyCol(k)) : nullptr;
          return ksp && ksp->cse && ksp->rect.r0 == KeyRow(k) && ksp->rect.c0 == KeyCol(k);
        })) {
      std::vector<uint64_t> anchors(members.begin(), members.end());
      std::sort(anchors.begin(), anchors.end());
      anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());
      if (SettleArraysTogether(anchors)) settled_keys.insert(anchors.begin(), anchors.end());
    }
    if (!settled_keys.empty()) std::erase_if(members, [&](uint64_t k) { return settled_keys.count(k) > 0; });
    // an array just entered with Ctrl+Shift+Enter that reads a cycle: each of its values that reads the cycle shows 0,
    // the others are calculated (Excel 16, measured: {=A1:A2+5} with A1 =A1+1 shows 0 and 5), and what reads it after
    {
      const std::unordered_set<uint64_t> in_cycle(members.begin(), members.end());
      for (const uint64_t k : cells) {
        if (in_cycle.count(k) || settled_keys.count(k) || !fresh.count(k)) continue;
        const Sheet* ks = S(KeySheet(k));
        const model::SpillRange* ksp = ks ? ks->SpillOf(KeyRow(k), KeyCol(k)) : nullptr;
        if (!ksp || !ksp->cse || ksp->rect.r0 != KeyRow(k) || ksp->rect.c0 != KeyCol(k)) continue;
        Value v;
        std::shared_ptr<formula::Array> arr;
        bool ok = true;
        host_->cycle_as_unset = true;
        try {
          v = EvaluateFormulaCell(k, &arr);
        } catch (const formula::NeedCell&) {  // it reads a cell still waiting
          ok = false;
        } catch (const std::exception&) {
          ok = false;
        }
        host_->cycle_as_unset = false;
        if (!ok) continue;
        auto zero_unset = [](Value& x) {
          if (x.IsError() && x.error() == core::Err::Blocked) x = Value::Number(0);
        };
        zero_unset(v);
        if (arr) {
          auto copy = std::make_shared<formula::Array>(*arr);
          for (auto& x : copy->v) zero_unset(x);
          arr = std::move(copy);
        }
        cycle_block_.erase(k);
        StoreResult(k, std::move(v), std::move(arr));
        ErasePending(k);
        settled_keys.insert(k);
      }
    }
    const bool settled = !settled_keys.empty();
    if (settled) {
      // what was set aside with it waits again; what reads a cycle still reported is set aside again
      const std::unordered_set<uint64_t> still(members.begin(), members.end());
      for (const uint64_t k : cells) {
        if (still.count(k) || settled_keys.count(k)) continue;
        cycle_block_.erase(k);
        const Sheet* cs = S(KeySheet(k));
        if (cs && cs->GetFormula(KeyRow(k), KeyCol(k))) AddPending(k);
      }
    }
    // reported, in the order found (the status bar names the first one)
    AppendNew(circular_cells, members);
    return;  // the cells stay blocked for the rest of this calculation
  }
  // iterative calculation (Excel): every round calculates each cell of the cycles, and each cell that reads them, once
  // in calculation order from the values the others have then; MaxIterations rounds at most, fewer when no value of
  // a round changed by MaxChange or more
  AppendNew(iter_cells_, members);
  for (uint64_t k : cells) cycle_block_.erase(k);
  const int rounds = std::max(1, max_iterations);
  for (int round = 0; round < rounds; ++round) {
    round_change_ = 0;
    for (uint64_t k : cells) {
      Value v;
      std::shared_ptr<formula::Array> arr;
      try {
        v = EvaluateFormulaCell(k, &arr);
      } catch (const formula::NeedCell&) {
        continue;  // reads a cell that is still waiting: it keeps its value this round
      } catch (const std::exception&) {
        v = Value::Error(core::Err::Calc);
      }
      ++stats_.evaluated;
      StoreResult(k, std::move(v), std::move(arr));
    }
    ++stats_.iterations;
    if (round_change_ < max_change) break;
  }
}

bool Document::SettleArrayReadingItself(uint64_t key, uint64_t* waits_for) {
  const uint32_t sheet = KeySheet(key), r = KeyRow(key), c = KeyCol(key);
  Sheet* s = M(sheet);
  const model::SpillRange* sp = s ? s->SpillOf(r, c) : nullptr;
  if (!sp || !sp->cse || sp->rect.r0 != r || sp->rect.c0 != c) return false;
  const Rect t = sp->rect;
  constexpr uint64_t kMaxCells = 4096;  // each round calculates all of it: a round a value at most
  if (t.Count() > kMaxCells) return false;
  const uint32_t rows = static_cast<uint32_t>(t.Rows()), cols = static_cast<uint32_t>(t.Cols());
  // its cells read as a value nothing calculates to until each is settled: a value of it that reads one of them
  // waits, and one that reads itself (or another that waits for it) never settles
  const Value unset = Value::Error(core::Err::Blocked);
  std::vector<Value> before(static_cast<size_t>(t.Count()));
  for (uint32_t i = 0; i < rows; ++i) {
    for (uint32_t j = 0; j < cols; ++j) before[size_t{i} * cols + j] = s->GetValue(t.r0 + i, t.c0 + j);
  }
  auto put_back = [&] {
    Sheet* ms = M(sheet);
    for (uint32_t i = 0; i < rows; ++i) {
      for (uint32_t j = 0; j < cols; ++j) ms->SetValue(t.r0 + i, t.c0 + j, before[size_t{i} * cols + j]);
    }
  };
  // a value of the result where UpdateSpill puts it (one row or column repeats, the rest is #N/A, empty is 0)
  auto value_at = [&](const Value& v, const formula::Array* arr, uint32_t i, uint32_t j) -> Value {
    if (!arr) return v;
    const uint32_t ii = arr->rows == 1 ? 0 : i, jj = arr->cols == 1 ? 0 : j;
    if (ii >= arr->rows || jj >= arr->cols) return Value::Error(core::Err::NA);
    Value x = arr->At(ii, jj);
    if (x.IsEmpty()) x = Value::Number(0);
    return x;
  };
  // it is the one calculated now: what it reads of itself isn't waiting for it
  const bool blocked = cycle_block_.erase(key) > 0;
  const bool was_pending = IsPending(key);
  if (was_pending) ErasePending(key);
  uint64_t need = ~0ull;
  auto evaluate = [&](Value* v, std::shared_ptr<formula::Array>* arr) {
    try {
      *v = EvaluateFormulaCell(key, arr);
      return true;
    } catch (const formula::NeedCell& n) {  // it reads a cell still waiting (another cycle's, or one not calculated yet)
      need = Key(static_cast<uint32_t>(n.sheet), n.row, n.col);
      return false;
    } catch (const std::exception&) {
      return false;
    }
  };
  {
    Sheet* ms = M(sheet);
    for (uint32_t i = 0; i < rows; ++i) {
      for (uint32_t j = 0; j < cols; ++j) ms->SetValue(t.r0 + i, t.c0 + j, unset);
    }
  }
  std::vector<char> done(static_cast<size_t>(t.Count()), 0);
  size_t left = done.size();
  Value v;
  std::shared_ptr<formula::Array> arr;
  bool ok = true;
  while (ok && left > 0) {
    if (!evaluate(&v, &arr)) {
      ok = false;
      break;
    }
    ++stats_.evaluated;
    size_t now = 0;
    Sheet* ms = M(sheet);
    for (uint32_t i = 0; i < rows; ++i) {
      for (uint32_t j = 0; j < cols; ++j) {
        const size_t at = size_t{i} * cols + j;
        if (done[at]) continue;
        Value x = value_at(v, arr.get(), i, j);
        if (x.IsError() && x.error() == core::Err::Blocked) continue;
        ms->SetValue(t.r0 + i, t.c0 + j, std::move(x));
        done[at] = 1;
        ++now;
      }
    }
    if (now == 0) ok = false;  // the rest wait for each other: a circular reference
    left -= now;
  }
  // calculated once more from the values settled, it gives them again (a value that counts its own cells, which
  // read as no number while they waited, doesn't)
  if (ok) {
    ok = evaluate(&v, &arr);
    for (uint32_t i = 0; ok && i < rows; ++i) {
      for (uint32_t j = 0; ok && j < cols; ++j) ok = value_at(v, arr.get(), i, j).SameAs(S(sheet)->GetValue(t.r0 + i, t.c0 + j));
    }
  }
  put_back();
  if (!ok) {
    if (blocked) cycle_block_.insert(key);
    if (was_pending) AddPending(key);
    if (waits_for) *waits_for = need;
    return false;
  }
  StoreResult(key, std::move(v), std::move(arr));
  // its values marked what reads them, itself among them: it is calculated, what reads it isn't yet
  ErasePending(key);
  std::erase(new_pending_, key);
  return true;
}

bool Document::SettleArraysTogether(const std::vector<uint64_t>& anchors) {
  struct Arr {
    uint64_t key;
    uint32_t sheet;
    Rect rect;
    std::vector<Value> before;
    std::vector<char> done;
    Value v;
    std::shared_ptr<formula::Array> arr;
    bool blocked, pending;
  };
  std::vector<Arr> list;
  uint64_t cells = 0;
  for (const uint64_t key : anchors) {
    const uint32_t sheet = KeySheet(key), r = KeyRow(key), c = KeyCol(key);
    const Sheet* s = S(sheet);
    const model::SpillRange* sp = s ? s->SpillOf(r, c) : nullptr;
    if (!sp || !sp->cse || sp->rect.r0 != r || sp->rect.c0 != c) return false;
    cells += sp->rect.Count();
    if (cells > 4096) return false;  // each round calculates all of them: a round a value at most
    Arr a{key, sheet, sp->rect, {}, std::vector<char>(static_cast<size_t>(sp->rect.Count()), 0), {}, nullptr, false, false};
    for (uint32_t i = sp->rect.r0; i <= sp->rect.r1; ++i) {
      for (uint32_t j = sp->rect.c0; j <= sp->rect.c1; ++j) a.before.push_back(s->GetValue(i, j));
    }
    list.push_back(std::move(a));
  }
  if (list.size() < 2) return false;
  // a value of a result where UpdateSpill puts it (one row or column repeats, the rest is #N/A, empty is 0)
  auto value_at = [](const Value& v, const formula::Array* arr, uint32_t i, uint32_t j) -> Value {
    if (!arr) return v;
    const uint32_t ii = arr->rows == 1 ? 0 : i, jj = arr->cols == 1 ? 0 : j;
    if (ii >= arr->rows || jj >= arr->cols) return Value::Error(core::Err::NA);
    Value x = arr->At(ii, jj);
    if (x.IsEmpty()) x = Value::Number(0);
    return x;
  };
  auto evaluate = [&](Arr& a) {
    try {
      a.v = EvaluateFormulaCell(a.key, &a.arr);
      return true;
    } catch (const formula::NeedCell&) {  // it reads a cell still waiting (of a cycle outside these arrays)
      return false;
    } catch (const std::exception&) {
      return false;
    }
  };
  // every cell of them reads as a value nothing calculates to until it is settled
  const Value unset = Value::Error(core::Err::Blocked);
  for (Arr& a : list) {
    a.blocked = cycle_block_.erase(a.key) > 0;
    a.pending = IsPending(a.key);
    if (a.pending) ErasePending(a.key);
    Sheet* ms = M(a.sheet);
    for (uint32_t i = a.rect.r0; i <= a.rect.r1; ++i) {
      for (uint32_t j = a.rect.c0; j <= a.rect.c1; ++j) ms->SetValue(i, j, unset);
    }
  }
  bool ok = true;
  for (size_t left = cells; ok && left > 0;) {
    size_t now = 0;
    for (Arr& a : list) {
      if (std::all_of(a.done.begin(), a.done.end(), [](char x) { return x != 0; })) continue;
      if (!evaluate(a)) {
        ok = false;
        break;
      }
      ++stats_.evaluated;
      Sheet* ms = M(a.sheet);
      const uint32_t rows = static_cast<uint32_t>(a.rect.Rows()), cols = static_cast<uint32_t>(a.rect.Cols());
      for (uint32_t i = 0; i < rows; ++i) {
        for (uint32_t j = 0; j < cols; ++j) {
          const size_t at = size_t{i} * cols + j;
          if (a.done[at]) continue;
          Value x = value_at(a.v, a.arr.get(), i, j);
          if (x.IsError() && x.error() == core::Err::Blocked) continue;
          ms->SetValue(a.rect.r0 + i, a.rect.c0 + j, std::move(x));
          a.done[at] = 1;
          ++now;
        }
      }
    }
    if (now == 0) ok = false;  // the rest wait for each other: a circular reference
    left -= std::min(left, now);
  }
  // calculated once more from the values settled, each gives them again
  for (size_t n = 0; ok && n < list.size(); ++n) {
    Arr& a = list[n];
    ok = evaluate(a);
    const uint32_t rows = static_cast<uint32_t>(a.rect.Rows()), cols = static_cast<uint32_t>(a.rect.Cols());
    for (uint32_t i = 0; ok && i < rows; ++i) {
      for (uint32_t j = 0; ok && j < cols; ++j) ok = value_at(a.v, a.arr.get(), i, j).SameAs(S(a.sheet)->GetValue(a.rect.r0 + i, a.rect.c0 + j));
    }
  }
  for (const Arr& a : list) {  // the values from before: StoreResult compares with them and marks what reads them
    Sheet* ms = M(a.sheet);
    size_t at = 0;
    for (uint32_t i = a.rect.r0; i <= a.rect.r1; ++i) {
      for (uint32_t j = a.rect.c0; j <= a.rect.c1; ++j) ms->SetValue(i, j, a.before[at++]);
    }
  }
  if (!ok) {
    for (const Arr& a : list) {
      if (a.blocked) cycle_block_.insert(a.key);
      if (a.pending) AddPending(a.key);
    }
    return false;
  }
  for (Arr& a : list) {
    StoreResult(a.key, std::move(a.v), std::move(a.arr));
    ErasePending(a.key);
    std::erase(new_pending_, a.key);
  }
  return true;
}

void Document::CalculateNow() {
  MarkAllVolatileDirty();
  force_iteration_ = true;
  Recalc();
  force_iteration_ = false;
  // F9 also calculates the data tables that automatic calculation leaves out
  bool any_table = false;
  for (const auto& sp : wb_.sheets) any_table = any_table || !sp->data_tables->empty();
  if (any_table && !in_tables_ && (tables_dirty_ || !auto_data_tables || !auto_calc)) RecalcDataTables();
}

void Document::RecalcSheet(uint32_t sheet) { RecalcOnly(sheet, Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1}, false); }

void Document::MarkDirty(uint32_t sheet, const Rect& rect_in) {
  const Sheet* s = S(sheet);
  if (!s || !s->has_used) return;
  const Rect rect{rect_in.r0, rect_in.c0, std::min(rect_in.r1, s->used.r1), std::min(rect_in.c1, s->used.c1)};
  if (rect.r0 > rect.r1 || rect.c0 > rect.c1) return;
  s->meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
    if (t.formula_count == 0) return;
    for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
      if (!t.formula[idx]) continue;
      const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
      if (rect.Contains(r, c)) AddPending(Key(sheet, r, c));
    }
  });
}

void Document::RecalcRange(uint32_t sheet, const Rect& rect_in) {
  Rect rect = rect_in;
  rect.r1 = std::min(rect.r1, core::kMaxRows - 1);
  rect.c1 = std::min(rect.c1, core::kMaxCols - 1);
  if (rect.r0 > rect.r1 || rect.c0 > rect.c1) return;
  RecalcOnly(sheet, rect, true);
}

void Document::RecalcOnly(uint32_t sheet, const Rect& rect, bool every_formula) {
  const Sheet* s = S(sheet);
  if (!s) return;
  // the sheet's (range's) volatile formulas; the others wait (they are calculated with the rest of the workbook)
  for (uint64_t k : volatile_) {
    if (KeySheet(k) == sheet && rect.Contains(KeyRow(k), KeyCol(k))) AddPending(k);
  }
  // Range.Calculate calculates every formula of the range, whether it waits or not (Excel)
  if (every_formula) {
    std::vector<uint64_t> cells;
    ForEachCellInTiles(s->meta, rect, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
      if (t.formula[idx]) cells.push_back(Key(sheet, r, c));
    });
    for (uint64_t k : cells) AddPending(k);
  }
  only_sheet_ = sheet;
  only_rect_ = rect;
  BeginCycles(sheet, rect);
  if (!pending_.empty()) {
    EvaluatePendingSerial(sheet);
    for (int pass = 0; pass < 64 && !new_pending_.empty(); ++pass) {
      Propagate();
      EvaluatePendingSerial(sheet);
    }
    ResolveCycles();
  }
  // a formula calculated from a value still waiting on another sheet (outside the range) waits too: the next
  // calculation calculates it again from the new value (Excel). Its dependents were marked with it.
  for (uint64_t k : stale_kept_) {
    const Sheet* ks = S(KeySheet(k));
    if (ks && ks->GetFormula(KeyRow(k), KeyCol(k))) AddPending(k);
  }
  stale_kept_.clear();
  stale_hit_ = false;
  only_sheet_ = -1;
  only_rect_ = Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1};
  in_progress_.clear();
  cycle_break_.clear();
  cycle_block_.clear();
  cycle_aborted_.clear();
  cycle_members_.clear();
  fresh_aborted_.clear();
  new_pending_.clear();
  std::erase_if(circular_cells, [&](uint64_t k) {
    const Sheet* cs = S(KeySheet(k));
    return !cs || !cs->GetFormula(KeyRow(k), KeyCol(k));
  });
}

bool Document::LivePart(int32_t sheet, Rect* rect) const {
  if (stale_reads_) return false;
  if (only_sheet_ < 0) return true;
  if (sheet != only_sheet_ || !only_rect_.Intersects(*rect)) return false;
  rect->r0 = std::max(rect->r0, only_rect_.r0);
  rect->c0 = std::max(rect->c0, only_rect_.c0);
  rect->r1 = std::min(rect->r1, only_rect_.r1);
  rect->c1 = std::min(rect->c1, only_rect_.c1);
  return true;
}

bool Document::CycleSpillAt(uint32_t sheet, uint32_t r, uint32_t c, uint64_t* anchor) const {
  auto it = spill_tiles_.find(TileKeyOf(Key(sheet, r, c)));
  if (it == spill_tiles_.end()) return false;
  for (uint64_t a : it->second) {
    if (!cycle_block_.count(a)) continue;
    auto ar = spill_anchor_.find(a);
    if (ar != spill_anchor_.end() && ar->second.Contains(r, c)) {
      *anchor = a;
      return true;
    }
  }
  return false;
}

bool Document::CycleBlockedIn(uint32_t sheet, const Rect& rect, uint64_t* first) const {
  if (cycle_block_.empty()) return false;
  // the smaller side is walked: the cells of the range, or the blocked cells. An array's cells are blocked with its
  // anchor: =SUM(D2:D5) over part of a circular {=C2:D3+1} was calculated from its zeros, where Excel 16 keeps the
  // value it shows, as for a cell of any cycle (measured: cyc_array_ cases)
  if (rect.Count() <= cycle_block_.size()) {
    for (uint32_t c = rect.c0; c <= rect.c1; ++c) {
      for (uint32_t r = rect.r0; r <= rect.r1; ++r) {
        if (cycle_block_.count(Key(sheet, r, c)) || CycleSpillAt(sheet, r, c, first)) {
          if (cycle_block_.count(Key(sheet, r, c))) *first = Key(sheet, r, c);
          return true;
        }
      }
    }
    return false;
  }
  for (uint64_t k : cycle_block_) {
    if (KeySheet(k) != sheet) continue;
    auto ar = spill_anchor_.find(k);
    if (rect.Contains(KeyRow(k), KeyCol(k)) || (ar != spill_anchor_.end() && ar->second.Intersects(rect))) {
      *first = k;
      return true;
    }
  }
  return false;
}

bool Document::ReadsThroughNames(uint64_t key) const {
  const Sheet* s = S(KeySheet(key));
  const formula::CompiledFormula* f = s ? Formula(s->GetFormula(KeyRow(key), KeyCol(key))) : nullptr;
  return f && (f->flags & model::FormulaDef::kDynamicRef);
}

void Document::SetDynamicDeps(uint64_t key, std::vector<std::pair<int32_t, Rect>>& reads) {
  // what a formula reads through a name, OFFSET or INDIRECT is known once it is calculated: its references for the
  // index are its own plus those cells, so a change there (a spill appearing) calculates it again and a cycle through
  // them is met whatever the order (Excel keeps such dependencies the same way). Without them, a cycle through a
  // relative name was found after Redo and not before (doc_fuzz seeds 10553941, 10573473).
  const uint32_t sheet = KeySheet(key), r = KeyRow(key), c = KeyCol(key);
  const Sheet* s = S(sheet);
  const uint32_t fid = s ? s->GetFormula(r, c) : 0;
  const formula::CompiledFormula* f = Formula(fid);
  if (!f) return;
  deps_.Remove(key);
  const RefInfo& info = RefInfoOf(fid, f);
  const core::CellPos at{r, c};
  for (const auto& spec : info.refs) {
    if (spec.lost_sheet) continue;
    formula::ResolvedRef ref;
    if (formula::ResolveRef(spec, at, &ref)) deps_.Add(key, ref.sheet < 0 ? sheet : static_cast<uint32_t>(ref.sheet), ref.rect);
  }
  if (info.table) AddTableDeps(key, sheet, at, f->root);
  std::sort(reads.begin(), reads.end(), [](const auto& a, const auto& b) {
    return std::tie(a.first, a.second.r0, a.second.c0, a.second.r1, a.second.c1) < std::tie(b.first, b.second.r0, b.second.c0, b.second.r1, b.second.c1);
  });
  reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
  size_t n = 0;
  for (const auto& [rs, rect] : reads) {
    if (rs < 0 || IsExternalSheet(rs) || ++n > 4096) continue;
    deps_.Add(key, static_cast<uint32_t>(rs), rect);
  }
}

void Document::EvaluatePendingSerial(int64_t only_sheet) {
  std::vector<std::pair<int32_t, Rect>> reads;
  std::vector<uint64_t> order;
  order.reserve(pending_.size());
  for (uint64_t k : pending_) {
    if (only_sheet < 0 || (KeySheet(k) == static_cast<uint64_t>(only_sheet) && only_rect_.Contains(KeyRow(k), KeyCol(k)))) order.push_back(k);
  }
  std::sort(order.begin(), order.end());
  std::vector<uint64_t> stack;
  for (uint64_t start : order) {
    if (StopRequested()) break;  // between cells: nothing is half calculated
    if (!IsPending(start)) continue;
    stack.clear();
    stack.push_back(start);
    in_progress_.insert(start);
    while (!stack.empty()) {
      const uint64_t top = stack.back();
      Value v;
      std::shared_ptr<formula::Array> arr;
      stale_hit_ = false;
      reads.clear();
      host_->record = ReadsThroughNames(top) ? &reads : nullptr;
      struct Off {
        CalcHost* h;
        ~Off() { h->record = nullptr; }
      } off{host_.get()};
      host_->ResetSelf();
      host_->soft_self = true;
      struct SoftOff {
        CalcHost* h;
        ~SoftOff() { h->soft_self = false, h->ResetSelf(); }
      } soft_off{host_.get()};
      try {
        v = EvaluateFormulaCell(top, &arr);
        if (host_->self_hit()) {
          // it read its own cell: a circular reference, as the NeedCell for it is below
          ++stats_.retries;
          ++stats_.self_reads;
          // an array entered with Ctrl+Shift+Enter that read only its own cells is settled here when no value of it
          // waits for itself, as ResolveCycles settles it after the pass, and what waits for it below on the stack
          // goes on. Left to ResolveCycles, the readers set aside with it were marked again, marked the array again
          // through what they feed, and met it pending pass after pass: {=A1:B3*2} in C2:D2 kept D2 at 0 after
          // B1 =VLOOKUP(A1,A1:C9,...) beside {=A1:B3*2} in B3:B5 (doc_fuzz --xlsx seed 24000181430)
          if (!iterate) {
            host_->record = nullptr;
            host_->soft_self = false;
            host_->ResetSelf();
            in_progress_.erase(top);
            uint64_t need = ~0ull;
            if (SettleArrayReadingItself(top, &need)) {
              stack.pop_back();
              continue;
            }
            in_progress_.insert(top);
            // its values read a cell not calculated yet (the read of its own cell stopped the formula before it got
            // there): that cell is calculated first and the array settled again, as any formula waits for what it
            // reads. Taken for a cycle, B1 {=A1:B2*2} met B2 still waiting at every pass, B1 was reported circular
            // or set aside what read it, and =TRANSPOSE(A1:B2) in B4 showed 0 unspilled (doc_fuzz --xlsx seed 2400534)
            if (need != ~0ull && need != top && IsPending(need) && !in_progress_.count(need)) {
              stack.push_back(need);
              in_progress_.insert(need);
              continue;
            }
          }
          AbortStack(stack, top);
          continue;
        }
      } catch (const formula::NeedCell& n) {
        const uint64_t nk = Key(static_cast<uint32_t>(n.sheet), n.row, n.col);
        ++stats_.retries;
        if (in_progress_.count(nk) || !IsPending(nk)) {
          // a circular reference, or a cell that is part of one: nothing on the stack is calculated (ResolveCycles)
          AbortStack(stack, nk);
        } else {
          stack.push_back(nk);
          in_progress_.insert(nk);
        }
        continue;
      } catch (const std::exception&) {
        v = Value::Error(core::Err::Calc);
      }
      ++stats_.evaluated;
      if (stale_hit_ && only_sheet_ >= 0) stale_kept_.push_back(top);
      if (host_->record) SetDynamicDeps(top, reads);
      StoreResult(top, std::move(v), std::move(arr));
      in_progress_.erase(top);
      stack.pop_back();
    }
  }
  in_progress_.clear();
}

void Document::StoreResult(uint64_t key, Value v, std::shared_ptr<formula::Array> arr) {
  const uint32_t sheet = KeySheet(key), r = KeyRow(key), c = KeyCol(key);
  // an array that spills over cells it reads is a circular reference: its spill marks the anchor itself pending, and
  // it stays pending so the next pass meets the cycle. Taken off at the end of this function, the first calculation
  // spilled as if nothing read the cells, and Redo (where the spill came back first) found the cycle: two results
  // for one workbook (doc_fuzz seed 23, a relative name read from the anchor)
  storing_ = key;
  storing_read_itself_ = false;
  spill_moved_ = false;
  if (arr || (!spill_anchor_.empty() && spill_anchor_.count(key))) UpdateSpill(key, &v, arr);
  storing_ = ~0ull;
  bool again = storing_read_itself_ && spill_anchor_.count(key);
  // what a name, OFFSET or INDIRECT reads isn't in the dependency index: such an anchor is calculated once more when
  // its spill appears or moves, and meets the cycle then if it reads its own spill
  if (!again && spill_moved_ && spill_anchor_.count(key)) {
    const Sheet* fs = S(sheet);
    const formula::CompiledFormula* f = fs ? Formula(fs->GetFormula(r, c)) : nullptr;
    again = f && (f->flags & model::FormulaDef::kDynamicRef) && respill_checked_.insert(key).second;
  }
  // this sheet was calculated (Worksheet_Calculate), whether or not the value changed
  if (calc_sheets_.empty() || calc_sheets_.back() != sheet) {
    if (std::find(calc_sheets_.begin(), calc_sheets_.end(), sheet) == calc_sheets_.end()) calc_sheets_.push_back(sheet); // lesson-ok: find-push the sheets
  }
  Sheet* s = M(sheet);
  if (wb_.precision_as_displayed) RoundShown(sheet, r, c, &v);  // a result keeps the digits its format shows
  if (s && !s->GetValue(r, c).SameAs(v)) {
    const Value& old = s->GetValue(r, c);
    round_change_ = std::max(round_change_, old.IsNumeric() && v.IsNumeric() ? std::fabs(v.AsDouble() - old.AsDouble()) : 1.0);
    s->SetValue(r, c, std::move(v));
    if (calc_change_cells_ < kMaxCalcChanges) {
      ++calc_change_cells_;
      changes_.push_back(CellChange{sheet, Rect::Cell(r, c), kSrcCalc});
    }
  }
  if (!again) ErasePending(key);
  // checked again, it is still the formula just entered: met as a cycle, it shows 0 (and no spill), as after Redo
  if (!again && !fresh_formulas_.empty()) fresh_formulas_.erase(key);
}

void Document::RecalcWaves() {
  // the worker threads only read the model: finish the lazy rebuilds (row / column index) first
  wb_.FinalizeForSnapshot();
  std::vector<uint64_t> cand;
  cand.reserve(pending_.size());
  for (uint64_t k : pending_) {
    const Sheet* s = S(KeySheet(k));
    const formula::CompiledFormula* f = s ? Formula(s->GetFormula(KeyRow(k), KeyCol(k))) : nullptr;
    if (!f || (f->flags & (model::FormulaDef::kUdf | model::FormulaDef::kStream))) continue;  // side effects: serial pass
    if (!spill_anchor_.empty() && spill_anchor_.count(k)) continue;  // spills write other cells: serial pass
    cand.push_back(k);
  }
  std::sort(cand.begin(), cand.end());  // neighbors share tiles: better locality per chunk
  enum : uint8_t { kBlocked = 0, kDone = 1, kSerial = 2 };
  std::vector<Value> out;
  std::vector<uint8_t> state;
  std::vector<uint64_t> next;
  std::vector<std::vector<std::pair<int32_t, Rect>>> reads;  // per formula reading through names (SetDynamicDeps)
  while (cand.size() >= kParallelMin && !StopRequested()) {
    out.assign(cand.size(), Value());
    state.assign(cand.size(), kBlocked);
    reads.assign(cand.size(), {});
    const auto t_eval = std::chrono::steady_clock::now();
    core::ParallelFor(cand.size(), 64, [&](size_t b, size_t e) {
      CalcHost host(*this, true);
      for (size_t i = b; i < e; ++i) {
        const uint64_t k = cand[i];
        const uint32_t sheet = KeySheet(k), r = KeyRow(k), c = KeyCol(k);
        const Sheet* s = S(sheet);
        const formula::CompiledFormula* f = s ? Formula(s->GetFormula(r, c)) : nullptr;
        if (!f) {
          state[i] = kSerial;
          continue;
        }
        host.ResetFlags();
        host.record = (f->flags & model::FormulaDef::kDynamicRef) ? &reads[i] : nullptr;
        std::shared_ptr<formula::Array> arr;
        try {
          out[i] = formula::EvaluateCell(f->root, host, static_cast<int32_t>(sheet), core::CellPos{r, c}, &arr);
        } catch (const std::exception&) {
          out[i] = Value::Error(core::Err::Calc);
        } catch (const formula::NeedCell&) {
          host.ResetFlags();  // not thrown by a parallel host; be safe: retry in the serial pass
          state[i] = kSerial;
          continue;
        }
        // read a still-pending cell: the value is meaningless, try again in the next wave
        state[i] = host.serial() || arr ? kSerial : host.blocked() ? kBlocked : kDone;  // a new spill: serial pass
      }
    });
    const auto t_commit = std::chrono::steady_clock::now();
    stats_.wave_eval_ms += std::chrono::duration<double, std::milli>(t_commit - t_eval).count();
    // write back on the engine thread (single writer); cells that weren't done stay pending
    size_t done = 0;
    next.clear();
    for (size_t i = 0; i < cand.size(); ++i) {
      if (state[i] == kDone) {
        if (ReadsThroughNames(cand[i])) SetDynamicDeps(cand[i], reads[i]);
        StoreResult(cand[i], std::move(out[i]));
        ++done;
      } else if (state[i] == kBlocked) {
        next.push_back(cand[i]);
      }
    }
    stats_.wave_commit_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_commit).count();
    stats_.evaluated += static_cast<uint32_t>(done);
    stats_.parallel += static_cast<uint32_t>(done);
    stats_.retries += static_cast<uint32_t>(next.size());
    ++stats_.waves;
    // little progress means a long dependency chain or a cycle: the serial pass handles those in linear time
    if (done == 0 || done * 8 < cand.size()) break;
    cand.swap(next);
  }
}

void Document::RecalcAll() {
  for (const auto& sc : wb_.sheets) {
    const Sheet& s = sc.get();
    s.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        if (t.formula[idx]) AddPending(Key(s.id, rt * model::kTileRows + idx % model::kTileRows, ct * model::kTileCols + idx / model::kTileRows));
      }
    });
  }
  InvalidateUdfCache();
  Recalc();
}

// ---- Dynamic arrays (spills) ----

bool Document::SpillAt(uint32_t sheet, uint32_t r, uint32_t c, uint32_t* anchor_r, uint32_t* anchor_c, Rect* rect) const {
  const Sheet* s = S(sheet);
  const model::SpillRange* sp = s ? s->SpillAt(r, c, anchor_r, anchor_c) : nullptr;
  if (!sp) return false;
  if (rect) *rect = sp->rect;
  return true;
}

void Document::RebuildSpillIndex() {
  spill_anchor_.clear();
  spill_tiles_.clear();
  pending_spills_.clear();
  for (const auto& sc : wb_.sheets) {
    for (const auto& [k, sp] : *sc->spills) {
      const uint64_t a = Key(sc->id, static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF));
      spill_anchor_[a] = sp.rect;
      IndexSpill(a, sp.rect, true);
      if (IsPending(a)) pending_spills_.insert(a);
    }
  }
}

void Document::IndexSpill(uint64_t anchor, const Rect& rect, bool add) {
  const uint32_t sheet = KeySheet(anchor);
  for (uint32_t rt = rect.r0 / model::kTileRows; rt <= rect.r1 / model::kTileRows; ++rt) {
    for (uint32_t ct = rect.c0 / model::kTileCols; ct <= rect.c1 / model::kTileCols; ++ct) {
      const uint64_t tk = (uint64_t{sheet} << 48) | (uint64_t{rt} << 16) | ct;
      if (add) {
        spill_tiles_[tk].push_back(anchor);
        continue;
      }
      auto it = spill_tiles_.find(tk);
      if (it == spill_tiles_.end()) continue;
      auto& v = it->second;
      v.erase(std::remove(v.begin(), v.end(), anchor), v.end());
      if (v.empty()) spill_tiles_.erase(it);
    }
  }
}

bool Document::PendingSpillAt(uint32_t sheet, uint32_t r, uint32_t c, uint64_t* anchor) const {
  auto it = spill_tiles_.find(TileKeyOf(Key(sheet, r, c)));
  if (it == spill_tiles_.end()) return false;
  for (uint64_t a : it->second) {
    // an anchor counts only while it is pending: one left in the index would make readers wait for it forever
    if (!pending_spills_.count(a) || !IsPending(a) || cycle_break_.count(a)) continue;
    auto ar = spill_anchor_.find(a);
    if (ar != spill_anchor_.end() && ar->second.Contains(r, c)) {
      *anchor = a;
      return true;
    }
  }
  return false;
}

bool Document::PendingSpillIn(uint32_t sheet, const Rect& rect, uint64_t* anchor) const {
  for (uint64_t a : pending_spills_) {
    if (KeySheet(a) != sheet || !IsPending(a) || cycle_break_.count(a)) continue;
    auto ar = spill_anchor_.find(a);
    if (ar != spill_anchor_.end() && ar->second.Intersects(rect)) {
      *anchor = a;
      return true;
    }
  }
  return false;
}


// STOCKHISTORY: its date column is shown as dates (like Excel) where the cells have no number format of their own
void Document::FormatStockDates(uint32_t sheet, const Rect& t, const formula::Array& a, const formula::Node& root) {
  std::vector<uint32_t> cols;
  uint32_t first_row = 0;
  for (uint32_t i = 0; i < a.rows && cols.empty(); ++i) {
    for (uint32_t j = 0; j < a.cols; ++j) {
      const Value& x = a.At(i, j);
      if (x.IsString() && x.str() == "Date") cols.push_back(j), first_row = i + 1;
    }
  }
  if (cols.empty()) {  // no headers: the columns asked for property 0 (the date; the default first column)
    if (root.kids.size() <= 5) cols.push_back(0);
    for (size_t k = 5; k < root.kids.size(); ++k) {
      if (root.kids[k].kind == formula::NK::Num && root.kids[k].num == 0) cols.push_back(static_cast<uint32_t>(k - 5));
    }
  }
  if (cols.empty() || first_row >= a.rows) return;
  Sheet* s = M(sheet);
  auto& styles = wb_.styles.mut();
  const uint32_t code = styles.InternString(DateFormatCode(false));
  for (uint32_t j : cols) {
    for (uint32_t i = first_row; i < a.rows && t.r0 + i <= t.r1; ++i) {
      const uint32_t rr = t.r0 + i, cc = t.c0 + j;
      model::Style st = styles.Get(s->GetStyle(rr, cc));
      if (st.numfmt) continue;
      st.numfmt = code;
      s->SetStyle(rr, cc, styles.Intern(st));
    }
  }
}

void Document::UpdateSpill(uint64_t key, Value* v, const std::shared_ptr<formula::Array>& arr) {
  const uint32_t sheet = KeySheet(key), r = KeyRow(key), c = KeyCol(key);
  Sheet* s = M(sheet);
  if (!s) return;
  const uint64_t rc = (uint64_t{r} << 16) | c;
  const model::SpillRange* cur = s->SpillOf(r, c);
  const bool had = cur != nullptr, cse = cur && cur->cse;
  const Rect old = had ? cur->rect : Rect::Cell(r, c);

  Rect t = old;
  if (!spill_blocked_.empty()) spill_blocked_.erase(key);
  if (!cse) {
    if (!arr) {
      if (had) ClearSpill(key, nullptr);
      return;
    }
    // the target range; anything in the way (other than our own members) blocks it
    const uint64_t r1 = uint64_t{r} + arr->rows - 1, c1 = uint64_t{c} + arr->cols - 1;
    bool blocked = r1 >= core::kMaxRows || c1 >= core::kMaxCols;
    if (!blocked) {
      t = Rect{r, c, static_cast<uint32_t>(r1), static_cast<uint32_t>(c1)};
      blocked = SpillBlocked(key, t, had ? &old : nullptr);
    }
    if (blocked) {
      if (had) ClearSpill(key, nullptr);
      *v = Value::Error(core::Err::Spill);
      // remember where it wanted to go: a change there evaluates it again
      spill_blocked_[key] = Rect{r, c, static_cast<uint32_t>(std::min<uint64_t>(r1, core::kMaxRows - 1)),
                                 static_cast<uint32_t>(std::min<uint64_t>(c1, core::kMaxCols - 1))};
      return;
    }
    if (t.r1 >= s->row_limit) {
      EnsureRowLimit(sheet, t.r1);
      s = M(sheet);
    }
  }

  // member values (an array formula fits its result to its fixed range: one row / column repeats, the rest is #N/A)
  auto value_at = [&](uint32_t i, uint32_t j) -> Value {
    if (!arr) return *v;
    const uint32_t ii = arr->rows == 1 ? 0 : i, jj = arr->cols == 1 ? 0 : j;
    if (ii >= arr->rows || jj >= arr->cols) return Value::Error(core::Err::NA);
    Value x = arr->At(ii, jj);
    if (x.IsEmpty()) x = Value::Number(0);
    return x;
  };
  bool changed = false;
  for (uint32_t i = 0; i < t.Rows(); ++i) {
    for (uint32_t j = 0; j < t.Cols(); ++j) {
      if (i == 0 && j == 0) continue;
      Value x = value_at(i, j);
      // precision as displayed: each cell an array fills keeps the digits its own format shows, as the anchor does in
      // StoreResult (Excel 16, measured: =SEQUENCE(3)/3 in 0.00 spills 0.33, 0.67, 1). Rounded only when the setting
      // came on, Redo spilled the full values again (doc_fuzz --gaps 714889)
      if (wb_.precision_as_displayed) RoundShown(sheet, t.r0 + i, t.c0 + j, &x);
      const Value& now = s->GetValue(t.r0 + i, t.c0 + j);
      if (now.SameAs(x)) continue;
      round_change_ = std::max(round_change_, now.IsNumeric() && x.IsNumeric() ? std::fabs(x.AsDouble() - now.AsDouble()) : 1.0);
      s->SetValue(t.r0 + i, t.c0 + j, std::move(x));
      changed = true;
    }
  }
  if (arr) *v = value_at(0, 0);
  if (arr && !cse) {
    if (const formula::CompiledFormula* f = Formula(s->GetFormula(r, c))) {
      const formula::Node& root = RootCall(f->root);
      if (root.kind == formula::NK::Func && root.str == "STOCKHISTORY") {
        FormatStockDates(sheet, t, *arr, root);
        s = M(sheet);
      }
    }
  }
  if (cse) {
    if (changed) {
      changes_.push_back(CellChange{sheet, t, kSrcCalc});
      MarkDependentsOf(sheet, t);
    }
    return;
  }
  // cells left over from a larger range
  if (had && !(old == t)) {
    for (const Rect& g : SubtractRect(old, t)) {
      std::vector<std::pair<uint32_t, uint32_t>> cells;
      ForEachCellInTiles(s->values, g, [&](uint32_t rr, uint32_t cc, const model::ValueTile& tile, uint32_t idx) {
        if (tile.Occupied(idx)) cells.emplace_back(rr, cc);
      });
      for (auto [rr, cc] : cells) s->SetValue(rr, cc, Value());
      if (!spill_blocked_.empty()) WakeBlockedSpills(sheet, g);
    }
  }
  if (!had || !(old == t)) {
    spill_moved_ = true;
    s->spills.mut()[rc] = model::SpillRange{t, false};
    if (had) IndexSpill(key, old, false);
    IndexSpill(key, t, true);
    spill_anchor_[key] = t;
    if (IsPending(key)) pending_spills_.insert(key);
    changed = true;
    MeetSpillCycle(key, t);
  }
  if (changed) {
    const Rect all = had ? t.Union(old) : t;
    changes_.push_back(CellChange{sheet, all, kSrcCalc});
    MarkDependentsOf(sheet, all);
    spills_changed_ = true;
  }
}

void Document::ClearSpill(uint64_t anchor, const Rect* keep) {
  const uint32_t sheet = KeySheet(anchor), r = KeyRow(anchor), c = KeyCol(anchor);
  auto ai = spill_anchor_.find(anchor);
  if (ai != spill_anchor_.end()) {
    IndexSpill(anchor, ai->second, false);
    spill_anchor_.erase(ai);
  }
  pending_spills_.erase(anchor);
  Sheet* s = M(sheet);
  const model::SpillRange* sp = s ? s->SpillOf(r, c) : nullptr;
  if (!sp) return;
  const Rect rect = sp->rect;
  s->spills.mut().erase((uint64_t{r} << 16) | c);
  std::vector<std::pair<uint32_t, uint32_t>> cells;
  ForEachCellInTiles(s->values, rect, [&](uint32_t rr, uint32_t cc, const model::ValueTile& tile, uint32_t idx) {
    if (!tile.Occupied(idx) || (rr == r && cc == c) || (keep && keep->Contains(rr, cc))) return;
    cells.emplace_back(rr, cc);
  });
  for (auto [rr, cc] : cells) s->SetValue(rr, cc, Value());
  changes_.push_back(CellChange{sheet, rect, kSrcCalc});
  MarkDependentsOf(sheet, rect);
  if (!cells.empty()) spills_changed_ = true;
  if (!spill_blocked_.empty()) WakeBlockedSpills(sheet, rect);
}

void Document::UnspillTouched(uint32_t sheet, const Rect& rect) {
  if (!spill_hook_) return;
  std::vector<uint64_t> hit;
  for (const auto& [a, rr] : spill_anchor_) {
    if (KeySheet(a) != sheet || !rr.Intersects(rect)) continue;
    if (rr.IsCell() || rect == Rect::Cell(KeyRow(a), KeyCol(a))) continue;  // only the anchor changed: its formula was re-entered
    hit.push_back(a);
  }
  for (uint64_t a : hit) {
    const Sheet* s = S(sheet);
    const model::SpillRange* sp = s ? s->SpillOf(KeyRow(a), KeyCol(a)) : nullptr;
    if (sp && sp->cse) continue;  // array formulas are edited as a whole (the UI refuses partial edits)
    ClearSpill(a, &rect);  // what was written stays; the anchor spills again over it -> #SPILL! unless it was cleared
    if (s && s->GetFormula(KeyRow(a), KeyCol(a))) AddPending(a);
  }
}

void Document::ClearSheetSpills(uint32_t sheet) {
  std::vector<uint64_t> list;
  const Sheet* s = S(sheet);
  if (!s) return;
  for (const auto& [k, sp] : *s->spills) {
    if (!sp.cse) list.push_back(Key(sheet, static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF)));
  }
  for (uint64_t a : list) ClearSpill(a, nullptr);
}

bool Document::SpillBlocked(uint64_t key, const Rect& t, const Rect* own) const {
  const uint32_t sheet = KeySheet(key), r = KeyRow(key), c = KeyCol(key);
  const Sheet* s = S(sheet);
  if (!s) return true;
  bool blocked = false;
  // a value or a formula in the way (the anchor's own cells and its spilled values aren't)
  auto foreign = [&](uint32_t rr, uint32_t cc) { return !(rr == r && cc == c) && !(own && own->Contains(rr, cc)); };
  ForEachCellInTiles(s->values, t, [&](uint32_t rr, uint32_t cc, const model::ValueTile& tile, uint32_t idx) {
    if (!blocked && tile.Occupied(idx) && foreign(rr, cc)) blocked = true;
  });
  ForEachCellInTiles(s->meta, t, [&](uint32_t rr, uint32_t cc, const model::MetaTile& tile, uint32_t idx) {
    if (!blocked && tile.formula[idx] && !(rr == r && cc == c)) blocked = true;
  });
  for (const Rect& m : *s->merges) blocked = blocked || m.Intersects(t);
  // a table's cells: a dynamic array doesn't spill in or into a table (Excel 16, measured: =SEQUENCE(2) typed in a
  // table and =SEQUENCE(6) above one are #SPILL!); a single value is no spill
  if (!t.IsCell()) {
    for (const auto& tb : *s->tables) blocked = blocked || tb.range.Intersects(t);
  }
  // another array's cells, whether or not they hold their values yet: two arrays never share a cell. An array
  // entered with Ctrl+Shift+Enter over a spill was calculated after it, and the spill had taken its cells
  // (doc_fuzz seed 10130034)
  const uint32_t rt0 = t.r0 / model::kTileRows, rt1 = t.r1 / model::kTileRows;
  const uint32_t ct0 = t.c0 / model::kTileCols, ct1 = t.c1 / model::kTileCols;
  for (uint32_t rt = rt0; rt <= rt1 && !blocked && !spill_tiles_.empty(); ++rt) {
    for (uint32_t ct = ct0; ct <= ct1 && !blocked; ++ct) {
      auto it = spill_tiles_.find((uint64_t{sheet} << 48) | (uint64_t{rt} << 16) | ct);
      if (it == spill_tiles_.end()) continue;
      for (const uint64_t other : it->second) {
        if (other == key) continue;
        auto ar = spill_anchor_.find(other);
        if (ar != spill_anchor_.end() && ar->second.Intersects(t)) {
          blocked = true;
          break;
        }
      }
    }
  }
  return blocked;
}

void Document::TakeSpillsUnder(uint32_t sheet, const Rect& rect) {
  // a table made over cells a dynamic array spilled into takes them empty: its header doesn't take the spilled values,
  // and the array, calculated again, shows #SPILL! (Excel 16, measured: a table over part of =SEQUENCE(3)'s spill has
  // the headers Column1, Column2; doc_fuzz seed 10320462)
  const Sheet* s = S(sheet);
  if (!s) return;
  std::vector<uint64_t> under;
  for (const auto& [k, sp] : *s->spills) {
    if (!sp.cse && sp.rect.Intersects(rect)) under.push_back(Key(sheet, static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF)));
  }
  for (const uint64_t a : under) {
    ClearSpill(a, nullptr);
    AddPending(a);
  }
}

void Document::MeetSpillCycle(uint64_t anchor, const Rect& spill) {
  // an array whose spill feeds, through other formulas, what it reads is a circular reference (Excel: =Rate*2 in A4
  // reading G5 of =TRANSPOSE(B3:C4) in G4, which reads B4 of A4's spill). Calculated one after the other they never
  // waited for each other, and no cycle was met; met after Redo, where the order differed (doc_fuzz seed 10553941).
  // When a spill appears or moves, the formulas that read it are followed (bounded); a path back to the anchor puts
  // the anchor and the path pending together, so the next pass meets the cycle as it meets any other.
  std::unordered_map<uint64_t, uint64_t> from;  // formula -> the one it was reached from
  std::vector<uint64_t> todo;
  const uint32_t sheet = KeySheet(anchor);
  auto reach = [&](uint32_t sh, const Rect& r, uint64_t parent) {
    deps_.QueryRect(sh, r, [&](uint64_t k) {
      if (from.emplace(k, parent).second) todo.push_back(k);
    });
  };
  reach(sheet, spill, anchor);
  for (size_t i = 0; i < todo.size() && i < 20000; ++i) {
    const uint64_t k = todo[i];
    if (k == anchor) {
      for (uint64_t at = from[anchor]; at != anchor && from.count(at); at = from[at]) AddPending(at);
      AddPending(anchor);
      return;
    }
    reach(KeySheet(k), Rect::Cell(KeyRow(k), KeyCol(k)), k);
    auto sp = spill_anchor_.find(k);
    if (sp != spill_anchor_.end()) reach(KeySheet(k), sp->second, k);
  }
}

void Document::RecheckBlockedSpills() {
  // whatever freed a #SPILL! anchor's range (a merge a paste removed, a cell a fill emptied, a path that wrote no
  // change there), it spills on the next calculation, as Excel's: the paths that wake anchors by the cells they write
  // missed a merged area removed under a fill (doc_fuzz seed 10301020). Few anchors wait at a time; a large range
  // waits for a change in it instead.
  // and the other way round: a spill a merged area now crosses (Undo put the merge back, a merge was made over the
  // spilled values) is calculated again and shows #SPILL!, as Excel's
  if (!spill_anchor_.empty() && !spill_tiles_.empty()) {
    std::vector<uint64_t> crossed;
    for (const auto& sc : wb_.sheets) {
      if (sc->merges->empty() || sc->merges->size() > 10000) continue;
      for (const Rect& m : *sc->merges) {
        for (uint32_t rt = m.r0 / model::kTileRows; rt <= m.r1 / model::kTileRows; ++rt) {
          for (uint32_t ct = m.c0 / model::kTileCols; ct <= m.c1 / model::kTileCols; ++ct) {
            auto it = spill_tiles_.find((uint64_t{sc->id} << 48) | (uint64_t{rt} << 16) | ct);
            if (it == spill_tiles_.end()) continue;
            for (const uint64_t a : it->second) {
              auto ar = spill_anchor_.find(a);
              const model::SpillRange* sp = sc->SpillOf(KeyRow(a), KeyCol(a));
              if (ar != spill_anchor_.end() && sp && !sp->cse && ar->second.Intersects(m) && !IsPending(a)) crossed.push_back(a);
            }
          }
        }
      }
    }
    for (const uint64_t a : crossed) AddPending(a);
  }
  if (spill_blocked_.empty() || spill_blocked_.size() > 64) return;
  std::vector<uint64_t> free;
  for (const auto& [k, want] : spill_blocked_) {
    if (IsPending(k) || want.Count() > 4096) continue;
    const Sheet* s = S(KeySheet(k));
    if (!s || !s->GetFormula(KeyRow(k), KeyCol(k))) continue;
    if (!SpillBlocked(k, want, nullptr)) free.push_back(k);
  }
  for (const uint64_t k : free) AddPending(k);
}

void Document::WakeBlockedSpills(uint32_t sheet, const Rect& rect) {
  for (auto it = spill_blocked_.begin(); it != spill_blocked_.end();) {
    const uint64_t a = it->first;
    const Sheet* s = S(KeySheet(a));
    if (!s || !s->GetFormula(KeyRow(a), KeyCol(a))) {
      it = spill_blocked_.erase(it);  // the anchor is gone
      continue;
    }
    if (KeySheet(a) == sheet && it->second.Intersects(rect) && !(rect == Rect::Cell(KeyRow(a), KeyCol(a)))) AddPending(a);
    ++it;
  }
}

void Document::RestoreBlockedSpills(const std::unordered_map<uint64_t, Rect>& blocked) {
  // the restored version's #SPILL! anchors show their error without a calculation: they wait for room again. Entries
  // the version left behind stay (an anchor no longer blocked only calculates once more when they wake it).
  for (const auto& [k, want] : blocked) spill_blocked_.emplace(k, want);
}

void Document::RestoreCircular(const std::vector<uint64_t>& circular) {
  // the restored version's cycles are reported again, and what reads them keeps its value, as before the step: a
  // version taken settled comes back without a calculation, so a cycle only its calculation had found was lost, and
  // the next calculation of a formula reading it gave a value (doc_fuzz seed 10882828: Undo of a merge beside
  // =COUNT(A:A) in A5 turned a VLOOKUP over A1:C9 from #N/A into 0). A cycle the restore broke is pending, and the
  // next calculation drops it (BeginCycles).
  std::vector<uint64_t> keep;
  for (const uint64_t k : circular) {
    const Sheet* s = S(KeySheet(k));
    if (s && s->GetFormula(KeyRow(k), KeyCol(k))) keep.push_back(k);
  }
  AppendNew(circular_cells, keep);
}

void Document::DropOrphanSpills() {
  std::vector<uint64_t> gone;
  for (const auto& [a, rr] : spill_anchor_) {
    const Sheet* s = S(KeySheet(a));
    if (!s || !s->GetFormula(KeyRow(a), KeyCol(a))) gone.push_back(a);
  }
  for (uint64_t a : gone) ClearSpill(a, nullptr);
}

// ---- External results ----

void Document::SetUdfArrayResult(const std::string& key, std::shared_ptr<formula::Array> a) {
  udf_stale_.erase(key);
  udf_stale_arrays_.erase(key);
  udf_cache_.erase(key);
  udf_arrays_[key] = std::move(a);
  udf_requested_.erase(key);
  auto it = udf_cells_.find(key);
  if (it == udf_cells_.end()) return;
  for (uint64_t k : it->second) {
    const Sheet* s = S(KeySheet(k));
    if (s && s->GetFormula(KeyRow(k), KeyCol(k))) AddPending(k);
  }
}

void Document::SetUdfResult(const std::string& key, Value v) {
  bool same = false;  // the answer the cells already show (asked again after the host started): nothing to do
  if (auto st = udf_stale_.find(key); st != udf_stale_.end()) {
    same = st->second.SameAs(v);
    udf_stale_.erase(st);
  }
  if (udf_stale_arrays_.erase(key)) same = false;
  if (same) {
    udf_cache_[key] = std::move(v);
    udf_requested_.erase(key);
    return;
  }
  udf_arrays_.erase(key);
  udf_cache_[key] = std::move(v);
  udf_requested_.erase(key);
  auto it = udf_cells_.find(key);
  if (it == udf_cells_.end()) return;
  for (uint64_t k : it->second) {
    const Sheet* s = S(KeySheet(k));
    if (s && s->GetFormula(KeyRow(k), KeyCol(k))) AddPending(k);
  }
}

void Document::UpdateTopic(const std::string& topic, Value v) {
  ++topic_updates_;
  auto it = topics_.find(topic);
  if (it == topics_.end()) return;
  it->second.value = std::move(v);
  it->second.has_value = true;
  for (uint64_t k : it->second.bound) SetValue(KeySheet(k), KeyRow(k), KeyCol(k), it->second.value, kSrcLive);
  for (auto c = it->second.cells.begin(); c != it->second.cells.end();) {
    const Sheet* s = S(KeySheet(*c));
    if (s && s->GetFormula(KeyRow(*c), KeyCol(*c))) {
      AddPending(*c);
      ++c;
    } else {
      c = it->second.cells.erase(c);
    }
  }
}

void Document::BindTopics(uint32_t sheet, const Rect& rect, const std::vector<std::string>& topics) {
  if (Refuse(sheet, &rect)) return;
  ClearContents(sheet, rect, kSrcPython);
  size_t i = 0;
  for (uint32_t r = rect.r0; r <= rect.r1; ++r) {
    for (uint32_t c = rect.c0; c <= rect.c1; ++c, ++i) {
      const uint64_t k = Key(sheet, r, c);
      if (auto old = bound_topic_.find(k); old != bound_topic_.end()) {
        if (auto t = topics_.find(old->second); t != topics_.end()) t->second.bound.erase(k);
        bound_topic_.erase(old);
      }
      if (i >= topics.size() || topics[i].empty()) continue;
      Topic& t = topics_[topics[i]];
      t.bound.insert(k);
      bound_topic_[k] = topics[i];
      if (t.has_value) SetValue(sheet, r, c, t.value, kSrcLive);
    }
  }
}

void Document::UnbindTopics(uint32_t sheet, const Rect& rect) {
  for (uint32_t r = rect.r0; r <= rect.r1; ++r) {
    for (uint32_t c = rect.c0; c <= rect.c1; ++c) {
      const uint64_t k = Key(sheet, r, c);
      auto old = bound_topic_.find(k);
      if (old == bound_topic_.end()) continue;
      if (auto t = topics_.find(old->second); t != topics_.end()) t->second.bound.erase(k);
      bound_topic_.erase(old);
    }
  }
}

std::vector<std::string> Document::BoundTopics(uint32_t sheet, const Rect& rect) const {
  std::vector<std::string> out;
  for (uint32_t r = rect.r0; r <= rect.r1; ++r) {
    for (uint32_t c = rect.c0; c <= rect.c1; ++c) {
      auto it = bound_topic_.find(Key(sheet, r, c));
      out.push_back(it == bound_topic_.end() ? std::string() : it->second);
    }
  }
  return out;
}

namespace {
bool CallsUdf(const formula::Node& n, const std::unordered_set<std::string>& names) {
  if (n.kind == formula::NK::Func && n.func_id < 0) {
    std::string u(n.str);
    for (char& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (names.count(u)) return true;
  }
  for (const auto& k : n.kids) {
    if (CallsUdf(k, names)) return true;
  }
  return false;
}
}  // namespace

void Document::RecalcUdfCells(const std::vector<std::string>* host_udfs) {
  std::unordered_set<std::string> names;
  if (host_udfs) {
    names.insert(host_udfs->begin(), host_udfs->end());
    names.insert("PY");
  }
  std::unordered_map<uint32_t, bool> calls;  // formula id -> calls one of the host's functions
  auto asks_host = [&](uint64_t k) {
    if (!host_udfs) return true;
    const Sheet* s = S(KeySheet(k));
    const uint32_t id = s ? s->GetFormula(KeyRow(k), KeyCol(k)) : 0;
    if (!id) return false;
    auto it = calls.find(id);
    if (it == calls.end()) {
      const formula::CompiledFormula* f = Formula(id);
      it = calls.emplace(id, f && CallsUdf(f->root, names)).first;
    }
    return it->second;
  };
  for (auto& [k, v] : udf_cache_) udf_stale_[k] = std::move(v);
  for (auto& [k, a] : udf_arrays_) udf_stale_arrays_[k] = std::move(a);
  udf_cache_.clear();
  udf_arrays_.clear();
  udf_requested_.clear();
  auto valid = [&](uint64_t k) {
    const Sheet* s = S(KeySheet(k));
    return s && s->GetFormula(KeyRow(k), KeyCol(k));
  };
  std::vector<uint64_t> mark;
  for (const auto& [key, cells] : udf_cells_) {
    const bool answered = udf_stale_.count(key) || udf_stale_arrays_.count(key);
    for (uint64_t k : cells) {
      if (!valid(k)) continue;
      mark.push_back(k);
      if (answered) break;  // one formula asks; the others keep the answer they show
    }
  }
  for (uint64_t k : udf_no_host_cells_) {  // formulas calculated while there was no host
    if (valid(k) && asks_host(k)) mark.push_back(k);
  }
  std::vector<uint64_t>().swap(udf_no_host_cells_);
  if (udf_without_host_) {  // too many to list: every #NAME? formula (and what follows them)
    udf_without_host_ = false;
    for (const auto& sc : wb_.sheets) {
      const Sheet& sh = sc.get();
      sh.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
        if (t.formula_count == 0) return;
        for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
          if (!t.formula[idx]) continue;
          const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
          const Value& v = sh.GetValue(r, c);
          if (v.IsError() && v.error() == core::Err::Name && asks_host(Key(sh.id, r, c))) mark.push_back(Key(sh.id, r, c));
        }
      });
    }
  }
  for (uint64_t k : mark) AddPending(k);
  Recalc();
  // a call whose formula didn't ask (it was edited since): ask with every formula of the call; a call no formula
  // makes any more is forgotten
  mark.clear();
  for (auto it = udf_cells_.begin(); it != udf_cells_.end();) {
    const std::string& key = it->first;
    if (udf_requested_.count(key) || udf_cache_.count(key) || udf_arrays_.count(key) ||
        !(udf_stale_.count(key) || udf_stale_arrays_.count(key))) {
      ++it;
      continue;
    }
    size_t n = 0;
    for (uint64_t k : it->second) {
      if (valid(k)) mark.push_back(k), ++n;
    }
    if (n == 0) {
      udf_stale_.erase(key);
      udf_stale_arrays_.erase(key);
      it = udf_cells_.erase(it);
    } else {
      ++it;
    }
  }
  if (!mark.empty()) {
    for (uint64_t k : mark) AddPending(k);
    Recalc();
  }
}

void Document::ProviderChanged() {
  for (auto& [name, t] : topics_) {
    t.subscribed = false;
    if (!provider) continue;  // subscribed when the host is there; the cells keep their values meanwhile
    for (uint64_t k : t.cells) {
      const Sheet* s = S(KeySheet(k));
      if (s && s->GetFormula(KeyRow(k), KeyCol(k))) AddPending(k);
    }
  }
  Recalc();
}

void Document::InvalidateUdfCache() {
  udf_cache_.clear();
  udf_arrays_.clear();
  udf_stale_.clear();
  udf_stale_arrays_.clear();
  udf_requested_.clear();
  udf_cells_.clear();
}

// ---- Transactions and undo ----

std::shared_ptr<const std::unordered_map<uint64_t, Rect>> Document::SharedBlocked() {
  if (!blocked_shared_ || !(*blocked_shared_ == spill_blocked_)) blocked_shared_ = std::make_shared<const std::unordered_map<uint64_t, Rect>>(spill_blocked_);
  return blocked_shared_;
}

std::shared_ptr<const std::vector<uint64_t>> Document::SharedCircular() {
  if (!circular_shared_ || !(*circular_shared_ == circular_cells)) circular_shared_ = std::make_shared<const std::vector<uint64_t>>(circular_cells);
  return circular_shared_;
}

void Document::Begin(std::string label) {
  if (txn_depth_++ == 0) {
    pending_undo_ = UndoEntry{};
    pending_undo_.label = std::move(label);
    pending_undo_.wb = wb_;  // COW copy
    pending_undo_.settled = Settled();
    pending_undo_.blocked = SharedBlocked();
    pending_undo_.circular = SharedCircular();
  }
}

void Document::Commit() {
  if (txn_depth_ == 0) return;
  if (txn_depth_ == 1) {
    // a table header the step wrote by any path (a fill of blanks, a paste, a cut) holds unique text again, as Excel's
    // does, inside the step so its Undo takes it back: the writes that bypass MarkChanged left a header empty until
    // Redo synced it (doc_fuzz seed 10290940)
    std::vector<uint32_t> sheets;
    for (const CellChange& rg : pending_undo_.regions) {
      if (std::find(sheets.begin(), sheets.end(), rg.sheet) != sheets.end()) continue;
      const Sheet* ts = S(rg.sheet);
      if (!ts || ts->tables->empty()) continue;
      for (const auto& t : *ts->tables) {
        if (t.header && rg.rect.Intersects(Rect{t.range.r0, t.range.c0, t.range.r0, t.range.c1})) {
          sheets.push_back(rg.sheet);
          break;
        }
      }
    }
    for (const uint32_t sid : sheets) SyncTableHeaders(sid);
    // Set precision as displayed: what the step wrote or formatted keeps only the digits its format shows
    if (wb_.precision_as_displayed) RoundShownIn(pending_undo_.regions);
    // a grown header that no sync reached takes its format now: none waits into another step
    for (const auto& g : grown_header_styles_) {
      if (Sheet* ms = M(g[0])) ms->ShowStyle(g[1], g[2], g[3]);
    }
    grown_header_styles_.clear();
  }
  if (--txn_depth_ > 0) return;
  if (pending_undo_.regions.empty() && pending_undo_.dims.empty() && !pending_undo_.structural && !pending_undo_.touched) {
    pending_undo_ = UndoEntry{};
    return;
  }
  MarkWholeIfOtherStateChanged(pending_undo_);
  undo_.push_back(std::move(pending_undo_));
  ++steps_made_;
  pending_undo_ = UndoEntry{};
  if (undo_.size() > undo_limit) {
    std::vector<UndoEntry> oldest;
    oldest.push_back(std::move(undo_.front()));
    undo_.erase(undo_.begin());
    DropUndo(std::move(oldest));
  }
  DropUndo(std::move(redo_));
}

void Document::DropUndo(std::vector<UndoEntry>&& entries) {
  if (entries.empty()) return;
  // typing after undoing a paste of six million cells waited 0.8 s here while the redo list's tiles were freed
  auto held = std::make_shared<std::vector<UndoEntry>>(std::move(entries));
  entries.clear();
  if (release_later) release_later(std::move(held));
}

void Document::NoteRegion(uint32_t sheet, const Rect& rect) {
  if (txn_depth_ == 0) return;
  if (pending_undo_.regions.size() < 4096) pending_undo_.regions.push_back(CellChange{sheet, rect, kSrcUser});
  else pending_undo_.structural = true;  // too many scattered regions: treat it as a structural change and rebuild everything on undo
}

void Document::NoteFormat(uint32_t sheet, const Rect& rect) {
  if (txn_depth_ == 0) return;
  if (pending_undo_.regions.size() < 4096) pending_undo_.regions.push_back(CellChange{sheet, rect, static_cast<uint8_t>(kSrcUser | kUndoFormatOnly)});
  else pending_undo_.structural = true;
}

void Document::NoteDims(uint32_t sheet, bool rows, uint32_t a, uint32_t b) {
  if (txn_depth_ == 0) return;
  if (pending_undo_.dims.size() < 4096) pending_undo_.dims.push_back(DimSpan{sheet, rows, std::min(a, b), std::max(a, b)});
  else pending_undo_.structural = true;
}

void Document::ForeignWrite() {
  // an entry undone with its whole version would also undo what Python wrote since: it goes, with every older one
  // (undoing an older entry across it would skip a change). Entries undone by their cells stay.
  for (size_t i = undo_.size(); i-- > 0;) {
    if (!undo_[i].Selective()) {
      std::vector<UndoEntry> gone(std::make_move_iterator(undo_.begin()), std::make_move_iterator(undo_.begin() + static_cast<std::ptrdiff_t>(i) + 1));
      undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
      DropUndo(std::move(gone));
      break;
    }
  }
  if (std::any_of(redo_.begin(), redo_.end(), [](const UndoEntry& e) { return !e.Selective(); })) DropUndo(std::move(redo_));
}

void Document::MarkWholeIfOtherStateChanged(UndoEntry& e) const {
  if (!e.Selective()) return;
  const model::Workbook& a = e.wb;  // before the change
  const model::Workbook& b = wb_;   // after it
  auto same = [](const auto& x, const auto& y) { return x.operator->() == y.operator->(); };
  // a part copied on write may still hold the same contents: the entry is then undone the old way, with the whole
  // version, which is always correct
  bool other = a.sheets.size() != b.sheets.size() || !same(a.names, b.names) || !same(a.panels, b.panels) ||
               !same(a.containers, b.containers) || !same(a.theme, b.theme) || !same(a.cell_styles, b.cell_styles) ||
               !same(a.external_links, b.external_links) || !same(a.bound_tables, b.bound_tables) ||
               !same(a.parameters, b.parameters) || !same(a.safety, b.safety) || !same(a.alerts, b.alerts) ||
               a.protect_structure != b.protect_structure || a.protect_windows != b.protect_windows;
  for (size_t i = 0; !other && i < a.sheets.size(); ++i) {
    const Sheet& x = a.sheets[i].get();
    const Sheet& y = b.sheets[i].get();
    other = x.id != y.id || x.name != y.name || x.visible != y.visible || x.tab_color != y.tab_color || x.row_limit != y.row_limit ||
            !same(x.controls, y.controls) || !same(x.charts, y.charts) || !same(x.cond_formats, y.cond_formats) ||
            !same(x.validations, y.validations) || !same(x.autofilter, y.autofilter) || !same(x.protection, y.protection) ||
            !same(x.tables, y.tables) || !same(x.shapes, y.shapes) || !same(x.sparklines, y.sparklines) ||
            !same(x.page_setup, y.page_setup) || !same(x.pivots, y.pivots) || !same(x.slicers, y.slicers) ||
            !same(x.data_tables, y.data_tables) || !same(x.scenarios, y.scenarios) || !same(x.solver, y.solver) ||
            !same(x.spills, y.spills) || !same(x.ignored_errors, y.ignored_errors);
  }
  if (other || WritesOverSpill(e)) e.touched = true;
}

bool Document::WritesOverSpill(const UndoEntry& e) {
  // the members of a spilled array are plain values in a version: put back by their cells they would block the array
  for (const CellChange& rg : e.regions) {
    if (rg.source & kUndoFormatOnly) continue;
    const model::Sheet* x = e.wb.SheetById(rg.sheet);
    if (!x) continue;
    for (const auto& [k, sp] : *x->spills) {
      if (sp.rect.Intersects(rg.rect)) return true;
    }
  }
  return false;
}

void Document::CopyRegionFrom(const model::Workbook& from, uint32_t sheet, const Rect& rect_in, bool formats_only) {
  const int32_t fi = from.IndexOfId(sheet);
  Sheet* s = M(sheet);
  if (fi < 0 || !s) return;
  const Sheet& f = from.sheets[static_cast<size_t>(fi)].get();
  Rect rect = rect_in;
  rect.c1 = std::min<uint32_t>(rect.c1, core::kMaxCols - 1);
  // every tile either version has inside the rectangle (the cost follows the data, not the rectangle)
  std::set<std::pair<uint32_t, uint32_t>> tiles;
  auto add = [&](uint32_t rt, uint32_t ct, const auto&) { tiles.emplace(rt, ct); };
  if (!formats_only) {
    ForEachTileInRect(f.values, rect, add);
    ForEachTileInRect(s->values, rect, add);
  }
  ForEachTileInRect(f.meta, rect, add);
  ForEachTileInRect(s->meta, rect, add);
  // values a dynamic array spilled aren't typed values: copied as such they blocked the array (Redo after Ctrl+D
  // through a spill, Undo of a column width over it: #SPILL!). They are left empty, and the array (its anchor inside the
  // rectangle or not) spills again.
  std::vector<std::pair<uint64_t, Rect>> spilled_in;
  if (!formats_only) {
    for (const auto& [k, sp] : *f.spills) {
      const uint32_t ar = static_cast<uint32_t>(k >> 16), ac = static_cast<uint32_t>(k & 0xFFFF);
      (void)ar, (void)ac;
      if (!sp.cse && sp.rect.Intersects(rect)) spilled_in.emplace_back(k, sp.rect);
    }
  }
  auto spilled_value = [&](uint32_t r, uint32_t c) {
    for (const auto& [k, sr] : spilled_in) {
      if (sr.Contains(r, c) && !(static_cast<uint32_t>(k >> 16) == r && static_cast<uint32_t>(k & 0xFFFF) == c)) return true;
    }
    return false;
  };
  for (const auto& [rt, ct] : tiles) {
    const uint32_t br = rt * model::kTileRows, bc = ct * model::kTileCols;
    const uint32_t r0 = std::max(rect.r0, br), r1 = std::min(rect.r1, br + model::kTileRows - 1);
    const uint32_t c0 = std::max(rect.c0, bc), c1 = std::min(rect.c1, bc + model::kTileCols - 1);
    for (uint32_t c = c0; c <= c1; ++c) {
      for (uint32_t r = r0; r <= r1; ++r) {
        if (!formats_only && !spilled_in.empty() && spilled_value(r, c)) {
          s->SetValue(r, c, Value());
          s->SetFormula(r, c, 0);
        } else if (!formats_only) {
          s->SetValue(r, c, f.GetValue(r, c));
          s->SetFormula(r, c, f.GetFormula(r, c));
        }
        s->SetStyle(r, c, f.GetOwnStyle(r, c));
      }
    }
  }
  // per-cell maps: fonts inside texts, notes, hyperlinks, arrays spilled from an anchor in the rectangle
  auto in = [&](uint64_t k) { return rect.Contains(static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF)); };
  auto copy_map = [&](auto& dst, const auto& src) {
    bool any = false;
    for (const auto& kv : *dst) any = any || in(kv.first);
    for (const auto& kv : *src) any = any || in(kv.first);
    if (!any) return;
    auto& m = dst.mut();
    for (auto it = m.begin(); it != m.end();) it = in(it->first) ? m.erase(it) : std::next(it);
    for (const auto& kv : *src) {
      if (in(kv.first)) m.insert(kv);
    }
  };
  for (const auto& [k, sr] : spilled_in) AddPending(Key(sheet, static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF)));
  copy_map(s->rich, f.rich);
  if (!formats_only) {
    copy_map(s->notes, f.notes);
    copy_map(s->links, f.links);
    copy_map(s->spills, f.spills);
  }
  // merged areas touching the rectangle
  const bool merges = std::any_of(s->merges->begin(), s->merges->end(), [&](const Rect& m) { return m.Intersects(rect); }) ||
                      std::any_of(f.merges->begin(), f.merges->end(), [&](const Rect& m) { return m.Intersects(rect); });
  if (merges) {
    auto& m = s->merges.mut();
    m.erase(std::remove_if(m.begin(), m.end(), [&](const Rect& x) { return x.Intersects(rect); }), m.end());
    for (const Rect& x : *f.merges) {
      if (x.Intersects(rect)) m.push_back(x);
    }
  }
}

void Document::CopyDimsFrom(const model::Workbook& from, const DimSpan& d) {
  const int32_t fi = from.IndexOfId(d.sheet);
  Sheet* s = M(d.sheet);
  if (fi < 0 || !s) return;
  const Sheet& f = from.sheets[static_cast<size_t>(fi)].get();
  auto copy_styles = [&](core::Cow<std::map<uint32_t, uint32_t>>& dst, const std::map<uint32_t, uint32_t>& src) {
    auto& st = dst.mut();
    for (auto it = st.lower_bound(d.a); it != st.end() && it->first <= d.b;) it = st.erase(it);
    for (auto it = src.lower_bound(d.a); it != src.end() && it->first <= d.b; ++it) st.insert(*it);
  };
  if (d.rows) {
    s->rows.mut().CopyRange(*f.rows, d.a, d.b);
    copy_styles(s->row_styles, *f.row_styles);
  } else {
    s->cols.mut().CopyRange(*f.cols, d.a, d.b);
    copy_styles(s->col_styles, *f.col_styles);
  }
}

void Document::RestoreSelective(const UndoEntry& e) {
  const model::Workbook leaving = wb_;  // copy on write: cheap
  // arrays spilled from an anchor in the restored cells go first: copying the cells takes their spill records, and the
  // values spilled outside the cells stayed behind as typed values (=SEQUENCE(1,3) typed, then undone, left 2 and 3)
  // and so do the arrays outside them that spill over them: cleared after the copy, they took the values it put back
  // with them (Undo of a fill that had freed a #SPILL! anchor emptied the cell that had blocked it: doc_fuzz seed
  // 10573473). Each is calculated again and spills, or meets what is back in its way.
  std::vector<uint64_t> anchors, outside;
  for (const auto& rg : e.regions) {
    if (rg.source & kUndoFormatOnly) continue;
    for (const auto& [a, rr] : spill_anchor_) {
      if (KeySheet(a) != rg.sheet) continue;
      if (rg.rect.Contains(KeyRow(a), KeyCol(a))) anchors.push_back(a);
      else if (rr.Intersects(rg.rect)) outside.push_back(a);
    }
  }
  for (uint64_t a : anchors) ClearSpill(a, nullptr);
  for (uint64_t a : outside) {
    const Sheet* os = S(KeySheet(a));
    const model::SpillRange* sp = os ? os->SpillOf(KeyRow(a), KeyCol(a)) : nullptr;
    if (!sp || sp->cse) continue;  // an array entered with Ctrl+Shift+Enter keeps its cells
    ClearSpill(a, nullptr);
    AddPending(a);
  }
  for (const auto& rg : e.regions) CopyRegionFrom(e.wb, rg.sheet, rg.rect, (rg.source & kUndoFormatOnly) != 0);
  for (const auto& d : e.dims) CopyDimsFrom(e.wb, d);
  // the spill hook stays on: an array the restored cells now block (or free) spills again like after typing
  for (const auto& rg : e.regions) {
    if (rg.source & kUndoFormatOnly) {
      changes_.push_back(CellChange{rg.sheet, rg.rect, kSrcUser});  // redraw only
      continue;
    }
    RebuildDepsInRegion(rg.sheet, rg.rect, leaving);
    MarkChanged(rg.sheet, rg.rect, kSrcUser);
  }
  for (const auto& d : e.dims) {
    const Sheet* s = S(d.sheet);
    if (!s) continue;
    // hidden rows change SUBTOTAL: what depends on them is recalculated (no cell was written: spills stay)
    MarkRowsColsChanged(d.sheet, d.rows ? Rect{d.a, 0, d.b, core::kMaxCols - 1} : Rect{0, d.a, s->row_limit - 1, d.b});
  }
  // rows a spill grew the sheet by (past row 1,048,576) go again with the formula: nothing is left in them
  for (const auto& rg : e.regions) {
    const int32_t oi = e.wb.IndexOfId(rg.sheet);
    Sheet* s = M(rg.sheet);
    if (oi < 0 || !s) continue;
    const uint32_t was = e.wb.sheets[static_cast<size_t>(oi)]->row_limit;
    if (s->row_limit <= was) continue;
    bool beyond = false;
    s->values.ForEach([&](uint32_t rt, uint32_t, const model::ValueTile& t) {
      for (uint32_t i = 0; i < model::kTileCells && !beyond; ++i) beyond = !t.v[i].IsEmpty() && rt * model::kTileRows + i % model::kTileRows >= was;
    });
    s->meta.ForEach([&](uint32_t rt, uint32_t, const model::MetaTile& t) {
      for (uint32_t i = 0; i < model::kTileCells && !beyond; ++i) beyond = (t.formula[i] || t.style[i]) && rt * model::kTileRows + i % model::kTileRows >= was;
    });
    if (beyond) continue;
    s->row_limit = was;
    if (s->has_used && s->used.r1 >= was) s->used.r1 = was - 1;
  }
  wb_.dirty = true;
}

void Document::RecordStruct(StructOp op) {
  const int32_t i = wb_.FindSheet(op.sheet);
  op.sheet_id = i >= 0 ? wb_.sheets[static_cast<size_t>(i)]->id : 0;
  if (txn_depth_ > 0) pending_undo_.ops.push_back(op);
  if (struct_log) {
    op.changes_at = changes_.size();
    struct_log->push_back(std::move(op));
  }
}

StructOp InverseOf(const StructOp& op) {
  StructOp x = op;
  switch (op.kind) {
    case StructOp::InsertRows: x.kind = StructOp::DeleteRows; break;
    case StructOp::DeleteRows: x.kind = StructOp::InsertRows; break;
    case StructOp::InsertCols: x.kind = StructOp::DeleteCols; break;
    case StructOp::DeleteCols: x.kind = StructOp::InsertCols; break;
    case StructOp::InsertCells: x.kind = StructOp::DeleteCells; break;
    case StructOp::DeleteCells: x.kind = StructOp::InsertCells; break;
    case StructOp::AddSheet: x.kind = StructOp::DeleteSheet; break;
    case StructOp::DeleteSheet: x.kind = StructOp::AddSheet; break;
    case StructOp::RenameSheet:
      x.sheet = op.name;
      x.name = op.sheet;
      break;
    case StructOp::MoveSheet:
      x.at = static_cast<uint32_t>(op.index);
      x.index = static_cast<int32_t>(op.at);
      break;
    case StructOp::CopySheet:
      x.kind = StructOp::DeleteSheet;
      x.sheet = op.name;
      x.name.clear();
      break;
  }
  return x;
}

void Document::NoteStructural() {
  if (txn_depth_ > 0) pending_undo_.structural = true;
}

void Document::ClearUndo() {
  DropUndo(std::move(undo_));
  DropUndo(std::move(redo_));
}

void Document::RestoreFrom(model::Workbook&& target, const std::vector<CellChange>& regions, bool structural, bool settled) {
  // grow-only tables and data outside undo: keep the current ones, but what in them is the workbook's look goes back
  // with the version (a theme's fonts set the default font; Effects is kept in the store): Undo of Page Layout >
  // Themes left the new default font and effects
  const std::string font = target.styles->default_font;
  const float font_size = target.styles->default_font_size;
  const auto fx = target.store->find("view.theme_effects");
  const std::optional<std::string> effects = fx == target.store->end() ? std::nullopt : std::optional<std::string>(fx->second);
  target.formulas = wb_.formulas;
  target.styles = wb_.styles;
  if (target.styles->default_font != font || target.styles->default_font_size != font_size) {
    target.styles.mut().default_font = font;
    target.styles.mut().default_font_size = font_size;
  }
  target.store = wb_.store;
  const auto now = target.store->find("view.theme_effects");
  if (effects && (now == target.store->end() || now->second != *effects)) target.store.mut()["view.theme_effects"] = *effects;
  if (!effects && now != target.store->end()) target.store.mut().erase("view.theme_effects");
  target.doc_props = wb_.doc_props;
  target.path = wb_.path;
  target.layout_json = wb_.layout_json;
  target.dirty = true;
  model::Workbook leaving = std::move(wb_);
  wb_ = std::move(target);
  formula::SetDate1904(wb_.date1904);  // Undo of the 1904 date system brings the other back
  Reindex();
  // a circular reference is only found by calculating its cells: with one in the workbook, everything is calculated
  // again and the cycle is reported where the restored version has it
  if (!circular_cells.empty()) settled = false;
  if (structural) {
    if (!settled) {
      RebuildAll();
    } else {
      // The version comes back with the values calculated for it: only what changes by itself is calculated again,
      // and only the sheets that differ from the version being left register their formulas again (a sheet nobody
      // changed is the same object in both). Rebuilding and recalculating everything took seconds at every undo.
      std::unordered_map<uint32_t, const Sheet*> before;
      for (const auto& sc : leaving.sheets) before[sc->id] = &sc.get();
      std::vector<uint32_t> redo_ids;
      for (const auto& sc : wb_.sheets) {
        auto it = before.find(sc->id);
        if (it == before.end() || it->second != &sc.get()) redo_ids.push_back(sc->id);
        if (it != before.end()) before.erase(it);
      }
      for (const auto& [gone, sh] : before) redo_ids.push_back(gone);  // sheets the version doesn't have
      auto formulas_of = [](const Sheet& sh) {
        size_t n = 0;
        sh.meta.ForEach([&](uint32_t, uint32_t, const model::MetaTile& t) { n += t.formula_count; });
        return n;
      };
      size_t total = 0, changed = 0;
      for (const auto& sc : wb_.sheets) total += formulas_of(sc.get());
      for (uint32_t sid : redo_ids) {
        if (const Sheet* sh = S(sid)) changed += formulas_of(*sh);
        const int32_t li = leaving.IndexOfId(sid);  // what leaves with the version: taken out one by one too
        if (li >= 0) changed += formulas_of(leaving.sheets[static_cast<size_t>(li)].get());
      }
      if (changed >= total) {  // most of the formulas moved: one rebuild is cheaper than taking them out one by one
        RebuildAll(false);
        ClearPending();
        MarkAllVolatileDirty();
        for (const auto& sc : wb_.sheets) {
          if (sc->has_used) changes_.push_back(CellChange{sc->id, sc->used, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
        }
        return;
      }
      const bool hook = spill_hook_;
      spill_hook_ = false;  // the version's spilled values come back as they were
      const Rect all{0, 0, core::kMaxRows - 1, core::kMaxCols - 1};
      for (uint32_t sid : redo_ids) RebuildDepsInRegion(sid, all, leaving);
      spill_hook_ = hook;
      RebuildSpillIndex();
      ClearPending();
      MarkAllVolatileDirty();
    }
    for (const auto& sc : wb_.sheets) {
      if (sc->has_used) changes_.push_back(CellChange{sc->id, sc->used, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
    }
    return;
  }
  spill_hook_ = false;
  for (const auto& rg : regions) {
    RebuildDepsInRegion(rg.sheet, rg.rect, leaving);
    MarkChanged(rg.sheet, rg.rect, kSrcUser);
  }
  spill_hook_ = true;
}

void Document::RebuildDepsInRegion(uint32_t sheet, const Rect& rect, const model::Workbook& leaving) {
  const int32_t li = leaving.IndexOfId(sheet);
  if (li >= 0) {
    std::vector<std::pair<uint32_t, uint32_t>> cells;
    ForEachCellInTiles(leaving.sheets[static_cast<size_t>(li)]->meta, rect, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
      if (t.formula[idx]) cells.emplace_back(r, c);
    });
    for (auto [r, c] : cells) UnregisterFormulaCell(sheet, r, c);
  }
  const Sheet* s = S(sheet);
  if (!s) return;
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> cells;
  ForEachCellInTiles(s->meta, rect, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.formula[idx]) cells.emplace_back(r, c, t.formula[idx]);
  });
  for (auto [r, c, id] : cells) {
    RegisterFormulaCell(sheet, r, c, id);
    // put back, not entered: a formula a cycle keeps uncalculated shows the value it had, not a typed formula's 0
    // (doc_fuzz seed 580: Undo of a fill over a VLOOKUP reading its own cell showed 0 where it had shown #N/A)
    fresh_formulas_.erase(Key(sheet, r, c));
    restored_formulas_.insert(Key(sheet, r, c));
  }
}

bool Document::UndoTarget(bool redo, uint32_t* sheet, Rect* rect) const {
  const auto& stack = redo ? redo_ : undo_;
  if (stack.empty() || stack.back().regions.empty()) return false;
  const auto& rg = stack.back().regions;
  *sheet = rg.front().sheet;
  *rect = rg.front().rect;
  for (const CellChange& c : rg) {
    if (c.sheet == *sheet) *rect = rect->Union(c.rect);
  }
  return true;
}

bool Document::Undo() {
  if (undo_.empty()) return false;
  // read-only mode: Ctrl+Z puts back no earlier edit (it would change the workbook); the steps stay for later
  if (RefuseReadOnly()) return false;
  UndoEntry e = std::move(undo_.back());
  undo_.pop_back();
  UndoEntry redo;
  redo.label = e.label;
  redo.wb = wb_;
  redo.regions = e.regions;
  redo.dims = e.dims;
  redo.structural = e.structural;
  redo.touched = e.touched || WritesOverSpill(redo);
  redo.settled = Settled();
  redo.blocked = SharedBlocked();
  redo.circular = SharedCircular();
  redo.ops = e.ops;
  const uint8_t bits = extra_src_bits;
  if (struct_log && !e.ops.empty()) {
    for (auto it = e.ops.rbegin(); it != e.ops.rend(); ++it) {
      struct_log->push_back(InverseOf(*it));
      struct_log->back().changes_at = changes_.size();
    }
    extra_src_bits |= kSrcRestore;
  }
  pivot_caches_.clear();  // a report taken back may have been refreshed from other data
  if (e.Selective()) RestoreSelective(e);
  else RestoreFrom(std::move(e.wb), e.regions, e.structural, e.settled);
  RestoreBlockedSpills(e.blocked ? *e.blocked : std::unordered_map<uint64_t, Rect>{});
  RestoreCircular(e.circular ? *e.circular : std::vector<uint64_t>{});
  extra_src_bits = bits;
  redo_.push_back(std::move(redo));
  return true;
}

bool Document::Redo() {
  if (redo_.empty()) return false;
  if (RefuseReadOnly()) return false;
  UndoEntry e = std::move(redo_.back());
  redo_.pop_back();
  UndoEntry undo;
  undo.label = e.label;
  undo.wb = wb_;
  undo.regions = e.regions;
  undo.dims = e.dims;
  undo.structural = e.structural;
  undo.touched = e.touched || WritesOverSpill(undo);
  undo.settled = Settled();
  undo.blocked = SharedBlocked();
  undo.circular = SharedCircular();
  undo.ops = e.ops;
  const uint8_t bits = extra_src_bits;
  if (struct_log && !e.ops.empty()) {
    for (const auto& op : e.ops) {
      struct_log->push_back(op);
      struct_log->back().changes_at = changes_.size();
    }
    extra_src_bits |= kSrcRestore;
  }
  pivot_caches_.clear();  // a report taken back may have been refreshed from other data
  if (e.Selective()) RestoreSelective(e);
  else RestoreFrom(std::move(e.wb), e.regions, e.structural, e.settled);
  RestoreBlockedSpills(e.blocked ? *e.blocked : std::unordered_map<uint64_t, Rect>{});
  RestoreCircular(e.circular ? *e.circular : std::vector<uint64_t>{});
  extra_src_bits = bits;
  undo_.push_back(std::move(undo));
  return true;
}

}  // namespace yc::engine
