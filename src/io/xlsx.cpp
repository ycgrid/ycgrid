// SPDX-License-Identifier: Apache-2.0
#include "io/xlsx.h"

#include "core/i18n.h"
#include "engine/document.h"
#include "engine/snapshot.h"
#include "formula/parser.h"
#include "io/ycg.h"

#include <pugixml.hpp>
#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <functional>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>

namespace yc::io {

namespace {

// ---------------------------------------------------------------------------
// ZIP
// ---------------------------------------------------------------------------
uint16_t U16(std::string_view s, size_t at) { return static_cast<uint16_t>(static_cast<uint8_t>(s[at]) | (static_cast<uint8_t>(s[at + 1]) << 8)); }
uint32_t U32(std::string_view s, size_t at) { return static_cast<uint32_t>(U16(s, at)) | (static_cast<uint32_t>(U16(s, at + 2)) << 16); }
void P16(std::string* s, uint32_t v) {
  s->push_back(static_cast<char>(v & 0xFF));
  s->push_back(static_cast<char>((v >> 8) & 0xFF));
}
void P32(std::string* s, uint32_t v) {
  P16(s, v & 0xFFFF);
  P16(s, v >> 16);
}

bool Inflate(std::string_view in, size_t size, std::string* out) {
  z_stream zs{};
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
  out->resize(size);
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = reinterpret_cast<Bytef*>(out->data());
  zs.avail_out = static_cast<uInt>(out->size());
  const int rc = inflate(&zs, Z_FINISH);
  inflateEnd(&zs);
  return rc == Z_STREAM_END && zs.total_out == size;
}

std::string Deflate(std::string_view in) {
  z_stream zs{};
  if (deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
  std::string out(deflateBound(&zs, static_cast<uLong>(in.size())), '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());
  const int rc = deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return rc == Z_STREAM_END ? out : std::string();
}

// ---------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------
std::string Esc(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (char ch : s) {
    switch (ch) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default:
        // XML 1.0 can't hold most control characters
        if (static_cast<unsigned char>(ch) < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') break;
        o.push_back(ch);
    }
  }
  return o;
}

std::string Num(double v) {
  char b[32];
  const auto r = std::to_chars(b, b + sizeof b, v);
  return std::string(b, r.ptr);
}

std::string ColName(uint32_t c) {
  std::string s;
  ++c;
  while (c) {
    s.insert(s.begin(), static_cast<char>('A' + (c - 1) % 26));
    c = (c - 1) / 26;
  }
  return s;
}
std::string CellRef(uint32_t r, uint32_t c) { return ColName(c) + std::to_string(r + 1); }

bool ParseRef(std::string_view s, uint32_t* r, uint32_t* c) {
  uint32_t col = 0, row = 0;
  size_t i = 0;
  while (i < s.size() && s[i] == '$') ++i;
  const size_t c0 = i;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) col = col * 26 + static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(s[i++])) - 'A' + 1);
  if (i == c0 || col == 0) return false;
  while (i < s.size() && s[i] == '$') ++i;
  const size_t r0 = i;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) row = row * 10 + static_cast<uint32_t>(s[i++] - '0');
  if (i == r0 || row == 0 || i != s.size()) return false;
  *r = row - 1;
  *c = col - 1;
  return true;
}

bool ParseRange(std::string_view s, core::Rect* out) {
  const size_t colon = s.find(':');
  uint32_t r0, c0, r1, c1;
  if (colon == std::string_view::npos) {
    if (!ParseRef(s, &r0, &c0)) return false;
    *out = core::Rect{r0, c0, r0, c0};
    return true;
  }
  if (!ParseRef(s.substr(0, colon), &r0, &c0) || !ParseRef(s.substr(colon + 1), &r1, &c1)) return false;
  *out = core::Rect{std::min(r0, r1), std::min(c0, c1), std::max(r0, r1), std::max(c0, c1)};
  return true;
}

// Excel's built-in number formats
const std::map<int, std::string>& BuiltinFormats() {
  static const std::map<int, std::string> m = {
      {0, "General"},       {1, "0"},           {2, "0.00"},           {3, "#,##0"},        {4, "#,##0.00"},
      {9, "0%"},            {10, "0.00%"},      {11, "0.00E+00"},      {12, "# ?/?"},        {13, "# ??/??"},
      {14, "m/d/yyyy"},     {15, "d-mmm-yy"},   {16, "d-mmm"},         {17, "mmm-yy"},       {18, "h:mm AM/PM"},
      // 37-40 as Excel reports them (with _) before the second section)
      {19, "h:mm:ss AM/PM"}, {20, "h:mm"},       {21, "h:mm:ss"},       {22, "m/d/yyyy h:mm"}, {37, "#,##0_);(#,##0)"},
      {38, "#,##0_);[Red](#,##0)"}, {39, "#,##0.00_);(#,##0.00)"}, {40, "#,##0.00_);[Red](#,##0.00)"}, {45, "mm:ss"}, {46, "[h]:mm:ss"},
      {47, "mmss.0"},       {48, "##0.0E+0"},   {49, "@"}};
  return m;
}

// the legacy indexed palette
constexpr uint32_t kIndexed[] = {
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF,
    0xFFFF00, 0xFF00FF, 0x00FFFF, 0x800000, 0x008000, 0x000080, 0x808000, 0x800080, 0x008080, 0xC0C0C0, 0x808080, 0x9999FF, 0x993366,
    0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080, 0x0066CC, 0xCCCCFF, 0x000080, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080,
    0x0000FF, 0x00CCFF, 0xCCFFFF, 0xCCFFCC, 0xFFFF99, 0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99, 0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00,
    0xFF9900, 0xFF6600, 0x666699, 0x969696, 0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399, 0x333333};

// a <color> element: false = automatic
bool ParseColor(const pugi::xml_node& n, uint32_t* out) {
  if (!n) return false;
  uint32_t rgb = 0;
  if (const auto a = n.attribute("rgb")) {
    const std::string v = a.value();
    rgb = static_cast<uint32_t>(std::strtoul(v.size() > 6 ? v.substr(v.size() - 6).c_str() : v.c_str(), nullptr, 16));
  } else if (const auto t = n.attribute("theme")) {
    // a theme color stays one (it follows Page Layout > Themes); .xlsx swaps the first two pairs
    static const int kOrder[] = {model::kLight1, model::kDark1, model::kLight2, model::kDark2, model::kAccent1, model::kAccent2,
                                 model::kAccent3, model::kAccent4, model::kAccent5, model::kAccent6, model::kHyperlink, model::kFollowed};
    const int i = t.as_int();
    if (i < 0 || i >= static_cast<int>(std::size(kOrder))) return false;
    *out = model::MakeThemeColor(kOrder[i], n.attribute("tint").as_double(0));
    return true;
  } else if (const auto x = n.attribute("indexed")) {
    const int i = x.as_int();
    if (i < 0 || i >= static_cast<int>(std::size(kIndexed))) return false;  // 64 = system foreground (automatic)
    rgb = kIndexed[i];
  } else {
    return false;
  }
  if (const double tint = n.attribute("tint").as_double(0); tint != 0) {
    auto ch = [&](int sh) {
      double c = static_cast<double>((rgb >> sh) & 0xFF);
      c = tint < 0 ? c * (1 + tint) : c + (255 - c) * tint;
      return static_cast<uint32_t>(std::clamp(std::lround(c), 0L, 255L)) << sh;
    };
    rgb = ch(16) | ch(8) | ch(0);
  }
  *out = 0xFF000000u | rgb;
  return true;
}

const model::Theme* g_export_theme = nullptr;  // the workbook being exported (theme colors are written as RGB)

std::string ColorAttr(uint32_t argb) {
  if (model::IsThemeColor(argb)) argb = model::ResolveColor(argb, g_export_theme ? *g_export_theme : model::CurrentTheme());
  char b[16];
  std::snprintf(b, sizeof b, "FF%06X", argb & 0xFFFFFF);
  return b;
}

const char* const kPatterns[] = {"solid",           "darkGray",      "mediumGray",     "lightGray",    "gray125",     "gray0625",
                                 "darkHorizontal",  "darkVertical",  "darkDown",       "darkUp",       "darkGrid",    "darkTrellis",
                                 "lightHorizontal", "lightVertical", "lightDown",      "lightUp",      "lightGrid",   "lightTrellis"};
const char* const kBorders[] = {"none", "thin", "medium", "dashed", "dotted", "thick", "double", "hair", "mediumDashed", "dashDot", "mediumDashDot",
                                "dashDotDot", "mediumDashDotDot", "slantDashDot"};
const char* const kHAlign[] = {"general", "left", "center", "right", "fill", "justify", "centerContinuous", "distributed"};
const char* const kVAlign[] = {"bottom", "center", "top", "justify", "distributed"};
const char* const kUnderline[] = {"none", "single", "double", "singleAccounting", "doubleAccounting"};

template <size_t N>
int IndexOf(const char* const (&names)[N], std::string_view v, int def) {
  for (size_t i = 0; i < N; ++i) {
    if (v == names[i]) return static_cast<int>(i);
  }
  return def;
}

