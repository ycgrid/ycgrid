// SPDX-License-Identifier: Apache-2.0
// fn_eng.cpp - engineering functions: number bases (BIN2DEC ... OCT2HEX), bitwise (BITAND ...), CONVERT, complex
// numbers (COMPLEX, IM*), Bessel functions, ERF / ERFC, DELTA, GESTEP
//
// Follows Excel: base conversions take at most 10 digits and use 10-digit two's complement for negative numbers;
// complex numbers are text such as "3+4i" (or with j).
#include "formula/fn_util.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace yc::formula::fn {
namespace {

constexpr double kPi = 3.14159265358979323846;

EV Result(double v) { return std::isfinite(v) ? Num(v) : Er(Err::Num); }

// ---- number bases ----

int BitsOf(int base) { return base == 2 ? 10 : base == 8 ? 30 : 40; }

// "1100" in base -> value (10 digits, two's complement); false = #NUM!
bool ParseBase(std::string s, int base, int64_t* out) {
  while (!s.empty() && s.back() == ' ') s.pop_back();
  if (s.empty()) {
    *out = 0;
    return true;
  }
  if (s.size() > 10) return false;
  int64_t v = 0;
  for (char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    if (d >= base) return false;
    v = v * base + d;
  }
  const int bits = BitsOf(base);
  if (s.size() == 10 && (v >> (bits - 1)) & 1) v -= int64_t{1} << bits;  // negative (two's complement)
  *out = v;
  return true;
}

// value -> text in base with places; #NUM! when out of range or too few places
EV FormatBase(Evaluator& ev, const Node& call, int64_t v, int base, int places_arg) {
  const int bits = BitsOf(base);
  const int64_t lo = -(int64_t{1} << (bits - 1)), hi = (int64_t{1} << (bits - 1)) - 1;
  if (v < lo || v > hi) return Er(Err::Num);
  static const char* kDigits = "0123456789ABCDEF";
  std::string s;
  uint64_t u = v < 0 ? static_cast<uint64_t>(v + (int64_t{1} << bits)) : static_cast<uint64_t>(v);
  do {
    s.insert(s.begin(), kDigits[u % static_cast<uint64_t>(base)]);
    u /= static_cast<uint64_t>(base);
  } while (u);
  if (v >= 0 && Has(call, places_arg)) {
    YC_NUM(p, places_arg);
    if (p < 0) return Er(Err::Num);
    const auto places = static_cast<size_t>(std::trunc(p));
    if (places > 10 || places < s.size()) return Er(Err::Num);
    s.insert(0, places - s.size(), '0');
  }
  return Str(s);
}

// the number to convert: a logical (TRUE, also from a cell) is #VALUE!, as in Excel
EV BaseToBase(Evaluator& ev, const Node& call, int from, int to) {
  const Value a = ev.Scalar(call.kids[0]);
  if (a.IsError()) return EV::S(a);
  if (a.IsBool()) return Er(Err::Value);
  const std::string s = ev.ToText(a);
  int64_t v;
  if (!ParseBase(s, from, &v)) return Er(Err::Num);
  if (to == 10) return Num(static_cast<double>(v));
  return FormatBase(ev, call, v, to, 1);
}

EV DecToBase(Evaluator& ev, const Node& call, int to) {
  const Value a = ev.Scalar(call.kids[0]);
  if (a.IsBool()) return Er(Err::Value);
  double d = 0;
  Err er = Err::None;
  if (!Evaluator::ToNumber(a, &d, &er)) return Er(er);
  if (!std::isfinite(d) || std::fabs(d) > 1e15) return Er(Err::Num);
  return FormatBase(ev, call, static_cast<int64_t>(std::trunc(d)), to, 1);
}

EV FBin2Dec(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 2, 10); }
EV FBin2Hex(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 2, 16); }
EV FBin2Oct(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 2, 8); }
EV FHex2Dec(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 16, 10); }
EV FHex2Bin(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 16, 2); }
EV FHex2Oct(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 16, 8); }
EV FOct2Dec(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 8, 10); }
EV FOct2Bin(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 8, 2); }
EV FOct2Hex(Evaluator& ev, const Node& call) { return BaseToBase(ev, call, 8, 16); }
EV FDec2Bin(Evaluator& ev, const Node& call) { return DecToBase(ev, call, 2); }
EV FDec2Hex(Evaluator& ev, const Node& call) { return DecToBase(ev, call, 16); }
EV FDec2Oct(Evaluator& ev, const Node& call) { return DecToBase(ev, call, 8); }

