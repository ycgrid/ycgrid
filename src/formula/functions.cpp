// SPDX-License-Identifier: Apache-2.0
#include "formula/functions.h"

#include "formula/collate.h"
#include "formula/fn_util.h"
#include "formula/numfmt.h"

#include <algorithm>
#include <unordered_map>

namespace yc::formula {

namespace {

struct Registry {
  std::vector<FuncInfo> list;
  std::unordered_map<std::string, int32_t> by_name;
};

// which arguments are lifted over arrays (see FuncInfo::lift); functions not listed take ranges / arrays as they are
void ApplyLifting(std::vector<FuncInfo>& list) {
  constexpr uint32_t kAll = ~0u;
  auto bits = [](std::initializer_list<int> v) {
    uint32_t m = 0;
    for (int i : v) m |= 1u << i;
    return m;
  };
  constexpr uint32_t kEven2 = 0x55555554u;  // criteria of SUMIFS-like functions: arguments 2, 4, 6...
  constexpr uint32_t kOdd1 = 0xAAAAAAAAu;   // criteria of COUNTIFS: arguments 1, 3, 5...
  struct L {
    const char* names;  // space separated
    uint32_t mask;
    bool rest;
  };
  const L table[] = {
      // math & trig
      {"ABS SIGN INT TRUNC ROUND ROUNDUP ROUNDDOWN MROUND MOD QUOTIENT SQRT POWER EXP LN LOG LOG10 RANDBETWEEN CEILING FLOOR "
       "CEILING.MATH FLOOR.MATH EVEN ODD FACT COMBIN SIN COS TAN ASIN ACOS ATAN ATAN2 DEGREES RADIANS SINH COSH TANH ASINH ACOSH ATANH",
       kAll, false},
      // text
      {"LEN LEFT RIGHT MID FIND SEARCH SUBSTITUTE REPLACE UPPER LOWER PROPER TRIM CLEAN TEXT VALUE NUMBERVALUE CONCATENATE REPT EXACT "
       "CHAR CODE UNICHAR UNICODE FIXED DOLLAR LENB LEFTB RIGHTB MIDB FINDB SEARCHB REPLACEB ASC DBCS HYPERLINK",
       kAll, false},
      {"TEXTBEFORE TEXTAFTER", bits({0, 2, 3, 4, 5}), false},  // the delimiter may be a list of delimiters
      // date & time
      {"DATE TIME YEAR MONTH DAY HOUR MINUTE SECOND WEEKDAY WEEKNUM ISOWEEKNUM EDATE EOMONTH DATEVALUE TIMEVALUE DAYS DAYS360 DATEDIF YEARFRAC "
       "EPOCH_TO_DATE DATE_TO_EPOCH TS_PART TS_FROM_PARTS DECIMAL ISDECIMAL",
       kAll, false},
      {"NETWORKDAYS WORKDAY", bits({0, 1}), false},
      {"NETWORKDAYS.INTL WORKDAY.INTL", bits({0, 1, 2}), false},
      // information and logical
      {"ISNUMBER ISTEXT ISNONTEXT ISBLANK ISLOGICAL ISERROR ISERR ISNA ISEVEN ISODD ERROR.TYPE NOT ADDRESS", kAll, false},
      {"IF IFERROR IFNA SWITCH CHOOSE", bits({0}), true},
      {"IFS", 0x55555555u, true},
      // financial and distributions
      {"PV FV PMT NPER RATE IPMT PPMT", kAll, false},
      {"NORM.DIST NORMDIST NORM.S.DIST NORMSDIST NORM.INV NORMINV NORM.S.INV NORMSINV STANDARDIZE T.DIST T.DIST.2T T.DIST.RT TDIST T.INV "
       "T.INV.2T TINV CHISQ.DIST CHISQ.DIST.RT CHIDIST CHISQ.INV CHISQ.INV.RT CHIINV F.DIST F.DIST.RT FDIST F.INV F.INV.RT FINV GAMMA.DIST "
       "GAMMADIST GAMMA.INV GAMMAINV GAMMALN GAMMALN.PRECISE GAMMA BINOM.DIST BINOMDIST POISSON.DIST POISSON EXPON.DIST EXPONDIST "
       "WEIBULL.DIST WEIBULL LOGNORM.DIST LOGNORMDIST LOGNORM.INV LOGINV CONFIDENCE.NORM CONFIDENCE CONFIDENCE.T",
       kAll, false},
      // a single value next to ranges
      {"SUMIF COUNTIF AVERAGEIF LARGE SMALL PERCENTILE PERCENTILE.INC PERCENTILE.EXC QUARTILE QUARTILE.INC TRIMMEAN", bits({1}), false},
      {"PERCENTRANK PERCENTRANK.INC PERCENTRANK.EXC", bits({1, 2}), false},
      {"SUMIFS AVERAGEIFS MAXIFS MINIFS", kEven2, false},
      {"COUNTIFS", kOdd1, false},
      {"VLOOKUP HLOOKUP", bits({0, 2}), false},
      {"MATCH XMATCH XLOOKUP LOOKUP RANK RANK.EQ RANK.AVG FORECAST FORECAST.LINEAR", bits({0}), false},
      {"INDEX", bits({1, 2}), false},
      // engineering
      {"BIN2DEC BIN2HEX BIN2OCT DEC2BIN DEC2HEX DEC2OCT HEX2BIN HEX2DEC HEX2OCT OCT2BIN OCT2DEC OCT2HEX BITAND BITOR BITXOR BITLSHIFT "
       "BITRSHIFT DELTA GESTEP ERF ERF.PRECISE ERFC ERFC.PRECISE BESSELI BESSELJ BESSELK BESSELY CONVERT COMPLEX IMREAL IMAGINARY IMABS "
       "IMARGUMENT IMCONJUGATE IMSQRT IMEXP IMLN IMLOG10 IMLOG2 IMSIN IMCOS IMTAN IMSINH IMCOSH IMCOT IMCSC IMCSCH IMSEC IMSECH IMPOWER "
       "IMDIV IMSUB",
       kAll, false},
      // financial (scalar arguments)
      {"COUPDAYBS COUPDAYS COUPDAYSNC COUPNCD COUPNUM COUPPCD PRICE YIELD DURATION MDURATION ODDLPRICE ODDLYIELD ODDFPRICE ODDFYIELD "
       "ACCRINT ACCRINTM PRICEDISC YIELDDISC DISC INTRATE RECEIVED PRICEMAT YIELDMAT TBILLPRICE TBILLYIELD TBILLEQ SLN SYD DB DDB VDB "
       "AMORLINC AMORDEGRC EFFECT NOMINAL ISPMT PDURATION RRI CUMIPMT CUMPRINC DOLLARDE DOLLARFR",
       kAll, false},
      {"FVSCHEDULE", bits({0}), false},
      {"MIRR", bits({1, 2}), false},
      // math, statistics and text with scalar arguments
      {"ACOT ACOTH COT COTH CSC CSCH SEC SECH ROMAN ARABIC BASE CEILING.PRECISE ISO.CEILING FLOOR.PRECISE COMBINA FACTDOUBLE SQRTPI MUNIT "
       "BETA.DIST BETADIST BETA.INV BETAINV BINOM.DIST.RANGE BINOM.INV CRITBINOM FISHER FISHERINV GAUSS PHI HYPGEOM.DIST HYPGEOMDIST "
       "NEGBINOM.DIST NEGBINOMDIST PERMUT PERMUTATIONA BAHTTEXT VALUETOTEXT REGEXTEST REGEXEXTRACT REGEXREPLACE ENCODEURL FILTERXML "
       "WEBSERVICE IMAGE TRANSLATE DETECTLANGUAGE",
       kAll, false},
      {"SERIESSUM", bits({0, 1, 2}), false},
      {"QUARTILE.EXC", bits({1}), false},
      {"Z.TEST ZTEST", bits({1, 2}), false},
      {"T.TEST TTEST", bits({2, 3}), false},
      {"PROB", bits({2, 3}), false},
      {"FORECAST.ETS FORECAST.ETS.CONFINT", bits({0}), false},
  };
  std::unordered_map<std::string, size_t> idx;
  for (size_t i = 0; i < list.size(); ++i) idx.emplace(list[i].name, i);
  for (const L& l : table) {
    std::string_view s = l.names;
    while (!s.empty()) {
      const size_t sp = s.find(' ');
      const std::string name(s.substr(0, sp));
      s = sp == std::string_view::npos ? std::string_view() : s.substr(sp + 1);
      auto it = idx.find(name);
      if (it == idx.end()) continue;
      list[it->second].lift = l.mask;
      list[it->second].lift_rest = l.rest;
    }
  }
}

const Registry& Reg() {
  static const Registry r = [] {
    Registry g;
    fn::RegisterMath(g.list);
    fn::RegisterStat(g.list);
    fn::RegisterLogic(g.list);
    fn::RegisterLookup(g.list);
    fn::RegisterText(g.list);
    fn::RegisterDate(g.list);
    fn::RegisterInfo(g.list);
    fn::RegisterYc(g.list);
    fn::RegisterFinancial(g.list);
    fn::RegisterDist(g.list);
    fn::RegisterArray(g.list);
    fn::RegisterLambda(g.list);
    fn::RegisterEngineering(g.list);
    fn::RegisterDatabase(g.list);
    fn::RegisterFinancial2(g.list);
    fn::RegisterMath2(g.list);
    fn::RegisterGroup(g.list);
    fn::RegisterText2(g.list);
    fn::RegisterOnline(g.list);
    ApplyLifting(g.list);
    for (size_t i = 0; i < g.list.size(); ++i) g.by_name.emplace(g.list[i].name, static_cast<int32_t>(i));
    // what each function does with an array of references (OFFSET(A1,{0,1},0)), as measured in Excel 16; the rest
    // (COUNT, MEDIAN, LARGE, CONCAT, + ...) see #VALUE! in every element
    auto mode = [&](std::string_view names, uint8_t m) {
      while (!names.empty()) {
        const size_t sp = names.find(' ');
        const auto it = g.by_name.find(std::string(names.substr(0, sp)));
        if (it != g.by_name.end()) g.list[static_cast<size_t>(it->second)].ref_mode = m;
        names = sp == std::string_view::npos ? std::string_view() : names.substr(sp + 1);
      }
    };
    mode("N T SUBTOTAL COUNTIF COUNTIFS SUMIF SUMIFS AVERAGEIF AVERAGEIFS MAXIFS MINIFS COUNTBLANK ROW COLUMN CELL AREAS", 1);
    mode("SUM AVERAGE MIN MAX PRODUCT SUMSQ STDEV STDEV.S STDEV.P STDEVP STDEVA STDEVPA VAR VAR.S VAR.P VARP VARA "
         "AVERAGEA MINA MAXA GEOMEAN AVEDEV DEVSQ NPV", 2);
    mode("SUMPRODUCT", 3);
    mode("AND OR XOR", 4);
    mode("SUMX2MY2 SUMX2PY2 SUMXMY2", 5);
    // the Analysis ToolPak's functions that read their numbers through ArgNumber: a logical there is #VALUE!
    // (each one measured in Excel 16; PDURATION, RRI, ISPMT, the BIT functions and Z.TEST take logicals, and the
    // date functions of the add-in check theirs in ArgDate)
    auto no_logical = [&](std::string_view names) {
      while (!names.empty()) {
        const size_t sp = names.find(' ');
        const auto it = g.by_name.find(std::string(names.substr(0, sp)));
        if (it != g.by_name.end()) g.list[static_cast<size_t>(it->second)].no_logical = true;
        names = sp == std::string_view::npos ? std::string_view() : names.substr(sp + 1);
      }
    };
    no_logical("QUOTIENT MROUND SQRTPI RANDBETWEEN FACTDOUBLE SERIESSUM "
               "BESSELJ BESSELY BESSELI BESSELK CONVERT DELTA ERF ERFC ERF.PRECISE ERFC.PRECISE GESTEP COMPLEX IMPOWER "
               "BIN2HEX BIN2OCT DEC2BIN DEC2HEX DEC2OCT HEX2BIN HEX2OCT OCT2BIN OCT2HEX "
               "EFFECT NOMINAL DOLLARDE DOLLARFR FVSCHEDULE CUMIPMT CUMPRINC XNPV XIRR PRICE YIELD ACCRINT ACCRINTM DISC "
               "DURATION MDURATION COUPDAYBS COUPDAYS COUPDAYSNC COUPNCD COUPNUM COUPPCD TBILLPRICE TBILLYIELD TBILLEQ "
               "AMORLINC AMORDEGRC ODDLPRICE ODDLYIELD ODDFPRICE ODDFYIELD INTRATE RECEIVED PRICEDISC PRICEMAT "
               "YIELDDISC YIELDMAT");
    return g;
  }();
  return r;
}

}  // namespace

int32_t FindFunction(std::string_view upper_name) {
  const auto& r = Reg();
  auto it = r.by_name.find(std::string(upper_name));
  return it == r.by_name.end() ? -1 : it->second;
}

const FuncInfo& GetFunction(int32_t id) { return Reg().list[static_cast<size_t>(id)]; }
const std::vector<FuncInfo>& AllFunctions() { return Reg().list; }

std::vector<std::string> FunctionNames() {
  std::vector<std::string> out;
  for (const auto& f : Reg().list) out.emplace_back(f.name);
  std::sort(out.begin(), out.end());
  return out;
}

namespace fn {

std::string CaseMapUtf8(std::string_view s, bool upper);

// LOWER / UPPER and every comparison that ignores case: ASCII in place; other text by code point (accented Latin,
// Greek, Cyrillic, full-width letters), as Excel does: UPPER of a full-width a is a full-width A
std::string Lower(std::string_view s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) >= 0x80) return CaseMapUtf8(s, false);
  }
  std::string o(s);
  for (char& c : o) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return o;
}

