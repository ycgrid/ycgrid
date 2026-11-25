// SPDX-License-Identifier: Apache-2.0
// fn_text2.cpp - more text and lookup functions: VALUETOTEXT ARRAYTOTEXT BAHTTEXT PHONETIC REGEXTEST REGEXEXTRACT
// REGEXREPLACE ENCODEURL FILTERXML TRIMRANGE AREAS
//
// The REGEX functions use the ECMAScript grammar of std::regex on UTF-16 text; that covers what people normally write
// (classes, groups, quantifiers, alternation, anchors, $1 in replacements). PCRE-only syntax such as look-behind or
// named groups is rejected with #VALUE!.
#include "formula/fn_util.h"
#include "formula/datetime.h"
#include "formula/numfmt.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

namespace yc::formula::fn {
namespace {

using ArrP = std::shared_ptr<Array>;

// ---- UTF-8 <-> UTF-16 ----

std::wstring Widen(std::string_view s) {
  std::wstring out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    size_t len;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c >> 5) == 6 && i + 1 < s.size()) {
      cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
      len = 2;
    } else if ((c >> 4) == 14 && i + 2 < s.size()) {
      cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
      len = 3;
    } else if ((c >> 3) == 30 && i + 3 < s.size()) {
      cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) | ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
      len = 4;
    } else {
      cp = 0xFFFD;
      len = 1;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += static_cast<wchar_t>(0xD800 + (cp >> 10));
      out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
    } else {
      out += static_cast<wchar_t>(cp);
    }
    i += len;
  }
  return out;
}

std::string Narrow(std::wstring_view w) {
  std::string out;
  for (size_t i = 0; i < w.size(); ++i) {
    uint32_t cp = static_cast<uint16_t>(w[i]);
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size()) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint16_t>(w[i + 1]) - 0xDC00);
      ++i;
    }
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

// ---- VALUETOTEXT / ARRAYTOTEXT ----

std::string ValueText(const Value& v, bool strict) {
  if (v.IsString()) {
    if (!strict) return Evaluator::ToText(v);
    std::string s = "\"";
    for (char c : Evaluator::ToText(v)) {
      if (c == '"') s += '"';
      s += c;
    }
    return s + "\"";
  }
  return Evaluator::ToText(v);
}

bool FormatArg(Evaluator& ev, const Node& call, int i, bool* strict, EV* err) {
  *strict = false;
  if (!Has(call, i)) return true;
  double d;
  Value e;
  if (!ev.ArgNumber(call.kids[static_cast<size_t>(i)], &d, &e)) {
    *err = EV::S(e);
    return false;
  }
  if (d != 0 && d != 1) {
    *err = Er(Err::Value);
    return false;
  }
  *strict = d == 1;
  return true;
}

EV FValueToText(Evaluator& ev, const Node& call) {
  bool strict;
  EV err;
  if (!FormatArg(ev, call, 1, &strict, &err)) return err;
  return Str(ValueText(ev.Scalar(call.kids[0]), strict));
}

EV FArrayToText(Evaluator& ev, const Node& call) {
  bool strict;
  EV err;
  if (!FormatArg(ev, call, 1, &strict, &err)) return err;
  const ArrP a = ev.ToArray(ev.Eval(call.kids[0]));
  if (!a) return Er(Err::Value);
  std::string s = strict ? "{" : "";
  for (uint32_t r = 0; r < a->rows; ++r) {
    for (uint32_t c = 0; c < a->cols; ++c) {
      if (r || c) s += strict ? (c ? "," : ";") : ", ";
      s += ValueText(a->At(r, c), strict);
    }
  }
  if (strict) s += "}";
  return Str(s);
}

// ---- BAHTTEXT (Thai words; UTF-8 escapes) ----

