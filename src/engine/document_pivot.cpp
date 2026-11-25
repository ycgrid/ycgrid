// SPDX-License-Identifier: Apache-2.0
// document_pivot.cpp - PivotTables: create, change, refresh (write the report into the sheet), delete, GETPIVOTDATA
#include "core/i18n.h"
#include "engine/document.h"
#include "formula/datetime.h"
#include "engine/pivot_engine.h"

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace yc::engine {

using model::Sheet;

namespace {
// an empty list of slicers (a sheet that is gone): a reference, so a loop over it never copies
const std::vector<model::Slicer>& NoSlicers() {
  static const std::vector<model::Slicer> none;
  return none;
}
bool ApplySlicerSelection(Document& d, const model::Slicer& slc, std::vector<std::string> hidden, std::string* err);

std::string LowerStr(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// a dark fill takes white text
bool Dark(uint32_t argb) {
  const double r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
  return 0.299 * r + 0.587 * g + 0.114 * b < 140;
}

}  // namespace

std::string Document::NextPivotName() const {
  for (int n = 1;; ++n) {
    const std::string name = "PivotTable" + std::to_string(n);
    bool used = false;
    for (const auto& sc : wb_.sheets) {
      for (const auto& p : *sc->pivots) used = used || LowerStr(p.name) == LowerStr(name);
    }
    if (!used) return name;
  }
}

uint32_t Document::AddPivot(uint32_t sheet, model::PivotTable pt, std::string* err) {
  if (Refuse(sheet, nullptr, Allow::Pivot)) return 0;
  Sheet* s = M(sheet);
  if (!s) return 0;
  // the source must be readable before anything is created
  PivotCache cache;
  std::string e;
  if (!ReadPivotSource(wb_, pt, &cache, &e)) {
    if (err) *err = e;
    return 0;
  }
  NoteTouched();
  pt.id = wb_.next_object_id++;
  if (pt.name.empty()) pt.name = NextPivotName();
  if (pt.fields.empty()) {
    pt.fields = cache.names;
    pt.fields_src = static_cast<int64_t>(cache.source_fields);
  }
  pt.has_output = false;
  pt.written.clear();
  const uint32_t id = pt.id;
  s->pivots.mut().push_back(std::move(pt));
  wb_.dirty = true;
  if (!RefreshPivot(sheet, id, err) && pivot_blocked_) {
    // where it would cover a table or data it may not replace: not created
    Sheet* s2 = M(sheet);
    auto& v = s2->pivots.mut();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const model::PivotTable& p) { return p.id == id; }), v.end());
    return 0;
  }
  return id;
}