// functions Excel writes with the _xlfn. prefix (added in Excel 2010 and later)
const std::set<std::string>& FutureFunctions() {
  static const std::set<std::string> s = {
      "ACOT", "ACOTH", "AGGREGATE", "ARABIC", "ARRAYTOTEXT", "BASE", "BETA.DIST", "BETA.INV", "BINOM.DIST", "BINOM.DIST.RANGE", "BINOM.INV",
      "BITAND", "BITLSHIFT", "BITOR", "BITRSHIFT", "BITXOR", "BYCOL", "BYROW", "CEILING.MATH", "CEILING.PRECISE", "CHISQ.DIST", "CHISQ.DIST.RT",
      "CHISQ.INV", "CHISQ.INV.RT", "CHISQ.TEST", "CHOOSECOLS", "CHOOSEROWS", "COMBINA", "CONCAT", "CONFIDENCE.NORM", "CONFIDENCE.T", "COT",
      "COTH", "COVARIANCE.P", "COVARIANCE.S", "CSC", "CSCH", "DAYS", "DECIMAL", "DROP", "ERF.PRECISE", "ERFC.PRECISE", "EXPAND", "EXPON.DIST",
      "F.DIST", "F.DIST.RT", "F.INV", "F.INV.RT", "F.TEST", "FILTER", "FILTERXML", "FLOOR.MATH", "FLOOR.PRECISE", "FORECAST.ETS",
      "FORECAST.ETS.CONFINT", "FORECAST.ETS.SEASONALITY", "FORECAST.ETS.STAT", "FORECAST.LINEAR", "FORMULATEXT", "GAMMA", "GAMMA.DIST",
      "GAMMA.INV", "GAMMALN.PRECISE", "GAUSS", "GROUPBY", "HSTACK", "HYPGEOM.DIST", "IFNA", "IFS", "IMAGE", "IMCOSH", "IMCOT", "IMCSC",
      "IMCSCH", "IMSEC", "IMSECH", "IMSINH", "IMTAN", "ISFORMULA", "ISOMITTED", "ISOWEEKNUM", "LAMBDA", "LET", "LOGNORM.DIST", "LOGNORM.INV",
      "MAKEARRAY", "MAP", "MAXIFS", "MINIFS", "MODE.MULT", "MODE.SNGL", "MUNIT", "NEGBINOM.DIST", "NETWORKDAYS.INTL", "NORM.DIST",
      "NORM.INV", "NORM.S.DIST", "NORM.S.INV", "NUMBERVALUE", "PDURATION", "PERCENTILE.EXC", "PERCENTILE.INC", "PERCENTOF", "PERCENTRANK.EXC",
      "PERCENTRANK.INC", "PERMUTATIONA", "PHI", "PIVOTBY", "POISSON.DIST", "QUARTILE.EXC", "QUARTILE.INC", "RANDARRAY", "RANK.AVG",
      "RANK.EQ", "REDUCE", "REGEXEXTRACT", "REGEXREPLACE", "REGEXTEST", "RRI", "SCAN", "SEC", "SECH", "SEQUENCE", "SHEET", "SHEETS",
      "SKEW.P", "SORT", "SORTBY", "STDEV.P", "STDEV.S", "SWITCH", "T.DIST", "T.DIST.2T", "T.DIST.RT", "T.INV", "T.INV.2T", "T.TEST", "TAKE",
      "TEXTAFTER", "TEXTBEFORE", "TEXTJOIN", "TEXTSPLIT", "TOCOL", "TOROW", "TRIMRANGE", "UNICHAR", "UNICODE", "UNIQUE", "VALUETOTEXT",
      "VAR.P", "VAR.S", "VSTACK", "WEBSERVICE", "WEIBULL.DIST", "WORKDAY.INTL", "WRAPCOLS", "WRAPROWS", "XLOOKUP", "XMATCH", "XOR",
      "Z.TEST", "ENCODEURL", "DETECTLANGUAGE", "TRANSLATE"};
  return s;
}

// FILTER, SORT, SORTBY, UNIQUE... are also in the worksheet namespace (_xlfn._xlws.)
bool XlwsFunction(const std::string& n) { return n == "FILTER" || n == "SORT"; }

// LET's and LAMBDA's parameter names in a formula (Excel writes them with the _xlpm. prefix, wherever they are used)
std::set<std::string> LambdaParams(const std::string& f) {
  std::set<std::string> out;
  auto upper = [](std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
  };
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] == '"') {
      const size_t e = f.find('"', i + 1);
      if (e == std::string::npos) break;
      i = e;
      continue;
    }
    if (!(std::isalpha(static_cast<unsigned char>(f[i])) || f[i] == '_') || (i > 0 && (std::isalnum(static_cast<unsigned char>(f[i - 1])) || f[i - 1] == '.' || f[i - 1] == '_'))) continue;
    size_t j = i;
    while (j < f.size() && (std::isalnum(static_cast<unsigned char>(f[j])) || f[j] == '_' || f[j] == '.')) ++j;
    const std::string name = upper(f.substr(i, j - i));
    if (j >= f.size() || f[j] != '(' || (name != "LET" && name != "LAMBDA")) {
      i = j - 1;
      continue;
    }
    // the call's top-level arguments
    std::vector<std::string> args;
    std::string cur;
    int depth = 0;
    size_t k = j + 1;
    for (; k < f.size(); ++k) {
      const char ch = f[k];
      if (ch == '"') {
        const size_t e = f.find('"', k + 1);
        cur.append(f, k, (e == std::string::npos ? f.size() : e + 1) - k);
        k = e == std::string::npos ? f.size() : e;
        continue;
      }
      if (ch == '(' || ch == '{') ++depth;
      if ((ch == ')' || ch == '}') && depth-- == 0) break;
      if (ch == ',' && depth == 0) {
        args.push_back(cur);
        cur.clear();
        continue;
      }
      cur += ch;
    }
    args.push_back(cur);
    auto add = [&](std::string a) {
      while (!a.empty() && a.front() == ' ') a.erase(a.begin());
      while (!a.empty() && a.back() == ' ') a.pop_back();
      if (!a.empty() && a.front() == '[' && a.back() == ']') a = a.substr(1, a.size() - 2);  // an optional parameter
      if (a.empty() || !(std::isalpha(static_cast<unsigned char>(a[0])) || a[0] == '_')) return;
      for (char ch : a) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.')) return;
      }
      out.insert(upper(a));
    };
    if (name == "LET") {
      for (size_t a = 0; a + 1 < args.size(); a += 2) add(args[a]);
    } else {
      for (size_t a = 0; a + 1 < args.size(); ++a) add(args[a]);
    }
    i = j;
  }
  return out;
}

// add / remove Excel's function prefixes outside string literals; LET / LAMBDA parameters get _xlpm., a spill
// reference A1# is written _xlfn.ANCHORARRAY(A1), and a reference to a deleted sheet (#REF!A1) is #REF! (Excel
// refused to open files written otherwise)
std::string AddPrefixes(const std::string& f) {
  const std::set<std::string> params = LambdaParams(f);
  std::string out;
  size_t i = 0;
  while (i < f.size()) {
    if (f[i] == '#' && f.compare(i, 5, "#REF!") == 0) {
      out += "#REF!";
      i += 5;
      while (i < f.size() && (std::isalnum(static_cast<unsigned char>(f[i])) || f[i] == '$' || f[i] == ':')) ++i;
      continue;
    }
    if (f[i] == '#' && !out.empty() && (std::isalnum(static_cast<unsigned char>(out.back())) || out.back() == '$')) {
      // A1# / Sheet1!A1# / 'My Sheet'!A1#: the reference before the # goes inside ANCHORARRAY
      size_t b = out.size();
      while (b > 0 && (std::isalnum(static_cast<unsigned char>(out[b - 1])) || out[b - 1] == '$')) --b;
      if (b > 0 && out[b - 1] == '!') {
        --b;
        if (b > 0 && out[b - 1] == '\'') {
          size_t q = b - 1;
          while (q > 0) {
            --q;
            if (out[q] == '\'' && (q == 0 || out[q - 1] != '\'')) break;
          }
          b = q;
        } else {
          while (b > 0 && (std::isalnum(static_cast<unsigned char>(out[b - 1])) || out[b - 1] == '_' || out[b - 1] == '.')) --b;
        }
      }
      const std::string ref = out.substr(b);
      out.resize(b);
      out += "_xlfn.ANCHORARRAY(" + ref + ")";
      ++i;
      continue;
    }
    if (f[i] == '"') {
      const size_t e = f.find('"', i + 1);
      const size_t end = e == std::string::npos ? f.size() : e + 1;
      out.append(f, i, end - i);
      i = end;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(f[i])) || f[i] == '_') {
      size_t j = i;
      while (j < f.size() && (std::isalnum(static_cast<unsigned char>(f[j])) || f[j] == '_' || f[j] == '.')) ++j;
      std::string name = f.substr(i, j - i);
      const bool call = j < f.size() && f[j] == '(';
      const bool qualified = i > 0 && (f[i - 1] == '!' || f[i - 1] == '.');
      std::string up = name;
      for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      const bool sheet_name = j < f.size() && f[j] == '!';
      if (!qualified && !sheet_name && params.count(up)) {
        name = "_xlpm." + name;
      } else if (call && !qualified) {
        if (FutureFunctions().count(up)) name = (XlwsFunction(up) ? "_xlfn._xlws." : "_xlfn.") + name;
      }
      out += name;
      i = j;
      continue;
    }
    out.push_back(f[i++]);
  }
  return out;
}

std::string StripPrefixes(std::string f) {
  // Excel's _xlfn.ANCHORARRAY(A1) is the spill reference A1#, and _xlfn.SINGLE(x) the implicit intersection @x
  for (const char* fn : {"_xlfn.ANCHORARRAY(", "_xlfn.SINGLE("}) {
    const size_t len = std::strlen(fn);
    for (size_t at = f.find(fn); at != std::string::npos; at = f.find(fn, at)) {
      int depth = 0;
      size_t e = at + len;
      for (; e < f.size(); ++e) {
        if (f[e] == '"') {
          const size_t q = f.find('"', e + 1);
          e = q == std::string::npos ? f.size() - 1 : q;
          continue;
        }
        if (f[e] == '(') ++depth;
        if (f[e] == ')' && depth-- == 0) break;
      }
      if (e >= f.size()) break;
      const std::string inner = f.substr(at + len, e - at - len);
      const std::string repl = fn[6] == 'A' ? inner + "#" : "@" + inner;
      f.replace(at, e + 1 - at, repl);
      at += repl.size();
    }
  }
  for (const char* p : {"_xlfn._xlws.", "_xlfn.", "_xlws.", "_xlpm."}) {
    for (size_t at = f.find(p); at != std::string::npos; at = f.find(p, at)) f.erase(at, std::strlen(p));
  }
  return f;
}

std::string Join(const std::string& dir, const std::string& target) {
  if (!target.empty() && target[0] == '/') return target.substr(1);
  std::filesystem::path p = std::filesystem::path(dir) / target;
  return p.lexically_normal().generic_string();
}

// ---------------------------------------------------------------------------
// import
// ---------------------------------------------------------------------------
struct ImportStyles {
  std::map<int, std::string> numfmts;
  std::vector<model::Style> fonts, fills, borders;  // partial styles (only their part is set)
  std::vector<model::Style> xfs;
};