// ---- bitwise (non-negative whole numbers below 2^48) ----

constexpr double kBitMax = 281474976710656.0;  // 2^48

bool BitArg(double d, uint64_t* out) {
  if (d < 0 || d >= kBitMax || d != std::floor(d)) return false;
  *out = static_cast<uint64_t>(d);
  return true;
}

template <class Op>
EV Bitwise(Evaluator& ev, const Node& call, Op op) {
  YC_NUM(a, 0);
  YC_NUM(b, 1);
  uint64_t x, y;
  if (!BitArg(a, &x) || !BitArg(b, &y)) return Er(Err::Num);
  return Num(static_cast<double>(op(x, y)));
}

EV FBitAnd(Evaluator& ev, const Node& call) { return Bitwise(ev, call, [](uint64_t a, uint64_t b) { return a & b; }); }
EV FBitOr(Evaluator& ev, const Node& call) { return Bitwise(ev, call, [](uint64_t a, uint64_t b) { return a | b; }); }
EV FBitXor(Evaluator& ev, const Node& call) { return Bitwise(ev, call, [](uint64_t a, uint64_t b) { return a ^ b; }); }

EV Shift(Evaluator& ev, const Node& call, bool left) {
  YC_NUM(n, 0);
  YC_NUM(s, 1);
  uint64_t x;
  if (!BitArg(n, &x) || std::fabs(s) > 53) return Er(Err::Num);
  int sh = static_cast<int>(std::trunc(s));
  if (!left) sh = -sh;
  const double r = sh >= 0 ? static_cast<double>(x) * std::ldexp(1.0, sh) : std::floor(static_cast<double>(x) / std::ldexp(1.0, -sh));
  if (r >= kBitMax) return Er(Err::Num);
  return Num(r);
}
EV FBitLShift(Evaluator& ev, const Node& call) { return Shift(ev, call, true); }
EV FBitRShift(Evaluator& ev, const Node& call) { return Shift(ev, call, false); }

// ---- DELTA, GESTEP, ERF ----

EV FDelta(Evaluator& ev, const Node& call) {
  YC_NUM(a, 0);
  YC_NUM_OPT(b, 1, 0);
  return Num(a == b ? 1 : 0);
}
EV FGeStep(Evaluator& ev, const Node& call) {
  YC_NUM(a, 0);
  YC_NUM_OPT(b, 1, 0);
  return Num(a >= b ? 1 : 0);
}
EV FErf(Evaluator& ev, const Node& call) {
  YC_NUM(lo, 0);
  if (!Has(call, 1)) return Result(std::erf(lo));
  YC_NUM(hi, 1);
  return Result(std::erf(hi) - std::erf(lo));
}
EV FErfPrecise(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  return Result(std::erf(x));
}
EV FErfc(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  return Result(std::erfc(x));
}

// ---- Bessel functions (n is truncated; n < 0 is #NUM!) ----

bool BesselArgs(Evaluator& ev, const Node& call, double* x, int* n, Value* err) {
  if (!ev.ArgNumber(call.kids[0], x, err)) return false;
  double d;
  if (!ev.ArgNumber(call.kids[1], &d, err)) return false;
  d = std::trunc(d);
  if (d < 0 || d > 1000) {
    *err = Value::Error(Err::Num);
    return false;
  }
  *n = static_cast<int>(d);
  return true;
}