const model::PivotTable* Document::PivotById(uint32_t sheet, uint32_t id) const {
  const Sheet* s = S(sheet);
  if (!s) return nullptr;
  for (const auto& p : *s->pivots) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

Rect PivotArea(const model::PivotTable& p) {
  if (p.has_output) return p.output;
  return Rect{p.r0, p.c0, p.r0 + 17, p.c0 + 2};
}

const model::PivotTable* Document::PivotAt(uint32_t sheet, uint32_t r, uint32_t c) const {
  const Sheet* s = S(sheet);
  if (!s) return nullptr;
  for (const auto& p : *s->pivots) {
    if (PivotArea(p).Contains(r, c)) return &p;
  }
  return nullptr;
}

const model::PivotTable* Document::FindPivot(std::string_view name, uint32_t* sheet) const {
  for (const auto& sc : wb_.sheets) {
    for (const auto& p : *sc->pivots) {
      if (LowerStr(p.name) == LowerStr(std::string(name))) {
        if (sheet) *sheet = sc->id;
        return &p;
      }
    }
  }
  return nullptr;
}

bool Document::UpdatePivot(uint32_t sheet, uint32_t id, const std::function<void(model::PivotTable&)>& fn, std::string* err) {
  if (Refuse(sheet, nullptr, Allow::Pivot)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  for (auto& p : s->pivots.mut()) {
    if (p.id != id) continue;
    NoteTouched();
    const model::PivotTable before = p;
    const std::string old_name = p.name;
    fn(p);
    p.id = id;
    // filters added or removed: the report's body stays where it is and the filter area (a row per filter and a blank
    // one) goes above it, the body moving down only as far as the rows above it fall short (Excel, measured: from A3
    // a filter takes A1:B1 and the body stays at A3; from A1 the body moves to A3 and stays there when the filter goes)
    if (p.r0 == before.r0 && p.c0 == before.c0) {
      auto page_rows = [](const model::PivotTable& x) { return x.pages.empty() ? 0u : static_cast<uint32_t>(x.pages.size()) + 1; };
      const uint32_t was = page_rows(before), now = page_rows(p);
      if (was != now) p.r0 = std::max(before.r0 + was, now) - now;
    }
    // a manual filter set again: the items there are now are the ones it knows (RefreshPivot fills them in)
    for (auto* v : {&p.rows, &p.cols, &p.pages}) {
      for (auto& f : *v) {
        const model::PivotAxisField* was = nullptr;
        for (const auto* bv : {&before.rows, &before.cols, &before.pages}) {
          for (const auto& b : *bv) {
            if (b.field == f.field) was = &b;
          }
        }
        if (!was || was->hidden != f.hidden) f.known.clear();
      }
    }
    if (p.name.empty()) p.name = old_name;
    const std::string new_name = p.name;
    wb_.dirty = true;
    if (!RefreshPivot(sheet, id, err)) {
      if (pivot_blocked_) {
        // the report would cover a table, another report or data it may not replace: it stays as it was
        for (auto& q : M(sheet)->pivots.mut()) {
          if (q.id == id) q = before;
        }
      }
      return false;
    }
    if (new_name != old_name) {
      // PivotCharts and slicers follow the new name
      for (auto& sc : wb_.sheets) {
        bool any = false;
        for (const auto& ch : *sc->charts) any = any || ch.pivot == old_name;
        for (const auto& sl : *sc->slicers) {
          any = any || sl.pivot == old_name || std::find(sl.more_pivots.begin(), sl.more_pivots.end(), old_name) != sl.more_pivots.end();
        }
        if (!any) continue;
        for (auto& ch : sc.mut().charts.mut()) {
          if (ch.pivot == old_name) ch.pivot = new_name;
        }
        for (auto& sl : sc.mut().slicers.mut()) {
          if (sl.pivot == old_name) sl.pivot = new_name;
          for (auto& mp : sl.more_pivots) {
            if (mp == old_name) mp = new_name;
          }
        }
      }
      UpdatePivotCharts();
    }
    return true;
  }
  return false;
}

bool Document::RefreshPivot(uint32_t sheet, uint32_t id, std::string* err) {
  // read-only mode: the report stays as it is (its cells would be refused half-way, the PivotTable already changed)
  if (RefuseReadOnly(sheet)) return false;
  pivot_blocked_ = false;
  pivot_overwrite_hit = false;
  Sheet* s = M(sheet);
  if (!s) return false;
  const model::PivotTable* cur = nullptr;
  for (const auto& p : *s->pivots) {
    if (p.id == id) cur = &p;
  }
  if (!cur) return false;
  PivotCache cache;
  std::string e;
  if (!ReadPivotSource(wb_, *cur, &cache, &e)) {
    if (err) *err = e;
    return false;
  }
  // fields follow their names (the source's columns moved, a group made fields): read again with the fields where
  // they are now (groups and calculated items name their field by position)
  model::PivotTable next = *cur;
  std::vector<int> map = RemapPivotFields(&next, cache);
  if (!map.empty()) {
    if (!ReadPivotSource(wb_, next, &cache, &e)) {
      if (err) *err = e;
      return false;
    }
    const std::vector<int> again = RemapPivotFields(&next, cache);
    if (!again.empty()) {
      for (int& m : map) m = m >= 0 && static_cast<size_t>(m) < again.size() ? again[static_cast<size_t>(m)] : -1;
    }
  }
  next.fields = cache.names;
  next.fields_src = static_cast<int64_t>(cache.source_fields);
  // fields made by a group go next to its field, once ("Years" before "Date", "Region2" before "Region")
  for (size_t gi = 0; gi < next.groups.size(); ++gi) {
    auto& g = next.groups[gi];
    if (g.placed) continue;
    std::vector<int> made;
    for (size_t f = 0; f < cache.made_by.size(); ++f) {
      if (cache.made_by[f] == static_cast<int>(gi)) made.push_back(static_cast<int>(f));
    }
    if (made.empty()) continue;
    g.placed = true;
    for (auto* axis : {&next.rows, &next.cols, &next.pages}) {
      const auto at = std::find_if(axis->begin(), axis->end(), [&](const model::PivotAxisField& a) { return a.field == g.field; });
      if (at == axis->end()) continue;
      std::vector<model::PivotAxisField> add;
      for (int f : made) {
        bool used = false;
        for (const auto* v : {&next.rows, &next.cols, &next.pages}) {
          for (const auto& a : *v) used = used || a.field == f;
        }
        if (used) continue;
        model::PivotAxisField af;
        af.field = f;
        add.push_back(af);
      }
      axis->insert(at, add.begin(), add.end());
      break;
    }
  }
  // fields that moved away from the source are dropped from the layout
  auto valid = [&](int f) { return f >= 0 && static_cast<size_t>(f) < cache.names.size(); };
  auto valid_axis = [&](int f) { return f >= 0 && static_cast<size_t>(f) < cache.source_fields; };  // calculated fields only go to Values
  for (auto* v : {&next.rows, &next.cols, &next.pages}) {
    v->erase(std::remove_if(v->begin(), v->end(), [&](const model::PivotAxisField& f) { return !valid_axis(f.field); }), v->end());
  }
  next.data.erase(std::remove_if(next.data.begin(), next.data.end(), [&](const model::PivotDataField& f) { return !valid(f.field); }), next.data.end());
  // a manual filter remembers the items there were when it was set (new ones stay out unless "Include new items in
  // manual filter" is on)
  for (auto* v : {&next.rows, &next.cols, &next.pages}) {
    for (auto& f : *v) {
      if (f.hidden.empty()) f.known.clear();
      else if (f.known.empty() && f.field >= 0 && static_cast<size_t>(f.field) < cache.items.size()) f.known = cache.items[static_cast<size_t>(f.field)];
    }
  }
  const PivotReport rep = ComputePivot(next, cache);
  const Rect area{next.r0, next.c0, next.r0 + std::max<uint32_t>(rep.rows, 1) - 1, next.c0 + std::max<uint32_t>(rep.cols, 1) - 1};
  const Rect old = cur->output;
  const bool had = cur->has_output;
  const std::vector<uint32_t> old_breaks = cur->breaks;
  if (rep.rows > 0) {
    // it may not cover another PivotTable or a table; data only when the caller allows it
    for (const auto& o : *s->pivots) {
      if (o.id != id && PivotArea(o).Intersects(area)) {
        if (err) *err = TR("A PivotTable report cannot overlap another PivotTable report.");
        pivot_blocked_ = true;
        return false;
      }
    }
    for (const auto& t : *s->tables) {
      if (t.range.Intersects(area)) {
        if (err) *err = TR("A PivotTable report cannot overlap a table.");
        pivot_blocked_ = true;
        return false;
      }
    }
    if (pivot_overwrite == 1) {
      bool filled = false;
      for (uint32_t r = area.r0; r <= area.r1 && !filled; ++r) {
        for (uint32_t c = area.c0; c <= area.c1 && !filled; ++c) {
          if (had && old.Contains(r, c)) continue;
          filled = !s->GetValue(r, c).IsEmpty();
        }
      }
      if (filled) {
        std::string book = wb_.path;
        const size_t slash = book.find_last_of("/\\");
        if (slash != std::string::npos) book = book.substr(slash + 1);
        const size_t dot = book.rfind('.');
        if (dot != std::string::npos && dot > 0) book = book.substr(0, dot);
        if (book.empty()) book = "Book1";
        if (err) *err = TR("There's already data in [") + book + "]" + s->name + TR(". Do you want to replace it?");
        pivot_blocked_ = true;
        pivot_overwrite_hit = true;
        return false;
      }
    }
  }
  NoteTouched();
  // formats the user gave the report's cells: a cell whose style isn't the one the last refresh left (kept by the
  // items of its row and column: Preserve cell formatting on update)
  std::map<std::pair<std::string, std::string>, uint32_t> user;
  for (const auto& f : cur->formats) user[{f.row, f.col}] = f.style;
  if (had && next.preserve_formatting && cur->written.size() == static_cast<size_t>(old.Count()) && cur->written_rows.size() == old.Rows() &&
      cur->written_cols.size() == old.Cols()) {
    size_t i = 0;
    for (uint32_t r = 0; r < old.Rows(); ++r) {
      for (uint32_t c = 0; c < old.Cols(); ++c, ++i) {
        const uint32_t now = s->GetStyle(old.r0 + r, old.c0 + c);
        if (now != cur->written[i]) user[{cur->written_rows[r], cur->written_cols[c]}] = now;
      }
    }
  }
  if (!next.preserve_formatting) user.clear();
  // the PivotTable as refreshed: slicers follow their fields' new places
  if (!map.empty()) {
    const std::string pname = next.name;
    for (auto& sc : wb_.sheets) {
      bool any = false;
      for (const auto& sl : *sc->slicers) any = any || sl.pivot == pname;
      if (!any) continue;
      for (auto& sl : sc.mut().slicers.mut()) {
        if (sl.pivot == pname && sl.field >= 0) sl.field = static_cast<size_t>(sl.field) < map.size() ? map[static_cast<size_t>(sl.field)] : -1;
      }
    }
    s = M(sheet);
  }
  // Insert page break after each item: the report's breaks in the sheet's page setup (the ones it put there before go)
  {
    std::vector<uint32_t> nb;
    for (uint32_t b : rep.breaks) nb.push_back(next.r0 + b);
    if (nb != old_breaks) {
      auto& ps = s->page_setup.mut();
      auto& rb = ps.row_breaks;
      rb.erase(std::remove_if(rb.begin(), rb.end(), [&](uint32_t b) { return std::find(old_breaks.begin(), old_breaks.end(), b) != old_breaks.end(); }), rb.end());
      std::vector<uint32_t> mine;
      for (uint32_t b : nb) {
        if (std::find(rb.begin(), rb.end(), b) != rb.end()) continue;  // a break the user put there stays the user's
        rb.push_back(b);
        mine.push_back(b);
      }
      std::sort(rb.begin(), rb.end());
      nb = std::move(mine);
    }
    next.breaks = std::move(nb);
  }
  next.output = area;
  next.has_output = rep.rows > 0;
  next.refreshed_at = formula::NowSerial();  // PivotTable.RefreshDate
  // ... and RefreshName: Application.UserName, else the Windows user's
  if (user_name.empty()) {
#if defined(_WIN32)
    char* u = nullptr;
    size_t n = 0;
    if (_dupenv_s(&u, &n, "USERNAME") == 0 && u) next.refreshed_by = u;
    std::free(u);
#else
    if (const char* u = std::getenv("USER")) next.refreshed_by = u;
#endif
  } else {
    next.refreshed_by = user_name;
  }
  next.written.clear();
  next.written_rows.clear();
  next.written_cols.clear();
  for (auto& p : s->pivots.mut()) {
    if (p.id == id) p = next;
  }
  pivot_caches_[{sheet, id}] = std::make_shared<const PivotCache>(std::move(cache));
  const uint32_t r0 = next.r0, c0 = next.c0;
  const model::PivotTable& opt = next;
  struct Writing {
    bool& flag;
    explicit Writing(bool& f) : flag(f) { flag = true; }
    ~Writing() { flag = false; }
  } writing(pivot_writing_);
  if (had) ClearAll(sheet, old, kSrcCalc, false, /*to_lines=*/true);
  if (rep.rows == 0) {
    UpdatePivotCharts();
    return true;
  }
  ClearAll(sheet, area, kSrcCalc, false, /*to_lines=*/true);
  // values
  std::vector<Value> values(static_cast<size_t>(rep.rows) * rep.cols);
  for (uint32_t r = 0; r < rep.rows; ++r) {
    for (uint32_t c = 0; c < rep.cols; ++c) values[static_cast<size_t>(r) * rep.cols + c] = rep.at(r, c).v;
  }
  SetValues(sheet, area, values, kSrcCalc);
  // look: the PivotTable style (Design > PivotTable Styles) with its Style Options
  auto styled = [&](const Rect& rr, const std::function<void(model::Style&)>& fn) { ModifyStyle(sheet, rr, fn); };
  const PivotStyleLook look = PivotStyleFor(opt.style);
  const uint32_t body0 = rep.page_rows + rep.header_rows;
  if (!look.none) {
    if (rep.header_rows > 0 && opt.style_col_headers) {
      const Rect band{r0 + rep.page_rows, c0, r0 + rep.page_rows + rep.header_rows - 1, c0 + rep.cols - 1};
      const uint32_t fill = look.header_fill, font = look.header_font;
      if (fill || font) {
        styled(band, [fill, font](model::Style& st) {
          if (fill) st.fill_color = fill;
          if (font) st.font_color = font;
        });
      }
      if (look.header_line) {
        const uint32_t line = look.header_line;
        styled(Rect{band.r1, band.c0, band.r1, band.c1}, [line](model::Style& st) { st.border[model::kBottom] = {model::LineStyle::Thin, line}; });
      }
    }
    // banded rows / columns: every other item row / value column of the body
    if (opt.style_banded_rows && look.band_fill) {
      int k = 0;
      for (uint32_t r = body0; r < rep.rows; ++r) {
        if (rep.row_kind[r] != 0 && rep.row_kind[r] != 4) continue;
        if (k++ % 2 == 1) {
          const uint32_t fill = look.band_fill;
          styled(Rect{r0 + r, c0, r0 + r, c0 + rep.cols - 1}, [fill](model::Style& st) { st.fill_color = fill; });
        }
      }
    }
    if (opt.style_banded_cols && look.band_fill && rep.rows > body0) {
      int k = 0;
      for (uint32_t c = rep.label_cols; c < rep.cols; ++c) {
        if (rep.col_kind[c] != 0) continue;
        if (k++ % 2 == 1) {
          const uint32_t fill = look.band_fill;
          styled(Rect{r0 + body0, c0 + c, r0 + rep.rows - 1, c0 + c}, [fill](model::Style& st) { st.fill_color = fill; });
        }
      }
    }
    for (uint32_t r = body0; r < rep.rows; ++r) {
      const int kind = rep.row_kind[r];
      if (kind == 1 && look.subtotal_fill) {
        const uint32_t fill = look.subtotal_fill;
        styled(Rect{r0 + r, c0, r0 + r, c0 + rep.cols - 1}, [fill](model::Style& st) { st.fill_color = fill; });
      }
      if (look.body_line && kind != 2) {
        const uint32_t line = look.body_line;
        styled(Rect{r0 + r, c0, r0 + r, c0 + rep.cols - 1}, [line](model::Style& st) { st.border[model::kBottom] = {model::LineStyle::Thin, line}; });
      }
    }
  }
  for (uint32_t r = 0; r < rep.rows; ++r) {
    const bool grand = rep.cols > 0 && rep.at(r, 0).grand_row;
    if (grand) {
      const uint32_t fill = look.none ? 0 : look.grand_fill, line = look.none ? 0 : look.grand_line;
      const uint32_t font = fill && Dark(fill) ? 0xFFFFFFFF : 0;
      styled(Rect{r0 + r, c0, r0 + r, c0 + rep.cols - 1}, [fill, line, font](model::Style& st) {
        if (fill) st.fill_color = fill;
        if (font) st.font_color = font;
        st.bold = true;
        if (line) st.border[model::kTop] = {model::LineStyle::Thin, line};
      });
    }
    for (uint32_t c = 0; c < rep.cols; ++c) {
      const PivotCell& cell = rep.at(r, c);
      if (!cell.bold && cell.indent == 0 && cell.numfmt.empty()) continue;
      const uint32_t fmt = cell.numfmt.empty() ? 0 : wb_.styles.mut().InternString(cell.numfmt);
      const bool bold = cell.bold;
      const uint8_t indent = cell.indent;
      styled(Rect::Cell(r0 + r, c0 + c), [bold, indent, fmt](model::Style& st) {
        if (bold) st.bold = true;
        if (indent) st.indent = indent;
        if (fmt) st.numfmt = fmt;
      });
    }
  }
  // the user's formats where their items are now
  if (!user.empty()) {
    for (uint32_t r = 0; r < rep.rows; ++r) {
      for (uint32_t c = 0; c < rep.cols; ++c) {
        const auto it = user.find({rep.row_ids[r], rep.col_ids[c]});
        if (it == user.end()) continue;
        const model::Style st = wb_.styles->Get(it->second);
        styled(Rect::Cell(r0 + r, c0 + c), [&st](model::Style& x) { x = st; });
      }
    }
  }
  // Autofit column widths on update: each column as wide as its text (not narrower than the sheet's default)
  if (Sheet* ws = M(sheet); ws && opt.autofit) {
    for (uint32_t c = 0; c < rep.cols; ++c) {
      size_t chars = 0;
      for (uint32_t r = 0; r < rep.rows; ++r) {
        const PivotCell& cell = rep.at(r, c);
        if (cell.kind == PivotCell::Empty || cell.kind == PivotCell::PageName) continue;
        size_t n = DisplayTextOf(cell.v, cell.numfmt.empty() ? std::string_view("General") : std::string_view(cell.numfmt)).size() + cell.indent * 2;
        if (cell.bold) n += n / 8 + 1;                   // bold text is wider
        if (cell.kind == PivotCell::Header) n += 3;      // "Row Labels" / "Column Labels" carry a filter button
        chars = std::max(chars, n);
      }
      for (const auto& t : rep.toggles) {
        if (t.c == c) chars += 2;  // the +/- button
      }
      const int32_t want = std::max<int32_t>(static_cast<int32_t>(std::min<size_t>(chars, 60) * 7 + 14), ws->cols->default_size());
      if (want != ws->cols->Size(c0 + c)) SetColWidth(sheet, c0 + c, c0 + c, want);
    }
  }
  // what this refresh left, to tell the user's formats at the next one
  if (Sheet* ws = M(sheet)) {
    std::vector<uint32_t> written;
    written.reserve(static_cast<size_t>(area.Count()));
    for (uint32_t r = area.r0; r <= area.r1; ++r) {
      for (uint32_t c = area.c0; c <= area.c1; ++c) written.push_back(ws->GetStyle(r, c));
    }
    std::vector<model::PivotFormat> keep;
    for (const auto& [k, style] : user) {
      if (keep.size() >= 4000) break;
      keep.push_back(model::PivotFormat{k.first, k.second, style});
    }
    for (auto& p : ws->pivots.mut()) {
      if (p.id != id) continue;
      p.written = std::move(written);
      p.written_rows = rep.row_ids;
      p.written_cols = rep.col_ids;
      p.formats = std::move(keep);
    }
  }
  UpdatePivotCharts();
  return true;
}

uint32_t Document::PivotShowDetails(uint32_t sheet, uint32_t r, uint32_t c, std::string* err) {
  if (RefuseReadOnly(sheet)) return 0;  // read-only mode: no details sheet
  const model::PivotTable* pt = PivotAt(sheet, r, c);
  if (!pt || !pt->has_output) return 0;
  PivotCache cache;
  std::string e;
  if (!ReadPivotSource(wb_, *pt, &cache, &e)) {
    if (err) *err = e;
    return 0;
  }
  const PivotReport rep = ComputePivot(*pt, cache);
  const uint32_t rr = r - pt->r0, cc = c - pt->c0;
  if (rr >= rep.rows || cc >= rep.cols || rep.at(rr, cc).kind != PivotCell::Data) return 0;
  if (!pt->enable_drilldown) {
    if (err) *err = TR("Show Details is turned off for this PivotTable (PivotTable Options > Data > Enable show details).");
    return 0;
  }
  auto filter = rep.row_filter[rr];
  filter.insert(filter.end(), rep.col_filter[cc].begin(), rep.col_filter[cc].end());
  const std::vector<size_t> recs = PivotDetailRecords(*pt, cache, filter);
  const int32_t idx = wb_.IndexOfId(sheet);
  const uint32_t ns = AddSheet({}, idx < 0 ? -1 : idx);
  if (!ns) return 0;
  const uint32_t nf = static_cast<uint32_t>(cache.columns);  // the source's columns (not calculated or grouped fields)
  std::vector<Value> values;
  values.reserve((recs.size() + 1) * nf);
  for (uint32_t f = 0; f < nf; ++f) values.push_back(Value::String(cache.names[f]));
  for (size_t rec : recs) {
    for (uint32_t f = 0; f < nf; ++f) values.push_back(cache.records[rec][f]);
  }
  const Rect area{0, 0, static_cast<uint32_t>(recs.size()), nf - 1};
  SetValues(ns, area, values, kSrcCalc);
  // the source's number formats
  for (uint32_t f = 0; f < nf; ++f) {
    if (cache.numfmts[f].empty() || recs.empty()) continue;
    const uint32_t fmt = wb_.styles.mut().InternString(cache.numfmts[f]);
    ModifyStyle(ns, Rect{1, f, static_cast<uint32_t>(recs.size()), f}, [fmt](model::Style& st) { st.numfmt = fmt; });
  }
  if (!recs.empty()) AddTable(ns, area, true, "TableStyleMedium2", nullptr);
  return ns;
}

void Document::UpdatePivotCharts() {
  for (auto& sc : wb_.sheets) {
    bool any = false;
    for (const auto& ch : *sc->charts) any = any || !ch.pivot.empty();
    if (!any) continue;
    for (auto& ch : sc.mut().charts.mut()) {
      if (ch.pivot.empty()) continue;
      const model::PivotTable* pt = FindPivot(ch.pivot);
      if (pt) PivotChartSeries(wb_, *pt, &ch);  // keeps what it showed when the source can't be read
    }
  }
}

bool PivotChartSeries(const model::Workbook& wb, const model::PivotTable& pt_ref, model::ChartObj* chp) {
  model::ChartObj& ch = *chp;
  const model::PivotTable* pt = &pt_ref;
  {
    {
      PivotCache cache;
      std::string e;
      if (!ReadPivotSource(wb, *pt, &cache, &e)) return false;
      const PivotReport rep = ComputePivot(*pt, cache);
      // categories: the item rows (a collapsed item is one; outer / inner labels joined); series: the item columns
      std::vector<uint32_t> rows;
      std::vector<std::string> cats;
      std::vector<std::vector<std::string>> paths;  // the same labels by level, outermost first (a multi-level axis)
      for (uint32_t r = 0; r < rep.rows; ++r) {
        const int k = rep.row_kind[r];
        if (k != 0 && k != 4 && k != 6) continue;
        if (k == 6 && rep.row_data[r] < 0 && pt->data.size() > 1 && pt->values_on_rows) continue;  // the caption rows below carry the values
        rows.push_back(r);
        std::string label;
        std::vector<std::string> path;
        for (const auto& [f, item] : rep.row_filter[r]) {
          path.push_back(cache.display[static_cast<size_t>(f)][static_cast<size_t>(item)]);
          label += (label.empty() ? "" : " - ") + path.back();
        }
        if (rep.row_kind[r] == 4 && rep.label_cols > 0) {
          path.push_back(DisplayTextOf(rep.at(r, 0).v, "General"));
          label += (label.empty() ? "" : " - ") + path.back();
        }
        cats.push_back(label.empty() ? std::string(TR("Total")) : label);
        paths.push_back(path.empty() ? std::vector<std::string>{cats.back()} : path);
      }
      std::vector<model::ChartSeries> series;
      size_t si = 0;
      for (uint32_t c = rep.label_cols; c < rep.cols; ++c) {
        if (rep.col_kind[c] != 0 && rep.col_kind[c] != 6) continue;
        model::ChartSeries se;
        std::string name;
        for (const auto& [f, item] : rep.col_filter[c]) name += (name.empty() ? "" : " - ") + cache.display[static_cast<size_t>(f)][static_cast<size_t>(item)];
        const int d = rep.col_data[c];
        if (d >= 0 && (pt->data.size() > 1 || name.empty())) name += (name.empty() ? "" : " - ") + PivotDataCaption(*pt, cache, static_cast<size_t>(d));
        se.name = name.empty() ? std::string(TR("Total")) : name;
        // keep each series' formats across refreshes
        if (si < ch.series.size()) {
          se.color = ch.series[si].color;
        }
        auto data = std::make_shared<model::SeriesData>();
        for (size_t i = 0; i < rows.size(); ++i) {
          const Value& v = rep.at(rows[i], c).v;
          data->x.push_back(static_cast<double>(i + 1));
          data->y.push_back(v.IsNumeric() ? v.AsDouble() : 0.0);
        }
        data->cats = cats;
        if (std::any_of(paths.begin(), paths.end(), [](const auto& p) { return p.size() >= 2; })) data->cat_paths = paths;
        data->version = (si < ch.series.size() && ch.series[si].data ? ch.series[si].data->version : 0) + 1;
        se.data = std::move(data);
        se.python_data = true;
        series.push_back(std::move(se));
        ++si;
      }
      ch.series = std::move(series);
      if (!ch.has_title || ch.title.empty()) {
        ch.title = pt->data.size() == 1 ? PivotDataCaption(*pt, cache, 0) : pt->name;
        ch.has_title = true;
      }
    }
  }
  return true;
}

uint32_t Document::AddSlicer(uint32_t sheet, model::Slicer sl) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return 0;
  Sheet* s = M(sheet);
  if (!s) return 0;
  NoteTouched();
  sl.id = wb_.next_object_id++;
  if (sl.name.empty()) sl.name = sl.caption.empty() ? "Slicer" : sl.caption;
  // names are unique on the sheet ("Region", "Region 1"...)
  const std::string base = sl.name;
  for (int k = 1; std::any_of(s->slicers->begin(), s->slicers->end(), [&](const model::Slicer& o) { return LowerStr(o.name) == LowerStr(sl.name); }); ++k)
    sl.name = base + " " + std::to_string(k);
  int z = 0;
  for (const auto& x : *s->controls) z = std::max(z, x.z + 1);
  for (const auto& x : *s->charts) z = std::max(z, x.z + 1);
  for (const auto& x : *s->shapes) z = std::max(z, x.z + 1);
  for (const auto& x : *s->slicers) z = std::max(z, x.z + 1);
  sl.z = z;
  const uint32_t id = sl.id;
  s->slicers.mut().push_back(std::move(sl));
  wb_.dirty = true;
  return id;
}

bool Document::UpdateSlicer(uint32_t sheet, uint32_t id, const std::function<void(model::Slicer&)>& fn) {
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  for (auto& x : s->slicers.mut()) {
    if (x.id != id) continue;
    NoteTouched();
    fn(x);
    x.id = id;
    x.columns = std::clamp(x.columns, 1, 20);
    wb_.dirty = true;
    return true;
  }
  return false;
}

bool Document::DeleteSlicer(uint32_t sheet, uint32_t id) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  auto& v = s->slicers.mut();
  const auto it = std::find_if(v.begin(), v.end(), [&](const model::Slicer& x) { return x.id == id; });
  if (it == v.end()) return false;
  NoteTouched();
  const std::string pivot = it->pivot;
  const int field = it->field;
  const bool timeline = it->timeline;
  const model::Slicer gone = *it;
  v.erase(it);
  if (!gone.table.empty()) {
    bool other = false;
    for (const auto& sc : wb_.sheets) {
      for (const auto& x : *sc->slicers) other = other || (x.table == gone.table && x.field == field);
    }
    if (!other) {
      ApplySlicerSelection(*this, gone, {}, nullptr);
    }
    wb_.dirty = true;
    return true;
  }
  // a slicer of a field off the report takes its filter with it (unless another slicer still uses that field); the
  // filter of a field on the report is the field's own and stays. Every PivotTable it was connected to.
  std::string fname;
  if (const model::PivotTable* first = FindPivot(pivot); first && field >= 0 && static_cast<size_t>(field) < first->fields.size())
    fname = first->fields[static_cast<size_t>(field)];
  std::vector<std::pair<std::string, int>> targets = {{pivot, field}};
  for (const auto& name : gone.more_pivots) {
    const model::PivotTable* q = FindPivot(name);
    for (size_t k = 0; q && k < q->fields.size(); ++k) {
      if (LowerStr(q->fields[k]) == LowerStr(fname)) targets.emplace_back(q->name, static_cast<int>(k));
    }
  }
  for (const auto& [tname, tfield] : targets) {
    bool other = false;
    for (const auto& sc : wb_.sheets) {
      for (const auto& x : *sc->slicers) {
        other = other || (LowerStr(x.pivot) == LowerStr(tname) && x.field == tfield);
        if (x.more_pivots.empty() || x.timeline != timeline) continue;
        const model::PivotTable* xp = FindPivot(x.pivot);
        const std::string xf = xp && x.field >= 0 && static_cast<size_t>(x.field) < xp->fields.size() ? xp->fields[static_cast<size_t>(x.field)] : std::string();
        for (const auto& mp : x.more_pivots) other = other || (LowerStr(mp) == LowerStr(tname) && LowerStr(xf) == LowerStr(fname));
      }
    }
    uint32_t ps = 0;
    const model::PivotTable* pt = FindPivot(tname, &ps);
    if (other || !pt) continue;
    const int fld = tfield;
    const bool has_slicer_filter = std::any_of(pt->slicer_filters.begin(), pt->slicer_filters.end(), [&](const model::PivotAxisField& f) { return f.field == fld; });
    const bool has_date = std::any_of(pt->date_filters.begin(), pt->date_filters.end(), [&](const model::PivotDateFilter& f) { return f.field == fld; });
    if (has_slicer_filter || (timeline && has_date)) {
      UpdatePivot(ps, pt->id, [&](model::PivotTable& p) {
        auto& sf = p.slicer_filters;
        sf.erase(std::remove_if(sf.begin(), sf.end(), [&](const model::PivotAxisField& f) { return f.field == fld; }), sf.end());
        if (timeline) {
          auto& df = p.date_filters;
          df.erase(std::remove_if(df.begin(), df.end(), [&](const model::PivotDateFilter& f) { return f.field == fld; }), df.end());
        }
      });
    }
  }
  wb_.dirty = true;
  return true;
}

