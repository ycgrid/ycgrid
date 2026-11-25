// SPDX-License-Identifier: Apache-2.0
// fn_text.cpp - text functions (lengths and positions count UTF-16 code units, like Excel)
#include "formula/fn_util.h"

#include "core/utf8.h"
#include "formula/numfmt.h"

#include <algorithm>

namespace yc::formula::fn {
namespace {

// array arguments are applied element by element (single-argument text functions)
EV MapText1(Evaluator& ev, const Node& call, const std::function<Value(const std::string&)>& f) {
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Array) {
    const auto& a = e.a;
    auto o = std::make_shared<Array>(*a);
    for (Value& v : o->v) v = v.IsError() ? v : f(Evaluator::ToText(v));
    return EV::A(o);
  }
  const Value v = ev.ToScalar(e);
  if (v.IsError() || v.IsPending()) return EV::S(v);
  return EV::S(f(Evaluator::ToText(v)));
}

EV FLen(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) { return Value::Number(static_cast<double>(Utf16Length(s))); });
}

// A count left empty is 0, not the 1 of a count that isn't there: LEFT("abc",) and RIGHT("abc",) are "" (Excel 16,
// measured; LEFTB and RIGHTB too)
EV FLeft(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM_OPT_BLANK0(n, 1, 1);
  if (n < 0) return Er(Err::Value);
  return Str(Utf16Substr(s, 0, static_cast<size_t>(n)));
}

EV FRight(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM_OPT_BLANK0(n, 1, 1);
  if (n < 0) return Er(Err::Value);
  const size_t len = Utf16Length(s);
  const size_t k = std::min(len, static_cast<size_t>(n));
  return Str(Utf16Substr(s, len - k, k));
}

EV FMid(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM(start, 1);
  YC_NUM(n, 2);
  // a position or a length past 2147483647 is #VALUE!, as Excel (MID("abc", 2147483648, 1))
  if (start < 1 || n < 0 || start >= 2147483648.0 || n >= 2147483648.0) return Er(Err::Value);
  return Str(Utf16Substr(s, static_cast<size_t>(start) - 1, static_cast<size_t>(n)));
}

// UTF-16 position <-> UTF-8 byte position
size_t ByteToU16(std::string_view s, size_t byte) { return Utf16Length(s.substr(0, byte)); }
size_t U16ToByte(std::string_view s, size_t u16) { return Utf16Substr(s, 0, u16).size(); }

// UTF-8 byte offset of the first match at or after byte sb (npos = none); SEARCH is case-insensitive with wildcards
size_t FindAt(const std::string& hay, const std::string& needle, size_t sb, bool case_insensitive) {
  if (case_insensitive) {
    const std::string h = Lower(hay), n = Lower(needle);
    if (n.find_first_of("*?~") != std::string::npos) {
      // SEARCH wildcards: the needle matches a prefix of the text at some start. A "*" appended makes WildcardMatch
      // that prefix match: one call per start instead of one per (start, end) pair (minutes on a 30k-character cell)
      std::string prefix = n;
      size_t tildes = 0;
      for (auto it = prefix.rbegin(); it != prefix.rend() && *it == '~'; ++it) ++tildes;
      if (tildes % 2 == 1) prefix.pop_back();  // an unescaped trailing ~ is dropped, as Excel does
      prefix += '*';
      for (size_t i = sb; i <= h.size(); ++i) {
        if (i < h.size() && (static_cast<unsigned char>(h[i]) & 0xC0) == 0x80) continue;  // inside a character
        if (WildcardMatch(prefix, std::string_view(h).substr(i))) return i;
      }
      return std::string::npos;
    }
    return h.find(n, sb);
  }
  return hay.find(needle, sb);
}

EV FindImpl(Evaluator& ev, const Node& call, bool case_insensitive) {
  YC_TEXT(needle, 0);
  YC_TEXT(hay, 1);
  // a start left empty is 0, so #VALUE!: FIND("b","abc",) and SEARCH("b","abc",) (Excel 16, measured; FINDB and
  // SEARCHB too)
  YC_NUM_OPT_BLANK0(startf, 2, 1);
  if (startf < 1) return Er(Err::Value);
  const size_t start16 = static_cast<size_t>(startf) - 1;
  // FIND takes a start just past the end (an empty needle is found there); SEARCH doesn't (Excel)
  if (start16 > Utf16Length(hay) || (case_insensitive && start16 >= Utf16Length(hay))) return Er(Err::Value);
  const size_t p = FindAt(hay, needle, U16ToByte(hay, start16), case_insensitive);
  if (p == std::string::npos) return Er(Err::Value);
  return Num(static_cast<double>(ByteToU16(hay, p) + 1));
}