void ReadStyles(const pugi::xml_document& doc, model::StyleTable& tbl, ImportStyles* st) {
  const auto ss = doc.child("styleSheet");
  for (const auto& n : ss.child("numFmts").children("numFmt")) st->numfmts[n.attribute("numFmtId").as_int()] = n.attribute("formatCode").value();
  for (const auto& f : ss.child("fonts").children("font")) {
    model::Style s;
    if (const auto n = f.child("name")) s.font_name = tbl.InternString(n.attribute("val").value());
    if (const auto z = f.child("sz")) s.font_size = static_cast<float>(z.attribute("val").as_double(11));
    auto flag = [&](const char* tag) {
      const auto n = f.child(tag);
      return n && n.attribute("val").as_bool(true);
    };
    s.bold = flag("b");
    s.italic = flag("i");
    s.strike = flag("strike");
    if (const auto u = f.child("u")) s.underline = static_cast<model::Underline>(IndexOf(kUnderline, u.attribute("val").as_string("single"), 1));
    if (const auto va = f.child("vertAlign")) {
      const std::string v = va.attribute("val").as_string();
      s.script = v == "superscript" ? 1 : v == "subscript" ? 2 : 0;
    }
    uint32_t c;
    if (ParseColor(f.child("color"), &c)) s.font_color = c;
    st->fonts.push_back(s);
  }
  for (const auto& f : ss.child("fills").children("fill")) {
    model::Style s;
    if (const auto p = f.child("patternFill")) {
      const std::string type = p.attribute("patternType").as_string("none");
      uint32_t fg = 0, bg = 0;
      const bool has_fg = ParseColor(p.child("fgColor"), &fg), has_bg = ParseColor(p.child("bgColor"), &bg);
      if (type == "solid") {
        if (has_fg) s.fill_color = fg;
      } else if (type != "none") {
        s.pattern = static_cast<model::FillPattern>(IndexOf(kPatterns, type, 0));
        s.pattern_color = has_fg ? fg : model::kColorAuto;
        s.fill_color = has_bg ? bg : model::Rgb(255, 255, 255);
      }
    } else if (const auto g = f.child("gradientFill")) {
      std::vector<uint32_t> stops;
      for (const auto& stop : g.children("stop")) {
        uint32_t c;
        if (ParseColor(stop.child("color"), &c)) stops.push_back(c);
      }
      if (stops.size() >= 2) {
        s.gradient = std::string(g.attribute("type").as_string("linear")) == "path" ? model::FillGradient::FromCenter : model::FillGradient::Linear;
        s.gradient_angle = static_cast<int16_t>(g.attribute("degree").as_int(0));
        if (s.gradient == model::FillGradient::FromCenter) {
          // a corner: left = right and top = bottom, each 0 or 1
          const double l = g.attribute("left").as_double(0), t = g.attribute("top").as_double(0);
          if ((l == 0 || l == 1) && (t == 0 || t == 1) && l == g.attribute("right").as_double(0) && t == g.attribute("bottom").as_double(0)) {
            s.gradient = model::FillGradient::FromCorner;
            s.gradient_angle = static_cast<int16_t>((l == 1 ? 1 : 0) + (t == 1 ? 2 : 0));
          }
        }
        s.fill_color = stops.front();
        s.gradient_color2 = stops.back();
      }
    }
    st->fills.push_back(s);
  }
  for (const auto& b : ss.child("borders").children("border")) {
    model::Style s;
    auto side = [&](const char* tag, int edge) {
      const auto n = b.child(tag);
      if (!n) return;
      const int ls = IndexOf(kBorders, n.attribute("style").as_string("none"), 0);
      if (ls == 0) return;
      s.border[edge].style = static_cast<model::LineStyle>(ls);
      uint32_t c;
      if (ParseColor(n.child("color"), &c)) s.border[edge].color = c;
    };
    side("left", model::kLeft);
    side("start", model::kLeft);
    side("top", model::kTop);
    side("right", model::kRight);
    side("end", model::kRight);
    side("bottom", model::kBottom);
    if (b.attribute("diagonalDown").as_bool()) side("diagonal", model::kDiagDown);
    if (b.attribute("diagonalUp").as_bool()) side("diagonal", model::kDiagUp);
    st->borders.push_back(s);
  }
  for (const auto& x : ss.child("cellXfs").children("xf")) {
    model::Style s;
    const size_t fi = x.attribute("fontId").as_uint(), fl = x.attribute("fillId").as_uint(), bi = x.attribute("borderId").as_uint();
    if (fi < st->fonts.size()) {
      const auto& f = st->fonts[fi];
      s.font_name = f.font_name, s.font_size = f.font_size, s.font_color = f.font_color, s.bold = f.bold, s.italic = f.italic, s.strike = f.strike, s.script = f.script,
      s.underline = f.underline;
    }
    if (fl < st->fills.size()) {
      const auto& f = st->fills[fl];
      s.fill_color = f.fill_color, s.pattern = f.pattern, s.pattern_color = f.pattern_color, s.gradient = f.gradient, s.gradient_angle = f.gradient_angle,
      s.gradient_color2 = f.gradient_color2;
    }
    if (bi < st->borders.size()) {
      for (int e = 0; e < model::kEdgeCount; ++e) s.border[e] = st->borders[bi].border[e];
    }
    const int nf = x.attribute("numFmtId").as_int();
    if (nf != 0) {
      auto it = st->numfmts.find(nf);
      const auto& bi2 = BuiltinFormats();
      auto bt = bi2.find(nf);
      const std::string code = it != st->numfmts.end() ? it->second : (bt != bi2.end() ? bt->second : std::string("General"));
      if (code != "General") s.numfmt = tbl.InternString(code);
    }
    if (const auto a = x.child("alignment")) {
      s.halign = static_cast<model::HAlign>(IndexOf(kHAlign, a.attribute("horizontal").as_string("general"), 0));
      s.valign = static_cast<model::VAlign>(IndexOf(kVAlign, a.attribute("vertical").as_string("bottom"), 0));
      s.wrap = a.attribute("wrapText").as_bool();
      s.shrink = a.attribute("shrinkToFit").as_bool();
      s.indent = static_cast<uint8_t>(std::clamp(a.attribute("indent").as_int(), 0, 250));
      const int rot = a.attribute("textRotation").as_int();
      s.rotation = static_cast<int16_t>(rot <= 90 ? rot : (rot <= 180 ? -(rot - 90) : 255));
    }
    if (const auto p = x.child("protection")) {
      s.locked = p.attribute("locked").as_bool(true);
      s.hidden = p.attribute("hidden").as_bool(false);
    }
    st->xfs.push_back(s);
  }
}

// the runs of a rich string (<r><rPr>...</rPr><t>...</t></r>); empty when it has none
model::RichText CellRich(const pugi::xml_node& n) {
  model::RichText rt;
  if (n.child("t")) return rt;
  uint32_t at = 0;
  for (const auto& r : n.children("r")) {
    const std::string t = r.child("t").text().get();
    model::RichRun run;
    run.start = at;
    if (const auto pr = r.child("rPr")) {
      auto flag = [&](const char* tag) -> int8_t {
        const auto x = pr.child(tag);
        return x && x.attribute("val").as_bool(true) ? 1 : 0;
      };
      if (const auto f = pr.child("rFont")) run.font = f.attribute("val").value();
      if (const auto z = pr.child("sz")) run.size = static_cast<float>(z.attribute("val").as_double(0));
      run.bold = flag("b");
      run.italic = flag("i");
      run.strike = flag("strike");
      const auto u = pr.child("u");
      run.underline = static_cast<int8_t>(u ? IndexOf(kUnderline, u.attribute("val").as_string("single"), 1) : 0);
      const std::string va = pr.child("vertAlign").attribute("val").as_string();
      run.script = static_cast<int8_t>(va == "superscript" ? 1 : va == "subscript" ? 2 : 0);
      uint32_t c;
      if (ParseColor(pr.child("color"), &c)) run.color = c;
    }
    rt.text += t;
    rt.runs.push_back(std::move(run));
    at += static_cast<uint32_t>(model::CharCount(t));
  }
  if (!model::Normalize(&rt)) rt.runs.clear();
  return rt;
}