bool Document::SetTimelineFilter(const std::string& pivot, int field, double from, double to, std::string* err) {
  uint32_t sheet = 0;
  const model::PivotTable* pt = FindPivot(pivot, &sheet);
  if (!pt) return false;
  return UpdatePivot(sheet, pt->id, [&](model::PivotTable& p) {
    auto& v = p.date_filters;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const model::PivotDateFilter& f) { return f.field == field; }), v.end());
    if (from <= to) v.push_back(model::PivotDateFilter{field, std::floor(from), std::floor(to)});
  }, err);
}

bool Document::SetSlicerFilter(const std::string& pivot, int field, std::vector<std::string> hidden, std::string* err) {
  uint32_t sheet = 0;
  const model::PivotTable* pt = FindPivot(pivot, &sheet);
  if (!pt) return false;
  return UpdatePivot(sheet, pt->id, [&](model::PivotTable& p) {
    auto& v = p.slicer_filters;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const model::PivotAxisField& f) { return f.field == field; }), v.end());
    // a field on the report has one filter: the slicer and the field's drop-down show and change the same items
    for (auto* axis : {&p.rows, &p.cols, &p.pages}) {
      for (auto& a : *axis) {
        if (a.field != field) continue;
        a.hidden = hidden;
        if (axis == &p.pages) {
          a.page_item.clear();
          if (!hidden.empty()) a.multi_page = true;
        }
        return;
      }
    }
    if (!hidden.empty()) {
      model::PivotAxisField f;
      f.field = field;
      f.hidden = std::move(hidden);
      v.push_back(std::move(f));
    }
  }, err);
}