// ---- *B functions: positions and lengths in DBCS bytes (CP950 rules: ASCII = 1 byte, other characters = 2 bytes) ----

struct DbcsChar {
  size_t pos, len;  // UTF-8 byte range of the character
  int bytes;        // 1 or 2
};

std::vector<DbcsChar> DbcsChars(const std::string& s) {
  std::vector<DbcsChar> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    out.push_back({i, std::min(n, s.size() - i), c < 0x80 ? 1 : 2});
    i += n;
  }
  return out;
}

size_t DbcsLength(const std::string& s) {
  size_t n = 0;
  for (const auto& c : DbcsChars(s)) n += static_cast<size_t>(c.bytes);
  return n;
}

// the characters covering bytes [b0, b1) (0-based); half of a double-byte character becomes a space, like Excel
std::string DbcsSlice(const std::string& s, size_t b0, size_t b1) {
  std::string out;
  size_t at = 0;
  for (const auto& c : DbcsChars(s)) {
    const size_t end = at + static_cast<size_t>(c.bytes);
    if (end > b0 && at < b1) {
      if (at >= b0 && end <= b1) out.append(s, c.pos, c.len);
      else out += ' ';
    }
    at = end;
    if (at >= b1) break;
  }
  return out;
}

// UTF-8 offset of the character that contains DBCS byte b (0-based)
size_t DbcsToUtf8(const std::string& s, size_t b) {
  size_t at = 0;
  for (const auto& c : DbcsChars(s)) {
    if (at + static_cast<size_t>(c.bytes) > b) return c.pos;
    at += static_cast<size_t>(c.bytes);
  }
  return s.size();
}

EV FLenB(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) { return Value::Number(static_cast<double>(DbcsLength(s))); });
}
EV FLeftB(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM_OPT_BLANK0(n, 1, 1);
  if (n < 0) return Er(Err::Value);
  return Str(DbcsSlice(s, 0, static_cast<size_t>(n)));
}
EV FRightB(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM_OPT_BLANK0(n, 1, 1);
  if (n < 0) return Er(Err::Value);
  const size_t len = DbcsLength(s), k = std::min(len, static_cast<size_t>(n));
  return Str(DbcsSlice(s, len - k, len));
}
EV FMidB(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM(start, 1);
  YC_NUM(n, 2);
  if (start < 1 || n < 0) return Er(Err::Value);
  const size_t b0 = static_cast<size_t>(start) - 1;
  return Str(DbcsSlice(s, b0, b0 + static_cast<size_t>(n)));
}
EV FindBImpl(Evaluator& ev, const Node& call, bool case_insensitive) {
  YC_TEXT(needle, 0);
  YC_TEXT(hay, 1);
  YC_NUM_OPT_BLANK0(startf, 2, 1);
  if (startf < 1) return Er(Err::Value);
  const size_t startb = static_cast<size_t>(startf) - 1;
  if (startb > DbcsLength(hay)) return Er(Err::Value);
  const size_t p = FindAt(hay, needle, DbcsToUtf8(hay, startb), case_insensitive);
  if (p == std::string::npos) return Er(Err::Value);
  return Num(static_cast<double>(DbcsLength(hay.substr(0, p)) + 1));
}
EV FFindB(Evaluator& ev, const Node& call) { return FindBImpl(ev, call, false); }
EV FSearchB(Evaluator& ev, const Node& call) { return FindBImpl(ev, call, true); }
EV FReplaceB(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM(start, 1);
  YC_NUM(n, 2);
  YC_TEXT(rep, 3);
  // a start or a length past 32767 is #VALUE! here (REPLACE("12", 1, 32768, "x"): Excel 16, measured)
  if (start < 1 || n < 0 || start >= 32768 || n >= 32768) return Er(Err::Value);
  const size_t len = DbcsLength(s);
  const size_t a = std::min(len, static_cast<size_t>(start) - 1);
  const size_t b = std::min(len, a + static_cast<size_t>(n));
  return Str(DbcsSlice(s, 0, a) + rep + DbcsSlice(s, b, len));
}