constexpr const char* kThDigits[] = {
    "\xE0\xB8\xA8\xE0\xB8\xB9\xE0\xB8\x99\xE0\xB8\xA2\xE0\xB9\x8C",  // 0
    "\xE0\xB8\xAB\xE0\xB8\x99\xE0\xB8\xB6\xE0\xB9\x88\xE0\xB8\x87",  // 1
    "\xE0\xB8\xAA\xE0\xB8\xAD\xE0\xB8\x87",                          // 2
    "\xE0\xB8\xAA\xE0\xB8\xB2\xE0\xB8\xA1",                          // 3
    "\xE0\xB8\xAA\xE0\xB8\xB5\xE0\xB9\x88",                          // 4
    "\xE0\xB8\xAB\xE0\xB9\x89\xE0\xB8\xB2",                          // 5
    "\xE0\xB8\xAB\xE0\xB8\x81",                                      // 6
    "\xE0\xB9\x80\xE0\xB8\x88\xE0\xB9\x87\xE0\xB8\x94",              // 7
    "\xE0\xB9\x81\xE0\xB8\x9B\xE0\xB8\x94",                          // 8
    "\xE0\xB9\x80\xE0\xB8\x81\xE0\xB9\x89\xE0\xB8\xB2",              // 9
};
// units of positions 1..5: ten, hundred, thousand, ten thousand, hundred thousand
constexpr const char* kThUnits[] = {
    "",
    "\xE0\xB8\xAA\xE0\xB8\xB4\xE0\xB8\x9A",
    "\xE0\xB8\xA3\xE0\xB9\x89\xE0\xB8\xAD\xE0\xB8\xA2",
    "\xE0\xB8\x9E\xE0\xB8\xB1\xE0\xB8\x99",
    "\xE0\xB8\xAB\xE0\xB8\xA1\xE0\xB8\xB7\xE0\xB9\x88\xE0\xB8\x99",
    "\xE0\xB9\x81\xE0\xB8\xAA\xE0\xB8\x99",
};
constexpr const char* kThMillion = "\xE0\xB8\xA5\xE0\xB9\x89\xE0\xB8\xB2\xE0\xB8\x99";
constexpr const char* kThTwentyPrefix = "\xE0\xB8\xA2\xE0\xB8\xB5\xE0\xB9\x88";
constexpr const char* kThEt = "\xE0\xB9\x80\xE0\xB8\xAD\xE0\xB9\x87\xE0\xB8\x94";
constexpr const char* kThBaht = "\xE0\xB8\x9A\xE0\xB8\xB2\xE0\xB8\x97";
constexpr const char* kThExact = "\xE0\xB8\x96\xE0\xB9\x89\xE0\xB8\xA7\xE0\xB8\x99";
constexpr const char* kThSatang = "\xE0\xB8\xAA\xE0\xB8\x95\xE0\xB8\xB2\xE0\xB8\x87\xE0\xB8\x84\xE0\xB9\x8C";
constexpr const char* kThMinus = "\xE0\xB8\xA5\xE0\xB8\x9A";

// 1..999999 in Thai words
std::string ThaiBelowMillion(int64_t n) {
  std::string s;
  const int64_t len = n == 0 ? 0 : static_cast<int64_t>(std::to_string(n).size());
  const std::string digits = std::to_string(n);
  for (int64_t i = 0; i < len; ++i) {
    const int d = digits[static_cast<size_t>(i)] - '0';
    const int64_t pos = len - 1 - i;  // 0 = units
    if (d == 0) continue;
    if (pos == 1 && d == 1) {
      s += kThUnits[1];  // "ten", not "one ten"
    } else if (pos == 1 && d == 2) {
      s += kThTwentyPrefix;
      s += kThUnits[1];
    } else if (pos == 0 && d == 1 && n > 9) {
      s += kThEt;  // a trailing one after tens: "et"
    } else {
      s += kThDigits[d];
      s += kThUnits[pos];
    }
  }
  return s;
}

std::string ThaiNumber(int64_t n) {
  if (n >= 1000000) {
    std::string s = ThaiNumber(n / 1000000) + kThMillion;
    if (n % 1000000) s += ThaiBelowMillion(n % 1000000);
    return s;
  }
  return ThaiBelowMillion(n);
}

