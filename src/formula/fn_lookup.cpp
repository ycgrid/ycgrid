// SPDX-License-Identifier: Apache-2.0
// fn_lookup.cpp - lookup and reference functions
#include "formula/fn_util.h"

#include "core/geometry.h"
#include "formula/parser.h"

#include <algorithm>
#include <cmath>

namespace yc::formula::fn {
namespace {

// 2D access: ranges go through the host (entire columns clamped to the used range), arrays are read directly
struct Grid {
  Evaluator* ev = nullptr;
  bool is_range = false;
  RangeRef r;
  std::shared_ptr<Array> a;
  uint32_t rows = 0, cols = 0;
  // the range's own size before a whole row or column is cut to the used range: a binary search runs over all of it,
  // the cells past the used range empty, as Excel's (measured: MATCH(43,G:G,1) over 20 unsorted values in G1:G20 is
  // 20 whatever other columns hold; cut to the used range it changed when a paste far below grew it, and the formula,
  // which doesn't read those cells, kept its old answer: calc_fuzz seed 3022)
  uint32_t full_rows = 0, full_cols = 0;

  bool Init(Evaluator& e, const EV& v) {
    ev = &e;
    if (v.k == EV::K::Range) {
      is_range = true;
      r = v.r;
      core::Rect rc = e.ClampRect(r.sheet, r.rect);
      full_rows = static_cast<uint32_t>(rc.Rows());
      full_cols = static_cast<uint32_t>(rc.Cols());
      const core::Rect used = e.host().UsedRect(r.sheet);
      if (rc.Rows() > 65536) rc.r1 = std::max(rc.r0, std::min(rc.r1, used.r1));
      if (rc.Cols() > 256) rc.c1 = std::max(rc.c0, std::min(rc.c1, used.c1));
      r.rect = rc;
      rows = static_cast<uint32_t>(rc.Rows());
      cols = static_cast<uint32_t>(rc.Cols());
      return true;
    }
    a = e.ToArray(v);
    if (!a) return false;
    rows = full_rows = a->rows;
    cols = full_cols = a->cols;
    return true;
  }
  Value At(uint32_t i, uint32_t j) const {
    if (is_range) return ev->host().Cell(r.sheet, r.rect.r0 + i, r.rect.c0 + j);
    return a->At(i, j);
  }
};

// the lookup functions compare numbers exactly, as Excel's (MATCH(0.3, A1, 0) doesn't find =0.1*3, whose value is
// 0.30000000000000004); the criteria functions and the = operator compare at 15 digits instead
int LookupCompare(const Value& a, const Value& b, bool* ok) {
  if (a.IsNumber() && b.IsNumber()) {
    *ok = true;
    const double x = a.number(), y = b.number();
    return x < y ? -1 : x > y ? 1 : 0;
  }
  return Evaluator::Compare(a, b, ok);
}

bool LooseEq(const Value& a, const Value& b) {
  bool ok;
  return TypeEq(a, b) && LookupCompare(a, b, &ok) == 0;
}

bool MatchesKey(const Value& key, const Value& v, bool wildcard) {
  if (wildcard && key.IsString() && v.IsString()) return WildcardMatch(Lower(key.str()), Lower(v.str()));
  return LooseEq(key, v);
}

bool HasWild(const Value& key) { return key.IsString() && key.str().find_first_of("*?~") != std::string_view::npos; }

// approximate match (ascending order): the last position <= key, by Excel's own binary search (measured on 3,500
// lists of mixed types, sorted or not, for MATCH, VLOOKUP and LOOKUP): it looks at the middle of lo..hi; a cell of
// another type (or empty) there is passed over forward to the next cell of the key's type up to hi, and with none
// the search goes on to the left of the middle; a value equal to the key ends it at the last of the equal values
// right after it
int64_t ApproxAsc(const std::function<Value(uint32_t)>& at, uint32_t n, const Value& key, uint32_t known) {
  // n: the range's whole length; known: the cells read (past them, up to n, every cell is empty)
  auto same_type = [&](const Value& v) { return !v.IsEmpty() && TypeEq(v, key); };
  int64_t lo = 0, hi = static_cast<int64_t>(n) - 1, ans = -1;
  bool ok;
  while (lo <= hi) {
    const int64_t mid = (lo + hi) / 2;
    int64_t j = mid;
    Value v = j < known ? at(static_cast<uint32_t>(j)) : Value();
    while (!same_type(v) && ++j <= hi) {
      if (j >= known) {
        j = hi + 1;  // only empty cells to the end
        break;
      }
      v = at(static_cast<uint32_t>(j));
    }
    if (j > hi) {
      hi = mid - 1;
      continue;
    }
    const int c = LookupCompare(v, key, &ok);
    if (c == 0) {
      while (j + 1 < static_cast<int64_t>(n)) {
        const Value w = at(static_cast<uint32_t>(j + 1));
        if (!same_type(w) || LookupCompare(w, key, &ok) != 0) break;
        ++j;
      }
      return j;
    }
    if (c < 0) {
      ans = j;
      lo = j + 1;
    } else {
      hi = j - 1;
    }
  }
  return ans;
}

// The value VLOOKUP, HLOOKUP, MATCH and LOOKUP look for: a blank (an empty cell, an argument left empty) is the
// number 0, so MATCH(A8,AA1:AA3,0) with A8 empty finds the 0 in AA1 and VLOOKUP(,AA1:AB3,2,FALSE) its row (Excel 16,
// measured). XLOOKUP and XMATCH don't: there a blank only finds a blank.
Value LookupKey(Evaluator& ev, const Node& n) {
  Value key = ev.Scalar(n);
  if (key.IsEmpty()) key = Value::Number(0);
  return key;
}

EV LookupVH(Evaluator& ev, const Node& call, bool vertical) {
  const Value key = LookupKey(ev, call.kids[0]);
  if (key.IsError()) return EV::S(key);
  Grid g;
  if (!g.Init(ev, ev.Eval(call.kids[1]))) return Er(Err::Num);
  YC_NUM(idxf, 2);
  // range_lookup left empty is FALSE, the exact match people write as VLOOKUP(x,table,2,) (Excel 16, measured)
  YC_BOOL_OPT_BLANK0(approx, 3, true);
  const uint32_t idx = static_cast<uint32_t>(idxf);
  const uint32_t n = vertical ? g.rows : g.cols;
  const uint32_t width = vertical ? g.cols : g.rows;
  if (idxf < 1) return Er(Err::Value);
  auto at = [&](uint32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); };
  int64_t pos = -1;
  if (approx) {
    pos = ApproxAsc(at, vertical ? g.full_rows : g.full_cols, key, n);
  } else {
    const bool wild = HasWild(key);
    for (uint32_t i = 0; i < n; ++i) {
      if (MatchesKey(key, at(i), wild)) {
        pos = i;
        break;
      }
    }
  }
  if (pos < 0) return Er(Err::NA);
  // a column past the table's is #REF! only for a value found: one that isn't is #N/A first (Excel 16, measured)
  if (idx > width) return Er(Err::Ref);
  return EV::S(vertical ? g.At(static_cast<uint32_t>(pos), idx - 1) : g.At(idx - 1, static_cast<uint32_t>(pos)));
}
EV FVLookup(Evaluator& ev, const Node& call) { return LookupVH(ev, call, true); }
EV FHLookup(Evaluator& ev, const Node& call) { return LookupVH(ev, call, false); }

EV FMatch(Evaluator& ev, const Node& call) {
  const Value key = LookupKey(ev, call.kids[0]);
  if (key.IsError()) return EV::S(key);
  Grid g;
  if (!g.Init(ev, ev.Eval(call.kids[1]))) return Er(Err::Num);
  // match_type left empty is 0, the exact match: MATCH(6,C1:C10,) is #N/A where MATCH(6,C1:C10) is 3 (Excel 16, measured)
  YC_NUM_OPT_BLANK0(typef, 2, 1);
  const int type = typef > 0 ? 1 : typef < 0 ? -1 : 0;  // Excel reads only the sign: MATCH(x, r, 0.5) is type 1
  if (g.rows != 1 && g.cols != 1) return Er(Err::NA);
  const bool vertical = g.cols == 1;
  const uint32_t n = vertical ? g.rows : g.cols;
  auto at = [&](uint32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); };
  if (type == 0) {
    const bool wild = HasWild(key);
    for (uint32_t i = 0; i < n; ++i) {
      if (MatchesKey(key, at(i), wild)) return Num(i + 1);
    }
    return Er(Err::NA);
  }
  if (type > 0) {
    const int64_t p = ApproxAsc(at, vertical ? g.full_rows : g.full_cols, key, n);
    return p < 0 ? Er(Err::NA) : Num(static_cast<double>(p + 1));
  }
  // descending: the last one >= key, or the first equal to it (9,7,7,7,5 finds 7 at 2, as Excel)
  int64_t ans = -1;
  for (uint32_t i = 0; i < n; ++i) {
    bool ok;
    const Value v = at(i);
    if (!TypeEq(v, key)) continue;
    const int c = LookupCompare(v, key, &ok);
    if (c < 0) break;
    ans = i;
    if (c == 0) break;
  }
  return ans < 0 ? Er(Err::NA) : Num(static_cast<double>(ans + 1));
}

