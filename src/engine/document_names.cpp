// SPDX-License-Identifier: Apache-2.0
// document_names.cpp - defined names (named ranges): define, modify (renaming updates formulas), resolve to ranges, create from selection
#include "engine/document.h"
#include "core/i18n.h"
#include "engine/tile_iter.h"

#include <algorithm>
#include <functional>
#include <cctype>

namespace yc::engine {

using model::NameDef;
using model::Sheet;

bool ValidateName(std::string_view name, std::string* error) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  if (name.empty()) return fail(TR("The name can't be blank."));
  if (name.size() > 255) return fail(TR("Names can be up to 255 characters."));
  const unsigned char c0 = static_cast<unsigned char>(name[0]);
  if (!(std::isalpha(c0) || c0 == '_' || c0 == '\\' || c0 >= 0x80)) return fail(TR("A name must start with a letter, a CJK character, an underscore or a backslash."));
  core::CellPos p;
  if (core::ParseCellA1(name, &p)) return fail(TR("A name can't be the same as a cell reference (e.g. A1, XFD100)."));
  std::string up(name);
  for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  if (up == "R" || up == "C") return fail(TR("A name can't be R or C."));
  // and, as Excel refuses them: the logical values, Excel's future-function prefix, a backslash and one character
  if (up == "TRUE" || up == "FALSE" || up.rfind("_XLFN.", 0) == 0 || (up.size() == 2 && up[0] == '\\')) {
    return fail(TR("That name isn't valid."));
  }
  // R1C1 forms are not allowed either, nor what starts like one (R2, C3, R2C3, R2D2, C3PO: Excel's Names.Add refuses
  // them, measured), or formulas couldn't use the name
  if (formula::IsR1C1LikeName(name)) return fail(TR("A name can't be the same as an R1C1 reference."));
  for (char ch : name) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (u < 0x80 && !(std::isalnum(u) || ch == '_' || ch == '.' || ch == '\\' || ch == '?')) return fail(TR("Names can only contain letters, digits, CJK characters, underscores, periods and backslashes (no spaces or operators)."));
  }
  return true;
}