// ASC: full-width ASCII forms (U+FF01..U+FF5E) and the ideographic space become half-width; DBCS: the reverse
std::string AppendUtf8(std::string out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}
uint32_t DecodeUtf8(const std::string& s, const DbcsChar& c) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data() + c.pos);
  if (c.len == 1) return p[0];
  if (c.len == 2) return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
  if (c.len == 3) return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
  return 0xFFFD;
}
std::string WidthConvert(const std::string& s, bool to_half) {
  std::string out;
  for (const auto& c : DbcsChars(s)) {
    const uint32_t cp = c.len <= 3 ? DecodeUtf8(s, c) : 0xFFFD;
    if (to_half && cp >= 0xFF01 && cp <= 0xFF5E) out = AppendUtf8(std::move(out), cp - 0xFEE0);
    else if (to_half && cp == 0x3000) out += ' ';
    else if (!to_half && cp >= 0x21 && cp <= 0x7E) out = AppendUtf8(std::move(out), cp + 0xFEE0);
    else if (!to_half && cp == 0x20) out = AppendUtf8(std::move(out), 0x3000);
    else out.append(s, c.pos, c.len);
  }
  return out;
}
EV FAsc(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) { return Value::String(WidthConvert(s, true)); });
}
EV FDbcs(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) { return Value::String(WidthConvert(s, false)); });
}
EV FFind(Evaluator& ev, const Node& call) { return FindImpl(ev, call, false); }
EV FSearch(Evaluator& ev, const Node& call) { return FindImpl(ev, call, true); }

// a text result past 32,767 characters is #VALUE! (Excel's SUBSTITUTE and REPLACE)
EV Long(std::string s) {
  if (s.size() > 32767 && Utf16Length(s) > 32767) return Er(Err::Value);
  return Str(std::move(s));
}

EV FSubstitute(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_TEXT(from, 1);
  YC_TEXT(to, 2);
  double inst = 0;
  // an instance left empty is 0, so #VALUE!, where one that isn't there means every occurrence
  // (SUBSTITUTE("aaa","a","b",): Excel 16, measured)
  const bool has_inst = Argc(call) > 3;
  if (has_inst) {
    // TRUE / FALSE as the instance, given or in a cell, is #VALUE! (Excel 16, measured; "1" is 1)
    const Value v = ev.Scalar(call.kids[3]);
    Err e = v.IsBool() ? Err::Value : Err::None;
    if (e != Err::None || !Evaluator::ToNumber(v, &inst, &e)) return Er(e);
  }
  if (has_inst && inst < 1) return Er(Err::Value);
  if (from.empty()) return Str(s);
  if (has_inst) {
    // one instance: overlapping occurrences count, as in Excel (SUBSTITUTE("aaaaa","aa","X",2) is aXaa)
    if (inst > static_cast<double>(s.size())) return Str(s);
    const size_t want = static_cast<size_t>(inst);
    size_t pos = 0, k = 0;
    while (true) {
      const size_t p = s.find(from, pos);
      if (p == std::string::npos) return Str(s);
      if (++k == want) return Long(s.substr(0, p) + to + s.substr(p + from.size()));
      pos = p + 1;
      while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) ++pos;
    }
  }
  std::string out;
  size_t pos = 0, k = 0;
  const size_t want = static_cast<size_t>(inst);
  while (true) {
    const size_t p = s.find(from, pos);
    if (p == std::string::npos) break;
    ++k;
    if (want == 0 || k == want) {
      out.append(s, pos, p - pos);
      out += to;
    } else {
      out.append(s, pos, p - pos + from.size());
    }
    pos = p + from.size();
    if (want != 0 && k == want) break;
  }
  out.append(s, pos, std::string::npos);
  return Long(std::move(out));
}

EV FReplace(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM(start, 1);
  YC_NUM(n, 2);
  YC_TEXT(rep, 3);
  // a start or a length past 32767 is #VALUE! here (REPLACE("12", 1, 32768, "x"): Excel 16, measured)
  if (start < 1 || n < 0 || start >= 32768 || n >= 32768) return Er(Err::Value);
  const size_t len = Utf16Length(s);
  const size_t a = std::min(len, static_cast<size_t>(start) - 1);
  const size_t b = std::min(len, a + static_cast<size_t>(n));
  return Long(Utf16Substr(s, 0, a) + rep + Utf16Substr(s, b, len - b));
}

