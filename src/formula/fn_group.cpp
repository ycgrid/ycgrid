// SPDX-License-Identifier: Apache-2.0
// fn_group.cpp - GROUPBY, PIVOTBY, PERCENTOF (Excel's dynamic-array summaries)
//
// GROUPBY(row_fields, values, function, [field_headers], [total_depth], [sort_order], [filter_array], [field_relationship])
// PIVOTBY(row_fields, col_fields, values, function, [field_headers], [row_total_depth], [row_sort_order],
//         [col_total_depth], [col_sort_order], [filter_array], [relative_to])
// function is a LAMBDA or a function name (SUM, AVERAGE, COUNT, PERCENTOF...): it gets the values of one group as a
// column; a LAMBDA with two parameters (and PERCENTOF) also gets all the values of its total.
#include "formula/fn_util.h"

#include <algorithm>
#include <map>

namespace yc::formula::fn {
namespace {

using ArrP = std::shared_ptr<Array>;

ArrP MakeArr(uint32_t rows, uint32_t cols) {
  auto a = std::make_shared<Array>();
  a->rows = rows;
  a->cols = cols;
  a->v.assign(static_cast<size_t>(rows) * cols, Value());
  return a;
}

// one value from a LAMBDA result
Value One(Evaluator& ev, const EV& r) {
  if (r.k == EV::K::Array && r.a && r.a->v.size() > 1) return Value::Error(Err::Calc);
  if (r.k == EV::K::Range && !r.r.rect.IsCell()) return Value::Error(Err::Calc);
  return ev.ToScalar(r);
}

ArrP Column(const std::vector<Value>& v) {
  auto a = MakeArr(static_cast<uint32_t>(std::max<size_t>(v.size(), 1)), 1);
  for (size_t i = 0; i < v.size(); ++i) a->v[i] = v[i];
  if (v.empty()) a->v[0] = Value::Error(Err::Calc);
  return a;
}

struct Agg {
  std::shared_ptr<const Lambda> fn;
  bool two = false;  // also pass the total's values
  Value Apply(Evaluator& ev, const std::vector<Value>& group, const std::vector<Value>& all) const {
    std::vector<EV> args{EV::A(Column(group))};
    if (two) args.push_back(EV::A(Column(all)));
    return One(ev, ev.Invoke(*fn, args));
  }
};

bool ReadAgg(Evaluator& ev, const Node& n, Agg* out, EV* err) {
  const EV e = ev.Eval(n);
  if (e.k != EV::K::Lambda) {
    *err = e.IsError() ? e : Er(Err::Value);
    return false;
  }
  out->fn = e.fn;
  if (e.fn->eta_func >= 0) out->two = std::string(GetFunction(e.fn->eta_func).name) == "PERCENTOF";
  else out->two = e.fn->params.size() >= 2;
  return true;
}

int IntOpt(Evaluator& ev, const Node& call, int i, int def, bool* ok, EV* err) {
  if (!Has(call, i)) return def;
  double d;
  Value e;
  if (!ev.ArgNumber(call.kids[static_cast<size_t>(i)], &d, &e)) {
    *err = EV::S(e);
    *ok = false;
    return def;
  }
  return static_cast<int>(std::trunc(d));
}

// a list of keys (the fields of one row)
using Key = std::vector<Value>;
int CmpValue(const Value& a, const Value& b) {
  // numbers < text < logicals < blanks, like Excel's sort
  auto rank = [](const Value& v) { return v.IsNumeric() ? 0 : v.IsString() ? 1 : v.IsBool() ? 2 : v.IsError() ? 3 : 4; };
  const int ra = rank(a), rb = rank(b);
  if (ra != rb) return ra < rb ? -1 : 1;
  bool ok = true;
  return Evaluator::Compare(a, b, &ok);
}
bool KeyLess(const Key& a, const Key& b) {
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    const int c = CmpValue(a[i], b[i]);
    if (c) return c < 0;
  }
  return a.size() < b.size();
}
bool KeyEq(const Key& a, const Key& b) { return !KeyLess(a, b) && !KeyLess(b, a); }

struct Fields {
  ArrP data;
  bool headers = false;
  std::vector<Value> names;  // header labels
};

// field_headers: 0 none, 1 has headers (hidden), 2 none (generate), 3 has headers (shown); -1 = detect
// The detection: the first row of values is text and the second isn't.
int DetectHeaders(const Array& values) {
  if (values.rows < 2) return 0;
  bool text = true, num = false;
  for (uint32_t c = 0; c < values.cols; ++c) {
    text = text && values.At(0, c).IsString();
    num = num || values.At(1, c).IsNumeric();
  }
  return text && num ? 3 : 0;
}

ArrP ArgArr(Evaluator& ev, const Node& n, EV* err) {
  ArrP a = ev.ToArray(ev.Eval(n));
  if (!a || a->v.empty()) *err = Er(Err::Value);
  return a;
}

std::string DefaultName(const char* base, size_t i) { return std::string(base) + " " + std::to_string(i + 1); }

// ---- GROUPBY ----

EV FGroupBy(Evaluator& ev, const Node& call) {
  EV err;
  ArrP rf = ArgArr(ev, call.kids[0], &err);
  if (!rf) return err;
  ArrP vals = ArgArr(ev, call.kids[1], &err);
  if (!vals) return err;
  if (rf->rows != vals->rows) return Er(Err::Value);
  Agg agg;
  if (!ReadAgg(ev, call.kids[2], &agg, &err)) return err;
  bool ok = true;
  int headers = IntOpt(ev, call, 3, -1, &ok, &err);
  const int depth = IntOpt(ev, call, 4, 1, &ok, &err);
  const int sort = IntOpt(ev, call, 5, 1, &ok, &err);
  if (!ok) return err;
  if (headers < 0) headers = DetectHeaders(*vals);
  if (headers > 3) return Er(Err::Value);
  ArrP filter;
  if (Has(call, 6)) {
    filter = ArgArr(ev, call.kids[6], &err);
    if (!filter) return err;
  }
  const uint32_t first = (headers == 1 || headers == 3) ? 1 : 0;
  const bool show = headers >= 2;
  const uint32_t k = rf->cols, m = vals->cols;
  // groups
  std::vector<std::pair<Key, std::vector<std::vector<Value>>>> groups;  // key -> values per value column
  std::vector<std::vector<Value>> all(m);
  for (uint32_t r = first; r < rf->rows; ++r) {
    if (filter) {
      const uint32_t fr = r < filter->rows ? r : 0;
      bool keep = false;
      Err e = Err::None;
      if (!Evaluator::ToBool(filter->At(fr, 0), &keep, &e) || !keep) continue;
    }
    Key key(k);
    for (uint32_t c = 0; c < k; ++c) key[c] = rf->At(r, c);
    auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return KeyEq(g.first, key); });
    if (it == groups.end()) {
      groups.push_back({key, std::vector<std::vector<Value>>(m)});
      it = groups.end() - 1;
    }
    for (uint32_t c = 0; c < m; ++c) {
      it->second[c].push_back(vals->At(r, c));
      all[c].push_back(vals->At(r, c));
    }
  }
  if (groups.empty()) return Er(Err::Calc);
  // aggregates
  struct Row {
    Key key;
    std::vector<Value> out;
  };
  std::vector<Row> rows;
  for (auto& g : groups) {
    Row row{g.first, {}};
    for (uint32_t c = 0; c < m; ++c) row.out.push_back(agg.Apply(ev, g.second[c], all[c]));
    rows.push_back(std::move(row));
  }
  // sort: by a row field (1..k) or a value column (k+1..), negative = descending
  const int sc = std::abs(sort) - 1;
  const bool desc = sort < 0;
  std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
    int c;
    if (sc >= 0 && static_cast<uint32_t>(sc) < k) {
      c = CmpValue(a.key[static_cast<size_t>(sc)], b.key[static_cast<size_t>(sc)]);
      if (c == 0) c = KeyLess(a.key, b.key) ? -1 : KeyLess(b.key, a.key) ? 1 : 0;
    } else if (sc >= 0 && static_cast<uint32_t>(sc) < k + m) {
      c = CmpValue(a.out[static_cast<size_t>(sc) - k], b.out[static_cast<size_t>(sc) - k]);
    } else {
      c = KeyLess(a.key, b.key) ? -1 : KeyLess(b.key, a.key) ? 1 : 0;
    }
    return desc ? c > 0 : c < 0;
  });
  // subtotals (depth 2 with several row fields): after each value of the first field
  std::vector<Row> body;
  if (std::abs(depth) >= 2 && k >= 2) {
    for (size_t i = 0; i < rows.size();) {
      size_t j = i;
      while (j < rows.size() && CmpValue(rows[j].key[0], rows[i].key[0]) == 0) ++j;
      Row sub{Key(k), {}};
      sub.key[0] = rows[i].key[0];
      for (uint32_t c = 0; c < m; ++c) {
        std::vector<Value> part;
        for (const auto& g : groups) {
          if (CmpValue(g.first[0], rows[i].key[0]) == 0) part.insert(part.end(), g.second[c].begin(), g.second[c].end());
        }
        sub.out.push_back(agg.Apply(ev, part, all[c]));
      }
      if (depth < 0) body.push_back(sub);
      for (size_t x = i; x < j; ++x) body.push_back(rows[x]);
      if (depth > 0) body.push_back(sub);
      i = j;
    }
  } else {
    body = rows;
  }
  Row total{Key(k), {}};
  total.key[0] = Value::String("Total");
  for (uint32_t c = 0; c < m; ++c) total.out.push_back(agg.Apply(ev, all[c], all[c]));
  const bool grand = depth != 0;
  if (grand && depth < 0) body.insert(body.begin(), total);
  if (grand && depth > 0) body.push_back(total);
  // output
  const uint32_t out_rows = static_cast<uint32_t>(body.size()) + (show ? 1 : 0);
  ArrP out = MakeArr(out_rows, k + m);
  uint32_t r0 = 0;
  if (show) {
    for (uint32_t c = 0; c < k; ++c) out->At(0, c) = first ? rf->At(0, c) : Value::String(DefaultName("Row Field", c));
    for (uint32_t c = 0; c < m; ++c) out->At(0, k + c) = first ? vals->At(0, c) : Value::String(DefaultName("Value", c));
    r0 = 1;
  }
  for (size_t i = 0; i < body.size(); ++i) {
    for (uint32_t c = 0; c < k; ++c) out->At(r0 + static_cast<uint32_t>(i), c) = body[i].key[c];
    for (uint32_t c = 0; c < m; ++c) out->At(r0 + static_cast<uint32_t>(i), k + c) = body[i].out[c];
  }
  return EV::A(std::move(out));
}