// order of values for a binary search: numbers, text, logical values, then anything else
int TypeRank(const Value& v) { return v.IsNumeric() ? 0 : v.IsString() ? 1 : v.IsBool() ? 2 : 3; }
int RankedCompare(const Value& v, const Value& key) {
  const int a = TypeRank(v), b = TypeRank(key);
  if (a != b) return a < b ? -1 : 1;
  if (a == 3) return 0;
  bool ok;
  return LookupCompare(v, key, &ok);
}

// XMATCH / XLOOKUP search_mode 2 (ascending) and -2 (descending): a real binary search, as Excel's, so data in the
// other order isn't found and a key between duplicates takes the last one before it (2.5 in 1,2,2,3 is position 3)
int64_t XBinary(const Value& key, const std::function<Value(uint32_t)>& at, uint32_t n, int mode, bool ascending) {
  // Excel's binary search, step by step, on the values as ascending (descending: the array read from its end): an
  // equal value moves on to the left, so the leftmost equal one probed wins (the rightmost of a descending array), and
  // the last one probed below the key and the first above it are its next smaller and next larger (Excel 16,
  // measured: XMATCH(2,{1,2,2,2,3},0,2) is 2, XMATCH(2,{3,2,2,2,1},0,-2) is 4, XMATCH(2,{1,2,3},0,-2) is 2 though the
  // data isn't descending, XMATCH(2.5,{1,2,2,3},-1,2) is 3)
  const int64_t m = static_cast<int64_t>(n);
  auto pos = [&](int64_t i) { return ascending ? i : m - 1 - i; };
  auto val = [&](int64_t i) { return at(static_cast<uint32_t>(pos(i))); };
  int64_t lo = 0, hi = m - 1, eq = -1, below = -1, above = -1;
  while (lo <= hi) {
    const int64_t mid = (lo + hi) / 2;
    const Value v = val(mid);
    const int c = RankedCompare(v, key);
    if (c < 0) {
      below = mid;
      lo = mid + 1;
    } else {
      if (c == 0 && TypeEq(v, key)) eq = mid;
      else if (c > 0) above = mid;
      hi = mid - 1;
    }
  }
  if (eq >= 0) return pos(eq);
  if (mode == 0) return -1;
  // next smaller (-1) / next larger (1), of the key's own type
  const int64_t pick = mode == -1 ? below : above;
  return pick >= 0 && TypeEq(val(pick), key) ? pos(pick) : -1;
}

