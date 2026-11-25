// SPDX-License-Identifier: Apache-2.0
// fn_array.cpp - dynamic array functions: FILTER, SORT, SORTBY, UNIQUE, SEQUENCE, RANDARRAY, VSTACK, HSTACK, TAKE,
// DROP, CHOOSEROWS, CHOOSECOLS, TOCOL, TOROW, TEXTSPLIT, EXPAND, WRAPROWS, WRAPCOLS
//
// They return arrays; a cell formula spills them (see engine/document.h). An empty result is #CALC!, like Excel.
#include "formula/fn_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <random>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace yc::formula::fn {
namespace {

constexpr uint64_t kMaxCells = 4'000'000;

using ArrP = std::shared_ptr<Array>;

ArrP Make(uint32_t rows, uint32_t cols, const Value& fill = Value()) {
  auto a = std::make_shared<Array>();
  a->rows = rows;
  a->cols = cols;
  a->v.assign(static_cast<size_t>(rows) * cols, fill);
  return a;
}

// argument as an array; nullptr (with *err) when it can't be one
ArrP Arg(Evaluator& ev, const Node& call, int i, EV* err) {
  ArrP a = ev.ToArray(ev.Eval(call.kids[static_cast<size_t>(i)]));
  if (!a) *err = Er(Err::Num);
  return a;
}

EV Out(ArrP a) {
  if (!a || a->v.empty()) return Er(Err::Calc);
  return EV::A(std::move(a));
}

// whole number argument (truncated like Excel); false = error in *err. With clamp, a count past 1E+12 is cut to it
// instead of refused: TAKE and DROP take any count, TAKE(Q1:Q3,1E+300) is the whole range (Excel 16, measured)
bool IntArg(Evaluator& ev, const Node& call, int i, int64_t def, int64_t* out, EV* err, bool clamp = false) {
  *out = def;
  if (!Has(call, i)) return true;
  double d;
  Value e;
  if (!ev.ArgNumber(call.kids[static_cast<size_t>(i)], &d, &e)) {
    *err = EV::S(e);
    return false;
  }
  if (clamp && std::isfinite(d)) d = std::clamp(d, -1e12, 1e12);
  if (!std::isfinite(d) || std::fabs(d) > 1e12) {
    *err = Er(Err::Num);
    return false;
  }
  *out = static_cast<int64_t>(std::trunc(d));
  return true;
}

// list of whole numbers from scalars and arrays (CHOOSEROWS indexes, SORT sort_index...)
bool IntList(Evaluator& ev, const Node& n, std::vector<int64_t>* out, EV* err) {
  ArrP a = ev.ToArray(ev.Eval(n));
  if (!a) {
    *err = Er(Err::Num);
    return false;
  }
  for (const Value& v : a->v) {
    double d;
    Err e = Err::None;
    if (!Evaluator::ToNumber(v, &d, &e)) {
      *err = Er(e);
      return false;
    }
    // a number past every array's size stays past it, and inside what int64_t holds: CHOOSEROWS({1;2;3},1E+300)
    // read rows that aren't there (Excel: #VALUE!, as for any row the array doesn't have)
    if (!std::isfinite(d)) {
      *err = Er(Err::Num);
      return false;
    }
    out->push_back(static_cast<int64_t>(std::trunc(std::clamp(d, -1e15, 1e15))));
  }
  return true;
}

// ---- ordering and equality ----

int Rank(const Value& v) {
  if (v.IsNumeric() || v.IsEmpty()) return 0;
  if (v.IsString()) return 1;
  if (v.IsBool()) return 2;
  return 3;  // errors (and pending values) last
}

// SORT / SORTBY put blank cells last whichever the order (Excel: SORT of 5,3,1,blank,blank is 1,3,5,0,0, and so
// is SORT(..., -1) with the blanks still at the end): -1 when only b is blank, 1 when only a is
int BlankLast(const Value& a, const Value& b) {
  const bool ea = a.IsEmpty(), eb = b.IsEmpty();
  return ea == eb ? 0 : ea ? 1 : -1;
}

int SortCompare(const Value& a, const Value& b) {
  const int ra = Rank(a), rb = Rank(b);
  if (ra != rb) return ra < rb ? -1 : 1;
  if (ra == 3) return 0;
  bool ok;
  return Evaluator::Compare(a, b, &ok);
}

// hashable form of a value for UNIQUE (text is case-insensitive like Excel)
void KeyOf(const Value& v, std::string& k) {
  switch (Rank(v)) {
    case 0: {
      const double d = v.IsEmpty() ? 0.0 : v.AsDouble();
      k += 'n';
      k.append(reinterpret_cast<const char*>(&d), sizeof(d));
      break;
    }
    case 1: k += 's'; k += Lower(v.str()); break;
    case 2: k += v.boolean() ? 'T' : 'F'; break;
    default: k += 'e'; k += static_cast<char>(v.IsError() ? static_cast<int>(v.error()) : 0); break;
  }
  k += '\x1f';
}

// ---- FILTER(array, include, [if_empty]) ----
EV FFilter(Evaluator& ev, const Node& call) {
  EV err;
  ArrP a = Arg(ev, call, 0, &err);
  if (!a) return err;
  ArrP inc = Arg(ev, call, 1, &err);
  if (!inc) return err;
  const bool by_rows = inc->cols == 1 && inc->rows == a->rows;
  const bool by_cols = !by_rows && inc->rows == 1 && inc->cols == a->cols;
  if (!by_rows && !by_cols) return Er(Err::Value);
  std::vector<uint32_t> keep;
  const uint32_t n = by_rows ? a->rows : a->cols;
  for (uint32_t i = 0; i < n; ++i) {
    const Value& v = inc->v[i];
    bool b = false;
    Err e = Err::None;
    if (v.IsString()) {
      return Er(Err::Value);
    } else if (!Evaluator::ToBool(v, &b, &e)) {
      return Er(e);
    }
    if (b) keep.push_back(i);
  }
  if (keep.empty()) return Has(call, 2) ? ev.Eval(call.kids[2]) : Er(Err::Calc);
  ArrP o = by_rows ? Make(static_cast<uint32_t>(keep.size()), a->cols) : Make(a->rows, static_cast<uint32_t>(keep.size()));
  for (uint32_t i = 0; i < keep.size(); ++i) {
    if (by_rows) {
      for (uint32_t c = 0; c < a->cols; ++c) o->At(i, c) = a->At(keep[i], c);
    } else {
      for (uint32_t r = 0; r < a->rows; ++r) o->At(r, i) = a->At(r, keep[i]);
    }
  }
  return Out(o);
}

// rearrange rows (or columns) of a by an order
ArrP Reorder(const Array& a, const std::vector<uint32_t>& order, bool by_col) {
  ArrP o = Make(a.rows, a.cols);
  for (uint32_t i = 0; i < order.size(); ++i) {
    if (by_col) {
      for (uint32_t r = 0; r < a.rows; ++r) o->At(r, i) = a.At(r, order[i]);
    } else {
      for (uint32_t c = 0; c < a.cols; ++c) o->At(i, c) = a.At(order[i], c);
    }
  }
  return o;
}

// ---- SORT(array, [sort_index], [sort_order], [by_col]) ----
EV FSort(Evaluator& ev, const Node& call) {
  EV err;
  ArrP a = Arg(ev, call, 0, &err);
  if (!a) return err;
  std::vector<int64_t> idx, ord;
  if (Has(call, 1) && !IntList(ev, call.kids[1], &idx, &err)) return err;
  if (Has(call, 2) && !IntList(ev, call.kids[2], &ord, &err)) return err;
  YC_BOOL_OPT(by_col, 3, false);
  if (idx.empty()) idx.push_back(1);
  if (ord.empty()) ord.push_back(1);
  if (ord.size() != 1 && ord.size() != idx.size()) return Er(Err::Value);
  const uint32_t keys = by_col ? a->rows : a->cols, n = by_col ? a->cols : a->rows;
  for (int64_t k : idx) {
    if (k < 1 || k > keys) return Er(Err::Value);
  }
  for (int64_t o : ord) {
    if (o != 1 && o != -1) return Er(Err::Value);
  }
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0u);
  auto at = [&](uint32_t item, int64_t key) -> const Value& {
    return by_col ? a->At(static_cast<uint32_t>(key - 1), item) : a->At(item, static_cast<uint32_t>(key - 1));
  };
  std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
    for (size_t i = 0; i < idx.size(); ++i) {
      const Value& vx = at(x, idx[i]);
      const Value& vy = at(y, idx[i]);
      if (const int b = BlankLast(vx, vy)) return b < 0;
      const int c = SortCompare(vx, vy);
      if (c != 0) return (ord.size() == 1 ? ord[0] : ord[i]) > 0 ? c < 0 : c > 0;
    }
    return false;
  });
  return Out(Reorder(*a, order, by_col));
}