EV FBahtText(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (!std::isfinite(x) || std::fabs(x) >= 1e15) return Er(Err::Num);
  const bool neg = x < 0;
  const double a = std::round(std::fabs(x) * 100);
  const auto baht = static_cast<int64_t>(a / 100);
  const auto satang = static_cast<int64_t>(a - static_cast<double>(baht) * 100);
  std::string s = neg ? kThMinus : "";
  if (baht == 0 && satang == 0) return Str(std::string(kThDigits[0]) + kThBaht + kThExact);
  if (baht > 0) s += ThaiNumber(baht) + kThBaht;
  if (satang == 0) s += kThExact;
  else s += ThaiNumber(satang) + kThSatang;
  return Str(s);
}

// ---- PHONETIC: the reading of Japanese text; without stored readings it is the text itself ----

EV FPhonetic(Evaluator& ev, const Node& call) {
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Scalar) return e.IsError() ? e : Str(Evaluator::ToText(e.s));
  std::string s;
  ev.ForEachValue(e, [&](const Value& v) {
    if (v.IsString()) s += Evaluator::ToText(v);
    return true;
  });
  return Str(s);
}

// ---- regular expressions ----

bool MakeRegex(const std::string& pattern, bool icase, std::wregex* out) {
  try {
    auto flags = std::regex::ECMAScript;
    if (icase) flags |= std::regex::icase;
    *out = std::wregex(Widen(pattern), flags);
    return true;
  } catch (const std::regex_error&) {
    return false;
  }
}

bool CaseArg(Evaluator& ev, const Node& call, int i, bool* icase, EV* err) {
  *icase = false;
  if (!Has(call, i)) return true;
  double d;
  Value e;
  if (!ev.ArgNumber(call.kids[static_cast<size_t>(i)], &d, &e)) {
    *err = EV::S(e);
    return false;
  }
  if (d != 0 && d != 1) {
    *err = Er(Err::Value);
    return false;
  }
  *icase = d == 1;
  return true;
}

EV FRegexTest(Evaluator& ev, const Node& call) {
  YC_TEXT(text, 0);
  YC_TEXT(pattern, 1);
  bool icase;
  EV err;
  if (!CaseArg(ev, call, 2, &icase, &err)) return err;
  std::wregex re;
  if (!MakeRegex(pattern, icase, &re)) return Er(Err::Value);
  return Bool(std::regex_search(Widen(text), re));
}

EV FRegexExtract(Evaluator& ev, const Node& call) {
  YC_TEXT(text, 0);
  YC_TEXT(pattern, 1);
  YC_NUM_OPT(mode, 2, 0);
  bool icase;
  EV err;
  if (!CaseArg(ev, call, 3, &icase, &err)) return err;
  if (mode != 0 && mode != 1 && mode != 2) return Er(Err::Value);
  std::wregex re;
  if (!MakeRegex(pattern, icase, &re)) return Er(Err::Value);
  const std::wstring w = Widen(text);
  if (mode == 0) {
    std::wsmatch m;
    if (!std::regex_search(w, m, re)) return Er(Err::NA);
    return Str(Narrow(m.str(0)));
  }
  if (mode == 1) {
    auto out = std::make_shared<Array>();
    for (auto it = std::wsregex_iterator(w.begin(), w.end(), re); it != std::wsregex_iterator(); ++it) out->v.push_back(Value::String(Narrow(it->str(0))));
    if (out->v.empty()) return Er(Err::NA);
    out->rows = static_cast<uint32_t>(out->v.size());
    out->cols = 1;
    return EV::A(std::move(out));
  }
  std::wsmatch m;
  if (!std::regex_search(w, m, re)) return Er(Err::NA);
  if (m.size() < 2) return Str(Narrow(m.str(0)));
  auto out = std::make_shared<Array>();
  for (size_t g = 1; g < m.size(); ++g) out->v.push_back(Value::String(Narrow(m.str(g))));
  out->rows = 1;
  out->cols = static_cast<uint32_t>(out->v.size());
  return EV::A(std::move(out));
}

