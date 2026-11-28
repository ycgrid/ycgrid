// SPDX-License-Identifier: Apache-2.0
#include "formula/eval.h"

#include "formula/collate.h"
#include "formula/functions.h"
#include "formula/numfmt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace yc::formula {

using core::Err;

namespace fn {
std::string Lower(std::string_view s);  // functions.cpp: ASCII, accented Latin, Greek, Cyrillic and full-width letters
size_t Utf16Length(std::string_view s);
std::string Utf16Substr(std::string_view s, size_t start, size_t len);
}

namespace {

constexpr size_t kMaxArrayCells = 4'000'000;

// comparisons ignore case as LOWER folds it: "CAF" + E acute equals "Caf" + e acute, as in Excel
std::string Lower(std::string_view s) { return fn::Lower(s); }

int TypeRank(const Value& v) {
  if (v.IsNumeric()) return 0;
  if (v.IsString()) return 1;
  if (v.IsBool()) return 2;
  return 0;
}

// values that convert exactly to Decimal (a Decimal itself or an integer within 15 digits)
bool AsDec(const Value& v, core::Dec* out) {
  if (v.IsDecimal()) {
    *out = core::DecFromValue(v);
    return true;
  }
  if (v.IsNumber()) {
    const double d = v.number();
    if (std::floor(d) == d && std::fabs(d) < 1e15) {
      core::Int128 m;
      if (core::Int128::FromDouble(d, &m)) {
        *out = core::Dec{m, 0};
        return true;
      }
    }
  }
  if (v.IsEmpty()) {
    *out = core::Dec{core::Int128(0), 0};
    return true;
  }
  return false;
}

}  // namespace

double Round15(double x) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.15g", x);
  return std::strtod(buf, nullptr);
}

int Compare15(double x, double y) {
  if (x == y) return 0;
  // further apart than two numbers that round to the same 15 digits can be: the plain order (rounding keeps it)
  if (!(std::fabs(x - y) <= std::max(std::fabs(x), std::fabs(y)) * 1e-14)) return x < y ? -1 : 1;
  const double a = Round15(x), b = Round15(y);
  return a == b ? 0 : (a < b ? -1 : 1);
}

bool Evaluator::ToNumber(const Value& v, double* out, Err* err) {
  switch (v.type()) {
    case core::VType::Number: *out = v.number(); return true;
    case core::VType::Decimal: *out = v.AsDouble(); return true;
    case core::VType::Bool: *out = v.boolean() ? 1.0 : 0.0; return true;
    case core::VType::Empty: *out = 0; return true;
    case core::VType::Error: *err = v.error(); return false;
    case core::VType::Pending: *err = Err::GettingData; return false;
    default: {
      const InputParse p = ParseInput(v.str());
      if (p.value.IsNumeric() && !p.typed_only && !LeadingSpaceDate(v.str(), p.format)) {
        *out = p.value.AsDouble();
        return true;
      }
      *err = Err::Value;
      return false;
    }
  }
}

bool Evaluator::LeadingSpaceDate(std::string_view text, std::string_view parsed_format) {
  return !text.empty() && text[0] == ' ' && !parsed_format.empty() && IsDateFormat(parsed_format);
}

bool Evaluator::ToBool(const Value& v, bool* out, Err* err) {
  switch (v.type()) {
    case core::VType::Bool: *out = v.boolean(); return true;
    case core::VType::Number: *out = v.number() != 0; return true;
    case core::VType::Decimal: *out = !v.dec_mantissa().IsZero(); return true;
    case core::VType::Empty: *out = false; return true;
    case core::VType::Error: *err = v.error(); return false;
    default: {
      const std::string l = Lower(v.str());
      if (l == "true") {
        *out = true;
        return true;
      }
      if (l == "false") {
        *out = false;
        return true;
      }
      *err = Err::Value;
      return false;
    }
  }
}

std::string Evaluator::ToText(const Value& v) {
  switch (v.type()) {
    case core::VType::Number: return NumberToText(v.number());
    case core::VType::Decimal: {
      std::string s = core::DecToString(core::DecFromValue(v));
      if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
      }
      return s;
    }
    case core::VType::Bool: return v.boolean() ? "TRUE" : "FALSE";
    case core::VType::Error: return core::ErrorText(v.error());
    case core::VType::Empty: return {};
    case core::VType::Pending: return "#GETTING_DATA";
    default: {
      if (std::string cap; ParseCubeValue(v.str(), &cap)) return cap;  // a CUBEMEMBER / CUBESET value: its caption
      return std::string(v.str());
    }
  }
}

int Evaluator::Compare(const Value& a0, const Value& b0, bool* ok) {
  *ok = true;
  Value a = a0, b = b0;
  // blank: 0, "" or FALSE depending on the other side's type
  if (a.IsEmpty() && b.IsEmpty()) return 0;
  if (a.IsEmpty()) a = b.IsString() ? Value::String("") : (b.IsBool() ? Value::Bool(false) : Value::Number(0));
  if (b.IsEmpty()) b = a.IsString() ? Value::String("") : (a.IsBool() ? Value::Bool(false) : Value::Number(0));
  const int ta = TypeRank(a), tb = TypeRank(b);
  if (ta != tb) return ta < tb ? -1 : 1;
  if (a.IsNumeric()) {
    if (a.IsDecimal() && b.IsDecimal()) return core::DecCompare(core::DecFromValue(a), core::DecFromValue(b));
    // Excel compares with 15 significant digits: 0.1+0.2=0.3 is TRUE
    return Compare15(a.AsDouble(), b.AsDouble());
  }
  if (a.IsString()) {
    // equal when the same ignoring case; otherwise in Excel's order (Windows' word sort: "co-op" > "coop", "_" < "1")
    const std::string x = Lower(a.str()), y = Lower(b.str());
    return CollateLowered(x, y);
  }
  if (a.IsBool()) return a.boolean() == b.boolean() ? 0 : (a.boolean() ? 1 : -1);
  *ok = false;
  return 0;
}

core::Rect Evaluator::ClampRect(int32_t sheet, const core::Rect& r) {
  core::Rect o = r;
  const uint32_t rl = host_.RowLimit(sheet);
  if (o.r1 >= rl) o.r1 = rl - 1;
  if (o.c1 >= core::kMaxCols) o.c1 = core::kMaxCols - 1;
  return o;
}