// order of values for XMATCH's next smaller / larger in a linear search: numbers, text, logical values, then empty
// cells, across types, so a key finds the nearest value of another type when its own has none (Excel 16, measured:
// XMATCH(13,{3,"b","A",TRUE},1) is 3, the "A"; XMATCH(FALSE,{"x",<empty>},1) the empty cell); errors are skipped
int LinearRank(const Value& v) { return v.IsNumeric() ? 0 : v.IsString() ? 1 : v.IsBool() ? 2 : v.IsEmpty() ? 3 : -1; }
int LinearCompare(const Value& v, const Value& key) {
  const int a = LinearRank(v), b = LinearRank(key);
  if (a != b) return a < b ? -1 : 1;
  if (a == 3) return 0;
  bool ok;
  return LookupCompare(v, key, &ok);
}

// XMATCH(key, array, [match_mode], [search_mode])
int64_t XFind(Evaluator& ev, const Value& key, const std::function<Value(uint32_t)>& at, uint32_t n, int mode, int search, uint32_t full) {
  (void)ev;
  const bool wild = mode == 2;
  if ((search == 2 || search == -2) && !wild) {
    // over the range's whole length, as MATCH's approximate search (cells past the used range are empty)
    const std::function<Value(uint32_t)> whole = [&](uint32_t i) { return i < n ? at(i) : Value(); };
    const int64_t p = XBinary(key, whole, std::max(n, full), mode, search == 2);
    return p < static_cast<int64_t>(n) ? p : -1;  // a blank key on an empty cell past the cells read: no row to give back
  }
  int64_t best = -1;
  Value bestv;
  const bool rev = search == -1 || search == -2;
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t i = rev ? n - 1 - k : k;
    const Value v = at(i);
    if (mode == 0 || mode == 2) {
      // an empty cell as the key finds an empty cell (XMATCH(A6,A1:A8) with A6 empty, as Excel; "" finds none)
      if (key.IsEmpty() ? v.IsEmpty() : MatchesKey(key, v, wild)) return i;
      continue;
    }
    if (LinearRank(v) < 0) continue;
    const int c = LinearCompare(v, key);
    if (c == 0) return i;
    if (mode == -1 && c < 0 && (best < 0 || LinearCompare(v, bestv) > 0)) {
      best = i;
      bestv = v;
    }
    if (mode == 1 && c > 0 && (best < 0 || LinearCompare(v, bestv) < 0)) {
      best = i;
      bestv = v;
    }
  }
  return best;
}

// XLOOKUP / XMATCH take only these modes, after INT (Excel 16, measured: a search_mode of -1.5 is -2, 0.5 is 0 and
// #VALUE!, -0.5 is -1): anything else is #VALUE!, not a guess
bool XModesOk(double* mode, double* search) {
  *mode = std::floor(*mode);
  *search = std::floor(*search);
  const double m = *mode, k = *search;
  return (m == 0 || m == -1 || m == 1 || m == 2) && (k == 1 || k == -1 || k == 2 || k == -2);
}

EV FXMatch(Evaluator& ev, const Node& call) {
  const Value key = ev.Scalar(call.kids[0]);
  if (key.IsError()) return EV::S(key);
  Grid g;
  if (!g.Init(ev, ev.Eval(call.kids[1]))) return Er(Err::Num);
  YC_NUM_OPT(mode, 2, 0);
  YC_NUM_OPT(search, 3, 1);
  if (!XModesOk(&mode, &search) || (g.rows != 1 && g.cols != 1)) return Er(Err::Value);
  const bool vertical = g.cols == 1;
  const uint32_t n = vertical ? g.rows : g.cols;
  const int64_t p = XFind(ev, key, [&](uint32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); }, n, static_cast<int>(mode), static_cast<int>(search),
                          vertical ? g.full_rows : g.full_cols);
  return p < 0 ? Er(Err::NA) : Num(static_cast<double>(p + 1));
}

