// SPDX-License-Identifier: Apache-2.0
// fn_yc.cpp - [YC] extension functions: exact decimals, nanosecond timestamps, streams (PLAN §5.2a, §13)
#include "formula/fn_util.h"

#include "core/decimal.h"
#include "formula/datetime.h"

#include <cstdio>

namespace yc::formula::fn {
namespace {

using core::Dec;

bool ToDec(const Value& v, Dec* out) {
  if (v.IsDecimal()) {
    *out = core::DecFromValue(v);
    return true;
  }
  if (v.IsNumber()) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.15g", v.number());
    return core::DecParse(buf, out);
  }
  if (v.IsString()) {
    std::string s(v.str());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t k = 0;
    while (k < s.size() && s[k] == ' ') ++k;
    return core::DecParse(std::string_view(s).substr(k), out);
  }
  if (v.IsBool()) {
    *out = Dec{core::Int128(v.boolean() ? 1 : 0), 0};
    return true;
  }
  if (v.IsEmpty()) {
    *out = Dec{core::Int128(0), 0};
    return true;
  }
  return false;
}

// =DECIMAL(text, radix): Excel's conversion of a number written in base 2..36 ("FF", 16 -> 255);
// =DECIMAL(value) [YC]: an exact decimal
EV FDecimal(Evaluator& ev, const Node& call) {
  if (Has(call, 1)) {
    YC_TEXT(t, 0);
    YC_NUM(radix, 1);
    radix = std::trunc(radix);
    if (radix < 2 || radix > 36 || t.size() > 255) return Er(Err::Num);
    double total = 0;
    for (char c : t) {
      const int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : 99;
      if (d >= radix) return Er(Err::Num);
      total = total * radix + d;
    }
    if (!std::isfinite(total)) return Er(Err::Num);
    return EV::S(Value::Number(total));
  }
  const Value v = ev.Scalar(call.kids[0]);
  if (v.IsError()) return EV::S(v);
  Dec d;
  if (!ToDec(v, &d)) return Er(Err::Value);
  return EV::S(core::DecToValue(d));
}

// =DECIMAL.SCALE(value, [places]) [YC]: an exact decimal with that many decimal places
EV FDecimalScale(Evaluator& ev, const Node& call) {
  const Value v = ev.Scalar(call.kids[0]);
  if (v.IsError()) return EV::S(v);
  Dec d;
  if (!ToDec(v, &d)) return Er(Err::Value);
  if (Has(call, 1)) {
    YC_NUM(sc, 1);
    if (sc < 0 || sc > core::kDecMaxScale) return Er(Err::Num);
    if (!core::DecRescale(d, static_cast<int>(sc), &d)) return Er(Err::Num);
  }
  return EV::S(core::DecToValue(d));
}

// =NOW_NS(): current UTC epoch seconds, Decimal(scale 9)
EV FNowNs(Evaluator&, const Node&) { return EV::S(core::DecToValue(Dec{core::Int128(NowEpochNs()), 9})); }

// timestamp -> total nanoseconds (Int128)
bool TsToNs(const Value& v, core::Int128* ns) {
  Dec d;
  if (!ToDec(v, &d)) return false;
  if (!core::DecRescale(d, 9, &d)) return false;
  *ns = d.m;
  return true;
}

// =EPOCH_TO_DATE(ts, [tz hours]): convert to an Excel date serial
EV FEpochToDate(Evaluator& ev, const Node& call) {
  const Value v = ev.Scalar(call.kids[0]);
  if (v.IsError()) return EV::S(v);
  YC_NUM_OPT(tz, 1, 0);
  core::Int128 ns;
  if (!TsToNs(v, &ns)) return Er(Err::Value);
  int64_t rem;
  const core::Int128 days = ns.DivSmall(86400ull * 1000000000ull, &rem);  // whole days + remainder, to avoid precision loss with large numbers
  const double serial = days.ToDouble() + static_cast<double>(rem) / 86400e9 + EpochSerial() + tz / 24.0;
  return Num(serial);
}

// =DATE_TO_EPOCH(date serial, [tz hours]): returns epoch seconds (Decimal, scale 6; a date serial itself is about µs precise)
EV FDateToEpoch(Evaluator& ev, const Node& call) {
  YC_NUM(d, 0);
  YC_NUM_OPT(tz, 1, 0);
  const double days = d - EpochSerial() - tz / 24.0;
  const double whole = std::floor(days);
  const double frac_us = std::round((days - whole) * 86400e6);
  core::Int128 w, f;
  if (!core::Int128::FromDouble(whole * 86400, &w) || !core::Int128::FromDouble(frac_us, &f)) return Er(Err::Num);
  Dec a{w, 0}, b{f, 6}, out;
  if (!core::DecAdd(a, b, &out)) return Er(Err::Num);
  return EV::S(core::DecToValue(out));
}

// =TS_PART(ts, unit): "s" whole seconds, "ms" / "us" / "ns" the sub-second part, "epoch_ms" / "epoch_us" / "epoch_ns" totals
EV FTsPart(Evaluator& ev, const Node& call) {
  const Value v = ev.Scalar(call.kids[0]);
  if (v.IsError()) return EV::S(v);
  YC_TEXT(unit, 1);
  core::Int128 ns;
  if (!TsToNs(v, &ns)) return Er(Err::Value);
  int64_t rem;
  core::Int128 secs = ns.DivSmall(1000000000ull, &rem);
  if (rem < 0) {  // floor
    rem += 1000000000;
    secs = secs - core::Int128(1);
  }
  const std::string u = Lower(unit);
  if (u == "s") return EV::S(core::DecToValue(Dec{secs, 0}));
  if (u == "ms") return Num(static_cast<double>(rem / 1000000));
  if (u == "us") return Num(static_cast<double>(rem / 1000));
  if (u == "ns") return Num(static_cast<double>(rem));
  if (u == "epoch_ns") return EV::S(core::DecToValue(Dec{ns, 0}));
  int64_t r2;
  if (u == "epoch_us") return EV::S(core::DecToValue(Dec{ns.DivSmall(1000, &r2), 0}));
  if (u == "epoch_ms") return EV::S(core::DecToValue(Dec{ns.DivSmall(1000000, &r2), 0}));
  return Er(Err::Value);
}

// =TS_FROM_PARTS(seconds, nanoseconds)
EV FTsFromParts(Evaluator& ev, const Node& call) {
  const Value sv = ev.Scalar(call.kids[0]);
  if (sv.IsError()) return EV::S(sv);
  YC_NUM(nsf, 1);
  Dec s, n{core::Int128(0), 9}, out;
  if (!ToDec(sv, &s)) return Er(Err::Value);
  core::Int128 nsi;
  if (!core::Int128::FromDouble(std::trunc(nsf), &nsi)) return Er(Err::Num);
  n.m = nsi;
  if (!core::DecAdd(s, n, &out)) return Er(Err::Num);
  if (!core::DecRescale(out, 9, &out)) return Er(Err::Num);
  return EV::S(core::DecToValue(out));
}

EV FIsDecimal(Evaluator& ev, const Node& call) { return Bool(ev.Scalar(call.kids[0]).IsDecimal()); }

// streams: =QUOTE(topic, [field]) / =STREAM(name, topic1, ...), the Python @stream of that name
EV StreamImpl(Evaluator& ev, const Node& call, const char* name) {
  std::vector<EV> args;
  args.reserve(call.kids.size());
  for (const Node& k : call.kids) {
    if (k.kind == NK::Missing) {
      args.push_back(EV::S(Value()));
      continue;
    }
    const Value v = ev.Scalar(k);
    if (v.IsError()) return EV::S(v);
    args.push_back(EV::S(v));
  }
  return ev.host().Stream(name, args, ev.sheet(), ev.origin());
}
EV FQuote(Evaluator& ev, const Node& call) { return StreamImpl(ev, call, "QUOTE"); }
EV FStream(Evaluator& ev, const Node& call) { return StreamImpl(ev, call, "STREAM"); }

}  // namespace

void RegisterYc(std::vector<FuncInfo>& t) {
  t.push_back({"DECIMAL", 1, 2, FDecimal});
  t.push_back({"DECIMAL.SCALE", 1, 2, FDecimalScale});
  t.push_back({"NOW_NS", 0, 0, FNowNs, true});
  t.push_back({"EPOCH_TO_DATE", 1, 2, FEpochToDate});
  t.push_back({"DATE_TO_EPOCH", 1, 2, FDateToEpoch});
  t.push_back({"TS_PART", 2, 2, FTsPart});
  t.push_back({"TS_FROM_PARTS", 2, 2, FTsFromParts});
  t.push_back({"ISDECIMAL", 1, 1, FIsDecimal});
  t.push_back({"QUOTE", 1, 8, FQuote});
  t.push_back({"STREAM", 1, 30, FStream});
}

}  // namespace yc::formula::fn
