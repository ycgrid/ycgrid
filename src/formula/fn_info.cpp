// SPDX-License-Identifier: Apache-2.0
// fn_info.cpp - information functions
#include "formula/fn_util.h"
#include "formula/numfmt.h"

#include <cmath>
#include <filesystem>

namespace yc::formula::fn {
namespace {

// IS* functions: argument errors don't propagate
template <class P>
EV IsImpl(Evaluator& ev, const Node& call, P pred) {
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Array) {
    auto o = std::make_shared<Array>(*e.a);
    for (Value& v : o->v) v = Value::Bool(pred(v));
    return EV::A(o);
  }
  return Bool(pred(ev.ToScalar(e)));
}

EV FIsNumber(Evaluator& ev, const Node& call) { return IsImpl(ev, call, [](const Value& v) { return v.IsNumeric(); }); }
EV FIsText(Evaluator& ev, const Node& call) { return IsImpl(ev, call, [](const Value& v) { return v.IsString(); }); }
EV FIsNonText(Evaluator& ev, const Node& call) { return IsImpl(ev, call, [](const Value& v) { return !v.IsString(); }); }
EV FIsBlank(Evaluator& ev, const Node& call) { return IsImpl(ev, call, [](const Value& v) { return v.IsEmpty(); }); }
EV FIsLogical(Evaluator& ev, const Node& call) { return IsImpl(ev, call, [](const Value& v) { return v.IsBool(); }); }
EV FIsError(Evaluator& ev, const Node& call) { return IsImpl(ev, call, [](const Value& v) { return v.IsError(); }); }
EV FIsErr(Evaluator& ev, const Node& call) { return IsImpl(ev, call, [](const Value& v) { return v.IsError() && v.error() != Err::NA; }); }
EV FIsNa(Evaluator& ev, const Node& call) { return IsImpl(ev, call, [](const Value& v) { return v.IsError() && v.error() == Err::NA; }); }

// ISEVEN / ISODD take numbers and numbers as text; a logical value is #VALUE! (Excel 16: ISEVEN(TRUE))
bool EvenOddArg(Evaluator& ev, const Node& call, double* x, EV* err) {
  const Value v = ev.Scalar(call.kids[0]);
  if (v.IsError()) return *err = EV::S(v), false;
  Err e = Err::None;
  if (v.IsBool() || !Evaluator::ToNumber(v, x, &e)) return *err = Er(v.IsBool() ? Err::Value : e), false;
  return true;
}
EV FIsEven(Evaluator& ev, const Node& call) {
  double x = 0;
  EV err;
  if (!EvenOddArg(ev, call, &x, &err)) return err;
  return Bool(std::fmod(std::trunc(x), 2.0) == 0);
}
EV FIsOdd(Evaluator& ev, const Node& call) {
  double x = 0;
  EV err;
  if (!EvenOddArg(ev, call, &x, &err)) return err;
  return Bool(std::fmod(std::trunc(x), 2.0) != 0);
}

EV FIsRef(Evaluator& ev, const Node& call) {
  const EV e = ev.Eval(call.kids[0]);
  return Bool(e.k == EV::K::Range);
}

EV FIsFormula(Evaluator& ev, const Node& call) {
  RangeRef r;
  if (!ev.ArgRange(call.kids[0], &r)) return Er(Err::Value);
  return Bool(ev.host().HasFormula(r.sheet, r.rect.r0, r.rect.c0));
}

EV FFormulaText(Evaluator& ev, const Node& call) {
  RangeRef r;
  if (!ev.ArgRange(call.kids[0], &r)) return Er(Err::Value);
  if (!ev.host().HasFormula(r.sheet, r.rect.r0, r.rect.c0)) return Er(Err::NA);
  return Str(ev.host().FormulaText(r.sheet, r.rect.r0, r.rect.c0));
}

EV FNa(Evaluator&, const Node&) { return Er(Err::NA); }

// GETPIVOTDATA(data_field, pivot_table, [field1, item1], ...)
EV FGetPivotData(Evaluator& ev, const Node& call) {
  YC_TEXT(field, 0);
  RangeRef r;
  if (!ev.ArgRange(call.kids[1], &r)) return Er(Err::Ref);
  std::vector<std::pair<std::string, std::string>> pairs;
  for (size_t i = 2; i + 1 < call.kids.size(); i += 2) {
    std::string f, item;
    Value e;
    if (!ev.ArgText(call.kids[i], &f, &e) || !ev.ArgText(call.kids[i + 1], &item, &e)) return EV::S(e);
    pairs.emplace_back(f, item);
  }
  Value out;
  if (!ev.host().PivotData(r.sheet, r.rect.r0, r.rect.c0, field, pairs, &out)) return Er(Err::Ref);
  return EV::S(out);
}

EV FType(Evaluator& ev, const Node& call) {
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Array) return Num(64);
  if (e.k == EV::K::Range && (e.r.rect.Rows() > 1 || e.r.rect.Cols() > 1)) return Num(64);
  const Value v = ev.ToScalar(e);
  if (v.IsNumeric() || v.IsEmpty()) return Num(1);
  if (v.IsString()) return Num(2);
  if (v.IsBool()) return Num(4);
  if (v.IsError()) return Num(16);
  return Num(1);
}

