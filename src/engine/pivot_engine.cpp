// SPDX-License-Identifier: Apache-2.0
// pivot_engine.cpp - PivotTable reports: reading the source, grouping, aggregating and laying out the report
#include "core/i18n.h"
#include "engine/pivot_engine.h"

#include "engine/document.h"
#include "model/sheet.h"
#include "model/table.h"
#include "model/workbook.h"
#include "formula/datetime.h"
#include "formula/eval.h"
#include "formula/functions.h"
#include "formula/numfmt.h"
#include "formula/parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <tuple>

namespace yc::model {
const char* PivotFuncName(PivotFunc f) {
  static const char* kNames[] = {TR("Sum"), TR("Count"), TR("Average"), TR("Max"), TR("Min"), TR("Product"), TR("Count Numbers"), TR("StdDev"), TR("StdDevp"), TR("Var"), TR("Varp"), TR("Distinct Count")};
  const int i = static_cast<int>(f);
  return i >= 0 && i < kPivotFuncs ? kNames[i] : "Sum";
}
}  // namespace yc::model

namespace yc::engine {

using core::Value;
using model::PivotFunc;
using model::PivotShowAs;

namespace {

std::string LowerStr(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// month names of grouped dates: English, as the items' identity (saved filters and files name them)
std::string MonthShort(int m) {
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return kMonths[std::clamp(m, 1, 12) - 1];
}

// Use Custom Lists when sorting: month and weekday names (English and the UI language) in calendar order;
// -1 = not in a list
int CustomListRank(const std::string& lower) {
  static const std::unordered_map<std::string, int> table = [] {
    std::unordered_map<std::string, int> t;
    auto add = [&](const std::string& s, int rank) { t.emplace(LowerStr(s), rank); };
    static const char* kMonEn[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char* kDayEn[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    for (int m = 0; m < 12; ++m) {
      add(std::string(kMonEn[m]).substr(0, 3), m);
      add(kMonEn[m], 100 + m);
      const Value v = Value::Number(formula::DateSerial(2001, m + 1, 1));
      add(DisplayTextOf(v, "mmm"), m);
      add(DisplayTextOf(v, "mmmm"), 100 + m);
    }
    for (int d = 0; d < 7; ++d) {
      add(std::string(kDayEn[d]).substr(0, 3), 200 + d);
      add(kDayEn[d], 300 + d);
      const Value v = Value::Number(formula::DateSerial(2006, 1, 1 + d));  // 2006-01-01 was a Sunday
      add(DisplayTextOf(v, "ddd"), 200 + d);
      add(DisplayTextOf(v, "dddd"), 300 + d);
    }
    return t;
  }();
  const auto it = table.find(lower);
  return it == table.end() ? -1 : it->second;
}

// item order: numbers, text (case-insensitive; names of months and days in calendar order first), TRUE / FALSE,
// errors, blanks last
int Rank(const Value& v) {
  if (v.IsNumeric()) return 0;
  if (v.IsString()) return 1;
  if (v.IsBool()) return 2;
  if (v.IsError()) return 3;
  return 4;
}

// an item's identity when the source is read: its kind and its text without case (Excel: AAPL and aapl are one item,
// shown as first spelled; the text "10" and the number 10 are two)
std::string ItemKey(const Value& v) { return std::to_string(Rank(v)) + '\x1f' + formula::FoldCase(PivotItemText(v)); }

// item names saved in the PivotTable (hidden, collapsed, the page item) match the cache's items without case: the
// spelling an item shows is its first record's, which may change when the source does
bool ItemNameEq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;  // FoldCase keeps the UTF-8 length
  if (a == b) return true;
  for (size_t i = 0; i < a.size(); ++i) {
    const unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
    if (x >= 0x80 || y >= 0x80) return formula::FoldCase(a) == formula::FoldCase(b);
    if (std::tolower(x) != std::tolower(y)) return false;
  }
  return true;
}
bool NameListed(const std::vector<std::string>& list, const std::string& item) {
  return std::any_of(list.begin(), list.end(), [&](const std::string& s) { return ItemNameEq(s, item); });
}

bool ItemLess(const Value& a, const Value& b, bool custom) {
  const int ra = Rank(a), rb = Rank(b);
  if (ra != rb) return ra < rb;
  if (ra == 0) return a.AsDouble() < b.AsDouble();
  if (ra == 1) {
    const std::string la = LowerStr(std::string(a.str())), lb = LowerStr(std::string(b.str()));
    if (custom) {
      const int ka = CustomListRank(la), kb = CustomListRank(lb);
      if (ka >= 0 || kb >= 0) {
        if (ka < 0) return false;
        if (kb < 0) return true;
        if (ka != kb) return ka < kb;
      }
    }
    return la < lb;
  }
  if (ra == 2) return !a.boolean() && b.boolean();
  return false;
}

struct Acc {
  double sum = 0, sumsq = 0, prod = 1, mn = std::numeric_limits<double>::infinity(), mx = -std::numeric_limits<double>::infinity();
  int64_t n = 0;         // numbers
  int64_t nonblank = 0;  // non-empty values
  int64_t records = 0;
  bool has_error = false;  // an error among the values: sums and the other math show it (Excel), counts count it
  core::Err error = core::Err::NA;
  std::shared_ptr<std::set<std::string>> distinct;  // Distinct Count: the values seen (set up by the caller)
  void Add(const Value& v) {
    ++records;
    if (v.IsEmpty()) return;
    ++nonblank;
    if (distinct) distinct->insert(LowerStr(PivotItemText(v)));
    if (v.IsError() && !has_error) {
      has_error = true;
      error = v.error();  // the first one
    }
    if (!v.IsNumeric()) return;
    const double x = v.AsDouble();
    ++n;
    sum += x;
    sumsq += x * x;
    prod *= x;
    mn = std::min(mn, x);
    mx = std::max(mx, x);
  }
  Value Result(PivotFunc f) const {
    if (records == 0) return Value();
    if (nonblank == 0) return Value();  // only blank values: an empty cell, not 0 (Excel; Count too)
    auto div0 = [] { return Value::Error(core::Err::Div0); };
    if (has_error && f != PivotFunc::Count && f != PivotFunc::CountNums && f != PivotFunc::DistinctCount) return Value::Error(error);
    switch (f) {
      case PivotFunc::Sum: return Value::Number(sum);
      case PivotFunc::Count: return Value::Number(static_cast<double>(nonblank));
      case PivotFunc::DistinctCount: return Value::Number(distinct ? static_cast<double>(distinct->size()) : 0.0);
      case PivotFunc::CountNums: return Value::Number(static_cast<double>(n));
      case PivotFunc::Average: return n ? Value::Number(sum / static_cast<double>(n)) : div0();
      case PivotFunc::Max: return Value::Number(n ? mx : 0.0);
      case PivotFunc::Min: return Value::Number(n ? mn : 0.0);
      case PivotFunc::Product: return Value::Number(n ? prod : 0.0);
      case PivotFunc::Var:
      case PivotFunc::StdDev: {
        if (n < 2) return div0();
        const double var = std::max(0.0, (sumsq - sum * sum / static_cast<double>(n)) / static_cast<double>(n - 1));
        return Value::Number(f == PivotFunc::Var ? var : std::sqrt(var));
      }
      case PivotFunc::VarP:
      case PivotFunc::StdDevP: {
        if (n < 1) return div0();
        const double var = std::max(0.0, (sumsq - sum * sum / static_cast<double>(n)) / static_cast<double>(n));
        return Value::Number(f == PivotFunc::VarP ? var : std::sqrt(var));
      }
    }
    return Value();
  }
};

std::string KeyStr(const std::vector<int>& row, const std::vector<int>& col) {
  std::string k;
  k.reserve((row.size() + col.size()) * 4 + 2);
  for (int i : row) k += std::to_string(i) + ",";
  k += "|";
  for (int i : col) k += std::to_string(i) + ",";
  return k;
}

bool ValidField(const PivotCache& c, int f) { return f >= 0 && static_cast<size_t>(f) < c.names.size(); }
// fields that can go on Rows / Columns / Filters (calculated fields only go to Values)
bool ValidAxis(const PivotCache& c, int f) { return f >= 0 && static_cast<size_t>(f) < c.source_fields && static_cast<size_t>(f) < c.items.size(); }


// a calculated field's formula for the formula engine: every field name (bare, or 'quoted' when it has spaces) becomes
// the name _ycpf<k> (k = its place in refs), so any worksheet function works on the fields' sums (=IF(Sales>100,
// Sales*0.1,0), =ROUND(Sales/Qty,2)). False when the text can't be read
bool PrepareCalcFormula(const std::vector<std::string>& names, size_t source_fields, const std::string& formula, std::string* text, std::vector<int>* refs) {
  refs->clear();
  text->clear();
  size_t i = 0;
  while (i < formula.size() && (formula[i] == ' ' || formula[i] == '\t')) ++i;
  if (i < formula.size() && formula[i] == '=') ++i;
  auto field_of = [&](const std::string& n) {
    for (size_t f = 0; f < source_fields && f < names.size(); ++f) {
      if (LowerStr(names[f]) == LowerStr(n)) return static_cast<int>(f);
    }
    return -1;
  };
  auto placeholder = [&](int f) {
    auto it = std::find(refs->begin(), refs->end(), f);
    if (it == refs->end()) {
      refs->push_back(f);
      it = refs->end() - 1;
    }
    return "_ycpf" + std::to_string(it - refs->begin());
  };
  auto ident_start = [](unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; };
  auto ident_char = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c >= 0x80; };
  while (i < formula.size()) {
    const char ch = formula[i];
    if (ch == '"') {
      // a text constant ("" inside it)
      size_t j = i + 1;
      while (j < formula.size()) {
        if (formula[j] == '"') {
          if (j + 1 < formula.size() && formula[j + 1] == '"') {
            j += 2;
            continue;
          }
          break;
        }
        ++j;
      }
      if (j >= formula.size()) return false;
      *text += formula.substr(i, j + 1 - i);
      i = j + 1;
      continue;
    }
    if (ch == '\'') {
      std::string name;
      size_t j = i + 1;
      bool closed = false;
      while (j < formula.size()) {
        if (formula[j] == '\'') {
          if (j + 1 < formula.size() && formula[j + 1] == '\'') {
            name += '\'';
            j += 2;
            continue;
          }
          closed = true;
          ++j;
          break;
        }
        name += formula[j++];
      }
      const int f = closed ? field_of(name) : -1;
      if (f < 0) return false;
      *text += placeholder(f);
      i = j;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) || (ch == '.' && i + 1 < formula.size() && std::isdigit(static_cast<unsigned char>(formula[i + 1])))) {
      size_t j = i;
      while (j < formula.size()) {
        const char c = formula[j];
        const bool exp = (c == 'e' || c == 'E') && j + 1 < formula.size() &&
                         (std::isdigit(static_cast<unsigned char>(formula[j + 1])) || formula[j + 1] == '+' || formula[j + 1] == '-');
        const bool sign = (c == '+' || c == '-') && j > i && (formula[j - 1] == 'e' || formula[j - 1] == 'E');
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || exp || sign)) break;
        ++j;
      }
      *text += formula.substr(i, j - i);
      i = j;
      continue;
    }
    if (ident_start(static_cast<unsigned char>(ch))) {
      size_t j = i;
      while (j < formula.size() && ident_char(static_cast<unsigned char>(formula[j]))) ++j;
      const std::string word = formula.substr(i, j - i);
      size_t k = j;
      while (k < formula.size() && formula[k] == ' ') ++k;
      const bool call = k < formula.size() && formula[k] == '(';
      const int f = call ? -1 : field_of(word);
      *text += f >= 0 ? placeholder(f) : word;
      i = j;
      continue;
    }
    *text += ch;
    ++i;
  }
  return true;
}

// the field sums a calculated field's formula reads (by the names PrepareCalcFormula gave them); no cells
class CalcFieldHost final : public formula::EvalHost {
 public:
  explicit CalcFieldHost(const std::vector<double>& v) : v_(v) {}
  const Value& Cell(int32_t, uint32_t, uint32_t) override {
    static const Value kEmpty;
    return kEmpty;
  }
  void ForEachCell(int32_t, const core::Rect&, const std::function<bool(uint32_t, uint32_t, const Value&)>&) override {}
  uint32_t RowLimit(int32_t) override { return 1; }
  core::Rect UsedRect(int32_t) override { return core::Rect{}; }
  int32_t FindSheet(std::string_view) override { return -1; }
  std::string SheetName(int32_t) override { return {}; }
  int32_t SheetIndex(int32_t) override { return 1; }
  int32_t SheetCount() override { return 1; }
  bool ResolveName(std::string_view name, int32_t, core::CellPos, formula::EV* out) override {
    std::string low(name);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low.size() < 6 || low.rfind("_ycpf", 0) != 0) return false;
    const size_t k = static_cast<size_t>(std::strtoul(low.c_str() + 5, nullptr, 10));
    if (k >= v_.size()) return false;
    *out = formula::EV::S(Value::Number(v_[k]));
    return true;
  }
  formula::EV CallUdf(const std::string&, const std::vector<formula::EV>&, int32_t, core::CellPos) override { return formula::EV::Err(core::Err::Name); }
  formula::EV Stream(const std::string&, const std::vector<formula::EV>&, int32_t, core::CellPos) override { return formula::EV::Err(core::Err::Name); }

 private:
  const std::vector<double>& v_;
};

// a calculated field's formula, parsed: the fields it reads, or why it can't be used
struct CalcFormula {
  bool ok = false;
  std::string why;
  std::vector<int> refs;
  std::shared_ptr<formula::Node> root;
};

CalcFormula CompileCalcFormula(const std::vector<std::string>& names, size_t source_fields, const std::string& formula) {
  CalcFormula c;
  std::string text;
  const std::string bad = TR("The formula uses a name that isn't a field of the source, or has a syntax error.");
  if (!PrepareCalcFormula(names, source_fields, formula, &text, &c.refs)) {
    c.why = bad;
    return c;
  }
  formula::ParseResult pr = formula::Parse(text, core::CellPos{0, 0}, nullptr);
  if (!pr.ok) {
    c.why = bad;
    return c;
  }
  // only fields, constants and worksheet functions: a cell reference or another name is refused
  std::vector<std::string> used;
  formula::CollectNames(pr.root, &used);
  for (const auto& n : used) {
    std::string low = n;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (low.rfind("_ycpf", 0) != 0) {
      c.why = bad;
      return c;
    }
  }
  std::vector<formula::ResolvedRef> cells;
  formula::CollectRefs(pr.root, core::CellPos{0, 0}, &cells);
  if (!cells.empty()) {
    c.why = TR("References, names, and arrays can't be used in PivotTable formulas.");
    return c;
  }
  c.root = std::make_shared<formula::Node>(std::move(pr.root));
  c.ok = true;
  return c;
}