EV FUpper(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) { return Value::String(Upper(s)); });
}
EV FLower(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) { return Value::String(Lower(s)); });
}
EV FProper(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) { return Value::String(ProperCase(s)); });
}

EV FTrim(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) {
    std::string o;
    bool space = false;
    for (char c : s) {
      if (c == ' ') {
        space = true;
        continue;
      }
      if (space && !o.empty()) o += ' ';
      space = false;
      o += c;
    }
    return Value::String(o);
  });
}

EV FClean(Evaluator& ev, const Node& call) {
  return MapText1(ev, call, [](const std::string& s) {
    std::string o;
    for (char c : s) {
      if (static_cast<unsigned char>(c) >= 32) o += c;
    }
    return Value::String(o);
  });
}

EV FText(Evaluator& ev, const Node& call) {
  const Value v = ev.Scalar(call.kids[0]);
  if (v.IsError()) return EV::S(v);
  YC_TEXT(fmt, 1);
  Value x = v;
  // a blank is the number 0: TEXT(A8,"0.0") of an empty A8 is 0.0, TEXT(,"@") 0 and TEXT(A8,"yyyy") 1900 (Excel 16,
  // measured), while an empty text stays one (TEXT("","0.0") is "")
  if (v.IsEmpty()) x = Value::Number(0);
  if (v.IsString()) {
    const InputParse p = ParseInput(v.str());
    if (p.value.IsNumeric() && !p.typed_only && !Evaluator::LeadingSpaceDate(v.str(), p.format)) x = p.value;  // TEXT(" 12:00","0.00") keeps the text
  }
  // an empty format shows nothing of a number but a negative's sign: TEXT(1234.567,"") is "" and TEXT(-5,"") "-";
  // text and logicals show as they are (Excel 16, measured)
  if (fmt.empty() && x.IsNumeric()) return Str(x.AsDouble() < 0 ? "-" : "");
  FormatResult fr = FormatValue(x, fmt);
  if (fr.bad_date || fr.bad_code) return Er(Err::Value);  // TEXT(-1,"yyyy") and TEXT(-1,"0;@;0") are #VALUE! in Excel
  return Str(std::move(fr.text));
}

EV FValue(Evaluator& ev, const Node& call) {
  const Value v = ev.Scalar(call.kids[0]);
  if (v.IsError()) return EV::S(v);
  if (v.IsNumeric()) return EV::S(v);
  if (v.IsEmpty()) return Num(0);
  if (v.IsBool()) return Er(Err::Value);
  std::string s(v.str());
  // trim both ends
  while (!s.empty() && s.back() == ' ') s.pop_back();
  size_t k = 0;
  while (k < s.size() && s[k] == ' ') ++k;
  const InputParse p = ParseInput(std::string_view(s).substr(k));
  if (p.value.IsNumeric() && !p.typed_only) return EV::S(p.value);
  return Er(Err::Value);
}