bool TableSlicerState(const model::Workbook& wb, const model::Slicer& sl, uint32_t* sheet, std::vector<std::string>* items, std::vector<std::string>* hidden,
                      std::vector<char>* has_data) {
  items->clear();
  hidden->clear();
  has_data->clear();
  uint32_t sid = 0;
  const model::Table* t = sl.table.empty() ? nullptr : model::FindTable(wb, sl.table, &sid);
  const model::Sheet* s = t ? wb.SheetById(sid) : nullptr;
  if (!s) return false;
  const Rect fr = TableFilterRange(*t);
  if (sl.field < 0 || fr.c0 + static_cast<uint32_t>(sl.field) > fr.c1) return false;
  if (sheet) *sheet = sid;
  const uint32_t col = fr.c0 + static_cast<uint32_t>(sl.field);
  const bool sheet_af = TableOwnsSheetFilter(*s, *t);
  const model::AutoFilter& af = sheet_af ? *s->autofilter : t->filter;
  const Rect range = sheet_af ? af.range : fr;
  const int fidx = static_cast<int>(col) - static_cast<int>(range.c0);
  const FilterTester tester(wb, *s, af, range);
  const auto& st = *wb.styles;
  std::map<std::string, Value> uniq;
  std::set<std::string> with;
  for (uint32_t r = fr.r0 + (t->header ? 1 : 0); r <= fr.r1; ++r) {
    const Value& v = s->GetValue(r, col);
    const std::string text = v.IsEmpty() ? std::string("(blank)") : DisplayTextOf(v, st.NumberFormat(st.Get(s->GetStyle(r, col))));
    uniq.emplace(text, v);
    if (tester.Passes(r, fidx)) with.insert(text);
  }
  // numbers in order, then text A to Z, blanks last (as a slicer lists them)
  std::vector<std::pair<std::string, Value>> v(uniq.begin(), uniq.end());
  std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
    const int ra = a.second.IsEmpty() ? 2 : a.second.IsNumeric() ? 0 : 1, rb = b.second.IsEmpty() ? 2 : b.second.IsNumeric() ? 0 : 1;
    if (ra != rb) return ra < rb;
    if (ra == 0) return a.second.AsDouble() < b.second.AsDouble();
    return LowerStr(a.first) < LowerStr(b.first);
  });
  const model::FilterColumn* fc = nullptr;
  for (const auto& c : af.columns) {
    if (static_cast<int>(c.field) == fidx && c.kind == 1) fc = &c;
  }
  for (const auto& [text, val] : v) {
    items->push_back(text);
    has_data->push_back(with.count(text) ? 1 : 0);
    if (fc) {
      const bool shown = text == "(blank)" ? fc->blanks : std::find(fc->values.begin(), fc->values.end(), text) != fc->values.end();
      if (!shown) hidden->push_back(text);
    }
  }
  return true;
}

