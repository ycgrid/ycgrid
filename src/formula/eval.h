// SPDX-License-Identifier: Apache-2.0
// eval.h - formula evaluation (PLAN §6.4)
//
// Reading a dirty formula cell that hasn't been computed in this cycle throws NeedCell; the scheduler computes it first and retries
// (the deferral of PLAN §7.4; static and dynamic references use the same path, so OFFSET / INDIRECT work naturally).
#pragma once

#include "core/decimal.h"
#include "core/geometry.h"
#include "core/value.h"
#include "formula/ast.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yc::formula {

using core::Value;
struct FuncInfo;

struct NeedCell {
  int32_t sheet;
  uint32_t row, col;
};

struct RangeRef {
  int32_t sheet = -1;  // sheet id
  core::Rect rect;
  int32_t sheet2 = -1;  // 3D reference: the same rect on every sheet from sheet to sheet2 (in tab order)
};

struct Array {
  uint32_t rows = 0, cols = 0;
  std::vector<Value> v;
  const Value& At(uint32_t r, uint32_t c) const { return v[static_cast<size_t>(r) * cols + c]; }
  Value& At(uint32_t r, uint32_t c) { return v[static_cast<size_t>(r) * cols + c]; }
};

struct Lambda;

// an array of references, as OFFSET(A1,{0,1},0) and INDIRECT({"A1","A2"}) give (measured against Excel 16): read as
// values it is #VALUE! in every element (the EV's array); the functions that take a reference take them one by one
// (N, SUBTOTAL, COUNTIF..., FuncInfo::ref_mode 1), the number functions read each one's first cell (SUM, MAX...,
// ref_mode 2) and INDEX picks one of them
struct RefArray {
  std::vector<RangeRef> refs;   // row-major, in the shape of the EV's array
  std::vector<core::Err> errs;  // Err::None, or the error an element is (INDIRECT("Nope!A1") is #REF!)
};

struct EV {
  enum class K : uint8_t { Scalar, Range, Array, Lambda };
  K k = K::Scalar;
  Value s;
  RangeRef r;
  std::shared_ptr<Array> a;
  std::shared_ptr<const Lambda> fn;
  std::shared_ptr<const RefArray> refs;  // with K::Array: the references behind it (see RefArray)

  static EV S(Value v) {
    EV e;
    e.s = std::move(v);
    return e;
  }
  static EV R(RangeRef r) {
    EV e;
    e.k = K::Range;
    e.r = r;
    return e;
  }
  static EV A(std::shared_ptr<Array> a) {
    EV e;
    e.k = K::Array;
    e.a = std::move(a);
    return e;
  }
  static EV L(std::shared_ptr<const Lambda> f) {
    EV e;
    e.k = K::Lambda;
    e.fn = std::move(f);
    return e;
  }
  static EV Err(core::Err err) { return S(Value::Error(err)); }
  bool IsError() const { return k == K::Scalar && s.IsError(); }
};

// a LET / LAMBDA variable
struct Var {
  std::string name;  // uppercase
  EV value;
  bool omitted = false;  // a LAMBDA parameter that wasn't given (ISOMITTED)
};

// a LAMBDA value: its parameters, its body and the variables it captured
struct Lambda {
  std::vector<std::string> params;  // uppercase
  std::vector<char> optional;       // per parameter: written in brackets, LAMBDA(x,[y],...): a call may leave it out
  const Node* body = nullptr;
  std::shared_ptr<const void> owner;  // keeps the body's syntax tree alive (a LAMBDA defined by a name)
  std::vector<Var> env;
  int32_t eta_func = -1;  // a built-in function passed by name (=BYROW(A1:C3, SUM)): called with the given arguments
};

