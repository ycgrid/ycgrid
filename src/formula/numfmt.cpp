// SPDX-License-Identifier: Apache-2.0
#include "formula/numfmt.h"

#include "core/decimal.h"
#include "formula/datetime.h"
#include "formula/functions.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace yc::formula {

using core::Value;

namespace {

enum class TK : uint8_t {
  Lit,
  D0,       // 0
  DHash,    // #
  DQ,       // ?
  Point,
  Percent,
  Exp,      // E+ / E-
  Slash,    // the / of fractions
  Text,     // @
  Year, Month, Day, Hour, Minute, Second, AmPm, ElapsedH, ElapsedM, ElapsedS, SubSec, RocYear, Era,
  General,
  DenLit,   // the fixed denominator of a fraction (# ?/16)
  Fill,     // *x: x repeated to fill the cell
};

struct FTok {
  TK k;
  std::string lit;
  int n = 0;          // length (yy, mmm...) or number of decimals
  bool plus = false;  // E+
  int ampm = 0;       // 1 = AM/PM, 2 = A/P, 3 = Chinese AM/PM marker
};

struct Section {
  std::vector<FTok> t;
  uint32_t color = 0;
  bool has_cond = false;
  std::string cond_op;
  double cond_val = 0;
  bool is_date = false, is_text = false, is_general = false, has_ampm = false;
  bool thousands = false, has_point = false, has_exp = false, has_fraction = false;
  bool locale_zh = false;
  int lcid = 0;  // the locale of [$-404] / [$-411] / [$-412] / [$-804] / [$-409] (0: none given)
  int dbnum = 0;  // [DBNum1] Chinese numerals, [DBNum2] the financial ones, [DBNum3] full-width digits
  bool modifier = false;  // a [...] was read (a color, a condition, a locale): alone, the section is General
  int percent = 0, scale = 0;
  int int_slots = 0, frac_slots = 0, exp_slots = 0, num_slots = 0, den_slots = 0;
  int den_fixed = 0;
  bool epoch = false;
  double mul = 1;  // [YC] [mul=10000]: the value is multiplied before formatting (basis points, per-mille...)
  int tz_mode = 0;  // 0 local, 1 UTC, 2 fixed offset
  int tz_offset_min = 0;
};

uint32_t ColorByName(std::string name) {
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const std::unordered_map<std::string, uint32_t> kMap = {
      {"black", 0xFF000000}, {"blue", 0xFF0000FF}, {"cyan", 0xFF00FFFF}, {"green", 0xFF00FF00},
      {"magenta", 0xFFFF00FF}, {"red", 0xFFFF0000}, {"white", 0xFFFFFFFF}, {"yellow", 0xFFFFFF00},
      {"\351\273\221\350\211\262", 0xFF000000}, {"\350\227\215\350\211\262", 0xFF0000FF}, {"\351\235\222\350\211\262", 0xFF00FFFF}, {"\347\266\240\350\211\262", 0xFF00FF00},
      {"\346\264\213\347\264\205\350\211\262", 0xFFFF00FF}, {"\347\264\205\350\211\262", 0xFFFF0000}, {"\347\231\275\350\211\262", 0xFFFFFFFF}, {"\351\273\203\350\211\262", 0xFFFFFF00},
  };
  auto it = kMap.find(name);
  if (it != kMap.end()) return it->second;
  if (name.rfind("color", 0) == 0) {
    static const uint32_t kPal[] = {0xFF000000, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF,
                                    0xFF00FFFF, 0xFF800000, 0xFF008000, 0xFF000080, 0xFF808000, 0xFF800080, 0xFF008080,
                                    0xFFC0C0C0, 0xFF808080};
    const int idx = std::atoi(name.c_str() + 5);
    if (idx >= 1 && idx <= 16) return kPal[idx - 1];
    return 0xFF000000;
  }
  return 0;
}

bool IEq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

void AddLit(Section& s, std::string text) {
  if (text.empty()) return;
  if (!s.t.empty() && s.t.back().k == TK::Lit) s.t.back().lit += text;
  else s.t.push_back(FTok{TK::Lit, std::move(text)});
}

Section ParseSection(std::string_view c) {
  Section s;
  size_t i = 0;
  bool after_point = false, after_exp = false, after_slash = false;
  int digits_before_slash = 0;
  while (i < c.size()) {
    const char ch = c[i];
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (ch == '"') {
      size_t j = c.find('"', i + 1);
      if (j == std::string_view::npos) j = c.size();
      AddLit(s, std::string(c.substr(i + 1, j - i - 1)));
      i = j + 1;
      continue;
    }
    if (ch == '\\') {
      if (i + 1 < c.size()) {
        // may be a multi-byte character
        size_t len = 1;
        const unsigned char nb = static_cast<unsigned char>(c[i + 1]);
        if (nb >= 0xF0) len = 4;
        else if (nb >= 0xE0) len = 3;
        else if (nb >= 0xC0) len = 2;
        AddLit(s, std::string(c.substr(i + 1, len)));
        i += 1 + len;
      } else {
        ++i;
      }
      continue;
    }
    if (ch == '_') {  // space for alignment
      AddLit(s, " ");
      i += 2;
      continue;
    }
    if (ch == '*') {  // repeat fill: \x02 marks where FormatValue's caller repeats the character
      size_t len = 0;
      if (i + 1 < c.size()) {
        const unsigned char nb = static_cast<unsigned char>(c[i + 1]);
        len = nb >= 0xF0 ? 4 : nb >= 0xE0 ? 3 : nb >= 0xC0 ? 2 : 1;
        len = std::min(len, c.size() - i - 1);
      }
      if (len > 0) s.t.push_back(FTok{TK::Fill, std::string(c.substr(i + 1, len))});
      i += 1 + len;
      continue;
    }
    if (ch == '[') {
      const size_t j = c.find(']', i);
      if (j == std::string_view::npos) {
        ++i;
        continue;
      }
      std::string b(c.substr(i + 1, j - i - 1));
      i = j + 1;
      s.modifier = true;
      std::string lb = b;
      for (char& x : lb) x = static_cast<char>(std::tolower(static_cast<unsigned char>(x)));
      if (lb == "h" || lb == "hh") s.t.push_back(FTok{TK::ElapsedH, "", static_cast<int>(lb.size())}), s.is_date = true;
      else if (lb == "m" || lb == "mm") s.t.push_back(FTok{TK::ElapsedM, "", static_cast<int>(lb.size())}), s.is_date = true;
      else if (lb == "s" || lb == "ss") s.t.push_back(FTok{TK::ElapsedS, "", static_cast<int>(lb.size())}), s.is_date = true;
      else if (lb == "epoch") s.epoch = true, s.is_date = true;
      else if (lb.rfind("mul=", 0) == 0) {
        const double m = std::strtod(lb.c_str() + 4, nullptr);
        if (m != 0 && std::isfinite(m)) s.mul = m;
      } else if (lb == "utc") s.tz_mode = 1;
      else if (lb == "local") s.tz_mode = 0;
      else if (lb.rfind("tz=", 0) == 0) {
        const std::string v = b.substr(3);
        if (!v.empty() && (v[0] == '+' || v[0] == '-')) {
          const int sign = v[0] == '-' ? -1 : 1;
          const int hh = std::atoi(v.substr(1, 2).c_str());
          const int mm = v.size() >= 6 ? std::atoi(v.substr(4, 2).c_str()) : 0;
          s.tz_mode = 2;
          s.tz_offset_min = sign * (hh * 60 + mm);
        } else if (lb.find("taipei") != std::string::npos || lb.find("shanghai") != std::string::npos || lb.find("hong_kong") != std::string::npos) {
          s.tz_mode = 2;
          s.tz_offset_min = 480;
        } else if (lb.find("tokyo") != std::string::npos) {
          s.tz_mode = 2;
          s.tz_offset_min = 540;
        } else {
          s.tz_mode = 1;
        }
      } else if (!b.empty() && (b[0] == '>' || b[0] == '<' || b[0] == '=')) {
        size_t k = 0;
        while (k < b.size() && (b[k] == '>' || b[k] == '<' || b[k] == '=')) ++k;
        s.has_cond = true;
        s.cond_op = b.substr(0, k);
        s.cond_val = std::atof(b.c_str() + k);
      } else if (lb == "dbnum1" || lb == "dbnum2" || lb == "dbnum3") {
        s.dbnum = lb.back() - '0';
      } else if (!b.empty() && b[0] == '$') {
        // [$NT$-404]: currency symbol and locale
        const size_t dash = b.find('-');
        const std::string sym = b.substr(1, dash == std::string::npos ? std::string::npos : dash - 1);
        AddLit(s, sym);
        if (dash != std::string::npos) s.lcid = static_cast<int>(std::strtol(b.c_str() + dash + 1, nullptr, 16) & 0xFFFF);
        if (b.find("404") != std::string::npos || lb.find("-zh") != std::string::npos) s.locale_zh = true;
      } else {
        const uint32_t col = ColorByName(b);
        if (col) s.color = col;
      }
      continue;
    }
    // General / its Chinese name
    if ((ch == 'G' || ch == 'g') && (IEq(c.substr(i, 7), "General"))) {
      s.t.push_back(FTok{TK::General});
      s.is_general = true;
      i += 7;
      continue;
    }
    if (c.substr(i).rfind("G/\351\200\232\347\224\250\346\240\274\345\274\217", 0) == 0) {
      s.t.push_back(FTok{TK::General});
      s.is_general = true;
      i += std::string_view("G/\351\200\232\347\224\250\346\240\274\345\274\217").size();
      continue;
    }
    if (c.substr(i).rfind("\344\270\212\345\215\210/\344\270\213\345\215\210", 0) == 0) {
      s.t.push_back(FTok{TK::AmPm, "", 0, false, 3});
      s.has_ampm = true;
      s.is_date = true;
      i += std::string_view("\344\270\212\345\215\210/\344\270\213\345\215\210").size();
      continue;
    }
    if ((ch == 'a' || ch == 'A') && i + 2 < c.size() && (c[i + 1] == 'a' || c[i + 1] == 'A') && (c[i + 2] == 'a' || c[i + 2] == 'A')) {
      // aaa / aaaa: the day of the week in Chinese (\u4e00 / \u661f\u671f\u4e00), as Excel's CJK codes
      size_t j = i;
      while (j < c.size() && (c[j] == 'a' || c[j] == 'A')) ++j;
      s.t.push_back(FTok{TK::Day, "zh", static_cast<int>(j - i)});
      s.is_date = true;
      i = j;
      continue;
    }
    if (IEq(c.substr(i, 5), "AM/PM")) {
      // the marker keeps the code's case, as Excel shows it: AM/PM gives PM, am/pm gives pm
      s.t.push_back(FTok{TK::AmPm, std::string(c.substr(i, 5)), 0, false, 1});
      s.has_ampm = true;
      s.is_date = true;
      i += 5;
      continue;
    }
    if (IEq(c.substr(i, 3), "A/P")) {
      s.t.push_back(FTok{TK::AmPm, std::string(c.substr(i, 3)), 0, false, 2});
      s.has_ampm = true;
      s.is_date = true;
      i += 3;
      continue;
    }
    const char lc = static_cast<char>(std::tolower(uch));
    if (lc == 'y' || lc == 'm' || lc == 'd' || lc == 'h' || lc == 's' || lc == 'e' || lc == 'g' || lc == 'b') {
      size_t j = i;
      while (j < c.size() && std::tolower(static_cast<unsigned char>(c[j])) == lc) ++j;
      const int n = static_cast<int>(j - i);
      TK k = TK::Lit;
      switch (lc) {
        case 'y': k = TK::Year; break;
        case 'm': k = TK::Month; break;
        case 'd': k = TK::Day; break;
        case 'h': k = TK::Hour; break;
        case 's': k = TK::Second; break;
        case 'e': k = TK::RocYear; break;
        case 'g': k = TK::Era; break;
        case 'b': k = TK::Year; break;  // Buddhist calendar (simplified: treated as Gregorian)
        default: break;
      }
      if (lc == 'e' && (after_exp || s.int_slots > 0) && !s.is_date) {
        // the e of 0.00e+00 belongs to scientific notation, handled below
      } else {
        s.t.push_back(FTok{k, "", n});
        s.is_date = true;
        i = j;
        continue;
      }
    }
    if ((ch == 'E' || ch == 'e') && i + 1 < c.size() && (c[i + 1] == '+' || c[i + 1] == '-')) {
      s.t.push_back(FTok{TK::Exp, "", 0, c[i + 1] == '+'});
      s.has_exp = true;
      after_exp = true;
      i += 2;
      continue;
    }
    switch (ch) {
      case '0':
      case '#':
      case '?': {
        const TK k = ch == '0' ? TK::D0 : (ch == '#' ? TK::DHash : TK::DQ);
        s.t.push_back(FTok{k});
        if (after_exp) ++s.exp_slots;
        else if (after_slash) ++s.den_slots;
        else if (after_point) ++s.frac_slots;
        else {
          ++s.int_slots;
          ++digits_before_slash;
        }
        ++i;
        continue;
      }
      case '.':
        if (s.is_date) {
          // fractional seconds: .0 .00 .000 (up to 9 digits)
          size_t j = i + 1;
          while (j < c.size() && c[j] == '0') ++j;
          if (j > i + 1) {
            s.t.push_back(FTok{TK::SubSec, "", static_cast<int>(j - i - 1)});
            i = j;
            continue;
          }
          AddLit(s, ".");
          ++i;
          continue;
        }
        s.t.push_back(FTok{TK::Point});
        s.has_point = true;
        after_point = true;
        ++i;
        continue;
      case ',':
        // between digit placeholders = thousands separator; after the digit placeholders (before the decimal point or at the end) = scale by /1000
        {
          bool digit_after = false;
          for (size_t j = i + 1; j < c.size(); ++j) {
            if (c[j] == '0' || c[j] == '#' || c[j] == '?') {
              digit_after = true;
              break;
            }
            if (c[j] == '.' || c[j] == ';') break;
            if (c[j] != ',') break;
          }
          if (digit_after && !after_point) s.thousands = true;
          else if (s.int_slots > 0 || s.frac_slots > 0) ++s.scale;
          else AddLit(s, ",");
        }
        ++i;
        continue;
      case '%':
        s.t.push_back(FTok{TK::Percent});
        ++s.percent;
        ++i;
        continue;
      case '/':
        if (!s.is_date && (s.int_slots > 0 || !s.t.empty())) {
          s.t.push_back(FTok{TK::Slash});
          s.has_fraction = true;
          s.num_slots = digits_before_slash;
          after_slash = true;
          // fixed denominator: /8, /16, /100 (its zeros are digits, not placeholders)
          size_t j = i + 1;
          int den = 0;
          while (j < c.size() && c[j] >= '0' && c[j] <= '9' && (den > 0 || c[j] != '0') && den < 100000000) {
            den = den * 10 + (c[j] - '0');
            ++j;
          }
          if (den > 0) {
            s.den_fixed = den;
            s.t.push_back(FTok{TK::DenLit, std::to_string(den)});
            i = j;
            continue;
          }
          ++i;
          continue;
        }
        AddLit(s, "/");
        ++i;
        continue;
      case '@':
        s.t.push_back(FTok{TK::Text});
        s.is_text = true;
        ++i;
        continue;
      case ' ':
        if (s.has_fraction || after_slash) {
          AddLit(s, " ");
          ++i;
          continue;
        }
        [[fallthrough]];
      default: {
        size_t len = 1;
        if (uch >= 0xF0) len = 4;
        else if (uch >= 0xE0) len = 3;
        else if (uch >= 0xC0) len = 2;
        AddLit(s, std::string(c.substr(i, len)));
        i += len;
        continue;
      }
    }
  }
  // fractions: the numerator placeholders are the last group of integer placeholders
  if (s.has_fraction) {
    // count the consecutive digit placeholders before the slash
    int n = 0;
    for (size_t k = 0; k < s.t.size(); ++k) {
      if (s.t[k].k == TK::Slash) break;
      if (s.t[k].k == TK::D0 || s.t[k].k == TK::DHash || s.t[k].k == TK::DQ) ++n;
      else if (s.t[k].k == TK::Lit && n > 0) n = 0;  // "# ?/?", "#-?/4": the text splits whole and numerator
    }
    s.num_slots = n;
    s.int_slots -= n;
  }
  // the ambiguity of m: right after h or before s it means minutes
  if (s.is_date) {
    int last = -1;  // index of the previous date/time token
    for (size_t k = 0; k < s.t.size(); ++k) {
      TK kk = s.t[k].k;
      if (kk == TK::Month && s.t[k].n <= 2) {
        bool minute = false;
        if (last >= 0 && (s.t[static_cast<size_t>(last)].k == TK::Hour || s.t[static_cast<size_t>(last)].k == TK::ElapsedH)) minute = true;
        for (size_t q = k + 1; q < s.t.size(); ++q) {
          const TK nk = s.t[q].k;
          if (nk == TK::Lit) continue;
          if (nk == TK::Second || nk == TK::ElapsedS) minute = true;
          break;
        }
        if (minute) s.t[k].k = TK::Minute;
      }
      if (kk != TK::Lit) last = static_cast<int>(k);
    }
  }
  // only modifiers ([Red], [$-404], [DBNum1], [>5]): the value as General, as Excel shows it (an empty section, as
  // in ;;; , still hides it)
  if (s.modifier && s.t.empty() && !s.is_text) {
    s.t.push_back(FTok{TK::General});
    s.is_general = true;
  }
  if (s.t.empty() && !s.is_text) s.is_general = false;
  return s;
}

std::vector<std::string> SplitSections(std::string_view code) {
  std::vector<std::string> out;
  std::string cur;
  bool q = false;
  int br = 0;
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = code[i];
    if (ch == '"') q = !q;
    if (!q && ch == '[') ++br;
    if (!q && ch == ']') --br;
    if (!q && br == 0 && ch == '\\' && i + 1 < code.size()) {
      cur += ch;
      cur += code[++i];
      continue;
    }
    if (!q && br == 0 && ch == ';') {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += ch;
  }
  out.push_back(cur);
  return out;
}

bool CondMatch(const Section& s, double v) {
  const std::string& o = s.cond_op;
  const double c = s.cond_val;
  if (o == ">") return v > c;
  if (o == ">=") return v >= c;
  if (o == "<") return v < c;
  if (o == "<=") return v <= c;
  if (o == "=") return v == c;
  if (o == "<>") return v != c;
  return false;
}

std::string Repeat(char c, size_t n) { return std::string(n, c); }

// round to frac decimals, return the digit strings of the integer and fraction parts (without sign).
// Like Excel: the value's 15 significant digits are rounded half away from zero (2.5 -> "3", 1.005 -> "1.01"),
// not the binary value with ties to even as printf does.
void SplitDigits(double av, int frac, std::string* ip, std::string* fp) {
  frac = std::clamp(frac, 0, 30);
  ip->clear();
  if (!(av > 0) || !std::isfinite(av)) {
    *fp = std::string(static_cast<size_t>(frac), '0');
    return;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.14e", av);  // d.dddddddddddddde+XX: 15 significant digits
  std::string digits;
  const char* p = buf;
  for (; *p && *p != 'e'; ++p) {
    if (*p >= '0' && *p <= '9') digits += *p;
  }
  const int exp10 = *p == 'e' ? std::atoi(p + 1) : 0;
  // the decimal point sits after point digits of "digits" (padded with zeros on either side)
  int point = exp10 + 1;
  if (point <= 0) {
    digits.insert(0, static_cast<size_t>(-point) + 1, '0');
    point = 1;
  }
  const size_t keep = static_cast<size_t>(point + frac);
  if (digits.size() < keep + 1) digits.append(keep + 1 - digits.size(), '0');
  const bool up = digits[keep] >= '5';
  digits.resize(keep);
  if (up) {
    size_t i = keep;
    while (i > 0) {
      --i;
      if (digits[i] == '9') {
        digits[i] = '0';
      } else {
        ++digits[i];
        break;
      }
      if (i == 0) {  // 9.99 -> 10.0
        digits.insert(0, 1, '1');
        ++point;
      }
    }
  }
  *ip = digits.substr(0, static_cast<size_t>(point));
  *fp = digits.substr(static_cast<size_t>(point));
  const size_t nz = ip->find_first_not_of('0');
  *ip = nz == std::string::npos ? std::string() : ip->substr(nz);
}

// split the digits exactly with Decimal (scale-9 timestamps etc.)
void SplitDigitsDec(const core::Dec& d, int frac, std::string* ip, std::string* fp) {
  core::Dec r;
  if (!core::DecRescale(core::Dec{d.m.Abs(), d.scale}, frac, &r)) {
    SplitDigits(std::fabs(core::DecToDouble(d)), frac, ip, fp);
    return;
  }
  std::string s = r.m.ToString();
  if (frac > 0) {
    if (static_cast<int>(s.size()) <= frac) s.insert(0, static_cast<size_t>(frac - static_cast<int>(s.size()) + 1), '0');
    *ip = s.substr(0, s.size() - static_cast<size_t>(frac));
    *fp = s.substr(s.size() - static_cast<size_t>(frac));
  } else {
    *ip = s;
    fp->clear();
  }
  if (*ip == "0") ip->clear();
}

std::string FormatNumber(const Section& s, double v, const core::Dec* dec, bool use_sign) {
  const bool neg = v < 0 || (dec && dec->m.IsNegative());
  double av = std::fabs(v);
  for (int i = 0; i < s.percent; ++i) av *= 100;
  for (int i = 0; i < s.scale; ++i) av /= 1000;
  av *= std::fabs(s.mul);
  const bool exact_dec = dec && s.percent == 0 && s.scale == 0 && s.mul == 1 && !s.has_exp && !s.has_fraction;

  std::string ip, fp, ep;
  int exponent = 0;
  bool frac_blank = false;  // a fraction format showing a whole number: blanks (or nothing) in the fraction's place
  if (s.has_exp) {
    if (av != 0) {
      exponent = static_cast<int>(std::floor(std::log10(av)));
      const int k = std::max(1, s.int_slots);
      bool eng = false;
      for (const FTok& t : s.t) {
        if (t.k == TK::DHash) eng = true;
        if (t.k == TK::Point || t.k == TK::Exp) break;
      }
      if (eng && k > 1) {
        exponent = static_cast<int>(std::floor(static_cast<double>(exponent) / k)) * k;
      } else {
        exponent -= (k - 1);
      }
      av /= std::pow(10.0, exponent);
      // rounding may carry over to 10
      std::string ti, tf;
      SplitDigits(av, s.frac_slots, &ti, &tf);
      if (static_cast<int>(ti.size()) > k && !(eng && k > 1)) {
        av /= 10;
        ++exponent;
      }
    }
    SplitDigits(av, s.frac_slots, &ip, &fp);
    // zero in scientific notation fills every whole placeholder with 0, as Excel: ##0.0E+0 shows 000.0E+0
    if (av == 0 && s.int_slots > 0) ip.assign(static_cast<size_t>(s.int_slots), '0');
    const int ae = std::abs(exponent);
    ep = std::to_string(ae);
    while (static_cast<int>(ep.size()) < s.exp_slots) ep.insert(0, "0");
  } else if (s.has_fraction) {
    double whole = 0, frac = av;
    if (s.int_slots > 0) {
      whole = std::floor(av);
      frac = av - whole;
    }
    long long num = 0, den = 1;
    if (s.den_fixed > 0) {
      den = s.den_fixed;
      num = std::llround(frac * den);
    } else {
      // Excel's fraction: the last continued-fraction convergent whose denominator has at most den_slots digits,
      // the terms taken in doubles (measured: 0.1 in "?/?" is 0/1, not 1/9; 0.7 is 2/3, not 5/7; 0.3 is 2/7, as
      // 1/0.3333... comes out 2.9999999999999987; pi in "# ?/??" is 3 1/7, not 3 14/99). O(log maxden): a scan of
      // every denominator took 1e9 steps for "?/?????????"
      const long long maxden = static_cast<long long>(std::pow(10, std::clamp(s.den_slots, 1, 9))) - 1;
      long long p0 = 0, q0 = 1, p1 = 1, q1 = 0;  // convergents h(-2)/k(-2), h(-1)/k(-1)
      double x = frac;
      num = 0;
      den = 1;
      for (int it = 0; it < 64; ++it) {
        const double af = std::floor(x);
        const long long a = af > 9e18 ? 9000000000000000000ll : static_cast<long long>(af);
        if (q1 > 0 && a > (maxden - q0) / q1) break;  // the next convergent's denominator is too big
        const long long p2 = a * p1 + p0, q2 = a * q1 + q0;
        if (q1 > 0 && p2 > 2147483647ll) {  // Excel keeps numerators in 32 bits: 2147483646.5 in "?/?" is 2147483647/1 (measured)
          num = std::llround(frac * static_cast<double>(q1));
          break;
        }
        p0 = p1;
        q0 = q1;
        p1 = p2;
        q1 = q2;
        num = p1;
        den = q1;
        const double rem = x - af;
        if (rem < 1e-12 || std::fabs(frac - static_cast<double>(p1) / static_cast<double>(q1)) < 1e-15) break;
        x = 1.0 / rem;
      }
    }
    if (s.int_slots > 0 && num == den) {
      whole += 1;
      num = 0;
    }
    // a whole number in "# ?/?": the whole part (0 too) and blanks where the fraction goes, as Excel ("1    ");
    // a 0 in the numerator ("# 0/4") still shows 0/4
    if (num == 0 && s.int_slots > 0) {
      int k = 0;  // the numerator's placeholders come after the whole part's
      bool zero_in_num = false;
      for (const FTok& t : s.t) {
        if (t.k == TK::Slash) break;
        if (t.k == TK::D0 || t.k == TK::DHash || t.k == TK::DQ) {
          if (k >= s.int_slots && t.k == TK::D0) zero_in_num = true;
          ++k;
        }
      }
      frac_blank = !zero_in_num;
    }
    ip = whole > 0 ? std::to_string(static_cast<long long>(whole)) : std::string(frac_blank ? "0" : "");
    // numerator and denominator borrow fp / ep
    fp = std::to_string(num);
    ep = std::to_string(den);
    if (frac_blank) {
      fp.clear();
      ep.clear();
    }
  } else if (exact_dec) {
    SplitDigitsDec(*dec, s.frac_slots, &ip, &fp);
  } else {
    SplitDigits(av, s.frac_slots, &ip, &fp);
  }

  std::string out;
  // Excel drops the sign of a negative number whose digits all show as zero: -0.4 in "0" shows 0, TEXT(-0.0004,
  // "0.00") 0.00, -1 in "#,##0," 0; a format without digits ("a") or a fraction with a whole part ("# ?/?": -0    )
  // keeps it, one without drops it with a numerator of 0 (-0.1 in "?/?" is 0/1, in "??/??" - 1/10: measured)
  bool shows_zero = s.has_fraction ? s.int_slots == 0 : s.int_slots + s.frac_slots > 0;
  for (const char ch : ip + fp) {
    if (ch >= '1' && ch <= '9') shows_zero = false;
  }
  if (neg && use_sign && !shows_zero) out += '-';

  // integer placeholders
  const int S = s.int_slots;
  const int L = static_cast<int>(ip.size());
  int int_seen = 0;
  int frac_seen = 0;
  int exp_seen = 0;
  int num_seen = 0, den_seen = 0;
  bool after_slash = false, after_exp = false, after_point = false;
  // fraction part: trailing zeros matching # are left out and ? become spaces, past every 0 placeholder too: the
  // digits close up, so "#.##0" shows 0.5 as .50 and 0 as .0 (Excel), not .500 / .000
  std::vector<TK> frac_kinds;
  for (const FTok& t : s.t) {
    if (t.k == TK::Point) after_point = true;
    else if (t.k == TK::Exp) after_exp = true;
    else if (after_point && !after_exp && (t.k == TK::D0 || t.k == TK::DHash || t.k == TK::DQ)) frac_kinds.push_back(t.k);
  }
  std::string frac_out = fp;
  {
    int sig = static_cast<int>(frac_out.size());  // the digits up to the last one that isn't 0
    while (sig > 0 && frac_out[static_cast<size_t>(sig) - 1] == '0') --sig;
    std::string closed = frac_out.substr(0, static_cast<size_t>(sig));
    for (int k = sig; k < static_cast<int>(frac_out.size()) && k < static_cast<int>(frac_kinds.size()); ++k) {
      if (frac_kinds[static_cast<size_t>(k)] == TK::D0) closed += '0';
      else if (frac_kinds[static_cast<size_t>(k)] == TK::DQ) closed += ' ';
    }
    // pad back to one character per placeholder (\x01 = nothing) so the loop below can index it
    closed.resize(std::max(closed.size(), frac_out.size()), '\x01');
    frac_out = closed;
  }
  after_point = after_exp = false;

  auto emit_int_digit = [&](char ch, int pos_from_right) {
    out += ch;
    if (s.thousands && pos_from_right > 0 && pos_from_right % 3 == 0) out += ',';
  };

  const int nslot = s.num_slots;
  const std::string& nstr = fp;
  const std::string& dstr = ep;

  // a fraction: where its numerator starts and its denominator ends, and whether a ? asks for blanks
  size_t num_first = s.t.size(), den_last = 0;
  bool num_has_q = false, whole_zero_slot = false;
  if (s.has_fraction) {
    int k = 0;
    bool slash = false;
    for (size_t i = 0; i < s.t.size(); ++i) {
      const TK kk = s.t[i].k;
      if (kk == TK::Slash) slash = true;
      if (kk == TK::D0 || kk == TK::DHash || kk == TK::DQ || kk == TK::DenLit) {
        if (!slash && k >= S && num_first == s.t.size()) num_first = i;
        if (!slash && k >= S && kk == TK::DQ) num_has_q = true;
        if (slash) den_last = i;
        if (k < S && kk == TK::D0) whole_zero_slot = true;
        if (kk != TK::DenLit) ++k;
      }
    }
  }
  const bool whole_shown = !ip.empty() || whole_zero_slot;

  for (size_t ti = 0; ti < s.t.size(); ++ti) {
    const FTok& t = s.t[ti];
    // the fraction's place of a whole number: spaces when the numerator has ?, else nothing
    const bool in_frac = s.has_fraction && int_seen >= S && ti <= den_last && (ti >= num_first || t.k == TK::Lit);
    if (frac_blank && in_frac) {
      if (num_has_q) {
        if (t.k == TK::Lit || t.k == TK::DenLit) out.append(t.lit.size(), ' ');
        else out += ' ';
      }
      continue;
    }
    // "# #/#": nothing in the whole part, so the text before the numerator goes too (1/2, not " 1/2")
    if (s.has_fraction && !whole_shown && !num_has_q && t.k == TK::Lit && int_seen >= S && ti < num_first) continue;
    switch (t.k) {
      case TK::Lit: out += t.lit; break;
      case TK::DenLit: out += t.lit; break;
      case TK::D0:
      case TK::DHash:
      case TK::DQ: {
        if (s.has_fraction && after_slash) {
          // denominator
          const int pos = den_seen++;
          if (pos < static_cast<int>(dstr.size())) out += dstr[static_cast<size_t>(pos)];
          else if (t.k == TK::DQ) out += ' ';
          if (pos == s.den_slots - 1 && static_cast<int>(dstr.size()) > s.den_slots) out += dstr.substr(static_cast<size_t>(s.den_slots));
          break;
        }
        if (s.has_fraction && int_seen >= S) {
          // numerator: right-aligned
          const int pos_from_right = nslot - 1 - num_seen;
          ++num_seen;
          if (num_seen == 1 && static_cast<int>(nstr.size()) > nslot) out += nstr.substr(0, nstr.size() - static_cast<size_t>(nslot));
          if (pos_from_right < static_cast<int>(nstr.size())) out += nstr[nstr.size() - 1 - static_cast<size_t>(pos_from_right)];
          else if (t.k == TK::DQ) out += ' ';
          break;
        }
        if (after_exp) {
          const int pos_from_right = s.exp_slots - 1 - exp_seen;
          ++exp_seen;
          if (exp_seen == 1 && static_cast<int>(ep.size()) > s.exp_slots) out += ep.substr(0, ep.size() - static_cast<size_t>(s.exp_slots));
          if (pos_from_right < static_cast<int>(ep.size())) out += ep[ep.size() - 1 - static_cast<size_t>(pos_from_right)];
          break;
        }
        if (after_point) {
          const int pos = frac_seen++;
          if (pos < static_cast<int>(frac_out.size())) {
            const char ch = frac_out[static_cast<size_t>(pos)];
            if (ch != '\x01') out += ch;
          }
          break;
        }
        // integer part
        const int pr = S - 1 - int_seen;  // position counted from the right
        if (int_seen == 0 && L > S) {
          for (int k = 0; k < L - S; ++k) emit_int_digit(ip[static_cast<size_t>(k)], L - 1 - k);
        }
        ++int_seen;
        if (pr < L) emit_int_digit(ip[static_cast<size_t>(L - 1 - pr)], pr);
        else if (t.k == TK::D0) emit_int_digit('0', pr);
        else if (t.k == TK::DQ) out += ' ';
        break;
      }
      case TK::Point:
        // no placeholder before the point (".00"): the whole digits still show, as Excel (1234.5 -> 1234.50)
        if (S == 0 && int_seen == 0 && L > 0) {
          for (int k = 0; k < L; ++k) emit_int_digit(ip[static_cast<size_t>(k)], L - 1 - k);
        }
        after_point = true;
        out += '.';
        break;
      case TK::Percent: out += '%'; break;
      case TK::Exp:
        after_exp = true;
        out += 'E';
        if (exponent < 0) out += '-';
        else if (t.plus) out += '+';
        break;
      case TK::Slash:
        after_slash = true;
        out += '/';
        break;
      case TK::Text: break;
      case TK::Fill: out += '\x02'; break;
      case TK::General: out += FormatGeneral(v); break;
      default: break;
    }
  }
  return out;
}

const char* kMonthEn[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
const char* kDayEn[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
// the digits of [DBNum1] (\u25cb for zero, as Excel's Traditional Chinese edition) and [DBNum2] (\u96f6)
const char* kZhDigit1[] = {"\342\227\213", "\344\270\200", "\344\272\214", "\344\270\211", "\345\233\233", "\344\272\224", "\345\205\255", "\344\270\203", "\345\205\253", "\344\271\235"};
const char* kZhDigit2[] = {"\351\233\266", "\345\243\271", "\350\262\263", "\345\217\203", "\350\202\206", "\344\274\215", "\351\231\270", "\346\237\222", "\346\215\214", "\347\216\226"};

// a whole number read in Chinese with its units, as Excel's [DBNum1] / [DBNum2] and NUMBERSTRING (measured against
// Excel 16): 12 is \u4e00\u5341\u4e8c, 1001 \u4e00\u5343\u25cb\u4e00 (a zero inside is \u25cb, \u96f6 in the
// financial digits), and the units stop at \u5146: more \u5146 than 9999 are written digit by digit
// (1E+16 is \u4e00\u25cb\u25cb\u25cb\u25cb\u5146)
std::string ChineseNumber(uint64_t n, int style) {
  static const char* u1[] = {"", "\345\215\201", "\347\231\276", "\345\215\203"};
  static const char* u2[] = {"", "\346\213\276", "\344\275\260", "\344\273\237"};
  static const char* big[] = {"", "\350\220\254", "\345\204\204", "\345\205\206"};
  const char** dg = style == 2 ? kZhDigit2 : kZhDigit1;
  const char** un = style == 2 ? u2 : u1;
  if (n == 0) return dg[0];
  std::string out;
  constexpr uint64_t kZhao = 1000000000000ULL;
  uint64_t rest = n;
  if (n / kZhao >= 10000) {
    for (const char ch : std::to_string(n / kZhao)) out += dg[ch - '0'];
    out += big[3];
    rest = n % kZhao;
  }
  std::vector<int> groups;  // groups of four digits, the lowest first
  for (uint64_t x = rest; x; x /= 10000) groups.push_back(static_cast<int>(x % 10000));
  bool zero = !out.empty() && rest > 0 && rest < kZhao / 10;  // 1E+16 + 5: ...\u5146\u25cb\u4e94
  for (size_t gi = groups.size(); gi-- > 0;) {
    const int g = groups[gi];
    if (g == 0) {
      if (!out.empty()) zero = true;
      continue;
    }
    if (!out.empty() && g < 1000) zero = true;  // 10050: \u4e00\u842c\u25cb\u4e94\u5341
    for (int k = 3; k >= 0; --k) {
      const int digit = g / (k == 3 ? 1000 : k == 2 ? 100 : k == 1 ? 10 : 1) % 10;
      if (digit == 0) {
        if (!out.empty()) zero = true;
        continue;
      }
      if (zero) out += dg[0];
      zero = false;
      out += dg[digit];
      out += un[k];
    }
    out += big[gi];
    zero = false;
  }
  return out;
}

// digits one by one: [DBNum1] / [DBNum2] Chinese digits, [DBNum3] full-width ones
std::string ZhDigits(std::string_view digits, int style) {
  std::string out;
  for (const char ch : digits) {
    if (ch < '0' || ch > '9') {
      out += ch;
    } else if (style == 3) {
      out += "\357\274";  // U+FF10 + digit
      out += static_cast<char>(0x90 + (ch - '0'));
    } else {
      out += (style == 2 ? kZhDigit2 : kZhDigit1)[ch - '0'];
    }
  }
  return out;
}

// a day, month, hour... of a date in Chinese (Excel): read with its units but without the one of ten in [DBNum1]
// (\u5341, \u5341\u4e00, \u4e8c\u5341\u4e5d), a leading zero of a two-digit field as \u25cb (\u25cb\u4e8c),
// zero as \u25cb\u25cb
std::string ZhDateNumber(int64_t v, int width, int style) {
  std::string digits = std::to_string(v);
  while (static_cast<int>(digits.size()) < width) digits.insert(0, "0");
  if (style == 3 || v <= 0) return ZhDigits(digits, style);
  std::string out;
  for (size_t i = 0; i + 1 < digits.size() && digits[i] == '0'; ++i) out += (style == 2 ? kZhDigit2 : kZhDigit1)[0];
  std::string r = ChineseNumber(static_cast<uint64_t>(v), style);
  const std::string one_ten = std::string(kZhDigit1[1]) + "\345\215\201";
  if (style != 2 && r.rfind(one_ten, 0) == 0) r.erase(0, std::string_view(kZhDigit1[1]).size());
  return out + r;
}

// the digits of a formatted number in Chinese: a General section reads the whole part with its units (\u4e00
// \u5343\u4e8c\u767e...), digit placeholders write every digit ("[DBNum1]0" shows 1234 as \u4e00\u4e8c\u4e09
// \u56db); decimals are always digit by digit; [DBNum3] writes full-width digits
std::string DbNumText(const std::string& text, int style, bool read) {
  if (style == 3 || !read) return ZhDigits(text, style);
  std::string out;
  size_t i = 0;
  bool after_point = false;
  while (i < text.size()) {
    const char ch = text[i];
    if (ch >= '0' && ch <= '9') {
      if (after_point) {
        out += (style == 2 ? kZhDigit2 : kZhDigit1)[ch - '0'];
        ++i;
        continue;
      }
      // the whole part: digits and the thousands separators between them
      std::string digits;
      while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || (text[i] == ',' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9'))) {
        if (text[i] != ',') digits += text[i];
        ++i;
      }
      if (digits.size() > 16) {
        // more zhao than 9999: their count digit by digit, then the rest (1E+20: \u4e00\u25cb...\u5146)
        const std::string head = digits.substr(0, digits.size() - 12), tail = digits.substr(digits.size() - 12);
        out += ZhDigits(head, style) + "\345\205\206";
        const uint64_t rest = std::stoull(tail);
        if (rest) out += (rest < 100000000000ULL ? std::string((style == 2 ? kZhDigit2 : kZhDigit1)[0]) : std::string()) + ChineseNumber(rest, style);
        continue;
      }
      out += ChineseNumber(std::stoull(digits), style);
      continue;
    }
    if (ch == '.') after_point = true;
    else if (ch == 'E' || ch == 'e' || ch == ' ') after_point = false;
    out += ch;
    ++i;
  }
  return out;
}

const char* kDayZh[] = {"\346\227\245", "\344\270\200", "\344\272\214", "\344\270\211", "\345\233\233", "\344\272\224", "\345\205\255"};
const char* kDayJa[] = {"\346\227\245", "\346\234\210", "\347\201\253", "\346\260\264", "\346\234\250", "\351\207\221", "\345\234\237"};
const char* kDayKo[] = {"\354\235\274", "\354\233\224", "\355\231\224", "\354\210\230", "\353\252\251", "\352\270\210", "\355\206\240"};

// aaa / aaaa: the day of the week in the section's language (Excel, measured): Traditional Chinese \u9031\u65e5 /
// \u661f\u671f\u65e5 unless [$-804] (\u5468\u65e5), [$-411] (\u65e5 / \u65e5\u66dc\u65e5), [$-412] or [$-409]
std::string AaaDay(int wd, int n, int lcid) {
  switch (lcid) {
    case 0x411: return std::string(kDayJa[wd]) + (n >= 4 ? "\346\233\234\346\227\245" : "");
    case 0x412: return std::string(kDayKo[wd]) + (n >= 4 ? "\354\232\224\354\235\274" : "");
    case 0x409: return n >= 4 ? std::string(kDayEn[wd]) : std::string(kDayEn[wd]).substr(0, 3);
    case 0x804: return std::string(n >= 4 ? "\346\230\237\346\234\237" : "\345\221\250") + kDayZh[wd];
    default: return std::string(n >= 4 ? "\346\230\237\346\234\237" : "\351\200\261") + kDayZh[wd];
  }
}

// Japan's eras for [$-411] g / e: their first day and names (g: letter, gg: first character, ggg: the whole name)
struct JaEra {
  int y, m, d;
  const char* letter;
  const char* name;
};
const JaEra kJaEras[] = {{2019, 5, 1, "R", "\344\273\244\345\222\214"}, {1989, 1, 8, "H", "\345\271\263\346\210\220"},
                         {1926, 12, 25, "S", "\346\230\255\345\222\214"}, {1912, 7, 30, "T", "\345\244\247\346\255\243"},
                         {1868, 9, 8, "M", "\346\230\216\346\262\273"}};
const JaEra* JaEraOf(int y, int m, int d) {
  for (const JaEra& e : kJaEras) {
    if (y > e.y || (y == e.y && (m > e.m || (m == e.m && d >= e.d)))) return &e;
  }
  return nullptr;
}

struct DateParts {
  int y = 1900, mo = 1, d = 0, h = 0, mi = 0, s = 0;
  int64_t ns = 0;  // nanoseconds below the second
  double serial = 0;
};

std::string Pad(int64_t v, int width) {
  std::string s = std::to_string(v);
  while (static_cast<int>(s.size()) < width) s.insert(0, "0");
  return s;
}

std::string FormatDate(const Section& s, const Value& v, bool* bad) {
  DateParts p;
  double serial = 0;
  if (s.epoch) {
    // the value is Unix epoch seconds (Number or Decimal)
    int64_t secs = 0, ns = 0;
    if (v.IsDecimal()) {
      core::Dec d = core::DecFromValue(v);
      core::Dec n9;
      if (core::DecRescale(d, 9, &n9)) {
        int64_t rem = 0;
        const core::Int128 q = n9.m.DivSmall(1000000000ULL, &rem);
        secs = static_cast<int64_t>(q.lo);
        if (q.IsNegative()) secs = -static_cast<int64_t>((-q).lo);
        ns = rem;
        if (ns < 0) {
          ns += 1000000000;
          secs -= 1;
        }
      }
    } else {
      const double x = v.AsDouble();
      secs = static_cast<int64_t>(std::floor(x));
      ns = static_cast<int64_t>(std::llround((x - std::floor(x)) * 1e9));
      if (ns >= 1000000000) {
        ns -= 1000000000;
        ++secs;
      }
    }
    int64_t local = secs;
    if (s.tz_mode == 2) {
      local += int64_t{s.tz_offset_min} * 60;
    } else if (s.tz_mode == 0) {
      std::time_t t = static_cast<std::time_t>(secs);
      std::tm lt{}, gt{};
#if defined(_WIN32)
      localtime_s(&lt, &t);
      gmtime_s(&gt, &t);
#else
      localtime_r(&t, &lt);
      gmtime_r(&t, &gt);
#endif
      lt.tm_isdst = 0;
      gt.tm_isdst = 0;
      local += static_cast<int64_t>(std::difftime(std::mktime(&lt), std::mktime(&gt)));
    }
    const int64_t days = (local >= 0 ? local / 86400 : (local - 86399) / 86400);
    const int64_t sod = local - days * 86400;
    serial = static_cast<double>(days) + EpochSerial();
    if (!SerialToYmd(serial, &p.y, &p.mo, &p.d)) {  // outside the calendar: not 1900-01-00
      *bad = true;
      return {};
    }
    p.h = static_cast<int>(sod / 3600);
    p.mi = static_cast<int>(sod / 60 % 60);
    p.s = static_cast<int>(sod % 60);
    p.ns = ns;
    p.serial = serial + static_cast<double>(sod) / 86400.0;
  } else {
    serial = v.AsDouble();
    // only elapsed time ([h], [mm], [ss] and text): any number, negative ones with a minus, as Excel ([h] of -1.5
    // is -36, [ss] of 1E+15 86400000000000000000); it is rounded to the second, then each unit cut down
    bool elapsed_only = false;
    for (const FTok& t : s.t) {
      if (t.k == TK::ElapsedH || t.k == TK::ElapsedM || t.k == TK::ElapsedS) elapsed_only = true;
      else if (t.k != TK::Lit && t.k != TK::Fill) {
        elapsed_only = false;
        break;
      }
    }
    if (elapsed_only) {
      const double secs = std::round(std::fabs(serial) * 86400.0);
      std::string out;
      if (serial < 0 && secs > 0) out += '-';
      for (const FTok& t : s.t) {
        if (t.k == TK::Lit || t.k == TK::Fill) {
          out += t.k == TK::Fill ? std::string(1, '\x02') : t.lit;
          continue;
        }
        const double units = std::floor(t.k == TK::ElapsedH ? secs / 3600 : t.k == TK::ElapsedM ? secs / 60 : secs);
        std::string digits, frac;
        SplitDigits(units, 0, &digits, &frac);
        if (digits.empty()) digits = "0";
        if (static_cast<int>(digits.size()) < t.n) digits.insert(0, static_cast<size_t>(t.n) - digits.size(), '0');
        out += digits;
      }
      return out;
    }
    if (serial < 0) {  // Excel shows a negative date or time as ##### across the cell
      *bad = true;
      return {};
    }
    // round to the smallest unit shown
    int sub = 0;
    for (const FTok& t : s.t) {
      if (t.k == TK::SubSec) sub = std::max(sub, t.n);
    }
    const double unit = 86400.0 * std::pow(10.0, std::min(sub, 3));
    const double rounded = std::round(serial * unit) / unit;
    if (!SerialToYmd(rounded, &p.y, &p.mo, &p.d)) {
      *bad = true;
      return {};
    }
    int ms;
    SerialToHms(rounded, &p.h, &p.mi, &p.s, &ms);
    p.ns = int64_t{ms} * 1000000;
    p.serial = rounded;
  }

  // [DBNum1] / [DBNum2] / [DBNum3]: the numbers of a date in Chinese or full-width digits (Excel): a year digit by
  // digit, the other fields read with their units (\u5341\u4e00\u6708\u4e8c\u5341\u4e5d\u65e5)
  const int db = s.dbnum;
  auto num = [&](int64_t v, int width) { return db ? ZhDateNumber(v, width, db) : Pad(v, width); };
  auto digits = [&](const std::string& d) { return db ? ZhDigits(d, db) : d; };
  bool has_era = false;
  for (const FTok& t : s.t) has_era = has_era || t.k == TK::Era;
  const JaEra* ja = s.lcid == 0x411 ? JaEraOf(p.y, p.mo, p.d) : nullptr;
  std::string out;
  for (const FTok& t : s.t) {
    switch (t.k) {
      case TK::Lit: out += t.lit; break;
      case TK::Year: out += t.n <= 2 ? num(p.y % 100, 2) : digits(std::to_string(p.y)); break;
      case TK::RocYear: {
        // e: the year of the Republic (1 is \u5143 after its era name); before 1912 the Gregorian year, or with
        // ggg the years before it (\u4e2d\u83ef\u6c11\u570b\u524d12); [$-411] the year of Japan's era
        int64_t year = p.y;
        if (ja) year = p.y - ja->y + 1;
        else if (p.y >= 1912) year = p.y - 1911;
        else if (has_era) year = 1912 - p.y;
        if (year == 1 && has_era && !ja && p.y >= 1912) out += "\345\205\203";
        else out += digits(std::to_string(year));
        break;
      }
      case TK::Era:
        if (s.lcid == 0x411) {
          if (ja) out += t.n == 1 ? std::string(ja->letter) : t.n == 2 ? std::string(ja->name, 3) : std::string(ja->name);
        } else {
          out += t.n >= 3 ? "\344\270\255\350\217\257\346\260\221\345\234\213" : "\346\260\221\345\234\213";
          if (p.y < 1912) out += "\345\211\215";
        }
        break;
      case TK::Month:
        if (t.n <= 2) {
          out += num(p.mo, t.n);
        } else if (s.locale_zh) {
          // [$-404] mmm / mmmm: \u4e00\u6708 ... \u5341\u4e8c\u6708; mmmmm their first character
          const std::string name = ZhDateNumber(p.mo, 1, 1) + "\346\234\210";
          out += t.n == 5 ? name.substr(0, 3) : name;
        } else if (t.n == 3) {
          out += std::string(kMonthEn[p.mo - 1]).substr(0, 3);
        } else if (t.n == 5) {
          out += std::string(kMonthEn[p.mo - 1]).substr(0, 1);
        } else {
          out += kMonthEn[p.mo - 1];
        }
        break;
      case TK::Day: {
        const int wd = Weekday(std::floor(p.serial)) - 1;
        if (t.lit == "zh") out += AaaDay(wd, t.n, s.lcid);  // aaa / aaaa
        else if (t.n <= 2) out += num(p.d, t.n);
        else if (s.locale_zh) out += (t.n == 3 ? std::string("\351\200\261") : std::string("\346\230\237\346\234\237")) + kDayZh[wd];
        else if (t.n == 3) out += std::string(kDayEn[wd]).substr(0, 3);
        else out += kDayEn[wd];
        break;
      }
      case TK::Hour: {
        int h = p.h;
        if (s.has_ampm) {
          h %= 12;
          if (h == 0) h = 12;
        }
        out += num(h, t.n >= 2 ? 2 : 1);
        break;
      }
      case TK::Minute: out += num(p.mi, t.n >= 2 ? 2 : 1); break;
      case TK::Second: out += num(p.s, t.n >= 2 ? 2 : 1); break;
      case TK::SubSec: {
        const int n = std::min(t.n, 9);
        const std::string ns = Pad(p.ns, 9);
        out += "." + digits(ns.substr(0, static_cast<size_t>(n)));
        break;
      }
      case TK::ElapsedH: out += Pad(static_cast<int64_t>(std::floor(p.serial * 24)), t.n); break;
      case TK::ElapsedM: out += Pad(static_cast<int64_t>(std::floor(p.serial * 1440)), t.n); break;
      case TK::ElapsedS: out += Pad(static_cast<int64_t>(std::floor(p.serial * 86400)), t.n); break;
      case TK::AmPm:
        if (t.ampm == 3) out += p.h < 12 ? "\344\270\212\345\215\210" : "\344\270\213\345\215\210";
        else if (t.ampm == 2) out += p.h < 12 ? t.lit.substr(0, 1) : t.lit.substr(2, 1);  // a/p: "a" or "p"
        else out += p.h < 12 ? t.lit.substr(0, 2) : t.lit.substr(3, 2);                    // am/pm: "am" or "pm"
        break;
      case TK::General: out += FormatGeneral(p.serial); break;
      case TK::Fill: out += '\x02'; break;
      default: break;
    }
  }
  return out;
}

// the fill a section asks for (*x): its place in the text, marked \x02 while formatting, and its character
void TakeFill(FormatResult* r, const Section& s) {
  const size_t at = r->text.find('\x02');
  if (at == std::string::npos) return;
  r->text.erase(at, 1);
  for (const FTok& t : s.t) {
    if (t.k == TK::Fill) {
      r->fill_at = static_cast<int>(at);
      r->fill = t.lit;
      break;
    }
  }
  for (size_t more = r->text.find('\x02'); more != std::string::npos; more = r->text.find('\x02')) r->text.erase(more, 1);  // one fill counts
}

}  // namespace

std::string ChineseNumerals(uint64_t n, int style) { return ChineseNumber(n, style); }

class NumberFormat {
 public:
  std::vector<Section> sec;
  bool general = true;
  bool bad = false;  // a code Excel refuses (BadCode): TEXT is #VALUE!, a cell shows the value as General
};

namespace {

// a number or date part, which the text section can't hold
bool NumberToken(TK k) {
  switch (k) {
    case TK::Lit:
    case TK::Text:
    case TK::AmPm:
    case TK::General:
    case TK::Fill: return false;
    default: return true;
  }
}

// the codes Excel refuses (Format Cells' "cannot use the number format", Range.NumberFormat raises, TEXT is #VALUE!;
// measured in Excel 16): more than four sections, an @ in any section but the last ("@;0", "0;@;0", "0;0;@;0"), and
// a text section (the fourth, or the last when it has an @) with a digit, a decimal point, a percent, an exponent, a
// fraction or a date part ("0;0;0;0", "@0", "0;0;0;yy"), or with General beside its @ ("@ General"); "0;0;0;General",
// "0;0;0;\0" and "0;\n@" are fine
bool BadCode(const std::vector<Section>& sec) {
  if (sec.size() > 4) return true;
  for (size_t i = 0; i + 1 < sec.size(); ++i) {
    if (sec[i].is_text) return true;
  }
  if (sec.empty() || (sec.size() < 4 && !sec.back().is_text)) return false;
  const Section& ts = sec.back();
  bool general = false;
  for (const FTok& t : ts.t) {
    if (NumberToken(t.k)) return true;
    general = general || t.k == TK::General;
  }
  return general && ts.is_text;
}

// the code's marks read as Excel's parser does (measured in Excel 16): a quote or a bracket left open ("abc, [>0, [Blue),
// an escape, a pad or a fill with nothing after it (0_, 0*), [ColorN] past 56, and two conditions in one section
// ([>100][<200]0) are refused; [Bluex] and a ] alone are fine
bool MarksOk(std::string_view c) {
  int conds = 0;
  for (size_t i = 0; i < c.size();) {
    const char ch = c[i];
    if (ch == '\\' || ch == '_' || ch == '*') {
      if (i + 1 >= c.size()) return false;
      i += 2;
      continue;
    }
    if (ch == '"') {
      const size_t j = c.find('"', i + 1);
      if (j == std::string_view::npos) return false;
      i = j + 1;
      continue;
    }
    if (ch == '[') {
      const size_t j = c.find(']', i + 1);
      if (j == std::string_view::npos) return false;
      const std::string_view in = c.substr(i + 1, j - i - 1);
      if (!in.empty() && (in[0] == '<' || in[0] == '>' || in[0] == '=') && ++conds > 1) return false;
      if (in.size() > 5 && IEq(in.substr(0, 5), "Color") && in.find_first_not_of("0123456789", 5) == std::string_view::npos) {
        const int n = std::atoi(std::string(in.substr(5)).c_str());
        if (n < 1 || n > 56) return false;
      }
      i = j + 1;
      continue;
    }
    if (ch == ';') conds = 0;
    ++i;
  }
  return true;
}

}  // namespace

std::shared_ptr<const NumberFormat> CompileFormat(std::string_view code) {
  // the same code again on this thread (a column of TEXT(x,"yyyy"), a format a frame draws a thousand times): no lock
  // and no allocation. Every call took one mutex and made a string, and a calculation wave of 160,000 TEXT formulas
  // spent most of its time waiting there (chaos seed 5043)
  thread_local std::string last_code;
  thread_local std::shared_ptr<const NumberFormat> last;
  if (last && code == last_code) return last;
  static std::shared_mutex mu;
  static std::unordered_map<std::string, std::shared_ptr<const NumberFormat>> cache;
  std::string key(code);
  {
    std::shared_lock<std::shared_mutex> read(mu);
    auto it = cache.find(key);
    if (it != cache.end()) {
      last_code = std::move(key);
      last = it->second;
      return last;
    }
  }
  std::unique_lock<std::shared_mutex> lock(mu);
  auto it = cache.find(key);
  if (it != cache.end()) {
    last_code = std::move(key);
    last = it->second;
    return last;
  }
  auto f = std::make_shared<NumberFormat>();
  const bool gen = code.empty() || IEq(code, "General") || code == "G/\351\200\232\347\224\250\346\240\274\345\274\217";
  if (!gen) {
    for (const std::string& part : SplitSections(code)) f->sec.push_back(ParseSection(part));
    f->general = false;
    f->bad = BadCode(f->sec) || !MarksOk(code);
  }
  if (cache.size() > 4096) cache.clear();
  cache.emplace(key, f);
  last_code = std::move(key);
  last = f;
  return f;
}

bool ValidFormatCode(std::string_view code) { return !CompileFormat(code)->bad; }

std::string FormatGeneral(double v, int max_chars) {
  if (v == 0) return "0";
  const double av = std::fabs(v);
  char buf[64];
  const int budget = std::max(1, max_chars - (v < 0 ? 1 : 0));
  // a small number whose fixed form would lose significant digits that scientific notation keeps (like Excel:
  // 0.0001234567 -> 0.000123457, 0.00001234567 -> 1.23457E-05)
  bool small_sci = false;
  if (av < 1 && av >= 1e-9) {
    const int lead = static_cast<int>(-std::floor(std::log10(av))) - 1;  // zeros after the decimal point
    const int fixed_sig = budget - 2 - lead;
    const int sci_sig = std::min(std::max(0, budget - 6), 5) + 1;
    char g[32];
    std::snprintf(g, sizeof(g), "%.15e", av);
    std::string m(g, std::strchr(g, 'e') - g);
    m.erase(std::remove(m.begin(), m.end(), '.'), m.end());
    while (m.size() > 1 && m.back() == '0') m.pop_back();
    const int need = static_cast<int>(m.size());
    small_sci = need > fixed_sig && sci_sig > fixed_sig;
  }
  // scientific notation when too large or too small
  // ...and in a column too narrow for its digits, a whole number too (Excel 16, measured: 1234567890 is 1E+09 in a
  // column 5 wide and 1.23E+09 in one 8 wide, 100000 1E+05 in 5), where it showed ####
  if (small_sci || av >= 1e11 || av < 1e-9 || ((std::floor(av) != av || max_chars < 11) && av >= std::pow(10.0, budget))) {
    auto sci = [&](int dec) {
      std::snprintf(buf, sizeof(buf), "%.*E", dec, v);
      std::string s(buf);
      // drop redundant trailing zeros: 1.50000E+11 -> 1.5E+11
      const size_t e = s.find('E');
      std::string mant = s.substr(0, e), ex = s.substr(e + 1);
      if (mant.find('.') != std::string::npos) {
        while (!mant.empty() && mant.back() == '0') mant.pop_back();
        if (!mant.empty() && mant.back() == '.') mant.pop_back();
      }
      const int en = std::atoi(ex.c_str());
      std::string exs = std::to_string(std::abs(en));
      if (exs.size() < 2) exs.insert(0, "0");
      return mant + "E" + (en < 0 ? "-" : "+") + exs;
    };
    // budget - 6 decimals fit a two-digit exponent; a three-digit one (1.2E-251, a regression's p-value) takes a
    // decimal less, like Excel, instead of ##### in the column
    int dec = std::min(std::max(0, budget - 6), 5);
    std::string s = sci(dec);
    while (dec > 0 && static_cast<int>(s.size()) - (v < 0 ? 1 : 0) > budget) s = sci(--dec);
    return s;
  }
  // decimals depend on the available characters (up to 15 significant digits)
  const int int_digits = av < 1 ? 1 : static_cast<int>(std::floor(std::log10(av))) + 1;
  int dec = std::max(0, std::min(budget - int_digits - 1, 15 - int_digits));
  if (av < 1) dec = std::min(budget - 2, 15 + static_cast<int>(-std::floor(std::log10(av))) - 1);
  dec = std::max(0, std::min(dec, 30));
  std::string ip, fp;
  SplitDigits(av, dec, &ip, &fp);  // Excel's rounding (half away from zero on 15 digits)
  std::string s = std::string(v < 0 ? "-" : "") + (ip.empty() ? "0" : ip) + (fp.empty() ? "" : "." + fp);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

bool RoundAsShown(double v, std::string_view code, double* out) {
  if (!std::isfinite(v)) return false;
  const auto f = CompileFormat(code);
  if (!f || f->general || f->bad || f->sec.empty()) return false;
  const size_t nsec = f->sec.size() >= 4 || (f->sec.size() > 1 && f->sec.back().is_text) ? f->sec.size() - 1 : f->sec.size();
  if (f->sec[0].has_cond || (nsec > 1 && f->sec[1].has_cond)) return false;
  const Section& s = v < 0 && nsec > 1 ? f->sec[1] : v == 0 && nsec > 2 ? f->sec[2] : f->sec[0];
  if (s.is_date || s.is_general || s.is_text || s.has_fraction || s.epoch) return false;
  // the digits the cell shows, made the way FormatValue makes them (SplitDigits: 15 significant digits, half away
  // from zero), and read back: -75.195 in 0.00 shows -75.20, while rounding -75.195 * 100 in binary gave -7519 and
  // stored -75.19 (gaps_fuzz seed 61311)
  if (s.mul == 0) return false;
  double av = std::fabs(v);
  for (int i = 0; i < s.percent; ++i) av *= 100;
  for (int i = 0; i < s.scale; ++i) av /= 1000;
  av *= std::fabs(s.mul);
  int exponent = 0;
  if (s.has_exp && av != 0) {
    exponent = static_cast<int>(std::floor(std::log10(av)));
    const int k = std::max(1, s.int_slots);
    bool eng = false;
    for (const FTok& t : s.t) {
      if (t.k == TK::DHash) eng = true;
      if (t.k == TK::Point || t.k == TK::Exp) break;
    }
    if (eng && k > 1) exponent = static_cast<int>(std::floor(static_cast<double>(exponent) / k)) * k;
    else exponent -= (k - 1);
    av /= std::pow(10.0, exponent);
  }
  std::string ip, fp;
  SplitDigits(av, s.frac_slots, &ip, &fp);
  // the shown digits as a decimal number back at the cell's scale, read once (12.3% is 0.123, not 12.3 / 100)
  const int shift = (s.has_exp ? exponent : 0) + 3 * s.scale - 2 * s.percent;
  const std::string text = (ip.empty() ? std::string("0") : ip) + (fp.empty() ? std::string() : "." + fp) + "e" + std::to_string(shift);
  double r = std::strtod(text.c_str(), nullptr);
  if (std::fabs(s.mul) != 1) r /= std::fabs(s.mul);
  if (v < 0) r = -r;
  if (!std::isfinite(r)) return false;
  *out = r;
  return true;
}

FormatResult FormatValue(const Value& v, std::string_view code, double max_chars) {
  return FormatValue(v, *CompileFormat(code), max_chars);
}

FormatResult FormatValue(const Value& v, const NumberFormat& f, double max_chars) {
  if (f.bad) {
    static const NumberFormat kGeneral;
    FormatResult g = FormatValue(v, kGeneral, max_chars);
    g.bad_code = true;
    return g;
  }
  FormatResult r;
  // an IMAGE() value shows its alt text wherever text is needed (the grid draws the picture itself)
  if (ImageSpec img; v.IsString() && ParseImageValue(v.str(), &img)) {
    r.text = img.alt;
    return r;
  }
  if (std::string cap; v.IsString() && ParseCubeValue(v.str(), &cap)) {  // a CUBE member or set: its caption
    r.text = cap;
    return r;
  }
  switch (v.type()) {
    case core::VType::Empty: return r;
    case core::VType::Error:
      r.text = core::ErrorText(v.error());
      return r;
    case core::VType::Pending:
      r.text = "#GETTING_DATA";
      return r;
    case core::VType::Bool:  // TRUE / FALSE go through the text section, as Excel ("Text: "@ shows Text: TRUE)
    case core::VType::StrInline:
    case core::VType::StrHeap: {
      const std::string str = v.IsBool() ? std::string(v.boolean() ? "TRUE" : "FALSE") : std::string(v.str());
      // the text section: the fourth, or the last when it has an @ ("0;\n@" shows a as na, measured)
      const Section* ts = nullptr;
      if (f.sec.size() >= 4) ts = &f.sec[3];
      else if (!f.sec.empty() && f.sec.back().is_text) ts = &f.sec.back();
      if (!ts) {
        r.text = str;
        return r;
      }
      for (const FTok& t : ts->t) {
        if (t.k == TK::Text || t.k == TK::General) r.text += str;  // "0;0;0;General" shows the text (measured)
        else if (t.k == TK::Fill) r.text += '\x02';
        else if (t.k == TK::Lit) r.text += t.lit;
      }
      r.color = ts->color;
      TakeFill(&r, *ts);
      return r;
    }
    default: break;
  }
  // numbers
  r.numeric = true;
  const double d = v.AsDouble();
  core::Dec dec;
  const core::Dec* pdec = nullptr;
  if (v.IsDecimal()) {
    dec = core::DecFromValue(v);
    pdec = &dec;
  }
  if (f.general || f.sec.empty()) {
    if (pdec) {
      core::Dec t = dec;
      std::string s = core::DecToString(t);
      if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
      }
      r.text = s;
      // General depends on the column width: fewer decimals (rounded) when it doesn't fit; double's General format only when the integer part doesn't fit
      if (max_chars > 0 && static_cast<double>(r.text.size()) > max_chars) {
        const size_t dot = r.text.find('.');
        const int int_len = static_cast<int>(dot == std::string::npos ? r.text.size() : dot);
        const int keep = static_cast<int>(max_chars) - int_len - 1;
        core::Dec rounded;
        if (dot != std::string::npos && keep >= 0 && core::DecRescale(dec, keep, &rounded)) {
          s = core::DecToString(rounded);
          if (s.find('.') != std::string::npos) {
            while (s.back() == '0') s.pop_back();
            if (s.back() == '.') s.pop_back();
          }
          r.text = s;
        } else {
          r.text = FormatGeneral(d, static_cast<int>(max_chars));
        }
      }
    } else {
      // as in Excel, General shows at most 11 characters however wide the column (0.333333333, not 15 digits);
      // narrower columns get fewer
      // (the minus doesn't count: -833.3333333, -1.42109E-14)
      const int limit = d < 0 ? 12 : 11;
      r.text = FormatGeneral(d, max_chars > 0 ? std::min(limit, static_cast<int>(max_chars)) : limit);
    }
    if (max_chars > 0 && static_cast<double>(r.text.size()) > max_chars) {
      r.hashes = true;
      r.text = std::string(static_cast<size_t>(std::max(1.0, max_chars)), '#');
    }
    return r;
  }
  // pick the section among the number sections: a text section after them (the fourth, or the last when it has an
  // @: "0;@", "0;0;@") takes no numbers, so "0;@" shows -1 by the first section with its minus and "0;0;@" shows 0
  // by the first (measured); a number that only the text section would take is #VALUE! in TEXT and ##### in a cell
  // ("[>0]0;@" of -1), as is one that matches neither of two conditions ("[>0]0;[<0]0" of 0)
  const size_t nsec = f.sec.size() >= 4 || (f.sec.size() > 1 && f.sec.back().is_text) ? f.sec.size() - 1 : f.sec.size();
  auto no_section = [&]() {
    r.bad_date = true;
    r.hashes = true;
    r.text = std::string(max_chars > 0 ? static_cast<size_t>(std::max(1.0, max_chars)) : 7, '#');
    return r;
  };
  const Section* s = &f.sec[0];
  bool use_sign = true;
  const bool has_cond = f.sec[0].has_cond || (nsec > 1 && f.sec[1].has_cond);
  if (has_cond) {
    if (f.sec[0].has_cond && CondMatch(f.sec[0], d)) {
      s = &f.sec[0];
    } else if (nsec > 1 && f.sec[1].has_cond && CondMatch(f.sec[1], d)) {
      s = &f.sec[1];
    } else if (nsec > 2) {
      s = &f.sec[2];
    } else if (nsec > 1 && !f.sec[1].has_cond) {
      s = &f.sec[1];
    } else if (nsec < f.sec.size() || (nsec == 2 && f.sec[0].has_cond)) {
      return no_section();
    }
    // the minus goes when the section only ever shows negative numbers, as Excel (measured): its own condition
    // takes none above or at zero ([<0], [<-1], [=-5]; not [<=0], [<100], [<>0]), or it is the second section after
    // a first one that takes [<x] or [>x] with x at most 0 ([>0]0;0 shows -5 as 5, [>=1000]#,##0;0 as -5)
    use_sign = true;
    if (d < 0) {
      auto only_negative = [](const Section& c) {
        return (c.cond_op == "<" && c.cond_val <= 0) || (c.cond_op == "<=" && c.cond_val < 0) ||
               (c.cond_op == "=" && c.cond_val < 0);
      };
      // one number section before the text section keeps the minus ("[<0]0;@" of -1 is -1); a lone section whose
      // condition fails still shows the number, without it ("[>0]0" of -1 is 1, measured)
      if (nsec == 1 && nsec < f.sec.size()) {
        use_sign = true;
      } else if (s->has_cond) {
        use_sign = CondMatch(*s, d) && !only_negative(*s);
      } else if (s == &f.sec[1] && f.sec[0].has_cond) {
        const std::string& o = f.sec[0].cond_op;
        use_sign = !(o == "<" || o == "<=" || ((o == ">" || o == ">=") && f.sec[0].cond_val <= 0));
      }
    }
  } else if (d < 0 && nsec >= 2) {
    s = &f.sec[1];
    use_sign = false;
  } else if (d == 0 && nsec >= 3) {
    s = &f.sec[2];
  }
  // number;@ (one number section): the first shows every number, negative ones with their minus (measured: -1 in
  // "0.0;@" is -1.0, in "m/d/yy h:mm;@" a negative date, #VALUE! in TEXT)
  r.color = s->color;
  if (s->is_text && !s->is_date && s->int_slots == 0) {
    // a number in the text section: show the number as is
    r.text = FormatGeneral(d);
  } else if (s->is_date) {
    r.text = FormatDate(*s, v, &r.bad_date);
    if (r.bad_date) {
      r.hashes = true;
      r.text = std::string(max_chars > 0 ? static_cast<size_t>(std::max(1.0, max_chars)) : 7, '#');
      return r;
    }
  } else if (s->is_general && s->int_slots == 0) {
    std::string pre, post;
    bool seen = false;
    for (const FTok& t : s->t) {
      if (t.k == TK::General) seen = true;
      else if (t.k == TK::Lit) (seen ? post : pre) += t.lit;
      else if (t.k == TK::Fill) (seen ? post : pre) += '\x02';
    }
    std::string g;
    if (s->dbnum && std::fabs(d) >= 1e11 && std::isfinite(d)) {
      // in Chinese the whole number is read, not written in scientific notation (1E+15: \u4e00\u5343\u5146)
      std::string frac;
      SplitDigits(std::fabs(d), 0, &g, &frac);
      if (d < 0 && use_sign) g.insert(0, "-");
    } else {
      g = FormatGeneral(use_sign ? d : std::fabs(d));
    }
    r.text = pre + g + post;
  } else if (s->has_fraction && s->int_slots == 0 && s->den_fixed == 0 && !(std::fabs(d) < 2147483647.0)) {
    // a fraction without a whole part takes values below 2^31 - 1 only (measured: 2147483646.5 in "?/?" is
    // 2147483647/1, 2147483647 and 12345678901 are ##### in a cell and #VALUE! in TEXT; "# ?/?" takes any)
    r.bad_date = true;
    r.hashes = true;
    r.text = std::string(max_chars > 0 ? static_cast<size_t>(std::max(1.0, max_chars)) : 7, '#');
    return r;
  } else {
    r.text = FormatNumber(*s, d, pdec, use_sign);
  }
  if (s->dbnum && !s->is_date) r.text = DbNumText(r.text, s->dbnum, s->is_general && s->int_slots == 0);
  TakeFill(&r, *s);
  if (max_chars > 0 && static_cast<double>(r.text.size()) > max_chars + 0.5) {
    r.hashes = true;
    r.fill_at = -1;
    r.text = std::string(static_cast<size_t>(std::max(1.0, max_chars)), '#');
  }
  return r;
}

bool IsDateFormat(std::string_view code) {
  auto f = CompileFormat(code);
  return !f->sec.empty() && f->sec[0].is_date;
}

bool IsPercentFormat(std::string_view code) {
  auto f = CompileFormat(code);
  return !f->general && !f->sec.empty() && f->sec[0].percent > 0 && !f->sec[0].is_date && !f->sec[0].is_text;
}

std::string PercentEntry(std::string_view text, std::string_view cell_code) {
  if (text.empty() || text.find('%') != std::string_view::npos || !IsPercentFormat(cell_code)) return std::string(text);
  InputParse p;
  const std::string with = std::string(text) + "%";
  if (!ParseNumberInput(text, &p) || !ParseNumberInput(with, &p)) return std::string(text);
  return with;
}

bool FormatShowsDate(std::string_view code) {
  auto f = CompileFormat(code);
  if (f->sec.empty() || !f->sec[0].is_date) return false;
  for (const FTok& t : f->sec[0].t) {
    if (t.k == TK::Year || t.k == TK::Month || t.k == TK::Day || t.k == TK::RocYear || t.k == TK::Era) return true;
  }
  return false;
}

std::string CellFormatCode(std::string_view code, bool* color, bool* parens) {
  *color = *parens = false;
  auto f = CompileFormat(code);
  if (f->general || f->sec.empty()) return "G";
  const Section& s = f->sec[0];
  bool open_paren = false, dollar = false;
  for (const FTok& t : s.t) {
    if (t.k != TK::Lit) continue;
    if (t.lit.find('(') != std::string::npos) open_paren = true;
    for (size_t i = t.lit.find('$'); i != std::string::npos; i = t.lit.find('$', i + 1)) {
      if (i == 0 || !std::isalpha(static_cast<unsigned char>(t.lit[i - 1]))) dollar = true;  // $, not NT$
    }
  }
  *color = f->sec.size() > 1 && f->sec[1].color != 0;
  *parens = open_paren;
  const std::string tail = std::string(*color ? "-" : "") + (*parens ? "()" : "");
  if (s.is_text || s.has_fraction || s.is_general) return "G";
  if (s.is_date) {
    bool y = false, mo = false, d = false, h = false, sec = false, ampm = false, elapsed = false;
    for (const FTok& t : s.t) {
      y = y || t.k == TK::Year || t.k == TK::RocYear;
      mo = mo || t.k == TK::Month;
      d = d || (t.k == TK::Day && t.n <= 2);
      h = h || t.k == TK::Hour;
      sec = sec || t.k == TK::Second;
      ampm = ampm || t.k == TK::AmPm;
      elapsed = elapsed || t.k == TK::ElapsedH || t.k == TK::ElapsedM || t.k == TK::ElapsedS;
    }
    if (y || mo || d) return (y && mo && d) || (!mo) ? "D1" : (mo && d) ? "D3" : "D2";
    if (elapsed || !h) return "G";  // [h]:mm:ss, mm:ss
    return std::string(ampm ? (sec ? "D6" : "D7") : (sec ? "D8" : "D9"));
  }
  const std::string dec = std::to_string(s.frac_slots);
  if (s.percent) return "P" + dec + tail;
  if (s.has_exp) return "S" + dec + tail;
  if (dollar) return "C" + dec + tail;
  if (s.thousands) return "," + dec + tail;
  return "F" + dec + tail;
}

std::string EditNumberText(double x) {
  if (!std::isfinite(x)) return NumberToText(x);
  if (std::fabs(x) < 2.2250738585072014e-308) return "0";  // zero and the subnormal numbers Excel keeps as 0
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.14e", x);
  std::string s = buf;
  const size_t e = s.find('e');
  const int exp = std::atoi(s.c_str() + e + 1);
  std::string mant = s.substr(0, e);
  const bool neg = !mant.empty() && mant[0] == '-';
  if (neg) mant.erase(0, 1);
  std::string digits;
  for (char ch : mant) {
    if (ch != '.') digits += ch;
  }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  std::string dec;
  if (exp >= 0) {
    const size_t whole = static_cast<size_t>(exp) + 1;
    dec = digits.size() <= whole ? digits + std::string(whole - digits.size(), '0') : digits.substr(0, whole) + "." + digits.substr(whole);
  } else {
    dec = "0." + std::string(static_cast<size_t>(-exp - 1), '0') + digits;
  }
  if (dec.size() > 21) {
    char ex[16];
    std::snprintf(ex, sizeof ex, "%02d", exp < 0 ? -exp : exp);
    dec = digits.substr(0, 1) + (digits.size() > 1 ? "." + digits.substr(1) : std::string()) + (exp >= 0 ? "E+" : "E-") + ex;
  }
  return (neg ? "-" : "") + dec;
}

namespace {
// the kinds of format a typed entry and a cell's format are compared by (0: none of them)
enum FormatKind { kNoKind, kPercent, kScientific, kFraction, kCurrency, kAccounting, kDate, kTime };

int KindOfCode(std::string_view code) {
  bool in_q = false;
  bool pct = false, sci = false, frac = false, dollar = false;
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = code[i];
    if (ch == '"') in_q = !in_q;
    if (in_q) continue;
    if (ch == ';') break;
    if (ch == '%') pct = true;
    if ((ch == 'E' || ch == 'e') && i + 1 < code.size() && (code[i + 1] == '+' || code[i + 1] == '-')) sci = true;
    if (ch == '/' && i > 0 && (code[i - 1] == '?' || code[i - 1] == '#' || std::isdigit(static_cast<unsigned char>(code[i - 1])))) frac = true;
    if (ch == '$') dollar = true;
  }
  if (IsDateFormat(code)) return FormatShowsDate(code) ? kDate : kTime;
  if (pct) return kPercent;
  if (sci) return kScientific;
  if (frac) return kFraction;
  if (code.rfind("_(", 0) == 0 || code.rfind("_-", 0) == 0) return kAccounting;
  if (dollar) return kCurrency;
  return kNoKind;
}

// the built-in formats (and the ones YCGrid's typed input gives) that a typed entry of another kind replaces
bool BuiltinReplaceable(std::string_view code) {
  static const char* const kCodes[] = {
      "0%", "0.00%", "0.00E+00", "##0.0E+0", "# ?/?", "# ??/??", "$#,##0_);($#,##0)", "$#,##0_);[Red]($#,##0)", "$#,##0.00_);($#,##0.00)",
      "$#,##0.00_);[Red]($#,##0.00)", "_(* #,##0_);_(* (#,##0);_(* \"-\"_);_(@_)", "_($* #,##0_);_($* (#,##0);_($* \"-\"_);_(@_)",
      "_(* #,##0.00_);_(* (#,##0.00);_(* \"-\"??_);_(@_)", "_($* #,##0.00_);_($* (#,##0.00);_($* \"-\"??_);_(@_)", "m/d/yyyy", "d-mmm-yy",
      "d-mmm", "mmm-yy", "m/d/yyyy h:mm", "yyyy/m/d", "yyyy/m/d h:mm", "h:mm AM/PM", "h:mm:ss AM/PM", "h:mm", "h:mm:ss", "mm:ss", "[h]:mm:ss",
      "mm:ss.0"};
  for (const char* c : kCodes) {
    if (code == c) return true;
  }
  return false;
}
}  // namespace

bool TypedFormatReplaces(std::string_view cell_code, std::string_view typed_code) {
  if (cell_code.empty() || cell_code == "General") return true;
  const int typed = KindOfCode(typed_code);
  if (typed == kNoKind || !BuiltinReplaceable(cell_code)) return false;  // 1,234 keeps any format; custom ones stay
  return KindOfCode(cell_code) != typed;
}

bool FractionForNumberFormat(std::string_view text, std::string_view cell_code, double* out) {
  if (cell_code.empty() || cell_code == "General" || cell_code == "@" || IsDateFormat(cell_code)) return false;
  size_t i = 0;
  while (i < text.size() && text[i] == ' ') ++i;
  const bool neg = i < text.size() && text[i] == '-';
  if (neg) ++i;
  auto digits = [&](uint64_t* v) {
    const size_t start = i;
    uint64_t n = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) && i - start < 15) n = n * 10 + static_cast<uint64_t>(text[i++] - '0');
    *v = n;
    return i > start;
  };
  uint64_t a = 0, b = 0;
  if (!digits(&a) || i >= text.size() || text[i] != '/') return false;
  ++i;
  if (!digits(&b) || b == 0) return false;
  while (i < text.size() && text[i] == ' ') ++i;
  if (i != text.size()) return false;
  *out = (neg ? -1.0 : 1.0) * static_cast<double>(a) / static_cast<double>(b);
  return true;
}