// a data field's plan: a calculated field sums its source fields, then evaluates the formula on the sums
struct DataPlan {
  bool calc = false;
  std::shared_ptr<formula::Node> root;
  std::vector<int> refs;
  size_t offset = 0;  // its sums in the accumulator vector
};

Value EvalPlan(const DataPlan& p, const PivotCache&, const std::vector<double>& sums) {
  if (!p.root) return Value::Error(core::Err::Name);
  CalcFieldHost host(sums);
  Value v = formula::EvaluateCell(*p.root, host, 0, core::CellPos{0, 0});
  if (v.IsEmpty()) return Value::Number(0);
  return v;
}

std::vector<DataPlan> PlanData(const model::PivotTable& pt, const PivotCache& cache, size_t* accs) {
  std::vector<DataPlan> plans(pt.data.size());
  *accs = std::max<size_t>(pt.data.size(), 1);
  for (size_t d = 0; d < pt.data.size(); ++d) {
    const int f = pt.data[d].field;
    if (static_cast<size_t>(f) < cache.source_fields) continue;
    const size_t ci = static_cast<size_t>(f) - cache.source_fields;
    if (ci >= pt.calc_fields.size()) continue;
    plans[d].calc = true;
    CalcFormula cf = CompileCalcFormula(cache.names, cache.columns, pt.calc_fields[ci].formula);
    plans[d].refs = std::move(cf.refs);
    plans[d].root = std::move(cf.root);
    plans[d].offset = *accs;
    *accs += plans[d].refs.size();
  }
  return plans;
}

// wildcards of Excel's filters: * any text, ? one character, ~ escapes (both texts lower-case)
bool WildMatch(const std::string& t, size_t ti, const std::string& p, size_t pi) {
  while (pi < p.size()) {
    if (p[pi] == '*') {
      for (size_t k = ti; k <= t.size(); ++k) {
        if (WildMatch(t, k, p, pi + 1)) return true;
      }
      return false;
    }
    if (ti >= t.size()) return false;
    if (p[pi] == '~' && pi + 1 < p.size()) ++pi;
    else if (p[pi] == '?') {
      ++ti, ++pi;
      continue;
    }
    if (t[ti] != p[pi]) return false;
    ++ti, ++pi;
  }
  return ti == t.size();
}

bool ParseNum(const std::string& s, double* out) {
  if (s.empty()) return false;
  char* e = nullptr;
  *out = std::strtod(s.c_str(), &e);
  while (e && *e == ' ') ++e;
  return e && *e == 0;
}

// a label filter (Label Filters: Equals... Between) on an item as shown
bool LabelPass(const model::PivotFilter& f, const std::string& text, const Value& v) {
  const std::string t = LowerStr(text), a = LowerStr(f.v1), b = LowerStr(f.v2);
  auto cmp = [&](const std::string& with, const std::string& raw) {
    double x = 0;
    if (v.IsNumeric() && ParseNum(raw, &x)) return v.AsDouble() < x ? -1 : v.AsDouble() > x ? 1 : 0;
    return t < with ? -1 : t > with ? 1 : 0;
  };
  switch (f.type) {
    case 15: return WildMatch(t, 0, a, 0);
    case 16: return !WildMatch(t, 0, a, 0);
    case 17: return t.rfind(a, 0) == 0;
    case 18: return t.rfind(a, 0) != 0;
    case 19: return t.size() >= a.size() && t.compare(t.size() - a.size(), a.size(), a) == 0;
    case 20: return !(t.size() >= a.size() && t.compare(t.size() - a.size(), a.size(), a) == 0);
    case 21: return t.find(a) != std::string::npos;
    case 22: return t.find(a) == std::string::npos;
    case 23: return cmp(a, f.v1) > 0;
    case 24: return cmp(a, f.v1) >= 0;
    case 25: return cmp(a, f.v1) < 0;
    case 26: return cmp(a, f.v1) <= 0;
    case 27: return cmp(a, f.v1) >= 0 && cmp(b, f.v2) <= 0;
    case 28: return !(cmp(a, f.v1) >= 0 && cmp(b, f.v2) <= 0);
    default: return true;
  }
}

// records that pass the report filters (page items, hidden items, label filters, slicers, timelines)
std::vector<size_t> FilteredRecords(const model::PivotTable& pt, const PivotCache& cache) {
  // which items of each filtered field pass (worked out once per field, not per record)
  struct Gate {
    int field;
    std::vector<char> ok;
  };
  std::vector<Gate> gates;
  auto gate = [&](const model::PivotAxisField& f, bool page) {
    if (!ValidAxis(cache, f.field)) return;
    const auto& items = cache.items[static_cast<size_t>(f.field)];
    const bool label = f.label_filter.type >= 15 && f.label_filter.type <= 28 && !page;
    if (f.hidden.empty() && !(page && !f.page_item.empty()) && !label) return;
    Gate g{f.field, std::vector<char>(items.size(), 1)};
    for (size_t i = 0; i < items.size(); ++i) {
      bool ok = !PivotItemHidden(f, items[i]);
      if (page && !f.page_item.empty() && !f.multi_page) ok = ok && ItemNameEq(items[i], f.page_item);
      if (label && ok) ok = LabelPass(f.label_filter, cache.display[static_cast<size_t>(f.field)][i], cache.item_values[static_cast<size_t>(f.field)][i]);
      g.ok[i] = ok ? 1 : 0;
    }
    gates.push_back(std::move(g));
  };
  for (const auto& f : pt.rows) gate(f, false);
  for (const auto& f : pt.cols) gate(f, false);
  for (const auto& f : pt.pages) gate(f, true);
  for (const auto& f : pt.slicer_filters) gate(f, false);
  std::vector<size_t> out;
  out.reserve(cache.records.size());
  for (size_t r = 0; r < cache.records.size(); ++r) {
    bool ok = true;
    for (const Gate& g : gates) {
      if (!g.ok[static_cast<size_t>(cache.item_of[static_cast<size_t>(g.field)][r])]) {
        ok = false;
        break;
      }
    }
    for (const auto& df : pt.date_filters) {
      if (!ok || df.field < 0 || static_cast<size_t>(df.field) >= cache.source_fields) continue;
      const Value& v = cache.records[r][static_cast<size_t>(df.field)];
      const double day = v.IsNumeric() ? std::floor(v.AsDouble()) : -1e300;
      ok = day >= df.from && day <= df.to;
    }
    if (ok) out.push_back(r);
  }
  return out;
}

// the label of a value grouped by a period (dates) or a range (numbers)
struct GroupLabeler {
  const model::PivotGroup* g = nullptr;
  uint32_t period = 0;  // a model::PivotPeriod; 0 = the group's own `by` (files from before several periods)
  double start = 0, end = 0;
  std::string datefmt;
  bool whole_data = true;  // numbers: every value of the field is a whole number (labels "0-9", else "0-10")
  bool blank_low = false;  // numbers from a Starting at the user set, with blanks: they join "<start or (blank)" (Excel)
  std::string Label(const Value& v, double* order) const {
    const bool numbers = g->by == model::PivotGroupBy::Numbers && period == 0;
    auto below = [&] {
      *order = -1e300;
      return "<" + DisplayTextOf(Value::Number(start), "General") + (blank_low ? " or (blank)" : "");
    };
    if (!v.IsNumeric()) {
      if (numbers && blank_low && v.IsEmpty()) return below();
      *order = 1e300;  // text and blanks after the groups
      return PivotItemText(v);
    }
    const double x = v.AsDouble();
    if (numbers) {
      const double step = g->interval > 0 ? g->interval : 1;
      if (x < start) return below();
      if (end > start && x > end) {
        *order = 1e299;
        return ">" + DisplayTextOf(Value::Number(end), "General");
      }
      const double lo = start + std::floor((x - start) / step) * step;
      *order = lo;
      // whole numbers (the step, the start and the field's values): "0-9"; otherwise "0-10" (lower bound inclusive)
      const bool whole = whole_data && std::floor(step) == step && std::floor(start) == start;
      return DisplayTextOf(Value::Number(lo), "General") + "-" + DisplayTextOf(Value::Number(whole ? lo + step - 1 : lo + step), "General");
    }
    // dates: Starting at / Ending at (when set) put the dates outside in "<date" / ">date"
    const std::string fmt = datefmt.empty() ? std::string("yyyy-mm-dd") : datefmt;
    if (period != 0 && start > 0 && std::floor(x) < start) {
      *order = -1e300;
      return "<" + DisplayTextOf(Value::Number(start), fmt);
    }
    if (period != 0 && end > 0 && end >= start && std::floor(x) > end) {
      *order = 1e299;
      return ">" + DisplayTextOf(Value::Number(end), fmt);
    }
    int y = 0, m = 0, d = 0;
    if (!formula::SerialToYmd(x, &y, &m, &d)) {
      *order = 1e300;
      return PivotItemText(v);
    }
    const double frac = x - std::floor(x);
    const int secs = static_cast<int>(std::lround(frac * 86400.0)) % 86400;
    char b[64];
    switch (period) {
      case model::kPeriodYears: *order = y; return std::to_string(y);
      case model::kPeriodQuarters: *order = (m - 1) / 3; return "Qtr" + std::to_string((m - 1) / 3 + 1);
      case model::kPeriodMonths: *order = m; return MonthShort(m);
      case model::kPeriodDays: {
        if (g->days <= 1) {
          *order = m * 100 + d;
          return std::to_string(d) + "-" + MonthShort(m);
        }
        const double base = start > 0 ? start : std::floor(x);
        const double lo = base + std::floor((std::floor(x) - base) / g->days) * g->days;
        *order = lo;
        // the last group ends at Ending at (Excel: "2026/3/17 - 2026/3/20", not a whole step past it)
        const double hi = end > 0 && end >= lo ? std::min(lo + g->days - 1, end) : lo + g->days - 1;
        return DisplayTextOf(Value::Number(lo), fmt) + " - " + DisplayTextOf(Value::Number(hi), fmt);
      }
      case model::kPeriodHours: {
        const int h = secs / 3600;
        *order = h;
        std::snprintf(b, sizeof(b), "%d %s", h % 12 == 0 ? 12 : h % 12, h < 12 ? "AM" : "PM");
        return b;
      }
      case model::kPeriodMinutes: *order = (secs / 60) % 60; std::snprintf(b, sizeof(b), ":%02d", (secs / 60) % 60); return b;
      case model::kPeriodSeconds: *order = secs % 60; std::snprintf(b, sizeof(b), ":%02d", secs % 60); return b;
      default: break;
    }
    // files from before several periods: one period by `by`
    switch (g->by) {
      case model::PivotGroupBy::Years: *order = y; return std::to_string(y);
      case model::PivotGroupBy::Quarters: *order = (m - 1) / 3; return "Qtr" + std::to_string((m - 1) / 3 + 1);
      case model::PivotGroupBy::Months: *order = m; return MonthShort(m);
      case model::PivotGroupBy::YearMonths: {
        *order = y * 100.0 + m;
        std::snprintf(b, sizeof(b), "%04d-%02d", y, m);
        return b;
      }
      default: {
        *order = std::floor(x);
        std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
        return b;
      }
    }
  }
};

const uint32_t kPeriodOrder[] = {model::kPeriodYears, model::kPeriodQuarters, model::kPeriodMonths, model::kPeriodDays,
                                 model::kPeriodHours, model::kPeriodMinutes, model::kPeriodSeconds};

uint32_t FinestPeriod(uint32_t periods) {
  for (uint32_t bit = 1; bit <= model::kPeriodYears; bit <<= 1) {
    if (periods & bit) return bit;
  }
  return 0;
}

// the fields a date grouped by several periods makes (English: the PivotTable names its fields by them)
const char* PeriodFieldName(uint32_t p) {
  switch (p) {
    case model::kPeriodYears: return "Years";
    case model::kPeriodQuarters: return "Quarters";
    case model::kPeriodMonths: return "Months";
    case model::kPeriodDays: return "Days";
    case model::kPeriodHours: return "Hours";
    case model::kPeriodMinutes: return "Minutes";
    default: return "Seconds";
  }
}

}  // namespace

bool PivotShowAsPercent(PivotShowAs k) {
  switch (k) {
    case PivotShowAs::PercentOfGrandTotal:
    case PivotShowAs::PercentOfColumnTotal:
    case PivotShowAs::PercentOfRowTotal:
    case PivotShowAs::PercentOf:
    case PivotShowAs::PercentOfParentRow:
    case PivotShowAs::PercentOfParentColumn:
    case PivotShowAs::PercentOfParent:
    case PivotShowAs::PercentDifferenceFrom:
    case PivotShowAs::PercentRunningTotal: return true;
    default: return false;
  }
}

bool PivotShowAsNeedsBase(PivotShowAs k) {
  switch (k) {
    case PivotShowAs::RunningTotal:
    case PivotShowAs::PercentOf:
    case PivotShowAs::PercentOfParent:
    case PivotShowAs::DifferenceFrom:
    case PivotShowAs::PercentDifferenceFrom:
    case PivotShowAs::PercentRunningTotal:
    case PivotShowAs::RankAscending:
    case PivotShowAs::RankDescending: return true;
    default: return false;
  }
}

bool PivotShowAsNeedsBaseItem(PivotShowAs k) {
  return k == PivotShowAs::PercentOf || k == PivotShowAs::DifferenceFrom || k == PivotShowAs::PercentDifferenceFrom;
}

bool PivotItemHidden(const model::PivotAxisField& af, const std::string& item) {
  if (NameListed(af.hidden, item)) return true;
  // a manual filter without "Include new items in manual filter": items that came after it are left out
  return !af.include_new && !af.hidden.empty() && !af.known.empty() && !NameListed(af.known, item);
}

int PivotLabelLevel(const PivotReport& rep, uint32_t r, uint32_t c) {
  if (r >= rep.row_filter.size()) return -1;
  const auto& rf = rep.row_filter[r];
  if (rf.empty()) return -1;
  int lvl = -1;
  for (size_t L = 0; L < rf.size() && L < rep.row_field_col.size(); ++L) {
    if (rep.row_field_col[L] == c) lvl = static_cast<int>(L);
  }
  return lvl >= 0 ? lvl : static_cast<int>(rf.size()) - 1;
}

int PivotColumnField(const PivotReport& rep, uint32_t c) {
  for (size_t L = 0; L < rep.row_field_col.size(); ++L) {
    if (rep.row_field_col[L] == c) return static_cast<int>(L);
  }
  return -1;
}