namespace {
// a slicer's selection: its table's column filter, or every PivotTable it is connected to
bool ApplySlicerSelection(Document& d, const model::Slicer& slc, std::vector<std::string> hidden, std::string* err) {
  const model::Workbook& wb_ = d.wb();
  if (!slc.table.empty()) {
    // a table's slicer: the column's AutoFilter (the sheet's when the table owns it, else the table's own)
    uint32_t tsheet = 0;
    const model::Table* t = model::FindTable(wb_, slc.table, &tsheet);
    const model::Sheet* ts = t ? d.S(tsheet) : nullptr;
    if (!ts) return false;
    const Rect fr = TableFilterRange(*t);
    if (slc.field < 0 || fr.c0 + static_cast<uint32_t>(slc.field) > fr.c1) return false;
    std::vector<std::string> items, h0;
    std::vector<char> data;
    TableSlicerState(wb_, slc, nullptr, &items, &h0, &data);
    const bool sheet_af = TableOwnsSheetFilter(*ts, *t);
    const uint32_t col = fr.c0 + static_cast<uint32_t>(slc.field);
    model::FilterColumn fc;
    fc.field = col - (sheet_af ? ts->autofilter->range.c0 : fr.c0);
    if (!hidden.empty()) {
      fc.kind = 1;
      fc.blanks = std::find(hidden.begin(), hidden.end(), "(blank)") == hidden.end();
      for (const auto& it : items) {
        if (it != "(blank)" && std::find(hidden.begin(), hidden.end(), it) == hidden.end()) fc.values.push_back(it);
      }
    }
    const uint32_t tid = t->id;
    if (sheet_af) d.SetFilterColumn(tsheet, fc);
    else d.SetTableFilterColumn(tsheet, tid, fc);
    return true;
  }
  // a PivotTable's slicer: every PivotTable it is connected to, by the field of the same name
  uint32_t ps = 0;
  const model::PivotTable* first = d.FindPivot(slc.pivot, &ps);
  if (!first || slc.field < 0 || static_cast<size_t>(slc.field) >= first->fields.size()) return d.SetSlicerFilter(slc.pivot, slc.field, hidden, err);
  const std::string fname = first->fields[static_cast<size_t>(slc.field)];
  bool ok = d.SetSlicerFilter(slc.pivot, slc.field, hidden, err);
  for (const auto& name : slc.more_pivots) {
    const model::PivotTable* pt = d.FindPivot(name);
    if (!pt) continue;
    for (size_t f = 0; f < pt->fields.size(); ++f) {
      if (LowerStr(pt->fields[f]) == LowerStr(fname)) {
        ok = d.SetSlicerFilter(pt->name, static_cast<int>(f), hidden, err) && ok;
        break;
      }
    }
  }
  return ok;
}
}  // namespace