std::string NumberToText(double v) {
  if (v == 0) return "0";
  if (!std::isfinite(v)) return FormatGeneral(v);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.14e", std::fabs(v));  // d.dddddddddddddde+XX: 15 significant digits
  std::string digits;
  const char* p = buf;
  for (; *p && *p != 'e'; ++p) {
    if (*p >= '0' && *p <= '9') digits += *p;
  }
  const int exp10 = *p == 'e' ? std::atoi(p + 1) : 0;
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  std::string fixed;
  if (exp10 >= 0) {
    const size_t whole = static_cast<size_t>(exp10) + 1;
    fixed = digits.size() <= whole ? digits + std::string(whole - digits.size(), '0') : digits.substr(0, whole) + "." + digits.substr(whole);
  } else {
    fixed = "0." + std::string(static_cast<size_t>(-exp10 - 1), '0') + digits;
  }
  const std::string sign = v < 0 ? "-" : "";
  if (fixed.size() <= 20) return sign + fixed;
  std::string e = std::to_string(std::abs(exp10));
  if (e.size() < 2) e.insert(0, "0");
  return sign + digits.substr(0, 1) + (digits.size() > 1 ? "." + digits.substr(1) : "") + "E" + (exp10 < 0 ? "-" : "+") + e;
}

// A typed number as Excel reads it (measured against Excel 16): the number itself (digits, thousands separators
// between groups of three, a decimal point, an exponent) with at most one of each mark before it, in any order and
// with spaces between them: a sign or parentheses (one negative mark: (-5) and -$(5) are text), a currency symbol
// ($ or NT$ in any case: $-5, -$5, $(5), NT$ 5) and a % (also after the number: %12.5, 12.5 %, (12.5)%). A currency
// amount is never a percentage ($5% is text). The format follows the marks: an exponent gives 0.00E+00, then 0% /
// 0.00%, the currency's, #,##0 / #,##0.00; a decimal point typed gives two decimals (12.5%, 1,000., $5.).
bool ParseNumberInput(std::string_view t, InputParse* out, bool excel_15_digit_input) {
  enum class Tok { Open, Close, Sign, Currency, Percent, Number };
  std::vector<Tok> toks;
  std::string_view num, frac;  // frac: the n/d of a mixed fraction (1 1/2), one space after the whole number
  bool minus = false, nt = false;
  for (size_t i = 0; i < t.size();) {
    const char c = t[i];
    if (c == ' ') {
      ++i;
    } else if (c == '(' || c == ')') {
      toks.push_back(c == '(' ? Tok::Open : Tok::Close);
      ++i;
    } else if (c == '+' || c == '-') {
      toks.push_back(Tok::Sign);
      minus = c == '-';
      ++i;
    } else if (c == '%') {
      toks.push_back(Tok::Percent);
      ++i;
    } else if (c == '$') {
      toks.push_back(Tok::Currency);
      ++i;
    } else if ((c == 'N' || c == 'n') && i + 2 < t.size() && (t[i + 1] == 'T' || t[i + 1] == 't') && t[i + 2] == '$') {
      toks.push_back(Tok::Currency);
      nt = true;
      i += 3;
    } else if ((c >= '0' && c <= '9') || c == '.') {
      size_t j = i;
      while (j < t.size() && ((t[j] >= '0' && t[j] <= '9') || t[j] == ',' || t[j] == '.')) ++j;
      if (j < t.size() && (t[j] == 'e' || t[j] == 'E')) {
        ++j;
        if (j < t.size() && (t[j] == '+' || t[j] == '-')) ++j;
        while (j < t.size() && t[j] >= '0' && t[j] <= '9') ++j;
      }
      toks.push_back(Tok::Number);
      num = t.substr(i, j - i);
      if (j + 1 < t.size() && t[j] == ' ' && t[j + 1] >= '0' && t[j + 1] <= '9') {  // 1 1/2: its n/d
        size_t k = j + 1;
        while (k < t.size() && t[k] >= '0' && t[k] <= '9') ++k;
        if (k < t.size() && t[k] == '/') {
          ++k;
          while (k < t.size() && t[k] >= '0' && t[k] <= '9') ++k;
          frac = t.substr(j + 1, k - j - 1);
          j = k;
        }
      }
      i = j;
    } else {
      return false;
    }
  }
  auto count = [&toks](Tok k) { return std::count(toks.begin(), toks.end(), k); };
  if (count(Tok::Number) != 1 || count(Tok::Sign) > 1 || count(Tok::Currency) > 1 || count(Tok::Percent) > 1 || count(Tok::Open) > 1 ||
      count(Tok::Open) != count(Tok::Close))
    return false;
  const bool paren = count(Tok::Open) == 1, currency = count(Tok::Currency) == 1, pct = count(Tok::Percent) == 1;
  if ((paren && count(Tok::Sign) > 0) || (currency && pct)) return false;
  const size_t at = static_cast<size_t>(std::find(toks.begin(), toks.end(), Tok::Number) - toks.begin());
  for (size_t k = 0; k < toks.size(); ++k) {
    if (k < at && toks[k] == Tok::Close) return false;
    if (k > at && toks[k] != Tok::Close && toks[k] != Tok::Percent) return false;  // 5$, 5-
  }
  std::string clean;
  bool ok = true, thousands = false, seen_digit = false, seen_dot = false, seen_e = false, lead_nonzero = false;
  // like Excel, a thousands separator comes after a number that isn't 0 and before three digits or more: 1,234,567,
  // 01,234 and 1,2345 are numbers, 0,123, 1,23 and 2024,05,06 text (Excel 16, measured)
  int group = 0;
  auto groups_ok = [&] { return !thousands || group >= 3; };
  for (size_t i = 0; i < num.size() && ok; ++i) {
    const char c = num[i];
    if (c >= '0' && c <= '9') {
      seen_digit = true;
      clean += c;
      if (!seen_dot && !seen_e) ++group;
      if (!thousands && c != '0') lead_nonzero = true;
    } else if (c == ',' && !seen_dot && !seen_e && seen_digit) {
      if (thousands ? group < 3 : !lead_nonzero) ok = false;
      thousands = true;
      group = 0;
    } else if (c == '.' && !seen_dot && !seen_e) {
      if (!groups_ok()) ok = false;
      seen_dot = true;
      clean += c;
    } else if ((c == 'e' || c == 'E') && seen_digit && !seen_e) {
      if (!seen_dot && !groups_ok()) ok = false;
      seen_e = true;
      clean += 'E';
      if (i + 1 < num.size() && (num[i + 1] == '+' || num[i + 1] == '-')) clean += num[++i];
    } else {
      ok = false;
    }
  }
  if (ok && !seen_dot && !seen_e && !groups_ok()) ok = false;
  if (ok && seen_e && !(clean.back() >= '0' && clean.back() <= '9')) ok = false;  // 1e, 1e+: text, as in Excel
  if (!ok || !seen_digit) return false;
  // a mixed fraction takes the marks a number takes (-$1 1/2, (1 1/2), 1 1/2%, 1,234 1/2) after a whole number,
  // with a numerator and a denominator of up to five digits (Excel 16, measured: 1 1/123456 is text)
  double frac_value = 0, denominator = 0;
  if (!frac.empty()) {
    const size_t slash = frac.find('/');
    const size_t nd = slash, dd = frac.size() - slash - 1;
    if (seen_dot || seen_e || nd < 1 || nd > 5 || dd < 1 || dd > 5) return false;
    denominator = std::strtod(std::string(frac.substr(slash + 1)).c_str(), nullptr);
    if (denominator == 0) return false;
    frac_value = std::strtod(std::string(frac.substr(0, slash)).c_str(), nullptr) / denominator;
  }
  const bool neg = minus || paren;
  const int sig = core::SignificantDigits(clean);
  if (!excel_15_digit_input && sig > 15 && !seen_e && !pct && frac.empty()) {
    core::Dec d;
    if (core::DecParse(clean, &d)) {
      if (neg) d.m = -d.m;
      out->value = core::DecToValue(d);
      out->format.clear();
      return true;
    }
  }
  if (sig > 15 && !seen_e) {
    // Excel: digits beyond 15 become 0, cut off rather than rounded (1234567890123456789 -> 1234567890123450000)
    int kept = 0;
    for (char& ch : clean) {
      if (ch < '0' || ch > '9') continue;
      if (kept >= 15) ch = '0';
      else if (kept > 0 || ch != '0') ++kept;
    }
  }
  double v = std::strtod(clean.c_str(), nullptr) + frac_value;
  if (!std::isfinite(v)) return false;  // 1e309: more than a double holds; Excel keeps the typed text
  if (neg) v = -v;
  if (pct) v /= 100;
  // Excel's numbers end at 9.99999999999999E+307 (1e308 is text) and at about 2.2251E-308: a smaller one down to
  // 1e-309 is 0, below that (or past what a double holds) text (Excel 16, measured)
  if (std::fabs(v) >= 1e308) return false;
  const size_t e_at = clean.find('E');
  if (v == 0 && clean.find_first_of("123456789") < std::min(e_at, clean.size())) return false;
  if (v != 0 && std::fabs(v) < 2.2251e-308) {
    if (std::fabs(v) < 1e-309) return false;
    v = 0;
  }
  out->value = Value::Number(v);
  if (!frac.empty()) out->format = pct ? "0.00%" : denominator >= 10 ? "# ??/??" : "# ?/?";
  else if (seen_e) out->format = "0.00E+00";
  else if (pct) out->format = seen_dot ? "0.00%" : "0%";
  else if (currency && nt) out->format = seen_dot ? "\"NT$\"#,##0.00_);(\"NT$\"#,##0.00)" : "\"NT$\"#,##0_);(\"NT$\"#,##0)";
  else if (currency) out->format = seen_dot ? "$#,##0.00_);[Red]($#,##0.00)" : "$#,##0_);[Red]($#,##0)";
  else if (thousands) out->format = seen_dot ? "#,##0.00" : "#,##0";
  else out->format.clear();
  return true;
}