// ---- SORTBY(array, by_array1, [sort_order1], [by_array2, sort_order2], ...) ----
EV FSortBy(Evaluator& ev, const Node& call) {
  EV err;
  ArrP a = Arg(ev, call, 0, &err);
  if (!a) return err;
  struct Key {
    ArrP by;
    int order = 1;
  };
  std::vector<Key> keys;
  int dir = 0;  // 1 = rows, 2 = columns
  for (int i = 1; i < Argc(call); i += 2) {
    Key k;
    k.by = Arg(ev, call, i, &err);
    if (!k.by) return err;
    if (i + 1 < Argc(call) && Has(call, i + 1)) {
      double o;
      Value e;
      if (!ev.ArgNumber(call.kids[static_cast<size_t>(i + 1)], &o, &e)) return EV::S(e);
      if (o != 1 && o != -1) return Er(Err::Value);
      k.order = static_cast<int>(o);
    }
    const int d = (k.by->cols == 1 && k.by->rows == a->rows) ? 1 : (k.by->rows == 1 && k.by->cols == a->cols) ? 2 : 0;
    if (d == 0 || (dir && d != dir)) return Er(Err::Value);
    dir = d;
    keys.push_back(std::move(k));
  }
  const bool by_col = dir == 2;
  std::vector<uint32_t> order(by_col ? a->cols : a->rows);
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
    for (const Key& k : keys) {
      if (const int b = BlankLast(k.by->v[x], k.by->v[y])) return b < 0;
      const int c = SortCompare(k.by->v[x], k.by->v[y]);
      if (c != 0) return k.order > 0 ? c < 0 : c > 0;
    }
    return false;
  });
  return Out(Reorder(*a, order, by_col));
}