class EvalHost {
 public:
  virtual ~EvalHost() = default;
  virtual const Value& Cell(int32_t sheet, uint32_t r, uint32_t c) = 0;  // may throw NeedCell
  // visit the non-blank cells of a range (with the dirty-formula check); fn returns false to stop
  virtual void ForEachCell(int32_t sheet, const core::Rect& rect, const std::function<bool(uint32_t, uint32_t, const Value&)>& fn) = 0;
  virtual uint32_t RowLimit(int32_t sheet) = 0;
  virtual uint32_t ColLimit(int32_t sheet) { (void)sheet; return core::kMaxCols; }
  virtual core::Rect UsedRect(int32_t sheet) = 0;
  // the last row of columns c0..c1 that holds something (the used range's last row where a host can't tell)
  virtual uint32_t LastRowIn(int32_t sheet, uint32_t c0, uint32_t c1) { (void)c0; (void)c1; return UsedRect(sheet).r1; }
  virtual int32_t FindSheet(std::string_view name) = 0;
  virtual std::string SheetName(int32_t sheet) = 0;
  virtual int32_t SheetIndex(int32_t sheet) = 0;  // 1-based (SHEET function)
  virtual int32_t SheetCount() = 0;
  virtual int32_t SheetIdAt(int32_t index) { (void)index; return -1; }  // 1-based tab position -> id
  virtual bool ResolveName(std::string_view name, int32_t current_sheet, core::CellPos origin, EV* out) = 0;
  // a defined name that is a union (=Data!$A$1,Data!$A$3, kept as ((...))): its parsed formula, so a function that
  // takes the union as a reference (SUM, COUNT, AREAS, INDEX) sees its areas instead of one value (#VALUE!)
  virtual bool NameUnion(std::string_view name, int32_t current_sheet, core::CellPos origin, Node* out) {
    (void)name; (void)current_sheet; (void)origin; (void)out;
    return false;
  }
  virtual EV CallUdf(const std::string& name, const std::vector<EV>& args, int32_t sheet, core::CellPos origin) = 0;
  virtual EV Stream(const std::string& name, const std::vector<EV>& args, int32_t sheet, core::CellPos origin) = 0;
  virtual bool RowHidden(int32_t sheet, uint32_t r) { (void)sheet; (void)r; return false; }
  // WEBSERVICE: the downloaded text (pending until it arrives); #VALUE! without a network provider
  virtual EV WebFetch(const std::string& url) { (void)url; return EV::Err(core::Err::Value); }
  // hidden by an AutoFilter (SUBTOTAL 1-11 skips these too; only rows hidden by hand count)
  virtual bool RowFiltered(int32_t sheet, uint32_t r) { (void)sheet; (void)r; return false; }
  virtual uint32_t CellStyleNumFmtIsDate(int32_t sheet, uint32_t r, uint32_t c) { (void)sheet; (void)r; (void)c; return 0; }
  // what CELL("format" / "prefix" / "protect" / "width") reads: the number format ("" = General), the horizontal
  // alignment (model::HAlign's number), the lock and the column's width in characters
  struct CellLook {
    std::string numfmt;
    uint8_t halign = 0;
    bool locked = true;
    double width = 8.43;
  };
  virtual CellLook LookOf(int32_t sheet, uint32_t r, uint32_t c) { (void)sheet; (void)r; (void)c; return {}; }
  virtual std::string FormulaText(int32_t sheet, uint32_t r, uint32_t c) { (void)sheet; (void)r; (void)c; return {}; }
  virtual bool HasFormula(int32_t sheet, uint32_t r, uint32_t c) { (void)sheet; (void)r; (void)c; return false; }
  // structured reference (NK::StructRef) at origin -> range; returns 0 or an error code (core::Err as int)
  virtual int TableRef(int32_t sheet, const Node& n, core::CellPos origin, RangeRef* out) { (void)sheet; (void)n; (void)origin; (void)out; return 4; }
  // A1#: the current spill range of the anchor (r,c); false when it doesn't spill (may throw NeedCell like Cell)
  virtual bool SpillRange(int32_t sheet, uint32_t r, uint32_t c, core::Rect* out) { (void)sheet; (void)r; (void)c; (void)out; return false; }
  // GETPIVOTDATA: the PivotTable covering (r,c); false = #REF!
  virtual bool PivotData(int32_t sheet, uint32_t r, uint32_t c, const std::string& data_field, const std::vector<std::pair<std::string, std::string>>& pairs,
                         Value* out) {
    (void)sheet, (void)r, (void)c, (void)data_field, (void)pairs, (void)out;
    return false;
  }
};

class Evaluator {
 public:
  Evaluator(EvalHost& host, int32_t sheet, core::CellPos origin) : host_(host), sheet_(sheet), origin_(origin) {}

  EV Eval(const Node& n);
  // a cell formula's own result: a last addition or subtraction that cancels out is 0, as in Excel (below)
  EV EvalTop(const Node& root);
  Value Scalar(const Node& n);            // implicit intersection
  Value ToScalar(const EV& e);
  std::shared_ptr<Array> ToArray(const EV& e);  // range -> array (entire-column references clamped to the used range)

  // type conversion (Excel rules)
  static bool ToNumber(const Value& v, double* out, core::Err* err);  // blank = 0, text is parsed if possible
  // text read as a date or time after a leading space: Excel converts it only in VALUE, DATEVALUE and TIMEVALUE
  // (HOUR(" 12:00") and " 1/2"+0 are #VALUE!, HOUR("12:00 ") is 12, " 5"+0 is 5); parsed_format is ParseInput's
  static bool LeadingSpaceDate(std::string_view text, std::string_view parsed_format);
  static bool ToBool(const Value& v, bool* out, core::Err* err);
  static std::string ToText(const Value& v);
  static int Compare(const Value& a, const Value& b, bool* ok);       // -1/0/1; text is case-insensitive