// NUMBERVALUE(text, [decimal_separator], [group_separator]), as Excel 16 (measured, 130 cases):
// - each separator is the first character of its text (a character, not a byte: a no-break space, an ideographic
//   full stop), and one given as nothing (an argument left empty, "", an empty cell) is #VALUE!;
// - the decimal separator given alone as "," (the group separator otherwise) makes "." the group separator, so
//   NUMBERVALUE("1.234,5",",") is 1234.5 and NUMBERVALUE("1.5",",") 15;
// - one character can't be both separators (NUMBERVALUE("1,5",",",",") is #VALUE!);
// - spaces, tabs and line breaks anywhere are ignored, before the separators are looked for (NUMBERVALUE("1 5"," ")
//   is 15); a group separator after the decimal one is an error; every trailing % divides by 100
//   (NUMBERVALUE("9%%") is 0.0009, as =9%%); what is left is read as typed input.
EV FNumberValue(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  std::string dec = ".", grp = ",";
  if (Argc(call) > 1) {
    YC_TEXT(d, 1);
    if (d.empty()) return Er(Err::Value);
    dec = std::string(core::Utf8First(d, 1));
    if (Argc(call) < 3 && dec == ",") grp = ".";
  }
  if (Argc(call) > 2) {
    YC_TEXT(g, 2);
    if (g.empty()) return Er(Err::Value);
    grp = std::string(core::Utf8First(g, 1));
  }
  if (dec == grp) return Er(Err::Value);
  std::string t;
  bool seen_dec = false;
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++i;
      continue;
    }
    if (s.compare(i, dec.size(), dec) == 0) {
      if (seen_dec) return Er(Err::Value);
      seen_dec = true;
      t += '.';
      i += dec.size();
      continue;
    }
    if (s.compare(i, grp.size(), grp) == 0) {
      if (seen_dec) return Er(Err::Value);
      i += grp.size();
      continue;
    }
    t += c;
    ++i;
  }
  int percents = 0;
  while (!t.empty() && t.back() == '%') {
    t.pop_back();
    ++percents;
  }
  if (t.empty()) return Num(0);  // nothing but spaces or % signs: 0 (Excel: NUMBERVALUE("%") is 0)
  const InputParse p = ParseInput(t);
  if (!p.value.IsNumeric() || p.typed_only) return Er(Err::Value);
  if (!percents) return EV::S(p.value);
  return Num(p.value.AsDouble() / std::pow(100.0, percents));
}

EV FConcat(Evaluator& ev, const Node& call) {
  std::string out;
  for (const Node& k : call.kids) {
    if (k.kind == NK::Missing) continue;
    const EV e = ev.Eval(k);
    if (e.k == EV::K::Scalar) {
      if (e.s.IsError()) return e;
      out += Evaluator::ToText(e.s);
      continue;
    }
    bool failed = false;
    Value err;
    ev.ForEachValue(e, [&](const Value& v) {
      if (v.IsError()) {
        err = v;
        failed = true;
        return false;
      }
      out += Evaluator::ToText(v);
      return true;
    });
    if (failed) return EV::S(err);
    if (out.size() > 32767 && Utf16Length(out) > 32767) return Er(Err::Calc);
  }
  // more than 32,767 characters: #CALC! (Excel's CONCAT and TEXTJOIN)
  if (out.size() > 32767 && Utf16Length(out) > 32767) return Er(Err::Calc);
  return Str(out);
}

// CONCATENATE doesn't accept ranges (implicit intersection)
EV FConcatenate(Evaluator& ev, const Node& call) {
  std::string out;
  for (const Node& k : call.kids) {
    const Value v = ev.Scalar(k);
    if (v.IsError()) return EV::S(v);
    out += Evaluator::ToText(v);
  }
  return Str(out);
}

EV FTextJoin(Evaluator& ev, const Node& call) {
  // the delimiter may be a range or an array: its texts are used in turn (Excel: TEXTJOIN({"-","+"},,1,2,3) is 1-2+3)
  std::vector<std::string> delims;
  {
    const EV d = ev.Eval(call.kids[0]);
    if (d.k == EV::K::Scalar) {
      if (d.s.IsError()) return d;
      delims.push_back(Evaluator::ToText(d.s));
    } else {
      Value derr;
      bool dfail = false;
      ev.ForEachValue(d, [&](const Value& v) {
        if (v.IsError()) {
          derr = v;
          dfail = true;
          return false;
        }
        delims.push_back(Evaluator::ToText(v));
        return true;
      }, true);
      if (dfail) return EV::S(derr);
      if (delims.empty()) delims.emplace_back();
    }
  }
  YC_BOOL_OPT(ignore_empty, 1, true);
  std::string out;
  bool first = true;
  bool failed = false;
  size_t joins = 0;
  Value err;
  auto add = [&](const Value& v) {
    if (v.IsError()) {
      err = v;
      failed = true;
      return false;
    }
    const std::string s = Evaluator::ToText(v);
    if (ignore_empty && s.empty()) return true;
    if (!first) out += delims[joins++ % delims.size()];
    first = false;
    out += s;
    return true;
  };
  for (size_t i = 2; i < call.kids.size(); ++i) {
    const Node& k = call.kids[i];
    if (k.kind == NK::Missing) {
      add(Value());
      continue;
    }
    const EV e = ev.Eval(k);
    if (e.k == EV::K::Scalar) add(e.s);
    else ev.ForEachValue(e, add, !ignore_empty);
    if (failed) return EV::S(err);
  }
  if (out.size() > 32767 && Utf16Length(out) > 32767) return Er(Err::Calc);  // as CONCAT
  return Str(out);
}