// ---- UNIQUE(array, [by_col], [exactly_once]) ----
EV FUnique(Evaluator& ev, const Node& call) {
  EV err;
  ArrP a = Arg(ev, call, 0, &err);
  if (!a) return err;
  YC_BOOL_OPT(by_col, 1, false);
  YC_BOOL_OPT(once, 2, false);
  const uint32_t n = by_col ? a->cols : a->rows, len = by_col ? a->rows : a->cols;
  std::vector<std::string> key(n);
  std::unordered_map<std::string, uint32_t> count;
  for (uint32_t i = 0; i < n; ++i) {
    for (uint32_t j = 0; j < len; ++j) KeyOf(by_col ? a->At(j, i) : a->At(i, j), key[i]);
    ++count[key[i]];
  }
  std::vector<uint32_t> keep;
  std::unordered_set<std::string> seen;
  for (uint32_t i = 0; i < n; ++i) {
    if (once ? count[key[i]] == 1 : seen.insert(key[i]).second) keep.push_back(i);
  }
  if (keep.empty()) return Er(Err::Calc);
  ArrP o = by_col ? Make(a->rows, static_cast<uint32_t>(keep.size())) : Make(static_cast<uint32_t>(keep.size()), a->cols);
  for (uint32_t i = 0; i < keep.size(); ++i) {
    for (uint32_t j = 0; j < len; ++j) {
      if (by_col) o->At(j, i) = a->At(j, keep[i]);
      else o->At(i, j) = a->At(keep[i], j);
    }
  }
  return Out(o);
}