EV Evaluator::EvalRef(const RefSpec& r) {
  if (r.bad || r.sheet == -2) return EV::Err(Err::Ref);
  const int32_t sh = r.sheet >= 0 ? r.sheet : sheet_;
  const uint32_t rl = host_.RowLimit(sh);
  if (rl == 0) return EV::Err(Err::Ref);
  int64_t r0 = r.r0.Resolve(origin_.row), c0 = r.c0.Resolve(origin_.col);
  if (r.sheet2 == -2 || (r.sheet2 >= 0 && host_.SheetIndex(r.sheet2) <= 0)) return EV::Err(Err::Ref);
  if (r.spill) {
    if (r0 < 0 || c0 < 0 || c0 >= core::kMaxCols || r0 >= int64_t{rl}) return EV::Err(Err::Ref);
    core::Rect sp;
    if (!host_.SpillRange(sh, static_cast<uint32_t>(r0), static_cast<uint32_t>(c0), &sp)) return EV::Err(Err::Ref);
    return EV::R(RangeRef{sh, sp});
  }
  int64_t r1 = (r.is_range ? r.r1 : r.r0).Resolve(origin_.row), c1 = (r.is_range ? r.c1 : r.c0).Resolve(origin_.col);
  if (r.whole_cols) {
    r0 = 0;
    r1 = rl - 1;
  }
  if (r0 > r1) std::swap(r0, r1);
  if (c0 > c1) std::swap(c0, c1);
  if (r0 < 0 || c0 < 0 || c1 >= core::kMaxCols || r0 >= int64_t{rl}) return EV::Err(Err::Ref);
  if (r1 >= int64_t{rl}) r1 = rl - 1;
  return EV::R(RangeRef{sh, core::Rect{static_cast<uint32_t>(r0), static_cast<uint32_t>(c0), static_cast<uint32_t>(r1), static_cast<uint32_t>(c1)}, r.sheet2});
}

Value Evaluator::ToScalar(const EV& e) {
  switch (e.k) {
    case EV::K::Scalar: return e.s;
    case EV::K::Lambda: return Value::Error(Err::Calc);  // a LAMBDA that isn't called
    case EV::K::Array: return e.a && !e.a->v.empty() ? e.a->v[0] : Value::Error(Err::Value);
    case EV::K::Range: {
      if (e.r.sheet2 >= 0) return Value::Error(Err::Value);  // a 3D reference is only for functions such as SUM
      const core::Rect& r = e.r.rect;
      if (r.IsCell()) return host_.Cell(e.r.sheet, r.r0, r.c0);
      // implicit intersection
      if (r.c0 == r.c1 && origin_.row >= r.r0 && origin_.row <= r.r1 && e.r.sheet == sheet_) return host_.Cell(e.r.sheet, origin_.row, r.c0);
      if (r.r0 == r.r1 && origin_.col >= r.c0 && origin_.col <= r.c1 && e.r.sheet == sheet_) return host_.Cell(e.r.sheet, r.r0, origin_.col);
      if (r.c0 == r.c1 && origin_.row >= r.r0 && origin_.row <= r.r1) return host_.Cell(e.r.sheet, origin_.row, r.c0);
      if (r.r0 == r.r1 && origin_.col >= r.c0 && origin_.col <= r.c1) return host_.Cell(e.r.sheet, r.r0, origin_.col);
      return Value::Error(Err::Value);
    }
  }
  return Value::Error(Err::Value);
}

std::shared_ptr<Array> Evaluator::ToArray(const EV& e) {
  if (e.k == EV::K::Array) return e.a;
  auto a = std::make_shared<Array>();
  if (e.k == EV::K::Scalar || e.k == EV::K::Lambda || e.r.sheet2 >= 0) {
    a->rows = a->cols = 1;
    a->v.push_back(e.k == EV::K::Scalar ? e.s : Value::Error(e.k == EV::K::Lambda ? Err::Calc : Err::Value));
    return a;
  }
  core::Rect r = e.r.rect;
  // entire-column and entire-row references are clamped so no 1M-row array is allocated: to the last row the
  // columns themselves hold, not the sheet's used range, which the formula's own spill widens (=B:B*2 in D5 spilled
  // to the used range's end, and calculated again it spilled further: Redo gave another size, doc_fuzz seed 26213328)
  const core::Rect used = host_.UsedRect(e.r.sheet);
  if (r.Rows() > 65536) r.r1 = std::max(r.r0, std::min(r.r1, host_.LastRowIn(e.r.sheet, r.c0, std::min(r.c1, core::kMaxCols - 1))));
  // whole rows are only 16,384 cells a row: kept whole while they fit, as Excel's array is (measured: =A3:XFD4*2 in
  // B10 is #SPILL!, past XFD; in A20 it spills). Cut to the used columns, its size followed what the sheet held, and the
  // spill itself, widening the used range, made Redo spill another size (doc_fuzz --wide seed 20920008)
  if (r.Cols() > 256 && uint64_t{r.Rows()} * r.Cols() > kMaxArrayCells) r.c1 = std::max(r.c0, std::min(r.c1, used.c1));
  if (r.Count() > kMaxArrayCells) return nullptr;
  a->rows = static_cast<uint32_t>(r.Rows());
  a->cols = static_cast<uint32_t>(r.Cols());
  a->v.resize(static_cast<size_t>(a->rows) * a->cols);
  host_.ForEachCell(e.r.sheet, r, [&](uint32_t row, uint32_t col, const Value& v) {
    a->At(row - r.r0, col - r.c0) = v;
    return true;
  });
  return a;
}

void Evaluator::ForEachValue(const EV& e, const std::function<bool(const Value&)>& fn, bool include_empty) {
  switch (e.k) {
    case EV::K::Scalar: fn(e.s); return;
    case EV::K::Lambda: fn(Value::Error(Err::Calc)); return;
    case EV::K::Array:
      for (const Value& v : e.a->v) {
        if (!fn(v)) return;
      }
      return;
    case EV::K::Range:
      if (e.r.sheet2 >= 0) {
        // 3D: the same range on each sheet from the first to the last one, in tab order
        int32_t a = host_.SheetIndex(e.r.sheet), b = host_.SheetIndex(e.r.sheet2);
        if (a <= 0 || b <= 0) return;
        if (a > b) std::swap(a, b);
        bool stop = false;
        for (int32_t i = a; i <= b && !stop; ++i) {
          const int32_t id = host_.SheetIdAt(i);
          if (id < 0) continue;
          ForEachValue(EV::R(RangeRef{id, e.r.rect}), [&](const Value& v) {
            if (fn(v)) return true;
            stop = true;
            return false;
          }, include_empty);
        }
        return;
      }
      if (include_empty) {
        auto a = ToArray(e);
        if (!a) return;
        for (const Value& v : a->v) {
          if (!fn(v)) return;
        }
        return;
      }
      host_.ForEachCell(e.r.sheet, ClampRect(e.r.sheet, e.r.rect), [&](uint32_t, uint32_t, const Value& v) { return fn(v); });
      return;
  }
}