EV FRept(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  YC_NUM(n, 1);
  if (n < 0) return Er(Err::Value);
  if (s.empty() || n < 1) return Str(std::string());        // nothing to repeat: an empty text whatever the count
  if (n > 32767) return Er(Err::Value);                     // checked before the cast: 1E15 must not loop (nor overflow size_t)
  const size_t k = static_cast<size_t>(n);
  if (Utf16Length(s) * k > 32767) return Er(Err::Value);      // 32,767 characters at most (Excel: REPT("a",32768) is #VALUE!)
  std::string o;
  o.reserve(s.size() * k);
  for (size_t i = 0; i < k; ++i) o += s;
  return Str(o);
}

EV FExact(Evaluator& ev, const Node& call) {
  YC_TEXT(a, 0);
  YC_TEXT(b, 1);
  return Bool(a == b);
}

void EncodeUtf8(uint32_t cp, std::string& o) {
  if (cp < 0x80) o += static_cast<char>(cp);
  else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
  else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
}
uint32_t FirstCodePoint(std::string_view s) {
  if (s.empty()) return 0;
  const unsigned char c = static_cast<unsigned char>(s[0]);
  uint32_t cp;
  size_t n;
  if (c < 0x80) { cp = c; n = 1; }
  else if (c < 0xE0) { cp = c & 0x1F; n = 2; }
  else if (c < 0xF0) { cp = c & 0x0F; n = 3; }
  else { cp = c & 0x07; n = 4; }
  for (size_t k = 1; k < n && k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[k]) & 0x3F);
  return cp;
}