std::string Upper(std::string_view s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) >= 0x80) return CaseMapUtf8(s, true);
  }
  std::string o(s);
  for (char& c : o) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  }
  return o;
}

namespace {
// UTF-8 -> code points
std::vector<uint32_t> Decode(std::string_view s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; n = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}
void Encode(uint32_t cp, std::string& o) {
  if (cp < 0x80) o += static_cast<char>(cp);
  else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
  else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
}
}  // namespace

namespace {
// one code point's other case (itself when it has none)
uint32_t CaseOf(uint32_t c, bool upper) {
  auto in = [&](uint32_t lo, uint32_t hi) { return c >= lo && c <= hi; };
  if (upper) {
    if (in('a', 'z') || (in(0xE0, 0xFE) && c != 0xF7) || (in(0x3B1, 0x3C9) && c != 0x3C2) || in(0x430, 0x44F) || in(0xFF41, 0xFF5A)) {
      return c - 32;
    }
    if (c == 0xFF) return 0x178;
    if (c == 0x3C2) return 0x3A3;  // final sigma
    if (in(0x450, 0x45F)) return c - 80;
    // Latin Extended-A: pairs, the upper case first (0x139-0x148 and 0x179-0x17E start one later)
    if ((in(0x101, 0x137) || in(0x14B, 0x177)) && (c & 1)) return c - 1;
    if ((in(0x13A, 0x148) || in(0x17A, 0x17E)) && !(c & 1)) return c - 1;
    return c;
  }
  if (in('A', 'Z') || (in(0xC0, 0xDE) && c != 0xD7) || (in(0x391, 0x3A9) && c != 0x3A2) || in(0x410, 0x42F) || in(0xFF21, 0xFF3A)) {
    return c + 32;
  }
  if (c == 0x178) return 0xFF;
  if (in(0x400, 0x40F)) return c + 80;
  if ((in(0x100, 0x136) || in(0x14A, 0x176)) && !(c & 1)) return c + 1;
  if ((in(0x139, 0x147) || in(0x179, 0x17D)) && (c & 1)) return c + 1;
  return c;
}
}  // namespace