bool Document::SetSlicerSelection(uint32_t sheet, uint32_t id, std::vector<std::string> hidden, std::string* err) {
  if (RefuseReadOnly(sheet)) return false;
  const model::Sheet* sh = S(sheet);
  for (const auto& x : sh ? *sh->slicers : NoSlicers()) {
    if (x.id == id) {
      const model::Slicer slc = x;
      return ApplySlicerSelection(*this, slc, std::move(hidden), err);
    }
  }
  return false;
}

bool Document::SetSlicerDates(uint32_t sheet, uint32_t id, double from, double to, std::string* err) {
  if (RefuseReadOnly(sheet)) return false;
  const model::Sheet* sh = S(sheet);
  const model::Slicer* sl = nullptr;
  for (const auto& x : sh ? *sh->slicers : NoSlicers()) {
    if (x.id == id) sl = &x;
  }
  if (!sl) return false;
  const model::Slicer slc = *sl;
  const model::PivotTable* first = FindPivot(slc.pivot);
  bool ok = SetTimelineFilter(slc.pivot, slc.field, from, to, err);
  if (!first || slc.field < 0 || static_cast<size_t>(slc.field) >= first->fields.size()) return ok;
  const std::string fname = first->fields[static_cast<size_t>(slc.field)];
  for (const auto& name : slc.more_pivots) {
    const model::PivotTable* pt = FindPivot(name);
    if (!pt) continue;
    for (size_t f = 0; f < pt->fields.size(); ++f) {
      if (LowerStr(pt->fields[f]) == LowerStr(fname)) {
        ok = SetTimelineFilter(pt->name, static_cast<int>(f), from, to, err) && ok;
        break;
      }
    }
  }
  return ok;
}