EV FBesselJ(Evaluator& ev, const Node& call) {
  double x;
  int n;
  Value e;
  if (!BesselArgs(ev, call, &x, &n, &e)) return EV::S(e);
  const double v = std::cyl_bessel_j(static_cast<double>(n), std::fabs(x));
  return Result(x < 0 && (n & 1) ? -v : v);
}
EV FBesselI(Evaluator& ev, const Node& call) {
  double x;
  int n;
  Value e;
  if (!BesselArgs(ev, call, &x, &n, &e)) return EV::S(e);
  const double v = std::cyl_bessel_i(static_cast<double>(n), std::fabs(x));
  return Result(x < 0 && (n & 1) ? -v : v);
}
EV FBesselK(Evaluator& ev, const Node& call) {
  double x;
  int n;
  Value e;
  if (!BesselArgs(ev, call, &x, &n, &e)) return EV::S(e);
  if (x <= 0) return Er(Err::Num);
  return Result(std::cyl_bessel_k(static_cast<double>(n), x));
}
EV FBesselY(Evaluator& ev, const Node& call) {
  double x;
  int n;
  Value e;
  if (!BesselArgs(ev, call, &x, &n, &e)) return EV::S(e);
  if (x <= 0) return Er(Err::Num);
  return Result(std::cyl_neumann(static_cast<double>(n), x));
}

// ---- CONVERT ----

enum class UCat { Mass, Distance, Time, Pressure, Force, Energy, Power, Magnetism, Temperature, Volume, Area, Info, Speed };

struct Unit {
  UCat cat;
  double factor;  // in the category's base unit
  bool prefix;    // takes metric prefixes
};