// ---- SEQUENCE(rows, [columns], [start], [step]) ----
EV FSequence(Evaluator& ev, const Node& call) {
  EV err;
  int64_t rows, cols;
  if (!IntArg(ev, call, 0, 1, &rows, &err) || !IntArg(ev, call, 1, 1, &cols, &err)) return err;
  YC_NUM_OPT(start, 2, 1);
  YC_NUM_OPT(step, 3, 1);
  if (rows < 0 || cols < 0) return Er(Err::Value);
  if (rows == 0 || cols == 0) return Er(Err::Calc);
  if (static_cast<uint64_t>(rows) * static_cast<uint64_t>(cols) > kMaxCells) return Er(Err::Num);
  ArrP o = Make(static_cast<uint32_t>(rows), static_cast<uint32_t>(cols));
  for (size_t i = 0; i < o->v.size(); ++i) o->v[i] = Value::Number(start + step * static_cast<double>(i));
  return Out(o);
}

std::mt19937_64& Rng() {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  return rng;
}

// ---- RANDARRAY([rows], [columns], [min], [max], [whole_number]) ----
EV FRandArray(Evaluator& ev, const Node& call) {
  EV err;
  int64_t rows, cols;
  if (!IntArg(ev, call, 0, 1, &rows, &err) || !IntArg(ev, call, 1, 1, &cols, &err)) return err;
  YC_NUM_OPT(lo, 2, 0);
  YC_NUM_OPT(hi, 3, 1);
  YC_BOOL_OPT(whole, 4, false);
  if (rows < 0 || cols < 0 || lo > hi) return Er(Err::Value);
  if (rows == 0 || cols == 0) return Er(Err::Calc);
  if (static_cast<uint64_t>(rows) * static_cast<uint64_t>(cols) > kMaxCells) return Er(Err::Num);
  ArrP o = Make(static_cast<uint32_t>(rows), static_cast<uint32_t>(cols));
  if (whole) {
    const long long a = static_cast<long long>(std::ceil(lo)), b = static_cast<long long>(std::floor(hi));
    if (a > b) return Er(Err::Value);
    std::uniform_int_distribution<long long> d(a, b);
    for (Value& v : o->v) v = Value::Number(static_cast<double>(d(Rng())));
  } else {
    std::uniform_real_distribution<double> d(lo, hi);
    for (Value& v : o->v) v = Value::Number(lo == hi ? lo : d(Rng()));
  }
  return Out(o);
}

// ---- VSTACK / HSTACK(array1, [array2], ...) ----
EV Stack(Evaluator& ev, const Node& call, bool vertical) {
  std::vector<ArrP> parts;
  uint64_t along = 0;
  uint32_t across = 0;
  for (int i = 0; i < Argc(call); ++i) {
    EV err;
    ArrP a = Arg(ev, call, i, &err);
    if (!a) return err;
    along += vertical ? a->rows : a->cols;
    across = std::max(across, vertical ? a->cols : a->rows);
    parts.push_back(std::move(a));
  }
  if (along * across > kMaxCells) return Er(Err::Num);
  ArrP o = vertical ? Make(static_cast<uint32_t>(along), across, Value::Error(Err::NA)) : Make(across, static_cast<uint32_t>(along), Value::Error(Err::NA));
  uint32_t off = 0;
  for (const ArrP& a : parts) {
    for (uint32_t r = 0; r < a->rows; ++r) {
      for (uint32_t c = 0; c < a->cols; ++c) {
        if (vertical) o->At(off + r, c) = a->At(r, c);
        else o->At(r, off + c) = a->At(r, c);
      }
    }
    off += vertical ? a->rows : a->cols;
  }
  return Out(o);
}
EV FVStack(Evaluator& ev, const Node& call) { return Stack(ev, call, true); }
EV FHStack(Evaluator& ev, const Node& call) { return Stack(ev, call, false); }