  // common helpers for functions
  bool ArgNumber(const Node& n, double* out, Value* err);
  bool ArgText(const Node& n, std::string* out, Value* err);
  bool ArgBool(const Node& n, bool* out, Value* err);
  bool ArgRange(const Node& n, RangeRef* out);  // the argument must be a reference
  // visit every value of a range or array (ranges only visit non-blank cells unless include_empty)
  void ForEachValue(const EV& e, const std::function<bool(const Value&)>& fn, bool include_empty = false);

  EvalHost& host() { return host_; }
  int32_t sheet() const { return sheet_; }
  core::CellPos origin() const { return origin_; }
  core::Rect ClampRect(int32_t sheet, const core::Rect& r);

  int depth = 0;

  // LET / LAMBDA variables, innermost last
  std::vector<Var> vars;
  // owner of the syntax tree being evaluated, captured by LAMBDAs created here (null for cell formulas)
  std::shared_ptr<const void> owner;
  const Var* FindVar(std::string_view name) const;  // case-insensitive
  // call a LAMBDA; omitted[i] marks arguments left empty; #VALUE! for too many arguments
  EV Invoke(const Lambda& fn, const std::vector<EV>& args, const std::vector<char>* omitted = nullptr);

 private:
  EV EvalNode(const Node& n);
  EV CallFunction(const FuncInfo& fi, const Node& n);  // with array lifting
  EV CallPlain(const FuncInfo& fi, const Node& n, bool preset = false);  // preset: the frame holds n's arguments
  EV CallRefMode(const FuncInfo& fi, const Node& n);  // FuncInfo::ref_mode: arrays of references (RefArray)
  Value LiftedElement(const EV& e);  // one element of the array a lifted call returns
  EV EvalRef(const RefSpec& r);
  EV Binary(char op, const EV& a, const EV& b);
  Value BinaryScalar(char op, const Value& a, const Value& b);
  EV Unary(char op, const EV& a);
  EV Percent(const EV& a);
  EV MapArray(const EV& a, const std::function<Value(const Value&)>& fn);

  EvalHost& host_;
  int32_t sheet_;
  core::CellPos origin_;
  // arguments already evaluated by CallFunction: Eval of call->kids[i] returns args[i] when set[i]
  struct ArgFrame {
    const Node* call = nullptr;
    const EV* args = nullptr;
    const char* set = nullptr;
  };
  ArgFrame frame_;
  // the function being called refuses logicals where a number goes (FuncInfo::no_logical); set by CallFunction
  bool arg_no_logical_ = false;
};

// evaluate one formula and return the cell value. With spill set, a result of several cells (array or range) is
// returned there as a whole (dynamic array) and the return value is its upper-left value; without it, ranges use
// implicit intersection and arrays give their first value.
Value EvaluateCell(const Node& root, EvalHost& host, int32_t sheet, core::CellPos origin, std::shared_ptr<Array>* spill = nullptr);

// the formula's result is a reference (=B1, =B1:B4, =OFFSET(B1,0,0), =IF(TRUE,B1)), not a value
bool EvaluatesToReference(const Node& root, EvalHost& host, int32_t sheet, core::CellPos origin);

// a number at 15 significant digits, the precision Excel shows numbers with and compares them at
double Round15(double x);

// two numbers the way Excel's = < > and the criteria of COUNTIF compare them (Excel 16, measured): each rounded to
// 15 significant digits first. 1+2^-48 = 1 is TRUE and 1+2^-47 = 1 FALSE; 1-2^-50 = 1 is FALSE (it shows as
// 0.999999999999999), which a tolerance relative to the numbers got wrong. -1, 0 or 1
int Compare15(double x, double y);

// Excel's correction of an addition or subtraction that cancels (a formula's last step, SUM's last addition; Excel
// 16, measured on some 700 cases): the result is 0 when its binary exponent is 50 or more below the left operand's,
// the left operand being what was accumulated before that step. =1-(1-2^-50) is 0 and =1-(1-2^-49) isn't;
// =(1-2^-50)-1 isn't either (the left operand is just under 1, one exponent lower).
inline bool CancelsToZero(double left, double result) {
  if (result == 0 || left == 0 || !std::isfinite(left) || !std::isfinite(result)) return false;
  return std::ilogb(left) - std::ilogb(result) >= 50;
}

}  // namespace yc::formula