bool Document::ConnectSlicer(uint32_t sheet, uint32_t id, const std::vector<std::string>& pivots, std::string* err) {
  if (RefuseReadOnly(sheet)) return false;
  const model::Sheet* sh = S(sheet);
  const model::Slicer* sl = nullptr;
  for (const auto& x : sh ? *sh->slicers : NoSlicers()) {
    if (x.id == id) sl = &x;
  }
  if (!sl || !sl->table.empty()) return false;
  if (pivots.empty()) {
    if (err) *err = TR("A slicer must be connected to at least one PivotTable.");
    return false;
  }
  const model::Slicer slc = *sl;
  const model::PivotTable* first = FindPivot(slc.pivot);
  if (!first || slc.field < 0 || static_cast<size_t>(slc.field) >= first->fields.size()) return false;
  const std::string fname = first->fields[static_cast<size_t>(slc.field)];
  // the filter the slicer shows now, given to the PivotTables it joins; the ones it leaves keep no filter of it
  std::vector<std::string> hidden;
  if (const auto* af = [&]() -> const model::PivotAxisField* {
        for (const auto* v : {&first->rows, &first->cols, &first->pages}) {
          for (const auto& a : *v) {
            if (a.field == slc.field) return &a;
          }
        }
        for (const auto& a : first->slicer_filters) {
          if (a.field == slc.field) return &a;
        }
        return nullptr;
      }()) {
    hidden = af->hidden;
  }
  double tl_from = 1, tl_to = 0;
  for (const auto& f : first->date_filters) {
    if (f.field == slc.field) tl_from = f.from, tl_to = f.to;
  }
  std::vector<std::string> before = {slc.pivot};
  before.insert(before.end(), slc.more_pivots.begin(), slc.more_pivots.end());
  std::vector<std::pair<std::string, int>> joined;
  for (const auto& name : pivots) {
    const model::PivotTable* pt = FindPivot(name);
    if (!pt) continue;
    int f = -1;
    for (size_t k = 0; k < pt->fields.size(); ++k) {
      if (LowerStr(pt->fields[k]) == LowerStr(fname)) f = static_cast<int>(k);
    }
    if (f < 0) continue;
    joined.emplace_back(pt->name, f);
  }
  if (joined.empty()) {
    if (err) *err = TR("A slicer must be connected to at least one PivotTable.");
    return false;
  }
  // the PivotTables it leaves: their filter of this field (from the slicer) is cleared
  for (const auto& name : before) {
    if (std::any_of(joined.begin(), joined.end(), [&](const auto& j) { return LowerStr(j.first) == LowerStr(name); })) continue;
    const model::PivotTable* pt = FindPivot(name);
    if (!pt) continue;
    for (size_t k = 0; k < pt->fields.size(); ++k) {
      if (LowerStr(pt->fields[k]) != LowerStr(fname)) continue;
      if (slc.timeline) SetTimelineFilter(pt->name, static_cast<int>(k), 1, 0, nullptr);
      else SetSlicerFilter(pt->name, static_cast<int>(k), {}, nullptr);
    }
  }
  UpdateSlicer(sheet, id, [&](model::Slicer& x) {
    x.pivot = joined[0].first;
    x.field = joined[0].second;
    x.more_pivots.clear();
    for (size_t k = 1; k < joined.size(); ++k) x.more_pivots.push_back(joined[k].first);
  });
  for (const auto& [name, f] : joined) {
    if (slc.timeline) SetTimelineFilter(name, f, tl_from, tl_to, nullptr);
    else SetSlicerFilter(name, f, hidden, nullptr);
  }
  return true;
}