// ---- TAKE / DROP(array, rows, [columns]) ----
// the kept span [b, e) of n items: take > 0 from the start, < 0 from the end; drop removes that span instead
bool Span(int64_t n, int64_t k, bool take, int64_t* b, int64_t* e) {
  if (take) {
    if (k == 0) return false;
    const int64_t m = std::min(n, std::llabs(k));
    *b = k > 0 ? 0 : n - m;
    *e = k > 0 ? m : n;
  } else {
    const int64_t m = std::min(n, std::llabs(k));
    *b = k > 0 ? m : 0;
    *e = k > 0 ? n : n - m;
  }
  return *e > *b;
}

EV TakeDrop(Evaluator& ev, const Node& call, bool take) {
  EV err;
  const EV src = ev.Eval(call.kids[0]);
  if (src.k == EV::K::Range && src.r.sheet2 < 0) {
    // of a reference they give that part of it, still a reference: ROW(TAKE(R1:R3,-1)) is 3, TAKE(Q1:Q3,2):Q3 a
    // range, COUNTBLANK(DROP(R1:R3,1)) works and a whole column keeps its rows (Excel 16, measured)
    const core::Rect rc = ev.ClampRect(src.r.sheet, src.r.rect);
    const int64_t rows = static_cast<int64_t>(rc.Rows()), cols = static_cast<int64_t>(rc.Cols());
    int64_t rk, ck;
    if (!IntArg(ev, call, 1, take ? rows : 0, &rk, &err, true) || !IntArg(ev, call, 2, take ? cols : 0, &ck, &err, true)) return err;
    int64_t r0, r1, c0, c1;
    if (!Span(rows, rk, take, &r0, &r1) || !Span(cols, ck, take, &c0, &c1)) return Er(Err::Calc);
    RangeRef part = src.r;
    part.rect = core::Rect{rc.r0 + static_cast<uint32_t>(r0), rc.c0 + static_cast<uint32_t>(c0), rc.r0 + static_cast<uint32_t>(r1 - 1),
                           rc.c0 + static_cast<uint32_t>(c1 - 1)};
    return EV::R(part);
  }
  ArrP a = ev.ToArray(src);
  if (!a) return Er(Err::Num);
  int64_t rk, ck;
  const int64_t all_r = take ? a->rows : 0, all_c = take ? a->cols : 0;
  if (!IntArg(ev, call, 1, all_r, &rk, &err, true) || !IntArg(ev, call, 2, all_c, &ck, &err, true)) return err;
  int64_t r0, r1, c0, c1;
  if (!Span(a->rows, rk, take, &r0, &r1) || !Span(a->cols, ck, take, &c0, &c1)) return Er(Err::Calc);
  ArrP o = Make(static_cast<uint32_t>(r1 - r0), static_cast<uint32_t>(c1 - c0));
  for (int64_t r = r0; r < r1; ++r) {
    for (int64_t c = c0; c < c1; ++c) o->At(static_cast<uint32_t>(r - r0), static_cast<uint32_t>(c - c0)) = a->At(static_cast<uint32_t>(r), static_cast<uint32_t>(c));
  }
  return Out(o);
}
EV FTake(Evaluator& ev, const Node& call) { return TakeDrop(ev, call, true); }
EV FDrop(Evaluator& ev, const Node& call) { return TakeDrop(ev, call, false); }