bool PivotItemCollapsed(const model::PivotAxisField& af, const std::string& item) {
  const bool listed = NameListed(af.collapsed, item);
  return af.collapse_all ? !listed : listed;
}

void SetPivotItemCollapsed(model::PivotAxisField& af, const std::string& item, bool collapsed) {
  auto& v = af.collapsed;
  v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& s) { return ItemNameEq(s, item); }), v.end());
  if (collapsed != af.collapse_all) v.push_back(item);
}

bool CheckPivotFormula(const std::vector<std::string>& source_names, const std::string& formula, std::string* err) {
  std::vector<int> refs;
  std::string t = formula;
  while (!t.empty() && t[0] == ' ') t.erase(0, 1);
  if (t.empty() || t == "=") {
    *err = TR("Enter a formula, for example =Sales/Qty.");
    return false;
  }
  const CalcFormula c = CompileCalcFormula(source_names, source_names.size(), t);
  if (!c.ok) {
    *err = c.why;
    return false;
  }
  return true;
}

std::string PivotItemText(const Value& v) {
  if (v.IsEmpty()) return "(blank)";
  return DisplayTextOf(v, "General");
}

std::string PivotItemDisplay(const PivotCache& cache, int field, const std::string& identity) {
  if (field < 0 || static_cast<size_t>(field) >= cache.items.size() || static_cast<size_t>(field) >= cache.display.size()) return identity;
  const auto& items = cache.items[static_cast<size_t>(field)];
  for (size_t i = 0; i < items.size(); ++i) {
    if (ItemNameEq(items[i], identity)) return cache.display[static_cast<size_t>(field)][i];
  }
  return identity;
}

std::string PivotFieldCaption(const model::PivotTable& pt, const PivotCache& cache, int field) {
  for (const auto* axis : {&pt.rows, &pt.cols, &pt.pages}) {
    for (const auto& a : *axis) {
      if (a.field == field && !a.caption.empty()) return a.caption;
    }
  }
  return ValidField(cache, field) ? cache.names[static_cast<size_t>(field)] : std::string("?");
}

int PivotFieldDefaultArea(const PivotCache& cache, int field) {
  if (field < 0 || static_cast<size_t>(field) >= cache.names.size()) return 0;
  if (static_cast<size_t>(field) >= cache.source_fields) return 1;  // a calculated field
  if (static_cast<size_t>(field) >= cache.columns) return 0;        // a field made by a group
  if (static_cast<size_t>(field) < cache.numfmts.size() && formula::IsDateFormat(cache.numfmts[static_cast<size_t>(field)])) return 0;
  bool numbers = false, other = false;
  const size_t n = std::min(cache.source_records, cache.records.size());
  for (size_t r = 0; r < n; ++r) {
    const Value& v = cache.records[r][static_cast<size_t>(field)];
    if (v.IsNumeric()) numbers = true;
    else other = true;
  }
  if (!numbers) return 0;
  return other ? 2 : 1;
}

bool ParseCalcItem(const std::vector<std::string>& items, const std::string& formula, std::vector<std::pair<std::string, double>>* terms, std::string* err) {
  terms->clear();
  std::string f = formula;
  size_t i = 0;
  while (i < f.size() && f[i] == ' ') ++i;
  if (i < f.size() && f[i] == '=') ++i;
  auto skip = [&]() {
    while (i < f.size() && f[i] == ' ') ++i;
  };
  auto number = [&](double* v) {
    skip();
    const char* b = f.c_str() + i;
    char* e = nullptr;
    *v = std::strtod(b, &e);
    if (e == b) return false;
    i += static_cast<size_t>(e - b);
    return true;
  };
  auto item = [&](std::string* name) {
    skip();
    if (i < f.size() && f[i] == '\'') {  // 'North East'
      const size_t e = f.find('\'', i + 1);
      if (e == std::string::npos) return false;
      *name = f.substr(i + 1, e - i - 1);
      i = e + 1;
      return true;
    }
    const size_t b = i;
    while (i < f.size() && f[i] != '+' && f[i] != '-' && f[i] != '*' && f[i] != '/' && f[i] != ' ') ++i;
    *name = f.substr(b, i - b);
    return !name->empty();
  };
  auto find = [&](const std::string& n) -> const std::string* {
    for (const auto& it : items) {
      if (LowerStr(it) == LowerStr(n)) return &it;
    }
    return nullptr;
  };
  double sign = 1;
  skip();
  if (i < f.size() && (f[i] == '-' || f[i] == '+')) sign = f[i++] == '-' ? -1 : 1;
  while (true) {
    double coef = sign, k = 0;
    std::string name;
    // [number *] item [* number | / number]
    const size_t mark = i;
    if (number(&k)) {
      skip();
      if (i < f.size() && f[i] == '*') {
        ++i;
        coef *= k;
      } else {
        i = mark;
      }
    }
    if (!item(&name)) {
      *err = TR("Expected an item name in the formula");
      return false;
    }
    const std::string* it = find(name);
    if (!it) {
      *err = TR("Not an item of this field: ") + name;
      return false;
    }
    skip();
    while (i < f.size() && (f[i] == '*' || f[i] == '/')) {
      const char op = f[i++];
      if (!number(&k) || (op == '/' && k == 0)) {
        *err = TR("Calculated items can multiply or divide items by numbers only");
        return false;
      }
      coef = op == '*' ? coef * k : coef / k;
      skip();
    }
    terms->emplace_back(*it, coef);
    skip();
    if (i >= f.size()) break;
    if (f[i] != '+' && f[i] != '-') {
      *err = TR("Calculated items support sums and differences of items (=East+West, =North*1.1)");
      return false;
    }
    sign = f[i++] == '-' ? -1 : 1;
  }
  if (terms->empty()) {
    *err = TR("The formula is empty");
    return false;
  }
  return true;
}

namespace {

// a PivotTable on the Data Model, which YCGrid no longer has (ADR 0012): kept in the file, not refreshed
bool ReadModelSource(const model::Workbook&, const model::PivotTable&, PivotCache*, std::string* err) {
  *err = TR("This PivotTable reads the Data Model, which YCGrid no longer has");
  return false;
}

}  // namespace

std::vector<int> RemapPivotFields(model::PivotTable* pt, const PivotCache& cache) {
  // the source's (and the groups') fields follow their names; calculated fields keep their place after them
  const size_t nold = pt->fields.size();
  const size_t old_src = pt->fields_src >= 0 ? std::min<size_t>(static_cast<size_t>(pt->fields_src), nold)
                                             : nold - std::min(nold, pt->calc_fields.size());
  const size_t new_src = cache.source_fields;
  const bool same = nold == 0 || (old_src == new_src && cache.names.size() >= old_src &&
                                  std::equal(pt->fields.begin(), pt->fields.begin() + static_cast<std::ptrdiff_t>(old_src), cache.names.begin()));
  if (same) {
    pt->fields = cache.names;
    pt->fields_src = static_cast<int64_t>(new_src);
    return {};
  }
  const size_t span = std::max(nold, cache.names.size()) + 64;
  std::vector<int> map(span, -1);
  for (size_t i = 0; i < old_src; ++i) {
    const std::string want = LowerStr(pt->fields[i]);
    for (size_t j = 0; j < new_src && j < cache.names.size(); ++j) {
      if (LowerStr(cache.names[j]) == want) {
        map[i] = static_cast<int>(j);
        break;
      }
    }
  }
  for (size_t i = old_src; i < span; ++i) {
    const size_t j = new_src + (i - old_src);
    map[i] = j < cache.names.size() ? static_cast<int>(j) : -1;
  }
  auto to = [&](int f) { return f >= 0 && static_cast<size_t>(f) < map.size() ? map[static_cast<size_t>(f)] : -1; };
  for (auto* axis : {&pt->rows, &pt->cols, &pt->pages, &pt->slicer_filters}) {
    for (auto& a : *axis) a.field = to(a.field);
    axis->erase(std::remove_if(axis->begin(), axis->end(), [](const model::PivotAxisField& a) { return a.field < 0; }), axis->end());
  }
  // data fields: their indexes move when one is dropped (sorts and value filters name them by index)
  std::vector<int> dmap(pt->data.size(), -1);
  std::vector<model::PivotDataField> data;
  for (size_t d = 0; d < pt->data.size(); ++d) {
    model::PivotDataField df = pt->data[d];
    df.field = to(df.field);
    if (df.field < 0) continue;
    if (df.base_field >= 0) df.base_field = to(df.base_field);
    dmap[d] = static_cast<int>(data.size());
    data.push_back(std::move(df));
  }
  pt->data = std::move(data);
  for (auto* axis : {&pt->rows, &pt->cols}) {
    for (auto& a : *axis) {
      if (a.sort_by >= 0) a.sort_by = static_cast<size_t>(a.sort_by) < dmap.size() ? dmap[static_cast<size_t>(a.sort_by)] : -1;
      if (a.value_filter.type != 0) {
        const int nd = a.value_filter.data >= 0 && static_cast<size_t>(a.value_filter.data) < dmap.size() ? dmap[static_cast<size_t>(a.value_filter.data)] : -1;
        if (nd < 0) a.value_filter = model::PivotFilter{};
        else a.value_filter.data = nd;
      }
    }
  }
  for (auto& d : pt->date_filters) d.field = to(d.field);
  pt->date_filters.erase(std::remove_if(pt->date_filters.begin(), pt->date_filters.end(), [](const model::PivotDateFilter& d) { return d.field < 0; }),
                         pt->date_filters.end());
  for (auto& g : pt->groups) g.field = to(g.field);
  pt->groups.erase(std::remove_if(pt->groups.begin(), pt->groups.end(), [](const model::PivotGroup& g) { return g.field < 0; }), pt->groups.end());
  for (auto& c : pt->calc_items) c.field = to(c.field);
  pt->calc_items.erase(std::remove_if(pt->calc_items.begin(), pt->calc_items.end(), [](const model::PivotCalcItem& c) { return c.field < 0; }),
                       pt->calc_items.end());
  pt->fields = cache.names;
  pt->fields_src = static_cast<int64_t>(new_src);
  map.resize(std::max(nold, static_cast<size_t>(1)));
  return map;
}

