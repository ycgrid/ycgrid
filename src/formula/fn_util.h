// SPDX-License-Identifier: Apache-2.0
// fn_util.h - small helpers shared by the function implementations (only for formula/fn_*.cpp)
#pragma once

#include "formula/eval.h"
#include "formula/functions.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace yc::formula::fn {

using core::Err;

inline EV Er(Err e) { return EV::Err(e); }
inline EV Num(double d) { return EV::S(Value::Number(d)); }
inline EV Bool(bool b) { return EV::S(Value::Bool(b)); }
inline EV Str(std::string s) { return EV::S(Value::String(s)); }

inline int Argc(const Node& call) { return static_cast<int>(call.kids.size()); }
inline bool Has(const Node& call, int i) { return i < Argc(call) && call.kids[static_cast<size_t>(i)].kind != NK::Missing; }

#define YC_NUM(var, i)                                  \
  double var = 0;                                       \
  {                                                     \
    Value _e;                                           \
    if (!ev.ArgNumber(call.kids[(i)], &var, &_e)) return EV::S(_e); \
  }
#define YC_NUM_OPT(var, i, def)                         \
  double var = (def);                                   \
  if (Has(call, (i))) {                                 \
    Value _e;                                           \
    if (!ev.ArgNumber(call.kids[(i)], &var, &_e)) return EV::S(_e); \
  }
#define YC_TEXT(var, i)                                 \
  std::string var;                                      \
  {                                                     \
    Value _e;                                           \
    if (!ev.ArgText(call.kids[(i)], &var, &_e)) return EV::S(_e); \
  }
#define YC_BOOL_OPT(var, i, def)                        \
  bool var = (def);                                     \
  if (Has(call, (i))) {                                 \
    Value _e;                                           \
    if (!ev.ArgBool(call.kids[(i)], &var, &_e)) return EV::S(_e); \
  }
// YC_NUM_OPT and YC_BOOL_OPT keep the default for an argument left empty as well as for one that isn't there
// (MID("abc",2,) and INDEX(C1:C10,3,) in Excel). For other arguments Excel takes the default only when the argument
// isn't there and reads one left empty as 0 / FALSE: MATCH(5,C1:C10,) is an exact match, VLOOKUP(6,C1:D10,2,) #N/A,
// LOG(100,) #NUM! and NORM.S.DIST(1,) the density (Excel 16, measured per function). These two read it so.
#define YC_NUM_OPT_BLANK0(var, i, def)                  \
  double var = (def);                                   \
  if ((i) < Argc(call)) {                               \
    Value _e;                                           \
    if (!ev.ArgNumber(call.kids[(i)], &var, &_e)) return EV::S(_e); \
  }
#define YC_BOOL_OPT_BLANK0(var, i, def)                 \
  bool var = (def);                                     \
  if ((i) < Argc(call)) {                               \
    Value _e;                                           \
    if (!ev.ArgBool(call.kids[(i)], &var, &_e)) return EV::S(_e); \
  }

// A blank given directly as an argument (one left empty, IF's empty branch, +A8 of an empty cell, a lookup that
// found an empty cell) is the number 0 to the functions that read numbers, while a reference to an empty cell isn't
// there for them: AVERAGE(,4) is 2, COUNT(IF(TRUE,,1)) 1 and LARGE(,1) 0, but COUNT(A8) is 0 and AVERAGE(A8)
// #DIV/0! (Excel 16, measured).
inline EV BlankArgAsZero(EV e) {
  if (e.k == EV::K::Scalar && e.s.IsEmpty()) e.s = Value::Number(0);
  return e;
}

// what CollectNumbers reads besides numbers (its mode)
enum NumsMode : int {
  kNumsPlain = 0,
  // MAXA, MINA, VARA, STDEVA...: in a reference a logical is 1 / 0 and text 0; in an array both are left out
  // (Excel 16, measured: STDEVA({1,TRUE,3}) and VARA({1,"a",3}) see 1 and 3 only)
  kNumsA = 1,
  // AVERAGEA: as kNumsA, but text in an array is 0 as well (AVERAGEA({1,"2"}) is 0.5, AVERAGEA({1,FALSE}) 1)
  kNumsAverageA = 2,
  // PRODUCT: an argument left empty isn't there (PRODUCT(2,,3) is 6), where the others read 0
  kNumsSkipBlankArgs = 4,
};

// argument rules of SUM, AVERAGE etc.: numeric text and logicals given directly are converted, a blank given directly
// is 0 (BlankArgAsZero); ranges and arrays only contribute numbers
// false means an error was found (*err is set)
bool CollectNumbers(Evaluator& ev, const Node& call, std::vector<double>* out, Value* err, int first_arg = 0, int mode = kNumsPlain);