const std::unordered_map<std::string, Unit>& Units() {
  static const std::unordered_map<std::string, Unit> u = [] {
    std::unordered_map<std::string, Unit> m;
    auto add = [&](std::initializer_list<const char*> names, UCat c, double f, bool p) {
      for (const char* n : names) m.emplace(n, Unit{c, f, p});
    };
    const double in = 0.0254, ly = 9.46073047258e15, pica_pt = in / 72, mi = 1609.344, nmi = 1852;
    // mass (grams)
    add({"g"}, UCat::Mass, 1, true);
    add({"sg"}, UCat::Mass, 14593.90293720636, false);
    add({"lbm"}, UCat::Mass, 453.59237, false);
    add({"u"}, UCat::Mass, 1.66053886e-24, true);
    add({"ozm"}, UCat::Mass, 28.349523125, false);
    add({"grain"}, UCat::Mass, 0.06479891, false);
    add({"cwt", "shweight"}, UCat::Mass, 45359.237, false);
    add({"uk_cwt", "lcwt", "hweight"}, UCat::Mass, 50802.34544, false);
    add({"stone"}, UCat::Mass, 6350.29318, false);
    add({"ton"}, UCat::Mass, 907184.74, false);
    add({"uk_ton", "LTON", "brton"}, UCat::Mass, 1016046.9088, false);
    // distance (meters)
    add({"m"}, UCat::Distance, 1, true);
    add({"mi"}, UCat::Distance, mi, false);
    add({"Nmi"}, UCat::Distance, nmi, false);
    add({"in"}, UCat::Distance, in, false);
    add({"ft"}, UCat::Distance, 0.3048, false);
    add({"yd"}, UCat::Distance, 0.9144, false);
    add({"ang"}, UCat::Distance, 1e-10, true);
    add({"ell"}, UCat::Distance, 1.143, false);
    add({"ly"}, UCat::Distance, ly, true);
    add({"parsec", "pc"}, UCat::Distance, 3.08567758128155e16, true);
    add({"Picapt", "Pica"}, UCat::Distance, pica_pt, false);
    add({"pica"}, UCat::Distance, in / 6, false);
    add({"survey_mi"}, UCat::Distance, 1609.347218694437, false);
    // time (seconds)
    add({"yr"}, UCat::Time, 31557600, false);
    add({"day", "d"}, UCat::Time, 86400, false);
    add({"hr"}, UCat::Time, 3600, false);
    add({"mn", "min"}, UCat::Time, 60, false);
    add({"sec", "s"}, UCat::Time, 1, true);
    // pressure (pascals)
    add({"Pa", "p"}, UCat::Pressure, 1, true);
    add({"atm", "at"}, UCat::Pressure, 101325, true);
    add({"mmHg"}, UCat::Pressure, 133.322, true);
    add({"psi"}, UCat::Pressure, 6894.757293168361, false);
    add({"Torr"}, UCat::Pressure, 133.322368421053, false);
    // force (newtons)
    add({"N"}, UCat::Force, 1, true);
    add({"dyn", "dy"}, UCat::Force, 1e-5, true);
    add({"lbf"}, UCat::Force, 4.4482216152605, false);
    add({"pond"}, UCat::Force, 0.00980665, true);
    // energy (joules)
    add({"J"}, UCat::Energy, 1, true);
    add({"e"}, UCat::Energy, 1e-7, true);
    add({"c"}, UCat::Energy, 4.184, true);
    add({"cal"}, UCat::Energy, 4.1868, true);
    add({"eV", "ev"}, UCat::Energy, 1.602176487e-19, true);
    add({"HPh", "hh"}, UCat::Energy, 2684519.537696172, false);
    add({"Wh", "wh"}, UCat::Energy, 3600, true);
    add({"flb"}, UCat::Energy, 1.3558179483314, false);
    add({"BTU", "btu"}, UCat::Energy, 1055.05585262, false);
    // power (watts)
    add({"HP", "h"}, UCat::Power, 745.6998715822702, false);
    add({"PS"}, UCat::Power, 735.49875, false);
    add({"W", "w"}, UCat::Power, 1, true);
    // magnetism (tesla)
    add({"T"}, UCat::Magnetism, 1, true);
    add({"ga"}, UCat::Magnetism, 1e-4, true);
    // temperature (handled separately; factor unused)
    add({"C", "cel", "F", "fah", "K", "kel", "Rank", "Reau"}, UCat::Temperature, 1, false);
    // volume (cubic meters)
    add({"tsp"}, UCat::Volume, 4.92892159375e-6, false);
    add({"tspm"}, UCat::Volume, 5e-6, false);
    add({"tbs"}, UCat::Volume, 1.478676478125e-5, false);
    add({"oz"}, UCat::Volume, 2.95735295625e-5, false);
    add({"cup"}, UCat::Volume, 2.365882365e-4, false);
    add({"pt", "us_pt"}, UCat::Volume, 4.73176473e-4, false);
    add({"uk_pt"}, UCat::Volume, 5.6826125e-4, false);
    add({"qt"}, UCat::Volume, 9.46352946e-4, false);
    add({"uk_qt"}, UCat::Volume, 1.1365225e-3, false);
    add({"gal"}, UCat::Volume, 3.785411784e-3, false);
    add({"uk_gal"}, UCat::Volume, 4.54609e-3, false);
    add({"l", "L", "lt"}, UCat::Volume, 1e-3, true);
    add({"ang3", "ang^3"}, UCat::Volume, 1e-30, true);
    add({"barrel"}, UCat::Volume, 0.158987294928, false);
    add({"bushel"}, UCat::Volume, 0.03523907016688, false);
    add({"ft3", "ft^3"}, UCat::Volume, 0.028316846592, false);
    add({"in3", "in^3"}, UCat::Volume, 1.6387064e-5, false);
    add({"ly3", "ly^3"}, UCat::Volume, ly * ly * ly, false);
    add({"m3", "m^3"}, UCat::Volume, 1, true);
    add({"mi3", "mi^3"}, UCat::Volume, mi * mi * mi, false);
    add({"yd3", "yd^3"}, UCat::Volume, 0.764554857984, false);
    add({"Nmi3", "Nmi^3"}, UCat::Volume, nmi * nmi * nmi, false);
    add({"Picapt3", "Picapt^3", "Pica3", "Pica^3"}, UCat::Volume, pica_pt * pica_pt * pica_pt, false);
    add({"GRT", "regton"}, UCat::Volume, 2.8316846592, false);
    add({"MTON"}, UCat::Volume, 1.13267386368, false);
    // area (square meters)
    add({"uk_acre"}, UCat::Area, 4046.8564224, false);
    add({"us_acre"}, UCat::Area, 4046.872609874252, false);
    add({"ang2", "ang^2"}, UCat::Area, 1e-20, true);
    add({"ar"}, UCat::Area, 100, true);
    add({"ft2", "ft^2"}, UCat::Area, 0.09290304, false);
    add({"ha"}, UCat::Area, 10000, false);
    add({"in2", "in^2"}, UCat::Area, 0.00064516, false);
    add({"ly2", "ly^2"}, UCat::Area, ly * ly, false);
    add({"m2", "m^2"}, UCat::Area, 1, true);
    add({"Morgen"}, UCat::Area, 2500, false);
    add({"mi2", "mi^2"}, UCat::Area, mi * mi, false);
    add({"Nmi2", "Nmi^2"}, UCat::Area, nmi * nmi, false);
    add({"Picapt2", "Pica2", "Pica^2", "Picapt^2"}, UCat::Area, pica_pt * pica_pt, false);
    add({"yd2", "yd^2"}, UCat::Area, 0.83612736, false);
    // information (bits)
    add({"bit"}, UCat::Info, 1, true);
    add({"byte"}, UCat::Info, 8, true);
    // speed (meters per second)
    add({"admkn"}, UCat::Speed, 0.514773333333333, false);
    add({"kn"}, UCat::Speed, 0.514444444444444, false);
    add({"m/h", "m/hr"}, UCat::Speed, 1.0 / 3600, true);
    add({"m/s", "m/sec"}, UCat::Speed, 1, true);
    add({"mph"}, UCat::Speed, 0.44704, false);
    return m;
  }();
  return u;
}