std::string CellText(const pugi::xml_node& n) {
  // <t> or rich text runs <r><t>, without phonetic runs
  if (const auto t = n.child("t")) return t.text().get();
  std::string s;
  for (const auto& r : n.children("r")) s += r.child("t").text().get();
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// ZIP (public)
// ---------------------------------------------------------------------------
constexpr uint64_t kMaxZipEntry = uint64_t{1} << 30;  // the largest part of a workbook read (1 GiB)

bool ReadZip(std::string_view b, std::vector<ZipEntry>* out, std::string* error) {
  out->clear();
  if (b.size() < 22) {
    *error = TR("Not a ZIP file");
    return false;
  }
  size_t eocd = std::string_view::npos;
  for (size_t i = b.size() - 22 + 1; i-- > 0 && b.size() - i <= 65557;) {
    if (U32(b, i) == 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string_view::npos) {
    *error = TR("Not a ZIP file");
    return false;
  }
  const uint32_t count = U16(b, eocd + 10), cd = U32(b, eocd + 16);
  size_t p = cd;
  for (uint32_t k = 0; k < count; ++k) {
    if (p + 46 > b.size() || U32(b, p) != 0x02014b50) {
      *error = TR("The ZIP directory is damaged");
      return false;
    }
    const uint16_t method = U16(b, p + 10);
    const uint64_t csize = U32(b, p + 20), usize = U32(b, p + 24);
    const uint16_t nlen = U16(b, p + 28), xlen = U16(b, p + 30), clen = U16(b, p + 32);
    const uint64_t loff = U32(b, p + 42);
    ZipEntry e;
    e.name = std::string(b.substr(p + 46, nlen));
    p += 46 + nlen + xlen + clen;
    if (loff + 30 > b.size() || U32(b, loff) != 0x04034b50) {
      *error = TR("The ZIP file is damaged");
      return false;
    }
    const size_t data = loff + 30 + U16(b, loff + 26) + U16(b, loff + 28);
    if (data + csize > b.size()) {
      *error = TR("The ZIP file is damaged");
      return false;
    }
    if (usize > kMaxZipEntry) {  // a crafted directory: never allocate what the file claims
      *error = TR("An entry of the ZIP file is too large: ") + e.name;
      return false;
    }
    const std::string_view raw = b.substr(data, static_cast<size_t>(csize));
    if (method == 0) {
      e.data = std::string(raw);
    } else if (method == 8) {
      if (!Inflate(raw, static_cast<size_t>(usize), &e.data)) {
        *error = TR("Couldn't decompress ") + e.name;
        return false;
      }
    } else {
      continue;  // unsupported compression: skip the entry
    }
    out->push_back(std::move(e));
  }
  return true;
}

std::string WriteZip(const std::vector<ZipEntry>& entries) {
  std::string out, dir;
  for (const auto& e : entries) {
    const uint32_t crc = static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(e.data.data()), static_cast<uInt>(e.data.size())));
    std::string comp = Deflate(e.data);
    const bool stored = comp.empty() || comp.size() >= e.data.size();
    const std::string& body = stored ? e.data : comp;
    const uint32_t off = static_cast<uint32_t>(out.size());
    // local header (UTF-8 names, 1980-01-01 00:00)
    P32(&out, 0x04034b50);
    P16(&out, 20);
    P16(&out, 0x0800);
    P16(&out, stored ? 0 : 8);
    P16(&out, 0);
    P16(&out, 0x21);
    P32(&out, crc);
    P32(&out, static_cast<uint32_t>(body.size()));
    P32(&out, static_cast<uint32_t>(e.data.size()));
    P16(&out, static_cast<uint32_t>(e.name.size()));
    P16(&out, 0);
    out += e.name;
    out += body;
    P32(&dir, 0x02014b50);
    P16(&dir, 20);
    P16(&dir, 20);
    P16(&dir, 0x0800);
    P16(&dir, stored ? 0 : 8);
    P16(&dir, 0);
    P16(&dir, 0x21);
    P32(&dir, crc);
    P32(&dir, static_cast<uint32_t>(body.size()));
    P32(&dir, static_cast<uint32_t>(e.data.size()));
    P16(&dir, static_cast<uint32_t>(e.name.size()));
    P16(&dir, 0);
    P16(&dir, 0);
    P16(&dir, 0);
    P16(&dir, 0);
    P32(&dir, 0);
    P32(&dir, off);
    dir += e.name;
  }
  const uint32_t cd = static_cast<uint32_t>(out.size());
  out += dir;
  P32(&out, 0x06054b50);
  P16(&out, 0);
  P16(&out, 0);
  P16(&out, static_cast<uint32_t>(entries.size()));
  P16(&out, static_cast<uint32_t>(entries.size()));
  P32(&out, static_cast<uint32_t>(dir.size()));
  P32(&out, cd);
  P16(&out, 0);
  return out;
}

bool IsXlsxPath(const std::wstring& path) {
  std::wstring ext = std::filesystem::path(path).extension().wstring();
  for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
  return ext == L".xlsx" || ext == L".xlsm";
}