// XLOOKUP(key, lookup_array, return_array, [if_not_found], [match_mode], [search_mode])
EV FXLookup(Evaluator& ev, const Node& call) {
  const Value key = ev.Scalar(call.kids[0]);
  if (key.IsError()) return EV::S(key);
  Grid g;
  if (!g.Init(ev, ev.Eval(call.kids[1]))) return Er(Err::Num);
  const EV ret = ev.Eval(call.kids[2]);
  if (ret.k == EV::K::Scalar && ret.s.IsError()) return ret;
  YC_NUM_OPT(mode, 4, 0);
  YC_NUM_OPT(search, 5, 1);
  if (!XModesOk(&mode, &search) || (g.rows != 1 && g.cols != 1)) return Er(Err::Value);
  const bool vertical = g.cols == 1;
  const uint32_t n = vertical ? g.rows : g.cols;
  {
    // the return array runs the same length as the lookup array (Excel: #VALUE! otherwise)
    uint32_t rr = 1, rc = 1;
    if (ret.k == EV::K::Range) {
      const core::Rect rect = ev.ClampRect(ret.r.sheet, ret.r.rect);
      rr = static_cast<uint32_t>(ret.r.rect.Rows());
      rc = static_cast<uint32_t>(rect.Cols());
    } else if (ret.k == EV::K::Array && ret.a) {
      rr = ret.a->rows;
      rc = ret.a->cols;
    }
    const bool one = g.rows == 1 && g.cols == 1;
    if (!one && (vertical ? rr : rc) != n) return Er(Err::Value);
  }
  const int64_t p = XFind(ev, key, [&](uint32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); }, n, static_cast<int>(mode), static_cast<int>(search),
                          vertical ? g.full_rows : g.full_cols);
  if (p < 0) return Has(call, 3) ? ev.Eval(call.kids[3]) : Er(Err::NA);
  if (ret.k == EV::K::Range) {
    core::Rect r = ret.r.rect;
    if (vertical) {
      r.r0 = r.r1 = r.r0 + static_cast<uint32_t>(p);
    } else {
      r.c0 = r.c1 = r.c0 + static_cast<uint32_t>(p);
    }
    return EV::R(RangeRef{ret.r.sheet, r});
  }
  auto a = ev.ToArray(ret);
  if (!a) return Er(Err::Num);
  if (vertical) return static_cast<uint32_t>(p) < a->rows ? EV::S(a->At(static_cast<uint32_t>(p), 0)) : Er(Err::Ref);
  return static_cast<uint32_t>(p) < a->cols ? EV::S(a->At(0, static_cast<uint32_t>(p))) : Er(Err::Ref);
}

EV FLookup(Evaluator& ev, const Node& call) {
  const Value key = LookupKey(ev, call.kids[0]);
  if (key.IsError()) return EV::S(key);
  if (Argc(call) > 2 && !Has(call, 2)) return Er(Err::Value);  // LOOKUP(6,C1:C10,): an empty result vector (Excel 16, measured)
  Grid g;
  if (!g.Init(ev, ev.Eval(call.kids[1]))) return Er(Err::Num);
  // the shape the range has (A:C is tall), not the part the used range leaves of it
  const bool vertical = g.full_rows >= g.full_cols;
  const uint32_t n = vertical ? g.rows : g.cols;
  const int64_t p = ApproxAsc([&](uint32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); }, vertical ? g.full_rows : g.full_cols, key, n);
  if (p < 0) return Er(Err::NA);
  if (Has(call, 2)) {
    const EV rv = ev.Eval(call.kids[2]);
    if (rv.k == EV::K::Range && rv.r.rect.Rows() == 1 && rv.r.rect.Cols() == 1) {
      // one cell: Excel reads the result along its row (LOOKUP(2, A1:A3, B1) finds C1 for the second position)
      const uint64_t c = uint64_t{rv.r.rect.c0} + static_cast<uint64_t>(p);
      if (c >= core::kMaxCols) return Er(Err::NA);
      return EV::S(ev.host().Cell(rv.r.sheet, rv.r.rect.r0, static_cast<uint32_t>(c)));
    }
    Grid r;
    if (!r.Init(ev, rv)) return Er(Err::Num);
    return EV::S(r.rows >= r.cols ? r.At(static_cast<uint32_t>(p), 0) : r.At(0, static_cast<uint32_t>(p)));
  }
  return EV::S(vertical ? g.At(static_cast<uint32_t>(p), g.cols - 1) : g.At(g.rows - 1, static_cast<uint32_t>(p)));
}

const Node& UnparenNode(const Node& n) { return n.kind == NK::Paren && !n.kids.empty() ? UnparenNode(n.kids[0]) : n; }
void UnionAreas(const Node& n, std::vector<const Node*>* out) {
  const Node& u = UnparenNode(n);
  if (u.kind == NK::Binary && u.op == ',') {
    UnionAreas(u.kids[0], out);
    UnionAreas(u.kids[1], out);
  } else {
    out->push_back(&u);
  }
}