// ---- PIVOTBY ----

EV FPivotBy(Evaluator& ev, const Node& call) {
  EV err;
  ArrP rf = ArgArr(ev, call.kids[0], &err);
  if (!rf) return err;
  ArrP cf = ArgArr(ev, call.kids[1], &err);
  if (!cf) return err;
  ArrP vals = ArgArr(ev, call.kids[2], &err);
  if (!vals) return err;
  if (rf->rows != vals->rows || cf->rows != vals->rows) return Er(Err::Value);
  Agg agg;
  if (!ReadAgg(ev, call.kids[3], &agg, &err)) return err;
  bool ok = true;
  int headers = IntOpt(ev, call, 4, -1, &ok, &err);
  const int row_depth = IntOpt(ev, call, 5, 1, &ok, &err);
  const int row_sort = IntOpt(ev, call, 6, 1, &ok, &err);
  const int col_depth = IntOpt(ev, call, 7, 1, &ok, &err);
  const int col_sort = IntOpt(ev, call, 8, 1, &ok, &err);
  if (!ok) return err;
  if (headers < 0) headers = DetectHeaders(*vals);
  ArrP filter;
  if (Has(call, 9)) {
    filter = ArgArr(ev, call.kids[9], &err);
    if (!filter) return err;
  }
  const uint32_t first = (headers == 1 || headers == 3) ? 1 : 0;
  const uint32_t kr = rf->cols, kc = cf->cols;
  // only the first value column (several value columns: one block per value)
  const uint32_t m = vals->cols;
  std::vector<Key> rkeys, ckeys;
  std::map<std::pair<size_t, size_t>, std::vector<std::vector<Value>>> cells;
  std::vector<std::vector<Value>> all(m);
  for (uint32_t r = first; r < vals->rows; ++r) {
    if (filter) {
      const uint32_t fr = r < filter->rows ? r : 0;
      bool keep = false;
      Err e = Err::None;
      if (!Evaluator::ToBool(filter->At(fr, 0), &keep, &e) || !keep) continue;
    }
    Key rk(kr), ck(kc);
    for (uint32_t c = 0; c < kr; ++c) rk[c] = rf->At(r, c);
    for (uint32_t c = 0; c < kc; ++c) ck[c] = cf->At(r, c);
    auto find = [](std::vector<Key>& keys, const Key& key) {
      for (size_t i = 0; i < keys.size(); ++i) {
        if (KeyEq(keys[i], key)) return i;
      }
      keys.push_back(key);
      return keys.size() - 1;
    };
    const size_t ri = find(rkeys, rk), ci = find(ckeys, ck);
    auto& cell = cells[{ri, ci}];
    if (cell.empty()) cell.resize(m);
    for (uint32_t c = 0; c < m; ++c) {
      cell[c].push_back(vals->At(r, c));
      all[c].push_back(vals->At(r, c));
    }
  }
  if (rkeys.empty()) return Er(Err::Calc);
  // order of the row and column items
  auto order = [](const std::vector<Key>& keys, int sort) {
    std::vector<size_t> o(keys.size());
    for (size_t i = 0; i < o.size(); ++i) o[i] = i;
    std::stable_sort(o.begin(), o.end(), [&](size_t a, size_t b) { return sort < 0 ? KeyLess(keys[b], keys[a]) : KeyLess(keys[a], keys[b]); });
    return o;
  };
  const auto ro = order(rkeys, row_sort), co = order(ckeys, col_sort);
  const bool rtot = row_depth != 0, ctot = col_depth != 0;
  // values of a row item / column item / both, for one value column
  auto gather = [&](int ri, int ci, uint32_t vc) {
    std::vector<Value> out;
    for (const auto& [rc, v] : cells) {
      if ((ri < 0 || static_cast<size_t>(ri) == rc.first) && (ci < 0 || static_cast<size_t>(ci) == rc.second)) out.insert(out.end(), v[vc].begin(), v[vc].end());
    }
    return out;
  };
  const uint32_t head_rows = kc + (m > 1 ? 1 : 0);
  const uint32_t ncols = static_cast<uint32_t>(co.size()) + (ctot ? 1 : 0);
  const uint32_t out_cols = kr + ncols * m;
  const uint32_t out_rows = head_rows + static_cast<uint32_t>(ro.size()) + (rtot ? 1 : 0);
  ArrP out = MakeArr(out_rows, out_cols);
  // column headers
  for (uint32_t j = 0; j < ncols; ++j) {
    for (uint32_t vc = 0; vc < m; ++vc) {
      const uint32_t col = kr + j * m + vc;
      for (uint32_t h = 0; h < kc; ++h) {
        if (j < co.size()) out->At(h, col) = ckeys[co[j]][h];
        else if (h == 0) out->At(h, col) = Value::String("Total");
      }
      if (m > 1) out->At(kc, col) = first ? vals->At(0, vc) : Value::String(DefaultName("Value", vc));
    }
  }
  // rows
  for (uint32_t i = 0; i < out_rows - head_rows; ++i) {
    const bool is_total = i >= ro.size();
    const uint32_t row = head_rows + i;
    for (uint32_t c = 0; c < kr; ++c) {
      if (!is_total) out->At(row, c) = rkeys[ro[i]][c];
      else if (c == 0) out->At(row, c) = Value::String("Total");
    }
    for (uint32_t j = 0; j < ncols; ++j) {
      for (uint32_t vc = 0; vc < m; ++vc) {
        const int ri = is_total ? -1 : static_cast<int>(ro[i]);
        const int ci = j < co.size() ? static_cast<int>(co[j]) : -1;
        const auto g = gather(ri, ci, vc);
        out->At(row, kr + j * m + vc) = g.empty() ? Value() : agg.Apply(ev, g, all[vc]);
      }
    }
  }
  return EV::A(std::move(out));
}

// ---- PERCENTOF(data_subset, data_all) ----

EV FPercentOf(Evaluator& ev, const Node& call) {
  auto sum = [&](const Node& n, double* out, EV* err) {
    const auto a = ev.ToArray(ev.Eval(n));
    if (!a) {
      *err = Er(Err::Value);
      return false;
    }
    double s = 0;
    for (const Value& v : a->v) {
      if (v.IsError()) {
        *err = EV::S(v);
        return false;
      }
      if (v.IsNumeric()) s += v.AsDouble();
    }
    *out = s;
    return true;
  };
  double part, whole;
  EV err;
  if (!sum(call.kids[0], &part, &err) || !sum(call.kids[1], &whole, &err)) return err;
  if (whole == 0) return Er(Err::Div0);
  return Num(part / whole);
}

}  // namespace

void RegisterGroup(std::vector<FuncInfo>& t) {
  t.push_back({"GROUPBY", 3, 8, FGroupBy});
  t.push_back({"PIVOTBY", 4, 11, FPivotBy});
  t.push_back({"PERCENTOF", 2, 2, FPercentOf});
}

}  // namespace yc::formula::fn