EV FRegexReplace(Evaluator& ev, const Node& call) {
  YC_TEXT(text, 0);
  YC_TEXT(pattern, 1);
  YC_TEXT(repl, 2);
  YC_NUM_OPT(occ, 3, 0);
  bool icase;
  EV err;
  if (!CaseArg(ev, call, 4, &icase, &err)) return err;
  std::wregex re;
  if (!MakeRegex(pattern, icase, &re)) return Er(Err::Value);
  const std::wstring w = Widen(text), wr = Widen(repl);
  if (occ == 0) return Str(Narrow(std::regex_replace(w, re, wr)));
  std::vector<std::wsmatch> matches;
  for (auto it = std::wsregex_iterator(w.begin(), w.end(), re); it != std::wsregex_iterator(); ++it) matches.push_back(*it);
  const auto n = static_cast<int64_t>(matches.size());
  int64_t k = static_cast<int64_t>(std::trunc(occ));
  k = k > 0 ? k - 1 : n + k;
  if (k < 0 || k >= n) return Str(text);
  const std::wsmatch& m = matches[static_cast<size_t>(k)];
  std::wstring out = w.substr(0, static_cast<size_t>(m.position(0)));
  out += m.format(wr);
  out += w.substr(static_cast<size_t>(m.position(0) + m.length(0)));
  return Str(Narrow(out));
}

// ---- web ----

EV FEncodeUrl(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return Str(out);
}

EV FFilterXml(Evaluator& ev, const Node& call) {
  YC_TEXT(xml, 0);
  YC_TEXT(xpath, 1);
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return Er(Err::Value);
  pugi::xpath_node_set nodes;
  try {
    nodes = doc.select_nodes(xpath.c_str());
  } catch (const pugi::xpath_exception&) {
    return Er(Err::Value);
  }
  if (nodes.empty()) return Er(Err::Value);
  auto out = std::make_shared<Array>();
  for (const auto& n : nodes) {
    std::string text = n.attribute() ? n.attribute().value() : n.node().text().get();
    if (text.empty() && n.node()) text = n.node().child_value();
    // numbers come back as numbers (like Excel)
    char* end = nullptr;
    const double d = std::strtod(text.c_str(), &end);
    if (!text.empty() && end && *end == 0 && std::isfinite(d)) out->v.push_back(Value::Number(d));
    else out->v.push_back(Value::String(text));
  }
  out->rows = static_cast<uint32_t>(out->v.size());
  out->cols = 1;
  if (out->v.size() == 1) return EV::S(out->v[0]);
  return EV::A(std::move(out));
}

EV FWebService(Evaluator& ev, const Node& call) {
  YC_TEXT(url, 0);
  if (url.empty() || url.size() > 2048) return Er(Err::Value);
  const std::string low = Lower(url.substr(0, 8));
  if (low.rfind("http://", 0) != 0 && low.rfind("https://", 0) != 0) return Er(Err::Value);
  EV r = ev.host().WebFetch(url);
  // Excel: more than 32767 characters is #VALUE!
  if (r.k == EV::K::Scalar && r.s.IsString() && Utf16Length(Evaluator::ToText(r.s)) > 32767) return Er(Err::Value);
  return r;
}

EV FImage(Evaluator& ev, const Node& call) {
  YC_TEXT(url, 0);
  ImageSpec spec;
  spec.url = url;
  const std::string low = Lower(url.substr(0, 8));
  if (low.rfind("https://", 0) != 0 && low.rfind("http://", 0) != 0 && low.rfind("file:", 0) != 0) return Er(Err::Value);
  if (Has(call, 1)) {
    YC_TEXT(alt, 1);
    spec.alt = alt;
  }
  YC_NUM_OPT(sizing, 2, 0);
  if (sizing != 0 && sizing != 1 && sizing != 2 && sizing != 3) return Er(Err::Value);
  spec.sizing = static_cast<int>(sizing);
  YC_NUM_OPT(h, 3, 0);
  YC_NUM_OPT(w, 4, 0);
  if (spec.sizing == 3 && h <= 0 && w <= 0) return Er(Err::Value);
  if (h < 0 || w < 0) return Er(Err::Value);
  spec.height = h;
  spec.width = w;
  return Str(MakeImageValue(spec));
}