// ---- CHOOSEROWS / CHOOSECOLS(array, num1, [num2], ...) ----
EV Choose(Evaluator& ev, const Node& call, bool rows) {
  EV err;
  ArrP a = Arg(ev, call, 0, &err);
  if (!a) return err;
  std::vector<int64_t> pick;
  for (int i = 1; i < Argc(call); ++i) {
    if (!IntList(ev, call.kids[static_cast<size_t>(i)], &pick, &err)) return err;
  }
  const int64_t n = rows ? a->rows : a->cols;
  for (int64_t& p : pick) {
    if (p == 0 || std::llabs(p) > n) return Er(Err::Value);
    p = p > 0 ? p - 1 : n + p;
  }
  ArrP o = rows ? Make(static_cast<uint32_t>(pick.size()), a->cols) : Make(a->rows, static_cast<uint32_t>(pick.size()));
  for (uint32_t i = 0; i < pick.size(); ++i) {
    const uint32_t s = static_cast<uint32_t>(pick[i]);
    if (rows) {
      for (uint32_t c = 0; c < a->cols; ++c) o->At(i, c) = a->At(s, c);
    } else {
      for (uint32_t r = 0; r < a->rows; ++r) o->At(r, i) = a->At(r, s);
    }
  }
  return Out(o);
}
EV FChooseRows(Evaluator& ev, const Node& call) { return Choose(ev, call, true); }
EV FChooseCols(Evaluator& ev, const Node& call) { return Choose(ev, call, false); }

// ---- TOCOL / TOROW(array, [ignore], [scan_by_column]) ----
EV ToLine(Evaluator& ev, const Node& call, bool col) {
  EV err;
  ArrP a = Arg(ev, call, 0, &err);
  if (!a) return err;
  int64_t ignore;
  if (!IntArg(ev, call, 1, 0, &ignore, &err)) return err;
  if (ignore < 0 || ignore > 3) return Er(Err::Value);
  YC_BOOL_OPT(by_col, 2, false);
  std::vector<Value> out;
  auto add = [&](const Value& v) {
    if ((ignore & 1) && v.IsEmpty()) return;
    if ((ignore & 2) && v.IsError()) return;
    out.push_back(v);
  };
  if (by_col) {
    for (uint32_t c = 0; c < a->cols; ++c) {
      for (uint32_t r = 0; r < a->rows; ++r) add(a->At(r, c));
    }
  } else {
    for (const Value& v : a->v) add(v);
  }
  if (out.empty()) return Er(Err::Calc);
  ArrP o = col ? Make(static_cast<uint32_t>(out.size()), 1) : Make(1, static_cast<uint32_t>(out.size()));
  o->v = std::move(out);
  return Out(o);
}
EV FToCol(Evaluator& ev, const Node& call) { return ToLine(ev, call, true); }
EV FToRow(Evaluator& ev, const Node& call) { return ToLine(ev, call, false); }

// ---- EXPAND(array, rows, [columns], [pad_with]) ----
EV FExpand(Evaluator& ev, const Node& call) {
  EV err;
  ArrP a = Arg(ev, call, 0, &err);
  if (!a) return err;
  int64_t rows, cols;
  if (!IntArg(ev, call, 1, a->rows, &rows, &err) || !IntArg(ev, call, 2, a->cols, &cols, &err)) return err;
  if (rows < a->rows || cols < a->cols) return Er(Err::Value);
  if (static_cast<uint64_t>(rows) * static_cast<uint64_t>(cols) > kMaxCells) return Er(Err::Num);
  const Value pad = Has(call, 3) ? ev.Scalar(call.kids[3]) : Value::Error(Err::NA);
  ArrP o = Make(static_cast<uint32_t>(rows), static_cast<uint32_t>(cols), pad);
  for (uint32_t r = 0; r < a->rows; ++r) {
    for (uint32_t c = 0; c < a->cols; ++c) o->At(r, c) = a->At(r, c);
  }
  return Out(o);
}

