// SPDX-License-Identifier: Apache-2.0
// fn_db.cpp - database functions: DAVERAGE DCOUNT DCOUNTA DGET DMAX DMIN DPRODUCT DSTDEV DSTDEVP DSUM DVAR DVARP
//
// DFUNC(database, field, criteria) like Excel: the first row of database and criteria holds the labels; conditions in
// one criteria row must all hold (AND), separate rows are alternatives (OR). A text condition without an operator
// matches cells that begin with it ("Dav" matches "Davolio"); "=Dav" matches exactly.
#include "formula/criteria.h"
#include "formula/fn_util.h"

#include <cmath>
#include <numeric>

namespace yc::formula::fn {
namespace {

struct Db {
  std::shared_ptr<Array> data;
  int field = -1;  // column of field, -1 = none given
  std::vector<size_t> rows;  // matching data rows (1-based in data)
};

// false: error in *err
bool Select(Evaluator& ev, const Node& call, bool field_optional, Db* out, EV* err) {
  out->data = ev.ToArray(ev.Eval(call.kids[0]));
  const auto crit = ev.ToArray(ev.Eval(call.kids[2]));
  // criteria are a row of labels and at least one row of conditions: the labels alone are #VALUE! (Excel 16, measured)
  if (!out->data || !crit || out->data->rows < 1 || crit->rows < 2) {
    *err = Er(Err::Value);
    return false;
  }
  const Array& d = *out->data;
  // the field: a label or a 1-based column number
  const bool has_field = Has(call, 1);
  if (has_field) {
    const Value f = ev.Scalar(call.kids[1]);
    if (f.IsError()) {
      *err = EV::S(f);
      return false;
    }
    if (f.IsNumeric()) {
      const double k = std::trunc(f.AsDouble());
      if (k < 1 || k > d.cols) {
        *err = Er(Err::Value);
        return false;
      }
      out->field = static_cast<int>(k) - 1;
    } else if (!(f.IsEmpty() && field_optional)) {
      const std::string name = Lower(Evaluator::ToText(f));
      for (uint32_t c = 0; c < d.cols; ++c) {
        if (Lower(Evaluator::ToText(d.At(0, c))) == name) out->field = static_cast<int>(c);
      }
      if (out->field < 0) {
        *err = Er(Err::Value);
        return false;
      }
    }
  } else if (!field_optional) {
    *err = Er(Err::Value);
    return false;
  }
  const std::vector<char> match = MatchCriteriaRows(d, *crit);
  for (uint32_t r = 1; r < d.rows; ++r) {
    if (match[r - 1]) out->rows.push_back(r);
  }
  return true;
}

bool FieldNumbers(const Db& db, std::vector<double>* v, EV* err) {
  for (size_t r : db.rows) {
    const Value& x = db.data->At(static_cast<uint32_t>(r), static_cast<uint32_t>(db.field));
    if (x.IsError()) {
      *err = EV::S(x);
      return false;
    }
    if (x.IsNumeric()) v->push_back(x.AsDouble());
  }
  return true;
}

// kind: 0 sum, 1 average, 2 max, 3 min, 4 product, 5 stdev, 6 stdevp, 7 var, 8 varp
EV Agg(Evaluator& ev, const Node& call, int kind) {
  Db db;
  EV err;
  if (!Select(ev, call, false, &db, &err)) return err;
  std::vector<double> v;
  if (!FieldNumbers(db, &v, &err)) return err;
  const size_t n = v.size();
  const double sum = std::accumulate(v.begin(), v.end(), 0.0);
  auto var = [&](bool sample) {
    const double m = sum / static_cast<double>(n);
    double s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return s / static_cast<double>(sample ? n - 1 : n);
  };
  switch (kind) {
    case 0: return Num(sum);
    case 1: return n ? Num(sum / static_cast<double>(n)) : Er(Err::Div0);
    case 2: return Num(n ? *std::max_element(v.begin(), v.end()) : 0);
    case 3: return Num(n ? *std::min_element(v.begin(), v.end()) : 0);
    case 4: {
      double p = n ? 1 : 0;
      for (double x : v) p *= x;
      return Num(p);
    }
    case 5: return n > 1 ? Num(std::sqrt(var(true))) : Er(Err::Div0);
    case 6: return n > 0 ? Num(std::sqrt(var(false))) : Er(Err::Div0);
    case 7: return n > 1 ? Num(var(true)) : Er(Err::Div0);
    default: return n > 0 ? Num(var(false)) : Er(Err::Div0);
  }
}

EV FDSum(Evaluator& ev, const Node& call) { return Agg(ev, call, 0); }
EV FDAverage(Evaluator& ev, const Node& call) { return Agg(ev, call, 1); }
EV FDMax(Evaluator& ev, const Node& call) { return Agg(ev, call, 2); }
EV FDMin(Evaluator& ev, const Node& call) { return Agg(ev, call, 3); }
EV FDProduct(Evaluator& ev, const Node& call) { return Agg(ev, call, 4); }
EV FDStdev(Evaluator& ev, const Node& call) { return Agg(ev, call, 5); }
EV FDStdevP(Evaluator& ev, const Node& call) { return Agg(ev, call, 6); }
EV FDVar(Evaluator& ev, const Node& call) { return Agg(ev, call, 7); }
EV FDVarP(Evaluator& ev, const Node& call) { return Agg(ev, call, 8); }

// DCOUNT: numbers in the field (all matching records when field is omitted); DCOUNTA: non-blank cells
EV Count(Evaluator& ev, const Node& call, bool a) {
  Db db;
  EV err;
  if (!Select(ev, call, true, &db, &err)) return err;
  if (db.field < 0) return Num(static_cast<double>(db.rows.size()));
  size_t n = 0;
  for (size_t r : db.rows) {
    const Value& x = db.data->At(static_cast<uint32_t>(r), static_cast<uint32_t>(db.field));
    if (a ? !x.IsEmpty() : x.IsNumeric()) ++n;
  }
  return Num(static_cast<double>(n));
}
EV FDCount(Evaluator& ev, const Node& call) { return Count(ev, call, false); }
EV FDCountA(Evaluator& ev, const Node& call) { return Count(ev, call, true); }

EV FDGet(Evaluator& ev, const Node& call) {
  Db db;
  EV err;
  if (!Select(ev, call, false, &db, &err)) return err;
  if (db.rows.empty()) return Er(Err::Value);
  if (db.rows.size() > 1) return Er(Err::Num);
  return EV::S(db.data->At(static_cast<uint32_t>(db.rows[0]), static_cast<uint32_t>(db.field)));
}

}  // namespace

void RegisterDatabase(std::vector<FuncInfo>& t) {
  t.push_back({"DAVERAGE", 3, 3, FDAverage});
  t.push_back({"DCOUNT", 3, 3, FDCount});
  t.push_back({"DCOUNTA", 3, 3, FDCountA});
  t.push_back({"DGET", 3, 3, FDGet});
  t.push_back({"DMAX", 3, 3, FDMax});
  t.push_back({"DMIN", 3, 3, FDMin});
  t.push_back({"DPRODUCT", 3, 3, FDProduct});
  t.push_back({"DSTDEV", 3, 3, FDStdev});
  t.push_back({"DSTDEVP", 3, 3, FDStdevP});
  t.push_back({"DSUM", 3, 3, FDSum});
  t.push_back({"DVAR", 3, 3, FDVar});
  t.push_back({"DVARP", 3, 3, FDVarP});
}

}  // namespace yc::formula::fn