Value Evaluator::Scalar(const Node& n) { return ToScalar(Eval(n)); }

bool Evaluator::ArgNumber(const Node& n, double* out, Value* err) {
  const Value v = Scalar(n);
  if (arg_no_logical_ && v.IsBool()) {  // the Analysis ToolPak's functions (FuncInfo::no_logical)
    *err = Value::Error(Err::Value);
    return false;
  }
  Err e = Err::None;
  if (!ToNumber(v, out, &e)) {
    *err = Value::Error(e);
    return false;
  }
  return true;
}

bool Evaluator::ArgText(const Node& n, std::string* out, Value* err) {
  const Value v = Scalar(n);
  if (v.IsError()) {
    *err = v;
    return false;
  }
  *out = ToText(v);
  return true;
}

bool Evaluator::ArgBool(const Node& n, bool* out, Value* err) {
  const Value v = Scalar(n);
  Err e = Err::None;
  if (!ToBool(v, out, &e)) {
    *err = Value::Error(e);
    return false;
  }
  return true;
}

bool Evaluator::ArgRange(const Node& n, RangeRef* out) {
  const EV e = Eval(n);
  if (e.k != EV::K::Range) return false;
  *out = e.r;
  return true;
}

Value Evaluator::BinaryScalar(char op, const Value& a, const Value& b) {
  if (a.IsError()) return a;
  // arithmetic reads its operands left to right, each as a number: text on the left that isn't one is #VALUE! before
  // the right side's error (Excel 16, measured: ="Qty"*#REF! is #VALUE!, a random sequence's rand_103)
  const bool arithmetic = op == '+' || op == '-' || op == '*' || op == '/' || op == '^';
  if (arithmetic && b.IsError() && a.IsString()) {
    double x;
    Err e = Err::None;
    if (!ToNumber(a, &x, &e)) return Value::Error(e);
  }
  if (b.IsError()) return b;
  if (a.IsPending() || b.IsPending()) return Value::Pending();
  switch (op) {
    case '&': {
      // a text holds 32,767 characters at most: & cuts the joined text there (Excel)
      std::string t = ToText(a) + ToText(b);
      if (t.size() > 32767 && fn::Utf16Length(t) > 32767) t = fn::Utf16Substr(t, 0, 32767);
      return Value::String(std::move(t));
    }
    case '=': case 'N': case '<': case '>': case 'L': case 'G': {
      bool ok;
      const int c = Compare(a, b, &ok);
      switch (op) {
        case '=': return Value::Bool(c == 0);
        case 'N': return Value::Bool(c != 0);
        case '<': return Value::Bool(c < 0);
        case '>': return Value::Bool(c > 0);
        case 'L': return Value::Bool(c <= 0);
        default: return Value::Bool(c >= 0);
      }
    }
    default: break;
  }
  // exact decimal: at least one side is a Decimal and the other converts exactly
  if ((a.IsDecimal() || b.IsDecimal()) && op != '^') {
    core::Dec x, y, r;
    if (AsDec(a, &x) && AsDec(b, &y)) {
      bool ok = false;
      switch (op) {
        case '+': ok = core::DecAdd(x, y, &r); break;
        case '-': ok = core::DecSub(x, y, &r); break;
        case '*': ok = core::DecMul(x, y, &r); break;
        case '/':
          if (y.m.IsZero()) return Value::Error(Err::Div0);
          ok = core::DecDiv(x, y, &r);
          break;
        default: break;
      }
      if (ok) return core::DecToValue(r);
      return Value::Error(Err::Num);
    }
  }
  double x, y;
  Err e = Err::None;
  if (!ToNumber(a, &x, &e) || !ToNumber(b, &y, &e)) return Value::Error(e);
  switch (op) {
    case '+': return Value::Number(x + y);
    case '-': return Value::Number(x - y);
    case '*': return Value::Number(x * y);
    case '/':
      if (y == 0) return Value::Error(Err::Div0);
      return Value::Number(x / y);
    case '^': {
      if (x == 0 && y == 0) return Value::Error(Err::Num);
      if (x == 0 && y < 0) return Value::Error(Err::Div0);
      double r = std::pow(x, y);
      // a negative base takes an exponent whose reciprocal is an odd whole number as that root: (-8)^(1/3) is -2,
      // as in Excel (POWER in fn_math.cpp does the same)
      if (x < 0 && y != std::trunc(y)) {
        const double inv = 1 / y, k = std::round(inv);
        if (std::fabs(inv - k) <= 1e-12 * std::fabs(inv) && std::fmod(k, 2) != 0) r = -std::pow(-x, y);
      }
      if (std::isnan(r)) return Value::Error(Err::Num);
      return Value::Number(r);
    }
    default: return Value::Error(Err::Value);
  }
}

EV Evaluator::MapArray(const EV& a, const std::function<Value(const Value&)>& fn) {
  if (a.k == EV::K::Scalar) return EV::S(fn(a.s));
  if (a.k == EV::K::Range && a.r.rect.IsCell()) return EV::S(fn(ToScalar(a)));
  auto arr = ToArray(a);
  if (!arr) return EV::Err(Err::Num);
  auto out = std::make_shared<Array>(*arr);
  for (Value& v : out->v) v = fn(v);
  return EV::A(out);
}

