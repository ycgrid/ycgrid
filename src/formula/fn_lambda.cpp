// SPDX-License-Identifier: Apache-2.0
// fn_lambda.cpp - LET, LAMBDA and the functions that call LAMBDAs: MAP, REDUCE, SCAN, BYROW, BYCOL, MAKEARRAY, ISOMITTED
#include "formula/fn_util.h"

#include <algorithm>

namespace yc::formula::fn {
namespace {

constexpr uint64_t kMaxCells = 4'000'000;

// restores the variable list (also when evaluation throws NeedCell)
struct ScopeGuard {
  std::vector<Var>& vars;
  size_t base;
  ~ScopeGuard() {
    if (vars.size() > base) vars.resize(base);
  }
};

// LET(name1, value1, [name2, value2, ...], calculation)
EV FLet(Evaluator& ev, const Node& call) {
  const int argc = Argc(call);
  if (argc < 3 || argc % 2 == 0) return Er(Err::Value);
  ScopeGuard guard{ev.vars, ev.vars.size()};
  for (int i = 0; i + 1 < argc; i += 2) {
    const Node& name = call.kids[static_cast<size_t>(i)];
    if (name.kind != NK::Name) return Er(Err::Value);
    Var v;
    v.name = Upper(name.str);
    v.value = ev.Eval(call.kids[static_cast<size_t>(i + 1)]);
    ev.vars.push_back(std::move(v));
  }
  return ev.Eval(call.kids.back());
}

// LAMBDA([parameter1, ...], calculation): a function value; calling it evaluates calculation with the arguments.
// A parameter written in brackets, LAMBDA(x,[y],...), is optional (the parser marks its name with Node::b).
EV FLambda(Evaluator& ev, const Node& call) {
  auto fn = std::make_shared<Lambda>();
  for (size_t i = 0; i + 1 < call.kids.size(); ++i) {
    const Node& p = call.kids[i];
    if (p.kind != NK::Name) return Er(Err::Value);
    std::string up = Upper(p.str);
    if (std::find(fn->params.begin(), fn->params.end(), up) != fn->params.end()) return Er(Err::Value);
    fn->params.push_back(std::move(up));
    fn->optional.push_back(p.b ? 1 : 0);
  }
  fn->body = &call.kids.back();
  fn->owner = ev.owner;
  fn->env = ev.vars;
  return EV::L(std::move(fn));
}

// the LAMBDA argument (the last one); nullptr with *err otherwise
std::shared_ptr<const Lambda> LambdaArg(Evaluator& ev, const Node& n, size_t params, EV* err) {
  EV e = ev.Eval(n);
  if (e.k != EV::K::Lambda) {
    *err = e.IsError() ? e : Er(Err::Value);
    return nullptr;
  }
  // a function passed by name takes as many arguments as it accepts (checked when it's called)
  if (e.fn->eta_func < 0 && e.fn->params.size() < params) {
    *err = Er(Err::Value);
    return nullptr;
  }
  return e.fn;
}

// a LAMBDA result of several cells where one value is wanted: BYROW / BYCOL / MAP then return one #CALC! (Excel)
bool Several(const EV& r) {
  return (r.k == EV::K::Array && r.a && r.a->v.size() > 1) || (r.k == EV::K::Range && !r.r.rect.IsCell());
}

// one value from a LAMBDA result (an array of several cells isn't allowed here). An empty result stays empty, an
// element like any other: CONCAT(MAP(R1:R3,LAMBDA(x,x))&"|") is "x||z|" with R2 empty and
// COUNT(MAP({1,2},LAMBDA(x,IF(TRUE,,)))) is 0 (Excel 16, measured); the cells the array spills to show 0
Value One(Evaluator& ev, const EV& r) {
  if (r.k == EV::K::Array && r.a && r.a->v.size() > 1) return Value::Error(Err::Calc);
  if (r.k == EV::K::Range && !r.r.rect.IsCell()) return Value::Error(Err::Calc);
  return ev.ToScalar(r);
}

// An array argument of MAP, REDUCE, SCAN, BYROW and BYCOL. A reference hands its cells (rows, columns) to the LAMBDA
// as references, so ROW(x), SUM(Q1:x), OFFSET(x,...) and COUNTBLANK(row) work inside and an empty cell is empty
// there (COUNT(x) is 0); anything else hands values (Excel 16, measured). A whole column is cut to the used rows,
// as everywhere an array is made of one.
struct Source {
  std::shared_ptr<Array> a;  // the values; with is_ref, a(r, c) is the cell (r0 + r, c0 + c) of the sheet
  bool is_ref = false;
  int32_t sheet = -1;
  uint32_t r0 = 0, c0 = 0;