InputParse ParseInput(std::string_view text, bool excel_15_digit_input) {
  InputParse r;
  std::string_view s = text;
  if (s.empty()) return r;
  if (s[0] == '\'') {
    r.value = Value::String(s.substr(1));
    return r;
  }
  std::string_view t = s;
  while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
  while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
  if (t.empty()) {
    r.value = Value::String(s);
    return r;
  }
  if (IEq(s, "TRUE") || IEq(s, "FALSE")) {  // without spaces: " TRUE" and "TRUE " stay text (Excel 16, measured)
    r.value = Value::Bool(IEq(s, "TRUE"));
    return r;
  }
  // full-width digits and marks (\uff11\uff12\uff13, \uff11\uff12.\uff15\uff05, \uff04\uff11\uff10\uff10) are read as
  // their half-width forms, as Excel does; text that isn't a number keeps what was typed
  if (t.find('\xEF') != std::string_view::npos || t.find("\343\200\200") != std::string_view::npos) {
    std::string half;
    for (size_t i = 0; i < t.size(); ++i) {
      const unsigned char b0 = static_cast<unsigned char>(t[i]);
      if (b0 == 0xEF && i + 2 < t.size()) {
        const uint32_t cp = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(t[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(t[i + 2]) & 0x3Fu);
        if (cp >= 0xFF01 && cp <= 0xFF5E) {
          half += static_cast<char>(cp - 0xFEE0);
          i += 2;
          continue;
        }
      } else if (b0 == 0xE3 && i + 2 < t.size() && t[i + 1] == '\x80' && t[i + 2] == '\x80') {  // ideographic space
        half += ' ';
        i += 2;
        continue;
      }
      half += t[i];
    }
    if (half != t) {
      InputParse h = ParseInput(half, excel_15_digit_input);
      if (!h.value.IsString()) return h;
    }
  }
  // the error values Excel takes typed, without spaces: #CALC!, #SPILL!, #BLOCKED! and #CONNECT! stay text, as
  // " #N/A" and "#N/A " do (Excel 16, measured)
  core::Err e;
  if (s[0] == '#' && core::ParseErrorText(s, &e) && e <= core::Err::GettingData) {
    r.value = Value::Error(e);
    return r;
  }
  // numbers, currency amounts, percentages and fractions ("0 1/2" is 0.5, "-1 3/16" -1.1875 in # ?/? or # ??/??;
  // "3/4" alone stays a date)
  if (ParseNumberInput(t, &r, excel_15_digit_input)) return r;
  // dates and times, never after a space (" 9:30" is text; DATEVALUE, TIMEVALUE and VALUE trim it first); the
  // Chinese AM / PM markers stay text when typed (Excel: \u4e0b\u5348 9:00 is text)
  double serial;
  DateForm form = DateForm::Numbers;
  bool overflow = false;
  const bool zh_ampm = t.find("\344\270\212\345\215\210") != std::string_view::npos || t.find("\344\270\213\345\215\210") != std::string_view::npos;
  if (s[0] != ' ' && !zh_ampm && ParseDateTime(t, &serial, &form, &overflow)) {
    r.value = Value::Number(serial);
    r.typed_only = form == DateForm::NumberMonthYear;
    const bool zh_date = t.find("\345\271\264") != std::string_view::npos;  // 2024\u5e741\u67087\u65e5
    const bool zh_time = t.find("\346\231\202") != std::string_view::npos;  // 9\u664230\u5206
    const bool has_time = t.find(':') != std::string_view::npos;
    const bool has_date = form != DateForm::Numbers || t.find('/') != std::string_view::npos || t.find('-') != std::string_view::npos;
    std::string dt;  // spaces beside a date's separators don't count (1 / 2 is 1/2)
    for (size_t k = 0; k < t.size(); ++k) {
      const bool sep_near = (k + 1 < t.size() && (t[k + 1] == '/' || t[k + 1] == '-')) || (!dt.empty() && (dt.back() == '/' || dt.back() == '-'));
      if (t[k] != ' ' || !sep_near) dt += t[k];
    }
    const size_t space = dt.find(' ');
    const std::string_view date_part = space == std::string::npos ? std::string_view(dt) : std::string_view(dt).substr(0, space);
    const bool month_day = form == DateForm::Numbers && std::count_if(date_part.begin(), date_part.end(), [](char ch) { return ch == '/' || ch == '-'; }) == 1;
    double tf = 0;
    const bool long_time = has_date && has_time && space != std::string_view::npos && ParseTimeOfDay(std::string_view(dt).substr(space + 1), &tf) && tf >= 1;
    // a decimal point before a digit (9:30. is 9:30), and the colons before a digit (9:30: has no seconds)
    bool subsec = false;
    int fields = 1;
    for (size_t k = 0; k < t.size(); ++k) {
      size_t n = k + 1;
      while (n < t.size() && t[n] == ' ') ++n;
      const bool digit_next = n < t.size() && t[n] >= '0' && t[n] <= '9';
      if (t[k] == '.' && k + 1 < t.size() && t[k + 1] >= '0' && t[k + 1] <= '9') subsec = true;
      if (t[k] == ':' && digit_next) ++fields;
    }
    subsec = subsec && has_time;
    // a month's name keeps its look, as in Excel: 15-Jan-26, 15-Jan, Jan-26
    if (zh_date) r.format = "yyyy\"\345\271\264\"m\"\346\234\210\"d\"\346\227\245\"";
    else if (zh_time) r.format = t.find("\347\247\222") != std::string_view::npos ? "h\"\346\231\202\"mm\"\345\210\206\"ss\"\347\247\222\"" : "h\"\346\231\202\"mm\"\345\210\206\"";
    else if (overflow && !subsec) r.format.clear();  // 9:60, 23:120, 2024/1/5 9:60: the number, in General (Excel)
    else if (form == DateForm::NameDayYear && !has_time) r.format = "d-mmm-yy";
    else if (form == DateForm::NameDay && !has_time) r.format = "d-mmm";
    else if ((form == DateForm::NameYear || form == DateForm::NumberMonthYear) && !has_time) r.format = "mmm-yy";
    else if (long_time) r.format.clear();  // 2024/1/7 25:00: the number, in General (Excel)
    else if (has_date && has_time) r.format = "yyyy/m/d h:mm";
    else if (month_day) r.format = "m\"\346\234\210\"d\"\346\227\245\"";  // 1/7: 1\u67087\u65e5, as Excel's Taiwan edition
    else if (has_date) r.format = "yyyy/m/d";
    else {
      const bool secs = fields >= 3;
      const bool ampm = t.find_first_of("aApP") != std::string_view::npos;
      if (subsec) r.format = "mm:ss.0";  // 9:00:00.5 shows 00:00.5, as Excel
      else if (serial >= 1) r.format = "[h]:mm:ss";
      else if (ampm) r.format = secs ? "h:mm:ss AM/PM" : "h:mm AM/PM";
      else r.format = secs ? "h:mm:ss" : "h:mm";
    }
    return r;
  }
  r.value = Value::String(s);
  return r;
}

}  // namespace yc::formula