EV Evaluator::Binary(char op, const EV& a, const EV& b) {
  const bool sa = a.k == EV::K::Scalar || (a.k == EV::K::Range && a.r.rect.IsCell());
  const bool sb = b.k == EV::K::Scalar || (b.k == EV::K::Range && b.r.rect.IsCell());
  if (sa && sb) return EV::S(BinaryScalar(op, ToScalar(a), ToScalar(b)));
  auto x = ToArray(a), y = ToArray(b);
  if (!x || !y) return EV::Err(Err::Num);
  auto out = std::make_shared<Array>();
  out->rows = (x->rows == 1) ? y->rows : (y->rows == 1 ? x->rows : std::max(x->rows, y->rows));
  out->cols = (x->cols == 1) ? y->cols : (y->cols == 1 ? x->cols : std::max(x->cols, y->cols));
  out->v.resize(static_cast<size_t>(out->rows) * out->cols);
  for (uint32_t r = 0; r < out->rows; ++r) {
    for (uint32_t c = 0; c < out->cols; ++c) {
      const bool xin = (x->rows == 1 || r < x->rows) && (x->cols == 1 || c < x->cols);
      const bool yin = (y->rows == 1 || r < y->rows) && (y->cols == 1 || c < y->cols);
      if (!xin || !yin) {
        out->At(r, c) = Value::Error(Err::NA);
        continue;
      }
      const Value& xv = x->At(x->rows == 1 ? 0 : r, x->cols == 1 ? 0 : c);
      const Value& yv = y->At(y->rows == 1 ? 0 : r, y->cols == 1 ? 0 : c);
      out->At(r, c) = BinaryScalar(op, xv, yv);
    }
  }
  return EV::A(out);
}

EV Evaluator::Unary(char op, const EV& a) {
  // Excel's unary plus changes nothing: =+TRUE is TRUE, =+"a" is "a" and =+A1 of an empty A1 is still empty for
  // ISBLANK (measured). It only makes a reference a value (ISREF(+A1) is FALSE). People meet it at every formula
  // typed the 1-2-3 way (=+A1&"x"), which was #VALUE! for a text cell.
  if (op == '+') {
    if (a.k != EV::K::Range) {
      if (!a.refs) return a;
      EV values = a;  // an array of references: its values, as any other operator sees it
      values.refs.reset();
      return values;
    }
    if (a.r.rect.IsCell() && a.r.sheet2 < 0) return EV::S(ToScalar(a));
    auto arr = ToArray(a);
    if (!arr) return EV::Err(Err::Num);
    return EV::A(arr);
  }
  return MapArray(a, [op](const Value& v) -> Value {
    if (v.IsError()) return v;
    if (v.IsDecimal()) {
      core::Dec d = core::DecFromValue(v);
      if (op == '-') d.m = -d.m;
      return core::DecToValue(d);
    }
    double x;
    Err e = Err::None;
    if (!ToNumber(v, &x, &e)) return Value::Error(e);
    return Value::Number(op == '-' ? -x : x);
  });
}

EV Evaluator::Percent(const EV& a) {
  return MapArray(a, [](const Value& v) -> Value {
    if (v.IsError()) return v;
    double x;
    Err e = Err::None;
    if (!ToNumber(v, &x, &e)) return Value::Error(e);
    return Value::Number(x / 100.0);
  });
}

EV Evaluator::Eval(const Node& n) {
  if (frame_.call) {
    const auto& kids = frame_.call->kids;
    const auto addr = reinterpret_cast<uintptr_t>(&n), base = reinterpret_cast<uintptr_t>(kids.data());
    if (addr >= base && addr < base + kids.size() * sizeof(Node)) {
      const size_t i = (addr - base) / sizeof(Node);
      if (frame_.set[i]) return frame_.args[i];
    }
    // anything else is evaluated normally: the frame doesn't apply inside it (e.g. a recursive call of the same formula)
    const ArgFrame saved = frame_;
    frame_ = ArgFrame{};
    struct Restore {
      ArgFrame& f;
      ArgFrame v;
      ~Restore() { f = v; }
    } restore{frame_, saved};
    return EvalNode(n);
  }
  return EvalNode(n);
}

namespace {

const Node& Unparen(const Node& n) { return n.kind == NK::Paren && !n.kids.empty() ? Unparen(n.kids[0]) : n; }
bool IsUnion(const Node& n) {
  const Node& u = Unparen(n);
  return u.kind == NK::Binary && u.op == ',';
}
void FlattenUnion(const Node& n, std::vector<Node>* out) {
  const Node& u = Unparen(n);
  if (u.kind == NK::Binary && u.op == ',') {
    FlattenUnion(u.kids[0], out);
    FlattenUnion(u.kids[1], out);
  } else {
    out->push_back(u);
  }
}

// functions whose arguments all count the same way: a union argument is the same as its areas given one by one
bool TakesAreaLists(std::string_view f) {
  static const char* kNames[] = {"SUM", "SUMSQ", "PRODUCT", "COUNT", "COUNTA", "AVERAGE", "AVERAGEA", "MIN", "MAX", "MINA", "MAXA", "STDEV", "STDEV.S",
                                 "STDEV.P", "STDEVA", "STDEVP", "STDEVPA", "VAR", "VAR.S", "VAR.P", "VARA", "VARP", "VARPA", "MEDIAN", "MODE", "MODE.SNGL",
                                 "GEOMEAN", "HARMEAN", "DEVSQ", "AVEDEV", "KURT", "SKEW", "SKEW.P", "AND", "OR", "XOR", "CONCAT", "TEXTJOIN"};
  for (const char* k : kNames) {
    if (f == k) return true;
  }
  return false;
}

}  // namespace

EV Evaluator::CallFunction(const FuncInfo& fi, const Node& n) {
  // a defined name that is a union stands for the union itself (=SUM(U) with U = A1,A3, as in Excel); LET's and
  // LAMBDA's own names are bindings, not uses
  const bool binds = fi.name == std::string_view("LET") || fi.name == std::string_view("LAMBDA");
  for (size_t i = 0; i < n.kids.size() && !binds; ++i) {
    const Node& k = n.kids[i];
    if (k.kind != NK::Name || (!vars.empty() && k.ref.sheet < 0 && FindVar(k.str))) continue;
    Node u;
    if (!host_.NameUnion(k.str, k.ref.sheet >= 0 ? k.ref.sheet : sheet_, origin_, &u)) continue;
    Node with = n;
    with.kids[i] = std::move(u);
    return CallFunction(fi, with);
  }
  // a union argument (rare): the functions that take lists of areas get its areas one by one
  for (const Node& k : n.kids) {
    if ((k.kind == NK::Paren || k.kind == NK::Binary) && IsUnion(k) && TakesAreaLists(fi.name)) {
      Node flat = n;
      flat.kids.clear();
      for (const Node& x : n.kids) FlattenUnion(x, &flat.kids);
      return CallFunction(fi, flat);
    }
  }
  // what ArgNumber refuses belongs to this call only: a function inside an argument sets its own, and the caller's
  // comes back when this one returns (or stops for a cell that isn't calculated yet)
  struct Restore {
    bool& flag;
    bool saved;
    ~Restore() { flag = saved; }
  } restore{arg_no_logical_, arg_no_logical_};
  arg_no_logical_ = fi.no_logical;
  return fi.ref_mode ? CallRefMode(fi, n) : CallPlain(fi, n);
}