// unit name (with an optional prefix) -> category and factor; false when unknown
bool LookupUnit(const std::string& name, UCat* cat, double* factor, std::string* base) {
  const auto& u = Units();
  if (auto it = u.find(name); it != u.end()) {
    *cat = it->second.cat;
    *factor = it->second.factor;
    *base = name;
    return true;
  }
  struct P {
    const char* p;
    double f;
  };
  static const P kDec[] = {{"Y", 1e24}, {"Z", 1e21}, {"E", 1e18}, {"P", 1e15}, {"T", 1e12}, {"G", 1e9}, {"M", 1e6},  {"k", 1e3},
                           {"h", 1e2},  {"da", 1e1}, {"e", 1e1},  {"d", 1e-1}, {"c", 1e-2}, {"m", 1e-3}, {"u", 1e-6}, {"n", 1e-9},
                           {"p", 1e-12}, {"f", 1e-15}, {"a", 1e-18}, {"z", 1e-21}, {"y", 1e-24}};
  static const P kBin[] = {{"ki", 1024.0}, {"Mi", 1048576.0}, {"Gi", 1073741824.0}, {"Ti", 1099511627776.0}, {"Pi", 1125899906842624.0},
                           {"Ei", 1152921504606846976.0}, {"Zi", 1180591620717411303424.0}, {"Yi", 1208925819614629174706176.0}};
  auto try_prefix = [&](const P* list, size_t n, bool binary) {
    for (size_t i = 0; i < n; ++i) {
      const size_t len = std::strlen(list[i].p);
      if (name.size() <= len || name.compare(0, len, list[i].p) != 0) continue;
      auto it = u.find(name.substr(len));
      if (it == u.end() || !it->second.prefix) continue;
      // binary prefixes are for bits and bytes only; decimal ones also take them (kbyte is 1000 bytes, as in Excel)
      if (binary && it->second.cat != UCat::Info) continue;
      double f = list[i].f;
      // squared / cubed units take the prefix to that power (km2 = 1e6 m2)
      const std::string& b = it->first;
      if (b.back() == '2') f = f * f;
      else if (b.back() == '3') f = f * f * f;
      *cat = it->second.cat;
      *factor = it->second.factor * f;
      *base = it->first;
      return true;
    }
    return false;
  };
  return try_prefix(kBin, std::size(kBin), true) || try_prefix(kDec, std::size(kDec), false);
}

double ToKelvin(const std::string& u, double v) {
  if (u == "C" || u == "cel") return v + 273.15;
  if (u == "F" || u == "fah") return (v - 32) * 5.0 / 9.0 + 273.15;
  if (u == "Rank") return v * 5.0 / 9.0;
  if (u == "Reau") return v * 1.25 + 273.15;
  return v;  // K, kel
}
double FromKelvin(const std::string& u, double k) {
  if (u == "C" || u == "cel") return k - 273.15;
  if (u == "F" || u == "fah") return (k - 273.15) * 9.0 / 5.0 + 32;
  if (u == "Rank") return k * 9.0 / 5.0;
  if (u == "Reau") return (k - 273.15) * 0.8;
  return k;
}