EV FErrorType(Evaluator& ev, const Node& call) {
  const Value v = ev.Scalar(call.kids[0]);
  if (!v.IsError()) return Er(Err::NA);
  switch (v.error()) {
    case Err::Null: return Num(1);
    case Err::Div0: return Num(2);
    case Err::Value: return Num(3);
    case Err::Ref: return Num(4);
    case Err::Name: return Num(5);
    case Err::Num: return Num(6);
    case Err::NA: return Num(7);
    case Err::GettingData: return Num(8);
    case Err::Spill: return Num(9);
    case Err::Connect: return Num(10);
    case Err::Blocked: return Num(11);
    case Err::Calc: return Num(14);
    default: return Num(15);
  }
}

EV FSheet(Evaluator& ev, const Node& call) {
  if (!Has(call, 0)) return Num(ev.host().SheetIndex(ev.sheet()));
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Range) return Num(ev.host().SheetIndex(e.r.sheet));
  const Value v = ev.ToScalar(e);
  if (v.IsString()) {
    const int32_t id = ev.host().FindSheet(v.str());
    if (id < 0) return Er(Err::NA);
    return Num(ev.host().SheetIndex(id));
  }
  return Er(Err::NA);
}

EV FSheets(Evaluator& ev, const Node& call) {
  if (!Has(call, 0)) return Num(ev.host().SheetCount());
  // a 3-D reference spans the sheets between its ends (Excel: SHEETS(Other:S3!A1) is 2), read from the reference
  // itself (a 3-D range has no value of its own)
  const Node& arg = call.kids[0].kind == NK::Paren && !call.kids[0].kids.empty() ? call.kids[0].kids[0] : call.kids[0];
  if (arg.kind == NK::Ref && !arg.ref.bad && arg.ref.sheet >= 0 && arg.ref.sheet2 >= 0) {
    const int32_t a = ev.host().SheetIndex(arg.ref.sheet), b = ev.host().SheetIndex(arg.ref.sheet2);
    if (a > 0 && b > 0) return Num(std::abs(a - b) + 1);
  }
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Scalar && e.s.IsError()) return e;
  return Num(1);
}

// CELL: a subset of the common info_type values
// INFO(type_text): environment information (values a 64-bit Windows Excel would report where they apply)
EV FInfo(Evaluator& ev, const Node& call) {
  YC_TEXT(type, 0);
  const std::string k = Lower(type);
  if (k == "numfile") return Num(static_cast<double>(ev.host().SheetCount()));
  if (k == "origin") return Str("$A:$A$1");
  if (k == "osversion") return Str("Windows (64-bit) NT 10.00");
  if (k == "recalc") return Str("Automatic");
  if (k == "release") return Str("16.0");
  if (k == "system") return Str("pcdos");
  if (k == "directory") {
    std::error_code ec;
    std::string d = std::filesystem::current_path(ec).string();
    if (!d.empty() && d.back() != '\\') d += '\\';
    return Str(d);
  }
  return Er(Err::Value);
}