// ---- WRAPROWS / WRAPCOLS(vector, wrap_count, [pad_with]) ----
EV Wrap(Evaluator& ev, const Node& call, bool rows) {
  EV err;
  ArrP a = Arg(ev, call, 0, &err);
  if (!a) return err;
  if (a->rows != 1 && a->cols != 1) return Er(Err::Value);
  int64_t w;
  if (!IntArg(ev, call, 1, 1, &w, &err)) return err;
  if (w < 1) return Er(Err::Num);
  const Value pad = Has(call, 2) ? ev.Scalar(call.kids[2]) : Value::Error(Err::NA);
  const uint64_t n = a->v.size();
  // a wrap longer than the vector gives the vector, unpadded (WRAPROWS({1,2,3},5) is 1, 2, 3, as Excel)
  if (static_cast<uint64_t>(w) > n) w = static_cast<int64_t>(n);
  if (static_cast<uint64_t>(w) > kMaxCells) return Er(Err::Num);
  const uint32_t lines = static_cast<uint32_t>((n + static_cast<uint64_t>(w) - 1) / static_cast<uint64_t>(w));
  ArrP o = rows ? Make(lines, static_cast<uint32_t>(w), pad) : Make(static_cast<uint32_t>(w), lines, pad);
  for (uint64_t i = 0; i < n; ++i) {
    const uint32_t line = static_cast<uint32_t>(i / static_cast<uint64_t>(w)), pos = static_cast<uint32_t>(i % static_cast<uint64_t>(w));
    if (rows) o->At(line, pos) = a->v[i];
    else o->At(pos, line) = a->v[i];
  }
  return Out(o);
}
EV FWrapRows(Evaluator& ev, const Node& call) { return Wrap(ev, call, true); }
EV FWrapCols(Evaluator& ev, const Node& call) { return Wrap(ev, call, false); }

// ---- TEXTSPLIT(text, col_delimiter, [row_delimiter], [ignore_empty], [match_mode], [pad_with]) ----
bool Delims(Evaluator& ev, const Node& call, int i, std::vector<std::string>* out, EV* err) {
  if (!Has(call, i)) return true;
  ArrP a = Arg(ev, call, i, err);
  if (!a) return false;
  for (const Value& v : a->v) {
    if (v.IsError()) {
      *err = EV::S(v);
      return false;
    }
    // an empty delimiter is #VALUE! (TEXTSPLIT("a,b",",","") and TEXTSPLIT("a,b",{",",""}), as Excel)
    std::string s = Evaluator::ToText(v);
    if (s.empty()) {
      *err = Er(Err::Value);
      return false;
    }
    out->push_back(std::move(s));
  }
  return true;
}

// the length of the longest delimiter found at byte i of hay, 0 for none
size_t DelimAt(const std::string& hay, size_t i, const std::vector<std::string>& delims) {
  size_t best = 0;
  for (const auto& d : delims) {
    if (d.size() > best && hay.compare(i, d.size(), d) == 0) best = d.size();
  }
  return best;
}

// one text split into rows of cells: at each place a column delimiter is tried first, then a row delimiter
// (Excel 16, measured: TEXTSPLIT("a--b","-","--") is a, "", b in one row; TEXTSPLIT("a-b-c","-","-") one row)
std::vector<std::vector<std::string>> SplitGrid(const std::string& s, std::vector<std::string> cd,
                                                std::vector<std::string> rd, bool ci) {
  const std::string hay = ci ? Lower(s) : s;
  if (ci) {
    for (auto& d : cd) d = Lower(d);
    for (auto& d : rd) d = Lower(d);
  }
  std::vector<std::vector<std::string>> grid(1);
  size_t start = 0, i = 0;
  while (i < hay.size()) {
    const size_t c = DelimAt(hay, i, cd);
    const size_t r = c ? 0 : DelimAt(hay, i, rd);
    if (!c && !r) {
      ++i;
      continue;
    }
    grid.back().push_back(s.substr(start, i - start));
    if (r) grid.emplace_back();
    i += c ? c : r;
    start = i;
  }
  grid.back().push_back(s.substr(start));
  return grid;
}