EV FConvert(Evaluator& ev, const Node& call) {
  YC_NUM(v, 0);
  YC_TEXT(from, 1);
  YC_TEXT(to, 2);
  UCat c1, c2;
  double f1, f2;
  std::string b1, b2;
  if (!LookupUnit(from, &c1, &f1, &b1) || !LookupUnit(to, &c2, &f2, &b2) || c1 != c2) return Er(Err::NA);
  if (c1 == UCat::Temperature) {
    // only K / kel take prefixes, and they were looked up as plain units
    return Result(FromKelvin(b2, ToKelvin(b1, v * (b1 == from ? 1 : f1))) / (b2 == to ? 1 : f2));
  }
  return Result(v * f1 / f2);
}

// ---- complex numbers ----

using Cx = std::complex<double>;

// text such as "3+4i", "-2.5e3-i", "j", or a number; *suffix gets 'i' or 'j' (0 when none)
bool ParseComplex(std::string s, Cx* out, char* suffix) {
  *suffix = 0;
  while (!s.empty() && s.back() == ' ') s.pop_back();
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  if (s.empty()) {
    *out = 0;
    return true;
  }
  if (s.back() == 'i' || s.back() == 'j') {
    *suffix = s.back();
    s.pop_back();
    // find where the imaginary part starts: the last + or - that is not an exponent sign and not the first char
    size_t pos = std::string::npos;
    for (size_t k = s.size(); k-- > 1;) {
      if ((s[k] == '+' || s[k] == '-') && s[k - 1] != 'e' && s[k - 1] != 'E') {
        pos = k;
        break;
      }
    }
    std::string re = pos == std::string::npos ? "" : s.substr(0, pos);
    std::string im = pos == std::string::npos ? s : s.substr(pos);
    if (im.empty() || im == "+") im = "1";
    else if (im == "-") im = "-1";
    auto num = [](const std::string& t, double* d) {
      if (t.empty()) {
        *d = 0;
        return true;
      }
      char* end = nullptr;
      *d = std::strtod(t.c_str(), &end);
      return end && *end == 0 && std::isfinite(*d);
    };
    double r, i;
    if (!num(re, &r) || !num(im, &i)) return false;
    *out = Cx(r, i);
    return true;
  }
  char* end = nullptr;
  const double r = std::strtod(s.c_str(), &end);
  if (!end || *end != 0 || !std::isfinite(r)) return false;
  *out = Cx(r, 0);
  return true;
}

std::string Fmt15(double d) {
  if (d == 0) return "0";
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.15G", d);
  return buf;
}

std::string FormatComplex(Cx z, char suffix) {
  if (!suffix) suffix = 'i';
  // values that are tiny compared with the other part are noise from the arithmetic (cos(pi/2)...)
  double re = z.real(), im = z.imag();
  if (std::fabs(re) < 1e-15 * std::fabs(im)) re = 0;
  if (std::fabs(im) < 1e-15 * std::fabs(re)) im = 0;
  if (im == 0) return Fmt15(re);
  std::string ims = im == 1 ? "" : im == -1 ? "-" : Fmt15(im);
  std::string out = re == 0 ? "" : Fmt15(re);
  if (re != 0 && im > 0) out += "+";
  return out + ims + suffix;
}

bool ArgComplex(Evaluator& ev, const Node& n, Cx* z, char* suffix, Value* err) {
  const Value v = ev.Scalar(n);
  if (v.IsError()) {
    *err = v;
    return false;
  }
  if (v.IsBool()) {
    *err = Value::Error(Err::Value);
    return false;
  }
  if (v.IsNumeric()) {
    *z = Cx(v.AsDouble(), 0);
    *suffix = 0;
    return true;
  }
  if (!ParseComplex(Evaluator::ToText(v), z, suffix)) {
    *err = Value::Error(Err::Num);
    return false;
  }
  return true;
}

#define YC_CX(var, sfx, i)                                                   \
  Cx var;                                                                    \
  char sfx = 0;                                                              \
  {                                                                          \
    Value _e;                                                                \
    if (!ArgComplex(ev, call.kids[(i)], &var, &sfx, &_e)) return EV::S(_e); \
  }

EV CxOut(Cx z, char suffix) {
  if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) return Er(Err::Num);
  return Str(FormatComplex(z, suffix));
}