// =INDEX(reference, row, [column], [area]): area picks one area of a union like (A1:B4,D1:E4)
EV FIndex(Evaluator& ev, const Node& call) {
  std::vector<const Node*> areas;
  UnionAreas(call.kids[0], &areas);
  YC_NUM_OPT(areaf, 3, 1);
  if (areaf < 1 || areaf > static_cast<double>(areas.size())) return Er(Err::Ref);
  if (areas[static_cast<size_t>(areaf) - 1]->kind == NK::Missing) return Er(Err::Value);  // INDEX(,1) (Excel 16, measured)
  const EV src = ev.Eval(*areas[static_cast<size_t>(areaf) - 1]);
  YC_NUM_OPT(rowf, 1, 0);
  YC_NUM_OPT(colf, 2, 0);
  if (rowf < 0 || colf < 0) return Er(Err::Value);
  if (rowf > 2147483647.0 || colf > 2147483647.0) return Er(Err::Ref);  // past any range: #REF!, not a wrapped index
  uint32_t row = static_cast<uint32_t>(rowf), col = static_cast<uint32_t>(colf);
  if (src.k == EV::K::Range) {
    const core::Rect r = src.r.rect;
    // a range of several rows and columns needs both numbers (Excel: INDEX(A1:B3, 2) is #REF!)
    if (Argc(call) == 2 && r.r0 != r.r1 && r.c0 != r.c1) return Er(Err::Ref);
    if (!Has(call, 2) && r.r0 == r.r1 && Argc(call) == 2) {
      // single-row range: the second argument is the column
      col = row;
      row = 1;
    }
    if (row > r.Rows() || col > r.Cols()) return Er(Err::Ref);
    core::Rect o = r;
    if (row > 0) o.r0 = o.r1 = r.r0 + row - 1;
    if (col > 0) o.c0 = o.c1 = r.c0 + col - 1;
    return EV::R(RangeRef{src.r.sheet, o});
  }
  auto a = ev.ToArray(src);
  if (!a) return Er(Err::Num);
  if (a->rows == 1 && Argc(call) == 2) {
    col = row;
    row = 1;
  }
  if (col == 0 && a->cols == 1) col = 1;  // one column: the row number picks the value
  if (row == 0 && a->rows == 1) row = 1;
  if (row > a->rows || col > a->cols) return Er(Err::Ref);
  if (row > 0 && col > 0 && src.refs) {
    // an array of references gives the reference itself: INDEX(OFFSET(A1,{0,1},0,2),1,2) is A2:A3 (Excel)
    const size_t k = static_cast<size_t>(row - 1) * a->cols + (col - 1);
    if (src.refs->errs[k] != Err::None) return Er(src.refs->errs[k]);
    return EV::R(src.refs->refs[k]);
  }
  if (row > 0 && col > 0) return EV::S(a->At(row - 1, col - 1));
  // 0 for the row or the column: the whole column or row, like Excel (INDEX(array, 0, 2)); of an array of references
  // the references go along (SUMPRODUCT(INDEX(OFFSET(A1,{0,1},0),0)) is 0 and SUM of it 3, as SUM(OFFSET(...)))
  if (row == 0 && col == 0) return src.refs ? src : EV::A(a);
  auto out = std::make_shared<Array>();
  out->rows = row == 0 ? a->rows : 1;
  out->cols = col == 0 ? a->cols : 1;
  out->v.reserve(static_cast<size_t>(out->rows) * out->cols);
  std::shared_ptr<RefArray> refs;
  if (src.refs) refs = std::make_shared<RefArray>();
  for (uint32_t r = 0; r < out->rows; ++r) {
    for (uint32_t c = 0; c < out->cols; ++c) {
      const uint32_t sr = row == 0 ? r : row - 1, sc = col == 0 ? c : col - 1;
      out->v.push_back(a->At(sr, sc));
      if (refs) {
        const size_t k = static_cast<size_t>(sr) * a->cols + sc;
        refs->refs.push_back(src.refs->refs[k]);
        refs->errs.push_back(src.refs->errs[k]);
      }
    }
  }
  EV e = EV::A(std::move(out));
  e.refs = std::move(refs);
  return e;
}

// an argument of OFFSET / INDIRECT that holds several values (an array, a range of several cells) makes the result
// an array of references, one per element (RefArray); false with *one set for a single value
bool ManyValues(Evaluator& ev, const EV& e, std::shared_ptr<Array>* many, Value* one) {
  if (e.k == EV::K::Array || (e.k == EV::K::Range && !e.r.rect.IsCell())) {
    *many = ev.ToArray(e);
    return true;
  }
  *one = ev.ToScalar(e);
  return false;
}

// the array of references built from one EV per element (a reference or an error), in a rows x cols shape: as a value
// every element is #VALUE! (Excel: =OFFSET(A1,{0,1},0) spills two #VALUE!)
EV RefArrayOf(uint32_t rows, uint32_t cols, const std::vector<EV>& elems) {
  auto ra = std::make_shared<RefArray>();
  auto vals = std::make_shared<Array>();
  vals->rows = rows;
  vals->cols = cols;
  vals->v.assign(elems.size(), Value::Error(Err::Value));
  ra->refs.resize(elems.size());
  ra->errs.assign(elems.size(), Err::None);
  for (size_t k = 0; k < elems.size(); ++k) {
    if (elems[k].k == EV::K::Range) ra->refs[k] = elems[k].r;
    else ra->errs[k] = elems[k].IsError() ? elems[k].s.error() : Err::Value;
  }
  EV out = EV::A(std::move(vals));
  out.refs = std::move(ra);
  return out;
}

// the broadcast size of several arrays (a 1-row or 1-column one repeats); false past the array limit
bool BroadcastSize(const std::shared_ptr<Array>* arrs, size_t n, uint32_t* rows, uint32_t* cols) {
  *rows = *cols = 1;
  for (size_t i = 0; i < n; ++i) {
    if (!arrs[i]) continue;
    *rows = *rows == 1 ? arrs[i]->rows : (arrs[i]->rows == 1 ? *rows : std::max(*rows, arrs[i]->rows));
    *cols = *cols == 1 ? arrs[i]->cols : (arrs[i]->cols == 1 ? *cols : std::max(*cols, arrs[i]->cols));
  }
  return uint64_t{*rows} * *cols <= 4'000'000;
}
// element (r, c) of a broadcast array, #N/A outside it
Value BroadcastAt(const Array& a, uint32_t r, uint32_t c) {
  const bool in = (a.rows == 1 || r < a.rows) && (a.cols == 1 || c < a.cols);
  return in ? a.At(a.rows == 1 ? 0 : r, a.cols == 1 ? 0 : c) : Value::Error(Err::NA);
}