// ---- TRIMRANGE(range, [trim_rows], [trim_cols]): 0 none, 1 leading blanks, 2 trailing blanks, 3 both (default) ----

EV FTrimRange(Evaluator& ev, const Node& call) {
  RangeRef r;
  if (!ev.ArgRange(call.kids[0], &r)) return Er(Err::Value);
  YC_NUM_OPT(tr, 1, 3);
  YC_NUM_OPT(tc, 2, 3);
  if (tr < 0 || tr > 3 || tc < 0 || tc > 3) return Er(Err::Value);
  const core::Rect rect = ev.ClampRect(r.sheet, r.rect);
  bool any = false;
  core::Rect used{};
  ev.host().ForEachCell(r.sheet, rect, [&](uint32_t row, uint32_t col, const Value& v) {
    if (v.IsEmpty()) return true;
    if (!any) {
      used = core::Rect{row, col, row, col};
      any = true;
    } else {
      used.r0 = std::min(used.r0, row);
      used.r1 = std::max(used.r1, row);
      used.c0 = std::min(used.c0, col);
      used.c1 = std::max(used.c1, col);
    }
    return true;
  });
  RangeRef out = r;
  if (!any) {
    out.rect = core::Rect::Cell(r.rect.r0, r.rect.c0);
    return EV::R(out);
  }
  const int itr = static_cast<int>(tr), itc = static_cast<int>(tc);
  out.rect = r.rect;
  if (itr & 1) out.rect.r0 = used.r0;
  if (itr & 2) out.rect.r1 = used.r1;
  if (itc & 1) out.rect.c0 = used.c0;
  if (itc & 2) out.rect.c1 = used.c1;
  return EV::R(out);
}

// ---- AREAS(reference): the areas of a union like (A1:B2,D4) ----

int CountAreas(Evaluator& ev, const Node& n) {
  if (n.kind == NK::Paren && !n.kids.empty()) return CountAreas(ev, n.kids[0]);
  if (n.kind == NK::Binary && n.op == ',') {
    const int a = CountAreas(ev, n.kids[0]), b = CountAreas(ev, n.kids[1]);
    return a < 0 || b < 0 ? -1 : a + b;
  }
  const EV e = ev.Eval(n);
  return e.k == EV::K::Range ? 1 : -1;
}

EV FAreas(Evaluator& ev, const Node& call) {
  const int n = CountAreas(ev, call.kids[0]);
  return n < 0 ? Er(Err::Value) : Num(n);
}


// NUMBERSTRING(number, type): a number in Chinese characters (a hidden Excel function of Chinese editions)
//   1 lower-case numerals with units, 2 financial (banker's) numerals, 3 digit by digit
EV FNumberString(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(type_d, 1);
  const int type = static_cast<int>(type_d);
  if (type < 1 || type > 3 || x < 0) return Er(Err::Num);
  const double r = std::round(x);
  if (r >= 1e16) return Er(Err::Num);
  static const char* const kLower[] = {"\xE2\x97\x8B", "\xE4\xB8\x80", "\xE4\xBA\x8C", "\xE4\xB8\x89", "\xE5\x9B\x9B", "\xE4\xBA\x94", "\xE5\x85\xAD", "\xE4\xB8\x83", "\xE5\x85\xAB", "\xE4\xB9\x9D"};
  std::string digits = std::to_string(static_cast<long long>(r));
  if (type == 3) {
    std::string o;
    for (char c : digits) o += kLower[c - '0'];
    return EV::S(Value::String(o));
  }
  // one reading for NUMBERSTRING and the [DBNum1] / [DBNum2] formats: 10 is one ten, 1001 one thousand o one (Excel)
  return EV::S(Value::String(ChineseNumerals(static_cast<uint64_t>(r), type)));
}