bool ReadPivotSource(const model::Workbook& wb, const model::PivotTable& pt, PivotCache* out, std::string* err) {
  *out = PivotCache{};
  if (pt.from_model) return ReadModelSource(wb, pt, out, err);
  uint32_t sid = pt.src_sheet;
  core::Rect r = pt.src;
  if (!pt.src_table.empty()) {
    const model::Table* t = model::FindTable(wb, pt.src_table, &sid);
    if (!t) {
      *err = TR("The PivotTable's source table doesn't exist: ") + pt.src_table;
      return false;
    }
    r = t->range;
    if (t->totals && r.r1 > r.r0) --r.r1;
  }
  const model::Sheet* sh = wb.SheetById(sid);
  if (!sh) {
    *err = TR("The PivotTable's source sheet doesn't exist");
    return false;
  }
  // entire columns: up to the used range
  if (r.r1 >= sh->row_limit - 1) {
    model::Sheet tmp = *sh;
    tmp.ComputeUsedRange();
    r.r1 = tmp.has_used ? std::max(r.r0, std::min(r.r1, tmp.used.r1)) : r.r0;
  }
  if (r.c1 >= core::kMaxCols - 1 || r.Cols() > 512) {
    *err = TR("The PivotTable's source is too wide");
    return false;
  }
  if (r.r1 <= r.r0) {
    *err = TR("The PivotTable's source needs a header row and at least one row of data");
    return false;
  }
  const uint32_t nf = static_cast<uint32_t>(r.Cols());
  std::set<std::string> seen;
  for (uint32_t c = 0; c < nf; ++c) {
    std::string name = DisplayTextOf(sh->GetValue(r.r0, r.c0 + c), "General");
    if (name.empty()) {
      *err = TR("The PivotTable field name is not valid: every column of the source needs a heading");
      return false;
    }
    // duplicate headings get a number (Sales, Sales2)
    std::string unique = name;
    for (int k = 2; seen.count(LowerStr(unique)); ++k) unique = name + std::to_string(k);
    seen.insert(LowerStr(unique));
    out->names.push_back(unique);
    std::string fmt;
    for (uint32_t row = r.r0 + 1; row <= r.r1 && fmt.empty(); ++row) {
      if (sh->GetValue(row, r.c0 + c).IsNumeric()) fmt = wb.styles->NumberFormat(wb.styles->Get(sh->GetStyle(row, r.c0 + c)));
    }
    out->numfmts.push_back(fmt == "General" ? std::string() : fmt);
  }
  for (uint32_t row = r.r0 + 1; row <= r.r1; ++row) {
    std::vector<Value> rec(nf);
    for (uint32_t c = 0; c < nf; ++c) rec[c] = sh->GetValue(row, r.c0 + c);
    out->records.push_back(std::move(rec));
  }
  out->columns = nf;
  out->source_records = out->records.size();
  // calculated items: each term's records again, under the item's name, with the data values times the factor
  // (so sums, subtotals and grand totals include them, as in Excel)
  std::vector<const model::PivotCalcItem*> formula_calc;
  if (!pt.calc_items.empty()) {
    std::set<int> axis_fields, data_fields;
    for (const auto* axis : {&pt.rows, &pt.cols, &pt.pages}) {
      for (const auto& a : *axis) axis_fields.insert(a.field);
    }
    for (const auto& d : pt.data) data_fields.insert(d.field);
    std::vector<std::vector<Value>> extra;
    for (const auto& ci : pt.calc_items) {
      if (ci.field < 0 || static_cast<uint32_t>(ci.field) >= nf) continue;
      std::set<std::string> uniq;
      for (size_t k = 0; k < out->source_records; ++k) uniq.insert(PivotItemText(out->records[k][static_cast<size_t>(ci.field)]));
      std::vector<std::pair<std::string, double>> terms;
      std::string why;
      if (!ParseCalcItem(std::vector<std::string>(uniq.begin(), uniq.end()), ci.formula, &terms, &why)) {
        // any other formula (functions, products of items): worked out in each report cell from its items' values
        formula_calc.push_back(&ci);
        continue;
      }
      for (const auto& [item, coef] : terms) {
        const std::string folded = formula::FoldCase(item);  // items group text without case (AAPL and aapl are one)
        for (size_t k = 0; k < out->source_records; ++k) {
          const auto& rec = out->records[k];
          if (formula::FoldCase(PivotItemText(rec[static_cast<size_t>(ci.field)])) != folded) continue;
          std::vector<Value> copy = rec;
          copy[static_cast<size_t>(ci.field)] = Value::String(ci.name);
          for (int df : data_fields) {
            if (df < 0 || static_cast<uint32_t>(df) >= nf || axis_fields.count(df) || df == ci.field) continue;
            Value& v = copy[static_cast<size_t>(df)];
            if (v.IsNumeric()) v = Value::Number(v.AsDouble() * coef);
          }
          extra.push_back(std::move(copy));
        }
      }
    }
    for (auto& e : extra) out->records.push_back(std::move(e));
  }
  // fields made by groups: the coarser periods of a date grouped by several ("Years", "Quarters"), and the groups of
  // chosen items ("Region2"); a record holds its source field's value for each
  struct Made {
    int base;
    size_t group;
    uint32_t period;  // 0: a group of items
  };
  std::vector<Made> made;
  auto unique_name = [&](std::string name) {
    std::string u = name;
    for (int k = 2; std::any_of(out->names.begin(), out->names.end(), [&](const std::string& n) { return LowerStr(n) == LowerStr(u); }); ++k)
      u = name + std::to_string(k);
    return u;
  };
  for (size_t gi = 0; gi < pt.groups.size(); ++gi) {
    const auto& g = pt.groups[gi];
    if (g.field < 0 || static_cast<uint32_t>(g.field) >= nf) continue;
    if (g.by == model::PivotGroupBy::Items) {
      if (g.named.empty()) continue;
      made.push_back(Made{g.field, gi, 0});
      out->names.push_back(unique_name(g.name.empty() ? out->names[static_cast<size_t>(g.field)] + "2" : g.name));
      out->numfmts.emplace_back();
      continue;
    }
    if (g.periods == 0 || g.periods == FinestPeriod(g.periods)) continue;
    const uint32_t finest = FinestPeriod(g.periods);
    for (uint32_t p : kPeriodOrder) {
      if (!(g.periods & p) || p == finest) continue;
      made.push_back(Made{g.field, gi, p});
      out->names.push_back(unique_name(PeriodFieldName(p)));
      out->numfmts.emplace_back();
    }
  }
  if (!made.empty()) {
    for (auto& rec : out->records) {
      for (const Made& m : made) rec.push_back(rec[static_cast<size_t>(m.base)]);
    }
  }
  const size_t nsrc = nf + made.size();
  out->source_fields = nsrc;
  out->made_by.assign(nsrc, -1);
  for (size_t k = 0; k < made.size(); ++k) out->made_by[nf + k] = static_cast<int>(made[k].group);
  for (const auto& cf : pt.calc_fields) {
    out->names.push_back(cf.name);
    out->numfmts.emplace_back();
  }
  // a group's range: its data's smallest and largest value (Starting at / Ending at: Auto)
  auto labeler = [&](const model::PivotGroup& g, uint32_t period) {
    GroupLabeler L;
    L.g = &g;
    L.period = period;
    L.start = g.start;
    L.end = g.end;
    const size_t f = static_cast<size_t>(g.field);
    L.datefmt = f < out->numfmts.size() && formula::IsDateFormat(out->numfmts[f]) ? out->numfmts[f] : std::string();
    if (g.auto_start || g.auto_end) {
      double lo = 0, hi = 0;
      bool any = false;
      for (size_t k = 0; k < out->source_records; ++k) {
        const Value& v = out->records[k][f];
        if (!v.IsNumeric()) continue;
        lo = any ? std::min(lo, v.AsDouble()) : v.AsDouble();
        hi = any ? std::max(hi, v.AsDouble()) : v.AsDouble();
        any = true;
      }
      const bool dates = g.by != model::PivotGroupBy::Numbers || period != 0;
      if (g.auto_start) L.start = any ? (dates ? std::floor(lo) : lo) : 0;
      if (g.auto_end) L.end = any ? (dates ? std::floor(hi) : hi) : 0;
      if (dates) {
        // auto: nothing falls outside
        if (g.auto_start) L.start = 0;
        if (g.auto_end) L.end = 0;
        if (g.days > 1 && any) L.start = std::floor(lo);
      }
    }
    if (g.by == model::PivotGroupBy::Numbers && period == 0) {
      bool blanks = false;
      for (size_t k = 0; k < out->source_records; ++k) {
        const Value& v = out->records[k][f];
        if (v.IsNumeric() && std::floor(v.AsDouble()) != v.AsDouble()) L.whole_data = false;
        if (v.IsEmpty()) blanks = true;
      }
      L.blank_low = blanks && !g.auto_start;
    }
    return L;
  };
  // items of every field, sorted (grouped fields: their groups, in period / range order)
  out->items.resize(nsrc);
  out->item_of.resize(nsrc);
  out->display.resize(nsrc);
  out->item_values.resize(nsrc);
  auto by_label = [&](size_t f, const std::function<std::string(const Value&, double*)>& label) {
    std::map<std::string, double> uniq;
    std::vector<std::string> per_rec;
    per_rec.reserve(out->records.size());
    for (const auto& rec : out->records) {
      double order = 0;
      per_rec.push_back(label(rec[f], &order));
      uniq.emplace(per_rec.back(), order);
    }
    std::vector<std::pair<std::string, double>> v(uniq.begin(), uniq.end());
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::unordered_map<std::string, int> index;
    for (const auto& [t, o] : v) {
      index[t] = static_cast<int>(out->items[f].size());
      out->items[f].push_back(t);
      out->display[f].push_back(t);
      out->item_values[f].push_back(Value::String(t));
    }
    out->item_of[f].reserve(out->records.size());
    for (const auto& t : per_rec) out->item_of[f].push_back(index[t]);
  };
  std::vector<std::unordered_map<std::string, int>> key_index(nsrc);  // [field] ItemKey -> item (fields not grouped)
  for (size_t f = 0; f < nsrc; ++f) {
    if (f >= nf) {
      const Made& m = made[f - nf];
      const auto& g = pt.groups[m.group];
      if (m.period != 0) {
        const GroupLabeler L = labeler(g, m.period);
        by_label(f, [&](const Value& v, double* order) { return L.Label(v, order); });
        continue;
      }
      // a group of items: the group's name for its members (placed where its first member was), the item otherwise
      const size_t base = static_cast<size_t>(m.base);
      // (names without case, as the base field's items: the members name items as they were spelled then)
      std::unordered_map<std::string, int> base_index;
      for (size_t i = 0; i < out->items[base].size(); ++i) base_index.emplace(formula::FoldCase(out->items[base][i]), static_cast<int>(i));
      std::unordered_map<std::string, std::pair<std::string, double>> member;
      for (const auto& [name, members] : g.named) {
        double first = 1e300;
        for (const auto& it : members) {
          const auto bi = base_index.find(formula::FoldCase(it));
          if (bi != base_index.end()) first = std::min(first, static_cast<double>(bi->second));
        }
        for (const auto& it : members) member.emplace(formula::FoldCase(it), std::make_pair(name, first));
      }
      const auto& base_keys = key_index[base];
      by_label(f, [&](const Value& v, double* order) {
        // the base field's item for the value (its first spelling), else its text
        const auto ki = base_keys.find(ItemKey(v));
        const std::string t = ki != base_keys.end() ? out->items[base][static_cast<size_t>(ki->second)] : PivotItemText(v);
        const std::string ft = formula::FoldCase(t);
        const auto mi = member.find(ft);
        if (mi != member.end()) {
          *order = mi->second.second;
          return mi->second.first;
        }
        const auto bi = base_index.find(ft);
        *order = bi != base_index.end() ? bi->second : 1e300;
        return t;
      });
      continue;
    }
    const model::PivotGroup* grp = nullptr;
    for (const auto& g : pt.groups) {
      if (g.field == static_cast<int>(f) && g.by != model::PivotGroupBy::None && g.by != model::PivotGroupBy::Items) grp = &g;
    }
    if (grp) {
      const GroupLabeler L = labeler(*grp, grp->periods ? FinestPeriod(grp->periods) : 0);
      by_label(f, [&](const Value& v, double* order) { return L.Label(v, order); });
      continue;
    }
    // one item per kind and text without case (ItemKey): its name is the first record's spelling
    std::map<std::string, std::pair<std::string, Value>> uniq;  // key -> (the item's text, a value of it for the order)
    std::vector<std::string> rec_keys;
    rec_keys.reserve(out->records.size());
    for (const auto& rec : out->records) {
      rec_keys.push_back(ItemKey(rec[f]));
      uniq.emplace(rec_keys.back(), std::make_pair(PivotItemText(rec[f]), rec[f]));
    }
    struct Entry {
      std::string key, text;
      Value val;
    };
    std::vector<Entry> v;
    v.reserve(uniq.size());
    for (auto& [k, tv] : uniq) v.push_back(Entry{k, std::move(tv.first), std::move(tv.second)});
    const bool custom = pt.custom_lists;
    std::stable_sort(v.begin(), v.end(), [custom](const Entry& a, const Entry& b) { return ItemLess(a.val, b.val, custom); });
    std::unordered_map<std::string, int>& index = key_index[f];
    // calculated items come after the field's own items
    std::stable_partition(v.begin(), v.end(), [&](const Entry& e) {
      return std::none_of(pt.calc_items.begin(), pt.calc_items.end(), [&](const model::PivotCalcItem& ci) { return ci.field == static_cast<int>(f) && ci.name == e.text; });
    });
    const std::string& fmt = out->numfmts[f];
    for (const auto& [key, t, val] : v) {
      index[key] = static_cast<int>(out->items[f].size());
      out->items[f].push_back(t);
      // shown in the source's format; numbers and dates stay numbers in the report's cells
      if (val.IsNumeric()) {
        out->display[f].push_back(fmt.empty() ? t : DisplayTextOf(val, fmt));
        out->item_values[f].push_back(val);
      } else {
        out->display[f].push_back(t);
        out->item_values[f].push_back(val.IsString() ? val : Value::String(t));
      }
    }
    out->item_of[f].reserve(out->records.size());
    for (const auto& k : rec_keys) out->item_of[f].push_back(index[k]);
    // formula items: items of their own (after the others), with no records
    for (const model::PivotCalcItem* ci : formula_calc) {
      const std::string ck = ItemKey(Value::String(ci->name));
      if (ci->field != static_cast<int>(f) || index.count(ck)) continue;
      const int k = static_cast<int>(out->items[f].size());
      index[ck] = k;
      out->items[f].push_back(ci->name);
      out->display[f].push_back(ci->name);
      out->item_values[f].push_back(Value::String(ci->name));
      if (out->formula_items.size() < nsrc) out->formula_items.resize(nsrc);
      out->formula_items[f].emplace_back(k, ci->formula);
    }
  }
  return true;
}

std::string PivotDataCaption(const model::PivotTable& pt, const PivotCache& cache, size_t d) {
  const auto& df = pt.data[d];
  if (!df.caption.empty()) return df.caption;
  if (cache.IsMeasure(df.field)) return cache.names[static_cast<size_t>(df.field)];
  auto automatic = [&](const model::PivotDataField& x) {
    const std::string field = ValidField(cache, x.field) ? cache.names[static_cast<size_t>(x.field)] : std::string("?");
    return std::string(model::PivotFuncName(x.func)) + TR(" of ") + field;
  };
  // the same field twice: "Sum of Sales", "Sum of Sales2" (Excel)
  const std::string base = automatic(df);
  int n = 1;
  for (size_t j = 0; j < d; ++j) {
    if (pt.data[j].caption.empty() && automatic(pt.data[j]) == base) ++n;
  }
  return n > 1 ? base + std::to_string(n) : base;
}