// ---------------------------------------------------------------------------
// import
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// What an Excel workbook holds that isn't carried over (XlsxSkipped), counted so the person is told
// ---------------------------------------------------------------------------
namespace {

// a formula read from the file keeps the cell's saved format (none inferred from its references)
constexpr uint8_t kFromFile = engine::kSrcFile;

std::string_view LocalName(const char* name) {
  const std::string_view n(name);
  const size_t colon = n.find(':');
  return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
bool EndsWith(std::string_view s, std::string_view suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }

// every element under n in document order, by its local name (without the namespace prefix); mc:Fallback, the copy
// of a Choice kept for older programs, is left out so nothing is counted twice
template <class Fn>
void ForEachElement(const pugi::xml_node& n, const Fn& fn) {
  for (const auto& c : n.children()) {
    if (c.type() != pugi::node_element) continue;
    const std::string_view name = LocalName(c.name());
    if (name == "Fallback") continue;
    fn(name, c);
    ForEachElement(c, fn);
  }
}

bool HasElement(const pugi::xml_node& n, std::string_view local) {
  bool found = false;
  ForEachElement(n, [&](std::string_view name, const pugi::xml_node&) { found = found || name == local; });
  return found;
}

int CountElements(const pugi::xml_node& n, std::string_view local) {
  int count = 0;
  ForEachElement(n, [&](std::string_view name, const pugi::xml_node&) { count += name == local; });
  return count;
}

// the workbook's parts YCGrid doesn't read: drawings (charts, pictures, shapes), PivotTables, slicers and timelines,
// tables, comments, controls, macros, links to other workbooks and data connections
void CountSkippedParts(const std::unordered_map<std::string, const std::string*>& files, XlsxSkipped* sk) {
  int legacy_comments = 0, threaded = 0;
  for (const auto& [name, data] : files) {
    const bool xml = EndsWith(name, ".xml");
    pugi::xml_document d;
    auto parsed = [&] { return d.load_buffer(data->data(), data->size()); };
    if (xml && StartsWith(name, "xl/pivotTables/pivotTable")) {
      ++sk->pivot_tables;
    } else if (xml && StartsWith(name, "xl/tables/table")) {
      ++sk->tables;
    } else if (xml && (StartsWith(name, "xl/ctrlProps/") || StartsWith(name, "xl/activeX/activeX"))) {
      ++sk->controls;
    } else if (xml && StartsWith(name, "xl/externalLinks/externalLink")) {
      ++sk->external_links;
    } else if (name == "xl/vbaProject.bin") {
      sk->macros = true;
    } else if (xml && StartsWith(name, "xl/slicers/") && parsed()) {
      sk->slicers += CountElements(d, "slicer");
    } else if (xml && StartsWith(name, "xl/timelines/") && parsed()) {
      sk->slicers += CountElements(d, "timeline");
    } else if (xml && StartsWith(name, "xl/comments") && parsed()) {
      legacy_comments += CountElements(d, "comment");
    } else if (xml && StartsWith(name, "xl/threadedComments/") && parsed()) {
      ForEachElement(d, [&](std::string_view n, const pugi::xml_node& e) { threaded += n == "threadedComment" && !e.attribute("parentId"); });
    } else if (name == "xl/connections.xml" && parsed()) {
      sk->queries += CountElements(d, "connection");
    } else if (xml && StartsWith(name, "xl/drawings/drawing") && parsed()) {
      ForEachElement(d, [&](std::string_view n, const pugi::xml_node& e) {
        if (n == "graphicFrame") {
          // a chart, or SmartArt and the like; slicers and timelines are counted from their own parts
          if (HasElement(e, "chart")) ++sk->charts;
          else if (!HasElement(e, "slicer") && !HasElement(e, "timeline")) ++sk->shapes;
        } else if (n == "pic") {
          ++sk->pictures;
        } else if (n == "sp") {
          if (!e.attribute("macro")) ++sk->shapes;  // with a macro attribute it is a form control (counted from its ctrlProps part)
        } else if (n == "cxnSp") {
          ++sk->shapes;
        }
      });
    }
  }
  sk->comments = std::max(legacy_comments, threaded);  // Excel writes a legacy comment beside every threaded one
}

// a worksheet's own parts YCGrid doesn't read: conditional formatting, data validation, hyperlinks, sparklines,
// frozen panes and protection (the cells are read, so sheetData is skipped)
void CountSkippedSheet(const pugi::xml_node& ws, XlsxSkipped* sk) {
  for (const auto& part : ws.children()) {
    if (part.type() != pugi::node_element) continue;
    const std::string_view pn = LocalName(part.name());
    if (pn == "sheetData") continue;
    if (pn == "sheetProtection") ++sk->protected_sheets;
    ForEachElement(part, [&](std::string_view n, const pugi::xml_node& e) {
      if (n == "cfRule") {
        // a rule in the x14 extension is a rule of its own, except a data bar, which Excel also writes as a plain rule
        const bool ext = std::string_view(e.name()).find(':') != std::string_view::npos;
        if (!ext || std::string_view(e.attribute("type").value()) != "dataBar") ++sk->cond_formats;
      } else if (n == "dataValidation") {
        ++sk->validations;
      } else if (n == "hyperlink" && LocalName(e.parent().name()) == "hyperlinks") {
        ++sk->hyperlinks;
      } else if (n == "sparklineGroup") {
        ++sk->sparklines;
      } else if (n == "pane" && StartsWith(e.attribute("state").value(), "frozen")) {
        ++sk->frozen_sheets;
      }
    });
  }
}

}  // namespace

bool XlsxSkipped::Any() const {
  return charts || pictures || shapes || controls || pivot_tables || slicers || tables || cond_formats || validations || comments ||
         hyperlinks || sparklines || frozen_sheets || protected_sheets || external_links || queries || panels || macros;
}

std::vector<std::string> XlsxSkipped::Lines() const {
  std::vector<std::string> out;
  auto add = [&](int n, const char* label) {
    if (n > 0) out.push_back(std::string(label) + ": " + std::to_string(n));
  };
  add(charts, TR("Charts"));
  add(pictures, TR("Pictures"));
  add(shapes, TR("Shapes and text boxes"));
  add(controls, TR("Form controls"));
  add(pivot_tables, TR("PivotTables"));
  add(slicers, TR("Slicers and timelines"));
  add(tables, TR("Tables"));
  add(cond_formats, TR("Conditional formatting rules"));
  add(validations, TR("Data validation rules"));
  add(comments, TR("Comments and notes"));
  add(hyperlinks, TR("Hyperlinks"));
  add(sparklines, TR("Sparkline groups"));
  add(frozen_sheets, TR("Sheets with frozen panes"));
  add(protected_sheets, TR("Protected sheets"));
  add(external_links, TR("Links to other workbooks"));
  add(queries, TR("Data connections and queries"));
  add(panels, TR("YCGrid panels"));
  if (macros) out.push_back(TR("Excel macros (YCGrid runs Python instead)"));
  return out;
}

XlsxSkipped XlsxExportSkipped(const model::Workbook& wb) {
  XlsxSkipped k;
  auto n = [](size_t v) { return static_cast<int>(v); };
  for (const auto& sh : wb.sheets) {
    k.charts += n(sh->charts->size());
    for (const model::ShapeObj& s : *sh->shapes) ++(s.kind == model::ShapeKind::Picture ? k.pictures : k.shapes);
    k.controls += n(sh->controls->size());
    k.pivot_tables += n(sh->pivots->size());
    k.slicers += n(sh->slicers->size());
    k.tables += n(sh->tables->size());
    k.cond_formats += n(sh->cond_formats->size());
    k.validations += n(sh->validations->size());
    k.comments += n(sh->notes->size());
    k.hyperlinks += n(sh->links->size());
    k.sparklines += n(sh->sparklines->size());
    if (sh->view.freeze_row || sh->view.freeze_col) ++k.frozen_sheets;
    if (sh->protection->on) ++k.protected_sheets;
  }
  k.external_links = n(wb.external_links->size());
  k.panels = n(wb.panels->size());
  return k;
}

bool ImportXlsxBytes(std::string_view bytes, model::Workbook* wb, std::string* error, std::vector<std::string>* warnings,
                     XlsxSkipped* skipped) {
  std::vector<ZipEntry> entries;
  if (!ReadZip(bytes, &entries, error)) return false;
  std::unordered_map<std::string, const std::string*> files;
  for (const auto& e : entries) files[e.name] = &e.data;
  auto load = [&](const std::string& name, pugi::xml_document* doc) {
    auto it = files.find(name);
    return it != files.end() && doc->load_buffer(it->second->data(), it->second->size(), pugi::parse_default | pugi::parse_ws_pcdata_single);
  };
  XlsxSkipped sk;
  CountSkippedParts(files, &sk);
  pugi::xml_document wbx, rels;
  if (!load("xl/workbook.xml", &wbx)) {
    *error = TR("This isn't an Excel workbook (xl/workbook.xml is missing).");
    return false;
  }
  std::map<std::string, std::string> rel_target;
  std::string sst_path = "xl/sharedStrings.xml", styles_path = "xl/styles.xml", theme_path = "xl/theme/theme1.xml";
  if (load("xl/_rels/workbook.xml.rels", &rels)) {
    for (const auto& r : rels.child("Relationships").children("Relationship")) {
      const std::string target = Join("xl", r.attribute("Target").value()), type = r.attribute("Type").value();
      rel_target[r.attribute("Id").value()] = target;
      if (type.find("/sharedStrings") != std::string::npos) sst_path = target;
      if (type.find("/styles") != std::string::npos) styles_path = target;
      if (type.find("/theme") != std::string::npos) theme_path = target;
    }
  }
  // the workbook's theme: its 12 colors and heading / body fonts
  model::Theme theme = model::DefaultTheme();
  bool has_theme = false;
  if (pugi::xml_document d; load(theme_path, &d)) {
    const auto elems = d.child("a:theme").child("a:themeElements");
    const auto scheme = elems.child("a:clrScheme");
    const char* tags[] = {"a:dk1", "a:lt1", "a:dk2", "a:lt2", "a:accent1", "a:accent2", "a:accent3", "a:accent4", "a:accent5", "a:accent6", "a:hlink", "a:folHlink"};
    for (int i = 0; i < model::kThemeColors; ++i) {
      const auto c = scheme.child(tags[i]);
      const char* v = c.child("a:srgbClr") ? c.child("a:srgbClr").attribute("val").value() : c.child("a:sysClr").attribute("lastClr").value();
      if (v && *v) {
        theme.colors[static_cast<size_t>(i)] = static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) & 0xFFFFFF;
        has_theme = true;
      }
    }
    if (const char* nm = scheme.attribute("name").value(); *nm) theme.colors_name = nm;
    const auto fonts = elems.child("a:fontScheme");
    if (const char* f = fonts.child("a:majorFont").child("a:latin").attribute("typeface").value(); *f) theme.major_font = f;
    if (const char* f = fonts.child("a:minorFont").child("a:latin").attribute("typeface").value(); *f) theme.minor_font = f;
    if (const char* nm = fonts.attribute("name").value(); *nm) theme.fonts_name = nm;
    if (const char* nm = d.child("a:theme").attribute("name").value(); *nm) theme.name = nm;
  }
  // shared strings
  std::vector<std::string> sst;
  std::vector<model::RichText> sst_rich;
  if (pugi::xml_document d; load(sst_path, &d)) {
    for (const auto& si : d.child("sst").children("si")) {
      sst.push_back(CellText(si));
      sst_rich.push_back(CellRich(si));
    }
  }
  engine::Document doc(model::Workbook::NewDefault(TR("Sheet")));
  if (has_theme) doc.wb_mut().theme.reset(theme);
  ImportStyles st;
  if (pugi::xml_document d; load(styles_path, &d)) ReadStyles(d, doc.wb_mut().styles.mut(), &st);
  // sheets
  struct SheetRef {
    std::string name, path;
    bool hidden;
    uint32_t id;
  };
  std::vector<SheetRef> sheets;
  // the workbook's date system and calculation options (Excel keeps them in workbook.xml)
  doc.wb_mut().date1904 = wbx.child("workbook").child("workbookPr").attribute("date1904").as_bool(false);
  doc.wb_mut().precision_as_displayed = !wbx.child("workbook").child("calcPr").attribute("fullPrecision").as_bool(true);
  doc.wb_mut().force_full_calc = wbx.child("workbook").child("calcPr").attribute("forceFullCalc").as_bool(false);
  for (const auto& s : wbx.child("workbook").child("sheets").children("sheet")) {
    const std::string rid = s.attribute("r:id").value();
    auto it = rel_target.find(rid);
    const std::string state = s.attribute("state").as_string("visible");
    sheets.push_back(SheetRef{s.attribute("name").value(), it != rel_target.end() ? it->second : std::string(), state != "visible", 0});
  }
  if (sheets.empty()) {
    *error = TR("The workbook has no sheets.");
    return false;
  }
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (i == 0) {
      sheets[0].id = doc.wb().sheets[0]->id;
      doc.RenameSheet(sheets[0].id, sheets[0].name);
    } else {
      sheets[i].id = doc.AddSheet(sheets[i].name);
    }
  }
  // defined names come first, before any cell: each definition calculates the workbook again, and defined after
  // the cells a workbook name over three cells spilled =Rate*2 over the cell a sheet-level Rate (defined next)
  // reads, which then read its own spill (doc_fuzz --more --xlsx seed 22032116)
  // defined names (Excel's own _xlnm names, except the print area, are not names in YCGrid)
  for (const auto& n : wbx.child("workbook").child("definedNames").children("definedName")) {
    std::string name = n.attribute("name").value();
    std::string refers = StripPrefixes(n.text().get());
    const int local = n.attribute("localSheetId").as_int(-1);
    if (name.rfind("_xlnm.", 0) == 0) {
      if (name == "_xlnm.Print_Area" && local >= 0 && static_cast<size_t>(local) < sheets.size()) {
        const size_t bang = refers.rfind('!');
        core::Rect r;
        std::string a = refers.substr(bang == std::string::npos ? 0 : bang + 1);
        a.erase(std::remove(a.begin(), a.end(), '$'), a.end());
        if (a.find(',') == std::string::npos && ParseRange(a, &r)) {
          doc.UpdatePageSetup(sheets[static_cast<size_t>(local)].id, [&](model::PageSetup& ps) {
            ps.has_area = true;
            ps.area = r;
          });
        }
      }
      continue;
    }
    model::NameDef def;
    def.name = name;
    def.refers_to = refers;
    def.sheet_scope = local >= 0 && static_cast<size_t>(local) < sheets.size() ? static_cast<int32_t>(sheets[static_cast<size_t>(local)].id) : -1;
    def.hidden = n.attribute("hidden").as_bool();
    std::string err;
    if (!doc.DefineName(def, &err) && warnings) warnings->push_back(TR("Skipped the name ") + name + ": " + err);
  }
  const uint32_t kMaxRow = core::kMaxRows - 1, kMaxCol = core::kMaxCols - 1;
  size_t formula_errors = 0;
  for (const auto& sr : sheets) {
    pugi::xml_document sx;
    if (sr.path.empty() || !load(sr.path, &sx)) {
      if (warnings) warnings->push_back(TR("Skipped a sheet that isn't a worksheet: ") + sr.name);
      continue;
    }
    const auto ws = sx.child("worksheet");
    CountSkippedSheet(ws, &sk);
    model::Sheet* msh = doc.wb_mut().MutSheetById(sr.id);
    if (sr.hidden) msh->visible = false;
    // column widths: characters -> pixels (Calibri 11: 7 px per character + 5 px of padding)
    auto char_px = [](double w) { return static_cast<int32_t>(std::lround(w * 7.0 + 5.0)); };
    if (const auto fmt = ws.child("sheetFormatPr")) {
      if (const auto dw = fmt.attribute("defaultColWidth")) msh->cols.mut().set_default_size(char_px(dw.as_double()));
      if (const auto dh = fmt.attribute("defaultRowHeight")) msh->rows.mut().set_default_size(static_cast<int32_t>(std::lround(dh.as_double() * 96.0 / 72.0)));
    }
    for (const auto& c : ws.child("cols").children("col")) {
      const uint32_t a = c.attribute("min").as_uint(1) - 1, b = std::min(c.attribute("max").as_uint(1) - 1, kMaxCol);
      for (uint32_t k = a; k <= b && k - a < 16384; ++k) {
        if (c.attribute("width")) msh->cols.mut().Set(k, char_px(c.attribute("width").as_double()));
        if (c.attribute("hidden").as_bool()) msh->cols.mut().Hide(k, true);
      }
    }
    // cells
    struct Shared {
      std::string text;
      uint32_t r, c;
    };
    std::map<int, Shared> shared;
    // dynamic arrays (cm="1" on an array formula): their other cells are the spilled values, not typed ones (typed
    // values would block the array: #SPILL!)
    std::vector<core::Rect> dynamic;
    struct Member {
      uint32_t r, c;
      core::Value v;
    };
    std::vector<Member> members;  // the values the file gives a dynamic array's other cells
    auto spilled = [&](uint32_t r, uint32_t c) {
      for (const core::Rect& d : dynamic) {
        if (d.Contains(r, c) && !(d.r0 == r && d.c0 == c)) return true;
      }
      return false;
    };
    for (const auto& row : ws.child("sheetData").children("row")) {
      const uint32_t rr = row.attribute("r").as_uint(0);
      if (rr == 0 || rr - 1 > kMaxRow) continue;
      const uint32_t r = rr - 1;
      if (row.attribute("customHeight").as_bool() || row.attribute("ht")) {
        if (row.attribute("ht")) msh->rows.mut().Set(r, static_cast<int32_t>(std::lround(row.attribute("ht").as_double() * 96.0 / 72.0)));
      }
      if (row.attribute("hidden").as_bool()) msh->rows.mut().Hide(r, true);
      uint32_t next_c = 0;
      for (const auto& cell : row.children("c")) {
        uint32_t cr = r, cc = next_c;
        if (const auto ref = cell.attribute("r")) {
          if (!ParseRef(ref.value(), &cr, &cc)) continue;
        }
        next_c = cc + 1;
        if (cc > kMaxCol) continue;
        const std::string t = cell.attribute("t").as_string("n");
        const size_t si = cell.attribute("s").as_uint(0);
        if (si > 0 && si < st.xfs.size()) {
          const uint32_t sid = doc.wb_mut().styles.mut().Intern(st.xfs[si]);
          doc.wb_mut().MutSheetById(sr.id)->SetStyle(cr, cc, sid);
        } else if (si == 0 && !st.xfs.empty() && !(st.xfs[0] == model::Style{})) {
          const uint32_t sid = doc.wb_mut().styles.mut().Intern(st.xfs[0]);
          if (sid) doc.wb_mut().MutSheetById(sr.id)->SetStyle(cr, cc, sid);
        }
        // formula
        if (const auto f = cell.child("f")) {
          const std::string ftype = f.attribute("t").as_string("normal");
          std::string text = StripPrefixes(f.text().get());
          if (ftype == "shared") {
            const int id = f.attribute("si").as_int(-1);
            if (!text.empty()) {
              shared[id] = Shared{text, cr, cc};
            } else if (auto it = shared.find(id); it != shared.end()) {
              // the master's formula moved to this cell
              const auto pr = formula::Parse(it->second.text, core::CellPos{it->second.r, it->second.c}, &doc);
              text = pr.ok ? formula::ToA1(pr.root, core::CellPos{cr, cc}, &doc) : std::string();
            }
          }
          if (!text.empty()) {
            std::string err;
            bool ok;
            if (ftype == "array" && f.attribute("ref") && !cell.attribute("cm")) {
              core::Rect ar;
              // an array formula without cm is a legacy one (Ctrl+Shift+Enter), of one cell too: read as a plain formula
              // it spilled (=A1:B3*2 in F1 filled F1:G3 where Excel shows the array's first value)
              ok = ParseRange(f.attribute("ref").value(), &ar) ? doc.SetArrayFormula(sr.id, ar, text, &err, kFromFile)
                                                               : doc.SetFormula(sr.id, cr, cc, text, &err, kFromFile);
            } else {
              core::Rect ar;
              if (ftype == "array" && cell.attribute("cm") && ParseRange(f.attribute("ref").value(), &ar) && !ar.IsCell()) dynamic.push_back(ar);
              ok = doc.SetFormula(sr.id, cr, cc, text, &err, kFromFile);
            }
            if (ok) continue;
            ++formula_errors;
          }
        }
        // value
        const auto v = cell.child("v");
        core::Value val;
        model::RichText rich;
        if (t == "s") {
          const size_t k = v.text().as_uint(0);
          if (k < sst.size()) {
            val = core::Value::String(sst[k]);
            rich = sst_rich[k];
          }
        } else if (t == "inlineStr") {
          val = core::Value::String(CellText(cell.child("is")));
          rich = CellRich(cell.child("is"));
        } else if (t == "str") {
          val = core::Value::String(v.text().get());
        } else if (t == "b") {
          val = core::Value::Bool(v.text().as_int() != 0);
        } else if (t == "e") {
          core::Err e;
          val = core::ParseErrorText(v.text().get(), &e) ? core::Value::Error(e) : core::Value::Error(core::Err::Value);
        } else if (v) {
          val = core::Value::Number(v.text().as_double());
        }
        if (!val.IsEmpty() && !dynamic.empty() && spilled(cr, cc)) members.push_back({cr, cc, val});
        else if (!val.IsEmpty()) doc.SetValue(sr.id, cr, cc, val, kFromFile);
        if (!rich.runs.empty()) doc.SetRichText(sr.id, cr, cc, std::move(rich));
      }
    }
    for (const auto& m : ws.child("mergeCells").children("mergeCell")) {
      core::Rect r;
      if (ParseRange(m.attribute("ref").value(), &r) && !r.IsCell()) doc.AddMergedArea(sr.id, r);
    }
    // a dynamic array keeps the range the file says it spilled over: the arrays are calculated again on open, and of
    // two arrays that meet, the one that held the cells keeps them (read in reading order, =SEQUENCE(3) in C2 spilled
    // and =TRANSPOSE(A1:B2) in B3, which had held C3:C4, showed #SPILL!; doc_fuzz --more seed 110480)
    if (model::Sheet* ms = doc.wb_mut().MutSheetById(sr.id)) {
      for (const core::Rect& d : dynamic) {
        if (!ms->GetFormula(d.r0, d.c0) || d.r1 >= ms->row_limit || d.c1 >= core::kMaxCols) continue;
        bool clash = false;
        for (const auto& [k, sp] : *ms->spills) clash = clash || sp.rect.Intersects(d);
        if (!clash) ms->spills.mut()[(uint64_t{d.r0} << 16) | d.c0] = model::SpillRange{d, false};
      }
      // its other cells hold the values the file gives them, as a .ycg's spilled values: the array's own, which it
      // calculates again (without them, an array of the cells it had held was woken first and took them)
      for (const Member& m : members) {
        for (const auto& [k, sp] : *ms->spills) {
          if (!sp.cse && sp.rect.Contains(m.r, m.c)) {
            ms->SetValue(m.r, m.c, m.v);
            break;
          }
        }
      }
    }
  }
  if (formula_errors && warnings) warnings->push_back(std::to_string(formula_errors) + TR(" formulas couldn't be read and were kept as values"));
  doc.Recalc();
  doc.ClearUndo();
  *wb = doc.wb();
  wb->dirty = false;
  wb->path.clear();
  if (skipped) *skipped = sk;
  return true;
}