void Document::RefreshAllPivots() {
  if (RefuseReadOnly()) return;
  std::vector<std::pair<uint32_t, uint32_t>> all;
  for (const auto& sc : wb_.sheets) {
    for (const auto& p : *sc->pivots) all.emplace_back(sc->id, p.id);
  }
  for (const auto& [s, id] : all) RefreshPivot(s, id, nullptr);
}

bool Document::DeletePivot(uint32_t sheet, uint32_t id, bool clear, bool detach_charts) {
  if (Refuse(sheet, nullptr, Allow::Pivot)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  auto& v = s->pivots.mut();
  const auto it = std::find_if(v.begin(), v.end(), [&](const model::PivotTable& p) { return p.id == id; });
  if (it == v.end()) return false;
  NoteTouched();
  const Rect out = it->output;
  const bool had = it->has_output;
  const std::string name = it->name;
  const std::vector<uint32_t> breaks = it->breaks;
  v.erase(it);
  pivot_caches_.erase({sheet, id});
  if (!breaks.empty()) {
    auto& rb = s->page_setup.mut().row_breaks;
    rb.erase(std::remove_if(rb.begin(), rb.end(), [&](uint32_t b) { return std::find(breaks.begin(), breaks.end(), b) != breaks.end(); }), rb.end());
  }
  pivot_writing_ = true;
  if (clear && had) ClearAll(sheet, out, kSrcCalc, false, /*to_lines=*/true);
  pivot_writing_ = false;
  // its PivotCharts become static charts: they keep the series they show
  if (detach_charts) {
    for (auto& sc : wb_.sheets) {
      bool any = false;
      for (const auto& ch : *sc->charts) any = any || ch.pivot == name;
      if (!any) continue;
      for (auto& ch : sc.mut().charts.mut()) {
        if (ch.pivot == name) ch.pivot.clear();
      }
    }
  }
  wb_.dirty = true;
  return true;
}

bool Document::PivotLookupAt(uint32_t sheet, uint32_t r, uint32_t c, const std::string& data_field,
                             const std::vector<std::pair<std::string, std::string>>& pairs, Value* out) const {
  const model::PivotTable* pt = PivotAt(sheet, r, c);
  if (!pt) return false;
  // the report as it was last refreshed (Excel 16, measured: an edit of the source changes nothing until Refresh)
  if (const auto it = pivot_caches_.find({sheet, pt->id}); it != pivot_caches_.end()) return PivotLookup(*pt, *it->second, data_field, pairs, out);
  PivotCache cache;
  std::string e;
  if (!ReadPivotSource(wb_, *pt, &cache, &e)) return false;
  return PivotLookup(*pt, cache, data_field, pairs, out);
}

}  // namespace yc::engine