PivotReport ComputePivot(const model::PivotTable& pt0, const PivotCache& cache) {
  // drop fields the source no longer has
  model::PivotTable pt = pt0;
  RemapPivotFields(&pt, cache);
  auto keep = [&](std::vector<model::PivotAxisField>& v) {
    v.erase(std::remove_if(v.begin(), v.end(), [&](const model::PivotAxisField& f) { return !ValidAxis(cache, f.field); }), v.end());
  };
  keep(pt.rows);
  keep(pt.cols);
  keep(pt.pages);
  pt.data.erase(std::remove_if(pt.data.begin(), pt.data.end(), [&](const model::PivotDataField& f) { return !ValidField(cache, f.field); }), pt.data.end());
  PivotReport rep;
  const size_t R = pt.rows.size(), C = pt.cols.size(), D = pt.data.size(), P = pt.pages.size();
  if (R == 0 && C == 0 && D == 0) return rep;  // empty: the UI shows the placeholder
  const bool vals_in_cols = D > 1 && !pt.values_on_rows, vals_in_rows = D > 1 && pt.values_on_rows;
  std::vector<size_t> recs = FilteredRecords(pt, cache);

  // ---- aggregation over every (row prefix, column prefix) ----
  std::unordered_map<std::string, std::vector<Acc>> agg;
  std::set<std::vector<int>> row_set, col_set;
  size_t naccs = 1;
  const std::vector<DataPlan> plans = PlanData(pt, cache, &naccs);
  auto aggregate = [&](const std::vector<size_t>& rr) {
    agg.clear();
    row_set.clear();
    col_set.clear();
    std::vector<int> rk(R), ck(C);
    for (size_t rec : rr) {
      for (size_t i = 0; i < R; ++i) rk[i] = cache.item_of[static_cast<size_t>(pt.rows[i].field)][rec];
      for (size_t i = 0; i < C; ++i) ck[i] = cache.item_of[static_cast<size_t>(pt.cols[i].field)][rec];
      row_set.insert(rk);
      col_set.insert(ck);
      for (size_t lr = 0; lr <= R; ++lr) {
        const std::vector<int> rp(rk.begin(), rk.begin() + static_cast<std::ptrdiff_t>(lr));
        for (size_t lc = 0; lc <= C; ++lc) {
          const std::vector<int> cp(ck.begin(), ck.begin() + static_cast<std::ptrdiff_t>(lc));
          auto& v = agg[KeyStr(rp, cp)];
          if (v.empty()) {
            v.resize(naccs);
            for (size_t d = 0; d < D; ++d) {
              if (pt.data[d].func == PivotFunc::DistinctCount && !plans[d].calc) v[d].distinct = std::make_shared<std::set<std::string>>();
            }
          }
          for (size_t d = 0; d < D; ++d) {
            if (!plans[d].calc) {
              v[d].Add(cache.records[rec][static_cast<size_t>(pt.data[d].field)]);
              continue;
            }
            v[d].Add(Value());  // counts the record
            for (size_t k = 0; k < plans[d].refs.size(); ++k) v[plans[d].offset + k].Add(cache.records[rec][static_cast<size_t>(plans[d].refs[k])]);
          }
          if (D == 0) v[0].Add(Value::Number(1));
        }
      }
    }
  };
  aggregate(recs);
  auto result = [&](const std::vector<Acc>& v, size_t d, int func) -> Value {
    if (!plans[d].calc) return v[d].Result(func >= 0 ? static_cast<PivotFunc>(func) : pt.data[d].func);
    if (v[d].records == 0) return Value();
    std::vector<double> sums;
    for (size_t k = 0; k < plans[d].refs.size(); ++k) sums.push_back(v[plans[d].offset + k].sum);
    return EvalPlan(plans[d], cache, sums);
  };
  // calculated items with a formula of their own (=East*West, =MAX(East,West)): parsed once, per field and item
  std::map<std::pair<int, int>, CalcFormula> fitems;
  for (size_t f = 0; f < cache.formula_items.size(); ++f) {
    for (const auto& [item, text] : cache.formula_items[f]) {
      fitems.emplace(std::make_pair(static_cast<int>(f), item), CompileCalcFormula(cache.items[f], cache.items[f].size(), text));
    }
  }
  auto fitem = [&](int field, int item) -> const CalcFormula* {
    if (fitems.empty()) return nullptr;
    const auto it = fitems.find({field, item});
    return it == fitems.end() ? nullptr : &it->second;
  };
  // the plain value (no Show Values As) of a data field where the row and column items hold
  std::unordered_map<std::string, Value> raw_memo;
  std::function<Value(const std::vector<int>&, const std::vector<int>&, size_t, int)> raw_fn;
  auto raw = [&](const std::vector<int>& r, const std::vector<int>& c, size_t d, int func = -1) -> Value { return raw_fn(r, c, d, func); };
  raw_fn = [&](const std::vector<int>& r, const std::vector<int>& c, size_t d, int func) -> Value {
    if (d >= D) return Value();
    if (fitems.empty()) {
      auto it = agg.find(KeyStr(r, c));
      return it == agg.end() ? Value() : result(it->second, d, func);
    }
    const std::string memo = KeyStr(r, c) + "#" + std::to_string(d) + "#" + std::to_string(func);
    if (const auto m = raw_memo.find(memo); m != raw_memo.end()) return m->second;
    Value out;
    // a formula item: its formula on the values of the items it names, in the same place
    auto eval_item = [&](const CalcFormula& cf, const std::function<Value(int)>& at) {
      if (!cf.ok) return Value::Error(core::Err::Name);
      std::vector<double> vals;
      for (int item : cf.refs) {
        const Value v = at(item);
        vals.push_back(v.IsNumeric() ? v.AsDouble() : 0.0);
      }
      CalcFieldHost host(vals);
      Value v = formula::EvaluateCell(*cf.root, host, 0, core::CellPos{0, 0});
      return v.IsEmpty() ? Value::Number(0) : v;
    };
    bool done = false;
    for (size_t L = 0; L < r.size() && !done; ++L) {
      if (const CalcFormula* cf = fitem(pt.rows[L].field, r[L])) {
        out = eval_item(*cf, [&](int item) {
          std::vector<int> k = r;
          k[L] = item;
          return raw_fn(k, c, d, func);
        });
        done = true;
      }
    }
    for (size_t L = 0; L < c.size() && !done; ++L) {
      if (const CalcFormula* cf = fitem(pt.cols[L].field, c[L])) {
        out = eval_item(*cf, [&](int item) {
          std::vector<int> k = c;
          k[L] = item;
          return raw_fn(r, k, d, func);
        });
        done = true;
      }
    }
    if (!done) {
      auto it = agg.find(KeyStr(r, c));
      if (it != agg.end()) out = result(it->second, d, func);
      // subtotals and grand totals of sums include the formula items below them (Excel)
      const PivotFunc fn = func >= 0 ? static_cast<PivotFunc>(func) : pt.data[d].func;
      if (fn == PivotFunc::Sum && !plans[d].calc) {
        double extra = 0;
        bool any = false;
        auto add_axis = [&](bool rows_axis) {
          const auto& fields = rows_axis ? pt.rows : pt.cols;
          const auto& keyset = rows_axis ? row_set : col_set;
          const std::vector<int>& mine = rows_axis ? r : c;
          for (size_t L = mine.size(); L < fields.size(); ++L) {
            std::vector<int> fis;
            for (const auto& [key, cf] : fitems) {
              if (key.first == fields[L].field) fis.push_back(key.second);
            }
            if (fis.empty()) continue;
            // every place (prefix of real items) the formula items sit under
            std::set<std::vector<int>> prefixes;
            for (const auto& k : keyset) {
              if (k.size() <= L || !std::equal(mine.begin(), mine.end(), k.begin())) continue;
              bool real = true;
              for (size_t q = mine.size(); q < L && real; ++q) real = !fitem(fields[q].field, k[q]);
              if (real) prefixes.insert(std::vector<int>(k.begin(), k.begin() + static_cast<std::ptrdiff_t>(L)));
            }
            for (const auto& pre : prefixes) {
              for (int fi : fis) {
                std::vector<int> k = pre;
                k.push_back(fi);
                const Value v = rows_axis ? raw_fn(k, c, d, func) : raw_fn(r, k, d, func);
                if (v.IsNumeric()) extra += v.AsDouble(), any = true;
              }
            }
          }
        };
        add_axis(true);
        add_axis(false);
        if (any && !out.IsError()) out = Value::Number((out.IsNumeric() ? out.AsDouble() : 0.0) + extra);
      }
    }
    raw_memo.emplace(memo, out);
    return out;
  };

  // ---- value filters (Value Filters, Top 10): items whose total passes, within their parent; outer fields first ----
  for (int axis = 0; axis < 2; ++axis) {
    const auto& fields = axis == 0 ? pt.rows : pt.cols;
    for (size_t L = 0; L < fields.size(); ++L) {
      const model::PivotFilter& vf = fields[L].value_filter;
      if (vf.type < 1 || vf.type > 14 || vf.data < 0 || static_cast<size_t>(vf.data) >= D) continue;
      const auto& keys = axis == 0 ? row_set : col_set;
      std::map<std::vector<int>, std::vector<std::pair<std::vector<int>, double>>> groups;  // parent -> (item prefix, total)
      std::set<std::vector<int>> seen;
      for (const auto& k : keys) {
        std::vector<int> pre(k.begin(), k.begin() + static_cast<std::ptrdiff_t>(L + 1));
        if (!seen.insert(pre).second) continue;
        const Value v = axis == 0 ? raw(pre, {}, static_cast<size_t>(vf.data)) : raw({}, pre, static_cast<size_t>(vf.data));
        groups[std::vector<int>(pre.begin(), pre.end() - 1)].emplace_back(pre, v.IsNumeric() ? v.AsDouble() : std::nan(""));
      }
      double a = 0, b = 0;
      ParseNum(vf.v1, &a);
      ParseNum(vf.v2, &b);
      std::set<std::vector<int>> pass;
      for (auto& [parent, list] : groups) {
        if (vf.type <= 6) {
          // Top 10: by count, percent of the total or running sum
          const bool top = vf.type % 2 == 1;
          std::vector<std::pair<std::vector<int>, double>> s;
          for (const auto& e : list) {
            if (!std::isnan(e.second)) s.push_back(e);
          }
          std::stable_sort(s.begin(), s.end(), [top](const auto& x, const auto& y) { return top ? x.second > y.second : x.second < y.second; });
          double total = 0;
          for (const auto& e : s) total += e.second;
          double run = 0;
          for (size_t i = 0; i < s.size(); ++i) {
            bool in = false;
            if (vf.type <= 2) in = static_cast<double>(i) < a;
            else if (vf.type <= 4) in = run < total * a / 100.0 || i == 0;
            else in = top ? run < a : run < a || i == 0;
            if (!in) break;
            run += s[i].second;
            pass.insert(s[i].first);
          }
          continue;
        }
        for (const auto& [pre, x] : list) {
          bool in = false;
          if (!std::isnan(x)) {
            switch (vf.type) {
              case 7: in = x == a; break;
              case 8: in = x != a; break;
              case 9: in = x > a; break;
              case 10: in = x >= a; break;
              case 11: in = x < a; break;
              case 12: in = x <= a; break;
              case 13: in = x >= std::min(a, b) && x <= std::max(a, b); break;
              case 14: in = !(x >= std::min(a, b) && x <= std::max(a, b)); break;
              default: in = true; break;
            }
          }
          if (in) pass.insert(pre);
        }
      }
      std::vector<size_t> next;
      next.reserve(recs.size());
      std::vector<int> pre(L + 1);
      for (size_t rec : recs) {
        for (size_t i = 0; i <= L; ++i) pre[i] = cache.item_of[static_cast<size_t>(fields[i].field)][rec];
        if (pass.count(pre)) next.push_back(rec);
      }
      recs = std::move(next);
      aggregate(recs);
      raw_memo.clear();
      rep.value_filtered = true;
    }
  }
  if (rep.value_filtered) rep.kept_records = recs;

  // ---- Show items with no data: every item of the field (not filtered out) ----
  auto all_items = [&](std::set<std::vector<int>>& keys, const std::vector<model::PivotAxisField>& fields) {
    for (size_t L = 0; L < fields.size(); ++L) {
      if (!fields[L].show_all_items || keys.empty()) continue;
      const auto& af = fields[L];
      const auto& items = cache.items[static_cast<size_t>(af.field)];
      std::vector<int> ok;
      for (size_t i = 0; i < items.size(); ++i) {
        if (PivotItemHidden(af, items[i])) continue;
        if (af.label_filter.type >= 15 &&
            !LabelPass(af.label_filter, cache.display[static_cast<size_t>(af.field)][i], cache.item_values[static_cast<size_t>(af.field)][i]))
          continue;
        ok.push_back(static_cast<int>(i));
      }
      std::set<std::vector<int>> more;
      for (const auto& k : keys) {
        for (int it : ok) {
          std::vector<int> k2 = k;
          k2[L] = it;
          more.insert(std::move(k2));
        }
      }
      keys.insert(more.begin(), more.end());
    }
  };
  all_items(row_set, pt.rows);
  all_items(col_set, pt.cols);
  // formula items take the places of their field's other items (keys added after the totals were worked out)
  auto with_formula_items = [&](std::set<std::vector<int>>& keys, const std::vector<model::PivotAxisField>& fields) {
    if (fitems.empty()) return;
    for (size_t L = 0; L < fields.size(); ++L) {
      std::vector<int> fis;
      for (const auto& [key, cf] : fitems) {
        if (key.first == fields[L].field && !PivotItemHidden(fields[L], cache.items[static_cast<size_t>(key.first)][static_cast<size_t>(key.second)]))
          fis.push_back(key.second);
      }
      if (fis.empty()) continue;
      std::set<std::vector<int>> more;
      for (const auto& k : keys) {
        if (k.size() <= L || fitem(fields[L].field, k[L])) continue;
        for (int fi : fis) {
          std::vector<int> k2 = k;
          k2[L] = fi;
          more.insert(std::move(k2));
        }
      }
      keys.insert(more.begin(), more.end());
    }
  };
  with_formula_items(row_set, pt.rows);
  with_formula_items(col_set, pt.cols);
  raw_memo.clear();

  // ---- order: labels (A to Z / Z to A / by hand), then by values where a field sorts by a data field ----
  auto ordered = [&](const std::set<std::vector<int>>& set, const std::vector<model::PivotAxisField>& fields, bool rows_axis) {
    std::vector<std::vector<int>> rank(fields.size());
    for (size_t L = 0; L < fields.size(); ++L) {
      const auto& af = fields[L];
      const auto& items = cache.items[static_cast<size_t>(af.field)];
      auto& rk = rank[L];
      rk.resize(items.size());
      for (size_t i = 0; i < items.size(); ++i) {
        int v = static_cast<int>(i);
        if (!af.manual_order.empty()) {
          const auto it = std::find_if(af.manual_order.begin(), af.manual_order.end(), [&](const std::string& m) { return ItemNameEq(m, items[i]); });
          v = it != af.manual_order.end() ? static_cast<int>(it - af.manual_order.begin()) : static_cast<int>(af.manual_order.size() + i);
        } else if (af.descending && af.sort_by < 0) {
          v = -v;
        }
        rk[i] = v;
      }
    }
    std::vector<std::vector<int>> keys(set.begin(), set.end());
    std::stable_sort(keys.begin(), keys.end(), [&](const std::vector<int>& a, const std::vector<int>& b) {
      for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const int x = rank[i][static_cast<size_t>(a[i])], y = rank[i][static_cast<size_t>(b[i])];
        if (x != y) return x < y;
      }
      return a.size() < b.size();
    });
    // by values: the siblings of each group ordered by their total in the chosen column (row)
    const auto& other_fields = rows_axis ? pt.cols : pt.rows;
    std::function<void(size_t, size_t, size_t)> by_value = [&](size_t lo, size_t hi, size_t level) {
      if (level >= fields.size() || hi - lo < 2) return;
      struct G {
        size_t lo, hi;
        double v;
        bool has;
      };
      std::vector<G> gs;
      for (size_t i = lo; i < hi;) {
        size_t j = i;
        while (j < hi && keys[j][level] == keys[i][level]) ++j;
        gs.push_back(G{i, j, 0, false});
        i = j;
      }
      const auto& af = fields[level];
      if (af.sort_by >= 0 && static_cast<size_t>(af.sort_by) < D && gs.size() > 1) {
        std::vector<int> other;
        for (size_t i = 0; i < af.sort_col.size() && i < other_fields.size(); ++i) {
          const auto& its = cache.items[static_cast<size_t>(other_fields[i].field)];
          const auto it = std::find(its.begin(), its.end(), af.sort_col[i]);
          if (it == its.end()) {
            other.clear();
            break;
          }
          other.push_back(static_cast<int>(it - its.begin()));
        }
        for (auto& g : gs) {
          const std::vector<int> pre(keys[g.lo].begin(), keys[g.lo].begin() + static_cast<std::ptrdiff_t>(level + 1));
          const Value v = rows_axis ? raw(pre, other, static_cast<size_t>(af.sort_by)) : raw(other, pre, static_cast<size_t>(af.sort_by));
          g.has = v.IsNumeric();
          g.v = g.has ? v.AsDouble() : 0;
        }
        const bool desc = af.descending;
        std::stable_sort(gs.begin(), gs.end(), [desc](const G& x, const G& y) {
          if (x.has != y.has) return x.has;  // no value: last
          return desc ? x.v > y.v : x.v < y.v;
        });
        std::vector<std::vector<int>> tmp;
        tmp.reserve(hi - lo);
        for (const auto& g : gs) {
          for (size_t i = g.lo; i < g.hi; ++i) tmp.push_back(std::move(keys[i]));
        }
        size_t pos = lo;
        for (auto& g : gs) {
          const size_t n = g.hi - g.lo;
          g.lo = pos;
          g.hi = pos + n;
          pos += n;
        }
        for (size_t i = 0; i < tmp.size(); ++i) keys[lo + i] = std::move(tmp[i]);
      }
      for (const auto& g : gs) by_value(g.lo, g.hi, level + 1);
    };
    by_value(0, keys.size(), 0);
    return keys;
  };
  const std::vector<std::vector<int>> rkeys = ordered(row_set, pt.rows, true);
  const std::vector<std::vector<int>> ckeys = ordered(col_set, pt.cols, false);
  auto collapsed = [&](const std::vector<model::PivotAxisField>& fields, size_t level, int item) {
    return level + 1 < fields.size() && PivotItemCollapsed(fields[level], cache.items[static_cast<size_t>(fields[level].field)][static_cast<size_t>(item)]);
  };

  // ---- columns ----
  struct ColEntry {
    std::vector<int> key;
    int data = -1;
    int kind = 0;  // 0 item, 1 subtotal, 2 grand, 6 collapsed item
  };
  std::vector<ColEntry> cols;
  auto add_cols = [&](const std::vector<int>& key, int kind) {
    if (vals_in_cols) {
      for (size_t d = 0; d < D; ++d) cols.push_back(ColEntry{key, static_cast<int>(d), kind});
    } else {
      cols.push_back(ColEntry{key, D ? 0 : -1, kind});
    }
  };
  // depth-first over the sorted column keys
  std::function<void(size_t, size_t, size_t)> walk_cols = [&](size_t lo, size_t hi, size_t level) {
    size_t i = lo;
    while (i < hi) {
      size_t j = i;
      while (j < hi && ckeys[j][level] == ckeys[i][level]) ++j;
      const std::vector<int> prefix(ckeys[i].begin(), ckeys[i].begin() + static_cast<std::ptrdiff_t>(level + 1));
      if (level + 1 == C) {
        add_cols(prefix, 0);
      } else if (collapsed(pt.cols, level, ckeys[i][level])) {
        add_cols(prefix, 6);
      } else {
        walk_cols(i, j, level + 1);
        if (pt.cols[level].subtotal && pt.subtotals != 0) add_cols(prefix, 1);
      }
      i = j;
    }
  };
  if (C == 0) {
    add_cols({}, 0);
  } else {
    walk_cols(0, ckeys.size(), 0);
    if (pt.col_grand) add_cols({}, 2);
  }

  // ---- rows ----
  struct RowEntry {
    std::vector<int> key;
    int data = -1;   // vals_in_rows: the data field of this row
    int kind = 0;    // 0 item (leaf), 1 subtotal (bottom), 2 grand, 3 group header (values when show_values), 4 data caption row
                     // (vals_in_rows), 5 blank line, 6 collapsed item
    size_t level = 0;
    bool show_values = true;
    int func = -1;   // custom subtotals: the function of this subtotal row
  };
  std::vector<RowEntry> rows;
  const int layout = std::clamp(pt.layout, 0, 2);
  // each row field's form (Field Settings > Layout & Print; else the report's) and the label column it uses: a field in
  // compact form puts the next field's labels in its own column
  std::vector<int> form(R);
  std::vector<uint32_t> lcol(R);
  for (size_t L = 0; L < R; ++L) {
    form[L] = pt.rows[L].form >= 0 ? std::clamp(pt.rows[L].form, 0, 2) : layout;
    lcol[L] = L == 0 ? 0 : lcol[L - 1] + (form[L - 1] == 0 ? 0 : 1);
  }
  rep.row_field_col = lcol;
  std::vector<uint32_t> breaks;  // rows (index into rows) where a new page starts
  auto add_rows = [&](const std::vector<int>& key, int kind, size_t level, bool values, int func = -1) {
    if (vals_in_rows && values) {
      if (kind != 2 || R > 0) rows.push_back(RowEntry{key, -1, kind == 0 ? 0 : kind, level, false, func});
      for (size_t d = 0; d < D; ++d) rows.push_back(RowEntry{key, static_cast<int>(d), 4, level + 1, true, func});
    } else {
      rows.push_back(RowEntry{key, vals_in_rows ? -1 : 0, kind, level, values, func});
    }
  };
  std::function<void(size_t, size_t, size_t)> walk_rows = [&](size_t lo, size_t hi, size_t level) {
    size_t i = lo;
    while (i < hi) {
      size_t j = i;
      while (j < hi && rkeys[j][level] == rkeys[i][level]) ++j;
      const std::vector<int> prefix(rkeys[i].begin(), rkeys[i].begin() + static_cast<std::ptrdiff_t>(level + 1));
      const auto& af = pt.rows[level];
      if (level + 1 == R) {
        add_rows(prefix, 0, level, true);
      } else if (collapsed(pt.rows, level, rkeys[i][level])) {
        add_rows(prefix, 6, level, true);  // one row with the item's totals
      } else {
        const bool sub = af.subtotal && pt.subtotals != 0;
        const uint32_t custom = sub ? (af.subtotal_funcs & ((1u << model::kPivotFuncs) - 1)) : 0;
        const bool at_top = af.sub_top >= 0 ? af.sub_top == 1 : pt.subtotals == 1;
        const bool top = sub && !custom && at_top && form[level] != 2;
        if (form[level] != 2) add_rows(prefix, 3, level, top);
        walk_rows(i, j, level + 1);
        if (custom) {
          for (int f = 0; f < model::kPivotFuncs; ++f) {
            if (custom & (1u << f)) add_rows(prefix, 1, level, true, f);
          }
        } else if (sub && !top) {
          add_rows(prefix, 1, level, true);
        }
      }
      if (af.blank_line) rows.push_back(RowEntry{prefix, -1, 5, level, false, -1});
      if (af.page_break) breaks.push_back(static_cast<uint32_t>(rows.size()));
      i = j;
    }
  };
  if (R == 0) {
    add_rows({}, 0, 0, true);
  } else {
    walk_rows(0, rkeys.size(), 0);
    while (!breaks.empty() && breaks.back() >= rows.size()) breaks.pop_back();  // none after the last item
    if (pt.row_grand) add_rows({}, 2, 0, true);
  }

  // ---- Show Values As: siblings of a field's items in the order shown ----
  std::map<std::pair<size_t, std::vector<int>>, std::vector<int>> row_sibs, col_sibs;
  // each item once, in the order shown: a find over the list so far took ten billion steps for 100,000 distinct items
  auto build_sibs = [](const std::vector<std::vector<int>>& keys, std::map<std::pair<size_t, std::vector<int>>, std::vector<int>>& out) {
    std::map<std::pair<size_t, std::vector<int>>, std::unordered_set<int>> seen;
    for (const auto& k : keys) {
      for (size_t L = 0; L < k.size(); ++L) {
        std::pair<size_t, std::vector<int>> key{L, std::vector<int>(k.begin(), k.begin() + static_cast<std::ptrdiff_t>(L))};
        auto& v = out[key];
        if (v.empty() || v.back() != k[L]) {
          if (seen[std::move(key)].insert(k[L]).second) v.push_back(k[L]);
        }
      }
    }
  };
  bool any_show = false;
  for (const auto& df : pt.data) any_show = any_show || df.show_as != PivotShowAs::Normal;
  if (any_show) {
    build_sibs(rkeys, row_sibs);
    build_sibs(ckeys, col_sibs);
  }
  // what a sibling group gives every cell in it, made once a group (and the other axis' key, and data field): each cell
  // walked all its siblings, and Rank over 100,000 distinct items took ten billion steps
  struct SibStats {
    std::unordered_map<int, size_t> pos;  // the item's place among its siblings
    std::vector<double> sorted;           // the numeric values, sorted (Rank)
    std::vector<double> run;              // the running sum in sibling order, numbers and 0 for the rest
  };
  std::map<std::tuple<bool, size_t, std::vector<int>, std::vector<int>, size_t>, SibStats> sib_stats;
  std::map<std::pair<int, std::string>, int> base_items;  // (field, lowered name) -> its item
  auto shown = [&](const std::vector<int>& r, const std::vector<int>& c, size_t d, int func) -> Value {
    Value v = raw(r, c, d, func);
    const auto& df = pt.data[d];
    const PivotShowAs k = df.show_as;
    if (func >= 0 || k == PivotShowAs::Normal) return v;
    // a cell of the report without a value (no records there, or only blanks) counts as 0 (Excel: 0% of the total,
    // a running total carries on, a difference from the base item); errors show as they are
    const bool none = v.IsEmpty();
    if (!none && !v.IsNumber()) return v;
    const double x = none ? 0.0 : v.AsDouble();
    auto ratio = [](double a, const Value& base) -> Value {
      if (base.IsError()) return base;
      if (!base.IsNumber()) return Value::Error(core::Err::NA);
      if (base.AsDouble() == 0) return Value::Error(core::Err::Div0);
      return Value::Number(a / base.AsDouble());
    };
    switch (k) {
      case PivotShowAs::PercentOfGrandTotal: return ratio(x, raw({}, {}, d));
      case PivotShowAs::PercentOfColumnTotal: return ratio(x, raw({}, c, d));
      case PivotShowAs::PercentOfRowTotal: return ratio(x, raw(r, {}, d));
      case PivotShowAs::Index: {
        const Value rt = raw(r, {}, d), ct = raw({}, c, d), g = raw({}, {}, d);
        for (const Value* e : {&rt, &ct, &g}) {
          if (e->IsError()) return *e;
        }
        if (!rt.IsNumber() || !ct.IsNumber() || !g.IsNumber()) return Value::Error(core::Err::NA);
        if (rt.AsDouble() == 0 || ct.AsDouble() == 0) return Value::Error(core::Err::Div0);
        return Value::Number(x * g.AsDouble() / (rt.AsDouble() * ct.AsDouble()));
      }
      case PivotShowAs::PercentOfParentRow:
        if (r.empty()) return Value::Number(1);
        return ratio(x, raw(std::vector<int>(r.begin(), r.end() - 1), c, d));
      case PivotShowAs::PercentOfParentColumn:
        if (c.empty()) return Value::Number(1);
        return ratio(x, raw(r, std::vector<int>(c.begin(), c.end() - 1), d));
      default: break;
    }
    // the base field: on rows or on columns (-1: the innermost row field)
    int bl = -1;
    bool brows = true;
    if (df.base_field < 0) {
      if (R > 0) bl = static_cast<int>(R) - 1;
      else if (C > 0) bl = static_cast<int>(C) - 1, brows = false;
    } else {
      for (size_t L = 0; L < R; ++L) {
        if (pt.rows[L].field == df.base_field) bl = static_cast<int>(L), brows = true;
      }
      for (size_t L = 0; L < C && bl < 0; ++L) {
        if (pt.cols[L].field == df.base_field) bl = static_cast<int>(L), brows = false;
      }
    }
    if (bl < 0) return Value::Error(core::Err::NA);
    const std::vector<int>& ak = brows ? r : c;
    if (ak.size() <= static_cast<size_t>(bl)) return Value();  // totals above the base field: blank
    const size_t L = static_cast<size_t>(bl);
    auto at = [&](int item) {
      std::vector<int> k2 = ak;
      k2[L] = item;
      return brows ? raw(k2, c, d) : raw(r, k2, d);
    };
    const auto& sibs_map = brows ? row_sibs : col_sibs;
    const auto si = sibs_map.find({L, std::vector<int>(ak.begin(), ak.begin() + bl)});
    static const std::vector<int> no_sibs;
    const std::vector<int>& sibs = si != sibs_map.end() ? si->second : no_sibs;
    // the group: the cell's key with the base level left out (at() keeps the levels around it), the other axis' key
    std::vector<int> group = ak;
    group[L] = -1;
    auto [st_it, fresh] = sib_stats.try_emplace({brows, L, std::move(group), brows ? c : r, d});
    SibStats& st = st_it->second;
    if (fresh) {
      double sum = 0;
      for (size_t i = 0; i < sibs.size(); ++i) {
        st.pos.emplace(sibs[i], i);
        const Value y = at(sibs[i]);
        if (y.IsNumeric()) st.sorted.push_back(y.AsDouble());
        sum += y.IsNumeric() ? y.AsDouble() : 0;
        st.run.push_back(sum);
      }
      std::sort(st.sorted.begin(), st.sorted.end());
    }
    const auto pi = st.pos.find(ak[L]);
    const auto pos = pi != st.pos.end() ? sibs.begin() + static_cast<std::ptrdiff_t>(pi->second) : sibs.end();
    switch (k) {
      case PivotShowAs::PercentOfParent: {
        const std::vector<int> parent(ak.begin(), ak.begin() + bl + 1);
        return ratio(x, brows ? raw(parent, c, d) : raw(r, parent, d));
      }
      case PivotShowAs::RunningTotal:
      case PivotShowAs::PercentRunningTotal: {
        const double total = st.run.empty() ? 0.0 : st.run.back();
        const double run = pi != st.pos.end() ? st.run[pi->second] : total;  // an item not among them: all of them
        if (k == PivotShowAs::RunningTotal) return Value::Number(run);
        return total == 0 ? Value::Error(core::Err::Div0) : Value::Number(run / total);
      }
      case PivotShowAs::RankAscending:
      case PivotShowAs::RankDescending: {
        if (none) return v;
        // 1 + the siblings with a smaller (larger) number; the cell's own value is never strictly either
        const auto& sv = st.sorted;
        const size_t below = static_cast<size_t>(std::lower_bound(sv.begin(), sv.end(), x) - sv.begin());
        const size_t above = static_cast<size_t>(sv.end() - std::upper_bound(sv.begin(), sv.end(), x));
        return Value::Number(1.0 + static_cast<double>(k == PivotShowAs::RankAscending ? below : above));
      }
      case PivotShowAs::PercentOf:
      case PivotShowAs::DifferenceFrom:
      case PivotShowAs::PercentDifferenceFrom: {
        int bi = ak[L];
        const std::string& b = df.base_item;
        const std::string lb = LowerStr(b);
        if (lb == "(previous)" || lb == "(next)") {
          if (pos != sibs.end()) {
            if (lb == "(previous)" && pos != sibs.begin()) bi = *(pos - 1);
            if (lb == "(next)" && pos + 1 != sibs.end()) bi = *(pos + 1);
          }
        } else {
          const int fld = brows ? pt.rows[L].field : pt.cols[L].field;
          auto [bit, bnew] = base_items.try_emplace({fld, lb}, -1);
          if (bnew) {  // found once, not at every cell
            const auto& its = cache.items[static_cast<size_t>(fld)];
            for (size_t i = 0; i < its.size() && bit->second < 0; ++i) {
              if (LowerStr(its[i]) == lb || LowerStr(cache.display[static_cast<size_t>(fld)][i]) == lb) bit->second = static_cast<int>(i);
            }
          }
          const int found = bit->second;
          if (found < 0) return Value::Error(core::Err::NA);
          bi = found;
        }
        if (bi == ak[L]) return k == PivotShowAs::PercentOf ? Value::Number(1) : Value();
        const Value base = at(bi);
        if (k == PivotShowAs::PercentOf) return ratio(x, base);
        if (base.IsError()) return base;
        if (base.IsEmpty()) return Value();  // the base item has no value here: blank (Excel)
        if (!base.IsNumber()) return Value::Error(core::Err::NA);
        if (k == PivotShowAs::DifferenceFrom) return Value::Number(x - base.AsDouble());
        if (none) return Value::Error(core::Err::Null);  // % Difference From of a cell without a value: #NULL! (Excel)
        return base.AsDouble() == 0 ? Value::Error(core::Err::Div0) : Value::Number((x - base.AsDouble()) / base.AsDouble());
      }
      default: return v;
    }
  };

  // ---- geometry ----
  const size_t col_levels = C + (vals_in_cols ? 1 : 0);
  rep.label_cols = R == 0 ? (vals_in_rows ? 1u : 0u) : lcol[R - 1] + 1;
  rep.page_rows = P ? static_cast<uint32_t>(P + 1) : 0;
  rep.header_rows = C > 0 ? static_cast<uint32_t>(1 + col_levels) : 1;
  rep.cols = std::max<uint32_t>(rep.label_cols + static_cast<uint32_t>(cols.size()), P ? 2u : 1u);
  rep.rows = rep.page_rows + rep.header_rows + static_cast<uint32_t>(rows.size());
  rep.cells.assign(static_cast<size_t>(rep.rows) * rep.cols, PivotCell{});
  rep.row_filter.assign(rep.rows, {});
  rep.col_filter.assign(rep.cols, {});
  rep.row_kind.assign(rep.rows, -1);
  rep.col_kind.assign(rep.cols, -1);
  rep.col_data.assign(rep.cols, -1);
  rep.row_data.assign(rep.rows, -1);
  rep.row_ids.assign(rep.rows, {});
  rep.col_ids.assign(rep.cols, {});
  auto ident = [&](const std::vector<model::PivotAxisField>& fields, const std::vector<int>& key) {
    std::string s;
    for (size_t i = 0; i < key.size() && i < fields.size(); ++i) {
      const size_t f = static_cast<size_t>(fields[i].field);
      s += cache.names[f] + "=" + cache.items[f][static_cast<size_t>(key[i])] + ";";
    }
    return s;
  };
  for (uint32_t r = 0; r < rep.page_rows; ++r) rep.row_ids[r] = "P" + std::to_string(r);
  for (uint32_t r = 0; r < rep.header_rows; ++r) rep.row_ids[rep.page_rows + r] = "H" + std::to_string(r);
  for (uint32_t c = 0; c < rep.label_cols; ++c) rep.col_ids[c] = "L" + std::to_string(c);
  for (size_t ci = 0; ci < cols.size(); ++ci) {
    const uint32_t c = rep.label_cols + static_cast<uint32_t>(ci);
    rep.col_kind[c] = cols[ci].kind;
    rep.col_data[c] = vals_in_rows ? -1 : cols[ci].data;
    auto& f = rep.col_filter[c];
    for (size_t i = 0; i < cols[ci].key.size(); ++i) f.emplace_back(pt.cols[i].field, cols[ci].key[i]);
    rep.col_ids[c] = std::to_string(cols[ci].kind) + "|" + std::to_string(rep.col_data[c]) + "|" + ident(pt.cols, cols[ci].key);
  }
  auto put = [&](uint32_t r, uint32_t c, PivotCell::Kind kind, Value v, bool bold = false, uint8_t indent = 0) {
    if (r >= rep.rows || c >= rep.cols) return;
    PivotCell& cell = rep.at(r, c);
    cell.kind = kind;
    cell.v = std::move(v);
    cell.bold = cell.bold || bold;
    cell.indent = indent;
  };
  auto text = [](const std::string& s) { return Value::String(s); };
  auto item_text = [&](const std::vector<model::PivotAxisField>& f, size_t level, int item) {
    const std::string& id_text = cache.items[static_cast<size_t>(f[level].field)][static_cast<size_t>(item)];
    for (const auto& [from, to] : f[level].item_captions) {
      if (ItemNameEq(from, id_text)) return to;
    }
    return cache.display[static_cast<size_t>(f[level].field)][static_cast<size_t>(item)];
  };
  // an item's label cell: its value (a number or date stays one, in the source's format)
  auto put_item = [&](uint32_t r, uint32_t c, PivotCell::Kind kind, const std::vector<model::PivotAxisField>& f, size_t level, int item, bool bold = false,
                      uint8_t indent = 0) {
    const size_t fld = static_cast<size_t>(f[level].field);
    const std::string& id_text = cache.items[fld][static_cast<size_t>(item)];
    for (const auto& [from, to] : f[level].item_captions) {
      if (ItemNameEq(from, id_text)) {
        put(r, c, kind, Value::String(to), bold, indent);  // renamed by typing over it
        return;
      }
    }
    const Value& v = cache.item_values[fld][static_cast<size_t>(item)];
    put(r, c, kind, v, bold, indent);
    if (r < rep.rows && c < rep.cols && v.IsNumeric() && fld < cache.numfmts.size()) rep.at(r, c).numfmt = cache.numfmts[fld];
  };
  auto caption = [&](int field) { return PivotFieldCaption(pt, cache, field); };
  const bool captions = pt.field_captions;
  // filters
  for (size_t i = 0; i < P; ++i) {
    const auto& f = pt.pages[i];
    put(static_cast<uint32_t>(i), 0, PivotCell::PageName, text(caption(f.field)), true);
    std::string shown_item = TR("(All)");
    if (!f.page_item.empty() && !f.multi_page) shown_item = PivotItemDisplay(cache, f.field, f.page_item);
    else if (!f.hidden.empty()) shown_item = TR("(Multiple Items)");
    put(static_cast<uint32_t>(i), 1, PivotCell::PageValue, text(shown_item));
  }
  // column header band
  const uint32_t h0 = rep.page_rows;
  for (uint32_t r = h0; r < h0 + rep.header_rows; ++r) {
    for (uint32_t c = 0; c < rep.cols; ++c) rep.at(r, c).header_row = true;
  }
  const uint32_t last_header = h0 + rep.header_rows - 1;
  if (C > 0) {
    put(h0, 0, PivotCell::Header, D == 1 ? text(PivotDataCaption(pt, cache, 0)) : Value(), true);
    // compact form: "Column Labels"; outline / tabular: the column field's name (Excel)
    if (captions) put(h0, rep.label_cols, PivotCell::Header, text(layout == 0 ? std::string(TR("Column Labels")) : caption(pt.cols[0].field)), true);
  }
  if (rep.label_cols > 0) {
    if (R == 0) {
      const std::string h = vals_in_rows ? TR("Values") : "";
      put(last_header, 0, PivotCell::Header, h.empty() ? Value() : text(h), true);
    } else if (captions) {
      // each label column: "Row Labels" over a compact column, else the caption of its (first) field
      for (size_t i = 0; i < R; ++i) {
        if (i > 0 && lcol[i] == lcol[i - 1]) continue;
        put(last_header, lcol[i], PivotCell::Header, text(form[i] == 0 ? std::string(TR("Row Labels")) : caption(pt.rows[i].field)), true);
      }
    }
  }
  // a column holding several fields (compact form) indents them by level; the others use the +/- room only
  std::vector<int> col_fields(rep.label_cols, 0);
  for (size_t L = 0; L < R; ++L) ++col_fields[lcol[L]];
  auto depth_in_col = [&](size_t level) {
    int n = 0;
    for (size_t q = 0; q < level && q < R; ++q) n += lcol[q] == lcol[std::min(level, R - 1)] ? 1 : 0;
    return n;
  };
  for (size_t ci = 0; ci < cols.size(); ++ci) {
    const ColEntry& e = cols[ci];
    const uint32_t c = rep.label_cols + static_cast<uint32_t>(ci);
    if (C == 0) {
      // no column fields: one header row with the data captions (or "Total")
      put(last_header, c, PivotCell::Header, e.data >= 0 ? text(PivotDataCaption(pt, cache, static_cast<size_t>(e.data))) : Value(), true);
      continue;
    }
    for (size_t lvl = 0; lvl < col_levels; ++lvl) {
      const uint32_t r = h0 + 1 + static_cast<uint32_t>(lvl);
      if (e.kind == 2) {
        if (lvl == 0) put(r, c, PivotCell::ColLabel, text(vals_in_cols ? TR("Total ") + PivotDataCaption(pt, cache, static_cast<size_t>(e.data)) : (pt.grand_name.empty() ? std::string(TR("Grand Total")) : pt.grand_name)), true);
        continue;
      }
      if (e.kind == 1) {
        if (lvl + 1 == e.key.size()) {
          const std::string label = item_text(pt.cols, lvl, e.key[lvl]) + (vals_in_cols ? " " + PivotDataCaption(pt, cache, static_cast<size_t>(e.data)) : std::string(TR(" Total")));
          put(r, c, PivotCell::ColLabel, text(label), true);
        }
        continue;
      }
      if (lvl < C) {
        if (lvl >= e.key.size()) continue;
        // an item only where its group starts
        const ColEntry* prev = ci > 0 ? &cols[ci - 1] : nullptr;
        const bool first = !prev || (prev->kind != 0 && prev->kind != 6) || prev->key.size() <= lvl ||
                           !std::equal(e.key.begin(), e.key.begin() + static_cast<std::ptrdiff_t>(lvl + 1), prev->key.begin());
        if (first) {
          const bool btn = lvl + 1 < C && pt.show_buttons;
          put_item(r, c, PivotCell::ColLabel, pt.cols, lvl, e.key[lvl], false, btn ? 1 : 0);
          if (lvl + 1 < C) rep.toggles.push_back(PivotReport::Toggle{r, c, false, pt.cols[lvl].field, e.key[lvl], e.kind == 6 && lvl + 1 == e.key.size()});
        }
      } else if (e.data >= 0) {
        put(r, c, PivotCell::ColLabel, text(PivotDataCaption(pt, cache, static_cast<size_t>(e.data))));
      }
    }
  }
  for (uint32_t b : breaks) {
    if (b > 0) rep.breaks.push_back(h0 + rep.header_rows + b);
  }
  // rows
  const uint8_t indent_unit = static_cast<uint8_t>(std::clamp(pt.indent, 0, 15));
  // the +/- buttons take an indent's room left of the outer items' labels (compact form: of every label)
  const uint8_t btn = pt.show_buttons && R > 1 ? 1 : 0;
  for (size_t ri = 0; ri < rows.size(); ++ri) {
    const RowEntry& e = rows[ri];
    const uint32_t r = h0 + rep.header_rows + static_cast<uint32_t>(ri);
    for (size_t i = 0; i < e.key.size(); ++i) rep.row_filter[r].emplace_back(pt.rows[i].field, e.key[i]);
    rep.row_kind[r] = e.kind;
    rep.row_data[r] = e.data;
    rep.row_ids[r] = std::to_string(e.kind) + "|" + std::to_string(e.data) + "|" + std::to_string(e.func) + "|" + ident(pt.rows, e.key);
    const bool compact_row = R > 0 ? form[std::min(e.level, R - 1)] == 0 : layout == 0;
    const bool bold = e.kind == 1 || e.kind == 2 || ((e.kind == 3 || e.kind == 6) && compact_row && pt.style_row_headers);
    // label
    if (rep.label_cols > 0 && e.kind != 5) {
      uint32_t lc = 0;
      uint8_t indent = 0;
      if (e.kind == 2) {
        put(r, 0, PivotCell::Grand, text(pt.grand_name.empty() ? std::string(TR("Grand Total")) : pt.grand_name), bold);
      } else if (e.kind == 4) {
        if (R == 0) {
          indent = static_cast<uint8_t>(layout == 0 ? e.level * indent_unit + btn : 0);
          lc = 0;
        } else {
          // the data captions under an item: below it in its column (compact: one level further in)
          const size_t lv = std::min(e.level, R - 1);
          lc = lcol[lv];
          indent = static_cast<uint8_t>(form[lv] == 0 ? (depth_in_col(lv) + (e.level >= R ? 1 : 0)) * indent_unit + btn : 0);
        }
        put(r, lc, PivotCell::RowLabel, text(PivotDataCaption(pt, cache, static_cast<size_t>(e.data))), bold, indent);
      } else if (e.kind == 1) {
        const std::string fn = e.func >= 0 ? std::string(" ") + model::PivotFuncName(static_cast<PivotFunc>(e.func)) : std::string(TR(" Total"));
        lc = lcol[e.level];
        indent = static_cast<uint8_t>(col_fields[lc] > 1 ? depth_in_col(e.level) * indent_unit + btn : 0);
        put(r, lc, PivotCell::Subtotal, text(item_text(pt.rows, e.level, e.key[e.level]) + fn), bold, indent);
      } else if (R > 0) {
        const bool toggle = (e.kind == 3 || e.kind == 6) && e.level + 1 < R;
        lc = lcol[e.level];
        indent = static_cast<uint8_t>(col_fields[lc] > 1 ? depth_in_col(e.level) * indent_unit + btn : (toggle ? btn : 0));
        put_item(r, lc, PivotCell::RowLabel, pt.rows, e.level, e.key[e.level], bold, indent);
        if (toggle) rep.toggles.push_back(PivotReport::Toggle{r, lc, true, pt.rows[e.level].field, e.key[e.level], e.kind == 6});
        if (e.kind == 0 || e.kind == 6 || e.kind == 3) {
          // outer items in other columns: a tabular field's on the first row of its group; Repeat Item Labels: on every row
          for (size_t lvl = 0; lvl < e.level; ++lvl) {
            if (lcol[lvl] == lc) continue;
            const RowEntry* prev = ri > 0 ? &rows[ri - 1] : nullptr;
            const bool first = !prev || prev->kind == 1 || prev->kind == 5 || prev->kind == 2 || prev->key.size() <= lvl ||
                               !std::equal(e.key.begin(), e.key.begin() + static_cast<std::ptrdiff_t>(lvl + 1), prev->key.begin());
            const bool tab = form[lvl] == 2 && first;
            const bool want = (pt.rows[lvl].repeat_labels && form[lvl] != 0) || tab;
            if (!want) continue;
            // a later field in the same column shows its own label there
            bool taken = false;
            for (size_t q = lvl + 1; q < e.level && !taken; ++q) taken = lcol[q] == lcol[lvl];
            if (taken) continue;
            put_item(r, lcol[lvl], PivotCell::RowLabel, pt.rows, lvl, e.key[lvl], false, tab ? btn : 0);
            if (tab) rep.toggles.push_back(PivotReport::Toggle{r, lcol[lvl], true, pt.rows[lvl].field, e.key[lvl], false});
          }
        }
      }
    }
    if (e.kind == 2) {
      for (uint32_t c = 0; c < rep.cols; ++c) rep.at(r, c).grand_row = true;
    }
    if (!e.show_values) continue;
    for (size_t ci = 0; ci < cols.size(); ++ci) {
      const ColEntry& ce = cols[ci];
      const int d = vals_in_rows ? e.data : ce.data;
      if (d < 0) continue;
      Value v = shown(e.key, ce.key, static_cast<size_t>(d), e.func);
      const uint32_t c = rep.label_cols + static_cast<uint32_t>(ci);
      const auto& df = pt.data[static_cast<size_t>(d)];
      if (v.IsEmpty()) {
        // For empty cells show
        if (pt.show_null && !pt.null_string.empty()) {
          double x = 0;
          put(r, c, PivotCell::Data, ParseNum(pt.null_string, &x) ? Value::Number(x) : text(pt.null_string), bold || ce.kind != 0);
        }
        continue;
      }
      if (v.IsError() && pt.show_error) {
        double x = 0;
        v = pt.error_string.empty() ? Value() : ParseNum(pt.error_string, &x) ? Value::Number(x) : text(pt.error_string);
        if (v.IsEmpty()) continue;
      }
      put(r, c, PivotCell::Data, std::move(v), bold || ce.kind != 0);
      std::string fmt = df.number_format;
      const bool custom_func = e.func >= 0;
      if (fmt.empty() && !custom_func && PivotShowAsPercent(df.show_as)) fmt = "0.00%";
      const PivotFunc fn = custom_func ? static_cast<PivotFunc>(e.func) : df.func;
      const bool plain = custom_func || df.show_as == PivotShowAs::Normal || df.show_as == PivotShowAs::RunningTotal || df.show_as == PivotShowAs::DifferenceFrom;
      if (fmt.empty() && plain && !pt.general_values && fn != PivotFunc::Count && fn != PivotFunc::CountNums && fn != PivotFunc::DistinctCount &&
          static_cast<size_t>(df.field) < cache.numfmts.size())
        fmt = cache.numfmts[static_cast<size_t>(df.field)];
      rep.at(r, c).numfmt = fmt;
    }
  }
  return rep;
}

