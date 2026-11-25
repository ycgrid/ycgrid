// SPDX-License-Identifier: Apache-2.0
// error_check.cpp - Excel's background error checking rules (see error_check.h)
#include "engine/error_check.h"

#include "core/i18n.h"
#include "engine/document.h"
#include "engine/rule_eval.h"
#include "formula/ast.h"
#include "formula/numfmt.h"
#include "formula/parser.h"
#include "model/table.h"
#include "model/workbook.h"

#include <algorithm>
#include <cctype>

namespace yc::engine {

namespace {

uint32_t FormulaAt(const model::Sheet& s, int64_t r, int64_t c) {
  if (r < 0 || c < 0 || r >= s.row_limit || c >= s.col_limit) return 0;
  return s.GetFormula(static_cast<uint32_t>(r), static_cast<uint32_t>(c));
}

const formula::CompiledFormula* Compiled(const model::Workbook& wb, uint32_t id) {
  return id ? static_cast<const formula::CompiledFormula*>(wb.formulas->Get(id)) : nullptr;
}

bool IsNumberCell(const model::Sheet& s, int64_t r, int64_t c) {
  if (r < 0 || c < 0 || r >= s.row_limit || c >= s.col_limit) return false;
  return s.GetValue(static_cast<uint32_t>(r), static_cast<uint32_t>(c)).IsNumeric();
}

// text that reads as a date whose year has two digits ("12/31/99", "31-Dec-99"): the last group of digits is the year
bool TwoDigitYearText(std::string_view t, size_t* year_pos) {
  int groups = 0;
  size_t last_pos = 0, last_len = 0;
  for (size_t i = 0; i < t.size();) {
    const unsigned char ch = static_cast<unsigned char>(t[i]);
    if (std::isdigit(ch)) {
      size_t j = i;
      while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) ++j;
      ++groups;
      last_pos = i;
      last_len = j - i;
      i = j;
    } else if (std::isalpha(ch)) {
      size_t j = i;
      while (j < t.size() && std::isalpha(static_cast<unsigned char>(t[j]))) ++j;
      ++groups;  // a month's name
      last_len = 0;
      i = j;
    } else if (ch == '/' || ch == '-' || ch == '.' || ch == ' ' || ch == ',') {
      ++i;
    } else {
      return false;  // a time (":") or anything else
    }
  }
  if (groups != 3 || last_len != 2) return false;
  if (year_pos) *year_pos = last_pos;
  return true;
}

bool IsTextDate(const core::Value& v, size_t* year_pos = nullptr) {
  if (!v.IsString()) return false;
  const std::string_view t = v.str();
  if (t.size() < 6 || t.size() > 20 || !TwoDigitYearText(t, year_pos)) return false;
  const formula::InputParse p = formula::ParseInput(t);
  if (!p.value.IsNumeric() || p.format.empty()) return false;
  return p.format.find_first_of("dDyY") != std::string::npos;
}

bool NumberAsText(const core::Value& v) {
  if (!v.IsString()) return false;
  const std::string_view t = v.str();
  size_t k = 0;
  while (k < t.size() && t[k] == ' ') ++k;
  // quick test first: a number starts with a digit, a sign, a point or a currency sign
  if (k >= t.size() || !(std::isdigit(static_cast<unsigned char>(t[k])) || t[k] == '-' || t[k] == '+' || t[k] == '.' || t[k] == '$' || t[k] == '(')) return false;
  const formula::InputParse p = formula::ParseInput(t);
  return p.value.IsNumeric() && p.format.empty();
}

// a range of the formula (one row or one column of at least two cells) with a number just past one of its ends
bool FindOmitted(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, core::Rect* old_range, core::Rect* new_range) {
  const formula::CompiledFormula* f = Compiled(wb, s.GetFormula(r, c));
  if (!f) return false;
  std::vector<formula::ResolvedRef> refs;
  formula::CollectRefs(f->root, core::CellPos{r, c}, &refs);
  for (const auto& ref : refs) {
    if (ref.sheet >= 0 && static_cast<uint32_t>(ref.sheet) != s.id) continue;
    core::Rect q = ref.rect;
    if (q.IsCell() || q.r1 >= s.row_limit - 1 || q.c1 >= s.col_limit - 1) continue;  // a cell, whole rows / columns
    const bool col = q.c0 == q.c1, row = q.r0 == q.r1;
    if (!col && !row) continue;
    core::Rect grown = q;
    auto self = [&](int64_t rr, int64_t cc) { return rr == r && cc == c; };
    if (col) {
      while (IsNumberCell(s, int64_t{grown.r1} + 1, q.c0) && !self(int64_t{grown.r1} + 1, q.c0)) ++grown.r1;
      while (grown.r0 > 0 && IsNumberCell(s, int64_t{grown.r0} - 1, q.c0) && !self(int64_t{grown.r0} - 1, q.c0)) --grown.r0;
    } else {
      while (IsNumberCell(s, q.r0, int64_t{grown.c1} + 1) && !self(q.r0, int64_t{grown.c1} + 1)) ++grown.c1;
      while (grown.c0 > 0 && IsNumberCell(s, q.r0, int64_t{grown.c0} - 1) && !self(q.r0, int64_t{grown.c0} - 1)) --grown.c0;
    }
    // the formula's own cell inside the grown range would make it circular
    if (grown == q || grown.Contains(r, c)) continue;
    if (old_range) *old_range = q;
    if (new_range) *new_range = grown;
    return true;
  }
  return false;
}

bool RefersToEmpty(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c) {
  const formula::CompiledFormula* f = Compiled(wb, s.GetFormula(r, c));
  if (!f) return false;
  std::vector<formula::ResolvedRef> refs;
  formula::CollectRefs(f->root, core::CellPos{r, c}, &refs);
  for (const auto& ref : refs) {
    if (!ref.rect.IsCell()) continue;
    const model::Sheet* rs = ref.sheet < 0 ? &s : wb.SheetById(static_cast<uint32_t>(ref.sheet));
    if (rs && rs->IsEmptyCell(ref.rect.r0, ref.rect.c0)) return true;
  }
  return false;
}

// a data cell of a table column whose other cells share one formula this one doesn't have
bool InconsistentColumnFormula(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c) {
  const model::Table* t = model::TableAt(wb, s.id, r, c);
  if (!t) return false;
  const core::Rect d = t->Data();
  if (d.r0 > d.r1 || r < d.r0 || r > d.r1 || d.Rows() < 3) return false;
  const uint32_t own = s.GetFormula(r, c);
  // the two cells above, the two below, or one on each side
  auto at = [&](int64_t rr) -> int64_t { return rr >= d.r0 && rr <= d.r1 ? static_cast<int64_t>(s.GetFormula(static_cast<uint32_t>(rr), c)) : -1; };
  const int64_t up = at(int64_t{r} - 1), up2 = at(int64_t{r} - 2), dn = at(int64_t{r} + 1), dn2 = at(int64_t{r} + 2);
  int64_t col_f = -1;
  if (up > 0 && up == dn) col_f = up;
  else if (up < 0 && dn > 0 && dn == dn2) col_f = dn;
  else if (dn < 0 && up > 0 && up == up2) col_f = up;
  if (col_f <= 0 || col_f == static_cast<int64_t>(own)) return false;
  return own || !s.GetValue(r, c).IsEmpty();
}

// a formula Misleading number formats looks at: references, numbers, arithmetic and the functions whose result is
// in the unit of their arguments (=A1+B1, =SUM(B2:B9)*2); =YEAR(A1) or =COUNT(A:A) give another kind of number
bool KeepsUnit(const formula::Node& n) {
  using formula::NK;
  switch (n.kind) {
    case NK::Num: case NK::Ref: case NK::Name: case NK::Paren: case NK::Unary: case NK::Percent: break;
    case NK::Binary:
      if (n.op != '+' && n.op != '-' && n.op != '*' && n.op != '/' && n.op != ':') return false;
      break;
    case NK::Func: {
      static const char* const kSame[] = {"SUM", "AVERAGE", "MIN", "MAX", "MEDIAN", "ROUND", "ROUNDUP", "ROUNDDOWN", "ABS", "MROUND", "FLOOR", "CEILING", "SUBTOTAL"};
      bool ok = false;
      for (const char* f : kSame) ok = ok || n.str == f;
      if (!ok) return false;
      break;
    }
    default: return false;
  }
  for (const formula::Node& k : n.kids) {
    if (!KeepsUnit(k)) return false;
  }
  return true;
}

bool BreaksTableValidation(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c) {
  if (s.validations->empty() || s.tables->empty()) return false;
  const core::Value& v = s.GetValue(r, c);
  if (v.IsEmpty()) return false;
  const model::Table* t = model::TableAt(wb, s.id, r, c);
  if (!t || !t->Data().Contains(r, c)) return false;
  const model::DvRule* rule = FindValidation(s, r, c);
  if (!rule) return false;
  const std::string text = v.IsString() ? std::string(v.str()) : DisplayTextOf(v, "General");
  std::string why;
  return !ValidateInput(wb, s.id, r, c, text, *rule, &why);
}

}  // namespace