std::string CaseMapUtf8(std::string_view s, bool upper) {
  std::string out;
  out.reserve(s.size());
  for (uint32_t cp : Decode(s)) Encode(CaseOf(cp, upper), out);
  return out;
}

// PROPER: a letter after a letter is lowered, any other is raised. Letters are the characters that have a case, so
// digits, a non-breaking or ideographic space and CJK characters start a word, as in Excel ("2nd" is "2Nd")
std::string ProperCase(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool prev_letter = false;
  for (uint32_t cp : Decode(s)) {
    const uint32_t up = CaseOf(cp, true), lo = CaseOf(cp, false);
    const bool letter = up != cp || lo != cp || cp == 0xDF;  // sharp s has no upper case of its own
    Encode(letter ? (prev_letter ? lo : up) : cp, out);
    prev_letter = letter;
  }
  return out;
}

}  // namespace fn

std::string FoldCase(std::string_view s) { return fn::Lower(s); }

namespace fn {

size_t Utf16Length(std::string_view s) {
  size_t n = 0;
  for (uint32_t cp : Decode(s)) n += cp >= 0x10000 ? 2 : 1;
  return n;
}

std::string Utf16Substr(std::string_view s, size_t start, size_t len) {
  std::string out;
  size_t pos = 0;
  for (uint32_t cp : Decode(s)) {
    const size_t w = cp >= 0x10000 ? 2 : 1;
    if (pos >= start && pos < start + len) Encode(cp, out);
    pos += w;
    if (pos >= start + len) break;
  }
  return out;
}

bool CollectNumbers(Evaluator& ev, const Node& call, std::vector<double>* out, Value* err, int first_arg, int mode) {
  const bool a_variant = (mode & (kNumsA | kNumsAverageA)) != 0;
  for (size_t i = static_cast<size_t>(first_arg); i < call.kids.size(); ++i) {
    const EV e = ev.Eval(call.kids[i]);  // an argument left empty is a blank
    if (e.k == EV::K::Scalar) {
      const Value& v = e.s;
      if (v.IsError()) {
        *err = v;
        return false;
      }
      if (v.IsEmpty()) {
        if (!(mode & kNumsSkipBlankArgs)) out->push_back(0);  // BlankArgAsZero's rule
        continue;
      }
      double d;
      Err er = Err::None;
      if (!Evaluator::ToNumber(v, &d, &er)) {
        *err = Value::Error(er);
        return false;
      }
      out->push_back(d);
      continue;
    }
    bool ok = true;
    // the ...A functions read logicals and text in references only; AVERAGEA alone reads an array's text (NumsMode)
    const bool logicals = a_variant && e.k == EV::K::Range;
    const bool texts = a_variant && (e.k == EV::K::Range || (mode & kNumsAverageA));
    ev.ForEachValue(e, [&](const Value& v) {
      if (v.IsError()) {
        *err = v;
        ok = false;
        return false;
      }
      if (v.IsPending()) {
        *err = v;
        ok = false;
        return false;
      }
      if (v.IsNumeric()) out->push_back(v.AsDouble());
      else if (logicals && v.IsBool()) out->push_back(v.boolean() ? 1 : 0);
      else if (texts && v.IsString()) out->push_back(0);
      return true;
    });
    if (!ok) return false;
  }
  return true;
}

bool CollectModeNumbers(Evaluator& ev, const Node& call, std::vector<double>* out, Value* err) {
  auto fail = [&](const Value& v) {
    *err = v;
    return false;
  };
  for (const Node& k : call.kids) {
    const EV e = ev.Eval(k);
    if (e.k == EV::K::Scalar) {
      if (e.s.IsError() || e.s.IsPending()) return fail(e.s);
      if (e.s.IsEmpty()) return fail(Value::Error(Err::Value));
      if (e.s.IsNumeric()) out->push_back(e.s.AsDouble());
      continue;
    }
    if (e.k == EV::K::Range && e.r.sheet2 < 0 && e.r.rect.IsCell() && ev.host().Cell(e.r.sheet, e.r.rect.r0, e.r.rect.c0).IsEmpty()) {
      return fail(Value::Error(Err::Value));
    }
    bool ok = true;
    ev.ForEachValue(e, [&](const Value& v) {
      if (v.IsError() || v.IsPending()) {
        *err = v;
        ok = false;
        return false;
      }
      if (v.IsNumeric()) out->push_back(v.AsDouble());
      return true;
    });
    if (!ok) return false;
  }
  return true;
}

EV SumValues(Evaluator& ev, const Node& call) {
  double sum = 0, before = 0;  // before: the sum without the last number (ExcelSum's rule)
  core::Dec dsum{core::Int128(0), 0};
  bool any_dec = false, dec_ok = true;
  Value err;
  bool failed = false;
  auto add = [&](const Value& v) {
    // the sum in doubles holds every number, the Decimals too: it is the result when a number that isn't exact
    // joins them (SUM(DECIMAL("1.5"), 0.5) was 0.5, the Decimals left out)
    const double x = v.AsDouble();
    before = sum;
    sum += x;
    if (v.IsDecimal()) {
      any_dec = true;
      core::Dec d = core::DecFromValue(v);
      if (!core::DecAdd(dsum, d, &dsum)) dec_ok = false;
    } else {
      if (std::floor(x) == x && std::fabs(x) < 1e15) {
        core::Int128 m;
        if (core::Int128::FromDouble(x, &m)) {
          if (!core::DecAdd(dsum, core::Dec{m, 0}, &dsum)) dec_ok = false;
        }
      } else {
        dec_ok = false;
      }
    }
  };
  for (const Node& k : call.kids) {
    if (k.kind == NK::Missing) continue;
    const EV e = ev.Eval(k);
    if (e.k == EV::K::Scalar) {
      const Value& v = e.s;
      if (v.IsError() || v.IsPending()) return EV::S(v);
      if (v.IsEmpty()) continue;
      if (v.IsNumeric()) {
        add(v);
        continue;
      }
      double d;
      Err er = Err::None;
      if (!Evaluator::ToNumber(v, &d, &er)) return Er(er);
      add(Value::Number(d));
      continue;
    }
    ev.ForEachValue(e, [&](const Value& v) {
      if (v.IsError() || v.IsPending()) {
        err = v;
        failed = true;
        return false;
      }
      if (v.IsNumeric()) add(v);
      return true;
    });
    if (failed) return EV::S(err);
  }
  if (any_dec && dec_ok) return EV::S(core::DecToValue(dsum));
  return Num(CancelsToZero(before, sum) ? 0 : sum);
}

bool WildcardMatch(std::string_view p, std::string_view t) {
  // iterative wildcard matching (* ? ~ escape); the caller has lowercased already. ? is one character, not one
  // byte, and * gives back whole characters: COUNTIF(A:A,"?" & a Chinese character) finds a two-character text
  auto char_len = [&](size_t i) {
    const unsigned char c = static_cast<unsigned char>(t[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    return std::min(n, t.size() - i);
  };
  // the pattern's * and ? are wildcards even where the text holds the same character (a* matches the text a*c), and
  // a ~ ending the pattern is dropped (Excel: ab~ matches ab)
  size_t tildes = 0;
  for (size_t i = p.size(); i > 0 && p[i - 1] == '~'; --i) ++tildes;
  if (tildes % 2 == 1) p.remove_suffix(1);
  size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
  while (ti < t.size()) {
    if (pi < p.size() && p[pi] == '~' && pi + 1 < p.size()) {
      if (p[pi + 1] == t[ti]) {
        pi += 2;
        ++ti;
        continue;
      }
    } else if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = ti;
      continue;
    } else if (pi < p.size() && p[pi] == '?') {
      ++pi;
      ti += char_len(ti);
      continue;
    } else if (pi < p.size() && p[pi] == t[ti]) {
      ++pi;
      ++ti;
      continue;
    }
    if (star != std::string_view::npos) {
      pi = star + 1;
      mark += char_len(mark);
      ti = mark;
      continue;
    }
    return false;
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

Criteria ParseCriteria(const Value& c) {
  Criteria cr;
  if (!c.IsString()) {
    cr.target = c.IsEmpty() ? Value::Number(0) : c;
    return cr;
  }
  std::string s(c.str());
  size_t k = 0;
  if (s.rfind("<>", 0) == 0) { cr.op = 1; k = 2; }
  else if (s.rfind("<=", 0) == 0) { cr.op = 3; k = 2; }
  else if (s.rfind(">=", 0) == 0) { cr.op = 5; k = 2; }
  else if (s.rfind("<", 0) == 0) { cr.op = 2; k = 1; }
  else if (s.rfind(">", 0) == 0) { cr.op = 4; k = 1; }
  else if (s.rfind("=", 0) == 0) { cr.op = 0; k = 1; }
  const std::string rest = s.substr(k);
  if (rest.empty()) {
    cr.blank = true;
    cr.blank_mode = k == 0 ? 0 : cr.op == 0 ? 1 : cr.op == 1 ? 2 : 3;
    return cr;
  }
  const InputParse p = ParseInput(rest);
  if ((p.value.IsNumeric() && !p.typed_only) || p.value.IsBool() || p.value.IsError()) {
    cr.target = p.value;
  } else {
    cr.target = Value::String(rest);
    // a pattern only with * or ?: without them a ~ is itself ("~~" matches ~~, not ~; Excel 16, measured)
    if ((cr.op == 0 || cr.op == 1) && rest.find_first_of("*?") != std::string::npos) {
      cr.wildcard = true;
      cr.pattern = Lower(rest);
    }
  }
  return cr;
}

bool Criteria::Match(const Value& v) const {
  if (blank) {
    switch (blank_mode) {
      case 0: return v.IsEmpty() || (v.IsString() && v.str().empty());
      case 1: return v.IsEmpty();
      case 2: return !v.IsEmpty();
      default: return false;
    }
  }
  if (wildcard) {
    const bool m = v.IsString() && WildcardMatch(pattern, Lower(v.str()));
    return op == 1 ? !m : m;
  }
  // types must match (numbers and text are not compared by size); except <>
  const bool num_target = target.IsNumeric();
  if (num_target) {
    double x;
    if (v.IsNumeric()) x = v.AsDouble();
    else if (v.IsString()) {
      const InputParse p = ParseInput(v.str());
      if (!p.value.IsNumeric() || p.typed_only || op != 0) return op == 1;
      x = p.value.AsDouble();
    } else {
      return op == 1;
    }
    // compared at 15 digits, as Excel's criteria do: COUNTIF(A1, 0.3) counts =0.1*3 (0.30000000000000004), and
    // ">0.3" doesn't (the lookup functions compare exactly instead)
    const int c = Compare15(x, target.AsDouble());
    switch (op) {
      case 0: return c == 0;
      case 1: return c != 0;
      case 2: return c < 0;
      case 3: return c <= 0;
      case 4: return c > 0;
      default: return c >= 0;
    }
  }
  // TRUE and FALSE among themselves (FALSE < TRUE), and errors by their number (#NULL! first, #N/A after #NUM!):
  // COUNTIF(r, "<=TRUE") counts both, "<=#N/A" #DIV/0! too (Excel 16, measured)
  auto by_order = [this](int c) {
    switch (op) {
      case 0: return c == 0;
      case 1: return c != 0;
      case 2: return c < 0;
      case 3: return c <= 0;
      case 4: return c > 0;
      default: return c >= 0;
    }
  };
  if (target.IsBool()) {
    if (!v.IsBool()) return op == 1;
    return by_order(static_cast<int>(v.boolean()) - static_cast<int>(target.boolean()));
  }
  if (target.IsError()) {
    if (!v.IsError()) return op == 1;
    return by_order(static_cast<int>(v.error()) - static_cast<int>(target.error()));
  }
  // text
  if (!v.IsString()) return op == 1;
  const std::string a = Lower(v.str()), b = Lower(target.str());
  const int c = CollateLowered(a, b);  // Excel's text order: COUNTIF(r, ">BRK-B") skips BRKB
  switch (op) {
    case 0: return c == 0;
    case 1: return c != 0;
    case 2: return c < 0;
    case 3: return c <= 0;
    case 4: return c > 0;
    default: return c >= 0;
  }
}

void ForEachCellAll(Evaluator& ev, const EV& e, const std::function<void(size_t, const Value&)>& fn, size_t* count) {
  auto a = ev.ToArray(e);
  if (!a) {
    *count = 0;
    return;
  }
  *count = a->v.size();
  for (size_t i = 0; i < a->v.size(); ++i) fn(i, a->v[i]);
}

}  // namespace fn
}  // namespace yc::formula