std::vector<size_t> PivotFilteredRecords(const model::PivotTable& pt, const PivotCache& cache) { return FilteredRecords(pt, cache); }

std::vector<size_t> PivotDetailRecords(const model::PivotTable& pt, const PivotCache& cache, const std::vector<std::pair<int, int>>& filter) {
  // value filters don't apply here: Show Details of a Top 2's grand total lists every record, the items the filter
  // left out too (Excel 16, measured), while the total itself is the two items' (GETPIVOTDATA, the report)
  std::vector<size_t> out;
  for (size_t rec : FilteredRecords(pt, cache)) {
    if (rec >= cache.source_records) continue;  // copies made for calculated items aren't source rows
    bool ok = true;
    for (const auto& [f, item] : filter) ok = ok && ValidAxis(cache, f) && cache.item_of[static_cast<size_t>(f)][rec] == item;
    if (ok) out.push_back(rec);
  }
  return out;
}

bool PivotLookup(const model::PivotTable& pt0, const PivotCache& cache, const std::string& data_field,
                 const std::vector<std::pair<std::string, std::string>>& pairs, Value* out) {
  model::PivotTable pt = pt0;
  RemapPivotFields(&pt, cache);
  if (cache.measure) {
    // Data Model names work too: "[Measures].[Total]", "[Sales].[Region]", "[Sales].[Region].&[North]"
    auto strip = [](std::string s) {
      if (s.size() > 2 && s.front() == '[' && s.back() == ']') {
        const size_t k = s.rfind("].&[") != std::string::npos ? s.rfind("].&[") + 4 : s.rfind("].[") != std::string::npos ? s.rfind("].[") + 3 : 1;
        s = s.substr(k, s.size() - k - 1);
      }
      return s;
    };
    int d = -1;
    for (size_t i = 0; i < pt.data.size() && d < 0; ++i) {
      if (LowerStr(PivotDataCaption(pt, cache, i)) == LowerStr(strip(data_field))) d = static_cast<int>(i);
    }
    if (d < 0) return false;
    std::vector<std::pair<int, int>> want;
    for (const auto& [field, item] : pairs) {
      int f = -1;
      for (size_t i = 0; i < cache.source_fields && f < 0; ++i) {
        if (LowerStr(cache.names[i]) == LowerStr(field) || (i < cache.cube_names.size() && LowerStr(cache.cube_names[i]) == LowerStr(field))) f = static_cast<int>(i);
      }
      bool on_report = false;
      for (const auto* v : {&pt.rows, &pt.cols, &pt.pages}) {
        for (const auto& af : *v) on_report = on_report || af.field == f;
      }
      if (f < 0 || !on_report) return false;
      int k = -1;
      const auto& items = cache.items[static_cast<size_t>(f)];
      for (size_t j = 0; j < items.size() && k < 0; ++j) {
        if (LowerStr(items[j]) == LowerStr(strip(item))) k = static_cast<int>(j);
      }
      if (k < 0) return false;
      want.emplace_back(f, k);
    }
    *out = cache.measure(pt, cache, static_cast<size_t>(d), want);
    return !out->IsEmpty();
  }
  // the data field: by caption ("Sum of Sales") or by source field name ("Sales")
  int d = -1;
  for (size_t i = 0; i < pt.data.size() && d < 0; ++i) {
    if (!ValidField(cache, pt.data[i].field)) continue;
    if (LowerStr(PivotDataCaption(pt, cache, i)) == LowerStr(data_field) || LowerStr(cache.names[static_cast<size_t>(pt.data[i].field)]) == LowerStr(data_field))
      d = static_cast<int>(i);
  }
  if (d < 0) return false;
  // the fields named in the pairs must be on the report (rows, columns or filters); items match as shown or as General
  std::vector<std::pair<int, std::vector<char>>> want;
  for (const auto& [field, item] : pairs) {
    int f = -1;
    for (size_t i = 0; i < cache.names.size(); ++i) {
      if (LowerStr(cache.names[i]) == LowerStr(field) || LowerStr(PivotFieldCaption(pt, cache, static_cast<int>(i))) == LowerStr(field)) f = static_cast<int>(i);
    }
    bool on_report = false;
    for (const auto* v : {&pt.rows, &pt.cols, &pt.pages}) {
      for (const auto& af : *v) on_report = on_report || af.field == f;
    }
    if (f < 0 || !on_report || !ValidAxis(cache, f)) return false;
    const std::string li = LowerStr(item);
    std::vector<char> match(cache.items[static_cast<size_t>(f)].size(), 0);
    for (size_t j = 0; j < match.size(); ++j) {
      match[j] = LowerStr(cache.items[static_cast<size_t>(f)][j]) == li || LowerStr(cache.display[static_cast<size_t>(f)][j]) == li ? 1 : 0;
    }
    want.emplace_back(f, std::move(match));
  }
  size_t naccs = 1;
  const std::vector<DataPlan> plans = PlanData(pt, cache, &naccs);
  const DataPlan& plan = plans[static_cast<size_t>(d)];
  std::vector<Acc> acc(naccs);
  if (pt.data[static_cast<size_t>(d)].func == PivotFunc::DistinctCount && !plan.calc) acc[static_cast<size_t>(d)].distinct = std::make_shared<std::set<std::string>>();
  std::vector<char> shown(want.size(), 0);  // each named item has data in the report (under its filters)
  // a value filter (Top 10, Value Filters) keeps the items whose totals pass: what the report shows, worked out by
  // the report itself (GETPIVOTDATA's grand total under a Top 2 summed every item; gaps_fuzz seed 20003)
  bool value_filters = false;
  for (const auto* v : {&pt.rows, &pt.cols}) {
    for (const auto& af : *v) value_filters = value_filters || (af.value_filter.type >= 1 && af.value_filter.type <= 14);
  }
  std::vector<size_t> records;
  if (value_filters) {
    PivotReport rep = ComputePivot(pt0, cache);
    records = rep.value_filtered ? std::move(rep.kept_records) : FilteredRecords(pt, cache);
  } else {
    records = FilteredRecords(pt, cache);
  }
  for (size_t rec : records) {
    bool ok = true;
    for (size_t i = 0; i < want.size(); ++i) {
      const auto& [f, match] = want[i];
      const bool hit = match[static_cast<size_t>(cache.item_of[static_cast<size_t>(f)][rec])] != 0;
      if (hit) shown[i] = 1;
      ok = ok && hit;
    }
    if (!ok) continue;
    if (!plan.calc) {
      acc[static_cast<size_t>(d)].Add(cache.records[rec][static_cast<size_t>(pt.data[static_cast<size_t>(d)].field)]);
    } else {
      acc[static_cast<size_t>(d)].Add(Value());
      for (size_t k = 0; k < plan.refs.size(); ++k) acc[plan.offset + k].Add(cache.records[rec][static_cast<size_t>(plan.refs[k])]);
    }
  }
  // an intersection without data (or with blanks only) of items that are all on the report is 0, as in Excel; an item
  // that isn't there (unknown, hidden, filtered out) is #REF!
  const bool all_shown = std::all_of(shown.begin(), shown.end(), [](char c) { return c != 0; });
  if (acc[static_cast<size_t>(d)].records == 0) {
    if (!all_shown) return false;
    *out = Value::Number(0);
    return true;
  }
  if (!plan.calc) {
    *out = acc[static_cast<size_t>(d)].Result(pt.data[static_cast<size_t>(d)].func);
    if (out->IsEmpty()) *out = Value::Number(0);
  } else {
    std::vector<double> sums;
    for (size_t k = 0; k < plan.refs.size(); ++k) sums.push_back(acc[plan.offset + k].sum);
    *out = EvalPlan(plan, cache, sums);
  }
  return true;
}