int FormatKindOf(std::string_view code) {
  if (code.empty() || code == "General") return kFmtGeneral;
  if (code == "@") return kFmtText;
  // the first section, without quoted text and escaped characters
  std::string t;
  bool q = false, quoted_currency = false;
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = code[i];
    if (ch == '"') {
      q = !q;
      continue;
    }
    if (q) {
      if (ch == '$') quoted_currency = true;  // "NT$"#,##0: Taiwan's currency, as typed or from the zh-TW ribbon
      continue;
    }
    if (ch == '\\' && i + 1 < code.size()) {
      ++i;
      continue;
    }
    if (ch == ';') break;
    t.push_back(ch);
  }
  const bool currency_tag = t.find("[$") != std::string::npos;
  // [Red], [h] and the like: only the currency tag [$...] matters
  std::string u;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '[') {
      const size_t e = t.find(']', i);
      const std::string_view inner = std::string_view(t).substr(i + 1, e == std::string::npos ? 0 : e - i - 1);
      if (!inner.empty() && (inner[0] == 'h' || inner[0] == 'H' || inner[0] == 'm' || inner[0] == 'M' || inner[0] == 's' || inner[0] == 'S')) u += std::string(inner);
      i = e == std::string::npos ? t.size() : e;
      continue;
    }
    u.push_back(t[i]);
  }
  if (u.find('%') != std::string::npos) return kFmtPercent;
  if (u.find("E+") != std::string::npos || u.find("E-") != std::string::npos || u.find("e+") != std::string::npos) return kFmtScientific;
  if (u.find('?') != std::string::npos && u.find('/') != std::string::npos) return kFmtFraction;
  if (formula::IsDateFormat(code)) {
    const bool date = u.find_first_of("yYdD") != std::string::npos;
    const bool time = u.find_first_of("hHsS") != std::string::npos;
    return date || !time ? kFmtDate : kFmtTime;
  }
  if (u.rfind("_(", 0) == 0 && u.find('*') != std::string::npos) return kFmtAccounting;
  if (currency_tag || quoted_currency || u.find_first_of("$") != std::string::npos || u.find("\xE2\x82\xAC") != std::string::npos || u.find("\xC2\xA3") != std::string::npos ||
      u.find("\xC2\xA5") != std::string::npos)
    return kFmtCurrency;
  return kFmtNumber;
}