EV Evaluator::CallRefMode(const FuncInfo& fi, const Node& n) {
  // the arguments are evaluated here; without an array of references the call goes on as usual with them (the frame
  // hands them over), with one it runs once per reference (mode 1; the arrays it lifts go along, so
  // COUNTIF(OFFSET(A1,{0;1},0),{1,2}) is 2 x 2, as in Excel) or each reference becomes its first cell (mode 2:
  // SUM(OFFSET(A1,{0,1},0,2)) is A1 + A2, as in Excel)
  const size_t argc = n.kids.size();
  constexpr size_t kInline = 6;
  EV pre_in[kInline];
  char set_in[kInline] = {};
  std::vector<EV> pre_heap;
  std::vector<char> set_heap;
  EV* pre = pre_in;
  char* pset = set_in;
  if (argc > kInline) {
    pre_heap.resize(argc);
    set_heap.assign(argc, 0);
    pre = pre_heap.data();
    pset = set_heap.data();
  }
  bool any = false;
  for (size_t i = 0; i < argc; ++i) {
    if (n.kids[i].kind == NK::Missing) continue;
    pre[i] = Eval(n.kids[i]);
    pset[i] = 1;
    any = any || pre[i].refs != nullptr;
  }
  const ArgFrame saved = frame_;
  struct Restore {
    ArgFrame& f;
    ArgFrame v;
    ~Restore() { f = v; }
  } restore{frame_, saved};
  if (any && fi.ref_mode == 4) {
    // AND / OR / XOR given an array of references are #N/A, whatever it points at; an error in another argument
    // wins, in any order (AND(OFFSET(A1,{0,1},0),1/0) is #DIV/0!, measured)
    for (size_t i = 0; i < argc; ++i) {
      if (!pset[i] || pre[i].refs) continue;
      if (pre[i].k == EV::K::Scalar) {
        if (pre[i].s.IsError()) return pre[i];
        continue;
      }
      Value err;
      ForEachValue(pre[i], [&](const Value& v) {
        if (!v.IsError()) return true;
        err = v;
        return false;
      });
      if (err.IsError()) return EV::S(err);
    }
    return EV::Err(Err::NA);
  }
  if (any && fi.ref_mode == 5) {
    // SUMX2MY2 / SUMX2PY2 / SUMXMY2 given an array of references are #NUM!, after an error in the other argument and
    // after arrays of different sizes (#N/A): SUMX2MY2(OFFSET(A1,{0,1},0),{1,2,3}) is #N/A (measured)
    size_t want = 0;
    bool sized = false, mismatch = false;
    for (size_t i = 0; i < argc; ++i) {
      if (!pset[i]) continue;
      size_t count = 0;
      if (pre[i].refs && pre[i].a) {
        count = pre[i].a->v.size();
      } else if (pre[i].k == EV::K::Scalar) {
        if (pre[i].s.IsError()) return pre[i];
        count = 1;
      } else {
        ForEachValue(
            pre[i],
            [&](const Value&) {
              ++count;
              return true;
            },
            true);  // blank cells count toward the size
      }
      if (sized && count != want) mismatch = true;
      want = count;
      sized = true;
    }
    return EV::Err(mismatch ? Err::NA : Err::Num);
  }
  if (any && (fi.ref_mode == 2 || fi.ref_mode == 3)) {
    // 2: each reference's first cell; 3: a value that isn't a number (SUMPRODUCT(OFFSET(A1,{0,1},0)) is 0, measured);
    // an element that is an error (INDIRECT("Nope!A1")) stays one
    for (size_t i = 0; i < argc; ++i) {
      if (!pre[i].refs || !pre[i].a) continue;
      const RefArray& ra = *pre[i].refs;
      auto vals = std::make_shared<Array>(*pre[i].a);
      for (size_t k = 0; k < ra.refs.size() && k < vals->v.size(); ++k) {
        const RangeRef& r = ra.refs[k];
        if (ra.errs[k] != Err::None) vals->v[k] = Value::Error(ra.errs[k]);
        else vals->v[k] = fi.ref_mode == 2 ? host_.Cell(r.sheet, r.rect.r0, r.rect.c0) : Value::String("");
      }
      pre[i] = EV::A(std::move(vals));
    }
  } else if (any) {
    auto lifts = [&](size_t i) { return ((i < 32 ? fi.lift >> i : fi.lift >> 31) & 1u) != 0; };
    std::vector<std::shared_ptr<Array>> lifted(argc);
    uint32_t rows = 1, cols = 1;
    auto grow = [&](uint32_t r, uint32_t c) {
      rows = rows == 1 ? r : (r == 1 ? rows : std::max(rows, r));
      cols = cols == 1 ? c : (c == 1 ? cols : std::max(cols, c));
    };
    for (size_t i = 0; i < argc; ++i) {
      if (!pset[i]) continue;
      if (pre[i].refs && pre[i].a) {
        grow(pre[i].a->rows, pre[i].a->cols);
      } else if (lifts(i) && (pre[i].k == EV::K::Array || (pre[i].k == EV::K::Range && !pre[i].r.rect.IsCell()))) {
        lifted[i] = ToArray(pre[i]);
        if (!lifted[i]) return EV::Err(Err::Num);
        grow(lifted[i]->rows, lifted[i]->cols);
      }
    }
    if (uint64_t{rows} * cols > kMaxArrayCells) return EV::Err(Err::Num);
    auto out = std::make_shared<Array>();
    out->rows = rows;
    out->cols = cols;
    out->v.resize(static_cast<size_t>(rows) * cols);
    std::vector<EV> elem(pre, pre + argc);
    for (uint32_t r = 0; r < rows; ++r) {
      for (uint32_t c = 0; c < cols; ++c) {
        for (size_t i = 0; i < argc; ++i) {
          const Array* a = pre[i].refs ? pre[i].a.get() : lifted[i].get();
          if (!a) continue;
          const bool in = (a->rows == 1 || r < a->rows) && (a->cols == 1 || c < a->cols);
          const size_t k = static_cast<size_t>(a->rows == 1 ? 0 : r) * a->cols + (a->cols == 1 ? 0 : c);
          if (!in) {
            elem[i] = EV::Err(Err::NA);
          } else if (pre[i].refs) {
            const RefArray& ra = *pre[i].refs;
            elem[i] = ra.errs[k] != Err::None ? EV::Err(ra.errs[k]) : EV::R(ra.refs[k]);
          } else {
            elem[i] = EV::S(a->v[k]);
          }
        }
        frame_ = ArgFrame{&n, elem.data(), pset};
        const EV e = fi.impl(*this, n);
        frame_ = ArgFrame{};
        out->At(r, c) = LiftedElement(e);
      }
    }
    return EV::A(out);
  }
  frame_ = ArgFrame{&n, pre, pset};
  return CallPlain(fi, n, true);
}