// one OFFSET: base moved by dr rows and dc columns, h x w in size; #REF! off the sheet
EV OffsetOne(Evaluator& ev, const RangeRef& base, double dr, double dc, double h, double w) {
  if (std::fabs(dr) > 4e9 || std::fabs(dc) > 4e9 || std::fabs(h) > 4e9 || std::fabs(w) > 4e9) return Er(Err::Ref);
  const core::Rect r = base.rect;
  const int64_t r0 = int64_t{r.r0} + static_cast<int64_t>(dr), c0 = int64_t{r.c0} + static_cast<int64_t>(dc);
  const int64_t hh = static_cast<int64_t>(h), ww = static_cast<int64_t>(w);
  if (hh == 0 || ww == 0) return Er(Err::Ref);
  int64_t ra = r0, rb = r0 + hh + (hh > 0 ? -1 : 1), ca = c0, cb = c0 + ww + (ww > 0 ? -1 : 1);
  if (ra > rb) std::swap(ra, rb);
  if (ca > cb) std::swap(ca, cb);
  if (ra < 0 || ca < 0 || rb >= int64_t{ev.host().RowLimit(base.sheet)} || cb >= core::kMaxCols) return Er(Err::Ref);
  return EV::R(RangeRef{base.sheet, core::Rect{static_cast<uint32_t>(ra), static_cast<uint32_t>(ca), static_cast<uint32_t>(rb), static_cast<uint32_t>(cb)}});
}

EV FOffset(Evaluator& ev, const Node& call) {
  const EV base = ev.Eval(call.kids[0]);
  if (base.k != EV::K::Range) return Er(Err::Value);
  // rows, cols, [height], [width]; any of them may hold several values: OFFSET(A1,{0,1},0) is an array of references
  double v[5] = {0, 0, 0, static_cast<double>(base.r.rect.Rows()), static_cast<double>(base.r.rect.Cols())};
  std::shared_ptr<Array> many[5];
  bool any = false;
  for (int i = 1; i <= 4; ++i) {
    if (i >= 3 && !Has(call, i)) continue;
    Value one;
    if (i < Argc(call) && ManyValues(ev, ev.Eval(call.kids[static_cast<size_t>(i)]), &many[i], &one)) {
      if (!many[i]) return Er(Err::Num);
      any = true;
      continue;
    }
    Err e = Err::None;
    if (!Evaluator::ToNumber(one, &v[i], &e)) return Er(e);
  }
  if (!any) return OffsetOne(ev, base.r, v[1], v[2], v[3], v[4]);
  uint32_t rows, cols;
  if (!BroadcastSize(many, 5, &rows, &cols)) return Er(Err::Num);
  std::vector<EV> elems;
  elems.reserve(static_cast<size_t>(rows) * cols);
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      double x[5] = {0, v[1], v[2], v[3], v[4]};
      Err e = Err::None;
      for (int i = 1; i <= 4 && e == Err::None; ++i) {
        if (many[i] && !Evaluator::ToNumber(BroadcastAt(*many[i], r, c), &x[i], &e) && e == Err::None) e = Err::Value;
      }
      elems.push_back(e != Err::None ? Er(e) : OffsetOne(ev, base.r, x[1], x[2], x[3], x[4]));
    }
  }
  return RefArrayOf(rows, cols, elems);
}

// one side of an R1C1 reference: R[n], Rn or R, then C[n], Cn or C (either part may be missing: whole columns or
// rows); relative parts count from the formula's cell. row / col are 0-based, kRowEnd / kColEnd for a missing part
bool ParseR1C1Side(std::string_view s, core::CellPos origin, int64_t* row, int64_t* col, bool* has_r, bool* has_c) {
  size_t i = 0;
  auto num = [&](int64_t base, int64_t* out) {
    if (i < s.size() && s[i] == '[') {
      const size_t j = s.find(']', i);
      if (j == std::string_view::npos || j == i + 1) return false;
      std::string t(s.substr(i + 1, j - i - 1));
      size_t k = t[0] == '-' || t[0] == '+' ? 1 : 0;
      if (k == t.size()) return false;
      for (; k < t.size(); ++k) {
        if (!std::isdigit(static_cast<unsigned char>(t[k]))) return false;
      }
      if (t.size() > 9) return false;
      *out = base + std::atoll(t.c_str());
      i = j + 1;
      return true;
    }
    size_t j = i;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
    if (j == i) {
      *out = base;  // R or C alone: the formula's own row or column
      return true;
    }
    if (j - i > 9) return false;
    const long long v = std::atoll(std::string(s.substr(i, j - i)).c_str());
    if (v < 1) return false;
    *out = v - 1;
    i = j;
    return true;
  };
  *has_r = *has_c = false;
  if (i < s.size() && (s[i] == 'R' || s[i] == 'r')) {
    ++i;
    *has_r = true;
    if (!num(origin.row, row)) return false;
  }
  if (i < s.size() && (s[i] == 'C' || s[i] == 'c')) {
    ++i;
    *has_c = true;
    if (!num(origin.col, col)) return false;
  }
  return i == s.size() && (*has_r || *has_c);
}