const char* FormatKindName(int kind) {
  switch (kind) {
    case kFmtCurrency: return TR("Currency");
    case kFmtAccounting: return TR("Accounting");
    case kFmtDate: return TR("Date");
    case kFmtTime: return TR("Time");
    case kFmtPercent: return TR("Percentage");
    case kFmtFraction: return TR("Fraction");
    case kFmtScientific: return TR("Scientific");
    case kFmtNumber: return TR("Number");
    case kFmtText: return TR("Text");
    default: return TR("General");
  }
}

bool MisleadingFormat(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, uint32_t* numfmt, int* kind) {
  const formula::CompiledFormula* f = Compiled(wb, s.GetFormula(r, c));
  if (!f || !s.GetValue(r, c).IsNumeric() || !KeepsUnit(f->root)) return false;
  std::vector<formula::ResolvedRef> refs;
  formula::CollectRefs(f->root, core::CellPos{r, c}, &refs);
  const model::StyleTable& st = *wb.styles;
  int common = -1, seen = 0;
  uint32_t first_fmt = 0;
  for (const auto& ref : refs) {
    const model::Sheet* rs = ref.sheet < 0 ? &s : wb.SheetById(static_cast<uint32_t>(ref.sheet));
    if (!rs || !rs->has_used) continue;
    const core::Rect q{ref.rect.r0, ref.rect.c0, std::min(ref.rect.r1, rs->used.r1), std::min(ref.rect.c1, rs->used.c1)};
    for (uint32_t rr = q.r0; rr <= q.r1 && seen < 64; ++rr) {
      for (uint32_t cc = q.c0; cc <= q.c1 && seen < 64; ++cc) {
        if (!rs->GetValue(rr, cc).IsNumeric()) continue;
        ++seen;
        const model::Style& cs = st.Get(rs->GetStyle(rr, cc));
        const int k = FormatKindOf(st.NumberFormat(cs));
        if (common < 0) {
          common = k;
          first_fmt = cs.numfmt;
        } else if (k != common) {
          return false;
        }
      }
    }
  }
  if (common != kFmtCurrency && common != kFmtAccounting && common != kFmtPercent && common != kFmtDate && common != kFmtTime && common != kFmtFraction &&
      common != kFmtScientific)
    return false;
  const int own = FormatKindOf(st.NumberFormat(st.Get(s.GetStyle(r, c))));
  if (own == common) return false;
  // General shows a plain number: misleading for dates, times, percentages and currency only
  if (own == kFmtGeneral && common != kFmtDate && common != kFmtTime && common != kFmtPercent && common != kFmtCurrency) return false;
  if (numfmt) *numfmt = first_fmt;
  if (kind) *kind = common;
  return true;
}