// One element of the array a lifted call returns: the function's result for that element, and a blank there is the
// number 0 (Excel 16, measured). COUNT(IF({TRUE,FALSE},,1)) is 2, and with R2 empty CONCAT(IF({1;1;1},R1:R3)) is
// "x0z" and COUNTA(XLOOKUP(Q1:Q3,Q1:Q3,R1:R3)) 3, while the same function called once hands the blank on
// (ISBLANK(IF(TRUE,,)) is TRUE, COUNTA(XLOOKUP(7,Q1:Q3,R1:R3)) 0).
Value Evaluator::LiftedElement(const EV& e) {
  Value v = e.k == EV::K::Array ? (e.a && !e.a->v.empty() ? e.a->v[0] : Value::Error(Err::Value)) : ToScalar(e);
  if (v.IsEmpty()) v = Value::Number(0);
  return v;
}

EV Evaluator::CallPlain(const FuncInfo& fi, const Node& n, bool preset) {
  if (!fi.lift) return fi.impl(*this, n);
  const size_t argc = n.kids.size();
  auto lifted = [&](size_t i) { return ((i < 32 ? fi.lift >> i : fi.lift >> 31) & 1u) != 0; };
  // small calls (almost all) keep the evaluated arguments on the stack
  constexpr size_t kInline = 6;
  EV args_in[kInline];
  char set_in[kInline] = {};
  std::vector<EV> args_heap;
  std::vector<char> set_heap;
  EV* args = args_in;
  char* set = set_in;
  if (argc > kInline) {
    args_heap.resize(argc);
    set_heap.assign(argc, 0);
    args = args_heap.data();
    set = set_heap.data();
  }
  if (preset && frame_.call == &n && !fi.lift_rest) {
    // arguments CallRefMode evaluated already: the ones that aren't lifted are handed over as they are
    for (size_t i = 0; i < argc; ++i) {
      if (!frame_.set[i]) continue;
      args[i] = frame_.args[i];
      set[i] = 1;
    }
  }
  std::vector<std::shared_ptr<Array>> arr;  // only allocated when something is lifted
  uint32_t rows = 1, cols = 1;
  auto take = [&](size_t i) -> bool {
    args[i] = Eval(n.kids[i]);
    set[i] = 1;
    const EV& e = args[i];
    if (!(e.k == EV::K::Array || (e.k == EV::K::Range && !e.r.rect.IsCell()))) return true;
    auto a = ToArray(e);
    if (!a) return false;
    if (arr.empty()) {
      arr.resize(argc);
      rows = a->rows;
      cols = a->cols;
    } else {
      rows = rows == 1 ? a->rows : (a->rows == 1 ? rows : std::max(rows, a->rows));
      cols = cols == 1 ? a->cols : (a->cols == 1 ? cols : std::max(cols, a->cols));
    }
    arr[i] = std::move(a);
    return true;
  };
  for (size_t i = 0; i < argc; ++i) {
    if (lifted(i) && n.kids[i].kind != NK::Missing && !take(i)) return EV::Err(Err::Num);
  }
  const ArgFrame saved = frame_;
  struct Restore {
    ArgFrame& f;
    ArgFrame v;
    ~Restore() { f = v; }
  } restore{frame_, saved};
  if (arr.empty()) {
    // no lifting: the function runs once, reusing the arguments evaluated above
    frame_ = ArgFrame{&n, args, set};
    return fi.impl(*this, n);
  }
  if (fi.lift_rest) {
    for (size_t i = 0; i < argc; ++i) {
      if (!set[i] && n.kids[i].kind != NK::Missing && !take(i)) return EV::Err(Err::Num);
    }
  }
  if (uint64_t{rows} * cols > kMaxArrayCells) return EV::Err(Err::Num);
  auto out = std::make_shared<Array>();
  out->rows = rows;
  out->cols = cols;
  out->v.resize(static_cast<size_t>(rows) * cols);
  std::vector<EV> elem(args, args + argc);
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      for (size_t i = 0; i < argc; ++i) {
        const Array* a = arr[i].get();
        if (!a) continue;
        const bool in = (a->rows == 1 || r < a->rows) && (a->cols == 1 || c < a->cols);
        elem[i] = in ? EV::S(a->At(a->rows == 1 ? 0 : r, a->cols == 1 ? 0 : c)) : EV::Err(Err::NA);
      }
      frame_ = ArgFrame{&n, elem.data(), set};
      const EV e = fi.impl(*this, n);
      frame_ = ArgFrame{};
      out->At(r, c) = LiftedElement(e);
    }
  }
  return EV::A(out);
}