EV IndirectOne(Evaluator& ev, const std::string& text_in, bool a1) {
  if (text_in.empty()) return Er(Err::Ref);
  // trailing spaces are ignored (Excel: INDIRECT("A1 ") is A1)
  std::string_view text = text_in;
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  std::string sheet_name;
  std::string_view addr;
  if (!core::SplitSheetRef(text, &sheet_name, &addr)) addr = text;
  int32_t sheet = ev.sheet();
  if (!sheet_name.empty()) {
    // a sheet name that needs quotes (My Sheet, TRUE) must have them: Excel's INDIRECT("My Sheet!A1") is #REF!
    if (text.front() != '\'' && core::QuoteSheetName(sheet_name).front() == '\'') return Er(Err::Ref);
    sheet = ev.host().FindSheet(sheet_name);
    if (sheet < 0) return Er(Err::Ref);
  }
  core::Rect r;
  if (a1) {
    if (!core::ParseRectA1(addr, &r)) {
      EV out;
      if (sheet_name.empty() && ev.host().ResolveName(text, ev.sheet(), ev.origin(), &out)) return out;
      // a structured reference (Table1[Qty], Table1[#All], [@Qty]) read at the formula's cell, as Excel's INDIRECT does
      if (addr.find('[') != std::string_view::npos) {
        const ParseResult pr = Parse(addr, ev.origin(), nullptr);
        RangeRef rr;
        if (pr.ok && pr.root.kind == NK::StructRef && ev.host().TableRef(ev.sheet(), pr.root, ev.origin(), &rr) == 0) return EV::R(rr);
      }
      return Er(Err::Ref);
    }
  } else {
    // R1C1: R2C3, RC[-1], R[1]C, R2 (a whole row), C3 (a whole column), and ranges of them (R2C3:R4C5, R1:R3)
    const int64_t rows = ev.host().RowLimit(sheet), cols = core::kMaxCols;
    const size_t colon = addr.find(':');
    int64_t ra = 0, ca = 0, rb = 0, cb = 0;
    bool hra, hca, hrb, hcb;
    if (!ParseR1C1Side(addr.substr(0, colon), ev.origin(), &ra, &ca, &hra, &hca)) return Er(Err::Ref);
    rb = ra, cb = ca, hrb = hra, hcb = hca;
    if (colon != std::string_view::npos &&
        (!ParseR1C1Side(addr.substr(colon + 1), ev.origin(), &rb, &cb, &hrb, &hcb) || hrb != hra || hcb != hca)) {
      return Er(Err::Ref);
    }
    if (!hra) ra = 0, rb = rows - 1;
    if (!hca) ca = 0, cb = cols - 1;
    if (ra > rb) std::swap(ra, rb);
    if (ca > cb) std::swap(ca, cb);
    if (ra < 0 || ca < 0 || rb >= rows || cb >= cols) return Er(Err::Ref);
    r = core::Rect{static_cast<uint32_t>(ra), static_cast<uint32_t>(ca), static_cast<uint32_t>(rb), static_cast<uint32_t>(cb)};
  }
  if (r.r1 == core::kRowEnd) r.r1 = ev.host().RowLimit(sheet) - 1;
  if (r.c1 == core::kColEnd) r.c1 = core::kMaxCols - 1;
  return EV::R(RangeRef{sheet, r});
}

EV FIndirect(Evaluator& ev, const Node& call) {
  std::shared_ptr<Array> many;
  Value one;
  const bool several = ManyValues(ev, ev.Eval(call.kids[0]), &many, &one);
  // a1 left empty is FALSE, the R1C1 style: INDIRECT("R1C1",) is A1 and INDIRECT("A1",) #REF! (Excel 16, measured)
  YC_BOOL_OPT_BLANK0(a1, 1, true);
  if (!several) {
    if (one.IsError()) return EV::S(one);
    return IndirectOne(ev, Evaluator::ToText(one), a1);
  }
  // several texts (INDIRECT({"A1","A2"}), INDIRECT("A"&{1,2})): an array of references
  if (!many) return Er(Err::Num);
  std::vector<EV> elems;
  elems.reserve(many->v.size());
  for (const Value& v : many->v) elems.push_back(v.IsError() ? EV::S(v) : IndirectOne(ev, Evaluator::ToText(v), a1));
  return RefArrayOf(many->rows, many->cols, elems);
}

// ROW / COLUMN of a range with several rows (columns): the vertical (horizontal) array of their numbers
EV RowCol(Evaluator& ev, const Node& call, bool row) {
  if (!Has(call, 0)) return Num((row ? ev.origin().row : ev.origin().col) + 1.0);
  RangeRef r;
  if (!ev.ArgRange(call.kids[0], &r)) return Er(Err::Value);
  const uint32_t first = row ? r.rect.r0 : r.rect.c0;
  const uint64_t n = row ? r.rect.Rows() : r.rect.Cols();
  if (n == 1) return Num(first + 1.0);
  if (n > 4'000'000) return Er(Err::Num);
  auto a = std::make_shared<Array>();
  a->rows = row ? static_cast<uint32_t>(n) : 1;
  a->cols = row ? 1 : static_cast<uint32_t>(n);
  a->v.resize(static_cast<size_t>(n));
  for (uint64_t i = 0; i < n; ++i) a->v[static_cast<size_t>(i)] = Value::Number(static_cast<double>(first + i + 1));
  return EV::A(a);
}
EV FRow(Evaluator& ev, const Node& call) { return RowCol(ev, call, true); }
EV FColumn(Evaluator& ev, const Node& call) { return RowCol(ev, call, false); }
// a union of areas like (A1:A3,C1:C3) has no one size: ROWS and COLUMNS of it are #REF! (Excel)
bool IsUnion(const Node& n) {
  std::vector<const Node*> areas;
  UnionAreas(n, &areas);
  return areas.size() > 1;
}
EV FRows(Evaluator& ev, const Node& call) {
  if (IsUnion(call.kids[0])) return Er(Err::Ref);
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Range) return Num(static_cast<double>(e.r.rect.Rows()));
  if (e.k == EV::K::Array) return Num(e.a->rows);
  if (e.k == EV::K::Scalar && e.s.IsError()) return e;  // ROWS(Nope) is #NAME?, ROWS(1/0) #DIV/0! (Excel)
  return Num(1);
}
EV FColumns(Evaluator& ev, const Node& call) {
  if (IsUnion(call.kids[0])) return Er(Err::Ref);
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Range) return Num(static_cast<double>(e.r.rect.Cols()));
  if (e.k == EV::K::Array) return Num(e.a->cols);
  if (e.k == EV::K::Scalar && e.s.IsError()) return e;
  return Num(1);
}