bool ErrorCheckOptions::On(int rule) const {
  switch (rule) {
    case kErrEvaluateToError: return evaluate_to_error;
    case kErrTextDate: return text_date;
    case kErrNumberAsText: return number_as_text;
    case kErrInconsistentFormula: return inconsistent_formula;
    case kErrOmittedCells: return omitted_cells;
    case kErrUnlockedFormula: return unlocked_formula;
    case kErrEmptyCellReference: return empty_cell_reference;
    case kErrListDataValidation: return list_data_validation;
    case kErrInconsistentListFormula: return inconsistent_list_formula;
    case kErrMisleadingFormat: return misleading_format;
    default: return false;
  }
}

bool ErrorIgnored(const model::Sheet& s, uint32_t r, uint32_t c) {
  for (const core::Rect& a : *s.ignored_errors) {
    if (a.Contains(r, c)) return true;
  }
  return false;
}

int ErrorCheckRule(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, const ErrorCheckOptions& o, bool check_ignored) {
  if (!o.background) return kErrNone;
  const uint32_t f = s.GetFormula(r, c);
  const core::Value& v = s.GetValue(r, c);
  int rule = kErrNone;
  if (f) {
    if (o.evaluate_to_error && v.IsError() && v.error() != core::Err::GettingData) {
      rule = kErrEvaluateToError;
    } else if (o.inconsistent_list_formula && !s.tables->empty() && InconsistentColumnFormula(wb, s, r, c)) {
      rule = kErrInconsistentListFormula;
    } else if (o.inconsistent_formula && [&] {
                 // shared formulas have one id per R1C1 text: neighbors that agree with each other but not with this one
                 const uint32_t up = FormulaAt(s, int64_t{r} - 1, c), down = FormulaAt(s, int64_t{r} + 1, c);
                 const uint32_t left = FormulaAt(s, r, int64_t{c} - 1), right = FormulaAt(s, r, int64_t{c} + 1);
                 return (up && up == down && up != f) || (left && left == right && left != f);
               }()) {
      rule = kErrInconsistentFormula;
    } else if (o.omitted_cells && FindOmitted(wb, s, r, c, nullptr, nullptr)) {
      rule = kErrOmittedCells;
    } else if (o.unlocked_formula && !wb.styles->Get(s.GetStyle(r, c)).locked) {
      rule = kErrUnlockedFormula;
    } else if (o.empty_cell_reference && RefersToEmpty(wb, s, r, c)) {
      rule = kErrEmptyCellReference;
    } else if (o.misleading_format && MisleadingFormat(wb, s, r, c, nullptr, nullptr)) {
      rule = kErrMisleadingFormat;
    }
  } else if (v.IsString()) {
    if (o.text_date && IsTextDate(v)) rule = kErrTextDate;
    else if (o.number_as_text && NumberAsText(v)) rule = kErrNumberAsText;
    else if (o.list_data_validation && BreaksTableValidation(wb, s, r, c)) rule = kErrListDataValidation;
    else if (o.inconsistent_list_formula && !s.tables->empty() && InconsistentColumnFormula(wb, s, r, c)) rule = kErrInconsistentListFormula;
  } else if (!v.IsEmpty() && !s.tables->empty()) {
    if (o.list_data_validation && BreaksTableValidation(wb, s, r, c)) rule = kErrListDataValidation;
    else if (o.inconsistent_list_formula && InconsistentColumnFormula(wb, s, r, c)) rule = kErrInconsistentListFormula;
  }
  if (rule != kErrNone && check_ignored && !s.ignored_errors->empty() && ErrorIgnored(s, r, c)) return kErrNone;
  return rule;
}