  bool Read(Evaluator& ev, const Node& n) {
    const EV e = ev.Eval(n);
    a = ev.ToArray(e);
    if (!a) return false;
    if (e.k == EV::K::Range && e.r.sheet2 < 0) {
      is_ref = true;
      sheet = e.r.sheet;
      r0 = e.r.rect.r0;
      c0 = e.r.rect.c0;
    }
    return true;
  }
  // the element at (r, c)
  EV At(uint32_t r, uint32_t c) const {
    if (is_ref) return EV::R(RangeRef{sheet, core::Rect::Cell(r0 + r, c0 + c)});
    return EV::S(a->At(r, c));
  }
  // row i (rows) or column i of the array
  EV Line(uint32_t i, bool rows) const {
    if (is_ref) {
      const core::Rect rc = rows ? core::Rect{r0 + i, c0, r0 + i, c0 + a->cols - 1} : core::Rect{r0, c0 + i, r0 + a->rows - 1, c0 + i};
      return EV::R(RangeRef{sheet, rc});
    }
    const uint32_t len = rows ? a->cols : a->rows;
    auto line = std::make_shared<Array>();
    line->rows = rows ? 1 : len;
    line->cols = rows ? len : 1;
    line->v.resize(len);
    for (uint32_t j = 0; j < len; ++j) line->v[j] = rows ? a->At(i, j) : a->At(j, i);
    return EV::A(std::move(line));
  }
};

// MAP(array1, [array2, ...], lambda)
EV FMap(Evaluator& ev, const Node& call) {
  const int argc = Argc(call);
  EV err;
  auto fn = LambdaArg(ev, call.kids.back(), static_cast<size_t>(argc - 1), &err);
  if (!fn) return err;
  std::vector<Source> in(static_cast<size_t>(argc - 1));
  uint32_t rows = 0, cols = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    if (!in[i].Read(ev, call.kids[i])) return Er(Err::Num);
    rows = std::max(rows, in[i].a->rows);
    cols = std::max(cols, in[i].a->cols);
  }
  if (uint64_t{rows} * cols > kMaxCells) return Er(Err::Num);
  auto out = std::make_shared<Array>();
  out->rows = rows;
  out->cols = cols;
  out->v.resize(static_cast<size_t>(rows) * cols);
  std::vector<EV> args(in.size());
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      for (size_t i = 0; i < in.size(); ++i) {
        const Array& a = *in[i].a;
        const bool ok = (a.rows == 1 || r < a.rows) && (a.cols == 1 || c < a.cols);
        args[i] = ok ? in[i].At(a.rows == 1 ? 0 : r, a.cols == 1 ? 0 : c) : Er(Err::NA);
      }
      const EV res = ev.Invoke(*fn, args);
      if (Several(res)) return Er(Err::Calc);
      out->At(r, c) = One(ev, res);
    }
  }
  return EV::A(out);
}

// REDUCE([initial_value], array, lambda(accumulator, value)) / SCAN(...): SCAN returns every step. Without an
// initial value the first element is the accumulator to start from and the LAMBDA is first called for the second:
// REDUCE(,{2,3,4},LAMBDA(a,x,a*x)) is 24 and SCAN's first step is the first element. REDUCE starts from the element
// itself, a reference when the array is one (ISREF(REDUCE(,Q1:Q1,LAMBDA(a,x,a*x))) is TRUE), SCAN from its value
// (Excel 16, measured)
EV Fold(Evaluator& ev, const Node& call, bool scan) {
  EV err;
  auto fn = LambdaArg(ev, call.kids[2], 2, &err);
  if (!fn) return err;
  const bool has_init = Has(call, 0);
  EV acc = has_init ? ev.Eval(call.kids[0]) : EV::S(Value());
  Source src;
  if (!src.Read(ev, call.kids[1])) return Er(Err::Num);
  const std::shared_ptr<Array>& a = src.a;
  std::shared_ptr<Array> out;
  if (scan) {
    out = std::make_shared<Array>();
    out->rows = a->rows;
    out->cols = a->cols;
    out->v.resize(a->v.size());
  }
  std::vector<EV> args(2);
  size_t first = 0;
  if (!has_init && !a->v.empty()) {
    acc = scan ? EV::S(a->v[0]) : src.At(0, 0);
    if (scan) out->v[0] = a->v[0];
    first = 1;
  }
  for (size_t i = first; i < a->v.size(); ++i) {
    args[0] = acc;
    args[1] = src.At(static_cast<uint32_t>(i / a->cols), static_cast<uint32_t>(i % a->cols));
    acc = ev.Invoke(*fn, args);
    if (scan) out->v[i] = One(ev, acc);
  }
  if (scan) return EV::A(out);
  return acc;  // an empty result stays empty: ISBLANK(REDUCE(0,{2,3},LAMBDA(a,x,IF(TRUE,,)))) is TRUE (measured)
}
EV FReduce(Evaluator& ev, const Node& call) { return Fold(ev, call, false); }
EV FScan(Evaluator& ev, const Node& call) { return Fold(ev, call, true); }