// CHAR / CODE: Windows-1252, as Excel on Windows: 0xA0-0xFF are Latin-1, 0x80-0x9F the 1252 letters (128 = euro)
// (the five codes 1252 leaves unassigned keep their own value)
static const uint16_t kCp1252High[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                         0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                         0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                         0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
EV FChar(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  if (n < 1 || n >= 256) return Er(Err::Value);
  const int code = static_cast<int>(n);
  std::string o;
  EncodeUtf8(code >= 0x80 && code < 0xA0 ? kCp1252High[code - 0x80] : static_cast<uint32_t>(code), o);
  return Str(o);
}
EV FCode(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  if (s.empty()) return Er(Err::Value);
  const uint32_t cp = FirstCodePoint(s);
  if (cp < 0x80 || (cp >= 0xA0 && cp < 0x100)) return Num(cp);
  for (int k = 0; k < 32; ++k) {
    if (kCp1252High[k] == cp) return Num(0x80 + k);
  }
  return Num(63);  // characters outside 1252 -> '?'
}
EV FUnichar(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  if (n < 1 || n > 0x10FFFF) return Er(Err::Value);
  // a surrogate half or a noncharacter (U+FFFE, U+10FFFF...) is #N/A (Excel)
  const uint32_t cp = static_cast<uint32_t>(n);
  if ((cp >= 0xD800 && cp <= 0xDFFF) || (cp & 0xFFFE) == 0xFFFE) return Er(Err::NA);
  std::string o;
  EncodeUtf8(static_cast<uint32_t>(n), o);
  return Str(o);
}
EV FUnicode(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  if (s.empty()) return Er(Err::Value);
  return Num(FirstCodePoint(s));
}

// N and T of a reference read its first cell (Excel: =T(B1:B3) is B1's text, not three values); an array is mapped
EV MapFirstOfRange(Evaluator& ev, const Node& call, Value (*f)(const Value&)) {
  const EV e = ev.Eval(call.kids[0]);
  if (e.k == EV::K::Scalar) return EV::S(f(e.s));
  std::shared_ptr<Array> a = ev.ToArray(e);
  if (!a || a->v.empty()) return EV::S(f(Value()));
  if (e.k == EV::K::Range) return EV::S(f(a->v[0]));
  auto o = std::make_shared<Array>(*a);
  for (Value& v : o->v) v = f(v);
  return EV::A(o);
}
Value NOf(const Value& v) {
  if (v.IsError() || v.IsNumeric()) return v;
  if (v.IsBool()) return Value::Number(v.boolean() ? 1 : 0);
  return Value::Number(0);
}
Value TOf(const Value& v) {
  if (v.IsError() || v.IsString()) return v;
  return Value::String("");
}
EV FN(Evaluator& ev, const Node& call) { return MapFirstOfRange(ev, call, NOf); }
EV FT(Evaluator& ev, const Node& call) { return MapFirstOfRange(ev, call, TOf); }

EV FFixed(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT(dec, 1, 2);
  YC_BOOL_OPT(no_commas, 2, false);
  const int d = static_cast<int>(dec);
  if (d > 127) return Er(Err::Value);
  std::string code = no_commas ? "0" : "#,##0";
  double v = x;
  if (d > 0) {
    code += ".";
    code.append(static_cast<size_t>(d), '0');
  } else if (d < 0) {
    const double p = std::pow(10.0, -d);
    v = std::round(x / p) * p;
  }
  // one section: a negative that rounds to zero shows no sign (FIXED(-0.004,2) is 0.00, as in Excel)
  return Str(FormatValue(Value::Number(v), code).text);
}

EV FDollar(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  // decimals left empty are 0 here (DOLLAR(1.5,) is $2), while FIXED keeps its 2 (Excel 16, measured)
  YC_NUM_OPT_BLANK0(dec, 1, 2);
  const int d = static_cast<int>(dec);
  if (d > 127) return Er(Err::Value);
  std::string code = "$#,##0";
  double v = x;
  if (d > 0) {
    code += ".";
    code.append(static_cast<size_t>(d), '0');
  } else if (d < 0) {
    const double p = std::pow(10.0, -d);
    v = std::round(x / p) * p;
  }
  return Str(FormatValue(Value::Number(v), code + ";(" + code + ")").text);
}

// TEXTBEFORE / TEXTAFTER(text, delimiter, [instance_num], [match_mode], [match_end], [if_not_found]): the delimiter
// may be a list of texts (any of them); match_end counts the end of the text (the start, counting from the end) as one
EV TextSplitImpl(Evaluator& ev, const Node& call, bool before) {
  YC_TEXT(s, 0);
  std::vector<std::string> delims;
  {
    std::shared_ptr<Array> a = ev.ToArray(ev.Eval(call.kids[1]));
    if (!a || a->v.empty()) return Er(Err::Value);
    for (const Value& v : a->v) {
      if (v.IsError()) return EV::S(v);
      delims.push_back(Evaluator::ToText(v));
    }
  }
  YC_NUM_OPT(instf, 2, 1);
  YC_NUM_OPT(mode, 3, 0);
  YC_NUM_OPT(endf, 4, 0);
  // the instance is taken down to a whole number (-0.5 is -1, 0.5 is 0: #VALUE!), and one given past the text's
  // length (UTF-16 units) is #VALUE! unless a delimiter is empty (Excel 16, measured: TEXTBEFORE("","-",1) is #VALUE!
  // but TEXTBEFORE("","-") #N/A, TEXTBEFORE("abc","",5) is "")
  const double ti = std::floor(instf);
  const bool any_empty = std::any_of(delims.begin(), delims.end(), [](const std::string& d) { return d.empty(); });
  if (ti == 0 || (Has(call, 2) && !any_empty && std::fabs(ti) > static_cast<double>(Utf16Length(s))))
    return Er(Err::Value);
  // match_mode and match_end are 0 or 1 (Excel: anything else is #VALUE!)
  if ((std::trunc(mode) != 0 && std::trunc(mode) != 1) || (std::trunc(endf) != 0 && std::trunc(endf) != 1)) return Er(Err::Value);
  const int64_t inst = static_cast<int64_t>(ti);
  const bool ci = std::trunc(mode) == 1;
  const std::string hay = ci ? Lower(s) : s;
  for (std::string& d : delims) {
    if (ci) d = Lower(d);
  }
  // every place a delimiter starts (left to right, not overlapping: the longest delimiter wins at one place)
  struct Hit {
    size_t at, len;
  };
  std::vector<Hit> hits;
  const bool empty_delim = std::any_of(delims.begin(), delims.end(), [](const std::string& d) { return d.empty(); });
  if (empty_delim) {
    hits.push_back({inst > 0 ? 0 : hay.size(), 0});  // an empty delimiter matches at once, from either end
  } else {
    for (size_t i = 0; i < hay.size();) {
      size_t best = 0;
      for (const std::string& d : delims) {
        if (d.size() > best && hay.compare(i, d.size(), d) == 0) best = d.size();
      }
      if (best) {
        hits.push_back({i, best});
        i += best;
      } else {
        ++i;
      }
    }
  }
  if (std::trunc(endf) == 1 && !empty_delim) {
    if (inst > 0) hits.push_back({hay.size(), 0});
    else hits.insert(hits.begin(), Hit{0, 0});
  }
  // an empty text has no delimiter to find (Excel: TEXTBEFORE("","-",,,1) is #N/A), and an empty delimiter is found at
  // once whatever the instance (TEXTAFTER("abc","",2) is abc)
  if (s.empty() && !empty_delim) return Has(call, 5) ? ev.Eval(call.kids[5]) : Er(Err::NA);
  const int64_t n = static_cast<int64_t>(hits.size());
  const int64_t k = empty_delim ? 0 : inst > 0 ? inst - 1 : n + inst;
  if (k < 0 || k >= n) return Has(call, 5) ? ev.Eval(call.kids[5]) : Er(Err::NA);
  const Hit& h = hits[static_cast<size_t>(k)];
  return before ? Str(s.substr(0, h.at)) : Str(s.substr(h.at + h.len));
}
EV FTextBefore(Evaluator& ev, const Node& call) { return TextSplitImpl(ev, call, true); }
EV FTextAfter(Evaluator& ev, const Node& call) { return TextSplitImpl(ev, call, false); }

}  // namespace