const char* ErrorRuleTitle(int rule, const core::Value& v) {
  switch (rule) {
    case kErrEvaluateToError:
      switch (v.IsError() ? v.error() : core::Err::Value) {
        case core::Err::Div0: return TR("Divide by Zero Error");
        case core::Err::NA: return TR("Value Not Available Error");
        case core::Err::Name: return TR("Invalid Name Error");
        case core::Err::Null: return TR("Null Error");
        case core::Err::Num: return TR("Number Error");
        case core::Err::Ref: return TR("Invalid Cell Reference Error");
        case core::Err::Spill: return TR("Spill range isn't blank");
        case core::Err::Calc: return TR("Calculation Error");
        default: return TR("Error in Value");
      }
    case kErrTextDate: return TR("Text Date with 2-Digit Year");
    case kErrNumberAsText: return TR("Number Stored as Text");
    case kErrInconsistentFormula: return TR("Inconsistent Formula");
    case kErrOmittedCells: return TR("Formula Omits Adjacent Cells");
    case kErrUnlockedFormula: return TR("Unprotected Formula");
    case kErrEmptyCellReference: return TR("Formula Refers to Empty Cells");
    case kErrListDataValidation: return TR("Data Validation Error");
    case kErrInconsistentListFormula: return TR("Inconsistent Calculated Column Formula");
    case kErrMisleadingFormat: return TR("Misleading Number Format");
    default: return TR("");
  }
}