// The numbers MODE, MODE.SNGL and MODE.MULT read: a logical or a text given directly is left out, never read as a
// number (MODE(1,TRUE,TRUE) and MODE(1,"1") are #N/A), and a blank, given directly or as a reference to one empty
// cell, is #VALUE! (MODE(,0), MODE(1,A8,1) with A8 empty; Excel 16, measured).
bool CollectModeNumbers(Evaluator& ev, const Node& call, std::vector<double>* out, Value* err);

// exact SUM: accumulates as Decimal when there are Decimals
EV SumValues(Evaluator& ev, const Node& call);

// the sum of numbers as SUM adds them: plain additions, and the last one is 0 when it cancels (CancelsToZero, with
// the sum before the last number as its left side). SUM(0.1,0.2,-0.3) is 0, not 5.55E-17, while SUM(0.1,0.2,-0.3,0)
// keeps the 5.55E-17 and SUM(1+2^-50,-1,1) is 1.0000000000000009 (Excel 16, measured: a check at every step gave 0
// and 1). AVERAGE, SUBTOTAL 1 / 9 and AGGREGATE 1 / 9 too.
inline double ExcelSum(const std::vector<double>& v) {
  double s = 0, before = 0;
  for (const double x : v) {
    before = s;
    s += x;
  }
  return CancelsToZero(before, s) ? 0 : s;
}

// criteria of the COUNTIF family
struct Criteria {
  int op = 0;  // 0 =, 1 <>, 2 <, 3 <=, 4 >, 5 >=
  Value target;
  bool wildcard = false;
  // an operator with nothing after it: "" matches empty cells and empty texts, "=" truly empty cells only, "<>"
  // everything but truly empty cells (a ="" cell counts), "<" ">" "<=" ">=" nothing (Excel)
  bool blank = false;
  int blank_mode = 0;  // 0 "", 1 "=", 2 "<>", 3 a comparison
  std::string pattern;  // lowercase (wildcards)
  bool Match(const Value& v) const;
};
Criteria ParseCriteria(const Value& c);
bool WildcardMatch(std::string_view pattern, std::string_view text);  // * ? ~, case-insensitive

// visit every cell of a range in order (blanks included), for functions that need aligned positions (SUMIF, COUNTIFS...)
// fn(i, value), i is the row-major index
void ForEachCellAll(Evaluator& ev, const EV& e, const std::function<void(size_t, const Value&)>& fn, size_t* count);

bool TypeEq(const Value& a, const Value& b);  // both numbers / text / logicals
std::string Lower(std::string_view s);
std::string Upper(std::string_view s);
std::string ProperCase(std::string_view s);  // PROPER beyond ASCII (functions.cpp)
size_t Utf16Length(std::string_view s);  // text functions count UTF-16 code units
std::string Utf16Substr(std::string_view s, size_t start, size_t len);  // start is 0-based

void RegisterMath(std::vector<FuncInfo>& t);
void RegisterStat(std::vector<FuncInfo>& t);
void RegisterLogic(std::vector<FuncInfo>& t);
void RegisterLookup(std::vector<FuncInfo>& t);
void RegisterText(std::vector<FuncInfo>& t);
void RegisterDate(std::vector<FuncInfo>& t);
void RegisterInfo(std::vector<FuncInfo>& t);
void RegisterYc(std::vector<FuncInfo>& t);
void RegisterFinancial(std::vector<FuncInfo>& t);
void RegisterDist(std::vector<FuncInfo>& t);
void RegisterArray(std::vector<FuncInfo>& t);
void RegisterLambda(std::vector<FuncInfo>& t);
void RegisterEngineering(std::vector<FuncInfo>& t);
void RegisterDatabase(std::vector<FuncInfo>& t);
void RegisterFinancial2(std::vector<FuncInfo>& t);
void RegisterMath2(std::vector<FuncInfo>& t);
void RegisterGroup(std::vector<FuncInfo>& t);
void RegisterText2(std::vector<FuncInfo>& t);
void RegisterOnline(std::vector<FuncInfo>& t);

// YEARFRAC by day-count basis 0-4 (fn_date.cpp); -1 for an invalid basis
double YearFraction(double start, double end, int basis);

// standard normal distribution (fn_stat.cpp)
double StdNormCdf(double z);
double StdNormPdf(double z);
double StdNormInv(double p);
// numeric pairs of the first two arguments (same-size ranges; pairs with a non-number are skipped); false = #N/A in *err
bool CollectPairs(Evaluator& ev, const Node& call, std::vector<double>* xs, std::vector<double>* ys, Value* err);

}  // namespace yc::formula::fn