namespace yc::formula {

std::vector<char> MatchCriteriaRows(const Array& d, const Array& crit) {
  std::vector<char> out(d.rows > 0 ? d.rows - 1 : 0, 0);
  // criteria columns -> database columns (-1 = label not found: such conditions never match)
  std::vector<int> map(crit.cols, -1);
  for (uint32_t c = 0; c < crit.cols; ++c) {
    const std::string label = fn::Lower(Evaluator::ToText(crit.At(0, c)));
    for (uint32_t k = 0; k < d.cols; ++k) {
      if (!label.empty() && fn::Lower(Evaluator::ToText(d.At(0, k))) == label) map[c] = static_cast<int>(k);
    }
  }
  struct Cond {
    int col;
    fn::Criteria crit;
  };
  std::vector<std::vector<Cond>> alts;
  for (uint32_t r = 1; r < crit.rows; ++r) {
    std::vector<Cond> conds;
    for (uint32_t c = 0; c < crit.cols; ++c) {
      Value v = crit.At(r, c);
      if (v.IsEmpty()) continue;
      if (v.IsString()) {
        std::string s = Evaluator::ToText(v);
        if (s.empty()) continue;
        // no operator: "begins with"
        if (s[0] != '<' && s[0] != '>' && s[0] != '=') v = Value::String(s + "*");
      }
      conds.push_back(Cond{map[c], fn::ParseCriteria(v)});
    }
    alts.push_back(std::move(conds));
  }
  if (alts.empty()) alts.emplace_back();  // only labels: every record
  for (uint32_t r = 1; r < d.rows; ++r) {
    bool any = false;
    for (const auto& conds : alts) {
      bool all = true;
      for (const Cond& c : conds) {
        if (c.col < 0 || !c.crit.Match(d.At(r, static_cast<uint32_t>(c.col)))) {
          all = false;
          break;
        }
      }
      if (all) {
        any = true;
        break;
      }
    }
    if (any) out[r - 1] = 1;
  }

  return out;
}

}  // namespace yc::formula