EV Evaluator::EvalNode(const Node& n) {
  if (++depth > 256) {
    --depth;
    return EV::Err(Err::Num);
  }
  struct Guard {
    int& d;
    ~Guard() { --d; }
  } guard{depth};
  switch (n.kind) {
    case NK::Num: return EV::S(Value::Number(n.num));
    case NK::Str: return EV::S(Value::String(n.str));
    case NK::Bool: return EV::S(Value::Bool(n.b));
    case NK::Err: return EV::Err(n.err);
    case NK::Missing: return EV::S(Value());
    case NK::Paren: return Eval(n.kids[0]);
    case NK::Ref: return EvalRef(n.ref);
    case NK::StructRef: {
      RangeRef rr;
      const int e = host_.TableRef(sheet_, n, origin_, &rr);
      if (e) return EV::Err(static_cast<Err>(e));
      return EV::R(rr);
    }
    case NK::Name: {
      if (!vars.empty() && n.ref.sheet < 0) {
        if (const Var* v = FindVar(n.str)) return v->value;
      }
      EV out;
      if (host_.ResolveName(n.str, n.ref.sheet >= 0 ? n.ref.sheet : sheet_, origin_, &out)) return out;
      if (n.ref.sheet >= 0) return EV::Err(Err::Name);
      // a function name used as a value (eta-reduced LAMBDA, like Excel): =GROUPBY(A2:A9, B2:B9, SUM)
      if (const int32_t f = FindFunction(n.str); f >= 0) {
        auto l = std::make_shared<Lambda>();
        l->eta_func = f;
        return EV::L(std::move(l));
      }
      return EV::Err(Err::Name);
    }
    case NK::Func: {
      if (n.func_id >= 0) return CallFunction(GetFunction(n.func_id), n);
      // a LAMBDA in a LET variable or a defined name, otherwise a Python UDF
      std::shared_ptr<const Lambda> fn;
      if (const Var* v = vars.empty() ? nullptr : FindVar(n.str); v && v->value.k == EV::K::Lambda) {
        fn = v->value.fn;
      } else if (!v) {
        EV nm;
        if (host_.ResolveName(n.str, sheet_, origin_, &nm) && nm.k == EV::K::Lambda) fn = nm.fn;
      }
      std::vector<EV> args;
      std::vector<char> omitted;
      args.reserve(n.kids.size());
      for (const Node& k : n.kids) {
        args.push_back(Eval(k));
        omitted.push_back(k.kind == NK::Missing);
      }
      if (fn) return Invoke(*fn, args, &omitted);
      return host_.CallUdf(n.str, args, sheet_, origin_);
    }
    case NK::Call: {
      const EV callee = Eval(n.kids[0]);
      if (callee.k != EV::K::Lambda) return EV::Err(Err::Value);
      std::vector<EV> args;
      std::vector<char> omitted;
      for (size_t i = 1; i < n.kids.size(); ++i) {
        args.push_back(Eval(n.kids[i]));
        omitted.push_back(n.kids[i].kind == NK::Missing);
      }
      return Invoke(*callee.fn, args, &omitted);
    }
    case NK::Unary:
      if (n.op == '@') {
        // Implicit intersection: one value, never an array. Of a reference it is the cell where the reference meets
        // the formula's row or column, still a reference (Excel, measured: ISREF(@R2) is TRUE, COUNT(@R2) of an
        // empty cell 0, and in row 3 ROW(@C1:C10) is 3); cells that don't meet the formula are #VALUE!, a value.
        EV e = Eval(n.kids[0]);
        if (e.k == EV::K::Range && e.r.sheet2 < 0) {
          const core::Rect& r = e.r.rect;
          if (r.IsCell()) return e;
          if (r.c0 == r.c1 && origin_.row >= r.r0 && origin_.row <= r.r1) return EV::R(RangeRef{e.r.sheet, core::Rect::Cell(origin_.row, r.c0)});
          if (r.r0 == r.r1 && origin_.col >= r.c0 && origin_.col <= r.c1) return EV::R(RangeRef{e.r.sheet, core::Rect::Cell(r.r0, origin_.col)});
        }
        return EV::S(ToScalar(e));
      }
      return Unary(n.op, Eval(n.kids[0]));
    case NK::Percent: return Percent(Eval(n.kids[0]));
    case NK::Array: {
      auto a = std::make_shared<Array>();
      a->rows = n.rows;
      a->cols = n.cols;
      for (const Node& k : n.kids) a->v.push_back(ToScalar(Eval(k)));
      return EV::A(a);
    }
    case NK::Binary: {
      if (n.op == ':') {
        const EV a = Eval(n.kids[0]), b = Eval(n.kids[1]);
        if (a.k == EV::K::Range && b.k == EV::K::Range && a.r.sheet == b.r.sheet) return EV::R(RangeRef{a.r.sheet, a.r.rect.Union(b.r.rect)});
        // an end that is an error gives that error, the left one first: =SUM(Foo:A3) over a name nobody defined is
        // #NAME?, INDEX(A1:A3,9):A3 #REF! (Excel 16, measured; it was #VALUE!, and a Replace that made Data!A:A3 of
        // Data!A1:A3 showed #VALUE! until a save as .xlsx and an open: doc_fuzz --xlsx seed 34112022)
        if (a.k == EV::K::Scalar && a.s.IsError()) return a;
        if (b.k == EV::K::Scalar && b.s.IsError()) return b;
        return EV::Err(Err::Value);
      }
      if (n.op == ' ') {  // intersection: A1:C3 B2:D4 is B2:C3; nothing in common is #NULL!
        const EV a = Eval(n.kids[0]), b = Eval(n.kids[1]);
        if (a.k == EV::K::Scalar && a.s.IsError()) return a;
        if (b.k == EV::K::Scalar && b.s.IsError()) return b;
        if (a.k != EV::K::Range || b.k != EV::K::Range) return EV::Err(Err::Value);
        if (a.r.sheet != b.r.sheet || !a.r.rect.Intersects(b.r.rect)) return EV::Err(Err::Null);
        const core::Rect x{std::max(a.r.rect.r0, b.r.rect.r0), std::max(a.r.rect.c0, b.r.rect.c0), std::min(a.r.rect.r1, b.r.rect.r1),
                           std::min(a.r.rect.c1, b.r.rect.c1)};
        return EV::R(RangeRef{a.r.sheet, x});
      }
      // a union read as a value (not passed to a function that takes its areas) is one #VALUE!, as in Excel:
      // =(A1:A3,D1:D3) doesn't spill
      if (n.op == ',') return EV::Err(Err::Value);
      const EV a = Eval(n.kids[0]);
      const EV b = Eval(n.kids[1]);
      return Binary(n.op, a, b);
    }
  }
  return EV::Err(Err::Value);
}

const Var* Evaluator::FindVar(std::string_view name) const {
  for (auto it = vars.rbegin(); it != vars.rend(); ++it) {
    if (it->name.size() != name.size()) continue;
    bool eq = true;
    for (size_t i = 0; i < name.size() && eq; ++i) {
      char ch = name[i];
      if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
      eq = ch == it->name[i];
    }
    if (eq) return &*it;
  }
  return nullptr;
}