bool ImportXlsx(const std::wstring& path, model::Workbook* wb, std::string* error, std::vector<std::string>* warnings,
                XlsxSkipped* skipped) {
  std::string bytes;
  if (!ReadFileBytes(path, &bytes)) {
    *error = TR("Couldn't read the file.");
    return false;
  }
  return ImportXlsxBytes(bytes, wb, error, warnings, skipped);
}

// ---------------------------------------------------------------------------
// export
// ---------------------------------------------------------------------------
namespace {

struct StyleHash {
  size_t operator()(const model::Style& s) const { return s.Hash(); }
};

struct ExportStyles {
  const model::StyleTable& tbl;
  std::vector<std::string> fonts, fills, borders, xfs;
  std::map<std::string, int> font_ix, fill_ix, border_ix, xf_ix;
  std::map<std::string, int> numfmt_ix;  // custom codes -> 164...
  std::unordered_map<model::Style, int, StyleHash> style_ix;

  explicit ExportStyles(const model::StyleTable& t) : tbl(t) {
    // Excel needs the default font, the two default fills and an empty border first
    Index(model::Style{});
  }

  static int Intern(std::vector<std::string>& v, std::map<std::string, int>& ix, const std::string& x) {
    auto it = ix.find(x);
    if (it != ix.end()) return it->second;
    ix[x] = static_cast<int>(v.size());
    v.push_back(x);
    return ix[x];
  }