void RegisterText(std::vector<FuncInfo>& t) {
  t.push_back({"LEN", 1, 1, FLen});
  t.push_back({"LEFT", 1, 2, FLeft});
  t.push_back({"RIGHT", 1, 2, FRight});
  t.push_back({"MID", 3, 3, FMid});
  t.push_back({"FIND", 2, 3, FFind});
  t.push_back({"SEARCH", 2, 3, FSearch});
  t.push_back({"SUBSTITUTE", 3, 4, FSubstitute});
  t.push_back({"REPLACE", 4, 4, FReplace});
  t.push_back({"UPPER", 1, 1, FUpper});
  t.push_back({"LOWER", 1, 1, FLower});
  t.push_back({"PROPER", 1, 1, FProper});
  t.push_back({"TRIM", 1, 1, FTrim});
  t.push_back({"CLEAN", 1, 1, FClean});
  t.push_back({"TEXT", 2, 2, FText});
  t.push_back({"VALUE", 1, 1, FValue});
  t.push_back({"NUMBERVALUE", 1, 3, FNumberValue});
  t.push_back({"CONCAT", 1, 255, FConcat});
  t.push_back({"CONCATENATE", 1, 255, FConcatenate});
  t.push_back({"TEXTJOIN", 3, 255, FTextJoin});
  t.push_back({"REPT", 2, 2, FRept});
  t.push_back({"EXACT", 2, 2, FExact});
  t.push_back({"CHAR", 1, 1, FChar});
  t.push_back({"CODE", 1, 1, FCode});
  t.push_back({"UNICHAR", 1, 1, FUnichar});
  t.push_back({"UNICODE", 1, 1, FUnicode});
  t.push_back({"N", 1, 1, FN});
  t.push_back({"T", 1, 1, FT});
  t.push_back({"FIXED", 1, 3, FFixed});
  t.push_back({"DOLLAR", 1, 2, FDollar});
  t.push_back({"TEXTBEFORE", 2, 6, FTextBefore});
  t.push_back({"TEXTAFTER", 2, 6, FTextAfter});
  t.push_back({"LENB", 1, 1, FLenB});
  t.push_back({"LEFTB", 1, 2, FLeftB});
  t.push_back({"RIGHTB", 1, 2, FRightB});
  t.push_back({"MIDB", 3, 3, FMidB});
  t.push_back({"FINDB", 2, 3, FFindB});
  t.push_back({"SEARCHB", 2, 3, FSearchB});
  t.push_back({"REPLACEB", 4, 4, FReplaceB});
  t.push_back({"ASC", 1, 1, FAsc});
  t.push_back({"DBCS", 1, 1, FDbcs});
}

}  // namespace yc::formula::fn