namespace {

std::string LowerStr(std::string_view s) {
  std::string out(s);
  for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

// a union written without parentheses (=Data!$A$1,Data!$A$3), which Excel takes as a name's Refers To: the formula
// language reads a union only inside parentheses, so the name keeps it as ((Data!$A$1,Data!$A$3)), every place that
// reads names reading it as the union, and Refers To shows it without the two pairs again (a name typed with one pair
// keeps that pair, as in Excel). Only references can be joined (=Data!$A$1,5 is refused, as in Excel)
bool UnionRef(const formula::Node& n) {
  using formula::NK;
  if (n.kind == NK::Binary && n.op == ',') return n.kids.size() == 2 && UnionRef(n.kids[0]) && UnionRef(n.kids[1]);
  if (n.kind == NK::Paren) return n.kids.size() == 1 && UnionRef(n.kids[0]);
  return n.kind == NK::Ref || n.kind == NK::Name || n.kind == NK::Func || n.kind == NK::StructRef ||
         (n.kind == NK::Binary && (n.op == ':' || n.op == ' '));
}

bool WrapTopUnion(std::string_view body, const formula::SheetResolver* sheets, std::string* out) {
  const std::string inner = "(" + std::string(body) + ")";
  formula::ParseResult pr = formula::Parse(inner, core::CellPos{0, 0}, sheets);
  if (!pr.ok || pr.root.kind != formula::NK::Paren || pr.root.kids.size() != 1) return false;
  const formula::Node& u = pr.root.kids[0];
  if (u.kind != formula::NK::Binary || u.op != ',' || !UnionRef(u)) return false;
  *out = "(" + formula::ToA1(pr.root, core::CellPos{0, 0}, sheets) + ")";  // written as Excel writes it ('T3'!$A$1)
  return true;
}

bool IsIdentChar(unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '.' || ch == '\\' || ch >= 0x80; }

// replace the name old with now where it appears as an identifier in formula text (strings, sheet names, function calls and Sheet!refs untouched)
std::string ReplaceNameToken(const std::string& f, const std::string& old, const std::string& now, bool* changed) {
  std::string out;
  out.reserve(f.size());
  const std::string lo = LowerStr(old);
  size_t i = 0;
  while (i < f.size()) {
    const char ch = f[i];
    if (ch == '"' || ch == '\'') {
      const char q = ch;
      size_t j = i + 1;
      while (j < f.size()) {
        if (f[j] == q) {
          if (j + 1 < f.size() && f[j + 1] == q) {
            j += 2;
            continue;
          }
          break;
        }
        ++j;
      }
      out.append(f, i, std::min(j + 1, f.size()) - i);
      i = j + 1;
      continue;
    }
    if (IsIdentChar(static_cast<unsigned char>(ch))) {
      size_t j = i;
      while (j < f.size() && IsIdentChar(static_cast<unsigned char>(f[j]))) ++j;
      const std::string tok = f.substr(i, j - i);
      const bool after_bang = i > 0 && f[i - 1] == '!';
      const bool sheet_prefix = j < f.size() && f[j] == '!';
      const bool call = j < f.size() && f[j] == '(';
      if (!after_bang && !sheet_prefix && !call && LowerStr(tok) == lo) {
        out += now;
        *changed = true;
      } else {
        out += tok;
      }
      i = j;
      continue;
    }
    out += ch;
    ++i;
  }
  return out;
}

}  // namespace

std::string ShownNameUnion(const std::string& refers_to, const formula::SheetResolver* sheets) {
  const size_t eq = !refers_to.empty() && refers_to[0] == '=' ? 1 : 0;
  std::string_view body = std::string_view(refers_to).substr(eq);
  if (body.size() < 5 || body.compare(0, 2, "((") != 0 || body.compare(body.size() - 2, 2, "))") != 0) return refers_to;
  formula::ParseResult pr = formula::Parse(body, core::CellPos{0, 0}, sheets);
  using formula::NK;
  if (!pr.ok || pr.root.kind != NK::Paren || pr.root.kids.size() != 1 || pr.root.kids[0].kind != NK::Paren ||
      pr.root.kids[0].kids.size() != 1)
    return refers_to;
  const formula::Node& u = pr.root.kids[0].kids[0];
  if (u.kind != NK::Binary || u.op != ',' || !UnionRef(u)) return refers_to;
  return refers_to.substr(0, eq) + std::string(body.substr(2, body.size() - 4));
}

bool Document::DefineName(model::NameDef def, std::string* error) {
  if (RefuseReadOnly()) return false;  // read-only mode: names stay as they are (the UI says why)
  if (!ValidateName(def.name, error)) return false;
  std::string_view body = def.refers_to;
  if (!body.empty() && body[0] == '=') body.remove_prefix(1);
  formula::ParseResult pr = formula::Parse(body, core::CellPos{0, 0}, this);
  std::string united;
  if (!pr.ok && WrapTopUnion(body, this, &united)) {
    def.refers_to = united;
  } else if (!pr.ok) {
    if (error) *error = TR("The formula in Refers To has an error: ") + pr.error;
    return false;
  } else {
    def.refers_to = std::string(body);
  }
  NoteStructural();
  auto& names = wb_.names.mut();
  bool found = false;
  for (auto& n : names) {
    if (n.sheet_scope == def.sheet_scope && LowerStr(n.name) == LowerStr(def.name)) {
      n = def;
      found = true;
    }
  }
  if (!found) names.push_back(std::move(def));
  MarkAllVolatileDirty();
  RecalcAll();
  wb_.dirty = true;
  return true;
}

bool Document::UpdateName(const std::string& old_name, int32_t old_scope, model::NameDef def, std::string* error) {
  if (RefuseReadOnly()) return false;
  auto& names = wb_.names.mut();
  auto it = std::find_if(names.begin(), names.end(), [&](const NameDef& n) { return n.sheet_scope == old_scope && LowerStr(n.name) == LowerStr(old_name); });
  if (it == names.end()) {
    if (error) *error = TR("Name not found: \"") + old_name + TR("\".");
    return false;
  }
  if (!ValidateName(def.name, error)) return false;
  std::string_view body = def.refers_to;
  if (!body.empty() && body[0] == '=') body.remove_prefix(1);
  formula::ParseResult pr = formula::Parse(body, core::CellPos{0, 0}, this);
  std::string united;
  if (!pr.ok && WrapTopUnion(body, this, &united)) {
    def.refers_to = united;
  } else if (!pr.ok) {
    if (error) *error = TR("The formula in Refers To has an error: ") + pr.error;
    return false;
  } else {
    def.refers_to = std::string(body);
  }
  const bool renamed = LowerStr(old_name) != LowerStr(def.name) || old_scope != def.sheet_scope;
  if (renamed) {
    for (const auto& n : names) {
      if (&n != &*it && n.sheet_scope == def.sheet_scope && LowerStr(n.name) == LowerStr(def.name)) {
        if (error) *error = TR("There's already a name \"") + def.name + TR("\".");
        return false;
      }
    }
  }
  NoteStructural();
  const std::string from = it->name;
  *it = def;
  // rename: update the formulas that use the name (like Excel). Workbook-level names skip sheets that have a sheet-level name of the same name
  if (LowerStr(from) != LowerStr(def.name)) {
    std::vector<uint32_t> sheets;
    for (const auto& sc : wb_.sheets) {
      const uint32_t sid = sc->id;
      if (old_scope >= 0 && static_cast<uint32_t>(old_scope) != sid) continue;
      if (old_scope < 0) {
        bool shadow = false;
        for (const auto& n : names) shadow = shadow || (n.sheet_scope == static_cast<int32_t>(sid) && LowerStr(n.name) == LowerStr(from));
        if (shadow) continue;
      }
      sheets.push_back(sid);
    }
    for (uint32_t sid : sheets) {
      const Sheet* s = S(sid);
      if (!s || !s->has_used) continue;
      std::vector<std::pair<core::CellPos, std::string>> edits;
      ForEachTileInRect(s->meta, s->used, [&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
        if (t.formula_count == 0) return;
        const uint32_t br = rt * model::kTileRows, bc = ct * model::kTileCols;
        for (uint32_t c = bc; c < bc + model::kTileCols; ++c) {
          for (uint32_t r = br; r < br + model::kTileRows; ++r) {
            if (!t.formula[model::TileIndex(r, c)]) continue;
            const std::string f = GetFormulaA1(sid, r, c);
            bool changed = false;
            std::string nf = ReplaceNameToken(f, from, def.name, &changed);
            if (changed) edits.push_back({core::CellPos{r, c}, nf.substr(1)});
          }
        }
      });
      for (const auto& [p, f] : edits) SetFormula(sid, p.row, p.col, f, nullptr, kSrcCalc | kSrcNoEvents);
    }
  }
  MarkAllVolatileDirty();
  RecalcAll();
  wb_.dirty = true;
  return true;
}

const model::NameDef* Document::FindName(std::string_view name, uint32_t current_sheet) const {
  const NameDef* wb_level = nullptr;
  const std::string lo = LowerStr(name);
  for (const auto& n : *wb_.names) {
    if (LowerStr(n.name) != lo) continue;
    if (n.sheet_scope == static_cast<int32_t>(current_sheet)) return &n;
    if (n.sheet_scope < 0) wb_level = &n;
  }
  return wb_level;
}

bool Document::ResolveNameRef(std::string_view name, uint32_t current_sheet, uint32_t* sheet, Rect* rect, std::string* error,
                              core::CellPos anchor) {
  // "Sheet!Name": the sheet-level name of that sheet
  std::string sname;
  std::string_view nm = name;
  uint32_t cur = current_sheet;
  std::string_view rest;
  if (core::SplitSheetRef(name, &sname, &rest) && !sname.empty()) {
    const int32_t id = FindSheet(sname);
    if (id < 0) {
      if (error) *error = TR("Sheet not found: \"") + sname + TR("\".");
      return false;
    }
    cur = static_cast<uint32_t>(id);
    nm = rest;
  }
  const NameDef* n = FindName(nm, cur);
  if (!n) {
    if (error) *error = TR("Name not found: \"") + std::string(nm) + TR("\".");
    return false;
  }
  const uint32_t base = n->sheet_scope >= 0 ? static_cast<uint32_t>(n->sheet_scope) : cur;
  return EvaluateRef(base, formula::MoveRelativeText(n->refers_to, core::CellPos{0, 0}, anchor, this), sheet, rect, error);
}

std::vector<model::NameDef> Document::NamesFromSelection(uint32_t sheet, const Rect& r, bool top, bool left, bool bottom, bool right) const {
  std::vector<model::NameDef> out;
  const Sheet* s = S(sheet);
  if (!s) return out;
  const std::string q = core::QuoteSheetName(s->name) + "!";
  auto clean = [](std::string t) {
    // invalid characters become underscores; a leading digit gets an underscore (like Excel)
    std::string o;
    for (char ch : t) {
      const unsigned char u = static_cast<unsigned char>(ch);
      o += (u >= 0x80 || std::isalnum(u) || ch == '_' || ch == '.') ? ch : '_';
    }
    if (!o.empty() && (std::isdigit(static_cast<unsigned char>(o[0])) || o[0] == '.')) o = "_" + o;
    // a label that reads as a cell or an R1C1 reference (Row1, C) gets an underscore after it (Excel 16, measured:
    // Row1_, C_)
    core::CellPos p;
    if (core::ParseCellA1(o, &p) || formula::IsR1C1LikeName(o)) o += "_";
    return o;
  };
  auto text = [&](uint32_t row, uint32_t col) {
    return DisplayTextOf(s->GetValue(row, col), wb_.styles->NumberFormat(wb_.styles->Get(s->GetStyle(row, col))));
  };
  auto a1 = [&](uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
    return q + core::CellA1(r0, c0, true, true) + (r0 == r1 && c0 == c1 ? "" : ":" + core::CellA1(r1, c1, true, true));
  };
  const uint32_t dr0 = r.r0 + (top ? 1 : 0), dr1 = r.r1 - (bottom ? 1 : 0), dc0 = r.c0 + (left ? 1 : 0), dc1 = r.c1 - (right ? 1 : 0);
  if (dr0 > dr1 || dc0 > dc1) return out;
  // a number is no label: Excel makes no name for it (measured: 12 beside a row gives none)
  auto add = [&](uint32_t row, uint32_t col, std::string ref) {
    if (s->GetValue(row, col).IsNumeric()) return;
    std::string label = clean(text(row, col));
    if (label.empty()) return;
    out.push_back(NameDef{label, ref, -1});
  };
  if (top) for (uint32_t c = dc0; c <= dc1; ++c) add(r.r0, c, a1(dr0, c, dr1, c));
  if (bottom) for (uint32_t c = dc0; c <= dc1; ++c) add(r.r1, c, a1(dr0, c, dr1, c));
  if (left) for (uint32_t row = dr0; row <= dr1; ++row) add(row, r.c0, a1(row, dc0, row, dc1));
  if (right) for (uint32_t row = dr0; row <= dr1; ++row) add(row, r.c1, a1(row, dc0, row, dc1));
  return out;
}

int Document::ApplyNames(uint32_t sheet, const Rect& area, const std::vector<std::string>& names, const ApplyNamesOptions& o) {
  if (RefuseReadOnly(sheet)) return 0;
  const bool ignore_relative = o.ignore_relative;
  const Sheet* s = S(sheet);
  if (!s) return 0;
  // each name that is a plain reference: where it points (sheet, cells) and whether its address is absolute
  struct Target {
    std::string name;
    uint32_t sheet;
    Rect rect;
  };
  std::vector<Target> targets;
  for (const std::string& n : names) {
    // only names that are one reference (=Sheet1!$A$1:$B$3), as Excel's Apply Names
    const model::NameDef* def = FindName(n, sheet);
    if (!def) continue;
    std::string_view text = def->refers_to;
    while (!text.empty() && (text.front() == '=' || text.front() == ' ')) text.remove_prefix(1);
    std::string sname;
    std::string_view addr = text;
    uint32_t target_sheet = def->sheet_scope >= 0 ? static_cast<uint32_t>(def->sheet_scope) : sheet;
    if (core::SplitSheetRef(text, &sname, &addr) && !sname.empty()) {
      const int32_t id = FindSheet(sname);
      if (id < 0) continue;
      target_sheet = static_cast<uint32_t>(id);
    }
    Rect r{};
    if (!core::ParseRectA1(addr, &r)) continue;
    targets.push_back({def->name, target_sheet, r});
  }
  if (targets.empty()) return 0;
  // the formulas of the area (the used part of it)
  Rect a = area;
  if (s->has_used) {
    a.r1 = std::min(a.r1, s->used.r1);
    a.c1 = std::min(a.c1, s->used.c1);
  }
  std::vector<std::pair<core::CellPos, std::string>> out;
  for (uint32_t r = a.r0; r <= a.r1 && r - a.r0 < 100000; ++r) {
    for (uint32_t c = a.c0; c <= a.c1 && c - a.c0 < 16384; ++c) {
      const formula::CompiledFormula* f = Formula(s->GetFormula(r, c));
      if (!f) continue;
      formula::Node root = f->root;
      bool changed = false;
      const core::CellPos origin{r, c};
      std::function<void(formula::Node&)> walk = [&](formula::Node& n) {
        if (n.kind == formula::NK::Ref && !n.ref.bad && !n.ref.spill && n.ref.sheet2 < 0) {
          formula::ResolvedRef rr;
          if (formula::ResolveRef(n.ref, origin, &rr)) {
            const uint32_t rs = rr.sheet < 0 ? sheet : static_cast<uint32_t>(rr.sheet);
            const bool absolute = n.ref.r0.abs && n.ref.c0.abs && (!n.ref.is_range || (n.ref.r1.abs && n.ref.c1.abs));
            for (const Target& t : targets) {
              if (t.sheet != rs || !(t.rect == rr.rect) || (!ignore_relative && !absolute)) continue;
              formula::Node name;
              name.kind = formula::NK::Name;
              name.str = t.name;
              n = std::move(name);
              changed = true;
              return;
            }
            // a range whose first or last cell has a name of its own: that end is named (Excel 16, measured:
            // =SUM(B2:C3) with Rate =$B$2 becomes =SUM(Rate:C3)), each end by its own $ when relative ones are kept
            if (n.ref.is_range && !n.ref.whole_rows && !n.ref.whole_cols) {
              const bool abs0 = n.ref.r0.abs && n.ref.c0.abs, abs1 = n.ref.r1.abs && n.ref.c1.abs;
              const Target* first = nullptr;
              const Target* last = nullptr;
              for (const Target& t : targets) {
                if (t.sheet != rs || !t.rect.IsCell()) continue;
                if (!first && t.rect.r0 == rr.rect.r0 && t.rect.c0 == rr.rect.c0 && (ignore_relative || abs0)) first = &t;
                if (!last && t.rect.r0 == rr.rect.r1 && t.rect.c0 == rr.rect.c1 && (ignore_relative || abs1)) last = &t;
              }
              if (first || last) {
                auto end_node = [&](const Target* t, bool second) {
                  formula::Node x;
                  if (t) {
                    x.kind = formula::NK::Name;
                    x.str = t->name;
                    return x;
                  }
                  x = n;
                  x.ref.is_range = false;
                  if (second) {
                    x.ref.r0 = n.ref.r1;
                    x.ref.c0 = n.ref.c1;
                  }
                  x.ref.r1 = x.ref.r0;
                  x.ref.c1 = x.ref.c0;
                  return x;
                };
                formula::Node repl;
                repl.kind = formula::NK::Binary;
                repl.op = ':';
                repl.kids.push_back(end_node(first, false));
                repl.kids.push_back(end_node(last, true));
                n = std::move(repl);
                changed = true;
                return;
              }
            }
            // Use row and column names: one cell no name covers exactly, where a name of a row of cells crosses a
            // name of a column of cells: "RowName ColumnName" (Excel's intersection operator); the name of the
            // formula's own column (or row) is left out with Omit column (row) name, and the other one is then read
            // in the formula's column (row) by implicit intersection (@)
            if (o.use_row_column && rr.rect.IsCell() && (ignore_relative || absolute)) {
              const Target* row_name = nullptr;
              const Target* col_name = nullptr;
              const uint32_t cr = rr.rect.r0, cc = rr.rect.c0;
              for (const Target& t : targets) {
                if (t.sheet != rs || t.rect.IsCell() || !t.rect.Contains(cr, cc)) continue;
                if (!row_name && t.rect.r0 == t.rect.r1) row_name = &t;
                if (!col_name && t.rect.c0 == t.rect.c1) col_name = &t;
              }
              if (row_name && col_name && row_name != col_name) {
                const bool same_sheet = rs == sheet;
                const bool omit_col = o.omit_column && same_sheet && origin.col == cc;
                const bool omit_row = o.omit_row && same_sheet && origin.row == cr;
                auto name_node = [](const std::string& nm) {
                  formula::Node x;
                  x.kind = formula::NK::Name;
                  x.str = nm;
                  return x;
                };
                formula::Node repl;
                if (omit_col && omit_row) {
                  // the formula's own cell: nothing to name
                } else if (omit_col || omit_row) {
                  repl.kind = formula::NK::Unary;
                  repl.op = '@';
                  repl.kids.push_back(name_node(omit_col ? row_name->name : col_name->name));
                } else {
                  repl.kind = formula::NK::Binary;
                  repl.op = ' ';
                  repl.kids.push_back(name_node(o.row_first ? row_name->name : col_name->name));
                  repl.kids.push_back(name_node(o.row_first ? col_name->name : row_name->name));
                }
                if (!(omit_col && omit_row)) {
                  n = std::move(repl);
                  changed = true;
                  return;
                }
              }
            }
          }
        }
        for (formula::Node& k : n.kids) walk(k);
      };
      walk(root);
      if (changed) out.emplace_back(origin, formula::ToA1(root, origin, this));  // SetFormula takes it without "="
    }
  }
  int done = 0;
  for (const auto& [p, text] : out) done += SetFormula(sheet, p.row, p.col, text) ? 1 : 0;
  return done;
}

}  // namespace yc::engine