  int Index(const model::Style& s) {
    auto it = style_ix.find(s);
    if (it != style_ix.end()) return it->second;
    // font
    std::string f = "<font>";
    if (s.bold) f += "<b/>";
    if (s.italic) f += "<i/>";
    if (s.strike) f += "<strike/>";
    if (s.underline != model::Underline::None) {
      f += "<u";
      if (s.underline != model::Underline::Single) f += std::string(" val=\"") + kUnderline[static_cast<int>(s.underline)] + "\"";
      f += "/>";
    }
    if (s.script) f += s.script == 1 ? "<vertAlign val=\"superscript\"/>" : "<vertAlign val=\"subscript\"/>";
    f += "<sz val=\"" + Num(tbl.FontSize(s)) + "\"/>";
    // an automatic color is written as none (OOXML's automatic): <color theme="1"/> opened as the theme's Text 1,
    // a color of its own (doc_fuzz --xlsx seed 76000)
    if (s.font_color >> 24) f += "<color rgb=\"" + ColorAttr(s.font_color) + "\"/>";
    f += "<name val=\"" + Esc(tbl.FontName(s)) + "\"/><family val=\"2\"/></font>";
    const int fi = Intern(fonts, font_ix, f);
    // fill (0 none and 1 gray125 are reserved by Excel)
    if (fills.empty()) {
      Intern(fills, fill_ix, "<fill><patternFill patternType=\"none\"/></fill>");
      Intern(fills, fill_ix, "<fill><patternFill patternType=\"gray125\"/></fill>");
    }
    int fl = 0;
    if (s.gradient != model::FillGradient::None) {
      std::string g = s.gradient == model::FillGradient::FromCenter ? "<fill><gradientFill type=\"path\" left=\"0.5\" right=\"0.5\" top=\"0.5\" bottom=\"0.5\">"
                                                                    : "<fill><gradientFill degree=\"" + std::to_string(s.gradient_angle) + "\">";
      if (s.gradient == model::FillGradient::FromCorner) {
        const char* x = (s.gradient_angle == 1 || s.gradient_angle == 3) ? "1" : "0";
        const char* y = s.gradient_angle >= 2 ? "1" : "0";
        g = std::string("<fill><gradientFill type=\"path\" left=\"") + x + "\" right=\"" + x + "\" top=\"" + y + "\" bottom=\"" + y + "\">";
      }
      const uint32_t c1 = (s.fill_color >> 24) ? s.fill_color : model::Rgb(255, 255, 255), c2 = (s.gradient_color2 >> 24) ? s.gradient_color2 : model::Rgb(255, 255, 255);
      g += "<stop position=\"0\"><color rgb=\"" + ColorAttr(c1) + "\"/></stop><stop position=\"1\"><color rgb=\"" + ColorAttr(c2) + "\"/></stop></gradientFill></fill>";
      fl = Intern(fills, fill_ix, g);
    } else if (s.pattern != model::FillPattern::Solid) {
      std::string p = std::string("<fill><patternFill patternType=\"") + kPatterns[static_cast<int>(s.pattern)] + "\">";
      p += (s.pattern_color >> 24) ? "<fgColor rgb=\"" + ColorAttr(s.pattern_color) + "\"/>" : std::string("<fgColor indexed=\"64\"/>");
      if (s.fill_color >> 24) p += "<bgColor rgb=\"" + ColorAttr(s.fill_color) + "\"/>";
      p += "</patternFill></fill>";
      fl = Intern(fills, fill_ix, p);
    } else if (s.fill_color >> 24) {
      fl = Intern(fills, fill_ix, "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"" + ColorAttr(s.fill_color) + "\"/><bgColor indexed=\"64\"/></patternFill></fill>");
    }
    // border
    auto side = [&](const char* tag, const model::BorderSide& b) {
      if (b.style == model::LineStyle::None) return "<" + std::string(tag) + "/>";
      std::string x = "<" + std::string(tag) + " style=\"" + kBorders[static_cast<int>(b.style)] + "\">";
      x += (b.color >> 24) ? "<color rgb=\"" + ColorAttr(b.color) + "\"/>" : std::string("<color indexed=\"64\"/>");
      return x + "</" + tag + ">";
    };
    const bool dd = s.border[model::kDiagDown].style != model::LineStyle::None, du = s.border[model::kDiagUp].style != model::LineStyle::None;
    std::string b = "<border";
    if (dd) b += " diagonalDown=\"1\"";
    if (du) b += " diagonalUp=\"1\"";
    b += ">" + side("left", s.border[model::kLeft]) + side("right", s.border[model::kRight]) + side("top", s.border[model::kTop]) +
         side("bottom", s.border[model::kBottom]) + side("diagonal", dd ? s.border[model::kDiagDown] : s.border[model::kDiagUp]) + "</border>";
    const int bi = Intern(borders, border_ix, b);
    // number format
    int nf = 0;
    const std::string code = tbl.NumberFormat(s);
    if (code != "General") {
      nf = -1;
      for (const auto& [id, c] : BuiltinFormats()) {
        if (c == code) nf = id;
      }
      if (nf < 0) {
        auto it2 = numfmt_ix.find(code);
        if (it2 == numfmt_ix.end()) it2 = numfmt_ix.emplace(code, 164 + static_cast<int>(numfmt_ix.size())).first;
        nf = it2->second;
      }
    }
    std::string x = "<xf numFmtId=\"" + std::to_string(nf) + "\" fontId=\"" + std::to_string(fi) + "\" fillId=\"" + std::to_string(fl) + "\" borderId=\"" +
                    std::to_string(bi) + "\" xfId=\"0\"";
    if (nf) x += " applyNumberFormat=\"1\"";
    if (fi) x += " applyFont=\"1\"";
    if (fl) x += " applyFill=\"1\"";
    if (bi) x += " applyBorder=\"1\"";
    const bool align = s.halign != model::HAlign::General || s.valign != model::VAlign::Bottom || s.wrap || s.shrink || s.indent || s.rotation;
    const bool prot = !s.locked || s.hidden;
    if (align) x += " applyAlignment=\"1\"";
    if (prot) x += " applyProtection=\"1\"";
    if (!align && !prot) {
      x += "/>";
    } else {
      x += ">";
      if (align) {
        x += "<alignment";
        if (s.halign != model::HAlign::General) x += std::string(" horizontal=\"") + kHAlign[static_cast<int>(s.halign)] + "\"";
        if (s.valign != model::VAlign::Bottom) x += std::string(" vertical=\"") + kVAlign[static_cast<int>(s.valign)] + "\"";
        if (s.wrap) x += " wrapText=\"1\"";
        if (s.shrink) x += " shrinkToFit=\"1\"";
        if (s.indent) x += " indent=\"" + std::to_string(s.indent) + "\"";
        if (s.rotation) x += " textRotation=\"" + std::to_string(s.rotation == 255 ? 255 : (s.rotation > 0 ? s.rotation : 90 - s.rotation)) + "\"";
        x += "/>";
      }
      if (prot) x += std::string("<protection") + (!s.locked ? " locked=\"0\"" : "") + (s.hidden ? " hidden=\"1\"" : "") + "/>";
      x += "</xf>";
    }
    const int idx = Intern(xfs, xf_ix, x);
    style_ix[s] = idx;
    return idx;
  }

  std::string Xml() const {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
    if (!numfmt_ix.empty()) {
      o += "<numFmts count=\"" + std::to_string(numfmt_ix.size()) + "\">";
      for (const auto& [code, id] : numfmt_ix) o += "<numFmt numFmtId=\"" + std::to_string(id) + "\" formatCode=\"" + Esc(code) + "\"/>";
      o += "</numFmts>";
    }
    auto list = [&](const char* tag, const std::vector<std::string>& v) {
      o += std::string("<") + tag + " count=\"" + std::to_string(v.size()) + "\">";
      for (const auto& x : v) o += x;
      o += std::string("</") + tag + ">";
    };
    list("fonts", fonts);
    list("fills", fills);
    list("borders", borders);
    o += "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>";
    list("cellXfs", xfs);
    o += "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>";
    return o;
  }
};

std::string QuoteSheet(const std::string& name) {
  bool plain = !name.empty() && !std::isdigit(static_cast<unsigned char>(name[0]));
  for (char ch : name) plain = plain && (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || static_cast<unsigned char>(ch) >= 0x80);
  if (plain) return name;
  std::string q = "'";
  for (char ch : name) {
    q += ch;
    if (ch == '\'') q += '\'';
  }
  return q + "'";
}

}  // namespace