namespace {

// the call node of an eta LAMBDA: FUNC(_ETA0, _ETA1, ...) for one function and number of arguments (cached; shared by
// all threads, nodes never move once created)
const Node* EtaBody(int32_t func, size_t argc) {
  static std::mutex mu;
  static std::map<std::pair<int32_t, size_t>, std::unique_ptr<Node>> cache;
  std::lock_guard<std::mutex> lk(mu);
  auto& slot = cache[{func, argc}];
  if (!slot) {
    slot = std::make_unique<Node>();
    slot->kind = NK::Func;
    slot->func_id = func;
    slot->str = GetFunction(func).name;
    for (size_t i = 0; i < argc; ++i) {
      Node k;
      k.kind = NK::Name;
      k.str = "_ETA" + std::to_string(i);
      slot->kids.push_back(std::move(k));
    }
  }
  return slot.get();
}

}  // namespace

EV Evaluator::Invoke(const Lambda& fn, const std::vector<EV>& args, const std::vector<char>* omitted) {
  if (fn.eta_func >= 0) {
    const FuncInfo& fi = GetFunction(fn.eta_func);
    if (static_cast<int>(args.size()) < fi.min_args || (fi.max_args >= 0 && static_cast<int>(args.size()) > fi.max_args)) return EV::Err(Err::Value);
    Lambda call;
    call.body = EtaBody(fn.eta_func, args.size());
    for (size_t i = 0; i < args.size(); ++i) call.params.push_back("_ETA" + std::to_string(i));
    return Invoke(call, args, omitted);
  }
  if (args.size() > fn.params.size() || !fn.body) return EV::Err(Err::Value);
  // too few arguments: a call written in the formula is #VALUE! (LAMBDA(x,y,x)(1); an argument left empty, (1,), is
  // ISOMITTED) unless every parameter left out is an optional one (LAMBDA(x,[y],...)(1)), and a function such as MAP
  // that has fewer values than parameters gets #N/A, optional or not (Excel 16, measured)
  if (args.size() < fn.params.size()) {
    if (!omitted) return EV::Err(Err::NA);
    for (size_t i = args.size(); i < fn.params.size(); ++i) {
      if (i >= fn.optional.size() || !fn.optional[i]) return EV::Err(Err::Value);
    }
  }
  // the body sees the variables the LAMBDA captured plus its parameters (lexical scope), not the caller's
  std::vector<Var> scope = fn.env;
  for (size_t i = 0; i < fn.params.size(); ++i) {
    Var v;
    v.name = fn.params[i];
    v.omitted = i >= args.size() || (omitted && i < omitted->size() && (*omitted)[i]);
    v.value = i < args.size() ? args[i] : EV::S(Value());
    scope.push_back(std::move(v));
  }
  struct Swap {
    std::vector<Var>& vars;
    std::shared_ptr<const void>& owner;
    std::vector<Var> saved_vars;
    std::shared_ptr<const void> saved_owner;
    ~Swap() {
      vars = std::move(saved_vars);
      owner = std::move(saved_owner);
    }
  } swap{vars, owner, std::move(vars), owner};
  vars = std::move(scope);
  owner = fn.owner;
  return Eval(*fn.body);
}

// Excel 97 and later take back the binary rounding of a formula's last step when it is an addition or a subtraction
// that cancels out (KB 78113): =(0.1+0.2)-0.3 and =A1-B1 show 0, not 5.55E-17. Only the formula's top step:
// =1*(0.5-0.4-0.1) and =(0.5-0.4-0.1) (parentheses are a step) keep the residue, and so does =x+0 when x is the
// residue itself. "Cancels" is CancelsToZero: the result's exponent 50 or more below the left operand's (measured;
// "smaller than the larger operand by 2^-48" zeroed results Excel keeps, such as =(1-2^-50)-1 and =2-(2-2^-48)).
EV Evaluator::EvalTop(const Node& root) {
  if (root.kind != NK::Binary || (root.op != '+' && root.op != '-') || root.kids.size() != 2) return Eval(root);
  const EV a = Eval(root.kids[0]);
  const EV b = Eval(root.kids[1]);
  EV e = Binary(root.op, a, b);
  if (e.k != EV::K::Scalar || !e.s.IsNumber() || e.s.number() == 0) return e;
  auto number = [&](const EV& x, double* out) {
    if (x.k != EV::K::Scalar && !(x.k == EV::K::Range && x.r.rect.IsCell() && x.r.sheet2 < 0)) return false;
    const Value v = ToScalar(x);
    if (!v.IsNumber()) return false;
    *out = v.number();
    return true;
  };
  double x = 0, y = 0;
  if (!number(a, &x) || !number(b, &y)) return e;
  if (CancelsToZero(x, e.s.number())) return EV::S(Value::Number(0));
  return e;
}

bool EvaluatesToReference(const Node& root, EvalHost& host, int32_t sheet, core::CellPos origin) {
  Evaluator ev(host, sheet, origin);
  return ev.EvalTop(root).k == EV::K::Range;
}

Value EvaluateCell(const Node& root, EvalHost& host, int32_t sheet, core::CellPos origin, std::shared_ptr<Array>* spill) {
  Evaluator ev(host, sheet, origin);
  const EV e = ev.EvalTop(root);
  if (spill) {
    spill->reset();
    std::shared_ptr<Array> a;
    if (e.k == EV::K::Array) {
      a = e.a;
    } else if (e.k == EV::K::Range && !e.r.rect.IsCell()) {
      a = ev.ToArray(e);  // =A1:B3 spills a copy of the range
      if (!a) return Value::Error(Err::Spill);  // too large to spill
    }
    if (a) {
      if (a->v.empty()) return Value::Error(Err::Calc);  // empty array (e.g. FILTER with nothing found and no fallback)
      if (a->v.size() > 1) *spill = a;
      Value v = a->v[0];
      if (v.IsEmpty()) v = Value::Number(0);
      return v;
    }
  }
  Value v;
  if (e.k == EV::K::Range && !e.r.rect.IsCell()) {
    v = ev.ToScalar(e);  // implicit intersection; the upper-left value when it doesn't apply (dynamic array spill is P2)
    if (v.IsError() && v.error() == Err::Value) v = host.Cell(e.r.sheet, e.r.rect.r0, e.r.rect.c0);
  } else {
    v = ev.ToScalar(e);
  }
  if (v.IsEmpty()) return Value::Number(0);  // =A1 (A1 blank) shows 0
  return v;
}

}  // namespace yc::formula