EV FAddress(Evaluator& ev, const Node& call) {
  YC_NUM(row, 0);
  YC_NUM(col, 1);
  YC_NUM_OPT(absn, 2, 1);
  YC_BOOL_OPT(a1, 3, true);
  const int am = static_cast<int>(absn);
  if (am < 1 || am > 4) return Er(Err::Value);
  const bool abs_row = am == 1 || am == 2, abs_col = am == 1 || am == 3;
  // an absolute part (and every A1 part) is a row 1..1048576 / column 1..16384; a relative R1C1 part is an offset
  // that may be negative: ADDRESS(-1, 1, 4, FALSE) is R[-1]C[1] (Excel)
  // (rows up to the sheet's current limit: 1,048,576 as Excel's until the sheet grows)
  const double max_row = static_cast<double>(ev.host().RowLimit(ev.sheet())), max_col = static_cast<double>(core::kMaxCols);
  auto part_ok = [&](double v, bool abs, double mx) { return (a1 || abs) ? v >= 1 && v <= mx : v > -mx && v < mx; };
  if (!part_ok(row, abs_row, max_row) || !part_ok(col, abs_col, max_col)) return Er(Err::Value);
  std::string s;
  if (a1) {
    s = core::CellA1(static_cast<uint32_t>(row) - 1, static_cast<uint32_t>(col) - 1, abs_row, abs_col);
  } else {
    const long long ri = static_cast<long long>(row), ci = static_cast<long long>(col);
    s = abs_row ? "R" + std::to_string(ri) : ri == 0 ? std::string("R") : "R[" + std::to_string(ri) + "]";
    s += abs_col ? "C" + std::to_string(ci) : ci == 0 ? std::string("C") : "C[" + std::to_string(ci) + "]";
  }
  if (Has(call, 4)) {
    YC_TEXT(sheet, 4);
    // Excel: ADDRESS(1,1,,,"") is !$A$1, and a [book] prefix is read as a reference's: the book and the sheet after it
    // are each quoted as needed, all of it together ("[Book1]Sheet1!R2C3", "'[My Book]Sheet1'!R2C3", "[Book1]!$C$2";
    // Excel 16, measured: the documentation's quoted example isn't what Excel answers)
    std::string shown = sheet.empty() ? std::string() : core::QuoteSheetName(sheet);
    if (const size_t close = sheet.find(']'); sheet.size() > 1 && sheet[0] == '[' && close != std::string::npos && close > 1) {
      const std::string book = sheet.substr(1, close - 1), rest = sheet.substr(close + 1);
      const bool quote = core::QuoteSheetName(book) != book || (!rest.empty() && core::QuoteSheetName(rest) != rest);
      if (!quote) shown = sheet;
      else if (shown == sheet) shown = "'" + sheet + "'";
    }
    s = shown + "!" + s;
  }
  return Str(s);
}

EV FTranspose(Evaluator& ev, const Node& call) {
  auto a = ev.ToArray(ev.Eval(call.kids[0]));
  if (!a) return Er(Err::Num);
  auto o = std::make_shared<Array>();
  o->rows = a->cols;
  o->cols = a->rows;
  o->v.resize(a->v.size());
  for (uint32_t r = 0; r < a->rows; ++r) {
    for (uint32_t c = 0; c < a->cols; ++c) o->At(c, r) = a->At(r, c);
  }
  return EV::A(o);
}

EV FHyperlink(Evaluator& ev, const Node& call) {
  // a friendly name left empty is a blank, shown as 0, not the link's text (HYPERLINK("x",) in Excel 16, measured)
  if (Argc(call) > 1) return EV::S(ev.Scalar(call.kids[1]));
  return EV::S(ev.Scalar(call.kids[0]));
}

}  // namespace

void RegisterLookup(std::vector<FuncInfo>& t) {
  t.push_back({"VLOOKUP", 3, 4, FVLookup});
  t.push_back({"HLOOKUP", 3, 4, FHLookup});
  t.push_back({"LOOKUP", 2, 3, FLookup});
  t.push_back({"MATCH", 2, 3, FMatch});
  t.push_back({"XMATCH", 2, 4, FXMatch});
  t.push_back({"XLOOKUP", 3, 6, FXLookup});
  t.push_back({"INDEX", 2, 4, FIndex});
  t.push_back({"OFFSET", 3, 5, FOffset, true, true});
  t.push_back({"INDIRECT", 1, 2, FIndirect, true, true});
  t.push_back({"ROW", 0, 1, FRow});
  t.push_back({"COLUMN", 0, 1, FColumn});
  t.push_back({"ROWS", 1, 1, FRows});
  t.push_back({"COLUMNS", 1, 1, FColumns});
  t.push_back({"ADDRESS", 2, 5, FAddress});
  t.push_back({"TRANSPOSE", 1, 1, FTranspose});
  t.push_back({"HYPERLINK", 1, 2, FHyperlink});
}

}  // namespace yc::formula::fn