const char* ErrorRuleHelp(int rule, const core::Value& v) {
  switch (rule) {
    case kErrEvaluateToError:
      switch (v.IsError() ? v.error() : core::Err::Value) {
        case core::Err::Div0: return TR("The formula or function used is dividing by zero or empty cells.");
        case core::Err::NA: return TR("A value is not available to the formula or function.");
        case core::Err::Name: return TR("The formula contains unrecognized text.");
        case core::Err::Null: return TR("The ranges in the formula don't intersect.");
        case core::Err::Num: return TR("A number in the formula or function is not valid or the result is too large or too small.");
        case core::Err::Ref: return TR("The formula refers to cells that were deleted or replaced.");
        case core::Err::Spill: return TR("The formula's result would spill into cells that aren't blank.");
        case core::Err::Calc: return TR("The calculation engine couldn't calculate this formula (an empty array, for example).");
        default: return TR("A value used in the formula is of the wrong data type.");
      }
    case kErrTextDate: return TR("The date in this cell is stored as text with a two-digit year, which can be read as 19XX or 20XX.");
    case kErrNumberAsText: return TR("The number in this cell is formatted as text or preceded by an apostrophe.");
    case kErrInconsistentFormula: return TR("The formula in this cell differs from the formulas in this area of the spreadsheet.");
    case kErrOmittedCells: return TR("The formula in this cell refers to a range that has additional numbers adjacent to it.");
    case kErrUnlockedFormula: return TR("The cell contains a formula and is not locked to protect it from being changed inadvertently.");
    case kErrEmptyCellReference: return TR("The formula in this cell refers to empty cells.");
    case kErrListDataValidation: return TR("The value in this cell doesn't meet the data validation rule of the cell.");
    case kErrInconsistentListFormula: return TR("This cell is inconsistent with the column formula of the table.");
    case kErrMisleadingFormat: return TR("The number format of this cell doesn't match the number format of the cells its formula refers to.");
    default: return TR("");
  }
}

bool OmittedCellsFix(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, core::Rect* old_range, core::Rect* new_range) {
  return FindOmitted(wb, s, r, c, old_range, new_range);
}

std::string OmittedCellsFormulaText(const Document& d, uint32_t sheet, uint32_t r, uint32_t c) {
  const model::Sheet* s = d.wb().SheetById(sheet);
  core::Rect from{}, to{};
  if (!s || !FindOmitted(d.wb(), *s, r, c, &from, &to)) return {};
  const formula::CompiledFormula* f = d.Formula(s->GetFormula(r, c));
  if (!f) return {};
  formula::Node root = f->root;
  const core::CellPos origin{r, c};
  formula::ForEachRef(root, [&](formula::RefSpec& rs) {
    formula::ResolvedRef rr;
    if (!formula::ResolveRef(rs, origin, &rr) || (rr.sheet >= 0 && static_cast<uint32_t>(rr.sheet) != sheet) || !(rr.rect == from)) return;
    auto put = [](formula::RefCoord& k, int64_t v, int64_t o) { k.v = k.abs ? v : v - o; };
    const bool rows_up = rs.r0.Resolve(r) <= rs.r1.Resolve(r), cols_up = rs.c0.Resolve(c) <= rs.c1.Resolve(c);
    put(rows_up ? rs.r0 : rs.r1, to.r0, r);
    put(rows_up ? rs.r1 : rs.r0, to.r1, r);
    put(cols_up ? rs.c0 : rs.c1, to.c0, c);
    put(cols_up ? rs.c1 : rs.c0, to.c1, c);
  });
  return "=" + formula::ToA1(root, origin, &d);
}

bool NextErrorCell(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, bool forward, const ErrorCheckOptions& o, uint32_t* out_r,
                   uint32_t* out_c) {
  if (!s.has_used) return false;
  const core::Rect u = s.used;
  // reading order: along the row, then the next row; a bounded walk of the used range
  uint64_t budget = 4'000'000;
  int64_t rr = r, cc = c;
  auto step = [&]() {
    if (forward) {
      if (++cc > u.c1) cc = u.c0, ++rr;
    } else {
      if (--cc < static_cast<int64_t>(u.c0)) cc = u.c1, --rr;
    }
  };
  if (rr < u.r0) rr = u.r0, cc = forward ? int64_t{u.c0} - 1 : u.c0;
  step();
  for (; budget-- && rr >= u.r0 && rr <= u.r1; step()) {
    if (cc < u.c0 || cc > u.c1) continue;
    if (ErrorCheckRule(wb, s, static_cast<uint32_t>(rr), static_cast<uint32_t>(cc), o) != kErrNone) {
      *out_r = static_cast<uint32_t>(rr);
      *out_c = static_cast<uint32_t>(cc);
      return true;
    }
  }
  return false;
}

}  // namespace yc::engine