// BYROW / BYCOL(array, lambda(row_or_column))
EV ByLine(Evaluator& ev, const Node& call, bool rows) {
  EV err;
  auto fn = LambdaArg(ev, call.kids[1], 1, &err);
  if (!fn) return err;
  Source src;
  if (!src.Read(ev, call.kids[0])) return Er(Err::Num);
  const uint32_t n = rows ? src.a->rows : src.a->cols;
  auto out = std::make_shared<Array>();
  out->rows = rows ? n : 1;
  out->cols = rows ? 1 : n;
  out->v.resize(n);
  std::vector<EV> args(1);
  for (uint32_t i = 0; i < n; ++i) {
    args[0] = src.Line(i, rows);
    const EV res = ev.Invoke(*fn, args);
    if (Several(res)) return Er(Err::Calc);
    out->v[i] = One(ev, res);
  }
  return EV::A(out);
}
EV FByRow(Evaluator& ev, const Node& call) { return ByLine(ev, call, true); }
EV FByCol(Evaluator& ev, const Node& call) { return ByLine(ev, call, false); }

// MAKEARRAY(rows, columns, lambda(row, column))
EV FMakeArray(Evaluator& ev, const Node& call) {
  YC_NUM(rf, 0);
  YC_NUM(cf, 1);
  EV err;
  auto fn = LambdaArg(ev, call.kids[2], 2, &err);
  if (!fn) return err;
  const int64_t rows = static_cast<int64_t>(rf), cols = static_cast<int64_t>(cf);
  if (rows < 1 || cols < 1) return Er(Err::Value);
  if (static_cast<uint64_t>(rows) * static_cast<uint64_t>(cols) > kMaxCells) return Er(Err::Num);
  auto out = std::make_shared<Array>();
  out->rows = static_cast<uint32_t>(rows);
  out->cols = static_cast<uint32_t>(cols);
  out->v.resize(static_cast<size_t>(rows * cols));
  std::vector<EV> args(2);
  for (uint32_t r = 0; r < out->rows; ++r) {
    for (uint32_t c = 0; c < out->cols; ++c) {
      args[0] = Num(r + 1.0);
      args[1] = Num(c + 1.0);
      out->At(r, c) = One(ev, ev.Invoke(*fn, args));
    }
  }
  return EV::A(out);
}

// ISOMITTED(argument): a LAMBDA parameter that wasn't given
EV FIsOmitted(Evaluator& ev, const Node& call) {
  const Node& n = call.kids[0];
  if (n.kind != NK::Name) return Bool(false);
  const Var* v = ev.FindVar(n.str);
  return Bool(v && v->omitted);
}

}  // namespace

void RegisterLambda(std::vector<FuncInfo>& t) {
  t.push_back({"LET", 3, 253, FLet});
  t.push_back({"LAMBDA", 1, 254, FLambda});
  t.push_back({"MAP", 2, 254, FMap});
  t.push_back({"REDUCE", 3, 3, FReduce});
  t.push_back({"SCAN", 3, 3, FScan});
  t.push_back({"BYROW", 2, 2, FByRow});
  t.push_back({"BYCOL", 2, 2, FByCol});
  t.push_back({"MAKEARRAY", 3, 3, FMakeArray});
  t.push_back({"ISOMITTED", 1, 1, FIsOmitted});
}

}  // namespace yc::formula::fn