EV FCell(Evaluator& ev, const Node& call) {
  YC_TEXT(kind, 0);
  RangeRef r{ev.sheet(), core::Rect{ev.origin().row, ev.origin().col, ev.origin().row, ev.origin().col}};
  if (Has(call, 1) && !ev.ArgRange(call.kids[1], &r)) return Er(Err::Value);
  const std::string k = Lower(kind);
  const uint32_t row = r.rect.r0, col = r.rect.c0;
  if (k == "row") return Num(row + 1.0);
  if (k == "col") return Num(col + 1.0);
  if (k == "address") {
    // another sheet's cell names its workbook and sheet, as Excel's '[Book1]My Sheet'!$C$3 (the workbook part is
    // CELL("filename")'s)
    const std::string a1 = core::CellA1(row, col, true, true);
    if (r.sheet == ev.sheet()) return Str(a1);
    const std::string q = core::QuoteSheetName(ev.host().SheetName(r.sheet));
    return Str(q.front() == '\'' ? "'[Book]" + q.substr(1) + "!" + a1 : "[Book]" + q + "!" + a1);
  }
  if (k == "contents") return EV::S(ev.host().Cell(r.sheet, row, col));
  if (k == "type") {
    const Value& v = ev.host().Cell(r.sheet, row, col);
    return Str(v.IsEmpty() ? "b" : v.IsString() ? "l" : "v");
  }
  if (k == "sheetname") return Str(ev.host().SheetName(r.sheet));
  if (k == "filename") return Str("[" + std::string("Book") + "]" + ev.host().SheetName(r.sheet));
  if (k == "format" || k == "color" || k == "parentheses") {
    bool color = false, parens = false;
    const std::string code = CellFormatCode(ev.host().LookOf(r.sheet, row, col).numfmt, &color, &parens);
    if (k == "format") return Str(code);
    return Num(k == "color" ? (color ? 1 : 0) : (parens ? 1 : 0));
  }
  if (k == "prefix") {
    // text's alignment: ' left (and General), " right, ^ centered, \ fill; nothing for numbers and blanks
    if (!ev.host().Cell(r.sheet, row, col).IsString()) return Str("");
    switch (ev.host().LookOf(r.sheet, row, col).halign) {
      case 0:
      case 1: return Str("'");
      case 2: return Str("^");
      case 3: return Str("\"");
      case 4: return Str("\\");
      default: return Str("");
    }
  }
  if (k == "protect") return Num(ev.host().LookOf(r.sheet, row, col).locked ? 1 : 0);
  if (k == "width") return Num(std::floor(ev.host().LookOf(r.sheet, row, col).width + 1e-9));
  return Er(Err::Value);
}

}  // namespace

void RegisterInfo(std::vector<FuncInfo>& t) {
  t.push_back({"ISNUMBER", 1, 1, FIsNumber});
  t.push_back({"ISTEXT", 1, 1, FIsText});
  t.push_back({"ISNONTEXT", 1, 1, FIsNonText});
  t.push_back({"ISBLANK", 1, 1, FIsBlank});
  t.push_back({"ISLOGICAL", 1, 1, FIsLogical});
  t.push_back({"ISERROR", 1, 1, FIsError});
  t.push_back({"ISERR", 1, 1, FIsErr});
  t.push_back({"ISNA", 1, 1, FIsNa});
  t.push_back({"ISEVEN", 1, 1, FIsEven});
  t.push_back({"ISODD", 1, 1, FIsOdd});
  t.push_back({"ISREF", 1, 1, FIsRef});
  t.push_back({"ISFORMULA", 1, 1, FIsFormula});
  t.push_back({"FORMULATEXT", 1, 1, FFormulaText});
  t.push_back({"NA", 0, 0, FNa});
  t.push_back({"GETPIVOTDATA", 2, -1, FGetPivotData, true});
  t.push_back({"TYPE", 1, 1, FType});
  t.push_back({"ERROR.TYPE", 1, 1, FErrorType});
  t.push_back({"SHEET", 0, 1, FSheet, true});    // volatile: sheets added, moved, copied and deleted change it
  t.push_back({"SHEETS", 0, 1, FSheets, true});
  t.push_back({"CELL", 1, 2, FCell, true});
  t.push_back({"INFO", 1, 1, FInfo, true});
}

}  // namespace yc::formula::fn