// DATESTRING(date): the date in the Republic of China calendar, like Excel's Traditional Chinese edition
EV FDateString(Evaluator& ev, const Node& call) {
  const Value v = ev.ToScalar(ev.Eval(call.kids[0]));
  if (v.IsError()) return EV::S(v);
  double serial = 0;
  if (v.IsNumeric()) serial = v.AsDouble();
  else if (v.IsString() && ParseDateText(v.str(), &serial)) {
  } else return Er(Err::Value);
  int y, m, d;
  if (!SerialToYmd(serial, &y, &m, &d)) return Er(Err::Num);
  char b[64];
  std::string o = "\xE6\xB0\x91\xE5\x9C\x8B";  // the era name
  if (y >= 1912) {
    std::snprintf(b, sizeof b, "%d", y - 1911);
  } else {
    o += "\xE5\x89\x8D";  // "before" the era
    std::snprintf(b, sizeof b, "%d", 1912 - y);
  }
  o += b;
  std::snprintf(b, sizeof b, "%s%02d%s%02d%s", "\xE5\xB9\xB4", m, "\xE6\x9C\x88", d, "\xE6\x97\xA5");  // year, month, day
  o += b;
  return EV::S(Value::String(o));
}

// USDOLLAR(number, [decimals]): like DOLLAR, with the US dollar sign
EV FUsDollar(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  double dec = 2;
  if (call.kids.size() > 1) {
    YC_NUM(dd, 1);
    dec = dd;
  }
  const int places = static_cast<int>(std::trunc(dec));
  if (places > 127) return Er(Err::Value);
  const double p = std::pow(10.0, places);
  const double r = std::round(std::fabs(x) * p) / p;
  std::string code = "#,##0";
  if (places > 0) code += "." + std::string(static_cast<size_t>(places), '0');
  std::string body = "US$" + FormatValue(Value::Number(r), code).text;
  return EV::S(Value::String(x < 0 && r != 0 ? "(" + body + ")" : body));
}
}  // namespace

void RegisterText2(std::vector<FuncInfo>& t) {
  t.push_back({"VALUETOTEXT", 1, 2, FValueToText});
  t.push_back({"ARRAYTOTEXT", 1, 2, FArrayToText});
  t.push_back({"BAHTTEXT", 1, 1, FBahtText});
  t.push_back({"PHONETIC", 1, 1, FPhonetic});
  t.push_back({"REGEXTEST", 2, 3, FRegexTest});
  t.push_back({"REGEXEXTRACT", 2, 4, FRegexExtract});
  t.push_back({"REGEXREPLACE", 3, 5, FRegexReplace});
  t.push_back({"ENCODEURL", 1, 1, FEncodeUrl});
  t.push_back({"FILTERXML", 2, 2, FFilterXml});
  t.push_back({"TRIMRANGE", 1, 3, FTrimRange});
  t.push_back({"AREAS", 1, 1, FAreas});
  t.push_back({"WEBSERVICE", 1, 1, FWebService});
  t.push_back({"IMAGE", 1, 5, FImage});
  t.push_back({"NUMBERSTRING", 2, 2, FNumberString});
  t.push_back({"DATESTRING", 1, 1, FDateString});
  t.push_back({"USDOLLAR", 1, 2, FUsDollar});
}

}  // namespace yc::formula::fn

namespace yc::formula {

// ESC "IMG" US url US alt US sizing US height US width
std::string MakeImageValue(const ImageSpec& s) {
  auto num = [](double d) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", d);
    return std::string(b);
  };
  return "\x1BIMG\x1F" + s.url + "\x1F" + s.alt + "\x1F" + std::to_string(s.sizing) + "\x1F" + num(s.height) + "\x1F" + num(s.width);
}

bool ParseImageValue(std::string_view t, ImageSpec* out) {
  if (t.size() < 5 || t.substr(0, 5) != "\x1BIMG\x1F") return false;
  std::vector<std::string> parts;
  size_t pos = 5;
  while (true) {
    const size_t next = t.find('\x1F', pos);
    parts.emplace_back(t.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos));
    if (next == std::string_view::npos) break;
    pos = next + 1;
  }
  if (parts.size() != 5) return false;
  out->url = parts[0];
  out->alt = parts[1];
  out->sizing = std::atoi(parts[2].c_str());
  out->height = std::atof(parts[3].c_str());
  out->width = std::atof(parts[4].c_str());
  return true;
}

}  // namespace yc::formula