EV FComplex(Evaluator& ev, const Node& call) {
  YC_NUM(re, 0);
  YC_NUM(im, 1);
  char suffix = 'i';
  if (Has(call, 2)) {
    YC_TEXT(s, 2);
    if (s == "j") suffix = 'j';
    else if (!s.empty() && s != "i") return Er(Err::Value);
  }
  return Str(FormatComplex(Cx(re, im), suffix));
}

EV FImReal(Evaluator& ev, const Node& call) {
  YC_CX(z, s, 0);
  (void)s;
  return Num(z.real());
}
EV FImaginary(Evaluator& ev, const Node& call) {
  YC_CX(z, s, 0);
  (void)s;
  return Num(z.imag());
}
EV FImAbs(Evaluator& ev, const Node& call) {
  YC_CX(z, s, 0);
  (void)s;
  return Result(std::abs(z));
}
EV FImArgument(Evaluator& ev, const Node& call) {
  YC_CX(z, s, 0);
  (void)s;
  if (z == Cx(0, 0)) return Er(Err::Div0);
  return Result(std::arg(z));
}

template <class F>
EV Unary(Evaluator& ev, const Node& call, F f, bool zero_is_num = false) {
  YC_CX(z, s, 0);
  if (zero_is_num && z == Cx(0, 0)) return Er(Err::Num);
  return CxOut(f(z), s);
}

EV FImConjugate(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::conj(z); }); }
EV FImSqrt(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::sqrt(z); }); }
EV FImExp(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::exp(z); }); }
EV FImLn(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::log(z); }, true); }
EV FImLog10(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::log(z) / std::log(10.0); }, true); }
EV FImLog2(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::log(z) / std::log(2.0); }, true); }
EV FImSin(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::sin(z); }); }
EV FImCos(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::cos(z); }); }
EV FImTan(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::tan(z); }); }
EV FImSinh(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::sinh(z); }); }
EV FImCosh(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return std::cosh(z); }); }
EV FImCot(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return Cx(1) / std::tan(z); }, true); }
EV FImCsc(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return Cx(1) / std::sin(z); }, true); }
EV FImCsch(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return Cx(1) / std::sinh(z); }, true); }
EV FImSec(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return Cx(1) / std::cos(z); }); }
EV FImSech(Evaluator& ev, const Node& call) { return Unary(ev, call, [](Cx z) { return Cx(1) / std::cosh(z); }); }

EV FImPower(Evaluator& ev, const Node& call) {
  YC_CX(z, s, 0);
  YC_NUM(p, 1);
  if (z == Cx(0, 0)) return p > 0 ? Str("0") : Er(Err::Num);
  return CxOut(std::pow(z, p), s);
}
EV FImDiv(Evaluator& ev, const Node& call) {
  YC_CX(a, s1, 0);
  YC_CX(b, s2, 1);
  if (s1 && s2 && s1 != s2) return Er(Err::Value);
  if (b == Cx(0, 0)) return Er(Err::Num);
  return CxOut(a / b, s1 ? s1 : s2);
}
EV FImSub(Evaluator& ev, const Node& call) {
  YC_CX(a, s1, 0);
  YC_CX(b, s2, 1);
  if (s1 && s2 && s1 != s2) return Er(Err::Value);
  return CxOut(a - b, s1 ? s1 : s2);
}

// IMSUM / IMPRODUCT: every argument may be a range or array of complex numbers (blanks are skipped)
EV Fold(Evaluator& ev, const Node& call, bool product) {
  Cx acc = product ? Cx(1, 0) : Cx(0, 0);
  char suffix = 0;
  for (const Node& k : call.kids) {
    const auto arr = ev.ToArray(ev.Eval(k));
    if (!arr) return Er(Err::Value);
    for (const Value& v : arr->v) {
      if (v.IsEmpty()) continue;
      if (v.IsError()) return EV::S(v);
      Cx z;
      char s = 0;
      if (v.IsNumeric()) z = Cx(v.AsDouble(), 0);
      else if (v.IsBool() || !ParseComplex(Evaluator::ToText(v), &z, &s)) return Er(Err::Num);
      if (s && suffix && s != suffix) return Er(Err::Value);
      if (s) suffix = s;
      acc = product ? acc * z : acc + z;
    }
  }
  return CxOut(acc, suffix);
}
EV FImSum(Evaluator& ev, const Node& call) { return Fold(ev, call, false); }
EV FImProduct(Evaluator& ev, const Node& call) { return Fold(ev, call, true); }

}  // namespace