// ---- PivotTable styles ----

namespace {
uint32_t Mix(uint32_t c, uint32_t with, double t) {
  auto ch = [&](int shift) {
    const double a = (c >> shift) & 0xFF, b = (with >> shift) & 0xFF;
    return static_cast<uint32_t>(std::lround(a + (b - a) * t)) & 0xFF;
  };
  return 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
uint32_t Tint(uint32_t c, double t) { return Mix(c, 0xFFFFFFFF, t); }
uint32_t Shade(uint32_t c, double t) { return Mix(c, 0xFF000000, t); }
}  // namespace

PivotStyleLook PivotStyleFor(const std::string& name) {
  PivotStyleLook L;
  std::string n = LowerStr(name);
  if (n.rfind("pivotstyle", 0) == 0) n = n.substr(10);
  int fam = -1;
  for (const auto& [p, f] : {std::pair<const char*, int>{"light", 0}, {"medium", 1}, {"dark", 2}}) {
    if (n.rfind(p, 0) == 0) {
      fam = f;
      n = n.substr(std::string(p).size());
    }
  }
  int num = 0;
  for (char ch : n) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      num = 0;
      break;
    }
    num = num * 10 + (ch - '0');
  }
  if (fam < 0 || num < 1 || num > 28) {
    L.none = true;  // "None" (or a name YCGrid doesn't know): no formatting
    return L;
  }
  // the seven colors of each row of the gallery: dark gray, then the theme's six accents
  static const uint32_t kAccent[7] = {0xFF404040, 0xFF4472C4, 0xFFED7D31, 0xFFA5A5A5, 0xFFFFC000, 0xFF5B9BD5, 0xFF70AD47};
  const uint32_t a = kAccent[(num - 1) % 7];
  const int tier = (num - 1) / 7;
  const uint32_t white = 0xFFFFFFFF;
  if (fam == 0) {
    switch (tier) {
      case 0:
        L.header_line = a;
        L.grand_line = a;
        L.band_fill = Tint(a, 0.85);
        break;
      case 1:
        L.header_fill = a;
        L.header_font = white;
        L.grand_line = a;
        L.band_fill = Tint(a, 0.85);
        break;
      case 2:
        L.header_fill = Tint(a, 0.8);
        L.header_line = Tint(a, 0.4);
        L.grand_fill = Tint(a, 0.8);
        L.grand_line = Tint(a, 0.4);
        L.band_fill = Tint(a, 0.9);
        break;
      default:
        L.header_fill = Tint(a, 0.6);
        L.body_line = Tint(a, 0.6);
        L.grand_fill = Tint(a, 0.6);
        L.band_fill = Tint(a, 0.85);
        break;
    }
    if (num == 16) {  // the default: Excel's exact light blue
      L.header_fill = L.grand_fill = 0xFFDDEBF7;
      L.header_line = L.grand_line = 0xFF8EA9DB;
    }
  } else if (fam == 1) {
    switch (tier) {
      case 0:
        L.header_fill = a;
        L.header_font = white;
        L.grand_fill = Tint(a, 0.6);
        L.band_fill = Tint(a, 0.85);
        break;
      case 1:
        L.header_fill = Shade(a, 0.25);
        L.header_font = white;
        L.subtotal_fill = Tint(a, 0.8);
        L.grand_fill = Shade(a, 0.25);
        L.band_fill = Tint(a, 0.85);
        break;
      case 2:
        L.header_fill = Tint(a, 0.4);
        L.grand_fill = Tint(a, 0.4);
        L.band_fill = Tint(a, 0.8);
        break;
      default:
        L.header_fill = Tint(a, 0.6);
        L.body_line = Tint(a, 0.4);
        L.grand_fill = Tint(a, 0.6);
        L.band_fill = Tint(a, 0.8);
        break;
    }
  } else {
    L.header_fill = Shade(a, tier % 2 ? 0.25 : 0.5);
    L.header_font = white;
    L.grand_fill = Shade(a, 0.5);
    L.subtotal_fill = Tint(a, tier >= 2 ? 0.4 : 0.6);
    L.band_fill = Tint(a, 0.6);
    L.grand_line = white;
  }
  return L;
}

std::vector<std::string> PivotStyleNames() {
  std::vector<std::string> out;
  for (const char* f : {"Light", "Medium", "Dark"}) {
    for (int n = 1; n <= 28; ++n) out.push_back(std::string("PivotStyle") + f + std::to_string(n));
  }
  return out;
}

}  // namespace yc::engine
