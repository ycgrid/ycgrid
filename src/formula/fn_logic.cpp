// SPDX-License-Identifier: Apache-2.0
// fn_logic.cpp - logical functions (the IF family only evaluates the chosen branch, PLAN §6.4)
#include "formula/fn_util.h"

namespace yc::formula::fn {
namespace {

bool Cond(Evaluator& ev, const Node& n, bool* out, Value* err) {
  const EV e = ev.Eval(n);
  const Value v = ev.ToScalar(e);
  if (v.IsError() || v.IsPending()) {
    *err = v;
    return false;
  }
  if (v.IsString()) {
    // IF("text") -> #VALUE!; but "TRUE"/"FALSE" are accepted
    Err er = Err::None;
    if (!Evaluator::ToBool(v, out, &er)) {
      *err = Value::Error(er);
      return false;
    }
    return true;
  }
  Err er = Err::None;
  if (!Evaluator::ToBool(v, out, &er)) {
    *err = Value::Error(er);
    return false;
  }
  return true;
}

// A branch left empty (IF(c,,x), IF(c,x,)) hands on a blank, as a reference to an empty cell does: the formula shows
// 0, but IF(c,,x)="" is TRUE, &"x" gives "x" and ISBLANK is TRUE (Excel 16, measured). The number 0 there made
// =IF(A1>0,,"n/a")="" FALSE and joined a "0" into texts.
EV FIf(Evaluator& ev, const Node& call) {
  bool c;
  Value err;
  if (!Cond(ev, call.kids[0], &c, &err)) return EV::S(err);
  if (c) return Has(call, 1) ? ev.Eval(call.kids[1]) : (Argc(call) > 1 ? EV::S(Value()) : Bool(true));
  if (Argc(call) < 3) return Bool(false);
  return Has(call, 2) ? ev.Eval(call.kids[2]) : EV::S(Value());
}

EV FIfs(Evaluator& ev, const Node& call) {
  if (Argc(call) % 2) return Er(Err::Value);
  for (int i = 0; i + 1 < Argc(call); i += 2) {
    bool c;
    Value err;
    if (!Cond(ev, call.kids[static_cast<size_t>(i)], &c, &err)) return EV::S(err);
    // a result left empty is the number 0 here, not IF's blank: IFS(TRUE,)&"x" is "0x" (Excel 16, measured)
    if (c) return Has(call, i + 1) ? ev.Eval(call.kids[static_cast<size_t>(i + 1)]) : Num(0);
  }
  return Er(Err::NA);
}

EV FIfError(Evaluator& ev, const Node& call) {
  const EV e = ev.Eval(call.kids[0]);
  const Value v = ev.ToScalar(e);
  if (v.IsError()) return ev.Eval(call.kids[1]);
  return e.k == EV::K::Range ? EV::S(v) : e;
}

EV FIfNa(Evaluator& ev, const Node& call) {
  const EV e = ev.Eval(call.kids[0]);
  const Value v = ev.ToScalar(e);
  if (v.IsError() && v.error() == Err::NA) return ev.Eval(call.kids[1]);
  return e.k == EV::K::Range ? EV::S(v) : e;
}

// AND / OR / XOR: no short circuit (like Excel). Text and blanks in ranges and arrays are skipped. An argument given
// directly: a blank (an argument left empty, IF's empty branch) is FALSE, the text "TRUE" / "FALSE" is that logical,
// and any other text is skipped as in a range, so AND("a",TRUE) is TRUE and AND("a","b"), with no logical at all,
// #VALUE! (Excel 16, measured: AND(,TRUE) is FALSE, OR(TRUE,"a") TRUE, AND(1,"0") TRUE).
EV Logical(Evaluator& ev, const Node& call, int kind) {
  bool any = false, acc = kind == 0;
  int trues = 0;
  for (const Node& k : call.kids) {
    const EV e = ev.Eval(k);
    bool failed = false;
    Value err;
    auto take = [&](const Value& v, bool direct) {
      if (v.IsError()) {
        err = v;
        failed = true;
        return false;
      }
      if (v.IsEmpty() && !direct) return true;
      if (v.IsString() && !direct) return true;
      bool b;
      Err er = Err::None;
      if (!Evaluator::ToBool(v, &b, &er)) {
        if (v.IsString()) return true;
        err = Value::Error(er);
        failed = true;
        return false;
      }
      any = true;
      if (kind == 0) acc = acc && b;
      else if (kind == 1) acc = acc || b;
      if (b) ++trues;
      return true;
    };
    if (e.k == EV::K::Scalar) take(e.s, true);
    else ev.ForEachValue(e, [&](const Value& v) { return take(v, false); });
    if (failed) return EV::S(err);
  }
  if (!any) return Er(Err::Value);
  if (kind == 2) return Bool(trues % 2 == 1);
  return Bool(acc);
}
EV FAnd(Evaluator& ev, const Node& call) { return Logical(ev, call, 0); }
EV FOr(Evaluator& ev, const Node& call) { return Logical(ev, call, 1); }
EV FXor(Evaluator& ev, const Node& call) { return Logical(ev, call, 2); }

EV FNot(Evaluator& ev, const Node& call) {
  bool b;
  Value err;
  if (!Cond(ev, call.kids[0], &b, &err)) return EV::S(err);
  return Bool(!b);
}

EV FTrue(Evaluator&, const Node&) { return Bool(true); }
EV FFalse(Evaluator&, const Node&) { return Bool(false); }

// A blank to compare (an empty cell, an argument left empty) is the number 0: SWITCH(A8,0,"zero","","empty") is
// "zero" for an empty A8, whichever case comes first. A result left empty is the number 0, as in IFS:
// SWITCH(1,1,)&"x" is "0x" (Excel 16, measured).
EV FSwitch(Evaluator& ev, const Node& call) {
  auto blank0 = [](Value v) { return v.IsEmpty() ? Value::Number(0) : v; };
  const Value x = blank0(ev.Scalar(call.kids[0]));
  if (x.IsError()) return EV::S(x);
  auto result = [&](int at) { return Has(call, at) ? ev.Eval(call.kids[static_cast<size_t>(at)]) : Num(0); };
  int i = 1;
  for (; i + 1 < Argc(call); i += 2) {
    const Value c = blank0(ev.Scalar(call.kids[static_cast<size_t>(i)]));
    bool ok;
    if (!c.IsError() && Evaluator::Compare(x, c, &ok) == 0 && ok && TypeEq(x, c)) return result(i + 1);
  }
  if (i < Argc(call)) return result(i);
  return Er(Err::NA);
}

EV FChoose(Evaluator& ev, const Node& call) {
  YC_NUM(idx, 0);
  const int i = static_cast<int>(std::floor(idx));
  if (i < 1 || i >= Argc(call)) return Er(Err::Value);
  return ev.Eval(call.kids[static_cast<size_t>(i)]);
}

}  // namespace

bool TypeEq(const Value& a, const Value& b) {
  return (a.IsNumeric() && b.IsNumeric()) || (a.IsString() && b.IsString()) || (a.IsBool() && b.IsBool());
}

void RegisterLogic(std::vector<FuncInfo>& t) {
  t.push_back({"IF", 1, 3, FIf});
  t.push_back({"IFS", 2, 254, FIfs});
  t.push_back({"IFERROR", 2, 2, FIfError});
  t.push_back({"IFNA", 2, 2, FIfNa});
  t.push_back({"AND", 1, 255, FAnd});
  t.push_back({"OR", 1, 255, FOr});
  t.push_back({"XOR", 1, 255, FXor});
  t.push_back({"NOT", 1, 1, FNot});
  t.push_back({"TRUE", 0, 0, FTrue});
  t.push_back({"FALSE", 0, 0, FFalse});
  t.push_back({"SWITCH", 3, 254, FSwitch});
  t.push_back({"CHOOSE", 2, 255, FChoose});
}

}  // namespace yc::formula::fn