std::string ExportXlsxBytes(const model::Workbook& wb) {
  g_export_theme = &*wb.theme;
  ExportStyles styles(*wb.styles);
  std::vector<std::string> sst;  // the inside of each <si>
  std::unordered_map<std::string, int> sst_ix;
  size_t sst_refs = 0;
  auto t_elem = [](const std::string& s) {
    const bool keep = !s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find('\n') != std::string::npos);
    return std::string("<t") + (keep ? " xml:space=\"preserve\"" : "") + ">" + Esc(s) + "</t>";
  };
  auto shared_xml = [&](const std::string& key, const std::function<std::string()>& xml) {
    ++sst_refs;
    auto it = sst_ix.find(key);
    if (it != sst_ix.end()) return it->second;
    sst_ix[key] = static_cast<int>(sst.size());
    sst.push_back(xml());
    return static_cast<int>(sst.size()) - 1;
  };
  auto shared = [&](const std::string& s) { return shared_xml(s, [&] { return t_elem(s); }); };
  // a string with fonts inside: runs with their full font (<rPr>), like Excel writes them
  auto shared_rich = [&](const model::RichText& rt, const model::Style& cell) {
    std::string xml;
    for (size_t i = 0; i < rt.runs.size(); ++i) {
      const size_t a = model::ByteOfChar(rt.text, rt.runs[i].start);
      const size_t b = i + 1 < rt.runs.size() ? model::ByteOfChar(rt.text, rt.runs[i + 1].start) : rt.text.size();
      if (b <= a) continue;
      const model::RunFont f = model::ResolveRunFont(*wb.styles, cell, &rt.runs[i]);
      xml += "<r><rPr>";
      if (f.bold) xml += "<b/>";
      if (f.italic) xml += "<i/>";
      if (f.strike) xml += "<strike/>";
      if (f.underline != model::Underline::None) {
        xml += "<u";
        if (f.underline != model::Underline::Single) xml += std::string(" val=\"") + kUnderline[static_cast<int>(f.underline)] + "\"";
        xml += "/>";
      }
      if (f.script) xml += f.script == 1 ? "<vertAlign val=\"superscript\"/>" : "<vertAlign val=\"subscript\"/>";
      xml += "<sz val=\"" + Num(f.size) + "\"/>";
      if (f.color >> 24) xml += "<color rgb=\"" + ColorAttr(f.color) + "\"/>";
      xml += "<rFont val=\"" + Esc(f.name) + "\"/><family val=\"2\"/></rPr>" + t_elem(rt.text.substr(a, b - a)) + "</r>";
    }
    return shared_xml("\x01" + xml, [&] { return xml; });
  };
  std::vector<ZipEntry> files;
  bool dynamic_arrays = false;  // a cell with cm="1": metadata.xml says what it means
  std::string sheets_xml, rels_xml, types_xml, names_xml;
  int idx = 0;
  for (const auto& shp : wb.sheets) {
    const model::Sheet& sh = *shp;
    ++idx;
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
    const core::Rect used = sh.has_used ? sh.used : core::Rect{0, 0, 0, 0};
    x += "<dimension ref=\"" + CellRef(used.r0, used.c0) + (used.IsCell() ? "" : ":" + CellRef(used.r1, used.c1)) + "\"/>";
    x += "<sheetFormatPr defaultRowHeight=\"" + Num(sh.rows->default_size() * 72.0 / 96.0) + "\"";
    if (sh.cols->default_size() != 64) x += " defaultColWidth=\"" + Num((sh.cols->default_size() - 5) / 7.0) + "\"";
    x += "/>";
    // columns
    const auto cols = sh.cols->Entries();
    if (!cols.empty()) {
      x += "<cols>";
      for (const auto& e : cols) {
        if (e.index > 16383) continue;
        const int32_t px = e.size > 0 ? e.size : (e.saved > 0 ? e.saved : sh.cols->default_size());
        x += "<col min=\"" + std::to_string(e.index + 1) + "\" max=\"" + std::to_string(e.index + 1) + "\" width=\"" + Num(std::max(0.0, (px - 5) / 7.0)) +
             "\" customWidth=\"1\"" + (e.size == 0 ? " hidden=\"1\"" : "") + "/>";
      }
      x += "</cols>";
    }
    // rows and cells
    std::map<int64_t, model::DimIndex::Entry> row_sizes;
    for (const auto& e : sh.rows->Entries()) row_sizes[e.index] = e;
    x += "<sheetData>";
    std::string rows;
    const uint32_t last_row = std::max<uint32_t>(sh.has_used ? used.r1 : 0, row_sizes.empty() ? 0 : static_cast<uint32_t>(std::min<int64_t>(row_sizes.rbegin()->first, core::kMaxRows - 1)));
    for (uint32_t r = 0; r <= last_row && (sh.has_used || !row_sizes.empty()); ++r) {
      std::string cells;
      if (sh.has_used && r >= used.r0 && r <= used.r1) {
        for (uint32_t c = used.c0; c <= used.c1; ++c) {
          const core::Value v = sh.GetValue(r, c);
          const uint32_t fid = sh.GetFormula(r, c);
          const uint32_t style_id = sh.GetStyle(r, c);
          if (v.IsEmpty() && fid == 0 && style_id == 0) continue;
          const model::Style& st = engine::StyleOf(wb, sh, r, c);
          const int xf = styles.Index(st);
          std::string cell = "<c r=\"" + CellRef(r, c) + "\"";
          if (xf) cell += " s=\"" + std::to_string(xf) + "\"";
          std::string f;
          uint32_t ar = r, ac = c;
          const model::SpillRange* spill = sh.SpillAt(r, c, &ar, &ac);
          const bool member = spill && (ar != r || ac != c);
          if (fid && !member) {
            std::string text = engine::FormulaTextOf(wb, sh.id, r, c);
            if (!text.empty() && text[0] == '=') text.erase(0, 1);
            if (!text.empty() && engine::DataTableText(sh, r, c).empty()) {
              f = AddPrefixes(text);
              if (spill && spill->cse) {
                const auto& sr = spill->rect;
                f = "<f t=\"array\" ref=\"" + CellRef(sr.r0, sr.c0) + ":" + CellRef(sr.r1, sr.c1) + "\">" + Esc(f) + "</f>";
              } else if (spill) {
                // a dynamic array, as Excel writes it: an array formula over the spilled range, marked dynamic by the
                // cell metadata (cm="1" -> metadata.xml); the members carry only their values
                const auto& sr = spill->rect;
                f = "<f t=\"array\" ref=\"" + CellRef(sr.r0, sr.c0) + ":" + CellRef(sr.r1, sr.c1) + "\">" + Esc(f) + "</f>";
                cell += " cm=\"1\"";
                dynamic_arrays = true;
              } else {
                // every other formula too is a dynamic-array formula of its one cell, as Excel 365 writes those that
                // can take arrays: written plain, Excel reads it the legacy way with implicit intersection and
                // =SUM(C1:C3*C1:C3) silently became =SUM(@C1:C3*@C1:C3) (1 instead of 14)
                f = "<f t=\"array\" ref=\"" + CellRef(r, c) + "\">" + Esc(f) + "</f>";
                cell += " cm=\"1\"";
                dynamic_arrays = true;
              }
            }
          }
          if (v.IsString()) {
            if (!f.empty()) cell += " t=\"str\">" + f + "<v>" + Esc(v.str()) + "</v></c>";
            else if (auto ri = sh.rich->find((uint64_t{r} << 16) | c); ri != sh.rich->end() && ri->second.text == v.str())
              cell += " t=\"s\"><v>" + std::to_string(shared_rich(ri->second, st)) + "</v></c>";
            else cell += " t=\"s\"><v>" + std::to_string(shared(std::string(v.str()))) + "</v></c>";
          } else if (v.IsBool()) {
            cell += " t=\"b\">" + f + "<v>" + (v.boolean() ? "1" : "0") + "</v></c>";
          } else if (v.IsError()) {
            // the file format holds the classic error values only: #SPILL!, #CALC! and the newer ones are written
            // #VALUE! (Excel recalculates on load and shows its own); Excel refused to open the file otherwise
            std::string et = core::ErrorText(v.error());
            static const std::set<std::string> kFileErrors = {"#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A"};
            if (!kFileErrors.count(et)) et = "#VALUE!";
            cell += " t=\"e\">" + f + "<v>" + Esc(et) + "</v></c>";
          } else if (v.IsNumeric()) {
            cell += ">" + f + "<v>" + Num(v.AsDouble()) + "</v></c>";
          } else {
            cell += f.empty() ? "/>" : ">" + f + "</c>";
          }
          cells += cell;
        }
      }
      auto rs = row_sizes.find(r);
      if (cells.empty() && rs == row_sizes.end()) continue;
      std::string row = "<row r=\"" + std::to_string(r + 1) + "\"";
      if (rs != row_sizes.end()) {
        const int32_t px = rs->second.size > 0 ? rs->second.size : (rs->second.saved > 0 ? rs->second.saved : sh.rows->default_size());
        row += " ht=\"" + Num(px * 72.0 / 96.0) + "\" customHeight=\"1\"";
        if (rs->second.size == 0) row += " hidden=\"1\"";
      }
      rows += cells.empty() ? row + "/>" : row + ">" + cells + "</row>";
    }
    x += rows + "</sheetData>";
    if (!sh.merges->empty()) {
      x += "<mergeCells count=\"" + std::to_string(sh.merges->size()) + "\">";
      for (const auto& m : *sh.merges) x += "<mergeCell ref=\"" + CellRef(m.r0, m.c0) + ":" + CellRef(m.r1, m.c1) + "\"/>";
      x += "</mergeCells>";
    }
    x += "</worksheet>";
    files.push_back({"xl/worksheets/sheet" + std::to_string(idx) + ".xml", std::move(x)});
    sheets_xml += "<sheet name=\"" + Esc(sh.name) + "\" sheetId=\"" + std::to_string(idx) + "\"" + (sh.visible ? "" : " state=\"hidden\"") + " r:id=\"rId" +
                  std::to_string(idx) + "\"/>";
    rels_xml += "<Relationship Id=\"rId" + std::to_string(idx) +
                "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" + std::to_string(idx) + ".xml\"/>";
    types_xml += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(idx) + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    // print area
    if (sh.page_setup->has_area) {
      const auto& a = sh.page_setup->area;
      names_xml += "<definedName name=\"_xlnm.Print_Area\" localSheetId=\"" + std::to_string(idx - 1) + "\">" + Esc(QuoteSheet(sh.name)) + "!$" + ColName(a.c0) + "$" +
                   std::to_string(a.r0 + 1) + ":$" + ColName(a.c1) + "$" + std::to_string(a.r1 + 1) + "</definedName>";
    }
  }
  for (const auto& n : *wb.names) {
    std::string local;
    if (n.sheet_scope >= 0) {
      const int32_t si = wb.IndexOfId(static_cast<uint32_t>(n.sheet_scope));
      if (si < 0) continue;
      local = " localSheetId=\"" + std::to_string(si) + "\"";
    }
    std::string refers = n.refers_to;
    if (!refers.empty() && refers[0] == '=') refers.erase(0, 1);
    names_xml += "<definedName name=\"" + Esc(n.name) + "\"" + local + (n.hidden ? " hidden=\"1\"" : "") + ">" + Esc(AddPrefixes(refers)) + "</definedName>";
  }
  const int n = idx;
  // shared strings
  std::string sx = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"" +
                   std::to_string(sst_refs) + "\" uniqueCount=\"" + std::to_string(sst.size()) + "\">";
  for (const auto& si : sst) sx += "<si>" + si + "</si>";
  sx += "</sst>";
  files.push_back({"xl/sharedStrings.xml", std::move(sx)});
  files.push_back({"xl/styles.xml", styles.Xml()});
  std::string wx = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                   "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">" + std::string(wb.date1904 ? "<workbookPr date1904=\"1\"/>" : "") +
                   "<bookViews><workbookView activeTab=\"" +
                   std::to_string(std::clamp<int32_t>(wb.active_sheet, 0, std::max(0, n - 1))) + "\"/></bookViews><sheets>" + sheets_xml + "</sheets>";
  if (!names_xml.empty()) wx += "<definedNames>" + names_xml + "</definedNames>";
  wx += std::string("<calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"") + (wb.precision_as_displayed ? " fullPrecision=\"0\"" : "") +
        (wb.force_full_calc ? " forceFullCalc=\"1\"" : "") + "/></workbook>";
  files.push_back({"xl/workbook.xml", std::move(wx)});
  rels_xml += "<Relationship Id=\"rId" + std::to_string(n + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>";
  rels_xml += "<Relationship Id=\"rId" + std::to_string(n + 2) +
              "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>";
  if (dynamic_arrays) {  // Excel's dynamic-array cell metadata (XLDAPR): without it Excel shows them as {array formulas}
    files.push_back({"xl/metadata.xml",
                     "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<metadata xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                     "xmlns:xda=\"http://schemas.microsoft.com/office/spreadsheetml/2017/dynamicarray\"><metadataTypes count=\"1\"><metadataType name=\"XLDAPR\" "
                     "minSupportedVersion=\"120000\" copy=\"1\" pasteAll=\"1\" pasteValues=\"1\" merge=\"1\" splitFirst=\"1\" rowColShift=\"1\" clearFormats=\"1\" "
                     "clearComments=\"1\" assign=\"1\" coerce=\"1\" cellMeta=\"1\"/></metadataTypes><futureMetadata name=\"XLDAPR\" count=\"1\"><bk><extLst><ext "
                     "uri=\"{bdbb8cdc-fa1e-496e-a857-3c3f30c029c3}\"><xda:dynamicArrayProperties fDynamic=\"1\" fCollapsed=\"0\"/></ext></extLst></bk>"
                     "</futureMetadata><cellMetadata count=\"1\"><bk><rc t=\"1\" v=\"0\"/></bk></cellMetadata></metadata>"});
    rels_xml += "<Relationship Id=\"rId" + std::to_string(n + 3) +
                "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sheetMetadata\" Target=\"metadata.xml\"/>";
  }
  files.push_back({"xl/_rels/workbook.xml.rels", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships "
                                                  "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" +
                                                      rels_xml + "</Relationships>"});
  files.push_back({"_rels/.rels", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                  "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
                                  "</Relationships>"});
  files.push_back({"[Content_Types].xml",
                   "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                   "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                   "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                   "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>" +
                       types_xml +
                       "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
                       "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>" +
                       std::string(dynamic_arrays ? "<Override PartName=\"/xl/metadata.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheetMetadata+xml\"/>" : "") +
                       "</Types>"});
  // [Content_Types].xml first, like Excel
  std::rotate(files.rbegin(), files.rbegin() + 1, files.rend());
  return WriteZip(files);
}

bool ExportXlsx(const model::Workbook& wb, const std::wstring& path, std::string* error) {
  const std::string bytes = ExportXlsxBytes(wb);
  return WriteFileAtomic(path, bytes, error);
}

}  // namespace yc::io