void RegisterEngineering(std::vector<FuncInfo>& t) {
  t.push_back({"BIN2DEC", 1, 1, FBin2Dec});
  t.push_back({"BIN2HEX", 1, 2, FBin2Hex});
  t.push_back({"BIN2OCT", 1, 2, FBin2Oct});
  t.push_back({"DEC2BIN", 1, 2, FDec2Bin});
  t.push_back({"DEC2HEX", 1, 2, FDec2Hex});
  t.push_back({"DEC2OCT", 1, 2, FDec2Oct});
  t.push_back({"HEX2BIN", 1, 2, FHex2Bin});
  t.push_back({"HEX2DEC", 1, 1, FHex2Dec});
  t.push_back({"HEX2OCT", 1, 2, FHex2Oct});
  t.push_back({"OCT2BIN", 1, 2, FOct2Bin});
  t.push_back({"OCT2DEC", 1, 1, FOct2Dec});
  t.push_back({"OCT2HEX", 1, 2, FOct2Hex});
  t.push_back({"BITAND", 2, 2, FBitAnd});
  t.push_back({"BITOR", 2, 2, FBitOr});
  t.push_back({"BITXOR", 2, 2, FBitXor});
  t.push_back({"BITLSHIFT", 2, 2, FBitLShift});
  t.push_back({"BITRSHIFT", 2, 2, FBitRShift});
  t.push_back({"DELTA", 1, 2, FDelta});
  t.push_back({"GESTEP", 1, 2, FGeStep});
  t.push_back({"ERF", 1, 2, FErf});
  t.push_back({"ERF.PRECISE", 1, 1, FErfPrecise});
  t.push_back({"ERFC", 1, 1, FErfc});
  t.push_back({"ERFC.PRECISE", 1, 1, FErfc});
  t.push_back({"BESSELI", 2, 2, FBesselI});
  t.push_back({"BESSELJ", 2, 2, FBesselJ});
  t.push_back({"BESSELK", 2, 2, FBesselK});
  t.push_back({"BESSELY", 2, 2, FBesselY});
  t.push_back({"CONVERT", 3, 3, FConvert});
  t.push_back({"COMPLEX", 2, 3, FComplex});
  t.push_back({"IMREAL", 1, 1, FImReal});
  t.push_back({"IMAGINARY", 1, 1, FImaginary});
  t.push_back({"IMABS", 1, 1, FImAbs});
  t.push_back({"IMARGUMENT", 1, 1, FImArgument});
  t.push_back({"IMCONJUGATE", 1, 1, FImConjugate});
  t.push_back({"IMSQRT", 1, 1, FImSqrt});
  t.push_back({"IMEXP", 1, 1, FImExp});
  t.push_back({"IMLN", 1, 1, FImLn});
  t.push_back({"IMLOG10", 1, 1, FImLog10});
  t.push_back({"IMLOG2", 1, 1, FImLog2});
  t.push_back({"IMSIN", 1, 1, FImSin});
  t.push_back({"IMCOS", 1, 1, FImCos});
  t.push_back({"IMTAN", 1, 1, FImTan});
  t.push_back({"IMSINH", 1, 1, FImSinh});
  t.push_back({"IMCOSH", 1, 1, FImCosh});
  t.push_back({"IMCOT", 1, 1, FImCot});
  t.push_back({"IMCSC", 1, 1, FImCsc});
  t.push_back({"IMCSCH", 1, 1, FImCsch});
  t.push_back({"IMSEC", 1, 1, FImSec});
  t.push_back({"IMSECH", 1, 1, FImSech});
  t.push_back({"IMPOWER", 2, 2, FImPower});
  t.push_back({"IMDIV", 2, 2, FImDiv});
  t.push_back({"IMSUB", 2, 2, FImSub});
  t.push_back({"IMSUM", 1, 255, FImSum});
  t.push_back({"IMPRODUCT", 1, 255, FImProduct});
  (void)kPi;
}

}  // namespace yc::formula::fn