// TEXTSPLIT of one text: its rows and columns, or an error
EV SplitOne(const std::string& text, const std::vector<std::string>& cd, const std::vector<std::string>& rd, bool ci,
            bool ignore_empty, const Value& pad) {
  if (text.empty()) return Er(Err::Value);  // nothing to split (Excel: TEXTSPLIT("",",") is #VALUE!)
  std::vector<std::vector<std::string>> grid;
  size_t width = 0;
  for (std::vector<std::string>& cells : SplitGrid(text, cd, rd, ci)) {
    if (ignore_empty) cells.erase(std::remove_if(cells.begin(), cells.end(), [](const std::string& x) { return x.empty(); }), cells.end());
    if (cells.empty()) continue;
    width = std::max(width, cells.size());
    grid.push_back(std::move(cells));
  }
  if (grid.empty()) return Er(Err::Calc);
  ArrP o = Make(static_cast<uint32_t>(grid.size()), static_cast<uint32_t>(width), pad);
  for (uint32_t r = 0; r < grid.size(); ++r) {
    for (uint32_t c = 0; c < grid[r].size(); ++c) o->At(r, c) = Value::String(grid[r][c]);
  }
  return Out(o);
}

EV FTextSplit(Evaluator& ev, const Node& call) {
  EV err;
  ArrP texts = Arg(ev, call, 0, &err);
  if (!texts) return err;
  std::vector<std::string> cd, rd;
  if (!Delims(ev, call, 1, &cd, &err) || !Delims(ev, call, 2, &rd, &err)) return err;
  if (cd.empty() && rd.empty()) return Er(Err::Value);
  YC_BOOL_OPT(ignore_empty, 3, false);
  int64_t mode;
  if (!IntArg(ev, call, 4, 0, &mode, &err)) return err;
  const Value pad = Has(call, 5) ? ev.Scalar(call.kids[5]) : Value::Error(Err::NA);
  const bool ci = mode == 1;
  if (texts->v.size() == 1) {
    if (texts->v[0].IsError()) return EV::S(texts->v[0]);
    return SplitOne(Evaluator::ToText(texts->v[0]), cd, rd, ci, ignore_empty, pad);
  }
  // several texts: each gives the first cell of its split, in the texts' shape (TEXTSPLIT(A1:A3,",") is the part of
  // each cell before its first comma, as Excel)
  ArrP o = Make(texts->rows, texts->cols);
  for (size_t k = 0; k < texts->v.size(); ++k) {
    const Value& t = texts->v[k];
    if (t.IsError()) {
      o->v[k] = t;
      continue;
    }
    const EV one = SplitOne(Evaluator::ToText(t), cd, rd, ci, ignore_empty, pad);
    if (one.k == EV::K::Scalar) o->v[k] = one.s;
    else if (ArrP a = ev.ToArray(one); a && !a->v.empty()) o->v[k] = a->v[0];
  }
  return Out(o);
}

}  // namespace

void RegisterArray(std::vector<FuncInfo>& t) {
  t.push_back({"FILTER", 2, 3, FFilter});
  t.push_back({"SORT", 1, 4, FSort});
  t.push_back({"SORTBY", 2, -1, FSortBy});
  t.push_back({"UNIQUE", 1, 3, FUnique});
  t.push_back({"SEQUENCE", 1, 4, FSequence});
  t.push_back({"RANDARRAY", 0, 5, FRandArray, true});
  t.push_back({"VSTACK", 1, -1, FVStack});
  t.push_back({"HSTACK", 1, -1, FHStack});
  t.push_back({"TAKE", 2, 3, FTake});
  t.push_back({"DROP", 2, 3, FDrop});
  t.push_back({"CHOOSEROWS", 2, -1, FChooseRows});
  t.push_back({"CHOOSECOLS", 2, -1, FChooseCols});
  t.push_back({"TOCOL", 1, 3, FToCol});
  t.push_back({"TOROW", 1, 3, FToRow});
  t.push_back({"EXPAND", 2, 4, FExpand});
  t.push_back({"WRAPROWS", 2, 3, FWrapRows});
  t.push_back({"WRAPCOLS", 2, 3, FWrapCols});
  t.push_back({"TEXTSPLIT", 2, 6, FTextSplit});
}

}  // namespace yc::formula::fn
